# Sun shadows: the engine's cascades and shadow mask, the enhanced mask (M11 shadows)

Status: the enhanced mask pass and the cascade overrides are built and verified in game (`src/enh/shadows.*`, `src/engine/shadow_maps.*`,
`shaders/shadows/shadow_mask.*`); settings category Shadows (`shadows.*`, default off = the game's shadows).
[C] = confirmed (decompiled and/or measured in the running game), [I] = inferred.

## 1. The complaint, and its cause

Walking inland from the opening beach towards the big cliff, the cliff's shadow fades in sharply a few metres ahead. The
D3D9 renderer shows exactly the same behaviour as the Vulkan one (user, 2026-10-22): it is the engine's design, not a
translation bug.

The cause [C]: on High, the sun cascades reach only **7,500 units (75 m)**, and the shadow mask lifts the cascade shadow
to fully lit between **0.8 x and 1.0 x of that range** (60-75 m, `FadOut`). Beyond it only the coarse terrain variance map
(1024², ~59 units per texel, light-bleeding cut 0.7) shadows the ground. A shadow cast by geometry that is not in the
terrain map, or that the variance map washes out, appears over 15 m of walking at 60-75 m from the camera. Within the
cascades the edges are 8 point-sampled, dither-rotated taps (noisy), and the cascades switch hard at each split (stencil).

## 2. The shadow mask (`eCImageFilterShadowMask::OnUpdateOutput` 0x668560, `post_SSM.fx`) [C]

`eCIlluminationAdmin::UpdateShadowMask` (0x5748d0) only updates the mask filter (`illum+0x448`, or the "Blur" filter
`+0x458` when SoftShadows is on) and stores the result at `eCShaderMaterialAdmin+0x298`: the `smpShadowMask` (s0) of every
material pixel shader. Mask = A8R8G8B8 at the scene size: **alpha = sun shadow** (1 lit), **red = SSAO**.

The pass, as logged with `-LogFrame` (opening beach, 1600x900; HAL logs now print each effect pass's constants and the
targets' formats):

1. R32F 400x225 (depth downscale); SSAO into A8R8G8B8 800x450; BlurSSAO twice.
2. The mask target 1600x900 A8R8G8B8 with the scene's D24S8: UpscaleSSAO (RGBA, z ALWAYS).
3. **One quad per cascade, near to far**, technique **SSM** (terrain map present; `DSSM` without one, `SSM_Low` without
   SoftShadows): z GREATER against the quad depth (VS `c2.x` = the cascade's far end as hardware depth: 0.9757 /
   0.9892 / 0.9971), stencil EQUAL 0 with PASS = FAIL = INCR, colour mask A. A pixel takes the first cascade whose far
   plane is behind it: planar selection by view z, hard edges.
4. **VSSM** quad at z = 1: the terrain variance map alone where no cascade drew; its stencil ops ZERO reset the stencil.
5. **Blur** (SoftShadows): 9-tap depth-aware bilateral blur of the whole mask into a second 1600x900 target = the s0 mask.

Per cascade constants (PS):

| reg | name | value (High, beach) |
|---|---|---|
| c0..c3 | DepthToShadowMatrix | `split+0xa0` (light view x ortho x bias) x the depth-map reconstruction (below) |
| c4..c6 | DepthToVarianceShadowMatrix | the terrain map's, same chain |
| c7 | DizzerKernelSize | 2.5 / map size |
| c8 | FadOut | (camFar / (0.2 Max), -4) = (40, -4): `fade = saturate(d c8.x + c8.y)` |
| c9, c10 | MinVariance, MinLightBleeding | 1e-5, 0.7 |
| c11 | (preshader) | 1 / (1 - 0.7) |

Reconstruction: `d` = depth-map value (view z / engine far, c11.w = 60,000 on High); the quad's texcoord `t` = D3D9 pixel
/ size; `p = (d (2 t.x - 1), d (1 - 2 t.y), d, 1)`; the cascade texcoord = `(dot(c0, p), dot(c1, p), dot(c2, p),
dot(c3, p))` (w = 1, orthographic). SSM's pixel: `max(mean of 8 compares, fade) * terrain`, compare `lit = z < stored`
(no hardware compare, no slope bias); terrain = `max(z <= mean, saturate((var / (var + (z - mean)^2) - 0.7) / 0.3))`.

## 3. The cascades (`eCCascadeShadowMap`) [C]

### 3.1 Object, splits, fit

`eCCascadeShadowMap` (0x40 bytes): `+0x10` light direction, `+0x1c` MaxDistance, `+0x20` LogSplitFactor, `+0x24` map size,
`+0x28` split count, `+0x30` splits array (elements 0x168 bytes: `+0x0c` size, `+0x18` image, `+0xa0` texture matrix,
`+0xf8` depth bias, `+0x160` split start).

* Rendered **every frame** by `eCCascadeShadowMap::RenderShadowMaps` (0x59f200), called from a pre-pass op of the main
  view's frustum database (`eCDirectionalLight_PS::UpdateShadowMaps` 0x591fb0); the depth maps are DF24 (sampled D32F).
* Split starts: `t = i / N`, `lin = (Max - 1) t + 1`, `log = Max^t`, `start = (lin - log) * factor + log`. High (7500,
  3, 0.4): **1 / 1012 / 2230 / 7500** (rvk.log: "splits 1012, 2230, 7500"); Medium (5000, 3, 0.65): 1 / 1090 / 2269 / 5000.
* Fit: a bounding sphere of the slice frustum, light-space box = centre +- radius, z minimum forced to -100,000 (casters up
  to 1 km towards the sun). Snapped to whole texels (`eCShadowMapDirectionalLight2::UpdateCamera` 0x5a13e0): no shimmer.
* Texture matrix: `view * ortho * bias`, bias = 0.5 / -0.5 scale + (0.5 + 0.5 / size) (D3D9's half texel), z - (1e-4 +
  3e-5 i) (about 10 + 3 i world units; no slope bias).
* Measured texel sizes (High, 1024²): 3.6 / 7.9 / 26.7 units (cm).

### 3.2 Configuration and override

`eCIlluminationAdmin::ApplyConfiguration` (0x577a50) reads `/Engine/Render/Shadows/<Quality>/{EnableTerrainShadows,
TerrainShadowMapSize, EnableDetailShadows, DetailShadowMapSize, DetailShadowMapCount, DetailShadowMapViewRange,
DetailShadowMapSplitFactor, ScreenSpaceAmbientOcclison, SoftShadows}` (`data\ini\ConfigDefault.xml`), writes the size,
calls `SetSplitCount` (0x59f650), then writes MaxDistance and the factor. The size has no power-of-two or maximum check.

Our override (`engine/shadow_maps.*`): a pre-hook of `RenderShadowMaps` writes MaxDistance / LogSplitFactor (read every
frame) and, for a new size, calls `SetShadowMapSize` (0x59f600, frees the splits) + `SetSplitCount` - what
ApplyConfiguration does. The game's values are tracked (a value the game writes after ours becomes the new game value), so
"game" restores them and a changed Shadow Quality is followed.

### 3.3 Terrain map, casters

`eCShadowMapDirectionalLight2VS`: G32R32F 1024² variance map (z, z² + 7-tap blur), fitted to camera +- camera far,
re-aimed only after the camera moved > 32 texels (~1,900 units) or the sun turned > 0.5° (the occasional ~200-draw Shadow
view). Casters by `ShadowCasterType` (`eCIlluminated_PS+0x38`) [I: terrain + buildings in the terrain map, buildings +
objects in the cascades]. Per cascade caster culling: no size cutoff; each entity casts only within its own view distance
(`entity+0xcc * db+0x15b4`, fading over the last 10 %), so a longer range cannot show casters the player cannot see anyway.

## 4. The enhanced mask (`enh/shadows.*`, `shaders/shadows/shadow_mask.*`)

A `d3d9::DrawReplacer` for the main view's ShadowMask pass (the device now holds several replacers, each with
`handles()`):

* Cascade draws (a PS with `DepthToShadowMatrix`, `ShadowSampler`, `DepthSampler`: SSM, DSSM, SSM_Low; found by CTAB names,
  new `d3d9::Semantic`s) are skipped; their matrix, the quad depth (VS c2.x) and the cascade map are kept.
* At the frame's last cascade (as many as the scene hooks counted DF24/DF16 cascade maps this frame,
  `Device::countShadowDepthMap`) one procedural triangle at z = 1 draws the whole mask on that draw's own states (z GREATER =
  every non-sky pixel, stencil EQUAL 0 INCR, alpha only); the engine's VSSM then finds the stencil set and draws nothing but
  its stencil reset, and the engine's Blur runs unchanged. If the VSSM comes first (fewer cascade draws than maps), the mask
  is drawn there.
