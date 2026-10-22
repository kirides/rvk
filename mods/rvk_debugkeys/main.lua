-- Debug-key runs of risen-enhanced-graphics (docs/research/debug-mode.md 8):
--
--   .\scripts\run.ps1 -Mode vulkan -TestMode -Mods rvk_debugkeys -Seconds 240 -Commands '700: type minsky','720: key ctrl+insert', ...
--
-- No scenario: the game keeps running until -Seconds or until a key quits it (F12). The mod closes the start window once the
-- new game started ("Press START" keeps the menu input) and logs the camera (risen.camera.get) and the frame number every
-- second, so the log shows whether a free / free-fly camera moved.
local started = false
local frames, last = 0, 0
local function fmt(c)
    if not c then return "nil" end
    return ("pos (%.0f, %.0f, %.0f) yaw %.1f pitch %.1f type %s"):format(c.x or 0, c.y or 0, c.z or 0, c.yaw or 0, c.pitch or 0,
        tostring(c.type))
end

risen.on("session.start", function(mode)
    started = true
    risen.log.info("rvk_debugkeys: session start (" .. tostring(mode) .. ") at frame " .. tostring(risen.debug.frame()))
end)

risen.on("ui.frame", function()
    if not started then return end
    frames = frames + 1
    if frames == 120 then pcall(risen.gui.close, 26) end
    if frames - last >= 60 then
        last = frames
        local ok, c = pcall(risen.camera.get)
        risen.log.info(("rvk_debugkeys: frame %s app_state %s camera %s"):format(tostring(risen.debug.frame()),
            tostring(risen.debug.app_state()), ok and fmt(c) or ("error " .. tostring(c))))
    end
end)
