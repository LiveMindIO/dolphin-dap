// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/GameConfigWidget.h"

#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Common/FileUtil.h"

#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/ConfigManager.h"
#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/ConfigControls/ConfigFloatSlider.h"
#include "DolphinQt/Config/ConfigControls/ConfigInteger.h"
#include "DolphinQt/Config/ConfigControls/ConfigRadio.h"
#include "DolphinQt/Config/ConfigControls/ConfigText.h"
#include "DolphinQt/Config/GameConfigEdit.h"
#include "DolphinQt/Config/Graphics/GraphicsPane.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/QtUtils.h"
#include "DolphinQt/QtUtils/WrapInScrollArea.h"

#include "UICommon/GameFile.h"

static void PopulateTab(QTabWidget* tab, const std::string& path, std::string& game_id,
                        u16 revision, bool read_only)
{
  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(game_id, revision))
  {
    const std::string ini_path = path + filename;
    if (File::Exists(ini_path))
    {
      auto* edit = new GameConfigEdit(nullptr, QString::fromStdString(ini_path), read_only);
      tab->addTab(edit, QString::fromStdString(filename));
    }
  }
}

GameConfigWidget::GameConfigWidget(const UICommon::GameFile& game) : m_game(game)
{
  m_game_id = m_game.GetGameID();

  m_gameini_local_path =
      QString::fromStdString(File::GetUserPath(D_GAMESETTINGS_IDX) + m_game_id + ".ini");

  m_layer = std::make_unique<Config::Layer>(
      ConfigLoaders::GenerateLocalGameConfigLoader(m_game_id, m_game.GetRevision()));
  m_global_layer = std::make_unique<Config::Layer>(
      ConfigLoaders::GenerateGlobalGameConfigLoader(m_game_id, m_game.GetRevision()));

  CreateWidgets();
  connect(&Settings::Instance(), &Settings::ConfigChanged, this, &GameConfigWidget::LoadSettings);

  PopulateTab(m_default_tab, File::GetSysDirectory() + GAMESETTINGS_DIR DIR_SEP, m_game_id,
              m_game.GetRevision(), true);
  PopulateTab(m_local_tab, File::GetUserPath(D_GAMESETTINGS_IDX), m_game_id, m_game.GetRevision(),
              false);

  bool game_id_tab = false;
  for (int i = 0; i < m_local_tab->count(); i++)
  {
    if (m_local_tab->tabText(i).toStdString() == m_game_id + ".ini")
      game_id_tab = true;
  }

  if (game_id_tab == false)
  {
    // Create new local game ini tab if none exists.
    auto* edit = new GameConfigEdit(
        nullptr, QString::fromStdString(File::GetUserPath(D_GAMESETTINGS_IDX) + m_game_id + ".ini"),
        false);
    m_local_tab->addTab(edit, QString::fromStdString(m_game_id + ".ini"));
  }

  // Fails to change font if it's directly called at this time. Is there a better workaround?
  QTimer::singleShot(100, this, [this] {
    SetItalics();
    Config::OnConfigChanged();
  });
}

