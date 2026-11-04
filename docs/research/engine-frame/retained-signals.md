# Retained composition with engine change signals: design study (2026-11-04)

Question (user): GD3D11 gets vob add / remove / move from Gothic. Does Risen's engine offer the same, and does that make retained composition (cache a composed draw across
frames, invalidate by signal) reach Full - Wrapper <= ~+0.8 ms? Starting point: Full - Wrapper +1.97 ms (Vulkan) / +1.93 (D3D12) at the perf savegame (performance.md 24).

**Answer in one paragraph.** The engine does have usable signals for the *object-static* part (entity cache-in / cache-out, world-matrix change), none for materials, lights, LOD or fade
(those are per-frame pulls or in-place field writes). But signals are not what limits retained composition: the V3 prototype (`native.retained`) already hits 100 % at the savegame
(1,118 hits per frame, 0 built, 0 invalid) and gains only 0.09 ms, because **the replay itself costs almost as much as the composition it replaces** (0.52 us per hit vs ~0.62 us). Validation
is a small part. Signals can remove the validation (~0.05 - 0.1 ms) and a class-split constant patch can bring the hit to ~0.3 us: **estimate for composition-only retention
with signals: gap ~1.6 ms (1.97 - ~0.4). No-go for that.** The structure that could reach the target retains the *packet* (bypassing `issue` + `prepareDraw`, 0.4 + 1.7 ms), where
signals do matter; that is a larger project (section 5) and is not prototyped here.

## 1. The engine's change signals

