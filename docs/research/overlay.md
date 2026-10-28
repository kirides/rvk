# Player settings, A/B comparison and the in-game overlay (M6 platform)

The first M6 platform piece (docs/PLAN.md M6, docs/research/enhancements-roadmap.md 4 and "do now" item 9): a settings
registry that every future enhancement registers its options in, the player's `bin\rvk\settings.toml`, an instant A/B
comparison, a Dear ImGui overlay on the Vulkan swapchain, and tool-API exports so test scenarios can flip settings.

Legend: **[C]** confirmed (decompiled, or verified in the running game), **[I]** inferred.

## 1. What exists

| piece | files |
|---|---|
| settings registry, TOML I/O, A/B, the renderer's own settings | `src/core/settings.{h,cpp}` (unit tests `tests/settings_tests.cpp`) |
| overlay: ImGui, window subclass (input filter), UI, badges, `[debug] commands` | `src/overlay/overlay.{h,cpp}` |
| ImGui's Vulkan backend (vendored, compiled against volk) | `third_party/imgui_vulkan/` (imgui 1.93.0, MIT), `src/overlay/imgui_vulkan.cpp` |
| present hook (draw after the back-buffer copy), swapchain image views | `src/d3d9/device.{h,cpp}` (`PresentOverlay`), `src/vk/swapchain.*` |
| applying settings (vsync, max_fps), debounced saving | `src/hal/vulkan/device.cpp` (`applyVsync`, `serviceSettings`) |
| loading at attach + config.toml locks | `src/bootstrap.cpp` (`loadSettings`) |
| tool API | `include/rvk_api.h`, `src/api/settings_api.*`, `src/proxy/d3d9_exports.cpp` (tests `tests/settings_api_tests.cpp`) |
| test switches | `scripts/run.ps1` `-Setting`, `-KeepSettings`, `-NoOverlay`, `-OverlayInScreenshots`, `-Commands` |

## 2. Settings registry

### 2.1 Model

A setting is a `settings::Spec` (stable key `"<table>.<name>"`, label, category, description, type Bool / Int / Float /
Enum, default, range or options, flags) plus its value. Values are doubles internally (bool 0/1, enum = option index) and
`std::atomic<double>`: any thread reads them lock-free (`asBool()`, `asInt()`, `asFloat()`, `asEnum()`).

