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

## 8. The engine's request per material shader [C]

* `ShaderMaterialPool_Master.smp` holds only an MD5 key per entry, not the defines: header `SMPL`, u16 version 2, then the counts
  4318 / 914 / 5506 / 1032 / 2549; the 5506 entries are 5444 `ge_default_phong.fx` + 62 `ge_default_leafs.fx`. An entry = 4 bytes,
  the 16-byte key, 4 zero bytes, u16 length + effect name, a few fields, the D3D9 bytecode with CTAB.
* The defines are in the engine's **request**: `eCShaderMaterialAdmin::CompileShaderPoolEllement(eSGfxHLSLShaderSrc const&,
  eEShaderMaterialVersion, unsigned long)` RVA `0x6b8140` (exported; 3778 bytes), called for every material shader the engine asks
  for, pool hits included. Compile path on a miss: overload `0x6b8890` -> `FUN 0x6bb7c0` -> `eCGfxHLSLShader` vtable `+0xe8` -> HAL
  Compile; in the DX9 layer `eCDX9HLSLShader::Compile` `0x749810` -> `D3DXCompileShader` (d3dx9_39.dll). `SaveShaderPool` `0x6b6260`
  writes `Data/Compiled/Materials/ShaderMaterialPool_%s.smp` (trigger unknown, F4 writes it).
* Key preimage [C: dump `shaderq_engine_dump.txt` 3601-3643]: MD5 over `src+0` text / file name, `src+0x10` entry, the 4 bytes at
  `src+0x18` (profile), then each macro name and value of the sorted list; a second key, the "preprocessor id" (D3DX-preprocessed text
  without blanks), on a miss. `shaderId = MD5(bytecode ++ file name)` [I].
* `eSGfxHLSLShaderSrc`: `+0` bCString file name / text, `+8` `eSGfxEffectMacro*` ({name, value}, 16 bytes each, a null name ends),
  `+0x10` bCString entry, `+0x18` u32 profile (0..7 ps, 8..11 vs), `+0x1c` u32 D3DX flags, `+0x20` byte "source is a file".
* The macro list: `eCShaderDefault::GetDeclarations` `0x6c8650` (flags by offset: `+0x2b8` transformation, `+0x2c0` XFX_SPECULAR,
  `0x2c1` DISABLELIGHTING, `0x2c2` FOG, `0x2c3` DIRLIGHTSHADOWS, `0x2c4` PNTLIGHTSHADOWS, `0x2c5` AMBIENTOCCLUSION, `0x2c6`
  COMPRESSEDNORMALMAP, `0x2d9` DOUBLESIDED, `0x2d0` LIGHTINGTYPE; the colour-source proxies' `GetImplementation` give the
  `XFX_PS_<SLOT>` snippet text), then `eCShaderBase::SetupShaderMacros` `0x6d06b0` (renumbers texture slots, merges duplicates, qsort
  by name `0x6ce7b0`) and `eCShaderMaterialAdmin::SetupShaderMacros` `0x6b8ac0` (adds XFX_QUALITY).
* The returned element: `+0x18` = the engine's `eCGfxHLSLShader`, whose `+0x18` = the API shader = our `HalHLSLShader`.
* Our hook (`engine/material_source.*`, symbol `shaderPoolCompile`) attaches the request to the `d3d9::Shader` (`source`) and, with
  `debug.material_sources`, writes JSON lines to `bin/rvk/cache/materials/sources.jsonl`: hash (run-time name), corpus (FNV-1a of
  the bytecode = the corpus file name), stage, profile, flags, inline, file, entry, macros.
* First capture (savegame spot, 40 s hold): 250 shaders (194 ps, 56 vs); entries PS_Default 180, VS_Default 33, VS_DefaultZPass 19,
  PS_DefaultZPass 10, 4 `VS` + 4 `PS` (other effects). Macros seen: XFX_QUALITY, XFX_VS_TEX0.., XFX_PS_DIFFUSE .. SPECULAR /
  SPECULARPOWER / NORMAL / OPACITY / SELFILLUMINATION / DISTORTION / ABSORBATION / CLOUDS, XFX_LIGHTINGTYPE, XFX_COMPRESSEDNORMALMAP,
  XFX_PNTLIGHTCOUNT, XFX_AMBIENTLIGHT(TYPE), XFX_DIRLIGHT, XFX_LIGHTMAP, XFX_MASKED, XFX_ALPHABLEND, XFX_SCENEDEPTH,
  XFX_TANGENTSPACE(_EYE), XFX_FRESNEL, XFX_SKINNED / INSTANCED / MORPHED / ST_* / VEGETATION / BILLBOARD, XFX_ZPASS ... The
  snippets use `tex2D`, `texCUBE`, `saturate`, `lerp`, `pow`, `max`, `min`, `dot`, `mul`, `transpose`, `float3`, `float3x3` (and
  XFX_PS_TEXn, VERTEXCOLOR, TANGENTEYE, SCENEDEPTH, PROJDISTANCE, REFLECT, CAMDISTANCE, VIEWPOS, `g_ShaderData`,
  `g_CamObstacleDistances`).

## 9. The GLSL material shader on the captured set [C]

`python tools/ubershader_check.py out/matsrc/*.jsonl`: per captured pixel shader the translated game shader
`game-shaders/ps30_<corpus>.frag` (`rvk_shadertool export corpus game-shaders`) against `shaders/material/ge_default.frag` with the
captured macros as `-D`, run by `rvk_shadereq` (16 trials, 8 jobs; 14 s for 194 shaders).

* First run: 139 of 194 EQUAL, 19 DIFF, 25 PORT-ERR, 11 UNSUPPORTED.
* Fixes: `XFX_PS_TANGENTEYE` = `.xy` (the parallax uv is `(float2 expr) * TANGENTEYE`: HLSL truncates the float3) cured 23 compile
  errors; `#define pow powD3D` + vec overloads (the snippets call the HLSL `pow` = |x|^y; all 20 DIFFs had `pow(` in a snippet, 6 %
  of the equal ones did) cured the numeric differences; scalar-first `max` / `min` overloads; `g_CamObstacleDistances` =
  `g_CamObstacleAndMaskReference.xy`.
* Final: **183 of 183 supported pixel shaders EQUAL**; the 11 unsupported are other effects: `ge_default_leafs.fx` PS_Default /
  PS_DefaultZPass (7), `ge_particle_2_0.fx` (3), `ge_skydome_2_0.fx` (1). `shader_equiv.py` 31/31 and `--vs` 17/17 still equal.

## 10. The GLSL material shader at run time [C]

* Setting `debug.ubershader` (default **off**), with `debug.material_sources` and `debug.ubershader_skip` (bit mask of classes kept
  translated, for bisecting: 1 z pass, 2 masked, 4 alpha blend, 8 lightmap, 16 normal map, 32 specular, 64 distortion, 128 plain).
* `shader/uber.*` assembles `#version` + the macros as `#define` (true / false -> 1 / 0, one line) + hook defines (`RVK_MV_*`) +
  `rvk_abi` + (with hooks) `rvk_gi` + `rvk_material_variant` + `ge_default.glsl` + `ge_default.frag` (the includes removed: embedded
  text, `rvk_glslc` text). `VariantSlot::Base` = the plain module; the GI / G-buffer / motion variants are built from it too
  (`GlslOptions::uber` / `uberDefines` set by `enh/material_variant`). Hooks in the shader: `g_AmbientLight_c` /
  `g_IndoorAmbientColor` read `rvk_gi[]` under `RVK_MV_AMBIENT`, `rvk_gbN` = the normal-mapped world normal in
  `CustumFragmentAmbient`, `rvkMvPrologue(vNormalAndAO)` at the start of `main`, `rvkMvEpilogue(oC0)` after the alpha test.
* Built in the background (`variantWorker`); SPIR-V cache `bin/rvk/cache/spirv/<hash>_<tag>_u<source hash>.spv`. A shader not
  covered, not built or whose pipeline is not there yet stays translated (same fallback as the other variants; `prepareDraw`:
  `uberBase`, `findPipeline` / `prewarmOnce`). rvk.log `d3d9: GLSL material shaders: N modules built, M failed`.
* Result at the savegame: 150-250 modules built, 0 failed (plain; 500 with gi + ao + native_aa); validation + synchronization
  validation clean (the only warning with FSR on is unrelated: `rw_luma_history` declared Rgba8 but the image is RGBA16F); fps
  unchanged (142.7 vs 142.5).
* Images: refcompare Vulkan runs A, C (off), B, D (on), 20 spots, mean abs difference of frame b0 over the spots: off/off 1.19
  (median 0.95), on/on 1.54 (1.38), off/on 1.44 (1.18): no separate systematic effect. Interior spots (no wind / water noise) equal
  to noise (pub_interior bad32 0.003 %, monastery_hall 0.06 %). The foliage spots jungle (off/on 1.75-2.18 vs off/off 0.88, on/on
  1.54) and swamp_camp (0.91-1.16 vs 0.58 / 0.94) show at most a faint excess (<= ~0.7 of 255) not isolated by the class bisect
  (skip masks 1..128 all 1.6-2.3 against a two-spot control run whose own noise was 1.9).
* **Lesson**: the run-to-run noise of the spots varies between 0.9 and 1.9 (jungle) between run types (full 20-spot runs vs 2-spot
  runs; two runs with identical effective settings, off2 vs skip255, differ by 1.89), so a single noise pair is not a floor: use
  >= 4 runs and compare the off/on pairs with the off/off and on/on pairs. `water-compare.ps1` is no ubershader A/B (it forces
  `water.enabled` on the enhanced side only).

### Open

* Not ported: `ge_default_leafs.fx`, particles, the sky dome pixel shader and all vertex shaders (the VS ubershader exists in
  `shaders/material/ge_default.vert` and is proven by `shader_equiv.py --vs`, but is not wired to the run time).
* The ubershader stays off by default until it brings something (coverage + improvements).
