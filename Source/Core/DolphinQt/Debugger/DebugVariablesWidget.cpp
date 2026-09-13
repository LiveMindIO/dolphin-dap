// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Debugger/DebugVariablesWidget.h"

#include <limits>

#include <QCloseEvent>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QTabWidget>
#include <QTreeView>

#include "Core/Core.h"
#include "Core/HW/CPU.h"
#include "Core/System.h"

#include "DolphinQt/Debugger/DebugVariablesModel.h"
#include "DolphinQt/Settings.h"

DebugVariablesWidget::DebugVariablesWidget(Core::System& system, QWidget* parent)
    : QDockWidget(parent), m_system(system),
      m_execution_observer_id(system.GetCPU().GetExecutionState().RegisterClient())
{
  setWindowTitle(tr("Variables"));
  setObjectName(QStringLiteral("variables"));
  setAllowedAreas(Qt::AllDockWidgetAreas);
  setHidden(!Settings::Instance().IsVariablesVisible() ||
            !Settings::Instance().IsDebugModeEnabled());

  m_tabs = new QTabWidget(this);
  m_locals_view = new QTreeView(m_tabs);
  m_globals_view = new QTreeView(m_tabs);
  m_locals_model = new DebugVariablesModel(system, Core::Debug::PPCVariableScope::Locals,
                                           m_execution_observer_id, this);
  m_globals_model = new DebugVariablesModel(system, Core::Debug::PPCVariableScope::Globals,
                                            m_execution_observer_id, this);
  m_locals_view->setModel(m_locals_model);
  m_globals_view->setModel(m_globals_model);
  for (QTreeView* view : {m_locals_view, m_globals_view})
  {
    view->setAlternatingRowColors(true);
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    view->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    view->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    view->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  }
  m_tabs->addTab(m_locals_view, tr("Locals (frame 0)"));
  m_tabs->addTab(m_globals_view, tr("Globals"));
  setWidget(m_tabs);

  const auto show_error = [this](const QString& error) {
    QMessageBox::warning(this, tr("Variable Error"), error);
  };
  connect(m_locals_model, &DebugVariablesModel::Error, this, show_error);
  connect(m_globals_model, &DebugVariablesModel::Error, this, show_error);
  connect(m_locals_view, &QTreeView::customContextMenuRequested, this,
          [this](const QPoint& pos) { ShowContextMenu(m_locals_view, m_locals_model, pos); });
  connect(m_globals_view, &QTreeView::customContextMenuRequested, this,
          [this](const QPoint& pos) { ShowContextMenu(m_globals_view, m_globals_model, pos); });
  connect(&Settings::Instance(), &Settings::VariablesVisibilityChanged, this,
          [this](const bool visible) { setHidden(!visible); });
  connect(&Settings::Instance(), &Settings::DebugModeToggled, this, [this](const bool enabled) {
    setHidden(!enabled || !Settings::Instance().IsVariablesVisible());
  });
  connect(&Settings::Instance(), &Settings::DebugFontChanged, this,
          [this](const QFont& font) { m_tabs->setFont(font); });
  m_tabs->setFont(Settings::Instance().GetDebugFont());

  m_system.GetCPU().GetExecutionState().SetClientEventCallback(
      m_execution_observer_id, [this](std::shared_ptr<const Core::Debug::ExecutionEvent> event) {
        QMetaObject::invokeMethod(
            this, [this, event = std::move(event)] { HandleExecutionEvent(std::move(event)); },
            Qt::QueuedConnection);
      });
  if (Core::GetState(m_system) == Core::State::Paused)
    Refresh();
}

DebugVariablesWidget::~DebugVariablesWidget()
{
  m_system.GetCPU().GetExecutionState().UnregisterClient(m_execution_observer_id);
}

void DebugVariablesWidget::closeEvent(QCloseEvent* event)
{
  Settings::Instance().SetVariablesVisible(false);
  QDockWidget::closeEvent(event);
}

void DebugVariablesWidget::HandleExecutionEvent(
    const std::shared_ptr<const Core::Debug::ExecutionEvent> event)
{
  switch (event->kind)
  {
  case Core::Debug::ExecutionEventKind::Continued:
    Clear();
    break;
  case Core::Debug::ExecutionEventKind::Stopped:
  case Core::Debug::ExecutionEventKind::ValuesChanged:
    Refresh();
    break;
  default:
    break;
  }
}

void DebugVariablesWidget::ShowContextMenu(QTreeView* view, DebugVariablesModel* model,
                                           const QPoint& position)
{
  const QModelIndex index = view->indexAt(position);
  const auto address = model->Address(index);
  if (!address)
    return;
  QMenu menu(this);
  if (model->IsProgramStatic(index))
  {
    const QString name = model->Name(index);
    menu.addAction(tr("Add Watch"), this,
                   [this, name, address] { emit RequestWatch(name, *address); });
  }
  if (const auto byte_size = model->ByteSize(index); byte_size && *byte_size > 0)
  {
    if (!menu.isEmpty())
      menu.addSeparator();
    const u32 end = *address > std::numeric_limits<u32>::max() - (*byte_size - 1) ?
                        std::numeric_limits<u32>::max() :
                        *address + *byte_size - 1;
    menu.addAction(tr("Add Read Breakpoint"), this, [this, address, end] {
      emit RequestMemoryBreakpoint(*address, end, true, false);
    });
    menu.addAction(tr("Add Write Breakpoint"), this, [this, address, end] {
      emit RequestMemoryBreakpoint(*address, end, false, true);
    });
    menu.addAction(tr("Add Access Breakpoint"), this, [this, address, end] {
      emit RequestMemoryBreakpoint(*address, end, true, true);
    });
  }
  if (menu.isEmpty())
    return;
  menu.exec(view->viewport()->mapToGlobal(position));
}

void DebugVariablesWidget::Refresh()
{
  if (Core::GetState(m_system) != Core::State::Paused)
  {
    Clear();
    return;
  }
  m_locals_model->Refresh();
  m_globals_model->Refresh();
}

void DebugVariablesWidget::Clear()
{
  m_locals_model->Clear();
  m_globals_model->Clear();
}
