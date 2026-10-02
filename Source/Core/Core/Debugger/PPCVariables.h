// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Debugger/DWARF/DwarfReader.h"
#include "Core/Debugger/ExecutionState.h"

namespace Core
{
class System;
}

namespace Core::Debug
{
enum class PPCVariableScope
{
  Locals,
  Globals,
};

enum class PPCVariableStorageKind
{
  None,
  Address,
  Register,
};

struct PPCVariableContext
{
  std::shared_ptr<const Dwarf::ParseResult> debug_info;
  Dwarf::TypeRef type;
  PPCVariableStorageKind storage_kind = PPCVariableStorageKind::None;
  u32 storage = 0;
  u32 depth = 0;
  u64 stop_generation = 0;
  std::optional<u32> byte_size;
  bool program_static = false;
};

struct PPCVariable
{
  std::string name;
  std::string value;
  std::string type;
  std::optional<u32> address;
  std::optional<u32> byte_size;
  bool writable = false;
  bool program_static = false;
  std::optional<PPCVariableContext> write_context;
  std::optional<PPCVariableContext> children;
};

class PPCVariables final
{
public:
  explicit PPCVariables(Core::System& system, std::optional<ExecutionState::ClientId> origin = {});

  bool HasDebugInfo() const;
  std::vector<PPCVariable> GetVariables(PPCVariableScope scope) const;
  std::expected<std::vector<PPCVariable>, std::string>
  GetChildren(const PPCVariableContext& context) const;
  std::expected<PPCVariable, std::string> SetValue(const PPCVariableContext& context,
                                                   std::string_view value) const;
  std::expected<PPCVariable, std::string> SetValue(PPCVariableScope scope, std::string_view name,
                                                   std::string_view value) const;

private:
  Core::System& m_system;
  std::optional<ExecutionState::ClientId> m_origin;
};
}  // namespace Core::Debug
