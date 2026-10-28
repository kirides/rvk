# Skipping the engine's render code: side effects of the animated / simulated contexts (RE, 2026-10-28)

Slice "anim" of the optimisation that stops running the engine's render code in render mode **Full** (README.md 17: the
engine's slot functions still run for every op and their draws are dropped at `prepareDraw`; the plan is to not enter the slots
at all). Covered here: skinned actors (`eCRenderContextAnimation2`), SpeedTree + tree billboards, vegetation, particles, strips,
light streaks, precipitation. For every function reached from these slots: each write or call that is **not** pure draw
preparation, who reads it later, how often it runs, whether it depends on the slot being reached, and whether our composition
(`src/native/static_ops_actors.cpp`, `static_ops_trees.cpp`, `static_ops_fx.cpp`) already reproduces it.

Sources: capstone disassembly of the test copy's `B:\DevDrive\risen-test-vk\bin\Engine.dll` (this session; helpers:
`B:\DevDrive\risen-ghidra\n56\edis.py` plus throw-away scripts for a `.pdata`-bounded function disassembler, a direct-caller scan
and a linear sweep for field writes), the decompiles `frameD_dump1..3.txt`, `skin_dump.txt`, `nativeskin_dump1/2.txt`. All
RVAs are Engine.dll. [C] = read in the code, [I] = inferred, [?] = open. Nothing here was run in the game.

Classes: **S** simulation (changes state the next tick / gameplay / another emitter reads), **C** caching / first use (lazy
creation, per-frame caches whose only reader is the render code), **K** state our composition or another consumer reads, **D**
droppable draw preparation (HAL state, constant pools, vertex fills, draws: our composition does all of it).

## 0. Summary

| kind | must keep (S / C / K) | drop (D) |
|---|---|---|
| skinned actors | palette rebuild per wrapper per frame (C, `UpdateRenderMatrices`); resource touches of the HRA resource and the primitive's material (K) | bones, morph weights / stream offsets, global alpha, GetPasses / passes / visual (D) |
| SpeedTree | material `RequestData(1)` per op (K); MaskReference = 80 on the material (K: RT alpha test, other readers of `shader +0x58`); the queue's lazy in-place sort + stamp reset (C) | world / instance / alpha slots, draw descs, frequencies, passes (D) |
| tree billboards | the billboard mesh resource touch (K) + whatever `eCMeshResource2::Render` / `RenderZPass` keep (static-mesh slice) | UploadTexCoords (D) |
| vegetation | the shared buffer's cursor `vegAdmin +0x40` (K: our composition reads it); mesh + first sub-mesh material touches per drawn batch (K) | the instance copy, AttachVertexData, frequencies, SetVegationAlpha, visual cull, passes (D) |
| particles | **in-slot OnProcess of an emitter without live particles (S)**; **GetMasterInstance creating the hidden master (S)**; `PS +0x3ac = 0` (S: the inactivity timer); **the in-place depth sort of the particle array (S)**; CreateVertices / CreateMaterial / CreateImage (C); material touch (K) | vertex fill, sampler SetImage, BlendMode / MaskReference / EnableFog writes, visual states, SetMasterFogging, lit constants, passes (D) |
| strips | CreateVertices / CreateVisual (C); material touch (K) | vertex fill, passes (D) |
| light streaks | CreateBuffers (C); material touch (K) (never draws: MaxShaderVersion 0) | everything else (D) |
| precipitation | nothing: the records are written by `Update` in OnProcess / `eCWeatherAdmin::Process` | FillVertexBuffer (its dirty flag is read only by itself), passes (D) |
| all kinds (cross-cutting, materials slice) | the shader path's own side effects: permutation compile on demand inside ExecutePass / ExecuteSpecialPass, overlay-material and sampler-image `RequestData` in OnGetPasses / UpdateSampler | - |

**Every S / C / K effect above happens only for ops that are reached** (an op in the executed list; culled objects are neither
sorted, re-simulated in the slot, touched nor palette-built). The lean path must call them **per op in Execute's order** (the
snapshot order of the frame tap), not per visible object.

