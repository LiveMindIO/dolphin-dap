--- lazy.nvim plugin spec — copy to ~/.config/nvim/lua/plugins/dolphin-dap.lua
---
--- Adjust `DOLPHIN_DAP_RT` if your dolphin-dap checkout lives elsewhere.

local DOLPHIN_DAP_RT = vim.fn.expand("~/projects/ai/yolo/dolphin-dap/Tools/dap/nvim")

local function setup_dolphin_dap()
  vim.opt.rtp:append(DOLPHIN_DAP_RT)
  require("dolphin-dap").setup()
end

local LazyMapping = require("lib.lazy_mapping")

return {
  {
    "rcarriga/nvim-dap-ui",
    dependencies = {
      "mfussenegger/nvim-dap",
      "nvim-neotest/nvim-nio",
    },
    config = function()
      setup_dolphin_dap()
    end,
  },
  {
    "mfussenegger/nvim-dap",
    keys = function()
      local dap = require("dap")
      return {
        LazyMapping.map("<leader>dc", "Continue / start Dolphin DAP", function()
          dap.continue()
        end),
        LazyMapping.map("<leader>da", "Pick Dolphin DAP configuration", function()
          require("dolphin-dap").run()
        end),
        LazyMapping.map("<leader>dr", "Toggle DAP REPL", function()
          dap.repl.toggle()
        end),
        LazyMapping.map("<leader>du", "Toggle DAP UI", function()
          require("dapui").toggle()
        end),
        LazyMapping.map("<leader>dK", "DAP hover", function()
          require("dap.ui.widgets").hover()
        end),
        LazyMapping.map("<leader>dt", "Toggle DAP breakpoint", function()
          dap.toggle_breakpoint()
        end),
        LazyMapping.map("<leader>dso", "DAP step over", function()
          dap.step_over()
        end),
        LazyMapping.map("<leader>dsi", "DAP step into", function()
          dap.step_into()
        end),
        LazyMapping.map("<leader>dO", "DAP step out", function()
          dap.step_out()
        end),
        LazyMapping.map("<leader>dl", "Re-run last DAP session", function()
          dap.run_last()
        end),
      }
    end,
  },
}
