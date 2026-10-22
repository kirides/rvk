# The FFT ocean (M8): spectrum cascades, the sea floor field, waves calming towards the shore and the harbour

The open sea of the enhanced water (docs/research/water.md) simulated from a wave spectrum: big waves far out that get
progressively smaller towards the beaches and inside the harbour basin, waves rolling in parallel to the coast, breaking on
the shallows, surf, foam, a real, lower shoreline; the colour stays the engine's sea material. Replaces the M7 Gerstner sum
when `water.ocean = fft` (the default of the sea simulation; the enhanced water itself stays off by default).

Legend: **[C]** confirmed (measured in the running game or by tests), **[I]** inferred / proposed. World units are
centimetres (Risen), the spectrum is evaluated in SI units.

## 0. Requirements (user)

* 2026-10-22, verbatim: "For the ocean and the harbor it's ok to have a slightly different look, as long as the color
  matches well enough and we have a crazy beautiful water simulation, big far waves getting progressively smaller the closer
  they get towards the harbor/shore."
* Still in force (water.md 0): the ocean is the open sea only (the world-spanning sea plane, water.md 3); lakes, rivers,
  waterfalls, the swamp pond never get waves or a shore; volumetric water and a real lower shoreline instead of the painted
  surf; the colour anchored to the engine's sea / harbour material. GLSL only. Visual only: the engine's physics plane is
  untouched.

## 1. What exists

| piece | files |
|---|---|
| spectrum + cascades + CPU reference (JONSWAP, Hasselmann spreading with the swell term, h0, dispersion, radix-2 IFFT, direct DFT) | `src/enh/ocean_spectrum.{h,cpp}` (tests `tests/ocean_tests.cpp`) |
| GPU simulation: tiles, compute pipelines, barriers, mip chains, the anisotropic wrap sampler | `src/enh/ocean.{h,cpp}` |
| compute shaders: spectrum (h0), time evolution (4 packed complex fields), the shared-memory FFT (spec constants N, log2 N), resolve + Jacobian foam | `shaders/water/ocean_compute.glsl`, `ocean_spectrum.comp`, `ocean_evolve.comp`, `ocean_fft.comp`, `ocean_resolve.comp` |
| sea floor field: top-down height raster of the engine's static meshes, signed distance, depth, travel time (eikonal), exposure, disk cache | `src/enh/sea_floor.{h,cpp}` (tests `tests/ocean_tests.cpp`); the rasteriser and the distance transform shared with `src/enh/shore_field.{h,cpp}` |
| observing the engine's static geometry: `DrawReplacer::wantsOpaque / observeOpaque` (main view, Solid pass, indexed static meshes) | `src/d3d9/device.h`, `src/d3d9/draw.cpp` (`prepareDraw`), `src/enh/water.cpp` (`observeOpaque`, `serviceFloor`) |
| the engine's weather (states + blend factors) -> sea-state level | `src/engine/weather.{h,cpp}`, symbols in `src/engine/symbols.h` |
| surface: cascade sampling (vertex: explicit LOD by footprint; pixel: filtered + lost slope variance + macro slope), attenuation, near-camera cap, the shore wave train | `shaders/water/ocean_surface.glsl` (included by `water_common.glsl`) |
| geometry (FFT path of the clipmap), shading (macro Fresnel, SSR on the macro normal, Toksvig-like spec, SSS, foam texture, surf milk) | `shaders/water/water_ocean.vert`, `shaders/water/water.frag`, `shaders/water/water_common.glsl` (`foamTexture`, `foamCover`, `waterWorley`) |
| the ocean in the water module: settings, sea state follower, the simulation pass (AfterOpaque), WaterBlock fields | `src/enh/water.{h,cpp}` |
| settings `water.ocean`, `water.ocean_quality`, `water.sea_state`, `water.wind_direction`, `water.wave_scale`; debug views `sea_floor`, `waves` | `src/core/settings.{h,cpp}`, `src/hal/vulkan/device.cpp` (`applyWater`) |
| spots `sea_cliff`, `sea_horizon`, `harbour_mouth`; motion sequences (`-Burst`, `-Interval` -> `motion_<spot>.png`) | `mods/rvkcompare/spots.lua`, `scripts/water-compare.ps1` |

## 2. The simulation

### 2.1 Spectrum (implemented from the papers; `ocean_spectrum.cpp` = `ocean_compute.glsl`)

