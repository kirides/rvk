# The ~80 fps limit in the world

Research of 2026-10-22/23: static RE of Risen.exe, Engine.dll, Game.dll, SharedBase.dll and the logs of earlier runs
(sections 2-8), then measurements in the test copy on 2026-10-23 (sections 9-11). Question (user): what causes the
steady ~80 fps in the world while the menu is not limited? Is it hard-coded frame pacing that guards the physics ("flying
to the moon" after grabbing a ledge)?

Legend: [C] read in the binaries / logs, [I] inferred. RVAs are relative to the DLL's image base (0x180000000).

## 1. Answer (measured 2026-10-23)

* **The cap is the AMD driver's Radeon Chill limiter, fed by an Adrenalin per-application profile for `Risen.exe` with
  Chill on and min = max = 80 fps** [C, section 9]. The present thread spends 80 % of each 12.4 ms present in
  `amdvlk64.dll`'s frame limiter (`Sleep(1)` loop on QPC, then a spin), and the driver's own Chill globals in the running
  process read `enabled = 1, MinFramerate = 80, MaxFramerate = 80`. The same executable renamed (`RisenTest.exe`, same
  directory, same renderer) runs the world at **175-240 fps**; the driver then loads min = max = 300 for it.
* **Why only the world**: Chill skips frames it takes for a loading screen: fewer than `LoadingScreenDrawsThresh` draws per
  frame (150 in this profile) for about 2 s. Menus (18 draws) and loading screens stay uncapped, the world (400-1500
  draws) is capped. Input does not matter at min = max.
* **No limiter in the game's code** (sections 3-5, static RE). Not the Steam overlay, OBS or other Vulkan layers: with
  every implicit layer disabled the cap and the driver stack are unchanged (section 9.4).
* **Not ours to fix in the renderer**: the limiter sits inside `vkQueuePresentKHR` of AMD's driver and keys on the
  executable's file name; there is no Vulkan-side opt-out. The user has to switch Chill off in the profile that actually
  applies (section 11). For tests, `run.ps1 -Exe RisenTest.exe` escapes it.
* **High frame rates** (section 10): walking, running jumps on flat ground and falls behave the same at 80, 120 and
  185-197 fps, but a running jump started uphill is cancelled after ~0.02 s at 185-197 fps (reproduced twice) and works
  at 80 and 120 fps. Ledge climbing was not triggered by the scenario (untested).

## 2. Evidence from logs [C]

| run | state | fps | present (present thread) | render thread waited |
|---|---|---|---|---|
| refcompare 20261022-231618 vk | world | 79.9 | 12.27-12.41 ms | 6.9-10 ms |
| water 20261022-141017 enh | world | 79.8-79.9 | 12.3-12.4 ms | 9-10 ms |
| same run, 14:14:10 / 14:14:15 | teleport loading screen (61 / 734 draws) | 120.1 / 118.4 | 0.12 ms | ~0 |
| same run, 14:14:20 | world again | 78.2 | 11.96 ms | 8 ms |
| performance.md 2 (10-21) | world, `max_fps` 60 | 60.0 | 0.11 ms | - |
| performance.md 2 (10-21) | D3D9 renderer, vsync off | 79.9 | (not measured) | - |

`Presenter::run` (`src/vk/presenter.cpp`) times `Swapchain::present` → `Context::queuePresent`. That is the queue lock
plus `vkQueuePresentKHR`. The render thread holds the lock only for `vkQueueSubmit2`. Since the render thread waits
7-10 ms per frame for the present thread, the time is inside `vkQueuePresentKHR`, not in lock contention [I, strong].
Presents below ~80 Hz never block, so the limiter behaves like a rate limiter (a fixed cadence), not like a GPU stall.

## 3. Main loop and eCTimer (Engine.dll) [C]

```
eCApplication::Run 0x4c0490: loop { eCInputAdmin::UpdateX360; vtbl+0x110 (OnRun) } (no wait)
eCApplication::OnRun 0x4c0570:
  Process 0x4c0820     eCTimer+0x50++, eCProcessor::Process (modules: session, physics, AI, ...), KillEntities
  DoRender(true) 0x4c0d40
    ... camera update, ClearViewport, OnPreRender (+0x5b8), eCCameraBase::Render / image effects
    OnPostRender (+0x5b0) -> eCApplication::Draw 0x4c13b0 -> GUI, SwapScreen -> IDirect3DDevice9::Present
    eCTimer::Update(flags of app+0x134)          <- after Present: the only engine sleep is here
  eCProfilerAdmin::NextFrame
```

`eCDX9Device::SwapScreen` (0x7407b0) is a plain `Present` (vtbl +0x88) + `GetBackBuffer`; it has no query or event wait.

`eCTimer` (instance via `GetInstance` 0x4ceef0). Fields, from `Invalidate` (0x4cee20), `ReadConfig` (0x4cefa0) and the
setters:

| field | meaning | default | source |
|---|---|---|---|
| +0x08 | last `bCTimer::GetTimeStamp` (ms) | | |
| +0x10 | game time (ms, scaled) | | |
| +0x24 | instantaneous fps = 1000 / raw dt | | `GetFPS` 0x4ced80 |
| +0x28 / +0x2c | frame time ms / s (smoothed, clamped) | | `GetFrameTime`, `GetFrameTimeInSeconds` |
| +0x34 | time scale | 1.0 | `SetTimeScaling` 0x4cecc0 |
| +0x38 | AverageFPS | 30 | `SetAverageFPS`; never used by `Update` |
| +0x3c | single-step frame time | -1 | `SetFixedFrameTimeSingleStep` 0x4cec90 |
| +0x40 | fixed frame time ms = 1000 / FixedFPS | -1; config FixedFPS=-1 -> -1000 | `SetFixedFPS` 0x4cec60, `SetFixedFrameTime` 0x4cec80 |
| +0x44 | min frame time = 1000 / MaxFPS | config 120 -> 8.33 ms | `SetMaxFPS` 0x4cec00; read **only** by `GetMaxFPS` 0x4cebd0 |
| +0x48 | max frame time = **MinFPS x 10** (sic) | config 10 -> 100 ms | `SetMinFPS` 0x4cec30, `GetMaxFrameTime` 0x4ced70 |
| +0x4c | relaxed (smoothed) frame time | | `RelaxTick` 0x4ced90 |
| +0x59 | smoothing on | 1 | `SetTimerIsSmooth` 0x4cebc0 |

Mapping config key to field [I, strong]: `ReadConfig` reads four `eSTimerOptions` keys whose conversions match the setters
exactly (x10 / 1000/x / raw / 1000/x).

`eCTimer::Update` (0x4cf0e0):

1. dt = (now - last) x scale. A dt ≤ 1e-5 becomes 1e-5. A dt > 1000 ms keeps the previous relaxed value.
2. +0x24 = 1000/dt, then `RelaxTick` smoothing.
3. If +0x40 > 0 (FixedFPS): `Sleep((int)(fixed - frametime))` and frame time = fixed, i.e. a fixed game step per frame
   (game time runs at frames x fixed, not real time). Otherwise, if 0 < +0x48 < frame time, the frame time is clamped
   to +0x48 (100 ms).

Consequences:

* `MaxFPS` is parsed but **never limits anything**. This confirms risen-spass's "not honoured" (DEVELOPMENT.md "Startup time";
  its "~80 ticks/s always" measurements of 2026-10-15 were taken while the external cap was active).
