# Performance and frame pacing (M5)

> Note (2026-11-05): the settings `debug.constant_compare`, `debug.fast_absorb`, `debug.fast_sort`, `debug.fs_read_cache`, `debug.ocean_spd`, `debug.sky_cube_interval`, `debug.sound_compressed`, `debug.split_constants`, `debug.xinput_interval`, `lighting.table_complete`, `lighting.tree_merge`, `native.cpu_opts`, `native.lean_effects`, `native.lean_restore`, `native.mirror`, `native.retained` named below were removed (removed 2026-11-05, now a constant in code or an always-on behaviour: see `docs/research/settings-audit.md`); read them as the history / the values the code now fixes, not as switches to set.

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

`pacing::FramePacer` (`src/core/pacer.*`), the waiting adapted from GD3D11's `fpslimiter.h` (same author): QPC time base, a
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
  2026-11-05 (docs/research/dxvk.md 2): driver 26.10.1 passes all 2561 modules; the recipe is now DXVK's (no retained
  link-time info, no pipeline cache, optimised = monolithic build; `RVK_GPL_RECIPE=lto` = the former): corpus libraries 3.6 s
  instead of 135 s, cold world entry 166 pipelines linked in 17 ms vs 163 built in 1940 ms without libraries; still off by
  default (`run.ps1 -PipelineLibraries`).

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

## 17. Vulkanize V1 (2026-10-28)

Cheap fixes, details and numbers in vulkanize/plan.md section 5. Ring blocks sized to what a dynamic buffer's frame copy
holds (Full: 36 -> 8 MiB per frame, GPU allocations -96 MiB at the savegame, -128 MiB at the jungle spot), no int / bool part in
the stage constants of shaders that read none, `debug.constant_compare` (register writes that change nothing keep the block),
`perf: stage constants` line (blocks per stage, KiB written, int / bool skipped, unchanged writes), optional
shaderDrawParameters + drawIndirectCount. CPU effect small (Full 4.70 -> 4.66 ms savegame, 5.88 -> 5.60 jungle).

## 18. Vulkanize V2 (2026-10-28)

Split constants, details and numbers in vulkanize/plan.md section 6. A stage's float registers are four windows by register
number (VS view / frame / object / material, PS view+fog / c5 / material / light); a window block goes to the ring only when
dirty (`debug.split_constants`, default on; `debug.split_verify` checks the blocks against the register file), `perf: split
constants` line. Ring traffic -60 to -65 % (Wrapper savegame 7,117 -> 2,789 KiB per frame, Full 8,126 -> 2,773), constant bytes
written -33 % (Full 2,607 -> 1,728 KiB); GPU memory and GPU time unchanged. CPU no gain (Wrapper savegame 3.31 -> 3.33 ms,
Full 4.67 -> 4.74 ms, +1.5 %: the always-on compare + window bookkeeping cancels the smaller copies): the CPU is in the per-draw
composition (V3).

## 19. V3 retained composition: profile (2026-10-28)

V3 (`native.retained`, default off) built and parity-proven but without CPU gain; the sampled profile (`-CpuProfile`, savegame
`Risen_Save_0009` hold 40, Full, `tools/pcprofile.py`) shows the retainable part (`materialDraws`) is ~6.4 % = ~0.33 ms of the
4.8 - 5.2 ms engine thread. Numbers, remaining time and the decision: vulkanize/plan.md 7.1.

## 20. Vulkanize V4: the per-draw device path (2026-10-29)

