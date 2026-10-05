// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DWARF/DwarfReader.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
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
  TAG_array_type = 0x0001,
  TAG_formal_parameter = 0x0005,
  TAG_global_subroutine = 0x0006,
  TAG_global_variable = 0x0007,
  TAG_lexical_block = 0x000b,
  TAG_local_variable = 0x000c,
  TAG_member = 0x000d,
  TAG_pointer_type = 0x000f,
  TAG_compile_unit = 0x0011,
  TAG_structure_type = 0x0013,
  TAG_subroutine = 0x0014,
  TAG_typedef = 0x0016,
  TAG_union_type = 0x0017,
  TAG_inlined_subroutine = 0x001d,
  TAG_entry_point = 0x0003,
  TAG_enumeration_type = 0x0004,
  TAG_subrange_type = 0x0021,
};

enum Attribute : u16
{
  AT_sibling = 0x0010 | FORM_REF,
  AT_location = 0x0020 | FORM_BLOCK2,
  AT_name = 0x0030 | FORM_STRING,
  AT_fund_type = 0x0050 | FORM_DATA2,
  AT_mod_fund_type = 0x0060 | FORM_BLOCK2,
  AT_user_def_type = 0x0070 | FORM_REF,
  AT_mod_u_d_type = 0x0080 | FORM_BLOCK2,
  AT_subscr_data = 0x00a0 | FORM_BLOCK2,
  AT_byte_size = 0x00b0 | FORM_DATA4,
  AT_bit_offset = 0x00c0 | FORM_DATA2,
  AT_bit_size = 0x00d0 | FORM_DATA4,
  AT_element_list = 0x00f0 | FORM_BLOCK4,
  AT_stmt_list = 0x0100 | FORM_DATA4,
  AT_low_pc = 0x0110 | FORM_ADDR,
  AT_high_pc = 0x0120 | FORM_ADDR,
  AT_start_scope = 0x02c0 | FORM_DATA4,
};

enum LocationAtom : u8
{
  OP_REG = 0x01,
  OP_BASEREG = 0x02,
  OP_ADDR = 0x03,
  OP_CONST = 0x04,
  OP_ADD = 0x07,
};

constexpr size_t MAX_DIES = 100000;
constexpr u32 MAX_ARRAY_ELEMENTS = 4096;

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

  bool ReadU8(u8* out)
  {
    if (m_data.empty())
      return false;
    *out = m_data.front();
    m_data = m_data.subspan(1);
    return true;
  }

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

  bool ReadS32(s32* out)
  {
    u32 value = 0;
    if (!ReadU32(&value))
      return false;
    *out = static_cast<s32>(value);
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
    const auto terminator = std::ranges::find(m_data, 0);
    if (terminator == m_data.end())
      return false;
    const u8* end = std::to_address(terminator);
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
    if (offset > m_data.size() || count > m_data.size() - offset)
      return {};
    return m_data.subspan(offset, count);
  }

  bool ReadBlock2(std::span<const u8>* out)
  {
    u16 length = 0;
    if (!ReadU16(&length) || m_data.size() < length)
      return false;
    *out = m_data.first(length);
    m_data = m_data.subspan(length);
    return true;
  }

  bool ReadBlock4(std::span<const u8>* out)
  {
    u32 length = 0;
    if (!ReadU32(&length) || m_data.size() < length)
      return false;
    *out = m_data.first(length);
    m_data = m_data.subspan(length);
    return true;
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
  u32 byte_size = 0;
  u32 start_scope = 0;
  std::optional<u32> bit_size;
  std::optional<u32> bit_offset;
  bool has_stmt_list = false;
  std::string name;
  TypeRef type;
  std::span<const u8> location;
  std::span<const u8> subscr_data;
  std::span<const u8> element_list;
};

