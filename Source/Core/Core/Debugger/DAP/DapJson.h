// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

namespace DAP::Json
{
// Parse a DAP message body into a JSON object. Returns nullopt when the payload
// is not valid JSON or is not a top-level object.
std::optional<picojson::object> ParseObject(std::string_view text);

// DAP memory references and addresses are transported as hex strings such as
// "0x80003100". Returns nullopt for empty or non-hex input.
std::optional<u32> ParseHexAddress(std::string_view text);

// Format an address as the canonical DAP memory reference string.
std::string FormatAddress(u32 address);

std::string Base64Encode(std::span<const u8> bytes);
std::optional<std::vector<u8>> Base64Decode(std::string_view text);
}  // namespace DAP::Json
