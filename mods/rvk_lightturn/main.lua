-- Light set during a camera turn (docs/research/rtgi.md 19):
--
--   .\scripts\run.ps1 -Mode vulkan -RvkLogLevel debug -Mods rvk_lightturn -Scenario rvk_lightturn -WaitForResult -Seconds 240 -Setting 'gi.enabled=true'
--
-- New game, start window closed. The camera stands at the hero's position (eye height) and turns one full circle in <turn>
-- seconds (an instant pose every few frames). rvk.log (debug level) carries one "enh: gi: turn:" line per frame with the
-- drawn objects' light count (the engine culls the lights of what is out of the view) and the GI's world light set.
local T = risen.test
local turn = tonumber(T.param("turn", "24")) or 24
local steps = tonumber(T.param("steps", "180")) or 180
T.plan { "new game started", "turn done" }

local frames = 0
risen.on("ui.frame", function() frames = frames + 1 end)

T.run(function()
    local started = T.wait_event("session.start", 240, function(m) return m == "new" end)
    T.eq("new game started", started, "new")
    local t0 = T.elapsed()
    T.wait_until(function() return T.elapsed() - t0 >= 3 end, 20, "wait 3 s")
    pcall(risen.gui.close, 26)
    T.sleep_frames(10)
    local player = risen.entity.player()
    -- -Param place=x;y;z: where the camera turns (default the monastery hall: candelabras, ~130 static lights loaded around)
    local place = T.param("place", "7100;12549;22040")
    local px, py, pz = place:match("^(-?[%d%.]+);(-?[%d%.]+);(-?[%d%.]+)$")
    if px and player then
        player:set_pose { x = tonumber(px), y = tonumber(py) + 50, z = tonumber(pz), yaw = 0 }
        T.sleep_frames(900) -- the world around streams in
        local np = risen.world.nav_point(tonumber(px), tonumber(py), tonumber(pz), 600)
        if np then player:set_pose { x = np[1], y = np[2], z = np[3], yaw = 0 } end
        T.sleep_frames(300)
    end
    local pp = player and player:pose()
    local P = pp and { pp.x, pp.y + 170, pp.z } or { 0, 0, 0 }
    risen.log.info(("rvk_lightturn: camera at (%.0f, %.0f, %.0f)"):format(P[1], P[2], P[3]))
    local function pose(a)
        local tgt = { P[1] + math.cos(a) * 1000, P[2] - 20, P[3] + math.sin(a) * 1000 }
        return risen.camera.set { pos = P, target = tgt, fov = 60, instant = true }
    end
    pose(0)
    T.sleep_frames(120)
    local per = math.max(1, math.floor(turn * 60 / steps))
    for i = 0, steps do
        pose(i / steps * 2 * math.pi)
        if i % 10 == 0 then risen.log.info(("rvk_lightturn: step %d / %d frame %s"):format(i, steps, tostring(risen.debug.frame()))) end
        T.sleep_frames(per)
    end
    T.eq("turn done", true, true)
    risen.camera.release()
    T.finish()
end, "rvk_lightturn")
