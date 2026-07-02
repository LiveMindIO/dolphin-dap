// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DAP/DAP.h"

#include "Common/Logging/Log.h"

namespace DAP
{
static bool s_active = false;

void Init(u32 port)
{
  INFO_LOG_FMT(CONSOLE, "DAP server starting on port {} (not yet implemented)", port);
  s_active = true;
}

#ifndef _WIN32
void InitLocal(const char* socket)
{
  INFO_LOG_FMT(CONSOLE, "DAP server starting on socket {} (not yet implemented)", socket);
  s_active = true;
}
#endif

void Deinit()
{
  if (!s_active)
    return;

  INFO_LOG_FMT(CONSOLE, "DAP server stopped.");
  s_active = false;
}

bool IsActive()
{
  return s_active;
}
}  // namespace DAP
