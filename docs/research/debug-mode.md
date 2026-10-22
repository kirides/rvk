# The engine's debug mode ("DebugMode"), the debug keys, and what they do to our Vulkan HAL

Static research (headless Ghidra, nothing run in the game), 2026-10-22. **[C]** = read in the decompilation / config files,
**[I]** = inferred. RVAs: Engine.dll / Game.dll / Script.dll / Script_Game.dll as marked (image base 0x180000000).
Dumps: `B:\DevDrive\risen-ghidra\debugmode\*.txt` (`game.txt`, `game2.txt`, `game3.txt`, `sg.txt`, `engine*.txt`, `cam.txt`,
`draw.txt`, `wire.txt`, `*callers*.txt`, `symrefs_*.txt`); helper scripts `XrefsTo.java`, `SymRefs.java` in the same directory.
Sections 0-7 are the static research; **section 8 records what the game actually did** (2026-10-23, test copy, D3D9 baseline
and Vulkan mode, `-Validation`), the fixes and the tooling; **section 9 is the user guide** (free camera for screenshots).

## 0. Summary

* There is **no "DebugMode"** setting. The switch is `/Engine/Debug/TestMode` (config `<Engine><Debug TestMode="true">`,
  console variable `game.bTestMode`). The debug keys only run while it is true. A second flag, **AdvancedTestMode**
  (`eCApplication+0xc49`), is needed for the free-fly camera; it is set only by typing the cheat code **`minsky`** in the
  game window (which also turns TestMode on).
* **F12 is "OnDebugQuitApplication"**: it quits the game on purpose (`eCApplication::OnQuit`). `ALT_R+F12` is
  "OnDebugDoNullPointerAccess": a deliberate write to address 0 (a crash by design, also in the stock game).
* Our test harness always ends the game with `Stop-Process -Force` (risen-spass `test-run.ps1:155`), so a **graceful engine
  shutdown with the Vulkan HAL has never been exercised**. F12 is the first thing that does it. Top suspect for "F12 crashes":
  HAL teardown order (section 4, A).
* The graphical debug keys split into: snapshot (F1, implemented), mode changes (F2 / Ctrl+F2: engine device reset path, unverified),
  wireframe (F3), indicator shaders (F4), and a family of **debug-draw** keys (bounding boxes, collision shapes, rays, navigation,
  skeleton, entity infos) that go through `eCGfxMixerAdmin::DrawLineList` and the **fixed-function** HAL slots, four of which are
  stubs in our HAL (109, 119, 134, 135).