* `MinFPS` has a latent bug: the max frame time is `MinFPS*10` ms, which equals 1000/MinFPS only at the default 10. spass's
  `MinFPS="2"` experiment set it to 20 ms.
* There is no sleep before the present at all. A sleep in `Update` would show in our `-Perf` numbers as engine+HAL time
  between presents, not inside `vkQueuePresentKHR`.

Config (test copy `data/ini/ConfigDefault.xml` / `ConfigDefaultNew.xml`, read only): `<Timer AverageFPS="30" FixedFPS="-1"
MaxFPS="120" MinFPS="10"/>`. `ConfigGenomeDefault.xml` (editor): `MaxFPS="1024"`. Console `timer.fTimeScale`, `PhysicFPS`
("steps the physicscene at [n] fps", keyboard_and_console.txt).

## 4. Physics: NovodeX / PhysX 2.x, statically linked [C]

`Engine.dll` exports `NxCreateCoreSDK` (0x2deba0) and uses `NxActor`, `NxScene`, `NXU::NxuPhysicsCollection`; there are
no PhysX DLLs in `bin`.

* `eCPhysicsScene::Process` (0x86970) → `Simulate(false)` (0x5ea200), once per frame from the module processing.
  * If `+0x179 == 1` (the default; `eCPhysicsScene::Invalidate` 0x5e45a0 writes +0x178 = 0x100): `NxScene::simulate(timer
    frame time in s)` (vtbl +0x468), `flushStream` (+0x288), `FetchResults`.
  * Otherwise an engine-side accumulator: +0x120 += dt; while acc ≥ step(+0x124) { simulate; FetchResults }. There is no
    sub-step cap in this loop; `SaveStep` runs only on the last iteration.
