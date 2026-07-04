--- Copy to your decomp / game project root as `.dolphin-dap.lua`.
--- Paths may use `~` or `$HOME`; they are expanded at runtime.

return {
  -- Built dolphin-emu-nogui (must include DAP support from feature/dap-server).
  dolphin = "~/projects/ai/yolo/dolphin-dap/build/Binaries/dolphin-emu-nogui",

  -- Game disc image to boot.
  iso = "~/roms/GALE01.iso",

  -- Sidecar debug ELF for DWARF line info (same build as the running DOL).
  elf = "~/projects/ai/yolo/melee/build/GALE01/main.elf",

  -- TCP port for attach configs (launch uses a dynamic port via nvim-dap).
  port = 5678,

  -- Optional: Unix socket attach on Linux (overrides TCP when set).
  -- socket = "/tmp/dolphin-dap.sock",

  -- Optional: working directory for the launch executable.
  -- cwd = "~/projects/ai/yolo/melee",
}