* **Changes** come from any thread (overlay, `rvk_setting_set`, file load, A/B flip). Ints are rounded and clamped, floats
  clamped, enums / bools validated. A change marks the setting; **callbacks run on the render thread** in
  `Registry::dispatch()`, which `HalDevice::SwapScreen` calls before every present (so "applies live" means "at the next
  frame boundary", the roadmap's runtime-switching rule). Several changes within one frame give one callback.
* **Flags**: `Restart` (stored, takes effect at the next start; the overlay shows "restart needed" while the value differs
  from the session's start value), `NoAB` (not part of the A/B comparison: hotkeys, overlay options), `Hidden` (file / API
  only).
* **A future enhancement** registers in a few lines and reads where it renders:

  ```cpp
  auto& water = reg.add({.key = "water.enhanced", .label = "Enhanced water", .category = "Water",
                         .description = "...", .type = settings::Type::Bool});
  reg.onChange(water, [](const settings::Setting& s) { /* create / destroy resources */ }, owner);
  if (water.asBool()) { ... }
  ```

  The overlay, the file, A/B and the API pick it up without further code. Callbacks of an object that dies before the
  registry are removed with `removeCallbacks(owner)` (HalDevice does that).

### 2.2 Registered now

| key | type | default | applies | notes |
|---|---|---|---|---|
| `renderer.mode` | enum `original` / `wrapper` / `full` | `wrapper` | Wrapper <-> Full live (frame boundary), Original <-> the others restart | System tab, "Renderer" (2026-10-28, 2.7): Original = the system d3d9 forwarded (no Vulkan, no enhancements, no overlay), Wrapper = the D3D9 model on Vulkan + enhancements, Full = the native frame takes over every draw (`native.static_ops` + `native.scene_ops`, all groups); `Spec::restartGroups` {0, 1, 1}; A/B-eligible (F10 flips Wrapper / Full in debug mode); config.toml's non-default `[renderer] mode` locks it |
| `vulkan.vsync` | enum `engine` / `on` / `off` | `engine` | live (swapchain recreated) | `engine` = the game's own video option (start parameters / `ToggleNewMode`) |
| `vulkan.max_fps` | float 0..500 | 0 | live (pacer) | 0 = no cap beyond vsync |
| `vulkan.fullscreen` | enum `borderless` / `exclusive` | `borderless` | live (display mode / swapchain) | exclusive: the monitor switches to the game's resolution + refresh while in front; needs the capability "Exclusive fullscreen" (window-mode.md 8) |
| `vulkan.exclusive_acquire` | bool (hidden) | on | live | exclusive fullscreen also takes Vulkan's exclusive mode (diagnostics, window-mode.md 8.3) |
| `vulkan.adapter` | int -1..15 | -1 | restart | Vulkan physical device (-1 = best discrete GPU) |
| `overlay.toggle_key` | enum (none, f1..f12, insert, home, end, page_up, page_down, pause, scroll_lock) | `f11` | live | NoAB (F11 since 2026-10-23: GD3D11 players know it; was F10) |
| `overlay.ab_key` | same | `f10` | live | NoAB, Debug (the hotkey only acts in debug mode; outside it the game gets the key) |
| `overlay.fps_counter` | bool | false | live | small fps readout while the overlay is closed; NoAB |
| `overlay.ab_badge` | bool | true | live | flip badge (1.5 s) and a "B" tag while side B is active; NoAB, Debug |
| `overlay.debug_mode` | bool | false | live | the registry's debug switch (2.5); NoAB |
| `overlay.show_advanced` | bool (hidden) | false | live | shows the `Flag::Advanced` settings: the checkbox at the top right of the window (2.6); NoAB |
| `rt.allow` | bool | true | restart | enable the ray tracing tier at device creation; requires `RayTracingHardware` (greyed + off without it); docs/research/raytracing.md |

### 2.3 Files and precedence (decision)

* `bin\rvk\settings.toml` is the **player's** file. The registry writes it ~1 s after the last change (debounced on the render
  thread; atomically: `.tmp` + `MoveFileEx`). **Only values that differ from the defaults are written** (a short file, and
  a default changed by a later version reaches players who never touched the setting). Side B of the A/B comparison goes
  to `[ab.<table>]` tables (again only non-default values). Unknown keys and bad values are skipped with a warning in
  `rvk.log` (an unknown key is dropped at the next save).
* `bin\rvk\config.toml` stays the **developer / debug** file. For the three overlapping options (`[vulkan] vsync`,
  `max_fps`, `adapter`) **config.toml wins when it sets a non-default value** (`vsync` true/false, `max_fps` > 0,
  `adapter` >= 0): the setting is *locked* for the session (read-only in the overlay with "(set by config.toml)",
  `rvk_setting_set` returns `RVK_SETTING_LOCKED`, excluded from A/B); the player's own value stays in settings.toml.
  `run.ps1` always writes `vsync = "engine"` and `max_fps = 0.0`, i.e. no lock unless `-Vsync` / `-MaxFps` are given.
  Since 2026-10-28 `[renderer] mode` too: a value other than `wrapper` locks `renderer.mode` (2.7).
* Test runs: `run.ps1` writes a fresh settings.toml before each run (reproducible renderer tests, refcompare included):
  the **test baseline** (`overlay.debug_mode = true`; `water.enabled` / `shadows.enabled` false, `water.quality` /
  `water.ocean_quality` / `rt.reflections` medium, `shadows.filter` high, `shadows.map_size` auto = the defaults before
  2026-10-23, i.e. parity) plus `-Setting 'vulkan.max_fps=60',...` (per key, over the baseline). `-PlayerDefaults` =
  the shipped defaults instead of the baseline (debug mode still on); `-KeepSettings` without `-Setting` keeps the file.