* `SetSimulationFPS` (0x5e7590): step +0x124 = 1/SimulationFPS, then `NxScene::setTiming(step, maxIter = ceil(maxFrameTime
  /1000 / step), method kept)` (vtbl +0x290). The default step is 1/60 (`Invalidate`: 0x3c888889). The world's value comes
  from the world's `eCPhysicsScene_PS` (`SimulationFPS` property, +0x4c, applied in `OnPostRead` 0x65f750); the actual
  value of the shipped world was not read [open]. With MinFPS 10 that gives maxIter = 6 at 60 Hz. PhysX's fixed method
  sub-steps internally and does not interpolate.
* `FetchResults` (0x5e36c0) / `FetchResultsOnDemand` (0x5e5a80): `while (!NxScene::fetchResults(NX_RIGID_BODY_FINISHED,
  false)) Sleep(0);`. This is a spin while PhysX's own thread finishes the step. It costs CPU and **never limits the frame
  rate**.

Nothing in the physics path looks at wall-clock time beyond the frame's dt.

## 5. Every wait or timer API in the game binaries [C]

Import scan of all binaries in `bin` (`Sleep`, `SleepEx`, `timeBeginPeriod`, `Nt/ZwSetTimerResolution`, `NtDelayExecution`,
waitable timers, `DwmFlush`, `DwmSetPresentParameters`, `SwitchToThread`, `timeSetEvent`):

| binary | imports | callers |
|---|---|---|
| Risen.exe | none | |
| Engine.dll | `Sleep` | `eCTimer::Update` (FixedFPS), physics `FetchResults*` (Sleep(0) spin), Bink video 0x70fb20 / 0x70fd00 |
| Game.dll | `Sleep` | 0x970e60 (Discord rich-presence thread: `RunCallbacks` + Sleep(1) loop), 0x9710e0 (Steam rich presence: `SteamAPI_RunCallbacks` + Sleep(10)), both started from `gCSession::PostInitialize`: own threads, not the main loop |
| SharedBase.dll | `Sleep` | `bCThreadEx::SleepThread` / `WaitIfThreadFinished` |
| FileSystem2, fmod, binkw64 | `Sleep` | I/O, audio, video threads |
| discord_game_sdk.dll | `SwitchToThread` | |
| d3d9.dll (ours) | waitable timer, `timeBeginPeriod` | our pacer |

Other Engine waits: `WaitForSingleObject` in 0x2f6dc0 / 0x346670 and `WaitForMultipleObjects` in 0x349eb0. These are thread
and job helpers, not reached from the frame path as far as checked [I]. QPC users are profilers, PhysX and Bink.
`gCSession::Process` (0x9711e0) has no wait. Constants 80.0 / 12.5 / 0.0125 / 1/80 appear only as distances, speed
factors or shader values (e.g. `gCCharacterMovement_PS::DetectClimbMode` 80 cm), never as a frame time.

No game binary raises the timer resolution, so any game `Sleep(n)` would be quantised to ~15.6 ms unless another
component raises it. A 12.5 ms cadence cannot come from such sleeps.

## 6. What high fps does to the game (the "fly to the moon" question)

