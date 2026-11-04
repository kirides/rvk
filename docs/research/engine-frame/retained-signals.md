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

## 9. Writer sweep (headless Ghidra, 2026-11-04)

Method: headless Ghidra on the real project (`B:\DevDrive\risen-ghidra\Engine`, `DumpDecomp.java`), plus capstone scans of the test copy's `Engine.dll` / `Game.dll` (all RUNTIME_FUNCTION bodies disassembled; store scans by `[reg + disp]`, `lea rcx,[reg+0x60]` before a SharedBase `bCMatrix` mutator, call-site scans, vtable-slot scans over all 2,585 `vftable` symbols of Engine.dll). Scratch: `%TEMP%\sweep\*.py`, `%TEMP%\sweep_q*.txt`. RVAs are Engine.dll RVAs. CONFIRMED = read in decompiled code / disassembly, INFERRED = follows from the code but not run, NOT FOUND = searched, no hit (a scan can only prove absence for the patterns it covers; stores through other register / inline forms of a different compiler idiom are not excluded).

### 9.1 Entity world matrix (entity +0x60)

Virtual slots (read from the vftables): `+0x230` = `OnUpdatedWorldMatrix(new, old)`, `+0x240` = `SetLocalMatrix`. Only two vftables in Engine.dll carry them: `eCGeometryEntity` (`0x230 = 0x6a8d70`, `0x240 = 0x6a62f0`) and `eCDynamicEntity` (`0x230 = 0x69e8a0`, `0x240 = 0x69eee0`); no other vftable in Engine.dll inherits `0x6a8d70` (so `eCDynamicEntity` is the only concrete subclass there; Game.dll has no `*Entity` class with its own vftable, and imports only `SetWorldMatrix / SetToWorldMatrix / MoveToWorldNode / SetWorldPosition`, all of which go through the chain). `eCTemplateEntity` is a separate hierarchy (its `+0x230 / +0x240` are other methods: `0x6ad1d0 / 0x6add20`).

