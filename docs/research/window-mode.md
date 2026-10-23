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
  (no minimise). No display mode change (exclusive fullscreen is the opt-in setting of section 8); DWM may promote the
  window to independent flip.
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

* Exclusive fullscreen: section 8.5.
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

## 8. Exclusive fullscreen (`vulkan.fullscreen = exclusive`, 2026-10-23)

User request (2026-10-23): let players use exclusive fullscreen and make the game's Hz option work, "so players could
decide to play 60 Hz vsync on an otherwise 360 Hz monitor, to save some energy/heat". Borderless stays the default.

### 8.1 How the game's refresh rate reaches us [C] (headless Ghidra, dumps `rvk_refresh_dump*.txt`, `rvk_refresh_dis.txt`)

* `CreateGraphicsSubsystem` 0x4c4330 picks the **first** context whose adapter matches, `setup.refresh == 0 ||
  setup.refresh == mode.refresh`, format 22 and **exact** width x height (also in fullscreen: the `<=` branch hangs on
  `setup+0x11` = `style == 3`, never true; hal-structs.md 1 corrected). Config `RefreshRate` (ConfigDefault: 0) is the
  setup's refresh; 0 = the first mode of that size in the HAL's list. The matched context is copied to app `+0x170`, so
  `+0x6cc` holds the **matched mode's** refresh (never 0), which also goes into the start parameters `+0x1c`.
* `SetResolutionEx` stores the refresh at `+0x6cc` without a lookup; `Reset` -> `eCGfxContextAdmin::ToggleNewMode`
  copies it into `eSAPIModeParameters +0x10` **in every mode** (windowed too; only D3D9 ignored it windowed).
  `SetResolution` keeps `+0x6cc`.
* Options menu (Game.dll `gCHudResolutionManager2`): `BuildResolutions` 0x8221d0 groups the contexts per size with the
  refresh rates of that size ascending (no windowed filter: the Hz spinner works windowed as well); `ApplyResolution`
  0x881db0 calls `SetResolutionEx(adapter, w, h, refresh value, vsync, fov, current mode)`; `AcceptResolution` ->
  `eCApplication::SaveUserResolution` 0x4c0030 writes `Right`, `Bottom`, `RefreshRate` (`+0x6cc`), `AdapterNumber`,
  `Vsync`, `FOV` (Left/Top = 0). `ApplyHardResolution` (the PerformanceMode spinner) hard-codes 60 Hz.
* Ours: the HAL mode list (`HalInstance::Init`) is the primary display's 32-bit modes (the engine creates its window at
  0,0), ordered by `engine::window::orderModes`: sizes ascending, per size **the desktop's refresh first**, then
  descending, so `RefreshRate 0` means the desktop's refresh (D3D9's ascending list made it the lowest, e.g. 48 Hz).
  rvk.log `hal: Vulkan instance, 128 display modes (desktop 2560x1440 @ 120 Hz; its size offers 120 360 240 100 75 60 48 Hz)`.
  The refresh reaches `window::onModeSet` from the start parameters and `ToggleNewMode` (`engine: window: mode WxH @ N Hz`).

### 8.2 What exclusive does