* Community knowledge: the 2009 version has high-fps bugs ("character disappears when trying to climb on ledges, or stops
  in mid-air when falling from heights", "too much thunder"). KonradAr's "Risen FPS and aspect ratio fix" ships an fps
  limiter for them. The January 2023 update (64-bit, the base of Risen Enhanced) lists "a few gameplay & technical
  (related to audio and physics) bugfixes". One PCGH forum user reports 144 fps with vsync on the 2023 version without the
  physics bugs: anecdotal, but it suggests the official build is not capped at 80 [I].
* In Game.dll the character controller (`gCCharacterMovement_PS`) integrates with the variable `eCTimer` frame time (~40
  call sites: `ProcessPhysicsMovements` 0x8e9e40, `ProcessFreeNavigationJump` 0x8ee430, `PutToGround`,
  `ProcessControledTranslation`, camera, combat). It is not on the PhysX fixed step.
* `ProcessClimbStandard` (0x8e8dc0) **clamps its dt to ≥ 0.0084 s** (`DAT_180f63174`, ≈ 119 fps) [C]. This looks like a
  deliberate guard for high frame rates in the climb code [I]: above ~119 fps climbing runs slightly faster than real
  time, but its per-frame step no longer shrinks.
* `CheckCharacterGotStuck` (0x8e4dc0) divides a displacement by the frame time to get a velocity. Small dt values make large
  velocities, the classic launch-into-the-sky pattern [I].
* So high frame rates are a gameplay risk at the controller level, not a PhysX one. **The engine does not defend against
  them with a cap**: `MaxFPS` is dead, and FixedFPS changes the game clock (fixed step per frame) instead of pacing in
  real time.

## 7. Measurement plan (carried out 2026-10-23: sections 9-10)

The goal is to name the component that blocks inside the present, and the state it keys on.

1. **Name the module that blocks** (decisive). Add a debug watchdog to the present thread: when `vkQueuePresentKHR` exceeds
   ~8 ms, a helper thread `SuspendThread` + `GetThreadContext` + `RtlVirtualUnwind` walks the present thread's stack and
   logs the module+offset chain once per 5 s. A `Sleep` / `NtDelayExecution` / `NtWaitForSingleObject` reached through
   `amdvlk64.dll` points at the driver (Chill / FRTC / another profile feature); through `gameoverlayrenderer64.dll` at
   Steam; through `graphics-hook64.dll` at OBS. The alternative without code: `wpr -start GeneralProfile -start CPU`,
   reach the world, `wpr -stop fps.etl`, then in WPA, CPU Usage (Precise) for the "rvk present" thread, the ready-thread
   and wait stacks. PresentMon (`--process_name Risen.exe`) also shows `MsInPresentAPI` per renderer.
2. **Exe-name test for driver profiles.** Copy `bin\Risen.exe` to `bin\RisenRvk.exe` in the test copy (never the real
   install) and start that. AMD (and Steam / overlay) per-game settings match the executable. Uncapped world = a
   per-game profile. Needs a `-Exe` switch in `run.ps1` / spass `test-run.ps1`.
3. **Strip the hooks** one at a time: Steam overlay off for Risen (Steam UI) or start without Steam; OBS closed +
   `DISABLE_VULKAN_OBS_CAPTURE=1`; Discord overlay off; RTSS / Afterburner closed. For Vulkan mode, a `[vulkan]` debug
   option could call `SetEnvironmentVariable("VK_LOADER_LAYERS_DISABLE", "~implicit~")` before `vkCreateInstance`
   (environment variables from `run.ps1` do not reach the game). Extend rvk.log's module list to `DiscordHook64.dll`,
   `RTSSHooks64.dll`, `amdvlk64.dll`, `amdxx64.dll`, `GameOverlayRenderer64.dll`.
4. **Which state toggles it.** Log fps and present time together with app state (`eCApplication` +0x134 flags, the
   loading flag, `gCSession` state, `eCApplication::ConfineCursor` / mouse captured, window focus, DirectInput
   acquired). Scenes: world idle, world with posted input (`-Commands 'key w'`, overlay `key` commands), ESC menu in the
   world, inventory, character screen, teleport loading (known uncapped), main menu. Also run one in the foreground
   (`-AllowFocus`) with real input. Background test runs never get input, and Windows treats unfocused windows
   differently (EcoQoS, "Optimizations for windowed games").
5. **Engine vs present split** (to confirm, cheap): `-Perf` already separates engine+HAL CPU from present. Add the
   `eCTimer` frame time (+0x28) and instant fps (+0x24), plus the time in `eCApplication::Process` 0x4c0820 vs `DoRender`
   0x4c0d40 (both large functions, safe to detour; or hook-free: QPC stamps at our existing phase markers). Split
   `Context::queuePresent` into lock wait and `vkQueuePresentKHR`. For the D3D9 renderer: `-Mode trace` times the HAL's
   `SwapScreen` slot = the time in `IDirect3DDevice9::Present`.
6. **Driver settings**: ask the user to check the Risen per-game profile and the global profile once more for every
   pacing feature: Chill (min/max), Frame Rate Target Control, HYPR-RX / HYPR-RX Eco, Anti-Lag, Fluid Motion Frames,
   Enhanced Sync, and "Reset" the per-game profile. Note the driver version.
7. Re-measure with `-Vsync off -MaxFps 0` (uncapped) and `-Vsync on` (120) after each change. Use `rvk_shadows` (`hold=25`)
   for both renderers (`-NoRenderer` for D3D9) so the numbers are comparable to performance.md.

## 8. If a cap is wanted for gameplay (options)

* The engine has no usable cap: `MaxFPS` is dead, and `FixedFPS` makes the game clock run per frame. Our `vk::FramePacer`
  (`[vulkan] max_fps`, QPC + high-resolution waitable timer) already caps accurately in Vulkan mode.
* Options: (a) a player setting "world frame cap" (`vulkan.max_fps`, or a world-only variant keyed on the session state,
  so menus stay uncapped), default off or ~120 to match the climb clamp; (b) leave uncapped by default, since the 2023 code
  clamps climbing at ≈119 fps, and test the known bug spots (ledge grab, long falls, storms) at 120 / 144 / 240 fps before
  choosing a default; (c) decoupling the simulation from rendering (fixed-rate `Process` + interpolated rendering) is not
  practical: game logic and rendering share one thread in `OnRun`, and entity poses would need interpolation across the
  whole scene.
* Removing an external cap (driver / overlay) breaks nothing in the engine. Gameplay above ~119 fps is the open risk to
  test (6.).
* After section 10: a cap at the monitor's rate (vsync on, 120 Hz here) or `vulkan.max_fps` 120 avoids the one glitch
  found (the cancelled uphill jump at ~190 fps). Not built as a separate setting: `vulkan.max_fps` (player setting,
  overlay) already applies in the world and the menus alike.

## 9. Measurements of 2026-10-23 [C]

Machine as in performance.md (RX 7900 XTX, Adrenalin with `amdvlk64.dll` 9.2.10.406 from
`DriverStore\FileRepository\amdvlk.inf_amd64_4bc8828c71eddf06`, 120 Hz display). Scene `rvk_shadows` (beach, ~1440
draws), Vulkan mode, vsync off, window in the background.

### 9.1 Where the present waits: the stall sampler

New debug tool `[debug] present_stalls = <ms>` (`run.ps1 -PresentStalls 4`, `src/core/stall_sampler.*`): while a present
lasts longer than the threshold, a helper thread suspends the presenting thread every 0.5 ms, walks its stack
(`RtlVirtualUnwind`), resumes it, and every 5 s logs the call rate, the durations and the top stacks by module + nearest
export (`diag:` lines). In Vulkan mode it watches the present thread's `vkQueuePresentKHR`, otherwise the engine's
`eCDX9Device::SwapScreen` (D3D9 `Present`, a detour installed only with the option). Result in the world:

```
diag: present thread vkQueuePresentKHR: 80.0 calls/s, avg 12.40 ms, max 12.71 ms; 400 calls >= threshold; 3187 samples
diag: ... time by first non-system module: amdvlk64 100 %
diag: ... stack 1 (80 %): ntdll!ZwDelayExecution < kernelbase!SleepEx < amdvlk64+0x271138 < amdvlk64+0x19fade
      < amdvlk64+0x1a0dde < steamoverlayvulkanlayer64!vkQueuePresentKHR+0x4a2 < graphics-hook64 < d3d9 (ours)
stacks 2-4 (the rest): amdvlk64+0x2737xx < amdvlk64+0x271146 (the spin after the sleep)
```

### 9.2 The limiter in amdvlk64.dll (RVAs of 9.2.10.406)

`amdvlk64+0x270e40..0x271200` (disassembly: `B:\DevDrive\risen-ghidra\fpslimit\amdvlk_limiter.txt`), called from the
driver's present path:

* Loading-screen detection: the frame's draw count against `LoadingScreenDrawsThresh` (`0x66e5160`); a frame below it
  resets the timer; at or above it for more than 2 x the QPC frequency (~2 s) a flag (`0x688e28a`) arms the limiter.
* Limiter active (`0x68931e0`, set at `0x271882` from the profile's enable `0x688c07c` and the hotkey toggle
  `0x66e5168`): target = QPC at the last present + QPC frequency / fps, fps from `MinFramerate` (`0x66e51a8`) adjusted by
  a step (`0x66e51c4`) between the min and max framerate; then `timeBeginPeriod(1)`, a loop of `Sleep(1)` while more than
  one tick away, a QPC spin (`0x2735c0`) for the rest, `timeEndPeriod`. A held right Shift (`GetAsyncKeyState(0xA1)`)
  breaks the wait when flag `0x1000000` of the level word is set (not in this profile). Input activity comes from a
  `GetAsyncKeyState` sweep over all keys (`0x274dc0`): Chill's "input raises the frame rate".
* Settings, read by name at `0x274ff5..` with their defaults: `ProfileEnableDefault` (`0x688c07c`), `ChillLevelDefault`
  (`0x66e5164`, 2), `MinFramerateDefault` (`0x66e51a8`, 70), `MaxFramerateDefault` (`0x66e5170`, 144),
  `MinFramerateRange` (30), `MaxFramerateRange` (300), `LoadingScreenDrawsThreshDefault_VUL` (`0x66e5160`, 200). The
  defaults 70 / 144 are Radeon Chill's.

### 9.3 The values in the running process (external `ReadProcessMemory`, `scripts\peek_chill.py` in the dump folder)

| executable (test copy `bin`) | enable | level | min / max fps | loading draws | world fps |
|---|---|---|---|---|---|
| `Risen.exe` | 1 | 2 | **80 / 80** | 150 | 79.9 (present 12.4 ms, 80 % in the driver's `Sleep`) |
| `RisenTest.exe` (byte copy) | 1 | 2 | 300 / 300 | 150 | **175-240** (present 0.12 ms, CPU-bound: engine+HAL 4-5.5 ms) |

At world entry the limiter switched on (`active` 0 -> 1, loading flag 0 -> 1 about 2 s after the first world frames);
the menu before ran unlimited. Registry (`Class\{4d36e968-...}\0000`): `KMD_ChillEnabled = 0`, `Gmx_FRTCEnabled = 0`,
`KMD_FRTCInitialStatus = 1`, `KMD_MaxFrameRateRequested = 60`: the global switches are off, the per-application profile
overrides them.

### 9.4 The profiles

* Adrenalin's game list `%LOCALAPPDATA%\AMD\CN\gmdb.blb` has one Risen entry: `exe_path`
  `B:\DevDrive\risen-test-vk\bin\Risen.exe` (`"manual": "TRUE"`, `"profileCustomized": "TRUE"`, `steam_id` 40300,
  `amdId` 537); the real install's exe is not listed separately.
* The driver-side user profiles `%LOCALAPPDATA%\ATI\ACE\APL\User.blb` (binary; string offsets = index x 8 + 0x60) hold
  five 3D profiles with these bindings: `steamappid:40300`; `Risen.exe` + `B:\DevDrive\risen-test-vk\bin*`; `Risen.exe` +
  `B:\DevDrive\risen-test-vk-cmp\bin*`; `rvk_vktest.exe` + `...\build\RelWithDebInfo*`; `Risen.exe` +
  `B:\DevDrive\risen-test\bin*`. Their Chill keys (`Chil_PFEnable`, `Chil_MinFRate`, `Chil_MaxFRate`, `Chil_Level`,
  `Chil_LdSDThsh`): three profiles Chill **on** 300 / 300, one Chill **off** 80 / 80, one Chill **on 80 / 80**. Which
  binding owns which profile was not decoded [open]; the process values (9.3) show that the `Risen.exe` in
  `risen-test-vk` gets a Chill-on 80 / 80 profile.
* The driver's own application database (`atiapfxx.blb`) also lists `risen.exe`; the loading-screen threshold 150 (not the
  default 200, absent from User.blb) presumably comes from there [I].
* Bisection: `[debug] environment = ["VK_LOADER_LAYERS_DISABLE=~implicit~"]` (`run.ps1 -Environment ...`): no Steam / OBS
  layer in the process, still 79.9 fps with the same `amdvlk64` `Sleep` stack. The Steam overlay and OBS are not the cause.
* D3D9 (passthrough, vsync on, the copy's settings): 29 fps, 97 % of `Present` waiting in
  `dwmapi!DwmpDxGetWindowSharedSurface` (windowed D3D9 vsync through DWM), not the Chill sleep; the 79.9 fps of the D3D9
  renderer with vsync off (performance.md 2) is the same profile [I] (D3D9 user-mode driver `amdxn64`, not sampled with
  vsync off).

## 10. High frame rates in play [C]

Scenario `mods/rvk_highfps` (this repo): new game, god mode, invisible; the hero's track per tick through a 3 s walk, four
running jumps (Space = `MoveUp`, 1.75 s apart), a drop from 15 m and a jump at a ledge found by physics rays. Same poses
in every run:

| | 80 fps (Chill) | 120 fps (`-MaxFps 120`, `RisenTest.exe`) | 185-197 fps (`RisenTest.exe`, two runs) |
|---|---|---|---|
| world frame time p50 / p99 | 12.52 / 12.89 ms | 8.33 ms | 5.0-5.3 / 5.6-5.9 ms |
| walk speed | 398 units/s | 397 | 396-397 |
| jump 1 (flat): rise, air time, length | 173, 0.89 s, 807 | 175, 0.88 s, 815 | 175, 0.87-0.88 s, 814 |
| jump 2 (uphill): rise, air time | 143, 0.87 s | 142, 0.84 s | **111, 0.02 s** (both runs: the jump is cancelled) |
| jumps 3 / 4: rise | 204 / 144 | 204 / 146 | 205 / 145-147 |
| drop 15 m: landed after | 1.74 s | 1.74 s | 1.71 s |
| ledge spot (-35391, 659, -3470): rise | 141 | 94 | 82 |

* No "flying to the moon": no rise above a jump's height, no launch after a fall; the largest per-tick position steps (33-65
  units) are the controller's ground snaps and slides, of the same size at every rate (the per-tick vertical speed only
  looks larger at high rates because the tick is shorter).
