// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <vector>

#include <OptionParser.h>
#include <gtest/gtest.h>

#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "UICommon/CommandLineParse.h"

namespace
{
class CommandLineParseTest : public ::testing::TestWithParam<CommandLineParse::ParserOptions>
{
protected:
  void SetUp() override
  {
    Config::Init();
    Config::OnConfigChanged();
  }
  void TearDown() override { Config::Shutdown(); }
};

TEST_P(CommandLineParseTest, LongDebuggerFlagEnablesCoreDebugging)
{
  ASSERT_FALSE(Config::Get(Config::MAIN_ENABLE_DEBUGGING));
  auto parser = CommandLineParse::CreateParser(GetParam());
  CommandLineParse::ParseArguments(parser.get(), std::vector<std::string>{"--debugger"});
  EXPECT_TRUE(Config::Get(Config::MAIN_ENABLE_DEBUGGING));
}

TEST_P(CommandLineParseTest, ShortDebuggerFlagEnablesCoreDebugging)
{
  ASSERT_FALSE(Config::Get(Config::MAIN_ENABLE_DEBUGGING));
  auto parser = CommandLineParse::CreateParser(GetParam());
  CommandLineParse::ParseArguments(parser.get(), std::vector<std::string>{"-d"});
  EXPECT_TRUE(Config::Get(Config::MAIN_ENABLE_DEBUGGING));
}

TEST_P(CommandLineParseTest, DebuggingIsDisabledByDefault)
{
  ASSERT_FALSE(Config::Get(Config::MAIN_ENABLE_DEBUGGING));
  auto parser = CommandLineParse::CreateParser(GetParam());
  CommandLineParse::ParseArguments(parser.get(), std::vector<std::string>{});
  EXPECT_FALSE(Config::Get(Config::MAIN_ENABLE_DEBUGGING));
}

TEST_P(CommandLineParseTest, OmittingFlagPreservesConfiguredDebugging)
{
  Config::SetCurrent(Config::MAIN_ENABLE_DEBUGGING, true);
  auto parser = CommandLineParse::CreateParser(GetParam());
  CommandLineParse::ParseArguments(parser.get(), std::vector<std::string>{});
  EXPECT_TRUE(Config::Get(Config::MAIN_ENABLE_DEBUGGING));
}

INSTANTIATE_TEST_SUITE_P(GuiAndNoGui, CommandLineParseTest,
                         ::testing::Values(CommandLineParse::ParserOptions::IncludeGUIOptions,
                                           CommandLineParse::ParserOptions::OmitGUIOptions));
}  // namespace
