# RT hit shading: which texture is the albedo (diffuse), alpha test, lightmap

Research note (read-only RE + shipped HLSL + shipped data), 2026-10-22. Goal: shade ray-traced reflection hits with each
material's diffuse texture. Legend: **[C]** confirmed (decompiled Engine.dll, shipped HLSL, shipped data), **[I]** inferred
or not yet seen in the running game. RVAs are Engine.dll (image base 0x180000000). Dumps:
`B:\DevDrive\risen-ghidra\albedo_dump{,2,3,4,5,6}.txt`. Related: `materials.md` (maps, normal decode), `draw-tags.md`
(`eCSubMesh` +0x38 = material).

## 1. Which sampler is the diffuse

### 1.1 How the engine assigns sampler slots [C]

The material pixel shader is not hand-written per material: `ge_default_phong.fx` / `ge_default_leafs.fx` + `ge_default_ps.fx`
are compiled with macros generated from the material's node graph (`eCShaderDefault::GetDeclarations` 0x6c8650 emits, per
role, `XFX_PS_DIFFUSE`, `XFX_PS_OPACITY`, `XFX_PS_SELFILLUMINATION`, `XFX_PS_SPECULAR`, `XFX_PS_SPECULARPOWER`, `XFX_PS_NORMAL`,
`XFX_PS_DISTORTION`, `XFX_PS_EMISSION` = an HLSL expression). A sampler node's expression is
`tex2D( g_SamplerData[k], XFX_PS_TEXj )` (`eCColorSrcSampler::GetImplementation` 0x6c59d0; combiner `a * b` / `+` / `-` / `max` /
`min`, blend `lerp(a,b,t)`, mask `( a > thr ) ? 1 : 0`, vertex colour `XFX_PS_VERTEXCOLOR`, cube `texCUBE( g_SamplerData[k],
XFX_PS_REFLECT )`; each proxy appends a channel swizzle table `"" .rgb .r .g .b .a` and an operator table `"" - 1- saturate
ceil floor abs frac`, proxy +0x28 / +0x2c).

`k` is the **index in `g_SamplerData` (register s9 + k)**, handed out in a fixed traversal order:

* `eCShaderDefault::Create` 0x6c7ff0: sampler pool name `"g_SamplerData"`, `this+0x219 = 9` (first register), `this+0x21a = 7`
  (max samplers), `this+0x21b = 0` (counter). Pool = `shader + 0xf8`, 7 entries of 0x28 bytes; entry +0x20 = `eCImageResource2*`
  of that slot; `shader+0x210` = the name bCString.
* `eCShaderBase::UpdateShaderConstants` 0x6d28a0 marks every element active, then walks `GetProxyAt(0..GetProxyCount-1)`
  (vtable +0x100 / +0xf8) and calls each reachable element's `UpdateShaderConstants` (vtable +0x140) once (the active flag at
  element +0x12 clears after the visit). `eCColorSrcSampler::UpdateShaderConstants` 0x6c54a0 does
  `k = pool.counter++` (or -1 when the pool is full) **before** recursing into its own children (`eCShaderEllementBase::
  UpdateShaderConstants` 0x6cdfd0 = same walk over the element's proxies). `eCColorSrcCubeSampler` 0x6c17b0 draws from the same
  pool. So slots are dense (s9 .. s9+n-1), a shared element gets its slot at its first visit.
* Proxy order of `eCShaderDefault` (`GetProxyAt` 0x6c8220, `GetProxyNameAt` 0x6c82b0; `eCShaderLeafs` inherits it):
  0 Diffuse (+0x2e0), 1 Opacity (+0x310), 2 SelfIllumination (+0x340), 3 Specular (+0x370), 4 SpecularPower (+0x3a0), 5 Normal
  (+0x3d0), 6 Distortion (+0x400), 7 LightEmission (+0x430); proxy = 0x30 bytes: vtable, element pointer at +8, element guid at
  +0x10, swizzle index +0x28, operator index +0x2c.
* Child order: combiner `GetProxyAt` 0x6bf580: +0x58 (colour 1), +0x88 (colour 2); blend 0x6bd2c0: +0x48, +0x78, +0xa8 (lerp
  factor last); mask 0x6c2c00: +0x50.

**Consequence: if the Diffuse proxy's tree contains at least one sampler, the first sampler of that tree (depth first, in the
child order above) is `g_SamplerData[0]` = s9.** Every other role gets higher slots (a texture used by several roles keeps its
first slot). If Diffuse has no sampler (constant colour, vertex colour, cube-only), s9 belongs to a later role: **s9 is then NOT
the albedo** (shipped xmats: ~44 of ~449 image-bearing materials have no `_Diffuse` image, e.g. hero armour with only
Normal / Detail / Reflect; 67 xmats have no image at all).

