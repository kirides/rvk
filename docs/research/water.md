# Water: classification, the enhanced water pass, the sea's shore (M7, first piece)

What Risen's water is (materials, meshes, passes, the engine's own sea and coast), how the renderer recognises each body of
water, and the optional enhanced water (settings category "Water", player default on since 2026-10-23, off in the test baseline: overlay.md 2.3): the engine's own water materials evaluated in
GLSL (the same water) with traced reflections, a wave grid for the open sea with waves rolling in, a lower, moving shoreline
with surf foam and wet sand instead of the engine's painted surf (better). Builds on the draw tags (draw-tags.md), the frame phases (frame-phases.md)
and the GLSL pipeline (injection.md).

**M8 (2026-10-22): the open sea's FFT ocean, the sea floor field and the attenuation towards the shore and the harbour
are in docs/research/ocean.md** (`water.ocean = fft`, the default; the Gerstner sum below is `water.ocean = gerstner`).
Settings renamed there: `water.ocean_waves` -> `water.ocean` (off / gerstner / fft), `water.wave_height` -> `water.wave_scale`.

Legend: **[C]** confirmed (decompiled, disassembled, or measured in the running game), **[I]** inferred / proposed. RVAs are
Engine.dll unless marked. Dumps: `B:\DevDrive\risen-ghidra\rvk_water_dump.txt`, `rvk_water_sb_dump.txt` (SharedBase).

## 0. Requirements (user, 2026-10-21/22)

* Enhancements are optional and player-configurable (overlay, A/B), the plain renderer stays available (PLAN.md).
* **The open sea** (water connected to the end of the world) gets waves in the distance and **a lower, real shoreline**:
  Risen fakes its shore with a moving texture laid over the beach; we want volumetric water and a real shore.
* **Puddles, lakes, rivers and waterfalls never get shore waves, a lowered shoreline or sea waves.** Use what Risen itself
  knows about sea and coast as the signal; a name list only as an override.
* **The water fits its environment**: the swamp is not shiny white or blue but keeps its murky look; colours, murkiness,
  reflections, glints and foam follow the original material and the local sky / weather (never a fixed blue).

## 1. What exists