* Statistical wave field after Tessendorf ("Simulating Ocean Water"): `h(k,t) = h0(k) e^{iwt} + conj(h0(-k)) e^{-iwt}`,
  `h0 = 0.5 (xi_r + i xi_i) sqrt(S(k) dk^2)` (so the real field's variance is `sum S(k) dk^2`; unit-tested statistically),
  Gaussian pairs from a PCG hash of (texel, cascade): deterministic, identical on CPU and GPU, no upload.
* **Wind sea: JONSWAP** with the fetch-limited parameters of Hasselmann et al. 1973 as given by Horvath 2015
  (`alpha = 0.076 (U^2 / F g)^0.22`, `w_p = 22 (g^2 / U F)^(1/3)`, `gamma = 3.3`, `sigma` 0.07 / 0.09). Measured [C, test
  `ocean_jonswap_significant_height`]: these parameters give an Hs 1.31 x the empirical growth law `g Hs / U^2 = 0.0016
  (g F / U^2)^0.5` at every fetch (the two fits of the JONSWAP data differ); the presets are chosen with it.
* **Directional spreading: Hasselmann** `D = Q(s) |cos(theta / 2)|^{2s}` with `s(w)` of Horvath 2015 eq. 39 plus his swell
  term `16 tanh(w_p / w) xi^2`, `Q(s)` from ln-Gamma (Stirling series after a shift; tested against `std::lgamma`);
  normalised over [-pi, pi] [C: test, 6 exponents within 0.2 %].
* **Swell**: a second JONSWAP-shaped spectrum at its own period and direction (wind + 0.3 rad), narrow (`s` = 40), its
  `alpha` solved so its significant height is the preset's [C: test].
* `S(k) = S(w, theta) dw/dk / k` (deep water); the k-plane integral equals the 1D `m0` [C: test, within 2 %]; the
  smallest ripples damped by `exp(-(k * 1 cm)^2)`.
* Dispersion quantised to a repeat time of 256 s (Tessendorf): the animation loops, the time stays small (fp32 phase).

### 2.2 Sea states (`seaStateForLevel`, levels interpolated, fetch in log space)

| level | U10 m/s | fetch km | wind-sea Hs (JONSWAP) | swell Hs / period | choppiness | weather (auto) |
|---|---|---|---|---|---|---|
| 0 calm | 5 | 25 | 0.5 m | 0.8 m / 10 s | 0.75 | |
| 0.6 | | | | | | `Sunny_Full` |
| 1 moderate | 8 | 60 | 1.3 m | 1.1 m / 11 s | 0.9 | |
| 1.2 | | | | | | `Sunny_Half_*`, clouds |
| 2 rough | 11.5 | 110 | 2.6 m | 1.5 m / 12 s | 1.1 | rain |
| 3 storm | 15 | 180 | 4.3 m | 2.2 m / 13 s | 1.25 | `Stormy_*`, thunder |

Measured totals (log `enh: ocean: sea state: Hs ...`): storm Hs 4.68 m (wind-sea peak 118 m) [C]. `water.wave_scale`
scales the amplitude (the spectrum's variance by its square).

### 2.3 Cascades (each wave number in exactly one band: no frequency counted twice)

| quality | tiles | sizes (m) | band edges |
|---|---|---|---|
| low | 3 x 128^2 | 250 / 50 / 10 | `k_high(i) = 4 * 2 pi / L(i+1)` (wavelengths 12.5 m, 2.5 m) |
| medium | 4 x 256^2 | 310 / 87 / 23 / 6 | wavelengths 21.75 m, 5.75 m, 1.5 m |
| high | 4 x 512^2 | 310 / 87 / 23 / 6 | (same bands, finer Nyquist: 1.2 cm in the last tile) |

Non-commensurate sizes (no common repeat); every band lies below its tile's Nyquist wave number [C: test]; the bands' height
variances add up to the 1D spectrum's within 8 % (medium, moderate sea) [C: test]. Per cascade the CPU also sums the band's
slope variance (`cascadeStats`, a 64^2 grid), used for the shading's roughness.

### 2.4 GPU passes (`Ocean::record`, at `AfterOpaque` before the wet-beach pass, main view only, while the sea was drawn in the last 30 frames)

1. **spectrum** (only when the sea state changed by > 1 %: the level is followed in steps of 0.05): `h0(k)`, `conj(h0(-k))`
   into an RGBA32F image per cascade;
2. **evolve**: `h(k,t)` and the spectra of the displacement and its derivatives packed as four complex fields whose inverse
   transforms are real (`Dx + i Dz`, `Dy + i dDy/dx`, `dDy/dz + i dDx/dx`, `dDz/dz + i dDx/dz`) into two RGBA32F work images;
3. **FFT** rows, then columns: one workgroup per line, N/2 threads, the line in shared memory (2 x N x 16 bytes: 16 KiB at
   512 = the Vulkan minimum), bit-reversed load, log2 N radix-2 stages; N and log2 N are specialisation constants
   (pipelines for 128 / 256 / 512 built at start; sizes beyond the GPU's limits are skipped); the same algorithm as
   `ifft1D` [C: test against a direct DFT, N = 8 / 32, error < 1e-5 relative];
4. **resolve**: the `(-1)^(x+z)` sign of the centred k-grid, choppiness, displacement (RGBA16F: dx, dy, dz in metres, foam)
   and derivatives (RGBA16F: dDy/dx, dDy/dz, dDx/dx, dDz/dz); **foam** where the Jacobian `(1 + Dxx)(1 + Dzz) - Dxz^2` falls
   below 0.8, accumulated over time (decay 0.35 / s) in the displacement's alpha;
5. **mip chains** of both outputs by blits (one barrier batch per level for all cascades); the images end in
   `SHADER_READ_ONLY` for the vertex and fragment stages.

Push descriptors (5 storage images) + a 112-byte push block; images are the module's own (`vk::Image`, per-layer views for
the bindless set). Memory: medium 17 MiB, high 69 MiB, low 3 MiB [C: log].

## 3. Geometry (`water_ocean.vert`, FFT path)

The M7 camera-centred clipmap (8 levels of N x N quads, 25 units at level 0, snapped to twice the spacing, the finer
footprint left out, odd outer-edge vertices averaged) is kept. Per vertex:

* the cascades are sampled at the undisplaced position with an **explicit LOD from a continuous footprint**
  (`max(25, distance * 6 / N)` world units): the same world point gets the same displacement on both sides of a level
  border (no cracks), the coarse levels take prefiltered waves from the mips (no aliasing, no popping where the clipmap's
  footprints move);
* attenuated per cascade by the sea floor field (section 4) and capped near the camera when the camera is down at the
  water (wading, swimming: `nearCameraCap`, 30 units at the camera, full from 3,000 units; the cap fades out for a camera
  2 .. 7 m above the sea). The pixel shader caps only the two long cascades' slopes the same way: the short waves' normals
  stay, so the flattened water near a swimmer still shows its chop;
* plus the shore wave train and the M7 lowered, swashing waterline (now in phase with the train: the run-up arrives with
  the crests);
* towards the far plane (45 .. 90 % of the engine's far) the displacement fades to the mean level: the sea meets the
  horizon without a jagged silhouette, the shading keeps the waves' normals.

The engine's sea plane draws stay skipped, the fake shore overlay too (water.md 4.1); the physics plane is untouched.

## 4. The sea floor field and the attenuation (the key visual)

### 4.1 The field (`sea_floor.cpp`)

**Source: the engine's own static geometry.** `d3d9::DrawReplacer` got an observer hook: for main-view Solid-pass draws of
kind `StaticMesh` (indexed, from buffers, triangle lists / strips, no BLENDINDICES, no constant-instancing TEXCOORD4) the
water module reads the draw's index range and vertices from the buffers' CPU copies, transforms them by the draw's world rows
(c33..c35) and queues the triangles; one entry per mesh range and placement (hash of buffers, range and world matrix), at
most 8 MiB of geometry per frame. A worker thread rasterises them top-down (highest surface per texel: terrain, the ground
under the water, rocks, quays, ships) into a fixed grid of **2048 x 2048 texels of 84 units** at (-104,000, -96,000) (the
coast overlay's box + 30,000 units out to sea, fixed so the cache stays valid) and bakes:

* **signed distance** to the waterline (Felzenszwalb-Huttenlocher on the land and on the water masks; land = the highest
  surface above the sea level);
* **depth** = sea level - highest surface (texels no geometry covered count as open sea, 30 m deep);
* **travel time to the shore** `T`: the eikonal `|grad T| = 1 / c(depth)` with the phase speed of an 8 s wave (Fenton &
  McKee's approximation of the dispersion relation: `sqrt(g d)` in the shallows, `g T / 2 pi` deep), solved by fast sweeping
  (Zhao 2005: 4 orderings x 2 rounds, Godunov upwind update), sources = water texels next to land, capped at 60 s;
* **direction** of travel `-grad T` and the **coherence** of the fronts (the length of the box-blurred direction: ~0 on the
  ridges where fronts from two coasts meet);
* **exposure**: on a coarse grid (~600-unit cells) the share of 16 directions in which the water stays open for 400 m,
  remapped (an open beach sees ~half of them -> 1, a basin behind a narrow mouth -> ~0), blurred once (diffraction);
* the M7 distance to the engine's coast overlay (beaches: where the waterline is lowered and the sand gets wet).

Two RGBA16F textures (A: signed distance, depth, direction x coherence; B: travel time, exposure, overlay distance, seen),
uploaded when a bake finishes. **Cache**: the heights (`bin\rvk\cache\ocean\floor_v1.bin`, 16 MiB, merged by max with what a
session sees) are written by the worker at most every 20 s and when the module is destroyed; never under the game's data.
Measured [C]: first session at the beach 100 meshes -> 3.0 M texels seen, the next sessions start from the cache
(`3424175 texels known`). A bake took 1.8 - 2.0 s at first; the transforms and the exposure now run in parallel and the
travel time is solved at half resolution (1024^2, interpolated over the water): **~0.4 s** (9.5).

Real-world check [C, a probe of the cached field]: the opening beach (-38,559 / -5,770) lies on land, the sea in front of it
is a 1 m deep shelf out to x = -42,000 (the waves break on its edge), 10 m at -45,000, 20 m at -50,000; travel times 3.3 s
(600 units out) .. 12 s (3,300 units); direction (0.70, 0.71) = towards the beach; exposure 0.57 .. 1. The harbour basin
(-5,588 / -22,007): exposure 0.001 (sheltered), its floor partly unseen (deep).

### 4.2 Attenuation (`cascadeWeights`, per cascade c with its typical wavelength L_c: the wind-sea peak clamped into the band)

* **depth**: `1 - exp(-d / (0.18 L_c))`: waves feel the bottom from about half their length, the long ones first;
* **distance to the waterline**: `smoothstep(0, 0.8 L_c, sd)`: no long swell right at the shore;
* **shelter**: `mix(1, exposure, (0.85, 0.75, 0.45, 0.2)[c])`: a harbour basin keeps 15 % of the long swell but most of the
  short wind chop;
* **breaking limit**: all weights scaled so the sea's amplitude (~Hs / 2) stays below 0.4 x the depth;
* near the camera at the water: the cap above.

Result: the far sea at full height, smaller towards the shore over the shallows, the harbour basin calm with short chop;
`water.debug=waves` shows the three longest cascades' weights in red / green / blue, `sea_floor` depth / exposure / the
train's phase.

### 4.3 The waves rolling in (`shoreTrain`)

A wave train whose phase is the travel time: `theta = w (t + T(x))`, period 6 + 1.5 x level s. Its crests move towards
the shore at the local phase speed, get closer together in the shallows (shoaling of the wavelength) and turn parallel to
the depth contours (refraction): both follow from the eikonal field, not from a hand-placed direction. Height `0.45 Hs`
(25 .. 160 units, x `wave_scale` up to 1), x a mild Green's-law shoaling (up to 1.35), faded beyond 2.5 .. 5 periods of
travel, by incoherent fronts and by low exposure. **Breaking**: where the height reaches 0.78 x the depth (`breaking` 0..1)
the crest leans forward (second harmonic), the horizontal motion grows, and the broken bore keeps ~0.5 x the depth over the
surf zone. Displacement and slope are analytic (`d theta / d xz = -w dir / c`).

### 4.4 Surf, foam, swash

* **Breaking foam**: a white front on the breaking crest and a trail of broken water behind it (`exp(-4.5 x phase since
  the crest)`), only on segments chosen by slow noise along the coast (not every stretch breaks at once).
* **Surf zone**: where waves break or have broken: streaky broken water + **milk** (the aerated, sandy water of the surf
  zone: the sea's own tint scattered back, up to 70 %).
* **Folding foam** of the open sea (the Jacobian, temporal), **contact lace** at the waterline and at rocks / walls.
* Every foam term is a coverage applied to one **foam texture** moving with the water (sampled at the undisplaced position):
  ridged-noise filaments at 95 / 34 / 12 units over a warped domain, cellular bubbles punched in, broken into patches;
  scales smaller than a few pixels (`fwidth` of the position) fade to their mean: no sparkle at distance. (The first
  versions used cellular noise edges: they read as a hexagon net and were replaced.)
* **Swash**: the M7 lowered waterline (13 units below the engine's, the overlay band only) swashes +-8 units in phase with
  the train's arrival; the wet-beach pass (water.md 4.5) darkens the sand it reaches.

## 5. Shading (`water.frag`, the engine's ocean model kept)

* The normal per pixel from the cascades' filtered derivatives (anisotropic trilinear on the tiles) with the same weights
  as the geometry, plus the train's slope; the material's normal maps on top, weakened by up to 30 % only where the FFT's
  short waves are strong, near (< 1,500 .. 6,000 units) and at steeper views: full strength in sheltered water (the
  harbour keeps the engine's texture), far away and at grazing views (the engine's sparkle on the sky reflection).
* **Fresnel of a calmer surface**: the engine's Fresnel term uses the macro normal (cascades 0 + 1 + the train) with its
  tilt damped to 40 % (at grazing views the faces turned away hide behind the crests and the eye sees the sky over most of
  the sea, as on the engine's flat sea); **SSR** follows the macro normal too (the short waves would scatter the rays into
  dark rocks); on slopes facing away the cube lookup's direction is kept above the horizon.
* **Specular without sparkle**: the slope variance the texture filtering removed (`textureQueryLod` per cascade x the
  band's slope variance) widens the engine's specular lobe (Toksvig-like, energy kept).
* **Subsurface scattering** on the crests: backlit by the sun when looking towards it (`((-V.L) / 2 + 1 / 2)^4`), more on
  thin grazing crests, a little sky light in every crest, in the material's own turquoise (its diffuse textures x 1.4).
* Fog, refraction, depth fade, the engine's lighting curve, volume tint: as in M7.

## 6. Sea state from the weather (`engine/weather.*`)

`eCModuleAdmin::FindModule("eCWeatherAdmin")` (vtable-checked), `GetWeatherStatesCount / At`, `eCWeatherStates::GetName /
GetBlendFactor` (the same calls risen-spass's `risen.world.weather` uses), SEH-guarded, read every 0.5 s on the render thread.
Each state's level from its name (storm / thunder 3, rain 2, clouds / `Half` 1.2, fog 0.5, sun 0.6, other 1; `Dungeon`,
cave and indoor states carry no weight), the blend-weighted mean; the sea follows it with a 3 s time constant (waves grow
as a storm comes in). Measured [C]: `Stormy_Rain 1.00 -> sea state 3.00`, `Sunny_Full -> 0.60`, `Sunny_Half_02 -> 1.20`
(rvk.log `enh: ocean: weather ...`). Wind direction: `water.wind_direction` or the prevailing 20 deg (towards +x, slightly
+z: from the south-west onto the west beaches); the swell 17 deg further.

## 7. Settings (category Water, live, A/B-eligible)

| key | default | |
|---|---|---|
| `water.ocean` | `fft` | `off` (the sea shaded on its own mesh, engine surf), `gerstner` (M7), `fft` |
| `water.ocean_quality` | `medium` | low 3 x 128^2, medium 4 x 256^2, high 4 x 512^2 (the largest the GPU's compute limits allow) |
| `water.sea_state` | `auto` | weather / calm / moderate / rough / storm |
| `water.wind_direction` | -1 | degrees (0 = +x, 90 = +z), -1 = prevailing |
| `water.wave_scale` | 1.0 (0..3) | replaces M7's `water.wave_height` (also scales the Gerstner waves) |
| `water.debug` | off | + `sea_floor`, `waves`; `foam` shows the FFT foam terms (red folding, green breaking, blue surf, white contact) |

No capability requirement: compute + storage images are the floor; anisotropic filtering when the device has it.

## 8. Costs [C]

`sea_horizon`, 1920 x 1080, vsync off, water quality high, ocean quality high (4 x 512^2), `-PerfFrame 2400`: the ocean
simulation pass **0.432 ms** (medium 0.222, low 0.122), the render pass with the refracted + alpha draws (sea grid incl.)
0.228 ms (low 0.129), the wet beach 0.025 ms; GPU frame 2.91 ms. CPU: the injected passes record ~0.2 ms per frame
(dispatches, push descriptors, 4 x 2 x 9 blits); observing the static meshes for the floor field did not change the
draw-call CPU time measurably (2.29 vs 2.33 ms per frame, Gerstner vs FFT). Live toggles of `water.ocean`, `water.ocean_quality`, `water.sea_state`, `water.enabled`: 0 pipelines built,
max frame interval 15.4 - 25 ms in the 5 s windows (the session's normal spread at 80 fps).

## 9. Verification

### 9.1 Side by side, luma / hue (`scripts\water-compare.ps1`, left engine, right FFT ocean, 1600 x 900)

Final runs: **`out\water\20261022-102006\`** (the six sea spots, engine pictures + water masks of `20261022-085233`:
splash-free engine captures; `sbs_<spot>.png`, motion sheets `motion_<spot>.png` = 6 frames 6 apart, single frames in
`enh\`) and `out\water\20261022-101627\` (all 10 water spots incl. the inland water, engine / masks of
`20261022-095007`). Mean Rec. 709 luma of the 8-bit pictures over the water mask, saturation-weighted hue:

| spot | engine luma | FFT ocean | d | engine hue | FFT | sat engine / FFT | M7 Gerstner (water.md 8) |
|---|---|---|---|---|---|---|---|
| sea_cliff (new, 15 m over the beach's shelf) | 86.1 | 85.8 | -0.2 | 180 | 182 | 0.40 / 0.41 | |
| sea_horizon (new, open sea to the horizon) | 73.3 | 73.0 | -0.3 | 164 | 168 | 0.26 / 0.28 | |
| harbour_mouth (new, basin -> mouth -> sea) | 59.4 | 54.6 | -4.8 | 160 | 166 | 0.26 / 0.30 | |
| harbour_docks | 39.2 | 36.5 | -2.7 | 114 | 119 | 0.24 / 0.22 | -4 |
| beach_rain (storm) | 60.7 | 45.9 | -14.7 | 192 | 191 | 0.22 / 0.23 | -12 |
| beach_sun | 132.3 | 98.7 | -33.6 | 99 | 118 | 0.18 / 0.20 | -18 |
| lake_jungle / waterfall_jungle / swamp_pond / crystal_cave (inland: unchanged by M8) | | | -1.4 / -2.4 / +0.9 / -3.0 | | | | |

The beaches stay darker than the engine: its additive surf overlay paints bright bands over the whole 1 m shelf (and onto
the sand, where the FFT ocean's lowered waterline shows wet sand); the FFT ocean has broken water, foam bands and milky
surf only where its waves break. Measured over the pixels both renderers show as water (beach_sun): engine 128.6 /
FFT ~92 -> the gap is the water itself, ~13 of it the surface colour (rough water at grazing views), the rest the engine's
painted surf. Tuning steps that moved it (each measured): macro-normal Fresnel / SSR (+3), the material's normal maps kept
at grazing views (+5), milk never darkening (+2); experiments that did not: the reflection vector kept above the horizon.
The open sea, the cliff view and the harbour match within 5.

### 9.2 What the pictures show (looked at, per spot)

* **sea_cliff**: the open sea to the horizon with waves of several scales, the shelf's breaking zone as milky water with
  patchy foam filaments, crests rolling in parallel to the coast (the motion sheet shows them advancing), no foam band with
  hard edges (the first versions had solid white bands and later a hexagon foam net: both replaced).
* **sea_horizon**: rolling swell to the far plane, the displacement fading into a straight horizon (no jagged silhouette).
* **harbour_mouth**: the basin calm with short chop and the engine's normal-map texture, the sea beyond the mole rougher:
  the waves get smaller towards and inside the harbour.
* **harbour_docks**: the teal harbour with the material's texture, the quay walls' waterline clean; the "glittery patch"
  bottom-left of the M7 picture is the engine's wave-splash particle effect (present in the engine's own capture
  `out\water\20261022-095007\eng\eng_harbour_docks.png`; timed, so only some captures show it): not part of the water.
* **beach_sun / beach_rain**: waves rolling in and breaking on the shelf's edge, foam lines advancing over the 1 m deep
  shelf, lace at the waterline, no hard straight waterline edge (a 12-unit soft edge, the swash).
* Storm (`water.sea_state=storm`, run `out\water\20261022-084940`): Hs 4.7 m, big swell to the horizon, whitecaps.

### 9.3 Motion

`-Burst 6 -Interval 6` (6 frames ~ 0.08 s apart at the 80 fps cap): the crests move consistently towards the shore,
the foam moves with the water (sampled at the undisplaced position), no popping where the clipmap levels meet (continuous
footprint LOD; no level border is visible in any sheet), no swimming textures (the foam / normal maps are attached to the
undisplaced world position, the grid's vertices are snapped). Sheets: `out\water\20261022-102006\motion_sea_cliff.png`,
`motion_beach_sun.png`, `motion_harbour_mouth.png`, `motion_sea_horizon.png`.

### 9.4 Swimming (`rvk_swim`, run with the water on and off: `out\water\swim\`)

The hero put into the sea off the beach (-43,500 / -8,200) swims at y = -250 (his origin; the engine's swim depth) [C];
far out (15 m deep, -47,000 / -10,500) the engine puts him back on the beach after ~6 s. With the follow camera (~2-3 m
above the water) the waves around him are capped to ~30 units: the waterline sits at his chest as with the engine's flat
sea. Open: from a camera 1 m above the water next to him the engine's swim-splash particle shows as a flat bright patch
over the FFT water and hides his upper body (with the engine's water it lies behind him): to investigate (section 10).

### 9.5 Costs, validation, toggles, tests

* GPU (1920 x 1080, `sea_horizon`, `-PerfFrame 2400`): ocean simulation **0.122 ms (low) / 0.222 ms (medium) / 0.432 ms
  (high)**; the render pass with the refracted + alpha draws incl. the sea grid 0.129 ms (low) / 0.228 ms (high); wet beach
  0.025 ms. Total ocean cost < 0.7 ms at high (budget 2 ms) [C].
* Floor bake (worker, after parallelising the transforms / exposure and solving the travel time at half resolution):
  ~0.4 s for the 2048^2 field (distance 42, travel time 262, exposure 48, pack 24 ms measured offline on the cached
  heights); the in-game log line reports the stages.
* `-SyncValidation` with water + ocean quality high at `beach_sun` and `harbour_mouth`: no validation messages [C].
* Live toggles (`-Commands`: ocean gerstner / fft, ocean quality high / low, sea state storm, ocean off / fft, water off /
  on): 0 pipelines built at draw time, max frame interval 15.4 - 25 ms per 5 s window [C].
* rvk_tests 96 / 96 (new: `ocean_lgamma_series`, `ocean_spreading_is_normalised`, `ocean_jonswap_significant_height`,
  `ocean_spectrum_k_plane_integral`, `ocean_cascades_cover_the_spectrum`, `ocean_fft_matches_dft`,
  `ocean_height_field_variance`, `ocean_phase_speed_limits`, `ocean_floor_beach`, `ocean_floor_harbour_exposure`,
  `ocean_floor_cache_round_trip`, `ocean_sea_state_from_weather`).
* Water off: refcompare against the new stored reference `out\refcompare\20261022-093252` (20 spots): 18 PASS + 2 WARN
  (sea noise of the engine's animated sea), run `out\refcompare\20261022-094445`.

## 10. Limits / open issues

* The floor field only knows geometry the engine drew in the main view (cache accumulates it); unseen texels count as
  open, deep sea (the harbour basin's floor is partly unseen: its depth there is 30 m, its exposure is right). Instanced
  meshes (constant instancing) are not rasterised.
* The shore train is a single period and direction field; crossing seas at headlands fade (coherence) instead of
  interfering.
* The near-camera cap uses the camera, not the player (the follow camera is ~3-7 m behind; `rvk_swim` shows the swim
  line plausible from it). From a camera 1 m above the water next to a swimmer the engine's swim-splash particle is drawn
  as a flat bright patch over the FFT water (9.4): not understood yet.
* The beaches are 15 - 34 luma darker than the engine's painted surf (9.1); the colour of the open sea and the harbour
  matches within 5.
* The engine's wave-splash particle effect at the harbour quay (the "glittery patch" bottom-left at `harbour_docks`) is an
  engine particle system shown by both renderers (it is in the engine capture `out\water\20261022-083322\eng`); it is not
  part of the water and is left as the engine draws it.
