// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace DAP
{
class DapMemoryEngine;

enum class MemoryScanDataType
{
  U8,
  U16,
  U32,
  U64,
  S8,
  S16,
  S32,
  S64,
  F32,
  F64,
  Bytes,
  String,
  PpcInstruction,
};

enum class MemoryScanFilter
{
  Exact,
  NotEqual,
  Between,
  GreaterThan,
  GreaterOrEqual,
  LessThan,
  LessOrEqual,
  Unknown,
  Changed,
  Unchanged,
  Increased,
  Decreased,
  IncreasedBy,
  DecreasedBy,
  Mnemonic,
  ValidInstruction,
};

enum class MemoryScanStringEncoding
{
  Utf8,
  Ascii,
};

struct MemoryScanRange
{
  u32 start = 0;
  u32 end = 0;  // Exclusive.
};

struct MemoryScanStartConfig
{
  std::vector<std::string> regions;
  std::vector<MemoryScanRange> ranges;
  MemoryScanDataType data_type = MemoryScanDataType::U8;
  MemoryScanFilter filter = MemoryScanFilter::Unknown;
  std::optional<std::string> value;
  std::optional<std::string> value2;
  std::vector<u8> byte_value;
  MemoryScanStringEncoding string_encoding = MemoryScanStringEncoding::Utf8;
  bool case_sensitive = true;
  bool aligned = true;
  bool pause_during_scan = false;
};

struct MemoryScanRefineConfig
{
  int scan_id = 0;
  MemoryScanFilter filter = MemoryScanFilter::Changed;
  std::optional<std::string> value;
  std::optional<std::string> value2;
  std::optional<bool> pause_during_scan;
};

struct MemoryRegionInfo
{
  std::string id;
  std::string name;
  u32 base_address = 0;
  u32 size = 0;
};

struct MemoryScanJobAccepted
{
  int scan_id = 0;
  int job_id = 0;
  bool pause_during_scan = false;
};

struct MemoryScanStatus
{
  int scan_id = 0;
  int job_id = 0;
  u64 generation = 0;
  std::string state;
  std::string phase;
  bool pause_during_scan = false;
  bool emulation_paused = false;
  u64 result_count = 0;
};

struct MemoryScanResult
{
  u32 address = 0;
  std::string scanned_value;
  std::vector<u8> raw;
  std::optional<std::string> disassembly;
};

struct MemoryScanResultPage
{
  int scan_id = 0;
  u64 generation = 0;
  u64 total_results = 0;
  u64 start = 0;
  std::vector<MemoryScanResult> results;
};

struct MemoryScanMutationResult
{
  int scan_id = 0;
  u64 generation = 0;
  u64 result_count = 0;
  u64 removed_count = 0;
  bool can_undo = false;
};

struct MemoryScanTerminalEvent
{
  std::string event;
  int scan_id = 0;
  int job_id = 0;
  u64 generation = 0;
  u64 result_count = 0;
  u64 duration_ms = 0;
  bool pause_during_scan = false;
  std::string message;
};

class MemoryScanBudget
{
public:
  class Charge
  {
  public:
    Charge() = default;
    Charge(Charge&& other) noexcept;
    Charge& operator=(Charge&& other) noexcept;
    ~Charge();

    Charge(const Charge&) = delete;
    Charge& operator=(const Charge&) = delete;

  private:
    struct State;
    friend class MemoryScanBudget;
    friend class DapMemoryEngine;
    Charge(std::shared_ptr<State> state, u64 bytes);
    Charge Split(u64 bytes);
    void Release();

    std::shared_ptr<State> m_state;
    u64 m_bytes = 0;
  };

  explicit MemoryScanBudget(u64 limit);
  std::optional<Charge> TryReserve(u64 bytes) const;
  u64 GetUsedBytes() const;

private:
  std::shared_ptr<Charge::State> m_state;
};

class DapMemoryEngine
{
public:
  using TerminalCallback = std::function<void(MemoryScanTerminalEvent)>;

  DapMemoryEngine(Core::System& system, TerminalCallback terminal_callback);
  // Tests may inject a smaller shared quota; production sessions use the process singleton.
  DapMemoryEngine(Core::System& system, TerminalCallback terminal_callback,
                  MemoryScanBudget budget);
  ~DapMemoryEngine();

  DapMemoryEngine(const DapMemoryEngine&) = delete;
  DapMemoryEngine& operator=(const DapMemoryEngine&) = delete;

  std::vector<MemoryRegionInfo> GetMemoryRegions() const;

  std::expected<MemoryScanJobAccepted, std::string> StartScan(const MemoryScanStartConfig& config);
  std::expected<MemoryScanJobAccepted, std::string>
  RefineScan(const MemoryScanRefineConfig& config);
  std::optional<MemoryScanStatus> GetStatus(int scan_id) const;
  std::expected<MemoryScanResultPage, std::string> GetResults(int scan_id, u64 start,
                                                              u32 count) const;
  std::expected<MemoryScanMutationResult, std::string> Undo(int scan_id);
  std::expected<MemoryScanMutationResult, std::string>
  RemoveResults(int scan_id, const std::vector<u32>& addresses);
  bool Cancel(int scan_id);
  bool Dispose(int scan_id);

  bool HasActiveJob() const;
  static std::unique_lock<std::mutex> TryLockCoreScan();

private:
  struct SnapshotRange
  {
    u32 start = 0;
    std::vector<u8> bytes;
    u64 first_candidate = 0;
    u64 candidate_count = 0;
    u32 candidate_offset = 0;
  };

  struct SnapshotStorage
  {
    MemoryScanBudget::Charge budget_charge;
    std::vector<SnapshotRange> ranges;
  };

  struct Generation
  {
    u64 number = 0;
    std::shared_ptr<const SnapshotStorage> ranges;
    MemoryScanBudget::Charge budget_charge;
    std::vector<u8> candidates;
    std::vector<u64> result_index;
    u64 result_count = 0;
  };

  struct Scan
  {
    int id = 0;
    MemoryScanStartConfig config;
    mutable std::mutex mutex;
    int current_job_id = 0;
    std::string state = "created";
    std::string phase = "waiting";
    bool job_pause_during_scan = false;
    bool emulation_paused = false;
    bool disposed = false;
    std::shared_ptr<const Generation> generation;
    std::vector<std::shared_ptr<const Generation>> undo_generations;
    u64 next_generation_number = 1;
  };

  struct ResolvedRange
  {
    u32 start = 0;
    u32 size = 0;
    std::string region_id;
  };

  using NumericValue = std::variant<u8, u16, u32, u64, s8, s16, s32, s64, float, double>;

  std::expected<std::vector<ResolvedRange>, std::string>
  ResolveRanges(const MemoryScanStartConfig& config) const;
  std::expected<std::vector<SnapshotRange>, std::string>
  CaptureSnapshot(const std::vector<ResolvedRange>& ranges, std::atomic<bool>& cancelled) const;
  std::expected<std::shared_ptr<Generation>, std::string>
  BuildGeneration(const Scan& scan, std::vector<SnapshotRange> snapshot,
                  const std::shared_ptr<const Generation>& previous, MemoryScanFilter filter,
                  const std::optional<std::string>& value, const std::optional<std::string>& value2,
                  std::atomic<bool>& cancelled, MemoryScanBudget::Charge reservation) const;

  void StartWorker(std::shared_ptr<Scan> scan, int job_id, MemoryScanFilter filter,
                   std::optional<std::string> value, std::optional<std::string> value2,
                   bool pause_during_scan, std::shared_ptr<const Generation> previous,
                   MemoryScanBudget::Charge reservation);
  void RunWorker(std::shared_ptr<Scan> scan, int job_id, MemoryScanFilter filter,
                 std::optional<std::string> value, std::optional<std::string> value2,
                 bool pause_during_scan, std::shared_ptr<const Generation> previous,
                 MemoryScanBudget::Charge reservation);
  void FinishWorker(std::shared_ptr<Scan> scan, int job_id, MemoryScanTerminalEvent terminal);
  bool ReapWorker();
  u64 CalculateGenerationBytes(const MemoryScanStartConfig& config,
                               const std::vector<ResolvedRange>& ranges) const;

  static u32 DataTypeSize(const MemoryScanStartConfig& config);
  static std::optional<NumericValue> ParseValue(MemoryScanDataType data_type,
                                                std::string_view value);
  static std::optional<std::string> ValidateFilter(MemoryScanDataType data_type,
                                                   MemoryScanFilter filter,
                                                   const std::optional<std::string>& value,
                                                   const std::optional<std::string>& value2,
                                                   bool has_previous);
  static std::string FormatValue(MemoryScanDataType data_type, const u8* bytes);
  static void BuildResultIndex(Generation* generation);

  Core::System& m_system;
  TerminalCallback m_terminal_callback;
  MemoryScanBudget m_budget;
  mutable std::mutex m_mutex;
  std::map<int, std::shared_ptr<Scan>> m_scans;
  std::mutex m_worker_mutex;
  std::thread m_worker;
  std::atomic<bool> m_cancelled{false};
  bool m_job_active = false;
  int m_active_scan_id = 0;
  int m_next_scan_id = 1;
  int m_next_job_id = 1;
};
}  // namespace DAP
