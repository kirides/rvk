-- Frame rates in the world, the ESC menu and the character screen, and quick-load times (risen-enhanced-graphics: the menu
-- frame rate limit, docs/research/menu-fps.md). Works with either renderer:
--
--   .\scripts\run.ps1 -Mode vulkan -Mods rvk_menufps -Scenario rvk_menufps -WaitForResult -Seconds 400 [-Param hold=6,loads=3]
--   .\scripts\run.ps1 -NoRenderer -Mods rvk_menufps -Scenario rvk_menufps ...      (the D3D9 reference)
--
-- New game, the start window closed; then per phase (world, ESC menu, world, character screen, world) the rendered frames
-- (ui.frame = eCApplication::DoRender) over <hold> seconds; then a quick save and <loads> quick loads, each timed from the
-- request to load.before / load.after / session.start / world.ready, with the frames rendered in between. One summary line
-- "rvk_menufps: summary ..." at the end (spass log).
local T = risen.test
local hold = tonumber(T.param("hold", "6")) or 6
local loads = tonumber(T.param("loads", "3")) or 3
local plan = { "new game started", "menu opened", "menu closed", "character screen opened", "character screen closed", "quick save" }
for i = 1, loads do plan[#plan + 1] = ("quick load %d"):format(i) end
T.plan(plan)

local frames = 0
risen.on("ui.frame", function() frames = frames + 1 end)

-- event times of the current load (real seconds since the run started) and the frame count at each
local stamps = {}
local function stamp(name)
    stamps[name] = { t = T.elapsed(), f = frames }
end
risen.on("load.before", function() stamp("load.before") end)
risen.on("load.after", function() stamp("load.after") end)
risen.on("session.start", function(mode) if mode == "load" then stamp("session.start") end end)
risen.on("world.ready", function(mode) if mode == "load" then stamp("world.ready") end end)
risen.on("save.after", function() stamp("save.after") end)

local function wait_s(s)
    local t0 = T.elapsed()
    T.wait_until(function() return T.elapsed() - t0 >= s end, s + 30, "wait " .. s .. " s")
end

local function page_visible(id)
    for _, p in ipairs(risen.gui.pages() or {}) do
        if p.id == id then return p.visible end
    end
    return false
end

local results = {}
-- rendered frames per second over s seconds
local function measure(phase, s)
    local f0, t0 = frames, T.elapsed()
    wait_s(s)
    local fps = (frames - f0) / (T.elapsed() - t0)
    results[#results + 1] = ("%s %.1f fps"):format(phase, fps)
    risen.log.info(("rvk_menufps: phase %s: %.1f fps (%d frames in %.2f s)"):format(phase, fps, frames - f0, T.elapsed() - t0))
end

local function character_action()
    for _, a in ipairs(risen.input.actions()) do
        if a:lower():find("character") then return a end
    end
    return nil
end

local function toggle_character(action)
    if action then
        risen.input.simulate(action, true)
        wait_s(0.2)
        risen.input.simulate(action, false)
    elseif page_visible(3) then
        risen.gui.close(3)
    else
        risen.gui.open(3)
    end
end

T.run(function()
    local started = T.wait_event("session.start", 300, function(m) return m == "new" end)
    T.eq("new game started", started, "new")
    wait_s(3)
    pcall(risen.gui.close, 26)
    wait_s(3)
    risen.log.info("rvk_menufps: phase world1 begins")
    measure("world1", hold)

    risen.log.info("rvk_menufps: phase menu begins")
    risen.gui.key(0x1B)
    wait_s(1)
    T.eq("menu opened", page_visible(13), true)
    measure("menu", hold)
    risen.gui.key(0x1B)
    wait_s(1)
    T.eq("menu closed", page_visible(13), false)
    risen.log.info("rvk_menufps: phase world2 begins")
    measure("world2", hold)

    local action = character_action()
    risen.log.info("rvk_menufps: phase character begins")
    toggle_character(action)
    wait_s(1)
    T.eq("character screen opened", page_visible(3), true)
    measure("character", hold)
    toggle_character(action)
    wait_s(1)
    T.eq("character screen closed", page_visible(3), false)
    risen.log.info("rvk_menufps: phase world3 begins")
    measure("world3", hold)

    stamps = {}
    risen.world.quick_save()
    T.wait_until(function() return stamps["save.after"] ~= nil end, 60, "save.after")
    T.expect("quick save", stamps["save.after"] ~= nil)
    wait_s(2)

    local loadTimes = {}
    for i = 1, loads do
        stamps = {}
        local t0, f0 = T.elapsed(), frames
        risen.log.info(("rvk_menufps: quick load %d requested"):format(i))
        risen.world.quick_load()
        T.wait_until(function() return stamps["world.ready"] ~= nil end, 180, "world.ready after quick load")
        local s = stamps
        local function at(name) return s[name] and s[name].t - t0 or -1 end
        local function fr(name) return s[name] and s[name].f - f0 or -1 end
        local total = at("world.ready")
        -- frames rendered between load.before and session.start: the loading screen's
        local loadFrames = (s["session.start"] and s["load.before"]) and (s["session.start"].f - s["load.before"].f) or -1
        local loadSecs = (s["session.start"] and s["load.before"]) and (s["session.start"].t - s["load.before"].t) or -1
        risen.log.info(("rvk_menufps: quick load %d: load.before %.2f s, load.after %.2f s, session.start %.2f s, world.ready %.2f s; "
            .. "frames %d / %d / %d / %d; loading screen %d frames in %.2f s (%.1f fps)"):format(i,
            at("load.before"), at("load.after"), at("session.start"), total,
            fr("load.before"), fr("load.after"), fr("session.start"), fr("world.ready"),
            loadFrames, loadSecs, loadSecs > 0 and loadFrames / loadSecs or 0))
        T.expect(("quick load %d"):format(i), total > 0, ("world.ready after %.2f s"):format(total))
        loadTimes[#loadTimes + 1] = ("%.2f/%.2f"):format(at("session.start"), total)
        wait_s(4)
    end
    risen.log.info("rvk_menufps: summary " .. table.concat(results, ", ") .. "; loads (session.start/world.ready s) "
        .. table.concat(loadTimes, " "))
    T.finish("rvk_menufps")
end, "rvk_menufps")
