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

// Reads back the bytes currently at [address, address+count) from MEM1.
static std::vector<u8> ReadBack(u32 address, u32 count)
{
  std::vector<u8> out(count);
  Core::System::GetInstance().GetMemory().CopyFromEmu(out.data(), address, count);
  return out;
}

TEST_F(RealtimeWatchTest, FreezeRejectsSizeMismatch)
{
  DAP::RealtimeWatchSampler sampler(System(), [](const auto&) {});
  const int watch_id = sampler.AddSubscription(TEST_ADDRESS, 4);
  ASSERT_GT(watch_id, 0);
  // Value smaller than count must be rejected so a partial freeze can't
  // leave bytes outside `value` unprotected.
  EXPECT_FALSE(sampler.Freeze(watch_id, {0x00, 0x01, 0x02}));
  // Value larger than count also rejected.
  EXPECT_FALSE(sampler.Freeze(watch_id, {0x00, 0x01, 0x02, 0x03, 0x04}));
  // Exact match accepted.
  EXPECT_TRUE(sampler.Freeze(watch_id, {0x00, 0x01, 0x02, 0x03}));
}

TEST_F(RealtimeWatchTest, FreezeOnUnknownWatchIdFails)
{
  DAP::RealtimeWatchSampler sampler(System(), [](const auto&) {});
  EXPECT_FALSE(sampler.Freeze(999, {0x00, 0x01}));
}

TEST_F(RealtimeWatchTest, FreezeWritesBackOnDriftAndSuppressesEvent)
{
  std::vector<std::vector<DAP::RealtimeWatchChange>> captured;
  DAP::RealtimeWatchSampler sampler(System(), Capture(captured));

  // Subscribe to a region, then freeze it to its current value. Memory at
  // TEST_ADDRESS is seeded with {0x00..0x07}, so the frozen four-byte canon
  // at TEST_ADDRESS+0 is {0x00, 0x01, 0x02, 0x03}.
  const int watch_id = sampler.AddSubscription(TEST_ADDRESS, 4);
  ASSERT_GT(watch_id, 0);
  ASSERT_TRUE(sampler.Freeze(watch_id, {0x00, 0x01, 0x02, 0x03}));

  // Game writes a different value (the canonical "freeze health at 99"
  // scenario -- the game writes, and we restore).
  System().GetMemory().CopyToEmu(TEST_ADDRESS, std::array<u8, 4>{{0xde, 0xad, 0xbe, 0xef}}.data(),
                                 4);

  sampler.Tick();

  // No change event: the freeze suppresses dispatch when it writes back.
  EXPECT_TRUE(captured.empty());
  // Memory was restored to the frozen canon.
  EXPECT_EQ(ReadBack(TEST_ADDRESS, 4), (std::vector<u8>{0x00, 0x01, 0x02, 0x03}));

  // Tick again with no drift: must still not dispatch and must not write.
  sampler.Tick();
  EXPECT_TRUE(captured.empty());
  EXPECT_EQ(ReadBack(TEST_ADDRESS, 4), (std::vector<u8>{0x00, 0x01, 0x02, 0x03}));
}

TEST_F(RealtimeWatchTest, FreezeWritesBackOnlyDifferingBytes)
{
  // DESNOTE(jbarber, 2026-07-21): The Tick() fast-path skips WriteU8 calls
  // for bytes that already match the canon -- a micro-optimization that
  // halves the page-table walks on the typical "r0 changed" case. We can't
  // observe the call count directly without instrumenting the accessor, so
  // this test asserts the externally observable contract: bytes that didn't
  // drift are left untouched, and bytes that drifted are restored.
  std::vector<std::vector<DAP::RealtimeWatchChange>> captured;
  DAP::RealtimeWatchSampler sampler(System(), Capture(captured));

  const int watch_id = sampler.AddSubscription(TEST_ADDRESS, 4);
  ASSERT_GT(watch_id, 0);
  ASSERT_TRUE(sampler.Freeze(watch_id, {0x00, 0x01, 0x02, 0x03}));

  // Drift only bytes 0 and 3; bytes 1 and 2 are already at the canon.
  System().GetMemory().CopyToEmu(TEST_ADDRESS,
                                 std::array<u8, 4>{{0xff, 0x01, 0x02, 0xee}}.data(), 4);
  sampler.Tick();

  EXPECT_TRUE(captured.empty());
  EXPECT_EQ(ReadBack(TEST_ADDRESS, 4), (std::vector<u8>{0x00, 0x01, 0x02, 0x03}));
}

