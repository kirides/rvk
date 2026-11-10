# Low bandwidth mode (`vulkan.low_bandwidth`)

Player setting, System page, Hardware section, Basic tier; Enum `auto | off | on`, default `auto`, switches at run time (A/B eligible).
Target: integrated GPUs that share the PC's memory (Radeon 680M: RDNA2, about 100 GB/s shared). It trades a little of the
enhancements' own intermediate images for bandwidth. Changes without a picture cost (always on) are not part of it
(performance.md 32).

## Decision

`caps::resolveLowBandwidth(mode, report)` (`src/core/caps.*`, pure, unit-tested in `tests/caps_tests.cpp`): `on` / `off` win;
`auto` = on when `Report::integratedGpu`. One function, so another criterion (video memory size) can be added later.
`integratedGpu`: Vulkan = the probe's `VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU` of the device in use (`Context::capabilities`);
D3D12 = dedicated video memory <= 512 MiB (as `enumerateD3D12Adapters`). The HAL (`hal/vulkan/device.cpp`) resolves it when the
capability report is set and on every change of the setting and stores it (`caps::setLowBandwidth`); the passes read
`caps::lowBandwidth(item)` where they create their images. rvk.log: `vulkan: low bandwidth: on (auto: integrated GPU)` /
`off (set by the player)`. The overlay's combo shows "Auto (on: integrated GPU)" (`overlay_ui.cpp`).
Images are transients keyed by description, so a change re-creates them; histories are invalidated where needed.
`-Environment 'RVK_FAKE_INTEGRATED=1'` makes the device count as integrated (auto = on, plus the Integrated graphics preset's first-run defaults, overlay.md 2.9 / frame-ownership.md 14.11).
Developer knob: `-Environment 'RVK_LOWBW_OFF=<mask>'` leaves single items out (1 AO, 2 screen probes, 8 material RT, 16 water RT) for bisecting pictures.

## Items (bytes at 1920x1080 = 2.07 M pixels; estimates from the formats, not measured traffic)

| # | Change | Resident | Per frame |
|---|---|---|---|
| 1 | `gtao.raw` R32F -> R16F (`gtao.comp` variant `GTAO_RAW_R16F`, pipeline built on a worker at first use; R32F in the meantime) | 8.3 -> 4.1 MB | one write + one read: 16.6 -> 8.3 MB |
| 2 | `sgi.resolved0/1`, `sgi.stats0/1` (RGBA16F) at half size. `sp_resolve` writes texel i for pixel 2i and reprojects its history in texel units; frame flag `kFrameSgiHalf`; readers (`rvk_material_variant.hlsli`) take the 2x2 texels around their pixel bilinear x depth weight (`rvkMvSgiTaps`), and the existing depth-aware gather (`rvkMvGather`, new scale argument) when too few are valid. Off while `gi.reference` runs (the reference reads at its pixel). | 66.4 -> 16.6 MB | writes 33 -> 8 MB, history reads and the solid pass's fetches a quarter; a quarter of the resolve's invocations |
| 3 | `matrt.hist0/1` at the trace image's size (the filter runs at that size, `pc.pad`; the composite does a depth-aware 2x2 lookup, `material_rt.frag`); every tier traces one ray per 2x2 pixels (High: 1x1 -> 2x2) | 33.2 -> 8.3 MB | about 37 MB less (filter write, history read, composite read), High traces a quarter of the rays |
| 4 | lamp shadows (`light.shadow raw / final`) half size: **tried and dropped**, see below | - | - |
| 5 | water RT scale forced to 2 (`water_rt.cpp`; Low / Medium already are) | High: 16.6 -> 4.15 MB | a quarter of the rays at High |
| 6 | `upscale.opaque` RGBA16F -> B10G11R11: **not done** (`upscale.cpp` is owned by another change; it is a transfer copy, so it would need a blit and a check of the FFX reactive-mask input formats) | - | 8.3 -> 4.1 MB would be possible |

### Item 4: lamp shadows at half size, dropped

The entries are ids (`id + q/64`), so the readers can only take the nearest texel. In `monastery_hall` the picture got visibly darker
(luma -3.1 %, mean |d| 8.4 against a noise floor of 0.8, 16.5 % of the pixels differing by more than 12 / 255); with item 4 left out the
residual fell to 1.8. The code was removed again (git history: the WIP commit of this work). A way to try it again: store the tested light
ids in a separate full-resolution-agnostic image and the visibilities filtered, or keep the trace at half size and upsample into the
full-size final image with a depth-aware filter.

## Pictures (Full, all enhancements on, `water-compare.ps1`, off vs on, 1600x900; `tools/sbs_diff.py`)

Control = the same settings on both sides (the run-to-run noise); low bandwidth on, items 1, 2, 3, 5:

| spot | mean abs diff control | mean abs diff on | luma change |
|---|---|---|---|
| harbour_docks | 1.03 | 0.75 | +0.1 % |
| jungle | 3.19 | 3.21 | +0.6 % |
| monastery_hall | 0.82 | 1.48 | -1.6 % |
| pub_interior | 0.94 | 1.18 | +0.2 % |

The pictures look the same to the eye (monastery_hall: the only measurable difference is a slightly lower fill light; the candle flame
differs by animation). Pictures: `out/water/20261106-180507` (final), `-175241` (with item 4, the dark one), control `-175540`.

## GPU time

RX 7900 XTX (not bandwidth bound; harbour_docks, frozen hold, 1600x900, vsync off, frame interval p50): off 5.26 - 5.29 ms, on 4.79 - 4.30 ms
(about -0.5 ms; the runs settle over time, so read this as "a small gain", not a precise number). GPU memory allocated 1003.7 -> 939.9 MiB
(-64 MiB). In jungle (CPU bound) no change. The 680M, which is bandwidth bound, should gain more; not measured here. `-PerfFrame` pass timings
were not obtained in these runs (the scenario ended before the frame).

## Validation

`rvk_tests` (incl. the auto resolution), `rvk_shadertool corpus ... --gbuf | --gi | --gbuf-motion | --cluster --gbuf` (0 failures),
`dxil-check embedded | corpus --gbuf | --gi`, `rvk_d3d9test --validation`, `rvk_rhitest --validation`; a Vulkan run with the validation layer
and GPU-assisted validation: no validation errors, no device loss.

## 680M notes

Shared memory: every byte written by the enhancements competes with the CPU and the game's own textures; the saved intermediate images are
mostly full-resolution RGBA16F. Further candidates (not done): the G-buffer normal / motion targets, the GI probe volumes' update rate, the
`upscale.opaque` copy (item 6), ocean FFT cascades.
