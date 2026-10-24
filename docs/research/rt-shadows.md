# Ray-traced soft sun shadows (M12 RT shadows)

Status: stage 1 built (2026-10-24): `src/enh/rt_shadows.*`, `shaders/shadows/rt_shadow_*.comp`, the mask shader's RT term.
Settings category Shadows (`shadows.rt` off | near | extended, default off). Needs the RT tier (ray query) and the enhanced
shadow mask (`shadows.enabled`). [C] = measured in the running game, [I] = inferred / design intent.

## 1. Idea and design [I -> C]

The enhanced mask (shadows.md 4) already is the seam: one fullscreen pass writes the sun's shadow alpha from the cascade maps
(`enh/shadows`). Within the near range the value now comes from shadow rays against the RT scene (enh/rt_scene, rt-reflections.md 3)
instead, blended out to the cascades over the last quarter of the range; beyond it nothing changes. The engine's cascades keep
rendering (they feed the mask beyond the range and, as a side effect, the RT scene's registry: casters seen only by the cascade
views stay in the TLAS, rtgi.md 8 / rt_scene.h).

Where it runs: three compute passes at `AfterDepthPrepass` (order 12, after `rt.scene` built the TLAS at -100), before the mask
is drawn (UpdateShadowMask follows EndZPass: frame-phases.md). Same inputs as the mask: the engine's linear depth map, the same
reconstruction (`2 x / size - 1` NDC minus the camera jitter), so the RT term lines up with the cascade term pixel for pixel.

| pass | shader | role |
|---|---|---|
| trace | `rt_shadow_trace.comp` | per pixel within the range: 1 (2 at High) shadow ray towards a random point of the sun's disc (cone half angle `shadows.rt_softness`, default 1 deg, the real sun is 0.27); origin = surface point + normal x (1.5 + 0.0006 x distance); `rvkRtVisibility` with the alpha test of cut-outs in the candidate loop (Low: cut-outs solid); interleaved gradient noise per pixel, shifted per frame / ray; reach 120 m |
| temporal | `rt_shadow_temporal.comp` | reprojection from the camera (prevViewProj, previous jitter), 4 depth-checked bilinear taps of the history (RGBA16F: mean, view depth, frames, second moment), history clamped to the raw 3x3 range, 1 / n blend up to 12 frames |
| spatial | `rt_shadow_spatial.comp` | depth-plane bilateral 5x5 (7x7 for the first frames) weighted by the variance (second moment - mean^2) and the history length: converged lit / blocked pixels are untouched, so hard contact edges stay hard; only penumbra pixels are smoothed |

The penumbra is geometric: a cone of half angle a gives a penumbra width 2 d tan a at occluder distance d, so contact hardening
needs no hit distance (the PCSS of shadows.md 6 is not needed). The normal comes from the depth map (one-sided differences, as in
GTAO), not from the G-buffer (not armed at AfterDepthPrepass).

Sun: the cascade object's light direction (`eCCascadeShadowMap +0x10`, read in the `RenderShadowMaps` pre-hook:
`engine::shadowmaps::sunVector`) is the direction the light travels [C: log "cascade object stores (-0.174, -0.853, 0.492)" with the
sun above: the rays go to the negation]. No cascades this frame (interiors without sun shadows) = no pass.