Supporting evidence from the shipped data [C]:
* Shader corpus (2282 material PS with s9+ samplers): declared sets are always contiguous from s9 (`9`, `9-10`, `9-11`,
  `9-12` ... `9-15`), and the first-use order of `texld` shows the HLSL order (GetNormalMap first, then diffuse): `(10, 9)` 262x,
  `(9)` 213x, `(11,10,9)` 205x, `(11,12,10,9)` 173x = normal on the highest of the early slots, diffuse last, s9.
* `materials.pak` xmats (516 files): the first image path of 389 of 449 image-bearing materials is a `*_Diffuse*` texture;
  suffix census over all image paths: `_Diffuse` 329 + `_Diffuse_S1` 124 + lower-case 33, `_Normal` 208 + `_Normal_S1` 102,
  `_Comp` 157, `_Specular_S1` 49, `_Reflect` (cube) 49, `_Detail_*_Normal/Diffuse` ~100.

### 1.2 Options to learn the slot at runtime

**(a) Read the engine's material graph (exact; recommended).** Per draw, the renderer needs the `eCShaderBase` whose pool is
bound. Two sources:
* `eCSubMesh +0x38` = `eCMaterialResource2*` (already tagged, `scene_hooks.cpp` `t.material`), shader = `*(material + 0x70)`
  (`eCMaterialResource2::GetShader` 0x9fab0, COMDAT with `eCImageResource2::GetImageData`: `[this + 0x70]`, same +0x70 data
  pointer the engine reads for images, `ExecuteShaderConstants`) [C for the accessor, I for in-game value].
* Better, because it handles the fallback shader (`GetPasses` 0x6d1ee0 may switch to `shader+0x2a0`'s shader), overlay
  materials and non-material callers: the exported data symbol **`eCShaderBase::ms_pActiveShader` (RVA 0xc62970)**, set by
  `eCShaderBase::ExecuteShaderConstants` 0x6d2f90 for each pass [C for the store, I that it is current at every HAL draw].
  Check the class by the vtable pointer: `eCShaderDefault::vftable` 0x97bdb0, `eCShaderLeafs::vftable` 0x9d7be8 (others:
  SkyDome, Particle, LightStreaks: no `g_SamplerData` layout).

Then (all offsets [C] from the decompilation; first run must verify them in game, see 1.4):
```
proxy   = shader + 0x2e0                    // Diffuse; 0x310 Opacity; 0x370 Specular; 0x3d0 Normal ...
elem    = *(void**)(proxy + 8)              // null = role unused
findSampler(elem):                          // depth first, same order the engine allocates slots
  vtbl == base+0x9823c8 (eCColorSrcSampler):  k = *(u32*)(elem + 0x98)   // -1 = none; pool ptr elem+0x90 != 0
       sampler type *(i32*)(elem + 0xd4): 0 image, 1 timed / animated (SwitchBegin ...), 2 switch; image array elem+0xe0 / count elem+0xe8
       uv set (no tex-coord source, elem+0xa8 == 0): *(u32*)(elem + 0xc8)  // XFX_VS_INPUT_TEX<n>, n in 0..3
       sRGB flag byte elem + 0x8c; TexRepeatU +0x58, TexRepeatV +0x68 (0 wrap, 1 clamp, 2 mirror)
  vtbl == base+0x982078 (eCColorSrcCombiner): findSampler(*(proxy+0x58 +8)), then (proxy+0x88 +8); type at +0x50 (0 add 1 sub 2 mul 3 max 4 min)
  vtbl == base+0x9b6bc0 (eCColorSrcBlend): proxies +0x48, +0x78, +0xa8
  vtbl == base+0x9d5818 (eCColorSrcMask): proxy +0x50
  vtbl == base+0x9d4c28 (eCColorSrcCubeSampler): slot at +0x60 (not an albedo: cube reflection map)
  other (Constant 0x9cd180, VertexColor 0x982210, Fresnel ...): no sampler
```
Result: diffuse slot `k` -> texture = what the draw has bound at `s(9 + k)` (our own texture id, no name link needed), plus
the pool's `eCImageResource2*` at `shader + 0xf8 + 0x20 + 0x28*k` for the name (`+0x40` bCString = file title, e.g.
`Monastery_Stone_Floor_01_Diffuse_01`; sampler +0x48 holds the full image path string) - good for logs and validation.
The pool entry is updated by `UpdateSampler` 0x6c3ab0 / `SetSwitch` 0x6c5310 for animated textures, so read it per frame.

