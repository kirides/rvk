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
  registry. **Open, for the user**: check Adrenalin's per-game settings for Risen (Radeon Chill, Frame Rate Target
  Control, Anti-Lag) and whether the cap disappears when the game window is in the foreground or when no other game
  (AION 2) runs. Our side cannot remove it; everything below improves what happens within it.

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

1. The external ~80 Hz present cadence (section 2): the largest item, outside our code.
2. Engine CPU (~3-4.5 ms per frame, single-threaded) and our draw path (1.9 us per draw): the next candidates are
   `bindVertexInput` (declaration matching per input per draw, `Buffer::gpu()` lock per stream), the per-draw constant
   blocks (2 x 4.4 KiB of ring space per draw, ~11.5 MiB per frame; only the used registers are written), the 176-byte draw
   block per draw.
3. First-run pipeline stalls (until the keys file exists) and the 288 ms world-entry frame: pipeline libraries would solve
   the first run if the AMD crash goes away; a shipped keys file (from a full play-through) would also do.
4. Ring memory is device-local host-visible (ReBAR); 11.5 MiB of address space per frame is fine, the CPU writes are the
   constant registers actually used.
