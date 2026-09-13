// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Debugger/CodeWidget.h"

#include <chrono>
#include <optional>

#include <fmt/format.h>

#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QStyleHints>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include "Core/Core.h"
#include "Core/Debugger/ExecutionState.h"
#include "Core/Debugger/PPCStack.h"
#include "Core/Debugger/PPCStepping.h"
#include "Core/HW/CPU.h"
#include "Core/Host.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DolphinQt/Debugger/BranchWatchDialog.h"
#include "DolphinQt/Debugger/SourceViewWidget.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

static const QString BOX_SPLITTER_STYLESHEET = QStringLiteral(
    "QSplitter::handle { border-top: 1px dashed black; width: 1px; margin-left: 10px; "
    "margin-right: 10px; }");

CodeWidget::CodeWidget(QWidget* parent)
    : QDockWidget(parent), m_system(Core::System::GetInstance()),
      m_ppc_symbol_db(m_system.GetPPCSymbolDB()),
      m_execution_observer_id(m_system.GetCPU().GetExecutionState().RegisterClient(
          [](std::shared_ptr<const Core::Debug::ExecutionEvent>) {
            Core::QueueHostJob([](Core::System&) { Host_UpdateDisasmDialog(); }, true);
          }))
{
  setWindowTitle(tr("Code"));
  setObjectName(QStringLiteral("code"));

  setHidden(!Settings::Instance().IsCodeVisible() || !Settings::Instance().IsDebugModeEnabled());

  setAllowedAreas(Qt::AllDockWidgetAreas);

  CreateWidgets();

  auto& settings = Settings::GetQSettings();

  restoreGeometry(settings.value(QStringLiteral("codewidget/geometry")).toByteArray());
  // macOS: setHidden() needs to be evaluated before setFloating() for proper window presentation
  // according to Settings
  setFloating(settings.value(QStringLiteral("codewidget/floating")).toBool());

  connect(&Settings::Instance(), &Settings::CodeVisibilityChanged, this,
          [this](bool visible) { setHidden(!visible); });

  connect(Host::GetInstance(), &Host::UpdateDisasmDialog, this, [this] {
    if (!m_lock_btn->isChecked() && Core::GetState(m_system) == Core::State::Paused)
      SetAddress(m_system.GetPPCState().pc, CodeViewWidget::SetAddressUpdate::WithoutUpdate);
    Update();
    if (m_branch_watch_dialog != nullptr)
      m_branch_watch_dialog->Update();
  });

  connect(&Settings::Instance(), &Settings::DebugModeToggled, this,
          [this](bool enabled) { setHidden(!enabled || !Settings::Instance().IsCodeVisible()); });

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [this](Core::State state) {
    if (state == Core::State::Stopping || state == Core::State::Uninitialized)
    {
      CancelAndJoinStepWorker();
      m_source_view->Clear();
    }
    Update();
  });

  ConnectWidgets();

  m_code_splitter->restoreState(
      settings.value(QStringLiteral("codewidget/codesplitter")).toByteArray());
  m_box_splitter->restoreState(
      settings.value(QStringLiteral("codewidget/boxsplitter")).toByteArray());
}

CodeWidget::~CodeWidget()
{
  CancelAndJoinStepWorker();
  m_system.GetCPU().GetExecutionState().UnregisterClient(m_execution_observer_id);
  auto& settings = Settings::GetQSettings();

  settings.setValue(QStringLiteral("codewidget/geometry"), saveGeometry());
  settings.setValue(QStringLiteral("codewidget/floating"), isFloating());
  settings.setValue(QStringLiteral("codewidget/codesplitter"), m_code_splitter->saveState());
  settings.setValue(QStringLiteral("codewidget/boxsplitter"), m_box_splitter->saveState());
}

void CodeWidget::closeEvent(QCloseEvent*)
{
  Settings::Instance().SetCodeVisible(false);
}

void CodeWidget::showEvent(QShowEvent* event)
{
  Update();
}