* Player defaults (2026-10-23): `water.enabled` on, `water.quality` / `water.ocean_quality` / `rt.reflections` high,
  `shadows.enabled` on, `shadows.filter` low, `shadows.map_size` 2048.

### 2.4 A/B comparison

Two sides: the active values and a stored other side, **initially the defaults** (since 2026-10-23 the defaults enable
water and shadows: for parity, set side B's features off). `flipAB()` swaps the two for every eligible setting (not
NoAB, not Restart, not locked) and fires the callbacks of what changed. The overlay offers "Flip", "Copy A to B" (the
active side over the other) and "Reset B to defaults", and lists the settings whose sides differ. The session always
starts on side A; saving while B is active still writes A to the main tables. Hotkey `overlay.ab_key` (F10, debug mode) works with the
overlay closed; the badge shows the side for 1.5 s, and a small "B" stays in the top-right corner while B is active.

Verified [C]: `ab flip` with A = `vsync off` switched the swapchain FIFO <-> IMMEDIATE each time (rvk.log "present mode
2/0"), with validation on.

### 2.5 Debug mode

`overlay.debug_mode` (default off) is the registry's debug switch (`Registry::setDebugSwitch`). Settings with
`Flag::Debug` (the Debug category's views `debug.tint_draws`, `debug.inject_view`, `rt.debug`, `water.debug`,
`shadows.debug`, plus `overlay.ab_key` / `overlay.ab_badge`) are shown in the overlay only in debug mode; outside it they
are *gated*: their effective value is the default (debug views off), the player's value stays in settings.toml, and the
change callbacks fire when the effective value changes. The A/B section (and its F10 hotkey), the "Frame structure"
panel and the performance panel's diagnostics (present mode, pacing, enhancement status lines) are debug-mode only too.
Turning debug mode off returns the A/B comparison to side A. `rvk_ab` / `[debug] commands` `ab ...` keep working
without it (scripted runs have debug mode on through run.ps1 anyway). Unit test `settings_debug_mode`.

### 2.6 The settings window (player layout, 2026-10-23)

Tabs like the game's own options and GD3D11: **Display | Graphics | System**, plus **Debug** in debug mode
(`Spec::tab`; sections inside a tab = `Spec::category`, in registration order). A checkbox "Show advanced settings" at
the top right (`overlay.show_advanced`, forced on in debug mode) adds the `Flag::Advanced` fine tuning.

| tab | sections, settings (advanced in italics) |
|---|---|
| Display | Display: vertical sync, frame rate limit ("Unlimited" at 0), fullscreen mode; a pointer to the game's Options menu |
| Graphics | Preset row (Original / Low / Medium / High / Ultra, "(custom)" when none matches); Water: enhanced water, water quality, ray-traced reflections, sea waves, sea detail, sea state, *water reflections, wind direction, wave height, murkiness, reflection strength*; Shadows: enhanced shadows, edge quality, resolution, *softness, detail blending, fade-out, offset, distance, detail distribution* |
| System | Renderer: render mode (2.7); Menu: settings menu key, show frame rate, debug mode; Hardware: *graphics card, ray tracing support*; "Graphics card" (capability report) and "Performance" panels (diagnostic lines in debug mode only) |
| Debug | A/B comparison panel; A/B options (key, indicator); Debug views (tint, injection, RT scene, water, shadows); "Frame structure" |

Each tab ends with "Reset <tab> settings to defaults" (`settings::resetTab`: every setting of the tab, advanced and
hidden ones included, locked ones kept). Presets (`settings::graphicsPresets`, `applyPreset`, `matchPreset`) set water,
sea, RT reflections and shadows together; advanced settings are not part of them; High = the defaults; Original lists
only what turns the enhancements off. The preset choices (which quality per preset) are a first judgement, not measured.
Tooltips are the specs' descriptions in player language (developer wording only on the Debug tab), wrapped, plus
"Default: <label>" (and `key = value` in debug mode). `[debug] commands`: `overlay tab <display|graphics|system|debug>`
selects a tab (screenshots of each tab). Unit test `settings_presets_and_tabs`.

### 2.7 Render mode (`renderer.mode`, 2026-10-28)

The user's decision: three render modes. **Original** = the system d3d9 forwarded (backend `Mode::Passthrough`: no Vulkan,
no enhancements, no overlay); **Wrapper** = the D3D9 model on Vulkan + the enhancements, the engine draws through the HAL
(what Vulkan mode was before the native frame); **Full** = Vulkan with the native frame taking over everything
(`native.static_ops` + `native.scene_ops`, every group of `native.scene_ops_groups`; the op tap runs as they need it;
compare modes off). `trace` stays a developer mode outside the three. Shipped default: Wrapper (Full still costs about
+5.4 ms CPU per frame at the savegame because the engine's own slot code still runs; to be revisited after the first
optimization). Code: `core/config.h` (`RenderMode`, `parseRenderMode`, `configModeLock`, `sessionBackend`,
`vulkanSessionMode`, `nativePlan`), `HalDevice::applyRenderMode` / `applyStaticOps`, bootstrap `loadSettings`.

