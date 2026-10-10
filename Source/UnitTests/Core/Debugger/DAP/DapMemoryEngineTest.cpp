// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Common/GekkoDisassembler.h"
#include "Core/Core.h"
#include "Core/Debugger/DAP/DapJson.h"
#include "Core/Debugger/DAP/DapMemoryEngine.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
constexpr u32 DATA_ADDRESS = 0x00004000;
constexpr u32 SCAN_ADDRESS = 0x80004000;

class DapMemoryEngineTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    auto& system = Core::System::GetInstance();
    system.GetMemory().Init();
    AddressSpace::Init();

    Core::DeclareAsCPUThread();
    auto& power_pc = system.GetPowerPC();
    power_pc.Reset();
    auto& ppc_state = system.GetPPCState();
    ppc_state.msr.IR = 0;
    ppc_state.msr.DR = 0;
    power_pc.MSRUpdated();
    Core::UndeclareAsCPUThread();

    m_engine =
        std::make_unique<DAP::DapMemoryEngine>(system, [this](DAP::MemoryScanTerminalEvent event) {
          std::lock_guard lock(m_event_mutex);
          m_events.emplace_back(std::move(event));
          m_event_cv.notify_all();
        });
  }

  void TearDown() override
  {
    m_engine.reset();
    AddressSpace::Shutdown();
    Core::System::GetInstance().GetMemory().Shutdown();
  }

  void WriteBytes(u32 address, std::span<const u8> bytes)
  {
    Core::System::GetInstance().GetMemory().CopyToEmu(address, bytes.data(), bytes.size());
  }

  std::optional<DAP::MemoryScanTerminalEvent> WaitForEvent(size_t index)
  {
    std::unique_lock lock(m_event_mutex);
    if (!m_event_cv.wait_for(lock, std::chrono::seconds(5),
                             [&] { return m_events.size() > index; }))
    {
      return std::nullopt;
    }
    return m_events[index];
  }

  DAP::MemoryScanStartConfig MakeConfig(DAP::MemoryScanDataType data_type, u32 start, u32 size,
                                        std::string value)
  {
    DAP::MemoryScanStartConfig config;
    config.ranges.push_back({start, start + size});
    config.data_type = data_type;
    config.filter = DAP::MemoryScanFilter::Exact;
    config.value = std::move(value);
    config.pause_during_scan = true;
    return config;
  }

  std::unique_ptr<DAP::DapMemoryEngine> m_engine;
  std::mutex m_event_mutex;
  std::condition_variable m_event_cv;
  std::vector<DAP::MemoryScanTerminalEvent> m_events;
};

TEST_F(DapMemoryEngineTest, ExactScanSupportsEveryNumericType)
{
  struct Case
  {
    DAP::MemoryScanDataType data_type;
    std::vector<u8> bytes;
    std::string value;
  };
  const std::vector<Case> cases{
      {DAP::MemoryScanDataType::U8, {0x7f}, "127"},
      {DAP::MemoryScanDataType::U16, {0x12, 0x34}, "4660"},
      {DAP::MemoryScanDataType::U32, {0xde, 0xad, 0xbe, 0xef}, "3735928559"},
      {DAP::MemoryScanDataType::U64,
       {0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01},
       "2305843009213693953"},
      {DAP::MemoryScanDataType::S8, {0xff}, "-1"},
      {DAP::MemoryScanDataType::S16, {0xff, 0xfe}, "-2"},
      {DAP::MemoryScanDataType::S32, {0xff, 0xff, 0xff, 0xfd}, "-3"},
      {DAP::MemoryScanDataType::S64, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfc}, "-4"},
      {DAP::MemoryScanDataType::F32, {0x3f, 0xc0, 0x00, 0x00}, "1.5"},
      {DAP::MemoryScanDataType::F64, {0xc0, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, "-2.25"},
  };

  for (size_t i = 0; i < cases.size(); ++i)
  {
    const Case& test = cases[i];
    WriteBytes(DATA_ADDRESS, test.bytes);
    const auto accepted = m_engine->StartScan(
        MakeConfig(test.data_type, SCAN_ADDRESS, static_cast<u32>(test.bytes.size()), test.value));
    ASSERT_TRUE(accepted.has_value());

    const auto terminal = WaitForEvent(i);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->event, "dolphin_memoryScanCompleted");
    EXPECT_EQ(terminal->result_count, 1u);

    const auto page = m_engine->GetResults(accepted->scan_id, 0, 1);
    ASSERT_TRUE(page.has_value());
    ASSERT_EQ(page->results.size(), 1u);
    EXPECT_EQ(page->results[0].address, SCAN_ADDRESS);
    EXPECT_EQ(page->results[0].raw, test.bytes);
    EXPECT_TRUE(m_engine->Dispose(accepted->scan_id));
  }
}