bool ParseModifiedType(std::span<const u8> data, bool big_endian, bool user_type, TypeRef* out)
{
  if (data.size() < (user_type ? 4u : 2u))
    return false;
  const size_t base_size = user_type ? 4 : 2;
  ByteReader reader(data, big_endian);
  while (reader.size() > base_size)
  {
    u8 modifier = 0;
    if (!reader.ReadU8(&modifier) || modifier < 1 || modifier > 4)
      return false;
    out->modifiers.push_back(static_cast<TypeModifier>(modifier));
  }
  if (user_type)
  {
    u32 offset = 0;
    if (!reader.ReadU32(&offset))
      return false;
    out->type = UserTypeRef{offset};
  }
  else
  {
    u16 fundamental = 0;
    if (!reader.ReadU16(&fundamental))
      return false;
    out->type = FundamentalTypeRef{fundamental};
  }
  return true;
}

Location ParseLocation(std::span<const u8> data, bool big_endian, bool member)
{
  ByteReader reader(data, big_endian);
  u8 atom = 0;
  if (!reader.ReadU8(&atom))
    return {};
  if (member)
  {
    u32 offset = 0;
    u8 add = 0;
    if (atom == OP_CONST && reader.ReadU32(&offset) && reader.ReadU8(&add) && add == OP_ADD &&
        reader.empty())
      return {LocationKind::MemberOffset, offset, 0};
    return {};
  }
  if (atom == OP_ADDR)
  {
    u32 address = 0;
    if (reader.ReadU32(&address) && reader.empty())
      return {LocationKind::Address, address, 0};
  }
  else if (atom == OP_REG)
  {
    u32 reg = 0;
    if (reader.ReadU32(&reg) && reader.empty())
      return {LocationKind::Register, reg, 0};
  }
  else if (atom == OP_BASEREG)
  {
    u32 reg = 0;
    u8 constant = 0;
    s32 offset = 0;
    u8 add = 0;
    if (reader.ReadU32(&reg) && reader.ReadU8(&constant) && constant == OP_CONST &&
        reader.ReadS32(&offset) && reader.ReadU8(&add) && add == OP_ADD && reader.empty())
      return {LocationKind::BaseRegisterOffset, reg, offset};
  }
  return {};
}

bool ParseDie(const u8* die_start, const u8* section_end, bool big_endian, DieInfo* info)
{
  if (die_start > section_end || static_cast<size_t>(section_end - die_start) < 4)
    return false;

  const size_t remaining = static_cast<size_t>(section_end - die_start);
  ByteReader die_reader({die_start, remaining}, big_endian);
  if (!die_reader.ReadU32(&info->length))
    return false;
  if (info->length == 4)
  {
    info->tag = TAG_padding;
    return true;
  }
  if (info->length < 6 || info->length > remaining)
    return false;

  const u8* die_end = die_start + info->length;
  ByteReader attr_reader({die_start + 4, static_cast<size_t>(die_end - die_start - 4)}, big_endian);
  if (!attr_reader.ReadU16(&info->tag))
    return false;

  while (attr_reader.size() >= 2)
  {
    u16 attr = 0;
    if (!attr_reader.ReadU16(&attr))
      return false;

    switch (FormFromAttribute(attr))
    {
    case FORM_DATA2:
    {
      u16 value = 0;
      if (!attr_reader.ReadU16(&value))
        return false;
      if (attr == AT_fund_type)
        info->type.type = FundamentalTypeRef{value};
      else if (attr == AT_bit_offset)
        info->bit_offset = value;
      break;
    }
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
      else if (attr == AT_user_def_type)
        info->type.type = UserTypeRef{value};
      else if (attr == AT_byte_size)
        info->byte_size = value;
      else if (attr == AT_start_scope)
        info->start_scope = value;
      else if (attr == AT_bit_size)
        info->bit_size = value;
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
    {
      std::span<const u8> value;
      if (!attr_reader.ReadBlock2(&value))
        return false;
      if (attr == AT_location)
        info->location = value;
      else if (attr == AT_subscr_data)
        info->subscr_data = value;
      else if (attr == AT_mod_fund_type &&
               !ParseModifiedType(value, big_endian, false, &info->type))
        info->type = {};
      else if (attr == AT_mod_u_d_type && !ParseModifiedType(value, big_endian, true, &info->type))
        info->type = {};
      break;
    }
    case FORM_BLOCK4:
      if (attr == AT_element_list)
      {
        if (!attr_reader.ReadBlock4(&info->element_list))
          return false;
      }
      else if (!attr_reader.SkipBlock4())
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
      // DESNOTE(jbarber, 2026-07-21): We don't know this attribute's encoded
      // size, so we can't safely advance past it to read subsequent attributes.
      // Bail out of the attribute loop for this DIE rather than the entire CU
      // walk: ParseDie still succeeds (the length/tag/already-read fields are
      // valid), and the caller advances via `info->length` / `info->sibling`
      // to the next DIE. Previously `return false` here propagated up through
      // ParseCompileUnits as a `break`, aborting parsing of subsequent CUs and
      // losing their functions/line info.
      WARN_LOG_FMT(SYMBOLS,
                   "DWARF 1.1: unknown attribute form 0x{:04x} in DIE tag 0x{:04x}; "
                   "skipping remaining attributes",
                   attr, info->tag);
      return true;
    }
  }

  return true;
}

