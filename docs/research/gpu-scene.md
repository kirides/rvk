# GPU-resident scene: design (M0, 2026-11-04)

> Note (2026-11-05): the settings `debug.light_collect`, `lighting.light_shadow_baked`, `lighting.table_complete`, `lighting.tree_merge`, `lighting.trust_table`, `native.gpu_scene`, `native.retained` named below were removed (removed 2026-11-05, now a constant in code or an always-on behaviour: see `docs/research/settings-audit.md`); read them as the history / the values the code now fixes, not as switches to set.

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

### 9.2 Session 2026-11-04 (third agent): measurement before building M1b part 2 - the object-window / persistent-window step cannot pay

Nothing of the translator `objectFromTable` / push extension was built: two cheap measurements (Full, `-PlayerDefaults`, savegame lod 2x, `native.packet_cache=1`, Vulkan) show it targets the wrong cost.

1. **Upper bound of persistent windows (experiment, removed again).** For static main-view replays the register setters and `uploadConstants` were skipped (windows reused from the previous draw, wrong pictures, timing only): static `issue()`
   1.960 -> 1.891 us per draw (bookkeeping 0.924 -> 0.827 us), `-Perf` CPU 7.22 -> 7.26 ms (noise). The whole constants chain (object / material / light windows + setters) of the replayed static draws is worth
   ~0.05 - 0.1 us per draw, i.e. < 0.05 ms per frame. An object window fetched from the instance table can save at most that, plus the fade (c32.w) still needs a window.
2. **At the player defaults most static / solid draws are not in the packet cache at all.** New counters (`native: packet cache not capturable by prepareDraw`): over the run 4.18 M draws were refused capture because they are
   `giDraw` (G-buffer / GI pixel-shader variants of lit main-view solid draws: `giDraws_.on` && `gbufWrite_` or an eligible ambient), 0.35 M because the VS / PS module differs from the engine's (variants). Per 5 s: 238 k draws replayed vs 497 k fresh
   (~360 vs ~750 per frame); fresh static draws cost `issue()` 1.96 us of which 1.04 us `prepareDraw` + submit. The earlier "940 draws replayed" figure holds only without the variant consumers (test baseline).
   So the deliverable's class (static z + solid, main view) is exactly the one the variants take out of every retained path.
3. Consequence for the plan: before any GPU-resident object window, the *variant decision* of `prepareDraw` (G-buffer / GI / material pack: `giEligible` reads the ambient registers every draw) must become a cached,
   signal-validated property of the op (a per-entry variant choice + its pipeline in `CachedDraw`, the ambient-nonzero test from the light-set stamp), otherwise the O(1) path never runs at the player defaults. Only then do
   persistent windows / table fetches matter (and by 1. they are ~0.05 ms). The composition (DrawDesc, ~1.25 us/op) remains the largest item (retained-signals.md 12, performance.md 24).

### 9.3 Session 2026-11-04 (fifth agent): the per-group profile at the player defaults, and why no GPU-driven consumer was built

Profile (Full, `-PlayerDefaults`, savegame lod 2x, `-CpuProfile -Perf`, Vulkan, main 133c36f; 11 windows, interval 7.40 ms with the profiler, `-Perf` CPU 6.38 ms) and the `issue cost` lines, per frame
(699 frames per 5 s window); composition = CPU time of the group's `compose*` (profile inclusive), issue = draws x us per draw in `issue()` (packet replay):

| group | draws / frame | composition ms | `issue()` us per draw -> ms | total ms |
|---|---|---|---|---|
| SpeedTree + tree billboards | 691 (655 batches / 169 ops: 2.9 instances per batch) | 0.78 (`composeTree`) + 0.16 (billboards) | 1.27 -> 0.88 | **~1.8** |
| static meshes (z + solid) | 240 | 0.47 (`composeOp`) | 1.54 -> 0.37 | 0.84 |
| shadow views (cascades, terrain map) | 167 | ~0.3 | 1.32 -> 0.22 | ~0.5 |
| vegetation (grass) | 68 (~30 ops, 1,500 nodes, 9,300 instances = 410 KB repacked per frame) | **0.16** (timer in `composeVegetation`: 111.7 ms per 5 s) | 3.5 (2.9 of it the device path, not replayable: new instance bytes every frame) -> 0.24 | 0.40 (device path 0.20 shared with Wrapper; Full-only ~0.1 - 0.16) |
| sky cube (5 faces) + dome | 24 | 0.33 | 11 / 7.7 -> 0.25 | 0.58 |
| effect units, particles, decals, alpha meshes, actors, precipitation | ~65 | ~0.6 | 2.4 - 13.7 -> ~0.3 | ~0.9 |

So "static meshes + cascades dominate" does not hold at the player defaults: **trees are ~1.8 ms of ~6.4**, static 0.84, shadow views 0.5, vegetation 0.4 (the user's side question: the grid-node walk + instance repack is
0.16 ms; a GPU grass path could save at most ~0.1 - 0.16 ms Full-only, the draws are shared with Wrapper).

**Merging tree batches into multi-draws (measured, dropped).** Hypothesis: the ~4 batches per tree op differ only by strip length (an index range), so consecutive batches could be one packet with N ranges (instance rows in consecutive
constant slots, `firstInstance` offsetting the instance-rate index stream). Probe counters in `composeTree` (removed again), per 5 s: z / shadow batches 111,151 -> 100,370 (-10 %: they are cut by the 8-bit limit as often as by strip), solid batches
384,131 -> 277,882 when merged by equal light *list* (-28 %), **384,131 -> 384,131 when merged by equal illumination object**: every tree has its own illumination object (a batch is drawn with its last instance's illumination: ambient / SH /
lights c14..c37 per instance), so an exact merge finds nothing and a merge by list changes the lighting of the earlier instances (not pixel-equal). Best case -28 % of ~650 draws x ~2.3 us = 0.35 ms with a picture change. No-go.