| state | what happens |
|---|---|
| setting `exclusive`, engine fullscreen, window **in front** | `ChangeDisplaySettingsExW(device, WxH@Hz, CDS_FULLSCREEN)` on the window's monitor (`pickDisplayMode`: the exact mode; refresh 0 -> the desktop's / the highest; a missing refresh -> the nearest, the higher on a tie; no mode of that size -> the desktop size via `SetResolution`), the window over the switched monitor (popup, not topmost), a swapchain with `VkSurfaceFullScreenExclusiveInfoEXT` `APPLICATION_CONTROLLED` + `...Win32InfoEXT` (the monitor), `vkAcquireFullScreenExclusiveModeEXT` (retried once a second while it fails; `FULL_SCREEN_EXCLUSIVE_MODE_LOST` from acquire / present -> not held, re-acquired later, no swapchain re-creation) |
| focus lost (alt-tab, another window, minimise) | desktop mode restored, the window minimised (`SW_SHOWMINNOACTIVE`, as D3D9's exclusive mode did), a normal swapchain; back in front -> switched again |
| engine fullscreen, never in front yet (background start) | no mode change: the window at the back buffer's size in the monitor's corner (`exclusive fullscreen waits for the window to come to the front`) |
| fullscreen request (options menu, F2, `window fullscreen WxH@Hz`) | the `SetResolutionEx` pre-hook keeps the requested size when the monitor has such a mode (borderless replaces it with the monitor's size) |
| leaving fullscreen / setting -> borderless | the desktop mode back first (in the pre-hook, before the engine sizes and centres its window); borderless then switches the engine to the monitor's size |
| process exit | `onProcessDetach` restores. Killed / crashed: **Windows restores a `CDS_FULLSCREEN` mode itself** when the process ends ([V]: a PowerShell process that set a monitor from 60 to 120 Hz was killed, back at 60 Hz within 4 s; every killed test run below ended at the desktop modes). No unhandled-exception hook needed. |

* Restore = every attached display whose current mode differs from its registry mode gets `ChangeDisplaySettingsExW(name,
  nullptr, ..., 0)`. GDI device names are **not stable** across mode changes here: `\\.\DISPLAY1` / `2` swapped between
  the two monitors after a mode change was undone ([V], twice), so names are derived from the window's `HMONITOR` at the
  moment of use.
* Between `SetResolutionEx` and the engine's `ToggleNewMode` (its next `Reset`) no placement happens (at most 3 s):
  leaving exclusive for a windowed size otherwise re-entered exclusive with the old size and left a popup window ([V], fixed).
* The swapchain gets the exclusive structures **only while the mode is set and the window in front**.
* Frame pacing: `Device::displayModeChanged` makes the pacer re-read the monitor's refresh (now the game's): vsync at 60 Hz
  paces at 60. In **borderless** fullscreen a game refresh below the desktop's becomes a frame cap with vsync
  (`Device::setRefreshCap`; vblank-locked when it divides the desktop's rate, e.g. 60 of 120 = every 2nd vblank; 100 of
  120 is paced unlocked). Windowed modes ignore the game's refresh (as D3D9 did).

### 8.3 Is Vulkan's exclusive acquire needed? (AMD RX 7900 XTX, driver 26.9.2, Windows 11 26300)

Measured with the frame pacer disabled (temporary build) at 1920x1080 @ 60 Hz, window in front, world idle, one run that
flipped the hidden diagnostic setting `vulkan.exclusive_acquire` live:

| | present modes offered | fps | interval p50 / p95 | `vkAcquireNextImageKHR` wait |
|---|---|---|---|---|
| mode switch + Vulkan exclusive acquired | 0, 2, 1000361000, 3 | 60.0 | 16.66 / 17.1-17.5 ms | 8.3 ms |
| mode switch + borderless popup (no acquire) | 0, 2, 1000361000, 3 (the same list with the exclusive info) | 60.0 | 16.66 / 17.1-17.3 ms | 8.3 ms |

On AMD the display mode change is what matters; the acquire changes nothing measurable. FIFO throttles to the new refresh
either way while the window is in front (so "AMD returns every present at once", performance.md, does not hold for a
window in front). The acquire is kept (default on: the spec's way to ask for exclusive presentation, which other drivers
may honour differently) with `vulkan.exclusive_acquire = false` (hidden) as the escape hatch. Latency / flip-model
counters (PresentMon) were not available here.

Test-machine pitfall: **while the monitors sleep (user idle beyond the display timeout) every FIFO swapchain runs at
~6 fps** (acquire ~160 ms; `vkcube --present_mode 2` too). Vsync measurements need awake displays (a 1-pixel
`SendInput` mouse nudge + `SetThreadExecutionState(ES_DISPLAY_REQUIRED | ES_CONTINUOUS)` kept them on).

### 8.4 Verified [V] (2026-10-23, `RisenTest.exe`, `-AllowFocus`, testdemo; primary 2560x1440 @ 120 Hz (360 Hz capable), secondary 2560x1440 @ 60 Hz)

* Start `-Fullscreen` (config 1600x900 @ 120): waits in the background; `window focus` -> `switched 2560x1440@120 ->
  1600x900@120` (150-220 ms), exclusive acquired.
* `window fullscreen 1920x1080@60` (the options menu's call): switched to 1920x1080@60 (420-560 ms), pacing 60, **60.0 fps**,
  screenshots 1920x1080 with the GUI / HUD laid out for it; `window display` (`EnumDisplaySettings(ENUM_CURRENT)`) showed
  the switched mode (the secondary monitor moved to x = 1920 meanwhile).
* Native resolution at a lower refresh, `2560x1440@60`: 60.0 fps; `2560x1440@120`: back at the desktop mode.
* `window minimize` -> `desktop display mode restored (the game window lost focus)`, window minimised; `window focus` ->
  switched again, the GUI intact afterwards.
* Live `set vulkan.fullscreen borderless` (desktop mode back, borderless at 2560x1440, game refresh 60 -> `frame pacing
  at 60.000 fps (vsync, the game's refresh rate)`, **60.0 fps**) and back to `exclusive` (switched to 60 Hz again).
* `window 1600x900` from exclusive: desktop mode restored, the engine's bordered 1600x900 window.
* The end of every run (the harness kills the game) left both monitors at their desktop modes.
* `-Validation`: 0 messages over mode switches, acquires, releases and setting flips. Default windowed test runs are
  unchanged (bordered 1600x900, no exclusive lines). The overlay lists "Fullscreen mode" under Display.
* Twice a test run lost focus ~3 s after the first switch and got it back 0.5 s later without a command [I: the harness's
  foreground handling]; the exclusive state followed (restore + minimise, switch again).

### 8.5 Player notes / left to check

* Options > Video: resolution and Hz apply with `Fullscreen mode = Exclusive` (overlay F10 > Display); with Borderless the
  window always covers the monitor at the desktop's resolution and a lower Hz becomes a frame cap (with vsync). The
  overlay's Performance section shows the window / display mode, present mode and the exclusive state.
* Alt-tab in exclusive costs a display mode change each way (~0.2-0.6 s of black; Windows may move other windows).
* Unchecked here: a 360 Hz desktop setting (the primary runs at 120 by the user's choice), NVIDIA / Intel drivers, HDR
  desktops (the swapchain is SDR), the game on a secondary monitor (the HAL's mode list is the primary's; a size the other
  monitor lacks falls back to its desktop size), G-Sync / FreeSync, a real keyboard alt-tab (tests used minimise and the
  focus command), the Performance section's new lines by eye.
* Test tooling: `run.ps1 -Fullscreen -AllowFocus -Setting 'vulkan.fullscreen=exclusive' -Exe RisenTest.exe` +
  `-Commands '100: window focus','400: window fullscreen 1920x1080@60','800: window display','1000: window minimize',
  '1100: window focus'`; rvk.log `engine: window: exclusive fullscreen: ...`, `desktop display mode restored (...)`,
  `vulkan: exclusive fullscreen acquired / released / lost`, `vulkan: swapchain ... (offered ...), exclusive fullscreen`.

## 9. The OS cursor (2026-10-23)

Symptom in Vulkan mode: the Windows arrow over the game at all times (menus: on top of the game's own cursor; in game too).

* The game's cursor is a GUI sprite: `eCGuiCursor2::Draw(eCSprite*, ...)` (0x537f40) through the HAL's sprite path;
  `eCGuiModule2::ShowCursor(bool)` (0x519f20) only toggles that sprite (IFFModuleGui2 slot +0xf0); `eCGuiModule2::Draw`
  (0x519a60) reads the position with `GetCursorPos` / `ScreenToClient` [C].
* The engine never hides the OS cursor in a window [C]: no `SetCursor` / `SetSystemCursor` import; the class "Engine" has
  `hCursor = LoadCursorA(NULL, IDC_ARROW)` (`eCWindow::Create`); `eCApplication::OnHandleMessage` (0x4bf0b0) handles only
  WM_SETFOCUS, WM_ACTIVATEAPP, WM_SYSCOMMAND and WM_LBUTTONDOWN: WM_SETCURSOR reaches `DefWindowProcA` = the arrow.
  `ShowCursor(FALSE)` runs only in popup mode (`eCWindow::Create`, `SetWindowMode`, WM_SETFOCUS's inlined
  `UpdateShowCursor` and 0x4c6cf0: `GetCursorInfo`, hide if showing); it lowers the display counter but does not survive
  DefWindowProc's WM_SETCURSOR in our borderless window. `IDirect3DDevice9::SetCursorProperties / SetCursorPosition /
  ShowCursor` are never called by the DX9 layer [C, grep of the decompiled layer].
* In D3D9 mode the system d3d9.dll's window subclass answers WM_SETCURSOR with `SetCursor(NULL)` while the device cursor is
  hidden (its default) [I: documented D3D9 behaviour, not traced].
* Fix (`engine/window_mode.cpp` `hideOsCursor`, from `onModeSet`, Vulkan mode only): a subclass of the game window answers
  WM_SETCURSOR over `HTCLIENT` with `SetCursor(NULL)`; caption and frame keep their cursors. It is installed before the
  overlay's subclass, so the overlay (open: hides the cursor itself, ImGui draws one) forwards to it when closed.
  rvk.log `engine: window: OS cursor hidden over the client area (the game draws its own)`.
