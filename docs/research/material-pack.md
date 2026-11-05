# Material pack: roughness / ORM / metalness / emissive for the game's materials

> Note (2026-11-05): the settings `materials.default_roughness`, `materials.derive_max`, `materials.derive_min`, `materials.environment`, `materials.specular`, `materials.wetness` named below were removed (removed 2026-11-05, now a constant in code or an always-on behaviour: see `docs/research/settings-audit.md`); read them as the history / the values the code now fixes, not as switches to set.

Status: first version built (2026-11-04). Legend: **[C]** confirmed (code, data or a game run), **[I]** inferred / from other notes, not re-verified.
Code: `src/enh/material_pack.*` (loader, names, manifest, pack word), `shaders/include/rvk_material_variant.hlsli` (raster response, `RVK_MV_PBR`),
`shaders/include/rvk_rt_trace.hlsli` (hit surface), `src/enh/material_variant.*`, `src/d3d9/draw.cpp` (the word per draw), `tools/matpack_make.py`,
sample `package/materialpack_sample/`. Tests: `rvk_tests matpack_*`.

## 1. Feasibility: node chain or sidecar?

### 1.1 What the engine's material graph is [C, rt-albedo-facts.md, materials.md]
A material (`._xmat`, 515 in `materials.pak`) owns an `eCShaderDefault` (512) / `eCShaderLeafs` (2) with eight role proxies (Diffuse, Opacity,
SelfIllumination, Specular, SpecularPower, Normal, Distortion, LightEmission), each pointing to a node tree (sampler, combiner, blend, mask,
constant, vertex colour, cube sampler, tex-coord sources). `eCShaderDefault::GetDeclarations` turns the trees into `XFX_PS_<ROLE>` HLSL
expressions; the pixel shader is the engine's own `ge_default*.fx` compiled with those macros per material x lighting situation, from the
**precompiled pool** `ShaderMaterialPool_Master.smp` (+ `_User.smp`, written on misses). Samplers are `g_SamplerData[k]` = s(9 + k), at most 7.

### 1.2 What the engine's lighting already has for gloss [C, ge_default_phong.fx]
* Phong specular only: `pow(saturate(dot(R, L)), SpecularPower * 128)` (`PhongSpecular`), added as `colAccSpecular * SpecularMask` AFTER the
  gamma step, per light (sun, <= 3 point lights, plus a lightmap-direction term). `XFX_SPECULAR` (material flag `EnableSpecular`) and quality > Low
  switch it on; the Complex BRDF multiplies by `Lamb / Fresnel` (a grazing-angle *boost*, not a Fresnel term).
* Inputs: the `Specular` role = a colour mask (51 materials have a specular map, mostly `_Specular_S1` DXT1 of skin / cloth), the `SpecularPower`
  role = texture or constant. No roughness, no metalness, no energy conservation, no environment reflection except a Diffuse that is a **cube
  sampler** (wet beach / rock layers, 19 materials), no glass, SelfIllumination is added unlit after the gamma step.
* So the pack complements the engine: unlisted materials keep the engine's Phong exactly; listed ones get a GGX response added on top (and
  less diffuse for metals). The engine's own spec of those materials stays (it is usually 0 or small); `materials.specular` scales ours.

### 1.3 Route A: the engine's node chain (a mod ships changed `._xmat` files)
Possible in data [I]: resources are found by bare compiled name, loose files in mounted dirs override (spass.md 3.6); an xmat could get a further
`eCColorSrcSampler` (the graph is a property stream), and slots are handed out densely. Why it is not the route:
1. **No role for the data.** The graph can only feed the eight roles; `SpecularPower` could carry a roughness (inverted), but there is no metalness,
   no reflection and no glass role. New behaviour needs new HLSL in the engine's `ge_default*.fx` (loose-file override of `effects` is conceivable)
   and the whole pool recompiled.
2. **The pool is closed per permutation.** A changed graph changes the macros -> a pool miss -> runtime compile with D3DX + a write to
   `ShaderMaterialPool_User.smp` in the player's install (materials.md 5; hard rule 1 forbids it in our test copy). A pack author would have to ship
   precompiled permutations or every player pays compile stalls.