**Why no indirect consumer was built.** (1) The ceiling of a perfect GPU-driven path is large (compose + issue of trees + static + shadow views ~3.1 ms of 6.4: Full would drop below Wrapper), but each of those draws needs per-draw state that is
composed on the CPU from engine memory every frame: the light window from the instance's own illumination object (content changes in place every frame, retained-signals.md 9.2), the material window, the instance rows. A draw from the instance
table without them is wrong for exactly the lit solid pass, so the first real consumer needs the whole persistent-window design at once: per-instance light blocks on the GPU fed from the light-set poll, translator `objectFromTable` /
`lightFromTable` prologues, a draw-index source on D3D12 (no `SV_StartInstanceLocation` below SM 6.8; per-draw root constants break multi-draw), push block growth past 128 B, DXIL parity, new keys for the packet cache / verify mode, and the RT scene /
pack / object-motion consumers: the rewrite of retained-signals.md 12, not one step. (2) The recommended static / cascade subset is 1.3 ms of composition + issue, of which persistent windows were bounded at < 0.05 ms (9.2) and the rest
(~1.2 us composition + 0.3 us `opEnter` per op) disappears only if the op is not entered, i.e. `Request` take-over (M3), which 9.1 showed saves ~0 on its own. A half-way consumer would add GPU passes and a second verify path for no CPU gain
(design risk 3: not worth keeping on).

**Next, by measured payoff:** (a) trees as a unit (~1.8 ms): per-instance persistent record {world, tree values, LOD alpha} + per-instance light block updated when the light-set poll changes, one indirect draw per (type, pass), lighting fetched by
instance id (the SpeedTree VS already indexes its instance rows by the instance-rate stream; D3D12 the same through a per-instance vertex stream, avoiding `SV_StartInstanceLocation`); needs the translator ABI + both RHI backends. (b) static + cascades on
the same blocks (0.8 + 0.5 ms). (c) vegetation: persistent per-node uploads of the raw entries (0.1 - 0.16 ms). (d) M3 last. Nothing was started: no code, setting or ABI changed (the diagnostics above were reverted), gates unchanged (tree = main 133c36f).

### 9.4 Trees as one GPU-driven unit: the ceiling, measured (2026-11-04, fifth agent, after the order to build it)

Before building the translator / RHI work for the tree path, its CPU ceiling was measured with a timing-only experiment in `composeTree` (`drawBatch` dropped for chosen batches, the instance rows still written, the pictures wrong; reverted; Vulkan Full,
player defaults, savegame lod 2x, `-Perf` CPU engine + HAL, last 4 windows of each 60 s run, interleaved pairs; base = unmodified):

| experiment | draws / frame | CPU ms | vs base |
|---|---|---|---|
| base | 1,277 - 1,290 | 6.39 / 6.35 / 6.55 / 6.38 (mean 6.42) | |
| **one draw per tree op** (every batch after the first dropped = a perfect "all instances of the op in one indirect draw", per-instance light data free) | 785 | 5.39 / 5.50 (mean 5.44) | **-0.98 ms** |
| **exact merge only** (a batch is dropped when it has the same light list, no per-object light flags and fits the limit with the previous drawn one; z: within the limit) = what a draw-index + per-instance light fetch can do without changing a pixel | 1,240 - 1,247 | 6.30 / 6.30 (mean 6.30) | **-0.17 ms** |

(An earlier attempt with `getenv` measured nothing because the proxy sets its `environment` entries with `SetEnvironmentVariable`; `GetEnvironmentVariableA` is needed.)

Reading: of the ~650 tree draws per frame only ~45 (3.5 %) are mergeable without changing the picture. The rest differ in the **light list** (the engine's batch is cut when the list pointer differs; different lists = different pixel-shader
permutations / pass counts: `psIdx = lights + 4 (kind + 3 (combi + 2 sun))`, GetPasses runs lights in passes of up to 3), or carry per-object light flags (static SH / lights, `alone`), or in the z pass hit the 8-bit limit as often as the strip split. A
per-instance light block fetched in the pixel shader (the design of 9.3) can only replace the register *values*, not the permutation or the number of passes, so it cannot join those draws. The -0.98 ms of the "one draw per op" arm is the
price of ignoring lighting differences (a different, non-identical picture); getting it exactly needs one tree pixel shader that loops over a per-instance light array = a new tree ubershader whose results are not bit-equal to the engine's
permutations (the `debug.ubershader` class of work, shader-permutations.md), not an ABI step on the translated shaders.

**Verdict (hard, with numbers):** the GPU-driven tree consumer of 9.3 (persistent per-instance records + per-instance light block + translator prologues + draw index on both backends) has an exact-picture ceiling of **-0.17 ms** on
Full CPU (6.42 -> 6.30), i.e. it cannot move Full - Wrapper (1.75) toward 0.5; its ABI / RHI / DXIL / verify cost is not justified. The same holds for static meshes and cascades by the same mechanism (their draws are cut by the same light-list /
pass rules; shadow views and z passes carry no lights but are cut by limit / state changes: 160 draws x 1.3 us = 0.2 ms ceiling total). The only measured lever with real size is a *new ubershader* for lit solid tree / static draws with per-instance
lights (-0.98 ms for trees at the cost of picture identity), which needs the user's decision to trade bit-identity with the engine's permutations for CPU.

### 9.5 The tree uber-shader path (user decision: bit-identity traded for CPU): ceiling with per-instance lights, and the CPU cost it adds (2026-11-04)