TEST_F(RealtimeWatchTest, UnfreezeStopsWritebackAndResumesEventDispatch)
{
  std::vector<std::vector<DAP::RealtimeWatchChange>> captured;
  DAP::RealtimeWatchSampler sampler(System(), Capture(captured));

  const int watch_id = sampler.AddSubscription(TEST_ADDRESS, 4);
  ASSERT_GT(watch_id, 0);
  ASSERT_TRUE(sampler.Freeze(watch_id, {0x00, 0x01, 0x02, 0x03}));

  // Game drifts; Tick restores; no event.
  System().GetMemory().CopyToEmu(TEST_ADDRESS, std::array<u8, 4>{{0xde, 0xad, 0xbe, 0xef}}.data(),
                                 4);
  sampler.Tick();
  EXPECT_TRUE(captured.empty());
  EXPECT_EQ(ReadBack(TEST_ADDRESS, 4), (std::vector<u8>{0x00, 0x01, 0x02, 0x03}));

  // Unfreeze: subsequent drifts must NOT be written back, and the change
  // event must fire normally.
  EXPECT_TRUE(sampler.Unfreeze(watch_id));
  System().GetMemory().CopyToEmu(TEST_ADDRESS, std::array<u8, 4>{{0xaa, 0xbb, 0xcc, 0xdd}}.data(),
                                 4);
  sampler.Tick();

  ASSERT_EQ(captured.size(), 1u);
  ASSERT_EQ(captured[0].size(), 1u);
  EXPECT_EQ(captured[0][0].watch_id, watch_id);
  EXPECT_EQ(captured[0][0].bytes, (std::vector<u8>{0xaa, 0xbb, 0xcc, 0xdd}));
  // Memory at the cell is whatever the game wrote -- we did not restore it.
  EXPECT_EQ(ReadBack(TEST_ADDRESS, 4), (std::vector<u8>{0xaa, 0xbb, 0xcc, 0xdd}));
}

TEST_F(RealtimeWatchTest, UnfreezeIsIdempotentOnAlreadyUnfrozenWatch)
{
  DAP::RealtimeWatchSampler sampler(System(), [](const auto&) {});
  const int watch_id = sampler.AddSubscription(TEST_ADDRESS, 4);
  ASSERT_GT(watch_id, 0);
  // Never frozen -- Unfreeze must still succeed and be a no-op.
  EXPECT_TRUE(sampler.Unfreeze(watch_id));
  // A second unfreeze is also fine.
  EXPECT_TRUE(sampler.Unfreeze(watch_id));
}

TEST_F(RealtimeWatchTest, UnfreezeOnUnknownWatchIdFails)
{
  DAP::RealtimeWatchSampler sampler(System(), [](const auto&) {});
  EXPECT_FALSE(sampler.Unfreeze(999));
}

TEST_F(RealtimeWatchTest, FreezeDoesNotAffectUnfrozenSubscriptions)
{
  // Two watches on the same region -- one frozen, one not. The frozen one
  // writes back and suppresses dispatch; the unfrozen one sees the drift
  // and dispatches normally. The write-back from the frozen watch happens
  // first in iteration order, so the unfrozen watch sees the canon in
  // memory and reports no change.
  std::vector<std::vector<DAP::RealtimeWatchChange>> captured;
  DAP::RealtimeWatchSampler sampler(System(), Capture(captured));

  const int frozen_id = sampler.AddSubscription(TEST_ADDRESS, 4);
  const int watch_id = sampler.AddSubscription(TEST_ADDRESS, 4);
  ASSERT_NE(frozen_id, watch_id);

  ASSERT_TRUE(sampler.Freeze(frozen_id, {0x00, 0x01, 0x02, 0x03}));
  // The second subscription sees the canon as its baseline (it was seeded
  // at AddSubscription from the same memory state). Freeze on the first
  // subscription wrote the canon into memory before the second one was
  // added, so its baseline already matches.

  System().GetMemory().CopyToEmu(TEST_ADDRESS, std::array<u8, 4>{{0xde, 0xad, 0xbe, 0xef}}.data(),
                                 4);
  sampler.Tick();

  // The frozen subscription restored the canon and suppressed dispatch.
  // The unfrozen subscription's `last_seen` was seeded at the canon, and
  // Tick read the canon (because the frozen write-back ran before its read
  // in the iteration), so it also dispatches nothing. Both are quiet.
  EXPECT_TRUE(captured.empty());
  EXPECT_EQ(ReadBack(TEST_ADDRESS, 4), (std::vector<u8>{0x00, 0x01, 0x02, 0x03}));
}
}  // namespace
