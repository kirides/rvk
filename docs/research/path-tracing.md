# Path tracing vs DDGI: measuring the probes against ground truth (2026-11-04)

> Note (2026-11-05): the settings `gi.characters`, `gi.emissive`, `gi.lights`, `gi.shadow_fill` named below were removed (removed 2026-11-05, now a constant in code or an always-on behaviour: see `docs/research/settings-audit.md`); read them as the history / the values the code now fixes, not as switches to set.

Question: would full path tracing give better fidelity than the DDGI probes + screen probes (replace mode, `gi.lights=world`)?
Step one (this document): measure how far DDGI is from ground truth, and try the first piece of a per-pixel traced GI. No vendor SDK work.

## 1. The reference path tracer (`gi.reference`)

* Code: `shaders/gi/gi_reference.comp.hlsl` (HLSL SM 6.6, DXIL parity checked: `rvk_shadertool dxil-check embedded` 58 of 58), `src/enh/gi_reference.cpp`
  (accumulation, dumps, view pass), hooks in `src/enh/gi.cpp` (`recordScreen` calls it right after the screen probes). Debug settings (Debug tab):
  `gi.reference = off | reference | ddgi | screen | error | reference_indirect | screen_indirect | error_ddgi` (the in-game view), `gi.reference_spp`
  (samples per pixel and frame, default 4), `gi.reference_max` (default 1024: accumulation stops, planes are dumped), `gi.reference_bounces` (6).
  Off = nothing exists (no pipeline, buffer or pass).
* Method. Per pixel of the main view the primary hit comes from the z pre-pass depth (position, depth-derived geometric normal: the same as the
  screen probes). Albedo at the primary hit: a camera ray traced against the RT scene (same material path as GI hits), once per epoch.
  Indirect = cosine-weighted paths, every hit lit like the engine's materials (GI hit shading, `gi_shade.hlsli`: albedo^2.2 x (sun x N.L with a
  shadow ray + one point light chosen by its unshadowed contribution with a shadow ray) + `gi.emissive` self-illumination), the path continues
  with throughput x albedo (cosine sampling), Russian roulette from the third bounce, up to `gi.reference_bounces`, the sky (the engine's hemisphere
  SH, `engine/hemisphere`) on a miss, no ray reach limit (1 km). Direct = the same sun + point light estimators at the primary hit. Units are the
  engine's "acc" (image = albedo x pow(acc, 0.45)); the indirect value = mean radiance of cosine rays = what `rvkGiEvalRegs` returns for the probes.
* Same inputs as DDGI: the same TLAS / records / materials, `gi.lights=world` lights, sun, sky SH, emissive gain; the only differences are the
  method's (no probe cache: the second bounce on is traced; no reach, no firefly cap, no filter). Not compared: the engine's shadow maps (direct
  light in both columns is the traced one), `gi.shadow_fill` / indoor floors of the material shader (these lift the engine's final ambient above the
  raw probe value; the comparison is of the probes' own irradiance: volume lookup, and volume x the screen probes' factor as the material shader applies
  it, the directional correction does not change the value at the geometric normal).
* Accumulation: restarts when the view, sun, sky SH, the lights, the RT scene's instance count or the settings change; stops at `gi.reference_max`.
  Dumps `bin\rvk\gi_ref\e<epoch>_full_<spp>.npy` (float16, planes ref indirect (+ luma^2 for the variance), ref direct, DDGI volume, DDGI + screen
  probes, albedo + view depth, normal, pixel-GI 1 spp, pixel-GI 4 spp) and luminance-only dumps at 64, 128, ... spp (convergence test), `e<epoch>_meta.json`.
* Cost: 1600x900 on the RX 7900 XTX: one reference sample per pixel is ~36 ms of GPU time (whole-frame GPU 77 ms at 1 spp vs 362 ms at 9 spp, includes
  the 4 pixel-GI samples); a 1000-sample image takes 125 frames at 8 spp/frame at 1280x720 (about 5 minutes per spot with warm-up).

## 2. Tooling

