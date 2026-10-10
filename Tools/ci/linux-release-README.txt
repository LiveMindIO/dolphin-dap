Dolphin DAP for Linux x64
========================

This archive is built and tested on Ubuntu 24.04 (glibc 2.39). It is not an
AppImage or a universally compatible Linux binary. It uses system shared
libraries; see shared-libraries.txt for the exact dependencies from the build.
Older distributions may need a source build instead.

On Ubuntu 24.04, install the runtime dependencies:

sudo apt-get update
sudo apt-get install libqt6widgets6 libqt6svg6 qt6-qpa-plugins libqt6opengl6 \
  libavcodec60 libavformat60 libavutil58 libswresample4 libswscale7 \
  libevdev2 libudev1 libxrandr2 libxi6 libbluetooth3 libasound2t64 libpulse0 \
  libgl1 libegl1 libusb-1.0-0 libstdc++6

Keep the extracted directory together. Run ./dolphin-emu for the Qt debugger
or use the absolute path to dolphin-emu-nogui in your editor's launch settings.
The NoGUI game-window backend on Linux is x11; a Wayland desktop needs XWayland.
Check missing libraries with: ldd ./dolphin-emu ./dolphin-emu-nogui

The official Dolphin auto-updater is disabled. No games or debug ELFs are included.
Setup: https://github.com/LiveMindIO/dolphin-dap/blob/master/Tools/dap/README.md
Qt: https://github.com/LiveMindIO/dolphin-dap/blob/master/Tools/dap/qt-source-debugging.md
