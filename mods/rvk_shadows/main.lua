-- A fixed view for renderer comparisons (risen-enhanced-graphics, Vulkan vs the game's D3D9 renderer):
--
--   .\scripts\test-run.ps1 -Mods rvk_shadows -WaitForResult -Seconds 240 -Scenario rvk_shadows [-Param hour=12,hold=40,shots=2]
--
-- New game, the start window closed, the clock held at <hour>:00, then the world runs untouched for <hold> seconds (the player
-- stands, the camera looks at the beach). With the D3D9 renderer <shots> screenshots
-- <Risen>\bin\spass\screenshots\rvk_shadows_<i>.png are taken over that time; the Vulkan renderer saves frames itself
-- (risen-enhanced-graphics run.ps1 -Screenshots).
local T = risen.test
local hour = tonumber(T.param("hour", "12")) or 12
local hold = tonumber(T.param("hold", "40")) or 40
local shots = tonumber(T.param("shots", "2")) or 2
T.plan { "new game started", "clock set" }

local function wait_s(s)
    local t0 = T.elapsed()
    T.wait_until(function() return T.elapsed() - t0 >= s end, s + 15, "wait " .. s .. " s")
end

T.run(function()
    local started = T.wait_event("session.start", 240, function(m) return m == "new" end)
    T.eq("new game started", started, "new")
    wait_s(2)
    pcall(risen.gui.close, 26)
    -- the clock is held at <hour>:00 (sun position) for the whole run
    risen.world.set_time(hour, 0)
    risen.on("tick", function() risen.world.set_time(hour, 0) end)
    local h = risen.world.time()
    T.eq("clock set", h, hour)
    for i = 1, shots do
        wait_s(hold / shots)
        local name = ("rvk_shadows_%d"):format(i)
        if pcall(risen.debug.screenshot, name, 8) then
            local t0 = T.elapsed()
            T.wait_until(function()
                return risen.debug.screenshot_status(name) ~= "pending" or T.elapsed() - t0 > 10
            end, 20, "screenshot")
            risen.log.info("rvk_shadows: " .. name .. " " .. tostring(risen.debug.screenshot_status(name)))
        end
    end
    T.finish()
end, "rvk_shadows")
