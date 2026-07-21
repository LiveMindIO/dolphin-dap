// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"

namespace Core
{
class System;
}

namespace DAP
{
// Return value of AddSubscription when the (address, count) arguments are
// rejected (count == 0 or address + count would wrap past u32 max).
inline constexpr int kInvalidWatchId = 0;

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
//
// Freeze layer: a subscription may optionally be "frozen" -- when set, Tick()
// writes the frozen bytes back to memory whenever the region drifts instead of
// dispatching a change event. The write happens at field rate on the CPU
// thread, so the restoration is timely but not atomic: the game's write lands
// for up to one field (~16 ms NTSC) before the next Tick restores it. That's
// acceptable for the canonical use cases (health/ammo/coins/timer) and is
// far cheaper than a per-write MMU interceptor would be.
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
  // subsequent changes emit events. Returns a watch id > 0, or
  // kInvalidWatchId on overflow/rejection.
  int AddSubscription(u32 address, u32 count);

  // Removes the subscription with the given watch id. Returns true on hit.
  bool RemoveSubscription(int watch_id);

  // Freezes an existing subscription: on every subsequent Tick(), if the
  // region's current contents differ from `value`, the sampler writes `value`
  // back to memory and suppresses the change event. `value.size()` must equal
  // the subscription's `count`. Returns false if no such watch_id or size
  // mismatch.
  bool Freeze(int watch_id, std::vector<u8> value);

  // Clears the freeze layer on an existing subscription; the watch continues
  // to live and dispatch change events normally. Returns true if the watch
  // exists (idempotent: returns true even if it wasn't frozen). Returns false
  // if no such watch_id.
  bool Unfreeze(int watch_id);

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
    // DESNOTE(jbarber, 2026-07-21): All three buffers are sized exactly once
    // at AddSubscription time and never reallocated. Tick() reads into
    // `current` in place, memcmps against `last_seen`, and swaps the two
    // pointers (not their storage) on a change. `frozen_value`, when set,
    // is the canon the cell is held to: any drift in `current` triggers a
    // write-back and no dispatch.
    std::vector<u8> last_seen;
    std::vector<u8> current;
    std::optional<std::vector<u8>> frozen_value;
    // Number of bytes successfully read in the last Tick(); <= count when the
    // readable tail of the region shrank. The reported region still carries
    // the original `count` so the client can tell where unreadable tail begins.
    u32 readable = 0;
  };

  Core::System& m_system;
  DispatchCallback m_dispatch;
  std::mutex m_mutex;
  std::vector<Subscription> m_subscriptions;
  std::atomic<int> m_next_id{1};
  Common::EventHook m_frame_hook;
};
}  // namespace DAP
