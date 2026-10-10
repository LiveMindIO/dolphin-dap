// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core::Debug::Dwarf
{
struct Function
{
  std::string name;
  u32 low_pc = 0;
  u32 high_pc = 0;
  std::string compile_unit;
};

struct LineEntry
{
  u32 address = 0;
  u32 file_index = 0;
  std::string file;
  u32 line = 0;
};

enum class TypeModifier : u8
{
  Pointer = 1,
  Reference = 2,
  Const = 3,
  Volatile = 4,
};

struct FundamentalTypeRef
{
  u16 type = 0;
};

struct UserTypeRef
{
  u32 die_offset = 0;
};

struct TypeRef
{
  std::variant<std::monostate, FundamentalTypeRef, UserTypeRef> type;
  std::vector<TypeModifier> modifiers;
};

enum class LocationKind
{
  Unavailable,
  Address,
  Register,
  BaseRegisterOffset,
  MemberOffset,
};

struct Location
{
  LocationKind kind = LocationKind::Unavailable;
  u32 value = 0;
  s32 offset = 0;
};

struct Member
{
  std::string name;
  TypeRef type;
  Location location;
  std::optional<u32> bit_size;
  std::optional<u32> bit_offset;
};

struct Enumerator
{
  std::string name;
  s64 value = 0;
};

enum class TypeKind
{
  Structure,
  Union,
  Typedef,
  Pointer,
  Array,
  Enumeration,
};

struct Type
{
  u32 die_offset = 0;
  TypeKind kind = TypeKind::Structure;
  std::string name;
  u32 byte_size = 0;
  TypeRef referenced_type;
  std::vector<Member> members;
  std::optional<u32> array_count;
  std::vector<Enumerator> enumerators;
  bool enumeration_is_unsigned = false;
};

enum class VariableKind
{
  Parameter,
  Local,
  Global,
};

struct Variable
{
  std::string name;
  TypeRef type;
  Location location;
  VariableKind kind = VariableKind::Local;
  u32 low_pc = 0;
  u32 high_pc = 0;
};

struct ParseResult
{
  std::vector<Function> functions;
  std::vector<LineEntry> lines;
  std::vector<std::string> files;
  std::vector<Type> types;
  std::vector<Variable> variables;
};

// DESNOTE(jbarber, 2026-07-03): Parses DWARF 1.1 (.debug + .line) as emitted by MWCC /
// CodeWarrior. DWARF 2+ uses different sections and will need a separate front-end.
std::optional<ParseResult> Parse(std::span<const u8> debug_section,
                                 std::span<const u8> line_section, bool big_endian = true);
}  // namespace Core::Debug::Dwarf