void GameConfigWidget::CreateWidgets()
{
  Config::Layer* layer = m_layer.get();
  // Core
  auto* core_box = new QGroupBox(tr("Core"));
  auto* core_layout = new QGridLayout;
  core_box->setLayout(core_layout);

  m_enable_dual_core = new ConfigBool(tr("Enable Dual Core"), Config::MAIN_CPU_THREAD, layer);
  m_enable_mmu = new ConfigBool(tr("Enable MMU"), Config::MAIN_MMU, layer);
  m_enable_fprf = new ConfigBool(tr("Enable FPRF"), Config::MAIN_FPRF, layer);
  m_sync_gpu = new ConfigBool(tr("Synchronize GPU thread"), Config::MAIN_SYNC_GPU, layer);
  m_emulate_disc_speed =
      new ConfigBool(tr("Emulate Disc Speed"), Config::MAIN_FAST_DISC_SPEED, layer, true);
  m_use_dsp_hle = new ConfigBool(tr("DSP HLE (fast)"), Config::MAIN_DSP_HLE, layer);

  const std::vector<std::string> choice{tr("auto").toStdString(), tr("none").toStdString(),
                                        tr("fake-completion").toStdString()};
  m_deterministic_dual_core =
      new ConfigStringChoice(choice, Config::MAIN_GPU_DETERMINISM_MODE, layer);

  m_enable_mmu->SetDescription(tr(
      "Enables the Memory Management Unit, needed for some games. (ON = Compatible, OFF = Fast)"));

  m_enable_fprf->SetDescription(
      tr("Enables Floating Point Result Flag calculation, needed for a few "
         "games. (ON = Compatible, OFF = Fast)"));
  m_sync_gpu->SetDescription(
      tr("Synchronizes the GPU and CPU threads to help prevent random freezes "
         "in Dual core mode. (ON = Compatible, OFF = Fast)"));
  m_emulate_disc_speed->SetDescription(
      tr("Enable emulated disc speed. Disabling this can cause crashes "
         "and other problems in some games. "
         "(ON = Compatible, OFF = Unlocked)"));

  core_layout->addWidget(m_enable_dual_core, 0, 0);
  core_layout->addWidget(m_enable_mmu, 1, 0);
  core_layout->addWidget(m_enable_fprf, 2, 0);
  core_layout->addWidget(m_sync_gpu, 3, 0);
  core_layout->addWidget(m_emulate_disc_speed, 4, 0);
  core_layout->addWidget(m_use_dsp_hle, 5, 0);
  core_layout->addWidget(new QLabel(tr("Deterministic dual core:")), 6, 0);
  core_layout->addWidget(m_deterministic_dual_core, 6, 1);

  // Stereoscopy
  auto* stereoscopy_box = new QGroupBox(tr("Stereoscopy"));
  auto* stereoscopy_layout = new QGridLayout;
  stereoscopy_box->setLayout(stereoscopy_layout);

  m_depth_slider =
      new ConfigFloatSlider(100, 200, Config::GFX_STEREO_DEPTH_PERCENTAGE, 1.0f, layer);
  m_convergence_slider =
      new ConfigFloatSlider(0, 1000, Config::GFX_STEREO_CONVERGENCE, 0.01f, layer);
  auto* const depth_slider_value = new QLabel();
  auto* const convergence_slider_value = new QLabel();
  m_use_monoscopic_shadows =
      new ConfigBool(tr("Monoscopic Shadows"), Config::GFX_STEREO_EFB_MONO_DEPTH, layer);

  m_depth_slider->SetDescription(
      tr("This value is multiplied with the depth set in the graphics configuration."));
  m_convergence_slider->SetDescription(
      tr("This value is added to the convergence value set in the graphics configuration."));
  m_use_monoscopic_shadows->SetDescription(
      tr("Use a single depth buffer for both eyes. Needed for a few games."));

  stereoscopy_layout->addWidget(new ConfigFloatLabel(tr("Depth Percentage:"), m_depth_slider), 0,
                                0);
  stereoscopy_layout->addWidget(m_depth_slider, 0, 1);
  stereoscopy_layout->addWidget(depth_slider_value, 0, 2);
  stereoscopy_layout->addWidget(new ConfigFloatLabel(tr("Convergence:"), m_convergence_slider), 1,
                                0);
  stereoscopy_layout->addWidget(m_convergence_slider, 1, 1);
  stereoscopy_layout->addWidget(convergence_slider_value, 1, 2);
  stereoscopy_layout->addWidget(m_use_monoscopic_shadows, 2, 0);

  depth_slider_value->setText(QString::asprintf("%.0f%%", m_depth_slider->GetValue()));
  convergence_slider_value->setText(QString::asprintf("%.2f", m_convergence_slider->GetValue()));

  auto* general_layout = new QVBoxLayout;
  general_layout->addWidget(core_box);
  general_layout->addWidget(stereoscopy_box);
  general_layout->addStretch();

  auto* general_widget = new QWidget;
  general_widget->setLayout(general_layout);

  // Debugging
  auto* debugging_layout = new QGridLayout;
  debugging_layout->setColumnStretch(1, 1);
  auto* debugging_widget = new QWidget;
  debugging_widget->setLayout(debugging_layout);

  m_debug_symbol_map = new ConfigText(Config::MAIN_DEBUG_SYMBOL_MAP, layer);
  m_debug_elf_file = new ConfigText(Config::MAIN_DEBUG_ELF_FILE, layer);
  m_debug_source_paths = new QTableWidget(0, 1);
  m_debug_source_paths->setHorizontalHeaderLabels({tr("Directory")});
  m_debug_source_paths->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  m_debug_source_paths->verticalHeader()->setVisible(false);
  m_debug_source_paths->setAlternatingRowColors(true);
  m_debug_source_paths->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_debug_source_paths->setSelectionMode(QAbstractItemView::SingleSelection);
  m_debug_source_paths->setEditTriggers(QAbstractItemView::DoubleClicked |
                                        QAbstractItemView::EditKeyPressed);
  m_debug_source_paths->setMinimumHeight(120);
  m_debug_replace_disc_executable =
      new ConfigBool(tr("Replace the disc executable with the ELF file"),
                     Config::MAIN_DEBUG_REPLACE_DISC_EXECUTABLE, layer);
  m_debug_replace_disc_executable->SetDescription(
      tr("Runs the disc bootstrap and apploader, then overlays the ELF file and starts at its "
         "entry point. When disabled, Dolphin only imports symbols and DWARF from the ELF. Changes "
         "take effect the next time the game is started."));

  auto* map_browse = new NonDefaultQPushButton(QStringLiteral("..."));
  auto* elf_browse = new NonDefaultQPushButton(QStringLiteral("..."));
  auto* source_browse = new NonDefaultQPushButton(tr("Add..."));
  m_debug_remove_source_path = new NonDefaultQPushButton(tr("Remove"));
  m_debug_remove_source_path->setEnabled(false);
  const auto initial_directory = [this](const ConfigText* edit) {
    if (!edit->text().isEmpty())
      return QFileInfo(edit->text()).absolutePath();
    return QFileInfo(QString::fromStdString(m_game.GetFilePath())).absolutePath();
  };
  connect(map_browse, &QPushButton::clicked, this, [this, initial_directory] {
    const QString path = DolphinFileDialog::getOpenFileName(this, tr("Select Memory Map File"),
                                                            initial_directory(m_debug_symbol_map),
                                                            tr("Map Files (*.map);;All Files (*)"));
    if (!path.isEmpty())
      m_debug_symbol_map->SetTextAndUpdate(QDir::toNativeSeparators(path));
  });
  connect(elf_browse, &QPushButton::clicked, this, [this, initial_directory] {
    const QString path = DolphinFileDialog::getOpenFileName(this, tr("Select ELF File"),
                                                            initial_directory(m_debug_elf_file),
                                                            tr("ELF Files (*.elf);;All Files (*)"));
    if (!path.isEmpty())
      m_debug_elf_file->SetTextAndUpdate(QDir::toNativeSeparators(path));
  });
  connect(source_browse, &QPushButton::clicked, this, [this] {
    const QString path = DolphinFileDialog::getExistingDirectory(
        this, tr("Add Source Directory"),
        QFileInfo(QString::fromStdString(m_game.GetFilePath())).absolutePath());
    if (path.isEmpty())
      return;

    const QSignalBlocker blocker(m_debug_source_paths);
    const int row = m_debug_source_paths->rowCount();
    m_debug_source_paths->insertRow(row);
    m_debug_source_paths->setItem(row, 0, new QTableWidgetItem(QDir::toNativeSeparators(path)));
    m_debug_source_paths->selectRow(row);
    SaveSourcePaths();
  });
  connect(m_debug_remove_source_path, &QPushButton::clicked, this, [this] {
    const int row = m_debug_source_paths->currentRow();
    if (row < 0)
      return;
    const QSignalBlocker blocker(m_debug_source_paths);
    m_debug_source_paths->removeRow(row);
    SaveSourcePaths();
  });
  connect(m_debug_source_paths, &QTableWidget::itemSelectionChanged, this, [this] {
    m_debug_remove_source_path->setEnabled(m_debug_source_paths->currentRow() >= 0);
  });
  connect(m_debug_source_paths, &QTableWidget::itemChanged, this, [this] { SaveSourcePaths(); });

  auto* source_paths_description =
      new QLabel(tr("Dolphin searches these directories in order to resolve relative or "
                    "basename-only paths stored in DWARF. Double-click an entry to edit it."));
  source_paths_description->setWordWrap(true);

  debugging_layout->addWidget(new QLabel(tr("Memory map file:")), 0, 0);
  debugging_layout->addWidget(m_debug_symbol_map, 0, 1);
  debugging_layout->addWidget(map_browse, 0, 2);
  debugging_layout->addWidget(new QLabel(tr("ELF file:")), 1, 0);
  debugging_layout->addWidget(m_debug_elf_file, 1, 1);
  debugging_layout->addWidget(elf_browse, 1, 2);
  auto* source_buttons = new QVBoxLayout;
  source_buttons->addWidget(source_browse);
  source_buttons->addWidget(m_debug_remove_source_path);
  source_buttons->addStretch();
  debugging_layout->addWidget(new QLabel(tr("Source paths:")), 2, 0, Qt::AlignTop);
  debugging_layout->addWidget(m_debug_source_paths, 2, 1);
  debugging_layout->addLayout(source_buttons, 2, 2);
  debugging_layout->addWidget(source_paths_description, 3, 1, 1, 2);
  debugging_layout->addWidget(m_debug_replace_disc_executable, 4, 0, 1, 3);
  debugging_layout->setRowStretch(5, 1);

  // Editor tab
  auto* advanced_layout = new QVBoxLayout;

  auto* default_group = new QGroupBox(tr("Default Config (Read Only)"));
  auto* default_layout = new QVBoxLayout;
  m_default_tab = new QTabWidget;

  default_group->setLayout(default_layout);
  default_layout->addWidget(m_default_tab);

  auto* local_group = new QGroupBox(tr("User Config"));
  auto* local_layout = new QVBoxLayout;
  m_local_tab = new QTabWidget;

  local_group->setLayout(local_layout);
  local_layout->addWidget(m_local_tab);

  advanced_layout->addWidget(default_group);
  advanced_layout->addWidget(local_group);

  auto* advanced_widget = new QWidget;

  advanced_widget->setLayout(advanced_layout);

  auto* layout = new QVBoxLayout;
  auto* tab_widget = new QTabWidget;
  tab_widget->addTab(general_widget, tr("General"));

  auto* const gfx_widget = new GraphicsPane{nullptr, m_layer.get()};
  tab_widget->addTab(gfx_widget, tr("Graphics"));

  const bool is_disc = m_game.GetPlatform() == DiscIO::Platform::GameCubeDisc ||
                       m_game.GetPlatform() == DiscIO::Platform::Triforce ||
                       m_game.GetPlatform() == DiscIO::Platform::WiiDisc;
  if (is_disc)
    tab_widget->addTab(debugging_widget, tr("Debugging"));
  else
  {
    delete debugging_widget;
    m_debug_symbol_map = nullptr;
    m_debug_elf_file = nullptr;
    m_debug_source_paths = nullptr;
    m_debug_remove_source_path = nullptr;
    m_debug_replace_disc_executable = nullptr;
  }

  const int editor_index = tab_widget->addTab(advanced_widget, tr("Editor"));

  connect(tab_widget, &QTabWidget::currentChanged, this, [this, editor_index](int index) {
    // Update the ini editor after editing other tabs.
    if (index == editor_index)
    {
      // Layer only auto-saves when it is destroyed.
      m_layer->Save();

      // There can be multiple ini loaded for a game, only replace the one related to the game
      // ini being edited.
      for (int i = 0; i < m_local_tab->count(); i++)
      {
        if (m_local_tab->tabText(i).toStdString() == m_game_id + ".ini")
        {
          m_local_tab->removeTab(i);

          auto* edit = new GameConfigEdit(
              nullptr,
              QString::fromStdString(File::GetUserPath(D_GAMESETTINGS_IDX) + m_game_id + ".ini"),
              false);

          m_local_tab->insertTab(i, edit, QString::fromStdString(m_game_id + ".ini"));
          break;
        }
      }
    }

    // Update other tabs after using ini editor.
    if (m_prev_tab_index == editor_index)
    {
      // Load won't clear deleted keys, so everything is wiped before loading.
      m_layer->DeleteAllKeys();
      m_layer->Load();
      Config::OnConfigChanged();
    }

    m_prev_tab_index = index;
  });

  connect(m_depth_slider, &ConfigFloatSlider::valueChanged, this, [this, depth_slider_value] {
    depth_slider_value->setText(QString::asprintf("%.0f%%", m_depth_slider->GetValue()));
  });
  connect(m_convergence_slider, &ConfigFloatSlider::valueChanged, this,
          [this, convergence_slider_value] {
            convergence_slider_value->setText(
                QString::asprintf("%.2f", m_convergence_slider->GetValue()));
          });

  const QString help_msg = tr(
      "Italics mark default game settings, bold marks user settings.\nRight-click to remove user "
      "settings.\nGraphics tabs don't display the value of a default game setting.\nAnti-Aliasing "
      "settings are disabled when the global graphics backend doesn't "
      "match the game setting.");

  auto* const help_label = new QLabel(tr("These settings override core Dolphin settings."));

  auto* const help_widget =
      QtUtils::CreateIconWarning(this, QStyle::SP_MessageBoxQuestion, help_label);

  help_widget->setToolTip(help_msg);

  layout->addWidget(help_widget);
  layout->addWidget(tab_widget);
  setLayout(layout);
}

