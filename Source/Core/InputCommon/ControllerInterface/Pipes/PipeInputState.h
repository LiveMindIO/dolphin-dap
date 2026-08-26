// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "Common/CommonTypes.h"

namespace ciface::Pipes
{
struct InputUpdateState
{
  bool input_requested = false;
  bool synchronize_gameplay = false;
};

constexpr u8 INPUT_REQUESTED = 1 << 0;
constexpr u8 SYNCHRONIZE_GAMEPLAY = 1 << 1;

extern std::atomic<u8> g_input_state;
extern thread_local InputUpdateState g_current_input_update;

inline void PublishInputState(bool synchronize_gameplay, bool request_input)
{
  u8 current = g_input_state.load();
  u8 desired;
  do
  {
    desired = current & INPUT_REQUESTED;
    if (synchronize_gameplay)
      desired |= SYNCHRONIZE_GAMEPLAY;
    if (request_input)
      desired |= INPUT_REQUESTED;
  } while (!g_input_state.compare_exchange_weak(current, desired));
}

inline InputUpdateState CaptureInputState()
{
  const u8 state = g_input_state.fetch_and(SYNCHRONIZE_GAMEPLAY);
  return {
      .input_requested = (state & INPUT_REQUESTED) != 0,
      .synchronize_gameplay = (state & SYNCHRONIZE_GAMEPLAY) != 0,
  };
}
}  // namespace ciface::Pipes