| piece | files |
|---|---|
| water kinds (`draws::Water`), name classification, records carry the body (`Record::water`, `replaced`) | `src/d3d9/draws.{h,cpp}` (tests `tests/draws_tests.cpp`) |
| material / mesh names (eCResource2 +0x40), cached per material, logged per new water material | `src/engine/scene_hooks.cpp` (`materialInfo`) |
| first draw of each water material x pixel shader: the engine's shaders, declaration, states, constants incl. g_ShaderData c6..c14 and the VS texcoord transforms c232..c241 (rvk.log `water: draw of material ...`), its shaders' D3D9 bytecode to `bin/rvk/cache/water/` | `src/d3d9/draw.cpp` (`logWaterDraw`) |
| survey only: `-RvkLogLevel trace` also names every opaque (Solid pass) material of the main view | `src/engine/scene_hooks.cpp` (`TagScope`) |
| draw replacement on the device (`DrawReplacer`: engine / skip / other shaders / procedural; the query carries the engine VS, the declaration, the blend state; 16 sampled textures), background pipeline builds for replacement shaders, `Buffer::readCpu`, `samplerFor`, `samplerOfSlot` | `src/d3d9/device.{h,cpp}`, `src/d3d9/draw.cpp` (`prepareDraw`), `src/d3d9/resources.cpp`, `src/vk/pipeline.h` (`contains`) |
| the water module: settings, sea detection, the sea mesh's texture mapping, coast collection, shore-field bake + texture, the per-draw `WaterBlock` (a DrawBlock + the engine's constants + ours), waterfalls, the wet-beach pass | `src/enh/water.{h,cpp}` |
| shore field bake (CPU, after GD3D11's ShoreField.cpp) | `src/enh/shore_field.{h,cpp}` (tests `tests/water_tests.cpp`) |
| the engine's water models by sampler layout (`engineWaterModel`), profiles (what is added per kind), the affine fit of the sea mapping | `src/enh/water_profile.{h,cpp}` (tests `tests/water_tests.cpp`) |
| GLSL: block + waves (Gerstner, shore waves) + ripples + shore helpers, the sea grid VS, the shading (the four engine models + reflections, volume, foam), the wet beach | `shaders/water/water_common.glsl`, `water_ocean.vert`, `water.frag`, `wet_shore.frag` |
| settings `water.*` (category Water) + `water.debug` (Debug) | `src/core/settings.{h,cpp}`, applied in `src/hal/vulkan/device.cpp` (`applyWater`) |
| survey scenario (water entities' boxes, a look at each), `find=` entity positions, `around=x;y;z` (17 pictures around a place) | `mods/rvk_water` |
| water spots `lake_jungle`, `waterfall_jungle`, `swamp_pond`, `swamp_camp` | `mods/rvkcompare/spots.lua` |
| engine vs enhanced side by side, `-Mask` (a `water.debug=normals` run: the water mask of each spot), luma / hue numbers | `scripts/water-compare.ps1` (-> `out\water\<stamp>\sbs_<spot>.png`), `tools/water_stats.py` |

## 2. The water of Risen [C]

### 2.1 Material names

`eCMaterialResource2` derives from `eCResource2` (ctor `0x68e700` calls `eCResource2::eCResource2`); `eCResource2 +0x40` is
a `bCString` holding the resource's file title (`GetFileTitleA` `0xa02e0` copies it; `GetFileName` `0xa03c0` appends the
extension from vtable `+0xe8`). `bCString` (SharedBase `GetText` `0xaf950`, `GetLength` `0x170d0`) is a `char*` whose `int`
length sits 8 bytes before the text. `eCMeshResource2` has the same field (mesh names). The hook on `eCSubMesh::Render`
reads them (SEH-guarded) for draws of the Refracted, Alpha and AlphaLate passes, once per material (re-read when the string
buffer changes); `-RvkLogLevel debug` logs every alpha-pass material.

### 2.2 The water list (survey `mods/rvk_water`, all water level-mesh entities of the world files, 2026-10-21)

| material | meshes (entities) | pass, states | kind |
|---|---|---|---|
| `Special_Water_Ocean_01_Diffuse_01` | `Levelmesh_Oceanmesh_01`: **one flat plane at y = 0, 672,672 x 572,545 units, 500 vertices** | Refracted, opaque, z write, 6 draws at the beach | Ocean |
| `Special_Water_Island_01_Diffuse_01` | `Levelmesh_Lakemesh_01/03/04/05/09` (lakes at y 5,213 .. 10,439, 2,500 .. 10,296 wide) | Refracted, alpha blend (global alpha 0.67) | Lake |
| `Special_Water_River_01_Diffuse_01` | `Levelmesh_Rivermesh_01..07` (05 at y 0: the river mouth) | Refracted, opaque | River |
| `Special_Water_Swamp_01_Diffuse_01` | `Levelmesh_Lakemesh_06` (a forest pond at y 6,658, 5,542 wide): the only swamp material | Refracted, opaque | Swamp |
| `Special_Water_Fall_01_Diffuse_01`, `Special_Water_Falls_01_Diffuse_01` | `Obj_Nat_Waterfall_Medium`, `Levelmesh_Waterfall_0001/0007/0100/0206/0300/0302/0403/0500/0605/0700` | **Alpha** (not refracted), blended or opaque | Waterfall |
| `Special_Water_Shore_01/02_Diffuse_01` | `Levelmesh_Ocean_Shore_01/02` (`Levelmesh_OceanShores.lrent`: strips along every sea beach, y -114 .. 134) | **Alpha, additive** (SRCALPHA / ONE), no z write | Shore (the fake shore) |
| `Arch_Obj_Bathtub_Water_01_Diffuse_01` | `Object_Interact_Waterbarrel` | Alpha | Other |
| `Special_Fog_Ground_01` | `Levelmesh_Lakemesh_02`, `Levelmesh_Lakemesh_07` | Alpha | **not water**: ground fog |

Refracted but not water: `Nat_Obj_Crystal_01_Diffuse_02` (crystals), `Arch_Obj_Window_Curch_01_Diffuse_01` (church window).
Water draws per frame: the opening beach 6 (the sea plane's sub-meshes), harbour 1-6, jungle river spots 1-9.

**Don's bandit camp** (`SP_Don` at -30,621 / 8,649 / 27,292) has **no water surface** [C, surveyed twice on 2026-10-22]:
`mods/rvk_water` `around=-30621;8649;27292` (17 pictures from 8 directions at 2,200 / 3,000 units, 8 looking outwards, one
from above) with every alpha / refracted material logged and, at `-RvkLogLevel trace`, every opaque one: the only
water-like draws near the camp are the ground fog `Special_Fog_Ground_01` (mesh `Levelmesh_Lakemesh_07`, Alpha pass, the
mesh's box covers the camp) over the landscape's **green mud** ground materials `Nat_Ground_GreenMud_01/02_Diffuse_01`
(`Levelmesh_Landscape_01`, opaque) with wooden walkways (`MiniLocation_Bridge_Swamp_02_L01`); no Refracted draw and no
water body (`enh: water: body ...`) was registered while the camera was there (the nearest are rivers 8,000+ units away).
The "swamp water" at the camp is the engine's muddy ground under fog: the enhanced water leaves it as the engine draws it.
The swamp water material exists once, the pond `Levelmesh_Lakemesh_06` (spot `swamp_pond`); the big marsh
`Levelmesh_Lakemesh_02` is ground fog as well. Spot `swamp_camp` now looks at the camp from the north-east over the walkways
(it used to look into foliage at point-blank range); the canopy still hides much of it from every direction surveyed.

### 2.3 The engine's water shaders (bytecode disassembled with `rvk_shadertool refdisasm`, constants logged per draw)

Each water material has its own `ge_default` pixel shader. Common to all four: the normal from two or three DXT5 normal
maps (x in alpha, y in green, `xy * 2 - 1`, z = sqrt(1 - x^2 - y^2), not normalised) in the VS's tangent frame (TEXCOORD4
normal, 5 tangent, 6 binormal = cross(N, T) * (1 - 2 COLOR0.r)); the lighting `pow(lerp(c14 indoor, SH9(N) [c15..c21],
COLOR1.w) + c22 * sat(N.L) * sat(4 Ngeo.L), 0.45)` (c22 = `g_DirLight` diffuse, HDR ~4 at noon, c24 = towards the sun);
a depth fade `pow((depthmap * far - viewW) * a + b, c)` (D3D9 pow: |x|); the refraction offset `nxy * strength` in screen
uv, rejected where the offset sample lies in front of the water; the radial fog (c3 colour, c4 settings); `lerp(refraction,
colour, alpha)`. Texture coordinates come from the VS's transforms of the mesh's texcoord (vs c232..c241, scrolled by the
engine: river flow). Models [C]:

| model (ps) | samplers | colour | alpha | constants (logged) |
|---|---|---|---|---|
| **ocean** `6e22a01a6cf0ac0a` | s0 sun shadow mask (.x, multiplies the lighting), s9 blend mask (tc1.zw), s10 x s11 diffuse (tc0.zw, tc1.xy), s12 sky cube, s13 / s14 normal maps (tc1.xy, tc0.xy) blended by the mask | `c6 * lit + spec + sat(lerp(diffuse * c8.y, cube, fresnel))`, spec = `sat(pow(R.L, 128 c8.w) N.L / N.V) * c23 * sat(cube * c8.x)`, fresnel = `c9.y (1 - N.V)^c9.z + c9.x` | `sat(fade * max(mask, fade))` (no Fresnel) | c6 (0.035, 0.035, 0.059, 0.1 = distortion), c7 fade (0.0007, 0, 0.3), c8 (0.42, 5, 0.42, 1), c9 (0.020, 0.980, 5); VS c233 (15, 15) mask tiling, c235 (100, 100), c237 (100, 50) normal tiling |
| **lake** `912d3cc09d934354` (cave variant `b47eb33da75f539d`: + 2 point lights c25..c32, s0 = their shadow mask, drawn opaque) | s9 sky cube, s10 / s11 reflection masks sampled by `R.xy * 0.5 + 0.5`, s12..s14 normal maps | `(c6 + sat(sat(cube) * s10.y * fresnel * c7.y * s11.y * c7.z * c6.w)) * c7.x * (lit + 1)` | `min(c10.y (1 - N.V)^c10.z + c10.x, sat(fade))`, x global alpha 0.67, **and blended** SRCALPHA / INVSRCALPHA | c6 (0.11, 0.15, 0.18, 0.5), c7 (0.6, 0.6, 7), c8 (0.6, 3, 5), c9 fade (0.01, 0, 1), c10 (0.1, 1, 2), c11 blends (0.3, 0.5), c12.w 0.05 distortion |
| **swamp** `2cd5ceec7eb11593` | s9 / s10 normal maps | `c6 * lit + spec * c9.rgb` (no reflection) | `min(c8.y (1 - N.V)^c8.z + c8.x, fade)` | c6 (0.149, 0.173, 0.118, 0.06), c7 fade (0.01, 0, 0.8), c8 (0.4, 1, 1), c9 (0.047, .., 0.5 blend), c10.w 0.2 (spec power 25.6) |
| **river** `11b69f874dfbacdb` | s9 whitewater scale (by `R.xy`), s10 / s11 whitewater blended by s12, s13 sky cube, s14 / s15 normal maps | `sat(lerp(sat(k * whitewater + c6), sat(cube), fresnel) * c6.w) * (lit + 1)` | `min(fade, COLOR0.a)` (the mesh's edge fade) | c6 (0.133, 0.137, 0.149, 0.5), c7 (0.5, 0.1 distortion, .., 5), c8 Fresnel (0.020, 0.980, 5), c9 fade (0.006, 0, 1) |

The sea's turquoise is its **diffuse textures** (`s10 * s11 * 5`, unlit), not c6; the lake's glitter is the sky cube masked
by two textures looked up with the reflection vector; the swamp reflects nothing. The first enhanced water used only c6
as an albedo and lost all of this (near-black harbour, grey sea, flat lake).

## 3. Which water is the open sea, which coast is the sea's [C]

**Signal chosen: Risen's own sea plane.** The engine has exactly one water surface that spans the world: the sea plane
`Levelmesh_Oceanmesh_01` (672,672 x 572,545 units, flat). The island's land lies within ~110,000 units; the largest inland
water is 20,833 wide. `enh::Water` measures each water body's world box once (its vertex buffer's CPU copy, transformed by
the draw's world rows c33..c35) and takes **a flat body wider than 200,000 units = the open sea** (rvk.log `enh: water: body
'...' ... -> the open sea (reaches the world's border)` / `inland water`). Lakes, rivers, swamp ponds, puddles and bath
tubs can never pass it, whatever their material is called; a material whose name says "ocean" but whose mesh is small is
treated as a lake. `kSeaOverrides` (empty) is the override list.

**Coast: Risen's own fake shore.** The scrolling surf overlay (`Levelmesh_Ocean_Shore_01/02`, `Special_Water_Shore_*`)
is laid along every sea beach and nowhere else: its placement *is* the engine's shoreline of the sea. Its triangles (all of
the mesh at its first draw: 1,709 + 1,526) are baked into the shore field (section 5).

Rejected:
* **Material names alone** (GD3D11's `OceanIdentifiers`): Risen's names are good hints (`Ocean`, `Island`, `River`,
  `Swamp`, `Fall`), but entity names mislead (`Levelmesh_Lakemesh_02/07` are ground fog) and a name says nothing about
  being connected to the world's edge. Kept for the kind of inland water and as the override.
* **Water zones** (`gCWaterZone_PS`, Game.dll, registered in `gCNavigationAdmin`): gameplay data (swim / wade / depth
  range, sub-zone polygons) per body, lakes have them too, no sea flag; only reachable after the world is loaded through
  Game.dll. Useful later for underwater detection (section 9).
* **A flood fill from the world border** (GD3D11-style geometric bake over all world meshes): the world's geometry is not
  available outside the draw stream; the sea plane test gives the same answer for Risen without it.

## 4. The enhanced water pass

### 4.1 Where: draw replacement, not a separate pass

The engine draws its water in the **Refracted pass, right after the refraction copy** and before alpha (frame-phases.md 3).
Instead of collecting the water draws and replaying them at an injection point, the device asks a **draw replacer** at each
water draw (`d3d9::DrawReplacer`, `prepareDraw`): draw it as the engine asked, **skip** it, draw **the same geometry with
other shaders** (the engine's vertex / index buffers and constants), or draw **procedural geometry** (`vkCmdDraw` without
buffers). The replaced draw keeps the engine's depth test / write and the exact place in the frame (after the refraction
copy, before particles and other alpha), so nothing has to be replayed and the engine's alpha draws sort against it as
before. The replacement swaps in its shaders, forces opaque blending and no culling for the draw, makes its inputs readable
before the rendering scope (up to 16 textures), points the push constant `draw` at its own `WaterBlock`, and restores
everything when `prepareDraw` returns. Records keep the draw with `replaced` set and the body (`water`).

**The engine's meshes keep the engine's vertex shader** (translated, unchanged): only the pixel shader is ours. Varyings are
matched by D3D9 semantic (`abi::varyingLocation`: TEXCOORDn = location n, COLOR0 = 16), so `water.frag` reads exactly what
the engine's water PS reads: the material's texture coordinates with the engine's scrolling (river flow), the tangent frame,
the world position, the global alpha. The `WaterBlock` **starts with a DrawBlock** (`tex[20]`, `smp[20]`, `posFix`, ...:
the engine VS reads its half-pixel fix there; the PS samples the material's textures through the engine draw's own slots
and sampler states, `Device::samplerOfSlot`), followed by the draw's constants (c0, c3, c4, c6..c14 g_ShaderData, c15..c21
g_AmbientLight, c22..c24 g_DirLight, point lights c25+ when the shader has them), the sea grid's mapping and ours. One
background pipeline build per engine water VS (registered at its first replaced draw; until it exists the engine draws).
**Unknown water shaders keep the engine's draw**: `engineWaterModel` recognises the four models by their exact sampler
layout and logs every new water pixel shader (`enh: water: material '...' (...): pixel shader ... -> model ...`).

| body | what happens with `water.enabled` |
|---|---|
| the open sea, `water.ocean` gerstner / fft | the first sea draw of the main view draws the **wave grid** (procedural), the other sea draws are skipped; the engine's **fake shore overlay is skipped** in frames with the grid |
| the open sea, waves off; lakes, rivers, swamp | the engine's mesh and VS with our pixel shader (the material's model) |
| a water material whose pixel shader is not one of the four models | the engine's |
| waterfalls | the engine's (their base becomes whitewater on a lake below: section 4.3) |
| crystals, glass, other refracting materials, bath tubs, ground fog | the engine's |
| no refraction copy / depth map bound (image effect off) | the engine's (logged once) |

### 4.2 The sea's wave grid (`water_ocean.vert`)

A camera-centred clipmap: 8 levels of N x N quads (N 48 / 64 / 96 by quality), spacing 25 units at level 0 doubling per
level (level 7 reaches ~1 km past the 600 m far plane), each level snapped to twice its spacing (vertices stay fixed in
the world while the camera moves: no swimming), the finer level's footprint left out of each coarser level (degenerate
quads), odd vertices on a level's outer edge averaged from their even neighbours (no T-junction cracks). 196,608 vertices
at medium. Height = the sea plane's level + **Gerstner waves** (8 waves, 240 .. 5,200 units long, deep-water dispersion,
wind angle 0.65 rad, largest amplitude 70 units at `water.wave_scale` 1), **damped near the coast** (shore field distance,
full height from 4,500 units out) and **near the camera** (capped to 30 units within ~7-30 m: the player wades and swims
in a calm sea; the roadmap's displacement cap). **Waves rolling in towards the coast** take over inside the coast band
(250 .. 5,000 units from the overlay): phase over the shore field's distance (crests parallel to the beach, moving towards
it), direction = the field's direction to the coast, 1,100 units long with a steeper front (a second harmonic), 25 units
high at `water.wave_scale` 1, faded where the field has no direction (coves, points). At the coast the waterline sits **13
units below the engine's** and **swashes** ±8 units (it never climbs above the engine's line, so nothing the engine kept
dry is flooded); the lowering is full at the engine's coast overlay and gone 500 units from it (quays and cliffs keep the
engine's level). Clip position from the engine's own c0..c3, so the depth test against the beach is exact.

