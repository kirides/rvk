# DXVK: techniques, optimizations and workarounds for our renderer

Source studied: `B:\DevDrive\git\github\dxvk` at `cf0a0d90` (2026-11-02), read only. DXVK's D3D9 shader compiler
(`src/dxso`) was removed from the tree in `aeb0f217` (replaced by the `subprojects/dxbc-spirv` submodule, not checked out):
its last version is `git show ee2cce21:src/dxso/dxso_compiler.cpp`. Survey 2026-11-05 in four areas (device / memory /
submission, presentation, pipelines / barriers, D3D9 semantics), each finding checked against our code.

Driver facts of the dev machine (RX 7900 XTX, AMD proprietary 26.10.1, `vulkaninfo`): offers VK_EXT_memory_priority,
VK_EXT_pageable_device_local_memory, VK_KHR_present_id2 / present_wait2, VK_KHR_swapchain_maintenance1,
VK_EXT_custom_border_color, VK_EXT_depth_range_unrestricted; **no** VK_EXT_depth_bias_control, no VK_AMD_buffer_marker.

## 1. Applied (2026-11-05, one commit each)

| what | DXVK | ours now |
|---|---|---|
| device pick skips CPU devices; scalarBlockLayout / bufferDeviceAddress in the floor check; VkDriverId kept | `dxvk_device_filter.cpp`, `dxvk_device_info.cpp` | `vk/probe.*`, `Context::pickPhysicalDevice` |
| VK_EXT_memory_priority + pageable device-local memory: attachments 1.0, everything else 0.5 | `dxvk_memory.cpp` | every `VmaAllocationCreateInfo` sets `priority` (zero-init would be the lowest!); rvk.log `vulkan: driver id ..., memoryPriority=, pageableDeviceLocal=` |
| frame waits in slices, device loss reported from waits | `dxvk_queue.cpp` (5 s, then lost) | `Frames::waitFor`: 4 s slices, `vulkan: the GPU has not finished frame value N after T s`, DEVICE_LOST -> `reportDeviceLost` |
| surface re-created after VK_ERROR_SURFACE_LOST_KHR | `dxvk_presenter.cpp` `recreateSwapChain` | `Swapchain::recreate` (before: rebuilt on the dead surface every frame, forever) |
| ChangeDisplaySettingsEx retried without the refresh rate | `wsi_window_win32.cpp` | `engine/window_mode.cpp` (exclusive fullscreen) |
| exact (rational) monitor refresh rate | `wsi_monitor_win32.cpp` (QueryDisplayConfig) | `pacing::monitorRefreshRate`: **bug fix**, the integer `dmDisplayFrequency` (59 for 59.94 Hz) kept the pacer's vblank lock (0.5 % tolerance) off on NTSC-rate panels: 59.000 fps against a 59.94 Hz FIFO = a repeated frame per second |
| pipeline compiles outside the map lock, no duplicate compiles, draw-time requests first | `dxvk_graphics.cpp` (placeholder + per-pipeline lock), `dxvk_pipemanager.cpp` (priorities) | `PipelineCache::get` / prewarm workers (`building_`, `builtCv_`), `prewarmOnce` -> urgent queue, `data()` own mutex |
| pipeline-library recipe | `dxvk_shader.cpp` 435 / 535, `dxvk_graphics.cpp` 1454-1512 | `vk/pipeline_library.cpp`: no retained link-time info, no VkPipelineCache, no multisample state in the fragment library, optimised = monolithic build; section 2 |
| translator: rcp / rsq / exp clamped to FLT_MAX, log to -FLT_MAX (floatEmulation "Enabled") | `dxso_compiler.cpp` 1951-1972, 2009-2043, 2217-2226 | `shader/hlsl.cpp`, kTranslatorVersion 10: 0 * rcp(0) = 0 and FXC's expanded pow(0, 0) = 1 as on D3D9 (legacy multiply), not NaN |
| translator: `rvk_pow` (pow(x, 0) = 1) on Vulkan too | `dxso_compiler.cpp` 2044-2060 | version 11 (SPIR-V Pow(0, 0) undefined; AMD gives 1, others may give NaN) |
| translator: `precise` position on Vulkan too | `oPos` decorated Invariant (3587) | version 12 (z pre-pass vs solid pass ZFUNC_EQUAL on non-AMD drivers) |
| viewport MinZ / MaxZ clamped to [0, 1] | `d3d9_device.cpp` 6796-6812 | `Device::computeDynamicState` |
| constant depth bias with reversed float depth | VK_EXT_depth_bias_control (float representation) | not offered on AMD: folded into the reversed-depth remap `posFix.w = 1 - k b` (exact; the rasterizer's float bias scaled with n / viewZ's exponent: ~1/1000 of the bias at 100 m); slope part stays in the rasterizer |

