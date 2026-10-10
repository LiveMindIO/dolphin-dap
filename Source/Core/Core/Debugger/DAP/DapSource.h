// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <limits>
#include <optional>

#include "Common/CommonTypes.h"

namespace DAP
{
using SourceReference = u64;

// Keep adapter-provided disassembly handles disjoint from one-based DWARF file indices.
// The largest handle is below 2^53, so JSON numbers represent every value exactly.
constexpr SourceReference DISASSEMBLY_SOURCE_REFERENCE_BASE = 1ull << 32;

constexpr SourceReference MakeDisassemblySourceReference(const u32 address)
{
  return DISASSEMBLY_SOURCE_REFERENCE_BASE + address;
}

constexpr std::optional<u32> DecodeDisassemblySourceReference(const SourceReference reference)
{
  if (reference < DISASSEMBLY_SOURCE_REFERENCE_BASE ||
      reference > DISASSEMBLY_SOURCE_REFERENCE_BASE + std::numeric_limits<u32>::max())
  {
    return std::nullopt;
  }
  return static_cast<u32>(reference - DISASSEMBLY_SOURCE_REFERENCE_BASE);
}
}  // namespace DAP
