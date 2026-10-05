// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <optional>

namespace Core
{
class System;
}

namespace DAP
{
class DapTransport;

struct SessionTestHooks
{
  // Bootless fixtures must establish the CPU-thread role on each stepping worker.
  std::function<void()> async_step_worker_started;
  std::function<void()> async_step_worker_joined;
  std::optional<std::chrono::milliseconds> step_out_timeout;
};

void RunSession(DapTransport& transport, Core::System& system,
                const SessionTestHooks* test_hooks = nullptr);
}  // namespace DAP
