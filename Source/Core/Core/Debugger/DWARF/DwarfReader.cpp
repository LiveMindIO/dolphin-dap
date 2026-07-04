// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DWARF/DwarfReader.h"

#include <cstring>
#include <string_view>

#include "Common/Logging/Log.h"
#include "Common/Swap.h"

namespace Core::Debug::Dwarf
{
namespace
{
enum Form : u16
{
  FORM_ADDR = 0x1,
  FORM_REF = 0x2,
  FORM_BLOCK2 = 0x3,
  FORM_BLOCK4 = 0x4,
  FORM_DATA2 = 0x5,
  FORM_DATA4 = 0x6,
  FORM_DATA8 = 0x7,
  FORM_STRING = 0x8,
};

enum Tag : u16
{
  TAG_padding = 0x0000,
  TAG_global_subroutine = 0x0006,
  TAG_compile_unit = 0x0011,
  TAG_subroutine = 0x0014,
  TAG_inlined_subroutine = 0x001d,
  TAG_entry_point = 0x0003,
};

enum Attribute : u16
{
  AT_sibling = 0x0010 | FORM_REF,
  AT_name = 0x0030 | FORM_STRING,
  AT_stmt_list = 0x0100 | FORM_DATA4,
  AT_low_pc = 0x0110 | FORM_ADDR,
  AT_high_pc = 0x0120 | FORM_ADDR,
};

constexpr u16 FormFromAttribute(u16 attr)
{
  return attr & 0xF;
}

class ByteReader
{
public:
  ByteReader(std::span<const u8> data, bool big_endian) : m_data(data), m_big_endian(big_endian) {}

  bool empty() const { return m_data.empty(); }
  size_t size() const { return m_data.size(); }
  const u8* data() const { return m_data.data(); }

  bool ReadU16(u16* out)
  {
    if (m_data.size() < 2)
      return false;
    const u16 value = m_big_endian ? Common::swap16(*reinterpret_cast<const u16*>(m_data.data())) :
                                     *reinterpret_cast<const u16*>(m_data.data());
    m_data = m_data.subspan(2);
    *out = value;
    return true;
  }

  bool ReadU32(u32* out)
  {
    if (m_data.size() < 4)
      return false;
    const u32 value = m_big_endian ? Common::swap32(*reinterpret_cast<const u32*>(m_data.data())) :
                                     *reinterpret_cast<const u32*>(m_data.data());
    m_data = m_data.subspan(4);
    *out = value;
    return true;
  }

  bool ReadU64(u64* out)
  {
    if (m_data.size() < 8)
      return false;
    u64 value = 0;
    if (m_big_endian)
    {
      value |= static_cast<u64>(m_data[0]) << 56;
      value |= static_cast<u64>(m_data[1]) << 48;
      value |= static_cast<u64>(m_data[2]) << 40;
      value |= static_cast<u64>(m_data[3]) << 32;
      value |= static_cast<u64>(m_data[4]) << 24;
      value |= static_cast<u64>(m_data[5]) << 16;
      value |= static_cast<u64>(m_data[6]) << 8;
      value |= static_cast<u64>(m_data[7]);
    }
    else
    {
      value = *reinterpret_cast<const u64*>(m_data.data());
    }
    m_data = m_data.subspan(8);
    *out = value;
    return true;
  }

  bool ReadString(std::string_view* out)
  {
    const u8* start = m_data.data();
    const u8* end = start;
    while (end < start + m_data.size() && *end != 0)
      ++end;
    if (end >= start + m_data.size())
      return false;
    *out = std::string_view(reinterpret_cast<const char*>(start), end - start);
    m_data = m_data.subspan(static_cast<size_t>((end - start) + 1));
    return true;
  }

  bool SkipBytes(size_t count)
  {
    if (m_data.size() < count)
      return false;
    m_data = m_data.subspan(count);
    return true;
  }

  bool SkipBlock2()
  {
    u16 block_len = 0;
    if (!ReadU16(&block_len))
      return false;
    return SkipBytes(block_len);
  }

  bool SkipBlock4()
  {
    u32 block_len = 0;
    if (!ReadU32(&block_len))
      return false;
    return SkipBytes(block_len);
  }