The two things the composition reads today that the engine's render code produced and that **break when the slots are skipped**:
the vegetation cursor (`vegAdmin +0x40`, read at op entry) and the particle "already updated this tick" path (`PS +0x3b0 ==
eCTimer +0x50` -> reads the engine's dynamic VB back). Both need a change in the composition (sections 4, 5).

## 1. The common side effect: resource touches (`eCResource2::RequestData(res, 1)`) [C]

`eCResource2::RequestData` 0x684100 (`?RequestData@eCResource2@@QEAA?AW4eEResourceState@@W4eEResourceLoadMode@@@Z`, prologue `40 53
48 83 ec 20 48 8b d9 48 8b 49`): cache = `res +0x30` (null: nothing), state `res +0x38`: <= 0 nothing, 1 - 2 `eCResourceCache2::
QueueData(cache, res, mode)` 0x6852d0 (**starts the load**), 3 `eCResourceCache2::CacheResource(cache, res)` 0x6843e0. Several
slots inline exactly this sequence with mode 1 (HRA::Render 0x4dd6b5 / 0x4dd74c, tree billboard Render 0x6ee784 / 0x6ee794 and
RenderZPass 0x6ee92f / 0x6ee93f, strip RenderVertices 0x6fd84e / 0x6fd85b, vegetation RenderMeshInstanced): equivalent to calling
RequestData(res, 1).

`CacheResource` = an LRU touch: the resource is unlinked and put at the list head (`cache +0x18`), stamped `res +0x20 = cache
+0x28`; `cache +0x28` is the cache's frame counter (`inc` in `eCResourceCache2::Process` 0x6885c0). `FlushBytes` 0x684250 (called
by `LoadData` 0x684e90 whenever a load exceeds the limits, and by SetLimits / ApplyConfiguration) walks from the tail (`cache
+0x20`), **stops at the first resource stamped this frame**, moves referenced resources (`+0x24 > 0`) to the head and flushes
unreferenced ones (`FlushResource` 0x684340: vtable +0x160 release, state = 1).

| effect | class | readers | when | reached-only | reproduced today |
|---|---|---|---|---|---|
| QueueData of a resource in state 1 - 2 | **K (first load)** | the streaming loader; then every reader of the resource (our composition checks state 3 / shader `+0x70` and draws nothing until loaded) | every reached op until loaded | yes | no (the engine slot does it) |
| CacheResource (LRU head + frame stamp) | **K (residency)** | FlushBytes during later loads (eviction order, the "touched this frame" barrier) | every reached op | yes | no |

Without the touches: a material / mesh / HRA resource whose only requester is the render slot (state 1 - 2 after a teleport or a
cache-in) **never loads** (the op stays a 0-draw op forever: trees, actors, grass, particles invisible), and resources used only
by the render lose their per-frame barrier, so a later FlushBytes walks further and can flush unreferenced ones still used by our
draws [I: whether the render-only resources are unreferenced is not dumped; materials held by meshes are probably refcounted].
**Keep every touch, in op order** (cheap: a few pointer writes per resource).

## 2. Skinned actors (`eCRenderContextAnimation2` slots 0x4ddc10 / 0x4ddc60 / 0x4ddcb0)

Chain: slot -> `FUN_1800adda0` (material admin getter, no side effect) -> `eCAnimation_PS::Render` 0x4d6fe0 -> (wrapper, HRA, then
`UpdateRenderMatrices`) -> `eCAnimation_HardRenderActor2::Render` 0x4dd650 -> `SetupMorphTargets` 0x4dd220, `RenderPrimitive`
0x4dd0a0. RenderRefracted is eCRenderContext's empty slot.

