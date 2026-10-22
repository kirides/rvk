# The "Press START" splash (Menu_Start, page 26) on every start

Decision (user, 2026-10-23): no change; stock behaviour.

User report: in windowed mode the Xbox controller "Press START" screen appears on every start of the game.
Method: static RE only (headless Ghidra on Game.dll / Script.dll / Script_Game.dll / Engine.dll, the Ghidra MCP with Risen.exe,
Steam's `appcache\appinfo.vdf`, the test copy's config). No game run. [C] = read in decompiled code / data, [I] = inferred.
All RVAs are image-relative (image base 0x180000000 in the DLLs, 0x140000000 in Risen.exe). Dumps:
`B:\DevDrive\risen-ghidra\pressstart_*.txt`, `pc_*.txt` (call sites of the page open / close functions).

## 1. Summary

The splash is stock game behaviour of the **default launch** (no `-keyboardmode` / `-controllermode` on the command line). It is
not tied to windowed vs fullscreen, to focus, or to any saved flag; nothing is persisted that could suppress it on the next start.
It is dismissed by (a) START released on an XInput pad, (b) any key down, or (c) the left mouse button down, each seen by the
page's paint script. Our proxy does not influence the condition (it only forwards messages while the overlay is closed).

## 2. Where it comes from

* The page: `Menu_Start` = `gEHudPage` 26, window `DLG_MENU_START` (1365x768 virtual, `eCGuiPanel2` 500x260 at 432,323 with the
  `eCScriptStatic` text "Press START" (id 1), a `gCModalResultButton` (id 2) and a `gCPageCloseButton`); read from
  `data/compiled/library/compiled_windows.bin`. Page 27 `Menu_Back` is the backdrop (risen-spass docs/RE.md section on pages).
  [C] (risen-spass guitree1.txt probe)
* **Opened by `gCSession::StartGame` (Game.dll 0x9712d0)**, called at start-up from Risen.exe main (RE.md "startup sequence"
  step 9). With `this[0x119] == 0` (no game running: the title) [C]:

  ```
  if (cmdline.Contains("-controllermode") || cmdline.Contains("-keyboardmode")) {   // eCApplication::GetCommandLineA()
      if (Game.ControllerMode option == true) {
          if (!eCInputX360::CheckPadConected() || keyboardmode) { pad active = 0; ControllerMode option = false }
          else pad default index = 0
      }
      gCActionAdmin::ApplyConfiguration();   // vtable +0x110
      gCHudSystem2::Open(13);                // Menu_Main directly
  } else {
      gCHudSystem2::Open(27);                // Menu_Back
      gCHudSystem2::Open(26);                // Menu_Start = "Press START"
  }
  ```
  The saved `Game ControllerMode` value (ConfigUser.xml, `ControllerMode="false"`) is **not** consulted on the no-flag path.
* The flags come from Steam's launch options (appinfo.vdf of app 40300, read-only look): default entry
  `bin\Risen.exe -nosplash`; extra entries "Risen (Force Gamepad Mode)" = `-controllermode`, "Risen (Force Keyboard Mode)" =
  `-keyboardmode`. [C] Risen.exe `FUN_140001fc0` (WinMain-like, 0x1fc0) lowercases the command line and keeps only
  `-keyboardmode` / `-controllermode` and `-nosplash` (it rebuilds the line as `-mnt=mountlist_packed.ini -projectfile=Projects
  -worldfile=World` + those); strings at 0x140025168 / 0x140025188. No user launch options are set for 40300 in this machine's
  Steam `localconfig.vdf` [C], so the default entry runs.
* The scripts `OnControl_Menu_*` have no entry for page 26; the page logic is the **paint script `OnX360Paint_Start`**
  (Script_Game.dll 0x158200), run every paint of the page. [C]

## 3. What dismisses it (OnX360Paint_Start, Script_Game.dll 0x158200)

While `GUI2::IsVisible(26)`:

1. `eCInputX360` is set active (`*pad = 1`; the pad block lives at `eCInputAdmin + 0x5a48`).
2. For pads 0..3 (XInput): if connected and **START (0x1000) was released** -> default pad = that pad, `GUI2::Close(26)`,
   `Close(27)`, `Game ControllerMode` option = true, ApplyConfiguration, `eCInputX360::Clear`, `GUI2::Open(13)`. A START press
   only arms the release (`pad[i+1] = 1`). An idle connected pad does nothing.
3. Else it polls `eCInputAdmin::GetRawKeyState(code)` for scan codes 1..0x200: **any key currently down** (or 0x200 = left mouse
   button) -> pad inactive, `Close(26)`, `Close(27)`, `ControllerMode` config value set to false (written to the user config,
   persisted at exit), ApplyConfiguration, `Open(13)`.
4. Otherwise the page stays.

Raw key state is filled by `eCInputAdmin::OnHandleMessage` (Engine.dll 0x5a3de0) from window messages: `WM_KEYDOWN/SYSKEYDOWN` ->
`DispatchKeyPressed(scan = lParam bits 16..24)`, `WM_KEYUP` -> `DispatchKeyReleased`, `WM_LBUTTONDOWN` -> code 0x200, right 0x201,
middle 0x202, X buttons 0x203/0x204, wheel 0x205/0x206 (`WM_INPUT` is only read for mouse motion). `ResetBufferedInput`
(0x5a3950: zero 0x319 bytes of state) is called by `StartGame` just before the page opens, so keys held earlier do not count.
Gamepad state is `XInputGetState` polled in `eCInputAdmin::UpdateX360` (0x5a3970, from `eCApplication::Run`, `PlayVideo`,
`LoopMessages`) [C]; XInput does not need window focus.

