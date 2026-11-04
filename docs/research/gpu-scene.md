# GPU-resident scene: design (M0, 2026-11-04)

User decision (firm; ABI-breaking changes and large rewrites allowed): the whole game state lives on the GPU and is only batch-updated on changes; rendering becomes
GPU-driven (better GPU use, slightly less CPU). `-Mode full` is to become the main mode: CPU at most Wrapper +0.5 ms at the player defaults (today +1.7 ms,
retained-signals.md 11, both backends). Labels: [C] confirmed in code / measured, [I] inferred, [?] open.

## 1. Where the CPU goes, and what earlier attempts teach (the numbers that justify the design)

Full - Wrapper = +1.7 ms at the savegame (player defaults, lod 2x). Per frame (retained-signals.md 7, 11, 12; opt-profile.md): composition of the ops that still runs per frame
~1.5 ms (trees 0.8 - 1.0, static meshes 0.4 - 0.6, sky cube 0.3, effect units 0.4 - 0.7, particles / decals / billboards ~0.3), the packet replay 0.35 us x ~940 = 0.33 ms, register
setters ~0.26 ms, per-op save / restore ~0.1 ms, the engine's slot calls (~1,200 ops x ~0.3 us = 0.4 ms). Draw counts: static 232, trees + billboards ~680 (batches), shadow views 160,
vegetation ~66, actors 21, alpha 19.

What did NOT pay, and why (so this design avoids it):

| tried | result | reason | consequence here |
|---|---|---|---|
| device-generated commands (performance.md 12) | 0 gain | recording `push constants + draw` on the CPU is ~0.1 us; the cost was pipeline binds (halved by grouping) and per-draw bookkeeping | GPU-driven submission alone saves nothing: the saving must come from *not composing / not preparing* per draw, i.e. from state that persists. Indirect draws are the consumer, not the gain |
| GPU occlusion probe (12579b2) | 8 - 24 % hidden, ~0.5 ms | small share of instances, 2-frame lag, pop-in | do not make occlusion the justification; frustum + distance first, HiZ optional later |
| small-caster cull | nothing to cull | | skip |
| composition-only retention (V3, native.retained) | -0.09 ms at 100 % hits | the replay of constants + lighting + touches ~ the composition | per-object constants must live in persistent GPU blocks, not be re-copied |
| packet cache (built) | -0.42 ms | still `issue` bookkeeping + register setters + the composition for ops not covered; engine still walks every op | the next step must remove the CPU *walk*, not only the draw prep |
| scene mirror N0-N4 (removed 23d60f2, 2026-10-29: "early hand-written prototypes were misleading and broken") | worked (native opaque list, 135 k draws verified) but the mirror fed *per-frame* refresh of 8 frames, own culling, own lighting composition | re-implementing the engine's culling / fade / lighting rules by hand diverged in edge cases (fade -> alpha list, instanced meshes, skinned, SpeedTree left to the engine) and the engine's side effects (touches, lod, light sets) were not kept | the new table is fed by the engine's **signals** (retained-signals.md 9), not by polling; the engine's op lists remain the *reference* in verify mode; side effects are kept via the lean path (O2b) that already exists |

Conclusion: three things must hold together to beat the +1.7 ms floor: (1) per-object constants persistent on the GPU (OBJECT / MATERIAL blocks), (2) validity from signals
(O(1)), (3) the engine's walk of the op lists + our slot call + composition skipped for covered classes. Only (3) removes the dominant ~1.5 + 0.4 ms. (1) + (2) alone reach about
+1.0 (retained-signals.md 12 estimates).

## 2. The scene representation (GPU-resident tables)

All tables are device buffers addressed by `uint2` pairs on both backends (the S1 rule: `rvkPtr<T,A>(pair).Get()`); updates go through one batched staging copy per frame
(Vulkan: host-visible BAR upload into a device-local buffer; D3D12: a GPU upload heap with UAV access, as the vertex capture pool; both exist as `RingAlloc::refRw` /
`GameDevice::captureAccess`). Slots are indices; every record has a generation to catch reuse.

