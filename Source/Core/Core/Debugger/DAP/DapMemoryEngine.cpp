// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapMemoryEngine.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <type_traits>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/GekkoDisassembler.h"
#include "Common/StringUtil.h"
#include "Core/Core.h"
#include "Core/Debugger/DAP/DapJson.h"
#include "Core/HW/DSP.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/System.h"

namespace DAP
{
namespace
{
constexpr u32 ARAM_BASE_ADDRESS = 0x7e000000;
constexpr u32 MAX_RESULT_PAGE_SIZE = 4096;
constexpr u64 MAX_RESULT_PAGE_RAW_BYTES = 1ull << 20;
constexpr u64 MAX_SNAPSHOT_BYTES = 256ull << 20;
constexpr u64 MAX_RETAINED_BYTES = 256ull << 20;
constexpr u64 MAX_BYTE_COMPARISON_WORK = 1ull << 30;
constexpr u64 MAX_PPC_DISASSEMBLY_WORK = 1ull << 22;
constexpr size_t MAX_SCAN_RANGES = 1024;
constexpr size_t MAX_SCAN_REGIONS = 3;
constexpr size_t MAX_NUMERIC_VALUE_LENGTH = 128;
constexpr size_t RESULT_INDEX_BLOCK_BYTES = 4096;
constexpr int MAX_SCANS = 8;
constexpr size_t MAX_UNDO_GENERATIONS = 16;

std::mutex s_core_scan_mutex;
thread_local const DapMemoryEngine* s_memory_scan_worker = nullptr;

MemoryScanBudget& GetProcessMemoryScanBudget()
{
  static MemoryScanBudget budget(MAX_RETAINED_BYTES);
  return budget;
}

template <typename T>
T ReadBigEndian(const u8* bytes)
{
  if constexpr (std::is_floating_point_v<T>)
  {
    using U = std::conditional_t<sizeof(T) == 4, u32, u64>;
    U value = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
      value = static_cast<U>((value << 8) | bytes[i]);
    return std::bit_cast<T>(value);
  }
  else
  {
    using U = std::make_unsigned_t<T>;
    U value = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
      value = static_cast<U>((value << 8) | bytes[i]);
    if constexpr (std::is_signed_v<T>)
      return std::bit_cast<T>(value);
    else
      return value;
  }
}

template <typename T>
std::optional<T> ParseNumeric(std::string_view text)
{
  if constexpr (std::is_floating_point_v<T>)
  {
    if (text.starts_with('+'))
      text.remove_prefix(1);
    return Common::ParseFiniteDecimal<T>(text);
  }
  else
  {
    int base = 10;
    bool negative = false;
    if constexpr (std::is_signed_v<T>)
    {
      if (text.starts_with('-'))
      {
        negative = true;
        text.remove_prefix(1);
      }
    }
    if (text.starts_with("0x") || text.starts_with("0X"))
    {
      base = 16;
      text.remove_prefix(2);
    }
    if (text.empty())
      return std::nullopt;

    u64 parsed = 0;
    const auto [ptr, error] = std::from_chars(text.data(), text.data() + text.size(), parsed, base);
    if (error != std::errc{} || ptr != text.data() + text.size())
      return std::nullopt;
    if constexpr (std::is_signed_v<T>)
    {
      const u64 positive_max = static_cast<u64>(std::numeric_limits<T>::max());
      if (negative)
      {
        const u64 negative_max = positive_max + 1;
        if (parsed > negative_max)
          return std::nullopt;
        if (parsed == negative_max)
          return std::numeric_limits<T>::min();
        return static_cast<T>(-static_cast<s64>(parsed));
      }
      if (parsed > positive_max)
        return std::nullopt;
    }
    else if (parsed > std::numeric_limits<T>::max())
    {
      return std::nullopt;
    }
    return static_cast<T>(parsed);
  }
}

template <typename T>
bool DifferenceEquals(T lhs, T rhs, T difference)
{
  if constexpr (std::is_floating_point_v<T>)
  {
    return lhs - rhs == difference;
  }
  else
  {
    using U = std::make_unsigned_t<T>;
    if constexpr (std::is_signed_v<T>)
    {
      if (difference < 0)
      {
        if (lhs > rhs)
          return false;
        const U magnitude = U{} - static_cast<U>(difference);
        return static_cast<U>(rhs) - static_cast<U>(lhs) == magnitude;
      }
    }
    if (lhs < rhs)
      return false;
    return static_cast<U>(lhs) - static_cast<U>(rhs) == static_cast<U>(difference);
  }
}

template <typename T>
bool MatchesFilter(MemoryScanFilter filter, T current, T previous, T value, T value2)
{
  switch (filter)
  {
  case MemoryScanFilter::Exact:
    return current == value;
  case MemoryScanFilter::NotEqual:
    return current != value;
  case MemoryScanFilter::Between:
    return current >= value && current <= value2;
  case MemoryScanFilter::GreaterThan:
    return current > value;
  case MemoryScanFilter::GreaterOrEqual:
    return current >= value;
  case MemoryScanFilter::LessThan:
    return current < value;
  case MemoryScanFilter::LessOrEqual:
    return current <= value;
  case MemoryScanFilter::Unknown:
    return true;
  case MemoryScanFilter::Changed:
    if constexpr (std::is_floating_point_v<T>)
    {
      if (std::isnan(current) || std::isnan(previous))
      {
        using U = std::conditional_t<sizeof(T) == 4, u32, u64>;
        return std::bit_cast<U>(current) != std::bit_cast<U>(previous);
      }
      return current != previous;
    }
    else
      return current != previous;
  case MemoryScanFilter::Unchanged:
    if constexpr (std::is_floating_point_v<T>)
    {
      if (std::isnan(current) || std::isnan(previous))
      {
        using U = std::conditional_t<sizeof(T) == 4, u32, u64>;
        return std::bit_cast<U>(current) == std::bit_cast<U>(previous);
      }
      return current == previous;
    }
    else
      return current == previous;
  case MemoryScanFilter::Increased:
    return current > previous;
  case MemoryScanFilter::Decreased:
    return current < previous;
  case MemoryScanFilter::IncreasedBy:
    return DifferenceEquals(current, previous, value);
  case MemoryScanFilter::DecreasedBy:
    return DifferenceEquals(previous, current, value);
  case MemoryScanFilter::Mnemonic:
  case MemoryScanFilter::ValidInstruction:
    return false;
  }
  return false;
}

bool NeedsValue(MemoryScanFilter filter)
{
  switch (filter)
  {
  case MemoryScanFilter::Exact:
  case MemoryScanFilter::NotEqual:
  case MemoryScanFilter::Between:
  case MemoryScanFilter::GreaterThan:
  case MemoryScanFilter::GreaterOrEqual:
  case MemoryScanFilter::LessThan:
  case MemoryScanFilter::LessOrEqual:
  case MemoryScanFilter::IncreasedBy:
  case MemoryScanFilter::DecreasedBy:
    return true;
  default:
    return false;
  }
}

bool IsPreviousValueFilter(MemoryScanFilter filter)
{
  return filter == MemoryScanFilter::Changed || filter == MemoryScanFilter::Unchanged ||
         filter == MemoryScanFilter::Increased || filter == MemoryScanFilter::Decreased ||
         filter == MemoryScanFilter::IncreasedBy || filter == MemoryScanFilter::DecreasedBy;
}

bool GetBit(const std::vector<u8>& bits, u64 index)
{
  return (bits[index / 8] & (1u << (index % 8))) != 0;
}

void SetBit(std::vector<u8>& bits, u64 index)
{
  bits[index / 8] |= static_cast<u8>(1u << (index % 8));
}

void ClearBit(std::vector<u8>& bits, u64 index)
{
  bits[index / 8] &= static_cast<u8>(~(1u << (index % 8)));
}

bool IsValidUtf8(std::string_view value)
{
  return UTF16ToUTF8(UTF8ToUTF16(value)) == value;
}
}  // namespace

struct MemoryScanBudget::Charge::State
{
  explicit State(u64 limit_) : limit(limit_) {}
  std::atomic<u64> used{0};
  const u64 limit;
};

MemoryScanBudget::Charge::Charge(std::shared_ptr<State> state, u64 bytes)
    : m_state(std::move(state)), m_bytes(bytes)
{
}

MemoryScanBudget::Charge::Charge(Charge&& other) noexcept
    : m_state(std::move(other.m_state)), m_bytes(std::exchange(other.m_bytes, 0))
{
}

MemoryScanBudget::Charge& MemoryScanBudget::Charge::operator=(Charge&& other) noexcept
{
  if (this != &other)
  {
    Release();
    m_state = std::move(other.m_state);
    m_bytes = std::exchange(other.m_bytes, 0);
  }
  return *this;
}

MemoryScanBudget::Charge::~Charge()
{
  Release();
}

MemoryScanBudget::Charge MemoryScanBudget::Charge::Split(u64 bytes)
{
  ASSERT(m_state && bytes <= m_bytes);
  m_bytes -= bytes;
  return Charge(m_state, bytes);
}

void MemoryScanBudget::Charge::Release()
{
  if (m_bytes != 0)
  {
    m_state->used.fetch_sub(m_bytes, std::memory_order_relaxed);
    m_bytes = 0;
  }
}

MemoryScanBudget::MemoryScanBudget(u64 limit) : m_state(std::make_shared<Charge::State>(limit))
{
}

std::optional<MemoryScanBudget::Charge> MemoryScanBudget::TryReserve(u64 bytes) const
{
  u64 used = m_state->used.load(std::memory_order_relaxed);
  do
  {
    if (used > m_state->limit || bytes > m_state->limit - used)
      return std::nullopt;
  } while (!m_state->used.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
  return Charge(m_state, bytes);
}

u64 MemoryScanBudget::GetUsedBytes() const
{
  return m_state->used.load(std::memory_order_relaxed);
}

DapMemoryEngine::DapMemoryEngine(Core::System& system, TerminalCallback terminal_callback)
    : DapMemoryEngine(system, std::move(terminal_callback), GetProcessMemoryScanBudget())
{
}

DapMemoryEngine::DapMemoryEngine(Core::System& system, TerminalCallback terminal_callback,
                                 MemoryScanBudget budget)
    : m_system(system), m_terminal_callback(std::move(terminal_callback)),
      m_budget(std::move(budget))
{
}

DapMemoryEngine::~DapMemoryEngine()
{
  m_cancelled.store(true);
  std::lock_guard lock(m_worker_mutex);
  if (m_worker.joinable())
  {
    if (s_memory_scan_worker == this)
      m_worker.detach();
    else
      m_worker.join();
  }
}

std::vector<MemoryRegionInfo> DapMemoryEngine::GetMemoryRegions() const
{
  auto& memory = m_system.GetMemory();
  std::vector<MemoryRegionInfo> regions;
  if (memory.GetRAM() != nullptr)
    regions.push_back({"mem1", "MEM1", Memory::MEM1_BASE_ADDR, memory.GetRamSizeReal()});
  if (m_system.IsWii())
  {
    if (memory.GetEXRAM() != nullptr)
      regions.push_back({"mem2", "MEM2", Memory::MEM2_BASE_ADDR, memory.GetExRamSizeReal()});
  }
  else if (m_system.GetDSP().GetARAMPtr() != nullptr)
  {
    regions.push_back({"aram", "ARAM", ARAM_BASE_ADDRESS, m_system.GetDSP().GetARAMSize()});
  }
  return regions;
}

std::expected<std::vector<DapMemoryEngine::ResolvedRange>, std::string>
DapMemoryEngine::ResolveRanges(const MemoryScanStartConfig& config) const
{
  if (config.regions.size() > MAX_SCAN_REGIONS)
    return std::unexpected("too many memory regions");
  if (config.ranges.size() > MAX_SCAN_RANGES)
    return std::unexpected("too many scan ranges");
  const std::vector<MemoryRegionInfo> available = GetMemoryRegions();
  if (available.empty())
    return std::unexpected("no emulated memory regions are available");

  std::vector<std::string> selected = config.regions;
  if (selected.empty())
    selected.emplace_back("mem1");
  std::ranges::sort(selected);
  if (std::adjacent_find(selected.begin(), selected.end()) != selected.end())
    return std::unexpected("memory regions must not be duplicated");

  auto find_region = [&](std::string_view id) -> const MemoryRegionInfo* {
    const auto it = std::ranges::find(available, id, &MemoryRegionInfo::id);
    return it == available.end() ? nullptr : &*it;
  };
  for (const std::string& id : selected)
  {
    if (find_region(id) == nullptr)
      return std::unexpected(fmt::format("memory region '{}' is unavailable", id));
  }

  std::vector<ResolvedRange> ranges;
  if (config.ranges.empty())
  {
    for (const std::string& id : selected)
    {
      const MemoryRegionInfo* region = find_region(id);
      ranges.push_back({region->base_address, region->size, region->id});
    }
  }
  else
  {
    for (const MemoryScanRange& requested : config.ranges)
    {
      if (requested.end <= requested.start)
        return std::unexpected("scan range end must be greater than start");

      const MemoryRegionInfo* owner = nullptr;
      for (const std::string& id : selected)
      {
        const MemoryRegionInfo* region = find_region(id);
        const u64 region_end = static_cast<u64>(region->base_address) + region->size;
        if (requested.start >= region->base_address &&
            static_cast<u64>(requested.end) <= region_end)
        {
          owner = region;
          break;
        }
      }
      if (owner == nullptr)
        return std::unexpected("scan range is outside the selected memory regions");
      ranges.push_back({requested.start, requested.end - requested.start, owner->id});
    }
  }

  std::ranges::sort(ranges, {}, &ResolvedRange::start);
  std::vector<ResolvedRange> normalized;
  normalized.reserve(ranges.size());
  u64 total_size = 0;
  for (ResolvedRange& range : ranges)
  {
    if (!normalized.empty())
    {
      ResolvedRange& previous = normalized.back();
      const u64 previous_end = static_cast<u64>(previous.start) + previous.size;
      if (previous_end > range.start)
        return std::unexpected("scan ranges must not overlap");
      if (previous_end == range.start && previous.region_id == range.region_id)
      {
        previous.size += range.size;
        total_size += range.size;
        if (total_size > MAX_SNAPSHOT_BYTES)
          return std::unexpected("requested snapshot exceeds the 256 MiB limit");
        continue;
      }
    }
    total_size += range.size;
    if (total_size > MAX_SNAPSHOT_BYTES)
      return std::unexpected("requested snapshot exceeds the 256 MiB limit");
    normalized.emplace_back(std::move(range));
  }
  return normalized;
}

std::expected<std::vector<DapMemoryEngine::SnapshotRange>, std::string>
DapMemoryEngine::CaptureSnapshot(const std::vector<ResolvedRange>& ranges,
                                 std::atomic<bool>& cancelled) const
{
  std::vector<SnapshotRange> snapshot;
  snapshot.reserve(ranges.size());
  auto& memory = m_system.GetMemory();

  for (const ResolvedRange& range : ranges)
  {
    if (cancelled.load())
      return std::unexpected("cancelled");

    const u8* source = nullptr;
    u32 offset = 0;
    if (range.region_id == "mem1")
    {
      source = memory.GetRAM();
      offset = range.start - Memory::MEM1_BASE_ADDR;
    }
    else if (range.region_id == "mem2")
    {
      source = memory.GetEXRAM();
      offset = range.start - Memory::MEM2_BASE_ADDR;
    }
    else if (range.region_id == "aram")
    {
      source = m_system.GetDSP().GetARAMPtr();
      offset = range.start - ARAM_BASE_ADDRESS;
    }
    if (source == nullptr)
      return std::unexpected(fmt::format("memory region '{}' became unavailable", range.region_id));

    SnapshotRange& out = snapshot.emplace_back();
    out.start = range.start;
    out.bytes.resize(range.size);
    constexpr size_t chunk_size = 1u << 20;
    for (size_t copied = 0; copied < range.size; copied += chunk_size)
    {
      if (cancelled.load())
        return std::unexpected("cancelled");
      const size_t count = std::min(chunk_size, static_cast<size_t>(range.size) - copied);
      std::memcpy(out.bytes.data() + copied, source + offset + copied, count);
    }
  }
  return snapshot;
}

u32 DapMemoryEngine::DataTypeSize(const MemoryScanStartConfig& config)
{
  switch (config.data_type)
  {
  case MemoryScanDataType::U8:
  case MemoryScanDataType::S8:
    return 1;
  case MemoryScanDataType::U16:
  case MemoryScanDataType::S16:
    return 2;
  case MemoryScanDataType::U32:
  case MemoryScanDataType::S32:
  case MemoryScanDataType::F32:
  case MemoryScanDataType::PpcInstruction:
    return 4;
  case MemoryScanDataType::U64:
  case MemoryScanDataType::S64:
  case MemoryScanDataType::F64:
    return 8;
  case MemoryScanDataType::Bytes:
  case MemoryScanDataType::String:
    return static_cast<u32>(config.byte_value.size());
  }
  return 1;
}

std::optional<DapMemoryEngine::NumericValue>
DapMemoryEngine::ParseValue(MemoryScanDataType data_type, std::string_view value)
{
#define PARSE_CASE(kind, type)                                                                     \
  case MemoryScanDataType::kind:                                                                   \
    if (const auto parsed = ParseNumeric<type>(value))                                             \
      return NumericValue{*parsed};                                                                \
    return std::nullopt
  switch (data_type)
  {
    PARSE_CASE(U8, u8);
    PARSE_CASE(U16, u16);
    PARSE_CASE(U32, u32);
    PARSE_CASE(U64, u64);
    PARSE_CASE(S8, s8);
    PARSE_CASE(S16, s16);
    PARSE_CASE(S32, s32);
    PARSE_CASE(S64, s64);
    PARSE_CASE(F32, float);
    PARSE_CASE(F64, double);
  case MemoryScanDataType::Bytes:
  case MemoryScanDataType::String:
    return std::nullopt;
  case MemoryScanDataType::PpcInstruction:
    return ParseValue(MemoryScanDataType::U32, value);
  }
#undef PARSE_CASE
  return std::nullopt;
}

std::optional<std::string> DapMemoryEngine::ValidateFilter(MemoryScanDataType data_type,
                                                           MemoryScanFilter filter,
                                                           const std::optional<std::string>& value,
                                                           const std::optional<std::string>& value2,
                                                           bool has_previous)
{
  if (!has_previous && IsPreviousValueFilter(filter))
    return "this filter requires a previous scan generation";
  if (data_type == MemoryScanDataType::Bytes || data_type == MemoryScanDataType::String)
  {
    if (filter != MemoryScanFilter::Exact && filter != MemoryScanFilter::NotEqual &&
        filter != MemoryScanFilter::Changed && filter != MemoryScanFilter::Unchanged)
    {
      return "this filter is unsupported for byte-pattern scans";
    }
    if ((filter == MemoryScanFilter::Exact || filter == MemoryScanFilter::NotEqual) && !value)
      return "this byte-pattern filter requires value";
    if ((filter == MemoryScanFilter::Changed || filter == MemoryScanFilter::Unchanged) && value)
      return "this byte-pattern filter does not accept value";
    if (value2)
      return "byte-pattern scans do not support value2";
    return std::nullopt;
  }
  if (data_type == MemoryScanDataType::PpcInstruction)
  {
    if (filter != MemoryScanFilter::Exact && filter != MemoryScanFilter::Mnemonic &&
        filter != MemoryScanFilter::ValidInstruction && filter != MemoryScanFilter::Changed &&
        filter != MemoryScanFilter::Unchanged)
    {
      return "this filter is unsupported for PPC instruction scans";
    }
    if ((filter == MemoryScanFilter::Exact || filter == MemoryScanFilter::Mnemonic) && !value)
      return "this PPC instruction filter requires value";
    if ((filter == MemoryScanFilter::ValidInstruction || filter == MemoryScanFilter::Changed ||
         filter == MemoryScanFilter::Unchanged) &&
        value)
    {
      return "this PPC instruction filter does not accept value";
    }
    if (value2)
      return "PPC instruction scans do not support value2";
    if (filter == MemoryScanFilter::Exact && !ParseValue(data_type, *value))
      return "invalid PPC instruction word";
    if (filter == MemoryScanFilter::Exact && value->size() > MAX_NUMERIC_VALUE_LENGTH)
      return "numeric scan value is too long";
    if (filter == MemoryScanFilter::Mnemonic &&
        (value->empty() || value->size() > 32 || !std::ranges::all_of(*value, [](unsigned char c) {
           return std::isalnum(c) || c == '_' || c == '.' || c == '+' || c == '-';
         })))
    {
      return "invalid PPC instruction mnemonic";
    }
    return std::nullopt;
  }
  if (filter == MemoryScanFilter::Mnemonic || filter == MemoryScanFilter::ValidInstruction)
    return "this filter requires a PPC instruction scan";
  if (NeedsValue(filter) && !value)
    return "this filter requires value";
  if (!NeedsValue(filter) && value)
    return "this filter does not accept value";
  if (filter == MemoryScanFilter::Between && !value2)
    return "between requires value2";
  if (filter != MemoryScanFilter::Between && value2)
    return "only between accepts value2";
  if ((value && value->size() > MAX_NUMERIC_VALUE_LENGTH) ||
      (value2 && value2->size() > MAX_NUMERIC_VALUE_LENGTH))
  {
    return "numeric scan value is too long";
  }
  if (value && !ParseValue(data_type, *value))
    return "invalid numeric scan value";
  if (value2 && !ParseValue(data_type, *value2))
    return "invalid numeric scan value2";
  return std::nullopt;
}

std::string DapMemoryEngine::FormatValue(MemoryScanDataType data_type, const u8* bytes)
{
#define FORMAT_CASE(kind, type)                                                                    \
  case MemoryScanDataType::kind:                                                                   \
    return fmt::format("{}", ReadBigEndian<type>(bytes))
  switch (data_type)
  {
    FORMAT_CASE(U8, u8);
    FORMAT_CASE(U16, u16);
    FORMAT_CASE(U32, u32);
    FORMAT_CASE(U64, u64);
    FORMAT_CASE(S8, s8);
    FORMAT_CASE(S16, s16);
    FORMAT_CASE(S32, s32);
    FORMAT_CASE(S64, s64);
    FORMAT_CASE(F32, float);
    FORMAT_CASE(F64, double);
  case MemoryScanDataType::Bytes:
  case MemoryScanDataType::String:
    return {};
  case MemoryScanDataType::PpcInstruction:
    return fmt::format("0x{:08x}", ReadBigEndian<u32>(bytes));
  }
#undef FORMAT_CASE
  return {};
}

std::expected<std::shared_ptr<DapMemoryEngine::Generation>, std::string>
DapMemoryEngine::BuildGeneration(const Scan& scan, std::vector<SnapshotRange> snapshot,
                                 const std::shared_ptr<const Generation>& previous,
                                 MemoryScanFilter filter, const std::optional<std::string>& value,
                                 const std::optional<std::string>& value2,
                                 std::atomic<bool>& cancelled,
                                 MemoryScanBudget::Charge reservation) const
{
  if (const std::optional<std::string> error =
          ValidateFilter(scan.config.data_type, filter, value, value2, previous != nullptr))
    return std::unexpected(*error);

  std::optional<NumericValue> parsed_value;
  std::optional<NumericValue> parsed_value2;
  if (scan.config.data_type != MemoryScanDataType::Bytes &&
      scan.config.data_type != MemoryScanDataType::String &&
      scan.config.data_type != MemoryScanDataType::PpcInstruction)
  {
    parsed_value = value ? ParseValue(scan.config.data_type, *value) : NumericValue{u8{0}};
    parsed_value2 = value2 ? ParseValue(scan.config.data_type, *value2) : NumericValue{u8{0}};
    if (!parsed_value || !parsed_value2)
      return std::unexpected("invalid numeric scan value");
  }

  std::vector<u8> byte_target;
  if ((scan.config.data_type == MemoryScanDataType::Bytes ||
       scan.config.data_type == MemoryScanDataType::String) &&
      (filter == MemoryScanFilter::Exact || filter == MemoryScanFilter::NotEqual))
  {
    if (previous)
    {
      if (scan.config.data_type == MemoryScanDataType::Bytes)
      {
        const std::optional<std::vector<u8>> decoded =
            value ? Json::Base64Decode(*value) : std::nullopt;
        if (!decoded || decoded->size() != scan.config.byte_value.size())
          return std::unexpected("byte-pattern scan value must match the original pattern width");
        byte_target = *decoded;
      }
      else
      {
        if (!value || value->size() != scan.config.byte_value.size())
          return std::unexpected("string scan value must match the original encoded width");
        byte_target.assign(value->begin(), value->end());
      }
    }
    else
    {
      byte_target = scan.config.byte_value;
    }
  }

  const u32 width = DataTypeSize(scan.config);
  const u32 stride = scan.config.aligned ? width : 1;
  auto generation = std::make_shared<Generation>();
  auto generation_ranges = std::make_shared<SnapshotStorage>();
  u64 snapshot_bytes = 0;
  for (const SnapshotRange& range : snapshot)
    snapshot_bytes += range.bytes.size();
  generation_ranges->ranges = std::move(snapshot);
  generation_ranges->budget_charge = reservation.Split(snapshot_bytes);
  generation->budget_charge = std::move(reservation);

  u64 total_candidates = 0;
  for (SnapshotRange& range : generation_ranges->ranges)
  {
    range.first_candidate = total_candidates;
    range.candidate_offset =
        scan.config.aligned ? static_cast<u32>((width - (range.start % width)) % width) : 0;
    range.candidate_count = range.bytes.size() < range.candidate_offset + width ?
                                0 :
                                (range.bytes.size() - range.candidate_offset - width) / stride + 1;
    total_candidates += range.candidate_count;
  }
  generation->ranges = generation_ranges;
  generation->candidates.assign(static_cast<size_t>((total_candidates + 7) / 8), 0);

  auto process = [&]<typename T>() -> std::expected<void, std::string> {
    const T target = value ? std::get<T>(*parsed_value) : T{};
    const T target2 = value2 ? std::get<T>(*parsed_value2) : T{};
    for (size_t range_index = 0; range_index < generation->ranges->ranges.size(); ++range_index)
    {
      const SnapshotRange& current_range = generation->ranges->ranges[range_index];
      const SnapshotRange* previous_range =
          previous ? &previous->ranges->ranges[range_index] : nullptr;
      for (u64 local = 0; local < current_range.candidate_count; ++local)
      {
        const u64 global = current_range.first_candidate + local;
        if ((global & 0xffff) == 0 && cancelled.load())
          return std::unexpected("cancelled");
        if (previous && !GetBit(previous->candidates, global))
          continue;

        const size_t offset = current_range.candidate_offset + static_cast<size_t>(local * stride);
        const T current = ReadBigEndian<T>(current_range.bytes.data() + offset);
        const T old =
            previous_range ? ReadBigEndian<T>(previous_range->bytes.data() + offset) : T{};
        if (MatchesFilter(filter, current, old, target, target2))
        {
          SetBit(generation->candidates, global);
          ++generation->result_count;
        }
      }
    }
    return {};
  };

  auto process_bytes = [&]() -> std::expected<void, std::string> {
    const u64 cancellation_interval = std::max<u64>(1, (1u << 20) / width);
    for (size_t range_index = 0; range_index < generation->ranges->ranges.size(); ++range_index)
    {
      const SnapshotRange& current_range = generation->ranges->ranges[range_index];
      const SnapshotRange* previous_range =
          previous ? &previous->ranges->ranges[range_index] : nullptr;
      for (u64 local = 0; local < current_range.candidate_count; ++local)
      {
        const u64 global = current_range.first_candidate + local;
        if (local % cancellation_interval == 0 && cancelled.load())
          return std::unexpected("cancelled");
        if (previous && !GetBit(previous->candidates, global))
          continue;
        const size_t offset = current_range.candidate_offset + static_cast<size_t>(local * stride);
        const u8* current = current_range.bytes.data() + offset;
        bool matches = false;
        if (filter == MemoryScanFilter::Exact || filter == MemoryScanFilter::NotEqual)
        {
          if (scan.config.data_type == MemoryScanDataType::String && !scan.config.case_sensitive)
          {
            matches =
                std::ranges::equal(std::span(current, width), byte_target, [](u8 lhs, u8 rhs) {
                  const auto fold = [](u8 byte) {
                    return byte >= 'A' && byte <= 'Z' ? static_cast<u8>(byte + ('a' - 'A')) : byte;
                  };
                  return fold(lhs) == fold(rhs);
                });
          }
          else
          {
            matches = std::ranges::equal(std::span(current, width), byte_target);
          }
          if (filter == MemoryScanFilter::NotEqual)
            matches = !matches;
        }
        else
        {
          const u8* old = previous_range->bytes.data() + offset;
          matches = std::ranges::equal(std::span(current, width), std::span(old, width));
          if (filter == MemoryScanFilter::Changed)
            matches = !matches;
        }
        if (matches)
        {
          SetBit(generation->candidates, global);
          ++generation->result_count;
        }
      }
    }
    return {};
  };

  auto process_ppc = [&]() -> std::expected<void, std::string> {
    const u32 target = filter == MemoryScanFilter::Exact ?
                           std::get<u32>(*ParseValue(scan.config.data_type, *value)) :
                           0;
    for (const SnapshotRange& current_range : generation->ranges->ranges)
    {
      const size_t range_index = &current_range - generation->ranges->ranges.data();
      const SnapshotRange* previous_range =
          previous ? &previous->ranges->ranges[range_index] : nullptr;
      for (u64 local = 0; local < current_range.candidate_count; ++local)
      {
        const u64 global = current_range.first_candidate + local;
        if ((global & 0xffff) == 0 && cancelled.load())
          return std::unexpected("cancelled");
        if (previous && !GetBit(previous->candidates, global))
          continue;
        const size_t offset = current_range.candidate_offset + static_cast<size_t>(local * stride);
        const u32 address = current_range.start + static_cast<u32>(offset);
        const u32 current = ReadBigEndian<u32>(current_range.bytes.data() + offset);
        bool matches = false;
        if (filter == MemoryScanFilter::Exact)
          matches = current == target;
        else if (filter == MemoryScanFilter::Mnemonic ||
                 filter == MemoryScanFilter::ValidInstruction)
        {
          const bool supported = PPCTables::IsValidInstruction(UGeckoInstruction{current}, address);
          if (supported)
          {
            const std::string disassembly =
                Common::GekkoDisassembler::Disassemble(current, address);
            const std::string_view mnemonic =
                std::string_view(disassembly).substr(0, disassembly.find_first_of("\t "));
            matches = !mnemonic.empty() && mnemonic != "(ill)" &&
                      (filter == MemoryScanFilter::ValidInstruction || mnemonic == *value);
          }
        }
        else
        {
          const u32 old = ReadBigEndian<u32>(previous_range->bytes.data() + offset);
          matches = current == old;
          if (filter == MemoryScanFilter::Changed)
            matches = !matches;
        }
        if (matches)
        {
          SetBit(generation->candidates, global);
          ++generation->result_count;
        }
      }
    }
    return {};
  };

  std::expected<void, std::string> result;
  switch (scan.config.data_type)
  {
  case MemoryScanDataType::U8:
    result = process.template operator()<u8>();
    break;
  case MemoryScanDataType::U16:
    result = process.template operator()<u16>();
    break;
  case MemoryScanDataType::U32:
    result = process.template operator()<u32>();
    break;
  case MemoryScanDataType::U64:
    result = process.template operator()<u64>();
    break;
  case MemoryScanDataType::S8:
    result = process.template operator()<s8>();
    break;
  case MemoryScanDataType::S16:
    result = process.template operator()<s16>();
    break;
  case MemoryScanDataType::S32:
    result = process.template operator()<s32>();
    break;
  case MemoryScanDataType::S64:
    result = process.template operator()<s64>();
    break;
  case MemoryScanDataType::F32:
    result = process.template operator()<float>();
    break;
  case MemoryScanDataType::F64:
    result = process.template operator()<double>();
    break;
  case MemoryScanDataType::Bytes:
  case MemoryScanDataType::String:
    result = process_bytes();
    break;
  case MemoryScanDataType::PpcInstruction:
    result = process_ppc();
    break;
  }
  if (!result)
    return std::unexpected(result.error());
  BuildResultIndex(generation.get());
  return generation;
}

std::expected<MemoryScanJobAccepted, std::string>
DapMemoryEngine::StartScan(const MemoryScanStartConfig& config)
{
  {
    std::lock_guard lock(m_mutex);
    if (m_job_active)
      return std::unexpected("another memory scan job is active");
  }
  if (!ReapWorker())
    return std::unexpected("memory scans cannot be started from a terminal callback");
  if (config.data_type == MemoryScanDataType::PpcInstruction && !config.aligned)
    return std::unexpected("PPC instruction scans require 4-byte alignment");
  if ((config.data_type == MemoryScanDataType::Bytes ||
       config.data_type == MemoryScanDataType::String) &&
      (config.byte_value.empty() || config.byte_value.size() > 4096))
  {
    return std::unexpected("byte-pattern scan value must contain 1 to 4096 bytes");
  }
  if (config.data_type == MemoryScanDataType::Bytes &&
      (!config.value || Json::Base64Encode(config.byte_value) != *config.value))
  {
    return std::unexpected("byte-pattern scan value is not canonical base64");
  }
  if (config.data_type == MemoryScanDataType::String &&
      (!config.value ||
       std::vector<u8>(config.value->begin(), config.value->end()) != config.byte_value))
  {
    return std::unexpected("string scan value does not match its encoded bytes");
  }
  if (config.data_type == MemoryScanDataType::String &&
      config.string_encoding == MemoryScanStringEncoding::Ascii &&
      std::ranges::any_of(config.byte_value, [](u8 byte) { return byte > 0x7f; }))
  {
    return std::unexpected("ASCII string scan value contains non-ASCII bytes");
  }
  if (config.data_type == MemoryScanDataType::String &&
      config.string_encoding == MemoryScanStringEncoding::Utf8 && !IsValidUtf8(*config.value))
  {
    return std::unexpected("UTF-8 string scan value is malformed");
  }
  const auto ranges = ResolveRanges(config);
  if (!ranges)
    return std::unexpected(ranges.error());
  if (IsPreviousValueFilter(config.filter))
    return std::unexpected("initial scan cannot compare against a previous value");
  if (const std::optional<std::string> error =
          ValidateFilter(config.data_type, config.filter, config.value, config.value2, false))
    return std::unexpected(*error);
  const u64 requested_bytes = CalculateGenerationBytes(config, *ranges);
  if (config.data_type == MemoryScanDataType::Bytes ||
      config.data_type == MemoryScanDataType::String)
  {
    const u64 width = DataTypeSize(config);
    const u64 stride = config.aligned ? width : 1;
    u64 candidate_count = 0;
    for (const ResolvedRange& range : *ranges)
    {
      const u64 offset = config.aligned ? (width - (range.start % width)) % width : 0;
      if (range.size >= offset + width)
        candidate_count += (range.size - offset - width) / stride + 1;
    }
    if (candidate_count > MAX_BYTE_COMPARISON_WORK / width)
      return std::unexpected("byte-pattern scan exceeds the comparison-work limit");
  }
  if (config.data_type == MemoryScanDataType::PpcInstruction &&
      (config.filter == MemoryScanFilter::Mnemonic ||
       config.filter == MemoryScanFilter::ValidInstruction))
  {
    u64 candidate_count = 0;
    for (const ResolvedRange& range : *ranges)
    {
      const u32 offset = (4 - (range.start % 4)) % 4;
      if (range.size >= offset + 4)
        candidate_count += (range.size - offset - 4) / 4 + 1;
    }
    if (candidate_count > MAX_PPC_DISASSEMBLY_WORK)
      return std::unexpected("PPC instruction scan exceeds the decoding-work limit");
  }
  std::optional<MemoryScanBudget::Charge> reservation = m_budget.TryReserve(requested_bytes);
  if (!reservation)
    return std::unexpected("process-wide memory scan budget exceeded");

  std::shared_ptr<Scan> scan;
  int job_id = 0;
  {
    std::lock_guard lock(m_mutex);
    if (m_job_active)
      return std::unexpected("another memory scan job is active");
    if (m_scans.size() >= MAX_SCANS)
      return std::unexpected("memory scan session limit reached");
    scan = std::make_shared<Scan>();
    scan->id = m_next_scan_id++;
    scan->config = config;
    scan->state = "running";
    scan->phase = "waiting";
    scan->job_pause_during_scan = config.pause_during_scan;
    job_id = m_next_job_id++;
    scan->current_job_id = job_id;
    m_scans.emplace(scan->id, scan);
    m_job_active = true;
    m_active_scan_id = scan->id;
    m_cancelled.store(false);
  }
  StartWorker(scan, job_id, config.filter, config.value, config.value2, config.pause_during_scan,
              nullptr, std::move(*reservation));
  return MemoryScanJobAccepted{scan->id, job_id, config.pause_during_scan};
}

std::expected<MemoryScanJobAccepted, std::string>
DapMemoryEngine::RefineScan(const MemoryScanRefineConfig& config)
{
  {
    std::lock_guard lock(m_mutex);
    if (m_job_active)
      return std::unexpected("another memory scan job is active");
  }
  if (!ReapWorker())
    return std::unexpected("memory scans cannot be refined from a terminal callback");

  std::shared_ptr<Scan> scan;
  std::shared_ptr<const Generation> previous;
  std::optional<MemoryScanBudget::Charge> reservation;
  int job_id = 0;
  bool pause_during_scan = false;
  std::optional<std::string> worker_value = config.value;
  {
    std::lock_guard lock(m_mutex);
    if (m_job_active)
      return std::unexpected("another memory scan job is active");
    const auto it = m_scans.find(config.scan_id);
    if (it == m_scans.end())
      return std::unexpected("no such memory scan");
    scan = it->second;
    {
      std::lock_guard scan_lock(scan->mutex);
      previous = scan->generation;
      if (!previous)
        return std::unexpected("memory scan has no completed generation");
      if (const std::optional<std::string> error = ValidateFilter(
              scan->config.data_type, config.filter, config.value, config.value2, true))
        return std::unexpected(*error);
      if (scan->config.data_type == MemoryScanDataType::PpcInstruction &&
          (config.filter == MemoryScanFilter::Mnemonic ||
           config.filter == MemoryScanFilter::ValidInstruction) &&
          previous->result_count > MAX_PPC_DISASSEMBLY_WORK)
      {
        return std::unexpected("PPC instruction scan exceeds the decoding-work limit");
      }
      if (scan->config.data_type == MemoryScanDataType::Bytes &&
          (config.filter == MemoryScanFilter::Exact || config.filter == MemoryScanFilter::NotEqual))
      {
        const size_t expected_encoded_size = ((scan->config.byte_value.size() + 2) / 3) * 4;
        if (!config.value || config.value->size() != expected_encoded_size)
          return std::unexpected("byte-pattern scan value must match the original pattern width");
        const std::optional<std::vector<u8>> decoded = Json::Base64Decode(*config.value);
        if (!decoded || decoded->size() != scan->config.byte_value.size() ||
            Json::Base64Encode(*decoded) != *config.value)
          return std::unexpected("byte-pattern scan value must match the original pattern width");
        worker_value = config.value;
      }
      else if (scan->config.data_type == MemoryScanDataType::String &&
               (config.filter == MemoryScanFilter::Exact ||
                config.filter == MemoryScanFilter::NotEqual))
      {
        if (!config.value || config.value->size() != scan->config.byte_value.size() ||
            (scan->config.string_encoding == MemoryScanStringEncoding::Ascii &&
             std::ranges::any_of(*config.value, [](unsigned char byte) { return byte > 0x7f; })) ||
            (scan->config.string_encoding == MemoryScanStringEncoding::Utf8 &&
             !IsValidUtf8(*config.value)))
        {
          return std::unexpected("string scan value must match the original encoded width");
        }
        worker_value = config.value;
      }
      pause_during_scan = config.pause_during_scan.value_or(scan->config.pause_during_scan);
    }
    const auto ranges = ResolveRanges(scan->config);
    if (!ranges)
      return std::unexpected(ranges.error());
    const u64 requested_bytes = CalculateGenerationBytes(scan->config, *ranges);
    reservation = m_budget.TryReserve(requested_bytes);
    if (!reservation)
      return std::unexpected("process-wide memory scan budget exceeded");
    {
      std::lock_guard scan_lock(scan->mutex);
      scan->state = "running";
      scan->phase = "waiting";
      scan->job_pause_during_scan = pause_during_scan;
      job_id = m_next_job_id++;
      scan->current_job_id = job_id;
    }
    m_job_active = true;
    m_active_scan_id = scan->id;
    m_cancelled.store(false);
  }
  StartWorker(scan, job_id, config.filter, std::move(worker_value), config.value2,
              pause_during_scan, std::move(previous), std::move(*reservation));
  return MemoryScanJobAccepted{scan->id, job_id, pause_during_scan};
}

void DapMemoryEngine::StartWorker(std::shared_ptr<Scan> scan, int job_id, MemoryScanFilter filter,
                                  std::optional<std::string> value,
                                  std::optional<std::string> value2, bool pause_during_scan,
                                  std::shared_ptr<const Generation> previous,
                                  MemoryScanBudget::Charge reservation)
{
  std::lock_guard lock(m_worker_mutex);
  m_worker =
      std::thread([this, scan = std::move(scan), job_id, filter, value = std::move(value),
                   value2 = std::move(value2), pause_during_scan, previous = std::move(previous),
                   reservation = std::move(reservation)]() mutable {
        s_memory_scan_worker = this;
        RunWorker(std::move(scan), job_id, filter, std::move(value), std::move(value2),
                  pause_during_scan, std::move(previous), std::move(reservation));
      });
}

void DapMemoryEngine::RunWorker(std::shared_ptr<Scan> scan, int job_id, MemoryScanFilter filter,
                                std::optional<std::string> value, std::optional<std::string> value2,
                                bool pause_during_scan, std::shared_ptr<const Generation> previous,
                                MemoryScanBudget::Charge reservation)
{
  const auto started = std::chrono::steady_clock::now();
  MemoryScanTerminalEvent terminal;
  terminal.scan_id = scan->id;
  terminal.job_id = job_id;
  terminal.pause_during_scan = pause_during_scan;
  if (previous)
  {
    terminal.generation = previous->number;
    terminal.result_count = previous->result_count;
  }

  const auto resolved = ResolveRanges(scan->config);
  if (!resolved)
  {
    terminal.event = "dolphin_memoryScanFailed";
    terminal.message = resolved.error();
    reservation = {};
    previous.reset();
    FinishWorker(std::move(scan), job_id, std::move(terminal));
    return;
  }

  std::expected<std::shared_ptr<Generation>, std::string> built =
      std::unexpected("snapshot capture failed");
  bool committed = false;
  std::unique_lock core_scan_lock(s_core_scan_mutex, std::defer_lock);
  while (!core_scan_lock.try_lock())
  {
    if (m_cancelled.load())
    {
      terminal.event = "dolphin_memoryScanCancelled";
      reservation = {};
      previous.reset();
      {
        std::lock_guard lock(scan->mutex);
        if (scan->disposed)
        {
          scan->generation.reset();
          scan->undo_generations.clear();
        }
      }
      FinishWorker(std::move(scan), job_id, std::move(terminal));
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  if (pause_during_scan)
  {
    const Core::CPUThreadGuard guard(m_system);
    {
      std::lock_guard lock(scan->mutex);
      scan->phase = "capturing";
      scan->emulation_paused = true;
    }
    auto snapshot = CaptureSnapshot(*resolved, m_cancelled);
    if (snapshot)
    {
      std::lock_guard lock(scan->mutex);
      scan->phase = "filtering";
    }
    if (snapshot)
      built = BuildGeneration(*scan, std::move(*snapshot), previous, filter, value, value2,
                              m_cancelled, std::move(reservation));
    if (built)
    {
      std::lock_guard lock(scan->mutex);
      if (!m_cancelled.load() && !scan->disposed)
      {
        scan->phase = "committing";
        (*built)->number = scan->next_generation_number++;
        if (previous)
        {
          if (scan->undo_generations.size() == MAX_UNDO_GENERATIONS)
            scan->undo_generations.erase(scan->undo_generations.begin());
          scan->undo_generations.push_back(previous);
        }
        scan->generation = *built;
        committed = true;
      }
    }
  }
  else
  {
    std::expected<std::vector<SnapshotRange>, std::string> snapshot;
    {
      const Core::CPUThreadGuard guard(m_system);
      {
        std::lock_guard lock(scan->mutex);
        scan->phase = "capturing";
        scan->emulation_paused = true;
      }
      snapshot = CaptureSnapshot(*resolved, m_cancelled);
    }
    {
      std::lock_guard lock(scan->mutex);
      scan->emulation_paused = false;
      scan->phase = "filtering";
    }
    core_scan_lock.unlock();
    if (snapshot)
      built = BuildGeneration(*scan, std::move(*snapshot), previous, filter, value, value2,
                              m_cancelled, std::move(reservation));
    else
      built = std::unexpected(snapshot.error());
    if (built)
    {
      std::lock_guard lock(scan->mutex);
      if (!m_cancelled.load() && !scan->disposed)
      {
        scan->phase = "committing";
        (*built)->number = scan->next_generation_number++;
        if (previous)
        {
          if (scan->undo_generations.size() == MAX_UNDO_GENERATIONS)
            scan->undo_generations.erase(scan->undo_generations.begin());
          scan->undo_generations.push_back(previous);
        }
        scan->generation = *built;
        committed = true;
      }
    }
  }

  if (core_scan_lock.owns_lock())
    core_scan_lock.unlock();

  bool disposed = false;
  {
    std::lock_guard lock(scan->mutex);
    scan->emulation_paused = false;
    disposed = scan->disposed;
  }
  terminal.duration_ms = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - started)
                                              .count());
  if (!committed && (m_cancelled.load() || disposed || (!built && built.error() == "cancelled")))
  {
    terminal.event = "dolphin_memoryScanCancelled";
  }
  else if (!committed)
  {
    terminal.event = "dolphin_memoryScanFailed";
    terminal.message = built.error();
  }
  else
  {
    terminal.event = "dolphin_memoryScanCompleted";
    terminal.generation = (*built)->number;
    terminal.result_count = (*built)->result_count;
  }
  built = std::unexpected("worker state released");
  reservation = {};
  previous.reset();
  FinishWorker(std::move(scan), job_id, std::move(terminal));
}

void DapMemoryEngine::FinishWorker(std::shared_ptr<Scan> scan, int job_id,
                                   MemoryScanTerminalEvent terminal)
{
  {
    std::lock_guard engine_lock(m_mutex);
    std::lock_guard scan_lock(scan->mutex);
    if (terminal.event == "dolphin_memoryScanCompleted")
    {
      scan->state = "completed";
      scan->phase = "completed";
    }
    else if (terminal.event == "dolphin_memoryScanCancelled")
    {
      scan->state = "cancelled";
      scan->phase = "cancelled";
    }
    else
    {
      scan->state = "failed";
      scan->phase = "failed";
    }
    if (scan->disposed)
    {
      scan->generation.reset();
      scan->undo_generations.clear();
    }
    if (m_active_scan_id == scan->id && scan->current_job_id == job_id)
    {
      m_job_active = false;
      m_active_scan_id = 0;
    }
  }
  TerminalCallback callback = m_terminal_callback;
  scan.reset();
  callback(std::move(terminal));
}

std::optional<MemoryScanStatus> DapMemoryEngine::GetStatus(int scan_id) const
{
  std::shared_ptr<Scan> scan;
  {
    std::lock_guard lock(m_mutex);
    const auto it = m_scans.find(scan_id);
    if (it == m_scans.end())
      return std::nullopt;
    scan = it->second;
  }
  std::lock_guard lock(scan->mutex);
  MemoryScanStatus status;
  status.scan_id = scan->id;
  status.job_id = scan->current_job_id;
  status.state = scan->state;
  status.phase = scan->phase;
  status.pause_during_scan = scan->job_pause_during_scan;
  status.emulation_paused = scan->emulation_paused;
  if (scan->generation)
  {
    status.generation = scan->generation->number;
    status.result_count = scan->generation->result_count;
  }
  return status;
}

std::expected<MemoryScanResultPage, std::string> DapMemoryEngine::GetResults(int scan_id, u64 start,
                                                                             u32 count) const
{
  std::shared_ptr<Scan> scan;
  {
    std::lock_guard lock(m_mutex);
    const auto it = m_scans.find(scan_id);
    if (it == m_scans.end())
      return std::unexpected("no such memory scan");
    scan = it->second;
  }
  std::shared_ptr<const Generation> generation;
  MemoryScanDataType data_type;
  bool aligned;
  {
    std::lock_guard lock(scan->mutex);
    generation = scan->generation;
    data_type = scan->config.data_type;
    aligned = scan->config.aligned;
  }
  if (!generation)
    return std::unexpected("memory scan has no completed generation");

  MemoryScanResultPage page;
  page.scan_id = scan_id;
  page.generation = generation->number;
  page.total_results = generation->result_count;
  page.start = start;
  if (start >= generation->result_count || count == 0)
    return page;
  count = std::min(count, MAX_RESULT_PAGE_SIZE);

  const u32 width = DataTypeSize(scan->config);
  count =
      std::min<u32>(count, static_cast<u32>(std::max<u64>(1, MAX_RESULT_PAGE_RAW_BYTES / width)));
  const u32 stride = aligned ? width : 1;
  const auto block_it =
      std::upper_bound(generation->result_index.begin(), generation->result_index.end(), start);
  const size_t block = static_cast<size_t>(std::prev(block_it) - generation->result_index.begin());
  u64 seen = generation->result_index[block];
  u64 global = static_cast<u64>(block) * RESULT_INDEX_BLOCK_BYTES * 8;
  size_t range_index = 0;
  while (range_index < generation->ranges->ranges.size() &&
         global >= generation->ranges->ranges[range_index].first_candidate +
                       generation->ranges->ranges[range_index].candidate_count)
  {
    ++range_index;
  }
  for (; global < generation->candidates.size() * 8; ++global)
  {
    if (!GetBit(generation->candidates, global))
      continue;
    if (seen++ < start)
      continue;
    while (range_index < generation->ranges->ranges.size() &&
           global >= generation->ranges->ranges[range_index].first_candidate +
                         generation->ranges->ranges[range_index].candidate_count)
    {
      ++range_index;
    }
    if (range_index == generation->ranges->ranges.size())
      break;
    const SnapshotRange& range = generation->ranges->ranges[range_index];
    const u64 local = global - range.first_candidate;
    const size_t offset = range.candidate_offset + static_cast<size_t>(local * stride);
    MemoryScanResult& result = page.results.emplace_back();
    result.address = range.start + static_cast<u32>(offset);
    if (data_type != MemoryScanDataType::Bytes && data_type != MemoryScanDataType::String)
      result.scanned_value = FormatValue(data_type, range.bytes.data() + offset);
    result.raw.assign(range.bytes.begin() + offset, range.bytes.begin() + offset + width);
    if (data_type == MemoryScanDataType::Bytes)
      result.scanned_value = Json::Base64Encode(result.raw);
    else if (data_type == MemoryScanDataType::String)
      result.scanned_value = UTF16ToUTF8(UTF8ToUTF16(
          std::string_view(reinterpret_cast<const char*>(result.raw.data()), result.raw.size())));
    else if (data_type == MemoryScanDataType::PpcInstruction)
    {
      result.disassembly = Common::GekkoDisassembler::Disassemble(
          ReadBigEndian<u32>(result.raw.data()), result.address);
    }
    if (page.results.size() == count)
      return page;
  }
  return page;
}

std::expected<MemoryScanMutationResult, std::string> DapMemoryEngine::Undo(int scan_id)
{
  std::lock_guard lock(m_mutex);
  if (m_job_active)
    return std::unexpected("cannot undo while a memory scan job is active");
  const auto it = m_scans.find(scan_id);
  if (it == m_scans.end())
    return std::unexpected("no such memory scan");

  const std::shared_ptr<Scan>& scan = it->second;
  std::unique_lock scan_lock(scan->mutex);
  if (scan->undo_generations.empty())
    return std::unexpected("memory scan has no generation to undo");
  scan->generation = scan->undo_generations.back();
  scan->undo_generations.pop_back();
  scan->state = "completed";
  scan->phase = "completed";
  return MemoryScanMutationResult{scan->id, scan->generation->number,
                                  scan->generation->result_count, 0,
                                  !scan->undo_generations.empty()};
}

std::expected<MemoryScanMutationResult, std::string>
DapMemoryEngine::RemoveResults(int scan_id, const std::vector<u32>& addresses)
{
  std::lock_guard lock(m_mutex);
  if (m_job_active)
    return std::unexpected("cannot remove results while a memory scan job is active");
  const auto it = m_scans.find(scan_id);
  if (it == m_scans.end())
    return std::unexpected("no such memory scan");

  const std::shared_ptr<Scan>& scan = it->second;
  std::unique_lock scan_lock(scan->mutex);
  if (!scan->generation)
    return std::unexpected("memory scan has no completed generation");

  const u32 width = DataTypeSize(scan->config);
  const u32 stride = scan->config.aligned ? width : 1;
  std::vector<u64> candidates_to_remove;
  candidates_to_remove.reserve(addresses.size());
  for (const u32 address : addresses)
  {
    const auto range_it = std::upper_bound(
        scan->generation->ranges->ranges.begin(), scan->generation->ranges->ranges.end(), address,
        [](u32 value, const SnapshotRange& range) { return value < range.start; });
    if (range_it == scan->generation->ranges->ranges.begin())
      continue;
    const SnapshotRange& range = *std::prev(range_it);
    if (address < range.start)
      continue;
    const u64 relative = static_cast<u64>(address) - range.start;
    if (relative < range.candidate_offset || relative + width > range.bytes.size())
      continue;
    const u64 candidate_delta = relative - range.candidate_offset;
    if (candidate_delta % stride != 0)
      continue;
    const u64 local = candidate_delta / stride;
    if (local >= range.candidate_count)
      continue;
    const u64 global = range.first_candidate + local;
    if (!GetBit(scan->generation->candidates, global))
      continue;
    candidates_to_remove.push_back(global);
  }

  std::ranges::sort(candidates_to_remove);
  const auto unique_end = std::ranges::unique(candidates_to_remove).begin();
  candidates_to_remove.erase(unique_end, candidates_to_remove.end());
  if (candidates_to_remove.empty())
  {
    return MemoryScanMutationResult{scan->id, scan->generation->number,
                                    scan->generation->result_count, 0,
                                    !scan->undo_generations.empty()};
  }

  scan_lock.unlock();
  scan_lock.lock();
  const u64 candidate_bytes = scan->generation->candidates.size();
  const u64 index_bytes =
      ((candidate_bytes + RESULT_INDEX_BLOCK_BYTES - 1) / RESULT_INDEX_BLOCK_BYTES + 1) *
      sizeof(u64);
  const u64 generation_bytes = candidate_bytes + index_bytes;
  std::optional<MemoryScanBudget::Charge> reservation = m_budget.TryReserve(generation_bytes);
  if (!reservation)
    return std::unexpected("process-wide memory scan budget exceeded");

  auto generation = std::make_shared<Generation>();
  generation->ranges = scan->generation->ranges;
  generation->candidates = scan->generation->candidates;
  generation->result_count = scan->generation->result_count;
  generation->budget_charge = std::move(*reservation);
  for (const u64 global : candidates_to_remove)
    ClearBit(generation->candidates, global);
  generation->result_count -= candidates_to_remove.size();
  BuildResultIndex(generation.get());
  const u64 removed_count = candidates_to_remove.size();
  generation->number = scan->next_generation_number++;
  if (scan->undo_generations.size() == MAX_UNDO_GENERATIONS)
    scan->undo_generations.erase(scan->undo_generations.begin());
  scan->undo_generations.push_back(scan->generation);
  scan->generation = std::move(generation);
  scan->state = "completed";
  scan->phase = "completed";
  return MemoryScanMutationResult{scan->id, scan->generation->number,
                                  scan->generation->result_count, removed_count, true};
}

bool DapMemoryEngine::Cancel(int scan_id)
{
  std::lock_guard lock(m_mutex);
  if (!m_job_active || m_active_scan_id != scan_id)
    return false;
  const auto it = m_scans.find(scan_id);
  if (it == m_scans.end())
    return false;
  std::lock_guard scan_lock(it->second->mutex);
  if (it->second->state != "running" || it->second->phase == "committing")
    return false;
  m_cancelled.store(true);
  return true;
}

bool DapMemoryEngine::Dispose(int scan_id)
{
  std::lock_guard lock(m_mutex);
  const auto it = m_scans.find(scan_id);
  if (it == m_scans.end())
    return false;
  {
    std::lock_guard scan_lock(it->second->mutex);
    it->second->disposed = true;
  }
  if (m_job_active && m_active_scan_id == scan_id)
  {
    std::lock_guard scan_lock(it->second->mutex);
    if (it->second->state == "running" && it->second->phase != "committing")
    {
      m_cancelled.store(true);
    }
  }
  m_scans.erase(it);
  return true;
}

bool DapMemoryEngine::HasActiveJob() const
{
  std::lock_guard lock(m_mutex);
  return m_job_active;
}

std::unique_lock<std::mutex> DapMemoryEngine::TryLockCoreScan()
{
  return std::unique_lock(s_core_scan_mutex, std::try_to_lock);
}

u64 DapMemoryEngine::CalculateGenerationBytes(const MemoryScanStartConfig& config,
                                              const std::vector<ResolvedRange>& ranges) const
{
  const u32 width = DataTypeSize(config);
  const u32 stride = config.aligned ? width : 1;
  u64 snapshot_bytes = 0;
  u64 candidate_count = 0;
  for (const ResolvedRange& range : ranges)
  {
    snapshot_bytes += range.size;
    const u32 offset = config.aligned ? (width - (range.start % width)) % width : 0;
    if (range.size >= offset + width)
      candidate_count += (range.size - offset - width) / stride + 1;
  }
  const u64 candidate_bytes = (candidate_count + 7) / 8;
  const u64 index_bytes =
      ((candidate_bytes + RESULT_INDEX_BLOCK_BYTES - 1) / RESULT_INDEX_BLOCK_BYTES + 1) *
      sizeof(u64);
  return snapshot_bytes + candidate_bytes + index_bytes;
}

void DapMemoryEngine::BuildResultIndex(Generation* generation)
{
  const size_t block_count =
      (generation->candidates.size() + RESULT_INDEX_BLOCK_BYTES - 1) / RESULT_INDEX_BLOCK_BYTES;
  generation->result_index.assign(block_count + 1, 0);
  u64 count = 0;
  for (size_t block = 0; block < block_count; ++block)
  {
    generation->result_index[block] = count;
    const size_t begin = block * RESULT_INDEX_BLOCK_BYTES;
    const size_t end = std::min(begin + RESULT_INDEX_BLOCK_BYTES, generation->candidates.size());
    for (size_t i = begin; i < end; ++i)
      count += std::popcount(generation->candidates[i]);
  }
  generation->result_index[block_count] = count;
}

bool DapMemoryEngine::ReapWorker()
{
  if (s_memory_scan_worker == this)
    return false;
  std::lock_guard lock(m_worker_mutex);
  if (m_worker.joinable())
    m_worker.join();
  return true;
}
}  // namespace DAP