TEST_F(DapMemoryEngineTest, U64IncreasedByUsesExactIntegerArithmetic)
{
  const std::vector<u8> initial{0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  WriteBytes(DATA_ADDRESS, initial);
  auto config = MakeConfig(DAP::MemoryScanDataType::U64, SCAN_ADDRESS, 8, "0");
  config.filter = DAP::MemoryScanFilter::Unknown;
  config.value.reset();
  const auto accepted = m_engine->StartScan(config);
  ASSERT_TRUE(accepted.has_value());
  ASSERT_TRUE(WaitForEvent(0).has_value());

  const std::vector<u8> changed{0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
  WriteBytes(DATA_ADDRESS, changed);
  DAP::MemoryScanRefineConfig refine;
  refine.scan_id = accepted->scan_id;
  refine.filter = DAP::MemoryScanFilter::IncreasedBy;
  refine.value = "1";
  const auto refined = m_engine->RefineScan(refine);
  ASSERT_TRUE(refined.has_value());

  const auto terminal = WaitForEvent(1);
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->event, "dolphin_memoryScanCompleted");
  EXPECT_EQ(terminal->generation, 2u);
  EXPECT_EQ(terminal->result_count, 1u);
}

TEST_F(DapMemoryEngineTest, CancelledRefinementPreservesCommittedGeneration)
{
  const std::vector<u8> bytes{0xde, 0xad, 0xbe, 0xef};
  WriteBytes(DATA_ADDRESS, bytes);
  auto config = MakeConfig(DAP::MemoryScanDataType::U32, SCAN_ADDRESS, 4, "0");
  config.filter = DAP::MemoryScanFilter::Unknown;
  config.value.reset();
  const auto accepted = m_engine->StartScan(config);
  ASSERT_TRUE(accepted.has_value());
  ASSERT_TRUE(WaitForEvent(0).has_value());

  auto core_scan_lock = DAP::DapMemoryEngine::TryLockCoreScan();
  ASSERT_TRUE(core_scan_lock.owns_lock());
  DAP::MemoryScanRefineConfig refine;
  refine.scan_id = accepted->scan_id;
  refine.filter = DAP::MemoryScanFilter::Changed;
  const auto refined = m_engine->RefineScan(refine);
  ASSERT_TRUE(refined.has_value());
  ASSERT_TRUE(m_engine->Cancel(accepted->scan_id));
  core_scan_lock.unlock();

  const auto terminal = WaitForEvent(1);
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->event, "dolphin_memoryScanCancelled");
  EXPECT_EQ(terminal->generation, 1u);
  EXPECT_EQ(terminal->result_count, 1u);
  const auto status = m_engine->GetStatus(accepted->scan_id);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status->state, "cancelled");
  EXPECT_EQ(status->generation, 1u);
  EXPECT_EQ(status->result_count, 1u);
}

TEST_F(DapMemoryEngineTest, DisposeCancelsJobWaitingForCore)
{
  const std::vector<u8> bytes{0x01};
  WriteBytes(DATA_ADDRESS, bytes);
  auto core_scan_lock = DAP::DapMemoryEngine::TryLockCoreScan();
  ASSERT_TRUE(core_scan_lock.owns_lock());
  const auto accepted =
      m_engine->StartScan(MakeConfig(DAP::MemoryScanDataType::U8, SCAN_ADDRESS, 1, "1"));
  ASSERT_TRUE(accepted.has_value());
  ASSERT_TRUE(m_engine->Dispose(accepted->scan_id));
  core_scan_lock.unlock();

  const auto terminal = WaitForEvent(0);
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->event, "dolphin_memoryScanCancelled");
  EXPECT_FALSE(m_engine->GetStatus(accepted->scan_id).has_value());
}

