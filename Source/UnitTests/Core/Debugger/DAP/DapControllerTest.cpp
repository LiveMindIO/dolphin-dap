// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Integration tests for DapDebugController against a real (but un-booted)
// Core::System. Following the pattern of PageFaultTest / PageTableHostMappingTest,
// we initialize just the memory subsystem and the address-space accessors, declare
// the test thread as the CPU thread, and drive the PPC state directly. No ISO,
// JIT, or boot process is required: with address translation disabled (MSR.DR=0)
// effective addresses map straight to physical RAM, which is all the debugger
// read/write/disassemble paths need.

#include <array>
#include <chrono>
#include <vector>

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/DAP/DapDebugController.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
// A GameCube-range physical/effective address comfortably inside MEM1.
constexpr u32 TEST_ADDRESS = 0x00003100;
// Segment 0 but past MEM1 (24 MiB), so not a RAM address.
constexpr u32 INVALID_ADDRESS = 0x0C000000;

class DapControllerTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    auto& system = Core::System::GetInstance();
    auto& memory = system.GetMemory();

    memory.Init();
    AddressSpace::Init();
    system.GetCoreTiming().Init();
    Core::DeclareAsCPUThread();
    system.GetCPU().Init(PowerPC::CPUCore::Interpreter);

    // Disable address translation so effective == physical and we can exercise
    // the debugger's Effective address space without setting up BATs/page tables.
    auto& power_pc = system.GetPowerPC();
    auto& ppc_state = system.GetPPCState();
    ppc_state.msr.IR = 0;
    ppc_state.msr.DR = 0;
    power_pc.MSRUpdated();

    power_pc.GetBreakPoints().Clear();
  }

  void TearDown() override
  {
    auto& system = Core::System::GetInstance();
    system.GetPowerPC().GetBreakPoints().Clear();
    system.GetCPU().Shutdown();
    AddressSpace::Shutdown();
    system.GetMemory().Shutdown();
    system.GetCoreTiming().Shutdown();
    Core::UndeclareAsCPUThread();
  }

  static Core::System& System() { return Core::System::GetInstance(); }
};

TEST_F(DapControllerTest, GetRegistersReflectsPpcState)
{
  auto& ppc_state = System().GetPPCState();
  ppc_state.gpr[0] = 0x11111111;
  ppc_state.gpr[3] = 0x12345678;
  ppc_state.gpr[31] = 0xcafebabe;
  ppc_state.pc = 0x80003100;
  LR(ppc_state) = 0x80004000;
  CTR(ppc_state) = 0x00000042;
  ppc_state.cr.Set(0xa5a5a5a5);
  UReg_XER xer;
  xer.Hex = 0x20000000;
  ppc_state.SetXER(xer);

  DAP::DapDebugController controller(System());
  const DAP::RegisterSnapshot snapshot = controller.GetRegisters();

  EXPECT_EQ(snapshot.gpr[0], 0x11111111u);
  EXPECT_EQ(snapshot.gpr[3], 0x12345678u);
  EXPECT_EQ(snapshot.gpr[31], 0xcafebabeu);
  EXPECT_EQ(snapshot.pc, 0x80003100u);
  EXPECT_EQ(snapshot.lr, 0x80004000u);
  EXPECT_EQ(snapshot.ctr, 0x00000042u);
  EXPECT_EQ(snapshot.cr, ppc_state.cr.Get());
  EXPECT_EQ(snapshot.xer, ppc_state.GetXER().Hex);
}

TEST_F(DapControllerTest, ReadMemoryRoundTripsRam)
{
  const std::array<u8, 8> payload{{0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04}};
  System().GetMemory().CopyToEmu(TEST_ADDRESS, payload.data(), payload.size());

  DAP::DapDebugController controller(System());
  const std::vector<u8> read = controller.ReadMemory(TEST_ADDRESS, payload.size());

  ASSERT_EQ(read.size(), payload.size());
  EXPECT_TRUE(std::equal(payload.begin(), payload.end(), read.begin()));
}

TEST_F(DapControllerTest, ReadMemoryInvalidAddressReturnsEmpty)
{
  DAP::DapDebugController controller(System());
  EXPECT_TRUE(controller.ReadMemory(INVALID_ADDRESS, 16).empty());
}

