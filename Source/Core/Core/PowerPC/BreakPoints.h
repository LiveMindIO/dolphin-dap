// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Core/PowerPC/Expression.h"

namespace Core
{
class System;
}

struct TBreakPoint
{
  u32 address = 0;
  bool is_enabled = false;
  bool log_on_hit = false;
  bool break_on_hit = false;
  std::optional<Expression> condition;
};

struct TMemCheck
{
  u32 start_address = 0;
  u32 end_address = 0;

  bool is_enabled = true;
  bool is_ranged = false;

  bool is_break_on_read = false;
  bool is_break_on_write = false;

  bool log_on_hit = false;
  bool break_on_hit = false;

  u32 num_hits = 0;

  std::optional<Expression> condition;

  // DESNOTE(jbarber, 2026-07-22): When true, this memcheck represents a DAP
  // "hard freeze" — writes to the range are silently suppressed (dropped
  // before WriteToHardware) instead of pausing the CPU. Reads are unaffected
  // (the frozen value persists in RAM because writes are suppressed).
  // Piggy-backs on the existing TMemCheck infrastructure (range lookup, JIT
  // de-optimization via OverlapsMemcheck/DBAT rebuild) so frozen pages are
  // forced onto the slow MMU::Write<T> path where suppression can fire.
  // The field-rate Tick in RealtimeWatchSampler remains as a fallback for
  // DMA/peripheral writes that bypass MMU::Write entirely.
  bool is_freeze = false;

  // returns whether to break
  bool Action(Core::System& system, u64 value, u32 addr, bool write, size_t size, u32 pc);
};

// Code breakpoints.
class BreakPoints
{
public:
  using ClientId = u64;

  struct CodeBreakpoint
  {
    u32 address = 0;
    bool is_enabled = true;
    bool log_on_hit = false;
    bool break_on_hit = true;
    std::optional<std::string> condition;

    bool operator==(const CodeBreakpoint&) const = default;
  };

  enum class ChangeReason
  {
    New,
    Changed,
    Removed,
  };

  struct Change
  {
    ChangeReason reason = ChangeReason::New;
    CodeBreakpoint breakpoint;
  };

  struct Event
  {
    u64 revision = 0;
    std::optional<ClientId> origin;
    std::vector<Change> changes;
    std::vector<CodeBreakpoint> breakpoints;
  };

  using EventCallback = std::function<void(std::shared_ptr<const Event>)>;

  explicit BreakPoints(Core::System& system);
  BreakPoints(const BreakPoints& other) = delete;
  BreakPoints(BreakPoints&& other) = delete;
  BreakPoints& operator=(const BreakPoints& other) = delete;
  BreakPoints& operator=(BreakPoints&& other) = delete;
  ~BreakPoints();

  using TBreakPoints = std::vector<TBreakPoint>;
  using TBreakPointsStr = std::vector<std::string>;

  struct Snapshot
  {
    u64 revision = 0;
    bool breaking_enabled = true;
    TBreakPoints breakpoints;
    std::optional<TBreakPoint> temporary_breakpoint;

    const TBreakPoint* GetBreakpoint(u32 address) const;
    const TBreakPoint* GetRegularBreakpoint(u32 address) const;
  };

  std::shared_ptr<const Snapshot> GetSnapshot() const;
  std::shared_ptr<const TBreakPoints> GetBreakPoints() const;
  TBreakPointsStr GetStrings() const;
  void AddFromStrings(const TBreakPointsStr& bp_strings);

  ClientId RegisterClient(EventCallback callback = {});
  void SetClientEventCallback(ClientId client_id, EventCallback callback);
  std::expected<void, std::string>
  ReplaceClientSourceBreakpoints(ClientId client_id, std::string source_key,
                                 std::vector<CodeBreakpoint> breakpoints);
  std::expected<void, std::string>
  ReplaceClientInstructionBreakpoints(ClientId client_id, std::vector<CodeBreakpoint> breakpoints);
  void ClearClientBreakpoints(ClientId client_id);
  void UnregisterClient(ClientId client_id);
  u64 GetRevision() const;

  bool IsAddressBreakPoint(u32 address) const;
  bool IsBreakPointEnable(u32 address) const;
  // Get the breakpoint in this address (for most purposes)
  std::shared_ptr<const TBreakPoint> GetBreakpoint(u32 address) const;
  // Get the breakpoint in this address (ignore temporary breakpoint, e.g. for editing purposes)
  std::shared_ptr<const TBreakPoint> GetRegularBreakpoint(u32 address) const;

  // GUI site removal is authoritative across owners. GUI replacement preserves
  // DAP claims and adds or replaces the legacy claim; compatible conditions are
  // aggregated by BuildProjection.
  std::expected<void, std::string> Add(u32 address, bool break_on_hit, bool log_on_hit,
                                       std::optional<Expression> condition);
  std::expected<void, std::string> Add(u32 address);
  std::expected<void, std::string> Add(TBreakPoint bp);
  // Add temporary breakpoint (e.g., Step Over, Run to Here)
  // It can be on the same address of a regular breakpoint (it will have priority in this case)
  // It's cleared whenever the emulation is paused for any reason
  // (CPUManager::SetStateLocked(State::Paused))
  // TODO: Should it somehow force to resume emulation when called?
  void SetTemporary(u32 address);

  bool ToggleBreakPoint(u32 address);
  bool ToggleEnable(u32 address);

  void EnableBreaking(bool enable);
  bool IsBreakingEnabled() const { return GetSnapshot()->breaking_enabled; }

