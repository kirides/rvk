# D. Render contexts other than plain static meshes (RE, 2026-10-27)

Slice D of the engine render-sequence decompile: skinned actors, SpeedTree (3D + billboards), vegetation / grass, particles,
strips, light streaks, billboards, decals, precipitation, the debug context. The frustum database itself, the static-mesh
material system, shadow setup, sky / post / GUI are other slices (A-frame.md and siblings). Headless Ghidra on Engine.dll (clone
`clones\EngineD`), dumps `B:\DevDrive\risen-ghidra\frameD_dump1..3.txt` (+ reused `nativeskin_dump*`, `nativemesh_dump*`);
vtables with `risen-gfx-re\py\vt.py`, unexported vtables found from RTTI with `risen-ghidra\frameD_rtti.py`, the VS constant
tables of the corpus in `risen-ghidra\frameD_vs_ctab.txt` (from `game-shaders/*.vert`). [C] = read in the decompilation, [I] =
inferred, [?] = open. RVAs are Engine.dll. Float constants read from the image: `DAT_1809f1698` = 1.0, `..1854` = -1.0,
`..1560` = 1/256, `..1790` = 80, `..17cc` = 1000, `..166c` = 0.8, `..17b4` = 255, `..159c` = 0.04, `..1684` = 0.99,
`..16f8` = 2.0, double `..16e0` = 0.9.

Singletons: `FUN_1800adda0` = `eCShaderMaterialAdmin`, `FUN_180265df0` = `eCVegetationAdmin`, `FUN_18025a6d0` =
`eCSpeedTreeAdmin`, `FUN_1800cd140` = `eCBillboardAdmin`, `FUN_1801ac230` = `eCParticleModule`, `FUN_18014f330` =
`eCIlluminationAdmin`, `FUN_1800956c0` = `eCGfxAdmin`.

## 0. Common machinery

### 0.1 Render contexts and their vtables [C]

