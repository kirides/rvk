-- Performance at a savegame's place (risen-enhanced-graphics, docs/research/performance.md 8): new game, then the copy's
-- QuickSave.save is loaded (copy the savegame to measure over <test copy>\localappdata\Local\Risen\SaveGames\QuickSave.save
-- first: scripts\run.ps1 -QuickSave <file>), the view is held for <hold> seconds; every 5 s the scenario logs the rendered
-- frame rate (ui.frame) and the hero's position. Pair it with -Perf / -PerfFrame and -Setting 'lod.view=...'.
--
--   .\scripts\run.ps1 -Mode vulkan -Exe RisenTest.exe -Vsync off -Perf -QuickSave <save> -Mods rvk_perfsave -Scenario rvk_perfsave -Param hold=30
local T = risen.test
local hold = tonumber(T.param("hold", "30")) or 30
T.plan { "new game started", "savegame loaded", "held" }

local frames = 0
risen.on("ui.frame", function() frames = frames + 1 end)

local function wait_s(s)
    local t0 = T.elapsed()
    T.wait_until(function() return T.elapsed() - t0 >= s end, s + 30, "wait " .. s .. " s")
end

T.run(function()
    local started = T.wait_event("session.start", 300, function(m) return m == "new" end)
    T.eq("new game started", started, "new")
    wait_s(3)
    pcall(risen.gui.close, 26)
    wait_s(1)
    risen.world.quick_load()
    local mode = T.wait_event("world.ready", 300, function(m) return m == "load" end)
    T.eq("savegame loaded", mode, "load")
    wait_s(5)  -- the loaded world settles (streaming, the post-load wait)
    local p = risen.entity.player()
    if p then
        local x, y, z = p:position()
        risen.log.info(("rvk_perfsave: hero at %.0f %.0f %.0f"):format(x, y, z))
    end
    risen.log.info("rvk_perfsave: hold begins")
    local t0, f0 = T.elapsed(), frames
    local tw, fw = t0, f0
    while T.elapsed() - t0 < hold do
        wait_s(1)
        if T.elapsed() - tw >= 5 then
            risen.log.info(("rvk_perfsave: %.1f fps"):format((frames - fw) / (T.elapsed() - tw)))
            tw, fw = T.elapsed(), frames
        end
    end
    local fps = (frames - f0) / (T.elapsed() - t0)
    risen.log.info(("rvk_perfsave: summary %.1f fps over %.1f s"):format(fps, T.elapsed() - t0))
    T.expect("held", fps > 0, ("%.1f fps"):format(fps))
    T.finish("rvk_perfsave")
end, "rvk_perfsave")
