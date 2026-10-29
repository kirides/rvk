# V0: the current draw path, its traffic and its CPU cost (2026-10-28)

Baseline facts for the "vulkanize" project (mesh arenas, material data in GPU tables, retained per-frame data, minimal CPU -> GPU
traffic, merged indirect draws). Scope: every CPU step from a composed `sc::DrawDesc` (Full) or an engine HAL draw (Wrapper) to the
recorded Vulkan commands, what it writes into GPU-visible memory, what it costs on the engine thread, and how much of a composed
draw's data repeats from frame to frame. Design is not part of this doc. Branch `feat/native-scene`, HEAD cf5c636 (the DLL built
at 6f7aee4 = the last code commit). Sibling docs: `V0-constants.md` (the register classes), `V0-gpu-resident.md` (blockers, device
caps, consumers).

Marks: **[M]** measured (a log line of the runs below), **[D]** derived (arithmetic on [M] / [C]), **[C]** read in the code
(file:line at cf5c636), **[E]** estimate / judgement, **[I]** inferred.

## 0. Runs and method

| run | what | log (git-ignored `out/vk0/`) |
|---|---|---|
| F_prof / W_prof | Full / Wrapper, `-Exe RisenTest.exe -Vsync off -Perf -CpuProfile`, `Risen_Save_0009`, `rvk_perfsave hold=40`, test baseline (1600x900, 1x detail), shipped build (`build\RelWithDebInfo`, = 6f7aee4); machine idle (`sysprobe`: 42 GiB free, no other load) | `F_prof.log`, `W_prof.log` (+ the DLLs for symbols) |
| Fi / Wi / Fi2 | the same savegame without the profiler, a **temporary probe build** (worktree at cf5c636 + `out/vk0/probe.diff`: atomic counters in `applyPacket` / `applyVertexInput` / `applyDynamicState`, `uploadConstants`, the DrawBlock, `Buffer::gpu`, the register setters; a churn tracker in `StaticOps::issue`; Fi2 adds per-op-kind tallies + value samples). Worktree removed afterwards | `Fi_noprof.log`, `Wi_noprof.log`, `Fi2_noprof.log` |

The probe slows the frame (Full 7.4 ms, Wrapper 3.65 ms: the churn tracker hashes / copies every composed draw), so **its counts are
used, never its times**; times come from F_prof / W_prof (profiled; profiler overhead ~0.2-0.4 ms) and the O3 unprofiled runs
(opt-plan.md "O2c / O3 result": Wrapper 3.36, Full 4.76 ms). Profiles are of the **engine thread only** (`diag: profile`): the two
record threads that record ~94 % of the packets (section 2.2) are not in them. All numbers are per frame, averaged over the hold's
last 6 windows; the hold's camera is static ("moved in 0 of 990 frames"), sun cascades update every 2nd frame
(`debug.shadow_stride=2`): frames alternate 817 / 1,000 draws (`hal: frame ... structure`), 909 on average.

Frame at the savegame (Full = Wrapper, [M]): `SkyCube{} Post{Shadow{ShadowDepth(37)} Shadow{(40)} Shadow{(106)} Main{DepthPrepass(260)
ShadowMask(9) Solid(506) Refracted(2) Alpha(20)} Post(19)} Gui{Gui(1)}`; draw kinds: SpeedTree 325, StaticMesh 184, Vegetation 112,
InstancedMesh 92, Skinned 38, SpeedTreeBillboard 28, Post 19, ShadowMask 9, Sky 4, Refracted 2, Particle 2, Decal 1, Gui 1 (main view).

---

## 1. Anatomy

### 1.1 Full: from the engine's op to a `DrawPacket` [C]

