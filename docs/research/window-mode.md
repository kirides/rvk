# Window modes: borderless fullscreen in Vulkan mode

User requirement (2026-10-22): "fullscreen mode should be borderless fullscreen. Resolution == Monitor resolution ==
Automatic borderless frame and "full screen" with positioning at 0,0,W,H". Implemented in `src/engine/window_mode.*`
(Vulkan mode only; passthrough / trace keep the engine's D3D9 behaviour). **[C]** = read in the decompilation (headless
Ghidra dumps `B:\DevDrive\risen-ghidra\rvk_window_dump.txt`, `rvk_window_dump2.txt`, `rvk_window_game_dump.txt`),
**[V]** = verified in the game (test copy, 2 x 2560x1440 monitors at 100 %, AMD), **[I]** = inferred.

## 1. Who owns the window

The engine. `eCApplication` derives from `eCWindow` (`+0x18` HWND, `+0x20` stored `eEWindowMode`); the mode **is** the
window style. Engine.dll RVAs:

| function | does |
|---|---|
| `eCApplication::OnCreateWindow` 0x4c5f90 | `eSCreate` from the config `<Engine><Window Left Top Right Bottom Center FullScreen ...>`: style 0xcf0000, or **0x90000000** (`WS_POPUP \| WS_VISIBLE`) when `FullScreen` is true or the command line has `fullscreen`; `-res=WxH` overrides the size [C]. (A `-fullscreen` switch did not give fullscreen in a test run [V]; the config key does.) |
| `eCApplication::CreateEngine` 0x4c4810 | windowed style 0xcf0000 becomes **0xc80000** (`WS_CAPTION \| WS_SYSMENU`: no resize, no maximise) before `eCWindow::Create`; `+0xe0` = fullscreen flag, `+0xe4` = Center; `eSSetupWindow` (+0xd0: width, height, refresh, adapter) -> `CreateGraphicsSubsystem` 0x4c4330, which needs a display mode of exactly that size and passes `windowed = !fullscreen` into the start parameters [C] |
| `eCWindow::Create` 0x73b340 | `CreateWindowExW` at (0,0), client = config size (`AdjustWindowRectEx`); popup: `WS_EX_TOPMOST`, `ShowCursor(FALSE)` [C] |
| `eCApplication::SetResolutionEx` 0x4c02e0 | **every mode change**: title "RISEN ... (WxH)", `eCWindow::SetWindowMode(mode)`, `SetClientSize(w, h, Center)`, stores the mode desc (`+0x6c4` width, `+0x6c8` height, `+0x6cc` refresh, `+0x6d0` vsync, `+0x6d4` float) and sets the reset flag `+0xc48` [C] |
| callers | `SetResolution` 0x4c0430, `SetFullscreen` 0x4c0280, `ToggleFullscreen` 0x4c01f0 (debug key F2), `ToggleDebugResolution` 0x4c09f0 (Ctrl+F2), Game.dll `gCHudResolutionManager2::ApplyResolution / RevertResolution / ApplyHardResolution` (options menu). **The options menu never changes the window mode**: it passes the current `GetWindowMode()`; fullscreen comes only from the config key, F2, or our command [C] |
| `eCWindow::SetWindowMode` 0x73bb00 | `SetWindowLongPtrA(GWL_STYLE, mode)` + `SetWindowPos(SWP_NOMOVE ...)`; entering popup: `SetTopMost(true)` + hide cursor, leaving: `SetTopMost(false)` + show cursor [C] |
| `eCWindow::SetClientSize` 0x73bd80 | client size via `AdjustWindowRectEx`, centred on the **primary** desktop (`ms_iDesktopWidth/Height`) when Center [C] |
| `eCWindow::GetWindowMode` 0x73bcc0 | from the live style: 0xcf0000 / 0xc80000 / (`WS_POPUP \| WS_VISIBLE`) bits, else the stored `+0x20` [C] |
| `eCApplication::Reset` 0x4c11d0 | in `DoRender` when `+0xc48` is set: modules release, `eCGfxContextAdmin::ToggleNewMode(windowed = mode != popup, ..)` -> device slot ToggleNewMode, modules restore [C] |
| `eCApplication::OnSize` 0x4bf530 | WM_SIZE: picks the GUI aspect preset nearest to the client size and calls `eCGuiModule2::ReSize(client w, h)`: **the GUI layout follows the window's client size**, not the back buffer [C] |
| `eCApplication::OnHandleMessage` 0x4bf0b0 | WM_ACTIVATE: popup hides the cursor again, cursor confinement (`ConfineCursor`, config `CaptureCursor`); no minimise on focus loss [C] |