| writer | RVA | evidence | calls `+0x230` afterwards? | verdict |
|---|---|---|---|---|
| `eCDynamicEntity::UpdateChildDependencies` | 0x69e530 | `operator=(this+0x60, local)` at 0x69e594 / 0x69e63a, then `(*vt+0x230)(this, this+0x60, old)` (decompile lines `if (parent==0)`: unconditional; parented: only `if (!IsEqual)`), then recurses into children through `vt+0x138` | yes (parented case only when the matrix changed) | CONFIRMED |
| `eCGeometryEntity::SetWorldMatrix` | 0x6a8e90 | copy old, `operator=`, `(*vt+0x230)(this, this+0x60, old)`; no direct Engine.dll callers (Game.dll / Script.dll import it) | yes | CONFIRMED |
| `eCDynamicEntity::SetLocalMatrix` (vt `+0x240`) | 0x69eee0 | `if (!IsEqual(this+0x140, new)) { this+0x140 = new; vt+0x2d0; vt+0x138 (= UpdateChildDependencies); if parent: vt+0x130 }` | yes, via `+0x138` | CONFIRMED |
| callers of `+0x240` (all reach the line above) | `SetLocalRotation` 0x6a63b0, `SetLocalPosition` 0x6a64c0, `MoveToWorldNode` 0x6aae20, `SetWorldPosition` 0x6aaec0, `SetToWorldMatrix` 0x6aaf80 (actor attachments 0x4d127a, Game / Script), dynamic `CopyEntityPrivateData` 0x69f930 (from a template: `SetLocalMatrix` + name) | scan `call [reg+0x240]` (36 sites; the others are PS code in 0x4d1041 / 0x4d1e70 / 0x4d378e / 0x5cf360 (`eCMover_PS::OnPostRead`) / 0x5e320e, receiver class not verified) | yes | CONFIRMED for the named ones, INFERRED for the PS ones |
| `eCGeometryEntity::SetLocalMatrix` (base) | 0x6a62f0 | only `operator=(this+0x60, m)` (+ a "5 m down" warning); **no notify** | **no** | CONFIRMED bypass, but no instance uses it: no vftable except `eCGeometryEntity` itself holds 0x6a62f0 / 0x6a8d70 (INFERRED) |
| `eCGeometryEntity::CopyEntityPrivateData` | 0x6a8820 | `bCMatrix::operator=(this+0x60, src+0x60)` at 0x6a889c when `param_2`; reached from `eCDynamicEntity::CopyEntityPrivateData` 0x69f930 (tail jmp at 0x69fb33), which first calls `SetLocalMatrix` for a template source | **no** | CONFIRMED bypass (entity copy / spawn, the copy is a new entity: INFERRED not yet cached in) |
| `eCGeometryEntity::OnRead` | 0x6a89b0 | `bCMatrix::operator<<(this+0x60, stream)` at 0x6a8a06 (NOT in the old list); stream load of an entity, then PS `vt+0x250` loop | **no** | CONFIRMED bypass (load time) |
| `eCDynamicEntity::ReadSaveGame` | 0x6a0ae0 | `if (flag) { operator<<(this+0x60); operator<<(this+0x140); boxes; spheres }` (decompile lines 168-176), no `+0x230` / `+0x138` call anywhere in the function | **no** | CONFIRMED bypass (savegame load) |
| unnamed chunks 0x4c77a5, 0x4c829c | in `eCBenchmark::RunVertexPerformanceTest` 0x4c76d0 / `RunFillrateTest` 0x4c81c0 | cold `.pdata` chunks; `SetIdentity(newobj + 0x60)` on a `new` `eCGfxCamera` (vftable `eCGfxCamera`, +0x160 flag) | n/a | CONFIRMED **not entities** |
| unnamed chunks 0x6e47a8, 0x6e80a5 | in `eCSkydome_PS::Render` 0x6e4730 / `UpdateSkydomeEnvCubemap` 0x6e7ed0 | `operator=(this_02+0x60, ...)` where `this_02` = `eCGfxCamera::ms_pActiveCamera` / `skydome+0x3c0` (a gfx camera; `+0xe0` view-proj, `+0x160` dirty) | n/a | CONFIRMED **not entities** |
| other `lea rcx,[x+0x60]` -> `bCMatrix` mutators (Engine.dll scan) | gfx cameras (`eCGfxCamera` 0x500xxx, `eCCameraBase::CalcProjectionMatrix` 0x4f6cb0, 0x4f699f, 0x4f6eb8), shadow-map cameras (0x59222d in `eCDirectionalLight_PS::UpdateShadowMaps`, 0x59f532 in `eCCascadeShadowMap::RenderShadowMaps`, stride 0x168 = `eCGfxCamera`), `eCShadowMap*Light::operator=` | all gfx camera objects | n/a | CONFIRMED not entities |
| Game.dll | whole DLL scanned (same pattern) | zero `lea rcx,[x+0x60]` + `bCMatrix` mutator hits | n/a | NOT FOUND |

Notifications fan out in `eCGeometryEntity::OnUpdatedWorldMatrix` 0x6a8d70: `eCPhysicObject::SetWorldMatrix(this+0x58, new)` (unless a physics flag) and `vt+0x278(new, old)` on every property set of the entity (`eCIlluminated_PS::OnUpdatedWorldMatrix` 0x595d00 sets its dirty byte there: see 9.2). `eCDynamicEntity::OnUpdatedWorldMatrix` 0x69e8a0 first sets `this+0x1e0 |= 2` and `UpdatePortalRoom`.

Other `call [reg+0x230]` sites (37 in Engine.dll) belong to unrelated classes (`eCDX9Device`, image filters, decal admin, physics state buffer, template entity, `eCEntity::PickGizmo` on property sets): checked by decompiling 20 of them; the only two that reach the entity slot are 0x69e5ad / 0x69e652 (`UpdateChildDependencies`) and 0x6a8ed0 (`SetWorldMatrix`).

