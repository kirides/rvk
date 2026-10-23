# Shader permutations of the shipped corpus

The 2483 captured shaders (2334 PS + 149 VS) are **compiled permutations of one HLSL ubershader**, not hand-made shaders.
Reproduce: `rvk_shadertool export corpus game-shaders`, then `python tools/shader_permutations.py game-shaders` (writes
`out/shader-permutations/macros.csv`: one row per shader with its inferred macro vector).

## 1. Where the permutations come from [C: source read; counts measured]

The game ships the HLSL (`data\compiled\effects\shadermaterial\ge_default.fxh`, `ge_default_ps.fx`, `ge_default_vs.fx`,
`ge_default_phong.fx`; materials.md 2). `ge_default.fxh` declares ~35 `XFX_*` defines with defaults and turns them into
compile-time constants (`static bool g_bDirLight = XFX_DIRLIGHT;`, `static int g_iPntLightCount = XFX_PNTLIGHTCOUNT;`, ...);
`ge_default_ps.fx` branches on them (`if (g_bLightMap) AddLightMap(..)`, `if (g_bDirLight) AddDirLight(..)`,
`for (i < g_iPntLightCount) AddPointLight(..)`), which D3DX removes at compile time. A permutation = one define vector + the
material's generated `XFX_PS_DIFFUSE / OPACITY / NORMAL / SPECULAR / SPECULARPOWER / SELFILLUMINATION / EMISSION / DISTORTION`
code snippets (the material graph).

So the shaders factor into **(material graph) x (engine-state macros)**.

## 2. Pixel shaders: the engine-state macros are a clean cross product

Read back from each shader's own constant table (`g_DirLight` present = `XFX_DIRLIGHT`, `g_PointLights` registers / 4 =
`XFX_PNTLIGHTCOUNT`, `smpLightMap1` = `XFX_LIGHTMAP`, `smpRefract` = `XFX_DISTORTION`, `g_FogSettings` = `XFX_FOG`, ...):

| macro | values in the corpus |
|---|---|
| `XFX_DIRLIGHT` | 0: 1186, 1: 1148 |
| `XFX_PNTLIGHTCOUNT` | 0: 735, 1: 725, 2: 437, 3: 437 (`XFX_MAX_PNTLIGHTS` = 3 on SM 3) |
| `XFX_LIGHTMAP` | 0: 1758, 1: 576 |
| `XFX_DISTORTION` (smpRefract; 156 also read smpDepthmap) | 132 |
| `XFX_AMBIENTLIGHT` / `XFX_FOG` / shadow mask sampler / `g_ShaderData` | 2246 / 2277 / 2168 / 2180 |
| `XFX_CLIPALPHATEST` (texkill) | 40 |

**The ordinary world pass** (no lightmap, no distortion, shadow mask + ambient + shader data): shaders per
(`XFX_DIRLIGHT`, `XFX_PNTLIGHTCOUNT`): `(0,1) (0,2) (0,3) (1,0) (1,1) (1,2) (1,3)` = **189 each (188 for (1,0))**, `(0,0)` = 118.
That is the same ~189 materials compiled for 2 x 4 = 8 lighting variants. The lightmapped set (`XFX_LIGHTMAP`) repeats the
pattern with 121-123 per variant over (dir 0/1, pointlights 0/1) = ~123 materials x 4 variants. Only 72 distinct lighting
variants exist over all 2334 PS; 14 of them hold 2200+ shaders.

Estimated distinct material graphs: >= ~400 (max shaders per lighting variant, summed over the 148 material-sampler keys;
coarse: lightmapped and refractive sets count separately), i.e. **~5.8 lighting permutations per material**. Conservatively:
~190 world materials, ~125 lightmapped, ~25 refractive, the rest special.

Evidence that the variants are the same code: `ps30_67766910d99f3660` vs `ps30_bc43ff069836d825` (same material, 1 point
light, `XFX_DIRLIGHT` 0 -> 1) differ only by the sun's Lambert block (`dp3_sat` / `lrp` / `mad` for `g_DirLight`); the
remaining instructions match up to register numbers and scheduling.

## 3. Vertex shaders

149 VS = the VS-side defines: `XFX_INSTANCED` 141, `XFX_SKINNED` 36, SpeedTree (`g_vWindMatrix`) 20, leaves 12,
`XFX_VEGETATION` 5, tangent input 91 (the rest are skinned / particle / billboard), `g_vShaderData` 79. The set is the product of a
few geometry-class macros with the tangent / shader-data axes; fewer permutations than the PS because the material graph
does not enter the VS.