* `scripts/gi-reference.ps1`: three `rvkcompare` runs (gi off / DDGI at `gi.quality=high`, replace, world lights / reference), then
  `tools/gi_reference.py analyze <run dir>`. Run in a private copy under its own lock:
  `.\scripts\with-test-lock.ps1 -Name rvk-pathtrace -Who pathtrace -Run { .\scripts\gi-reference.ps1 -GameDir B:\DevDrive\rvk-work\copies\risen-test-vk-pt -Instance 6 -Spp 8 -Max 1024 }`.
  `-RunDir` re-analyses; the dump of a spot is found by its camera position (the RVKCMP lines).
* Output in `out\gi-reference\<stamp>\analysis`: `sbs_<spot>.png` (reference | DDGI volume | DDGI + screen / indirect-only images / error heat map
  / pixel-GI rows), `err_volume_*`, `crop_*` (the three worst 96 px tiles, 3x: reference | DDGI | error), `triptych_*` (the game's own gi off | DDGI | reference view),
  `summary.md`, `results.json` (mean abs error, bias, relative errors, over/under shares, chroma error, per-class shares of the squared error, convergence).
* Runs: full set `out\gi-reference\20261104-121754` (8 spots, 1024 spp), pixel-GI set `out\gi-reference\20261104-124510` (5 spots).

## 3. How converged is the reference?

Standard error of the final mean from the stored second moment, per spot (median / p90 relative): beach_sun 1.9 / 3.2 %, jungle 3.2 / 4.7 %, harbour_night 3.3 / 5.2 %,
pub_interior 3.8 / 5.1 %, monastery_hall 5.5 / 9.1 %, crystal_cave 1.0 / 1.6 %, lake_jungle 3.8 / 7.2 %, shadow_inland_a 2.2 / 4.9 %. RMS difference to the final image at
64 / 128 / 256 / 512 spp (jungle): 13.8 / 9.4 / 6.2 / 3.6 %, i.e. the 1/sqrt(n) law; the final is ~3 % per pixel noisy. The DDGI errors below (mean 5 - 25 %, bias
-16..+90 %) are larger than the reference noise except in the quietest spots, and noise averages out in the bias and in the class tables. Monastery hall (dark interior, 61 % of pixels over 5 %
s.e.) needs more samples before its per-pixel numbers are trustworthy; its bias (+89 %) is not noise.

## 4. DDGI error (indirect irradiance, 1280x720, 1024 spp; "screen" = probes + screen probes, "volume" = probes alone)

| spot | ref mean (acc) | screen bias | screen mean rel. err | volume bias | volume mean rel. err | lit-image rel. err |
|---|---|---|---|---|---|---|
| beach_sun | 0.570 | -16.5 % | 25 % | +17.9 % | 33 % | 9.3 % |
| jungle | 0.152 | +2.8 % | 13 % | +20.2 % | 34 % | 4.7 % |
| harbour_night | 0.108 | +9.0 % | 14 % | +34.0 % | 44 % | 5.8 % |
| pub_interior | 0.012 | -4.1 % | 5 % | -8.9 % | 14 % | 6.1 % |
| monastery_hall | 0.060 | +89 % | 106 % | +523 % | 645 % | 18.5 % |
| crystal_cave | 0.006 | +7.1 % | 4 % | +285 % | 88 % | 4.1 % |
| lake_jungle | 0.343 | -2.5 % | 18 % | +9.2 % | 30 % | 5.6 % |
| shadow_inland_a | 0.450 | +0.6 % | 22 % | +21.8 % | 46 % | 10.9 % |

(lit-image = error in the image albedo x pow(direct + indirect, 0.45), which is what is seen; the chroma error of the screen probes is 0.02 - 0.08 in rgb/luminance units.)
The screen probes roughly halve the volume's error everywhere (probes alone: 30 - 45 % mean relative error outdoors). With them the outdoor error is 13 - 25 % per pixel,
unbiased within a few % (the beach: -16 %), and the visible (pow 0.45) image error is 4 - 11 %. Indoors where the ambient is dark (pub, crystal cave) both
are within a few % in absolute terms; monastery_hall is the outlier (see 1.).

### Ranked failure modes (images: the paths above; `summary.md` has the class table)

1. **Indoor over-brightening / sky leak (monastery_hall: screen +89 %, volume +523 %; crystal_cave volume +285 %).** The probes return several times the traced irradiance in
   a stone hall (82 % of pixels > +50 %, `sbs_monastery_hall.png`). The pixel-GI prototype (probes only as the second-bounce cache) shows the same +70 - 80 % bias, so the
   cause sits in the probe cache: the probes see more sky than the reference path (probe depth / inside-geometry classification, far-field / ambient
   fallbacks, the 4 km reach with the far-field lookup). Biggest single error and the most visible (the interior is lit like open air); a fix is probable
   in the probes (visibility, ambient fallback), not in the method.
2. **Contact shadows, creases, rock bases and object undersides** (depth-edge pixels: +14.6 % bias, 38 % mean relative error, 14 % pixels over +50 %; ceilings/undersides +11 %, 38 %):
   DDGI too bright where a surface meets another (`sbs_shadow_inland_a.png`, red at rock bases / planks; `crop_*`). The 1.5 m probe grid cannot see occlusion below its spacing; the screen probes
   (16 px tiles) fix the broad part, not the sub-tile creases.
3. **Canopy and foliage** (jungle, lake_jungle, shadow_inland_a, harbour_night): errors of both signs in alternating patches under trees (the traced shade is
   patchy, the probes smooth it), canopy tops / sky-facing leaves DDGI too dark (blue, -33 % under-estimates on 5 - 6 % of pixels), leaf cards need `gi.foliage`.
4. **Bright sunlit bounce** (beach_sun: bias -16.5 %, "bright indirect > 0.3" class -39.5 % bias, 45 % of those pixels under -33 %): the light bounced from the sunlit sand / rock into
   shade and the sea surface is under-represented (probe hysteresis and the screen-probe ratio cap of 2); the sea mesh pixels read 0 in the volume (blue band in `sbs_beach_sun.png`).
5. **Coloured bounce / bleeding** (class "coloured light": 74 % of the squared error in the all-spot aggregate, mean 27 %): hue is right on average (chroma error 2 - 8 %), magnitude in coloured areas is what is off.
6. **Dark indirect (ref < 0.06)**: relative errors are large (+80 % bias, 129 % mean) but absolute errors tiny (0.001 - 0.002) and the engine's indoor floor lifts these areas anyway.
7. **Temporal response to a moving light**: not measured here (the reference needs a still scene). The probes converge with hysteresis 0.98 (about 100 frames); `rvk_lightturn` measures the light counts, an offline moving-light reference would need
   per-frame reference rendering (1000 spp per frame): out of scope.

## 5. Per-pixel traced GI prototype (`gi.mode=pixel`: measured, not shipped)

Not a switchable renderer path yet: the measurement lives in the reference pass (planes 6 and 7: k = 1 and 4 cosine rays per pixel from the G-buffer hit, each shaded with
`giShadeRay`: sun + a point light with shadow rays + the DDGI probes for the second bounce on, far field when the ray runs out), raw per frame. The spatial filter is evaluated offline
(`tools/gi_reference.py atrous`: edge-avoiding a-trous, 5 iterations, weights from normal / depth / luminance; temporal accumulation is not simulated: a still camera accumulates N frames of raw samples,
which equals the 4-spp column for N = 4). Mean relative error of the indirect irradiance against the reference:

| spot | DDGI + screen probes | pixel 1 spp raw | 1 spp + a-trous | 4 spp raw | 4 spp + a-trous |
|---|---|---|---|---|---|
| jungle | 13 % | 73 % | 18 % | 36 % | 14 % |
| pub_interior | 6 % | 64 % | 7 % | 41 % | 6 % |
| shadow_inland_a | 23 % | 64 % | 30 % | 35 % | 27 % |
| beach_sun | 24 % | 37 % | 19 % | 22 % | 19 % |
| monastery_hall | 107 % | 150 % | 84 % | 109 % | 92 % |

Biases equal the probes' (the cache bias passes straight into the pixel estimate: monastery +70 %, beach -14 %). Reading: with a filter, 1 spp per pixel matches the screen probes' error (better on the beach, a little worse in
foliage where the a-trous blurs thin geometry) but does not beat it, because the remaining error is the probe cache's bias and the filter's blur, not the first bounce's variance. Cost (RX 7900 XTX, 1600x900, whole-frame GPU
deltas, jungle): about 9 ms per shaded pixel sample (4 samples + the probe lookups ~36 ms), i.e. ~9 ms for 1 spp before any denoiser, against ~0.5 - 1 ms for the screen probes. The shading is not optimised (128-light
loop, incoherent shadow rays); a 2 - 3x gain is plausible (no light loop for far lights, ReSTIR-style light picks, ray sorting) but 3 ms+ remains.

