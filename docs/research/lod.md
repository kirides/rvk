# Detail distances (LOD)

Settings `lod.trees` / `lod.meshes` / `lod.view` (Graphics tab, category Detail; player defaults 2 / 2 / 1, test baseline in
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

Open: a motion test across the transition at x3; cost in dense forests; `lod.view` in game (only x1 tested); whether
DoRender's `UpdateFromAppTemplate` runs every frame (otherwise a `lod.view` change applies at the next configuration apply);
`CacheInRange` streaming limits for `lod.view` > 1.