TEST_F(DapMemoryEngineTest, AlignmentAndResultPagingUseAbsoluteAddresses)
{
  const std::vector<u8> bytes{0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x05};
  WriteBytes(DATA_ADDRESS + 1, bytes);
  auto config = MakeConfig(DAP::MemoryScanDataType::U32, SCAN_ADDRESS + 1,
                           static_cast<u32>(bytes.size()), "5");
  config.aligned = true;
  const auto accepted = m_engine->StartScan(config);
  ASSERT_TRUE(accepted.has_value());
  const auto terminal = WaitForEvent(0);
  ASSERT_TRUE(terminal.has_value());
  ASSERT_EQ(terminal->result_count, 2u);

  const auto page = m_engine->GetResults(accepted->scan_id, 1, 1);
  ASSERT_TRUE(page.has_value());
  ASSERT_EQ(page->results.size(), 1u);
  EXPECT_EQ(page->results[0].address, SCAN_ADDRESS + 8);
}

TEST_F(DapMemoryEngineTest, AdjacentRangesPreserveCrossBoundaryCandidates)
{
  const std::vector<u8> bytes{0x12, 0x34, 0x56, 0x78};
  WriteBytes(DATA_ADDRESS, bytes);
  auto config = MakeConfig(DAP::MemoryScanDataType::U32, SCAN_ADDRESS, 4, "0x12345678");
  config.ranges = {{SCAN_ADDRESS, SCAN_ADDRESS + 1}, {SCAN_ADDRESS + 1, SCAN_ADDRESS + 4}};
  const auto accepted = m_engine->StartScan(config);
  ASSERT_TRUE(accepted.has_value());
  const auto terminal = WaitForEvent(0);
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->result_count, 1u);
}

TEST_F(DapMemoryEngineTest, TerminalCallbackObservesInactiveEngine)
{
  std::mutex mutex;
  std::condition_variable condition;
  bool callback_ran = false;
  bool active_in_callback = true;
  bool refine_accepted_in_callback = true;
  m_engine = std::make_unique<DAP::DapMemoryEngine>(
      Core::System::GetInstance(), [&](DAP::MemoryScanTerminalEvent) {
        active_in_callback = m_engine->HasActiveJob();
        DAP::MemoryScanRefineConfig refine;
        refine.scan_id = 1;
        refine.filter = DAP::MemoryScanFilter::Unchanged;
        refine_accepted_in_callback = m_engine->RefineScan(refine).has_value();
        {
          std::lock_guard lock(mutex);
          callback_ran = true;
        }
        condition.notify_one();
      });
  const std::vector<u8> bytes{1};
  WriteBytes(DATA_ADDRESS, bytes);
  ASSERT_TRUE(m_engine->StartScan(MakeConfig(DAP::MemoryScanDataType::U8, SCAN_ADDRESS, 1, "1"))
                  .has_value());
  std::unique_lock lock(mutex);
  ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(5), [&] { return callback_ran; }));
  EXPECT_FALSE(active_in_callback);
  EXPECT_FALSE(refine_accepted_in_callback);
}

TEST_F(DapMemoryEngineTest, NumericFiltersRejectIgnoredArguments)
{
  const std::vector<u8> bytes{1};
  WriteBytes(DATA_ADDRESS, bytes);
  auto config = MakeConfig(DAP::MemoryScanDataType::U8, SCAN_ADDRESS, 1, "1");
  config.value2 = "2";
  EXPECT_FALSE(m_engine->StartScan(config).has_value());

  config.filter = DAP::MemoryScanFilter::Unknown;
  config.value = "1";
  config.value2.reset();
  EXPECT_FALSE(m_engine->StartScan(config).has_value());
}