**The grid hands `water.frag` the engine ocean VS's interpolators**: the sea mesh's texture coordinate is an affine
function of world x, z (least-squares fit over its 500 vertices at the first sea draw, `fitAffineXZ`; rvk.log `sea mesh
mapping fitted: u = 6.29593e-06 x ... (max error 0.0634)`; near the island the mesh maps exactly u = x / 150,000 + 0.517,
the fit's scale differs by ~6 %, invisible in the tiled textures), sent through the VS's transforms c232..c237; the
tangent and binormal sign (COLOR0.r) and the outdoor factor (COLOR1.w) come from the mesh's vertices; the normal is the
wave normal (slopes faded towards the mean far away). TEXCOORD7 carries the grid's own values (undisplaced xz, coast
distance, shore-wave weight), TEXCOORD5.w / 6.w the wave amplitude and crest steepness.

### 4.3 Shading (`water.frag`), linear HDR into the FP16 scene target

**The same water**: `engineSurface` = the draw's own model (section 2.3) ported instruction by instruction: normal maps,
diffuse / whitewater textures, sky cube, the 9-coefficient ambient with the indoor blend, the sun with the engine's
shadow mask (sea), point lights with their shadow mask (the cave lake), Fresnel, specular, depth fade, the refraction offset
and its rejection, the fog, `lerp(refraction, colour, alpha)`; the lake's blending over the scene is emulated (the
replacement draws opaque). Verified by drawing the engine's meshes with it and SSR off (`water.reflections=sky`,
`water.ocean_waves=false`): luma within 0.1-0.6 of the engine at lake_jungle and crystal_cave, beach_sun matching
(run `out\water\20261022-012956`).