Verification: build, rvk_tests 248 / 248, rvk_vktest default / `--rt` / `--passes`, rvk_d3d9test, shader corpus 2483 / 2483
SPIR-V + DXIL (`corpus`, `dxil-check`), `testdemo` + `rvkcompare jungle` with `-Validation` (0 messages, vsync on: pacer
locked at 120.003 on the 120 Hz monitor; the 59.94 Hz path itself was not testable here), refcompare against
`out\refcompare\20261022-093252`: 11 PASS, FAIL `sea_cliff` / `harbour_mouth` (fail in every run of the day before these
changes too) and `harbour_docks` (surf foam beside the pier, wave-phase dependent: the same difference, smaller, in the
morning's run `20261105-095841`).

## 2. Graphics pipeline libraries (still off by default)

Cold pipeline state (`pipelines.bin` / `pipelines.keys` deleted), rvkcompare `jungle,pub_interior`, Wrapper, `-Perf`:

| | libraries off | libraries, old recipe (`RVK_GPL_RECIPE=lto`) | libraries, DXVK recipe (default) |
|---|---|---|---|
| draw-time pipelines at world entry | 163 built in **1940 ms** | 166 linked in 3.3 ms | 166 linked in 16.9 ms |
| background optimised builds | (120 earlier-run pipelines, 4.9 s) | 154 in 2958 ms + 20 in 452 ms + 3 | 179 in 2188 ms + 3 |
| library compiles | | 927 in ~60 s of loader-thread time (testdemo) | corpus: 2561 in **3.6 s** (old recipe 135 s) |
| crashes / validation | 0 / 0 | 0 / 0 | 0 / 0 (`-Validation`) |

The AMD compiler crash that disabled libraries (2 fragment libraries, driver of 2026-10, performance.md 6) does not reproduce
with driver 26.10.1 (`rvk_vktest --gpl-corpus`, 2561 modules, both recipes). Turning `[vulkan] pipeline_libraries` on by
default (or per driver version) is open: a player on an older AMD driver could still hit the crash. Test switch:
`run.ps1 -PipelineLibraries` (+ `-Environment 'RVK_GPL_RECIPE=lto'`).

## 3. Candidates not done (ranked)

1. **Present wait** (VK_KHR_present_id2 / present_wait2, offered on AMD): DXVK's `dxvk-frame` thread waits for each FIFO
   frame to be displayed and limits the CPU to frame N - maxFrameLatency (`dxvk_presenter.cpp` 2107-2189,
   `d3d9_swapchain.cpp` 1157). Our limit is GPU completion (2 in flight) + a 3-image FIFO + the vblank-locked pacer; DXVK's D3D9
   default (BackBufferCount + 1) is about the same depth, so the gain is at most one refresh of latency with vsync; useful
   for a measured "frame shown" time.
2. **Swapchain maintenance1**: present fences (spec-correct semaphore / swapchain teardown without device-wait-idle),
   present-mode switch per present (vsync toggle without recreation). `dxvk_presenter.cpp` 196-298, 736-879; keep DXVK's
   NVIDIA quirk (no fence signal on PRESENT_TIMING_QUEUE_FULL).
3. **Upload command buffer barrier**: `rhi/vulkan/vulkan_device.cpp` `uploadCommands` orders the frame's uploads after ALL
   earlier work (ALL_COMMANDS -> TRANSFER): frames with uploads lose the overlap with the previous frame's tail. DXVK orders init
   work only through the touched resources' barriers. Small gain on one queue; needs the re-upload-of-a-live-image case handled.
4. **Barrier batching** (`DxvkBarrierBatch`, `dxvk_barrier.cpp` 444-508): `Image::transition` issues one vkCmdPipelineBarrier2
   per image (scope begin, prepareTextures); the rhi `transitions()` batch exists but the hot paths do not use it.
5. **Clears as load ops / scopes that end only when the targets change** (`dxvk_context.cpp` 2232-2420, 7642-7652):
   `Device::clear` begins with LOAD + vkCmdClearAttachments; setRenderTarget ends the scope at once.
6. **Bake state into optimised pipelines** (`dxvk_graphics.cpp` 781-826): ours keep vertex input / blend / write mask dynamic
   in every pipeline; measure with RGP before building (two-level key, more pipelines).
7. **Staging in system memory** (`dxvk_staging.cpp`): texture staging comes from the BAR ring (256 MiB without ReBAR), ring
   chunks / arena blocks are never freed; budget-aware fallbacks (`VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT`) for low-VRAM cards.
8. **Driver workaround framework**: `VkDriverId` is now kept; a decoded driver version + `matchesDriver` helper
   (`dxvk_device_info.cpp` 1130-1148) when the first workaround needs it. DXVK's list worth remembering: NVIDIA < 560.28.3 wrong
   clear-colour format with a different view format, Intel up to Xe2 imprecise sin / cos near 0, NVIDIA < 580 uint -> float
   conversion signed, NVIDIA present_id2 broken on old drivers.
9. **Strict multiply** ("floatEmulation = Strict": `select(b == 0, 0, a) * ...` on every mul / mad / dp, `dxso_compiler.cpp`
   1373-1435): DXVK enables it only where the driver pattern-matches it to a native legacy multiply; the clamps of section 1
   cover the common cases.
10. Smaller: GPU checkpoints on NVIDIA (VK_NV_device_diagnostic_checkpoints) for the crash markers; exact border colours
   (VK_EXT_custom_border_color, offered; ours snap to black / white / transparent); alpha-test precision by render-target
   format (`GetAlphaTestPrecision`); anisotropy only with a mip filter and > 1 mip; gamma ramp in the present blit (exclusive
   fullscreen only); IMMEDIATE before MAILBOX as a vsync-off option; volume `texldp` divides only xy; FF fog for SM < 3 PS
   (check whether the game uses it).

Already equivalent (no action): timeline-semaphore frames, transient per-slot command pools, push descriptors + bindless,
dynamic rendering, fast link + background optimise, redundant bind skipping, secondary command buffers, per-mip layout
tracking, sampler dedupe, D24S8 -> D32S8 fallback, device-fault reports, surface format / image count / extent logic, present
thread, out-of-date / suboptimal recreation, exclusive fullscreen handling, the frame limiter (waitable timer + spin), vPos /
vFace / half-pixel / oDepth clamp / rsq and log of abs / nrm of 0 / lit clamp / cmp / cnd / mova rounding / relative
constant bounds / loop caps / missing attributes / unbound samplers.
