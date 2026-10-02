// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/PPCVariables.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

#include <fmt/format.h>

#include "Common/StringUtil.h"
#include "Core/Core.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace Core::Debug
{
namespace
{
constexpr u32 MAX_VALUE_DEPTH = 32;
constexpr size_t MAX_CHILDREN = 1000;
constexpr u32 MAX_STRING_PREVIEW = 256;

bool IsStopped(Core::System& system, const ExecutionState& execution)
{
  const Core::State state = Core::GetState(system);
  // Unit tests initialize the CPU directly without booting the global Core state machine.
  return execution.IsStopped() &&
         (state == Core::State::Paused ||
          (state == Core::State::Uninitialized && system.GetCPU().IsStepping()));
}

const Dwarf::Type* FindType(const Dwarf::ParseResult& info, const u32 offset)
{
  const auto it = std::ranges::lower_bound(info.types, offset, {}, &Dwarf::Type::die_offset);
  return it == info.types.end() || it->die_offset != offset ? nullptr : &*it;
}

std::optional<u32> FundamentalSize(const u16 type)
{
  switch (type)
  {
  case 1:
  case 2:
  case 3:
  case 21:
    return 1;
  case 4:
  case 5:
  case 6:
    return 2;
  case 7:
  case 8:
  case 9:
  case 10:
  case 11:
  case 12:
  case 13:
  case 14:
    return 4;
  case 15:
  case 0x8008:
  case 0x8108:
  case 0x8208:
    return 8;
  default:
    return std::nullopt;
  }
}

std::string FundamentalName(const u16 type)
{
  switch (type)
  {
  case 1:
    return "char";
  case 2:
    return "signed char";
  case 3:
    return "unsigned char";
  case 4:
  case 5:
    return "short";
  case 6:
    return "unsigned short";
  case 7:
  case 8:
    return "int";
  case 9:
    return "unsigned int";
  case 10:
  case 11:
    return "long";
  case 12:
    return "unsigned long";
  case 13:
    return "void*";
  case 14:
    return "float";
  case 15:
    return "double";
  case 20:
    return "void";
  case 21:
    return "bool";
  case 0x8008:
  case 0x8108:
    return "long long";
  case 0x8208:
    return "unsigned long long";
  default:
    return "unknown";
  }
}

std::string TypeName(const Dwarf::ParseResult& info, const Dwarf::TypeRef& ref, u32 depth = 0)
{
  if (depth >= MAX_VALUE_DEPTH)
    return "unknown";
  if (!ref.modifiers.empty())
  {
    auto modified = ref;
    const auto modifier = modified.modifiers.front();
    modified.modifiers.erase(modified.modifiers.begin());
    if (modifier == Dwarf::TypeModifier::Pointer)
      return TypeName(info, modified, depth + 1) + "*";
    if (modifier == Dwarf::TypeModifier::Reference)
      return TypeName(info, modified, depth + 1) + "&";
    return TypeName(info, modified, depth + 1);
  }
  if (const auto* fundamental = std::get_if<Dwarf::FundamentalTypeRef>(&ref.type))
    return FundamentalName(fundamental->type);
  const auto* user = std::get_if<Dwarf::UserTypeRef>(&ref.type);
  const auto* type = user ? FindType(info, user->die_offset) : nullptr;
  if (!type)
    return "unknown";
  if (!type->name.empty())
    return type->name;
  if (type->kind == Dwarf::TypeKind::Pointer)
    return TypeName(info, type->referenced_type, depth + 1) + "*";
  if (type->kind == Dwarf::TypeKind::Array)
    return fmt::format("{}[{}]", TypeName(info, type->referenced_type, depth + 1),
                       type->array_count.value_or(0));
  if (type->kind == Dwarf::TypeKind::Enumeration)
    return "enum";
  return type->kind == Dwarf::TypeKind::Union ? "union" : "struct";
}

std::optional<u64> ReadRegisterValue(const PowerPC::PowerPCState& state, const u32 reg)
{
  if (reg < 32)
    return state.gpr[reg];
  if (reg == 65)
    return LR(state);
  if (reg == 66)
    return CTR(state);
  if (reg == 76)
    return state.GetXER().Hex;
  return std::nullopt;
}

bool WriteRegisterValue(PowerPC::PowerPCState& state, const u32 reg, const u64 value)
{
  if (value > std::numeric_limits<u32>::max())
    return false;
  if (reg < 32)
    state.gpr[reg] = static_cast<u32>(value);
  else if (reg == 65)
    LR(state) = static_cast<u32>(value);
  else if (reg == 66)
    CTR(state) = static_cast<u32>(value);
  else if (reg == 76)
  {
    UReg_XER xer;
    xer.Hex = static_cast<u32>(value);
    state.SetXER(xer);
  }
  else
    return false;
  return true;
}

std::optional<u64> ReadBigEndianValue(const Core::CPUThreadGuard& guard, const u32 address,
                                      const u32 size)
{
  if (size == 0 || size > 8 || address > std::numeric_limits<u32>::max() - (size - 1))
    return std::nullopt;
  auto* accessors = AddressSpace::GetAccessors(AddressSpace::Type::Effective);
  if (!accessors || !accessors->IsValidAddress(guard, address) ||
      !accessors->IsValidAddress(guard, address + size - 1))
    return std::nullopt;
  u64 value = 0;
  for (u32 i = 0; i < size; ++i)
    value = value << 8 | accessors->ReadU8(guard, address + i);
  return value;
}

bool WriteBigEndianValue(const Core::CPUThreadGuard& guard, const u32 address, const u32 size,
                         const u64 value)
{
  if (size == 0 || size > 8 || address > std::numeric_limits<u32>::max() - (size - 1) ||
      (size < 8 && value >= (u64{1} << (size * 8))))
  {
    return false;
  }
  auto* accessors = AddressSpace::GetAccessors(AddressSpace::Type::Effective);
  if (!accessors)
    return false;
  for (u32 i = 0; i < size; ++i)
  {
    if (!accessors->IsValidAddress(guard, address + i))
      return false;
  }
  for (u32 i = 0; i < size; ++i)
    accessors->WriteU8(guard, address + i, static_cast<u8>(value >> ((size - i - 1) * 8)));
  return true;
}

PPCVariableContext MakeContext(std::shared_ptr<const Dwarf::ParseResult> info,
                               const Dwarf::TypeRef& type, const PPCVariableStorageKind kind,
                               const u32 storage, const u32 depth, const u64 stop_generation,
                               const std::optional<u32> byte_size, const bool program_static)
{
  return {std::move(info), type, kind, storage, depth, stop_generation, byte_size, program_static};
}

PPCVariable UnavailableVariable(std::string name, std::string type,
                                const std::optional<u32> address = {},
                                const std::optional<u32> byte_size = {},
                                const bool program_static = false)
{
  return {std::move(name), "<unavailable>", std::move(type), address,     byte_size,
          false,           program_static,  std::nullopt,    std::nullopt};
}

bool IsPlainCharType(const Dwarf::ParseResult& info, Dwarf::TypeRef ref, const u32 depth = 0)
{
  if (depth >= MAX_VALUE_DEPTH)
    return false;
  while (!ref.modifiers.empty() && (ref.modifiers.front() == Dwarf::TypeModifier::Const ||
                                    ref.modifiers.front() == Dwarf::TypeModifier::Volatile))
  {
    ref.modifiers.erase(ref.modifiers.begin());
  }
  if (!ref.modifiers.empty())
    return false;
  if (const auto* fundamental = std::get_if<Dwarf::FundamentalTypeRef>(&ref.type))
    return fundamental->type == 1;
  const auto* user = std::get_if<Dwarf::UserTypeRef>(&ref.type);
  const auto* type = user ? FindType(info, user->die_offset) : nullptr;
  return type && type->kind == Dwarf::TypeKind::Typedef &&
         IsPlainCharType(info, type->referenced_type, depth + 1);
}

std::optional<std::string> FormatCharArray(const Core::CPUThreadGuard& guard,
                                           const Dwarf::ParseResult& info, const Dwarf::Type& type,
                                           const u32 address)
{
  if (!type.array_count || !IsPlainCharType(info, type.referenced_type))
    return std::nullopt;
  const auto* accessors = AddressSpace::GetAccessors(AddressSpace::Type::Effective);
  if (!accessors)
    return std::nullopt;
  const u32 count = std::min(*type.array_count, MAX_STRING_PREVIEW);
  std::string value{"\""};
  bool terminated = false;
  for (u32 i = 0; i < count; ++i)
  {
    if (address > std::numeric_limits<u32>::max() - i ||
        !accessors->IsValidAddress(guard, address + i))
      return std::nullopt;
    const u8 byte = accessors->ReadU8(guard, address + i);
    if (byte == 0)
    {
      terminated = true;
      break;
    }
    switch (byte)
    {
    case '\\':
      value += "\\\\";
      break;
    case '"':
      value += "\\\"";
      break;
    case '\n':
      value += "\\n";
      break;
    case '\r':
      value += "\\r";
      break;
    case '\t':
      value += "\\t";
      break;
    default:
      value += byte >= 0x20 && byte <= 0x7e ? std::string(1, static_cast<char>(byte)) :
                                              fmt::format("\\x{:02x}", byte);
      break;
    }
  }
  value += '"';
  if (!terminated && *type.array_count > count)
    value += "...";
  return value;
}

std::optional<u32> TypeSize(const Dwarf::ParseResult& info, const Dwarf::TypeRef& ref,
                            u32 depth = 0)
{
  if (depth >= MAX_VALUE_DEPTH)
    return std::nullopt;
  if (!ref.modifiers.empty() && (ref.modifiers.front() == Dwarf::TypeModifier::Const ||
                                 ref.modifiers.front() == Dwarf::TypeModifier::Volatile))
  {
    auto unqualified = ref;
    unqualified.modifiers.erase(unqualified.modifiers.begin());
    return TypeSize(info, unqualified, depth + 1);
  }
  if (!ref.modifiers.empty() && ref.modifiers.front() == Dwarf::TypeModifier::Pointer)
    return 4;
  if (!ref.modifiers.empty())
    return std::nullopt;
  if (const auto* fundamental = std::get_if<Dwarf::FundamentalTypeRef>(&ref.type))
    return FundamentalSize(fundamental->type);
  const auto* user = std::get_if<Dwarf::UserTypeRef>(&ref.type);
  const auto* type = user ? FindType(info, user->die_offset) : nullptr;
  if (!type)
    return std::nullopt;
  if (type->kind == Dwarf::TypeKind::Pointer)
    return 4;
  if (type->byte_size != 0)
    return type->byte_size;
  if (type->kind == Dwarf::TypeKind::Typedef)
    return TypeSize(info, type->referenced_type, depth + 1);
  if (type->kind == Dwarf::TypeKind::Enumeration)
    return type->byte_size == 0 ? std::optional<u32>(4) : std::optional<u32>(type->byte_size);
  if (type->kind == Dwarf::TypeKind::Array && type->array_count)
  {
    const auto element = TypeSize(info, type->referenced_type, depth + 1);
    if (element && *element != 0 &&
        *type->array_count <= std::numeric_limits<u32>::max() / *element)
      return *element * *type->array_count;
  }
  return std::nullopt;
}

PPCVariable Materialize(const Core::CPUThreadGuard& guard,
                        std::shared_ptr<const Dwarf::ParseResult> info, std::string name,
                        Dwarf::TypeRef ref, const PPCVariableStorageKind storage_kind,
                        const u32 storage, std::optional<u64> direct_value, const u32 depth,
                        const u64 stop_generation, const bool program_static)
{
  const std::string display_type = TypeName(*info, ref);
  const auto address =
      storage_kind == PPCVariableStorageKind::Address ? std::optional<u32>(storage) : std::nullopt;
  if (depth >= MAX_VALUE_DEPTH)
    return UnavailableVariable(std::move(name), display_type, address, TypeSize(*info, ref),
                               program_static);

  bool is_const = false;
  while (!ref.modifiers.empty() && (ref.modifiers.front() == Dwarf::TypeModifier::Const ||
                                    ref.modifiers.front() == Dwarf::TypeModifier::Volatile))
  {
    is_const |= ref.modifiers.front() == Dwarf::TypeModifier::Const;
    ref.modifiers.erase(ref.modifiers.begin());
  }
  if (!ref.modifiers.empty() && ref.modifiers.front() == Dwarf::TypeModifier::Reference)
    return UnavailableVariable(std::move(name), display_type, address, TypeSize(*info, ref),
                               program_static);

  const auto write_context =
      [&](const Dwarf::TypeRef& write_type) -> std::optional<PPCVariableContext> {
    if (is_const || storage_kind == PPCVariableStorageKind::None)
      return std::nullopt;
    const auto byte_size = TypeSize(*info, write_type);
    if (storage_kind == PPCVariableStorageKind::Register && (!byte_size || *byte_size > 4))
      return std::nullopt;
    return MakeContext(info, write_type, storage_kind, storage, depth, stop_generation, byte_size,
                       program_static);
  };
  if (!ref.modifiers.empty() && ref.modifiers.front() == Dwarf::TypeModifier::Pointer)
  {
    const std::optional<u64> value = direct_value ? direct_value :
                                     address      ? ReadBigEndianValue(guard, *address, 4) :
                                                    std::nullopt;
    if (!value || *value > std::numeric_limits<u32>::max())
      return UnavailableVariable(std::move(name), display_type, address, 4, program_static);
    const auto writable = write_context(ref);
    ref.modifiers.erase(ref.modifiers.begin());
    std::optional<PPCVariableContext> children;
    if (*value != 0)
      children = MakeContext(info, ref, PPCVariableStorageKind::Address, static_cast<u32>(*value),
                             depth + 1, stop_generation, TypeSize(*info, ref), false);
    return {std::move(name),
            fmt::format("0x{:08x}", static_cast<u32>(*value)),
            display_type,
            address,
            4,
            writable.has_value(),
            program_static,
            writable,
            std::move(children)};
  }

  if (const auto* fundamental = std::get_if<Dwarf::FundamentalTypeRef>(&ref.type))
  {
    const auto size = FundamentalSize(fundamental->type);
    std::optional<u64> value = direct_value    ? direct_value :
                               address && size ? ReadBigEndianValue(guard, *address, *size) :
                                                 std::nullopt;
    if (!size || !value)
      return UnavailableVariable(std::move(name), display_type, address, size, program_static);
    if (*size < 8)
      *value &= (u64{1} << (*size * 8)) - 1;
    const auto writable = write_context(ref);
    return {std::move(name),
            fmt::format("0x{:0{}x}", *value, *size * 2),
            display_type,
            address,
            size,
            writable.has_value(),
            program_static,
            writable,
            std::nullopt};
  }

  const auto* user = std::get_if<Dwarf::UserTypeRef>(&ref.type);
  const auto* type = user ? FindType(*info, user->die_offset) : nullptr;
  if (!type)
    return UnavailableVariable(std::move(name), display_type, address, TypeSize(*info, ref),
                               program_static);
  if (type->kind == Dwarf::TypeKind::Typedef)
    return Materialize(guard, info, std::move(name), type->referenced_type, storage_kind, storage,
                       direct_value, depth + 1, stop_generation, program_static);
  if (type->kind == Dwarf::TypeKind::Pointer)
  {
    const std::optional<u64> value = direct_value ? direct_value :
                                     address      ? ReadBigEndianValue(guard, *address, 4) :
                                                    std::nullopt;
    if (!value || *value > std::numeric_limits<u32>::max())
      return UnavailableVariable(std::move(name), display_type, address, 4, program_static);
    std::optional<PPCVariableContext> children;
    if (*value != 0)
      children = MakeContext(info, type->referenced_type, PPCVariableStorageKind::Address,
                             static_cast<u32>(*value), depth + 1, stop_generation,
                             TypeSize(*info, type->referenced_type), false);
    const auto writable = write_context(ref);
    return {std::move(name),
            fmt::format("0x{:08x}", static_cast<u32>(*value)),
            display_type,
            address,
            4,
            writable.has_value(),
            program_static,
            writable,
            std::move(children)};
  }
  if (type->kind == Dwarf::TypeKind::Enumeration)
  {
    const u32 size = type->byte_size == 0 ? 4 : type->byte_size;
    const std::optional<u64> value = direct_value ? direct_value :
                                     address      ? ReadBigEndianValue(guard, *address, size) :
                                                    std::nullopt;
    if (!value || size == 0 || size > 8)
      return UnavailableVariable(std::move(name), display_type, address, size, program_static);
    const u64 masked = size == 8 ? *value : *value & ((u64{1} << (size * 8)) - 1);
    std::string display = fmt::format("0x{:0{}x}", masked, size * 2);
    const s64 signed_value = size == 8 ?
                                 static_cast<s64>(masked) :
                                 static_cast<s64>(masked << (64 - size * 8)) >> (64 - size * 8);
    const auto enumerator = std::ranges::find_if(type->enumerators, [&](const auto& entry) {
      return type->enumeration_is_unsigned ? static_cast<u64>(entry.value) == masked :
                                             entry.value == signed_value;
    });
    if (enumerator != type->enumerators.end())
      display = fmt::format("{} ({})", enumerator->name, display);
    const auto writable = write_context(ref);
    return {std::move(name),      std::move(display), display_type, address,     size,
            writable.has_value(), program_static,     writable,     std::nullopt};
  }
  if (!address || (type->kind == Dwarf::TypeKind::Array && !type->array_count))
    return UnavailableVariable(std::move(name), display_type, address, TypeSize(*info, ref),
                               program_static);
  const std::optional<std::string> char_array = type->kind == Dwarf::TypeKind::Array ?
                                                    FormatCharArray(guard, *info, *type, *address) :
                                                    std::nullopt;
  return {std::move(name),
          char_array.value_or(fmt::format("@ 0x{:08x}", *address)),
          display_type,
          address,
          TypeSize(*info, ref),
          false,
          program_static,
          std::nullopt,
          MakeContext(info, ref, PPCVariableStorageKind::Address, *address, depth + 1,
                      stop_generation, TypeSize(*info, ref), program_static)};
}

enum class ScalarKind
{
  Signed,
  Unsigned,
  Boolean,
  Character,
  Pointer,
  Float,
  Double,
  Enumeration,
};

struct WritableScalar
{
  ScalarKind kind;
  u32 size;
  const Dwarf::Type* enumeration = nullptr;
};

std::optional<WritableScalar> WritableType(const Dwarf::ParseResult& info, Dwarf::TypeRef ref,
                                           u32 depth = 0)
{
  if (depth >= MAX_VALUE_DEPTH)
    return std::nullopt;
  while (!ref.modifiers.empty() && ref.modifiers.front() == Dwarf::TypeModifier::Volatile)
    ref.modifiers.erase(ref.modifiers.begin());
  if (!ref.modifiers.empty() && ref.modifiers.front() == Dwarf::TypeModifier::Const)
    return std::nullopt;
  if (!ref.modifiers.empty() && ref.modifiers.front() == Dwarf::TypeModifier::Pointer)
    return WritableScalar{ScalarKind::Pointer, 4};
  if (!ref.modifiers.empty())
    return std::nullopt;
  if (const auto* fundamental = std::get_if<Dwarf::FundamentalTypeRef>(&ref.type))
  {
    const auto size = FundamentalSize(fundamental->type);
    if (!size)
      return std::nullopt;
    if (fundamental->type == 1)
      return WritableScalar{ScalarKind::Character, *size};
    if (fundamental->type == 21)
      return WritableScalar{ScalarKind::Boolean, *size};
    if (fundamental->type == 13)
      return WritableScalar{ScalarKind::Pointer, *size};
    if (fundamental->type == 14)
      return WritableScalar{ScalarKind::Float, *size};
    if (fundamental->type == 15)
      return WritableScalar{ScalarKind::Double, *size};
    const bool is_unsigned = fundamental->type == 3 || fundamental->type == 6 ||
                             fundamental->type == 9 || fundamental->type == 12 ||
                             fundamental->type == 0x8208;
    return WritableScalar{is_unsigned ? ScalarKind::Unsigned : ScalarKind::Signed, *size};
  }
  const auto* user = std::get_if<Dwarf::UserTypeRef>(&ref.type);
  const auto* type = user ? FindType(info, user->die_offset) : nullptr;
  if (!type)
    return std::nullopt;
  if (type->kind == Dwarf::TypeKind::Pointer)
    return WritableScalar{ScalarKind::Pointer, 4};
  if (type->kind == Dwarf::TypeKind::Typedef)
    return WritableType(info, type->referenced_type, depth + 1);
  if (type->kind == Dwarf::TypeKind::Enumeration && type->byte_size <= 8)
  {
    return WritableScalar{ScalarKind::Enumeration, type->byte_size == 0 ? 4 : type->byte_size,
                          type};
  }
  return std::nullopt;
}

std::optional<u64> ParseScalarValue(const WritableScalar scalar, const std::string_view text)
{
  if (scalar.kind == ScalarKind::Enumeration && scalar.enumeration)
  {
    const auto enumerator =
        std::ranges::find(scalar.enumeration->enumerators, text, &Dwarf::Enumerator::name);
    if (enumerator != scalar.enumeration->enumerators.end())
    {
      const u32 bits = scalar.size * 8;
      const u64 raw = static_cast<u64>(enumerator->value);
      return bits == 64 ? raw : raw & ((u64{1} << bits) - 1);
    }
  }
  if (scalar.kind == ScalarKind::Boolean)
  {
    if (text == "true" || text == "1")
      return 1;
    if (text == "false" || text == "0")
      return 0;
    return std::nullopt;
  }
  if (scalar.kind == ScalarKind::Character && text.size() >= 3 && text.front() == '\'' &&
      text.back() == '\'')
  {
    const std::string_view contents = text.substr(1, text.size() - 2);
    if (contents.size() == 1)
      return static_cast<u8>(contents.front());
    if (contents.size() == 2 && contents.front() == '\\')
    {
      switch (contents.back())
      {
      case '0':
        return 0;
      case 'n':
        return '\n';
      case 'r':
        return '\r';
      case 't':
        return '\t';
      case '\\':
        return '\\';
      case '\'':
        return '\'';
      default:
        return std::nullopt;
      }
    }
    return std::nullopt;
  }
  if (scalar.kind == ScalarKind::Float || scalar.kind == ScalarKind::Double)
  {
    double parsed = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), parsed, std::chars_format::general);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(parsed))
      return std::nullopt;
    if (scalar.kind == ScalarKind::Float)
    {
      const float narrowed = static_cast<float>(parsed);
      if (!std::isfinite(narrowed))
        return std::nullopt;
      return std::bit_cast<u32>(narrowed);
    }
    return std::bit_cast<u64>(parsed);
  }
  if (scalar.kind == ScalarKind::Signed)
  {
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
      u64 raw = 0;
      if (!TryParse(std::string(text), &raw, 0) ||
          (scalar.size < 8 && raw >= (u64{1} << (scalar.size * 8))))
      {
        return std::nullopt;
      }
      return raw;
    }
    s64 parsed = 0;
    if (!TryParse(std::string(text), &parsed, 0))
      return std::nullopt;
    const u32 bits = scalar.size * 8;
    if (bits < 64)
    {
      const s64 minimum = -(s64{1} << (bits - 1));
      const s64 maximum = (s64{1} << (bits - 1)) - 1;
      if (parsed < minimum || parsed > maximum)
        return std::nullopt;
      return static_cast<u64>(parsed) & ((u64{1} << bits) - 1);
    }
    return static_cast<u64>(parsed);
  }

  if (scalar.kind == ScalarKind::Enumeration && scalar.enumeration &&
      !scalar.enumeration->enumeration_is_unsigned)
  {
    WritableScalar signed_scalar{ScalarKind::Signed, scalar.size};
    return ParseScalarValue(signed_scalar, text);
  }

  u64 parsed = 0;
  if (!TryParse(std::string(text), &parsed, 0) ||
      (scalar.size < 8 && parsed >= (u64{1} << (scalar.size * 8))))
    return std::nullopt;
  return parsed;
}
}  // namespace

