Dolphin DAP for macOS 14 or newer

Choose arm64 for Apple Silicon or x86_64 for an Intel Mac.
Open the DMG and drag Dolphin DAP.app into Applications. Keep the entire app bundle.

These builds are ad-hoc signed, NOT Developer ID signed or notarized. macOS may
block the first launch. After verifying the download checksum and its origin,
use macOS System Settings > Privacy & Security to allow the app if you trust it.
No Apple Developer signing credentials are configured in this repository.

For an editor-managed DAP session, the NoGUI executable is:
/Applications/Dolphin DAP.app/Contents/MacOS/dolphin-emu-nogui
Quote the path when running it in a terminal. dolphin-tool is in the same folder.

Games, ISOs and debug ELFs are not included. See:
https://github.com/LiveMindIO/dolphin-dap/tree/master/Tools/dap