TEST_F(DapMemoryEngineTest, FloatChangedTreatsNaNsAsStableValues)
{
  const std::vector<u8> nan{0x7f, 0xc0, 0x00, 0x00};
  WriteBytes(DATA_ADDRESS, nan);
  auto config = MakeConfig(DAP::MemoryScanDataType::F32, SCAN_ADDRESS, 4, "0");
  config.filter = DAP::MemoryScanFilter::Unknown;
  config.value.reset();
  const auto accepted = m_engine->StartScan(config);
  ASSERT_TRUE(accepted.has_value());
  ASSERT_TRUE(WaitForEvent(0).has_value());

  DAP::MemoryScanRefineConfig refine;
  refine.scan_id = accepted->scan_id;
  refine.filter = DAP::MemoryScanFilter::Unchanged;
  ASSERT_TRUE(m_engine->RefineScan(refine).has_value());
  const auto terminal = WaitForEvent(1);
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->result_count, 1u);
}

TEST_F(DapMemoryEngineTest, FloatParsingRejectsLocaleAndUnderflowForms)
{
  const std::vector<u8> zero{0, 0, 0, 0};
  WriteBytes(DATA_ADDRESS, zero);
  EXPECT_FALSE(m_engine->StartScan(MakeConfig(DAP::MemoryScanDataType::F32, SCAN_ADDRESS, 4, "1,5"))
                   .has_value());
  EXPECT_FALSE(
      m_engine->StartScan(MakeConfig(DAP::MemoryScanDataType::F32, SCAN_ADDRESS, 4, "1e-50"))
          .has_value());
}

TEST_F(DapMemoryEngineTest, FloatParsingRejectsInvalidAndNonFiniteForms)
{
  for (const auto type : {DAP::MemoryScanDataType::F32, DAP::MemoryScanDataType::F64})
  {
    for (const char* text :
         {" 1.5", "1.5 ", "++1.5", "1.5suffix", "0x1p0", "nan", "inf", "1e999", "1e-999"})
    {
      SCOPED_TRACE(text);
      EXPECT_FALSE(m_engine->StartScan(MakeConfig(type, SCAN_ADDRESS, 8, text)).has_value());
    }
  }
}

TEST_F(DapMemoryEngineTest, FloatParsingAcceptsDecimalExponentAndSignedZero)
{
  const std::vector<u8> zero(8, 0);
  WriteBytes(DATA_ADDRESS, zero);
  size_t event = 0;
  for (const char* text : {"+1.5", "-1.5e+2", ".5", "-0e-999", "1.40129846e-45"})
  {
    SCOPED_TRACE(text);
    ASSERT_TRUE(m_engine->StartScan(MakeConfig(DAP::MemoryScanDataType::F32, SCAN_ADDRESS, 4, text))
                    .has_value());
    ASSERT_TRUE(WaitForEvent(event++).has_value());
  }
}

TEST_F(DapMemoryEngineTest, DeepResultPageUsesBitmapIndex)
{
  constexpr u32 size = 1u << 20;
  std::vector<u8> bytes(size, 1);
  WriteBytes(DATA_ADDRESS, bytes);
  auto config = MakeConfig(DAP::MemoryScanDataType::U8, SCAN_ADDRESS, size, "0");
  config.filter = DAP::MemoryScanFilter::Unknown;
  config.value.reset();
  const auto accepted = m_engine->StartScan(config);
  ASSERT_TRUE(accepted.has_value());
  ASSERT_TRUE(WaitForEvent(0).has_value());
  const auto page = m_engine->GetResults(accepted->scan_id, size - 1, 1);
  ASSERT_TRUE(page.has_value());
  ASSERT_EQ(page->results.size(), 1u);
  EXPECT_EQ(page->results[0].address, SCAN_ADDRESS + size - 1);
}

