#!/usr/bin/env bash
set -euo pipefail
export QT_QPA_PLATFORM=offscreen
for binary in dolphin-emu dolphin-emu-nogui dolphin-tool; do
  dependencies=$(ldd "/usr/bin/$binary")
  if grep -q 'not found' <<< "$dependencies"; then
    echo "$dependencies" >&2
    exit 1
  fi
  if [[ "$binary" == dolphin-tool ]]; then
    "/usr/bin/$binary" convert --help
  else
    "/usr/bin/$binary" --version
  fi
done
test -s /usr/share/dolphin-emu/Sys/GC/font_western.bin
find /usr/share/locale -name dolphin-emu.mo -print -quit | grep -q .