* **Source of truth**: settings.toml's `renderer.mode` (the overlay writes it). config.toml's `[renderer] mode` is the
  developer override: when it is not the default (`wrapper`; aliases `vulkan` = wrapper, `passthrough` / `d3d9` =
  original), it locks the setting for the session like `[vulkan] vsync` (`"trace"` locks it to original; an unknown value
  or a broken config.toml = original, the safe mode). The shipped config.toml says `wrapper` = the overlay decides.
  A missing `[renderer] mode` (or a missing config.toml) = the player's value too.
* **Restart**: the backend is decided at start (bootstrap, after settings.toml, before the hooks). Wrapper <-> Full apply
  live: the change callback runs in the registry's dispatch before a present, i.e. at a frame boundary (the per-stage
  take-overs were already switchable mid-run, README.md 16). Original <-> Wrapper / Full need a restart: `Spec::restartGroups`
  ({0, 1, 1}: options of different groups) makes `restartPending()` true, so the overlay shows "restart needed". A Vulkan
  session asked for Original renders as Wrapper until the restart. To leave Original (no overlay), delete the
  `[renderer]` lines of settings.toml (README.txt says so).
* **A/B**: the setting is A/B-eligible (no Restart flag), so in debug mode F10 flips Wrapper vs Full (side B = full).
* **Debug overrides**: `native.static_ops` / `native.scene_ops` (Debug) turn a stage on in Wrapper too; in Full
  `native.scene_ops_groups` masks groups; `native.*_compare` work on top of either mode. `native.opaque` / `native.shadows`
  / `native.debug` (the N3 mirror-driven path) stay Debug-only as the basis of a later optimized mode.
* rvk.log: `renderer: mode <m> at start (<default | settings.toml renderer.mode | config.toml [renderer] mode>; backend
  <passthrough | vulkan | trace>)`, then in a Vulkan session `renderer: mode wrapper|full (...)` at the device and on every
  change (`mode original requested ...` for a pending restart). The capability report (`caps:` lines, the System tab's
  "Graphics card" panel) has `render mode: <m> at start`.
* Tooling: `run.ps1 -Mode original|wrapper|full` (also `passthrough`, `vulkan`, `trace`; default original); `original` /
  `full` lock the setting, `wrapper` / `vulkan` leave it to the baseline settings.toml (wrapper), so `-Commands '2000: set
  renderer.mode full'` / `-Setting 'renderer.mode=full'` work. `refcompare.ps1` / `water-compare.ps1` / `shadow-compare.ps1
  -Mode wrapper|full` (default wrapper); Wrapper vs Full side by side: `water-compare.ps1 -EngineSetting
  'renderer.mode=wrapper' -Setting 'renderer.mode=full'`. Unit tests `config_render_modes`, `render_mode_native_plan`,
  `settings_renderer_mode`.