* **Verified in game (section 8):** the "F12 crash" was ours: quitting crashed in d3d9.dll (RT scene torn down before the
  water's reflections; textures the engine frees after its device at process exit). Both fixed; debug lines now drawn like
  D3D9 (fixed-function emulation); free fly works. Ctrl+E crashes the stock game too.

## 1. What "DebugMode" is

| piece | where | meaning |
|---|---|---|
| `/Engine/Debug/TestMode` | `ConfigDefault.xml` `<Engine><Debug TestMode="false">` (`ConfigDefaultNew.xml` and `ConfigGenomeDefault.xml` ship `"true"`); read by `eSDebugOptions::eSDebugOptions` `Engine 0x82130` (`+0x10` key, `+0x18` default) | the **master switch of the debug keys** [C] |
| gate | `gCActionAdmin::ProcessBegin` `Game 0x804310`: `if (eCApplication vtbl[0xa1]() == 0) { if (config bool at eSDebugOptions+0x10) ProcessDebugKeys(); }` (`ProcessDebugKeys` `Game 0x802c80`) | evaluated **every frame**, so changing the config at run time applies at once [C]. Slot 0xa1 is probably "running inside the editor", false in the shipped game [I] |
| other TestMode readers | `eCApplication::OnSysCommand` `Engine 0x4bf460`; `eCGlobalStatistics::ToggleStatistics` `Engine 0x6f9740`; `gCSession::PostInitialize` `Game 0x970670` (copies it to `gCSession+0x108` bit 1) | see below [C] |
| `eCApplication::OnSysCommand` | with TestMode **off** it swallows `SC_KEYMENU` (0xf100: Alt / F10), `SC_TASKLIST` (0xf130) and `SC_SCREENSAVE` (0xf140); with it **on** they go to `DefWindowProc` | with TestMode on, a tap on Alt (e.g. before `ALT_L+...` debug keys) can start Windows' modal menu loop and freeze the render loop until the next key [C code, I effect] |
| Statistics (Ctrl+S) | `ToggleStatistics` does nothing unless TestMode is on | [C] |
| AdvancedTestMode | `eCApplication+0xc49` (`SetAdvancedTestMode` `Engine 0x4c1a10`, getter `0x4c1a20`) | only `gCActionAdmin::ScanForTestMode` `Game 0x803350` and `gCSession::SetAdvancedTestMode` `Game 0x974480` write it; no caller reads the config into it at start [C]. Needed by `OnDebugToggleFreeFlyCam` (`Script_Game 0xe71c0`); the achievements are disabled while it is on (`Achievement::*` call `gCSession::IsInAdvancedTestMode`) |
| the cheat code | `gCGameApp::OnHandleMessage` `Game 0x7a3b40` forwards every `WM_CHAR` (0x102) to `ScanForTestMode`: it keeps a typed buffer (cleared when a timer `+0x404` passes a threshold) and looks for the string `"lhmrjx"` with each char +1 = **`minsky`** (lower-cased). On a hit it toggles AdvancedTestMode, writes `/Engine/Debug/TestMode` = the new state into the config layer `s_strCfgLayer+0x18`, sets/clears `gCSession+0x108` bit 1 and prints "test mode activated/deactivated" to the game log | [C] |
| console | `game.bTestMode 0|1` ("toggles the testmode", `data\ini\keyboard_and_console.txt`) | sets the config value; AdvancedTestMode stays off [I] |
| command line | none: the only switches in Risen.exe / Game.dll are `-nosplash -nomenu -keyboardmode -controllermode -importquests` | [C] |

Config layering: `ConfigDefault.xml` (data\ini) < `ConfigLocal.xml` (data\ini, "values from ConfigDefault.xml can be overridden here") <
`%LOCALAPPDATA%\Risen\ConfigUser.xml` (which the game rewrites) [I for the order]. Neither the test copy's `ConfigUser.xml` nor
`ConfigLocal.xml` mention TestMode today.

## 2. The debug keys

Source: `ConfigDefault.xml` lines 757-818 `<Game><Controls><DebugKeys>` (`name="KEY"`; read by `gCActionAdmin::ReadDebugKeyMap`
`Game 0x7f9f40` -> `eCInputAdmin::AddKeyDebug` `Engine 0x5a2cf0`; the value is a script name from Script_Game.dll). Chords:
`eCInputAdmin::DispatchKeyPressed` `Engine 0x5a31c0` picks, per key, the binding whose modifier key is down (modifier bindings
win over the plain one), so plain F12 never also fires `ALT_R+F12`. The key state comes from the **posted WM_KEYDOWN messages**
(scan code = `(lParam >> 16) & 0x1ff`, extended flag 0x100), not from `GetKeyState` [C]. `OE`/`UE`/`AE` = the umlaut keys.
`keyboard_and_console.txt` of this build lists the same F-keys (F12 "Quit Application", F3 "Toggle RenderMode").
Every script starts with `Input::GetCurrentDebugEvent` and acts on event 1 (pressed).

| key | script (Script_Game RVA) | does | graphics path |
|---|---|---|---|
| F1 | `OnDebugTakeSnapshot` 0xe8540 | `eCApplication::Snapshot` (Engine 0x4bfc90): `#G3:/snapshots/<prefix>_<date>_<nnnn>.bmp`, flag `+0x144`; `DoSnapshot` 0x4c0c50 runs in `eCApplication::Draw` before `SwapScreen` | device slot **159 SaveBackbufferToFile**: implemented and verified in game (`-Snapshots`, missing-slots.md 3; the test detour calls the same `Snapshot()`) |
| F2 | `OnDebugToggleFullscreen` 0xe6970 | `ToggleFullscreen` 0x4c01f0 -> `SetFullscreen` 0x4c0280 -> `SetResolutionEx(.., mode 0x90000000 popup / 0xc80000 window)` | window style + size change, `eCApplication::Reset` -> device slot **ToggleNewMode** (ours: `dev_.resize`, `hal/vulkan/device.cpp:786`). **Breaks the "always windowed" rule** |
| Ctrl_L+F2 | `OnDebug_ChangeResolution` 0xe6890 | `ToggleDebugResolution` 0x4c09f0: cycles `/Engine/Debug/Resolutions` Test1..7 (800x600 ... 1280x1024 `Window`) | same reset path (windowed). spass: a run-time resize misaligns GUI and mouse |
| F3 | `OnDebugToggleRenderMode` 0xe8380 | `eCShaderMaterialAdmin::ToggleWireframe` 0x6b6240: `+0x340` 0 <-> 2 | `SetCurrentRenderStates` 0x6b7c50 calls device slot 36 **SetMasterRenderMode(2)** per material draw (value = D3DFILL_WIREFRAME); ours: `master_[M_RENDERMODE]`, `RS_FILLMODE` -> `VkPolygonMode LINE` (pipeline key `polygonMode`, `fillModeNonSolid` enabled when supported) |
| F4 | `OnDebugCycleIndicators` 0xea270 | counter 0..8 (skips 4) -> `eCShaderMaterialAdmin+0x31c` (overwrite indicator), and `eCImageFilterAdmin::ActivateDefaultEffect` / `ActivateEffect("Indicator")` | shader variants `XFX_INDICATOR_DEPTH / AMBIENTOCCLUSION / DIRLIGHTSHADOWS / LIGHTMAPS / MISC`: `FUN_1806da1f0` / `0x6d9cf0` (CompileIndicator) read **`ge_default_indicator.fx` from the VFS** ("Shader file:%s not found on disk!" and no compile when absent; not found in `effects.pak` strings [I: paks may be compressed]); if present they compile HLSL (device `Compile`, ours via the engine's own eCDX9 compile code on a stand-in, `resources.cpp:528`) and add pool elements: **rule 1 hazard (`ShaderMaterialPool_User.smp` through the junction)** |
| F5 / F6 | `OnDebugToggleSpeedUp` 0xe6b50 / `OnDebugToggleSloMo` 0xe6cf0 | `PSClock::SetFactor` / `eCTimer::SetTimeScaling` | none |
| F7 | `OnDebugResetPlayer` 0xe7950 | script `0x1802cbe10` on the player | none |
| **F12** | `OnDebugQuitApplication` 0xe88f0 | `eCApplication::OnQuit` Engine 0x4c5a20: sets the quit flag `+0x13` | normal engine shutdown (section 4, A) |
| Alt_R+F12 | `OnDebugDoNullPointerAccess` 0xea3c0 | `*(int*)0 = 0` | deliberate access violation (stock game too) |
| Pause | `OnDebugToggleTimeFreeze` 0xe8bb0 | `SetAppState(GetAppState()==8 ? 0 : 8)` | none (scene keeps rendering) |
| Num_8 | `OnDebugToggleFreeCam` 0xe6e00 | `Entity::SetCameraTarget(None)` + `SetCurrentCameraByName("cam_free")` (back: `"cam_normal"`), log text "Free Cam Mode activated"; only when `PSCombatSystem::GetCombatMode < 3` | none |
| Ctrl_L+Insert | `OnDebugToggleFreeFlyCam` 0xe71c0 | needs `IsAdvancedTestModeEnabled`; `gCCameraAdmin::OnToggleFreeFlyCam` 0x7ac530 (toggles `+0x38`, copies the pose into `+0x2c0`) | none |
| Ctrl_L+Home | `OnDebugToggleFixedCam` 0xe70e0 | `OnToggleFixedCam` (`+0x38 = 0x101`) | none |
| Ctrl_L+PageUp | `OnDebugMovePlayerToCamera` 0xe7e60 | player `MoveTo` the camera pose | none |
| Ctrl_L+H | `OnDebug_ToggleHUD` 0xe6a50 | `GUI2::SetPageMode(1)` / restore | GUI only (good for promo shots) |
| Ctrl_L+M | `OnDebugToggleScreenMessages` 0xe8ab0 | `gCGUIEntityRenderer::SetRenderEntityNames` | GUI text |
| Ctrl_L+S | `OnDebugToggleStatistics` 0xe8460 | `ToggleGlobalStatistics` 0x4c0960 (needs TestMode) | GUI text; the gfx counters are the `eCDX9Device::ms_pReg*` callbacks that never fire in Vulkan mode: zeros, harmless [I] |
| Ctrl_L+E | `OnDebugToggleEntityInfos` 0xe8620 | `eCDebugModule::ShowEntities` | debug draw (below) |
| Ctrl_L+G | `OnDebugToggleStateGraphInfos` 0xe89d0 | toggles a static render-debug flag | debug draw |
| Ctrl_L+B | `OnDebugToggleBoundingBoxes` 0xe7d70 | `eCRenderSystemAdmin` frustum-database caps template `+4` cycles 0..2 -> `eCFrustumDatabase::DrawBox` 0x66d0c0 | debug draw |
| Ctrl_L+Q / Ctrl_L+R | 0xe7bb0 / 0xe7c90 | `eCPhysicsScene::ToggleRenderCollisionShapes` / `ToggleRenderRaycasts` (PhysX debug triangles / lines) | debug draw |
| Ctrl_L+N | 0xe81d0 | `gCDummyCollectorAdmin::ViewNavigation` (+ collision helpers) | debug draw |
| Ctrl_L+OE / Ctrl_L+AE | 0xe8e70 / 0xe8f50 | `eCAnimation_PS` `ms_bRenderDebug` / `ms_bRenderDebugCM`: skeleton, collision mesh | debug draw (`DrawSkeleton`, `DrawNodeAxes`) |
| Ctrl_L+T / Ctrl_L+V | 0xe80f0 / 0xe8010 | `eCRenderContextSpeedTree::ToggleRendering` / `eCVegetationAdmin::ToggleVegetation` | removes draws, no new HAL path |
| Ctrl_L+P, +UE | 0xe8cb0, 0xe8d90 | PhysX visual debugger connect (network), dump physics groups | none |
| Ctrl_L+K, F8, L, X/Y/C, F, A, O; Alt_L+U/I/J/K/L/M/S | various | beam forward, reset with lift, levitate, kill / defeat, focus entity, audio emitter, autorun, head cycling, profile logging, movement debug, say SVM | none |

### 2.1 Debug draw: the fixed-function path

`eCGfxMixerAdmin::DrawLine / DrawLineList / DrawBox / DrawSphere / DrawCircle / DrawTriangle*` (Engine 0x50f270, 0x50f010,
0x50cc90, 0x50c670, 0x50b720, 0x50eb50...) are the only drawing code of these keys. `DrawLineList` 0x50f010 [C] does, on the device
(slot numbers of `eCAPIDevice`, `api_generated.h`):
`GetLighting(59)`, `SetLighting(58)(0)`, `SetColorFactor(89)(colour)`, `GetAlphaBlending(61)`, `SetAlphaBlending(60)`,
`SetBlending(90)`, `SetAlphaTesting(91)`, **`SetColorCalc(134)` x2, `SetAlphaCalc(135)`** (texture-stage ops, stage 0 and 1),
**`SetVertexProgram(109)(fvf 2 = D3DFVF_XYZ, true)`** (the D3D9 body `0x743430` turns that into `SetVertexDeclaration(NULL)` +
`SetFVF`), **`SetPixelProgram(119)(0)`**, `SetDepthTesting(73)`, `SetDepthWrite(74)`, then
`DrawUP(84)(LINELIST, n/2, data, 12)`, then it restores lighting and alpha blending. `DrawBox` first calls `SetWorldMatrix(79)(identity)`.

Our HAL: everything is implemented **except 109, 119, 134, 135** (generated stubs `stub_generated.h:1477, 1544, 1646, 1654`:
log once, return Ok; no fixed-function emulation). So the `DrawUP` is drawn with **whatever vertex/pixel shader and vertex declaration
the previous material draw left bound**, or skipped (`skippedNoShader`) when none [I]: wrong or no lines, not an obvious CPU crash.
Risks: a stale vertex declaration with attributes beyond the 12-byte stride reads the rest of the upload ring (no CPU effect, validation
noise; an out-of-range fetch at a ring-chunk end could in theory fault the GPU); `Frames::allocate` returns an empty `RingAlloc` when a
chunk cannot be created and `drawPrimitiveUP` then `memcpy`s to `nullptr` (`d3d9/draw.cpp:910`, `vk/frames.cpp:222`), reachable with
the very large line lists of collision shapes. `missing-slots.md` 7 ("assembly programs / FF texture stages are not called in the game")
is therefore wrong for debug draw: they are called whenever one of these keys is on.

## 3. The free camera (for promo shots)

* **Num_8 (`cam_free`)**: the engine's camera type `cam_free` (61 camera types in `compiled_camera.bin`; spass RE.md: AxisX true,
  MoveInCameraSpace true, ControlRotation false). The camera is detached from the hero (`SetCameraTarget(None)`); the free-cam
  block of the type is at `+0xe8..`. Console tuning: `FreeCamSpeed`, `FreeCamSpeedModifier`, `FreeCamFOV`, `FreeCamTurnSpeedFactor`
  (Script_Game `CON_FreeCam*` 0x150600 / 0x150780 / 0x150900 / 0x150a80 -> `gCCameraAdmin::SetFreeCam*` Game 0x7a83f0 / 0x7a8390 / 0x7a8330 / 0x7a8270).
  Back with Num_8 again (needs a "normal cam" for the creature).
* **Ctrl_L+Insert (free fly)**: after typing `minsky`. `gCCameraAdmin::OnAction` 0x7abe70 then feeds the normal movement actions
  (held flags `+0x336..0x33b`, forward / back / strafe / up-down) and the mouse axes (yaw `+0x340`, pitch `+0x344`, scaled by the camera
  type's `+0xf4 / +0xf8`) into the free pose `+0x2c0` instead of the hero [C structure, I mapping of action numbers to WASD / mouse].
  While the camera is not in free-fly and `gCSession+0x108` bit 0 is set, the normal camera actions are ignored.
* Ctrl_L+Home = fixed camera, Ctrl_L+PageUp = move the player to the camera, Ctrl_L+H = hide the HUD, Pause = freeze time,
  F6 = slow motion, F1 = engine snapshot BMP (to `<localappdata>\Risen\ScreenShots` in the test copy).
* For renderer features keyed to the **player** position (shore field / sea floor field "player position", ocean foam buffer is
  camera-centred, RT scene, shadow cascades use the camera) the free camera far from the hero is a new situation: test it.

## 4. Crash candidates, in order of suspicion

A. **F12 = quit -> graceful shutdown in Vulkan mode** (never exercised: the harness kills the process).
   `HalTexture::~HalTexture` / `HalVertexData::~HalVertexData` call `dev_.dev().release(...)` through a `HalDevice&` that is **not
   reference counted** (`resources.cpp:46, 271`; the engine's `Release` frees the device when its count hits zero, the resources are
   separate objects). D3D9 resources keep the device alive via COM, so the engine never had to order the two. If the engine releases
   `eCGfxContextAdmin::ms_pAPIDevice` before the last textures / buffers / effects (resource cache teardown), those destructors run on
   a destroyed `HalDevice` [I]. Also: `HalDevice::~HalDevice` (overlay shutdown, `dev_.destroy()`), threads still running (pipeline
   compile workers, present thread), `onProcessDetach` (`bootstrap.cpp:378`). Windows additionally reserves F12 for the debugger
   (a break when a debugger is attached; harmless otherwise).
B. **Alt_R+F12** (or a layout that sends it, e.g. AltGr = Ctrl_L+Alt_R + F12): crashes by design in every renderer.
C. **F2 / Ctrl_L+F2**: `eCApplication::Reset` releases every default-pool resource, calls `ToggleNewMode`, recreates them
   (`hal-structs.md` 2). Ours only resizes the back buffer / depth image (`Device::resize`, `d3d9/device.cpp:276`); PLAN M5 still
   lists "alt-tab/resize" as open; features holding screen-sized transient images (water, shadows, RT scene, overlay) are untested
   across a resize. Keep out of the test loop (fullscreen violates the windowed rule).
D. **Debug-draw keys** (Ctrl_L+B / Q / R / N / E / OE / AE): section 2.1 (stale shaders, null ring allocation).
E. **F3**: `VK_POLYGON_MODE_LINE` for every pipeline: thousands of pipeline variants unless EDS3 polygon mode is active (hitch,
   not crash); validation noise if `fillModeNonSolid` is off.
F. **F4**: indicator shader compile (section 2 table); also writes the user shader pool if it ever compiles.
G. TestMode on: Alt / F10 start a modal menu loop (freeze, not crash).
H. Not graphics: the keys call Script_Game scripts through `eCScriptProxyScript`; if a **non-graphics key** (F5 / F6 / F7) also
   crashes, the HAL is not the cause and the faulting module in the dump decides it.

## 5. Would the stock D3D9 renderer crash too?

From the code: F12 quits cleanly; Alt_R+F12 crashes by design; every other key is a working developer tool (the DebugKeys are shipped
in `ConfigDefault.xml`, only the TestMode gate keeps them off). Whether they behave in this PGO x64 build with D3D9 is untested:
run each key once with `-Mode passthrough` first (section 7). F4's `ge_default_indicator.fx` and `Indicator` effect may simply be
missing in the retail data in both renderers.

## 6. Open points

* Slot 0xa1 of `eCApplication` (gate of ProcessDebugKeys / OnQuit / DoRender): meaning not decompiled.
* Which config layer wins (Local vs User) and whether the game rewrites `TestMode` into `ConfigUser.xml` on exit after `minsky`.
* Whether `ge_default_indicator.fx` and the `Indicator` image-filter effect exist in the paks.
* `OnToggleFreeFlyCam` action numbers (1..6, 7..10 of `gCCameraAdmin::OnAction`) to real input actions.

## 7. Verification plan (in game, test copy only)

Prepare (the test copy `B:\DevDrive\risen-test-vk`, windowed; never the real install; `data\common` / `data\compiled` are junctions):

1. **Enable TestMode** in the copy only. Either `localappdata\Local\Risen\ConfigUser.xml`: inside the existing `<Engine>` add
   `<Debug TestMode="true"/>` (the file lives in the copy; `test-run.ps1` rewrites window size / audio in it before a start, so edit
   after that or use a run mode that keeps it), or the copy's `data\ini\ConfigLocal.xml` (a real directory in the copy): add the same
   inside its `<Engine>`. Or type `minsky` (needs a `WM_CHAR` post; sets both flags and the config).
2. **Send the keys.** `run.ps1 -Commands '700: key f3'` posts WM_KEYDOWN/UP with the scan code (`overlay.cpp:339`), works in the
   background, but `vkFromName` knows only A-Z, 0-9, F1-F12, escape / enter / space / tab / arrows / backspace: **no modifiers, no
   numpad (Num_8), no Pause, no WM_CHAR**. Needed for Ctrl chords, Num_8, Pause and `minsky`: extend the `key` command (e.g.
   `keydown / keyup <name>` + `char <text>`), or post from outside with `PostMessageW(hwnd, WM_KEYDOWN, vk, 1 | scan << 16 | ext << 24)`
   (the engine keeps its own key-state array from the messages, so `LCtrl` down, `Insert` down works; extended keys use scan | 0x100).
   `-AllowFocus` is not required. Keys work only in the world (frame ~450+ of `testdemo`), not in the menu.
3. **Baseline D3D9 first**: `.\scripts\run.ps1 -Mode passthrough -Mods testdemo -Scenario testdemo -WaitForResult -Seconds 240 -Commands '600: key f12'`
   (the overlay `key` command is a proxy feature: if passthrough does not run commands, post from outside). Expect a clean exit.
4. **One key per run** (a crash must be attributable): F1, F3, F4, F5, F6, F12 on `-Mode vulkan`, with `-Validation` (and once
   `-SyncValidation`), `-RvkLogLevel debug`; screenshots `-Screenshots '650,700'`. Then the chords (Ctrl_L+B, Q, R, N, E, OE,
   H, S) and the free camera (Num_8; `minsky` then Ctrl_L+Insert, move, F1 snapshot).
5. **F12 specifically**: `-Commands '600: key f12'` in Vulkan mode; the run must end by itself. Compare with a menu quit if the
   scenario allows. Capture: `rvk.log` tail (the "process detach (terminating = ...)" line, any `hal:` destructor logs),
   `bin\spass\crash\*.dmp` + `bin\spass\spass.log` (`crash_dump = "mini"` in `bin\spass\config.toml`), Windows Event Log /
   WER dump of Risen.exe, the exit code. The faulting module (`d3d9.dll` = ours, Engine.dll, Game.dll, nvoglv/amdvlk = driver) and the
   stack decide between A and the rest. A cheap extra: log in `HalDevice::~HalDevice` and in the resource destructors to see the order.
6. After each run grep `rvk.log` for `unimplemented` (expected new: device 109, 119, 134, 135 after a debug-draw key; none for
   F1 / F3), `hal: ToggleNewMode` (F2), `snapshot`, `warn|error`; after F4 check read-only that `Data\Compiled\Materials\ShaderMaterialPool_User.smp` (the
   junction target in the real install) was not written (timestamp unchanged).
7. Result handling: record confirmed / refuted per key in this file, fix the confirmed cause, update `missing-slots.md` 7
   (109 / 119 / 134 / 135 are reached by debug draw) and PLAN.md.

## 8. In-game results (2026-10-23)

Runs in the test copy (`run.ps1 -TestMode -Mods rvk_debugkeys`, windowed, keys posted by `[debug] commands`), every key in the
game's own D3D9 renderer first (`-Mode passthrough`), then in Vulkan mode with `-Validation`. Crash evidence: spass's
`crash.log` / minidumps, the Windows `Application Error` events (Event ID 1000), our pdb (`llvm-symbolizer --obj=d3d9.dll`).

### 8.1 Per key: confirmed vs the static claims

| key | D3D9 | Vulkan before the fixes | Vulkan now |
|---|---|---|---|
| F12 quit | quits, exit code 0 | quit, then **crash in d3d9.dll** during process exit (8.2 A); with `rt.reflections` on a crash in `RtScene::release` (8.2 B) | quits; the remaining exit faults are the game's / Steam's (8.2 C) |
| F1 snapshot | BMP in `<localappdata>\Risen\ScreenShots` | same | same (verified: `Snapshot__<date>_<n>.bmp`) |
| F3 wireframe | wireframe | wireframe (no validation messages) | same |
| Ctrl+B / Ctrl+Q / Ctrl+R (boxes, collision shapes, raycasts) | white boxes, white / red / purple shapes, white rays | lines in the colours of the last material's shaders (stubs 109 / 119 / 134 / 135, logged as unimplemented) | **same picture as D3D9** (8.3) |
| Ctrl+N, Ctrl+OE, Ctrl+AE (navigation, skeleton, collision mesh) | green nav lines, filled white / blue / yellow volumes | same as D3D9 (these draw with shaders) | same |
| **Ctrl+E entity infos** | **crash**: `Script_Game.dll+0xdc066` (`OnGetEntityDebugLines`+0x6a6), read of a truncated 32-bit pointer | not run (stock crash) | game bug, also in the user's event log (2026-10-22 21:31) |
| Ctrl+S statistics | statistics GUI | same (gfx counters read 0, as predicted) | same |
| Ctrl+F2 debug resolutions | windowed 800x600, 960x540, ... | `ToggleNewMode` -> swapchain resized, no messages | same |
| F5 / F6 speed-up / slow motion | not run | no crash, no messages (effect not checked) | same |
| Pause time freeze | app state 8 | app state 8, the scene keeps rendering | same |
| Num_8 `cam_free` | "Free Cam Mode activated" | same; pressed during the intro cutscene, the cutscene's end puts `cam_normal` back | not renderer-related |
| `minsky` + Ctrl+Insert free fly | - | "test mode activated", the camera flies with W/A/S/D, Space up (Ctrl_L is bound to MoveDown), mouse look via raw input (not scriptable) | verified: ~1,100 units forward, 420 up, strafing; HUD off (Ctrl+H), freeze, F1, back to the hero with Ctrl+Insert; also with water / ocean / RT reflections / shadows on, no validation messages |
| F4 indicators, F2 fullscreen, Alt_R+F12 | not run | not run | F4 can write `ShaderMaterialPool_User.smp` through the copy's junction into the real install (rule 1), F2 breaks the windowed rule (Vulkan mode: borderless, window-mode.md), Alt_R+F12 is the deliberate null write |

Corrections of the static research: the free-fly up / down keys are Space / Ctrl_L (`MoveUp` / `MoveDown`); `missing-slots.md` 7
was wrong for 109 / 119 / 134 / 135 (the debug draws reach them; now implemented for FVFs). The key-state claim holds: posted
`WM_KEYDOWN` / `WM_KEYUP` (scan code + extended bit) drive the debug keys, also with the window in the background; a press and
a release one frame apart is seen as a press.

### 8.2 The crashes on quit

**A. Resources released after the device (ours, fixed).** The engine releases its device (`HalDevice::~HalDevice`) during the
normal shutdown, but 7 textures only later, in Game.dll's static destructors at `ExitProcess` (stack: `ntdll` DLL detach ->
`Game.dll+0x5a6466` -> `Engine.dll+0x50088d` -> `HalTexture::~HalTexture` -> `HalDevice::unregisterTexture` on freed memory). D3D9
keeps its device alive through COM references; ours did not. Fix: `HalDevice::isLive`; a HAL resource destroyed after its device
leaks its d3d9 object (the process is exiting), and `d3d9::Device::destroy` frees the GPU objects of every texture / buffer still
alive (a live registry, `orphan()`): no use after free, and no "17 leaked objects" at `vkDestroyDevice` with validation on.

**B. RT scene torn down before its consumers (ours, fixed).** `~HalDevice` reset `rtScene_` before `water_`; the water's traced
reflections (`WaterRt`) call `RtScene::release` in their destructor. The user's crash of 2026-10-22 22:04 (`d3d9.dll+0x2f2c9a` =
`rvk::enh::RtScene::release`, symbolized with the pdb next to that build) is this one: every quit with `rt.reflections` on.
Fix: consumers first (`rtDebug_`, `water_`, `shadows_`), then the scene.