1. **Instance table** (one record per cached-in geometry entity; M1a built CPU side: `engine/scene_table.*`): world matrix (3x4), previous matrix, entity flags, entity-sphere
   (bounds), mesh table id, LOD state, material list range, light-set id, fade, generation. Lifecycle: allocated on `eCPrefetchAdmin::OnEntityCachedIn` (0x696120), freed on
   `OnEntityCachedOut` (0x696040) and `eCEntityAdmin::NotifyDeleteEntities` (0x695310); world matrix replaced by the notifies `eCDynamicEntity::OnUpdatedWorldMatrix` (0x69e8a0) /
   `eCGeometryEntity::OnUpdatedWorldMatrix` (0x6a8d70) [C: retained-signals.md 9.1; load-time bypasses OnRead / ReadSaveGame / CopyEntityPrivateData happen before cache-in, which
   re-reads the matrix]. Dirty slots are drained in batches per frame (`drainDirty`). Per-frame fields with no signal (fade, LOD index, lighting) are *polls* of the entity
   (contexts +0x28, fade) done in the per-frame update only for entities the GPU says are visible or near, or recomputed on the GPU (below).
2. **Mesh table**: `vk::MeshArena` (vertex / index ranges, 64 MiB blocks, device addresses) is already the persistent mesh store; the table adds per sub-mesh {first index, count,
   vertex offset, stream strides, declaration id, material slot, bounds}. Filled at the same time the HAL creates the buffers (engine resource load: `-> 3` at 0x684e90, retained-signals 9.4).
3. **Material table**: per engine material pool (`eCShaderConstantPool`, data at +0x18) a persistent record of its float registers + texture slots (bindless indices). Validity: no
   signal covers all writers (9.3): per-frame writers are only `Process -> SetTime` and switches, so a *poll of the dirty range / a hash of timed-element pools* once per frame
   (cheap: only pools with timed elements) + a re-upload on the cold property-change handlers when the poll sees a difference. Textures: slot indices from the bindless set, refreshed on
   resource-state change (poll `res +0x38`, `image +0x70`: stays on the CPU as the "touch" bookkeeping, see 5).
4. **Per-frame global block** (one per view: main, 4 cascades, terrain map, point-light atlas cells, RT shadow): view / viewProj, camera, sun, fog, time, wind clock (c12), the 3 wind
   matrices + angle pairs (`UpdateWindMatrices` 0x6efa10, once per DB execution), light list, lighting pool registers c14..c37 (sun / SH: hemisphere hook already reads them). Written by
   the CPU once per view per frame (a few KB): this is the "VIEW / FRAME / LIGHT window classes" of the V2 translator ABI.
5. **Trees (SpeedTree + billboards, the biggest class: ~650 batches / frame)**: per tree *type* a persistent record (shader, geometry ranges, strip table); per tree instance a record
   {position, rotation, scale, entity slot} in a persistent instance buffer, uploaded when the tree's entity caches in (trees do not move: signals = cache-in / out only). What
   the CPU does today per frame per batch: sorts the queue, writes instance rows (world, LOD alpha / strip lengths from camera distance, wind). [I until the vertex shader
   constants are re-read] Wind is evaluated by the engine's vertex shader from the 3 wind matrices + angles (global block) and per-instance rows; LOD alpha and strip lengths are a pure
   function of camera distance and the type's limits (`UpdateLoD` 0x6f8f30): a compute pass computes them per instance each frame and writes the compacted instance stream, so the
   CPU composes nothing. Risk: the SpeedTree LOD is the engine's (`lod.trees` scaling already detours `UpdateLoD`); the compute port must match it bit-for-bit enough that the
   CPU fallback (verify mode, section 6) agrees within the strip-length quantisation.