3. Every changed material must be a full xmat copy (property stream editing, no merge): packs conflict with each other and with mods that also edit materials.
4. We replace the engine's shaders anyway (translated bytecode); an added sampler would only be one more `s(9+k)` that we would need to recognise by name.
Verdict: **not feasible as a pack format for the community; not needed.** (Not re-verified with Ghidra in this task: the loose xmat override is
from spass notes, the pool behaviour from materials.md; I did not decompile further.)

### 1.4 Route B: sidecar, keyed by name [chosen, built]
At a draw we know the material (`eCSubMesh +0x38`, `draws::Tag::material`) and, through the material graph (`engine/materials`), the pool image of
its Diffuse sampler [C: every Diffuse sampler is k = 0 = s9, rt-albedo-facts.md 1.4]. Both have names: the material resource title (`Arch_Wood_Planks_01_Diffuse_01`,
`ItMisc_01_Diffuse_01_Glass`) and the diffuse image title. The pack maps those names to maps / constants. No permutation changes, works for every
material that has any name, needs no engine data to be rewritten, and packs merge (folders in order; later wins).
**Both routes were considered; route B only.** If a node-chain route is ever wanted, what it needs: new HLSL roles + a precompiled pool; our renderer
could then read the extra sampler through the same pool (`poolImageName`).

## 2. Pack format (spec)

