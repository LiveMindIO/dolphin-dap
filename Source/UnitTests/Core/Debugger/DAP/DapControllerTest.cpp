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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Common/SymbolDB.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/DAP/DapDebugController.h"
#include "Core/Debugger/DWARF/DwarfImport.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "../DWARF/DwarfTestFixture.h"

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
    ppc_state.Exceptions = 0;
    power_pc.MSRUpdated();

    power_pc.GetBreakPoints().Clear();
    System().GetPPCSymbolDB().Clear();
  }

  void TearDown() override
  {
    auto& system = Core::System::GetInstance();
    system.GetPowerPC().GetBreakPoints().Clear();
    system.GetPowerPC().GetMemChecks().Clear();
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

TEST_F(DapControllerTest, SetRegisterGprRoundTrips)
{
  DAP::DapDebugController controller(System());

  const std::optional<u32> written =
      controller.SetRegister(DAP::REGISTERS_SCOPE, "r3", "0x12345678");
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(*written, 0x12345678u);
  EXPECT_EQ(System().GetPPCState().gpr[3], 0x12345678u);
  EXPECT_EQ(controller.GetRegisters().gpr[3], 0x12345678u);
}

TEST_F(DapControllerTest, SetRegisterPcScopeRoundTrips)
{
  DAP::DapDebugController controller(System());

  ASSERT_TRUE(controller.SetRegister(DAP::PC_SCOPE, "pc", "0x80003100").has_value());
  ASSERT_TRUE(controller.SetRegister(DAP::PC_SCOPE, "lr", "0x80004000").has_value());
  ASSERT_TRUE(controller.SetRegister(DAP::PC_SCOPE, "ctr", "42").has_value());
  ASSERT_TRUE(controller.SetRegister(DAP::PC_SCOPE, "cr", "0xa5a5a5a5").has_value());
  ASSERT_TRUE(controller.SetRegister(DAP::PC_SCOPE, "xer", "0x20000000").has_value());

  const DAP::RegisterSnapshot snapshot = controller.GetRegisters();
  EXPECT_EQ(snapshot.pc, 0x80003100u);
  EXPECT_EQ(snapshot.lr, 0x80004000u);
  EXPECT_EQ(snapshot.ctr, 42u);
  EXPECT_EQ(snapshot.cr, 0xa5a5a5a5u);
  EXPECT_EQ(snapshot.xer, 0x20000000u);
}

TEST_F(DapControllerTest, SetRegisterRejectsUnknownScope)
{
  DAP::DapDebugController controller(System());
  EXPECT_FALSE(controller.SetRegister(9999, "r0", "0").has_value());
}

TEST_F(DapControllerTest, SetRegisterRejectsUnknownName)
{
  DAP::DapDebugController controller(System());
  EXPECT_FALSE(controller.SetRegister(DAP::REGISTERS_SCOPE, "foo", "0").has_value());
  EXPECT_FALSE(controller.SetRegister(DAP::PC_SCOPE, "msr", "0").has_value());
}

TEST_F(DapControllerTest, SetRegisterRejectsInvalidValue)
{
  DAP::DapDebugController controller(System());
  EXPECT_FALSE(controller.SetRegister(DAP::REGISTERS_SCOPE, "r0", "not-a-number").has_value());
}

TEST_F(DapControllerTest, GetThreadsReturnsSinglePpcThread)
{
  DAP::DapDebugController controller(System());
  const std::vector<DAP::ThreadInfo> threads = controller.GetThreads();
  ASSERT_EQ(threads.size(), 1u);
  EXPECT_EQ(threads[0].id, 1);
  EXPECT_EQ(threads[0].name, "PPC");
}

TEST_F(DapControllerTest, GetStackTraceUsesLrAndWalksStackChain)
{
  const u32 sp = TEST_ADDRESS + 0x500;
  const u32 parent = TEST_ADDRESS + 0x600;

  auto& ppc_state = System().GetPPCState();
  ppc_state.gpr[1] = sp;
  LR(ppc_state) = 0x80001004;

  const std::array<u8, 4> backchain{{0x00, 0x00, 0x37, 0x00}};
  const std::array<u8, 4> parent_backchain{{0x00, 0x00, 0x00, 0x00}};
  const std::array<u8, 4> saved_lr{{0x80, 0x00, 0x20, 0x04}};
  System().GetMemory().CopyToEmu(sp, backchain.data(), backchain.size());
  System().GetMemory().CopyToEmu(parent, parent_backchain.data(), parent_backchain.size());
  System().GetMemory().CopyToEmu(parent + 4, saved_lr.data(), saved_lr.size());

  DAP::DapDebugController controller(System());
  const DAP::StackTraceResult trace = controller.GetStackTrace();

  ASSERT_EQ(trace.total_frames, 2);
  ASSERT_EQ(trace.frames.size(), 2u);
  EXPECT_EQ(trace.frames[0].address, 0x80001000u);
  EXPECT_EQ(trace.frames[1].address, 0x80002000u);
}

TEST_F(DapControllerTest, GetStackTraceFallsBackToPcWhenEmpty)
{
  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  ppc_state.gpr[1] = 0;
  LR(ppc_state) = 0;

  DAP::DapDebugController controller(System());
  const DAP::StackTraceResult trace = controller.GetStackTrace();

  ASSERT_EQ(trace.total_frames, 1);
  ASSERT_EQ(trace.frames.size(), 1u);
  EXPECT_EQ(trace.frames[0].address, TEST_ADDRESS);
}

TEST_F(DapControllerTest, GetStackTraceHonorsStartFrameAndLevels)
{
  const u32 sp = TEST_ADDRESS + 0x500;
  const u32 parent = TEST_ADDRESS + 0x600;

  auto& ppc_state = System().GetPPCState();
  ppc_state.gpr[1] = sp;
  LR(ppc_state) = 0x80001004;

  const std::array<u8, 4> backchain{{0x00, 0x00, 0x37, 0x00}};
  const std::array<u8, 4> parent_backchain{{0x00, 0x00, 0x00, 0x00}};
  const std::array<u8, 4> saved_lr{{0x80, 0x00, 0x20, 0x04}};
  System().GetMemory().CopyToEmu(sp, backchain.data(), backchain.size());
  System().GetMemory().CopyToEmu(parent, parent_backchain.data(), parent_backchain.size());
  System().GetMemory().CopyToEmu(parent + 4, saved_lr.data(), saved_lr.size());

  DAP::DapDebugController controller(System());
  const DAP::StackTraceResult trace = controller.GetStackTrace(1, 1);

  ASSERT_EQ(trace.total_frames, 2);
  ASSERT_EQ(trace.frames.size(), 1u);
  EXPECT_EQ(trace.frames[0].id, 1);
  EXPECT_EQ(trace.frames[0].address, 0x80002000u);
}

TEST_F(DapControllerTest, GetStackTraceLevelsZeroReturnsAllFrames)
{
  const u32 sp = TEST_ADDRESS + 0x500;
  const u32 parent = TEST_ADDRESS + 0x600;

  auto& ppc_state = System().GetPPCState();
  ppc_state.gpr[1] = sp;
  LR(ppc_state) = 0x80001004;

  const std::array<u8, 4> backchain{{0x00, 0x00, 0x37, 0x00}};
  const std::array<u8, 4> parent_backchain{{0x00, 0x00, 0x00, 0x00}};
  const std::array<u8, 4> saved_lr{{0x80, 0x00, 0x20, 0x04}};
  System().GetMemory().CopyToEmu(sp, backchain.data(), backchain.size());
  System().GetMemory().CopyToEmu(parent, parent_backchain.data(), parent_backchain.size());
  System().GetMemory().CopyToEmu(parent + 4, saved_lr.data(), saved_lr.size());

  DAP::DapDebugController controller(System());
  // levels == 0 means "all frames" per the DAP spec, not "no frames".
  const DAP::StackTraceResult trace = controller.GetStackTrace(0, 0);

  ASSERT_EQ(trace.total_frames, 2);
  ASSERT_EQ(trace.frames.size(), 2u);
  EXPECT_EQ(trace.frames[0].address, 0x80001000u);
  EXPECT_EQ(trace.frames[1].address, 0x80002000u);
}

TEST_F(DapControllerTest, GetStackTraceStartFramePastEndReturnsNoFramesButFullTotal)
{
  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  ppc_state.gpr[1] = 0;
  LR(ppc_state) = 0;

  DAP::DapDebugController controller(System());
  const DAP::StackTraceResult trace = controller.GetStackTrace(5, 10);

  // The fallback PC frame is the only frame, so a start past it yields nothing,
  // but the reported total still reflects the real depth.
  EXPECT_EQ(trace.total_frames, 1);
  EXPECT_TRUE(trace.frames.empty());
}

TEST_F(DapControllerTest, SetRegisterRejectsOutOfRangeGpr)
{
  DAP::DapDebugController controller(System());
  EXPECT_FALSE(controller.SetRegister(DAP::REGISTERS_SCOPE, "r32", "0").has_value());
  EXPECT_FALSE(controller.SetRegister(DAP::REGISTERS_SCOPE, "r", "0").has_value());
  EXPECT_FALSE(controller.SetRegister(DAP::REGISTERS_SCOPE, "rx", "0").has_value());
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
  controller.SetCodeBreakpoints({{.address = 0x80003100}, {.address = 0x80003200}});
  EXPECT_TRUE(breakpoints.IsAddressBreakPoint(0x80003100));
  EXPECT_TRUE(breakpoints.IsAddressBreakPoint(0x80003200));

  // setBreakpoints is authoritative: a new set replaces the previous one.
  controller.SetCodeBreakpoints({{.address = 0x80003300}});
  EXPECT_FALSE(breakpoints.IsAddressBreakPoint(0x80003100));
  EXPECT_FALSE(breakpoints.IsAddressBreakPoint(0x80003200));
  EXPECT_TRUE(breakpoints.IsAddressBreakPoint(0x80003300));

  controller.SetCodeBreakpoints({});
  EXPECT_FALSE(breakpoints.IsAddressBreakPoint(0x80003300));
}

TEST_F(DapControllerTest, SetCodeBreakpointsStoresCondition)
{
  DAP::DapDebugController controller(System());
  controller.SetCodeBreakpoints({{.address = TEST_ADDRESS, .condition = "r3 == 0"}});

  const TBreakPoint* bp = System().GetPowerPC().GetBreakPoints().GetRegularBreakpoint(TEST_ADDRESS);
  ASSERT_NE(bp, nullptr);
  ASSERT_TRUE(bp->condition.has_value());
  EXPECT_EQ(bp->condition->GetText(), "r3 == 0");
}

TEST_F(DapControllerTest, SetDataBreakpointsAddsReadWriteWatchpoint)
{
  auto& memchecks = System().GetPowerPC().GetMemChecks();

  DAP::DapDebugController controller(System());
  controller.SetDataBreakpoints(
      {{.address = TEST_ADDRESS, .read = true, .write = false, .condition = "r3 == 1"}});

  const TMemCheck* check = memchecks.GetMemCheck(TEST_ADDRESS);
  ASSERT_NE(check, nullptr);
  EXPECT_TRUE(check->is_break_on_read);
  EXPECT_FALSE(check->is_break_on_write);
  ASSERT_TRUE(check->condition.has_value());
  EXPECT_EQ(check->condition->GetText(), "r3 == 1");
}

TEST_F(DapControllerTest, SetDataBreakpointsReplacesPreviousWatchpoints)
{
  auto& memchecks = System().GetPowerPC().GetMemChecks();

  DAP::DapDebugController controller(System());
  controller.SetDataBreakpoints({{.address = TEST_ADDRESS, .read = true, .write = true}});
  controller.SetDataBreakpoints({{.address = TEST_ADDRESS + 4, .read = false, .write = true}});

  EXPECT_EQ(memchecks.GetMemCheck(TEST_ADDRESS), nullptr);
  ASSERT_NE(memchecks.GetMemCheck(TEST_ADDRESS + 4), nullptr);
}

TEST_F(DapControllerTest, SetDataBreakpointsEmptyClearsExisting)
{
  auto& memchecks = System().GetPowerPC().GetMemChecks();

  DAP::DapDebugController controller(System());
  controller.SetDataBreakpoints({{.address = TEST_ADDRESS, .read = true, .write = true}});
  ASSERT_NE(memchecks.GetMemCheck(TEST_ADDRESS), nullptr);

  controller.SetDataBreakpoints({});
  EXPECT_EQ(memchecks.GetMemCheck(TEST_ADDRESS), nullptr);
  EXPECT_FALSE(memchecks.HasAny());
}

TEST_F(DapControllerTest, SetCodeBreakpointsInvalidConditionAddsUnconditionalBreakpoint)
{
  auto& breakpoints = System().GetPowerPC().GetBreakPoints();

  DAP::DapDebugController controller(System());
  // A condition that fails to parse must not drop the breakpoint; it stays as an
  // unconditional stop rather than silently disappearing.
  controller.SetCodeBreakpoints({{.address = TEST_ADDRESS, .condition = "not a condition"}});

  const TBreakPoint* bp = breakpoints.GetRegularBreakpoint(TEST_ADDRESS);
  ASSERT_NE(bp, nullptr);
  EXPECT_FALSE(bp->condition.has_value());
}

TEST_F(DapControllerTest, SetCodeBreakpointsEmptyConditionIsUnconditional)
{
  auto& breakpoints = System().GetPowerPC().GetBreakPoints();

  DAP::DapDebugController controller(System());
  controller.SetCodeBreakpoints({{.address = TEST_ADDRESS, .condition = ""}});

  const TBreakPoint* bp = breakpoints.GetRegularBreakpoint(TEST_ADDRESS);
  ASSERT_NE(bp, nullptr);
  EXPECT_FALSE(bp->condition.has_value());
}

// Conditional breakpoint behavior is gated by PowerPCManager::CheckBreakPoints and
// TMemCheck::Action, both of which call EvaluateCondition — the same path the
// Qt debugger and interpreter use. We install via DAP then drive PPC state and
// assert those core checks, mirroring PageFaultTest / PageTableHostMappingTest
// (real Core::System, no ISO/JIT boot).

TEST_F(DapControllerTest, ConditionalCodeBreakpointFiresWhenExpressionTrue)
{
  auto& power_pc = System().GetPowerPC();
  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  ppc_state.gpr[3] = 5;

  DAP::DapDebugController controller(System());
  controller.SetCodeBreakpoints({{.address = TEST_ADDRESS, .condition = "r3 == 5"}});

  EXPECT_TRUE(power_pc.CheckBreakPoints());
}

TEST_F(DapControllerTest, ConditionalCodeBreakpointSuppressedWhenExpressionFalse)
{
  auto& power_pc = System().GetPowerPC();
  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  ppc_state.gpr[3] = 0;

  DAP::DapDebugController controller(System());
  controller.SetCodeBreakpoints({{.address = TEST_ADDRESS, .condition = "r3 == 5"}});

  EXPECT_FALSE(power_pc.CheckBreakPoints());
}

TEST_F(DapControllerTest, UnconditionalCodeBreakpointFiresWithoutCondition)
{
  auto& power_pc = System().GetPowerPC();
  System().GetPPCState().pc = TEST_ADDRESS;

  DAP::DapDebugController controller(System());
  controller.SetCodeBreakpoints({{.address = TEST_ADDRESS}});

  EXPECT_TRUE(power_pc.CheckBreakPoints());
}

TEST_F(DapControllerTest, ConditionalMemCheckFiresWhenExpressionTrue)
{
  auto& memchecks = System().GetPowerPC().GetMemChecks();
  System().GetPPCState().gpr[3] = 1;

  DAP::DapDebugController controller(System());
  controller.SetDataBreakpoints(
      {{.address = TEST_ADDRESS, .read = false, .write = true, .condition = "r3 == 1"}});

  TMemCheck* check = memchecks.GetMemCheck(TEST_ADDRESS);
  ASSERT_NE(check, nullptr);
  EXPECT_TRUE(check->Action(System(), 0, TEST_ADDRESS, true, 4, TEST_ADDRESS));
}

TEST_F(DapControllerTest, ConditionalMemCheckSuppressedWhenExpressionFalse)
{
  auto& memchecks = System().GetPowerPC().GetMemChecks();
  System().GetPPCState().gpr[3] = 0;

  DAP::DapDebugController controller(System());
  controller.SetDataBreakpoints(
      {{.address = TEST_ADDRESS, .read = false, .write = true, .condition = "r3 == 1"}});

  TMemCheck* check = memchecks.GetMemCheck(TEST_ADDRESS);
  ASSERT_NE(check, nullptr);
  EXPECT_FALSE(check->Action(System(), 0, TEST_ADDRESS, true, 4, TEST_ADDRESS));
}

TEST_F(DapControllerTest, ConditionalMemCheckRespectsAccessType)
{
  auto& memchecks = System().GetPowerPC().GetMemChecks();
  System().GetPPCState().gpr[3] = 1;

  DAP::DapDebugController controller(System());
  // Read-only watchpoint must not fire on a write even when the condition is true.
  controller.SetDataBreakpoints(
      {{.address = TEST_ADDRESS, .read = true, .write = false, .condition = "r3 == 1"}});

  TMemCheck* check = memchecks.GetMemCheck(TEST_ADDRESS);
  ASSERT_NE(check, nullptr);
  EXPECT_FALSE(check->Action(System(), 0, TEST_ADDRESS, true, 4, TEST_ADDRESS));
  EXPECT_TRUE(check->Action(System(), 0, TEST_ADDRESS, false, 4, TEST_ADDRESS));
}

TEST_F(DapControllerTest, EvaluateExpressionReturnsRegisterValue)
{
  System().GetPPCState().gpr[3] = 0x12345678;

  DAP::DapDebugController controller(System());
  const std::optional<std::string> result = controller.EvaluateExpression("r3");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "0x12345678");
}

