# Material pack: roughness / ORM / metalness / emissive for the game's materials

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
ior = 1.5   transmission = 1.0   wetness = 0.0     # parsed, kept for glass / wet / path tracing consumers; NOT used by the raster shader yet
```
Quantisation: scale 5 bit (0..1.94, step 1/16), bias 5 bit (0..1), metalness 5 bit (1/31); the pack word (below) is the only thing the shaders see.

### The pack word [C] (`enh::matpack::encodeWord`, mirrored in the HLSL)
32 bit: bits 0-13 bindless 2D slot of the map (0 = constants only), 14 present, 15 ORM layout, 16-20 roughness scale (/16), 21-25 roughness bias (/31),
26-30 metalness (/31), 31 emissive. Roughness = saturate(tex * scale + bias) with tex = G (ORM) / R (roughness map) / 1 (no map); metalness = B * m (ORM) / m;
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