  std::span<const u8> Subspan(size_t offset, size_t count) const
  {
    if (offset + count > m_data.size())
      return {};
    return m_data.subspan(offset, count);
  }

private:
  std::span<const u8> m_data;
  bool m_big_endian;
};

struct DieInfo
{
  u32 length = 0;
  u16 tag = 0;
  u32 sibling = 0;
  u32 low_pc = 0;
  u32 high_pc = 0;
  u32 stmt_list_offset = 0;
  bool has_stmt_list = false;
  std::string name;
};

bool ParseDie(const u8* die_start, const u8* section_end, bool big_endian, DieInfo* info)
{
  if (die_start + 4 > section_end)
    return false;

  ByteReader die_reader({die_start, static_cast<size_t>(section_end - die_start)}, big_endian);
  if (!die_reader.ReadU32(&info->length))
    return false;
  if (info->length <= 4 || die_start + info->length > section_end || info->length < 6)
    return false;

  const u8* die_end = die_start + info->length;
  ByteReader attr_reader({die_start + 4, static_cast<size_t>(die_end - die_start - 4)}, big_endian);
  if (!attr_reader.ReadU16(&info->tag))
    return false;

  while (attr_reader.data() + 2 <= die_end)
  {
    u16 attr = 0;
    if (!attr_reader.ReadU16(&attr))
      return false;

    switch (FormFromAttribute(attr))
    {
    case FORM_DATA2:
      if (!attr_reader.SkipBytes(2))
        return false;
      break;
    case FORM_DATA4:
    case FORM_REF:
    {
      u32 value = 0;
      if (!attr_reader.ReadU32(&value))
        return false;
      if (attr == AT_sibling)
        info->sibling = value;
      else if (attr == AT_stmt_list)
      {
        info->stmt_list_offset = value;
        info->has_stmt_list = true;
      }
      break;
    }
    case FORM_DATA8:
      if (!attr_reader.SkipBytes(8))
        return false;
      break;
    case FORM_ADDR:
    {
      u32 value = 0;
      if (!attr_reader.ReadU32(&value))
        return false;
      if (attr == AT_low_pc)
        info->low_pc = value;
      else if (attr == AT_high_pc)
        info->high_pc = value;
      break;
    }
    case FORM_BLOCK2:
      if (!attr_reader.SkipBlock2())
        return false;
      break;
    case FORM_BLOCK4:
      if (!attr_reader.SkipBlock4())
        return false;
      break;
    case FORM_STRING:
    {
      std::string_view value;
      if (!attr_reader.ReadString(&value))
        return false;
      if (attr == AT_name)
        info->name.assign(value);
      break;
    }
    default:
      return false;
    }
  }

  return true;
}

bool ParseLineTable(std::span<const u8> line_section, u32 stmt_list_offset, u32 compile_unit_base,
                    const std::string& file, bool big_endian, std::vector<LineEntry>* lines)
{
  if (stmt_list_offset + 8 > line_section.size())
    return false;

  const u8* ptr = line_section.data() + stmt_list_offset;
  const u8* section_end = line_section.data() + line_section.size();

  u32 table_length = 0;
  ByteReader reader({ptr, static_cast<size_t>(section_end - ptr)}, big_endian);
  if (!reader.ReadU32(&table_length))
    return false;

  const u8* table_end = ptr + table_length;
  if (table_end > section_end || table_end <= ptr + 4)
    return false;

  u32 base = 0;
  if (!reader.ReadU32(&base))
    return false;

  if (base == 0)
    base = compile_unit_base;

  while (reader.data() + 10 <= table_end)
  {
    u32 line_number = 0;
    u16 column = 0;
    u32 address_delta = 0;
    if (!reader.ReadU32(&line_number))
      break;
    if (!reader.ReadU16(&column))
      break;
    if (!reader.ReadU32(&address_delta))
      break;

    if (line_number == 0)
      break;

    LineEntry entry;
    entry.address = base + address_delta;
    entry.file = file;
    entry.line = line_number;
    lines->push_back(std::move(entry));
  }

  return true;
}

bool IsFunctionTag(u16 tag)
{
  return tag == TAG_global_subroutine || tag == TAG_subroutine || tag == TAG_inlined_subroutine ||
         tag == TAG_entry_point;
}
}  // namespace

std::optional<ParseResult> Parse(std::span<const u8> debug_section,
                                 std::span<const u8> line_section, bool big_endian)
{
  if (debug_section.empty())
    return std::nullopt;

  ParseResult result;
  const u8* section_end = debug_section.data() + debug_section.size();
  const u8* current = debug_section.data();

  while (current < section_end)
  {
    DieInfo unit_info;
    if (!ParseDie(current, section_end, big_endian, &unit_info))
      break;

    if (unit_info.length <= 4 || unit_info.length < 6)
      break;

    if (unit_info.tag != TAG_compile_unit)
    {
      current += unit_info.length;
      continue;
    }

    const std::string compile_unit_name = unit_info.name;
    if (!compile_unit_name.empty())
      result.files.push_back(compile_unit_name);

    if (unit_info.has_stmt_list && !line_section.empty())
    {
      ParseLineTable(line_section, unit_info.stmt_list_offset, unit_info.low_pc, compile_unit_name,
                     big_endian, &result.lines);
    }

    const u8* const unit_end =
        unit_info.sibling != 0 && unit_info.sibling < debug_section.size() ?
            debug_section.data() + unit_info.sibling :
            section_end;

    const u8* child = current + unit_info.length;
    while (child < section_end && child < unit_end)
    {
      DieInfo child_info;
      if (!ParseDie(child, section_end, big_endian, &child_info))
        break;

      if (child_info.length <= 4 || child_info.length < 6)
        break;

      if (IsFunctionTag(child_info.tag) && !child_info.name.empty())
      {
        Function function;
        function.name = child_info.name;
        function.low_pc = child_info.low_pc;
        function.high_pc = child_info.high_pc;
        function.compile_unit = compile_unit_name;
        result.functions.push_back(std::move(function));
      }

      if (child_info.sibling != 0)
      {
        if (child_info.sibling >= debug_section.size())
          break;
        child = debug_section.data() + child_info.sibling;
      }
      else
      {
        child += child_info.length;
      }
    }

    if (unit_info.sibling != 0)
    {
      if (unit_info.sibling >= debug_section.size())
        break;
      current = debug_section.data() + unit_info.sibling;
    }
    else
    {
      current += unit_info.length;
    }
  }

  if (result.functions.empty() && result.lines.empty())
  {
    WARN_LOG_FMT(SYMBOLS, "DWARF 1.1 parse found no functions or line entries");
    return std::nullopt;
  }

  return result;
}
}  // namespace Core::Debug::Dwarf