GameConfigWidget::~GameConfigWidget()
{
  // Destructor saves the layer to file.
  m_layer.reset();

  // If a game is running and the game properties window is closed, update local game layer with
  // any new changes. Not sure if doing it more frequently is safe.
  auto local_layer = Config::GetLayer(Config::LayerType::LocalGame);
  if (local_layer && SConfig::GetInstance().GetGameID() == m_game_id)
  {
    local_layer->DeleteAllKeys();
    local_layer->Load();
    Config::OnConfigChanged();
  }

  // Delete empty configs
  if (File::GetSize(m_gameini_local_path.toStdString()) == 0)
    File::Delete(m_gameini_local_path.toStdString());
}

void GameConfigWidget::LoadSettings()
{
  // Load globals
  auto update_bool = [this](auto config, bool reverse = false) {
    const Config::Location& setting = config->GetLocation();

    // Don't overwrite local with global
    if (m_layer->Exists(setting) || !m_global_layer->Exists(setting))
      return;

    std::optional<bool> value = m_global_layer->Get<bool>(config->GetLocation());

    if (value.has_value())
    {
      const QSignalBlocker blocker(config);
      config->setChecked(value.value() ^ reverse);
    }
  };

  auto update_int = [this](auto config) {
    const Config::Location& setting = config->GetLocation();

    if (m_layer->Exists(setting) || !m_global_layer->Exists(setting))
      return;

    std::optional<int> value = m_global_layer->Get<int>(setting);

    if (value.has_value())
    {
      const QSignalBlocker blocker(config);
      config->setValue(value.value());
    }
  };

  auto update_string = [this](ConfigText* config) {
    const Config::Location& setting = config->GetLocation();
    if (m_layer->Exists(setting) || !m_global_layer->Exists(setting))
      return;
    const std::optional<std::string> value = m_global_layer->Get<std::string>(setting);
    if (value)
    {
      const QSignalBlocker blocker(config);
      config->setText(QString::fromStdString(*value));
    }
  };

  for (ConfigBool* config : {m_enable_dual_core, m_enable_mmu, m_enable_fprf, m_sync_gpu,
                             m_use_dsp_hle, m_use_monoscopic_shadows})
  {
    update_bool(config);
  }

  update_bool(m_emulate_disc_speed, true);
  if (m_debug_replace_disc_executable)
  {
    update_bool(m_debug_replace_disc_executable);
    update_string(m_debug_symbol_map);
    update_string(m_debug_elf_file);
    LoadSourcePaths();
  }

  update_int(m_depth_slider);
  update_int(m_convergence_slider);
}

