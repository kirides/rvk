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
composed draw of its position), Debug | NoAB, default off; `native.scene_ops_groups` (Debug, default 7) = bit 1 alpha / refracted mesh
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
  hack's matrices, the sky version class's element offsets and the sun's per-frame vertex data: not done in this stage.
* **Opaque billboards' queue** (`eCBillboardAdmin::RenderBillboards` 0x4f0530; ~1 draw per view at the savegame, none at the spots): queue
  entries 0x18 {billboard, light set, blend, lights}, the engine's qsort with `SortRenderQueue` 0x4f0ff0, batches of <= 1000 quads, the
  default shader `admin +0x30` with BlendMode written directly (+0x50) and `SetImageResource(admin +0x38)`, an illumination built on the
  stack `{light set +0x10, f = 1, ...}`, `GetPasses(shader, lights, &illum, 0, false, 0)`; the decompiled VB / IB write addresses are lost
  (re-read the disassembly before composing it).
* Overlay passes and additive light passes: composed, never met in game (above).
* As in N5.1 the engine's code still runs for every taken-over op / path (only its GPU work is ours).
