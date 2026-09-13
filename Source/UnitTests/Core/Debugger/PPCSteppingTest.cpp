// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/PPCStepping.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
constexpr u32 TEST_ADDRESS = 0x00003100;
constexpr std::array<u8, 4> NOP{{0x60, 0x00, 0x00, 0x00}};

class PPCSteppingTest : public ::testing::Test
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
    state.Exceptions = 0;
    system.GetPowerPC().MSRUpdated();
    system.GetPowerPC().GetBreakPoints().Clear();
    system.GetPPCSymbolDB().Clear();
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

TEST_F(PPCSteppingTest, InstructionIntoAdvancesOneOpcode)
{
  System().GetMemory().CopyToEmu(TEST_ADDRESS, NOP.data(), NOP.size());
  System().GetPPCState().pc = TEST_ADDRESS;

  EXPECT_EQ(Core::Debug::StepPPC(System(), Core::Debug::PPCStepMode::Into,
                                 Core::Debug::PPCStepGranularity::Instruction),
            Core::Debug::PPCStepResult::Stepped);
  EXPECT_EQ(System().GetPPCState().pc, TEST_ADDRESS + 4);
}

TEST_F(PPCSteppingTest, InstructionOverContinuesToTemporaryReturnBreakpoint)
{
  constexpr std::array<u8, 4> call{{0x48, 0x00, 0x00, 0x41}};
  System().GetMemory().CopyToEmu(TEST_ADDRESS, call.data(), call.size());
  System().GetPPCState().pc = TEST_ADDRESS;

  EXPECT_EQ(Core::Debug::StepPPC(System(), Core::Debug::PPCStepMode::Over,
                                 Core::Debug::PPCStepGranularity::Instruction),
            Core::Debug::PPCStepResult::Continuing);
  EXPECT_FALSE(System().GetCPU().IsStepping());
  EXPECT_NE(System().GetPowerPC().GetBreakPoints().GetBreakpoint(TEST_ADDRESS + 4), nullptr);
}

TEST_F(PPCSteppingTest, SourceIntoLeavesExactFileIdentityAndLine)
{
  constexpr std::array<u8, 8> code{{0x60, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00}};
  System().GetMemory().CopyToEmu(TEST_ADDRESS, code.data(), code.size());
  auto& symbols = System().GetPPCSymbolDB();
  const u32 first = symbols.AddSourceFileInstance("same.c");
  const u32 second = symbols.AddSourceFileInstance("same.c");
  symbols.AddLineEntry(TEST_ADDRESS, first, 7);
  symbols.AddLineEntry(TEST_ADDRESS + 4, second, 7);
  System().GetPPCState().pc = TEST_ADDRESS;

  Core::Debug::StepPPC(System(), Core::Debug::PPCStepMode::Into,
                       Core::Debug::PPCStepGranularity::SourceRow);
  EXPECT_EQ(System().GetPPCState().pc, TEST_ADDRESS + 4);
}

TEST_F(PPCSteppingTest, SourceOverRunsCallToCallerBeforeLeavingRow)
{
  constexpr std::array<u8, 12> caller{
      {0x48, 0x00, 0x00, 0x41, 0x60, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00}};
  constexpr std::array<u8, 4> callee{{0x4e, 0x80, 0x00, 0x20}};
  System().GetMemory().CopyToEmu(TEST_ADDRESS, caller.data(), caller.size());
  System().GetMemory().CopyToEmu(TEST_ADDRESS + 0x40, callee.data(), callee.size());
  auto& symbols = System().GetPPCSymbolDB();
  const u32 caller_file = symbols.AddSourceFile("caller.c");
  const u32 callee_file = symbols.AddSourceFile("callee.c");
  symbols.AddLineEntry(TEST_ADDRESS, caller_file, 3);
  symbols.AddLineEntry(TEST_ADDRESS + 8, caller_file, 4);
  symbols.AddLineEntry(TEST_ADDRESS + 0x40, callee_file, 20);
  System().GetPPCState().pc = TEST_ADDRESS;

  Core::Debug::StepPPC(System(), Core::Debug::PPCStepMode::Over,
                       Core::Debug::PPCStepGranularity::SourceRow);
  EXPECT_EQ(System().GetPPCState().pc, TEST_ADDRESS + 8);
}

TEST_F(PPCSteppingTest, MissingSourceRowFallsBackByRequestedMode)
{
  constexpr std::array<u8, 8> caller{{0x48, 0x00, 0x00, 0x41, 0x60, 0x00, 0x00, 0x00}};
  constexpr std::array<u8, 4> callee{{0x4e, 0x80, 0x00, 0x20}};
  System().GetMemory().CopyToEmu(TEST_ADDRESS, caller.data(), caller.size());
  System().GetMemory().CopyToEmu(TEST_ADDRESS + 0x40, callee.data(), callee.size());

  System().GetPPCState().pc = TEST_ADDRESS;
  Core::Debug::StepPPC(System(), Core::Debug::PPCStepMode::Into,
                       Core::Debug::PPCStepGranularity::SourceRow);
  EXPECT_EQ(System().GetPPCState().pc, TEST_ADDRESS + 0x40);

  System().GetPPCState().pc = TEST_ADDRESS;
  Core::Debug::StepPPC(System(), Core::Debug::PPCStepMode::Over,
                       Core::Debug::PPCStepGranularity::SourceRow);
  EXPECT_EQ(System().GetPPCState().pc, TEST_ADDRESS + 4);
}