TEST_F(DapMemoryEngineTest, RemoveResultsAndUndoPreserveImmutableGenerations)
{
  const std::vector<u8> bytes{1, 2, 3, 4};
  WriteBytes(DATA_ADDRESS, bytes);
  auto config = MakeConfig(DAP::MemoryScanDataType::U8, SCAN_ADDRESS, 4, "0");
  config.filter = DAP::MemoryScanFilter::Unknown;
  config.value.reset();
  const auto accepted = m_engine->StartScan(config);
  ASSERT_TRUE(accepted.has_value());
  ASSERT_TRUE(WaitForEvent(0).has_value());

  const auto removed = m_engine->RemoveResults(
      accepted->scan_id, {SCAN_ADDRESS + 1, SCAN_ADDRESS + 1, SCAN_ADDRESS + 20});
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(removed->generation, 2u);
  EXPECT_EQ(removed->result_count, 3u);
  EXPECT_EQ(removed->removed_count, 1u);
  EXPECT_TRUE(removed->can_undo);

  const auto no_change = m_engine->RemoveResults(accepted->scan_id, {SCAN_ADDRESS + 1});
  ASSERT_TRUE(no_change.has_value());
  EXPECT_EQ(no_change->generation, 2u);
  EXPECT_EQ(no_change->removed_count, 0u);

  const auto undone = m_engine->Undo(accepted->scan_id);
  ASSERT_TRUE(undone.has_value());
  EXPECT_EQ(undone->generation, 1u);
  EXPECT_EQ(undone->result_count, 4u);
  EXPECT_FALSE(undone->can_undo);
  EXPECT_FALSE(m_engine->Undo(accepted->scan_id).has_value());

  const auto removed_again = m_engine->RemoveResults(accepted->scan_id, {SCAN_ADDRESS + 2});
  ASSERT_TRUE(removed_again.has_value());
  EXPECT_EQ(removed_again->generation, 3u);
  EXPECT_EQ(removed_again->result_count, 3u);
}

TEST_F(DapMemoryEngineTest, BytePatternScanSupportsOverlapAndChangedRefinement)
{
  const std::vector<u8> bytes{0xaa, 0xaa, 0xaa, 0xbb};
  WriteBytes(DATA_ADDRESS, bytes);
  DAP::MemoryScanStartConfig config;
  config.ranges.push_back({SCAN_ADDRESS, SCAN_ADDRESS + static_cast<u32>(bytes.size())});
  config.data_type = DAP::MemoryScanDataType::Bytes;
  config.filter = DAP::MemoryScanFilter::Exact;
  config.value = "qqo=";
  config.byte_value = {0xaa, 0xaa};
  config.aligned = false;
  config.pause_during_scan = true;
  const auto accepted = m_engine->StartScan(config);
  ASSERT_TRUE(accepted.has_value());
  const auto completed = WaitForEvent(0);
  ASSERT_TRUE(completed.has_value());
  EXPECT_EQ(completed->result_count, 2u);

  const auto page = m_engine->GetResults(accepted->scan_id, 0, 10);
  ASSERT_TRUE(page.has_value());
  ASSERT_EQ(page->results.size(), 2u);
  EXPECT_EQ(page->results[0].address, SCAN_ADDRESS);
  EXPECT_EQ(page->results[1].address, SCAN_ADDRESS + 1);
  EXPECT_EQ(page->results[0].scanned_value, "qqo=");

  const std::vector<u8> changed{0xaa, 0xaa, 0xab, 0xbb};
  WriteBytes(DATA_ADDRESS, changed);
  DAP::MemoryScanRefineConfig refine;
  refine.scan_id = accepted->scan_id;
  refine.filter = DAP::MemoryScanFilter::Changed;
  const auto refined = m_engine->RefineScan(refine);
  ASSERT_TRUE(refined.has_value());
  const auto refined_event = WaitForEvent(1);
  ASSERT_TRUE(refined_event.has_value());
  EXPECT_EQ(refined_event->result_count, 1u);
  const auto refined_page = m_engine->GetResults(accepted->scan_id, 0, 10);
  ASSERT_TRUE(refined_page.has_value());
  ASSERT_EQ(refined_page->results.size(), 1u);
  EXPECT_EQ(refined_page->results[0].address, SCAN_ADDRESS + 1);
  EXPECT_EQ(refined_page->results[0].scanned_value, "qqs=");
}

