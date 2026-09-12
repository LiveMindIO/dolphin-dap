// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/BreakPoints.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/Core.h"
#include "Core/Debugger/DebugInterface.h"
#include "Core/Host.h"
#include "Core/PowerPC/Expression.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/System.h"

BreakPoints::BreakPoints(Core::System& system) : m_system(system)
{
  m_snapshot.store(std::make_shared<const Snapshot>());
}

BreakPoints::~BreakPoints() = default;

const TBreakPoint* BreakPoints::Snapshot::GetBreakpoint(const u32 address) const
{
  if (temporary_breakpoint && temporary_breakpoint->address == address)
    return &*temporary_breakpoint;
  return GetRegularBreakpoint(address);
}

const TBreakPoint* BreakPoints::Snapshot::GetRegularBreakpoint(const u32 address) const
{
  const auto breakpoint = std::ranges::find(breakpoints, address, &TBreakPoint::address);
  return breakpoint == breakpoints.end() ? nullptr : &*breakpoint;
}

bool BreakPoints::IsAddressBreakPoint(u32 address) const
{
  return GetBreakpoint(address) != nullptr;
}

bool BreakPoints::IsBreakPointEnable(u32 address) const
{
  const auto snapshot = GetSnapshot();
  if (!snapshot->breaking_enabled)
    return false;
  const TBreakPoint* breakpoint = snapshot->GetBreakpoint(address);
  return breakpoint != nullptr && breakpoint->is_enabled;
}

std::shared_ptr<const BreakPoints::Snapshot> BreakPoints::GetSnapshot() const
{
  return m_snapshot.load();
}

std::shared_ptr<const BreakPoints::TBreakPoints> BreakPoints::GetBreakPoints() const
{
  const auto snapshot = GetSnapshot();
  return std::shared_ptr<const TBreakPoints>(snapshot, &snapshot->breakpoints);
}

std::shared_ptr<const TBreakPoint> BreakPoints::GetBreakpoint(u32 address) const
{
  const auto snapshot = GetSnapshot();
  const TBreakPoint* breakpoint = snapshot->GetBreakpoint(address);
  if (!breakpoint)
    return {};
  return std::shared_ptr<const TBreakPoint>(snapshot, breakpoint);
}

std::shared_ptr<const TBreakPoint> BreakPoints::GetRegularBreakpoint(u32 address) const
{
  const auto snapshot = GetSnapshot();
  const TBreakPoint* breakpoint = snapshot->GetRegularBreakpoint(address);
  if (!breakpoint)
    return {};
  return std::shared_ptr<const TBreakPoint>(snapshot, breakpoint);
}

BreakPoints::TBreakPointsStr BreakPoints::GetStrings() const
{
  std::lock_guard lock(m_mutex);
  TBreakPointsStr bp_strings;
  for (const auto& [_, breakpoint] : m_legacy_breakpoints)
  {
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss << fmt::format("${:08x} ", breakpoint.address);
    if (breakpoint.is_enabled)
      ss << "n";
    if (breakpoint.log_on_hit)
      ss << "l";
    if (breakpoint.break_on_hit)
      ss << "b";
    if (breakpoint.condition)
      ss << "c " << *breakpoint.condition;
    bp_strings.emplace_back(ss.str());
  }

  return bp_strings;
}

void BreakPoints::AddFromStrings(const TBreakPointsStr& bp_strings)
{
  for (const std::string& bp_string : bp_strings)
  {
    TBreakPoint bp;
    std::string flags;
    std::istringstream iss(bp_string);
    iss.imbue(std::locale::classic());

    if (iss.peek() == '$')
      iss.ignore();
    iss >> std::hex >> bp.address;
    iss >> flags;
    bp.is_enabled = flags.contains('n');
    bp.log_on_hit = flags.contains('l');
    bp.break_on_hit = flags.contains('b');
    if (flags.contains('c'))
    {
      iss >> std::ws;
      std::string condition;
      std::getline(iss, condition);
      bp.condition = Expression::TryParse(condition);
    }
    (void)Add(std::move(bp));
  }
}

