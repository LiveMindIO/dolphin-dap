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

  Common::Event sync_event;
  auto& power_pc = m_system.GetPowerPC();
  const PowerPC::CoreMode old_mode = power_pc.GetMode();
  power_pc.SetMode(PowerPC::CoreMode::Interpreter);
  cpu.StepOpcode(&sync_event);
  sync_event.WaitFor(std::chrono::milliseconds(20));
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
