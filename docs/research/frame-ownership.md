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

### 7.17 F4-1 billboards (2026-11-06, `native.owned_billboards`; records compared AND owned)

Debug setting (Bool, run time, default off, Full; `needs` native.owned_windows + native.lighting_images + native.scene_draws). Files: `src/native/bb_records.h` (record, stats, reasons),
`src/native/static_ops_bb.cpp` (StaticOps `bbEnter / bbComposed / bbDraw / bbEnd / bbReplay / bbReport`, the pattern of 7.14 copied, not shared: F2c-3 changes those functions),
`src/gpuscene/billboard_table.*` (persistent constants images), `src/d3d9/owned_submit.cpp` (`Device::submitOwned`, `ownedBindingsLive`), small additions: `IllumTable::pollDraw`,
`WindowTables::fillViews(.., vs0, ps0)` (view registers given by the caller), `VariantSlot::SceneVsLod`, `Device::setSceneBillboards`, test `rvk_d3d9test` "owned submit (F4-1)".

* **Scope and a deviation.** The main view's `eCRenderContextSpeedTreeBB` z / solid ops (eCMeshResource2::Render path with a cell index range; z = two special passes). **No viewBegin compaction:**
  an owned op stays in the engine's lists (order, sort, hooks untouched) and is replayed at its own slot instead of being composed (`opEnterImpl`: `if (ownedBb_ && bbReplay(op)) return;`,
  slot skipped through the lean commit). The CPU gain is the composition + issue, which is all compaction would remove for 13 - 21 ops; F2c-3's helper can replace the call later.
