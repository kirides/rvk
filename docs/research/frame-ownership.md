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
