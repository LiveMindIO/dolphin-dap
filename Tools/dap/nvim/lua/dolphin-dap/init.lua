--- Dolphin GameCube/Wii DAP integration for nvim-dap.
---
--- Dolphin exposes a DAP server (not a separate adapter binary). Start it with
--- `-C Dolphin.General.DAPPort=5678` (not `Main.…` — that is silently ignored),
--- then attach from Neovim — or let nvim-dap spawn dolphin-emu-nogui with
--- `--platform headless`, nogui + x11/win32 for video, or dolphin-emu (Qt).
---
--- Per-project settings: add `.dolphin-dap.lua` at the repo root, e.g.
---   return {
---     dolphin = "~/projects/dolphin-dap/build/Binaries/dolphin-emu-nogui",
---     iso = "~/roms/GALE01.iso",
---     elf = "~/melee/build/GALE01/main.elf", -- optional sidecar DWARF
---     port = 5678,
---   }

local PROJECT_FILE = ".dolphin-dap.lua"
local DAP_PORT_CONFIG = "Dolphin.General.DAPPort=%s"
local DOLPHIN_FILETYPES = { "c", "cpp" }

local M = {}
M._setup_done = false

local function notify(msg, level)
  vim.notify(msg, level or vim.log.levels.INFO, { title = "dolphin-dap" })
end

