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
* **Reflections in `water.frag`**, inline ray query in the fragment shader at the place of `traceSSR` (all four water
  models): one mirror ray along the calmer normal the SSR uses (the waves are the roughness, as GD3D11), the ripple
  deviation breaks the lookup up; **screen reuse**: a hit that lies on screen and matches the depth map takes the engine's
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
| **P2 materials at hits** | albedo slot from the material graph (+ fallback), uv set + map, alpha-tested geometry (SpeedTree branches / fronds rest pose, Masked props) with the candidate loop, `rt.debug = albedo` | traced albedo view vs the frame at the spots |
| **P3 water RT reflections** | `rt.reflections`, hit shading (screen reuse, sun + shadow ray, SH ambient, lightmaps if cheap, fog), ray cones, the watery look, all water models, quality tiers | water-compare side by sides (SSR vs RT) at beach_sun, harbour_docks, lake_jungle, waterfall_jungle, swamp_pond, crystal_cave; GPU cost per tier; toggles without hitch |
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
