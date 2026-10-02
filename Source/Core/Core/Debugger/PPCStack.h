// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace Core::Debug
{
struct StackFrameSource
{
  std::string file;
  u32 file_index = 0;
  u32 line = 0;
};

struct StackFrame
{
  std::size_t index = 0;
  u32 address = 0;
  std::string name;
  std::optional<StackFrameSource> source;
  std::optional<u32> disassembly_base;
  u32 disassembly_line = 1;
};

struct StackTrace
{
  std::vector<StackFrame> frames;
  std::size_t total_frames = 0;
};

StackTrace GetPPCStackTrace(Core::System& system, int start_frame = 0, int levels = 0);
}  // namespace Core::Debug
