#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
destination=$(realpath -m "${1:-$root/release}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 "$root/Tools/ci/linux-packages/prepare-flatpak.py" "$work/manifest.yml"
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --install-deps-from=flathub --disable-rofiles-fuse \
  --repo="$work/repo" --force-clean "$work/build" "$work/manifest.yml"
mkdir -p "$destination"
flatpak build-bundle "$work/repo" "$destination/dolphin-dap-linux-x86_64.flatpak" \
  io.github.LiveMindIO.DolphinDAP --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user --noninteractive "$destination/dolphin-dap-linux-x86_64.flatpak"
flatpak run --user --env=QT_QPA_PLATFORM=offscreen io.github.LiveMindIO.DolphinDAP --version
flatpak run --user --command=dolphin-emu-nogui io.github.LiveMindIO.DolphinDAP --version
