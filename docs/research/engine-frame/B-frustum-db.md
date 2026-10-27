# Engine frame, slice B: `eCFrustumDatabase` and the render-op machinery

Status: research, 2026-10-27 (read-only RE, headless Ghidra on Engine.dll, clone `clones\EngineB`). Dumps:
`B:\DevDrive\risen-ghidra\frameB_dump1..5.txt` (+ `B:\DevDrive\risen-gfx-re\frame.txt` for Execute / Extract / Request /
shadow map), qsort comparators and small getters disassembled with `B:\DevDrive\risen-ghidra\frameB_py\fbdis.py`.
[C] = read in the decompilation / disassembly, [I] = inferred, [?] = open. RVAs are Engine.dll (image base 0x180000000).
Names used below: `RSA` = `eCRenderSystemAdmin` (getter `FUN_180095630`), `GFX` = the gfx admin (`FUN_1800956c0`, `+0x5f8` =
the `eCAPIDevice`, `+0x5e8` = `eCGfxMixerAdmin`), `SMA` = `eCShaderMaterialAdmin` (`FUN_1800adda0`), `IA` =
`eCIlluminationAdmin` (`FUN_18014f330`), `SCN` = the scene / spatial admin (`FUN_1800957e0`). HAL slots by vtable offset
(`src/hal/api_generated.h`): `0xf0` SetViewport (30), `0xf8` GetViewport (31), `0x100` ClearViewport (32), `0x110` BeginScene
(34), `0x118` EndScene (35), `0x150..0x168` Set/GetMasterDepthTesting / Write (42..45), `0x278` SetWorldMatrix (79), `0x460`
SetRenderTarget (140), `0x468` GetRenderTarget (141); texture `+0x20` GetRenderTargetHandle (4), `+0x58`
CopyCurrentRTToImage (11).

Corrections to earlier docs (found here):
* **The refracted list (+0x1578) is drawn before the alpha list (+0x14e8)**, both after `DoBeginRenderAlpha`; the refracted
  list is not sorted (engine-gfx.md 4.2 / 8 had alpha first) [C].