std::expected<void, std::string> BreakPoints::Add(TBreakPoint bp)
{
  std::expected<bool, std::string> dispatch = false;
  {
    std::lock_guard lock(m_mutex);
    dispatch = ReplaceSiteLocked(ToCodeBreakpoint(bp));
  }
  if (!dispatch)
    return std::unexpected(dispatch.error());
  if (*dispatch)
    DrainDispatchQueue();
  return {};
}

std::expected<void, std::string> BreakPoints::Add(u32 address)
{
  return BreakPoints::Add(address, true, false, std::nullopt);
}

std::expected<void, std::string> BreakPoints::Add(u32 address, bool break_on_hit, bool log_on_hit,
                                                  std::optional<Expression> condition)
{
  CodeBreakpoint breakpoint;
  breakpoint.address = address;
  breakpoint.break_on_hit = break_on_hit;
  breakpoint.log_on_hit = log_on_hit;
  if (condition)
    breakpoint.condition = condition->GetText();
  std::expected<bool, std::string> dispatch = false;
  {
    std::lock_guard lock(m_mutex);
    if (const auto it = m_projection.find(address); it != m_projection.end())
      breakpoint.is_enabled = it->second.is_enabled;
    dispatch = ReplaceSiteLocked(std::move(breakpoint));
  }
  if (!dispatch)
    return std::unexpected(dispatch.error());
  if (*dispatch)
    DrainDispatchQueue();
  return {};
}

void BreakPoints::SetTemporary(u32 address)
{
  std::lock_guard lock(m_mutex);
  if (m_temporary_address && *m_temporary_address != address)
    m_system.GetJitInterface().InvalidateICache(*m_temporary_address, 4, true);
  m_temporary_address = address;
  PublishSnapshotLocked();
  m_system.GetJitInterface().InvalidateICache(address, 4, true);
}

bool BreakPoints::ToggleBreakPoint(u32 address)
{
  bool added = false;
  bool dispatch = false;
  {
    std::lock_guard lock(m_mutex);
    if (m_projection.contains(address))
    {
      dispatch = RemoveSiteLocked(address);
    }
    else
    {
      const auto result = ReplaceSiteLocked(CodeBreakpoint{.address = address});
      if (!result)
        return false;
      dispatch = *result;
      added = true;
    }
  }
  if (dispatch)
    DrainDispatchQueue();
  return added;
}

bool BreakPoints::ToggleEnable(u32 address)
{
  bool dispatch = false;
  {
    std::lock_guard lock(m_mutex);
    const auto iter = m_projection.find(address);
    if (iter == m_projection.end())
      return false;
    auto legacy = m_legacy_breakpoints;
    auto clients = m_clients;
    SetSiteClaimsEnabled(address, !iter->second.is_enabled, legacy, clients);
    const auto projection = BuildProjection(legacy, clients);
    if (!projection)
      return false;
    m_legacy_breakpoints = std::move(legacy);
    m_clients = std::move(clients);
    dispatch = ApplyProjectionLocked(*projection, std::nullopt);
  }
  if (dispatch)
    DrainDispatchQueue();
  return true;
}

void BreakPoints::EnableBreaking(bool enable)
{
  std::lock_guard lock(m_mutex);
  m_breaking_enabled = enable;
  PublishSnapshotLocked();
}

bool BreakPoints::Remove(u32 address)
{
  bool dispatch = false;
  {
    std::lock_guard lock(m_mutex);
    if (!m_projection.contains(address))
      return false;
    dispatch = RemoveSiteLocked(address);
  }
  if (dispatch)
    DrainDispatchQueue();
  return true;
}

