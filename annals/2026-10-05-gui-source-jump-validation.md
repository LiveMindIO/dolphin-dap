# GUI source-line jump validation and DWARF mismatch warning

Date: 2026-10-05
Status: implemented
Scope: DolphinQt source debugger behavior for `ShowSource`/`SelectLine` navigation and disassembly fallback.

## Git provenance

- Repository: Dolphin emulator debugger branch, `feature/gui-source-debugger`
- Canonical remote: `origin` (`https://github.com/Jacoby6000/dolphin.git`, redirects to `https://github.com/LiveMindIO/dolphin-dap.git`)
- Branch state: clean, tracking `origin/feature/gui-source-debugger`
- Recording HEAD: `129ff7b29092e44a7ba3dba355b889ba4b565d00` (`129ff7b290`), nearest describe `5.0-25542-g129ff7b290`
- Decision commit: `487a26b5c01d4aced68e8b8ceffd4fc20bfccf97` (`487a26b5c0`), subject `DolphinQt: validate source jumps and warn on DWARF mismatches`
- Source session: `ses_f07c8f576ffergQyBAxj2eibaJ`, response `msg_10d2f60e6001HvvlgYg0qnH4KP`

## Decision

Every GUI source-line jump requested by navigation, breakpoint location selection, or cursor/gutter behavior must be validated against the loaded source document. When DWARF references a line outside the loaded file, the source view clears stale content, emits `SourceLineMismatch`, falls back to disassembly, and shows a non-modal warning naming the path, requested line, loaded line count, and advice to rebuild the ELF. Missing files and unmapped addresses fall back without a mismatch warning.

## Context and problem

`SourceViewWidget::ShowSource` previously validated line bounds only on cache misses. A cached source file could therefore accept an out-of-range DWARF line, retain the wrong selected line, and appear to succeed. Hash or modification-time cache invalidation was proposed and rejected because the reproducer did not modify the file and file freshness cannot validate DWARF line validity.

## Options considered

- Validate every requested line at jump time, cached or fresh. Chosen because correctness is independent of cache state and file freshness.
- Use file hash or modification time to invalidate cached source. Rejected; does not detect invalid DWARF line mappings and cannot guarantee source matches the compiled ELF.
- Treat any disassembly fallback as a mismatch. Rejected; missing files and unmapped addresses are normal fallback cases, not DWARF/source line mismatches.

## Rationale

The requirement separates source-name resolution from jump validity. A line jump is meaningful only if the loaded document contains that line. The warning gives actionable guidance when DWARF and source drift.

## Consequences

- `SourceViewWidget::SelectLine` now returns success/failure, emits `SourceLineMismatch`, clears stale state, and prevents stale gutter breakpoint toggles after invalid jumps.
- `CodeWidget` displays `source_mismatch_warning`, selects the disassembly tab on mismatch, and clears the warning on valid navigation or shutdown.
- Caret/gutter navigation cannot wipe the view without surfacing the mismatch; Bugbot's gutter-clearing comment describes the requested fallback rather than an independent regression.

## Implementation evidence

- Temporary offscreen executable drove real `SourceViewWidget`/`CodeWidget` objects: 15 checks passed for cached and fresh misses, recovery, disassembly selection, no false warnings for missing files/unmapped addresses, and warning clearing.
- Builds passed for `tests`, `dolphin-emu`, `dolphin-emu-nogui`.
- Test suite result at commit context: 1,875 tests passed with four DAP tests excluded; later full-suite state in this session reached 1,894 with no exclusions.

## Follow-up obligations

- If source-cache invalidation is reconsidered, treat it as a separate file-freshness feature, not the fix for DWARF line validity.
- Keep missing-file and unmapped-address fallback distinct from DWARF mismatch fallback.

## Reconsideration triggers

- If line validation proves too costly for very large files, bound the check by cached document block count rather than reparsing.
- If a reliable source/ELF identity API emerges, use it to strengthen the warning, but still validate requested bounds.

## Open questions

- None durable. The temporary reproducer remains under `/tmp/opencode/pr5-gui-review.cpp` and is not a committed test.