void CodeWidget::CreateWidgets()
{
  auto* layout = new QHBoxLayout;

  layout->setContentsMargins(2, 2, 2, 2);
  layout->setSpacing(0);

  auto* top_layout = new QHBoxLayout;
  m_search_address = new QLineEdit;
  m_search_address->setPlaceholderText(tr("Search Address"));

  m_lock_btn = new QToolButton();
  m_lock_btn->setIcon(Resources::GetThemeIcon("pause"));
  m_lock_btn->setCheckable(true);
  m_lock_btn->setMinimumSize(24, 24);
  m_lock_btn->setToolTip(tr("When enabled, prevents automatic updates to the code view."));
  m_branch_watch = new QPushButton(tr("Branch Watch"));

  top_layout->addWidget(m_search_address);
  top_layout->addWidget(m_lock_btn);
  top_layout->addWidget(m_branch_watch);

  auto* right_layout = new QVBoxLayout;
  m_code_view = new CodeViewWidget;
  m_source_view = new SourceViewWidget(m_ppc_symbol_db);
  m_code_tabs = new QTabWidget;
  m_code_tabs->addTab(m_source_view, tr("Source"));
  m_code_tabs->addTab(m_code_view, tr("Disassembly"));
  m_code_tabs->setCurrentWidget(m_code_view);
  right_layout->addLayout(top_layout);
  right_layout->addWidget(m_code_tabs);

  m_box_splitter = new QSplitter(Qt::Vertical);
  m_box_splitter->setStyleSheet(BOX_SPLITTER_STYLESHEET);

  auto add_search_line_edit = [this](const QString& name, QWidget* list_widget) {
    auto* widget = new QWidget;
    auto* line_layout = new QGridLayout;
    auto* label = new QLabel(name);
    auto* search_line_edit = new QLineEdit;

    widget->setLayout(line_layout);
    line_layout->addWidget(label, 0, 0);
    line_layout->addWidget(search_line_edit, 0, 1);
    line_layout->addWidget(list_widget, 1, 0, -1, -1);
    m_box_splitter->addWidget(widget);
    return search_line_edit;
  };

  // Callstack
  m_callstack_list = new QListWidget;
  m_search_callstack = add_search_line_edit(tr("Callstack"), m_callstack_list);

  // Symbols
  auto* symbols_tab = new QTabWidget;
  m_symbols_list = new QListWidget;
  m_note_list = new QListWidget;
  symbols_tab->addTab(m_symbols_list, tr("Symbols"));
  symbols_tab->addTab(m_note_list, tr("Notes"));
  m_search_symbols = add_search_line_edit(tr("Symbols"), symbols_tab);

  // Function calls
  m_function_calls_list = new QListWidget;
  m_search_calls = add_search_line_edit(tr("Calls"), m_function_calls_list);

  // Function callers
  m_function_callers_list = new QListWidget;
  m_search_callers = add_search_line_edit(tr("Callers"), m_function_callers_list);

  m_code_splitter = new QSplitter(Qt::Horizontal);

  // right_layout is the searchbar area and the codeview.
  QWidget* right_widget = new QWidget;
  right_widget->setLayout(right_layout);

  m_code_splitter->addWidget(m_box_splitter);
  m_code_splitter->addWidget(right_widget);

  layout->addWidget(m_code_splitter);

  // Corrects button height mis-aligning the layout. Note: Margin only populates values after this
  // point.
  const int height_fix =
      m_branch_watch->sizeHint().height() - m_search_address->sizeHint().height();
  auto margins = right_layout->contentsMargins();
  margins.setTop(margins.top() - height_fix / 2);
  right_layout->setContentsMargins(margins);
  right_layout->setSpacing(right_layout->spacing() - height_fix / 2);

  QWidget* widget = new QWidget(this);
  widget->setLayout(layout);
  setWidget(widget);
}

void CodeWidget::ConnectWidgets()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
          [this](Qt::ColorScheme colorScheme) {
            m_box_splitter->setStyleSheet(BOX_SPLITTER_STYLESHEET);
          });
