#!/usr/bin/env bash
# Runs in a SECOND container without compiler/development packages.
set -euo pipefail
case "${1:?format}" in
  deb)
    apt-get update
    apt-get install -y --no-install-recommends /release/*.deb
    ;;
  rpm) dnf install -y /release/*.rpm ;;
  arch)
    pacman -Syu --noconfirm
    pacman -U --noconfirm /release/*.pkg.tar.zst
    ;;
  appimage)
    apt-get update
    apt-get install -y --no-install-recommends libgl1 libegl1 libfuse2t64
    export QT_QPA_PLATFORM=offscreen APPIMAGE_EXTRACT_AND_RUN=1
    /release/dolphin-dap-linux-x86_64.AppImage --version
    /release/dolphin-dap-linux-x86_64.AppImage --nogui --version
    exit 0
    ;;
  *) echo 'Gentoo binary installation is checked using Portage in the build container.' >&2; exit 2 ;;
esac
bash /source/Tools/ci/linux-packages/smoke-binaries.sh
