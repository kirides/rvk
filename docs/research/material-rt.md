# Traced reflections of the material pack's surfaces, rain wetness and puddles

Status: built 2026-11-05 (traced reflections, rain, puddles; glass: section 8; GI follow-ups: section 9, not built). Legend: **[C]** confirmed in a game run, **[I]** inferred.
Code: `src/enh/material_rt.*` (the pass), `src/enh/rain_wetness.h` (pure wetness model), `engine::rainOfWeather` (`src/engine/weather.*`),
`shaders/materials/material_rt_common.hlsli`, `material_rt.comp.hlsl`, `material_rt_filter.comp.hlsl`, `material_rt.frag.hlsl`. Tests: `rvk_tests rain_*`.

## 1. Settings
`materials.quality = high` (the option needs ray query; without it the setting applies as medium) switches the pass on; the pack must be on (`materials.pack`).
`materials.rt_reflections` low | medium (default) | high = the cost tier (below), `materials.rt_range` (m, 150), `materials.rt_strength` (1),
`materials.rain_wetness` (1), `materials.puddles` (0.7), debug `materials.rain_force` (0..1: pretend this much rain; 0 = the game's weather).
**Off = nothing exists**: no pass, no pipelines are used (the compute pipelines are built in the background at start when the RT tier exists),
no images, the raster's sky cube reflection as before. While the pass runs the raster's sky term is off (`params[2].z = 0`): the traced image carries the sky (a miss reads the sky cube).

## 2. Design
Why a pass over the G-buffer and not a ray query in the material pixel shaders (the water reflection's first version did that: +3.4 ms): the pixel shader variants
are ~2300 x 3; ray-query code and its registers in each would cost compile time and occupancy for every lit draw. A screen-space pass costs one dispatch.
* **Input** [C]: the G-buffer's normal target (octahedral normal in rg, **roughness / metalness of the pack entries in b**, `rvkGbufferMaterialDecode`; 0 = no entry) at the
  point of AfterOpaque (order 30, after the motion fill), the engine's depth map, the scene HDR (lit colour: screen reuse and the metal tint), the sky cube.
  The G-buffer is forced on while the pass is on (`Gbuffer::Settings::forced`).
* **Trace** (`material_rt.comp`): pixels with a pack entry rougher than 0.7 keep the raster only; the others trace one ray per pixel (tier high; tier low / medium: one per 2x2 block, the
  sample rotating through the block each frame), the direction drawn from the **GGX lobe** of the roughness (a mirror below 0.04). Hit shading as the water's RT (`water_rt.comp`):
  the hit's pixel is taken from the scene where it is on screen with a matching depth (the engine's lit colour), else albedo x pow(ambient SH + sun N.L x shadow ray, 0.45) with
  the engine's fog; the ambient is the hemisphere's sky where a ray straight up from the hit reaches the sky, else the engine's indoor colour. The sun and fog come from the lit
  draws' registers (`MaterialRt` is a `DrawObserver`: DirLight c22.., FogColor / FogSettings), the sky SH from `engine::hemisphere`.
* **Energy**: radiance x the raster's split-sum Fresnel (F0 = 0.04, or the surface colour's tint for metals) x `lerp(1, 0.25, rough)`, capped like the raster's own additions to 0.15 (rough) .. 0.55 (smooth)
  of the surface's lit brightness (metals / puddles exempt): the engine's colours stay; fogged like the surface.
* **Filter** (`material_rt_filter.comp`): a 4x4 gaussian gather to every pixel weighted by depth, normal, roughness and "is a candidate", then a temporal blend with the motion-reprojected
  history clamped to the gather's colour range (keep 0.6 for a mirror .. 0.88 for rough). Output rgb = the light to add, a = the factor the scene is multiplied by.
