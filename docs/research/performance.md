# Performance and frame pacing (M5)

Measurements and findings of the first performance pass (2026-10-21). Machine: AMD Radeon RX 7900 XTX (Adrenalin, Vulkan
1.4.355), 120 Hz primary display (DWM clock 120.01 Hz) + a 60 Hz virtual display, game window 1600x900 windowed.

Commits: `4fa8a57` (instrumentation, present thread, vsync override), `764ac2e` (pacer), `c1041dd` (draw path, pipeline
cache saving), `9f2a164` (pipelines of earlier runs, optional pipeline libraries).

## 1. How to measure

```powershell
.\scripts\run.ps1 -Mode vulkan -Mods rvk_shadows -Scenario rvk_shadows -WaitForResult -Param hold=25,shots=1 -Perf [-PerfFrame 2500]
.\scripts\run.ps1 ... -Vsync off           # [vulkan] vsync = false: uncapped ("engine" = the game's option, true)
.\scripts\run.ps1 ... -MaxFps 60           # [vulkan] max_fps
.\scripts\run.ps1 -NoRenderer -Mods rvk_shadows -Scenario rvk_shadows ...   # D3D9 reference (fps from the scenario's log)
```

* Scene: `rvk_shadows` (new game, start window closed, clock held at 12:00, the camera on the beach for `hold` s; ~1440
  draws, 35 render passes per frame). The scenario logs `rvk_shadows: fps <n>` every 5 s from `ui.frame` for either
  renderer, so the D3D9 reference has a comparable number.
* `[debug] perf` (`-Perf`) adds three `perf:` lines to the 5 s stats in rvk.log:
  * frame interval p50 / p95 / max (present to present); CPU per frame: *engine+HAL* = from the end of one `present()`
    to the start of the next (the engine's frame including our HAL work), of it *draw calls* (inside `Device::draw*`:
    state translation, uploads, pipeline lookup, command recording) and *read-backs* (mid-frame GPU flushes); *present* =
    inside `present()` (pacer, waiting for the present thread, acquire, blit recording, submit), *slot wait* = waiting
    for a free frame slot (GPU behind); GPU per frame from timestamps (`vk::GpuTimer`, per submission, read two
    submissions later; needs `hostQueryReset`).
  * draws, render passes, pipeline binds per frame; ring memory handed out, texture / buffer bytes uploaded per frame;
    pipelines built at draw time, shaders translated (glslang only on SPIR-V cache misses), with their time.
  * the pacer: frames that waited, sleep + spin per wait, timer lateness, release error, cadence restarts.
* `[debug] perf_frame = N` (`-PerfFrame N`) logs the GPU time of every render pass of frame N (timestamps around each
  pass and between passes: barriers, copies, inline uploads).
* The `hal:` line has the fps, the last frame's draws, the time in `vkQueuePresentKHR` (on the present thread) and how
  long the render thread waited for it.

**Caveat: the GPU is shared.** Another game (AION 2) ran on the same GPU during all measurements, and other agents ran
Risen instances in parallel at times. GPU times per world frame were 0.9-1.2 ms when nothing else rendered and up to
3.5-18 ms under contention; CPU numbers vary ~+-15 %. Compare runs taken close together.

## 2. What limited the frame rate: an external ~80 Hz present cadence

In the world the game runs at **79.5-80.0 fps** whatever the renderer does:

| configuration (beach scene) | fps | notes |
|---|---|---|
| Vulkan, FIFO + pacer at 120 (before) | 79.5 | 5.8 ms per frame inside `vkQueuePresentKHR` on the render thread |
| Vulkan, IMMEDIATE (vsync off), no pacer | 79.5 | the same 5.8 ms: not vsync |
| Vulkan, present thread (after) | 79.9 | `vkQueuePresentKHR` now takes **12.4 ms** on its thread, the render thread waits ~5.7 ms for it |
| Vulkan, `max_fps` 60 | 60.0 | `vkQueuePresentKHR` 0.11 ms: presents below ~80 Hz never block |
| Vulkan, loading screens, vsync off | 184-210 | presents 0.12 ms: the cadence only applies in the world |
| D3D9 (game's renderer), vsync on | 28-30 | quantised to a quarter of 120 Hz |
| D3D9 (game's renderer), vsync off (`ConfigUser.xml`) | **79.9** | the same cap with the game's own D3D9 renderer |

* Neither CPU nor GPU is the limit: CPU per frame (engine+HAL) 6.0-7.5 ms, GPU 1.0-1.2 ms (uncontended).
* The frame interval is a steady 12.5 ms (p50 12.50, p95 13-14 ms), not an alternation of 1 and 2 vblanks (80 =
  120 x 2/3 was a coincidence of the numbers).
* `vkQueuePresentKHR` returns on a fixed ~12.5 ms cadence: with the presents on their own thread it blocks 12.4 ms, on
  the render thread it blocked 12.5 ms minus the frame's CPU time. With FIFO and IMMEDIATE alike. Synchronous
  experiment: waiting for the frame's GPU completion first (3.8 ms) left 2.0 ms in the present: the same total.
* The engine does not limit: `eCTimer::Update` (Engine.dll 0x4cf0e0) sleeps only for a fixed frame time (+0x40), and
  risen-spass found `<Timer MaxFPS>` not honoured ("~80 ticks/s always", risen-spass docs/DEVELOPMENT.md "Startup time").
  `eCTimer::RelaxTick` (0x4ced90) only smooths the frame time.
* Global AMD settings: Radeon Chill off, FRTC off (registry `KMD_ChillEnabled=0`, `Gmx_FRTCEnabled=0`). The driver knows
  Risen (Adrenalin game list `amdId` 537, "profileCustomized": TRUE); the per-game settings are not readable from the
  registry.
* ~~Resolved (user, 2026-10-21): Radeon Chill in the per-game profile, disabled~~: **wrong.** The user switched Chill off
  on 10-21, but every world run afterwards was still capped. **Measured 2026-10-23 (docs/research/fps-limit.md 9): the cause
  is the AMD driver's Radeon Chill limiter from an Adrenalin per-application profile for `Risen.exe` that has Chill on with
  min = max = 80 fps** (read from the driver's globals in the running process). 80 % of every 12.4 ms present is
  `amdvlk64.dll` sleeping in its frame limiter (`[debug] present_stalls` stacks). Chill skips frames below 150 draws
  (loading-screen detection), hence uncapped menus and loading screens. The same executable renamed (`run.ps1 -Exe
  RisenTest.exe`) runs the world at **175-240 fps** with vsync off (CPU-bound: engine+HAL 4-5.5 ms per frame). Not the
  Steam overlay or OBS (all implicit layers disabled: unchanged). What the user can do: fps-limit.md 11.

## 3. Present thread

Before, `vkQueuePresentKHR` blocked the render thread for the rest of the 12.5 ms cadence; with any cap below 80 the
blocked presents interleaved with the pacer (section 4). Now `vk::Presenter` runs the presents on its own thread
(`src/vk/presenter.*`); queue access (submit, present, `vkDeviceWaitIdle`) goes through `Context`'s queue lock; the
render thread waits for the present thread only before it touches the swapchain again (acquire, recreate). Read-backs
wait for their own submission instead of `vkDeviceWaitIdle`.

The draw counter of the stats line was reset by every mid-frame flush (read-backs flush and start a new command buffer),
so a frame with a read-back reported only the draws after it. It now spans every submission of the frame. The engine
presents once per frame (presents = frames = `ui.frame`). The "0 draws" alternation in the character screen was not
reproducible afterwards, also not with the pre-session build (`rvk_charscreen`: 1465-1567 draws per sampled frame, 18
in the main menu at the end).

## 4. Frame pacing

`vk::FramePacer` (`src/vk/pacer.*`), the waiting adapted from GD3D11's `fpslimiter.h` (same author): QPC time base, a
high-resolution waitable timer for the bulk, a `YieldProcessor` spin for the rest. Changes:

* **Cadence**: frame k is released at t0 + k x interval; a slightly late frame is made up by the next one (exact average
  rate), a frame later than one whole interval restarts the cadence (no burst after loading / hitches).
* **Vsync lock**: with vsync the interval is DWM's measured refresh period (`DwmGetCompositionTimingInfo`, 120.01 Hz
  here, not the nominal 120) and the slots are phase-locked to DWM's vblank, also for integer fractions (`max_fps` 60
  on 120 Hz = every second vblank). The DWM clock must match the window monitor's nominal rate within 0.5 % (it is the
  primary display's clock; under load single readings stray, e.g. 116.9 Hz once during a validation run), otherwise the
  nominal rate is used unlocked and the lock is retried once a second.
* `max_fps` below the refresh rate now also applies with vsync.
* **Spin margin** adapts to the measured timer lateness: 1.5 x average lateness + 50 us within [0.1, 0.5] ms. Measured
  lateness of the high-resolution timer ~0.28-0.30 ms (max ~1 ms) -> ~0.19 ms spin per frame.
* `NtSetTimerResolution` (GD3D11 raises it to the maximum): no measurable effect on the high-resolution timer (lateness
  0.296 vs 0.29 ms), so it is not used. Without high-resolution timers (before Windows 10 1803) the fallback timer
  raises `timeBeginPeriod(1)` and restores it in the destructor.

Accuracy (beach scene, 5 s windows after the world settled):

| target | before | after | interval p50 / p95 | release error |
|---|---|---|---|---|
| `max_fps` 60, vsync on (vblank-locked, 2 vblanks) | 43-47 fps (pacer slept 7-11 ms + blocked presents) | **60.0** (300-301 frames / 5 s) | 16.66 / 17.05 ms | 0.005-0.011 ms |
| `max_fps` 50, vsync off | - | **50.0** (250-251 frames / 5 s) | - | 0.006-0.016 ms |
| vsync 120 (vblank-locked) | 120 in menus | 120 in menus / loading; 80 in the world (section 2) | 12.5 ms in the world | - |
| uncapped (vsync off) | 79.5 | loading 184-210, world 80 (section 2) | - | - |

## 5. CPU: draw path

Per draw the device recorded every dynamic state (~22 `vkCmdSet*`), rebound vertex buffers and the dynamic vertex layout,
hashed the ~520-byte `PipelineDesc` byte by byte under a lock and looked up each used sampler (lock + hash). Temporary
section timers (removed again) per frame, 1441 draws: pipeline lookup 1.17 ms, vertex input 0.47, samplers + draw block
0.39, dynamic state 0.25, constants 0.17, begin + description 0.14, texture checks 0.10.

Now, per command buffer: dynamic state only when it differs from what was recorded (all pipelines declare the same
dynamic states, so it survives pipeline binds), vertex buffers / vertex layout only on change, the pipeline lookup skipped
when the description equals the previous draw's (and an 8-bytes-per-step hash otherwise), the bindless sampler index cached
per slot until its sampler state changes. Validation caught a first version that carried the cached colour-attachment
count into the next command buffer (blend state never set there): the cache is reset per command buffer, and every such
change needs a `-Validation` run.

| per frame, beach scene | before | after |
|---|---|---|
| draw calls (Device::draw*) | 3.6 ms (2.5 us per draw) | 2.7-2.9 ms (1.9 us per draw) |
| engine + HAL, rest | 3.0 ms | 3.0-4.5 ms (engine; noisy, see caveat) |
| CPU frame total | 6.6 ms | 6.0-7.5 ms |

Parity: `rvk_d3d9test` pixel checks, testdemo / rvk_shadows / rvk_charscreen PASS with `-Validation` clean, and frame
sequences of the beach intro (frames 1000-1900, mean luminance per frame) compared with the previous build. Those
sequences showed intermittent **overexposed frames during the intro cinematic** (letterboxed, frames ~1000-1800, mean
luminance 85-160 instead of 25-50) — also with the pre-session build `28ede14` (3 of 3 runs) and with synchronous
presents, so it is not caused by this pass. Probably the intro's wake-up effect; a frame-indexed D3D9 sequence would tell
(not possible yet: the D3D9 path has no frame-numbered screenshots). Open.

## 6. Pipeline creation stalls

A monolithic `vkCreateGraphicsPipelines` costs ~13 ms on AMD: **166 pipelines (2145 ms) when the world first appears**
(worst frame 1.66 s) and a few per material that comes into view (3 pipelines, 41 ms, in the first seconds of the hold).
AMD's `vkGetPipelineCacheData` returns an empty blob (52 bytes): the driver keeps its own cache, the `VkPipelineCache` file
does not help there (it is now saved every 10 s anyway: the game is killed rather than shut down; useful elsewhere).

* **Pipelines of earlier runs** (`<cache>/pipelines.keys`): every pipeline created for a draw is recorded by its shaders'
  hashes plus the rest of its description; on the next run a known pipeline is built on background threads (1-4) as
  soon as both shaders exist, which happens while the game loads. Second run: **8 pipelines (64 ms) at draw time
  instead of 166 (2145 ms), worst frame at world entry 288 ms instead of 1661 ms**, none during the hold (193 keys,
  ~100 KiB). The rest of the world-entry frame: 14 shader translations from the SPIR-V cache, 340 KiB texture uploads.
* **Graphics pipeline libraries** (`src/vk/pipeline_library.cpp`, `[vulkan] pipeline_libraries`, **off by default**):
  shader libraries compiled at shader creation, a fast link per new state combination, the link-time-optimised pipeline
  built in the background and swapped in. The AMD driver **crashes** (access violation inside its compiler) on 2 of the
  game's 2561 shaders compiled as fragment-shader libraries (corpus `ps30_7dd28aa4c2be47a5`, `ps30_72a979df27ac55e3`;
  the same SPIR-V is fine in monolithic pipelines; not caused by the Steam fossilize layer, the pipeline cache or the
  retain-LTO flag). `rvk_vktest --gpl-corpus <bin/rvk/cache/spirv>` compiles a directory as libraries and reports such
  crashes (SEH-guarded, test tool only). Enable it only on drivers that pass that check.