Source: the existing dumps / notes (engine-frame/*.md, native-renderer.md 8) and a capstone scan of Engine.dll by the lookup agents (the Ghidra MCP had Risen.exe open, not Engine.dll, so no new decompilation;
direct field-store sweeps were therefore only done for the world matrix). RVAs are Engine.dll RVAs. [C] read in code, [I] inferred.

| what a composed draw depends on | signal | RVA | hook sees all writers? | hotness |
|---|---|---|---|---|
| render contexts exist (entity in the world) | `eCGeometryEntity::OnCacheIn` (vt +0x1d0) / `OnCacheOut` (+0x1d8); inside: `eCFrustumElementItem::CacheIn` 0x670e60 creates the contexts, `eCPrefetchAdmin::OnEntityCachedIn` 0x696120 inserts into the scene sphere tree; `CacheOut` 0x670de0 destroys them | 0x6a9720 / 0x6a97b0 [C] | yes: only called from `eCPrefetchAdmin::OnCacheIn / OnCacheOut` (0x696590 / 0x696550) through the vtable; destroy is always preceded by cache-out (fatal error otherwise) | per streaming event (already hooked for lod.md) |
| entity deleted | `eCEntity::Destroy` 0x6a4180 -> `eCEntityAdmin::NotifyDeleteEntities` 0x695310 | [C] | yes (after cache-out) | rare |
| world matrix (entity +0x60) | `eCGeometryEntity::OnUpdatedWorldMatrix(new, old)` (vt +0x230; dynamic override 0x69e8a0, base 0x6a8d70) | [C] | every change through the setter chain (`UpdateChildDependencies` 0x69e530 <- `SetLocalMatrix` 0x69eee0; `SetWorldMatrix` 0x6a8e90). Bypasses: `CopyEntityPrivateData` 0x6a8820, base `SetLocalMatrix` 0x6a62f0 (non-dynamic subclasses), savegame `OnRead`, four unnamed chunks (0x4c77a5, 0x4c829c, 0x6e47a8, 0x6e80a5) [I] | once per matrix change of a moved entity (equal matrices early-out) |
| attachments / animated props | `eCActorAttachment::Process` 0x4d0f90 -> `SetToWorldMatrix` 0x6aaf80 -> the chain above | [C] | yes | per frame per attached entity |
| visibility / enable | `EnableRendering` 0x6a5d80 (flag 0x2), `Enable` 0x6a3670 / 0x6a96a0 (0x4) | [C] | yes | rare |
| mesh LOD | `eCRenderContextMesh::UpdateLoD` 0x5c74b0 stores ctx +0x28 (via item `Request` 0x671370, slot +0x30) | [C] | the only writer, but it runs per visible item per **database** per frame and shadow databases overwrite the same field: poll ctx +0x28 in the op (we read it anyway) | hot |
| fade | `CalculateFadeOut` 0x671220, recomputed every frame (< 1 moves the op to the alpha list) | [C] | no signal | per frame |
| material values | none: constant pools are written in place; the dirty range `pool +0xc/+0x10` is a CPU->GPU upload flag reset every draw; property notify reacts only to "FallbackMaterial" | [C] | - | - |
| material time elements (scroll, rotate, oscillate, animated sampler) | `eCShaderMaterialAdmin::Process` 0x6bb250 -> `SetTime` on every timed element | [C] | single caller | per frame |
| sampler image / register layout | `SetImageResource` 0x6c49f0 (+ UpdateSampler 0x6c3ab0), permutation compile funnel `CompileShaderPoolEllement` 0x6b8140 / 0x6b8890 (already hooked: engine/material_source) | [C] | compile: one funnel; `SetImageResource`: other writers unverified | rare for meshes |
| resource load / evict | `eCResource2::Load` 0x683540, `LoadData` 0x6828f0, `LoadDataV2` (mesh) 0x75e080, `FlushResource` 0x684340, `FlushBytes` 0x684250; state field `res +0x38` read directly everywhere; the 0 -> 3 store site not located | [C]/[I] | no: poll `res +0x38` / `image +0x70` (our composition already reads them) | event-rate |
| lighting | `eCIlluminationAdmin::Process` 0x579260 -> `UpdateLightSets` 0x575130 rebuilds every light set **every frame**, no dirty flag; per-entity `eCIlluminated_PS::UpdateIllumination` 0x595300 (from `Request` when bit 0x80); the lighting pool caches (list ptr, f, SH flag) at pool +0x48..0x54 | [C] | single entry per frame; field stores direct | per frame (sun / hemisphere / flicker) |
| SpeedTree wind / LOD / instance alpha | `UpdateWindMatrices` 0x6efa10 (once per DB execution, 3 matrices + 4 angle pairs -> c14..c29); LOD `0x6e8be0 -> 0x6f8f30` with camera distance | [C] | single callers | per frame |
| vegetation | `eCVegetation_PS::Render` 0x707800 gathers instances into the dynamic VB each frame | [C] | - | per frame |
| skin palette | `UpdateRenderMatrices` 0x4dfee0, stamp-guarded, frame id | [C] | single caller | per frame per visible actor |
| particles / strips / rain / decals / billboards / star vertices | CPU-built every frame | [C] | - | per frame |

Hook cost: `OnCacheIn / OnCacheOut` and `OnUpdatedWorldMatrix` are cold or per-change (vtable slots or real functions, not thunks / COMDAT-folded). The per-frame classes have no signal because they
have no *change*: they are pulls, and a pull cannot be cached.

## 2. Prior art in our code and why it did not pay

* `enh/rt_registry` / `rt_scene`: instance identity by sighting (quantised world rows + owner views, "moved" = a new instance replaces the unseen owner's latest one, "volatile ranges" = churn counters): no
  signals, a per-draw hash lookup (~0.35 ms `observe`). A world-matrix signal would replace its moved detection (the callback receives the old and the new matrix).
* `object_motion`: previous-frame vertex capture of moving draws; signal-compatible (the moved set is known).
* `native.retained` (V3, vulkanize/plan.md 7): memoises `materialDraws` / `specialPassDraw` by key + a **read log** (every engine read logged and compared at replay). Re-measured on the current build (savegame,
  lod 2x, 2 runs each): 6.530 -> **6.444 ms (-0.09 ms)**, hit rate 100 % (1,118 hits / frame, built 0, invalid 0, stale 0). Own timer: **replay 0.585 ms / frame = 0.52 us per hit**; the unretained
  `materialDraws` is 0.70 ms for the same calls (profile). Replay = switch writes (pool registers, slot images), `writeLighting`, `constantsInto` (all live registers from the mirror again), the water tag
  classification, image touches (`leanImage`: the resource-cache LRU touch the engine made every frame) and validation (~0.05 ms). **Validation was never the cost; the per-frame redo of our own work is.**

## 3. What each op class depends on

| op class | static per object (cacheable) | changes with a signal | per frame by nature |
|---|---|---|---|
| static mesh z / solid (`composeZ` / `composeSolid`) | shader / permutation, decl, streams, index ranges, states, sampler states, sub-mesh draw list | world matrix (moved), cache-out, mesh LOD (poll), lightmap combi (once), image load (poll) | view (c0..c11, fog), fade, light list + lighting pool c14..c37, sun / ambient, material time elements, image touches |
| trees (`composeTree`, ~650 batches / frame) | per type: shader, geometry ranges, states | cache-out (trees do not move) | wind c14..c29, LOD alpha / strip lengths (camera distance), batch grouping (sorted queue), view |
| tree billboards | UploadTexCoords tables, geometry | cache-out | view, fade |
| vegetation | node geometry | - | the whole instance buffer, cursor |
| actors | shader / geometry per body part | cache-out, attachments | palette, morphs, alpha, lights |
| particles / strips / rain / decals / sky | - | - | everything (CPU-built vertices) |
| shadow-view ops | same as their mesh / tree / actor op | same | the cascade view, per cascade |

A draw carries ~75 live registers; only the object block (world, instance rows) is signal-invalidated; VIEW, FRAME (wind, sun), LIGHT, TIME and the touches are per frame.

## 4. Estimate: composition-only retention with signals

Per frame at the savegame: ~1,118 retainable hits; Full - Wrapper = composition ~1.5 ms + bookkeeping `issue` 0.4 + shadow views / sky / effects.

| step | per hit | per frame |
|---|---|---|
| today, unretained `materialDraws` | ~0.62 us | 0.70 ms |
| V3 replay (measured) | 0.52 us | 0.585 ms |
| + signals instead of the read log (validation -> a stamp compare) | ~0.45 us | ~0.5 ms |
| + class-split patch (only VIEW / FRAME / LIGHT registers re-copied) and batched touches | ~0.3 us | ~0.33 ms |

Gain over today ~0.37 ms on `materialDraws`; the rest of the composition (op loops, `composeTree`'s per-instance world writes, sky, effects, lean commit) does not shrink. **Full - Wrapper ~1.6 ms.**
Composition-only retention is a no-go; the per-hit floor is what must happen every frame (three classes of constants, the lighting block, the image touches).

## 5. Where signals would matter: retaining the packet (not prototyped)

After composition a draw still costs `issue` (0.4 ms bookkeeping + setters) and the device path (`prepareDraw` 1.7 ms incl. observers, `computeVertexInput` 0.27, `prepareTextures` 0.13, constant upload 0.15; the
fingerprint test found only 17.8 % of consecutive draws identical, so neighbour comparison cannot shortcut it). A cached **per-draw packet** (pipeline, vertex input, push block, descriptor indices, texture list,
OBJECT / MATERIAL window blocks in a persistent buffer) stays valid as long as its object does (signals: cache-in / out, world matrix; polls: LOD, lightmap combi, image state, permutation-compile counter), and per
frame only the per-view addresses (VIEW / FRAME / LIGHT window blocks) are patched before it goes to the record threads. Estimate: ~0.25 us per cached draw (stamp check + 3 patches + enqueue) instead of ~3.1 us
(compose 1.5 + issue 0.4 + prepareDraw 1.2); for ~1,000 retainable of 1,290 draws about -2.5 ms, i.e. Full could end *below* Wrapper (the engine still walks and culls the op lists; the touches stay).
Risks: (1) every consumer hangs off `prepareDraw` (RT scene `observe`, replacers, object motion, G-buffer / GI variants, tint, coverage): with an enhancement active the draw takes the normal path or the consumer needs its
own signal feed (the RT registry's moved detection is exactly `OnUpdatedWorldMatrix`); (2) the window-block ABI: the OBJECT block must live in a persistent GPU buffer with per-object slots (today the ring, 2.3 MB / frame);
(3) correctness only checkable if the packet path can run "fresh" every N frames and be diffed (verify mode); (4) per-frame classes (particles, vegetation, actor palettes) stay on the old path, so only static meshes,
trees and billboards qualify. First stage: static-mesh z / solid packets with all enhancements off, verified against fresh composition; gain ~0.8 - 1.0 ms at ~800 draws if the estimate holds.

## 6. Verdict

* Signals: usable for lifecycle and movement (OnCacheIn / OnCacheOut / OnUpdatedWorldMatrix + polls); none for materials, lights, LOD, fade.
* Composition-only retention: **no-go** (measured replay 0.52 us per hit; ceiling ~0.4 ms; Full - Wrapper stays ~+1.6).
* Packet-level retention (bypass `issue` + `prepareDraw`): **conditional go as its own project** (estimate -1 to -2.5 ms; risk in the `prepareDraw` consumers); stage 1 = a per-draw cost measurement of the bypass on static
  meshes with enhancements off, not a signal hook.

## 7. Stage 0 of the packet retention: the ceiling, measured (2026-11-04)

Instrumentation (kept: `-Perf` only): `issue()` times itself and the device draw call inside it per group; rvk.log `native: <group>: issue cost (5 s, -Perf): ...`. Savegame, Full, 5 s windows (~742 frames),
timer overhead ~0.05 us per draw included. Vulkan / D3D12 are within 3 % of each other.

| group | draws / frame | us per draw in `issue()` | of it the device call (`prepareDraw` + submit) | bookkeeping (setters, save / restore) |
|---|---|---|---|---|
| static meshes z / solid (main view) | 232 | 2.10 (D3D12 2.13) | 1.71 (1.73) | 0.40 |
| SpeedTree + tree billboards | 680 | 1.56 (1.57) | 1.15 (1.16) | 0.41 |
| shadow views (all view kinds) | 160 | 1.59 (1.60) | 1.19 (1.21) | 0.40 |
| skinned actors | 21 | 1.77 | 1.16 | 0.62 |
| vegetation | 66 | 2.50 (2.24) | 2.00 | 0.50 |
| alpha / refracted meshes | 19 | 4.15 | 3.57 | 0.58 |
| particles / sky cube / decals / precipitation | ~30 | 3 - 13 | | |

Ceiling of the bypass at a replay cost of 0.3 us per draw (stamp compare + 3 address patches + window uploads + enqueue; **not yet measured**): static meshes alone 232 x (2.10 - 0.3) = **0.42 ms**
(1.8 ms per 1,000 draws), trees 0.86, shadow views 0.21, actors 0.03, vegetation 0.14, alpha 0.07: **~1.7 ms with every retainable class**, ~1.5 ms without vegetation / alpha. So the
"~-1 ms for static meshes alone" expectation does not hold (0.42 ms); the target (Full - Wrapper 1.97 -> ~0.3-0.5) needs trees + static + shadow views together, and the composition itself (1.5 ms) stays on top unless
the signals (section 1) let it be skipped too. Saved per 1,000 draws: ~1.3 ms for trees / shadow views, ~1.8 ms for static meshes. Not built: the replay itself (a persistent object-block slot, per-view window patching,
`beginRendering` / scope bookkeeping, record threads, the verify mode and the consumer switch-off are a Device-level change; see section 5).

## 8. Stage 1 result: the retained packet, measured (2026-11-04; `native.packet_cache`, commit 32b9956)

What it is: for the main view's static-mesh (z / solid) and SpeedTree / tree-billboard draws, `StaticOps::issue()` keeps the finished `DrawPacket` (+ its DrawBlock, the interned vertex-input /
dynamic-state **values** (the interning arena is per frame: pointers cannot be kept), the buffers' GPU bindings, the textures with their bindless slots, the viewport / targets) in
`Device::CachedDraw`, keyed by (op context, op desc, list, draw index, group) and valid while the composed `DrawDesc` is structurally equal (shaders, declaration, streams, ranges, states, samplers, tag;
registers are NOT compared: they are re-set every frame and the constant windows re-uploaded). `Device::replayDraw` skips `prepareDraw` and the state setters (decl, streams, indices, 10 render states, sampler states),
re-validates shaders / modules / buffer bindings / textures / env, rebuilds the per-frame parts (windows via `uploadConstants`, DrawBlock c5 + depth remap, draw id) and submits the packet. Restore
after the op only touches the states a normally drawn draw set. Cache refuses per draw while an observer observes the draw's kind / phase (RT scene, GI, object motion), a replacer handles it, tint /
ubershader / G-buffer / GI variants / upscaling jitter / logging are on; off for compare, tap, coverage runs. Setting values 2 / 3 verify (every 10th / every frame): the draw is made fresh and compared
with what the replay would have produced (pipeline, vertex input, dynamic state, ranges, flags, DrawBlock bytes): **4.9 M checks, 0 differing** (savegame, mode 3).

Measured (savegame, lod 2x, test-baseline settings = no RT scene / water, `-Perf` CPU = engine + HAL, 3 interleaved runs per arm):

| backend | Full, cache off | Full, cache on | Wrapper | saved | Full - Wrapper before -> after |
|---|---|---|---|---|---|
| Vulkan | 5.470 (5.479 / 5.451 / 5.482) | **5.145** (5.126 / 5.133 / 5.175) | 3.674 | **0.325 ms** | 1.80 -> **1.47** |
| D3D12 | 5.781 (5.654 / 5.589 / 6.099, the last disturbed; median 5.65) | **5.279** (5.253 / 5.291 / 5.293) | 3.820 | 0.37 - 0.50 ms | 1.96 (1.83 on the median) -> **1.46** |

~1,020 draws per frame are replayed (756,520 per 5 s), ~90 per frame are not capturable (uploading textures, dynamic buffers). **Measured replay cost: `replayDraw` 0.359 ms per frame = 0.35 us per draw (the 0.3 us
assumption was right)**: `replayBuild` 0.126, `uploadConstants` / `splitStage` 0.136, `submitInterned` 0.155. What is left of `issue()` per replayed draw is ~0.45 us: the register setters (`setVertexShaderConstantF`
0.22 ms + PS 0.04 for the whole frame, the compare + copy into the device's register file), the save / restore pass, the map lookup + structural compare (0.05 ms), plus the composition that still runs.
So the saved 0.33 - 0.4 ms is **not** the 1.3 ms of section 7: that ceiling came from the player-default run (RT scene `observe` 0.36 ms inside `prepareDraw`, 2.1 us per static draw); with enhancements off the
same draws cost 1.25 us in `issue()` (static 1.25, trees 1.27), of which replay removes ~0.4 us. **With the player defaults (water + RT reflections) the cache is refused for every draw (the RT scene observes
static meshes), so Full gains nothing there until the observer is fed from signals (stage 4).**

Gates (build 32b9956): `rvk_tests` 216 / 216; `rvk_d3d9test --validation` Vulkan + D3D12 0 failures / 0 validation errors; `testdemo` 7 / 7 with `-Validation` and `native.packet_cache=3` on Vulkan
wrapper / full and D3D12 wrapper / full, 0 validation lines; compare run (`static_ops_compare`, `scene_ops_compare`, `lean_check=10`, cache setting 3: auto-off) 26 groups `mismatches: none`;
`refcompare` Full + cache on vs `20261103-130010`: Vulkan beach_rain / jungle / pub_interior PASS, hero_closeup WARN, harbour_night FAIL (dluma -3.5, the known night noise); D3D12 the same set.