--- Walk upward from `start_dir` (or the current buffer's directory) for `.dolphin-dap.lua`.
function M.find_project_config(start_dir)
  if not start_dir or start_dir == "" then
    local bufname = vim.api.nvim_buf_get_name(0)
    if bufname ~= "" then
      start_dir = vim.fn.fnamemodify(bufname, ":h")
    end
    if not start_dir or start_dir == "" then
      start_dir = vim.fn.getcwd()
    end
  end
  local dir = vim.fn.fnamemodify(start_dir, ":p")

  while true do
    local path = vim.fs.joinpath(dir, PROJECT_FILE)
    if vim.fn.filereadable(path) == 1 then
      local chunk, load_err = loadfile(path)
      if not chunk then
        notify("Failed to load " .. path .. ": " .. tostring(load_err), vim.log.levels.ERROR)
        return nil, dir
      end
      local ok, cfg = pcall(chunk)
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
  local dolphin = expand_path(project.dolphin) or "dolphin-emu-nogui"
  local dolphin_gui = expand_path(project.dolphin_gui)
  if not dolphin_gui then
    if dolphin:match("-nogui$") then
      dolphin_gui = (dolphin:gsub("-nogui$", ""))
    else
      dolphin_gui = "dolphin-emu"
    end
  end
  return {
    dolphin = dolphin,
    dolphin_gui = dolphin_gui,
    iso = expand_path(project.iso),
    elf = expand_path(project.elf),
    port = project.port or 5678,
    socket = expand_path(project.socket),
    host = project.host or "127.0.0.1",
    cwd = expand_path(project.cwd),
    platform = project.platform,
    nogui = project.nogui,
  }
end

local function default_nogui_platform()
  if vim.fn.has("win32") == 1 then
    return "win32"
  end
  if vim.fn.has("mac") == 1 or vim.fn.has("macunix") == 1 then
    return "macos"
  end
  return "x11"
end

--- Resolve binary + `--platform` for spawn/launch configs.
--- Qt builds omit `--platform`; nogui builds pass headless, x11, win32, etc.
local function resolve_launch_target(config, project)
  local use_nogui = config.nogui
  if use_nogui == nil then
    use_nogui = config.headless ~= false
  end

  if not use_nogui then
    return { command = project.dolphin_gui, platform = nil }
  end

  local platform = config.platform or project.platform
  if platform == nil or platform == "" then
    platform = config.headless == false and default_nogui_platform() or "headless"
  end
  if platform == "auto" or platform == "video" then
    platform = default_nogui_platform()
  end

  return { command = project.dolphin, platform = platform }
end

local function build_launch_args(project, target, port)
  if not project.iso or project.iso == "" then
    return nil, "`.dolphin-dap.lua` must set `iso` for launch configs"
  end

  local args = {
    "-C",
    string.format(DAP_PORT_CONFIG, port),
    "--exec",
    project.iso,
  }

  if target.platform then
    vim.list_extend(args, { "--platform", target.platform })
  end

  if project.elf and project.elf ~= "" then
    vim.list_extend(args, { "--debug-elf", project.elf })
  end

  return args
end

local function dap_run(config)
  require("dap").run(config)
end

function M.register_adapter()
  local dap = require("dap")

  dap.adapters.dolphin = function(callback, config)
    config = config or {}

    local function merge_project()
      local cfg = M.find_project_config()
      return normalize_project(vim.tbl_extend("force", cfg or {}, config))
    end

    if config.request == "launch" or (config.request == "attach" and config.spawn) then
      local project = merge_project()
      local target = resolve_launch_target(config, project)
      local port = config.request == "launch" and "${port}" or (config.port or project.port)
      local launch_args, err = build_launch_args(project, target, port)
      if not launch_args then
        notify(err, vim.log.levels.ERROR)
        return
      end

      callback({
        type = "server",
        host = project.host,
        port = port,
        id = "dolphin-dap",
        executable = {
          command = target.command,
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
        options = { max_retries = 40 },
      })
      return
    end

    callback({
      type = "server",
      host = config.host or "127.0.0.1",
      port = config.port or 5678,
      id = "dolphin-dap",
      options = {
        -- DAP listens only after the CPU thread starts (post-apploader).
        max_retries = 40,
      },
    })
  end
end

function M.build_configurations(project)
  project = normalize_project(project)

  local video_platform = project.platform == "auto" or project.platform == "video"
      and default_nogui_platform()
    or project.platform
    or default_nogui_platform()

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
      request = "attach",
      name = string.format("Dolphin attach (nogui video spawn, :%d)", project.port),
      spawn = true,
      nogui = true,
      platform = video_platform,
      port = project.port,
      host = project.host,
      iso = project.iso,
      elf = project.elf,
      cwd = project.cwd,
    },
    {
      type = "dolphin",
      request = "attach",
      name = string.format("Dolphin attach (Qt spawn, :%d)", project.port),
      spawn = true,
      nogui = false,
      port = project.port,
      host = project.host,
      iso = project.iso,
      elf = project.elf,
      cwd = project.cwd,
    },
    {
      type = "dolphin",
      request = "launch",
      name = "Dolphin launch headless",
      nogui = true,
      platform = "headless",
      iso = project.iso,
      elf = project.elf,
      cwd = project.cwd,
    },
    {
      type = "dolphin",
      request = "launch",
      name = string.format("Dolphin launch nogui (video, %s)", video_platform),
      nogui = true,
      platform = video_platform,
      iso = project.iso,
      elf = project.elf,
      cwd = project.cwd,
    },
    {
      type = "dolphin",
      request = "launch",
      name = "Dolphin launch (Qt)",
      nogui = false,
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

  return configs
end

local function replace_dolphin_configs(filetype, configs)
  local dap = require("dap")
  local existing = dap.configurations[filetype] or {}
  local kept = vim.tbl_filter(function(entry)
    return entry.type ~= "dolphin"
  end, existing)
  dap.configurations[filetype] = kept
  vim.list_extend(dap.configurations[filetype], configs)
end

function M.register_configurations(project)
  local configs = M.build_configurations(project)
  for _, ft in ipairs(DOLPHIN_FILETYPES) do
    replace_dolphin_configs(ft, configs)
  end
end

function M.ensure_setup(opts)
  if M._setup_done then
    return
  end
  M.setup(vim.tbl_extend("force", { quiet = true }, opts or {}))
end

function M.setup(opts)
  opts = opts or {}
  local cfg = M.find_project_config()
  local project = normalize_project(vim.tbl_extend("force", cfg or {}, opts))

  M.register_adapter()
  M.register_configurations(project)

  local dap = require("dap")
  dap.providers.configs["dolphin-dap"] = function(_)
    local project_cfg = M.find_project_config()
    if not project_cfg then
      return {}
    end
    return M.build_configurations(normalize_project(project_cfg))
  end

  if opts.setup_dap_ui ~= false then
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
      dapui.open()
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

  vim.api.nvim_create_user_command("DolphinDapCmd", function()
    local cmd, err = M.shell_command()
    if not cmd then
      notify(err or "failed to build command", vim.log.levels.ERROR)
      return
    end
    vim.fn.setreg("+", cmd)
    notify("Copied manual launch command to clipboard (+ register)", vim.log.levels.INFO)
    print(cmd)
  end, { desc = "Print/copy dolphin DAP launch command from .dolphin-dap.lua" })

  M._setup_done = true
end

--- Build a shell-ready dolphin command from `.dolphin-dap.lua` (manual attach workflow).
---@param opts? { port?: number, platform?: string, nogui?: boolean, headless?: boolean }
---@return string|nil command
---@return string|nil error
function M.shell_command(opts)
  opts = opts or {}
  local cfg = M.find_project_config()
  local project = normalize_project(vim.tbl_extend("force", cfg or {}, opts))

  local launch_config = {
    nogui = opts.nogui,
    headless = opts.headless,
    platform = opts.platform or project.platform,
  }
  if launch_config.nogui == nil then
    launch_config.nogui = true
    if launch_config.headless == nil then
      launch_config.headless = launch_config.platform == nil
    end
  end

  local target = resolve_launch_target(launch_config, project)
  local port = opts.port or project.port
  local args, err = build_launch_args(project, target, port)
  if not args then
    return nil, err
  end

  local parts = { vim.fn.shellescape(target.command) }
  for _, arg in ipairs(args) do
    table.insert(parts, vim.fn.shellescape(arg))
  end
  return table.concat(parts, " ")
end

--- Continue the active session, or attach if Dolphin is already running.
function M.continue()
  M.ensure_setup()
  local dap = require("dap")
  if dap.session() then
    dap.continue()
    return
  end
  M.attach()
end

--- Attach to a running Dolphin instance (uses `.dolphin-dap.lua` port/host).
function M.attach()
  M.ensure_setup()
  local cfg, dir = M.find_project_config()
  if not cfg then
    notify(
      "no `.dolphin-dap.lua` found (searched from buffer dir and cwd: " .. dir .. ")",
      vim.log.levels.ERROR
    )
    return
  end
  local project = normalize_project(cfg)
  dap_run({
    type = "dolphin",
    request = "attach",
    name = string.format("Dolphin attach (:%d)", project.port),
    port = project.port,
    host = project.host,
  })
end

--- Pick attach vs launch, merging any `.dolphin-dap.lua` on the fly.
function M.run()
  M.ensure_setup()
  local cfg = M.find_project_config()
  if not cfg then
    notify("no `.dolphin-dap.lua` found — create one at your project root", vim.log.levels.ERROR)
    return
  end
  M.register_configurations(normalize_project(cfg))
  local configs = M.build_configurations(normalize_project(cfg))
  if #configs == 1 then
    dap_run(configs[1])
    return
  end
  vim.ui.select(configs, {
    prompt = "Dolphin DAP",
    format_item = function(item)
      return item.name
    end,
  }, function(choice)
    if choice then
      dap_run(choice)
    end
  end)
end

return M