TEST_F(DapControllerTest, EvaluateExpressionRejectsInvalidSyntax)
{
  DAP::DapDebugController controller(System());
  EXPECT_FALSE(controller.EvaluateExpression("not an expression").has_value());
}

TEST_F(DapControllerTest, EvaluateExpressionFormatsFractionalResultAsDecimal)
{
  DAP::DapDebugController controller(System());
  // Non-integral results can't be shown as a 32-bit hex word, so they fall back
  // to a plain decimal string.
  const std::optional<std::string> result = controller.EvaluateExpression("1.5");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "1.5");
}

TEST_F(DapControllerTest, EvaluateExpressionFormatsNegativeResultAsDecimal)
{
  DAP::DapDebugController controller(System());
  // Negative values fall outside the unsigned-hex range and use decimal too.
  const std::optional<std::string> result = controller.EvaluateExpression("0 - 5");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "-5");
}

TEST_F(DapControllerTest, EvaluateExpressionWritesBackToRegisters)
{
  auto& ppc_state = System().GetPPCState();
  ppc_state.gpr[3] = 0;

  DAP::DapDebugController controller(System());
  // The PPC expression evaluator supports assignment; the controller must run it
  // under a CPUThreadGuard so the write reaches emulated state.
  const std::optional<std::string> result = controller.EvaluateExpression("r3 = 42");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(ppc_state.gpr[3], 42u);
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
  controller.SetCodeBreakpoints({{.address = TEST_ADDRESS + 4}});
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

TEST_F(DapControllerTest, SetPcMovesProgramCounterAndNpc)
{
  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  ppc_state.npc = TEST_ADDRESS + 4;

  DAP::DapDebugController controller(System());
  controller.SetPC(TEST_ADDRESS + 0x40);

  EXPECT_EQ(ppc_state.pc, TEST_ADDRESS + 0x40u);
  EXPECT_EQ(ppc_state.npc, TEST_ADDRESS + 0x40u);
  EXPECT_EQ(controller.GetPC(), TEST_ADDRESS + 0x40u);
}

TEST_F(DapControllerTest, GetStopInfoClassifiesCodeBreakpoint)
{
  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;

  DAP::DapDebugController controller(System());
  controller.SetCodeBreakpoints({{.address = TEST_ADDRESS}});

  const DAP::StopInfo info = controller.GetStopInfo();
  EXPECT_EQ(info.reason, DAP::StopReason::CodeBreakpoint);
  ASSERT_TRUE(info.hit_breakpoint_address.has_value());
  EXPECT_EQ(*info.hit_breakpoint_address, TEST_ADDRESS);
}

TEST_F(DapControllerTest, GetStopInfoClassifiesDataBreakpointFromMemcheckFlag)
{
  // A watchpoint hit is signaled by EXCEPTION_FAKE_MEMCHECK_HIT on the accessing
  // instruction, not by the PC matching the watched data address.
  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  ppc_state.Exceptions = EXCEPTION_DSI | EXCEPTION_FAKE_MEMCHECK_HIT;

  DAP::DapDebugController controller(System());
  const DAP::StopInfo info = controller.GetStopInfo();
  EXPECT_EQ(info.reason, DAP::StopReason::DataBreakpoint);
  // The watched data address is not the PC, so no code-breakpoint id is attached.
  EXPECT_FALSE(info.hit_breakpoint_address.has_value());
}

TEST_F(DapControllerTest, GetStopInfoPrefersDataBreakpointOverCodeBreakpointAtPc)
{
  // If a watchpoint fired while the PC also sits on a code breakpoint, the
  // executed data access is the actual cause and wins the classification.
  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  ppc_state.Exceptions = EXCEPTION_DSI | EXCEPTION_FAKE_MEMCHECK_HIT;

  DAP::DapDebugController controller(System());
  controller.SetCodeBreakpoints({{.address = TEST_ADDRESS}});

  EXPECT_EQ(controller.GetStopInfo().reason, DAP::StopReason::DataBreakpoint);
}

TEST_F(DapControllerTest, GetStopInfoWithWatchpointButNoHitFlagIsStep)
{
  // A watchpoint being installed is not itself a stop cause; without the
  // memcheck-hit flag the classification falls back to Step.
  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;

  DAP::DapDebugController controller(System());
  controller.SetDataBreakpoints({{.address = TEST_ADDRESS, .read = true, .write = true}});

  EXPECT_EQ(controller.GetStopInfo().reason, DAP::StopReason::Step);
}

TEST_F(DapControllerTest, GetStopInfoDefaultsToStepWithoutBreakpoint)
{
  System().GetPPCState().pc = TEST_ADDRESS;

  DAP::DapDebugController controller(System());
  const DAP::StopInfo info = controller.GetStopInfo();
  EXPECT_EQ(info.reason, DAP::StopReason::Step);
  EXPECT_FALSE(info.hit_breakpoint_address.has_value());
}

TEST_F(DapControllerTest, GetExceptionInfoReturnsNulloptWhenNoException)
{
  System().GetPPCState().Exceptions = 0;

  DAP::DapDebugController controller(System());
  EXPECT_FALSE(controller.GetExceptionInfo().has_value());
}

TEST_F(DapControllerTest, GetExceptionInfoDescribesPendingExceptions)
{
  System().GetPPCState().Exceptions = EXCEPTION_DSI | EXCEPTION_PROGRAM;

  DAP::DapDebugController controller(System());
  const auto info = controller.GetExceptionInfo();
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->exceptions, static_cast<u32>(EXCEPTION_DSI | EXCEPTION_PROGRAM));
  EXPECT_NE(info->description.find("DSI"), std::string::npos);
  EXPECT_NE(info->description.find("Program"), std::string::npos);
}

