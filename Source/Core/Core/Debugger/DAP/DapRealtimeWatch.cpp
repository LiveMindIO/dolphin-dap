// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapRealtimeWatch.h"

#include <algorithm>
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
  {
    Core::CPUThreadGuard guard(m_system);
    AddressSpace::Accessors* accessors =
        AddressSpace::GetAccessors(AddressSpace::Type::Effective);
    for (u32 i = 0; i < count; ++i)
    {
      const u32 addr = address + i;
      if (!accessors->IsValidAddress(guard, addr))
      {
        seed.resize(i);
        break;
      }
      seed[i] = accessors->ReadU8(guard, addr);
    }
  }

  std::lock_guard lock(m_mutex);
  Subscription sub;
  sub.watch_id = m_next_id++;
  sub.address = address;
  sub.count = count;
  sub.last_seen = std::move(seed);
  sub.current.assign(count, 0);
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
      sub.current.assign(sub.count, 0);
      for (u32 i = 0; i < sub.count; ++i)
      {
        const u32 addr = sub.address + i;
        if (!accessors->IsValidAddress(guard, addr))
        {
          // Shrink so the comparison reflects only readable bytes; the
          // reported region still carries the original `count` so the client
          // can tell where unreadable tail begins.
          sub.current.resize(i);
          break;
        }
        sub.current[i] = accessors->ReadU8(guard, addr);
      }

      if (sub.current == sub.last_seen)
        continue;

      RealtimeWatchChange change;
      change.watch_id = sub.watch_id;
      change.address = sub.address;
      change.count = sub.count;
      change.bytes = sub.current;
      changes.push_back(std::move(change));
      sub.last_seen = sub.current;
    }
  }

  if (!changes.empty() && m_dispatch)
    m_dispatch(changes);
}
}  // namespace DAP