Mask: `shadow_mask.frag` takes `rt` (the final image's slot) and `rtParams` (range in depth-map units, blend band); inside the range
`mask = mix(cascade x terrain, rtVis, w)`, w = 1 - smoothstep(range (1 - band), range, d). The terrain variance term goes with the
RT value (the rays see the terrain). Debug `shadows.debug = rt_term` (the RT term alone, lit beyond the range) / `rt_coverage`.

Settings: `shadows.rt` (off / near 15 m / extended 35 m; the enhanced cascades split at 13.5 / 29.5 m), `shadows.rt_quality`
(low / medium / high), `shadows.rt_softness` (deg), `shadows.rt_range` (m, auto from the mode). rvk.log `enh: shadows.rt: ...`.

## 2. Guarantee: nothing changes while it is off [C]

The user's constraint: the RT shadows must not touch the regular render parts. How that holds, and how it was checked:

* **Off = nothing exists.** `RtShadows` builds nothing in its constructor; with `shadows.rt = off` (default), without RT hardware
  (`caps::Cap::RayQuery`: the setting's effective value is `off`), or with `shadows.enabled = false` there is no layout, no
  pipeline, no thread, no transient image, no injected pass, no RT scene acquire (`RtScene::acquire` / `setDynamic` happen in
  `apply()` when the mode turns on, and are undone when it turns off: `removePasses`, `release`, transients released). The three
  compute pipelines start building in the background on the first switch on; until they are ready the passes skip and the cascades
  give the mask (no hitch).
* **The mask shader** takes the RT value only where `ShadowBlock::rt.x != 0`; the C++ side leaves it 0 unless this frame's RT
  image exists, has the mask target's size and the camera is valid. With 0 the arithmetic of the pass is the old one
  (`mask = shadow * terrain`; the new debug modes 4 / 5 are separate branches). Frames where the RT passes skipped (no cascades in
  an interior, RT scene not valid, pipelines not ready) use the cascades exactly as before.
* **The engine's cascades are untouched in every mode**: the sun cascades keep their size, range, split factor and stride
  (`shadows.map_size` / `range` / `split`, the stride throttle) whatever `shadows.rt` says; the `RenderShadowMaps` pre-hook only
  reads the light direction (`sunVector`). They also keep feeding the RT scene's registry with casters the camera does not see.
* **Skipping / shrinking cascade 0 / 1 is not built.** If it ever is, it may only happen while the RT term is active for that
  range (RT image valid this frame, mode > 0) and must restore the user's / the game's cascade settings the moment the RT term is
  turned off or a frame has none (the `Tracked` game values of `engine/shadow_maps.cpp` are the restore point).
* **Checked** (`out/shadows/rt-off-check`, spots shadow_inland_b / beach_sun / hero_closeup / pub_interior, `shadows.enabled`
  true and false, `-Perf`): the build before this work (HEAD in a separate worktree) against the new build with `shadows.rt` at its
  default and set to `off` explicitly. The game's own run-to-run noise (eye adaptation, particles; frozen game time) is already
  a mean absolute difference of 0.9 - 2.5 (of 255) between two runs of the SAME build; old vs new build: 1.0 / 2.4 / 3.8 / 2.1
  (pub / hero / beach / inland_b, enabled), 0.7 / 2.2 / 2.3 / 2.1 (disabled): inside that noise. Draw counts per frame
  (1364 - 1382 in the steady windows), cascade update / skip counts (`engine: shadow cascades: N updates, M skipped`: about
  1 : 1 at stride 2) and pipelines built are the same. The mask shader is identical by construction when `rt.x == 0`.

## 3. Results (2026-10-24, 1600x900, RX 7900 XTX)

* **Agreement with the cascades** [C]: at `shadow_inland_b` / `_top` the ground shadows, the palm and bush shadows and the cliff's
  shade are where the enhanced cascades put them (`out/shadows/20261024-144717`, `out/shadows/rt-burst1`); the RT edges are a little softer
  at the occluders' far ends (the penumbra grows with distance), sharp where an object touches the ground. The cascade-term / RT-term
  debug views (`shadows.debug`) differ mostly on the fronds of the palms themselves (rest pose in the RT scene against the animated
  raster mesh: rt-reflections.md 3 "misplaced") and beyond the range. Characters (`hero_closeup`, `hero_idle` with actors) show no
  artefact; the hero is in the RT scene (previous frame's skinned vertices: a moving character's shadow lags one frame).
* **Stability** [C] (12 consecutive captures, frozen game time, luma std mean): cascades 0.214 / 0.003, RT extended 0.258 / 0.019
  (`shadow_inland_b` / `_top`): the 1 ray per pixel noise is fully absorbed by the temporal + spatial filters.
* **Cost** [C] (`-PerfFrame`, `shadow_inland_b`: ground-level forest, almost every pixel inside 35 m; total GPU 1.30 ms with the
  cascades alone): `near` (15 m) trace 0.42 ms; `extended` (35 m, medium) trace 0.96 ms; `low` 0.69; `high` (2 rays) 1.33; temporal
  0.06 + spatial 0.07 ms in every mode. Total GPU per frame: off 1.30, near 1.96 - 2.21, extended 2.05 - 2.08, extended high
  2.87 - 2.94 ms. The cascades' map passes cost 0.07 + 0.25 + 0.12 ms there and keep running: the RT term is a quality feature, not
  a saving.
* **Validation**: `-Validation` run (extended, high, inland_b + hero_closeup): no VUID messages; the same with `-SyncValidation`: no hazard messages.

## 4. Open

* Cascade 0 / 1 skipping or shrinking (CPU / GPU saving ~0.4 ms + the casters' draws): needs the RT scene to keep the off-screen
  casters without the cascade views, and the guarantee above.
* Cost: a coarse first pass (checkerboard / half-rate tracing with the history), a cheaper candidate loop for alpha-tested leaves
  (a pre-baked opacity), `rt_range` per quality.
* Wind: the RT scene holds SpeedTree fronds in their rest pose; the raster is animated, so frond self-shadows differ slightly.
* Interiors with sun shadow views (windows) work like outdoors; interiors without shadow views have no pass.

## 5. Fixes after the first player test (2026-10-24)

The player reported (1) trails of shadow behind a walking hero, plus a comb pattern along shadow edges, (2) shadows that move in
big chunks, (3) no wind in trees / grass. Test scenario `mods/rvk_rtwalk` (a quicksave or a new game, the hero walks with
`keys=MoveBackward` / `MoveForward`, a picture burst via `risen.debug.screenshot`: `walk2`-style runs, e.g. `-Mods rvk_rtwalk
-Scenario rvk_rtwalk -Param 'load=1','hour=18','keys=MoveBackward','burst=40','interval=10','name=rt1'`; the quicksave's
AutoSave of an earlier run in the test copy would be loaded instead: delete it first).

* **Chunky sun** [C]: `eCDirectionalLight_PS::UpdateShadowMaps` (0x591fb0) keeps the sun's true direction at `this+0x80` and a copy at
  `+0x8c`; the copy follows only after the sun turned ~0.5 degree and is what the cascade object (`+0x10`) gets. The RT shadows read
  the cascade object's copy: the shadows jumped in 0.5 degree steps (logged: the vector changed every ~15 frames at 20 game
  minutes per second). Now `engine::shadowmaps::trueSunVector` (a pre-hook of that function, read-only) feeds the rays: the sun
  changes every frame (checked in the same scenario). The cascades keep the engine's stepped vector (what they were rendered with).
* **Trails** [C]: not the temporal filter. Grass blades (small `StaticMesh` / `InstancedMesh` ranges of ~200 triangles, 16 x 6 x 80
  units) get new world rows almost every frame while the camera moves (fading by distance / per-frame instance data): each new row
  was a new registry instance (key = BLAS + quantised rows) without an owner, kept for 600 frames / 20 s. While the hero walked 5 s the
  registry went 490 -> 25,558 instances (`added 48,223, replaced 21,956`) and the TLAS filled to its cap of 16,384 with ghosts of
  places the blades had been: shadows of every earlier frame. Fix (`RtScene::noteNewInstance`): a range with >= 48 new instances in 90
  frames is volatile for 360 frames; the TLAS takes its instances only while seen in the last 3 frames, and every 16 frames the
  registry drops the ones unseen for 12 (`Registry::removeIf`, unit test `rt_registry_remove_if`). After: TLAS 513 - 605 instances,
  registry ~700 during the same walk.
* **Comb** [C]: along every terminator (the sun grazing a wall or a tower) the random cone samples that point below the surface hit
  the surface itself; the interleaved gradient noise made a diagonal comb of it. A sample with `dot(dir, N) < 0.02` is now counted lit
  (the material's own N.L darkens the terminator). Gone in the same scenario.

## 6. Trees in the wind: animated geometry = the vertex shader's output, captured (2026-10-24)

The RT scene holds SpeedTree branches / fronds in their rest pose, and no leaves at all (rtgi.md 8 / the "leaves (op > 1)"
rejection): the cascades' shadows (animated mesh) and the RT ones differed, and the jungle's bushes cast no RT shadow. The
character mechanism (rt-reflections.md 3, rtgi.md 9: a vertex shader variant writes the world positions into the frame's ring, one
BLAS per draw is refitted from it) now covers the trees too:

* `RtScene::observeTree` (called for SpeedTree draws of the main view's solid pass, plain draws and instanced batches; within
  `shadows.rt` range + 15 m of the camera, at most 160 draws a frame): `Device::requestVertexCapture(buffer, instances)`; the capture
  variant (tag `cap4`) writes instance k's vertices at `(k x slots + index)` (`gl_InstanceIndex`; `draw.cpp` lets instanced draws
  take the capture only when the request says so). `slots` = the BLAS's highest index + 1; strips use the converted list indices of the
  rest-pose BLAS (same vertex numbering).
* The animated BLAS (`DynBlas`, key = the rest-pose instance's key) is refitted from the **previous frame's** capture at the
  next AfterDepthPrepass (a rebuild every 30th refit, staggered per tree); its record is the rest-pose instance's (material, alpha
  test, flags) with the captured vertices (stride 32, TEXCOORD0 at 16) and the world masks (not the characters' mask: `gi.characters`
  does not leave trees out). While it was built this frame the rest-pose instance is left out of the TLAS (`animatedKeys_`).
* Leaves (`op > 1`, vertex shader class Leaves) have no rest pose: their BLAS exists only for topology and material (`animatedOnly`:
  never an instance of its own), the animated capture is all of them. Grass / ferns (vertex shader class Vegetation, instances whose
  rows change every frame) are still not in the RT scene.
* Setting `shadows.rt_wind` (default on, Advanced). Cost [C] (beach, 16 trees in range, near mode): GPU +0.07 ms, CPU +0.10 ms per
  frame; 81 trees at `extended`.
* Checks [C]: validation + synchronization validation clean (walk, extended, high); `rvk_shadertool corpus corpus --capture` 0 failures
  (cap4), `--gbuf-motion` 0; `rt.debug=depth` at the beach: red (RT lacks geometry) -5 % with the leaves in range, magenta (misplaced)
  unchanged.