TEST_F(DapControllerTest, GetExceptionInfoExcludesMemcheckHit)
{
  // A watchpoint hit (synthetic DSI + fake-memcheck flag) is a data breakpoint,
  // not a guest exception, so exceptionInfo reports nothing.
  System().GetPPCState().Exceptions = EXCEPTION_DSI | EXCEPTION_FAKE_MEMCHECK_HIT;

  DAP::DapDebugController controller(System());
  EXPECT_FALSE(controller.GetExceptionInfo().has_value());
}

TEST_F(DapControllerTest, GetExceptionInfoReportsRealExceptionAlongsideMemcheckHit)
{
  // Only the synthetic DSI is suppressed by the memcheck flag; a genuine
  // exception raised in the same word is still reported.
  System().GetPPCState().Exceptions =
      EXCEPTION_DSI | EXCEPTION_FAKE_MEMCHECK_HIT | EXCEPTION_PROGRAM;

  DAP::DapDebugController controller(System());
  const auto info = controller.GetExceptionInfo();
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->exceptions, static_cast<u32>(EXCEPTION_PROGRAM));
  EXPECT_NE(info->description.find("Program"), std::string::npos);
  EXPECT_EQ(info->description.find("DSI"), std::string::npos);
}

TEST_F(DapControllerTest, GetLoadedSourcesListsFunctionSymbols)
{
  Core::CPUThreadGuard guard(System());
  System().GetPowerPC().GetSymbolDB().AddKnownSymbol(guard, TEST_ADDRESS, 0x100, "main", "game.elf",
                                                     Common::Symbol::Type::Function);

  DAP::DapDebugController controller(System());
  const std::vector<DAP::LoadedSource> sources = controller.GetLoadedSources();
  ASSERT_EQ(sources.size(), 1u);
  EXPECT_EQ(sources[0].source_reference, static_cast<int>(TEST_ADDRESS));
  EXPECT_EQ(sources[0].name, "game.elf");
  EXPECT_EQ(sources[0].path, "0x00003100");
}