TEST_F(DapControllerTest, ReadMemoryStopsAtEndOfRam)
{
  const u32 ram_size = System().GetMemory().GetRamSizeReal();
  const u32 start = ram_size - 2;

  DAP::DapDebugController controller(System());
  // Only the two bytes before the end of RAM are readable.
  EXPECT_EQ(controller.ReadMemory(start, 8).size(), 2u);
}

TEST_F(DapControllerTest, WriteMemoryPersistsToRam)
{
  const std::array<u8, 4> payload{{0xca, 0xfe, 0xd0, 0x0d}};

  DAP::DapDebugController controller(System());
  const std::size_t written = controller.WriteMemory(TEST_ADDRESS, payload);
  EXPECT_EQ(written, payload.size());

  std::array<u8, 4> read_back{};
  System().GetMemory().CopyFromEmu(read_back.data(), TEST_ADDRESS, read_back.size());
  EXPECT_EQ(read_back, payload);

  // And it round-trips back through the controller's own read path.
  const std::vector<u8> via_controller = controller.ReadMemory(TEST_ADDRESS, payload.size());
  ASSERT_EQ(via_controller.size(), payload.size());
  EXPECT_TRUE(std::equal(payload.begin(), payload.end(), via_controller.begin()));
}

TEST_F(DapControllerTest, WriteMemoryStopsAtEndOfRam)
{
  const u32 ram_size = System().GetMemory().GetRamSizeReal();
  const u32 start = ram_size - 2;
  const std::array<u8, 6> payload{{1, 2, 3, 4, 5, 6}};

  DAP::DapDebugController controller(System());
  EXPECT_EQ(controller.WriteMemory(start, payload), 2u);
}

TEST_F(DapControllerTest, WriteMemoryInvalidAddressWritesNothing)
{
  const std::array<u8, 4> payload{{1, 2, 3, 4}};
  DAP::DapDebugController controller(System());
  EXPECT_EQ(controller.WriteMemory(INVALID_ADDRESS, payload), 0u);
}

TEST_F(DapControllerTest, DisassembleDecodesKnownOpcodes)
{
  // Big-endian PPC encodings: ori r0,r0,0 (canonical nop) and blr.
  const std::array<u8, 8> code{{0x60, 0x00, 0x00, 0x00, 0x4e, 0x80, 0x00, 0x20}};
  System().GetMemory().CopyToEmu(TEST_ADDRESS, code.data(), code.size());

  DAP::DapDebugController controller(System());
  const std::string disasm = controller.Disassemble(TEST_ADDRESS, 2);

  EXPECT_NE(disasm.find("nop"), std::string::npos) << disasm;
  EXPECT_NE(disasm.find("blr"), std::string::npos) << disasm;
}

TEST_F(DapControllerTest, DisassembleInvalidAddressReportsNoRam)
{
  DAP::DapDebugController controller(System());
  const std::string disasm = controller.Disassemble(INVALID_ADDRESS, 1);
  EXPECT_NE(disasm.find("No RAM"), std::string::npos) << disasm;
}

TEST_F(DapControllerTest, SetCodeBreakpointsAddsThenReplaces)
{
  auto& breakpoints = System().GetPowerPC().GetBreakPoints();

  DAP::DapDebugController controller(System());
  controller.SetCodeBreakpoints({0x80003100, 0x80003200});
  EXPECT_TRUE(breakpoints.IsAddressBreakPoint(0x80003100));
  EXPECT_TRUE(breakpoints.IsAddressBreakPoint(0x80003200));

  // setBreakpoints is authoritative: a new set replaces the previous one.
  controller.SetCodeBreakpoints({0x80003300});
  EXPECT_FALSE(breakpoints.IsAddressBreakPoint(0x80003100));
  EXPECT_FALSE(breakpoints.IsAddressBreakPoint(0x80003200));
  EXPECT_TRUE(breakpoints.IsAddressBreakPoint(0x80003300));

  controller.SetCodeBreakpoints({});
  EXPECT_FALSE(breakpoints.IsAddressBreakPoint(0x80003300));
}