| RVA | write / call | class | readers | frequency | reached-only | reproduced |
|---|---|---|---|---|---|---|
| 0x4d7005 `FUN_1800a1c40` | module lookup of `eCAnimation2Admin` (static cache `DAT_180c6d8e0`) | C | itself | first call | - | the composition calls it (`frameAdmin_`) |
| 0x4d6fe0 statics `DAT_180c6db4c / 44 / 3c` | guarded constants 0x200000 / 0x400000 / 0x800000 | C | itself | first call | - | not needed |
| 0x4d7117 `UpdateRenderMatrices(wrapper, GetFrameSync)` 0x4dfee0 | palette `wrapper +0x48` (realloc to bone count `actor +0xd8`, capacity `+0x54`, count `+0x50`) = X[i] (`AI +0x28`) `*=` W[i] (`AI +0x18`) with 0xa2710; stamp `+0x58` = frame id | **C** | **only HRA::Render** (`palette + 0x40 * remap[i]`). No other code reads `+0x48`: UpdateRenderMatrices has one caller (0x4d7117); `GetRenderMatrices` (0x83e40, folded `lea rax,[rcx+0x40]`) has no direct caller and no other module imports it; IntersectsRay 0x4de250, GetRandomPointsOnMesh 0x4df730, UpdateBoundary 0x4df400, DeformBodyParts 0x4df300 use the EMotionFX world matrices, not the palette. Attachments, hit tests, physics: world matrices from Process. RT skin / object motion: our composed draws' constants | once per wrapper per frame (frame id = `eCAnimation2Admin +0x4c`, copied from `eCTimer +0x50` in `eCAnimation2Admin::Process` 0x4cfdf0) | yes (the first reached op of the actor in any view) | yes: `paletteMatrix` reads the stored palette when stamped, else computes the entries it needs on copies (bit-equal) |
| 0x4dd6b5 / 0x4dd74c | touch of the HRA's resource (`HRA +0`), then (state 3) of the primitive's material (`prim +0x10`) | **K** | section 1 | every reached op whose UpdateRenderMatrices returned true | yes | **no** |
| 0x4dd798 SetBoneMatrix x n | global pool c33 + 3 i | D | the draw | per op | | yes |
| 0x4dd7e2 SetupMorphTargets | stream offsets of streams 1..3 written into the HRA visual's primitive record (`SetStreamOffset`, slot 6; they persist) | D | the HRA's next draw only | per op with targets | | yes (the composition sets the offsets in its own draw) |
| 0x4dd7f2 / 0x4dd80a | c31.xyz morph weights, c32.w global alpha | D | the draw | per op | | yes |
| 0x4dd836 RenderPrimitive | draw description fields of the HRA visual's record; GetPasses / ExecutePass / ExecuteSpecialPass / Execute | D (+ section 8) | the draw | per op | | yes |

No simulation happens in the actor slots: the animation update, visibility (`PS +0x22d` bit 2, written only by
`eCAnimation_PS::Process` 0x4da710 from the camera-frustum test in FUN_1804d17f0, and Invalidate), body-part deformation
(`DeformBodyParts`, from 0x4d9bae in the Process path), blinks (`eCBodyPart_PS::Process`) and the morph weights all run in
Process. The actor LOD `AI +0x250` is only read in HRA::Render (its writer is still unknown, README 10; not in a render function).

**Lean path.** Per reached actor op, in order: if wrapper, HRA (`res +0x98`) and `UpdateRenderMatrices` (call the engine's
0x4dfee0, `?UpdateRenderMatrices@eCWrapper_emfx3ActorInstance@@QEAA_NK@Z`, prologue `40 53 41 56 41 57 48 83 ec 50 4c 8b`, with
`GetFrameSync` = `[admin +0x4c]`) returns true: `RequestData(HRA +0, 1)`; if that resource is in state 3: `RequestData(prim
material, 1)`; then compose. UpdateRenderMatrices self-guards by the stamp, so calling it at every op costs one compare after the
first; the composition then always takes the stored palette (drop the copy path or keep it as the fallback). Alternative: keep the
native palette but cache it per (wrapper, frame id) - the engine's own function is the same cost and keeps `+0x48 / +0x58`
consistent for any op still left to the engine.

**Ordering.** UpdateRenderMatrices after `eCAnimation2Admin::Process` (frame id) and the actors' animation update (both Process,
before rendering), and before the first composition of that actor's bones in the frame: the **shadow cascades run first**, so the
first call usually happens in a cascade op (as today: README 12).

## 3. SpeedTree (`eCRenderContextSpeedTree` 0x6ee320 / 0x6ee3f0) and tree billboards

