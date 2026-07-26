# Entrypoints sidecar format (`entrypoints.json`)

Normalized debug metadata for **function entrypoint-only** source mapping. Read this
when implementing the Melee/objdiff producer, Dolphin importer, or DAP UX for
sparse line tables.

## Problem

Matching decomp units link freshly compiled `src/*.o` with MWCC `-sym on`, so
`main.elf` carries full DWARF 1.1 (see
[`.ai-doc-reference/dwarf-1.1-format.md`](dwarf-1.1-format.md)).

NonMatching units intentionally link retail `obj/*.o` (no DWARF) to preserve byte
matching. objdiff still knows which **individual functions** match between
`target_path` (retail) and `base_path` (decomp `src/*.o`), including absolute
linked addresses via `metadata.virtual_address`.

This sidecar bridges that gap without synthesizing fake per-instruction line
tables or patching ELF sections.

## Design principles

| Principle | Rationale |
|-----------|-----------|
| JSON sidecar, not a custom ELF section | Diffable, schema-versioned, no `objcopy`/linker work; Dolphin needs a reader either way |
| Stable contract, objdiff as adapter input | objdiff `report.json` evolves; Melee tool normalizes into this format |
| `precision: entrypoint` is explicit | Consumers must not treat mappings as full DWARF |
| DWARF wins on conflict | Importer skips entrypoints when dense line info already covers the address |
| Addresses match sidecar ELF layout | Same contract as `--debug-elf`; runtime RAM must match linked layout |

## File location and discovery

Convention (Melee):

```
build/GALE01/main.elf
build/GALE01/entrypoints.json   # sibling of debug ELF
```

Dolphin resolution order (planned):

1. `--debug-entrypoints <path>` / `Dolphin.Debug.Entrypoints`
2. Else `<dirname(debug_elf)>/entrypoints.json` if present
3. Else entrypoints import is skipped (DWARF-only)

## Top-level schema (version 1)

```json
{
  "schema_version": 1,
  "precision": "entrypoint",
  "game_id": "GALE01",
  "dwarf_elf": "build/GALE01/main.elf",
  "generated_at": "2026-07-04T20:50:00Z",
  "generated_from": {
    "tool": "entrypoints.py",
    "tool_version": "0.1.0",
    "report": "build/GALE01/report.json",
    "objdiff_version": "3.6.1"
  },
  "match_threshold": 100.0,
  "include_complete_units": false,
  "units": [],
  "functions": []
}
```

### Required fields