## 7. GPU

GPU time per world frame 0.9-1.2 ms uncontended (35 render passes). Passes of one frame (`-PerfFrame 2500`, measured
while another game shared the GPU, total 3.44 ms): main scene pass 1600x900 with 792 draws 1.65 ms, the first scene pass
(380 draws) 0.69 ms, three 1024x1024 shadow cascades 0.05 / 0.11 / 0.16 ms, the post chain (luminance 64x64 -> 1x1,
bloom 400x225 / 309x210, 800x450 blurs) ~0.5 ms, barriers / copies between passes 1-26 us each, the present blit 0.03 ms.
Not a limit at this resolution.

## 8. Before / after

| | before (2026-10-21 start) | after |
|---|---|---|
| world fps, vsync | 79.5 (external cadence) | 79.9 (external cadence) |
| world fps, `max_fps` 60 | 43-47 | 60.0 |
| CPU frame (engine + HAL) | 6.6 ms, + 5.8 ms blocked in present | 6.0-7.5 ms, presents off the render thread |
| draw calls | 3.6 ms / 1441 draws | 2.7-2.9 ms |
| GPU frame | 1.0-1.2 ms (3.5 ms under contention) | the same |
| pipelines built at world entry | 166 / 2145 ms, every run | 8 / 64 ms from the second run |
| D3D9 reference (same scene) | vsync 28-30 fps, vsync off 79.9 fps | - |

## 9. Remaining hotspots

1. ~~The external ~80 Hz present cadence~~: an AMD Chill profile on `Risen.exe` (section 2, fps-limit.md). Without it
   (renamed executable) the world runs at 175-240 fps, CPU-bound.
2. Engine CPU (~3-4.5 ms per frame, single-threaded) and our draw path (1.9 us per draw): the next candidates are
   `bindVertexInput` (declaration matching per input per draw, `Buffer::gpu()` lock per stream), the per-draw constant
   blocks (2 x 4.4 KiB of ring space per draw, ~11.5 MiB per frame; only the used registers are written), the 176-byte draw
   block per draw.
3. First-run pipeline stalls (until the keys file exists) and the 288 ms world-entry frame: pipeline libraries would solve
   the first run if the AMD crash goes away; a shipped keys file (from a full play-through) would also do.
4. Ring memory is device-local host-visible (ReBAR); 11.5 MiB of address space per frame is fine, the CPU writes are the
   constant registers actually used.

## 10. Larger object draw distances (2026-10-23)

Scene: a player savegame (forest cliff, hero at -17392 / 7652 / 14352; `run.ps1 -QuickSave <save>` copies it over the test
copy's quicksave, scenario `mods/rvk_perfsave` loads it and holds the view), player defaults (water, enhanced shadows, RT
reflections), `RisenTest.exe`, vsync off, `-Perf`.

