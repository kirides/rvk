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

## 7. Characters: this frame's pose from the cascades' draws (2026-10-24)

The player asked to skin the characters on the GPU in a compute / vertex pass whose result both the RT scene and the raster draws
use. What was measured and built instead:

* **Size of the lag** [C]: the RT scene took the previous frame's captures (the main view's solid pass comes after the shadow mask).
  The hero walks 340 units/s: 1.7 cm per frame at 200 fps, 5.7 cm at 60 fps (a few pixels at 3 m, visible as a gap at the foot's
  contact at low frame rates; not the cause of the trails, which were the ghost instances of 5).
* **Why not a skinning pass of our own** [I]: the bone matrices are VS constants (`g_vInstanceData[idx x 3 + 1 ..]`, engine-gfx.md)
  that exist only when the engine issues the draw; the engine skins in the vertex shader, there is no earlier place that owns the
  palette (the render ops are built from the animation's matrices inside `eCAnimation_PS::Render`), so a compute pass would need a
  second copy of the engine's animation output and a replacement vertex shader for every skinned material. Not built.
* **Built**: the sun cascades are drawn before the main view of every frame, with the same bone constants: the skinned draws of the
  cascades' views (`Shadow`) are captured by the same vertex shader variant (`cap5`; in a cascade's view the world position is the
  operand the shader multiplies with its view-projection: `GlslOptions::dotCaptures` on `g_vViewProjection`, header bit 31 of the
  slot count; the main view keeps the clip-position path). `RtScene::buildDynamic` takes per key the newest capture that ran: this
  frame's (cascade views, or the main view when the build runs at AfterOpaque), else the previous frame's. The main-view capture
  still runs for the motion vectors (the previous main-view capture chain is kept separately). Characters outside the camera's view
  but inside a cascade now cast RT shadows too (characters in the TLAS 28 -> ~57 in the harbour).
* **Cost / switch** [C] (harbour quicksave, walking, `shadows.rt=near`, `-Perf`): the engine redraws its cascades at most every other
  frame while the camera barely moves (`debug.shadow_stride`); with RT shadows on they are now drawn every frame (`debug.rt_shadow_sync`,
  default on, Debug): CPU 5.7 -> 6.2 ms, GPU 3.2 -> 3.6 ms, frame interval 5.8 -> 6.1 ms (RT shadows off: 4.9 / 1.9 / 5.0 ms). Off: the
  stride decides and about a third of the character refits use this frame's capture, the rest the previous frame's.
  Refits from this frame's capture: 55 % of all (all characters, none of the trees) with the switch on, 30 % off.
* Open: a velocity extrapolation of the previous captures would remove the remaining lag without the cascades' cost; the GI still uses
  the previous frame's characters.

## 8. Cheaper tracing: profile and what did not pay (2026-10-24)

Question: cut the GPU cost of the trace pass without visible loss (no new trails / flicker / comb). Everything below was measured at
`shadow_inland_b` (the forest-floor spot, almost every pixel inside 35 m), `shadows.rt = extended`, quality medium, 1600x900, RX 7900
XTX, the camera held (rvkcompare hold), `RisenTest.exe` (uncapped). New tooling: the environment variable `RVK_PERF_FRAMES=M` (with
`run.ps1 -PerfFrame N -Environment 'RVK_PERF_FRAMES=24'`) logs the pass timings of the M frames from N on, so a pass time is an
average of 24 frames instead of one noisy frame.

**Where the time goes** [C] (average of 24 frames): rt.scene (TLAS) 0.22 ms, trace 0.78 - 0.84, temporal 0.06, spatial 0.07 - 0.14.
A frame with RT shadows off: GPU 1.84 ms, CPU 3.13 ms; RT shadows on with `debug.rt_shadow_sync` off: 2.45 / 4.06; with the sync
(default): 2.67 / 4.37. So the RT term costs more CPU (+0.9 ms: observing the draws for the RT scene, the TLAS instance assembly,
the tree captures) than GPU, and the game is CPU bound; the cascade sync adds +0.22 GPU / +0.30 CPU while the camera is held (while
the camera moves, the engine redraws the cascades every frame anyway). The trace pass itself is set up cheaply (depth fetches, the
normal): 0.04 ms with the rays removed; the rest is ray traversal.

**What did not pay** [C] (trace pass ms, medium, extended; baseline 0.78 - 0.84):

