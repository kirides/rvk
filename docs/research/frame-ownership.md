# Frame ownership: from the composed engine frame (Full / N5) to an owned, GPU-driven frame (plan, 2026-11-06)

User decision (2026-11-06, firm): `-Mode full` eventually **owns the whole frame**, as GD3D11 does for Gothic: collecting, skinning, culling,
drawing, lighting, and later our own re-created shaders. Work iteratively; minor setbacks at intermediate stages (picture differences,
temporary perf regressions) are acceptable. Wrapper stays faithful; Full may change the picture but keeps the visual identity (sun flare,
eye adaptation, vivid colours). Low-end targets (AMD Radeon 680M iGPU, low VRAM) matter: the owned frame is GPU-driven and writes no output
nobody reads.

Status: **F0 in progress (2026-11-06): session 1 done, section 7.** Labels: [C] confirmed in docs / code / measurement, [I] inferred, [?] open (RE or measurement
needed). ms marked "est." are estimates from measured costs.

Sources: CLAUDE.md; gpu-scene.md (incl. 9.1-9.11); engine-frame/README.md 1-5, 17; B-frustum-db.md; D-contexts.md; retained-signals.md 9-13;
opt-side-effects-mesh.md / opt-side-effects-anim.md 0; native-renderer.md 1-14; shader-permutations.md; gd3d11.md 2.3-2.4; lod.md;
streaming.md 2; upscaling.md 9. Code checked: `src/rhi/rhi.h`, `src/rhi/d3d12/d3d12_device.cpp` (ExecuteIndirect signature),
`src/engine/scene_table.h`, `src/gpuscene/instance_buffer.h`, `src/enh/light_table.h`, `src/shader/uber.h`, `shaders/include/rvk_draw.hlsli`,
`shaders/rt/rt_skin.comp.hlsl`, `src/vk/context.cpp`, `src/d3d9/inject.h`.

## 0. Why the half-way steps failed, and what changes now

The blockers measured in gpu-scene.md 9.2-9.5 / 9.9 [C]:

1. **Batches are cut by permutations and pass structure**: light count / sun flag / lightmap combination pick the shader
   (`psIdx = lights + 4 (kind + 3 (combi + 2 sun))`), > 3 lights add additive passes; 88 % of solid tree batch cuts are the trees' own
   illumination (static lights, static SH).
2. **Per-instance lighting looked CPU-only**: `writeLighting` ~6 guarded reads per instance, +0.5 ms for ~1,900 instances vs ~650 batches.
3. **The cost is our per-op composition + issue** (~1.25 us composition, ~1 us issue per draw), not the engine's walk (~0, 9.1).

What is new: **owned shaders + owned lighting + suppressed op creation.**

