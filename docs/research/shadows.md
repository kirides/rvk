# Sun shadows: the engine's cascades and shadow mask, the enhanced mask (M11 shadows)

> Note (2026-11-05): the settings `shadows.cascade_blend`, `shadows.fade`, `shadows.softness`, `shadows.split` named below were removed (removed 2026-11-05, now a constant in code or an always-on behaviour: see `docs/research/settings-audit.md`); read them as the history / the values the code now fixes, not as switches to set.

Status: the enhanced mask pass and the cascade overrides are built and verified in game (`src/enh/shadows.*`, `src/engine/shadow_maps.*`,
`shaders/shadows/shadow_mask.*`); settings category Shadows (`shadows.*`; player default on since 2026-10-23 with filter low + 2048 maps, off = the game's shadows in the test baseline: overlay.md 2.3).
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
* Filter cost (S-8, frame-ownership.md 14.11): the 3x3 / 4x4 grids are one separable weighted pass over the touched texels by 2x2 gathers (4 / 4-9 gathers instead of 9 / 16; the receiver plane at the texel), the 2x2 Low filter keeps the per-tap form; the depth cross is 2 gathers; only the reachable cascades are computed; terrain fetch skipped at shadow 0; the RT term skips the cascades where it covers the pixel. RGA gfx1035 77 -> 67 VGPRs; no GPU change on the XTX (0.05 ms at 1600x900), picture unchanged.
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

**Regression and fix (2026-10-28 / 29, docs/research/engine-frame/shadow-bug.md):** from d120038 (reversed depth) the mask
triangle's vertex shader read `pc.draw.posFix.zw` from the `ShadowBlock` (a replaced draw's block, not a DrawBlock), the triangle was
clipped and the mask drew nothing = no enhanced sun shadows. Fixed by a `depthRemap` field in `ShadowBlock` (shared GLSL declaration
`shaders/shadows/shadow_block.glsl`; rvk_tests `shadows_block_glsl_matches_cpp`). A replacer's own block must carry everything its
shaders read from `pc.draw`.

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

## 7. Lamp shadows (clustered lights, 2026-11-05)

Point lights (candles, torches, lamps) get ray-traced shadows of their own, `lighting.light_shadows` (gpu-scene.md 9.10, rtgi.md 28): the engine's own point-light shadows (3 lights, atlas) stay for lights with a shadow channel; every other light of the table is traced by the `light.shadow` pass and scales the light's colour in the material variants. Debug view `lighting.debug=shadows`. Verified: the hero's and palm leaves' (cut-out) shadows from a spawned light (`rvk_pointshadow cast=false`), Vulkan = D3D12. Engine-owned register lights get their table twin's visibility since 2026-11-05 (rtgi.md 28; not yet seen at a walked-in lamp).

## 8. Sun shadow quality roadmap: SDSM fit, multiview cascades, PCSS + temporal denoise (Stage 0 design note, 2026-11-14)

Status: DESIGN ONLY (no code yet). [E] = estimate from a model (not measured in the game), [C] = from the engine notes (docs/research/engine-frame/E-shadows-lights-sky.md, README.md "The shadow views' setup", B-frustum-db.md), [I] = inferred. The per-pass in-game cost table (beach / jungle / harbour_docks / shadow_inland, Full + Wrapper, `RVK_PERF_FRAMES=24`) is still to be measured; section 5's old figures (cascades 0.16 -> 0.69 ms at 4096², mask draws ~0.27 ms, XTX) are the only numbers so far.

### 8.1 What GD3D11 does (E:\git\github\GD3D11, D3D11ShadowMap.cpp)

Same family as the engine, no SDSM, no multiview: per cascade the frustum slice's *minimal* bounding sphere (centre slid along the view axis until near / far radii are equal), radius rounded up to 1/16, light-space centre snapped to `floor(c / texelSize) * texelSize` on a world-origin grid; cascades rendered one DSV after the other (texture array or atlas); PCSS exists as a light-size setting in the shader (`SQ_LightSize`); no temporal filter. Reusable: the minimal-sphere fit and the grid snap.

### 8.2 The engine's fit (the seam) [C]