  // Remove Breakpoint. Returns whether it was removed.
  bool Remove(u32 address);
  void Clear();
  void ClearTemporary();

private:
  struct ClientBreakpoints
  {
    std::map<std::string, std::vector<CodeBreakpoint>> sources;
    std::vector<CodeBreakpoint> instructions;
    EventCallback callback;
  };

  using Projection = std::map<u32, CodeBreakpoint>;

  struct PendingDispatch
  {
    std::shared_ptr<const Event> event;
  };

  // Enabled claims are combined per action. Each action's unique canonical
  // conditions form a logical OR, with unconditional dominating; break and log
  // predicates must match when both actions are active because TBreakPoint has
  // only one physical condition.
  std::expected<Projection, std::string>
  BuildProjection(const std::map<u32, CodeBreakpoint>& legacy,
                  const std::unordered_map<ClientId, ClientBreakpoints>& clients) const;
  bool ApplyProjectionLocked(Projection projection, std::optional<ClientId> origin);
  std::expected<bool, std::string> ReplaceSiteLocked(CodeBreakpoint breakpoint);
  bool RemoveSiteLocked(u32 address);
  static bool EraseSiteClaims(u32 address, std::map<u32, CodeBreakpoint>& legacy,
                              std::unordered_map<ClientId, ClientBreakpoints>& clients);
  static void SetSiteClaimsEnabled(u32 address, bool enabled, std::map<u32, CodeBreakpoint>& legacy,
                                   std::unordered_map<ClientId, ClientBreakpoints>& clients);
  void PublishSnapshotLocked();
  void DrainDispatchQueue();
  void SynchronizeDispatch();
  static CodeBreakpoint ToCodeBreakpoint(const TBreakPoint& breakpoint);

  Projection m_projection;
  std::map<u32, CodeBreakpoint> m_legacy_breakpoints;
  std::unordered_map<ClientId, ClientBreakpoints> m_clients;
  std::optional<u32> m_temporary_address;
  Core::System& m_system;
  bool m_breaking_enabled = true;
  mutable std::mutex m_mutex;
  // Delivery is serialized separately from state mutation. Reentrant mutations
  // append to m_pending_dispatches and are delivered after the current event.
  std::recursive_mutex m_dispatch_mutex;
  bool m_is_draining = false;
  std::deque<PendingDispatch> m_pending_dispatches;
  std::atomic<std::shared_ptr<const Snapshot>> m_snapshot;
  ClientId m_next_client_id = 1;
  u64 m_revision = 0;
};

class DelayedMemCheckUpdate;

// Memory breakpoints
class MemChecks
{
public:
  explicit MemChecks(Core::System& system);
  MemChecks(const MemChecks& other) = delete;
  MemChecks(MemChecks&& other) = delete;
  MemChecks& operator=(const MemChecks& other) = delete;
  MemChecks& operator=(MemChecks&& other) = delete;
  ~MemChecks();

  using TMemChecks = std::vector<TMemCheck>;
  using TMemChecksStr = std::vector<std::string>;

  const TMemChecks& GetMemChecks() const { return m_mem_checks; }
  TMemChecksStr GetStrings() const;
  void AddFromStrings(const TMemChecksStr& mc_strings);

  DelayedMemCheckUpdate Add(TMemCheck memory_check);

  bool ToggleEnable(u32 address);

  TMemCheck* GetMemCheck(u32 address, size_t size = 1);
  bool OverlapsMemcheck(u32 address, u32 length) const;
  DelayedMemCheckUpdate Remove(u32 address);

  void EnableBreaking(bool enable);
  bool IsBreakingEnabled() const { return m_breaking_enabled; }

  void Update();
  void Clear();
  bool HasAny() const { return !m_mem_checks.empty() && m_breaking_enabled; }

  BitSet32 GetGPRsUsedInConditions() { return m_gprs_used_in_conditions; }
  BitSet32 GetFPRsUsedInConditions() { return m_fprs_used_in_conditions; }

private:
  // Returns whether any change was made
  bool UpdateRegistersUsedInConditions();

  TMemChecks m_mem_checks;
  Core::System& m_system;
  BitSet32 m_gprs_used_in_conditions;
  BitSet32 m_fprs_used_in_conditions;
  bool m_mem_breakpoints_set = false;
  bool m_breaking_enabled = true;
};

class DelayedMemCheckUpdate final
{
public:
  DelayedMemCheckUpdate(MemChecks* memchecks, bool update_needed = false)
      : m_memchecks(memchecks), m_update_needed(update_needed)
  {
  }

  DelayedMemCheckUpdate(const DelayedMemCheckUpdate&) = delete;
  DelayedMemCheckUpdate(DelayedMemCheckUpdate&& other) = delete;
  DelayedMemCheckUpdate& operator=(const DelayedMemCheckUpdate&) = delete;
  DelayedMemCheckUpdate& operator=(DelayedMemCheckUpdate&& other) = delete;

  ~DelayedMemCheckUpdate()
  {
    if (m_update_needed)
      m_memchecks->Update();
  }

  DelayedMemCheckUpdate& operator|=(DelayedMemCheckUpdate&& other)
  {
    if (m_memchecks == other.m_memchecks)
    {
      m_update_needed |= other.m_update_needed;
      other.m_update_needed = false;
    }
    return *this;
  }

  operator bool() const { return m_update_needed; }

private:
  MemChecks* m_memchecks;
  bool m_update_needed;
};
