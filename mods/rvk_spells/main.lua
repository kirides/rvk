---@diagnostic disable: need-check-nil
-- Spell effect check for risen-enhanced-graphics (docs/research/engine-frame/README.md 14: particles, strips, light streaks):
--
--   .\scripts\run.ps1 -Mode vulkan -Mods rvk_spells -Scenario rvk_spells -WaitForResult -Seconds 600 -Setting 'native.scene_ops_compare=true'
--
-- New game, the opening beach at night (time 22, clear weather, the clock held): the hero gets the fire and frost crystals and
-- the skills, and per round draws each spell (the magic ball in his hand: PS_MagicBall_Loop), holds it, shoots it (the projectile,
-- its trail and the impact), then a few engine effects are started on him (risen.effect.start). The camera looks at the hero from
-- the side (`dist`). Parameters: rounds (3), time (22), dist (450), hold (2: seconds per phase), weather ("Sunny_Full").
local T = risen.test

local rounds = tonumber(T.param("rounds", "3")) or 3
local hour = tonumber(T.param("time", "22")) or 22
local dist = tonumber(T.param("dist", "450")) or 450
local holdS = tonumber(T.param("hold", "2")) or 2
local weather = T.param("weather", "Sunny_Full")

T.plan { "new game started", "crystals given", "rounds cast" }

local hold = nil
risen.on("tick", function() if hold then risen.world.set_time(hold, 0) end end)

local function wait_s(s)
    local t0 = T.elapsed()
    T.wait_until(function() return T.elapsed() - t0 >= s end, s + 60, "wait " .. s .. " s")
end

local EFFECTS = { "eff_projectile_fireball_impact_01", "eff_magic_cast_telekinesis_01", "eff_projectile_fireball_damage_01" }

T.run(function()
    local started = T.wait_event("session.start", 300, function(m) return m == "new" end)
    T.eq("new game started", started, "new")
    wait_s(30)
    pcall(risen.gui.close, 26)
    T.sleep_frames(10)
    pcall(risen.debug.console, "god")
    hold = hour
    risen.world.set_weather(weather, { thunder = false })
    risen.hud.set_visible(false)
    local p = risen.entity.player()
    assert(risen.npc.set_stats(p, { skills = { Fireball = 1, Circle = 4 }, mana_max = 2000, mana = 2000 }))
    pcall(risen.npc.set_stats, p, { skills = { Frost = 1 } })
    local spells = {}
    for _, name in ipairs({ "Fireball", "Frost" }) do
        local info = risen.spell.info(name)
        local crystal = info and info.items and info.items.crystal
        if crystal then
            risen.item.give(p, crystal, 1)
            spells[#spells + 1] = crystal
        end
    end
    T.eq("crystals given", #spells, 2)
    local px, py, pz = p:position()
    risen.log.info(("rvk_spells: hero at (%.0f, %.0f, %.0f), crystals %s"):format(px or 0, py or 0, pz or 0, table.concat(spells, ", ")))
    risen.camera.set { pos = { px + dist, py + 180, pz + dist * 0.4 }, target = { px, py + 120, pz }, fov = 60, instant = true }
    local cast = 0
    for r = 1, rounds do
        for _, crystal in ipairs(spells) do
            local cur = risen.spell.equipped(p)
            if cur and cur ~= crystal then risen.item.unequip(p, cur) end
            if p:state() ~= "PS_Normal" then p:set_state("PS_Normal"); T.sleep(60) end
            risen.item.equip(p, crystal)
            p:set_state("PS_Magic")
            local ok = pcall(T.wait_until, function() return p:state() == "PS_MagicBall_Loop" end, 90, "magic ball of " .. crystal)
            risen.log.info(("rvk_spells: round %d %s: magic ball %s, frame %d"):format(r, crystal, tostring(ok), risen.debug.frame and risen.debug.frame() or -1))
            wait_s(holdS)
            p:set_state("PS_MagicBall_Shoot")
            wait_s(holdS)
            pcall(risen.npc.set_stats, p, { mana = 2000 })
            cast = cast + 1
        end
        for _, e in ipairs(EFFECTS) do
            local ok = risen.effect.start(e, p)
            risen.log.info(("rvk_spells: round %d effect %s started %s"):format(r, e, tostring(ok)))
            wait_s(holdS * 0.5)
        end
    end
    T.eq("rounds cast", cast, rounds * #spells)
    hold = nil
    pcall(risen.camera.release)
    risen.hud.set_visible(true)
    T.finish()
end, "rvk_spells")