* **The sky is drawn last in the solid phase** (its op lives in list `+0x1530`, after the solid list, the debug list, the
  modules' `OnEndRenderSolid`, decals and billboards), not first (`inject.h` `BeforeSolid` comment says "sky, solid ops ...") [C].
* `eSRenderTargetDesc` = `{ +0 refraction image, +8 depth-map image }` (both `eCGfxImageData*`, API texture at `+0x60`); the
  scene HDR target is bound by the caller (`eCImageFilterScene::OnUpdateOutput`), not by `Execute` [C].
* The main view's extraction / request does not happen in `Execute`: `eCImageFilterScene::OnUpdateOutput` calls
  `eCCameraBase::Update` (extract + pre-execute + request) right before `Execute` [C].
* Point-light shadow maps (packed shadow map) are rendered from **`PreExecute`** (pre-pass list `+0x14d0`), i.e. inside the
  main view's `Update`, before `Execute` [C].

## 1. Ordered call tree

```
eCImageFilterScene::OnUpdateOutput 0x75cf10                     (normal in-game main view; image effect running)
  vt+0x150 (filter enabled?) else return
  cam = app+0x38 (current camera) ?: app+0xc8 (default camera)
  eCCameraBase::Update(cam) 0x4f7270                            == eCCameraBase::Render 0x4f8470 without Execute
    if cam+0x1699 (db 0x15a1): RSA+0x64 ++                      (render frame id)
    DAT_180c628c0 ++                                            (global extraction stamp, entity +0xe0)
    db+0x20 = 0 (item count); db+8 ++ (db frame stamp)
    eCFrustumFilter::UpdateFrameDependencies(db+0x28, db) 0x67ec40
      UpdateCameraObject 0x67e790 (filter camera = APP CURRENT CAMERA, not db+0x14c0)
      FillViewFrustum 0x67e370 (6 planes from proj x view), frustum bounding sphere, z row, depth scale
      eCBeamTree::Init(db+0x118, viewproj, portals = caps 0x15a3) 0x67bc40
    eCFrustumDatabase::ExtractFromFrustum(db) 0x66fa60
      [caps 0x15a3 portals] FindPortalRoom(cam pos) -> room: ExtractFromEntity(room entity, 1),
                            ExtractFromPortalRooms 0x66db40 (recursive, beam clipping), AddPlane(far)
                            occluders (eCPortalAdmin +0x38): CullNode, vt+0x2c8 AddOccluder, qsort 0x66e000, AddPoly
                            rooms seen from outside (eCPortalAdmin +0x20): CullNode, exterior portals -> ExtractFromPortalRooms
      if beam root (db+0x118) != 0: sphere-tree walk FUN_18015c4a0(SCN+0x390, OnQueryNodes 0x66d960,
                                    {db, mask = caps 0x15b0 | (portals ? 6 : 0), value = caps 0x15ac})
        -> ExtractFromEntity 0x66d750 per accepted leaf  (fully-inside subtrees: FUN_18066da10, status 1)
    eCFrustumDatabase::PreExecute(db) 0x66e790                  [caps 0x15aa]
      for each item with caps 0x400: ctx vt+0x38 RequestPrePasses (lights)  -> list +0x14d0
      for each pre-pass op: ctx vt+0x28 RenderPrePass            (dynamic lights update, packed point-light shadow map)
    eCFrustumDatabase::Request(db) 0x66e8c0 -> RequestRenderOps(db, mask) 0x66cfe0
      clear lists +0x14e8 +0x1500 +0x1518 +0x1530 +0x1548 +0x1560 +0x1578 (not +0x14d0)
      for each item i (extraction order): fade = CalculateFadeOut 0x671220; if fade > 0: eCFrustumElementItem::Request 0x671370
        [mask & 0x80] UpdateIllumination(item+0x20); [fade >= eps] [mask & 0x40] item+0xc = db+8
        for each context: [ctx caps & mask & 1] vt+0x30 UpdateLoD;  vt+0x40 RequestRenderOps(db, fade, ctx caps & mask)
  GFX dev: GetRenderTarget(&rt, &ds)
  if filter+0x38 (output image): GetRenderTargetHandle -> SetRenderTarget(handle, ds)   (scene HDR target; failure = log, return)
  eCFrustumDatabase::Execute(cam db, desc = filter+0x40) 0x66e990                     (section 2.6)
  GetViewport(&vp); output ? SetRenderTarget(rt, ds) + SetViewport(vp)
                           : (GFX+0x5b8 && desc.refr+0x60) desc.refr tex->CopyCurrentRTToImage(1, &vp, &vp)

eCCameraBase::Render 0x4f8470   (no image effect): the same Update steps + Execute(db, param)
eCCameraBase::Execute 0x4f7230  Execute(db, desc) only (lists of the last Update)
eCRenderSystemAdmin::Extract 0x66cd80  Update without Execute; ::Execute 0x66cde0 = Execute
eCRenderSystemAdmin::RenderEntity 0x66ceb0  UpdateFrameDependencies, ExtractFromEntities(e, 2) 0x66f790, Execute(db, NULL)
                                           (no PreExecute / Request: the lists of the db's last Request [C], [?] intent)
Shadow map (per face; eCShadowMapBase::RenderShadowMap 0x5a1070: vt+0x128 SetRenderTarget(face), vt+0x120 ClearViewPort,
  vt+0x160 UpdateCamera, vt+0x168 RenderShadowScene -> eCCameraBase::RenderShadowMap 0x4f71c0 (SetCurrentCamera(shadow cam)),
  vt+0x130 ResetRenderTarget):
  eCRenderSystemAdmin::RenderShadowMap 0x66ce00 (GFX valid)
    ExtractShadowMap 0x66f470   UpdateFrameDependencies (filter camera = the shadow camera, now current), db+0x20 = 0,
                                sm vt+0x150 UpdateFrustumFilter (Dir2Ex: beam = light frustum sm+0x100, origin sm+0x80),
                                casters: sm vt+0x140 GetShadowCaster() list ? (IsShadowCasterCullingEnabled ? CullNode : -) ->
                                AddShadowCasterEntity 0x66d860 : sphere-tree walk (OnQueryNodes, as above)
    RequestShadowMap 0x66f5e0   RequestRenderOps(db, 0x10 | (caps 0x15a5 ? 1 : 0))
    sm vt+0x178 OnPostExtraction (base: true) -> ExecuteShadowMap 0x66f630                 (section 2.7)
```

## 2. Per step

### 2.1 `eCFrustumFilter::UpdateFrameDependencies` 0x67ec40 [C]
Filter = db+0x28 (db offset = filter offset + 0x28).
* `UpdateCameraObject`: `cam = app+0x38 ?: app+0xc8`; `f+0x10 = cam`, `f+0x28 = cam+0x10` (view matrix), `f+0x30 = cam+0x50`
  (projection), `f+0x18 = cam+0xc0 (far) + 1500.0` (`DAT_1809f17d4`), `f+0x1c = cam+0xbc` (near), `f+0x20/+0x24 = cam+0xb4/+0xb8`;
  `f+0x38..0x40` = row 2 of the inverted view (view direction).
* `M = proj x view` (`bCMatrix::SetToProduct(out, f+0x30, f+0x28)`); `FillViewFrustum(M)`: `bCFrustum` at `f+0x80`, 6 planes of
  16 B (`+0x80` col3+col2, `+0x90` col3-col2, `+0xa0` col3+col0, `+0xb0` col3-col0, `+0xc0` col3+col1, `+0xd0` col3-col1;
  normalised), `f+0xe0..0xe8` = per-axis n-vertex selectors of plane 0.
* Frustum bounding sphere: radius `f+0x44`, centre `f+0x48` (merge of the 8 corners); its AABB `f+0x54..0x68`.
* Depth-key plane `f+0x6c..0x78` = view matrix column 2 (`v[2], v[6], v[10], v[14]`: view-space z), `f+0x7c = 16777215 / cam+0xc0`.
* `eCBeamTree::Init(f+0xf0, M, portals)`: planes far, left, right, bottom, top, and **near only when portals are off** (portal
  beams replace it); plane array `beam+0x12d0` (data `+0x12d8`, count `+0x12e0`, stride 0x14: normal, d, sign mask).

### 2.2 `ExtractFromFrustum` 0x66fa60 [C]
* Portals (`caps 0x15a3`, default 1): camera room `eCPortalAdmin::FindPortalRoom(db cam+0x90)` 0x672610 (room BSP at room+0x58,
  bounding sphere test then plane walk). Portal = 0x78 B in `room+0x20` (count `+0x28`): 4 corners `+0/+0xc/+0x18/+0x24`, normal
  `+0x30`, box `+0x3c..+0x50`; "camera in the portal" = camera inside the box grown by 2.0 (`DAT_1809f16f8`) -> beam copied
  unclipped; else the portal must face the camera (`dot(cam - p0, n) <= 0` from inside, `>= 0` from outside) and clips the beam
  (`eCBeamTree::AddPoly`). A portal into the exterior (cell is not a room) from inside adds the portal polygon to the db's main
  beam (`db+0x118`): the outdoor extraction is limited to what is seen through it. `ExtractFromPortalRoom` 0x66daa0: each entity
  of the room (`room+0x70`, count `+0x78`) whose world box (vt+0x260) passes `eCBeamTree::Cull` -> `ExtractFromEntity(e, 1)`
  (no CullNode, no view range test), else `RSA+0xac ++`.
* Occluders: `eCOccluder_PS` (portal admin `+0x38`, count `+0x40`): `CullNode(entity+0xd8) != 0` -> vt+0x2c8 `AddOccluder(filter,
  descs, indices)`; descs (0x28 B) qsorted by float `+0x18` descending (0x66e000), added to the beam in reverse order (anti-portals) [C];
  whether the shipped world has occluders / portal rooms [?].
* Sphere-tree walk `FUN_18015c4a0(tree, cb, ctx)` [C]: tree = `SCN+0x390` (`+0x20` root index, `+0x30` nodes, stride 0x58). A
  LIFO stack starts with the root; for a popped node, its children (first `+0x40`, next sibling `+0x30`, -1 = end) are passed to
  the callback in sibling order; a child is pushed when the callback returns true and it has children. Node: `+0x00` sphere
  (radius, centre), `+0x10` entity, `+0x18` u32 flags, `+0x50` byte (`& 4` = leaf).
* `OnQueryNodes` 0x66d960: leaf: if `(node+0x18 & mask) == value` and `s = CullCamera(sphere) != 0`: `ExtractFromEntity(e, s)`;
  returns false. Inner node: `s = CullCamera(sphere)`; `s == 1` (fully inside) -> `FUN_18066da10`: pre-order DFS of the subtree,
  every leaf passing the mask test -> `ExtractFromEntity(e, 1)`; returns `s == 2` (descend). Default `mask = 1 | 6, value = 1`: a leaf
  needs bit 0 and neither bit 1 (set by `SetPortalRoom` 0x670a70 while the entity is in a portal room) nor bit 2 [?: bit 2 meaning]:
  **room entities are only reached through portals**.

### 2.3 `ExtractFromEntity` 0x66d750 [C]
```
if entity+0xe0 == DAT_180c628c0: return true            (already extracted in this pass)
reject if entity+0xf0 == 0 (no render context)
       or (flags+0x28 & 0x8026) != 0x8026 (boundary valid 0x8000, cached in 0x20, enabled 0x04, 0x02)
       or flags & 0x08 (killed)
       or (db+0x15b8 cull callback && !cb(db+0x28, entity))
       or (status != 1 && CullNode(entity+0xd8, db) == 0)
append item = entity+0xd8 to db+0x10 (bTValArray<eCFrustumElementItem*>: data +0x18, count +0x20); entity+0xe0 = DAT_180c628c0
```
`CullNode` 0x6710e0: requires flag 0x8000; `R = caps 0x15a9 ? 0 : ViewRange(entity+0xcc) * caps 0x15b4`;
`CullCamera(box = vt+0x260 GetWorldNodeBoundary, sphere = vt+0x268 GetWorldNodeSphere, R)`. Node bounds: `eCGeometryEntity`
`+0xa0` box / `+0xb8` sphere; **`eCDynamicEntity` `+0x180` box / `+0x198` sphere** (0x69eec0 / 0x69eeb0).
`CullTree` 0x6711a0 (only `ExtractFromEntities`, parents): flag 0x10000, `entity+0xa0` box, `+0xb8` sphere, raw ViewRange
(no factor; `lod.cpp` scales it in a detour).
`CullCamera(box, sphere, R)` 0x67e8e0: `CullCameraSphere(sphere)` (`|c - fc|^2 <= (r + fr)^2` against the frustum sphere) else 0;
`R > 0`: `CullCameraDistance` 0x67e580 with `d^2 = |c - filtercam+0x90|^2`: `d^2 > (r+R)^2` -> 0, `R < r || d^2 > (r-R)^2` -> 2, else 1;
then `b = eCBeamTree::Cull(box)` 0x67ce90 (0 out, 1 in, 2 straddling); distance 2 and beam 1 -> 2. `CullCamera(sphere)` 0x67eb70:
frustum-sphere test + beam on the sphere. `CullCamera(box)` 0x67edf0: box vs the frustum sphere's AABB, then beam. Counters
`RSA+0x98` tests, `+0x9c` sphere-culled, `+0xa0` range-culled, `+0xa4/+0xa8` beam-culled.
`AddShadowCasterEntity` 0x66d860: same stamp; needs flags `0x20` and `(flags & 0x8006) == 0x8006`, not `0x08`, cull callback; **no
`+0xf0` and no CullNode test** (the tree walk / caster list already culled).

### 2.4 `PreExecute` 0x66e790 / pre-passes [C]
`caps 0x15aa` (shadow dbs: 0). `db+0x14e0 = 0`; items with `item+0x10 & 0x400` -> each context vt+0x38; then each op of `+0x14d0`
-> ctx vt+0x28 `RenderPrePass(op, db)`. Contexts with 0x400: `eCRenderContextDynamicLight` (vt 0x9a98b0: one op per db frame,
global stamp `DAT_180c62884`; RenderPrePass 0x59e840 = light PS vt+0x2f0 [I: UpdateEffectedObjects]) and `eCRenderContextPointLight`
(vt 0x9a9908, static and dynamic point lights: RequestPrePasses 0x59e8c0 picks a shadow-cell level `k = int((dist - r) / r) + 1`
(static, needs `PS+0x68`) or `+ 2` (dynamic), `k < 6` -> `AddShadowCells(k, db+0xa8 frustum)`; one op per db frame (`DAT_180c62888`);
RenderPrePass 0x59eaa0 = `IA+0x380` vt+0x170 = `eCPackedShadowMap::RenderShadowMap` 0x5a08f0).

### 2.5 `Request` 0x66e8c0 / `RequestRenderOps` 0x66cfe0 [C]
Request mask (u16) from the caps: `2` solid always; `|4` alpha (`0x1591`); `|0x20` refracted (`0x15a7`); `|1` LoD (`0x15a5`); `|0x40`
visible stamp (`0x15a1`); `|0x80` illumination update (`0x15a8`); `|8` z pre-pass (`0x1590`); `|0x100` debug (`0x15a4`). Shadow:
`0x10 | (0x15a5 ? 1 : 0)`. Radiosity (editor baker): `0x200 | LoD`.
`eCFrustumElementItem::Request` 0x671370: nothing unless `item+0x10 (u16 OR of the contexts' caps) & mask`; bit 0x80 ->
`eCIlluminated_PS::UpdateIllumination(item+0x20)`; then only if `fade >= g_fEpsilon`: bit 0x40 -> `item+0xc = db+8`
(`WasVisibleLastFrame` 0x6715e0; decals test `entity+0xe4 == db+8`), for every context in the list (`item+0x18`, next `ctx+8`):
`caps = ctx+0x20 & mask`; `caps & 1` -> vt+0x30 UpdateLoD; vt+0x40 `RequestRenderOps(db, fade, caps)`. Contexts are appended at the
list head by `CacheIn` 0x670e60 (property sets in reverse order; a debug context `0x140` is added when `RSA+0x1c0` = RenderDebug).

Context caps (`ctx+0x20`, from `CreateRenderContext`):

| context (vtable) | caps | lists it writes |
|---|---|---|
| Mesh (0x9ae070) | 0x27f | solid, alpha, refracted, z, (0x200 radiosity -> solid) |
| Animation2 (0x97b1f8) | 0x7f | refracted, solid, alpha, z |
| SpeedTree (0x9debe8) | 0x5b | solid (batched), z (batched) |
| SpeedTreeBB (0x9df3f0) | 0x4a | solid, z (pre-pass bit 8 only: no shadow) |
| Vegetation (0x9e31f0) | 0x4b | solid, z (bit 8 only) |
| Billboard, Particles (0x9b7180), Strip (0x9e2448), LightStreaks (0x9abd10) | 0x46 | alpha (billboards: solid ones queued in `eCBillboardAdmin`) |
| Precipitation (0x9e7a18) | 0x46 | `+0x1548` (late alpha) |
| Skydome (0x9ddd90) | 0x46 | `+0x1530` (solid bit), `+0x1548` (alpha bit, empty RenderAlpha) |
| DynamicLight / PointLight / StaticPointLight | 0x400 | pre-pass `+0x14d0` |
| Debug (0x9c9b00) | 0x140 | debug `+0x1560` (bit 0x100) |

Context vtable slots: `+0` Render, `+8` RenderAlpha, `+0x10` RenderRefracted, `+0x18` RenderZPass, `+0x20` RenderRadiosityPass,
`+0x28` RenderPrePass, `+0x30` UpdateLoD, `+0x38` RequestPrePasses, `+0x40` RequestRenderOps (empty base `0x7faa0`).

**`eCRenderContextMesh::RequestRenderOps` 0x5c75e0** (res = `ctx+0x28`, mesh PS = `ctx+0x18`, entity = `ctx+0x10`):
```
m = fade == 1.0f ? caps : caps & ~0x1a                    (a fading mesh: no solid, z, shadow)
res state (+0x38): 1..2 QueueData(res, 1), 3 CacheResource; continue only if 3; res+0x24 ++ (busy) ... -- at the end
key = clamp24(((dot(f+0x6c.., sphere.c) + f+0x78) - sphere.r) * f+0x7c)    (sphere = entity vt+0x268)
allZ = true
for sub-mesh i (res+0x58, stride 0x48, count res+0x60), mat = sub+0x38 (null: skipped):
  touch mat (QueueData / CacheResource)
  s = mat+0x70 (eCShaderBase)
  if s && s->IsRefracted() (vt+0x1a8):          mi = m & 0xffe1   (refracted, 0x200)
  elif (s && s->IsBlended() (vt+0x198)) || fade != 1: mi = m & 0xffc5   (alpha, 0x200)
  else:                                          mi = m & 0xffdb   (solid, z 0x08, shadow 0x10, 0x200)
  if ctx+0x34 == 0 (no hierarchy):
    mi & 2:  res+0xa0 == 0 -> solid op (desc i, key, fade field 1.0)
             res+0xa0 == 1 (instanced) -> once per resource per db frame (res+0xa4 != db+8): free res+0xb0 instances,
                                          solid op desc 0xffffff00 | i (fade 1.0); flag "instanced"
    mi & 4:  alpha op (desc i, key, fade)     mi & 0x20: refracted op (desc i, key, fade)    mi & 0x200: solid op (fade)
    allZ &= (mi & 0x18) != 0
  else: box tree of the sub-mesh (sub+0x40, nodes 0x28 B: +0 skip pointer, +8 box, +0x20 first primitive): walk, CullCamera(box):
        0 -> skip subtree; leaf, inside (1), or straddling (2) with fewer than ctx+0x34 * 3 indices below -> ops for the node
        (desc node << 8 | i) into solid / alpha / refracted / z (0x18) / 0x200 as above; else descend
if ctx+0x34 == 0 && (m & 0x18):   z ops (list +0x1518), fade field = fade:
  allZ -> ONE op desc 0xff (whole mesh, at the first sub-mesh that is neither blended nor refracted)
  else -> one op per sub-mesh j that is neither blended nor refracted (desc j)
if instanced: AddRenderInstance(res, entity+0x60, meshPS+0x20, illum = entity+0xf8 ? +0x78 : NULL,
                                flags = (ef & 0x400000 ? 1) | (ef & 0x200000 ? 2) | (ef & 0x800000 ? 4)); res+0xa4 = db+8
```
`IsBlended` 0x6d2ef0: `TradeAlphaAsSolid (+0x79) -> false`; blend mode `+0x50 != 0 -> mode != 1`; mode 0 -> `HasOpacity()` (vt+0x190;
`eCShaderDefault`: GUID `+0x320` valid) `|| IsRefracted()`. `IsRefracted`: `eCShaderDefault` 0x6c9d20 = refraction-source GUID
`+0x410` valid, base false. `IsMasked` = mode 1.

`eCRenderContextMesh::UpdateLoD` 0x5c74b0: `n = meshPS+0x6c`, only if `n > 1`; `d2 = |sphere.c - db cam(+0x14c0)+0x90|^2`;
`ctx+0x28 = 0`; for `b = n-1 .. 0`: `R_b = b < n-1 ? meshPS+0x60[b] : +inf`, `res_b = meshPS+0x40[b]`; `want = d2 <= R_b^2 ? res_b
: want`; if `res_b` loaded and (`d2 <= R_b^2` or none chosen yet) `ctx+0x28 = res_b`. Result: the finest loaded level whose range
covers d (else the coarsest loaded one); the finest wanted level is prefetched (`PrefetchData`) if not loaded. **The camera is
the database's own**: shadow dbs re-pick the LOD from the shadow camera (writes the shared `ctx+0x28`).

Other emitters (all [C]):
* Animation2 0x4ddd00: key from the node sphere; `FillRenderOpActor` 0x4dd8b0 for the main actor (`PS+0x30`, needs `+0x38`) and each
  body part (`PS+0x1b0`, u16 count `+0x1b8`): primitives of the current LOD (`actor+0x250` LOD, table `hra+0x38` stride 0x14:
  first u16 `+0x38`, count `+0x3a`; material at primitive `+0x10`, array `hra+0x90` stride 0x20). Classification as the mesh
  (refracted / blended or fade != 1 / solid); ops: refracted (**fade field left 1.0**), solid, alpha (fade), z (`0x18`). desc =
  `primitive << 16 | actor index` (0 = main, k = body part k-1). UpdateLoD empty.
* SpeedTree 0x6ee4e0 (`s_bRenderingEnabled`, resource `PS+0x38` loaded): solid bit: once per db frame (buffer `res+0x78`, stamp
  `+0xe0`): queue count `+0xc0 = 0`, one solid op per geometry slot `i < 4` with a material (`buf+0x80+8i`, present if `buf+0x18+8i`),
  desc i, key 0xffffff; every tree `AddToRenderQueue(buf, PS)`. `0x18`: once per db frame (`+0xe4`), `+0xd8 = 0`, z op per slot, desc
  `mask << 16 | i`, key 0xffffff; `AddToZPassRenderQueue(buf, PS, key)`. `mask & 0x1a` and `admin+0x68 != db+8` ->
  `eCSpeedTreeAdmin::UpdateWindMatrices`. No alpha, no fade: SpeedTrees ignore the fade value.
* SpeedTreeBB 0x6ee9b0: per sub-mesh box tree as the mesh hierarchy (threshold `ctx+0x28`), solid / z (bit 8) ops desc `node<<8|i`,
  key 0.
* Vegetation 0x70d4f0: only with `caps 0x15a6` and the vegetation admin enabled (`FUN_180265df0()+0x10`); visible grid nodes = cache
  rect of the camera, sphere (db cam pos, `grid+0x14`) vs node box `+0x44`, `CullCamera(box)`; nodes qsorted (0x118 B,
  `SortGridNodes`); one solid and one z op (bit 8) per used mesh i, desc i, key 0, sort id from its material.
* Billboard 0x4f1400: blended material or fade != 1 -> alpha op (desc 0, fade); solid -> `eCBillboard_PS::Render` (queue,
  drawn in `RenderBillboards`). Particles 0x5d33a0: alpha op, material `PS+0x450`, key from entity `+0x90` (translation) minus
  `PS+0x2a0`. Strip 0x701170: alpha op, material `PS+0x120`. LightStreaks 0x5c1880: alpha op, material `PS+0x70`.
  Precipitation 0x719b40: `+0x1548` op (desc 0, fade 1, sort 0). Skydome 0x6e8a30 (`PS+0x18 & 1`): solid bit -> `+0x1530` op
  desc 0 (Render 0x6e8a10 = the dome); alpha bit -> `+0x1548` op desc 1 (RenderAlpha 0x6e8a00 empty).
* Debug 0x67f4f0: debug op (desc 0, sort 0, fade); copies caps `0x1598/99/9a` to `ctx+0x28/29/2a` (lines to children, -, axes).

### 2.6 `eCFrustumDatabase::Execute` 0x66e990 [C]
```
RSA stats: +0xb0 += items, +0xbc += z ops, +0xc0 += solid + 0x1530, +0xc4 += alpha + 0x1548, +0xc8 += refracted
ExecuteGfxCamera(db+0x14c0) 0x4f6ad0: GetViewport; SetViewport(cam rect +0xc4, z 0..1); eCGfxCamera::Execute(cam+0x1738)
eCGfxFog::Execute(db+0x14c8)
dev.GetRenderTarget(&rt0, &ds0); dev.GetViewport(&vp)                       [vp argument: I]
h = 0; if desc && desc+8 (depth image) && image+0x60 (API tex):
        image+0x24 == 1 (cube) ? tex.GetRenderTargetHandle(6, &h) : image+0x20 == 0 ? tex.GetRenderTargetHandle(&h) : -
dev.ClearViewport(h ? 1 : 7, 0xff000000, 1.0, 0)                            (ms_pRegisterViewportClearCall hook first)
SMA depth map (+0x290) released, = NULL
if caps 0x1590 (z pre-pass):
  dev.GetRenderTarget(&rtS, &dsS); dev.SetRenderTarget(h, dsS) (h may be NULL); dev.SetViewport(vp)
  dev.ClearViewport(7, 0xffffffff, 1.0, 0)                                    (depth map white = far, z = 1, stencil 0)
  SMA.BeginZPass(NULL, true, h != 0) 0x6b9720
  qsort(+0x1518, 0x18, 0x66e670); for op: ctx vt+0x18 RenderZPass(op, db)
  SMA.EndZPass() 0x6b9910; dev.EndScene(); dev.SetRenderTarget(rtS, dsS); dev.SetViewport(vp)
SMA.SetDepthmap holder (+0x270) = desc ? desc+8 : NULL                        (FUN_1806bc540: AddRef new, Release old)
IA.UpdateShadowMask() 0x5748d0
dev.BeginScene(); SMA.BeginMaterial() 0x6b9d20
if !caps 0x15a0: eCModuleAdmin::DoBeginRenderSolid 0x4cb810 (enabled modules, vt+0x150 if overridden)
qsort(+0x1500, 0x18, 0x66e6a0); for op: ctx vt+0 Render(op, db)
if debug count (+0x1570) > 0: SMA.EndMaterial; debug ops vt+0; SMA.BeginMaterial
if caps 0x1594 (boxes, ToggleBoundingBoxes 0x66c3b0): save GetMasterDepthWrite / Testing; SetMasterDepthWrite(2),
   SetMasterDepthTesting(4) (devirtualized: D3DRS_ZWRITEENABLE 1, D3DRS_ZFUNC 4 = LESSEQUAL); SetWorldMatrix(identity);
   items in reverse: DrawBox(mode 2 ? entity+0xa0 : vt+0x260 box) 0x66d0c0; restore
if !caps 0x15a0: every enabled module vt+0x158 OnEndRenderSolid (no override check)
eCDecalAdmin::RenderDecals(db) 0x4fc220     (decals whose entity has +0xe4 == db+8; queue qsorted, SortRenderQueue)
eCBillboardAdmin::RenderBillboards() 0x4f0530 (queue qsorted)
list +0x1530 (unsorted): ctx vt+0 Render                                       (the sky dome)
if desc && desc+0 (refraction image):
  dev.EndScene(); SMA.EndMaterial()
  if GFX+0x5b8 && refr+0x60: refr tex.CopyCurrentRTToImage(1, &vp, &vp)       [rect args: I]
  SMA refract map holder (+0x248) = refr (SetRefractmap); dev.BeginScene(); SMA.BeginMaterial()
if !caps 0x15a0: DoBeginRenderAlpha 0x4cb710 (vt+0x160)
list +0x1578 refracted (unsorted): ctx vt+0x10 RenderRefracted
qsort(+0x14e8, 0x18, 0x66e600); for op: ctx vt+8 RenderAlpha
if !caps 0x15a0: DoEndRenderAlpha 0x4cb680 (vt+0x168)
list +0x1548 (unsorted): ctx vt+8 RenderAlpha                                  (rain, sky alpha stub)
dev.EndScene(); SMA.EndMaterial(); SMA refract map (+0x268) released, = NULL
```
Module overrides (all DLLs' exports): `eCPhysicsScene::OnEndRenderSolid` 0x5e0a80 (jmp 0x5e5100, [I] physics debug draw),
`gCHudModule2::On{Begin,End}Render{Solid,Alpha}` (Game.dll 0x83cdb0..0x83cde0) [?: bodies not read]. Where the z pass's
`BeginScene` comes from (BeginZPass or an outer scene) [?].

### 2.7 `ExecuteShadowMap` 0x66f630 [C]
`RSA+0xb4 += items, +0xb8 += z ops`; `ExecuteGfxCamera(db+0x14c0)`; `dev.BeginScene()`; `qsort(+0x1518, 0x66e670)` (front to back
from the shadow camera); `SMA.BeginZPass(shadow map as eCDepthMap, true, false)`; every op `vt+0x18 RenderZPass(op, db)`;
`SMA.EndZPass()`; `dev.EndScene()`. No clear (the shadow map's `ClearViewPort` vt+0x120 before). Shadow-camera dbs (set by
`eCShadowMapBase::Create` 0x5a0eb0 after `UpdateFromAppTemplate`): z pre-pass 0, alpha 0, stamp 0, portals 0, illumination update
0, pre-passes 0, LoD 1. Cull callbacks (`SetEntityCullCallback`, db+0x15b8; illumination `entity+0xf8`):
`eCCascadeShadowMap::CullEntityCallback` 0x59ec60 = `(CastDirLightShadows +0x40 || CastDirLightShadowsOverwrite value +0x50 == 1)
&& ShadowCasterType (+0x38) in {1, 2}`; `eCDirectionalLight_PS::CullTSEntityCallback` 0x592300 = same with type in {0, 1};
`eCPackedShadowMap::CullEntityCallback` 0x5a0150 = `(CastPntLightShadows +0x41 || overwrite +0x60 == 1) && type == 2`.
The z list of a shadow db holds the same ops as the main z pass (meshes: non-blended, non-refracted sub-meshes, fade == 1 only).

## 3. Data structures [C]

**`eCFrustumDatabase`** (embedded in `eCCameraBase` at `+0xf8`: camera offset = db offset + 0xf8; vtable 0x9c9a10; ctor 0x6706c0;
`Create` 0x66e160):

| off | field |
|---|---|
| `+0x08` | frame stamp, `++` per extract; initial value spread per db (`+= 123456` per created db: stamps of different dbs never meet) |
| `+0x10` | `bTValArray<eCFrustumElementItem*>` extracted items (data `+0x18`, count `+0x20`) |
| `+0x28` | `eCFrustumFilter` (to `+0x118`; `+0x6c..+0x7c` depth key = db `+0x94..+0xa4`, frustum = db `+0xa8`) |
| `+0x118` | `eCBeamTree` (root pointer at `+0`) = filter `+0xf0` |
| `+0x13e8` | beam scratch array (portal pass) |
| `+0x14c0` | `eCCameraBase*` (the db's own camera; shadow cams: self) |
| `+0x14c8` | `eCGfxFog*` (created in Create: colour (32,32,32), start 3000, end 20000, density 1.0, `+0x24` = 3) |
| `+0x14d0` | pre-pass ops; `+0x14e8` alpha; `+0x1500` solid; `+0x1518` z (pre-pass / shadow); `+0x1530` solid-late (sky); `+0x1548` alpha-late (rain); `+0x1560` debug; `+0x1578` refracted. Each `bTValArray<eCRenderOp2>`: vtable `+0`, data `+8`, count `+0x10`, capacity `+0x14` (growth max(4, min(1024, cap/8)), helper 0xae210 / append 0xae1e0) |
| `+0x1590` | `eSCaps` (`GetCaps` 0x66f8b0; template `RSA+0x70`, copied by `UpdateFromAppTemplate` 0x6708a0, which also sets the camera's ClipDepth.y `cam+0xc0` = caps far) |
| `+0x15b8` | entity cull callback (not part of the caps) |

`eSCaps` (db offset: default; source):

| off | type | meaning |
|---|---|---|
| `0x1590` | bool | z pre-pass + request bit 8 (1) |
| `0x1591` | bool | alpha, request bit 4 (1) |
| `0x1594` | int | debug boxes 0 / 1 node box / 2 tree box (0; `ToggleBoundingBoxes` flips main camera's) |
| `0x1598..0x159a` | bool x3 | debug-context flags (0) |
| `0x159c` | float | far clip (10000; `ApplyConfiguration` 0x66c3e0: `/Engine/Render/zFar{High,Medium,Low}` by `/Engine/Render/ViewDistance` [I: key order]) |
| `0x15a0` | bool | no module callbacks (0) |
| `0x15a1` | bool | RSA frame id `+0x64` ++ and request bit 0x40 (1) |
| `0x15a2` | bool | (0) [?: no reader found] |
| `0x15a3` | bool | portals (1) |
| `0x15a4` | bool | debug ops, request 0x100 (`/Engine/Render/RenderDebug` or app vt[0xa1]) |
| `0x15a5` | bool | LoD update, request bit 1 (1) |
| `0x15a6` | bool | vegetation (`/Engine/Render/VegetationRenderingEnabled` [I]) |
| `0x15a7` | bool | refracted, request 0x20 (1) |
| `0x15a8` | bool | illumination update, request 0x80 (1) |
| `0x15a9` | bool | ignore view ranges (0) |
| `0x15aa` | bool | pre-passes (1) |
| `0x15ac` / `0x15b0` | u32 | tree leaf flag value / mask (1 / 1) |
| `0x15b4` | float | view range factor (1.0; `lod.view` writes it, lod.cpp) |

**`eCFrustumElementItem`** (entity `+0xd8`): `+0x00` entity, `+0x08` extraction stamp (entity `+0xe0`), `+0x0c` visible stamp
(entity `+0xe4`), `+0x10` u16 caps (OR of the contexts' `+0x20`), `+0x18` context list head, `+0x20` `eCIlluminated_PS*`,
`+0x28` portal room.
**Render context** (base): `+0` vtable, `+8` next, `+0x10` entity, `+0x18` property set, `+0x20` u16 caps. Mesh: `+0x28`
current `eCMeshResource2*` (LOD), `+0x34` hierarchy threshold (`meshPS+0x28` when `meshPS+0x24 == 1`, else 0; `Create` 0x5c7360).
**`eCRenderOp2`** (0x18 B, `Create(mat, ctx, desc, key)` 0x67f680): `+0x00` ctx; `+0x08` u32 desc; `+0x0c` float fade (Create: 1.0;
callers overwrite for alpha / refracted / z); `+0x10` u32 depth key (24 bit); `+0x14` u32 sort id = `mat+0x78 & 0xfff |
shader->GetAdditionalSortID() (vt+0x1b0) << 12 | shader->RenderPriority (+0x7c) << 20` (0 without material).
`GetAdditionalSortID`: `eCShaderDefault` 0x6c9d40 = `byte +0x2b8 | 0x10` [?: meaning of +0x2b8]; base 0. desc per context: section 2.5.
**Render instance** (`eCMeshResource2 +0xa8` array: data `+0xb0`, count `+0xb8`; 0x58 B; `AddRenderInstance` 0x7602a0, refuses a
NULL illumination with a log line): `+0x00` world matrix (64 B), `+0x40` int variant (`meshPS+0x20`), `+0x48`
`eSShaderMaterialIllumination*`, `+0x50` byte flags. Resource `+0xa0` instancing flag, `+0xa4` stamp (db frame at request; Render
zeroes it after sorting).

## 4. Rules as formulas [C]

* **Depth key** (every emitter except SpeedTree 0xffffff, SpeedTreeBB / vegetation 0, precipitation / sky 0):
  `key = clamp(int(((v2 . c + v14) - r) * 16777215 / far), 0, 0xffffff)`, `v2 / v14` = view matrix column 2 of the filter
  camera, `(c, r)` = `GetWorldNodeSphere` of the entity (particles: entity translation and `PS+0x2a0`).
* **Z / shadow list order** (0x66e670): `key` ascending (front to back); qsort, not stable.
* **Solid order** (0x66e6a0): u64 at `+0x10` ascending = `sortId` (u32 compare) then `key` (front to back). So: RenderPriority,
  then additional sort id, then material id (12 bit), then depth.
* **Alpha order** (0x66e600), `L = sortId & 0xfff00000` (RenderPriority << 20): if both `L < 1000 << 20`: `key` descending (back to
  front), ties `sortId` ascending. Else `L` descending first (a priority >= 1000 is drawn **before** ordinary alpha; between two
  such, the higher first), then `key` descending, then `sortId` ascending.
* **Refracted, `+0x1530`, `+0x1548`, debug, pre-pass**: request order = extraction order (portal rooms, then the tree walk).
* **Fade** (`CalculateFadeOut` 0x671220): `base = entity+0xc8`; `caps 0x15a9` -> base; `R = entity+0xcc * caps 0x15b4`; `R <= eps`
  -> base; `d^2 = |sphere.c - dbcam+0x90|^2`; `d^2 > R^2` -> 0; `d^2 > (0.9 R)^2` -> `base * (1 - (d - 0.9R) / (0.1 R))`; else base.
  `fade <= 0`: item not requested at all; `fade < eps`: Request does nothing.
* **Opaque requires `fade == 1.0f` exactly** (meshes, animation, billboards): otherwise solid / z / shadow bits are dropped and
  every non-refracted part becomes an alpha op carrying `fade` (global alpha). Fading objects cast no shadow and have no z pass.
* **View range cull** (CullNode, partial nodes only): `d > r + R` -> culled (`d` from the filter camera); fully-inside tree nodes
  and portal-room entities skip it and rely on the fade.
* **LOD**: section 2.5 (distance from the db camera to the node sphere centre; ranges `meshPS+0x60[b]`, last = infinite).
* **Instance batches** (`eCRenderContextMesh::Render` 0x5c7110, desc `0xffffff00|i`): if `res+0xa4`: qsort instances
  (`SortRenderInstances` 0x75f690: light-list pointer `*illum`, variant, flags byte, then `illum+0x90` static-SH byte), `res+0xa4 = 0`;
  `ExecuteRenderInstances(i)` 0x7607b0: `BindLightmaps(res, 1)`; if not instanced, < 2 instances or lightmap combi == 1: each
  instance through `eCSubMesh::Render` (sub-mesh i, variant, combi, illum, alpha 1.0, matrix, flags). Else batches: consecutive
  instances with equal light list, variant, flags, outdoor factor `illum+8`, none with static lights (`illum+0x0d`) or static SH
  (`illum+0x90`) (such an instance is a batch of its own), at most `GetMaxInstanceCount()` (vt+0x200) per batch; per instance k
  `SetWorldMatrix(m, k)` (vt+0x208), `SetGlobalAlpha(1.0, k)` (vt+0x210); per batch streams 0..2 frequency `0x40000000 | n`
  (`D3DSTREAMSOURCE_INDEXEDDATA`), draw description (triangle list, sub `+0x30` base vertex, `+0x28` start index, `+0x34` vertices,
  `+0x2c / 3` primitives), `GetPasses(variant, illum, 0, true, flags)` + `ExecutePass` + `eCGfxVisual::Execute`, frequencies back
  to 1. Instancing exists only in the solid list; the z / shadow ops of instanced meshes are per entity.
* Non-instanced mesh op: `eCMeshResource2::Render(res, entity+0x60, sub (0xff -> -1 = all), node, variant meshPS+0x20, illum,
  &op.fade, flags byte)` (per-context Render: other slice).

## 5. For a native implementation

A native list builder that replaces the engine's must reproduce, per view (main, each shadow face):
1. **The candidate set**: the cached-in entities of the scene tree whose leaf flags pass `(flags & mask) == value` (main: bit 0 set,
   bits 1 / 2 clear), plus the portal traversal of the camera's room (entities of rooms only through visible portals), plus the
   entity rules of 2.3, plus the view's cull callback (shadows: the caster-type rules of 2.7).
2. **Culling**: node sphere / box (`GetWorldNodeSphere/Boundary`: geometry `+0xb8/+0xa0`, dynamic `+0x198/+0x180`), view range,
   the beam (frustum without near plane when portals are on; portal / occluder clipping).
3. **Fade and the class of each part** (section 4): fade == 1 exactly, `IsRefracted` / `IsBlended` (with TradeAlphaAsSolid and
   HasOpacity), refracted parts get no z op, blended ones no z / shadow op.
4. **The lists and their orders** (section 4), whole-mesh z ops (desc 0xff), instance batching (section 4), SpeedTree / vegetation
   batching per resource per view.
5. **Side effects of Request** the engine relies on (keep them if the engine's extraction is ever bypassed): LOD selection
   (`ctx+0x28`, per view camera), `UpdateIllumination` (bit 0x80; Dynamic illumination is only computed when requested), the visible
   stamp `entity+0xe4` (decals), resource-cache touches of mesh resources and materials, SpeedTree wind matrices, the render-instance
   arrays, the pre-passes (dynamic lights, point-light shadow cells).

**Deviations of `src/enh/native_opaque_list.cpp` (`NativeOpaque::buildList`) from the engine** (main view, z pass + solid only):

| # | engine | native | effect |
|---|---|---|---|
| 1 | candidates = tree walk + portal traversal; room entities only through visible portals; leaf flag mask | every mirrored entity with meshes | superset (indoor rooms drawn without portal culling); harmless for depth, costs draws [I] |
| 2 | node sphere / box via vt+0x268 / +0x260: dynamic entities `+0x198 / +0x180` | sphere `+0xb8` for every entity (`engine/scene_mirror.cpp:39`) | fade distance and frustum test of `eCDynamicEntity` meshes use the tree sphere (children included) [C: offsets] |
| 3 | frustum sphere pre-test, beam planes (GL-style near, no near with portals), box test | 6 D3D planes on the sphere | superset at the near plane; harmless |
| 4 | opaque iff fade == 1.0f exactly | `alpha < 0.999` or `d > 0.9 R` -> engine | alpha in [0.999, 1) is blended in the engine, opaque natively (drawn twice: our opaque + the engine's alpha op) [I: rare] |
| 5 | base alpha `+0xc8` and ViewRange read every frame | mirror `alpha` / `viewRange` refreshed every few frames | an entity fading in / out can be listed for a few frames while the engine draws it as an alpha op: double draw [I] |
| 6 | `IsRefracted` (GUID `+0x410`) -> refracted only; `IsBlended` = `!TradeAlphaAsSolid && (mode >= 2 \|\| mode 0 && (HasOpacity \|\| refracted))` | Default / Leafs with blend mode <= 1 opaque / masked; "Water" in the material name skipped | a mode-0 material with an opacity source (`+0x320` GUID) is blended in the engine and opaque natively (double draw); refracted materials not named "Water" would be drawn natively [C: rules; I: frequency] |
| 7 | `res+0xa0 == 1` meshes: one instanced solid op per (resource, sub-mesh) per view, z ops per entity | `+0xa0` not checked: every instance's parts listed | the engine's z draws of these parts are skipped (same key), its instanced solid draw is a different draw (kind not StaticMesh) and probably not skipped -> drawn twice in the colour pass [?: verify with `-LogDraws`] |
| 8 | z list: front to back by the 24-bit view-z key (centre z - r), opaque and masked mixed, whole-mesh ops | Euclidean centre distance, opaque class before masked | only early-z efficiency differs |
| 9 | solid list: sort id (priority, additional id, material id) then key; TradeAlphaAsSolid layers interleave by sort id | our base surfaces all drawn at `BeforeSolid`, before every engine solid draw | a layer whose sort id is lower than its base's is overwritten by the base in the engine (LESSEQUAL) but stays visible over our base [I: check terrain layers' sort ids] |
| 10 | mesh hierarchy (`ctx+0x34 > 0`): per-node box culling, desc `node<<8 \| i` | whole sub-mesh | superset; also the engine's per-node draws of such meshes have other first / count than the listed part -> not skipped [?: are hierarchical meshes in the shipped data] |
| 11 | LOD from the db camera at Request; shadow dbs re-pick from the shadow camera first (same `ctx+0x28`) | live `ctx+0x28` checked against the mirror's copy | the mirror's copy can hold the shadow camera's choice (taken mid-frame) -> `listStale` parts left to the engine [I] |
| 12 | `caps 0x15a9` (ignore view range) honoured | not read | none in game (0) |

Native shadows (`native_shadows.cpp`, not reviewed in detail): the engine's casters additionally require fade == 1 (fading entities
cast nothing), the caster-type callbacks of 2.7, and use LODs chosen from the shadow camera.

## 6. Open

* Tree leaf flag bit 2 (excluded with portals) and who sets bit 0; the shipped world's portal rooms / occluders (count at
  `eCPortalAdmin +0x28 / +0x40` in game).
* `caps 0x15a2`; `eCShaderDefault +0x2b8` (additional sort id); `eEShadowCasterType` value names (cascades accept 1, 2; terrain
  shadows 0, 1; packed 2).
* `GetRenderTargetHandle(6, ..)` for a cube depth map; `eEViewportClear` bits (1 = colour [I], 7 = all); the viewport / rect
  arguments the decompiler dropped in `Execute` (GetViewport, CopyCurrentRTToImage).
* Which `BeginScene` opens the z pass; module overrides in Game.dll (`gCHudModule2`).
* Whether terrain layers are `TradeAlphaAsSolid` (they are drawn in the solid phase [I]) and their sort ids vs their bases.
* `RenderEntity` executes without a Request (stale lists?) [?].
* In-game checks worth one run: `RSA+0x98..0xc8` counters per frame vs our draw counts; `res+0xa0` meshes in the native list
  (deviation 7); the sky op position (`-LogFrame`: last draws of the Solid segment).