TEST_F(DapControllerTest, StepIntoAdvancesPc)
{
  const std::array<u8, 4> nop{{0x60, 0x00, 0x00, 0x00}};
  System().GetMemory().CopyToEmu(TEST_ADDRESS, nop.data(), nop.size());

  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  ASSERT_TRUE(System().GetCPU().IsStepping());

  DAP::DapDebugController controller(System());
  controller.StepInto();
  EXPECT_EQ(ppc_state.pc, TEST_ADDRESS + 4u);
}

TEST_F(DapControllerTest, StepIntoWhenNotSteppingIsNoOp)
{
  const std::array<u8, 4> nop{{0x60, 0x00, 0x00, 0x00}};
  System().GetMemory().CopyToEmu(TEST_ADDRESS, nop.data(), nop.size());

  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  System().GetCPU().SetStepping(false);
  ASSERT_FALSE(System().GetCPU().IsStepping());

  DAP::DapDebugController controller(System());
  controller.StepInto();
  EXPECT_EQ(ppc_state.pc, TEST_ADDRESS);
}

TEST_F(DapControllerTest, StepOverWhenNotSteppingReturnsSteppedWithoutAdvancing)
{
  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  System().GetCPU().SetStepping(false);

  DAP::DapDebugController controller(System());
  EXPECT_EQ(controller.StepOver(), DAP::StepOverResult::Stepped);
  EXPECT_EQ(ppc_state.pc, TEST_ADDRESS);
}

TEST_F(DapControllerTest, StepOutWhenNotSteppingIsNoOp)
{
  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  System().GetCPU().SetStepping(false);

  DAP::DapDebugController controller(System());
  controller.StepOut();
  EXPECT_EQ(ppc_state.pc, TEST_ADDRESS);
}

TEST_F(DapControllerTest, StepOverNonBranchAdvancesPc)
{
  const std::array<u8, 4> nop{{0x60, 0x00, 0x00, 0x00}};
  System().GetMemory().CopyToEmu(TEST_ADDRESS, nop.data(), nop.size());

  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  ASSERT_TRUE(System().GetCPU().IsStepping());

  DAP::DapDebugController controller(System());
  EXPECT_EQ(controller.StepOver(), DAP::StepOverResult::Stepped);
  EXPECT_EQ(ppc_state.pc, TEST_ADDRESS + 4u);
}

TEST_F(DapControllerTest, StepOverBranchSetsTemporaryAndContinues)
{
  // bl +4 then nop — step over should resume until the insn after the branch.
  const std::array<u8, 8> code{{0x48, 0x00, 0x00, 0x01, 0x60, 0x00, 0x00, 0x00}};
  System().GetMemory().CopyToEmu(TEST_ADDRESS, code.data(), code.size());

  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  ASSERT_TRUE(System().GetCPU().IsStepping());

  DAP::DapDebugController controller(System());
  EXPECT_EQ(controller.StepOver(), DAP::StepOverResult::Continuing);
  EXPECT_FALSE(System().GetCPU().IsStepping());
  // The resume relies on a temporary breakpoint planted at the return address
  // (pc + 4) so the core stops again once the call returns.
  EXPECT_NE(System().GetPowerPC().GetBreakPoints().GetBreakpoint(TEST_ADDRESS + 4), nullptr);
}

TEST_F(DapControllerTest, StepOutRunsUntilReturn)
{
  const std::array<u8, 8> code{{0x60, 0x00, 0x00, 0x00, 0x4e, 0x80, 0x00, 0x20}};
  System().GetMemory().CopyToEmu(TEST_ADDRESS, code.data(), code.size());

  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  LR(ppc_state) = TEST_ADDRESS + 0x100;
  ASSERT_TRUE(System().GetCPU().IsStepping());

  DAP::DapDebugController controller(System());
  controller.StepOut();
  EXPECT_EQ(ppc_state.pc, TEST_ADDRESS + 0x100u);
}

TEST_F(DapControllerTest, StepOutStopsAtBreakpointBeforeReturn)
{
  // nop; nop; blr — a breakpoint on the second instruction must abort the
  // step-out there rather than running through to the return address.
  const std::array<u8, 12> code{
      {0x60, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x4e, 0x80, 0x00, 0x20}};
  System().GetMemory().CopyToEmu(TEST_ADDRESS, code.data(), code.size());

  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  LR(ppc_state) = TEST_ADDRESS + 0x200;
  ASSERT_TRUE(System().GetCPU().IsStepping());

  DAP::DapDebugController controller(System());
  controller.SetCodeBreakpoints({TEST_ADDRESS + 4});
  controller.StepOut();

  EXPECT_EQ(ppc_state.pc, TEST_ADDRESS + 4u);
  EXPECT_NE(ppc_state.pc, TEST_ADDRESS + 0x200u);
}