bool ParseLineTable(std::span<const u8> line_section, u32 stmt_list_offset, u32 compile_unit_base,
                    u32 file_index, const std::string& file, bool big_endian,
                    std::vector<LineEntry>* lines)
{
  if (stmt_list_offset > line_section.size() || line_section.size() - stmt_list_offset < 8)
    return false;

  const u8* ptr = line_section.data() + stmt_list_offset;
  const u8* section_end = line_section.data() + line_section.size();

  u32 table_length = 0;
  ByteReader reader({ptr, static_cast<size_t>(section_end - ptr)}, big_endian);
  if (!reader.ReadU32(&table_length))
    return false;

  if (table_length > static_cast<size_t>(section_end - ptr) || table_length <= 4)
    return false;
  const u8* table_end = ptr + table_length;

  u32 base = 0;
  if (!reader.ReadU32(&base))
    return false;

  if (base == 0)
    base = compile_unit_base;

  while (reader.data() <= table_end && static_cast<size_t>(table_end - reader.data()) >= 10)
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
    if (address_delta > std::numeric_limits<u32>::max() - base)
      continue;
    entry.address = base + address_delta;
    entry.file_index = file_index;
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

bool IsTypeTag(u16 tag)
{
  return tag == TAG_structure_type || tag == TAG_union_type || tag == TAG_typedef ||
         tag == TAG_pointer_type || tag == TAG_array_type || tag == TAG_enumeration_type;
}

std::vector<Enumerator> ParseElementList(std::span<const u8> data, const bool big_endian)
{
  ByteReader reader(data, big_endian);
  std::vector<Enumerator> result;
  while (!reader.empty())
  {
    s32 value = 0;
    std::string_view name;
    if (!reader.ReadS32(&value) || !reader.ReadString(&name))
      return {};
    result.push_back({std::string(name), value});
  }
  return result;
}

bool CanHaveChildren(u16 tag)
{
  return tag == TAG_compile_unit || IsFunctionTag(tag) || tag == TAG_lexical_block ||
         tag == TAG_structure_type || tag == TAG_union_type || tag == TAG_array_type;
}

std::optional<std::pair<TypeRef, u32>> ParseArrayType(std::span<const u8> data, bool big_endian)
{
  ByteReader reader(data, big_endian);
  u8 format = 0;
  u16 index_type = 0;
  u32 lower = 0;
  u32 upper = 0;
  u8 element_format = 0;
  if (!reader.ReadU8(&format) || format != 0 || !reader.ReadU16(&index_type) ||
      !reader.ReadU32(&lower) || !reader.ReadU32(&upper) || !reader.ReadU8(&element_format) ||
      element_format != 8 || upper < lower || upper - lower >= MAX_ARRAY_ELEMENTS)
    return std::nullopt;

  u16 attr = 0;
  if (!reader.ReadU16(&attr))
    return std::nullopt;
  TypeRef type;
  if (attr == AT_fund_type)
  {
    u16 fundamental = 0;
    if (!reader.ReadU16(&fundamental))
      return std::nullopt;
    type.type = FundamentalTypeRef{fundamental};
  }
  else if (attr == AT_user_def_type)
  {
    u32 offset = 0;
    if (!reader.ReadU32(&offset))
      return std::nullopt;
    type.type = UserTypeRef{offset};
  }
  else if (attr == AT_mod_fund_type || attr == AT_mod_u_d_type)
  {
    std::span<const u8> block;
    if (!reader.ReadBlock2(&block) ||
        !ParseModifiedType(block, big_endian, attr == AT_mod_u_d_type, &type))
      return std::nullopt;
  }
  else
  {
    return std::nullopt;
  }
  if (!reader.empty())
    return std::nullopt;
  return std::pair{std::move(type), upper - lower + 1};
}
}  // namespace

