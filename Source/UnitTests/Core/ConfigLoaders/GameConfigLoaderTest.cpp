// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>

#include <gtest/gtest.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Common/FileUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "UICommon/UICommon.h"

namespace
{
class GameConfigLoaderTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    m_previous_user_directory = File::GetUserPath(D_USER_IDX);
    m_user_directory = File::CreateTempDir();
    ASSERT_FALSE(m_user_directory.empty());
    UICommon::SetUserDirectory(m_user_directory);
    ASSERT_TRUE(File::CreateFullPath(File::GetUserPath(D_GAMESETTINGS_IDX) + "GALE01.ini"));
    Config::Init();
  }

  void TearDown() override
  {
    Config::Shutdown();
    UICommon::SetUserDirectory(m_previous_user_directory);
    File::DeleteDirRecursively(m_user_directory);
  }

  std::string m_previous_user_directory;
  std::string m_user_directory;
};

TEST_F(GameConfigLoaderTest, PersistsAndRemovesDiscDebugAssociation)
{
  {
    Config::Layer layer(ConfigLoaders::GenerateLocalGameConfigLoader("GALE01", 2));
    layer.Set(Config::MAIN_DEBUG_SYMBOL_MAP.GetLocation(), std::string{"/debug/main.map"});
    layer.Set(Config::MAIN_DEBUG_ALTERNATE_ELF.GetLocation(), std::string{"/debug/main.elf"});
    layer.Set(Config::MAIN_DEBUG_REPLACE_DISC_EXECUTABLE.GetLocation(), true);
    layer.Save();
  }

  {
    Config::Layer layer(ConfigLoaders::GenerateLocalGameConfigLoader("GALE01", 2));
    EXPECT_EQ(layer.Get(Config::MAIN_DEBUG_SYMBOL_MAP), "/debug/main.map");
    EXPECT_EQ(layer.Get(Config::MAIN_DEBUG_ALTERNATE_ELF), "/debug/main.elf");
    EXPECT_TRUE(layer.Get(Config::MAIN_DEBUG_REPLACE_DISC_EXECUTABLE));

    layer.DeleteKey(Config::MAIN_DEBUG_SYMBOL_MAP.GetLocation());
    layer.DeleteKey(Config::MAIN_DEBUG_ALTERNATE_ELF.GetLocation());
    layer.DeleteKey(Config::MAIN_DEBUG_REPLACE_DISC_EXECUTABLE.GetLocation());
    layer.Save();
  }

  Config::Layer layer(ConfigLoaders::GenerateLocalGameConfigLoader("GALE01", 2));
  EXPECT_FALSE(layer.Exists(Config::MAIN_DEBUG_SYMBOL_MAP.GetLocation()));
  EXPECT_FALSE(layer.Exists(Config::MAIN_DEBUG_ALTERNATE_ELF.GetLocation()));
  EXPECT_FALSE(layer.Exists(Config::MAIN_DEBUG_REPLACE_DISC_EXECUTABLE.GetLocation()));
}
}  // namespace