* **Record** (`bbrec::Record`, key ctx / desc / list, main view): `orec::Stamp` (billboard mesh = PS +0x30, sub-mesh range / material / shader class + flags, illumination + light set + sun bit
  for solid, lightmaps, variant epoch, view, material structure versions) + the **billboard source** (PS, its list pointer +0x40, range entry first / count) ; `draws[]` = F2c-2 `DrawRec`
  (own copy of the retained `CachedDraw`, buffers, textures, seven sampler states, alpha test / mask, matSlot, lighting key, register sets, tag fields; 2 draws per op in z and solid);
  per draw the **material poll's arguments** (`MatArgs`: owner, material, zPass, PS mask, blend, mask ref, the sampler inputs of the pool slots); the **constants image** (VS c32..c36 = the
  engine's fixed table 0xbfcbd0, c37 + j = the PS list entries: 198 registers in every area seen) with its **source read log** (3 hashed reads: table, range entry, list entries) and a slot in the
  persistent table (images interned by content: 1 - 3 distinct images; 3.2 KB each, uploaded once, no per-frame upload; window base = record - 31 x 16); the composition's full read log (signal
  audit); the side effects (the mesh's inline touch, the images the draws sample, stream 3 of the z op; BindLightmaps is re-run for solid).
* **Windows of a billboard draw** (found from the shaders' read extents, z VS reads c0..c3 / c11 and c32..c231, PS c5 only): VS 0 / PS 0 = a view block written from the view's mirror
  (`bbWindowsFor`: c0..c3 = vp or dbvp by the shader's depth-bias flag, c4..c11 + c12..c30 + PS c0..c4 from the mirror) with `fillViews(.., vs0, ps0)`; VS 1 = the same block's c12..c30; VS 2 = the
  constants table; VS 3 / PS 2 = Material records; PS 3 = the lighting image; PS 1 = the DrawBlock's c5.
* **Billboards as scene draws** (needed by both parts): `Device::setSceneBillboards` makes `SpeedTreeBillboard` opaque draws batchable (they were not: `meshKind` list) and eligible for
  `sceneDecide`; at the player defaults `lod.trees = 2` every billboard draw uses the fade vertex module (`lodModule`), so a scene form of it exists (`VariantSlot::SceneVsLod`: lod
  `inputOverrides` + `sceneRecord`; the first run found every draw refused with "variant gate" without it).
* **Compare** (every composed frame whose record is valid: check frames, 1 in 10 plus the frames the stamps broke): scene form, pipeline, vertex input / dynamic state, geometry (the cell index
  range), flags, tag, windows (VS 0 / PS 0 on the registers the draw composed: the others are the device's carry-over, found by the first run), VS 1 (c12..c30 live registers against the admin
  pool), VS 2 / constants (c32.. against the image), material / lighting windows, DrawBlock tex / smp / posFix / alpha / pad / c5.
* **Owned frame** (`bbReplay`, lean frames that are not check frames): stamps + constants sources + the composition's read log valid, the view (`viewAndMirror`, view stamp), every buffer /
  texture of the record still alive (`ownedBindingsLive`: a record outlives them), the **material poll** (`checkMaterial` with the recorded arguments and samplers: slot and structure version
  must be the build's), the illumination polled (`IllumTable::pollDraw`: same slot, bytes against the record once per frame) and its lights handed to the light table, the draw tag, then a
  **dry run of every draw** (`submitOwned(.., dryRun)`: split shaders, no observers, windows cover the read extents, `deriveScene`: modules, phase, variants, environment, bindings, textures)
  before the first is submitted (no partial op), then `submitOwned` per draw (DrawBlock derived, push block from the window offers, `toScenePacket`: the null-address guard applies) and the lean
  commit. Anything refused composes the op as before (counted by reason). `RVK_BB_CHECK=N` (`-Environment`): N = check period (default 10, 1 = never owned, 0 = no check frames).
* **GPU safety.** CPU guards as above; `rvk_d3d9test --validation` "owned submit" on **Vulkan and D3D12**: four quads submitted with junk in the register file, 0 bytes differ from the normal draws,
  one indirect run of 4 records, a dry run draws nothing, a short image / null base / the scene guard refuse and draw nothing, a changed image changes the picture; 0 validation errors. First game
  runs with `-Validation` + `VK_KHRONOS_VALIDATION_VALIDATE_GPU_BASED=GPU_BASED_GPU_ASSISTED` (jungle, 3 runs, the last with owned replays): no VUID, no device loss. `rvk_tests` 288 / 288 (settings count 129).
* **Findings that shaped the code:** the `lod.trees != 1` fade module (above); the material table's own verification caught the first version of the poll (sampler states of pool slots the PS does
  not sample were missing from the record: `MatArgs` keeps all stages checkMaterial reads); view windows differ in registers the shader never reads (negative zeros in the device's carry-over).

**Numbers** (Full, `-PlayerDefaults`, the 8 native settings of the task; logs `rvk-work/scratch/f41/`; "owned" = ops replayed without composition, the rest composed: check frames 1 in 10 + rebuilds):

| run | recordable ops composed (compared) | owned ops | share owned | DIFFERING | audit disagreements | owned replay CPU / frame |
|---|---|---|---|---|---|---|
| Vulkan 4 spots (jungle, lake_jungle, waterfall_jungle, swamp_camp) | 25.1 k | 214 k | 89 % | **0** | 2 (+33 caught by the post stamps) | 0.26 - 0.30 ms (the busiest spot) |
| savegame Risen_Save_0005 hold (lod 2x) | 7.3 k | 51.7 k | 88 % | **0** | 2 (+33) | 0.11 ms |
| savegame `lod.trees=3` | 9.2 k | 59.2 k | 87 % | **0** | 2 (+33) | 0.10 ms |
| D3D12 (jungle, lake_jungle) | 17.5 k | 128.6 k | 88 % | **0** | 0 (+66) | 0.25 ms |

No `[error]`, no device loss, no validation message in any run. Refusals: only "check frame". The audit's 2 disagreements are the pointer-streaming kind of 7.14 (`other`).

**Pictures** (jungle spot frozen, `native.owned_billboards` toggled at run time every 200 presents, a shot every 50, pairs inside the hold chosen afterwards): control pairs (same state) 0.004 - 0.05 mean
abs; on vs off **0.22 mean abs, max 19, ~1,200 pixels above 8, all in one band of the horizon rows (y 113 - 136, soft sky / haze)**. The same toggle with owned replays disabled (`RVK_BB_CHECK=1`: the
scene-draw path only) shows the same size of difference (0.3 - 0.6, max 28): **the change comes from the billboards becoming batched scene draws** (their opaque draws are grouped by pipeline within a flush, the
engine's order of same-depth surfaces changes), not from the owned replay; accepted as a Full-mode picture change like the batching of 7.11. Owned frames vs composed frames differ by nothing the compare
sees (DIFFERING 0 on every check frame).

**CPU A/B** (savegame, `-Perf`, `RisenTest.exe`, vsync off, lod 2x, shared default copy under `with-test-lock -Who f41-perf`, interleaved off / on / off / on, mean of the last 6 windows): engine + HAL
CPU per frame **off 6.72 / 6.71 ms, on 7.03 / 6.97 ms (+0.29 ms: no gain, a loss)**; GPU 3.36 / 3.37 vs 3.27 / 3.27 ms (-0.10 ms: fewer, batched calls). The owned replay itself cost 0.36 ms per frame in
those runs (the first build) against the 0.25 - 0.55 ms of compose + issue it replaces: **the owned replay is not cheaper than the composition yet.** After the poll was made once per material per
frame and the windows of the dry run were reused (this commit's last change; the 4 spots / save runs above predate it), the savegame hold measured 107 -> **86 us per frame** for the replay with DIFFERING 0
(not re-timed A/B: the shared copy was busy with another agent's run). What is left is per-op fixed work: stamps (~40 guarded reads), the source / read-log validation (3 KB hash of the constants), `viewAndMirror`
(`initMirror`), `fillViews` + `fillWindows` per draw, two derivations per draw (dry run + submit). The honest reading: billboards are too few ops (13 - 21 per list) for the record path to win on CPU
by itself; its value is the proven path (record -> derive -> submit with nothing composed, DIFFERING 0, both backends) that F4-2..4 reuse for the 77 tree ops / 129 draws of a view, where per-op costs amortise.

**Not done / handed on:** viewBegin compaction (slot-time replay instead); the shadow views and the trees themselves (F4-2..4); the per-frame cost of the replay is dominated by the material
poll (`constantsInto` + `checkDraw` per draw: polling once per material per frame is the next saving) and `initMirror` per op; check-frame period / env knob should become `native.owned_check` when F2c-3 lands it.

### 7.18 F2c-3 + F4-1 integration (2026-11-06)

Both branches merged (F2c-3 first). `native.owned_check` is the check period of both (F4-1's `bbCheckFrame`; `RVK_BB_CHECK=<n>` still overrides). `Device::replayOwned` (register-file based, observers see the loaded windows) and `Device::submitOwned` (no register file, windows from the persistent images) were NOT merged: their guards differ by design. Game check (jungle + pub_interior, Full, all owned settings, Vulkan with GPU-assisted validation / Vulkan / D3D12): 0 validation errors, owned static 63-67 % of ops with 0 undrawn and check frames DIFFERING 0, billboards 100 % recordable and owned between check frames, DIFFERING 0 (records + billboards, total and 5 s).

### 7.19 F2c-4-0 ABI v2 + measurements (2026-11-06, `TranslateOptions::sceneView`, HLSL `RVK_SCENE_VIEW`, `RVK_F2C4_MEASURE=1`)

Stage F2c-4-0 of section 11: the per-frame inputs leave the DrawRecord, the F2c-3 owned path (`replayOwned`) draws on the new ABI, and the seven open questions of section 11 are measured. No CPU gain was expected (and there is none: v1 and v2 owned
paths cost the same, see below); F2c-4a builds on it. F2b-1's composed scene draws and the F4-1 billboards stay on v1 (`kVariantSlots` 15 -> 21: the v2 forms are `SceneVs2 .. SceneGbufferA2` = v1 slot + 7; no v2 billboard form).

* **ABI v2** (`src/shader/abi.h`, twin `shaders/include/rvk_draw.hlsli`; offsets checked by `rvk_tests abi_hlsl_sceneview_layout_matches_cpp` + the changed `abi_hlsl_scene_layout_matches_cpp`):
  `ViewBlock2` (640 B, `kViewBlock2Size`): VS c0..c11 @0, PS c0..c4 @192, VS c12..c30 @272, `posFix` @576 (1/w + jitter.x, -1/h + jitter.y, depth remap z / w: exactly the DrawBlock's), PS c5.xyz @592, flags @608, reserved @624.
  Window bases are `block + offset - first register x 16`: VS 0 = block + 0, VS 1 = block + 80, PS 0 = block + 192 (all offsets positive: no table pad needed on D3D12). `ScenePush::views` @40 (the view table base; spare0 renamed),
  `ref.y` = view slot, `VARY14` = `(slot << 24) | record index` (`kViewSlotShift`, `kSceneRecordMask`), `DrawRecord::c5w` @104 = the bits of PS c5.w (the draw's mask reference; section 11 only named "mask bits": the field was added).
  In a v2 shader `rvkVsWin(0 / 1)`, `rvkPsWin(0)` come from `pc.views + ref.y * 640 + offset`; `RVK_PSC(5)` folds to `rvkSceneC5()` (view c5.xyz + the record's c5.w; a register-RELATIVE pixel read of c5 through the window sees xyz only, w = 0:
  no shader of the corpus does that); `rvkLoadDraw()` replaces the DrawBlock's `posFix` / `psC5` by the view's. `TranslateOptions::sceneView` implies `sceneRecord`; `uberPixelHlsl(.., sceneRecord, sceneView)` / `-D RVK_SCENE_VIEW=1`;
  `rvk_shadertool corpus|dxil-check corpus --scene2`, `show <f> --scene2`. **Default (non-scene) text byte-identical (12 random corpus shaders against the main build: default and `--scene` text equal); `kTranslatorVersion` NOT bumped.**
  Slots (`kViewSlot*`): 0 main z, 1 main solid, 2 - 3 depth-bias view-projection, 4 - 7 cascades (reserved), 8.. dynamic.
* **View table** (`src/gpuscene/view_table.*`, `gpuscene::viewtable()`, active exactly while `native.owned_static` is): a `GpuTable<ViewBlock2>` (64 slots, 40 KiB, uploaded at the present by `flush`, only the slots whose bytes changed), `set(slot, regs)` once per view and
  frame; a second `set` of a slot with other bytes in the same frame is refused (the GPU sees one content per frame; counted as `conflicts`, 0 so far); `acquire` = dynamic slots (searched by content per frame); `fillWindows(slot, OwnedWindows)` builds the VS 0 / 1 + PS 0
  entries (`OwnedWindows::viewSlot`) that the caller's per-draw windows carry, so the F1a / F2c-1 served-window checks and `debug.split_verify` work unchanged. `ownedPass` writes slot 0 (z pass) / 1 (solid pass) from the mirror's registers + `Device::viewPosFix()`;
  without the table (or a refused slot) the pass falls back to the v1 view block of the window tables. rvk.log `native: view table (5 s)`; `RVK_OWNED_V1=1` (`-Environment`) forces v1 for A/B.
* **Device** (`scene_draws.cpp`): `sceneDecide` also resolves the v2 pipeline of a static mesh draw while `setSceneViews(true)` (`CachedDraw::scenePipeline2`; a v2 form still being built = `scenePending`, the v1 form draws the draw meanwhile);
  `ensureSceneRefV2(slot)` = one ref stream `(i, slot)` per view slot (16 ring blocks of 8 KiB per frame at most; a larger slot is refused), so a run never mixes slots (the vertex input differs); `toScenePacket(.., viewSlot, c5w)` zeroes the record's four view window bases
  and stores c5w; `sceneFlushRun` puts `ScenePush::views` in the run's push block and drops (error) a run that mixes ABI forms. `replayOwned` with `OwnedWindows::viewSlot` set: guards 16 (no v2 pipeline / no table), 17 (slot >= table size or >= 16),
  18 (the windows are not the slot's bases), 19 (the slot's posFix / c5.xyz differ from the draw's derived ones: a jitter / viewport / pass change after the slot was written), 15 (the scene packet's null-address guard also checks `views`).
* **Gates.** `rvk_tests` 289 / 289. `rvk_shadertool corpus corpus --scene2` 2483 / 2483 (also `--scene` 2483 / 2483), `dxil-check corpus corpus --scene2` 2483 / 2483 (push block 112 B; `--scene` 2483 / 2483), `dxil-check embedded` 77 (2 new `material scene view` jobs + the layout test shader),
  `dxil-check uber` gained the variants `plain+sceneview`, `gbuffer+motion+sceneview` (no `out/matsrc` here: not run). `rvk_rhitest --validation` (`RVK_PARITY_ONLY='scene variant'`): each v1 case again as ABI v2 (asm pair, three ubershader requests, rigid + skinned corpus pairs) on Vulkan and D3D12:
  0 pixels differ from the per-draw push path, Vulkan vs D3D12 worst difference 0 - 1, **the wrong-view-slot mutation (every draw pointing at the other slot) differs on both backends**; 0 validation errors. (A first run of the new cases passed `views = 0` to v2 shaders by mistake: the GPU
  faulted, VK_ERROR_DEVICE_LOST in rhitest; the harness was fixed, the device's guards 15 / 16 refuse such a draw on the CPU side.) `rvk_d3d9test --validation` both backends, new case "owned replay v2 (view table)": 8 owned draws = ONE indirect call over 8 records, 0 bytes differ from the normal draws;
  two view slots = 2 calls, the reference picture; a stale posFix refused (19) and drawn again once the slot is rewritten; windows of another slot (18), slot out of range (17), null-address guard (15), null table (16): nothing drawn, v2 draws again afterwards; 0 validation errors.
* **Game runs** (Full, `-PlayerDefaults`, owned chain on, logs `rvk-work/scratch/f2c40/`): the first run pub_interior 60 s with `-Validation` + `VK_KHRONOS_VALIDATION_VALIDATE_GPU_BASED=GPU_BASED_GPU_ASSISTED`: PASS, 0 VUID, no device loss. Then, all with check frames DIFFERING 0, no error line:
  rvkcompare harbour_docks + jungle + pub_interior (Vulkan), the savegame hold (Risen_Save_0005, lod 2x, `rvk_perfsave hold=40`, RisenTest.exe), pub_interior + harbour_docks with `-Api d3d12`, `upscale.mode=jitter_only` + `upscale.object_motion=off` (jitter on: posFix.xy changes EVERY frame, 0 refusals "view block differs"),
  and picture A/B toggling `native.owned_static` inside one hold (7.11 method, 300-frame periods, a shot per 50 presents): pub_interior toggle pairs 0.42 (on -> off) / 0.86 (off -> on) mean abs vs same-state pairs 0.19 - 1.3; harbour_docks 0.46 / 1.35 / 0.53 vs 0.3 - 2.7 (sea);
  jungle 0.58 / 0.74 / 0.57 vs 0.04 - 0.8 (wind: 7.5 in the moving windows): inside the controls' range. **`upscale.mode=native_aa` turns the owned path off** (the G-buffer / object-motion observers read more than material constants: `observers ok false`, 0 compacted views), as in 7.16;
  jitter on the owned path is therefore proven with `jitter_only`.
* **Per-piece CPU of the owned path** (savegame hold, `-Perf`, RisenTest.exe, vsync off, measure OFF, interleaved v1 / v2 / v1 / v2; mean of the last six 5 s windows, per main view, ~380 owned ops / 400 draws at this position): v1 (`RVK_OWNED_V1=1`): compaction 215 / 209 us (stamps + record checks 123 / 118),
  view + windows 2, material 36 / 33, build 202 / 183, side effects 28 / 23, submit 586 / 553 = **1069 / 1003 us**; v2: compaction 215 / 223 (123 / 129), view 3, material 35 / 36, build 202 / 203, side effects 28 / 25, submit 582 / 590 = **1065 / 1080 us**; engine + HAL CPU per frame 10.27 / 9.52 (v1) vs 10.11 / 10.57 (v2) ms,
  GPU 2.4 - 3.7 ms (both arms alike): **v2 is CPU-neutral, as planned for this stage** (2.6 - 2.7 us per owned draw at this position; 7.16 measured 3.4 us at a smaller one). The view slot is written once per pass (3 us) instead of a view-block lookup + record fill (2 us); the saving only appears with F2c-4a's persistent records.

#### The seven measurements (RVK_F2C4_MEASURE=1: `native: f2c4 measure (5 s)`; counters in `src/core/f2c4_measure.*`, one bool test per site when off)

1. **Main views per frame: exactly 1** in all 11,500 frames of the runs (histogram 0 / 1 / 2 / 3 / 4+ = 0 / all / 0 / 0 / 0; one z pass and one solid pass per view); a view block differing from the same pass's earlier block of the frame: 0, `conflicts` of the table 0.
   So slots 0 / 1 are enough; the multi-view case (cascades, dbvp) was not seen in the owned lists (they are main-view only here).
2. **Does the view block change per frame?** Compared with the same pass's block of the previous frame (9,217 comparisons at player defaults): **c12..c30 (the admin pool's view-wide registers) changed in 97 %** (every frame at a fixed camera too: the block is rewritten each frame; time-dependent admin values, which register was not
   identified), PS c0..c4 72 % (fog / sky colours), VS c0..c11 9 % (only with camera movement), **PS c5.xyz 8 %** (rare), **posFix 0 %: no jitter at the player defaults**, posFix.zw (depth remap) never. With jitter (`jitter_only`, 2,419 comparisons) posFix.xy changes in 100 %; c12..c30 still 98 %.
   Consequence for 4a: a slot is rewritten every frame anyway (640 B, 1.3 slots / frame uploaded), jitter adds nothing; c5 / posFix can live in the view block as designed, and "per-frame epoch of the global inputs" = this one block.
3. **Where `submit` goes** (per `replayOwned` call, measure on, ns; pub_interior / harbour / jungle windows): prep (scope, shaders, phase) 110 - 210, **deriveScene 550 - 830**, **observers 550 - 810** (the RT scene's `observe`: the only registered observer at the defaults), windows (`uploadConstants` + served check) 80 - 100,
   packet (DrawBlock ring alloc, push, `toScenePacket`, `submitInterned`) 140 - 190: **derive 28 - 36 %, observers 29 - 40 %, windows 4 - 5 %, packet 7 - 9 %, prep 8 - 10 %** (the timers themselves add ~0.2 us per call: submit 760 vs 585 us per view at the savegame with them off). F2c-4a removes derive
   (persistent records), F2c-4b the observer loop (`observeOwned` in bulk).
4. **Does `desc` vary with the LOD resource?** `desc` is the sub-mesh / node code, not a pointer. Over 2.5 M context-frame pairs (ctx + 0x28 = the LOD resource): the resource changed in only 27 pairs (LOD switches are rare); in 16 of them the context's desc SET also changed, in 11 it did not;
   the desc set changed with the SAME resource in 407 pairs (ops entering / leaving the list). So the record key (context, desc, list) is stable across LOD switches of one-sub-mesh meshes, but a LOD switch can change the set and always changes the stamp's `LOD resource` (record invalid, rebuilt): the key must stay, the validity comes from the dep.
5. **`RequestData(res, 1)` idempotent within a frame? Yes for the state, no for the LRU order** (Engine.dll, test copy; `eCResourceCache2::RequestData(res, mode)` RVA 0x687ad0, `eCResource2::RequestData(mode)` 0x684100: both read the state dword at res + 0x38 and dispatch):
   state <= 0 or > 3: nothing; state 3 (resident): **0x6843e0 = LRU touch**: if `res` is already the list head (`cache + 0x18`) it only re-stamps `res + 0x20 = cache + 0x28` (the cache's epoch counter: the same value for the whole frame); otherwise it unlinks `res` (prev / next at +0x10 / +0x18)
   and relinks it at the head, then stamps. States 1 / 2 (not loaded / loading): **0x6852d0** (mode 1 = load): skips when the state is already 2 (loading); else sets state 2, adds `res + 0x28` to the cache's pending budget (`cache + 0x34`) and queues the load.
   So a second call in the same frame changes nothing observable except the position of `res` in the LRU list (and its stamp is already equal): it is safe to touch once per resource per frame (F2c-4b), provided the LRU order among the frame's touched resources need not equal the draw order (the eviction picks the tail: only resources untouched for a frame can reach it).
6. **Buckets** (a bucket = v2 scene pipeline + vertex input + dynamic state by content + index binding; `f2c4BucketKey`): live records / draws -> distinct buckets: harbour_docks 459 / 474 -> 161, jungle 1,053 / 1,096 -> 313, pub_interior 1,083 / 1,127 -> 333, savegame 888 / 933 -> 218 (about 3.5 draws per bucket).
   Buckets hit per frame (average / max): harbour 41 - 102 / 132, jungle 100 - 154 / 193, pub_interior 24 - 28 / 35, savegame 92 / 115 (so 4c needs well under 256 bucket ids and a hit list of ~100 per frame: the per-bucket count rewrite is small). Max owned draws in one pass: z 49 (pub) - 476 (savegame) / solid 10 - 68;
   ops z 47 - 475 / solid 10 - 68: the id list per (view, list) fits 1,024 with a margin; the largest observed live set is 1,127 draws (the 64K table is oversized by 50x).
7. **Compaction split** (per `compactOwned` call, z or solid list; measure on): select loop (vtable read + `ownedSelect`: stamps, record checks) **82 - 87 %**, rewrite of the engine's list (kept ops + 2 writes) **1 %**, stable sort of the owned ops with the engine's comparator 5 - 11 %, `resolve` of the owned ops 5 - 8 %.
   pub_interior 13 us per call (select 11.1, sort 0.7, resolve 1.0, rewrite 0.2), savegame 163 us (select 134, sort 18, resolve 10, rewrite 1). The comparator sort and the list rewrite together are only 6 - 12 % of the compaction (about 7 us of 62 at 7.16's position, ~20 us here):
   the cost is the per-op select (stamps), which F2c-4a's bulk validity removes; the GPU compaction of 4c would take over the sort (stable by the engine's order) and the rewrite.

### 7.20 F8-0 persistent RT entries (2026-11-06, `rt.table_entries` + `debug.rt_table_compare`; Full only, Debug, default off)

Files: `src/enh/rt_scene.{h,cpp}` (`TableEntry`, `coversDraw`, `tableObserved`, `tableMaintain`, `logTable`), `rt_registry.{h,cpp}` (`Instance::pinned`), `rt_material.{h,cpp}` (`signature` from its parts),
`d3d9/device.h` (`CoverQuery`, `DrawObserver::coversDraw`) + one-line pre-filter calls in the three observer loops (`prepareDraw`, `replayDraw`, `replayOwned`), settings (133 now), tests `rt_registry_pinned_never_stale`.

* **Design as built (differs from section 12 in one point: no second TLAS path).** The registry stays the one owner of TLAS instances, so `record()` / `emitInstance` are untouched and duplicates are impossible by
  construction (the key-equality of the registry + owner replacement stay the guard; no `tableOwners_`). A **TableEntry** (key = hash(entity, opDesc, first, primitives)) holds: scene-table slot (index, generation,
  version), the draw's `BlasKey`, the BLAS id and registry instance key, rows, class mask (1 main solid, 2 main z, 4 cascades: the classes fully observed since the learn), per-class material signature
  (`RtMaterials::signature`: engine VS / PS hashes, tag material, sampled textures s9..s15). It is learned at the end of the full observation of a plain draw (kind StaticMesh, not instanced / blended / water / glass,
  not SpeedTree) when the slot exists and **its rows equal the entity's own matrix** (`rows[r*4+j] = world[4j+r]`, 1e-5: a hierarchy node or carried matrix is refused; measured refusals 0). Learning **pins** the registry
  instance (`pinned`: never aged for being unseen; the radius still drops it; counts as seen for the volatile-range rule).
* **Pre-filter** `coversDraw(CoverQuery)` (kind, phase, tag entity / opDesc, buffers, offsets, range, blending, engine shaders, textures; no query is built): true = skip query building + `observe`. Falls through (counted by
  reason): no entry, class not yet observed, slot gone / reallocated / **moved** (slot version), buffer id / generation / offset / range differs (LOD switch, new GPU copy), material inputs differ (shader / streamed-in texture),
  **relearn** (every 32nd frame per *instance*: all entries over one instance are observed in the same frame, so the frame's last draw still decides its material), check frame. Per frame `tableMaintain` drops entries whose slot
  changed (the instance unpins and ages as before); a registry removal of a pinned instance (distance, BLAS died, owner replaced, cap) erases its entries (`onRemoved`); entries of the same (entity, op) on another vertex /
  index buffer are erased when a new one is learned (LOD switch). Off = `coversDraw` returns at once; nothing else exists.
* **Compare** (`debug.rt_table_compare`, check frame every 10th, pre-filter off for it): each eligible full observation is compared with what the covered draw would have skipped: BLAS key / instance key, rows (1e-5, also vs the
  slot), flags, data word (low 16 bits: alpha test; the kind in the high bits is the latest sighting's, diagnostics only), view bit, and the instance's final material of the frame against its pre-state (content, ignoring the
  animated uv map). Hard differences (key / rows / flags / data) or material differences above 0.5 % of the compared pairs in a 5 s window turn the entries off (`rt: scene table: turned off`; `RVK_TABLE_NOTRIP=1` keeps
  counting). `missing` = an old instance of an eligible draw without an entry, `extra` = entries not drawn on the check frame (per check frame). rvk.log `rt: scene table (5 s)`.
* **Found while building it (each cost a compare round):** (1) several entries share one instance (z range = cascade range): the instance is pinned until the last entry goes; (2) the instance's material is "the last rank-4
  draw", so an entry relearning alone flips it: the relearn phase is per instance; (3) material signature per class (z / solid / cascade shaders differ: one signature ping-ponged); (4) a streamed-in texture
  (placeholder -> real) changes the bound `Texture*`: part of the pre-filter, else the old material lives up to 32 frames; (5) the draw kind (`data >> 16`) flips between a plain and an instanced sighting of one instance: not compared.
* **Coverage (Vulkan, rvkcompare lake_jungle / harbour_docks / jungle / pub_interior, compare on; per frame):** ~676 eligible candidates asked, 603 covered (89 %: 10 % are the check frames, relearn 1.2 %, material inputs ~0,
  no entry ~1 %), full observations of the class 56 (all draws of the RT scene observed: 600 -> ~350 per frame at the savegame). Savegame without compare: 200 of 221 covered (90 %), refused learns 0. Registry composition
  (jungle): 2778 instances, 1111 pinned (main-only 1686 / cascade-only 562 / both 530 of all: **cascade-only share 20 - 30 %**); the rest are instanced batches, SpeedTrees, grass and other excluded classes.
* **Compare results (pairs compared / key / rows / flags / data / material / missing):** Vulkan 4 spots 916,678 / 0 / 0 / 0 / 0 / 12 / 1; depth-probe run 865,709 / 0 / 0 / 0 / 0 / 47 / 3; instances-probe run 812,566 / 0 / 0 / 0 / 0 / 54 / 5; savegame
  344,060 / all 0; teleport soak (10 spots) 850,351 / 0 / 0 / 0 / 0 / 30 / 8; D3D12 873,896 / 0 / 0 / 0 / 0 / 56 / 13. The material differences (1 in 15,000 - 70,000 pairs, same texture name, other sampler / untextured <->
  textured variants) are instances several unlike draws land on in a frame (instanced + plain, or two passes with different materials on one range): the covered path keeps the last learned state. They are below the 0.5 %
  trip rate and only shift which of those materials a hit shows; not chased further in F8-0.
* **Probes** (`rt.debug=depth`, 4 spots, 25 probes x ~70 frames): table off 1138 match / 201 farther / 200 closer / 0 missed / 0 sky; on 1079 / 214 / 175 / 0 / 0 (the traced pixels are a sample: no new red or magenta); `rt.debug=instances` on:
  1140 / 235 / 156 / 0 / 2 sky (no off arm).
* **GPU safety:** first run pub_interior 120 s Vulkan `-Validation` + GPU-assisted validation with shadows.rt near + gi + compare: no validation message, no device loss (twice, before and after the signature changes); teleport soak with
  `debug.crash_markers` (10 spots, 600 s): no loss (the markers warned "debug.regions is off": the soak ran without regions); D3D12 420 s: no loss, no `[error]`; every run's only `[error]` is the existing `native: mesh parts: fetch
  DIFFERING ... uv (-nan)` NaN compare of the F0 self-test.
* **CPU (savegame Risen_Save_0005, lod 2x, shared copy + lock, `-Perf`, 2 runs per arm, no compare):** RT scene observe 0.289 / 0.302 -> 0.207 / 0.208 ms per frame (-0.09 ms; observed draws 550 -> 352), the pre-filter costs 0.02 ms (perf-only
  timers), **but the RT pass (`rt.scene` CPU) grows 0.378 / 0.398 -> 0.462 / 0.463 ms** because off-screen entries stay (TLAS 725 -> 906 instances, BLAS 346 -> 389, AS +2.6 MiB): engine + HAL CPU 7.71 / 7.26 -> 7.25 / 7.33 ms = **no net
  gain (within noise)**; the estimate of -0.15 .. -0.25 ms did not hold: the observe cost was 0.3 ms to begin with and the remaining 352 observed draws per frame are the instanced / SpeedTree / skinned / excluded classes.
  F8-1 (materials from records) was not started: F8-0 is not clean (rare material differences) and its own gain was estimated at -0.02 .. -0.05 ms.
* **Next:** the saving needs the other classes (F4 trees / instanced batches, F6 characters) and the TLAS assembly per instance (instances cut by a radius that matches the consumers instead of the 300 m registry radius, or the
  GPU assembly F8-4) before the entries pay. Kept as a Debug option, default off.

### 7.21 F2c-4a persistent records (2026-11-06, `native.owned_persistent`)

Stage F2c-4a of section 11: the owned static draws of 7.16 / 7.19 no longer have a per-draw derivation, DrawBlock or packet per frame. Debug setting (Bool, run time, default off, Full; `needs` native.owned_static; the persistent path additionally needs the scene ABI v2 view table, so it is off with `RVK_OWNED_V1=1`). Files: `src/gpuscene/draw_record_table.*` (the two tables + the slot allocator), `src/d3d9/owned_persist.cpp` (the device's half: `Device::ownedEnvEpoch / ownedDrawSig / ownedBuild / ownedDerive / ownedObserve / ownedReserve / ownedFlush`), `src/d3d9/scene_hash.h` (the vertex-input / dynamic-state hashes now shared), `native/static_ops_owned.cpp` (`ownedDrawOpPersistent`, `ownedMissCheck`, the pass loop, the 5 s lines), `native/op_records.h` (`DrawRec::persist`), `static_ops_records.cpp` (the check-frame hook in `recDraw`), `tools/d3d9test` case "owned persistent (F2c-4a)".

* **Tables.** `gpuscene::drawrecords()`: `GpuTable<DrawRecord>` (128 B) + `GpuTable<DrawBlock>` (208 B) x 4,096 slots (512 KiB + 832 KiB; the largest live set seen is 1,076 records / 1,127 draws, measurement 6) + a persistent 4 KiB zero block (the windows no shader reads point at it: the per-frame ring zero block of `splitStage` cannot be a persistent record's address). A slot = one owned draw (`orec::DrawRec::persist.handle`, released by the draw record's destructor, so an op record that is rebuilt or dropped gives its slots back); `alloc` zeroes + uploads the slot, **`release` defers the reuse by `kFramesInFlight + 1` presents** (tested), a full table is `alloc = kNoSlot`, counted (`table full`), and the op is composed the old way. Uploaded at the present with the other tables (`drawrecords().flush`, the GpuTable recipe: only changed slots, read-back check every 240 frames). The DrawBlock keeps `tex[]`, `smp[]`, alpha test, pack word; its posFix / psC5 bytes are zero (a v2 shader reads the view block's); the record holds the VS 2 / 3 and PS 2 / 3 window bases (object, material, lighting; the view's four are zero), the block address, the flags and the c5.w bits (`drawId` 0: nothing reads it).
* **Validity (deviation from section 11: memoised epochs instead of reverse lists).** A draw's slot is reused while `(env epoch, signature)` equal the ones it was derived at. `Device::ownedEnvEpoch(solid, viewSlot)` hashes everything `deriveScene` / `replayOwned` read from the DEVICE (phase, `variantEpoch`, GI / pack / G-buffer state, LOD bias / dither / thicken / render ratio, viewport, scissor, targets + formats, depth reversal, jitter, depth remap, the sampler states the derivation does not take from the record (the seven it owns are overwritten per draw and left out), the pack stamp, the view slot + table) once per pass; the epoch moves when the hash differs from the pass kind's last. `ownedDrawSig` hashes the per-draw inputs: each sampled texture's bindless slot + ready state (the same test `deriveScene` applies), each stream's / the index buffer's `Buffer::gpu()` binding, the GI variant module the draw was offered, the ambient test, `replayAllowed`, the pack word, the four window bases + extents. The CPU-side reads of the engine (the op record's stamps: LOD resource, entity, geometry / material hash, illumination, lightmaps, overlay flags, fade, switch, object slot + generation, view; the material structure versions) stay what `ownedSelect` / `ownedMaterialsOk` do per op (7.16): **the per-op stamp is still ~half of the compaction** (142 us of 281 per compacted view at the 509-draw savegame view, measure timers on) and a polled dep table with reverse lists was not built: a record's deps are only polled while its op is visible, memoised per frame where several records share them (the material poll, the per-pass material / lighting window and ambient memos); a dep table pays only when the stamp itself moves to signals (F2c-5). A record that was judged valid but whose derivation inputs moved is simply derived again and rewritten (`derived` / `rewrote the slot` in the 5 s line).
* **CPU-written arguments.** `ownedFlush`: the pass's items (record slot, bucket key, retained draw) are put in buckets (v2 scene pipeline + vertex input + dynamic state by content + index binding + view slot, first-seen order, items keep the engine's order inside a bucket; 1 item in 8 verified exactly against the bucket's first), a ring block of ref entries `(slot, view slot)` + one of `VkDrawIndexedIndirectCommand`s is written contiguously per bucket (`firstInstance` = the entry's position, so the existing instance-rate ref stream binding at the list's offset serves it, `ScenePush.records` = the persistent record table, `views` = the view table), and ONE `DrawIndexedIndirect` packet per bucket is queued (`internSceneViAt`: the scene vertex input with the ref stream at the list's buffer / offset). The blocks are reserved before the pass (`ownedReserve`, bound = the draws of every op record of the pass); a refusal (ring full / any guard) queues nothing and the contributing ops are composed instead. An op composed late flushes the pending items first (its composition rebuilds / drops op records the items point into, and its texture transition lets the next ops' derivations succeed: an early version that deferred the late ops to the pass's end refused 12 - 15 % of the draws instead of < 0.3 %); the next flush gets a block of its own.
* **GPU safety.** CPU guards: `ownedFlush` refuses an item whose slot is >= the table, whose record was never written (no block address / window bases in the CPU twin) or whose draw has no v2 pipeline, a null records / frame block / views base, more items than the reserved block, a view slot out of range; `ownedBuild` refuses a null block / zero-block address, a window the shaders read that the caller does not serve (14) and a null window base (15); `ownedDerive` keeps replayOwned's guards 10 / 11 / 16 - 19. The d3d9test case runs through exactly these functions.
* **Checks.** (1) **MISS** = a draw whose slot the owned path would reuse (env epoch unchanged, signature equal, replay allowed) but whose content differs from a fresh derivation. On every check frame (`native.owned_check`, the old path composes) `recDraw` hands its derivation to `ownedMissCheck`, which builds the windows afresh (no per-pass memo), the signature and `ownedBuild` and compares the slot's record + block byte for byte (`check frames: N compared, M skipped`: skipped = the owned path would derive the draw anyway); (2) a **rolling scrub** re-derives 8 draws per pass of >= 128 draws (1 in 16 at the least: the stride is `max(16, draws / 8)`, the phase rolls) inside the owned frames and compares the same way; any MISS rewrites the slot and, with a DIFFERING, turns owned_static off (`native: owned static turned off: N MISS`); (3) the check frames' F2c-2 compare (derivation == composition, DIFFERING) is unchanged, so slot == derivation == composition; (4) **fault injection**: `-Environment 'RVK_F2C4A_INJECT=<n>'` makes every n-th detected change of an epoch / signature go unnoticed (the slot keeps its old content and looks up to date).
* **Gates.** `rvk_tests` 289 / 289 (settings count 132). `rvk_d3d9test --validation` PASS on Vulkan and D3D12 (0 failures, 0 validation errors), new case "owned persistent (F2c-4a)" through exactly the device functions above (`ownedDerive` -> `DrawRecordTable::write` -> `ownedFlush` -> the table's flush in the frame's upload list): eight persistent draws on one view slot = ONE indirect call, 0 bytes differ from the normal draws; the next frame submits the same slots with nothing derived and nothing written (derivation counter and table writes unchanged); a slot rewritten (quad 3's alpha test never passes: the picture equals the seven-quad reference, then rewritten back: the eight-quad one); a released slot is not handed out again for `kFramesInFlight` presents after its release and comes back after the third (the reused slot draws); two view slots = two buckets = two calls = the reference's second picture; refusals draw nothing and leave nothing behind: a window of another slot (18), a null block address (15), a view slot out of range (17), more items than the reserved block, a null records table, a view slot out of range in the flush, an out-of-range / never-written slot; the same draws work right after them. (A first version leaked its three table buffers past the device: `DrawRecordTable::destroy()`, the test calls it.) `rvk_rhitest --validation` PASS. `rvk_shadertool corpus corpus --scene2` 2,483 / 2,483 and `dxil-check corpus corpus --scene2` 2,483 / 2,483 (no shader or ABI change in this stage; run as the gate). The first game run of the final code: pub_interior, Vulkan, `-Validation` + `VK_KHRONOS_VALIDATION_VALIDATE_GPU_BASED=GPU_BASED_GPU_ASSISTED`, 150 s: PASS, 0 VUID, 0 error / warning lines, no device loss, MISS 0, DIFFERING 0 (an earlier build of this stage: the same, 60 s).
* **Game runs** (Full, `-PlayerDefaults`, owned chain + `native.owned_persistent`, `RVK_F2C4_MEASURE=1`, private copy `risen-test-vk-f2c4a` instance 26, logs `rvk-work/scratch/f2c4a/f_*.log`; all `[error]`-free and PASS except the injection run; sums over the 5 s windows):

| run | passes / draws | reused | derived (rewrote the slot) | draws composed late | scrub (MISS) | check frames compared (skipped) MISS | DIFFERING | slots peak | draws per bucket |
|---|---|---|---|---|---|---|---|---|---|
| Vulkan 3 spots (harbour_docks, jungle, pub_interior) | 7.3 k / 902 k | 96.2 % | 30.8 k (1,137) | 0.45 % (texture: the slot-0 render target of 7.16) | 29.2 k (0) | 222.7 k (944) **0** | **0** | 1,077 | 2.5 |
| savegame Risen_Save_0005 hold 60 s (lod 2x, RisenTest.exe not used here) | 8.3 k / 2.05 M | 97.7 % | 42.5 k (1,055) | 0.23 % | 41.4 k (0) | 511.4 k (0) **0** | **0** | 926 | 4.4 |
| `-Api d3d12` pub_interior + harbour_docks | 6.8 k / 642 k | 95.6 % | 25.1 k (588) | 0.56 % | 24.5 k (0) | 160.5 k (0) **0** | **0** | 550 | 2.4 |
| `rvk_rtwalk` walk 150 s from the savegame (turning: LOD switches, cache in / out) | 19.8 k / 4.75 M | 97.7 % | 98.2 k (1,088) | 0.23 % | 97.1 k (0) | 1.18 M (0) **0** | **0** | 936 | 4.3 |
| Vulkan `-Validation` + GPU-assisted, pub_interior | 4.3 k / 446 k | 95.5 % | 16.6 k (550) | 0.86 % | 16.0 k (0) | 110.4 k (0) **0** | **0** | 529 | 2.5 |
| fault injection `RVK_F2C4A_INJECT=2` (3 spots) | 1.4 k / 121 k | 94.0 % | 6.3 k (494) | 0.74 % | 5.8 k (**12**) | 30.0 k (0) **72** | 0 | 474 | 2.5 |

  The walk exercised slot reuse: 1,330 slots allocated, 394 released, 393 reused (op records rebuilt for LOD resource / illumination / material structure / derive refused), 0 refusals for a full table, the table's read-backs (every 240th frame) 0 differing. Env epoch changes: 2 - 8 per run (the start-up: viewport / targets / variants), none while a spot holds. A pass with no env or signature change derives only its scrub draws. **Fault injection:** 13 content changes were dropped (`injected 13`); the check frames found 72 stale slots, the rolling scrub 12, the first MISS lines name the differing field (`PS window 3 0x23dae5ea0 vs 0x23db02b20`: a lighting entry that had moved), and `native: owned static turned off: 83 MISS` followed in the same 5 s window; the same run without the knob has MISS 0. (A first version of the knob skipped every n-th *detected* change; most detected changes did not change the content (a stream binding re-checked, a texture that streamed in with the same slot) and the check had nothing to find: it now skips every n-th change that does change the content.)
* **Picture A/B** (the 7.11 method: `native.owned_persistent` toggled inside one hold every 300 frames, a shot per 50 frames from the start of the hold, `freeze_wait=3200`; mean absolute difference per RGB value of consecutive shots; logs / shots `rvk-work/scratch/f2c4a/ab_*`): pub_interior toggle pairs 0.86 / 0.66 / 0.007 / 0.001 / 1.14 / 0.000 / 0.000 against same-state pairs 0.19 (persistent on) and 2.75 (off; max 18.5 in the first seconds: eye adaptation), several toggle pairs **bit-identical**; harbour_docks 0.47 / 1.49 / 0.79 / 0.66 / 0.63 / 0.90 / 0.67 against same-state 0.50 - 1.29 (the sea, one 8.7); jungle 0.08 / 0.74 / 0.13 / 0.07 / 0.05 / 0.09 / 0.002 against same-state 0.004 - 0.52 (wind; one 21.6 outlier). Inside the controls' range everywhere; no error line in any of them. The order across buckets (first-seen) against the engine's pass order shows nowhere.
* **CPU** (per main view = per frame, the owned path's pieces of the `native: owned static (5 s)` line, mean of the last six 5 s windows, `-Perf`, RisenTest.exe, vsync off, measure timers OFF, shared copy + default lock, interleaved A / B / A / B; A = `native.owned_persistent` off = the 7.19 v2 per-draw path):

| position | arm | owned draws / view | compaction (stamps) | view | material | build | side effects | submit | **total** | per draw | engine + HAL CPU per frame |
|---|---|---|---|---|---|---|---|---|---|---|---|
| savegame hold (lod 2x; the position of this run is the small one of 7.16) | A | 82 | 51 (26) | 2.5 | 9.6 / 9.4 | 60 | 4.3 | 103 | **231 / 230 us** | 2.8 us | 6.72 / 6.73 ms |
| | B persistent | 82 | 50 (25) | 2.5 | 7.4 | 120 | 4.4 | 0 | **185 us** | 2.25 us | 6.64 ms (the second B run was a short sample, left out) |
| pub_interior spot hold | A | 139 | 69 (38) | 2.5 | 13.9 | 98 | 7.1 | 164 | **354 us** | 2.55 us | 8.34 / 7.98 ms |
| | B persistent | 139 / 141 | 71 / 69 (40 / 38) | 2.5 | 12.1 / 11.6 | 191 / 188 | 8.2 / 7.4 | 0 | **285 / 279 us** | 2.0 us | 8.10 / 7.88 ms |

  Persistent costs **-20 % of the whole owned path** at both positions (about -34 % per draw once the ~45 us fixed part of a pass, estimated from the two positions, is taken out: the view setup, the register-file restore, the env epoch, the first flush); the engine + HAL CPU per frame moves by -0.1 .. -0.2 ms, inside the run-to-run noise. A larger view (savegame, 509 owned draws per compacted view, private copy, **timers on** so it overstates): 877 us per view = 1.72 us per draw; fine split per compacted view: windows 58 us, signature 49, derive 15, **observers 280**, flush 40, other 83 (serve of the object record, ambient memo, items). **The section 11 target (~105 / ~400 us) is not reached**: what is left is, per draw, the observers' loop (0.55 us: 45 % of the persistent per-draw cost: F2c-4b), windows + signature (0.2 us), and per op the stamps (0.3 us: still 40 - 50 % of the compaction) and the object record's serve. Removing the derive + packet per draw took the per-draw cost from ~1.6 us (v2) to ~1.0 us; the next 0.5 us is the observers.
* **Refusals / MISS seen.** MISS and DIFFERING 0 in every run without the injection knob; draws composed late 0.2 - 0.9 % (the render-target texture of 7.16; derive refusal 7), refusals for a full table, a flush, a reserve or an observer's capture request: 0. **An early version composed the late ops after the pass's draws were queued and refused 12 - 15 % of the draws** (every op sampling the render target was refused for the whole frame instead of the first one: the late composition's texture transition lets the others derive); and its derivations ran before the rendering scope began (the device's formats are the scope's: `env` refusals 5 %): `ownedBeginScope` before the pass's epoch, an op composed late flushes the pending items first.
* **Deviations from section 11 and why.** (a) The dep table with reverse lists is a per-frame memoised env epoch + a per-draw signature (above): a polled dep per entity / resource would not be cheaper than the stamp reads it replaces (most entities have one op), and the cases the reverse lists were meant for (a texture streaming in, a lighting entry moving, a binding changing) are in the signature. The op-level stamps are unchanged. (b) `firstInstance` is the position in the frame's ref list, not the record slot (no 4,096-entry identity stream; the list also carries the view slot). (c) Lighting entries are not pinned by a seen mark + generation: `entryFor` is called per draw and frame (memoised per pass: it marks the entry seen) and the entry's window base is part of the signature, so an entry that was released and reused while a record was not drawn is found at the next sight. (d) The slot table is 4,096 (not 64K); a full table refuses, counted. (e) The persistent path needs the v2 view table (`RVK_OWNED_V1=1` = off). (f) F4-1 billboards, skinned actors and the shadow views are not on it.
* **Open issues.** F2c-4b: the per-draw observer loop (45 % of what is left; `loadOwnedRegisters` copies the windows' images into the register file for every observed draw and the pass restores the file: 15 KB twice per pass) and the per-op side effects; the per-op stamps / object serve (signals); the arena (28 bytes per draw per frame in the ring) can become device-local with F2c-4c's compaction. The injection knob only finds what a run changes (the 3 spots: 13 changes of ~30 per window at start-up): a walk gives more changes (a follow-up run). Not exercised in the game: a full table (the largest live set is 1,077 of 4,096; tested in the d3d9test case only through the guards), exclusive fullscreen, frame generation (they do not touch the owned lists).

**Addendum 2026-11-07: validity holes of the F2c-2 records (stampfix).** The soak's two findings had one cause class: the record stamps did not cover everything the composition depended on.
* *`PS window 3` DIFFERING (walk3 of the soak, same ctx every check frame).* The composed lighting entry is keyed by the illumination's **table slot** (`lightingKey(illumSlot, ...)`); the record kept `DrawRec::illumSlot` and the stamp only the illumination POINTER. Static owned ops are never composed, so nothing marked their illumination drawn: `IllumTable`'s idle sweep (1,800 frames) freed the slot, the next composition re-created it at another slot and thus another entry, while the retained record still derived the old one (and the freed slot's GPU record is zeroed: unlit). The billboards already called `pollDraw`; the static records did not. Fix: `OpRecord::illumSlot` + `IllumTable::slotLive(illum, slot)` (no lock, one vector compare; also marks the slot drawn = keep-alive), checked in `recEnter` and `ownedSelect` through `StaticOps::recLiveWhy` (rebuild reason "illumination slot gone").
* *Audit disagreements (2 - 3 per walk, all four walks, in the first seconds = loads).* Identified by vtable RVA of the object around the read address: `eCImageResource2` (0x9cc5d0) `+0x70` image data `0 -> pointer` (a sampled image bound as the placeholder until the data arrived) and `eCMaterialResource2` (0x9ccc20) `+0x38` state `2 -> 3` (a material not covered by the op's sub-mesh list, so not in the geometry stamp). The stamps hold the LOD resource's state and the lightmaps' residency but not these. The audit caught them (and rebuilt the record), but only as "disagreement". Fix: a record keeps `watch` = the logged word reads that looked at a loading resource (`orec::loadingRead`: a 4-byte state below 3 at an Engine.dll object's +0x38, or null image data at +0x70 of an object whose state is below 3; <= 16 per record); `recLiveWhy` re-reads only those (resident records have none: no steady-state cost), a change = rebuild ("resource load completed"). Pure rule tested (`native_tests`).
* *The audit stays as the net.* A disagreement now logs `native: owned records: MISS (audit): N read log disagreements` (warn, non-fatal; the record is rebuilt as before) next to the existing `audit DISAGREEMENTS` counters.
* *Soak (6 walks x 280 s, rvk_rtwalk from Risen_Save_0005, owned chain + `rt.table_entries` + compare, perdraw / bulk alternating, private copy -f2c4b, instance 28):* audit lines 0 / 0 / 0 / 0 / 0 / 0 (before: 2 - 3 per walk), `audit DISAGREEMENTS` 0, DIFFERING 0 (before: 5 draws in one of four walks, owned path turned off), MISS 0, owned path never turned off, error lines 0; "resource load completed" rebuilds 1 - 2 per walk. A first version probed `addr - 0x38` with the guarded read at page starts and logged a first-chance fault (scenario check FAIL): `objectVtable` now VirtualQuery-checks those. CPU: the owned path's `compaction` / `stamp and record checks` per main view over the same soak workload: before 222 / 113 us and 235 / 123 us (walk 2 / 4), after 226 / 113, 220 / 112, 220 / 113 us (walk 2 / 4 / 6): unchanged within noise (the slot check is one compare per owned op, the watch list is empty for resident records). The shared-lock savegame hold was not run separately: the soak's `-Perf`-free `RVK_F2C4_MEASURE` lines were used as the A/B. `rvk_tests` 294 / 0, `rvk_d3d9test --validation` Vulkan + `--backend d3d12`: 0 failures, 0 validation errors.

### 7.22 F5-0 measurements (2026-11-06, `RVK_F5_MEASURE=1`; no behaviour change)

Stage F5-0 of section 13. Code: `src/core/f5_measure.{h,cpp}` (pure geometry: ~128-triangle clusters from meshoptimizer, object-space frustum planes, normal cones, per-triangle classes, counters + report; tests `tests/f5_tests.cpp`, 4 cases),
`src/d3d9/f5_note.cpp` (`Device::f5Note`, one call per draw from `prepareDraw` behind one `f5::enabled()` bool test; nothing else changes). rvk.log `native: f5 measure (5 s) [<view>]`, `native: f5 clusters [<view>] meshopt|spatial`,
`native: f5 exact [...]`, `native: f5 exact vs clusters [...]`, `native: f5 lod memory`. Runs (Full, `-PlayerDefaults`: lod 2x, **`native.packet_cache=off`**: a replayed packet does not pass `prepareDraw`, so with the cache on only the draws it
refuses are counted), copy `risen-test-vk-f50`, logs in `rvk-work/scratch/f50/` (`m_<spot>.log`, `lod_save.log`, `pf_*.log`; `agg.py` / `pf.py` digest them). Spots = rvkcompare harbour_docks / jungle / pub_interior (200 s runs, 1,973 - 2,209 main
views each) and the savegame hold (`Risen_Save_0005`, `rvk_perfsave hold=60`, 3,173 main views).

**Methods and limits.**
* *Submitted triangles / index entries / vertex ranges*: CPU counts at the draw (`primitives` x instances; index entries = 3 x triangles = the vertex-shader upper bound without the post-transform cache; the vertex range is the engine's `numVertices`, an overestimate for sub-meshes).
  View keys: main z (`DepthPrepass`), main solid, main alpha (refracted + alpha + late), sun cascades / other shadow views by entry order (`shadow 0..3`; shadow 3 = rarely entered, 3 - 5 frames per run, ignore), sky cube. The cascades update every 2nd frame while the camera is still
  (`debug.shadow_stride` 2): numbers are per frame **in which the view was drawn**. **No pipeline-statistics queries**: the RHI has none (Vulkan `pipelineStatisticsQuery` is not enabled, D3D12 would need `PIPELINE_STATISTICS` queries
  too) and a query pool per scope is more than a small addition; the CPU counts and the per-triangle classification below stand in (the GPU numbers are `-PerfFrame` pass times).
* *Clusters*: for rigid static draws (`Kind::StaticMesh`, indexed triangle lists, a material vertex shader with `g_vInstanceData` at c32 and no skinning / SpeedTree / vegetation, POSITION FLOAT3 at offset 0; **large = >= 1,024 triangles per draw**) the draw's CPU copies
  of the vertex / index range (`Buffer::readCpu`) are cached and clustered once per distinct draw range (two builders: `meshopt_buildMeshlets` cone weight 0.5 and `meshopt_buildMeshletsSpatial` 64..128 triangles, <= 128 vertices; ~0.9 s of builds per run, 80 - 250 MiB cache).
  Per view and frame the draw's VS c0..c3 (checked against the camera block: 0 differences) and world rows c33..c35 give an object-space frustum (six planes) and eye; a cluster is out when its sphere is outside a plane, back-facing by meshopt's cone test
  (`dot(c - eye, axis) >= cutoff * |c - eye| + r`, ortho: view direction), "runs" = maximal runs of visible clusters in the builder's order and in a Morton order of the cluster centres (GD3D11's BVH order stand-in).
  Skinned, instanced, SpeedTree, vegetation, billboards, blended draws are counted in the submission tables but not clustered.
* *Exact per-triangle classes* (sampled: every 4th main frame for the main z pass, every 2nd shadow frame for large shadow draws): each vertex through the draw's clip rows; a triangle is frustum-out when all three vertices are outside one plane, back-facing by its screen winding and the draw's cull mode,
  else its projected area in pixels (viewport of the view) goes into the histogram. Conservative per triangle (a triangle crossing a frustum corner but outside counts as drawn). 70 - 370 M triangles per run. **Validation of the cone verdict**: a cluster called back-facing whose triangle is front-facing and fully inside = a false negative:
  with the right winding sign 0 of 24,000+ back clusters violated (main) and 0 of 6,000+ (shadows); the other sign violated every time (the sign is `+` for D3D9's winding in both perspective and ortho views). So the estimates are valid, and the cone result below is not an artifact.
* *LOD memory*: for the contexts of the main view's static draws (`ctx + 0x18` mesh PS: LOD count byte `+0x6c`, LOD resources `+0x40[b]`; `ctx + 0x28` = the chosen one), `res + 0x38 == 3` = loaded and `res + 0x2c` = the engine's byte count of a loaded resource (7.19 item 5).
* Extrapolation: no Radeon 680M was available; every 680M figure below is an **extrapolation** from the RX 7900 XTX (shared with other agents' games: GPU times are +- 20 %).

**1. Triangles submitted per frame (average over the hold; draws / triangles, k = 1,000; "inst" = triangles in instanced draws: SpeedTree / vegetation / instanced meshes).**

| view | harbour_docks | jungle | pub_interior | savegame |
|---|---|---|---|---|
| main z | 360 d / 704k (inst 334k) | 559 d / 949k (473k) | 308 d / 585k (299k) | 687 d / 775k (268k) |
| main solid | 376 d / 712k | 523 d / 957k | 317 d / 591k | 432 d / 781k |
| main alpha | 63 d / 22k | 110 d / 32k | 57 d / 20k | 50 d / 10k |
| shadow 0 (1024^2) | 50 d / 61k | 62 d / 59k | 58 d / 57k | 42 d / 32k |
| shadow 1 (2048^2) | 67 d / 96k | 91 d / 110k | 77 d / 103k | 68 d / 66k |
| shadow 2 (2048^2) | 165 d / 262k | 247 d / 347k | 207 d / 324k | 403 d / 474k |
| sky cube (when refreshed) | 20 d / 28k | 20 d / 28k | 20 d / 28k | 20 d / 28k |

Index entries = 3 x triangles (main z 1.8 - 2.9 M per frame). The three cascades add 0.4 - 0.6 M triangles in the frames they are drawn, z + solid 2 x 0.6 - 0.95 M: the geometry of one frame is 1.6 - 2.5 M triangles (4.8 - 7.5 M index entries) at the player defaults.

By triangles per draw, main z (draws / triangles per frame):

| class | harbour_docks | jungle | pub_interior | savegame |
|---|---|---|---|---|
| < 256 | 127 / 239k | 193 / 297k | 111 / 212k | 180 / 136k |
| 256 - 1K | 148 / 119k | 256 / 210k | 130 / 106k | 386 / 238k |
| 1K - 4K | 65 / 158k | 87 / 214k | 52 / 130k | 103 / 182k |
| 4K - 16K | 16 / 121k | 20 / 156k | 12 / 90k | 15 / 143k |
| > 16K | 3 / 66k | 4 / 73k | 2 / 47k | 4 / 76k |
| **>= 1K: share of draws / of triangles** | 23 % / 49 % | 19 % / 47 % | 21 % / 46 % | 18 % / 52 % |

The < 256 class is mostly instanced draws (SpeedTree / vegetation: triangles of all instances). Cascades follow the same shape (shadow 2 of the savegame: 51 / 40k, 251 / 245k, 101 / 184k, 1 / 5k). About 20 % of the draws hold half the triangles; 3 - 4 draws a frame are > 16K triangles (level meshes).

**2. Large static meshes (>= 1K triangles/draw, rigid): what a cluster cull could remove** (share of the large static draws' triangles; "large static" = 35 - 43 % of the main z triangles: 277k / 340k / 204k / 334k per frame; 54 - 108 such draws per frame; 40 - 65 clusters of ~128 triangles per draw; cluster
columns: meshopt / spatial, same draws and frames as the exact column, the exact figures from the sampled frames of the same run).

| view | exact per-triangle frustum-out | cluster sphere out (meshopt / spatial) | exact back-facing (hardware culls it) | cone back-only (meshopt / spatial) | triangles drawn by the hardware |
|---|---|---|---|---|---|
| main z, harbour_docks | 56.4 % | 50.8 / 53.0 % | 25.3 % | 0.4 / 1.0 % | 18.3 % |
| main z, jungle | 45.2 % | 38.8 / 41.3 % | 29.5 % | 0.4 / 1.0 % | 25.3 % |
| main z, pub_interior | 50.4 % | 44.0 / 46.5 % | 29.1 % | 0.6 / 1.3 % | 20.5 % |
| main z, savegame | 54.9 % | 46.4 / 49.4 % | 23.0 % | 0.1 / 0.3 % | 22.0 % |
| cascades (all drawn ones), harbour_docks | 34.0 % | 30.0 / 32.0 % | 29.5 % | 0.1 / 0.1 % | 36.5 % |
| cascades, jungle | 14.4 % | 13.6 / 14.0 % | 39.7 % | 0.1 / 0.2 % | 45.9 % |
| cascades, pub_interior | 21.9 % | 20.6 / 21.3 % | 35.6 % | 0.0 / 0.2 % | 42.6 % |
| cascades, savegame | 0.3 % | 0.3 / 0.3 % | 47.5 % | 0.2 / 0.1 % | 52.2 % |

(The exact figures are over the whole-run triangle sums, e.g. 137 - 265 M triangles per spot for main z; the cluster columns come from the same draws in the same frames; a per-frame window varies by +- 10 points with the view.)
* **The frustum cull of clusters is real: 39 - 53 % of the large meshes' triangles in the main z pass** (cluster tests find 90 - 95 % of what the exact per-triangle test finds: 50.8 of 56.4, 38.8 of 45.2, 44.0 of 50.4, 46.4 of 54.9; the spatial builder gets 2 - 3 points more for 40 % more clusters),
  which is **15 - 21 % of the whole main z triangles** (large static share x cluster-out: 0.39 x 0.51, 0.36 x 0.39, 0.35 x 0.44, 0.43 x 0.46). The solid pass is similar (cluster-out 51 - 65 %, but only 27 - 45 large draws per frame, since the solid pass lists static meshes per sub-mesh).
  In the cascades the cull is 0 - 30 % (cascade 0 and 1 are small and mostly inside their frusta; cascade 2 / the long ones 17 - 25 %; the savegame's cascades are all inside), about **3 - 10 % of a cascade's triangles**.
* **The back-face (normal cone) cull is worthless at cluster size: 0.1 - 1.3 % against 23 - 47 % of triangles the hardware culls individually.** Risen's level meshes (rock, terrain, walls, 2009 modelling) give 128-triangle clusters wide normal spread (the cone test needs `dot(view, axis) >= sin(half angle)`),
  and the radius term of the conservative form kills it at close range. Cone data in a cluster table is not worth its bytes: **frustum only** (plus HiZ later).
* Visible clusters per large draw: 36 - 48 (meshopt) / 52 - 68 (spatial); **visible runs per large draw in the main z pass: 1.7 - 2.4 (builder order), 2.2 - 3.0 (Morton order of the centres; the builder's own order is better than a Morton re-sort), spatial builder 1.9 - 3.1 / 2.2 - 3.8**,
  cascades 1.0 - 1.4 runs. So 70 - 110 large draws a frame become ~150 - 260 indirect commands in the main view (GD3D11's merging works: about 2 commands per draw), 1.0 - 1.4 per draw in the cascades. A draw whose clusters are all out: 1 - 4 % of large draws (meshopt), 3 - 14 % (spatial).
  Cluster builds: 332 meshes / 1.5 M triangles = 0.9 s (meshopt + spatial together) for ~90 large meshes of one spot: the offline cache is small (the unit of caching = the mesh range).

**3. Triangle screen-size histogram** (main z, all analysed rigid static draws of every size, sampled frames; share of the triangles the hardware actually rasterizes = inside the frustum and front-facing; projected area in 1600x900 pixels). Of all submitted rigid triangles 37 - 49 % are
frustum-out, 28 - 33 % back-facing, only 22 - 30 % are drawn.

| | < 1 px | 1 - 4 px | 4 - 16 px | 16 - 64 px | > 64 px |
|---|---|---|---|---|---|
| harbour_docks | 32.5 % | 22.4 % | 21.6 % | 14.1 % | 9.4 % |
| jungle | 35.1 % | 22.9 % | 21.1 % | 12.8 % | 8.1 % |
| pub_interior | 31.0 % | 22.4 % | 22.0 % | 14.7 % | 9.8 % |
| savegame | 31.0 % | 25.0 % | 22.3 % | 12.6 % | 9.1 % |
| cascades (map pixels), large draws, jungle / savegame | 44 % / 50 % | 23 % / 28 % | 16 % / 13 % | 11 % / 6 % | 6 % / 3 % |

**About a third of the drawn static triangles cover less than one pixel and 55 - 58 % less than four** (rigid static draws only: skinned / SpeedTree / vegetation are not in it). Sub-pixel triangles cost front end + setup, and each one that is rasterized shades at least a 2x2 quad
in the solid pass (4x pixel work for a 1-pixel triangle); the player defaults' `lod.meshes` / `lod.view` 2x double the distances at which the engine switches LODs, which pushes this share up (the runs are at 2x). Approximate: the histogram counts triangles, not covered pixels, and the z-pass uses the same geometry as the solid pass.

**4. GPU time** (`-PerfFrame` with `RVK_PERF_FRAMES=24`, averaged, RX 7900 XTX, 1600x900, `RisenTest.exe` vsync off, Full, player defaults, GPU shared with other agents: +- 20 %, the pass times of the vertex-bound passes are the stable ones). Frames 3500 (savegame hold) and 1500 (pub_interior hold).
ms per frame: cascades = the three depth-only passes (drawn every frame in these 24 frames' average: 0.24 ms in the pub's frame 1500 when all three draw).

| | pub_interior | pub, low bandwidth on | savegame (2 runs) | savegame, low bandwidth on (2 runs) |
|---|---|---|---|---|
| main submission total | 4.63 | 2.79 | 4.49 / 2.21 | 4.35 / 3.81 |
| depth pre-pass (main z, 209 draws / ~690) | 0.30 | 0.15 | 0.33 / 0.24 | 0.33 / 0.33 |
| solid pass | 1.33 | 0.75 | 1.29 / 0.40 | 1.53 / 1.09 |
| cascades (3 passes) | 0.12 | 0.12 | 0.13 / 0.13 | 0.13 / 0.13 |
| water.rt + water.ocean + wet shore | 1.51 | 0.67 | 1.62 / 0.82 | 1.32 / 1.16 |
| rt.scene + light.cluster + light.shadow | 0.42 | 0.42 | 0.54 / 0.17 | 0.40 / 0.58 |
| other raster passes (alpha, refracted, post chain, GUI) | 0.41 | 0.54 | 0.43 / 0.38 | 0.42 / 0.39 |

* **The geometry passes are small on the 7900 XTX: z pre-pass 0.24 - 0.33 ms, cascades 0.13 ms per frame (0.24 ms when all three are drawn), together 0.4 - 0.6 ms = 10 - 20 % of the 2.2 - 4.6 ms main submission**; the pixel / compute passes (solid 0.4 - 1.5, water RT 0.6 - 1.0, ocean 0.2 - 0.7, post) dominate.
  Low bandwidth only changes the pixel passes (solid, water RT, ocean: the test copy's discrete GPU: -0.5 to -1.8 ms in the pub, within the noise of the other agents' load in the savegame); z and cascades do not move (they are vertex / ROP bound).
* **680M extrapolation (not measured)**: the XTX has 96 CU, 6 shader engines (6 primitive units), ~2.5 GHz; the 680M 12 CU, 1 shader engine, 2.2 GHz: front end ~5 - 7x slower, shader throughput ~15 - 20x, bandwidth ~10x less. The z pre-pass and the cascades are front-end / vertex bound,
  so ~5 - 7x: **z 1.2 - 2.3 ms, cascades 0.7 - 1.6 ms (drawn frames), solid 6 - 20 ms** (pixel + vertex): geometry about 2 - 4 ms of a 680M frame at best, certainly not the largest part (the solid pass and the post / RT / water passes are). Treat as +- 2x.

**5. Memory: every LOD of every in-view static mesh resident.** At the savegame (lod 2x), main-view static draws (446 meshes per frame, 260 with 2 or more LODs; 252 frames): the resources of the **chosen LODs 46.6 MiB**, the siblings that are loaded today **62.8 MiB** (all resident LODs of the in-view meshes),
with every sibling resident (the 2 % not loaded estimated from the measured size ratio LOD1 / LOD0 = 0.33, 351,716 samples) **64.8 MiB: +2.0 MiB (+3 %) over today, +18 MiB (+39 %) over the chosen-only set**. The engine keeps nearly all sibling LODs of the in-view meshes loaded already (3,780 of 177,912 sibling slots per
252 frames unloaded): its `UpdateLoD` prefetches the finest wanted level and the coarser ones were loaded earlier while the camera moved. The whole mesh arena holds 69.6 MiB at that point (the cached-in world; `perf: mesh arena`), 1 block of 64 MiB + 1 dedicated buffer. **Memory is not an obstacle to a GPU-side LOD choice among the engine's own LODs**
(all LODs resident costs +2 MiB; even forcing the coarser ones in would be ~+18 MiB of 70). The resource byte count (`res + 0x2c`) matches the unique drawn vertex + index buffer bytes in small cases (2.5 = 2.5, 11.9 = 11.9 MiB) and is larger at the savegame (46.6 vs 32.1 MiB: contexts whose draws are replayed / instanced share buffers; treat the
accounting as an upper bound).

**Recommendation for F5-1 / F5-2.**
* **Threshold: >= 1,024 triangles per draw** (static rigid draws): 18 - 23 % of the main z draws (66 - 122 per frame) hold 46 - 52 % of its triangles, and a draw of < 1K triangles has < 8 clusters (cluster culling inside it gains nothing the engine's per-object cull does not already give). Keep the 256 - 1K class (130 - 390 draws, 106 - 238k triangles) unclustered;
  revisit 512 only if the first measurements of F5-2 say the draw count matters. Cluster size 128 triangles / 128 vertices; **meshopt's `buildMeshlets` is enough** (the spatial builder culls 2 - 3 points more but has 40 % more clusters and more runs; GD3D11's BVH order is not better than the builder's order here).
* **Expected triangle saving of F5-2**: main z / solid **15 - 21 % of the view's triangles** (39 - 53 % of the large static meshes), cascades **3 - 10 %**; no cone (0.1 - 1.3 %), no back-face test in the cull pass: **frustum clusters only**; the cull is exact enough (false negatives against the exact triangle test: none, the sphere test is conservative by construction; the 5 - 10 points the cluster test misses against the per-triangle one are triangles
  whose cluster sphere crosses the plane). Merged runs: ~2 indirect commands per large draw (main), 1 - 1.4 (cascades).
* **GPU saving**: on the 7900 XTX nothing visible (0.4 - 0.6 ms of geometry passes x 15 - 20 % = 0.06 - 0.12 ms); **on the 680M an extrapolated 0.3 - 0.7 ms per frame** (z 1.2 - 2.3 ms x 18 % + drawn cascades 0.7 - 1.6 ms x 6 %, plus the saved vertex shading in the solid pass) = a few percent of a 680M frame. **Cluster culling alone is not worth its complexity on the 680M**;
  it is worth doing as part of F2c-4c's GPU-driven draws because the indirect-count machinery exists then, the cull pass is cheap (4 - 7k clusters per view, a trivial compute pass) and the per-cluster table is needed for HiZ (F5-3) anyway. Order: F5-1 (build + table) can follow F2c-4c cheaply; ship F5-2 behind a setting and keep it only if the 680M measures >= 0.3 ms.
  The 25 - 47 % hardware back-face cull and the 22 - 30 % drawn share also say the geometry front end shades (vertex-shades) ~3 - 4x more vertices than it rasterizes: a cheaper lever on weak GPUs than cluster culling is the **vertex shader's cost per vertex** (the z pass could use a position-only shader / stream: z pass at 700k triangles has no need for normals, uv, colours).
* **Does the far-triangle data change "no cluster LOD"? Not the decision, but it sharpens the next step.** 31 - 35 % of drawn static triangles are < 1 px and 55 - 58 % < 4 px at lod 2x: that is real front-end and quad-overshading cost, and it grows with the player defaults' 2x distances.
  But the engine already has 2 - 3 LOD levels per mesh, all resident (+2 MiB measured), so **the F5-2 per-instance GPU LOD pick among the engine's own LODs** (distance -> index range) is the cheap remedy and is memory-free; no simplified cluster hierarchy is needed. Re-measure after F5-2: the < 1 px share of the main z pass
  should fall well below 20 % with a screen-size-driven LOD pick (target: a LOD whose triangles average >= ~2 px), and the F5-3 HiZ pass decision stays open until the occlusion share (not measured here: the engine's per-object cull already removes whole objects) is known.

### 7.23 F2c-4b bulk observers (2026-11-07, `DrawObserver::observeOwned`; no new setting, `RVK_F2C4B_PERDRAW=1` = the per-draw path)

Stage F2c-4b of section 11 / F8-2 of section 12. Files: `src/d3d9/device.h` (`DrawObserver::bulkOwned / observeOwned`, `Device::OwnedRecordView / OwnedObsInfo / OwnedDrawScope`, `OwnedFrame`), `src/d3d9/owned_persist.cpp` (`ownedObserve`, `ownedBulkBegin / Flush`, the scope, the default `observeOwned`), `native/static_ops_owned.cpp` (the pass loop's calls), `src/enh/rt_scene.{h,cpp}` (`observeOwned`, `OwnedCover`, entry liveness, the ambiguity guard), `src/enh/rt_registry.h` (`Instance::ambiguous`), `src/engine/scene_table.{h,cpp}` (`stamp`), `tools/d3d9test` (`BulkTestObserver`, block 7 of "owned persistent (F2c-4a)").

* **Interface.** An observer that returns `bulkOwned()` is not shown the persistent owned draws one by one before they are queued; the persistent loop (`Device::ownedObserve`, called per draw with `OwnedObsInfo` = persistent slot id, derivation signature, env epoch, "slot rewritten this frame") appends an immutable `OwnedRecordView` (retained draw, the record's own windows, the tag it is drawn under, id, `entered` = not in the previous pass of the kind, `changed`) for **plain static meshes only** (`Kind::StaticMesh`, not instanced, not blended, triangle list: the draws that can never ask for a capture) and `ownedBulkFlush` delivers the list to every bulk observer through `observeOwned(Device&, const OwnedFrame&)`: once at the end of each owned pass (z pre-pass, solid), plus once before an op of the pass is composed late (its record may be dropped: the draws collected so far are flushed first, so the observation order stays the engine's). The last flush of a pass carries `left` (ids of the previous pass of the kind that are not in this one). `OwnedDrawScope` = the device state of one owned draw (windows, tag, the draw's shaders, its `ReplaceQuery`; `loadRegisters()` only on demand; a capture request made through it is counted as `late capture requests`, 0 in every run). **The default `observeOwned` is the per-draw loop** (observes / `coversDraw` / registers / `observe`), exercised by the d3d9test observer (calls, entered / left / mid-pass flush / non-bulk observers / the capture contract). Observers that stay per-draw (GI, object motion, material RT, the opaque-observing replacers: the ocean floor) are served in the loop as before, only now **with the F8-0 pre-filter** (`coversDraw`), which the persistent loop did not have.
* **RT scene hand-over (F8-2).** `RtScene::bulkOwned()` = table entries on and the scene active. `observeOwned` keeps, per record id, an `OwnedCover` (the retained draw's pointer, entity / op, signature, env epoch, the covering entry's liveness slot + generation, its scene-table stamp, slot and relearn salt). A record is skipped with **no hash lookup and no query** when: it is not `entered` / `changed`, not a check frame, the cover matches the record view field for field, the entry is alive (`entryGen_[idx]` moves whenever `tableErase` / `tableClear` kills an entry: LOD switch, slot moved, instance gone, ambiguity), `engine::scenetable::stamp(slot)` equals the entry's (the new compact stamp array: generation + version, 0 = free; the slot structs are 200 bytes and were the cache misses of the old checks, `tableMaintain` uses it too) and it is not the entry's relearn frame (every 32nd). Everything else takes the long way: the same `coversDraw` + full `observe` as per draw (derived from the record's windows). The registry stays the only owner of TLAS instances; discovery is the learn on the first full observation (entered records), drops are the existing signals; `left` is counted (rvk.log `rt: scene table bulk (5 s)`), the entries of records that left stay (pinned) as in F8-0.
* **F8-0's material ambiguity: measured, guarded, not the cause.** Hypothesis of 7.20: an instance two unlike draws land on in one frame (instanced + plain, two passes) keeps the last learned material while covered. Fix built: `applyMaterial` flags `Instance::ambiguous` when a second rank-4 draw of the frame resolves a *different* material than the first; such an instance's entries are dropped (`tableDropInstance`), never learned again (`refusedAmbiguous`), its draws stay on the observed path; the check frames' material diff skips it (`ambiguousSkipped`). **It never fired in any run (learns refused 0, entries dropped 0, in ~25 runs of the gates below)**: the material differences that remain are not rank-4 conflicts. Their form (same texture, `sampler 7 / 6`, `uvSet 0x300 / 0x310` = the "no texture" bit, masked / alpha fields, shield and bark / herb instances; carried items and cut-outs) says the instance's material was resolved from an engine-graph state that changed without a signal the signature (`RtMaterials::signature`: shaders, tag material, texture pointers) contains; the covered path keeps the older result until the next relearn (<= 32 frames). Rate **1 - 7 x 10^-5 of the compared pairs**, independent of bulk vs per-draw (walk, same build: bulk 45 / 2 / 67 and per-draw 47 / 35 in 330 - 740 k pairs), the same range as 7.20 (1.3 - 6.6 x 10^-5); key / rows / flags / data differences 0 everywhere. Decision (the conservative one): keep the guard (free), keep `kTableRelearn` = 32, document the residue; the trip rule stays (a 5 s window with > 8 and > 0.5 %: reached only in the first seconds after a load, where streaming changes many materials: one short run did trip, the rest were run with `RVK_TABLE_NOTRIP=1` to keep counting).
* **Side effects batched / register file: already per resource / per pass, measured small.** `touchNow` already touches once per resource per frame (`touched_.first`; 7.19 / 7.21 measurement 5), the side effects of an owned op cost 3.8 - 4.5 us (savegame) / 11 - 14 us (pub) per main view = 2 - 3 %; `BindLightmaps` per op and `EnableStream` are not proven idempotent and were left alone. The register file is saved once and restored once per pass (15 KB each way, ~1 us) and `loadOwnedRegisters` now runs only for the draws that take the long way (the bulk path loads per long-way record, not per draw); a lazy save inside the device would change which state the pass gives back after a late composition: not done (about 1 us).
* **Gates.** `rvk_tests` 294 / 294. `rvk_d3d9test --validation` PASS on Vulkan and D3D12 (0 failures, 0 validation errors), new block 7 of the persistent case (observer calls, entered / left deltas, mid-pass flush, per-draw observer, 0 capture contract violations). The change adds **no GPU address**: observation is CPU only, so there is no new shader read to guard; first game run still as required: pub_interior Vulkan `-Validation` + GPU-assisted validation, `shadows.rt=near`, table entries + compare: no validation message, no `[error]`, no device loss. Game runs (Full, `-PlayerDefaults`, owned chain + `native.owned_persistent` + `rt.table_entries` + `debug.rt_table_compare`, private copy `risen-test-vk-f2c4b` instance 28, logs `rvk-work/scratch/f2c4b/*.log`; `gi.enabled` is **not** used: the GI observer reads every register, so owned draws are not made while it is on, section "Deviations"):

| run (compare on) | check frames | pairs compared | key / rows / flags / data | material | missing | owned MISS / DIFFERING | errors |
|---|---|---|---|---|---|---|---|
| Vulkan pub_interior `-Validation` + GPU-assisted | 235 | 59,981 | 0 | 0 | 0 | 0 / 0 | 0 |
| Vulkan 3 spots (harbour_docks, jungle, pub_interior) | 474 + 428 (2 runs) | 143,872 + 139,303 | 0 | 1 + 11 | 0 + 3 | 0 / 0 | 0 |
| savegame hold 60 s (lod 2x) | 542 + 531 | 339,380 + 409,832 | 0 | 15 + 12 | 0 | 0 / 0 | 0 |
| D3D12 pub_interior + harbour_docks | 357 + 391 | 83,272 + 100,079 | 0 | 0 + 1 | 0 | 0 / 0 | 0 |
| FFT ocean (`water.ocean=fft`, sea floor bakes: 132 / 146 meshes observed through `observeOpaque`) harbour_docks + sea_cliff | 430 | 117,445 | 0 | 2 | 0 | 0 / 0 | 0 |
| `rvk_rtwalk` 100 s from the savegame (turn 12 deg / s: LOD switches, characters skinned by compute, animated SpeedTrees: 15 + 25 in the TLAS) x 4 bulk runs | 865 / 860 / 852 / 859 | 417 k / 741 k / 732 k / 413 k | 0 | 3 / 49 / 45 / 2 | 1 / 0 / 5 / 0 | 0 / 0 (one run: see below) | 0 (one run: see below) |
| same, per draw (`RVK_F2C4B_PERDRAW=1`) x 2 | 736 / 863 | 348 k / 413 k | 0 | 35 / 47 | 1 / 5 | 0 / 0 | 0 |

  **Bulk path engaged** in all of them (`native: owned bulk observers`: 40 k records per 5 s, ~97 % of the table's asked draws skipped without a lookup, late capture requests 0, entered / left in the hundreds per run = records that cache in / out). `rt.debug=depth` probes (2 spots, 10 windows x 25 probes): table off 161 match / 45 farther / 18 closer / 0 missed / 0 sky, on (bulk) 162 / 43 / 19 / 0 / 0: no new red or magenta. **Picture A/B inside one hold** (`rt.table_entries` toggled every 300 frames, pub_interior, `shadows.rt=near`, `freeze_wait=3200`, a shot per 50 frames, `ab_pub`): the six later toggle pairs are **bit-identical** (mean abs 0.000, max 0), the first pair 1.13 (max 14: the first seconds' eye adaptation, as in 7.21), same-state pairs 0.08 (on) / 0.24 (off); the toggles also exercise the run-time switch (`tableClear` -> per-draw path -> bulk). **One finding:** one of the five bulk walks ended with `native: owned static turned off: 44 draws DIFFERING on the check frames` (F2c-2's own compare: `PS window 3: kind 3 base 0x23f9897a0 vs composed 0x23f9af8a0`, a retained record's lighting-entry window base differs from the composition's, at a window with 74 record rebuilds for LOD / material structure changes, the same record every check frame afterwards: ~800 per 5 s); the two repeat bulk walks, the per-draw walks and all other runs: 0. The window is the lighting entry (`native.lighting_images`) which neither the observers nor the bulk path touch, so it is judged a rare pre-existing invalidation miss of the F2c-2 records (7.21's walk had 0 in one run); not reproduced, **open** (a longer walk soak with `RVK_F2C4B_PERDRAW=1` against bulk would tell).
* **CPU** (per main view = z + solid pass, `-Perf`, RisenTest.exe, vsync off, measure timers OFF, shared copy + default lock, `shadows.rt=near` so the RT scene is active in every arm, `gi` off; A = `native.owned_persistent` only, table entries off = every draw observed; B = + `rt.table_entries` per draw (`RVK_F2C4B_PERDRAW=1`, with the pre-filter now also in the persistent loop); C = F2c-4b bulk; E = C + the slot stamp + the cover copies (final build); windows with >= 60 / >= 100 owned draws per view only, mean of the last six 5 s windows; the second round of each pair ran beside another agent's CPU profile and is shown where clean, the rest ranges):

| position | arm | owned draws / view | owned path total | of it compaction / build / side effects | bulk observers | RT observe (draws, of them skipped by the engine pass) | RT pass CPU | engine + HAL per frame |
|---|---|---|---|---|---|---|---|---|
| savegame hold (lod 2x) | A | 82 | **189 / 186 us** | 51 / 124 / 4.5 | - | 0.283 / 0.285 ms (549, 228) | 0.387 / 0.394 ms | 6.89 / 6.96 ms |
| | B | 82 | **164 / 164 us** | 50 / 101 / 4.3 | - | 0.206 / 0.206 ms (352) | 0.444 / 0.459 ms | 6.93 / 6.96 ms |
| | C | 82 | **160 / 161 us** | 49 / 98 / 4.2 | 14.9 / 14.7 us | 0.205 / 0.207 ms (350) | 0.446 / 0.458 ms | 6.88 / 7.00 ms |
| | E | 82 | **160 us** | 50 / 97 / 3.9 | 14.5 us | 0.211 ms (352) | 0.456 ms | 6.92 ms |
| pub_interior spot hold | A | 139 | **421 / 430 us** | 120 / 268 / 13.6 | - | 0.419 / 0.468 ms (887) | 0.390 / 0.400 ms | 8.14 / 8.39 ms |
| | B | 139 | **355 us** (2nd run disturbed) | 118 / 207 / 11.5 | - | 0.290 ms (589) | 0.424 ms | 8.02 ms |
| | C | 139 | **344 us** (2nd: 396) | 116 / 197 / 11.1 | 41 us | 0.290 ms (587) | 0.429 ms | 8.04 ms |
| | E | 139 | **346 / 377 us** | 118 / 199 / 11.5 | 42 / 45 us | 0.299 / 0.321 ms (587) | 0.435 / 0.456 ms | 8.24 / 8.85 ms |

  **Result: -15 % / -18 % of the owned path against A (189 -> 160 us, 421 -> 345 us), RT observe -0.07 / -0.12 ms, RT pass CPU +0.07 / +0.05 ms (the off-screen entries stay in the TLAS: F8-0's growth), engine + HAL per frame inside the run-to-run noise (+-0.1 ms).** Owned path + RT observe + RT pass together: savegame 0.86 -> 0.83 ms, pub 1.23 -> 1.08 ms. **Most of it is the persistent loop's pre-filter (A -> B: -25 / -66 us); the bulk hand-over adds -4 / -11 us on top of B** (the covered draws were already cheap with the pre-filter: ~0.4 us each). **The section 11 target (~25 - 30 / ~95 - 120 us) is not reached**: what is left of the owned path at the savegame is compaction 50 us (the per-op stamps / record checks 25), build 97 us (windows + signature + derive inputs + object serve; 7.21's fine split), side effects 4; the bulk observers cost 14.5 / 41 us = ~130 ns per record, **of which the relearn is the largest part**: every entry takes the full `observe` again every 32nd frame (materials, uv maps: ~8 of 280 records per frame at ~3 us), the skip path itself is ~15 ns per record (a steady frame: 4.4 us for 34 records).
* **Deviations from sections 11 / 12 and why.** (a) `observeOwned` is called per owned *pass* (and before a late composition), not once per frame: an observer reads device state (phase, view, tag, windows) that is the pass's, and a mid-pass flush keeps the engine's observation order across late-composed ops; entered / left are per pass kind. (b) Bulk applies to plain static meshes only; every other kind / observer stays per draw (a capture request must be possible before the draw is queued). (c) F8-2 "discovery from entered / left" is the existing learn on the first full observation plus the skip cover; `left` records keep their entries (pinned) exactly as in F8-0: dropping entries on `left` would only throw away what the next sighting needs (the TLAS cover of off-screen objects is the point). (d) The material ambiguity fix is a guard that never fired; the measured differences are engine-graph residency changes (above). (e) `gi.enabled` cannot be combined with the owned path (`Registers::All` of the GI observer): the "GI + shadows.rt" first run of the brief was run with `shadows.rt=near` only; teaching GI the owned windows is separate work (it reads the ambient registers of every solid draw). (f) The register-file save was not made lazy (state semantics, ~1 us) and the per-op side effects not deduplicated further (2 - 3 %).
* **Open issues / what the rest needs.** (1) The per-op stamps (~0.3 us / op = half of the compaction): they validate an op record against the engine's change state by reading ~10 engine fields per op; cheaper signals = the engine's own change notifications for materials / illumination / LOD (as `scenetable` does for matrices) or an op-level generation derived from the signals F2c-4c's GPU compaction would also need. (2) The relearn: a signal for "the material graph of this resource changed" would let the 32-frame period go (the residual material differences above are the same gap); until then it is the biggest part of the bulk cost and of the 1 - 7 x 10^-5 differences. (3) The build (97 / 199 us): windows + signature are per draw per frame by design (the signature is the validity test); a per-record "nothing moved" stamp from the same signals would let a record skip them. (4) `observes()` / the record view are built per draw in `ownedObserve` (~20 ns); fine. (5) The F2c-2 lighting-window DIFFERING seen once (above) needs a soak. (6) GI cannot use the owned path.

### 7.24 Full-frame CPU profile (2026-11-07, build 91720f3 + `-CpuProfile`; no behaviour change)

Question: where does the engine thread's frame go in Full, and how much of it can ownership remove? Savegame recipe (`Risen_Save_0005`, `RisenTest.exe`, vsync off, `-PlayerDefaults`, lod 2x, hold 60, shared copy + default lock), steady windows only (fps >= 100, no loading samples), 2 runs per arm (24 windows) and one pub_interior hold (4 windows); logs and scripts in `rvk-work/scratch/fprof/` (`ana.py`, `comb.py`, `pcdir.py`). ms per frame = share x the frame interval of that 5 s window. Arms: **Full** = defaults; **Owned** = the whole F1 / F2c chain (`native.material_table, illum_table, lighting_images, owned_windows, owned_objects, owned_records, scene_draws`) + `owned_static`, `owned_persistent`, `owned_billboards`, `rt.table_entries` (`-Setting` does not apply `needs`: a first attempt with only the four named settings had owned_static gated off and was discarded); **Wrapper**. `-Perf` CPU (engine + HAL) of the same arms: Full 6.53 / 6.34, Owned 7.12 / 7.42, Wrapper 5.30 / 5.22 (draw calls 2.0 - 2.1 of it in Wrapper, 0.57 in Full).

**By module** (ms / frame of the engine thread):

| module | Full | Owned | Wrapper | pub_interior (Full) |
|---|---|---|---|---|
| frame interval | 6.50 | 7.54 | 5.75 | 4.93 |
| d3d9.dll (ours) | 3.79 | 4.58 | 2.47 | 2.75 |
| Engine.dll | 0.84 | 0.88 | 1.46 | 0.59 |
| ntdll (heap, SRW locks, waits) | 0.48 | 0.58 | 0.63 | 0.40 |
| vcruntime140 (memcpy / memmove) | 0.40 | 0.44 | 0.19 | 0.33 |
| driver amdvlk64 | 0.36 | 0.40 | 0.37 | 0.32 |
| sharedbase (engine math, strings) | 0.33 | 0.35 | 0.40 | 0.31 |
| Game.dll + Script + gui2 + fmod + sentry | 0.18 | 0.17 | 0.17 | 0.12 |
| ucrtbase + msvcp140 | 0.07 | 0.09 | 0.09 | 0.06 |

**The engine's own code is 18 % of Full's thread (Engine.dll 0.84 + sharedbase 0.33 = 1.2 ms); our DLL is 58 %** (4.4 ms with the CRT / ntdll / driver it calls). Same structure as performance.md 21.1 (Full: d3d9 4.37, Engine 1.03 of 8.09; now 3.79 / 0.84 of 6.50: the packet cache, lean frames and the fast sort did their work). What the lean path left is **our composition of the engine's ops** (`StaticOps::opEnter` 3.35 ms = 52 % of the frame), not the engine's collection.

**Inclusive tree, Full** (ms / frame; the sampler lists entries >= 1.5 % only, nesting is read from the named frames; names of unexported functions come from the sampler's COMDAT hints, `~` = best guess):

| item | ms | what it is |
|---|---|---|
| `eCApplication::DoRender` | 5.83 | 90 % of the thread |
| `eCFrustumDatabase::Execute` (main view) | 4.09 | |
| - `FrameTap` / `StaticOps::opEnter` (ours) | 3.35 | the native frame: compose + issue + HAL for every op of the lists |
| - - `StaticOps::issue` (HAL replay: `replayDraw` 0.49, `drawIndexedPrimitive` 0.58, `prepareDraw` 0.39) | 1.54 | per draw: packet build / replay |
| - - mesh slots `RenderContextMesh::Render` / `RenderZPass` (`composeOp`, `materialDraws`, `meshZDraws`) | 0.44 / 0.56 | main + shadow views together |
| - - SpeedTree Render / ZPass (`composeTree` 0.52 incl. lambda 0.19) | 0.42 / 0.51 | trees |
| - - tree billboards Render / ZPass | 0.17 / 0.19 | |
| - - vegetation Render / ZPass | 0.28 / 0.15 | |
| - - skinned actors, particles (`composeParticle` 0.16, `composeFx` 0.20), effects (`composeEffect` 0.21, `issueLean` 0.17) | ~0.5 | |
| - engine residual of Execute (`OnQueryNodes` 0.21, `RequestRenderOps` 0.21, `Request` 0.14, `ExtractFromFrustum` 0.10, `CullCamera` 0.11, `eCBeamTree::Cull` self 0.12, op-list sorts 0.10 in our `fastsort`) | ~0.75 | engine collection / culling / sorting |
| `eCCameraBase::Update` -> `UpdateShadowMaps` -> `RenderShadowMaps` | 1.07 / 0.87 | `ExecuteShadowMap` 0.57 (our shadow z ops, the same slots as above); cascade fit / setup ~0.3 |
| injected passes `Device::inject` (CPU recording) | 0.91 | `LightTable::record` 0.41 (+ `lights::collect` 0.09), `RtScene::record` 0.33 / `observe` 0.24, ocean 0.05, `CopyCurrentRTToImage` 0.14 |
| post chain `eCImageFilterEffect::OnUpdateOutput` / `ExecuteEffect` (`effectBegin` 0.43, `UpdateShadowMask` 0.20) | 0.36 | |
| `eCProcessor::Process` (game logic, scripts 0.06, AI, animation, physics, streaming, audio: all of it) | 0.65 | 10 % |
| COMDAT hint `eCStaticLightPreprocessor` copy ctor (~ a large struct copy under Process / PreExecute) | 0.22 | unidentified |

Cross-cutting costs inside the above (self / site view, `pcsites`): `operator new` call sites 2.5 % of the thread (0.16 ms: `std::list` / map nodes, material pack adopt, RT registry; `MaterialPack::lookup` string work 0.15), `safeRead` / `safeLoad8` SEH-guarded engine reads 0.16 + 0.12 (memcpy 1.9 %), driver recording `GameRecord::applyPacket` 2.2 % (0.14), `Image::transition` 0.7 %, record-thread hand-off (`scopeChunk` notify 0.6 %, `MaterialPack::adopt` mutex unlock 0.8 %).

**Wrapper reference** (5.75 ms): Engine.dll 1.46 (its own `eCSubMesh::Render` 0.38, `RenderLeafs` 0.59, `RenderFronds` 0.29, sky cube 0.19, particles 0.15), the HAL / D3D9 model 2.3 (`drawIndexedPrimitive` 2.25, `prepareDraw` 1.8, `MaterialPack::lookup` 0.37), `eCProcessor::Process` 0.82. Full is 0.75 ms slower than Wrapper in this scene because of the native frame (composition of all groups, light table, RT scene) even though it issues 815 draws instead of 1,286: Full pays ~3.3 ms in `opEnter` where Wrapper pays 2.3 ms in the HAL + ~1.5 ms in the engine's render code.

**Owned arm** (the whole-frame check of 7.16 / 7.21): `-Perf` CPU Full 6.4, tables only (the F1 / F2c chain without owned_static) 6.9 / 7.3, + owned_static + owned_persistent 6.6, + billboards + `rt.table_entries` 7.1 / 7.4 (noise +-0.3). The tables (windows, objects, records, check frames every 10th) cost ~ +0.6 - 0.7 ms; owned_static + persistent win back ~ -0.5; billboards and the RT entries add ~ +0.6 again. Net of the whole chain at the savegame: **+0.7 ms (+11 %), not a gain.** Visible in the profile as new inclusive entries: `Device::sceneFlushRun` 0.20, `checkMaterial` 0.20, `ownedPass` 0.16, `writeLighting` 0.15, `submitOwned` 0.12, `scene_hash.h` hashing 0.12 (`hashVi` 0.07), `MaterialTable::checkDraw` 0.16, `flushBatch` 0.25. The owned path draws 39 % of the static ops (the rest: hardware batch 7k, instance list 14k, hierarchy 44k per 5 s), and the SpeedTree / vegetation / shadow-view composition (the biggest groups) is unchanged. pub_interior (Full defaults) is 4.9 ms: d3d9 2.75, Engine 0.59, same shape.

**Classification** (Full defaults, savegame; ms / frame; estimates are each stage's own remaining cost, not strictly additive):

| class | item | now | removable by | est. gain | note |
|---|---|---|---|---|---|
| removable | SpeedTree + billboard + tree Z compose / issue (slots 7 - 10) | 1.29 | F4 (trees GPU-driven, 7.15; billboards F4-1 exists but are 0.37 of it) | -0.8 .. -1.1 | biggest single block |
| removable | shadow views' op compose / issue (`ExecuteShadowMap` 0.57, inside the mesh / tree Z slots) | 0.57 | F3 (static + trees in cascades / terrain map) | -0.3 .. -0.45 | the large part needs F4 first |
| removable | RT scene observe + record | 0.57 | F8 (TLAS from the tables; `rt.table_entries` alone did not pay: the compose of its inputs must go too) | -0.3 .. -0.45 | |
| removable | vegetation compose (slots 11 / 12) | 0.43 | F7 | -0.25 .. -0.35 | repack of 410 KB / frame |
| removable | static mesh ops (slots 0 / 3, main view) | ~1.0 incl. issue | F2 completion: hardware batches, instance lists, hierarchy meshes (60 % of the ops are still not owned) | -0.3 .. -0.5 | `owned_static` today: 39 % of ops, net ~0 |
| reducible | light table collection (`LightTable::record` + `lights::collect`) | 0.50 | persistent table + dirty signals instead of a per-frame walk / sort | -0.25 | light_table.cpp, not an ownership stage |
| reducible | allocations (operator new 0.16, ntdll heap 0.25), `safeRead` / `safeLoad` 0.3, `MaterialPack::lookup` 0.15 | ~0.8 | arenas / reserve, unguarded reads on validated ranges, key cache | -0.3 .. -0.4 | plain engineering, all modes |
| reducible | effects / particles / strips / rain composition (CPU vertices) | ~0.5 | stays CPU by design (README 14); sky cube stride | -0.1 | |
| removable, high risk | engine collection: `OnQueryNodes`, `Request`, `RequestRenderOps`, `ExtractFromFrustum`, `CullCamera`, beam tree, sorts | ~0.75 | F10 (GPU cull, residency from read-back) | -0.4 .. -0.6 | residency side effects; last; the sorts (0.10) go with F2 / F4 when no op exists |
| stays engine | `eCProcessor::Process` (logic, scripts, AI, animation, physics, streaming, audio) | 0.65 | - | 0 | `Game` / `Script` / `fmod` self < 0.2 |
| stays engine | cascade fit / camera update, post chain control, GUI, message loop, the unidentified struct copy 0.22 | ~0.8 | partly (cascade fit: `shadow_stride`) | ~0 | |
| stays (driver / API) | amdvlk64 recording | 0.36 | fewer, larger indirect draws (all stages above) | -0.1 | |

Floor of the engine thread if every removable item went: game logic 0.65 + engine-side residue (cascade fit, post control, GUI) ~0.8 + driver ~0.3 + particle / effect composition ~0.4 + per-frame fixed pass recording ~0.4 = **~2.5 ms of today's 6.4 ms**; the ownership stages in the order below realistically reach ~4.0 - 4.5 ms.

**Ranked next steps** (CPU win at the savegame): 1. **F4 trees** -0.8 .. -1.1 ms (largest block; the owned machinery exists for billboards); 2. **F3 shadow views** -0.3 .. -0.45 (after F4, so trees are in it); 3. **F8 RT from tables** -0.3 .. -0.45 (needs the compose of its inputs gone first); 4. **allocation / guarded-read / pack-lookup cleanup** -0.3 .. -0.4 (cheap, no ownership needed, helps every mode); **result (2026-11-07, performance.md 33): ~ -0.2 ms Full (heap -0.17, pack lock / strings gone); guard inlining is impossible (MSVC does not inline `__try`), the guarded reads need block reads instead;** 5. **F7 vegetation** -0.25 .. -0.35; 6. persistent light table -0.25; 7. F2 completion (hardware batch / instance list / hierarchy) -0.3 .. -0.5; 8. F10 -0.4 .. -0.6 (last, high risk). Before any more stage: the tables' own fixed cost (+0.6 - 0.7 ms with `native.owned_*` + `rt.table_entries` on) has to be paid for by the stages that follow; the current chain is a net loss until F4 / F3 land.

**Method limits.** The sampler lists only entries >= 1.5 % (inclusive) / ~1 % (self), so nesting and sums of children are read from named frames and are approximate; the mesh / tree Z slots include their shadow-view calls (main vs shadow split only via `ExecuteShadowMap`); Wrapper has 10 usable windows (its first run hit a slow load: 2 windows); pub_interior is a single run with 4 windows; profiler and `-Perf` timers cost ~0.4 ms (performance.md 21); run-to-run noise of `-Perf` CPU is +-0.3 ms, so the ablation differences are indicative only; the Owned arm includes check frames (every 10th); unnamed Engine.dll functions are named by COMDAT hints (the Ghidra project open in this session is Risen.exe, not Engine.dll: no new RE was done); GPU numbers were not examined.

### 7.25 F4-2/F4-3 owned SpeedTrees (2026-11-07, `native.owned_trees`; correct, CPU -0.2 ms, GPU worse: NOT a win yet)

Debug setting (Bool, Full, default off, `needs` native.owned_persistent so the whole F1 / F2c chain). Files: `src/native/tree_owned.h` (templates, stamps, plan, stats), `src/native/static_ops_trees_owned.cpp` (all of it), `src/d3d9/owned_trees.cpp`
(`Device::ownedTreeFlush`, `treeEnvEpoch`, `ownedTreeObserversOk`), hooks: `static_ops_trees.cpp` composeTree (`treeCompBegin / Batch / Instance`, the ramp), `static_ops.cpp` (`treeCompEnd` after `bbEnd`, `treeSceneIssue`, `treeReport`),
`frame_tap.cpp` (`treeViewBegin`, tree ops in `compactOwned` -> `treeTake`, `treePass` in `passBegin`), `scene_draws.cpp` `sceneDecide` (SpeedTree draws get their scene ABI v2 form ONLY: `scenePipe_` stays None, the composed packet is not a scene packet: its instance count would be lost),
`IllumTable::ensureSlot`, `DrawDesc::treeKey`, test `rvk_d3d9test` "owned trees (F4-3)".

* **F4-2 templates + plan.** Per (op ctx, desc, list) an `OpRec` with one TEMPLATE per bucket = (type, z / solid, LOD geometry (strip + start | leaf LOD + start + indices), sun bit): the composed draw of the bucket (retained packet from the packet cache,
  `orec::DrawRec`, a copy of the `DrawDesc`), made on every composed frame of the op (kept while its stamp holds: render buffer, resource, material, shader, visual, transformation, variant epoch, registry generation, hardware / table-merge). Only
  hardware-instanced ops with exactly one draw per batch qualify (that is every tree op seen: 77 ops / 129 draws; 0 software). `treePlan` plans every instance of the queue from the engine's tree objects (one `ObjSnap<0x70>` + the entity matrix + the
  illumination snapshot per tree; LOD fields from one 0x138-byte snapshot of the render buffer; leaves LOD-major) = the row (c32 = tree +0x6c / rb +0xa0|0xa4 or wrapper +0x34, alpha 80 / ratio; c33..c35 = the world matrix columns) + the bucket key.
  Composed frames compare the plan with the composition (instance order, tree, bucket key, row bits) and the new draws with the retained templates; `MISS` = a persistent template block differs from a fresh derivation although its signature / epoch held.
  Result: **rows 0 differ, counts 0, MISS 0 over millions of instances** (savegame hold 52 k ops / 472 k instances per 5 s; jungle). Two known non-fatal differences, reported separately: (a) the engine draws a whole batch with its FIRST instance's start index
  (batches cut on strip length only), the plan gives each instance its own LOD: 0.25 % of the instances at the frozen jungle spot (D3D12 run: 326 of 127,911) = a picture difference of the owned path (the instance is drawn with its own, correct LOD; the engine's
  is a bug-compat quirk we do not reproduce); (b) a template whose draw changed (a texture streaming in: not in the stamps) is replaced and counted (2 per run).
* **F4-3 owned execution (main view, z + solid).** `FrameTap::compactOwned` takes tree ops with a template record out of the lists at the view's beginning (check / warm-up / gated frames compose as before). `treePass` (at BeginZPass / DoBeginRenderSolid) per op: stamp +
  view stamp + material poll (`pollOwned`, structure version), the engine slot's side effects (`leanTreeSort`, `leanMaskRef`, touches), the plan, per-instance lighting entry in the solid pass (`IllumTable::ensureSlot` + `LightingImages::entryFor`), templates made ready once
  per pass (the owned_persistent recipe: `ownedBuildWindows` with a dummy object window, `ownedDrawSig`, `ownedDerive`, DrawBlock in a `drawrecords()` slot, rolling scrub 1 / 64); an op that cannot be drawn this way (no record, stamp, view, material, plan, instance with no
  light list / static ambient / dynamic or unknown static lights = "special lighting", no template, template refused, flush refused) is composed right there (`composeLate`). Then ONE ring block of rows (c31 header + 64 B per position; VS window 2 base = the block, c32 = row 0)
  and, per run, one `DrawRecord` (the template's slot record with `vsWin[2]` = the rows, `psWin[3]` = the instance's lighting entry): z = one record per template run, solid = one per run of equal lighting entry; `Device::ownedTreeFlush` writes the ref list (record index repeated over the run's
  positions, view slot) + arguments `{indexCount, instanceCount = run, firstIndex, vertexOffset, firstInstance = first position}` and queues one indirect call per bucket. **The instance index of the tree VS is the engine's own mechanism:** stream 1 is an instance-rate identity
  ramp, so `vInstance = firstInstance + i` = the row position; the composed tree draws use OUR ramp buffer (16,384 floats, `treeEnsureRamp`) instead of the engine's 256-float one (same values, so composed pictures are unchanged); passes with more than 16,384 instances compose.
  View: fixed view table slots 14 (z) / 15 (solid) carry the tree view (c0..c3 = view-projection, c14..c29 wind from `treeWind`, c30 fog); trees are owned only when `Device::ownedTreeObserversOk` (no observer wants main-view SpeedTree draws).
* **Gate that limits it: the RT scene.** `RtScene::observes(SpeedTree, Main)` is true whenever any consumer holds the RT scene (at the player defaults: water RT / material RT / ...). Then trees stay composed (`observers ok false`). Owned trees therefore act only with `-Setting 'rt.allow=false'` (or
  no RT consumer) until F8 supplies the RT scene's tree rows (section 12). All measurements below use `rt.allow=false` (both arms).
* **GPU safety.** CPU guards: ramp bound (16,384), ring allocations, every item's cd / record / window bases / instance sums checked before anything is written, every op decided before its rows are laid out; `rvk_d3d9test` "owned trees (F4-3)" on **Vulkan and D3D12 with `--validation`**: a tree-like
  instanced draw (stream 1 ramp, `c32[a0.x]` rows, c12 from the view block) is 0 bytes different from the normal draws for one run of six, three runs of two (one call, three argument records), reversed rows (the picture follows the rows), and a null window base / a wrong position count
  draw nothing; 0 validation errors. Game: savegame hold with `-Validation` + `VK_KHRONOS_VALIDATION_VALIDATE_GPU_BASED=GPU_BASED_GPU_ASSISTED` (owned trees on, 319 of 399 views owned, 49,126 ops drawn, 409 k instances): no VUID, no device loss; jungle likewise.
  D3D12 jungle: runs, 216 of 270 views owned, no device removal.
* **Numbers** (savegame Risen_Save_0005 hold 40 s, `-Perf`, `RisenTest.exe`, vsync off, lod 2x, `rt.allow=false`, shared copy, interleaved A = owned_trees off, B = on; mean of the last 8 `perf:` windows):

| arm | engine + HAL CPU / frame | GPU / frame |
|---|---|---|
| A (off) | 6.17 / 6.16 / 6.17 ms | 1.57 / 1.56 / 1.58 ms |
| B first build | 6.35 / 6.33 ms | 1.99 / 1.98 ms |
| B + flat light memo, miss check 1 in 8 | **5.95 ms (-0.22)** | 2.55 ms (another agent's game shared the GPU in this run: read as +0.4 .. +1.0) |

  Owned view cost (z + solid, 5 s means): side effects 41 us, plan 139, templates 208 (made ready + per-instance lookups + lighting entries), rows 63, flush 37 = ~0.5 ms against ~1.29 ms of composition + issue of the same ops; the composed frames (1 in 10 plus the warm-up frame: 20 % of the frames) still
  pay composition + the plan + templates. **Target -0.8 .. -1.1 ms NOT reached (-0.2 ms).** **GPU is WORSE** (+0.4 ms or more): the solid pass has one indirect command per run of equal lighting entry (245,949 items for 408,958 instances = 0.6 per instance, ~385 per flush) instead of 129 hardware-instanced draws.
* **Not done / open.** (1) GPU: merge runs by lighting-entry CONTENT (many illuminations have equal registers) or keep the engine's illumination cut as the batch key in the solid pass (one record per batch, last instance's lighting: the engine's own picture) before this can be turned on by anyone; (2) the composed frames' share (check every 10th
  frame) and `treeCompEnd`'s `DrawDesc` copies; (3) D3D12 `lod.trees=3`, harbour_docks, `rvk_rtwalk` sway (needs the RT scene: trees are composed then), refcompare Full, picture A/B crops were NOT run; (4) shadow views (F4-4) and persistent per-tree records (F4-5) untouched; (5) software instancing, and ops with an instance whose
  illumination has no list / static ambient / unknown static lights are composed as a whole op ("special lighting": 463 - 1,152 ops per 5 s at the jungle spots, ~4 % of the ops); (6) the RT scene gate above.

### 7.25b F4-3 follow-up (2026-11-07): one command per template run, the RT scene served

Files: `src/d3d9/owned_trees.cpp` / `device.h` (`ownedTreeFlush(items, recs, posRec, ...)`, `ownedTreeObserve`, `ownedTreeObserved`, `DrawObserver::ownedTreesOk`), `static_ops_trees_owned.cpp` (the rows loop), `src/enh/rt_scene.*` (`ownedTreesOk`, the sighting-set diagnostic), `rvk_d3d9test` "owned trees (F4-3)". Private copy `risen-test-vk-f43` instance 29 / shared copy for timings; logs `rvk-work/scratch/f43b/`.

* **GPU fix (7.25 problem 1).** The ref stream is instance-rate (record index, view slot) per list POSITION, so a record per (template, lighting entry) and a record index per position give per-instance lighting in ONE draw without a scene ABI change (v2 unchanged, default shader text unchanged, no `kTranslatorVersion` bump). An item = a template's whole run of positions = one indirect argument record; consecutive items of one device bucket still go out as one multi-draw call. Flush at the savegame (RT on, 5 s window): **73 commands / flush (solid + z averaged) against the engine's 150 composed batches per view = 75 per pass** (jungle spot: 165 per flush against 171), records 0.55 per instance (128 B each), indirect calls = 1 per ~2 items. Before: ~385 commands per solid flush. Test: `rvk_d3d9test` owned trees now also draws six instances with entries {1,2,0,1,2,0}, {0,0,1,1,2,2}, {2 x 6} (PS window 3 = a lighting image per entry, reference = six normal draws, c14 per instance, ramp offset per row): **0 bytes differ, 1 indirect call** each; entries really change the picture; a record index out of range / a null lighting base are refused (2 counted), the next frame is right; PASS 0 failures, 0 validation errors on Vulkan and D3D12 `--validation`.
* **GPU numbers** (`-PerfFrame 3000`, `RVK_PERF_FRAMES=24`, savegame hold, `RisenTest.exe`; "z + solid" = the 1600x900 passes with >= 100 draws; A = `native.owned_trees` off, B = on; shared GPU: another agent's game made some runs noisy):

| arm | A1 / A2 main, z + solid (ms) | B1 / B2 main, z + solid (ms) |
|---|---|---|
| `rt.allow=false` | 1.504 / 1.573, 0.614 / 0.629 | 1.593 / 1.711, 0.638 / 0.683 |
| RT on (player defaults) | 3.130 / 3.260, 1.016 / 1.050 | 3.202 / (2.132, 0.695: A-side contention lifted, not comparable), 1.074 |

  and the `perf:` "GPU per frame" (mean of the last 8 steady windows): RT off A 2.53 / 2.52 vs B 2.59 / 2.57; RT on A 2.15 / 2.16 vs B 2.17 / (3.42, contended). **GPU is now within +0.03 .. +0.05 ms of the composition (z + solid +5 %), no longer +0.4 .. 1.0; not "equal or better": the per-position record fetch of the scene form is the remaining difference (not chased).**
* **RT hand-over (problem 2).** `Device::ownedTreeObserversOk` now accepts an observer of the main view's SpeedTree draws when it says `ownedTreesOk()` (RtScene: `!animated_`) and reads no more than the material windows. Per template run the pass sets the draw tag (SpeedTree, op context / entity / desc, the template's null sub-mesh / material), the register file (saved once per pass, restored at its end) gets the template's CPU window images and the rows as c32 + 4 i (chunks of 50 instances), stream 1 = our ramp, and `Device::ownedTreeObserve` calls `observes` / `coversDraw` / `observe` with the template's retained query (`instances` = the chunk): **the RT scene's existing instanced-batch path unchanged** (BLAS key from the template's buffers, registry key = BLAS + rows, flags / alpha word from the query). A capture request from an observer would be lost (draws are queued): `treeObserveCaptures` counts them, 0 in every run. **Which path is active at defaults:** animated SpeedTrees (`rt_scene.animated_`) are on only while RT shadows (`shadows.rt` with its wind option, default on) or GI foliage ask for them; at the player defaults (water RT / material RT off by default, no RT shadows, no GI) they are off, so the RT scene takes the REST-POSE tree instances only (no vertex capture) and owned trees act with RT on. With `shadows.rt=near` (wind default on) the animated capture needs the composed draw's capture variant: **trees stay composed** (`main views N (0 owned)`; measured, jungle: 261 / 0), as before. `rvk_rtwalk` sway therefore stays on the composed path by design; making the capture work from owned runs needs a capture variant of the v2 pipeline (F8 / F4-6).
* **Compare (problem 2 gate).** New diagnostic in RtScene: per frame the set of SpeedTree instances sighted in the main solid pass (count + order-independent hash of key, flags, alpha word), counted per frame class (trees owned / composed); rvk.log `rt: scene: SpeedTree sightings`. Jungle (rvkcompare spot, frozen game time, RT on, `debug.rt_table_compare`, Vulkan `-Validation` + GPU-assisted): composed frames 63 / 61 / 63 / 56 / 57 and owned frames 253 / 247 / 249 / 223 / 231 per 5 s: **1 distinct set in each class and the same set in both** in every steady window; the windows with an NPC / streaming move show 6 / 6 distinct sets with 3 shared (both classes change together). D3D12 jungle with `lod.trees=3` (debug layer, `RVK_NO_GBV=1`): 1 / 1 / 1 likewise (one window with 2 + 9 distinct while things moved). Owned check frames: DIFFERING 0 (rows 0, counts 0, templates 0), MISS 0 of 3460 compared; static `rt_table_compare`: key / rows / flags / data 0, 0 missing. `rt.debug=depth` probes were not rerun (the TLAS tree set is identical by the hash above).
* **GPU safety.** First game run Vulkan jungle `-Validation` + `GPU_BASED_GPU_ASSISTED`, owned trees on, `rt.allow=false`, then RT on (`shadows.rt=near`: trees composed; RT at defaults: owned + observers): no validation message, no device loss, no `[error]` except the known non-fatal per-instance-LOD "DIFFERING" lines (see below). D3D12 jungle debug layer: no errors (the 1477 PSO warnings are pre-existing; the GBV run is too slow to finish).
* **CPU** (savegame hold 40 s, `-Perf`, `RisenTest.exe`, vsync off, lod 2x, mean of the last 8 steady windows; engine + HAL per frame):

| position | A (trees composed) | B (owned) |
|---|---|---|
| RT off (`rt.allow=false`) | 6.12 / 5.99 | **5.86 / 5.81 (-0.22 ms)** |
| RT on (player defaults, `rt.table_entries`) | 6.55 / 6.66 | 6.55 / 6.41 (-0.0 .. -0.25) |

  Owned view (RT on): side effects 41 us, plan 144, templates 217, rows + observers 173 (the observers' ~130 us were paid in the composed draws before), flush 26 us. With RT on most of the saving is eaten by the RT scene's own per-instance work, which both paths pay.
* **Check frames (problem 3).** `native.owned_check` 10 (default) vs 60 with owned trees on, RT on, alternating, A (composed trees, check 60) as control: B10 6.58 / 6.58 ms (587 of 735 views owned), B60 6.35 / 6.55 ms (733 of 758 owned), A60 6.56 / 6.52 ms. The period moves the owned share from 80 % to 97 %, the CPU by 0.03 .. 0.23 ms (the two pairs disagree: inside the +-0.1 ms noise). The knob is shared with the static ops (F2c); defaults unchanged.
* **Per-instance LOD start (problem 4, decision).** Kept per instance (the engine draws a batch with its first instance's start index, 0.25 % of the instances here: 520 / 610 per 5 s of ~130 k compared): an accepted Full picture change, reported as `per-instance LOD start differs`, not counted as DIFFERING. `lod.trees=3` ran on D3D12 (above) with no refusal. **Not done:** harbour_docks D3D12 (the first attempt with GBV timed out), refcompare Full, `rvk_rtwalk` (composed by design with wind), picture A/B crops toggling `native.owned_trees` inside one hold (tree lighting may differ only where the engine merged a batch under one illumination).
* **Open.** (1) GPU +0.04 ms: fetch of the record per position; merging records by lighting content would shrink the record table only. (2) Animated RT trees from owned runs (capture variant). (3) The list above; (4) shadow views (F4-4) and persistent per-tree records (F4-5) untouched.

### 7.26 Owned chain tally (2026-11-07, main 37a4564: F2c-4a / F2c-4b / F4-3 trees / performance.md 33; measurement only, no code change)

Question: is the whole owned chain a net CPU gain in Full yet, and what does each layer cost / save? **Answer: no. Full defaults 5.90 ms, the whole chain (arm A) 6.77 ms = +0.88 ms (+15 %); the loss is the table / records layer (+1.07), the owned draws win back -0.32, trees + billboards add +0.12 (noise-level).** Recipe: savegame `Risen_Save_0005`, `RisenTest.exe`, vsync off, `-Perf`, `-PlayerDefaults`, `lod.view / trees / meshes = 2`, `hold=60`, shared copy + default lock (the copy still lives at the old `B:\DevDrive\risen-test-vk`), 2 rounds interleaved W F T S A, mean of the steady 5 s `perf:` windows of each run (world windows with GPU > 1 ms, the first one dropped: 11 - 13 per run); logs + `ana.py` / `drive.ps1` in `rvk-work/scratch/tally/`. `-Setting` does not apply `needs`, so every prerequisite is set explicitly:

* **W** `-Mode wrapper`. **F** `-Mode full`, defaults (table lighting on, packet cache, lean).
* **T** F + the table chain without owned_static: `native.illum_table, material_table, lighting_images, owned_windows, owned_objects, owned_records, scene_draws`.
* **S** T + `native.owned_static, owned_persistent` + `rt.table_entries`. **A** S + `native.owned_trees, owned_billboards`.

**Verified acting** (last 5 s window of each run): T: owned windows draws 545 k per 5 s (786 / frame), owned objects served VS2 132 k, lighting images 114 entries / 9,217 compose passes, owned records 54,054 eligible ops derived and compared **every frame** (no owned_static: nothing submitted from them), no `owned static` lines (stage off, as intended). S: `owned static` 718 main views (574 compacted, 72 check frames, **0 gated off**), owned 44.8 k of 114 k ops (39.2 %), `owned persistent` reused 92.6 %, MISS 0, DIFFERING 0; `rt: scene table` entries 548, covered draws skipped 90 %, `observeOwned` 1,175 calls (66 records / frame). A: additionally `owned trees` 719 main views (575 owned), 53.9 k tree ops taken, 54 k drawn from templates, composed late 0, 73.0 commands / flush, DIFFERING 0, MISS 0; `owned billboards` 4,176 of 4,176 recordable ops (100 %), DIFFERING 0. Nothing was gated off or refused in any arm (the only refusals: 573 `replay refusals: texture` per 5 s in S / A, composed late as designed). RT stays on in every arm (`rt.allow` default): in A the RT scene is served by `ownedTreeObserve` and trees are owned.

**Per arm** (mean of the two runs; run 1 / run 2 in brackets; engine + HAL CPU per frame; GPU = `perf:` GPU per frame, see the noise note):

| arm | CPU ms / frame | GPU ms / frame | fps | injected-pass CPU (ms) |
|---|---|---|---|---|
| W | 4.96 (5.05 / 4.86) | 2.21 / 2.55 | 190 | 0.21 |
| F | 5.90 (5.93 / 5.86) | 2.11 / 2.74 | 161 | 0.54 |
| T | 6.97 (7.04 / 6.90) | 2.16 / 2.33 | 138 | 0.55 |
| S | 6.65 (6.66 / 6.64) | 2.18 / 3.08 | 145 | 0.59 |
| A | 6.77 (6.70 / 6.84) | 2.11 / 3.04 | 142 | 0.58 |

**Deltas** (CPU, ms / frame; mean, per-round range):

| delta | what it adds | CPU | GPU (round 1 only) | fps |
|---|---|---|---|---|
| F - W | the native frame (Full vs Wrapper) | **+0.94** (+0.88 .. +1.00) | -0.10 | -28 |
| T - F | tables: material / illum / lighting images, windows, objects, records (all derived + compared every frame) | **+1.07** (+1.03 .. +1.11) | +0.04 | -23 |
| S - T | owned_static + owned_persistent + rt.table_entries | **-0.32** (-0.26 .. -0.38) | +0.03 | +6 |
| A - S | owned trees + billboards | **+0.12** (+0.04 .. +0.20) | -0.07 | -3 |
| A - F | the whole chain | **+0.88** (+0.77 .. +0.98) | 0.00 | -19 |

Run-to-run noise of the CPU mean: 0.03 .. 0.15 ms between the two runs of an arm, window sd 0.02 - 0.24. **GPU: round 2 was contended by another agent's game** (F 2.74 / S 3.08 / A 3.04 against 2.11 / 2.18 / 2.11 in round 1; T 2.33, W 2.55 less so): use round 1; there the GPU of all arms is within +-0.1 ms (2.11 - 2.21), so no layer costs GPU at this scene (trees' per-position record fetch +0.03 .. 0.05 of 7.25b is inside that). The CPU runs are not affected by the GPU contention (vsync off, CPU-bound, per-arm sd small), but fps follows the CPU numbers.

**What each layer costs / saves** (owned lines of the last windows, per main view; ~1 main view per frame):

* **Tables + records (T - F = +1.07):** the largest item. T derives and compares every eligible op of the main view each frame (54 k ops per 5 s = ~75 per frame) and submits nothing derived, plus the windows (material / view / object / lighting windows served from tables, 786 draws per frame, with `uploaded` still 365 k / 5 s for the other windows), the object table, the lighting image compose passes (9.2 k per 5 s = 64 per frame, 114 entries each) and the per-frame material polls (779 k per 5 s). Without owned_static this is pure overhead by construction (`owned_records` derives and compares; nothing derived is submitted); S shows the same tables paying back.
* **owned_static + persistent + rt.table_entries (S - T = -0.32):** owned static per main view (S run 1): compaction 51.6 us (of it stamp / record checks), view + windows setup 2.6, material polls + stamps 7.4, build 102.7, side effects 4.1 = **~168 us ~ 0.17 ms / frame** (A: 177 us), `owned persistent` flush 8 us; it draws 39.2 % of the ops from records (the rest: hardware batch 7.5 k, instance list 14.9 k, hierarchy 47 k per 5 s), 92.6 % of the persistent records reused. The saving it buys is the composition + issue of those ops (~-0.5 .. -0.7 with the records no longer compared) and the RT entries' observation (`rt.scene @AfterDepthPrepass` 0.15 -> 0.18 .. 0.19 ms: the record pass costs +0.04, the per-draw observation it replaces is not timed separately; covered draws skipped 90 %, `observeOwned` 66 records / frame). The two layers cannot be split here (S sets them together; 7.24's ablation put static + persistent at ~ -0.5 and billboards + RT entries at ~ +0.6).
* **Trees + billboards (A - S = +0.12, inside the noise):** owned trees per owned view (575 of 719 views = 80 %; the rest are check / warm-up frames that compose): side effects 41 us, plan 142, templates 215, rows 177 (observers included), flush 25 = **~600 us per owned view ~ 0.48 ms / frame**, against the ~1.3 ms the composition + issue of these ops cost (7.24): the saving lands at ~ -0.4 .. -0.6, eaten by the 20 % composed frames (compose + plan + templates) and by the billboard record (`owned billboards` is 100 % valid, no composition on owned frames). 54 k tree ops / 779 k instances per 5 s become 73 commands per flush against 150 composed batches per view (GPU neutral, above). 7.25b's A/B on trees alone (RT on: -0.0 .. -0.25) agrees.
* **F - W = +0.94** (7.24: +0.75): Full still costs about a millisecond more than Wrapper at this savegame even before the tables (light table 0.33 + 0.15 RT scene pass in `injected pass CPU` 0.54 against Wrapper's 0.21; the rest the native composition).

**Verdict.** The owned chain is **not** a net CPU gain: **+0.88 ms (+15 %, 161 -> 142 fps) over Full's defaults**, +1.83 ms over Wrapper (4.96 -> 6.77), GPU unchanged. The tables' fixed cost (+1.07) is not yet repaid: owned_static (-0.32 incl. RT entries) and trees / billboards (+0.12 .. ~ -0.2) recover about a third of it. Every layer is correct (DIFFERING / MISS 0, nothing gated off). What it needs to pay off: (1) make the records check-only on check frames (derive + compare every frame without owned_static is the T arm's +1 ms, and that is also what `owned_check` frames in S / A pay: 10 % of the frames); (2) the table maintenance per frame (material polls 780 k / 5 s, lighting image passes, object table) must become dirty-driven; (3) the stages that remove composition (hardware batches, instance lists, hierarchy = 60 % of the static ops; shadow views F3; vegetation F7) before the tables are worth their cost. Until then the chain stays Debug / off by default (it is), Full defaults are the best arm.
**Method limits.** One scene (the savegame), 2 runs per arm, CPU noise +-0.1 .. 0.15 ms between runs, GPU of round 2 unusable (other agent's game); S - T mixes owned_static / persistent with rt.table_entries; the A arm keeps `native.owned_check` 10; the per-layer microsecond lines are the engine thread's own timers (fine timers off, `RVK_F2C4_MEASURE` not set).

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
| F4-2 | tree records + per-view plans, compared | DIFFERING 0, audit 0 | +0.05..+0.1 (built, 7.25: DIFFERING 0) |
| F4-3 | CPU-owned tree execution main view (compaction, rows, records, args, side effects, check frames) | check frames 0, refcompare Full, jungle spots, rtwalk sway, lod.trees=3, both backends, validation | -1.0..-1.4 (built, 7.25 / 7.25b: -0.22 ms CPU with RT off, ~-0.1 with RT on, GPU z + solid +0.03..0.05 ms, commands <= the engine's batches, RT scene served except animated trees; open: refcompare, rtwalk with wind, picture A/B) |
| F4-4 | tree shadow views (cascades, z) | shadow-compare | -0.15..-0.3 |
| F4-5 | persistent per-tree records (UpdateLoD post-hook), GPU compaction | compaction = CPU list | -0.05..-0.15 + GPU |
| F4-6 | owned tree shaders (leaf PS wired: `uberSupportsFile`; vertex pulling; graph-shared pipelines) | equality gates | calls ~10-40 |

## 11. F2c-4 design: the CPU no longer touches individual owned draws (2026-11-06, accepted by the orchestrator)

Starting point (7.16): the owned path costs ~3.4 us per draw vs ~1.7 us for the packet-cache replay it replaced; F2c-4 must remove the whole
owned path (281 us at the savegame, ~990 us at pub_interior) to pay. The section 9 F2c-4 row (-0.03..-0.05) is superseded.
* **Persistent DrawRecords** (DrawRecordTable 128 B x 64K, slot reuse deferred by frames-in-flight + 1) + a persistent **DrawBlockTable** (208 B
  slots). Per-frame inputs move out of the record: **ABI v2** (`TranslateOptions::sceneView`, own variant slots, default text unchanged):
  `ref.y` = a fixed ViewBlock2 slot (640 B: VS c0..c11, PS c0..c4, VS c12..c30, posFix incl. jitter + depthRemap, PS c5.xyz, flags; fixed slots
  0 main z, 1 main solid, 2-3 dbvp, 4-7 cascades, dynamic slots after); `rvkVsWin(0/1)` / `rvkPsWin(0)` computed from `pc.views + ref.y x
  stride` (positive offsets only); `rvkLoadDraw()` takes posFix / c5.xyz from the view, c5.w = the record's mask bits; VARY14 = (viewSlot << 24)
  | record. Lighting entries pinned by a per-frame `seen` mark + an entry generation in the record.
* **Bulk validity**: a dep table (global env epoch per pass; per entity: overlay flags, illum ptr, object slot / generation; per ctx: LOD
  resource, fade, switch; per resource: MeshPart generation, lightmaps; per material: structure version; per illumination / light set) polled
  once per dep per frame; records are invalidated through reverse lists; per draw a 1-byte valid read. Check frames run the full stamp + read
  log for every visible record (MISS counter, auto-off like DIFFERING) + a rolling scrub of 16 records per frame + a fault-injection debug knob.
* **Compaction** (4c): one compute dispatch per present in the upload list (as the lighting compose), job per (view, list), per (job, bucket)
  stable wave-prefix compaction of the visible id list into args / ref / count in a device-local per-frame arena; bucket = (scene pipeline, vi,
  dyn, ib, frame block) with stable ids + caps; one `DrawIndexedIndirectCount` per hit bucket (new DrawPacket op; zero-filled tail fallback
  without the cap). Order inside a bucket = engine order (stable); across buckets = bucket id (visible only for coincident EQUAL geometry:
  re-run the picture A/B).
* **Observers in bulk**: `DrawObserver::observeOwned(Device&, const OwnedFrame&)` once per frame (views with visible ids + `entered` / `left`
  deltas, per-record immutable views: query, tag, textures, object rows + version, view blocks; default = the old per-id loop). **This is also
  the F8 hand-over** (section 12): RtScene derives its table upserts / drops from entered / left.
* **Side effects batched** per resource / material / primitive per frame (first visible record does the touch), after checking that
  `RequestData(res, 1)` is idempotent per frame (disassemble first).

| stage | content | gate | CPU (savegame / pub_interior, est.) |
|---|---|---|---|
| F2c-4-0 | ABI v2 + the F2c-3 owned path on v2; measurements (main views per frame, posFix / c5 per frame, the `submit` split derive / packet / observers, desc vs LOD, RequestData idempotence, buckets, sort share of compaction) | rvk_tests, d3d9test v2 cases, parity_scene v2 == v1, corpus / DXIL v2 | ~F2c-3 |
| F2c-4a | persistent records + dep epochs + CPU-written args (one indirect per hit bucket, exact CPU count) | check frames DIFFERING 0 + MISS 0, A/B, validation, long tour | ~105 / ~400 us (from 281 / 990); **built (7.21): -20 % of the owned path, 231 -> 185 / 354 -> 282 us; the observers + stamps are what is left** |
| F2c-4b | bulk observers (`observeOwned`) + batched side effects | TLAS set equal on check frames, rtwalk, water floor | ~25-30 / ~95-120 us: -0.11 / -0.9 ms vs the pre-F2c replay; **built (7.23): owned path 189 -> 160 us (savegame) / 421 -> 346 us (pub_interior) with the RT scene on (-15 / -18 %), RT observe -0.07 / -0.12 ms, RT pass +0.05; the target is NOT reached (the pre-filter in the persistent loop gives most of it; compaction stamps, build and the 32-frame relearn are what is left)** |
| F2c-4c | GPU compaction + `DrawIndexedIndirectCount` (F5 prerequisite) | rhitest compaction case, read-back DIFFERING 0, sync validation, D3D12 GBV | -10..-15 us CPU; GPU 10-30 us (680M 50-100) |

## 12. F8 design: the RT scene from the GPU scene tables (2026-11-06, accepted by the orchestrator)

Findings: the tables hold no entity -> parts relation for never-drawn entities (parts are created on first sight; the link is the OpRecord), so
"off-screen" coverage = every entity ever sighted and still cached in, dropped by signals (cache-out, delete, bufferChanged), not by age;
never-sighted ones need an engine visual enumeration (F10, not now). The BLAS key is derivable from `PartCpu` + buffer generations (no strips:
SpeedTree is never a part), so table-built and observed instances share `blasByKey_` (BLAS builds, proxies, budget unchanged). Rows come from
`scenetable::slot(i).world` on the CPU (identity (slot, generation, part), moved = slot.version, no quantised keys). Materials at hits are per
material record except the uv map (evaluated on record change from the record's pools + a representative VS / PS; the table needs a CPU
`Texture*` per sampler slot + the last VS so RtScene keeps its layout / release checks).
* Interface: persistent `RtStaticEntry` (objSlot / gen, opDesc, part / gen, matSlot / structVersion, entity, viewMask, rank) maintained by deltas
  (from `OwnedFrame` entered / left once F2c-4b exists; before that learned from the first full observation); `DrawObserver::coversDraw` pre-filter
  in the observer loops (prepareDraw, replayDraw, replayOwned) skips query building for covered (entity, opDesc); `record()` takes a merged list
  (table entries + registry) through one shared `emitInstance`; no duplicates: covered draws skip observe, a `tableOwners_` guard in
  `selectForTlas`, the observed instance removed on migration. Stays observed: characters (F6), SpeedTrees (F4), instanced batches, grass, glass,
  water, blended layers, fade < 1, hierarchy meshes, everything in Wrapper.
* Risks: parts idle-free after 1800 frames (pin parts with live entries), more BLAS alive (check `rt: scene: lod bands`, cap by radius), uv-map
  animation (dirty only on uv-slice constants), stale addresses in persistent entries (rebuild the BLAS key per frame from buffer generations).
* Verification: `debug.rt_table_compare` on check frames: table set vs observed set (missing = DIFFERING, extra = off-screen, per-pair material
  fields), auto-off; `rt.debug=depth / instances` probes; lamp / RT shadow and water A/B; teleport soak with crash markers; GPU-safety steps.

| stage | content | gain (est.) |
|---|---|---|
| F8-0 | persistent entries learned from observation, rows from the scene table, signal aging, `coversDraw` pre-filter, merged selection, owner guard (no F2c-4 dependency) | -0.15..-0.25 ms CPU |
| F8-1 | materials from records (`RtMaterials::fromRecord`) | -0.02..-0.05 |
| F8-2 | discovery from `OwnedFrame` entered / left (needs F2c-4b) | -0.05..-0.1; **built (7.23): `RtScene::observeOwned` skips covered records by liveness + slot stamp; RT observe -0.07 / -0.12 ms measured (with the pre-filter); the material "ambiguity" of F8-0 did not occur (guard built, never fired): the residual 1 - 7 x 10^-5 material differences are engine-graph state changes bounded by the 32-frame relearn** |
| F8-3 | cascade coverage (after F3 shadow-view records) | the larger share of the 0.376 ms |
| F8-4 | optional GPU instance assembly | only if profiled |
Measure first: observer calls per view / pass / class, observe vs query-building ms, registry composition by class, the share of instances
whose entity has an ownable OpRecord, the cascade-only (off-screen) share.

## 13. F5 plan: measure first, then cluster culling (no cluster LOD) (2026-11-06, accepted by the orchestrator + user)

User decision 2026-11-06: **cluster LOD (Nanite-style simplified cluster hierarchies) is not planned**. Risen's geometry is from 2009, and the
meshes it would help simplify badly: SpeedTree leaves and alpha-tested cards, skinned actors, meshes whose lightmap UVs and vertex colours carry
baked light. RT gets nothing from it on AMD (the caps report: no cluster acceleration structures on the RX 7900 XTX). Revisit only if F5-0 shows
that far, sub-pixel triangles cost real GPU time. **LOD selection itself** moves to the GPU inside the F5 cull pass (per instance: distance ->
the LOD's index range in the indirect args; never in the vertex shader, which cannot change a draw's vertex count). The engine's resource cache
must still see its touches: either every LOD of an in-view object stays resident, or the GPU's choices are read back a frame late and touched per
resource in the batched side effects of F2c-4b, as GD3D11 does for its texture streaming. Measure the memory of "all LODs resident" before
deciding the 680M default. Until F5 the engine's LOD choice stays: it is a dep of the F2c-4a records (7.19 measurement 4: 27 LOD-resource
changes in 2.5 M context-frame pairs).

**Model: GD3D11's GPU world** (`E:\git\github\GD3D11`, `Shaders/D3D12/WorldCull.hlsl`, `WorldObjects.h` `MeshCluster`,
`WorldConverter::ClusterWorldMeshTriangles`, `SpatialBVH.h`). The world mesh lies in XZ grid sections, and each mesh's triangles are split into
~128-triangle spatial clusters by a BVH. The index buffer is reordered so every cluster is a contiguous range (the shadow indices are derived
after clustering, so they stay valid). Per view a compute pass culls the sections, then the meshes, then the clusters (a groupshared visibility
bit mask of up to 2,048 clusters per mesh; beyond that the rest join the last run unculled). A **run of contiguous visible clusters becomes one
indirect command**, so hidden clusters drop out without multiplying draws.

| stage | content | gate | gain (est.) |
|---|---|---|---|
| **F5-0** measurement (no behaviour change) | at the player defaults (savegame, harbour_docks, jungle, pub_interior): triangles / vertices submitted per view kind (main z, main solid, each cascade) and per mesh-size class (pipeline-statistics queries where offered: input primitives, clipping primitives, fragment invocations; otherwise index counts on the CPU); the share of each large mesh's triangles outside the frustum and back-facing (offline from cluster bounds + normal cones built from the mesh arena's copies); a triangle screen-size histogram (a debug pass or offline from captured positions); `-PerfFrame` GPU time of the depth pre-pass + cascades vs the pixel-heavy passes; memory of "all LODs resident" vs today. A 680M run if the hardware is available, otherwise extrapolate from the 7900 XTX and say so. | numbers in 7.x | decides F5-1..3 |
| **F5-1** cluster build | static meshes above a size threshold (level meshes first; set the threshold from F5-0): ~128-triangle spatial clusters (`meshopt_buildMeshlets` or a BVH split as GD3D11), the **mesh arena's copy** of the index range reordered so each cluster is contiguous; z pre-pass, solid pass and cascades all use the same reordered indices, so the solid pass's depth EQUAL stays invariant; per cluster: bounds + normal cone in a GPU cluster table (the GpuTable pattern); cached on disk keyed by mesh content | per sub-mesh, the index multiset is equal to the engine's; picture A/B 0 beyond noise; rt / ocean floor / captures still see valid offsets | none by itself |
| **F5-2** cluster cull (after F2c-4c) | per view (main + cascades), per visible owned record of a clustered mesh: frustum + back-face cone per cluster, runs of visible clusters merged (cap of runs per record, else the whole range), args written into the F2c-4c arena, `DrawIndexedIndirectCount`; per-instance GPU LOD selection in the same pass (see above) | GPU-safety steps; read-back verify on check frames (a cluster drawn by the engine but culled by us = a false negative, must be 0 for the conservative tests); bursts while turning (no pop-in); sync validation; D3D12 GBV | vertex + raster work of hidden parts of large meshes (680M first) |
| **F5-3** HiZ occlusion | same-frame two-phase HiZ from the main depth pre-pass (section 2's F5 row), per cluster and per small mesh | as F5-2 + pop-in bursts | depends on occlusion (old probe: 8-24 % hidden) |

Only opaque / z / shadow-caster draws are clustered. Alpha-blended draws keep their engine order and their whole range.