TEST_F(DapControllerTest, GetSourceReturnsDisassembly)
{
  const std::array<u8, 4> nop{{0x60, 0x00, 0x00, 0x00}};
  System().GetMemory().CopyToEmu(TEST_ADDRESS, nop.data(), nop.size());

  DAP::DapDebugController controller(System());
  const auto source = controller.GetSource(TEST_ADDRESS, 0, 0);
  ASSERT_TRUE(source.has_value());
  EXPECT_EQ(source->mime_type, "text/x-disassembly");
  EXPECT_NE(source->content.find("nop"), std::string::npos);
}

TEST_F(DapControllerTest, GetSourceRejectsInvalidBase)
{
  DAP::DapDebugController controller(System());
  EXPECT_FALSE(controller.GetSource(INVALID_ADDRESS, 0, 0).has_value());
}

TEST_F(DapControllerTest, GetBreakpointLocationsStopsAtInvalidMemory)
{
  DAP::DapDebugController controller(System());
  const std::vector<DAP::BreakpointLocation> locations =
      controller.GetBreakpointLocations(TEST_ADDRESS, 0, 3);
  ASSERT_EQ(locations.size(), 4u);
  EXPECT_EQ(locations[0].line, 0);
  EXPECT_EQ(locations[3].line, 3);
}