void BreakPoints::Clear()
{
  bool dispatch = false;
  {
    std::lock_guard lock(m_mutex);
    auto clients = m_clients;
    for (auto& [_, client] : clients)
    {
      client.sources.clear();
      client.instructions.clear();
    }
    const auto projection = BuildProjection({}, clients);
    if (projection)
    {
      m_legacy_breakpoints.clear();
      m_clients = std::move(clients);
      dispatch = ApplyProjectionLocked(*projection, std::nullopt);
    }
    if (m_temporary_address)
    {
      m_system.GetJitInterface().InvalidateICache(*m_temporary_address, 4, true);
      m_temporary_address.reset();
      PublishSnapshotLocked();
    }
  }
  if (dispatch)
    DrainDispatchQueue();
}

void BreakPoints::ClearTemporary()
{
  std::lock_guard lock(m_mutex);
  if (m_temporary_address)
  {
    m_system.GetJitInterface().InvalidateICache(*m_temporary_address, 4, true);
    m_temporary_address.reset();
    PublishSnapshotLocked();
  }
}

BreakPoints::CodeBreakpoint BreakPoints::ToCodeBreakpoint(const TBreakPoint& breakpoint)
{
  CodeBreakpoint result;
  result.address = breakpoint.address;
  result.is_enabled = breakpoint.is_enabled;
  result.log_on_hit = breakpoint.log_on_hit;
  result.break_on_hit = breakpoint.break_on_hit;
  if (breakpoint.condition)
    result.condition = breakpoint.condition->GetText();
  return result;
}

BreakPoints::ClientId BreakPoints::RegisterClient(EventCallback callback)
{
  std::lock_guard lock(m_mutex);
  const ClientId id = m_next_client_id++;
  m_clients.emplace(id, ClientBreakpoints{.callback = std::move(callback)});
  return id;
}

void BreakPoints::SetClientEventCallback(const ClientId client_id, EventCallback callback)
{
  {
    std::lock_guard lock(m_mutex);
    if (auto it = m_clients.find(client_id); it != m_clients.end())
      it->second.callback = std::move(callback);
  }
  SynchronizeDispatch();
}

std::expected<void, std::string>
BreakPoints::ReplaceClientSourceBreakpoints(const ClientId client_id, std::string source_key,
                                            std::vector<CodeBreakpoint> breakpoints)
{
  bool dispatch = false;
  {
    std::lock_guard lock(m_mutex);
    auto clients = m_clients;
    const auto client = clients.find(client_id);
    if (client == clients.end())
      return std::unexpected("unknown breakpoint client");
    if (breakpoints.empty())
      client->second.sources.erase(source_key);
    else
      client->second.sources[std::move(source_key)] = std::move(breakpoints);
    const auto projection = BuildProjection(m_legacy_breakpoints, clients);
    if (!projection)
      return std::unexpected(projection.error());
    m_clients = std::move(clients);
    dispatch = ApplyProjectionLocked(*projection, client_id);
  }
  if (dispatch)
    DrainDispatchQueue();
  return {};
}

std::expected<void, std::string>
BreakPoints::ReplaceClientInstructionBreakpoints(const ClientId client_id,
                                                 std::vector<CodeBreakpoint> breakpoints)
{
  bool dispatch = false;
  {
    std::lock_guard lock(m_mutex);
    auto clients = m_clients;
    const auto client = clients.find(client_id);
    if (client == clients.end())
      return std::unexpected("unknown breakpoint client");
    client->second.instructions = std::move(breakpoints);
    const auto projection = BuildProjection(m_legacy_breakpoints, clients);
    if (!projection)
      return std::unexpected(projection.error());
    m_clients = std::move(clients);
    dispatch = ApplyProjectionLocked(*projection, client_id);
  }
  if (dispatch)
    DrainDispatchQueue();
  return {};
}

void BreakPoints::ClearClientBreakpoints(const ClientId client_id)
{
  bool dispatch = false;
  {
    std::lock_guard lock(m_mutex);
    auto client = m_clients.find(client_id);
    if (client == m_clients.end())
      return;
    client->second.sources.clear();
    client->second.instructions.clear();
    const auto projection = BuildProjection(m_legacy_breakpoints, m_clients);
    if (projection)
      dispatch = ApplyProjectionLocked(*projection, client_id);
  }
  if (dispatch)
    DrainDispatchQueue();
}