std::optional<ParseResult> Parse(std::span<const u8> debug_section,
                                 std::span<const u8> line_section, bool big_endian)
{
  if (debug_section.empty() || debug_section.size() > std::numeric_limits<u32>::max())
    return std::nullopt;

  ParseResult result;
  const u8* section_end = debug_section.data() + debug_section.size();
  const u8* current = debug_section.data();
  size_t unit_count = 0;

  while (current < section_end && ++unit_count <= MAX_DIES)
  {
    DieInfo unit_info;
    if (!ParseDie(current, section_end, big_endian, &unit_info))
      break;

    if (unit_info.tag == TAG_padding)
    {
      current += unit_info.length;
      continue;
    }
    if (unit_info.length < 6)
      break;

    if (unit_info.tag != TAG_compile_unit)
    {
      current += unit_info.length;
      continue;
    }

    const std::string compile_unit_name = unit_info.name;
    const u32 file_index = static_cast<u32>(result.files.size());
    result.files.push_back(compile_unit_name);

    if (unit_info.has_stmt_list && !line_section.empty())
    {
      ParseLineTable(line_section, unit_info.stmt_list_offset, unit_info.low_pc, file_index,
                     compile_unit_name, big_endian, &result.lines);
    }

    const u32 unit_offset = static_cast<u32>(current - debug_section.data());
    const u8* const unit_end =
        unit_info.sibling > unit_offset && unit_info.sibling <= debug_section.size() ?
            debug_section.data() + unit_info.sibling :
            section_end;

    struct Parent
    {
      u16 tag = 0;
      const u8* end = nullptr;
      u32 low_pc = 0;
      u32 high_pc = 0;
      std::optional<size_t> type_index;
    };
    std::vector<Parent> parents{
        {TAG_compile_unit, unit_end, unit_info.low_pc, unit_info.high_pc, std::nullopt}};

    const u8* child = current + unit_info.length;
    size_t die_count = 0;
    while (child < section_end && child < unit_end && ++die_count <= MAX_DIES)
    {
      while (!parents.empty() && child >= parents.back().end)
        parents.pop_back();
      if (parents.empty())
        break;

      DieInfo child_info;
      if (!ParseDie(child, unit_end, big_endian, &child_info))
        break;
      if (child_info.tag == TAG_padding)
      {
        child += child_info.length;
        continue;
      }
      if (child_info.length < 6)
        break;

      const u32 die_offset = static_cast<u32>(child - debug_section.data());

      if (IsFunctionTag(child_info.tag) && !child_info.name.empty())
      {
        Function function;
        function.name = child_info.name;
        function.low_pc = child_info.low_pc;
        function.high_pc = child_info.high_pc;
        function.compile_unit = compile_unit_name;
        result.functions.push_back(std::move(function));
      }

      std::optional<size_t> type_index;
      if (IsTypeTag(child_info.tag))
      {
        Type type;
        type.die_offset = die_offset;
        type.name = child_info.name;
        type.byte_size = child_info.byte_size;
        type.referenced_type = child_info.type;
        if (child_info.tag == TAG_structure_type)
          type.kind = TypeKind::Structure;
        else if (child_info.tag == TAG_union_type)
          type.kind = TypeKind::Union;
        else if (child_info.tag == TAG_typedef)
          type.kind = TypeKind::Typedef;
        else if (child_info.tag == TAG_pointer_type)
          type.kind = TypeKind::Pointer;
        else if (child_info.tag == TAG_enumeration_type)
        {
          type.kind = TypeKind::Enumeration;
          type.enumerators = ParseElementList(child_info.element_list, big_endian);
          if (type.byte_size != 0 && type.byte_size < sizeof(s64))
          {
            const s64 signed_max = (s64{1} << (type.byte_size * 8 - 1)) - 1;
            type.enumeration_is_unsigned =
                std::ranges::any_of(type.enumerators, [signed_max](const Enumerator& enumerator) {
                  return enumerator.value > signed_max;
                });
          }
        }
        else
        {
          type.kind = TypeKind::Array;
          if (auto array = ParseArrayType(child_info.subscr_data, big_endian))
          {
            type.referenced_type = std::move(array->first);
            type.array_count = array->second;
          }
        }
        result.types.push_back(std::move(type));
        type_index = result.types.size() - 1;
      }

      if (child_info.tag == TAG_member)
      {
        for (auto parent = parents.rbegin(); parent != parents.rend(); ++parent)
        {
          if (!parent->type_index)
            continue;
          Type& type = result.types[*parent->type_index];
          if (type.kind == TypeKind::Structure || type.kind == TypeKind::Union)
          {
            Location location = ParseLocation(child_info.location, big_endian, true);
            if (type.kind == TypeKind::Union && location.kind == LocationKind::Unavailable &&
                child_info.location.empty())
            {
              location = {LocationKind::MemberOffset, 0, 0};
            }
            type.members.push_back({child_info.name, child_info.type, std::move(location),
                                    child_info.bit_size, child_info.bit_offset});
          }
          break;
        }
      }

      if (child_info.tag == TAG_formal_parameter || child_info.tag == TAG_local_variable ||
          child_info.tag == TAG_global_variable)
      {
        Variable variable;
        variable.name = child_info.name;
        variable.type = child_info.type;
        variable.location = ParseLocation(child_info.location, big_endian, false);
        variable.kind = child_info.tag == TAG_formal_parameter ? VariableKind::Parameter :
                        child_info.tag == TAG_global_variable  ? VariableKind::Global :
                                                                 VariableKind::Local;
        if (variable.kind != VariableKind::Global)
        {
          u32 function_base = 0;
          for (auto parent = parents.rbegin(); parent != parents.rend(); ++parent)
          {
            if (IsFunctionTag(parent->tag))
            {
              variable.low_pc = parent->low_pc;
              variable.high_pc = parent->high_pc;
              function_base = parent->low_pc;
              break;
            }
          }
          for (const Parent& parent : parents)
          {
            if (parent.tag != TAG_lexical_block || parent.low_pc >= parent.high_pc)
              continue;
            variable.low_pc = std::max(variable.low_pc, parent.low_pc);
            variable.high_pc = std::min(variable.high_pc, parent.high_pc);
          }
          if (child_info.start_scope != 0 &&
              child_info.start_scope <= std::numeric_limits<u32>::max() - function_base)
          {
            variable.low_pc = std::max(variable.low_pc, function_base + child_info.start_scope);
          }
        }
        result.variables.push_back(std::move(variable));
      }

      const u8* const die_end = child + child_info.length;
      const u8* sibling = nullptr;
      if (child_info.sibling > die_offset && child_info.sibling <= debug_section.size())
        sibling = debug_section.data() + child_info.sibling;

      const u8* const child_scope_end = sibling ? sibling : parents.back().end;
      if (CanHaveChildren(child_info.tag) && child_scope_end <= parents.back().end &&
          die_end < child_scope_end)
      {
        u32 low_pc = child_info.low_pc;
        u32 high_pc = child_info.high_pc;
        if (child_info.tag == TAG_lexical_block && low_pc >= high_pc)
        {
          low_pc = parents.back().low_pc;
          high_pc = parents.back().high_pc;
        }
        parents.push_back({child_info.tag, child_scope_end, low_pc, high_pc, type_index});
        child = die_end;
      }
      else if (sibling && sibling <= parents.back().end)
      {
        child = sibling;
      }
      else
      {
        child = die_end;
      }
    }

    if (unit_info.sibling > unit_offset && unit_info.sibling < debug_section.size())
    {
      current = debug_section.data() + unit_info.sibling;
    }
    else
    {
      current += unit_info.length;
    }
  }

  if (result.functions.empty() && result.lines.empty() && result.types.empty() &&
      result.variables.empty())
  {
    WARN_LOG_FMT(SYMBOLS, "DWARF 1.1 parse found no functions or line entries");
    return std::nullopt;
  }

  return result;
}
}  // namespace Core::Debug::Dwarf