* A frame-rate dependency exists: the second running jump, started on a rising slope, ends after 4 ticks at ~190 fps
  but flies normally at 80 and 120. Likely mechanism [I]: the jump's ground check right after take-off; with ~5 ms ticks
  the hero has not cleared its threshold above the rising ground yet.
* The ledge jump differs between all three rates (different slide paths); no climb state was reached at any rate, so
  ledge climbing (the `ProcessClimbStandard` clamp at 0.0084 s) stays untested. A known climbable ledge (`-Param
  ledge=x;y;z;yaw`) is needed.

## 11. What the user can do

1. AMD Software: Adrenalin Edition -> Gaming -> Games -> **Risen** (the tile points at `B:\DevDrive\risen-test-vk\bin\Risen.exe`)
   -> Radeon Chill **off**; check every other Risen tile / copy the same way, and Graphics -> Global Graphics. The profile
   that is in effect for `Risen.exe` has Chill on with min = max = 80, even though one of the five Risen profiles has it
   off. If toggling does not change it, "Reset game profile" for the tile (or, with Adrenalin closed, move
   `%LOCALAPPDATA%\ATI\ACE\APL\User.blb` aside: that resets all per-application profiles).
2. Check: `run.ps1 ... -PresentStalls 4` must no longer show `amdvlk64` `SleepEx` stacks, the world fps then exceeds 80
   (175-240 here with vsync off).
