# M9: ray-traced reflections with textured hits (plan + record)

User target (2026-10-22): "even better water with textured ray traced reflections, detailed but 'water-y' reflections. Not
limited to ocean water." So: every water body the enhanced water shades (sea, lakes, rivers, the swamp pond) reflects the
real world geometry, off screen too, with the hit's own material texture; the reflection stays watery: bent and broken by
the waves and ripples, softer with distance, Fresnel-weighted, tinted like the engine's water.

Inputs (read these first): `docs/research/rt-scene-facts.md` (vertex layouts, world transforms, texcoords per draw kind,
where the data sits in our code), `docs/research/rt-albedo-facts.md` (the diffuse sampler slot from the engine's material
graph, alpha test, lightmaps), `docs/research/raytracing.md` (the tier, `rt::` helpers, self-test), `docs/research/water.md`
4.3 + `shaders/water/water.frag` (`traceSSR` = the seam the RT reflection plugs into), roadmap 3.2 - 3.5. GD3D11's D3D12
path (ours to reuse, credit it): `E:\git\github\GD3D11\D3D11Engine\D3D12Engine\D3D12RayTracing.{h,cpp}`,
`Shaders\D3D12\include\RtScene.hlsl`, `Shaders\D3D12\WaterRT.hlsl` (summary in section 4).

Legend: **[C]** confirmed, **[I]** inferred / proposed.

## 1. Decisions [I, this plan]

* **One RT scene for every consumer** (`enh/rt_scene.*`): water reflections now, glossy surfaces / RT shadows / GI later.
  Active only while a consumer wants it (no cost with everything off). Requires the tier (`caps::Cap::RayQuery`).
* **Geometry from the engine's own buffers**: positions are FLOAT3 at offset 0 of stream 0 in object space for every world
  kind except SpeedTree billboards [C, rt-scene-facts 3], so a BLAS points at the static VB with its stride (the buffers
  already carry the build-input usage with the tier). One BLAS per (VB id + generation, IB id + generation, index range,
  base vertex, stride), shared by all instances; transforms = the VS's `g_vInstanceData` rows (c33..c35 for plain draws,
  `c[4i+1..4i+3]` for instanced batches) read on the CPU at draw time = `VkTransformMatrixKHR` as is.
* **Persistent instance registry**: instances seen in the main view *and the shadow cascades* (they cover the area around
  the camera, also behind it) stay in the TLAS while their buffers live, within a radius of the camera, until unseen for
  a while; an entity part seen with a new transform or mesh replaces its old instance at once. Reflections need geometry
  outside the camera's frustum (cliff tops, canopies above the screen's top edge). [C, P1: the part = entity + `opDesc`
  per view, not the `eCSubMesh` (a LOD switch draws another sub-mesh object); water and main-view-blended meshes are
  learnt and left out of every view (the cascades draw them untagged): 3.1.]
* **TLAS rebuilt every frame** at `AfterOpaque` (before the water draws of the Refracted pass), BLAS builds budgeted per
  frame (GD3D11: 48 builds / 400k triangles), compaction + LRU eviction under a memory budget. [C, P1: the TLAS is built
  PREFER_FAST_BUILD (0.13 ms) - PREFER_FAST_TRACE cost 0.70 ms per frame on this GPU; BLASes PREFER_FAST_TRACE.]