| Field | Type | Description |
|-------|------|-------------|
| `schema_version` | integer | Must be `1` for this document |
| `precision` | string | Must be `"entrypoint"` (future: `"line"`, etc.) |
| `game_id` | string | DOL/ISO game ID (e.g. `GALE01`) for validation/logging |
| `dwarf_elf` | string | Path to the sidecar ELF whose layout these addresses use (relative paths resolved from the JSON file's directory) |
| `functions` | array | Function records (see below) |

### Optional metadata

| Field | Type | Description |
|-------|------|-------------|
| `generated_at` | ISO 8601 string | Producer timestamp |
| `generated_from` | object | Provenance for debugging stale sidecars |
| `match_threshold` | number | Minimum `fuzzy_match_percent` used when filtering objdiff functions |
| `include_complete_units` | boolean | If false (default), omit units already linked with full DWARF |
| `units` | array | Optional TU summaries (progress reporting); not required for import |

## Function record

Each element of `functions`:

```json
{
  "name": "GetMatchTimer",
  "address": 2158970376,
  "size": 124,
  "file": "src/melee/gm/gm_16AE.c",
  "line": 117,
  "unit": "main/melee/gm/gm_16AE",
  "match_percent": 100.0
}
```

### Required

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Symbol name (matches ELF symtab / decomp source) |
| `address` | integer | **Absolute** linked VMA (`0x80000000`…`0x817FFFFF` for GameCube) |
| `size` | integer | Function size in bytes (from objdiff / symtab) |
| `file` | string | Repo-relative source path (`metadata.source_path` from objdiff unit) |

### Optional

| Field | Type | Description |
|-------|------|-------------|
| `line` | integer | 1-based source line of the function **definition**; omit if lookup failed |
| `unit` | string | objdiff unit name (`main/melee/gm/gm_16AE`) |
| `match_percent` | number | objdiff `fuzzy_match_percent` at generation time |
| `object_offset` | integer | Object-relative `.text` offset (objdiff `address`); audit only |

### Address encoding

- **On disk:** JSON number (decimal integer). Example: `2158970760` = `0x8016AF88`.
- **Not used in v1:** hex strings, `"0x…"` strings (reject or normalize in importer).
- Importer must verify `address` is 4-byte aligned for PPC function entrypoints.

### Duplicate policy (producer)

- One record per `(address)`; if objdiff emits duplicates, keep highest
  `match_percent`, then longest `size`.
- Same name at different addresses (inlined clones, thunks): keep both; disambiguate
  by address in the importer, not by renaming.

## Unit summary record (optional)

```json
{
  "name": "main/melee/gm/gm_16AE",
  "file": "src/melee/gm/gm_16AE.c",
  "complete": false,
  "function_count": 102
}
```

Informational only; Dolphin import may ignore `units`.

## Producer pipeline (Melee)

```
configure.py --debug
    → ninja (all_source + main.elf)
    → objdiff-cli report generate → build/GALE01/report.json

entrypoints.py
    → read report.json
    → filter units (skip complete unless --include-complete-units)
    → filter functions (fuzzy_match_percent >= threshold)
    → enrich line: regex or tree-sitter on file + name
    → optional: validate address against main.elf symtab
    → write build/GALE01/entrypoints.json
```

### objdiff field mapping

| objdiff (`report.json`) | entrypoints.json |
|-------------------------|------------------|
| `units[].metadata.source_path` | `file` (on each function) |
| `units[].metadata.complete` | skip unit when false and `include_complete_units` is false |
| `units[].name` | `unit` |
| `functions[].name` | `name` |
| `functions[].size` (string) | `size` (integer) |
| `functions[].metadata.virtual_address` (string decimal) | `address` (integer) |
| `functions[].address` (object offset, string) | `object_offset` (optional) |
| `functions[].fuzzy_match_percent` | `match_percent`; filter threshold |

Example objdiff function (from Melee `gm_16AE`):

```json
{
  "name": "GetMatchTimer",
  "size": "124",
  "fuzzy_match_percent": 100.0,
  "address": "336",
  "metadata": { "virtual_address": "2148970376" }
}
```

→ `address: 2148970376`, `line: 117` (from source scan).

## Consumer pipeline (Dolphin)

After `ImportDwarfFromElf` (sidecar ELF):

```
ImportEntrypointsFromJson(path)
    for each function in functions (sorted by address):
        if HasDenseLineInfo(address, size): continue
        AddKnownSymbol(address, size, name, file)
        if line present:
            AddLineEntry(address, AddSourceFile(file), line)
    Index()
```

### Merge rules

| Existing state | Action |
|----------------|--------|
| No symbol at address | Add symbol + optional line entry |
| Symbol, no line entry | Add line entry only |
| Line table covers `[address, address+size)` with >1 distinct lines | Skip (full DWARF wins) |
| Single line entry at same address | Keep existing (idempotent re-import) |

### `precision: entrypoint` semantics in DAP

Existing `PPCSymbolDB` behavior with sparse entries:

| API | Behavior |
|-----|----------|
| `GetSourceLine(addr)` | Nearest preceding line entry → whole function maps to definition line |
| `GetLineAddress(file, line)` | Exact line match, else last entry with `line <= requested` → breakpoints on signature lines work; interior lines may bind to entry |
| `GetBreakpointLocations` | Only lines with explicit entries |
| Stack frames | File + definition line when inside mapped function range |

UI/docs should treat this as **function-granular**, not statement-granular.

## Validation (recommended)

Producer:

- [ ] Every `address` appears in `dwarf_elf` symtab as `FUNC` with matching `name` (warn on mismatch)
- [ ] `size` matches symtab st_size when present
- [ ] `file` exists relative to decomp repo root
- [ ] `schema_version` and `precision` recognized

Consumer:

- [ ] Reject unknown `schema_version`
- [ ] Reject `precision` other than `"entrypoint"` until supported
- [ ] Log count: imported / skipped (DWARF overlap) / skipped (invalid)

## Versioning

Increment `schema_version` when:

- Removing or renaming required fields
- Changing address semantics
- Changing merge rules incompatibly

Add optional fields without a version bump. Importers must ignore unknown fields.

## Future extensions (not v1)

| Extension | Notes |
|-----------|-------|
| `precision: "line"` | Full line table in sidecar (unlikely; use DWARF) |
| `schema_version: 2` with hex address strings | Only if decimal integers become awkward |
| Direct `report.json` import in Dolphin | Prefer keeping objdiff as Melee-only adapter |
| Checksum of `dwarf_elf` | Detect stale sidecar after relink |

## Related files (planned)

| Repo | Path |
|------|------|
| Melee | `tools/entrypoints.py` |
| Melee | `build/GALE01/entrypoints.json` (generated) |
| Dolphin | `Source/Core/Core/Debugger/Entrypoints/EntrypointsImport.{h,cpp}` |
| Dolphin | `Source/UnitTests/Core/Debugger/Entrypoints/EntrypointsImportTest.cpp` |

## JSON Schema

Machine-readable schema: [`.ai-doc-reference/entrypoints.schema.json`](entrypoints.schema.json).
