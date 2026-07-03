// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
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
constexpr int REGISTERS_SCOPE = 1000;
constexpr int PC_SCOPE = 1001;

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
  // Steps until the current function returns, a breakpoint is hit, or `timeout`
  // wall-clock time elapses. The timeout bounds otherwise non-returning code
  // (e.g. an infinite loop) and is injectable so it can be exercised in tests.
  void StepOut(std::chrono::milliseconds timeout = std::chrono::seconds(5));
  void SetCodeBreakpoints(const std::vector<u32>& addresses);

  RegisterSnapshot GetRegisters();
  // Writes a register exposed by the `variables` scopes. Returns the new value
  // on success, or nullopt when the scope, name, value, or writability is invalid.
  std::optional<u32> SetRegister(int variables_reference, std::string_view name,
                                 std::string_view value);
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
