# Full mode's extra CPU time: where it goes (2026-10-28, measurement only)

Question: Full (`renderer.mode = full`, README section 17) costs about +5.4 ms CPU per frame against Wrapper at the savegame (section
16). Where exactly? Goal of the coming optimization: remove the engine's double work (its render code still runs for every op / unit,
its draws are dropped at `prepareDraw`). Build: HEAD 2e61385 (`feat/native-scene`, d3d9.dll RelWithDebInfo of 08:44, an incremental
rebuild relinked nothing). No source change; every number below is from the runs in `out/optprof/` (logs `rN_*.log`, scripts
`batch.ps1`, `table.py`, `cats.py`).

**Result in one paragraph.** Full costs +5.70 ms (profiled) / +5.22 ms (unprofiled) per frame over Wrapper. Engine.dll's own self time is
the same in every mode (1.23 - 1.33 ms in all 16 profiled runs): the engine does not work harder in Full. The extra is ours: the
composition + issue of our draws at the op's entry (`StaticOps::opEnter` = 5.70 ms inclusive, 61 % of the Full frame) and the effect units.
The truly "double" part (the engine's render code + its HAL calls + its dropped draws + the save / restore that only exists because the
engine's code runs after ours, the per-op re-read of the engine's pools, the effect units' state capture) is about **2.4 ms**; the larger rest (**~3 - 3.5 ms**) is the cost of our own composition and issue
path per draw (~5 - 6 us per composed draw, against ~1.1 us for a wrapper draw), which stays after the engine's work is removed unless it is
made cheaper. Per group the cost follows the draw count: SpeedTrees alone +1.97 ms (351 draws per frame), static meshes +1.19 ms (255),
vegetation +0.64, shadow views' z ops +0.60, the post chain +0.34 (19 draws: the most expensive per draw).

## 1. Method

Scene and recipe: savegame `Risen_Save_0009` (the N5.x cost scene of section 16), `run.ps1 -Exe RisenTest.exe -Vsync off -Perf -CpuProfile
-QuickSave ...\Risen_Save_0009.save -Mods rvk_perfsave -Scenario rvk_perfsave -Param hold=40` (test baseline settings, lod 1x, 1600x900,
windowed, in the background), one setting per run, one run at a time. Machine idle before the runs (`sysprobe` before run 1, during batch
1 and before batch 2: 16-23 % total CPU = the agent tooling, nothing heavy, 41-44 GiB free, no steamwebhelper above 0.4 GiB).

Numbers: the last 6 `perf:` 5-s windows of the hold (the first window after "hold begins" dropped); CPU = `perf:` "engine+HAL" per frame
(the engine thread between presents, i.e. engine + our HAL / device + recording), GPU = the GPU time per frame. Profile = `[debug]
cpu_profile` (`diag::frameSampler`, 0.5 ms sampling of the engine thread, ~4,500 samples per window), aggregated over the same 6 windows;
a percentage x the window's average between-present time ("base") = ms per frame. Our DLL's function starts symbolized with
`llvm-symbolizer` (`out/optprof/table.py`), our leaf pcs with `--inlines` (`tools/pcprofile.py` and `table.py`).

Limits of the profiler (no code was changed, so these stay): it prints per window the top 12 modules, top **30** self functions, top **45**
inclusive functions and the top **120** 16-byte pc buckets of our module. So (1) a function below the cut in a window counts 0 there
(small values are lower bounds; e.g. `eCProcessor::Process` shows 0.42 ms in Full against 0.73 in Wrapper only because it fell below the
cut in 3 of 6 windows); (2) there are no caller / callee pairs, so a leaf such as the CRT's memcpy cannot be attributed to its caller; (3)
the pc buckets cover only 31 % of the engine thread in Full (our module is 54 %): spread-out functions (`prepareDraw`) are under-counted
there, the function-level self list is used for them. The bisection runs (one group per run) are the primary measurement; the profile
explains them. The profiler itself costs ~10 % (3.66 vs 3.30 ms Wrapper, 9.36 vs 8.52 ms Full).

Compare modes in Full: off. `nativePlan` (core/config.cpp) sets compare only from `native.static_ops_compare` / `native.scene_ops_compare`
(default false); every group's log line says "own draws checked 0". The coverage counter and the frame tap's attribution have no switch:
they run whenever the tap is on (every Full run); their cost is bounded by run 3 (the tap alone in Wrapper).

## 2. Runs

`native.scene_ops_groups` bits (settings.cpp): 1 alpha / refracted meshes, 2 overlay passes, 4 decals, 8 sky dome, 16 billboards, 32 sky
cube faces, 64 skinned actors, 128 SpeedTrees + tree billboards, 256 vegetation, 512 shadow views' z ops, 1024 shadow mask chain + terrain
blur, 2048 shadow views' setup, 4096 particles, 8192 strips + streaks, 16384 precipitation, 32768 post chain, 65536 sprite batcher. The
static ops (z pre-pass + solid static meshes) are on in every Full run (Full sets `staticTake` regardless of the groups), so `full g0` = the
tap + the static ops.

| run | setting | fps (`-Perf`) | CPU engine+HAL ms (range) | draw calls ms | GPU ms | draws | ring KiB / frame |
|---|---|---|---|---|---|---|---|
| r1 | wrapper | 254.1 | 3.66 (3.62-3.71) | 0.97 | 1.51 | 916 | 7,871 |
| r3 | wrapper + `native.frame_tap=true` | 236.1 | 3.96 (3.87-4.05) | 1.07 | 1.51 | 916 | 7,869 |
| r4 | full, groups 0 (static ops only) | 184.5 | 5.15 (5.12-5.17) | 1.11 | 1.51 | 916 | 8,264 |
| r8 | full, 63 (alpha, overlays, decals, sky, billboards, sky cube) | 177.2 | 5.39 | 1.10 | 1.50 | 910 | 24,531 |
| r5 | full, 64 (actors) | 175.2 | 5.44 | 1.12 | 1.51 | 910 | 8,224 |
| r10 | full, 128 (SpeedTrees) | 135.5 | 7.12 | 1.16 | 1.50 | 918 | 8,544 |
| r11 | full, 256 (vegetation) | 165.1 | 5.79 | 1.14 | 1.48 | 916 | 10,615 |
| r6 | full, 384 (trees + vegetation) | 125.2 | 7.72 | 1.17 | 1.51 | 909 | 10,820 |
| r12 | full, 512 (shadow views' z ops) | 166.2 | 5.75 | 1.16 | 1.49 | 917 | 8,394 |
| r13 | full, 3072 (shadow mask chain + shadow setup) | 178.5 | 5.34 | 1.12 | 1.48 | 910 | 8,221 |
| r7 | full, 3584 (all shadow groups) | 162.7 | 5.87 | 1.15 | 1.51 | 909 | 8,352 |
| r14 | full, 4096 (particles) | 180.7 | 5.27 | 1.11 | 1.48 | 918 | 17,270 |
| r15 | full, 32768 (post chain) | 173.8 | 5.49 | 1.16 | 1.47 | 910 | 8,233 |
| r16 | full, 65536 (sprite batcher) | 183.6 | 5.18 | 1.11 | 1.48 | 910 | 8,171 |
| r9 | full, 126976 (particles, strips, precipitation, post, GUI) | 171.3 | 5.58 | 1.14 | 1.47 | 909 | 17,116 |
| r2 | **full** (131071) | **103.9** | **9.36 (9.27-9.49)** | 1.24 | 1.52 | 909 | 36,233 |
| r17 | wrapper, no `-CpuProfile` | 281.4 | 3.30 (3.27-3.40), p50 3.64 | 0.87 | 1.46 | 909 | 7,808 |
| r18 | full, no `-CpuProfile` | 114.1 | 8.52 (8.47-8.60), p50 9.20 | 1.13 | 1.48 | 909 | 36,386 |

GPU time is the same in every run (1.46 - 1.52 ms): Full is purely CPU-bound extra. Window-to-window spread is <= 0.2 ms; the groups add up:
g0 + the deltas of 63 / 64 / 384 / 3584 / 126976 = 5.15 + 0.24 + 0.29 + 2.57 + 0.72 + 0.43 = 9.40 ms vs 9.36 measured; 128 + 256 = 2.61 vs
384 = 2.57; 512 + 3072 = 0.79 vs 3584 = 0.72.

Module self time (ms per frame, profiled runs):

| run | d3d9 (ours) | Engine.dll | vcruntime140 | ntdll | SharedBase | amdvlk64 |
|---|---|---|---|---|---|---|
| wrapper | 0.90 | 1.23 | 0.20 | 0.43 | 0.40 | 0.21 |
| wrapper + tap | 1.10 | 1.29 | 0.26 | 0.40 | 0.42 | 0.21 |
| full g0 | 2.05 | 1.27 | 0.42 | 0.46 | 0.42 | 0.25 |
| full g384 | 3.92 | 1.26 | 0.92 | 0.64 | 0.42 | 0.22 |
| full | 5.00 | 1.32 | 1.17 | 0.71 | 0.50 | 0.26 |

Of the +5.70 ms: ours +4.10, vcruntime140 +0.97 (a memcpy, below), ntdll +0.28 (heap: `RtlAllocateHeap` 0.17 + `RtlFreeHeap` 0.08), Engine.dll
+0.09, SharedBase +0.10, amdvlk64 +0.05.

## 3. Cost per group (bisection)

Per frame at the savegame (Full run r2: 909 draws, ~500 ops; ops / draws from the groups' "drawn by the renderer" totals over 4,990 world
frames):

| group | ops / frame | draws / frame | CPU added | per draw |
|---|---|---|---|---|
| op tap + attribution + coverage (r3 - r1) | ~650 snapshotted | 916 seen | **+0.30 ms** | 0.3 us |
| static ops (r4 - r3) | 184 | 255 | **+1.19 ms** | 4.7 us |
| 1-32 alpha / refracted, overlays, decals, sky dome, billboards, sky cube (r8 - r4) | 22 | 26 | +0.24 ms | ~9 us |
| 64 skinned actors (r5 - r4) | 39 | 39 | +0.29 ms | 7.4 us |
| 128 SpeedTrees + tree billboards (r10 - r4) | 120 | 351 | **+1.97 ms** | 5.6 us |
| 256 vegetation (r11 - r4) | 33 | 108 | +0.64 ms | 5.9 us |
| 512 shadow views' z ops (r12 - r4) | 89 (1.5 cascade views) | 97 | +0.60 ms | 6.2 us |
| 1024 + 2048 shadow mask chain + setup (r13 - r4) | 1 mask (9 draws) + setup | 9 | +0.19 ms | ~20 us |
| 4096 particles (r14 - r4) | 2 | 2 (~200 quads) | +0.12 ms | (CPU quads) |
| 32768 post chain (r15 - r4) | 19 units | 19 | +0.34 ms | 18 us |
| 65536 sprite batcher (r16 - r4) | 1 flush | ~2 | +0.03 ms | |
| **all (r2 - r1)** | | | **+5.70 ms** | |

The cost scales with the number of composed draws, not with the kind: the SpeedTree / vegetation / static / shadow paths all land at 5 - 6 us
per draw because they share the same issue machinery (constant composition, `StaticOps::issue`); the trees are the largest group only because
they have the most draws (instanced batches: ~2.9 draws per op). The effect units (post chain, mask) cost 18 - 20 us per draw (a full device
capture + apply per unit, `EffectOps::captureDevice` / `applyDevice`: every render state, 16 x 13 sampler states, all 480 float registers
compared one by one).

## 4. Buckets of the Full frame

Profiled Full frame 9.36 ms (unprofiled 8.52: scale by ~0.91). "Measured" = inclusive / self time from the profile or a bisection delta;
"[I]" = derived (subtraction of measured terms), with the reasoning.

| bucket | ms / frame | how obtained |
|---|---|---|
| non-render base shared with Wrapper (game logic `eCProcessor::Process`, culling / requests, cascade culling, frame, our inject passes, RT scene) | ~1.6 [I] | Wrapper 3.66 - its draw recording 1.05 - (a)+(b) ~1.0 |
| **(a) engine render code still running** (the slots' material / pass / light code, `GetPasses`, `SetConstants`, `ExecuteEffect`, the sprite batcher, `UpdateVertices`, Execute's loops + its qsort) | **~0.7 - 0.8** [I] | Wrapper: `eCFrustumDatabase::Execute` 1.63 incl - its draw recording (`HalPrimitive::Execute` 1.05 x ~0.88 main-view share) = ~0.7 for the main view incl. (b); shadow views ~0.1 (`RenderShadowMap` 0.32 incl - 97 draws' recording - the cascade Request); post / mask / GUI engine code ~0.2 (`ExecuteEffect` 0.18 + GUI). Engine.dll self is mode-invariant (1.23 / 1.32), so this is the same in both modes |
| **(b) the engine's HAL / device calls whose results are dropped** (SetRenderState / SetSamplerState / Set*ShaderConstantF / SetTexture / SetStreamSource through `HalDevice` -> `d3d9::Device`, the effects' parameter / preshader work) | **~0.2 - 0.3** [I] (part of the ~1.0 above) | Wrapper pc self of `HalDevice::set*`, `Device::set*`, `fx::` (lower bound 0.06 in the top-120 buckets) + their share of Execute's residual; not separable from (a) by the flat profile |
| (b') the engine's dropped draws (`HalPrimitive::Execute` -> `HalDevice::Draw` -> `drawIndexedPrimitive` -> `prepareDraw` start -> `FrameTap::onDraw` -> route + `StaticOps::engineDraw` + coverage -> return) | **~0.3** [I] | Full `prepareDraw` incl 0.94 vs ~0.71 for the same number of fully recorded draws (Wrapper) -> ~0.2 for ~900 dropped draws + their `HalPrimitive::Execute` part (below the cut, < 0.1) |
| **(c) + (f) op tap + attribution + coverage** (list snapshot at Execute, the second qsort with the engine's comparators, `resolve` per op, pass-through thunks, `Attribution` per op / draw, `Coverage` per draw, the reason strings) | **0.30** | r3 - r1 (wrapper + `native.frame_tap`); of it the duplicate qsort +0.02 (Engine.dll 0x4a4aa0 self 0.035 -> 0.057), `FrameTap::opEnter` self 0.05, coverage's u64 map ~0.01-0.03, `Attribution::begin` / `engineReason` ~0.01 each. Coverage alone: no switch, not separable; its hash map is <= 0.03 ms self in Full |
| **(d) composition** (the op's draw list, shaders, states, constants, samplers, geometry from engine data at the op's entry) | **~3.9** | `StaticOps::opEnter` 5.70 incl - `issue` 2.28 = 3.42 (all op groups, main + shadow views) + the effect units ~0.5 (post 0.34 + mask / setup 0.19 + GUI 0.03 incl their own issue) |
| - of it `constantsInto` (every live register of both shaders, per draw) | 1.02 (0.87 self) | inclusive; bit-by-bit `std::bitset<256> / <224>` copies and tests (`_Set_unchecked` / `_Subscript` ~0.35) + 16-byte copies per register |
| - per-op mirror re-read (`initMirror`: admin VS / PS pools, 201 global registers, lighting pool = 4.2 KB through `safeRead`; `mirror_.shaders.clear()` + one `ShaderPools` node per shader per op) | ~0.3 - 0.4 [I] | heap 0.25 ms total in ntdll (vs 0.03 Wrapper) is mostly these nodes and the per-op vectors (`composeTree`'s `inst` / `batches`, `DrawDesc::vertexBytes`) |
| - guarded engine reads (`get<T>` -> `readAt` -> out-of-line `safeRead` with `__try` -> a CRT memcpy call of a runtime size, per field) | ~0.4 self + their memcpy calls | `safeRead` 0.07, `get<u32>` 0.07, `get<ptr>` 0.06, `readAt<ptr>` 0.06, `readAt<u32>` 0.03, `get<u8>` 0.03, ... |
| - `HalDevice::textureOfApi` (mutex + `unordered_map<const void*, Texture*>` per sampler per draw) | ~0.12 | self 0.07 + FNV hashing 0.05 |
| **(e) issue path beyond recording** (`StaticOps::issue`: per-op save of the device state, per-draw setters, per-op restore) | **~1.3** | `issue` incl 2.28 - our draws' own `drawIndexedPrimitive` ~0.95 (the same recording the engine's draws get in Wrapper); self: `issue` 0.52, `setSamplerState` 0.18, `samplerState` 0.18, `setVertexShaderConstantF` 0.13, `slotOf` / `setTexture` / `texture` 0.09; hot lines: the per-register VS / PS loops (static_ops.cpp:1678 / 1681, 0.31), the restore's sampler loop (:1715) |
| - of it per-op save / restore (480 float registers read + written back whole = 15 KB of memcpy per op, 16 stages x 7 sampler states read + written, 16 textures, 4 streams, 10 render states) | ~0.6 [I] | `samplerState` is called only by the save (112 per op) and the composition; `setSamplerState` mostly by the restore (112 per op) + per-draw material samplers; ~500 ops x 15 KB = 7.5 MB memcpy per frame |
| - per-draw setters (one `setVertexShaderConstantF(r, v, 1)` per live register: a 16-byte CRT memcpy call each; samplers; streams) | ~0.7 [I] | |
| our own draws' recording (`drawIndexedPrimitive` of our draws: `prepareDraw`, vertex input, constants, packets, record threads) | ~0.95 | replaces the engine draws' recording in Wrapper (1.05): not extra |
| (g) leaf costs inside (d) / (e) | | **vcruntime140 0x1c010** (an unexported memmove-shaped copy routine: `mov rax,rcx; lea r10,[base]; cmp r8,0xf`) **0.97 ms** self (Wrapper 0.09): memcpy calls of runtime size from `safeRead`, `Device::setVertexShaderConstantF` / `setPixelShaderConstantF` (count x 16), `getVertexShaderConstantF`, the mirror reads, the own-VB copies; caller split not measurable (no stacks in the output). Heap 0.25. `RtlQueryPerformanceCounter` 0.13 (`-Perf` timers, also in Wrapper) |
| sum | 1.6 + 1.0 + 0.3 + 0.3 + 3.9 + 1.3 + 0.95 = 9.35 | vs 9.36 measured |

The ring grows from 7.9 to 36.2 MiB per frame in Full (our own CPU-geometry / instance buffers: group 63 +16 MiB (decals, billboards, sky
stars), 4096 / 126976 +9 MiB (particle quads), 256 +2.4 MiB (grass instances)); the CPU cost of those copies is small (group 63 adds 0.24 ms
in total) and the GPU time is unchanged.

## 5. Top functions in Full

**Our DLL, self** (r2; pc buckets symbolized with inlines; ms per frame, Wrapper beside it; the function-level self list adds
`prepareDraw` 0.148 (Wrapper 0.12), `computeVertexInput` 0.071, `uploadConstants` 0.045, `newDraw` 0.029, two unnamed pages
`d3d9+0xa63000` 0.085 / `+0x145000` 0.065):

| # | function | Full | Wrapper |
|---|---|---|---|
| 1 | `native::StaticOps::constantsInto` | 0.775 | 0 |
| 2 | `native::StaticOps::issue` | 0.421 | 0 |
| 3 | (unsymbolized: leaf code without unwind info) | 0.204 | 0.027 |
| 4 | `d3d9::Device::setSamplerState` | 0.181 | 0.008 |
| 5 | `d3d9::Device::samplerState` | 0.180 | 0 |
| 6 | `d3d9::Device::setVertexShaderConstantF` | 0.133 | 0.001 |
| 7 | `engine::safeRead` | 0.074 | 0 |
| 8 | `native::get<unsigned int>` | 0.072 | 0 |
| 9 | `native::get<const unsigned char*>` | 0.070 | 0 |
| 10 | `engine::readAt<const unsigned char*>` | 0.064 | 0 |
| 11 | `hal::vulkan::HalDevice::textureOfApi` | 0.061 | 0 |
| 12 | `std::_Fnv1a_append_value<const void*>` (pointer-key hashing: textureOfApi, the mirror's shader map) | 0.052 | 0 |
| 13 | `d3d9::Device::slotOf` | 0.036 | 0 |
| 14 | `engine::readAt<unsigned int>` | 0.034 | 0 |
| 15 | `native::get<unsigned char>` | 0.033 | 0 |
| 16 | `std::_Hash<u64, u64>` (coverage / attribution maps) | 0.029 | 0 |
| 17 | `d3d9::Device::setTexture` | 0.029 | 0.003 |
| 18 | `d3d9::Device::computeVertexInput` | 0.028 (0.071 function-level) | 0.051 |
| 19 | `d3d9::Device::texture` | 0.026 | 0 |
| 20 | `engine::readAt<unsigned char>` | 0.020 | 0 |
| 21 | `d3d9::Device::prepareDraw` | 0.019 (0.148 function-level) | 0.051 |
| 22 | `std::_Hash<const Shader*, StaticOps::Live>` (`liveOf`) | 0.019 | 0 |
| 23 | `native::EffectOps::applyDevice` | 0.018 (0.034 function-level) | 0 |
| 24 | `d3d9::Device::wantSecondaryScope` | 0.014 | 0.013 |
| 25 | `native::scaled` (static_ops_fx) | 0.014 | 0 |
| 26 | `native::get<int>` | 0.013 | 0 |
| 27 | `d3d9::Device::drawIndexedPrimitive` | 0.013 | 0.015 |
| 28 | `native::StaticOps::shaderOfElement` | 0.013 | 0 |
| 29 | `__security_check_cookie` | 0.012 | 0.002 |
| 30 | `d3d9::Device::setRenderState` | 0.011 | 0.001 |

**Our DLL, inclusive** (Full / Wrapper, ms): scene detours `detourEffectExecute` 8.17 / 2.59, `detourExecute` 6.43 / 1.63;
`FrameTap::opEnter` = `StaticOps::opEnter` = `opEnterImpl` **5.70 / 0**; `StaticOps::issue` 2.28; `materialDraws` 1.42; `drawIndexedPrimitive`
1.30 / 1.00; `passDraw` 1.26; `composeTree` 1.24 (its batch lambda 0.92); `constantsInto` 1.02; `prepareDraw` 0.94 / 0.71; the slot thunks
`contextSlot<7>` (SpeedTree Render) 1.67 / 0.35, `<8>` (SpeedTree RenderZPass) 1.01 / 0.23, `<0>` (Mesh Render) 0.99 / 0.24, `<3>` (Mesh
RenderZPass) 0.70 / < 0.2; shadows `detourShadowMapRender` 0.87 / 0.32. In g0 (static ops alone): `opEnter` 1.39, `issue` 0.53,
`composeSolid` 0.43, `materialDraws` 0.37.

**Engine.dll in Full.** Engine.dll's self total does not change with the mode (1.23 Wrapper, 1.29 tap, 1.24 - 1.33 in every Full group run,
1.32 Full), so the engine's functions are the same in Full as in Wrapper; each is <= 0.1 ms and in Full only 8 reach the top-30 self cut. Self
(Wrapper / tap / Full, ms): `bCSphere::Contains` (SharedBase) 0.061 / 0.067 / 0.096, `eCBeamTree::Cull(bCSphere)` 0.046 / 0.048 / 0.014*,
`qsort` 0x4a4aa0 (+0x5b) 0.035 / 0.057 / 0.070, `eCBeamTree::Cull(bCBox)` 0.040 / 0.040 / 0.008*, `bCSphere::Intersects(bCBox)` 0.035 / 0.036 /
-*, `eCVegetation_PS::Render` area 0x707875 0.031 / 0.038 / 0.009*, `eCFrustumDatabase::OnQueryNodes` 0.030 / 0.034 / -*, 0x6b5d20 0.028 /
0.025 / 0.017, 0x426160 0.029 / 0.018 / -, 0x427b19 0.028 / 0.017 / -, `bCPlane::ClassifyPoint` 0.016 / 0.022 / -, the CacheEntities walk
0x217570 0.011 / 0.019 / -, `FindWightedStaticLights` 0.020 / 0.008 / -, `eCFrustumDatabase` copy constructor - / 0.020 / -,
`FindPortalRoom` 0.008 each (* = below the cut in some Full windows, not less work). Inclusive (Full / Wrapper): `DoRender` 8.34 / 2.74,
`eCImageEffect::Execute` 8.15 / 2.58, `eCFrustumDatabase::Execute` 6.35 / 1.63 (Full: 5.70 of it is our `opEnter`), the scene filter's other
branch (camera Update, cascades) 1.29 / 0.73, `eCCameraBase::Update` 1.05 / 0.50, `RenderShadowMaps` 0.87 / 0.33,
`eCRenderSystemAdmin::RenderShadowMap` 0.40 / 0.26, `eCProcessor::Process` 0.42* / 0.73.

The engine-side render work (slots + material code + HAL calls) therefore cannot be read off per engine function in this profiler output;
its total is bounded by the Execute residual: Full `Execute` 6.35 - `opEnter` 5.70 = 0.65 ms (the engine's slot code + its HAL calls + the
dropped draws + the non-op paths' composition (decals, billboards, mask) + the tap's per-draw route), Wrapper `Execute` 1.63 -
recording ~0.92 = ~0.7 ms.

## 6. Ranked list: the double dipping and what removing it saves

"Saving" = estimated CPU per frame at this savegame (profiled ms; unprofiled ~0.9 x).

**The double work itself** (exists only because the engine's render code still runs for every op / unit):

| # | double-dipping cost | ms now | saving if removed | note |
|---|---|---|---|---|
| D1 | the engine's render code for the taken-over ops / units ((a)) + its HAL / device setter calls ((b)) | ~1.0 [I] | **~0.9 - 1.0** | needs the slots' side effects reproduced (README 7: RequestData / LRU stamps, BindLightmaps, pool switches / dirty ranges, `ms_pActiveShader`, the record's stream-3 flag, the instance sort, particle simulation / `UpdateVertices` RNG state) |
| D2 | the per-op device-state save / restore around our draws (15 KB memcpy of all float registers each way, 112 + 112 sampler-state calls, textures, streams) | ~0.6 [I] | **~0.6** | only needed so the engine's code after ours sees its own state; goes with D1 (or earlier: restore only what our draws changed, a dirty set instead of the whole state) |
| D3 | the engine's dropped draws (HalPrimitive::Execute -> prepareDraw start -> tap route -> drop) | ~0.3 [I] | **~0.3** | goes with D1 |
| D4 | the effect units' full device capture + apply + compare per unit (post chain, mask, GUI) | ~0.25 of the 0.56 the effect groups cost | ~0.2 | capture exists to start from the engine's state at the unit's entry |
| D5 | per-op mirror re-read of the engine's constant pools + rebuilt shader-pool map (heap) | ~0.3 - 0.4 [I] | ~0.3 | per view instead of per op once nothing of the engine writes the pools between ops (i.e. with D1; until then: invalidate per pool write) |
| D6 | op tap: second qsort of the lists with the engine's comparators | 0.02 | 0.02 | negligible; the snapshot itself stays (it is the op source) |
| D7 | coverage counter + attribution per draw (debug bookkeeping, always on in Full) | <= ~0.1 of the tap's 0.30 | ~0.1 | could be debug-only in the player mode |
| | **sum of the double work** | **~2.4 - 2.6** | **~2.4** (of 5.70 profiled; ~2.2 of 5.22 unprofiled) | |

**Our own per-draw cost** (stays after the double work is removed; ~3 - 3.5 ms; this is what makes a composed draw 5 - 6 us instead of the
wrapper's ~1.1 us):

| # | cost | ms now | estimated saving | how (not built) |
|---|---|---|---|---|
| O1 | `constantsInto`: every register of both shaders per draw, bit by bit (`std::bitset` single-bit set / test, 480 per draw) | 1.02 | ~0.7 - 0.8 | word-wise live masks (`vsLive = lv.f`), iterate set bits / precomputed live ranges per (VS, PS, material shader), copy runs |
| O2 | per-draw setters through the D3D9 device model (one `setVertexShaderConstantF(r, v, 1)` per live register = a CRT memcpy call each, sampler states one by one) | ~0.7 [I] | ~0.5 | write the composed block straight into the draw packet / one ranged copy per run of registers |
| O3 | guarded field reads (`get<T>` / `readAt` -> out-of-line `__try` `safeRead` -> runtime-size memcpy) | ~0.4 self + part of the 0.97 memcpy | ~0.3 - 0.5 | validate an object's pointer once per op, then plain reads (or an inlined fixed-size `safeRead<T>`) |
| O4 | heap per op (shader-pool map nodes, `composeTree`'s vectors, `DrawDesc::vertexBytes`) | 0.25 | ~0.2 | reuse buffers / flat maps (partly D5) |
| O5 | `textureOfApi` (mutex + hash map per sampler per draw) | ~0.12 | ~0.1 | cache our `Texture*` per engine image-data object per frame |
| O6 | trees: 351 draws per frame from 120 ops (2.9 draws per op, instanced batches) at ~5.6 us each | 1.97 | (the O1 - O5 savings apply per draw) | the biggest group only by its draw count; merged batches would also cut it |

Rough end state: removing D1 - D7 brings Full from 9.36 to ~7.0 ms profiled (~6.4 unprofiled); O1 - O5 another ~2 ms, to ~5 ms profiled
(~4.5 unprofiled), still ~1.2 ms above Wrapper because the composition reads engine data per draw that the engine had computed anyway. The
engine's code at the slots is 1.0 ms at most, so "remove the engine's double work" alone cannot win back the +5.4 ms; the composition's own
cost per draw is the larger lever.

## 7. Open / not measured

* Caller attribution of the CRT copy routine (vcruntime140 0x1c010, 0.97 ms) and of the heap calls: the profiler output has no stacks;
  the split between `safeRead`, the per-register constant setters, the save / restore and the mirror reads is inferred.
* (a) vs (b) inside the engine-side ~1.0 ms are not separable from a flat profile; the 0.7 ms main-view residual includes the engine's own
  qsort and Execute's loops (which stay as long as Execute runs).
* The coverage counter alone (no switch): bounded by the tap run (0.30 ms for tap + attribution + coverage + thunks).
* Only the savegame's scene (lod 1x, 909 draws): at higher object distances the per-draw buckets (O1 - O5, D2, D3) scale with the draws.
* A tracing profiler with call stacks (ETW needs an elevated shell; not available in this session) or a caller-tree mode of
  `diag::frameSampler` would close the [I] rows.
