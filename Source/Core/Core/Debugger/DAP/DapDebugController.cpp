// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapDebugController.h"

#include <chrono>

#include "Common/Event.h"
#include "Core/Core.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/BreakPoints.h"
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

void DapDebugController::SetCodeBreakpoints(const std::vector<u32>& addresses)
{
  auto& breakpoints = m_system.GetPowerPC().GetBreakPoints();
  breakpoints.Clear();
  for (const u32 address : addresses)
    breakpoints.Add(address, true, false, std::nullopt);
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
}  // namespace DAP