**C. Not ours.** After the fixes a quit can still end in `SharedBase.dll+0x7caa9` (`call [rax+8]` on a code address while
Game.dll's static destructors run; also with the game's own D3D9 renderer in the same harness, and many times in the user's
event log from earlier sessions; spass RE.md 13 saw it too) or in `tier0_s64.dll_unloaded` (a Steam client thread). Both happen
after `ExitProcess(0)`: the save / config writes are done, the exit code is mostly 0. Left alone (game / Steam code).

Other crashes in the user's event log of 2026-10-22: `d3d9.dll+0x2f2a0a` (4x, three different builds) and `+0x20e13a` (1x) came
from builds whose pdbs are gone; those builds were RT-scene work in progress, so probably B; not reproducible now.

### 8.3 Fixed-function debug drawing (d3d9/ffshader.*)

`eCGfxMixerAdmin::DrawLineList` sets `SetVertexProgram(D3DFVF_XYZ, fvf = true)` (eCDX9: `SetVertexDeclaration(NULL)` + `SetFVF`,
no vertex shader), `SetPixelProgram(0)` (no pixel shader), stage 0 `SetColorCalc(0, SELECTARG1, TFACTOR, TEXTURE)` and the same
for alpha, stage 1 disabled, `SetColorFactor(colour)`, then `DrawUP(LINELIST, n, data, 12)`. Ours now:

* `SetVertexProgram(fvf, true)`: the FVF's declaration + a generated `vs_3_0` (position through c248..c251 = transposed
  world * view * projection from `SetWorldMatrix` / `SetViewAndProjectionMatrices`; colour = the FVF's diffuse or white).
  FVFs without an untransformed XYZ position (XYZRHW, blend weights) are logged once and skipped. Assembly programs (fvf = false)
  stay unimplemented (no `CreateVertexProgram` caller).
* `SetPixelProgram(0)`: a generated `ps_3_0`, `oC0 = v0 * c220 + c221`.
* `SetColorCalc` / `SetAlphaCalc`: stage 0 recorded; at the draw it is reduced to that linear form (DISABLE, SELECTARG1/2,
  MODULATE(2X/4X), ADD over DIFFUSE / CURRENT / TFACTOR; the texture counts as white).
* Draw / DrawUP / DrawIndexedUP: while a generated shader is bound, the four VS and two PS registers are saved, written,
  drawn, restored (the engine's constants stay as they were). rvk.log: `hal: first fixed-function draw ...`.
* The bytecode equals what D3DX assembles from the listing (`rvk_tests shader_fixed_function`) and goes through the normal
  translator (cached SPIR-V like any engine shader).
* Also: `drawPrimitiveUP` / `drawIndexedPrimitiveUP` drop the draw (warn once) when the upload ring cannot take it, instead of
  a `memcpy` to null.

### 8.4 Tooling

* `[debug] commands` (`run.ps1 -Commands`): `key <chord> [frames]` (e.g. `key ctrl+insert`, `key w 150` = held 150 frames,
  default 1), `keydown <chord>` / `keyup <chord>`, `type <text>` (WM_CHAR, e.g. `type minsky`). Chord names: `ctrl` / `ctrl_l` /
  `ctrl_r`, `alt` / `alt_l` / `alt_r`, `shift` ..., `insert`, `home`, `page_up`, `pause`, `num0`..`num9` (= `num_8`), `oe` / `ae` /
  `ue` (by scan code), `f1`..`f12`, letters, digits, arrows, `space`, `enter`, `escape` (src/overlay/keys.h). In Vulkan mode the
  overlay runs them at presents; in passthrough / trace mode `bootstrap` runs the key commands at `eCApplication::Draw` calls
  (so the D3D9 baseline gets the same input; frame numbers then count Draw calls).
* `run.ps1 -TestMode`: `<Engine><Debug TestMode="true">` in the copy's `ConfigUser.xml` (that layer wins over ConfigLocal /
  ConfigDefault: verified, the debug keys work); every run without the switch removes it again.
