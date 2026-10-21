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
| `vulkan.vsync` | enum `engine` / `on` / `off` | `engine` | live (swapchain recreated) | `engine` = the game's own video option (start parameters / `ToggleNewMode`) |
| `vulkan.max_fps` | float 0..500 | 0 | live (pacer) | 0 = no cap beyond vsync |
| `vulkan.adapter` | int -1..15 | -1 | restart | Vulkan physical device (-1 = best discrete GPU) |
| `overlay.toggle_key` | enum (none, f1..f12, insert, home, end, page_up, page_down, pause, scroll_lock) | `f10` | live | NoAB |
| `overlay.ab_key` | same | `f11` | live | NoAB |
| `overlay.fps_counter` | bool | false | live | small fps readout while the overlay is closed; NoAB |
| `overlay.ab_badge` | bool | true | live | flip badge (1.5 s) and a "B" tag while side B is active; NoAB |
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
* Test runs: `run.ps1` deletes settings.toml before each run (reproducible renderer tests, refcompare included) unless
  `-KeepSettings`; `-Setting 'vulkan.max_fps=60','vulkan.vsync=off'` writes a fresh one.

### 2.4 A/B comparison

Two sides: the active values and a stored other side, **initially the defaults** (= the plain renderer, so a player who
enabled features compares them against parity with one key). `flipAB()` swaps the two for every eligible setting (not
NoAB, not Restart, not locked) and fires the callbacks of what changed. The overlay offers "Flip", "Copy A to B" (the
active side over the other) and "Reset B to defaults", and lists the settings whose sides differ. The session always
starts on side A; saving while B is active still writes A to the main tables. Hotkey `overlay.ab_key` (F11) works with the
overlay closed; the badge shows the side for 1.5 s, and a small "B" stays in the top-right corner while B is active.

Verified [C]: `ab flip` with A = `vsync off` switched the swapchain FIFO <-> IMMEDIATE each time (rvk.log "present mode
2/0"), with validation on.

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

F10 (overlay) and F11 (A/B) are unbound in Risen [C]: the game's bindings (`ConfigUser.xml` `<Keys>`: F8 QuickSave, F9
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