void BreakPoints::UnregisterClient(const ClientId client_id)
{
  bool dispatch = false;
  {
    std::lock_guard lock(m_mutex);
    if (m_clients.erase(client_id) == 0)
      return;
    const auto projection = BuildProjection(m_legacy_breakpoints, m_clients);
    if (projection)
      dispatch = ApplyProjectionLocked(*projection, client_id);
  }
  if (dispatch)
    DrainDispatchQueue();
  SynchronizeDispatch();
}

u64 BreakPoints::GetRevision() const
{
  return GetSnapshot()->revision;
}

std::expected<BreakPoints::Projection, std::string>
BreakPoints::BuildProjection(const std::map<u32, CodeBreakpoint>& legacy,
                             const std::unordered_map<ClientId, ClientBreakpoints>& clients) const
{
  struct Predicate
  {
    bool unconditional = false;
    std::set<std::string> conditions;
  };

  std::map<u32, std::vector<CodeBreakpoint>> claims_by_address;
  for (const auto& [_, breakpoint] : legacy)
    claims_by_address[breakpoint.address].push_back(breakpoint);
  for (const auto& [_, client] : clients)
  {
    for (const auto& [__, source] : client.sources)
    {
      for (const CodeBreakpoint& breakpoint : source)
        claims_by_address[breakpoint.address].push_back(breakpoint);
    }
    for (const CodeBreakpoint& breakpoint : client.instructions)
      claims_by_address[breakpoint.address].push_back(breakpoint);
  }

  const auto canonicalize = [](const std::optional<std::string>& condition)
      -> std::expected<std::optional<std::string>, std::string> {
    if (!condition)
      return std::nullopt;
    const std::string_view stripped = StripWhitespace(*condition);
    std::string canonical;
    canonical.reserve(stripped.size());
    for (const unsigned char character : stripped)
    {
      const bool is_ascii_whitespace = character == ' ' || character == '\t' || character == '\n' ||
                                       character == '\r' || character == '\f' || character == '\v';
      if (!is_ascii_whitespace)
        canonical.push_back(static_cast<char>(character));
    }
    const auto parsed = Expression::TryParse(canonical);
    if (!parsed)
      return std::unexpected(fmt::format("invalid breakpoint condition: {}", *condition));
    return parsed->GetText();
  };
  const auto strip_outer_parentheses = [](std::string_view condition) {
    while (condition.size() >= 2 && condition.front() == '(' && condition.back() == ')')
    {
      size_t depth = 0;
      bool encloses_entire_condition = true;
      for (size_t i = 0; i < condition.size(); ++i)
      {
        if (condition[i] == '(')
          ++depth;
        else if (condition[i] == ')')
          --depth;
        if (depth == 0 && i + 1 != condition.size())
        {
          encloses_entire_condition = false;
          break;
        }
      }
      if (!encloses_entire_condition)
        break;
      condition = condition.substr(1, condition.size() - 2);
    }
    return condition;
  };
  std::function<void(std::string_view, std::set<std::string>&)> add_disjuncts;
  add_disjuncts = [&](std::string_view condition, std::set<std::string>& disjuncts) {
    condition = strip_outer_parentheses(condition);
    size_t depth = 0;
    size_t operand_start = 0;
    bool split = false;
    for (size_t i = 0; i + 1 < condition.size(); ++i)
    {
      if (condition[i] == '(')
        ++depth;
      else if (condition[i] == ')')
        --depth;
      else if (depth == 0 && condition[i] == '|' && condition[i + 1] == '|')
      {
        add_disjuncts(condition.substr(operand_start, i - operand_start), disjuncts);
        operand_start = i + 2;
        ++i;
        split = true;
      }
    }
    if (split)
      add_disjuncts(condition.substr(operand_start), disjuncts);
    else
      disjuncts.emplace(condition);
  };
  const auto add_condition = [&](Predicate& predicate,
                                 const std::optional<std::string>& condition) {
    if (condition)
      add_disjuncts(*condition, predicate.conditions);
    else
      predicate.unconditional = true;
  };
  const auto predicates_equal = [](const Predicate& lhs, const Predicate& rhs) {
    if (lhs.unconditional || rhs.unconditional)
      return lhs.unconditional == rhs.unconditional;
    return lhs.conditions == rhs.conditions;
  };
  const auto build_condition =
      [](const Predicate& predicate) -> std::expected<std::optional<std::string>, std::string> {
    if (predicate.unconditional || predicate.conditions.empty())
      return std::nullopt;
    if (predicate.conditions.size() == 1)
      return *predicate.conditions.begin();

    std::string combined;
    for (const std::string& condition : predicate.conditions)
    {
      if (!combined.empty())
        combined += " || ";
      combined += fmt::format("({})", condition);
    }
    const auto parsed = Expression::TryParse(combined);
    if (!parsed)
      return std::unexpected("failed to aggregate breakpoint conditions");
    return parsed->GetText();
  };

  Projection projection;
  for (auto& [address, claims] : claims_by_address)
  {
    for (CodeBreakpoint& claim : claims)
    {
      auto condition = canonicalize(claim.condition);
      if (!condition)
        return std::unexpected(condition.error());
      claim.condition = std::move(*condition);
    }

    const bool any_enabled = std::ranges::any_of(claims, &CodeBreakpoint::is_enabled);
    CodeBreakpoint effective{
        .address = address, .is_enabled = any_enabled, .log_on_hit = false, .break_on_hit = false};
    if (!any_enabled)
    {
      projection.emplace(address, std::move(effective));
      continue;
    }

    Predicate break_predicate;
    Predicate log_predicate;
    Predicate display_predicate;
    for (const CodeBreakpoint& claim : claims)
    {
      if (!claim.is_enabled)
        continue;
      effective.break_on_hit |= claim.break_on_hit;
      effective.log_on_hit |= claim.log_on_hit;
      add_condition(display_predicate, claim.condition);
      if (claim.break_on_hit)
        add_condition(break_predicate, claim.condition);
      if (claim.log_on_hit)
        add_condition(log_predicate, claim.condition);
    }

    const Predicate* effective_predicate = &display_predicate;
    if (effective.break_on_hit && effective.log_on_hit)
    {
      if (!predicates_equal(break_predicate, log_predicate))
      {
        return std::unexpected(fmt::format(
            "incompatible breakpoint break/log condition policies at 0x{:08x}", address));
      }
      effective_predicate = &break_predicate;
    }
    else if (effective.break_on_hit)
    {
      effective_predicate = &break_predicate;
    }
    else if (effective.log_on_hit)
    {
      effective_predicate = &log_predicate;
    }

    auto condition = build_condition(*effective_predicate);
    if (!condition)
      return std::unexpected(condition.error());
    effective.condition = std::move(*condition);
    projection.emplace(address, std::move(effective));
  }
  return projection;
}