TEST_F(DapControllerTest, GetStackTraceIncludesSourceForKnownSymbol)
{
  Core::CPUThreadGuard guard(System());
  System().GetPowerPC().GetSymbolDB().AddKnownSymbol(guard, TEST_ADDRESS, 0x100, "main", "game.elf",
                                                     Common::Symbol::Type::Function);

  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS + 8;
  LR(ppc_state) = 0;

  DAP::DapDebugController controller(System());
  const DAP::StackTraceResult trace = controller.GetStackTrace();
  ASSERT_EQ(trace.frames.size(), 1u);
  ASSERT_TRUE(trace.frames[0].source_base.has_value());
  EXPECT_EQ(*trace.frames[0].source_base, TEST_ADDRESS);
  EXPECT_EQ(trace.frames[0].source_line, 2);
}

TEST_F(DapControllerTest, RestartResetsPpcState)
{
  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = TEST_ADDRESS;
  ppc_state.gpr[3] = 0xdeadbeef;

  DAP::DapDebugController controller(System());
  controller.Restart();

  EXPECT_NE(ppc_state.pc, TEST_ADDRESS);
  EXPECT_EQ(ppc_state.gpr[3], 0u);
}

TEST_F(DapControllerTest, TerminateBreaksCpu)
{
  DAP::DapDebugController controller(System());
  controller.Terminate();
  EXPECT_TRUE(System().GetCPU().IsStepping());
}

