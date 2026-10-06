# Build, test, and release CI

[`.github/workflows/build.yml`](../../.github/workflows/build.yml) runs on pull
requests, branch pushes, tag pushes, and manual dispatch. It builds **Windows x64**
and **Linux x64**, in both Release and Debug configurations, including the Qt source
debugger and NoGUI DAP frontend. Every job builds and runs the unit tests. No game
image or debug ELF is needed.

The recipes adapt upstream's Buildbot factories and
[Ubuntu builder dependencies](https://github.com/dolphin-emu/sadm/blob/1b682df75adebc5d93d4abd0db5f965f18ae71ed/containers/ubuntu-lts-builder/Dockerfile) from
[`dolphin-emu/sadm`, revision `1b682df75ade`](https://github.com/dolphin-emu/sadm/blob/1b682df75adebc5d93d4abd0db5f965f18ae71ed/roles/buildbot/etc/master.cfg):

- Windows uses CMake with the Visual Studio 2022 generator and the `unittests`
  target, following upstream's CMake path after removal of the native projects.
  Both platforms share the same debugger source lists. POSIX-only socket tests retain
  their existing Windows guards; they run in the Linux job.
- Linux uses CMake/Ninja, with `FASTLOG=ON` for Debug and the `unittests` target.
  Dependencies follow upstream's Ubuntu 24.04 builder. Configuration is explicit
  to select GitHub's available compiler versions rather than upstream's VS 2026 preset.
- Distribution identity is `LiveMindIO`. Upstream automatic updates are disabled.
  Upstream's Buildbot workers, signing credentials, website notifications, FifoCI,
  and Flatpak publishing infrastructure are not used.

## Artifacts and tag releases

Successful Release jobs upload packages as Actions artifacts, including on PRs.
JUnit XML and Linux CTest diagnostics are uploaded even if tests fail.

Only a **tag push** publishes a GitHub Release. Publication depends on all four
build/test jobs passing, verifies both packages' SHA-256 checksums, and uploads:

- `dolphin-dap-windows-x64.zip` and its `.sha256` file: Qt and NoGUI executables,
  Qt plugins, app-local MSVC runtime, Sys resources, translations, and licenses.
- `dolphin-dap-linux-x64.tar.gz` and its `.sha256` file: Qt and NoGUI executables,
  Sys resources, translations, licenses, runtime dependency information, and usage
  instructions. This is an **Ubuntu 24.04 native archive**, not an AppImage or
  Flatpak; system runtime libraries must be installed. See the included README.

The tagged commit must contain the workflow. Manual dispatch and branch pushes
build/test only, even if a manual run selects a tag. Rerunning a tag build replaces
assets on an existing release; it does not change that release's draft/prerelease
status or notes. Builds do not publish until both platforms and both configurations
have passed. Only the publish job has `contents: write`; PR jobs have read-only
repository permissions and do not use release secrets.

The packages do not contain games, debug ELFs, or a separate user profile. Extract
them together and follow the [DAP](../dap/README.md) or
[Qt debugger](../dap/qt-source-debugging.md) guide. macOS, Android, and ARM64 builds
are not part of this workflow.

## Local CI checks

Install PyYAML (`python3-yaml` on Ubuntu) for the workflow contract tests. Without
it those tests are skipped; the CI Linux job installs it explicitly.

```sh
python3 -m unittest discover -s Tools/ci/tests -v
bash -n Tools/ci/package-linux.sh
actionlint .github/workflows/build.yml
```

The packaging tests use synthetic build outputs and mock `ldd` and Visual Studio
discovery; they check archive contents, permissions, checksums, and failures, not
whether Dolphin actually runs. PowerShell packaging tests run if `pwsh` is available
on the Linux host; otherwise they are skipped.
CI additionally smoke-tests the real staged binaries. Windows compilation and
PowerShell packaging require a Windows runner with Visual Studio 2022.