PPCVariables::PPCVariables(Core::System& system,
                           const std::optional<ExecutionState::ClientId> origin)
    : m_system(system), m_origin(origin)
{
}

bool PPCVariables::HasDebugInfo() const
{
  const auto info = m_system.GetPPCSymbolDB().GetDwarfDebugInfo();
  return info && !info->variables.empty();
}

std::vector<PPCVariable> PPCVariables::GetVariables(const PPCVariableScope scope) const
{
  Core::CPUThreadGuard guard(m_system);
  const auto info = m_system.GetPPCSymbolDB().GetDwarfDebugInfo();
  if (!info)
    return {};
  const auto& state = m_system.GetPPCState();
  const auto& execution = m_system.GetCPU().GetExecutionState();
  if (!IsStopped(m_system, execution))
    return {};
  const u64 stop_generation = execution.GetStopGeneration();
  const bool globals = scope == PPCVariableScope::Globals;
  std::vector<PPCVariable> result;
  for (const auto& variable : info->variables)
  {
    const bool is_global = variable.kind == Dwarf::VariableKind::Global;
    if (is_global != globals ||
        (!globals && (variable.low_pc >= variable.high_pc || state.pc < variable.low_pc ||
                      state.pc >= variable.high_pc)))
      continue;
    PPCVariableStorageKind storage_kind = PPCVariableStorageKind::None;
    u32 storage = 0;
    std::optional<u64> direct;
    if (variable.location.kind == Dwarf::LocationKind::Address)
    {
      storage_kind = PPCVariableStorageKind::Address;
      storage = variable.location.value;
    }
    else if (variable.location.kind == Dwarf::LocationKind::Register)
    {
      storage_kind = PPCVariableStorageKind::Register;
      storage = variable.location.value;
      direct = ReadRegisterValue(state, storage);
    }
    else if (variable.location.kind == Dwarf::LocationKind::BaseRegisterOffset)
    {
      if (const auto base = ReadRegisterValue(state, variable.location.value);
          base && *base <= std::numeric_limits<u32>::max())
      {
        const s64 resolved = static_cast<s64>(*base) + variable.location.offset;
        if (resolved >= 0 && resolved <= std::numeric_limits<u32>::max())
        {
          storage_kind = PPCVariableStorageKind::Address;
          storage = static_cast<u32>(resolved);
        }
      }
    }
    result.push_back(Materialize(guard, info, variable.name, variable.type, storage_kind, storage,
                                 direct, 0, stop_generation, is_global));
  }
  return result;
}

