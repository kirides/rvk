# Detail distances (LOD)

Settings `lod.trees` / `lod.meshes` / `lod.view` (Graphics tab, category Detail; player defaults 2 / 2 / 2, test baseline in
`scripts/run.ps1` = 1 / 1 / 1). Code: `src/engine/lod.{h,cpp}`, `Device::lodModule` (`src/d3d9/device.cpp`),
`GlslOptions::inputOverrides` (`src/shader/glsl.h`), `abi::kFrameParamLod` = 1. Commit 916040b. **[C]** = read in the
decompilation (Engine.dll / Game.dll RVAs, image base 0x180000000) or measured in the game, **[I]** = inferred.

## 1. What the game offers [C]

* `Render/ViewDistance` 0-3 (ConfigUser / ConfigDefault) only selects the far clip zFarLow / Medium / High =
  15000 / 30000 / 60000 (`eCRenderSystemAdmin::ApplyConfiguration` 0x66c3e0, `eSRenderOptions` ctor 0x8b6a0 +0x58).
* `Render/ScreenObjectDistanceCulling` is dead: the string is absent from Engine.dll and Game.dll.
* Vegetation/<Quality> `ViewRange` 2500 / 4000 / 7500 = grass only (`eCVegetationAdmin` +0x58,
  `eCVegetation_PS::UpdateDefaultViewRange` 0x708130). `DetailShadowMapViewRange` = shadows.
* The pop distances are world data: per tree `eCSpeedTree_PS` properties LoDNearDistance (+0x2c), LoDFarDistance (+0x30),
  LoDFactor (+0x34), applied via `ApplyLoDValues` 0x6e8cd0; per mesh `eCMesh_PS` LoD ranges (+0x60 float[3], count byte
  +0x6c; `GetLoDRange` 0x5c5b50 returns +inf for the last LOD); per entity `eCGeometryEntity` ViewRange +0xcc
  (`SetViewRange` 0x6a65c0 also updates `eCPrefetchAdmin`). No config key reaches any of them.

## 2. SpeedTree LOD [C]

`eCWraper_SpeedTreeRT` (embedded at `eCSpeedTree_PS`+0x40): +0 `CSpeedTreeRT*`, +8 `SLodValues`, +0x2c LOD level, +0x30 LoD
factor. `UpdateLoD` 0x6f8f30: `SetTreePosition`, `GetLodLimits` (FUN_1802aed80 -> instance `*(tree+8)` +0x54 near, +0x50
far), `SetLodLimits(near*factor, far*factor)`, `ComputeLodLevel` (FUN_1802c4860: lod = 1 - (dist - near)/(far - near)
clamped 0..1, camera position static `DAT_180c5bd38`, approximate sqrt), `GetLodValues` (FUN_1802aed90: branch / frond /
leaf LOD indices, drop-to-billboard flag `DAT_180c5bb91`), then restores the limits. Called from
`eCSpeedTree_PS::UpdateLoD` 0x6e8be0 <- `eCRenderContextSpeedTree::UpdateLoD` 0x6ee4d0. `GetBillboardFadeValues` 0x6f8a00 uses
the same limits x factor.

Measured limits (rvk.log `engine: lod: tree LOD limits`, jungle / beach / inland): near 1000-4500, far 6000-13500 units
(10-45 m / 60-135 m), factor always 1.0.

### 2.1 Billboards [C]

`eCSpeedTreeBBMesh_PS::FillBBVertexIndexBuffers` 0x6ea910 (the tool-time `CreateBillboardMesh` path; the meshes ship as
data) bakes per vertex (stride 64) BLENDWEIGHT1.x = fade-in start, .y = fade-in range from the limits x factor (linear in
near / far). `ge_default_vs.fx` (`g_bSpeedTreeBillboards`): `fDistanceFade = clamp((camDist - vWeights1.x) / vWeights1.y,
0, 1)`. `eCRenderContextSpeedTreeBB::RequestRenderOps` 0x6ee9b0 culls per cell only.

## 3. Mesh LOD and view range [C]

`eCRenderContextMesh::UpdateLoD` 0x5c74b0 compares the squared camera distance with the ranges and prefetches the chosen
LOD. Measured mesh ranges: 3000-16500 units (2 or 3 LODs).

`eCFrustumElementItem::CullNode` 0x6710e0 / `CalculateFadeOut` 0x671220 use the entity ViewRange x `eCFrustumDatabase` +0x15b4
(a fade between `DAT_1809f1674` x range and range; skipped when +0x15a9 is set). +0x15b4 is copied by
`eCFrustumDatabase::UpdateFromAppTemplate` 0x6708a0 from the `eCRenderSystemAdmin` caps template +0x94
(`GetFrustumDatabaseCapsTemplate` 0x66cf70 = admin+0x70), which `ApplyConfiguration` always sets to 1.0: there is no other
writer. `UpdateFromAppTemplate` runs from `eCApplication::DoRender` 0x4c0d40 (frequency not measured [I]), camera init,
shadow map creation and `ApplyConfiguration`.

## 4. Implementation

Detours (MinHook, Vulkan mode only, installed with the scene hooks):

* `UpdateLoD` of the wrapper and `GetBillboardFadeValues` multiply +0x30 by `lod.trees` for the duration of the call.
* Mesh `UpdateLoD` scales `ranges[0..count-2]` for the call (`scaleRanges` / `restoreRanges`, unit-tested in
  `tests/lod_tests.cpp`).
