# Design: D3D9 reference comparison at fixed spots

Status: **built and run end to end (2026-10-21)**: renderer exports (4.1, 4.3), spass controls (3, risen-spass RE.md 58-60), the
scenario `mods/rvkcompare` (5), the diff tool (6), the orchestration `scripts\refcompare.ps1` (7); determinism (8.1); results (11).
The design text below is kept; the as-built parts say so. Goal (user decision): run the **same deterministic scenario** once
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
* States of the shipped game (26, read in the running game by `risen.world.weather_states()`, spass RE.md 59 [C]):
  `Sunny_Full`, `Sunny_Half`, `Sunny_Half_02`, `Stormy`, `Stormy_Rain`, `Foggy`, `Foggy_Rain`, `Swamp_Don_Sunny`,
  `Gyrger_Sunny_Temp`, `Gyrger_Sunny_Green`, `Dungeon_Blue`, `Dungeon_Blue2..5`, `Dungeon_Blue5_library`, `Dungeon_Blue6_Big`,
  `Dungeon_Green`, `Dungeon_Green2`, `Dungeon_Green_Big`, `Dungeon_Grey`, `Dungeon_Grey2`, `Dungeon_Red_Big`, `Dungeon_Swamp`,
  `Dungeon_MiniDungeons1`, `_DefaultEdit_Setting`. There is **no** `Game_Start`, `Swamp` or `Dungeon_Lava` (an earlier offline
  read of `WeatherStates.g3ws` listed them wrongly): the new-game beach in zone mode is `Stormy_Rain` ~0.98 + `Gyrger_Sunny_Green`
  ~0.02 (the blend drifts with the zones' random transitions), the volcano dungeon's zone gives `Dungeon_Blue6_Big` 1.0. They
  carry `ThunderProbability`, `EffectProbability`/`RandomEffect` (`eff_env_thunder`), `WindPower`, precipitation parameters.
* The daytime fed to the states comes from an `fmod` of a clock value (`FUN_1804ad148` = `fmodf`) [C]; that it is the world
  clock that `risen.world.set_time` sets is [I] (spass screenshots at noon support it). `SetCurrentDayTime` (`0x713ef0`) only
  writes `+0x48`, which goes to `eCWeatherEnvironment::Process`, **not** to the sky: do not use it.
* Important for interiors: mode 2 skips the zones, so a forced outdoor state also lights a dungeon. Interiors must force their
  zone's own weights (`Dungeon_Blue6_Big` in the volcano dungeon, ...), read in a calibration run, see 5.5.

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

## 5. Scenario: `mods/rvkcompare` (this repo)

**Built (2026-10-21).** Renderer scenario mods live in this repo (`mods/`, user rule); `scripts\run.ps1` hands
`-ModPath <repo>\mods` to spass's `test-run.ps1`. Type-checked with `cd mods; lua-language-server --check . --configpath
.luarc.json --checklevel Warning` against spass's `types/`.

### 5.1 Layout and parameters

```
mods/rvkcompare/
  mod.toml          id = "rvkcompare"
  main.lua          the scenario (risen.test), reads spots.lua (require "spots")
  spots.lua         the spot table (5.3), data only
```
`-Param` keys: `tag` (file prefix, `d3d9` / `vk`), `spots` (`all` or ids separated by `;`), `mode` (`run` | `calibrate`),
`burst` (3), `interval` (10 frames), `settle` (default 600 frames; per spot `settle`), `freeze_wait` (60 frames),
`start_wait` (30 s after the new game: the intro camera), `wind` (`0` default: `SetVegWindParams 0 0 0 0`, grass and leaves
rest; `1` keeps the wind), `repeat` (n: every spot n times in a row as `<id>_r<k>`: the noise of one session), `pin_time`
(`1` default: `risen.debug.set_game_time` before each freeze, 8.1).

### 5.2 Spot schema (as built)

```lua
{ id = "beach_rain",
  player = { x = -38559, y = 15, z = -5770, yaw = 58 },   -- anchor, snapped to risen.world.nav_point (snap = false: as is)
  camera = { target = { 0, 120, 0 }, yaw = 238, pitch = -6, distance = 700, fov = nil },  -- target relative to the snapped
                                                           -- player (abs = true: absolute); yaw/pitch = view direction
  hide_player = true, hide_npcs = 2500,                    -- radius: NPCs/monsters not rendered
  time = { 10, 0 }, weather = "Stormy_Rain",               -- a state name or { State = weight }; always thunder = false
  hud = false, gui = nil,                                  -- { page = 2 } | { page = 3 } | { key = 0x1B, page = 13 }
  settle = 900, notes = "..." }
```

### 5.3 The spots (calibrated: every anchor was teleported to and looked at, mode=calibrate)

| id | where (anchor x, y, z) | clock, weather | camera | exercises |
|---|---|---|---|---|
| `beach_rain` | new-game beach, `PC_Hero` (-38559, 15, -5770) | 10:00, `Stormy_Rain` | yaw 238 (to the sea), 700 | rain streaks, sea (refraction copy), wet ground, fog, splash particles |
| `beach_sun` | same | 13:00, `Sunny_Full` | same | sun shadows, water specular, sky dome + clouds, bloom |
| `jungle` | Wild_Outdoor forest path (`Nav_Path_High` 8130, 5541, -1188) | 15:00, `Sunny_Half_02` | yaw 0, pitch -12, 1200 | dense foliage (alpha test), SpeedTree, grass, dappled shadows |
| `harbour_docks` | Harbour quay, `SP_Harbour` (-5588, 245, -22007) | 18:30, `Sunny_Half_02` | yaw 180 (pier, ship, harbour basin), 900 | large water, ship and rigging, stone, low sun |
| `harbour_night` | Harbour Town square (`Obj_LS_Torch_Ground_01` -4361, 486, -19659) | 23:30, `Sunny_Full` | yaw 90, 800 | night: point lights (lamps, torches), dark tonemapping, stars |
| `pub_interior` | Harbour pub (-1100, 600, -19300; candles at y 682) | 20:00, zone weights `Sunny_Half_02` 0.87 + `Sunny_Full` 0.13 | yaw 90, 400 | candles, sconce, indoor ambient, wood/plaster normal maps |
| `monastery_hall` | monastery hall with candelabras (7100, 12549, 22040) | 12:00, `Sunny_Full` (zone) | yaw 90, 600 | many candle lights, banners, stone interior |
| `lava_cave` | volcano dungeon, `SP_BD_VulcanLeft` (-3009, 10397, 60283) | 12:00, `Dungeon_Blue6_Big` (zone) | yaw 150, pitch -10, 250 | emissive lava, red lighting, dungeon fog, heat distortion (moves under the freeze: 80 % burst noise allowed) |
| `crystal_cave` | Dungeon_Caves crystal cave (`Nav_Zone_Pos` -343, 5361, 11529) | 12:00, zone weights `Sunny_Full` 0.67 + `Sunny_Half_02` 0.33 | yaw 270, pitch -8, 500 | glowing crystals (coloured point lights), wet rock, water, cave fog |
| `hero_closeup` | beach, player visible (yaw 58) | 13:00, `Sunny_Full` | yaw 238 at the player's face, 220; **HUD on** | skinned mesh, cloth, self-shadowing, HUD bars and compass |
| `gui_inventory` | as `hero_closeup` | same | same, `risen.gui.open(2)` | panels, icons, text over the world |
| `gui_character` | as `hero_closeup` | same | same, `risen.gui.open(3)` | the C screen: attribute/skill lists, numbers, text |
| `menu_esc` | as `hero_closeup` | same | same, ESC (`gui.key(0x1B)`, page 13) | menu bar, title artwork, font |

13 spots. Order: beach (no teleport) -> forest -> harbour -> pub -> monastery -> volcano -> cave -> beach again for the character and GUI
spots. The fixed camera (`cam_cinema`) draws the 2.37:1 letterbox bars (rows 113..787 of 900, spass RE.md 59) in both
renderers; the GUI spots draw over them.

### 5.4 Per-spot flow (as built)

```
 1. clock held: set_time(h, m) on every tick (ticks stop under the freeze); set_weather(spot.weather, { thunder = false })
 2. player: set_pose at the anchor (+50 up), 90 frames (the world part streams in), nav_point snap, set_pose again
 3. hide the player / NPCs in hide_npcs; camera.set{target, yaw, pitch, distance, instant}; HUD on/off
 4. settle frames (600, the beach spots 900: rain, wetness, sea need hundreds of frames after a weather switch)
 5. re-pin: hiding, HUD and camera again, retried until camera.get().type == "cam_cinema" and settled
    (a far teleport streams a new world part in, which brings back cam_normal, the HUD and the player's rendering)
 6. set_time again; set_game_time(20000000 + 1000000 * spot position), 2 frames; time_freeze(true); freeze_wait frames (60)
 7. GUI spots: open the page / press ESC, park the cursor at the bottom right, 60 frames
 8. debug.screenshot(<tag>_<id>, { burst = 3, interval = 10, wait_frames = 1 }); wait until written
 9. log  RVKCMP {"id","tag","backend","status","files","frame","camera":{x,y,z,dx,dy,dz,fov,yaw,pitch,type},
                 "weather":{mode,weights,dominant,thunder},"time":[h,m],"player","anchor","hud","gui","game_ms","playing_s",...}
10. close the GUI, unfreeze, HUD on, restore rendering
```
Each spot is one check (`spot <id>`): an error or a failed capture fails the spot, not the run. Before the first spot:
`console("invisible")`, `console("god")`, `console("SetVegWindParams", 0, 0, 0, 0)`.

### 5.5 Calibration

`run.ps1 ... -Mods rvkcompare -Scenario rvkcompare -Param mode=calibrate[,spots=...]`: per spot weather `"auto"` (zones) at
the anchor for 240 frames, then `cal_<id>_spot.png` (the spot's camera) and `cal_<id>_y000/090/180/270.png` (around the
player), and one `RVKCAL {json}` line with the snapped pose and the zone weights (`zone_weather`). The weights of interiors
and dungeons go into `spots.lua` (forced weather ignores zones). Measured zone weights: beach `Stormy_Rain`/`Gyrger_Sunny_Green`
(drifting: 0.999/0.001 at 10:00, 0.436/0.564 at 13:00 in the same run), forest `Sunny_Half_02` 0.79 + `Sunny_Full` 0.21,
harbour `Sunny_Half_02` 1.0, Harbour Town and pub `Sunny_Half_02`/`Sunny_Full`, monastery `Sunny_Full` 1.0, volcano dungeon
`Dungeon_Blue6_Big` 1.0.

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
* Known-noise masks (added with the scenario, 8.1): `calibrate A A2` also writes `<out stem>_masks/<spot>_known.png` (pixels
  where the two reference runs differ by > `noise.known_tol` = 8, or that are unstable in either burst, dilated by
  `noise.known_dilate` = 4 px) and stores the relative path as `spot.<id>.known_mask`; `compare --thresholds` loads it,
  `compare --baseline A2` computes it on the fly (`--no-known-mask`: thresholds only). The mask is excluded from the metrics
  like the burst mask; `noise_burst` / `noise_known` are reported separately, `invalid.noise_max` applies to the burst noise,
  `invalid.known_max` (75 %) to the mask. A reference spot whose pose differs between A and A' gets neither floor nor mask.
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

### 7.1 How to run (as built, 2026-10-21)

```powershell
.\scripts\refcompare.ps1                                   # game D3D9 run + Vulkan run, all spots, report
.\scripts\refcompare.ps1 -Calibrate                        # two D3D9 runs -> out\refcompare\<stamp>\thresholds.json + masks
.\scripts\refcompare.ps1 -ReferenceDir out\refcompare\<stamp>   # Vulkan run only, against a stored reference
.\scripts\refcompare.ps1 -Spots beach_rain,pub_interior -Baseline   # + a second D3D9 run used as --baseline
```
Other switches: `-GameDir` (default `B:\DevDrive\risen-test-vk-cmp`; `risen-test-vk` is refused because `-NoRenderer` deletes
its `d3d9.dll`), `-Instance` (3), `-Seconds` (1500), `-Param 'settle=900','burst=5'` (scenario parameters, 5.1),
`-PostEffect DoF|None|...`, `-Quality`, `-WindowSize`, `-Validation`, `-NoImages`, `-Config`.
1. Free RAM is printed; a `steamwebhelper.exe` above 5 GiB is killed (user instruction). The real install is refused.
2. The dedicated copy is created on first use (`new-test-copy.ps1`). The renderer is **snapshotted** at the start
   (`build\<Config>\d3d9.dll` -> `build\refcompare-snapshot\`, deployed with `run.ps1 -Config refcompare-snapshot`): another
   session rebuilding `build\RelWithDebInfo` cannot swap the DLL under a run.
3. Before every run the copy's `localappdata\Local\Risen\ConfigUser.xml` is pinned (7.2).
4. Each run: `run.ps1 [-NoRenderer | -Mode vulkan] -GameDir <copy> -Instance 3 -Mods rvkcompare -Scenario rvkcompare
   -WaitForResult -Param tag=<tag>,spots=...`; then `bin\spass\screenshots\<tag>_*.png` are moved to
   `out\refcompare\<stamp>\<dir>\`, the `RVKCMP` lines of `spass.log` become `meta.json` (a JSON list), and `spass.log`,
   `test-result.json`, `rvk.log` (Vulkan) and `run.json` (exit code, picture count, seconds, pinned config, hashes of
   spots.lua/main.lua/spass, renderer DLL hash + time, git HEAD, whether `src/` was dirty) are kept beside them.
5. `python tools\refcompare.py compare d3d9 vk -o report --tag-a d3d9 --tag-b vk` plus `--thresholds <ref>\thresholds.json`
   (a stored reference) or `--baseline d3d9_b` (second D3D9 run). The summary table and the report path are printed; the exit
   code is refcompare.py's (0 PASS/WARN, 1 FAIL, 2 INVALID), 3 when a game run produced no pictures.
6. `-Calibrate` stops after the two D3D9 runs with `refcompare.py calibrate d3d9 d3d9_b -o thresholds.json`: per spot
   thresholds (noise floor x 1.5, never stricter than the defaults) and a known-noise mask `thresholds_masks\<spot>_known.png`.
   The stamp directory is then a reference set for `-ReferenceDir` (`reference.json` stores the hashes of spots.lua, main.lua,
   the spass DLL and the pinned options; a mismatch is warned about: re-calibrate after changing them).

Runs are sequential (one game at a time in the copy). Wall time measured: a D3D9 run of the 12 spots ~6-7 min (the game's
D3D9 runs at ~30 fps in the background), a Vulkan run ~5 min.

### 7.2 Pinned configuration

A fixed `ConfigUser.xml` graphics block written before each run: `ViewDistance`, `Shadows Quality`, `PostProcessing
DefaultEffect` (two report variants: `DoF` as shipped and `None` to separate post-processing differences from the scene),
`TextureFilterMode`, `Vegetation/Effect Quality`, `Cache Preset`, `Window 1600x900`, `Vsync`, `FOV="60"`,
`EnableTutorialSystem="false"`, `UserPageMode` fixed. `test-run.ps1` already rewrites window size, audio and languages; the
graphics block can be a new `-ConfigUserXml <file>` switch there or done by `refcompare.ps1` before calling `run.ps1`.

As built: `refcompare.ps1` (`Set-PinnedConfig`) writes before every run `Engine/Render ViewDistance=3`, `Shadows
Quality=<-Quality, High>`, `PostProcessing DefaultEffect=<-PostEffect, DoF>`, `Graphics TextureFilterMode=4`,
`Vegetation/Effect Quality=<-Quality>`, `Cache Preset=High`, `Window Left=0 Top=0 Right/Bottom=<-WindowSize, 1600x900>
FOV=60 Vsync=true AdapterNumber=0`, `Game EnableTutorialSystem=false UserPageMode=7 ShowFocusHighlights=true
ShowSubtitles=true` into the copy's `localappdata\Local\Risen\ConfigUser.xml` (never a path under `data\` or the real
install). The pinned values are part of the reference identity (`reference.json`, `run.json`).

---------------------------------------------------------------------------------------------------------------------

## 8. Determinism problems and mitigations

| # | Source of difference | Mitigation |
|---|---|---|
| 1 | new-game intro camera fly-in (~25 s), "Press START" page 26 | wait for `session.start` + intro (as `rvk_shadows`), `risen.gui.close(26)`; spots start after it |
| 2 | weather: random state transitions of `eCWeatherEnvironment` in zone mode 0 | force mode 2 with explicit weights per spot (3.4); interiors get their zone state from calibration (5.5) |
| 3 | thunder/lightning flashes (`ThunderProbability`, `eff_env_thunder`) | `set_weather(..., { thunder = false })` zeroes thunder and random effects of every state (done for every spot); burst + luma-outlier drop (6.2) as a safety net |
| 4 | the clock runs (sky, sun direction, shadow direction) | `set_time` right before freezing; time freeze |
| 5 | animation, wind (vegetation, SpeedTree), water waves, clouds, particles, light flicker | time freeze (app state 8: stable bursts); `set_game_time` before the freeze pins sea surface and clouds; `SetVegWindParams 0 0 0 0`; player hidden; the rest by the known-noise mask of two reference runs (8.1) |
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

### 8.1 The animation phase under the freeze: found and partly pinned (2026-10-21)

Under the time freeze (app state 8) a burst is stable (three captures 10 frames apart: 0.0 % unstable pixels at 11 of 12
spots), but two runs freeze the world at different phases of the sea, the clouds, the player's idle pose and foliage.
Measured between two D3D9 runs of the spots (same pose, weather, clock to the minute): beach_sun mean luma -5.9, SSIM 0.85,
12.8 % of the pixels off by more than 32; hero_closeup SSIM 0.84; jungle 0.89; the GUI pages and the pub 0.997-1.0.

Where the engine's animation time comes from [C, Engine.dll; dumps `B:\DevDrive\risen-ghidra\rvk_cmp_time_dump*.txt`;
risen-spass RE.md section 60 has the details]:
* `eCTimer` (static at RVA `0xc6fcf0`): `+0x08` real ms stamp, `+0x0c` its float copy, `+0x10` game time (sum of scaled frame
  times, `GetGameTime`), `+0x28/+0x2c` frame time. `eCTimer::Update` (`0x4cf0e0`) does not advance anything under the freeze.
  The getters are inlined everywhere in Engine.dll (found with a capstone scan of the `GetInstance` call sites).
* `eCShaderMaterialAdmin::Process` (`0x6bb250`) feeds every timed shader element (texture scrollers, oscillators, rotators,
  animated colours and samplers) with `GameTime / 1000`; the scroller's offset is `frac(t * speed)`: **a pure function of the
  game time**. This drives the sea surface and the clouds.
* Accumulators of the frame time / game-time deltas (not pinnable by a timer value): SpeedTree wind
  (`eCSpeedTreeAdmin::UpdateWindMatrices`), the sky's env-cubemap cadence, skeletal animation, particles, the shore foam /
  waves, the weather.
* Experiments: pinning `+0x10`/`+0x0c` only *during* the freeze changes nothing (the constants were written before it);
  setting the game time two frames *before* the freeze (an external `WriteProcessMemory`, then the new spass binding) puts
  clouds and sea surface at the same phase: D3D9 vs Vulkan at the beach without any mask SSIM 0.986 and 0.66 % of the pixels
  > 32 (two unpinned D3D9 runs: 0.85 / 12.8 %).

**spass capability added** (risen-spass, generic): `risen.debug.set_game_time(ms)` writes `eCTimer +0x10` after checking that
`GetGameTime` reads that field (RE.md section 60, scenario `scene` checks it). `mods/rvkcompare` sets `20000000 + 1000000 *
<position of the spot in spots.lua>` two frames before every freeze (`pin_time=1`, default), the value is logged as
`pinned_game_ms`.

What stays nondeterministic and how the comparison handles it:
* **Removed**: the player is hidden in the world spots (no idle pose, no shadow), NPCs in a radius are hidden, vegetation
  wind is off (`SetVegWindParams 0 0 0 0`), thunder and random weather effects are off, the clock is held every tick, the
  weather is forced.
* **Masked**: `refcompare.py calibrate d3d9 d3d9_b` (or `compare --baseline`) turns the pixels where two D3D9 runs differ
  (> 8, max channel, burst medians, dilated 4 px) into a per-spot **known-noise mask** (`thresholds_masks\<spot>_known.png`,
  hatched in the heat maps), excluded from the D3D9-vs-Vulkan metrics like the burst mask; the per-spot thresholds come from
  the floor A vs A' outside the mask (x 1.5, never stricter than the defaults). A spot is INVALID when the burst noise exceeds
  30 % or the known mask 75 % (`tests/refcompare/thresholds.toml` overrides per spot). The cost: a renderer difference inside
  a masked area is not measured (the side-by-side and blink GIF still show it).
* The volcano dungeon's full-screen heat distortion keeps moving under the freeze in both renderers (~65 % of the pixels
  shimmer 1-2 px within a burst; time source not found: it is not the eCTimer fields above, which stand): `lava_cave` allows
  80 % burst noise and compares the rest.

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

---------------------------------------------------------------------------------------------------------------------

## 11. Results (2026-10-21, first end-to-end run)

Reference `out\refcompare\20261021-190139` (`-Calibrate`: two game-D3D9 runs, 221 s each), Vulkan run
`out\refcompare\20261021-190928` (`-ReferenceDir`, 193 s; renderer snapshot of `build\RelWithDebInfo\d3d9.dll` from
19:08 with another session's uncommitted perf work in `src/`, HEAD 4fa8a57), 1600x900, DoF, High. Both renderers ran at
~80 fps. Report: `out\refcompare\20261021-190928\report\report.html` (git-ignored, game-derived).

| spot | verdict | ssim | psnr_q | bad32 % | dluma | masked % (burst+known) | without the known mask |
|---|---|---|---|---|---|---|---|
| gui_inventory | WARN | 0.9860 | 35.1 | 0.77 | +0.65 | 0.1+19.4 | FAIL (ssim 0.968, bad32 2.4) |
| beach_sun | WARN | 0.9929 | 35.0 | 0.42 | +0.35 | 0.0+26.2 | FAIL (0.979, 2.1) |
| jungle | PASS | 0.9868 | 45.4 | 0.01 | +0.24 | 0.0+33.1 | FAIL (0.914, 0.56) |
| beach_rain | PASS | 0.9881 | 44.1 | 0.00 | -0.14 | 0.0+19.7 | WARN |
| hero_closeup | PASS | 0.9954 | 40.9 | 0.01 | -0.24 | 0.0+19.0 | FAIL (0.981, 1.8) |
| harbour_night | PASS | 0.9959 | 49.8 | 0.00 | -0.03 | 0.0+5.4 | PASS |
| monastery_hall | PASS | 0.9963 | 44.9 | 0.04 | +0.04 | 0.0+11.5 | PASS |
| pub_interior | PASS | 0.9972 | 46.0 | 0.00 | -0.16 | 0.0+5.8 | PASS |
| crystal_cave | PASS | 0.9976 | 47.9 | 0.07 | +0.05 | 0.0+3.0 | WARN |
| harbour_docks | PASS | 0.9978 | 51.5 | 0.00 | +0.37 | 0.0+1.7 | PASS |
| lava_cave | PASS | 0.9985 | 65.5 | 0.00 | +0.03 | 65.8+70.0 | PASS (burst mask only) |
| gui_character | PASS | 1.0000 | 63.7 | 0.00 | +0.00 | 0.1+0.3 | PASS |
| menu_esc | PASS | 1.0000 | 100.0 | 0.00 | +0.00 | 0.1+0.3 | PASS |

What differs, and why it is not counted as a renderer bug:
* `gui_inventory`: a wave-spray particle burst at the rocks on the horizon is present in the Vulkan picture and absent in
  both D3D9 pictures (worst tiles x 640-900, y 384: `gui_inventory_tiles.png`); particles step with the frame time and are
  not pinned. Remaining sky offsets (+6 luma in the band above the horizon) lie inside the D3D9 run-to-run spread (below).
* `beach_sun`: sea near the horizon (shore waves/foam, frame-time driven) and the sky haze: the mean sky luma is A 176.0,
  A' 168.9, Vulkan 171.2: Vulkan lies between the two D3D9 runs.
* Without the known mask the FAILs are foliage (`jungle`: SpeedTree wind accumulates even with `SetVegWindParams 0`), the
  hero's idle pose, sea foam, rain streaks and candle flames: the same areas differ between the two D3D9 runs.
* GUI: `gui_character` and `menu_esc` are pixel-identical apart from 0.3 % (text and panels exact); the inventory page itself
  is identical, only the world behind it differs.

**Brightness question** (an earlier uncontrolled comparison suggested +6..+13 luma in jungle views after the shadow
change): not reproduced. With pinned weather, clock and game time the mean luma differences Vulkan - D3D9 (`dluma`) are
-0.24..+0.65 on the compared pixels and -1.16..+0.72 without the known mask, over all spots (jungle +0.24 masked, +0.34 unmasked; mean frame luma A 42.22, A' 42.37, Vulkan 42.56), within or next to the
spread between two D3D9 runs (up to 4.7 at beach_sun). The earlier difference came from the uncontrolled weather.

No renderer bug was found at these 13 spots at the default thresholds (SSIM >= 0.98, bad32 <= 0.2 %, |dluma| <= 1, psnr_q
>= 38 dB). Limits: the masked areas (sea, foliage, flames, particles: 0.3-33 %, the volcano 70 %) are not judged; the
spots do not cover shadows of point lights up close, decals, combat effects or the title screen (candidates for more spots).
