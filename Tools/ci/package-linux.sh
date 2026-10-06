#!/usr/bin/env bash
# Package a LINUX_LOCAL_DEV build. System shared libraries are intentionally not bundled.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build=$(realpath "${1:-$root/build}")
destination=$(realpath -m "${2:-$root/release}")
name=dolphin-dap-linux-x64
stage="$destination/$name"
if [[ -e "$stage" ]]; then
  echo "Release staging directory already exists: $stage" >&2
  exit 1
fi
mkdir -p "$stage/Languages"

for executable in dolphin-emu dolphin-emu-nogui; do
  test -x "$build/Binaries/$executable"
  cp "$build/Binaries/$executable" "$stage/"
  # Fail on unresolved libraries instead of publishing a nonfunctional binary.
  dependencies=$(ldd "$stage/$executable")
  if grep -q 'not found' <<< "$dependencies"; then
    echo "$dependencies" >&2
    exit 1
  fi
  printf '%s\n%s\n' "$executable:" "$dependencies" >> "$stage/shared-libraries.txt"
done
cp -a "$root/Data/Sys" "$stage/Sys"
cp -a "$root/LICENSES" "$stage/LICENSES"
cp "$root/COPYING" "$stage/"

# The Qt translation loader checks Languages/<lang>/dolphin-emu.mo beside the executable.
translations=0
for translation in "$build"/Binaries/Languages/*/dolphin-emu.mo; do
  [[ -f "$translation" ]] || continue
  language=$(basename "$(dirname "$translation")")
  mkdir -p "$stage/Languages/$language"
  cp "$translation" "$stage/Languages/$language/dolphin-emu.mo"
  translations=$((translations + 1))
done
if [[ "$translations" -eq 0 ]]; then
  echo 'Missing compiled Qt translations; install gettext before configuring.' >&2
  exit 1
fi
cp "$root/Tools/ci/linux-release-README.txt" "$stage/README.txt"

# Run the staged binaries, not the build-tree copies. Qt uses an offscreen backend in CI.
"$stage/dolphin-emu-nogui" --version
QT_QPA_PLATFORM=offscreen "$stage/dolphin-emu" --version

# Archive contents include a named directory and retain executable permissions.
tar -C "$destination" -czf "$destination/$name.tar.gz" "$name"
(cd "$destination" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
