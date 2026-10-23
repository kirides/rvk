# Ray-traced global illumination (RTGI): research and plan (M10)

Research of 2026-10-23, three strands: our renderer's RT groundwork (code / docs), how Risen lights a surface (Engine.dll RE +
the shipped material HLSL + the shader corpus; dumps `B:\DevDrive\risen-ghidra\gi\`), and a technique survey. [C] = read in
code / measured, [I] = inferred, [P] = published figure, [est] = estimate. Supersedes enhancements-roadmap.md 3.6 in detail.

## 1. How Risen lights a pixel [C]

Ground truth: the material HLSL shipped loose in `data\compiled\effects\shadermaterial\` (`ge_default_ps.fx`,
`ge_default_phong.fx`, `ge_common.fxh`, `ge_default_leafs.fx`, `ge_default_vs.fx`), matched line by line against corpus
bytecode (`ps30_b20d1a1354a09194`, `ps30_00ed2ad09cb6faae`). One shader family (`eCShaderDefault`) for every class:

```
acc = 0
ambient  : acc += lerp(c14, AmbSH(N), AO_v)                         // AO_v = vertex COLOR1.a (TEXCOORD4.w)
lightmap : acc += 4 * lerp(LM2.rgb, LM1.rgb, sat(dot(Nts, LMdir)))  // s4 / s5, uv TEXCOORD7 (vertex TC6)
sun      : acc += c22.rgb * sat(N.L) * sat(4 Ngeo.L) * shadowMask.a  // L = c24 (towards the sun)
points   : acc += col_i * sat(N.L_i) * sat(4 Ngeo.L_i) * (1 - sat(d^2 / R^2)) * (1 - dot(sel_i, shadowMask))   (<= 3 / pass)
rgb = diffuse * pow(acc, 0.45) * shadowMask.r + specular * specMask + selfIllum      // shadowMask.r = engine SSAO
fog : rgb = lerp(rgb, c3.rgb, sat(min(dist * c4.y + c4.x, c4.z)))
```

* **The scene target holds gamma-encoded values** (`pow(acc, 0.45)`), the tonemapper (`post_hdr.fx`: eye adaptation clamped to
  avgLum 0.4-0.8, scalar Reinhard with white point 10, blue shift for darks, colour matrix) works on them directly.
* **Ambient** = `lerp(c14 g_IndoorAmbientColor, SH9(N), AO_v)`. `c15..c21 g_AmbientLight[7]` are real 9-coefficient SH with the
  constants folded in ("stupid SH tricks" layout, world Y up): c15-17 = (L1x, L1y, L1z, L0 - C7) per channel, c18-20 = (C6, C5,
  3 C7, C4) for (xy, yz, zz, zx), c21.rgb = C8 for (x^2 - y^2) (`CalcAmbientLightComplex`; Medium and below: L0 / L1 only).
  Per draw: `SH = hemisphere SH * f + staticSH`, `L0 += (1 - f) * indoor` (`FUN_1806b5bb0`); the hemisphere light
  (`eCHemisphere_PS`: GeneralAmbient, BackLight, GroundLight, SunLight fill, IndoorAmbientColor) is driven every frame by
  `eCWeatherAdmin::Process` 0x710730 from the weather states; `f` = the object's outdoor factor from its portal room's openings
  (`eCIlluminated_PS::UpdateIllumination` 0x595300, eased over 0.3 s; 1 outside rooms); `staticSH` = the static lights
  (`FindWightedStaticLights` 0x5765a0) that did not become per-pixel point lights. Ambient is applied in a draw's **first pass**
  only (later passes add point lights).
* **Lightmaps** (576 of 2330 PS; static level meshes with a TC6 stream): baked from **static artificial lights + radiosity
  bounces, not the sun or sky** [I, strongly supported: no sun / sky pass in the bake]; DXT5 pairs `<name>_1 / _2.dds`, s3 never
  used. Outdoor lightmapped surfaces get sky from the SH and the sun dynamically.
* **Sun** `c22` diffuse x intensity (midday ~ (3.9, 3.3, 1.9)), `c23` specular, `c24` towards the sun; shadow = `shadowMask.a`
  (cascade PCF x terrain variance map). **Point lights** `c25 + 4 i`: pos + 1/R^2, colour, specular, shadow selector; <= 2
  static (strongest weighted) + <= 2 dynamic (`eCLightSet`) per object, 2 passes. No spot lights. **Self-illumination** is added
  after the pow, unlit; there is no emissive term that lights anything.
* **SSAO** exists (`eCImageFilterShadowMask`, `post_SSM.fx` SSAO / blur / upscale) and multiplies the whole lit result
  (direct light included) through `shadowMask.r`.
* Units: a sunlit white surface at noon reaches acc ~ SH + 4; typical albedo 0.3 -> ~0.6 in the target. GI irradiance belongs
  **inside acc, before the pow, in the units of `c22 * N.L`**; the pow makes it sub-linear exactly like the engine's ambient.

## 2. What GI replaces and keeps

* **Replace** the indirect diffuse: the whole ambient term `lerp(c14, SH9(N), AO_v)` (not c14 or the SH alone: the indoor lerp
  and the outdoor factor both act on it); optionally the lightmap term ("replace ambient + lightmaps" mode, judged per scene:
  lightmaps are the only indoor bounce source today); the engine SSAO factor when GI supplies occlusion (it also darkens direct
  light: replacing it shifts the sun's look slightly).
* **Keep** sun, point lights, specular, self-illumination, fog.

## 3. What our renderer has [C]

* **TLAS** of StaticMesh / InstancedMesh / SpeedTree branches + fronds / terrain (enh/rt_scene), rebuilt every frame at
  **AfterOpaque** (after the Solid pass: the material shaders cannot trace it in the same frame -> GI data is one frame late,
  fine for probes with hysteresis). Missing: skinned characters, grass, leaf cards, billboards, decals, water, glass. Interiors
  have **no sun cascades**, so geometry behind the camera is only there through the 20 s registry persistence.
  Limits: 16384 TLAS instances, 30000-unit radius, 48 BLAS builds / frame.
* **Hits** (`rvkRtHitSurface`): albedo (ray-cone mip), world position, interpolated + geometric normal, uv, lightmap uv,
  instance id. Not at hits: emissive, normal maps, specular, lightmap textures, vertex AO. `water_rt.comp` already shades hits
  with the engine's formula (SH9 + c14 + sun with a shadow ray, fog), from constants copied off the water draw (one frame late).
* **No global light data** in the FrameBlock; no G-buffer, no normal buffer, no motion vectors (`prevVsConsts` = 0); the depth
  map holds the z pre-pass subset; the sky cube is A8R8G8B8 128^2 (LDR).
* **Hooks**: `GlslOptions::constOverrides` (register -> GLSL expression, direct and relative reads), `epilogue`,
  `inputOverrides`, per-shader variants through `Device::variantModule` (cached `<hash>_<tag>.spv`, built on first use);
  injection points AfterDepthPrepass / AfterOpaque / AfterRefractionCopy / AfterMainView / BeforeGui; persistent transient images
  for atlases / history.
* **Costs** (RX 7900 XTX): TLAS build 0.12-0.15 ms; a primary ray per pixel at 1600x900 0.7 ms (cut-outs solid) - 1.26 ms
  (alpha-tested foliage doubles it); water RT medium 1.3 ms at 1080p. **GPU frame uncontended ~1 ms at 1600x900** (the
  3.5 ms figures were with another game on the GPU); the game is CPU-bound uncapped, so GPU work up to ~2-4 ms is nearly free.

## 4. Technique choice (survey)

| technique | fit here | why |
|---|---|---|
| **DDGI** probe volumes (Majercik 2019 / 2021) | **tier 1** | world-space cache the material shader samples with its own normal: no G-buffer, motion vectors or screen denoiser; [P] ~1 ms on a 2080 Ti at 1080p incl. BVH update, GIBS 2024: 6k probes x 16+ rays in 0.7 ms (Series X); UE 5.8's "Lumen Lite" is the same idea |
| screen-space AO (visibility bitmask / XeGTAO, MIT) after the z pre-pass | tier 0 + complement | contact AO DDGI lacks; covers characters missing from the TLAS; the non-RT fallback |
| GI-1.0-style screen probes (Boisse 2023, Capsaicin MIT) + hash-grid cache | **ultra** | [P] RX 6900 XT 1080p 1.9-3.1 ms total, tracing only 0.24-0.68 ms; adapt: probes after the z pre-pass, interpolated in the material PS, terminate into DDGI |
| SHaRC | ultra's far-field cache at most | path-termination cache, NVIDIA licence: reimplement the idea (GI-1.0's hash grid is MIT) |
| ReSTIR GI / PT | no | [P] 8.9 ms at 1080p on a 3090, needs G-buffer + strong denoiser |
| Brixelizer GI (MIT, SDF) | no (maybe a no-RT ray backend) | screen composite + G-buffer; Vulkan only in FidelityFX SDK 1.1 |
| surfel GI (GIBS), radiance cascades, Lumen clone | no | complexity / 3D cost |

Licences: RTXGI-DDGI, SHaRC and NRD are under the NVIDIA RTX SDKs licence (no open-source use): implement from the papers;
MIT references: Wicked Engine (DDGI, surfel GI), Capsaicin (GI-1.0), Kajiya (archived), XeGTAO, FidelityFX Denoiser (shadows /
reflections only).

## 5. Design (tier 1)

**Key idea: probes store L2 SH (the engine's ambient format), and GI enters through `constOverrides` of c14..c21.** The
material PS keeps its own maths: per pixel, the 8 surrounding probes' SH are blended (trilinear x DDGI Chebyshev visibility x
backface weight) into one SH9 in the c15..c21 layout, c14 is set to its L0 (so the indoor lerp becomes a no-op; AO_v then only
blends between identical values), and the engine's `CalcAmbientLightComplex` evaluates it with the normal-mapped normal, inside
acc, before the pow. No shader rewrite beyond a variant that replaces those constant reads with a function call (an include
with the probe lookup + the world position from `gl_FragCoord` + depth / the interpolated position); first-pass variants only.

* **Volumes**: camera-centred clipmap cascades (e.g. 32x16x32 probes at 1.5 / 3 / 6 m, scrolling) + later interior volumes
  (portal-room bounds: `eCPortalRoom_PS`, open). Per probe: SH9 irradiance (RGB, 27 floats; or octahedral 8x8 if SH rings),
  octahedral 16x16 mean / mean^2 depth for visibility, state (offset, class: relocation inside geometry by backface ratio,
  sleeping / vigilant), all in persistent transients.
* **Trace** (AfterOpaque, after rt.scene): a budgeted subset of probes per frame (new / near / vigilant first), N spherical-
  Fibonacci rays with a per-frame rotation, ray query with cut-outs solid (foliage: cheap coverage accept, no texture loop).
* **Hit shading in acc units**: outgoing `L = albedo_mean * acc_hit` with `acc_hit` = sun (c22 * N.L * shadow: cascade lookup in
  range, shadow ray beyond) + the frame's point-light list (from the draws' c25+, deduplicated) + the previous frame's GI SH at
  the hit (multi-bounce) [+ the hit's lightmap later]; misses = the **engine's hemisphere SH evaluated in the ray direction**
  (the engine's own sky ambient, HDR-consistent; the LDR sky cube only for detail if ever).
* **Blend**: project rays to SH / depth moments with hysteresis 0.95-0.98 (adaptive on change), lightning flashes excluded.
* **Modes** (settings): replace (pure GI), ratio (`engine ambient x GI / unoccluded GI`: keeps the engine's art direction
  outdoors, the safe default), strength; lightmaps keep | replace; engine SSAO keep | ours.
* **Budget** [est, RX 7900 XTX 1440p]: trace 0.3-1.0 ms (128k-512k rays), blend 0.1-0.3, PS lookup 0.2-0.5, SSAO 0.3-0.6:
  ~1-2.5 ms.

## 6. Phases

* **P0 global light capture**: sun (c22-24), the hemisphere SH (c15-21 of an outdoor draw with f = 1 / or read
  `eCHemisphere_PS` directly), indoor colour, fog (c3 / c4), per-frame point-light list (c25+ deduplicated) into the FrameBlock;
  water_rt switches to it (removes its one-frame lag). Debug view of the captured values.
* **P1 DDGI core**: one camera cascade, trace / blend passes, hit shading, `rt.debug=gi_probes` (probe spheres with their SH),
  `gi.debug=irradiance` (the lookup as the pixel's ambient); costs in `-PerfFrame`.
* **P2 material integration**: the c14..c21 override variant (built in the background like `registerReplacement`), the modes,
  a first-pass-only rule; refcompare at strength 0 (parity) and in-game A/B at the pub / monastery / caves / jungle.
* **P3**: cascades, interior volumes from portal rooms, lightmap / SSAO policies, screen-space AO.
* **P4 ultra**: screen probes after the z pre-pass (short screen march catches characters, then ray query, terminate into
  DDGI), probe-space filtering; optional ReSTIR-style temporal reuse of probe rays.

## 7. Risks / open questions

* Light leaks: single-sided walls / roofs in the harbour town, coplanar duplicates, interiors with geometry missing behind the
  camera (no cascades indoors) -> interior volumes used exclusively by indoor pixels (`f < 1`), backface classification.
* Missing occluders: characters, grass and leaf cards (porous canopies over-light forest floors; the ratio mode bounds it).
* Lightmap double counting / flat interiors without lightmaps at hits; static lights may already be both baked and dynamic
  (open, materials RE 8.1).
* Variant build cost (~2300 PS) and register pressure of the lookup (`--shader-stats`); the draw batching groups by pipeline,
  so variants change the groups.
* Eye adaptation (avgLum clamped 0.4-0.8) reacts to brighter interiors; darks are compressed by the tonemap: judge GI in the
  final image, not the target.
* Open RE: indoor zone / `eCLightSet` assignment, the weather -> ambient mapping, whether the exposure semantic reaches the
  tonemap, terrain lightmaps, lightmap sRGB.