std::expected<std::vector<PPCVariable>, std::string>
PPCVariables::GetChildren(const PPCVariableContext& context) const
{
  Core::CPUThreadGuard guard(m_system);
  const auto& execution = m_system.GetCPU().GetExecutionState();
  if (!IsStopped(m_system, execution) || context.stop_generation != execution.GetStopGeneration())
    return std::unexpected("stale variable context");
  const auto& info = context.debug_info;
  if (!info || info != m_system.GetPPCSymbolDB().GetDwarfDebugInfo())
    return std::unexpected("stale variable context");
  if (context.depth >= MAX_VALUE_DEPTH || context.storage_kind != PPCVariableStorageKind::Address)
    return std::vector<PPCVariable>{};
  if (!context.type.modifiers.empty() ||
      std::holds_alternative<Dwarf::FundamentalTypeRef>(context.type.type))
  {
    return std::vector<PPCVariable>{Materialize(
        guard, info, "*", context.type, PPCVariableStorageKind::Address, context.storage,
        std::nullopt, context.depth, context.stop_generation, context.program_static)};
  }
  const auto* user = std::get_if<Dwarf::UserTypeRef>(&context.type.type);
  const auto* type = user ? FindType(*info, user->die_offset) : nullptr;
  if (!type)
    return std::vector<PPCVariable>{};
  if (type->kind == Dwarf::TypeKind::Typedef)
  {
    return std::vector<PPCVariable>{Materialize(guard, info, "value", type->referenced_type,
                                                PPCVariableStorageKind::Address, context.storage,
                                                std::nullopt, context.depth,
                                                context.stop_generation, context.program_static)};
  }

  std::vector<PPCVariable> result;
  if (type->kind == Dwarf::TypeKind::Structure || type->kind == Dwarf::TypeKind::Union)
  {
    for (const auto& member : type->members)
    {
      if (result.size() >= MAX_CHILDREN)
        break;
      const auto member_size = TypeSize(*info, member.type);
      if (member.bit_size || member.bit_offset)
      {
        std::optional<u32> member_address;
        if (member.location.kind == Dwarf::LocationKind::MemberOffset &&
            member.location.value <= std::numeric_limits<u32>::max() - context.storage)
        {
          member_address = context.storage + member.location.value;
        }
        result.push_back(UnavailableVariable(member.name, TypeName(*info, member.type),
                                             member_address, member_size, context.program_static));
        continue;
      }
      const bool outside_parent =
          context.byte_size && (!member_size || member.location.value > *context.byte_size ||
                                *member_size > *context.byte_size - member.location.value);
      if (member.location.kind != Dwarf::LocationKind::MemberOffset || outside_parent ||
          member.location.value > std::numeric_limits<u32>::max() - context.storage)
      {
        result.push_back(UnavailableVariable(member.name, TypeName(*info, member.type)));
        continue;
      }
      result.push_back(Materialize(guard, info, member.name, member.type,
                                   PPCVariableStorageKind::Address,
                                   context.storage + member.location.value, std::nullopt,
                                   context.depth, context.stop_generation, context.program_static));
    }
  }
  else if (type->kind == Dwarf::TypeKind::Array && type->array_count)
  {
    const auto element_size = TypeSize(*info, type->referenced_type);
    if (!element_size || *element_size == 0)
      return result;
    const u32 count = std::min<u32>(*type->array_count, MAX_CHILDREN);
    for (u32 i = 0; i < count; ++i)
    {
      const u64 offset = static_cast<u64>(i) * *element_size;
      if ((context.byte_size &&
           (offset > *context.byte_size || *element_size > *context.byte_size - offset)) ||
          offset > std::numeric_limits<u32>::max() - context.storage)
      {
        break;
      }
      const u32 address = context.storage + static_cast<u32>(offset);
      result.push_back(Materialize(guard, info, fmt::format("[{}]", i), type->referenced_type,
                                   PPCVariableStorageKind::Address, address, std::nullopt,
                                   context.depth, context.stop_generation, context.program_static));
    }
  }
  return result;
}

