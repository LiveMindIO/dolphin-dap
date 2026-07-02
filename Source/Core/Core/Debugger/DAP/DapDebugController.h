// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace DAP
{
struct RegisterSnapshot
{
  std::array<u32, 32> gpr{};
  u32 pc = 0;
  u32 lr = 0;
  u32 ctr = 0;
  u32 msr = 0;
  u32 cr = 0;
  u32 xer = 0;
};

class DapDebugController
{
public:
  explicit DapDebugController(Core::System& system);

  void Continue();
  void Pause();
  void StepInto();
  void SetCodeBreakpoints(const std::vector<u32>& addresses);

  RegisterSnapshot GetRegisters();
  std::vector<u8> ReadMemory(u32 address, std::size_t size);
  std::string Disassemble(u32 address, int instruction_count);

  u32 GetPC();

private:
  Core::System& m_system;
};
}  // namespace DAP
