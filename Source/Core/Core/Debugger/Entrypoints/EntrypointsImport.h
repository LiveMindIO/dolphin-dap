// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

class PPCSymbolDB;

namespace Core
{
class CPUThreadGuard;
}  // namespace Core

namespace Core::Debug
{
bool ImportEntrypointsFromJson(const CPUThreadGuard& guard, PPCSymbolDB& symbol_db,
                               const std::string& json_path);

bool ImportConfiguredEntrypoints(const CPUThreadGuard& guard, PPCSymbolDB& symbol_db);
}  // namespace Core::Debug