std::expected<PPCVariable, std::string>
PPCVariables::SetValue(const PPCVariableContext& context, const std::string_view value_text) const
{
  u32 size = 0;
  u64 value = 0;
  {
    Core::CPUThreadGuard guard(m_system);
    const auto& execution = m_system.GetCPU().GetExecutionState();
    if (!IsStopped(m_system, execution) || context.stop_generation != execution.GetStopGeneration())
      return std::unexpected("stale variable context");
    const auto& info = context.debug_info;
    if (!info)
      return std::unexpected("debug information unavailable");
    if (info != m_system.GetPPCSymbolDB().GetDwarfDebugInfo())
      return std::unexpected("stale variable context");
    const auto scalar = WritableType(*info, context.type);
    if (!scalar)
      return std::unexpected("variable is not writable");
    const auto parsed = ParseScalarValue(*scalar, value_text);
    if (!parsed)
      return std::unexpected("invalid variable value");
    value = *parsed;
    size = scalar->size;
    if (scalar->kind == ScalarKind::Pointer && value != 0)
    {
      const auto* accessors = AddressSpace::GetAccessors(AddressSpace::Type::Effective);
      auto pointee = context.type;
      if (!pointee.modifiers.empty() && pointee.modifiers.front() == Dwarf::TypeModifier::Pointer)
      {
        pointee.modifiers.erase(pointee.modifiers.begin());
      }
      else if (const auto* user = std::get_if<Dwarf::UserTypeRef>(&pointee.type))
      {
        if (const Dwarf::Type* type = FindType(*info, user->die_offset);
            type && type->kind == Dwarf::TypeKind::Pointer)
        {
          pointee = type->referenced_type;
        }
      }
      const u32 target_size = TypeSize(*info, pointee).value_or(1);
      if (value > std::numeric_limits<u32>::max() || !accessors || target_size == 0 ||
          value > std::numeric_limits<u32>::max() - (target_size - 1) ||
          !accessors->IsValidAddress(guard, static_cast<u32>(value)) ||
          !accessors->IsValidAddress(guard, static_cast<u32>(value) + target_size - 1))
      {
        return std::unexpected("invalid pointer target");
      }
    }
    bool written = false;
    if (context.storage_kind == PPCVariableStorageKind::Address)
      written = WriteBigEndianValue(guard, context.storage, size, value);
    else if (context.storage_kind == PPCVariableStorageKind::Register && size <= 4)
      written = WriteRegisterValue(m_system.GetPPCState(), context.storage, value);
    if (!written)
      return std::unexpected("variable write failed");
  }

  auto& execution = m_system.GetCPU().GetExecutionState();
  execution.PublishValuesChanged(m_origin,
                                 context.storage_kind == PPCVariableStorageKind::Address ?
                                     std::optional<u32>(context.storage) :
                                     std::nullopt,
                                 context.storage_kind == PPCVariableStorageKind::Address ?
                                     std::optional<u32>(size) :
                                     std::nullopt);

  Core::CPUThreadGuard guard(m_system);
  if (!IsStopped(m_system, execution) || context.stop_generation != execution.GetStopGeneration() ||
      context.debug_info != m_system.GetPPCSymbolDB().GetDwarfDebugInfo())
  {
    return std::unexpected("stale variable context");
  }
  const auto direct = context.storage_kind == PPCVariableStorageKind::Register ?
                          ReadRegisterValue(m_system.GetPPCState(), context.storage) :
                          std::nullopt;
  return Materialize(guard, context.debug_info, "", context.type, context.storage_kind,
                     context.storage, direct, context.depth, context.stop_generation,
                     context.program_static);
}

std::expected<PPCVariable, std::string> PPCVariables::SetValue(const PPCVariableScope scope,
                                                               const std::string_view name,
                                                               const std::string_view value) const
{
  const std::vector<PPCVariable> variables = GetVariables(scope);
  const auto it = std::ranges::find(variables, name, &PPCVariable::name);
  if (it == variables.end() || !it->write_context)
    return std::unexpected("variable is not writable");
  auto result = SetValue(*it->write_context, value);
  if (result)
    result->name = it->name;
  return result;
}
}  // namespace Core::Debug
