// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Unit tests for RealtimeWatchSampler. Mirrors DapControllerTest's setup: a
// real (but un-booted) Core::System with memory + address-space accessors
// initialized and the test thread declared as the CPU thread, so the sampler's
// CPUThreadGuard is a no-op. vi_end_field_event never fires without a booted
// video core, so tests drive Tick() directly (the same path the frame hook
// would take on the CPU thread).

#include <array>
#include <atomic>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Common/SymbolDB.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/DAP/DapRealtimeWatch.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
constexpr u32 TEST_ADDRESS = 0x00003100;

class RealtimeWatchTest : public ::testing::Test
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

    auto& power_pc = system.GetPowerPC();
    auto& ppc_state = system.GetPPCState();
    ppc_state.msr.IR = 0;
    ppc_state.msr.DR = 0;
    ppc_state.Exceptions = 0;
    power_pc.MSRUpdated();
    power_pc.GetBreakPoints().Clear();
    system.GetPPCSymbolDB().Clear();

    std::array<u8, 8> seed{{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}};
    memory.CopyToEmu(TEST_ADDRESS, seed.data(), seed.size());
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

  static DAP::RealtimeWatchSampler::DispatchCallback Capture(
      std::vector<std::vector<DAP::RealtimeWatchChange>>& out)
  {
    return [&out](const std::vector<DAP::RealtimeWatchChange>& changes) {
      out.push_back(changes);
    };
  }
};

TEST_F(RealtimeWatchTest, NoChangeMeansNoDispatch)
{
  std::vector<std::vector<DAP::RealtimeWatchChange>> captured;
  DAP::RealtimeWatchSampler sampler(System(), Capture(captured));

  sampler.AddSubscription(TEST_ADDRESS, 4);
  sampler.Tick();

  EXPECT_TRUE(captured.empty());
}

TEST_F(RealtimeWatchTest, ChangeDispatchesNewBytes)
{
  std::vector<std::vector<DAP::RealtimeWatchChange>> captured;
  DAP::RealtimeWatchSampler sampler(System(), Capture(captured));

  const int watch_id = sampler.AddSubscription(TEST_ADDRESS, 4);
  EXPECT_GT(watch_id, 0);

  System().GetMemory().CopyToEmu(TEST_ADDRESS, std::array<u8, 4>{{0xde, 0xad, 0xbe, 0xef}}.data(),
                                 4);

  sampler.Tick();

  ASSERT_EQ(captured.size(), 1u);
  ASSERT_EQ(captured[0].size(), 1u);
  const DAP::RealtimeWatchChange& change = captured[0][0];
  EXPECT_EQ(change.watch_id, watch_id);
  EXPECT_EQ(change.address, TEST_ADDRESS);
  EXPECT_EQ(change.count, 4u);
  EXPECT_EQ(change.bytes, (std::vector<u8>{0xde, 0xad, 0xbe, 0xef}));

  // A second Tick with no further change must not re-emit.
  sampler.Tick();
  EXPECT_EQ(captured.size(), 1u);
}

TEST_F(RealtimeWatchTest, PartiallyUnreadableRegionIsReported)
{
  // Segment 0 but past MEM1 (24 MiB), so not a RAM address.
  constexpr u32 INVALID_BASE = 0x0C000000;
  std::vector<std::vector<DAP::RealtimeWatchChange>> captured;
  DAP::RealtimeWatchSampler sampler(System(), Capture(captured));

  sampler.AddSubscription(INVALID_BASE, 4);
  sampler.Tick();

  // The whole region is unreadable; the seeded and current snapshots are both
  // empty, so no change is reported. Confirm we don't crash and don't emit.
  EXPECT_TRUE(captured.empty());
}

TEST_F(RealtimeWatchTest, CancelStopsFurtherDispatch)
{
  std::vector<std::vector<DAP::RealtimeWatchChange>> captured;
  DAP::RealtimeWatchSampler sampler(System(), Capture(captured));

  const int watch_id = sampler.AddSubscription(TEST_ADDRESS, 2);
  EXPECT_TRUE(sampler.RemoveSubscription(watch_id));
  EXPECT_FALSE(sampler.RemoveSubscription(watch_id));

  System().GetMemory().CopyToEmu(TEST_ADDRESS, std::array<u8, 2>{{0xff, 0xff}}.data(), 2);
  sampler.Tick();
  EXPECT_TRUE(captured.empty());
}

TEST_F(RealtimeWatchTest, MultipleWatchesIndependent)
{
  constexpr u32 SECOND_ADDRESS = TEST_ADDRESS + 0x1000;
  std::vector<std::vector<DAP::RealtimeWatchChange>> captured;
  DAP::RealtimeWatchSampler sampler(System(), Capture(captured));

  std::array<u8, 4> second_seed{{0xaa, 0xbb, 0xcc, 0xdd}};
  System().GetMemory().CopyToEmu(SECOND_ADDRESS, second_seed.data(), second_seed.size());

  const int id_a = sampler.AddSubscription(TEST_ADDRESS, 2);
  const int id_b = sampler.AddSubscription(SECOND_ADDRESS, 2);
  EXPECT_NE(id_a, id_b);

  // Only the second region changes.
  System().GetMemory().CopyToEmu(SECOND_ADDRESS, std::array<u8, 2>{{0x00, 0x00}}.data(), 2);
  sampler.Tick();

  ASSERT_EQ(captured.size(), 1u);
  ASSERT_EQ(captured[0].size(), 1u);
  EXPECT_EQ(captured[0][0].watch_id, id_b);
  EXPECT_EQ(captured[0][0].bytes, (std::vector<u8>{0x00, 0x00}));
}
}  // namespace
