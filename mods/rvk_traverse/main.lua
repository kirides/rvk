-- WS26 traversal test: the hero is carried along a long route through the world at running speed for minutes, so the engine streams
-- sectors / vegetation / NPCs the way a real run does (rvk_rtwalk's walk blocks after 6 s and never leaves the start area).
-- docs/research/frame-ownership.md 16.15. Deterministic: the position is a function of the test clock (a slow frame skips ahead, as a
-- real run does), the camera turns follow a fixed sine, the game time is pinned.
--
-- New game (or -QuickSave with load=1), then: the hero is put at the route's first point, the world streams in (settle), then every frame
-- he is placed on the route (set_pose at `speed` units/s along the polyline, y = the ground found by the engine's physics ray, yaw =
-- the heading + a sine swing of `swing` degrees with a `period` s period = camera turns). Route progress is logged every `log` s:
--   rvk_traverse: t 12.0 frame 2301 seg 2 dist 7800 pos x z y ground ok/miss
-- and one summary line at the end. `route=` selects a polyline (below); `mode=probe` only walks it fast (speed x4) and logs the ground.
--
-- Parameters: hour (16), speed (650: running; ~900 sprint), seconds (180), route (harbour), swing (35), period (7), log (5),
-- load (0 = new game, 1 = quicksave of the copy), settle (6 s after the first placement), mode (run | probe), loops (1: ping-pong back and forth).
local T = risen.test
local hour = tonumber(T.param("hour", "16")) or 16
local speed = tonumber(T.param("speed", "650")) or 650
local seconds = tonumber(T.param("seconds", "180")) or 180
local route_name = T.param("route", "harbour")
local swing = tonumber(T.param("swing", "35")) or 35
local period = tonumber(T.param("period", "7")) or 7
local log_every = tonumber(T.param("log", "5")) or 5
local use_save = T.param("load", "0") ~= "0"
local settle = tonumber(T.param("settle", "6")) or 6
local mode = T.param("mode", "run")

-- polylines {x, z} (rvkcompare's anchors: spots.lua), y comes from the ground under the hero
local ROUTES = {
    -- the new game's beach -> the harbour town -> the harbour -> west of it -> and all the way back
    harbour = { { -38559, -5770 }, { -22000, -14000 }, { -5588, -22007 }, { -4361, -19659 }, { -13098, -19673 } },
}
local route = ROUTES[route_name] or ROUTES.harbour
T.plan { "new game started", "route walked", "done" }

local frames = 0
risen.on("ui.frame", function() frames = frames + 1 end)

local function wait_s(s)
    local t0 = T.elapsed()
    T.wait_until(function() return T.elapsed() - t0 >= s end, s + 30, "wait " .. s .. " s")
end

-- the polyline as ping-pong: point at distance d (units) along it, back and forth
local seglen, total = {}, 0
for i = 1, #route - 1 do
    local dx, dz = route[i + 1][1] - route[i][1], route[i + 1][2] - route[i][2]
    seglen[i] = math.sqrt(dx * dx + dz * dz)
    total = total + seglen[i]
end
local function point_at(d)
    local cycle = d % (2 * total)
    local dir = 1
    if cycle > total then cycle = 2 * total - cycle; dir = -1 end
    local i = 1
    while i < #seglen and cycle > seglen[i] do cycle = cycle - seglen[i]; i = i + 1 end
    local a, b = route[i], route[i + 1]
    local f = seglen[i] > 0 and cycle / seglen[i] or 0
    local x, z = a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f
    local heading = math.deg((math.atan2 or math.atan)(b[1] - a[1], b[2] - a[2])) % 360   -- yaw 0 = +z, 90 = +x
    if dir < 0 then heading = (heading + 180) % 360 end
    return x, z, heading, i
end

local last_y = nil
local misses = 0
local function ground(x, z, y0)
    for _, span in ipairs { 400, 3000, 12000 } do
        local ok, blocked, dist, hx, hy, hz = pcall(risen.world.trace, x, y0 + span, z, x, y0 - span, z)
        if ok and blocked then return hy end
    end
    misses = misses + 1
    return nil
end

T.run(function()
    local started = T.wait_event("session.start", 300, function(m) return m == "new" end)
    T.eq("new game started", started, "new")
    wait_s(3)
    pcall(risen.gui.close, 26)
    wait_s(1)
    if use_save then
        risen.world.quick_load()
        T.wait_event("world.ready", 300, function(x) return x == "load" end)
    end
    wait_s(5)
    pcall(risen.debug.console, "god")
    risen.hud.set_visible(false)
    local p = risen.entity.player()
    local x0, z0, h0 = point_at(0)
    p:set_pose { x = x0, y = (route[1][3] or 15) + 50, z = z0, yaw = h0 }
    wait_s(settle)
    local gy = ground(x0, z0, 15)
    last_y = gy or 15
    local clock0 = T.elapsed()
    local tickT = risen.on("tick", function() risen.world.set_time(math.floor(hour), math.floor((hour % 1) * 60)) end)
    risen.log.info(("rvk_traverse: begins at frame %d, route %s (%d points, %.0f units), speed %.0f, %.0f s"):format(frames, route_name, #route, total, speed, seconds))
    local v = (mode == "probe") and speed * 4 or speed
    local nextlog, lastd, maxstep = 0, 0, 0
    local t_prev = T.elapsed()
    while T.elapsed() - clock0 < seconds do
        local t = T.elapsed() - clock0
        local d = t * v
        local x, z, heading, seg = point_at(d)
        local y = ground(x, z, last_y)
        if y then last_y = y + 5 end
        local yaw = (heading + swing * math.sin(2 * math.pi * t / period)) % 360
        p:set_pose { x = x, y = last_y, z = z, yaw = yaw }
        maxstep = math.max(maxstep, d - lastd); lastd = d
        if t >= nextlog then
            nextlog = nextlog + log_every
            risen.log.info(("rvk_traverse: t %.1f frame %d seg %d dist %.0f pos %.0f %.0f %.0f yaw %.0f ground-misses %d"):format(t, frames, seg, d, x, last_y, z, yaw, misses))
        end
        T.sleep_frames(1)
    end
    risen.log.info(("rvk_traverse: ends at frame %d, distance %.0f, largest per-frame step %.0f, ground misses %d"):format(frames, lastd, maxstep, misses))
    pcall(function() risen.off(tickT) end)
    T.eq("route walked", true, true)
    T.expect("done", true, "")
    T.finish("rvk_traverse")
end, "rvk_traverse")