6. **Vegetation / grass (user addition)**. Today (D-contexts.md 4, README N5.4): the blade mesh is a static VB and the vertex shader already does wind and fade on the GPU. The
   CPU does (a) the visibility walk (grid cells near the camera, node box vs view-range sphere + CullCamera, `SortGridNodes` by illumination / light flags, `GetIlluminationAt` per node),
   (b) copies each visible node's 0x2c-byte instance entries (node +0x18, per type start / count at node +0x30[type]) into the admin's shared 1000-instance dynamic VB, repacked
   (`sc::grassInstance`, `planGrass`, cursor `vegAdmin+0x40`, wraps), (c) splits batches per illumination / light set and per 1000 instances: ~66 draws / frame at ~2.5 us (0.17 ms
   in `issue()`) plus the walk and copies (**not measured: the first task of M2 is a `-CpuProfile` share of the walk + copies = the milestone's target**).
   GPU design: one persistent instance buffer per grid node holding the raw 0x2c entries (uploaded in a batch when the node's cell caches in; freed on cache-out; the vertex shader
   reads the raw entries through a per-instance fetch, no repack), a node table {box, instance range per type, light-set id}. A compute pass per frame: node cull (box vs view
   sphere + frustum), optional per-instance cull (distance / fade, frustum) with stream compaction into an indirect draw per (type, light-set bucket), so the 1000-instance
   batch limit disappears. Per-grid fade (c31.xy) and wind clock (c12) come from the global block; the node's illumination / light set (`GetIlluminationAt`) is cached per node and
   refreshed on the lighting dirty signal (`eCIlluminated_PS` +0x188 / light-set poll, 9.2) or when the player moves a node-size. The engine's cursor / admin buffer state must stay consistent
   *wherever engine code still runs* (e.g. compare mode, or ops not taken over): the existing take-over (`planGrass`) already mirrors the cursor; the GPU path leaves the admin's VB
   untouched and simply never advances the cursor for taken ops, as the lean path does for other classes (lean commit writes the cursor in `static_ops_lean.cpp`).
   Bonus: per-instance GPU culling makes a higher density affordable (config `/Engine/Vegetation` density 1..8): a possible **player setting** (Detail category), once the instance
   buffers hold the denser entries (the engine generates the instances at node build time, so the setting changes what is uploaded, not the render path).
7. **Skinned actors**: bone palettes (`UpdateRenderMatrices` 0x4dfee0, stamp-guarded, per visible actor per frame) uploaded as ONE batch per frame into a palette buffer; draws indexed
   by actor slot. Characters are ~21 draws: later, as the numbers justify.
8. **Particles / decals / strips / rain / sky / effects / GUI**: stay CPU-composed per frame (their vertices are CPU-built each frame); they remain on the existing native path.

## 3. The render side

Per view a compute pipeline: (1) `cull`: for every instance record: frustum (+ cascade-cylinder for shadow views) and distance test against the engine's view range
(`lod.view` scaling is already in the engine's ranges), results to a visibility bitset; (2) `compact`: per (pipeline, material bucket) a count + indices into an indirect-draw
buffer (`DrawIndexedIndirect` + the instance index as `firstInstance`); (3) the draws: `vkCmdDrawIndexedIndirectCount` when the optional `drawIndirectCount` cap is present
(V1 caps lines already detect `Indirect draw count`), else `vkCmdDrawIndexedIndirect` with the CPU-known max count (empty draws cost ~nothing); D3D12 `ExecuteIndirect`
(SM 6.6, command signature with `DrawIndexed` args only: the root signature is fixed, per-draw data comes from the instance table via `SV_InstanceID` / `firstInstance`).
Buckets = (pipeline, material). The engine's per-draw pipeline state (shader pair, declaration, blend / depth / cull) is the bucket key: a few hundred pipelines exist, so a
bucket list is rebuilt only when a new (pipeline, material) shows up (signal: cache-in of a mesh with a new material).
Culling needs no engine data beyond the tables; the engine's own culling / sorting is *not* needed for covered classes once the walk is taken over (section 5).
Shadow views reuse the same pass with the cascade's frustum / sun cylinder; the RT shadow / GI consumers read the instance table directly (the RT scene registry is already a GPU-side
instance registry: its quantised-row hash "moved detection" is replaced by the table's `version` / `movedFrame`).

**Translated shaders stay the engine's own.** The ABI change (the part that touches `abi.h`, `rvk_abi.hlsli`, `rvk_draw.hlsli`, the translator's V2 split-constants windows, DXIL
parity, `kTranslatorVersion`): today the translated VS / PS read float registers through 4 per-stage register windows (`pc.vsw` / `pc.psw`) uploaded per draw. In the GPU scene the
windows become *per-class base addresses*: VIEW / FRAME / LIGHT window = the per-view block (patched per frame), MATERIAL window = the material table record (persistent), OBJECT
window = the instance record selected by `firstInstance` (a gather in the generated `rvk_in` prologue: `rvkObjectWindow(instance)` returns the same register values as the
CPU-composed rows: world rows c32.. etc. derived from the 3x4). A new generator option (`TranslateOptions::objectFromTable`) writes the prologue; the push block keeps <= 128 B
(two addresses + the table base). Both backends: the table is an addressed buffer (`uint2` pair), no new root parameters. Verification of the gather: an `rvk_shadereq`-style
equality run (translated draw with CPU windows vs the same draw through the table) = the S1 / parity gate.

## 4. What the enhancements consume

RT scene (TLAS instances: the table slot is the identity; BLAS per mesh table entry; moved detection from `version`), object motion (`prev` matrix: replaces the previous-frame
vertex capture for rigid draws), G-buffer variants / material pack pack-word (a material table field), GI / water (global block + the instance table's static flags). During M1 the
`prepareDraw` observers keep working on the verify path; a draw taken by the GPU path is announced to the observers through the table (they already work from the retained query,
retained-signals.md 10).

## 5. What the CPU keeps doing, and how to stop paying for the engine's walk

The engine's frame is: frustum DB cull -> per entity `Request` (0x671370: `UpdateLoD`, light-set `UpdateIllumination`, fade `CalculateFadeOut`) -> op lists (z / solid / alpha /
refracted ...) sorted -> `Execute` calls our slot per op. Two ways:

* **Keep the engine walking, make the slot O(1)** (M1 - M2): the op's slot is skipped by the lean path today; the remaining cost per op is `opEnter` + validity (~0.3 us x 1,200).
  With signals the validity is a generation compare. Gain ~0.3 - 0.4 ms. Needs no hooks beyond the table.
* **Take the traversal over for the covered classes** (M3, the structural gain): stop the engine from building the op for covered entities. The engine builds ops in `Request`
  (per visible item per database): hooking `eCFrustumElementItem::Request` for items whose entity is covered and returning "culled" skips its `UpdateLoD`, `UpdateIllumination`, op
  creation and sorting: ~0.3 - 0.5 us x ~1,800 items per frame x DBs. **This is the risky part**: the engine's side effects of `Request` (LOD pick, the light-set / illumination
  update, prefetch of the next LOD, fade) are *state other engine code reads* (lighting of dynamic entities, resource prefetch, the alpha-list move at fade < 1). The lean path
  (O2b, `static_ops_lean.cpp`) already enumerates and reproduces the side effects of the *slot*; `Request`'s are not catalogued [?]. M3 starts with a headless-Ghidra dump of
  `Request` 0x671370, `UpdateLoD` 0x5c74b0, `UpdateIllumination` 0x595300 and a verify-mode diff of the entity fields they write.
