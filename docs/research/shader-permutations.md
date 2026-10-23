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
  macro'd GLSL is an equivalent rewrite, never byte-exact. The shipped HLSL is the exact source: port `ge_default*.fx` to GLSL
  macros and verify each variant against the corpus with `shader/sm_eval` (CPU evaluation) instead of text comparison.
* Per-material snippets are not in the corpus (they come from the `ShaderMaterialPool` build, materials.md 5): to list them,
  group shaders by `mat_samplers` + `mat_fetch_order` in `macros.csv` and diff within a group.
