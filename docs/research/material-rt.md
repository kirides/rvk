# Traced reflections of the material pack's surfaces, rain wetness and puddles

Status: built 2026-11-05 (features 1 and 4 of the RT follow-up list; glass and the GI follow-ups: section 7). Legend: **[C]** confirmed in a game run, **[I]** inferred.
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

## 6. Costs and gates
GPU time of the pass `materials.rt` (trace + filter + composite, `-PerfFrame 4300`, `RVK_PERF_FRAMES=24`, 24 frames averaged, RX 7900 XTX, 1600x900, harbour_docks hold of rvkcompare, tier medium,
sample pack, other agents' games on the same GPU: +-0.3 ms noise; the span includes the wait for the solid pass to drain):
| | dry (`rain_force=0`) | pouring (`rain_force=1`: every flat pixel traces, roof rays) |
|---|---|---|
| Vulkan | 3.1 - 3.5 ms | 3.0 ms |
| D3D12 | 1.3 - 1.6 ms | 1.9 ms |
A first version of the filter gathered 4x4 taps, the final one 2x2 (3x3 at full rate): no measurable change, so the filter is not the main cost; the trace and the pass's barriers are. Not
optimised further (a tier-low option exists: no shadow rays at hits, cut-outs solid). Vulkan costing more than D3D12 is unexplained (same HLSL; probably the RT scene wait / barrier span).
Gates of the commit: `rvk_tests` 231/231 (incl. `rain_*`, settings count 161), `rvk_d3d9test --validation` Vulkan + D3D12 0 failures / 0 validation errors, `rvk_rhitest` PASS,
`rvk_shadertool dxil-check embedded` 64/64 (3 new), testdemo 7/7 with `-Validation` on Vulkan and D3D12 (`RVK_NO_GBV=1`) with `materials.quality=high` + `materials.rain_force=1`: 0 VUID / `d3d12: validation`.
No corpus variant was touched (no change in `rvk_material_variant.hlsli`, the G-buffer layout or the ABI), so the corpus checks do not apply.
Pictures: `out/water/20261105-131557` (dry, 4 spots), `-132159` / `-132526` (forced rain: the pier turns dark and glossy, the forest floor gets dark puddles, no patchiness after the roof-ray fix).

## 7. Not built / evidence
* **Glass / window transmission (feature 2)**: not built. Findings: (a) the RT scene deliberately excludes alpha-blended meshes of the main view (`RtScene::observe`: "blended in the main view" is rejected),
  so a window pane is not in the TLAS and a camera ray cannot find it; (b) a blended glass draw has no position of its own in the material variants (they rebuild positions from the depth buffer, which holds
  the opaque scene behind the pane), so neither the raster variant nor a G-buffer pass knows where the glass is; (c) the pack word has no free bit for ior / tint. A real implementation needs the glass
  meshes in the TLAS as their own mask (a flagged instance class), a glass id in the G-buffer (metalness code 15 is free: real metals clamp to 14) and a pass after the alpha passes that traces the
  primary ray to the pane, adds the Fresnel reflection and (for thick glass) refracts through two interfaces. Windows lit from inside seen from outside already work through the engine's blend.
* **GI follow-ups (feature 3)**: not started in this round (sea proxy / contact occlusion, moving-light response, probe improvements).
* No screen-space march fallback for GPUs without ray query.