**World matrix: hook on `OnUpdatedWorldMatrix` (0x69e8a0 / 0x6a8d70) sees all runtime movement writers: partly.** Three load-time writers bypass it (`OnRead`, `ReadSaveGame`, `CopyEntityPrivateData`) and the unused base `SetLocalMatrix`. All three happen on an entity that is not cached in yet (INFERRED; `OnCacheIn` 0x6a9720 follows and builds the contexts), so `OnCacheIn` + `OnUpdatedWorldMatrix` together are sufficient *if* a packet is only built after cache-in and cache-in always re-reads the matrix. Safety net: compare the matrix (or its hash) once at cache-in and in verify mode.

### 9.2 Light sets / illumination object

The "illumination object" is `eCIlluminated_PS` + 0x78 (`GetShaderMaterialIllumination` 0x597660 returns `this+0x78`; entity `+0xf8` -> the PS, set by `OnListenEntityEvent` 0x595c20 event 3 and cleared on event 4). Layout relative to the PS (task offsets +0x78-based in brackets): `+0x78` pointer = `lightset+0x10` [+0], `+0x80` float f (fade of the static lights, eased by `eCTimer`) [+8], `+0x85` count of static lights [+0xd], `+0x88` two weighted static lights x 0x40 B [+0x10], `+0x108` SH flag [+0x90], `+0x10c` SH 0x70 B [+0x94], `+0x180` the `eCLightSet*` itself, **`+0x188` dirty byte**, `+0x28` the StaticIlluminated enum.