| experiment | trace ms | verdict |
|---|---|---|
| ray reach 120 m -> 40 m | 0.84 | no change: lit rays do not travel far, the cost is the canopy near the surface |
| cap of 4 / 8 / 1 alpha-tested candidates per ray | 0.83 / 0.91 / 0.87 | no change: the leaf alpha test is not the tail |
| opacity mip x4 (coarser leaf alpha fetch) | 0.84 | no change |
| TLAS built `PREFER_FAST_TRACE` | 0.82 (rt.scene 0.22 -> 0.63) | no trace gain, 3x build |
| animated tree BLASes `PREFER_FAST_TRACE` | 0.88 (rt.scene 0.22 -> 0.36) | no trace gain |
| the same sun-disc sample for each 4 x 4 / 2 x 2 pixels (coherent rays) | 0.76 / 0.82 | within noise: not divergence bound |
| only 1/4 of the lanes trace / 1/4 of the 8 x 8 tiles trace / 1/32 of the tiles | 0.70 / 0.58 / 0.25 | the cost is far from proportional to the rays: a fixed tail of ~0.2 ms remains with 3 % of the tiles; skipping most rays buys ~0.25 ms |
| quality low (cut-outs solid), full / 1/32 of the tiles | 0.78 / 0.125 | the tail is the canopy's traversal through leaf cards (rays that pass the gaps keep going) |
| coarse pass (one ray per 2 x 2 pixels, every frame) + the fine pass skipping rays where the 3 x 3 coarse blocks agree, the depth fits and the history is converged to the same value | coarse 0.45 + fine 0.61 = 1.06 | **worse than no skipping** (0.84): the coarse pass alone costs 55 % of the full trace for 25 % of the rays, and the converged-and-calm test passes for few pixels where the wind moves leaves (jungle, beach, harbour: 0.96 - 1.06 against 0.83) |

Why ray skipping barely helps: the pass time is dominated by the slowest rays (long traversals through overlapping leaf-card
instances) and a per-wave tail, not by the number of rays; a wave with one blocked / lit mix still pays its slowest lane. Staleness
would also come with it (a shadow edge entering a skipped area shows one frame late: the player had just reported trails). Dropped.

**What was kept**
* **The second ray of `rt_quality = high` only where it can matter** (`rt_shadow_trace.comp`): a pixel whose history is settled (>= 4
  frames, variance < 0.002, mean < 0.03 or > 0.97) is confirmed by one ray as well as by two; penumbra and new pixels keep two. Trace
  1.34 -> 1.23 ms (-8 %; the pixels that still take two rays are most of a forest floor). Stability (12 consecutive captures at
  `shadow_inland_b`, high): luma std mean 0.319 against 0.299 for two rays everywhere, p99 3.27 / 2.78; the two runs' median images
  differ by 1.88 (the game's own run-to-run difference is 0.9 - 2.5). Validation + synchronization validation (walking scenario,
  extended, high): no messages; `rvk_tests` 133.
* `rtsHistory` (the history reprojection of the temporal pass) moved into `rt_shadow_common.glsl` so the trace pass can read it; the
  temporal pass is arithmetically unchanged.

**Open** (where the real cost is): the CPU side of the RT scene (observe 0.42 ms + TLAS assembly 0.34 ms per frame at ~890
instance sightings) and shading fewer rays by a cheaper *scene* (a shadow LOD of the leaf cards, distant canopies as solid blobs) rather
than by skipping pixels; velocity extrapolation of the characters' last capture instead of the cascade sync (-0.22 ms GPU / -0.30
ms CPU with the camera held).

## 9. RT scene CPU (2026-10-24)

Profile at the harbour quicksave (`Risen_Save_0007`-style player save, detail 2x, RT shadows extended, 1600x900, CPU bound: frame 8.4 ms,
CPU 8.2 ms) with temporary section timers [C]: the pass (`rt.scene @AfterDepthPrepass`, 0.46 ms CPU per frame) was `buildDynamic` 0.30
(of that `vkCmdBuildAccelerationStructuresKHR` recording 0.226: 194 refits per frame = 85 skinned draws + 109 trees at ~1.2 us of driver
time each), selection 0.046, assembly of instances and records 0.062, ageing 0.02; the draw observation 0.67 ms (1,550 draws, 1,400
sightings; with the `-Perf` timers). The sampled CPU profile puts the whole RT scene at ~3 % of the engine thread. What was done:

* **Trees are refitted every other frame** (staggered by their key; `RtScene::buildDynamic`): the wind is slow, the BLAS of the frame
  before stands in and the instance still takes this frame's capture for the vertex reads (the capture itself is written every
  frame). 109 -> ~55 refits per frame: pass CPU 0.46 -> 0.37 ms, GPU 0.25 -> 0.23 ms.
* `vkGetAccelerationStructureBuildSizesKHR` per captured draw per frame is cached per BLAS (it only depends on the counts and the index
  type): -0.03 ms.
* `materialOf` answers repeats of the same BLAS / signature / rank in a frame from a memo (a range is sighted in several views and
  instances), the water-key lookup runs only for ranges without a BLAS and `observe` looks the BLAS up once: observe 0.67 -> 0.64 ms.
* `logStats` no longer walks every BLAS each frame, only when its numbers are shown.

Together about -0.12 ms CPU per frame in this scene (1.5 % of the frame); validation + synchronization validation (walking scenario,
GI + RT shadows extended): no messages; `rvk_tests` 133. **Not done / the rest**: the 85 skinned draws' refits (1.2 us each; their
distance is not known on the CPU: far characters could be refitted less often), the registry's per-sighting hash lookups and
`Registry::see` (cache-missing `list_` entries), skipping the cascade views' sightings of draws the main view already registered.

## 10. Characters skinned by compute from the engine's own inputs (2026-10-24)