std::expected<bool, std::string> BreakPoints::ReplaceSiteLocked(CodeBreakpoint replacement)
{
  auto legacy = m_legacy_breakpoints;
  legacy[replacement.address] = std::move(replacement);

  const auto projection = BuildProjection(legacy, m_clients);
  if (!projection)
    return std::unexpected(projection.error());
  m_legacy_breakpoints = std::move(legacy);
  return ApplyProjectionLocked(*projection, std::nullopt);
}

bool BreakPoints::RemoveSiteLocked(const u32 address)
{
  auto legacy = m_legacy_breakpoints;
  auto clients = m_clients;
  if (!EraseSiteClaims(address, legacy, clients))
    return false;

  const auto projection = BuildProjection(legacy, clients);
  if (!projection)
    return false;
  m_legacy_breakpoints = std::move(legacy);
  m_clients = std::move(clients);
  return ApplyProjectionLocked(*projection, std::nullopt);
}

bool BreakPoints::EraseSiteClaims(const u32 address, std::map<u32, CodeBreakpoint>& legacy,
                                  std::unordered_map<ClientId, ClientBreakpoints>& clients)
{
  bool erased = legacy.erase(address) != 0;
  for (auto& [_, client] : clients)
  {
    for (auto source = client.sources.begin(); source != client.sources.end();)
    {
      const auto removed =
          std::erase_if(source->second, [address](const CodeBreakpoint& breakpoint) {
            return breakpoint.address == address;
          });
      erased |= removed != 0;
      if (source->second.empty())
        source = client.sources.erase(source);
      else
        ++source;
    }
    const auto removed =
        std::erase_if(client.instructions, [address](const CodeBreakpoint& breakpoint) {
          return breakpoint.address == address;
        });
    erased |= removed != 0;
  }
  return erased;
}

