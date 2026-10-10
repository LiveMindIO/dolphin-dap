// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
constexpr u32 FIRST_ADDRESS = 0x3100;
constexpr u32 SECOND_ADDRESS = 0x3200;

class MemoryBreakpointRegistryTest : public ::testing::Test
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
    Registry().Clear();
  }

  void TearDown() override
  {
    auto& system = Core::System::GetInstance();
    for (const auto client : m_clients)
      Registry().UnregisterClient(client);
    Registry().Clear();
    system.GetCPU().Shutdown();
    AddressSpace::Shutdown();
    system.GetMemory().Shutdown();
    system.GetCoreTiming().Shutdown();
    Core::UndeclareAsCPUThread();
  }

  static MemChecks& Registry() { return Core::System::GetInstance().GetPowerPC().GetMemChecks(); }

  MemChecks::ClientId Register(MemChecks::EventCallback callback = {})
  {
    const auto client = Registry().RegisterClient(std::move(callback));
    m_clients.push_back(client);
    return client;
  }

  static MemChecks::MemoryBreakpoint Watch(u32 address)
  {
    return {.start_address = address,
            .end_address = address + 3,
            .is_break_on_write = true,
            .break_on_hit = true};
  }

  std::vector<MemChecks::ClientId> m_clients;
};

TEST_F(MemoryBreakpointRegistryTest, ClientCollectionsReplaceAndDisconnectIndependently)
{
  const auto first = Register();
  const auto second = Register();
  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(first, {Watch(FIRST_ADDRESS)}));
  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(second, {Watch(SECOND_ADDRESS)}));

  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(first, {}));
  EXPECT_EQ(Registry().GetMemCheck(FIRST_ADDRESS), nullptr);
  EXPECT_NE(Registry().GetMemCheck(SECOND_ADDRESS), nullptr);

  Registry().UnregisterClient(second);
  EXPECT_EQ(Registry().GetMemCheck(SECOND_ADDRESS), nullptr);
}

TEST_F(MemoryBreakpointRegistryTest, IncompatibleOverlapIsRejectedAtomically)
{
  const auto first = Register();
  const auto second = Register();
  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(first, {Watch(FIRST_ADDRESS)}));
  const auto before = Registry().GetSnapshot();

  auto conflict = Watch(FIRST_ADDRESS + 2);
  conflict.is_break_on_read = true;
  const auto result =
      Registry().ReplaceClientBreakpoints(second, {conflict, Watch(SECOND_ADDRESS)});
  ASSERT_FALSE(result);
  EXPECT_EQ(Registry().GetRevision(), before->revision);
  EXPECT_EQ(Registry().GetMemCheck(SECOND_ADDRESS), nullptr);
  EXPECT_EQ(before->mem_checks.size(), Registry().GetSnapshot()->mem_checks.size());
}

TEST_F(MemoryBreakpointRegistryTest, IdenticalClaimsCoalesceDeterministically)
{
  const auto first = Register();
  const auto second = Register();
  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(second, {Watch(FIRST_ADDRESS)}));
  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(first, {Watch(FIRST_ADDRESS)}));
  ASSERT_EQ(Registry().GetSnapshot()->mem_checks.size(), 1u);
  Registry().UnregisterClient(first);
  EXPECT_NE(Registry().GetMemCheck(FIRST_ADDRESS), nullptr);
}

TEST_F(MemoryBreakpointRegistryTest, GuiMutationFansOutAndClientCanReAdd)
{
  std::vector<std::shared_ptr<const MemChecks::Event>> events;
  const auto client = Register(
      [&](std::shared_ptr<const MemChecks::Event> event) { events.push_back(std::move(event)); });
  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(client, {Watch(FIRST_ADDRESS)}));
  events.clear();

  EXPECT_TRUE(Registry().ToggleEnable(FIRST_ADDRESS));
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events.back()->changes.front().reason, MemChecks::ChangeReason::Changed);
  EXPECT_FALSE(Registry().GetMemCheck(FIRST_ADDRESS)->is_enabled);
  Registry().Remove(FIRST_ADDRESS);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events.back()->changes.front().reason, MemChecks::ChangeReason::Removed);

  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(client, {Watch(FIRST_ADDRESS)}));
  EXPECT_NE(Registry().GetMemCheck(FIRST_ADDRESS), nullptr);
}

