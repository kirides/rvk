---@diagnostic disable: need-check-nil
-- Water survey for risen-enhanced-graphics (docs/research/water.md):
--
--   .\scripts\run.ps1 -Mode vulkan -Mods rvk_water -Scenario rvk_water -WaitForResult -Seconds 900 [-Param 'only=Levelmesh_Waterfall_0300']
--
-- New game, start window closed. Logs the world bounding box of every water level-mesh entity of the world files
-- (`rvk_water: entity <name> bounds ...`), then visits each one (or the ones in `only`, separated by ';'): the player is put
-- on the navigation mesh next to it (hidden), a fixed camera looks at its centre from `distance` (default: from the box
-- size) and a screenshot `<tag>_<name>.png` is taken (bin\spass\screenshots). The renderer logs the materials it sees
-- ("water: material ..." in rvk.log). Parameters: tag (file prefix, "w"), only, settle (frames before the picture, 300),
-- time (hour, 12), weather ("Sunny_Full").
local T = risen.test

local tag = T.param("tag", "w")
local only = T.param("only", "")
local settle = tonumber(T.param("settle", "300")) or 300
local hour = tonumber(T.param("time", "12")) or 12
local weather = T.param("weather", "Sunny_Full")

-- the water level meshes of the world files (data\common\projects.pak: Levelmesh_Water*.lrent)
local NAMES = { "Levelmesh_Oceanmesh_01", "Levelmesh_Ocean_Shore_01", "Levelmesh_Ocean_Shore_02" }
for i = 1, 9 do NAMES[#NAMES + 1] = ("Levelmesh_Lakemesh_%02d"):format(i) end
for i = 1, 7 do NAMES[#NAMES + 1] = ("Levelmesh_Rivermesh_%02d"):format(i) end
for _, n in ipairs { "0001", "0007", "0100", "0206", "03", "0300", "0302", "0403", "0500", "0605", "0700" } do
    NAMES[#NAMES + 1] = "Levelmesh_Waterfall_" .. n
end
NAMES[#NAMES + 1] = "Levelmesh_Waterflow_0201"
NAMES[#NAMES + 1] = "SP_BD_Waterfall"

local wanted = nil
if only ~= "" then
    wanted = {}
    for n in only:gmatch("[^;,%s]+") do wanted[n] = true end
end

T.plan { "new game started", "survey" }

local function wait_s(s)
    local t0 = T.elapsed()
    T.wait_until(function() return T.elapsed() - t0 >= s end, s + 60, "wait " .. s .. " s")
end

local hold = nil
risen.on("tick", function() if hold then risen.world.set_time(hold, 0) end end)

local function capture(name)
    local ok, why = risen.debug.screenshot(name, { wait_frames = 1 })
    if not ok then return "refused: " .. tostring(why) end
    T.wait_until(function() return risen.debug.screenshot_status(name) ~= "pending" end, 120, "screenshot " .. name)
    return risen.debug.screenshot_status(name) or "unknown"
end

local function visit(name, b)
    local cx, cy, cz = (b[1] + b[4]) / 2, (b[2] + b[5]) / 2, (b[3] + b[6]) / 2
    local sx, sy, sz = b[4] - b[1], b[5] - b[2], b[6] - b[3]
    local player = risen.entity.player()
    -- the player next to the box (streams the world part in), snapped to the navigation mesh, hidden
    local px, py, pz = cx, b[5] + 100, cz - math.min(sz / 2 + 600, 3000)
    player:set_pose { x = px, y = py, z = pz, yaw = 0 }
    T.sleep_frames(120)
    local np = risen.world.nav_point(px, py, pz, 4000)
    if np then player:set_pose { x = np[1], y = np[2], z = np[3], yaw = 0 } end
    T.sleep_frames(30)
    player:enable_rendering(false)
    local size = math.max(sx, sy, sz)
    local dist = math.max(600, math.min(size * 0.6, tonumber(T.param("maxdist", "4000")) or 4000))
    local pitch = sy > math.max(sx, sz) * 0.5 and -5 or -25
    -- camera position + target (the cinema camera does not collide then); from the south, `pitch` degrees down
    local rad = math.rad(-pitch)
    local cam = { cx, b[5] + math.sin(rad) * dist + 150, cz - math.cos(rad) * dist }
    risen.camera.set { pos = cam, target = { cx, b[5], cz }, fov = 60, instant = true }
    T.sleep_frames(settle)
    local c = risen.camera.get()
    local st = capture(tag .. "_" .. name)
    risen.log.info(("rvk_water: visit %s centre (%.0f, %.0f, %.0f) size (%.0f, %.0f, %.0f) camera (%.0f, %.0f, %.0f) pitch %d dist %.0f frame %s shot %s")
        :format(name, cx, cy, cz, sx, sy, sz, c and c.x or 0, c and c.y or 0, c and c.z or 0, pitch, dist, tostring(risen.debug.frame()), st))
    player:enable_rendering(true)
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
    -- extra entities to locate (`find=Don;Mike`): their positions are logged
    for n in T.param("find", ""):gmatch("[^;,%s]+") do
        local e = risen.entity.get(n)
        local x, y, z = nil, nil, nil
        if e then x, y, z = e:position() end
        risen.log.info(("rvk_water: find %s -> %s"):format(n, e and ("(%.0f, %.0f, %.0f)"):format(x or 0, y or 0, z or 0) or "not found"))
    end
    local found = {}
    for _, n in ipairs(NAMES) do
        local e = risen.entity.get(n)
        local b = e and { e:bounds() }
        if b and b[1] then
            risen.log.info(("rvk_water: entity %s bounds (%.0f, %.0f, %.0f) - (%.0f, %.0f, %.0f)"):format(n, b[1], b[2], b[3], b[4], b[5], b[6]))
            found[#found + 1] = { n, b }
        else
            local p = e and { e:position() }
            risen.log.info("rvk_water: entity " .. n .. (e and (" without bounds, position " .. tostring(p and p[1]) .. ", " .. tostring(p and p[2]) .. ", " .. tostring(p and p[3])) or " not found"))
        end
    end
    for _, f in ipairs(found) do
        if not wanted or wanted[f[1]] then
            local ok, err = pcall(visit, f[1], f[2])
            if not ok then risen.log.warn("rvk_water: visit " .. f[1] .. ": " .. tostring(err)) end
        end
    end
    T.eq("survey", #found > 0, true)
    hold = nil
    pcall(risen.camera.release)
    risen.hud.set_visible(true)
    T.finish()
end, "rvk_water")
