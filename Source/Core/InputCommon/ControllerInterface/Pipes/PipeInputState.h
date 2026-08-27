// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <limits>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Common/Timer.h"

namespace ciface::Pipes
{
enum class InputRequestSource : u8
{
  Unknown,
  GameStart,
  MenuFrame,
  FrameBookend,
};

constexpr s32 INPUT_FRAME_UNKNOWN = std::numeric_limits<s32>::min();

constexpr std::string_view InputRequestSourceName(InputRequestSource source)
{
  switch (source)
  {
  case InputRequestSource::GameStart:
    return "game_start";
  case InputRequestSource::MenuFrame:
    return "menu_frame";
  case InputRequestSource::FrameBookend:
    return "frame_bookend";
  default:
    return "unknown";
  }
}

struct InputUpdateState
{
  u64 input_requests = 0;
  bool synchronize_gameplay = false;
  u64 request_sequence = 0;
  s32 request_frame = INPUT_FRAME_UNKNOWN;
  InputRequestSource request_source = InputRequestSource::Unknown;
};

constexpr u8 SYNCHRONIZE_GAMEPLAY = 1 << 0;

extern std::atomic<u8> g_input_state;
extern std::atomic<u64> g_pending_input_requests;
extern std::atomic<u64> g_input_request_sequence;
extern std::atomic<u64> g_last_input_request_us;
extern std::atomic<s32> g_last_input_request_frame;
extern std::atomic<u8> g_last_input_request_source;
extern std::atomic<u64> g_last_consumed_request_sequence;
extern std::atomic<u64> g_last_consumed_request_us;
extern std::atomic<u64> g_last_si_update_us;
extern thread_local InputUpdateState g_current_input_update;

struct InputTimingSnapshot
{
  u64 request_sequence;
  u64 last_request_us;
  s32 request_frame;
  InputRequestSource request_source;
  u64 consumed_request_sequence;
  u64 last_consumed_request_us;
  u64 last_si_update_us;
};

inline void PublishInputState(bool synchronize_gameplay, bool request_input,
                              InputRequestSource source = InputRequestSource::Unknown,
                              s32 frame = INPUT_FRAME_UNKNOWN)
{
  g_input_state.store(synchronize_gameplay ? SYNCHRONIZE_GAMEPLAY : 0, std::memory_order_release);

  if (request_input)
  {
    g_last_input_request_us.store(Common::Timer::NowUs(), std::memory_order_relaxed);
    g_last_input_request_frame.store(frame, std::memory_order_relaxed);
    g_last_input_request_source.store(static_cast<u8>(source), std::memory_order_relaxed);
    g_input_request_sequence.fetch_add(1, std::memory_order_release);
    g_pending_input_requests.fetch_add(1, std::memory_order_release);
  }
}

inline InputUpdateState CaptureInputState()
{
  const u8 state = g_input_state.load(std::memory_order_acquire);
  const u64 input_requests = g_pending_input_requests.exchange(0, std::memory_order_acq_rel);
  const u64 request_sequence = g_input_request_sequence.load(std::memory_order_acquire);
  if (input_requests != 0)
  {
    g_last_consumed_request_sequence.store(request_sequence, std::memory_order_relaxed);
    g_last_consumed_request_us.store(Common::Timer::NowUs(), std::memory_order_release);
  }
  return {
      .input_requests = input_requests,
      .synchronize_gameplay = (state & SYNCHRONIZE_GAMEPLAY) != 0,
      .request_sequence = request_sequence,
      .request_frame = g_last_input_request_frame.load(std::memory_order_relaxed),
      .request_source = static_cast<InputRequestSource>(
          g_last_input_request_source.load(std::memory_order_relaxed)),
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
      .request_frame = g_last_input_request_frame.load(std::memory_order_relaxed),
      .request_source = static_cast<InputRequestSource>(
          g_last_input_request_source.load(std::memory_order_relaxed)),
      .consumed_request_sequence = g_last_consumed_request_sequence.load(std::memory_order_relaxed),
      .last_consumed_request_us = consumed_us,
      .last_si_update_us = g_last_si_update_us.load(std::memory_order_acquire),
  };
}
}  // namespace ciface::Pipes
