# Material system: normal maps, specular, detail maps

Research note on how Risen's materials produce the lit surface: which maps exist, how the shaders use them, and what that
means for optional enhancements (exposing the per-pixel normal, specular anti-aliasing, quality overrides). Findings from a
research agent, 2026-10-22.

Legend: **[C]** confirmed (export table, shader source, shader corpus, asset listing), **[I]** inferred or proposed (from
names or from reading HLSL, not verified in the running game). RVAs are Engine.dll. Related: `docs/research/engine-gfx.md`
(HAL, frame, shaders), `docs/research/water.md` (water materials, binormal sign in COLOR0.r),
`docs/research/enhancements-roadmap.md` (normals and roughness outputs).

---

## 1. Engine material classes (Engine.dll exports) [C]

Shader classes: `eCShaderDefault`, `eCShaderLeafs`, `eCShaderSkyDome`, `eCShaderParticle`, `eCShaderLightStreaks`.

`eCShaderDefault` ColorSrc slots and properties:

| Export | RVA |
|---|---|
| `GetColorSrcDiffuse` | 0xad050 |
| `GetColorSrcOpacity` | 0xad060 |
| `GetColorSrcSelfIllumination` | 0xad070 |
| `GetColorSrcSpecular` | 0xad080 |
| `GetColorSrcSpecularPower` | 0xad090 |
| `GetColorSrcNormal` | 0xad0a0 |
| `GetColorSrcDistortion` | 0xad0b0 |
| `GetColorSrcLightEmission` | 0xad0c0 |
| `GetCompressedNormalMap` / `SetCompressedNormalMap` | 0xaced0 / 0xacee0 |
| `EnableSpecular` | 0xacc90 |
| `BRDFLightingType` | 0xacf30 |
| `IsVertexTangentUsed` | 0x6c8630 |

* `BRDFLightingType` is an `eEShaderMaterialBRDFType`: `Phong_Simple`, `Phong_Complex`, `Phong_WrapAround`.
* `eCTexCoordSrcBumpOffset` (`GetPSImplementation` 0x6dbdb0, `GetVSImplementation` 0x6dbf90): a parallax / offset bump texcoord
  source. **[I]** from the name only, not decompiled.
* Other sources: `eCColorSrcSampler`, `eCColorSrcCubeSampler`, `eCTexCoordSrcReflect`.
* `CompressNormalMapToDXT5InPlace` 0x752440 (the engine converts normal maps to DXT5nm).
* `eCShaderMaterialAdmin::SetQuality` 0x6b8ea0 / `GetQuality` 0xa3730 (both 1-3 instruction bodies: never detour). The value
  is the `XFX_QUALITY` shader define, traced in section 5.

## 2. Material HLSL shipped with the game [C]

