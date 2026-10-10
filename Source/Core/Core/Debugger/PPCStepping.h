// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace Core::Debug
{
enum class PPCStepGranularity
{
  Instruction,
  SourceRow,
};

enum class PPCStepMode
{
  Into,
  Over,
  Out,
};

enum class PPCStepResult
{
  Stepped,
  Continuing,
  NotStepped,
};

struct PPCStepOptions
{
  const std::atomic<bool>* cancelled = nullptr;
  std::chrono::milliseconds instruction_timeout = std::chrono::seconds(2);
  std::chrono::milliseconds timeout = std::chrono::seconds(5);
  std::size_t instruction_cap = 1000000;
  bool ignore_current_code_breakpoint = false;
  std::function<void(u32)> temporary_breakpoint_installed;
};

// Source-row stepping runs synchronously until execution leaves the exact DWARF file identity and
// line. Out always uses the instruction-level return operation because it has no row boundary.
PPCStepResult StepPPC(Core::System& system, PPCStepMode mode, PPCStepGranularity granularity,
                      const PPCStepOptions& options = {});
}  // namespace Core::Debug