TEST_F(MemoryBreakpointRegistryTest, FreezesArePrivateAndExcludedFromEventsAndPersistence)
{
  std::vector<std::shared_ptr<const MemChecks::Event>> events;
  const auto first = Register(
      [&](std::shared_ptr<const MemChecks::Event> event) { events.push_back(std::move(event)); });
  const auto second = Register();
  const auto first_freeze = Registry().AddClientFreeze(first, FIRST_ADDRESS, FIRST_ADDRESS + 3);
  const auto second_freeze = Registry().AddClientFreeze(second, SECOND_ADDRESS, SECOND_ADDRESS + 3);
  ASSERT_TRUE(first_freeze);
  ASSERT_TRUE(second_freeze);
  EXPECT_TRUE(events.empty());
  EXPECT_TRUE(Registry().GetStrings().empty());
  EXPECT_TRUE(Registry().GetMemChecks()->empty());
  EXPECT_EQ(Registry().GetMemCheck(FIRST_ADDRESS), nullptr);
  EXPECT_EQ(Registry().GetMemCheck(SECOND_ADDRESS), nullptr);
  ASSERT_EQ(Registry().GetSnapshot()->private_freeze_ranges.size(), 2u);
  EXPECT_FALSE(Registry().ToggleEnable(FIRST_ADDRESS));
  EXPECT_FALSE(Registry().Remove(FIRST_ADDRESS));
  Registry().Clear();
  EXPECT_TRUE(Registry().GetSnapshot()->OverlapsPrivateFreeze(FIRST_ADDRESS, 4));
  EXPECT_TRUE(Registry().GetSnapshot()->OverlapsPrivateFreeze(SECOND_ADDRESS, 4));

  Registry().ClearClientFreezes(first);
  EXPECT_EQ(Registry().GetMemCheck(FIRST_ADDRESS), nullptr);
  EXPECT_FALSE(Registry().GetSnapshot()->OverlapsPrivateFreeze(FIRST_ADDRESS, 4));
  EXPECT_TRUE(Registry().GetSnapshot()->OverlapsPrivateFreeze(SECOND_ADDRESS, 4));
  EXPECT_TRUE(events.empty());
}

TEST_F(MemoryBreakpointRegistryTest, GuiEditAndClearDoNotMutatePrivateFreezes)
{
  const auto first = Register();
  const auto second = Register();
  ASSERT_TRUE(Registry().AddClientFreeze(first, FIRST_ADDRESS, FIRST_ADDRESS + 3));
  ASSERT_TRUE(Registry().AddClientFreeze(second, SECOND_ADDRESS, SECOND_ADDRESS + 3));

  TMemCheck gui;
  gui.start_address = FIRST_ADDRESS;
  gui.end_address = FIRST_ADDRESS + 3;
  gui.is_ranged = true;
  gui.is_break_on_write = true;
  gui.break_on_hit = true;
  Registry().Add(std::move(gui));
  ASSERT_NE(Registry().GetMemCheck(FIRST_ADDRESS), nullptr);
  TMemCheck replacement;
  replacement.start_address = FIRST_ADDRESS;
  replacement.end_address = FIRST_ADDRESS + 3;
  replacement.is_ranged = true;
  replacement.is_break_on_write = true;
  replacement.log_on_hit = true;
  replacement.break_on_hit = true;
  Registry().Replace(FIRST_ADDRESS, std::move(replacement));
  ASSERT_TRUE(Registry().GetMemCheck(FIRST_ADDRESS)->log_on_hit);
  Registry().Clear();

  EXPECT_TRUE(Registry().GetMemChecks()->empty());
  EXPECT_TRUE(Registry().GetSnapshot()->OverlapsPrivateFreeze(FIRST_ADDRESS, 4));
  EXPECT_TRUE(Registry().GetSnapshot()->OverlapsPrivateFreeze(SECOND_ADDRESS, 4));
}

TEST_F(MemoryBreakpointRegistryTest, InvalidFreezeRangeIsRejectedAtomically)
{
  const auto client = Register();
  const auto first = Registry().AddClientFreeze(client, FIRST_ADDRESS, FIRST_ADDRESS);
  ASSERT_TRUE(first);
  const auto before = Registry().GetSnapshot();
  const auto invalid = Registry().AddClientFreeze(client, SECOND_ADDRESS, FIRST_ADDRESS);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), "freeze range start exceeds end");
  EXPECT_EQ(Registry().GetSnapshot(), before);

  const auto valid = Registry().AddClientFreeze(client, SECOND_ADDRESS, SECOND_ADDRESS);
  ASSERT_TRUE(valid);
  EXPECT_EQ(*valid, *first + 1);
}

