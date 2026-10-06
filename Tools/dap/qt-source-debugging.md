# Source debugging in Dolphin's Qt interface

Use the built-in debugger without VS Code, Neovim, or a DAP connection. You need
a legally obtained game ISO, a debug ELF with MWCC/CodeWarrior DWARF 1.1, and the
matching source checkout. Compile the files you want to inspect without optimization.

## Build and start Qt Dolphin

Install the [platform build prerequisites](../../Readme.md#building),
including Qt development packages. From the Dolphin repository root on Linux:

```sh
git submodule update --init --recursive
cmake -B build -DENABLE_QT=ON -DLINUX_LOCAL_DEV=ON
cmake --build build --target dolphin-emu
ln -s ../../Data/Sys build/Binaries/Sys
./build/Binaries/dolphin-emu \
  -C Dolphin.Interface.DebugModeEnabled=True \
  -C Dolphin.General.DAPSocket= \
  -C Dolphin.General.DAPPort=-1
```

Skip the `ln` command if `build/Binaries/Sys` already exists. Enabling Qt explicitly
also works when your build directory was previously configured with Qt disabled.
On Windows, follow the [Windows build instructions](https://github.com/dolphin-emu/dolphin/wiki/Building-for-Windows)
and start your built `Dolphin.exe` with the same three `-C` overrides on one line.
Qt Dolphin does not take a `--platform` argument.

The first override enables core debugging and its GUI panes. The other two disable
a saved DAP listener for this standalone workflow, so the game does not wait for
an external client. All three apply only to this launch.

## Configure the game before booting

1. In **Options → Configuration → Paths**, add the directory containing your ISO
   so it appears in the game list.
2. Right-click the ISO in the game list and open **Properties → Debugging**. This
   tab is for disc games; use the ISO entry, not the ELF entry.
3. Set **ELF file** to the debug ELF built from your source checkout. Enable
   **Replace the disc executable with the ELF file**. The ISO supplies the disc
   bootstrap and game files; the ELF supplies the executed code and debug information.
4. Under **Source paths**, use **Add...** to add source directories in search order.
   For current Melee, add your checkout's `src`, then `libs/dolphin/src` (older
   checkouts may use `extern/dolphin/src`). Double-click an entry to edit it; use
   **Remove** to delete it. These settings are saved for this game and take effect
   on its next boot.
5. Close Properties, enable **Options → Boot to Pause**, and show **View → Code**.
   Start the ISO from the game list.

For metadata-only debugging of the ISO's original executable, leave replacement
disabled instead. That is safe only if the ELF addresses match the running DOL.
See the [sidecar limitations](README.md#sidecar-elf).

## Set a source breakpoint and step

1. At the initial pause, the Code pane may show startup disassembly. In its
   **Symbols** list, search for and select a function with debug information.
   Dolphin opens the corresponding **Source** tab when it can resolve the file.
2. Click the gutter beside an executable source line to toggle its breakpoint.
   Lines without an address mapping cannot provide a breakpoint.
3. Click **Play** and perform the game action that reaches the breakpoint.
4. When paused in **Source**, use **Step** to enter calls, **Step Over** to skip
   over calls, or **Step Out** to return to the caller. Step and Step Over use
   source rows when the Source tab is active; in **Disassembly** they step by
   instruction instead. Code without a source row falls back to instruction stepping.
5. Inspect the Code pane's **Callstack** and variable tree. Typed locals and globals
   are available for the top frame; older frames do not expose their locals.
   Click **Play** to resume, or **Stop** to end emulation.

If a function stays in Disassembly, check the ELF's debug information and source
roots. If Dolphin warns that a DWARF line is outside the loaded source document,
rebuild the ELF from that checkout before continuing; do not treat the fallback
disassembly as confirmation that the source and executable agree.
