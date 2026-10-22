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
* 2026-10-22, the tuning pass (review of `out\water\20261022-102006\sbs_*.png`, approved by the user): (1) big far waves
  in every weather (the calm sea read as flat), the storm look kept; (2) the harbour calm but alive (chop, some swell through
  the mouth, reflections, slosh at walls and hulls) and as bright as the engine's; (3) surf that reads as surf (breaking crest
  lines, white water spreading into lacy residual foam, a bright swash edge with a wet, glossy sand line), not thin
  filaments; (4) brighter beaches: shallow water showing the lit sea floor; the near-water wave cap keyed to the player.
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
| surface: cascade sampling (vertex: explicit LOD by footprint; pixel: filtered + lost slope variance + macro slopes), attenuation, the near-water cap (player) | `shaders/water/ocean_surface.glsl` (included by `water_common.glsl`) |
| the floor field's sampling, the shore wave train and its surf terms as plain functions (shared by the draws and the foam pass) | `shaders/water/ocean_shore.glsl` |
| noise (pcg2d lattice hash) and the foam pattern: a thickness with its gradient (warped fractal sheet + round holes of three sizes, soft, filtered per pixel axis) | `shaders/water/water_noise.glsl` (`foamField`, `foamDensity`) |
| the foam's shading (lit relief, wrapped sun, translucency, shoulder), the waterline (thin-water Fresnel, soft edge) | `shaders/water/water.frag` |
| the water's clock: the engine's game time (`eCTimer::GetGameTime`), fixed steps under the time freeze, jumps followed (sea state snapped, histories reset, the foam buffer spun up) | `src/enh/water.cpp` (`seaClock`), `src/engine/weather.{h,cpp}` (`gameTimeMs`, `appState`), `src/enh/ocean.cpp` (`resetHistory`, the spin-up in `recordFoam`) |
| the surf's foam buffer: per-cell coverage over time around the camera (white water, residual foam, contact foam), advected | `shaders/water/ocean_foam.comp`, `src/enh/ocean.{h,cpp}` (`recordFoam`) |
| the player's position (`eCWeatherAdmin::GetPlayerPosition`) | `src/engine/weather.{h,cpp}`, `src/engine/symbols.h` |
| geometry (FFT path of the clipmap), shading (macro Fresnel, SSR on the macro normal, Toksvig-like spec, SSS, foam texture, surf milk) | `shaders/water/water_ocean.vert`, `shaders/water/water.frag`, `shaders/water/water_common.glsl` (`foamTexture`, `foamCover`, `waterWorley`) |
| the ocean in the water module: settings, sea state follower, the simulation pass (AfterOpaque), WaterBlock fields | `src/enh/water.{h,cpp}` |
| settings `water.ocean`, `water.ocean_quality`, `water.sea_state`, `water.wind_direction`, `water.wave_scale`; debug views `sea_floor`, `waves` | `src/core/settings.{h,cpp}`, `src/hal/vulkan/device.cpp` (`applyWater`) |
| spots `sea_cliff`, `sea_horizon`, `harbour_mouth`; motion sequences (`-Burst`, `-Interval` -> `motion_<spot>.png`) | `mods/rvkcompare/spots.lua`, `scripts/water-compare.ps1` |
| the sky cube redrawn whole every frame while the water is on (9.8); spot `beach_night_storm` (thunder, the clock running, only by name: `explicit`) | `src/engine/scene_hooks.cpp` (`detourSkyCube`), `src/d3d9/device.h`, `src/hal/vulkan/device.cpp` (`applyWater`), `mods/rvkcompare` |

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
* **Swell**: a second JONSWAP-shaped spectrum at its own period and direction (wind + 0.3 rad), narrow (`s` = 60:
  long-crested, from a distant storm), its `alpha` solved so its significant height is the preset's [C: test]. It is there in
  every weather (tuning pass): the calm sea carries a 1.5 m / 12 s swell under its small wind sea, so the open sea always
  shows big, long waves.
* `S(k) = S(w, theta) dw/dk / k` (deep water); the k-plane integral equals the 1D `m0` [C: test, within 2 %]; the
  smallest ripples damped by `exp(-(k * 1 cm)^2)`.
* Dispersion quantised to a repeat time of 256 s (Tessendorf): the animation loops, the time stays small (fp32 phase).

### 2.2 Sea states (`seaStateForLevel`, levels interpolated, fetch in log space)

