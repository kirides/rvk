---@diagnostic disable: need-check-nil
-- Sea swim check for risen-enhanced-graphics (docs/research/ocean.md 9):
--
--   .\scripts\run.ps1 -Mode vulkan -Mods rvk_swim -Scenario rvk_swim -WaitForResult -Seconds 600 -Setting 'water.enabled=true'
--
-- New game, start window closed. The hero is put into the open sea off the opening beach (not snapped: the engine makes him
-- swim), the game's own follow camera looks at him; his position is logged every second (`rvk_swim: player ...`: the
-- swim line against the sea level 0), then pictures: `<tag>_follow_b<k>.png` (follow camera, a burst) and
-- `<tag>_water_b<k>.png` (a fixed camera 1 m above the water next to him). Parameters: tag ("swim"), x / z (the spot,
-- default -43500 / -8200, a few metres deep; far out (15 m) the engine puts the hero back on the beach after ~6 s), sea (sea state for the log only), weather ("Sunny_Full"), time (13).
local T = risen.test

local tag = T.param("tag", "swim")
local sx = tonumber(T.param("x", "-43500")) or -43500
local sz = tonumber(T.param("z", "-8200")) or -8200
local hour = tonumber(T.param("time", "13")) or 13
local weather = T.param("weather", "Sunny_Full")

T.plan { "new game started", "swimming", "pictures" }

local hold = nil
risen.on("tick", function() if hold then risen.world.set_time(hold, 0) end end)

local function wait_s(s)
    local t0 = T.elapsed()
    T.wait_until(function() return T.elapsed() - t0 >= s end, s + 60, "wait " .. s .. " s")
end

local function burst(name, n)
    local ok, why = risen.debug.screenshot(name, { burst = n, interval = 8, wait_frames = 1 })
    if not ok then return "refused: " .. tostring(why) end
    T.wait_until(function() return risen.debug.screenshot_status(name) ~= "pending" end, 120, "screenshot " .. name)
    return risen.debug.screenshot_status(name) or "unknown"
end

T.run(function()
    local started = T.wait_event("session.start", 300, function(m) return m == "new" end)
    T.eq("new game started", started, "new")
    wait_s(30)
    pcall(risen.gui.close, 26)
    T.sleep_frames(10)
    pcall(risen.debug.console, "invisible")
    pcall(risen.debug.console, "god")
    hold = hour
    risen.world.set_weather(weather, { thunder = false })
    risen.hud.set_visible(false)
    local player = risen.entity.player()
    -- off the beach first (streams the world part in), then into the sea
    player:set_pose { x = -38559, y = 15, z = -5770, yaw = 238 }
    T.sleep_frames(240)
    player:set_pose { x = sx, y = 0, z = sz, yaw = 238 }
    local function where(i)
        local x, y, z = player:position()
        risen.log.info(("rvk_swim: player %s (%.0f, %.1f, %.0f)"):format(tostring(i), x or 0, y or 0, z or 0))
        return x, y, z
    end
    for i = 1, 4 do
        T.sleep_frames(30)
        where(i)
    end
    local x, y, z = where("before pictures")
    -- the engine keeps a swimming character at a fixed depth below the water level 0 (not on the ground, not back on land)
    T.eq("swimming", y ~= nil and y < -50 and y > -400 and math.abs(x - sx) < 1500, true)
    local st1 = burst(tag .. "_follow", 4)
    where("after the follow pictures")
    local a = math.rad(238)
    risen.camera.set { pos = { x - math.sin(a) * 600, 100, z - math.cos(a) * 600 }, target = { x, (y or 0) + 60, z }, fov = 60, instant = true }
    T.sleep_frames(120)
    local st2 = burst(tag .. "_water", 4)
    where("after the water pictures")
    risen.log.info(("rvk_swim: pictures follow %s, water %s"):format(st1, st2))
    T.eq("pictures", st1 == "done" and st2 == "done", true)
    hold = nil
    pcall(risen.camera.release)
    risen.hud.set_visible(true)
    T.finish()
end, "rvk_swim")
