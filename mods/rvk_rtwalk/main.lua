-- The hero walks through shadows (RT shadows trails / smoothness checks, docs/research/rt-shadows.md 4). New game, then the
-- copy's QuickSave.save is loaded (scripts\run.ps1 -QuickSave <file>); then:
--   mode=walk   the hero walks forward <seconds> s (game time pinned to <hour>), turning by <turn> degrees per second
--   mode=sun    the hero stands, game time runs from <hour> by <rate> game minutes per real second (the sun moves)
-- Parameters: hour (default 16), seconds (20), mode (walk), turn (0), rate (20). Pair with -Screenshots / -Commands.
local T = risen.test
local hour = tonumber(T.param("hour", "16")) or 16
local seconds = tonumber(T.param("seconds", "20")) or 20
local mode = T.param("mode", "walk")
local turn = tonumber(T.param("turn", "0")) or 0
local rate = tonumber(T.param("rate", "20")) or 20
local use_save = T.param("load", "1") ~= "0"          -- 0: stay in the new game (no quicksave)
local pose = T.param("pose", "")                   -- "x;y;z;yaw": the hero is placed there
local weather = T.param("weather", "")             -- e.g. Sunny_Full
local burst = tonumber(T.param("burst", "0")) or 0     -- pictures taken during the walk (risen.debug.screenshot: <name>_b<k>.png in bin\spass\screenshots)
local interval = tonumber(T.param("interval", "6")) or 6  -- frames between them
local name = T.param("name", "walk")
local keys = {}
for k in T.param("keys", "MoveForward"):gmatch("[^,]+") do keys[#keys + 1] = k end
local function press(on) for _, k in ipairs(keys) do risen.input.simulate(k, on) end end
T.plan { "new game started", "savegame loaded", "done" }

local function wait_s(s)
    local t0 = T.elapsed()
    T.wait_until(function() return T.elapsed() - t0 >= s end, s + 30, "wait " .. s .. " s")
end

local frames = 0
risen.on("ui.frame", function() frames = frames + 1 end)

T.run(function()
    local started = T.wait_event("session.start", 300, function(m) return m == "new" end)
    T.eq("new game started", started, "new")
    wait_s(3)
    pcall(risen.gui.close, 26)
    wait_s(1)
    if use_save then
        risen.world.quick_load()
        local m = T.wait_event("world.ready", 300, function(x) return x == "load" end)
        T.eq("savegame loaded", m, "load")
    else
        T.eq("savegame loaded", "load", "load")
    end
    wait_s(5)
    pcall(risen.debug.console, "god")
    risen.hud.set_visible(false)
    local p = risen.entity.player()
    if pose ~= "" then
        local px, py, pz, pyaw = pose:match("([%-%d%.]+);([%-%d%.]+);([%-%d%.]+);([%-%d%.]+)")
        p:set_pose { x = tonumber(px), y = tonumber(py), z = tonumber(pz), yaw = tonumber(pyaw) }
        wait_s(8)
    end
    if weather ~= "" then risen.world.set_weather(weather, { thunder = false }) end
    local x, y, z = p:position()
    risen.log.info(("rvk_rtwalk: hero at %.0f %.0f %.0f, frame %d, mode %s"):format(x, y, z, frames, mode))
    local t = hour * 60
    local clock0 = risen.clock()
    local sunTick = risen.on("tick", function()
        if mode == "sun" then t = t + rate * (1 / 60) end
        local h = (t / 60) % 24
        risen.world.set_time(math.floor(h), math.floor((h % 1) * 60))
    end)
    wait_s(2)
    risen.log.info(("rvk_rtwalk: begins at frame %d"):format(frames))
    if mode == "walk" then
        press(true)
        local t0 = T.elapsed()
        local started = false
        while T.elapsed() - t0 < seconds do
            wait_s(0.5)
            if burst > 0 and not started and T.elapsed() - t0 >= 1 then
                started = true
                local ok, why = risen.debug.screenshot(name, { burst = burst, interval = interval, wait_frames = 1 })
                risen.log.info(("rvk_rtwalk: burst %d x %d frames from frame %d: %s %s"):format(burst, interval, frames, tostring(ok), tostring(why)))
            end
            local px, py, pz = p:position()
            risen.log.info(("rvk_rtwalk: t %.1f frame %d hero %.0f %.0f %.0f state %s"):format(T.elapsed() - t0, frames, px, py, pz, tostring(p:state())))
            press(true)
        end
        if started then T.wait_until(function() return risen.debug.screenshot_status(name) ~= "pending" end, 120, "screenshot " .. name) end
        press(false)
    else
        wait_s(1)
        if burst > 0 then
            local ok, why = risen.debug.screenshot(name, { burst = burst, interval = interval, wait_frames = 1 })
            risen.log.info(("rvk_rtwalk: burst %d x %d frames from frame %d: %s %s"):format(burst, interval, frames, tostring(ok), tostring(why)))
            T.wait_until(function() return risen.debug.screenshot_status(name) ~= "pending" end, 120, "screenshot " .. name)
        end
        wait_s(seconds)
    end
    risen.log.info(("rvk_rtwalk: ends at frame %d"):format(frames))
    T.expect("done", true, "")
    T.finish("rvk_rtwalk")
end, "rvk_rtwalk")
