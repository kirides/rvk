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

## 6. Phases (order decided 2026-10-23)

**Stage A: DDGI and working ray-traced lighting, characters included.**

* **P0 global light capture**: sun (c22-24), the hemisphere SH (c15-21 of an outdoor draw with f = 1 / or read
  `eCHemisphere_PS` directly), indoor colour, fog (c3 / c4), per-frame point-light list (c25+ deduplicated) into the FrameBlock;
  water_rt switches to it (removes its one-frame lag). Debug view of the captured values.
* **P1 DDGI core**: one camera cascade, trace / blend passes, hit shading, `rt.debug=gi_probes` (probe spheres with their SH),
  `gi.debug=irradiance` (the lookup as the pixel's ambient); costs in `-PerfFrame`.
* **P2 material integration**: the c14..c21 override variant (built in the background like `registerReplacement`), the modes,
  a first-pass-only rule; refcompare at strength 0 (parity) and in-game A/B at the pub / monastery / caves / jungle.
* **P3 characters in the RT scene**: skinned draws (hero, NPCs, monsters) as per-frame BLASes from GPU-skinned vertices
  (a compute pass running the draw's skinning with its bone constants, or a VS-capture variant writing positions), refit /
  rebuild budget per frame; so characters occlude and bounce GI (and appear in the water's RT reflections).
* **P4**: DDGI cascades, interior volumes from portal rooms, lightmap / SSAO policies.

**Stage B: frame data (also needed for FSR 3 / 4 upscaling and GTAO).**

* **P5 thin G-buffer**: extra outputs of the existing translated material PS (the reserved `kSpecNormalOutput` specialisation
  hook): octahedral world normal (RG16 / RGB10A2) + material / dynamic id; never a separate geometry pass (the game is
  CPU-bound). Depth: the hardware depth of the main view, not only the z pre-pass map.
* **P6 motion vectors**: camera reprojection from depth for the static world; per-object motion for characters and moving
  objects through VS variants (previous vs current clip position: `prevVsConsts`, a per-draw history of the vertex constants;
  injection.md 4). Consumers: FSR 3 / 4, TAA, GTAO / SSAO temporal accumulation, GI temporal reuse.
* **P7 GTAO / screen-space AO** on the G-buffer (XeGTAO or visibility bitmask, MIT), applied to the ambient / GI term only.

**Stage C: advanced GI on top of Stage B.**

* **P8 screen probes** (GI-1.0 style): probes per 8x8 tile after the z pre-pass, short screen march then ray query,
  terminating into DDGI; probe-space filtering; interpolated in the material PS.
* **Later / optional**: a denoiser of our own (SVGF / A-SVGF style), ReSTIR GI with a world radiance cache, materials at hits
  through RT pipelines (section 7), path tracing as an experiment.

## 7. Beyond ray queries: what RT pipelines and other techniques would add (2026-10-23)

The ray API is not the bottleneck: what limits the options is the frame data (G-buffer, motion vectors, a denoiser) and
complete geometry. Ray query was chosen as the floor; this GPU / driver also offer `VK_KHR_ray_tracing_pipeline` (caps report:
"exists, information only").

* **RT pipelines add**: per-material hit shaders, i.e. the real material at a hit (normal map, lightmap, self-illumination,
  alpha) instead of the mean albedo, by turning translated engine shaders into closest-hit / any-hit shaders; ray reordering
  (large wins on NVIDIA, about none on RDNA3); recursion / callables for multi-bounce paths.

| option | needs | cost / fit |
|---|---|---|
| GI-1.0 screen probes + hash-grid world cache (AMD, MIT) | depth, normals, camera motion | [P] 2-3 ms at 1080p on RDNA2; ray query is enough; Stage C |
| ReSTIR GI + world cache + denoiser ("RTXGI 2" style) | full G-buffer, per-object motion vectors, strong denoiser | [P] 8+ ms at 1080p on a 3090 for ReSTIR GI: too heavy on RDNA3 now |
| full path tracing (ReSTIR PT) | the above + materials at hits + emissives | 15-30+ ms; needs upscaling + a neural denoiser (DLSS Ray Reconstruction is NVIDIA-only; AMD's counterpart is not available in Vulkan) |
| surfel GI (EA GIBS) | G-buffer normals, surfel grid | [P] 2.5-3 ms on consoles; very complex |
| Lumen-style (screen -> world trace -> surface cache) | mesh cards / SDFs, surface cache | [P] 4 ms at 1080p on consoles; too big |
| neural radiance cache (NVIDIA NRC) | cooperative matrix (`VK_KHR_cooperative_matrix`, RDNA3 has it) | research grade |
| no RT hardware: Brixelizer GI (MIT, SDF), voxel cone tracing, radiance cascades (screen space only in practice) | | Brixelizer ~2 ms on a 7900 XTX: a fallback for GPUs without RT |

Prerequisites for anything beyond DDGI / screen probes, in the order of Stage A-C: characters in the RT scene (P3), the thin
G-buffer (P5), motion vectors (P6), a denoiser of our own (NRD's licence does not fit an MIT project), materials at hits (RT
pipelines). Path tracing proper is a multi-milestone project whose GPU cost is several times today's frame.

## 8. Risks / open questions

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

## 9. Stage A first light: built (2026-10-23)

([C] = measured / read in code.)

**Code** [C]: `src/enh/gi.{h,cpp}` (a `d3d9::DrawObserver` + a pass `gi` at AfterOpaque, order 10, after `rt.scene`),
`shaders/gi/gi_trace.comp`, `gi_blend.comp`, `gi_common.glsl`, `shaders/include/rvk_gi.glsl` (self-contained; embedded as text via
`rvk_glslc text` and pasted into the GI variants with `GlslOptions::prelude`; mirrors `enh::GiVolumeHeader`, 304 bytes).

**P0, light from the main view's solid draws** [C]: outdoor ambient = the most frequent `g_AmbientLight` set of the frame (kept
from earlier frames when it covers < 30 % of the ambient draws), indoor colour c14, sun = the most frequent non-zero `g_DirLight`,
up to 32 deduplicated point lights (`g_PointLights`, nearest the camera). Measured: noon beach sky L0 (0.85, 0.83, 0.75), sun
(3.93, 3.27, 1.94).

**P1, probes** [C]: 3 cascades of 32 x 16 x 32 probes, 150 / 300 / 600 units apart, toroidal scrolling; each probe stores its
absolute cell as a key, lookups skip probes whose key differs. Per frame a budget (`gi.quality` low 1024 / medium 2048 / high 4096
probes; shares 50 / 30 / 20 % per cascade): scrolled-in cells first, then round robin. 64 spherical-Fibonacci rays per probe,
randomly rotated per frame, cut-outs solid. Hits: albedo^2.2 x (sun x N.L x shadow ray + one point light picked by its unshadowed
light with a shadow ray + the probes' own lookup at the hit = multi-bounce); misses = the engine's outdoor ambient as radiance
(L1 x 3/2, L2 x 4: an all-sky probe returns the engine's ambient). Back faces (interpolated vertex normal facing away; not for
alpha-tested / SpeedTree / characters) store 0 and -0.2 t; > 25 % back faces = probe inside, never used. Blend: SH projection
4 pi / 64, band factors 1, 2/3, 1/4 (acc units = irradiance / pi), stored directly in the engine's 7-register layout (c15..c21),
hysteresis 0.97 (depth 0.95) with a 1/(n+1) ramp; depth moments 8 x 8 octahedral (fp32, distances in cells capped at 4, cosine^10
lobe). Memory 34 MiB.

**P2, material variants** [C]: the material pixel shaders' GI variant (`d3d9::VariantSlot::Gi`, tag `gi3`): `constOverrides` of
`g_AmbientLight` + `g_IndoorAmbientColor` -> `rvk_gi[]` filled in a prologue (world position from `gl_FragCoord` + FrameBlock
`invViewProj`, geometric normal from derivatives, DDGI weights: trilinear x backface x Chebyshev, normal bias 0.1 / view bias 0.3
cells, 2-cell cascade blend band); c14 := the GI average. Only main-view Solid / Alpha / AlphaLate draws with colour output,
non-water, the engine's ambient non-zero and not ONE/ONE blending (`Device::giEligible`). Variants translate + compile on 3
background threads (SPIR-V cache), pipelines through `PipelineCache::prewarmOnce`; the draw uses the engine's shaders until both
exist. `gi.lightmaps = keep` (default): the draw's own ambient beyond the outdoor sky stays on top: engine = f x sky + static with
static >= 0, f bounded by min over channels of L0(draw) / L0(sky), remainder = draw - f x sky (0 for outdoor draws); `replace`:
lightmap samplers read black and no remainder. The first version without the remainder made the pub nearly black (static lights
are not point lights the probes know).

**P3, characters in the RT scene** [C]: skinned main-view solid draws: the vertex shader's capture variant
(`VariantSlot::Capture`, needs `vertexPipelineStoresAndAtomics`) writes world position + TEXCOORD0 (32 bytes per index-buffer
value, 16-byte header with the slot count) into the frame's ring; `RtScene` builds / refits one BLAS per character draw
(PREFER_FAST_BUILD | ALLOW_UPDATE, full rebuild every 30 refits), identity transform, instance mask `kRtMaskDynamic` (rays can
skip them: `RVK_RT_TRACE_NO_DYNAMIC`; `gi.characters` off = GI rays skip them), records without normals, the draw's material.
Measured: 9 characters in the TLAS at `hero_closeup`, ~120 builds + ~3400 refits per 5 s.

**Debug** [C]: `gi.debug=irradiance` (the lookup on a white surface, geometric normal), `cascades` (red / green / blue, dark red =
no probes). rvk.log `enh: gi:` lines every 5 s (probes per frame, pending cells, GI draws/s, light), `rt: scene: characters ...`.
Offline check: `rvk_shadertool corpus corpus --gi` (2246 PS pass) / `--capture` (149 VS pass).

**Results** [C] (water-compare runs `out\water\20261023-145539` / `-150204` / `-151353`, rvkcompare spots): no errors / failed
variants; outdoor spots close to the engine with soft extra occlusion (jungle canopy, eaves, hero); monastery luma +4; pub +24
(brighter than the engine with `keep`: tuning open); crystal cave darker (-14); debug views smooth, no leaks visible at these
spots. Cost: ~0.4 ms GPU at 1920x1080 on an RX 7900 XTX (harbour hold: 3.56 ms with GI vs 3.15 ms without), 2047 probes x 64 rays
per frame at medium.

**Open**: interior volumes (portal rooms), true hemisphere SH from the engine (`eCHemisphere_PS`) instead of the most frequent
draw set, interior brightness tuning (the `keep` remainder over-brightens some rooms), SSAO policy, `water_rt` switching to the
captured light, lightmaps at hits, per-pass GPU timing via `-PerfFrame`.

## 10. Stage B frame data: built (2026-10-23)

([C] = measured / read in code.)

**Code** [C]: `src/enh/gbuffer.{h,cpp}` (passes `gbuffer.targets` at AfterDepthPrepass, `gbuffer.ao` at AfterOpaque order 20,
`gbuffer.debug` at BeforeGui; also a `d3d9::DrawReplacer` for the game's SSAO), `src/enh/material_variant.{h,cpp}` +
`shaders/include/rvk_material_variant.glsl` (embedded as text; the shared Gi / Gbuffer / GbufferMotion pixel shader variants,
replacing the GI variant code of section 9), `shaders/gbuffer/{gtao.comp, gtao_filter.comp, gbuffer_debug.frag, ao_white.frag,
gbuffer_common.glsl}`. Device: `Device::setGbuffer` / `gbufferArmed` / `setFrameAo`, `VariantSlot::Gbuffer` / `GbufferMotion`.
Translator: `GlslOptions::dotCaptures`, `Reflection::pixelInputs`.

**P5, G-buffer** [C]: extra colour attachments 1 and 2 of the first main view's solid pass when it renders into the scene HDR
target alone at the G-buffer's size. Normal = A2B10G10R10_UNORM (xy octahedral world normal via `rvkGiOctEncode`, z free, w =
id / 3: 1 static material, 2 character, 0 none); motion = R16G16_SFLOAT (previous uv - this uv); 8 bytes per pixel. Cleared at the
frame's first scope (loadOp CLEAR). Written by the Gbuffer variants of the material pixel shaders (`Device::gbufferEligible`: main
view solid, depth tested, not ONE/ONE additive, not sky / particles / precipitation / water, a first pass when lit); every other
draw in the scope keeps them (write mask 0 with EDS3, in the pipeline otherwise). The normal: the operand the shader dots with
`g_AmbientLight[0]` (`GlslOptions::dotCaptures`: normal-mapped, world space), else the interpolated TEXCOORD4 normal, else the
geometric one. Arming: the device prewarms the 3-attachment pipeline of every solid draw (`PipelineCache::prewarmOnce`) and switches
the attachments on once a frame's solid pass found all pipelines + variants (measured: "g-buffer armed: 301 solid draws, 0
missing"; with validation it took the 10 s timeout, 72 missing, then built at their draws).

**P6, motion** [C]: material pixels write the camera's motion of their surface point; characters their own: the capture vertex
shader variant (cap3) reads the previous frame's capture buffer of the same draw (header y/z = its address) and outputs the
previous world position at varying location 15 (TEXCOORD15; location 30 exceeded maxVertexOutputComponents 128:
VUID-RuntimeSpirv-Location-06272), read by the GbufferMotion pixel variant; needs the RT scene active (GI or RT reflections).
Pixels without a material (id 0: sky, effects) get the camera's motion from the hardware depth in the AO pass (motion is a storage
image there: needs shaderStorageImageExtendedFormats, else AO is unavailable). No TAA jitter yet.

**P7, GTAO** [C]: XeGTAO-style horizon search, 2 slices x 8 steps per side, radius `ao.radius` (default 1 m = 100 units, falloff
60 %), per-pixel interleaved gradient noise rotated per frame; normal from the depth (nearest neighbour per axis): the
normal-mapped G-buffer normal self-occluded (speckles on floors / terrain in the first test). Depth-aware 3x3 filter + temporal
accumulation (history 0.85, motion reprojection, clamped to the 3x3 raw range +- 0.1) into a ping-pong history (R32F). The
material shaders' Gbuffer variants reproject last frame's AO (`prevViewProj`) and scale all ambient registers (c14..c21, so the GI
too) by mix(1, ao, `ao.strength`). `ao.mode = replace` (default): the game's SSAO (`post_SSM.fx`, the shadow mask's red channel,
multiplies the whole lit result incl. the sun) is switched off by replacing its draws into the half-size A8R8G8B8 targets of the
ShadowMask pass (SSAO + 2 blurs, 800x450 at 1600x900) with a fullscreen write of 1 (log "the game's SSAO draws replaced (800x450
target)"); `add` = both. Draw replacers now hand a draw on when they leave it to the engine (enh/shadows handles the same pass).

**Settings**: `ao.enabled` (default off, Lighting), `ao.mode` replace|add, `ao.strength`, `ao.radius` (m); `gbuffer.debug` =
normals|motion|ao (Debug). rvk.log `d3d9: g-buffer requested / armed`, `enh: gbuffer:`. Offline: `rvk_shadertool corpus corpus
--gbuf | --gbuf-motion`.

**Results** [C] (`out\water\20261023-161012` AO vs off, `-161202` debug views): smooth contact AO at barrels / stools / beams /
corners / clothing; normal-map detail visible in the normals view; sky excluded; validation run with GI + AO + water: no messages.
Luma vs AO off: beach -7, harbour -4, hero -6, jungle +3, pub +20 (the pub's GI keeps its brightness).

**Open**: a faint arc pattern near the pub's right wall (stepping / temporal), strong contact shade under thin overlay meshes
(beach seaweed patches), no depth history for the temporal rejection, no TAA jitter / FSR yet, moving non-skinned objects have
camera-only motion, AO costs not measured separately.

## 11. Stage C screen probes: built (2026-10-23)

([C] = measured / read in code.)

**Code** [C]: `shaders/gi/sp_trace.comp`, `shaders/gi/sp_resolve.comp`, `shaders/gi/screen_common.glsl`, `shaders/gi/gi_shade.glsl` (the
hit shading, shared with `gi_trace.comp`); `src/enh/gi.cpp` `Gi::recordScreen` (after the DDGI update in the `gi` pass at AfterOpaque;
needs this frame's G-buffer, RT scene and hardware depth); `FrameBlock::sgi` (976 bytes). Setting `gi.screen` off|low|medium|high
(default medium, Advanced; needs RayQuery; turns the G-buffer on); debug `gbuffer.debug=screen_gi`.

**Method** [C] (after GI-1.0, Boisse et al. 2023, adapted): one probe per tile at a per-frame jittered pixel; tiles / rays per probe: low
16x16 / 8, medium 16x16 / 16, high 8x8 / 16. Cosine-weighted hemisphere rays around the G-buffer normal, one ray per invocation (64 per
workgroup), shaded by `gi_shade.glsl` like the DDGI probe rays (sun + one point light with shadow rays, the DDGI at hits, sky on misses).
Probe rays are 40 m (4000 units); a ray that runs out takes the DDGI's light at its end (`rvkGiLookupFast` evaluated towards -dir).
The probe stores the LUMINANCE ratio traced irradiance / DDGI irradiance at its point and normal (cap 2; neutral where the DDGI is
< 2e-3). `sp_resolve`: per pixel the 2x2 nearest probes, weighted bilinear x tangent-plane distance x normal similarity, then temporal
accumulation (0.95, motion reprojection, clamp to the probes' range +- 0.25) into a ping-pong RGBA16F image. The material shaders'
Gbuffer variants scale their DDGI registers by last frame's factor at the reprojected position.

**Cost** [C] (a player's quicksave `Risen_Save_0007`: harbour town, detail trees / objects 4x, view 2x, GI high, 1600x900, RX 7900 XTX).
First version (a thread per probe looping over 16 rays, 8x8 tiles, 80 m rays, full DDGI lookup + 200 m sun rays at hits): pass `gi` 29.5 ms
of 32 ms GPU, 28 fps; screen probes off: GPU 4.7 ms (96 fps, CPU-bound); GI off 1.86 ms. After the fix (one ray per invocation,
`rvkGiLookupFast` at hits = trilinear without visibility, 40 m rays, 120 m sun shadow rays, 16x16 tiles at low / medium): medium GPU
7-8.5 ms total frame (~80-90 fps, CPU-bound at ~11 ms). The measurements had the player's own game running on the same GPU (the high /
low windows are contaminated).

**Fixes in the same commit** [C]: the reprojected screen-probe factor and the AO clamp their lookup to the previous screen's edge (a hard
band appeared at the screen edge when turning: the strip new on screen had no correction); the DDGI "inside" flag has hysteresis on a
smoothed back-face share (key.w bits 16-23); variant SPIR-V cache tags = base + hash of the embedded GLSL (`materialVariantTag`).

**Flicker investigation** [C] (12 consecutive captures of a still view at the quicksave, luma std mean): D3D9 1.2, Vulkan GI off + game
shadow map 2.0, GI off + forced 4096 map 3.0, GI on 4.6 -> 3.7 after the hysteresis. The shadows moving near the big house are the game's
palms in the wind (present in D3D9); detail 4x and the 4096 map make more / sharper moving shadows; GI adds faint grain in shaded areas.

**Open**: the DDGI's own multi-bounce now uses the cheap lookup (possible leaks through walls at the second bounce); one-frame lag of AO /
screen probes (solved in section 12); remaining GI grain;
the `gi.screen` high cost over time (unverified, measurement contaminated); the per-pixel ratio is luminance only (no colour bleeding
from near bounces).

## 12. Follow-ups after stage C (2026-10-23)

([C] = measured / read in code.)

**Stable screen probe filter (015bfda)** [C]: the `sp_resolve` 3 x 3 probes are weighted by a Gaussian of the distance to each TILE
CENTRE (sigma one tile) x tangent-plane x normal weights. Weighting by the probe's own (per-frame jittered) pixel made the filter flicker.
Grain (luma std mean, 12 frames of a still view at the player's quicksave): 2x2 3.72, 3x3 jittered weights 4.29, 3x3 stable 2.33, screen
probes off 3.13, GI off 2.98; a later run of the same build family gave 3.42 (the run-to-run spread from the palms' wind motion is large:
treat +-1 as noise). Tried and reverted: per-channel ratios (colour noise, 3.7 -> 4.3) and removing the engine's indoor fill from the kept
remainder (the pub's barrels went black: there the fill is the only light).

**Same-frame AO and screen probes (ebff985)** [C]: the RT scene builds its TLAS at AfterDepthPrepass (AfterOpaque only for frames without a
z pre-pass); characters come from the previous frame's vertex captures (`Device::vertexCapturedPrev`). Screen probes (own pass `gi.screen`
at AfterDepthPrepass, using the last DDGI update's light / volume) and GTAO (AfterDepthPrepass, history RG16F ao + view depth) run on this
frame's depth with depth normals and camera-only temporal reprojection. The material shaders' Gbuffer variants `texelFetch` them at their
own pixel where the stored view depth matches theirs (2 %, >= 4 units): no reprojection lag, ghosting or screen-edge band. Draws not in the
z pre-pass (characters, alpha) keep the uncorrected DDGI and no AO. The G-buffer motion fill stays at AfterOpaque (`gtao.comp` mode 2).
Player quicksave (harbour town, detail 4x, 1600x900): 95.8 fps, GPU ~8.3 ms, CPU-bound ~10.4 ms; validation clean.

**Open**: characters / alpha get no same-frame AO or screen probe correction (could fall back to a reprojected previous result); the GTAO
arc pattern next to the pub wall persists; characters lag one frame in the RT scene.

## 13. Interior brightness: a minimum ambient (2026-10-24)

([C] = measured in the game.)

**Finding** [C]: the old "pub +24 luma, cave -14" numbers no longer hold. Whole-image luma (the water-compare table's `px` mask
varies between runs and is not usable indoors; compare the full frame, letterbox cropped) of GI on vs engine at the rvkcompare spots
was already within +-6 (pub -5.2, monastery +1.9, cave -5.7, jungle -1.9, beach -3.0, harbour +0.2). What was wrong is local: the pub's
barrels, tables and floor patches in sun shadow went **black** (`gi.strength=0` and screen probes off do not change it: it is the
DDGI plus the `keep` remainder). Cause [C] (a `gi.debug=irradiance` view with f / L0 written out): in an interior the engine's
hemisphere is dim (the zone's weather: pub sky L0 ~0.12), so the sky-derived factor `f = L0(engine) / L0(sky)` is ~1 and the `keep`
remainder `eng - sky * f` is ~0; the probes only get sun patches through the windows, so shadowed objects had no ambient at all
while the engine lights them with its indoor fill (dynamic objects have no lightmap).

**Change** [C] (`shaders/include/rvk_material_variant.glsl`, `rvkMvGi`): a minimum ambient. The average (L0 of the final registers)
is lifted by a constant on c15.w / c16.w / c17.w to `min(L0(engine), RVK_MV_FLOOR_MAX) * RVK_MV_INDOOR_FLOOR` (0.15 and 0.85) where
it is lower. The cap leaves bright (outdoor) engine ambient alone, so canopy / contact shade is untouched; only deep darks are lifted.
No setting (the two defines can be overridden by the C++ prelude). A first version scaled by the sky-derived indoor factor
`1 - f` did nothing for exactly the reason above.

| spot (whole-image luma, GI - engine) | before | after |
|---|---|---|
| pub_interior | -5.2 | -5.1 (barrels / table now visible, as the engine) |
| crystal_cave | -5.7 | +1.7 |
| monastery_hall | +1.9 | +1.2 |
| jungle | -1.9 | -1.0 |
| beach_sun | -3.0 | -2.1 |
| harbour_docks | +0.2 | +0.3 |

Run `out\water\20261024-130906` (before: `-125229`), 1600x900, GI on at the player's defaults (screen probes medium). Validation run (pub +
cave): no messages; `rvk_shadertool corpus corpus --gi / --gbuf / --gbuf-motion`: 0 failures; rvk_tests 131 ok.

**Open**: the true hemisphere SH from the engine would give a reliable sky (and `f`) indoors; with it the remainder could keep exactly
the engine's non-sky ambient instead of this cap.
