// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDockWidget>

#include "Common/CommonTypes.h"
#include "Core/Debugger/ExecutionState.h"

class DebugVariablesModel;
class QCloseEvent;
class QTabWidget;
class QTreeView;

namespace Core
{
class System;
}

class DebugVariablesWidget final : public QDockWidget
{
  Q_OBJECT

public:
  explicit DebugVariablesWidget(Core::System& system, QWidget* parent = nullptr);
  ~DebugVariablesWidget() override;

signals:
  void RequestWatch(const QString& name, u32 address);
  void RequestMemoryBreakpoint(u32 start, u32 end, bool read, bool write);

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  void HandleExecutionEvent(std::shared_ptr<const Core::Debug::ExecutionEvent> event);
  void ShowContextMenu(QTreeView* view, DebugVariablesModel* model, const QPoint& position);
  void Refresh();
  void Clear();

  Core::System& m_system;
  Core::Debug::ExecutionState::ClientId m_execution_observer_id;
  QTabWidget* m_tabs;
  QTreeView* m_locals_view;
  QTreeView* m_globals_view;
  DebugVariablesModel* m_locals_model;
  DebugVariablesModel* m_globals_model;
};