* Verified [C] (2026-10-28): `-Mode original` / `passthrough` testdemo PASS, system d3d9 loaded, no Vulkan device, no
  `native:` lines; `-Mode wrapper -Validation` testdemo PASS, `renderer: mode wrapper`, no `native:` lines, 0 validation
  messages; `-Mode full -Validation` savegame (`rvk_perfsave`, hold 30) PASS, coverage 100.000 % ours per 5 s window,
  99.976 % over the run (685 engine draws, all after the load), 0 validation messages; `-Mode vulkan -Validation` savegame
  with `-Commands '2000: set renderer.mode full','3000: set renderer.mode wrapper'`: PASS, `renderer: mode full` / the
  frame tap on at the first command, coverage 100.000 % ours in both Full windows, tap off and no coverage after the
  second, 0 validation messages, 80 fps (Chill cap) throughout.

## 3. Input: how the engine reads it, how the overlay takes it

### 3.1 The engine's input path [C] (Engine.dll, decompiled: `B:\DevDrive\risen-ghidra\rvk_input_dump.txt`)

* Static imports: no DirectInput anywhere (Engine, Game, GUI2, FFC, FFCore, SharedBase, Risen.exe). Engine.dll imports
  `RegisterRawInputDevices`, `GetRawInputData`, `GetCursorPos`, `SetCursorPos`, `ClipCursor`, `ShowCursor`,
  `GetCursorInfo` and XInput 1.4; GUI2.dll `GetAsyncKeyState`, `GetKeyState`.
* `eCInputAdmin::Initialize` (0x5a40f0) registers **raw input for the mouse only** (usage page 1, usage 2, flags 0,
  `hwndTarget` null: WM_INPUT goes to the focused window).
* `eCApplication::OnHandleMessage` (0x4bf0b0, virtual, called from the window procedure) gives each message first to the
  console (`eCConsole2::OnHandleMessage`), then to the GUI module (`eCGuiModule2`, through its handler at +0x48), then to
  `eCInputAdmin::OnHandleMessage` (0x5a3de0):
  * `WM_KEYDOWN` / `WM_SYSKEYDOWN` -> `DispatchKeyPressed(scan code)` (lParam bits 16-24; auto-repeat ignored),
    `WM_KEYUP` / `WM_SYSKEYUP` -> `DispatchKeyReleased`;
  * mouse buttons and wheel from `WM_LBUTTONDOWN` .. `WM_MOUSEHWHEEL` (codes 0x200 .. 0x206);
  * **mouse motion only from `WM_INPUT`** (`GetRawInputData`, relative `lLastX` / `lLastY` -> analog axes).
  * key state array at `this+0x18` (0x319 bytes, cleared by `ResetBufferedInput` 0x5a3950 and on `WM_ACTIVATEAPP`).
* `WM_LBUTTONDOWN` and activation call `eCApplication::ConfineCursor` (0x4bf010: `ClipCursor` to the client rect when the
  window mode asks for it). `WM_SYSCOMMAND` `SC_KEYMENU` is swallowed (F10 / Alt do not open a system menu).