* **Hit data** by buffer device address: a geometry record per (instance, geometry) (VB / IB addresses, index size,
  stride, uv / normal offsets, the albedo's bindless texture + sampler, uv map, alpha mode + reference, flags), reached
  through `instanceCustomIndex + geometryIndex`; TLAS + record addresses in the FrameBlock (uvec2, no shaderInt64).
* **Albedo** = the texture bound at s(9 + k), k from walking the material's Diffuse tree (rt-albedo-facts 1.2); bytecode
  fallback s9 unless it is a cube / the normal map. Alpha-tested (`Masked`) materials are non-opaque geometry resolved by
  the ray-query candidate loop with the draw's own alpha reference, mip by ray cone.
* **Reflections for `water.frag`** [P3: built as a compute pass before the water draws, a reflection image of the water
  planes' mirror rays that water.frag looks up bent by its waves: ray query inside the water's pixel shader cost 2-3x
  more, 3.3], (all four water models): one mirror ray along the calmer normal the SSR uses (the waves are the roughness,
  as GD3D11), the ripple deviation breaks the lookup up; **screen reuse**: a hit that lies on screen and matches the depth map takes the engine's
  fully shaded scene colour (refraction copy); off screen the hit is shaded like the engine's material: albedo x
  `pow(SH ambient + sun N.L x shadow ray, 0.45)` with the water draw's own light constants, the engine's fog over the
  path; texture LOD from a ray cone widened by the ripples (distant reflections soften); miss = the engine's sky cube as
  now. SSR stays the non-RT path.
* **Settings**: `rt.reflections` (category "Ray tracing", off / low / medium / high, requires RayQuery) used by the water
  when `water.enabled`; debug views `rt.debug` (instances / normals / albedo / depth check).

## 2. Phases (each built, verified in game, committed; one fresh agent each)

| phase | content | verify |
|---|---|---|
| **P1 RT scene** | draw observer, BLAS cache, instance registry, TLAS per frame, geometry records, FrameBlock fields, stats (rvk.log / overlay), `rt.debug = instances / depth` (camera rays traced against the TLAS over the frame) | the traced view lines up with the raster at lake_jungle / harbour_docks / beach_sun (depth check ~0 except foliage / characters), no leaks over teleports, CPU / GPU cost, sync validation |
| **P2 materials at hits** (done, 3.2) | albedo slot from the material graph (+ fallback), uv set + map, alpha-tested geometry (SpeedTree branches / fronds rest pose, Masked props) with the candidate loop, `rt.debug = albedo` | traced albedo view vs the frame at the spots |
| **P3 water RT reflections** (done, 3.3) | `rt.reflections`, hit shading (screen reuse, sun + shadow ray, SH ambient, fog), ray cones, the watery look, all water models, quality tiers; a compute pass (reflection image) instead of ray query in the water's pixel shader | water-compare side by sides (SSR vs RT) at beach_sun, harbour_docks, lake_jungle, waterfall_jungle, swamp_pond, crystal_cave; GPU cost per tier; toggles without hitch |
| **P4 the rest of the world** | leaf cards / billboards / grass (VS capture or their analytic expansion), skinned characters (VS capture per frame), lightmaps at hits | reflections of trees and the hero at lake_jungle / beach_sun |

## 3. Record

### 3.1 P1: the RT scene (2026-10-22) [C unless noted]

**Built**

| piece | files |
|---|---|
| draw observer (`d3d9::DrawObserver`: `observes(kind, phase)` + `observe(device, ReplaceQuery)` in `prepareDraw` before the replacers, `bufferChanged(id)` on a static buffer's new generation / release); `ReplaceQuery` gained `instances`, `blending`, `alphaTest` / `alphaFunc` / `alphaRef`, `depthWrite`, `depthBias`; `Device::streamSource`, `setRtScene` / `rtSceneValid`, `setStatus` (overlay lines); `Buffer::deviceAddress`, `ringCopied`, `id` | `src/d3d9/device.h`, `draw.cpp`, `device.cpp`, `resources.cpp` |
| pure bookkeeping: BLAS key, instance key (BLAS + rows quantised 1/16 unit, 1/4096), owner key (entity + `opDesc`), registry (owner replacement per view, aging frames AND seconds, radius by world bounds, cap), build budget, TLAS selection (superseded cascade LODs out) | `src/enh/rt_registry.{h,cpp}`, `tests/rt_tests.cpp` (8 tests) |
| the scene: BLAS cache (PREFER_FAST_TRACE + ALLOW_COMPACTION, compaction a frame-slot later via `VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR`, shared scratch with `minAccelerationStructureScratchOffsetAlignment`, budget 48 builds / 400k triangles, 512 MiB with LRU eviction to 85 %, unused 3600 frames AND 60 s dropped), TLAS per frame slot sized for 16,384 instances, instances + records in the frame's ring, sync2 barriers, stats, `describe()` for probes | `src/enh/rt_scene.{h,cpp}` |
| ABI: `FrameBlock` 896 -> 928 bytes (`rtScene` = TLAS + records addresses as uvec2 pairs, `rtInfo` = counts + `kFrameRtValid`), `RtGeometryRecord` (96 bytes), `kRtRecord*`, `kRtMask*`; GLSL `RvkRtRecord`, address math, index / attribute fetch (FLOAT1..4, D3DCOLOR, UBYTE4(N), SHORT2/4(N), USHORT2/4N, FLOAT16_2/4), `rvkRtSurface` (position, interpolated normal, uv0 / uv1 / lightmap uv, geometric normal), world normal from WorldToObject | `src/shader/abi.h`, `shaders/include/rvk_abi.glsl`, `shaders/include/rvk_rt.glsl`; offsets checked by `abi_glsl_layout_matches_cpp`, `abi_glsl_rt_layout_matches_cpp` (`shaders/test/abi_rt_layout.comp`) |
| debug view `rt.debug` (Debug, NoAB, needs RayQuery: off / instances / depth / normals) + hidden `rt.scene_views` (all / main / shadow) | `src/enh/rt_debug.{h,cpp}`, `shaders/rt/rt_debug.{glsl,comp,frag}`, `rt_debug_depth.comp`, `src/core/settings.cpp`, wiring `src/hal/vulkan/device.cpp` |

No translator change: the FrameBlock grew at its end, old cached SPIR-V simply does not declare the new members (no
`kTranslatorVersion` bump; corpus 2483 / 2483 valid, `compare_disasm` 2483 / 2483).

**How it runs.** Inactive (no consumer) = no observer, no pass, no GPU memory (`rt: scene: off (GPU memory released)`).
Active: the observer takes StaticMesh / InstancedMesh / SpeedTree branches + fronds (`opDesc` 0 / 1, rest pose) of the main
view's DepthPrepass + Solid and of the Shadow view; material VS only (`g_vInstanceData` at c32, not skinned / vegetation /
leaves, no `g_vInvView` billboard meshes), instancing of the draw = of the VS, static buffers only, POSITION FLOAT3 @0 of
stream 0 (other declarations are logged once: none at the three spots). Rows: c33..c35, or per instance
`c[32 + floor(4 v + 0.5) + 1 ..]` with v read from the TEXCOORD4 stream (`readCpu`). At `AfterOpaque` of the first main
view (order -100): WAR / WAW barrier, aging, eviction, compaction copies, up to the budget of builds (index range scanned on
the CPU for `maxVertex` + object bounds), barrier, compaction-size queries, instances + records (ring), TLAS build, barrier
to fragment + compute (`AS_READ`), `Device::setRtScene` (the FrameBlock gets the addresses only in the submission that
built them). Alpha-tested instances (alpha test on, or fronds) are `FORCE_NO_OPAQUE` with mask `kRtMaskAlphaTested`; P1's
shaders confirm every candidate.

**Findings while verifying (corrections to the plan / facts)**

* **Instance stream** = 0, 1, 2, ... in every batch [C: `rt: scene: instance stream check`]; the batch's rows are
  `c[32 + 4 i + 1 ..]`.
* **The sun cascades draw the water** (sea, lakes, the oil puddle `Nat_Ground_Burned_01_Diffuse_01_OIL`, glass
  `Arch_Glass_01_Diffuse_01`) as untagged StaticMesh: water is learnt from the main view's Refracted / water-tagged draws
  (material pointer + exact draw range) and left out in every view. Likewise ranges the main view only draws alpha-blended
  (excluded in the cascades unless the main view drew the same range opaque within 600 frames: the terrain's blended layers
  share its geometry; objects fading in are blended for a while).
* **Level of detail**: the engine switches LOD by drawing another mesh resource for the same entity part; owner key =
  (entity, `opDesc` = sub-mesh index | node) per view replaces the old LOD's instance at once (with `eCSubMesh*` as the key
  the walkway and the houses at lake_jungle kept a stale LOD for 20 s: magenta in the depth check after a teleport). The
  cascades may draw another LOD than the main view: a cascade-only instance whose owner has a main-view instance is left out
  of the TLAS.
