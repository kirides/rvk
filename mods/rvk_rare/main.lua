---@diagnostic disable: need-check-nil
-- Rare render paths for risen-enhanced-graphics (docs/research/engine-frame/README.md 16, N5.8 "never met" kinds):
--
--   .\scripts\run.ps1 -Mode vulkan -Mods rvk_rare -Scenario rvk_rare -WaitForResult -Seconds 900 -Setting 'native.scene_ops_compare=true'
--
-- On the opening beach (new game) each step spawns templates in front of the hero, points the camera at them and holds `hold`
-- seconds: streaks (Obj_Streak_Window_Curch_01: the one template with eCLightStreaks_PS, placed nowhere in the world), crystal
-- (Default_CrystalLight_Blue: an eCStaticPointLight_PS with CastDynamicShadows), fog (VFX_EMT_Fog_Ground_01 / VFX_EMT_Fog_Pollon:
-- LightingStyle Particle = lit emitters), fire (VFX_EMT_Magic_FireInferno_Burn_02 / _03 / VFX_EMT_Magic_Inferno_PerSmoke: Instanced emitters), billboards
-- (Obj_LS_Crystal_AntiMagic, Decal_Glow, VFX_BB_Ice_01: blended billboard sets), strip (VFX_EMT_Titan_Strip_01 moved on a
-- circle every tick: strip segments), hit (DoDamage on the hero above a quarter of its hit points: the OverlayHit image effect).
-- rvk.log `rvk_rare: step <name> ...` at each step's start (frame numbers for -LogFrame). Parameters: steps (all, comma list),
-- hold (12), time (22: night, for the lights), dist (450), spacing (600: the steps' distance along x), shots (a screenshot name prefix).
local T = risen.test

local hour = tonumber(T.param("time", "22")) or 22
local holdS = tonumber(T.param("hold", "12")) or 12
local dist = tonumber(T.param("dist", "450")) or 450
local only = T.param("steps", "streaks,crystal,fog,fire,billboards,strip,hit")
local shots = T.param("shots", "") -- a name prefix: a screenshot per step in the hold (risen.debug.screenshot -> bin\spass\screenshots)
local spacing = tonumber(T.param("spacing", "600")) or 600

local steps = {}
for s in only:gmatch("[^,]+") do steps[#steps + 1] = s end
local plan = { "new game started" }
for _, s in ipairs(steps) do plan[#plan + 1] = "step " .. s end
plan[#plan + 1] = "done"
T.plan(plan)

local hold = nil
local mover = nil
local moverCentre = nil
local moverT = 0
risen.on("tick", function()
    if hold then risen.world.set_time(hold, 0) end
    if mover ~= nil and moverCentre ~= nil then
        moverT = moverT + 0.08
        pcall(function() mover:set_pose { x = moverCentre[1] + 120 * math.cos(moverT), y = moverCentre[2] + 40 * math.sin(2 * moverT), z = moverCentre[3] + 120 * math.sin(moverT), yaw = math.deg(moverT) } end)
    end
end)

local function wait_s(s)
    local t0 = T.elapsed()
    T.wait_until(function() return T.elapsed() - t0 >= s end, s + 60, "wait " .. s .. " s")
end

local function spawn(name, x, y, z, props)
    if props then
        local t = risen.template.get(name)
        if t then
            for _, p in ipairs(props) do
                local ok = t:set_property(p[1], p[2], p[3])
                risen.log.info(("rvk_rare: template %s %s.%s = %s: %s"):format(name, p[1], p[2], p[3], tostring(ok)))
            end
        end
    end
    local e = risen.entity.spawn(name, x, y, z)
    risen.log.info(("rvk_rare: spawned %s at (%.0f, %.0f, %.0f): %s"):format(name, x, y, z, tostring(e ~= nil)))
    return e
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
    risen.world.set_weather("Sunny_Full", { thunder = false })
    risen.hud.set_visible(false)
    local player = risen.entity.player()
    local px, py, pz = player:position()
    risen.log.info(("rvk_rare: player at (%.0f, %.0f, %.0f)"):format(px or 0, py or 0, pz or 0))
    local function look(tx, ty, tz)
        risen.camera.set { pos = { tx - dist, ty + 220, tz - dist * 0.3 }, target = { tx, ty + 40, tz }, fov = 60, instant = true }
    end
    for i, s in ipairs(steps) do
        local ox, oz = px + 250 + spacing * i, pz + 250
        local tx, ty, tz = ox, py, oz
        risen.log.info(("rvk_rare: step %s at frame %d"):format(s, risen.debug.frame and risen.debug.frame() or -1))
        if s == "streaks" then
            spawn("Obj_Streak_Window_Curch_01", ox, py + 150, oz)
        elseif s == "crystal" then
            spawn("Default_CrystalLight_Blue", ox, py + 120, oz)
            spawn("Default_CrystalLight_Red", ox + 150, py + 120, oz)
        elseif s == "fog" then
            spawn("VFX_EMT_Fog_Ground_01", ox, py + 20, oz)
            spawn("VFX_EMT_Fog_Pollon", ox + 100, py + 80, oz)
        elseif s == "fire" then
            spawn("VFX_EMT_Magic_FireInferno_Burn_02", ox, py, oz)
            spawn("VFX_EMT_Magic_FireInferno_Burn_03", ox + 120, py, oz)
            spawn("VFX_EMT_Magic_Inferno_PerSmoke", ox - 120, py, oz)
        elseif s == "billboards" then
            spawn("Obj_LS_Crystal_AntiMagic", ox, py + 60, oz)
            spawn("Decal_Glow", ox + 120, py + 100, oz)
            spawn("VFX_BB_Ice_01", ox - 120, py + 100, oz)
        elseif s == "strip" then
            moverCentre = { ox, py + 120, oz }
            mover = spawn("VFX_EMT_Titan_Strip_01", ox, py + 120, oz)
        elseif s == "hit" then
            tx, ty, tz = px, py, pz
            look(tx, ty, tz)
            -- DoDamage (Script_Game 0xd8060) starts OverlayHit when the victim is the player and the damage exceeds a quarter of its
            -- hit points: an NPC attacker (a peasant, far away), god mode off for the hits
            pcall(risen.debug.console, "god")
            local attacker = risen.npc and risen.npc.spawn and risen.npc.spawn("Peasant_1_01", px + 2500, py, pz + 2500, { name = "RVK_Attacker" })
            risen.log.info(("rvk_rare: attacker %s"):format(tostring(attacker ~= nil)))
            for k, dmg in ipairs({ 20, 40, 80, 160 }) do
                local ok, r = pcall(risen.script.call, "DoDamage", attacker or player, player, dmg)
                risen.log.info(("rvk_rare: DoDamage %d on the hero (%d): %s %s"):format(dmg, k, tostring(ok), tostring(r)))
                wait_s(2)
            end
        end
        look(tx, ty, tz)
        if shots ~= "" then
            wait_s(holdS / 2)
            risen.debug.screenshot(shots .. "_" .. s)
            wait_s(holdS / 2)
        else
            wait_s(holdS)
        end
        mover = nil
        T.eq("step " .. s, true, true)
    end
    hold = nil
    pcall(risen.camera.release)
    risen.hud.set_visible(true)
    T.eq("done", true, true)
    T.finish()
end, "rvk_rare")
