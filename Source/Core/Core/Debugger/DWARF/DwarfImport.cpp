// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/DWARF/DwarfImport.h"

#include <utility>
#include <vector>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/Boot/ElfReader.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/Debugger/DWARF/DwarfReader.h"
#include "Core/PowerPC/PPCSymbolDB.h"

namespace Core::Debug
{
namespace
{
bool ApplyParseResult(const CPUThreadGuard& guard, PPCSymbolDB& symbol_db,
                      const Dwarf::ParseResult& parsed, const std::string& object_name)
{
  std::vector<u32> file_indices;
  file_indices.reserve(parsed.files.size());
  for (const std::string& file : parsed.files)
    file_indices.push_back(symbol_db.AddSourceFile(file));

  auto file_index_for = [&](const std::string& file) -> u32 {
    for (size_t i = 0; i < parsed.files.size(); ++i)
    {
      if (parsed.files[i] == file)
        return file_indices[i];
    }
    return symbol_db.AddSourceFile(file);
  };

  for (const Dwarf::Function& function : parsed.functions)
  {
    const u32 size = function.high_pc > function.low_pc ? function.high_pc - function.low_pc : 0;
    symbol_db.AddKnownSymbol(guard, function.low_pc, size, function.name,
                             function.compile_unit.empty() ? object_name : function.compile_unit,
                             Common::Symbol::Type::Function);
  }

  for (const Dwarf::LineEntry& line : parsed.lines)
  {
    symbol_db.AddLineEntry(line.address, file_index_for(line.file), line.line);
  }

  symbol_db.Index();
  return true;
}
}  // namespace

bool ImportDwarf(const CPUThreadGuard& guard, PPCSymbolDB& symbol_db,
                 std::span<const u8> debug_section, std::span<const u8> line_section,
                 const std::string& object_name)
{
  const std::optional<Dwarf::ParseResult> parsed = Dwarf::Parse(debug_section, line_section, true);
  if (!parsed)
    return false;

  NOTICE_LOG_FMT(SYMBOLS, "Imported DWARF: {} functions, {} line entries, {} source files",
                 parsed->functions.size(), parsed->lines.size(), parsed->files.size());
  return ApplyParseResult(guard, symbol_db, *parsed, object_name);
}

bool ImportDwarfFromElf(const CPUThreadGuard& guard, PPCSymbolDB& symbol_db,
                        const std::string& elf_path)
{
  ElfReader elf(elf_path);
  if (!elf.IsValid())
    return false;

  const SectionID debug_section = elf.GetSectionByName(".debug");
  const SectionID line_section = elf.GetSectionByName(".line");
  if (debug_section < 0)
  {
    WARN_LOG_FMT(SYMBOLS, "No .debug section in {}", elf_path);
    return false;
  }

  const u8* debug_data = elf.GetSectionDataPtr(debug_section);
  const u8* line_data = line_section >= 0 ? elf.GetSectionDataPtr(line_section) : nullptr;
  if (!debug_data)
    return false;

  const size_t debug_size = elf.GetSectionSize(debug_section);
  const size_t line_size = line_data ? elf.GetSectionSize(line_section) : 0;

  return ImportDwarf(guard, symbol_db, {debug_data, debug_size},
                     line_data ? std::span<const u8>{line_data, line_size} : std::span<const u8>{},
                     elf_path);
}

bool ImportConfiguredDwarfElf(const CPUThreadGuard& guard, PPCSymbolDB& symbol_db)
{
  const std::string& path = Config::Get(Config::MAIN_DEBUG_DWARF_ELF);
  if (path.empty())
    return false;

  if (!File::Exists(path))
  {
    WARN_LOG_FMT(SYMBOLS, "Configured DWARF ELF not found: {}", path);
    return false;
  }

  return ImportDwarfFromElf(guard, symbol_db, path);
}
}  // namespace Core::Debug
