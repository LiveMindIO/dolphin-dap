#!/usr/bin/env bash
set -euo pipefail
cd /work
mkdir -p tools AppDir
# Checked hashes deliberately fail closed if a rolling tool release changes.
curl -fL --retry 3 -o tools/linuxdeploy \
  https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
curl -fL --retry 3 -o tools/linuxdeploy-plugin-qt \
  https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
curl -fL --retry 3 -o tools/runtime-x86_64 \
  https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
echo '8aea8da0f7f7039d2a2cecb14657d752a222a5e1d3825caeef186c82f751cdd1  tools/linuxdeploy' | sha256sum -c
echo 'cfc1055b2b9dbc08412b579f20990b7b41a17b61beaa5847dc9477c96c9e9617  tools/linuxdeploy-plugin-qt' | sha256sum -c
echo '156f4bdbde9c52d01814600013e0a273f0118dc2de98975f3c8c63427ec79074  tools/runtime-x86_64' | sha256sum -c
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
export LDAI_RUNTIME_FILE=/work/tools/runtime-x86_64
export EXTRA_PLATFORM_PLUGINS=libqoffscreen.so
# linuxdeploy normally assumes these libraries exist on a desktop. Bundle them
# explicitly; leave glibc and OpenGL/EGL driver-facing libraries on the host.
libraries=()
for library in libasound.so.2 libstdc++.so.6 libgpg-error.so.0 libfontconfig.so.1 \
  libfreetype.so.6 libharfbuzz.so.0 libexpat.so.1 libz.so.1 libuuid.so.1 \
  libX11.so.6 libX11-xcb.so.1 libxcb.so.1 libSM.so.6 libICE.so.6; do
  libraries+=(--library "/usr/lib/x86_64-linux-gnu/$library")
done
tools/linuxdeploy --appdir AppDir --plugin qt --output appimage \
  --executable AppDir/usr/bin/dolphin-emu --executable AppDir/usr/bin/dolphin-emu-nogui \
  --executable AppDir/usr/bin/dolphin-tool --desktop-file "$desktop" \
  --icon-file /source/Data/dolphin-emu.svg "${libraries[@]}"
"$OUTPUT" --version
"$OUTPUT" --nogui --version
