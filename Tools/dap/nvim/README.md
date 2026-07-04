# Neovim + Dolphin DAP

Dolphin **is** the DAP adapter — there is no separate debug adapter binary. Neovim
connects over TCP (or a Unix socket on Linux) after Dolphin is listening.

## lazy.nvim setup

1. Copy the lazy spec into your Neovim config:

   ```bash
   cp Tools/dap/nvim/lazy-dolphin-dap.lua ~/.config/nvim/lua/plugins/dolphin-dap.lua
   ```

   Edit `DOLPHIN_DAP_RT` at the top of that file if your checkout path differs.

2. Remove or merge duplicate keymaps from an existing `lua/plugins/nvim-dap.lua`
   (the dolphin plugin spec already registers `<leader>d*` maps).

3. Add per-project settings at the game/decomp root:

   ```bash
   cp Tools/dap/nvim/.dolphin-dap.example.lua /path/to/melee/.dolphin-dap.lua
   # edit iso / elf / dolphin binary paths
   ```

Buffer diagnostics moved to `<leader>ld` so `<leader>d*` is free for DAP (see `dolphin-dap.lua`).

4. `:Lazy sync`, restart Neovim, open a `.c` file in your decomp tree, then:

   - `<leader>dA` — attach to a running Dolphin (manual terminal workflow)
   - `<leader>da` — pick attach / launch configuration
   - `<leader>dc` — attach if needed, then continue (unpause the game)
   - `<leader>du` — toggle DAP UI
   - `:DolphinDapCmd` — copy a manual terminal launch command (attach workflow)

## Workflows

### Attach (Dolphin already running)

Terminal:

```bash
dolphin-emu-nogui \
  -C Dolphin.General.DAPPort=5678 \
  --debug-elf /path/to/main.elf \
  --exec /path/to/game.iso \
  --platform headless
```

Wait for boot (or `ss -tlnp | grep 5678`), then in Neovim choose **Dolphin attach (:5678)**.

**Important:** `-C` must use the `Dolphin` system prefix (`Dolphin.General.DAPPort=5678`).
`Main.General.DAPPort=5678` is silently ignored and DAP will not listen.

GUI build (same flags, no `--platform headless`):

```bash
dolphin-emu \
  -C Dolphin.General.DAPPort=5678 \
  --debug-elf /path/to/main.elf \
  --exec /path/to/game.iso
```

Or pick **Dolphin attach (Qt spawn, :5678)** in Neovim to start `dolphin-emu` and
connect on the configured port.

Nogui with an emulator window (no Qt UI, but video output):

```bash
dolphin-emu-nogui \
  -C Dolphin.General.DAPPort=5678 \
  --debug-elf /path/to/main.elf \
  --exec /path/to/game.iso \
  --platform x11
```

On Linux use `x11`; on Windows `win32`; on macOS `macos`. Or pick **Dolphin launch
nogui (video, x11)** / **Dolphin attach (nogui video spawn, :5678)** in Neovim.

### Launch (Neovim starts Dolphin)

Requires `iso` in `.dolphin-dap.lua`. Neovim spawns Dolphin with a dynamic DAP port
and connects automatically:

| Config | Binary | Platform |
|--------|--------|----------|
| **Dolphin launch headless** | `dolphin-emu-nogui` | `headless` (no video) |
| **Dolphin launch nogui (video, …)** | `dolphin-emu-nogui` | `x11` / `win32` / `macos` |
| **Dolphin launch (Qt)** | `dolphin-emu` | _(none — full Qt UI)_ |

Set `platform = "x11"` (or `"auto"`) in `.dolphin-dap.lua` to override the default
video backend. Optional `dolphin_gui` overrides the Qt binary path (defaults to
`dolphin` with `-nogui` stripped).

## Source paths / DWARF

For real file:line stack traces, load DWARF via `--debug-elf` (sidecar) or boot a
debug ELF. Source paths in DWARF must match your editor paths — open the decomp
tree so `loadedSources` paths resolve (e.g. `src/melee/gm/foo.c`).

**Note:** line breakpoints in source files still use the disassembly fallback in
Dolphin's `setBreakpoints` handler; instruction breakpoints and DWARF stack/source
views work today. Prefer `<leader>dt` on a line for breakpoints until source
breakpoints are wired through the line table.

## Unix socket (Linux)

Start Dolphin with:

```bash
dolphin-emu-nogui -C Dolphin.General.DAPSocket=/tmp/dolphin-dap.sock --exec /path/to/game.iso
```

Or set in `~/.config/dolphin-emu/Dolphin.ini`:

```ini
[General]
DAPSocket = /tmp/dolphin-dap.sock
```

Set the same path in `.dolphin-dap.lua` as `socket = "/tmp/dolphin-dap.sock"` and
use **Dolphin attach (unix socket)**.

GDB and DAP are mutually exclusive — do not enable `GDBPort` at the same time.