| `lod.view` | fps | draws | CPU frame | of it draw calls | engine + HAL rest | GPU |
|---|---|---|---|---|---|---|
| 1x | 175 | 1105 | 5.4 ms | 2.2 ms | 3.2 ms | 3.0 ms |
| 2x | 103 | 2283 | 9.4 ms | 4.6 ms | 4.8 ms | 3.7 ms |
| 3x | 84 | 2630 | 11.5 ms | 5.5 ms | 6.0 ms | 3.7 ms |

CPU-bound throughout; the extra draws are mostly the main view's (2x: z pre-pass 323 -> 709, solid 374 -> 951; the
cascades 301 -> 519).

**CPU profile** (`run.ps1 -CpuProfile`, `[debug] cpu_profile`: `diag::frameSampler` samples the engine thread between
presents every 0.5 ms and logs a flat profile every 5 s: per module, self and inclusive per function; functions keyed by
their start in the unwind table, our DLL named from its PDB or symbolized offline with `llvm-symbolizer
--obj=build\RelWithDebInfo\d3d9.dll 0x180000000+<start>`; costs ~6 % fps). At 2x: d3d9 (ours) 42 % self, Engine.dll 22 %,
amdvlk64 13 % (the `vkCmd*` recording), ntdll 8 %, SharedBase 6 %. Inclusive: `drawIndexedPrimitive` 50 %, `prepareDraw`
38 %, `RtScene::observe` 13 % (the RT scene's per-draw bookkeeping), the engine's cascades 20 %, its game logic
(`eCProcessor::Process`) 9 %; `Buffer::gpu` 3.6 % self + its SRW lock.

Changes:
* `Buffer::gpu()` returns a clean static buffer's binding without the mutex / copy plan (`cleanStatic_`, cleared by a
  writing lock).
* `rt::Registry::age` caches each instance's world sphere (its rows are part of its key) instead of transforming its BLAS
  bounds every frame.
* Tried and dropped: observing the RT scene's draws on every 3rd frame (or the main view only): 128 / 121 fps, but the TLAS
  plateaued at ~1800 instead of ~2450 instances: the scene's learning depends on per-frame sightings.

Result at 2x: **103 -> 110 fps** (`-Perf`; 118 fps without it), CPU frame 9.4 -> 8.75 ms (draw calls 4.6 -> 4.25 ms), the
TLAS unchanged (~2450-2550 instances). The rest is spread over many ~1 % items; the next step is structural (fewer Vulkan
calls per draw, or recording off the engine thread).

## 11. Mesh arena (2026-10-23)

`vk::MeshArena` (`src/vk/mesh_arena.*`): the static vertex / index buffers' GPU copies are suballocated from 64 MiB
device-local buffers (VMA virtual blocks; above 8 MiB a dedicated buffer). `Buffer::gpu(strideHint)` places a new copy at a
multiple of its first use's vertex stride / index size; `bindVertexInput` binds a single per-vertex stream whose offset is a
multiple of its stride at offset 0 and adds the offset to the draw's `vertexOffset` (`vertexShift_`), `bindIndexBuffer`
folds the index offset into `firstIndex` and records the binding only when it changed (`ibCache_`). Consecutive meshes keep
their bindings; the arena is also the base for batched indirect draws (one binding per batch). With the RT tier the arena
carries the BLAS-input / device-address usages; `Buffer::deviceAddress` = the block's address + offset.

At the savegame (2x): the whole scene's static meshes take 69.5 MiB (1 block + 1 dedicated buffer); **110 -> 115 fps**
(draw calls 4.25 -> 4.0 ms); the TLAS unchanged; `-Validation` clean; rvkcompare against the stored reference: beach_sun,
jungle, harbour_docks, hero_closeup, gui_inventory, lake_jungle PASS (ssim >= 0.991). The replace query no longer copies the
engine vertex shader's `shared_ptr` per observer / replacer (`ReplaceQuery::engineVsRef` is a pointer: 1.3 % of the engine
thread in atomic reference counts).

Profile after it (2x, engine thread 9.1 ms): amdvlk64 14.8 % (pipeline binds, push constants, draws: ~985 pipeline binds per
2283 draws), `prepareDraw` 4.4 % self, the RT scene ~6 %, vertex input / constants / dynamic state / textures ~5 %, Engine.dll
+ SharedBase + Game.dll ~32 %.

Batching options measured on this machine (RX 7900 XTX, driver 26.10.1): `VK_EXT_device_generated_commands` is available
(push-constant tokens: the shaders stay as they are) but without pipeline switching inside a batch
(`supportedIndirectCommandsShaderStagesPipelineBinding` none); the engine's order gives runs of ~2.3 draws per pipeline, so
useful batches need the opaque passes' draws grouped by pipeline (order-independent under the depth test except coplanar
surfaces).

## 12. Batched opaque draws, device-generated commands (2026-10-23)

Every draw is computed into a `Device::DrawPacket` (pipeline, `VertexInputValues`, `DynamicStateValues`, push constants,
index binding, draw parameters; `prepareDraw` + the draw call) and recorded by `applyPacket` through the per-command-buffer
caches. **Batchable** packets: kinds StaticMesh / InstancedMesh / SpeedTree / Skinned in the main view's z pre-pass and
solid pass and the shadow views' depth pass, not replaced, not tinted, indexed, no blending, depth test with LESS /
LESS_EQUAL / EQUAL, no stencil. They wait in `batch_`; anything else that records into the frame's command buffer flushes
first (a draw that is not batchable, `endRendering`, `clear`), so the order between batched and other draws is kept.
`flushBatch` groups the batch by pipeline (stable: the engine's order within a pipeline, groups in the order their pipelines
first appeared). Only surfaces at exactly the same depth can show a different winner. `debug.batch_draws` (Debug tab, default
on) switches it off.