* **The engine's linear depth map is not a reference**: only the z-pass subset of the opaque draws writes it (the
  walkway planks at lake_jungle are not in it). The depth check therefore captures the main view's **hardware depth** at
  AfterOpaque (`rt.debug.depth`, D3D `z = A + B / viewZ` with the FrameBlock projection; the white square at the top left
  of the view = this source).
* **TLAS build flags**: PREFER_FAST_TRACE took **0.70 ms** GPU per frame for ~290 instances on the 7900 XTX;
  **PREFER_FAST_BUILD 0.12-0.15 ms**. The TLAS is rebuilt every frame: FAST_BUILD (trace cost to be weighed in P3).
* The kind must not be OR-ed over sightings (an instanced mesh seen plain in a cascade became "Refracted"): it travels in
  the instance's data word.

**Verification**

* `rt.debug=depth` at lake_jungle / harbour_docks / beach_sun (`run.ps1 -Mods rvkcompare -Scenario rvkcompare -Param
  'spots=lake_jungle;harbour_docks;beach_sun','burst=1' -Setting 'rt.debug=depth' -SyncValidation -Perf`), pictures
  `out\rt-p1\rtfinal_*.png`: terrain, rocks, buildings, the walkway, the ship, the quay, skeletons, posts green; red only
  where the raster has geometry P1 leaves out (SpeedTree leaves, grass, ferns, characters); blue = tree billboards against
  the sky; orange specks = flowers / decals. Probe grid (5 x 5, rvk.log `enh: rt.debug probes`): harbour 19-20 match / 1-2
  farther (foliage); beach 11 / 1 / 3; lake 6-10 match / 15-19 farther (the jungle's leaves and grass cover most probes).
  Residual: beach_sun's shore strip (level mesh sub-mesh `op 0x3`, 1872 triangles at sea level) is 15-20 units off in both
  directions at 8-32 m (traced 3168 vs raster 3188): the raster draws it displaced (VS or bias without the D3D9 bias
  state; not resolved) - not a transform / index error (those would be metres).
* `rt.debug=normals` (`out\rt-p1\rtnorm_*.png`): smooth vertex normals everywhere, no magenta (the fetched triangle's
  position never disagreed with the hit by > max(2, 0.2 % t)): records, index / vertex fetch and rows are right.
* `rt.debug=instances` (`out\rt-p1\rtinst2_*.png`): one colour per object; coplanar duplicates show as speckle (harbour
  sea floor: two level-mesh parts overlap; a cascade-only cliff part, white checker) - their depth agrees.
* Turning around + teleports (rvkcompare `mode=calibrate`: the spot + 4 yaws per spot, 3 spots in one run): instances
  stay off screen; registry 170-420 instances, BLAS 190-400, AS memory 66-93 MiB (BLAS 27-54 MiB after compaction, TLAS
  2 x 3.6 MiB, scratch 32 MiB): no unbounded growth (far / aged instances: 216-490 over the run, evicted 0).
* `-SyncValidation`: **0** validation / synchronization messages over the three-spot runs (depth view, builds,
  compactions, TLAS, ray queries, the depth capture).
* Costs (`rvk_shadows`, beach, ~1050 observed draws, `-Perf -Vsync off`): everything off = baseline (CPU engine+HAL
  5.27-5.36 ms, draw calls 2.19-2.21 ms, GPU 1.95 ms; no `rt: scene` line: nothing registered). Scene active: observer
  **0.15-0.22 ms CPU per frame** (draw calls 2.52-2.54 ms: +0.33 ms incl. the query building), pass CPU 0.05-0.14 ms,
  **GPU 0.12-0.15 ms** (`pass rt.scene at AfterOpaque` 0.134 ms in `-PerfFrame 2500`), builds when the view changes up to
  ~0.05 ms CPU per frame; the debug view itself 1.19 ms GPU (one ray per pixel at 1600x900) + 0.017 ms depth capture. The
  first frame with builds took 300 ms CPU once per session (the driver compiles its BVH build kernels: `rt: scene: slow
  pass`).
* rvk_tests 108 / 108, `rvk_vktest --rt` / `--passes` / contract, `rvk_d3d9test` PASS.
* Parity with everything off: `refcompare.ps1 -ReferenceDir out\refcompare\20261022-093252` (run
  `out\refcompare\20261022-190022`): 16 PASS, 3 WARN, 1 INVALID, 0 FAIL. WARNs: beach_rain / sea_cliff (their known noise),
  harbour_docks (psnr_q 30.4: one swaying bush at the left edge beside its known-noise mask; earlier runs 43-49);
  INVALID lava_cave = the scenario's camera placement (eye 7.5 off), as before. (rvkcompare's main.lua changed after that
  reference was stored.)

**Open (for P2+)**

