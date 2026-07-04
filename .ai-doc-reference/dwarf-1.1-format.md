# DWARF 1.1 in Dolphin (MWCC / CodeWarrior)

Reference for the native DWARF 1.1 importer in
`Source/Core/Core/Debugger/DWARF/`. Read this when extending the parser,
adding DWARF 2+, or validating compiler output.

## Scope (current)

- **In:** `.debug` DIE walk (compile units + subroutines), `.line` statement
  tables (address ↔ file:line), import into `PPCSymbolDB`.
- **Out (later):** types, locals/scopes, `.debug_aranges`, DWARF 2+
  (`.debug_info` / `.debug_line` / abbrev tables).

## Sections

| Section | Role |
|---------|------|
| `.debug` | DWARF 1.1 debugging information entries (DIEs) |
| `.line` | Line number tables referenced by `AT_stmt_list` on compile units |

MWCC/CodeWarrior ELFs use these names (not DWARF 2's `.debug_info` /
`.debug_line`). GC ELFs are **big-endian**.

## DIE layout (`.debug`)

Each entry:

1. `u32 length` — total byte size of the entry **including** the length field.
2. `u16 tag` — e.g. `TAG_compile_unit` (0x0011), `TAG_global_subroutine`
   (0x0006), `TAG_subroutine` (0x0014).
3. Attributes until end of entry: `u16 attr` (name in high bits, **form** in
   low nibble), then value.

Important attributes:

| Attribute | Encoding | Use |
|-----------|----------|-----|
| `AT_name` | `0x0030 \| FORM_STRING` | Function / CU name |
| `AT_low_pc` | `0x0110 \| FORM_ADDR` | Start address |
| `AT_high_pc` | `0x0120 \| FORM_ADDR` | End address (exclusive) |
| `AT_stmt_list` | `0x0100 \| FORM_DATA4` | Byte offset into `.line` |
| `AT_sibling` | `0x0010 \| FORM_REF` | Offset of next sibling DIE |

Form values (`attr & 0xF`): `FORM_ADDR=1`, `FORM_REF=2`, `FORM_BLOCK2=3`,
`FORM_BLOCK4=4`, `FORM_DATA2=5`, `FORM_DATA4=6`, `FORM_DATA8=7`,
`FORM_STRING=8`.

Ownership: a compile unit's children immediately follow the CU DIE; sibling
chains use `AT_sibling` (see binutils `bfd/dwarf1.c`).

## Line table (`.line`)

Per compile unit at `AT_stmt_list` offset:

1. `u32 length` — table size from this field (same inclusive rule as DIEs).
2. `u32 base` — address of first instruction in the CU (added to each delta).
3. Repeated **10-byte** entries until `line == 0`:
   - `u32 line`
   - `u16 column` (`0xffff` = `SOURCE_NO_POS`, whole line)
   - `u32 address_delta` (added to base)

## Dolphin integration

```
.debug + .line  →  DwarfReader::Parse  →  Core::Debug::ImportDwarf  →  PPCSymbolDB
```

- **Boot:** `ElfReader::LoadSymbols` calls `ImportDwarf` when `.debug` exists.
- **Sidecar:** `Main.Debug.DwarfElf` / `--debug-elf` → `ImportConfiguredDwarfElf` in
  `SConfig::OnTitleDirectlyBooted` (ISO, DOL, WAD, etc.).
- **GUI:** Symbols → Load DWARF/Debug Info… → `ImportDwarfFromElf`.
- **DAP:** `GetStackTrace` / `loadedSources` / `source` / `breakpointLocations`
  read `PPCSymbolDB` line tables when present; otherwise disassembly fallback.

Line info lives on `PPCSymbolDB` (not `Common::Symbol`) so the generic symbol
base and DSP maps stay unchanged.

## Test fixture

`Source/UnitTests/Core/Debugger/DWARF/DwarfTestFixture.h` — hand-crafted
`.debug`/`.line` bytes validated against the spec and binutils parsing rules.
Addresses use MEM1 (`0x00003100`) so unit tests can run `PPCAnalyst` safely.

To regenerate from a real MWCC object (when `wibo`/Wine + MWCC are available):

```bash
mwcceppc.exe -sym on -g … -c file.c -o file.o
dtk dwarf dump file.o   # cross-check against Dolphin parser
```

## Future: DWARF 2+

Add a second front-end that feeds the same `PPCSymbolDB` line model. Do not
extend the DWARF 1.1 walker for `.debug_abbrev` / line opcodes — that is a
separate parser.