TEST_F(MemoryBreakpointRegistryTest, ComplementaryReadAndWriteClaimsCompose)
{
  const auto first = Register();
  const auto second = Register();
  auto read = Watch(FIRST_ADDRESS);
  read.is_break_on_write = false;
  read.is_break_on_read = true;
  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(first, {read}));
  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(second, {Watch(FIRST_ADDRESS)}));

  const auto effective = Registry().GetMemCheck(FIRST_ADDRESS);
  ASSERT_NE(effective, nullptr);
  EXPECT_TRUE(effective->is_break_on_read);
  EXPECT_TRUE(effective->is_break_on_write);
  EXPECT_TRUE(effective->break_on_hit);
  EXPECT_FALSE(effective->condition);
}

TEST_F(MemoryBreakpointRegistryTest, CompatibleConditionalClaimsAggregateAndEvaluate)
{
  const auto first = Register();
  const auto second = Register();
  auto false_claim = Watch(FIRST_ADDRESS);
  false_claim.is_break_on_read = true;
  false_claim.condition = "0";
  auto true_claim = false_claim;
  true_claim.condition = "1";
  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(first, {false_claim}));
  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(second, {true_claim}));

  const auto effective = Registry().GetMemCheck(FIRST_ADDRESS);
  ASSERT_NE(effective, nullptr);
  ASSERT_TRUE(effective->condition);
  EXPECT_TRUE(effective->Action(Core::System::GetInstance(), 0, FIRST_ADDRESS, false, 4, 0));
  EXPECT_TRUE(effective->Action(Core::System::GetInstance(), 0, FIRST_ADDRESS, true, 4, 0));
  EXPECT_EQ(effective->GetNumHits(), 2u);
}

TEST_F(MemoryBreakpointRegistryTest, IncompatibleAccessActionAndConditionPoliciesAreRejected)
{
  const auto first = Register();
  const auto second = Register();
  auto read_break = Watch(FIRST_ADDRESS);
  read_break.is_break_on_write = false;
  read_break.is_break_on_read = true;
  read_break.condition = "1";
  auto write_log = Watch(FIRST_ADDRESS);
  write_log.break_on_hit = false;
  write_log.log_on_hit = true;
  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(first, {read_break}));
  EXPECT_FALSE(Registry().ReplaceClientBreakpoints(second, {write_log}));

  auto write_break = Watch(FIRST_ADDRESS);
  write_break.condition = "0";
  EXPECT_FALSE(Registry().ReplaceClientBreakpoints(second, {write_break}));
  EXPECT_TRUE(Registry().GetMemCheck(FIRST_ADDRESS)->is_break_on_read);
  EXPECT_FALSE(Registry().GetMemCheck(FIRST_ADDRESS)->is_break_on_write);
}

TEST_F(MemoryBreakpointRegistryTest, DisabledClaimsDoNotAffectComposition)
{
  const auto first = Register();
  const auto second = Register();
  auto active = Watch(FIRST_ADDRESS);
  auto disabled = Watch(FIRST_ADDRESS + 2);
  disabled.is_enabled = false;
  disabled.is_break_on_read = true;
  disabled.is_break_on_write = false;
  disabled.log_on_hit = true;
  disabled.break_on_hit = false;
  disabled.condition = "invalid condition";
  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(first, {active}));
  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(second, {disabled}));

  const auto effective = Registry().GetMemCheck(FIRST_ADDRESS);
  ASSERT_NE(effective, nullptr);
  EXPECT_FALSE(effective->is_break_on_read);
  EXPECT_TRUE(effective->is_break_on_write);
  EXPECT_FALSE(effective->log_on_hit);
  EXPECT_TRUE(effective->break_on_hit);
}

TEST_F(MemoryBreakpointRegistryTest, HitCountMatchesActionsAndResetsOnlyWhenSemanticsChange)
{
  const auto client = Register();
  auto watch = Watch(FIRST_ADDRESS);
  watch.log_on_hit = true;
  watch.break_on_hit = false;
  watch.condition = "1";
  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(client, {watch}));
  auto effective = Registry().GetMemCheck(FIRST_ADDRESS);
  ASSERT_NE(effective, nullptr);
  EXPECT_FALSE(effective->Action(Core::System::GetInstance(), 0, FIRST_ADDRESS, true, 4, 0));
  EXPECT_EQ(effective->GetNumHits(), 1u);
  EXPECT_FALSE(effective->Action(Core::System::GetInstance(), 0, FIRST_ADDRESS, false, 4, 0));
  EXPECT_EQ(effective->GetNumHits(), 1u);

  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(client, {watch}));
  EXPECT_EQ(Registry().GetMemCheck(FIRST_ADDRESS)->GetNumHits(), 1u);
  watch.condition = "0";
  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(client, {watch}));
  effective = Registry().GetMemCheck(FIRST_ADDRESS);
  EXPECT_EQ(effective->GetNumHits(), 0u);
  EXPECT_FALSE(effective->Action(Core::System::GetInstance(), 0, FIRST_ADDRESS, true, 4, 0));
  EXPECT_EQ(effective->GetNumHits(), 0u);
}