void BreakPoints::SetSiteClaimsEnabled(const u32 address, const bool enabled,
                                       std::map<u32, CodeBreakpoint>& legacy,
                                       std::unordered_map<ClientId, ClientBreakpoints>& clients)
{
  if (auto breakpoint = legacy.find(address); breakpoint != legacy.end())
    breakpoint->second.is_enabled = enabled;
  for (auto& [_, client] : clients)
  {
    for (auto& [__, source] : client.sources)
    {
      for (CodeBreakpoint& breakpoint : source)
      {
        if (breakpoint.address == address)
          breakpoint.is_enabled = enabled;
      }
    }
    for (CodeBreakpoint& breakpoint : client.instructions)
    {
      if (breakpoint.address == address)
        breakpoint.is_enabled = enabled;
    }
  }
}

bool BreakPoints::ApplyProjectionLocked(Projection projection, const std::optional<ClientId> origin)
{
  std::vector<Change> changes;
  for (const auto& [address, old_breakpoint] : m_projection)
  {
    const auto next = projection.find(address);
    if (next == projection.end())
      changes.push_back({ChangeReason::Removed, old_breakpoint});
    else if (next->second != old_breakpoint)
      changes.push_back({ChangeReason::Changed, next->second});
  }
  for (const auto& [address, breakpoint] : projection)
  {
    if (!m_projection.contains(address))
      changes.push_back({ChangeReason::New, breakpoint});
  }
  if (changes.empty())
    return false;

  for (const Change& change : changes)
    m_system.GetJitInterface().InvalidateICache(change.breakpoint.address, 4, true);

  std::vector<CodeBreakpoint> snapshot;
  snapshot.reserve(projection.size());
  for (const auto& [_, breakpoint] : projection)
    snapshot.push_back(breakpoint);

  auto event = std::make_shared<Event>();
  event->revision = ++m_revision;
  event->origin = origin;
  event->changes = std::move(changes);
  event->breakpoints = std::move(snapshot);
  m_projection = std::move(projection);
  PublishSnapshotLocked();
  m_pending_dispatches.push_back({std::move(event)});
  return true;
}

void BreakPoints::PublishSnapshotLocked()
{
  auto snapshot = std::make_shared<Snapshot>();
  snapshot->revision = m_revision;
  snapshot->breaking_enabled = m_breaking_enabled;
  snapshot->breakpoints.reserve(m_projection.size());
  for (const auto& [_, breakpoint] : m_projection)
  {
    TBreakPoint physical;
    physical.address = breakpoint.address;
    physical.is_enabled = breakpoint.is_enabled;
    physical.log_on_hit = breakpoint.log_on_hit;
    physical.break_on_hit = breakpoint.break_on_hit;
    if (breakpoint.condition)
      physical.condition = Expression::TryParse(*breakpoint.condition);
    snapshot->breakpoints.emplace_back(std::move(physical));
  }
  if (m_temporary_address)
  {
    TBreakPoint temporary;
    temporary.address = *m_temporary_address;
    temporary.is_enabled = true;
    temporary.break_on_hit = true;
    snapshot->temporary_breakpoint.emplace(std::move(temporary));
  }
  m_snapshot.store(std::move(snapshot));
}

