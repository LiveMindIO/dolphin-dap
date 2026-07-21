// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <map>
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
  // DESNOTE(jbarber, 2026-07-21): The underlying StepInto didn't complete
  // (async StepOpcode timed out without CPU thread acknowledgement), so
  // the PC has not advanced. The session suppresses the stopped event
  // rather than emit a false stop for an unchanged PC.
  NotStepped,
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

struct SourceBreakpointContext
{
  std::optional<int> source_reference;
  std::optional<std::string> source_path;
  std::optional<std::string> source_name;
};

struct SourceBreakpointSpec
{
  u32 line = 0;
  std::optional<std::string> condition;
};

struct DataBreakpointRequest
{
  u32 address = 0;
  // Number of bytes to watch starting at `address`. Defaults to 1 (a single-
  // byte DAP data breakpoint). length > 1 installs a ranged PPC memcheck
  // ([address, address+length-1]).
  u32 length = 1;
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
  // Steps one PPC instruction in interpreter mode. Returns true when the step
  // completed synchronously (the CPUThreadGuard path), was a no-op on an
  // already-stopped core, or the async StepOpcode signal fired within its
  // 2s wait. Returns false only when the step was attempted but the CPU
  // thread didn't acknowledge the StepOpcode within the 2s timeout — in that
  // case the PC hasn't advanced and callers must not emit a `stopped`/`step`
  // event (the late completion has no observable state transition for
  // PollBreakpointStop to catch, so suppressing the stop here is the only
  // correct response).
  bool StepInto();
  StepOverResult StepOver();
  // Steps until the current function returns, a breakpoint is hit, or `timeout`
  // wall-clock time elapses. The timeout bounds otherwise non-returning code
  // (e.g. an infinite loop) and is injectable so it can be exercised in tests.
  void StepOut(std::chrono::milliseconds timeout = std::chrono::seconds(5));
  void SetCodeBreakpoints(std::vector<CodeBreakpointRequest> breakpoints);
  std::vector<std::optional<u32>> UpdateSourceBreakpoints(std::string_view source_key,
                                                          const SourceBreakpointContext& context,
                                                          std::vector<SourceBreakpointSpec> breakpoints);
  void UpdateInstructionBreakpoints(std::vector<CodeBreakpointRequest> breakpoints);
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
  // returns the number written; stops at the first invalid address. After
  // the bytes land, invalidates the iCache and JIT block cache for every
  // 32-byte cacheline the write touched so interpreter and JIT fetches see
  // the new bytes (MMU::WriteToHardware bypasses the icbi path).
  std::size_t WriteMemory(u32 address, std::span<const u8> data);
  // Invalidates the L1 iCache and JIT block cache for every 32-byte
  // cacheline overlapping [address, address+length). Mirrors the icbi path
  // (InstructionCache::Invalidate) per cacheline so callers that write code
  // directly via AddressSpace (which has no icbi hook) stay consistent with
  // the icbi-cleared state.
  void InvalidateCodeRange(u32 address, std::size_t length);
  // Scans MEM1 RAM for the smallest 4-byte-aligned zero-run >= `count` and
  // returns its canonical effective address (0x80000000+offset). Returns
  // nullopt when no such region exists. Used by clients that need a code
  // cave but don't know the game's memory layout.
  std::optional<u32> FindFreeMemory(u32 count);
  // Writes PPC machine code at `address` (when provided) or at an
  // auto-allocated region (via FindFreeMemory). Returns the address the
  // code was written to, or 0 on failure (invalid address, no free region,
  // or a short WriteMemory). The caller can `goto` the returned address or
  // set a breakpoint at its entry. WriteMemory handles iCache/JIT
  // invalidation so the injected bytes are live immediately.
  u32 InjectCode(std::optional<u32> address, std::vector<u8> code);
  // Result of a transparent detour. `original_instruction` are the 4 bytes
  // that lived at `target_address` before patching; the trampoline replays
  // them so the patched-out instruction still executes.
  struct DetourResult
  {
    u32 target_address = 0;
    u32 detour_address = 0;
    u32 trampoline_address = 0;
    std::vector<u8> original_instruction;
  };
  // Installs a transparent detour at `target_address`:
  //   1. If `detour_address` is nullopt, allocates a region large enough for
  //      detour_body + appended `b trampoline` + trampoline (original + `b
  //      target+4`) via FindFreeMemory.
  //   2. Writes detour_body at detour_address.
  //   3. Appends `b trampoline_address` to the detour so control resumes at
  //      the trampoline after the body.
  //   4. Writes the trampoline at trampoline_address: original_instruction
  //      followed by `b target+4`.
  //   5. Patches target_address with `b detour_address`.
  // Returns nullopt when the target address can't be read or no free region
  // of the required size exists. All writes invalidate the iCache/JIT.
  std::optional<DetourResult> Detour(u32 target_address, std::optional<u32> detour_address,
                                     std::vector<u8> detour_body);
  std::string Disassemble(u32 address, int instruction_count);

  u32 GetPC();
  void SetPC(u32 address);
  // Classifies why the core is stopped at the current PC (code breakpoint,
  // memory watchpoint, or a plain step) for the DAP `stopped` event.
  StopInfo GetStopInfo();
  // Returns pending PPC exceptions, or nullopt when none are raised.
  std::optional<ExceptionInfo> GetExceptionInfo();

  std::optional<u32> ResolveSourceLineBreakpoint(const SourceBreakpointContext& context, u32 line);

private:
  void ApplyCodeBreakpoints(const std::vector<CodeBreakpointRequest>& breakpoints);
  void ReapplyCodeBreakpoints();

  Core::System& m_system;
  std::map<std::string, std::vector<CodeBreakpointRequest>> m_source_breakpoints;
  std::vector<CodeBreakpointRequest> m_instruction_breakpoints;
};
}  // namespace DAP
