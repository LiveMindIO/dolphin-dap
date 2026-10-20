# Publish validated cross-platform Dolphin DAP release

Date: 2026-10-09
Status: implemented
Scope: Publish a complete, checksum-verified cross-platform release only after the unified CI matrix passes.

## Git provenance

- Repository: Dolphin emulator; canonical release remote: `https://github.com/LiveMindIO/dolphin-dap.git` (configured `origin`: `https://github.com/Jacoby6000/dolphin.git`); branch: `master`.
- Release commit and recording HEAD: `312bf66b19d8851e5f4631defca4c4bf1820bad9` (`CI: resolve Homebrew libraries when fixing macOS release bundles`).
- Release tag: annotated `v1.0.1+upstream-f87e849`, pointing to that commit; tag object `d019d65f7ee81e83f38b243a07b1bbbf37f6d77f`. The prior `v1.0.0+upstream-f87e849` release and tag were retained.
- Relevant implementation commits: `1e691bfe76358ea61dbdf1069cdb7174062556d4` through `312bf66b19d8851e5f4631defca4c4bf1820bad9`, including the portability and packaging fixes in `8c5f991cb2`, `a467342d24`, `91b0b44d66`, and `6145a9eeed`.
- Validation evidence: successful branch workflow [37939448702](https://github.com/LiveMindIO/dolphin-dap/actions/runs/37939448702) and successful tag workflow, attempt 3, [37990189706](https://github.com/LiveMindIO/dolphin-dap/actions/runs/37990189706), both at the release commit.
- Published release: [v1.0.1+upstream-f87e849](https://github.com/LiveMindIO/dolphin-dap/releases/tag/v1.0.1%2Bupstream-f87e849), published 2026-10-09T23:26:15Z.
- Source conversation: session and response history were provided in the completion checkpoint; source response `msg_122fe857c001pt2AWaDeIU4bNA`. Authoring agent: Annalist (GPT-5.6 Terra). The checkpoint is a summary rather than a complete transcript; GitHub and local artifacts below were independently verified.
- Working tree at recording: `annals/README.md` was already modified and seven earlier annals were already untracked. They were not edited by this record.

## Decision summary

Publish `v1.0.1+upstream-f87e849` from `312bf66b19` after both a complete branch validation and a complete tag-triggered validation passed. Publish the Windows ZIP, nine native platform artifacts (six Linux packages and two macOS DMGs plus Windows), and a SHA-256 sidecar for each asset.

## Context and evidence

The earlier 2026-10-08 packaging records adopted a full release matrix and explicitly prohibited partial publication while Windows, macOS, and Gentoo validation remained incomplete. The current workflow enforces that policy: its publish job requires Windows, Linux, Linux-package, and macOS jobs, checks all nine asset sidecars with `sha256sum --check`, and only runs on a pushed tag (`.github/workflows/build.yml`, lines 267-301).

Branch run 37939448702 completed successfully for Windows Release/Debug, Linux Release/Debug, macOS arm64/x86_64, and DEB, RPM, Arch, Gentoo GPKG, AppImage, and Flatpak packaging. Its publish job was correctly skipped because it was a branch run. The tagged workflow 37990189706 subsequently completed successfully with the same matrix and its publish job completed successfully. Earlier tag-workflow attempts encountered Docker Hub image-pull rate limits, timeouts, and HTTP 500 responses before builds; the successful rerun establishes these as transient infrastructure failures, not validated product failures.

The public release has nine binary assets and nine checksum files. All nine were downloaded to the recorded local validation directory and each `sha256sum --check` invocation returned `OK`.

## Options considered

- Publish after only the branch matrix: rejected. Branch workflows cannot publish under the defined tag-only policy, and a tag run was required to validate the release path itself.
- Publish only already-passing formats: rejected. The accepted matrix decision requires every platform and package job to pass before publication.
- Treat image-pull failures as implementation failures or bypass the matrix: rejected. They occurred before compilation/package validation and rerunning produced a fully successful workflow; bypassing would weaken the release gate.
- Use notarized or Developer ID-signed macOS images: not selected for this release. The implemented packaging uses ad-hoc signing and explicit verification, without notarization.

## Chosen option and rationale

The chosen tag was the version increment `v1.0.1+upstream-f87e849`, preserving the existing `v1.0.0+upstream-f87e849` release. The tag message identifies the release contents and cites the successful branch run. The user authorized release creation only after validation; the dual successful runs, workflow-enforced checksum gate, and independent post-publication download verification supplied that evidence. Confidence is high for the release state and asset integrity because it is corroborated by GitHub workflow/release APIs, the local Git tag, and local checksum verification.

## Consequences, tradeoffs, and follow-up

- Published assets are: Windows x64 ZIP; Ubuntu 24.04 DEB; Fedora 44 RPM; Arch package; Gentoo GPKG; AppImage; Flatpak; and macOS arm64/x86_64 DMGs, each with a SHA-256 file.
- macOS images require macOS 14 or newer and are ad-hoc signed, not Developer ID-signed or notarized. Gatekeeper may block first launch; this remains a user-facing limitation rather than a claim of notarization.
- The release pipeline remains tag-only, validates every named artifact before upload, and may replace assets on rerun of a tag workflow. Future release changes must keep the asset list and checksum verification aligned.
- The published release is immutable only to the extent GitHub release and tag permissions enforce it; no signature, notarization, or public signed Gentoo binhost was added by this decision.

## Relationship to prior records

This fulfills the open validation and release obligations in `Cross-platform CI packaging validation fixes`, `Packaging fixes local unit validation completed`, and `Retry CI clears Linux packaging failures; Windows and macOS portability work remains`. Their retained-full-matrix decision is confirmed, not superseded; their time-bounded claims of incomplete CI validation are superseded by the successful runs above.

Those records and the annals index had pre-existing uncommitted changes at recording time. To avoid overwriting concurrent work, this record does not add their required correction backlinks or alter their statuses. Reconcile this record into `annals/README.md` and add dated subsequent-development backlinks to those three records when their current owner can safely incorporate the changes.

## Reconsideration triggers and open questions

- Reconsider the release process if a published asset fails installation or runtime validation on its documented target environment, or if workflow artifacts and public downloads no longer checksum-match.
- Add Developer ID signing and notarization before claiming a frictionless macOS installation experience.
- The full transcripts, exact earlier failed-run IDs, and Docker registry logs were not available in the supplied checkpoint. Their transient classification is limited to the reported pre-build failures and the later successful rerun; this record does not independently reconstruct those failed attempts.

## Correction (2026-10-19)

The three 2026-10-08 records named above are absent from this branch's `annals/` directory, so their original local Markdown links could not resolve. Their titles and the historical relationship claim are retained as plain text; no conclusion about their missing history is drawn.
