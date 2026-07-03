// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapDebugController.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "Common/Event.h"
#include "Common/StringUtil.h"
#include "Core/Core.h"
#include "Core/Debugger/DAP/DapJson.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/Expression.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace DAP
{
namespace
{
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

void DapDebugController::StepInto()
{
  auto& cpu = m_system.GetCPU();
  if (!cpu.IsStepping())
    return;

  auto& power_pc = m_system.GetPowerPC();
  if (Core::IsCPUThread())
  {
    Core::CPUThreadGuard guard(m_system);
    const PowerPC::CoreMode old_mode = power_pc.GetMode();
    power_pc.SetMode(PowerPC::CoreMode::Interpreter);
    power_pc.SingleStep();
    power_pc.SetMode(old_mode);
    return;
  }

  Common::Event sync_event;
  const PowerPC::CoreMode old_mode = power_pc.GetMode();
  power_pc.SetMode(PowerPC::CoreMode::Interpreter);
  cpu.StepOpcode(&sync_event);
  sync_event.WaitFor(std::chrono::milliseconds(20));
  power_pc.SetMode(old_mode);
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

  StepInto();
  return StepOverResult::Stepped;
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

void DapDebugController::SetCodeBreakpoints(std::vector<CodeBreakpointRequest> breakpoints)
{
  auto& breakpoint_manager = m_system.GetPowerPC().GetBreakPoints();
  breakpoint_manager.Clear();
  for (CodeBreakpointRequest& request : breakpoints)
  {
    std::optional<Expression> condition;
    if (request.condition && !request.condition->empty())
      condition = Expression::TryParse(*request.condition);

    breakpoint_manager.Add(request.address, true, false, std::move(condition));
  }
}

void DapDebugController::SetDataBreakpoints(std::vector<DataBreakpointRequest> breakpoints)
{
  auto& memchecks = m_system.GetPowerPC().GetMemChecks();
  memchecks.Clear();
  for (const DataBreakpointRequest& request : breakpoints)
  {
    TMemCheck check;
    check.start_address = request.address;
    check.end_address = request.address;
    check.is_ranged = false;
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
    frames.push_back(std::move(frame));
  };

  const auto is_stack_bottom = [&](const u32 addr) {
    return !addr || !PowerPC::MMU::HostIsRAMAddress(guard, addr);
  };

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

  if (frames.empty())
    push_frame(ppc_state.pc);

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

std::vector<u8> DapDebugController::ReadMemory(u32 address, std::size_t size)
{
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
  return written;
}

std::string DapDebugController::Disassemble(u32 address, int instruction_count)
{
  Core::CPUThreadGuard guard(m_system);
  auto& debug_interface = m_system.GetPowerPC().GetDebugInterface();

  std::string result;
  for (int i = 0; i < instruction_count; ++i)
  {
    const u32 addr = address + static_cast<u32>(i * 4);
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
