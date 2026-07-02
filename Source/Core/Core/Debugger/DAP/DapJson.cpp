// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DapJson.h"

#include <array>
#include <string>

#include <fmt/format.h>
#include <mbedtls/base64.h>

#include "Common/StringUtil.h"

namespace DAP::Json
{
std::optional<int> ExtractIntField(const std::string_view json, const std::string_view field)
{
  const std::string needle = fmt::format("\"{}\":", field);
  const size_t pos = json.find(needle);
  if (pos == std::string_view::npos)
    return std::nullopt;

  size_t index = pos + needle.size();
  while (index < json.size() && (json[index] == ' ' || json[index] == '\t'))
    ++index;

  bool negative = false;
  if (index < json.size() && json[index] == '-')
  {
    negative = true;
    ++index;
  }

  int value = 0;
  bool any = false;
  while (index < json.size() && json[index] >= '0' && json[index] <= '9')
  {
    value = value * 10 + (json[index] - '0');
    ++index;
    any = true;
  }

  if (!any)
    return std::nullopt;

  return negative ? -value : value;
}

std::optional<std::string_view> ExtractStringField(const std::string_view json,
                                                     const std::string_view field)
{
  const std::string needle = fmt::format("\"{}\":", field);
  const size_t pos = json.find(needle);
  if (pos == std::string_view::npos)
    return std::nullopt;

  size_t index = pos + needle.size();
  while (index < json.size() && (json[index] == ' ' || json[index] == '\t'))
    ++index;

  if (index >= json.size() || json[index] != '"')
    return std::nullopt;

  ++index;
  const size_t end = json.find('"', index);
  if (end == std::string_view::npos)
    return std::nullopt;

  return json.substr(index, end - index);
}

std::optional<u32> ParseHexAddress(const std::string_view text)
{
  std::string trimmed(text);
  if (trimmed.starts_with("0x") || trimmed.starts_with("0X"))
    trimmed.erase(0, 2);

  u32 value = 0;
  if (!TryParse(trimmed, &value, 16))
    return std::nullopt;

  return value;
}

std::string EscapeString(const std::string_view text)
{
  std::string escaped;
  escaped.reserve(text.size());
  for (const char ch : text)
  {
    switch (ch)
    {
    case '"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += ch;
      break;
    }
  }
  return escaped;
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
}  // namespace DAP::Json
