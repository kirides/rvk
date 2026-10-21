# Water: classification, the enhanced water pass, the sea's shore (M7, first piece)

What Risen's water is (materials, meshes, passes, the engine's own sea and coast), how the renderer recognises each body of
water, and the optional enhanced water (settings category "Water", default off): our GLSL shading of the engine's water
draws, a wave grid for the open sea with a lower, moving shoreline instead of the engine's painted surf, and a profile per
body of water so each keeps its original look. Builds on the draw tags (draw-tags.md), the frame phases (frame-phases.md)
and the GLSL pipeline (injection.md).

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
| first draw of each water material: the engine's shaders, declaration, states, constants (rvk.log `water: draw of material ...`), its shaders' D3D9 bytecode to `bin/rvk/cache/water/` | `src/d3d9/draw.cpp` (`logWaterDraw`) |
| draw replacement on the device (`DrawReplacer`: engine / skip / other shaders / procedural), background pipeline builds for replacement shaders, `Buffer::readCpu`, `samplerFor` | `src/d3d9/device.{h,cpp}`, `src/d3d9/draw.cpp` (`prepareDraw`), `src/d3d9/resources.cpp`, `src/vk/pipeline.h` (`contains`) |
| the water module: settings, sea detection, coast collection, shore-field bake + texture, the per-draw `WaterBlock`, waterfalls | `src/enh/water.{h,cpp}` |
| shore field bake (CPU, after GD3D11's ShoreField.cpp) | `src/enh/shore_field.{h,cpp}` (tests `tests/water_tests.cpp`) |
| water profiles (look per body from the engine material) | `src/enh/water_profile.{h,cpp}` (tests `tests/water_tests.cpp`) |
| GLSL: block + waves + ripples + shore helpers, the engine-mesh VS, the sea grid VS, the shading | `shaders/water/water_common.glsl`, `water_mesh.vert`, `water_ocean.vert`, `water.frag` |
| settings `water.*` (category Water) + `water.debug` (Debug) | `src/core/settings.{h,cpp}`, applied in `src/hal/vulkan/device.cpp` (`applyWater`) |
| survey scenario (water entities' boxes, a look at each), `find=` entity positions | `mods/rvk_water` |
| water spots `lake_jungle`, `waterfall_jungle`, `swamp_pond`, `swamp_camp` | `mods/rvkcompare/spots.lua` |
| engine vs enhanced side by side | `scripts/water-compare.ps1` (-> `out\water\<stamp>\sbs_<spot>.png`) |

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

**Don's bandit camp** (`SP_Don` at -30,621 / 8,649 / 27,292) lies in `Levelmesh_Lakemesh_07`'s box, but that mesh is
**ground fog** (`Special_Fog_Ground_01`) over wet terrain: there is no water draw at the camp. The enhanced water leaves it
as the engine draws it (spot `swamp_camp` checks that); the swamp water material is the pond `Levelmesh_Lakemesh_06`
(spot `swamp_pond`). The big marsh `Levelmesh_Lakemesh_02` is ground fog as well.

### 2.3 The engine's water shaders (bytecode disassembled with `rvk_shadertool refdisasm`)

All water shaders are `ge_default` variants (different per material): `g_ShaderData` c6.rgb = the **water colour** (multiplied
by the lighting), c6.w = distortion strength; a depth fade `pow((depthmap * far - viewW) * cX.x + cX.y, cX.z)` (ocean and
swamp in c7: ocean `(0.0007, 0, 0.3)` = opaque after ~1,430 units of depth difference, swamp `(0.01, 0, 0.8)` = after
~100; lake / river in c9), a Fresnel term `bias + scale * pow(1 - N.V, power)`, a specular colour and power; the result
`lerp(refraction, colour, min(fresnel, depthFade) * globalAlpha)`. Lighting: `c6 * pow(ambient(N) + g_DirLight.diffuse *
N.L * shadowMask.r, 0.45)` + Phong specular. **`g_DirLight`**: c22 diffuse colour (HDR, ~4 at noon), c23 specular colour,
c24 the direction **towards the sun**. Fog (`ge_common.fxh RadialFog`): `f = saturate(min(dist * c4.y + c4.x, c4.z))`,
`lerp(colour, c3, f)`; c3 / c4 are 0 in fog-free weather. The shadow mask (s0, A8R8G8B8 screen space) is bound for the sea
only. Reflections: the sea samples a 128² cube (the sky environment cube, roles of frame-phases.md 4).

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
other shaders** (the engine's vertex / index buffers, its VS constants: world rows c33..c35, view-projection c0..c3), or
draw **procedural geometry** (`vkCmdDraw` without buffers). The replaced draw keeps the engine's depth test / write and the
exact place in the frame (after the refraction copy, before particles and other alpha), so nothing has to be replayed and
the engine's alpha draws sort against it as before. The replacement swaps in its shaders (with reflections that decide the
vertex inputs: POSITION0, NORMAL0, TEXCOORD0, TEXCOORD4; and the constant uploads: all 256 VS registers), forces opaque
blending and no culling for the draw, makes its inputs readable before the rendering scope, points the push constant
`draw` at its own `WaterBlock`, and restores everything when `prepareDraw` returns. Records keep the draw with `replaced`
set and the body (`water`).

| body | what happens with `water.enabled` |
|---|---|
| the open sea, `water.ocean_waves` on | the first sea draw of the main view draws the **wave grid** (procedural), the other sea draws are skipped; the engine's **fake shore overlay is skipped** in frames with the grid |
| the open sea, waves off; lakes, rivers, swamp | the engine's mesh with our shaders |
| waterfalls | the engine's (their base becomes whitewater on the water below: section 4.4) |
| crystals, glass, other refracting materials, bath tubs, ground fog | the engine's |
| no refraction copy / depth map bound (image effect off) | the engine's (logged once) |

### 4.2 The sea's wave grid (`water_ocean.vert`)

A camera-centred clipmap: 8 levels of N x N quads (N 48 / 64 / 96 by quality), spacing 25 units at level 0 doubling per
level (level 7 reaches ~1 km past the 600 m far plane), each level snapped to twice its spacing (vertices stay fixed in
the world while the camera moves: no swimming), the finer level's footprint left out of each coarser level (degenerate
quads), odd vertices on a level's outer edge averaged from their even neighbours (no T-junction cracks). 196,608 vertices
at medium. Height = the sea plane's level + **Gerstner waves** (8 waves, 240 .. 5,200 units long, deep-water dispersion,
wind angle 0.65 rad, largest amplitude 70 units at `water.wave_height` 1), **damped near the coast** (shore field distance,
full height from 4,500 units out) and **near the camera** (capped to 30 units within ~7-30 m: the player wades and swims
in a calm sea; the roadmap's displacement cap). At the coast the waterline sits **16 units below the engine's** and
**swashes** ±10 units with a phase running towards the coast (it never climbs above the engine's line, so no inland area is
flooded that the engine did not flood). Clip position from the engine's own c0..c3, so the depth test against the beach is
exact.

### 4.3 Shading (`water.frag`), linear HDR into the FP16 scene target

* **Normal**: the mesh normal (lakes, rivers: flat; slopes keep their tangent frame), the Gerstner normal on the sea (faded
  towards the mean normal far away against glitter), plus three octaves of gradient-noise ripples (Quilez noise with
  analytic derivatives) per profile, faded with distance.
* **Refraction**: the refraction copy at the screen position offset by the normal's perturbation (stronger in deeper water,
  weaker far away), rejected where the offset sample lies in front of the water (as `ge_default.fxh Refract`).