| RVA | write / call | class | readers | frequency | reached-only | reproduced |
|---|---|---|---|---|---|---|
| 0x6ee36e / 0x6ee43d | in-place `qsort` of the solid queue (`rb +0xb8`, count `+0xc0`) with SortInstances 0x6f2180 / of the z queue (`+0xd0`, `+0xd8`) with SortInstancesZPass 0x6f2120; stamp `rb +0xe0` / `+0xe4` = 0 | **C** | only the tree render functions (the queues are refilled and the stamps reset by the next RequestRenderOps of another database: `+0xe0 != db +8`) | first reached op of the resource per database execution (z queue: each shadow view and the main z pass) | yes (any of the resource's 4 type ops; also 0-draw ones) | yes, on a copy, whenever the stamp is non-zero |
| RenderBranches 0x6f1d1d, Fronds 0x6f18cd, Leafs 0x6f13e5, BranchesZPass 0x6f107b, FrondsZPass 0x6f0d25, LeafsZPass 0x6f08b8 | `RequestData(rb +0x80 + 8 t, 1)` (the type's material) when the queue is non-empty and visual / wrapper exist; **before** the shader check | **K (first load + residency)** | section 1 | per reached op | yes; the shadow fronds rule (RenderFrondsZPass: `rb +0x3c > +0x3a` -> nothing, not even RequestData) | **no** |
| same, 0x6f1dd7 etc. | **MaskReference forced: `shader +0x58 = 0x50`** between `NotifyPropertyValueChangedEnterEx` / `ExitEx` (vtable +0xb0 / +0xb8 = SharedBase `bCObjectBase`; eCShaderBase's ExitEx override 0x6d27a0 reacts only to "FallbackMaterial") whenever the shader is not an eCShaderDefault or has the right TransformationType | **K** | the shader's own constant upload (alpha reference), `ExecuteSpecialPass` (PS c5.w for masked), **our RT material** (`engine/materials.cpp` reads `+0x58` -> `rt_material.cpp` alpha test of the leaves in RT shadows / reflections), any other composition reading `+0x58` of that material | every reached op (persistent field: once forced it stays 80 until the material is reloaded) | yes | in the composition's mirror only (`maskRefOverride_`), **not** in the material |
| 0x6f1df5, 0x6f2066 / 0x6f207a, ... | SetGlobalAlpha, SetWorldMatrix, SetSpeedTreeInstanceData per slot; draw desc (`AccessDrawCallDescription`), stream 0 frequency | D | the draw | per op / batch | | yes |
| GetPasses / ExecutePass / ExecuteSpecialPass / Execute | | D (+ section 8) | | | | yes |

**Not in the render slots** (unchanged by the skip): `eCSpeedTreeAdmin::UpdateWindMatrices` 0x6efa10 (wind time accumulation,
the CSpeedWind objects, `SetSpeedTreeData` 0x6b7df0 -> c14..c29) has **one caller, `RequestRenderOps` 0x6ee700** (once per database
execution, `admin +0x68` stamp, `mask & 0x1a`) [C]; the LOD selection incl. the leaf LODs / alphas (`tree +0x48 / +0x50 / +0x58 ..
+0x64`) is `UpdateLoD` (context slot +0x30 -> 0x6e8be0 -> 0x6f8f30), called by the database's request before RequestRenderOps
(B-frustum-db.md); the queues are filled by `AddToRenderQueue` / `AddToZPassRenderQueue` in RequestRenderOps.

Stamp safety when the render no longer zeroes `+0xe0 / +0xe4`: the next request compares the stamp with its database's `db +8`;
db stamps start `123456` apart per database and increment per extract (B-frustum-db.md), so a stale non-zero stamp never equals
another request's stamp: the queue is still cleared and the ops still created [C for the request, I for "never meet" over a
session]. Our composition's "sort when the stamp is non-zero" then always sorts (correct, but a sort per op instead of per
resource).

**Tree billboards** (`eCRenderContextSpeedTreeBB::Render` 0x6ee8d0 -> 0x6ee730, RenderZPass 0x6ee8e0): touch of the billboard mesh
resource `PS +0x30` (inline RequestData, **K**), `UploadTexCoords` 0x6ec150 (c32..c36 + c37.. into the eCShaderDefault global pool:
**D**, composed), then `eCMeshResource2::Render` 0x760ca0 (BindLightmaps 0x764220 + `eCSubMesh::Render`) / `RenderZPass` 0x7603b0
twice: their side effects are the static-mesh / material slice's.

**Lean path.** Per reached tree op: (1) at the first op of a render buffer whose stamp (`+0xe0` solid, `+0xe4` z) is non-zero:
the engine's qsort 0x4a4aa0 with SortInstances / SortInstancesZPass **in place** and the stamp = 0 (exactly the engine's two
instructions; the composition then reads the live, sorted queue and its copy sort goes away); (2) the shadow-fronds early out; (3)
if the queue is non-empty and visual + wrapper exist: `RequestData(material, 1)`; (4) shader non-null and class / TT as the engine
checks: `shader +0x58 = 0x50` (with or without the two notifies: they reach only `bCObjectBase`'s default handlers [I: no
listener seen]; writing only when the byte differs saves the calls). Billboards: `RequestData(PS +0x30, 1)` + the static-mesh
slice's keep-list for its sub-meshes.