* `mods/rvk_debugkeys`: no scenario (the game runs until `-Seconds` or a quit key), closes the start window, logs the camera
  (`risen.camera.get`) and the app state every second: free-fly movement shows as changing positions.
* Example (free fly, screenshots before / after, quit):
  `.\scripts\run.ps1 -Mode vulkan -Validation -TestMode -Mods rvk_debugkeys -Seconds 400 -Commands '1500: type minsky','1550: key ctrl+insert','1600: key w 150','1800: key ctrl+h','2450: key f12' -Screenshots '1540,1790,1880'`
  (the intro cutscene runs until about frame 1200 in Vulkan mode: camera keys before that are undone by it).

## 9. User guide: debug mode and the free camera (for screenshots)

**Enable the debug keys.** Either type `minsky` in the running game (lower case, in the game world, not in a menu; the game
log says "test mode activated"): this turns on the debug keys **and** the free-fly camera, and the game writes
`TestMode="true"` into your config; type it again to turn it off. Or edit `%LOCALAPPDATA%\Risen\ConfigUser.xml` (game closed)
and add inside `<Engine>`:
```xml
<Debug TestMode="true">
</Debug>
```
That enables the debug keys but not free fly (type `minsky` for that). Remove the element (or set `false`) to switch it off.
Achievements are disabled while the `minsky` test mode is active.