| item | function / RVA | evidence | verdict |
|---|---|---|---|
| dirty byte `+0x188` set to 1 | ctor 0x595e80 / copy ctor 0x595db0, `Destroy` 0x595270, `Invalidate` 0x5952d0 (no Engine.dll caller; exported), `OnCacheIn` 0x595cf0, **`OnUpdatedWorldMatrix` 0x595d00** | each is `this[0x188] = 1`; the store scan over all of Engine.dll finds `+0x188` byte stores in the PS only at 0x595297, 0x595b26 (the clear), 0x595e58, 0x595f1b, `operator=` 0x0ab7ef | CONFIRMED |
| dirty byte cleared | `UpdateIllumination` 0x595300 (cold chunk at 0x595b26): `this[0x188] = 0` at its end | decompile line `this[0x188] = 0;` | CONFIRMED |
| who calls `UpdateIllumination` | `eCFrustumElementItem::Request` 0x671370 (call at 0x6713b7, when request bit 0x80 and the item has a PS at `+0x20`) and itself (0x595383: the parent's PS) | call-site scan: those two sites only; no vtable reference | CONFIRMED |
| skipped when clean | whole body is `if (this[0x188] == 1) { ... }`; it is **not** skipped by "static illuminated" (`+0x28 == 1`): that value only selects the branch that recomputes the 2 weighted static lights + SH (`FindWightedStaticLights`, or copies from the parent's PS when the parent is also static); the light set is then re-queried (`QueryLightSet` / vt `+0x270`) and `SetLightSet`; static data read by `OnRead` 0x594e20 (`+0x85`, `+0x88` 0x80 B, `+0x108`, `+0x10c` 0x70 B, `+0x80`) is overwritten by the first recompute (dirty is 1 after construction) | decompile | CONFIRMED (skip rule), INFERRED (that the baked values are always overwritten) |
| stores to the list pointer `+0x78` / `+0x180` | `eCIlluminated_PS::SetLightSet` 0x5972e0 only (`*(this+0x78) = *(this+0x180) + 0x10`, `*(this+0x180) = set`); also copy / operator= | callers of SetLightSet: `UpdateLightSets` 0x575130 (call 0x575357, re-points every PS of an id-matched set), `eCDynamicLight_PS::UpdateEffectedObjects` 0x593630 (4 sites), `UpdateIllumination` (cold chunk 0x595af4), `OnCacheOut` 0x595ce0, `OnListenEntityEvent` 0x595c20 | CONFIRMED |
| contents of the light set | `eCLightSet::UpdateIllumination` 0x579c70: zeroes set `+0x10` (byte) / `+0x44` / `+0xc8..0x138` (0x70 B SH), then each light does `vt+0x2e8(light, set+0x10)`; called by `UpdateLightSets` (0x575505, every set, every frame), `RemoveEffectedDynamicLight` / `AddEffectedDynamicLight` / `SetEffectedDynamicLights` (0x5798eb, 0x57998b, 0x579f0d) | decompile + call-site scan | CONFIRMED: the list pointer in the PS stays, the *contents behind it* change in place every frame |
| `eCIlluminationAdmin::UpdateLightSets` 0x575130 | single caller 0x579480 = `eCIlluminationAdmin::Process` 0x579260 | call-site scan | CONFIRMED |
| lighting pool cache `+0x48` (set ptr) / `+0x50` (f) / `+0x54` (flag) of the pools `DAT_180c62950` / `DAT_180c62968` | filled by `FUN_1806cd1d0` (0x6cd1d0; stores `+0x48` at 0x6cd3c1, `+0x50` 0x6cd255, `+0x54` 0x6cd247) per draw from the op's illumination (`param_2+0x10`: reads `+0` list, `+8` f, `+0xd` count, `+0x10` lights `0x40 B` each, `+0x90` flag) and `FUN_1806b5bb0` (0x6b5bb0: SH, 7 registers from register 0xf, scaled by f); `+0x48` stores in `BeginZPass / EndZPass / BeginMaterial / EndMaterial / BeginRadiosityPass` (0x6b989c, 0x6b98ac, 0x6b995d, 0x6b9bd5, 0x6b9dde, 0x6b9f0e ...) write both globals (invalidate: values are registers, probably 0, INFERRED); ctor 0x6bca20 inits (0, 1, 1) | disassembly + decompile of 0x6cd1d0 / 0x6b5bb0 | CONFIRMED for the fill sites, INFERRED for the invalidation values |

Game.dll / Script.dll import none of `SetLightSet`, `Invalidate`, `ClearStaticLightData` (import scan).

**Light sets: hook on `UpdateIllumination` / `SetLightSet` sees all writers of the *pointer* and of the static data: yes (single-function writers + the dirty-flag signals above). It does NOT see the contents of the set (rebuilt in place every frame by `eCLightSet::UpdateIllumination`): no signal, a pull. Overall: partly.** Correction to section 1: the per-entity update is gated by a dirty byte (`+0x188`) that is raised by the world-matrix signal and cache-in, so `OnUpdatedWorldMatrix` already implies "static light data of this entity will be recomputed at its next `Request`".

### 9.3 Material pools (`eCShaderConstantPool`, 0x30 B)

Layout re-confirmed by the writers: `+4` first register, `+8` count, `+0xc / +0x10` dirty range (min / max, relative), **`+0x18` the float4 register data** (the task's "+0x38" is not the register data: the elements all index `*(pool+0x18)`), `+0x120` (on the shader) a sampler-dirty bit mask, samplers `+0x20 + slot*0x28`. Upload + reset of the range: `FUN_1806b5d20` 0x6b5d20 (called from `ExecuteShaderConstants` 0x6d2f90 and the admin's Begin / End calls).

| writer class | function / RVA | evidence | when | verdict |
|---|---|---|---|---|
| time elements | `eCShaderMaterialAdmin::Process` 0x6bb250 -> vt `+0x110` of every registered timed element: `eCColorSrcConstantAnimation::SetTime` 0x6c0480, `eCColorSrcSampler::SetTime` 0x6c53d0, `eCTexCoordSrcOscillator` 0x6dcf20, `Rotator` 0x6df170, `Scroller` 0x6e0c00, `eCShaderLightStreaks` 0x6d3e40, `eCShaderParticle` 0x6d59c0 | decompile (Process loop); dirty stores found by scan | per frame | CONFIRMED |
| switches | `eCColorSrcConstantSwitch::SetSwitch` 0x6c0c60, `eCColorSrcSampler::SetSwitch` 0x6c5310, `eCShaderBase::SetSwitch` 0x6d2840 (writes `+0x120` bit, registers) | decompile | on switch change | CONFIRMED |
| initial fill | element `UpdateShaderConstants` (vt `+0x140`): `eCColorSrcConstant` 0x6bfe90, `ConstantSwitch` 0x6c0e20, `Fresnel` 0x6c2280, `CamDistance` 0x6bda10, `CamObstacle::SetShaderConstants` 0x6be360, `TexCoordSrcBumpOffset` 0x6dc1e0, `Scale` 0x6e0630, base 0x6cdfd0; called from `eCShaderBase::UpdateShaderConstants` 0x6d28a0 and `eCShaderBase::OnRead` 0x6d21f0 (`vt+0x140(this, this+0x98, this+0xc8, this+0xf8)`) | decompile + dirty-pair scan (29 functions in 0x6b..0x6e hold both `+0xc` and `+0x10` stores) | material load / pool rebuild | CONFIRMED (callers of 0x6d28a0 are vtable / other shader classes: INFERRED load-time only) |
| **property change** | `OnNotifyPropertyValueChangedExitEx`: `eCColorSrcConstant` 0x6bfda0 (rewrites the colour register + dirty range), `CamDistance` 0x6bdaf0, `CamObstacle` 0x6be8e0 (-> `SetShaderConstants`), `Fresnel` 0x6c2340, `Mask` 0x6c2fb0, `CubeSampler` 0x6c19a0 (image + sampler dirty bit), `Sampler` 0x6c4e10 ("ImageFilePath": image, timed / switch registration, `UpdateSampler`), `TexCoordSrcBumpOffset` 0x6dc460, `Scale` 0x6e0850, **`eCShaderSkyDome` 0x6d7840 (rewrites VS registers 0x1f..0x22 and PS 6..7 of the sky pool from its properties)** | decompiled (0x6bfda0, 0x6bdaf0, 0x6c2340, 0x6c2fb0, 0x6c19a0, 0x6d7840 read in full) | editor / script property sets (not seen per frame by the game: INFERRED) | CONFIRMED: **section 1's "property notify reacts only to FallbackMaterial" is only true of `eCShaderBase / eCShaderDefault / eCShaderParticle` 0x6d27a0 / 0x6c81a0 / 0x6d58b0** |
| lighting registers (global pool) | `FUN_1806cd1d0`, `FUN_1806b5bb0` (see 9.2); `FUN_1806d7020` and `FUN_180766b80` also call `0x6b5bb0` (sky / other SH users) | decompile + call scan | per draw | CONFIRMED |
| samplers / images | `eCColorSrcSampler::SetImageResource` 0x6c49f0, `UpdateSampler` 0x6c3ab0, `ExecuteShaderConstants` (reads the resource `+0x38` state and `eCImageResource2 +0x70` per draw, `QueueData` when state 1/2, `CacheResource` when 3) | decompile | image events | CONFIRMED |

**Pool registers: no single hook sees all writers. Per-frame writers are exactly `Process` -> `SetTime` (+ switches, lighting fill, `ExecuteShaderConstants` upload); the property-change writers are many element handlers (all cold: property sets by script / editor). A poll of the pool's data (hash of `[+0x18, count*16]`) or the dirty range at compose time is the only complete detector. Verdict: partly.**

### 9.4 Resources (`eCResource2 +0x38` state)

States seen: 0 = not available / load failed, 1 = known but not loaded (ctor, flushed), 2 = queued for lazy (async) read, 3 = loaded, 4 = created at run time.

| transition | store site | evidence | verdict |
|---|---|---|---|
| **-> 3 (loaded)** | `eCResourceCache2::LoadData(resource, file)` 0x684e90, store at 0x6850af, after `eCResource2::LoadData` 0x6828f0 returned 0 (`*(res+0x2c) = size; *(res+0x38) = 3; cache bytes += size; CacheResource`) | decompile; the store scan over 0x67e000-0x6a0000 / 0x75e000 finds no other `= 3` | CONFIRMED: **single site** |
| -> 0 (failed) | the same function (0x685161), `LoadData(bCString)` 0x685180, `QueueData` file-not-found, `RequestResource` 0x687f33, `GetResource` 0x68191a / 0x681a22, `SpawnResource` 0x685836, `LoadQueuedData` 0x684d3f | store scan | CONFIRMED |
| -> 2 (queued) | `eCResourceCache2::QueueData` 0x6852d0 (stores at 0x68548a; `LoadMode 1`, async `OpenLazy`) | decompile | CONFIRMED |
| async completion | `eCResourceCache2::LoadQueuedData` 0x6848d0 (callers `eCResourceCache2::Process`, `eCResourceModule2::ProcessCaches`): finished lazy reads -> `LoadData` 0x684e90 (store 3) or `= 0` | decompile + callers | CONFIRMED that completion also ends in the single `-> 3` site; the engine thread runs it (callers are the per-frame cache Process: INFERRED) |
| **-> 1 (flushed)** | `eCResourceCache2::FlushResource` 0x684340 (store 0x684371; called by `FlushBytes` 0x684250, `ReloadResource`, `eCResourceModule2::FlushCaches`) **and an inlined copy in `eCResourceCache2::Flush` 0x688500 (store 0x688557)**; also ctor 0x683db0, `ImportResource` 0x685670, `InitResource` 0x686ac0 | decompile; both flush sites first call `vt+0x160` (OnUnloadData) | CONFIRMED: **two flush sites**, a hook on `FlushResource` alone misses `Flush`; the resource virtual `OnUnloadData` (`vt+0x160`, e.g. `eCImageResource2::OnUnloadData` 0x68b840) is called by both |
| -> 4 | `eCResourceCache2::CreateResource` 0x687e80 (store 0x687ea6) | store scan | CONFIRMED |

`eCResource2::Load` 0x683540 is **not** the data load: it reads the resource's header / description file ("GR01" magic, `bCPropertyObjectTypeBase::ReadObject`, sets `+0x28` size / `+0x48`), returns `eEResourceFileState`. The data load is `eCResource2::LoadData` 0x6828f0 (called only from `eCResourceCache2::LoadData` 0x684e90), which calls the class `OnLoadData` (image: 0x68b8f0).

Image data pointer `eCImageResource2 +0x70` (`eCGfxImageData*`; read by `ExecuteShaderConstants` per draw): written by `OnLoadData` 0x68b8f0 (store 0x68ba23 in its cold chunk), `OnUnloadData` 0x68b840 (null, stores 0x68b87b / 0x68b88e), `SetImageData` 0x68e2e0 (0x68e353; callers `eCGuiTexture2::CreateBitmap / SetImageData`, `eCIlluminationAdmin::PostInitialize / RestoreResources`), `RestoreResources` 0x68df80 (0x68e061, device restore), ctor 0x68c820 / dtor 0x68c790. `eCGfxImageData +0x60` (layers pointer) is written inside `eCGfxImageData` itself (`WrapAPITexture`, `Convert*InPlace`, `ComputeNormalMapInPlace`, `DestroyLayers`, `Destroy`, ctor and one unnamed helper at 0x75245d) - about 25 store sites, all inside the 0x750000-0x756000 image-data code.

**Resource state: the `-> 3` transition has one writer (`LoadData` 0x684e90) so a hook there sees it; the `-> 1` transition has two (`FlushResource`, `Flush`) plus the unload virtual; the image pointer has five writers (`OnLoadData`, `OnUnloadData`, `SetImageData`, `RestoreResources`, ctor / dtor). Verdict: partly (hook `LoadData` post + `OnUnloadData` for images, or keep polling `+0x38` / `+0x70`, which the composition already does).**

### 9.5 Mesh LOD (`eCRenderContextMesh +0x28`)

| writer | RVA | evidence | verdict |
|---|---|---|---|
| `UpdateLoD` | 0x5c74b0 | zeroes `+0x28` (`*(this+0x28) = 0`), then picks the loaded LoD (`res +0x38 == 3`) whose distance band contains the camera; only runs when the mesh PS has more than one LoD (`PS+0x6c > 1`), otherwise the field is left as is; may leave it **null** for a frame when no candidate is loaded; also prefetches the next LoD (`PrefetchData`) | CONFIRMED |
| `Create` | 0x5c7360 | `*(this+0x28) = *(meshPS+0x40)` (LoD 0 resource) | CONFIRMED |
| `eCMesh_PS::CreateRenderContext` | 0x5c5740 | initial `*(ctx+0x28) = 0` | CONFIRMED |
| copy ctor 0x175670 / `operator=` 0x1756c0 | | `*(this+0x28) = *(src+0x28)` | CONFIRMED |
| `Destroy` 0x5c72c0, `Invalidate` 0x5c72e0, dtor 0x5c7300, ctor 0x5c7330 | | `*(this+0x28) = 0` | CONFIRMED |

Section 1's "the only writer" is only true of the per-frame update. `UpdateLoD` is virtual (referenced from the vtable, no direct call site found); the shadow databases overwrite the same field (section 1). Verdict: **no (several writers, one per-frame): keep polling `ctx +0x28` (null is possible).**

### 9.6 Verdicts and differences to section 1

| signal | hook sees all writers? | note |
|---|---|---|
| world matrix | **partly** | runtime movement yes (`OnUpdatedWorldMatrix`); load-time bypasses `eCGeometryEntity::OnRead`, `eCDynamicEntity::ReadSaveGame`, `CopyEntityPrivateData`, unused base `SetLocalMatrix`; the four "unnamed chunks" are gfx cameras |
| light sets / illumination | **partly** | pointer and static data: single-writer functions + a dirty byte raised by the world-matrix signal and cache-in; set contents rebuilt in place every frame |
| material pool values | **no / partly** | per-frame writers only `SetTime` / switches / lighting fill; many cold property-change handlers (colour, fresnel, mask, cam distance, sampler, sky dome) rewrite registers |
| resource state | **partly** | `-> 3` single site; `-> 1` two sites + `OnUnloadData`; image `+0x70` five writers |
| mesh LoD | **no** | `UpdateLoD` per frame + `Create` / assign / zero |

Differences to section 1:
1. The four unnamed `.pdata` chunks are not entity code (benchmark and skydome `eCGfxCamera`); the real extra world-matrix bypass is `eCGeometryEntity::OnRead` 0x6a89b0 (not listed there).
2. Section 1's "property notify reacts only to FallbackMaterial" is wrong for the colour / texture-coordinate / sky elements (9.3); the pool data pointer is `+0x18`.
3. "Mesh LOD: the only writer" - `Create`, copy / assign and the zeroing in `Destroy / Invalidate` also write `ctx +0x28`; `UpdateLoD` can transiently null it.
4. The state `-> 1` has a second site (`Flush` 0x688500 inlines `FlushResource`); the "0 -> 3 store site not located" is now `0x6850af` in `LoadData` 0x684e90.
5. New signal for lighting that section 1 missed: `eCIlluminated_PS` dirty byte `+0x188` (set by `OnUpdatedWorldMatrix` 0x595d00, `OnCacheIn`, ctor, `Invalidate`, `Destroy`; cleared at the end of `UpdateIllumination`); `UpdateIllumination` runs only for dirty PSs, regardless of "static illuminated".
6. `vt+0x240` is `SetLocalMatrix` (the dynamic override 0x69eee0 notifies through `+0x138` -> `UpdateChildDependencies` -> `+0x230`); `eCDynamicEntity` is the only subclass overriding `+0x230 / +0x240` in Engine.dll.

## 10. Player defaults: the gap, and the observer feed (2026-11-04)

**Step 1, measured at `-PlayerDefaults`** (savegame, lod 2x, `-Perf`; the machine was shared, outlier runs dropped):

| | Vulkan | D3D12 |
|---|---|---|
| Full (2 runs) | 6.458 (6.445 / 6.471) | 6.763 (6.729 / 6.796) |
| Wrapper (2 runs) | 4.459 | 4.727 |
| **Full - Wrapper** | **2.00** | **2.04** |

Profile (`-CpuProfile`, ms per frame, Wrapper / Full): `RtScene::observe` **0.376 / 0.376** (D3D12 0.334 / 0.352): Full pays the observer exactly as Wrapper does, once per draw (the draws the tap drops
never reach it), so it is not part of the gap; it only blocks the packet cache. `prepareDraw` 1.575 / 1.750, `drawIndexedPrimitive` 2.088 / 2.279, `uploadConstants` 0.152 / 0.156, injected passes 0.26 / 0.27: the
same picture as the test baseline (gap 1.8 there), +0.2 ms from the enhancements' per-draw costs on our composed draws.

**Step 2, the observer feed (built).** Instead of moving the RT scene to signals (its registry reads the device's constant registers per draw and decides capture variants), the cache **replays the observers' input**:
`CachedDraw::query` keeps the `ReplaceQuery` the observers and opaque-observing replacers were shown when the draw was made fresh; `replayDraw` refreshes its tag / textures / mainView, runs `observes` + `observe`
(and `observeOpaque` of the water replacer for main-view solid static meshes) exactly as `prepareDraw` would, and refuses the replay (without asking the observers again: `skipObservers_`) if one of them now wants the draw's vertex capture
(animated trees within the range, object motion), which needs the capture pipeline variant. Replacers that `handles()` a draw still refuse. Verify mode additionally compares the cached query field by field with the fresh one (kind, phase, buffers,
strides, ranges, instancing, shaders, declaration, blend / alpha / depth flags, textures): the observers are pure functions of that query and the device's registers, which replay sets identically. 563 k verified draws at the
player defaults with RT scene + water on, 0 differing; the RT scene's own counters with the cache on and off agree (652 instances in the TLAS, registry 655, 561 instance sightings per frame, both).

Measured effect at the player defaults (cache on vs off, 6 interleaved runs each, machine shared with other agents: 2 - 3 runs per arm were disturbed by 1 - 9 ms): Vulkan per-draw timers `issue()`: static
2.84 -> 1.65 us, trees 2.01 -> 1.23 us (-1.19 / -0.78 us per draw, ~-0.8 ms per frame at 900 draws, on a loaded machine whose draws were ~35 % slower than on a quiet one); `-Perf` CPU per frame, paired good runs: -0.26 / -0.42 / -0.47 / -0.58 ms
(median ~-0.4 ms) on Vulkan; D3D12 pairs scatter between -0.5 and +0.7 ms (median ~0): not resolvable under this load. **Do not turn the cache on by default yet**: a quiet-machine run (or `-Perf` GPU-independent per-draw counters) is needed.

Gates (build with the observer feed): `rvk_tests` 216 / 216; `rvk_d3d9test --validation` both backends 0; `testdemo` 7 / 7 with `-PlayerDefaults -Validation native.packet_cache=3` on Vulkan wrapper / full and D3D12 wrapper / full, 0
validation lines; compare run 0 mismatches; `refcompare` Full + cache on: Vulkan jungle / beach_rain / pub_interior PASS, hero_closeup WARN, harbour_night FAIL (known night noise, dluma -3.8); D3D12 same set with harbour_night WARN.
