// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <string>

#include "Common/FileUtil.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/DWARF/DwarfImport.h"
#include "../DWARF/DwarfTestFixture.h"
#include "Core/Debugger/Entrypoints/EntrypointsImport.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
class EntrypointsImportTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    auto& system = Core::System::GetInstance();
    system.GetMemory().Init();
    AddressSpace::Init();
    system.GetCoreTiming().Init();
    Core::DeclareAsCPUThread();
    system.GetCPU().Init(PowerPC::CPUCore::Interpreter);
    system.GetPPCSymbolDB().Clear();
    m_temp_dir = File::CreateTempDir();
    ASSERT_FALSE(m_temp_dir.empty());
  }

  void TearDown() override
  {
    auto& system = Core::System::GetInstance();
    system.GetCPU().Shutdown();
    AddressSpace::Shutdown();
    system.GetMemory().Shutdown();
    system.GetCoreTiming().Shutdown();
    Core::UndeclareAsCPUThread();
    File::DeleteDirRecursively(m_temp_dir);
  }

  std::string WriteEntrypointsJson(std::string contents) const
  {
    const std::string path = m_temp_dir + "/entrypoints.json";
    EXPECT_TRUE(File::WriteStringToFile(path, std::move(contents)));
    return path;
  }

  static PPCSymbolDB& SymbolDB() { return Core::System::GetInstance().GetPPCSymbolDB(); }

  std::string m_temp_dir;
};

TEST_F(EntrypointsImportTest, ImportAddsSymbolAndSparseLineEntry)
{
  Core::CPUThreadGuard guard(Core::System::GetInstance());
  const std::string path = WriteEntrypointsJson(R"({
    "schema_version": 1,
    "precision": "entrypoint",
    "game_id": "GALE01",
    "dwarf_elf": "main.elf",
    "functions": [
      {
        "name": "GetMatchTimer",
        "address": 2148970376,
        "size": 124,
        "file": "src/melee/gm/gm_16AE.c",
        "line": 117
      }
    ]
  })");

  ASSERT_TRUE(Core::Debug::ImportEntrypointsFromJson(guard, SymbolDB(), path));

  const Common::Symbol* symbol = SymbolDB().GetSymbolFromAddr(0x8016AF88);
  ASSERT_NE(symbol, nullptr);
  EXPECT_EQ(symbol->function_name, "GetMatchTimer");

  const std::optional<PPCSymbolDB::SourceLine> line =
      SymbolDB().GetSourceLine(0x8016AF90);
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line->file, "src/melee/gm/gm_16AE.c");
  EXPECT_EQ(line->line, 117u);
}

TEST_F(EntrypointsImportTest, SkipsWhenDenseDwarfLineInfoExists)
{
  Core::CPUThreadGuard guard(Core::System::GetInstance());
  ASSERT_TRUE(Core::Debug::ImportDwarf(guard, SymbolDB(), DwarfTestFixture::kDebugSection,
                                        DwarfTestFixture::kLineSection));

  const std::string path = WriteEntrypointsJson(R"({
    "schema_version": 1,
    "precision": "entrypoint",
    "game_id": "GALE01",
    "dwarf_elf": "main.elf",
    "functions": [
      {
        "name": "fixture_func",
        "address": 12544,
        "size": 32,
        "file": "fixture.c",
        "line": 99
      }
    ]
  })");

  EXPECT_FALSE(Core::Debug::ImportEntrypointsFromJson(guard, SymbolDB(), path));
}

TEST_F(EntrypointsImportTest, RejectsUnsupportedSchemaVersion)
{
  Core::CPUThreadGuard guard(Core::System::GetInstance());
  const std::string path = WriteEntrypointsJson(R"({
    "schema_version": 2,
    "precision": "entrypoint",
    "game_id": "GALE01",
    "dwarf_elf": "main.elf",
    "functions": []
  })");

  EXPECT_FALSE(Core::Debug::ImportEntrypointsFromJson(guard, SymbolDB(), path));
}
}  // namespace