**Free camera.**
* `Ctrl (left) + Insert`: free-fly camera on / off (needs `minsky`). Fly with W / S / A / D, Space up, left Ctrl down, mouse to
  look. Console (the game's console, `data\ini\keyboard_and_console.txt`): `FreeCamSpeed <n>`, `FreeCamSpeedModifier <n>`,
  `FreeCamFOV <degrees>`, `FreeCamTurnSpeedFactor <n>`.
* `Num 8`: the simpler free camera (`cam_free`, detached from the hero); press it again for the normal camera. A cutscene
  ending puts the normal camera back.
* `Ctrl (left) + Page Up`: move the hero to the camera. `Ctrl (left) + Home`: fixed camera.

**Clean pictures.** `Ctrl (left) + H` hides the HUD (again to show it), `Pause` freezes time (animations and the world clock stop;
whether the free-fly camera moves while frozen was not tested), F6 slow motion, F5 speed-up. Pictures: F1 = the engine's snapshot (a BMP in
`%LOCALAPPDATA%\Risen\ScreenShots`, at the game's resolution), or any screenshot tool; the renderer's overlay (F10) is never in
F1 snapshots.

**Warnings.**
* F12 quits the game at once (no question; progress since the last save is lost).
* `Alt Gr` / right `Alt` + F12 crashes the game on purpose (a developer test key, also with the original renderer).
* `Ctrl + E` (entity infos) crashes the game (a bug of the 64-bit game, also with the original renderer).
* F4 (shader indicators) may make the game compile debug shaders into `data\compiled\Materials\ShaderMaterialPool_User.smp`
  of the installation (the game's own shader cache; delete that file to get rid of them). Not tested.
* F2 toggles fullscreen (in Vulkan mode a borderless window); `Ctrl + F2` cycles small windowed debug resolutions: set your
  resolution again in the options afterwards.
* With debug mode on, tapping Alt alone can open Windows' window menu and pause the game until the next key.
* Quitting can still end with an error report from `SharedBase.dll` or Steam's `tier0_s64.dll` after the game has shut down
  (game / Steam bugs, also without this renderer); the game has saved its settings before that.
