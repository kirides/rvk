# RT scene facts: vertex layouts, world transforms, texcoords per draw kind (input for M9)

What the BLAS / TLAS builder and the hit shader of M9 (RT water reflections with textured hits, enhancements-roadmap.md 3,
raytracing.md 5.2) need to know about the engine's geometry: per `d3d9::draws::Kind` the vertex declaration, how the
material VS forms the world position, and how it forms the texcoords the material samples.

Legend: **[C]** confirmed (shipped HLSL, translated bytecode listings, engine calls logged in game, our code), **[I]** inferred.

Sources:
* The shipped material HLSL (read only, through the test copy's junction): `data\compiled\effects\shadermaterial\ge_default_vs.fx`,
  `ge_default.fxh`, `ge_common_vs.fxh`, `ge_common.fxh`.
* The corpus (`corpus/`, 149 VS) listed with `rvk_shadertool show` and classified by world transform (`out/vs_classify.py`:
  it also computes each VS's `Device` hash, i.e. FNV-1a over the tokens seeded with `kTranslatorVersion` 2, `src/d3d9/device.cpp:799`).
* Run 1, Vulkan, 2026-10-22: `run.ps1 -Mode vulkan -Mods rvkcompare -Scenario rvkcompare -Param 'spots=lake_jungle','settle=3000'
  -LogFrame 3700 -Screenshots 3700`. Frame 3700 is the lake from the player's position: lake water, jungle trees, grass,
  rocks, bloodflies. Each draw's `[View/Pass]`, the last `HLSLShader::Execute vs`, and the `Primitive::Execute` stride were
  joined with the corpus classes (`out/logframe_draws.py`).
* Run 2, trace mode (D3D9) at the same spot: the trace logs each distinct `CreateVertexDeclaration` (18 of them) and the
  first calls of each `eCAPIPrimitive` slot.

All scripts are in `out/` (git-ignored).

## 1. Constant layout of the material VS [C]

From `ge_common_vs.fxh` and `ge_default.fxh` (registers fixed in every shader; draw-tags.md 3.1):

| reg | name | content |
|---|---|---|
| c0-c3 | `g_vViewProjection` | the only clip transform |
| c8-c10 | `g_vInvView` (3x3) | screen-aligned billboards |
| c11 | `g_vCamPosAndFar` | camera position, far |
| c12 | `g_vVegetationData1` | grass: x time stamp, y wind strength, z bend weight, w wave speed |
| c14-c22 | `g_vWindMatrix[3]` (3x3 each, 3 registers per matrix) | SpeedTree wind |
| c23-c25 | `g_vLeafAngleMatrix` (3x3) | SpeedTree leaf-card orientation |
| c26-c29 | `g_vLeafAngles[4]` | leaf rock / rustle |
| c30 | `g_vFogSettings` | |
| c31 | `g_vInstanceMisc` | morph target weights (xyz), grass dissolve (xy) |
| c32-c231 | `g_vInstanceData[200]` | per draw / per instance / bones (section 2) |
| c232-c251 | `g_vShaderData[20]` | per-material parameters: the texcoord transforms (section 4) |

In the CTAB, plain (non-instanced) VS declare `g_vInstanceData` as **x4** (c32-c35) and every other variant as x200
(draw-tags.md 3.1). `g_bInstanced` / `g_bSkinned` / `g_bSpeedTree*` / `g_bVegetation` are compile-time `static bool`s:
each permutation is its own VS.

## 2. World transform per kind

### 2.1 `g_vInstanceData` layouts [C] (`ge_default_vs.fx:178-247`, listings)

`GetMatrixFromInstanceData(o) = transpose(float3x4(D[o], D[o+1], D[o+2]))`, used as `mul(pos, M)`. In the bytecode this
is `world.x = dp4(pos, c[o])`, `.y = dp4(pos, c[o+1])`, `.z = dp4(pos, c[o+2])`. **The three registers are the rows of
a 3x4 row-major object-to-world matrix, i.e. exactly `VkTransformMatrixKHR::matrix[3][4]`**, with no transposing. Normals
and tangents use `dp3` with the same rows.

| variant | world matrix | other per-draw / per-instance data |
|---|---|---|
| plain (one object per draw) | `D[1..3]` = **c33, c34, c35** | `D[0].w` = global alpha (c32.w) |
| instanced (`g_bInstanced`) | `D[4i+1..4i+3]` | `D[4i]`: `.w` alpha; SpeedTree branch `.x` current LOD, `.y` LOD radius (branch dissolve); SpeedTree leaves `.xy` rock / rustle angles |
| skinned (`g_bSkinned`) | `sum_k w_k * D[3*b_k+1 .. +3]`: bone b at **c33 + 3b**, 4 bones (BLENDINDICES UBYTE4 x 3), weights = BLENDWEIGHT0.xyzw | `D[0].w` alpha (plain) or `D[4i].w` (variants that also read TEXCOORD4 use it for the alpha only) |
| SpeedTree billboards | none: the vertex position is already world space | `D[0].z` 2*pi, `D[0].w` / `D[1].zw` / `D[2].z` fade alphas, `D[4]` clip position, `D[corner].xy` corner signs, `D[offset+image]` the billboard atlas uv rectangles |
| vegetation (grass) | none: per-instance quaternion + scale + offset from stream 1 (2.4) | does not use `g_vInstanceData` |

**Instance index** [C]: TEXCOORD4 (FLOAT1) is in its own stream, a float instance number `i`. The bytecode computes the
register as `floor(4 * i) + 1` (`mad r, v.x, 4, 1` + `frc`/`add`, `mova`). The engine sets
`SetStreamSrcFrequency(3, 0x80000001)` (INSTANCEDATA | 1, trace) for the meshes' instance stream, and
`EnableStream(3, false)` for draws without instancing. The instance stream's buffer is
`eCShaderMaterialAdmin::GetHardwareInstancingVertexData` (`0x6b6c80`). Its contents are taken to be 0, 1, 2, ... [I].
Our device: stream 0 frequency INDEXEDDATA|n -> `instanceCount_ = n` (`src/d3d9/draw.cpp:168-171`), record flag
`Record::Instanced`, `Record::instances`.

Batch size [I]: 200 registers / 4 per instance = at most 50 instances per draw, likewise for SpeedTree
(`eCShaderBase::GetMaxInstanceCount` `0x6d33b0`, `eCShaderDefault::GetMaxInstanceCount` `0x6c9df0` would confirm). Bones:
at most (200 - 1) / 3 = 66 per draw [I].

The world matrices in the constants are the authoritative source per draw. An alternative is entity `+0x60` (static
meshes) or `eCMeshResource2` render instances (`+0xa8`, 0x58 bytes each, matrix first; `AddRenderInstance` `0x7602a0`)
(roadmap 1.1). These are the same matrices, but the constants also cover SpeedTree, and they need no engine-memory reads.

### 2.2 Static and instanced meshes (StaticMesh, InstancedMesh, Refracted) [C]

* World = c33..c35 (plain) or `c[4i+1..4i+3]` (instanced); `world = M * float4(pos.xyz, 1)` (FLOAT3 position, w = 1).
* Morph targets (`g_bMorphed`, a few VS: POSITION1..3 / NORMAL1..3 in streams 1..3, weights c31.xyz): position +=
  sum(delta_k * w_k) before the world matrix. Not seen at the spot.
* Screen-aligned billboard meshes (`g_bBillboard`, VS with `g_vInvView` + TEXCOORD5): world = M * pos
  + invView[0] * tc5.x + invView[1] * tc5.y, which depends on the camera (3 draws at the spot, stride 32).

### 2.3 SpeedTree (Kind SpeedTree; op desc 0 branches, 1 fronds, 2 / 3 leaves)

**At lake_jungle every SpeedTree draw is instanced** [C]: all 191 SpeedTree draws used `c[4i+1..]` VS (2.6). Plain-world
SpeedTree VS also exist in the corpus (`plain+wind`, `plain+wind+leaf`, 10 VS) but were not drawn at this spot.

* **Wind** (all SpeedTree, `ge_default_vs.fx:304`):
  `pos = lerp(pos, mul(pos, g_vWindMatrix[BLENDINDICES.x]), BLENDWEIGHT0.x)`, in object space, before the world matrix.
  The wind matrix register is `c14 + 3 * BLENDINDICES.x` [C, listing]. For RT: ignore the wind (rest pose = the vertex
  position) [I]. Its deflection is small next to the hit accuracy a reflection needs.
* **Branches / fronds** (stride 60): `world = M_i * windpos`. The branch-LOD dissolve
  `alpha = lerp(0.33, 1, saturate((D[4i].x - BW1.x) / D[4i].y))` is an alpha-test fade, not geometry.
* **Leaf meshes** (stride 96, `g_bSpeedTreeLeafMesh`): `world = M_i * windpos + MeshMatrix * RockX(rock) * BW2.xyz`, where
  `MeshMatrix = (NORMAL1, -cross(NORMAL1, NORMAL2), NORMAL2)`, `rock = D[4i].x * g_vLeafAngles[BW2.w].x`. POSITION is the
  leaf's anchor; the shape is the offset BW2.xyz. No camera dependence.
* **Leaf cards** (stride 88, `g_bSpeedTreeLeafCards`):
  `offset = float3(BW1.yx * BW2.yz, 0)` (shadow-map pass: `.z = dot(offset.xy, 0.5)`), rotated by
  `RotZ(BW1.w + rock)`, then `RotY(BW1.z + rustle)`, then **`g_vLeafAngleMatrix` (c23-c25)**; then
  `world = M_i * windpos + offset`. The offset is added in world space **after** the world matrix (listing `d520a98`:
  `add r1.xyz, offset, M*pos`). c23-c25 are set per frame by the engine. That they hold the camera-facing orientation is
  [I], from SpeedTree's leaf-card convention; the constant values were not logged. For RT: build the cards once with a fixed
  orientation (e.g. this frame's c23-c25, or two crossed quads) [I].
* **Billboards** (Kind SpeedTreeBillboard, stride 64, `g_bSpeedTreeBillboards`): POSITION FLOAT4 = **world** position of the
  tree pivot, `.w` = corner index. `world = pos + X * TC5.x + Y * TC5.y`, with `Y = BW2.xyz` and
  `X = cross(normalize(horizontal(cam - pos)), Y)`, so the quad is camera-facing. The atlas image depends on the azimuth
  (BW0: tex offset, image count, tree azimuth, slice width); uv = `D[offset + image].xy - D[offset + image].zw * D[corner].xy`.
  For RT: distant trees; skip, or build crossed quads from the pivot [I].

### 2.4 Vegetation (grass, seaweed; Kind Vegetation) [C]

`ge_default_vs.fx:339-386`. No world matrix and no `g_vInstanceData`. Per instance, from **stream 1** (trace: decl with a
stream-1 block BW0 FLOAT4 @0, BW1 FLOAT4 @16, BW2 FLOAT3 @32 = 44 bytes; `SetStreamOffset(1, 0 / 176 / 440)` = multiples
of 44):

* BW0.xyz instance offset (world position), BW0.w wind strength;
* BW1 = rotation quaternion (`qrot(q, v) = v + 2 cross(q.xyz, cross(q.xyz, v) + q.w v)`);
* BW2.xy scale (applied as `.xyx`), BW2.z normal bend.

`world = qrot(q, pos * scale.xyx)`, bent by the wind (`g_vVegetationData1`, keeps the length, scales with the height),
`+ offset`. **That is exactly a TLAS instance**: one BLAS per grass mesh and per instance the 3x4 matrix
`R(q) * diag(s.x, s.y, s.x)` with translation `offset` (wind ignored) [I for the RT mapping, C for the math]. The
stream-1 buffer is `eCVegetationAdmin`'s (`LockVertexStream` `0x7013b0`, NOOVERWRITE appends), a dynamic buffer with a
CPU copy (section 5). Whether stream 1 has INSTANCEDATA frequency was not logged [I: 75 solid draws of ~73 triangles each
cover a meadow, so they are instanced].

### 2.5 Skinned (Kind Skinned) [C]

Bones at c33 + 3b (2.1), 4 influences: BLENDINDICES UBYTE4 @28, BLENDWEIGHT0 FLOAT4 @12 of the 72-byte vertex. Morph
variants add streams 1..3 (POSITION1..3 FLOAT3 + NORMAL1..3 D3DCOLOR). For RT, roadmap 3.3: compute skinning from the draw's
VS constants, or VS capture.

### 2.6 What the spot actually draws (frame 3700, run 1) [C]

Each draw was joined to its VS class. The kind follows from the class: the histogram's counts match line for line.

| kind (histogram) | VS class / stride | draws (Main / Shadow) | triangles |
|---|---|---|---|
| StaticMesh 265 [Main 203, Shadow 62] (+ Refracted 11) | plain c33..c35; stride 52 or 60 | Solid 67, DepthPrepass 114, Alpha 29, Refracted 11, Shadow 62 | Solid 245k (stride 60: 243k in 60 draws, i.e. the terrain / level meshes) |
| InstancedMesh 68 [Main 68] | instanced; stride 52 | Solid 61 (+ Alpha) | Solid 31k |
| SpeedTree 191 [Main 130, Shadow 61] | instanced + wind: branches / fronds stride 60 (Solid 42, Prepass 28, Shadow 33); leaf cards stride 88 (28 / 17 / 21); leaf meshes stride 96 (9 / 6 / 7) | 130 / 61 | Solid 16.7k + 3.2k + 4.4k |
| SpeedTreeBillboard 80 [Main 80] | "other" (POS FLOAT4 + TC5 + BW0..2); stride 64 | Solid 40, Prepass 40 | 12.8k per pass |
| Vegetation 100 [Main 100] | vegetation; stride 52 (+ stream 1) | Solid 75, Prepass 25 | Solid 5.5k |
| Skinned 42 [Main 20, Shadow 22] | skinned; stride 72 | Solid 10, Prepass 10, Shadow 22 | 16.5k per pass |
| Sky 4, Post 19, ShadowMask 9, Gui 1 | n/a | | |

Totals: 790 draws. Histogram at the spot camera (frame 5892, the burst): StaticMesh 307 [234 / 73], SpeedTree 164 [101 /
63], Vegetation 92, SpeedTreeBillboard 88, InstancedMesh 78, Skinned 16, Refracted 12. For comparison, the jungle spot at
frame 1109 had SpeedTree 615, StaticMesh 363, SpeedTreeBillboard 136, Vegetation 113, InstancedMesh 88, Skinned 91.

Not measured: the number of distinct meshes (vertex buffers) and the instances per instanced draw. The run's log, which
had the stream-0 pointers, was overwritten by run 2 before it was saved, and neither value is logged anywhere. A
records dump (`Device::drawRecords().last()`: `vertexBuffer.id`, `instances`) gives both; it is the first thing the RT
instance registry would log anyway. The only proxy: plain Main/Solid stride-60 draws used 13 distinct declaration
handles for 60 draws.

## 3. Vertex declarations (trace run 2: all 18 distinct ones at the spot) [C]

Types: F1/F2/F3/F4 = FLOAT1..4, C = D3DCOLOR, U4 = UBYTE4. `sN` = stream. Kind = matched by stride + the VS inputs of
the class that drew it (2.6) [C for the declarations; the kind mapping is C where the stride + VS inputs are unique,
otherwise noted].

| stride | elements (stream 0 unless noted) | kind |
|---|---|---|
| **52** | POS F3 @0, NORMAL F3 @12, TANGENT F3 @24, COLOR0 C @36, COLOR1 C @40, TC0 F2 @44; **s3 TC4 F1 @0** | StaticMesh / InstancedMesh / water (water.md's logged decl is this one) |
| 52 | same as above + **s1: BW0 F4 @0, BW1 F4 @16, BW2 F3 @32** + s3 TC4 | Vegetation (stream 1 = per-instance) |
| 60 | 52-layout + TC1 F2 @52; s3 TC4 | StaticMesh (second uv set) |
| 60 | 52-layout + **TC6 F2 @52** (lightmap uv) | StaticMesh, lightmapped (no instance stream) |
| 68 | 52-layout + TC1 F2 @52 + TC6 F2 @60; s3 TC4 | StaticMesh |
| 60 | POS F3 @0, NORMAL F3 @12, TANGENT F3 @24, COLOR0 C @36, TC0 F2 @40, BW1 F1 @48, BW0 F1 @52, BI0 U4 @56; **s1 TC4 F1** | SpeedTree branches / fronds |
| 88 | ... TC0 F2 @40, BW2 F4 @48, BW0 F1 @64, BI0 U4 @68, BW1 F4 @72; s1 TC4 | SpeedTree leaf cards |
| 96 | ... TC0 F2 @40, BW2 F4 @48, BW0 F1 @64, BI0 U4 @68, NORMAL1 F3 @72, NORMAL2 F3 @84; s1 TC4 | SpeedTree leaf meshes |
| 64 | **POS F4 @0** (w = corner), TC5 F2 @16, BW0 F4 @24, BW1 F3 @40, BW2 F3 @52 | SpeedTreeBillboard |
| 72 | POS F3 @0, BW0 F4 @12, BI0 U4 @28, NORMAL F3 @32, TANGENT F3 @44, COLOR0 C @56, COLOR1 C @60, TC0 F2 @64 | Skinned |
| 72 + s1..s3 | the same + s1/s2/s3: POS1..3 F3 @0, NORMAL1..3 C @12 | Skinned with morph targets |
| 48 | POS F3, NORMAL F3 @12, COLOR0 @24, COLOR1 @28, TC0 @32, TC1 @40 | non-material (particles / FX) [I] |
| 52 | POS F3, NORMAL F3, TANGENT F3, TC0 F2 @36, TC5 F2 @44 | screen-aligned billboard mesh [I] |
| 32 | POS F3, COLOR0 @12, TC0 @16, TC5 @24 / TC1 @24 | billboards / particles [I] |
| 28 / 20 / 12 | POS F4 + C + TC0 (GUI); POS F3 + TC0 (post quads); POS F3 (sky) | not geometry for RT |

Consequences for the BLAS [C]:

* **Every world-geometry kind except SpeedTree billboards has POSITION FLOAT3 at offset 0 of stream 0, in object space.**
  So `VkAccelerationStructureGeometryTrianglesDataKHR` can point straight at the engine's VB:
  `vertexFormat R32G32B32_SFLOAT`, `vertexStride` = the stream stride, `vertexData` = buffer address + stream offset.
  No conversion pass is needed for static, instanced, SpeedTree or grass geometry.
* uv0 = TC0 FLOAT2 at offset 44 (static, stride 52/60/68), 40 (SpeedTree), 64 (skinned).
  Lightmap uv = TC6 FLOAT2 at offset 52 (stride 60) or 60 (stride 68); absent otherwise. The VS then reads the missing
  input as 0: `bindVertexInput` binds a null buffer, `src/d3d9/draw.cpp:209`.
* Indices: triangle lists (type 4) in all world draws seen at frame 3700; 16- or 32-bit (`Buffer::index32()`). A sub-mesh
  draws `first` (start index) / `primitives` / `baseVertex` (records, `ReplaceQuery`).
* The same mesh VB is used by the plain and the instanced variant. The kind and the VS class decide where the transform
  comes from.

## 4. Texcoords: what the hit shader must reproduce

### 4.1 The VS forms four uv sets from the vertex uv [C]

`VS_Default`: `o2.xy = XFX_VS_TEX0`, `o3.xy = XFX_VS_TEX1`, `o2.zw = XFX_VS_TEX2`, `o3.zw = XFX_VS_TEX3`, where o2 is the
PS's TEXCOORD0 and o3 its TEXCOORD1. Each macro defaults to `input.vTex0` (`ge_default.fxh:47-61`); the material's
generated code may redefine it.

* **79 of 149 VS transform the uv** with `g_vShaderData` (c232..c242). Every pattern in the corpus is **affine in uv0**:
  * `mul c, uv` (scale);
  * `add c, uv` (offset);
  * `mad uv, cA, cB` (scale + offset);
  * `dp3 (uv * cS, 1), c_row0 / c_row1` (2x3 affine: tiling + rotation + offset);
  * chains of these, e.g. `vs30_2fbf406a8ef0f61a`: `o2.xy = (uv + c242) * c241`.

  One permutation family reads TEXCOORD1 instead of uv0. The rest pass uv0 through unchanged
  (`mov o2, v5.xyxy; mov o3, v5.xyxy`). The full per-VS list is in `out/vs_classify.py`'s output.
* SpeedTree, vegetation and skinned VS mostly pass uv0 through (1 SpeedTree VS scales by c232). SpeedTree billboards
  compute uv from the atlas table (2.3).

### 4.2 The PS picks a set per sampler [C]

Survey of all 2282 material PS (`out/ps_samplers.py`):

* **s9 (`g_SamplerData[0]`) is sampled in 2273 of them**, and is the lowest material sampler not decoded as a normal map in
  2237.
* Its coordinate:

  | coordinate | PS |
  |---|---|
  | TEXCOORD0.xy (= VS TEX0) | 915 |
  | TEXCOORD1.xy (= VS TEX1) | 793 |
  | .zw sets | 46 |
  | computed in the PS (~520; parallax, uv offsets from another texture, blends) | the rest |

  Example `ps30_b20d1a1354a09194`: `texld r0, v0, s9`, then `oC0.rgb = r0 * lighting + fog`.
* Which sampler holds the albedo in general (s9 is the default, not a rule): see docs/research/rt-albedo-facts.md 1.

### 4.2a Measured by M9 P2 (2026-10-22) [C]

The proposal below was implemented as "evaluate the draw's own VS + PS on the CPU" (`src/shader/sm_eval.*`,
`src/enh/rt_material.cpp`, rt-reflections.md 3.2): the coordinate of the PS's first texld of the diffuse sampler as a function
of the vertex's TEXCOORD0 / TEXCOORD1, cached by the values of the constants a backward slice of both programs names.

* Corpus (`rvk_shadertool evalcheck corpus`): **all 2476 SM 3.0 shaders evaluate** without an unsupported instruction; the
  2273 PS sampling s9 have **no flow control** in the slice of that coordinate and depend on **0.45 PS constants** on average.
* In game (5 spots, 159 material x PS lines): **136 identity maps, 17 x2, one each x3 / x3.5 / x5**, the lava's scale + scroll
  (0.7, offset), **none on TEXCOORD1**, 2 not affine in the vertex uv (`Special_Skydome_Reflect_04` of a wet specular layer:
  a reflection-vector coordinate; the lava). The tex-coord sources of the graph (56 lines say "tex-coord source") are the
  scales: they live in the VS (c232+) as 4.1 found.

### 4.3 What a hit shader needs [I, a proposal]

Per geometry record:

1. the VB address, stride, uv0 offset;
2. the diffuse sampler's bindless texture index (s9 of the draw's `Record::textures` / `ReplaceQuery::textures`);
3. **which of the four VS sets** the PS samples it with;
4. **that set's affine map** `uv' = A * uv0 + b` (2x3).

Items 3 and 4 differ per VS and per material constants. A generic, exact way to get the affine map without parsing VS code:

* Evaluate the draw's translated VS (or the CPU decoder) on three probe vertices, uv0 = (0,0), (1,0), (0,1), with the
  draw's own constants.
* Read o2 / o3 and solve for A, b per set.

All transforms in the corpus are affine in uv0 (4.1), so three probes are exact. Per material and constant state they are
cheap and cacheable. Animated uv (scrolling water / lava) changes the constants: re-probe when c232.. change.
PS-computed coordinates (~23 % of the PS) fall back to the set's affine uv (approximate) [I].

## 5. Where our code has the data at draw time [C]

* **VS constants on the CPU**:
  * `d3d9::Device::vsF_[256][4]` (`src/d3d9/device.h:707`), the D3D9 float constants as last set;
  * read with **`Device::vsConstant(reg)`** (`device.h:575`, render thread);
  * set by `Device::setVertexShaderConstantF` (`src/d3d9/device.cpp:1050`) from `HalDevice::SetVertexProgramRegisterF`
    (`src/hal/vulkan/device.cpp:1374`) and from effects (`src/hal/vulkan/effect.cpp:353`).

  Valid during `prepareDraw` (`src/d3d9/draw.cpp:470`). A registry hook placed where `observeOpaque` is called
  (`draw.cpp:510-518`) sees exactly the draw's constants: c33..c35 (plain), `c[4i+1..]` for i < `instanceCount_`
  (instanced), c232.. (uv transforms).

  Prior art, `src/enh/water.cpp`:
  * `Water::worldRows` (`:201-205`) copies `vsConstant(33..35)` as the world rows;
  * `Water::observeOpaque` (`:715`) reads the IB / VB with `readCpu` and transforms positions by those rows, for the
    sea-floor bake.
* **GPU copies**:
  * `uploadConstants` (`draw.cpp:439-460`) copies `vsF_[0 .. reflection.floatConstCount)` into the frame's ring
    (`StageConsts`, `shader/abi.h`, 4416 bytes);
  * `Record::vsConsts` / `psConsts` / `drawBlock` (`src/d3d9/draws.h:136`, filled at `draw.cpp:776-778`) are those
    addresses, valid until the frame's ring slot is reused.

  A GPU-side builder (compute pass) can read the instance matrices from `vsConsts + 16 * reg` instead.
* **Per-draw description**:
  * `ReplaceQuery` (`device.h:245-264`): kind, phase, tag (entity / sub-mesh / material / mesh resource), stream-0
    `Buffer*` + offset + stride, index `Buffer*`, primitive type, first / primitives / baseVertex, instanced flag,
    textures s0..s15, engine VS / PS, `VertexDecl*`;
  * `draws::Record` (`draws.h:100-138`): the same as ids + generations (`ResourceRef`: `ref()` changes when a static
    buffer gets a new `VkBuffer`, the BLAS cache key), `instances`, VS / PS hashes + semantics flags.

  Note: `ReplaceQuery` and the record cover **stream 0 only**. Streams 1 (vegetation / SpeedTree instance data) and 3 are
  in `Device::streams_[]` (private; `device.h:702`) and would need an accessor.
* `VertexDecl::elements()` (`d3d9::VertexElement`: stream, offset, type, usage, usageIndex), `hasBlendIndices()`,
  `hasTexcoord4()`.

## 6. Vertex / index buffers: CPU copies and usage [C]

* **Every `d3d9::Buffer` keeps a full CPU copy** (`std::vector<uint8_t> cpu_` sized at creation, `src/d3d9/resources.cpp:355-356`):
  * `Lock` returns a pointer into it;
  * **`Buffer::readCpu(offset, size, out)`** (`:378-383`, mutex, any thread) copies from it;
  * static, dynamic and system-memory buffers alike, so CPU-side bounding boxes, hashing, or a CPU BLAS-input conversion
    are possible.
* Static buffers (`Buffer::gpu`, `:417-450`): a **new device-local `VkBuffer` per update** (the old one is retired). With
  the RT tier on, usage `TRANSFER_DST | VERTEX/INDEX | SHADER_DEVICE_ADDRESS | STORAGE_BUFFER |
  ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_KHR` (`:423-427`).
  * Static mesh VBs keep their generation while unchanged (draw-tags.md 2).
  * `Buffer::Binding{buffer, offset}` exposes the `VkBuffer`; there is no device-address accessor yet
    (`vkGetBufferDeviceAddress` on `binding.buffer`).
* Dynamic / system-memory buffers (vegetation instance stream, particles): a ring copy per frame
  (`Frames::newChunk` usage includes `ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_KHR`, raytracing.md 4). Valid for one
  frame: a BLAS input from them must be built that frame, or copied.

## 7. Summary for the M9 registry [I unless noted]

| kind | BLAS input | TLAS transform | uv for the hit | note |
|---|---|---|---|---|
| StaticMesh | VB pos F3 @0 + IB range per sub-mesh [C] | c33..c35 rows [C] | TC0 @44 + affine (4.3) | terrain = big stride-60 meshes |
| InstancedMesh | same mesh [C] | `c[4i+1..3]`, i < instances [C] | same | ≤ 50 per draw |
| SpeedTree branches / fronds | VB pos F3 @0 (rest pose, wind ignored) | `c[4i+1..3]` | TC0 @40 | alpha test (fronds) |
| SpeedTree leaf meshes | pos + MeshMatrix * BW2.xyz: needs a small expansion pass | `c[4i+1..3]` | TC0 @40 | alpha test |
| SpeedTree leaf cards | card quads expanded with a fixed orientation (c23..c25 of a frame) | `c[4i+1..3]`, offset added in world space | TC0 @40 | alpha test, camera-dependent |
| SpeedTreeBillboard | skip (or crossed quads at the world pivot) | identity | atlas table | camera-dependent |
| Vegetation | the grass mesh (stream 0) | per stream-1 record: quat + scale.xyx + offset [C math] | TC0 @44 | alpha test; often excluded |
| Skinned | skinning pass (bones c33 + 3b) or VS capture | identity after skinning | TC0 @64 | dynamic |
| Refracted (water) | excluded (it is the reflector) | | | |

## 8. Open

* ~~Distinct meshes and instances per draw at the spot~~ Measured by the M9 P1 RT scene (rt-reflections.md 3.1): 170-420
  instances from 190-400 distinct draw ranges (BLAS) at lake_jungle / harbour_docks / beach_sun.
* The values of `g_vLeafAngleMatrix` (c23..c25): camera-facing or fixed? Log the VS constants of one leaf-card draw.
* Stream-1 frequency of vegetation / SpeedTree instance data (INSTANCEDATA assumed); `GetMaxInstanceCount` value.
* ~~Instance stream contents (0, 1, 2, ... assumed)~~ [C, P1: `rt: scene: instance stream check: TEXCOORD4 = 0, 1, 2, ...
  in every batch`, read per draw with `readCpu`].
* The per-PS diffuse set and the PS-computed uv cases (4.2, 4.3): rt-albedo-facts.md.

## 9. Found by the RT scene (M9 P1) [C]

* The sun cascades draw water meshes (sea, lakes, rivers, the oil puddle, glass) and meshes the main view draws alpha-blended
  as ordinary untagged StaticMesh draws (`Tag::water` is resolved only in the Refracted / Alpha passes).
* A level-of-detail switch draws another mesh resource / `eCSubMesh` for the same entity and `opDesc` (sub-mesh index |
  node); the cascades may draw another LOD than the main view.
* The main view's linear depth map holds only the z-pass draws (e.g. the walkway planks at lake_jungle are missing): the
  hardware depth buffer at AfterOpaque has every solid draw.
* World-space level meshes: the island's level mesh draws with identity rows (vertices in world space, one entity, sub-meshes
  of 800-140,000 triangles, VB / IB shared, 32-bit indices); terrain layers share geometry (opaque base + blended layers).
* beach_sun's shore strip (level mesh `op 0x3`, 1872 triangles at sea level) renders 15-20 units away from its static
  vertices (VS displacement or offset; not resolved).

## 10. Found by the RT materials (M9 P2) [C]

* **Merged ranges**: the sun cascades (and the main view's z pre-pass) draw a level mesh in **one index range** (e.g. first
  307113 x 141,446 triangles at lake_jungle) while the solid pass draws it **per material** in sub-ranges inside it (same
  VB / IB / base vertex, e.g. first 307113 x 20,426). The merged range is a BLAS of its own, coplanar with the sub-range
  BLASes; its triangles take the sub-ranges' materials (rt-reflections.md 3.2 "segments").
* Level meshes / terrain: a z pre-pass range + alpha-over blended layers (blend mode 2, vertex-colour opacity) in the solid
  pass, sometimes on sub-ranges that are never drawn opaque; overlays on top of them: caustics (texture opacity), wet
  specular layers (a reflection map or a cube as the diffuse, constant opacity).
* The same mesh range can be drawn with several materials (instanced items, layers): materials are per instance.
* **SpeedTree branches / fronds are drawn as triangle strips** (D3DPT_TRIANGLESTRIP; `rt: scene: SpeedTree draw (op 0,
  Shadow ShadowDepth, stride 60) not taken: not a triangle list`), contrary to 3 ("triangle lists in all world draws") and
  P1's assumption: none reach the RT scene (0 SpeedTree records at jungle / lake_jungle / beach). A BLAS needs the strip
  converted to a list (a CPU index conversion at build time).
