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
  overlay effect (if any): 1 draw                                                                                                F 2.6
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
| N5.2 alpha / refracted / fading statics, decals, billboards, sky dome | the rest of the main view's mesh-like ops | | as above + water spots |
| N5.3 skinned actors | palette + morphs from the actor (D 1), body parts | | hero_idle / pub_idle |
| N5.4 SpeedTree, billboards, vegetation | global wind, batches, the 1000-instance grass ring | | jungle / forest spots |
| N5.5 shadows | terrain VSM, cascades (engine fit E 2.2), point atlas, `post_SSM` mask | engine cascades + mask | shadow-compare |
| N5.6 particles, strips, streaks, precipitation | CPU vertex data from the engine's buffers | | rain / fire spots |
| N5.7 post + GUI | the 13 effect graphs in GLSL (DoF default), sprite batcher | effect runtime path | refcompare all spots |
| then | own modes, one at a time: mirror-driven lists (the current N3.3 code), GPU culling, merged indirect draws, the ubershader, bindless materials, RT consumers from the native frame | | A/B against the copy |

## 5. Open across slices

Request bit 0x200 (blended terrain layers in the solid list), the shader clock (game vs real time), masked materials when the z pre-pass
is off, the PSSM channel mapping, actor LOD (`ActorInstance +0x250`) writer, SpeedTree instancing mode (`admin +0x320`), which overlay
effect makes the 1-draw `Post{Post(1)}`, module `Process` order (weather before illumination?); the slices' own section 6 / 7 lists.

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
