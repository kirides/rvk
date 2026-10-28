---@diagnostic disable: need-check-nil
-- Point-light shadow check for risen-enhanced-graphics (docs/research/engine-frame/README.md 13: the packed point-light atlas
-- and the shadow mask's PSSM pass):
--
--   .\scripts\run.ps1 -Mode vulkan -Mods rvk_pointshadow -Scenario rvk_pointshadow -WaitForResult -Seconds 600 -Setting 'native.scene_ops_compare=true'
--
-- The game's lights have neither CastDynamicShadows (static) nor CastShadows (dynamic), so the engine never renders its atlas
-- (README.md 12). This scenario makes one that has: the light spell's item template (It_Cast_Light, an eCPointLight_PS) gets
-- CastShadows = true (and the range `range`), one is spawned next to the hero at night on the opening beach, the camera looks at
-- the hero and the light from `dist`, and the scenario holds `hold` seconds (`-LogFrame` / `-Screenshots` frames in the hold).
-- rvk.log: `rvk_pointshadow: ...` (the light's properties as read back), `native: frame tap [point-light atlas]` with the tap,
-- `native: effect draws` (the mask's cells, its PSSM draws outside the composed units). Parameters: time (22), range (800),
-- hold (20), dist (500), height (120: the light above the ground), weather ("Sunny_Full").
local T = risen.test

local hour = tonumber(T.param("time", "22")) or 22
local range = T.param("range", "800")
local holdS = tonumber(T.param("hold", "20")) or 20
local dist = tonumber(T.param("dist", "500")) or 500
local height = tonumber(T.param("height", "120")) or 120
local weather = T.param("weather", "Sunny_Full")

T.plan { "new game started", "light spawned", "light casts shadows", "held" }

local hold = nil
risen.on("tick", function() if hold then risen.world.set_time(hold, 0) end end)

local function wait_s(s)
    local t0 = T.elapsed()
    T.wait_until(function() return T.elapsed() - t0 >= s end, s + 60, "wait " .. s .. " s")
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
    local px, py, pz = player:position()
    risen.log.info(("rvk_pointshadow: player at (%.0f, %.0f, %.0f)"):format(px or 0, py or 0, pz or 0))
    local t = risen.template.get("It_Cast_Light")
    T.expect("template", t ~= nil, "It_Cast_Light")
    local okShadow = t and t:set_property("eCPointLight_PS", "CastShadows", "true")
    local okRange = t and t:set_property("eCPointLight_PS", "Range", range)
    risen.log.info(("rvk_pointshadow: template CastShadows set %s, Range set %s"):format(tostring(okShadow), tostring(okRange)))
    local light = risen.entity.spawn("It_Cast_Light", px + 150, py + height, pz + 150)
    T.eq("light spawned", light ~= nil, true)
    if light ~= nil then
        risen.log.info(("rvk_pointshadow: light CastShadows %s, Range %s, Enabled %s, Intensity %s"):format(
            tostring(light:get_property("eCPointLight_PS", "CastShadows")), tostring(light:get_property("eCPointLight_PS", "Range")),
            tostring(light:get_property("eCPointLight_PS", "Enabled")), tostring(light:get_property("eCPointLight_PS", "Intensity"))))
        if tostring(light:get_property("eCPointLight_PS", "CastShadows")):lower() ~= "true" then
            light:set_property("eCPointLight_PS", "CastShadows", "true")
        end
        T.eq("light casts shadows", tostring(light:get_property("eCPointLight_PS", "CastShadows")):lower(), "true")
    end
    risen.camera.set { pos = { px - dist, py + 250, pz - dist * 0.3 }, target = { px + 75, py + 60, pz + 75 }, fov = 60, instant = true }
    wait_s(holdS)
    T.eq("held", true, true)
    hold = nil
    pcall(risen.camera.release)
    risen.hud.set_visible(true)
    T.finish()
end, "rvk_pointshadow")
