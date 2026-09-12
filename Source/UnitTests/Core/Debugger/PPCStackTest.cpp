// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include <gtest/gtest.h>

#include "Common/SymbolDB.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/PPCStack.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
constexpr u32 TEST_ADDRESS = 0x00003100;

class PPCStackTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    auto& system = Core::System::GetInstance();
    system.GetMemory().Init();
    AddressSpace::Init();
    system.GetCoreTiming().Init();
    Core::DeclareAsCPUThread();
    system.GetCPU().Init(PowerPC::CPUCore::Interpreter);

    auto& state = system.GetPPCState();
    state.msr.IR = 0;
    state.msr.DR = 0;
    system.GetPowerPC().MSRUpdated();
    system.GetPPCSymbolDB().Clear();
  }

  void TearDown() override
  {
    auto& system = Core::System::GetInstance();
    system.GetCPU().Shutdown();
    AddressSpace::Shutdown();
    system.GetMemory().Shutdown();
    system.GetCoreTiming().Shutdown();
    Core::UndeclareAsCPUThread();
  }

  static Core::System& System() { return Core::System::GetInstance(); }
};

TEST_F(PPCStackTest, IncludesPcLrAndBackchainFramesWithPaging)
{
  constexpr u32 stack_pointer = TEST_ADDRESS + 0x500;
  constexpr u32 parent = TEST_ADDRESS + 0x600;
  auto& state = System().GetPPCState();
  state.pc = TEST_ADDRESS + 0x100;
  state.gpr[1] = stack_pointer;
  LR(state) = 0x80001004;

  constexpr std::array<u8, 4> backchain{{0x00, 0x00, 0x37, 0x00}};
  constexpr std::array<u8, 4> bottom{{0x00, 0x00, 0x00, 0x00}};
  constexpr std::array<u8, 4> saved_lr{{0x80, 0x00, 0x20, 0x04}};
  System().GetMemory().CopyToEmu(stack_pointer, backchain.data(), backchain.size());
  System().GetMemory().CopyToEmu(parent, bottom.data(), bottom.size());
  System().GetMemory().CopyToEmu(parent + 4, saved_lr.data(), saved_lr.size());

  const Core::Debug::StackTrace trace = Core::Debug::GetPPCStackTrace(System(), 1, 2);
  ASSERT_EQ(trace.total_frames, 3u);
  ASSERT_EQ(trace.frames.size(), 2u);
  EXPECT_EQ(trace.frames[0].index, 1u);
  EXPECT_EQ(trace.frames[0].address, 0x80001000u);
  EXPECT_EQ(trace.frames[1].index, 2u);
  EXPECT_EQ(trace.frames[1].address, 0x80002000u);
}

TEST_F(PPCStackTest, DecoratesFramesWithSourceOrSymbolRelativeDisassembly)
{
  Core::CPUThreadGuard guard(System());
  auto& symbol_db = System().GetPPCSymbolDB();
  symbol_db.AddKnownSymbol(guard, TEST_ADDRESS, 0x100, "main", "game.elf",
                           Common::Symbol::Type::Function);
  const u32 file_index = symbol_db.AddSourceFile("src/main.cpp");
  symbol_db.AddLineEntry(TEST_ADDRESS + 8, file_index, 42);

  auto& state = System().GetPPCState();
  state.gpr[1] = 0;
  LR(state) = 0;

  state.pc = TEST_ADDRESS + 8;
  Core::Debug::StackTrace trace = Core::Debug::GetPPCStackTrace(System());
  ASSERT_EQ(trace.frames.size(), 1u);
  ASSERT_TRUE(trace.frames[0].source);
  EXPECT_EQ(trace.frames[0].source->file, "src/main.cpp");
  EXPECT_EQ(trace.frames[0].source->file_index, file_index);
  EXPECT_EQ(trace.frames[0].source->line, 42u);
  EXPECT_FALSE(trace.frames[0].disassembly_base);

  symbol_db.ClearSourceLineInfo();
  trace = Core::Debug::GetPPCStackTrace(System());
  ASSERT_EQ(trace.frames.size(), 1u);
  ASSERT_TRUE(trace.frames[0].disassembly_base);
  EXPECT_EQ(*trace.frames[0].disassembly_base, TEST_ADDRESS);
  EXPECT_EQ(trace.frames[0].disassembly_line, 3u);
}

TEST_F(PPCStackTest, KeepsUnmappedPcWithoutInventingSource)
{
  auto& state = System().GetPPCState();
  state.pc = TEST_ADDRESS;
  state.gpr[1] = 0;
  LR(state) = 0;

  const Core::Debug::StackTrace trace = Core::Debug::GetPPCStackTrace(System());
  ASSERT_EQ(trace.total_frames, 1u);
  ASSERT_EQ(trace.frames.size(), 1u);
  EXPECT_EQ(trace.frames[0].address, TEST_ADDRESS);
  EXPECT_FALSE(trace.frames[0].source);
  EXPECT_FALSE(trace.frames[0].disassembly_base);
}
}  // namespace