#endif

  connect(m_search_address, &QLineEdit::textChanged, this, &CodeWidget::OnSearchAddress);
  connect(m_search_address, &QLineEdit::returnPressed, this, &CodeWidget::OnSearchAddress);
  connect(m_lock_btn, &QPushButton::toggled, m_code_view, &CodeViewWidget::OnLockAddress);
  connect(m_search_symbols, &QLineEdit::textChanged, this, &CodeWidget::OnSearchSymbols);
  connect(m_search_calls, &QLineEdit::textChanged, this, [this] {
    if (const Common::Symbol* symbol = m_ppc_symbol_db.GetSymbolFromAddr(m_code_view->GetAddress()))
      UpdateFunctionCalls(symbol);
  });
  connect(m_search_callers, &QLineEdit::textChanged, this, [this] {
    if (const Common::Symbol* symbol = m_ppc_symbol_db.GetSymbolFromAddr(m_code_view->GetAddress()))
      UpdateFunctionCallers(symbol);
  });
  connect(m_search_callstack, &QLineEdit::textChanged, this, &CodeWidget::UpdateCallstack);

  connect(m_branch_watch, &QPushButton::clicked, this, &CodeWidget::OnBranchWatchDialog);
  connect(m_note_list, &QListWidget::itemPressed, this, &CodeWidget::OnSelectNote);
  connect(m_symbols_list, &QListWidget::itemPressed, this, &CodeWidget::OnSelectSymbol);
  connect(m_callstack_list, &QListWidget::itemPressed, this, &CodeWidget::OnSelectCallstack);
  connect(m_function_calls_list, &QListWidget::itemPressed, this,
          &CodeWidget::OnSelectFunctionCalls);
  connect(m_function_callers_list, &QListWidget::itemPressed, this,
          &CodeWidget::OnSelectFunctionCallers);

  connect(Host::GetInstance(), &Host::PPCSymbolsChanged, this, &CodeWidget::OnPPCSymbolsChanged);
  connect(Host::GetInstance(), &Host::PPCBreakpointsChanged, m_source_view,
          &SourceViewWidget::RefreshBreakpoints);
  connect(m_source_view, &SourceViewWidget::BreakpointToggleRequested, this, [this](u32 address) {
    m_system.GetPowerPC().GetBreakPoints().ToggleBreakPoint(address);
  });
  connect(m_code_view, &CodeViewWidget::UpdateCodeWidget, this, &CodeWidget::Update);

  connect(m_code_view, &CodeViewWidget::RequestPPCComparison, this,
          &CodeWidget::RequestPPCComparison);
  connect(m_code_view, &CodeViewWidget::ShowMemory, this, &CodeWidget::ShowMemory);
  connect(m_code_view, &CodeViewWidget::ActivateSearch, this, &CodeWidget::ActivateSearchAddress);
}

void CodeWidget::OnBranchWatchDialog()
{
  if (m_branch_watch_dialog == nullptr)
  {
    m_branch_watch_dialog = new BranchWatchDialog(m_system, m_system.GetPowerPC().GetBranchWatch(),
                                                  m_ppc_symbol_db, this, this);
  }
  m_branch_watch_dialog->show();
  m_branch_watch_dialog->raise();
  m_branch_watch_dialog->activateWindow();
}

void CodeWidget::OnSetCodeAddress(u32 address)
{
  SetAddress(address, CodeViewWidget::SetAddressUpdate::WithDetailedUpdate);
}

void CodeWidget::OnPPCSymbolsChanged()
{
  UpdateSymbols();
  UpdateNotes();
  UpdateCallstack();

  const Common::Symbol* symbol = m_ppc_symbol_db.GetSymbolFromAddr(m_code_view->GetAddress());
  UpdateFunctionCalls(symbol);
  UpdateFunctionCallers(symbol);
  m_source_view->Clear();
  NavigateToAddress(m_code_view->GetAddress(), CodeViewWidget::SetAddressUpdate::WithoutUpdate);
}