Tried and **removed** (commit after c60541f): runs of identical-state draws as `VK_EXT_device_generated_commands` executions
(push-constant + indexed-draw tokens, 68-byte sequences in the ring, per-slot preprocess buffers; the device has no pipeline
switching inside generated commands). It validated clean but measured no gain (table below), and merely enabling the
`deviceGeneratedCommands` feature (with `VK_KHR_maintenance5`) hid the ImGui overlay drawn into the swapchain image on AMD
26.10.1 (bisected with `scripts/capture-window.ps1`: `maintenance5` alone was fine, the DGC feature broke it even without any
indirect-commands layout or generated draw; validation clean). GD3D11 enables the same pair with a working overlay but
creates a Vulkan 1.4 device and vendors another ImGui backend: the difference was not investigated.

A/B at the savegame (2x, one 60 s hold, switches flipped at run time; `perf:` lines):

| mode | fps | CPU frame | pipeline binds |
|---|---|---|---|
| batching + generated draws (~1100 of ~1950 draws in ~80 executions) | 127-128 | 7.47-7.56 ms | ~410 |
| batching only | 126-128 | 7.51-7.61 ms | ~410 |
| neither | 122-123 | 7.79-7.86 ms | ~825 |

The grouping's gain is the halved pipeline binds (the AMD driver's most expensive per-draw call); recording a push constant +
indexed draw directly is already cheap, so generating them on the GPU saves nothing measurable. `-Validation` clean;
rvkcompare against the stored reference: 19 PASS, harbour_docks WARN (a wave-splash particle effect at the quay that varies
between runs; particles are not batched).

