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

4. `:Lazy sync`, restart Neovim, open a `.c` file, then:

   - `<leader>da` — pick **attach** or **launch**
   - `<leader>dc` — continue / start debugging
   - `<leader>du` — toggle DAP UI

## Workflows

### Attach (Dolphin already running)

Terminal:

```bash
dolphin-emu-nogui \
  -C Main.General.DAPPort=5678 \
  --debug-elf /path/to/main.elf \
  --exec /path/to/game.iso \
  --platform headless
```

Wait for `DAP: waiting for client to connect...`, then in Neovim choose
**Dolphin attach (:5678)**.

### Launch (Neovim starts Dolphin)

Requires `iso` in `.dolphin-dap.lua`. Neovim spawns `dolphin-emu-nogui` with a
dynamic DAP port and connects automatically.

## Source paths / DWARF

For real file:line stack traces, load DWARF via `--debug-elf` (sidecar) or boot a
debug ELF. Source paths in DWARF must match your editor paths — open the decomp
tree so `loadedSources` paths resolve (e.g. `src/melee/gm/foo.c`).

**Note:** line breakpoints in source files still use the disassembly fallback in
Dolphin's `setBreakpoints` handler; instruction breakpoints and DWARF stack/source
views work today. Prefer `<leader>dt` on a line for breakpoints until source
breakpoints are wired through the line table.

## Unix socket (Linux)

```ini
# Dolphin.ini
DAPSocket = /tmp/dolphin-dap.sock
```

Set the same path in `.dolphin-dap.lua` as `socket = "/tmp/dolphin-dap.sock"` and
use **Dolphin attach (unix socket)**.

GDB and DAP are mutually exclusive — do not enable `GDBPort` at the same time.