void CodeWidget::ActivateSearchAddress()
{
  m_search_address->setFocus();
  m_search_address->selectAll();
}

void CodeWidget::OnSearchAddress()
{
  bool good = true;
  u32 address = m_search_address->text().toUInt(&good, 16);

  QPalette palette;
  QFont font;

  if (!good && !m_search_address->text().isEmpty())
  {
    font.setBold(true);
    palette.setColor(QPalette::Text, Qt::red);
  }

  m_search_address->setPalette(palette);
  m_search_address->setFont(font);

  if (good)
    SetAddress(address, CodeViewWidget::SetAddressUpdate::WithUpdate);

  Update();

  m_search_address->setFocus();
}

void CodeWidget::OnSearchSymbols()
{
  m_symbol_filter = m_search_symbols->text();
  UpdateSymbols();
  UpdateNotes();
}

void CodeWidget::OnSelectSymbol()
{
  const auto items = m_symbols_list->selectedItems();
  if (items.isEmpty())
    return;

  const u32 address = items[0]->data(Qt::UserRole).toUInt();
  const Common::Symbol* const symbol = m_ppc_symbol_db.GetSymbolFromAddr(address);

  SetAddress(address, CodeViewWidget::SetAddressUpdate::WithUpdate);
  UpdateCallstack();
  UpdateFunctionCalls(symbol);
  UpdateFunctionCallers(symbol);

  m_code_tabs->currentWidget()->setFocus();
}

void CodeWidget::OnSelectNote()
{
  const auto items = m_note_list->selectedItems();
  if (items.isEmpty())
    return;

  const u32 address = items[0]->data(Qt::UserRole).toUInt();

  SetAddress(address, CodeViewWidget::SetAddressUpdate::WithUpdate);
}

void CodeWidget::OnSelectCallstack()
{
  const auto items = m_callstack_list->selectedItems();
  if (items.isEmpty())
    return;

  SetAddress(items[0]->data(Qt::UserRole).toUInt(), CodeViewWidget::SetAddressUpdate::WithUpdate);
  Update();
}

void CodeWidget::OnSelectFunctionCalls()
{
  const auto items = m_function_calls_list->selectedItems();
  if (items.isEmpty())
    return;

  SetAddress(items[0]->data(Qt::UserRole).toUInt(), CodeViewWidget::SetAddressUpdate::WithUpdate);
  Update();
}

void CodeWidget::OnSelectFunctionCallers()
{
  const auto items = m_function_callers_list->selectedItems();
  if (items.isEmpty())
    return;

  SetAddress(items[0]->data(Qt::UserRole).toUInt(), CodeViewWidget::SetAddressUpdate::WithUpdate);
  Update();
}

void CodeWidget::SetAddress(u32 address, CodeViewWidget::SetAddressUpdate update)
{
  NavigateToAddress(address, update);

  if (update == CodeViewWidget::SetAddressUpdate::WithUpdate ||
      update == CodeViewWidget::SetAddressUpdate::WithDetailedUpdate)
  {
    Settings::Instance().SetCodeVisible(true);
    raise();
    m_code_tabs->currentWidget()->setFocus();
  }
}

void CodeWidget::NavigateToAddress(const u32 address, const CodeViewWidget::SetAddressUpdate update)
{
  m_code_view->SetAddress(address, update);

  const std::optional<PPCSymbolDB::SourceLine> source = m_ppc_symbol_db.GetSourceLine(address);
  if (source && m_source_view->ShowSource(source->file_index, source->line))
  {
    m_code_tabs->setCurrentWidget(m_source_view);
    m_source_view->SetCurrentAddress(Core::GetState(m_system) == Core::State::Paused ?
                                         std::make_optional(m_system.GetPPCState().pc) :
                                         std::nullopt);
  }
  else
  {
    m_code_tabs->setCurrentWidget(m_code_view);
  }
}