## 6. NRD evaluation (not built)

Sources (web, 2026-11-04): NVIDIA-RTX/NRD on GitHub (https://github.com/NVIDIA-RTX/NRD): REBLUR (recurrent blur), RELAX (a-trous, for RTXDI), SIGMA (shadows); API-agnostic: NRD makes no graphics calls, the application runs
its compute dispatches (own implementation of the NRD API, or the NRI layer for D3D11 / D3D12 / Vulkan); shaders are HLSL (`NRD.hlsli`), precompiled DXBC / DXIL / SPIRV can be embedded.
License: not retrievable in this session; the task statement says MIT, to verify against LICENSE.txt before any use.
Inputs (REBLUR diffuse): noisy diffuse radiance packed with a normalised hit distance (`REBLUR_FrontEnd_PackRadianceAndNormHitDist`), world normal + linear roughness, view Z, non-jittered motion vectors, optional confidence / disocclusion
threshold. We have all of them for a diffuse-only signal: normals / view Z / motion from the G-buffer (`enh/gbuffer`; roughness = 1 constant), hit distance is a by-product of our traced ray (the closest hit t, already in `giShadeRay`), jitter-free.
Fit with our RHI: its dispatch descriptions (pipelines + resource bindings of ~20 compute shaders, constants up to several hundred bytes) do not fit "push constants <= 128 B, one root CBV b8, UAVs u0..u7 space1" as they are: we would map them
through the root CBV and the bindless heaps (NRD's permanent / transient pools are textures we create anyway) and compile its HLSL through our DXC path for SM 6.6 / SPIR-V and DXIL; its SPIRV / DXIL embed options remove that step. Estimated 3 - 5 days
(resource pool + binding mapping on both backends + tuning), not day-sized, so not built. Expected value: a production-grade denoiser would remove the blur of our a-trous at 1 spp and handle disocclusion, but cannot remove
the cache bias (section 5), which is what limits quality. Decision: not before the probe cache bias (failure mode 1) is understood.

## 7. DLSS Ray Reconstruction / AMD FSR Ray Regeneration: what they need from us

* NVIDIA DLSS Ray Reconstruction: RTX GPUs only; unavailable on the RX 7900 XTX of this machine: nothing here is verified. From NVIDIA's public integration notes (not retrieved today): noisy radiance, diffuse albedo, specular albedo,
  normals, roughness, linear depth, motion vectors (specular hit-distance / motion optional), exposure; replaces the denoiser and the upscaler.
* AMD FSR Ray Regeneration: part of the FSR SDK (2.2 from March 2026 added it; 2.3 / June 2026 ships Ray Regeneration 1.2.0 with ambient + specular occlusion denoising; `amd_fidelityfx_denoiser*.dll` next to the signed loader). Per
  https://gpuopen.com/amd-fsr-sdk/ (fetched 2026-11-04): Radeon RX 9000 series (RDNA 4) or newer, **DirectX 12 only (no Vulkan), SM 6.6, Windows 11**, signed AMD DLLs; the page lists no input buffers (the manual
  was not read). Press coverage: https://www.cgchannel.com/2026/03/amd-releases-fsr-sdk-2-2-with-new-ai-denoiser-ray-regeneration/, https://tweaktown.com/news/112349/amd-updates-fsr-sdk-with-fsr-4-1-1-support-for-rdna-3-and-improved-fsr-ray-regeneration-1-2-0/.
  Not usable on the 7900 XTX (RDNA 3) and only through the D3D12 backend: cannot be tested here.
* What we have vs what such denoisers want (both are specular-aware): noisy diffuse radiance (yes, planes 6 / 7), normals (G-buffer), depth, motion vectors (yes), diffuse albedo (the primary albedo trace of the reference pass proves we can
  get it; the G-buffer does not store it: add it), specular albedo and roughness (**missing**: Risen has no roughness / ORM textures; this is what the material-pack project would supply; a constant roughness 1 / dielectric F0 = 0.04
  would only feed the diffuse path), exposure (the engine's adapted luminance, `engine/exposure`). A specular GI / reflection signal does not exist in the game's lighting model at all, so these denoisers would act on diffuse GI
  (where a conventional denoiser does as well) until materials have roughness.

## 8. Recommendation

* **No-go for pixel GI as the default / as the next stage.** At 1 spp it costs ~9 ms at 1600x900 (unoptimised) and, filtered by a baseline a-trous, matches but does not beat the screen probes (5 - 25 % error vs 6 - 30 %), because what limits
  quality now is the probe cache's bias, and the pixel path would inherit it. A neural denoiser would improve noise, not bias, and is not available on the target GPU / backend (Ray Regeneration: RDNA 4 + D3D12 only).
* **Stage 1 (cheap, high value): fix DDGI's measured failure modes using the reference as the oracle**: indoor sky leak / bias (monastery +89 %, crystal cave volume +285 %), the bright-bounce under-estimate (beach -16 %), then sub-probe contact
  occlusion. `gi.reference=error` shows each in-game; `scripts/gi-reference.ps1` re-measures after every change (the unit: mean relative error and bias per spot in `summary.md`).
* **Stage 2 (optional, evaluate later):** pixel GI only as a high-quality tier for the near field (contact light within a few metres, 1 spp + temporal + a-trous), after stage 1 and after the diffuse albedo is in the G-buffer; then NRD
  REBLUR diffuse (3 - 5 days) if the in-game noise of the a-trous baseline is the complaint. Ray Regeneration / DLSS-RR when materials have roughness and only as optional vendor paths.
* Not verified: the temporal response to a moving light, D3D12 numbers beyond a smoke run, 2560x1440 cost, any vendor denoiser's quality.

## 9. Gates

* `rvk_tests`: 216 of 216 (the settings count test: 129 -> 133). `rvk_shadertool dxil-check embedded`: 58 of 58 (gi 5 / 5 with `gi_reference.comp`).
* `testdemo` PASS on Vulkan and on D3D12 (default settings: the new settings are Debug, off: the default look is untouched; nothing is created when `gi.reference` is off).
* Reference on Vulkan with `-Validation`: pub_interior run clean (no validation message). Reference on D3D12 (pub_interior, 256 spp): works and agrees with Vulkan (screen probes bias +0.8 %, mean rel. error 8 %; Vulkan 4 - 13 % depending on spp).
* Not run: refcompare (the changes only act with the Debug setting on), sync validation, 2560x1440 timing, GPU timings on the default lock (the cost figures are whole-frame GPU times from `-Perf` in the private copy, jungle, 1600x900).
* Known limits of the tool: the reference restarts whenever the sun / sky registers change (the hemisphere light drifts: epochs complete only in frozen scenes); the primary normal comes from the depth map (no normal maps, as the screen probes);
  characters are in the RT scene only if `gi.characters`; the sea surface is not in the volume's comparison (see failure mode 4).

## 10. Reducing DDGI's error: results (2026-11-04, second agent)

Tooling added: `tools/gi_reference.py analyze <run> --ref-run <dir[,dir]>` (reference planes from earlier converged runs, DDGI planes from this run: a re-measure needs only a few
reference samples, `gi-reference.ps1 -Spp 8 -Max 8 -SkipOff -SkipDdgi -RefRun ...`, `-Environment` for `RVK_SHADER_DIR` / `RVK_GI_EXP`), the reference's restart hash is now quantised
(flickering candles / a drifting sky no longer restart the accumulation), and `RVK_GI_EXP=<mask>` (developer experiments read from the volume header; with `RVK_SHADER_DIR` no rebuild):
1 no sky-ambient fallback when the probe lookup at a hit fails, 2 no exact point-light term at hits when the probes carry the lights, 4 sky misses = 0, 8 no multi-bounce at hits,
16 the screen probes WITHOUT the lights' term (= the behaviour before this work), 32 screen-probe reach x4.

### 10.1 The first finding was a measurement mismatch, then a real bug in the screen probes

* **Mismatch.** With `gi.lights=world` + `gi.lightmaps=replace` the probes carry the point lights' direct light (a delta per visible light, `gi_blend`); the study's reference "indirect" plane
  did not, so the candle-lit monastery read "+89 % / volume +523 %". The reference now puts the point lights' direct light at the primary hit into the indirect plane (the sun stays in the direct plane;
  `gi_reference.comp`). Corrected monastery_hall: reference mean 0.060 -> **0.486**; the volume is now **-27 %** (smooth 1.5 m probes against the candles' sharp pools and shadows), not +523 %.
  Spots without world lights (all outdoor spots, pub, crystal cave) are unchanged. There is no sky leak into the hall: EXP 1 (no sky fallback at hits) changes the monastery by 1 %, EXP 4 (no sky at all) by 2 %.
* **Bug.** The screen probes' ratio (traced / DDGI volume) divided an irradiance with the lights' direct term by one without it, so the screen resolve stripped the candle light: monastery screen bias **-77 %**
  (the lit picture went dark). `sp_trace.comp` now adds the lights' direct light at the probe's own surface (one light per ray, stratified, with a shadow ray) when the probes carry the lights.
* Other candidates, measured at the monastery (screen bias / mean rel. err.): fix alone -25 % / 87 %, EXP 2 (no light term at hits) -34 % / 75 %, EXP 3 (2 + 1) -35 % / 75 %,
  EXP 8 (no bounce) -38 % / 71 %, EXP 4 -27 % / 85 %: the bounce at hits adds ~13 % of the hall's light; the double count of the lights at hits (exact term + the probes' own) is real (EXP 2 lowers the error spread)
  but removing it makes the bias worse (the lights are under-represented in the probes), so it stays; nothing else in the cache leaks.

### 10.2 Before / after, all 8 spots (1280x720, 1024 spp reference, DDGI planes from short runs of the same build; EXP 16 = before, EXP 0 = after)

| spot | ref mean | screen bias before | after | mean rel. err before | after | MAE before | after | lit-image err before | after |
|---|---|---|---|---|---|---|---|---|---|
| beach_sun | 0.570 | -6.5 % | -6.4 % | 19 % | 19 % | 0.057 | 0.057 | 8.9 % | 8.9 % |
| jungle | 0.152 | +2.2 % | -4.1 % | 20 % | 22 % | 0.016 | 0.020 | 6.7 % | 7.2 % |
| harbour_night | 0.108 | -2.4 % | -2.4 % | 17 % | 17 % | 0.013 | 0.013 | 6.9 % | 6.9 % |
| pub_interior | 0.012 | -3.9 % | -3.9 % | 5 % | 6 % | 0.002 | 0.002 | 6.4 % | 6.3 % |
| monastery_hall | 0.486 | **-76.9 %** | **-19.1 %** | 89 % (median 81 %) | 98 % (median 42 %) | 0.405 | **0.190** | 48.2 % | **33.2 %** |
| crystal_cave | 0.006 | -7.0 % | -4.5 % | 4 % | 4 % | 0.001 | 0.001 | 3.8 % | 3.5 % |
| lake_jungle | 0.343 | -2.9 % | -2.9 % | 28 % | 28 % | 0.054 | 0.054 | 7.9 % | 7.9 % |
| shadow_inland_a | 0.450 | +0.2 % | -0.4 % | 25 % | 28 % | 0.048 | 0.052 | 11.9 % | 12.9 % |

Spots without world lights run the identical code path in both columns: their differences (jungle, shadow_inland_a, pub) are the run-to-run noise of the screen probes (repeat runs of one build:
jungle -4.1 / +6.6 / +2.2 %, lake_jungle -2.9 / -0.4 / -2.5 %, rel. err +-3 points). The monastery's mean relative error stays high because it is dominated by the pixels in the candles' shadows
(reference near 0, the smooth probes leak light into them; relative error against max(ref, 0.02)); its absolute and median errors halve. The monastery reference counts the lights now: **the old +89 % / +523 % figures of
section 4 are void** (the other rows of section 4 stand; the beach's old -16.5 % reads -6.5 % in this build's runs: run-to-run state differs).
The crystal cave's reference is the study's (no world lights there; a converged new reference was not obtainable: its sky / lights drift, the accumulation ended at 600 of 1024 samples twice).

### 10.3 Evaluated, not adopted

* **Bright sunlit bounce / the sea** (beach_sun): the sea is excluded from the RT scene on purpose (it reflects; `RtScene::learnExcluded`). Pixels where the volume reads 0 are 4.9 % of the beach picture with a reference of 0.095
  (against a mean of 0.57): no weight in the error, and the game falls back to the engine's ambient there. A proxy hit for the sea would make rays towards the sea darker (the sea floor / sky they see now are brighter than a dark sea albedo)
  and push the beach's slight under-estimate the wrong way: not built. The beach's under-estimate (-6 %) is the screen probes' far field (the sky radiance reconstructed from the probe SH: rtgi.md 23); a 4x longer
  screen-probe reach (EXP 32, 160 m) made it **worse** (-29.7 %, jungle +218 % in the same run) and was dropped.
* **Contact occlusion**: `gi.screen=high` (finer tiles) lowers the mean relative error by 0 - 5 points on the foliage / shadow spots (jungle 22 -> 22 %, lake_jungle 28 -> 24 %, shadow_inland_a 28 -> 23 %, monastery 98 -> 82 %;
  one run of harbour_night in that batch diverged: +173 %, volume +160 %, an unexplained state difference, not used) at the cost of its extra probes (not re-timed here). The depth-edge class has biases of -2 .. -6 % in this
  build (the study's +14.6 % came with an older state) and a relative error of 22 - 59 % that looks like noise, not a systematic over-brightening; a GTAO / bent-normal term would need a second per-pixel occlusion
  source blended with the traced ratio: not built, the default stays `gi.screen=medium`.
* **Canopy / coloured bounce**: not touched (errors of both signs, patchy; chroma errors 2 - 8 %).
* **Moving light**: not measured (a converged reference needs a still scene, section 4 point 7). Probe hysteresis is 0.98 with the adaptive fast blend (rtgi.md 20).

### 10.4 Gates

`rvk_tests` 216 / 216, `rvk_shadertool dxil-check embedded` 58 / 58; `rvk_shadertool corpus corpus --gi`: 2246 PS ok, 0 failures (2483 shaders); testdemo 7 / 7 with `-Validation` and GI on (world lights, replace) on Vulkan wrapper + full (no validation message in the digest);
D3D12 wrapper + full with defaults and `-Validation` 7 / 7, D3D12 wrapper with GI on and the debug layer (`RVK_NO_GBV=1`) 7 / 7 (with the GPU-based validation and GI on the RT scene makes the run ~0.1 fps and the scenario times out: known, rtgi/dx12 notes).
Not run: `rvk_rhitest` GI parity, refcompare (gi off paths are untouched: the only changes act with `flags.z` bit 0 set, i.e. GI on with world lights + replace, and in the Debug reference), a GPU timing of the extra shadow ray per screen-probe ray
(one `giPointLights` call per ray at the probe's own surface when the lights are in the probes: the 128-light loop is the cost driver of that pass; expected well under the pass's ~1 ms, unmeasured), the moving-light convergence measurement.

## 11. The sky of escaping rays: the engine's sky cube, and the hemisphere SH instead of the far probe (2026-11-04, third agent)

Question: let screen-probe rays that escape (no hit within 40 m, `rvkGiSkyRadiance` of the far DDGI probe's SH) read the engine's real sky. Result: **no change shipped; the far-probe field stays.** Two parts.

### 11.1 The sky cube is not a usable radiance source [C]
Source: the engine's `eCSkydome_PS::UpdateSkydomeEnvCubemap` cube (illum +0x470, 128^2 A8R8G8B8, LDR, display-space colour; dome + clouds + sun disc / glow + stars / moon, no geometry;
bindless tCube slot = `RvkFrame.images.w`, the same one the water and the material pack sample). Read in the shader without trouble (`sp_trace` diag, sampler `screen3.w`); the cube is **not stale** in the test spots (water on = whole cube
redrawn every frame vs water off: numbers within 3 %), so stability is not the problem. Measured with `RVK_GI_DIAG=1` (`tools/gi_sky_diag.py`, runs `out\skyfar\diag0/diag1`): the cube along the far rays, as `^2.2`
(display -> acc, assumed) against the engine's hemisphere SH sky on the same rays (cube^2.2 / SH sky; spread = std / mean over the rays):

| spot | beach_sun | jungle | harbour_night | lake_jungle | shadow_inland_a | pub_interior |
|---|---|---|---|---|---|---|
| mean ratio | 0.31 | 2.4 | 0.2 | 0.35 | 0.37 | (indoor fill 0.17 vs cube 0.21 on few rays) |
| spread over rays | 1.5 - 1.8 | 0.8 | 1.9 - 2.0 | 1.9 - 2.0 | 1.4 - 1.8 | 0.7 - 1.0 |

The cube's brightness has no fixed relation to the hemisphere's ambient (art-directed separately: jungle's cube is 2.4x its SH, beach 0.3x), its angular structure differs from the SH by more than the SH's own mean (spread 1.5 - 2: sun glow,
horizon, clouds), it is 8-bit and clamped (the sun disc is 1.0, the NEE sun's colour is above that: the disc would be counted twice), and indoors (pub: the engine's "sky" is a fill colour) it holds the day sky. A per-frame calibration to the SH's brightness would
leave only the shape mismatch. And the probes store **L2 irradiance** (7 registers): detail above L2 (clouds, disc) cannot be represented by DDGI nor survives the cosine-weighted screen-probe average; only the L0 - L2 content of the cube would matter, and that is
exactly what differs from the engine's own ambient (hue and the unoccluded look would drift away from the engine's: the GI replaces the engine's ambient, it should keep its sky). Not built: no pass, no reference change. The diag stays (hidden, `RVK_GI_DIAG`: the `sky cube:` terms of the `enh: gi diag` lines).

### 11.2 The hemisphere SH for every escape (`RVK_GI_EXP=64`) is worse [C]
The reference path tracer and the DDGI rays already use `rvkGiSkyRadiance(sky)` (the hemisphere SH) on a miss, so the cleanest "same source" experiment is to drop the far-probe override of the screen probes (exp 64). Quick re-measure
(`out\skyfar\remeasure.ps1`: 8 spp, the converged references of the earlier runs; screen bias / mean rel. err / MAE / lit-image err; the same build, EXP 0 vs 64):

| spot | EXP 0 (far probe, shipped) | EXP 64 (SH sky) |
|---|---|---|
| beach_sun | -11.6 % / 21 % / 0.077 / 8.9 % | -35.6 % / 41 % / 0.208 / 10.9 % |
| jungle | -3.3 % / 21 % / 0.018 / 6.9 % | +15.1 % / 35 % / 0.032 / 10.5 % |
| harbour_night | -2.4 % / 17 % / 0.013 / 6.9 % | +32.3 % / 39 % / 0.036 / 12.4 % |
| pub_interior | -4.0 % / 6 % / 0.002 / 6.4 % | -3.5 % / 6 % / 0.002 / 6.6 % |
| monastery_hall | -19.6 % / 96 % / 0.190 / 33.0 % | -19.7 % / 95 % / 0.189 / 32.6 % |
| crystal_cave | -5.6 % / 4 % / 0.001 / 3.6 % | +2.3 % / 3 % / 0.001 / 2.6 % |
| lake_jungle | -3.0 % / 26 % / 0.052 / 7.5 % | +18.0 % / 46 % / 0.108 / 11.1 % |
| shadow_inland_a | +0.3 % / 26 % / 0.049 / 12.3 % | +24.1 % / 50 % / 0.143 / 16.4 % |

The far probe carries what lies beyond the 40 m reach (hills, tree lines, the reference sees them out to 1 km): the flat SH over-lights those pixels. So the beach's -6 .. -12 % is the price of the L2 reconstruction at the horizon, not of a wrong source.
(Beach's -35 % under EXP 64 is odd and not explained; the other spots' signs fit "no distant occlusion". Run-to-run spread of one spot's bias is +-4 points.) Kept as the developer switch `RVK_GI_EXP=64`.
Not tried: a hybrid (the far probe's occlusion with the SH's shape) or a longer, cheaper far trace; both stay open ideas.

### 11.3 Tooling and housekeeping
`gi-reference.ps1` now deletes `gi_ref` (tens of GB: one 59 MB dump per frame at `-Max 8`) after the analysis unless `-KeepDumps` or `-Max >= 512`; use `-Spp 8 -Max 64` for quick re-measures. A run ended while a dump was being written leaves a truncated `.npy`:
delete it and its `_meta.json`. Gates: rvk_tests 224 / 224, `dxil-check embedded` 58 / 58, one `-Validation` run of beach_sun + pub_interior with GI + diag: no VUID. Not run: corpus --gi (no corpus in this worktree), rhitest, testdemo on both APIs (the shipped paths are unchanged: the cube sampling sits behind `RVK_GI_DIAG`).
