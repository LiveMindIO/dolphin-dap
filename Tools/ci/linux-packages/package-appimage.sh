#!/usr/bin/env bash
set -euo pipefail
cd /work
mkdir -p tools AppDir
# Checked hashes deliberately fail closed if a rolling tool release changes.
curl -fL --retry 3 -o tools/linuxdeploy \
  https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
curl -fL --retry 3 -o tools/linuxdeploy-plugin-qt \
  https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
echo '8aea8da0f7f7039d2a2cecb14657d752a222a5e1d3825caeef186c82f751cdd1  tools/linuxdeploy' | sha256sum -c
echo 'cfc1055b2b9dbc08412b579f20990b7b41a17b61beaa5847dc9477c96c9e9617  tools/linuxdeploy-plugin-qt' | sha256sum -c
chmod +x tools/linuxdeploy tools/linuxdeploy-plugin-qt
cp -a stage/usr AppDir/
cp -a build/Binaries/Languages AppDir/usr/bin/
ln -s ../share/dolphin-emu/Sys AppDir/usr/bin/Sys
cp /source/Tools/ci/linux-packages/appimage-launcher AppDir/usr/bin/dolphin-dap-launcher
chmod +x AppDir/usr/bin/dolphin-dap-launcher
desktop=AppDir/usr/share/applications/dolphin-emu.desktop
sed -i 's/^Exec=.*/Exec=dolphin-dap-launcher/; s/^Name=.*/Name=Dolphin DAP/' "$desktop"
export PATH="/work/tools:$PATH" APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE=/usr/bin/qmake6 OUTPUT=/release/dolphin-dap-linux-x86_64.AppImage
tools/linuxdeploy --appdir AppDir --plugin qt --output appimage \
  --executable AppDir/usr/bin/dolphin-emu --executable AppDir/usr/bin/dolphin-emu-nogui \
  --executable AppDir/usr/bin/dolphin-tool --desktop-file "$desktop" \
  --icon-file /source/Data/dolphin-emu.svg
"$OUTPUT" --version
"$OUTPUT" --nogui --version
