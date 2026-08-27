// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "Common/CommonTypes.h"
namespace ExpansionInterface
{
constexpr std::size_t MENU_FRAME_PAYLOAD_KIND_OFFSET = 0x53;
constexpr u8 MENU_FRAME_PAYLOAD_KIND_PAUSE_OPEN = 1;
constexpr u8 MENU_FRAME_PAYLOAD_KIND_PAUSE_CLOSE = 2;

struct MenuFrameInputState
{
  bool input_requested = false;
  bool synchronize_gameplay = false;
};

inline MenuFrameInputState GetMenuFrameInputState(std::span<const u8> payload)
{
  const u16 scene = payload.size() >= 3 ? u16(payload[1]) << 8 | payload[2] : 0;
  const u8 payload_kind =
      payload.size() > MENU_FRAME_PAYLOAD_KIND_OFFSET ? payload[MENU_FRAME_PAYLOAD_KIND_OFFSET] : 0;
  const bool is_pause_transition = payload_kind == MENU_FRAME_PAYLOAD_KIND_PAUSE_OPEN ||
                                   payload_kind == MENU_FRAME_PAYLOAD_KIND_PAUSE_CLOSE;
  const bool is_in_game_telemetry =
      !is_pause_transition && (scene == 0x0202 || scene == 0x0208 || scene == 0x0302);
  return {
      .input_requested = !is_in_game_telemetry,
      .synchronize_gameplay = is_in_game_telemetry,
  };
}
}  // namespace ExpansionInterface