* Shader: the engine's reconstruction and matrices; splits from the quads' hardware depth through the main camera's
  projection (`viewDepthOfHardwareZ`); per cascade an N x N grid of bilinear PCF taps (one `textureGather` + 4 compares
  each; N = 2 / 3 / 4) with a receiver-plane depth bias + a constant bias. The receiver plane comes from the depth map's
  neighbours, on each axis the one with the smaller depth step (one-sided difference), clamped to 3 x the depth of a 45°
  texel: screen derivatives (`dFdx`) of the reconstructed position jump across every stone and leaf edge, and the wrong
  plane shadowed the surface itself (a soft dark patch over the shell pile of `shadow_inland_b` that D3D9 and the engine's
  own mask leave lit, section 5); the cascades blended over the last `cascade_blend` of each
  cascade's depth range (when the next map covers the pixel); the last cascade faded over its last `fade` (0.35); times the
  engine's terrain term, exactly as SSM.
* Filter width: `softness` texels between taps in the nearest cascade; the coarser cascades take fewer texels
  (`texelWorld[0] / texelWorld[i]`, at least half a texel), so their edges do not grow into metre-wide blobs.
* Engine side ("auto" with the enhanced mask): 4096² maps, 100 m range, split factor 0.4 -> splits 1 / 1347 / 2945 /
  10000, texels 1.2 / 2.6 / 8.9 units (the game: 3.6 / 7.9 / 26.7). User feedback that led there: at the game's 75 m with
  0.4 - 0.45 the enhanced shadows were "crisp close by, less detailed further, blobs at the far" (the far cascade's 27 cm
  texels under a fixed 4-texel filter).

