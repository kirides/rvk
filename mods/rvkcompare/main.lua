---@diagnostic disable: need-check-nil
-- Scenario `rvkcompare`: the fixed spots of the D3D9-vs-Vulkan reference comparison (docs/research/reference-compare.md sections 5 and 8).
-- Normally driven by scripts\refcompare.ps1 (both renderers, collection, diff report); by hand:
--
--   .\scripts\run.ps1 -GameDir B:\DevDrive\risen-test-vk-cmp -Instance 3 -NoRenderer -Mods rvkcompare -Scenario rvkcompare -WaitForResult -Seconds 1200 -Param tag=d3d9
--   .\scripts\run.ps1 -GameDir B:\DevDrive\risen-test-vk-cmp -Instance 3 -Mode vulkan -Mods rvkcompare -Scenario rvkcompare -WaitForResult -Seconds 1200 -Param tag=vk
--
-- Parameters (-Param key=value,...): tag (file prefix, default "x"), spots (all | id,id,...), mode (run | calibrate), burst (3), interval (10 frames),
-- settle (default frames before the freeze, 600), freeze_wait (frames between freeze and capture, 60), start_wait (seconds after the new game, 30),
-- wind (1 = keep the vegetation wind, default 0: SetVegWindParams 0 0 0 0 so grass and leaves rest), repeat (n: every spot n times in a row,
-- ids <id>_r<k>), pin_time (default 1: risen.debug.set_game_time to a fixed value per spot two frames before the freeze).
--
-- Per spot (spots.lua): weather (forced, thunder off) + clock held each tick -> player placed at the anchor (snapped to the navigation mesh), hidden on request,
-- NPCs around hidden -> fixed camera (risen.camera.set, instant) -> HUD off/on -> settle frames -> time freeze -> GUI page if any -> burst capture with the
-- active backend (rvk in Vulkan mode, the D3D9 Present hook otherwise) -> one log line `RVKCMP {json}` (actual camera, weather, clock, frame, files)
-- -> unfreeze and restore. Files: <Risen>\bin\spass\screenshots\<tag>_<id>_b<k>.png.
--
-- mode=calibrate: per spot the zone weather at the anchor (mode 0 for 240 frames, logged), then pictures cal_<id>_spot (the spot's camera with zone weather)
-- and cal_<id>_y<yaw> (yaw 0/90/180/270 around the player); one `RVKCAL {json}` line per spot with the snapped pose, the zone weights and the cameras.
local T = risen.test
local SPOTS = require("spots")

local tag = T.param("tag", "x")
local mode = T.param("mode", "run")
local only = T.param("spots", "all")
local burst = tonumber(T.param("burst", "3")) or 3
local interval = tonumber(T.param("interval", "10")) or 10
local settle_default = tonumber(T.param("settle", "600")) or 600
local freeze_wait = tonumber(T.param("freeze_wait", "60")) or 60
local start_wait = tonumber(T.param("start_wait", "30")) or 30
local keep_wind = T.param("wind", "0") == "1"
local repeats = tonumber(T.param("repeat", "1")) or 1
local pin_time = T.param("pin_time", "1") == "1"
-- the game time each spot is frozen at (by its position in spots.lua, so a subset of spots gets the same values)
local spot_index = {}
for i, s in ipairs(SPOTS) do spot_index[s.id] = i end

-- ---- the spot list -------------------------------------------------------------------------------------------------------------------------------------
local selected = {}
do
    local want = nil
    if only ~= "all" and only ~= "" then
        want = {}
        for id in only:gmatch("[^,;%s]+") do want[id] = true end
    end
    for _, s in ipairs(SPOTS) do
        if not want or want[s.id] then selected[#selected + 1] = s end
    end
end
local plan = { "new game started" }
for _, s in ipairs(selected) do
    for r = 1, repeats do plan[#plan + 1] = "spot " .. s.id .. (repeats > 1 and ("_r" .. r) or "") end
end
T.plan(plan)

-- ---- helpers ------------------------------------------------------------------------------------------------------------------------------------------
local frames = 0
risen.on("ui.frame", function() frames = frames + 1 end)

-- the clock is held at the current spot's time every tick (ticks stop under the freeze)
local hold_time = nil
risen.on("tick", function()
    if hold_time then risen.world.set_time(hold_time[1], hold_time[2]) end
end)

local function json(v)
    local t = type(v)
    if t == "nil" then return "null" end
    if t == "boolean" then return tostring(v) end
    if t == "number" then
        if v ~= v or v == math.huge or v == -math.huge then return "null" end
        if v == math.floor(v) and math.abs(v) < 1e15 then return string.format("%d", v) end
        return string.format("%.6g", v)
    end
    if t == "string" then
        return '"' .. v:gsub('[%c"\\]', function(c)
            if c == '"' then return '\\"' elseif c == "\\" then return "\\\\" end
            return string.format("\\u%04x", c:byte())
        end) .. '"'
    end
    if t == "table" then
        local n = #v
        if n > 0 or next(v) == nil then
            local parts = {}
            for i = 1, n do parts[i] = json(v[i]) end
            return "[" .. table.concat(parts, ",") .. "]"
        end
        local keys = {}
        for k in pairs(v) do keys[#keys + 1] = tostring(k) end
        table.sort(keys)
        local parts = {}
        for _, k in ipairs(keys) do
            local val = v[k]
            if val == nil then val = v[tonumber(k)] end
            parts[#parts + 1] = json(k) .. ":" .. json(val)
        end
        return "{" .. table.concat(parts, ",") .. "}"
    end
    return json(tostring(v))
end

local function wait_s(s, name)
    local t0 = T.elapsed()
    T.wait_until(function() return T.elapsed() - t0 >= s end, s + 60, name or ("wait " .. s .. " s"))
end

local function page_visible(id)
    for _, p in ipairs(risen.gui.pages() or {}) do
        if p.id == id then return p.visible end
    end
    return false
end

local function round(x, d)
    local m = 10 ^ (d or 3)
    return math.floor(x * m + 0.5) / m
end

local function camera_info()
    local c = risen.camera.get()
    if not c then return nil end
    return { x = round(c.x), y = round(c.y), z = round(c.z), dx = round(c.dx, 6), dy = round(c.dy, 6), dz = round(c.dz, 6), fov = round(c.fov, 4),
             yaw = round(c.yaw, 4), pitch = round(c.pitch, 4), type = c.type, fixed = c.fixed, settled = c.settled }
end

local function weather_info()
    local w = risen.world.weather()
    if not w then return nil end
    local weights = {}
    for k, v in pairs(w.weights or {}) do weights[k] = round(v, 4) end
    return { mode = w.mode, weights = weights, dominant = w.dominant, thunder = w.thunder }
end

local function player_info()
    local p = risen.entity.player()
    local pp = p and p:pose()
    return pp and { x = round(pp.x, 2), y = round(pp.y, 2), z = round(pp.z, 2), yaw = round(pp.yaw, 3) } or nil
end

-- waits (rendered frames) until the screenshot is written; -> status string
local function capture(name, opts)
    local ok, why = risen.debug.screenshot(name, opts)
    if not ok then return "refused: " .. tostring(why) end
    T.wait_until(function() return risen.debug.screenshot_status(name) ~= "pending" end, 120, "screenshot " .. name)
    return risen.debug.screenshot_status(name) or "unknown"
end

-- ---- placement ----------------------------------------------------------------------------------------------------------------------------------------
local hidden = {}

local function restore_hidden()
    for _, e in ipairs(hidden) do pcall(e.enable_rendering, e, true) end
    hidden = {}
end

-- the player at the spot's anchor; -> the snapped position {x, y, z} or nil, reason
local function place_player(spot)
    local player = risen.entity.player()
    if not player then return nil, "no player" end
    local a = spot.player
    -- first jump (the world around streams in), then snap to the navigation mesh and place exactly
    player:set_pose { x = a.x, y = a.y + 50, z = a.z, yaw = a.yaw or 0 }
    T.sleep_frames(90)
    local p = { a.x, a.y, a.z }
    if a.snap ~= false then
        local np = risen.world.nav_point(a.x, a.y, a.z, a.radius or 600)
        if np then p = { np[1], np[2], np[3] } end
    end
    local ok, why = player:set_pose { x = p[1], y = p[2], z = p[3], yaw = a.yaw or 0 }
    if not ok then return nil, why end
    T.sleep_frames(10)
    return p
end

local function apply_hiding(spot)
    restore_hidden()
    local player = risen.entity.player()
    if spot.hide_player and player then
        player:enable_rendering(false)
        hidden[#hidden + 1] = player
    end
    if (spot.hide_npcs or 0) > 0 and player then
        for _, e in ipairs(risen.entity.npcs(player, spot.hide_npcs) or {}) do
            if e ~= player and not e:is_player() then
                if e:enable_rendering(false) then hidden[#hidden + 1] = e end
            end
        end
    end
end

-- the camera spec of a spot (target relative to the spot's anchor, not to the snapped player: the nav-mesh snap can differ
-- by some units between runs; abs = true: absolute); yaw override for calibration
local function camera_spec(spot, _, yaw)
    local a = spot.player
    local p = { a.x, a.y, a.z }
    local c = spot.camera
    local t = c.target or { 0, 150, 0 }
    local target = c.abs and { t[1], t[2], t[3] } or { p[1] + t[1], p[2] + t[2], p[3] + t[3] }
    return { target = target, yaw = yaw or c.yaw or 0, pitch = c.pitch or -5, distance = c.distance or 600, fov = c.fov, instant = true }
end

local function set_camera(spec)
    local ok, why = risen.camera.set(spec)
    if not ok then return false, why end
    local f0 = frames
    T.wait_until(function() return risen.camera.settled() or frames - f0 >= 120 end, 60, "camera settles")
    return true
end

-- camera, hiding and HUD again right before the capture: a far teleport streams in a new world part, which brings back the
-- follow camera (cam_normal), the HUD and the rendering of the player and of NPCs that were not loaded yet
local function pin_view(spot, p)
    apply_hiding(spot)
    risen.hud.set_visible(spot.hud == true)
    local spec = camera_spec(spot, p)
    for _ = 1, 5 do
        set_camera(spec)
        T.sleep_frames(5)
        local c = risen.camera.get()
        if c and c.type == "cam_cinema" and c.settled then return true end
    end
    local c = risen.camera.get()
    return false, "camera type " .. tostring(c and c.type)
end

local function set_weather(spec)
    return risen.world.set_weather(spec, { thunder = false })
end

local function close_gui(spot)
    if spot.gui then
        if spot.gui.key then
            if page_visible(spot.gui.page or 13) then risen.gui.key(spot.gui.key) end
            T.sleep_frames(10)
        end
        if spot.gui.page and page_visible(spot.gui.page) then pcall(risen.gui.close, spot.gui.page) end
        T.sleep_frames(10)
    end
end

local function open_gui(spot)
    local g = spot.gui
    if g.key then
        risen.gui.key(g.key)
    else
        local ok, why = risen.gui.open(g.page)
        if not ok then return false, why end
    end
    T.sleep_frames(30)
    -- the GUI cursor follows the (background) mouse position: park it in a corner
    local cw, ch = risen.gui.client_size()
    risen.gui.mouse("move", (cw or 1600) - 2, (ch or 900) - 2)
    T.sleep_frames(30)
    return page_visible(g.page or 0), "page " .. tostring(g.page) .. " visible " .. tostring(page_visible(g.page or 0))
end

-- ---- one spot -----------------------------------------------------------------------------------------------------------------------------------------
local function run_spot(spot)
    T.section(spot.id)
    local f_start = frames
    hold_time = spot.time or { 12, 0 }
    risen.world.set_time(hold_time[1], hold_time[2])
    local okw, whyw = set_weather(spot.weather or "Sunny_Full")
    if not okw then return false, "weather: " .. tostring(whyw) end
    local p, why = place_player(spot)
    if not p then return false, "player: " .. tostring(why) end
    apply_hiding(spot)
    local okc, whyc = set_camera(camera_spec(spot, p))
    if not okc then return false, "camera: " .. tostring(whyc) end
    risen.hud.set_visible(spot.hud == true)
    T.sleep_frames(spot.settle or settle_default)
    -- re-pin the view (identical spec: a no-op unless the streaming reset something), a few frames for it, then freeze
    local okv, whyv = pin_view(spot, p)
    if not okv then risen.log.warn("rvkcompare: " .. spot.id .. ": " .. tostring(whyv)) end
    T.sleep_frames(10)
    risen.world.set_time(hold_time[1], hold_time[2])
    -- the timed shader elements (sea surface, clouds) are a function of the engine's game time: the same value two frames
    -- before the freeze in every run puts them at the same phase (risen-spass RE.md section 60; reference-compare.md 8.1)
    local pinned = nil
    if pin_time and risen.debug.set_game_time then
        pinned = 20000000 + (spot_index[spot.id] or 0) * 1000000
        local okt, whyt = risen.debug.set_game_time(pinned)
        if not okt then
            risen.log.warn("rvkcompare: set_game_time: " .. tostring(whyt))
            pinned = nil
        end
        T.sleep_frames(2)
    end
    risen.debug.time_freeze(true)
    T.sleep_frames(freeze_wait)
    local gui_note = nil
    if spot.gui then
        local okg, note = open_gui(spot)
        gui_note = note
        if not okg then
            risen.log.warn("rvkcompare: " .. spot.id .. ": GUI " .. tostring(note))
        end
    end
    local name = tag .. "_" .. (spot.run_id or spot.id)
    local f_capture = risen.debug.frame()
    local status = capture(name, { burst = burst, interval = interval, wait_frames = 1 })
    local h, m = risen.world.time()
    local files = {}
    for k = 0, burst - 1 do files[#files + 1] = burst > 1 and (name .. "_b" .. k .. ".png") or (name .. ".png") end
    local timer = risen.debug.timer()
    risen.log.info("RVKCMP " .. json {
        id = spot.run_id or spot.id, tag = tag, backend = risen.debug.screenshot_backend(), status = status, files = files,
        frame = f_capture, frames_spot = frames - f_start, settle = spot.settle or settle_default, freeze_wait = freeze_wait,
        burst = burst, interval = interval, app_state = risen.debug.app_state(),
        camera = camera_info(), weather = weather_info(), time = { h, tonumber(m) }, player = player_info(), anchor = { round(p[1], 2), round(p[2], 2), round(p[3], 2) },
        hud = spot.hud == true, gui = spot.gui and (spot.gui.page or spot.gui.key) or nil, gui_note = gui_note,
        game_ms = timer and timer.game_ms, playing_s = timer and timer.playing_s, pinned_game_ms = pinned,
    })
    close_gui(spot)
    risen.debug.time_freeze(false)
    risen.hud.set_visible(true)
    restore_hidden()
    return status == "done", status
end

-- ---- calibration of one spot --------------------------------------------------------------------------------------------------------------------------
local function calibrate_spot(spot)
    T.section("calibrate " .. spot.id)
    hold_time = spot.time or { 12, 0 }
    risen.world.set_time(hold_time[1], hold_time[2])
    set_weather("auto")
    local p, why = place_player(spot)
    if not p then return false, "player: " .. tostring(why) end
    apply_hiding(spot)
    risen.hud.set_visible(false)
    set_camera(camera_spec(spot, p))
    T.sleep_frames(240)
    local zone = weather_info()
    pin_view(spot, p)
    risen.hud.set_visible(false)
    T.sleep_frames(10)
    local shots = {}
    local st = capture("cal_" .. spot.id .. "_spot", { wait_frames = 1 })
    shots[#shots + 1] = { name = "cal_" .. spot.id .. "_spot", status = st, camera = camera_info() }
    for _, yaw in ipairs(spot.calib or { 0, 90, 180, 270 }) do
        set_camera(camera_spec(spot, p, yaw))
        apply_hiding(spot)
        T.sleep_frames(20)
        local n = ("cal_%s_y%03d"):format(spot.id, yaw)
        shots[#shots + 1] = { name = n, status = capture(n, { wait_frames = 1 }), camera = camera_info() }
    end
    risen.log.info("RVKCAL " .. json {
        id = spot.id, anchor = spot.player, snapped = { round(p[1], 2), round(p[2], 2), round(p[3], 2) }, player = player_info(),
        zone_weather = zone, shots = shots, time = { risen.world.time() },
    })
    risen.hud.set_visible(true)
    restore_hidden()
    return true, "zone " .. json(zone and zone.weights)
end

-- ---- the run ------------------------------------------------------------------------------------------------------------------------------------------
T.run(function()
    local started = T.wait_event("session.start", 300, function(m) return m == "new" end)
    T.eq("new game started", started, "new")
    wait_s(start_wait, "intro")
    pcall(risen.gui.close, 26)
    T.sleep_frames(10)
    -- monsters ignore the player; no vegetation sway (rest pose: the same in every run)
    pcall(risen.debug.console, "invisible")
    pcall(risen.debug.console, "god")
    if not keep_wind then pcall(risen.debug.console, "SetVegWindParams", 0, 0, 0, 0) end
    risen.log.info("RVKCMP_RUN " .. json { tag = tag, mode = mode, spots = #selected, burst = burst, interval = interval, settle = settle_default,
        freeze_wait = freeze_wait, wind = keep_wind, frame = risen.debug.frame() })
    for _, s in ipairs(selected) do for r = 1, repeats do
        -- repeat=n takes every spot n times in a row (ids <id>_r<k>): the run-to-run noise of one game session
        local spot = setmetatable({ run_id = s.id .. (repeats > 1 and ("_r" .. r) or "") }, { __index = s })
        local okr, res, detail = pcall(mode == "calibrate" and calibrate_spot or run_spot, spot)
        if not okr then
            -- an error inside a spot fails that spot, not the run
            T.expect("spot " .. spot.run_id, false, "error: " .. tostring(res))
            pcall(risen.debug.time_freeze, false)
            pcall(restore_hidden)
        else
            T.expect("spot " .. spot.run_id, res, tostring(detail))
        end
    end end
    hold_time = nil
    pcall(risen.camera.release)
    pcall(risen.world.set_weather, "auto")
    T.finish()
end, "rvkcompare")