| # | step | where | per | what it does / copies |
|---|---|---|---|---|
| F1 | engine `Execute` / `ExecuteShadowMap` walks its sorted op lists, calls the slot | `engine/scene_hooks.cpp` `detourExecute`, slot thunks `contextSlot<I>` :552 | op | draw tag scope, pass scope, `OpTap::opEnter`; a lean op returns before the engine's slot code |
| F2 | frame tap + attribution | `native/frame_tap.cpp:624` `FrameTap::opEnter` | op | op bookkeeping (next-op cursor, refraction copy) then `StaticOps::opEnter` |
| F3 | group selection, entry snapshot | `native/static_ops.cpp:1376` `opEnter`, `:1402` `opEnterImpl` | op | picks the group (static / mesh / actors / trees / vegetation / shadow views / fx / sky ...); copies the device's bool + int registers (640 B) for the op (`:1541-1544`); `leanBegin` |
| F4 | composition | `composeOp` `:1326` -> `composeZ` `:1148` / `composeSolid` `:1237` / `composeActor` (static_ops_actors.cpp:139) / `composeTree` (static_ops_trees.cpp:181) / `composeVegetation` (:447) / `composeParticle` (static_ops_fx.cpp:752) / `composeSky` (static_ops_sky.cpp:263) / decals, billboards (static_ops_nonop.cpp:146 / 464) | op -> draws | reads the engine's objects through SEH-guarded loads (`engine::readAt` / `safeLoad1..8`), walks material passes (`materialDraws` :1049, `passDraw` :916, `specialPassDraw` :862), compiles a missing permutation at the slot's place (O2c) |
| F5 | one `sc::DrawDesc` per draw | `newDraw` :419, `drawFrom` :712 | draw | DrawDesc = ~10.6 KB object (vsF[256][4], psF[224][4], Src arrays, samplers, bitsets; static_compose.h:303); `newDraw` resets ~1.1 KB (Src arrays, masks) and copies the entry int / bool registers |
| F6 | constants into the DrawDesc | `constantsInto` :732 | draw | per shader the live register masks (`liveOf`, a hash map), then runs of live registers copied from our register file `mirror_` (admin VS c0..c30 (VP / depth-bias VP copied in per draw, :742), global pool c31..c231, the shader's pools, PS admin c0..c5, lighting c14..c37): **132.6 VS + 11.6 PS registers per draw [M]** = 2.1 KB |
| F7 | samplers / textures | `materialSamplers` :816, `adminSamplers` :976, `samplerSnap` :804, `textureOfImageData` :386 (4,096-entry cache) | draw | the 7 sampler states + texture per sampled stage |
| F8 | lean side effects | `leanCommit` static_ops_lean.cpp:329 | op | resource touches (2,222 per frame, 247 `RequestData` calls), lightmaps, stream 3, tree queue sorts, palettes, particle timers, pool write-backs [M: `native: lean (5 s)`] |
| F9 | issue: save | `issue` :1774-1811 | op | union of the op's composed register ranges + sampled stages saved (`savedVs_` / `savedPs_`) |
| F10 | issue: set | :1816-1881 | draw | shaders, declaration, 4 streams, indices, 10 owned render states, per stage texture + 7 sampler states, **the composed registers as runs** (`setVertexShaderConstantF` / `PS`: memcpy into `Device::vsF_ / psF_`, marks the stage dirty, device.cpp:1666-1677); CPU-generated geometry (decals, billboards, sky stars, particles, rain, vegetation instance data) locked into our own dynamic buffers (`ownVb_` 4 MiB, `ownIb_` 64 KiB, `ownInstVb_` 2 MiB; static_ops.h:644-646) with DISCARD / NOOVERWRITE (:1827-1872) |
| F11 | the device draw | `Device::drawIndexedPrimitive` (d3d9/draw.cpp:1258) / `drawPrimitive` (:1219) | draw | section 1.3 |
| F12 | issue: restore | :1901-1920 | op | the saved registers / stages / states written back (the engine's caches must stay true for engine code that still runs) |
| F13 | effect units (post_SSM mask, SSAO / blur chains, post chain, overlay, GUI sprites, PSSM) | `EffectOps::effectBegin` (native/effect_ops.cpp:502) -> `composeEffect` :605 -> `issueLean` (effect_ops_lean.cpp:125) / `issue` :869 -> `applyDevice` :294; sprites `issueSprite` (effect_ops_gui.cpp:314) | unit (~25) / draw (~28) | `applyDevice` diffs the whole register files (256 VS + 224 PS registers memcmp), 16 stages x 13 sampler states, 210 render states, targets, viewport, then writes the differing ranges; sprite vertices DISCARD-locked into `spriteVb_` (448 KiB) per draw |

### 1.2 Wrapper: from the engine's HAL calls to a `DrawPacket` [C]

| # | step | where | per |
|---|---|---|---|
| W1 | the engine's slot code: materials, `ExecutePass`, `eCGfxShader` pools -> HAL setters | Engine.dll | op / pass |
| W2 | HAL constant setters -> device register file | `HalDevice::SetVertexShaderConstantF` (hal/vulkan/device.cpp:1921) / PS (:2039); effects: the pass's float registers 0..n-1 (hal/vulkan/effect.cpp:397-401) | call |
| W3 | HAL states, textures, samplers -> `d3d9::Device` setters | hal/vulkan/device.cpp, effect.cpp | call |
| W4 | `HalPrimitive::Execute` (streams, indices, frequency) -> `HalDevice::Draw` | hal/vulkan/resources.cpp:452, device.cpp:1880 | draw |
| W5 | `Device::drawIndexedPrimitive` -> the shared device path | d3d9/draw.cpp:1258 | draw |

Measured difference in the setter traffic (probe): Wrapper `setVertexShaderConstantF` 920 calls / 10,140 registers per frame, PS 728 /
5,264; **Full 4,101 calls / 166,973 registers, PS 3,065 / 13,863** [M] (the composition writes every live register of every draw,
then the restore writes the union back).

### 1.3 The shared device path: `Device::prepareDraw` .. `vkCmd*` [C]

| # | step | d3d9/draw.cpp | per draw: what / how much |
|---|---|---|---|
| D1 | `DrawTimer` (perf only), frame tap note (`drawTap_`: N5 compare / tap only) | :1260, :640-667 | `steady_clock::now` x2 with `-Perf` |
| D2 | phase / kind | :674-680 `phases_.current()`, `syncPhaseRegions`, `draws::classify` | debug-label sync, BeforeAlpha injection check |
| D3 | observers (`enh/rt_scene`, `enh/gi`, `enh/object_motion`, `enh/native_parts`) | :721-730 | builds a `ReplaceQuery` per interested observer; **none active at the test baseline** (water / shadows / GI / upscaling off) [C+M: no `rt: scene` lines] |
| D4 | replacers (`enh/water`, `enh/shadows`, `enh/gbuffer`, `enh/native_opaque`) | :731-773 | `handles()` per replacer; none registered at the test baseline [I] |
| D5 | texture readiness | `prepareTextures` :161 (called :822) | per bound sampler slot: layout / pending check, upload if needed |
| D6 | rendering scope | `beginRendering` :60 (:825); G-buffer / secondary-scope switch :824 | dynamic rendering begin when the targets change (34.5 render passes / frame [M]) |
| D7 | shader variants | :833-987 | tint, ubershader, LOD billboard, capture / G-buffer / GI variants: `findPipeline` probes per variant (all off at the baseline) |
| D8 | `vk::PipelineDesc` (vk/pipeline.h:55, ~520 B) | :874-893 | modules, topology class, colour / depth formats, blend (without EDS3) |
| D9 | vertex input | `computeVertexInput` :262 (`vertexPlan` :192 per (decl, shader), cached) | buffers + offsets from `Buffer::gpu` (d3d9/resources.cpp:419: static = mesh arena binding, dynamic = ring copy, below), the single per-vertex stream at offset 0 + `vertexShift_` (mesh arena), the layout as dynamic state (`VK_EXT_vertex_input_dynamic_state`), the pipeline key without it |
| D10 | pipeline lookup | :988-1017; `findPipeline` :251 (256-entry direct-mapped cache, 520-B memcmp), `PipelineCache::get` | memcmp against the last draw's desc first |
| D11 | dynamic state values | `computeDynamicState` :440 | viewport, scissor, cull, topology, depth / stencil, bias, blend constants, EDS3 blend enable / equation / write mask per attachment (~150 B) |
| D12 | **stage constants** | `uploadConstants` :560-581 | per dirty stage: `Frames::allocate(4,416)` from the ring + memcpy of `floatConstCount x 16` B (highest read register + 1; 256 with relative addressing) + 256 B ints + 64 B bools. **Not dirty ranges: the whole prefix**, into the ring's mapped device-local memory (`VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE` + `HOST_ACCESS_SEQUENTIAL_WRITE`, vk/frames.cpp:189-221: write-combined ReBAR) |
| D13 | **DrawBlock** (192 B, shader/abi.h:66) | :1022-1062 | per sampled slot: bindless texture slot (`Texture::bindlessSlot`) + sampler index (`samplerIndex`, device.cpp:2275: per-slot cache + 2-entry memo, then `lookupSamplerStates` :2372 = mutex + hash map); posFix (half pixel + jitter + reverse-Z remap), alpha test; `Frames::allocate(192)` + memcpy |
| D14 | push constants (48 B, shader/abi.h:76) | :1086 | VS / PS constant addresses, DrawBlock address, FrameBlock address (`frameBlockAddress`, device.cpp:135: 976 B, 2.5 per frame), capture slot, draw id, flags |
| D15 | batchable? | :1090-1101 | mesh kinds in Main z / solid and Shadow depth, opaque, depth-tested, no stencil |
| D16 | draw record (`draws::Record`, ~300 B CPU, draws.h:101) | :1107-1153 | tag, geometry, shader hashes, ring addresses, 16 texture ids (59 ns per draw [M: `perf: draw tags + records`]) |
| D17 | index binding | `drawIndexedPrimitive` :1276-1291, `foldIndexOffset` :1239 | `Buffer::gpu` of the index buffer, offset folded into `firstIndex` |
| D18 | `submitPacket` :1347 | interns `VertexInputValues` / `DynamicStateValues` against the previous draw's (`internVi` :1322 / `internDyn` :1335: memcmp, arena push on change); batchable -> `batch_` (flush at 160 packets with record threads); else `flushBatch` then inline / chunk |
| D19 | `flushBatch` :1389 | per batch | stable group-by-pipeline (open-addressing table + one sort of 64-bit keys), each packet to `emitPacket` (record.cpp:214) or `applyPacket` |
| D20 | record threads | record.cpp: `wantSecondaryScope` :41 (Main z / solid, Shadow depth), `emitPacket` :214 (copy into a 160-packet chunk), `recordChunk` :150 (secondary command buffer, `applyPacket` :182), `vkCmdExecuteCommands` :277 | 2 workers |
| D21 | `applyPacket` :1365 | per packet, per command buffer cache (`RecordState`) | `applyVertexInput` :352 (bind VB / set vertex input on change), `vkCmdBindPipeline` on change, `applyDynamicState` :503 (each state on change; front face / primitive restart once per command buffer), `vkCmdPushConstants` (always), `bindIndexBuffer` :1250 (on change), `vkCmdDrawIndexed` / `vkCmdDraw` |

Dynamic / CPU-written buffers [C]: `Buffer::lock` copies into the buffer's CPU copy (resources.cpp:400); at the next GPU use
`Buffer::gpu` (:419) plans by `BufferCopyState` (identity.cpp:15-80): a dynamic buffer after a DISCARD or in a new frame gets a
**fresh ring allocation of the whole buffer size** (`frames().allocate(size_)`, :429) and only the written range is copied;
NOOVERWRITE appends patch the same copy. Static buffers live in `vk::MeshArena` (64 MiB device-local blocks, a staging copy through the
ring + `vkCmdCopyBuffer` on change, :446-475).

---

## 2. Traffic per frame at the savegame

### 2.1 Bytes into GPU-visible memory (the frame ring) [M: probe Fi / Wi, `perf:` lines]

| category | Full allocated | Full written | Wrapper allocated | Wrapper written | note |
|---|---|---|---|---|---|
| VS stage constants | 909 x 4,416 B = 3.92 MiB | **2,752 KiB** (floats 2,468 + int / bool 284) | 809 x 4,416 = 3.41 MiB | **2,685 KiB** | 3.03 KiB per upload = ~190 float registers on average (`floatConstCount` prefix, not the live set); Full uploads for **every** draw (setters always dirty), 220 of them byte-identical to the previous VS upload |
| PS stage constants | 904 x 4,416 = 3.90 MiB | **532 KiB** (floats 250) | 697 x 4,416 = 2.94 MiB | **444 KiB** | 361 (Full) / 153 (Wrapper) identical to the previous PS upload; ints / bools are always zero (V0-constants.md 0.3): 0.57 MB of the written bytes per frame are zeros [D] |
| DrawBlocks | 910 x 192 = 171 KiB | 171 KiB | 914 x 192 = 171 KiB | 171 KiB | 713 / 716 identical to the previous draw's block |
| FrameBlock | 2.5 x 976 B | 2.4 KiB | same | 2.4 KiB | |
| dynamic buffers, new copies | 14 copies, **27,428 KiB** | 320 KiB | 14 copies, 1,181 KiB | 527 KiB | Full: our own 4 MiB / 448 KiB / 2 MiB buffers DISCARD-locked per draw -> a whole-size ring block each [C+M] |
| dynamic buffers, patches | 111 | 221 KiB | 106 | 175 KiB | vegetation instance appends (Full), the engine's sprite / vegetation buffers (Wrapper) |
| static buffers (mesh arena staging) | 0 | 0 | 1 | 0.1 KiB | steady state |
| user-pointer draws | 0 | 0 | 0 | 0 | |
| **ring total** (`perf: ring`) | **35,419 KiB** | **~4.0 MB** | **7,850 KiB** | **~4.0 MB** | `perf: buffer uploads` 541 / 702 KiB = dynamic written + patches [M] |

Consequences [M/D]: (a) GPU-visible bytes **written** are the same in both modes (~4 MB/frame; at 200 fps ~0.8 GB/s), 82 % of it
stage constants, and 83 % of that is the VS prefix copy; (b) Full's ring **footprint** is 4.5x Wrapper's purely from whole-buffer
allocations of our own dynamic buffers (27.4 MiB allocated for 0.32 MiB written): GPU memory in allocations 695 vs 600 MiB [M:
`perf: GPU memory`]; a range-sized ring copy (or NOOVERWRITE appends) would remove it independently of vulkanize [E]; (c) the
live data in those 3.3 MB of constants is ~2.3 KB per composed draw (132.6 + 11.6 registers [M]) = ~2.0 MB, of which ~0.3-0.4 MB is
not carry-over / duplicated per-frame data (section 4, V0-constants.md 0.4) [D/E].

CPU-generated vertex data in Full: **532 KiB per frame** in `DrawDesc::vertexBytes / indexBytes` [M] (particles 2 ops / ~180 quads,
decal, sky objects, rain, vegetation instance data), copied three times: composition -> DrawDesc vector -> buffer CPU copy (`lock`)
-> ring (`Buffer::gpu`) [C].

CPU-side constant copies per frame (not GPU-visible, memcpy work) [D from M counts]:

| copy | Full | Wrapper |
|---|---|---|
| mirror -> DrawDesc (`constantsInto`) | 881 x (132.6 + 11.6) x 16 B = 2.03 MB | - (engine-side) |
| `newDraw` resets + int / bool copies | ~881 x 1.1 KB = 1.0 MB | - |
| issue save (union) | ~0.8 MB (= the restore) | - |
| setters -> `Device::vsF_ / psF_` | (166,973 + 13,863) x 16 = 2.89 MB | (10,140 + 5,264) x 16 = 0.25 MB |
| register file -> ring (`uploadConstants`, WC memory) | 3.28 MB | 3.13 MB |
| effect units: `applyDevice` diff (480 registers memcmp per draw) | 28 x 7.7 KB = 0.2 MB read | - |
| **total** | **~10 MB** | **~3.4 MB** |

That matches the profile's vcruntime (memcpy / memmove) delta: 0.55 vs 0.22 ms [M] (+0.33 ms for ~6.6 MB more copies).

### 2.2 Commands per frame [M: probe; Full / Wrapper within 1 %]

| command | Full | Wrapper | note |
|---|---|---|---|
| `vkCmdDrawIndexed` | 910 | 914 | `vkCmdDraw` 0 (no non-indexed draw at the savegame) |
| `vkCmdPushConstants` (48 B) | 910 | 914 | one per draw, always |
| `vkCmdBindPipeline` | 220 | 222 | after batching (622 of 909 draws batched in 8.5 batches) |
| `vkCmdBindVertexBuffers` | 172 | 171 | the mesh arena keeps most bindings |
| `vkCmdSetVertexInputEXT` | 85 | 84 | |
| `vkCmdBindIndexBuffer` | 19.5 | 19.5 | arena: offsets folded into `firstIndex` |
| dynamic state, total | ~400 | ~395 | viewport 24.5, scissor 24.5, cull 63.6, topology 23, depth test 13.5 / write 20.5 / compare 25.5, stencil test 14.5 / op 31 / masks + ref 37.6, depth bias enable 12.5 / bias 12.5, blend constants 12.5, colour blend enable 17 / equation 16 / write mask 14, front face + primitive restart 2 x 12.5 (once per command buffer) |
| packets recorded on the record threads | **859 (94 %)** | 863 | Main z / solid + Shadow depth scopes; the engine thread records ~51 inline (post, GUI, alpha, sky, mask) + `vkCmdExecuteCommands` |
| **vkCmd* total** | **~2,720** | ~2,720 | 3.0 per draw; render passes 34.5 (`vkCmdBeginRendering`), secondary command buffers ~12.5 |

Draw recording is therefore not Full's problem: both modes issue the same commands; the per-draw driver work runs mostly on the
record threads (`amdvlk64` on the engine thread: 0.23 vs 0.20 ms [M]).

---

## 3. CPU cost per step (engine thread, profiled) [M: F_prof / W_prof, `out/o2b/table.py` with `RVK_DLL`, last 6 windows]

Totals: engine+HAL **Full 5.17 ms vs Wrapper 3.60 ms** (+1.57; unprofiled O3: 4.76 vs 3.36, +1.40). By module (self ms): ours
(d3d9.dll) 2.35 / 0.87, Engine.dll 0.93 / 1.21, vcruntime140 0.55 / 0.22, ntdll 0.42 / 0.42, SharedBase 0.39 / 0.40, amdvlk64
0.23 / 0.20, Game.dll 0.12 / 0.13. Full - Wrapper = ours +1.48, memcpy +0.33, Engine.dll -0.28.

### 3.1 Full, inclusive per step (909 draws, 881 composed by `StaticOps`, ~505 ops, ~28 effect / sprite draws)

| step | ms / frame | us per draw | us per op | how |
|---|---|---|---|---|
| `detourExecute` (engine Execute shell + brackets + our slot path) | 2.88 | | | [M] |
| `StaticOps::opEnter` (F2-F12) | **2.62** | 2.97 | 5.2 | [M] |
| . composition = opEnter - issue (F3-F8: group, compose, constants, samplers, lean side effects) | **1.43** | **1.62** | 2.8 | [D]; of it `materialDraws` 0.41 incl, `composeTree` 0.31, `passDraw` 0.24 [M]; self: SEH loads `safeLoad1/2/4/8` 0.17, `textureOfImageData` 0.03, `newDraw` 0.024, ShaderPools / Live hash maps 0.034, `initMirror` 0.015, `skyBillboardVertices` 0.02, `constantsInto` + its run lambdas ~0.03 [M pc buckets] |
| . `issue` (F9-F12 + the device draws of 881 draws) | **1.19** | 1.35 | | [M] |
| . . issue's own part (save / set / restore, own-buffer locks) | ~0.14 | ~0.16 | | [D] = 1.19 - 881/909 x 1.08 |
| . . device draw (`drawIndexedPrimitive`, D1-D21 on the engine thread) | ~1.05 | **1.19** | | [D] |
| `Device::drawIndexedPrimitive` (all 909 draws) | 1.08 | 1.19 | | [M] |
| . `prepareDraw` (D2-D16) | 0.77 | 0.85 | | [M]; self 0.13: `computeVertexInput` 0.05-0.07, `prepareDraw` body 0.045, `uploadConstants` 0.02-0.04 (+ its memcpy in vcruntime), `Frames::allocate` 0.016, `prepareTextures` 0.013, `steady_clock::now` 0.015 (perf timers), `boundDepthReversed` 0.008, `samplerIndex` 0.007, `findPipeline` 0.006, `internVi` 0.008 [M pc buckets] |
| . submit / batch / inline apply + driver (D17-D21 on this thread) | 0.31 | 0.34 | | [D]; the threaded packets' `applyPacket` runs on the workers |
| `EffectOps::effectBegin` (F13, ~25 units) | **0.41** | ~14.6 per effect draw | ~16 per unit | [M]; `applyDevice` 0.008 self, `composeEffect` 0.02 self |
| `detourShadowMapRender` (cascades: engine fit / setup + our composed ops) | 0.46 | | | [M], overlaps opEnter |
| memcpy / memmove (vcruntime, all callers) | 0.55 | 0.60 | | [M]; callers not separable in the flat profile; byte budget in 2.1 |

### 3.2 Wrapper for comparison

| step | ms | us per draw |
|---|---|---|
| `HalPrimitive::Execute` incl | 1.03 | 1.13 |
| `HalDevice::Draw` / `drawIndexedPrimitive` incl | 0.99 | 1.09 |
| `prepareDraw` incl | 0.70 | 0.77 |
| engine slot code + HAL setters (Engine.dll self 1.21 + our HAL handlers) | ~1.4 | |

So per composed draw Full spends **1.62 us composing + 0.16 us issuing + 1.19 us in the device** (Wrapper: the engine's slot code
instead of the composition + 1.09 us in the device). The device part is the same in both modes (+0.1 us in Full: every draw uploads
both stages). **What Full adds over Wrapper is the composition (1.43 ms) + effect units (0.41) + the extra copies (0.33), minus
the engine's skipped slot code (-0.28) and HAL handlers** [D].

---

## 4. Per-draw data churn of the composed draws (Full) [M: probe Fi / Fi2, 11 windows, stable to +-0.5 %]

Key per draw = (op context, op desc, list, view kind, cascade, draw index k in the op, vs, ps, sub-mesh, pass flags, occurrence in the
frame); compared with the last issue of the same key (average gap 1.12 frames: shadow-view draws come every 2nd frame). Static camera.

| of 881 composed draws per frame | % |
|---|---|
| key not seen before (new draw) | 0.0 |
| **identical** to the last issue (constants, states, samplers, streams, geometry, CPU bytes) | **16.5** |
| **only constants differ** | **78.0** |
| VS constants differ | 68-70 |
| PS constants differ | 59.5 |
| render states / bool / int differ | **0.0** |
| sampler textures / states differ | **0.0** |
| streams / declaration / index buffer differ | 5.3 (vegetation's instance stream offset in `ownInstVb_`, particles) |
| draw ranges (prim type, base vertex, index range, counts) differ | **0.0** |
| CPU vertex / index bytes differ | 1.0 (particles, rain) |

Per draw: 132.6 VS / 11.6 PS registers set, **50.8 VS / 6.5 PS changed** per frame.

By op kind (per frame; Fi2) [M]:

| op kind (view) | draws | identical | VS differ | PS differ | VS regs set | VS regs changed |
|---|---|---|---|---|---|---|
| mesh (main) | 278 | 36 % | 27 % | 55 % | 67.5 | 8.5 |
| mesh (shadow) | 32.5 | 95 % | 5 % | 0 % | 8.0 | 0.1 |
| actor (main) | 38 | 0 % | 100 % | 50 % | 205 | 96 |
| actor (shadow) | 11 | 0 % | 100 % | 0 % | 204 | 95 |
| SpeedTree (main) | 325 | 0 % | 100 % | 74 % | 217 | 101 |
| SpeedTree (shadow) | 49.5 | 0 % | 100 % | 0 % | 216 | 100 |
| tree billboard | 28 | 50 % | 4 % | 50 % | 205 | 2.0 |
| vegetation | 112 | 0 % | 100 % | 83 % | 7.0 | 1.0 |
| particle | 2 | 0 % | 100 % | 0 % | 10 | 1.0 |
| sky | 4 | 0 % | 100 % | 50 % | 12.5 | 3.5 |

What changes (value samples of Fi2 + code) [M/C/I]:

* **PS c4, c15..c24: changed in 100 % of the draws that read them, every frame** [M]. c15..c21 = the ambient SH, c22..c24 the light
  list's sun block, both written by `writeLighting` (static_ops.cpp:603-623) from the engine's light list; they follow the weather /
  game time [C/I]. The same value for every draw of a view: a per-view datum duplicated into ~560 PS uploads (the 55-83 % PS rows).
* **Actors: their bone palette (c33 + 3b, ~96 registers) changes every frame** [M samples: c33 / c36 / c40 / c100 / c130 of skinned
  draws move by ~1e-4 per frame]: real per-object data, ~49 draws x 96 x 16 B = 75 KB per frame.
* **SpeedTrees and instanced meshes: the "changes" are register-file carry-over, not their data** [M samples]: a SpeedTree draw's
  c36 = (0.001, 0.005, **-0.19165856**, 0.952) where .xy / .w are its instance data and .z equals the preceding actor's bone register
  c36.z of the same frame; its c100 / c130 equal the actor's c100 / c130 whole. The composition reproduces the D3D9 register file
  exactly (the shader's CTAB declares the whole instance array c32..c231; slots beyond the batch's instance count keep whatever the
  last writer, usually an actor's palette, left there: opt-plan.md "The register file and O2"); those registers are read by no
  vertex of the draw. With per-instance data in its own buffer they disappear: no per-frame churn remains for a static tree batch
  except the wind (admin VS c0..c30, per op) [I].
* **Static meshes (main)**: 36 % identical; the rest differ in the lighting registers (PS) and in 8.5 VS registers per draw (instanced
  batches' carry-over slots, as above) [M/I].
* **Shadow-view meshes: 95 % identical** (no lighting, VP of the cascade unchanged while the camera and the sun stand).
* **Vegetation**: 1 of its 7 VS registers changes per frame (the wind / time register, [I]) and its instance data (stream 1, CPU
  composed every frame) lands at a new offset of our instance buffer (`ownInstVb_`) [M/C].
* Not covered by the probe: the effect units (~28 draws; their constants are per-frame post parameters, eye adaptation, jitter).

**With a moving camera** [D/C]: VS c0..c3 (view-projection; the depth-bias VP for UseDepthBias materials), c4..c7 (inverse
projection: only on FOV changes), c8..c11 (inverse view, eye position), PS c5.xy / fog (c30) change for every draw of the view:
"identical" drops to ~0 % and every draw's VS block changes, while the per-object part stays as above. These are 1 VIEW block per
view (V0-constants.md 2.0), today duplicated into every draw's 3 KiB prefix.

So a retained design needs per frame, at the savegame: 1 block per view (camera, fog, lighting SH / sun: <1 KB each, 1 main + 3
cascades), the actors' palettes (~75 KB), particle / rain / vegetation CPU data (~0.5 MB, or GPU-generated), the instance data of
moving / changed objects (none in this hold) and the visibility list; states, samplers, textures, geometry ranges and material
constants did not change for a single draw in 990 frames [M].

---

## 5. Ranked: where the CPU and the traffic go, and what a GPU-resident design removes

| rank | item | today (savegame, per frame) | GPU-resident design | removes (estimate) |
|---|---|---|---|---|
| 1 | **composition** of every draw every frame (F3-F8) | 1.43 ms engine thread (1.62 us per draw) [M]; 0 % of states / samplers / geometry change between frames [M] | compose once per (op, draw) when the engine object's identity / generation changes; per frame only visibility + the dynamic slots (bones, wind, fade, CPU geometry) | **~1.0-1.2 ms** [E] (change detection per op ~0.2-0.4 us x 505 ops = 0.1-0.2 ms remains; the lean side effects (touches, palettes, sorts) stay: they are gameplay-visible) |
| 2 | **device draw path** (prepareDraw: kind, pipeline key + lookup, vertex input, dynamic state values, constants, DrawBlock, record, packet) | 1.08 ms engine thread (1.19 us per draw) in both modes [M]; + record-thread `applyPacket` for 859 packets (not profiled) | persistent draw records; per pipeline group one `vkCmdDrawIndexedIndirect(Count)` over a GPU-written command list (220 pipeline groups today; fewer with merged material / vertex-pulling pipelines) | **~0.8-0.9 ms** in Full [E] (per group ~1-2 us remains: ~0.2-0.4 ms); Wrapper unchanged (the engine drives its draws) |
| 3 | **constant copies** (mirror -> DrawDesc -> device register file -> ring, + save / restore) | ~10 MB CPU copies (vcruntime 0.55 ms vs 0.22) [M/D]; GPU-visible 3.28 MB written / 7.8 MiB allocated per frame, of it 0.57 MB int / bool zeros, every draw both stages [M] | VIEW blocks once per view, MATERIAL tables persistent (dirty-tracked), OBJECT / instance data persistent (bones and moved objects per frame), no int / bool, no carry-over registers | ~0.3-0.4 ms CPU (memcpy + setters + save / restore + uploadConstants) [E]; GPU writes 3.3 MB -> ~0.1-0.2 MB + the CPU geometry [E] |
| 4 | **effect units** (post_SSM mask, chains, post, GUI, PSSM: ~25 units, ~28 draws) | 0.41 ms (~16 us per unit) [M] | few draws, keep on the legacy StageConsts path; retained composition per unit (parameters change every frame, structure does not) | ~0.2-0.3 ms [E]; low priority per draw count, high per draw |
| 5 | **dynamic-buffer ring footprint** | 27.4 MiB allocated for 0.32 MiB written (whole-buffer ring blocks per DISCARD lock of `ownVb_` 4 MiB / `spriteVb_` 448 KiB / `ownInstVb_` 2 MiB) [M]; +95 MiB GPU memory in allocations | ring blocks sized to the written range, or append-only streams | memory, not CPU (allocation is a pointer bump) [C]; fixable today without the redesign [E] |
| 6 | **CPU-generated geometry** (particles, decal, sky objects, rain, vegetation instances) | 532 KiB per frame built on the CPU, copied 3x [M/C] | write once into the ring (or generate on the GPU: particles / rain from their records, vegetation instances from the cell data) | ~0.05-0.1 ms copies [E]; GPU generation removes the composition part too |
| 7 | **pipeline-level recording** | ~2,720 vkCmd* per frame (910 draws + 910 push constants + 220 pipeline binds + 172 VB binds + 85 vertex-input sets + ~400 dynamic states + 19.5 IB binds) [M], 94 % on the record threads | merged indirect draws per pipeline group, vertex pulling from the arena (no VB binds / vertex-input sets), per-group dynamic state | the record threads' work (~2,500 commands -> a few hundred) and amdvlk's 0.23 ms engine-thread share [E]; frame time gain small while recording is parallel [I] |
| 8 | DrawBlock / sampler / bindless resolution | 910 x 192 B, 78 % identical to the previous draw's [M]; `samplerIndex` + bindless lookups per sampled slot | per-material texture / sampler indices in a material table | part of rank 2 [E] |
| 9 | the engine's remaining render shell (Execute walk, brackets, frame tap, shadow setup) | `detourExecute` - opEnter = 0.26 ms; cascades' fit / setup in `detourShadowMapRender` [M] | stays while the engine culls (gameplay reads the visible stamp, opt-plan.md O2) | - |

Upper bound of the per-draw part [D]: ranks 1-3 are ~3.0 ms of Full's 5.17 profiled (composition 1.43 + device 1.08 + memcpy 0.55);
a design that keeps the engine's culling and op lists but retains composition and draws indirectly would land near Wrapper minus
its device path: **~2.5-3.0 ms engine+HAL** at the savegame (Wrapper 3.36, Full 4.76 unprofiled) [E]. The engine's own share
(Engine.dll 0.93, SharedBase 0.39, ntdll 0.42 ms: culling, scene update, waits) is untouched by any of this [M].

Related facts for the design:

* `enh/native_opaque.*` (setting `native.opaque`, removed in 23d60f2, 2026-10-29; was default off; native-renderer.md 9 / 12 / 14) already draws the mirror's static parts
  with one `vkCmdDrawIndexedIndirect` per class from `NativeIndexArena` + a `NativeItemBlock` (928 B) per part; it uses hand-written
  shaders and was not visually equal to the engine (native-renderer.md 14, "Not verified at every spot"). Earlier measurement of
  device-generated commands: no gain over direct recording of push constant + draw (performance.md 12).
* Trimming the VS copy to the live ranges gave no fps in Wrapper (performance.md 15.1): bytes alone are not the cost; the copies and
  the per-draw CPU around them are.
* Carry-over registers make "content dedup" of today's StageConsts useless (section 4): almost every SpeedTree / actor / instanced
  block differs every frame although its own data did not change.