## 4. What this means for plain-text source

* The exact source of the 2334 PS is **~190 + ~125 + ... material snippets x the macro vector above**, and the pipeline code
  (`ge_default_ps.fx`) is shared. A macro-driven GLSL ubershader (`#define XFX_DIRLIGHT`, `XFX_PNTLIGHTCOUNT 0..3`,
  `XFX_LIGHTMAP`, `XFX_DISTORTION`, `XFX_FOG`, ...) + one include per material graph replaces them.
* The translated bytecode is **not** identical across variants: D3DX schedules and allocates registers per compile, so a
  macro'd GLSL is an equivalent rewrite, never byte-exact. The shipped HLSL is the exact source: it is ported in
  `shaders/material` (section 6) and checked against the translated game shaders on the GPU (section 7).
* Per-material snippets are not in the corpus (they come from the `ShaderMaterialPool` build, materials.md 5): to list them,
  group shaders by `mat_samplers` + `mat_fetch_order` in `macros.csv` and diff within a group.

## 5. The shipped HLSL reproduces the shader pool instruction for instruction [C]

`tools/hlsl_match.py <fx dir> <corpus> <shader> -D NAME=VALUE ...` compiles the game's HLSL with D3DX and compares the
instruction stream with a corpus shader:

* **Compiler**: the corpus' creator string is `Microsoft (R) D3DX9 Shader Compiler 9.11.519.0000` = **`d3dx9_29.dll`** (present in
  System32; d3dx9_39, which the other tools use, writes different register allocation). Flags **0** (the `half` types give the
  `_pp` hints; `D3DXSHADER_PARTIALPRECISION` marks too much), entry `PS_Default` (`PS_DefaultZPass` for the depth passes), profile
  `ps_3_0`, file `ge_default_phong.fx`, defines `XFX_SHADERMODEL_3`, `XFX_QUALITY=2` + the macro vector + the material's
  `XFX_PS_*` expressions (`tex2D( g_SamplerData[k], XFX_PS_TEX0 )` with the swizzles of rt-albedo-facts.md 1.1).
* **One revision difference**: the pool was built from an older `ge_default_ps.fx`: `InitPassData` has `half3 vecWorldEye` where the
  file on disk says `float3` (this changes the `_pp` of the fog distance maths). `tools/hlsl_infer.py --make-fx <dst>` writes the
  patched copy of the shadermaterial directory (default `out/hlsl-fx`). Nothing else differed in the shaders tried.
* The pool's shaders carry no `_pp` on the `dcl` register tokens; `hlsl_match.py` ignores that bit.
* **Reproduced exactly**: `ps30_038f29bf19cc049e` (unlit diffuse + self illumination 1.0, radial fog:
  `XFX_FOG XFX_DISABLELIGHTING XFX_AMBIENTOCCLUSION=false XFX_PS_DIFFUSE XFX_PS_SELFILLUMINATION=1.0`) and `ps30_0a7c888f3bfcec67`
  (sun, SH ambient, DXT5nm normal on s10: `XFX_DIRLIGHT XFX_AMBIENTLIGHT XFX_AMBIENTLIGHTTYPE=2 XFX_COMPRESSEDNORMALMAP
  XFX_PS_DIFFUSE XFX_PS_NORMAL`). So the define system, the entry points and the compiler are right.
* **Automatic inference is partial**: `tools/hlsl_infer.py` (macros from the constant table, searched toggles, canonical
  expressions per sampler role) solved 4 of 60 random pixel shaders (6.7 %, all one-sampler materials without `g_ShaderData`). The
  rest need what the engine's node implementations generate beyond `tex2D(..).swz`: `g_ShaderData[i]` constants multiplying
  nodes (most shaders: c6..c13), cube samplers, `lerp` / mask nodes, and the other effect families in the corpus (e.g. the camera
  obstacle shaders, `g_CamObstacleAndMaskReference`). A complete define list per corpus shader needs the node strings
  (`eCShaderDefault::GetDeclarations` 0x6c8650, `eCColorSrc*::GetImplementation`) or a decode of `ShaderMaterialPool_Master.smp`.

## 6. The GLSL port: `shaders/material` [C]

