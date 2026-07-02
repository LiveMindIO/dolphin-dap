// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapJson.h"

#include <string>

#include <fmt/format.h>
#include <mbedtls/base64.h>

#include "Common/StringUtil.h"

namespace DAP::Json
{
std::optional<picojson::object> ParseObject(const std::string_view text)
{
  picojson::value root;
  std::string error;
  picojson::parse(root, text.data(), text.data() + text.size(), &error);
  if (!error.empty() || !root.is<picojson::object>())
    return std::nullopt;

  return root.get<picojson::object>();
}

std::optional<u32> ParseHexAddress(const std::string_view text)
{
  std::string trimmed(text);
  if (trimmed.starts_with("0x") || trimmed.starts_with("0X"))
    trimmed.erase(0, 2);

  if (trimmed.empty())
    return std::nullopt;

  u32 value = 0;
  if (!TryParse(trimmed, &value, 16))
    return std::nullopt;

  return value;
}

std::string FormatAddress(const u32 address)
{
  return fmt::format("0x{:08x}", address);
}

std::string Base64Encode(const std::span<const u8> bytes)
{
  if (bytes.empty())
    return {};

  size_t output_length = 0;
  std::string output((bytes.size() + 2) / 3 * 4 + 1, '\0');
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(output.data()), output.size(),
                            &output_length, bytes.data(), bytes.size()) != 0)
  {
    return {};
  }

  output.resize(output_length);
  return output;
}

std::optional<std::vector<u8>> Base64Decode(const std::string_view text)
{
  if (text.empty())
    return std::vector<u8>{};

  // DESNOTE(jbarber, 2026-07-02): mbedtls reports the required buffer size via
  // MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL when called with a null/zero output, so
  // size the buffer from the base64 length (3 bytes per 4 chars upper bound).
  std::vector<u8> output(text.size() / 4 * 3 + 3);
  size_t output_length = 0;
  const int result =
      mbedtls_base64_decode(output.data(), output.size(), &output_length,
                            reinterpret_cast<const unsigned char*>(text.data()), text.size());
  if (result != 0)
    return std::nullopt;

  output.resize(output_length);
  return output;
}
}  // namespace DAP::Json