TEST_F(DapControllerTest, StepOutTimesOutOnNonReturningCode)
{
  // b . (branch-to-self, 0x48000000) never returns and hits no breakpoint, so
  // only the timeout can end the step-out. A small deadline keeps this fast;
  // without the timeout the interpreter loop would spin forever.
  const std::array<u8, 4> spin{{0x48, 0x00, 0x00, 0x00}};
  System().GetMemory().CopyToEmu(TEST_ADDRESS, spin.data(), spin.size());

  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  ASSERT_TRUE(System().GetCPU().IsStepping());

  DAP::DapDebugController controller(System());
  controller.StepOut(std::chrono::milliseconds(5));

  // The branch loops back to itself, so the bounded step-out returns with the
  // PC still parked on the branch instead of hanging.
  EXPECT_EQ(ppc_state.pc, TEST_ADDRESS);
}

TEST_F(DapControllerTest, StepOutReturnsImmediatelyWhenPcIsOnReturn)
{
  // PC already sits on a blr: step-out should take the return on the first
  // iteration and land on LR without stepping any preceding instructions.
  const std::array<u8, 4> blr{{0x4e, 0x80, 0x00, 0x20}};
  System().GetMemory().CopyToEmu(TEST_ADDRESS, blr.data(), blr.size());

  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  LR(ppc_state) = TEST_ADDRESS + 0x100;
  ASSERT_TRUE(System().GetCPU().IsStepping());

  DAP::DapDebugController controller(System());
  controller.StepOut(std::chrono::seconds(1));
  EXPECT_EQ(ppc_state.pc, TEST_ADDRESS + 0x100u);
}

TEST_F(DapControllerTest, StepOutStepsOverNestedCall)
{
  // bl +0x40 then blr; the callee at +0x40 is nop; blr. Step-out must run the
  // linking branch's inner loop (stepping the whole call as a unit) instead of
  // stopping inside the callee, then take the outer return.
  const std::array<u8, 8> caller{{0x48, 0x00, 0x00, 0x41, 0x4e, 0x80, 0x00, 0x20}};
  const std::array<u8, 8> callee{{0x60, 0x00, 0x00, 0x00, 0x4e, 0x80, 0x00, 0x20}};
  System().GetMemory().CopyToEmu(TEST_ADDRESS, caller.data(), caller.size());
  System().GetMemory().CopyToEmu(TEST_ADDRESS + 0x40, callee.data(), callee.size());

  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  ASSERT_TRUE(System().GetCPU().IsStepping());

  DAP::DapDebugController controller(System());
  controller.StepOut(std::chrono::seconds(1));

  // bl sets LR to TEST_ADDRESS + 4; the callee returns there, so the inner loop
  // ends on the instruction after the call rather than parking inside the
  // callee body ([TEST_ADDRESS + 0x40, TEST_ADDRESS + 0x48)).
  EXPECT_EQ(ppc_state.pc, TEST_ADDRESS + 4u);
}

TEST_F(DapControllerTest, StepOverNonLinkingBranchStepsInto)
{
  // Only linking branches get the temporary-breakpoint/continue treatment. A
  // plain b +8 has no LK bit, so step-over degrades to step-into and follows
  // the branch rather than planting a temporary breakpoint.
  const std::array<u8, 4> branch{{0x48, 0x00, 0x00, 0x08}};
  System().GetMemory().CopyToEmu(TEST_ADDRESS, branch.data(), branch.size());

  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  ASSERT_TRUE(System().GetCPU().IsStepping());

  DAP::DapDebugController controller(System());
  EXPECT_EQ(controller.StepOver(), DAP::StepOverResult::Stepped);
  EXPECT_EQ(ppc_state.pc, TEST_ADDRESS + 8u);
  EXPECT_TRUE(System().GetCPU().IsStepping());
}
}  // namespace