| level | U10 m/s | fetch km | wind-sea Hs (JONSWAP) | swell Hs / period | choppiness | weather (auto) |
|---|---|---|---|---|---|---|
| 0 calm | 5.5 | 30 | 0.6 m | 1.5 m / 12 s | 0.8 | |
| 0.6 | | | | | | `Sunny_Full` |
| 1 moderate | 8 | 60 | 1.3 m | 1.7 m / 12.5 s | 0.9 | |
| 1.2 | | | | | | `Sunny_Half_*`, clouds |
| 2 rough | 11.5 | 110 | 2.6 m | 2.0 m / 13 s | 1.1 | rain |
| 3 storm | 15 | 180 | 4.3 m | 2.5 m / 14 s | 1.25 | `Stormy_*`, thunder |

(Before the tuning pass: calm 5 m/s, 25 km, swell 0.8 m / 10 s; storm swell 2.2 m / 13 s; swell `s` 40.) Measured totals
(log `enh: ocean: sea state: Hs ...`): storm Hs 4.60 m (wind-sea peak 118 m) [C]. `water.wave_scale`
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
* attenuated per cascade by the sea floor field (section 4) and capped around a swimming or wading **player**
  (`nearCameraCap`, 30 units within 700 units of him, full from 3,000 units). The player's position is the one the game
  hands the weather (`eCWeatherAdmin::GetPlayerPosition`, Engine.dll 0x713ea0, read once per frame, SEH-guarded) [C: at the
  spots it is the player's spawn point (-38,559 / 15 / -5,770) while the camera is elsewhere]; the cap is full while his
  origin is >= 150 units below the sea level (swimming: -250), partial while he wades, off while he stands on the beach (the
  surf rolls in in front of him). Without the symbol: around the camera when the camera is 2 .. 7 m above the sea, as
  before. The pixel shader caps only the two long cascades' slopes the same way: the short waves' normals stay, so the
  flattened water near a swimmer still shows its chop;
* plus the shore wave train and the M7 lowered, swashing waterline (now in phase with the train: the run-up arrives with
  the crests);
* towards the far plane (60 .. 97 % of the engine's far; 45 .. 90 % before the tuning pass) the displacement fades to the
  mean level: the long swell still lifts the horizon line a little, the far plane cuts no jagged silhouette (the coarse
  levels take the short waves from the mips), the shading keeps the waves' normals.

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
  remapped (an open beach sees ~half of them -> 1, a basin behind a narrow mouth -> ~0), blurred once, then **diffracted**:
  the exposure spreads from open water into sheltered water through the water only (geodesic, x 0.8 per cell, 16 rounds of
  an 8-neighbour max), so a harbour gets calmer the further in from its mouth (tuning pass; +10 ms per bake);
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
* **shelter**: `mix(1, exposure, (0.72, 0.6, 0.3, 0.1)[c])`: a closed harbour basin keeps 28 % of the long swell (more near
  its mouth: the diffracted exposure) and most of the short wind chop (before the tuning pass 0.85 / 0.75 / 0.45 / 0.2: the
  harbour read as dead flat);
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

### 4.4 Surf, foam, swash (rewritten in the tuning pass)

**Coverage over time: the foam buffer** (`ocean_foam.comp`, `Ocean::recordFoam`, AfterOpaque after the simulation): a
camera-centred window of 1,024 x 1,024 cells of 24 units (245 m), addressed toroidally (cell c in texel c mod N: the window
follows the camera without copies; cells entering it start from an estimate of the steady state), two RGBA16F images
(this frame's, last frame's = the history; 16 MiB, created with the tiles). Per cell and frame:

* the history, read bilinear through a wrapping sampler **advected** back along the drift (towards the beach: 20 units/s,
  110 in the surf zone) and spread a little (4 neighbours mixed in at 0.6 / s);
* **white water** (r): set by a crest plunging where it breaks (the train's breaking x its crest front x a segment noise that
  **peels along the crest**: advected along the crest by a two-phase flow map) and by the bores rolling on across the surf
  zone (weaker the further past the break), decays at 2.2 / s;
* **residual foam** (g): gains part of what the white water loses (16 % calm .. 45 % storm), dissolves at 0.28 / s (calm) ..
  0.12 / s (storm), faster outside the surf zone;
* **contact foam** (b): within ~0.5 .. 1.5 m of walls, rocks and hulls (the floor field's signed distance), pulsing with a
  slosh at the train's period, out of phase along the wall.

The shading turns coverages into foam with one **pattern** attached to the water (`water_noise.glsl foamField /
foamDensity`; the foam pass of 2026-10-22, 9.7, replaced the binary "sheet minus holes" mapping, which read as paper
cut-outs at the beach and as hard white "cow spots" from the cliff; rejected before that: cell borders - a hexagon net,
ridged-noise filaments - cracks / lightning, `out\water\20261022-102006\sbs_beach_sun.png`):

* **One coverage, a thickness, not a cut-out.** The terms (below) are combined as a union (`1 - prod(1 - c_i)`, white water
  weighted `0.55 + 0.45 c`, residual 0.7, contact 0.85, streaks 0.4) with a *freshness* (the share of white water and
  contact foam: fresh foam is thicker, with smaller holes). The pattern returns a **thickness** `h` (0..1) and its
  gradient: a warped 4-octave fractal sheet through a logistic (about uniform, so the coverage is the foam's share)
  thresholded *softly* (a band 0.14 .. 0.26 wide, wider for thin, old foam), times round holes of three sizes (cells of
  48 / 17 / 7 units, each hole with its own size 0.6 .. 1.25, soft rims; large in old foam so a lace is left, small in
  fresh white water), times a thickness variation (bubbles of different sizes); the coverage itself varies slowly over
  the water (x 0.5 .. 1.5 at 900 units: no large uniform areas). Opacity `1 - exp(-3.2 h)`: dense where thick, a
  translucent veil where thin.
* **Filtered per pixel axis** (the views are grazing: one pixel spans ~1-2 units across and 10-20 units along the view).
  `foamDensity` gets the screen derivatives of the pattern position: every transition (the sheet's threshold, each hole
  size's rim) is at least ~2.4 pixels wide *along the screen* (`|grad . dX|`, `|grad . dY|`), so the squashed pattern has
  soft edges instead of hard lines and no sub-pixel dashes; the fractal sheet's octaves fade by the pixel's longest extent,
  the holes keep their detail along the shortest (and vanish when thinner than ~2 pixels along the longest: they flickered
  as dashes); beyond 30 .. 90 units per pixel the mean thickness.
* **The pattern drifts with the water** as the foam buffer advects the coverage (towards the beach, 20 .. 110 units/s):
  a two-phase flow map (period 6 s, offsets +-3 s), switched region by region (a noise spreads the switch over ~3 s: no
  global pulse); only the ~15 % of pixels in a switch evaluate the pattern twice. (Rejected: stretching the sheet along the
  flow for streaks - a rotation that varies over the water, applied to world coordinates, turned into fine moire stripes.)
* **Lit, shaded, translucent.** Foam is a scattering layer: the engine's own light (its 9-coefficient ambient + the sun,
  its `^0.45` curve) on the foam's **relief normal** (the thickness gradient x 9 / (1 + footprint / 4), slope clamped to
  0.7: thick lumps brighter on the sun's side, their lee darker), the sun **wrapped** (0.45: light through the bubbles),
  the ambient occluded in thin parts (0.72 .. 1), albedo 0.5 (thin) .. 0.78 (thick), neutral (the sun's warm colour made it
  read as sand), thin foam tinted a little (12 %) by the sea's own colour; at least 1.3 x as bright as the water under it
  (at grazing views the water mirrors the bright sky and in a storm the light is dim: foam darker than the water read as
  dirt); a **shoulder** above 0.55 towards ~0.9 (the scene target is shown without a tone curve: lit foam clipped to flat
  white); fogged like the surface.
* **Milk** (the aerated surf water) at 45 % (was 70 %).
* **Cost**: the noise's lattice hash is now the integer pcg2d (Jarzynski and Olano 2020) instead of `fract(sin(...))`
  (also for the ripples, the swash phase and the foam pass: a different random field, the same statistics); octaves and
  holes below the visibility threshold are skipped, the warp is one octave.

Per pixel on top: the open sea's **whitecaps** (the tiles' temporal Jacobian foam), the **swash zone** (the last metre of
water before the beach: always some broken water), **sheltered streaks** (harbour basins, coves inside the engine's coast
overlay, low exposure: faint drifting scum lines along the wind, where the engine painted its moving foam), the **swash
edge** (the water's last centimetres on the beach or at a wall: 15 units deep and dense while the swash runs up, 5 units
and lacy while it drains back, in phase with `water_ocean.vert`'s swash). Outside the buffer's window the same terms are
evaluated in place (no history). **Milk** (the aerated water of the surf zone) mostly right behind the
breaking crests, clearing between the bores.

**The wet sand** (`wet_shore.frag`): besides the M7 darkening up to the swash's reach, where the FFT ocean's swash has just
drained away (the time since the water uncovered the point, from the swash's phase: `pi - asin(y)`), the sand carries a
film of water: darker (x up to 0.6) and **glossy** (the sky cube's reflection x Schlick Fresnel, strong at grazing views),
drying within ~1.6 s (blend changed to `dst * src.a + src.rgb`; the sky cube via `ImageRole::SkyEnvCube`). The film's
front is soft (`smoothstep` over +-0.12 of the swash's phase; foam pass): the hard `sin(phase) > y` test drew a jagged line
of dark and white specks along the sand's height lines where the wave grid's own (interpolated) waterline does not lie on
it - the "stair steps" at the beach_sun waterline.

**The waterline** (foam pass): the thin water over the sand is a broken, rough film rather than a mirror: the Fresnel
reflection and the specular fall to 30 % below 3 units of vertical depth, full from 35 units (the grey, sky-mirroring band
along the beach); the soft edge where the water meets the ground (the depth map) is at least ~2 pixels wide on screen
(`2 x fwidth`, 12 .. 30 units; wider let engine seaweed props near the surface show through with dark halos); the
swash's rim (contact foam, dense on the uprush, lacy on the backwash) goes through the same pattern and shading.
(Rejected: fading the water within a few pixels above a beach crest that hides the waterline - it put dark halos above
every rock at the water.)

### 4.5 The water's clock (foam pass, `Water::seaClock`)

All of the enhanced water's time (the FFT's `t mod 256 s`, the shore train, the swash, the foam buffer, the wet sand, the
noise drift; `WaterBlock::grid.w`) is one clock that follows the **engine's game time** (`eCTimer::GetGameTime`, Engine.dll
0x4ceee0 via `GetInstance` 0x4ceef0, ms; bound by name, SEH-guarded) [C]: per frame it advances by the game time's step;
while the game time stands still it steps on by **1 / 60 s per frame under the time freeze** (`eCApplication::GetAppState`
0x4c1ad0 == 8) and by the real time otherwise (loading screens, menus); a jump (< 0 or >= 1 s, e.g. rvkcompare's
`set_game_time` two frames before its freeze, a loaded save) is followed at once. On a jump (and the first frame with
water) the sea state snaps to the weather's (no 3 s follow-up: the spot before a capture no longer leaks into it), the
tiles' temporal Jacobian foam is cleared and the **foam buffer is spun up** over the last 10 s of the clock (41 steps of
0.25 s in one frame, ~2.5 ms GPU once: 41 x the ~0.06 ms step, estimated; also when the sea comes into view, after a
pause or a teleport): the surf shows its steady state at once instead of fading in. rvk.log: `enh: water: clock from the engine's game time`, `clock follows a game
time jump of ...`, `clock in fixed steps under the time freeze (app state 8)`, `foam buffer spun up over 10.0 s`.

Measured [C]: two identical `water-compare` runs (beach_sun, sea_cliff): the open sea is pixel-identical; what still
differs is the engine's (its splash particles at the rocks, the clouds and the eye adaptation, which run on real time) and
the foam's sub-pixel phase (the two real frames between `set_game_time` and the freeze): mean |diff| 2.3 / 3.0 of 255
(before: 7.2 / 4.6 and the sea state depending on the spot before). Luma numbers are therefore comparable within ~+-5 at
the beaches (engine exposure + particles).

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
* **The swell's bands** (tuning pass): the Fresnel weight's surface takes the longest cascade (the swell) at 85 % of its
  tilt, the second cascade and the train at 40 % (masking): the long swell reads as light and dark bands towards the horizon.
* **Shallow water shows the sea floor** (tuning pass): the transparency of the FFT ocean follows the light's real way through
  the water (refracted down to the floor and back, `T = exp(-0.0011 murkiness path)`), not the view ray's length (the
  engine's depth fade turned even 1 m of water opaque at grazing views): `colour += (1 - F) T (refraction - body)`, so what
  is not reflected shows the lit floor through shallow water (tinted along the path) and the material's body colour where
  it is deep; deep water is the engine's model unchanged.
* **Wind ripples** (tuning pass): 3 octaves of drifting gradient-noise slopes (170 units) on top of the cascades, weak on
  the open sea, stronger in sheltered water (gusty); they also break up the traced reflections of walls and hulls there.
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

**Tuning pass** [C] (`beach_sun`, 1920 x 1080, vsync off, water quality high, ocean quality high, `-PerfFrame 2200`): the
`water.ocean` pass (simulation + the foam buffer, 1,024^2 cells) **0.492 ms** (the simulation alone was 0.432: the foam pass
~0.06 ms), the wet beach 0.026 ms, the render pass with the refracted + alpha draws incl. the sea 0.228 ms: the ocean
**~0.75 ms** of a 3.0 ms GPU frame (budget 2 ms). Memory: + 16 MiB (the foam buffer). The floor bake's exposure stage + 10 ms
(diffraction). The foam pattern is evaluated only where some coverage is > 0.004.

**Foam pass** [C] (`beach_sun`, 1920 x 1080, vsync off, water + ocean quality high, `-PerfFrame 3400 / 3500` in a
one-spot rvkcompare run, the view at the beach): `water.ocean` 0.51 ms, wet beach 0.025 ms, the render pass with the
refracted + alpha draws incl. the sea **1.23 .. 1.47 ms** (the same frame with the code before the foam pass: 1.06 ms; with
the foam evaluation switched off: 1.06 ms): the foam costs **~0.2 .. 0.4 ms** there, the ocean **1.77 .. 1.9 ms** in total
(budget 2 ms; the tuning pass's 0.228 ms for that render pass was measured at another frame / moment: the same frame
measures 1.06 ms with its code). The spin-up of the foam buffer (41 steps) runs once per clock jump / view of the sea.
The pcg2d hash and skipping invisible octaves did not change the numbers measurably (within the run-to-run spread).

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

### 9.6 Tuning pass (2026-10-22)

Side by side + motion (`scripts\water-compare.ps1 -EngineDir / -MaskDir out\water\20261022-085233 -Burst 6 -Interval 6`):
**`out\water\20261022-124354\`** (`sbs_<spot>.png`, `motion_<spot>.png`); earlier iterations of the pass in
`20261022-121656` (first: foam far too dense), `122017`, `122428`, `122753`, `123059`, `123347`; the foam debug view
(`water.debug=foam`: green white water, blue residual, red whitecaps, grey contact) in `20261022-122222`.

| spot | engine luma | before (9.1) | tuning pass | d | engine hue | now | spread over runs 5-7 |
|---|---|---|---|---|---|---|---|
| sea_cliff | 86.1 | 85.8 | 103.3 | +17.2 | 180 | 183 | +12.4 .. +17.2 |
| sea_horizon | 73.3 | 73.0 | 72.3 | -1.1 | 164 | 166 | -0.3 .. -1.1 |
| harbour_mouth | 59.4 | 54.6 | 58.2 | -1.2 | 160 | 168 | +0.7 .. -1.6 |
| harbour_docks | 39.2 | 36.5 | 41.5 | +2.3 | 114 | 118 | -0.9 .. +2.3 |
| beach_sun | 132.3 | 98.7 | 130.1 | -2.2 | 99 | 113 | -1.8 .. -15.9 (mean of 5 runs -7) |
| beach_rain (storm) | 60.7 | 45.9 | 52.1 | -8.5 | 192 | 192 | -6.9 .. -8.5 |

(The spread: the ocean's time is not pinned by the scenario: the phase of the bores, the swash and the foam differ per
capture, as do the engine's own splash particles; the frame's lighting was the same in every run: sand 55.3 .. 56.2, engine
56.1. A sanity run after the last refactor, `20261022-124946`: beach_sun -15.9 in a phase between the bores, harbour_mouth
-1.3.) sea_cliff by screen band (run 5,
engine / ours): the sky-near sea 161 / 164, the open sea 62 / 58, the shelf 46 / 69 and 48 / 97: the open sea matches, the
difference is the surf on the shelf (white water + lace where the engine shows dark rock under its two painted foam
blobs) and the shallow shelf now showing its floor; accepted (the user asked for surf that reads as surf, the colour of the
sea itself matches).

Looked at, per spot:
* **sea_horizon**: long swell rolling to the horizon in the calm Sunny_Half weather (before: almost flat), the bands of the
  swell lit differently, the horizon line lifted a little by the swell; the storm look unchanged in character.
* **sea_cliff**: crest lines breaking along the shelf's edge (segments peeling along the crest), white water behind them
  spreading into lace with round, torn holes, the shelf turquoise over its rocks, the open sea with big swell.
* **beach_sun / beach_rain**: breaking crest lines and bores rolling in, residual lace drifting towards the beach, the swash
  edge running up and draining with a glossy film on the sand; the shallow water shows the lit sand (beach_sun -2 .. -16
  instead of -34); beach_rain still darker than the engine's bright painted overlay (-7 .. -9 instead of -15).
* **harbour_mouth / harbour_docks**: residual swell and chop in the basin, faint drifting scum streaks, a slosh line at the
  walls and hulls, the traced reflections of walls and hulls broken up by the ripples; luma within 2.5. The bright splash at
  the quay walls in some captures is the engine's wave-splash particle effect (section 10).
* **Swimming** (`rvk_swim`, `out\water\swim2\`): the cap now follows the player (logged at the spots: player -38,559 / 15 /
  -5,770 while the camera is elsewhere; in the sea the hero at -43,500 / -250 / -8,200): the waterline at his chest as
  before; from the camera 1 m above the water the near foam showed round "cheese" holes and a beige tint (sand through
  translucent foam): the holes got their own warp, the sheet's edge was sharpened, the foam colour made neutral.

Checks: `-SyncValidation` with water + ocean quality high at beach_sun and harbour_mouth: no validation messages; live toggles
(`rvk_shadows -Commands`: water on, ocean gerstner / fft, ocean quality high, sea state storm, water off / on): 0 pipelines
built at draw time, max frame interval 13.4 - 25.3 ms per 5 s window except the window of the session's first enable
(48 ms: the first floor-field upload and, at the time, the foam buffer's allocation, which now happens with the tiles);
rvk_tests 96 / 96; water off: refcompare against `out\refcompare\20261022-093252`: run `20261022-124228` 16 PASS + 2 WARN
(beach_sun, sea_cliff: the engine's animated sea) + sea_cliff FAIL / lava_cave INVALID (pose 4.4 > 4) while two other game
sessions ran on the GPU (the engine's timed surf overlay at the rocks in another phase); re-run alone
(`20261022-124747`, `-Spots sea_cliff,lava_cave,beach_sun`): sea_cliff WARN, beach_sun WARN, lava_cave PASS = the known
state.

### 9.7 Foam pass (2026-10-22)

From the review of `out\water\20261022-124354\sbs_*.png` (approved by the user): beach_sun's surf read as flat, hard-edged
paper cut-outs with a grey band along the waterline, sea_cliff's shelf foam as hard white "cow spots". Iterations (all in
`out\water\`): `131605` (first: far too much, clipped white foam - the user: "way too bright", "too thick, can't even see
ground / water"), `131911` (thin, translucent: dull beige-grey sheets), `132306` / `132728` (the foam colour clipped to
white: the shoulder; per-pixel-axis edge widths), `133133`, `133546` (the wet sand's hard film front found and softened),
`134548`, `134824` (rejected: stretched streaks -> moire), `135347` / `135609` (holes along the shortest pixel extent),
`140635` .. `141849`, the final run **`out\water\20261022-145309\`** (`sbs_<spot>.png`, `motion_<spot>.png` = 6 frames 6
apart, single frames `enh\`; engine pictures + masks of `20261022-141017`, captured at the same pinned game times).

| spot | engine luma | tuning pass (9.6) | foam pass (final run) | spread over the last runs | engine hue | hue now |
|---|---|---|---|---|---|---|
| sea_cliff | 88.8 | +12.4 .. +17.2 | +7.6 | +7.6 .. +14.0 | 180 | 184 |
| sea_horizon | 73.3 | -0.3 .. -1.1 | +0.6 | +0.5 .. +0.6 | 165 | 166 |
| harbour_mouth | 58.4 | +0.7 .. -1.6 | +0.1 | -0.1 .. +0.1 | 160 | 168 |
| harbour_docks | 39.1 | -0.9 .. +2.3 | -0.1 | -1.7 .. +1.3 | 114 | 121 |
| beach_sun | 131.9 | -1.8 .. -15.9 | -5.0 | -9.0 .. +5.1 | 96 | 97 .. 139 (saturation 0.16: the hue is noisy) |
| beach_rain (storm) | 60.4 | -6.9 .. -8.5 | -6.0 | -3.2 .. -6.0 | 192 | 192 |

(The engine pictures of this table were taken at the pinned game times; the tuning-pass column's engine luma differed by
up to 2.5. The spread at the beaches is now mostly the engine's: its eye adaptation and its splash particles, 4.5.)

Looked at, per spot (crops at 1.2 - 5 x, consecutive frames):
* **beach_sun**: the surf is foamy, broken water: lace and streaks of soft-edged foam, denser in the white water behind
  the bores, thin veils between, the water showing between them; no clipped white; the waterline a soft, wet, dark edge
  with a foam rim; the jagged stair-step line along the waterline is gone (the wet-sand film's front).
* **sea_cliff**: crests breaking with peeling white water, the shelf covered by lace with holes of varied sizes, soft
  edges; the spray at the rocks kept (the engine's particles + white water); the open sea unchanged.
* **beach_rain**: one breaking line far out and residual foam bands; the storm's dim light makes the foam a little brighter
  than the water (>= 1.3 x); the inner surf zone carries little foam (the coverage, not the shading: `water.debug=foam`
  shows white water only on the outer break in this phase).
* **sea_horizon / harbour_mouth / harbour_docks**: unchanged in character, luma within 2.
* **Motion** (6 frames 0.1 s apart, and 4 consecutive frames): the foam drifts towards the beach with the water and moves
  with the waves; the pattern switches its flow-map phase region by region (no pulse). Consecutive frames at beach_sun
  differ ~4x more than before (mean |diff| 3.9 vs 0.95 of 255): the old foam had little detail; the pictures show coherent
  motion of the finer structure, no reshuffling; at grazing distances a few 1-pixel dashes remain at the edges of foam bands.
* **Swimming** (`rvk_swim`, `out\water\swim3\`): the waterline at the hero's chest as before; from the camera 1 m above the
  water the near foam is a soft lace with the floor showing (the "cheese" blobs of `swim2` are gone); the engine's
  swim-splash particle is unchanged (10).

Checks: `-SyncValidation` with water + ocean quality high at beach_sun and harbour_mouth (incl. clock jumps, history
resets and spin-ups): no validation messages; live toggles (`rvk_shadows -Commands`: water on, ocean gerstner / fft, ocean
quality high, sea state storm, water off / on): 0 pipelines built at draw time after start-up, max frame interval 40 ms in
the window of the session's first enable (was 48 ms), 13.9 - 25.6 ms in the others; rvk_tests 96 / 96; water off:
refcompare against `out\refcompare\20261022-093252`: run `20261022-144459` 19 PASS (beach_sun and sea_cliff PASS too) +
lava_cave INVALID (pose 7.5 > 4), lava_cave alone (`20261022-145028`) PASS.

### 9.8 Bug: at night one half of the sea bright, the other dark (the sky cube's refresh; 2026-10-22)

**Report** (user, a screenshot of the starting beach at night in a thunderstorm, third-person camera looking out to sea):
"the water sometimes is bright on one side and dark on the other" - a perfectly vertical, sharp seam near the screen centre
through the whole sea, from the horizon to the shore; left the dark night sea with its waves and foam, right a greyish-white,
smooth, mirror-like sea with a glowing band at the rocks; the sky above uniformly dark.

**Cause [C].** The water reflects the engine's sky environment cube (the ocean model's s12, the lake's s9, the river's s13;
the FFT ocean's SSR misses keep it). The engine refreshes that cube in `eCSkydome_PS::UpdateSkydomeEnvCubemap`
(Engine.dll 0x6e7ed0, called every frame from `eCIlluminationAdmin::Process`): `+0x3b8` accumulates `eCTimer`'s frame time
(`+0x2c`) against the period `+0x3bc` (10.0 s; game time, so nothing under the time freeze); when it expires, a global step
counter renders **one face per call**: +x, -x, +y, +z, -z in five consecutive frames, -y is not refreshed (decompiled;
logged face binds: faces 0, 1, 2, 4, 5 at frames n .. n + 4, every ~800 frames at 80 fps). Between two refreshes the cube
is up to 10 s old, and what the sky does during the five frames of a refresh stays baked into the faces drawn then:
* a **lightning flash** (thunder: each frame `Rand() < ThunderProbability` of the dominant weather state) during a refresh
  lights only the faces drawn in those frames; the water reflecting them is bright for ~10 s, up to a cube edge. The
  reflection's horizontal direction is the view ray's, and the +x / -z face edge lies on the diagonal x = -z: a camera
  looking along it (yaw ~225 - 230, exactly the beach's view out to sea) sees the edge as a vertical line through the
  screen centre - the seam. Reproduced: spot `beach_night_storm` (23:00, `Stormy_Rain` with thunder, the clock running,
  60 pictures 30 frames apart), run `out\water\20261022-152022\enh\enh_beach_night_storm_b24..b31.png`: a flash at b21 - b23,
  the refresh during it, then the right half 43 .. 91 luma brighter than the left (13 - 22) for the rest of the run; the
  user saw the same in that run's last frames.
* a **time jump** (sleeping, a loaded save, rvkcompare's `set_time`) leaves the whole cube showing the old sky for up to
  10 s: day -> 23:00 with 2 frames settled, the sea was 59 .. 86 luma (the day sky mirrored) instead of 12 .. 19
  (`20261022-151648\enh\enh_xn_225*.png`).
The engine's own water reflects the same cube (with the water off the effect is the engine's; its Fresnel at night makes it
less obvious: not separately confirmed); the enhanced water shows the reflection over most of the grazing sea.

**Fix.** While `water.enabled` is on, the device asks for the cube every frame (`d3d9::Device::setSkyCubeEveryFrame`, set
in `HalDevice::applyWater`): the `UpdateSkydomeEnvCubemap` detour (`src/engine/scene_hooks.cpp`) sets the timer past the
period and calls the original until the step counter wraps (the HAL counts the face binds in the SkyCube view: at most 5
calls; each is self-contained: material begin / end, the main camera restored), so the whole cube is redrawn in one frame,
all faces from the same sky. rvk.log: `scene: sky cube refreshed every frame (5 faces in this frame, the engine's period
10.0 s)`. A flash now lights the whole sea for the frames it lasts (the sea mirrors the lightning) and nothing is left
behind; after a time jump the cube is current in the next frame. With the water off the engine's own refresh is untouched
(parity). Rejected: hiding the stale cube in the shader (a brightness clamp or a cube average would also dull the honest
reflections of the day sky and lightning).

**Cost** [C] (`beach_sun`, 1600 x 900, `-PerfFrame 1500`): 5 render passes of 128^2 with 4 sky draws each, **0.053 ms GPU**;
the five original calls **~0.14 ms CPU** per frame (measured inside the detour over 600 frames).

**Verified.** Before / after sheet `out\water\night_seam\before_after.png` (top: after a flash during a refresh, before /
after; middle: the engine's water at night, a flash after the fix; bottom: the day -> night jump before / after). After the
fix (`out\water\20261022-152434\enh`, the same spot, 60 pictures): flashes light both halves alike (+30 .. +60 during the
flash), no lasting difference between the halves (the remaining +-5 .. 11 is the engine's wave-splash particle at the rock
right of the centre); the day -> night jump shows the night sea at once (`20261022-152616`). Standard spots at the default
quality (`out\water\20261022-153331`, engine + masks of `20261022-141017`): sea_cliff +8.0, sea_horizon +0.6, harbour_mouth
+0.1, harbour_docks +0.0, beach_sun -9.7, beach_rain -3.9 (9.7: +7.6, +0.6, +0.1, -0.1, -5.0, -6.0; the beaches' spread
-9 .. +5 / -3 .. -6), the look unchanged; at high quality `20261022-153038`. `-SyncValidation` with water + ocean quality
high at beach_night_storm (lightning) and beach_sun: no validation messages. rvk_tests 97 / 97. Water off (the refresh is inactive then):
refcompare against `out\refcompare\20261022-093252`: run `20261022-153751` 18 PASS + 2 WARN (harbour_mouth: the engine's
animated sea; hero_closeup psnr 37.7); re-runs: harbour_mouth PASS (`20261022-154300`), hero_closeup FAIL there (the
hero's idle pose and the compass needle) and PASS in `20261022-154439`.

**Night in general** (before the fix but with a current cube, 23:00 `Stormy_Rain` without thunder, yaw 208 / 238 / 268 and
from 15 m up, `out\water\20261022-151111\sbs_xn_*.png`): the enhanced sea is much darker than the engine's (water luma
16 - 28 vs 54 - 71). The open sea far out is close (engine 16 - 19, ours 13 - 15 in the band below the horizon); the
difference is the surf: the engine's painted, additive shore overlay glows white along the beach at night as by day, ours
is foam lit by the night's ambient + moon (>= 1.3 x the water). The physically plausible night sea is the dark one (the
sky above is 6 - 9); the bright right half of the report was the day / lightning sky mirrored, not a night look.
`water.reflection_strength` (0 .. 2) scales the reflection, but at night the cube is dark, so it brightens little; a
deliberate "moonlit" night look would be a new tunable (not done: a matter of taste).

## 10. Limits / open issues

* The floor field only knows geometry the engine drew in the main view (cache accumulates it); unseen texels count as
  open, deep sea (the harbour basin's floor is partly unseen: its depth there is 30 m, its exposure is right). Instanced
  meshes (constant instancing) are not rasterised.
* The shore train is a single period and direction field; crossing seas at headlands fade (coherence) instead of
  interfering.
* The near-water cap follows the player (9.6, tuning pass). From a camera 1 m above the water next to a swimmer the
  engine's swim-splash particle is drawn as a flat bright patch over the FFT water (9.4): not understood yet.
* The beaches (9.7): beach_sun -9 .. +5 luma against the engine (the engine's eye adaptation and particles run on real
  time), beach_rain 3 - 6 darker (the inner surf zone carries little foam in the storm: the coverage terms, not the
  shading), sea_cliff 8 - 14 brighter (its surf and the visible shelf). The open sea and the harbour within 2.
* Foam pass (9.7): the foam costs ~0.2 - 0.4 ms at the beach at 1080p (the ocean ~1.8 - 1.9 ms of the 2 ms budget); a few
  1-pixel dashes at the edges of foam bands far away at grazing views; no streaks along the flow (a varying rotation of the
  pattern turned into moire; would need advected texture coordinates in the foam buffer); the open sea's whitecaps still
  come from the tiles' Jacobian only.
* Reproducibility (4.5): the water is a function of the engine's game time; what the scenario cannot pin is the engine's
  (eye adaptation, particles) and the two real frames between `set_game_time` and the freeze.
* The foam buffer covers 245 m around the camera; further out the surf terms are evaluated in place (no history: no trail,
  no drift); the residual foam has no notion of currents (it drifts towards the beach along the train's direction).
* The foam buffer's sources come from the shore train only (one period / direction field); the open sea's whitecaps keep
  the tiles' Jacobian foam.
* The sky cube (9.8) is refreshed every frame while the water is on (~0.05 ms GPU, ~0.14 ms CPU); the engine never
  redraws its -y face. The enhanced sea at night is much darker than the
  engine's painted, glowing surf (9.8, "Night in general"): plausible, but a matter of taste; no "moonlit" knob yet.
* The engine's wave-splash particle effect at the harbour quay (the "glittery patch" bottom-left at `harbour_docks`) is an
  engine particle system shown by both renderers (it is in the engine capture `out\water\20261022-083322\eng`); it is not
  part of the water and is left as the engine draws it.
