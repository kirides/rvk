# The engine's frame, decompiled (2026-10-27)

The complete render sequence of Risen Enhanced (x64), read from Engine.dll (headless Ghidra, six parallel slices) and checked
against a recorded frame. Purpose: the specification of the **native frame** (native-renderer.md N5): first a faithful copy of
this sequence on our Vulkan backend, then our own optimized modes step by step (section 4).

[C] = read in the decompilation, [C, run] = also seen in the game, [I] = inferred, [?] = open. RVAs are Engine.dll.

| slice | file | contents |
|---|---|---|
| A | [A-frame.md](A-frame.md) | `OnRun` / `Process` / `DoRender` / `Draw`, app states, camera update, fog path, sky cube timing, image-effect jobs, scene targets, off-screen views = pre-passes, module callbacks |
| B | [B-frustum-db.md](B-frustum-db.md) | `eCFrustumDatabase`: extract (sphere tree, portals, view range, fade, LOD), request (list rules, request mask), `Execute` step by step, sort keys, render op / instance layouts, shadow-map databases; deviations of `native_opaque_list.cpp` |
| C | [C-materials.md](C-materials.md) | shader material admin, static mesh ops per pass, render states per blend mode, samplers, VS c0-c251 / PS c0-c37 with writers, permutation rules (6 VS x 48 PS + special passes), material graph runtime, instancing; gaps of the native shaders |
| D | [D-contexts.md](D-contexts.md) | skinned actors (+ morphs, body parts), SpeedTree (+ billboards), vegetation, particles, strips, light streaks, billboards, decals, precipitation, debug: lists, geometry, shaders, constants, states |
| E | [E-shadows-lights-sky.md](E-shadows-lights-sky.md) | terrain VSM, sun cascades (fit, snap, bias, formats), packed point-light atlas, `post_SSM.fx` mask graph, light sets / selection, hemisphere, weather, sky dome + sky cube |
| F | [F-post-water-gui.md](F-post-water-gui.md) | image-effect graphs (13, decoded from `compiled_imageeffects.bin`), the default DoF frame's 20 jobs with maths and constant sources, water / refraction, GUI sprite batcher, snapshot, present |

Dumps (outside the repo): `B:\DevDrive\risen-ghidra\frame{A..F}_dump*.txt` + helper scripts `frame{A..F}_*`; the recorded frame:
`B:\DevDrive\risen-ghidra\frame_logframe_save0009.log` (`-LogFrame 2710`, user savegame Risen_Save_0009, engine path, 936 draws).

## 1. One world frame, in order

```
eCApplication::OnRun
 Process                       modules (eCProcessor, 256 priority buckets): weather (fog -> camera +0x15c0, sun, hemisphere, sky),
                               illumination (light sets, sky cube step), image filter admin (overlay weight, RT pool GC), ...   A 1
 DoRender
  back buffer bound, ClearViewport(colour|z|stencil, 0, 1.0, 0); OnPreRender = CacheEntities                                     A 2.1, 2.5
  [app state & 3 != 0 (ESC menu, loading): nothing more; GUI + present]                                                          A 4
  [sky cube: every 10 s, 6 consecutive frames: one 128² face per frame (90 deg, near 1, far 1000), -Y only cleared grey;
   sky / clouds / stars / sun / moon only; runs inside eCIlluminationAdmin::Process]                                              A 2.4, E 2.6
  eCImageEffect::Execute(scene effect, default "DoF")  -- jobs pulled from the Screen outputs, inputs first                        F 2.1
   job RenderScene = eCImageFilterScene::OnUpdateOutput
    scene targets (FP16 +0x38 with HDR, refraction copy +0x40, R32F depth map +0x48)                                             A 2.7
    eCCameraBase::Update: frame++, UpdateFrameDependencies, ExtractFromFrustum (cull), PreExecute, Request                       B 1
     PreExecute = the OFF-SCREEN VIEWS (pre-pass list +0x14d0, one dynamic-light op per db frame):                               A 2.9, E 1
      terrain variance map (G32R32F, Terrain + Building casters, blurred) only when camera / sun moved                           E 2.1
      sun cascades eCCascadeShadowMap::RenderShadowMaps (2-3 x DF24, Building + Object casters, own shadow dbs:
        extract -> request (z ops only) -> ExecuteShadowMap: z list front to back, special pass 3)                               E 2.2-2.3, B 2.7
      point lights: one packed 1024² atlas, 6 cells per light, Object casters                                                     E 2.4
    eCFrustumDatabase::Execute (main view)                                                                                       B 2.6
     camera to device (viewport, c0..c11, PS c0 / c1), fog (PS c3 / c4, VS c30)                                                  A 2.3, 2.10
     ClearViewport(colour, 0xff000000) on the scene target
     Z PRE-PASS: depth map bound, cleared white (far), z = 1; BeginZPass; z list (+0x1518) sorted front to back
       (24-bit view-depth key); per op RenderZPass (special pass 0 = XFX_ZPASS: writes view z, masked ops clip
       alpha x globalAlpha - maskRef/256; a mesh's consecutive non-masked sub-meshes merge into one draw); EndZPass             B 2.6, C 1.2
     UpdateShadowMask: post_SSM.fx graph -> mask (.a sun, .r SSAO, .g / .b point lights)                                         E 2.5
     BeginMaterial; DoBeginRenderSolid
     SOLID list (+0x1500) sorted by (RenderPriority << 20 | AdditionalSortID << 12 | material id) then depth;
       opaque ops at depth EQUAL against the pre-pass ([C, run]: 265 of 504 Solid draws EQUAL, 235 LESSEQUAL);
       blended terrain layers are solid ops (request bit 0x200 [?]); instanced batches (<= 50, stream 3)                         B 2.5, C 1.3-1.4
     debug list; modules OnEndRenderSolid; decals (entities seen this pass); opaque billboards
     list +0x1530: the SKY DOME, last ([C, run]: a 1534-triangle strip, z write off, LESSEQUAL, the Solid's last draw)            B 2.6, E 2.7
     refraction copy (CopyCurrentRTToImage scene -> +0x40)
     DoBeginRenderAlpha; REFRACTED list (+0x1578, unsorted: water, unblended, z write on) BEFORE the alpha list                 B 2.6, F 3
     ALPHA list (+0x14e8) back to front (RenderPriority >= 1000 first): blended / fading meshes, characters,
       particles, strips, light streaks, blended billboards                                                                      B 4, D 5-8
     DoEndRenderAlpha; list +0x1548 (precipitation); EndMaterial                                                                 D 10
   post jobs of DoF (19): log-luminance 64² -> 32..2 -> adaptive 1² (persistent) -> copy; bright pass 309x210 (sky alpha);
     glow H; sun streaks; glow V blended onto last frame's bloom (persistent); tone map; 5 DoF stages at 1/4; DoF -> back buffer F 2.3
  overlay effect (if any): 1 draw (OverlayMovieStrips at the refcompare spots: section 15)                                     F 2.6
 OnPostRender -> Draw: GUI (one eCSprite batcher: GUI, text, cursor, videos), snapshot, SwapScreen = present                     F 4
 eCTimer::Update
```

Recorded frame 2710 (savegame, `-LogFrame`): `SkyCube{} Post{Main{DepthPrepass(355) ShadowMask(9) Solid(504) Refracted(3)
Alpha(39)} Post(19)} Gui{Gui(7)}`, no cascades in that frame (`debug.shadow_stride`), kinds StaticMesh 271, SpeedTree 156,
SpeedTreeBillboard 128, Vegetation 105, InstancedMesh 97, Skinned 96.

## 2. Corrections to earlier docs

| claim | was | is |
|---|---|---|
| place of the sky (`inject.h` BeforeSolid comment) | first in Solid | last in Solid, list +0x1530, after decals / billboards [C, run] |
| who renders the cascades (frame-phases.md 7) | a job of the effect [I] | PreExecute of the main database inside `eCImageFilterScene::OnUpdateOutput` [C] |
| PS c1 (native-renderer.md 10) | (0.5, -0.5, 1/vpW + 0.5, 1/vpH + 0.5) | (0.5, -0.5, 0.5 + 0.5/vpW, 0.5 + 0.5/vpH) [C] |
| fog source (native-renderer.md 10 open) | from the engine's last draw | `eCWeatherAdmin::Process` -> camera `+0x15c0` -> `eCGfxFog::Execute` [C] |
| alpha test of masked materials | D3D alpha test | never the hardware test: the z pass clips (ref / 256), solid draws at depth EQUAL [C, run] |
| static mesh cull | per material | always CW; DoubleSided only flips the normal in the PS [C] |
| point-light shadows | cube maps | one packed atlas (`eCPackedShadowMap`); the cube classes are unused [C] |
| cascade split class (E 2.2) | `eCShadowMapDirectionalLight2`, vtable 0x9768d0 | `eCShadowMapDirectionalLight2Ex`, vtable 0x9a29e0 [checked, section 12] |
| N3.3 (native-renderer.md 14) | image equals the engine's | not at `pub_interior` / `monastery_hall` (2026-10-27, water-compare: luma +19.9 / +10.2, holes in the pub): native-renderer.md 14 |

## 3. What a faithful copy needs (summary of the slices' section 5 / 6)

* **Decisions stay the engine's at first**: extraction, culling, LOD, list assignment and sort keys are complex (B 4: fade exactly 1.0
  per frame, LOD picked per camera, instancing batch rules, portal rooms) and already correct in the engine. The copy reads the
  engine's sorted lists at `Execute` (render op: context, material, desc, sort key; B 3) instead of rebuilding them.
* **Shaders stay the engine's at first**: the material permutation of each op (C 4: pass kind, point-light count, lightmap, sun,
  special pass, instanced / billboard variant) selects an already-translated HAL shader (`eCShaderVersionDefault_3_0 +0x1a8 / +0x208 /
  +0x28 / +0xe8`, our `d3d9::Shader`s); the GLSL ubershader with the engine's macro vector is the second step (C 4.4).
* **Constants and states are composed natively** from engine data (C 3, D 0.5, E 3, F 2.5): camera, fog, lighting pool c14-c37,
  material graph registers, uv transforms, wind, bones; render states per blend mode (C 2.2), samplers (C 2.3).
* **CPU-generated geometry** (particles, strips, decals, billboards, precipitation, vegetation ring, SpeedTree software instances)
  is read from the engine's dynamic buffers (they are ours in Vulkan mode) until a native generator exists (D 13).
* **Post chain** is data (F 2.2): 13 graphs, the default DoF = 20 jobs; persistent luminance / bloom targets (F 6).

## 4. Native frame plan (N5): copy first, then optimize

Each stage behind a `native.*` setting (default off), A/B against the engine path at the same spots, verified pixel-equal (refcompare
/ water-compare luma |d| < 0.5, no holes), validation clean, committed (CLAUDE.md rule 3).

| stage | delivers | replaces | verification |
|---|---|---|---|
| **N5.0 op tap** | read the main view's sorted lists at `Execute` every frame into a `native::FrameDesc` (views, passes, ops: context kind, entity, material, desc, sort key, list) | nothing (observer) | per pass and kind equal to the HAL draw stream (`-LogFrame` counts; ops -> draws incl. z-pass merges, instancing, SpeedTree batches) |
| **N5.1 static ops** | z pre-pass + solid ops of static meshes (incl. terrain layers, masked, instanced) executed by our recorder with the engine's own translated shaders, constants / states composed natively; the engine's `RenderZPass` / `Render` of those ops skipped | the HAL path of those ops | pub_interior / monastery_hall / jungle / savegame equal to the engine |
| N5.2 alpha / refracted / fading statics, decals, billboards, sky dome (+ sky cube faces) | the rest of the main view's mesh-like ops | | as above + water spots (done: sections 8, 9) |
| N5.3 skinned actors | palette + morphs from the actor (D 1), body parts | | hero_idle / pub_idle (done: section 10) |
| N5.4 SpeedTree, billboards, vegetation | global wind, batches, the 1000-instance grass ring | | jungle / forest spots (done: section 11) |
| N5.5 shadows | the shadow views' z ops: cascades, terrain VSM, point-atlas cells (done: section 12); N5.5c the `post_SSM` mask chain (SSAO, cascades, VSSM, blur; the PSSM point-light pass stays the engine's), its depth copy, the terrain map's Blur7H / V, the shadow views' setup (fit + camera compared, target / viewport / clear / reset taken over) (done: section 13) | the engine's shadow-view and mask draws | compare + same-frame switch (done) |
| N5.6 particles, strips, streaks, precipitation | the particle quads (UpdateVertices re-done natively), the strips' vertex fill, the rain's billboard records from the drops; light streaks never met (done: section 14) | the engine's particle / strip / rain draws | compare + same-frame switch at fire / rain / spell spots (done) |
| N5.7 post + GUI | every effect filter job of the graphs that run (DoF 19 draws, HDRI 14, the OverlayMovieStrips overlay), composed with the semantic parameters natively; the sprite batcher's batches + vertex / index data (done: section 15) | the engine's post and GUI draws | compare + same-frame switch at spots, savegame, menus, loading, native AA / quality (done) |
| N5.8 leftovers + completeness proof | a coverage counter for every device draw (who issued it, the reason it stayed), the PSSM point-light pass and the point-light atlas's setup, the never-met kinds forced by a scenario (lit / instanced emitters, alpha billboards, strips, light streaks), the unreachable ones named (done: sections 16, 17) | the last engine draws of the main frame | coverage 99.96 - 99.99 % ours in every run, compare + same-frame switches, validation (done) |
| render modes (2026-10-28) | the finished copy as the player mode Full of `renderer.mode` (Original / Wrapper / Full, section 17; overlay.md 2.7) | | unit tests, Original / Wrapper / Full runs, a validated Wrapper -> Full -> Wrapper switch |
| then | own modes, one at a time: mirror-driven lists (the current N3.3 code), GPU culling, merged indirect draws, the ubershader, bindless materials, RT consumers from the native frame | | A/B against the copy |

## 5. Open across slices

Request bit 0x200 (blended terrain layers in the solid list), the shader clock (game vs real time), masked materials when the z pre-pass
is off, the PSSM channel mapping (dynamic CastShadows lights: green, section 13; static lights met in N5.8: `Default_CrystalLight_Blue` / `_Red`, 10 cells per mask composed and equal, their colour flags from ShadowMaskIndex, section 16; the point-light atlas met, its setup composed), actor LOD (`ActorInstance +0x250`) writer, SpeedTree instancing mode (`admin +0x320`: hardware in every run, section 11), which overlay
effect makes the 1-draw `Post{Post(1)}` (answered: OverlayMovieStrips, section 15; who starts it is open), module `Process` order (weather before illumination?); after N5.8 also: the "after a dropped draw" issue model for permutations the engine compiles inside the draw (section 16), OverlayHit's trigger, the debug list; the slices' own section 6 / 7 lists.

## 6. N5.0 result: the op tap (2026-10-27)

Setting `native.frame_tap` (Debug, default off). `src/native/frame_desc.*` = `native::FrameDesc` + `native::Attribution` (pure, unit
tests `tests/native_tests.cpp`); `src/native/frame_tap.*` = the engine side. Off: no hook work beyond the phase markers (nothing is
patched; the existing draw-tag thunks only carry the new `draws::Tag::op`).