3. For gameplay, vsync on (120 Hz) or `vulkan.max_fps` 120 is the tested safe rate (section 10; 144 untested); at ~190
   fps a jump on a slope can fail.

## 12. VaanaCZ Risen-FPS-Patch (2009 32-bit) vs the x64 build

**Built** (commit 4817bab, `src/engine/high_fps.*`, setting `compat.high_fps` "High Fps compatibility (Vaana)", System tab,
default on). In game at ~190 fps all 4 sites are patched, the fall scaling is active (x6.1), walk / jumps / the 15 m drop are
unchanged; climbing and thunder are not exercised by the scenario.

The patch (github.com/VaanaCZ/Risen-FPS-Patch; a `Script_*.dll` for the original 2009 build only) fixes three
frame-rate dependencies. Checked against the x64 build in the decompilation (dumps
`B:\DevDrive\risen-ghidra\rvk_fpspatch_*.txt`); **[C]** = read in the code, **[I]** = inferred, not measured.

| # | patch | x64 build |
|---|---|---|
| 1 climbing | `ProcessClimbStandard` / `ProcessClimbHigh` divide the position correction by the frame time and call `AddToCurrentVelocity`; the patch sets Y through `SetCurrentPosition` | **Present** [C] in `gCCharacterMovement_PS::ProcessClimbStandard` 0x8e8dc0 and `ProcessClimbHigh` 0x8e8910: `EL_GetFrameTimeInSeconds` 0x4ac550 returns max(frame time, 0.0084) (`DAT_180f63174`), then `bCVector::GetInvScaled`, `AddToCurrentVelocity` 0x8ee170. Above ~119 fps the clamped dt exceeds the real one: under-travel rather than overshoot [I] |
| 2 airwalk | the falling landing check is \|dy\| < 5 cm per frame; the patch scales it by 30 fps / dt | **Present** [C] in `CheckChangedMovementStates` 0x8e53f0, movement mode 0xf (`IsFalling` 0x8f4ba0): \|worldPos.y - this+0x18c\| < 5.0 (`DAT_180f632c4`) or velocity.y > 0 accumulates `GetLastFrameTimeInSeconds` into this+0x50c; > 0.5 s (`DAT_180f631e8`) -> `PutToGround` + landed mode (also when the ground is within 1.0, `DAT_180f63238`). A fall slower than 5 / dt units/s (500 at 100 fps, 1200 at 240 fps) counts as stalled: a mid-air landing after 0.5 s [I] |
| 3 thunder | one `rand` per frame against the probability; the patch scales it by 30 fps / dt | **Present** [C] in `eCWeatherAdmin::Process` (Engine.dll 0x710730): one `bCMersenneTwister::Rand` per call compared with `eCWeatherStates` ThunderProbability (+0x54) and the random-effect sound probability (+0x70), no dt factor; only the flash duration (`DAT_180c6eaec`) is decremented by the frame time. Consequence: storms thunder more at high fps |