* With our own material shader and a dynamic light loop, light count / sun flag / pass count stop selecting pipelines: batches can merge.
* Per-instance lighting splits in two:
  * per instance, signal-driven: the illumination's static lights, static SH, outdoor factor, recomputed only when the dirty byte
    `eCIlluminated_PS +0x188` is set (cleared at the end of `UpdateIllumination` 0x595300) [C, retained-signals 9.2];
  * per light set, rewritten in place every frame: sun, ambient SH, indoor colour, up to 2 dynamic lights (`eCLightSet::UpdateIllumination`
    0x579c70).

  So the GPU gets exact per-instance lighting from a per-instance illumination record (uploaded on the dirty signal) + a per-frame copy of
  the light sets: O(#sets) per frame, not O(#instances) [C, 7: exactly one eCLightSet is updated per frame].
* The per-op CPU cost goes away only if **the engine stops creating ops for owned content**. Hook point: the contexts' `RequestRenderOps`
  (vtable +0x40), which receives `(db, fade, caps)` after `UpdateLoD` / `UpdateIllumination` [C, B-frustum-db 2.5]: the engine still chooses
  visibility and LOD; we only stop the op append.

## 1. Target architecture

### 1.1 What the engine keeps, and how we read it

| engine provides | read by | how |
|---|---|---|
| entity lifecycle | `engine/scene_table` (built, M1a) | signals `OnEntityCachedIn` 0x696120, `OnEntityCachedOut` 0x696040, `NotifyDeleteEntities` 0x695310 [C] |
| world matrix | scene_table -> `gpuscene::InstanceBuffer` (built) | signal `OnUpdatedWorldMatrix` 0x69e8a0 / 0x6a8d70; load-time bypasses covered by the re-read at cache-in [C] |
| visibility candidates, per-view LOD, fade, portals | new hooks on `eCRenderContextMesh::RequestRenderOps` 0x5c75e0, SpeedTree 0x6ee4e0, SpeedTreeBB 0x6ee9b0, Vegetation 0x70d4f0, Animation2 0x4ddd00 | per frame per item and view (main + each shadow db); LOD read from `ctx+0x28` at that moment, so the shadow dbs' own LOD picks are right (fixes deviation #11 of B 5) [C] |
| static illumination (2 weighted static lights, static SH, flags) | illumination table | post-hook of `UpdateIllumination` when the dirty byte was 1; light-set pointer changes via `SetLightSet` 0x5972e0 [C] |
| outdoor factor f (`illum +0x08`, eased) | illumination record | poll for visible instances [? who eases it, when] |
| light set contents | light-set table | per-frame pull of the sets visible instances reference (contents change in place) [C] |
| world lights | `enh/light_table` (built) | `engine/lights::collect` + drawn-list notes [C] |
| material registers + textures | material table | fill at first use; per-frame poll of pools with timed elements (`Process -> SetTime`); residency via `LoadData` 0x684e90 post / `OnUnloadData` (vt+0x160) [C: writer catalogue 9.3 / 9.4] |
| animation pose | palette table | call / post-hook `UpdateRenderMatrices` 0x4dfee0 (stamp-guarded); today only `eCAnimation_PS::Render` calls it [C]: the owned frame calls it itself |
| SpeedTree wind | frame block | `UpdateWindMatrices` 0x6efa10 post: admin +0xac / +0x6c (c14..c29) [C] |
| grass wind, time, fog, camera | frame / view block | `eCVegetationAdmin` +0x44.., `eCGfxFog` (db+0x14c8), camera +0x15c0 [C] |
| streaming / residency | unchanged | `RequestRenderOps` touches resource + materials; the owned hook keeps `QueueData` / `CacheResource` and the busy counter `res+0x24` |
| simulation, physics, scripts, weather, particle simulation + CPU particle vertices, post-graph parameters (eye adaptation), GUI | unchanged | particles, strips, rain, sky, post, GUI stay on the N5.6 / N5.7 compositions |

### 1.2 GPU scene tables

All `rhi::Buffer`s addressed by `GpuRef` / `uint2` pairs (S1 rule); batched updates (one staging block per frame, one copy per contiguous run);
every 240th frame a read-back verify against the CPU copy (as `instance_buffer.cpp`).

| table | record | written | size |
|---|---|---|---|
| Instance (exists, 160 B) -> v2 128 B | world 3x4, prev 3x4, node sphere (`+0xb8` geometry / `+0x198` dynamic), illum id, flags (caster type, cast flags, overlay flags), generation | signals | 32k slots, 4 MiB |
| MeshPart | per (resource, sub-mesh / LOD): first index, count, vertex address (arena), stride, layout id, material id, local bounds | HAL buffer creation / first sight | ~10k |
| Layout | the declaration as pull offsets / formats (position, normal, tangent, colour, uv0..6) | once per declaration | < 100 |
| Material | VS c232.. `g_vShaderData`, PS c6..c13 `g_ShaderData`, bindless indices of s9..s15 + lightmap slots, sampler state indices, mask reference, blend class, double-sided, pack word, graph id (= pipeline class), version | first use + timed-pool poll | ~1-2k x ~256 B |
| Illumination | static lights 2 x 64 B, static SH 7 x 16 B, counts, f, light-set id | dirty signal | ~2-4k x 256 B |
| LightSet | sun c22..c24, up to 2 dynamic lights, SH c15..c21 base, indoor c14 | every frame | one record per frame [C, 7] x 320 B |
| Light + clusters (exists) | `RvkLight` 64 B, 16x9x24 near + 32x18x8 far grid | every frame | exists |
| View blocks | per view (main, 4 cascades, terrain map, atlas cells, RT): view / viewProj, frustum planes, camera, LOD factors (`lod.*`), fog, HiZ reference, jitter | every frame | KB |
| Draw templates | per (part, pipeline bucket): args template + instance-ref range (GD3D11's `CSBuildArgs` model) | when a part / LOD / material first appears | |
| Per-view visible list | (slot, part / LOD id, class bits) from the RequestRenderOps hooks; later GPU cull | every frame | ~2k x 8 B per view |
| Tree types / instances | per resource: visuals, materials, strip length / start per LOD, vertices per instance, leaf-LOD index ranges; per tree: slot + LOD record (CPU-read from `wrapper+0x2c/+0x48/+0x50/+0x58..0x64` first, GPU `ComputeLodLevel` port later) | type once; LOD every frame | |
| Vegetation nodes | per grid node: box, per type (start, count), illum id; raw 0x2c entries persistent | node build / cell cache-in [?] | |
| Palettes / posed vertices | per actor: full palette 3x4 (no 66-bone limit), remap, morph weights; posed buffer (packed position, oct normal / tangent; a previous copy only while a motion consumer is on) | every frame (visible + RT range) | |

### 1.3 Frame graph of the end state

Inserted at the existing points `BeforeDepthPrepass`, `BeforeSolid`, `AfterOpaque` + a new shadow-view point (pre-hook of `ExecuteShadowMap` 0x66f630).

| # | pass | GPU-driven | notes |
|---|---|---|---|
| 0 | CPU: engine `Process`, camera `Update` (extract -> `Request` with our hooks recording visible lists instead of creating ops) | - | the cull walk ~0 ms [C, 9.1] |
| 1 | upload: dirty instance / illum / material records, light sets, light table, palettes, tree LOD, visible lists | - | one staging block |
| 2 | compute: skinning (actors), light cluster build (exists), template args build | yes | |
| 3 | shadow views: cull per cascade (frustum + sun cylinder) -> compaction -> indirect z draws (static, trees, actors); terrain VSM only when camera / sun move (engine rule) | yes | point-light atlas: RT lamp shadows in Full (decided), else the engine's |
| 4 | main z pre-pass, phase 1: last frame's visible owned instances -> HiZ (SPD-style, reuse `ocean_mips.comp.hlsl`; reversed depth: min-reduce) -> phase 2: cull the rest against HiZ -> draw | yes | depth-only; R32F linear map see 1.6 |
| 5 | the engine leftovers' z ops (N5 composition) | no | |
| 6 | shadow mask chain (N5.5c or enhanced), RT shadows, lamp shadows, GTAO, TLAS from the tables | mixed | |
| 7 | solid: owned indirect colour pass at `BeforeSolid` (EQUAL / LESSEQUAL vs the pre-pass), then the engine's terrain layers, decals, billboards, sky dome (N5, in sort order) | yes | N3.3 order lesson [C]: base before the engine's blended layers |
| 8 | refraction copy, water (enh), refracted list, alpha list (alpha meshes, fading objects, particles, strips: N5), precipitation | no | |
| 9 | post chain (N5.7 of the engine's graphs: adaptation, bloom, sun streaks, tone map, DoF) -> FSR -> GUI -> present | no | keeps the visual identity |

### 1.4 Shader model

* Source: `shaders/material/ge_default.{hlsli,vert.hlsl,frag.hlsl}`, the engine's own HLSL ported and GPU-proven (PS 183/183 captured
  requests equal, `shader_equiv` PS 31/31, VS 17/17 incl. skinned, morphed, SpeedTree branches / fronds / leaf cards / leaf meshes /
  billboards, vegetation) [C].
* New front-ends in `shaders/scene/`: `scene_common.hlsli` (table records + fetches); VS `scene_static`, `scene_tree`, `scene_veg`,
  `scene_posed` (skinned actors read posed vertices); PS `scene_material` (`PS_Default`), `scene_z` (`PS_DefaultZPass`, masked); they
  `#include` ge_default with `RVK_SCENE=1`.
* Constant access: `g_ShaderData[i]` / `g_vShaderData[i]` become material-record reads, `g_SamplerData[k]` the record's bindless index +
  sampler state.
* Compile-time: the `XFX_PS_*` graph snippet (pipeline = graph text after normalising slot / register indices: materials of the same graph
  share a pipeline). Dynamic uniform branches: `XFX_DIRLIGHT`, `XFX_PNTLIGHTCOUNT` (loop over the illumination's <= 2 static lights, the
  set's <= 2 dynamic lights, then cluster lights the object lacks; one pass, no additive passes), `XFX_LIGHTMAP` (may stay a macro).
* Vertex pulling from `vk::MeshArena` by address + the layout record; the only IA input is the per-instance ref stream (section 4).
  Pipeline key: (graph, pass, blend, cull, geometry class); no vertex declaration.
* `ge_default_leafs.fx` (the leaf PS; its VS is the shared `ge_default_vs.fx`) is **ported** (2026-11-06, shader-permutations.md 11: `RVK_LEAFS` in `ge_default.frag.hlsl`,
  `uberPixelHlsl(file, ...)`; 11 of 11 PS EQUAL on the GPU, 17 of 17 requests instruction-exact against D3DX). Not wired to the run time
  (`uberSupports` stays phong-only until F4 passes the file). Not ported yet: particles, sky dome: translated until their stage.
* Look: `rvk_light_model.hlsli` (the engine's falloff, `pow(acc, 0.45)`, fog); `lighting.style` original / physical; D3D9 numerics stay
  (`powD3D`, `atan2D3D`, `normalize(0) = 0`).
* Expected picture change (accepted in Full): one pass per object instead of `pow` per pass + additive passes changes objects with > 3
  lights slightly; gate with `tools/sbs_diff.py` (luma / saturation / p99) against plain Full and Wrapper.

### 1.5 RT sharing

* One scene: TLAS instances = (instance slot, part) over every cached-in entity in RT range, off-screen casters included; BLAS per
  MeshPart; "moved" from the record's `version`. Replaces the per-draw `RtScene::observe` (0.376 ms per frame in both modes [C, retained-signals 10]).
* Skinned actors: raster and BLAS refits read the same posed buffer; `rt/rt_skin.comp.hlsl` takes the palette from the table instead of
  draw-captured bone constants.
* Trees: raster keeps wind in the VS; only trees in RT range get a pose compute (the same VS function as compute), replacing the `cap4`
  vertex captures.
* Motion: previous matrix / previous posed buffer = exact object motion for FSR / framegen without capture variants.

### 1.6 Bandwidth rules (hard rules of the owned frame)

1. No output without a reader (G-buffer / motion / previous posed buffers only while a consumer is on). Applies to today's renderer too
   (user rule 2026-11-06).
2. The owned z pre-pass is depth-only (RDNA's depth-only rate). The engine's R32F linear depth map is written once by a compute from the
   hardware depth after phase 2, and only while a reader remains (translated alpha / water / particle shaders sample it) [I: list readers].
3. Skin once per frame, not per pass (z + solid + up to 3 cascades = up to 5x VS skinning today) [I].
4. Packed records: instance 128 B, posed vertices <= 32 B, lamp-shadow image only while lamp shadows are on.
5. Compaction gives exact counts (count buffer): no zero-instance draws.
6. Smaller scene HDR target [?]: the scene alpha is read by the bright pass ("sky alpha", F 2.3); R11G11B10 only once that use is understood
   or moved.

## 2. Stages (each shippable behind settings, Full only)

Proposed settings (Debug, run time where possible, default 0 until the stage passes its gates; frozen into `namespace frozen` once default,
settings-audit rule): `native.owned_groups` bits (1 static main view, 2 static shadow views, 4 trees, 8 tree billboards, 16 vegetation,
32 actors, 64 terrain layers, 128 GPU part / instance cull, 256 same-frame HiZ) and `native.owned_check=N` (every Nth frame the original
`RequestRenderOps` runs, its ops are composed by N5 and dropped, as `lean_check`; op set diffed against the recorded visible list, composed
registers against the table records). Fallback stays: an entity whose template / pipeline is not ready falls through to the original
`RequestRenderOps` and N5 draws it that frame.

| stage | scope | engine code it stops calling | verification | risk | setback accepted | CPU (Full, est.) |
|---|---|---|---|---|---|---|
| **F0 foundations** | MeshPart / Layout / Material / Illumination / LightSet tables + upload + read-back verify; RequestRenderOps hooks in *observe* mode (ops still created, lists recorded); RHI: non-count `drawIndexedIndirect` fallback, instance-rate ref stream parity test | nothing | lists vs op lists per view 0 diff; tables vs composed registers 0 DIFFERING (check frames); DXIL gate; `rvk_rhitest` indirect parity | low | +0.1-0.2 ms while observing | +0.1 |
| **F1 owned shaders on the CPU path** | N5's static z / solid ops draw with scene shaders reading the tables (push = slot, part, material, view); composition shrinks to the pipeline pick | `GetPasses` / `ExecutePass` composition, register setters of static ops | offline `shader_equiv` / `ubershader_check` style (translated + CPU windows vs scene shader + tables, random data, both backends); in-run A/B on frozen rvkcompare spots, `sbs_diff.py` vs F0 | medium (material register layout, 3.1) | single-pass lighting picture change | -0.2 .. -0.3 |
| **F2 static meshes GPU-driven, main view** | suppress mode: no z / solid op for owned parts (fade 1, not blended / refracted, no overlay flags, not hierarchy-node meshes [?]); templates + compaction + indirect z at `BeforeDepthPrepass`, colour at `BeforeSolid`; instanced resources (`res+0xa0`) become ordinary instances | the `eCRenderContextMesh` op append, slots 0x5c7110 / 0x5c7410 for those parts, `eCSubMesh::Render`, `ExecuteRenderInstances`, our `composeOp` / `issue` / packet cache for them | check-frame op diff (false negatives 0); `native.coverage` class "ours (GPU)"; refcompare Full (known FAILs monastery_hall / crystal_cave / pub_interior); `-Validation`, sync validation, D3D12 | medium-high (residency touches, layer order) | GPU +0.1 ms compaction | -0.6 .. -0.8 (static 0.84 [C, 9.3]) |
| **F3 static shadow views** | same templates for cascades + terrain map; candidates from the shadow dbs' RequestRenderOps (caster callbacks applied), cylinder cull per cascade; new injection point at `ExecuteShadowMap` | shadow z-op append for static parts, `RenderZPass` slots | `shadows.debug=cascades`, `shadow-compare.ps1` | low-medium | | -0.3 (shadow views 0.5 [C]) |
| **F4 trees + tree billboards** | per type x geometry type x LOD bucket indirect draws, per-instance + illumination records (merges across illuminations become exact), wind from the frame block, `MaskReference` 80 kept; port `ge_default_leafs.fx` | SpeedTree queue build / op append, `RenderBranches` / `Fronds` / `Leafs` (+ ZPass), `composeTree` (0.78 ms), tree issue (0.88 ms) | VS / PS equality offline (`--billboard-valid`); check frames: queue vs instance list; jungle / forest spots, `rvk_rtwalk` sway | high (LOD strips, leaf LOD blend) | CPU-read LOD at first; small sway / LOD differences | -1.2 .. -1.6 (trees ~1.8 [C]) |
| **F5 GPU culling** | per-part frustum cull, two-phase same-frame HiZ for the main view, per-instance tree LOD on the GPU (`ComputeLodLevel` port) | - | false negatives 0 vs the engine set; pop-in check (bursts while turning) | medium | GPU +0.05-0.1 ms HiZ; gain depends on occlusion (old probe 8-24 % hidden [C]) | ~0 CPU; GPU gain on the 680M |
| **F6 actors** | palette upload (full skeleton + remap), morph targets in compute, posed buffers; z / solid / shadows draw posed vertices; body parts as separate actors; overlay passes stay on the engine path | `eCAnimation_PS::Render` / `HRA::Render` for owned ops (`UpdateRenderMatrices` called by us) | `rt_skin_verify`-style compare; hero_idle / pub_idle (`freeze=false`); `rvk_spells` | medium | | -0.1 CPU; skinning once; exact motion |
| **F7 vegetation** | persistent per-node raw-entry buffers, node table, GPU node + per-instance cull with fade c31 / wind c12, indirect per (type, illum bucket) | `eCVegetation_PS::Render`, the repack (410 KB / frame [C]), the cursor logic | grass spots pixel stats; density setting later (Detail) | medium (node lifecycle [?]) | | -0.1 .. -0.16 [C bound] |
| **F8 RT from the tables** | TLAS from the instance table + posed BLAS; tree pose compute in RT range; `RtScene::observe` off for owned classes | draw observation / captures for owned classes | `rt.debug=depth / instances` probes (no red / magenta), lamp / RT shadow A/B | medium | | -0.3 .. -0.4 (observe 0.376 [C]) |
| **F9 terrain layers + decals** (optional) | blended terrain layers in our solid pass in sort order, then single-pass layer blending | layer ops | jungle / beach / town ground crops | medium ([?] bit 0x200, sort ids) | | small CPU; GPU + bandwidth gain |
| **F10 collection takeover** (optional, last) | GPU frustum cull of the whole table for the main view; `Request` skipped; side effects done by us; residency touches from a GPU visible-set read-back (2-frame lag + margin, GD3D11 model) | `ExtractFromFrustum`, `Request` | M3 side-effect diff (entity / resource fields) = 0 | high | | ~0 [C, 9.1: the walk ~0]; architectural |

Order: static first (F1-F3: proven shader set, the richest compare tools `native.static_ops_compare` / `lean_check`, builds the tables, ABI,
templates, ref stream, buckets and the shadow injection everything else reuses); trees second (F4: the largest cost ~1.8 ms but more
unknowns: leaf PS port, LOD strips, leaf-LOD alpha pairs, wind constants). Shaders before merges (F1 before F2 / F4: merges are legal only
once light count / sun flag / passes no longer pick pipelines); lighting before per-instance draws (illumination + light-set tables in F0,
proven in F1). Actors and grass are small on the CPU, actors early for RT / motion sharing (F6 before F8). Sky cube, particles, strips, rain,
post, GUI stay on N5 (CPU-generated vertices; the sky cube stride of performance.md 27 is the cheap lever).

## 3. Engine unknowns per stage, and keeping side effects

### 3.1 Per stage

| stage | unknowns / to verify |
|---|---|
| F0 / F1 | Material macros + register map without an engine draw: with ops suppressed the engine never calls `GetPasses` -> `CompileShaderPoolEllement` 0x6b8140, so no request is captured. Plan: a one-time call of the engine's compile path per material (the O2c "compile at the slot's place" machinery exists), or `eCShaderDefault::GetDeclarations` 0x6c8650 + `SetupShaderMacros` 0x6d06b0 / 0x6b8ac0 directly [? side-effect free]. Graph register allocation: a compile re-walks the graph into the pools / sampler slots (resets `+0xc1/+0xf1/+0x21b`) [C]; whether `g_ShaderData[i]` is stable across a material's permutations [?]. Number of light sets and illumination dirty rate per frame [?]. Who eases `illum +0x08` [?]. |
| F2 | `RequestRenderOps` 0x5c75e0 side effects to keep: `QueueData` / `CacheResource` of res and every sub-mesh material, busy `res+0x24`, the instanced render-instance list (only if the engine still draws instanced ops) [C]. Hierarchy meshes (`ctx+0x34 > 0`, per-node box ops) in shipped data [?]. `eCShaderDefault +0x2b8` (additional sort id) [?]. Render-side touches of the slots: material images (`ExecuteShaderConstants` 0x6d2f90), lightmaps (`BindLightmaps` 0x764220), overlay materials [C, opt-side-effects-mesh 0]: done per visible part per frame (`static_ops_lean.cpp` has the calls). Stream-3 flag (only for leftover engine draws). |
| F3 | Shadow dbs' candidate rules (cull callbacks 0x59ec60 / 0x592300 / 0x5a0150) [C]; LOD from the shadow camera [C]; fronds-in-shadow condition `rb+0x3c <= rb+0x3a` [?]. |
| F4 | SpeedTree `RequestRenderOps` stamps `rb+0xe0/+0xe4` and the queue (lazy in-slot sort + stamp reset) [C]; `UpdateWindMatrices` once per db execution (keep) [C]; instance data `.y` (`rb+0xa0/+0xa4`) and the leaves' `wrapper+0x34` [?]; leaf-LOD pair `tree+0x58/+0x5c` with alphas `80/alpha` [C]; `ComputeLodLevel` with its static camera position `DAT_180c5bd38` and approximate sqrt (GPU match) [C formula, ? exactness]; readers of the wrapper LOD fields outside rendering [?]; the `ge_default_leafs.fx` port. |
| F6 | ActorInstance `+0x250` LOD writer (EMotionFX) [?]; whether world matrices are current for actors the engine does not render (`UpdateTransformations` from `Process` [I]); morph setup `+0x1d0`; `DeformBodyParts`; the blink morph; attachments are mesh entities (matrix signal) [C]. |
| F7 | Grid node lifecycle (cell cache-in / out: no signal known) [?]; entry fields `+0x10..0x27` [partly C]; `GetIlluminationAt` per node -> illumination refresh rule [?]; `SortGridNodes` keys [C]. |
| F9 | Request bit 0x200 (blended terrain layers in solid) [?]; `TradeAlphaAsSolid`; layer sort ids vs base [?]. |
| F10 | The full `Request` 0x671370 catalogue (M3): `UpdateLoD` 0x5c74b0 (+ `PrefetchData` of the wanted LOD), `UpdateIllumination` (Dynamic only when requested), visible stamp `item+0xc` / `entity+0xe4` (decals read it), `CalculateFadeOut` 0x671220, portals / occluders in the shipped world [?], leaf flag bit 2 [?], pre-pass requests (dynamic lights, `AddShadowCells`). |

### 3.2 Keeping side effects intact

1. **Suppress only the op append.** Safest: call the original `RequestRenderOps` into a **scratch op list** (swap `db+0x1500/+0x1518` for the
   call, discard the ops), so every touch and stamp happens bit-exactly [I: costs the engine's own ~0.1-0.3 us per item; measure; if too slow,
   reproduce from the catalogue].
2. Per visible part per frame the owned frame does the render-side effects the skipped slots did: image / lightmap / overlay touches,
   `MaskReference` 80, the billboard queue reset (if billboards are owned), `UpdateRenderMatrices`.
3. Check frames (`native.owned_check=N`): the original path runs, its draws are dropped; diff op sets, LRU stamps (`res+0x20`), busy counters,
   queue stamps, wind stamp, palette stamp: must stay 0 (as `native: effect units lean` "fields differing").
4. Mode switches (Wrapper <-> Full at run time): the signal hooks are always installed (`scenetable::install`), so the tables are complete; on
   switch / load a resync walk of the scene sphere tree (`pa+0x2a8`, exactly the cached-in set [C]) rebuilds them.

## 4. Both backends: indirect draws and the per-draw index

| | Vulkan (1.3 + push descriptor, dynamic rendering) | D3D12 (SM 6.6, fixed root signature, <= 128 B push) |
|---|---|---|
| indirect call | `drawIndexedIndirectCount` (RHI has it; optional `drawIndirectCount`, detected [C]); fallback `vkCmdDrawIndexedIndirect` with the CPU max count (add to the RHI; needs `multiDrawIndirect`), then a CPU loop | `ExecuteIndirect` with a `DRAW_INDEXED` signature + count buffer (exists, `drawIndexedSignature`) [C] |
| per-draw index (**chosen, both**) | **per-instance-rate vertex stream = the compaction output** (`uint2 ref : INSTREF` = (instance slot, template id)); args `firstInstance` = prefix offset; needs `drawIndirectFirstInstance` (checked with `multiDrawIndirect` in `features_.indirectDraws` [C]) | the same stream; `StartInstanceLocation` offsets per-instance IA fetches; no root arguments, no SM 6.8 |
| not used | `gl_DrawID` / `BaseInstance` (`shaderDrawParameters`, Vulkan-only); `SV_InstanceID` lookups (DXC maps it to `InstanceIndex` incl. firstInstance unless `-fvk-support-nonzero-base-instance`; D3D12 excludes it: semantics differ [I, verify]) | `D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT` (root constant per command: D3D12-only, root-signature-bound command signature); `SV_StartInstanceLocation` (SM 6.8) |
| vertices | pulled by address from MeshArena / a u32 index arena (`native_index_arena.*` in `git show 23d60f2^`); args `vertexOffset = 0`, base vertex from the template (Risen's sub-mesh indices are absolute [C]); parity test done: D3D12 `SV_VertexID` ignores the base vertex, Vulkan `VertexIndex` includes it, so pulling by `SV_VertexID` is portable only with `vertexOffset = 0` and absolute indices [C, 7] | same |
| per-bucket data | push constants (view, table refs) before each indirect call | root constants b0 before each `ExecuteIndirect` |
| buffer states | `bufferBarrier(.., VertexInput / Indirect)` after the compute | UAV writes, then transition to vertex buffer + indirect argument (RHI binding calls [C]) |

Gates on both: `rvk_shadertool dxil-check` for the scene shaders, a new `rvk_rhitest` case (indirect draw + ref stream + vertex pulling: equal
images), `rvk_d3d9test --validation` both backends, `-Validation` / GBV short runs.

## 5. Low-end path (Radeon 680M: RDNA2, 12 CU, ~100 GB/s shared, small carve-out)

| owned-frame property | why it helps the 680M |
|---|---|
| same-frame two-phase HiZ (F5) | cuts vertex + raster work for houses, interiors, trees behind terrain; no 2-frame lag, no pop-in |
| depth-only pre-pass, R32F map only if read | RDNA's depth-only rate; -4 B/px colour export in the heaviest pass |
| one lit pass per object (cluster lights, no additive passes) | fewer overdraw passes on lit surfaces |
| skin once per frame, posed buffer read by z / solid / cascades | up to 5x fewer skinning VS evaluations |
| grouped buckets (graph-shared pipelines) | fewer pipeline binds; CPU on a mobile Zen 3+ too |
| GPU grass cull + distance thinning (F7) | lower far-field density without CPU repack |
| no unread outputs (1.6) | prev posed / G-buffer / motion only with consumers; RT passes "off = nothing exists" |
| cascade caching option [I] | static casters re-rendered only when sun / camera move past the snap, dynamic casters on top (measure copy vs draw cost; `debug.shadow_stride` already halves updates while still [C]) |
| 128 B records, packed posed vertices | VRAM carve-out and bandwidth |

## 6. Risks, fallbacks, first steps

### 6.1 Risks and fallbacks

1. Residency / side effects of suppressed ops: the scratch-list call (3.2.1); check frames diff the fields, any non-zero diff turns the group off.
2. Material macros / register map unavailable without an engine draw (3.1 F0): the engine creates the op for that part until a one-time
   compile captured the request (per-part fall-through).
3. Bucket explosion: if distinct graph texts far exceed the ~400 estimated [I, shader-permutations 2], cap the bucket count; the long tail stays on N5.
   Measured: 196 pipelines (graph x static macros), well below ~400 [C, 7.3]; the risk is reduced, not gone (long tail of graphs).
4. Too many light sets for a per-frame copy: measured not the case, one set per frame [C, 7.2]; the fallback (deduplicated visible sets, or
   per-object reads for trees with dynamic lights) is not needed.
5. Picture drift (single pass, weights): accepted in Full; gates `sbs_diff.py` (mean |d| vs noise floor, luma / saturation / p99 for the flare)
   against plain Full and Wrapper at pub_interior, monastery_hall, harbour_night, jungle, crystal_cave; visible loss of vividness / contrast /
   flare = failure (9.6 rule).
6. Pipeline compile hitches: background PSO builds (exist); per-template fall-through to N5 while compiling.
7. D3D12 semantic differences: parity tests before any in-game use.
8. Tree LOD / wind mismatch: CPU-read LOD first; the GPU LOD port only in F5, bit-compared on check frames.
9. Two paths: N5 stays the fallback and the verification reference; owned groups replace it class by class; `native.packet_cache` keeps
   serving the non-owned classes.
10. No CPU gain at a stage (gpu-scene.md 8 risk 3): every stage gets a clean paired `-Perf` timing (savegame recipe, interleaved arms); a stage
    without gain may stay on only if it unblocks the next.

### 6.2 First concrete steps

**Session 1: measurements + RHI parity (no picture change).**
1. Count hooks on `UpdateIllumination` 0x595300 (pre: dirty byte), `SetLightSet` 0x5972e0, `eCLightSet::UpdateIllumination` 0x579c70; per 5 s:
   sets alive / referenced by visible instances, dirty illuminations per frame, f changes. Decides the light-set table design.
2. Graph-text census from `debug.material_sources` (`sources.jsonl`): normalise the `XFX_PS_*` snippets (slot / register indices ->
   placeholders), count distinct texts weighted by visible draws = the bucket / pipeline estimate.
3. `rvk_rhitest` case: indirect indexed draw + per-instance ref stream + vertex pulling, equal on both backends; add the non-count
   `drawIndexedIndirect` to `rhi::CommandList`.

**Session 2: F0 tables + observe hooks.** `src/gpuscene/` grows `mesh_parts.*`, `materials.*`, `illumination.*`, `light_sets.*` (each with the
read-back verify of `instance_buffer.cpp`); `src/engine/request_hooks.*` (or `scene_hooks.*`): the mesh `RequestRenderOps` 0x5c75e0 detour in
observe mode recording per-view visible lists. Gate: lists equal the op lists of `native/frame_tap` (0 diff over a long savegame run, both
backends); tables vs the composed registers of N5 (`static_ops.cpp` composes every register).

**Session 3: scene shader ABI v1 + F1 offline.** `shaders/scene/scene_common.hlsli` + the static VS / PS front-ends over ge_default with
`RVK_SCENE=1`; `tools/shader_equiv.py` / `rvk_shadereq` extended to "translated + CPU windows" vs "scene shader + tables" on random data; DXIL
gate; then F1 for static z / solid behind `native.owned_groups=1` in check mode only.

Critical files: `src/native/static_ops.cpp` (+ `static_ops.h`, `static_ops_lean.cpp`, `static_compose.*`), `src/gpuscene/instance_buffer.cpp`
(+ `src/engine/scene_table.cpp`), `shaders/material/ge_default.*` (+ `src/shader/uber.cpp`), `src/rhi/rhi.h` (+ `src/rhi/d3d12/d3d12_device.cpp`,
`src/vk/context.cpp`), `src/enh/light_table.cpp` (+ `rvk_light_model.hlsli`, `light_cluster.comp.hlsl`); supporting: `src/engine/scene_hooks.cpp`,
`src/native/static_ops_trees.cpp` (F4), `src/native/static_ops_actors.cpp` + `shaders/rt/rt_skin.comp.hlsl` (F6), `src/enh/rt_scene.cpp` (F8),
`src/hal/vulkan/device.cpp` (`frozen` constants, mode wiring).

## 7. Status

F0 session 1 (measured 2026-11-06; labels as above, [C] = measured).

### 7.1 RHI indirect draw (commit 5046f4d)

* `rhi::CommandList::drawIndexedIndirect(args, offset, drawCount, stride)`: Vulkan `vkCmdDrawIndexedIndirect` (a loop of single draws without
  `multiDrawIndirect`); D3D12 `ExecuteIndirect` with the `DRAW_INDEXED` signature, no count buffer. `DeviceCaps` gained `multiDrawIndirect`,
  `drawIndirectFirstInstance`; D3D12 `toDxgi` gained `R32G32_UINT` (the `uint2` ref stream).
* Test `testIndirectPull` (`tools/rhitest/main.cpp`, `shaders/test/rhi_ind.{comp,vert}.hlsl`): compute-written indirect records + count,
  instance-rate `uint2` ref stream, vertex pulling by address, drawn with both indirect calls; equal to a CPU expectation on Vulkan and D3D12,
  also with validation.
* [C] **D3D12 `SV_VertexID` ignores the base vertex (`vertexOffset`), Vulkan `VertexIndex` includes it**: vertex pulling by `SV_VertexID` is
  portable only with `vertexOffset = 0` and absolute indices (section 4).
* [C] `firstInstance` offsets instance-rate fetches on both backends; `SV_InstanceID` is not used.

### 7.2 Illumination census (commit 920df7a)

`src/engine/illum_census.*`, `RVK_ILLUM_CENSUS=1`; hooks by decorated name on `SetLightSet` 0x5972e0, `eCIlluminated_PS::UpdateIllumination`
0x595300, `eCLightSet::UpdateIllumination` 0x579c70, `UpdateLightSets` 0x575130; draw-side counts in `StaticOps::writeLighting`. Full mode,
rvkcompare `harbour_docks` / `pub_interior` / `monastery_hall` and savegame `Risen_Save_0005` (jungle, lod 2x, packet cache off).

| measurement | result [C] |
|---|---|
| `eCLightSet` updated per frame | exactly 1 (town run: 1 distinct in total; jungle: 2, only during the load) |
| light lists referenced by drawn ops | 1 distinct per frame (also trees, vegetation) |
| distinct illuminations drawn per frame | ~300 harbour_docks (815 lit passes), 37 pub (130), 25-36 monastery, 111 jungle (314) |
| SpeedTree illuminations | 147 town, 33 jungle |
| `eCIlluminated_PS::UpdateIllumination` calls per frame | 349 town, 307 jungle |
| dirty byte set on entry | ~0 per frame in steady state; bursts at cache-in / teleport up to 943 |
| `SetLightSet` per frame | 0 in steady state; bursts at transitions |
| f (`illum +0x08`) changed | 0 per frame while still (a lower bound: frozen spots / held savegame) |

Consequence: the LightSet table is one record per frame; per-object lighting = illumination records uploaded on the dirty signal, so exact
per-instance lighting for the GPU path is cheap. Resolves the light-set count [?] of sections 0 / 1.2 and risk 4 of 6.1. Still open: who eases
`illum +0x08` and when (f changes were only measured while the view stood still).

### 7.3 Material graph census (commit c0b8a23)

`tools/material_graph_census.py` over `out/matsrc/census_town.jsonl` + `census_jungle.jsonl` (`debug.material_sources`; weights = requests,
the records carry no draw counts).

* 453 requests (230 town, 223 jungle, only 61 shared); 147 distinct normalised graphs (slot / register indices -> `#`; constants never split
  graphs; alpha-renaming adds 3); 99 % of requests need 143 graphs: a long tail, no dominant set.
* Static macros that split pipelines: ALPHABLEND, BILLBOARD, COMPRESSEDNORMALMAP, DISABLELIGHTING, DOUBLESIDED, FOG, FRESNEL, INSTANCED,
  LIGHTINGTYPE, MASKED, MORPHED, QUALITY (always 2), SCENEDEPTH, SHADERMODEL_3, SHADOWMAPPASS, SKINNED, SPECULAR, ST_*, TANGENTSPACE(_EYE),
  VARIANCESHADOWMAP, VEGETATION, ZPASS (112 combinations).
* Dynamic (uniform branches in the owned shader): DIRLIGHT, DIRLIGHTSHADOWS, PNTLIGHTCOUNT 0-3, PNTLIGHTSHADOWS, LIGHTMAP, AMBIENTLIGHT(TYPE),
  AMBIENTOCCLUSION (32 combinations).
* **Pipeline estimate graph x static macros: 196** (union; PS_Default 116, VS_Default 32, VS_DefaultZPass 20, PS_DefaultZPass 11, leafs 10,
  particle 5, skydome 2) against the ~400 assumed in 6.1 risk 3. VS_DefaultZPass: 4 graphs but 19 static combinations (skinned / instanced /
  morphed split more than the graph).

### 7.4 Observe hooks (commit 43fe79e, `native.owned_observe`)

The five contexts' `RequestRenderOps` detoured (observe only); per-view visible lists verified against the op lists and the frame tap: 0 unmatched ops / requests in steady state (rvkcompare spots + Risen_Save_0005, Full); cost within noise.

### 7.5 Illumination + light-set tables (2026-11-06, `native.illum_table`)

`src/gpuscene/illumination_table.*` + `illum_compose.h` (pure compose, tests `illum_table_*`) + `shaders/include/rvk_illum.hlsli` (+ GPU twin
`shaders/test/illum_compose.comp.hlsl`, DXIL-checked, not yet run on the GPU). Records are RAW engine bytes: illumination 288 B (uint4 info + illum
[0,0x110)), light set 336 B (uint4 info + list [0,0x140) = set + 0x10); capacity 16384 illuminations / 8 sets. Hooks (shared with the census,
`engine/illum_census.cpp`): post `eCIlluminated_PS::UpdateIllumination` (dirty byte), post `SetLightSet`, post `eCLightSet::UpdateIllumination` +
`UpdateLightSets` (set records), pre `eCIlluminated_PS::Destroy`; `OnCacheOut` 0x595ce0 is not exported (freed by Destroy or 1800 idle frames).
Check: `StaticOps::writeLighting` composes c14..c37 from the records and compares: **DIFFERING 0** (1.7-1.9 M draws per run, Vulkan / D3D12 /
validation, rvkcompare spots with and without table lighting, Risen_Save_0005); GPU read-back every 240th frame 0 differing. Finding: hooks alone
were NOT enough (123,891 of 1.85 M draws differed): some illumination objects (0x118 stride, not eCIlluminated_PS + 0x78: batch copies) are
rewritten in place without a hook, so the objects drawn are also polled once per frame against their record (`poll` uploads, 40-140 per minute at
the spots, 0-1 at the savegame). Upload rate at the savegame ~1 record + 0.33 KiB per frame (set data dominates: the set changes nearly every frame).

### 7.6 MeshPart + Layout tables (2026-11-06, `native.mesh_parts`)

`src/gpuscene/mesh_layout.h` (pure: records, declaration -> slot words, D3D9 element decode; tests `mesh_layout_*`), `mesh_parts.{h,cpp}` (tables, check,
GPU fetch test), `shaders/include/rvk_mesh.hlsli` (`rvkMeshLoadPart / LoadLayout / Fetch / FetchPosition / FetchNormal / FetchUv / FetchIndex`, DXIL-checked) +
`shaders/test/mesh_fetch.comp.hlsl`. Debug setting `native.mesh_parts` (Bool, restart, default off; Full only because only StaticOps::issue feeds it; off = nothing
allocated, no observer registered). `GpuTable<T>` (illumination_table.cpp) is reused (its log prefix is now `native: gpu table:`); `d3d9::Buffer::arenaAddress()` is the
static GPU copy's address without the RT gate of `gpuAddress()`.

* **MeshPart record, 128 B** (uint4 x 8): info {live, layout id, generation, flags (in arena, index32, streams 0..2 bound)}, index {index buffer address (byte 0 of the
  buffer), first index, index count}, range {base vertex (int; 0 on the static path, kept so the shader adds it: D3D12 `SV_VertexID` ignores `vertexOffset`), min index,
  vertex count, index size}, 3 x stream {address of the stream's data = buffer byte 0 + stream offset, stride, bound}, material {material pointer, sub-mesh pointer}, owner
  {mesh resource pointer; local bounds: not available, the engine's sub-mesh carries none and a scan of the vertices is not "cheap"}. Addresses are `uint2` pairs (Vulkan
  device address; D3D12 heap slot + offset: `GpuAddr::ref.plus(offset).pair()`). Key = (sub-mesh pointer, first index, primitive count): each LOD node / merged z range is its
  own part; slot on first sight, after the draw (the buffers have their GPU copy then); freed by `DrawObserver::bufferChanged` (a buffer got a new GPU copy or was released)
  or after 1800 frames unseen (shadow-only parts reappear).
* **Layout record, 80 B**: info {live, content hash, elements, elements not expressible} + 16 semantic words (offset | (D3DDECLTYPE + 1) << 16 | stream << 24; 0 = absent):
  position, blend weight / indices, normal, tangent, binormal, colour 0 / 1, texcoord 0..7. Deduplicated by the element list (exact compare behind the hash), id = index.
  Measured in the game, 4 layouts in total: (a) FLOAT3 position, FLOAT3 normal, FLOAT3 tangent, D3DCOLOR colour 0 + 1, FLOAT2 uv0, FLOAT2 uv6 (the lightmap set); (b) the same
  + FLOAT2 uv1; (c) (a) with the instance index (TEXCOORD4 FLOAT1) on **stream 3** in place of uv6; (d) FLOAT4 position, FLOAT2 uv5, FLOAT4 blend weight 0 (+ blend weight 1 / 2
  FLOAT3: not expressible, counted). The part table holds streams 0..2 only: an element on stream 3 (instance data, per frame) fetches as 0. Types seen: FLOAT1/2/3/4 and
  D3DCOLOR; the other types are covered by the unit tests (hand-computed D3D9 values) and the shader's decode only.
* **Verification** (`rvk.log native: mesh parts (5 s)`): every composed static draw (z, solid, additive, shadow views' z; skipped: draws without an eCSubMesh = actors, trees,
  vegetation, billboards, particles; unindexed; CPU-generated geometry) is compared with its record (index range, base vertex, min index / vertex count, every stream's
  address / stride, layout id, material key) and, for a draw issued fresh, with the **device's own bindings** (`Device::vertexDecl / indexBuffer / streamSource`) read after the
  draw; a replayed (packet-cache) draw only against its own description. A record whose addresses were unresolvable at creation and resolve later counts `late address`, not a
  difference. Every 240th frame the tables are read back and compared with the CPU copy (`verify:`), and `mesh_fetch.comp` fetches the index at a random position of a random
  part's range plus position / normal / uv0 / colour 0 of a random vertex through `rvk_mesh.hlsli` for 256 cases, compared with `decodeElement` over the buffers' CPU bytes
  (`fetch test:`; tolerance 1e-5). Results (Full, 2026-11-06, all DIFFERING 0): Vulkan rvkcompare `harbour_docks,jungle,pub_interior` (463 parts peak, 4 layouts, 4.3 M
  draws checked, 17 table read-backs, 17 x 256 fetch cases); Vulkan savegame `Risen_Save_0005` (packet cache off: 4.7 M draws, all compared with the device bindings, 448 slots,
  4 layouts, 27 read-backs, 24 x 256 fetch cases, 1 late address); D3D12 spots (5.9 M draws, 24 x 256 fetch cases); `-Validation` spots `harbour_docks,jungle` (no VUIDs, 16 x 256 cases).
* **Outside the arena: none** in any run (0 ring-copied, 0 without an address, 0 without a buffer). Static meshes are static buffers; a part with a ring-copied buffer (dynamic /
  system-memory) or a null address (Vulkan **without the RT tier**: the arena only has `SHADER_DEVICE_ADDRESS` when ray tracing is on, so `arenaAddress()` is null) keeps its CPU
  description with the `in arena` flag clear and is counted by reason in that line. Making the Vulkan arena addressable without RT is a follow-up for the owned frame (usage bits at
  device creation).
* Cost (savegame, packet cache off, `-Perf`; the check does a mutex + 4 buffer-address lookups per draw): CPU ~6.8 ms vs ~6.7 ms per frame, interval p50 7.5 vs 7.2 ms: about
  +0.1 - 0.3 ms, a verification cost (a shipping path reads the tables only on the GPU). Upload ~0.1 record / 0.01 KiB per frame in steady state, ~0.3 records at cache-in.

### 7.7 Material table (2026-11-06, `native.material_table`)

`src/gpuscene/material_table.*` + `material_compose.h` (pure: record build from raw engine bytes + `diffMaterial`, tests `material_table_*`) +
`material_graph.h` (graph normalisation / ids, parity vectors shared with `tools/tests/test_material_graph_census.py`) + `shaders/include/rvk_material_table.hlsli`
(+ `shaders/test/material_fetch.comp.hlsl`, DXIL-checked, not run on the GPU). Debug setting (Bool, restart, default off, Full): off = nothing allocated.

* Record (1104 B, key = the eCShaderDefault whose pools / slots the composition reads): uint4 info (live, version, pack word reserved 0, flags: opacity /
  refracted / trade-alpha / z-fill / depth bias / double-sided from `XFX_DOUBLESIDED` / has switch / slots overflow), counts (vsFirst, vsCount, psFirst, psCount),
  state (blend mode +0x50, mask ref +0x58, base stage | slots), graph (PS_Default id, PS_DefaultZPass id), switch masks; then VS pool 32 x float4 (c232..),
  PS pool 16 x float4 (c6..c13), 8 sampler slots x 32 B (bindless slot of the image as `materialSamplers` resolves it: image data -> stage placeholder -> error
  image; the 6 raw sampler-state words, 0xABABABAB = the gfx admin's default filter, a draw parameter; sRGB int8). Capacity 4096 (4.3 MiB).
  The pack word is reserved: the pack keys by draw tag at issue time, not by material.
* Textures: the composition gets the bindless index from `textureOfImageData` -> HAL texture -> `Texture::bindlessSlot()` (assigned at creation, `heapSampled`);
  the record stores it, every draw compares its composed binding with it (`textures DIFFERING`, plus sampler states).
* Updates: slot at first sight in `passDraw` / `specialPassDrawImpl`; every drawn record is re-read ONCE per frame at its first draw (poll; no engine hook:
  the per-frame pool writers are SetTime + switches, and the illumination table showed hooks alone miss rewrites) and uploaded when the bytes changed.
  **Native pool writes are not in the engine's memory**: SetSwitch elements (`applySwitch`), the sky's SetColor / SetImage, decal / FX sampler images write the
  composition's mirror pools only (engine slot code is skipped). Those registers / slots are noted (`noteSwitchWrite`, sticky union in the record's switch
  masks) and excluded from the comparison (counted `switch-skipped`); F1 needs the switch tables / sky colours as a table input. Without the sky's notes the
  check showed 2-4 k DIFFERING draws per run (PS c6 / stage 9 of the sky material), i.e. the notes are necessary, not cosmetic.
* Verification: `diffMaterial(record, draw)` over every static draw N5 composes (main z / solid, shadow z, overlays): VS pool registers >= c232 that are live,
  PS c6..c13 that are live, texture slot + 7 sampler words per sampled material stage, blend mode and mask reference (not when an override is active). Read-back of
  the whole table every 240th frame. Log: `native: material table (5 s)`, `native: material graphs` (every 30 s).
* Results (rvk.log in `B:\DevDrive\rvk-work\scratch\material-table\`): Full, Vulkan, rvkcompare `harbour_docks,jungle,pub_interior`: 4.55 M draws checked, 4.60 M
  registers, 8.66 M texture bindings + sampler states, 2.77 M blend / mask, 21 k switch-skipped registers: **registers DIFFERING 0, textures DIFFERING 0**,
  read-back 23 x 0 differing. Savegame Risen_Save_0005 (lod 2x, packet cache off): 5.0 M draws, 0 / 0. D3D12: 6.8 M draws, 0 / 0. `-Validation`: 4.35 M, 0 / 0,
  no validation message. (The first three configurations ran before the last graph-bookkeeping change, which does not touch the compared data; the Vulkan
  spots run was repeated with the final build.) Records 105-167 live (peak 172) at the spots, 87-161 at the savegame; 0 unresolved textures, 0 overflow.
* Upload rates: new 174 over a run; `poll` 27.5 k per 70 s (~7 records / frame, 7 KiB / frame): ~10 materials change per frame, all time-driven: the water
  materials (`Special_Water_*`: VS c232 / c233, ~2 k uploads each per run), lava, a few more (`texture`: 18 per run = streaming swaps; `graph`: 315 at first sight).
  Steady state without those: 0.
* Graph ids (id = hash(file, entry, normalised graph text), as the census): 103 distinct ids seen in a spots run (80 PS_Default on 134 live records + z
  passes), 63 at the savegame, 111 pipelines (graph x static macros, census estimate 196 over town + jungle); 56-80 distinct graphs per frame (avg ~70).
  Materials sharing a graph (spots, color pass): 11, 8, 6, 4, 3, 2, 2, 2, 2, 2 (top 10: a long tail: 134 records on 80 graphs, no dominant one; the biggest
  is diffuse + normal map). A material's z / special passes name other graph variants than the one the record keeps (the first seen): 14 k per run counted
  (`other graph variants seen`), not uploaded.
* Cost (checkDraw time only, debug setting, Full): 0.03-0.22 ms per frame (draw count dependent: 60-80 k checked draws per second-run frame x ~3 us incl. the
  mutex, the hash lookup, the compare), flush negligible. Not a shipping cost: the table's draw check is the F0 gate; F1 reads the records on the GPU.

### 7.8 F1a owned windows (2026-11-06, `native.owned_windows`)

Debug setting (Bool, runtime, default off, Full, `needs` native.material_table; Wrapper / off = the old path untouched). Files: `src/d3d9/owned_windows.h`
(`OwnedWindows` / `OwnedStats`), `src/gpuscene/window_compose.h` (pure address math, tests `window_*`), `window_tables.*` (view blocks + the 5 s log),
`MaterialTable::fillWindows`, `StaticOps::setOwnedFor`, `Device::setOwnedWindows` + `splitStage` / `verifyOwned` (`d3d9/draw.cpp`).

* **Mechanism.** `StaticOps::issue()` builds an `OwnedWindows` per issued draw right after `setRegs(d)` (also before `replayDraw`: the packet cache's
  replay re-runs `uploadConstants`) and hands it to the device (`setOwnedWindows`; cleared after the op's draws). `Device::splitStage` (the one place the
  four windows per stage are made) serves a window from the override when the image covers the shader's extent (`covered >= need`), else the normal
  ring block: nothing is uploaded and `constWin_` / its dirty bit stay as they are. Windows served: **VS 3 + PS 2** from the Material record
  (`MaterialRecord.vs` / `.ps` at record offsets 80 / 592, base = record address + regs offset - first register x 16), **VS 0 + PS 0** from a per-view
  block (`WindowTables`, `ViewRecord` = VS c0..c11 + PS c0..c4, 272 B; a draw reuses one of the last 8 blocks of the frame when its 17 registers are
  equal, else writes a new slot; slots restart every present and re-upload only when their bytes differ), **PS 3** (c14..c37) from F1b's lighting image
  entry (`LightingImages::windowBase`, only with `native.lighting_images`). VS 1 / VS 2 (object rows, instances) and PS 1 (c5) stay per draw (F1a scope).
  Not served (counted): a stage of a material whose record has switch / native pool writes (`sw` masks, per stage), draws without a record (sky,
  decals, FX, non-default shaders), images shorter than the shader's extent (`image too short`: 21.6 k of 1.05 M candidate windows at the savegame: a
  pool shorter than the shader's c232.. read).
* **Address arithmetic.** Vulkan: a GpuRef is the device address, `plus(delta)` is 64-bit. D3D12: {heap slot, byte offset}, the offset word wraps
  modulo 2^32 and the shaders' 32-bit offset arithmetic undoes it (tests `gpu_ref_*` + `window_register_addresses_*`), but the tables now start with
  `kTablePad` = 4 KiB unused bytes (`GpuTable::create(..., padBytes)`: the buffer is that much larger, `ref()` points past it, uploads / read-back
  offset by it) so the biased base never goes below 0: record 0 of the Material table has base offset 4096 + 80 - 232 x 16 = 464. The view table needs no
  bias (window 0 starts at c0).
* **Ordering (item 4).** Both tables are flushed at the present into the frame's UPLOAD list, which the backend runs before ALL of that frame's
  draws (barrier all-commands -> transfer at its start), so a draw reads the CPU shadow as it is at the present. A Material record only changes at its
  first draw of a frame (the poll in `checkDraw`) and a view slot is written once per frame, so that value is the one every draw of the frame was
  composed with; the draw check of F0 (`diffMaterial`, 0 differing) and `split_verify` below confirm the CPU side. A record that does not exist on the GPU
  yet (buffer created at the first present) is not offered. What this does NOT cover: a record changed by a later poll in the same frame (none
  exists); the water materials' VS c232 / c233 change every frame and are re-uploaded every frame, they are served like the others.
* **Verification.** `debug.split_verify`: for every served window `verifyOwned` compares the image's CPU twin with the register file over the registers
  the draw composed (`vsSet` / `psSet`; views: all), plus a diagnostic for registers inside the shader's extent that the draw did NOT compose and whose
  image differs (the old path read stale register values there): **0** in every run. The lighting window has no CPU twin (F1b's GPU read-back compares the
  image with the drawn registers, c14.w and unwritten registers excluded; DIFFERING 0). Log `native: owned windows (5 s)`: draws, windows from tables
  (material VS / PS, view VS / PS, lighting), uploaded / reused (the draws' other windows), fallbacks, view block stats, verified / MISMATCHES.
  Developer bisect: `-Environment 'RVK_OWNED_MASK=<bits>'` (1 VS material, 2 PS material, 4 VS view, 8 PS view, 16 lighting; default all).
* **Results** (rvk.log in `B:\DevDrive\rvk-work\scratch\owned-windows\`): Full, Vulkan, `harbour_docks,jungle,pub_interior`, material table + owned
  windows + split_verify: 1.81 M draws, windows from tables: material VS 324 k / PS 723 k, view VS 1.81 M / PS 1.04 M (= 3.9 M windows, all compared:
  **MISMATCHES 0**, 0 uncomposed differing), uploaded 2.49 M / reused 1.02 M (the other windows), fallback: switch materials 5.6 k, no record 2.8 k.
  With `native.illum_table` + `native.lighting_images` (pub_interior, harbour_docks, 2 runs): 0.47 M draws each, lighting served 236 k / 238 k, 99.5 %
  of the candidate windows from tables (the rest: 3 k switch materials, 3 k no record), MISMATCHES 0, view table read-backs 0 differing, lighting images
  read-backs DIFFERING 0. D3D12 (pub_interior, lighting on): 0.94 M draws, 2.65 M of 2.66 M candidate windows served, 0 / 0. `-Validation` (Vulkan,
  lighting on): 0.24 M draws, 0 mismatches, no validation message. View blocks: 6-9 per frame, 0.1 - 3 blocks uploaded per frame (a few 100 B).
* **Picture A/B.** Not bit-comparable between runs here: two runs with the SAME settings differ by more than owned on vs off. rvkcompare spots, separate
  runs (the first run after a deploy and the odd run of a batch are outliers): `pub_interior` off vs off 0.83 / 0.75 mean abs, off vs on 0.91 / 0.94 /
  0.40 (on vs on 0.40 - 0.61); `jungle` off vs off 2.4 / 1.8, off vs on 2.2 - 2.4; `harbour_docks` (animated shore, caustics, storm sea phase) off vs off
  2.5 - 5.5, on vs on 0.14 - 0.39, off vs on 0.7. All differences sit in the water-related materials / foliage (heat maps), none at the lit or textured
  static meshes. **The in-run toggle (`-Commands 'N: set native.owned_windows ...'`) is not usable as a control**: with `RVK_OWNED_MASK=0` (nothing
  served) toggling the setting changed the harbour picture by 20 mean abs (a different camera / sea state), so the toggle itself perturbs the scenario.
  Honest status: no systematic picture difference found, bit-exactness not proven by pixels; the proof is the per-draw register compare (3.9 M windows, 0).
* **CPU.** Savegame Risen_Save_0005 (Full, player defaults, lod 2x, vsync off, `-Perf`, 4 runs off / on / off / on, engine + HAL CPU per frame): off 6.30 /
  6.62 ms, on 6.92 / 6.33 ms: no measurable change (noise +-0.3 ms), as expected (the register setters stay; only ~1.0 M window blocks per 5 s are not
  uploaded, 0.1 KiB-scale each). The value of F1a is the proven register images; the CPU gain needs F2 (nothing composed per draw).
* **Counts.** Savegame (last 5 s log line of an owned run, lighting images off there): 531 k draws, view blocks 535 k lookups / 531 k reused / 3.7 k new,
  material windows 72 k + 146 k, `image too short` 21.6 k, 2.5 k switch-material and 0.9 k no-record windows.

### 7.9 F1b lighting register images (2026-11-06, `native.lighting_images`)

`src/gpuscene/lighting_images.{h,cpp}` + `shaders/gpuscene/lighting_compose.comp.hlsl` (the first GPU run of `rvkIllumCompose`). Debug setting (Bool, restart,
default off, Full, `needs` `native.illum_table`; off = nothing allocated).

* **Entry layout.** One persistent device buffer, `kLightingPad` = 4096 B pad (no entry) + 8192 entries x `kLightingStride` = 384 B (24 float4 = c14 .. c37,
  entry register `r` at `+ (r - 14) x 16`), 3 MiB. Window base of entry `e` = `entryRef(e) - 14 x 16` (`LightingImages::windowBase`, `GpuRef::plus` with a negative
  delta): on D3D12 the (heap slot, byte offset) form needs offset >= 224, the pad guarantees it for entry 0; entries are 16-byte aligned. The compose pass writes
  through the buffer's `shaderRefRw` form, F1a must read through `windowBase` (the read form: `shaderRef`; they differ on D3D12).
* **Parameter set** (everything `composeLighting` takes besides the two records): illumination slot (the F0 table's), `tableLit`, `holdsAll`, `n` (lights the
  pass takes: 3 when table-lit, else `passLights(flags)` 0..3), `firstLight`. Key `lightingKey(slot, p)`; a GPU job record (16 B: slot or 0xffffffff = free,
  flags, n, first) per entry in `GpuTable<LightingJob>`. Entries are created on first sight in `StaticOps::writeLighting` (`IllumTable::checkDraw` now returns the
  illumination slot), freed after 600 idle frames (job -> free, the pass skips it). Spots: 460-530 entries peak, 55-60 live; savegame 248 live (peak 665); 0 overflow.
* **Ordering decision.** `LightingImages::flush` runs at present right after `illumtable().flush`, recording the compose pass into the UPLOAD command list (the
  tables' own copies are in the same list: one memory barrier between). The upload list executes before ALL draws of the frame it is recorded for (backend:
  `uploadCommands()`), and the present flush uploads the tables' END-OF-CPU-FRAME state. So the "illumination flush at present = previous frame" caveat does not apply:
  records and images are this frame's, an entry first seen this frame is composed in this frame, no extra flush point before the main view is needed. A draw
  composed before a record's last in-frame change would differ from the image; the check below counts that and found none.
* **Pass.** One thread per entry slot, compose = `rvkIllumCompose` (all 24 registers, unwritten ones 0). Bit-exactness needed one change in the shared
  `rvk_illum.hlsli`: `precise` on the SH accumulation (without it the driver fused `g x indoor + v.w` into an FMA: c15..c17.w off by 1 ulp in ~30 % of entries).
* **Verification** (every 60th frame, `kVerifyEvery`): the images are copied back; each live entry is compared (a) with `composeLighting` on the CPU from the
  illumination table's shadow records at that present, (b) with the registers `writeLighting` last wrote for it in that CPU frame (entries not drawn that frame
  are skipped for (b)). Left out of both: registers the composition does not write (c14.w, the lights beyond the pass's count: found by composing twice on different
  priors): the engine's register file keeps STALE values there (the image has 0); the pixel-shader permutation of n lights never reads them, but a window
  block compared whole against the CPU register file (`split_verify`, F1a) will see them differ. Two denormal-only differences (a static SH word the engine
  holds as 7.4e-44, the GPU's float ALU flushes denormals) are counted as `denormal-only words`, not as differences (a shader reads 0 either way).
  rvk.log `native: lighting images (5 s)`: entries / new / freed, passes, read-back checks (entries, not comparable = record freed or its light set gone (idle entries),
  `of them drawn this frame` must be 0), `DIFFERING N (vs CPU compose, max ulp) / M (vs drawn registers, compared, max ulp)`.
* **Results** (rvk.log in the scratch dir `rvk-work/scratch/f1b/`, Full): Vulkan rvkcompare `harbour_docks,pub_interior,monastery_hall`: 80 read-backs, 21.6 k entries,
  12.3 k compared with drawn registers, 1.95 M draws noted: **0 (vs CPU) / 0 (vs drawn), max 0 ulp**; Risen_Save_0005 (lod 2x, packet cache off): 64 read-backs, 16.1 k
  entries, 11.3 k vs drawn: 0 / 0; D3D12 spots 100 read-backs 0 / 0 (17.2 k vs drawn), D3D12 savegame 76 read-backs 0 / 0 (16.6 k vs drawn); `-Validation` savegame:
  68 read-backs 0 / 0, no validation message. Illumination table's own check stays DIFFERING 0 in all runs. Job table read-backs 0 differing.
* **GPU cost**: the whole upload stream per frame at the savegame (248 entries, `-Perf`, Vulkan): 0.003 ms without -> 0.014 ms with the table + images (the pass is
  ~0.011 ms; at the spots ~55 entries per pass); main stream unchanged. CPU: one hash lookup + a 384 B copy per lit draw (debug setting, not a shipping cost).
* Next (F1a, other agent): point PS window 3 at `windowBase(entry)` for the static ops; the entry index of a draw is what `LightingImages::note` returns.

### 7.10 F2a scene variant (2026-11-06, `TranslateOptions::sceneRecord`, HLSL `RVK_SCENE_RECORD`)

The shader side of section 8's F2: a scene variant of a translated pair / the ubershader reads everything of the draw's push block from a **DrawRecord**.
Nothing in `src/d3d9` / `src/native` uses it yet (F2b); the default (non-scene) generated text is byte-identical (checked on three corpus shaders against the
main build; `kTranslatorVersion` NOT bumped).

* **ABI** (`src/shader/abi.h`, twin `shaders/include/rvk_draw.hlsli` under `RVK_SCENE_RECORD`, offsets checked by `rvk_tests abi_hlsl_scene_layout_matches_cpp`):
  `DrawRecord` (128 B, uint4 rows) = `vsWin[4]` @0, `psWin[4]` @32 (address pairs as in `PushConstants`), `draw` (DrawBlock) @64, `prevVs` @72,
  `drawId` @80, `flags` @84 (kind / pass / view / D3D12 UBYTE4 bits), `instanceSlot` @88, `part` @92, `materialSlot` @96, `illumEntry` @100, 24 B reserved.
  Push block `ScenePush` (same 112 B): `records` @0 (the frame's record buffer), `frame` @24 (the offset of `PushConstants::frame`: hooks'
  `rvkLoadFrame(pc.frame)` is unchanged), `recordBase` @32 (added to `ref.x`), the rest spare. `pc.vs / ps / draw / flags / drawId / prevVs` and the windows
  do not exist in a scene shader: `rvkVsWin / rvkPsWin / rvkLoadDraw` and the new `rvkDrawFlags() / rvkDrawId() / rvkDrawPrevVs() / rvkDrawAddr()` read the
  record (in plain shaders the same accessors read the push block; `rvk_material_variant.hlsli` and the RT capture hook use them instead of `pc.*`).
* **Locations.** Vertex input `uint2 ref` = location **24**, semantic `ATTR24` (`abi::kSceneRefLocation`): D3D9 declarations stop at v15 and the ubershader's compact
  locations at 21, so no game declaration or existing stream uses 24 (instanced draws use the same locations for their instance-rate elements; the ref stream is one
  more instance-rate binding). Cost: needs `maxVertexInputAttributes >= 25` (every desktop driver reports >= 29; D3D12 allows 32). ref.x = record index, ref.y free.
  Varying `nointerpolation uint` at **VARY14** = TEXCOORD14 (`abi::kSceneVaryingLocation`): no corpus shader reads or writes it (the translator refuses
  such a shader and any shader reading i# / b# constants, which need the full stage block; none in the corpus); the ubershader's D3D12 pad14 gives way to it; the
  RT / GI variants' extra varying stays at 15. D3D12's dense signatures pad around it as for any location.
* **Generated text, vertex shader** (`show <file> --scene`): `#define RVK_SCENE_RECORD 1` before the include, `[[vk::location(24)]] uint2 rvk_sceneRef : ATTR24;` in
  `RvkVsIn`, `[[vk::location(14)]] nointerpolation uint rvk_sceneIdx : VARY14;` in `RvkVsOut`, `rvkSceneBeginVs(rvk_in.rvk_sceneRef);` before `rvk_draw = rvkLoadDraw();`
  and `rvk_out.rvk_sceneIdx = rvk_sceneIndex;`. Pixel shader: the define, the varying in `RvkPsIn`, `rvkSceneBeginPs(rvk_in.rvk_sceneIdx);` first. Ubershader:
  `uberPixelHlsl(..., sceneRecord)` / `-D RVK_SCENE_RECORD=1` for `ge_default.vert / .frag`.
* **GPU test** (`tools/rhitest/parity_scene.inl`, both backends, also `--validation`): 4 draws of a mesh with distinct register files and textures, blended
  additively, drawn (a) with per-draw push blocks, (b) with the scene variants by ONE `drawIndexedIndirect` (firstInstance = i, `vertexOffset` 0) over the DrawRecords: the
  images are byte-equal (0 pixels differ) on Vulkan and D3D12 and the scene image agrees across backends (worst difference 0 - 1). Cases: the asm pair of the
  translated-draw test, the ubershader (simple, heavy with normal map / specular / lightmap / lights, GI hooks reading window 3), two real corpus pairs (a rigid mesh + a
  12-texture lit material, a skinned mesh + the same material; `RVK_CORPUS` or a `corpus/` found above the source). A mutation (a record pointing at the next draw's DrawBlock)
  fails every case. `RVK_PARITY_ONLY='scene variant'` runs only these.
* **Checks:** `rvk_shadertool corpus corpus --scene` (2483 / 2483), `dxil-check corpus corpus --scene [--fillers | --gbuf-motion | --gi | --capture]` (0 failures; push block 112 B),
  `dxil-check embedded` (74, `material scene` = `ge_default.vert / .frag` with the define), `dxil-check uber` gained the variants `plain+scene`, `gbuffer+motion+scene`.
  Note: `rvk_rhitest` "references (GI lookup, RT records)" fails on D3D12 in this environment with and without this change (also in the main build, 10 of 2048 values; unrelated).

### 7.11 F2b-1 indirect scene draws (2026-11-06, `native.scene_draws`)

Debug setting (Bool, runtime, default off, Full only: `StaticOps` issues the draws; Wrapper / off = nothing changes) + `native.scene_draws_check` (Int, every N-th frame
a GPU read-back check). Files: `src/d3d9/scene_draws.cpp` (decision, records, ref stream, runs, check, 5 s log), `draw.cpp` (`prepareDraw` / `submitPacket` /
`replayDraw` / `flushBatch` hooks), `rhi::DrawPacket::Op::DrawIndexedIndirect` (Vulkan `vkCmdDrawIndexedIndirect`, D3D12 `ExecuteIndirect` with a signature made at
device creation), `GameCaps::maxVertexInputAttributes`, `VariantSlot::SceneVs / ScenePs / SceneGi / SceneGbuffer(M|A)`, test `rvk_d3d9test` "scene draws (indirect)".

* **What changes.** The CPU still composes every draw (registers, windows, DrawBlock, push block) exactly as before. For a draw of the main view's z pre-pass / solid
  pass issued by the static ops (kind StaticMesh, batchable, indexed) `prepareDraw` also resolves its scene form: the scene VS (`SceneVs`) + the scene form of the pixel
  shader it takes (plain `ScenePs`, GI `SceneGi`, G-buffer `SceneGbuffer*`; a scene slot = the base slot's provider options + `sceneRecord`, built in the background like
  every variant) and the pipeline with the ref stream added (binding after the draw's own, instance rate, stride 8, attribute location 24 `R32G32_UINT`; baked into the
  pipeline description where the vertex layout is not dynamic). `submitPacket` / `replayDraw` then turn the finished packet into a scene packet: its push block (8 window
  bases, DrawBlock, `prevVs`, drawId, flags) becomes a 128 B `DrawRecord`, the packet waits in the batch. `flushBatch` groups the batch by pipeline as before and cuts it into
  runs of scene packets that agree in pipeline, vertex input (buffers; `vertexShift` is zeroed, the per-command `vertexOffset` carries it), dynamic state, index binding and
  frame block: **one `DrawIndexedIndirect` per run** over a ring block of its records and one of its 20 B argument records (`firstInstance` = the record's index in the block =
  `ref.x`; the ref stream is a per-frame ring block of `(i, 0)`, 1024 entries). The packet cache keeps the scene pipeline in `CachedDraw` (a draw whose scene variants were not
  ready is retried: `variant decision` refusal) and re-makes the scene packet at replay.
* **Not scene draws (counted as fallbacks in `native: scene draws (5 s)`):** not batchable / no scope, capture / motion / tint / ubershader forms (`variant kind`), scene
  variant failed or pending, pipeline pending, no room in the vertex input (16 attributes / 8 bindings), ring full, null address (guard below). Capability:
  `maxVertexInputAttributes >= 25`, `drawIndirectFirstInstance`, `multiDrawIndirect` (else logged once and off).
* **The crash of the first version (two driver timeouts).** `Device::packet_` is reused by every draw; `toScenePacket` set `packet_.scene` and `prepareDraw` did not clear
  it, so a later normal draw (fallback, another kind, same batch) inherited a stale record index and was recorded as an indirect call with a normal pipeline and somebody
  else's records: a null read, `VK_ERROR_DEVICE_LOST`. Fixed (`packet_.scene = 0` at the start of `prepareDraw` and after the submit) and guarded: a scene packet is refused
  (normal path, counter `null address / bad record`) when the DrawBlock, the frame block, any of the 8 window bases, the ref stream or the vertex input is 0; `sceneFlushRun`
  drops a run whose packet has no valid record. Test `rvk_d3d9test` "scene draws (indirect)" (both backends, `--validation`): eight indexed quads as static-mesh draws of the
  main view's solid pass, drawn normally (reference) and as scene draws (exactly one indirect call over eight records, **0 bytes differ**), with a normal draw inside the
  batch (7 scene draws, 0 differ: the stale-packet case), with the guard forced (all eight refused, 0 differ) and again after it. Vulkan and D3D12 (debug layer): 0 failures,
  0 validation errors. The first game run after the fix used GPU-assisted validation (`VK_KHRONOS_VALIDATION_VALIDATE_GPU_BASED=GPU_BASED_GPU_ASSISTED`): no fault, no VUID
  (the environment variable reached the process; the layer does not log whether it applied it).
* **Check mode** (`native.scene_draws_check=10`): the GPU copies the frame's record and argument blocks (upload list, ring buffers as copy sources) into a read-back buffer,
  compared word by word with what the CPU wrote (padding excluded): Vulkan 3 spots with `-Validation` 243 read-backs / 4.9 MiB, D3D12 55 read-backs, **DIFFERING 0**. (The first
  version compared the padding too and reported differences: a check bug.)
* **Batches (savegame Risen_Save_0005, lod 2x, Full, material table + owned windows):** 99.8 k static main-view draws per 5 s window, **all** drawn as scene draws (99.1 k of
  them replayed from the packet cache), 16.7 k indirect calls: **mean batch 6.0, max 100** (spots 2.4 - 7.4, max 73 - 97). Runs end almost only because the next packet has
  another pipeline (16.7 k pipeline groups); 6.9 k of the runs are cut inside one pipeline by another vertex input (another vertex buffer), dynamic state / index binding / frame
  block / run size cuts are ~0. The run length is bounded by the number of draws that share a pipeline in one flush (a flush every 1024 packets; 160 without scene draws).
  Many materials = many pipelines; larger runs need the ubershader's merged permutations (F2c).
* **Verification.** `debug.split_verify` with owned windows + scene draws: MISMATCHES 0 (Vulkan 3 spots with `-Validation`: 63.7 k windows; D3D12). D3D12 `-Validation` (debug
  layer, pub_interior): no device removal, only the old DSV-format warning of pipelines without depth. `rvk_tests` 279 / 279.
* **Picture A/B (same run, toggled at run time with `-Commands 'N: set native.scene_draws true|false'`, a shot every 50 presents).** A spot's hold does not start at the same present
  number in two runs (loading time), so the pairs are taken after the run from consecutive shots inside the hold; same-state pairs are the control. pub_interior (hold ~1250 - 1700):
  control on/on 0.63, 0.41, 1.20, 0.40, off/off 0.46, 0.52, 0.33 mean abs (max 12 - 18), toggle pairs 0.61 and 0.43 (max 12 / 19); harbour_docks: control 1.68 - 2.03, toggles 1.75 /
  2.13 (max 88 - 93); jungle: control 1.61 - 1.90, toggles 1.70 / 1.78 (max 91 - 102): **the toggle pairs lie inside the control pairs' range** (eye adaptation, sea, rain / foliage
  animation are the noise). Bit-exactness is not proven by game pixels; the per-draw proofs are the window verification and the check mode, and the d3d9test scenes are bit-exact.
* **The setting-toggle "perturbation" of the F1a finding: not reproduced.** Toggling `native.scene_draws` (through `applyStaticOps`; `StaticOps::setMode` returns early when nothing
  changes, the packet cache entries are remade through the `variant decision` refusal) does not change the picture beyond the control noise. The earlier "different camera / sea
  state" came from comparing shots at fixed present numbers: the hold starts at different presents in different runs and shots outside it show the spot being placed or the start
  prompt. Use a long `freeze_wait`, many shots, and choose the pairs inside the hold afterwards (`rvk-work/scratch/f2b1/ab.py`).
* **CPU** (savegame, `-Perf`, `RisenTest.exe`, vsync off, interleaved off / on / off / on, shared default copy under `with-test-lock -Who f2b1-perf`): engine + HAL CPU per frame
  off 6.20 / 6.13 ms, **on 6.32 / 6.34 ms (+0.15 ms)**; GPU 3.33 / 3.32 vs 3.32 / 3.36 ms; the "draw calls" part 0.54 ms in both. No CPU gain: the composition stays, the
  indirect path adds the record / argument writes and the merge scan, and the calls it removes (pipeline bind + bindings + push + draw per run of ~6 draws) were already cheap on the
  record threads. The value of F2b-1 is the DrawRecord / indirect path proven on both backends; the CPU gain needs F2c (nothing composed per draw).

### 7.12 F2c-0 measurements (2026-11-06, `RVK_F2C_MEASURE=1`; no behaviour change)

Counters only (`src/native/f2c_measure.{h,cpp}`; hooks in `static_ops.cpp` `f2cClassify` / `opEnterImpl` / `finishOp` / `issue` / `poolsOf` / `bindLightmaps` /
`materialSamplers` / `writeLightingRegisters` / `engineDraw`, and the observer / replacer loops of `d3d9/draw.cpp` `prepareDraw` + `replayDraw`). The environment variable is
read once at the first view (`GetEnvironmentVariableA`; `run.ps1 -Environment 'RVK_F2C_MEASURE=1'`); off = one bool test per site. rvk.log `native: f2c measure (5 s): ...` (one line
per 5 s: ops / draws by list and reason, invalidation changed/pairs, observers, replacers), `(run)` every 30 s (+ `CTAB` lines, + the first hierarchy / instanced meshes named once).
Scope: the main view's mesh z + solid ops of the Static group (the ones `StaticOps` composes). Reasons are bits per op; "excl" = the first reason in the priority order of
`f2c::Reason` (instanced, instance list, overlay flags, fade, switch, no list, not table-lit, hierarchy, node op, shader class, video, sort id, draw-level blockers, no retain),
"any" = every bit. Draws of an op = the draws we composed (ops we left to the engine would count the engine's; none occurred: **0 ops not composed in every run**).
Runs (Full, Vulkan, `RisenTest.exe`, own copy `risen-test-vk-f2c0`, logs `rvk-work/scratch/f2c0/{F,G}-*.log`, summed over the 5 s windows with a world frame count,
loading windows left out; `summ.py` / `cumul.py` there): "PD" = `-PlayerDefaults` (rvk.log `native: table lighting on`), "lighting off" = the test baseline
(`lighting.clustered` / `full_lit` pinned off), lod 2x where a savegame ran.

**1. Owned-eligible share** (section 9's first cut: mesh ctx, static, not instanced, fade 1, no overlay flags, no switch material, table-lit, no hierarchy; the cut = "elig").

| run | main views | z ops (/view) | z draws | solid ops (/view) | solid draws | z elig draws | solid elig draws | all elig | + hierarchy | + instance list | + hw batches |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 3 spots (PD; time frozen) | 4573 | 1.15 M (252) | 1.19 M | 646 k (141) | 846 k | 82.8 % | 21.2 % | **57.2 %** | 77.1 % | 83.4 % | 99.9 % |
| savegame hold (PD, lod 2x) | 4935 | 2.24 M (453) | 2.38 M | 756 k (153) | 1.05 M | 93.0 % | 23.4 % | **71.7 %** | 81.4 % | 85.9 % | 100 % |
| savegame walk 60 s (PD) | 7457 | 3.34 M (447) | 3.55 M | 1.14 M (153) | 1.57 M | 92.7 % | 23.1 % | **71.4 %** | 81.4 % | 85.4 % | 100 % |
| new game, beach circle (PD) | 9774 | 1.59 M (163) | 1.63 M | 877 k (90) | 1.14 M | 79.8 % | 13.9 % | **52.7 %** | 76.0 % | 82.9 % | 99.7 % |
| lightturn, monastery hall (PD) | 2044 | 364 k (178) | 385 k | 176 k (86) | 234 k | 85.2 % | 18.7 % | **60.1 %** | 76.0 % | 83.7 % | 98.8 % |
| 3 spots, lighting off | 4661 | 611 k | 630 k | 458 k | 673 k | 68.7 % | 0 % | 33.2 % | 48.2 % | 56.5 % | 77.9 % |
| savegame hold, lighting off | 6041 | 2.71 M | 2.88 M | 929 k | 3.07 M | 92.7 % | 0 % | 44.9 % | 48.4 % | 51.5 % | 91.4 % |

(The three columns on the right are cumulative shares of ALL main-view static z + solid draws when the cut is widened by that class.) Exclusion reasons, share of the list's draws
(PD runs: spots / savegame / walk / beach / lightturn):

| reason | z draws | solid draws |
|---|---|---|
| **hierarchy** (ctx +0x34 > 0: Levelmesh_Landscape_01 [25000], Harbour_01_L02 [15000], Monastery_01_L03 [25000]; node ops, desc node != 0, are a subset: 2 - 6 % of z, 5 - 8 % of solid draws at the spots / savegame) | 17.0 / 7.0 / 7.3 / 19.8 / 12.8 % | 23.9 / 15.8 / 16.4 / 28.3 / 21.1 % |
| **instanced (hw batch)**: a render-instance op drawn by hardware instancing (res +0xa0, >= 2 instances, no lightmaps; avg 5 - 10 instances per op) | - | 39.6 / 46.2 / 47.6 / 40.9 / 40.0 % |
| **instance list (per instance)**: node 0xffffff but drawn one instance at a time; **always exactly 1 instance per op** (props: Obj_Dec_Barrel, Item_*, It_Plant, bones, fences ...) | - | 15.2 / 14.5 / 12.9 / 16.9 / 20.2 % |
| switch material | 0.2 / 0.03 / 0.01 / 0.3 / 2.0 % | <= 0.1 % |
| overlay flags | 0.1 % in the beach run only (the focus highlight) | 0 |
| no light list (solid) | - | 0.00 % (14 ops in a run) |
| no retain (on-demand compile of a pass at the slot's place) | 0.003 % | 0.003 % |
| not table-lit | - | 100 % of the non-instanced ones with lighting off (the pass count then follows the dynamic light count: also 3x more solid draws, 3.07 M vs 1.05 M at the savegame) |
| fade < 1, shader class, video image, +0x2b8 (sort id / transformation) non-zero, draw inherits a stage (texture not ready), CPU vertex stream / non-indexed | **0** in every run | **0** |

Notes: the "instanced" op kind of section 9 is two things (the first measurement flagged both alike): `composeSolid` takes the hardware-batch path only with `res +0xa0`, n >= 2 and
no lightmaps; the other node-0xffffff ops are plain per-instance draws whose world matrix / illumination / switch / flags come from the resource's own render-instance list
(0x58-byte records: matrix, switch +0x40, illum +0x48, flags +0x50), **no entity** (so the scene-table / object-table slot of section 9 does not exist for them). Informational bits
(never exclusions): masked materials (blend mode 1) 21 - 26 % of ops, UseDepthBias materials **0**, lightmaps bound by 55 % of solid ops, TradeAlphaAsSolid 42 - 58 % of ops
(the flag is not rare). z draws are 58 - 70 % of the static main-view draws, so the z pass dominates what an owned cut saves.

**2. Observers / replacers on static main-view z + solid draws** (counts per path; `typeid` names; PD runs, the totals of the last 5 s window of the savegame hold):

| device | calls | what it reads from the draw (code reading; `enh/rt_scene.cpp observe`, `enh/water.cpp observeOpaque`, `enh/gi.cpp`, `enh/material_rt.cpp`, `enh/object_motion.cpp`) |
|---|---|---|
| `enh::RtScene` (DrawObserver; active whenever a consumer holds the RT scene: water RT, RT shadows, GI, material RT) | ~600 per main view = every static draw: 342,700 per 5 s, **99.8 % in `replayDraw`** (575 in `prepareDraw`) | the `ReplaceQuery` (kind, phase, tag {entity, opDesc, material, materialName, water}, vertex / index `Buffer`, offsets, stride, decl, `first / primitives / baseVertex / minIndex / primitiveType`, `engineVs / enginePs` hash + semantics, `textures[]` of the material slots (the signature), blend / alpha-test / depth-write / depth-bias state, instanced); **VS float registers c33..c35** (the world rows, `rowsAt(kInstanceDataBase + 1)`; instanced: c32 + 4i + 1); on a material memo miss only (`rt: scene: materials ...: 1 - 4 resolves per frame, 11 - 59 uv evaluations per 5 s`) `RtMaterials::uvMap` hashes + evaluates the VS / PS float registers (and int / bool files) the uv-map slice reads |
| `enh::Water` as replacer (`wantsOpaque()` + `observeOpaque`: the FFT ocean's sea-floor learning) | 8 - 50 k per 5 s (static solid draws while `floorPending_` has room) | vertex / index bytes (`readCpu`), decl, range, **c33..c35** world rows; once per (mesh range, placement) key; `handles()` never true |
| `enh::Gi`, `enh::MaterialRt` (observers), `ObjectMotion` (rigid: Debug only), `Gbuffer` / `Shadows` replacers, `MeshPartTable` | **0** calls at the player defaults (and with lighting off nothing at all is registered: the RT scene has no consumer) | Gi / MaterialRt tally per frame the PS registers named by semantics (ambient SH c15..c21 + indoor, DirLight, PointLights, FogColor / FogSettings = lighting c14..c37 and view c0..c4); ObjectMotion the vertex buffers + entity; the replacers `handles(kind, phase, tag)` |

**Decision (what an owned draw hands the observers).** (a) the observers are called **once per visible record per frame, exactly as `replayDraw` does today** (RtScene's registry `see()`
refreshes an instance every frame, it ages out otherwise: skipping the call would drop the object from the TLAS); (b) the query comes from the OpRecord (`CachedDraw.query` + tag +
`textures` are already retained for the packet cache); (c) the three world rows move from "read the register file" to a new `ReplaceQuery::objectRows` (3 float4 from the object table /
the entity matrix of the stamp), preferred by `RtScene::rowsAt` and `Water::worldRows`; (d) the register-dependent resolve (`RtMaterials::uvMap`, <= 12 per second) asks for a
"materialize this draw's registers" callback, i.e. composes that one draw the old way (rare, no CPU cost worth optimising); (e) Gi / MaterialRt (off at the defaults) get per-frame
snapshots instead of per-draw register reads: the view record's PS c0..c4 and the lighting entry's composed c14..c37 (`LightingImages` CPU twin, the owned path composes them anyway)
through a new per-distinct-lighting-entry hook, written when one of them is enabled in Full; (f) `DrawReplacer::handles` is asked once per record at compaction (a settings epoch in the
stamp); today none ever handles a static main-view draw. `replayAllowed` (observers shown the cached query) is the existing proof that (b) works.

**3. Invalidation rates** (changed / pairs = a key seen in the previous main view whose value differs; "new" = not seen in the previous main view; per main view; lod / overlay / illum ptr
are per solid op or entity, illum bytes per illumination drawn):

| input | spots (PD, frozen) | savegame hold | walk 60 s | beach circle (new game, moving all frames) | lightturn (monastery) |
|---|---|---|---|---|---|
| LOD resource (ctx +0x28) of a visible solid part | 13 / 645 k | 11 / 756 k | 12 / 1.14 M | 24 / 869 k (new 8,527 = 0.87 per view entering) | 34 / 174 k (0.017 per view) |
| entity overlay flags (+0x28 & 0xe00000) | 0 / 318 k | 0 / 382 k | 0 / 586 k | 0 / 443 k | 0 / 85 k |
| illum pointer (entity +0xf8) | 0 / 318 k | 0 / 382 k | 0 / 586 k | 0 / 443 k | 0 / 85 k |
| illumination record bytes [0, 0x110) | 1 / 502 k | 0 / 649 k | 0 / 970 k | 1 / 692 k | **218 / 138 k (0.107 per view)** |
| engine dirty byte (illum +0x110) set when drawn | never | never | never | never | never |
| light-set sun bit (list +0) | 0 / 4,570 | 2 / 4,931 | 2 / 7,454 | 0 / 9,773 | 0 / 2,042 |
| `holdsAll` (hypothetical: no dynamic light, every static light known by the table; the shipped code keeps `fullLitSkipsReads` off) | 0 / 502 k (all true) | 0 / 649 k | 0 / 970 k | 69 / 692 k (the first frames) | 48 / 138 k (0.023 per view) |
| `LightTable::registryComplete()` | 0 flips | 0 | 0 | 1 (startup) | 0 |
| material pool bytes (VS + PS pools read at the main view's first use) | 8,593 / 262 k (**3.3 %, 1.9 per view**) | 9,394 / 317 k (3.0 %, 1.9) | 14,489 / 503 k (2.9 %, 1.9) | 13,855 / 450 k (3.1 %, 1.4) | 2,503 / 85 k (3.0 %, 1.2) |
| material sampler images / ready state (per shader, per pass) | 11 / 510 k | 5 / 578 k | 5 / 907 k | 2 / 884 k | 4 / 162 k |
| lightmap slots (res +0x90 / +0x98, flag, image data) | 3 / 318 k | 0 / 382 k | 0 / 586 k | 0 / 446 k | 0 / 85 k |

Reading: every stamp the OpRecord needs is quiet (<= 0.02 changes per main view, most 0), **except the material pools**, which change for ~3 % of the drawn materials every frame
(the time-driven ones: water, lava ...; 1 - 2 per view). That argues for a record stamp on the material's *structure* (graph id, texture slots / images, sampler states: 0.001 - 0.002
changes per view) and NOT on the pool bytes, which the Material table re-uploads per frame anyway (7.7) and the windows read at draw time. The only place light stamps move is a camera
turn / walk between lamps (lightturn: illumination bytes 0.1 per view, holdsAll 0.02 per view; a record rebuild per such change is cheap). Caveats: the spots freeze game time (lower
bound); the savegame hold stands still; the walk run's hero walked only ~14 m in 60 s (one moving window) and the beach run circles ~10 m; no run crosses a town. Visible-set churn
("new" keys) is the cost of walking: ~0.9 new part-keys per main view entering the view in the beach run, 0.08 at the savegame hold, i.e. record creation, not invalidation.

**4. CTAB / reflection: float registers the static material vertex shaders read** (decoded bytecode, `def`'d constants excluded, relative reads flagged; every static z / solid draw's
VS; the same 12 non-instanced VS (VS_Default / VS_DefaultZPass permutations) in every run, 1 distinct read set; 2 - 3 instanced VS apart):

* non-instanced static VS read **c0..c3 and c11** (c0..c3 = view-projection, or the depth-bias one; c4..c10 are NOT read), **c32..c35** (g_vInstanceData: fade in c32.w, world rows c33..c35)
  and **c232..c239** (the material's VS pool). **c12..c31 are read by none of them** (no relative addressing): VS window 1 (c12..c30) and c31 of window 2 never matter for them,
  so the per-view record can be c0..c3 + c11 only (5 float4, no `dbvp` c0..c3 variant needed: no UseDepthBias material was drawn in any run) and window 1 may point at any valid address.
* the instanced VS read c0..c3, c32..c39 and (relative, `c32 + 4 i`) c40..c255: its instance blocks (up to 50 x 4 float4) are an image of consecutive instance rows.

**Recommendation: first-cut scope of the owned records.**
1. **Include in cut 1:** z ops (all non-hierarchy, non-switch: 80 - 93 % of z draws) and solid non-instanced entity ops, table-lit, as section 9 says. Expected coverage 53 - 72 % of the
   main-view static draws (57 % spots, 72 % savegame): expect that share of the section 9 estimate (-0.5 .. -0.7 ms) for the compose + packet path, not all of it. Keep the guards for
   fade < 1, overlay flags, shader class, video, sort id, inherited stages, CPU streams, on-demand compile: they never fire in these runs, so they cost no coverage.
2. **Widen next, in this order of payoff / cost:** (a) **hierarchy meshes** (+ 9 - 24 points of all static draws: 7 - 20 % of z, 16 - 28 % of solid draws = the level geometry: landscape,
   harbour, monastery shells): the node's first index / count are static per mesh resource (`ptr(sub,0x40) + 0x28 * node`), so an OpRecord can hold them (stamp: MeshPart generation);
   the 3 resources named above carry it all; (b) **instance-list ops** (+ 5 - 7 points: one instance per op, 13 - 20 % of solid draws) with an instance-record table keyed (resource, index)
   instead of the entity object table; (c) the **hardware-batch** ops (40 - 48 % of the solid draws, 20 - 30 % of all static draws, avg 5 - 10 instances) last: they need the instance rows
   as a c32 + 4 i image (a block per batch), and with lighting off they explode to 2.4 M draws; keep them on the old path in cut 1.
3. **Exclude from the owned design (small or zero):** switch-element materials (0.1 - 0.7 % of ops, 2 % of z at the monastery), overlay-flag entities (0.1 % of z with the focus highlight),
   no-light-list ops (14 per run).
4. **Table lighting is a precondition, not an option:** with `lighting.clustered` / `full_lit` off no solid op is eligible (the pass count follows the dynamic light count; 3x more draws).
5. Stamps: Material structure (not pool bytes), MeshPart generation, LOD (ctx +0x28), illumination record + light-set epoch, overlay flags, illum pointer, lightmap residency; all <= 0.02
   changes per view in the measured scenes.

### 7.13 F2c-1 object table (2026-11-06, `native.owned_objects`)

Debug setting (Bool, runtime, default off, Full, `needs` native.owned_windows + native.material_table; off = nothing allocated). Files: `src/gpuscene/object_compose.h` (pure: record,
rows, window, tests `object_*`), `object_table.{h,cpp}`, `window_tables.*` (view block extension), `StaticOps::setOwnedFor` (the only edit in static_ops.cpp), `d3d9/owned_windows.h` +
`draw.cpp` (`Kind::Object`, `ignoreMask` / `wOnlyMask`, stats), `LightingImages::entryFor`, `LightTable::registryGeneration`, `rvk_d3d9test` case "owned windows (VS 1 / 2)".

* **ObjectRecord (96 B, indexed by scene-table slot, capacity 32768 = 3 MiB, 4 KiB pad):** c31 (0: the engine keeps the actors' / trees' carry-over there), c32 = (0, 0, 0, fade),
  c33..c35 = columns 0..2 of entity +0x60 exactly as `writeWorld` (m[k], m[4+k], m[8+k], m[12+k]), info {generation, version, flags, live}. VS window 2 base = record - 31 x 16
  (covered 5 registers). A VS that reads c31 is refused (`c31 live`, 0 seen); c32.xyz is carry-over and only .w is compared by split_verify (`wOnlyMask`).
* **Update rules.** `ObjectTable::serve` per draw (composition done): the slot's signal-fed matrix must equal the composed rows (`matrix check`, the "table matrix vs entity +0x60 now"
  test), the CPU shadow is rewritten right then (so split_verify's twin is what the present uploads) and marked. The GPU sees ONE value per frame (uploads precede all draws), so a
  record already served this frame cannot change: a draw with another fade / matrix keeps its upload (`fade / matrix conflict`). Rewrites: `new records`, `fade rewrites`,
  `matrix rewrites`. The present drains the scene table's dirty list (matrix signal) and re-composes existing records; a slot served this frame whose version changed after the serve
  counts `changed after serve`. Only when nothing else drains (`instances().ready()` false; native.gpu_scene is frozen 0).
* **Which of c12..c31 are per view / per draw (measured, all runs):** no non-instanced static mesh VS (z or solid) reads any of c12..c30 (VS window 1 need = 0 in every draw: the
  device never counted a VS1 served or short window), so window 1 is trivially view-wide for them; the view block carries c12..c30 anyway (`ViewRecord` 576 B, candidate =
  `mirror_.adminVs[12..30]`, a draw whose composed c12..c30 differ keeps the upload: `composed differs`, 0). c0..c3 are the view-projection or `dbvp` (UseDepthBias material,
  shader +0x78); the depth-bias variant is just another content-deduped block of the frame (`WindowTables::dbvpBlocks()` / `vs1SplitBlocks()` count them; not in the 5 s log).
  c31 is never live for these draws; c32 = fade (per draw: z 1 / solid op fade), c33..c35 per object. Instanced draws (c32 + 4i) are not covered (`instanced`).
* **Coverage** (draws of Static-group mesh ops, VS window 2 served by the device): Vulkan 3 spots 441,324 of 443,648 non-instanced candidates (99.5 %; the rest read no c31..c35),
  + 128,969 instanced draws left on the old path; D3D12 610,486 / 613,901; savegame 140,220 / 140,686 + 23,015 instanced. Fallbacks: not ready 0, no slot 0, c31 live 0, matrix
  differs 0, conflict 0, short windows 0. Uploads: 551 new records + 21 matrix rewrites in 3 spots (0 - 17 records / frame; savegame 783 new, 476 matrix rewrites).
* **Verification (all 0):** split_verify MISMATCHES 0 (owned windows verified 4.30 M Vulkan spots / 6.10 M D3D12 / 4.28 M Vulkan validation / 0.78 M savegame), `uncomposed differ` 0,
  object-table read-backs every 240th frame DIFFERING 0, view table read-backs 0, matrix check bad 0 and changed after serve 0 in every run, no `[error]`, no validation message,
  no device loss. Runs (rvk.log in `rvk-work/scratch/f2c1/`: `val1` first run, `vk_spots`, `f2c1d3d12_spots.log` in scratch, `f2c1vk_val.log`, `save_vk`): (1) first run per the GPU safety
  rule: pub_interior 120 s Vulkan `-Validation` + `VK_KHRONOS_VALIDATION_VALIDATE_GPU_BASED=GPU_BASED_GPU_ASSISTED`: clean; (2) Vulkan `harbour_docks,jungle,pub_interior`; (3) D3D12 same;
  (4) Vulkan `-Validation` same; (5) Risen_Save_0005 (lod 2x, player defaults). `rvk_d3d9test --validation` passes on both backends with the new case; `rvk_tests` 284 / 284.
* **Also in this stage:** `LightingImages::entryFor(slot, ComposeParams)` (the entry of `note()` without drawn registers; unit test shares keys with `note`), `LightTable::registryGeneration()`
  (bumped when the "registry complete" flag flips or the order-independent hash of the last build's light positions changes; test `light_table_registry_generation`).
* **Not measured:** CPU gain (none expected: the composition still runs; the point is the records F2c-2 builds DrawRecords from) and picture A/B (no per-draw difference is possible
  by construction + verify; the picture was not compared).

### 7.14 F2c-2 op records (2026-11-06, `native.owned_records`; compare only, nothing derived is submitted)

Debug setting (Bool, runtime, default off, Full, `needs` native.owned_objects + native.lighting_images; run.ps1 test runs additionally need `native.scene_draws` only for the scene-form
comparison: without it both sides say "not a scene draw"). Files: `src/native/op_records.h` (pure: key, stamp + `firstDifference`, exclusion / field / audit names, statistics; test
`op_record_stamp_first_difference`), `src/native/static_ops_records.cpp` (StaticOps members `recEnter / recComposed / recDraw / recEnd / recReport`), hooks in `static_ops.cpp`
(`opEnterImpl`, `issue`, `finishOp`, `passDraw` / `specialPassDrawImpl` capture the alpha test, the mask and the lighting key into `sc::DrawDesc`), `d3d9/scene_draws.cpp`
(`SceneView`, `Device::setSceneProbe`, `Device::deriveScene`), `Device::variantEpoch()`, `MaterialTable::structVersion`, `ObjectTable::windowFor`, `ReplaceQuery::objectRows`.

* **OpRecord** (keyed ctx, desc, list, viewKey = 0 for the main view): `Stamp`, `draws[]`, the composition's read log (`ret::ReadRec`, finalised), 8 anchors (ctx, mesh resource, entity,
  illumination, light set, sub-mesh table, material, shader: the audit's classes), the side-effect list (mesh resource for `BindLightmaps` on solid ops, stream 3 on z ops, the images the draws
  sample: data only in this stage). Per draw (`DrawRec`): an own copy of the retained `CachedDraw` (the packet cache's entry: pipeline / scene pipeline, vertex input + dynamic state values,
  index range, bindings, textures, variant decision, env), the buffers + strides, the sampled stages' textures and the seven sampler states the composition owns, the alpha test **as the
  pass's states give it** (`alphaTestState` = `deviceStates()[StAlphaTest]`, `rs.alphaFunc / alphaRef`, `maskSet / maskValue` = what the pass writes into PS c5.w), `matSlot`, the lighting key
  (`illumSlot` + `ComposeParams` {tableLit, holdsAll, n, firstLight}: `LightingImages::entryFor` makes the entry), `vsSet / psSet` (the composed registers: Material windows), the draw tag fields,
  `vs1Offered`. ~1.4 KB per draw, ~1000 - 1100 records in the 3 spots, 960 at the savegame.
* **Stamp** (read at the op's entry, 14 fields; `kStampNames`): LOD resource (ctx +0x28), entity, **geometry / material** (hash of the covered sub-meshes' ranges +0x28..+0x34, material, its cached
  state, shader pointer + vtable, blend mode, mask ref, trade-alpha / opacity / refracted / z-fill / depth-bias / sort-id / invalid flags, version limit, admin pointer; the vertex / index
  resource +0x88), **illumination** (illum + list pointers, sun bit, no-sun, static / dynamic light counts), lightmaps (res +0x90 / +0x98 images + residency), overlay flags, illum pointer
  (+0xf8), fade, switch value, object slot + generation (scene table; the matrix itself is the object table's), `Device::variantEpoch()` (bumped by the setters of GI / G-buffer / pack / tint /
  ubershader / jitter / LOD bias / alpha dither / cut-out thickening / render ratio / billboard fade / retain / scene draws), `registryGen` (`LightTable::registryGeneration()` **only while
  `fullLitSkipsReads()`**: only the table's `holdsAll` verdict depends on the registry and the shipped code never skips the light reads: with the global stamp every lamp movement of the
  light-turn scenario rebuilt every record, 85 -> 15 % valid; without it 99 %), and after the composition the **view** (admin pointer / version / type / z write / render mode, the global alpha,
  depth-bias order, fog) and the draws' **material structure versions** (`MaterialTable::structVersion`: bumped by a texture slot / sampler state / count / state word / flag / graph change,
  NOT by pool bytes; the materials are polled by the composition, so this stamp can only be compared after it). An op whose composition changes a stamp is rebuilt from this very composition.
* **Cut 1** (`orec::Excl`, counted per op, first reason): hardware batch, instance list, overlay flags, fade < 1, hierarchy, node op, no light list, not table-lit, shader class, sort id, not
  composed (left to the engine), switch material (the material table's `switchNotes` moved during the composition), draw level (inherited stage / CPU stream / not indexed / no lighting
  entry), no retain (compile at the slot's place), draw not retainable (no packet cache entry). Video images: 0 in every run (not tested separately).
* **Derive** (`Device::deriveScene(cd, DeriveIn, SceneView&)`): the record's own `CachedDraw` is validated like `replayBuild` against ITS shaders / modules, the phase, the consumers, the
  variant decision (**the ambient test answered from the record's illumination**: `IllumTable::compose(illumSlot, params)` -> the ambient registers c15..c21, not from the device's registers;
  the Gi variant the draw was offered, read from `Shader::variantState`: a module that exists now = another pixel shader and pipeline), the env (viewport, scissor, target formats, depth
  reversal), jitter flags, the stream / index bindings (`Buffer::gpu`) and the textures (null / pending / layout / bindless slot); then the **DrawBlock is built natively**: `tex[]` from the
  record's textures (bindless slot or the default slot), `smp[]` through `lookupSamplerStates` of the seven owned states + the device's setting of the stage's other states (ADDRESSW, border
  colour, max anisotropy: a global setting; **not** left to the composition), `posFix` from the viewport + jitter + `depthRemap`, `alphaFunc` / `alphaRef` from the pass states (alpha func is
  always ALWAYS: `SetMasterAlphaTesting(1)`), `psC5` = (view obstacle xy from the mirror, z, the pass's mask only when it sets one), pad[1] = `packWord()` (the pack's lookup by the draw tag) for
  GI / pack draws; the packet: pipeline (the scene pipeline), the scene vertex input (`internSceneVi`), dynamic state, first / count / vertexOffset, index binding, flags. Windows by table slot
  (`recDraw`): Material windows from `MaterialTable::fillWindows(matSlot, vsSet, psSet)`, VS window 2 from `ObjectTable::windowFor(slot)`, PS window 3 from `LightingImages::entryFor` + `windowBase`,
  the view-level ones (VS 0 / 1, PS 0) from the first draw of the view and list (the view block slot is a per-view input).
* **Compare** (`recDraw`, every issued draw of an op valid at entry): classes scene form, pipeline, vertex input / dynamic state, geometry, flags, tag, windows (kind, base, covered; view blocks
  at another slot with the same registers count as equal: `view blocks at another slot ...`, the dedupe search window), block tex / smp / posFix / alpha / pad / c5. **Ignored carry-over**
  (counted, nothing in the shader reads it): `alphaRef` when the test is off (the device keeps the last mask of an earlier pass), PS c5 when the pixel shader's live set does not hold c5
  (and c5.w when the pass sets no mask). A derivation that is **refused** (module, variant decision, binding, texture) makes the record stale: it is rebuilt from this composition
  (`derive refused`), a refusal for phase / consumers is not.
* **Signal audit** (`recAudit`): at every op entry the record's read log (`ret::validate`, SEH-guarded reads) is checked against the engine's memory while the stamps call the record valid; the
  composition runs with the log on only when the record is (re)built (`ret::gLog` = the store's log; view-level reads - `composeView`, `initMirror` - and the entity's matrix / whole flag word
  are paused with `RecNoLog`: the matrix is the object table's, only the overlay bits of +0x28 are used and stamped). A change the stamps called valid is a disagreement unless the material /
  view stamps (compared after the composition) catch it (`caught by the post stamps`); the first changed read is classified by the anchor it lies in and logged with its op.
* **Observers (item 4)**: `ReplaceQuery::objectRows` (3 float4, c33..c35) = the object-table CPU twin of the draw's window (`Device::ownedObjectRows`: set for draws whose VS window 2 is an
  object-table window, in `prepareDraw`'s query lambda and in `replayDraw`'s cached query), preferred by `RtScene::rowsAt` (non-instanced) and `Water::worldRows`; every handout is compared with
  the register file c33..c35 (`ObjectRowStats`, in the 5 s line): 0 differing in every run (1.8 M handouts in the 3 spots, 2.5 M at the savegame, 3.5 M walking, 1.4 M on D3D12).
  Nothing submitted changed (the rows are equal by the check). The register-dependent resolve (`RtMaterials::uvMap`, <= 12 per s) keeps using the composed registers; Gi / MaterialRt / ObjectMotion /
  `DrawReplacer::handles` are not touched in this stage (design 7.12: snapshots / settings epoch are F2c-3 work).
* **Findings that shaped the code** (all in the compare, found by it): the sampler state **beyond the seven owned ones** (max anisotropy etc.) is the device's, defaults give 25 % wrong sampler
  indexes; PS c5 and `alphaRef` are carry-over where nothing reads them (above); the GI / pack variant decision is the common case at the player defaults (`gate` always true with the pack
  provider), so the ambient test must come from the record's lighting; a cold variant cache (first run of a spot) makes `wanted` draws change shader and pipeline when the variant module
  appears (derive refuses and the record is remade: 0 DIFFERING with the variant cache deleted, `rebuilt 18,378`); view blocks are deduplicated by a bounded search, so an equal view block can
  sit at another slot; the registry generation is not an input of a draw that never skips the light reads.

**Numbers** (Full, `-PlayerDefaults`, `native.material_table / illum_table / lighting_images / owned_windows / owned_objects / scene_draws / owned_records`; logs in `rvk-work/scratch/f2c2/`;
"valid" = records valid at the op's entry; ops / draws summed over the run, loading frames included; DIFFERING = draws whose derived view differs from the composed draw in any class):

| run | eligible ops (share of the Static group's main-view z / solid ops) | records valid at entry | built / rebuilt | derived / compared with a scene draw | DIFFERING | audit disagreements |
|---|---|---|---|---|---|---|
| Vulkan 3 spots (harbour_docks, jungle, pub_interior), 4 runs | 1.12 - 1.18 M (59 %) | 99.4 % | 1.1 k / 0.8 - 4 k | 1.21 M / 1.17 M | **0** | 0 - 3 (1 - 3, `other`; 2 of them caught later by the post stamps) |
| Vulkan 3 spots, cold variant cache (scene / Gi variants deleted) | 1.11 M | 97.1 % (derive refused while variants build) | 1.2 k / 18.4 k | 1.13 M / 1.09 M | **0** | 3 (`other`) |
| Vulkan 3 spots, cold pipeline cache | 1.14 M | 99.5 % | 1.1 k / 4.1 k | 1.18 M / 1.14 M | **0** | 0 |
| savegame Risen_Save_0005 hold (lod 2x) | 1.96 M (75 %) | 99.4 % | 0.9 k / 0.9 k | 2.07 M / 2.00 M | **0** | 2 (`other`) |
| savegame walk 60 s (`rvk_rtwalk mode=walk`) | 2.75 M (75.5 %) | 99.0 % | 1.0 k / 16.9 k (LOD resource, material structure, illumination) | 2.90 M / 2.80 M | **0** | 1 (`other`) |
| `rvk_lightturn` (monastery hall) | 0.35 M (55 %) | 98.9 % (85 % with the registry generation as a global stamp: 15 % in the worst window) | 1.5 k / 1.0 k | 0.37 M / 0.36 M | **0** | 2 (`other`) |
| D3D12 3 spots | 0.89 M (59 %) | 95.4 % | 1.1 k / 1.3 k | 0.92 M / 0.90 M | **0** | 4 (`other`; 2 more caught by the post stamps) |
| Vulkan `-Validation`, 2 spots | 0.55 M | 98.8 % | 0.7 k / 0.8 k | 0.57 M / 0.54 M | **0** | 4 (`other`) |

* **Audit disagreements** are the same four kinds of read, all `other` (or the entity flag word before it was paused): an 8-byte pointer that goes from 0 to a resource while a texture streams in
  (the resolved texture / placeholder did not change, so the compare is clean: the read log is conservative) and a 4-byte count (`was 0x2, now 0x3`, D3D12 only). None came with a compare difference
  (`of them with a compare difference` stays 0); none is a missed writer of a record input, but the streaming pointer is the writer to name if F2c-3 wants the log to be exact: the material's image
  resource slot -> `textureOfImageData` (the material table's texture poll catches the change one op later: `material structure`).
* **Not derived** (counted, never DIFFERING): ~0.3 - 1 % of the draws: `variant gate` (a lit draw whose variant decision changed since the record: the record is remade), `stream / index
  binding` (6 - 38 per window while the mesh arena moves).
* **Rebuild reasons** (walking / light turning): LOD resource 1 - 190 per 5 s, illumination 3 - 4, entity 1 - 4, material structure 1 - 280, geometry / material 2, derive refused (variants /
  bindings) tens - 360 at a cold start, read log 1 - 4. `registry generation` rebuilds are gone with the `fullLitSkipsReads()` condition.
* **CPU per frame** (Debug setting on, mean of the 5 s windows with > 1000 ops; engine thread; the main view's ops): **record build 1 - 5 us** (a few hundred builds per window; 8 - 52 us
  while variants / the D3D12 backend churn), **derive + compare 370 us** at the 3 spots (the device's derivation alone 155 us), **575 us** at the savegame (248 us the derivation, 245 us stamps),
  stamps 160 - 260 us, audit 30 - 60 us (the read-log validation of ~1000 records per frame; the D3D12 run's means are higher, derive + compare 430 us, stamps 1.3 ms: its windows include the long start-up frames). The compare computes the content hashes of the vertex input / dynamic state, the 20-slot block, the
  windows and the tag of 150 k draws per second; F2c-3 needs the derivation only (no hashing, no per-draw window re-derivation): 0.15 - 0.25 ms per frame measured here is an upper bound for
  the draw-record rebuild of the owned ops, against the ~1 ms the composition + packet path of those ops costs (7.12 estimate -0.5 .. -0.7 ms).
* **Gates**: `rvk_tests` 288 / 288 (settings count 128), `rvk_d3d9test --validation` PASS on Vulkan and D3D12, `rvk_rhitest --validation` PASS on both backends (no GPU read is added: the records are
  compared on the CPU), no device loss, no `[error]`, no validation message in the Vulkan `-Validation` run (2 spots) nor the D3D12 run; the timing counters of the last build were measured in the
  last two runs (3 spots + savegame), the earlier runs predate only the extra timer.
* **Handed to F2c-3**: the record layout above is the one `Device::replayOwned` reads (`DrawRec::cd` = the packet, `DeriveIn` = the per-draw inputs); what is still composed per op at entry is the
  stamp (160 - 260 us per frame for ~1000 ops: batch it per entity / resource), the side effects (the record lists them, they are not replayed yet), the materials' poll (the stamp for the
  material structure needs it: `MaterialTable` re-reads a record at its first draw of a frame; an owned op must poll without composing), and the ambient bit (`IllumTable::compose` per lit draw: cache
  it per (illumination slot, params) once per frame).

### 7.15 F4-0 tree measurements (2026-11-06, `RVK_F4_MEASURE=1`; no behaviour change)

Counters only (`src/native/f4_measure.{h,cpp}`; hooks in `static_ops_trees.cpp` `composeTree` / `composeTreeBillboard` + ONE line in `static_ops.cpp` (`f4m::IssueScope` around
`issue()`, its header comes through `f2c_measure.h`)). The variable is read once at the first tree op (`GetEnvironmentVariableA`); off = one bool test per site. rvk.log
`native: f4 measure (5 s) <section>: ...` (sections `time`, `census`, `hist`, `trees`, `illum`, `inval`, `stream1 freq`; raw sums, a "view" = the ops sharing frame / view kind /
cascade / db / desc), `native: f4 ctab:` and `native: f4 stream1:` once per shader / buffer. Summing script: `B:\DevDrive\rvk-work\scratch\f40\summ4.py <log>` (sums the windows that
held a main view with tree ops). Runs (Full, Vulkan, `-PlayerDefaults`, own copy `risen-test-vk-f40`, logs `rvk-work/scratch/f40/*.log`): **spots** = rvkcompare `jungle, lake_jungle,
waterfall_jungle, swamp_camp` (time frozen, 4173 main views), **save** = Risen_Save_0005 hold 40 s with lod view / trees / meshes 2x (4577 views), **save lod.trees=3** (4711),
**walk** = `rvk_rtwalk` from that save, 60 s, turning 3 deg / s (7164 views; the hero walks, trees stream in: "fresh").
Caveats: other agents ran games concurrently (absolute ms are noisy, shares are what matter); the timers cost ~2 QPC reads per phase switch (per tree in `readLight`) and the
census itself costs ~0.2 ms per category per frame (reported as "census", NOT in the sums): the phase numbers are an upper bound, `light` most of all (2 QPC per solid instance).

**1. Phase timers** (ms per view of the category, save run; the other runs within +-15 %: spots 0.72 / 0.43 / 0.29, walk 0.59 / 0.38 / 0.20, lod.trees=3 0.67 / 0.40 / 0.22 per main-view frame).
"issue" = `StaticOps::issue()` of the tree ops (outside composeTree).

| category | queue | snapshot | readLight | rows | plan | draw (materialDraws / specialPassDraw) | other | issue | sum | views / frame |
|---|---|---|---|---|---|---|---|---|---|---|
| main solid | 0.026 | 0.029 | 0.069 | 0.022 | 0.009 | 0.157 | 0.062 | 0.275 | **0.649** | 1 |
| main z | 0.026 | 0.016 | - | 0.020 | 0.007 | 0.097 | 0.060 | 0.164 | **0.389** | 1 |
| shadow z (cascades etc.) per view | 0.017 | 0.006 | - | 0.007 | 0.003 | 0.030 | 0.024 | 0.052 | 0.140 | 1.5 -> 0.21 per frame |

Per main-view frame the tree ops cost **~1.25 ms** (spots 1.44, walk 1.16), of which `issue` 0.5 and `draw` 0.3 are per-DRAW costs (composition + device calls); the per-INSTANCE work
(queue copy + sort, snapshots, light reads, rows, plan: main + shadow) is only **~0.26 ms**. Billboards (per main-view frame, solid + z; no shadow billboards): 13 ops / 26 draws per
list in the save, 21 ops / 43 draws at the jungle spots; compose 0.17 ms + issue 0.08 ms (save), 0.30 + 0.27 ms (spots). Vegetation not measured here.

**2. Census** (per view of the category; save run, spots in brackets; z and solid see the same trees and ops):

| | main solid | main z | shadow z (per view; 1.5 views / frame) |
|---|---|---|---|
| unique trees | 237 [261] | 237 | cas0 15 [24], cas1 37 [72], cas2 416 [314] |
| tree ops (type 0 / 1 / 2 / 3) | 79.6 (21 / 24 / 26 / 8) [71.9] | same | 31 (11 / 3 / 14 / 3) [36] |
| queue entries / instances (rows) | 643 / 681 [681 / 754] | same | 278 / 272 |
| (op, type, LOD) buckets | 137 [127] | 137 | 37 [44] |
| today: planned batches / device draws | 138 / 129 [134 / 126] | 135 / 126 | 36.5 / 36 |
| **calls under the F4 cut rule** (shader x vb x ib x topology x sun) | **77 [71]** | **77** | **31 [36]** |
| hardware / software instancing | 79.6 / 0 (all) | all | all |

* **Calls**: the cut rule gives ~one call per op (77 vs 129 device draws today, -40 %); the buckets (137) become args records of those calls. Today's batches (138) are already close to the
  bucket count, because the 88 % illumination cuts cost little at this scale (solid draws 129 = 1 pass per batch + a few multi-pass ones): **the F4 win is per-draw CPU (compose + issue),
  not fewer GPU calls alone.**
* **Instances per bucket** (share of buckets; save, main): 1 inst 23 - 31 %, 2: 13 - 18 %, 3-4: 15 - 22 %, 5-8: 18 - 24 %, 9-16: 9 - 18 %, 17-32: 2 - 9 %, > 32: ~1 %; mean 5 per bucket. Args records
  are the right shape (a bucket is rarely worth its own call).
* **Two leaf-LOD buckets** (trees in leaf ops with both +0x58 and +0x5c >= 0): main 23 % (save), 32 % (spots), **50 % with `lod.trees=3`**; cascades 7 - 45 %. Instances > queue entries by that share.
* **Trees without an entity (tree+0x18 & 0x10)**: **0** in every run (entity world matrix always available).
* **Sun values**: every tree instance had the sun on (100 %, one light list per frame in every main view, 0 off-only / both): at the player defaults the sun-split call key never splits.
* **Illumination classes** (solid instances): plain (list only) 75 - 79 %, 1 - 2 static lights 20 - 25 %, static ambient SH 0.1 - 0.9 %, **> 2 static lights 0, dynamic lights in the list 0, no list 0**.
* **Hardware instancing** (admin +0x320 != 0): all ops in all runs; stream-0 divider `n | 0x40000000` on every draw; software path never ran (still composed by the old code, not measured).

**3. Invalidation** (changed / pairs, per tree per view; main view; a pair = the tree was in the previous view of the same category; "fresh" = not in it):

| input | spots (frozen) | save hold | save lod.trees=3 | walk |
|---|---|---|---|---|
| LOD ints +0x48 / +0x50 / +0x58 / +0x5c | 0.02 % each | 0.000 % (2-5 of 1.08 M) | 0.000 % | 0.002 % |
| float refs +0x60 / +0x64 | 5.5 / 2.9 % | 0.38 / 0.31 % | 0.55 / 0.69 % | 3.7 / 2.1 % |
| +0x6c | 10.4 % | 0.72 % | 1.14 % | 7.7 % |
| any of the 7 fields | 10.4 % | **0.72 %** | 1.14 % | 7.7 % |
| entity matrix (entity +0x60) | **0 / 1.09 M** | 0 | 0 | 0 |
| illumination pointer / bytes [0, 0x110) | **0 / 0** | 0 / 0 | 0 / 0 | 0 / 0 |
| render buffer tables (+0x38.., +0xe8..) | 0 / 105 k | 0 / 125 k | 0 / 129 k | 0 / 191 k |
| wind block: admin +0xac (matrices) / +0x6c (rock) | 0.38 / 0.36 % of views | 0.22 / 0.20 % | 0.21 / 0.19 % | 0.15 / 0.13 % |
| billboard constant block (c32..c36 + c37+j) | 0 / 89 k | 0 / 60 k | 0 / 60 k | 0 / 92 k |
| fresh trees per run (main) | 2440 | 762 | 761 | 779 (the walk streams few) |

Reading: the LOD index ints almost never change; the float fade ratios / +0x6c change for 0.4 - 8 % of the trees per view (distance-driven, more while walking or at the spots' frozen game time
where the cascades / fades keep moving: not zero-cost but a per-view snapshot is cheap); **entity matrix, illumination and render-buffer tables are constant** (trees do not move: rows could
persist per tree; the sun-lit illumination never changes). The wind block changes in 0.1 - 0.4 % of the views (so c14..c29 is a per-view record that is almost always the same).
Cascade views read the same: 0.00 - 0.04 % for the ints, up to 12 % for any-field at cas2 (spots).

**4. CTAB / stream 1** (decoded bytecode, 10 tree VS + 9 PS shaders per run, the same in every run; `native: f4 ctab`):
* **VS** (every tree VS, z and solid, shadow views): reads c0..c3 (+ c11 for the 3 main-view z variants), **c14..c31** (wind c14..c29, c30 / c31 fog etc.; 14..31 in full), c32..c39 and
  **relative reads c40+** (the instance block: `g_vInstanceData(vInstance * 4 + k)` with relative addressing, instanced flag set). None reads c4..c10 or c12 / c13.
* **PS**: solid PS read c0, c1, c3, c4, (c6 / c7 / c8), c14..c31 (lighting), c32..c36 (billboard / leaf texcoord rows) and **never c5**; the z-pass PS read **c5 (MaskReference) and c0 / c6 / c7**: the alpha-test value matters for the z passes only,
  as `leanMaskRef` assumes. Shadow-map PS read c5 only.
* **Stream 1**: ONE buffer in every run (`size 1024 B, stride 4, offset 0`): **frequency `0x80000001` (INSTANCEDATA | 1: one element per instance)**, a **float identity ramp 0, 1, 2 ... 255** (256 elements, all equal to
  their index), shared by all tree draws (z, solid, shadow, every type); stream 0 carries the divider `n | 0x40000000`. 100 % of the 1.29 M - 2.24 M composed draws per run used it.
  => the GPU-driven design may replace it by our own ramp (>= the row capacity: today <= 255 instances per call under the batch limit) and later set vInstance from `firstInstance` / ref.y.

**5. RT coupling at the player defaults**: `rt: scene: ... animated SpeedTrees 0`, `tree draws 0 (0 beyond the range, 0 captured)` in every window of every run: `RtScene::animated_` is off
(`setAnimated` is only called by the RT shadows / GI consumers, both off by default), so no tree draw is captured and `observeTree` is never reached. Composition for the animated-tree path is
therefore only needed while `shadows.rt` or `gi.enabled` is on (not measured; code reading: `observeTree` takes the draw's rows from the VS c32.. block and the instanced stream, one capture per
tree within `animRange_`, `kMaxAnimatedTrees` / 512 per frame).

**Recommendation.**
1. **F4-1 billboards first** (confirmed easiest): the constant block never changes (0 / 300 k pairs), 13 - 21 ops per list per view, ~0.25 - 0.55 ms per frame (compose + issue), so the estimate of -0.15 .. -0.25 holds (the upper part at jungle-like spots).
2. **F4-2 tree records + per-view plans**: the plan inputs are the render-buffer tables (never change), the 7 LOD fields (snapshot per view, 0.7 - 10 % changed) and nothing per-tree else: rows (entity matrix) and
   illumination are constant (retain per tree, key = tree pointer + the table epoch). The sun-split is dead at the defaults (keep the bit in the key, no work).
3. **F4-3 scope**: the target is the per-draw work, `draw` + `issue` + `other` = ~0.8 of the ~1.25 ms; the per-instance work is 0.26 ms (the design's -1.0 .. -1.4 is optimistic: expect **-0.7 .. -0.9 ms** per frame
   with main + shadow views done, -0.5 .. -0.6 for the main view alone). Calls 129 -> 77 (main), 36 -> 31 (shadow): little GPU gain, the CPU gain is retained DrawRecords + one args record per bucket (137).
   Do the shadow views (F4-4) with the main view: they are ~1/6 of the cost and cas2 sees 300 - 430 trees.
4. Bucket records: 5 instances on average, 23 - 50 % of the trees in two leaf buckets: records must support a tree in two buckets (as args records, not per-tree rows).
5. Keep composition for the software instancing path and while `RtScene::animated_` is on; no entity-less trees exist (drop that guard's cost model), 0 dynamic-light / no-list / > 2 static-light illuminations (the
   simple per-instance lighting image is enough at the defaults).

### 7.16 F2c-3 owned static draws (2026-11-06, `native.owned_static`; correct, no CPU gain yet)

Debug setting (Bool, run time, default off, Full; `needs` native.owned_records + native.scene_draws, so also the whole F1 / F2c chain) + `native.owned_check` (Int, default 10, 0 = never). Files:
`src/native/static_ops_owned.cpp` (all of it: `ownedViewBegin / ownedListOk / ownedSelect / ownedTake / ownedPass`, the 5 s line), `frame_tap.cpp` (`compactOwned`, `passBegin`),
`engine/scene_hooks.cpp` (`OpTap::passBegin` after BeginZPass's original and after the BeforeSolid injection), `d3d9/scene_draws.cpp` (`Device::replayOwned`), `static_ops_records.cpp`
(`recExclusion`, `viewStampOf`, the side-effect lists in the record), `gpuscene/material_table.*` (`pollOwned`, `fillWindows(.., pad)`), `window_tables.*` (`fillViews` with explicit registers),
`DrawObserver::registersRead`, test `rvk_d3d9test` "owned replay (records)".

* **Compaction (frame tap, main view only).** `FrameTap::beginView` reads the raw (unsorted) z / solid list; every Mesh op whose record is eligible, `ownable`, and whose stamps equal the record's
  (`recStamp` minus view / materials) is taken out of the engine's array **in place** (the kept ops keep their relative order, the count at db +list+0x10 shrinks); `raw_` / the snapshot / the sort copy /
  the attribution describe exactly what Execute then runs, so there are no "not entered" counters. The owned ops are sorted with the engine's comparator and kept in `StaticOps::ownedOps_`.
  Only when the z list is executed (caps +0x1590) / DoBeginRenderSolid is called (caps +0x15a0 == 0), not with `native.owned_observe`, not on a check frame / its warm-up frame
  (`sc::leanFrameKind(frame, native.owned_check)`), not while an observer that reads more than material constants is registered (`DrawObserver::Registers`: GI, material RT, object motion = All; the RT scene = Material).
* **Drawing.** `OpTap::passBegin(false / true)` fires after BeginZPass returned / after BeforeSolid: `ownedPass` composes the view like the first op's composition would (`composeView`, `initMirror`), writes the view
  block once per pass (`fillViews` with the mirror's c0..c11 / c12..c30 / PS c0..c4: no register file), then per op: view stamp at the pass (the admin's type bytes differ between the passes: not computable
  at viewBegin, found in the first run), materials polled once per slot and frame (`MaterialTable::pollOwned`, the missed writer of 7.14) and their structure versions compared with the record's,
  the object record served (`ObjectTable::serve` with the entity's matrix: same matrix check, fade 1 / the op's), then per draw `Device::replayOwned`: `deriveScene` (one derivation: validation + DrawBlock),
  all eight windows from the caller (material windows padded to the whole window: a composed draw read the register file's carry-over beyond the pool's count, the record's array holds zeros), observers
  with the windows' register images loaded first (the RT scene's uv map), `uploadConstants`, a guard (every window the shaders read must be served, `ownedServed_`), the DrawBlock, the scene packet
  (`toScenePacket`: null address guard), `submitInterned`. Nothing is queued before a refusal (reasons 1..15 in `ownedReplayStats`, listed in the 5 s line). An op that cannot be drawn this way (material stamp,
  object window, a refused draw: a texture still pending / in another layout, a variant gate) is **composed right there** (`opEnter`: what the engine's slot would have done, the record is rebuilt): never lost.
  Side effects per owned op afterwards: `BindLightmaps(res, 1)` (+ our lightmap slots), `touchNow` of every image / material the slot touches, `EnableStream(3, ..)` (lists collected while the record
  was built: `recCollect_`, a record built before is rebuilt once: `ownable`).
* **Order.** Owned ops are drawn first in each pass (their relative engine order kept), before the engine's own ops of that pass. Eligibility now also requires opaque, depth-writing draws (new exclusions
  `blended / no z write`, `depth-bias material`, `lean block`), so the order against the engine's blended terrain layers / decals (hierarchy ops, the decal admin: after the solid pass) is immaterial;
  the pass batches draws by pipeline anyway.
* **Check frames.** Every 10th frame (and the one before it) nothing is compacted: the old path composes, the F2c-2 compare derives each draw and compares; any DIFFERING in a 5 s window turns owned_static off
  (error line). The compare now only compares the registers of a view window the shader reads (the first runs showed non-live carry-over registers of view blocks differing between draws of one view: a compare
  artefact, 60 draws, turned the feature off once).
* **Safety steps.** (a) every owned submit is guarded as above (window bases, DrawBlock, frame block, ref stream, pipeline, scene record; `packet_.scene` is reset by `toScenePacket` / `submitInterned`);
  (b) `rvk_d3d9test` "owned replay (records)" on both backends with `--validation`: 8 owned draws = one indirect call over 8 records, 0 bytes differ from the normal draws; the null-address guard refused 8 of 8,
  a changed stream 8 of 8 (code 6), another bound shader (11), nothing drawn by any refusal, owned draws again afterwards: 0 failures, 0 validation errors; (c) the first game run:
  pub_interior, Vulkan, `-Validation` + `VK_KHRONOS_VALIDATION_VALIDATE_GPU_BASED=GPU_BASED_GPU_ASSISTED`: PASS, no VUID, no device loss; the rebuilt code again with pub_interior + jungle: PASS, clean.
* **Numbers** (Full, `-PlayerDefaults`, logs `rvk-work/scratch/f2c3/`): owned share of the Static group's main-view z / solid ops (mesh ops looked at, incl. hardware batch / instance list / hierarchy) 3 spots 52 - 68 %,
  savegame hold (lod 2x) 39 %, walk 74 %; 97 - 99.7 % of the owned ops drawn from records, the rest composed late (almost all: one op per view whose slot-0 texture is a 1600x900 render target in
  COLOR_ATTACHMENT layout), 0 undrawn, check frames DIFFERING 0 in every run (Vulkan 3 spots + monastery_hall, Vulkan validation, D3D12 `-Api d3d12` pub_interior + harbour_docks, savegame, walk 150 s, mode switches).
  Run-time Full <-> Wrapper (`-Mode wrapper -Setting renderer.mode=full`, 4 switches inside pub_interior's hold): owned_static follows (`owned static on / off`), 0 undrawn, no error; after the last switch
  ~10 ops per view stayed on the late composition (replay refusal `variant`: the ambient test of the record's lighting differs from the composed decision, not found yet; harmless).
  Residency: `placeholder binds 0` in the walk and the savegame runs; touches per owned op replace the slot's RequestData (counts in the 5 s line: 6 - 9 touches per owned op-view); no texture went missing.
  Picture A/B (7.11 method, toggled inside one hold with 300-frame periods, shots at frames not = 0 / 9 mod 10): pub_interior toggle pairs 0.7 / 3.1 mean abs vs controls 0.4 - 3.9, harbour_docks 0.5 - 1.3 vs
  control 0.7, jungle 2.2 - 2.6 vs control 1.7 - 2.9 (wind), **monastery_hall 0.06 / 0.014 / 0.003 mean abs, max 2 levels** (eye adaptation only): the toggle pairs lie inside the control pairs' range.
  refcompare Full (stored reference 20261022-093252, whose hashes no longer match spots.lua / spass: the absolute verdicts are stale; `-Setting` = the F1 / F2c chain + `lighting.clustered / full_lit`, owned_static on vs off, same run conditions): both arms 13 FAIL / 4 WARN / 3 PASS with the same spots failing (the FAIL set is the known Full lighting differences + the stale reference), per-spot ssim within the run noise (pub_interior 0.9853 on / 0.9847 off, monastery_hall 0.9727 / 0.9727, jungle 0.7976 / 0.7982, harbour_docks 0.8349 / 0.8350). Without table lighting (a plain Full refcompare) owned_static is gated off (`not this view ... table lighting false`).
* **CPU (the honest result: no gain).** Savegame, `-Perf`, `RisenTest.exe`, vsync off, shared copy, interleaved, engine + HAL CPU per frame: base (no records, no owned) **6.69 / 6.60 ms**, owned with `owned_check=0`
  **6.65 / 6.70**, owned with check 10 **6.67 / 6.70** (GPU 3.1 / 3.35: unchanged). Per piece of the owned path at the savegame (per main view = per frame, 78 owned ops / 82 draws, check 0):
  compaction 62 us (of it the stamp + record checks 32), view + window setup 2 us, material polls + stamp compare 12 us, build 76 us (DeriveIn, window lookups, object serve, ambient memo), side effects 5 us,
  submit 124 us (`replayOwned`: derivation + observers + the scene packet; the RT scene's observe is in it as it was in the old replay) = **281 us per frame, 3.4 us per owned draw**; the pub_interior spot (560 owned ops /
  frame, validation off) compaction 211 (stamps 119), material 35, build 193, side effects 26, submit 527 us. The composition + issue it replaces was already cheap (retained packets, F2b-1: the packet
  cache replays 99 % of those draws), so the owned path trades ~1.7 us / draw of compose + packet replay for ~3.4 us of stamp + derivation + window + submit work. Not tuned further (coordinator's rule: report the
  pieces). Where the cost is: the stamp (~60 reads per op: batch per entity / resource), `deriveScene` + `replayOwned` per draw, observers; ideas: a per-frame epoch for the global inputs, persistent DrawRecords
  per op (F2c-4: nothing derived per frame, only the dirty ones), moving stamps to the signals. Check frames cost ~0.1 ms / frame averaged at the savegame (20 % of the frames run compare).
* **Gates.** `rvk_tests` 288 / 288 (settings count 130), `rvk_d3d9test --validation` PASS both backends, Vulkan validation + GPU-assisted clean.

## 8. F1 / F2 design decision: windows onto persistent tables (2026-11-06, orchestrator)

**Decision.** F1 does not start with new shaders. Translated shaders and the ported ubershader read every float register through 4 register windows per
stage (`RVK_VSC(n)` / `RVK_PSC(n)`, `shaders/include/rvk_draw.hlsli`; window starts `abi.h kVsWindowFirst = {0, 12, 31, 232}`, `kPsWindowFirst =
{0, 5, 6, 14}`: `c[n]` = `rvkRawLoad(window base + n * 16)`). The window classes already match the tables:

| window | vertex stage | pixel stage |
|---|---|---|
| 0 | c0..c11 view / camera (per view) | c0..c4 camera, fog (per view) |
| 1 | c12..c30 vegetation / tree data | c5 mask reference / obstacle (per draw: DrawBlock `psC5`) |
| 2 | c31..c231 object rows, instance data, bones | c6..c13 material `g_ShaderData` (Material table) |
| 3 | c232.. material `g_vShaderData` (Material table) | c14..c37 lighting (Illumination + LightSet, composed) |

So a window can point at a **persistent table record laid out as a register image** (record address - first register x 16) instead of a per-draw
ring upload, and every existing shader reads identical values: picture identical by construction, verified with the existing `debug.split_verify`
(every window block a draw reads compared with the CPU register file) and the tables' own checks.

* **F1a (CPU path, persistent windows):** for the static ops N5 composes, the windows that a table covers point into the table: PS window 2 + VS window 3
  -> a Material register image (c6..c13, c232..c251), VS / PS window 0 -> a per-view block written once per view, VS window 2 -> a per-instance object
  block (the world rows / fade the composition writes, re-written on the matrix signal or a poll), PS window 3 -> a lighting register image per
  (illumination, pass parameters) composed from the Illumination / LightSet records (first on the CPU with `gpuscene::composeLighting`, then F1b on the
  GPU with its HLSL twin `rvkIllumCompose` in one compute pass per frame). Window bases may lie below the record (base = record - first x 16): Vulkan
  addresses are plain 64-bit arithmetic; D3D12 (heap slot, byte offset) needs offset >= first x 16, so register-image tables start with a pad of
  first x 16 bytes (VS window 3: 3,712 B; a 4 KiB pad covers every window). Gate: `split_verify` mismatches 0, picture identical, the composition
  of the covered registers skipped (CPU measured, gpu-scene.md 9.2 bounds the setter part at < 0.05 ms: the value of F1a is the proof of the register
  images, the CPU gain comes with F2).
* **F2 (GPU-driven):** one uniform shader change for every pipeline of an owned draw: a *scene variant* where `rvkVsWin(w)` / `rvkPsWin(w)` read the 8
  window bases (+ the DrawBlock address) from a per-instance **DrawRecord** selected by the instance-rate ref stream (section 4) instead of the push
  block. DrawRecords are written by the compaction compute from (instance slot, part, material slot, illumination slot, pass); indirect draws per
  pipeline bucket. Translated engine shaders stay valid: batches are still split by pipeline (the engine's permutations), but no longer by register
  values, and the CPU no longer touches the draw. The owned ubershader (graph-shared pipelines, dynamic light loop: 196 pipelines estimated, 7.3)
  is the later step that merges permutations.

## 9. F2c design: DrawRecords from retained op records, no per-frame composition (2026-11-06, accepted by the orchestrator)

**Hook point (changes 3.2 / F2's "suppress at RequestRenderOps"):** the engine runs `RequestRenderOps` unchanged, appends included (all request-time
side effects stay bit-exact). At `viewBegin` (frame tap, before the sort) the owned ops are compacted out of the db's z / solid lists; an op without
a valid retained record simply stays and takes today's path (composition + F2b-1 scene draw). No late composition.

**OpRecord** keyed (ctx, desc, list, viewKey) = today's packet key minus the draw index, snapshotted from the last composition: draws[] (the
`CachedDraw` packet + matSlot, partSlot, illum info, the shader quad sun x lightmap-combi, scene pipeline), side-effect list (images, lightmap res,
stream 3), flags. Validity = stamp compares instead of `packetSameDesc`: Material version, MeshPart generation, object-table version, illum record
version + light-set epoch (sun bit per set per frame), `variantEpoch` (every setter of GI / G-buffer / pack / tint gates), target-format epoch,
`LightTable::registryGeneration()` (new) for `holdsAll`, a per-illum cached `ambientLit` bit (CPU compose of c15..c21 from the records), per-frame
polls: lightmap residency (res+0x90/+0x98), entity overlay flags (+0x28 & 0xe00000), illum pointer (+0xf8), texture-ready per material.
Owned in the first cut only: mesh ctx, static, main view z + solid, not instanced, fade 1, no overlay flags, no switch-element material, no video
image, no hierarchy node, **table lighting on** (one pass per sub-mesh; without it the pass count follows the per-frame dynamic light count).
Traps: alpha func / ref are not composed (device carry-over) and must be derived natively; observers (RT scene) need the draw: decide before F2c-3.

**Object table** (`GpuTable<ObjectRecord>` by scene-table slot, image c31..c35: c32.w fade, c33..c35 world rows from entity +0x60, base = rec - 31 x 16;
written in the scene-table drain on the matrix signal, fade from the visible list, `checkEntity` on check frames); VS window 1 (c12..c30) in the
view record (+ a `dbvp` c0..c3 variant per view for depth-bias materials). Instanced VS (c32 + 4i) stays on the old path.

**Side effects per visible record per frame** (from the lean path): image LRU touches per visible material, `BindLightmaps(res, 1)` per solid
resource, stream 3 for leftover engine draws, texture `lastUseFrame_`, draw stats. Check frames (`native.owned_check=N`, the lean frame kinds):
no compaction, the old path runs, the record-derived draws are built and compared (normalised fields), not submitted; any diff turns the group off.

| stage | content | gate | CPU (Full, est.) |
|---|---|---|---|
| F2c-0 | measure: owned-eligible share by reason, observer reads, invalidation rates, CTAB-live c12..c31 of the static VS, occurrence of hierarchy / instanced ops | numbers: done, 7.12 | - |
| F2c-1 | object table, VS window 1 / 2 served, `LightingImages::entryFor`, `LightTable::registryGeneration` (`native.owned_objects`) | split_verify 0, read-backs 0, checkEntity 0 | ~0 |
| F2c-2 | OpRecords + record-derived DrawRecords built and compared while composition runs; read-log signal audit (`native.owned_records`) | DIFFERING 0, audit disagreements 0 (done, 7.14: DIFFERING 0 in every run, 0 - 4 benign audit disagreements per run) | +0.05..0.1 (measured 0.37 - 0.58 ms with the compare, 0.15 - 0.25 ms the derivation) |
| F2c-3 | viewBegin compaction + `Device::replayOwned` at BeforeDepthPrepass / BeforeSolid, engine sort order, side effects, check frames | check frames 0, refcompare Full, long tour (placeholderBinds -> 0), both backends, validation | **-0.5..-0.7** |
| F2c-4 | persistent records, GPU compaction (ref.y = view slot), later cull (F5) | compaction = CPU list | -0.03..-0.05 CPU, GPU / low-end gain |

## 10. F4 design: SpeedTrees + tree billboards GPU-driven (2026-11-06, accepted by the orchestrator)

Same recipe as F2c (section 9): a retained per-(render buffer rb, type) record + a per-view per-tree snapshot (LOD fields tree +0x48 / +0x50 /
+0x58 / +0x5c / +0x60 / +0x64 / +0x6c, read once per tree per view at `viewBegin`: they are overwritten per db); tree ops compacted out of the z /
solid lists; the CPU writes per frame the instance rows (64 B {c32..c35} per instance, in draw order, VS window 2 base = rows - 31 x 16), one
DrawRecord per instance (template copy + vsWin2 + psWin3 = the instance's lighting image `entryFor(illum, params)`) and one 20 B indirect record per
LOD bucket (indexCount = strip / leaf count, firstIndex = start, firstInstance = the bucket's first row); multi-draw runs through the F2a scene
variant / F2b-1 `sceneFlushRun`. **Per-instance DrawRecords remove the illumination cut (88 % of today's solid tree batch cuts), the light-list /
static-SH / own-lights cuts and the 50-instance limit** (under table lighting; the remaining per-instance permutation input is the sun bit).
Remaining cuts: pipeline (type x z / solid / shadow x sun x material graph / states), the tree resource's vertex / index buffers, topology.
The SpeedTree VS reads its rows as `g_vInstanceData(vInstance * 4 ...)` (TEXCOORD4 = stream 1, ramp 0..n-1 [I]): stage 1 replaces stream 1 by our
own identity ramp (instance-rate, length >= row capacity; `firstInstance` offsets it), later the owned VS sets vInstance from ref.y. Wind c14..c29
(+ c30 fog) in a tree view block (VS window 1), once per view. Billboards: per-(ctx, desc) retained constants images (c32..c36 + c37+j), the easiest
stage. Side effects at compaction: queue stamp reset (rb +0xe0 / +0xe4) + sort (`leanTreeSort`), MaskReference 80 (`leanMaskRef`), material /
resource touches, billboard lightmaps, RtScene observer calls; keep composition for trees while RtScene's animated-tree path (captures) is on
(until F8 supplies the rows). Check frames compare rows / strips / alphas / pipeline exactly; lighting only where the engine's batch had one
illumination (it draws a merged batch with its last instance's lighting; ours is exact per instance: an accepted Full picture change).

| stage | content | gate | CPU (est.) |
|---|---|---|---|
| F4-0 | measure (`RVK_F4_MEASURE=1`): composeTree phase timers, census (trees / ops / buckets / calls under the new rule, two-bucket trees, no-entity share, sun values, illum classes, hw vs sw instancing), invalidation rates of the tree LOD fields / matrix / illum / rb tables / wind, CTAB of the tree VS / PS (c0..c31, PS c5), stream-1 freq / size / content, RtScene coupling | numbers | 0 |
| F4-1 | billboards from retained records | DIFFERING 0 | -0.15..-0.25 |
| F4-2 | tree records + per-view plans, compared | DIFFERING 0, audit 0 | +0.05..+0.1 |
| F4-3 | CPU-owned tree execution main view (compaction, rows, records, args, side effects, check frames) | check frames 0, refcompare Full, jungle spots, rtwalk sway, lod.trees=3, both backends, validation | -1.0..-1.4 |
| F4-4 | tree shadow views (cascades, z) | shadow-compare | -0.15..-0.3 |
| F4-5 | persistent per-tree records (UpdateLoD post-hook), GPU compaction | compaction = CPU list | -0.05..-0.15 + GPU |
| F4-6 | owned tree shaders (leaf PS wired: `uberSupportsFile`; vertex pulling; graph-shared pipelines) | equality gates | calls ~10-40 |