**Better**, on top of that model:
* **Reflections**: wherever the engine reflects its sky cube (sea, lake, river), the reflection ray is traced in screen
  space first (GD3D11's D3D12 `Water.hlsl TraceWaterSSR` ported: quadratic steps, 4 binary refinements, thickness test;
  20 / 40 steps at medium / high). The ray follows a calmer normal (35 % of the detail normal: the normal maps' ripples
  would scatter it into noise), the hit's lookup is offset by the detail normal's deviation (the ripples break the
  reflected image up), a miss (off screen, through the sky) keeps the engine's cube. A traced dark wall may dim the cube's
  sky but never below 55 % (sea) / 85 % (lake, river: their cube is masked into glitter by the material and reads as the
  light on the water, also in caves). While the water is on, the engine's sky cube is redrawn whole every frame (the
  engine draws one face per frame every 10 s: lightning and time jumps left stale faces, ocean.md 9.8). The swamp, which reflects nothing in the engine, gets a dim reflection (Schlick
  Fresnel x 0.45, the cube at 60 %). `water.reflection_strength` scales the models' Fresnel terms.
  **Ray traced** (`rt.reflections`, M9 P3, rt-reflections.md 3.3): a compute pass before the water draws traces the mirror
  ray of each body's plane against the world's TLAS into a reflection image (screen reuse or the engine's material lighting
  with a sun shadow ray, the engine's fog, ray-cone mips); `traceReflection` looks it up at the pixel bent by the waves'
  macro tilt and the ripples (as SSR's jitter), SSR fills in where it has nothing (medium / high), floors 0.3 (sea) / 0.75
  (lake, river) x the outdoor factor, the glint suppressed where the world is reflected. Off screen cliffs, canopies,
  masts and houses are reflected with their textures.
* **The sea's waves**: the wave normal is the frame of the engine's normal maps; wind ripples (3 octaves of gradient
  noise with analytic derivatives, 420 units, 2.5 x choppier inside the engine's coast overlay: harbour basins, coves)
  perturb it, faded with distance.
* **Volume**: what lies under the sea is tinted along the light's way through the water (down to the ground and back
  along the refracted direction, Snell n 1.33): `exp(-(0.0016, 0.0007, 0.0005) x murkiness x path)`, before the engine's
  depth fade (shallow water turns turquoise over the sand, not grey).
* **Foam (the sea only)**, from smooth fields only (the refraction-free water depth and the shore field): breaker lines
  rolling in over the shallows (phase over coast distance + 6 x depth, a sharp front with a fading trail, within 1,800 units
  of the coast and above 180 units of depth), streaky broken water in the swash zone (3 .. 110 units deep), faint churn
  streaks in deeper water inside the overlay (harbours), a thin lace at the waterline and at walls (short view-ray
  thickness), crests of the large waves and of the waves rolling in. Foam lies on top of the water (composited after the
  engine's blend), white (the light's luminance, 25 % of its tint: the sun's warm colour made it look like sand) with albedo
  0.7 through the engine's lighting curve, fogged like the surface.
* **Whitewater** below waterfalls on lakes (the river material has its own whitewater art): the falls' bases (from their
  own boxes) within ~35 % of their width, at half strength.
* A soft edge over the last 5 units of water (no seam at the waterline).
* `water.debug`: shore field, depth, normals, reflection only, foam (red) over the surf zone (green) and depth (blue),
  `reflection_source` (rt.reflections: green screen reuse, blue shaded hit, yellow SSR, black the cube),
  `reflection_shaded` (the reflection with every RT hit shaded: the exposure match of shaded vs reused hits).

### 4.4 Profiles (`enh/water_profile.*`)

The look is the material's; the profile only scales what is added: `reflection` (the player's
`water.reflection_strength`), `extraReflection` (swamp 0.45, others 0), `foam` (sea 1, others 0), `shoreWaves` (sea 25 x
`water.wave_scale`), `murkiness` (`water.murkiness`: the sea's tint). Lakes, rivers, swamps never get foam, shore waves or
a lowered waterline (they are never the sea: section 3).

### 4.5 The wet beach (`wet_shore.frag`, injection point AfterOpaque)

With the lower waterline the sand the engine always kept under its water and painted surf is seen. A fullscreen pass
before the refraction copy (so the water's thin edge shows the same sand: no seam) multiplies the scene where the ground
(from the depth map) lies in or next to the engine's coast overlay (shore field distance < 150 .. 500) and below the swash's
highest reach + 22 units: up to 24 % darker, darkest just above the water, drying upwards. Only while the wave grid is
drawn (last two frames) and the shore field exists; its pipeline is created with the water module (no toggle hitch).
Cost 0.020 ms at 1080p.

### 4.6 Settings (category "Water"; live; A/B-eligible except the debug view)

| key | default | |
|---|---|---|
| `water.enabled` | off | the enhanced water |
| `water.quality` | medium | low: sky reflections only, grid N 48; medium: 20 SSR steps, N 64; high: 40 steps, N 96 |
| `water.reflections` | `ssr` | `sky` = the sky cube only |
| `rt.reflections` (category Ray tracing) | medium | with `ssr`: off = SSR; low / medium / high = the ray-traced reflection image (rt-reflections.md 3.3: low 1 ray / 2 x 2 px, cut-outs solid, no shadows, no SSR fallback; medium + alpha-tested cut-outs, sun shadow rays, SSR fallback; high 1 ray / px, alpha-tested shadows, reach 30,000); needs the ray tracing tier |
| `water.ocean` | fft | `gerstner` / `fft` (ocean.md): the sea's wave grid + lower coast + the fake shore off; `off`: the sea shaded on its own mesh, engine surf kept (was `water.ocean_waves`) |
| `water.wave_scale` | 1.0 (0..3) | offshore waves and the waves rolling in (was `water.wave_height`); FFT: `water.ocean_quality`, `water.sea_state`, `water.wind_direction` (ocean.md 7) |
| `water.murkiness` | 1.0 (0.25..4) | the sea's tint along the light's path |
| `water.reflection_strength` | 1.0 (0..2) | |
| `water.debug` | off | Debug category, NoAB: `shore_field`, `depth`, `normals`, `reflection`, `foam` |

**No hitch when toggled**: the water shaders exist from device creation; the grid's pipeline is built **in the background**
(`PipelineCache::prewarm`) as soon as the session's first water draw shows the formats (also with the feature off), the
pipelines of the engine VSs with our PS at their first replaced draw; until they exist the engine's water is drawn. The
wet-beach pipeline is created with the module. The first attempt (building at enable) cost 224 ms in one frame; with
background builds the toggles show no frame above the session's normal spread (section 8).

## 5. The shore field (`enh/shore_field.*`)

A top-down RGBA16F grid around the island: R the distance to the nearest coast texel (world units, capped 30,000), G the
overlay's height, BA the direction towards the coast (minus the gradient of the box-blurred distance). The coast = the
engine's shore overlay triangles rasterised top-down (texel centres inside the xz projection + the corners), the distance
by the Felzenszwalb-Huttenlocher transform (both from GD3D11's `ShoreField.cpp`, by the author of this project). Grid:
the coast's box + 6,000 units, 1,024 texels on the long side, cells >= 48 units. Measured [C]: **1,016 x 1,024 texels of
119 units at (-78,611, -69,779), 120,242 coast texels**, baked on a worker thread in ~70-130 ms after the first frame
with both overlays, uploaded as a managed texture. Inside the overlay strip the distance is ~0 (the overlay is the surf
zone); the breaker lines therefore use the water depth there. The overlay also covers **harbour basins and rocky coves**
(deep water; `water.debug=foam` at harbour_docks shows the whole basin inside it) [C]: the engine's moving foam texture is
what gives its harbour water the streaky look; the enhanced water puts churn streaks and choppier ripples there.

## 6. Underwater

Not in this piece. The grid is drawn without culling and the camera below the sea's level flags the draw (`WATER_UNDERSIDE`:
no reflection, plain absorption), but there is no underwater fog / absorption post yet. Plan: the water zones
(`gCWaterZone_PS::GetUnderWaterDepth` `0x947920`, sub-zone polygons, Game.dll) or the sea level + the shore field for the
sea, an AfterMainView post pass with the profile's absorption and scattering (section 10).

## 7. Costs [C]

`rvkcompare` `beach_sun` frozen for 1,500 frames, 1920 x 1080, vsync off, `-PerfFrame 3700`, quality medium (grid of
196,608 vertices, 20 SSR steps; the sea fills ~40 % of the screen; logs `%TEMP%\perf_water_{true,false}.log` of
2026-10-22): the render pass after the refraction copy (refracted + alpha draws) **0.199 ms with the engine's water, 0.357
ms with the enhanced water**, the wet-beach pass **0.020 ms**: **+0.18 ms GPU**, the whole frame 1.531 -> 1.707 ms. The
first piece cost +0.027 ms; the engine material's texture fetches, the per-pixel SSR on a calmer normal, the shore waves
and the foam noise are the difference. Well inside the 1.5 ms budget. CPU: one ring block (1,104 bytes) per water draw;
the body / coast / falls / mapping caches are built once per vertex buffer. Ray-traced reflections (`rt.reflections`,
1080p): medium +1.0 ms (harbour_docks) .. +1.6 ms (lake_jungle) GPU per frame, low +0.5 .. +0.95, high +1.35 .. +2.5
(rt-reflections.md 3.3).

## 8. Verification

* **Side by side** (`scripts\water-compare.ps1 -EngineDir out\water\20261022-011203 -Mask`, left engine, right enhanced,
  1600 x 900): **`out\water\20261022-022940\`** (`sbs_beach_sun.png`, `sbs_beach_rain.png`, `sbs_harbour_docks.png`,
  `sbs_lake_jungle.png`, `sbs_waterfall_jungle.png`, `sbs_swamp_pond.png`, `sbs_swamp_camp.png`, `sbs_crystal_cave.png`;
  `mask_<spot>.png` = the water pixels, `water_stats.json`). The first piece for comparison: `out\water\20261022-004703\`.
* **Water luma / hue** (`tools/water_stats.py`: mean Rec. 709 Y' of the 8-bit pictures over the water mask of a
  `water.debug=normals` run, saturation-weighted mean HSV hue):

  | spot | engine luma | enhanced | first piece | engine hue | enhanced | first piece | sat engine / enhanced |
  |---|---|---|---|---|---|---|---|
  | beach_sun | 131.4 | 113.2 (-18) | 133.8 | 103 | 87 | 102 | 0.18 / 0.18 |
  | beach_rain | 60.7 | 49.0 (-12) | 46.5 | 191 | 192 | 192 | 0.22 / 0.23 |
  | harbour_docks | 39.3 | 35.2 (-4)* | 29.0 | 114 | 121 | 127 | 0.24 / 0.24 (first piece 0.14) |
  | lake_jungle | 67.4 | 66.0 (-1.4) | 67.0 | 51 | 51 | 59 | 0.43 / 0.43 |
  | crystal_cave | 43.9 | 40.5 (-3.3) | | 149 | 152 | | 0.23 / 0.23 |
  | swamp_pond | 45.1 | 45.9 (+0.7) | 47.7 | 79 | 80 | 80 | 0.29 / 0.28 |
  | waterfall_jungle | 136.3 | 140.9 (+4.6) | 152.0 | 57 | 57 | 49 | 0.31 / 0.31 |
  | swamp_camp | no water (mask: 20 noise pixels) | | | | | | |

  \* run `20261022-021315`; in `20261022-022940` the enhanced capture caught the engine's wave-splash particle effect at the
  harbour wall (+9.4): a timed engine effect (not a water draw: no sub-mesh, no material; it shows in some captures of both
  wave modes, its timing follows the run's frame times). The sea's darker luma is the engine's painted surf (an additive
  overlay of bright foam bands, skipped with the wave grid) against our breaking-wave foam; hue 87 vs 103 = the clear
  shallows over the (wet) sand instead of the overlay's white-blue.
* Looked at, per spot: **beach_sun** turquoise sea with the engine's normal-map texture and colour, the rocks and the
  boat wreck reflected (SSR), waves offshore and rolling in, breaker lines and swash foam, a lower waterline over darker
  wet sand, no bright overexposed bands; **beach_rain** the grey stormy sea with dimmer surf than the engine's overlay;
  **harbour_docks** green-teal like the engine (was near black), reflections of the quay walls broken by the ripples,
  churn streaks, a lace of foam at the walls; **lake_jungle** the engine's ripples and glitter (cube through the
  material's masks) plus the trees reflected; **crystal_cave** the cave pool lit by the crystals' point lights with the
  engine's texture and glitter (was dark and flat; the variant with point lights is now a known model); **swamp_pond**
  murky olive as the engine, a dim reflection of the trees; **waterfall_jungle** the river's whitewater art (the engine's)
  and its flow; **swamp_camp** unchanged (no water: section 2.2).
* The engine model alone (`water.reflections=sky`, `water.ocean_waves=false`, run `20261022-012956`): beach_sun, lake_jungle,
  crystal_cave within 0.1 .. 0.6 luma of the engine and visually the same.
* Live toggles (`rvk_shadows`, `-Commands` water on at 1000 / off 1400 / on 1800 / quality high 2200 / waves off 2600 / on
  3000, `-Perf`): `0 pipelines built` at draw time in every 5 s window, max frame interval 17.4 .. 28 ms (the session's
  normal spread at 60 fps).
* `-SyncValidation` with `water.quality=high` at beach_sun, harbour_docks, lake_jungle, swamp_pond, crystal_cave,
  waterfall_jungle: no validation messages (the layer does not check bindless accesses: injection.md 6).
* rvk_tests 84 / 84, incl. `water_profiles_follow_the_material`, `water_engine_models_from_the_sampler_layout`,
  `water_affine_fit_of_the_sea_mapping`.
* `water.enabled` off: refcompare against the stored reference: section 8.1.

### 8.1 Regression with the feature off

M8 (2026-10-22): new stored reference `out\refcompare\20261022-093252` (20 spots, `swamp_camp` re-recorded), water off:
18 PASS + 2 WARN (sea noise), run `20261022-094445` (reference-compare.md 11.1). Before: stored reference
`out\refcompare\20261022-002054` (`-Calibrate`, 17 spots). Vulkan run with the water off (the default),
after this pass: **16 / 16 PASS** (beach_rain too), run `out\refcompare\20261022-022425`; `swamp_camp` left out (its pose
changed, so the stored reference's spots.lua hash differs: re-calibrate that spot with the next `-Calibrate`).
Earlier: 16 PASS + 1 WARN (beach_rain) in `20261022-003113`.

## 9. Limits

* Waves are a Gerstner sum + one shore-wave train (FFT cascades: M8); the shore field is unsigned (inside the overlay
  strip the distance is ~0) and only knows the engine's overlay, not the ground; shore waves could lift the water over very
  flat land within 250 .. 5,000 units of the overlay on its land side (not seen at the spots).
* The engine's painted surf brightened the beach more than our foam does (beach_sun -18 luma, beach_rain -12).
* The wet beach is a darkening, not a material change (no specular sheen of wet sand yet).
* Waterfalls keep the engine's shading.
* SSR has the usual screen-space limits (no off-screen geometry; RT reflections are M9); missed rays use the engine's sky
  cube also in caves (its glitter is the engine's look there).
* Unknown water shaders (other weather / quality variants of the materials) keep the engine's draw until a model is added
  (rvk.log names them with their pixel shader hash).
* No underwater post (section 6).

## 10. Plan: the rest of M7 and M8

* **M7 rest**: underwater fog / absorption post (water zones + sea level), area profiles (a table keyed by region / water
  zone, overlay-tweakable values), a sheen on wet sand, a water depth prepass (depth of the water surface for later passes),
  more engine models when variants show up.
* **M8 ocean: done (2026-10-22), docs/research/ocean.md**: FFT cascades (Tessendorf, JONSWAP + Hasselmann spreading + swell,
  sea state from the engine's weather) on the same grid; the sea floor field from the engine's static meshes (signed
  distance, depth, travel time to the shore, exposure) for the attenuation, shoaling, refraction-like turning, breaking and
  surf; the displacement cap near the camera at the water. Open there: ocean.md 10.
