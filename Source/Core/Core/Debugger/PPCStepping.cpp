// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/PPCStepping.h"

#include <optional>

#include "Common/Event.h"
#include "Common/ScopeGuard.h"
#include "Core/Core.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace Core::Debug
{
namespace
{
bool IsCancelled(const PPCStepOptions& options)
{
  return options.cancelled != nullptr && options.cancelled->load();
}

bool WillInstructionReturn(Core::System& system, const UGeckoInstruction instruction)
{
  if (instruction.hex == 0x4C000064u)
    return true;

  const auto& state = system.GetPPCState();
  const bool counter =
      (instruction.BO_2 >> 2 & 1) != 0 || (CTR(state) != 0) != ((instruction.BO_2 >> 1 & 1) != 0);
  const bool condition = instruction.BO_2 >> 4 != 0 ||
                         state.cr.GetBit(instruction.BI_2) == (instruction.BO_2 >> 3 & 1);
  const bool is_bclr = instruction.OPCD_7 == 0b010011 && instruction.XO == 16;
  return is_bclr && counter && condition && !instruction.LK_3;
}

PPCStepResult StepInstructionInto(Core::System& system, const PPCStepOptions& options)
{
  auto& cpu = system.GetCPU();
  if (!cpu.IsStepping())
    return PPCStepResult::Stepped;

  auto& power_pc = system.GetPowerPC();
  if (Core::IsCPUThread())
  {
    Core::CPUThreadGuard guard(system);
    const PowerPC::CoreMode old_mode = power_pc.GetMode();
    power_pc.SetMode(PowerPC::CoreMode::Interpreter);
    power_pc.SingleStep();
    power_pc.SetMode(old_mode);
    return PPCStepResult::Stepped;
  }

  Common::Event sync_event;
  const PowerPC::CoreMode old_mode = power_pc.GetMode();
  power_pc.SetMode(PowerPC::CoreMode::Interpreter);
  cpu.StepOpcode(&sync_event);
  const bool completed = sync_event.WaitFor(options.instruction_timeout);
  power_pc.SetMode(old_mode);
  return completed ? PPCStepResult::Stepped : PPCStepResult::NotStepped;
}

PPCStepResult StepInstructionOver(Core::System& system, const PPCStepOptions& options)
{
  auto& cpu = system.GetCPU();
  if (!cpu.IsStepping())
    return PPCStepResult::Stepped;

  const UGeckoInstruction instruction = [&] {
    Core::CPUThreadGuard guard(system);
    return PowerPC::MMU::HostRead_Instruction(guard, system.GetPPCState().pc);
  }();

  if (!instruction.LK)
    return StepInstructionInto(system, options);

  auto& breakpoints = system.GetPowerPC().GetBreakPoints();
  breakpoints.SetTemporary(system.GetPPCState().pc + 4);
  cpu.SetStepping(false);
  return PPCStepResult::Continuing;
}

PPCStepResult StepSourceRow(Core::System& system, const PPCStepMode mode,
                            const PPCStepOptions& options)
{
  auto& cpu = system.GetCPU();
  if (!cpu.IsStepping() || options.instruction_cap == 0)
    return PPCStepResult::Stepped;

  using clock = std::chrono::steady_clock;
  const clock::time_point deadline = clock::now() + options.timeout;
  auto& power_pc = system.GetPowerPC();
  auto& state = system.GetPPCState();
  const std::optional<PPCSymbolDB::SourceLine> start_line =
      system.GetPPCSymbolDB().GetSourceLine(state.pc);
  Core::CPUThreadGuard guard(system);
  const PowerPC::CoreMode old_mode = power_pc.GetMode();
  power_pc.SetMode(PowerPC::CoreMode::Interpreter);
  const bool resume_watchpoint = (state.Exceptions & EXCEPTION_FAKE_MEMCHECK_HIT) != 0;
  power_pc.ClearSteppingMemcheckHit();
  power_pc.SetSteppingMemchecksEnabled(!resume_watchpoint);
  Common::ScopeGuard restore_mode{[&] {
    power_pc.SetSteppingMemchecksEnabled(false);
    power_pc.SetMode(old_mode);
  }};

  std::size_t instruction_count = 0;
  bool hit_breakpoint = false;
  const auto can_continue = [&] {
    return !IsCancelled(options) && instruction_count < options.instruction_cap &&
           clock::now() < deadline && !hit_breakpoint;
  };
  const auto step_one = [&] {
    power_pc.SingleStep();
    power_pc.SetSteppingMemchecksEnabled(true);
    ++instruction_count;
    hit_breakpoint = power_pc.DidSteppingMemcheckHit();
    if (!hit_breakpoint && power_pc.CheckBreakPoints())
    {
      hit_breakpoint = true;
      const u32 pc = state.pc;
      system.GetCPU().Break(
          {.cause = ExecutionStopCause::CodeBreakpoint, .pc = pc, .code_breakpoint_address = pc});
    }
  };
  const auto step_logical = [&] {
    const UGeckoInstruction instruction = PowerPC::MMU::HostRead_Instruction(guard, state.pc);
    if (mode != PPCStepMode::Over || !instruction.LK)
    {
      step_one();
      return;
    }

    const u32 return_pc = state.pc + 4;
    do
    {
      step_one();
    } while (can_continue() && state.pc != return_pc);
  };

  if (can_continue())
    step_logical();
  while (start_line && can_continue())
  {
    const std::optional<PPCSymbolDB::SourceLine> current_line =
        system.GetPPCSymbolDB().GetSourceLine(state.pc);
    if (current_line && (current_line->file_index != start_line->file_index ||
                         current_line->line != start_line->line))
    {
      break;
    }
    step_logical();
  }
  return PPCStepResult::Stepped;
}

PPCStepResult StepOut(Core::System& system, const PPCStepOptions& options)
{
  auto& cpu = system.GetCPU();
  if (!cpu.IsStepping())
    return PPCStepResult::NotStepped;

  using clock = std::chrono::steady_clock;
  const clock::time_point deadline = clock::now() + options.timeout;
  auto& power_pc = system.GetPowerPC();
  auto& state = power_pc.GetPPCState();
  Core::CPUThreadGuard guard(system);
  const PowerPC::CoreMode old_mode = power_pc.GetMode();
  power_pc.SetMode(PowerPC::CoreMode::Interpreter);
  const bool resume_watchpoint = (state.Exceptions & EXCEPTION_FAKE_MEMCHECK_HIT) != 0;
  power_pc.ClearSteppingMemcheckHit();
  power_pc.SetSteppingMemchecksEnabled(!resume_watchpoint);
  Common::ScopeGuard restore_mode{[&] {
    power_pc.SetSteppingMemchecksEnabled(false);
    power_pc.SetMode(old_mode);
  }};

  bool stepped = false;
  bool interrupted = false;
  const auto can_continue = [&] {
    if (IsCancelled(options) || clock::now() >= deadline)
    {
      interrupted = true;
      return false;
    }
    if (power_pc.DidSteppingMemcheckHit())
      return false;
    if ((!stepped && options.ignore_current_code_breakpoint) || !power_pc.CheckBreakPoints())
      return true;
    const u32 pc = state.pc;
    system.GetCPU().Break(
        {.cause = ExecutionStopCause::CodeBreakpoint, .pc = pc, .code_breakpoint_address = pc});
    return false;
  };
  const auto step_one = [&] {
    power_pc.SingleStep();
    power_pc.SetSteppingMemchecksEnabled(true);
    stepped = true;
  };

  UGeckoInstruction instruction = PowerPC::MMU::HostRead_Instruction(guard, state.pc);
  while (can_continue())
  {
    if (WillInstructionReturn(system, instruction))
    {
      step_one();
      break;
    }

    if (instruction.LK)
    {
      const u32 return_pc = state.pc + 4;
      do
      {
        step_one();
      } while (state.pc != return_pc && can_continue());
    }
    else
    {
      step_one();
    }

    instruction = PowerPC::MMU::HostRead_Instruction(guard, state.pc);
  }
  return interrupted ? PPCStepResult::NotStepped : PPCStepResult::Stepped;
}
}  // namespace

PPCStepResult StepPPC(Core::System& system, const PPCStepMode mode,
                      const PPCStepGranularity granularity, const PPCStepOptions& options)
{
  if (mode == PPCStepMode::Out)
    return StepOut(system, options);
  if (granularity == PPCStepGranularity::SourceRow)
    return StepSourceRow(system, mode, options);
  if (mode == PPCStepMode::Over)
    return StepInstructionOver(system, options);
  return StepInstructionInto(system, options);
}
}  // namespace Core::Debug