Consequence [I]: the splash waits until a **key/mouse press reaches the game window**. A window that is not focused (windowed run
started in the background, like every `run.ps1` test run, or launched while another window has the foreground) never receives
WM_KEYDOWN, so the splash stays up until the user clicks into the window and presses a key. In exclusive fullscreen the window
always has focus and the first key press after the title appears clears it (a key used earlier to skip the intro videos does not
count: state reset at StartGame), which is probably why it is noticed less there. The click that focuses the window (WM_LBUTTONDOWN) counts as the left button if
the paint script sees it while it is down.

## 4. Windowed vs fullscreen, stock D3D9 vs our Vulkan renderer

* No branch on the window mode in `StartGame`, `OnX360Paint_Start`, `gCHudSystem2::Open/HandlePageChange` or the input path [C].
  The stock D3D9 renderer (passthrough) shows the same page on the same launch. [I] from the code; to be confirmed (section 5).
* Our proxy: `src/overlay/overlay.cpp` `handleMessage` forwards every message to the game's window procedure unmodified while the
  overlay is closed (it only swallows the F10 / F11 hotkeys); when open it blocks keyboard/mouse messages. So it cannot keep the
  splash up unless the overlay is open. `src/hal/vulkan/device.cpp` ("fullscreen requested, running windowed") only changes the
  presentation, not the input path. The early adapter query detour (early-d3d9.md) runs before the window exists. [C]
* The user's `ConfigDefault.xml` has `Engine/Window FullScreen="true"`; windowed needs `FullScreen="false"` in ConfigUser. The
  test copy's ConfigUser has no FullScreen attribute (the test scripts force windowed on their own).
* Test runs: the risen-spass autostart leaves the page open (docs/DEVELOPMENT.md: "close it with `risen.gui.close(26)`"), the
  pages visible in `risen.hud.state()` are `1,26` [C, spass notes].

## 5. How to verify in game

Compare what the pages report and a screenshot at the title, same scenario, three launches (each `-AllowFocus` where a real key
press is wanted):

```powershell
.\scripts\run.ps1 -Mode passthrough -NoAutostart -Mods testdemo -Scenario testdemo -Seconds 90 -Screenshots '450'   # stock D3D9
.\scripts\run.ps1 -Mode vulkan      -NoAutostart ... (same)
```
Look for: the `Menu_Start` panel in the title screenshot of both modes (Vulkan screenshots via `-Screenshots`; D3D9 via
`risen.debug.screenshot`), and `risen.hud.state().pages` containing 26 in both. Then the dismissal: with `-AllowFocus`, send a
real key (or `-Commands '600: key space'`, which posts WM_KEYDOWN/UP: the key is down only between two polls, so prefer a held
key via a spass `risen.gui.key`-style test, or click the window) and check page 26 disappears and page 13 opens in both modes.
Command-line variant: `run-windowed.ps1 -ExtraArgs '-keyboardmode'` (risen-spass `scripts/run-windowed.ps1`; `run.ps1` does not
forward `ExtraArgs` yet, a one-line `$spassArgs.ExtraArgs = ...` addition) must show pages `13` and no 26/27 from the first frame.
Expected result: identical in both renderers; the only variable is whether a key reaches the window.

## 6. Fix options

1. **Steam launch option** (no code): `-keyboardmode` (or start the "Risen (Force Keyboard Mode)" entry). `StartGame` then opens
   Menu_Main directly, resets a saved pad mode to keyboard. Costs the START prompt for pad users (use `-controllermode` for
   those). Verify first that the backdrop (page 27) is not needed on this path (stock "Force Keyboard Mode" users get it as is).
2. **Proxy hook** (opt-in setting, default stock): detour `gCSession::StartGame` (Game.dll export
   `?StartGame@gCSession@@QEAAXXZ`, 0x9712d0; a real function, not a COMDAT getter) and, when no XInput pad is connected, run the
   keyboard branch (or re-enter the original with " -keyboardmode" appended to the stored command line, whose bCString we would
   have to locate via `eCApplication::GetCommandLineA`, an export: do not detour it, it may be a tiny getter). Safer variant: let
   the original run, then if `GUI2::IsVisible(26)` and `XInputGetState` shows no pad, call `GUI2::Close(26)`, `Close(27)`,
   set `ControllerMode` false, ApplyConfiguration (vtable +0x110) and `GUI2::Open(13)` (the exact keyboard branch of
   `OnX360Paint_Start`). Needs the Script.dll `GUI2` exports bound in `engine/symbols.h`.
3. **risen-spass mod** (Lua): on the first `ui.frame`, `risen.gui.close(26)`, `risen.gui.close(27)`, `risen.gui.open(13)`
   (the test scenarios already do the first). Same logic as option 2 without C++, but ties the fix to the spass layer.