**Ordering.** The in-place sort before the first composition of any op of that render buffer in that database execution
(cascade views before the main view; the z queue is shared by all views and refilled per request). MaskReference before the RT
scene / RT material reads the material (rt_scene's observer sees our draw: write it before issuing the draw).

## 4. Vegetation (`eCRenderContextVegetation` 0x70d4b0 / 0x70d4d0 -> `eCVegetation_PS::Render` 0x707800)

| RVA | write / call | class | readers | frequency | reached-only | reproduced |
|---|---|---|---|---|---|---|
| Render 0x707800 | **cursor `vegAdmin +0x40`** += the instances of each successful lock (lock NOOVERWRITE, DISCARD at 0; only when the lock returns 1); reset to 0 when it reaches 1000 (after the full draw) | **K** | the next vegetation op's batching (wrap points) - and **our composition reads it at every op entry** (`composeVegetation`: `cursor = get(vadmin, 0x40)` -> `sc::planGrass`) | per reached op; reset per frame by `eCVegetationAdmin::Process` 0x7016b0 (also writes c12) | yes | it is read, not reproduced: **with the slots skipped it stays 0** -> different batch splits than the engine (same image, different draw stream; compare mode mismatches) |
| Render | the 0x2c-byte instance copy into the admin's dynamic VB `+0x38` | D | the engine's draw only (we draw from our own instance buffer) | per op | | yes (own buffer) |
| RenderMeshInstanced 0x707e40 | touch of the type's mesh (`type +0x58`; state 0 -> log "could not be loaded"); when it is in state 3 with a visual and the admin VB: `RequestData(first sub-mesh material, 1)` | **K** | section 1 | **per drawn batch** (never for an op with no instance) | yes | **no** |
| RenderMeshInstanced | AttachVertexData(1, admin VB) + detach, frequencies set / reset, **visual cull `+0x1c` = 2 - two-sided (not restored)**, draw desc, SetVegationAlpha (c31.xy, c32.w), GetPasses / ExecuteSpecialPass / Execute | D | the mesh's visual is used only by vegetation [I] | per batch | | yes |

**Lean path.** Keep a native cursor: 0 at the frame start (where Process resets it; or read `+0x40` once at the first vegetation
op of the frame), advanced per op exactly as `planGrass` already models it (the wrap at 1000), and **written back to `vegAdmin
+0x40` after each op** (keeps an op left to the engine consistent; one store). Per planned draw: the mesh touch, then (state 3 +
visual + VB) the material touch. Instance data from our buffer as now.

**Ordering.** Ops in Execute order (all z pre-pass vegetation ops before the solid ones; no shadow-map ops exist); the cursor
reset belongs to `eCVegetationAdmin::Process` (before rendering).

## 5. Particles (`eCParticleRenderContext` slot 0x5d3360 -> `eCParticle_PS::Render` 0x5e0080)

Render, in order [C]: (1) instanced (`PS +0x29d == 1`): `eCParticleModule::GetMasterInstance(module, PS)` 0x5d1290, null ->
return; (2) not instanced and **no live particle** (`PS +0x390 == 0`): `vtable +0x248` = `eCParticle_PS::OnProcess` 0x5d94a0;
(3) `PS +0x3ac = 0`; (4) `RenderVertices(fade, master)` 0x5d56a0: an instance with `+0x2b == 0` -> nothing; `UpdateVertices(target
= master or self)` 0x5d58b0; false (or no gfx admin) -> nothing; world matrix; `ApplyRenderStates(target)` 0x5d6e50; `RenderPasses`
0x5d5570.

| RVA | write / call | class | readers | frequency | reached-only | reproduced |
|---|---|---|---|---|---|---|
| 0x5e00ab GetMasterInstance 0x5d1290 | **creates the hidden master** when missing: clones the creator template, `MoveToWorldNode`, `SetTemporary`, `EnableRendering`, `eCEntity::Kill` on failure, registers it in the module's hash (`+0x58 / +0x60`) | **S** | the master's simulation (every instance's OnProcess runs `DoProcessV2(master)`), all instances' draws | once per creator template (the first reached instance op, unless an instance's OnProcess created it first) | yes | no: the composition finds the master without creating one; "master created inside the slot" = 36 ops left to the engine (README 16) |
| 0x5e00d0 `vtable +0x248` OnProcess | `DoProcessV2` 0x5d51a0 (guarded by `PS +0x3b4 == eCTimer +0x10`: nothing if this game-time tick was already processed): particle array realloc, ProcessTicks / ProcessFrame / TickParticles (spawns, **SharedBase `bCRange3::GetRandom`**, collisions, `+0x3ac` inactivity accounting) ; profiling `module +0x9c` | **S** | the emitter's particles (drawn this op and later), the global RNG sequence (every later RNG user), emitters spawning from other emitters (`SpawnFromOtherEmitter`) | per reached op with 0 live particles; effective only when the emitter's own OnProcess did not run this tick (out of processing range / not yet processed) | **yes**: a culled emitter is not kicked | no: such ops are "Simulated", left to the engine (README 16; in every run the engine drew none of them) |
| 0x5e00d9 | **`PS +0x3ac = 0`** | **S** | `DoProcessV2`: `+0x3ac` accumulates dt (`eCTimer +0x2c`) while the entity's view stamp (`entity +0xe4`) differs from the application camera's `+0x100`; at >= SecondsBeforeInactive (`PS +0x48`, with `+0x414 > 0`, `+0x28 == 1`) the emitter **stops simulating** | per reached op (the instance's own field for instanced emitters) | yes | no |
| UpdateVertices 0x5d58e1 | stamp `target +0x3b0 = eCTimer +0x50` (the frame counter) | C | UpdateVertices itself; **our composition** (`== timer +0x50` -> reads the engine's VB back) | once per frame per target | yes | read by the composition |
| 0x5d58ea `DepthSortParticles` 0x5d8680 | **in-place qsort of the particle array** (`+0x388`, `+0x390` entries of 0xd0) with comparator 0x5d8780; axis globals `0xc6bde0..ec` (read only by the comparator) | **S** | the simulation iterates the array in this order next tick (TickParticles' per-particle `GetRandom`: which particle gets which random value; respawn into slots), other emitters spawning from this one | once per frame per reached target (a master: at its first reached instance) | yes | on a copy only (`particleDepthCmp`), the live array stays unsorted |
| 0x5d58f2 `CreateVertices` 0x5d7210 | dynamic VB `capacity(+0x394) x 0xc0`, static IB, visual (first use) | **C** | the engine's draw; **our composition reads the visual / record / `+0x428`** (else "buffers not created yet", left) | first reached op (and capacity changes [?]) | yes | no (left to the engine) |
| UpdateVertices body | lock DISCARD, 4 vertices per particle, primitive count | D | the engine's draw | per frame | | yes (bit-equal generator) |
| ApplyRenderStates 0x5d6eb3 `CreateMaterial` 0x5dffc0 | the PS's material (also done in RequestRenderOps 0x5d33a0) | C | | idempotent | | no (`NotCreated` skip) |
| 0x5d6ecd | `RequestData(PS +0x450 material, 1)` | **K** | section 1 | per reached op whose UpdateVertices returned true | yes | **no** |
| 0x5d6f1a `CreateImage` 0x5d75c0 | `PS +0x440` = the image resource of the texture name `+0x210` (`eCResource2::GetResource` + vtable +0xf8) | **C** | the sampler, **our composition** (`image == nullptr` -> NotCreated) | first reached op | yes | no |
| 0x5d6f30 sampler SetImage, 0x5d6fff.. | the material's sampler image; `EnableFog` (`+0x2b0`), `BlendMode` (`+0x50`), `MaskReference` (`+0x58`) with notifies; visual fill / cull / depth func / depth write; `SetMasterFogging(1)` for blend != 1; SetGlobalAlpha, world c31..c33 | D (persistent material / device fields, but every reader is a particle draw of ours: the composition derives all of them from the PS: `blendOverride_`, `maskRefOverride_`, `fogOverride_`, `slotImage`) | | per op | | yes |
| RenderPasses / version Execute 0x6d6c30 / light constants 0x6d7020 | lit emitters: sky SH, sun, lights into the particle global pool c35..c60 | D | particle draws only | per op | | yes (README 16) |

Note for instanced emitters: the master is hidden (it has no op), so its sort, CreateVertices and material touch happen only
through a reached instance; its simulation runs in every processed instance's OnProcess (`DoProcessV2(master)`, 0x5d94d6).

**Lean path** (per reached particle op, in the alpha list's order): (1) instanced: call `GetMasterInstance` (0x5d1290,
`?GetMasterInstance@eCParticleModule@@QEAAPEAVeCParticle_PS@@PEBV2@@Z`, prologue `48 89 5c 24 10 48 89 6c 24 18 48 89`; module =
`eCParticleModule::GetInstance` 0x5d16c0 -> 0x1ac230), null -> done; (2) not instanced and `+0x390 == 0`: call the PS's `vtable
+0x248` (OnProcess 0x5d94a0, prologue `48 89 5c 24 08 57 48 83 ec 20 48 8b`); (3) store `PS +0x3ac = 0`; (4) instance `+0x2b == 0`
-> done; (5) target = master or self: if our own per-target stamp != `eCTimer +0x50`: call `DepthSortParticles(target)` (0x5d8680,
`?DepthSortParticles@eCParticle_PS@@IEAAXXZ`, prologue `40 53 48 83 ec 60 83 b9 b0 00 00 00`) **on the live array** and
`CreateVertices(target)` (0x5d7210, prologue `48 89 5c 24 08 48 89 6c 24 18 48 89`; false -> done, as UpdateVertices); (6) if
quads were built (and in the already-updated case as the engine: true): `CreateMaterial(target)` 0x5dffc0, `RequestData(+0x450,
1)`, `CreateImage(target)` 0x5d75c0 (when `+0x448` is set); then compose. This removes three leftovers of README 16 (Simulated,
master created in the slot, buffers not created yet).

**Composition changes this needs.** (a) Read the **live** sorted array (no second sort of a copy: qsort is not stable, a re-sort
of sorted data can permute ties [I]); (b) do **not** use `PS +0x3b0 == timer +0x50` as "the engine's VB holds this tick's quads"
any more - the engine no longer fills it. Keep our own per-target stamp + vertex cache (a master drawn by several instances in one
frame is sorted once and its vertices reused), and leave `+0x3b0` untouched: an op still left to the engine then runs its own
UpdateVertices (sort of an already sorted array + its own fill), which is correct for its draw.

**Ordering.** GetMasterInstance before anything of the master; the in-slot OnProcess before the sort and the vertex generation
(it may spawn the particles drawn by this very op); the sort before the vertex generation and before the next tick's
`DoProcessV2` (next frame's Process); everything in op order (the RNG sequence consumed by in-slot OnProcess calls and the
particle order the next ticks consume follow it). `+0x3ac = 0` any time after this frame's DoProcessV2 (it is: rendering follows
Process).

## 6. Strips, light streaks, precipitation

**Strips** (slot 0x701120: `PS +0xe8 >= 2` -> `eCStrip_PS::UpdateVertices` 0x6fd960 + `RenderVertices` 0x6fd800; the segments
are pushed by `PushSegment` 0x6fe6a0 in OnProcess, nothing is simulated in the slot):

| RVA | write / call | class | readers | reproduced |
|---|---|---|---|---|
| 0x6fd978 `CreateVertices` 0x6fde70 (-> `CreateVisual` 0x6fe170) | VB `capacity(+0xec) x 64`, FVF 0x242 declaration, the visual (first use) | C | the composition reads the visual / VB (first frame: left) | no |
| UpdateVertices body | lock DISCARD, 2 vertices per segment, the record's primitive count | D | | yes |
| 0x6fd84e / 0x6fd85b | touch of the material `PS +0x120` | K | section 1 | no |
| RenderVertices | SetGlobalAlpha, identity world, GetPasses / passes / Execute | D | | yes |

Lean: per reached strip op with >= 2 segments: `CreateVertices` (0x6fde70, `?CreateVertices@eCStrip_PS@@IEAA_NXZ`, prologue `48 89
5c 24 10 48 89 74 24 18 48 89`; idempotent once created), `RequestData(PS +0x120, 1)`, compose.

**Light streaks** (slot 0x5c1840 -> `eCLightStreaks_PS::Render` 0x5c4ae0): with a material (`+0x70`) and the entity's illumination:
`CreateBuffers` 0x5c2450 (static VB / IB from the streak polygons, first use: C), `RequestData(+0x70, 1)` (K), then shader checks,
world, passes (D). The engine draws none (MaxShaderVersion 0, README 16). Lean: the two calls (or nothing at all: the only template
is placed in no world layer).

**Precipitation** (slot 0x719b00 = `eCPrecipitation_PS::RenderAlpha` inlined: `PS +0x18 & 1` -> `eCSkyObject::Render` 0x6e25e0 of
`PS +0x108`): `FillVertexBuffer` 0x6e1a50 fills the dynamic VB when the dirty byte `+0x49` is 1 (and `+0x48` dynamic, records > 0)
and clears it - its only reader is itself (D); world / alpha / passes (D). The drops and the billboard records are written by
`eCPrecipitation_PS::Update` 0x71a4d0, called from `OnProcess` 0x71b920 and `eCWeatherAdmin::Process` 0x710730 (S, outside the
render; the ground rays are there too). `GetNumParticlesAlive` 0x71d1e0 has no caller (inlined). Lean: nothing to keep.

## 7. Cross-cutting: the shader path every slot of this slice calls (owner: the materials slice)

`eCShaderBase::GetPasses` 0x6d1ee0 / `OnGetPasses` 0x6d3170 (overlay materials of the actor flags with `RequestData`, C 1.3),
`ExecutePass` 0x6d2060 (the version's Execute: **permutation compile on demand**, sampler `UpdateSampler` with the image's
`RequestData` / cache, C 2.3 / 4), `ExecuteSpecialPass` 0x6d1cd0 (`CompileSpecialPass` on demand). Skipping the slots removes these
too: (a) the "permutation not compiled" leftover (README 16) would become **permanent** (nobody compiles it any more: the composition
skips the op every frame); (b) the textures of composed materials lose their touches / first loads. Both must be handled once for
all kinds by the materials slice's keep-list (e.g. drive the engine's compile of a missing permutation the way ExecutePass does - an
engine pool miss writes `ShaderMaterialPool_User.smp` exactly as before: hard rule 1 applies to the test copy's junction; and touch
the images the composition binds).

## 8. Ordering constraints (frame level)

1. Process phase (unchanged, before rendering): `eCAnimation2Admin::Process` (frame id), the actors' animation, particle /
   strip / precipitation OnProcess, `eCVegetationAdmin::Process` (cursor 0, c12), `eCResourceCache2::Process` (cache frame ++).
2. Each database's Request (unchanged): UpdateLoD, the tree queues, UpdateWindMatrices.
3. Per database execution, in the frame tap's op order (cascades, terrain map, atlas cells, then the main view: z, solid, debug,
   sky, refracted, alpha, late): the keep-calls of each reached op **immediately before its composition**: resource touches;
   actors' UpdateRenderMatrices; the tree queue's in-place sort at the render buffer's first op; MaskReference 80; the vegetation
   cursor; the particle sequence of section 5 (master, in-slot OnProcess, `+0x3ac`, live sort, CreateVertices, material / image).
4. The engine's own leftovers (ops still entered) see the same engine state as today: vegetation cursor written back, tree stamps
   reset by us exactly as the engine would, particle `+0x3b0` untouched.

## 9. Risks

* **Resource residency / first load (high).** Dropping the touches makes render-only resources never load after a cache-in and
  weakens the per-frame eviction barrier (section 1). Visible symptom: trees / actors / grass / particles missing after teleports
  or new areas; worst case a flushed resource still referenced by a composed draw [I]. Keep every touch.
* **Shader compile + image touches (high, materials slice).** Section 7.
* **Particle simulation equivalence (medium).** The in-slot OnProcess, the in-place sort and `+0x3ac` are simulation state:
  calling the engine's functions in op order reproduces them exactly; any reordering (e.g. composing particle ops on another thread
  or batched by emitter) changes the global RNG sequence and the particle order. Must run on the engine's thread at the op's place.
* **Instanced masters (low).** GetMasterInstance creates and kills entities: call it only where the engine would (a reached
  instance op), never speculatively.
* **Vegetation cursor (low).** Only the draw split changes if not simulated; but compare mode and any engine-left vegetation op
  rely on it.
* **MaskReference (low).** Not forcing it changes the leaves' RT alpha test (rt_material) and any non-tree reader of a tree
  material; the render-target image is unaffected (the composition overrides).
* **Stale stamps (low).** Tree queue stamps are reset by us; db stamps never collide (B-frustum-db.md) [I over long sessions].
* **Not verified in game.** Every row is [C] from the code; the lean path is unbuilt. Check after building: the frame tap's 0-draw
  counts per kind unchanged after teleports (residency), particle images vs the engine at running-clock spots (motion bursts, the
  storm), `rvk_spells` / `rvk_rare` (instanced masters, kicked emitters), the `+0x3ac` inactivity at an emitter leaving and
  re-entering the view, RT shadows of leaves (MaskReference).
