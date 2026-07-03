// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <span>
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

enum class StepOverResult
{
  Stepped,
  Continuing,
};

class DapDebugController
{
public:
  explicit DapDebugController(Core::System& system);

  void Continue();
  void Pause();
  void StepInto();
  StepOverResult StepOver();
  void StepOut();
  void SetCodeBreakpoints(const std::vector<u32>& addresses);

  RegisterSnapshot GetRegisters();
  std::vector<u8> ReadMemory(u32 address, std::size_t size);
  // Writes as many leading bytes of `data` as map to valid addresses and
  // returns the number written; stops at the first invalid address.
  std::size_t WriteMemory(u32 address, std::span<const u8> data);
  std::string Disassemble(u32 address, int instruction_count);

  u32 GetPC();

private:
  Core::System& m_system;
};
}  // namespace DAP