* Alpha-tested geometry: instances are flagged (`kRtRecordAlphaTested`, `FORCE_NO_OPAQUE`, mask 2) and the record has
  `alpha` = D3DCMPFUNC | ref << 8 of the draw; P2 fills `albedoTex` / `albedoSampler` / `uvSet` / `uvMap*` at observe time
  (the draw's `ReplaceQuery::textures` + rt-albedo-facts 1) and replaces the "confirm all" candidate loop by the alpha test.
  Records are rebuilt every frame from the registry: material data must live on the registry instance (or the BLAS) like
  the alpha word (`Instance::data`).
* Coplanar duplicates (overlapping level-mesh parts, LODs only the cascades draw): hits between them are arbitrary. Harmless
  for depth; for shading prefer a deterministic pick (e.g. the instance the main view saw last).
* The shore strip offset at beach_sun (above).
* Observer cost: ~1000 observed draws per frame of which 70 % are rejected after building a `ReplaceQuery`; a per-VS
  verdict cache would halve it [I].
* Scratch is 32 MiB minimum (the TLAS needs ~2 MiB); could start smaller.
* Compaction runs on the main queue in the frame (copies are cheap; measured within the 0.13 ms).
* Not done in P1: SpeedTree leaves / billboards, grass, skinned characters (P4), morph-target meshes, plain-world SpeedTree
  (not drawn at the spots), the `rt.debug=albedo` view (P2).

### 3.2 P2: materials at hits (2026-10-22) [C unless noted]

**Built**

| piece | files |
|---|---|
| CPU interpreter of SM 3.0 programs (all arithmetic, a0 / aL relative constants, predicates, if / rep / loop / break, texld* through a callback) + a backward slice (the constants / inputs a source operand or an output depends on); `rvk_shadertool evalcheck <dir>` runs it over a corpus | `src/shader/sm_eval.{h,cpp}`, `tests/shader_eval_tests.cpp` (2 tests), `tools/shadertool/main.cpp` |
| the engine's material graph: the shader of a draw (its tag's `eCMaterialResource2 +0x70`, else `eCShaderBase::ms_pActiveShader`), class by vtable, Diffuse / Opacity / Normal trees walked depth first to the first `eCColorSrcSampler` (slot +0x98, uv set +0xc8, tex-coord source +0xa8), blend mode +0x50, mask reference +0x58, the pool image (`+0xf8 + 0x20 + 0x28 k`) and its API texture (`eCImageResource2 +0x70` -> `eCGfxImageData +0x60`); SEH-guarded reads, cached per shader object, re-validated per lookup | `src/engine/materials.{h,cpp}`, `src/engine/memread.{h,cpp}` (safeRead / readAt / readCString, moved out of scene_hooks), symbols in `src/engine/symbols.h` |
| materials of the RT scene: slot (graph, else s9), the draw's texture + sampler states at s(9 + k), the uv map by evaluating the draw's VS + PS on 7 probe vertices, opacity (diffuse alpha / the Opacity tree's sampler + channel / no texture), the masked alpha test; interned entries with reference counts, a released texture marks its entries dead; one log line per material x pixel shader | `src/enh/rt_material.{h,cpp}` |
| scene side: per-BLAS material slots by draw signature (<= 8, LRU), per-instance material choice by rank (4 opaque solid draw > 3 first alpha-over blended solid layer > 2 z pre-pass > 1 cascades), overlays (caustics / decals: blend mode 2 with a texture opacity; untextured blended layers) never a base and their instances left out, blended-only layer ranges (`layers_`) and sub-range BLASes as **material segments** of merged ranges (the cascades / z pre-pass draw a level mesh in one range, the main view per material), records + segment table per frame | `src/enh/rt_scene.{h,cpp}` |
| ABI: `RtGeometryRecord` 96 -> 112 bytes (`segments`, `segmentCount`), `RtMaterialSegment` (64 bytes), `kRtUv*` flags, `kRtRecordSegmented` | `src/shader/abi.h`, `shaders/include/rvk_rt.glsl`, `shaders/test/abi_rt_layout.comp`, `tests/inject_tests.cpp` |
| GLSL helpers: `rvkRtApplySegment`, `rvkRtMaterialUv`, `rvkRtMaterialTriangle`, `rvkRtTextureLod` (ray cone, GD3D11's formula with the world-space triangle area), `rvkRtAlphaTest`, `rvkRtOpacity`, `rvkRtCandidatePasses`, `rvkRtAlbedo` (rvk_rt.glsl); `rvkRtTraceClosest` + `rvkRtHitSurface` (new `shaders/include/rvk_rt_trace.glsl`) | |
| `rt.debug` = `albedo` / `albedo_split` / `albedo_opaque` / `uv`; probes every second in the material views with each probe's material and the BLAS's material slots | `src/enh/rt_debug.{h,cpp}`, `shaders/rt/rt_debug.{glsl,comp}`, `src/core/settings.cpp` |
| d3d9 / HAL: `DrawObserver::textureReleased`, `Device::vsConstants / psConstants / *IntConstants / *BoolConstants`, `Device::engineTexture` (the HAL's live `eCAPITexture` -> `d3d9::Texture` map), `ReplaceQuery::srcBlend / dstBlend` | `src/d3d9/device.{h,cpp}`, `draw.cpp`, `src/hal/vulkan/{hal_vulkan.h,device.cpp,resources.cpp}` |

No translator change (`kTranslatorVersion` unchanged; corpus / `compare_disasm` unaffected). The record grew to 112 bytes:
hand-written shaders only (rvk_tests `abi_glsl_rt_layout_matches_cpp` checks record + segment offsets).

**Methods chosen**

* **Slot**: the material graph (rt-albedo-facts 1.2 (a)); the shader comes from the **draw tag's material** when there is
  one (`ms_pActiveShader` is stale in the z pre-pass / cascades), else from `ms_pActiveShader`. The bytecode fallback (s9
  if the PS declares a 2D s9) never triggered at the spots. A Diffuse without a sampler (constant, vertex colour, cube) =
  no texture (`kRtUvNoTexture`, mid grey); a depth pass whose PS samples no s9 takes the pool image through the HAL map,
  only with the tag's material.
* **uv**: exact by evaluation, not by parsing: the draw's VS (probe vertex inputs: uv0 / uv1 = (0,0), (1,0), (0,1), a check
  point, a moved position) feeds its outputs by semantic into the PS; the coordinate of the PS's first texld of the diffuse
  sampler gives `coord = b + A0 uv0 + A1 uv1`; the larger of A0 / A1 picks the set (`kRtUvApprox` when the check point or
  the moved position disagree: not affine / world dependent). Cached per (VS, PS, sampler) and the values of the constants
  the backward slice names (0.45 PS constants on average), resolved again every 64 frames per BLAS slot (animated maps).
* **Opacity / alpha test**: the draw's alpha test (D3DCMPFUNC | ref), else the masked material's own (GREATER
  MaskReference); opacity = the Opacity tree's sampler with the proxy's channel (else the diffuse alpha; a non-texture
  opacity = never cut). Instances are non-opaque only when there is a texture to test.
* **Which draw gives an instance its material** (one range can be drawn with several materials): rank 4 the main view's
  opaque solid draw (the last of the frame: opaque passes over one surface), 3 its first alpha-over blended solid layer
  of the frame (the terrain's base; only layers with a diffuse texture and vertex-colour opacity), 2 its z pre-pass, 1 the
  cascades. Overlays (blend mode 2 with a texture opacity; blended layers that are no terrain layer; untextured layers)
  are never a base, and instances known only through one are left out of the TLAS.
* **Segments**: a BLAS without a rank >= 3 material of its own (the cascades' / z pre-pass's merged range of a level mesh)
  gets per-triangle materials from the BLASes and blended-only layer ranges inside it (same buffers, base vertex);
  `rvkRtApplySegment` binary-searches the hit's primitive. Coplanar hits between the merged and the sub-range instances
  then show the same texture.
* **Lifetime**: records take each texture's bindless slot every frame (0 when it is not in SHADER_READ_ONLY); a released
  texture (`DrawObserver::textureReleased`) marks its materials dead and their users resolve again at the next sighting.

**Helper API for P3** (`#include "rvk_rt_trace.glsl"` after rvk_abi.glsl; enables GL_EXT_ray_query)

* `RvkRtHit rvkRtTraceClosest(f, origin, dir, tMin, tMax, coneWidth0, coneSpread, flags)`: closest hit, alpha-tested
  candidates tested with the record's alpha test on the opacity at the ray cone's mip (`width = coneWidth0 + coneSpread t`);
  flags `RVK_RT_TRACE_OPAQUE` (cut-outs solid: the cheap tier), `RVK_RT_TRACE_NO_CUTOUTS` (mask them out).
* `RvkRtHitSurface rvkRtHitSurface(f, hit, dir, coneWidth0, coneSpread)`: albedo (rgba as sampled at the cone's mip; the
  record's colour without a texture), world position, normal + geometric normal (facing the ray), uv, lightmap uv, lod,
  record flags / uvSet, instance id; segments applied.
* Lower level (rvk_rt.glsl): `rvkRtRecord`, `rvkRtApplySegment`, `rvkRtMaterialTriangle`, `rvkRtTextureLod`,
  `rvkRtAlphaTest`, `rvkRtOpacity`, `rvkRtCandidatePasses`, `rvkRtAlbedo`, `rvkRtSurface`.

**Verification**

* `rt.debug=albedo` (and `uv`) at lake_jungle, harbour_docks, beach_sun, monastery_hall, crystal_cave, jungle (`run.ps1 -Mode
  vulkan -Mods rvkcompare -Scenario rvkcompare -Param 'spots=...','burst=1' -Setting 'rt.debug=albedo' -SyncValidation`):
  pictures `out\rt-p2\final_<spot>.png`, side by side with the raster `out\rt-p2\final_sbs_<spot>.png`, overview
  `final_overview.png`; uv `out\rt-p2\uvm_*.png`; cut-outs `out\rt-p2\cutouts_jungle.png` (raster / albedo /
  albedo_opaque). Quay cobbles, quay wall, ship, crane, houses, walkway planks, rocks, cliffs, the lake / cave floors
  (terrain layers), the monastery's brick walls, pillars and floor slabs, banners: pattern, scale and orientation match
  the raster; herbs / ivy / ferns cut out (solid quads with `albedo_opaque`). The iterations that led there (`run1..12`
  pictures): terrain untextured (its solid layers are blended), caustics / wet specular layers taken as the base, stale
  active shaders in the depth passes, merged cascade ranges coplanar with the main view's sub-ranges (speckle, wrong
  textures).
* Material log: ~160 material x PS lines, every Diffuse at k0 = s9 with its `*_Diffuse*` pool image, 0 mismatches between
  `ms_pActiveShader` and the tag's material in solid draws, 0 failed uv evaluations, 1-3 approximate maps (rt-scene-facts
  4.2a, rt-albedo-facts 1.4).
* Depth check unchanged from P1 (`out\rt-p2\dep_*.png`, probes at the captures: beach 11 match / 1 farther / 3 closer,
  harbour 19 / 2 / 0, lake 6 / 19 / 0 - foliage).
* `-SyncValidation`: 0 messages over the albedo (6 spots) and depth (3 spots) runs. rvk_tests 110 / 110, `rvk_vktest --rt`,
  `--passes`, `rvk_d3d9test` PASS; `rvk_shadertool evalcheck corpus` 2476 / 2476.
* Costs (`rvk_shadows`, beach, ~1060 observed draws, -Perf -Vsync off; **another game used the GPU at ~95 % during these
  runs**: GPU numbers are upper bounds, CPU numbers skewed): draw calls 3.40-3.48 ms with everything off, 3.63-3.77 ms with
  the scene + albedo view (+0.25-0.3 ms; the observer's own timer says 0.43-0.47 ms per frame, P1 0.15-0.22 without the
  load); material resolves 2-5 per frame, 0.005-0.015 ms; uv evaluations 7-35 per 5 s; pass CPU 0.08-0.12 ms. GPU
  (`-PerfFrame`): `rt.scene` 0.055 ms, `rt.debug` albedo 1.26 ms at 1600x900 (one ray per pixel, alpha test on),
  `albedo_opaque` 0.70 ms: **the alpha-test candidate loop roughly doubles a primary ray at the beach** (herbs).

**Open (for P3 / P4)**

* SpeedTree branches / fronds are **not in the scene**: drawn as triangle strips (`rt: scene: SpeedTree draw ... not
  taken: not a triangle list`); leaves (op > 1) P4. P1's statement corrected (rt-scene-facts 10).
* Cascade-only instances without main-view sub-ranges stay untextured (~170 of ~420 records at the spots, mostly far /
  behind the camera): the cascades carry no material tag and their PS samples no diffuse. Candidates: remember materials by
  mesh resource, or tag the cascades' draws.
* Approximate maps: reflection-vector coordinates (wet specular layers) and PS-computed offsets are taken as their affine
  part.
* Per-triangle materials only for merged ranges; one material per instance otherwise (a decal layer on the same surface is
  ignored).
* The far sea floor at beach_sun shows the caustics texture (an overlay only the cascades see): under water.
* Observer cost grew (materials + blended-layer handling): the per-VS verdict cache (P1 item) is still open.
* Lightmaps at hits (P4); the hit's lighting is P3 (`albedo * pow(SH + sun N.L * shadow, 0.45)`).

### 3.3 P3: ray-traced water reflections (2026-10-22) [C unless noted]

**Step 0: the scene's gaps**

| piece | files |
|---|---|
| SpeedTree branches / fronds (triangle **strips**) in the scene: a strip's indices are converted to a list at BLAS build (every second triangle's winding swapped, degenerate restarts dropped) into a device buffer of the BLAS (uploaded through the frame's ring, a copy -> build / shader barrier); the BLAS and the geometry record read that list (`rt::BlasKey::strip`, `Blas::listIndices`, `Blas::triangles`) | `src/enh/rt_registry.{h,cpp}`, `src/enh/rt_scene.{h,cpp}` |
| materials of **depth-only draws** (main-view z pre-pass, sun cascades): they run `eCMeshResource2::RenderZPass` (Engine.dll 0x7603b0): one DrawIndexedPrimitive over the merged range of the opaque sub-meshes (start = the first one's first index, count = the sum of index counts / 3), masked sub-meshes alone, blended ones skipped; no `eCSubMesh::Render`, so no material in the draw tag. The render context (`eCRenderContextMesh +0x28` = `eCMeshResource2`; sub-mesh array `+0x58` pointer / `+0x60` count, 0x48 bytes each: first index `+0x28`, index count `+0x2c`, first vertex `+0x30`, vertex count `+0x34`, `eCMaterialResource2*` `+0x38`) names them: one sub-mesh over the range = the draw's material (resolved with its pool image, uv unmapped), several = per-triangle **engine segments** of the BLAS, filling the gaps the main view's sub-ranges leave (checked again every 256 frames or when a material died) | `src/enh/rt_scene.cpp` (`engineSubMeshes`, `learnEngineMaterials`, `refreshSegments`) |
| a per-VS verdict cache in the observer (shader class, `g_vInstanceData`, billboards, instancing) | `src/enh/rt_scene.cpp` |

Result at the spots (rvk.log `rt: scene: materials ...`): **SpeedTree records 230-300** (P2: 0), **untextured records
13-23 of ~500** (P2: ~170 of ~420), engine sub-mesh segments 120-250. `rt.debug=albedo` shows palms with fronds, the bare
lake trees' branches, the harbour's sea floor and quay textured (`out\water\20261022-212216\enh\`). One ship hull at
harbour_mouth stays untextured (checker). The observer's own timer: 0.35-0.46 ms per frame (P2 0.43-0.47: the verdict
cache saves little; the cost is per instance of the instanced batches, now with SpeedTree).

**Step 1: the reflections**

| piece | files |
|---|---|
| setting `rt.reflections` (Ray tracing; off / low / medium / high, default medium, needs RayQuery, unmet = off; acts with `water.enabled` + `water.reflections = ssr` only) and hidden `rt.reflection_tweaks` (cost measurements: 1 cut-outs solid, 2 no shadow rays, 4 no SSR fallback, 8 unshaded); `water.debug = reflection_source / reflection_shaded` | `src/core/settings.{h,cpp}`, `src/hal/vulkan/device.cpp` |
| the pass: `enh::WaterRt` (holds the RT scene while on), compute `water.rt` at **AfterRefractionCopy** (before the water draws) into a transient RGBA16F reflection image (premultiplied colour + coverage), its pipeline built in the background from device creation | `src/enh/water_rt.{h,cpp}`, `shaders/water/water_rt.comp` |
| the surfaces: each water body as a plane (least squares over its world vertices, worst error logged; > 150 units = no plane: such a body keeps the SSR), its world box, its outdoor factor (mean COLOR1.w); bodies drawn in the last 120 frames, the 16 nearest; the light of the water draws (c3 / c4, c14..c22, c24) | `src/enh/water.{h,cpp}` (`fitPlane`, `measureOutdoor`, `fillBlock`) |
| `water.frag` looks the image up like a planar reflection (`traceReflection`), SSR where it has nothing; `WaterBlock::rtRefl` | `shaders/water/water.frag`, `water_common.glsl`, `src/enh/water.h` |
| `rvkRtVisibility` (shadow rays: terminate on first hit, the same alpha-tested candidate loop) | `shaders/include/rvk_rt_trace.glsl` |
| `rvk_vktest --shader-stats` (`VK_KHR_pipeline_executable_properties`: VGPRs / LDS / scratch of the water PS and the compute pass; `vk::ContextOptions::pipelineStatistics`) | `tools/vktest/main.cpp`, `src/vk/context.{h,cpp}` |
| `rvk_glslc -D NAME[=VALUE]` and CMake `RVK_SHADER_VARIANTS` (`"<name>=<source>:<DEF>,<DEF>=v"`; empty now) | `tools/glslc/main.cpp`, `CMakeLists.txt` |
| `water-compare.ps1 -EngineSetting` (the left run's settings instead of the engine's water: SSR vs RT) | `scripts/water-compare.ps1` |

**How it works.** Per pixel of the reflection image (1 per 2 x 2 screen pixels at low / medium, per pixel at high): the
view ray against the body planes (camera above the plane, inside the box + 60, in front of the engine's depth map), the
nearest one; the **mirror ray of that plane** (`dir.y >= 0.02`, origin + 2 units along the normal, tMin 1), traced with
`rvkRtTraceClosest` (cut-outs alpha-tested except at low), ray cone = the output pixel's angle x distance + 0.0015 rad
(texture mips: sharp near contact, softer with distance). Hit: **screen reuse** when it projects on screen, lies above the
plane (a hit under the waterline projects onto the refraction copy's underwater scene: it lit quay walls up as the bright
floor in the first version) and the depth map agrees within max(30, 1.5 % viewZ), faded over the screen's 3 % border;
else **shaded** like the engine's material: albedo (no texture: the constant colour <= 0.5; no material: 0.2 grey) x
`pow(max(mix(c14, SH9(N), outdoor), 0) + c22 x sat(N.L) x sat(4 G.L) x shadow, 0.45)`, a shadow ray (medium: cut-outs
solid; high: alpha-tested; reach 20,000), the engine's fog over the camera's distance to the hit (the same fog a reused hit
carries: no seam). Coverage fades over the last 15 % of the reach (low 15,000, medium 20,000, high 30,000 units; the RT
scene's radius is 30,000). No lightmaps / point lights at hits (P4). In `water.frag`: the image at the pixel's uv +
**bend** = `0.12 x nr.xz / (1 + dist / 4000)` (nr = the 35 % ripple normal SSR's ray follows: the FFT macro waves' tilt
and the ripples) + SSR's ripple jitter; if the bent lookup lands off the water (coverage < half the pixel's own) the
pixel's own; the colour un-premultiplied, coverage = conf; where coverage < 1 (medium / high, the model's reflection weight
>= 2 %) SSR fills in underneath (leaf cards, grass, characters are not in the TLAS); then each model's own path:
`keepSky` with an RT floor of 0.3 (sea) / 0.75 (lake, river) x mix(0.35, 1, outdoor) (the lake / river glitter stays; SSR
keeps 0.55 / 0.85), the swamp's dim mirror, the sun's glint x (1 - 0.85 coverage) where the world is reflected
(sea, swamp). Underwater camera: the body is skipped (camera below its plane) and the water's `underside` path as before.

**Why not ray query in the water's pixel shader** (built first, measured, replaced): one ray per water pixel inside
`water.frag` cost **+3.4-3.7 ms GPU** at lake_jungle / harbour_docks (1080p, 7900 XTX, medium): the ray query raised the
pixel shader from 163 to 234 VGPRs (`--shader-stats`; per-model variants 160-200 did not help: 3.5-3.8 ms), the rays in
dense foliage with alpha-tested fronds, shadow rays and SSR fallback. A FAST_TRACE TLAS saved ~1 ms there but costs 0.25-0.6
ms per build; with the compute pass FAST_BUILD is better (P1's choice stays).

**Costs** (1920 x 1080, vsync off, the GPU otherwise idle except a browser; frame GPU time, A/B within one hold of the spot
via `[debug] commands` (`run.ps1 -Perf -WindowSize 1920x1080 -Commands '5200: set rt.reflections medium',...`, rvkcompare
`freeze_wait=4800`; the spot's hold starts near frame 3,800), rvk.log `perf:` windows):

| spot | off (SSR) | low | medium | high |
|---|---|---|---|---|
| lake_jungle | 2.6 ms | 3.55 (+0.95) | 4.2 (+1.6) | 5.15 (+2.5) |
| harbour_docks | 3.05 | 3.53 (+0.5) | 4.08 (+1.0) | 4.4 (+1.35) |

`-PerfFrame` at lake_jungle medium: `water.rt` 1.32 ms, `rt.scene` ~0.1 ms, the water's render pass 0.38 ms (SSR 0.44:
fewer SSR rays); compute 121 VGPRs (wave64), water.frag 172 (SSR-only 163). Of the 1.32 ms: alpha-tested cut-outs ~0.5,
shading ~0.13, shadow rays ~0.1 (`rt.reflection_tweaks` 1 / 8 / 2). CPU: one 1.2 KiB parameter block per frame, the pass
< 0.05 ms. Turning the tier on from off costs one ~26 ms frame (the RT scene rebuilds its BLASes within its budget; the
session's first activation ~300 ms: the driver's build kernels); medium <-> high: none.

**Verification**

* SSR vs RT medium, composites (`water-compare.ps1 -EngineDir out\water\20261022-210848 -MaskDir ... -Setting
  rt.reflections=medium -Burst 4 -Interval 1`): **`out\water\20261022-225604\`** (8 spots, left SSR / right RT): the look
  and colour stay the engine water's; luma over the water masks within -2.6 .. +3.2 of SSR (beach_sun +2.6, crystal_cave
  -2.6, harbour_docks -0.5, harbour_mouth -0.6, lake_jungle -1.1, sea_cliff -0.3, swamp_pond -0.2, waterfall_jungle +3.2),
  hue +-2.
* Reflection only (`water.debug=reflection`, SSR left / RT right): **`out\water\20261022-230105\`**: harbour_mouth
  (`crop_harbour_mouth_0_450.png`: SSR shows a broken piece of the hull; RT the whole hull, the mast and the rigging,
  broken up by the ripples), harbour_docks (the quay wall and cliff reflected whole), lake_jungle (trunks and canopy over
  the lake, darker than the cube SSR kept), beach_sun (the rocks' complete reflections), swamp_pond (the trees mirrored).
* Where each reflection comes from (`water.debug=reflection_source`, left; reflection right): `out\water\20261022-223731\`
  (`sheet.png`): green screen reuse, blue shaded hits (off-screen canopy at the lake), yellow SSR fallback, black sky.
* Motion (`-Burst 4 -Interval 1`, mean absolute frame-to-frame difference of the 8-bit pictures): RT = SSR within noise
  (lake 0.02-0.03 / 0.02-0.03, harbour_docks 0.07 / 0.10, sea_cliff 0.45 / 0.46, beach_sun 0.37 / 0.32): no shimmer added
  (no stochastic rays; the half-resolution image is bilinear).
* First version findings fixed on the way: bright blobs at quay walls (hits under the waterline reused the underwater
  refraction copy), lakes' glitter lost (RT floor 0.6 -> 0.75), caves (floor x outdoor).
* `-SyncValidation` (tier high, lake_jungle / harbour_mouth / crystal_cave): 0 messages. rvk_tests 110 / 110,
  `rvk_vktest --rt` / `--passes` / `--shader-stats`, `rvk_d3d9test` PASS.
* Defaults (water off: no RT consumer) against the stored reference (`refcompare.ps1 -ReferenceDir
  outefcompare61022-093252`, run `outefcompare61022-231047`): 18 PASS, 1 WARN (swamp_camp ssim 0.975), 1 FAIL
  harbour_docks = the engine's timed wave-splash particles over the basin in that capture (water.md 8); re-run of the spot
  `outefcompare61022-231618`: PASS (psnr_q 51.7).

**Open (for P4 and later)**

* Leaf cards, billboards, grass and skinned characters are not in the TLAS: screen reuse / the SSR fallback show them on
  screen only; off screen the canopies reflect as fronds + branches (the leaf cards' gaps let the cube through).
* Lightmaps and point lights at shaded hits (caves: hits are lit by the water's ambient and the sun with a shadow ray; the
  cube floor x outdoor keeps the engine's cave glitter, which is the engine's look, not a leak).
* Bodies that are no plane (rivers with falls; none at the spots) keep the SSR; the sea's swell is a bent lookup of the
  flat plane's image, not a per-wave ray.
* The reflection image is screen-space at 1 ray / 4 pixels at medium: thin far objects (rigging) alias a little.
* Cost at the jungle lake (+1.6 ms medium) is mostly the alpha-tested fronds' candidate loop: opacity micromaps (not on
  this GPU) or a cheaper cut-out rule for far candidates would help.

## 4. GD3D11's D3D12 RT path (survey 2026-10-22; root `E:\git\github\GD3D11\D3D11Engine\`)

* Scene (`D3D12RayTracing.cpp`): masks World / Vob / Dynamic / Carrier; caps 12,288 instances, 16,384 geometry records,
  48 BLAS builds or 400k triangles per frame, 384 MB cache, evict after 120 unused frames (only over budget, back to 85 %);
  BLAS suballocated from 32 MB pool chunks; PREFER_FAST_TRACE | ALLOW_COMPACTION, compacted when the frame slot's fence
  retired (`BuildCached` 505-551, `ProcessCompactions` 486-502; a generation check against stale compactions); TLAS
  rebuilt every frame, sized for the cap, its scratch taken first; radius = max over consumers (water 15k - 60k units by
  tier); skinned NPCs: a fresh PREFER_FAST_BUILD BLAS per frame over the posed stream; wind-swayed foliage: rest pose,
  FORCE_NON_OPAQUE, shadow rays skip candidates closer than its sway reach.
* Hit data (`RtScene.hlsl`): `RtGeom{BaseVertex, StartIndex, Material, Kind}` at `InstanceID() + GeometryIndex()`,
  material word = bindless slot | AlphaTest | NoTexture, re-resolved every frame (texture streaming); `FetchTri` per kind
  (stride, index size, uv offset); geometric normal from the edges (normal transform not inverse-transpose: uniform scale
  only); texture LOD by ray cone `0.5 log2(texelArea / triArea) + log2(coneWidth / max(cos, 0.1))`, cone width =
  pixel angle x (view distance + t); alpha test 0.5 on that mip in the candidate loop (`WaterRT.hlsl` 85-93).
* Water (`WaterRT.hlsl`): compute at half resolution (tiers low .. ultra: rays per 2x2, textured, shadow rays, screen reuse,
  max distance 20k - 60k), the pixel shader's own wave ray (normal lerped towards up with distance, `dir.y >= 0.02`),
  origin +2 up, TMin 1; screen reuse = the hit projected into the frame, depth within `max(30, 1.5 % viewZ)` -> the lit
  scene colour; else Lambert sun x shadow ray (origin + N x 4, TMin 2) + ambient, albedo de-lit like the raster, fog by
  distance; coverage fades over the last 15 % of the max distance, premultiplied for the upsample; no denoiser; on the
  hit: highlight compression + saturation tweak, radiance clamp, rain fog, sun glint blocked where geometry is reflected.