void BreakPoints::DrainDispatchQueue()
{
  std::lock_guard dispatch_lock(m_dispatch_mutex);
  if (m_is_draining)
    return;
  m_is_draining = true;
  while (true)
  {
    std::shared_ptr<const Event> event;
    std::vector<EventCallback> callbacks;
    {
      std::lock_guard lock(m_mutex);
      if (m_pending_dispatches.empty())
        break;
      event = std::move(m_pending_dispatches.front().event);
      m_pending_dispatches.pop_front();
      callbacks.reserve(m_clients.size());
      for (const auto& [_, client] : m_clients)
      {
        if (client.callback)
          callbacks.push_back(client.callback);
      }
    }
    for (const EventCallback& callback : callbacks)
      callback(event);
    Host_PPCBreakpointsChanged();
  }
  m_is_draining = false;
}

void BreakPoints::SynchronizeDispatch()
{
  std::lock_guard lock(m_dispatch_mutex);
}

MemChecks::MemChecks(Core::System& system) : m_system(system)
{
}

MemChecks::~MemChecks() = default;

MemChecks::TMemChecksStr MemChecks::GetStrings() const
{
  TMemChecksStr mc_strings;
  for (const TMemCheck& mc : m_mem_checks)
  {
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss << fmt::format("${:08x} {:08x} ", mc.start_address, mc.end_address);
    if (mc.is_enabled)
      ss << 'n';
    if (mc.is_break_on_read)
      ss << 'r';
    if (mc.is_break_on_write)
      ss << 'w';
    if (mc.log_on_hit)
      ss << 'l';
    if (mc.break_on_hit)
      ss << 'b';
    if (mc.condition)
      ss << "c " << mc.condition->GetText();

    mc_strings.emplace_back(ss.str());
  }

  return mc_strings;
}

void MemChecks::AddFromStrings(const TMemChecksStr& mc_strings)
{
  const Core::CPUThreadGuard guard(m_system);
  DelayedMemCheckUpdate delayed_update(this);

  for (const std::string& mc_string : mc_strings)
  {
    TMemCheck mc;
    std::istringstream iss(mc_string);
    iss.imbue(std::locale::classic());

    if (iss.peek() == '$')
      iss.ignore();

    std::string flags;
    iss >> std::hex >> mc.start_address >> mc.end_address >> flags;

    mc.is_ranged = mc.start_address != mc.end_address;
    mc.is_enabled = flags.contains('n');
    mc.is_break_on_read = flags.contains('r');
    mc.is_break_on_write = flags.contains('w');
    mc.log_on_hit = flags.contains('l');
    mc.break_on_hit = flags.contains('b');
    if (flags.contains('c'))
    {
      iss >> std::ws;
      std::string condition;
      std::getline(iss, condition);
      mc.condition = Expression::TryParse(condition);
    }

    delayed_update |= Add(std::move(mc));
  }
}

DelayedMemCheckUpdate MemChecks::Add(TMemCheck memory_check)
{
  const Core::CPUThreadGuard guard(m_system);

  // Check for existing breakpoint, and overwrite with new info.
  // This is assuming we usually want the new breakpoint over an old one.
  const u32 address = memory_check.start_address;
  auto old_mem_check = std::ranges::find(m_mem_checks, address, &TMemCheck::start_address);
  if (old_mem_check != m_mem_checks.end())
  {
    memory_check.is_enabled = old_mem_check->is_enabled;  // Preserve enabled status
    *old_mem_check = std::move(memory_check);
    old_mem_check->num_hits = 0;
  }
  else
  {
    m_mem_checks.emplace_back(std::move(memory_check));
  }

  return DelayedMemCheckUpdate(this, true);
}

