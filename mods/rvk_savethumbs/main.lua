-- Savegame thumbnails in the Load / Save pages (docs/research/savegame-thumbnails.md):
--
--   .\scripts\run.ps1 -Mode vulkan -GameDir B:\DevDrive\risen-test-vk-cmp -Instance 3 -Mods rvk_savethumbs `
--       -Scenario rvk_savethumbs -WaitForResult -Seconds 300 [-Param save=rvk_vk]
--
-- New game, the start window closed, then (with save=<name>) an auto save named <name> is written (the thumbnail is taken from
-- the frame on screen: gCSaveGameManager::UpdateThumbnail). Then the Load page (Menu_Load, 15) and the Save page (Menu_Save,
-- 16) are opened and screenshotted (risen.debug.screenshot: <Risen>\bin\spass\screenshots\rvk_savethumbs_<page>.png, either
-- renderer). save= (empty) only lists the savegames already there, e.g. the ones the other renderer made.
local T = risen.test
local save = T.param("save", "")
T.plan { "new game started", "load page opened", "save page opened" }

local function wait_s(s)
    local t0 = T.elapsed()
    T.wait_until(function() return T.elapsed() - t0 >= s end, s + 15, "wait " .. s .. " s")
end

local function shot(name)
    name = "rvk_savethumbs_" .. name
    if pcall(risen.debug.screenshot, name, 3) then
        local t0 = T.elapsed()
        T.wait_until(function() return risen.debug.screenshot_status(name) ~= "pending" or T.elapsed() - t0 > 10 end, 20,
            "screenshot " .. name)
        risen.log.info("rvk_savethumbs: " .. name .. ": " .. tostring(risen.debug.screenshot_status(name)))
    end
end

local function page_visible(id)
    for _, p in ipairs(risen.gui.pages() or {}) do
        if p.id == id then return p.visible end
    end
    return false
end

local function open_page(id)
    risen.gui.open(id)
    wait_s(1.5)
    if not page_visible(id) then
        -- from the main menu (ESC), like a player
        risen.gui.key(0x1B)
        wait_s(1.5)
        risen.gui.open(id)
        wait_s(1.5)
    end
    return page_visible(id)
end

T.run(function()
    local started = T.wait_event("session.start", 240, function(m) return m == "new" end)
    T.eq("new game started", started, "new")
    wait_s(2)
    pcall(risen.gui.close, 26)
    wait_s(4)
    if save ~= "" then
        T.wait_until(function() return risen.world.is_save_allowed() end, 30, "saving allowed")
        local saved = nil
        risen.on("save.after", function(_, ok) saved = ok end)
        risen.world.auto_save(save)
        T.wait_until(function() return saved ~= nil end, 60, "savegame written")
        T.expect("savegame " .. save .. " written", saved == true)
        wait_s(2)
    end
    shot("world")
    T.eq("load page opened", open_page(15), true)
    wait_s(2)
    shot("load")
    risen.gui.close(15)
    wait_s(1)
    T.eq("save page opened", open_page(16), true)
    wait_s(2)
    shot("save")
    risen.gui.close(16)
    wait_s(1)
    T.finish()
end, "rvk_savethumbs")