* **Composite** (`material_rt.frag`): one fullscreen draw into the scene HDR target, blend `src.rgb + dst * src.a`.
* **SSR fallback**: the screen reuse of hits *is* the screen-space part (the engine's lit pixel where the hit is visible); a GPU without ray query does not get the pass at all (the quality option
  is unavailable and falls back to the raster's sky cube); a separate screen-space ray march for such GPUs is **not built**.

## 3. Tiers
| tier | rays | cut-outs | shadow ray at hits |
|---|---|---|---|
| low | 1 per 2x2 | solid | none |
| medium | 1 per 2x2 | alpha-tested | cut-outs solid |
| high | 1 per pixel | alpha-tested | alpha-tested |

## 4. Verification [C]
Picture runs with `water-compare.ps1` (left `materials.quality=medium`, right `high`, sample pack in the copy's `bin\rvk\materialpack`), spots pub_interior, monastery_hall,
harbour_docks, harbour_night: no artefacts; the effect is subtle by construction (the cap): monastery hall floor and the plaza stones get a slightly stronger sheen; no
white-out, no seams, no trails. See section 6 for costs and the gates.

## 5. Rain wetness and puddles (feature 4)
* **Weather** [C source, I thresholds]: `engine::rainOfWeather` = the weight share of the weather states named rain / storm / thunder / drizzle / shower (dungeon / cave / indoor states
  count as dry); read once a second. `rain::wetnessStep`: wetness follows the rain share with a 25 s time constant up and 240 s from 1 to 0 down. rvk.log `enh: materials.rt: the world is
  getting wet / dry` and the 5 s line (`rain x wetness y`). `materials.rain_force` sets both at once (tests).
* **Wet surfaces** (pack entries): roughness x (1 - 0.7 w), the scene x (1 - 0.3 w), where a ray up and a little out of the surface (`up + 0.6 N`, 40 m) reaches the sky (roofs, caves, overhangs are
  dry; walls catch less: x 0.3 .. 1 by the normal's y). The first version used a straight-up ray and made rugged cliff faces patchy (every ledge flipped wet / dry): the tilted ray + the
  normal weight fixed it [C].
* **Puddles**: flat surfaces (G-buffer normal y > 0.78, also pixels without a pack entry: the ground) where an fbm value noise of ~2 m cells (world xz) passes a threshold that falls with the wetness
  (the puddles spread): the surface there becomes a flat water mirror (normal = up + the rings of falling drops, roughness 0.03, F0 0.06 lifted so a puddle reads in the engine's flat light), the
  scene under it x 0.55, the reflection traced like any other (the sky, buildings, trees, ships; characters through the RT scene). `mrtPuddle` is deterministic in the position: the filter evaluates the
  same candidate set. Rings: `mrtRipple`, a 55 cm grid of expanding rings.
* **Only with the traced reflections** (quality high): the raster pack's wetness stays the authored constant x `materials.wetness`; the rain acts in the traced pass, which is the one place that has a roof test.

## 6. Costs and gates (re-measured section: per-stage)
GPU time of the pass `materials.rt` (trace + filter + composite, `-PerfFrame 4300`, `RVK_PERF_FRAMES=24`, 24 frames averaged, RX 7900 XTX, 1600x900, harbour_docks hold of rvkcompare, tier medium,
sample pack, other agents' games on the same GPU: +-0.3 ms noise; the span includes the wait for the solid pass to drain):
| | dry (`rain_force=0`) | pouring (`rain_force=1`: every flat pixel traces, roof rays) |
|---|---|---|
| Vulkan | 3.1 - 3.5 ms | 3.0 ms |
| D3D12 | 1.3 - 1.6 ms | 1.9 ms |
**Per-stage breakdown** (the pass is now three injected passes `materials.rt trace / filter / composite`, harbour_docks, tier medium, dry, 24 frames): Vulkan trace 1.8 ms / filter 0.08 / composite 0.01;
D3D12 trace 1.4 / 0.07 / 0.01 (a second Vulkan sample read 2.8 ms for the trace: other agents' games share the GPU, the spread is +-1 ms). So the "Vulkan costs more" of the first numbers was measurement
noise plus the old single span (which also covered the sync with the solid pass); the two APIs run the same shader at about the same speed, and **the ray trace is the cost**, not the barriers. A lower roughness limit
(0.55 instead of 0.7) did not give a measurable gain at this spot and was reverted. A first version of the filter gathered 4x4 taps, the final one 2x2 (3x3 at full rate): no measurable change, so the filter is not the main cost; the trace and the pass's barriers are. Not
optimised further (a tier-low option exists: no shadow rays at hits, cut-outs solid). Vulkan costing more than D3D12 is unexplained (same HLSL; probably the RT scene wait / barrier span).
Gates of the commit: `rvk_tests` 231/231 (incl. `rain_*`, settings count 161), `rvk_d3d9test --validation` Vulkan + D3D12 0 failures / 0 validation errors, `rvk_rhitest` PASS,
`rvk_shadertool dxil-check embedded` 64/64 (3 new), testdemo 7/7 with `-Validation` on Vulkan and D3D12 (`RVK_NO_GBV=1`) with `materials.quality=high` + `materials.rain_force=1`: 0 VUID / `d3d12: validation`.
No corpus variant was touched (no change in `rvk_material_variant.hlsli`, the G-buffer layout or the ABI), so the corpus checks do not apply.
Pictures: `out/water/20261105-131557` (dry, 4 spots), `-132159` / `-132526` (forced rain: the pier turns dark and glossy, the forest floor gets dark puddles, no patchiness after the roof-ray fix).

## 7. Not built / evidence
* **GI follow-ups (feature 3)**: see section 9.
* No screen-space march fallback for GPUs without ray query (the option `materials.quality = high` is unavailable there; the raster's sky cube stays).

## 8. Glass panes (setting `materials.glass`, default on; acts with the traced reflections)
Findings that shaped it [C]: the sample pack's glass entries are drawn two ways. **Windows** (`Arch_Glass_01_Diffuse_01`, mesh `Obj_Window_Harbour_01`) go through the **refracted pass** (kind
Refracted, like water, the engine's pixel shaders sample the refraction copy) and were excluded from the RT scene as "water"; other glass (`ItMisc_01_Diffuse_01_Glass`, bottles) is drawn unblended
(blend 0) in the main view and is already an ordinary opaque instance (nothing is added for it: the traced material reflections cover it through its G-buffer entry: smooth, metal exempt from the cap).
* **TLAS**: while the glass pass is on (`RtScene::setGlass`), a main-view draw of a pack entry with transmission > 0 that is blended or in the refracted pass (and no real water) enters the scene as an
  ordinary instance whose record carries `kRtRecordGlass` and whose TLAS mask is `kRtMaskGlass` (8). `rvkRtTraceMask` now excludes that bit from every ray; only `RVK_RT_TRACE_GLASS` rays see it:
  shadows, GI, the water, the material reflections are unchanged. rvk.log `rt: scene: glass pane BLAS #n: material ...`. Off = `isGlassDraw` is false at once: the observer path is the old one.
* **Pass** `materials.glass` at AfterMainView (order 20; the scene HDR holds the engine's blended glass; skipped when the TLAS holds no glass instance): `material_glass.comp` casts the primary ray of every
  pixel against the glass only (to just behind the depth map's opaque surface) and, on a pane: Schlick Fresnel (F0 0.04) x a reflection traced like the material reflections' (screen reuse, engine-style
  hit shading, sky cube), capped at 2x the pixel's own brightness; **thin** panes (the exit face within 8 cm and parallel) add nothing else (the engine's blend shows what lies behind a slab); **thick / curved**
  glass refracts in (eta 1/1.5), a ray inside finds the exit face, refracts out (total internal reflection reflects), the transmitted ray's hit replaces 80 % of (1 - F) of the scene behind it, tinted by the
  glass' diffuse colour. The composite is the material reflections' (`src.rgb + dst * src.a`). The engine's blend is kept as the base: off = nothing changes.
* **Seen** [C]: harbour windows: 12 glass instances in the TLAS in the harbour spots, the pass runs (`enh: materials.glass: first glass frame`), no validation messages. The visible effect in the available spots is **subtle**:
  the harbour_night windows are dark panes in shadow reflecting a dark street (4 % Fresnel at a steep angle); a daylit window at a glancing angle would show it and was **not** captured. Refraction through thick glass
  (bottles) is implemented but no bottle is drawn through the refracted / blended path in the test spots (the pack's bottles are opaque draws), so **it is untested in the game**.

## 9. GI follow-ups (feature 3): not built, and why
`path-tracing.md` 10.3 already evaluated the three candidates with the reference path tracer: the **sea proxy** (measured: it would push the beach's under-estimate the wrong way: the sea floor / sky the rays see now are brighter than
a dark sea albedo; ~5 % of the beach picture), **contact occlusion** (`gi.screen=high` gives 0 - 5 points of mean relative error at extra probe cost; a GTAO / bent-normal blend is the open idea), and the **moving light**
(not measured: a converged reference needs a still scene). None of them was started here: each needs hours-long converged reference runs per spot (1024 spp, tens of GB of dumps) to show a gain, which did not fit this round; the
decisions stand as documented there. Open ideas, unchanged: a GTAO term blended with the traced ratio, a hybrid far field (far probe occlusion with the SH's shape), a moving-light convergence test (a light spell
orbiting a still camera, compare the DDGI plane against a reference restarted per light position).