**(b) Name suffix of the texture resource.** Works for ~90 % of materials (`_Diffuse*`) but needs the HAL texture <->
`eCImageResource2` link, which does not exist: `ExecuteShaderConstants` binds `eCImageResource2 +0x70` (`eCGfxImageData`) through
`FUN_1806bc340` 0x6bc340 (sets `eCGfxImageData +0x4c` = stage, `eCGfxImageData::Execute`), and the HAL texture sits behind
`eCGfxImageData` (field not researched). Do not build this; (a) already gives the same name through the pool pointer.
It cannot tell diffuse from detail-diffuse, and misses materials without a `_Diffuse` image.

**(c) Bytecode heuristic (fallback only).** In the PS the normal map is the sampler decoded with `*2-1` (already detected for
`materials.md`); the diffuse is the sampler whose rgb is multiplied with `pow(acc, 0.45)` at the end
(`CustumFragmentFinish`: `colFinal.rgb = Diffuse.rgb * pow(accDiffuse, 0.45) * ssao + spec + selfIllum`). Cheap rule:
**s9, unless** it is declared `dcl_cube`, or the texture is a DXT5nm normal / named `*Normal*`, or it is the sampler the
normal decode uses. Wrong for materials whose Diffuse has no sampler (~10 %): those get a normal / detail map as albedo.

**Recommendation:** (a) with `ms_pActiveShader`, cached per (shader pointer, texture-id tuple) because the graph does not
change per draw; fall back to (c) `s9 + guards` when the shader pointer / vtable check fails or the tree has no sampler
(constant-colour diffuse: use mid grey or the average of the bound texture).

### 1.3 Shading model for a hit [C from the HLSL]

