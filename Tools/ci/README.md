# Build, test, and release CI

[`.github/workflows/build.yml`](../../.github/workflows/build.yml) runs on pull
requests, branch pushes, tag pushes, and manual dispatch. It builds **Windows x64**
and **Linux x64**, in both Release and Debug configurations, including the Qt source
debugger and NoGUI DAP frontend. Six additional Release packaging jobs compile and
test Linux in their target environments. No game
image or debug ELF is needed.

The recipes adapt upstream's Buildbot factories and
[Ubuntu builder dependencies](https://github.com/dolphin-emu/sadm/blob/1b682df75adebc5d93d4abd0db5f965f18ae71ed/containers/ubuntu-lts-builder/Dockerfile) from
[`dolphin-emu/sadm`, revision `1b682df75ade`](https://github.com/dolphin-emu/sadm/blob/1b682df75adebc5d93d4abd0db5f965f18ae71ed/roles/buildbot/etc/master.cfg):

- Windows uses CMake with the Visual Studio 2026 generator and the `unittests`
  target, following upstream's CMake path after removal of the native projects.
  Both platforms share the same debugger source lists. POSIX-only socket tests retain
  their existing Windows guards; they run in the Linux job.
- Linux uses CMake/Ninja, with `FASTLOG=ON` for Debug and the `unittests` target.
  Dependencies follow upstream's Ubuntu 24.04 builder. Configuration is explicit
  to apply the fork's distribution and auto-update settings on both platforms.
- Distribution identity is `LiveMindIO`. Upstream automatic updates are disabled.
  Upstream's Buildbot workers, signing credentials, website notifications, FifoCI,
  and Flatpak publishing infrastructure are not used.

## Artifacts and tag releases

Successful Release jobs upload packages as Actions artifacts, including on PRs.
JUnit XML and Linux CTest diagnostics are uploaded even if tests fail.

Only a **tag push** publishes a GitHub Release. Publication depends on all four
Windows/Linux build/test jobs, both macOS jobs and all six Linux packaging jobs passing, verifies every package's
SHA-256 checksum, and uploads:

- `dolphin-dap-windows-x64.zip` and its `.sha256` file: Qt and NoGUI executables,
  Qt plugins, app-local MSVC runtime, Sys resources, translations, and licenses.
- The six Linux packages below, each with a `.sha256` file.
- `dolphin-dap-macos-arm64.dmg` and `dolphin-dap-macos-x86_64.dmg`, each with a
  `.sha256` file. These require macOS 14 or newer. Open the DMG and drag
  `Dolphin DAP.app` to Applications. The app contains bundled Qt dependencies,
  resources, licenses, and NoGUI/CLI executables under `Contents/MacOS`.
  Builds are ad-hoc signed, **not Developer ID signed or notarized**; macOS may
  block first launch. See the included README before allowing the app.

| Format | Build environment / scope | Installation |
| --- | --- | --- |
| `.deb` | Ubuntu 24.04 amd64 | `sudo apt install ./dolphin-dap-ubuntu24.04-amd64.deb` |
| `.rpm` | Fedora 44 x86_64 | `sudo dnf install ./dolphin-dap-fedora44-x86_64.rpm` |
| `.AppImage` | Ubuntu 24.04 baseline, glibc 2.39 or newer | Make executable with `chmod +x`, then run |
| `.gpkg.tar` | Gentoo amd64 desktop/systemd, glibc | Install through Portage; see below |
| `.pkg.tar.zst` | Current Arch Linux x86_64 | `sudo pacman -U ./dolphin-dap-archlinux-x86_64.pkg.tar.zst` |
| `.flatpak` | KDE 6.10 runtime, x86_64 | `flatpak install --user ./dolphin-dap-linux-x86_64.flatpak` |

DEB, RPM, Arch and GPKG contain binaries compiled in their own distribution, not
renamed Ubuntu archives. They conflict with the distribution's upstream Dolphin
package because both install the same executable and resource paths. Dependencies
are generated from ELF linkage for DEB/RPM and declared for Arch/Gentoo. DEB/RPM/Arch
are installed and launched in a second container without the build dependencies.
Portage creates and reinstalls the Gentoo binary package. All packages include the
GUI, NoGUI frontend, resources, translations and licenses. Native packages also
include `dolphin-tool`.
Linux package builds disable FFmpeg frame-dump encoding to keep the dependency
baseline manageable; emulation and source/DAP debugging remain enabled.

The AppImage bundles Qt and non-baseline libraries using checksum-verified
linuxdeploy tools. It is **not compatible with every Linux distribution**: older
glibc and musl-based systems are outside its scope. FUSE is needed for ordinary
execution; `APPIMAGE_EXTRACT_AND_RUN=1 ./dolphin-dap-linux-x86_64.AppImage` is an
alternative. Add `--nogui` before Dolphin arguments to launch NoGUI.
Rolling packaging-tool downloads are hash-checked; updating them requires
reviewing and changing the hashes in `linux-packages/package-appimage.sh`.

For Gentoo, copy the package into a local binary package directory, then run
`PKGDIR=/path/to/binpkgs emaint binhost --fix` to index it. Install with
`PKGDIR=/path/to/binpkgs emerge --usepkgonly --oneshot games-emulation/dolphin-dap`.
The included `dolphin-dap.ebuild` is a build-pipeline staging recipe, not a source
ebuild for end users. The binary package is unsigned and intended for manual local
installation, not a public signed binhost. Check its SHA-256 first.

Flatpak uses `io.github.LiveMindIO.DolphinDAP`, separate from upstream Dolphin.
Add Flathub if necessary:
`flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo`.
Launch with `flatpak run io.github.LiveMindIO.DolphinDAP`. For NoGUI use
`flatpak run --command=dolphin-emu-nogui io.github.LiveMindIO.DolphinDAP`.
TCP DAP networking is enabled, but filesystem access remains sandboxed. Grant
access to your ISO/ELF/source directories explicitly, for example:
`flatpak override --user --filesystem=/path/to/project:ro io.github.LiveMindIO.DolphinDAP`.
Use a separate writable permission for your DAP Unix-socket directory if needed.

The tagged commit must contain the workflow. Manual dispatch and branch pushes
build/test only, even if a manual run selects a tag. Rerunning a tag build replaces
assets on an existing release; it does not change that release's draft/prerelease
status or notes. All build/test and packaging jobs must pass before publication.
Only the publish job has `contents: write`; PR jobs have read-only
repository permissions and do not use release secrets.

The packages do not contain games, debug ELFs, or a separate user profile. Extract
them together and follow the [DAP](../dap/README.md) or
[Qt debugger](../dap/qt-source-debugging.md) guide. Android and Linux/Windows ARM64
builds are not part of this workflow. macOS has native Apple Silicon and Intel
Release builds/tests; it does not reuse a cross-compiled Linux binary.

## Local CI checks

Install PyYAML (`python3-yaml` on Ubuntu) for the workflow contract tests. Without
it those tests are skipped; the CI Linux job installs it explicitly.

```sh
python3 -m unittest discover -s Tools/ci/tests -v
bash -n Tools/ci/package-linux.sh
actionlint .github/workflows/build.yml
shellcheck Tools/ci/linux-packages/*.sh
# Requires Docker; builds, tests, packages and validates installation:
bash Tools/ci/linux-packages/container-build.sh deb /tmp/opencode/packages/deb
# Other container formats: rpm, arch, gpkg, appimage
# Requires flatpak, flatpak-builder and PyYAML:
bash Tools/ci/linux-packages/build-flatpak.sh /tmp/opencode/packages/flatpak
```

The packaging tests use synthetic build outputs and mock `ldd` and Visual Studio
discovery; they check archive contents, permissions, checksums, and failures, not
whether Dolphin actually runs. PowerShell packaging tests run if `pwsh` is available
on the Linux host; otherwise they are skipped.
CI additionally smoke-tests the real staged binaries. Windows compilation and
PowerShell packaging require the `windows-2025-vs2026` runner with Visual Studio 2026.