TEST_F(DapMemoryEngineTest, BytePatternRefineRequiresOriginalWidth)
{
  const std::vector<u8> bytes{0xaa, 0xbb};
  WriteBytes(DATA_ADDRESS, bytes);
  DAP::MemoryScanStartConfig config;
  config.ranges.push_back({SCAN_ADDRESS, SCAN_ADDRESS + 2});
  config.data_type = DAP::MemoryScanDataType::Bytes;
  config.filter = DAP::MemoryScanFilter::Exact;
  config.value = "qrs=";
  config.byte_value = bytes;
  config.pause_during_scan = true;
  const auto accepted = m_engine->StartScan(config);
  ASSERT_TRUE(accepted.has_value());
  ASSERT_TRUE(WaitForEvent(0).has_value());

  DAP::MemoryScanRefineConfig refine;
  refine.scan_id = accepted->scan_id;
  refine.filter = DAP::MemoryScanFilter::Exact;
  refine.value = "qg==";
  const auto refined = m_engine->RefineScan(refine);
  EXPECT_FALSE(refined.has_value());
  const auto status = m_engine->GetStatus(accepted->scan_id);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status->generation, 1u);
  EXPECT_EQ(status->state, "completed");

  refine.value = "qrs=";
  EXPECT_TRUE(m_engine->RefineScan(refine).has_value());
}

TEST_F(DapMemoryEngineTest, BytePatternScanRejectsExcessiveComparisonWork)
{
  DAP::MemoryScanStartConfig config;
  config.regions = {"mem1"};
  config.data_type = DAP::MemoryScanDataType::Bytes;
  config.filter = DAP::MemoryScanFilter::Exact;
  config.byte_value.assign(64, 0xaa);
  config.value = DAP::Json::Base64Encode(config.byte_value);
  config.aligned = false;
  const auto accepted = m_engine->StartScan(config);
  ASSERT_FALSE(accepted.has_value());
  EXPECT_NE(accepted.error().find("comparison-work limit"), std::string::npos);
}

TEST_F(DapMemoryEngineTest, StringScanSupportsAsciiInsensitiveMatching)
{
  const std::vector<u8> bytes{'h', 'E', 'l', 'L', 'o', '!', 0xff};
  WriteBytes(DATA_ADDRESS, bytes);
  DAP::MemoryScanStartConfig config;
  config.ranges.push_back({SCAN_ADDRESS, SCAN_ADDRESS + static_cast<u32>(bytes.size())});
  config.data_type = DAP::MemoryScanDataType::String;
  config.filter = DAP::MemoryScanFilter::Exact;
  config.value = "Hello";
  config.byte_value = {'H', 'e', 'l', 'l', 'o'};
  config.string_encoding = DAP::MemoryScanStringEncoding::Ascii;
  config.case_sensitive = false;
  config.aligned = false;
  config.pause_during_scan = true;
  const auto accepted = m_engine->StartScan(config);
  ASSERT_TRUE(accepted.has_value());
  const auto completed = WaitForEvent(0);
  ASSERT_TRUE(completed.has_value());
  EXPECT_EQ(completed->result_count, 1u);

  const auto page = m_engine->GetResults(accepted->scan_id, 0, 10);
  ASSERT_TRUE(page.has_value());
  ASSERT_EQ(page->results.size(), 1u);
  EXPECT_EQ(page->results[0].scanned_value, "hElLo");

  const std::vector<u8> invalid_utf8{'h', 'E', 0xff, 'L', 'o', '!', 0xff};
  WriteBytes(DATA_ADDRESS, invalid_utf8);
  DAP::MemoryScanRefineConfig refine;
  refine.scan_id = accepted->scan_id;
  refine.filter = DAP::MemoryScanFilter::Changed;
  const auto refined = m_engine->RefineScan(refine);
  ASSERT_TRUE(refined.has_value());
  ASSERT_TRUE(WaitForEvent(1).has_value());
  const auto changed = m_engine->GetResults(accepted->scan_id, 0, 10);
  ASSERT_TRUE(changed.has_value());
  ASSERT_EQ(changed->results.size(), 1u);
  EXPECT_EQ(changed->results[0].scanned_value, "hE\xef\xbf\xbdLo");
  EXPECT_EQ(changed->results[0].raw, (std::vector<u8>{'h', 'E', 0xff, 'L', 'o'}));

  DAP::MemoryScanRefineConfig invalid_ascii;
  invalid_ascii.scan_id = accepted->scan_id;
  invalid_ascii.filter = DAP::MemoryScanFilter::Exact;
  invalid_ascii.value = "é!!!";
  EXPECT_FALSE(m_engine->RefineScan(invalid_ascii).has_value());
}

