#!/usr/bin/env bash
# Package the tested native build as a drag-to-Applications disk image.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
build=${1:?build directory}
destination=${2:?release directory}
arch=${3:?arm64 or x86_64}
case "$arch" in arm64|x86_64) ;; *) exit 2 ;; esac
mkdir -p "$destination"
work=$(mktemp -d)
mountpoint="$work/mounted"
cleanup() {
  if mount | grep -Fq " on $mountpoint "; then hdiutil detach "$mountpoint"; fi
  rm -rf "$work"
}
trap cleanup EXIT
stage="$work/image"
app="$stage/Dolphin DAP.app"
mkdir -p "$stage"
ditto "$build/Binaries/DolphinQt.app" "$app"
for binary in dolphin-emu-nogui dolphin-tool; do
  cp "$build/Binaries/$binary" "$app/Contents/MacOS/"
done
cp "$root/COPYING" "$app/Contents/Resources/"
cp -R "$root/LICENSES" "$app/Contents/Resources/"
/usr/libexec/PlistBuddy -c 'Set :CFBundleIdentifier io.github.LiveMindIO.DolphinDAP' "$app/Contents/Info.plist"
"$(brew --prefix qt)/bin/macdeployqt" "$app" \
  "-executable=$app/Contents/MacOS/dolphin-emu-nogui" \
  "-executable=$app/Contents/MacOS/dolphin-tool" -always-overwrite
cmake "-DDOLPHIN_BUNDLE_PATH=$app" -P "$root/CMake/DolphinPostprocessBundle.cmake"
entitlements="$root/Source/Core/DolphinQt/DolphinEmu.entitlements"
for binary in dolphin-emu-nogui dolphin-tool; do
  bash "$root/Tools/mac-codesign.sh" -e "$entitlements" - "$app/Contents/MacOS/$binary"
done
# Ad-hoc signing enables Apple Silicon execution; this is NOT notarization.
bash "$root/Tools/mac-codesign.sh" -e "$entitlements" - "$app"
codesign --verify --deep --strict "$app"
ln -s /Applications "$stage/Applications"
cp "$root/Tools/ci/macos-release-README.txt" "$stage/README.txt"
archive="$destination/dolphin-dap-macos-$arch.dmg"
hdiutil create -volname 'Dolphin DAP' -srcfolder "$stage" -format UDZO "$archive"
mkdir -p "$mountpoint"
hdiutil attach -nobrowse -readonly -mountpoint "$mountpoint" "$archive"
mounted="$mountpoint/Dolphin DAP.app"
codesign --verify --deep --strict "$mounted"
for binary in DolphinQt dolphin-emu-nogui dolphin-tool; do
  "$mounted/Contents/MacOS/$binary" --version
done
hdiutil detach "$mountpoint"
(cd "$destination" && shasum -a 256 "$(basename "$archive")" > "$(basename "$archive").sha256")