`RenderShadowMaps` 0x59f200 computes per split: the slice frustum (NDC box inverse-projected, planes 0 / 1 set to the split's start / end), a bCSphere by **incremental Merge of the 8 corners (not minimal)**, box = centre +- radius in light space (z min -100,000), stored at `split+0xe0` (box), `+0x60` (light view), `+0x100` (world slice frustum = the caster-cull beam), `+0x160` (start). Then `split vtbl+0x170` = `eCShadowMapBase::RenderShadowMap` 0x5a1070 -> `UpdateCamera` 0x5a13e0 (view = inv(L), ortho from the box, texel snap by `fmodf` of the projected translation, texture matrix `split+0xa0` = bias x P x V) -> `RenderShadowScene` -> `eCRenderSystemAdmin::RenderShadowMap` 0x66ce00 (`ExtractShadowMap` incl. `UpdateFrustumFilter` 0x5a1870 reads the beam at `+0x100`, origin `+0x80`; then the draws). The mask (and our RT shadows) read `split+0xa0` afterwards. Consequence: **a post-hook of `UpdateCamera` can rewrite camera view / projection (`split+0x58` camera `+0x10` / `+0x50`), the texture matrix `+0xa0`, and must also rewrite the beam `+0x100` (+ `+0x80`) so the caster cull matches** (otherwise casters outside the engine's box but inside ours are culled: shadows missing at the cascade edge). Open: the exact plane layout `UpdateFrustumFilter` reads (decompile 0x5a1870 first; these facts come from earlier sessions' Engine.dll notes and were not re-decompiled now); Full's owned cascade records (F3-1) read the same per-split view block, so the same hook feeds them.

### 8.3 Does SDSM pay? Texel density model [E]

Model script (flat ground, eye 1.7 m, fovY 50 / 65°, 16:9, pitch -6 / -25°, sun 35 / 60°, camera yaw 0 / 90 / 180° to the sun, splits 1 / 1347 / 2945 / 10000 as in "auto"): side length of the square map footprint
* min bounding sphere (GD3D11 fit): 26 / 57 / 192 m (fovY 50), 35 / 77 / 262 m (fovY 65);
* tight light-space box of the slice frustum: 0.86 - 1.0 x the sphere;
* tight box of the *visible ground pixels* in each slice (the SDSM idea, ground only; casters are z-side, not receiver x/y): 1.2 - 1.8 x smaller than the sphere for cascade 0, 1.3 - 2.8 x for cascade 1, 1.6 - 4.1 x for cascade 2 (the far slice sees the ground only in a narrow band below the horizon).

The measured engine texels (1.2 / 2.6 / 8.9 cm at 4096²) are ~1.9 x coarser than the min-sphere model's 0.6 / 1.4 / 4.7 cm: **the engine's incremental sphere merge is looser than a minimal sphere** (to be confirmed by logging the engine's box side). Stages by gain per risk:
1. min-sphere fit + grid snap instead of the merge sphere (free, rotation invariant, no shimmer risk): ~1.9 x linear density (3.6 x texels) at the same map size, or the same density at 2048² (cascade cost / 4);
2. SDSM bounds on top (visible min / max depth + light-space x / y bounds per slice): a further 1.2 - 2.8 x linear on flat ground (more in forests / interiors), but the footprint follows the view direction: it needs the quantisation below.

### 8.4 Plan A (SDSM fit)

* Where: an `UpdateCamera` post-hook in `engine/shadow_maps.*` (setting `shadows.fit = engine | sphere | sdsm`, Shadows page, `Spec::needs` shadows.enabled; `engine` = no hook work).
* A1 `sphere`: minimal sphere of the slice (GD3D11's formula, radius ceil to 1/16), centre snapped on a world grid of `texelSize`; rewrite view / projection / `+0xa0` / beam. No GPU work.
* A2 `sdsm`: compute reduction over the previous frame's scene depth (quarter resolution: min / max view depth + light-space x / y bounds per slice, workgroup reduction + atomics, ~0.03 ms [E]) into a small readback buffer (never the write-combined frame ring), consumed by the next frame's hook (one frame of lag), widened by a margin proportional to the camera's per-frame translation / rotation, reset to the sphere fit on a camera cut / loading, and quantised: extent rounded up in 1/16 steps with hysteresis (shrink only after ~30 stable frames), centre snapped to whole texels of that extent, so a rotating camera moves the box in whole-texel steps. A GPU-side fit is impossible for the engine's draws (CPU constants) and the owned cascades read the same CPU view block: one frame of lag + margin is the design.
* Verification: slow-rotation scenario, crops of one shadow edge over 8+ frames, camera cut / quick-load; log line `enh: shadows: fit ...` (side and cm per texel per cascade).
* Risks: caster-cull consistency (8.2), FSR jitter in the reduced depth (margin), flicker when the margin is too tight.

### 8.5 Plan B (multiview cascades): expected NOT to pay [I]

Full's owned cascade draws are persistent records flushed as a few indirect draws per view (14.29.1: cascades 42 -> 4 commands, 0.21 - 0.30 ms GPU on the XTX at 4096²): what is left is vertex work + rasterisation of each cascade's own casters, which multiview does not reduce (and a draw visible in one cascade would run its VS for all views unless a per-view cull in the VS / view masks is built). Multiview saves command recording / draw setup, which the indirect flush already collapsed. Seams if built: `Device::ownedFlush` per view slot (`owned_persist.cpp`, shared file), dynamic rendering `viewMask` (core 1.1), a depth array target (the mask reads three separate maps through bindless slots), D3D12 view instancing tier or `SV_RenderTargetArrayIndex` from the VS (`VPAndRTArrayIndexFromAnyShaderFeedingRasterizer` cap; fallback = today's path). Wrapper mode records the cascades through the engine's own sequential passes: no multiview there. The point-light atlas has no cubes (one 2D atlas, a pass per cell). Decision: not before A1 / A2 are measured; revisit only if the owned cascades cost > 0.3 ms CPU or VS-bound GPU at the player defaults.

### 8.6 Plan C (PCSS + temporal denoise) [I]

Seam: `shadow_mask.frag.hlsl` `cascadeShadow`. PCSS = blocker search (8 Vogel taps over a light-size kernel) -> penumbra -> 4-8 Vogel taps rotated per pixel (interleaved gradient noise + per frame); denoise: `rt_shadow_temporal` / `rt_shadow_spatial` (reprojection + clamp) extended to take the cascade term. **Motion vectors exist only with a G-buffer motion target (FSR / FG / native AA)**; with upscaling off the cheap source is camera-only reprojection from depth (static scene exact, actors ghost; tight neighbourhood clamp) with a spatial-only fallback (the engine's Blur follows our mask anyway). Today's 3x3 / 4x4 gather grid is 4 gathers (0.05 ms at 1600x900 on the XTX), so C is a *quality* option (`shadows.soft = off | pcss`, default off), not a cost win, unless it replaces the grid on the integrated-GPU preset. A small default light size keeps the game's look; judge on `shadow_inland_*` with `shadow-compare.ps1`.

### 8.7 Order and gates

A1 -> measure density + picture -> A2 -> measure -> C -> B only if justified. Per stage: rvk_tests, `dxil-check embedded`, `rvk_vktest --passes`, `rvk_rhitest --validation` (+ `--d3d12`), `rvk_d3d9test --validation`, `shadow-compare`, refcompare Full at the shadow spots, picture_check, perf-ab (`-Key shadows.fit -A engine -B sphere`) at beach / jungle / harbour_docks / hold. Shared-file edits expected: `engine/shadow_maps.*`, `enh/shadows.*`, `core/settings.cpp`; none in owned_persist / static_ops for A and C.

### 8.8 Stage A1 result (2026-11-14): `shadows.fit = engine | sphere` (default sphere)

Decompile of `UpdateFrustumFilter` 0x5a1870 (headless Ghidra, Engine.dll): it is one call, `eCBeamTree::Init(db+0x118, split+0x100, split+0x80)`: the caster-cull beam is the camera's **slice frustum** (`+0x100`, set by `RenderShadowMaps`) with `+0x80` as the sweep direction, NOT the light box. A tighter box therefore needs no beam rewrite (the slice frustum is a superset of what any fit inside it can see); the shadow camera's own frustum (rebuilt from the new projection x view) still culls casters outside the box. `RenderShadowMaps` 0x59f200 confirmed: light view L = LookAt(light dir) with the main camera's position as translation, sphere = 8x `bCSphere::Merge` of the view-space slice corners, centre -> world (inv view) -> light (inv L), box = centre +- radius (z min -100,000); `UpdateCamera` 0x5a13e0 then builds V = inv(L), ortho from the box, the texel snap (`fmodf` of the projected V translation by 2 / size: origin-anchored grid, stable while the box size is constant) and the texture matrix.

Built (`engine/shadow_maps.*`): a pre-hook of `UpdateCamera` (verified at install: the Dir2Ex vtable slot +0x160 must be 0x5a13e0; acts only on cascade splits) overwrites the split's box (+0xe0..0xf4) with GD3D11's minimal sphere of the slice (`minimalSphere`, pure, unit-tested; radius rounded up to 1/16 cm so projection noise never rescales the map) before the engine builds camera, projection, snap and texture matrix from it: cascade draws, the owned cascades (Full), the mask and RT shadows all read the same result. `native/effect_ops_setup` (group 2048) compares the engine's camera with one composed from the engine's own fit: while the override is active it skips that compare (`fitOverridden()`; the composed camera draws nothing). rvk.log `enh: shadows: fit sphere: cascade i a .. b: box side ... (engine ...) cm, ... cm per texel`.

**Measured (rvk.log, 1600x900 test window, 4096^2, auto splits 1 / 1347 / 2945 / 10000):** engine box side 4920 / 10721 / 36403 cm -> sphere 4215 / 9220 / 31303 cm = **x1.17 / x1.16 / x1.16 linear** (cm per texel 1.20 / 2.62 / 8.89 -> 1.03 / 2.25 / 7.64; at the spot with a narrower view 0.92 -> 0.77, x1.19), i.e. ~1.35 x texel area for free (no GPU pass, one hook call per cascade and frame). **The 1.9 x claim of 8.3 was wrong:** the model's fov was narrower than the game's; the engine's merged sphere is only ~16 % looser than the minimal one. Pictures (`out/shadows/20261114-211011/sbs_*.png`, left engine fit, right sphere, shadow_inland_a / _b, beach_sun, _far): the same shadows, no missing casters at the cascade edges, no new artefacts (differences: water and foliage animation only; I read inland_a and inland_b). The remaining gain of section 8.3 (1.2 - 2.8 x on visible ground) is SDSM's (A2).


### 8.9 Stage A2 result (2026-11-14): `shadows.fit = sdsm` (opt-in, "Adaptive"; default stays sphere)

**Built.** `enh/sdsm.*` + `shaders/shadows/sdsm_reduce.comp.hlsl` (pass `shadows.sdsm` at AfterDepthPrepass: the hardware depth is complete there, in Wrapper and in Full): every 2nd pixel in x / y of the main view's depth, non-sky pixels binned by log view depth (128 bins, zMin 10 cm .. the player's shadow range), per bin the light-space x / y min / max (level 1: 16x16 group, groupshared atomics; level 2: `rvkRwAtomicMax` of the non-empty words into a 2 KiB buffer, cleared with `fillBuffer` first; order-preserving uint encoding, 0 = empty, so max of x, -x, y, -y). The result is copied into a Readback buffer (4 slots, like depth_z compare) and decoded at the top of the next cascade updates once `completedFrame()` passed it (never waits, never touches the frame ring). Off = the pass, the pipeline and the buffers do not exist. The light space is the engine's (the inverse of `split+0x60`, camera position as translation); the reduction stores the matrix it used, the pose and the projection. DXIL parity, ABI `BLOCK(SdsmPush)`, `rvk_vktest --passes` pass.

**Fit** (`engine/shadow_maps.cpp`, the pure part `sdsm::fit` / `merge` / `step` / `snap` / `margin`, unit-tested): the UpdateCamera pre-hook computes the sphere box as in 8.8 and then, per cascade, merges the bins overlapping the slice [n, f] (conservative: whole bins, 5 %), shifts them into this frame's light space (difference of the inverse light matrices' translation), widens by `margin = translation + angle * f + 30 cm + 1 % of f (+ the throttle's drift allowance)` where translation / angle are the REAL pose difference between the reduction's camera and this frame's (so the margin follows the measured latency, not an assumed frame), keeps the sides within 2:1 (the mask's filter is sized from the x axis), rounds each side up on a 2^(k/16) grid (grow at once, shrink after 90 calm updates to the grid value above the window's peak), snaps the centre to whole texels (the engine's own origin-anchored snap then keeps the grid world-stable while the extent is constant), caps at the sphere's side. z is the sphere's (casters toward the sun are still culled by the slice beam, 8.8). Shadow range: the last cascade's end follows the visible depth x1.6 (min 1500, max the player's range, same step / hysteresis), so interiors split a shorter range (everything visible stays before the fade zone of the last cascade). Fallback to the sphere box per cascade (counters in the 5 s log line `enh: shadows: sdsm (5 s)`): no data, data older than 3 updates / 0.25 s, sun turned (light axes changed > 2e-4: ~every 0.5 degree during a sun sweep) or field of view changed, motion since the reduction > 4 m or 20 degrees, empty slice; a camera cut (jump > 10 m, fov change, gap > 0.3 s = loading / menu) drops the data and the extents until a reduction that ran after the cut arrives. The throttle (cascades redrawn every 2nd frame while the camera drifts less than a limit) uses tighter limits under sdsm (60 cm / 0.006).

**Latency (measured, rvk.log `reduction age`).** The reduction of frame N is read when the GPU completed frame N: at the cascade update of frame N+1 it is almost never complete; the use is at N+2 (age histogram over a run at 1600x900 Full: 0 / ~25 % at 1 update / ~75 % at 2 updates, never 0 or more than 2; mean 1.7 - 2.0). The margin therefore covers two frames of camera motion; averaged it is 5.5 % of the slice's far end in the quick-load / walk runs (30 cm + 1 % of f fixed, the rest motion + throttle drift). **Density that survives margin + hysteresis (Full, 4096^2, rvk.log, linear cm per texel vs the sphere):** harbour_docks cascade 0 / 1 / 2: sphere 1.03 / 2.25 / 7.64 -> sdsm 0.44 x 0.74 / 0.68 x 1.35 / 2.38 x 3.36 (x2.1 / x2.7 / x3.1 linear on the larger axis vs the ENGINE, i.e. x1.8 / x2.3 / x2.7 vs the sphere); shadow_inland (stands in the forest, sun behind) sphere 0.77 / 1.69 / 5.75 -> 0.37 x 0.62 / 0.71 x 1.30 / 2.18 x 3.2 (x1.2 - 2.1 vs the sphere on the longer axis, x2 - 2.7 on the shorter). In a walk and a turn the extents stay at their window peak (90 updates), the boxes are 1.3 - 2 x smaller than the sphere's per axis in cascade 0 / 1, 1.6 - 2.5 x in cascade 2: most of the model's gain survives. The same ratios hold at the shipped 2048^2: sdsm at 2048 is about as sharp as the sphere at 4096 (area-equal), the cascade pass costing the same as a 2048 map.

**Cost (perf-ab `-Key shadows.fit -A sphere -B sdsm -Mode full -PlayerDefaults -Pairs 4`, 2048^2 cascades, RisenTest.exe, shared copy):** the reduction pass is below the list's 0.01 ms floor (not among the top passes in any scene); GPU frame +0.21 ms (hold, sd 0.6 / 1.1: noise), +0.10 ms (jungle, z 6), +0.03 ms (beach_sun), +0.06 ms (harbour_docks), +0.07 ms (shadow_inland_b); the cascade pass itself +0.08 ms in jungle (0.246 -> 0.330), +0.077 ms in shadow_inland_b (0.125 -> 0.202), +0.038 hold, 0 beach_sun, -0.006 harbour_docks: a tighter box rasterises the casters' triangles larger (more texels per caster), the same effect a bigger map has; CPU per frame unchanged (+-0.05 ms, z < 0.6).

**Pictures** (Full, sphere vs sdsm, side by side + x4 difference, read: shadow_inland_b, harbour_docks, monastery_hall; the other spots' sheets are in the scratch dir): the same shadows; the differences are foliage / water animation and candle flicker. Rotation walk (12 pictures every 5 frames, sdsm and sphere), a sun sweep (10 pictures) and a quick-load (14 pictures, the first 2.5 s after the world is ready): no garbage frame after the cut, no popping where the extents step, no missing shadow at the screen edge at the 1:1 pictures I looked at. NOT covered: a really fast mouse turn (the test harness has keyboard turns only: the 'fast' walk run is a straight walk with a turn parameter the mod does not honour); the margin is sized from the actual pose delta and a fallback to the sphere above 20 degrees of turn in two frames, but the picture was not checked. picture_check (Full test baseline + `shadows.enabled=true`): FAILED cycles are the same with sphere and sdsm (94 vs 91 over the 7-spot run, 56 vs 55 of them the jungle box [0,418..1599,662] named in the task; the others whole-frame boxes in both arms), owned shadows DIFFERING 0 / MISS 0 in both. GPU-AV first run (harbour_docks + shadow_inland_b): 0 VUID, 0 errors. D3D12 backend run (harbour_docks + shadow_inland_b, 450 fits): works, age 2.00 every update (min = median = max), 0 errors. Run-time recreation: `shadows.map_size` 4096 -> 2048 -> 1024 (-> 4096) at run time (SetShadowMapSize + SetSplitCount, the ApplyConfiguration path; the Shadow Quality menu itself was not driven) plus a quick-load, sdsm and sphere, with validation: 0 VUID, no crash.

**Decision.** sdsm is clearly sharper (x1.2 - 2.7 linear vs the sphere at the same map size, free of CPU cost, +0.03 - 0.1 ms GPU, mostly in the cascade pass) but it keeps a sphere fallback during sun sweeps (every 0.5 degree: a texel-size change for a frame) and the fast-turn case is not verified with a mouse: **default stays `sphere`, `sdsm` is the opt-in "Adaptive"**; flip it after a fast-mouse-turn check and a look at the sun-sweep fallback rate (a refinement: carry the reduction across a small sun turn by rotating the bounds instead of falling back).

**Port to a same-frame GPU fit (follow-up, if the number above says the margin eats the gain).** `sdsm::fit` is one pure function over plain inputs (`FitIn`: the reduction, slice, current light inverse, camera, sphere box, drift; `FitOut`: box x / y; state: two extents per cascade) with no engine access, so it ports 1:1 to a compute shader (depth prepass -> reduction -> fit writing the cascade view / ortho / texture matrices to a buffer; the margin then needs no motion term and the latency is 0). The consumers that read the engine's CPU matrices today and would need the GPU matrices: the owned cascades' view slots (F3-1 `Device::ownedFlush` view blocks), the enhanced mask (`enh/shadows.cpp` `drawMask` captures the mask draw's psConstants = split +0xa0 rows), the RT shadows (cascade view of the character captures, rt-shadows.md 7), F3-2's cascade cluster-cull planes (derived from the box), and the composed check path (`native/effect_ops_setup` compares the engine's camera with one composed from the engine's own fit; `fitOverridden()` stands it down today). The engine's own cascade draws (Wrapper) read CPU constants and cannot follow a GPU fit: Full-only.

**Run-time safety (review of 8.8).** The UpdateCamera pre-hook reads `self` first (vtable) and every field of the cascade object through `safeRead` (the object may be gone after a Shadow Quality change / world change: it is refreshed only by RenderShadowMaps); `g_lastFit` is mutex-guarded, the log memory is reset by the render thread (a flag from `setOverrides`).

### 8.10 Stage A3 (2026-11-15): fast turns, sun sweeps, camera cuts under `shadows.fit = sdsm`: default decision

**Method.** Scenario `mods/rvk_sdsmtest` (this repo): hero + NPCs hidden, HUD off, weather / clock pinned like the rvkcompare spots (`shadow_inland_b`, `harbour_docks`), then `risen.camera.set` (instant, position fixed, direction rotated) **once per rendered frame** by a fixed step: `mode=turn,dps=180|360|720` (3 / 6 / 12 degrees per frame, frame-locked at 60 fps so the pose delta per frame is identical in the sphere and the sdsm run: the burst frames pair 1:1), `mode=sun,rate=60|240` (game minutes per second, the clock released), `mode=cut` (the hero + camera jump between the two spots, a 30-frame burst starts on the jump frame). Bursts: `risen.debug.screenshot(name, {burst=30, interval=1})` in Full (and one Wrapper pair); sphere vs sdsm side by side + |difference| x4 (water, sky and foam differ between runs, so mean-difference tables are noise-dominated: two sphere runs of the same scene differ by 3.7 mean, sdsm vs sphere by 2.4; the judgement is by reading the pictures). The renderer logs one `enh: shadows: sdsm update N cK: <sdsm|reason> box X x Y cm (sun S rad)` line per cascade update at debug level (`-RvkLogLevel debug`; the 5 s counters of 8.9 stay); a scratch script parsed them per burst window.

**Findings (before the changes below).**
- *Turn 180 / 360 dps (3 / 6 degrees per frame):* 0 fallbacks over 120 updates in both spots (the 20-degree-in-2-updates limit is not reached); the boxes grow to the sphere cap in cascades 0 / 1 inland within ~1 s (the whole slice becomes visible while turning), at the harbour they stay tight. Pictures (shadow_inland_b 360, frames 4 / 14 / 24 / 29; harbour 180 / 360): the same shadows as sphere, no shadow missing at the screen edge that enters the view, no popping; lit patches on the forest floor, cliff and rock shadows, the pier shadow identical; the only differences are water, sky, foam.
- *Turn 720 dps (12 degrees per frame, a flick):* 24 degrees over 2 updates > 20: the motion fallback fired on 58 of 119 updates and **toggled sdsm <-> sphere on every other update** (up to 17 linear-size jumps of x1.1 - 1.8 per cascade in 2 s). Pictures (harbour 720, frames 10 / 12 / 14 / 16): no missing shadow, no visible step (the picture changes by a large part per frame; the rope shadow is the only difference: sdsm is sharper), but a still camera after a flick would see the toggling as flicker.
- *Sun sweep:* the engine's light direction does not move every frame but in steps of 0.01 - 0.02 rad (every ~0.5 s at rate 60); the old 2e-4 threshold dropped to the sphere on 45 % of the updates at rate 60 (harbour; 35 % inland) and on 100 % at rate 240, each time a x1.2 - 2.5 texel-size step. Sun <-> moon switches are single jumps of 1.4 - 1.8 rad.
- *Camera cuts:* the hero jumped between the spots 3 times per run (a different clock and weather too): 1 fallback per cut (cut / sun-fov), the first frame after the cut shows the shadows of the new place (character, pier, trees, leaves) in sdsm like sphere, no garbage frame in the 30 frames after each cut (harbour -> forest -> harbour; cut 4 was refused by the screenshot writer: the previous burst was still being written).
- *Quick-load + walk* (`rvk_rtwalk`, pristine QuickSave, Full, validation layer): the load is a cut (`cuts 3`, 2 motion fallbacks), the 32 burst pictures of the walk show the character, tree and ground shadows, 0 VUID, reduction age 1 / 2 updates as in 8.9.

**Changes** (`engine/shadow_maps.*`, test `sdsm_fit_whole_cascade`):
1. *Sun turn absorbed:* the reduction's bounds are in its own light axes; a turn up to `kMaxSunAngle` (0.05 rad, ~3 degrees; the largest row difference of the light basis between the reduction and this update) now adds `turn * 1.5 * f` to the margin (a point at distance r from the camera moves by at most turn * r across the box; 1.5 = the slice corner) instead of the sphere fallback; above it (sun <-> moon, a clock jump) the sphere as before. Sun sweep rate 60: 1 fallback in 332 updates (the sun <-> moon jump) instead of 151; rate 240: 18 of 420 (the sun / moon jumps and ~4 degree steps) instead of 419.
2. *Motion hold:* after a motion fallback the cascade keeps the sphere for 20 updates (`kSdsmMotionHold`, ~1/3 s): a flick toggled every other update, now one transition in and one out (720 dps: 2 - 5 linear-size jumps per cascade over 120 updates instead of 8 - 17).
3. *Debug line* per update (reason + box + sun turn), see Method.

**Decision.** With the two changes `sdsm` is fit to be the player default: no missing shadow, popping, shimmer or garbage frame was found at 180 / 360 / 720 degrees per second, in sun sweeps, at cuts and after a quick-load, in Full and Wrapper, with the validation layer (0 VUID; `rvk_d3d9test --validation` 0). Residual, known: (a) a turn that exceeds 20 degrees in two cascade updates (720 dps at 60 fps, or 360 dps at 30 fps: the limit is per update, not per second) shows the sphere's density for ~1/3 s after the motion (a texel-size step once in, once out); (b) a sun <-> moon switch and every cut step the texel size once; (c) "empty" fallbacks (a cascade whose slice has no visible pixel, e.g. cascade 0 against the sky) are harmless. Flipping the default is `shadows.fit` in `core/settings.cpp` (sphere -> sdsm) + the README line; not done on this branch.

### 8.11 Stage C result (2026-11-15): `shadows.soft = off | pcss` (+ `shadows.soft_size`), default off

**Built** (`enh/shadows.*`, `shaders/shadows/shadow_cascade.hlsli` (the cascade term shared with the mask pass), `shadow_soft_trace / temporal / spatial.comp.hlsl`). At the mask draw (`Shadows::drawMask`, through `Device::runPassNow`: the cascade matrices are the draw's own, the cascade maps are rendered by then) three compute passes run before the draw: (1) **trace**: per pixel the whole cascade term (blend band + last-cascade fade included) with PCSS: a blocker search (the pixel's own gather + N Vogel-disk gathers over a search radius derived from the sun's cone and the cascade's texel size, 3..14 texels) -> average blocker depth -> penumbra = 2 tan(half angle) * dz / texel depth (texels; the receiver-plane bias as today) -> N bilinear PCF taps on a Vogel disk of that radius, kernel rotated per pixel (interleaved gradient noise) and frame; N = 2 / 3 / 4 from `shadows.filter`; raw R = visibility, G = filter radius; (2) **temporal**: reprojection with the camera (depth from the depth map + `prevViewProj`, depth-checked bilinear history, `rtsHistory`), history clamped to the raw 3x3 min / max, the history length follows the penumbra (a pixel whose filter radius is under ~0.75 texel takes no history and no kernel rotation: the engine's crispness, no flicker, no blur of hard edges under camera motion; wide penumbrae accumulate up to 10 frames); (3) **spatial**: depth-plane bilateral blur only where variance / short history AND a wide penumbra. The mask draw then fetches the result (`ShadowBlock.rt.y`) instead of evaluating the grid; terrain (VSM) term and the RT term unchanged; the engine's own Blur still follows. Not built: motion vectors (the G-buffer motion exists only from the solid pass on, after the mask: reprojection is camera-only; actors rely on the depth check + clamp), half-size history for low bandwidth. Off = no pipelines, passes, images (built on first use, `shadows.soft` images released on off). Fallback to the PCF grid for frames without pipelines / size mismatch / debug views.

**Cost** (perf-ab `-Key shadows.soft -A off -B pcss -Pairs 4 -PlayerDefaults`, RisenTest.exe, shared copy, XTX, soft_size 0.15; the mask draw gets cheaper because it only fetches): Full (after merging main, sdsm default): GPU frame +0.034 ms (+1.3 %, shadow_inland_b), +0.074 (+2.3 %, jungle), +0.033 (+1.4 %, harbour_docks); trace 0.042 - 0.063 ms, spatial 0.028 ms (jungle), mask pass -0.030 .. -0.047 ms; CPU +0.03 - 0.04 ms (z < 1). Wrapper: +0.045 (jungle), +0.040 (harbour_docks), shadow_inland_b -0.12 (noise, sd 0.3). 680M extrapolation (x10 - 15 for compute with gathers, frame-ownership.md 14.3 model): ~ +0.5 - 0.8 ms at 1600x900, ~2 % of a frame. A quality option, not free; stays off in the presets.

**Pictures** (wrapper, 8 spots engine / PCF / PCSS; crops read): PCSS keeps the shadows' places and edges. The sun size is the whole look: the real sun's 0.27 degree half angle already blurs a palm frond 6 m up over ~6 cm and the fronds' crisp shadow detail on the ground goes soft (the game's identity: crisp); 0.6 degrees (first guess) is a smudge. **At 0.1 degrees (frond pattern kept, slightly softer) to 0.15 the picture stays natural**; default `shadows.soft_size` 0.15 (range 0.1 - 2). Contact hardening: the fallen log's shadow stays defined at its base and widens along its length. No visible noise or banding in the penumbra.

**Motion** (rvk_rtwalk walk + camera turn, 20-frame bursts at interval 1; rvkcompare freeze + burst 20 interval 1 at shadow_inland_b / hero_idle / pub_idle with upscale off / native_aa / quality, PCF vs PCSS at 0.25): static scenes: temporal luminance std 0.24 / 0.35 / 0.38 (PCF) vs 0.24 / 0.30 / 0.37 (PCSS): no boiling; the animated hero (hero_idle) std 2.0 / 1.7 / 1.7 vs 1.9 / 1.8 / 1.5: no extra smear; the walking hero's shadow (bursts, crops read): clean, no trail. Not covered: a fast mouse turn; a turn burst that faced the shadows (the turn pictures looked at the sea).

**Gates**: rvk_tests 363 ok (`shadows_soft_params`, ABI block checks), `dxil-check embedded` 0 failures (190), `rvk_vktest --passes`, `rvk_rhitest --validation` (Vulkan + D3D12), `rvk_d3d9test --validation`, GPU-assisted validation game run clean. Settings: `shadows.soft`, `shadows.soft_size` (Advanced).

**Also run**: Full + shadows + `native.picture_check` (jungle, shadow_inland_b): FAILED lines 21 with PCSS and 21 with PCF (the known jungle cycles, unchanged by pcss); D3D12 (wrapper, shadow_inland_b, 0.25): same picture, no errors; Vulkan GPU-assisted validation run clean.
