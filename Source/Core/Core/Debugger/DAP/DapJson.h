// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"

namespace DAP::Json
{
std::optional<int> ExtractIntField(std::string_view json, std::string_view field);
std::optional<std::string_view> ExtractStringField(std::string_view json, std::string_view field);
std::optional<u32> ParseHexAddress(std::string_view text);
std::string EscapeString(std::string_view text);
std::string Base64Encode(std::span<const u8> bytes);
}  // namespace DAP::Json
