# DAP mutation paths must invalidate writable variable expansion handles

Date: 2026-10-05
Status: implemented
Scope: DAP `writeMemory` and mutating `evaluate` expressions vs. variable expansion-handle lifetime.

## Git provenance

- Repository: Dolphin emulator debugger branch, `feature/gui-source-debugger`
- Canonical remote: `origin` (`https://github.com/Jacoby6000/dolphin.git`, redirects to `https://github.com/LiveMindIO/dolphin-dap.git`)
- Branch state: clean, tracking `origin/feature/gui-source-debugger`; untracked `annals/` preserved untouched
- Recording HEAD: `88a5ddc3b99aa2bce2df97a12452f2486197032d` (`88a5ddc3b9`), nearest describe `5.0-25543-g88a5ddc3b9`
- Source session: `ses_f07c8f576ffergQyBAxj2eibaJ`, response `msg_10d3d3060001E1yYYHexxiez6b`
- Open finding at `129ff7b290`, remediated in `88a5ddc3b9`.

## Resolution (2026-10-05, `88a5ddc3b9`)

Implemented as recommended in the original record, commit `88a5ddc3b9` ("DAP: invalidate variable handles after memory and expression writes"), pushed to `feature/gui-source-debugger`; PR #5 head verified at `88a5ddc3b99aa2bce2df97a12452f2486197032d`.

- `DapDebugController::WriteMemory` publishes shared `ValuesChanged` with the actual written address/size after the `CPUThreadGuard` scope, covering partial writes even when the caller later reports an error; zero-byte writes do not notify.
- `DapDebugController::EvaluateExpression` evaluates under the guard and, afterwards, publishes `ValuesChanged` when the parsed AST's new `Expression::MayWriteState()` detects an `OP_ASSIGN` node or any `write_*` host function. Conservative by design: a conditional branch that never executes the write still invalidates; string literals naming write functions (e.g. inside `streq`) and read-only expressions do not.
- Tests: new parameterized socket suite `DapMutationInvalidationTest` (writeMemory, partial writeMemory, register-pointer evaluate, memory-write evaluate) verifying stale pointer reads/writes are rejected, fresh expansion references must be reacquired, and the fresh reference resolves the current pointee; controller-level tests verify other registered execution clients receive the event, read-only evaluations do not, and parsed-node detection is conservative-but-precise across nine expression cases. Seven new tests passed 20 repetitions; full suite 1,901 tests passed with no exclusions; `tests`, `dolphin-emu`, and `dolphin-emu-nogui` built.
- Docs: `Tools/dap/capabilities.md` expansion-reference lifetime text now names `writeMemory` (including partial writes) and mutating `evaluate` expressions as invalidation triggers.
- Incidental: the first commit attempt was blocked by a stale empty `index.lock`; after verifying no Git process owned it, the lock was removed and the commit succeeded. Server restarts may also remove `/tmp/opencode` temporary reproducers while preserving build artifacts.
- Pre-existing untracked `annals/` was preserved untouched.

## Finding

`DapDebugController::EvaluateExpression` (lines 487–498) and `DapDebugController::WriteMemory` (lines 1069–1102) mutate guest state without publishing `ValuesChanged`, and the DAP `HandleWriteMemory` path does not retire expansion references. A temporary socket-level review (`/tmp/opencode/pr5-mutation-invalidation-review.cpp`, both cases failing) demonstrated:

- Obtain a `variablesReference` for a pointer expansion (register- or address-located).
- Mutate the pointer via `evaluate` assignment (`r3 = 0x6000`) or `writeMemory` (base64 `AABgAA==` at the pointer's address).
- The old expansion reference remains usable: a later `setVariable` through it succeeds.
- That `setVariable` writes to the **old pointee's address** (e.g. `0x5000`) instead of the current target (`0x6000`).

This is a correctness defect, not merely a stale-display issue: with writable typed children, a stale handle performs a real guest-memory write to an unintended address.

## Context and problem

`b23715c6ec` invalidated expansion handles after DAP value edits, and the typed-write support added in the same session makes expansion handles writable. However, the invalidation only covers `setVariable`-initiated edits (register/global/local/typed-child) and external `ValuesChanged` events. Raw `writeMemory` and expression assignments through `evaluate` bypass that path.

## Options considered

- Publish a shared `ValuesChanged` (outside `CPUThreadGuard`) for all successful mutation paths — `writeMemory` (including partial writes) and any `evaluate` expression that mutates state. Chosen recommendation.
- Retire expansion handles only in the DAP `HandleWriteMemory` path. Rejected as incomplete; `evaluate` assignments and GUI/other-client mutations must also invalidate.
- Take no action. Rejected; a stale writable handle can corrupt guest memory.

## Rationale

Expansion handles are derived from mutable execution state. Any mutation of registers or memory — regardless of which client surface initiated it — can invalidate every derived handle. The shared `ValuesChanged` event is the existing, topology-correct invalidation signal and also refreshes the GUI and other DAP clients.

## Consequences

- Until remediated, clients should re-request scope variables after `writeMemory` or any mutating `evaluate` rather than reusing expansion references (same reacquisition semantics `b23715c6ec` documented for edits).
- A follow-up fix should add regression coverage for stale-read rejection and stale-write rejection after both `writeMemory` and mutating `evaluate`.

## Implementation evidence

- Temporary parameterized socket test (`EvaluateAndWriteMemory/ReviewMutationInvalidation`) failed both before-fix cases: stale read succeeded, stale write succeeded writing `0x5000` instead of `0x6000`.
- Full suite at `129ff7b290`: 1,894 tests passed with no exclusions. PR head verified unchanged at `129ff7b29092e44a7ba3dba355b889ba4b565d00`.

## Follow-up obligations

- (Fulfilled in `88a5ddc3b9`) Publish `ValuesChanged` from `WriteMemory` (all successful and partial writes) and from `EvaluateExpression` when the expression mutates state, outside `CPUThreadGuard`.
- (Fulfilled in `88a5ddc3b9`) Preserve reacquisition semantics in client tests and `Tools/dap/capabilities.md`.
- (Fulfilled in `88a5ddc3b9`) Add regression tests mirroring the temporary review reproducer.

## Reconsideration triggers

- If expansion handles become generation-scoped rather than cleared, retain the guarantee that stale derived handles cannot read or write through old addresses.

## Open questions

- Should `evaluate` assignments be classified conservatively (always invalidate) or only when the expression is known to mutate? The safe default is to invalidate on any `evaluate` that reaches a write. **Resolved in `88a5ddc3b9`:** conservative — any parsed assignment or `write_*` call invalidates even under skipped branches; only expressions free of both retain handles.