`out.rgb = Diffuse.rgb * pow(acc, 0.45) * ssao + specular*specMask + selfIllum`, `acc` = SH ambient (+ lightmap term) + sun
Lambert * shadow + point lights (all in linear-ish units, then gamma'd by `pow 0.45`); the diffuse texture is multiplied as
sampled (the sampler's `SRGBRead` flag, pool entry +0x14, feeds D3DSAMP_SRGBTEXTURE: follow the draw's own sampler state).
Diffuse role may also be a combiner of diffuse * detail diffuse (`_Detail_*_Diffuse`, 17+11+4+2 xmats): the first sampler is
the base; ignore the rest.

### 1.4 In-game verification still to do [I]

Log once per distinct shader pointer: class (vtable), Diffuse / Opacity / Normal sampler slots, image names from the pool, and
compare with the textures bound at s9..s15 in `-LogDraws` (names from the existing texture ids); pick a plain material
(`Monastery_Stone_Floor_01`: Diffuse s9, Normal s10, Comp s11?), a diffuse-less one (`Ani_Hero_Armor_Smith_BodyMetal_Diffuse`:
Reflect + Detail normal) and a SpeedTree leaf. Confirm that `ms_pActiveShader` is current inside `eCSubMesh::Render` draws.

## 2. Alpha-tested materials

* Blend mode = `eCShaderBase +0x50` (`eEShaderMaterialBlendMode`): **0 Normal, 1 Masked, 2 AlphaBlend**, 7 additive-like
  (Ghost materials), 8 one outlier. Measured over the 516 shipped xmats (value decoded from the property stream): 391 Normal,
  45 Masked (43 `eCShaderDefault` + 2 `eCShaderLeafs`), 64 AlphaBlend, 14 mode 7, 1 mode 8 [C for the census, I for the names
  of 7 / 8]. `IsMasked` 0x6d2f50 = `+0x50 == 1`; `GetDeclarations` 0x6d29b0 then adds `XFX_MASKED=true`.
* Masked materials are: **SpeedTree** (`ST_Bark_01..08`, `ST_Composite_01__Diffuse_{Billboards,Fronds,LeafCards,LeafMeshes}`;
  both `eCShaderLeafs` materials are LeafCards / LeafMeshes), ferns / herbs / ivy (`Nat_Veg_*`, `Nat_Vegetation_Ivy_01`,
  `Special_Brushes_Nat_*`), nets, rags, banners, carpets, straw roofs, posters, monster wings / cloth, player cloth armour
  (the `(null)` material too).
* **Reference: `eCShaderBase +0x58` byte `MaskReference`** (`SetMaskReference` 0xac070), 0..255. Values in the xmats: 80
  (18 materials; all SpeedTree), 125 (9), 128 (5), 120 / 150 / 180 (2 each), 50, 64, 90, 130, 135, 190 once.
* Colour pass: `FillBlendingRenderStates` 0x6d0430 case 1 writes alpha test ENABLED (state word +8 = 2 = `eEAlphaTest` enabled),
  `ref = MaskReference` (times the global fade factor `param_3` when it is not 1.0), `func = 5` (`eECmpFunc` 5 =
  D3DCMP_GREATER, "unchanged" per `hal/types.h`), blending off. The HAL gets this as `SetAlphaTesting` 0x742a50
  (`D3DRS_ALPHAREF` / `ALPHAFUNC`), so **the draw's own `alphaFunc` / `alphaRef` (`DrawBlock`) are the reference** [C code, I
  for the state numbers in game]. The pixel shader's output alpha is `opacity * globalAlpha` (`g_bAlphaBlended` is true for
  blend modes 1, 2, 4, 7 and for mode 0 with an Opacity node: `FinalOutput.a *= PassData.m_fGlobalAlpha`), so the tested value
  is that product.
* Depth / shadow passes: `PS_DefaultZPass`: `Result.a = m_colDiffuseAndAlpha.a * m_fGlobalAlpha; clip( Result.a -
  g_MaskReference )`, `g_MaskReference` = `c5.w` (`g_CamObstacleAndMaskReference`, ge_common_ps.fxh); only the SpeedTree
  billboards clip in the colour pass too (`g_bMasked && g_bSpeedTreeBillboards`).
* **Where the opacity comes from:** `GetOpacityMap`: `m_colDiffuseAndAlpha.a = XFX_PS_OPACITY` when the Opacity proxy has a node,
  otherwise the alpha of the Diffuse expression (`m_colDiffuseAndAlpha = XFX_PS_DIFFUSE`, a float4: the diffuse texture's `.a`,
  DXT5 / DXT3 textures). So by default **opacity = diffuse texture alpha**; materials with an Opacity node (leaf cards have a
  combiner + `eCColorSrcCamObstacle` = camera-near fade) use that tree: resolve with the same `findSampler` on proxy +0x310
  (swizzle at proxy +0x28: 5 = `.a`, 2 = `.r`, ...) and fall back to the diffuse alpha when it is null.
