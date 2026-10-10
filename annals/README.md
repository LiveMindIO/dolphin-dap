# Annals index

Reverse chronological. Recording branch: `dolphin-dap-slippi`; recording HEAD: `764647ad2ee8cb0820225c2a75fed8f2e10f43b4`; nearest describe: `v1.0.1+upstream-f87e849-568-g764647ad2e`.

- [2026-10-10: Merge master into the Slippi fork while retaining Slippi behavior](2026-10-10-merge-master-into-slippi.md) - implemented - Slippi/master integration and fork behavior preservation - `764647ad2ee8cb0820225c2a75fed8f2e10f43b4`.
- [2026-10-05: Source-step interruption guards and temporary step-over breakpoint classification](2026-10-05-source-step-interruption-and-temporary-step-over-classification.md) — proposed — Source-row destination predicates, temporary step-over classification — open findings at `8297b5057d`.
- [2026-10-05: Step-out return-destination breakpoint evaluation (implemented)](2026-10-05-stepout-return-destination-fix.md) — implemented — DAP/Qt step-out destination predicates — `8297b5057d`.
- [2026-10-05: Step-out skips evaluation of the breakpoint at its return destination](2026-10-05-stepout-return-destination-breakpoint-gap.md) — implemented — DAP/Qt step-out destination predicates — finding at `88a5ddc3b9`; fixed in `8297b5057d`.
- [2026-10-05: DAP `writeMemory`/`evaluate` must invalidate stale writable expansion handles](2026-10-05-dap-write-memory-evaluate-stale-handle-invalidation.md) — implemented — DAP variable expansion invalidation — `88a5ddc3b9`.
- [2026-10-05: Step stop causes and instruction-step destination breakpoint evaluation](2026-10-05-step-stop-causes-and-instruction-destination-breakpoints.md) — implemented — Debugger/DAP step classification — final `129ff7b290`, intermediate `6ba0dadb23` superseded; step-out gap split off at `88a5ddc3b9`.
- [2026-10-05: DAP data breakpoint hit ID correlation by exact watchpoint range](2026-10-05-dap-data-breakpoint-hit-id-correlation.md) — implemented — DAP data breakpoints — `ec07612613`.
- [2026-10-05: DAP bootless test fixture CPU-thread role for async step workers](2026-10-05-dap-test-fixture-cpu-thread-role.md) — implemented — DAP test fixture/exclusions — `bf4ac46572`.
- [2026-10-05: Step-out timeout publishes owned terminal stops](2026-10-05-step-out-timeout-terminal-stops.md) — implemented — DAP/DolphinQt step-out timeout — DAP `5f94cb4734`, Qt `7b088a7cd2`.
- [2026-10-05: DAP variable expansion handle invalidation after value edits](2026-10-05-dap-variable-handle-invalidation.md) — implemented — DAP variables — `b23715c6ec`.
- [2026-10-05: GUI source-line jump validation and DWARF mismatch warning](2026-10-05-gui-source-jump-validation.md) — implemented — DolphinQt source debugger — `487a26b5c0`.