Context object (0x28 bytes, built by the property set's `CreateRenderContext`, vslot `+0x150` of the PS): `+0` vtable, `+8`
next, `+0x10` entity (null for template PSs: `PS+0x18 & 0x10`), `+0x18` property set, `+0x20` u16 caps (Animation2 `0x7f`,
SpeedTree `0x5b`, particle / strip / streaks / billboard / precipitation `0x46`; use [?]: whether the database ANDs it with the
request mask is slice B's). Vtable slots: `+0x00` Render (solid), `+0x08` RenderAlpha, `+0x10` RenderRefracted, `+0x18`
RenderZPass, `+0x20` RenderRadiosityPass (mesh only), `+0x30` UpdateLoD, `+0x40` RequestRenderOps(db, fade, mask), empty
slots = `0x7faa0` (no-op).

| context (vtable) | class / emitter | slots used | lists |
|---|---|---|---|
| `eCRenderContextAnimation2` (`0x97b1f8`) | `eCAnimation_PS::CreateRenderContext` 0x4d7190 | 0 `0x4ddc10`, 8 `0x4ddc60`, 0x18 `0x4ddcb0`, 0x30 `0x4ddcf0` (empty), 0x40 `0x4ddd00` | solid, alpha, refracted, z |
| `eCRenderContextSpeedTree` (`0x9debe8`) | `eCSpeedTree_PS` 0x6e8c60 | 0 `0x6ee320`, 0x18 `0x6ee3f0`, 0x30 `0x6ee4d0`, 0x40 `0x6ee4e0` | solid, z (+ shadow) |
| `eCRenderContextSpeedTreeBB` (`0x9df3f0`) | `eCSpeedTreeBBMesh_PS` 0x6ea370 (`Create` 0x6ee8b0) | 0 `0x6ee8d0`, 0x18 `0x6ee8e0`, 0x40 `0x6ee9b0` | solid, z |
| `eCRenderContextVegetation` (`0x9e31f0`) | `eCVegetation_PS` 0x7081c0 | 0 `0x70d4b0`, 0x18 `0x70d4d0`, 0x40 `0x70d4f0`, 0x50 IsPrefetching | solid, z |
| `eCParticleRenderContext` (`0x9b7180`, unexported) | `eCParticle_PS` 0x5d9730 | 8 `0x5d3360`, 0x40 `0x5d33a0` | alpha |
| `eCStripRenderContext` (`0x9e2448`) | `eCStrip_PS` 0x6ff060 | 8 `0x701120`, 0x40 `0x701170` | alpha |
| `eCLightStreaksRenderContext` (`0x9abd10`) | `eCLightStreaks_PS` 0x5c1f80 | 8 `0x5c1840`, 0x40 `0x5c1880` | alpha |
| `eCBillboardRenderContext` (`0x982810`) | `eCBillboard_PS` 0x4f1900 (none if `PS+0x94`) | 8 `0x4f13c0`, 0x40 `0x4f1400` | alpha, or the billboard admin queue |
| `eCPrecipitationRenderContext` (`0x9e7a18`) | `eCPrecipitation_PS` 0x71b9a0 | 8 `0x719b00`, 0x40 `0x719b40` | **post-alpha list `+0x1548`** |
| `eCRenderContextDebug` (`0x9c9b00`) | entity debug | 0 `0x67f070`, 0x40 `0x67f4f0` | debug `+0x1560` |

(draw-tags.md 4.1 lists the particle / strip contexts as "without exported vtable": the table above names them; their PS-level
`Render` functions are what our detours catch.) The empty base vtable `eCRenderContext` is `0x97b250`.

### 0.2 Request mask (`eCFrustumDatabase::Request` 0x66e8c0) [C]

`mask = 2` (solid, always) `| 4` if `db+0x1591` (alpha) `| 0x20` if `+0x15a7` (refracted) `| 1` if `+0x15a5` `| 0x40` if
`+0x15a1` `| 0x80` if `+0x15a8` `| 8` if `+0x1590` (z pre-pass) `| 0x100` if `+0x15a4` (debug); bits 1 / 0x40 / 0x80 are not
read by any context of this slice [?]. **`0x10` = shadow-map z pass** [C: SpeedTree reads it back from its z op, 2.4; [I] it
is the mask of `RequestShadowMap`]: contexts treat `mask & 0x18` as "z list wanted".

### 0.3 Render ops these contexts create [C]

`eCRenderOp2` (engine-gfx.md 8): `+0` context, `+8` desc (u32, meaning per context), `+0xc` fade, `+0x10` 24-bit depth key,
`+0x14` sort id (`Create(mat, ctx, desc, key)` 0x67f680 builds it from the material: id `& 0xfff`, `GetAdditionalSortID`
(shader vslot `+0x1b0`) `<< 12`, `shader+0x7c << 20`). Contexts that bypass `Create` (vegetation, precipitation, debug) fill
the fields by hand.

Depth key (Animation2, SpeedTree, strips, streaks, billboards): `key = clamp(int((dot(c, db+0x94) + db+0xa0 - r) * db+0xa4),
0, 0xffffff)` with `(r, c.xyz)` = entity vslot `+0x268` (bounding sphere: `[0]` radius, `[1..3]` centre); particles use the
entity position `+0x90` and `PS+0x2a0` instead of the sphere. Alpha ops get `fade` = the request's `param_2`.

### 0.4 Drawing through the shader [C]

Per draw: shader = `eCMaterialResource2+0x70` (`eCShaderBase`); `eCShaderBase::GetPasses(shader, variant, illum, lightmap,
bool, flags)` returns the pass count; each pass = vslot `+0x1d0` `ExecutePass(i)` (0x6d2060) then `eCGfxVisual::Execute`
(0x5069a0). In z / shadow passes (`eCShaderMaterialAdmin+0x312` = 1, set by `BeginZPass` 0x6b9720 with the depth map, also
for shadow maps) contexts call vslot `+0x1d8` `ExecuteSpecialPass(bool)` (0x6d1cd0) once instead: it picks the special
shader pair of the depth-map type (`admin+0x314`, 6 = none) and the bool, sets PS `c5.w` = `MaskReference/256` for masked
materials (blend mode 1), and resets the VS constant c0 block when the depth-bias flag `shader+0x78` changes.

Shader-side setters used by this slice (vtable of `eCShaderDefault` `0x97bdb0`): `+0x1e8` GetMaxBoneMatrixCount (= 66),
`+0x1f0` SetBoneMatrix, `+0x1f8` SetMorphTargetWeights, `+0x200` GetMaxInstanceCount (= 50), `+0x208` SetWorldMatrix(m,
slot), `+0x210` SetGlobalAlpha(a, slot), `+0x220` SetSpeedTreeInstanceData(v2, slot), `+0x228` SetVegationAlpha(start, end),
`+0x238` SetSpeedTreeBillboardTexCoords(v4, slot); `+0x198` IsBlended, `+0x1a0` IsMasked, `+0x1a8` IsRefracted, `+0xb0` /
`+0xb8` = property-change enter / exit around direct writes (`"MaskReference"` = `shader+0x58` byte, `"BlendMode"` =
`shader+0x50`).

### 0.5 VS constant register map (absolute registers) [C]

`eCShaderDefault::ms_pGlobalShaderConstants+0x20` = a constant pool (base `+0x24`, dirty min `+0x2c` / max `+0x30`, data
`+0x38`); `eCShaderMaterialAdmin+0x228` = the frame VS pool (base `+0x22c`, min `+0x234`, max `+0x238`, data `+0x240`).
`FUN_180238390(pool, r, m)` writes the 3x4 transpose of `m` to `c[r..r+2]`; `FUN_180225ec0(pool, r, m)` the 3x3 transpose.

| reg | CTAB name (corpus) | written by | value |
|---|---|---|---|
| c12 | `g_vVegetationData1` | `eCVegetationAdmin::Process` 0x7016b0 (every frame), `SetVegationWindData` 0x6b7f30 | (time s = `timer+0x10`/1000, admin`+0x44`, `+0x50`, `+0x54`) |
| c14..c22 | `g_vWindMatrix` | `eCShaderMaterialAdmin::SetSpeedTreeData` 0x6b7df0 | 3 wind matrices (3x3 rows) from `eCSpeedTreeAdmin+0xac` (stride 0x40) |
| c23..c25 | `g_vLeafAngleMatrix` | same (`FUN_180225ec0(pool, 23, cam)`) | transposed 3x3 of the camera matrix (`app+0x38` or `+200`, `+0x10`) [I: camera rotation] |
| c26..c29 | `g_vLeafAngles` | same | 4 x (rock_i, rustle_i, 0, 0) radians from `eCSpeedTreeAdmin+0x6c..0xa8` |
| c31.xyz | `g_vInstanceMisc` | SetMorphTargetWeights 0x6c9da0 | 3 morph weights |
| c31.xy | `g_vInstanceMisc` | SetVegationAlpha 0x6c9f20 | fade: `x = -1/R`, `y = (R + start)/R`, `R = (end - start)/(1 - MaskRef/256)`; also c32.w = 1 |
| c32+4i .xy | `g_vInstanceData` | SetSpeedTreeInstanceData 0x6c9eb0 | per tree instance (2.3) |
| c32+4i .w | `g_vInstanceData` | SetGlobalAlpha 0x6c9e30 | global alpha of instance i |
| c33+4i..c35+4i | `g_vInstanceData` | SetWorldMatrix 0x6c9e00 | world 3x4 of instance i (i < 50) |
| c33+3b..c35+3b | `g_vInstanceData` | SetBoneMatrix 0x6c9d80 | bone b (b < 66): `(b + 11) * 3` |
| c32+k | `g_vInstanceData` | SetSpeedTreeBillboardTexCoords 0x6ca050 | billboard tex-coord vec4 k (3.) |
| c232.. | `g_vShaderData` | material (slice C) | |

Particle and light-streak shaders own a second global pool (`eCShaderParticle::ms_pGlobalShaderConstants` 0xc62988,
`eCShaderLightStreaks::...` 0xc62980, `+0x20`): c31..c33 `g_vWorldMatrix` (`SetWorldMatrix` 0x6d5f80 / 0x6d3a70), c34.w
`g_vGlobalAlpha` (`eCShaderParticle::SetGlobalAlpha` 0x6d5fa0). Their per-shader pools (`Create`): particle VS
`g_vShaderData` c57..c76, PS `g_ShaderData` c6..c13, samplers `g_SamplerData` s9..s15; light streaks VS `g_vShaderData`
c38..c58 with time at c38.y (`SetTime` 0x6d3e40; the shader registers as a timed element), PS c6..c13, s9..s15.

### 0.6 `eCGfxVisual` fields these contexts set [C]

`+0x10` `eCAPIPrimitive` (our `HalPrimitive`: draw description = slot 12 `GetDrawCallDescription`, the devirtualized path
writes `prim+0x88`; stream frequency = slot 4 (stream 0 at `+0x3c`, stream 1 at `+0x4c`), stream offset = slot 6 (stream 1
at `+0x44`)), `+0x18` render mode (D3DFILLMODE, 3 solid), `+0x1c` culling (D3DCULL: 1 none, 2 CW, 3 CCW), `+0x20` depth
test (D3DCMP), `+0x24` depth write (1 off, 2 on), `+0x38` extra vertex data (vegetation stream 1), `+0x60` `eCGfxShader`.
Draw description (`eSGfxDrawCallDesc`): `+0` indexed (byte), `+4` primitive type (4 list, 5 strip), `+8` primitive count,
`+0x10` first / min vertex, `+0x14` vertex count, `+0x18` start index, `+0x1c` 0 [I: base vertex].

## 1. Skinned actors (`eCRenderContextAnimation2`)

### 1.1 Lists, sort [C]

`RequestRenderOps` 0x4ddd00: needs `PS+0x38` (the EMotionFX ActorInstance of the embedded wrapper `PS+0x30`); one depth key
for the entity, then `FillRenderOpActor` 0x4dd8b0 for actor 0 (the PS's own wrapper) and for each body part `PS+0x1b0[k]`
(count u16 `+0x1b8`) whose ActorInstance (`bodypart+0x38`) exists (actor index k + 1). `FillRenderOpActor(db, fade, mask,
wrapper, actorIdx, key)`: needs `wrapper+8` (ActorInstance) and `wrapper+0x18` (`eCMotionActorResource2`) `+0x98`
(`eCAnimation_HardRenderActor2` = HRA); LOD = ActorInstance `+0x250`; primitives `HRA+0x38 + lod*0x14`: first u16 `+0`,
count u16 `+2`; per primitive k: material = `HRA+0x90[first+k] +0x10` (prim stride 0x20); list mask by the material's shader:
* `IsRefracted` -> refracted list `+0x1578` only (`mask & 0xffe1`);
* else `IsBlended` or `fade != 1.0` -> alpha list `+0x14e8` only (`& 0xffc5`), op fade = `fade`;
* else solid `+0x1500` + z list `+0x1518` (`& 0xffdb`; z when `mask & 0x18`).
Op desc = `actorIdx | primIdx << 16`; key as 0.3. A fading-out actor (fade < 1) moves wholly to the alpha list and leaves
the z pre-pass / shadow maps.

### 1.2 Render [C]

All three slots call `eCAnimation_PS::Render(PS, actorIdx, primIdx, zpass, fade)` 0x4d6fe0: Render / RenderAlpha with
`zpass = false, fade = op+0xc`, RenderZPass with `true, 1.0`. It selects the wrapper (actor 0 = `PS+0x30`, else
`bodypart[actorIdx-1]+0x30`) and its variant `PS+0x20` / `bodypart+0x20` (the material switch, as for meshes), builds a flag
byte from the entity flags `+0x28` (0x200000 -> 2, 0x400000 -> 1, 0x800000 -> 4; passed to GetPasses), then
`UpdateRenderMatrices(admin+0x4c frame id)` 0x4dfee0 (palette refresh, once per frame per wrapper: engine-gfx.md 8) and
`HRA::Render(wrapper, primIdx, illum = entity+0xf8, variant, flags, zpass, fade)` 0x4dd650:
1. LOD = min(ActorInstance+0x250, 4); prim = `HRA+0x90[HRA+0x38+lod*0x14 + primIdx]`; resources requested / cached.
2. Bones: for the prim's bone list (`+0x18` start, `+0x1a` count into the u16 remap `HRA+0x28`): `SetBoneMatrix(i,
   palette[remap[start+i]])` (`palette` = `wrapper+0x48`, 0x40-byte matrices) -> c33+3i.
3. Morphs: if `HRA+0xa8` (target count) != 0, `SetupMorphTargets` 0x4dd220 picks the 3 largest weights of the ActorInstance's
   morph setup (`+0x1d0`, 0xc-byte entries, weight at `+4`) and sets the stream offset of streams 1..3 to `target *
   lodVertexCount(HRA+0x48+lod*0x14) * 0x10`; `SetMorphTargetWeights(w)` -> c31.xyz (zeros when no morphs).
4. `SetGlobalAlpha(fade, 0)` -> c32.w; illumination = `entity+0xf8 +0x78` (null in z pass).
5. `RenderPrimitive` 0x4dd0a0: z pass (`admin+0x312`): `ExecuteSpecialPass(false)` + one draw; else `GetPasses(shader,
   variant, illum, 0, false, flags)` and one draw per pass. Draw description from the prim: start index `+0`, primitive count
   `+4`, min vertex `+8`, vertex count `+0xc` (indexed triangle list, set at creation).

### 1.3 Geometry [C]

Per `eCMotionActorResource2`, built once (`HRA::Create` 0x4dc690, `CreateLoD` 0x4dc480, `FillBuffers` 0x4dbcb0): `HRA+8`
static VB, all LODs concatenated (stride 0x48, LOD table `+0x44` first vertex / `+0x48` count), `HRA+0x18` 32-bit IB (`+0x3c`
start / `+0x40` triangles), `HRA+0x10` morph VB (0x10 bytes per vertex per target: float3 position delta + 4-byte packed normal
delta, bias 0x80), `HRA+0xa0` the visual. Declaration (`CreateVertexDeclaration` 0x4dcd00), stream 0: POSITION f3 @0,
BLENDWEIGHT f4 @0xc, BLENDINDICES ubyte4 @0x1c, NORMAL f3 @0x20, TANGENT f3 @0x2c, COLOR0 @0x38, COLOR1 @0x3c, TEXCOORD0 f2
@0x40; with morphs streams 1..3: POSITION1..3 f3 @0 + NORMAL1..3 d3dcolor @0xc. BLENDINDICES are slots of the prim's bone
list, not skeleton bones.

### 1.4 Shaders [C from the corpus CTABs]

Material VS with BLENDINDICES + BLENDWEIGHT (skinned) and POSITION1..3 / NORMAL1..3 (morphed) inputs, constants c0..c3,
c32..c231, c31 (morph), c232+; the z / shadow special VS take position + colour + texcoord (+ texcoord4 / 5). The
TransformationType of the material is `Skinned` / `Morphed` / `Skinned_Morphed` (`eEShaderMaterialTransformation`, values [?]).

### 1.5 Body parts, attachments, CPU [C]

Body parts (`eCBodyPart_PS`, heads / armour / hair) are separate actors in the same context, posed by
`eCWrapper_emfx3ActorInstance::DeformBodyParts` 0x4df300 (copies the parent's world matrices to the body part's node array by
node mapping). `eCBodyPart_PS::Process` 0x4db920 also animates a random blink: a morph weight (`bodypart+0x110 +4`) driven by a
timer (`+0x118`, `+0x11c`). Attachments (weapon, shield) are ordinary mesh entities (native-renderer.md 8). Per frame on the
CPU: `eCAnimation_PS::Process` 0x4da710 (animation update, `UpdateTransformations`, boundary, attachments; `PS+0x22d & 2` =
visible, `IsVisible` 0x4d8790), then at render the palette.

### 1.6 Shadows [C / I]

Skinned shadow casting = the z list path in the shadow-map database (`mask & 0x18`), drawn with `RenderZPass` ->
`ExecuteSpecialPass(false)` (the depth-map type's special shader, the same bone constants). Per-entity cast flags
(`eCIlluminated_PS::AccessCastDirLightShadows`) are applied by the shadow request (slice C) [I].

## 2. SpeedTree (`eCRenderContextSpeedTree`)

### 2.1 Lists, sort [C]

`RequestRenderOps` 0x6ee4e0, gated by `s_bRenderingEnabled` (0xbfbec7) and the resource loaded (`PS+0x38 +0x38 == 3`).
Render buffer = `resource+0x78` (`eCSpeedTreeRenderBuffer`, one per tree resource, shared by all its instances). **One op per
geometry type per resource per database execution**, not per tree:
* `mask & 2`: if `rb+0xe0 != db+8` (execution stamp): clear the solid queue, for type t = 0..3 with geometry (`rb+0x18+8t`
  visual) add a solid op (material `rb+0x80+8t`, desc t, key 0xffffff); then `AddToRenderQueue(tree)` 0x6f8220 (entry 0x18:
  tree PS, `entity+0xf8 +0x78` illumination, 0).
* `mask & 0x18`: likewise for the z list (`rb+0xe4` stamp), desc `mask << 16 | t`; `AddToZPassRenderQueue(tree, key)`
  0x6f82d0.
* `mask & 0x1a`: `eCSpeedTreeAdmin::UpdateWindMatrices` once per database execution (`admin+0x68` stamp).
Types: 0 branches, 1 fronds, 2 leaf cards, 3 leaf meshes. Solid ops sort with the material's sort id; the instances are sorted
inside the op: `SortInstances` 0x6f2180 (LOD level `wrapper+0x2c` ascending, then illumination pointer, then a byte of it),
`SortInstancesZPass` 0x6f2120 (LOD, then depth key); sorting happens lazily at the first Render of the frame (`rb+0xe0` /
`+0xe4` dirty).

### 2.2 Render [C]

`Render` 0x6ee320: desc 0 `RenderBranches` 0x6f1cd0, 1 `RenderFronds` 0x6f1880, 2 / 3 `RenderLeafs(t)` 0x6f1390.
`RenderZPass` 0x6ee3f0: `shadow = (desc >> 16) & 0x10`; `RenderBranchesZPass` 0x6f1030, `RenderFrondsZPass` 0x6f0cc0,
`RenderLeafsZPass` 0x6f0860.

Per type: material shader must be `eCShaderDefault` with TransformationType (`shader+0x2b8`) 3 Tree_Branches / 4 Tree_Fronds
/ 5-6 Tree_LeafCards / Tree_LeafMeshes (else a log line, no draw); **MaskReference forced to 80** (`shader+0x58 = 0x50`).
Instances are drawn in batches of up to `rb+0x128+t` (byte, <= 50) instances; a batch ends when the illumination differs,
the LOD differs (branches / fronds: strip length per LOD; leaves: per leaf LOD), the batch is full, or the illumination has
the per-object light flags (`illum+0x90` or `+0xd`). Per instance slot i: `SetWorldMatrix(entity+0x60, i)`,
`SetSpeedTreeInstanceData(v, i)`, leaves also `SetGlobalAlpha(a, i)`. Per batch: `GetPasses(shader, 0, illum, 0, true, 0)`,
`ExecutePass` + draw per pass (z: `ExecuteSpecialPass` once; leaves z `ExecuteSpecialPass(true)`).
* Branches: v = (tree LOD level `tree+0x6c`, `rb+0xa0`), global alpha 1; strip length `rb+0x40[lod]`, start index
  `rb+0xe8[lod]`, vertices per instance `rb+0x38`.
* Fronds: v = (`tree+0x6c`, `rb+0xa4`); `rb+0x50[lod]`, `rb+0xf8[lod]`, `rb+0x3a`.
* Leaves: loop over the leaf LODs (`FUN_1802afb00` = `CSpeedTreeRT+0xf4` count); a tree contributes to the leaf LOD it shows
  (`tree+0x58` / `+0x5c` = two blended leaf LODs, alpha `+0x60` / `+0x64`): global alpha `80 / alpha` (= the SpeedTree
  alpha-test fade expressed against the fixed reference 80); v = `wrapper+0x34` of the resource's `eCWraper_SpeedTreeRT`
  (`rb+8`) [?: meaning]; index count per LOD `rb+0x60+0x10(t-2)` + 2 lod, start `+0xa8` from there; triangle lists. Shadow
  z pass: one leaf LOD (current + 1, clamped), alpha 1.
* Draw counts: software instancing (`eCShaderMaterialAdmin+0x320 == 0`): vertex count `n * vertsPerInstance`, branches /
  fronds `(strip + 2) * n - 4` strip primitives (degenerate links), leaves `n * indices/3`; hardware instancing: stream 0
  frequency `n | 0x40000000` (D3DSTREAMSOURCE_INDEXEDDATA), one instance's counts. Which mode the game runs [?] (slice C:
  who writes `admin+0x320`).

### 2.3 Geometry and declarations [C]

Built once per resource (`CreateGeometry` 0x6f8090, `CreateBranchGeometry` 0x6f71d0, `CreateFrondGeometry` 0x6f6270,
`CreateLeafCardGeometry` 0x6f5160, `CreateLeafMeshGeometry` 0x6f40b0, up to 50 copies for software instancing). Stream 1 holds
one float per vertex, TEXCOORD4 = the instance slot [I: copy index / instance index buffer].
* Branches / fronds (`GetBranchFrondVertexDecl` 0x6ef6a0, stride 0x3c): POSITION f3 @0, NORMAL f3 @0xc, TANGENT f3 @0x18,
  COLOR0 @0x24, TEXCOORD0 f2 @0x28, BLENDWEIGHT1 f1 @0x30, BLENDWEIGHT0 f1 @0x34, BLENDINDICES ubyte4 @0x38; stream 1 @0
  TEXCOORD4 f1.
* Leaf cards (`GetLeafCardVertexDecl` 0x6ef4a0, stride 0x58): pos, normal, tangent, colour, uv0 as above, BLENDWEIGHT2 f4
  @0x30, BLENDWEIGHT0 f1 @0x40, BLENDINDICES ubyte4 @0x44, BLENDWEIGHT1 f4 @0x48; stream 1 TEXCOORD4 f1.
* Leaf meshes (`GetLeafMeshVertexDecl` 0x6ef290, stride 0x60): ... BLENDWEIGHT2 f4 @0x30, BLENDWEIGHT0 f1 @0x40, BLENDINDICES
  @0x44, NORMAL1 f3 @0x48, NORMAL2 f3 @0x54; stream 1 TEXCOORD4 f1.
Render-buffer layout: `+8` `eCWraper_SpeedTreeRT*`, `+0x18+8t` visuals, `+0x38+2t` vertices per instance, `+0x80+8t`
materials, `+0xb0` solid queue (`+0xb8` data, `+0xc0` count), `+0xc8` z queue (`+0xd0`, `+0xd8`), `+0xe0` / `+0xe4` stamps,
`+0x128+t` batch limits.

### 2.4 Shaders and wind [C]

VS classes in the corpus: branches / fronds = material VS with BLENDINDICES / BLENDWEIGHT0..1 + c14..c22; leaf cards = +
c23..c25 + c26..c29 (camera-facing card corners rotated by the leaf angles, indexed by BLENDINDICES / BLENDWEIGHT); leaf
meshes = + c26..c29 with NORMAL1/2. Wind: `eCSpeedTreeAdmin::UpdateWindMatrices` 0x6efa10: time = `timer+0x10`/1000, step
clamped to 0.04 s, accumulated at `admin+0x40`; every `eCSpeedTreeWind_PS` (`admin+0x50` list, count `+0x58`; none = no
update and no constants) gets strength `ps+0x28 * admin+0x16c` (wind power) and advances its CSpeedWind (`ps+0x90`); the
blended global wind `admin+0x60` yields 3 matrices (`admin+0xac`) and 4 rock / rustle angle pairs (`admin+0x6c`, deg -> rad),
then `SetSpeedTreeData` (0.5) writes c14..c29. All trees share the wind: a native renderer reads `admin+0xac` / `+0x6c` (no
per-tree capture).

### 2.5 LOD [C]

`UpdateLoD` 0x6ee4d0 -> `eCSpeedTree_PS::UpdateLoD` 0x6e8be0 -> `eCWraper_SpeedTreeRT::UpdateLoD(entity+0x90)` 0x6f8f30
(lod.md 2): continuous LOD level `wrapper+0x2c` (0..1) + discrete branch / frond / leaf LOD indices in `wrapper+8`
(`SLodValues`). Branch / frond LOD index `tree+0x48` / `+0x50` (`-1` = none: the tree is not drawn for that type).

## 3. SpeedTree billboards (`eCRenderContextSpeedTreeBB`)

[C] One context per `eCSpeedTreeBBMesh_PS` (an area's baked billboard mesh = `eCMeshResource2` at `PS+0x30`, one sub-mesh per
billboard material). `RequestRenderOps` 0x6ee9b0: per sub-mesh, a linked list of cells (`submesh+0x40`, nodes 0x28: next, box
`+8`); `CullCamera(db+0x28, box)`: inside (1), or intersecting (2) with fewer than `ctx+0x28 * 3` indices -> op; solid (`mask
& 2`) and z (`& 8`, not shadow), desc `cell << 8 | submesh`, key 0. Render 0x6ee8d0 / 0x6ee730: `UploadTexCoords(submesh)`
0x6ec150 (c32..c36 = 5 fixed vec4 from `DAT_180bfcbd8`, c37.. = the sub-mesh's vec4 list `PS+0x40` range `PS+0x58[k]`), then
`eCMeshResource2::Render(res, null, submesh, cell, 0, illum, null, 0)` (static-mesh path, slice C). RenderZPass 0x6ee8e0:
`eCMeshResource2::RenderZPass` 0x7603b0 twice (bool false, then true). Declaration (`CreateBBVertexDeclaration` 0x6eb330, stride
0x40): POSITION f4 @0, TEXCOORD5 f2 @0x10, BLENDWEIGHT f4 @0x18, BLENDWEIGHT1 f3 @0x28 (fade-in start / range, lod.md 2.1),
BLENDWEIGHT2 f3 @0x34. Fade: baked per vertex from `GetBillboardFadeValues` 0x6f8a00 (tool time); the VS fades by camera
distance (`g_bSpeedTreeBillboards`).

## 4. Vegetation / grass (`eCRenderContextVegetation`)

### 4.1 Lists [C]

`RequestRenderOps` 0x70d4f0: needs `db+0x15a6` (vegetation allowed in this view) and `eCVegetationAdmin+0x10` (enabled,
config `/Engine/Vegetation/...`, `ApplyConfiguration` 0x701750; density `+0x5c` 1..8). `UpdateDefaultViewRange`; clears the
visible-node list `PS+0x98` (entries 0x118: `+8` grid node, `+0x10` `eSShaderMaterialIllumination` from
`eCIlluminationAdmin::GetIlluminationAt(node box)` + light set, `+0x1d` / `+0xa0` light flags). Grid `PS+0x90`
(`eCVegetation_Grid`): cells in `CalculateCacheRect(camera)` intersected with the grid rect; a cell's node (`grid+0x28`) is
visible if its box (`node+0x44`) intersects the sphere (camera pos `cam+0x90`, radius `grid+0x14`) and passes `CullCamera(db+
0x28)` -> `AddVisibleGridNode` 0x706400. The node list is qsorted (`SortGridNodes` 0x7063a0: illumination, light flags). Then
per mesh type with any entries in a visible node (`node+0x30[type]` = (start, count)): an op on the solid list (`mask & 2`)
and the z list (`mask & 8`; **no shadow-map ops**), desc = type, fade 1, key 0, sort id from the type's material.

### 4.2 Render [C]

`Render` 0x70d4b0 / `RenderZPass` 0x70d4d0 -> `eCVegetation_PS::Render(type, zpass)` 0x707800. One shared dynamic VB
(`eCVegetationAdmin+0x38`, `CreateVertexStream` 0x7014a0: 44000 bytes = 1000 x 0x2c, D3DUSAGE_DYNAMIC, write cursor `+0x40`):
for each visible node (batch break on another illumination or light flags), its entries of the type (`node+0x18`, 0x2c bytes)
are copied into the buffer (lock NOOVERWRITE, DISCARD at cursor 0; full at 1000 -> draw and restart) as
`[pos.xyz (entry+4), wind = type.mesh+0x2c != 0 ? 1 : 0] [entry+0x10..+0x1c: rotation quaternion] [entry+0x20, +0x24: scale
xy, type.mesh+0x20 != 0 ? 0.8 : 0]`. `RenderMeshInstanced` 0x707e40: type mesh resource (`type+0x58`), visual `res+0x88`;
stream 1 = the admin's VB at the batch's byte offset, stream 0 frequency `n | 0x40000000`, stream 1 `0x80000001`
(INSTANCEDATA); cull `2 - (type+0x30 != 0)` (two-sided types = none); first sub-mesh only (`res+0x58`): indexed triangle
list, start `+0x28`, count `+0x2c`/3, vertices `+0x30` / `+0x34`; `SetVegationAlpha(grid+0x50, grid+0x54)` (fade start / end
-> c31.xy); solid: `GetPasses(shader, 0, illum, 0, false, 0)`, z: `ExecuteSpecialPass()`; frequencies reset to 1 and stream 1
detached afterwards.

### 4.3 Shader [C, corpus VS `vs30_15dccf4a41723755` and siblings]

Inputs: mesh POSITION / NORMAL / COLOR / TEXCOORD0 (+6), instance BLENDWEIGHT0 (pos + wind flag), BLENDWEIGHT1 (quaternion),
BLENDWEIGHT2 (scale). Wind: phase = `|instance pos| * 0.02 + c12.x`, four sine frequencies (0.178, 0.1034, 0.1743, 0.1227),
bend `c12.y * wind flag` growing with the scaled height (`c12.z`), `c12.w` frequency scale; fade `o5.w = saturate(dist * c31.x
+ c31.y)` (alpha-tested against MaskReference). Wind parameters: `eCVegetationAdmin::SetWindParameters` 0x701650 (`+0x44..
+0x54`, from config `+0x48..+0x54`).

## 5. Particles (`eCParticle_PS`)

[C] `RequestRenderOps` 0x5d33a0: `CreateMaterial` 0x5dffc0 (the PS's material file `+0x208`, else a default from
`eCParticleModule::CreateParticleMaterial` 0x5d0ab0: an `eCShaderParticle` with one sampler (`eCParticleMaterialSimple`) or
two samplers + frame blend (`eCParticleMaterialBlendSubdivisions`)); alpha list only (`mask & 4`), material `PS+0x450`, key
from the entity position and `PS+0x2a0`, fade = request fade. RenderAlpha 0x5d3360 -> `eCParticle_PS::Render(fade)` 0x5e0080
-> (instanced emitters `PS+0x29d`: the master instance's vertices from `eCParticleModule::GetMasterInstance`) `OnProcess` if
nothing was simulated, `RenderVertices` 0x5d56a0:
1. `UpdateVertices` 0x5d58b0, once per timer tick (`timer+0x50` stamp `PS+0x3b0`): `DepthSortParticles` 0x5d8680 (qsort
   0xd0-byte particles along the camera z axis, entity-local for coordinate system 1), lock DISCARD, per live particle 4
   vertices built on the CPU (orientation modes `PS+0x170`: 0 camera-facing, 1-4 velocity / fixed-axis / ... variants,
   `PS+0x178` axis, `PS+0x198` axis in entity space), sprite-sheet frames (`PS+0x228` x `+0x22c` tiles; two frames for
   blending).
2. World matrix: coordinate system `PS+0xb0` 0 (world: identity, positions world-space) / 1 (entity-relative: entity `+0x60`);
   `ApplyRenderStates` 0x5d6e50: `SetGlobalAlpha(fade)` -> c34.w, `SetWorldMatrix` -> c31..c33, `EnableFog = !PS+0x298`,
   BlendMode from `PS+0x220` (1 -> 2 AlphaBlend, 2 -> 3, 3 -> 5, 4 -> 4, 5 -> 6, 6 -> 7, 7 -> 8; 0 -> AlphaBlend if fade < 0.9,
   else Normal), MaskReference `PS+0x299 ? PS+0x29a : 0`, visual states fill solid, cull none, depth test `PS+0x29b ?
   LessEqual : Always`, depth write `PS+0x29c ? on : off`; blend modes other than 1 also call device slot `+0x1c0`
   (SetMasterFogging(1)) [C, purpose I].
3. `RenderPasses` 0x5d5570: illumination = `entity+0xf8 +0x78` if lighting `PS+0x2b0`, `GetPasses(shader, 0, illum, 0, false,
   0)`, one draw per pass.
Geometry (`CreateVisual` 0x5d7620, `CreateVertices` 0x5d7210): dynamic VB `capacity(PS+0x394) * 0xc0` (4 vertices x 0x30,
D3DUSAGE 0x208), static 16-bit IB (0,1,2, 0,2,3 per quad), indexed triangle list. Declaration: POSITION f3 @0, NORMAL f3 @0xc,
COLOR0 @0x18, COLOR1 @0x1c, TEXCOORD0 f2 @0x20 (frame A), TEXCOORD1 f2 @0x28 (frame B). VS `vs20_60faa4ebe3d86b65` (c0..c3,
c11, c30 fog, c31..c33 world, c34 alpha). Shadows: none (alpha list only).

## 6. Strips / trails (`eCStrip_PS`)

[C] `RequestRenderOps` 0x701170: alpha only, material `PS+0x120`, entity key. RenderAlpha 0x701120 (and `Render` 0x6feae0):
if >= 2 segments (`PS+0xe8`): `UpdateVertices` 0x6fd960 (lock DISCARD; 2 vertices per segment from the segment array `PS+0xe0`
pushed by `PushSegment(matrix)` 0x6fe6a0 in `OnProcess`; colour / alpha lerp `PS+0x20`/`+0x38` -> `+0x40`/`+0x58`, uv from
segment `+0x18`/`+0x1c` over `PS+0x108`/`+0x10c`), `RenderVertices` 0x6fd800: alpha = fade, times `clamp(PS+0x134 / PS+0xa8)`
(age fade-in) unless `PS+0xa0`; world identity; `GetPasses(shader, PS+0xa4, null, 0, false, 0)`. Visual (`CreateVisual`
0x6fe170): non-indexed triangle strip, `2n - 2` primitives, fill solid, cull none, depth LessEqual, depth write off. VB
(`CreateVertices` 0x6fde70): `capacity(PS+0xec) * 64` bytes, FVF 0x242, decl POSITION f3 @0, COLOR0 @0xc, TEXCOORD0 f2 @0x10,
TEXCOORD1 f2 @0x18 (stride 0x20). Spawn modes `eEStripSpawning_Continuous / Movement / Timed`.

## 7. Light streaks (`eCLightStreaks_PS`)

[C] `RequestRenderOps` 0x5c1880: alpha only, material `PS+0x70`. RenderAlpha 0x5c1840 -> `Render(fade)` 0x5c4ae0: needs
illumination (`entity+0xf8`), `CreateBuffers` 0x5c2450 (static VB + IB built once from the streak polygons `PS+0x48` (count
`+0x50`, 0x30 bytes each, `CreateLightStreakPolys`), positions in entity space); shader must be `eCShaderLightStreaks`;
global alpha (only for a non-streak shader), world = entity `+0x60` -> c31..c33; `GetPasses(shader, 0, illum+0x78, 0, false, 0)`.
Visual (`CreateVisual` 0x5c2190): indexed triangle strip (degenerate-linked), cull CW (2); decl POSITION f3 @0, COLOR0 @0xc,
TEXCOORD0 f2 @0x10, TEXCOORD1 f4 @0x18, TEXCOORD2 f1 @0x28 (stride 0x2c). The animated look is the shader's time c38.y
(`FadeWaveLenght`, `EnableFading` properties) [I].

## 8. Billboards (`eCBillboard_PS`, `eCBillboardAdmin`)

[C] `RequestRenderOps` 0x4f1400: if the PS's billboard `PS+200` has no material, the admin's default material is created
(`CreateDefaultMaterial` 0x4efdc0). Blended material (`IsMaterialBlended` 0x4f4ba0) or fade != 1 -> alpha op (material
`GetMaterial`, key, fade); else on `mask & 2` it calls `eCBillboard_PS::Render` 0x4f4bf0 **during the request** ->
`eCBillboardAdmin::RenderBillboard(bb, lightset = entity+0xf8 +0x180, blend PS+0x88, lights PS+0x90, false, 1.0)` 0x4f0c00,
which with `!alpha && fade >= 0.99` only **queues** (admin `+0x18`: 0x18 entries bb, light set, blend, lights); the queue is
drawn by `eCBillboardAdmin::RenderBillboards` 0x4f0530 at Execute step 6 (after solid, beside the decals; frame order:
engine-gfx.md 4.2): qsort (`SortRenderQueue` 0x4f0ff0: material, texture, light set, lights), batches of <= 1000 quads per
(material / texture, light set, blend, lights), copies each billboard's prebuilt vertices (`bb+0x18`, 4 x 0x34 per quad, count
`bb+0x20`) into the admin VB `+0x48` (4000 vertices) and 6 indices per quad into IB `+0x50` (6000), then per batch: material
shader or the default shader `admin+0x30` with BlendMode = blend and the sampler `admin+0x38` = the texture, global alpha 1,
world identity, `GetPasses(shader, lights, {lightset+0x10}, 0, false, 0)`. Alpha path (RenderAlpha 0x4f4c70 -> 0x4f13c0):
`RenderBillboard(..., true, fade)` draws immediately with the same buffers (one billboard set per draw). Vertex (admin
`CreateBuffers` 0x4f00a0, stride 0x34): POSITION f3 @0, NORMAL f3 @0xc, TANGENT f3 @0x18, TEXCOORD0 f2 @0x24, TEXCOORD5 f2
@0x2c. Billboard vertices are rebuilt on the CPU by `eCBillboard_PS::UpdateBillboards` 0x4f1ab0 (random points on the target
mesh / animated actor, random size / rotation / sprite tile; target modes `eEBillboardTargetMode_Self / Parent / Target`)
[C, when it runs I: on property changes].

## 9. Decals (`eCDecalAdmin::RenderDecals` 0x4fc220)

[C] Not a render context: called from `eCFrustumDatabase::Execute` step 6 with the database. All decals (`admin+0x20`, count
`+0x28`) whose entity resolves, **was in this database's view** (`entity+0xe4 == db+8`, the execution stamp) and has vertices
(`decal[6]`) and indices (`decal[9]`) are queued (`+0x30`: decal, `entity+0xf8 +0x78` illumination), qsorted (`SortRenderQueue`
0x4fcf80: material, texture, lightmap mesh `decal[0xb]`, light flags) and batched (<= 6000 vertices, <= 10000 indices per
batch; decals above that are skipped). Vertices (stride 0x3c, VB `admin+0x60` 360000 bytes, IB `+0x68` 20000, both locked
DISCARD per batch): copied from the decal (`decal[5]`), dynamic decals (`decal+0x54`) transformed by the entity world matrix
(position, normal, tangent), COLOR0 = `alpha(decal+0x50) * 255 << 24 | 0xffffff`. Per batch: material shader or the default
shader `admin+0x48` + sampler `admin+0x50` = the texture, global alpha 1, world identity, `BindLightmaps(mesh, 1)` (the
decal receiver's lightmaps), `GetPasses(shader, 0, illum, lightmapType, false, 0)`; drawn through direct device calls (render
mode / cull / depth / write from the visual `admin+0x58`, `eCGfxShader::Execute`, primitive slot `+0x78`). Declaration
(`CreateBuffers` 0x4fbd80): POSITION f3 @0, NORMAL f3 @0xc, TANGENT f3 @0x18, COLOR0 @0x24, COLOR1 @0x28, TEXCOORD0 f2 @0x2c,
TEXCOORD6 f2 @0x34 (lightmap uv).

## 10. Precipitation (rain, snow: `eCPrecipitation_PS`)

[C] `RequestRenderOps` 0x719b40: on `mask & 4` an op on the **post-alpha list `+0x1548`** (after `DoEndRenderAlpha`), desc 0,
fade 1, key 0. RenderAlpha 0x719b00 (= `eCPrecipitation_PS::RenderAlpha` 0x71d1a0 inlined): if `PS+0x18 & 1` [I: enabled],
the embedded dynamic primitive `PS+0x108` (vtable `+0x10` = render with a matrix; created in `Create` 0x719e20 as
`(1, 4, 1, 0)` [?]) is drawn with the identity world (one indexed draw of the whole field, ~6000 triangles, every frame even
when dry: draw-tags.md 4.1). Shader (`CreateShader` 0x71b6e0): a code-built `eCShaderDefault`, BlendMode AlphaBlend (2),
TransformationType 7 [?: name], DisableLighting, MaxShaderVersion 3, one sampler = the texture `PS+0x20`. CPU per frame
(`OnProcess` 0x71b920 -> `Update(camera pos)` 0x71a4d0): drops in a cube of size `PS+0x28` around the camera, moved by direction
`PS+0x34` x speed, spawned `PS+0x30` per second at random spawn points (`PS+0xa8`, 0x1c bytes, physics ray `TraceRayNearestHit`
for the ground), splashes (`SplashDuration`, size scale, texture tiles `PS+0x58` x `+0x5c`, `PS+0x70` duration, `PS+0x74` /
`+0x60` tiles), jitter `PS+0x64`. Weather admin (`eCWeatherAdmin`, `eCWeatherZone_PS`) scales it (slice A / weather).

## 11. Debug context (`eCRenderContextDebug`)

[C] `RequestRenderOps` 0x67f4f0: on `mask & 0x100` an op on the debug list `+0x1560` (fade, desc 0, key 0) + copies the db's
debug switches (`db+0x1598..0x159a`) into the context. `Render` 0x67f070: each property set's `OnRenderDebug` (vslot
`+0x218`), coordinate / translation axes, a line list to the children (`eCGfxMixerAdmin::DrawLineList`). Editor / debug only.

## 12. Searched, not in this slice

* Lens flares / sun glow: no class in Engine.dll (no `Flare` / `Glow` symbols); sun, moon, stars are the sky context
  (`0x9ddd90`, `eCSkydome_PS`: slice of the sky). Lightning: `eCWeatherStates` thunder properties (`ThunderProbability`,
  `ThunderIntesity`, `ThunderDuration`) drive the sky / fog (slice A / sky), no geometry context found.
* `eCIteratorPrimitive*::Render` (billboard, line / triangle lists) and `eCBillboardAdmin::SetEnableEditorBillboards`: editor
  gizmos (`eCIteratorPrimitiveBillboard::Render` 0x5bcc60 wraps `RenderBillboard` in its own Begin / EndMaterial).
* `eCSkyObject` billboards (`SetBillboards`, `SetBillboardRenderCount`): sky slice.

## 13. For a native implementation

Per context, what to read (engine objects only; no draw stream needed):
* **Skinned**: entity `+0xf8` illumination, `eCAnimation_PS +0x30` wrapper (`+8` ActorInstance: `+0x250` LOD, `+0x1d0` morph
  weights; `+0x48` palette, `+0x58` stamp, `+0x50` count; `+0x18` resource `+0x98` HRA), `+0x1b0` / `+0x1b8` body parts, HRA
  buffers (`+8` VB, `+0x10` morph VB, `+0x18` IB, `+0x28` remap, `+0x38` LOD table, `+0x90` prims, `+0xa8` targets).
  `UpdateRenderMatrices` runs only inside the engine's Render: a native pass must call it itself (or keep the engine's request
  phase) for actors the engine does not draw; the per-prim bone subset (`<= 66` bones per prim) exists only because of the
  D3D9 constant limit: a native skinner can take the whole palette with the remap.
* **SpeedTree**: per tree `eCSpeedTree_PS +0x38` resource -> `+0x78` render buffer (geometry, materials, LOD tables), `+0x40`
  wrapper (LOD level `+0x2c`, LOD indices, leaf LOD alphas `+0x58..+0x64`), entity `+0x60`; global wind `eCSpeedTreeAdmin
  +0xac` / `+0x6c` after `UpdateWindMatrices` (call it or hook its end); camera matrix for c23..c25; MaskReference 80. The
  per-resource queue + LOD-sorted instancing maps directly onto one instanced draw per (resource, type, LOD, illumination).
* **SpeedTree billboards**: the BB mesh resource (static mesh path) + its cell lists + `UploadTexCoords` table.
* **Vegetation**: `eCVegetation_PS +0x90` grid (cells `+0x28`, rect `+0x18`, view range `+0x14`, fade `+0x50` / `+0x54`),
  nodes (`+0x18` entries 0x2c, `+0x30` per-type ranges, box `+0x44`), types `PS+0x38` (mesh `+0x58`, flags `+0x20` / `+0x2c` /
  `+0x30`), admin wind `+0x44..+0x54`, time. The entries are static: a native renderer can upload them once per node (GPU
  instancing / indirect) instead of the per-frame 1000-instance ring.
* **Particles / strips / streaks / billboards / decals / precipitation**: the vertex data is produced on the CPU by the engine
  (`UpdateVertices`, `UpdateBillboards`, decal clipping, precipitation `Update`); a native renderer either reads the locked
  dynamic buffers (they pass through our HAL in Vulkan mode) or re-implements the CPU code. Render states are small and fixed
  per class (above); blend modes come from `eEShaderMaterialBlendMode` (shader `+0x50`; strings in Engine: Normal, Masked,
  AlphaBlend, Modulate, AlphaModulate, Translucent, Darken, Brighten, Invisible; numeric order 0 Normal, 1 Masked, 2
  AlphaBlend [C: rt-albedo-facts.md], the rest [I: G3 order 3 Modulate, 4 AlphaModulate, 5 Translucent, 6 Darken, 7 Brighten,
  8 Invisible]).
* Order inside the frame: particles, strips, streaks, blended billboards are depth-sorted alpha ops (db-wide sort with the
  meshes' alpha ops); opaque billboards and decals are drawn at step 6; precipitation after the alpha list.

## 14. Open

* Actor LOD: who writes ActorInstance `+0x250` (EMotionFX internal; `eCAnimation_HardRenderActor2::ms_fMaxBoneLODDistance`
  0xbfbea8 has no code reference).
* `eCShaderMaterialAdmin+0x320` (SpeedTree hardware vs software instancing) and how stream 1 (TEXCOORD4) is bound for SpeedTree
  in each mode.
* SpeedTree instance data `.y` (`rb+0xa0` / `+0xa4`) and the leaves' `wrapper+0x34` pair: meaning (wind weight / AO?), and the
  leaf-card VS's use of c32[...] (a corpus leaf VS reads c33..c35 without instance indexing: z / shadow variant?).
* Shadow-map fronds: `RenderFrondsZPass(shadow)` skips unless `rb+0x3c <= rb+0x3a` [C, meaning ?].
* Request bits 1 / 0x40 / 0x80 (`db+0x15a5 / 0x15a1 / 0x15a8`) and whether caps `ctx+0x20` filter the mask (slice B).
* `eCPrecipitation_PS+0x108` class (dynamic primitive) and its vertex format; TransformationType 7 name.
* When `eCBillboard_PS::UpdateBillboards` runs (once at creation / property change, or per frame).
* Body-part ops: whether the alpha decision (fade) is per actor or per body part (the code uses the PS fade for all).
* Verify in the game (`-LogFrame`): the SpeedTree batch sizes, vegetation 1000-instance batches, the particle states.