bool MemChecks::ToggleEnable(u32 address)
{
  auto iter = std::ranges::find(m_mem_checks, address, &TMemCheck::start_address);

  if (iter == m_mem_checks.end())
    return false;

  iter->is_enabled = !iter->is_enabled;
  return true;
}

void MemChecks::EnableBreaking(bool enabled)
{
  m_breaking_enabled = enabled;
  Update();
}

DelayedMemCheckUpdate MemChecks::Remove(u32 address)
{
  const auto iter = std::ranges::find(m_mem_checks, address, &TMemCheck::start_address);

  if (iter == m_mem_checks.cend())
    return DelayedMemCheckUpdate(this, false);

  const Core::CPUThreadGuard guard(m_system);
  m_mem_checks.erase(iter);

  return DelayedMemCheckUpdate(this, true);
}

void MemChecks::Clear()
{
  const Core::CPUThreadGuard guard(m_system);
  m_mem_checks.clear();
  Update();
}

void MemChecks::Update()
{
  const Core::CPUThreadGuard guard(m_system);

  const bool registers_changed = UpdateRegistersUsedInConditions();

  // If we've added a first memcheck, clear the JIT cache so it can switch to watchpoint-compatible
  // code. Or, if we've added a memcheck whose condition wants to read from a new register, clear
  // the JIT cache to make the slow memory access code flush that register. And conversely, if the
  // aforementioned functionality is no longer needed, clear the JIT cache to switch to faster code.
  if (registers_changed || m_mem_breakpoints_set != HasAny())
  {
    m_system.GetJitInterface().ClearCache(guard);
    m_mem_breakpoints_set = HasAny();
  }

  m_system.GetMMU().DBATUpdated();
}

bool MemChecks::UpdateRegistersUsedInConditions()
{
  BitSet32 gprs_used, fprs_used;
  for (TMemCheck& mem_check : m_mem_checks)
  {
    if (mem_check.condition)
    {
      gprs_used |= mem_check.condition->GetGPRsUsed();
      fprs_used |= mem_check.condition->GetFPRsUsed();
    }
  }

  const bool registers_changed =
      gprs_used != m_gprs_used_in_conditions || fprs_used != m_fprs_used_in_conditions;

  m_gprs_used_in_conditions = gprs_used;
  m_fprs_used_in_conditions = fprs_used;

  return registers_changed;
}

TMemCheck* MemChecks::GetMemCheck(u32 address, size_t size)
{
  const auto iter = std::ranges::find_if(m_mem_checks, [address, size](const auto& mc) {
    return mc.end_address >= address && address + size - 1 >= mc.start_address;
  });

  // None found
  if (iter == m_mem_checks.cend())
    return nullptr;

  return &*iter;
}

bool MemChecks::OverlapsMemcheck(u32 address, u32 length) const
{
  if (!HasAny())
    return false;

  const u32 page_end_suffix = length - 1;
  const u32 page_end_address = address | page_end_suffix;

  return std::ranges::any_of(m_mem_checks, [&](const auto& mc) {
    return ((mc.start_address | page_end_suffix) == page_end_address ||
            (mc.end_address | page_end_suffix) == page_end_address) ||
           ((mc.start_address | page_end_suffix) < page_end_address &&
            (mc.end_address | page_end_suffix) > page_end_address);
  });
}

bool TMemCheck::Action(Core::System& system, u64 value, u32 addr, bool write, size_t size, u32 pc)
{
  if (!is_enabled)
    return false;

  if (((write && is_break_on_write) || (!write && is_break_on_read)) &&
      EvaluateCondition(system, this->condition))
  {
    if (log_on_hit)
    {
      auto& ppc_symbol_db = system.GetPPCSymbolDB();
      NOTICE_LOG_FMT(MEMMAP, "MBP {:08x} ({}) {}{} {:x} at {:08x} ({})", pc,
                     ppc_symbol_db.GetDescription(pc), write ? "Write" : "Read", size * 8, value,
                     addr, ppc_symbol_db.GetDescription(addr));
    }
    if (break_on_hit)
      return true;
  }
  return false;
}
