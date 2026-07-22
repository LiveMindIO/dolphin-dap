// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapRealtimeWatch.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "Core/Core.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PPCCache.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "VideoCommon/VideoEvents.h"

namespace DAP
{
namespace
{
// DESNOTE(jbarber, 2026-07-22): Mirrors DapDebugController::InvalidateCodeRange.
// Freeze back-writes go through AddressSpace::WriteU8 -> MMU::HostWrite, which
// does NOT fire icbi / erase JIT blocks. If the frozen region spans code (a
// client pinning a code patch in place to defeat self-modifying code, or
// freezing a region that just happens to be executable), the L1 iCache and JIT
// block cache would keep serving stale instructions. Re-running the icbi loop
// on every back-write keeps the freeze honest at field rate. Bugbot #61.
// The body is a 4-line cacheline walk (PPC L1 lines are 32 bytes); duplicating
// it here keeps RealtimeWatch decoupled from DapDebugController.
void InvalidateCodeRange(Core::System& system, u32 address, u32 length)
{
  if (length == 0)
    return;
  if (length - 1u > std::numeric_limits<u32>::max() - address)
    return;
  auto& ppc_state = system.GetPPCState();
  auto& memory = system.GetMemory();
  auto& jit_interface = system.GetJitInterface();
  const u32 end_addr = address + length;
  const u32 start_line = address & ~u32{31u};
  const u32 end_line = (end_addr + 31u) & ~u32{31u};
  for (u32 line = start_line; line < end_line; line += 32)
    ppc_state.iCache.Invalidate(memory, jit_interface, line);
}
}  // namespace

RealtimeWatchSampler::RealtimeWatchSampler(Core::System& system, DispatchCallback dispatch)
    : m_system(system), m_dispatch(std::move(dispatch))
{
  // DESNOTE(jbarber, 2026-07-20): vi_end_field_event fires on the CPU thread at
  // the end of every emulated field (~60 Hz NTSC / ~50 Hz PAL), the same hook
  // the Qt MemoryViewWidget and CheatsManager use for frame-accurate memory
  // sampling. Tick() therefore runs on the CPU thread, where CPUThreadGuard is
  // a no-op pause -- a watch never stalls emulation.
  m_frame_hook = m_system.GetVideoEvents().vi_end_field_event.Register([this] { Tick(); });
}

RealtimeWatchSampler::~RealtimeWatchSampler() = default;

int RealtimeWatchSampler::AddSubscription(u32 address, u32 count)
{
  // DESNOTE(jbarber, 2026-07-21): Reject ranges that wrap past u32 max so
  // later `address + i` arithmetic in Tick() can't silently read from low
  // memory. A subscription of count == 0 is meaningless and would make the
  // change detector noisy without producing useful bytes. We also cap
  // `count` at the system's physical RAM size: the sampler reads via the
  // Effective address space, which can never exceed RAM, and an
  // unbounded count (e.g. UINT32_MAX) would allocate multi-gigabyte
  // buffers and a seed loop that reads past the end of RAM. The previous
  // form `address > UINT32_MAX - count` was correct for the wrap itself
  // but still let a UINT32_MAX count through when address == 0, allocating
  // 4 GB of zero-fill for `current`/`last_seen`. Off-by-one fix below: last
  // byte is address + count - 1; reject only when (count - 1) > (UINT32_MAX
  // - address), so a 1-byte watch at 0xFFFFFFFF is accepted.
  if (count == 0)
    return kInvalidWatchId;
  if (count - 1u > std::numeric_limits<u32>::max() - address)
    return kInvalidWatchId;
  const u32 ram_size = m_system.GetMemory().GetRamSizeReal();
  if (ram_size == 0 || count > ram_size)
    return kInvalidWatchId;

  // Seed the last-seen snapshot with the current region contents so the first
  // Tick() after subscribe doesn't echo the initial value back to the client
  // as a spurious "change".
  std::vector<u8> seed(count, 0);
  u32 readable = 0;
  {
    Core::CPUThreadGuard guard(m_system);
    AddressSpace::Accessors* accessors =
        AddressSpace::GetAccessors(AddressSpace::Type::Effective);
    for (; readable < count; ++readable)
    {
      const u32 addr = address + readable;
      if (!accessors->IsValidAddress(guard, addr))
        break;
      seed[readable] = accessors->ReadU8(guard, addr);
    }
  }

  std::lock_guard lock(m_mutex);
  Subscription sub;
  sub.watch_id = m_next_id++;
  sub.address = address;
  sub.count = count;
  sub.last_seen = std::move(seed);
  // DESNOTE(jbarber, 2026-07-21): `current` is sized once here and read into
  // in place every Tick() -- the previous form's `assign(count, 0)` was a
  // reallocation-then-zero-fill on every field, which is wasted work on the
  // CPU thread.
  sub.current.assign(count, 0);
  sub.readable = readable;
  // DESNOTE(jbarber, 2026-07-21): Initialize prev_readable to the seeded
  // readable count so the first Tick()'s compare window includes the full
  // seed prefix. Without this, prev_readable defaults to 0 and the first
  // Tick would see `grew = (readable > 0)` and dispatch unconditionally
  // -- a spurious change event whose payload matches the seed. The seed
  // already captured the current value; the first tick should treat it as
  // the baseline.
  sub.prev_readable = readable;
  m_subscriptions.push_back(std::move(sub));
  return m_subscriptions.back().watch_id;
}

bool RealtimeWatchSampler::RemoveSubscription(int watch_id)
{
  std::lock_guard lock(m_mutex);
  const auto it = std::find_if(m_subscriptions.begin(), m_subscriptions.end(),
                               [watch_id](const Subscription& sub) {
                                 return sub.watch_id == watch_id;
                               });
  if (it == m_subscriptions.end())
    return false;
  m_subscriptions.erase(it);
  return true;
}

bool RealtimeWatchSampler::Freeze(int watch_id, std::vector<u8> value)
{
  // DESNOTE(jbarber, 2026-07-22): Acquire CPUThreadGuard BEFORE m_mutex.
  // Tick() runs on the CPU thread (via vi_end_field_event), takes m_mutex,
  // then constructs its own CPUThreadGuard — which is a no-op there because
  // the calling thread *is* the CPU thread. If Freeze took m_mutex first
  // and constructed CPUThreadGuard second, a Tick fired on the CPU thread
  // between those two lines would block on m_mutex; the session thread's
  // CPUThreadGuard would then wait for the CPU thread to idle (via
  // PauseAndLock), which can't happen because the CPU thread is stuck in
  // Tick holding-waiting-for-m_mutex. Acquiring the CPU guard first pauses
  // the CPU (Tick can't start or completes its current run before idle),
  // then m_mutex is safe. Mirrors AddSubscription's ordering.
  Core::CPUThreadGuard guard(m_system);

  std::lock_guard lock(m_mutex);
  const auto it = std::find_if(m_subscriptions.begin(), m_subscriptions.end(),
                                [watch_id](const Subscription& sub) {
                                  return sub.watch_id == watch_id;
                                });
  if (it == m_subscriptions.end())
    return false;
  // The frozen canon must match the subscription's width exactly -- a
  // mismatched length would leave bytes outside `value` either un-frozen or
  // read out of bounds when memcpy'd back into memory.
  if (value.size() != it->count)
    return false;
  it->frozen_value = std::move(value);
  // Reset last_seen to the frozen canon so the next Tick() either sees the
  // cell already at the frozen value (no-op) or restores it and continues
  // without surfacing a spurious change event to the client.
  std::memcpy(it->last_seen.data(), it->frozen_value->data(), it->count);

  // DESNOTE(jbarber, 2026-07-21): Write the canon into guest RAM immediately
  // so the freeze takes effect at once, not on the next vi_end_field_event
  // Tick. The Tick path only runs at field rate while the core is running;
  // when paused (typical right after attach), Tick is never called and the
  // watched bytes would stay at the game value until the user continued.
  // Mirror the per-byte HostWrite path Tick uses: IsValidAddress per byte
  // (a shrunken readable tail can't slip a write through), and WriteU8
  // routes through MMU::HostWrite which bypasses Memcheck (the freeze back-
  // write is not an emulated watchpoint trap). Done under CPUThreadGuard so
  // the writes are atomic with respect to the stepping CPU.
  // DESNOTE(jbarber, 2026-07-22): Invalidate the iCache/JIT block cache over
  // the written range so a frozen code region (a pinned patch or any
  // executable bytes) doesn't leave the interpreter/JIT fetching stale
  // instructions. Mirrors DapDebugController::WriteMemory's post-write
  // flush. Bugbot #61.
  u32 frozen_written = 0;
  if (it->count > 0)
  {
    AddressSpace::Accessors* accessors =
        AddressSpace::GetAccessors(AddressSpace::Type::Effective);
    const u8* frozen = it->frozen_value->data();
    for (u32 i = 0; i < it->count; ++i)
    {
      const u32 addr = it->address + i;
      if (!accessors->IsValidAddress(guard, addr))
        break;
      accessors->WriteU8(guard, addr, frozen[i]);
      ++frozen_written;
    }
  }
  if (frozen_written > 0)
    InvalidateCodeRange(m_system, it->address, frozen_written);
  return true;
}

bool RealtimeWatchSampler::Unfreeze(int watch_id)
{
  std::lock_guard lock(m_mutex);
  const auto it = std::find_if(m_subscriptions.begin(), m_subscriptions.end(),
                               [watch_id](const Subscription& sub) {
                                 return sub.watch_id == watch_id;
                               });
  if (it == m_subscriptions.end())
    return false;
  // Idempotent: clearing an already-clear frozen_value is a no-op, not an
  // error. The client may legitimately call unfreeze on a watch it's unsure
  // about.
  it->frozen_value.reset();
  return true;
}

void RealtimeWatchSampler::Tick()
{
  std::vector<RealtimeWatchChange> changes;
  {
    std::lock_guard lock(m_mutex);
    if (m_subscriptions.empty())
      return;

    Core::CPUThreadGuard guard(m_system);
    AddressSpace::Accessors* accessors =
        AddressSpace::GetAccessors(AddressSpace::Type::Effective);

    for (Subscription& sub : m_subscriptions)
    {
      // Read the current region bytes into `sub.current` in place; the buffer
      // was sized once at AddSubscription time and never reallocates.
      u32 readable = 0;
      for (; readable < sub.count; ++readable)
      {
        const u32 addr = sub.address + readable;
        if (!accessors->IsValidAddress(guard, addr))
          break;
        sub.current[readable] = accessors->ReadU8(guard, addr);
      }
      // DESNOTE(jbarber, 2026-07-21): If the readable tail shrank mid-region,
      // leave the stale bytes in `current` past `readable` but only compare/
      // dispatch the prefix that's actually valid. Track the shrink so
      // reporting carries the original count (the client can tell where the
      // unreadable tail begins).
      sub.readable = readable;

      // Frozen path: if the cell drifts from the canon, write the canon back
      // and suppress the change event. The dispatch callback is never called
      // for a frozen subscription -- the freeze itself is the response.
      if (sub.frozen_value)
      {
        u8* cur = sub.current.data();
        const u8* frozen = sub.frozen_value->data();
        // Only compare/restore the readable prefix; bytes past `readable`
        // in `cur` are stale from a prior Tick and mustn't drive the freeze
        // comparison or the write-back. Without this guard a frozen region
        // whose tail went unreadable would spuriously "drift" on every field
        // (stale current bytes vs canon) and could overwrite the unreadable
        // tail with canon bytes via an unchecked WriteU8.
        const u32 frozen_bytes = std::min(sub.readable, sub.count);
        if (frozen_bytes == 0 ||
            std::memcmp(cur, frozen, frozen_bytes) == 0)
        {
          continue;
        }
        for (u32 i = 0; i < frozen_bytes; ++i)
        {
          const u32 addr = sub.address + i;
          // Skip the write and the mirror when the cell already matches --
          // halves the WriteU8 count on the typical "only r0 changed" case
          // and avoids issuing writes against addresses that may be invalid
          // mid-region. IsValidAddress is re-checked per byte so a shrunken
          // readable tail can't slip a write through to a bad address.
          if (cur[i] == frozen[i] || !accessors->IsValidAddress(guard, addr))
            continue;
          // DESNOTE(jbarber, 2026-07-21): WriteU8 routes through
          // PowerPC::MMU::HostWrite, which calls WriteToHardware<NoException>
          // -- the same primitive CodeWidget / RegisterWidget use for their
          // "set value" path. That primitive does NOT invoke
          // PowerPC::MMU::Memcheck, so data watchpoints race-firing on the
          // canon write-back is a non-issue here. The MMU::Write path
          // (which DOES fire memchecks) is reserved for emulated
          // instructions raising real watchpoint traps.
          accessors->WriteU8(guard, addr, frozen[i]);
          cur[i] = frozen[i];
        }
        // DESNOTE(jbarber, 2026-07-22): Invalidate the iCache/JIT block cache
        // over the frozen range so a frozen code region stays in sync -- the
        // write-back above went through MMU::HostWrite (no icbi), so without
        // this flush the interpreter/JIT would keep executing stale bytes for
        // up to one field between back-writes. Over-invalidation is safe and
        // cheap (32-byte cachelines); the bytes that were skipped (already
        // matching) didn't change and re-flushing their lines is a no-op.
        // Bugbot #61.
        InvalidateCodeRange(m_system, sub.address, frozen_bytes);
        // Keep last_seen in lockstep with the restored prefix so a stable
        // value doesn't keep triggering the write-back path.
        std::memcpy(sub.last_seen.data(), frozen, frozen_bytes);
        continue;
      }

      // Unfrozen path: compare against last_seen and dispatch on drift.
      // DESNOTE(jbarber, 2026-07-21): Bound the comparison to bytes that
      // were readable BOTH this tick AND the previous tick (min(readable,
      // prev_readable)). Bytes that just became readable had no real prior
      // value in last_seen -- the previous Tick couldn't read them -- and
      // comparing against the zero-fill there would always show "changed"
      // even though guest memory was stable. The newly readable bytes are
      // still useful to the client (they reveal the cell's current value),
      // so when `readable > prev_readable` we dispatch unconditionally to
      // surface them; otherwise we compare only the previously-readable
      // prefix and dispatch only if that prefix actually drifted.
      const u32 prev_readable = sub.prev_readable;
      const u32 compare_bytes = std::min(sub.readable, prev_readable);
      bool changed = false;
      if (compare_bytes > 0)
        changed = std::memcmp(sub.current.data(), sub.last_seen.data(), compare_bytes) != 0;
      const bool grew = sub.readable > prev_readable;
      if (!changed && !grew)
      {
        // No drift and no new bytes surfaced: nothing to report. Keep
        // last_seen and prev_readable as-is so the next tick has the same
        // baseline.
        continue;
      }

      RealtimeWatchChange change;
      change.watch_id = sub.watch_id;
      change.address = sub.address;
      change.count = sub.count;
      change.bytes.assign(sub.current.begin(), sub.current.begin() + sub.readable);
      changes.push_back(std::move(change));
      // Sync last_seen to the freshly-read prefix; bytes past `readable` are
      // left untouched (they're stale from a prior tick, but the next Tick
      // will overwrite stale bytes if they become readable again -- and the
      // compare window above is bounded by prev_readable, so stale tail
      // bytes can't drive the comparison).
      std::memcpy(sub.last_seen.data(), sub.current.data(), sub.readable);
      sub.prev_readable = sub.readable;
    }
  }

  if (!changes.empty() && m_dispatch)
    m_dispatch(changes);
}
}  // namespace DAP