TEST_F(DapMemoryEngineTest, PpcInstructionScanMatchesMnemonicAndDisassembles)
{
  const std::vector<u8> bytes{0x60, 0x00, 0x00, 0x00, 0x4e, 0x80,
                              0x00, 0x20, 0x00, 0x00, 0x00, 0x00};
  WriteBytes(DATA_ADDRESS, bytes);
  DAP::MemoryScanStartConfig config;
  config.ranges.push_back({SCAN_ADDRESS, SCAN_ADDRESS + static_cast<u32>(bytes.size())});
  config.data_type = DAP::MemoryScanDataType::PpcInstruction;
  config.filter = DAP::MemoryScanFilter::Mnemonic;
  config.value = "nop";
  config.aligned = true;
  config.pause_during_scan = true;
  const auto accepted = m_engine->StartScan(config);
  ASSERT_TRUE(accepted.has_value());
  const auto completed = WaitForEvent(0);
  ASSERT_TRUE(completed.has_value());
  EXPECT_EQ(completed->result_count, 1u);

  const auto page = m_engine->GetResults(accepted->scan_id, 0, 10);
  ASSERT_TRUE(page.has_value());
  ASSERT_EQ(page->results.size(), 1u);
  EXPECT_EQ(page->results[0].address, SCAN_ADDRESS);
  EXPECT_EQ(page->results[0].scanned_value, "0x60000000");
  ASSERT_TRUE(page->results[0].disassembly.has_value());
  EXPECT_NE(page->results[0].disassembly->find("nop"), std::string::npos);
}

TEST_F(DapMemoryEngineTest, PpcInstructionScanFiltersValidInstructions)
{
  const std::vector<u8> bytes{0x60, 0x00, 0x00, 0x00, 0x44, 0x00, 0x00, 0x00};
  WriteBytes(DATA_ADDRESS, bytes);
  DAP::MemoryScanStartConfig config;
  config.ranges.push_back({SCAN_ADDRESS, SCAN_ADDRESS + static_cast<u32>(bytes.size())});
  config.data_type = DAP::MemoryScanDataType::PpcInstruction;
  config.filter = DAP::MemoryScanFilter::ValidInstruction;
  config.aligned = true;
  config.pause_during_scan = true;
  const auto accepted = m_engine->StartScan(config);
  ASSERT_TRUE(accepted.has_value());
  const auto completed = WaitForEvent(0);
  ASSERT_TRUE(completed.has_value());
  EXPECT_EQ(completed->result_count, 1u);
}

TEST_F(DapMemoryEngineTest, PpcInstructionValidityRejectsReservedEncoding)
{
  const std::vector<u8> bytes{
      0x00, 0x00, 0x00, 0x00,  // Zero is rendered specially but is not executable.
      0x08, 0x00, 0x00, 0x00,  // PPC64-only tdi.
      0x10, 0x00, 0x00, 0x02,  // Unsupported paired-single encoding.
      0x44, 0x00, 0x00, 0x00,  // Reserved system-call encoding.
  };
  WriteBytes(DATA_ADDRESS, bytes);
  DAP::MemoryScanStartConfig config;
  config.ranges.push_back({SCAN_ADDRESS, SCAN_ADDRESS + static_cast<u32>(bytes.size())});
  config.data_type = DAP::MemoryScanDataType::PpcInstruction;
  config.filter = DAP::MemoryScanFilter::ValidInstruction;
  config.pause_during_scan = true;
  const auto accepted = m_engine->StartScan(config);
  ASSERT_TRUE(accepted.has_value());
  const auto completed = WaitForEvent(0);
  ASSERT_TRUE(completed.has_value());
  EXPECT_EQ(completed->result_count, 0u);
}

