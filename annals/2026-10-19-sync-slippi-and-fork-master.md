# Sync Slippi upstream and personal-fork master

Date: 2026-10-19
Status: implemented
Scope: Update `dolphin-dap-slippi` first from the Slippi upstream default branch, then from the personal-fork `master`, while retaining Slippi and existing debugger behavior.

## Git provenance

- Repository: Dolphin emulator fork. Configured remotes: `origin` (`https://github.com/Jacoby6000/dolphin.git`), `slippi` (`https://github.com/project-slippi/dolphin.git`), and `upstream` (`https://github.com/dolphin-emu/dolphin.git`). The canonical remote for this branch remains unverified.
- Recording branch and HEAD: `dolphin-dap-slippi` at `322ae530c977d762ce630d74c11ad548b46d4dc9` (`Merge personal fork master into dolphin-dap-slippi`). No tag points at HEAD; nearest tag is `v1.0.3+upstream-f87e849-593-g322ae530c9`.
- Upstream merge: `40dc9d9fc257f21759b1a635414d98d72c0a102c`, merging prior branch tip `d61ddcda1c` with `slippi/slippi` `9d13b39188`.
- Personal-fork merge: `322ae530c977d762ce630d74c11ad548b46d4dc9`, merging `40dc9d9fc2` with `origin/master` `ed0db5096e`.
- Working tree at recording was clean. The branch was 482 commits ahead of `origin/dolphin-dap-slippi`; no push was performed.
- Source: completion-hook transcript for session `ses_ea8be4209ffeh55fTTkBSWIQFe`, response `msg_1574499620013z8c7fB0qlrVdg`, plus the merge commits, ancestry checks, and retained local test logs. Authoring agent: Annalist (GPT-5.6 Terra).

## Decision summary

Merge `slippi/slippi` before `origin/master`. Preserve the branch's hardened ELF/DWARF and DirectIO GCZ implementations, Slippi playback and product defaults, and existing Qt source-debugger and launch-option work; incorporate upstream Slippi diagnostics, desync-recovery fixes, Azure signing, forced analytics disablement, and the pinned AppImage runtime.

## Context and evidence

The prior integration record, [Merge master into the Slippi fork while retaining Slippi behavior](2026-10-10-merge-master-into-slippi.md), brought master through `275143f7b0` into the Slippi branch. Subsequent Slippi updates and personal-fork master changes required another ordered synchronization.

The first merge commit states that it retains hardened ELF/DWARF and DirectIO GCZ work while adding upstream ELF diagnostics, desync recovery, and Azure signing. Its diff modifies the Slippi workflow, ELF diagnostics, and desync-recovery paths. The second merge commit states that it retains Slippi playback and product defaults while taking forced analytics disablement and the pinned AppImage runtime. Its diff includes `CMakeLists.txt`, `DolphinAnalytics.cpp`, and CI tests.

The transcript initially asserted that DAP launch option `--dap` would be present in binary help. Direct inspection refuted that assertion: `dolphin-emu --help` lists `--debugger`, `--debug-elf`, `--debug-entrypoints`, and `--config`, but not `--dap`. The final validation correctly checked the supported options instead.

## Options considered

- Merge Slippi updates and fork master in the requested order: selected. This permits conflict resolution against the intended upstream base before applying personal-fork policy changes.
- Accept Slippi's older ELF and GCZ implementations wholesale: rejected during conflict resolution. The branch retained its hardened ELF/DWARF and DirectIO GCZ implementations, adding compatible diagnostic logging instead.
- Retain the prior opt-in analytics option: rejected. Fork builds now force analytics off, including existing build directories and packaging overrides, while retaining the Slippi playback option.

## Chosen option and rationale

The first merge retains existing debugger-relevant ELF/DWARF behavior and DirectIO GCZ APIs while incorporating upstream logging, desync-recovery corrections, and Azure Artifact Signing. The second preserves `SLIPPI_PLAYBACK` and Slippi defaults but adopts the personal fork's hard analytics disablement and reproducible AppImage runtime pin. This maintains the branch's Slippi-focused product boundary and previously integrated Qt source-debugger and command-line debugging capabilities.

Confidence is high for commit ancestry, conflict outcomes, and local validation because they are represented by the commits, working tree, and retained logs. Confidence is medium that all intended gameplay behavior is preserved; no live gameplay or cross-platform packaging run was performed.

## Consequences and follow-up

- `dolphin-dap-slippi` now contains both `slippi/slippi` `9d13b39188` and `origin/master` `ed0db5096e`.
- The retained Linux build directory configured Qt, NoGUI, tests, and the Rust extension. Builds of `dolphin-emu`, `dolphin-emu-nogui`, and `tests` succeeded.
- Local unit tests passed: 1,964 tests in 89 suites. Python CI regression tests passed: 29 tests.
- Both binaries reported version `4.0.0-mainline-beta.19`; help output confirmed `--debugger`, `--debug-elf`, `--debug-entrypoints`, and `--config`.
- Cross-platform packaging and live gameplay were not tested. Validate supported Slippi packaging targets and an actual gameplay session before release or publication.

## Reconsideration triggers and open questions

- Reconsider the merge policy if Slippi desync recovery, playback, analytics suppression, DAP/source debugging, or ELF loading regresses.
- The supplied transcript is complete through the named response but does not include an issue, pull request, or external review record for this integration.
- The canonical remote and any remote publication status beyond the local no-push assertion were not independently established.

## Relationship to prior records

This is a subsequent synchronization after [Merge master into the Slippi fork while retaining Slippi behavior](2026-10-10-merge-master-into-slippi.md); it does not supersede that earlier integration decision.
