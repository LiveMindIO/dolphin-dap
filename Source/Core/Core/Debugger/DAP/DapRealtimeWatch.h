// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"

namespace Core
{
class System;
}

namespace DAP
{
// A single region whose change is reported to the dispatch callback.
struct RealtimeWatchChange
{
  int watch_id = 0;
  u32 address = 0;
  u32 count = 0;
  std::vector<u8> bytes;
};

// Realtime memory region watcher. A client subscribes with an (address, count)
// pair (e.g. "0xdeadb33f" + 0x100); the sampler re-reads every subscribed
// region on each emulated video field and, when its contents differ from the
// previously seen value, hands the new bytes to the dispatch callback.
//
// Sampling runs on the CPU thread via the `vi_end_field_event` hook (the same
// signal the Qt `MemoryViewWidget`/`CheatsManager` use for frame-accurate
// realtime memory views). Because Tick() executes on the CPU thread, the
// CPUThreadGuard taken for the read is a no-op pause -- emulation is never
// stalled to satisfy a watch, and events arrive at field rate (~60 Hz NTSC,
// ~50 Hz PAL) rather than at the session poll cadence.
//
// Subscription add/remove run on the session (request-handling) thread; Tick()
// runs on the CPU thread. A mutex guards the subscription list and the
// per-subscription buffers; the dispatch callback is invoked outside that mutex
// is *not* held (callback owns its own synchronization for the outbound
// socket).
class RealtimeWatchSampler
{
public:
  using DispatchCallback = std::function<void(const std::vector<RealtimeWatchChange>&)>;

  // Registers a `vi_end_field_event` listener that drives Tick(). The callback
  // is stored and invoked on the CPU thread; it must be safe to call from
  // there (e.g. enqueue into a mutex-guarded event queue).
  RealtimeWatchSampler(Core::System& system, DispatchCallback dispatch);
  ~RealtimeWatchSampler();

  RealtimeWatchSampler(const RealtimeWatchSampler&) = delete;
  RealtimeWatchSampler& operator=(const RealtimeWatchSampler&) = delete;

  // Adds a subscription for [address, address+count). The current region
  // contents are read immediately and seeded as "last seen", so only
  // subsequent changes emit events. Returns a watch id > 0.
  int AddSubscription(u32 address, u32 count);

  // Removes the subscription with the given watch id. Returns true on hit.
  bool RemoveSubscription(int watch_id);

  // Re-reads every subscribed region and dispatches changes. Public so tests
  // can drive sampling without booting the video core (vi_end_field_event is
  // otherwise only fired by VideoInterface during emulation).
  void Tick();

private:
  struct Subscription
  {
    int watch_id = 0;
    u32 address = 0;
    u32 count = 0;
    std::vector<u8> last_seen;
    std::vector<u8> current;
  };

  Core::System& m_system;
  DispatchCallback m_dispatch;
  std::mutex m_mutex;
  std::vector<Subscription> m_subscriptions;
  std::atomic<int> m_next_id{1};
  Common::EventHook m_frame_hook;
};
}  // namespace DAP
