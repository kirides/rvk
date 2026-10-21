# Design: D3D9 reference comparison at fixed spots

Status: design (2026-10-21); built so far: the renderer side, section 4.1 (exports, `include/rvk_api.h`) and 4.3 (slot 159), and the diff tool (section 6, `tools/refcompare.py`). Goal (user decision): run the **same deterministic scenario** once
with the game's own D3D9 renderer (`scripts\run.ps1 -NoRenderer`) and once with ours (`-Mode vulkan`), capture the back
buffer at fixed spots (camera pose, time of day, weather, HUD state), and diff the pictures automatically with a report.
This is the verification criterion of PLAN.md M4 ("screenshot-diff suite at N poses within thresholds").

Markers: **[C]** confirmed by decompilation (headless Ghidra, read-only; dumps `B:\DevDrive\risen-ghidra\rvk_refcmp_*.txt`),
**[R]** read in source/docs, **[I]** inferred, must be verified in the running game before relying on it. Nothing here was
verified in the game. RVAs are relative to image base `0x180000000`; bind by decorated name, never by RVA.

---------------------------------------------------------------------------------------------------------------------

## 0. Overview

```
 scripts\refcompare.ps1 (new, our repo)
   |-- pins ConfigUser.xml (graphics options, window 1600x900, tutorials off), dedicated test copy
   |-- run A: run.ps1 -NoRenderer   -Mods rvkcompare -Scenario rvkcompare -Param tag=d3d9,...  (game's D3D9)
   |-- run B: run.ps1 -Mode vulkan  -Mods rvkcompare -Scenario rvkcompare -Param tag=vk,...    (our renderer)
   |      both: spass mod mods/rvkcompare walks spots.lua:
   |            teleport/hide player -> fixed camera -> weather + clock -> HUD -> settle -> freeze time -> burst capture
   |            risen.debug.screenshot -> backend "rvk" (our d3d9.dll export rvk_screenshot) or "d3d9" (spass Present hook)
   |            one JSON line per spot in spass.log: actual camera pose, weather weights, clock, frame numbers
   |-- collects bin\spass\screenshots\*.png + the JSON lines into out\refcompare\<stamp>\{d3d9,vk}\
   '-- python tools\refcompare.py  -> per spot metrics, noise masks, heat maps, report.html / report.json, exit code
```

Three pieces of work: **spass extensions** (camera, weather, time freeze, HUD, screenshot backend), **renderer export**
(`rvk_screenshot`), **tooling** (mod, diff tool, orchestration). Section 9 has the effort estimate.

---------------------------------------------------------------------------------------------------------------------

## 1. What exists today

### 1.1 risen-spass (paths relative to `B:\DevDrive\git\github\risen-spass`)