Both variants of the plan need the lighting values (c14..c37: ambient SH from the list, sun, up to 3 lights per pass, static lights / SH of the illumination) **per instance**; the engine composes them once per *batch* (last instance's illumination).

1. **Per-instance light values, the engine's own pixel shaders** (batches merge when their pass structure is equal: list sun flag, illumination no-sun flag, static / dynamic light counts, so the same permutation and pass count; the light registers come from a per-instance table). Timing-only ceiling (merged batches cost nothing; same method as 9.4, reverted): draws 1,290 -> 1,118, CPU 6.375 -> 6.13 / 6.16 = **-0.22 ms**. The batches of one op differ in pass structure far more often than in list pointer.
2. **One draw per op through a single maximum-permutation pixel shader** (sun on, 3 lights, missing terms zero: the additive terms vanish exactly; instances with 4+ lights keep the engine's second pass): the 9.4 ceiling -0.98 ms (785 draws) assumed the per-instance light data free. It is not: `writeLighting` reads the list (SH 112 B, indoor, sun 48 B, up to 3 x 64 B lights) and the illumination from engine memory with ~6 guarded reads, and it would now run for ~1,900 instances per frame instead of ~650 batches (every tree has its own illumination object, 9.4), +1,250 compositions x ~0.4 us = **+0.5 ms**, which cancels more than half of the ceiling; the light data cannot be fetched by the GPU (engine heap memory, no signal for in-place changes, retained-signals.md 9.2). Estimated net -0.3 .. -0.5 ms for a new pixel shader that is not bit-equal, a translator / ABI change on both backends and a changed pass structure (additive passes, fog per pass) - and that before the multi-range draws that different strip lengths of branches / fronds need.

Not built: the measured exact ceilings (-0.17, -0.22 ms) are below the cost of the change, and the picture-changing ceiling (-0.98) is eaten by the per-instance lighting composition that only the CPU can do. Remaining honest levers for Full - Wrapper (1.75 ms): sky cube 0.58 (content changes every frame with the game time, so a cache is not exact; an update stride like `debug.shadow_stride` would be a picture change of ~0 and save ~0.3 ms in both modes), vegetation 0.1 - 0.16 (exact only by persistent per-node uploads), effect units ~0.2, shadow-view ops ~0.2.

### 9.6 Direction after the user's clarifications (2026-11-04): Full is not bit-identical; Full-owned clustered lighting as the basis

User decisions (relayed): Full may change the picture (Wrapper stays faithful); the lighting of the GPU-driven path is to be a Full-owned clustered-forward evaluation that **reproduces the engine's light model and look** (falloff, colour, intensity, ambient SH / hemisphere, sun, the strong sun flare,
exposure / adaptation, vivid colours; luma / hue stats and side-by-side crops against Wrapper, any visible loss of vividness / contrast / flare = failure), with more lights, point-light shadow cubes and GI as additions; a player setting `lighting.style = original | physical` (Lighting category, run-time, default original; physical = inverse-square, physical exposure, energy conserving / PBR with the material pack)
on the same light table and path. Staging: (a) light table + compute cluster build + clustered evaluation in a Full-only tree / material shader variant (default off); (b) GPU-driven draws consuming it (the 9.4 / 9.5 ceilings: -0.98 ms for trees once per-instance light composition is gone, because the light data comes from our table instead of per-instance engine reads); (c) point-light shadow cube array.
**Not started in this session** (the cheap levers of section 27 were done first; this is a multi-week program: the engine-faithful evaluation function must be extracted from the ported `ge_default` pixel shader + the lighting pool layout, the light table must be fed by `engine/lights`, and every stage needs crop validation). The measured facts it builds on: the engine's per-instance lighting is the 0.5 ms that 9.5 says the CPU cannot avoid; the ceilings of 9.4; the translator hooks (`prelude` / `prologue` / `constOverrides`), `rvk_instance.hlsli` and the instance table of 9.1.

### 9.7 Stage (a) of the Full-owned lighting: light table, cluster grid, clustered point-light variants (2026-11-04, built, default off)

**Built** (setting `lighting.clustered`, Debug + restart, **Full only**, default off; run-time `lighting.enabled` (A/B), `lighting.style = original | physical`, `lighting.debug = clusters | slots`):
* `shaders/include/rvk_light_model.hlsli`: the engine's model written down (rtgi.md 1: `col x sat(N.L) x sat(4 Ngeo.L) x (1 - sat(d^2/R^2))`, `pow(acc, 0.45)`, fog) + the table / cluster layout + the physical distance law as a *ratio* to the engine's (`phys/orig = 4.6297 (1 - u) / (1 + d^2/r0^2)`, r0 = 0.2 R, normalised to equal the engine's at 0.35 R): the game's own light loop multiplies it by `1 - u`, so the physical style changes only the distance law (sun, ambient SH / hemisphere, exposure, tone map, encode untouched).
* `enh/light_table.*`: every frame (main view, AfterDepthPrepass) `engine/lights::collect` (static tree + registered dynamic lights, <= 1024 nearest) into a per-frame-in-flight upload buffer (id from the position, change count vs the previous frame in the log); `shaders/lighting/light_cluster.comp.hlsl` (one dispatch, one thread per cluster) copies the lights into the device buffer, writes the header (frame stamp, grid) and builds a 16 x 9 x 24 froxel grid (exponential slices 20 .. 16000 units, sphere vs cluster box, <= 63 indices each). Both backends (the same HLSL; DXIL gate passes, `rvk_d3d9test --validation` both backends 0 errors, `rvk_rhitest` pass, testdemo 7/7 Full Vulkan and D3D12 with the table on). FrameBlock params[4] = buffer reference, params[5] = enabled / style / debug (no ABI struct change).
* `RVK_MV_CLUSTER` in the Gi / G-buffer material variants (`enh/material_variant`, `rvk_material_variant.hlsli`): the shader's point-light registers (c25 + 4 i, i < its compiled count <= 3) are constant-overridden with per-pixel values: the engine's own, except slots whose light is in the pixel's cluster (a table light within 8 units) which are refilled with the best lights of the cluster (luminance x falloff x (0.25 + 0.75 N.L)); slots with a shadow selector (cubemap shadow lights) and lights the table does not know stay as the engine gave them; unused slots get colour 0. Rides on the pack's gate (a no-op pack provider). All 2,321 + 4 corpus pixel shaders compile with `--cluster` (`rvk_shadertool corpus corpus --gbuf --cluster`, SPIR-V valid).
* tests: `light_table_*` (slice math, physical ratio), settings count 148. `tools/sbs_diff.py` = whole-image luma / saturation / diff numbers + heat maps of water-compare side by sides.

**Image evidence** (Full, Vulkan, `water-compare` engine = `lighting.clustered=false`, test = true; `tools/sbs_diff.py`; a control run with `false` on both sides is the noise floor): pub_interior mean |d| 1.10 (control 0.88), monastery_hall 1.02 (1.37), harbour_night 0.73 (0.79), jungle 2.91 (3.29), whole-image luma change -1.3 / -0.8 / -0.8 / -1.1 % (control -2.0 / -1.3 / -1.2 / -0.4 %), saturation unchanged within 0.004, p99 luma unchanged (sun flare / candle highlights intact). At those spots the registry holds no lights (0 in the table: the spots are teleports, nothing cached in) and every engine light is kept: **the picture is the engine's within noise, as designed**. harbour_docks (130 lights in the table): +3.4 % luma, mean |d| 2.7; the heat map shows the difference is the wave splash particles, the lit dock stone is unchanged (control 0.23). **No loss of vividness / contrast / flare measured.** (The `px` columns of water-compare's table are water masks: not usable for lighting.)

**Honest findings / limits (stage a is not the CPU win; it is the basis):**
1. **The variant only reaches draws whose pixel shader already has a point-light loop.** The engine picks the permutation by its per-object light count (0 lights = no loop). `lighting.debug=clusters` at harbour_docks: only the ship and a few leaf cards take the table, the dock stone does not. More lights affecting the image therefore needs stage (b): Full chooses the maximum-lights permutation for lit draws itself (then no per-object light composition is needed either: the CPU win of 9.4 / 9.5).
2. The engine's second (additive) passes for objects with > 3 lights still draw their own lights: a light can be counted twice (table in pass 1, engine in pass 2); stage (b) removes those passes.
3. The registry (static tree + registered dynamic lights) is empty where nothing is cached in; the table does not yet take the drawn objects' lights (GI's `pickLight` merge does); engine lights the table does not know are simply kept per object.
4. Specular colour of table lights = 0.5 x diffuse (assumed, not measured), `lighting.style=physical` is basic (distance law only; PBR / energy conservation come with the material pack variant, which reads the same per-pixel registers).
5. Not measured: CPU / GPU cost of the pass (the build is a 3,456-thread dispatch + <= 1,024 x 64 B copy; expected << 0.1 ms) and the Full - Wrapper CPU delta (stage a adds the `collect` walk, 0.14 ms in the GI's measurement; it removes nothing yet).
Next: (b) GPU-driven / maximum-permutation lit draws consuming the table, lights of drawn objects as a second table source; (c) point-light shadow cube array on the table's ids (misc.y reserved).

### 9.8 Stage (b): table lighting in Full (`lighting.full_lit`, built 2026-11-05, Debug, run-time, default off)

**Built** (needs `lighting.clustered` + `lighting.enabled`, Full, no compare run; `enh::fullLitActive()` -> `native::StaticOps::setTableLit`; rvk.log `native: table lighting on`, the 5 s `enh: lights:` line ends `table lighting on` and counts `N from drawn lists`):
* **One pass per lit draw.** `sc::buildPasses` with `PassInputs::tableLit` emits ONE base pass: the sun bit as the engine's first pass has it, the **3-light permutation always** (psIndex lights = 3), lightmap combi as before; the engine's additive kind-1 passes (objects with > 3 lights) are gone, so no light is counted twice and those objects do not draw 2+ times. A missing 3-light permutation is compiled at the slot's place like any other (`compilePass`); the test copy's pool file `ShaderMaterialPool_User.smp` stayed absent over all runs (checked).
* **Registers.** `writeLighting` still writes ambient SH / indoor / sun from the list (the engine's look); the point-light registers get the engine's own first <= 3 lights (zeroed beyond `nStatic + dynamic`: reading them blindly would pick stale list entries with the 3-light pass), and `RVK_MV_CLUSTER` keeps those the table does not know and refills the rest per pixel from the cluster (unchanged from stage a).
  **A first version zeroed all engine lights ("the table is the only source")**: pub_interior luma -17 %, monastery_hall -26 %: the candles / torches are *static* lights of the objects' illuminations and the registry holds none after a teleport. Fixed by (1) keeping the engine's lights as the fallback and (2) making drawn objects a table source.
* **Drawn objects as a table source** (`LightTable::noteDrawnLight`, `StaticOps::noteListLights`): each composed light list's dynamic lights (once per list per frame) and, while the registry walk returns no static lights (`wantStaticNotes`), each illumination's static lights (+0xd / +0x10) are handed to the table; they persist for 900 builds while re-noted (packet-cache replays do not re-compose) and merge into the next build (same light = within 8 units of a registry light; one frame latency). Where the registry has the lights (130 at harbour_docks) nothing is added.
* **Batches**: mesh batches (`planBatches(..., tableLit)`) no longer end on a different illumination alpha factor (+8); static lights / static SH objects still end a batch (their lights are per illumination, merging them would light the earlier instances with the last one's: the first version's picture loss).

**Image evidence** (`water-compare -Mode full`, engine = `lighting.clustered=false`, test = `clustered + full_lit`, `tools/sbs_diff.py`; stage-a noise floor: mean |d| 0.9 - 3.3): pub_interior (`show_actors=1`) luma +1.5 % mean |d| 0.87, sat 0.531 / 0.534, p99 189 / 192; monastery_hall +0.3 % / 1.18, p99 242 / 243; harbour_docks +0.1 % / 2.57 (130 table lights); jungle 0.0 % / 2.59. Candles / torches lit as in the engine's picture, vividness and flare unchanged. (Not covered: harbour_night, spells / torches at night, a refcompare run: see below.)

**CPU** (`-Perf` engine+HAL ms per frame, savegame Risen_Save_0005, lod 2x, player defaults, `RisenTest.exe`, vsync off, last 6 windows per run, interleaved arms; this machine state gives a larger Full gap than section 27's numbers, compare inside the table):

| | Wrapper | Full | Full + `lighting.clustered` | Full + `clustered` + `full_lit` |
|---|---|---|---|---|
| Vulkan | 6.04 / 6.06 / 6.01 | 7.98 / 8.10 / 8.05 (8.04) | 8.09 / 8.15 / 8.07 (8.10) | 7.53 / 7.40 / 7.29 and 7.58 / 7.46 / 7.47 (7.46) |
| D3D12 | | 8.26 / 8.20 (8.23) | | 7.64 / 7.74 (7.69) |

Table lighting is **-0.58 ms (Vulkan) / -0.54 ms (D3D12) against plain Full**, -0.64 against stage (a) alone (table build + variant cost +0.06); **Full - Wrapper 2.0 -> ~1.4 ms (Vulkan)**. **The ~+0.5 ms goal is not reached**: the saving is the collapsed passes (fewer `passDraw` + `issue`), the relaxed mesh batches and the dropped additive passes; the per-object light reads (`writeLighting`) remain because the engine's lights are the fallback that keeps the picture right where the registry knows nothing. `light.cluster` CPU recording 0.037 ms; GPU cost not measured separately.

**What remains.** (1) Tree batches unchanged (they end on list pointer / static lights / strip): merging needs per-instance ambient SH. (2) Dropping the light reads needs a complete table (registry static lights cached in everywhere + a drawn-list source without latency); then the merge by list is safe and the 9.4 ceiling applies. (3) Not run: refcompare (Full, switch on), harbour_night / spells / `rvk_pointshadow` pictures, one-frame latency of fast-moving torches, `-PerfFrame` GPU cost. (4) Stage (c): point-light shadow cubes on the table's ids (`misc.y`).

### 9.9 Stage (b) completed: tree merge, per-object light reads, and what the measurements say (2026-11-05)

**Built.** `lighting.tree_merge` (Debug, default on, acts with `lighting.full_lit`): `sc::planTreeBatches(..., tableMerge)` - the solid tree batches no longer end on a different light *list* pointer; they end on a different sun flag (pass structure), static ambient SH (illum +0x90), dynamic lights in the list (cubemap shadow channel lives there), more than two static lights, or a different static-light signature (hash of the first two entries). The batch's ambient / sun values are its last instance's. `lighting.table_complete` (Debug, **default off**): with a non-empty registry, lit draws skip their own point-light reads (`writeLighting` returns after the sun / ambient reads; the three registers stay zero; lists with dynamic lights keep their reads). `LightTable::registryComplete()` = "the registry walk returned lights this frame". rvk.log `native: trees (5 s)` ends `solid batch cuts: limit / strip / list / sun / static / own lights / null list` (why batches end).

**Measured (Vulkan, savegame Risen_Save_0005, lod 2x, player defaults, `-Perf` engine+HAL ms, last 6 windows, interleaved pairs):** Wrapper 5.94 / 5.92; Full 7.99 / 8.02; clustered + full_lit 7.58 / 7.45 (7.52); + tree_merge 7.54 / 7.51 (7.53). The jungle save has **no lights within the table's reach** (0 in the table), so `table_complete` skips nothing there (+-0.05, noise) and the merge finds little: **88 % of the solid tree batches end on the trees' own illumination** (a static light each, 800 units radius, 16,000+ units from the camera, i.e. outside the grid, plus per-tree static SH), 12 % on the LOD strip length. With the merge that ignored those (experiment: table far = 48,000 so the lights are in the table, `table_complete` on) the tree batches fell 360k -> 215k per 5 s (-40 %) but CPU only 7.47 -> 7.42 (profile: composeTree -0.19, issue -0.24 ms, total -0.3 .. -0.5 in the sampled profile): **99.9 % of the draws are packet-cache replays (~1 us)**, so a removed draw saves little. The same experiment changed the monastery_hall picture (mean |d| 12.7 vs 1.7, p99 243 -> 208: the larger grid / 63-light cluster cap redistributes the candle lights): reverted, `kLightFar` stays 16,000.
Profile (`-CpuProfile`, inclusive, Full table lit with / without merge): `StaticOps::issue` 2.75 -> 2.51, `composeTree` 0.70 -> 0.52, `materialDraws` 0.67 -> 0.52, `replayDraw` 1.41 -> 1.27 ms; Full - Wrapper (sampled interval) 2.0 -> 1.5 ms. Still large in Full: `composeOp` 1.2 - 1.3, `issue` 2.5, `composeZ` 0.63, `specialPassDraw` 0.5 ms.

**Verdict.** Full - Wrapper is ~1.5 ms (plain Full 2.07), not 0.5: what remains is the composition + replay of ~1,300 draws per frame, which no lighting change removes. The reads `table_complete` drops cost ~0 where there are no lights and the safe form (registry non-empty = trusted) dims interiors whose local lights the registry misses, hence default off.
**Not done:** stage (c) (point-light shadow cube array: the engine's cubemap shadows are screen-space mask channels selected by `sel_i`; a cube array needs a scene render per face = the CPU cost this work removes, an RT shadow-ray alternative on the table ids (`misc.y`) is the cheaper design: compute pass at AfterDepthPrepass over the nearest N flagged lights writing a visibility image the variant reads); per-instance static ambient for tree batches (88 % of the cuts need it).

### 9.10 Lighting work, round 3 (2026-11-05): gates, far level, lamp shadows (stage c), physical style

**Gates of the defaults (Full, Vulkan, `water-compare -Mode full`, engine arm `lighting.clustered=false`, test arm `clustered + full_lit`; `tools/sbs_diff.py`).** harbour_docks mean |d| 2.41 (luma -0.7 %), harbour_night 0.89 (-1.1 %; control engine-vs-engine 1.11 / +1.9 %), monastery_hall 0.98 (-0.2 %), pub_interior 0.49 (-0.2 %): all inside the noise floor. One local difference at harbour_night: a thin warm strip of ground lit by a torch behind a wall corner is missing in the table arm (both control arms have it): a light the registry does not hold at a teleport spot (the drawn-list source notes only the first lights of each list), not a gameplay case (the registry holds the lights while walking). D3D12 `harbour_docks` (out\refcompare\20261105-095841): the diff heat map is the wave-splash particles at the left quay and the foam / sea at the right edge; the dock stone, ship and houses are at noise level (same pattern as the Vulkan control), i.e. not lighting. **Moving torch latency**: the dynamic lights of the table come from the engine's own `AddToLightList` call at AfterDepthPrepass of the same frame, so they are the positions / flicker the draws' registers carry (an engine register light within 8 units of a table light is "known" and refilled): no latency; only lights noted from drawn lists (used where the registry is empty) are one frame old, and those are static.

**Far level (light reach).** The table's cluster grid got a second, coarser-in-depth level beyond `kLightFar` = 16,000 units: 32 x 18 tiles x 8 exponential slices up to `kLightFar1` = 96,000 (header 128 B: `grid1` / `zs1`; `rvkLightClusterAt` picks the level by view depth for the material variants and the shadow trace; the cluster pass builds both levels, 3,456 + 4,608 clusters). The collection radius is 96,000, the table keeps the nearest 1,024 lights by reach (at harbour_docks 1,499 are dropped: the far lights of the whole island). Level 0 is unchanged (same slices, same cap of 63 per cluster), pixels beyond 16,000 used to read the last near slice. **Tree batches**: an instance whose static lights (<= 2 entries) are all in the table (`LightTable::lightKnown`: within 8 units of a table light, registry non-empty) no longer ends a batch on its light signature (`rvk.log native: trees (5 s)`: `instances with table-known lights`).

**Stage (c): ray-traced lamp shadows** (`lighting.light_shadows = off | nearby | many`, **default nearby in Full with the RT tier** (user decision 2026-11-05: ray-traced lamp shadows are the point-light shadow path, no cube array / cubemap management is built; without RT the engine's own point-light shadows stay); quality low / medium / high, lamp size cm; needs the RT tier and Full + the table). The nearest 8 (`nearby`, within 25 m) / 31 (`many`, 60 m) table lights get a shadow id (`RvkLight.misc.y`); pass `light.shadow` (AfterDepthPrepass, order 5, after the cluster build and the TLAS) runs two compute dispatches: `light_shadow_trace.comp.hlsl` scores each pixel's cluster lights as the variants do (luminance x falloff x (0.25 + 0.75 N.L)), takes the best four shadow-casting ones, traces one ray each (two at high) towards a random point of a disc of the lamp's size around the light with `rvkRtVisibility` (cut-outs per quality, stops 18 units short of the light, no ray within 40 units), writes entries `id + q / 64` into an RGBA16F image; `light_shadow_filter.comp.hlsl` blurs each entry over a 5 x 5 depth-aware window. The table header carries the final image's bindless slot + size only on frames the pass is certain to run; the variants (`rvkMvLightShadow`) scale the colour of each table light they put into a register by its visibility. The engine's own shadow-channel lights keep their screen-space mask (their table twins stay excluded). Unit tests: exactness of the entries in half floats, far-level slices. `lighting.debug=shadows` (white lit, red shadowed, dark blue no lamp).

**Findings of round 3 that change the earlier statements (important).**
1. **The earlier "within noise" gates (9.7 / 9.8) were vacuous at the rvkcompare spots.** `collect(..., 16000)` returned no lights there (nothing cached in around the teleported camera), so the table was empty and every light stayed the engine's. With the far level's collection radius (96,000) the table holds the real lights (monastery_hall: 482 within the near range, 2,526 in the island's tree), and the table-lit picture then differs from the engine's: **stage (a) alone** (cluster variant, no `full_lit`) monastery_hall mean |d| 7.5 / luma -7.7 % with table colours replacing the engine's. `debug.light_collect=16000` reproduces the old empty-table behaviour (monastery 1.7). The same was behind the 12.7 of the "far 48,000" experiment of 9.9: it was not the grid, it was the table starting to work.
2. **The engine weights every static light per object.** Matching the drawn lists' register colours (the draws' c26 + 4 i, `noteDrawnLight`) against the registry's colour (rgb x intensity x scale x 0.5) of the same light gives ratios of **0.2 / 0.5 / 0.75 / 1.75 / 2.0** (20,459 matches at the monastery: 4,004 / 4,658 / 1,149 / 901 / 9,744; all `eCStaticAmbientLight_PS`, the class the candles / torches use as point lights): the share 0.5 is right for none of them. A per-light calibration (a matched registry light takes the drawn colour seen last) did not bring the picture back (12.5), the engine's per-object weight is not a per-light constant.
3. **Fix (variant, `rvkMvCluster`):** an engine register light keeps its slot and its colour (the object's weight) and its table twin is excluded; the table fills only slots the engine left empty (and the physical style multiplies the engine slots by its distance ratio too). Monastery 12.5 -> 5.7 (luma +5.9 %), pub_interior 5.6 (+9 %), crystal_cave 4.8 (+15 %), harbour_night 2.3 (+4 %), harbour_docks 4.8 (the quay splash particles; stone unchanged): the remaining difference is the intended extra light on surfaces / objects the engine gave fewer than three lights, not a colour error. The additive passes of objects with more than three lights stay gone (`full_lit`): their 4th+ lights only appear where a slot is free.
4. `registryThin` / `registryComplete` now count lights that reach the NEAR level (`kLightFar`), not the whole tree; the far lights of the island fill the table's 1,024 capacity only after every near light (stable partition by reach); drawn-list notes are taken always (they also tell the registry's static lights nothing any more, see 3: the colour override stays for table-only lights).
5. The cluster build stages 64 lights at a time in group-shared memory (view-space centre + range): 0.228 -> 0.112 ms (Vulkan), 0.232 -> 0.150 ms (D3D12) at harbour_docks 1920 x 1080.

**Cost** (harbour_docks, `-PerfFrame 4500`, 24 frames, 1920 x 1080, GPU ms per frame): Vulkan Full 1.62 -> `clustered + full_lit` 1.90 (cluster build 0.11) -> + `light_shadows=many` 2.12 (pass `light.shadow` 0.07, 9 lights); D3D12 1.77 -> 2.36 (cluster 0.15) -> 2.66 (shadow 0.09). CPU (savegame, lod 2x, `-Perf` engine+HAL): Vulkan Wrapper 5.99, Full 8.03 / 8.05, lit 7.34 (-0.7; with the PBR code compiled in 7.56 / 7.28 and +0.45 ms GPU, hence restart-gated), D3D12 Full 8.39 / 8.28, lit 7.55 / 7.45. Tree batches (Vulkan lit): the `static` cuts fall 1,673,416 -> 9,297 per run and 4.27 M -> 2.70 M batches, CPU -0.1 against the stage-b table: 99.9 % of the draws are packet-cache replays, a removed batch saves ~1 us. **Full - Wrapper stays ~1.3 ms (target 0.5 not reached): composition + replay of ~1,300 draws, not lighting.**

**Decision (user, 2026-11-05).** Keep the extra interior light of the table-lit Full (finding 3: luma pub_interior +9 %, crystal_cave +15 %, monastery_hall +6 %, harbour_night +4 %) and re-baseline: `refcompare` Full FAILs `monastery_hall`, `crystal_cave`, `pub_interior` against the engine D3D9 reference on both backends, and these are **expected differences in Full** (reference-compare.md 11.2); the faithful comparison is Wrapper. Knobs back to the old picture: `lighting.clustered=false`, `debug.light_collect=16000` (empty table), Wrapper.

### 9.11 Lamp shadows follow-up (2026-11-05): why the hero threw no shade at a torch

Report: Full mode, RT GPU, hero next to a wall torch / barrels / candles, no visible shade of the hero or props on the floor and walls. Findings (Vulkan, Full, the player's settings: gi on high + `gi.lightmaps=replace`, ao, materials high, `lighting.clustered / full_lit`; own test copy):
* **The pass runs and traces.** `lighting.debug=shadows` at the harbour torch (`torch_hero` spot): the floor is "tested, lit" (white), the lamp has id 1 (the nearest of 35 lamps within 25 m of the camera), the hero is in the TLAS (`rt.debug=depth`: green). `lighting.light_shadows` is default `nearby` in Full; it needs Full + the RT tier + the clustered table; **`shadows.rt` is NOT required** (the lamp shadows hold their own RT scene consumer).
* **Cause 1 (fixed): the light on static surfaces is baked.** Floors / walls get their lamp light from the lightmaps (and, with `gi.lightmaps=replace`, the probes), which no per-pixel table light replaces; the traced visibility only scaled the table lights written into the shader's three registers (hero / props / pack draws). A character or barrel therefore never shaded a floor lit by a baked lamp. The material variant now also takes the traced visibility of every shadow-casting lamp of the pixel's cluster off the baked light: `1 - baked * sum(E (1 - vis)) / (sum(E) + 4 lum + 0.05)` with `E = lum(colour) x falloff x N.L`, the pixel's own lit luminance as the measure of the rest of the light (characters excluded: their lamp light is the table's). `lighting.light_shadow_baked` (Advanced, 0..1, default 0.8; 0 = the old behaviour; **now a constant `kLampShadowBaked` = 0.4** since 2026-11-08, user decision: with the lamp-shadow push block fixed (9.12) the traced shadows really act, and 0.8 turned dark surfaces fully shadowed from a candle (the pub rug under the round table) nearly black; 0.4 keeps a visible contact shade with the baked light dominant) is a run-time switch: in-run A/B of one frozen frame (`-Commands 'N: set lighting.light_shadow_baked 0'`).
* **Cause 2 (changed): the 8 nearest lamps to the camera.** A town has 28 - 35 lamps within 25 m; `nearby` now gives ids to the nearest 12 (the trace still keeps the best 4 per pixel).
* **Cause 3 (user objection, `lighting.table_complete` renamed `lighting.trust_table`, Debug):** it skipped the per-object light reads whenever the registry returned any light, so an object lit by a light the registry misses lost it, and where the table does hold the light the engine's per-object weight (0.2 - 2.0 x) is lost. The skip now requires every static light of the object's illumination to be within 8 units of a table light (`LightTable::lightKnown`); the picture still dims where the table's colours are weaker than the engine's (monastery_hall luma -29 %, pub_interior -14 %), so it stays off; old key loads with a warning.
* **Reproduction caveats.** At a teleport spot (`rvkcompare`) the registry holds nothing near the camera at first (beach start: 1 lamp within 25 m, none near the visible torch); the harbour spots have 28 - 35. The hero standing directly under the torch (`torch_hero` anchor: 9 units from the torch) shades only a tiny patch; his own pixels are lit by the engine's kept register lights and show no tested state. The in-run A/B (baked 0.8 vs 0, one frozen frame) differs by mean |d| 0.22, only at the shaded patch: the shade there is subtle, not absent; a placement 3 - 4 m from the torch with the camera behind the hero is the judging view.

* **Debug views of the baked-light term (2026-11-08):** `lighting.debug=bake` (r = the darkening share `rvk_clBake.x / (rvk_clBake.y + 0.05)`, g = lamp light sum, b = a shadow-casting lamp reached the pixel), `prebake` (the colour before the darkening: grey = luminance, red = NaN, green = infinite) and `path` (yellow / green = the pixel ran the cluster variant's epilogue, magenta = the table was not used for it, alpha 1: a draw that is not painted did not run the variant). Used for the pub rug: the term is identical on Vulkan and D3D12 (dx12-game-port.md, defects of 2026-11-08); a rug with a dark texture under a table with a candle on it is fully shadowed and, measured against its own luminance, goes nearly black (0.8 x share). The Vulkan 'lit rug' was a draw that never ran the variant: **not the `native.cluster_cull` path (corrected 2026-11-08, frame-ownership.md 7.34 addendum): the variants of those shaders were still being built (cold SPIR-V / pipeline cache after a rebuild); with hot caches every draw runs the variant on both backends.**

### 9.12 Lamp shadow push block: the pass read its own push constants at the wrong offsets (2026-11-08, fixed, both backends)

* **Symptom.** D3D12 GPU-based validation: "uninitialized descriptor heap index 1600" in `light.shadow trace` of every Full run with `lighting.clustered` (frame-ownership.md 7.32 / 7.34 / 7.36 / 7.41), and one device removal in 5 spot runs in that dispatch (7.44).
* **Cause.** `LightShadowPush` held `uint2 table` right after the 16-byte header and `uint4 tex` after it. The C++ mirror put `tex` at offset 24, but HLSL's rule (a 2 - 4 component member never straddles a 16-byte boundary) puts it at 32: `pc.tex.x` (the depth map's heap index) read the width (1600 at 1600x900), `tex.zw` / `cfg` / `misc` read the next members' bits (a float as the extent, the frame index as the ray count ...). 1600 was a width used as a descriptor index (an unwritten slot on D3D12; the garbage extent / rays are the likely cause of the one device removal). The SPIR-V compiled by DXC has the same offsets (tex at 32, checked with spirv-dis), so **Vulkan read the wrong data too, silently**: the depth fetch hit slot 1600 of the bindless array (some valid texture), GPU-assisted validation could not object, and the lamp shadows were effectively off (`lighting.debug=shadows` showed everything "tested, lit").
* **Fix.** The block is ordered `tex, cfg, misc, table` (the uint2 last), both in `shaders/lighting/light_shadow_common.hlsli` and `src/enh/light_table.h` (`static_assert` on the 16-byte offsets); the trace shader returns before touching a descriptor if the depth slot is 0 / >= 65536 or the extent is 0 / > 16384, and clamps the ray count to 1..8; rvk_tests `pass_push_blocks_do_not_straddle_16_bytes` scans every embedded pass push block (all other blocks were already fine).
* **Evidence** (Full, `lighting.clustered / full_lit / light_shadows=nearby`, private copy `risen-test-vk-lsh`): D3D12 `-Validation` (debug layer + GBV), pub_interior / monastery_hall / harbour_docks / torch_hero, 3 runs: 0 GBV messages, 0 errors, no removal (3,000+ lamp-shadow frames traced each); Vulkan `-Validation` + GPU-assisted: 0 VUID, 0 errors. Pictures (old DLL = main 9f8964e vs new, Vulkan): with `lighting.debug=shadows` the old picture is uniformly "lit" (white), the new one shows the shadows of the torch post, the hero, the pub's tables / stools / pillar and the monastery's windows' frames as soft red shade; the normal view barely changes (the lamp's light on baked surfaces is only darkened by the traced share, 9.11); the pub's carpet under the table now falls into the table's shade. Vulkan vs D3D12 after the fix: pub_interior and monastery_hall agree (debug and normal view); torch_hero differs at the hero (open, below).
* **Open.** `torch_hero`: on D3D12 the hero's silhouette is mostly "lit" with holes and his cast shade lies long to the left on the ground, on Vulkan it is a short patch at his feet (same camera, same 35 lamps, same 12 ids): his skinned instance in the RT scene differs between the backends (not the push block). Not investigated here.


### 9.13 Rugs black under lamp shadows; the "80 shaders compiling that never drain" (2026-11-08, fixed)

* **Rugs.** `pub_interior` (Full, ubershader, clustered + full_lit, GI, AO): with `lighting.light_shadows=nearby` both rugs and the stool next to them went near-black (rug luminance 21 -> 8 in a 1600x900 crop). Checked and **refuted**: the draws not running the cluster variant (`lighting.debug=path`: rugs, stools, tables all run it, no magenta), a NaN / negative path (`prebake` clean), thin geometry self-shadowing, a wrong lamp id, the baked share (0.4 at most darkens to 0.6). **Cause:** the traced shadow is right and total. The pub's candle stands ON the round table: every ray from the floor within ~2.5 m (the top's radius x light height / candle height above the top: the shadow of the top) to the flame hits the table top, the lamp's registers get `k *= 0`, the baked term darkens, and nothing gives the blocked light back (the engine's ambient there is nearly black, the GI probes do not carry the shadowed lamp's bounce). `lighting.debug=shadows` shows a long red disc under and around the table and the far table's rug. A candle on a table is the worst case of a point light with no bounce. **Fix:** `LS_FILL = 0.4` (`light_shadow_common.hlsli`): the trace pass lerps every traced visibility 40 % towards 1 (a blocked lamp still lights its surroundings by bounce off the lit table top and walls), so shade stays visible (a character, a table, the monastery's window frames still darken their floor) but is no black hole: rug 21 / 8 (before) / 13 (now), far rug 26 / 15 / 19, stool 18 / 10 / 13. It acts on the table lights' registers and on the baked term alike (both read the one image). Constant, no setting.
* **"80 compiling".** The number in the `hal: frame` line was a **running total of draws skipped since start** (`Stats::skippedCompiling`, never reset), not a pending count: 73 - 83 draws in the first ~1,500 load frames (a warm cache still goes through the workers' queue) and then flat for 5,000 frames; no module stays pending (`0 shaders queued` at the end of every run, no `does not compile` / error lines). No geometry is missing because of it. The line now says "skipped draws since start: ... N while their shader compiled (a running total, not a pending count; M shaders queued now)" (`Device::queuedShaderJobs`).
* **Variant decisions (checked after the cluster-cull agent's note, frame-ownership.md 7.34 addendum).** The variant modules of the rugs / pillar are a separate pending population from the "80" (which counts skipped draws of translated shaders). In the ab04 logs and the new runs `native: packet cache refusals (5 s)` shows `variant decision` 228 - 316 in the first 3 windows (loading / first world frames) and **0 in the remaining 15 windows** of every arm, i.e. the rugs run their variant for the whole picture window: all pictures are valid arms (a picture is only valid with the count at 0 in its window).
