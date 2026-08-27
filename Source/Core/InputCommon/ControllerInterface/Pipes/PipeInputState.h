// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "Common/CommonTypes.h"
#include "Common/Timer.h"

namespace ciface::Pipes
{
struct InputUpdateState
{
  u64 input_requests = 0;
  bool synchronize_gameplay = false;
};

constexpr u8 SYNCHRONIZE_GAMEPLAY = 1 << 0;

extern std::atomic<u8> g_input_state;
extern std::atomic<u64> g_pending_input_requests;
extern std::atomic<u64> g_input_request_sequence;
extern std::atomic<u64> g_last_input_request_us;
extern std::atomic<u64> g_last_consumed_request_sequence;
extern std::atomic<u64> g_last_consumed_request_us;
extern std::atomic<u64> g_last_si_update_us;
extern thread_local InputUpdateState g_current_input_update;

struct InputTimingSnapshot
{
  u64 request_sequence;
  u64 last_request_us;
  u64 consumed_request_sequence;
  u64 last_consumed_request_us;
  u64 last_si_update_us;
};

inline void PublishInputState(bool synchronize_gameplay, bool request_input)
{
  g_input_state.store(synchronize_gameplay ? SYNCHRONIZE_GAMEPLAY : 0, std::memory_order_release);

  if (request_input)
  {
    g_last_input_request_us.store(Common::Timer::NowUs(), std::memory_order_relaxed);
    g_input_request_sequence.fetch_add(1, std::memory_order_release);
    g_pending_input_requests.fetch_add(1, std::memory_order_release);
  }
}

inline InputUpdateState CaptureInputState()
{
  const u8 state = g_input_state.load(std::memory_order_acquire);
  const u64 input_requests = g_pending_input_requests.exchange(0, std::memory_order_acq_rel);
  if (input_requests != 0)
  {
    g_last_consumed_request_sequence.store(g_input_request_sequence.load(std::memory_order_acquire),
                                           std::memory_order_relaxed);
    g_last_consumed_request_us.store(Common::Timer::NowUs(), std::memory_order_release);
  }
  return {
      .input_requests = input_requests,
      .synchronize_gameplay = (state & SYNCHRONIZE_GAMEPLAY) != 0,
  };
}

inline bool IsInputRequested()
{
  return g_pending_input_requests.load(std::memory_order_acquire) != 0;
}

inline void RecordSIUpdate(u64 now_us)
{
  g_last_si_update_us.store(now_us, std::memory_order_release);
}

inline InputTimingSnapshot GetInputTimingSnapshot()
{
  const u64 sequence = g_input_request_sequence.load(std::memory_order_acquire);
  const u64 consumed_us = g_last_consumed_request_us.load(std::memory_order_acquire);
  return {
      .request_sequence = sequence,
      .last_request_us = g_last_input_request_us.load(std::memory_order_relaxed),
      .consumed_request_sequence = g_last_consumed_request_sequence.load(std::memory_order_relaxed),
      .last_consumed_request_us = consumed_us,
      .last_si_update_us = g_last_si_update_us.load(std::memory_order_acquire),
  };
}
}  // namespace ciface::Pipes
