-- The character screen (key C) for renderer comparisons (risen-enhanced-graphics: the GUI font atlases are updated by partial
-- texture locks whenever a new glyph shows up):
--
--   .\scripts\test-run.ps1 -Mods rvk_charscreen -WaitForResult -Seconds 240 -Scenario rvk_charscreen [-Param hold=6,shots=1]
--
-- New game, the start window closed, then the character screen is opened through the engine's "Character" action, held for
-- <hold> seconds while the mouse hovers over a grid of points (tooltips), closed, and opened again. Every step logs the number
-- of rendered frames (ui.frame) so that a renderer's own frame-numbered screenshots can be matched. Last the main menu (ESC). With shots=1 the D3D9
-- renderer takes screenshots <Risen>\bin\spass\screenshots\rvk_charscreen_<step>.png (risen.debug.screenshot; the Vulkan
-- renderer saves frames itself: risen-enhanced-graphics run.ps1 -Screenshots).
local T = risen.test
local hold = tonumber(T.param("hold", "6")) or 6
local shots = T.param("shots", "0") == "1"
T.plan { "new game started", "character screen opened", "character screen closed", "character screen reopened", "main menu opened" }

local frames = 0
risen.on("ui.frame", function() frames = frames + 1 end)

local function wait_s(s)
    local t0 = T.elapsed()
    T.wait_until(function() return T.elapsed() - t0 >= s end, s + 15, "wait " .. s .. " s")
end

local function mark(step)
    risen.log.info(("rvk_charscreen: %s at ui.frame %d, %.1f s"):format(step, frames, T.elapsed()))
    if shots then
        local name = "rvk_charscreen_" .. step
        if pcall(risen.debug.screenshot, name, 2) then
            local t0 = T.elapsed()
            T.wait_until(function()
                return risen.debug.screenshot_status(name) ~= "pending" or T.elapsed() - t0 > 10
            end, 20, "screenshot")
        end
    end
end

local function page_visible(id)
    for _, p in ipairs(risen.gui.pages() or {}) do
        if p.id == id then return p.visible end
    end
    return false
end

-- the action that opens the character screen in the default key map ("C")
local function character_action()
    for _, a in ipairs(risen.input.actions()) do
        if a:lower():find("character") then return a end
    end
    return nil
end

local function press(action)
    risen.input.simulate(action, true)
    wait_s(0.2)
    risen.input.simulate(action, false)
end

local function toggle(action)
    if action then
        press(action)
    elseif page_visible(3) then
        risen.gui.close(3)
    else
        risen.gui.open(3)
    end
end

-- hover over a grid of points in the GUI's virtual 1365 x 768 space (labels, values, skill entries: tooltips appear)
local function hover(tag)
    local cw, ch = risen.gui.client_size()
    cw, ch = cw or 1600, ch or 900
    local n = 0
    for _, vy in ipairs { 150, 250, 350, 450, 550, 650 } do
        for _, vx in ipairs { 200, 450, 700, 950, 1200 } do
            risen.gui.mouse("move", math.floor(vx * cw / 1365), math.floor(vy * ch / 768))
            wait_s(hold / 30)
            n = n + 1
        end
        mark(("%s_row%d"):format(tag, math.floor(n / 5)))
    end
end

T.run(function()
    local started = T.wait_event("session.start", 240, function(m) return m == "new" end)
    T.eq("new game started", started, "new")
    wait_s(2)
    pcall(risen.gui.close, 26)
    wait_s(2)
    local action = character_action()
    risen.log.info("rvk_charscreen: character action " .. tostring(action))
    mark("world")

    toggle(action)
    wait_s(1)
    T.eq("character screen opened", page_visible(3), true)
    mark("open1")
    wait_s(hold / 2)
    mark("open1_wait")
    hover("hover1")
    wait_s(hold / 2)
    mark("open1_end")

    toggle(action)
    wait_s(1.5)
    T.eq("character screen closed", page_visible(3), false)
    mark("closed")

    toggle(action)
    wait_s(1)
    T.eq("character screen reopened", page_visible(3), true)
    mark("open2")
    hover("hover2")
    wait_s(hold)
    mark("open2_end")
    toggle(action)
    wait_s(1)
    -- the main menu (ESC) for a look at the other GUI text
    risen.gui.key(0x1B)
    wait_s(2)
    T.eq("main menu opened", page_visible(13), true)
    mark("menu")
    risen.gui.key(0x1B)
    wait_s(1)
    T.finish()
end, "rvk_charscreen")
