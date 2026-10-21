-- Camera block check for risen-enhanced-graphics (docs/research/frame-phases.md 5):
--
--   .\scripts\run.ps1 -Mode vulkan -Mods rvk_camera -Scenario rvk_camera -WaitForResult -Seconds 240
--
-- New game, start window closed. Two fixed camera poses (the engine's cinema camera, pos + target), each held for <hold>
-- seconds so that the renderer's 5-second "hal: camera of frame ..." line in rvk.log falls inside it; the scenario logs what
-- it asked for and risen.camera.get() ("rvk_camera: pose ..."). Then a slow flight (instant = false) between two poses: the
-- renderer's "moved ... since the previous frame" must be > 0 while it flies. The comparison itself is done on the logs.
local T = risen.test
local hold = tonumber(T.param("hold", "7")) or 7
T.plan { "new game started", "pose 1 settled", "pose 2 settled", "flight started" }

local function wait_s(s)
    local t0 = T.elapsed()
    T.wait_until(function() return T.elapsed() - t0 >= s end, s + 15, "wait " .. s .. " s")
end

local frames = 0
risen.on("ui.frame", function() frames = frames + 1 end)

local function fmt3(a) return ("(%.2f, %.2f, %.2f)"):format(a[1], a[2], a[3]) end

local function log_pose(name, pos, target)
    local c = risen.camera.get()
    if not c then
        risen.log.info("rvk_camera: " .. name .. " camera.get() = nil")
        return
    end
    local dx, dy, dz = target[1] - pos[1], target[2] - pos[2], target[3] - pos[3]
    local l = math.sqrt(dx * dx + dy * dy + dz * dz)
    risen.log.info(("rvk_camera: %s asked pos %s dir (%.3f, %.3f, %.3f); camera.get pos (%.2f, %.2f, %.2f) dir (%.3f, %.3f, %.3f) fov %s, settled %s, frame %s")
        :format(name, fmt3(pos), dx / l, dy / l, dz / l, c.x, c.y, c.z, c.dx or 0, c.dy or 0, c.dz or 0, tostring(c.fov),
            tostring(c.settled), tostring(risen.debug.frame())))
end

local function set_pose(name, pos, target, instant)
    local ok, why = risen.camera.set { pos = pos, target = target, fov = 60, instant = instant }
    if not ok then return false, why end
    local f0 = frames
    T.wait_until(function() return risen.camera.settled() or frames - f0 >= 600 end, 60, name .. " settles")
    T.sleep_frames(30)
    log_pose(name, pos, target)
    return risen.camera.settled()
end

T.run(function()
    local started = T.wait_event("session.start", 240, function(m) return m == "new" end)
    T.eq("new game started", started, "new")
    wait_s(3)
    pcall(risen.gui.close, 26)
    T.sleep_frames(10)
    local player = risen.entity.player()
    local pp = player and player:pose()
    local P = pp and { pp.x, pp.y, pp.z } or { 0, 0, 0 }
    risen.log.info("rvk_camera: player at " .. fmt3(P))

    local pos1, tgt1 = { P[1] + 600, P[2] + 250, P[3] - 400 }, { P[1], P[2] + 120, P[3] }
    T.eq("pose 1 settled", set_pose("pose 1", pos1, tgt1, true), true)
    wait_s(hold)
    log_pose("pose 1 (after hold)", pos1, tgt1)

    local pos2, tgt2 = { P[1] - 300, P[2] + 900, P[3] + 700 }, { P[1] + 200, P[2], P[3] - 100 }
    T.eq("pose 2 settled", set_pose("pose 2", pos2, tgt2, true), true)
    wait_s(hold)
    log_pose("pose 2 (after hold)", pos2, tgt2)

    -- a flight back to pose 1: the camera moves every frame
    local ok = risen.camera.set { pos = pos1, target = tgt1, fov = 60, instant = false }
    T.eq("flight started", ok, true)
    local f0 = frames
    risen.log.info("rvk_camera: flight started at frame " .. tostring(risen.debug.frame()))
    wait_s(hold)
    local c = risen.camera.get()
    risen.log.info(("rvk_camera: flight after %d frames: camera.get pos (%.2f, %.2f, %.2f), settled %s"):format(frames - f0,
        c and c.x or 0, c and c.y or 0, c and c.z or 0, tostring(c and c.settled)))
    risen.camera.release()
    T.finish()
end, "rvk_camera")