void CodeWidget::Update()
{
  if (!isVisible())
    return;

  const Common::Symbol* const symbol = m_ppc_symbol_db.GetSymbolFromAddr(m_code_view->GetAddress());

  UpdateCallstack();

  m_code_view->Update();
  const std::optional<u32> pc = Core::GetState(m_system) == Core::State::Paused ?
                                    std::make_optional(m_system.GetPPCState().pc) :
                                    std::nullopt;
  m_source_view->SetCurrentAddress(pc);
  m_source_view->RefreshBreakpoints();

  UpdateFunctionCalls(symbol);
  UpdateFunctionCallers(symbol);
}

void CodeWidget::UpdateCallstack()
{
  m_callstack_list->clear();

  if (Core::GetState(m_system) != Core::State::Paused)
    return;

  const Core::Debug::StackTrace stack = Core::Debug::GetPPCStackTrace(m_system);

  const QString filter = m_search_callstack->text();

  for (const Core::Debug::StackFrame& frame : stack.frames)
  {
    std::string label = fmt::format("{} [{:08x}]", frame.name, frame.address);
    if (frame.source)
      label += fmt::format(" - {}:{}", frame.source->file, frame.source->line);
    const QString name = QString::fromStdString(label);

    if (!name.contains(filter, Qt::CaseInsensitive))
      continue;

    auto* item = new QListWidgetItem(name);
    item->setData(Qt::UserRole, frame.address);
    m_callstack_list->addItem(item);
  }
}

void CodeWidget::UpdateSymbols()
{
  const QString selection = m_symbols_list->selectedItems().isEmpty() ?
                                QString{} :
                                m_symbols_list->selectedItems()[0]->text();
  m_symbols_list->clear();

  m_ppc_symbol_db.ForEachSymbol([&](const Common::Symbol& symbol) {
    QString name = QString::fromStdString(symbol.name);

    // If the symbol has an object name, add it to the entry name.
    if (!symbol.object_name.empty())
    {
      name += QString::fromStdString(fmt::format(" ({})", symbol.object_name));
    }

    auto* item = new QListWidgetItem(name);
    if (name == selection)
      item->setSelected(true);

    // Disable non-function symbols as you can't do anything with them.
    if (symbol.type != Common::Symbol::Type::Function)
      item->setFlags(Qt::NoItemFlags);

    item->setData(Qt::UserRole, symbol.address);

    if (name.contains(m_symbol_filter, Qt::CaseInsensitive))
      m_symbols_list->addItem(item);
  });

  m_symbols_list->sortItems();
}

void CodeWidget::UpdateNotes()
{
  const QString selection = m_note_list->selectedItems().isEmpty() ?
                                QStringLiteral("") :
                                m_note_list->selectedItems()[0]->text();
  m_note_list->clear();

  m_ppc_symbol_db.ForEachNote([&](const Common::Note& note) {
    const QString name = QString::fromStdString(note.name);

    auto* item = new QListWidgetItem(name);
    if (name == selection)
      item->setSelected(true);

    item->setData(Qt::UserRole, note.address);

    if (name.toUpper().indexOf(m_symbol_filter.toUpper()) != -1)
      m_note_list->addItem(item);
  });

  m_note_list->sortItems();
}

void CodeWidget::UpdateFunctionCalls(const Common::Symbol* symbol)
{
  m_function_calls_list->clear();
  if (symbol == nullptr)
    return;

  const QString filter = m_search_calls->text();

  for (const auto& call : symbol->calls)
  {
    const u32 addr = call.function;
    const Common::Symbol* const call_symbol = m_ppc_symbol_db.GetSymbolFromAddr(addr);

    if (call_symbol)
    {
      QString name;

      if (!call_symbol->object_name.empty())
      {
        name = QString::fromStdString(
            fmt::format("< {} ({}, {:08x})", call_symbol->name, call_symbol->object_name, addr));
      }
      else
      {
        name = QString::fromStdString(fmt::format("< {} ({:08x})", call_symbol->name, addr));
      }

      if (!name.contains(filter, Qt::CaseInsensitive))
        continue;

      auto* item = new QListWidgetItem(name);
      item->setData(Qt::UserRole, addr);
      m_function_calls_list->addItem(item);
    }
  }
}

