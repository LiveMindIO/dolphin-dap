// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapRealtimeWatch.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "Core/Core.h"
#include "Core/HW/AddressSpace.h"
#include "Core/System.h"
#include "VideoCommon/VideoEvents.h"

namespace DAP
{
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
  // DESNOTE(jbarber, 2026-07-21): Reject ranges that wrap past u32 max so later
  // `address + i` arithmetic in Tick() can't silently read from low memory.
  // A subscription of count == 0 is meaningless and would make the change
  // detector noisy without producing useful bytes.
  if (count == 0 || address > std::numeric_limits<u32>::max() - count)
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
        if (std::memcmp(cur, frozen, sub.count) == 0)
          continue;
        for (u32 i = 0; i < sub.count; ++i)
        {
          const u32 addr = sub.address + i;
          // Only write back bytes that differ from what's in memory now --
          // skip the ones that already match to halve the WriteU8 calls on
          // the typical "only r0 changed" case. Writes go through the same
          // CPUThreadGuard as reads; the accessors handle the page-table walk.
          if (cur[i] != frozen[i])
            accessors->WriteU8(guard, addr, frozen[i]);
          // Mirror into `current` so a later `memcmp` against last_seen
          // (which we keep in lockstep below) doesn't re-flag this as a
          // change on the next field.
          cur[i] = frozen[i];
        }
        // Keep last_seen in lockstep with the restored cell so a stable
        // value doesn't keep triggering the write-back path.
        std::memcpy(sub.last_seen.data(), frozen, sub.count);
        continue;
      }

      // Unfrozen path: compare against last_seen and dispatch on drift.
      if (sub.readable == 0 ||
          std::memcmp(sub.current.data(), sub.last_seen.data(), sub.readable) == 0)
      {
        continue;
      }

      RealtimeWatchChange change;
      change.watch_id = sub.watch_id;
      change.address = sub.address;
      change.count = sub.count;
      change.bytes.assign(sub.current.begin(), sub.current.begin() + sub.readable);
      changes.push_back(std::move(change));
      std::swap(sub.last_seen, sub.current);
    }
  }

  if (!changes.empty() && m_dispatch)
    m_dispatch(changes);
}
}  // namespace DAP