TEST_F(DapControllerTest, GetStackTraceUsesDwarfSourceLineWhenAvailable)
{
  Core::CPUThreadGuard guard(System());
  ASSERT_TRUE(Core::Debug::ImportDwarf(guard, System().GetPowerPC().GetSymbolDB(),
                                       DwarfTestFixture::kDebugSection,
                                       DwarfTestFixture::kLineSection));

  auto& ppc_state = System().GetPPCState();
  ppc_state.pc = DwarfTestFixture::kLineTwoAddress;
  LR(ppc_state) = 0;

  DAP::DapDebugController controller(System());
  const DAP::StackTraceResult trace = controller.GetStackTrace();
  ASSERT_EQ(trace.frames.size(), 1u);
  ASSERT_TRUE(trace.frames[0].source_file.has_value());
  EXPECT_EQ(*trace.frames[0].source_file, DwarfTestFixture::kCompileUnitName);
  EXPECT_EQ(trace.frames[0].source_line, 2);
}

TEST_F(DapControllerTest, GetLoadedSourcesListsDwarfFiles)
{
  Core::CPUThreadGuard guard(System());
  ASSERT_TRUE(Core::Debug::ImportDwarf(guard, System().GetPowerPC().GetSymbolDB(),
                                       DwarfTestFixture::kDebugSection,
                                       DwarfTestFixture::kLineSection));

  DAP::DapDebugController controller(System());
  const std::vector<DAP::LoadedSource> sources = controller.GetLoadedSources();
  ASSERT_EQ(sources.size(), 1u);
  EXPECT_EQ(sources[0].path, DwarfTestFixture::kCompileUnitName);
  EXPECT_EQ(sources[0].source_reference, 1);
}
}  // namespace
