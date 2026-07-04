// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
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

struct ThreadInfo
{
  int id = 1;
  std::string name;
};

struct StackFrame
{
  int id = 0;
  u32 address = 0;
  std::string name;
  // When DWARF line info is available, source_file holds the path and source_line
  // is the 1-based source line. Otherwise source_base + source_line map to a
  // disassembly pseudo-source (instruction index from base).
  std::optional<std::string> source_file;
  std::optional<u32> source_base;
  int source_line = 0;
};

struct StackTraceResult
{
  std::vector<StackFrame> frames;
  int total_frames = 0;
};

struct LoadedSource
{
  int source_reference = 0;
  std::string name;
  std::string path;
};

struct SourceContent
{
  std::string content;
  std::string mime_type;
};

struct BreakpointLocation
{
  int line = 0;
};

struct CodeBreakpointRequest
{
  u32 address = 0;
  std::optional<std::string> condition;
};

struct DataBreakpointRequest
{
  u32 address = 0;
  bool read = false;
  bool write = false;
  std::optional<std::string> condition;
};

enum class StopReason
{
  Step,
  CodeBreakpoint,
  DataBreakpoint,
};

struct StopInfo
{
  StopReason reason = StopReason::Step;
  // Set when the stop is attributable to a breakpoint/watchpoint at the PC.
  std::optional<u32> hit_breakpoint_address;
};

struct ExceptionInfo
{
  u32 exceptions = 0;
  std::string description;
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
  void SetCodeBreakpoints(std::vector<CodeBreakpointRequest> breakpoints);
  void SetDataBreakpoints(std::vector<DataBreakpointRequest> breakpoints);
  // Evaluates a PPC debugger expression (same syntax as breakpoint conditions).
  std::optional<std::string> EvaluateExpression(std::string_view expression);

  RegisterSnapshot GetRegisters();
  // Writes a register exposed by the `variables` scopes. Returns the new value
  // on success, or nullopt when the scope, name, value, or writability is invalid.
  std::optional<u32> SetRegister(int variables_reference, std::string_view name,
                                 std::string_view value);
  std::vector<ThreadInfo> GetThreads();
  StackTraceResult GetStackTrace(int start_frame = 0, int levels = 20);
  std::vector<LoadedSource> GetLoadedSources();
  std::optional<SourceContent> GetSource(u32 base_address, int start_line, int end_line);
  std::vector<BreakpointLocation> GetBreakpointLocations(u32 base_address, int start_line,
                                                         int end_line);
  void Restart();
  void Terminate();
  std::vector<u8> ReadMemory(u32 address, std::size_t size);
  // Writes as many leading bytes of `data` as map to valid addresses and
  // returns the number written; stops at the first invalid address.
  std::size_t WriteMemory(u32 address, std::span<const u8> data);
  std::string Disassemble(u32 address, int instruction_count);

  u32 GetPC();
  void SetPC(u32 address);
  // Classifies why the core is stopped at the current PC (code breakpoint,
  // memory watchpoint, or a plain step) for the DAP `stopped` event.
  StopInfo GetStopInfo();
  // Returns pending PPC exceptions, or nullopt when none are raised.
  std::optional<ExceptionInfo> GetExceptionInfo();

private:
  Core::System& m_system;
};
}  // namespace DAP
