# Entrypoints sidecar format (`entrypoints.json`)

Normalized debug metadata for **function entrypoint-only** source mapping. Read this
when implementing the Melee/objdiff producer, Dolphin importer, or DAP UX for
sparse line tables.

**Implementation status:** Dolphin's importer and sidecar discovery are implemented.
The consumer sections below describe the current code, not a proposed feature.
The Melee/objdiff producer pipeline is an integration sketch; the producer is not
shipped in this Dolphin repository. Keep genuine future changes in
[Future extensions](#future-extensions-not-v1).

## Problem

Reconstructed source units compiled with MWCC `-sym on` can provide
full DWARF 1.1 in `main.elf` (see
[`.ai-doc-reference/dwarf-1.1-format.md`](dwarf-1.1-format.md)).

Some matching-build configurations retain retail objects without DWARF rather
than executing reconstructed source for every unit. For these builds, objdiff
can identify which **individual functions** match between
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

Dolphin resolution order (implemented by `ImportConfiguredEntrypoints`):

1. `--debug-entrypoints <path>` / `Dolphin.Debug.Entrypoints`
2. Else `entrypoints.json` beside the ELF passed to the importer, or beside
   `Dolphin.Debug.ELFFile` when no ELF path was passed, if that sibling file exists
3. Else entrypoints import is skipped (DWARF-only)

A configured sidecar takes precedence even if the file is missing; Dolphin logs
the missing file rather than falling back to sibling discovery. `--debug-entrypoints`
sets the same `Dolphin.Debug.Entrypoints` configuration key. The `dwarf_elf` field
inside the JSON is provenance, not a path Dolphin uses to select or load an ELF.

## Top-level schema (version 1)

```json
{
  "schema_version": 1,
  "precision": "entrypoint",
  "game_id": "GALE01",
  "dwarf_elf": "main.elf",
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

### Required fields in the producer schema

| Field | Type | Description |
|-------|------|-------------|
| `schema_version` | integer | Must be `1` for this document |
| `precision` | string | Must be `"entrypoint"` (future: `"line"`, etc.) |
| `game_id` | string | DOL/ISO game ID (e.g. `GALE01`); provenance for producer-side validation |
| `dwarf_elf` | string | Path identifying the ELF whose layout these addresses use; producers can use a path relative to the JSON directory |
| `functions` | array | Function records (see below) |

Dolphin does not run the JSON Schema validator. It requires `schema_version: 1`,
`precision: "entrypoint"`, and a `functions` array, but does not currently read or
validate `game_id` or `dwarf_elf`. It does not verify that these addresses match
the running executable. Producers and users must establish that match.

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
  "address": 2148970376,
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

- **On disk:** JSON number (decimal integer). Example: `2148970376` = `0x8016AF88`.
- Hex strings are not supported by the current importer; emit numeric addresses.
- Producers should verify 4-byte PPC alignment and that the address/size lie
  within the intended executable. The current importer does not enforce alignment
  or compare entries with an ELF symbol table.

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

## Producer integration sketch (Melee/objdiff)

This describes a producer that emits the schema above; it is not a runnable setup
recipe or a claim that a particular upstream Melee checkout includes this tool.
Use the build/report commands supported by your checkout. Emit entries only for
code whose linked addresses match the executable you will run.

```
configure.py --debug
    → build a debug ELF and matching objdiff report
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
| `units[].metadata.complete` | skip an already-complete unit when true and `include_complete_units` is false |
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

After loading available ELF/DWARF information, the importer processes records in
JSON array order (it does not sort or deduplicate them):

```
ImportEntrypointsFromJson(path)
    validate schema_version, precision, and functions array
    for each record in functions:
        if name/file/address/size missing or invalid, or size == 0: continue
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
| Symbol, no dense line info | Update symbol name/object name/size; add a line entry if supplied |
| Line table covers `[address, address+size)` with >1 distinct lines | Skip (full DWARF wins) |
| Single line entry at same address, no dense range | Replace that row if the record supplies `line` |

An identical re-import restores the same mapping, but changed records can replace
sparse rows. Duplicate addresses are not rejected; later records can overwrite
earlier sparse mappings. Deduplicate in the producer as described above.

### `precision: entrypoint` semantics in DAP

Existing `PPCSymbolDB` behavior with sparse entries:

| API | Behavior |
|-----|----------|
| `GetSourceLine(addr)` | Nearest preceding row, restricted to the same function when symbol boundaries are known; a sparse row maps that function to its definition line |
| `GetLineAddress(file, line)` | Exact line match, else the smallest recorded line greater than the request; no later row means no address. Interior lines do not generally bind to this function's entry |
| `GetBreakpointLocations` | Only lines with explicit entries |
| Stack frames | File + definition line when inside mapped function range |

UI/docs should treat this as **function-granular**, not statement-granular.

## Validation (recommended)

Producer:

- [ ] Every `address` appears in `dwarf_elf` symtab as `FUNC` with matching `name` (warn on mismatch)
- [ ] `size` matches symtab st_size when present
- [ ] `file` exists relative to decomp repo root
- [ ] `schema_version` and `precision` recognized

Current consumer checks:

- Rejects malformed JSON, a non-object root, versions other than `1`, precision
  other than `"entrypoint"`, and a missing/non-array `functions` field.
- Skips non-object records, missing/invalid name/file/address/size fields, and
  zero-size functions.
- Skips functions whose range already contains more than one distinct DWARF line.
- Logs imported and dense-overlap counts, but does not count invalid records
  separately. No imported entries returns `false`, including an all-dense-overlap
  import; that does not mean existing DWARF was removed.

ELF-layout validation, game-ID checks, alignment checks, and source-file existence
checks remain producer/user responsibilities, not guarantees of the importer.

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

## Current consumer files and producer conventions

| Status | Path |
|--------|------|
| Producer integration sketch, not shipped here | Melee `tools/entrypoints.py` |
| Suggested generated-file location | Melee `build/GALE01/entrypoints.json` |
| Implemented importer | [`EntrypointsImport.cpp`](../Source/Core/Core/Debugger/Entrypoints/EntrypointsImport.cpp) and [header](../Source/Core/Core/Debugger/Entrypoints/EntrypointsImport.h) |
| Existing consumer tests | [`EntrypointsImportTest.cpp`](../Source/UnitTests/Core/Debugger/Entrypoints/EntrypointsImportTest.cpp) |

## JSON Schema

Machine-readable schema: [`.ai-doc-reference/entrypoints.schema.json`](entrypoints.schema.json).