* `UpdateFromAppTemplate` post-hook writes +0x15b4 = `lod.view` when the game's value is 1.0.
* Billboard draws (Kind `SpeedTreeBillboard`, not replaced) use a vertex-shader variant while `lod.trees` != 1: BLENDWEIGHT1
  is read as `vec4($v.xy * pc.frame.params[1].x, $v.zw)` (`abi::kFrameParamLod`; cache file `<hash>_lod1_t2.spv`). The
  default translation is unchanged at x1.

rvk.log: `engine: lod: tree LOD limits ...` / `mesh with N LODs, ranges ...` (first 24 distinct each), `hal: settings: detail
distances: ...`, `d3d9: billboard fade variant of vertex shader ...: built`.

## 5. Verification (2026-10-23)

rvkcompare spots jungle, beach_sun, shadow_inland_far at x1 vs x3 (`out\lod\lod1`, `lod3`, `sbs_*.png`):

* bursts noise-free (0 px > 24 within a run); x1 vs x3 differ by 10k / 17k / 150k px;
* distant billboards become full trees; near palms keep more leaves (the SpeedTree leaf / branch LOD steps scale too); more
  distant mesh detail at the beach; no doubled billboard + tree seen;
* 4 billboard VS variants built; no errors, no validation messages;
* fps capped at 80 (Radeon Chill) in both, draws ~1500-1600 in both: **cost unmeasured** (needs `-Exe RisenTest.exe -Perf`).

Open: a motion test across the transition at x3; cost in dense forests.

## 6. Object draw distance beyond the cache-in range (2026-10-23)

User report: `lod.view` had no visible effect beyond ~70 m (barrels on a building popped in at the same place at x3). Cause
[C]: the frustum factor (+0x15b4) is only the last of three gates; the other two use the entity's raw range. Dumps:
`B:\DevDrive\risen-ghidra\viewdist\` (`prefetch_dump.txt`, `range_dump.txt`, `scene_opts_dump.txt`, `prerender_dump.txt`).

1. **Cache-in.** `eCFrustumDatabase::ExtractFromEntity` 0x66d750 only takes entities with flag 0x20 (cached in, set by
   `eCGeometryEntity::OnCacheIn` 0x6a9720). `eCApplication::OnPreRender` 0x4c0650 -> `eCPrefetchAdmin::CacheEntities` 0x6969c0
   (prefetch admin = scene admin +0xe8, centre = the camera +0x90): an entity is cached in while the camera is inside its
   sphere (`OnTestCacheInPos` 0x696530). **`GetPrefetchSphere` 0x6967e0**: radius = CacheInRange (+0xd0) when > 0, else
   bound radius + (ViewRange +0xcc when > 0, else the default range prefetch +0x28 = `/Engine/Scene/EntityROI`, 4000,
   `SetDefaultCacheInRange` 0x695c10). No view factor: at x3 the entity is still not resident beyond its own range.
2. **Subtree cull.** `eCFrustumDatabase::ExtractFromEntities` 0x66f790 culls entities with children (flag 0x10000) with
   **`eCFrustumElementItem::CullTree` 0x6711a0**, which passes the raw ViewRange to `CullCamera` (CullNode 0x6710e0 multiplies
   by +0x15b4).
3. `UpdateFromAppTemplate` runs every frame from `DoRender` (unconditional), so the +0x15b4 post-hook holds.

Other facts: the default ViewRange is 0 (`eCGeometryEntity::Invalidate` 0x6a6cf0; property "Culling/ViewRange", CacheInRange
"Rendering/CacheInRange": world / template data). An entity with ViewRange 0 has no distance cull, only the EntityROI
cache-in (bound + 40 m). `CalculateFadeOut` 0x671220 fades over the last 10 % of R (`DAT_1809f1674` = 0.9), so a scaled range
keeps its fade. The far clip (15000 / 30000 / 60000 by ViewDistance) is the only global limit. Ranges seen (rvk.log `engine:
lod: entity ViewRange ...`, logged unscaled): 3500, 4500 (props: 35-45 m + bound), 150000 (CacheInRange = ViewRange), 0.

**Built**: post-hook on `GetPrefetchSphere` adds `(f - 1) x` the range term (`scaledCacheInRadius`, unit-tested); `CullTree`
scales the entity's ViewRange by the database's factor for the call; a factor change re-evaluates every entity's sphere at
the next present (`SetDefaultCacheInRange(d + 1)` then `(d)`: the engine's own walk re-inserts the changed spheres; the
prefetch admin is the `this` seen in the hook). `lod.view` is a normal (not advanced) setting now, player default 2x (test
baseline 1x).

Measured (rvkcompare harbour_mouth / harbour_docks / swamp_camp, x1 vs x3, `out\viewdist\`): props appear on the far quay
(a stall, crates, the quay's wave-splash effect), harbour_mouth 347 -> 436 draws, harbour_docks 1019 -> 1135; ~1 % of the
harbour_mouth pixels change strongly. Cost: on the beach approach the draws went from ~1465 to ~2750 at x3 (170 -> 107 fps
uncapped, RX 7900 XTX); GPU memory allocated 635 -> 792 MiB.