void GameConfigWidget::LoadSourcePaths()
{
  const Config::Location& location = Config::MAIN_DEBUG_SOURCE_PATHS.GetLocation();
  const bool has_local_value = m_layer->Exists(location);
  const bool has_global_value = m_global_layer->Exists(location);

  std::string value;
  if (has_local_value)
    value = m_layer->Get(Config::MAIN_DEBUG_SOURCE_PATHS);
  else if (has_global_value)
    value = m_global_layer->Get(Config::MAIN_DEBUG_SOURCE_PATHS);
  else
    value = Config::GetBase(Config::MAIN_DEBUG_SOURCE_PATHS);

  const QSignalBlocker blocker(m_debug_source_paths);
  m_debug_source_paths->setRowCount(0);
  m_debug_remove_source_path->setEnabled(false);
  const QStringList paths =
      QString::fromStdString(value).split(QLatin1Char(';'), Qt::SkipEmptyParts);
  for (const QString& path : paths)
  {
    const int row = m_debug_source_paths->rowCount();
    m_debug_source_paths->insertRow(row);
    m_debug_source_paths->setItem(row, 0, new QTableWidgetItem(path));
  }

  QFont font = m_debug_source_paths->font();
  font.setBold(has_local_value);
  font.setItalic(!has_local_value && has_global_value);
  m_debug_source_paths->setFont(font);
}