* Per frame regardless: the image touches (~600 resource-LRU touches, which can become one batched list per frame from the GPU's visible set read back with a 1-2 frame lag, with
  the prefetch margin the engine already has), the view / frame / light blocks, the palettes, the CPU-composed classes of 2.8.

## 6. Verification strategy

Verify mode compares the GPU-culled instance set with the engine's op list per view: false negatives (an op the engine draws whose instance the GPU did not) are bugs, false
positives are counted and must be conservative (bounds padding). Pictures: pixel diff of the GPU path vs the engine path (known noise masks, refcompare). Counters in rvk.log
(`scene table:`; DIFFERING / unresolved must stay 0), the existing `native.*_compare` flags, `lean_check=10`, the packet cache's verify (the GPU path draws are checked like packets).

## 7. Milestones and expected ms (from the profile above)

| M | content | gate | expected gain (ms, Full - Wrapper) |
|---|---|---|---|
| **M1a (built)** | signal-fed CPU instance table (`engine/scene_table.*`), `native.gpu_scene=1` verifies every op's entity resolves and its matrix equals | 0 DIFFERING / 0 unresolved over a long run | 0 (foundation; hooks are cold: cache-in / out, matrix notify) |
| M1b | GPU instance buffer (batched dirty upload, both backends) + mesh table + the OBJECT window gather in the translator (`objectFromTable`), static meshes main view z + solid drawn from the table with CPU culling still the engine's op list (draw per op, constants from the table) | rvk_shadereq equality, DXIL gate, compare none | -0.2 (removes OBJECT window upload + register setters of static draws) |
| M1c | compute cull + compaction + indirect draws for static z / solid, main view; verify vs op list | false negatives 0 | -0.2 on CPU, GPU time measured (must not exceed the saved CPU+GPU of 232 draws) |
| M2a | vegetation: measure the walk + copies share first; node table + persistent instance buffers + per-node / per-instance cull + indirect | pixel equal in rvkcompare grass spots | est. -0.3 .. -0.5 (walk + copies + 66 draws at 2.5 us; to be measured) |
| M2b | trees / billboards: instance buffers per type, LOD alpha / strip length + wind inputs from the global block, compute compaction | verify vs `composeTree` | est. -0.8 .. -1.0 (the ~0.85 ms trees ceiling of retained-signals.md 7) |
| M2c | shadow cascades from the same tables (cylinder cull per cascade) | cascades compare none | est. -0.2 |
| M3 | take the traversal over (hook `Request`) for covered classes; O(1) validity | side-effect diff 0 | est. -0.5 .. -0.8 (engine's walk + slot calls); decides whether +0.5 is reached |

Sum of estimates: 1.7 - (0.2 + 0.2 + 0.4 + 0.9 + 0.2 + 0.65) ~ -0.15 .. +0.4 vs Wrapper: the target (+0.5) is reachable only if trees and M3 deliver; M1 alone cannot. These are
estimates from the profile, not measurements.

## 8. Risks

1. M3's engine side effects (section 5) are the largest unknown; mitigation: verify-mode diff of entity fields, keep the engine's walk as the fallback behind a switch.
2. Tree LOD / wind compute must match the engine's CPU result; mitigation: bit-compare in verify mode against `composeTree`'s rows.
3. GPU-driven submission alone does not pay (performance.md 12, 15.1): the gain comes from removing CPU composition. If a milestone shows no CPU gain it is not worth keeping on.
4. The material table's validity has no single signal (9.3): a poll per frame of timed pools; an unseen property change would show stale constants (verify: hash compare every N frames).
5. D3D12 parity: the gather must compile to DXIL from the same text; ExecuteIndirect with `SV_InstanceID` has no base-instance builtin: use a root constant per bucket or `SV_StartInstanceLocation` (SM 6.8) [?: check the DX12 backend's SM 6.6 floor; a per-bucket uint in the argument buffer works with a command signature carrying the constant].

## 9. Status

M0 doc and M1a are committed. M1a gate (Full, testdemo 7/7, `native.gpu_scene=1`, 45 s): 787 slots, 2,614 op entities checked, all resolved, matrices equal, DIFFERING 0, no hook warnings; rvk_tests 224/224. Not run yet: D3D12, -Validation, long run, refcompare (hooks are backend independent). Nothing beyond the CPU table existed then: no GPU buffer, culling, indirect draws or shader ABI change.

### 9.1 M1b part 1: the GPU instance table (built 2026-11-04)

`src/gpuscene/instance_buffer.*`: a 32,768-slot x 160 B device-memory buffer (`InstanceRecord`: world, prev, generation, version, flags, live, movedFrame; HLSL
`shaders/include/rvk_instance.hlsli`, `rvkLoadInstance(tableRef, i)`), addressed by one `GpuRef` (Vulkan device address / D3D12 heap slot + offset) from `rhi::Buffer::shaderRef`.
`flush()` runs once per present (`native.gpu_scene=2`, default 0): `scenetable::drainFreed` (new: freed slots, tracked only while a consumer drains) then `drainDirty`, one ring staging
block per frame, one `copyBuffer` per contiguous run, a barrier. Every 240th frame the table is read back (Readback buffer, collected once the frame completed) and compared with
the CPU copy that was uploaded. Gate (Full, savegame `out/fg/perf.save`, lod 2x, `-Validation`, 60 s): Vulkan 3,411 flushes, 108,909 slots in 63,755 runs (17 MiB), 21 read-backs
(67,077 slots) DIFFERING 0, 0.068 ms CPU per flush, scene table 3.4 M op entities checked, DIFFERING 0; D3D12 (debug layer, RVK_NO_GBV) 3,721 flushes, 19 read-backs (71,660 slots)
DIFFERING 0, 0.096 ms per flush, no validation messages; testdemo 7/7 with `-Validation` (Vulkan Full); rvk_tests 224/224.

**Finding that changes the plan (from the existing profile, performance.md 24):** in Wrapper `eCFrustumDatabase::Execute` is 3.18 ms of which the device draw path 2.07 and the engine's
slot code ~1.1: the engine's own walk (cull, `Request`, sorts) is ~0 of it. So M3 (taking over `Request`) cannot pay on its own; the gain lies in what is done *per drawn op*: our
composition (~1.25 us per op x ~1,200) + `issue` / replay (0.35 us x 940) + register setters + the shared prepareDraw path for ops that miss the packet cache. A draw from persistent
GPU state (object window from the instance table, material / light windows persistent, packets replayed in grouped batches) is the lever; `Request` skipping only a later step.

**Not built (nothing of it exists in the tree):** the translator's `objectFromTable` (world rows c33..c35 of non-instanced static meshes via a variant through `TranslateOptions::constOverrides`;
needs a push block extension `objTable` + `objIndex` (112 -> 128 B), `noteWindow` skipping overridden registers, a per-draw VS variant choice in `prepareDraw`), the mesh / material tables,
GPU cull + indirect draws, vegetation, trees, cascades, `Request`. The GPU table has no consumer yet, so `native.gpu_scene=2` costs ~0.07 - 0.1 ms CPU for nothing: keep it at 0 until
M1b part 2 or the RT scene / object motion read it. No CPU number improved.