A pack = a folder (several allowed: `<Risen>\bin\rvk\materialpack\` and every folder in the environment variable `RVK_MATPACK`, `;`-separated, later ones override earlier ones; sub-folders are scanned).

| file | meaning |
|---|---|
| `<Name>_orm.dds` / `.png` | RGBA map over the diffuse texture's uv: R occlusion, G roughness, B metalness, A emissive mask (the diffuse colour is added where A > 0, needs `emissive > 0` in the manifest) |
| `<Name>_r.dds` / `.png` (`_rough`, `_roughness`) | roughness in R (single channel; PNG is read as 8 bit, stored L8) |
| `pack.toml` / `manifest.toml` | optional constants per material / wildcard |

`<Name>` = the engine's name, case-insensitive, without the extension: the **material name** (tried first: `ItMisc_01_Diffuse_01_Glass`) or the **diffuse image
title** (`Arch_Stone_Bricks_01_Diffuse_01`; both visible in rvk.log `rt: material '<name>' (...)` lines with `-RvkLogLevel` info after the RT scene
is on, or `-Environment 'RVK_TRACE_MATERIAL=<part>'`; `materials.debug=coverage` shows which draws matched). `._ximg` / `.dds` / `.png` extensions in manifest keys are stripped.
Formats: DDS (any D3D9 format the renderer supports: BC1/3/5 DXT, A8R8G8B8, L8, ...; own mip chain, no cube / volume) or PNG (8 bit; mips are generated on load,
RGBA ORM -> A8R8G8B8, roughness -> L8). The map is sampled with the diffuse's sampler and uv, so it may have any resolution (a quarter of the
diffuse is fine). Linear data, not sRGB. Missing map = constants only (or the default roughness, `materials.default_roughness`).

```toml
[defaults]                       # constants of every entry
roughness_scale = 1.0
[material."ItMisc_01_Diffuse_01_Glass"]   # exact name or wildcard ('*' any run, '?' one char), later rules override earlier ones
roughness = 0.04                 # constant roughness (entries WITHOUT a map only: wildcards must not flatten a map)
metalness = 0.0                  # ORM entry: multiplies the map's B; otherwise the constant
roughness_scale = 1.0            # map roughness = saturate(R_or_G * scale + bias)
roughness_bias = 0.0
emissive = 1.0                   # > 0: the ORM alpha channel is an emissive mask
map = "wood.dds"                 # explicit map file (relative to the manifest) instead of the naming convention
wetness = 0.5                    # 0..1 (2 bit): smoother (x 1 - 0.7 w) and darker (x 1 - 0.35 w), scaled by the player setting materials.wetness
transmission = 1.0               # > 0 marks GLASS: the reflection is not capped by the surface's own brightness (the engine's alpha blend stays: no refraction)
ior = 1.5                        # parsed, kept for a later refractive response; not used yet
```
Quantisation: scale 4 bit (0..1.875, step 1/8), bias 4 bit (0..1, step 1/15), metalness 4 bit (1/15), wetness 2 bit (0, 1/3, 2/3, 1); the pack word (below) is the only thing the shaders see.

### The pack word [C] (`enh::matpack::encodeWord`, mirrored in the HLSL)
32 bit: bits 0-13 bindless 2D slot of the map (0 = constants only), 14 present, 15 ORM layout, 16-19 roughness scale (/8, 0..1.875), 20-23 roughness bias (/15),
24-27 metalness (/15), 28-29 wetness (/3), 30 glass (transmission > 0), 31 emissive. Roughness = saturate(tex * scale + bias) with tex = G (ORM) / R (roughness map) / 1 (no map); metalness = B * m (ORM) / m;
occlusion = R (ORM) else 1; emissive = A (ORM + flag). **Word 0 = no entry = unchanged pixels.**

## 3. How it is built

* **Loader** (`MaterialPack`): a worker thread scans the folders, parses manifests, decodes DDS / PNG into `d3d9::Texture`s (bindless slots, uploaded lazily by
  the render thread the first time a draw uses them), budget `materials.budget` (MiB, default 256: further maps are skipped with a warning). Lookup =
  `lookupMaterial(material)` (cached per material pointer and pack generation) or `lookup(name)` (normalised key, exact entry, then the manifest's rules).
  rvk.log: `enh: materials: pack loaded: N files, M entries (K failed), R rules, X MiB, T ms`, every 5 s `enh: materials: N draws looked up, M matched`,
  `d3d9: material pack: first draw with a map applied (word ..)`. Hot reload: `MaterialPack::configure` again (not wired to a command yet).
* **Raster (headline)**: the pixel shader variant machinery (`enh/material_variant`): with the pack on at start (`materials.pack`, restart; and the folder has files)
  every Gi / G-buffer variant also contains `rvkMvPbr` (`RVK_MV_PBR`), and the device sends the lit main-view material draws through the variant slot even
  without GI (`Device::setPackProvider`). The draw's DrawBlock `pad[1]` carries the word; the translator captures the diffuse `texld` (coordinate + colour of s9,
  `TranslateOptions::texCaptures`). `rvkMvPbr`, in the epilogue, only for word != 0: undoes the engine's fog, reads the map at the diffuse coordinate, then
  `lit * (1 - metalness) + GGX(sun, specular colour, shadow mask alpha) + GGX(<= 3 point lights, medium and up) + sky cube reflection (roughness-blurred
  mips, split-sum Fresnel, indoor factor = the vertex AO) * occlusion + diffuse * emissive`, F0 = lerp(0.04, diffuse, metalness), re-applies the fog. The specular sum
  goes through the engine's 0.45 gamma like its diffuse accumulator. Positions come from the depth buffer (as the GI variant), normals from the captured
  normal-mapped operand of the ambient SH dot (as the G-buffer). Frame params[2] = quality / specular / environment / debug (`kFrameParamMaterials`).
  Settings: `materials.pack`, `materials.quality` (off / low = sun + sky / medium (default) = + point lights / high = reserved for traced reflections),
  `materials.specular`, `materials.environment`, `materials.default_roughness`, `materials.budget`, `materials.debug` (roughness / metalness / orm / coverage).
  Works in wrapper and full mode (the variant is a pixel shader variant of the D3D9 model's draws) on both backends (HLSL, no Vulkan-only types).
* **RT consumers** (small shared-file edits): `RtGeometryRecord::packWord` (the former pad word) and `RtMaterialSegment::packWord`; `RtScene::materialFields` looks the
  material's name up (the map must already be uploaded: a raster draw of the material does it); `RvkRtHitSurface` gets `roughness`, `metalness`, `occlusion`,
  `emissive`, `packWord` (default without an entry: roughness 1, metalness 0, occlusion 1); debug `rt.debug=roughness|metalness` (dark magenta = no entry).
  `RtMaterial` interning now includes the name (two materials of one texture stay apart).

## 3b. Later additions (2026-11-04)
* **G-buffer material channel** (no new attachment, no format cost): the normal target's B channel (A2B10G10R10, it was always 0) holds, for draws with a pack entry,
  `512 | metalness (4 bit) << 5 | roughness (5 bit)` as a 10 bit unorm; 0 = no entry (`rvkGbufferMaterialEncode / Decode` in `shaders/include/rvk_oct.hlsli`, written by
  the G-buffer variants' epilogue, `rvk_pbrGb`). Consumers read `rvkGbufferMaterialDecode(normalTexel.z)` -> (roughness, metalness, has entry). 5 / 4 bit steps: coarse but enough
  for lobe selection and reflection blur; occlusion / emissive are not stored (the raster pass already applied them). Debug view `gbuffer.debug=material` (red roughness, green metalness, dark blue none).
  Only exists while the G-buffer is armed (`ao.enabled`, `gi.screen`, `upscale.mode`, `gbuffer.debug`) and the pack is on.
* **Hot reload**: command `materials reload` (`-Commands '900: materials reload'`, overlay command line) re-reads all pack folders; the new snapshot replaces the old at the next lookup.
* **Authoring log**: with `-RvkLogLevel debug` every material is logged once: `enh: materials: seen '<material>' (diffuse image '<image>'): pack entry|no entry`.
* **Packet cache** (`native.packet_cache`): draws that take the pack variant are not replayed (`Device::replayAllowed` refuses a lit main-view material draw while a pack is on).
* Not built (design only): SSR / RT reflections for smooth surfaces (`materials.quality=high`), glass (`transmission` / `ior` need a word extension or side table and an alpha-blended response:
  the pack word has no free bits left), lazy texture loading / freeing unused maps.

## 3c. Verification and cost (2026-11-04, RX 7900 XTX, 1600x900)
* **Corpus**: `rvk_shadertool corpus corpus --pbr --gi|--gbuf|--gbuf-motion`: 2246 / 2321 / 2321 variants OK, 0 failed (DXC + spirv-val); `dxil-check corpus corpus --pbr --gi|--gbuf`: all columns equal (signed DXIL SM 6.6, push constants 112 B).
* **testdemo 7/7 with `-Validation`**: Vulkan wrapper + full, D3D12 wrapper + full: no VUID / `d3d12: validation` / `[error]`. (D3D12: the first version read the shadow mask slot of shaders that do not declare it and the engine's sky cube while it was still in render-target layout: GPU-based validation "Incompatible texture barrier layout"; fixed: the mask only when the shader samples it itself, the cube is transitioned in `prepareTextures` while a pack is on.)
* **No entry = unchanged pixels**: refcompare against `out/refcompare/20261103-130010` with a pack folder present that matches nothing (variants active): Vulkan 2 FAIL (sea_cliff, harbour_mouth: the known-noise spots), 6 WARN, 12 PASS; D3D12 1 FAIL (sea_cliff), 6 WARN, 13 PASS; harbour_docks and every interior PASS on both. A pixel diff of two runs is limited by the engine's run-to-run noise (flickering lights, splash particles): `scripts/matpack-nomatch.ps1` runs arms off / off2 / on-no-match; the on-no-match arm is as close to `off` as `off2` is, apart from particles. An exact proof needs an in-run toggle (not built).
* **GPU cost** (`scripts/matpack-perf.ps1`, harbour_docks hold, sample pack, about 55 % of the lookups matched, 3 interleaved runs per arm, 24 frames each, default lock + copy, RisenTest.exe, vsync off): GPU main submission 1.79 / 1.93 / 1.76 ms (off) vs 1.97 / 1.74 / 1.75 ms (on): no difference beyond the +-0.1 ms noise. CPU: uncapped fps of the hold 236 / 284 / 300 (off) vs 272 / 294 / 286 (on): no measurable cost (the lookup is a pointer hash per lit material draw).
* **Look** (shot-reviewer + own block statistics; pictures `out/matpack/final`): the first version doubled the night plaza and washed out the docks' cobbles. Fix: what the pack adds is capped to 0.2 (rough) .. 0.8 (smooth) of the engine's lit brightness of that surface (metals exempt; the first floor of 0.01 dominated at night), the sky reflection to 0.35 of it, scaled by the lit level. Final worst 200x200 blocks (display luma): night plaza +32 %, monastery +12 %, pub floor +31 % (roughness 0.22), docks pier close to the engine's (splash particles differ between runs).

## 3d. Later additions, part 2 (2026-11-04)
* **Exact "no entry = unchanged" proof**: debug setting `materials.alternate = N` bypasses the pack variants (the engine's own pixel shaders draw) on every second block of N frames (`Device::setPackBypass`, decided at each frame's `BeforeDepthPrepass`). `scripts/matpack-alternate.ps1`: ONE run of rvkcompare with a pack folder that matches nothing, `burst=4 interval=10 materials.alternate=10`: b0 / b2 = variants, b1 / b3 = engine shaders. The engine animates some pixels between frames even with the game time frozen (flames, water, foliage), so only the pixels that are identical across both same-state pairs count (the animated region, dilated by 24 px for glow, is excluded; the pairs b0-b2 / b1-b3 cannot be influenced by the variants). Result on `pub_interior, monastery_hall, harbour_docks, harbour_night, crystal_cave, hero_closeup` (+ jungle on Vulkan): **0 differing pixels** on Vulkan wrapper, D3D12 wrapper and Vulkan full; crystal_cave and hero_closeup are static (the whole frame, 1.44 M pixels each, identical); the animated spots keep 20 - 32 % of their pixels as static.
* **Maps are decoded lazily**: the index (names, constants) is built on the worker at start; a map's texture is created when a draw first matches the entry (render thread, one small decode), so unused maps cost nothing (`enh: materials: map of '<key>' loaded`); a map that fails to load leaves the entry's constants.
* **Wetness** (pack word, 2 bit, manifest `wetness`, player setting `materials.wetness` 0..1, frame params[3].x): the engine does not expose rain / weather wetness to the renderer in a form I used, so wetness is a constant per material scaled by the setting (default 1 = as authored).
* **Glass** (manifest `transmission > 0` -> pack word bit 30): the sky reflection is not capped by the surface's own lit brightness (like metals); glass keeps the engine's alpha blending (windows and bottles are blended materials): no refraction / transmission response. A refractive response needs the refraction copy (`scene HDR` copy of the opaque scene exists only in the Refracted pass), per-material ior and the blend state: not built.
* **Not built**: reflections beyond the sky cube (SSR needs the opaque colour while the solid pass is still drawing it; RT reflections in the material pixel shader would need the ray-query code (rvk_rt_trace.hlsli) in every variant: the TLAS of the frame exists from the z pre-pass on, so it is feasible, but it adds ray-query code and register pressure to ~2300 shader variants x 3 and was not attempted), occlusion + flags in the G-buffer (no free bits in the normal target).
* **Player package**: `scripts/package.ps1` ships `rvk/materialpack_sample/` (pack.toml + 5 maps + README.txt, NOT enabled; the README explains: copy to `rvk/materialpack`; `package check` fails if `rvk/materialpack/` would ship), `package/rvk/README.txt` has a short paragraph.

* **Richer sample pack + final look** (`package/materialpack_sample`, shot-reviewer + block statistics; pictures `out/water/20261104-153802`, `-154750`): wood (planks maps + wildcards), stone (bricks / cobble maps, monastery floor 0.25), plaster, rusty metal, weapons, glass (`*_Glass*`, `*Window*`, `*Bottle*`: transmission flag), barrels / shipstuff, wet river ground, moss, mud, gravel, rock, bark. Findings: pub, monastery, jungle: subtle (+6 .. +12 % in the worst blocks), no artefacts; harbour night plaza +25 % (a cool damp sheen, plausible); lake_jungle bridge planks in sun were +33 % with the first cap (clipped pixels doubled): cap retuned to 0.15 (rough) .. 0.55 (smooth), `Arch_Wood_*` roughness 0.5; harbour docks: no visible wet look (wetness 0.33 / 0.66 steps are subtle by design). No white-out, black materials or seams in any spot.
* **Final gates of this round** (build of the final shader text): `rvk_tests` 224/224; corpus `--pbr --gi` 2246 OK / 0 failed; testdemo with `-Validation` on Vulkan wrapper / full and D3D12 wrapper / full: 0 hits; `materials.alternate` proof: 0 differing pixels on Vulkan wrapper, D3D12 wrapper, Vulkan full (measured before the last light-cap constants, which only change the pack branch that a word-0 draw never runs); `scripts/package.ps1`: `package check: ok (21 files)` incl. `rvk/materialpack_sample/`.

## 4. What is NOT done (limits)
* G-buffer: roughness / metalness in the normal target's spare channel (3b); occlusion / emissive / flags are not stored (an RGBA8 attachment would cost +4 B/pixel and a new attachment in the armed-scope pipelines).
* Raster: the water RT / SSR reflections and the RT scene are not mixed into the pack's reflection (sky cube only); no glass transparency (`transmission` / `ior` are
  parsed only); the sun shadow factor is the engine's mask alpha (no pack-specific shadows); alpha-blended passes use the depth buffer's position (wrong for
  blended materials); the ubershader path (`debug.ubershader`, default off) does not capture the diffuse (its pack data = grey albedo, no map).
* A material needs a lit (ambient) pixel shader with a DirLight or point lights; unlit / particle / sky shaders are never touched.
* The pack texture memory is not freed until exit; RT-only consumption needs a raster draw to upload the map first.
* `materials.pack_path` is the environment variable `RVK_MATPACK` plus `bin\rvk\materialpack` (the settings registry has no string type).

## 5. Authoring
1. Find names: run with `-Setting 'materials.debug=coverage'` (green = matched), `-RvkLogLevel debug` and read `rt: material '...'` lines (every material x pixel shader in view, once the RT scene is on:
   `-Setting 'rt.debug=albedo'`), or list `images.pak` with risen-spass `tools/g3pak.py list data\compiled\images.pak` (names = `<Name>._ximg`).
2. Make maps: `python -I tools/matpack_make.py <Name>._ximg <out dir> --preset wood|stone|cobble|metal|rust` writes `<Name>_orm.png` from the diffuse's luma (a starting point; paint your own).
3. Put the folder in `bin\rvk\materialpack\` (or `RVK_MATPACK`), restart. Debug: `materials.debug=roughness|metalness|orm`, `rt.debug=roughness`.
4. Test: `.\scripts\water-compare.ps1 -Spots pub_interior,monastery_hall,harbour_docks -EngineSetting 'water.enabled=true','materials.pack=false' -Setting 'materials.pack=true'` (left engine, right pack).

## 6. Gloss derived from the engine's specular data (materials.derive_from_specular)
Draws WITHOUT a pack entry whose engine material is glossy get a roughness derived from the engine's own data, so every consumer of the pack word (raster variant, G-buffer
material channel -> traced material reflections, RT hit surface) covers them. Lookup order: **pack entry > derived from the Specular / SpecularPower roles > the consumers' own default**
(`materials.default_roughness` for a power TEXTURE; surfaces with no specular data keep word 0 = exactly the old behaviour).
* **Data read** (`engine/materials`, `ShaderInfo::specular / specularPower / enableSpecular`): `EnableSpecular` = bool at eCShaderDefault +0x2c0 [C, GetDeclarations 0x6c8650 emits `XFX_SPECULAR` from it]; Specular proxy +0x370, SpecularPower proxy +0x3a0; a role whose
  root element is an `eCColorSrcConstant` exposes its rgba at element +0x50..+0x5c [C: C-materials.md 3.4; the rgba ORDER is [I]]; the proxy swizzle (+0x28) index 0 none, 1 .rgb, 2 .r, 3 .g, 4 .b, 5 .a picks the scalar of a constant power (0 / 1 / 2 = r).
  No SpecularPower role = the engine's default `m_fSpecularPower = 1` (ge_default_ps.fx 271).
* **Derivation** (`matpack::deriveFromSpecular`, pure, `matpack_derive_*` tests): n = SpecularPower * 128 (PhongSpecular) -> GGX alpha = sqrt(2 / (n + 2)) -> roughness = sqrt(alpha) (the pack's convention: the shaders use `a = r * r`); clamped to
  `[materials.derive_min 0.25, materials.derive_max 0.9]`; the specular mask is the strength: a constant colour = its luminance, a map = `kMapMaskStrength` 0.8 (the per-pixel mask is NOT read: that would need a second texld capture in ~2300 x 3 variants);
  a weak mask pushes the roughness towards 1 (`r + (1 - r)(1 - strength)`): too rough to mirror anything, which is how "reflection strength" reaches all consumers through ONE number. A black constant mask, no Specular role or `EnableSpecular` off = no derived entry.
  A power TEXTURE = `default_roughness` stands in. Metalness is never derived (F0 stays 0.04: **no F0 tint** from the specular colour, only its luminance).
* **Word**: `kWordDerived` = bit 15 with slot 0 (an ORM layout needs a map, so the combination was free), roughness 8 bit in bits 16-23, all other fields 0. Provider-side: `Hit::derived`, counter `derived` in the 5 s line
  (`enh: materials: N draws looked up, M matched ..., K derived from the specular data`); `-RvkLogLevel debug`: `enh: materials: derived '<material>': specular map|const L, power P -> roughness R` and `seen ...: derived from the specular data`.
* **Raster**: a derived draw keeps the engine's own Phong highlights (no direct GGX on top: that would double them); it gets the sky-cube reflection (existing caps: <= 0.15 .. 0.55 of the surface's lit brightness, `materials.environment`) and the G-buffer channel (`rvk_pbrGb`) that the traced reflections read.
  `materials.debug=coverage`: green = pack entry, **orange = derived**. RT scene: `RtMaterial::spec` (part of the interning key) -> `RtScene::materialFields` -> `MaterialPack::deriveWord`.
* **Off / exact state**: `materials.derive_from_specular` off, or no pack folder (no variants exist): word 0 as before. Settings: `materials.derive_from_specular` (default on), `materials.derive_min`, `materials.derive_max` (Advanced). Changing them bumps the pack generation (raster re-looks-up at once; RT records refresh when their materials are re-seen).
* **Measured** (2026-11-05, Vulkan wrapper, materials.quality=high, a dummy pack folder so the variants exist, derive off vs on, water off, `water-compare` hero_closeup / pub_interior / harbour_docks / monastery_hall): 21-22 k of ~60-70 k draws per 5 s get a derived word (stone, wood, bark, herbs, floors: most have a Specular MAP and a constant SpecularPower of 0.1 .. 0.9 -> roughness 0.5 .. 0.7 after the strength fold; `NonAni_Hero_*` bodies too). Mean luma on/off: pub 1.013, hero 1.003, monastery 1.001, docks (sky and splash excluded) 0.974 = splash particles / flicker noise; p99 |diff| 8 .. 34 (candle / particle noise dominates), no mirror-like cloth or stone, no white-out. The effect on screen is subtle by design (sky reflection capped at <= 0.15 of the lit brightness on rough surfaces); the larger consumer is the G-buffer channel for traced reflections (`materials.rt_reflections` skips surfaces rougher than its reach). Skinned actors have no material tag (`tag.material` null): their word comes from the executing shader (`MaterialPack::lookupMaterial(nullptr)` -> `byShader_`). `rvkcompare -Param focus=1` shows the engine's own green focus highlight, not our coverage tint.
* **Gates**: `rvk_tests` 236/236, `rvk_d3d9test --validation` Vulkan + D3D12 0 errors, `rvk_rhitest` PASS, `dxil-check embedded` / corpus `--pbr --gi` (2246) / `--pbr --gbuf` OK, `testdemo` 7/7 with `-Validation` on Vulkan and D3D12 (`RVK_NO_GBV=1`; only the known DSV-format warning 1477).
* **Not built**: per-pixel specular-map mask / power map (a second texld capture in every variant), F0 tint from the specular colour.

### 6.1 Follow-up (2026-11-05): corrections, gloss gate, proofs
* **Correction of the data read**: the constant's value is NOT at element +0x50 (that read garbage: powers of 1e22); it lives in the shader's PS register pool: element +0x68 = the pool (== shader +0xc8), +0x70 = the register, pool first register shader +0xcc, count +0xf1, data pointer +0xe0 (`engine/materials.cpp fillRole`, the same pool `native/static_ops.cpp poolsOf` mirrors). Real values: SpecularPower 0.1 .. 0.9 (n = 13 .. 115).
* **Gloss gate** (`matpack::glossGate`, `kGlossStart` 0.35, `kGlossSpan` 0.3): the literal conversion of those exponents gives roughness 0.5 .. 0.7 for nearly every stone / wood / bark material (almost all have a Specular map): a faint sky veil at F0 0.04 plus a traced ray per pixel (the traced limit is 0.7) on most of the screen for no visible gain, and a change of the engine look. The derived roughness therefore runs to 1 between 0.35 and 0.65 (nothing added, not traced); only genuinely glossy surfaces (power >= ~0.5: n >= 64, e.g. glass, polished stone) keep a reflection. The weak-mask fold comes after the gate.
* **Cube diffuse is skipped** (the wet beach / rock `*_Diffuse_01_Specular` layers, diffuse = cube sampler, `enableSpecular` true or false): the layer IS the engine's own environment reflection drawn blended over the ground; a second sky reflection would double it. `-RvkLogLevel debug` `seen` lines of "no entry" now print the class, the roles' sources, `enableSpecular`, the constant values.
* **RVK_MATPACK via `run.ps1 -Environment` was ignored**: `std::getenv` reads the CRT's startup snapshot, `[debug] environment` sets the variable with `SetEnvironmentVariableW` from DllMain. The pack folders now read the Win32 environment (`GetEnvironmentVariableA`). **Consequence: `matpack-alternate.ps1` / `matpack-nomatch.ps1` (which pass the no-match folder this way) ran WITHOUT variants before this fix** (no "materials: pack on" line): their "0 differing pixels" were vacuous. Re-run with the fix (rvk.log shows `enh: materials: pack on`).
* **Exact-off proof** (`scripts/matpack-alternate.ps1`, the no-match pack so the variants exist, derive OFF, ONE run: variants vs the engine's shaders, pixels identical across both same-state pairs): **Vulkan wrapper 0, Vulkan full 0, D3D12 wrapper 3 pixels of 1 level (crystal_cave; flicker level, not reproducible as a systematic difference)** of 0.3 .. 1.4 M static pixels per spot over pub_interior, monastery_hall, harbour_docks, harbour_night, crystal_cave, jungle, hero_closeup. No pack folder: no variants and no `MaterialPack` exist, the draw path is the old one. The script now prints the magnitude of the differences when there are some.
* **`matpack-nomatch.ps1`** (pack off vs on-no-match, derive off, now with variants): off vs off2 (noise floor) 11.19 M differing pixels over 15 pictures (mean 0.55 .. 1.07 levels per picture), off vs no-match 11.10 M (mean 0.46 .. 4.6; harbour_docks 4.6 in both arms' noise class: splash particles): the no-match arm is as close to `off` as `off2` is. Cross-run pixel diffs only bound the noise; the in-run alternate proof above is the exact one.
* **Cost** (`scripts/derive-perf.ps1`, harbour_docks hold, RisenTest.exe, vsync off, Full mode, no-match pack in both arms, 3 interleaved runs per arm, 9 five-second windows each, materials.quality=medium; GPU = main submission over 24 frames): Vulkan Full GPU 1.775 ms (off) vs 1.771 ms (on); CPU engine+HAL / frame at the hold 1.85 .. 1.95 ms (off) vs 1.85 .. 1.98 ms (on), uncapped interval p50 2.19 .. 2.26 ms vs 2.19 .. 2.27 ms. D3D12 Full: GPU 1.916 vs 1.912 ms, hold CPU 1.98 .. 2.02 vs 1.98 .. 2.00 ms, p50 2.16 .. 2.19 vs 2.16 .. 2.19 ms. No measurable cost on either backend (the lookup is a pointer hash per lit material draw, cached per material / shader; the derived pixels add only the sky term).
* **Derive ON, same in-run A/B** (Vulkan wrapper, with the gate): crystal_cave 2.0 % of the static pixels differ (max 3 levels, mean luma x1.0005), harbour_docks 2.4 % (max 7, x1.0016), monastery_hall 0.05 % (max 1), jungle 2 pixels, harbour_night / hero_closeup / pub_interior 0. The engine look is kept.
* **Traced reflections A/B** (materials.quality=high, `materials.rain_force=0.8` so the derived surfaces are wet; derive off vs on; `out/water/20261105-174353`): in the rain the derived surfaces (stone, wood, ground) get the wet look (smoother, a little darker, glints): harbour pier cobbles and the cliff darker with a damp sheen (mean luma x0.989), beach ground and rocks darker with sky glints (x0.984, hero_closeup x0.979), pub interior and monastery nearly unchanged (indoors: no roof rays reach the sky: x0.994 / x1.003). No mirror-like surfaces, no flicker seen in the stills.