* **Volume**: the ground behind from the engine's linear depth map (`rvkWorldFromViewDepth`), the light's path inside the
  water along the **refracted** direction (Snell, n 1.33: ~1.5 x the vertical depth at a grazing view, not the long view
  ray), Beer-Lambert `T = exp(-absorb * path)`, in-scattering `scatter * pow(ambient + sun * L.y * shadow, 0.45)` (the
  engine's water lighting curve): `under = refraction * T + inscatter * (1 - T)`.
* **Reflection**: screen-space against the depth map, colours from the refraction copy (GD3D11's D3D12 `Water.hlsl
  TraceWaterSSR` ported: quadratic steps, 4 binary refinements, thickness test, foreground occluders skipped); a ray that
  leaves the screen through the sky returns the scene's sky there; a ray that leaves the screen **behind on-screen
  geometry** (forest canopy, cliffs) returns that geometry dimmed (no bright sky under trees); otherwise the engine's **sky
  environment cube** (the actual sky of the moment: weather, time of day). Fresnel Schlick F0 0.02 x the profile's
  reflectivity, weakened in very shallow water (wet-sand edge).
* **Sun glint**: GGX lobe with the profile's roughness, the engine's sun (c23 specular colour, c24 direction), its shadow
  mask where bound.
* **Foam** (the sea only): breaker lines in the surf zone (the coast overlay's band of the shore field, phase over the water
  depth so lines run towards the shallows), contact foam where the water is a few units deep (follows the swash up and down
  the beach), wave crests (Gerstner steepness); broad + fine noise, fine detail faded with distance; tinted by the water
  colour and lit like a diffuse surface (half-desaturated sun). **Whitewater** below waterfalls (any body): the falls' bases
  (from their own boxes) within ~35 % of their width.
* **Fog**: the engine's radial fog (c3 / c4 of the water draw) on the surface layer; the refracted part is already fogged in
  the copy: `transmitted + mix(layer, fogColour * layerOpacity, fog)`.
* A soft edge over the last 6 units of water path (no seam on the ground); `water.debug` views: shore field, depth,
  normals, reflection only.

### 4.4 Profiles: each body keeps its look (`enh/water_profile.*`)

The in-water colour is the **material's own colour constant** (c6.rgb: sea `(0.035, 0.035, 0.059)`, lake `(0.11, 0.15,
0.18)`, river `(0.13, 0.14, 0.15)`, swamp `(0.149, 0.173, 0.118)`), lit by the engine's ambient + sun through the engine's
curve. A per-kind table adds what the engine has no constant for, matched to the engine's depth fade:

| kind | absorption / unit (r, g, b) | turbidity | reflectivity | roughness | glint | ripples (strength, scale) | refraction | foam |
|---|---|---|---|---|---|---|---|---|
| sea | 0.0040, 0.0016, 0.0013 (clear: ~14 m) | 0.12 | 1.0 | 0.07 | 1.0 | 0.55, 700 | 1.0 | yes (tinted) |
| lake | 0.014, 0.009, 0.009 | 0.25 | 0.9 | 0.06 | 0.8 | 0.35, 420 | 0.7 | no |
| river | 0.012, 0.008, 0.008 | 0.25 | 0.85 | 0.10 | 0.7 | 0.65, 260 | 0.8 | no |
| swamp | 0.050, 0.034, 0.060 (murky: ~1 m) | 0.55 | 0.55 | 0.22 | 0.2 | 0.15, 520 | 0.3 | no |

Turbidity = how much of the lit water colour lies over what is seen through it (the engine's water always shows its colour,
also in the shallows; not over the last 30 units at the edge). The ground is attenuated along the vertical depth too (the
light reaching it through the water) and then along the refracted path. The engine's **point lights** of the draw
(g_PointLights c25 + 4 i, up to 4, when its pixel shader has them; falloff `1 - saturate(d^2 / range^2)` as
`AddPointLight`) light the water colour and add a glint each.

Foam colour = 0.82 x (0.8 + 0.2 x colour / max(colour)): never pure white. Player tweaks: `water.murkiness` (x absorption),
`water.reflection_strength` (x reflectivity and glint). Area overrides (a profile per world region or zone, e.g. from the
water zones' effect material) are the next step when a body needs one (section 10).

### 4.5 Settings (category "Water"; live; A/B-eligible except the debug view)

| key | default | |
|---|---|---|
| `water.enabled` | off | the enhanced water |
| `water.quality` | medium | low: sky reflections only, grid N 48; medium: 20 SSR steps, N 64; high: 40 steps, N 96 |
| `water.reflections` | `ssr` | `sky` = the sky cube only |
| `water.ocean_waves` | on | the sea's wave grid + lower coast + the fake shore off (off: the sea shaded on its own mesh, engine surf kept) |
| `water.wave_height` | 1.0 (0..3) | |
| `water.murkiness` | 1.0 (0.25..4) | |
| `water.reflection_strength` | 1.0 (0..2) | |
| `water.debug` | off | Debug category, NoAB: `shore_field`, `depth`, `normals`, `reflection` |

**No hitch when toggled**: the water shaders exist from device creation and their two pipelines are built **in the
background** (`PipelineCache::prewarm`) as soon as the session's first water draw shows the formats (also with the feature
off); until they exist the engine's water is drawn. The first attempt (building at enable) cost 224 ms in one frame; with
background builds the toggles show no frame above the session's normal spread (section 8).

## 5. The shore field (`enh/shore_field.*`)

A top-down RGBA16F grid around the island: R the distance to the nearest coast texel (world units, capped 30,000), G the
overlay's height, BA the direction towards the coast (minus the gradient of the box-blurred distance). The coast = the
engine's shore overlay triangles rasterised top-down (texel centres inside the xz projection + the corners), the distance
by the Felzenszwalb-Huttenlocher transform (both from GD3D11's `ShoreField.cpp`, by the author of this project). Grid:
the coast's box + 6,000 units, 1,024 texels on the long side, cells >= 48 units. Measured [C]: **1,016 x 1,024 texels of
119 units at (-78,611, -69,779), 120,242 coast texels**, baked on a worker thread in ~70-130 ms after the first frame
with both overlays, uploaded as a managed texture. Inside the overlay strip the distance is ~0 (the overlay is the surf
zone); the breaker lines therefore use the water depth there.

## 6. Underwater

Not in this piece. The grid is drawn without culling and the camera below the sea's level flags the draw (`WATER_UNDERSIDE`:
no reflection, plain absorption), but there is no underwater fog / absorption post yet. Plan: the water zones
(`gCWaterZone_PS::GetUnderWaterDepth` `0x947920`, sub-zone polygons, Game.dll) or the sea level + the shore field for the
sea, an AfterMainView post pass with the profile's absorption and scattering (section 10).

## 7. Costs [C]

`rvkcompare` `beach_sun` frozen for 1,500 frames, 1920 x 1080, vsync off, `-PerfFrame 3700` (a build before the last look
changes: point lights, turbidity; the sea fills ~40 % of the
screen): the render pass after the refraction copy (refracted + alpha draws) **0.072 ms with the engine's water, 0.099 ms
with the enhanced water (medium: grid of 196,608 vertices, 20 SSR steps)**: **+0.027 ms GPU**, the whole frame 0.880 ->
0.896 ms. High (grid 442,368 vertices, 40 SSR steps): 0.150 ms for that pass (+0.078 ms; measured while another game
ran on the GPU, the opaque pass was 1.4x slower too). CPU: one ring block (592 bytes) per water draw and two extra constant uploads; the body / coast / falls caches
are built once per vertex buffer. Well inside the 1.5 ms budget.

## 8. Verification

* Side by side (`scripts\water-compare.ps1`, left engine, right enhanced; 1600 x 900; final build):
  **`out\water\20261022-004703\`** (`sbs_beach_sun.png`, `sbs_beach_rain.png`, `sbs_harbour_docks.png`,
  `sbs_lake_jungle.png`, `sbs_waterfall_jungle.png`, `sbs_swamp_pond.png`, `sbs_swamp_camp.png`, `sbs_crystal_cave.png` (its
  left half = the stored D3D9 reference)); earlier iterations in `out\water\20261021-*` (`20261021-234737\eng\` = the engine
  pictures). Looked at: the sea
  reflects the sky and the rocks (SSR), the sand shows through the shallows with depth-dependent colour, the waterline is
  lower than the engine's with foam following it, no hard seams on the beach; the harbour keeps a teal, fogged look; the
  swamp pond stays murky olive with dull reflections; the lake reflects the forest instead of a white sky; the pool below
  the waterfall is whitewater; Don's camp is unchanged (fog, no water).
* Live toggles (`rvk_shadows`, `[debug] commands` water on / off / on / quality high / waves off / on): pipelines ready in the
  background before the first toggle, `0 pipelines built` at draw time, the 5 s windows' max frame interval 40-50 ms
  before and after the toggles (the background session's normal spread; with the old synchronous build: 241 ms).
* `-SyncValidation` with `water.quality=high` at beach_sun, harbour_docks, lake_jungle, waterfall_jungle, swamp_pond: no
  validation messages (the layer does not check bindless accesses: injection.md 6).
* rvk_tests: `draws_classify_water`, `water_half_float_round_trip`, `water_shore_field_distance_and_direction`,
  `water_profiles_follow_the_material`.
* `water.enabled` off: refcompare against the new stored reference: section 8.1.

### 8.1 Regression with the feature off

New stored reference `out\refcompare\20261022-002054` (`-Calibrate`, 17 spots incl. the four water spots; the old
reference's main.lua / spots.lua hashes no longer matched). Vulkan run with the water off (the default): **16 PASS + 1
WARN** (beach_rain: bad32 0.53 %, rain and sea noise as in every earlier run), run `out\refcompare\20261022-003113`. For
information, the same with `water.enabled=true` (a build before the last look changes): 11 PASS, 4 WARN, 2 FAIL
(`out\refcompare\20261022-003557`; the sea lies mostly inside the known-noise masks; FAIL = gui_inventory (the sea behind the
panel) and crystal_cave (the cave pool: section 9)).

## 9. Limits of this piece

* Waves are a Gerstner sum (FFT cascades: M8); the wave direction does not turn towards the coast; the shore field is
  unsigned (inside the overlay strip the distance is ~0) and only knows the engine's overlay, not the ground.
* Rivers do not flow (no flow direction yet; the engine scrolls its river textures through texcoord sources); the engine's
  river art (whitewater textures) is approximated by the falls' whitewater only.
* Waterfalls keep the engine's shading.
* Enclosed places: the cave pool (`crystal_cave`) and the harbour basin come out darker and duller than the engine's water,
  which shows its own textures lit by the light around; missed reflections fall back to the sky cube also in caves (no
  local probe yet), and the engine's water textures are not used.
* SSR has the usual screen-space limits (no off-screen geometry; RT reflections are M9).
* No underwater post (section 6).

## 10. Plan: the rest of M7 and M8

* **M7 rest**: underwater fog / absorption post (water zones + sea level), river flow from the engine's texcoord scroll
  constants (the river shader's c-registers) for flow-aligned ripples and foam, area profiles (a table keyed by region /
  water zone, overlay-tweakable values), an average of the material's diffuse texture as an alternative albedo (its 1x1
  mip), a sky-average for the cube's exposure, a water depth prepass (depth of the water surface for later passes).
* **M8 ocean**: FFT cascades (Tessendorf, JONSWAP from the weather's wind) on the same grid; the shore field upgraded to a
  signed field with the sea floor (a persistent top-down height field accumulated from the depth map, or the static
  meshes' geometry from the draw records rasterised like the coast) for wave shoaling, run-up and refraction towards the
  coast; breaking waves; the displacement cap tied to the player's swim state.
