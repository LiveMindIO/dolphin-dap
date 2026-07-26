// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapDebugController.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "Common/Event.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/StringUtil.h"
#include "Common/SymbolDB.h"
#include "Core/Core.h"
#include "Core/Debugger/DAP/DapJson.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/Expression.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCCache.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace DAP
{
namespace
{
// Encodes a PPC `b target` instruction relative to `pc`. PPC b: opcode 18
// (0x48000000, MSB-numbered bits 0-5 = LSB u32 bits 31-26), LI is the
// 24-bit signed offset/4 placed at MSB-numbered bits 6-29 (= LSB u32 bits
// 2-25, mask 0x03FFFFFC), AA=0 (relative, bit 30), LK=0 (no link, bit 31).
// Compute the offset in s32 so backward branches (target < pc) sign-extend
// correctly when masking the LI field. The previous iterative fix used
// `<< 6` (placing LI at u32 bits 6-29, mask 0x3FFFFFC0), which clobbered
// the opcode on negative LI values: a 24-bit value with its sign bit set,
// shifted left by 6, lands at bit 29 of the result and spills across the
// opcode boundary when OR'd with 0x48000000. The correct placement is
// `<< 2` (LSB u32 bits 2-25), which leaves bits 26-31 clear for the opcode.
// DESNOTE(jbarber, 2026-07-21): Return nullopt when the signed offset falls
// outside the 24-bit signed displacement PPC `b` supports (±32 MiB). The
// previous form masked the LI field silently, so a distant detour
// (detourAddress more than ~32 MiB from the patch site) encoded the wrong
// target and quietly corrupted control flow at runtime. Detour rejects
// any out-of-range branch now, restoring memory to its pre-Detour state.
constexpr std::optional<u32> MakeBranchInstruction(u32 pc, u32 target)
{
  const s32 offset = static_cast<s32>(target - pc);
  const s32 li = offset >> 2;  // arithmetic shift: signed div-by-4
  // LI is a 24-bit signed field, so its representable range is
  // [-2^23, 2^23 - 1]. Anything outside that can't be encoded as a relative
  // `b` -- callers must fall back to a sequence (e.g. `lis + ori + mtctr +
  // bctr`) or relocate the detour closer to the patch site.
  if (li < -(1 << 23) || li >= (1 << 23))
    return std::nullopt;
  const u32 masked_li = static_cast<u32>(li) & 0x00FFFFFFu;
  return 0x48000000u | (masked_li << 2);
}

constexpr std::array<u8, 4> BigEndianBytes(u32 word)
{
  return {static_cast<u8>(word >> 24), static_cast<u8>(word >> 16),
          static_cast<u8>(word >> 8), static_cast<u8>(word)};
}

constexpr u32 ReadBigEndianU32(std::span<const u8> bytes)
{
  return (static_cast<u32>(bytes[0]) << 24) | (static_cast<u32>(bytes[1]) << 16) |
         (static_cast<u32>(bytes[2]) << 8) | static_cast<u32>(bytes[3]);
}

bool WillInstructionReturn(Core::System& system, UGeckoInstruction inst)
{
  if (inst.hex == 0x4C000064u)
    return true;

  const auto& ppc_state = system.GetPPCState();
  const bool counter =
      (inst.BO_2 >> 2 & 1) != 0 || (CTR(ppc_state) != 0) != ((inst.BO_2 >> 1 & 1) != 0);
  const bool condition =
      inst.BO_2 >> 4 != 0 || ppc_state.cr.GetBit(inst.BI_2) == (inst.BO_2 >> 3 & 1);
  const bool is_bclr = inst.OPCD_7 == 0b010011 && inst.XO == 16;
  return is_bclr && counter && condition && !inst.LK_3;
}

std::string DescribeExceptions(u32 exceptions)
{
  static constexpr std::array<std::pair<u32, std::string_view>, 10> NAMES{{
      {EXCEPTION_DECREMENTER, "Decrementer"},
      {EXCEPTION_SYSCALL, "Syscall"},
      {EXCEPTION_EXTERNAL_INT, "External Interrupt"},
      {EXCEPTION_DSI, "DSI"},
      {EXCEPTION_ISI, "ISI"},
      {EXCEPTION_ALIGNMENT, "Alignment"},
      {EXCEPTION_FPU_UNAVAILABLE, "FPU Unavailable"},
      {EXCEPTION_PROGRAM, "Program"},
      {EXCEPTION_PERFORMANCE_MONITOR, "Performance Monitor"},
      {EXCEPTION_FAKE_MEMCHECK_HIT, "Memcheck Hit"},
  }};

  std::string result;
  u32 remaining = exceptions;
  for (const auto& [mask, name] : NAMES)
  {
    if ((exceptions & mask) == 0)
      continue;
    if (!result.empty())
      result += ", ";
    result += name;
    remaining &= ~mask;
  }

  if (remaining != 0)
  {
    if (!result.empty())
      result += ", ";
    result += fmt::format("Unknown (0x{:08x})", remaining);
  }
  return result;
}
}  // namespace

DapDebugController::DapDebugController(Core::System& system) : m_system(system)
{
}

void DapDebugController::Continue()
{
  Core::SetState(m_system, Core::State::Running);
}

void DapDebugController::Pause()
{
  Core::SetState(m_system, Core::State::Paused);
}

bool DapDebugController::StepInto()
{
  auto& cpu = m_system.GetCPU();
  // DESNOTE(jbarber, 2026-07-21): When the core isn't stepping (paused
  // already, or never started), SingleStep is a no-op. Return true so the
  // session emits a stopped event — the core IS stopped, after all. The
  // false return is reserved for the async path where StepOpcode timed
  // out without the CPU thread acknowledging: in that case the PC hasn't
  // advanced and a stopped/step event would be a lie.
  if (!cpu.IsStepping())
    return true;

  auto& power_pc = m_system.GetPowerPC();
  if (Core::IsCPUThread())
  {
    Core::CPUThreadGuard guard(m_system);
    const PowerPC::CoreMode old_mode = power_pc.GetMode();
    power_pc.SetMode(PowerPC::CoreMode::Interpreter);
    power_pc.SingleStep();
    power_pc.SetMode(old_mode);
    return true;
  }

  Common::Event sync_event;
  const PowerPC::CoreMode old_mode = power_pc.GetMode();
  power_pc.SetMode(PowerPC::CoreMode::Interpreter);
  cpu.StepOpcode(&sync_event);
  // DESNOTE(jbarber, 2026-07-21): Wait for the CPU thread to ack the step
  // rather than bail after 20ms. The previous 20ms timeout was chosen so
  // the session thread wouldn't block when the CPU was mid-block or under
  // load, returning false and instructing the caller NOT to emit a
  // stopped event. But PollBreakpointStop can't observe the late
  // completion -- it only emits stops on a not-stepping->stepping
  // transition, and after Pause/SyncSteppingBaseline the core is already
  // stepping=true with no further state change. The step's late
  // completion sets sync_event silently and the client hangs waiting for
  // a stop that never arrives (Bugbot: "Step timeout drops stopped
  // event"). Bumping to 2s keeps the session responsive while covering
  // the realistic range of CPU-thread step ack latency; if the ack truly
  // never arrives (deadlock), no stop is emitted and the client will at
  // worst time out itself rather than hang on a phantom in-flight step.
  const bool completed = sync_event.WaitFor(std::chrono::seconds(2));
  power_pc.SetMode(old_mode);
  return completed;
}

StepOverResult DapDebugController::StepOver()
{
  auto& cpu = m_system.GetCPU();
  if (!cpu.IsStepping())
    return StepOverResult::Stepped;

  const UGeckoInstruction inst = [&] {
    Core::CPUThreadGuard guard(m_system);
    return PowerPC::MMU::HostRead_Instruction(guard, m_system.GetPPCState().pc);
  }();

  if (inst.LK)
  {
    auto& breakpoints = m_system.GetPowerPC().GetBreakPoints();
    breakpoints.SetTemporary(m_system.GetPPCState().pc + 4);
    cpu.SetStepping(false);
    return StepOverResult::Continuing;
  }

  // DESNOTE(jbarber, 2026-07-21): Propagate StepInto's failure rather than
  // unconditionally report Stepped. StepInto returns false when the async
  // StepOpcode path timed out without the CPU thread acknowledging -- in
  // that case the PC has not advanced. The session handler mirrors the
  // stepIn guard (suppress the stopped event on false), so a `next` whose
  // underlying single-step didn't complete no longer emits a spurious
  // stopped/"step" event for a PC that didn't move.
  const bool stepped = StepInto();
  return stepped ? StepOverResult::Stepped : StepOverResult::NotStepped;
}

void DapDebugController::StepOut(std::chrono::milliseconds timeout_ms)
{
  auto& cpu = m_system.GetCPU();
  if (!cpu.IsStepping())
    return;

  using clock = std::chrono::steady_clock;
  const clock::time_point timeout = clock::now() + timeout_ms;

  auto& power_pc = m_system.GetPowerPC();
  auto& ppc_state = power_pc.GetPPCState();
  Core::CPUThreadGuard guard(m_system);

  const PowerPC::CoreMode old_mode = power_pc.GetMode();
  power_pc.SetMode(PowerPC::CoreMode::Interpreter);

  UGeckoInstruction inst = PowerPC::MMU::HostRead_Instruction(guard, ppc_state.pc);
  do
  {
    if (WillInstructionReturn(m_system, inst))
    {
      power_pc.SingleStep();
      break;
    }

    if (inst.LK)
    {
      const u32 next_pc = ppc_state.pc + 4;
      do
      {
        power_pc.SingleStep();
      } while (ppc_state.pc != next_pc && clock::now() < timeout && !power_pc.CheckBreakPoints());
    }
    else
    {
      power_pc.SingleStep();
    }

    inst = PowerPC::MMU::HostRead_Instruction(guard, ppc_state.pc);
  } while (clock::now() < timeout && !power_pc.CheckBreakPoints());

  power_pc.SetMode(old_mode);
}

void DapDebugController::ApplyCodeBreakpoints(const std::vector<CodeBreakpointRequest>& breakpoints)
{
  auto& breakpoint_manager = m_system.GetPowerPC().GetBreakPoints();
  breakpoint_manager.Clear();
  for (const CodeBreakpointRequest& request : breakpoints)
  {
    std::optional<Expression> condition;
    if (request.condition && !request.condition->empty())
      condition = Expression::TryParse(*request.condition);

    breakpoint_manager.Add(request.address, true, false, std::move(condition));
  }
}

void DapDebugController::ReapplyCodeBreakpoints()
{
  std::vector<CodeBreakpointRequest> breakpoints;
  for (const auto& [_, source_breakpoints] : m_source_breakpoints)
  {
    breakpoints.insert(breakpoints.end(), source_breakpoints.begin(), source_breakpoints.end());
  }
  breakpoints.insert(breakpoints.end(), m_instruction_breakpoints.begin(),
                     m_instruction_breakpoints.end());
  ApplyCodeBreakpoints(breakpoints);
}

void DapDebugController::SetCodeBreakpoints(std::vector<CodeBreakpointRequest> breakpoints)
{
  m_source_breakpoints.clear();
  m_instruction_breakpoints.clear();
  ApplyCodeBreakpoints(breakpoints);
}

std::optional<u32> DapDebugController::ResolveSourceLineBreakpoint(
    const SourceBreakpointContext& context, const u32 line)
{
  Core::CPUThreadGuard guard(m_system);
  auto& symbol_db = m_system.GetPowerPC().GetSymbolDB();

  if (symbol_db.HasSourceLineInfo())
  {
    if (context.source_reference && *context.source_reference > 0)
    {
      const auto& files = symbol_db.GetSourceFiles();
      if (static_cast<size_t>(*context.source_reference) <= files.size())
      {
        if (const std::optional<u32> address =
                symbol_db.GetLineAddress(files[*context.source_reference - 1], line))
          return address;
      }
    }

    if (context.source_path)
    {
      if (const std::optional<u32> address =
              symbol_db.GetLineAddressForQuery(*context.source_path, line))
        return address;
    }

    if (context.source_name)
    {
      if (const std::optional<u32> address =
              symbol_db.GetLineAddressForQuery(*context.source_name, line))
        return address;
    }
  }

  std::optional<u32> base;
  if (context.source_path)
    base = Json::ParseHexAddress(*context.source_path);
  if (!base && context.source_name)
    base = Json::ParseHexAddress(*context.source_name);
  if (!base)
    return std::nullopt;

  // DESNOTE(jbarber, 2026-07-22): Compute in 64-bit so a wildly-large `line`
  // produces a non-resolvable nullopt rather than silently wrapping past
  // u32 max and installing a breakpoint at a nonsense PC. Mirrors the same
  // guard in ParseSetBreakpoints. Bugbot #63.
  const u64 effective = static_cast<u64>(*base) + static_cast<u64>(line) * 4ull;
  if (effective > static_cast<u64>(std::numeric_limits<u32>::max()))
    return std::nullopt;
  return static_cast<u32>(effective);
}

std::vector<std::optional<u32>> DapDebugController::UpdateSourceBreakpoints(
    const std::string_view source_key, const SourceBreakpointContext& context,
    std::vector<SourceBreakpointSpec> breakpoints)
{
  std::vector<CodeBreakpointRequest> resolved;
  resolved.reserve(breakpoints.size());

  std::vector<std::optional<u32>> addresses;
  addresses.reserve(breakpoints.size());

  for (SourceBreakpointSpec& spec : breakpoints)
  {
    std::optional<u32> address = ResolveSourceLineBreakpoint(context, spec.line);
    addresses.push_back(address);
    if (!address)
      continue;

    CodeBreakpointRequest request;
    request.address = *address;
    request.condition = std::move(spec.condition);
    resolved.push_back(std::move(request));
  }

  if (resolved.empty())
    m_source_breakpoints.erase(std::string(source_key));
  else
    m_source_breakpoints[std::string(source_key)] = std::move(resolved);

  ReapplyCodeBreakpoints();
  return addresses;
}

void DapDebugController::UpdateInstructionBreakpoints(
    std::vector<CodeBreakpointRequest> breakpoints)
{
  m_instruction_breakpoints = std::move(breakpoints);
  ReapplyCodeBreakpoints();
}

void DapDebugController::SetDataBreakpoints(std::vector<DataBreakpointRequest> breakpoints)
{
  // DESNOTE(jbarber, 2026-07-21): Dolphin has a single global memcheck store
  // tied to the one emulated PPC core, so installing this client's list
  // replaces the global set. Concurrent DAP clients on the same core (a rare
  // multi-client setup) will clobber each other's watchpoints here; DAP and
  // GDB are mutually exclusive, and the typical flow is one client per core.
  // DESNOTE(jbarber, 2026-07-26): memchecks.Clear() also wipes freeze memchecks
  // installed by dolphin_freeze. Clear m_freezes so a later RemoveFreeze /
  // dolphin_unfreeze safely returns false instead of calling
  // MemChecks::Remove at a stale address and deleting an unrelated data
  // watchpoint at the same address. The session's m_watch_to_freeze map
  // becomes stale (freeze_ids no longer in m_freezes), but HandleUnfreeze
  // and HandleRealtimeWatchCancel both handle RemoveFreeze returning false
  // gracefully. The RealtimeWatchSampler's field-rate Tick remains as a
  // fallback, so the frozen value is still restored on DMA drift — only the
  // MMU-level CPU write suppression is lost (collateral damage of the global
  // memcheck store being wiped). Bugbot #77.
  auto& memchecks = m_system.GetPowerPC().GetMemChecks();
  {
    Core::CPUThreadGuard guard(m_system);
    memchecks.Clear();
  }
  m_freezes.clear();
  for (const DataBreakpointRequest& request : breakpoints)
  {
    const u32 length = request.length == 0 ? 1 : request.length;
    // Guard against u32 wrap: if address + length overflows, clamp end to
    // u32 max so GetMemCheck still resolves a sane, monotonic range rather
    // than a wrapped (start > end) one that silently never hits.
    const u32 end = (length - 1u > std::numeric_limits<u32>::max() - request.address) ?
                        std::numeric_limits<u32>::max() :
                        request.address + (length - 1u);

    TMemCheck check;
    check.start_address = request.address;
    check.end_address = end;
    // DESNOTE(jbarber, 2026-07-21): Dolphin's TMemCheck distinguishes single-
    // byte vs ranged checks; DAP data breakpoints default to one byte, but a
    // client may pass `length` (a Dolphin extension) to watch a region.
    check.is_ranged = (length > 1);
    check.is_break_on_read = request.read;
    check.is_break_on_write = request.write;
    check.break_on_hit = true;
    check.log_on_hit = false;
    check.is_enabled = true;
    if (request.condition && !request.condition->empty())
      check.condition = Expression::TryParse(*request.condition);
    memchecks.Add(std::move(check));
  }
}

std::optional<std::string> DapDebugController::EvaluateExpression(const std::string_view expression)
{
  const std::optional<Expression> parsed = Expression::TryParse(expression);
  if (!parsed)
    return std::nullopt;

  Core::CPUThreadGuard guard(m_system);
  const double value = parsed->Evaluate(m_system);
  if (value == std::trunc(value) && value >= 0 && value <= 0xffffffff)
    return fmt::format("0x{:08x}", static_cast<u32>(value));

  return fmt::format("{}", value);
}

RegisterSnapshot DapDebugController::GetRegisters()
{
  Core::CPUThreadGuard guard(m_system);
  const auto& ppc_state = m_system.GetPPCState();

  RegisterSnapshot snapshot;
  for (std::size_t i = 0; i < snapshot.gpr.size(); ++i)
    snapshot.gpr[i] = ppc_state.gpr[i];
  snapshot.pc = ppc_state.pc;
  snapshot.lr = LR(ppc_state);
  snapshot.ctr = CTR(ppc_state);
  snapshot.msr = ppc_state.msr.Hex;
  snapshot.cr = ppc_state.cr.Get();
  snapshot.xer = ppc_state.GetXER().Hex;
  return snapshot;
}

std::optional<u32> DapDebugController::SetRegister(const int variables_reference,
                                                   const std::string_view name,
                                                   const std::string_view value_text)
{
  const std::optional<u32> value = Json::ParseRegisterValue(value_text);
  if (!value)
    return std::nullopt;

  Core::CPUThreadGuard guard(m_system);
  auto& ppc_state = m_system.GetPPCState();

  if (variables_reference == REGISTERS_SCOPE)
  {
    if (name.size() < 2 || name[0] != 'r')
      return std::nullopt;

    unsigned index = 0;
    if (!TryParse(std::string(name.substr(1)), &index, 10) || index >= 32)
      return std::nullopt;

    ppc_state.gpr[index] = *value;
    return ppc_state.gpr[index];
  }

  if (variables_reference != PC_SCOPE)
    return std::nullopt;

  if (name == "pc")
  {
    ppc_state.pc = *value;
    return ppc_state.pc;
  }
  if (name == "lr")
  {
    LR(ppc_state) = *value;
    return LR(ppc_state);
  }
  if (name == "ctr")
  {
    CTR(ppc_state) = *value;
    return CTR(ppc_state);
  }
  if (name == "cr")
  {
    ppc_state.cr.Set(*value);
    return ppc_state.cr.Get();
  }
  if (name == "xer")
  {
    UReg_XER xer;
    xer.Hex = *value;
    ppc_state.SetXER(xer);
    return ppc_state.GetXER().Hex;
  }

  return std::nullopt;
}

std::vector<ThreadInfo> DapDebugController::GetThreads()
{
  // Dolphin exposes a single emulated PPC thread to DAP clients. OS-level
  // threads from GetDebugInterface().GetThreads() can be layered on later.
  return {{1, "PPC"}};
}

StackTraceResult DapDebugController::GetStackTrace(const int start_frame, const int levels)
{
  Core::CPUThreadGuard guard(m_system);
  auto& power_pc = m_system.GetPowerPC();
  const auto& ppc_state = power_pc.GetPPCState();
  auto& debug_interface = power_pc.GetDebugInterface();

  std::vector<StackFrame> frames;

  const auto push_frame = [&](const u32 address) {
    StackFrame frame;
    frame.id = static_cast<int>(frames.size());
    frame.address = address;
    std::string description = debug_interface.GetDescription(address);
    if (description.empty() || description == "Invalid")
      description = fmt::format("0x{:08x}", address);
    frame.name = std::move(description);

    const Common::Symbol* symbol = power_pc.GetSymbolDB().GetSymbolFromAddr(address);
    const std::optional<PPCSymbolDB::SourceLine> source_line =
        power_pc.GetSymbolDB().GetSourceLine(address);
    if (source_line)
    {
      frame.source_file = source_line->file;
      frame.source_line = static_cast<int>(source_line->line);
    }
    else if (symbol != nullptr && symbol->type == Common::Symbol::Type::Function)
    {
      frame.source_base = symbol->address;
      frame.source_line = static_cast<int>((address - symbol->address) / 4);
    }
    else
    {
      frame.source_base = address;
      frame.source_line = 0;
    }

    frames.push_back(std::move(frame));
  };

  const auto is_stack_bottom = [&](const u32 addr) {
    return !addr || !PowerPC::MMU::HostIsRAMAddress(guard, addr);
  };

  // DESNOTE(jbarber, 2026-07-21): The innermost frame is the current PC --
  // it's where execution actually stopped. The previous form only pushed the
  // PC when the frame list was otherwise empty, so a typical `stackTrace`
  // response started at LR-4 / stack-walked callers and omitted the
  // instruction the user actually cares about. Walking up first, then
  // prepending the PC, would shuffle addresses past DAP frame ids; instead
  // push the PC first so the rest of the walk appends in declaration order.
  push_frame(ppc_state.pc);

  if (LR(ppc_state) != 0)
    push_frame(LR(ppc_state) - 4);

  if (!is_stack_bottom(ppc_state.gpr[1]))
  {
    u32 addr = PowerPC::MMU::HostRead<u32>(guard, ppc_state.gpr[1]);
    for (int count = 0; !is_stack_bottom(addr) && !is_stack_bottom(addr + 4) && count < 20; ++count)
    {
      const u32 func_addr = PowerPC::MMU::HostRead<u32>(guard, addr + 4);
      push_frame(func_addr - 4);
      addr = PowerPC::MMU::HostRead<u32>(guard, addr);
    }
  }

  StackTraceResult result;
  result.total_frames = static_cast<int>(frames.size());

  const std::size_t begin = static_cast<std::size_t>(std::max(0, start_frame));
  if (begin >= frames.size())
    return result;

  // DESNOTE(jbarber, 2026-07-03): Per the DAP spec `levels` of 0 (or omitted)
  // means "all frames"; a negative value is invalid and treated the same way.
  // See https://microsoft.github.io/debug-adapter-protocol/specification#Requests_StackTrace
  const std::size_t end = levels <= 0 ?
                              frames.size() :
                              std::min(frames.size(), begin + static_cast<std::size_t>(levels));
  frames.erase(frames.begin() + static_cast<std::ptrdiff_t>(end), frames.end());
  frames.erase(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(begin));
  for (std::size_t i = 0; i < frames.size(); ++i)
    frames[i].id = static_cast<int>(begin + i);

  result.frames = std::move(frames);
  return result;
}

std::vector<LoadedSource> DapDebugController::GetLoadedSources()
{
  Core::CPUThreadGuard guard(m_system);
  auto& symbol_db = m_system.GetPowerPC().GetSymbolDB();

  std::vector<LoadedSource> sources;
  if (symbol_db.HasSourceLineInfo())
  {
    const auto& files = symbol_db.GetSourceFiles();
    for (u32 i = 0; i < files.size(); ++i)
    {
      LoadedSource source;
      source.source_reference = static_cast<int>(i + 1);
      source.path = files[i];
      source.name = files[i];
      const size_t slash = source.name.find_last_of("/\\");
      if (slash != std::string::npos)
        source.name = source.name.substr(slash + 1);
      sources.push_back(std::move(source));
    }
    return sources;
  }

  symbol_db.ForEachSymbol([&](const Common::Symbol& symbol) {
    if (symbol.type != Common::Symbol::Type::Function)
      return;

    LoadedSource source;
    source.source_reference = static_cast<int>(symbol.address);
    source.path = Json::FormatAddress(symbol.address);
    source.name = symbol.object_name.empty() ? symbol.name : symbol.object_name;
    if (source.name.empty())
      source.name = source.path;
    sources.push_back(std::move(source));
  });
  return sources;
}

std::optional<SourceContent> DapDebugController::GetSource(const u32 base_address,
                                                           const int start_line, const int end_line)
{
  Core::CPUThreadGuard guard(m_system);
  auto& symbol_db = m_system.GetPowerPC().GetSymbolDB();

  const int first_line = std::max(start_line, 0);
  // DESNOTE(jbarber, 2026-07-21): DAP's `endLine` defaults to -1 when the
  // client means "through end of file/source". The previous form
  // `end_line > first_line ? end_line : first_line + 63` collapsed -1 to
  // at most first_line + 63, silently truncating long files. Treat any
  // negative end_line as "no upper bound" so the read loop runs to EOF or
  // the line_count cap (256) below, whichever comes first. The cap keeps
  // responses bounded; clients paging past it just send another request.
  const int last_line = end_line < 0 ? std::numeric_limits<int>::max() :
                     (end_line > first_line ? end_line : first_line + 63);
  // DESNOTE(jbarber, 2026-07-21): For the disassembly case below we bound
  // the result at 256 lines via `line_count`. The DWARF source-file path
  // above reads lines from disk in a `while (fgets)` loop -- the only
  // bound there is `current_line > last_line`, which is INT_MAX when the
  // client sent no endLine, so a multi-GB source file would stall the
  // session (or OOM the response). Cap the file-read pass at the same
  // 256-line budget so a pathological source can't hang the session.
  // DESNOTE(jbarber, 2026-07-26): Use unsigned arithmetic to avoid signed
  // overflow when last_line is INT_MAX: `INT_MAX - first_line + 1` can
  // overflow if first_line <= 0 (e.g., INT_MAX - 0 + 1 = INT_MAX + 1 → UB).
  // Compute in u64, clamp to kMaxResponseLines, then back to int. Bugbot.
  constexpr int kMaxResponseLines = 256;
  const u64 span = static_cast<u64>(last_line) - static_cast<u64>(first_line) + 1ull;
  const int line_count = static_cast<int>(std::min(span, static_cast<u64>(kMaxResponseLines)));

  if (symbol_db.HasSourceLineInfo() && base_address > 0 &&
      base_address <= symbol_db.GetSourceFiles().size())
  {
    const std::string& path = symbol_db.GetSourceFiles()[base_address - 1];
    File::IOFile file(path, "r");
    if (!file)
      return std::nullopt;

    SourceContent result;
    result.mime_type = "text/x-c";

    char buffer[4096];
    const int source_first_line = std::max(first_line, 1);
    int current_line = 1;
    int emitted_lines = 0;
    while (std::fgets(buffer, sizeof(buffer), file.GetHandle()))
    {
      if (current_line > last_line || emitted_lines >= kMaxResponseLines)
        break;
      if (current_line >= source_first_line)
      {
        if (current_line > source_first_line)
          result.content += '\n';
        std::string_view line(buffer);
        if (!line.empty() && line.back() == '\n')
          line.remove_suffix(1);
        if (!line.empty() && line.back() == '\r')
          line.remove_suffix(1);
        result.content.append(line);
        ++emitted_lines;
      }
      ++current_line;
    }

    // DESNOTE(jbarber, 2026-07-21): Previous form `... && current_line == 0`
    // was unreachable -- current_line starts at 1 and only increments. The
    // real signal is "we went through the file but extracted no lines" (e.g.
    // file shorter than source_first_line), in which case empty content
    // already encodes it.
    if (result.content.empty())
      return std::nullopt;
    return result;
  }

  if (!PowerPC::MMU::HostIsRAMAddress(guard, base_address))
    return std::nullopt;

  SourceContent result;
  result.mime_type = "text/x-disassembly";
  auto& debug_interface = m_system.GetPowerPC().GetDebugInterface();

  for (int i = 0; i < line_count; ++i)
  {
    const u32 addr = base_address + static_cast<u32>((first_line + i) * 4);
    if (!PowerPC::MMU::HostIsRAMAddress(guard, addr))
      break;
    if (i > 0)
      result.content += '\n';
    result.content += fmt::format("{:08x}: {}", addr, debug_interface.Disassemble(&guard, addr));
  }

  if (result.content.empty())
    return std::nullopt;
  return result;
}

std::vector<BreakpointLocation> DapDebugController::GetBreakpointLocations(const u32 base_address,
                                                                           const int start_line,
                                                                           const int end_line)
{
  Core::CPUThreadGuard guard(m_system);
  std::vector<BreakpointLocation> locations;

  const int first_line = std::max(start_line, 0);
  // DESNOTE(jbarber, 2026-07-21): DAP's `endLine` defaults to -1 meaning
  // "through end". Treat negative end_line as unbounded so we enumerate
  // every instruction slot / line entry in the range rather than stopping
  // at first_line + 63. Cap the iteration at 65536 lines regardless so a
  // pathological end_line (or an omitted one defaulting to INT_MAX)
  // doesn't spin the session thread for billions of GetLineAddress calls.
  constexpr int kMaxLocationEnumerations = 65536;
  const int last_line = end_line < 0 ? std::numeric_limits<int>::max() :
                     (end_line >= first_line ? end_line : first_line + 63);
  const int capped_last = std::min(last_line, first_line + kMaxLocationEnumerations - 1);

  auto& symbol_db = m_system.GetPowerPC().GetSymbolDB();
  if (symbol_db.HasSourceLineInfo() && base_address > 0 &&
      base_address <= symbol_db.GetSourceFiles().size())
  {
    const std::string& file = symbol_db.GetSourceFiles()[base_address - 1];
    for (int line = first_line; line <= capped_last; ++line)
    {
      if (symbol_db.GetLineAddress(file, static_cast<u32>(line)))
        locations.push_back({line});
    }
    return locations;
  }

  for (int line = first_line; line <= capped_last; ++line)
  {
    const u32 addr = base_address + static_cast<u32>(line * 4);
    if (!PowerPC::MMU::HostIsRAMAddress(guard, addr))
      break;
    locations.push_back({line});
  }
  return locations;
}

void DapDebugController::Restart()
{
  Core::CPUThreadGuard guard(m_system);
  m_system.GetPowerPC().Reset();
  // DESNOTE(jbarber, 2026-07-21): PowerPC::Reset only clears PPC/MMU/cache
  // state; it doesn't touch the CPU run state. If the client had continued
  // execution (stopOnEntry:false or after `continue`), the core stays
  // Running and the post-restart `stopped` event the session emits would
  // lie -- the client would believe emulation halted while the CPU kept
  // running. Force Break + State::Paused here so the post-restart stop is
  // truthful, mirroring what Terminate does.
  m_system.GetCPU().Break();
  Core::SetState(m_system, Core::State::Paused);
}

void DapDebugController::Terminate()
{
  m_system.GetCPU().Break();
  Core::SetState(m_system, Core::State::Paused);
}

void DapDebugController::ClearBreakpoints()
{
  // DESNOTE(jbarber, 2026-07-22): Dolphin has one shared global breakpoint /
  // memcheck store tied to the PPC core. This session installed its
  // breakpoints/watchpoints there via ApplyCodeBreakpoints (which clears the
  // global store first) and SetDataBreakpoints (same for memchecks). Without
  // this teardown call, a disconnecting client would leave the core halting
  // on stale debugger state that no DAP client is around to clear. Wipe the
  // session's tracked sets AND the global stores so follow-on emulation runs
  // clean. Mirrors what ApplyCodeBreakpoints/SetDataBreakpoints do at the
  // start of each set -- here we're doing the empty-set version.
  m_source_breakpoints.clear();
  m_instruction_breakpoints.clear();
  {
    Core::CPUThreadGuard guard(m_system);
    m_system.GetPowerPC().GetBreakPoints().Clear();
    m_system.GetPowerPC().GetMemChecks().Clear();
  }
  // Also clear any freezes this controller installed. ClearFreezes itself
  // takes the guard and removes memchecks, so call it after the above guard
  // scope ends to avoid nested guards (ClearFreezes opens its own).
  // Actually -- ClearBreakpoints just wiped ALL memchecks via .Clear(), so
  // the freeze memchecks are already gone from the global store. Just clear
  // our tracking vector so RemoveFreeze/ClearFreezes don't try to remove
  // already-gone entries. Bugbot-resistent: idempotent.
  m_freezes.clear();
}

u32 DapDebugController::InstallFreeze(u32 address, u32 count, std::span<const u8> value)
{
  // DESNOTE(jbarber, 2026-07-22): Installs a `is_freeze` TMemCheck on
  // [address, address+count) so MMU::Write<T> suppresses CPU stores to that
  // range. The frozen `value` is written to RAM immediately (via HostWrite,
  // which bypasses the freeze memcheck — so the freeze back-write itself
  // isn't suppressed). The RealtimeWatchSampler's field-rate Tick remains as
  // a fallback for DMA/peripheral writes that bypass MMU::Write.
  if (count == 0)
    return 0;
  if (count - 1u > std::numeric_limits<u32>::max() - address)
    return 0;
  if (value.size() < count)
    return 0;

  const u32 freeze_id = m_next_freeze_id++;
  m_freezes.push_back({freeze_id, address, count});

  {
    Core::CPUThreadGuard guard(m_system);
    auto& memchecks = m_system.GetPowerPC().GetMemChecks();
    TMemCheck mc;
    mc.start_address = address;
    mc.end_address = address + count - 1;
    mc.is_ranged = true;
    mc.is_enabled = true;
    mc.is_freeze = true;
    // No break/log — freeze memchecks suppress silently.
    memchecks.Add(std::move(mc));

    // Write the frozen value into RAM so reads return the frozen bytes.
    // HostWrite bypasses Memcheck (and thus the freeze suppression), so
    // this write always lands.
    for (u32 i = 0; i < count; ++i)
      PowerPC::MMU::HostWrite<u8>(guard, value[i], address + i);
  }

  // Invalidate iCache/JIT for the range in case it's code.
  InvalidateCodeRange(address, count);

  return freeze_id;
}

bool DapDebugController::RemoveFreeze(u32 freeze_id)
{
  auto it = std::find_if(m_freezes.begin(), m_freezes.end(),
                         [freeze_id](const FreezeEntry& e) {
                           return e.freeze_id == freeze_id;
                         });
  if (it == m_freezes.end())
    return false;
  {
    Core::CPUThreadGuard guard(m_system);
    auto& memchecks = m_system.GetPowerPC().GetMemChecks();
    // Remove the freeze memcheck matching this address range. MemChecks
    // doesn't have a RemoveById, so we remove by address.
    memchecks.Remove(it->address);
  }
  m_freezes.erase(it);
  return true;
}

void DapDebugController::ClearFreezes()
{
  if (m_freezes.empty())
    return;
  {
    Core::CPUThreadGuard guard(m_system);
    auto& memchecks = m_system.GetPowerPC().GetMemChecks();
    for (const FreezeEntry& e : m_freezes)
      memchecks.Remove(e.address);
  }
  m_freezes.clear();
}

std::vector<u8> DapDebugController::ReadMemory(u32 address, std::size_t size)
{
  // DESNOTE(jbarber, 2026-07-21): Reject ranges that wrap past u32 max so a
  // huge `size` paired with a high `address` can't silently advance past
  // 0xFFFFFFFF and onto unrelated low RAM. The previous form's
  // `address + static_cast<u32>(i)` arithmetic wrapped in u32, and the
  // IsValidAddress check could succeed for the wrapped address (low RAM is
  // typically valid), reading bytes from the wrong region. Returning an
  // empty vector surfacing the failure as "couldn't read anything" matches
  // dolphin_realtimeWatch and ParseFreeze's existing overflow protection.
  //
  // DESNOTE(jbarber, 2026-07-21): Off-by-one fix. The rejected condition
  // must be (last_byte > UINT32_MAX), i.e. (size - 1) > (UINT32_MAX -
  // address). The previous `address > UINT32_MAX - size` form rejected
  // when address == UINT32_MAX - size + 1 even though the last byte
  // (address + size - 1) is still UINT32_MAX and the read is valid -- a
  // top-of-MEM1 read of exactly 1 byte at 0xFFFFFFFF was incorrectly
  // rejected.
  if (size > static_cast<std::size_t>(std::numeric_limits<u32>::max()) ||
      (size > 0 &&
       static_cast<u32>(size - 1) > std::numeric_limits<u32>::max() - address))
  {
    return {};
  }
  Core::CPUThreadGuard guard(m_system);
  AddressSpace::Accessors* accessors = AddressSpace::GetAccessors(AddressSpace::Type::Effective);

  std::vector<u8> bytes;
  bytes.reserve(size);
  for (std::size_t i = 0; i < size; ++i)
  {
    const u32 addr = address + static_cast<u32>(i);
    if (!accessors->IsValidAddress(guard, addr))
      break;
    bytes.push_back(accessors->ReadU8(guard, addr));
  }
  return bytes;
}

std::size_t DapDebugController::WriteMemory(u32 address, std::span<const u8> data)
{
  // DESNOTE(jbarber, 2026-07-21): Same overflow guard as ReadMemory above --
  // a huge user-supplied `data` paired with a high base address mustn't wrap
  // and overwrite low RAM. Returning 0 here surfaces the failure to the
  // caller, including Detour's staged-write rollback. Off-by-one fixed to
  // use (size - 1) > (UINT32_MAX - address) so a 1-byte write at 0xFFFFFFFF
  // is accepted (last byte is exactly UINT32_MAX).
  if (data.size() > static_cast<std::size_t>(std::numeric_limits<u32>::max()) ||
      (!data.empty() &&
       static_cast<u32>(data.size() - 1) > std::numeric_limits<u32>::max() - address))
  {
    return 0;
  }
  Core::CPUThreadGuard guard(m_system);
  AddressSpace::Accessors* accessors = AddressSpace::GetAccessors(AddressSpace::Type::Effective);

  std::size_t written = 0;
  for (const u8 byte : data)
  {
    const u32 addr = address + static_cast<u32>(written);
    if (!accessors->IsValidAddress(guard, addr))
      break;
    accessors->WriteU8(guard, addr, byte);
    ++written;
  }
  // DESNOTE(jbarber, 2026-07-21): MMU::WriteToHardware copies bytes into RAM
  // with no icbi/JIT hook, so writes that patch code leave the L1 iCache and
  // JIT block cache pointing at stale bytes. Invalidate every cacheline we
  // touched so interpreter and JIT fetch paths both see the new bytes the
  // next time control reaches this address. Without this, an injected
  // detour or code patch can be silently ignored.
  InvalidateCodeRange(address, written);
  return written;
}

void DapDebugController::InvalidateCodeRange(u32 address, std::size_t length)
{
  if (length == 0)
    return;
  // DESNOTE(jbarber, 2026-07-21): Bounds-check the byte range against u32
  // max before any arithmetic, mirroring WriteMemory / ReadMemory. The
  // previous form computed `end_addr = address + (u32)length` which silently
  // wrapped on a high address paired with a large length; the wrapped
  // end_addr produced an end_line below start_line and the invalidation
  // loop skipped -- JIT/iCache kept stale instructions after writeMemory,
  // inject, or detour near the top of the 32-bit space. Bail out (caller is
  // told the write succeeded but we can't safely flush the iCache for the
  // tail; rejecting the whole write at this layer would be more disruptive
  // than the wrap, and WriteMemory already validates the wrap for the
  // actual byte writes themselves).
  // DESNOTE(jbarber, 2026-07-22): Use last_byte (inclusive) and iterate
  // cachelines with `!=` so a 1-byte invalidate at 0xFFFFFFFF works: the
  // previous `(end_addr + 31) & ~31` form computed `end_line = 0` when
  // end_addr wrapped past u32 max, producing `start_line (0xFFFFFFE0) <
  // end_line (0)` == false and skipping the loop entirely. The inclusive
  // form: start_line, then increment-by-32 until we hit end_line_inclusive,
  // then one final invalidate on end_line_inclusive. Loop never wraps
  // because we already verified the range fits in u32. Bugbot #66.
  const u64 last_byte_64 = static_cast<u64>(address) + static_cast<u64>(length) - 1;
  if (last_byte_64 > static_cast<u64>(std::numeric_limits<u32>::max()))
    return;
  const u32 last_byte = static_cast<u32>(last_byte_64);
  Core::CPUThreadGuard guard(m_system);
  auto& ppc_state = m_system.GetPPCState();
  auto& memory = m_system.GetMemory();
  auto& jit_interface = m_system.GetJitInterface();
  // PPC L1 iCache lines are 32 bytes (CACHE_BLOCK_SIZE * 4). Round the range
  // out to the cacheline boundaries it overlaps and walk every line through
  // the icbi path -- same loop GeckoCode.cpp uses to flush its installer.
  const u32 start_line = address & ~u32{31u};
  const u32 end_line_inclusive = last_byte & ~u32{31u};
  for (u32 line = start_line; line != end_line_inclusive; line += 32)
    ppc_state.iCache.Invalidate(memory, jit_interface, line);
  // Final iteration for the last cacheline. Always invoked (handles the
  // start_line == end_line_inclusive single-line case as well as the
  // post-loop case).
  ppc_state.iCache.Invalidate(memory, jit_interface, end_line_inclusive);
}

std::optional<u32> DapDebugController::FindFreeMemory(u32 count)
{
  if (count == 0)
    return std::nullopt;
  Core::CPUThreadGuard guard(m_system);
  auto& memory = m_system.GetMemory();
  // DESNOTE(jbarber, 2026-07-21): Use GetRamSizeReal(), not GetRamSize().
  // GetRamSize() returns the power-of-two-padded size of the MEM1 arena
  // (typically 24 MiB retail rounded up to 32 MiB); the padded tail beyond
  // GetRamSizeReal() isn't backed by the GC's actual RAM and writing
  // there corrupts nothing but isn't usable code-cave space either. Auto-
  // allocation for dolphin_injectCode / dolphin_detour previously scanned
  // into that padded tail and could return addresses in non-real RAM.
  const u32 ram_size = memory.GetRamSizeReal();
  if (ram_size == 0)
    return std::nullopt;
  const u8* ram = memory.GetRAM();
  const u32 ram_words = ram_size / 4u;
  // Scan RAM word-by-word for runs of zero words. We want the smallest run
  // that satisfies `count` (rounding up to a word) and starts at a
  // 4-byte-aligned offset, mirroring what a real allocator would return
  // for a code-cave request. Iterating by word keeps the start address
  // naturally 4-byte-aligned.
  const u32 need_words = (count + 3u) / 4u;
  std::optional<u32> best_addr;
  std::optional<u32> best_bytes;
  u32 run_start = 0;
  bool in_run = false;
  for (u32 w = 0; w < ram_words; ++w)
  {
    u32 word = 0;
    std::memcpy(&word, ram + w * 4, 4);
    if (word == 0)
    {
      if (!in_run)
      {
        run_start = w;
        in_run = true;
      }
    }
    else if (in_run)
    {
      const u32 run_bytes = (w - run_start) * 4;
      if (run_bytes >= count && (!best_bytes || run_bytes < *best_bytes))
      {
        best_bytes = run_bytes;
        best_addr = 0x80000000u + run_start * 4;
      }
      in_run = false;
    }
  }
  if (in_run)
  {
    const u32 run_bytes = (ram_words - run_start) * 4;
    if (run_bytes >= count && (!best_bytes || run_bytes < *best_bytes))
      best_addr = 0x80000000u + run_start * 4;
  }
  return best_addr;
}

u32 DapDebugController::InjectCode(std::optional<u32> address, std::vector<u8> code)
{
  if (code.empty() || code.size() % 4u != 0u)
    return 0;
  u32 target = address.value_or(0);
  if (!address)
  {
    auto alloc = FindFreeMemory(static_cast<u32>(code.size()));
    if (!alloc)
      return 0;
    target = *alloc;
  }
  const std::size_t written = WriteMemory(target, std::span<const u8>{code});
  if (written != code.size())
    return 0;
  return target;
}

std::optional<DapDebugController::DetourResult>
DapDebugController::Detour(u32 target_address, std::optional<u32> detour_address,
                           std::vector<u8> detour_body)
{
  if (detour_body.empty() || detour_body.size() % 4u != 0u)
    return std::nullopt;
  // Snapshot the original instruction we're about to patch out. The
  // detour is transparent: the trampoline replays this instruction and
  // then branches back to `target + 4`.
  const std::vector<u8> original_target_bytes = ReadMemory(target_address, 4);
  if (original_target_bytes.size() != 4u)
    return std::nullopt;  // Target address couldn't be read.

  // Region layout: [detour body][b trampoline][trampoline: original + b target+4]
  const u32 body_size = static_cast<u32>(detour_body.size());
  // DESNOTE(jbarber, 2026-07-22): HandleDetour now pre-allocates the cave
  // and passes a non-nullopt detour_address through; the internal
  // FindFreeMemory fallback here only fires for in-process callers (tests)
  // that pass nullopt. Bugbot #68.
  u32 detour_addr = detour_address.value_or(0);
  const u32 detour_size = body_size + 12u;  // body + tail branch + 8-byte trampoline
  if (!detour_address)
  {
    auto alloc = FindFreeMemory(detour_size);
    if (!alloc)
      return std::nullopt;
    detour_addr = *alloc;
  }
  else
  {
    // DESNOTE(jbarber, 2026-07-22): Validate the client-supplied detour cave
    // is large enough for the full detour layout (body + tail branch + 8-byte
    // trampoline). Without this check, Detour silently writes body+12 bytes
    // from `detour_addr` -- a too-short cave overwrites adjacent guest
    // memory the client didn't intend to release. We can't peek at the whole
    // range without a CPUThreadGuard, so we use AddressSpace::IsValidAddress
    // for the last byte (which itself takes the guard internally if it
    // needs to). We check the end byte rather than per-byte iteration to
    // keep this O(1); a high address plus a large body that wraps in u32
    // also fails here (end_addr < detour_addr). Bugbot #71.
    const u64 end_byte_64 = static_cast<u64>(detour_addr) + static_cast<u64>(detour_size) - 1ull;
    if (end_byte_64 > static_cast<u64>(std::numeric_limits<u32>::max()))
      return std::nullopt;
    const u32 end_byte = static_cast<u32>(end_byte_64);
    AddressSpace::Accessors* accessors =
        AddressSpace::GetAccessors(AddressSpace::Type::Effective);
    {
      Core::CPUThreadGuard guard(m_system);
      if (!accessors->IsValidAddress(guard, detour_addr) ||
          !accessors->IsValidAddress(guard, end_byte))
        return std::nullopt;
    }
  }
  const u32 tail_branch_addr = detour_addr + body_size;
  const u32 trampoline_addr = tail_branch_addr + 4u;
  // Build branches:
  //   - tail_branch: at end of body, branches to the trampoline.
  //   - detour_branch: replaces the target, branches to the detour.
  //   - trampoline_return: after the replayed original, branches back to
  //     `target + 4` to resume execution after the patch site.
  const auto tail_branch_opt = MakeBranchInstruction(tail_branch_addr, trampoline_addr);
  const auto detour_branch_opt = MakeBranchInstruction(target_address, detour_addr);
  const auto trampoline_return_opt =
      MakeBranchInstruction(trampoline_addr + 4u, target_address + 4u);
  // DESNOTE(jbarber, 2026-07-21): A detour layout that places the patch site
  // and its targets > ±32 MiB apart can't be encoded as a relative `b`
  // instruction -- the PPC `b` LI field is 24-bit signed. Reject the detour
  // outright instead of silently encoding a truncated branch that would
  // jump to a nonsense PC at runtime.
  if (!tail_branch_opt || !detour_branch_opt || !trampoline_return_opt)
    return std::nullopt;
  const u32 tail_branch = *tail_branch_opt;
  const u32 detour_branch = *detour_branch_opt;
  const u32 trampoline_return = *trampoline_return_opt;

  // DESNOTE(jbarber, 2026-07-21): Each (address, snapshot) pair below records
  // the pre-detour bytes we're about to overwrite. If any WriteMemory call
  // fails partway through the chain, we walk the `written` list in reverse
  // and restore each region from its snapshot so the caller is returned to
  // the pre-detour byte layout (no partially-patched target/trampoline
  // left behind). The `written` vector grows as writes succeed, so a
  // failure on the Nth write restores only the first N-1 regions.
  struct PendingWrite
  {
    u32 address;
    std::vector<u8> snapshot;
  };
  std::vector<PendingWrite> written;
  const auto restore = [&] {
    for (auto it = written.rbegin(); it != written.rend(); ++it)
      (void)WriteMemory(it->address, std::span<const u8>{it->snapshot});
  };
  const auto stage_or_fail = [&](u32 address, const std::vector<u8>& snapshot,
                                  std::span<const u8> new_bytes) -> bool {
    if (snapshot.size() != new_bytes.size() ||
        WriteMemory(address, new_bytes) != new_bytes.size())
    {
      restore();
      return false;
    }
    written.push_back({address, snapshot});
    return true;
  };

  // Order matters for live emulation: install the detour body and
  // trampoline FIRST, then patch the target. A CPU that hits the target
  // before the trampoline is in place would `b detour` -> fall through to
  // garbage after body. Even though tests don't run the CPU, this is the
  // correct ordering for production.
  if (!stage_or_fail(detour_addr, ReadMemory(detour_addr, detour_body.size()), detour_body))
    return std::nullopt;
  const auto tail_bytes = BigEndianBytes(tail_branch);
  if (!stage_or_fail(tail_branch_addr, ReadMemory(tail_branch_addr, 4), tail_bytes))
    return std::nullopt;
  if (!stage_or_fail(trampoline_addr, ReadMemory(trampoline_addr, 4), original_target_bytes))
    return std::nullopt;
  const auto return_bytes = BigEndianBytes(trampoline_return);
  if (!stage_or_fail(trampoline_addr + 4u, ReadMemory(trampoline_addr + 4u, 4), return_bytes))
    return std::nullopt;
  // Finally patch the target with `b detour_addr`.
  const auto patch_bytes = BigEndianBytes(detour_branch);
  if (!stage_or_fail(target_address, ReadMemory(target_address, 4), patch_bytes))
    return std::nullopt;

  DetourResult result;
  result.target_address = target_address;
  result.detour_address = detour_addr;
  result.trampoline_address = trampoline_addr;
  result.original_instruction = original_target_bytes;
  return result;
}

std::string DapDebugController::Disassemble(u32 address, int instruction_count)
{
  Core::CPUThreadGuard guard(m_system);
  auto& debug_interface = m_system.GetPowerPC().GetDebugInterface();

  std::string result;
  for (int i = 0; i < instruction_count; ++i)
  {
    // DESNOTE(jbarber, 2026-07-21): Compute the byte offset in u64 and
    // bounds-check against u32 max before the cast. The previous form
    // did `address + static_cast<u32>(i * 4)` which silently wrapped on
    // a high base address paired with a large instructionCount (up to
    // 65536 from the protocol parser cap), so a request near the top of
    // MEM1 wrapped around to disassemble low RAM instead of failing
    // safely. Bail out when the offset would exceed u32 max rather than
    // produce nonsense output for the wrapped tail.
    const u64 offset = static_cast<u64>(i) * 4u;
    if (offset > static_cast<u64>(std::numeric_limits<u32>::max()) - address)
      break;
    const u32 addr = address + static_cast<u32>(offset);
    if (i > 0)
      result.push_back('\n');
    result += debug_interface.Disassemble(&guard, addr);
  }
  return result;
}

u32 DapDebugController::GetPC()
{
  Core::CPUThreadGuard guard(m_system);
  return m_system.GetPPCState().pc;
}

void DapDebugController::SetPC(const u32 address)
{
  Core::CPUThreadGuard guard(m_system);
  m_system.GetPPCState().pc = address;
  m_system.GetPPCState().npc = address;
}

StopInfo DapDebugController::GetStopInfo()
{
  StopInfo info;

  Core::CPUThreadGuard guard(m_system);
  const auto& ppc_state = m_system.GetPPCState();
  const u32 pc = ppc_state.pc;

  // DESNOTE(jbarber, 2026-07-03): A watchpoint fires on the instruction that
  // accessed the watched data (MMU.cpp tags it EXCEPTION_DSI |
  // EXCEPTION_FAKE_MEMCHECK_HIT). The PC is that instruction, not the watched
  // data address, so this cannot be detected by matching the PC against a
  // memcheck range. The flag is transient (PowerPC::CheckExceptions clears it),
  // so this is best-effort: if it has already been cleared we fall back to
  // Step rather than mis-reporting, which is safe for the DAP `stopped` reason.
  if ((ppc_state.Exceptions & EXCEPTION_FAKE_MEMCHECK_HIT) != 0)
  {
    info.reason = StopReason::DataBreakpoint;
    return info;
  }

  if (m_system.GetPowerPC().GetBreakPoints().GetBreakpoint(pc) != nullptr)
  {
    info.reason = StopReason::CodeBreakpoint;
    info.hit_breakpoint_address = pc;
    return info;
  }

  info.reason = StopReason::Step;
  return info;
}

std::optional<ExceptionInfo> DapDebugController::GetExceptionInfo()
{
  Core::CPUThreadGuard guard(m_system);
  u32 exceptions = m_system.GetPPCState().Exceptions;

  // DESNOTE(jbarber, 2026-07-03): A memory watchpoint hit is delivered as a
  // synthetic DSI tagged with EXCEPTION_FAKE_MEMCHECK_HIT (see MMU.cpp). That is
  // the debugger's own break mechanism, not a guest exception, so it is surfaced
  // as a "data breakpoint" stop and excluded from exceptionInfo here.
  if ((exceptions & EXCEPTION_FAKE_MEMCHECK_HIT) != 0)
    exceptions &= ~(EXCEPTION_FAKE_MEMCHECK_HIT | EXCEPTION_DSI);

  if (exceptions == 0)
    return std::nullopt;

  ExceptionInfo info;
  info.exceptions = exceptions;
  info.description = DescribeExceptions(exceptions);
  return info;
}
}  // namespace DAP