TEST_F(MemoryBreakpointRegistryTest, HitCountIsAtomicAcrossConcurrentActions)
{
  const auto client = Register();
  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(client, {Watch(FIRST_ADDRESS)}));
  const auto effective = Registry().GetMemCheck(FIRST_ADDRESS);
  ASSERT_NE(effective, nullptr);
  std::vector<std::thread> workers;
  for (size_t thread = 0; thread < 4; ++thread)
  {
    workers.emplace_back([&] {
      for (size_t hit = 0; hit < 1000; ++hit)
        (void)effective->Action(Core::System::GetInstance(), 0, FIRST_ADDRESS, true, 4, 0);
    });
  }
  for (std::thread& worker : workers)
    worker.join();
  EXPECT_EQ(effective->GetNumHits(), 4000u);
}

TEST_F(MemoryBreakpointRegistryTest, ReentrantCallbackQueuesOrderedMutation)
{
  MemChecks::ClientId client = 0;
  std::vector<u64> revisions;
  client = Register([&](const std::shared_ptr<const MemChecks::Event>& event) {
    revisions.push_back(event->revision);
    if (revisions.size() == 1)
    {
      EXPECT_TRUE(Registry().ReplaceClientBreakpoints(client, {Watch(SECOND_ADDRESS)}));
    }
  });

  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(client, {Watch(FIRST_ADDRESS)}));
  ASSERT_EQ(revisions.size(), 2u);
  EXPECT_LT(revisions[0], revisions[1]);
  EXPECT_EQ(Registry().GetMemCheck(FIRST_ADDRESS), nullptr);
  EXPECT_NE(Registry().GetMemCheck(SECOND_ADDRESS), nullptr);
}

TEST_F(MemoryBreakpointRegistryTest, CallbackCanUnregisterItself)
{
  MemChecks::ClientId client = 0;
  size_t callbacks = 0;
  client = Register([&](const std::shared_ptr<const MemChecks::Event>&) {
    ++callbacks;
    Registry().UnregisterClient(client);
  });

  ASSERT_TRUE(Registry().ReplaceClientBreakpoints(client, {Watch(FIRST_ADDRESS)}));
  EXPECT_EQ(callbacks, 1u);
  EXPECT_EQ(Registry().GetMemCheck(FIRST_ADDRESS), nullptr);
}

TEST_F(MemoryBreakpointRegistryTest, UnregisterWaitsForInFlightCallback)
{
  using namespace std::chrono_literals;
  std::promise<void> entered;
  std::shared_future<void> release = std::async(std::launch::deferred, [] {}).share();
  std::promise<void> release_promise;
  release = release_promise.get_future().share();
  const auto client = Register([&](const std::shared_ptr<const MemChecks::Event>&) {
    entered.set_value();
    release.wait();
  });

  std::thread writer(
      [&] { EXPECT_TRUE(Registry().ReplaceClientBreakpoints(client, {Watch(FIRST_ADDRESS)})); });
  entered.get_future().wait();
  auto unregister = std::async(std::launch::async, [&] { Registry().UnregisterClient(client); });
  EXPECT_EQ(unregister.wait_for(50ms), std::future_status::timeout);
  release_promise.set_value();
  writer.join();
  EXPECT_EQ(unregister.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(Registry().GetMemCheck(FIRST_ADDRESS), nullptr);
}

TEST_F(MemoryBreakpointRegistryTest, SnapshotsRemainValidDuringConcurrentReplacement)
{
  const auto client = Register();
  std::atomic<bool> done = false;
  std::thread writer([&] {
    for (u32 i = 0; i < 1000; ++i)
      EXPECT_TRUE(
          Registry().ReplaceClientBreakpoints(client, {Watch(FIRST_ADDRESS + (i % 2) * 8)}));
    done.store(true);
  });

  std::vector<std::shared_ptr<const MemChecks::Snapshot>> retained;
  while (!done.load())
  {
    const auto snapshot = Registry().GetSnapshot();
    retained.push_back(snapshot);
    EXPECT_LE(snapshot->mem_checks.size(), 1u);
  }
  writer.join();
  for (const auto& snapshot : retained)
    EXPECT_LE(snapshot->mem_checks.size(), 1u);
}
}  // namespace
