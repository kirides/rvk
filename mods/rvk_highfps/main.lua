---@diagnostic disable: need-check-nil
-- Does the hero's movement depend on the frame rate? (docs/research/fps-limit.md 6: the climb code clamps its step at
-- 0.0084 s, CheckCharacterGotStuck divides by the frame time). Run it capped and uncapped and compare the numbers:
--
--   .\scripts\run.ps1 -Mode vulkan -Mods rvk_highfps -Scenario rvk_highfps -WaitForResult -Seconds 400 -Vsync off              # AMD Chill profile: 80 fps
--   .\scripts\run.ps1 -Mode vulkan -Mods rvk_highfps -Scenario rvk_highfps -WaitForResult -Seconds 400 -Vsync off -Exe RisenTest.exe
--   ... -MaxFps 60 / 144                                                                                                          # other rates
--
-- New game, start window closed, god mode + invisible. Logged ("rvk_highfps: ..."): the frame time distribution (ui.frame
-- by risen.clock()), then per test the hero's track sampled every tick: a 3 s walk (speed), four running jumps (rise,
-- air time, length), a drop from 15 m (fall time, landing, any rise), a ledge climb (a ledge searched with physics rays
-- around the beach: a blocked ray at knee height, a clear one above 2.3 m; rise, end height, states). Parameters: ledge
-- ("x;y;z;yaw" of a known ledge instead of the search).
local T = risen.test

T.plan { "new game started", "walked", "jumped", "fell", "ledge" }

local player
local function wait_s(s)
    local t0 = T.elapsed()
    T.wait_until(function() return T.elapsed() - t0 >= s end, s + 60, "wait " .. s .. " s")
end