* The gamepad (`eCInputAdmin::UpdateX360`, XInput) is polled, not message-driven.
* Window procedure and rendering run on the **same thread** [C] (rvk.log: "window messages on thread N, rendering on
  thread N").

### 3.2 The filter

`Overlay::init` subclasses the game window (`SetWindowLongPtrW(GWLP_WNDPROC)`; the window is Unicode [C]).

* **Closed**: every message goes to the game. Only the two hotkeys (down and up) are swallowed. The filter remembers which
  keys / buttons the game saw pressed. ImGui is not touched at all; with no badge visible the present path is unchanged.
* **Open**: keyboard and mouse messages (`WM_KEY*`, `WM_SYSKEY*`, `WM_*CHAR`, `WM_MOUSEFIRST..WM_MOUSELAST`, `WM_INPUT`) are
  queued for ImGui (replayed on the render thread through `ImGui_ImplWin32_WndProcHandler`) and not passed to the game.
  Exceptions: releases of keys / buttons the game saw pressed still reach it (nothing stays held down when the overlay
  opens mid-movement); `WM_INPUT` goes to `DefWindowProc` (its cleanup); system keys go to `DefWindowProc` (Alt+F4 keeps
  working). `WM_SETCURSOR` hides the OS cursor; ImGui draws its own.
* Not filtered: the gamepad (XInput is polled), and `GetAsyncKeyState` queries in GUI2.dll [I: modifier checks].

### 3.3 Hotkeys

F11 (overlay) and F10 (A/B, debug mode) are unbound in Risen [C]: the game's bindings (`ConfigUser.xml` `<Keys>`: F8 QuickSave, F9
QuickLoad) and debug keys (`ConfigDefault.xml` `<DebugKeys>`: F1-F7, F12, Ctrl+F2, Ctrl+F8, Alt+F12) leave them free.
Both are player settings (`overlay.toggle_key`, `overlay.ab_key`; `none` disables).

### 3.4 Verified in the game [C] (`rvk_shadows` scenario, background window, posted messages)

| step | result |
|---|---|
| overlay closed, posted `C` (WM_KEYDOWN/UP) | the character screen opens (control: posted messages reach the game) |
| overlay open, posted `C`, overlay closed again | no character screen (frame_2100); after closing, a posted `C` opens it (frame_2500) |
| overlay open, posted mouse move + click at 300,300 | ImGui opened the combo under it; the game's GUI cursor did not move. Overlay closed: the same click moves the game's cursor |
| posted `F10`, `F11`, `F10` | overlay opens (hotkey path), A/B flips (badge), overlay closes; rvk.log "kept from the game" counts |

## 4. Tool API (include/rvk_api.h)

New exports (all optional for callers, any thread, any mode; ABI additions only):

| export | purpose |
|---|---|
| `rvk_setting_count()`, `rvk_setting_info(index, struct rvk_setting_info*, size)` | enumerate settings; the struct is versioned by size like `rvk_stats` (v1: 80 bytes) |
| `rvk_setting_get(key, buf, size)` | value as text; returns the full length (truncation detectable) or an error |
| `rvk_setting_set(key, text, flags)` | flags must be 0; `RVK_SETTING_OK` / `UNKNOWN_KEY` / `BAD_VALUE` / `LOCKED` / `BAD_ARGUMENT`; applies at the next frame and is saved like an overlay change |
| `rvk_ab(op)` | `RVK_AB_QUERY/FLIP/SIDE_A/SIDE_B/COPY/RESET`; returns the active side (0 = A, 1 = B) |
| `rvk_overlay(op)` | `RVK_OVERLAY_QUERY/OPEN/CLOSE/TOGGLE`; 1 open, 0 closed, -1 no overlay |
| `RVK_CAP_OVERLAY` (0x8) | new `rvk_capabilities` bit: the overlay exists |

risen-spass has no generic way to call these from Lua yet (its `rvk` binding covers the screenshot exports only), so
**automated A/B from scenarios needs a small spass binding** (e.g. `risen.debug.rvk_setting(key [, value])`,
`risen.debug.rvk_ab(op)`), not done in this step. Renderer tests drive the same code paths through `[debug] commands`.

## 5. Debug and test hooks

* `[debug] commands = ["<frame>: <command>", ...]` (`run.ps1 -Commands`): at that present (render thread):
  `overlay open|close|toggle`, `set <key> <value>`, `ab flip|a|b|copy|reset`, `key <name>` (posts WM_KEYDOWN + WM_KEYUP with
  the right scan code to the game window: the real input path, incl. the hotkeys), `move x y`, `click x y` (posted mouse
  messages, client coordinates). Results go to rvk.log ("overlay: [debug] command at frame ...").
