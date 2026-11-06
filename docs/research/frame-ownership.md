# Frame ownership: from the composed engine frame (Full / N5) to an owned, GPU-driven frame (plan, 2026-11-06)

User decision (2026-11-06, firm): `-Mode full` eventually **owns the whole frame**, as GD3D11 does for Gothic: collecting, skinning, culling,
drawing, lighting, and later our own re-created shaders. Work iteratively; minor setbacks at intermediate stages (picture differences,
temporary perf regressions) are acceptable. Wrapper stays faithful; Full may change the picture but keeps the visual identity (sun flare,
eye adaptation, vivid colours). Low-end targets (AMD Radeon 680M iGPU, low VRAM) matter: the owned frame is GPU-driven and writes no output
nobody reads.

Status: **plan only, nothing built.** Labels: [C] confirmed in docs / code / measurement, [I] inferred, [?] open (RE or measurement
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
  the light sets: O(#sets) per frame, not O(#instances) [? the number of light sets: measure first, 6.2].
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
| LightSet | sun c22..c24, up to 2 dynamic lights, SH c15..c21 base, indoor c14 | every frame | [?] sets x 320 B |
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
* Not ported yet: `ge_default_leafs.fx` (7 corpus shaders, the leaf PS), particles, sky dome: translated until their stage.
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
| vertices | pulled by address from MeshArena / a u32 index arena (`native_index_arena.*` in `git show 23d60f2^`); args `vertexOffset = 0`, base vertex from the template (Risen's sub-mesh indices are absolute [C]) [? parity test anyway] | same |
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
4. Too many light sets for a per-frame copy: copy only the sets visible instances reference (deduplicated), or keep the per-object reads for
   trees with dynamic lights.
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