**How the order is reproduced.** At the entry of every main-view `eCFrustumDatabase::Execute` the seven lists are copied (data `+8`,
count `+0x10` of each `bTValArray`) and the z / solid / alpha copies are sorted by **the engine's own qsort with the engine's own
comparators**. Engine.dll imports no CRT (MSVC's CRT is linked statically), so Execute's `qsort` is Engine.dll's `0x4a4aa0`, not ours.
The tap does not assume addresses: at its first use it decodes Execute's three sort sites (`mov rcx,[rdi+list+8]; lea r9,[rip+cmp];
mov rdx,rax; mov r8d,0x18; call qsort` at Execute +0x2c1 / +0x42e / +0x976) and checks the targets (qsort `0x4a4aa0` with its prologue
`push r12/r13/r14; sub rsp,0x450`, comparators `0x66e670` / `0x66e6a0` / `0x66e600` by their first 10 bytes); any difference leaves the
tap idle. Same function, same comparator, byte-identical input, and MSVC's qsort is deterministic: the copy's permutation is the
engine's, ties included. This is **proved per op at run time**: Execute's loops pass `&data[i]` (disassembled: `rdx = data + 0x18 i`), so
every slot entry names its list position; the tap compares the live 0x18 bytes there with element i of the sorted copy and checks that
the entries come in the snapshot's sequence (z, solid, debug, +0x1530, refracted, alpha, +0x1548).

**How draws are attributed.** `draws::Tag::op` = the `eCRenderOp2*` whose slot is executing: the draw-tag thunks set it, render
functions nested in a slot inherit it (particles, sky), and slots without a thunk get a pass-through one when the tap first meets an op
of theirs (`engine::scene::ensureOpSlot`: particle and strip RenderAlpha, the animation context's empty RenderRefracted, ...; patched on
the render thread before Execute's loops run). `d3d9::DrawTap` shows every draw to the tap at the start of `prepareDraw`. Context kinds
come from the vtable: exported ones by decorated name, the six unexported ones (particle `0x9b7180`, strip `0x9e2448`, light streaks
`0x9abd10`, billboard `0x982810`, precipitation `0x9e7a18`, sky `0x9ddd90`) by RVA, verified by their `RequestRenderOps` slot (+0x40).
rvk.log: `native: frame tap:` every 5 s (last view + window, per list / kind buckets, first mismatches), `-LogFrame N` one line per op.

**Result** (validation on, 0 messages; savegame Risen_Save_0009 `rvk_perfsave` hold 30 s, and `rvkcompare` pub_interior + jungle; every
5-s window `clean` in both final runs):

| | savegame (2,511 main views) | pub_interior + jungle (3,239) |
|---|---|---|
| ops snapshotted / entered by the engine | 1,553,555 / 1,553,555 | 1,851,806 / 1,851,806 |
| out of order, bytes differ from the sorted copy, never entered, entered twice, list count changed during Execute | 0 | 0 |
| main-view draws / attributed to an op | 2,241,773 / 2,185,753 | 3,543,419 / 3,513,325 |
| draws without an op | 56,020 = ShadowMask 22,599 + Decal 31,192 + Billboard 2,229 | 30,094 = ShadowMask 29,151 + Decal 943 |
| a draw of another op than the executing one; pass / entity / material / sub-mesh mismatches | 0 | 0 |

Frame 2700 of the savegame (`-LogFrame 2700`): 654 ops (ZPass 295, Solid 304, Sky 1, Refracted 3, Alpha 49, Late 2), 917 main-view
draws (DepthPrepass 355, ShadowMask 9, Solid 504, Refracted 3, Alpha 46), as the recorded engine frame 2710
(`frame_logframe_save0009.log`: 355 / 9 / 504 / 3 / 39; the alpha count follows the particles of the moment).

**Draws without an op** are the documented non-op paths of Execute (B 2.6): the `post_SSM` shadow-mask graph (`UpdateShadowMask`, 9 per
view), `eCDecalAdmin::RenderDecals` and the opaque billboards' queue `eCBillboardAdmin::RenderBillboards`.

**Ops with 0 or several draws, all explained** ([C] = rule read in the code; "checked" = the engine's condition read at run time):

| list / kind | draws per op | rule |
|---|---|---|
| ZPass / Mesh, desc 0xff | 1-2 | whole-mesh z op: consecutive non-masked sub-meshes merged into one draw, each masked sub-mesh alone (C 1.2) |
| Solid / Mesh, desc 0xffffff00 \| i | 1-12 | the instanced op: every instance of the resource in batches of <= 50, the non-instanced fallback per instance (C 1.4) |
| ZPass, Solid / SpeedTree | 0-43 | one op per resource x geometry type: batches of all its trees and leaf LODs (D 2.2); **0** when every queued tree is at a LOD without that type's geometry (RenderFronds `0x6f1b0f`: a batch with strip length 0 is not drawn) [C, not checked] |
| ZPass, Solid / SpeedTreeBB | 2 | **new**: every tree-billboard op draws twice with two vertex shaders: z = `eCMeshResource2::RenderZPass` twice (false, true), solid = TransformationType 11 duplicates each pass (VS variant 1 / 2) [checked: the second draw's VS differs]; 0 while the billboard mesh is not cached (first world frames) [checked] |
| Solid, ZPass / Vegetation | 1-7 | one op per grass type: an instanced draw per run of visible grid nodes with the same illumination / light flags, and per 1000 instances (D 4.2) [checked: all instanced] |
| Solid / Animation | 2 | **new**: the entity's overlay flags (`+0x28 & 0xe00000`: burned / frozen / highlighted) add the overlay materials' passes (`OnGetPasses`, C 1.3); seen in the pub: the second draw alpha-blended with one PS shared by all of the actor's materials [checked: flags set] |
| Alpha / Mesh | 2 | `EnableAlphaZWrite`: the z-fill pass (no colour writes) before the colour pass (GetPasses rule 1, C 1.3), on the fading waterfall [checked] |
| Sky / Skydome | 4 | the sky dome op (dome, clouds, stars / sun / moon; partly additive) |
| Late / Skydome | 0 | the sky's alpha op: RenderAlpha `0x6e8a00` is `ret 0` [checked: empty slot] |
| Alpha / Strip | 0 | fewer than 2 segments (RenderAlpha `0x701120`: `cmp [PS+0xe8],1; jle`), or no visual / material shader yet (RenderVertices `0x6fd800`) [checked] |
| Late / Precipitation | 0-1 | `PS+0x18 & 1` clear (`0x719b00`) or no drop alive (`GetNumParticlesAlive` `0x71d1e0` = `PS+0xe0 - PS+0xc8`) [checked] |
| Alpha / Particle | 0-1 | 0 without live particles (`PS+0x390`) [checked], or with live particles but no quad built: `UpdateVertices` returns `primitives != 0` (`0x5d6e13`) [C] |
| ZPass, Solid / Mesh | 0 | the op's resource, visual, material or shader not ready (`eCResource2 +0x38 == 3` for resource and material, visual `res+0x88`, shader `mat+0x70`), e.g. an entity just cached in [checked] |

Engine rules newly learned: tree-billboard ops always issue two draws per pass (z and solid); entity overlay flags add draws to skinned
ops; ops whose mesh or material is still streaming stay in the lists and draw nothing; emitters with live particles can build no quad;
the rain mesh is drawn only while drops are alive (draw-tags.md 4.1's "every frame even when dry" was probably a weather state with drops alive [I]). Additive
light passes of meshes (pass kind 1) did not occur at these spots (no op had an additive draw except the sky's).

Read-only check: the images of a tap-off and a tap-on `rvkcompare` run (pub_interior, jungle) differ by mean |d| 1.12 / 3.21 (8-bit),
two tap-off runs by 1.14 / 3.04: the spots' run-to-run noise (flames, wind); the tap only reads engine memory and adds pass-through
thunks.

For N5.1 the tap is the source of truth: the executed sequence of the main view's ops is known before Execute's loops run, with each
op's context, kind, entity, mesh sub-mesh / material / shader, desc, key and sort id, and the draw-count rules above.

## 7. N5.1 result: static ops (2026-10-27)

Settings `native.static_ops` (the renderer draws the ops) and `native.static_ops_compare` (every engine draw of those ops is diffed
against the composed draw of its position), both Debug | NoAB, default off; off = nothing beyond the N5.0 tap's pass-through.
`src/native/static_compose.*` = the engine's rules re-written (pure, unit tests `tests/native_tests.cpp` `static_*`),
`src/native/static_ops.*` = the engine side, driven by the frame tap (`FrameTap` forwards view begin / end, slot entries and draws).

**Scope.** Ops of kind Mesh in the z list and the solid list of the main view: single sub-mesh and whole-mesh z ops (desc `i` / `0xff`,
hierarchy nodes `node << 8 | i`), solid sub-mesh / whole-mesh / node ops, blended terrain layers (solid ops with fade, request bit 0x200),
instanced ops (desc `0xffffff00 | i`: hardware batches and the per-instance fallback). Only `eCShaderDefault` materials whose version is
`eCShaderVersionDefault_3_0` (vtables and 14 + 5 virtual slots checked once against their RVAs; anything else is left to the engine and
counted). Entities with overlay flags (burned / frozen / highlighted) are left to the engine (their extra passes are not composed).

**When and how.** At the op's slot entry (the tap's thunk, before the engine's `RenderZPass` / `Render` code) the op is composed into its
complete draw list. Take-over issues those draws right there through `d3d9::Device` (shaders, declaration, streams, indices, the owned
states, textures + sampler states, the owned constant registers, the draw tag as the engine's hooks would set it) with the device state
saved before and restored after; then the engine's slot runs as usual and each of its draws is dropped at the start of `prepareDraw`
(`DrawTap::onDraw` returns true: nothing recorded, observed or counted as a draw). So **every side effect of the engine's code stays**
(material / image `RequestData` and LRU stamps, `BindLightmaps` and the admin's lightmap slots, `SetSwitch` on the pools, the pools'
dirty ranges, the `ms_pActiveShader` / lighting-pool caches, the record's stream-3 enable flag, the instance sort): the engine's state
machine evolves exactly as without the take-over, and because the device state is restored around our draws, its caches stay true.

**Composition (all from engine data, in the engine's order) [C]:**

| group | how |
|---|---|
| draw list | z: `RenderZPass` 0x7603b0 (whole mesh: consecutive non-masked sub-meshes merge and draw with the **last added** sub-mesh's shader; a masked one alone; blended ones skipped and end a run; not-cached materials passed over); solid: `eCSubMesh::Render` 0x5c7eb0 per sub-mesh, passes by `eCShaderVersionDefault_3_0::GetPasses` 0x6cc970 (z-fill kind 2 first for `EnableAlphaZWrite` blended materials; lights in passes of up to 3; sun only in the first, gated by list `+0` and illum `+0xc`); instanced: `ExecuteRenderInstances` 0x7607b0 (instances sorted by a copy sorted with the engine's own qsort 0x4a4aa0 + `SortRenderInstances` 0x75f690, call site verified in `eCRenderContextMesh::Render`; a batch breaks on light list, switch, overlay flags, f, 50 instances, and after an instance with static lights / SH; fallback per instance when not capable, < 2 instances or lightmapped) |
| shaders | VS / PS of the permutation: version `shader +0x258 + 8 v` (z: v = admin `+0x170`; solid: min(admin `+0x170`, shader `+0x68`), falling back to a lower existing one); z: special pass `type = admin +0x314`, idx `2 type`, compiled byte `+0x18 + idx`, elements `+0x30 / +0xf0 + 16 idx`; solid: `+0x1b0 + 16 vsIdx`, `+0x210 + 16 psIdx` (`vsIdx = nonInstanced 3 + variant`, `psIdx = lights + 4 (kind + 3 (combi + 2 sun))`); element `+0x18 +0x18` = our `HalHLSLShader` |
| states | `zPassStates` (BeginZPass: colour writes = the desc's depth image has an API texture) / `passStates` (FUN_1806cc610 + `FillBlendingRenderStates` 0x6d0430) through the HAL's master semantics: ZFUNC master; ZWRITE master or the visual's `+0x24`; blend / src / dst; COLORWRITE = colour 7 \| alpha 8 (**kind 0 / 1 write colour, not alpha**: `local_3c._1_2_ = 1`); alpha test always off; CULL = visual `+0x1c`; FILL = admin `+0x340` master or visual `+0x18`; FOGENABLE 0 |
| constants | a mirror of the engine's pools taken at the op's entry (admin VS c0..c30, admin PS c0..c5, `ms_pGlobalShaderConstants` c31..c231, the lighting pool c14..c37 (`DAT_180c62968`, address decoded from FUN_1806cd1d0), the shader's VS c232+ / PS c6+ pools), written as the op's path writes them: **natively** the camera c0..c3 (VP, or the depth-bias VP for `+0x78`), c4..c7 (transposed inverse projection), c8..c10.xyz, c11 and PS c0, PS c1 (viewport), fog PS c3 / c4 and VS c30 (db `+0x14c8` or `ms_pMasterFog`), PS c5.xy (application camera `+0xe4`), c5.w (mask reference / 256 where the pass sets it), world c33+4i..c35+4i, fade c32+4i.w, the lighting block per pass (FUN_1806cd1d0 / FUN_1806b5bb0); **from the pools** the material graph registers (SetTime ran in the admin's Process before the frame) with the switch elements applied (constant switch 0x6c0c60), and the carry-over registers nobody writes per draw (c32+4i.xyz, c8..c10.w, PS c2, c14.w, lights beyond the pass's count) |
| samplers | material slots (`shader +0xf8 + 0x28 k` -> stage `+0x219 + k` when the PS element's mask `+0x38` has it; image `+0x20` (switch samplers 0x6c5310 applied) -> `+0x70` image data, the per-stage placeholder (table decoded from 0x68c650) while streaming, the ErrorImage `admin +0x348` for none; states U, V, MAG, MIN, MIP, BIAS, SRGB of the slot, `0xABABABAB` = gfx `+0x640`); admin s0 / s1 / s2 / s4 / s5 in the solid pass (s4 / s5 as `BindLightmaps` updates them); other sampled stages inherited (the z pass's s0..s8); the per-stage override table of `eCGfxImageData::Execute` (decoded from 0x7551c0) must be empty |
| geometry | the visual's record (`eCGfxVisual +0x10` = our `HalPrimitive`): declaration, streams (stream 3 off in the z pass, on for batches, else the record's flag), indices; ranges from the sub-mesh (`+0x28 / +0x2c / +0x30 / +0x34`, node ops through the `+0x40` entries); batches: streams 0..2 frequency `n \| 0x40000000` |
| tag | sub-mesh / material / mesh resource only for draws of `eCSubMesh::Render` (the engine's hook sets them there only: z draws and batches keep null) |

**Verification** (compare = the composed draw vs the engine's own device state at `prepareDraw`, every field above; constant registers
those live in the shaders' CTAB):

| run | draws compared | equal | registers / stages compared | ops left to the engine |
|---|---|---|---|---|
| compare only, pub_interior + monastery_hall + jungle | 1,343,887 | 1,343,887 | 84.3 M / 3.78 M | 234 of 1,089,550 |
| compare only, savegame Risen_Save_0009 (`rvk_perfsave` hold 30) | 1,152,012 | 1,152,012 | 48.0 M / 3.69 M | 256 of 1,050,884 |
| take-over + compare (the dropped engine draws vs ours), 3 spots | 1,300,511 | 1,300,511 | 81.8 M / 3.65 M | 234 |
| take-over + compare, 3 spots, validation on (0 messages) | 1,273,444 | 1,273,444 | 80.2 M / 3.57 M | 232 |
| take-over + compare, savegame (uncapped) | 1,700,948 | 1,700,948 | 71.2 M / 5.45 M | 253 of 1,550,665 |

In take-over runs our own draws are captured at their own `prepareDraw` as well and compared with their composition (what the recorder got
= what was composed): every one equal (1,300,511 and 1,273,444). Ops left to the engine: a permutation not compiled yet (the engine
compiles it on demand at that draw; the next frame composes it) and 3 "unreadable" (the admin / sub-mesh index checks). The first build
already compared at 0 mismatches (the rules above were read before the code was written); one difference outside the compared fields was
found by the scene mirror's cross-check (z merges tagged with a sub-mesh, which the engine's hooks never do) and fixed; the tag is now a
compared field.

Images (`water-compare.ps1 -Spots pub_interior,monastery_hall,jungle`, water off on both sides; mean |d| 8-bit / share of pixels with a
channel |d| > 32):

| spot | engine vs take-over | engine vs engine (noise floor) |
|---|---|---|
| pub_interior | 0.43 / 0.004 % | 0.49 / 0.003 % |
| monastery_hall | 0.75 / 0.147 % | 1.12 / 0.088 % |
| jungle | 0.79 / 0.032 % | 0.70 / 0.007 % |
| savegame, separate runs (`-MaxFps 60`, frames 2000 / 2600) | 3.77 / 2.10 %, 4.16 / 2.67 % | 2.08 / 1.28 %, 2.27 / 1.37 % |
| savegame, one run switched at frame 2000 (`-Commands '2000: set native.static_ops true'`; frames 1980 -> 2020) | 1.00 / 0.80 % (40 frames apart) | 0.75 / 0.49 % (1960 -> 1980, 20 frames) |

`shot-reviewer` (crops, amplified diff maps against the noise pair): no holes, missing / extra geometry, texture, layer, decal or lighting
difference at any spot; what differs is candle flames, glow, fog / particles and wind, as in the noise pair. At the savegame two separate
runs differ more than two engine runs (high-frequency detail on the cobblestones, NPC poses: the runs reach a frame number at different game
times and stream differently); switched inside one run, the static scene is pixel-equal across the switch (8x8 block means 0.003 - 0.05 on
every static block, e.g. the cobblestones 0.01 both off -> off and off -> on) and the remaining difference is the moving NPCs, the hero's idle
and the foliage, the same blocks as in the off / off and on / on pairs.

Cost (savegame, `-Exe RisenTest.exe -Vsync off -Perf`, 1,165 draws per frame, `perf:` 5-s windows): the GPU time is unchanged (1.42 - 1.48
ms per frame); the CPU (engine + HAL per frame) grows. Pair 1 (back to back): off 4.53 - 4.56 ms (p50 frame 5.0 ms) vs take-over +
compare 7.60 - 7.70 ms (p50 8.0 ms): +3.1 ms. Pair 2 (later, an unrelated ffmpeg process at ~80 % CPU on the machine: both runs slower): off 7.53 - 7.91 ms vs take-over alone
10.09 - 11.48 ms: +2.6 - 3.6 ms. The engine's code still runs completely for every op (only its GPU work is replaced), so the extra is
ours: the composition (the pool mirror read per op, ~4 KB; the register and sampler loops), the device state saved and restored per op and
a second set of device setter calls per draw. Not optimised yet (as planned: the take-over pays off when the engine's slot is skipped).

Engine rules newly learned [C]: the whole-mesh z merge draws with the last added sub-mesh's special-pass shader; `ExecuteSpecialPass` uses
admin `+0x170` without the shader's max version; kind 0 / 1 passes write colour but not alpha; the lighting pool's c14 / sun registers are
rewritten only when the light list changes (c14.w never), so composing the whole block every pass is equal; an instance batch takes the
illumination, switch and flags of its **last** instance; `eCSubMesh::Render` never touches the record's stream-3 flag (it keeps what the
last z op / batch of that visual set); bCGuid validity is its byte `+0x10` (HasOpacity = shader `+0x330`, IsRefracted = `+0x420`).

Not yet: the overlay passes; additive light passes (kind 1) follow the same rules but were not counted separately in these runs; other
material classes (`eCShaderLeafs`, decals, sky), alpha / refracted lists (N5.2). The engine's code still runs for every taken-over op
(only its GPU work is ours): the CPU cost is the engine's plus the composition; skipping the engine's slot needs its side effects
reproduced first (the "own modes" of section 4).

## 8. N5.2 result: alpha / refracted mesh ops, overlay passes, decals (2026-10-27)

Settings `native.scene_ops` (the renderer draws them) and `native.scene_ops_compare` (every engine draw of them is diffed against the
composed draw of its position), Debug | NoAB, default off; `native.scene_ops_groups` (Debug, default 7; 63 since N5.2c, section 9; 127 since N5.3, section 10) = bit 1 alpha / refracted mesh
ops, bit 2 entity overlay passes (also for the N5.1 static ops while scene ops are on), bit 4 decals. Off = nothing beyond the N5.0 tap's
pass-through; `native.static_ops` behaves as before (its overlay ops stay the engine's unless scene ops are on). Same machinery as N5.1
(`native::StaticOps`, one `GroupState` per group: take-over / compare switches, counts, skips), the decals in
`src/native/static_ops_nonop.cpp`.

**Scope.** Kind Mesh ops of the alpha list (+0x14e8) and the refracted list (+0x1578): every blend mode (C 2.2), the EnableAlphaZWrite
z-fill + colour pair, fading statics (fade < 1: alpha ops), node / whole-mesh ops; the entity overlay passes of any mesh op; the decals.
Left to the engine (counted as skips): a permutation not compiled yet (compiled on demand by the engine at that draw), shader classes
other than `eCShaderDefault` / `eCShaderVersionDefault_3_0`. Not done: the sky dome op, the opaque billboards' queue (see "Not yet").

**Alpha / refracted mesh ops [C].** `eCRenderContextMesh::Render / RenderAlpha / RenderRefracted` are all `jmp 0x5c7110` (F 3): the N5.1
solid composition applies unchanged; only the op's fade and the material's blend mode differ, and between the lists Execute's
`EndMaterial` / `CopyCurrentRTToImage` / `BeginMaterial` re-binds the admin samplers (s1 = the refraction copy): the composition reads the
admin slots and pools at each op's entry, so the re-bind is seen. Two engine-side details matter for a take-over:
* the slot thunk tells the op tap about an op **inside** the slot's pass marker (`engine/scene_hooks.cpp contextSlot`): the refracted
  ops' own draws must be in the Refracted pass (draw kind Refracted, the water replacer, phase-dependent observers);
* the draw tag of `eCSubMesh::Render` carries the material's water classification and name in the Refracted / Alpha passes
  (`TagScope`); the composition reproduces it (`engine::scene::subMeshWater`) and it is a compared field. With `water.enabled` the
  enhanced water's DrawReplacer therefore acts on the composed draw exactly as on the engine's (same kind, tag, state, constants).

**Overlay passes [C]** (`eCShaderBase::OnGetPasses` 0x6d3170, `ExecutePass` 0x6d2060, FUN_1806cc610; dumps frameC_dump2..4): after the
base shader's version GetPasses, for flags 1 (burned, shader +0x88), 2 (frozen, +0x80), 4 (highlighted, +0x90) in that order:
`RequestData(res, 1)` (no wait) and, when `res +0x70` (an `eCShaderDefault`) exists, that shader's own `OnGetPasses` (vtable +0x1c8) with
the base's switch, illumination, combi and instancing, flags 0 (overlays do not nest; its version = min(admin +0x170, its +0x68)). The
passes land in the shared `ms_arrShaderPasses` with the overlay's version object; `ExecutePass(i)` on the base shader: **SetSwitch and
the depth-bias c0 switch use the base shader**, the version's Execute uses its owner (the overlay shader): blend mode, global alpha
(the shared c32.w), `ExecuteShaderConstants` (its pools and samplers). Instanced batches carry flags too (they break on them). Composed
for static and scene ops (`native.scene_ops_groups` bit 2).

**Decals [C]** (`eCDecalAdmin::RenderDecals` 0x4fc220, frameD_dump1.txt 5582-6124; layouts in byte offsets): queue = admin `+0x20` (array of
decal pointers, count `+0x28`) filtered by: entity (proxy at decal `+0x10`: when its slot +8 is `eCEntityProxy::ResolveEntity`, the
entity is `*(decal +0x18)` +0), `entity +0xe4 == db +8`, vertex count `+0x30` != 0, index count `+0x48` != 0, entity `+0xf8` != 0; entries
`{decal, entity +0xf8 + 0x78}` (16 bytes) qsorted by the engine's qsort with `SortRenderQueue` 0x4fcf80 (call site checked). Lock cycles
(VB admin `+0x60` 360,000 bytes, IB `+0x68` 20,000, DISCARD at offset 0): a decal above 6,000 vertices or 10,000 indices is never
drawn; the cycle flushes (draws its records) when a decal would overflow it, and at the end. A new record (material `+0`, image `+8`,
lightmap mesh `+0x58`, illumination, first vertex, start index) starts when material, image, lightmap mesh, light list (`illum +0`) or
outdoor factor (`illum +8`) change, or after a record whose first decal has static lights (`illum +0xd`), static SH (`+0x90`) or a
lightmap mesh. Per decal: indices first, rebased by `(short)` the cycle's vertex count; vertices (stride 0x3c) from `+0x28`: dynamic
decals (`+0x54 == 1`) through SharedBase's `bCVector::Transform` (position) and `TransformDirection` (normal `+0xc`, tangent `+0x18`) with
the entity's `+0x60`, COLOR0 = `int(alpha +0x50 x 255) << 24 | 0xffffff`; static decals copied, COLOR0 replaced only when alpha < 1.
Per record: material shader (`+0x70`; null: skipped) or, without material, the admin's default shader `+0x48` with
`eCColorSrcSampler::SetImageResource(admin +0x50, image)` (the pool slot's image; no image: skipped); `SetGlobalAlpha(1, 0)`,
`SetWorldMatrix(identity, 0)`, `BindLightmaps(lightmap mesh, 1)` when there is one, `GetPasses(shader, 0, illum, combi, false, 0)`, per pass
`ExecutePass` + the visual's (`admin +0x58`) render mode / cull / depth / depth write and the primitive: indexed list, min vertex = first
vertex, vertices, start index, `indices / 3` primitives, base 0. No Begin / EndMaterial, no state block. Compared: everything above plus
the vertex / index bytes against the engine's dynamic buffers at its draw; take-over draws from our own dynamic VB / IB (the same bytes at
the same positions) and the engine's code still runs (its side effects: resource cache touches, BindLightmaps, SetImageResource, the
locks).

**Refraction copy (as data).** `FrameDesc::refraction`: the render-target desc's image `desc +0` and its API texture (`+0x60`) at Execute's
entry; the HAL's `CopyCurrentRTToImage` in the main view reports to the tap, which checks that exactly one copy goes into that texture,
after the last op up to the +0x1530 list and before the first refracted op (`refractionCopyInPlace`): every main view of every run below
(e.g. 400 of 400 per window, 0 bad). The gfx admin's `+0x5b8` gate was not read (the copy was never missing).

**Bool / int registers.** None of the 2,483 captured game shaders (corpus) has a bool or int constant in its CTAB, and no CTAB-live one
was met in any compared draw ("0 live in the CTABs"). The material path never calls the HAL's bool / int setters, so the composition of
the two register files is the device's at the op's entry; every draw compares all 16 + 16 per stage (64 per draw: 190 M registers in
the spot run below), all equal (also for the N5.1 ops).

**Verification** (compare only: `native.frame_tap` + `native.static_ops_compare` + `native.scene_ops_compare`, water off, validation off):

| run | static z / solid (N5.1) | alpha | refracted | z-fill (of alpha / refracted) | decals | left to the engine |
|---|---|---|---|---|---|---|
| 13 rvkcompare spots (pub_interior, monastery_hall, jungle, lake_jungle, waterfall_jungle, sea_cliff, harbour_mouth, harbour_night, harbour_docks, lava_cave, crystal_cave, swamp_camp, pub_idle; actors shown) | 2,976,196 / 2,976,196 | 189,034 / 189,034 | 67,163 / 67,163 | 5,182 / 5,182 | 65,316 / 65,316 | permutation not compiled 359 + 50 + 5, unreadable 6 |
| savegame Risen_Save_0009 (`rvk_perfsave` hold 30) | 774,953 / 774,953 | 52,990 / 52,990 | 6,723 / 6,723 | 304 / 304 | 2,724 / 2,724 | 162 + 22 + 1 |
| 11 spots, first build (no decals) | 2,771,343 / 2,771,343 | 179,533 / 179,533 | 63,044 / 63,044 | 5,164 / 5,164 | - | 366 + 53 |

(draws equal / compared; every field class of N5.1 plus bool / int registers, the water tag and the decals' vertex / index bytes;
0 mismatches in every class; the op tap's order / content / attribution checks 0 everywhere, but 3 of its 5-s windows per spot run are
NOT clean from unexplained 0-draw Vegetation / Animation ops while actors are shown (`show_actors=1`, outside N5.2's scope)). Registers / stages compared in the 13-spot run: 173 M + 7.0 M + 1.7 M float, 9.4 M + 1.3 M + 0.4 M stages.

**Additive light passes (kind 1).** Counted per draw class and by the lights of each composed sub-mesh draw (dynamic list `+0x34` +
static `illum +0xd`). At all 13 spots and the savegame: 0, 1 or 2 lights only (spots: 1,198,502 / 291,040 / 103,399 static-op draws,
173,252 / 41,791 / 36,022 alpha / refracted; 3 or 4 lights: 0). An additive pass needs 4 (passes of up to 3): the light sets give at most
2 dynamic point lights (E 3.2: `count +0x34 < 2`), and `illum +0xd` (the static per-pixel lights of `eCIlluminated_PS`) was 0 for every
composed draw, so no draw with 3 lights exists either. Kind-1 passes are composed by the same verified rules (pass flags, PS index,
states: blend One / One or the mode's, no sun / lightmap) but **were never met, so they are not verified in game**; `native: lights:` in
rvk.log names the first 4-light sub-meshes should a place have them.

**Overlay passes.** Composed, 0 met in these runs: in the pub and elsewhere the overlay flags occur on skinned actors (N5.0), which are
not mesh ops; a highlighted (focused) static item would be one. **Not verified in game.**

**Take-over** (`native.scene_ops`, with compare: the dropped engine draws vs ours; our own draws re-captured at their `prepareDraw` and
compared with their composition; static ops off; `water-compare.ps1`, validation on):

| run | alpha | refracted | z-fill | decals | own draws vs composition | validation |
|---|---|---|---|---|---|---|
| 7 spots, water off | 110,591 / 110,591 | 35,367 / 35,367 | 4,367 / 4,367 | 7,188 / 7,188 | 153,146 equal | 0 messages (both runs) |
| 7 spots, water on (enhanced water replaces the composed water draws: the same `enh: water:` models on both sides) | 103,258 / 103,258 | 33,516 / 33,516 | 4,493 / 4,493 | 7,624 / 7,624 | 144,398 equal | 0 messages |
| savegame, switched on at frame 2000 (`-MaxFps 60`) | 53,438 / 53,438 | 6,987 / 6,987 | 329 / 329 | 2,714 / 2,714 | 43,428 equal | not run |

Images (mean |d| 8-bit / share of pixels with a channel |d| > 32; engine vs take-over, engine vs engine = the noise floor):

| spot | water off: take-over | water off: noise | water on: take-over | water on: noise |
|---|---|---|---|---|
| pub_interior | 0.48 / 0.004 % | 0.56 / 0.004 % | 0.81 / 0.140 % | 0.97 / 0.005 % |
| monastery_hall | 0.97 / 0.164 % | 1.38 / 0.196 % | 0.83 / 0.070 % | 0.80 / 0.117 % |
| jungle | 2.28 / 0.789 % | 1.82 / 0.417 % | 2.27 / 0.828 % | 2.26 / 0.988 % |
| lake_jungle | 1.28 / 0.710 % | 1.49 / 0.871 % | 1.40 / 0.841 % | 1.68 / 1.170 % |
| waterfall_jungle | 1.49 / 0.562 % | 2.38 / 1.917 % | 2.95 / 2.613 % | 1.75 / 0.780 % |
| sea_cliff | 3.66 / 3.509 % | 7.41 / 6.631 % | 6.76 / 7.542 % | 5.00 / 5.028 % |
| harbour_mouth | 0.26 / 0.008 % | 0.29 / 0.003 % | 0.33 / 0.008 % | 0.37 / 0.010 % |

`shot-reviewer` (diff maps, crops against the noise pair; out/n52/review_a, review_b): no hole, missing / extra transparency, wrong
blending, unreplaced or missing water, decal or sky difference at any spot. What differs is the noise pair's: candle flames and their
light (pub), the haze shafts (monastery), a flickering light on the jungle ground, wind on the fronds, the surf / foam at sea_cliff
(the shore foam and wash move between runs: up to 2.3x brighter patches in either pair). A second take-over sample (water on, sea_cliff + waterfall_jungle) against the same engine
pictures: sea_cliff 5.45 / 5.23 % (the bright wash patch of the first sample is gone: box 830-960 x 700-780 mean RGB (47, 67, 64), as in
the noise run's (47, 66, 63), the first sample's (139, 193, 194)); waterfall_jungle 3.11 / 2.88 %. At waterfall_jungle with water on the two
take-over samples agree with each other (1.41) but both differ from both engine runs in one place: the blurred foreground palm frond
(x 0 - 400, y 100 - 450) is in another sway phase (shifted, brighter). That frond is a SpeedTree (its draws are the engine's, not scene
ops), so this is a different wind phase at the capture frame, not a composed draw: every taken-over draw compared equal [I: the wind
clock advances with real time and the take-over's CPU cost shifts the time of the capture frame; not checked]. Savegame switched inside one run (frames
1960 / 1980 off, 2020 / 2040 on): 3.35 / 0.46 % (off -> off, 20 frames), 4.00 / 0.68 % (across the switch, 40 frames), 3.89 / 0.70 % (on ->
on, 20 frames): the view is mostly in motion (hero idle, NPCs, foliage); the 142 8x8 blocks static in both same-mode pairs stay static
across the switch (mean 0.061).

Cost (savegame, `-Exe RisenTest.exe -Vsync off -Perf`, ~915 draws per frame, the last six 5-s `perf:` windows of each run; an unrelated
ffmpeg used ~10 cores of the machine during both runs, so absolute numbers are inflated): GPU 1.50 - 1.51 ms per frame in both; CPU
(engine + HAL) off 5.53 - 6.01 ms (p50 frame 5.85 - 6.40), scene ops on 5.89 - 6.41 ms (p50 6.29 - 6.91): about +0.3 - 0.4 ms for ~60 alpha /
refracted / decal ops per frame (the engine's code still runs for every op; ours is the composition, the state save / restore and the
second set of setter calls, as in N5.1).

Engine rules newly learned [C]: overlay passes = the overlay shader's own OnGetPasses with the base's switch / illumination / combi /
instancing and flags 0, executed with the base shader's SetSwitch and depth-bias flag and the overlay's states / constants / samplers;
`eCSubMesh::Render`'s tag holds the water class + name in the Refracted / Alpha passes; the refraction copy happens exactly once per
main view, between the +0x1530 list and the refracted list; decal records break on light list and outdoor factor although the sort key
ignores them, and a decal with static lights / SH / a lightmap mesh closes its record; static decals at alpha 1 keep their stored
COLOR0; no captured game shader has bool / int constants; no composed draw had more than 2 lights (no additive light pass exists at the
sampled places).

**Not yet** (RE done, recorded for the next stage):
* **Sky dome op** (list +0x1530, 4 draws; RE 2026-10-27, dumps skyrender*_dump.txt [C unless marked]): `eCSkydome_PS::Render` 0x6e4730
  (op slot 0x6e8a10 is a 4-instruction tail jump: never detour it). It rewrites the active camera (view translation zeroed; projection
  `_33 = 1`, `_43 = 0`: depth exactly 1) and runs `eCGfxCamera::Execute` (c0..c11, PS c0 / c1 from the hacked camera); draws stars (sky
  object +0x278, shader +0x148, BlendMode 7 SrcAlpha / One), sun (+0x170, shader +0x150, mode 7; its 1-billboard VB is rewritten every
  frame by a non-DISCARD lock), moon (+0x220, shader +0x148 with BlendMode 5 One / InvSrcColor) through `eCSkyObject::Render` 0x6e25e0
  (eCShaderDefault, TransformationType 7 = XFX_BILLBOARD, DisableLighting / DisableFog; world from +0x2d0 / +0x310 / +0x350; billboard
  quads stride 0x20); restores the camera (second `Execute`), then the dome: `eCShaderSkyDome` (vtable 0x9da1e8; version class
  `eCShaderVersionSkyDome_2_0` 0x9da420, one pass, states 0x6d9740: AlphaModulate One / InvSrcAlpha, alpha write off, LESSEQUAL, depth
  write off) with VS c31 sun normal, c32 sun colour, c33 sky colour, c34 haze colour (corrects E 2.7), PS c6 cloud rgb + thickness,
  c7 sun colour, samplers s9.. cloud / absorption images; the 32 x 25 screen-grid strip (1534 triangles). Composing it needs the camera
  hack's matrices, the sky version class's element offsets and the sun's per-frame vertex data: done in N5.2c (section 9, which also
  corrects this register map: the registers are pool registers written by the shader's property notify).
* **Opaque billboards' queue** (`eCBillboardAdmin::RenderBillboards` 0x4f0530; ~1 draw per view at the savegame, none at the spots): queue
  entries 0x18 {billboard, light set, blend, lights}, the engine's qsort with `SortRenderQueue` 0x4f0ff0, batches of <= 1000 quads, the
  default shader `admin +0x30` with BlendMode written directly (+0x50) and `SetImageResource(admin +0x38)`, an illumination built on the
  stack `{light set +0x10, f = 1, ...}`, `GetPasses(shader, lights, &illum, 0, false, 0)`; the decompiled VB / IB write addresses are lost
  (re-read the disassembly before composing it).
* Overlay passes and additive light passes: composed, never met in game (above).
* As in N5.1 the engine's code still runs for every taken-over op / path (only its GPU work is ours).

## 9. N5.2c result: the sky dome op, the opaque billboards' queue, the sky cube faces (2026-10-27)

Groups of `native.scene_ops_groups` (behind `native.scene_ops` / `native.scene_ops_compare` as in section 8): **8** the sky dome op
(list +0x1530), **16** the opaque billboards' queue (`eCBillboardAdmin::RenderBillboards`), **32** the sky environment cube's
faces (`eCSkydome_PS::Render` inside `UpdateSkydomeEnvCubemap`). Code: `src/native/static_ops_sky.cpp` (sky + cube faces),
`static_ops_nonop.cpp` (billboards, beside the decals), the pure rules in `static_compose.*` (`skyBillboardVertices`,
`skyQuadIndices`, `skyDomeStates`, `planBillboards`, `billboardQuadIndices`; tests `sky_*`, `billboard_*`). Dumps:
`B:\DevDrive\risen-ghidra\n52c_dump1.txt` / `n52c_dump2.txt` (clone EngineD) + capstone disassembly of the test copy's Engine.dll.
All [C] unless marked; "checked" = held in the compare runs below.

**Sky dome op [C, checked]** (`eCSkydome_PS::Render` 0x6e4730, disassembled; the context's slot 0x6e8a10 passes context +0x18 = the
property set; nothing when it is null or `PS +0x18 & 1` is clear):
* Camera: `eCGfxCamera::ms_pActiveCamera` (main view; the cube camera `+0x3c0` when `+0x3cc`). The hack copies the view (+0x20),
  zeroes `_41 _42 _43`, writes it back, `+0x120 = view; bCMatrix::Invert(+0x120)`; `+0xe0 = (+0x60) * (+0x20)` (SharedBase
  `bCMatrix::operator*`, the projection on the left); a projection copy with `_33 = 1`, `_43 = 0`, `+0xe0` recomputed, dirty
  `+0x160`, `eCGfxCamera::Execute` (full path: SetCamera c0..c3 = +0xe0, or `+0xa0 *= +0x20` for depth-bias materials, c4..c7 =
  `bCMatrix::GetInverted(+0x60)`, c8..c10 = +0x120, c11 / PS c0 = position + the app camera's far, PS c1 viewport, PS c5.xy
  obstacle). The restore after the moon does the same with the saved view / projection (Invert and operator* again, not a copy),
  then Execute. The composition calls the same four SharedBase functions (`Invert`, `operator*`, `operator*=`, `GetInverted`) on
  copies: bit-equal. The later ops of the view keep the pre-sky camera constants (the restore reproduces them bit for bit: the
  N5.2 alpha / refracted compares agree).
* Stars (`CreateStars` 0x6e3ee0 true: texture +0x140 and the object's count +0x290 == StarCount +0x28; else rebuilt inside this
  Render: that frame is left to the engine): shader +0x148 `BlendMode` = 7 through the property notify, `SetImageResource(+0x160,
  +0x140)` (the sampler's pool slot, `UpdateSampler` 0x6c3ab0), `eCColorSrcConstant::SetColor(+0x168, +0x390)` -> its pool register
  (element +0x68 pool, +0x70 register: r, g, b of the colour and a = element +0x60; `OnNotifyPropertyValueChangedExitEx` 0x6bfda0),
  then `eCSkyObject::Render(+0x2d0)` of the object at +0x278. Sun (texture +0x138): `SetImageResource(+0x158, +0x138)` on shader
  +0x150 (its BlendMode untouched), `SetBillboardRenderCount(1)`, its one billboard rewritten every frame = position (0, 0, -1 =
  DAT_1809f1854), s = (size, 0), r = (0, size), colour 0xffffffff, uv (0, 0, 1, 1), size = SunSize +0x3a8 x +0x50; dirty +0x49 ->
  FillVertexBuffer locks the (managed) VB at 0 with flags 0 every frame; object +0x170, matrix +0x310. Moon (texture +0x130;
  `CreateMoon` builds the billboard (0, 0, -1), s = (+0x78, 0), r = (0, +0x78) once): shader +0x148 again with `BlendMode` 5, image
  +0x130, colour +0x58, object +0x220, matrix +0x350. **Stars and moon share one eCShaderDefault whose blend mode, image and colour
  change between the two draws every frame** (at the op's entry it holds the moon's).
* `eCSkyObject`: billboards `bTValArray` data +0x10 / count +0x18 (`eSSkyObjectBillboard` 0x30: position, s @0xc, r @0x14, colour
  @0x1c, uv rect @0x20), shader +0x20, vertex data +0x28, index data +0x30, declaration +0x38, visual +0x40, dynamic +0x48, dirty
  +0x49, depth write +0x4c, depth func +0x50. `Render(world)` 0x6e25e0: shader and `FillVertexBuffer` 0x6e1a50 (vertex data; the
  record's primitives / 2 billboards; when dirty: lock 0, n x 0x80 bytes, DISCARD only when dynamic) -> `SetWorldMatrix(world, 0)`,
  `SetGlobalAlpha(1, 0)`, `GetPasses(shader, 0, null, 0, false, 0)`, ExecutePass + `eCGfxVisual::Execute`. Vertex (stride 0x20; decl
  POSITION f3, COLOR @0xc, TEXCOORD0 f2 @0x10, TEXCOORD1 f2 @0x18 = the corner): corners `-s + r`, `s + r`, `s - r`, `-s - r` with
  uv (u0 v0), (u1 v0), (u1 v1), (u0 v1); indices `4k + (0, 1, 2, 0, 2, 3)` (FillIndexBuffers 0x6e1940); visual cull 1 (none).
* Dome: `eCShaderSkyDome` (vtable 0x9da1e8: eCShaderBase's ExecutePass / OnGetPasses / ExecuteShaderConstants / SetSwitch,
  `GetGlobalShaderConstants` null, `HasOpacity` / `IsRefracted` false, `GetGlobalAlpha` 1, `SetGlobalAlpha` a no-op, `SetWorldMatrix`
  -> its own VS pool register 0x23) with **one** version object `eCShaderVersionSkyDome_2_0` (vtable 0x9da420, at +0x268 = version
  2; OnGetPasses' min(admin +0x170, +0x68) falls back to it): GetPasses 0x6d91a0 = one pass {version, flags 0, illumination, switch};
  Execute 0x6d90d0: VS element `version +0x1b0`, PS `+0x1d0` (+0x1c0 / +0x1e0 with SunOcclusion `shader +0x324`), states 0x6d9740 =
  FillBlendingRenderStates(not additive, fade 1), colour on / alpha off, LESSEQUAL, depth write off when blended (the dome's
  BlendMode 4: One / InvSrcAlpha), render mode 0; then `ExecuteShaderConstants` with **eCShaderBase's register map**: admin VS
  c0..c30, the shader's VS pool from `+0x9c` (c31 = (+0x318 .. +0x320, 1), c32 = (+0x2d0 .. +0x2d8, 1), c33 = (+0x2b8 .. +0x2c0, 1),
  c34 = (+0x2e8 .. +0x2f0, 1), c35.. world; which field is sun normal / sky / haze / sun colour stays as E 2.7 inferred it), admin PS
  c0..c5, the shader's PS pool from `+0xcc` (c6 = (+0x300, +0x304, +0x308, thickness +0x310), c7 = the same vector as VS c32); no
  global pool, no lighting pool. **Correction to E 2.7 / section 8**: those pool registers are written by
  `eCShaderSkyDome::OnNotifyPropertyValueChangedExitEx` 0x6d7840 (property changes: the weather's), not by Render: Render writes the
  fields +0x300..+0x310 directly without a notify, so PS c6 holds the values of the last notify; the composition takes the pools at
  the op's entry. Geometry: the screen grid (`CreateBuffers` 0x6e5f90: 32 x 25 vertices, x, y = `2 i / (n - 1) - 1`, z = 1, float3; a
  zig-zag triangle strip of `rows x cols x 2 - 2` = 1534 triangles), visual cull 1, depth write off.
* Render's other work (kept: the engine's code still runs in a take-over): the sun VB rewrite; the clouds' `Scale` (1 / +0xb8,
  1 / +0xbc on elements +0x110 / +0x118) and the scroller's `SetDirection(+0x108, +0xb0)` through property notifies (the scroller
  writes its pool register only in `SetTime` 0x6e0c00, the admin's Process); none of them changed a composed register in the runs.

**Opaque billboards' queue [C, checked]** (`eCBillboardAdmin::RenderBillboards` 0x4f0530, read in the disassembly; the decompile
lost the buffer writes): `CreateBuffers` / `CreateDefaultMaterial` 0x4efdc0 (admin +0x30 = a material resource whose +0x70 is an
eCShaderDefault: TransformationType 7, DisableLighting, MaxShaderVersion 3, BlendMode 0; its one sampler at admin +0x38); queue +0x20 /
count +0x28 (entries 0x18: billboard, light set, blend mode, **switch value** = the `GetPasses` argument; section 8 called it
"lights"), qsorted by the engine's qsort with `SortRenderQueue` 0x4f0ff0 (call site checked), emptied (+0x28 = 0) at the end. For
each entry: if the cycle's quads + its quads > 1000 (0x4f0696) and records are open, the open records are drawn first; entries with
0 quads (`bb +0x20`) are skipped entirely; the first added entry of a cycle locks VB +0x48 (4000 x 0x34, DISCARD) and IB +0x50 (6000 x
2, DISCARD) and resets the cycle's quad count; a record {material bb +0, image bb +8, blend, first quad, quads, light set, switch}
starts when the cycle has none or any of the five keys differs from the previous record's; quad q's indices `4q + (0, 1, 3, 1, 2,
3)` at `6q`; vertices = the billboard's prebuilt `bb +0x18` (quads x 0xd0 bytes) copied at `q x 0xd0`. Drawing a cycle: unlock both;
per record the visual's draw record (`admin +0x40`) gets start index 6 first, min vertex 4 first, 4 n vertices, base 0, 2 n
primitives; shader = the material's +0x70 (the resource touched first: QueueData / CacheResource; no shader: skipped) or, without
material and with an image, the default shader with `BlendMode` = the record's and `SetImageResource(admin +0x38, image)`;
`SetGlobalAlpha(1, 0)`, `SetWorldMatrix(identity, 0)`; an illumination on the stack `{light set ? light set +0x10 : null, f = 1,
+0xc = +0xd = 0, static SH +0x90 = 0}`; `GetPasses(shader, switch, &illum, 0, false, 0)`, ExecutePass + `eCGfxVisual::Execute`. Composed
and compared byte for byte (vertex / index data) like the decals; the take-over draws from our own buffers.

**Sky cube faces [C, checked]** (`UpdateSkydomeEnvCubemap`, A 2.4): each face's `eCSkydome_PS::Render` with `+0x3cc = 1` is the
same composition with the cube camera `+0x3c0` (view at the origin, 90 deg, near 1, far 1000) and the face's viewport; that path runs
no `eCGfxFog::Execute` (PS c3 / c4, VS c30 as the admin's pools hold them). Hooked in `eCSkydome_PS::Render`'s draw-tag detour (view
SkyCube), outside Execute, as a non-op unit. With the enhanced water the cube is redrawn whole every frame (5 faces), else one face per
frame during a 10-s refresh.

**SpeedTree wind clock (section 8's side question) [C]**: `eCSpeedTreeAdmin::UpdateWindMatrices` 0x6efa10 reads
`eCTimer::GetInstance() +0x10` (= `eCTimer::GetGameTime` 0x4ceee0, ms) / 1000: the **game time**, the clock the enhanced water
follows (ocean.md 4.5), not real time. `eCTimer::Update` 0x4cf0e0 advances +0x10 by the real time-stamp delta x the time scale +0x34
(not while paused). The wind accumulates `min(max(t - last, 0), 0.04 s)` (DAT_1809f159c) per update at `admin +0x40` and advances every
`eCSpeedTreeWind_PS` with it. So the wind stands still under rvkcompare's time freeze, and its phase is the sum of **clamped** steps:
every frame longer than 40 ms (loading screens at ~0.1 fps, hitches) adds only 0.04 s. Two runs reach a spot with a different history
of long frames (load times; a take-over's CPU cost) and so with a different frozen wind phase: that explains section 8's
waterfall_jungle frond without a real-time dependency [I: those frond draws were not compared]. For N5.4: take the wind from `admin
+0xac` / `+0x6c` at the op (D 2.4), never from a clock of our own.

**Verification** (compare = the composed draw vs the engine's own device state at `prepareDraw`, every field class of sections 7 / 8:
draw count, tag, shaders, declaration, streams, indices, ranges incl. primitive type and base vertex, the CPU vertex / index bytes
against the engine's buffers at its draw, states, textures + sampler states, every CTAB-live float register, all bool / int
registers):

| run | sky dome | sky cube faces | opaque billboards | N5.1 static / N5.2 alpha + refracted / decals (same run) | left to the engine |
|---|---|---|---|---|---|
| compare, 4 spots (harbour_night, sea_horizon, jungle, pub_interior), water off | 22,760 / 22,760 | - | 568 / 568 | - | billboards: unreadable 1 |
| compare, 7 spots (+ beach_night_storm, hero_idle (clock running), harbour_docks), water on (cube every frame) | 31,812 / 31,812 | 158,496 / 158,496 | 0 | 1,932,280 / 154,185 / 47,995, all equal | cube: buffers not created 1; permutations not compiled 256 / 36 / 2 |
| compare, savegame Risen_Save_0009 (`rvk_perfsave` hold 30) | 13,976 / 13,976 | 92 / 92 | 0 (the engine drew none: the tap lists no Billboard draw without op) | 892,782 / 68,770 / 3,062, all equal | cube 2, permutations 186 / 25 / 1 |
| take-over + compare, 6 spots, water on, **validation on** | 28,324 / 28,324 | 147,636 / 147,636 | 573 / 573 | - / 142,407 / 22,955 | as above |
| savegame, take-over switched on at frame 2000 (+ compare, **validation on**, `-MaxFps 60`) | 10,104 / 10,104 (5,880 drawn by us) | 72 / 72 (40) | 0 | - / 49,985 / 2,200 | cube 2 |

(draws equal / compared; 0 mismatches in every field class; in the take-over runs every dropped engine draw equals ours and every own draw
equals its composition at its own `prepareDraw`; 0 validation messages in both validation runs, also in the savegame switch run. The
op tap: every 5-s window clean except one savegame window with two unexplained 0-draw Animation ops (outside scope, as in section 8).)
The first cube build compared 111,987 of 149,316 face draws different in one field: s0 (the shadow mask stage of the billboard shaders)
was composed as the admin's image while the engine had none: `HalDevice::SetRenderTarget` (as `eCDX9Device`) unbinds all 16 stages, and
UpdateSkydomeEnvCubemap's BeginMaterial runs **before** the face's SetRenderTarget, so the admin stages are empty at the faces' draws
(a new engine rule; composed since). The one billboard skip ("unreadable": a queue read / count check of one frame) was not
investigated further.

Images (`water-compare.ps1`, the 6 spots; mean |d| 8-bit / share of pixels with a channel |d| > 32; engine vs take-over with
`native.scene_ops_groups=63`, engine vs engine = the noise floor; `out\water\20261027-230512` / `230911` / `231059` / `231432`):

| spot | water off: take-over | water off: noise | water on: take-over | water on: noise |
|---|---|---|---|---|
| harbour_night | 0.50 / 0.037 % | 0.56 / 0.036 % | 0.51 / 0.027 % | 0.43 / 0.030 % |
| beach_night_storm | 4.54 / 3.215 % | 4.07 / 3.442 % | 42.14 / 29.21 % | 41.65 / 28.55 % |
| sea_horizon | 0.41 / 0.000 % | 0.77 / 0.000 % | 1.48 / 0.000 % | 0.12 / 0.000 % |
| jungle | 1.14 / 0.149 % | 1.35 / 0.240 % | 2.31 / 0.740 % | 1.12 / 0.170 % |
| pub_interior | 0.58 / 0.003 % | 0.49 / 0.004 % | 1.19 / 0.101 % | 0.63 / 0.002 % |
| hero_idle | 2.20 / 1.698 % | 3.16 / 2.773 % | 4.87 / 1.718 % | 1.72 / 0.966 % |

`shot-reviewer` (out/n52c/review: grids, x8 / x12 diff maps): water off, every spot at the noise floor; no dome seam, haze, cloud,
sun / moon / star, billboard, decal, transparency or geometry difference anywhere. beach_night_storm with water on: the engine picture
caught a lightning flash, both enhanced runs did not (take-over vs noise run directly: 2.04, no sky-cube seam visible in either; the
flash's reflection is not judged by these pictures). **With water on the take-over darkens the sky slightly** (sea_horizon: the
upper image bands -2.9 .. -3.5 levels, the noise run -0.1; hero_idle about -4 %): reproduced weaker in a repeat (sea_horizon 0.55,
bands -1.1; noise 0.14), absent with water off. Bisected by group at sea_horizon (noise 0.12 - 0.14): groups 31 (no cube) 0.17,
sky op alone 0.26, cube faces alone 0.56, all 0.55 - 1.48 - and **compare only (no draw of ours, the engine draws everything) 0.36
with the same darker-sky bands (-0.7)**. So the shift is not in the drawn content (which compares equal) but follows our CPU cost:
with the water on the cube's five faces are composed every frame, the frames take longer, and the frozen spot reaches its capture
with a different history [I: most likely more game time / weather blending between rvkcompare's `set_game_time` and its time freeze,
which moves the sky colours; the same mechanism as the wind phase above; not proven]. Savegame switched inside one run (frames 1960 /
1980 off, 2020 / 2040 on): 3.61 / 0.69 % (off -> off), 4.49 / 1.08 % (across the switch, 40 frames apart), 3.59 / 0.65 % (on -> on);
the 1,731 8x8 blocks static in both same-mode pairs stay static across the switch (mean 0.038; 0.016 / 0.018 within the modes).

Cost (savegame, `-Exe RisenTest.exe -Vsync off -Perf`, `rvk_perfsave` hold 40, the last six 5-s `perf:` windows; the machine idle (no
ffmpeg / find.exe, checked before); scene ops on = groups 63, i.e. N5.2 + N5.2c; out/n52c/perf_1..4_rvk.log): GPU unchanged (1.50 /
1.51 ms water off, 2.00 - 2.03 / 2.01 - 2.02 ms water on); CPU engine + HAL per frame water off 3.36 - 3.45 ms (p50 frame 3.73 - 3.83 ms,
269 - 276 fps) -> 3.80 - 3.93 ms (p50 4.19 - 4.30, 239 - 246 fps): **+0.45 ms**; water on 4.14 - 4.21 ms (p50 4.58 - 4.62, 222 - 225
fps) -> 4.88 - 4.93 ms (p50 5.29 - 5.34, 192 - 193 fps): **+0.73 ms**, the extra ~0.3 ms being the sky cube's five faces composed and
issued every frame. As before the engine's code still runs for every taken-over op / path. Our own CPU-geometry buffers are now locked
DISCARD over just the range a draw reads (was the whole 512 KB / 64 KB buffer per draw: a full ring copy per decal / billboard / sky
object draw).

Engine rules newly learned [C]: the sky's camera hack recomputes the inverse view and the view-projection with SharedBase's `Invert` /
`operator*` (projection on the left) and the restore does it again; stars and moon share one eCShaderDefault re-configured (BlendMode,
image, colour) between the two draws every frame; the sun's billboard and its VB are rewritten every frame (a plain lock of a managed
buffer); the dome's sky / haze / sun registers are pool registers written by its shader's property notify, not by Render (PS c6 lags
Render's field writes by one notify); `eCShaderSkyDome` has one version object (2) and eCShaderBase's register map (no global, no
lighting pool); the billboard queue's fourth field is the GetPasses switch value, records break on five keys, cycles hold 1000 quads;
in the sky cube `SetRenderTarget` empties every stage after the refresh's BeginMaterial, so the admin stages are unbound at the faces'
draws; the SpeedTree wind runs on the game time with 40-ms clamped steps.

**Not yet / open**: the billboard queue was met only at the spot runs (~570 draws; none in the savegame runs this time, where N5.0 had
~1 per view): a billboard-heavy view was not found; the one "unreadable" billboard skip is not explained; the lightning flash in the
sky cube (beach_night_storm) is not judged (no flash in the enhanced pictures); the water-on sky shift is attributed to our CPU cost
[I], not proven; the overlay / additive light passes of section 8 still never met.

## 10. N5.3 result: skinned actors (2026-10-28)

Group **64** of `native.scene_ops_groups` (behind `native.scene_ops` / `native.scene_ops_compare` as in sections 8 / 9; the entity
overlay passes of actors need bit 2 as for meshes): the main view's `eCRenderContextAnimation2` ops in the z, solid, alpha and
refracted lists. Code: `src/native/static_ops_actors.cpp` (+ `materialDraws`, factored out of `subMeshDraws` in `static_ops.cpp`:
GetPasses + overlays + ExecutePass over any geometry), the pure rules in `static_compose.*` (`pickMorphTargets`, `actorLod`,
`actorPrimitive`, `primitiveBones`, `boneRegister`, `morphStreamOffset`; test `actor_morph_targets_and_bones`). Read in the
disassembly of the test copy's Engine.dll (capstone) and the dumps `skin_dump.txt` / `nativemesh_dump2.txt`; all [C] unless marked;
"checked" = held in the compare runs below. The composition checks 13 code sites (call targets, the `+0x250` / `+0x4c` /
`+0x1f0` / `+0x48` operands) and the eCShaderDefault slots `+0x1f0` SetBoneMatrix / `+0x1f8` SetMorphTargetWeights at first use.

**The op [C, checked].** Slots 0 / 1 / 3 (`0x4ddc10` Render, `0x4ddc60` RenderAlpha, `0x4ddcb0` RenderZPass) all call
`eCAnimation_PS::Render(PS = context +0x18, desc & 0xffff = actor, desc >> 16 = primitive, zpass, fade)` with (false, op fade) /
(false, op fade) / (true, 1.0 = DAT_1809f1698). **Slot 2 (RenderRefracted) is eCRenderContext's empty function**: an actor primitive
whose material IsRefracted is requested into the refracted list (D 1.1) and never drawn (none occurred in the runs below).
`eCAnimation_PS::Render` 0x4d6fe0: overlay flags from the entity (`PS +0x10`) `+0x28` (0x200000 -> 2 frozen, 0x400000 -> 1 burned,
0x800000 -> 4 highlighted); the wrapper = `PS +0x30` (switch `PS +0x20`) for actor 0, else body part `PS +0x1b0[actor - 1]` `+0x30`
(switch part `+0x20`); nothing without the resource (`wrapper +0x18`) or its render hook (`resource +0x98` =
eCAnimation_HardRenderActor2, HRA) or when `UpdateRenderMatrices(wrapper, frame id)` returns false (no ActorInstance `wrapper +8` or no
`resource +0x70` actor). The frame id = `[FUN_1800a1c40() + 0x4c]` (a module getter; Render calls it first).

**Palette [C, checked].** `UpdateRenderMatrices` 0x4dfee0 rebuilds the palette (`wrapper +0x48`, count `+0x50`, capacity `+0x54`)
when the stamp `+0x58` (32-bit) differs from the frame id or the count is 0: n = the actor's bone count (`resource +0x70 +0xd8`),
`palette[i] = X[i]` (ActorInstance `+0x28`) then `FUN_1800a2710(palette[i], W[i])` (ActorInstance `+0x18`): an inline 3x4 affine
product `a *= b` of Engine.dll's own (not SharedBase's). The composition reads the stored palette when the stamp is this frame's,
else computes the entries it needs with the same function on copies (bit-equal: the first op of an actor in a frame whose cascades
did not render it, ~8 % of the ops; the engine's own refresh happens later inside the same Render). It is never refreshed for actors
nobody renders (rt-shadows.md 10).

**HardRenderActor2::Render 0x4dd650 [C, checked].** LOD = ActorInstance `+0x250` (u32) clamped to 4; the HRA's resource (`HRA +0`)
must be cached (`+0x38 == 3`; QueueData / CacheResource side effects before the check); primitive = `HRA +0x90 + 0x20 *
u16(HRA +0x38 + 0x14 lod (u16 first) + primitive)` (eSHardPrimitive: `+0` start index, `+4` primitives, `+8` min vertex, `+0xc`
vertices, `+0x10` material, `+0x18` bone start, `+0x1a` bone count); shader = material `+0x70` (null: nothing). Bones: for i <
count (the loop runs while `start < u16(start + count)`) `SetBoneMatrix(i, palette + 0x40 * remap[start + i])` (u16 remap `HRA
+0x28`) -> the global pool's c(3 (i + 11)) .. +2, the 3x4 transpose (FUN_180238390, as SetWorldMatrix). Morphs when `HRA +0xa8`
(target count, unsigned) != 0: `SetupMorphTargets` 0x4dd220 keeps three (target, weight) slots starting at (0, 0); for each target
k of the ActorInstance's setup (`+0x1d0`, 0xc-byte entries, weight at `+4`) the weakest slot (1 when w0 > w1 else 0; that one when
its weight <= w2 else 2) takes (k, w) if w is strictly greater; then weight i -> the vector and the stream offset of stream i + 1
(the morph VB) = target x the LOD's vertex count (`HRA +0x48 + 0x14 lod`) x 0x10 through the primitive's SetStreamOffset (slot 6).
Always `SetMorphTargetWeights` (c31.xyz; zeros without targets) and `SetGlobalAlpha(fade, 0)` (c32.w). `RenderPrimitive` 0x4dd0a0:
the material admin's `+0x312` (z pass) -> `ExecuteSpecialPass(false)` + one draw, else `eCShaderBase::GetPasses(shader, switch,
illumination = entity +0xf8 +0x78 (null in the z pass), combi 0, not instanced, flags)` + per pass ExecutePass + the visual (`HRA
+0xa0`) with the primitive's four range fields written into the record (type, indexed, base vertex as created). Overlay passes as
for meshes (section 8). **The engine skins on the GPU only**: eCAnimation_HardRenderActor2 is the one render hook
(`eCMotionActorResource2::GetAnimationRenderHook`), no CPU-skinned path exists. The draws keep the slot's tag (Skinned, context, op,
entity) and no sub-mesh tag (no eCSubMesh::Render), so `draws::Kind::Skinned` and every consumer see them as before.

**Verification** (compare = every field class of sections 7 - 9 incl. bool / int registers; draws equal / compared, 0 mismatches
everywhere; skips = "permutation not compiled", the engine compiles it at that draw):

| run | skinned draws | z / solid / alpha | overlay passes | other groups in the same run | skips |
|---|---|---|---|---|---|
| compare, pub_idle + hero_idle + pub_interior, actors shown | 173,284 / 173,284 | 82,733 / 89,651 / 900 | 6,928 (highlighted) | alpha / refracted 64,143, sky 12,948, all equal | 52 |
| compare, savegame Risen_Save_0009 (`rvk_perfsave` hold 30) | 161,686 / 161,686 | 80,845 / 80,841 / 0 | 0 | 71,138 / 3,122 / 14,436, all equal | 60 |
| compare, 9 spots, actors shown, `focus=1` (+ `native.static_ops_compare`) | 396,531 / 396,531 | 183,175 / 200,696 / 12,660 | 17,660 | static 2,333,910 equal, **incl. 2,696 mesh overlay passes** | 164 |
| compare, 13 spots (section 8's list), actors shown | 448,162 / 448,162 | 213,040 / 223,341 / 11,781 | 10,335 | all equal | 167 |
| take-over + compare, savegame, hero walking (`rvk_rtwalk`; ~200 units, then a dialogue), **validation on** (0 messages), groups 127 | 160,215 / 160,215 | 80,116 / 80,099 / 0 | 0 | every group drawn by us, all equal | 101 |
| take-over + compare, new game's beach, the hero walking ~2,700 units (`rvk_rtwalk load=0`, 6.5 s of walking), groups 127 | 104,648 / 104,648 | 51,884 / 52,548 / 216 | 676 | every group drawn by us, all equal | 54 |
| take-over + compare, savegame switched 63 -> 127 at frame 2000, **validation on** (0 messages) | 197,186 / 197,186 | 96,412 / 100,774 | 0 | | 15 |
| take-over + compare, pub_interior / pub_idle / hero_idle, actors shown, groups 66, **validation on** (0) | 184,863 / 184,863 | 88,617 / 95,481 / 765 | 6,874 | | 52 |

In every take-over run each own draw equals its composition at its own `prepareDraw` (e.g. 160,215 of 160,215). Statistics over
the runs (~1.6 M skinned ops): **actor LOD always 0** (ActorInstance `+0x250` never non-zero; its writer is still not found: EMotionFX
internal, `ms_fMaxBoneLODDistance` unreferenced), 73 - 93 % of the ops are body-part actors (heads, armour, hair), 30 - 41 % have morph targets,
of those 2 - 4 % with a non-zero weight (blinks, speech), burned / frozen never occurred, the refracted list never held an actor. The
**highlighted overlay** (bit 4) occurs on the NPC the interaction focus is on (rvkcompare `focus=1` pins it with
`risen.interact.set_focus`; in the pub it happens without it); it is the entity's flag, so the focused NPC's attached mesh entities
(weapon, items) get it too [I: the first overlay passes on mesh ops, section 8 had none, appeared only in the focus run].

**0-draw Animation ops** (sections 8 / 9: "NOT clean" tap windows). Section 8's were whole actors (every primitive of one entity or
body part, z and solid) in the windows of rvkcompare's teleports pub_interior -> monastery_hall and lava_cave -> crystal_cave, read
without a condition (state -1). The tap now reads a skinned op's draw condition at its slot entry (`frame_tap animationState`: bit 1
wrapper + resource + hook, 2 UpdateRenderMatrices can run, 4 the hook's resource cached, 8 the primitive's shader, 16 passes for the
list), and a 0-draw op with a bit missing counts as "nothing to draw". In the N5.3 runs (~1.6 M skinned ops, ~190 tap windows incl.
the same 13-spot teleport sequence) **no 0-draw Animation op occurred**, so the cause is [I]: an actor appearing at a teleport before
its HRA resource / material is cached (the streaming rule of mesh ops, N5.0); a recurrence now names its missing bit in the tap's
mismatch line. The 0-draw **Vegetation** ops recurred once (13-spot run, the sea_cliff -> harbour_mouth teleport, frame 4289: one
vegetation entity's z / solid ops with desc 0x0 / 0x23 / 0x24, key 0, state -1): left for N5.4.

**Consumers** (savegame hold, `native.scene_ops_groups` 2 -> 66 at frame 2400 in one run, `-MaxFps 60`):
* RT skinning + GI characters (`shadows.rt=near`, `debug.rt_skin_verify`, `gi.enabled`): `rt: skin verify` max error 0.0044 units
  before and after (1.1 - 1.45 M vertices per second either way); `rt: scene: characters` 99.1 % / 99.3 % of the characters' refits
  from this frame's pose skinned by compute before / after, solid-pass draws with a job 47 % / 47 %; the counts follow the NPCs in
  view (61 -> 54 characters).
* Object motion (`upscale.mode=native_aa`, `upscale.object_motion=characters`; without RT, where object motion takes its own
  captures): 28,879 / 25,682 captures per 10 s before, 28,818 / 28,807 / 26,297 / 24,610 after, 41 - 48 tracked draws throughout.

**Images.** `water-compare.ps1` (water off both sides, actors shown, take-over groups 66 vs the plain engine; mean |d| 8-bit / share
of pixels with a channel |d| > 32), `out\water\20261028-003032` (take-over) and `003319` (a second engine run = the noise floor):

| spot | engine vs take-over | engine vs engine |
|---|---|---|
| hero_idle (clock running) | 4.20 / 1.504 % | 10.43 / 11.07 % |
| pub_idle (clock running) | 1.19 / 0.022 % | 2.74 / 1.297 % |
| pub_interior (frozen) | 0.66 / 0.153 % | 0.95 / 0.030 % |

`shot-reviewer` (crops engine / take-over / noise, the 8-frame motion sequences 1 frame apart; out/n53/review): faces, hands, hair,
shirt, straps, trousers, feet identical; the only differences are the idle animation's phase (head / arm angle, smaller than in the
noise run); no popping or flicker between consecutive frames. Morphs (blinks), the highlight and weapons are too small in these views
to judge by eye (they compare equal field by field). Savegame switched inside one run (frames 1940 / 1960 / 1980 groups 63, 2020 /
2040 / 2060 groups 127): on the 2,634 8x8 blocks static in all four 20-frame same-mode pairs, the 40-frame pairs give mean |d|
0.271 before, **0.190 across the switch**, 0.118 after, 0 blocks above 1.0 in any; a first switch run with 20-frame same-mode pairs
showed 41 of 16,118 static blocks moving across the 40-frame switch pair, all on slowly moving things (the chickens pecking, the
bushes, an NPC in a doorway).

**Cost** (savegame, `-Exe RisenTest.exe -Vsync off -Perf`, `rvk_perfsave` hold 40, scene ops taken over in both runs, groups 63 vs 127,
the last four 5-s `perf:` windows; the machine idle before the runs (2.6 % CPU, `sysprobe`); out/n53/perf_off / perf_on_rvk.log): GPU
unchanged (1.50 vs 1.47 - 1.50 ms); CPU engine + HAL per frame 3.70 - 3.73 ms (p50 frame 4.01 - 4.07 ms) -> 4.02 - 4.04 ms (p50 4.35 -
4.40 ms): **+0.32 ms** for the ~38 skinned ops per frame (44,308 per 5 s), i.e. ~8 us per op (the bone / palette reads, the pool mirror,
the state save / restore and the second set of setter calls; the engine's own code still runs for every op).

Engine rules newly learned [C]: eCRenderContextAnimation2 has no RenderRefracted (refracted actor primitives are never drawn); the
palette is the product `X[i] *= W[i]` with Engine.dll's inline affine function, stamped per frame by a module's frame id and
rebuilt at the actor's first Render of a frame (in the cascades or the main view, whichever comes first); a primitive's bones are a
remapped subset (<= 66) set from slot 0; the morph weights always reach c31.xyz (zeros without targets), the three strongest by a
"replace the weakest when strictly greater" rule, their offsets into the primitive record (persisting); RenderPrimitive keys the z
pass on the admin's `+0x312`, not on its bool; the actor LOD stays 0; no CPU skinning exists; the highlighted overlay follows the
interaction focus and reaches the focused NPC's attachments.

**Not yet / open**: burned / frozen overlays and refracted actor primitives never met (composed by the same rules); the 0-draw
Animation ops not reproduced (cause inferred); the actor LOD writer; as before the engine's code still runs for every taken-over op.

## 11. N5.4 result: SpeedTrees, tree billboards, vegetation (2026-10-28)

Groups **128** (the SpeedTree ops + the tree billboards' ops) and **256** (the vegetation ops) of `native.scene_ops_groups` (behind
`native.scene_ops` / `native.scene_ops_compare` as in sections 8 - 10; default mask now 511). Code: `src/native/static_ops_trees.cpp`, the
pure rules in `static_compose.*` (`planTreeBatches`, `treeRange`, `planGrass`, `grassInstance`, `vegetationAlpha`, `billboardPassPairs`;
tests `tree_*`, `grass_*`, `billboard_passes_pair_variants`, `native_attribution_tree_and_grass_zero_draws`), the draw conditions in
`frame_tap.cpp` (`treeState`, `vegetationState`). Read in the decompiles `frameD_dump1..3.txt` (D-contexts.md 2 - 4) and the capstone
disassembly of the test copy's Engine.dll (helpers in `B:\DevDrive\risen-ghidra\n54\`); the composition checks 13 code sites, the
eCShaderDefault slots +0x220 / +0x228 / +0x238 and eCShaderLeafs' vtable at first use. All [C] unless marked; "checked" = held in the
compare runs below.

**SpeedTree ops [C, checked]** (`eCRenderContextSpeedTree::Render` 0x6ee320: desc = geometry type; `RenderZPass` 0x6ee3f0: desc = request
mask << 16 | type, bit 0x10 of the mask = a shadow-map op, never in the main view):
* The render buffer = PS +0x38 (the tree resource) +0x78. Queues: solid +0xb8 / count +0xc0, z +0xd0 / +0xd8 (0x18-byte entries {tree PS,
  illumination, depth key}); the first Render / RenderZPass of a frame qsorts the queue (while the stamp +0xe0 / +0xe4 is non-zero) with the
  engine's qsort 0x4a4aa0 + `SortInstances` 0x6f2180 / `SortInstancesZPass` 0x6f2120: the composition sorts a copy the same way (sites
  checked).
* `RenderBranches` 0x6f1cd0 / `RenderFronds` 0x6f1880 / `RenderLeafs` 0x6f1390 and the z passes 0x6f1030 / 0x6f0cc0 / 0x6f0860: material
  `rb +0x80 + 8 t` (`RequestData(1)`), shader +0x70; for eCShaderDefault / eCShaderLeafs the TransformationType must be 3 / 4 / 5-6 (else a
  log line, nothing drawn); **MaskReference forced to 80** every time (with the property notifies); branches (solid and z) and fronds (solid
  only: `RenderFrondsZPass` has no SetGlobalAlpha) call `SetGlobalAlpha(1, 0)`.
* Instances: branches / fronds take every queued tree whose LOD index (`tree +0x48` / `+0x50`) is >= 0, with strip length `rb +0x40 / +0x50
  [lod]` and start index `rb +0xe8 / +0xf8 [lod]`; per slot `SetWorldMatrix(entity +0x60)` (c33 + 4 i) and `SetSpeedTreeInstanceData((tree
  +0x6c, rb +0xa0 / +0xa4))` (c32 + 4 i .xy). Leaves: per leaf LOD L of `CSpeedTreeRT +0xf4` (index count `rb +0x40 + 0x10 t + 2 L`, start
  `+0xa8` from there; a LOD with 0 indices ends the op) the trees with `tree +0x58 == L` (alpha `+0x60`) or else `+0x5c == L` (`+0x64`):
  world, `SetGlobalAlpha(80 / alpha)` (c32 + 4 i .w), `SetSpeedTreeInstanceData(wrapper +0x34)`.
* Batches (`sc::planTreeBatches`): a batch ends before an instance when its 8-bit count equals the limit `rb +0x128 + t`, when the strip
  length differs (branches / fronds), and in the solid passes when the light **list** (illum +0) differs or the instance or the batch's last
  one has static SH / lights (+0x90 / +0xd). A batch is drawn with its last instance's illumination and its first instance's start index;
  **a fronds batch with strip length 0 is not drawn** (its slots are still written).
* Draw: `GetPasses(shader, 0, illum, 0, instanced, 0)` + ExecutePass per pass (solid); z: `ExecuteSpecialPass(true)` = special pass `type *
  2 + 1` in all three (`mov dl, 1` checked). **The instancing mode (section 5's open item `admin +0x320`) is hardware in every op of every
  run** (1.3 M SpeedTree ops counted, 0 software): stream 0 frequency `n | 0x40000000`, one instance's counts (`strip - 2` primitives, leaves
  `indices / 3`), **branches start at index 0**, fronds and leaves at their start index. The software path (`(strip + 2) n - 4` primitives,
  `n * vertsPerInstance` vertices) is composed by `sc::treeRange` but never met.
* Wind: c14..c22 = the 3x3 transposes of the three matrices at `eCSpeedTreeAdmin +0xac`, c23..c25 = that of the application camera's matrix
  (`app +0x38` (else `+0xc8`) `+0x10`), c26..c29 = the rock / rustle angles at `+0x6c`, as `SetSpeedTreeData` 0x6b7df0 writes them during
  the database's Request (nothing when no wind PS exists, `+0x58 == 0`; then the pool's values): composed natively (compare classes "vs wind
  c12 c14-22" / "vs leaf angles c23-29"), never from a clock of ours (section 9).
* The draws keep the slot's tag (SpeedTree, context, op, entity) and no sub-mesh tag.

**Tree billboards [C, checked]** (`eCRenderContextSpeedTreeBB::Render` 0x6ee8d0, `RenderZPass` 0x6ee8e0; desc = cell << 8 | sub-mesh of the
area's billboard mesh `PS +0x30`): `UploadTexCoords` 0x6ec150 writes c32..c36 = the 5 fixed vec4 at 0xbfcbd0 and c37 + j = the PS's vec4 list
(+0x40) over the sub-mesh's range (+0x58: 0xc-byte entries, +4 first, +8 count); z = `eCMeshResource2::RenderZPass` with a null world twice,
option false then true (special passes 0 / 1); solid = `eCMeshResource2::Render(null world, sub-mesh, cell, switch 0, entity +0xf8 +0x78, null
fade)` -> `eCSubMesh::Render` (no world / alpha writes; the cell's range for cell != 0, cell 0 draws the whole sub-mesh) with
**TransformationType 11's pass pairs**: `GetPasses` 0x6cc970 marks every pass of the shared array VS variant 1 (flags | 0x400) and appends the
array again as variant 2 (| 0x800), i.e. `vsIdx` 4 / 5 (non-instanced, variant). The solid draws carry eCSubMesh::Render's sub-mesh tag.

**Vegetation [C, checked]** (`eCRenderContextVegetation::Render` 0x70d4b0 / `RenderZPass` 0x70d4d0 -> `eCVegetation_PS::Render` 0x707800,
desc = the grass type): the PS's visible grid nodes (+0x98, count +0xa0, 0x118-byte entries: +0 light set, +8 grid node, +0x10 the
illumination, +0x1d / +0xa0 its static lights / SH flags) in order; per node its instances of the type (`grid node +0x30 [type]` = (start,
count) into `+0x18`, 0x2c-byte entries) are copied into the vegetation admin's shared buffer (+0x38, 1000 instances, cursor +0x40: DISCARD
at 0, NOOVERWRITE after; `Process` resets it each frame) as `[pos (+4), wind = type +0x2c != 0 ? 1 : 0, rotation (+0x10), scale (+0x20,
+0x24), lift = type +0x20 != 0 ? 0.8 : 0]` (`sc::grassInstance`). Draws (`sc::planGrass`): when the buffer is full (the cursor wraps to 0),
in the solid pass before a node whose light set differs or when it or the previous node has the flags (a node without instances of the
type still switches the illumination), the rest at the end. `RenderMeshInstanced` 0x707e40: the type's mesh (+0x58) cached with its visual,
the admin's buffer attached as stream 1 (`AttachVertexData`: its stride, offset = slot x 0x2c, frequency 0x80000001), stream 0 `n |
0x40000000`, cull `2 - two-sided (+0x30)`, the first sub-mesh's range (indexed list), `SetVegationAlpha(grid +0x50, +0x54)` = c31.xy (`-1 /
R`, `(R + start) / R`, `R = (end - start) / (1 - maskRef / 256)`) and c32.w = 1, then `GetPasses(shader, 0, illum, 0, not instanced, 0)` or
`ExecuteSpecialPass(false)`; frequencies reset and stream 1 detached afterwards. The instance bytes are compared with the engine's buffer
at its draw (stream 1) and, in a take-over, drawn from our own instance buffer (appended within the frame: one ring copy per frame). c12
(`eCVegetationAdmin::Process` 0x7016b0: game time / 1000, admin +0x44 / +0x50 / +0x54) is composed natively unless the clock moved between
Process and the op (rvkcompare sets the game time at spot changes: 376 ops in the 8-spot run, 0 at the savegame), where the pool's copy is
the shader's value.

**0-draw ops, checked at run time.** The tap now reads the draw condition of SpeedTree and vegetation ops at the slot's entry (`Op::state`):
SpeedTree bits 1 render buffer + visual + queue, 2 shader + TransformationType, 4 a queued tree with the type's geometry at its LOD (fronds:
a non-zero strip); vegetation bits 1 instances in a visible node, 2 the type's mesh cached, 4 the shared buffer, 8 the first sub-mesh's
shader. **Every 0-draw SpeedTree op in the runs below lacks bit 4** (all its queued trees at a LOD without that geometry; e.g. 34,602 of
749,634 SpeedTree ops at 8 spots, 5,604 at the savegame; every tap window clean): section 6's tree-LOD rule is now checked. The 0-draw
**Vegetation** ops of sections 8 / 10 (the sea_cliff -> harbour_mouth teleport) did not recur in the 13-spot run with the same teleports
(31 tap windows, all clean); their logged sort ids (0x13b, 0x156: no additional sort id, where the vegetation ops carry 0x1a << 12) point to
a type whose material had no shader yet at the request, i.e. a mesh / material still streaming in after the teleport [I]; a recurrence now
names the missing bit.

**Verification** (compare = every field class of sections 7 - 10 incl. bool / int registers, the vegetation's stream-1 bytes and the two
new wind classes; draws equal / compared, 0 mismatches everywhere; skips = special passes / permutations not compiled at the world's first
frame, which the engine compiles at that draw):

| run | SpeedTree + tree billboards (z / solid) | vegetation (z / solid) | skips |
|---|---|---|---|
| compare, 8 spots (jungle, waterfall_jungle, lake_jungle, swamp_camp, swamp_pond, beach_sun, hero_idle (clock running), shadow_inland_a) | 2,952,509 / 2,952,509 (966,495 / 1,986,014) | 697,008 / 697,008 (188,046 / 508,962) | 34 / 13 |
| compare, the 13 spots of section 8 (teleports), actors shown | 2,618,570 / 2,618,570 | 733,945 / 733,945 | 33 / 15 |
| compare, savegame Risen_Save_0009 (`rvk_perfsave` hold 30) | 1,185,092 / 1,185,092 | 357,152 / 357,152 | 27 / 11 |
| compare, savegame walk + turn (`rvk_rtwalk`), `lod.trees=3`, `lod.meshes=3`, `lod.view=1.5` | 828,483 / 828,483 | 202,076 / 202,076 | 30 / 16 |
| take-over + compare, 4 spots, **validation on** (0 messages) | 1,947,391 / 1,947,391 | 395,701 / 395,701 | 33 / 16 |
| take-over + compare, savegame switched 127 -> 511 at frame 2000, **validation on** (0 messages) | 511,497 / 511,497 | 162,288 / 162,288 | 0 |
| take-over + compare, jungle + swamp_camp, own instance buffer (below), **validation on** (0 messages) | 1,702,957 / 1,702,957 | 310,385 / 310,385 | 34 / 9 |

In the take-over runs every dropped engine draw equals ours and every own draw equals its composition at its own `prepareDraw`. Statistics of
the 8-spot run: 749,634 SpeedTree ops (branches 205,088, fronds 236,998, leaf cards 222,346, leaf meshes 85,202) with 5.4 M instances in 2.4
M batches (179,326 fronds batches at strip length 0, not drawn), 752,757 leaf LOD passes; 376,468 tree billboard ops; 336,390 vegetation ops
with 45.8 M instances in 697,008 draws (41,658 buffer wraps). The first builds found four rule errors, all fixed before the counts above:
`RenderFrondsZPass` writes no global alpha; the leaves' material class is eCShaderLeafs (vtable 0x9d7be8: eCShaderDefault's slots except
GetObjectType / Create / GetEllementTypeName / GetShaderFileName); TransformationType 11 duplicates the passes; c12 lags the clock when a
scenario sets the game time.

**Consumers** (savegame hold, `shadows.rt=near`, `shadows.rt_wind`, `gi.enabled`, groups 127 -> 511 at frame 2400, `-MaxFps 60`;
out/n54/consumers_rvk.log): `rt: scene: characters 19 + animated SpeedTrees 37`, tree draws 94,200 per 5 s (83,100 beyond the range, 11,100
captured), BLAS refits from this frame's capture ~5,680 / the previous frame's ~11,130 in every window before and after the switch; GI 300
updates and ~31,140 GI draws per second before and after. The lod detours act before Execute (the composition reads the LOD fields they
produce): equal at `lod.* = 3 / 3 / 1.5`.

**Images** (`water-compare.ps1`, water off on both sides, take-over groups 384 vs the plain engine, `-Burst 8 -Interval 1`;
`out\water\20261028-013335` = engine A + take-over, `013815` = engine B; mean |d| 8-bit / share of pixels with a channel |d| > 32):

| spot | A vs take-over | A vs B (noise) | B vs take-over |
|---|---|---|---|
| jungle | 1.96 / 0.519 % | 1.08 / 0.150 % | 1.99 / 0.554 % |
| waterfall_jungle | 2.97 / 2.769 % | 1.96 / 1.194 % | 2.25 / 1.786 % |
| swamp_camp | 1.46 / 0.040 % | 1.32 / 0.030 % | 0.86 / 0.006 % |
| hero_idle (clock running) | 2.00 / 1.337 % | 3.19 / 2.269 % | 2.97 / 1.757 % |

`shot-reviewer` (out/n54/review: crops, x8 diff maps against the noise pair): no hole, missing or extra geometry, texture, cut-out or lighting
difference; at jungle and waterfall_jungle the take-over differs more than the noise pair only as a sway offset (leaf cards shifted by a
fraction of a leaf, the blurred foreground fronds): the frozen spots' wind phase is the sum of clamped steps over each run's frame-time history
(section 9), which the take-over's CPU cost shifts [I: no composed draw differs]; the 8-frame sequences are frozen (0.00 - 0.03 between
frames) in all runs, hero_idle moves at the same rate in both. **Savegame switched inside one run** (127 -> 511 at frame 2000, frames 1940 /
1960 / 1980 / 2020 / 2040 / 2060): on the 3,652 8x8 blocks static in all four 20-frame same-mode pairs the 40-frame pairs give 0.018 before,
**0.026 across the switch**, 0.024 after (1 block above 1.0 across, 0 before / after); the reviewer (out/n54/review_switch) finds the switch
pair's diff the same size and pattern as the same-mode pairs (wind on leaves and grass, the NPCs), no step in luma. The distant tree
billboards are small in these views: they compare equal field by field, their fades were not judged by eye in a billboard-heavy view.

**Cost** (savegame, `-Exe RisenTest.exe -Vsync off -Perf`, `rvk_perfsave` hold 40, scene ops taken over in both runs, groups 127 vs 511,
the last four 5-s `perf:` windows; the machine idle before the runs (`sysprobe`: < 2 % CPU, no other game / ffmpeg); out/n54/perf_off /
perf_on2_rvk.log): GPU unchanged (1.55 vs 1.50 - 1.51 ms); CPU engine + HAL per frame 4.03 - 4.12 ms (p50 frame 4.42 - 4.52 ms) -> 6.11 -
6.20 ms (p50 6.41 - 6.50 ms): **+2.1 ms** for ~155 tree / billboard / vegetation ops and ~390 draws per frame (~13 us per op: the queue copy
and sort, the per-instance slot writes into the mirror, the pool mirror, the vegetation's instance bytes, the state save / restore and the
second set of setter calls; the engine's own code still runs for every op). The first take-over build drew the vegetation's instance data
with a DISCARD per draw into the 512 KiB CPU-geometry buffer (a fresh ring copy of the whole buffer each time: ring 67.6 MiB per frame);
it now appends to its own 2 MiB instance buffer within the frame (NOOVERWRITE, one ring copy per frame): ring 9.8 -> 12.4 MiB per frame.

Engine rules newly learned [C]: one SpeedTree op draws every queued tree of its resource and type, sorted once per frame by the engine at
its first Render; tree batches break on the light list (not the illumination), the light flags, an 8-bit count at the buffer's limit and
(branches / fronds) the strip length, take the last instance's illumination and the first one's start index; the game runs hardware
instancing (admin +0x320 != 0), with hardware branches drawn from index 0; MaskReference is forced to 80 on every tree material every frame;
the SpeedTree z passes use special-pass option 1, the vegetation option 0, the tree billboards both (two RenderZPass calls); RenderFrondsZPass
writes no global alpha; leaves use eCShaderLeafs; TransformationType 11 doubles the whole pass array (variants 1 / 2); billboard cell 0 draws
the whole sub-mesh in the solid pass; the vegetation fills one 1000-instance buffer per frame across all its ops, draws when it is full and
on light-set / flag changes, and a node without instances still switches the batch's illumination; the vegetation's c12 is the game time of
the admin's Process.

**Not yet / open**: the software instancing path (composed, never met); shadow-map tree ops (main view only here); the 0-draw Vegetation ops
not reproduced (cause inferred); a billboard-heavy view for the billboard fades by eye; as before the engine's code still runs for every
taken-over op (its CPU cost is the engine's plus ours).

## 12. N5.5 result: the shadow views (2026-10-28)

Group **512** of `native.scene_ops_groups` (behind `native.scene_ops` / `native.scene_ops_compare` as in sections 8 - 11; default mask
now 1023): every op of every shadow map's z list. Code: `native/frame_tap.cpp` (`shadowExecuteBegin`, `shadowKind`, `treeState`'s
shadow rules), `native/static_ops*.cpp` (the view kind, `ShadowStats`), `engine/scene_hooks.cpp` (the tap's calls in the
ExecuteShadowMap detour), `engine/shadow_maps.*` (`cascadeObject()`). The **post_SSM shadow mask** (UpdateShadowMask, 9 draws per main
view) is **not part of N5.5**: it is a later stage (no group bit). All [C] unless marked; "checked" = held in the runs below.

**The tap in the shadow views [C, checked].** `eCFrustumDatabase::ExecuteShadowMap` 0x66f630 (disassembled): `ExecuteGfxCamera(db
+0x14c0)`, BeginScene, `qsort(+0x1520 data, +0x1528 count, 0x18, 0x66e670)` at +0x72 (`mov rcx, [rbx + 0x1520]; lea r9, [rip +
cmp]; mov rdx, rax; mov r8d, 0x18; call 0x4a4aa0`: the main z pass's qsort and comparator, checked before use), `BeginZPass(map,
true, false)`, every op's slot 3 (RenderZPass) with `&data[i]`, EndZPass, EndScene. No fog, no other list (a shadow database's caps
say "no z pre-pass", its z list runs anyway). The tap snapshots the z list at the detour's entry, sorts the copy with the engine's
qsort and attributes the view's draws (view Shadow, pass ShadowDepth) with the same order / byte / attribution checks as the main
view; rvk.log `native: frame tap [cascade|terrain map|point-light atlas|other shadow map]: ...` per kind, `views by split index`.
Views are classified by the `eCShadowMapBase`'s vtable:

| view kind | class (vtable RVA) | when | ops seen |
|---|---|---|---|
| cascade | `eCShadowMapDirectionalLight2Ex` **0x9a29e0** (an element of the `eCCascadeShadowMap` split array +0x30, 0x168 bytes each: its index = the cascade) | 3 per frame (every frame with `debug.shadow_stride=1`, else every 2nd while still) | Mesh, Animation, SpeedTree |
| terrain map | `eCShadowMapDirectionalLight2VS` 0x9a2848 (G32R32F, special pass 4) | only when re-aimed (teleports, the sun's 0.5 deg steps) | Mesh |
| point-light atlas | `eCPackedShadowMap` 0x976738 (one ExecuteShadowMap per cell, the atlas object itself as the map) | never met | - |

**Correction to E 2.2**: the cascade splits are `eCShadowMapDirectionalLight2Ex` (0x9a29e0), not `eCShadowMapDirectionalLight2`
(0x9768d0, its base class) [checked: every cascade view's map has vtable 0x9a29e0]. No tree billboard or vegetation op occurred in
any shadow view (their contexts request no shadow-map ops, D 3 / 4 [C]); no other context class appeared.

**Composition differences of a shadow view [C, checked]** (everything else is the main view's z rules of sections 7, 10, 11, unchanged):
* BeginZPass 0x6b9720 with the map: z colour writes = the map's eCDepthMap format (+8) is not a depth format (3..6 = D16 / D24S8 /
  DF16 / DF24): off for the DF24 cascades, on for the G32R32F terrain map; the admin's special-pass type +0x314 = the map's vtable
  +0xf8 (cascades 3, terrain map 4) picks the shaders as before.
* No `eCGfxFog::Execute`: PS c3 / c4 and VS c30 as the admin's pools hold them. The camera c0..c11 and PS c0 / c1 come from the shadow
  camera (db +0x14c0 = the shadow map's own eCCameraBase, its eCGfxCamera +0x1738) and the viewport ExecuteGfxCamera set; c11.w and
  the obstacle PS c5.xy stay the application camera's (compared equal).
* SpeedTree shadow-map ops (desc bit `0x10 << 16`, passed by RenderZPass 0x6ee3f0 to the z passes): branches as in the z pre-pass;
  **fronds only when the leaf cards' vertex count (rb +0x3c) is <= the fronds' (rb +0x3a)** (RenderFrondsZPass 0x6f0cc0: else the op
  draws nothing, not even RequestData); **leaves at the one LOD `min(tree +0x58 + 1, LODs - 1)` with global alpha 1** (RenderLeafsZPass
  0x6f0860; the main z pass's two blended LODs and `80 / alpha` do not apply). The tap's draw condition (`treeState`) follows the same
  rules, so every 0-draw tree op of the shadow views is "nothing to draw" (checked).
* Skinned actors: unchanged (RenderZPass -> `ExecuteSpecialPass(false)`; the palette is usually composed here, since the cascades
  render before the main view and `UpdateRenderMatrices` stamps it later in the same frame, section 10).

**Verification** (compare = every field class of sections 7 - 11 incl. bool / int registers; draws equal / compared; skips =
"permutation not compiled" at the world's first frame, which the engine compiles at that draw):

| run | cascade draws Mesh / Animation / SpeedTree (views) | terrain map draws (views) | equal / compared | skips |
|---|---|---|---|---|
| compare, shadow_inland_a, jungle, pub_interior, harbour_night, `debug.shadow_stride=1` | 546,701 / 182,632 / 483,565 (14,418) | 1,732 (8) | 1,214,630 / 1,214,630 | 193 |
| compare, savegame Risen_Save_0009, `shadows.enabled=true`, `shadows.map_size=4096`, `shadows.range=100`, stride 1 | 323,312 / 118,080 / 383,596 (10,146: split 0 / 1 / 2 = 400 each per window) | 950 (3) | 825,938 / 825,938 | 146 |
| compare, shadow_inland_b / _high / _far, hero_idle (clock running: the sun moves, the terrain map re-aims), actors shown, `shadows.rt=near` | 184,944 / 76,718 / 229,213 (8,805) | 912 (5) | 491,787 / 491,787 | 125 |
| take-over + compare, shadow_inland_a, jungle, hero_idle, harbour_night, actors shown, stride 1, **validation on** (0 messages) | 565,138 / 212,265 / 470,279 (16,749) | 1,927 (9) | 1,249,609 / 1,249,609; own draws 1,249,609 equal to their composition | 191 |
| take-over + compare, savegame switched on at frame 2000 / 2400 (engine mask; enhanced mask + `shadows.rt=near` + `debug.rt_skin_verify`) | | | 224,208 + 380,343 equal (165,480 + 203,569 drawn by us) | 125 + 146 |
| take-over + compare, enhanced mask + `shadows.debug` terrain / cascade term, shadow_inland_a / _b (2 runs) | 180,315 / 67,778 / 216,653 | 712 | all equal | 125 |
| take-over + compare, shadow_inland_a + jungle, stride 1, **sync validation** (`-SyncValidation`) | 369,186 / 89,580 / 377,739 (8,739) with `shadows.rt=near`; 741,257 draws without | 1,141 (5) | 837,646 + 741,257 equal | 154 + 143 |

Every tap window of every view kind was clean in every run (0 out of order, 0 byte differences, 0 unattributed draws, every 0-draw op
explained by its read draw condition, every multi-draw op a whole-mesh z op or a tree batch).

**The point-light atlas never rendered** (pub_interior's candles and torches, harbour_night, the savegame, the inland spots). Why
[C, from the decompiles frameA_dump4 / rvk_shadow_dump2]: `eCPackedShadowMap` (illum +0x380) is created unconditionally
(ApplyConfiguration 0x577a50: 1024², the directional format; no config key, no Shadow Quality gate), but a cell is only requested by
`eCRenderContextPointLight::RequestPrePasses` 0x59e8c0 for an **`eCStaticPointLight_PS` with `CastDynamicShadows` (+0x68) set** (level
`int((d - R) / R) + 1`; its AddShadowCells 0x59c800 needs level < 4 and baked per-face bytes +0xf8 + 0x44 i != 0xff) or an
**`eCPointLight_PS` (dynamic) with `CastShadows` (+0xc0) set** (level + 2 < 6; AddShadowCells 0x598df0: 6 cube faces, near 10, far R),
and only when a face survives `CullShadowCell` and finds a free quadtree cell; without a cell no pre-pass op exists and the atlas is
never drawn. The game's lights at the sampled places have neither flag [I: inferred from the atlas never rendering; the light
entities' flags were not dumped]. When it does render, each cell is one ExecuteShadowMap with the eCPackedShadowMap as the map [C],
which the tap classifies as "point-light atlas" and group 512 composes (special pass type 2; not verified in game).

**Images.** `water-compare.ps1` (water off, engine mask), engine vs take-over at shadow_inland_a / shadow_inland_b / shadow_inland_high /
jungle (mean |d| 8-bit / share of pixels > 32): 13.53 / 18.8 %, 2.00 / 0.69 %, 1.73 / 1.14 %, 1.81 / 0.81 %; engine vs engine 6.38 /
4.3 %, 1.92 / 0.60 %, 1.16 / 0.58 %, 1.02 / 0.09 % (`out\water\20261028-022314`, `022631`). `shot-reviewer` (out/n55/review): no acne,
peter-panning, cascade seam, missing caster or shadow-shape difference at shadow_inland_b / _high / jungle; at shadow_inland_a the
foreground river bed (x 700-1600, y 600-790, under the water surface) is brighter in the take-over picture. Six separate runs of that
spot (3 engine, 3 take-over) put the bed's mean luma at 43.8 / 47.5 / 54.4 (engine) and 87.0 / 65.2 / 80.3 (take-over), pairwise mean
|d| 6.4 - 13.5 in either mix: a strongly history-dependent area (the canopy's swaying leaf shadows over the water; the frozen wind phase
is the sum of each run's clamped frame steps, section 9) [I]. **The same-frame switch at the frozen spot settles it**: rvkcompare
shadow_inland_a held 3,000 frames, take-over switched on at frame 4660, pictures 4600 / 4620 / 4640 / 4680 / 4700 / 4720: mean |d|
**0.000 across the switch** on the whole image (22,400 of 22,400 8x8 blocks static; 0.003 before, 0.000 after; bed luma 69.1 before
and after). The take-over draws the identical image; the separate-run spread is the runs' history, not the shadow views. Savegame
switched at frame 2000 (engine mask, `-MaxFps 60`): on the 3,458 8x8 blocks static in all four 20-frame same-mode pairs the 40-frame
pairs give 0.071 before, **0.062 across the switch**, 0.037 after, 0 blocks above 1.0.

**Consumers** (savegame, enhanced mask + `shadows.rt=near` + `debug.rt_skin_verify`, take-over switched on at frame 2400): `rt: skin
verify` max error 0.0070 - 0.0073 units before and after (395 - 415 k vertices per second either way); `rt: scene: characters` 19 + 37
animated SpeedTrees in the TLAS, refits from this frame's capture 2,850 - 2,869 per window before and after, characters from this
frame's pose 2,850 - 2,869 (compute-skinned 2,808 - 2,848); `enh: shadows.rt` ~300 traced frames per 5 s throughout. The overrides
(`shadows.map_size=4096`, `shadows.range=100`: 3 maps of 4096², splits 1347 / 2945 / 10000) and `debug.shadow_stride` 1 / 2 act before
the views exist (the RenderShadowMaps pre-hook): every view composed equal with them.

**Validation**: 0 messages in the validation run; the sync-validation take-over run without RT shadows: 0 messages. With
`shadows.enabled=true` + `shadows.rt=near` the sync-validation run lost the device after ~1 min (`VK_ERROR_DEVICE_LOST` in
vkQueueSubmit2, then the follow-up VUIDs), **and so did the same run without any native setting** (out/n55/sync_base_rvk.log): a
pre-existing interaction of the synchronization validation layer with the RT shadows, not of N5.5 (no SYNC- hazard was reported before
the loss in either run).

**Cost** (savegame, `-Exe RisenTest.exe -Vsync off -Perf`, `rvk_perfsave` hold 40, scene ops taken over in both runs, groups 511 vs
1023, the last four 5-s `perf:` windows; the machine idle before the runs (`sysprobe`: 0.7 % CPU, no other game / ffmpeg); out/n55/
perf_off / perf_on_rvk.log): GPU unchanged (1.49 - 1.50 ms); CPU engine + HAL per frame 6.16 - 6.23 ms (p50 frame 6.49 - 6.55 ms) ->
6.65 - 6.68 ms (p50 6.89 - 7.39 ms): **+0.45 ms** for ~70,000 shadow-view ops per 5 s (the cascades every 2nd frame while the camera
is still, `debug.shadow_stride` 2). As before the engine's code still runs for every taken-over op (only its GPU work is ours).

Engine rules newly learned [C]: ExecuteShadowMap sorts and runs the z list exactly like the main z pass (same qsort, comparator,
RenderZPass slot), with no fog and BeginZPass's colour writes decided by the map's format; the cascade splits are
eCShadowMapDirectionalLight2Ex; SpeedTree fronds cast no shadow when the tree's leaf cards have more vertices than its fronds, and leaves
cast from one leaf LOD coarser than the visible one at alpha 1; tree billboards and vegetation cast no shadow-map ops; the point-light
atlas has no quality gate but needs lights with CastDynamicShadows / CastShadows, which the sampled places do not have.

**Not yet / open**: the point-light atlas (composed by the same rules, never met; a light with `CastShadows` would show it, `-LogFrame`
then lists the `frame tap [point-light atlas]` views); the views' cameras / viewports / targets / clears and the cascade fit stay the
engine's (read, not composed: the composition takes the shadow camera's matrices and the bound viewport; a native fit is an own-mode
item); the terrain map's post_blur Blur7H / Blur7V passes (effect draws after ExecuteShadowMap) and the post_SSM mask stay the engine's
(the mask is the next stage: section 13); the PSSM channel mapping (section 5) stays open (no atlas, no PSSM pass met; section 13 renders the atlas with a modded light and reads the mapping); as before the engine's code
still runs for every taken-over op.

## 13. N5.5c result: the shadow mask, the terrain map's blur, the shadow views' setup (2026-10-28)

Groups **1024** (the effect draws around the shadows) and **2048** (the shadow views' setup) of `native.scene_ops_groups` (behind
`native.scene_ops` / `native.scene_ops_compare` as in sections 8 - 12; default mask now 4095). Code: `src/native/effect_compose.*` (pure
rules: the fx pass evaluation, the mask's parameter formulas, the setup rules, the draw description and its comparison; tests
`native_effect_*`), `src/native/effect_ops.cpp` (the engine side of 1024), `src/native/effect_ops_setup.cpp` (2048), the detours in
`engine/scene_hooks.cpp` (`EffectTap`: UpdateShadowMask, `eCImageFilterEffect::ExecuteEffect`, `eCImageFilterCopy::OnUpdateOutput`,
`eCShadowMapBase::RenderShadowMap`, ExecuteShadowMap), the fit tap in `engine/shadow_maps.cpp`, the HAL taps (`HalTexture::
CopyImageToCurrentRT`, `HalDevice::SetRenderTarget` / `SetViewport` / `ClearViewport`) and `hal::vulkan::EffectInfo` (our fx effect behind
an engine API effect). All [C] unless marked; "checked" = held in the runs below. Dumps: `rvk_sunshadow_dump2.txt` (the mask's
OnUpdateOutput), `frameF_dump2.txt` (the effect runtime), `imgparam_dump.txt` / `imgparam_dump2.txt` (the parameter classes), capstone
disassembly of the test copy's Engine.dll (the fit's and UpdateCamera's call arguments).

**The effect runtime, as data [C, checked]** (`eCImageFilterEffect`, F 2.1 extended; dumps frameF_dump2, imgparam_dump*):
* `ExecuteEffect` 0x75af70 (vtable +0x1b8 of `eCImageFilterEffect` 0x9ee0b8 **and** `eCImageFilterShadowMask` 0x9c87b0: the mask
  overrides neither it nor UpdateVisual / OnSetParams / the pass functions, only OnUpdateOutput 0x668560): BeginEffectTechnique (+0x198:
  OnSetParams, the API effect's SetTechnique / BeginTechnique), per pass ExecuteEffectPass (+0x1a8: gfx effect +0x28 = pass,
  `eCGfxEffect::Execute` 0x750210 -> API SetPassNumber, skipped when the same effect is active and not dirty) and ExecuteVisual
  (`eCGfxVisual::Execute` 0x5069a0: SetRenderMode +0x18, SetCullingMode +0x1c, SetDepthTesting +0x20, SetDepthWrite +0x24, all
  unconditional (the HAL's master flags make them no-ops), the visual's shader +0x60, the primitive), EndEffectTechnique.
* Parameter objects (`eSImageEffectParam`, filter +0xe0, count +0xe8; base ctor 0x759b00: +0x08 name, +0x10 effect handle, +0x18
  semantic, +0x20 class, +0x24 type, +0x28 / +0x2c rows / columns, +0x30 elements, +0x48 the owning filter, +0x50 the technique-used
  mask): **every Apply is `SetParameter(handle, this + 0x58, N)`** (raw bytes; no SetFloatArray / SetMatrix), the column-major
  `eTEffectParamTransposedMatrix<bCMatrix>` 0x9eddb8 transposes a copy first; arrays (`eTEffectParamArray<T>`) keep a heap buffer at
  +0x58. Set (+0x70) copies N bytes into +0x58. Semantic classes (FUN_180759f00): NEARFAR = `Camera` 0x9ee480 kind 6 = the application
  camera +0xbc (8 bytes); TEXTURESIZEn = `ImageSize` 0x9ee280: the input whose suffix (+0x58) matches, its output image's width / height
  (+0x18 / +0x1c) as two floats. `eSImageEffectParamTexture` 0x9ee880: image +0x58 (mode +0x60: 0 own, 1 the refraction copy, 2 the depth
  map), Apply = `AttachImmImageData` 0x74fef0 (the image's API texture +0x60 -> SetTexture / SetCubeMap / SetVolume), Reset = SetTexture
  null. Inputs (+0xb8, 0x60 each: source filter +0, output +0x10, handle +0x20, suffix +0x58) are attached the same way from the source's
  output image (`eCImageFilterRTBase` +0x70 in output mode 0; `eCImageFilterTexture` / `TextureRandom` by mode +0x38: +0x40, the
  refraction copy `matAdmin +0x268`, the depth map `+0x290`).
* So the draw is a function of engine data: every parameter object's value, the inputs' images, the technique's pass, the visual, the
  bound target; the composition evaluates the fx pass with our own rules (`fxc::applyPass` = the HAL effect's order: pass states, then
  each shader's CTAB constants + preshader and its samplers' `sampler_state` assignments) on the device state at the call.

**The mask's own parameters [C, checked]** (OnUpdateOutput 0x668560, the constants read from Engine.dll: DAT_1809f1718 2.5,
DAT_1809f184c -0.5, DAT_1809f1630 0.5, DAT_1809f15d4 0.2, DAT_1809f16f8 2, DAT_1809f1698 1): R = inv(V) *= inv(diag(P11, P22, 1)) *=
SetToScaling(far) (SharedBase); per cascade DizzerKernelSize 2.5 / size, PCFFilterOffset (-0.5, -0.5, 0.5, 0.5) / size, PCFFilterScale
(size, size), FadOut (far / (0.2 Max), -((Max - 0.2 Max) / (0.2 Max))) (the first cascade also sets them for the shared size and, with a
terrain map, DepthToVarianceShadowMatrix = terrain +0xa0 *= R and VarianceShadowTexture = terrain +0x18), DepthToShadowMatrix = split
+0xa0 *= R, ZDistance = (f P33 + P43) / f (f = the next split's start, the last: Max), ShadowTexture = split +0x18; VSSM: the variance
matrix again, ZDistance 1, the variance texture; techniques UpscaleSSAO (with SSAO), then per split SSM / DSSM (without a terrain map)
+ "_Low" without HighQuality, VSSM with a terrain map, PSSM per packed map with cells. **Parameters the current call has not set yet
keep their last value**: the composition takes those from the parameter object (the carry-over), the ones the unit writes natively.

**The 1/4 depth copy [C, checked]**: illum +0x450 is an `eCImageFilterCopy` (FUN_18014f410: vtable 0x9c61a0, R32F, size mode 1 x 0.25,
input = the depth-map texture filter +0x460); OnUpdateOutput 0x665640 binds its target without depth, viewport (0, 0, w, h, 0, 1), and
calls the input image's `CopyImageToCurrentRT(POINT, null, (0, 0, w, h))` (eCAPITexture +0x60; gated by gfx admin +0x5b8).

**The terrain map's blur [C, checked]**: `eCShadowMapDirectionalLight2VS::RenderShadowMap` 0x5a1b40 after the base render: its post_blur
filter (+0x110, output mode 1 = it draws into the bound target, no depth; input 0 = a texture filter +0x118) runs Blur7H with the map
(+0x18) into the temp image (+0x108, G32R32F, created once), then Blur7V back into the map; `weights7` is an `eTEffectParamArray<float>`.

**The shadow views' setup [C, checked]**:
* The fit (RenderShadowMaps 0x59f200; capstone for the lost arguments): inv(V); a frustum from the box (-1, -1, 0) .. (1, 1, 1) inverse-
  transformed by P and normalised; L = SetIdentity, LookAt(normalised light direction +0x10) **with the camera position (+0x90) as its
  translation row**; splits `(((Max - 1) t + 1) - pow(Max, t)) lambda + pow(Max, t)`, t = i / n, with the engine's own CRT `powf` 0x4ac030
  (Engine.dll links its CRT statically); per split a copy of the frustum with plane 0's d = -start, plane 1's d = the next start (Max),
  its 8 vertices merged into a bCSphere (radius +0, centre +4), the centre through inv(V) then inv(L), `SetBox(centre, radius)`, min z =
  -100000; the split stores L (+0x60), the box (+0xe0), the frustum transformed by inv(V) (+0x100) and its start (+0x160).
* UpdateCamera 0x5a13e0: V = inv(L); P = SetToScaling(2 / dx, 2 / dy, 1 / dz) with translation ((min + max) x -0.5 x s) for x / y and
  -(s.z min z); t = V's translation through P; **P's x / y translation minus fmodf(t, 2 / size)** (the engine's CRT `fmodf` 0x4ad148;
  E 2.2's "2 / size" is right, a first reading of 0.5 / size was off by whole texels); ClipDepth 0 .. max z (camera +0xbc / +0xc0, db
  +0x1694), Perspective off (+0xdc), view +0x10, projection +0x50; the texture matrix +0xa0 = bias (0.5, -0.5, 0.5 + 0.5 / size, -depth
  bias) *= P *= V.
* `eCShadowMapBase::RenderShadowMap` 0x5a1070: CreateRenderTarget, then (one pass with face 6 for 2D maps) eCDepthMap::SetRenderTarget
  0x59fee0 (a depth format 3..6: the working colour buffer of the size, `GetShadowMapWorkingBuffer` 0x575830 = a hash map at illum +0x318
  (buckets +0x320, count +0x328, `(size >> 4) % count`, nodes {key, image +8, next +0x18}), with the map as depth; a colour format: the map
  with its own depth buffer +0x20 or the shared one illum +0x310), SetViewport (0, 0, size, size, +0x50, +0x54), ClearViewPort 0x59fe50 (z
  + stencil for depth formats, else colour too; colour +0x28, z 1, stencil 0), UpdateCamera, the scene (ExecuteShadowMap), ResetRenderTarget
  (the target saved by GetRenderTarget only when both were bound, then the saved viewport). The point-light atlas renders through
  `eCPackedShadowMap::RenderShadowMap` 0x5a08f0 instead (one target for all cells): not part of this group.

**Composition and take-over.** At `ExecuteEffect`'s entry (a mask-chain filter during UpdateShadowMask, identified by the illum
admin's fields +0x430 / +0x438 / +0x440 / +0x448 / +0x458; the terrain map's +0x110 outside it) the unit's draws are composed: the
technique (the mask's from its plan, built at UpdateShadowMask's entry from +0xfc / +0x114 / +0x118 / +0x120: UpscaleSSAO, SSM|DSSM[_Low]
per split, VSSM; the blur's Blur7H then Blur7V; the chain's own +0xf0), the parameter values (the mask's own natively, accumulated over
the plan; semantic ones from their sources; the rest from +0x58), the inputs' images, the pass (`fxc::applyPass`) and the visual
(`fxc::applyVisual` with the HAL's master flags) on the device state at the call, the quad's record (HalPrimitive), the target (the
filter's output image +0x70 with the depth bound at UpdateShadowMask's entry and viewport (0, 0, +0x80, +0x84, 0, 1); the blur: the
bound image, no depth, the whole target). Compare = every field class of the draw: shaders, declaration, streams (incl. frequencies),
indices, range, **all 210 render states** (depth, stencil, blend, colour write, raster, other), the pixel shader's sampled stages
(texture + all 13 sampler states), every float register the shaders read (bytecode + CTAB), all bool / int registers, render target,
depth surface, viewport. Take-over issues the composed draws at the call (only what differs is set; the device restored after) and drops
the engine's at prepareDraw; the copy is made with `stretchRect` at the HAL call instead of the engine's; the setup's device calls get the
composed arguments. The enhanced mask (`shadows.enabled`, a DrawReplacer on the ShadowMask pass) acts on our draws as on the engine's.
rvk.log: `native: effect draws: <unit> <technique>: ...` (units, draws equal / compared, registers / stages / states compared, field
mismatches, drawn by us, own draws vs their composition), `native: shadow setup [cascade|terrain map]: ...`.

**Verification** (draws equal / compared, every field class; units = ExecuteEffect calls; the setup's fits / cameras / device calls; 0
mismatches in every row; masks = UpdateShadowMask calls):

| run | masks | effect draws (SSAO, BlurSSAO x2, UpscaleSSAO, SSM x3, VSSM, Blur + Blur7H / V) | depth copies | setup: cascade renders / fits / cameras / device calls | terrain map renders |
|---|---|---|---|---|---|
| compare, 8 spots (shadow_inland_a / _b / _high / _far, jungle, pub_interior, harbour_night, hero_idle), actors, stride 1 | 9,367 | 93,688 / 93,688 (1.08 M registers, 234 k stages, 17.7 M render states) incl. Blur7H / V 9 + 9 | 9,367 | 24,186 / 24,186 / 24,186 / 120,930 | 8 |
| compare, savegame, enhanced mask + 4096² + 100 m + `shadows.debug=cascades`, stride 1 | 3,627 | 36,276 / 36,276 | 3,627 | 10,863 / 10,863 / 10,863 / 54,315 | 2 |
| compare, shadow_inland_b + hero_idle (clock running), enhanced mask + `shadows.rt=near` | 3,826 | 38,266 / 38,266 | 3,826 | 5,751 / 5,751 / 5,751 / 28,755 | 2 |
| compare, `rvk_pointshadow` (point-light atlas rendering) | 3,620 | 36,208 / 36,208 (+ the PSSM draws left to the engine) | 3,620 | 5,433 / 5,433 / 5,433 / 27,165 | 3 |
| take-over + compare, 5 spots, actors, stride 1, **validation on** | 6,372 | 63,736 / 63,736; drawn by us 63,736, own draws vs composition 57,364 equal | 6,372 | 16,140 / 16,140 / 16,140 / 80,700 (all made by us) | 7 |
| take-over + compare, `rvk_pointshadow`, groups 4095, **validation on** | 2,581 | 25,816 / 25,816 | 2,581 | 3,867 / ... / 19,335 | 2 |
| take-over + compare, shadow_inland_a + jungle, stride 1, **sync validation** (no RT shadows) | 3,106 | 31,070 / 31,070 | 3,106 | 9,306 / ... / 46,530 | 4 |
| take-over switched on mid-run: frozen spot (frame 4660), savegame with RT consumers (2400), savegame (2400) | 6,330 / 3,490 / 3,625 | 63,308 / 34,906 / 36,256, all equal | | all equal | |

First builds: 0 mismatches in the mask chain at the first run; two fixes before the table: the live registers now include every register
the bytecode reads (an effect shader's preshader outputs are not in its CTAB: the first count compared none for BlurSSAO), and the
terrain blur's `weights7` is an `eTEffectParamArray<float>` (left to the engine until added). The setup's first run compared the fit,
the device calls and the target equal and the camera different by whole texels in x / y: the snap divisor is 2 / size (above). The first
frame of a session has no working colour buffer of the size yet (created by that very SetRenderTarget): 3 cascade + 1 terrain renders per
run are left uncomposed ("with a buffer not created yet").

**Images** (whole picture, mean |d| 8-bit / share of pixels with a channel |d| > 32; `water-compare.ps1`, water off; engine vs take-over
of groups 3072 / engine vs engine = the noise floor; `out\water\20261028-043458` / `042405` / `043811`):

| spot | take-over | noise | enhanced mask on both sides: take-over |
|---|---|---|---|
| shadow_inland_a | 7.19 / 7.14 % | 9.34 / 9.25 % | |
| shadow_inland_b | 2.15 / 0.82 % | 1.89 / 0.40 % | 2.63 / 1.58 % |
| shadow_inland_high | 1.15 / 0.49 % | 2.11 / 1.59 % | |
| jungle | 1.99 / 0.54 % | 1.65 / 0.35 % | 2.27 / 1.06 % |

`shot-reviewer` (amplified diff maps against the noise pair, `out/n55c/review`): nothing in the take-over pair outside the engine-vs-engine noise (river foam and splashes at shadow_inland_a, foliage speckle, the jungle's fire, palm sway); at shadow_inland_b a brighter dapple on the shallow water floor (x 880-1360, y 500-760) of the kind the noise pair shows elsewhere; with the enhanced mask the pair sits slightly above the engine-only noise floor (2.85 / 2.52 vs 2.0 / 1.8 luma). The same-frame switches settle both: **same-frame switch** (the run-to-run spread at these spots is the runs' history, section 12): rvkcompare shadow_inland_a held,
take-over switched on at frame 4660, pictures 4600 / 4620 / 4640 / 4680 / 4700 / 4720: **mean |d| 0.000 across the switch** (all 22,400
8x8 blocks static; before 0.002, after 0.000). Savegame (`-MaxFps 60`, switched at 2400, frames 2340 .. 2460): on the 4,112 blocks static
in all four 20-frame same-mode pairs, the 40-frame pairs give 0.093 before, **0.078 across**, 0.073 after (blocks above 1.0: 40 / 63 /
32); the enhanced mask at the frozen shadow_inland_b (switched at 4660; the enhanced mask itself varies a little frame to frame there:
20,875 static blocks): 0.090 before, 0.090 / 0.095 across, 0.074 after (2-step pairs; 1-step across 0.051, same-mode 0.060 - 0.111); with the enhanced mask + RT shadows + `debug.rt_skin_verify` (4,159 static blocks): 0.115 before, 0.119 across, 0.108 after (50 / 81 /
76): the moving NPCs, the hero's idle and the swaying RT shadows, as in the same-mode pairs.

**Consumers** (savegame, enhanced mask + `shadows.rt=near` + `debug.rt_skin_verify`, switched at 2400): `rt: skin verify` max error
0.0071 units before and 0.0071 - 0.0073 after (494 - 515 k vertices per second either way); `rt: scene: characters` 22 + 37 animated
SpeedTrees, characters from this frame's pose 3,300 per window before and after; `enh: shadows.rt` ~300 traced frames per 5 s throughout.

**Validation**: 0 messages in both validation runs (5 spots; the point-light scenario with every group taken over), 0 in the
sync-validation run without RT shadows (no SYNC- hazard). The known device loss of sync validation + `shadows.rt=near` (section 12) was
not re-run.

**Cost** (savegame, `-Exe RisenTest.exe -Vsync off -Perf`, `rvk_perfsave` hold 40, scene ops taken over, groups 1023 vs 4095, the last
four 5-s `perf:` windows; machine idle: no process above 5 % of a core, no ffmpeg / other game): GPU 1.51 vs 1.50 ms; CPU engine + HAL
6.74 - 6.87 ms (p50 frame 6.98 - 7.58) vs 6.85 - 6.91 ms (p50 7.11 - 7.65): **about +0.1 ms** for ~11 effect draws, one copy and 3
cascade setups per frame (the composition per ExecuteEffect evaluates the fx pass and the parameter objects; the engine's code still
runs).

**The point-light atlas and the PSSM channel mapping** (scenario `mods/rvk_pointshadow`: the light spell's item template
`It_Cast_Light` (an eCPointLight_PS) with `CastShadows = true` and Range 800, spawned 1.5 m above the hero on the opening beach at night):
the engine renders its packed atlas (6 cells of 1024², one per cube face, 6 `ExecuteShadowMap` per frame through
`eCPackedShadowMap::RenderShadowMap`) and the mask's PSSM pass (18 draws per mask: 3 passes x 6 cells). The atlas views are composed and
compared by group 512 (section 12): 190,386 + 42,642 + 42,693 Mesh / Animation / SpeedTree draws equal in the compare run, every tap
window clean, and taken over in the validation run. **Channel mapping [checked]**: the cells' colour flags (+0x11c) are 0xff00 (a
dynamic light with CastShadows); the pyramids' vertex colour is ~flags = 0xffff00ff (A 1, R 1, G 0, B 1), the pass writes max(1 - lit,
colour) through ZERO / SRCCOLOR, so **the green channel receives a dynamic CastShadows light's shadow** (the light list's selector +0x68 =
(0, 1, 0, 0) of E 3.2 selects .g: consistent); static lights' flags come from their ShadowMaskIndex (+0x60) [C], none met. **The PSSM
pass is not composed**: its passes run outside ExecuteEffect (BeginEffectTechnique / ExecuteEffectPass / the packed visual directly,
the stencil through the HAL's stencil call, a camera-sphere / cell-frustum test per cell, CPU pyramids built by
`UpdatePackedShadowMapVisual` 0x669e00); its draws stay the engine's (counted: `engine draws outside the units`, 0 without point-light
cells), and the atlas's own setup (`eCPackedShadowMap::RenderShadowMap` 0x5a08f0, one target for all cells) is not part of group 2048.

Engine rules newly learned [C, checked]: image-filter parameters are pushed as raw bytes through one SetParameter slot (a column-major
matrix transposed by its class), and parameters a call does not set keep their last value; TEXTURESIZEn is the matched input image's
size as floats; the mask's filters share the ExecuteEffect / pass functions of eCImageFilterEffect; the 1/4 depth copy is a POINT
CopyImageToCurrentRT; the terrain blur's temp target is created once and Blur7V writes back into the map; the cascade light view's
translation row is the camera position; the shadow camera's x / y snap is fmodf by 2 / size with the engine's own CRT; the working colour
buffer of depth maps is a per-size hash map of the illumination admin; a light with CastShadows renders the point-light atlas and its
shadow lands in the mask's green channel.

**Not yet / open**: the PSSM pass and the atlas's own target / clear / cell cameras (composed by nobody yet: they occur only with
modded lights); the terrain map's fit (UpdateShadowMaps' re-aim: its light view / box are read, the camera from them is composed); the
first frame's working-buffer creation; as before the engine's code still runs for every taken-over unit.

## 14. N5.6 result: particles, strips, light streaks, precipitation (2026-10-28)

Groups **4096** (particle ops), **8192** (strip + light streak ops) and **16384** (the precipitation op of the late list +0x1548) of
`native.scene_ops_groups` (behind `native.scene_ops` / `native.scene_ops_compare` as in sections 8 - 13; default mask now 32767).
Code: `src/native/static_ops_fx.cpp` (+ the shared draw path of `static_ops.cpp`: non-indexed draws, the particle shader's global pool in
`constantsInto`, a draw-tag override), the pure rules in `static_compose.*` (`particleBlendMode`, `particleTile`, `particleDepthOrder`,
`stripAlpha`; test `particle_rules_follow_update_vertices`), the spell scenario `mods/rvk_spells`. Read in the capstone disassembly of the
test copy's Engine.dll (helper `B:\DevDrive\risen-ghidra\n56\edis.py`: exports + IAT names) and the dumps frameD_dump1..3. All [C] unless
marked; "checked" = held in the compare runs below. **Every kind's CPU vertex data is generated natively** (particles from the particle
array, strips from the segments, rain from the drop records) and compared byte for byte with the engine's dynamic buffer at its draw.

**Which kinds the alpha / late lists hold** (op tap, 13 spot visits + the savegame): Alpha/Mesh, Alpha/Animation, Alpha/Particle (10 - 37
ops per view), Alpha/Strip (always 0 draws, below), Late/Precipitation, Late/Skydome (the empty RenderAlpha). No LightStreaks and no
blended Billboard op occurred anywhere. After N5.6 every op kind of the main view's lists is composed; light streaks and blended
billboards are composed by nobody (never met).

**Particles [C, checked]** (`eCParticleRenderContext` slot 0x5d3360 -> `eCParticle_PS::Render` 0x5e0080 -> `RenderVertices` 0x5d56a0):
* Render: an instanced emitter (`PS +0x29d`) draws its master's vertices (`eCParticleModule::GetMasterInstance`); with no live particle
  (`PS +0x390 == 0`) the emitter is simulated inside the slot (vtable +0x248) first. Both are left to the engine (counted): instanced
  emitters never occurred; the "no live particle" ops never drew in any run (counted per op: 0 of ~23 k).
* `UpdateVertices` 0x5d58b0, once per timer tick (`eCTimer +0x50` vs `PS +0x3b0`; the tick advanced every frame in every run, so the
  "already updated" path that reads the engine's bytes back was never taken): `DepthSortParticles` 0x5d8680 (axis = column 2 of the
  application camera's matrix +0x10, times the entity matrix for entity-relative emitters; the engine's qsort 0x4a4aa0 with the
  comparator 0x5d8780 = `((ay y + ax x) + az z) + aw`, far first), `CreateVertices` (VB `capacity +0x394 x 0xc0` once, static IB),
  lock DISCARD, per particle (0xd0 bytes) unless `+0xb8 > +0xbc`, `+0xc8 & 1`, or FUN_1801accf0 (alpha `+0x90` / `+0xb0` <= 0, size
  `+0x60` or tint scale `+0x6c` zero): the basis by orientation `PS +0x170` (0 camera rows of `ms_pActiveCamera +0x20` inverted; 1
  velocity `+0x18 x +0x24` crossed with the look row; 2 / 4 a fixed axis `+0x178` / the velocity with R = (-L.z, 0, L.x) or (0, -L.z,
  L.y) when L equals GetUp; 3 the negated axis), `+0x198` axis in entity space, entity-relative (`PS +0xb0 == 1`) through the inverted
  entity matrix without translation, the half sizes `((s.y +0x70) +0x418)` / `((s.x +0x6c) +0x418)` with pivots `+0x184 / +0x188`,
  three rotations (`+0x48` about R, `+0x4c` about U, `+0x50` about L) applied to corners, normals and L, the facing flip of L
  (`((Lw.y n.y + Lw.x n.x) + Lw.z n.z) < 0`), normal modes `+0x2b0` (0 / 1 all = L, 2 corner + L, 3 position - emitter), pinned corners
  2 / 3 (`+0x200`), sprite frames (tiles `+0x228 + 1` x `+0x22c + 1`, `+0x23b` row-major, `+0x239` blending two frames, the first frame's
  u0 / v0 plus a full texel `1 / image +0x58 / +0x5c`), COLOR0 = (age ratio, |dot|, max(dot, 0), 1 - frame fraction) unclamped,
  COLOR1 = colour x tint clamped (SharedBase's `bCFloatAlphaColor::Clamp`, `bCByteAlphaColor`, `GetColor_ARGB_8888`); the primitive
  count 2 x quads, `return primitives != 0`. Re-done natively with the same SharedBase vector / matrix functions the engine calls
  (`GetScaled`, `SetCrossProduct`, `Normalize`, `TransformDirection`, `bCMatrix3::SetToRotation`, ...; 22 bound by decorated name) and
  the inline float arithmetic in the disassembly's order: **bit-equal in every compared draw**.
* `ApplyRenderStates` 0x5d6e50: shader = material `+0x450` `+0x70` when it is an `eCShaderParticle` (vtable 0x9b6d38); the particle
  material's samplers (`+0x448`: `eCParticleMaterialSimple` 0x9b7160 / `BlendSubdivisions` 0x9b7170, SetImage -> `SetImageResource`
  of `+0x10` (+ `+0x18`)) get the image `+0x440`; BlendMode from `+0x220` (1 -> 2, 2 -> 3, 3 -> 5, 4 -> 4, 5 -> 6, 6 -> 7, 7 -> 8, else
  AlphaBlend while the fade as a double is < 0.9: 0.9f itself is below it), MaskReference `+0x299 ? +0x29a : 0`, EnableFog
  (shader `+0x2b0`) = `!PS +0x298`, the visual solid / no culling / depth test `+0x29b ? LessEqual : Always` / write `+0x29c`,
  SetGlobalAlpha(fade) = particle pool c34.w, SetWorldMatrix = c31..c33 (identity for world emitters, the entity's matrix for
  entity-relative ones); for blend modes other than 1 `eCDX9Device::SetMasterFogging(1)` (FOGENABLE 0).
* `eCShaderParticle` [C, checked]: eCShaderBase's SetSwitch / ExecutePass / OnGetPasses / ExecuteShaderConstants, HasOpacity and
  IsRefracted false, its own global pool `ms_pGlobalShaderConstants` (decoded from SetGlobalAlpha 0x6d5fc2: c31..c60), VS pool c57..c76,
  PS c6..c13, samplers from s9; one version object (v 2 only, vtable 0x9d99f0): GetPasses 0x6d6d50 = one pass, key = EnableFog (+ with an
  illumination: ambient 2, sun 4, lights << 4), Execute 0x6d6c30 = VS element `+0x1b0 + 16 ((dir + 2 ambient) 5 + lights)`, PS `+0x2f0
  + 16 fog`, the light constants 0x6d7020 (nothing without illumination), the shared state function 0x6d9740 (= the sky dome's,
  `sc::skyDomeStates` with GetGlobalAlpha). Lit emitters (`+0x2b0 != 0`: RenderPasses passes the entity's illumination) never occurred
  (the light constants are not composed: left to the engine if met).
* Draw tag: eCParticle_PS::Render's (TagScope Particle, the property set, the executing op, entity `PS +0x10`), set on our draws.

**Strips [C]** (`eCStripRenderContext` slot 0x701120 = `UpdateVertices` 0x6fd960 + `RenderVertices` 0x6fd800 when `PS +0xe8 >= 2`
segments; not through `eCStrip_PS::Render`, so the draws keep the slot's tag): 2 vertices (0x20) per segment (0x24: positions `+0` /
`+0xc`, uv steps `+0x18 / PS +0x108`, `+0x1c / PS +0x10c` accumulated after use, alpha `+0x20`), colours lerped from `PS +0x20 / +0x38`
to `+0x40 / +0x58` through SharedBase's bCFloatAlphaColor functions; alpha = fade x clamp(age `+0x134` / fade-in `+0xa8`) unless `+0xa0`;
world identity, `GetPasses(shader, PS +0xa4, null, 0, false, 0)`, a **non-indexed** triangle strip of 2 n - 2 primitives (the draw path
now supports non-indexed draws). Composed natively, **never drawn by the engine** at any sampled place: every strip with >= 2 segments
had a material without a shader (`+0x120 +0x70` null; the tap's "nothing to draw"), so the generator is unverified in game.

**Light streaks [C]** (`eCLightStreaks_PS::Render` 0x5c4ae0: static VB / IB from CreateBuffers, `eCShaderLightStreaks` 0x9acd68 with
its own version 0x9d96f8: a directional pass + one per point light, global pool 0xc62980, time c38.y): never met; left to the engine.

**Precipitation [C, checked]** (slot 0x719b00 = `eCPrecipitation_PS::RenderAlpha` 0x71d1a0 inlined): when `PS +0x18 & 1`, the embedded
object at `PS +0x108` is an **`eCSkyObject`** (vtable 0x989f28, created with dynamic 1, LessEqual, write off) rendered with the identity
world: the sky objects' Render / FillVertexBuffer of N5.2c (corrects D 10's "dynamic primitive (1, 4, 1, 0)" and "every frame even when
dry": the draw needs records). Its billboard array (sky +0x10) is written by `eCPrecipitation_PS::Update` 0x71a4d0 in OnProcess, before
the frame's rendering: per live drop (0x34 at `PS +0xd8`, count `+0xe0`, dead byte `+0x1d`) in order, D = the direction `+0x34` through
the application camera's matrix normalised, k = `|D.xy| x +0x54 + 1`, Rt = (0, 0, 1) x D, V = (0, 0, 1) x Rt (normalised), record =
(position jittered by SharedBase `g_sinf` / `g_cosf` when `+0x68` and `+0x6c` > 0, size.x Rt.xy, size.y V.xy (x k while falling: timer
`+0x28 == 0`), 0xffffffff, tile uv + 1 / tiles `+0x58 / +0x5c`); the fill covers the drops counted at Update's start (spawned drops
are appended), primitive count = 2 x records. The composition re-does the records from the drops at the slot's entry (**equal to the
engine's array in 4,698 of 4,698 ops**) and expands them with `sc::skyBillboardVertices`. Shader = the code-built eCShaderDefault
(BlendMode 2, TransformationType 7, DisableLighting). Drawn only while records exist (0 draws when dry or with the clock frozen).

**0-draw ops, checked at run time**: particle ops with live particles but no quad built (UpdateVertices returns primitives != 0):
composed as 0 draws, the engine drew none (81 + 10 + 25 + 22 + 71 ops in the runs below); particle ops without a live particle at the
entry: left to the engine, it drew none of them; strips: no shader; precipitation: no records.

**Verification** (compare = every field class of sections 7 - 13 incl. the CPU vertex bytes and bool / int registers; draws equal /
compared, 0 mismatches everywhere; "left" = left to the engine: buffers not created yet (an emitter's first frame), no live particle at the
entry, a precipitation permutation not compiled at the world's first frame):

| run | particle draws | precipitation draws | other |
|---|---|---|---|
| compare, 8 spots (pub_interior, harbour_night, lava_cave, monastery_hall, beach_rain, waterfall_jungle, hero_idle, pub_idle) | 111,030 / 111,030 | 3,835 / 3,835 | strips 16,442 ops composed, 0 drawn; left 668 + 5,303 / 1 |
| compare, beach_night_storm + beach_rain (clock running at the storm) | 38,855 / 38,855 | 4,696 / 4,696 | |
| compare, savegame Risen_Save_0009 (`rvk_perfsave` hold 30) | 6,977 / 6,977 | 565 / 565 | |
| compare, `rvk_spells` (fireball / frost magic balls, shots, impacts, effects at night) | 80,565 / 80,565 | 2,830 / 2,830 | orientation 3 only here (3,261 ops) |
| compare, the storm + rain spots with the native rain records | 37,849 / 37,849 | 4,698 / 4,698 | records 4,698 / 4,698 equal |
| take-over + compare, 7 spots (incl. beach_night_storm, hero_idle), **validation on** (0 messages) | 107,044 / 107,044; own 107,044 equal | 3,470 / 3,470; own 3,470 equal | |
| take-over + compare, pub_interior + beach_night_storm + lava_cave, **sync validation** (0 messages, no SYNC- hazard) | 34,560 / 34,560 | 2,669 / 2,669 | |
| take-over + compare, every group (default mask 32767), beach_night_storm / beach_rain / pub_interior / hero_idle, **validation on** (0) | 47,930 / 47,930; own equal | 3,933 / 3,933; own equal; records 3,933 / 3,933 | all groups of sections 8 - 13 equal (e.g. trees 969,484, shadow views 300,366) |
| take-over + compare, every group, savegame, **validation on** (0) | 6,221 / 6,221; own equal | 247 / 247; records 247 / 247 | all groups equal |

Statistics (the 8-spot run): 112,970 particle ops, 1.16 M quads, orientation 0 43,251 / 1 25,304 / 2 38,491 / 3 0 / 4 0, world 44,409 /
entity-relative 62,637, frame blending 25,553, lit 0, instanced emitters 0; 10.8 M rain drops in 5,531 precipitation ops.

**Images** (`water-compare.ps1`, water off; mean |d| 8-bit / share of pixels with a channel |d| > 32; engine A + take-over
`out\water\20261028-053004`, engine B `20261028-053421`):

| spot | A vs take-over | A vs B (noise) | B vs take-over |
|---|---|---|---|
| pub_interior (frozen) | 0.71 / 0.003 % | 0.47 / 0.003 % | 0.70 / 0.001 % |
| lava_cave (frozen) | 1.96 / 0.554 % | 2.32 / 0.647 % | 0.84 / 0.017 % |
| harbour_night (frozen) | 0.83 / 0.072 % | 0.57 / 0.037 % | 0.60 / 0.058 % |
| beach_night_storm (rain, clock running) | 2.03 / 0.938 % | 1.91 / 0.701 % | 1.90 / 0.940 % |
| hero_idle (clock running) | 2.95 / 2.270 % | 2.63 / 1.570 % | 2.83 / 1.627 % |

`shot-reviewer` (x8 diff maps, crops of flames / torches / rain, `out/n56/review`): no missing, extra, stretched or mis-sorted sprite,
no blend, colour or sprite-frame difference; the candle and torch flames differ in shape between all three runs (the engine's flame
animation: A vs B differs the same way), the rain streaks match in density, angle and brightness, the storm's splash mist moves between
runs; small whole-image offsets (storm +4 %, hero_idle -1.5 levels) not attributable to particles (the next run's native-AA pair at the
storm went the other way). **Motion** (8 frames 1 apart): at the frozen spots the particles stand still (pub_interior and harbour_night
0.00 between consecutive frames in both runs: the particle simulation runs on the game time), lava_cave's first frames settle the same
way in both (1.84 -> 0.43 vs 1.86 -> 0.34), the rain moves 0.34 - 0.55 per frame in both, hero_idle 0.26 - 0.42 in both; no flicker or
popping. **Same-frame switch** (take-over on at frame 4660 at the frozen pub_interior, pictures 4600 / 4620 / 4640 / 4680 / 4700 / 4720):
all 22,400 8x8 blocks static; mean |d| 0.016 before, **0.016 across**, 0.017 after. Savegame switched at 2400 (`-MaxFps 60`): on the 3,529
blocks static in all four 20-frame same-mode pairs 0.065 before, **0.046 across**, 0.055 after, 0 blocks above 1.0.

**Consumers**: `upscale.mode=native_aa` + `upscale.reactive` (FSR's reactive mask from the scene copied at BeforeAlpha: our particle and
rain draws keep the engine's position, so the copy is taken at the same point) + `upscale.object_motion=characters`, savegame switched
at 2400: frames upscaled at the same rate before and after (~300 per 5 s), object motion 22 tracked draws and ~13 k captures per 10 s
throughout; native-AA engine vs take-over at beach_night_storm / lava_cave: 1.50 / 0.495 %, 1.38 / 0.116 % (below the non-AA engine noise
there). The draw kinds stay Particle / Precipitation (compared as the Tag class).

**Cost** (savegame, `-Exe RisenTest.exe -Vsync off -Perf`, `rvk_perfsave` hold 40, scene ops taken over, groups 4095 vs 32767, machine
idle: no process above 10 % of a core): GPU 1.50 - 1.51 vs 1.47 ms; CPU engine + HAL 6.90 - 6.93 vs 7.01 - 7.02 ms (p50 frame 7.60 - 7.70 vs
7.76 - 7.77 ms): **about +0.1 ms** for ~35 particle ops per frame (the copy + sort of the particle array and the SharedBase calls per
particle; the engine's own UpdateVertices still runs).

Engine rules newly learned [C, checked]: particle vertices are built once per timer tick inside the render slot, sorted in place by an
axis from the application camera; the frame blend's second uv set stays (0, 0, 1, 1) without blending, only u0 / v0 get the texel
offset; COLOR0 carries age, facing and frame fraction for the shader, COLOR1 the clamped colour; the 0.9 fade threshold is a double
compare; particles use eCShaderParticle with one version (v 2), its own c31..c60 pool and the sky dome's state function, and switch the
device's fog off for every blend mode but Masked; emitters with no live particle simulate inside the render slot; the strip slot calls
UpdateVertices / RenderVertices directly (strips are untagged) and strips draw non-indexed; the rain is an eCSkyObject whose records
Update writes from the drops in OnProcess, drawn only while records exist; at frozen game time particles and rain stand still.

**Not yet / open**: strips (composed, the engine drew none: no strip with a shader met), light streaks, lit and instanced particle
emitters, blended billboard ops (alpha list) - all never met; the lightning flash's look in the take-over not judged (no flash in its bursts); as
before the engine's code still runs for every taken-over op. Main-view draws still made by the engine after N5.6: the post_SSM PSSM
point-light pass (only with lights that cast shadows), the post-processing chain (DoF default: 19 jobs), the overlay effect, the GUI;
plus ops left by rule (first frames, not-yet-compiled permutations, the kinds above that never occurred).

## 15. N5.7 result: the post chain and the sprite batcher (2026-10-28)

Groups **32768** (the post chain: every effect filter job of the image-effect graphs) and **65536** (the sprite batcher: GUI, text,
cursor, loading screens, videos) of `native.scene_ops_groups` (behind `native.scene_ops` / `native.scene_ops_compare` as in sections
8 - 14; default mask now 131071). Code: `src/native/effect_ops_post.cpp` (32768) and `src/native/effect_ops_gui.cpp` (65536), both
part of `native::EffectOps` (the N5.5c machinery of section 13: the fx pass evaluation, the device capture / apply / issue, the
field-class comparison), the pure rules in `effect_compose.*` (`colorMatrix`, `focalRangeNear` / `Far`, `clampScreenVector`, `sunFlare`,
`planSprites`, `spriteQuad`, `spriteQuadIndices`, `spriteBlend`, `spriteBytesEqual`; tests `native_post_semantic_rules`,
`native_sprite_batches_follow_render_batches`), the detours in `engine/scene_hooks.cpp` (`EffectTap::postBegin / postEnd` around
`eCImageEffect::Execute`, `spriteFlushBegin / End` around `eCSprite::Flush` 0x757d10; `inSceneJob()`), `hal::vulkan::EffectInfo` (the
effect's current values / textures / technique). Read in the decompiles `frameF_dump1..7.txt`, `imgparam_dump*.txt` and the capstone
disassembly of the test copy's Engine.dll (`n56/edis.py`). All [C] unless marked; "checked" = held in the runs below.

**Which graphs run [checked].** rvk.log `native: post: effect ... (scene|overlay): <filters>` names each `eCImageEffect` the first time
it runs (filter name, fx file : technique index, screen mode, nested effects):
* the scene effect (`eCImageFilterAdmin +0x90`) = `/Engine/Render/PostProcessing/DefaultEffect`: **DoF** (ConfigDefault / Performance
  High / Normal; the test copy) = DownSampleCoC, HDRI (nested: AvgLuminance, Bloom, ToneMap), BlurH, BlurV, CalcNearCoc, SmallBlur,
  DoF (screen), Bloom (nested): **19 effect draws per frame** (LogLuminance, Down32 / 16 / 8 / 4 / 2, AdaptiveLiminance, AvgLuminance
  Copy, BrightPassAlphaMask, GlowH, Streak, GlowVAddBloom2, ToneMap, DoFDown, DoFBlurCocH / V, DoFNearCoc, DoFSmallBlur, DoF), F 2.3's
  job list confirmed; **HDRI** (ConfigPerformanceLow; checked by editing the test copy's ConfigUser.xml) = HDRI (screen), AvgLuminance,
  Bloom, ToneMap: 14 draws (the ToneMap job runs although only HDRI draws to the screen: it is a public filter the graph pulls);
  **Bloom** (the code default when the key is missing) has **no Screen-output filter**, so Execute pulls no job at all: no world is
  rendered (`Post{}` empty, 680 fps of GUI only) - a new engine rule; the shipped configs never select it.
* the overlay effect (`+0x98`): **OverlayMovieStrips** (`overlay_color.fx` technique 5 OverlayMovieStripsAbs, screen) - **this is the
  1-draw `Post{Post(1)}` of the refcompare spots** (open item of F 7 / section 5 resolved): weight (`+0xb0`) 1.0 at every rvkcompare
  spot and during the new game's opening, 0.012 / 0.726 while fading, at the savegame's first ~160 frames; at a 16:9 screen with
  `MovieStripAspect` 16/9 the clip leaves no bar, so it draws nothing visible. Its name is not in any DLL (it comes from game data:
  effect commands / scripts) [I: the starter not identified].
* never met: OverlayBlackScreen / WhiteScreen / OverlayHit (its `eCImageFilterCopy` without input = CopyCurrentRTToImage would stay the
  engine's: rvk.log counts `copies in the post chain left to the engine`, 0 in every run), RadialBlur, Wobble, Indicator (Copy to the
  screen), EdgeSmooth; AvgLuminance / RenderScene run only nested. **Underwater**: no effect switches (rvk_swim: the DoF graph
  throughout, F 3's [C] that no underwater effect exists).

**The post filters' composition [C, checked]** (at `eCImageFilterEffect::ExecuteEffect` 0x75af70 of an effect filter while
`eCImageEffect::Execute` runs and outside the scene job `eCImageFilterScene::OnUpdateOutput`: the terrain map's Blur7H / V and the mask
chain run inside it, in the Post view before Execute's Main scope - a first build took the terrain blur for a post job):
* Parameters (OnSetParams 0x75c740: each parameter object whose technique bit `+0x50` is set, its class's Set at vtable +0x28; then
  every input's AttachImmImageData). The values start from what the API effect holds (a class that sets nothing keeps the last value);
  13 classes checked by their Set RVA: FrameTime 0x9ee500 (`eCTimer +0x2c`), Time 0x9ee580 (`+0xc / 1000`), SceneExposure 0x9ee680
  (weather `+0x158`), BloomIntensity 0x9ee400 (`+0x15c / +0x158`), BloomThreshold 0x9ee780 (`+0x160`), BloomOffset 0x9ee700 (`+0x164`),
  ColorMatrix 0x9ee600 (9 floats in 0x758980's order: f = 1 - s, t_i = E tint_i, (a, b, c) = f (0.3, 0.59, 0.11) from Engine.dll data;
  rows ((a + s) t0, b t0, c t0), (a t1, (b + s) t1, c t1), (a t2, b t2, (c + s) t2)), OverlayBlendValue 0x9ee380 (image filter admin
  `+0xb0`), Camera 0x9ee480 (kinds 0 / 1 / 3 / 4 / 6 - 11 composed; 7 SCREENSIZE = the rect +0xc4 as `float(right - left)`,
  `float(bottom - top)`; 10 FOCALRANGENEAR: n = `+0xe4` <= SharedBase `g_fEpsilon` -> 0, else n /= far, (-1 / n, -((-1 / n) n)); 11
  FOCALRANGEFAR: e = `+0xec` (far when <= s `+0xe8`), r = (e - s) / far, r >= 1 -> 0, else (1 / r, -((s / far) / r))), SkyDome 0x9ee300
  (via `eCIlluminationAdmin::GetSkydome`; none = nothing set; kind 4 SUNLIGHTSCREENVECTOR = the light `+0xf0 +0x318` through
  SharedBase `TransformDirection(view)`, x far, `bCVector::operator*=(projection)`, x / y clamped by the engine's compare order; kind 5
  SUNFLAREINTENSITY = max(-z_view, 0)^2), Image 0x9ee900, Texture 0x9ee880 and ImageSize 0x9ee280 (section 13; no suffix match = nothing
  set). The module getters the Set functions call (weather 0x275c30, image filter admin 0x95750, illumination 0x14f330: cached
  FindModule lookups) are called by RVA after their call sites were checked. VIEWPROJ / INVVIEWPROJ (kinds 2 / 5) are not used by the
  13 graphs (left to the engine if met).
* Inputs through the graph: the source filter's output by class - eCImageFilterScene 0x9eec08 (`GetOutputAt` 0x75d770: 0 = FP16 `+0x38`
  else `+0x40`, 1 = depth map `+0x48`), effect / copy filters (`+0x70` in texture mode), textures (mode `+0x38`), eCImageFilterImage
  0x9c6560 (resource `+0x30 +0x70`), and nested effects eCImageFilterImageEffect 0x9c6910 (the admin's effect map `+0x68` by the name
  `+0x30` through the engine's lookup 0x1f2410, then its Public filters' outputs in order, counts by class: scene 2, RTBase `+0x38 == 0`,
  texture / image 1).
* Target (`eCImageFilterRTBase::SetRenderTarget` 0x668370): texture mode = the output image `+0x70` with **the depth bound at Execute's
  entry** (each job's ResetRenderTarget restores it) and (0, 0, `+0x80`, `+0x84`, 0, 1); screen mode (`+0x38 == 1`, no image) = the
  target / depth / viewport bound at Execute's entry (the back buffer; with render scale the scene hook's full-rect viewport).
* `eCGfxEffect::Execute` 0x750210 skips SetPassNumber for the active, clean effect while `ms_bOptimize`: modelled; **ms_bOptimize is
  false** in the game (every Execute applies its pass).
* Persistent targets (AdaptiveLiminance / AvgLuminance Copy, the Bloom accumulator) are the engine's images; GlowVAddBloom2's blend onto
  last frame's bloom is the pass's states. The FSR scene substitute (upscaling.md 4) applies to our draws in the device as to the
  engine's (the composed texture is the engine's scene image).

**The sprite batcher [C, checked]** (`eCSprite::Flush` 0x757d10 -> RenderBatches 0x757180 -> DrawBatch 0x7570b0 -> eCSpriteMesh::Draw
0x528650; call sites checked):
* Records `+0xa8` (0x48: corners 4 x (x, y), uv rect `+0x20`, z `+0x30`, image `+0x38`, colour `+0x40`, blend `+0x44`), count `+0xb0`;
  Flush sorts with qsort_s only for flags `0xe0` (never: GUI and videos Begin(0); left to the engine if met).
* RenderBatches: bound image `+0x88` = 0, pending `+0x90` = record 0's image, blend `+0x98` = 0, wanted `+0x9c` = record 0's blend; per
  record a DrawBatch first on a blend change, a texture change, or a full buffer (`capacity - (start + count) == 0`: then start = count
  = 0); a quad = Lock((start + count) x 0x70, 0x70, DISCARD at 0 else NOOVERWRITE); a final DrawBatch. **A batch's `+0x98` starts at 0
  every flush**, so a first record with blend 0 never calls SetBlend (the state of the previous flush / Begin's SetBlend(0) holds).
* DrawBatch: SetBlend when `+0x98 != +0x9c` (alpha blending slot 0x1e0 + `eCGfxMixerAdmin::SetMasterBlending(src, dst)` = the HAL's
  master blend: 0 SRCALPHA / INVSRCALPHA, 0x1000 ONE / ONE, 0x2000 SRCALPHA / ONE, 0x3000 DESTCOLOR / ZERO, 0x4000 DESTCOLOR / SRCCOLOR,
  0x5000 ONE / ZERO with blending off); on a texture change DeAttach + AttachImmImageData(`+0xf8`, image) + eCGfxEffect::Execute (the
  pass); the mesh draw when count != 0: indexed list, base 0, min vertex 4 start, 4 count vertices, start index 6 start, 2 count
  primitives, then start += count.
* The sprite effect (inline HLSL, D3DXCreateEffectCompiler): **its technique handle `+0x20` is null** (Create never sets it), the HAL
  rejects SetTechnique(null) as D3DX does, the technique stays the first; `g_mWorldViewProj` = SharedBase `bCMatrix::operator*(sprite
  +0x48, device view)` (Flush 0x757e15; the view = slot 0x290 GetViewMatrix = the GUI's scale / offset from BeginFrame).
* Vertices: (p0, u0 v0), (p1, u1 v0), (p3, u1 v1), (p2, u0 v1), stride 0x1c; **the declaration is FLOAT3 position** (CreateDelcaration
  0x528cf0: FLOAT3 @0, D3DCOLOR @0x10, FLOAT2 @0x14), so bytes 0xc..0xf of each vertex are never read - RenderBatches leaves stack data
  there (the xmm lane of a stack temporary); those 4 bytes are excluded from the byte comparison (counted: `w lanes differing`, every
  vertex) and written as 1.0 by us. Indices 4q + (0, 2, 3, 0, 1, 2).
* Composed at Flush's entry: the batches (`fxc::planSprites`), each DrawBatch's state on the device state at the entry, the vertex /
  index bytes; compared per draw over every field class + the bytes of the buffers the draw reads; take-over issues them from our own
  dynamic vertex buffer (the engine's offsets, DISCARD of the range) and our own static index buffer (generated), and drops the engine's.

**Verification** (draws equal / compared; post = the 19 + 1 effect draws per frame, all field classes of section 13: 210 render
states, sampled stages + 13 sampler states, every register the shaders read, bool / int, target, depth, viewport; GUI the same + vertex
/ index bytes; 0 mismatches in every row; "own" = our draws re-captured and compared with their composition):

| run | post draws (registers / stages / states) | GUI draws (flushes) | own draws, differing |
|---|---|---|---|
| compare, `rvk_menufps` (world, ESC menu, character screen, 3 quick loads) | 79,854 (0.50 M / 124 k / 16.8 M) | 84,795 (8,316) | - |
| compare, 6 spots (pub_interior, jungle, harbour_night, beach_night_storm, lava_cave, hero_idle (clock)) | 173,295 (1.07 M / 265 k / 36.4 M) | 21,593 (6,488) | - |
| compare, savegame Risen_Save_0009 (`rvk_perfsave` hold 30) | 70,368 | 16,243 (4,529) | - |
| compare, `rvk_swim` + `upscale.mode=native_aa` | 67,154 | 19,807 (4,711) | - |
| take-over + compare, `rvk_menufps`, **validation** (0 messages) | 67,978 | 77,540 (7,084) | 145,518, 0 |
| take-over + compare, 4 spots, `upscale.mode=quality` (1066x600 -> 1600x900), **validation** (only the FSR DLL's known `rw_luma_history` warning) | 120,438 | 19,309 | 139,747, 0 |
| take-over + compare, DefaultEffect HDRI, 2 spots, `-Snapshots 1500,2500` | 46,867 (14 jobs) | 20,015 | 66,882, 0 |
| take-over + compare, **every group** (131071), savegame, **validation** (0) | 51,911 | 10,726 | 62,637, 0 |
| take-over + compare, every group, pub_interior + jungle, **sync validation** (0 messages, no SYNC-) | 49,302 | 10,681 | 59,983, 0 |

GUI per screen (`rvk_menufps` take-over, 5-s windows by `engine: menu state`): world HUD ~1,000 - 2,800 draws, ESC menu 3,904, the
character screen 31,500 (its many icons / text: ~50 draws per flush), loading screens (`gui sprites (view None)`: RenderEdit, outside
DoRender) 2,637 - 3,504 per window; batch breaks counted by cause (texture >> blend, buffer-full never at 4096 quads).

**Images.** Same-frame switches (`-Commands '<N>: set native.scene_ops true'` with compare on, groups 98304; `out/n57/switch_*`,
`out/n56/switch6.py`: 40-frame pairs before / across / after on the 8x8 blocks static in all four 20-frame same-mode pairs):

| place | static blocks | before | across | after |
|---|---|---|---|---|
| pub_interior frozen (bloom, DoF, tone map) | 22,400 of 22,400 | 0.0071 | **0.0073** | 0.0076 |
| sea_horizon frozen (sky, sun) | 22,400 | 0.0000 | **0.0000** | 0.0000 |
| ESC menu (GUI only: the world is not rendered in the menu state) | 22,400 | 0.0000 | **0.0000** | 0.0000 |
| character screen over the world | 21,311 | 0.0003 | **0.0004** | 0.0004 |
| savegame, HUD (`-MaxFps 60`, 2400) | 3,634 | 0.0234 | **0.0243** | 0.0219 |

`shot-reviewer` (x8 diff maps, crops of the candles' bloom, the sky, the HUD bars / compass, the character screen's text and icons;
`out/n57/review`): no visible difference across any switch beyond the same-mode pairs; the HUD bars 0.01 in both pairs, text and
icons identical at 2x, no letterbox bar, no colour shift; the savegame's whole-image luma drift (45.1 -> 45.6) is the eye adaptation.

**Consumers**: FSR native AA and quality (frames upscaled at the same rate, the GUI at output size), the reactive mask, the menu frame
rate limit and the unpaced loading screens (`engine: menu state` changes as without the take-over: menu 60 fps, loading unpaced), the
engine snapshot (`-Snapshots 1500,2500`: two 1600x900 BMPs after the GUI, the `rvk_screenshot` self-tests PASS), the ImGui overlay
(drawn at present, after the GUI; untouched).

**Cost** (savegame, `-Exe RisenTest.exe -Vsync off -Perf`, `rvk_perfsave` hold 40, scene ops taken over, groups 32767 vs 131071, the last
four 5-s `perf:` windows; machine idle, 1 % CPU before the runs (`sysprobe`)): GPU 1.56 vs 1.55 - 1.56 ms; CPU engine + HAL 7.10 - 7.17 vs
7.38 - 7.41 ms (p50 frame 7.88 - 7.98 vs 7.34 - 8.19): **about +0.25 ms** for 20 effect draws and ~3 sprite flushes per frame (the
parameter objects / graph walk per job, the record read + quad generation per flush; the engine's code still runs).

Engine rules newly learned [C, checked]: the post filters' semantic parameters are computed by their class's Set and pushed straight
to the API effect (not stored); the depth bound at Execute's entry is every texture-mode filter's depth; the scene job's off-screen
pre-passes (terrain blur, shadow mask) run inside the Post view; DefaultEffect "Bloom" renders no world; the 1-draw overlay is
OverlayMovieStrips at weight 1 (invisible at 16:9); eCGfxEffect::ms_bOptimize is false; the sprite effect's technique handle is null;
a flush's first batch never re-sets blend mode 0; the sprite vertices' w lane is uninitialised and unread.

**Not yet / open**: the overlay effects other than OverlayMovieStrips, RadialBlur / Wobble / Indicator / EdgeSmooth and the post-chain
copy filters never ran (composed by the same rules where they are effect filters; copies stay the engine's); sorted sprite flushes
(flags 0xe0) and the Bink video frames' sprites not met (the intros are skipped in test runs); who starts OverlayMovieStrips; as before
the engine's code still runs for every taken-over unit.

**Draws still made by the engine after N5.7** (every group on): the post_SSM PSSM point-light pass (only with lights that cast
shadows, section 13); the point-light atlas's own target / clear (not draws); ops left by rule: first frames (buffers not created),
not-yet-compiled permutations (compiled by the engine at that draw), particle ops without a live particle at the entry, VIEWPROJ /
INVVIEWPROJ parameters; and the kinds never met: light streaks, strips with a shader, lit / instanced particle emitters, blended billboard
ops, the other overlay effects' copies, sorted sprite flushes, the debug list, the item / character preview view (`eCRenderSystemAdmin::
RenderEntity`, View::Preview) [I: not measured in these runs]. **Superseded by N5.8**: the PSSM pass and the atlas's setup are taken over,
the never-met kinds were met or proven unreachable, and the leftovers are counted per reason: sections 16 (result) and 17 (status of every
frame element).

## 16. N5.8 result: copy leftovers and the completeness proof (2026-10-28)

Three slices (commits 892766b N5.8a coverage, 33075eb N5.8b PSSM + atlas setup, 33fab94 N5.8c rare kinds + `rvk_rare`): a counter that
accounts for every draw the device sees, the two engine passes N5.7 left (group 1024's PSSM point-light pass, group 2048's point-light
atlas setup), and the kinds no earlier run met, forced by the new scenario `rvk_rare` or shown unreachable. All [C] unless marked;
"checked" = held in the runs below (`out/n58/*.log`).

**The coverage counter [checked].** `src/native/coverage.{h,cpp}` = `native::Coverage` (pure; test
`native_coverage_counts_by_source_and_reason`). Every draw that reaches the device (the frame tap's `d3d9::DrawTap`, start of
`prepareDraw`; the tap is on whenever any native setting is) is counted by who issued it: **ours** (a native unit's own draw),
**replaced** (an engine draw dropped for ours: not a draw of the frame) or **the engine's** with the reason it stayed; keyed view x pass
x draw kind x reason. StaticOps / EffectOps report why an op / unit was left (`leftReason`): "<group>: group off", "<group>: composed,
compare only", "<group>: <skip reason>" (permutation not compiled, buffers not created yet, an instanced emitter, ...), "no group
composes <kind> ops of the <list> list", effect units "<unit>: <skip>", and for draws outside any unit "shadow mask: outside the units",
"post chain: outside the units", "GUI: outside a sprite flush", "a draw of the view outside every op / native unit", "outside every
native unit". rvk.log: `native: coverage (5 s): N draws: ours a (x %), engine b (y %); engine draws replaced by ours r`, a `by
view/pass (ours / engine)` line and one `engine, <reason>: n (%): view/pass/kind ...` line per reason; `native: coverage (run)` at the
end; with `-LogFrame N` the same for that frame (`native: coverage (frame N)`). In every take-over run below ours == replaced in every
window (each own draw replaces exactly one engine draw).

**Coverage per run [checked]** (every group taken over: `native.static_ops=true`, `native.scene_ops=true`, groups default 131071):

| run | draws | ours | engine draws | engine reasons |
|---|---|---|---|---|
| 4 rvkcompare spots (pub_interior, jungle, harbour_night, lava_cave; actors) | 5,379,290 | 99.979 % | 1,129 | permutations not compiled (static, trees, shadow views, actors, vegetation, alpha meshes, decals, precipitation), particle buffers not created yet, sky cube buffers not created yet, 1 billboard queue "unreadable" |
| savegame Risen_Save_0009 (`rvk_perfsave` hold 30) | 2,917,901 | 99.976 % | 688 | same classes; the last windows 0 / 1 / 0 engine draws per 5 s (frame 2600: 999 of 999 ours) |
| `rvk_menufps` (world, ESC menu, character screen, 3 quick loads) | 4,106,114 | 99.981 % | 779 | same; ESC menu windows 100.000 %, character screen 99.999 % / 100.000 % (GUI 31,494 draws ours) |
| `rvk_spells` | 4,306,599 | 99.983 % | 721 | same + particle buffers not created yet 20 (new emitters' first frame) |
| rvkcompare beach_night_storm, beach_rain, hero_idle, pub_interior (actors) | 4,033,359 | 99.984 % | 634 | same |
| `rvk_pointshadow` + sync validation | 1,973,796 | 99.960 % | 790 | same |
| `rvk_rare` (every rare kind), validation | 7,360,691 | 99.990 % | 753 | same |

The engine's share is the first world window after a load (the largest 5-s counts are identical in every run: static 180, trees 129,
shadow views 126, actors 40, vegetation 28): the permutations the engine compiles on demand at that draw; after it 0 - 20 per 5 s (new
emitters' first frame, the sky cube's first face, streaming meshes).

**The leftovers and why they stay.**

| leftover | why |
|---|---|
| permutation not compiled (all op groups) | the engine compiles the shader permutation inside that very draw (the eCShaderVersion element is missing at the op's entry and created by ExecutePass); composing it ourselves would mean triggering the engine's compile (a shader pool miss writes `ShaderMaterialPool_User.smp`: hard rule 1) or composing after the engine's own code ran (the device state / pools would no longer be the op entry's). The next frame composes it. **A sound alternative identified, not built**: compose at the engine's first draw of such an op (everything exists then) and issue ours right after the dropped draw (needs a Device hook "after a dropped draw"); deferred, it changes the issue model of every group |
| buffers not created yet (particles, sky cube / dome first face, strip first frame) | the emitter's VB is created by UpdateVertices inside the slot; the composition takes the engine's buffer / visual for the draw it compares, created inside the very slot |
| no live particle at the entry (`Simulated`) | the emitter is simulated inside the slot (vtable +0x248) before its draw; re-doing it would disturb the engine's random generator state; in every run the engine drew none of those ops (counted per op: 0 drew) |
| instanced emitter before its master exists | 36 ops in `rvk_rare`: GetMasterInstance creates the master inside the slot |
| billboard queue "unreadable" | 1 per run: one queue read of one frame (as in section 9), not investigated |
| copies, not draws | the post chain's eCImageFilterCopy without input (OverlayRadialBlur's copy, CopyCurrentRTToImage) stays the engine's HAL copy (`copies in the post chain left to the engine`: 15 / 32 per 5 s while RadialBlur ran) |

**The PSSM point-light pass (group 1024) [C, checked].** `eCImageFilterShadowMask::OnUpdateOutput` 0x668560, after its ExecuteEffect
units: if the mask's packed maps exist (`+0x128`), SetTechinque("PSSM", with "_Low" without HighQuality `+0x114`) at 0x669341 (string
0x8f9074); ViewProj = the app camera's P (`+0x50`) `*=` V (`+0x10`); TexViewProj = identity with _11 0.5, _22 -0.5, _33 1, _41 / _42 =
0.5 / w + 0.5, 0.5 / h + 0.5 (the viewport GetViewport returns = the mask's target), _43 0, `*=` ViewProj. Per packed map (0xb0 each, count
`+0x130`) with cells (`+0x68`, 0x120 each, count `+0x70`): DizzerKernelSize = 2 (0x9f16f8) / size (`+0xc`), ShadowTexture = map `+0x18`,
PCFFilterOffset = (-0.5, -0.5, 0.5, 0.5) / size, PCFFilterScale = (size, size).
* `UpdatePackedShadowMapVisual` 0x669e00 (<= 48 cells; the visual `+0x138`: VB 240 x 0x50 dynamic, declaration POSITION f3, COLOR0,
  TEXCOORD0..3 f4; one DISCARD lock): per cell 5 vertices = the NDC far corners (-1,1,1) (1,1,1) (1,-1,1) (-1,-1,1) (Engine.dll data
  0x9f1bc0 / 0x9f1d30 / 0x9f1c20) through `bCVector::GetTransformed` of the inverted cell `+0x90`, the apex = GetInverted(cell `+0x10`)
  row 3 (the light), COLOR0 = `~`cell `+0x11c`, TEXCOORD0..3 = cell `+0xd0` `*=` the depth-to-world matrix (inv(V) inv(diag(P11, P22,
  1)) scale(far)). Static IB 18 x u32: (0,1,3) (3,1,2) (4,1,0) (4,2,1) (4,3,2) (4,0,3); record: indexed list, 6 primitives, cells x 5
  vertices, base 0.
* The loop: UpdateEffect (`+0x180`), BeginEffectTechnique (`+0x198`: OnSetParams); per cell `bCFrustum::SetFrustum(cell +0x90)` +
  Intersects of the camera's near sphere (radius cam `+0xbc` / g_cosf(cam `+0xd4`), centre cam `+0x90`); per pass SetMasterStencilTesting(2,
  data) (fail / zfail / pass KEEP, func ALWAYS, ref 0, masks `~0` per cell; pass 0 INCR, pass 1 DECR into zfail when the sphere meets
  the frustum, else into pass; pass 2 ZERO everywhere with NOTEQUAL), ExecuteEffectPass, the visual's cull / depth func (2 / 4, 3 / 4, 1 / 8),
  the primitive with base vertex 5 c (reset to 0 before the loop); EndEffectTechnique. **3 passes x cells draws.**
* Composed at UpdatePackedShadowMapVisual's return (new detour; `effect_ops_pssm.cpp`; pure rules `fxc::pssmPass`,
  `fxc::applyPssmStencil`, `fxc::kPssmIndices`; test `native_pssm_pass_states_follow_the_mask_loop`); vertex / index bytes compared (field
  class Geometry); the take-over draws come from our own VB.
* Static lights with CastDynamicShadows (`Default_CrystalLight_Blue` / `_Red`, spawned by `rvk_rare`; no world layer places them, the
  placed ones are in the Testlevel world): 10 cells per mask (30 PSSM draws), all equal: static-light point shadows (ShadowMaskIndex) met
  and composed.

**The point-light atlas's setup (group 2048) [C, checked].** `eCPackedShadowMap::RenderShadowMap` 0x5a08f0 (new detour):
CreateRenderTarget (`+0x118` = eCDepthMap 0x59fd00), SetRenderTarget(face 6 = no cube face) (`+0x128` = 0x59fee0: the working colour
buffer for depth formats + the map as depth, saves the bound target, SetViewport(0, 0, size, size, `+0x50`, `+0x54`)), ClearViewPort
(`+0x120` = 0x59fe50: flags 6 for depth formats, colour `+0x28`, z 1, stencil 0) once for the whole atlas; per cell UpdateCamera
(`+0x180` = 0x5a09b0, no device call: Perspective off, ClipDepth cell `+0x114` / `+0x118` (db `+0x1694` = far), ViewMatrix cell `+0x10`,
ProjectionMatrix cell `+0x50`, Screen = (x, y, x + w, y + h) from the cell rect `+0..+0xc`), then `eCCameraBase::RenderShadowMap` 0x4f71c0
-> ExecuteShadowMap, whose ExecuteGfxCamera 0x4f6ad0 sets the viewport = the Screen rect, z 0..1; then ResetRenderTarget (`+0x130` =
0x5a00e0).
* Cell layout (AddCell 0x5a0400): `+0` x, y, w, h (ints), `+0x10` view, `+0x50` projection (border-shrunk (w - 6) / w), `+0x90` the cell
  view-projection, `+0xd0` texture matrix, `+0x110` bias, `+0x114` / `+0x118` near / far, `+0x11c` colour flags. The cells are cleared by
  `eCPackedShadowMap::ClearCells` 0x5a0730 at the end of UpdateShadowMask.
* Composed: target / viewport / clear / restore (as group 2048's other maps), per cell the camera (view, projection, clip, perspective,
  screen rect) and the cell viewport inside the scene. Compare: 7,320 / 7,320 cell cameras, 13,420 / 13,420 device calls (11 per render
  with 6 cells); take-over: the device calls made by the renderer (2,024 per 5 s), 0 mismatches. The first render of a session has no
  working buffer yet (1 left).

**PSSM verification [checked]:** compare (`rvk_pointshadow`) 21,978 / 21,978 draws equal; take-over + compare + validation (0 messages),
mid-run switch: 14,598 own draws = their composition; switch runs 2 / 3: 31,122 and 68,868 own draws equal; sync validation run
(`shadows.rt=off`): 8,370 / 8,370, 0 messages; `rvk_rare`: 102,228 / 102,228 including the static lights' cells.
Images: the light (It_Cast_Light spawned 1.2 m up) drifts during the hold and at some point a bright patch floods the foreground in every
run - it appeared BEFORE the switch in the first switch run (frame 3980 of a switch at 4000) and not in a control run up to frame 6400: a
scene event of that spot, not the take-over. Static 8x8 blocks across the switch: settled run 0.0347 vs 0.041 before / 0.092 after (1
block > 1.0); PSSM-only switch 0.069 vs 0.062 / 0.061 (11 blocks: the hero's point-light shadow moving with the light, the same drift in
the control). `shot-reviewer` (`out/n58/review_pssm`): the hero's point-light shadow present with the same shape / darkness / softness
before and after and as in the control; no doubling, inversion, blockiness, cell seams or leaks.

**The never-met kinds: triggers and results [checked].** Scenario `rvk_rare` (`-Param 'steps=streaks,crystal,fog,fire,billboards,strip,hit',
'time=22','spacing=600','shots=<prefix>'`) spawns the templates in front of the hero:

| kind | trigger | result |
|---|---|---|
| light streaks (eCLightStreaks_PS) | one template only, `Obj_Streak_Window_Curch_01` (placed in no world layer: dead content); spawned by `rvk_rare` | ops every frame; **the engine draws nothing**: the streak material's MaxShaderVersion (`+0x68`) is 0 and the shader has only a version-2 object (`+0x268`), so OnGetPasses finds no version (min(admin `+0x170` = 3, 0) and below) -> 0 passes. Composed (recipe below) and counted as 0 draws (draw count compared: 1,650 ops, 0 draws, equal) |
| strips with a shader | `VFX_EMT_Titan_Strip_01` (material Special_Weapontrails_01, eCShaderDefault) moved on a circle by `rvk_rare`; weapons' trails need a swing (65 templates carry eCStrip_PS) | 597 / 597 draws equal (the N5.6 generator verified for the first time), taken over |
| lit particle emitters (LightingStyle `+0x2b0`: 1 Flat, 2 Particle, 3 System) | `VFX_EMT_Fog_Ground_01`, `VFX_EMT_Fog_Pollon` (world: OutMain_VFX 21, Druid 11, Old Mine 9, ...) | composed: key ambient 2 / sun 4 / min(4, lights) << 4 from the entity's illumination (`+0xf8 +0x78`; L `+0` = P), light constants 0x6d7020 into the particle global pool: c35..c41 = P `+0xb8` rows x `bCVector4::Scale(L +8)`, c42..c44 = P `+4` (sun only), c45 + 4 i = light i (L `+0x10` + 64 i for i < L `+0xd`, else P `+0x38` + 64 (i - L `+0xd`)); the normals by style were already in N5.6's generator; 3,052 lit ops, all equal |
| instanced particle emitters (`+0x29d`) | `VFX_EMT_Magic_FireInferno_Burn_02` / `_03`, `VFX_EMT_Magic_Inferno_PerSmoke` | composed: the master found without creating one (GetCreator key: eCDynamicEntity 0x21cd10 = the resolved proxy `+0x1d0`'s entity; module cache 0xc6e5d8 / flag 0xc6e5d0; bucket ((u32) key >> 4) % [`+0x60`] of [`+0x58`], node {key `+0`, master `+8`, next `+0x30`}); the instance's byte `+0x2b`, world (space 0: translation -(instance entity `+0x90`); space 1: the master entity's `+0x60` with the instance position), illumination and tag; 5,332 instanced ops equal, 36 left (master created inside the slot) |
| blended billboard ops (alpha list) | `Obj_LS_Crystal_AntiMagic` (BlendMode 7; Monastery / waterfall / volcano dungeons), `Decal_Glow`, `VFX_BB_Ice_01` | composed (group 16, renamed `scene ops [billboards]`): RenderAlpha 0x4f4c70 -> RenderBillboard 0x4f0c00 alpha path: bb = PS `+0xc8`, light set [[entity `+0xf8`] `+0x180`], blend PS `+0x88`, switch PS `+0x90`, fade = op `+0xc`; one billboard per draw, both buffers DISCARD at 0 for exactly its quads, record (0, 0, 4q, 0, 2q), SetGlobalAlpha(fade), a material without a shader falls to the default shader (image bb `+8`, BlendMode); 4,703 / 4,703 equal. First build: 2,334 texture mismatches (the composition's mirror reset after writing the default shader's sampler slot): fixed |
| static point lights with CastDynamicShadows | `Default_CrystalLight_Blue` / `_Red` | the atlas renders 10 cells per frame, PSSM 30 draws per mask, all composed and equal (above) |
| OverlayHit | Script_Game `DoDamage` 0xd8060: victim = player and damage > GetTotal(victim, 0) / 4 (Script.dll EffectSystem::StartImageEffect) | not started in `rvk_rare` (DoDamage with an NPC attacker returned true but no OverlayHit graph ran) [I: the condition's damage / HP terms not met]; instead **OverlayRadialBlur** ran on the hits (`overlay_color.fx` technique 2 + a copy filter): 47 / 48 effect draws equal, taken over; its copy (CopyCurrentRTToImage, not a draw) stays the engine's |
| OverlayBlackScreen / WhiteScreen | camera types' `OverlayImageEffect` property (Game.dll 0x7b4270 on camera switches; gCCameraAdmin::UpdateOverlayImageEffect 0x7abcb0) | not triggered (no camera-type switch scripted) |
| RadialBlur / Wobble / EdgeSmooth / Indicator | effect data (gCEffectCommandStartImageEffect 0x828550) / F4 debug key (Indicator: never press F4) | RadialBlur met (above); the others not |
| sorted sprite flushes (flags 0xe0) | `eCSprite::Begin` 0x757ff0 is called with 0 by every caller (eCGuiDevice2::BeginFrame, PlayVideo); Game.dll does not import Begin | **dead code in the shipped game**: cannot occur |
| item / character preview (`eCRenderSystemAdmin::RenderEntity`) | only caller `eCCameraBase::RenderEntity` 0x4f7150, imported by no DLL | **editor / tool code**: the inventory and character screens use the main view; cannot occur |
| debug list (`+0x1560`) | `eCRenderContextDebug::RequestRenderOps` 0x67f4f0 on request mask 0x100 = db caps `+0x15a4`, from eSRenderOptions (config `/Engine/Render/RenderDebug`, false in the shipped configs) | not triggered (needs a config change + restart); the debug ops would be "no group composes" |
| light streak passes | n/a | recipe composed (passes: directional when P `+0` != 0, a point pass per P `+0x34` and per L `+0xd` light; VS `+0x1b0` + 16 ((f >> 3 & 3) + 2 (f & 7)), PS `+0x230`; c31..c33 world, c34..c36 directional P `+4`, c37..c40 point; states 0x6d9740 alpha 1; static buffers drawn from the engine's) but **never exercised** (no version) |

**Same-frame switches of the rare kinds [checked]** (`out/n58/rare_switch`, `rare_switch2`; static 8x8 blocks, 40-frame pairs before /
across / after): lit fog step (on at 3800) 0.0007 / 0.0011 / 0.0009 (22,387 static blocks, 0 > 1.0); fog step (off at 4300) 0.0009 /
0.0009 / 0.0008; instanced fire step (on at 4900) 0.0010 / 0.0013 / 0.0014 (0 > 1.0); billboards step (off at 5800) 0.0002 / 0.0002 /
0.0001; moving strip step (on at 6800) 0.0112 / 0.0315 / 0.0187 (3 blocks > 1.0: the strip moves every tick). `shot-reviewer`
(`out/n58/review_rare`, `review_rare_ab`): the billboards (the crystal's translucency / glow / facets) and the strip (colour, fade,
occlusion by the crystal and bush) are the same across the switch and engine vs take-over; the fog / pollen / fire sprites are too small /
dark in the spawned framing to judge by eye (their draws are field-equal including vertex bytes).

**Validation [checked].** Validation: `rvk_pointshadow` switch run 0 messages, `rvk_rare` take-over runs x3 0 each. Sync validation
without RT shadows: `rvk_pointshadow`, every group taken over + compare: 0 messages, no SYNC- hazard.

**Cost** (savegame Risen_Save_0009, `-Exe RisenTest.exe -Vsync off -Perf`, `rvk_perfsave` hold 40, the last four 5-s `perf:` windows;
machine idle before (`sysprobe`: ~6 % of 16 cores, no game / ffmpeg); `out/n58/perf_off_rvk.log`, `perf_on_rvk.log`):

| | GPU | CPU engine + HAL | p50 frame | draws per frame | ring per frame |
|---|---|---|---|---|---|
| native off | 1.50 ms | 3.28 - 3.35 ms | 3.63 - 3.71 ms (~270 fps) | 908 | 7.8 MiB |
| every group taken over (coverage counting included) | 1.52 ms | 8.60 - 8.74 ms | 9.39 - 9.48 ms (~106 fps) | 916 | 35 - 39 MiB (our own CPU-geometry / instance buffers) |

So the whole copy costs about **+5.4 ms CPU per frame** at the savegame (the engine's code still runs for every unit, plus the
composition, the state save / restore and a second set of setter calls); GPU unchanged. N5.7's last measurement (another session, groups
131071) was 7.38 - 7.41 ms: this run is ~1.2 ms higher [I: not a same-session pair; part may be the coverage counter (one hash-map
increment per draw and per dropped draw, ~1,800 per frame) and the reason strings; not separated].

Engine rules newly learned [C]:
* The PSSM pass is a hand-driven effect technique: OnUpdateOutput sets its parameters, UpdatePackedShadowMapVisual builds the pyramids
  with one DISCARD lock, and the loop drives stencil (master), passes and the visual itself; the camera's near sphere decides zfail vs pass
  counting per cell.
* The point-light atlas sets one target / clear for all cells; each cell's viewport comes from its camera's Screen rect through
  ExecuteGfxCamera.
* Static crystal lights with CastDynamicShadows render 10 cells (two lights) per frame; their PSSM colour flags come from ShadowMaskIndex.
* The vanilla light streaks never draw (MaxShaderVersion 0, only a version-2 object); the only streak template is placed nowhere.
* Lit particle emitters write the sky SH (scaled by the illumination's intensity), the sun and up to 4 lights into the particle global
  pool in the version's Execute; the style only changes the CPU normals.
* Instanced emitters share one hidden master per creator template (found by a hash map keyed by GetCreator), drawn per instance with the
  instance's world / illumination; GetCreator of plain entities is null (no instancing).
* Alpha billboards are drawn one per draw with SetGlobalAlpha(fade); a material without a shader falls back to the admin's default shader.
* Sorted sprite flushes and `eCRenderSystemAdmin::RenderEntity` are unreachable in the shipped game.
* Damage on the hero started OverlayRadialBlur (an overlay graph with a copy filter), not OverlayHit, in the scripted hits.

**Not yet / open**: the "after a dropped draw" issue model for permutations the engine compiles inside the draw (the first world window
after a load keeps ~0.02 % of draws); particle ops without a live particle at the entry and instanced emitters before their master exists
(by rule); OverlayHit's trigger, OverlayBlackScreen / WhiteScreen, Wobble, EdgeSmooth, Indicator never ran; light streaks composed but
never exercised (the engine draws none); the debug list; the 1 "unreadable" billboard queue per run; the CPU cost (+5.4 ms at the
savegame: the engine's code still runs for every unit; removing it is the "then" row of section 4).

## 17. The copy: status

Every element of the engine's frame, who draws it with every group on (`native.static_ops` + `native.scene_ops`, groups 131071), its
`native.scene_ops_groups` bit and where it is verified. "us" = our own draw replaces the engine's (after the first frame of a unit that
needs buffers / a compiled permutation: section 16's leftovers); "the engine" = stays the engine's; "never met" = composed or not, no
run produced it; "unreachable" = cannot occur in the shipped game.

**Player mode (2026-10-28)**: this copy is the render mode **Full** of the player setting `renderer.mode` (Original = the system
d3d9, Wrapper = the D3D9 model on Vulkan as before the native frame, Full = every group on; default Wrapper while Full costs about
+5.4 ms CPU; overlay.md 2.7). Wrapper <-> Full switch at a frame boundary (A/B F10 in debug mode); the `native.*` settings of this
table stay Debug overrides (a stage in Wrapper, `native.scene_ops_groups` masks Full's groups, compare on top of either). Test runs:
`run.ps1 -Mode full` (locks the mode) or `-Mode wrapper -Commands '2000: set renderer.mode full'`.

| frame element | drawn by | group | verification |
|---|---|---|---|
| z pre-pass static meshes | us | `native.static_ops` | section 7; every spot + savegame equal to the engine, section 16 coverage 99.96 - 99.99 % |
| solid static meshes, terrain layers, instanced | us | `native.static_ops` | section 7 |
| alpha + refracted meshes, z-fill, overlay passes | us (overlay passes: group 2) | 1 (overlay passes 2) | section 8 |
| decals | us | 4 | section 8 |
| sky dome | us | 8 | section 9 |
| sky cube faces | us (first face of a session: the engine's, buffers not created yet) | 32 | section 9; section 16 leftovers |
| opaque billboard queue | us (1 "unreadable" queue read per run: the engine's) | 16 | section 9 |
| blended billboard ops (alpha list) | us | 16 (renamed `scene ops [billboards]`) | section 16: 4,703 / 4,703 equal; switch 0.0002 / 0.0002 / 0.0001 |
| skinned actors | us | 64 | section 10 |
| SpeedTrees + tree billboards | us | 128 | section 11 |
| vegetation | us | 256 | section 11 |
| sun cascades / terrain map / point-light atlas z ops | us | 512 | section 12 |
| shadow views' setup: cascades, terrain map, point-light atlas | us (target / viewport / clear / reset; the atlas per cell camera) | 2048 | section 13; atlas section 16: 7,320 / 7,320 cell cameras, 13,420 / 13,420 device calls |
| shadow mask chain: SSAO, SSM / DSSM, VSSM, blur, depth copy | us | 1024 | section 13 |
| PSSM point-light pass | us | 1024 | section 16: 21,978 / 21,978 draws equal; 10 cells per mask for the static crystal lights |
| terrain map blur (Blur7H / V) | us | 1024 | section 13 |
| particles: plain | us | 4096 | section 14 |
| particles: lit (LightingStyle 1 / 2 / 3) | us | 4096 | section 16: 3,052 lit ops equal |
| particles: instanced | us (36 ops with the master created inside the slot: the engine's) | 4096 | section 16: 5,332 ops equal |
| strips | us | 8192 | section 14 generator; section 16: with a shader 597 / 597 draws equal |
| light streaks | never exercised: the engine draws 0 (MaxShaderVersion 0); recipe composed | 8192 | section 16: 1,650 ops, 0 draws, equal |
| precipitation | us | 16384 | section 14 |
| post chain: DoF / HDRI | us | 32768 | section 15: 19 / 14 draws per frame; sections 15 tables |
| overlay effects: OverlayMovieStrips | us | 32768 | section 15 |
| overlay effects: OverlayRadialBlur | us (its copy: the engine's) | 32768 | section 16: 47 / 48 effect draws equal |
| overlay effects: OverlayHit / BlackScreen / WhiteScreen / Wobble / EdgeSmooth | never met | 32768 | section 16 (triggers listed) |
| post-chain copy filters (no input) | the engine (HAL copies, not draws) | - | section 16: counted `copies in the post chain left to the engine` |
| GUI sprite batcher | us | 65536 | section 15; character screen 31,494 draws ours, section 16 |
| loading screens (view None sprites) | us | 65536 | section 15 |
| sorted sprite flushes (flags 0xe0) | unreachable (dead code) | - | section 16 |
| item / character preview (RenderEntity) | unreachable (editor / tool code) | - | section 16 |
| debug list | never met (needs `/Engine/Render/RenderDebug` + restart) | - | section 16 |

## 18. Full vs Wrapper brightness (2026-10-29, investigation: no renderer difference)

Report to check: at `pub_interior` with `upscale.mode=native_aa / quality` the Full capture was darker than Wrapper (floor -41 %, left
wall -26 %, mean luma 26.6 vs 28.0). **Not reproduced; no renderer difference found, no code change.** Method: `rvkcompare` bursts
(mean luma of the median burst frame; tools `out/fvw/*`, git-ignored), Wrapper vs Full vs Original (system D3D9), same settings, bursts
of one capture agree to 0.03 luma, so everything below is a run-to-run state difference of the engine, not frame noise.

* **In one run the modes are identical.** `-Commands 'set renderer.mode full' / 'wrapper'` alternating every 500 frames inside a
  spot hold (back-buffer shots between the switches; monastery_hall, pub_interior with player defaults + `upscale.mode=quality`, start
  mode Wrapper or Full, test baseline): the mean luma moves monotonically with the engine's slow drift (28.65 -> 28.86), the
  switches add nothing (mean |difference| of consecutive shots 0.08 - 0.13 luma, max 2.8 = the drift). Full's native compare over
  jungle + pub_interior: 0 mismatches (static ops 115,562 draws, every scene-op group equal; `lean_check=10`).
* **Across runs the pub differs by the engine's light / adaptation state, in both directions.** Pub alone (no preceding spot,
  `freeze_wait=3000`): Wrapper 28.1 / 28.1, Full 28.1 / 28.4, Original D3D9 28.2 / 28.3 (wall 102 - 106, floor 12.0 - 12.5). With a bright
  spot before it (`beach_sun;harbour_docks;pub_interior`, `beach_sun;pub_interior`, `jungle;pub_interior;monastery_hall`): Wrapper 25.3 /
  25.5 (25.5 with player defaults + quality), Full 26.8 / 26.9 (26.4), **Original 26.7**: Full = Original, Wrapper 5 % darker. The
  difference sits on the candle-lit walls (wall +8 %, floor +2 %), not a global gain. Wrapper vs the D3D9 original in this carry-over
  case is therefore the open point (not a Full regression); `jungle;pub_interior` with the short default hold: Wrapper 28.7 / 29.2,
  Full 29.3 / 29.3. In no combination was Full darker than Wrapper (player defaults, the test baseline, `upscale.mode=quality`, the
  Full switches `native.lean=false`, `native.scene_ops_groups=0`: 27.9 - 28.1 pure-pub runs, all within the state spread above).
* Not the cause (Wrapper stays at 25.1 - 26.0 after the beach with any of them): `debug.prefetch_stride=1`, `debug.shadow_stride=1`,
  `debug.record_threads=false`, `debug.batch_draws=false`. Frame rate is the Chill cap (80 fps) in all modes, so the engine's
  `FrameTime`-weighted eye adaptation sees the same time.
* The earlier side-by-sides `out/water/20261029-002714` / `-014741` are two Wrapper runs (rvk.log `renderer: mode wrapper`), the
  second with `upscale.provider=fsr4` (softer, upscaling.md 15), not Full captures.
* For A/B work at the pub: use a spot of its own (no preceding bright spot), `freeze_wait=3000`, and compare against Original;
  mean luma differences below ~6 % between separate runs are the engine's state, not the renderer.