* `[debug] overlay_in_screenshots = true` (`-OverlayInScreenshots`): the overlay is drawn into the back buffer before the
  captures, so `-Screenshots` / `rvk_screenshot` contain it. **Default: it is drawn into the swapchain image after the
  back-buffer copy, so no capture (rvk_screenshot, `screenshot_frames`, the engine's slot-159 snapshot) ever contains it**
  [C: a spass screenshot taken while the overlay was open shows only the game]. The engine snapshot is taken before
  SwapScreen and never contains the overlay, also not in this mode.
* `[overlay] enabled = false` (`-NoOverlay`): no ImGui, no subclass.

## 6. Rendering

* ImGui 1.93.0 from vcpkg (`imgui[win32-binding]`: core + Win32 backend). The Vulkan backend is **vendored**
  (`third_party/imgui_vulkan`, same release) and compiled with `IMGUI_IMPL_VULKAN_USE_VOLK`: vcpkg's `vulkan-binding`
  compiles it against the Vulkan prototypes and links `vulkan-1.lib`, whose symbol names collide with volk's global
  function pointers (`src/vk/volk_impl.c`). When vcpkg's imgui version changes, replace the two files from the same
  release (`IMGUI_CHECKVERSION` catches a mismatch at startup). Setup as in GD3D11's `ImGuiShim::InitVulkan`.
* Dynamic rendering (`UseDynamicRendering`, `PipelineRenderingCreateInfo` with the swapchain format), own descriptor pool,
  no ini file. Font: Segoe UI 18 px (ProggyClean when missing), scaled with the window height.
* `d3d9::Device::present`: after the blit into the swapchain image, if `overlay->active()` the image goes to
  `COLOR_ATTACHMENT_OPTIMAL`, `vkCmdBeginRendering` (LOAD/STORE), ImGui's draws, then `PRESENT_SRC`. With nothing to show
  (closed, no badge) the present path is the old one (one barrier).
* ImGui's texture uploads (the font atlas) submit to the queue and `vkQueueWaitIdle`: they run under `Context`'s queue lock
  (the present thread presents concurrently); `ImDrawData::Textures` is then cleared so `RenderDrawData` only records.
  Shutdown (vkDeviceWaitIdle) also holds the lock.
* Performance panel: fps / frame time (240-frame history and plot), CPU per frame (engine + renderer), GPU time per frame
  when the GPU timer runs (`[debug] perf`; per-pass timestamps are too costly to keep on for everyone), draws, vsync, cap.
  `Device::liveFrame()` provides the numbers; the perf vector `frameIntervalsMs` now only grows with `[debug] perf` (it
  grew unbounded before, ~4 bytes per frame).

### 6.1 Regression check

`refcompare.ps1 -ReferenceDir out\refcompare\20261021-190139` with the overlay build (overlay closed, default settings):
**13 / 13 PASS** (SSIM 0.989-1.000, dluma within +-0.51), run `out\refcompare\20261021-201718`. Validation run with the
overlay open in the swapchain path, A/B flips (swapchain recreated) and the badges: no validation messages.

## 7. Open

* Spass Lua binding for `rvk_setting_*` / `rvk_ab` / `rvk_overlay` (section 4).
* Presets (Off / Low / Medium / High / Ultra / Custom) come with the first enhancement that needs them (roadmap 4).
  Capability resolution ("requested -> effective + reason", `Spec::requirements`) is done: docs/research/raytracing.md 3.
* GPU time without `[debug] perf`: a begin/end-only timestamp mode would be cheap enough to keep on.
* The gamepad is not blocked while the overlay is open.
* A flip of `vulkan.vsync` recreates the swapchain (a short hitch); feature toggles of later enhancements should flip
  shader variants / resources instead.