-- frame times (every rendered frame, also while the world is paused)
local last, dts = nil, {}
risen.on("ui.frame", function()
    local now = risen.clock()
    if last then dts[#dts + 1] = now - last end
    last = now
end)
local function frame_stats(label)
    local d = dts
    dts = {}
    if #d < 2 then return end
    table.sort(d)
    local sum = 0
    local fast = 0
    for _, v in ipairs(d) do
        sum = sum + v
        if v < 0.0084 then fast = fast + 1 end
    end
    local function pct(q) return d[math.min(#d, math.max(1, math.floor(q * #d + 0.5)))] * 1000 end
    risen.log.info(("rvk_highfps: frames %s: %d frames, %.1f fps; frame time ms p1 %.2f / p50 %.2f / p95 %.2f / p99 %.2f / max %.2f; below 8.4 ms (the climb clamp): %.0f %%")
        :format(label, #d, #d / sum, pct(0.01), pct(0.5), pct(0.95), pct(0.99), d[#d] * 1000, 100 * fast / #d))
end

-- the hero's track, one sample per tick while `track` is set
local track
local states = {}
risen.on("tick", function()
    if not track or not player then return end
    local x, y, z = player:position()
    local st = player:state()
    track[#track + 1] = { t = risen.clock(), x = x, y = y, z = z, s = st }
    if st and not states[st] then states[st] = true end
end)
local function begin_track() track, states = {}, {} end
local function end_track()
    local tr = track
    track = nil
    local seen = {}
    for s in pairs(states) do seen[#seen + 1] = s end
    table.sort(seen)
    return tr, table.concat(seen, ",")
end
local function summary(tr, y0)
    local maxy, miny, maxstep, maxvy = -1e9, 1e9, 0, 0
    for i, p in ipairs(tr) do
        maxy = math.max(maxy, p.y)
        miny = math.min(miny, p.y)
        if i > 1 then
            local q = tr[i - 1]
            local dt = p.t - q.t
            local step = math.sqrt((p.x - q.x) ^ 2 + (p.y - q.y) ^ 2 + (p.z - q.z) ^ 2)
            maxstep = math.max(maxstep, step)
            if dt > 0 then maxvy = math.max(maxvy, math.abs(p.y - q.y) / dt) end
        end
    end
    return { rise = maxy - y0, drop = y0 - miny, maxstep = maxstep, maxvy = maxvy, n = #tr }
end
local function hold(action, s)
    risen.input.simulate(action, true)
    wait_s(s)
    risen.input.simulate(action, false)
end

local atan2 = math.atan2 or math.atan
local function heading(dx, dz) return math.deg(atan2(dx, dz)) end

-- a ledge in front of point (x, y, z) in world direction `a` (degrees, atan2(dx, dz)): knee-high ray blocked close by,
-- a ray at 2.3 m clear over the same distance + 0.6 m
local function ledge_at(x, y, z, a)
    local r = math.rad(a)
    local dx, dz = math.sin(r), math.cos(r)
    local blocked, dist = risen.world.trace(x, y + 50, z, x + dx * 120, y + 50, z + dz * 120)
    if not blocked or not dist or dist < 20 then return nil end
    local high = risen.world.trace(x, y + 230, z, x + dx * (dist + 60), y + 230, z + dz * (dist + 60))
    if high then return nil end
    local mid = risen.world.trace(x, y + 140, z, x + dx * (dist + 60), y + 140, z + dz * (dist + 60))
    return { dist = dist, tall = mid and true or false }
end

T.run(function()
    local started = T.wait_event("session.start", 300, function(m) return m == "new" end)
    T.eq("new game started", started, "new")
    wait_s(20)
    pcall(risen.gui.close, 26)
    T.sleep_frames(10)
    pcall(risen.debug.console, "god")
    pcall(risen.debug.console, "invisible")
    risen.hud.set_visible(false)
    player = risen.entity.player()
    risen.on("tick", function() risen.world.set_time(12, 0) end)

    -- 1. idle frame times
    frame_stats("loading/start")
    wait_s(5)
    frame_stats("world idle")

    -- 2. walk 3 s
    player:set_pose { x = -38559, y = 15, z = -5770, yaw = 238 }
    T.sleep_frames(120)
    wait_s(2)
    local x0, y0, z0 = player:position()
    begin_track()
    hold("MoveForward", 3)
    wait_s(1)
    local tr = end_track()
    local x1, y1, z1 = player:position()
    local dist = math.sqrt((x1 - x0) ^ 2 + (z1 - z0) ^ 2)
    local walkYaw = heading(x1 - x0, z1 - z0)
    local yawOffset = walkYaw - 238
    local s = summary(tr, y0)
    risen.log.info(("rvk_highfps: walk: %.0f units in 3 s key time (%.0f units/s), heading %.1f (pose yaw 238: offset %.1f), height change %.1f, max step per tick %.1f, %d ticks")
        :format(dist, dist / 3, walkYaw, yawOffset, y1 - y0, s.maxstep, s.n))
    frame_stats("walk")
    T.expect("walked", dist > 300 and math.abs(y1 - y0) < 300, ("%.0f units"):format(dist))

    -- 3. running jumps
    local jumps = {}
    risen.input.simulate("MoveForward", true)
    wait_s(0.6)
    for i = 1, 4 do
        local jx, jy, jz = player:position()
        begin_track()
        hold("MoveUp", 0.15)
        wait_s(1.6)
        local jt, seen = end_track()
        local js = summary(jt, jy)
        local air = 0
        for k = 2, #jt do
            if jt[k].s and jt[k].s:find("Jump") then air = air + (jt[k].t - jt[k - 1].t) end
        end
        local ex, _, ez = player:position()
        jumps[i] = js
        risen.log.info(("rvk_highfps: jump %d: rise %.1f, %.2f s in a Jump state, length %.0f over 1.75 s, max vertical speed %.0f units/s, max step per tick %.1f, %d ticks; states %s")
            :format(i, js.rise, air, math.sqrt((ex - jx) ^ 2 + (ez - jz) ^ 2), js.maxvy, js.maxstep, js.n, seen))
    end
    risen.input.simulate("MoveForward", false)
    frame_stats("jumps")
    local okj = true
    for _, j in ipairs(jumps) do okj = okj and j.rise > 10 and j.rise < 300 end
    T.expect("jumped", okj, ("rises %.0f / %.0f / %.0f / %.0f"):format(jumps[1].rise, jumps[2].rise, jumps[3].rise, jumps[4].rise))

    -- 4. drop from 15 m
    wait_s(1)
    local fx, fy, fz = player:position()
    player:set_pose { x = fx, y = fy + 1500, z = fz }
    local tdrop = risen.clock()
    begin_track()
    wait_s(5)
    local ft, fseen = end_track()
    local fs = summary(ft, fy + 1500)
    local landed
    for k = 2, #ft do
        if not landed and ft[k].y < fy + 30 then landed = ft[k].t - tdrop end
    end
    local _, ly = player:position()
    risen.log.info(("rvk_highfps: drop 1500: landed after %s s at %.1f (start ground %.1f), rise above the drop point %.1f, max vertical speed %.0f units/s, max step per tick %.1f, %d ticks; states %s")
        :format(landed and ("%.2f"):format(landed) or "never", ly, fy, fs.rise, fs.maxvy, fs.maxstep, fs.n, fseen))
    frame_stats("drop")
    T.expect("fell", landed ~= nil and fs.rise < 50 and math.abs(ly - fy) < 300, ("landed %s, end %.0f"):format(tostring(landed), ly - fy))

    -- 5. ledge climb
    local spot
    local lp = T.param("ledge", "")
    if lp ~= "" then
        local a, b, c, d = lp:match("([^;]+);([^;]+);([^;]+);([^;]+)")
        spot = { x = tonumber(a), y = tonumber(b), z = tonumber(c), a = tonumber(d) }
    else
        -- search: a grid around the walk's end point, 16 directions per point
        local cx, _, cz = player:position()
        for ring = 0, 4 do
            for gx = -ring, ring do
                for gz = -ring, ring do
                    if not spot and (math.abs(gx) == ring or math.abs(gz) == ring) then
                        player:set_pose { x = cx + gx * 400, y = fy + 200, z = cz + gz * 400 }
                        T.sleep_frames(40)
                        local px, py, pz = player:position()
                        for k = 0, 15 do
                            local l = not spot and ledge_at(px, py, pz, k * 22.5)
                            if l and l.tall then spot = { x = px, y = py, z = pz, a = k * 22.5, dist = l.dist } end
                        end
                    end
                end
            end
        end
    end
    if not spot then
        risen.log.info("rvk_highfps: ledge: none found around the beach")
        T.expect("ledge", true, "no ledge found (not tested)")
    else
        player:set_pose { x = spot.x, y = spot.y + 10, z = spot.z, yaw = spot.a - yawOffset }
        T.sleep_frames(60)
        local sx, sy, sz = player:position()
        begin_track()
        risen.input.simulate("MoveForward", true)
        wait_s(0.3)
        hold("MoveUp", 0.2)
        wait_s(3)
        risen.input.simulate("MoveForward", false)
        wait_s(1)
        local lt, lseen = end_track()
        local ls = summary(lt, sy)
        local ex, ey, ez = player:position()
        risen.log.info(("rvk_highfps: ledge at (%.0f, %.0f, %.0f) heading %.1f, %.0f units ahead: rise max %.1f, end %.1f, moved %.0f, max vertical speed %.0f units/s, max step per tick %.1f, %d ticks; states %s")
            :format(spot.x, spot.y, spot.z, spot.a, spot.dist or -1, ls.rise, ey - sy, math.sqrt((ex - sx) ^ 2 + (ez - sz) ^ 2), ls.maxvy, ls.maxstep, ls.n, lseen))
        frame_stats("ledge")
        T.expect("ledge", ls.rise < 500 and ey - sy < 400, ("rise %.0f, end %.0f"):format(ls.rise, ey - sy))
    end
    risen.hud.set_visible(true)
    T.finish()
end, "rvk_highfps")
