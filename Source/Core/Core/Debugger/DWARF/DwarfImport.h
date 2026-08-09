// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string>

#include "Common/CommonTypes.h"

class PPCSymbolDB;

namespace Core
{
class CPUThreadGuard;
}  // namespace Core

namespace Core::Debug
{
bool ImportDwarf(const CPUThreadGuard& guard, PPCSymbolDB& symbol_db,
                 std::span<const u8> debug_section, std::span<const u8> line_section,
                 const std::string& object_name = {});

bool ImportDwarfFromElf(const CPUThreadGuard& guard, PPCSymbolDB& symbol_db,
                        const std::string& elf_path);

bool ImportConfiguredDwarfElf(const CPUThreadGuard& guard, PPCSymbolDB& symbol_db);
}  // namespace Core::Debug