Flat-profile attack on the device path, both modes (numbers: vulkanize/plan.md 8). The sampler learned to name the callers of the
CRT's memcpy / memcmp and to attribute stacks to our own source lines inclusive of what they call (`diag: profile site / site2 /
crt`, `tools/pcsites.py`; 1 % of the thread = ~0.05 ms). What the profile showed: `vk::PipelineDesc pd{}` (member-wise init of 512
bytes) 1.4 % Full / 2.1 % Wrapper, CRT calls in the setters / `internVi` / `internDyn` / `prepareDraw` compares 3 - 4 %, allocation
from the effect passes ~2 % in Full, hash-map lookups of the composition 0.9 %, the 64-bit division of the stream shift 0.4 %.
Fixed: blank-description copy, 52-byte description compare (EDS3 + dynamic vertex input), inline SSE2 compares (`d3d9/memeq.h`),
changed-span register writes, 32-bit division, phase-generation early exit, pointer memos, lazy snapshot hash, effect-pass
buffers reused, LOD diagnostics without per-call allocation. Result: Full 5.03 -> 4.95 ms, Wrapper 3.45 -> 3.38 ms savegame, jungle
-0.13 ms in both (~2 %); vcruntime140 share Full 9.1 -> 7.5 %, Wrapper 5.4 -> 3.1 %. No item above ~1 % of the thread is left in
the device path; merged indirect draws would not touch the engine thread (plan.md 8.4). Remaining engine-thread time is the
engine itself (Engine.dll 18 % / 33 %, sharedbase 8 / 11 %), SEH-guarded engine reads (3 %) and the driver's inline `vkCmd*` (4.5 %).

## 21. CPU pass 3: fresh profile of both backends, XInput, typed op sort, D3D12 pipeline hash (2026-11-04)

Scene and recipe as in 14 (perf savegame `out/fg/perf.save`, lod 2x, `-Exe RisenTest.exe -Vsync off -PlayerDefaults -Perf [-CpuProfile]`, scenario
`rvk_perfsave`, hold 60; D3D12 = `-Api d3d12`, Full on D3D12 = `-Mode wrapper -Setting renderer.mode=full`). Scripts `out/cpu/run1.ps1` /
`batch*.ps1` (git-ignored); `tools/profsum.py` averages the named `diag: profile module|self|incl` lines over the steady windows (ms per frame =
share x the frame interval) and symbolizes our unnamed functions (`--dll`). The profiler and `-Perf`'s timers cost ~0.4 ms
(`RtlQueryPerformanceCounter` 0.18 + `steady_clock::now` 0.22 + `msvcp140 _Query_perf_counter` 0.2 are measurement artefacts, not in normal play).

### 21.1 Profile (build 13e14e6 + profiler, 9 windows of 5 s, ms per frame of the engine thread)

| | Vulkan wrapper | D3D12 wrapper | Vulkan full | D3D12 full |
|---|---|---|---|---|
| frame interval (profiled) | 5.55 | 5.66 | 8.09 | 8.24 |
| d3d9.dll (ours) | 1.98 | 1.97 | 4.37 | 4.40 |
| Engine.dll | 1.57 | 1.50 | 1.03 | 1.04 |
| ntdll | 0.65 | 0.68 | 0.65 | 0.63 |
| driver: amdvlk64 / amdxc64 + d3d12core | 0.48 | 0.53 + 0.16 | 0.58 | 0.61 + 0.17 |
| sharedbase.dll | 0.42 | 0.41 | 0.42 | 0.40 |
| vcruntime140 (memcpy / memmove) | 0.21 | 0.19 | 0.76 | 0.68 |

Inclusive, ranked (wrapper, Vulkan / D3D12; Full differs where noted):

| rank | item | ms (VK / D3D12) | note |
|---|---|---|---|
| 1 | `eCFrustumDatabase::Execute` (main view) | 3.15 / 3.25 | contains the HAL: `eCGfxVisual::Execute` 2.16 / 2.19 (engine draws incl. ours); Full: `StaticOps::opEnter` 4.6 |
| 2 | our draw path `Device::drawIndexedPrimitive` | 2.06 / 2.10 | `prepareDraw` 1.51 / 1.63 (D3D12 +0.12: the pipeline hash, fixed below), `computeVertexInput` 0.26, `splitStage` 0.07 self |
| 3 | `eCCameraBase::Update` (sun cascades: `RenderShadowMaps` 0.64) | 0.95 / 1.00 | `ExecuteShadowMap` 0.34; SpeedTree leaves / fronds / branches in the main view 0.43 + 0.23 + 0.21 |
| 4 | `eCProcessor::Process` (game logic, illumination 0.18, sky cube 0.16) | 0.72 / 0.71 | engine |
| 5 | injected passes (`Device::inject`) | 0.39 / 0.59 | ocean 0.20 / 0.34 (64 mip blits), `CopyCurrentRTToImage` 0.26 / 0.42 |
| 6 | `RtScene::observe` | 0.35 / 0.34 | |
| 7 | frustum extract / request: `OnQueryNodes` 0.30, `Request` 0.22, `ExtractFromFrustum` 0.19, `RequestRenderOps` 0.25, `CullCamera` 0.16 | ~1.0 | `eCBeamTree::Cull` self 0.13 - 0.16 + `bCPlane::ClassifyPoint` 0.04 + `bCSphere::Contains` 0.02 = ~0.22 (4 %) |
| 8 | **XInput**: `eCInputAdmin::UpdateX360` -> `XInputGetState` (through the Steam overlay's hook) | **0.13 / 0.16** | a disconnected pad = a driver call (`NtDeviceIoControlFile`) per index per frame: **2.5 - 3 %**; fixed below |
| 9 | the engine's CRT `qsort` of the op lists | 0.038 | measured by an instrumented detour (the "2 %" estimate in 13 was too high) |
| 10 | record threads: `NtWaitForAlertByThreadId` 0.06 + `NtAlertThreadByThreadId` 0.05 + critical sections 0.05 | ~0.16 | already minimised (14.1) |
| 11 | `eCFrustumDatabase` copy ctor 0.01, `SetTime` loop 0.02, `SetCurrentRenderStates` 0.03 | < 0.06 | each < 0.6 %: not built |

D3D12-only (wrapper, vs Vulkan): the `std::unordered_map<PipelineDesc>` hash (`DescHash`, byte-wise FNV over 520 bytes) 0.12 - 0.17 ms (D3D12 has
no EDS3 / dynamic vertex input, so the 52-byte recent-pipeline compare of `findPipeline` misses more and the full map lookup runs; also behind
`blitDraw`), `D3DCommandList::transitionRange` 0.17 + `blitTexture` 0.21 (the blit is a draw: an SRV and an RTV created per blit, two barrier calls
around it; the ocean's mip chains are 64 of them per frame), `d3d12core` + `amdxc64` 0.69 vs `amdvlk64` 0.48 (runtime + driver +0.21). Injected
passes 0.59 vs 0.39 (the blits again).

### 21.2 Changes (each behind a Debug setting, A/B'd; savegame, wrapper, 14 windows per run, 3 interleaved runs per arm, `-Perf` CPU = engine + HAL)

| change | setting | result |
|---|---|---|
| **XInput throttle** (`engine/xinput_throttle.*`): after `ERROR_DEVICE_NOT_CONNECTED` the index is not asked again for 1 s (hooks `XInputGetState` / `XInputSetState` of xinput1_4 once it is loaded; a connected pad is polled every frame) | `debug.xinput_interval` (ms, default 1000, 0 = off) | CPU 4.707 -> 4.618 ms (-0.09, -1.9 %) alone; the profile lines are gone (0.13 / 0.16 ms -> 0). Not tested with a connected pad (none here): by construction only a "not connected" answer is cached |
| **typed op sort** (`engine/fast_sort.*`): the statically linked CRT `qsort` (0x4a4aa0) for the z / solid / alpha op lists (24-byte records; comparators 0x66e670 / 0x66e6a0 / 0x66e600) replaced by the same algorithm (read from the disassembly: median of three, cut-off 8, same partition and stack order) with inlined comparators; other callers go to the original | `debug.fast_sort` (1 default, 0 off, 2 = also run the engine's qsort on a copy and compare: rvk.log `fast sort: N lists compared ..., 0 differ`) | **bit-identical** (unit test against the UCRT's qsort, equal keys included; in game mode 2 in Wrapper and Full with the static / scene ops compares and `lean_check=10`: 0 differences, `mismatches: none`). Sort cost 7.3 -> 2.8 ms per second (8.0 -> 3.0 us per list, ~920 lists / 122,000 ops per second) = **0.038 -> 0.015 ms per frame (-0.5 %)**, below the A/B noise (A/B CPU +0.035 and -0.03 ms in two pairs). Kept: free and exact. The frame tap's qsort prologue check accepts the hook |
| **D3D12 `PipelineDesc` hash**, 8 bytes per step instead of byte-wise FNV (`d3d12_game_priv.inc` `DescHash`) | none (pure) | D3D12 CPU 5.026 -> 4.812 ms (-0.21, -4.3 %; about -0.09 of it the hash, the rest XInput + sort) |

**D3D12 vs Vulkan** (wrapper, CPU per frame, 3 runs each): before 5.026 (5.016 / 5.043 / 5.018) vs 4.707 (4.749 / 4.699 / 4.672) = **+6.8 %**;
after 4.812 (4.818 / 4.827 / 4.792) vs 4.588 (4.594 / 4.596 / 4.574) = **+4.9 %** (goal <= +5 %). The remaining gap is the injected passes
(0.51 vs 0.35: the ocean's mip blits) and the D3D12 runtime / driver (+0.2). Plain interval p50 (Vulkan) 5.21 -> 5.05 ms with both Vulkan
switches on (-0.16 ms, -3 %).

### 21.3 Evaluated, not built

* **Frustum cull replacement (SIMD / batched `eCBeamTree::Cull`)**: the sphere / box cull is a pointer walk of an occluder beam tree
  (`bCPlane::ClassifyPoint` through SharedBase per node, one explicit stack), not a batch of independent plane tests; all of cull is ~0.22 ms
  (4 %, under the 5 % bar). A bit-identical inline detour would save the ClassifyPoint calls and the stack cookie only (~0.05 ms, 1 %) and must
  reproduce SharedBase's float formula exactly. The walk repeats per sun cascade (`ExecuteShadowMap` 0.34). Not worth the risk.
* **Off-thread resource work on our side**: in the steady state texture uploads are 0 KiB per frame and 0 pipelines / shaders are built per
  interval (the 5 s `perf:` lines); in the first world frames 8 pipelines (115 ms, asynchronous, the draw is skipped meanwhile) and 330 KiB of
  textures: nothing is left on the engine thread to move to the workers. The frame-time spikes of a still scene (p95 6.2 ms, an occasional 17 -
  27 ms max per 5 s) and the loading screens (680 / 617 ms frames) are the engine's own streaming / loading on the presenting thread (engine
  surgery, out of scope).
* **Drop far ops from the z pre-pass (13 (1))**: not built: no profile share above 3 % to win (the z-list sort and the z draws are inside rank 1)
  and the depth map feeds the shadow mask, SSAO, DoF and water.
* **SetTime skip, `eCFrustumDatabase` copy, redundant render / sampler state filter in the HAL**: each < 0.6 % of the thread (0.01 - 0.03 ms);
  `setSamplerState` / `setRenderState` self time is 0.26 % + 0.17 %: a HAL state cache would have to stay bit-exact for ~0.02 ms.
* **Compute mip generation / batched barriers for `blitDraw` on D3D12**: the largest D3D12-only item left (~0.1 ms of the 0.2 gap): needs a
  compute mip-chain pass in `rhi::CommandList` (or batching the per-blit barrier pairs); not done in this pass.

### 21.4 Gates and Full mode (2026-11-04)

Full mode (Vulkan, savegame, 2 interleaved runs each, all new switches on vs the 13e14e6 DLL): CPU per frame 6.910 / 6.911 -> 6.729 / 6.821 ms
(-0.14 ms, -2.0 %; p50 interval 7.61 / 7.57 -> 7.41 / 7.50). Gates: `rvk_tests` 215 / 215, `rvk_d3d9test --validation` (Vulkan and D3D12) 0 failures /
0 validation errors, `rvk_rhitest --validation` PASS, `testdemo` 7 / 7 PASS with `-Validation` on Vulkan wrapper + full and D3D12 wrapper + full (0 validation
lines in rvk.log), Full compare run (`native.static_ops_compare`, `scene_ops_compare`, `lean_check=10`, `debug.fast_sort=2`) `mismatches: none`, 0 sort
differences. `refcompare` vs `out/refcompare/20261103-130010`: Vulkan 11 PASS / 6 WARN / 3 FAIL (`harbour_docks`, `sea_cliff`, `harbour_mouth` = the known
sea-edge noise), D3D12 12 / 6 / 2 (`sea_cliff`, `harbour_mouth`); no new FAIL (`out/refcompare/20261104-021314` Vulkan, `20261104-021937` D3D12).

## 22. Ocean mip chains by compute (2026-11-04)

The FFT ocean's displacement + derivative images (2 per cascade, 4 cascades at High: rgba16f, 128 / 256 / 512 squared, 8 - 10 levels) got their mips
from one blit per level (64 blits per frame on the savegame's quality, each an SRV + RTV and two barrier calls on D3D12). Now
`shaders/water/ocean_mips.comp.hlsl` (structure of AMD FidelityFX SPD, MIT: a workgroup reduces a tile through groupshared memory, several levels per
dispatch; attribution in the file header) writes them: per image **dispatch A** mip 0 -> 1..6 (one group of 16 x 16 threads per 64 x 64 texels, 16 KiB
groupshared) and **dispatch B** mip 6 -> 7..9 (one group; the source is at most 8 x 8), one `computeBarrier` between all A and all B dispatches and
no per-level layout transitions (every level in GENERAL, then one transition to SHADER_READ_ONLY). Not SPD's single dispatch: the last-group stage
needs a globallycoherent UAV + an atomic counter, and the root signature's storage images (u0..u7 space1) hold a source + 6 destination levels per
dispatch anyway, so two dispatches without cross-group reads are the simple, portable form (no globallycoherent in the HLSL / DXIL path, no counter
reset). Filter: the 2 x 2 box = what the bilinear blit of a power-of-two level computes (all sizes are 128 / 256 / 512); intermediate levels stay float
inside a group (the blit rounds to half per level: difference <= 1e-3, one half ulp). Switch `debug.ocean_spd` (Debug, default on; off = the blits,
kept for A/B; also the fallback when the GPU lacks 16 KiB of shared memory).

**Gates.** `rvk_rhitest` test "ocean mip chain by compute" (128 and 512, random + smooth field, vs a CPU box chain): worst difference 9.5e-4 (half
rounding) on both backends, **Vulkan vs D3D12 bit-identical** (difference 0 over 350,000 values); `dxil-check embedded` and the pipelines test
include the new shader; `rvk_tests` 215 / 215, `rvk_vktest --passes` PASS. Pictures (`water-compare`, `water.ocean=fft`, spd off left vs on right,
sea_cliff / sea_horizon / harbour_mouth / beach_sun, `tools` diff = mean absolute difference per pixel, 0..255): SPD vs blit on Vulkan 2.0 / 0.11 / 0.25
/ 2.9, on D3D12 3.3 / 0.17 / 0.93 / 3.9 (beach_sun / sea_horizon / harbour_mouth / sea_cliff), against the **same-setting noise** (spd on vs on) 2.6 /
0.31 / 0.84 / 2.2 (Vulkan run 3.3 / 0.08 / 0.22 / 4.2): the surf's splash particles and eye adaptation dominate; the waves are identical to the eye.

**Numbers** (savegame recipe of 14 / 21, wrapper, lod 2x, `-Perf`, 3 interleaved runs per arm; CPU = engine + HAL per frame, mean of the last 10
windows; one off-arm run per backend was a 6.6 / 7.7 ms outlier (other load), shown in the means, medians of the other two in brackets):

| | Vulkan off (blits) | Vulkan on | D3D12 off | D3D12 on |
|---|---|---|---|---|
| CPU ms per frame | 5.284 (4.652 / 6.614 / 4.585; ex outlier 4.62) | **4.506** (4.450 / 4.625 / 4.442) | 5.784 (4.823 / 4.820 / 7.709; ex 4.82) | **4.556** (4.566 / 4.544 / 4.557) |
| `water.ocean` pass CPU | 0.215 | **0.048** | 0.355 | **0.099** |
| all injected passes CPU | 0.318 | 0.144 | 0.467 | 0.194 |
| `pass water.ocean` GPU (`-PerfFrame 4000`, 24 frames, 2 runs) | 0.421 / 0.419 | **0.318 / 0.286** | 0.557 / 0.590 | **0.247 / 0.366** |

CPU: -0.12 ms (-2.5 %) on Vulkan, -0.26 ms (-5.5 %) on D3D12 against the clean off runs; the ocean's CPU 0.215 -> 0.048 / 0.355 -> 0.099. GPU: the
chain is 0.1 ms (Vulkan) to 0.2+ ms (D3D12) cheaper (fewer barriers, 16 dispatches instead of 64 blits). **D3D12 vs Vulkan** (SPD on): 4.556 vs 4.506 =
**+1.1 %** (was +4.9 %, 4.812 vs 4.588).

Validation / regression gates (SPD on): `testdemo` 7 / 7 PASS with `-Validation` on Vulkan wrapper + full (also sync validation) and D3D12 wrapper + full, 0
validation lines in rvk.log; `water-compare` (sea_cliff / sea_horizon / beach_sun, `water.ocean=fft`, `water.ocean_quality=high`) with `-Validation` (+ sync
validation on Vulkan) 0 validation lines on both backends (rvk.log `mip chains by compute`); `refcompare` vs `out/refcompare/20261103-130010`: Vulkan 11 PASS /
7 WARN / 2 FAIL (`harbour_mouth`, `sea_cliff`), D3D12 13 / 5 / 2 (same two): no new FAIL. Gotcha: `-Setting` bools are lowercase `true` / `false`
(`True` / `False` from PowerShell are not parsed: an A/B arm silently stayed on). Rejected: a true single-dispatch SPD with a global counter (no
globallycoherent in our HLSL / DXIL path, 8 storage image slots); not needed, the two dispatches already remove the per-level cost.

## 23. Streaming and loading screens (2026-11-04; details docs/research/streaming.md)

The frame-time spikes of a still scene and the loading screens (21.3: "the engine's own streaming on the presenting thread") were profiled with
the new spike profile (`-CpuProfile`: samples more than `RVK_SPIKE_MS` = 8 ms into a frame, `diag: spike` rows, `tools/spikesum.py`).

* **Still scene**: frames over 8 ms are rare (64 ms of time beyond 8 ms in 90 s); 29 % of them is `FMOD::System::createSound` (a sound that
  left the cache plays again: `gCEffectCommandPlaySound` -> `eCSoundResource2::Play` -> direct `QueueData` -> decode, 1 - 18 ms), the rest our
  first draws / the engine's render. p95 vs p50 (5.9 vs 5.4 ms) is plain frame variance. **Walking** (55 s): 18 ms beyond 8 ms in total, no
  resource load in any stack. Resource file IO of the lazy queue is already asynchronous (FFC async read + completion queue); only the
  parse (`OnLoadData`) is on the engine thread and not provably thread-safe (streaming.md 4).
* **Loading screens**: 40 % of the engine thread was `NtReadFile` / `SetFilePointerEx` (FileSystem2's packed stream: two system calls per
  Read, 6 million Reads per load) and 22 % `eCSkydome_PS::CreateAbsorbationImage` (128 divisions per pixel, 2 x 280 ms per load).
* **Built** (`engine/load_accel.*`): `debug.fs_read_cache` (KiB, default 64; read-ahead of packed streams; 6.48 M buffered reads verified
  against the engine's own reads, 0 differ), `debug.fast_absorb` (default 1; sliding windows, bit-identical, `=2` compares in-game: 0 rows
  differ), `debug.sound_compressed` (default off; sounds as compressed FMOD samples: 320 -> 12.5 ms of `createSound` per 257 sounds; audio
  unchecked), `debug.stream_stats` (1 = timing + `engine: stream:` lines, 2 = verify the buffer). **Quick load 4.13 -> 2.78 s (-33 %),
  new game 6.2 -> 3.9 s (-37 %)**; still-scene CPU unchanged.

## 24. Full mode CPU: where the gap to Wrapper is, first batch (2026-11-04)

Goal (user): `-Mode full` as the main mode, Full - Wrapper <= ~0.5 ms. Recipe as in 21 (savegame `out/fg/perf.save`, lod 2x, `-Perf`; profiled with `-CpuProfile`,
`tools/profsum.py`, `tools/pcsites.py --callers`, `tools/pcprofile.py`; scripts `out/cpx/run1.ps1` / `ab.py`, git-ignored). Profile of main cb5097a (1 % = 0.078 ms;
Full 7.82 ms interval profiled, Wrapper 5.38; 1,290 draws per frame in both):

| item (inclusive, ms per frame) | Wrapper | Full | note |
|---|---|---|---|
| `eCFrustumDatabase::Execute` main view | 3.18 | 4.79 | +1.6 |
| of it the device draw path (`drawIndexedPrimitive`, `prepareDraw` 1.54 / 1.64) | 2.07 | 2.19 | **parity**: the same code, the same draws |
| engine slot code (Wrapper) vs our composition + `issue` bookkeeping + lean commit (Full) | ~1.1 | ~2.3 | `opEnterImpl` 4.51 = `issue` 2.52 (2.19 of it the draw) + compose / commit ~2.0 (sky cube 0.49 of it) |
| sky cube faces (`UpdateSkydomeEnvCubemap`, 5 faces per frame) | 0.15 | 0.52 | +0.37: star vertices converted + copied 3-4 times per face |
| sun cascades / shadow-view ops (`eCCameraBase::Update`) | 0.88 | 1.16 | +0.28 |
| effect units (`effectBegin`, ~25 units / ~45 draws) | 0.25 | 0.45 | +0.2 |

Module view (profiled): ours 1.98 -> 4.37, Engine.dll 1.48 -> 0.96, vcruntime140 0.20 -> 0.79 (memcpy / memmove). So the per-draw device path is shared, the engine
already runs less in Full, and the gap is our composition being ~2x the engine's own slot code for the same ops (plus the sky cube). **There is no single hot spot**:
`pcprofile` self time is flat (largest: `skyBillboardVertices` 2 %, `setVertexShaderConstantF` 1.9 % (the issue-time compare + copy), `safeLoad8/4/1/2` 3.3 % + `safeRead`
1.9 % = ~0.4 ms of guarded engine reads, mostly cache misses on engine memory that the engine's own slot code paid before, `prepareDraw` 2.5 %). The 5 - 6 us per composed
draw = ~1.5 us composition + ~0.3 us `issue` bookkeeping + ~1.6 us shared device path + the rest of the frame.

### 24.1 Changes (`native.cpu_opts` bit mask, Debug, default 255: bit 1 sky cache, bit 2 batched tree-object reads; the CRT-call removal is unconditional)

| change | evidence | result |
|---|---|---|
| sky objects: scratch vectors + the converted star vertices / quad indices kept per sky object while the input billboards are byte-equal (the sun / moon / stars alternate inside one cube face: a single entry thrashed and did not hit; one entry per object does) | `skyObjectDraws` 0.337 -> 0.151, `skyCubeBegin` 0.49 -> 0.31 (profile) | -0.18 ms |
| inline SSE register-run copies (`d3d9::copyRegisters`) in `constantsInto`, `issue`'s save runs, `setVertexShaderConstantF` / `setPixelShaderConstantF`, `splitStage` (each was a CRT `memcpy` call per run, ~20 runs per draw) | vcruntime140 0.79 -> 0.55 ms, d3d9 +0.15 | ~-0.08 ms |
| SpeedTree composition: tree / illumination objects read as one 0x70 / 0x98 byte snapshot instead of 6 + 3 guarded loads per instance | `composeTree` 0.73 -> 0.77 (profile noise) | neutral (the cost is the cache misses, not the call count); kept behind bit 2 |

A/B (Full, savegame, 3 runs each, last 14 windows, `-Perf` CPU = engine + HAL, `build\Base` = cb5097a): **6.898 (6.736 / 7.005 / 6.954) -> 6.532 (6.534 / 6.549 / 6.512) ms, -0.37 ms
(-5.3 %)**; interval p50 7.36 -> 7.08 ms. Wrapper (same session) 4.560 (4.549 / 4.572). **Full - Wrapper 2.34 -> 1.97 ms.** (The machine is shared with other agents; the
arms were interleaved; a first `cpu_opts=0` run at 8.9 ms was a disturbed outlier.) Gates: `rvk_tests` (settings count), Full `testdemo` 7 / 7 with `-Validation` + `native.static_ops_compare`
+ `scene_ops_compare` + `lean_check=10`: every group `mismatches: none`, 0 validation messages (own copy, instance 4).

### 24.2 What is left and what it would take

Remaining Full - Wrapper ~2 ms = composition ~1.0 ms more than the engine's slot code, sky cube +0.2, shadow-view ops +0.28, effect units +0.2, the rest spread. Items measured and
**not** worth it: dropping the per-op device restore (0.07 ms, 21), more SEH-call removal (the cost is cache misses), retained composition (19), merged indirect draws (8.4), device-generated
commands (12). To reach <= +0.5 ms the composition must stop being a second pipeline staged through `DrawDesc` (mirror -> `DrawDesc.vsF` -> device registers with compare -> window block ->
ring = 3 copies of the constants; the engine's own path has 2): **compose each draw straight into the device and issue it per draw** (streaming `newDraw` + `issue`, no per-op draw list; needs the
compare modes to keep the list path) is the structural change with the largest estimate (~0.5 - 0.8 ms: the copy chain, `newDraw` clears of a ~5 KB descriptor, the op-wide save pass), followed by
**moving the shadow-view and sky-cube compositions onto a worker** (they only read engine memory the lean path no longer mutates; ~0.4 ms but needs a thread-safe `Device` draw interface: the render-thread
split that 14.1 / 15.1 rejected).

### 24.3 Bounds for the structural changes, and gates (2026-11-04, second pass)

**Streaming each composed draw into the device** (drop the per-op `DrawDesc` list): bounded from the profile before building it. What it can remove: `newDraw` clears (0.04 + CRT 0.04 ms), one of the
three constant copies (`constantsInto` 0.28 ms must still write the final registers; the compare + copy in `setVertexShaderConstantF` 0.21 ms is the part that goes), `issue` bookkeeping
(self 0.13 + CRT 0.25 ms, partly the save / restore). Upper bound ~0.4 ms, realistic ~0.2 ms (not the 0.5 - 0.8 first guessed). And it breaks the composition's atomicity: ops fall back to
the engine when a composition returns a `Skip` after some draws were composed (`Unreadable`, `NotCompiled`, ...), so earlier draws would already have been drawn. Not built; a streaming path
would have to validate the op up front (a read-only dry run = the composition cost again).

**Composition on a worker** (structural change 2): blocked by the contract of the lean path, not by locks. (1) `opEnter` must answer synchronously whether the engine's slot runs (a `Skip` = it
runs); a deferred composition cannot say. (2) The composition mutates `mirror_` (world / alpha / instance registers written per draw) in op order, and the lean commit's side effects
(touches, instance / tree queue sorts in place, palettes `UpdateRenderMatrices`, particle depth sort, MaskReference stores, class pools) write engine memory the next op's composition
reads, so they must stay on the engine thread, in order, before it. (3) Shadow-view and sky-cube compositions read the same live objects the engine thread keeps changing between slots (shader
pools via `SetSwitch`, the admin's view fields). A one-worker in-order pipeline would only overlap composition with *skipped* slots (nothing) - there is no engine work to overlap with in
lean mode, so the engine thread would just wait for the worker. Estimated gain ~0 ms. Rejected for the same reason as 14.1 / 15.1.

**The device path is the shared 2.2 ms** (`prepareDraw` 1.7 incl: `RtScene::observe` 0.36, `computeVertexInput` 0.27, `uploadConstants` 0.15, `prepareTextures` 0.13, pipeline / intern / dynamic
~0.2). A draw-state fingerprint fast path in Full only (the composed `DrawDesc` already knows pipeline, vertex input and textures; 78 % of consecutive draws differ only in constants) could reuse
`computeVertexInput` + `prepareTextures` + pipeline / intern for ~0.4 - 0.5 ms and would be the largest remaining Full-only gain; it touches the 670-line `prepareDraw` and every consumer of its
results (RT scene, replacers, object motion, G-buffer variants), so it needs its own stage with the compare tools as the gate. Not built here.

**Gates** (build 382a1bd): `rvk_tests` 215 / 215; `rvk_d3d9test --validation` Vulkan and D3D12 `PASS: 0 failure(s), 0 validation error(s)`; `testdemo` 7 / 7 with `-Validation` on Vulkan wrapper,
Vulkan full, D3D12 wrapper, D3D12 full (0 validation lines each; own copy, instance 4); Full compare run (`static_ops_compare`, `scene_ops_compare`, `lean_check=10`) `mismatches: none`.
**D3D12 CPU** (savegame, 2 runs per arm, interleaved): Full 6.727 (6.739 / 6.716) vs Wrapper 4.794 (4.789 / 4.799) = **+1.93 ms**; Vulkan Full - Wrapper +1.97 ms.
`refcompare` vs `out/refcompare/20261103-130010` (Vulkan Full, 5 spots): jungle / beach_rain / pub_interior PASS, hero_closeup WARN (ssim 0.972, noise), harbour_night FAIL once (dluma -3.6, the known night
noise) and PASS twice on re-run (cpu_opts 255 +0.39, cpu_opts 0 +0.37). Not run: D3D12 refcompare, the jungle CPU spot, the 17-spot route.

### 24.4 Draw-state fingerprint: measured ceiling; jungle; D3D12 refcompare (2026-11-04, third pass)

**Fingerprint fast path in `prepareDraw`: not built, the ceiling is too low.** A diagnostic (reverted) compared, at every `prepareDraw`, everything the derivation reads (shaders, declaration, the 4
stream tuples, index buffer, the 100+ render states, the bound stages' textures + sampler states, viewport, targets, kind / view / pass, primitive type) with the previous draw: **17.8 % of the
Full draws at the savegame (5 M draws) are identical to their predecessor**. The derivation it would skip costs `computeVertexInput` 0.27 + `prepareTextures` 0.13 + `computeDynamicState` / intern /
`findPipeline` ~0.15 = ~0.55 ms, so the saving is ~0.1 ms (and the pieces keyed on fewer inputs, e.g. the vertex input on shader + declaration + streams, would hit more often but then save one
component each). Below the noise; the consumers' complexity (observers, replacers, variants, G-buffer) is not worth it.

**Composing per material / per tree type once.** The tree work is per batch, not per instance: per frame ~168 tree ops, ~1,900 instances in ~650 batches, 70 vegetation draws; `materialDraws` costs
0.70 ms = ~1.07 us per batch, of which `constantsInto` + `materialSamplers` are 0.42 (the live registers copied from the mirror: they must be redone because the instance rows differ per batch) and the
pass logic ~0.15. A clone-and-patch of the previous batch's `DrawDesc` (same shader / illumination / geometry stream) would redo the constants, switch writes, lighting, touches and tag - exactly what the V3
replay did (0.57 us per hit, "net zero", plan.md 7.1) - so the ceiling is ~0.3 us x 650 = 0.2 ms. The engine's own tree code recomputes the same per batch (its `RenderBranches` / `RenderLeafs` ~1.0 ms in
Wrapper), i.e. the per-batch cost is inherent to the engine's batch model, ours is on par (0.95 ms for trees + billboards). Static meshes: `composeOp` 0.44 vs the engine's `eCSubMesh::Render` +
`RenderZPass` ~0.5-0.8: also on par. **Per group our composition is not slower than the engine's code; the difference to Wrapper is the sum of the parts the engine does not have to do**: sky cube
(+0.16), effect units (+0.2), the `issue` bookkeeping (~0.4), the shadow-view ops (+0.28), the guarded reads, and the lean commit.

**Jungle spot** (`rvkcompare` jungle hold, `-Perf`, last 3 windows, 2 runs each, `build\Base` = cb5097a): Full before 5.917 (5.933 / 5.900), after 5.975 (6.090 / 5.860) - no change within the noise (the
spot has no night sky work; the sky cache does not apply), Wrapper 3.960 (3.973 / 3.947): **Full - Wrapper +2.0 ms**.

**D3D12 refcompare** (Full via `renderer.mode=full`, 6 spots vs `20261103-130010`): jungle / pub_interior PASS, hero_closeup WARN, harbour_night + sea_cliff FAIL (the known night / sea-edge noise),
beach_rain FAIL once (bad32 1.28) - flaky: the same spot PASSes in D3D12 Wrapper and in Full with `native.cpu_opts=255` on a re-run, FAILs with `cpu_opts=0`, so it is run-to-run noise (rain), not this change.

**Floor.** Ceilings measured or bounded this pass: sky cube further ~0.15, streaming draws ~0.2, per-batch clone ~0.2, fingerprint ~0.1, effect units ~0.1: about 0.7 ms together, for large risk. The
realistic floor of the current architecture (engine's code skipped, composition re-done per frame next to a shared 2.2 ms device path) is **Full - Wrapper ~1.3 ms** (Vulkan and D3D12). Getting to +0.5
ms needs the composition's *results* to survive frames (retained composition across frames with an engine-side invalidation signal instead of a read log; V3's validation was only 0.05 ms, its replay cost
was the constants / lighting / touches redone per frame), i.e. a persistent per-object GPU record, not a faster per-frame composition.

### 24.5 Retained composition with engine change signals: study (2026-11-04)

Full write-up: engine-frame/retained-signals.md. Signals exist for lifecycle and movement (OnCacheIn / OnCacheOut 0x6a9720 / 0x6a97b0, OnUpdatedWorldMatrix vt +0x230 / 0x69e8a0), none for materials, lights, LOD, fade (per-frame pulls / in-place writes). `native.retained` re-measured on the current build: 100 % hits (1,118 per frame), CPU 6.530 -> 6.444 ms (-0.09 ms), replay 0.52 us per hit vs ~0.62 composing: the redo of our own constants / lighting / touches is the cost, not validation. Estimate with signals + class-split patching: ~-0.4 ms, Full - Wrapper ~+1.6 ms: no-go for composition-only retention. Conditional go for retaining the packet (bypassing issue + prepareDraw, estimate -1 to -2.5 ms) as a separate project.

### 24.6 Packet retention, stage 0: the ceiling (2026-11-04)

`-Perf` now logs per group what `issue()` costs per draw and how much of it is the device call (retained-signals.md 7). Savegame Full: static meshes 2.10 us/draw (232 draws per frame), trees 1.56 (680), shadow views 1.59 (160), actors 1.77 (21), vegetation 2.50 (66); Vulkan and D3D12 within 3 %. Bypassing issue + prepareDraw at 0.3 us per replayed draw would save 0.42 ms for static meshes alone and ~1.5-1.7 ms with trees, shadow views, actors (vegetation, alpha). Static meshes alone do not reach -1 ms; the combination does.

### 24.7 Packet retention, stage 1 (2026-11-04)

`native.packet_cache` (Debug, default off): static-mesh + tree draws of the main view replayed from kept packets (retained-signals.md 8). Savegame, baseline settings, 3 runs each: Vulkan Full 5.470 -> 5.145 ms (-0.33), Wrapper 3.674, gap 1.80 -> 1.47; D3D12 5.78 -> 5.28, gap -> 1.46. Measured replay 0.35 us per draw; the section-7 ceiling (1.3 ms) was inflated by the RT scene observer that the player defaults have inside prepareDraw; with it active the cache is refused. Verify mode: 4.9 M checks, 0 differing.

### 24.8 native.packet_cache is on by default (2026-11-04)

Default 1 (Full mode only; auto-off while a compare / tap / coverage is on and per draw while a consumer needs prepareDraw without a cached query; modes 2 / 3 = verify, Debug). Evidence: 0 differing over millions of verify checks (baseline and player defaults), all gates on both backends, Vulkan paired runs -0.33 / -0.92 ms (min-of-arm -0.33). **The D3D12 gain is unresolved under load** (pairs scatter -1.1 .. 0; the other agents' builds are not covered by the lock names): re-measure opportunistically with one off/on pair when the machine is quiet (`-Setting native.packet_cache=0|1`, `-PlayerDefaults`).

### 24.9 Packet cache stage 4 and clean timings (2026-11-04)

Shadow-view ops joined the cache; refusal reasons are logged. Clean player-default timings, 3 interleaved pairs: Vulkan 6.672 -> 6.254 ms (-0.42), D3D12 6.939 -> 6.520 (-0.42); Wrapper 4.50 / 4.84, so Full - Wrapper 1.75 / 1.68 ms. `native.retained` + the cache: no extra gain (stage 3 no-go). Details and the floor: engine-frame/retained-signals.md 11.

## 25. Variant draws in the packet cache (2026-11-04)

The player defaults (material pack present) took the lit solid draws out of the packet cache; the variant decision is now a cached, re-validated property of the packet (retained-signals.md 13): replayed share ~32 % -> 99.9 %, Full CPU
Vulkan 7.04 -> 6.56 ms, D3D12 7.32 -> 6.69 ms (3 interleaved pairs each); with GI + AO -0.47 ms, with upscaling on top -0.89 ms (jitter is patched at replay). The material pack costs +0.57 ms in Wrapper and +0.25 ms in Full (the earlier
"7.2 vs the documented 6.25" was the pack: Full without it 6.3). `native.packet_cache=4` = the old behaviour for A/B. The remaining Full - Wrapper gap is the composition (flat profile, ~1.2 us per op); see retained-signals.md 13.

## 26. Per-group Full CPU profile at the player defaults; tree batch merge probe (2026-11-04, gpu-scene.md 9.3)

Savegame, lod 2x, Vulkan Full, `-CpuProfile -Perf` (`-Perf` CPU 6.38 ms engine + HAL). Composition + `issue()` per group: trees 1.8 ms (655 batches / frame, 2.9 instances each), static meshes 0.84, shadow views ~0.5, sky cube + dome 0.58,
vegetation 0.40 (composition 0.16 measured; the device path is shared with Wrapper), effects / particles / decals / alpha / actors ~0.9. A tree-batch merge (consecutive batches of an op as one multi-range draw) finds -10 % (z) / -28 % (solid, equal
light list) / 0 % (equal illumination object) draws: every tree has its own illumination object, so only a picture-changing merge helps. No code change; gpu-scene.md 9.3 says why a GPU-driven consumer needs the whole persistent-window design (trees first).

Update (2026-11-04, gpu-scene.md 9.4): ceiling experiments for a GPU-driven tree path: one draw per tree op (picture changing) -0.98 ms; exact merge (same light list / limit; what a draw index + per-instance light fetch can do) -0.17 ms (1,290 -> 1,245 draws).

## 27. Sky cube refresh interval in Full (2026-11-04)

With the enhanced water on (player default) the engine's sky cube (5 faces: 20 draws per frame, composition 0.33 + `issue()` 0.25 ms in Full) was redrawn whole every frame (ocean.md 9.8: its own refresh is one face per ~10 s, which left lightning flashes baked into some faces).
`debug.sky_cube_interval` (Debug, ms, default 100, 0 = every frame; **applies in Full only**, Wrapper keeps the per-frame refresh) refreshes the whole cube every 100 ms of wall time instead; in between the detour resets the engine's timer and calls its own function, which then
takes the "not expired" path and draws nothing (`engine/scene_hooks.cpp`, `detourSkyCube`). The cube is the same whole-cube refresh at 10 Hz (a lightning flash reaches the reflection <= 0.1 s late).
Paired timing (player defaults, savegame lod 2x, `-Perf` CPU, 2 pairs): Vulkan Full 6.43 -> **5.98** (-0.45), D3D12 Full 6.49 -> **6.07** (-0.42); Wrapper (no stride) Vulkan 4.855, D3D12 5.08: **Full - Wrapper 1.58 -> 1.12 (Vulkan), 1.41 -> 0.99 (D3D12)**. (Wrapper with the stride, measured for
information: 4.855 -> 4.67; not shipped, Wrapper stays the faithful mode.) Pictures: `water-compare -Mode full` interval 0 vs 100 on `beach_night_storm` (thunder, clock running), `sea_horizon`, `beach_sun`: no cube seam, differences are the running clock's waves / lightning timing.

Remaining Full - Wrapper after this (profile of the same run, ms per frame inclusive): shadow-map path 0.90 (engine 0.9 of which our ops ~0.3), effect units 0.42 (`effectBegin`, composition 0.20, `issueLean` 0.18), trees 1.8, static 0.84, vegetation 0.4 (0.1 - 0.16 Full-only).

## 28. Table lighting in Full (2026-11-05, gpu-scene.md 9.8)

`lighting.full_lit` (Debug, default off; with `lighting.clustered`): one 3-light pass per lit draw, no additive passes, mesh batches merge across different illumination alpha factors, the table takes drawn objects' lights. Paired CPU (savegame, lod 2x, player defaults, interleaved, `-Perf` engine+HAL): Vulkan Full 8.04 -> 7.46 ms (-0.58; Wrapper 6.04, gap 2.0 -> 1.4), D3D12 8.23 -> 7.69 (-0.54); `clustered` alone 8.10 (+0.06). Pictures at pub_interior / monastery_hall / harbour_docks / jungle within the stage-a noise floor once the engine's own lights were kept as the fallback (zeroing them dimmed interiors by 17 - 26 %). Goal (gap <= ~0.5 ms) not reached.

## 29. Table lighting, completed (2026-11-05, gpu-scene.md 9.9)

`lighting.tree_merge` + `lighting.table_complete` (see gpu-scene.md 9.9). Vulkan, paired: Wrapper 5.93, Full 8.00, `clustered + full_lit` 7.52, + `tree_merge` 7.53 (the savegame's trees end their batches on their own far static lights: nothing to merge). A forced 40 % batch reduction (table far 48,000) gained 0.05 - 0.1 ms in timing runs, 0.3 - 0.5 in the sampled profile: draws are 99.9 % packet-cache replays. Gap Full - Wrapper 1.6 ms. `kLightFar` = 48,000 changed pictures (monastery_hall mean |d| 12.7): stays 16,000.

## 30. Lighting round 3 (2026-11-05, gpu-scene.md 9.10)

Far light level, the group-shared cluster build (0.228 -> 0.112 ms Vulkan, 0.232 -> 0.150 ms D3D12), lamp shadows (`light.shadow` 0.07 / 0.09 ms at 9 lights, +0.25 / +0.3 ms GPU in `many` mode at harbour_docks), physical-style PBR code (+0.45 ms GPU Vulkan, +0.8 ms D3D12 in the jungle save when compiled into every cluster variant: now compiled in only when a session starts with `lighting.style=physical`). CPU: lit 7.34 vs Full 8.04 (Vulkan), 7.50 vs 8.33 (D3D12, mean of two); Wrapper 5.99: Full - Wrapper 1.3 - 1.5 ms. The table-known tree rule cuts the tree batches by 37 % (4.27 M -> 2.70 M per run) for -0.1 ms.

## 31. Frame spikes 4 times a second: the display watcher (2026-11-06)

`-Perf` now logs `perf: frame spikes: N of M intervals > X ms (median m), gap G frames (min / max); spike frames: interval =
CPU (slot wait) + present; other frames: ...` per 5 s window (intervals above 2x the median and 4 ms over it; "present" = the
previous frame's present call, which the interval contains). It found a +17 ms CPU spike every 250 ms: `watchDisplay`
enumerated all 334 display modes each time (window-mode.md 10.1). Fixed; note that `-CpuProfile`'s window (present end ->
`SwapScreen` entry) does not cover our own `SwapScreen` prologue (settings, overlay, menu pacing, BeforePresent passes,
window / display watchers): a spike the `perf: frame spikes` line shows as CPU but the spike profile misses lives there.