void GameConfigWidget::SaveSourcePaths()
{
  QStringList paths;
  for (int row = 0; row < m_debug_source_paths->rowCount(); ++row)
  {
    const QTableWidgetItem* const item = m_debug_source_paths->item(row, 0);
    if (item && !item->text().trimmed().isEmpty())
      paths.push_back(item->text().trimmed());
  }

  m_layer->Set(Config::MAIN_DEBUG_SOURCE_PATHS.GetLocation(),
               paths.join(QLatin1Char(';')).toStdString());
  Config::OnConfigChanged();
}

void GameConfigWidget::SetItalics()
{
  // Mark system game settings with italics. Called once because it should never change.
  auto italics = [this](auto config) {
    if (!m_global_layer->Exists(config->GetLocation()))
      return;

    QFont ifont = config->font();
    ifont.setItalic(true);
    config->setFont(ifont);
  };

  for (auto* config : findChildren<ConfigBool*>())
    italics(config);
  for (auto* config : findChildren<ConfigFloatSlider*>())
    italics(config);
  for (auto* config : findChildren<ConfigInteger*>())
    italics(config);
  for (auto* config : findChildren<ConfigRadioInt*>())
    italics(config);
  for (auto* config : findChildren<ConfigChoice*>())
    italics(config);
  for (auto* config : findChildren<ConfigStringChoice*>())
    italics(config);
  for (auto* config : findChildren<ConfigText*>())
    italics(config);

  for (auto* config : findChildren<ConfigComplexChoice*>())
  {
    std::pair<Config::Location, Config::Location> location = config->GetLocation();
    if (m_global_layer->Exists(location.first) || m_global_layer->Exists(location.second))
    {
      QFont ifont = config->font();
      ifont.setItalic(true);
      config->setFont(ifont);
    }
  }
}