With D3D9 the runtime adds the exclusive display-mode switch for `Windowed = FALSE`. In Vulkan mode nobody did anything:
a fullscreen request gave a topmost popup of the config size at (0,0) at the desktop's resolution, and a windowed mode at
the monitor's size was clamped by Windows (client 2560x1421 for 2560x1440 with a caption [V]: a squashed swapchain).

## 2. What we do

`decide(windowed, back buffer, monitor rect)` (pure, `rvk_tests window_mode_decide`):

| engine mode | result |
|---|---|
| fullscreen (popup), size == monitor | borderless over `rcMonitor`, `WS_POPUP` stays |
| fullscreen, other size | the engine switches to the monitor's size first, then as above |
| windowed, size == monitor | borderless over `rcMonitor`; caption / frame bits removed, **not** `WS_POPUP` (style 0x14c80000 -> 0x14080000), so `GetWindowMode` still answers "windowed" from its stored mode |
| windowed, other size | the engine's own bordered window, untouched (pixel-identical runs) |

* **Size of fullscreen requests**: a pre-hook of `SetResolutionEx` replaces width / height of a popup request with the
  size of the monitor the window is on (captured before the engine centres the window on the primary). So F2, the
  options menu while fullscreen (its resolution choice then has no effect: always the monitor's) and our command all
  run the engine's own path at the monitor's size: back buffer, viewports, the GUI (`OnSize`) and the mouse agree. A
  fullscreen **start** from the config (`Right`/`Bottom` != monitor) is corrected once at the first present with
  `eCApplication::SetResolution(monitor, popup)` (the engine's next `Reset` brings the device to the monitor's size;
  [V] 1600x900 -> 2560x1440 at the loading screen). Reporting the monitor's size to the engine while keeping its back
  buffer was rejected: the GUI lays out for the client size and the mouse uses client coordinates.
* **Placement** (`onModeSet` from the HAL's device creation and `ToggleNewMode`, applied at the next `SwapScreen` before
  the present, on the engine's thread, so `WM_SIZE` -> `OnSize` never runs inside the device creation or `Reset`):
  style as above, `WS_EX_*EDGE` / `DLGMODALFRAME` removed, `SetWindowPos(HWND_NOTOPMOST, rcMonitor, SWP_FRAMECHANGED |
  SWP_NOACTIVATE)`; when the game is not the foreground window it is put right behind the foreground window (background
  test runs do not cover the desktop). The swapchain is re-created at once (`Device::windowResized`; it always takes the
  surface's `currentExtent`). The engine's topmost flag is dropped already in `onModeSet` (z-order only), so the loading
  screen (first present 11-17 s after device creation) does not sit topmost over everything.
* **Not topmost**: alt-tab and other windows behave as with any window; the window stays borderless when it loses focus
  (no minimise). No exclusive fullscreen, no display mode change; DWM may promote the window to independent flip.
* **Kept on its monitor**: every 250 ms while borderless (not minimised, no caption = the engine is not mid-change): moved
  away / topmost again -> placed again ([V]: an external `SetWindowPos` to (300,200) was undone within 0.4 s); on a
  monitor of another size -> `SetResolution(that size, same mode)` (at most twice in a row). The harness'
  `run-windowed.ps1` moves the window to (100,100) with `SWP_NOSIZE`; that is undone too.
* **Leaving fullscreen**: the engine restores its own style in `SetWindowMode` (caption, not topmost, cursor shown) and
  centres the client on the primary; `ToggleNewMode(windowed, smaller size)` -> bordered, nothing to do. F2 from
  fullscreen keeps the size (the monitor's), so it lands in "windowed at the monitor's resolution" = still borderless by
  the rule above; a smaller windowed size (options menu, Ctrl+F2 in TestMode, `window WxH`) gives the bordered window.

## 3. Frame pacer, overlay

`vk::FramePacer` already re-queries the refresh rate when `MonitorFromWindow` changes (once per second). The overlay
(`src/overlay`) sizes ImGui from the swapchain extent and reads client coordinates from the window procedure: it follows
the new size without changes ([V] F10 after four mode switches: "overlay: opened").

## 4. DPI

Risen.exe has no `dpiAware` manifest entry (DPI unaware). On a scaled display Windows would virtualise `GetMonitorInfo`
/ window sizes (e.g. 1707x960 for 2560x1440 at 150 %) while `EnumDisplaySettings` (the engine's mode list) reports
physical modes, and DWM would stretch the window bitmap (blurry, and "windowed == monitor size" could never match). In
Vulkan mode the `sEnumAdapterDesc` detour (before the game window exists, after the splash) calls
`SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` (rvk.log `engine: window: process DPI awareness: per-monitor v2`
[V]): window, client and monitor sizes are physical pixels. Side effects: a windowed game on a scaled display is no
longer enlarged by DWM (crisp, physically smaller window); `eCWindow::ms_iDesktopWidth/Height` may have been read before
(centring only) [I]. Moving between monitors of different DPI: the engine ignores `WM_DPICHANGED`, the window keeps its
pixel size [I]. Not testable here (100 % displays).

## 5. Test tooling

* `run.ps1 -Fullscreen`: writes `<Window FullScreen="true">` into the copy's `ConfigUser.xml` (the key players have
  from `ConfigDefault.xml`; test-run.ps1 keeps it and only rewrites `Right`/`Bottom`). Every run without the switch
  removes the key, so the copy's `ConfigLocal.xml` (`FullScreen="false"`) decides again. With the default
  `-WindowSize 1600x900` this exercises the "fullscreen at another size" correction.
* Windowed at the monitor's size: `run.ps1 -WindowSize 2560x1440` (the monitor's resolution).
* Runtime switches: `-Commands '900: window 1600x900','1300: window fullscreen','1700: window 2560x1440','2100: window
  toggle','2400: window state'` (overlay command `window`: `fullscreen` = `SetResolution(monitor, popup)`, `toggle` =
  `ToggleFullscreen` (F2's action), `<W>x<H>` = windowed at that size, `state` = one line about the window).
* rvk.log: `engine: window: mode WxH windowed|fullscreen`, `borderless over monitor \\.\DISPLAYn (x,y WxH) (...): style
  .. -> .., ex .. -> .., window .., client .., back buffer ..`, `bordered (the engine's window): ...`,
  `SetResolution(...)`, `fullscreen request WxH -> the monitor's WxH`, `placed over its monitor again`, plus
  `vulkan: swapchain WxH`.

## 6. Verified (2026-10-22, testdemo, two 2560x1440 monitors at 100 %)

* `-Fullscreen` (config 1600x900): device 1600x900 fullscreen -> first present `SetResolution(2560x1440)` -> Reset ->
  `ToggleNewMode 2560x1440 (windowed false)` -> borderless over DISPLAY2 (0,0 2560x1440), style 0x94000000, ex 0 (topmost
  gone), swapchain 2560x1440; loading screen, intro and world screenshots fill 2560x1440 with the GUI centred and the HUD
  (bars, compass) at the edges, correct aspect.
* Runtime, one run with `-Validation` (0 validation messages): fullscreen -> `window 1600x900` (bordered, client
  1600x900, window centred at 472,250) -> `window fullscreen` (borderless) -> `window 2560x1440` (windowed borderless,
  style 0x14080000) -> `window toggle` (F2 path, borderless popup) -> F10 opens the overlay.
* Windowed at 2560x1440 from the start: borderless (client 2560x1440 instead of the clamped 2560x1421); external move
  undone; `window 1600x900` -> bordered.
* Windowed 1600x900 (normal test runs): bordered, the engine's window, no extra swapchain re-creation.

## 7. Manual checks left to the player / user

* Multi-monitor with **different** resolutions, the game on a secondary monitor, Win+Shift+arrow across monitors.
* Scaled displays (125 / 150 %) and HDR: per-monitor DPI path untested here.
* Real alt-tab / focus behaviour with the game in front (test runs keep it in the background), the debug keys F2 /
  Ctrl+F2 themselves (need TestMode, debug-mode.md), the options menu's Accept/Revert dialog while fullscreen.
* The mode list (`HalInstance`, `EnumDisplaySettings(nullptr)`) is the primary display's: a windowed resolution that
  only a secondary monitor has cannot be chosen at start (as with D3D9).