| Need | Existing API | Where | Usable as is? |
|---|---|---|---|
| deterministic start | `test-run.ps1` autostart `risen.world.new_game()` on the first tick; `risen.test` scenarios (`T.run`, `T.wait_until`, `T.param`, `T.finish`) | `scripts/test-run.ps1`, `src/game/lua_test.cpp`, `mods/testdemo` | yes |
| place the player | `Entity:set_pose{x,y,z,yaw,pitch,roll}` (`Entity::MoveTo`), `Entity:teleport(name or Entity)`, `risen.entity.by_id(guid)` | `types/risen.d.lua:146`, `:95` | yes (verified on objects/NPCs, the player inferred in spass) |
| ground height | `risen.world.trace` / `raycast` / `nav_point` | `types/risen.d.lua:3244..3330` | yes |
| camera | read-only `risen.world.camera()` -> `{x,y,z,dx,dy,dz,fov,aspect}`; follow camera only via `risen.input.simulate("ZoomIn"/"TurnLeft"/"ResetCamera"...)` | `src/game/lua_geometry.cpp:449`, `types/risen.d.lua:3384`, `:2004` | read: yes; set: **missing** |
| time of day | `risen.world.time()`, `set_time(h, m)` (`Entity::SetWorldTime(h + m*0.01)`, Script.dll) | `src/game/lua_routine.cpp:678`, `types/risen.d.lua:3249/3255` | yes, but the clock keeps running |
| weather | none | | **missing** |
| freeze / time scale / pause | none (`World::Pause` happens only through the ESC menu) | RE.md section 42 | **missing** |
| HUD | `risen.hud.state()`, `risen.gui.open/close/show/close_menu/key/mouse/click`, page ids (2 Inventory, 3 Character, 13 Menu_Main, 26 "Press START") | `src/game/lua_hud.cpp`, `src/game/lua_gui.cpp`, `types/risen.d.lua:2066`, `:2233..2303` | GUI pages: yes; HUD on/off: **missing** (`gui.show(1,false)` unverified) |
| NPC/animation/wind | `Entity:set_frozen` (turns into an ice statue: unusable for looks), `Entity:enable_rendering(false)` | `types/risen.d.lua:86/90` | hide only |
| screenshot | `risen.debug.screenshot(name, wait_frames)` + `screenshot_status(name)`: hooks `IDirect3DDevice9::Present` found through a throw-away device; PNG to `<bin>\spass\screenshots\<name>.png` | `src/game/screenshot.cpp`, `types/risen.d.lua:4272` | D3D9 only; `wait_frames` is ignored (bug, `Request::wait` never decremented); **in Vulkan mode the request stays pending forever** (the hook lands on the system d3d9's Present, which nobody calls) |
| console commands | `risen.script.call(name, self, other, arg)` (registered scripts) | `src/game/script_bridge.cpp:360` | not for `CON_*` (needs an argument struct, see 2.6) |
| per-frame hooks | `tick` (`gCSession::Process`), `ui.frame` (`eCApplication::DoRender`); test waits are pumped from `ui.frame` **only while a menu page is browsing** | `src/game/hooks.cpp:76-95` | must be widened for a frozen world (3.8) |
| module lookup | `eCModuleAdmin::GetInstance` + `FindModule(className)` (`module_admin_instance`, `module_find_by_class`) | `src/game/symbols.h:284`, used in `src/game/lua_input.cpp:230` | yes: the pattern for `gCCameraAdmin`, `eCWeatherAdmin`, `eCVegetationAdmin` |
| a first fixed view | `mods/rvk_shadows` (untracked, another agent's WIP): new game, clock set, one screenshot | `mods/rvk_shadows/main.lua` | superseded by `mods/rvkcompare` |

### 1.2 risen-enhanced-graphics

| Piece | Where |
|---|---|
| exports beside the d3d9 forwarders; `rvk_version()` already exists "for tools (risen-spass)" | `src/proxy/d3d9.def`, `src/proxy/d3d9_exports.cpp:127` |
| back-buffer read-back (`readRenderTarget` into a SYSTEMMEM A8R8G8B8 texture) | `src/d3d9/device.cpp:819` (`Device::readBackBuffer`) |
| PNG writer (BGRA -> RGB) | `src/core/png.h` |
| capture at fixed frame numbers in `HalDevice::SwapScreen` (`[debug] screenshot_frames`, `-Screenshots`) | `src/hal/vulkan/device.cpp:430`, `src/core/config.h` |
| HAL slot 159 `SaveBackbufferToFile(char*)` (the engine's own snapshot path) | `src/hal/api_generated.h:494`; implemented since 2026-10-21 (4.3) |
| tool API: `rvk_screenshot`, `rvk_screenshot_status`, `rvk_capabilities`, `rvk_frame_index`, `rvk_stats` | `include/rvk_api.h`, `src/api/capture.*`, `src/proxy/d3d9_exports.cpp` (4.1, since 2026-10-21) |
| `-NoRenderer` reference runs, `-GameDir`, `-Instance`, `-Param` passthrough to spass | `scripts/run.ps1` |
| `/out/` is git-ignored (screenshots are game-derived: never commit them) | `.gitignore` |

---------------------------------------------------------------------------------------------------------------------

## 2. Engine facts found for this design

### 2.1 Console commands (Script_Game.dll) [C]

Script_Game.dll exports 44 console commands `__int64 CON_<name>(gCScriptProcessingUnit*, void* self, void* other, __int64 args)`
(`?CON_Weather@@YA_JPEAVgCScriptProcessingUnit@@PEAX1_J@Z` ...), registered by static initialisers into a table
`{bCString name, bCString file, fn}` (`FUN_180053380`, source file `...\script\player\player_interface\gui_console.cpp`).
`args` is a pointer to a 0x20-byte struct:

| offset | type | meaning |
|---|---|---|
| `+0x00` | int | phase: 0 = help text for argument `argc`, 1 = tab completion, **2 = execute** |
| `+0x08` | `bCUnicodeString*` | argument array (8 bytes per string) |
| `+0x10` | int | `argc` |
| `+0x18` | `bCUnicodeString` | message written back by the command |

`self`/`other` non-null avoid `GetSelfEntity(processingUnit)`. Relevant commands: `CON_Weather` (`0x14ef60`), `CON_time`
(`0x14aa10`, shifts `PlayingTime`), `CON_timescale` (`0x14f820`, `eCTimer::SetTimeScaling`, clamped), `CON_SetVegWindParams`
(`0x1510c0`, `eCVegetationAdmin::SetWindParameters(base, factor, bendweight, wavespeed)`), `CON_CameraDump` (`0x14ed70`,
hides the console and calls `eCApplication::SnapshotEx(name)`), `CON_FreeCam*`, `CON_ShadowQuality`, `CON_EffectQuality`,
`CON_VegetationQuality`, `CON_AmbientOcclusion`, `CON_ignoreDrawCalls`, `CON_god`, `CON_invisible`, `CON_benchmark`.
`data\ini\keyboard_and_console.txt` (test copy) lists the debug keys and console commands of the build.

### 2.2 Debug-action scripts (Script_Game.dll) [C]

These are what the debug keys would call (spass found the debug actions are not in this build's key table, RE.md 46), and
show the engine calls to make directly:

| Script (RVA) | Does |
|---|---|
| `OnDebugToggleTimeFreeze` (`0xe8bb0`) | `eCApplication::SetAppState(GetAppState() == 8 ? 0 : 8)`: **app state 8 = "time freeze"** |
| `OnDebug_ToggleHUD` (`0xe6a50`) | `GUI2::GetPageMode() == 1 ? GUI2::RestoreUserPageMode() : GUI2::SetPageMode(&(gEPageMode)1, false)`: **page mode 1 = HUD off** |
| `OnDebugToggleFreeFlyCam` (`0xe71c0`) | needs `Entity::IsAdvancedTestModeEnabled()`; `gCCameraAdmin::OnToggleFreeFlyCam()` |
| `OnDebugToggleFreeCam` (`0xe6e00`) | `Entity::SetCameraTarget(None...)` + `Entity::SetCurrentCameraByName("cam_free")` (back: `"cam_normal"`) |
| `OnDebugTakeSnapshot` (`0xe8540`) | `eCApplication::Snapshot()` |
| `OnDebugToggleSloMo` (`0xe6cf0`) | `eCTimer::SetTimeScaling(slow / 1.0)` (`DAT_18027f45c` = the normal 1.0 [I]) |
| `OnDebugMovePlayerToCamera` (`0xe7e60`) | `Entity::GetCameraPose()` -> `Entity::MoveTo(player, ...)` |

### 2.3 Camera: `gCCameraAdmin` (Game.dll) [C]

* Instance: `eCModuleAdmin::FindModule("gCCameraAdmin")` + dynamic cast (Script_Game `FUN_1800ea640` does exactly this).
* `SetFixCamera(const bCMatrix& target, float dist, float distMin, float distMax, const bCEulerAngles& yawPitch, bool instant,
  8 floats: CamFOV, CamFOVChangeSpeedMax, CamMoveSpeedMax, CamRotationSpeedMax, CamMoveAcceleration, CamMoveDecceleration,
  CamRotationAcceleration, CamRotationDecceleration, bool CamMoveAndRotationSync)` (`0x7a8940`,
  `?SetFixCamera@gCCameraAdmin@@QEAAXAEBVbCMatrix@@MMMAEBVbCEulerAngles@@_NMMMMMMMM2@Z`): switches to the camera type
  `cam_cinema`, stores the orthonormalised target matrix at `+0x2c0`, writes the `FixTargetRotation*` properties of the
  camera type (distance, yaw, pitch, their min/max = the same values) and the speeds; `instant` also sets the current FOV and
  distance (`+0x228`, `+0x320`, `+0x32c`, `+0x304`) so the camera does not blend in; sets `+0x57c` (= `IsFixCameraActive`).
  An **orbit camera around a target pose**.
* Overload `SetFixCamera(const bCMatrix& target, const bCVector& eye, bool instant, 8 floats, bool)` (`0x7a8630`): derives
  distance, yaw and pitch from the eye position relative to the target and calls the first one: **"look from eye at target"**.
* `gCInfoCommandStartFixCamera::Execute` (`0x89c080`, dialogue command) uses both: target = an entity's world matrix plus an
  offset; then either distance/yaw/pitch from the info data (`0x7a8940`) or a second entity's position as the eye
  (`0x7a8630`); `instant` and the eight speeds come from the command's data.
* `StopFixCamera()` (`0x7a8460`) returns to `cam_normal` behind the player. `IsCameraInDestinationPose()` (`0x7abdd0`) is a
  settle signal. `GetCameraFOV()` (`0x7acaa0`) gives the default FOV to pass. `GetCamera()` (`0x7ac4e0`) = the camera entity
  (`+0x170`).
* `OnToggleFreeFlyCam()` (`0x7ac530`) toggles `+0x38`; on enabling it copies the current pose (`+0x188`) into `+0x2c0`; the
  free-fly pose then lives in `+0x2c0` [I] (raw offset: fallback only).
* `LoadDump/SaveDump` read/write `#G3:/appdata/<name>.xdmp` (XML `Pos`, `Rot`, `Description`) [C]; not needed.

### 2.4 Weather: `eCWeatherAdmin` (Engine.dll) [C]

* Instance: `FindModule("eCWeatherAdmin")` (Script_Game `FUN_180151440`).
* Mode at `+0x4c4` (`SetCurrentMode` `0x2743f0` is a one-store setter: call it, never detour it). `Process` (`0x710730`):
  * **0** = zones by player position (`+0x4b4`, set by `gCSession::Process` via `SetPlayerPosition`): weather zones blend
    their states, and every `eCWeatherEnvironment::Process` runs its **random state transitions** (probabilities): a source of
    run-to-run differences;
  * 1 = the same by the current camera position [I];
  * **2 = forced**: no zones, no environments; each `eCWeatherStates` contributes with its weight at **`+0x360`** (normalised;
    all zero -> state 0); every state still gets `SetCurrentDayTime(daytime)` so the sky follows the clock;
  * 3 = debug/reset path (weights zeroed).
* `CON_Weather <name>` = mode 2, weight 1.0 for the named state, 0 for the others; an unknown name = mode 0 ("reset to
  *PlayerPos* mode"). State names: `eCWeatherStates + 0x78` (`bCString`); `GetWeatherStatesCount/At/ByName` exported.
* States in the shipped `library.pak/WeatherStates.g3ws` [C, offline read]: `Game_Start`, `Stormy`, `Stormy_Rain`, `Foggy`,
  `Foggy_Rain`, `Sunny_Full`, `Sunny_Half`, `Sunny_Half_02`, `Swamp`, `Swamp_Don_Sunny`, `Gyrger*`, `Dungeon_Default`,
  `Dungeon_Blue..6`, `Dungeon_Green*`, `Dungeon_Grey*`, `Dungeon_Lava`, `Dungeon_Red_Big`, `Dungeon_Swamp`,
  `Dungeon_MiniDungeons1`, `_DefaultEdit_Setting`. They carry `ThunderProbability`, `EffectProbability`/`RandomEffect`
  (`eff_env_thunder`), `WindPower`, precipitation parameters.
* The daytime fed to the states comes from an `fmod` of a clock value (`FUN_1804ad148` = `fmodf`) [C]; that it is the world
  clock that `risen.world.set_time` sets is [I] (spass screenshots at noon support it). `SetCurrentDayTime` (`0x713ef0`) only
  writes `+0x48`, which goes to `eCWeatherEnvironment::Process`, **not** to the sky: do not use it.
* Important for interiors: mode 2 skips the zones, so a forced outdoor state also lights a dungeon. Interiors must force their
  zone's own state (`Dungeon_Lava`, ...), see 5.2.

### 2.5 Time: `eCTimer`, app state, session pause [C]

* `eCTimer::GetInstance()` (`0x4ceef0`), `SetTimeScaling(f)` (`+0x34`, multiplies the measured frame time), `SetFixedFrameTime(ms)`
  (`+0x40`: every frame counts as this long; `Update` sleeps when the real frame was shorter), `SetFixedFrameTimeSingleStep(ms)`
  (sets app state 8 and a one-frame step), `GetMasterFrameCounter()` (`0x4cee10`).
* `eCApplication::SetAppState(n)` (`0x4c1ab0`) writes `+0x134`/`+0x13c`; `eCApplication::Process` hands the state to
  `eCProcessor` (`+0xc00`) every frame. States seen: 0 running, **1 = paused** (`gCSession::Pause` -> `World::Pause`: also pauses
  the `World_MCP` clock, `gCClock_PS::SetPause`), **8 = time freeze** (debug), `0x10` (falls back to 0 after 2 s).
  Which modules still run under 8 (camera admin? particles? `gCSession::Process`, i.e. the `tick` event?) and whether
  `eCTimer::Update` stops the shader time is [I]: the early-out in `Update` (`0x4cf0e0`) depends on a virtual of the app
  (slot `0xa2`) and the process counter `+0x50`.

### 2.6 Snapshots: the engine's own capture [C]

`eCApplication::SnapshotEx(const bCString& name)` (`0x4bfba0`) creates `#G3:/snapshots/` and sets a request flag (`+0x144`,
name `+0x148`); `eCApplication::Draw` -> `DoSnapshot` (`0x4c0c50`) after the GUI and before `SwapScreen` calls the HAL device's
**slot 159 `SaveBackbufferToFile(path)`** (`+0x4f8`). `eCDX9Device::SaveBackbufferToFile` (`0x746260`) =
`GetBackBuffer` -> `D3DXSaveSurfaceToFileInMemory(BMP)` -> `eCArchiveManager::CreateFile`. The test copy mounts
`snapshots=$(localappdata)/Risen/ScreenShots` (`bin\mountlist_packed.ini`), i.e. `<test copy>\localappdata\Local\Risen\ScreenShots`
with spass's `[debug] app_data` (safe: not a junction). The exact file name built by `SnapshotEx` is [I] (decompiler output
garbled). `Snapshot()` names files `<prefix>_<date>_<nnnn>.bmp`: seen in game `#G3:/snapshots/Snapshot__21.10.2026_0000.bmp`,
the counter continuing over existing files [C, 2026-10-21]. D3DX writes a 32-bit BMP there (missing-slots.md 3).

### 2.7 Other bits [C]

* HUD: `GUI2::SetPageMode(const gEPageMode&, bool)` `?SetPageMode@GUI2@@QEAAXAEBW4gEPageMode@@_N@Z`, `RestoreUserPageMode()`,
  `GetPageMode()` (Script.dll, object `gui2` already bound by spass). `ConfigUser.xml` stores `UserPageMode="7"`.
* Vegetation wind: `FindModule("eCVegetationAdmin")` -> `SetWindParameters(base, factor, bendweight, wavespeed)` (`0x701650`).
* Randomness: `bCMersenneTwister::InitSeed()` (SharedBase `0xa1bb0`, private), `bCRandom::SetSeed(int)`; Engine/Game import
  `bCMersenneTwister::Rand*` (particles, effects) [C imports]. Seeding source not decompiled.
* `eCWeatherZone_PS`, world layers: offline extraction of 50 761 named entities with matrices from `common/projects.pak/*.lrent`
  (record format of spass RE.md section 43/1572) gave the spot anchors of 5.3 (`B:\DevDrive\risen-ghidra\rvk_refcmp_world_entities.txt`,
  game-derived, outside the repo). Positions from layer files are [I] until a teleport confirms them.

---------------------------------------------------------------------------------------------------------------------

## 3. spass extensions (risen-spass repo)

Rules of that repo apply: every binding goes into `types/risen.d.lua` in the same commit (`python tools/check_luadefs.py`,
LuaLS check of `mods/`), new symbols as `required = false` X-macro rows in `src/game/symbols.h`, engine calls through
`guarded()`, validation before touching the engine, findings into `docs/RE.md`, events/APIs into `docs/LUA_API.md`, run the
`hardening` scenario after adding bindings. Proposed new file `src/game/lua_render.cpp` (camera, weather, time freeze, HUD
mode, console runner) next to `screenshot.cpp`.

### 3.1 Screenshot with two backends (change `src/game/screenshot.cpp`)

```lua
---@param name string            1-64 chars [A-Za-z0-9_.-]
---@param opts? integer|{ wait_frames?: integer, burst?: integer, interval?: integer }
---@return true|nil ok, string|nil reason
function debugApi.screenshot(name, opts) end      -- burst n>1 writes <name>_b0.png .. <name>_b{n-1}.png
---@return "rvk"|"d3d9"|nil
function debugApi.screenshot_backend() end
---@return integer|nil frame  presented frames (rvk) or Present calls seen (d3d9) or eCTimer::GetMasterFrameCounter
function debugApi.frame() end
```

* Backend selection on the first request: `GetModuleHandleW(L"d3d9.dll")` -> `GetProcAddress("rvk_screenshot")`; if present
  and it accepts the request (it refuses in `passthrough`/`trace` mode), use it ("rvk"); otherwise the existing Present hook
  ("d3d9"). Never call `findPresent()` when the rvk backend is active (it would create a device through our proxy for nothing).
* Fix the Present hook: decrement `Request::wait` per `Present` and capture only at 0 (5 lines in `detourPresent`); add
  `burst`/`interval` (requeue with `wait = interval`).
* Both backends capture the same thing: the finished back buffer (scene + post + GUI) of the frame being presented, before it
  is shown. Same file layout `<bin>\spass\screenshots\<name>.png`; the rvk backend gets the absolute path.
* Status keeps the current strings (`pending`, `done`, `failed: ...`); with rvk it polls `rvk_screenshot_status(ticket)`.
* Optional third backend `"engine"` (`eCApplication::SnapshotEx`, 2.6) for a capture-parity self-test (3.9), not for routine use.

### 3.2 Camera: `risen.camera` (new)

```lua
---Fixed camera (gCCameraAdmin::SetFixCamera, camera type cam_cinema). Either eye + target, or target + yaw/pitch/distance.
---@param spec { eye?: number[], target: number[], yaw?: number, pitch?: number, distance?: number, fov?: number, instant?: boolean }
---@return true|nil ok, string|nil reason
function cameraApi.set(spec) end
function cameraApi.release() end                  -- StopFixCamera (back to cam_normal behind the player)
---@return boolean|nil settled                       IsCameraInDestinationPose
function cameraApi.settled() end
---@return { x,y,z, dx,dy,dz, fov, aspect, view: number[] }|nil   the render camera incl. the 16 view-matrix numbers
function cameraApi.get() end
```

* Implementation: `FindModule("gCCameraAdmin")`; build the target `bCMatrix` (identity rotation, translation = target; row-major,
  row 3 = position, RE.md 50); `eye` -> overload `0x7a8630`, else overload `0x7a8940` with `bCEulerAngles{yaw, pitch, 0}`;
  `instant = true` by default; speeds/accelerations large (e.g. 1e5) so nothing blends; FOV default `GetCameraFOV()`.
  Validation: finite numbers, distance 10..100 000, |pitch| < 89.
* `get()` adds the view matrix to what `risen.world.camera()` already reads (`cam_get_view`, `src/game/lua_geometry.cpp`).
* Verification in game (risk): the `cam_cinema` type may still apply collision avoidance or smoothing. Accept when
  `risen.camera.get()` matches the request within 1 unit / 0.1 degree after `settled()`. Fallbacks in order: (a) free-fly
  camera (`OnToggleFreeFlyCam` + write the matrix at `gCCameraAdmin + 0x2c0`, raw offset), (b) the follow camera:
  `set_pose` of the player + `ResetCamera` + `settled()` (works today, exact enough when the same player pose gives the same
  camera; the actual pose is logged and compared anyway, 5.4).

### 3.3 Time: freeze, scale, fixed frame time

```lua
function debugApi.time_freeze(on) end              -- eCApplication::SetAppState(8) / restore the previous state
function debugApi.time_scale(f) end                -- eCTimer::SetTimeScaling(f), f in [0, 10]
function debugApi.fixed_frame_time(ms) end         -- eCTimer::SetFixedFrameTime(ms), 0 = off
---@return integer|nil state
function debugApi.app_state() end                  -- eCApplication::GetAppState
```

Verification (risk, decides the noise strategy): under app state 8, (1) does rendering go on (`ui.frame` keeps coming), (2) do
particles, vegetation/SpeedTree wind, water waves, sky clouds, animated actors, torch flicker and the clock stop, (3) does
`gCCameraAdmin::Process` still run (a fixed camera set before freezing must stay), (4) does `tick` stop. Measure with two
bursts (5.4): the intra-run difference under freeze should drop to (near) zero. If 8 does not freeze shader time, try
`time_scale(0)` (the measured frame time becomes 0; `Update` clamps it to a minimum, so time crawls instead of stopping [I]).

### 3.4 Weather: `risen.world.weather*` (new)

```lua
---@return string[]                                    names of the eCWeatherStates (index order)
function worldApi.weather_states() end
---@return { mode: integer, weights: table<string, number> }|nil
function worldApi.weather() end
---Forces the weather (mode 2) with one state at 1.0, or explicit weights; "auto" returns to zone mode 0.
---@param spec string|table<string, number>
---@return true|nil ok, string|nil reason
function worldApi.set_weather(spec) end
```

Direct memory writes as `CON_Weather` does (`+0x4c4` mode via `SetCurrentMode`, weights at state `+0x360`), or simply build
the console call of 3.6 (`weather <name>`); explicit weights need the direct writes. Rain particles (`eCPrecipitation_PS`) and
wetness take a moment to build up after a switch: switch first, settle later.

### 3.5 HUD, cursor, tutorials

```lua
function hudApi.set_visible(on) end                -- GUI2::SetPageMode(1) / RestoreUserPageMode (OnDebug_ToggleHUD)
```

The mouse cursor of the GUI is drawn by the game at the real mouse position: before a GUI capture move it to a fixed client
pixel with `risen.gui.mouse("move", x, y)` (exists). Tutorial pop-ups: `EnableTutorialSystem="false"` in the pinned
`ConfigUser.xml` (7.2) and `risen.hud.tutorial_stop` for any that still appear.

### 3.6 Console runner (new, generic)

```lua
---Runs a console command of Script_Game.dll (CON_<cmd>, phase 2) and returns its message.
---@param cmd string   e.g. "Weather", "timescale", "SetVegWindParams", "ShadowQuality", "god", "invisible"
---@param ... string   arguments
---@return boolean|nil ok, string|nil message
function debugApi.console(cmd, ...) end
```

Resolve `?CON_<cmd>@@YA_JPEAVgCScriptProcessingUnit@@PEAX1_J@Z` in Script_Game.dll (whitelist the names), build the 0x20-byte
argument struct of 2.1 with `bCUnicodeString`s (spass has `UString`), pass the player as self/other, read and destroy the
message. Covers weather, wind (`SetVegWindParams 0 0 0 0` to stop vegetation sway), quality presets, `invisible`/`god` (keep
monsters from attacking the player at a spot) without a binding each.

### 3.7 NPCs, AI, particles

No clean per-entity "pause animation" exists. Plan: time freeze (3.3) for everything; per spot `hide_npcs = radius`
(`risen.entity.npcs(center, radius)` -> `enable_rendering(false)`, identical in both runs) where NPC routines would place
people differently; `console("invisible")` so monsters ignore the player. Optional, after measuring: seed pinning with a
detour of `bCMersenneTwister::InitSeed` (constant seed) to make particle spawn patterns repeatable [I: worth it only if
time freeze leaves particles moving].

### 3.8 Test waits in a frozen world (change `src/game/hooks.cpp`)

`detourAppRender` pumps `rt.processPending()` + `test::onTick()` only when no session tick happened **and** a menu is
browsing. Add "or `risen.debug.time_freeze` is on" (a flag set by 3.3), otherwise a scenario that freezes time and then waits
stalls until the deadline (if app state 8 stops `gCSession::Process`). Waits under freeze must count frames (`ui.frame`), not
ticks: add `T.wait_frames(n)` (counts `ui.frame`) to `risen.test`.

### 3.9 Capture-parity self-test (scenario `screenshot_parity`)

Same frame through two capture paths: in D3D9 mode Present hook vs `SnapshotEx` (BMP via D3DX), in Vulkan mode
`rvk_screenshot` vs `SnapshotEx` (our slot 159). Pixels must be identical (proves that a cross-renderer difference is a
rendering difference, not a capture difference: format, row order, BGRA swizzle, alpha).

---------------------------------------------------------------------------------------------------------------------

## 4. Renderer side (this repo)

### 4.1 Exports (`src/proxy/d3d9.def`, `src/proxy/d3d9_exports.cpp`)

**Implemented (2026-10-21), final ABI.** The public header is `include/rvk_api.h` (self-contained C, copy it into spass);
it is the normative version, this is a summary.

```c
// all optional for callers: detect with GetProcAddress(GetModuleHandleW(L"d3d9.dll"), "...")
const char* rvk_version(void);                                   // "0.1.0"
uint32_t    rvk_capabilities(void);                              // RVK_CAP_SCREENSHOT 0x1 | RVK_CAP_STABLE_CAPTURE 0x2 (never
                                                                 // set yet) | RVK_CAP_STATS 0x4; 0 outside Vulkan mode
uint32_t    rvk_screenshot(const wchar_t* path, uint32_t wait_frames, uint32_t flags);   // ticket > 0, or 0 = refused
int32_t     rvk_screenshot_status(uint32_t ticket, char* msg, uint32_t msg_size);         // 0 pending, 1 done, -1 failed
uint64_t    rvk_frame_index(void);                               // presents so far (0 outside Vulkan mode)
int32_t     rvk_stats(struct rvk_stats* out, uint32_t size);     // bytes written (versioned by size), 0 if out null / size < 8

struct rvk_stats {             // v1 = 56 bytes, fields only ever appended
    uint32_t size;             // bytes written
    uint32_t mode;             // RVK_MODE_PASSTHROUGH 0, RVK_MODE_TRACE 1, RVK_MODE_VULKAN 2
    uint64_t frame_index;      // = rvk_frame_index
    uint32_t draws_last_frame;
    uint32_t unimplemented_slots;   // distinct HAL slots called that the Vulkan HAL lacks
    uint64_t draws_total;
    uint64_t draws_skipped_total;   // no shader + no target + vertex input + pipeline
    uint32_t screenshots_pending;   // waiting, capturing or encoding
    float    fps;                   // presents/s over the last second
    uint64_t texture_cpu_bytes;     // CPU copies of textures
};
```

Semantics as implemented:

* **Mode rule**: in `passthrough`/`trace` mode (and after a Vulkan session fell back to passthrough at startup)
  `rvk_capabilities()` is 0 and `rvk_screenshot` returns 0, so spass falls back to its own D3D9 path: one rule, no mode
  coupling on the spass side. Requests already queued when the fallback happens fail.
* **`wait_frames`**: 0 = the next present (from the game thread between two presents: the frame being built), n = n presents
  after that. The done message says which: `"frame <n>, waited <k> presents, <w>x<h>"`, where `<n>` is the present's
  1-based index (= `rvk_frame_index()` right after it). Pending messages: `"waiting <k> presents"` / `"writing"`; failed:
  the reason (read-back failed, write error, renderer gone, `"unknown ticket"`). The last 1024 finished tickets are kept.
* **Path**: absolute UTF-16, `.png` (opaque RGB) or `.bmp` (24-bit `BI_RGB` bottom-up) by extension (case-insensitive).
  Refused (ticket 0, a warning in rvk.log): null/empty/relative path, other extensions, a path in `<game>\data` as written
  **or after resolving links/junctions** (`weakly_canonical`), or in the target of any junction inside `<game>\data`
  (`data\common`, `data\compiled` -> the real install). Missing directories are created; the file is written as
  `<path>.tmp` and renamed over an existing file.
* **Flags**: none defined; any set bit is refused (lets a caller probe for a future flag). Reserved: 0x1 `RVK_SHOT_STABLE`,
  0x2 `RVK_SHOT_DUMP_RTS` (4.2).
* **Queue**: at most 64 requests pending (waiting + being written), then refused. Requests come from any thread (a mutex);
  `HalDevice::SwapScreen` counts the waits down and, when due, calls `dev_.readBackBuffer` **before** `dev_.present()` (the
  same point as the D3D9 Present hook and `DoSnapshot`); several requests due on the same present share one read-back. The
  read-back waits for the GPU (like `GetRenderTargetData`); PNG/BMP encoding and the file write run on a writer thread
  (`rvk screenshot writer`), so the present does not wait for them.
* **Pixels**: the raw back-buffer bytes as the game wrote them (UNORM BGRA, no sRGB re-encode, alpha dropped), top-down,
  exactly what `GetRenderTargetData` gives in D3D9. Verified: identical to the engine snapshot (slot 159) of the same frame.
* **Stats**: `pending uploads` of the draft is not provided: uploads are recorded lazily when a texture is first used, there is
  no upload queue to count; `texture_cpu_bytes` and `screenshots_pending` are there instead.
* `[debug] screenshot_frames` stays for quick debugging; it is now a request of the same queue (async, `frame_<n>.png`).
* `[debug] snapshot_frames = [n, ...]` (`run.ps1 -Snapshots`, any mode): at those `eCApplication::Draw` calls (hooked) the
  engine's `eCApplication::Snapshot()` plus a self-test of these exports through `GetProcAddress` (requests for the same
  present, a wait of 3, and the refusal cases; results as `api selftest:` lines in rvk.log, files `bin\rvk\screenshots\api_draw<n>.*`).
  Verified 2026-10-21 in Vulkan mode (also with validation: clean) and passthrough (everything refused, as specified).
* Caveat for spass: in passthrough mode the system `d3d9.dll` is loaded as well (full path), so `GetModuleHandleW(L"d3d9.dll")`
  could in principle return either module [I]; both outcomes lead to the D3D9 fallback there.

### 4.2 Flags (later, Vulkan-only diagnostics; not part of the comparison itself)

* `RVK_SHOT_STABLE`: capture when K consecutive frames differ by less than eps (64x36 thumbnail blitted and read back each
  frame), else after `wait_frames` as a timeout; the status message reports the frames waited and the residual difference.
* `RVK_SHOT_DUMP_RTS`: additionally write every render target of that frame (scene RT, shadow maps, post chain, depth as 16-bit
  PNG) with a JSON index (`frame_<n>/rt_<k>_<format>_<w>x<h>.png`): bisects a difference found by the diff tool to the pass
  that introduces it (the D3D9 run cannot do this; compare against expectations or a PIX capture of D3D9).

### 4.3 HAL slot 159 `SaveBackbufferToFile(char* path)`

**Implemented (2026-10-21)**, details in missing-slots.md 3: the virtual path (`#G3:/snapshots/Snapshot__<dd.mm.yyyy>_<NNNN>.bmp`)
goes through `eCVirtualFileSystem::MakeAbsPhysicalPath`; refused inside `<game>\data`; the file is the BMP D3DX writes for
the X8R8G8B8 back buffer (**32 bpp** with the stored 4th byte, not 24-bit; header identical byte for byte to a D3D9 run's).
The engine's own snapshot (`Snapshot`, `SnapshotEx`, `CON_CameraDump`, the debug key) now works in Vulkan mode, so 3.9 can
use it; in Vulkan mode its pixels equal `rvk_screenshot`'s for the same present (verified). `SaveBackbufferToMemory`
(slot 160) has no game caller and stays a stub.

### 4.4 Timing: "capture after N stable frames"

The comparison does not rely on the renderer to decide stability; both runs follow the same engine-level script (5.4):
camera settled -> weather/clock set -> wait for post-processing adaptation (fixed number of frames) -> time freeze -> wait
`F` frames (default 30) -> burst of 3 captures 10 frames apart. Identical in both renderers by construction. The renderer's
`RVK_SHOT_STABLE` is a diagnostic: it tells how many frames the scene needed to settle in Vulkan mode, which calibrates `F`.

---------------------------------------------------------------------------------------------------------------------

## 5. Scenario: `mods/rvkcompare` (in the risen-spass repo, beside `rvk_shadows`)

It lives in spass because it uses spass APIs and must pass spass's LuaLS check; thresholds and known-difference notes live
here (`tests/refcompare/thresholds.toml`, 6.4).

### 5.1 Layout and parameters

```
mods/rvkcompare/
  mod.toml          id = "rvkcompare"
  main.lua          the scenario (risen.test), reads spots.lua
  spots.lua         the spot table (5.2), data only
```
`-Param tag=d3d9|vk` (file prefix), `spots=all|a,b,c`, `burst=3`, `interval=10`, `freeze=1`, `settle=240` (frames),
`mode=run|calibrate`. `run.ps1 -Param` already passes these through.

### 5.2 Spot schema

```lua
return {
  { id = "beach_rain",
    player = { x = -38559, y = 15, z = -5770, yaw = 58 } | { entity = "<guid>" } | "start",  -- where the player stands
    camera = { eye = {x,y,z}, target = {x,y,z}, fov = 60 } | { follow = true, zoom = 0 },     -- fixed or follow camera
    hide_player = false, hide_npcs = 0,          -- radius in engine units
    time = { 10, 0 }, weather = "Game_Start",    -- a state name, a weights table, or "auto"
    hud = false, gui = nil,                      -- nil | { page = 2 } | { page = 3 } | { key = 0x1b } ; cursor = {x, y}
    post = "default",                            -- informational: the post effect comes from ConfigUser.xml (7.2)
    settle = 240, burst = 3,                     -- per-spot overrides
    notes = "rain, sea, refraction, palms",
  },
}
```

### 5.3 The spots (anchors from the layer files [I]; final numbers from calibration, 5.5)

| id | where (anchor) | time / weather | camera, extras | exercises | noise risk |
|---|---|---|---|---|---|
| `beach_rain` | the new-game beach, no teleport (`_Intern.lrent` `PC_Hero` -38559, 15, -5770; `WeatherZone_Gamestart`) | 10:00, `Game_Start` (storm + rain) | fixed, along the shore to the sea, HUD off | **rain particles + splashes, sea water (refraction copy), wet look, palms (SpeedTree), fog** (the user's "rain on the beach") | high: rain, thunder flashes (`ThunderProbability`), waves |
| `beach_sun` | same pose | 13:00, `Sunny_Full` | same | sun shadows (cascades), water specular/sun glint, sky dome, bloom | low/medium |
| `waterfall_jungle` | Wild_Outdoor waterfall (`VFX_EMT_Waterfall_Water_Splash_01` 8475, 2440, 9403) | 15:00, `Sunny_Half` | fixed, waterfall + foliage | dense alpha-tested foliage, grass instancing, flowing water, spray particles, wind | medium (wind, spray) |
| `harbour_docks` | Harbour outdoor ships (`Obj_Dec_Ship_01` 13402, -51, -25742) | 18:30, `Sunny_Half_02` | fixed, docks + sea + town | low sun: long shadows, sunset colours, large water, distant LOD, fog | low |
| `harbour_night` | Harbour Town street with ground torches (`Harbour_Town_Dyn_Light`, `Obj_LS_Torch_Ground_01`) | 23:30, `Sunny_Full` (clear night) | fixed, HUD off | **night**: many point lights, light falloff, moon/stars, dark tonemapping, fire particles | medium (flicker, fire) |
| `pub_interior` | Harbour pub (`Obj_LS_Sconce_Harbour_01` -1084, 872, -19114; candles at -930, 682, -19479; floor y ~682) | 20:00, weather `"auto"` captured at calibration (interior zone) | fixed, `hide_npcs = 800` | **interior with lights**: candles, sconces, indoor ambient, normal maps | medium (flicker) |
| `monastery_hall` | Monastery (`Hlp_Story_Initiation_PlayerStart` 12966, 13350, 28251; candelabras, wall torches) | 12:00, zone state from calibration | fixed, `hide_npcs` | stone interiors, light shafts/windows, shadows of point lights | medium |
| `lava_dungeon` | Vulcan dungeon (`VFX_EMT_Lava_Smoke_02_Red` -11, 10527, 59753) | any, `Dungeon_Lava` | fixed | emissive lava, heat distortion (post `distort`), smoke, red point lights | high (smoke) |
| `hero_closeup` | `beach_sun` area, player at a fixed pose | 13:00, `Sunny_Full` | follow camera, `ZoomIn` 1 s (RE.md 46) or fixed orbit `target = player head, distance = 150`; **HUD on** | skinned mesh, armour/skin/hair, specular, self-shadowing + the HUD over the world (bars, compass) | low (idle animation: freeze) |
| `gui_inventory` | after `hero_closeup` | - | `risen.gui.open(2)`, cursor parked at (10, 10) | 2D: panels, item icons (DXT), **text** (fonts), the paused world behind | low |
| `gui_character` | same | - | `risen.gui.open(3)` (the C screen) | text-heavy page: attribute/skill lists, numbers | low |
| `menu_main` | in-game ESC menu (page 13 + background 27, via `risen.gui.key(0x1b)`); optional title screen in a separate short `-NoAutostart` run | - | cursor parked | menu bar, tab images, title font | low |

The first nine cover: beach + rain + water, vegetation, interiors with lights, night, shadows (sun cascades and point lights),
post effects (bloom/DoF/distort), and characters; the last three cover the GUI with text. Order the run so that far teleports
are few (beach -> harbour -> pub -> monastery -> waterfall -> lava), and do the GUI spots last.

### 5.4 Per-spot flow

```
T.section(id)
 1. weather + clock      set_weather(spot.weather); set_time(h, m)        (before travelling: rain builds up while loading)
 2. player               set_pose / teleport (y from risen.world.trace down); console("invisible"); hide_player?
 3. camera               risen.camera.set{...} (or follow: ResetCamera / ZoomIn); wait until camera.settled() (timeout 10 s)
 4. world                hide_npcs; hud.set_visible(spot.hud); gui page if any; gui.mouse("move", cursor)
 5. settle               wait settle frames (default 240 ~ 4 s at 60 fps): streaming, LOD fades, eye adaptation, DoF focus
 6. re-pin               set_time(h, m) again (the clock ran while settling)
 7. freeze               debug.time_freeze(true); wait 30 frames
 8. capture              debug.screenshot(tag.."_"..id, { burst = 3, interval = 10 }); wait until not pending
 9. log                  risen.log.info("RVKCMP " .. json{ id, tag, backend, frame, camera = camera.get(), weather = world.weather(),
                                                         time = {world.time()}, app_state, player pose, files })
10. unfreeze             time_freeze(false); close GUI pages; release camera; restore NPC rendering
```
Each step is a `T.expect` (missing APIs or a failed teleport fail the spot, not the run). The JSON line is the metadata
channel (no file I/O in the sandbox is needed): the orchestrator extracts `RVKCMP {...}` lines from `spass.log`.

### 5.5 Calibration (once per spot, by hand or by an agent with screenshots)

`-Param mode=calibrate,spots=pub_interior`: teleports to the anchor, applies `auto` weather, logs `risen.world.weather()`
(the zone's weights at that place: copy them into `spots.lua`, so both renderers get the same forced weights), the player pose
after `nav_point`, and takes one screenshot per candidate camera (eye offsets around the anchor). Pick the best, write the
numbers into `spots.lua`. Spots are data, so tuning never touches code.

---------------------------------------------------------------------------------------------------------------------

## 6. Diff tool: `tools/refcompare.py` (this repo)

Python 3.12 with numpy, Pillow, scipy (all installed here; no scikit-image/OpenCV needed: SSIM via `scipy.ndimage`).

**Implemented (2026-10-21)**: `tools/refcompare.py`, tests `tools/tests/test_refcompare.py` (unittest, 25 cases on synthetic
images: identical, shifts, brightness/colour bias, missing square, burst noise mask, lightning drop, size/missing/pose ->
INVALID, threshold files, calibrate, CLI exit codes). As built, where it differs from the draft below:

* CLI: `compare A B -o out [--thresholds f.json|f.toml] [--meta-a f --meta-b f] [--baseline A2 [--meta-baseline f]]
  [--tag-a t --tag-b t] [--spots a,b] [--no-images]`, `calibrate A A2 -o thresholds.json [--margin 1.5]`, `selftest`.
  A/B are directories or single image files (PNG/BMP). No thresholds file is read unless given (built-in defaults = 6.4).
* Pairing: `<spot>.png` or bursts `<spot>_b<k>.png`; the tag prefix (`d3d9_`, `vk_`) is stripped from `--tag-*`, the
  metadata's `tag`, or automatically (a first token common to all files of a side) when the names do not match otherwise.
  A spot on one side only is INVALID (`missing in A/B`).
* Metadata: `<dir>/meta.json` by default; accepts `RVKCMP {json}` log lines (spass.log as is), JSONL, a JSON list,
  `{spots: [...]}` or `{<id>: {...}}`. Checked: camera `x,y,z` (eye), `dx,dy,dz` (angle), `fov`; weather `mode` + `weights`
  (or a state name string); `time` as `[h, m]`, `{h, m}` or hours (wraps at midnight). Missing fields are listed as unchecked.
* Units: `bad8`/`bad32`/`noise` in **percent**; `dluma`, `bias_rgb` = B - A (test minus reference); `chroma` = (R-G, B-G) of
  `bias_rgb`. `shift`: phase correlation of the lumas (Hann window, peak in [-4, 4]^2, parabolic sub-pixel `shift_sub`);
  `shift_significant` only when aligning by it lowers the luma MAE below 75 % (an unrelated image has a peak too); only a
  significant shift counts for `shift_max`. `tiles`: 32x32 tiles (`--tile`) ranked by MAE, top 10 with x, y, mae, bad32, ssim.
* Verdict rules are per tier (`pass`, `warn`): keys `ssim_min psnr_min psnr_q_min mae_max bad8_max bad32_max dluma_abs_max
  chroma_abs_max shift_max`; plus `invalid.noise_max`, `noise.{burst_tol, dilate, flash_rel}`, `pose.{eye, dir_deg, fov,
  weather, clock_min}`, per spot under `spot.<id>` with `known = "..."`. Exit code 2 wins over 1 (INVALID before FAIL).
* Calibrate / `--baseline`: per spot and rule, error limits `max(default, margin x floor)`, `ssim_min = min(default,
  1 - margin x (1 - ssim))`, PSNR limits `min(default, psnr - 20 log10 margin)`; never stricter than the defaults; the floor
  metrics are stored as `spot.<id>.baseline`.
* Outputs per spot: `_diff.png` (heat map as specified), `_ssim.png` (1 - SSIM, x4), `_blink.gif` (A/B at 1 Hz, labelled),
  `_side.jpg` (A | B | diff at half size), `_tiles.png` (worst three tiles, x4 nearest, A | B | diff), `_a.png`/`_b.png`
  (the burst medians); `report.html` (self-contained: embedded thumbnails, links to the files), `report.json`, `summary.md`.
* First use on existing ad-hoc sets (not aligned in time; sanity check only): pixel-identical main menu D3D9 vs Vulkan (PASS,
  MAE 0); the character page after the font fix PASS (psnr_q 43.8 dB, was WARN at 35.6 before it), the remaining
  differences in the world behind the page (sky, swaying palms); world shots FAIL with the expected brightness/shadow differences.

### 6.1 Inputs and checks

`refcompare.py <ref_dir> <test_dir> [--thresholds tests/refcompare/thresholds.toml] [--out dir] [--baseline <ref2_dir>]`.
Pairs files by spot id and burst index; reads both `meta.json` files (the RVKCMP lines). Before comparing pixels:
* sizes equal (else INVALID);
* **pose check**: camera eye distance < 1 unit and view direction < 0.1 degree, same weather weights and clock, else the spot is
  `INVALID: pose/state mismatch` (a scenario problem, not a renderer bug);
* burst consistency inside each run (6.2).

### 6.2 Noise model

* **Intra-run noise mask**: for each run, pixels whose max channel differs by more than 6/255 between any two burst frames are
  unstable (particles, flicker, anything frozen time did not freeze). Mask = union of both runs' unstable pixels, dilated by
  2 px. Metrics are computed on the unmasked pixels; the masked fraction is reported as `noise`. With a perfect time freeze the
  mask is empty and the comparison is strict.
* **Representative frame**: per-pixel median of the burst (removes single-frame flashes).
* **Lightning**: a burst frame whose mean luma deviates > 10 % from the burst median is dropped (thunder flash).
* **Baseline** (`--baseline`, recommended once per spot set): compare two independent D3D9 runs with the same tool; their
  metrics are the floor of what is achievable at that spot (run-to-run nondeterminism of the game itself). Thresholds become
  `max(default, 1.5 x baseline)` per spot and metric.

### 6.3 Metrics (per spot, on unmasked pixels; 8-bit sRGB values as stored)

| metric | definition | catches |
|---|---|---|
| `mae` | mean absolute error over RGB (0..255) | general |
| `bad8`, `bad32` | fraction of pixels whose max-channel difference > 8 / > 32 | localised errors (missing objects, wrong shader) |
| `psnr` | 20 log10(255 / RMSE) | general |
| `psnr_q` | PSNR after 4x4 box downsampling | the same, tolerant to sub-pixel/AA/filtering differences |
| `ssim` | mean SSIM of Rec.709 luma (Gaussian window sigma 1.5) | structure (blur, missing detail, shifted geometry) |
| `dluma` | signed mean luma difference | gamma/sRGB/tonemapping bias, exposure |
| `chroma` | mean difference of the per-channel means (R-G, B-G) | colour-space / channel swizzle |
| `shift` | best integer offset in [-2, 2]^2 maximising correlation | half-pixel / viewport offsets (expect (0, 0)) |
| `tiles` | the metrics on a 16 x 9 grid, worst 5 tiles listed | where it goes wrong |

### 6.4 Verdicts and thresholds

`tests/refcompare/thresholds.toml` (our repo): `[default]` and `[spot.<id>]` overrides, plus `known = "..."` notes for open
features (PLAN.md M3/M4: occlusion queries report visible -> lens flares/sun glow, colour-only shadow maps, ...), shown in the
report next to the verdict. Starting values, to be replaced by the baseline rule of 6.2:

| verdict | rule |
|---|---|
| PASS | `ssim >= 0.98`, `bad32 <= 0.2 %`, `abs(dluma) <= 1.0`, `psnr_q >= 38 dB`, `shift == (0,0)` |
| WARN | `ssim >= 0.95`, `bad32 <= 1 %`, `abs(dluma) <= 3.0` |
| FAIL | otherwise |
| INVALID | missing image, size or pose/state mismatch, `noise > 30 %` |

GUI spots get stricter `bad32` and looser `ssim` near text edges (glyph rasterisation through `eCAPIFont::GetGlyphData` may
differ by a pixel): judged by `psnr_q` and `bad32`.

### 6.5 Outputs (`out\refcompare\<stamp>\report\`, git-ignored)

* per spot: `<id>_diff.png` (max-channel difference, x4, colour ramp black-blue-yellow-red clamped at 64, over a desaturated
  reference at 50 %; masked pixels hatched grey), `<id>_ssim.png`, `<id>_blink.gif` (A/B alternating at 1 Hz), side-by-side
  crops of the worst three tiles;
* `report.html` (table sorted by severity, thumbnails linking to full size, metrics, known notes, pose deltas), `report.json`
  (machine-readable, for trend tracking), `summary.md` (paste into PLAN.md);
* exit code 0 (all PASS/WARN), 1 (any FAIL), 2 (any INVALID or nothing compared).
* `--selftest`: synthetic images (identical, +1 brightness, 1 px shift, a missing square, noise) must give the expected verdicts:
  a unit test without the game.

---------------------------------------------------------------------------------------------------------------------

## 7. Orchestration: `scripts\refcompare.ps1` (this repo, wraps `run.ps1`)

### 7.1 Steps

```powershell
.\scripts\refcompare.ps1 [-Spots all|a,b] [-Baseline] [-SkipReference <dir>] [-GameDir B:\DevDrive\risen-test-vk-cmp] [-Seconds 900]
```
1. Free RAM check; kill a `steamwebhelper.exe` above 5 GiB (user instruction). Refuse `B:\SteamLibrary*`.
2. **Dedicated test copy** `B:\DevDrive\risen-test-vk-cmp` (created by `new-test-copy.ps1` through `run.ps1 -GameDir`): `-NoRenderer`
   deletes `d3d9.dll` from `bin`, which would silently change the renderer of another agent's run in the shared
   `risen-test-vk`. Use `-Instance n` when another Risen runs.
3. Pin the game configuration (7.2) before **each** run (the game rewrites `ConfigUser.xml`).
4. Run A (reference): `run.ps1 -NoRenderer -Mods rvkcompare -Scenario rvkcompare -WaitForResult -Param tag=d3d9,spots=...`.
   Move `bin\spass\screenshots\*.png` and the `RVKCMP` lines of `bin\spass\spass.log` (-> `meta.json`) to `out\refcompare\<stamp>\d3d9\`;
   keep `test-result.json` and the log.
5. Run B: `run.ps1 -Mode vulkan ...tag=vk` -> `...\vk\` (+ `bin\rvk\rvk.log`: unimplemented slots, validation messages).
6. Optional `-Baseline`: run A a second time -> `...\d3d9_b\`.
7. `python tools\refcompare.py out\refcompare\<stamp>\d3d9 out\refcompare\<stamp>\vk [--baseline ...\d3d9_b]` -> report;
   print the summary table and the report path; exit with its code (or 2 when a run produced no result).
8. `-SkipReference <dir>` reuses a stored reference set (the D3D9 side does not change between renderer commits): one game
   run per comparison (~5-8 minutes for 12 spots instead of twice that). Invalidate it when spass, the spots or the config change
   (store their hashes in `meta.json`).

Sequential by default. Running A and B in parallel (two instances) halves the wall time but shares the GPU and changes
loading order; allowed once time freeze is proven to make the captures independent of timing.

### 7.2 Pinned configuration

A fixed `ConfigUser.xml` graphics block written before each run: `ViewDistance`, `Shadows Quality`, `PostProcessing
DefaultEffect` (two report variants: `DoF` as shipped and `None` to separate post-processing differences from the scene),
`TextureFilterMode`, `Vegetation/Effect Quality`, `Cache Preset`, `Window 1600x900`, `Vsync`, `FOV="60"`,
`EnableTutorialSystem="false"`, `UserPageMode` fixed. `test-run.ps1` already rewrites window size, audio and languages; the
graphics block can be a new `-ConfigUserXml <file>` switch there or done by `refcompare.ps1` before calling `run.ps1`.

---------------------------------------------------------------------------------------------------------------------

## 8. Determinism problems and mitigations

| # | Source of difference | Mitigation |
|---|---|---|
| 1 | new-game intro camera fly-in (~25 s), "Press START" page 26 | wait for `session.start` + intro (as `rvk_shadows`), `risen.gui.close(26)`; spots start after it |
| 2 | weather: random state transitions of `eCWeatherEnvironment` in zone mode 0 | force mode 2 with explicit weights per spot (3.4); interiors get their zone state from calibration (5.5) |
| 3 | thunder/lightning flashes (`ThunderProbability`, `eff_env_thunder`) | prefer `Game_Start`/`Foggy_Rain` for rain spots if they flash less; burst + luma-outlier drop (6.2) |
| 4 | the clock runs (sky, sun direction, shadow direction) | `set_time` right before freezing; time freeze |
| 5 | animation, wind (vegetation, SpeedTree), water waves, clouds, particles, light flicker | time freeze (app state 8) [I]; else intra-run noise mask + burst median (6.2); `SetVegWindParams 0 0 0 0` |
| 6 | post-processing history: eye adaptation (HDR), bloom, DoF auto-focus | settle frames before freezing; second report variant with `DefaultEffect="None"` |
| 7 | streaming / LOD fade-in after a teleport (worker threads, timing-dependent) | fixed settle frames, burst consistency check (big unstable areas -> retry the spot once later), order spots to minimise far jumps; Vulkan-only `rvk_stats` pending uploads as a diagnostic |
| 8 | camera smoothing/collision of the follow camera | fixed camera (`SetFixCamera`, instant); pose logged and compared (6.1) |
| 9 | NPC routines place people differently; monsters attack | `hide_npcs` radius, `console("invisible")`, spots without NPCs in view, player hidden or behind the camera |
| 10 | random seeds (Mersenne twister): particle spawn patterns | noise mask; optional seed pinning (3.7) |
| 11 | frame pacing/vsync: the frame at which a capture happens differs | frozen time makes it irrelevant; captures are frame-counted, not time-based |
| 12 | configuration drift (`ConfigUser.xml` rewritten by the game, other agents' runs) | pinned config before each run, dedicated test copy (7) |
| 13 | mouse cursor, tutorial pop-ups, HUD log messages and their timers | cursor parked (3.5), tutorials off, HUD off for world spots, GUI spots taken after messages expired |
| 14 | autosave trigger zones (`OutMain_AutosaveTriggerZones`) on teleports: "saving" hitch/HUD | HUD off; `CON_AutoSave` off if it shows up |
| 15 | driver-level differences that are not bugs (D3D9 driver vs Vulkan: anisotropic filtering, mip selection, precision, glyph rasterisation) | `psnr_q`/SSIM thresholds; D3D9-vs-D3D9 baseline separates game noise from renderer differences |
| 16 | known open renderer features (occlusion queries "visible" -> sun flares, colour-only shadow maps, ...) | `known` notes per spot in `thresholds.toml`; they turn into hard thresholds when implemented |
| 17 | capture path differences (format, swizzle, alpha) | both capture the presented back buffer, raw bytes; parity self-test 3.9 |

---------------------------------------------------------------------------------------------------------------------

## 9. Phasing and effort

| Part | Content | Effort |
|---|---|---|
| spass: screenshot backends | rvk backend, `wait_frames` fix, burst, `screenshot_backend`, `frame`; d.lua, docs | 0.5-1 d |
| spass: camera | `risen.camera.set/release/settled/get` on `SetFixCamera`; in-game verification; free-fly fallback if needed | 1-2 d (RE risk) |
| spass: time | `time_freeze`, `time_scale`, `fixed_frame_time`, `app_state`; `ui.frame` pumping under freeze; `T.wait_frames`; the freeze verification of 3.3 | 1 d |
| spass: weather, HUD, console | `weather_states/weather/set_weather`, `hud.set_visible`, `debug.console` (+ hardening run) | 1 d |
| renderer: exports | `rvk_screenshot` queue + writer thread + status, `rvk_frame_index`, `rvk_capabilities`, `rvk_stats` | 0.5-1 d |
| renderer: slot 159/160 | `SaveBackbufferToFile/Memory` | 0.5 d |
| renderer: diagnostics (later) | `RVK_SHOT_STABLE`, `RVK_SHOT_DUMP_RTS` | 1-2 d |
| mod `rvkcompare` | scenario, spot schema, calibration mode, calibrating 12 spots in the game | 1.5-2 d |
| `tools/refcompare.py` | metrics, masks, verdicts, heat maps, HTML/JSON report, self-test | 1-1.5 d |
| `scripts/refcompare.ps1` | two runs, config pinning, collection, report, reference reuse | 0.5-1 d |
| **total** | | **~8.5-13 d** |

Suggested order: (1) spass screenshot backend + renderer exports (a usable A/B capture with today's follow camera:
`set_pose` + `ResetCamera`), (2) diff tool + orchestration on 3-4 spots (`beach_rain`, `beach_sun`, `pub_interior`,
`gui_inventory`) with the noise mask, (3) time freeze + weather + HUD (most noise goes away), (4) fixed camera, the remaining
spots and the D3D9 baseline, (5) renderer diagnostics. Steps 1-2 already give M4 a measurable signal.

---------------------------------------------------------------------------------------------------------------------

## 10. Verification checklist (in the game, before relying on the design)

* `rvk_screenshot` and the D3D9 Present hook give identical pixels for the same frame through `SnapshotEx` (3.9).
* App state 8: rendering continues; what stops (particles, wind, water, sky, actors, clock, shader time); `tick` and
  `gCCameraAdmin::Process` under 8; returning to 0 restores everything. Record in `docs/research/engine-gfx.md` and spass RE.md.
* `SetFixCamera` (both overloads, `instant = true`): the render camera equals the request; it holds under time freeze; FOV unit
  (degrees vs radians: the camera stores radians, `CamFOV` property unit [I]); `StopFixCamera` restores the follow camera.
* `set_weather` mode 2: rain and wetness appear, interiors keep their look with their zone state; `"auto"` returns to zones.
* `GUI2::SetPageMode(1)` hides the whole HUD (bars, compass, log) and `RestoreUserPageMode` brings it back.
* `set_time` drives the sky/sun daytime of the weather admin (2.4).
* The layer-file anchors of 5.3 are world positions (teleport to each and read `risen.world.camera()`).
* Two D3D9 runs of the full spot list: baseline metrics, run time, peak RAM.