`ge_default.glsl` (defines, HLSL vocabulary, constants), `ge_default.frag` (`PS_Default`, `PS_DefaultZPass`; `ge_default_ps.fx` +
`ge_default_phong.fx` + the hooks of `ge_default.fxh`), `ge_default.vert` (`VS_Default`, `VS_DefaultZPass`; `ge_default_vs.fx`).

* Same defines as the HLSL as `-DXFX_DIRLIGHT=1 -DXFX_PNTLIGHTCOUNT=2 ...` (booleans 0 / 1; `XFX_PASS=1` selects the z pass). The
  material graph is `-D"XFX_PS_DIFFUSE=tex2D( g_SamplerData[0], XFX_PS_TEX0 )"` etc.: the engine's expressions are valid GLSL as
  they are (`tex2D`, `texCUBE`, `g_SamplerData[k]` = register s9+k, `g_ShaderData[i]` = c6+i, `lerp`, `saturate`, `frac` come from the
  compatibility layer; the VS has `input.vTex0`, `g_vShaderData[i]`).
* Interface = the translator's ABI, so a variant replaces the translated shader: varyings at location = TEXCOORD index (COLOR0 =
  16), `pc.ps->c[n]` / `pc.vs->c[n]` constants, `pc.draw` texture / sampler slots, the alpha test, the half-pixel fix. The VS inputs
  are declared only when the macros read them, locations in the order of the file (renderer side: map usage -> location from it).
* **D3D9 numerics are kept**: `pow = |x|^y`, `sqrt = sqrt(|x|)`, `normalize(0) = 0`, and the D3DX `atan2` polynomial
  (`atan2D3D`); plain GLSL gives NaN where the game shows a colour (a negative SH ambient term under the gamma `pow`, a DXT normal with
  x^2 + y^2 > 1).
* Not ported: `PS_DefaultRadiosityPass` (the editor's baker), `GetDistortion` / `ComputeVarianceShadowMap`'s SM2 branch,
  `ge_default_leafs.fx` / `_indicator` / `_preview` (their custom hooks) and the other effect files (particles, sky, light streaks).

## 7. Verification: the port against the translated game shaders on the GPU [C]

`python tools/shader_equiv.py [--vs]` (needs `out/hlsl-fx`, `rvk_shadertool`, `rvk_shadereq`, the Vulkan SDK's glslc): per define
vector the shipped HLSL is compiled with d3dx9_29 (the pool's compiler), translated by `rvk_shadertool export` exactly as the
renderer does at run time, and **run against the port** by `rvk_shadereq` (new, `tools/shadereq`) in the renderer's draw pipeline
(bindless set, buffer-address constants, dynamic rendering) with identical random constants, varyings / vertices and textures.

* Pixel shaders: **31 of 31 vectors equal** (48 random trials each, worst relative difference 4e-7 against the 1e-4 tolerance):
  unlit, sun x 0..3 point lights, DXT5nm / RGB normal maps, specular (complex and simple BRDF), lightmaps, simple / complex / no
  ambient, no AO / no shadows, no fog, alpha blend, opacity + self illumination + emission, distortion (refraction), double sided,
  a lerp / mask material graph, and the z passes (masked, shadow map plain / projected / point, variance map).
* Vertex shaders (the clip position is replaced by a fixed triangle in both, exported as a varying; a probe pixel shader dumps
  the interpolated varyings): **17 of 17 equal**: default, four texcoord sets, instanced, skinned, skinned + morphed, billboard,
  vegetation, SpeedTree branch / leaf mesh / leaf cards / billboards (both azimuth variants), z passes (masked, shadow map,
  leaf cards, vegetation).
* The check discriminates: a deliberately wrong pairing (sun on / off) differs in 21 of 48 trials.
* **Known divergence (invalid data only)**: with random inputs the SpeedTree billboards disagreed in ~2 % of trials. The bytecode's
  `int(az / slice + 1)` is `x - frc(az / slice) + ...`, which is not exact at integer values, so `iImage360 > iNumImages - 1` can
  flip when `iImage360 == iNumImages - 1`; the port has the ideal semantics. With valid inputs (azimuth in [0, 2 pi], 8 images;
  `shader_equiv.py` passes `--billboard-valid` for these vectors) 2000 trials are equal. A game that hits the exact boundary
  can therefore pick image 0 instead of the last one in the original; not reproduced.
* Textures are random 8x8 RGBA8 with a repeat / linear sampler: the check is arithmetic equality, not a look-alike.