TEST_F(PPCSteppingTest, SourceStepHonorsCancellationBeforeAdvancing)
{
  System().GetMemory().CopyToEmu(TEST_ADDRESS, NOP.data(), NOP.size());
  System().GetPPCState().pc = TEST_ADDRESS;
  std::atomic<bool> cancelled{true};
  Core::Debug::PPCStepOptions options;
  options.cancelled = &cancelled;

  Core::Debug::StepPPC(System(), Core::Debug::PPCStepMode::Into,
                       Core::Debug::PPCStepGranularity::SourceRow, options);
  EXPECT_EQ(System().GetPPCState().pc, TEST_ADDRESS);
}

TEST_F(PPCSteppingTest, InstructionOutCanIgnoreCodeBreakpointAtCurrentPc)
{
  constexpr std::array<u8, 4> return_instruction{{0x4e, 0x80, 0x00, 0x20}};
  System().GetMemory().CopyToEmu(TEST_ADDRESS, return_instruction.data(),
                                 return_instruction.size());
  auto& state = System().GetPPCState();
  state.pc = TEST_ADDRESS;
  LR(state) = TEST_ADDRESS + 0x100;
  (void)System().GetPowerPC().GetBreakPoints().Add(TEST_ADDRESS);
  Core::Debug::PPCStepOptions options;
  options.ignore_current_code_breakpoint = true;

  Core::Debug::StepPPC(System(), Core::Debug::PPCStepMode::Out,
                       Core::Debug::PPCStepGranularity::Instruction, options);
  EXPECT_EQ(state.pc, TEST_ADDRESS + 0x100);
}

TEST_F(PPCSteppingTest, StepOutCancellationIsNotReportedAsCompletion)
{
  System().GetMemory().CopyToEmu(TEST_ADDRESS, NOP.data(), NOP.size());
  System().GetPPCState().pc = TEST_ADDRESS;
  std::atomic<bool> cancelled{true};
  Core::Debug::PPCStepOptions options;
  options.cancelled = &cancelled;

  EXPECT_EQ(Core::Debug::StepPPC(System(), Core::Debug::PPCStepMode::Out,
                                 Core::Debug::PPCStepGranularity::Instruction, options),
            Core::Debug::PPCStepResult::NotStepped);
  EXPECT_EQ(System().GetPPCState().pc, TEST_ADDRESS);
}

TEST_F(PPCSteppingTest, RawBreakPublishesOnlyOnRunningToSteppingTransition)
{
  auto& cpu = System().GetCPU();
  std::vector<std::shared_ptr<const Core::Debug::ExecutionEvent>> events;
  const auto client = cpu.GetExecutionState().RegisterClient(
      [&](std::shared_ptr<const Core::Debug::ExecutionEvent> event) {
        events.emplace_back(std::move(event));
        cpu.SetStepping(true);
      });
  System().GetPPCState().pc = TEST_ADDRESS;
  cpu.SetStepping(false);

  cpu.Break();
  cpu.Break();

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events.front()->stop_cause, Core::Debug::ExecutionStopCause::Unknown);
  EXPECT_EQ(events.front()->pc, TEST_ADDRESS);
  cpu.GetExecutionState().UnregisterClient(client);
}

TEST_F(PPCSteppingTest, BreakWithoutDebugStopTransitionsWithoutPublishing)
{
  auto& cpu = System().GetCPU();
  std::vector<std::shared_ptr<const Core::Debug::ExecutionEvent>> events;
  const auto client = cpu.GetExecutionState().RegisterClient(
      [&](std::shared_ptr<const Core::Debug::ExecutionEvent> event) {
        events.emplace_back(std::move(event));
      });
  cpu.SetStepping(false);

  cpu.BreakWithoutDebugStop();

  EXPECT_TRUE(cpu.IsStepping());
  EXPECT_TRUE(events.empty());
  cpu.GetExecutionState().UnregisterClient(client);
}

TEST_F(PPCSteppingTest, ClassifiedBreakPublishesWhileAlreadyStepping)
{
  auto& cpu = System().GetCPU();
  std::vector<std::shared_ptr<const Core::Debug::ExecutionEvent>> events;
  const auto client = cpu.GetExecutionState().RegisterClient(
      [&](std::shared_ptr<const Core::Debug::ExecutionEvent> event) {
        events.emplace_back(std::move(event));
      });

  cpu.Break({.cause = Core::Debug::ExecutionStopCause::CodeBreakpoint,
             .pc = TEST_ADDRESS,
             .code_breakpoint_address = TEST_ADDRESS});
  cpu.Break({.cause = Core::Debug::ExecutionStopCause::Exception,
             .pc = TEST_ADDRESS + 4,
             .exceptions = 1});

  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0]->stop_cause, Core::Debug::ExecutionStopCause::CodeBreakpoint);
  EXPECT_EQ(events[1]->stop_cause, Core::Debug::ExecutionStopCause::Exception);
  cpu.GetExecutionState().UnregisterClient(client);
}

TEST_F(PPCSteppingTest, CodeBreakpointPublishesExactlyOneClassifiedStop)
{
  auto& cpu = System().GetCPU();
  std::vector<std::shared_ptr<const Core::Debug::ExecutionEvent>> events;
  const auto client = cpu.GetExecutionState().RegisterClient(
      [&](std::shared_ptr<const Core::Debug::ExecutionEvent> event) {
        events.emplace_back(std::move(event));
      });
  System().GetPPCState().pc = TEST_ADDRESS;
  ASSERT_TRUE(System().GetPowerPC().GetBreakPoints().Add(TEST_ADDRESS));

  EXPECT_TRUE(System().GetPowerPC().CheckAndHandleBreakPoints());

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events.front()->stop_cause, Core::Debug::ExecutionStopCause::CodeBreakpoint);
  EXPECT_EQ(events.front()->code_breakpoint_address, TEST_ADDRESS);
  cpu.GetExecutionState().UnregisterClient(client);
}
}  // namespace