Settings: `shadows.enabled`, `shadows.filter` (low / medium / high), `shadows.softness`, `shadows.cascade_blend`,
`shadows.fade`, `shadows.bias`, `shadows.map_size` (auto / game / 1024 / 2048 / 4096), `shadows.range` (m, 0 = auto),
`shadows.split`, `shadows.debug` (cascades: the mask shows 25 / 50 / 75 % per cascade; cascade_term / terrain_term: one
term alone).
Tooling: `scripts/shadow-compare.ps1` (engine vs enhanced at rvkcompare spots; the explicit spots `shadow_inland_*` walk from
the beach towards the cliff), rvk.log `enh: shadows: ...`, `engine: shadow cascades: ...`.

## 5. Results (2026-10-22, 1600x900, AMD test machine)

* Splits and texels as computed (rvk.log `enh: shadows: mask pass ...`): game 1012 / 2230 / 7500, 3.6 / 7.9 / 26.7;
  auto 1347 / 2945 / 10000 at 4096², 1.2 / 2.6 / 8.9. `engine: shadow cascades: ...` logs the game's values and ours.
* D3D9 (passthrough) vs the Vulkan engine mask vs enhanced at `shadow_inland_b` (the clearing inland of the beach): after
  the receiver-plane fix all three agree on what is lit; the enhanced edges are smooth instead of dithered.
  `shadow_inland_high` (30 m up): palm-frond shadows on the path keep their shape (the engine: blobs); bushes 80 - 110 m
  away get their canopy / cliff shadows (the engine: lit, beyond its 75 m).
* Debug views `shadows.debug=cascade_term` / `terrain_term` separate the two terms (how the false self-shadow was found).
* GPU (`-PerfFrame`, beach, `rvk_shadows`): frame 1.94 -> 2.63 ms; cascades 0.16 -> 0.69 ms (4096²), the mask's draws
  ~0.19 -> 0.27 ms. Validation layer clean.
* Not yet reproduced: the walk to the starting area's cliff itself (its place is not known to the spots; the explicit
  spots `shadow_inland_a` / `_b` / `_top` / `_high` / `_far` look inland from the beach).

## 6. Open

* Ray-traced sun shadows (RT tier) as a later source of the same mask alpha: the mask pass is the seam.
* The terrain variance map's size / light bleeding beyond the cascades.
* PCSS-style contact hardening.