TEST_F(DapMemoryEngineTest, NumericScanRejectsPpcOnlyFilter)
{
  const std::vector<u8> bytes{0x60, 0x00, 0x00, 0x00};
  WriteBytes(DATA_ADDRESS, bytes);
  auto config = MakeConfig(DAP::MemoryScanDataType::U32, SCAN_ADDRESS, 4, "0");
  config.filter = DAP::MemoryScanFilter::ValidInstruction;
  config.value.reset();
  EXPECT_FALSE(m_engine->StartScan(config).has_value());
}

TEST_F(DapMemoryEngineTest, PpcMnemonicScanRejectsExcessiveDecodingWork)
{
  DAP::MemoryScanStartConfig config;
  config.data_type = DAP::MemoryScanDataType::PpcInstruction;
  config.filter = DAP::MemoryScanFilter::Mnemonic;
  config.value = "nop";
  EXPECT_FALSE(m_engine->StartScan(config).has_value());
}

TEST(DapMemoryEngine, GekkoDisassemblyIsSafeAcrossThreads)
{
  constexpr u32 iterations = 10000;
  const std::string expected_nop = Common::GekkoDisassembler::Disassemble(0x60000000, 0x80004000);
  const std::string expected_branch =
      Common::GekkoDisassembler::Disassemble(0x48000000, 0x80005000);
  std::atomic_bool valid = true;
  const auto run = [&](u32 instruction, u32 address, const std::string& expected) {
    for (u32 i = 0; i < iterations && valid.load(); ++i)
    {
      if (Common::GekkoDisassembler::Disassemble(instruction, address) != expected)
        valid = false;
    }
  };
  std::thread nop_thread(run, 0x60000000, 0x80004000, std::cref(expected_nop));
  std::thread branch_thread(run, 0x48000000, 0x80005000, std::cref(expected_branch));
  nop_thread.join();
  branch_thread.join();
  EXPECT_TRUE(valid.load());
}

TEST(DapMemoryEngine, SharedBudgetChargesAndReleasesExactly)
{
  DAP::MemoryScanBudget budget(10);
  auto first = budget.TryReserve(6);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(budget.GetUsedBytes(), 6u);
  EXPECT_FALSE(budget.TryReserve(5).has_value());
  first.reset();
  EXPECT_EQ(budget.GetUsedBytes(), 0u);
  EXPECT_TRUE(budget.TryReserve(10).has_value());
}

TEST_F(DapMemoryEngineTest, SharedBudgetBoundsMultipleEngines)
{
  const std::vector<u8> bytes{1, 2, 3, 4};
  WriteBytes(DATA_ADDRESS, bytes);
  DAP::MemoryScanBudget budget(21);
  std::mutex event_mutex;
  std::condition_variable event_condition;
  size_t event_count = 0;
  const auto callback = [&](DAP::MemoryScanTerminalEvent) {
    {
      std::lock_guard lock(event_mutex);
      ++event_count;
    }
    event_condition.notify_one();
  };
  m_engine = std::make_unique<DAP::DapMemoryEngine>(Core::System::GetInstance(), callback, budget);
  auto second =
      std::make_unique<DAP::DapMemoryEngine>(Core::System::GetInstance(), callback, budget);

  auto config = MakeConfig(DAP::MemoryScanDataType::U8, SCAN_ADDRESS, 4, "0");
  config.filter = DAP::MemoryScanFilter::Unknown;
  config.value.reset();
  const auto accepted = m_engine->StartScan(config);
  ASSERT_TRUE(accepted.has_value());
  {
    std::unique_lock lock(event_mutex);
    ASSERT_TRUE(
        event_condition.wait_for(lock, std::chrono::seconds(5), [&] { return event_count == 1; }));
  }
  EXPECT_EQ(budget.GetUsedBytes(), 21u);
  EXPECT_FALSE(second->StartScan(config).has_value());

  EXPECT_TRUE(m_engine->Dispose(accepted->scan_id));
  m_engine.reset();
  EXPECT_EQ(budget.GetUsedBytes(), 0u);
  ASSERT_TRUE(second->StartScan(config).has_value());
  {
    std::unique_lock lock(event_mutex);
    ASSERT_TRUE(
        event_condition.wait_for(lock, std::chrono::seconds(5), [&] { return event_count == 2; }));
  }
  second.reset();
  EXPECT_EQ(budget.GetUsedBytes(), 0u);
}
}  // namespace
