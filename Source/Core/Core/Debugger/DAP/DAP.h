// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace DAP
{
void Init(u32 port);
void InitLocal(const char* socket);
void Deinit();
bool IsActive();
}  // namespace DAP
