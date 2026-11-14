-- Shadow fit (shadows.fit sphere vs sdsm) under fast camera motion, sun sweeps and camera cuts (docs/research/shadows.md 8.10):
--
--   .\scripts\run.ps1 -Mode full -Mods rvk_sdsmtest -Scenario rvk_sdsmtest -WaitForResult -Seconds 300 -Setting 'shadows.enabled=true','shadows.fit=sdsm' `
--       -Param mode=turn,spot=shadow_inland_b,dps=180,burst=30
--
-- Parameters: mode = turn | sun | cut | hold (default turn); spot = shadow_inland_b | harbour_docks (default shadow_inland_b);
--   dps (turn: degrees per second of yaw, converted at <fps> frames per second = 60 to a fixed step per rendered frame, so the pose
--   delta per frame does not depend on the real frame rate), pitch_dps (0: a pitch wobble of 0.3 rad amplitude at that rate), rate (sun:
--   game minutes per second, 60), burst (frames of the picture burst, 30), interval (1), settle (frames after the placement, 400),
--   lead (frames of motion before the burst starts, 60), seconds (sun / hold: run length, 12), name (picture prefix, default <mode>).
-- Pictures: <Risen>\bin\spass\screenshots\<name>_b<k>.png. The hero and the NPCs are hidden, the HUD off, the weather and the clock pinned
-- like the rvkcompare spot (the sun sweep releases the clock). The camera is risen.camera.set (instant) once per frame.
-- mode=cut: the hero and the camera jump between the two spots every <every> frames (180); a burst of <burst> frames starts on the
-- frame of each jump (picture k = k frames after the cut), <cuts> jumps (4).
local T = risen.test
local mode = T.param("mode", "turn")
local spot_id = T.param("spot", "shadow_inland_b")
local dps = tonumber(T.param("dps", "180")) or 180
local pitch_dps = tonumber(T.param("pitch_dps", "0")) or 0
local fps = tonumber(T.param("fps", "60")) or 60
local rate = tonumber(T.param("rate", "60")) or 60
local burst = tonumber(T.param("burst", "30")) or 30
local interval = tonumber(T.param("interval", "1")) or 1
local settle = tonumber(T.param("settle", "400")) or 400
local lead = tonumber(T.param("lead", "60")) or 60
local seconds = tonumber(T.param("seconds", "12")) or 12
local every = tonumber(T.param("every", "180")) or 180
local cuts = tonumber(T.param("cuts", "4")) or 4
local name = T.param("name", mode)

local SPOTS = {
    shadow_inland_b = { x = -38559, y = 15, z = -5770, yaw = 58, cam = { yaw = 58, pitch = -4, distance = 300 }, time = { 13, 0 }, weather = "Sunny_Full" },
    harbour_docks = { x = -5588, y = 245, z = -22007, yaw = 190, cam = { yaw = 180, pitch = -8, distance = 900 }, time = { 18, 30 }, weather = "Sunny_Half_02" },
}
local spot = SPOTS[spot_id]
T.plan { "new game started", "placed", "done" }

local frames = 0
risen.on("ui.frame", function() frames = frames + 1 end)

local function wait_s(s)
    local t0 = T.elapsed()
    T.wait_until(function() return T.elapsed() - t0 >= s end, s + 60, "wait " .. s .. " s")
end

local hold_time = spot.time
local pin_clock = true

local function place(sp)
    local player = risen.entity.player()
    player:set_pose { x = sp.x, y = sp.y + 50, z = sp.z, yaw = sp.yaw }
    T.sleep_frames(90)
    local p = { sp.x, sp.y, sp.z }
    local np = risen.world.nav_point(sp.x, sp.y, sp.z, 600)
    if np then p = { np[1], np[2], np[3] } end
    player:set_pose { x = p[1], y = p[2], z = p[3], yaw = sp.yaw }
    T.sleep_frames(10)
    return p
end

local function hide()
    local player = risen.entity.player()
    player:enable_rendering(false)
    for _, e in ipairs(risen.entity.npcs(player, 2500) or {}) do
        if e ~= player and not e:is_player() then e:enable_rendering(false) end
    end
    risen.hud.set_visible(false)
end

-- the orbit camera of a spot (rvkcompare's spec)
local function aim(sp, p)
    return risen.camera.set { target = { sp.x, sp.y + 170, sp.z }, yaw = sp.cam.yaw, pitch = sp.cam.pitch, distance = sp.cam.distance, instant = true }
end

local function start_burst(tag)
    local ok, why = risen.debug.screenshot(tag, { burst = burst, interval = interval, wait_frames = 1 })
    risen.log.info(("rvk_sdsmtest: burst %s %d x %d from frame %d (render frame %s): %s %s"):format(tag, burst, interval, frames, tostring(risen.debug.frame()), tostring(ok), tostring(why)))
    return tag
end

local function wait_burst(tag)
    T.wait_until(function() return risen.debug.screenshot_status(tag) ~= "pending" end, 180, "screenshot " .. tag)
    risen.log.info(("rvk_sdsmtest: burst %s: %s"):format(tag, tostring(risen.debug.screenshot_status(tag))))
end

T.run(function()
    local started = T.wait_event("session.start", 300, function(m) return m == "new" end)
    T.eq("new game started", started, "new")
    wait_s(3)
    pcall(risen.gui.close, 26)
    T.sleep_frames(10)
    risen.on("tick", function()
        if pin_clock then risen.world.set_time(hold_time[1], hold_time[2]) end
    end)
    risen.world.set_weather(spot.weather, { thunder = false })
    local p = place(spot)
    hide()
    aim(spot, p)
    for _ = 1, math.max(1, math.floor(settle / 30)) do
        T.sleep_frames(30)
        hide()
        aim(spot, p)
    end
    T.sleep_frames(30)
    local c = risen.camera.get()
    T.eq("placed", c and c.type or "nil", "cam_cinema")
    -- the orbit pose is fixed now: from here the camera position stays, the direction rotates
    local cx, cy, cz, dx, dy, dz = c.x, c.y, c.z, c.dx, c.dy, c.dz
    local yaw0 = math.atan(dz, dx)
    local pitch0 = math.asin(dy)
    risen.log.info(("rvk_sdsmtest: mode %s spot %s camera (%.0f %.0f %.0f) dir (%.3f %.3f %.3f) frame %d dps %.0f fps %.0f"):format(mode, spot_id, cx, cy, cz, dx, dy, dz, frames, dps, fps))
    local function look(yaw_off, pitch_off)
        local y, pt = yaw0 + yaw_off, math.max(-1.4, math.min(1.4, pitch0 + pitch_off))
        local ddx, ddz, ddy = math.cos(pt) * math.cos(y), math.cos(pt) * math.sin(y), math.sin(pt)
        risen.camera.set { pos = { cx, cy, cz }, target = { cx + ddx * 1000, cy + ddy * 1000, cz + ddz * 1000 }, fov = c.fov or 60, instant = true }
    end

    if mode == "turn" then
        local step = math.rad(dps) / fps
        local pstep = math.rad(pitch_dps) / fps
        local n = 0
        local tag
        local total = lead + burst * interval + 30
        while n < total do
            look(n * step, math.sin(n * pstep * 0.5) * 0.3)
            if n % 15 == 0 then risen.hud.set_visible(false) end
            if n == lead then tag = start_burst(name) end
            n = n + 1
            T.sleep_frames(1)
        end
        risen.log.info(("rvk_sdsmtest: turn ended: %d steps of %.2f deg in %d rendered frames"):format(n, math.deg(step), frames))
        if tag then wait_burst(tag) end
    elseif mode == "sun" then
        local t = hold_time[1] * 60 + hold_time[2]
        local n = 0
        pin_clock = false
        local tag
        while n < seconds * fps do
            if n % 15 == 0 then risen.hud.set_visible(false) end
            t = t + rate / fps
            local h = (t / 60) % 24
            risen.world.set_time(math.floor(h), math.floor((h % 1) * 60))
            if n == lead then tag = start_burst(name) end
            n = n + 1
            T.sleep_frames(1)
        end
        if tag then wait_burst(tag) end
    elseif mode == "cut" then
        local other = SPOTS[spot_id == "harbour_docks" and "shadow_inland_b" or "harbour_docks"]
        local sps = { spot, other }
        local ps = { p, nil }
        -- visit the other spot once (streams its world part in, snaps the position), then back
        hold_time = other.time
        risen.world.set_weather(other.weather, { thunder = false })
        ps[2] = place(other)
        hide(); aim(other, ps[2])
        for _ = 1, math.max(1, math.floor(settle / 30)) do
            T.sleep_frames(30)
            hide(); aim(other, ps[2])
        end
        local tags = {}
        for i = 1, cuts do
            local k = (i % 2) + 1   -- 2 = other first, then back to the first spot
            local sp, pp = sps[k], ps[k]
            k = (i % 2 == 1) and 1 or 2
            sp, pp = sps[k], ps[k]
            local tag = name .. "_c" .. i
            tags[#tags + 1] = start_burst(tag)
            hold_time = sp.time
            risen.world.set_weather(sp.weather, { thunder = false })
            risen.entity.player():set_pose { x = pp[1], y = pp[2], z = pp[3], yaw = sp.yaw }
            aim(sp, pp)
            risen.log.info(("rvk_sdsmtest: cut %d to %s at frame %d"):format(i, k == 1 and spot_id or "other", frames))
            T.sleep_frames(60)
            hide(); aim(sp, pp)
            T.sleep_frames(every - 60)
        end
        for _, tag in ipairs(tags) do wait_burst(tag) end
    else
        T.sleep_frames(seconds * fps)
    end
    risen.log.info(("rvk_sdsmtest: ends at frame %d"):format(frames))
    T.eq("done", true, true)
    risen.camera.release()
    T.finish("rvk_sdsmtest")
end, "rvk_sdsmtest")