The player asked: "can't we just get real character skinning data instead of extrapolating". Yes: the inputs are in hand at the draw.

**Where the data lives** (engine-gfx.md 9, [C] from the decompilation, matching the VS listings of the corpus):
the engine skins in the vertex shader (hardware skinning, EMotionFX). A skinned draw's vertex shader reads the bone matrices from the
constants `c33 + 3 b` (rows of a 3x4 matrix; bone `b` = BLENDINDICES UBYTE4, 4 influences with BLENDWEIGHT0 FLOAT4) and computes
`world = sum_k w_k (row_j(b_k) . (position, 1))`; the morph variants add `position += POSITION1..3 x c31.xyz` first (the heads;
the weights are zero most of the time). `eCAnimation_PS::Render` (0x4d6fe0) -> `eCWrapper_emfx3ActorInstance::UpdateRenderMatrices`
(0x4dfee0: fills the actor's palette once per frame, stamped with the frame id at `+0x58`: an array of 0x40-byte matrices at `+0x48`,
count `+0x50`) -> `eCAnimation_HardRenderActor2::Render` (0x4dd650) which hands the matrix of each of a sub-primitive's remapped
bones to the shader (vtable `+0x1f0`, slot i = palette[remap[i]]), then `RenderPrimitive` draws. So the constants of any skinned draw
of the frame, in the z pre-pass or in a sun cascade, hold this frame's final pose.

**Built** (`shaders/rt/rt_skin.comp`, `RtScene::observeSkinned / recordSkin`, setting `debug.rt_skin`, default on): at each skinned draw
of the z pre-pass and of the cascades the scene copies c32..c231 (3.2 KB) into the frame's ring and records a job (the draw's static
vertex buffer by address, its stream offset and base vertex, the layout offsets from the declaration, the capture-format output buffer
of the BLAS builder). One compute dispatch before the BLAS builds skins all jobs (64 vertex slots per workgroup, the job found by binary
search over the chunk starts) and writes the capture layout: world position (w = 1) + TEXCOORD0 per vertex-index slot, so the BLAS
refit, the material vertex reads and everything downstream take it unchanged. The z pre-pass is complete before the TLAS is built
(AfterDepthPrepass), so every character on screen is this frame's pose with no cascade redraw. The solid pass keeps its vertex shader
capture for the motion vectors (the previous main-view positions) and as the fallback for a draw the pass cannot take (active morph
targets, a dynamic vertex buffer, another declaration). Floor: no Int64 (the address maths is a 32-bit add with carry).

**Verified** [C]:
* `rvk_vktest --skin`: the compute shader against a CPU reference of the vertex shader (two jobs of different layouts in one dispatch,
  bone indices past the palette, random weights): max error 2.4e-5, no validation message.
* In the game (`debug.rt_skin_verify`: the cascades' draws also run the capture variant and the compute pass compares both): max error
  0.006 - 0.012 world units (0.1 mm) over ~2 million vertices per second, morph draws with zero weights included. Slots the reference did
  not write are index gaps of a draw (not referenced by its triangles).
* Validation + synchronization validation (walking scenario, harbour, extended, RT shadows): no messages.
* `rvk_tests` 133.

**Result** (harbour quicksave, walking, `shadows.rt=extended`, `-Perf`, one run per configuration: treat 0.1 ms as noise):

| configuration | characters from this frame's pose | engine+HAL CPU | GPU | frame p50 |
|---|---|---|---|---|
| RT shadows off | - | 4.41 ms | 1.15 ms | 3.56 ms |
| old default: capture + cascade sync (`debug.rt_shadow_sync` on) | 95 % | 5.36 | 2.06 | 4.64 |
| capture, no sync | 48 % | 4.99 | 1.89 | 4.15 |
| **skinning pass** (the new default, sync off) | 74 % (every character the z pre-pass draws: 100 %) | 5.11 | 1.89 | 4.35 |

The rest of the 74 % are characters outside the view that only a sun cascade draws, in frames in which the engine skips its cascades
(`debug.shadow_stride`): they keep the previous frame's pose (their shadows reach the picture only as a one-frame lag of off-screen
casters). The pass itself costs nothing measurable (`rt.scene` pass 0.206 ms CPU / 0.125 ms GPU with and without). 93 % of the cascade /
pre-pass draws are taken (the others have active morph targets). Against the old default the sync's cost is gone (-0.25 ms CPU,
-0.17 GPU, -0.3 ms frame) with the hero and every other visible character in this frame's pose; `debug.rt_shadow_sync` now defaults
off (kept as the way to get the off-screen casters of this frame too).

**Not done / open**: the engine's palette (`UpdateRenderMatrices`) is only filled for actors it renders, so an actor outside every view
has no fresh matrices to read anywhere (calling it ourselves would need the actor registry: not attempted); morph draws with active
weights (faces speaking, 7 % of the draws) keep the capture; the motion vectors still come from the main-view capture; the engine's
own draws still skin in their vertex shaders (sharing the result with them needs a replacement vertex shader for every skinned
material: not built, no frame-time gain expected since the draw calls stay).
