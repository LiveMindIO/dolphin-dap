--- lazy.nvim plugin spec — copy to ~/.config/nvim/lua/plugins/dolphin-dap.lua
---
--- Adjust `DOLPHIN_DAP_RT` if your dolphin-dap checkout lives elsewhere.
--- Do not add a separate `nvim-dap.lua` spec — Lazy merges by plugin id and
--- `enabled = false` there would disable this plugin too.

local DOLPHIN_DAP_RT = vim.fn.expand("~/projects/ai/yolo/dolphin-dap/Tools/dap/nvim")
local rtp_ready = false

--- Lazy keymaps can run before `config`; always call this before `require("dolphin-dap")`.
local function dolphin_dap()
  if not rtp_ready then
    vim.opt.rtp:append(DOLPHIN_DAP_RT)
    rtp_ready = true
  end
  local mod = require("dolphin-dap")
  mod.ensure_setup()
  return mod
end

local LazyMapping = require("lib.lazy_mapping")

return {
  {
    "mfussenegger/nvim-dap",
    name = "dolphin-dap",
    dependencies = {
      "rcarriga/nvim-dap-ui",
      "nvim-neotest/nvim-nio",
    },
    init = function()
      vim.opt.rtp:append(DOLPHIN_DAP_RT)
      rtp_ready = true
    end,
    config = function()
      dolphin_dap()
    end,
    keys = function()
      return {
        LazyMapping.map("<leader>dc", "Continue Dolphin DAP (attach if needed)", function()
          dolphin_dap().continue()
        end),
        LazyMapping.map("<leader>da", "Pick Dolphin DAP configuration", function()
          dolphin_dap().run()
        end),
        LazyMapping.map("<leader>dA", "Attach to running Dolphin", function()
          dolphin_dap().attach()
        end),
        LazyMapping.map("<leader>dr", "Toggle DAP REPL", function()
          require("dap").repl.toggle()
        end),
        LazyMapping.map("<leader>du", "Toggle DAP UI", function()
          require("dapui").toggle()
        end),
        LazyMapping.map("<leader>dK", "DAP hover", function()
          require("dap.ui.widgets").hover()
        end),
        LazyMapping.map("<leader>dt", "Toggle DAP breakpoint", function()
          require("dap").toggle_breakpoint()
        end),
        LazyMapping.map("<leader>dso", "DAP step over", function()
          require("dap").step_over()
        end),
        LazyMapping.map("<leader>dsi", "DAP step into", function()
          require("dap").step_into()
        end),
        LazyMapping.map("<leader>dO", "DAP step out", function()
          require("dap").step_out()
        end),
        LazyMapping.map("<leader>dl", "Re-run last DAP session", function()
          require("dap").run_last()
        end),
      }
    end,
  },
}
