// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/Entrypoints/EntrypointsImport.h"

#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <picojson.h>

#include "Common/FileUtil.h"
#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/PowerPC/PPCSymbolDB.h"

namespace Core::Debug
{
namespace
{
std::optional<std::string> ResolveEntrypointsPath(const std::string& configured_path,
                                                   const std::string& dwarf_elf_path)
{
  if (!configured_path.empty())
    return configured_path;

  if (dwarf_elf_path.empty())
    return std::nullopt;

  std::string directory;
  std::string filename;
  SplitPath(dwarf_elf_path, &directory, &filename, nullptr);
  const std::string sibling =
      directory.empty() ? "entrypoints.json" : directory + "/entrypoints.json";
  if (File::Exists(sibling))
    return sibling;

  return std::nullopt;
}
}  // namespace

bool ImportEntrypointsFromJson(const CPUThreadGuard& guard, PPCSymbolDB& symbol_db,
                               const std::string& json_path)
{
  picojson::value root;
  std::string error;
  if (!JsonFromFile(json_path, &root, &error))
  {
    WARN_LOG_FMT(SYMBOLS, "Failed to read entrypoints sidecar {}: {}", json_path, error);
    return false;
  }

  if (!root.is<picojson::object>())
  {
    WARN_LOG_FMT(SYMBOLS, "Entrypoints sidecar {} is not a JSON object", json_path);
    return false;
  }

  const picojson::object& object = root.get<picojson::object>();

  const std::optional<u32> schema_version = ReadNumericFromJson<u32>(object, "schema_version");
  if (!schema_version || *schema_version != 1)
  {
    WARN_LOG_FMT(SYMBOLS, "Unsupported entrypoints schema_version in {}", json_path);
    return false;
  }

  const std::optional<std::string> precision = ReadStringFromJson(object, "precision");
  if (!precision || *precision != "entrypoint")
  {
    WARN_LOG_FMT(SYMBOLS, "Unsupported entrypoints precision in {}", json_path);
    return false;
  }

  const auto functions_it = object.find("functions");
  if (functions_it == object.end() || !functions_it->second.is<picojson::array>())
  {
    WARN_LOG_FMT(SYMBOLS, "Entrypoints sidecar {} missing functions array", json_path);
    return false;
  }

  size_t imported = 0;
  size_t skipped_dense = 0;

  for (const picojson::value& entry : functions_it->second.get<picojson::array>())
  {
    if (!entry.is<picojson::object>())
      continue;

    const picojson::object& fn = entry.get<picojson::object>();
    const std::optional<std::string> name = ReadStringFromJson(fn, "name");
    const std::optional<std::string> file = ReadStringFromJson(fn, "file");
    const std::optional<u32> address = ReadNumericFromJson<u32>(fn, "address");
    const std::optional<u32> size = ReadNumericFromJson<u32>(fn, "size");
    if (!name || !file || !address || !size || *size == 0)
      continue;

    if (symbol_db.HasDenseLineInfoInRange(*address, *size))
    {
      ++skipped_dense;
      continue;
    }

    symbol_db.AddKnownSymbol(guard, *address, *size, *name, *file, Common::Symbol::Type::Function);

    if (const std::optional<u32> line = ReadNumericFromJson<u32>(fn, "line"))
    {
      const u32 file_index = symbol_db.AddSourceFile(*file);
      symbol_db.AddLineEntry(*address, file_index, *line);
    }

    ++imported;
  }

  if (imported == 0 && skipped_dense == 0)
  {
    WARN_LOG_FMT(SYMBOLS, "No entrypoints imported from {}", json_path);
    return false;
  }

  symbol_db.Index();
  NOTICE_LOG_FMT(SYMBOLS,
                 "Imported {} entrypoint(s) from {} ({} skipped; dense DWARF already present)",
                 imported, json_path, skipped_dense);
  return imported > 0;
}

bool ImportConfiguredEntrypoints(const CPUThreadGuard& guard, PPCSymbolDB& symbol_db)
{
  const std::string& dwarf_elf = Config::Get(Config::MAIN_DEBUG_DWARF_ELF);
  const std::string& configured = Config::Get(Config::MAIN_DEBUG_ENTRYPOINTS);
  const std::optional<std::string> path = ResolveEntrypointsPath(configured, dwarf_elf);
  if (!path)
    return false;

  if (!File::Exists(*path))
  {
    WARN_LOG_FMT(SYMBOLS, "Configured entrypoints sidecar not found: {}", *path);
    return false;
  }

  return ImportEntrypointsFromJson(guard, symbol_db, *path);
}
}  // namespace Core::Debug
