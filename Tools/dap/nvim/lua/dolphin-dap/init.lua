--- Dolphin GameCube/Wii DAP integration for nvim-dap.
---
--- Dolphin exposes a DAP server (not a separate adapter binary). Start it with
--- DAPPort or DAPSocket, then attach from Neovim — or let nvim-dap spawn
--- dolphin-emu-nogui headless with `--exec` / `--debug-elf`.
---
--- Per-project settings: add `.dolphin-dap.lua` at the repo root, e.g.
---   return {
---     dolphin = "~/projects/dolphin-dap/build/Binaries/dolphin-emu-nogui",
---     iso = "~/roms/GALE01.iso",
---     elf = "~/melee/build/GALE01/main.elf", -- optional sidecar DWARF
---     port = 5678,
---   }

local M = {}

local PROJECT_FILE = ".dolphin-dap.lua"

local function notify(msg, level)
  vim.notify(msg, level or vim.log.levels.INFO, { title = "dolphin-dap" })
end

--- Walk upward from `start_dir` looking for `.dolphin-dap.lua`.
function M.find_project_config(start_dir)
  start_dir = start_dir or vim.fn.getcwd()
  local dir = vim.fn.fnamemodify(start_dir, ":p")

  while true do
    local path = vim.fs.joinpath(dir, PROJECT_FILE)
    if vim.fn.filereadable(path) == 1 then
      local ok, cfg = pcall(dofile, path)
      if not ok then
        notify("Failed to load " .. path .. ": " .. tostring(cfg), vim.log.levels.ERROR)
        return nil, dir
      end
      if type(cfg) ~= "table" then
        notify(path .. " must return a table", vim.log.levels.ERROR)
        return nil, dir
      end
      return cfg, dir
    end

    local parent = vim.fs.dirname(dir)
    if parent == dir then
      break
    end
    dir = parent
  end

  return nil, start_dir
end

local function expand_path(path)
  if path == nil or path == "" then
    return path
  end
  return vim.fn.expand(path)
end

local function normalize_project(project)
  project = project or {}
  return {
    dolphin = expand_path(project.dolphin) or "dolphin-emu-nogui",
    iso = expand_path(project.iso),
    elf = expand_path(project.elf),
    port = project.port or 5678,
    socket = expand_path(project.socket),
    host = project.host or "127.0.0.1",
    cwd = expand_path(project.cwd),
  }
end

local function build_launch_args(project)
  if not project.iso or project.iso == "" then
    return nil, "`.dolphin-dap.lua` must set `iso` for launch configs"
  end

  local args = {
    "-C",
    "Main.General.DAPPort=${port}",
    "--exec",
    project.iso,
    "--platform",
    "headless",
  }

  if project.elf and project.elf ~= "" then
    vim.list_extend(args, { "--debug-elf", project.elf })
  end

  return args
end

function M.register_adapter()
  local dap = require("dap")

  dap.adapters.dolphin = function(callback, config)
    config = config or {}

    if config.request == "launch" then
      local cfg = M.find_project_config()
      local project = normalize_project(vim.tbl_extend("force", cfg or {}, config))
      local launch_args, err = build_launch_args(project)
      if not launch_args then
        notify(err, vim.log.levels.ERROR)
        return
      end

      callback({
        type = "server",
        host = project.host,
        port = "${port}",
        id = "dolphin-dap",
        executable = {
          command = project.dolphin,
          args = launch_args,
          cwd = project.cwd,
          detached = true,
        },
        options = {
          -- ISO boot + apploader can take a few seconds before accept() returns.
          max_retries = 40,
        },
      })
      return
    end

    -- attach
    if config.mode == "pipe" or (config.socket and config.socket ~= "") then
      local socket = expand_path(config.socket)
      if not socket or socket == "" then
        local cfg = M.find_project_config()
        local project = normalize_project(cfg or {})
        socket = project.socket
      end
      if not socket or socket == "" then
        notify("attach via unix socket requires `socket` in config or `.dolphin-dap.lua`", vim.log.levels.ERROR)
        return
      end
      callback({
        type = "pipe",
        pipe = socket,
        id = "dolphin-dap",
        options = { max_retries = 20 },
      })
      return
    end

    callback({
      type = "server",
      host = config.host or "127.0.0.1",
      port = config.port or 5678,
      id = "dolphin-dap",
      options = { max_retries = 20 },
    })
  end
end

function M.register_configurations(project)
  local dap = require("dap")
  project = normalize_project(project)

  local configs = {
    {
      type = "dolphin",
      request = "attach",
      name = string.format("Dolphin attach (:%d)", project.port),
      port = project.port,
      host = project.host,
    },
    {
      type = "dolphin",
      request = "launch",
      name = "Dolphin launch headless",
      dolphin = project.dolphin,
      iso = project.iso,
      elf = project.elf,
      cwd = project.cwd,
    },
  }

  if project.socket and project.socket ~= "" then
    table.insert(configs, 1, {
      type = "dolphin",
      request = "attach",
      name = "Dolphin attach (unix socket)",
      mode = "pipe",
      socket = project.socket,
    })
  end

  for _, ft in ipairs({ "c", "cpp" }) do
    dap.configurations[ft] = dap.configurations[ft] or {}
    vim.list_extend(dap.configurations[ft], configs)
  end
end

function M.setup(opts)
  opts = opts or {}
  local cfg = M.find_project_config()
  local project = normalize_project(vim.tbl_extend("force", cfg or {}, opts))

  M.register_adapter()
  M.register_configurations(project)

  if opts.setup_dap_ui ~= false then
    local dap = require("dap")
    local dapui = require("dapui")

    dapui.setup(opts.dapui or {
      layouts = {
        {
          elements = {
            { id = "scopes", size = 0.25 },
            "breakpoints",
            "stacks",
            "watches",
          },
          size = 0.33,
          position = "left",
        },
        {
          elements = {
            "repl",
            "console",
          },
          size = 0.33,
          position = "bottom",
        },
      },
    })

    local function open_ui_once()
      if not dapui.is_open() then
        dapui.open()
      end
    end

    dap.listeners.after.event_initialized["dolphin-dap-ui"] = open_ui_once
    dap.listeners.after.event_stopped["dolphin-dap-ui"] = open_ui_once
    dap.listeners.before.event_terminated["dolphin-dap-ui"] = function()
      dapui.close()
    end
    dap.listeners.before.event_exited["dolphin-dap-ui"] = function()
      dapui.close()
    end
  end

  if not opts.quiet then
    if project.iso then
      notify(string.format("project ISO: %s", project.iso))
    else
      notify(
        "no `.dolphin-dap.lua` found — add one with at least `iso` (and optional `elf`) for launch",
        vim.log.levels.WARN
      )
    end
  end
end

--- Pick attach vs launch, merging any `.dolphin-dap.lua` on the fly.
function M.run()
  local cfg = M.find_project_config()
  local project = normalize_project(cfg or {})
  M.register_configurations(project)
  require("dap").run()
end

return M
