// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
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
  std::string file;
  u32 line = 0;
};

struct ParseResult
{
  std::vector<Function> functions;
  std::vector<LineEntry> lines;
  std::vector<std::string> files;
};

// DESNOTE(jbarber, 2026-07-03): Parses DWARF 1.1 (.debug + .line) as emitted by MWCC /
// CodeWarrior. DWARF 2+ uses different sections and will need a separate front-end.
std::optional<ParseResult> Parse(std::span<const u8> debug_section,
                                 std::span<const u8> line_section, bool big_endian = true);
}  // namespace Core::Debug::Dwarf
