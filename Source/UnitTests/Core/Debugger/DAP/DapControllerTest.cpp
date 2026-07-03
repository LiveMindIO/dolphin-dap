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
#include <vector>

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Core/Core.h"
#include "Core/Debugger/DAP/DapDebugController.h"
#include "Core/HW/AddressSpace.h"
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
    Core::DeclareAsCPUThread();

    auto& power_pc = system.GetPowerPC();
    power_pc.Reset();

    // Disable address translation so effective == physical and we can exercise
    // the debugger's Effective address space without setting up BATs/page tables.
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
    AddressSpace::Shutdown();
    system.GetMemory().Shutdown();
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
}  // namespace