Options (not built): detours scaling these by dt like the patch, or the existing `vulkan.max_fps` cap (~120, section 10).

## 13. Files

Dumps (outside the repo): `B:\DevDrive\risen-ghidra\fpslimit\` (`engine_imports.txt`, `engine_phys*.txt`,
`game_imports.txt`, `game_session.txt`, `game_move.txt`, `game_maxspeed.txt`, `sb_imports.txt`; script
`ImportRefs.java` = import callers + float constants), earlier `B:\DevDrive\risen-ghidra\testspeed\{timer,app,onrun}.txt`.
2026-10-23: `amdvlk_limiter.txt` (the limiter's disassembly), `peek1.txt` / `peek_renamed.txt` (driver values during the
runs), `user_blb.txt` (User.blb strings in file order); scripts in `fpslimit\scripts\`: `dis.py` (capstone disassembly of
a DLL range with import names), `xref.py` (RIP-relative references to a global), `settings.py` (string operands),
`peek_chill.py` (reads the Chill globals of the test copy's running Risen), `blbgrep.py`, `blbdump.py`, `blbprof.py`
(User.blb profiles), `gmdb.py`.

Sources: [Risen FPS and aspect ratio fix (GamePressure)](https://www.gamepressure.com/download/risen-risen-fps-and-aspect-ratio-fix-v10b-mod/ze138aa),
[ModDB news](https://www.moddb.com/news/risen-fps-and-aspect-ratio-fix-10-released),
[PCGH thread on the January 2023 update](https://extreme.pcgameshardware.de/threads/risen-pc-bekam-ein-update-im-januar-2023.629836/).