* For RT any-hit: `alpha = texture(diffuse, uv).a * (opacity node ? ... : 1)`; discard when `alpha*255 <= MaskReference`
  (GREATER) using the draw's `alphaRef` / `alphaFunc`; the global alpha (TEXCOORD3.w, instance fade) is per-instance and can
  be taken as 1 for reflections.

## 3. Lightmap

* Samplers (ge_common_ps.fxh) [C]: s0 `smpShadowMask`, s1 `smpRefract`, s2 `smpDepthmap`, **s3 `smpLightMap0` ("light index
  map")**, **s4 `smpLightMap1`** (rgb = lightmap colour, `.a` = ambient light direction x), **s5 `smpLightMap2`** (rgb = second
  lightmap, `.a` = direction y). `AddLightMap` (ge_default_ps.fx): `dir = (a4*2-1, a5*2-1, sqrt(1-x²-y²))`,
  `ambient = lerp( LM2.rgb, LM1.rgb, saturate( dot( tangentNormal, dir ) ) ) * 4`, added to the light accumulator `acc` before
  `pow 0.45`; at quality > Low plus a Phong specular from the lightmap direction. The radiosity-bake variant
  (`XFX_RADIOSITYLIGHTMAP`) adds only s4.
* UV: **`m_vecLightMapCoord = input.vLightmap` = pixel shader input `TEXCOORD7`**, produced by the vertex shader from the
  **vertex input `TEXCOORD6` (`vLightmap`)** (ge_default_vs.fx: TEXCOORD0..3 = material UV sets 0..3, TEXCOORD4 instance index,
  TEXCOORD5 billboard, TEXCOORD6 lightmap UV) [C from the shipped HLSL; the vertex declaration of lightmapped meshes is for the
  vertex-layout research]. Lightmapped meshes are the static level geometry (`g_bLightMap` = `XFX_LIGHTMAP`, chosen by the pass:
  `eCShaderBase::GetPasses` takes an `eEShaderMaterialLightmapCombi`; 484 of the 1894 normal-mapped corpus PS have it).
* Binding [I]: `eCMeshResource2::BindLightmaps` 0x764220 returns the combi mode; `eCShaderMaterialAdmin::SetLightmap(
  eCGfxImageData*, uchar index)` 0x6b80d0 binds stage 4 / 5 through the same `FUN_1806bc340`; the textures are per mesh /
  instance, not per material. A draw's lightmaps = whatever is bound at s4 / s5 when its PS declares them.
* For a reflection hit: indirect = `lerp(LM2, LM1, 0.5-ish) * 4` with the geometric normal (no tangent normal at hit) or just
  `(LM1.rgb + LM2.rgb) * 2`; the `.a` channels are direction data, never colour.

## 4. Summary of offsets (Engine.dll)

| What | Where |
|---|---|
| shader of a material | `eCMaterialResource2 +0x70` (accessor 0x9fab0) |
| currently executing shader | data `eCShaderBase::ms_pActiveShader` RVA 0xc62970 |
| blend mode / mask reference | `eCShaderBase +0x50` (int) / `+0x58` (byte) |
| role proxies | `eCShaderDefault +0x2e0 / 0x310 / 0x340 / 0x370 / 0x3a0 / 0x3d0 / 0x400 / 0x430`, element at proxy +8 |
| sampler pool | `eCShaderBase +0xf8`, 7 x 0x28, image at entry +0x20; base register byte `+0x219` (9), max `+0x21a` (7), count `+0x21b` |
| sampler node | slot `+0x98`, pool `+0x90`, type `+0xd4`, uv set `+0xc8`, tex-coord proxy element `+0xa8`, sRGB `+0x8c`, path `+0x48`, images `+0xe0` / `+0xe8` |
| vtables | Sampler 0x9823c8, Combiner 0x982078, Blend 0x9b6bc0, Mask 0x9d5818, Cube 0x9d4c28, Constant 0x9cd180, VertexColor 0x982210, Default 0x97bdb0, Leafs 0x9d7be8 |