void CodeWidget::UpdateFunctionCallers(const Common::Symbol* symbol)
{
  m_function_callers_list->clear();
  if (symbol == nullptr)
    return;

  const QString filter = m_search_callers->text();

  for (const auto& caller : symbol->callers)
  {
    const u32 addr = caller.call_address;
    const Common::Symbol* const caller_symbol = m_ppc_symbol_db.GetSymbolFromAddr(addr);

    if (caller_symbol)
    {
      QString name;

      if (!caller_symbol->object_name.empty())
      {
        name = QString::fromStdString(fmt::format("< {} ({}, {:08x})", caller_symbol->name,
                                                  caller_symbol->object_name, addr));
      }
      else
      {
        name = QString::fromStdString(fmt::format("< {} ({:08x})", caller_symbol->name, addr));
      }

      if (!name.contains(filter, Qt::CaseInsensitive))
        continue;

      auto* item = new QListWidgetItem(name);
      item->setData(Qt::UserRole, addr);
      m_function_callers_list->addItem(item);
    }
  }
}

void CodeWidget::Step()
{
  StartStep(Core::Debug::PPCStepMode::Into);
}

void CodeWidget::StepOver()
{
  StartStep(Core::Debug::PPCStepMode::Over);
}

void CodeWidget::StepOut()
{
  StartStep(Core::Debug::PPCStepMode::Out);
}

bool CodeWidget::JoinCompletedStepWorker()
{
  if (!m_step_thread.joinable())
    return true;
  if (!m_step_done.load())
    return false;
  m_step_thread.join();
  return true;
}

void CodeWidget::CancelAndJoinStepWorker()
{
  m_step_cancelled.store(true);
  if (m_step_thread.joinable())
  {
    static_cast<void>(m_system.GetCPU().GetExecutionState().CancelActiveStep(
        m_system.GetCPU().GetHostExecutionClientId()));
    m_step_thread.join();
  }
  m_step_done.store(true);
}

