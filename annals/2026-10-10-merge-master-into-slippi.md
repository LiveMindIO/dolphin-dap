# Merge master into the Slippi fork while retaining Slippi behavior

Date: 2026-10-10
Status: implemented
Scope: Integrate the current local `master` into `dolphin-dap-slippi` without regressing Slippi-specific product behavior.

## Git provenance

- Repository: Dolphin emulator fork; configured remotes are `origin` (`https://github.com/Jacoby6000/dolphin.git`), `slippi` (`https://github.com/project-slippi/dolphin.git`), and `upstream` (`https://github.com/dolphin-emu/dolphin.git`). The canonical remote for this Slippi branch has not been independently established.
- Recording branch and merge commit: `dolphin-dap-slippi` at `764647ad2ee8cb0820225c2a75fed8f2e10f43b4` (`Merge master into dolphin-dap-slippi`), authored and committed by Jacob Barber on 2026-10-10T08:30:25-05:00.
- Merge parents: Slippi branch tip `cdf609292572248dd57fdc9e30e15a37cd76341e` (`Fix Slippi pipe request generation ordering`) and local `master` `275143f7b086280420fbeb2f763b8b0b1029bb2d` (`Annals: record validated cross-platform release`).
- No tag points at the merge commit. Nearest describe at recording: `v1.0.1+upstream-f87e849-568-g764647ad2e`; the release tag itself points to `312bf66b19d8851e5f4631defca4c4bf1820bad9`.
- The branch was 452 commits ahead of `origin/dolphin-dap-slippi` at recording; no push was authorized or performed. Working tree was clean.
- Source: user-provided conversation checkpoint in this session plus the merge commit, parent history, diff, and local validation logs. No complete source transcript or response ID was supplied. Authoring agent: Annalist (GPT-5.6 Terra).

## Decision summary

Merge local `master` into `dolphin-dap-slippi`, accepting the upstream DAP, build, and cross-platform release work while explicitly retaining Slippi branding, Rust integration, Melee-only behavior and game-settings pruning, and Slippi pipe request ordering.

## Context and evidence

The fork needed the current `master` changes, including the validated cross-platform DAP release work recorded in [Publish validated cross-platform Dolphin DAP release](2026-10-09-publish-validated-cross-platform-dap-release.md), but it must remain a Slippi-focused distribution rather than becoming a generic Dolphin build.

The merge commit message and its two-parent result are primary evidence of the resolved policy. Its first-parent diff contains 535 changed files, so this record limits itself to consequential conflict resolutions rather than treating every imported upstream change as a new decision. The retained `BootManager.cpp` both preserves the Melee rejection and incorporates master's debug-ELF boot behavior. The merge also retains the Slippi game-settings deletions rather than restoring the non-Melee INIs introduced or modified by master.

The supplied checkpoint reports 74 conflicts. The saved merge-preview log independently shows modify/delete conflicts for Slippi-pruned game INIs, while the final tree has those non-Melee INIs deleted relative to master. The local configure and build logs show Qt 6.11.2 configuration and a Rust Release build of `slippi_rust_extensions`; the unit-test log records a successful aggregate test target, and the CI regression log records 26 passing checks.

## Options considered

- Accept master wholesale: rejected. That would restore non-Melee settings and risk losing Slippi branding, Rust extension integration, and fork-specific defaults and pipe ordering.
- Keep the Slippi branch without merging: rejected. It would omit master DAP, debugging, build, and release work.
- Merge master with targeted Slippi preservation: selected. This receives the upstream work while retaining the fork's intended product boundary.

## Chosen option and rationale

The chosen merge preserves the following consequential fork behavior while taking compatible master changes:

- Keep Slippi branding, Rust extension integration, the Melee-only boot restriction, Slippi graphics/default behavior, and pipe request ordering.
- Keep Slippi's removal of non-Melee game INIs, including master files involved in modify/delete conflicts.
- Retain master's debug-ELF boot integration, with the Slippi Melee restriction still enforced in `Source/Core/Core/BootManager.cpp`.
- Update the SDL submodule pointer to `fa2c02bb6e21974a89ea9824bc53c9932abe5f9c`, reported in the checkpoint as upstream SDL 3.4.16 and a descendant of Slippi's 3.4.0 pointer. The nested submodule objects were unavailable locally when this annal was written, so that ancestry/release claim remains checkpoint-sourced rather than independently verified.
- Adapt `Source/UnitTests/Core/SerialInterfaceTest.cpp` from the removed `ChangeDevice` API to `Config::GetInfoForSIDevice` and `Config::SetCurrent`.
- Raise vendored `semver` and `open-vcdiff` CMake minimum versions from 2.8-era requirements to 3.5 for CMake 4 compatibility.
- Adopt the merged Sys-resource lookup ordering: local `Sys`, configured user `Sys`, then installed `Sys` on non-Apple, non-Android hosts.

Confidence is high for the merge state and local Linux validation because they are directly represented by Git and logs. Confidence is medium for the full conflict count, SDL ancestry/version, and aggregate per-suite test count because those details come from the checkpoint and are not all reproduced in the retained logs.

## Consequences and follow-up

- The Slippi branch now includes master through `275143f7b086280420fbeb2f763b8b0b1029bb2d`, including its DAP release work and annals.
- Local validation completed in `/home/jbarber/.cache/dolphin-slippi-master-merge-build`: Qt and NoGUI builds, Rust Release build, aggregate unit-test target (reported by the checkpoint as 1,964 tests across 89 suites), 26 CI regression checks, and Qt/NoGUI `--version` startup checks. The aggregate test and CI logs directly show success; the exact 1,964/89 count and version-command output were not retained in the inspected excerpts.
- The Rust build emitted a pre-existing-looking `slippi-user` dead-code warning for `MatchStatus`; it did not fail the build.
- Cross-platform Slippi packaging CI was not run. Before publishing or releasing this branch, validate packaging on the supported Slippi targets.
- Reconsider this integration approach if a Slippi workflow, branding path, Melee restriction, pipe timing behavior, or Rust extension behavior regresses, or if the newer SDL pointer proves incompatible with Slippi.

## Relationship to prior records

This imports, but does not supersede, the master release decision in [Publish validated cross-platform Dolphin DAP release](2026-10-09-publish-validated-cross-platform-dap-release.md). That record describes publication from `master`; this one records the separate decision to integrate that work into the Slippi branch without publishing the branch.

## Open historical gaps

- No issue, pull request, or architecture record for this merge was supplied or found locally.
- The complete conflict-resolution transcript and the command output substantiating the exact 1,964-test/89-suite count and both version checks were unavailable.
- The configured `origin` remote is not sufficient evidence that it is the canonical remote for the Slippi branch.

## Subsequent developments (2026-10-19)

The branch was subsequently synchronized with `slippi/slippi` `9d13b39188` and then `origin/master` `ed0db5096e`. The ordered merge retained the product-boundary approach documented here; see [Sync Slippi upstream and personal-fork master](2026-10-19-sync-slippi-and-fork-master.md). This is a subsequent implementation, not a supersession or refutation of this record.