Totals at the savegame, 2x object draw distance, `-Perf`: **103 -> ~127 fps** (CPU frame 9.4 -> 7.5 ms) over sections 10-12.
What remains: Engine.dll + SharedBase ~4.5 ms (culling, render ops, material constants: the engine's, single-threaded), our
per-draw bookkeeping ~1.5 us per draw (`prepareDraw`: classification, replacers / observers, the RT scene's sightings, draw
records, constants and draw block).

## 13. Engine-side candidates (researched 2026-10-23, not built)

Code-only RE of Engine.dll (dumps `B:\DevDrive\risen-ghidra\enginecpu\`); savings estimated from the 2x profile, unmeasured.
Ranked: (1) drop far ops from the main view's z pre-pass list after `eCCameraBase::Update`'s Request (db+0x1520 / count
+0x1528, op key +0x10 = depth x 16777215 / zFar; SpeedTree via a pre-hook on `eCSpeedTreeRenderBuffer::AddToZPassRenderQueue`
0x6f82d0): 3-10 %, visual risk (the depth map feeds the shadow mask, SSAO / DoF, water); (2) a typed sort for the CRT
`qsort` 0x4a4aa0 (comparators: z 0x66e670 u32 at +0x10, solid 0x66e6a0 u64 at +0x10, alpha 0x66e600): 1.5-2 %; (3) throttle
`eCPrefetchAdmin::CacheEntities` 0x6969c0 (the cache-in sphere-tree walk, ~3 %, enlarged by `lod.view`; the profile's
"eCSpeedTreeRenderBuffer::operator=" is this non-exported code); (4) skip the no-op `SetTime` calls in
`eCShaderMaterialAdmin::Process` (~0.6 %); (5) stagger the far sun cascade / a chain-only `eCBeamTree::Cull` fast path. Facts:
one Extract + Request per view (the main view is not culled twice; each sun cascade walks the scene again); material
constants are already cached by the engine; `eCBeamTree` is the frustum test itself; the engine's `eCDX9Device` state cache is
bypassed with our vtables, so redundant render / sampler states (7 `SetSamplerState` per sampler on each material switch)
reach our HAL. Threading the engine's extract / request is high risk (shared per-resource state).
## 14. CPU overhead pass 1 (2026-10-24, e6b5dab)

Scene: savegame `Risen_Save_0005` (hero -17392 / 7652 / 14352), `run.ps1 -Mode vulkan -Exe RisenTest.exe -Vsync off -PlayerDefaults
-Setting 'lod.view=2','lod.trees=2','lod.meshes=2' -QuickSave <save> -Mods rvk_perfsave -Scenario rvk_perfsave -Param hold=60`.
Baseline (98c377e): plain 118 fps; `-Perf`: CPU 8.35 ms per frame (draw calls 3.56, rest 4.79), GPU 3.45 ms, 2283 draws (Main
DepthPrepass 709 / Solid 951 / Alpha 45; cascades 39 / 67 / 414). `-CpuProfile` modules: d3d9 (ours) 41 %, Engine.dll 26 %,
amdvlk64 10 %, ntdll 8 %, SharedBase 7 %, vcruntime140 5 %. Inclusive: `HalPrimitive::Execute` 44 %, `prepareDraw` 32 %,
`RtScene::observe` ~13 % (0.5-0.75 ms), `applyPacket` + `flushBatch` 8 %, the engine's cascades (`RenderShadowMaps`) 19.5 %,
`eCFrustumDatabase::Execute` 56 %, `eCProcessor::Process` 10 %.

**Profiler**: `diag: profile pc` lines (our module's leaf addresses in 16-byte buckets, top 120 per 5 s window);
`tools/pcprofile.py <rvk.log> [--dll] [--top] [--skip]` aggregates the windows and symbolizes with `llvm-symbolizer --inlines`
to functions / lines. Gotchas: line numbers are right only if the DLL matches the log's build; leaf code without unwind info is
bucketed per 4 KiB page in the old "self" list (names there are unreliable).

Changes:
* Draw path: a direct-mapped cache of recent pipeline descriptions in front of the pipeline map; `flushBatch` groups with one
  open-addressing table + one sort; shader switches by non-owning `ShaderRef`; vertex layout plans per (declaration, shader);
  per-shader sampler masks; draw packets hold interned vertex-input / dynamic-state blocks (pointers, not ~850 bytes inline).
* RT scene: cheaper BlasKey / instance key hashing, `IdHash` (`core/id_hash.h`) for u64-keyed maps, closed-form `worldSphere`,
  `blasByKey_` holds `Blas*`.
* **Record threads** (`debug.record_threads`, default on; `src/d3d9/record.cpp`): scopes of Main DepthPrepass / Solid and Shadow
  ShadowDepth only (`wantSecondaryScope`), decided per scope at `beginRendering` (a scope that started inline is restarted at the
  first eligible draw / clear). Chunk = 160 draw packets, recorded by two workers into secondary command buffers (pools per
  worker thread x frame slot, reset in `beginFrame`; the inheritance rendering info carries the scope's attachment formats). A
  flush of the batch = one chunk (batch limit 160 when threaded: the workers record while the engine still produces draws; before
  this the batch was flushed only at the scope's end and the engine thread idled: `NtWaitForAlertByThreadId` 3.6 % in the
  profile). Clears are `Op::Clear` packets (`vkCmdClearAttachments` inside the secondary); debug labels are scope items replayed
  in order in the primary between the `vkCmdExecuteCommands` calls. Workers spin ~4000 pauses before sleeping on the condition
  variable (the engine thread notifies only when a worker sleeps). Validation found: after `vkCmdExecuteCommands` the primary's
  pipeline, dynamic state and descriptor sets are undefined: `rec_.reset()` + rebinding the bindless set after
  `vkCmdEndRendering` (24 VUIDs "state was invalidated by vkCmdExecuteCommands" before the fix; clean with `-Validation
  -SyncValidation` after).
* **CacheEntities stride** (`debug.prefetch_stride`, 1..8, default 4; hook in `engine/lod.cpp`):
  `eCPrefetchAdmin::CacheEntities(bCVector const&, unsigned char slot, bool walkRange, bool walkCache)` 0x6969c0 (exported; this =
  scene admin + 0xe8; called by `eCApplication::OnPreRender` every frame with the camera position, slot 0, true, bVar3) has no
  camera-moved early-out. Its two bools gate the two tree walks (FUN 0x217570: slot 0 = prefetch range walk, slot + 1 = cache-in
  walk; callbacks fire only on per-node bit transitions, so a repeat call at the same position changes nothing); the dirty-list
  flush and the centre store always run. The hook calls the original with both bools false unless `stride` frames passed, the
  centre moved >= 100 units (1 m) since the last walk, or `lod.view` changed. Cost: entities at their cache-in sphere edge appear /
  vanish up to stride - 1 frames late (also the first frames after a savegame load). RE source (inferred, `ClearPrefetchCoherence`
  0x6963c0 body not verified live): `B:\DevDrive\risen-ghidra\viewdist\prefetch_dump.txt`, `enginecpu\d2.txt`.

Results (plain fps / `-Perf` CPU per frame / draw calls):

| step | plain fps | CPU frame | draw calls |
|---|---|---|---|
| baseline 98c377e | 118 | 8.35 ms | 3.56 ms |
| round 1 (pipeline cache, flushBatch table sort, ShaderRef, IdHash, worldSphere, RT keys) | 132.5-133.4 | 7.65 ms | 3.05 ms |
| + vertex plans, sampler masks, interning | ~133-136 | neutral on their own | |
| + record threads: on vs off | 138.5-139.8 vs 134.9-136.8 | | 2.7 vs 3.0 ms |
| + stride 4 vs stride 1 | 141-142.6 vs 138.9 | rest 4.29 vs 4.56 ms | |
| end state | ~142 (+20 %) | 7.05 ms | GPU 3.63 ms |

**Image parity** (record threads): `refcompare` against the stored reference (its main_lua / spass_dll hashes differ from the
calibration: stale): threads on 11 PASS / 7 WARN / 1 FAIL / 1 INVALID; off 15 / 3 / 1 / 1, a second off run 15 / 4 / 1 / 0. Vulkan
on vs off directly (`tools/refcompare.py compare A B`): 7 FAIL / 6 WARN; off vs off with identical settings: 6 FAIL / 7 WARN
(harbour_docks 5 % bad32, hero_closeup 4 %, jungle ssim 0.92, sea_cliff 2.8 %): run-to-run noise (splash particles, foliage wind,
eye adaptation) of the same size. Visual review of the on-vs-off differences (sea_cliff, hero_closeup, jungle, harbour_docks b0):
breaking-wave spray in one run only, wind-swayed leaves, a drifting soft ground shadow; no missing draws, no depth-order errors, no
shadow differences on static objects. Lesson: `refcompare.ps1 -Calibrate` first, else renderer changes and noise are not
separable at those spots.

**Remaining** (record-threads run, engine thread 7.0 ms): d3d9 ~35 %, engine 30 %, ntdll 12 % (incl. the `-Perf` QPC timers),
SharedBase 8 %, amdvlk64 6 %, vcruntime140 5 % (memmove). Ours: many ~1 % items (`prepareDraw`, `computeVertexInput`,
`Registry::see`, `RtScene::findBlas`, `RtScene::record`, `uploadConstants`: a 4.4 KiB ring block per draw = 18.6 MB per frame).
Engine: `eCFrustumDatabase` copy constructor 0.9 %, `qsort`, `eCBeamTree::Cull`, `OnQueryNodes`. Ideas not built: observe
sightings on a worker (needs RtScene thread safety; slow paths need engine-thread state), far-cascade small-caster culling
(cascade 3 = 414 of 518 cascade draws), throttling the far cascade (visual risk), a deferred render-thread Device API (large;
our enhancement code reads engine memory during draws: too risky).

**Ubershader replacement of the translated engine shaders: blocked, not built.** The port verifies (shader-permutations.md 7),
but the engine-shader-hash -> define list + material-graph snippet mapping cannot be inferred automatically (section 5:
`hlsl_infer` solves 6.7 %). It needs the node-string generators (`eCShaderDefault::GetDeclarations` 0x6c8650,
`eCColorSrc*::GetImplementation`) or a decode of `ShaderMaterialPool_Master.smp`.

### 14.1 Follow-ups (commit 88bc1a0)

* **Record threads, engine thread helps**: at a scope's end the engine thread records queued chunks itself (work stealing, own
  command pools) instead of waiting. A worker whose chunk was stolen must wait again: it used to treat the empty queue as "quit"
  and exit (found by reading, fixed). Record threads need >= 4 hardware threads (guard).
* `rt::Registry::see` skips the `owners_` write via `Instance::ownerViews`.
* **Observe cost**: `rt: scene:` stats now print "N instance sightings" per frame: ~1426 sightings for ~1273 accepted draws (2055
  observed, 782 skipped) at the savegame; observe 0.68 ms with `-Perf` timers (~0.33 us per observed draw). Instanced batches are
  not what makes observe expensive: it is the ~5 hash lookups per draw.
* **Tried and removed, no measurable gain**: copying only the vertex shader's used float constant ranges per draw (reflection
  bitset of read registers; relative-addressing shaders excluded; pixel shaders excluded because the GI / G-buffer variants read
  ambient registers the engine shader may not read). Plain 141.8-142.9 vs 141-142.6 fps, CPU 7.11 vs 7.05 ms.
* **Final numbers** (savegame, 2x object distance, plain): 141.9 / 142.9 fps with record threads, 135.5 without; `-Perf` CPU
  7.1-7.4 ms, GPU 3.6 ms. At the game's own distances (lod 1x): 227.6 vs 223.5 fps. Validation + synchronization validation clean;
  `rvk_shadertool corpus` 2483 / 2483. Baseline of the whole pass: 118 fps, so +20 % at 2x.
* **Profile of that state** (`-Perf -CpuProfile`, engine thread): d3d9 37.8 %, engine 29.0 %, ntdll 10.7 % (4 % of it the `-Perf` QPC
  timers, 2.4 % waiting for workers before stealing), SharedBase 6.8 %, amdvlk64 6.8 %, vcruntime140 5.0 %. Inclusive:
  `eCFrustumDatabase::Execute` 53 % (`HalPrimitive::Execute` 41.8 %, `prepareDraw` 32.4 %, `RtScene::observe` 11.5 % incl. timers),
  camera `Update` (cascades) 24 %, `eCProcessor::Process` 11.7 %, inject 7 %.
* **Not worth building**: (a) replacing the z pre-pass by a GPU-driven pass: the solid pass tests depth EQUAL against the engine's
  z pre-pass, so it needs bit-identical vertex math (a different shader permutation rounds differently); (b) a deferred
  render-thread Device API (what DXVK's CS thread does): dynamic vertex buffers are copied at draw time and the enhancements read
  live engine objects, so silent corruption is a risk for ~1.3 ms.

## 15. CPU overhead pass 2 (2026-10-24, 12579b2)

Scene and recipe as in section 14 (savegame, lod 2x, vsync off, `-PlayerDefaults`).

* **Sun cascade update stride** (`debug.shadow_stride`, 1..4, default 2; `engine/shadow_maps.cpp`): the `eCCascadeShadowMap::RenderShadowMaps`
  pre-hook skips the call on alternate frames while the camera is nearly still. Conditions for a skip (all): the camera moved
  < 1.5 m and its view-projection rows 0..2 changed < 0.015 (~1 deg), measured against the previous frame's camera block
  (`RenderShadowMaps` runs BEFORE the main view, so the current camera is not read yet); sun direction changed < ~0.1 deg; cascade
  range / split factor / map size unchanged; gap since the last update < 0.25 s. The cascade maps and matrices stay in the cascade
  object; a skipped frame credits the cascade count (split count at +0x28) through `Device::creditShadowDepthMaps`, so the mask pass
  of `enh/shadows` expects the right number of depth maps. rvk.log: `engine: shadow cascades: N updates, M skipped so far`.
  Measured: `-Perf` fps 139 -> ~150 (CPU 6.35 ms per frame at stride 2); frames alternate 2285 / 1765 draws. Validation +
  synchronization validation clean; `shadow-compare` images (static rvkcompare camera) equal within noise (the only larger
  difference was animated water). Moving-camera quality was not A/B'd separately (the force conditions cover it).
* **Occlusion probe** (a HZB of the z pre-pass + a per-sphere test over the RT registry's instances; code removed, see 15.1): ~24 % of the
  instances (21-25 % of the triangles) are hidden at lod 2x, ~8 % at lod 1x, 10-16 % are off-screen. Realizable gain ~0.5 ms at 2x /
  ~0.1 ms at 1x, with pop-in risk and a 2-frame lag: not built.
* **Small-caster cull** (histogram in cm bins <5 <10 <20 <40 <80 <160 >=160): almost nothing under 20 cm; ~1430 instances of
  20-40 cm, seen by both the main view and the cascades: not worth culling.
* **Render-thread split audit: rejected** (weeks of work, silent-corruption risk from live engine pointers, <= 1.3 ms net; see 14.1).
* **Per-pass injected CPU** (`-Perf` line `injected pass CPU`): `rt.scene` at AfterDepthPrepass 0.20 ms, `water.ocean` 0.16 ms,
  the others negligible.
* **Remaining candidates**: `rt.scene` incremental records (<= 0.15 ms), a typed `qsort` replacement (2 %), the
  `eCFrustumDatabase` copy constructor (0.9 %), a no-op `SetTime` (0.6 %).

### 15.1 Tried and removed (do not redo without a new idea)

| Idea | Result | Why dropped |
|---|---|---|
| GPU occlusion probe (HZB of the z pre-pass, sphere test of the RT registry's instances; `debug.occlusion_probe`) | 24 % of instances hidden at 2x, 8 % at 1x | realizable ~0.5 ms / ~0.1 ms, pop-in risk, 2-frame lag; code deleted (in git history, commit 12579b2) |
| Small-caster cull of the cascades | almost no instances under 20 cm radius | nothing to cull |
| Render-thread split (engine thread / render thread) | <= 1.3 ms net | weeks of work, silent-corruption risk from live engine pointers read in `prepareDraw` and the observers |
| Per-draw vertex-constant range trimming | no gain | reverted (14) |
| Device-generated commands for the opaque passes | no gain | removed (12) |
| Record threads off vs on | on wins 5-6 fps at 2x | kept on |
| GLSL material shaders (`debug.ubershader`) | equal image and GPU time, +0.05-0.1 ms CPU | kept as a base for visual improvements, default off |

## 16. Full render mode (2026-10-28)

`renderer.mode = full` (the native frame composes ~99.98 % of the draws from engine data while the engine's render code still runs,
engine-frame/README.md 17) costs more CPU than Wrapper; GPU time is the same. Where it goes, measured with `-CpuProfile` and one
group per run: engine-frame/opt-profile.md; the plan (O1 our own costs, O2 the lean executor without the engine's render code, O3
effects / GUI) and each stage's result: engine-frame/opt-plan.md. After O1 (2026-10-28) at Risen_Save_0009 (uncapped, unprofiled):
Wrapper 3.33 ms, Full 5.48 ms (was 8.53). Switches for A/B: `native.mirror` (0 our register file, 1 / 2 the engine's pools re-read per
view / per op), `native.coverage` (the per-draw coverage counter in plain Full runs). Recipe: `out/o1/batch.ps1` (wrapper / full,
with and without `-CpuProfile`), `out/o1/table.py` (`RVK_DLL` = the DLL of the profiled run for symbols).

O2a (2026-10-28, opt-plan.md "O2a result"): `native.lean` (Debug, default off) skips the engine's slot code of the composed mesh-like
ops (static, alpha / refracted meshes, decals, sky, billboards, sky cube; ~150 slots per frame at the savegame) and runs their side
effects itself; `native.lean_check=N` runs the engine every N-th frame (+ a warm-up frame) for the compare. Measured at the savegame:
no unprofiled gain (Full 5.69 - 5.77 ms lean off vs 5.74 - 5.76 on; profiled Execute -0.16 ms): those slots are cheap, the engine's
double work is in the SpeedTree / vegetation / shadow-view slots and the effect units (O2b / O2c / O3). The machine measured ~0.3 ms
slower in that session than at O1 (the O1 DLL: 5.48 at O1, 5.82 in the O2a session): compare runs of one session only. Recipe:
`out/o2a/batch.ps1`, `out/o2a/table.py`.

O2b (2026-10-28, opt-plan.md "O2b result"): `native.lean` now also skips the actors', SpeedTrees', tree billboards', vegetation's,
particles', strips', light streaks' and rain's slots and every shadow-view z op (~470 slots per frame at the savegame) and is **on by
default in Full** (off with a compare unless `native.lean_check` is set). At the savegame (same session, 3 runs each): Wrapper 3.39 ms,
Full lean off 5.66, lean on **4.97** (fps 169 -> 192); jungle spot 3.63 / 6.92 / **5.77**. Of the gain: the engine's slot code
(Engine.dll self 1.17 -> 0.74 ms profiled), a device sampler cache that the per-op restore's state flip-flop defeated (now a two-entry
memo per stage; `native.lean_restore=false` = the restore's A/B: +0.09 ms left), the frame tap's per-op `std::format` notes on lean
frames (now formatted only while kept). Recipe: `out/o2b/batch.ps1` (`name|mode|settings|save or jungle`), `out/o2b/table.py`.

O2c / O3 (2026-10-28, opt-plan.md "O2c / O3 result"): on lean frames the effect filters, sprite flushes and the PSSM loop skip the
engine's code too (`native.lean_effects`, default on: their A/B), missing permutations are compiled at the slot's place (coverage
leftovers ~15 draws per run). Same session, 3 runs each: savegame Wrapper 3.36 ms, Full before (O2b DLL) 4.85, after **4.76** (fps 196 ->
200); jungle 3.71 / 5.79 / **5.69**. Profiled, Full 5.06 vs Wrapper 3.55: our DLL +1.48, memcpy +0.28, Engine.dll -0.27 (less than in
Wrapper): the rest of Full - Wrapper is our own composition / recording per draw. `native.lean_restore=false` saves 0.07 ms (kept).
Recipe: `out/o3/perf.ps1` (`batch.ps1` entries `name|mode|settings|save or jungle|config`, config `Before` = a DLL in `build\Before`).