void CodeWidget::StartStep(const Core::Debug::PPCStepMode mode)
{
  auto& cpu = m_system.GetCPU();
  if (!cpu.IsStepping())
    return;
  if (!JoinCompletedStepWorker())
  {
    Core::DisplayMessage(tr("Another step is already in progress.").toStdString(), 2000);
    return;
  }

  const bool source_step = mode != Core::Debug::PPCStepMode::Out &&
                           m_code_tabs->currentWidget() == m_source_view &&
                           m_source_view->HasSource();
  const Core::Debug::PPCStepGranularity granularity =
      source_step ? Core::Debug::PPCStepGranularity::SourceRow :
                    Core::Debug::PPCStepGranularity::Instruction;
  const Core::Debug::ExecutionOperationKind kind = [&] {
    if (mode == Core::Debug::PPCStepMode::Out)
      return Core::Debug::ExecutionOperationKind::StepOut;
    if (mode == Core::Debug::PPCStepMode::Over)
      return source_step ? Core::Debug::ExecutionOperationKind::SourceStepOver :
                           Core::Debug::ExecutionOperationKind::StepOver;
    return source_step ? Core::Debug::ExecutionOperationKind::SourceStepInto :
                         Core::Debug::ExecutionOperationKind::StepInto;
  }();

  auto& execution = cpu.GetExecutionState();
  const auto origin = cpu.GetHostExecutionClientId();
  m_step_cancelled.store(false);
  const auto operation = execution.BeginOperation(origin, kind, &m_step_cancelled);
  if (!operation)
  {
    Core::DisplayMessage(tr("Another step is already in progress.").toStdString(), 2000);
    return;
  }
  m_step_done.store(false);
  if (const auto published =
          execution.PublishContinued(origin, *operation, m_system.GetPPCState().pc);
      !published)
  {
    execution.AbandonStep(*operation);
    m_step_done.store(true);
    Core::DisplayMessage(tr("The step could not be started.").toStdString(), 2000);
    return;
  }

  m_step_thread = std::thread([this, mode, granularity, origin, operation = *operation] {
    Core::Debug::PPCStepOptions options;
    options.cancelled = &m_step_cancelled;
    options.instruction_timeout = std::chrono::milliseconds(20);
    options.timeout = std::chrono::seconds(5);
    options.instruction_cap = 1000000;
    options.ignore_current_code_breakpoint = mode == Core::Debug::PPCStepMode::Out;
    options.temporary_breakpoint_installed = [this, operation](const u32 address) {
      Core::System* const system = &m_system;
      auto cleanup = [system, address] {
        system->GetPowerPC().GetBreakPoints().ClearTemporary(address);
      };
      if (!m_system.GetCPU().GetExecutionState().SetActiveStepCleanup(operation, cleanup))
        cleanup();
    };

    const Core::Debug::PPCStepResult result =
        Core::Debug::StepPPC(m_system, mode, granularity, options);
    auto& worker_execution = m_system.GetCPU().GetExecutionState();
    if (m_step_cancelled.load())
    {
      worker_execution.AbandonStep(operation);
    }
    else if (result == Core::Debug::PPCStepResult::Stepped &&
             worker_execution.IsOperationActive(operation))
    {
      const u32 pc = m_system.GetPPCState().pc;
      if (m_system.GetPowerPC().GetBreakPoints().IsAddressBreakPoint(pc))
      {
        m_system.GetCPU().Break({.cause = Core::Debug::ExecutionStopCause::CodeBreakpoint,
                                 .origin = origin,
                                 .operation_id = operation,
                                 .pc = pc,
                                 .code_breakpoint_address = pc});
      }
      else
      {
        m_system.GetCPU().Break({.cause = Core::Debug::ExecutionStopCause::Step,
                                 .origin = origin,
                                 .operation_id = operation,
                                 .pc = pc});
      }
    }
    else if (result == Core::Debug::PPCStepResult::NotStepped)
    {
      worker_execution.AbandonStep(operation);
    }
    worker_execution.MarkStepWorkerComplete(operation);
    m_step_done.store(true);
  });
}

void CodeWidget::Skip()
{
  m_system.GetPPCState().pc += 4;
  ShowPC();
}

void CodeWidget::ShowPC()
{
  SetAddress(m_system.GetPPCState().pc, CodeViewWidget::SetAddressUpdate::WithUpdate);
  Update();
}

void CodeWidget::SetPC()
{
  const std::optional<u32> address = GetActiveAddress();
  if (!address)
    return;
  m_system.GetPPCState().pc = *address;
  Update();
}

void CodeWidget::ToggleBreakpoint()
{
  if (m_code_tabs->currentWidget() == m_source_view)
  {
    const std::vector<u32> addresses = m_source_view->GetSelectedAddresses();
    if (addresses.empty())
      return;

    auto& breakpoints = m_system.GetPowerPC().GetBreakPoints();
    const auto snapshot = breakpoints.GetSnapshot();
    bool removed_existing = false;
    for (const u32 address : addresses)
    {
      if (snapshot->GetRegularBreakpoint(address) != nullptr)
      {
        breakpoints.ToggleBreakPoint(address);
        removed_existing = true;
      }
    }
    if (!removed_existing)
      breakpoints.ToggleBreakPoint(addresses.front());
    return;
  }
  m_code_view->ToggleBreakpoint();
}

void CodeWidget::AddBreakpoint()
{
  if (m_code_tabs->currentWidget() == m_source_view)
  {
    if (const std::optional<u32> address = m_source_view->GetSelectedAddress())
      static_cast<void>(m_system.GetPowerPC().GetBreakPoints().Add(*address));
    return;
  }
  m_code_view->AddBreakpoint();
}

std::optional<u32> CodeWidget::GetActiveAddress() const
{
  if (m_code_tabs->currentWidget() == m_source_view)
    return m_source_view->GetSelectedAddress();
  return m_code_view->GetAddress();
}