The game ships its material HLSL source (read only in the install, reachable through the test copy's junction):
`data\compiled\effects\shadermaterial\ge_default.fxh`, `ge_default_phong.fx`, `ge_default_ps.fx`, `ge_default_vs.fx`.

**Normal decode** (`GetNormalMap` in `ge_default.fxh`):

* `n = (tex - 0.5) * 2`.
* If `g_bCompressedNormalMap` (DXT5nm): `x = .w`, `y = .y`, `z = sqrt(1 - x^2 - y^2)`; otherwise `normalize(n.xyz)`.
* Transformed with the tangent frame (`vTangent` = TEXCOORD5, `vBinormal` = TEXCOORD6, `vNormalAndAO` = TEXCOORD4) into
  `m_vecWorldNormal`; `m_vecWorldReflect` and `m_fFresnel` are recomputed from it.

**Consumers of the perturbed normal:**

* sun and point light Lambert;
* Phong specular: `pow(R.L, specPower * 128)`, `ColorSrcSpecular` as a mask, specular power from a texture or a constant;
* SH ambient (`CalcAmbientLightComplex` / `CalcAmbientLightSimple` take the world normal);
* the directional lightmap term: two lightmaps (s4 / s5) carry an ambient light direction, dotted with `m_vecTangentNormal`;
* the distortion offset.

**Sampler layout:** s0 shadow mask, s4 / s5 lightmaps, s9 to s15 material textures.

## 3. Shipped shader corpus [C]

Counts over `corpus\*.bin`: 2483 shaders = 2334 PS + 149 VS. These are **permutation counts, not material counts.**

| Set | Count |
|---|---|
| Material-family PS (TEXCOORD4 input) | 2281 / 2334 |
| of those: decode a material-sampler normal map | 1894 (83 %) |
| of those: DXT5nm / normal-only | 1886 / 8 |
| of those: normal map + directional lightmap | 484 |
| Material-family PS without a normal map | 387 (some are z / shadow-pass variants reading only diffuse alpha) |
| VS declaring `dcl_tangent` | 91 / 149 |
| Material-family VS (TEXCOORD4 to 6 outputs) | 146 / 149 |

* The VS without a tangent are skinned / particle / billboard variants.
* No VS uses a BINORMAL input: the binormal is `cross(N, T)` in the VS, with the sign in COLOR0.r (see `water.md`).

Examples:

* `corpus\ps30_b20d1a1354a09194.bin`: lightmapped, DXT5nm normal on s10, TBN, SH ambient on the perturbed normal, lightmap
  direction . tangent normal; no direct light, no specular.
* `corpus\ps30_847e4be71eac145a.bin`, `ps30_fcc8eaa9fb50ac4f.bin`: water types.
* `corpus\vs30_00d30de5cca9da89.bin`: material VS (normal, tangent, color, color1, texcoord).

## 4. Assets [C]

Listed read only with risen-spass `tools/g3pak.py` (`._ximg` = DDS at ~0x109, risen-spass `docs/RE.md` section 43).

**Textures** (`images.pak`, 1342 textures):

| Kind | Count | Notes |
|---|---|---|
| `_Normal` | ~357 | all DXT5 (= DXT5nm); typically half the diffuse resolution (e.g. `Monastery_Stone_Floor_01` normal 256^2 vs diffuse 512^2) |
| `_Specular` | ~84 | DXT1; character skin / cloth |
| `_Comp` | ~119 | DXT1, packed composite; channel layout unknown |
| `_Mask` | 3 | |
| sky reflect maps | 7 | |

Normal-map coverage by set: Level/ 143 / 180, Animation/ 142 / 161, Speedtree/ 8 / 9, Special/ 25 / 35.

**Materials** (`data\common\materials.pak`, 515 `._xmat`):

| Feature | Materials |
|---|---|
| reference a normal texture | 392 (76 %) |
| a detail normal (`Special_Detail_0x_Normal_01`) | 132 |
| `eCTexCoordSrcBumpOffset` | 67 |
| a cube sampler | 19 |
| a `_Comp` map | 115 |
| a specular map | 51 |
| shader class `eCShaderDefault` / `eCShaderLeafs` / `eCShaderLightStreaks` | 512 / 2 / 1 |

Example `Monastery_Stone_Floor_01_Diffuse_01._xmat`: Diffuse + Normal + Comp + BumpOffset texcoord + detail normal.

**By class** (approximate name grouping):

| Class | Materials | With normal map | Other |
|---|---|---|---|
| characters / monsters / heads (`Ani_*`) | 185 | 163 | 50 specular, 60 detail normal |
| buildings / props (`Arch_*`) | 110 | 102 | 56 `_Comp`, 42 BumpOffset, 37 detail normal |
| nature (`Nat_*`) | 72 | 60 | |
| SpeedTree | 14 | 12 | bark `ST_Bark_*_Normal_01` + Comp; leaf cards use `eCShaderLeafs`; billboards have an "OmniNormal" texture |
| weapons | 10 | 9 | |
| hero armour | 8 | 6 | |
| level monastery / dungeon | 8 / 3 | 7 / 3 | |
| `Special_*` (VFX, decals, shore) | 75 | 22 | |

Terrain: not checked (unknown).

## 5. Quality gating

The HLSL gates are read from source **[I]** (not verified in game); the source of the quality value is traced **[C]**
(re-lookup 2026-10-22; Engine.dll RVAs, image base 0x180000000; dumps `B:\DevDrive\risen-ghidra\shaderq_engine_dump.txt`,
`shaderq2..6_engine_dump.txt`, `shaderq_scriptgame_dump.txt`).

**HLSL gates [I]:**

* Normal mapping is **not** gated by `g_iQuality`.
* Lower quality removes: specular (<= LOW), self-shadow (<= MEDIUM), lightmap specular (LOW), complex SH ambient
  (< HIGH), and swaps the gamma `pow` for `sqrt`.
* `g_iQuality` is a compile-time define **[C]**: `ge_common.fxh:20-28` has `#define XFX_QUALITY_LOW 0 / MEDIUM 1 / HIGH 2`,
  `#ifndef XFX_QUALITY #define XFX_QUALITY 2`, `static int g_iQuality = XFX_QUALITY;`.

**Where the value comes from [C]:**

| Step | Function / file | RVA |
|---|---|---|
| macro list | `eCShaderMaterialAdmin::SetupShaderMacros` appends ("XFX_QUALITY", `"%i"` of `*(uint*)(this+0x350)`) | 0x6b8ac0 |
| init | `eCShaderMaterialAdmin::PostInitialize` writes `this+0x350` from config `/Engine/Render/Shader/Quality` | 0x6ba9d0 |
| live set | `SetQuality`: `*(this+0x350) = q; ReadShaderPool(this)` | 0x6b8ea0 |
| get | `GetQuality` returns `this+0x350` | 0xa3730 |
| config | `eSRenderOptions` (`GetInstance` 0x66c190), created in 0x8b420 from the ctor 0x8b6a0; key pair at +0xe0, value at +0xe8 | |

* Code default is 2 (High), written into config layer 0 when the key is missing.
* Config files: `Risen.exe` main (0x140001290) uses the base name `Config` (string 0x14002504c, override `-config <name>`);
  `eCApplication::LoadConfiguration` 0x4bfa40 loads `ConfigDefault.xml` (layer "global"), `Config.xml`, `ConfigLocal.xml`
  ("local"); `ConfigUser.xml` (`#g3:/appdata/ConfigUser.xml`) via `LoadConfigXML` 0x4c9cf0. `ConfigGenomeDefault.xml` only
  with `-config ConfigGenome`. `ConfigDefaultNew.xml` is never read (no binary references "DefaultNew"; leftover) **[C/I]**.
  `ConfigDefault.xml` `<Shader>` has no `Quality` key, `ConfigUser.xml` has none either: the default 2 (High) applies.
* In-game control: Options -> **Performance** page (`Menu_System`, DLG_MENU_OPTIONS_SYSTEM; "Shader Quality" label at 353,427,
  spin id 1008 at 678,428; `B:\DevDrive\risen-ghidra\guitree2.txt`) **[C]** for the script, **[I]** for the binding. Same page:
  Texture quality, Texture filter, Shadow details, Vegetation details, Effect quality, Range of sight, Depth of field.
  * `OnSpin_Quality_Shader` (`Script_Game.dll` 0x159c60) cycles Low/Medium/High and writes the config (layer 2, probably the
    "override:" layer = `ConfigUser.xml` **[I]**). It does not call `SetQuality`, so it applies on the next launch **[I]**.
  * Console command `CON_ShaderQuality` (`Script_Game.dll` 0x14e230, "low/medium/high") writes the config **and** calls
    `SetQuality` live ("Shader quality is now %s").

**Shipped shaders are the High permutation [C]:** High gamma = `pow(acc, 0.45)`, lower = `sqrt` (`ge_default_phong.fx:229-236`);
2267 of 2334 corpus PS contain 0.45 (bytes `66 66 E6 3E`); of the 67 without it only 14 are material-family and those still
have `pow`/`log` (not the `sqrt` variant). Sample `ps30_002c975b5f451fc5.bin` shows 0.45 + Phong `pow` specular.

**Pool consequences:**

* `ReadShaderPool` 0x6b6ee0 loads `ShaderMaterialPool_Master.smp` + `ShaderMaterialPool_User.smp` from `materials.pak`, no
  quality in the file name **[C]**. The pool is keyed by the "preprocessor id", which includes `XFX_QUALITY` **[I]**, so a
  Low/Medium setting would miss the pool, compile at runtime from the `effects.pak` HLSL and write
  `ShaderMaterialPool_User.smp` **[I]**. Whether Master holds Low/Medium variants is not verified. Master is extracted for
  later parsing at `B:\DevDrive\risen-ghidra\smp_extract` (13 MB); User is 22 bytes (empty).
* **Conclusion:** players get the highest material quality by default and the menu can only lower it. The Vulkan path already
  receives the High shaders (specular, self-shadow, complex SH ambient, `pow` gamma present): nothing to force. A lower setting
  in Vulkan mode would trigger runtime compiles + a pool write (CLAUDE.md hard rule 1 hazard): it needs a redirect /
  read-only guard before anyone tests it.
* Side finding: the code default of `/Engine/Render/Shadows/Quality` is "High", `ConfigDefault.xml` ships "Medium", the test
  copy's `ConfigUser.xml` has "High".

**Other [I]:**

* `data\ini\ConfigPerformanceLow.xml` / `ConfigPerformanceHigh.xml` have no shader or normal-map switch (shadows, vegetation,
  effects, texture filter only). `ConfigDefault.xml`: `CurrentShaderVersion="3_0"`, `EnableMaterialFallbacks`.
* Materials carry `MaxShaderVersion` + `FallbackMaterial`; an SM2 fallback might drop maps (untested).

## 6. Enhancement potential [I]

Suggestions, all untested:

* Plain normal mapping already exists (76 % of materials, 83 % of material PS), so there is nothing to add for it.
* **Expose the perturbed per-pixel normal** plus a gloss proxy (SpecularPower / specular map) as optional outputs of the
  translated material PS (MRT, through the reserved specialisation-constant hooks), for SSR / RT reflections / RT GI
  instead of geometric or depth-reconstructed normals. This supersedes the "not recoverable from bytecode" caveat in
  `enhancements-roadmap.md`.
* Anisotropic filtering on the material samplers (s9 and up).
* Toksvig-style specular anti-aliasing from the normal-map length.
* ~~Force the quality-gated terms (self-shadow, specular, complex SH ambient) as on High.~~ Not needed: the shipped shaders
  already are the High permutation (section 5).
* Parallax only where height data exists: 67 BumpOffset materials, but no height map found; `_Comp` might hold height / AO
  (layout unknown).
* Detail normals for classes that have none (`Special_*` decals / effects).
* Albedo-derived normals are a poor idea.

## 7. Open questions

* Terrain materials (normal map usage unknown).
* `_Comp` channel layout.
* ~~Where `eCShaderMaterialAdmin::SetQuality` gets its value~~: traced (section 5: `/Engine/Render/Shader/Quality`, Options >
  Performance, default High). Still open: whether the HLSL quality gates behave as read (needs a Low/Medium run with a
  guarded pool write), whether `ShaderMaterialPool_Master.smp` holds Low/Medium variants (parse `smp_extract`), and which
  config layer the `OnSpin_Quality_Shader` write lands in.
* `eCTexCoordSrcBumpOffset` implementation (parallax or plain offset; decompile 0x6dbdb0 / 0x6dbf90).
