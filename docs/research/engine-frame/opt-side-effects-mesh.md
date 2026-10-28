# Skipping the engine's slot code: side effects of the mesh-like slots (2026-10-28)

Preparation for the "own modes" step after the copy (README 4, 17): in render mode Full our code composes ~99.98 % of the draws,
but the engine's render code still runs for every op and its draws are dropped at `prepareDraw`. This file lists, for the
**main-view and shadow-view mesh-like slots**, every write / call of that code that is *not* pure draw preparation, so that the
optimisation can skip the slot and still keep the engine's state machine (streaming, caches, simulation) intact.

Slice: `eCRenderContextMesh` Render / RenderAlpha / RenderRefracted / RenderZPass (static meshes, sub-meshes, hierarchy nodes,
instanced batches, whole-mesh z merge, terrain / level-mesh layers), entity overlay passes, decals, opaque and blended billboards,
the sky dome op, sky objects (stars / sun / moon), the sky cube faces, and the material code they call (`eCShaderBase` /
`eCShaderDefault` / `eCShaderVersionDefault_3_0`). Not in this slice: skinned actors, SpeedTrees / tree billboards, vegetation,
particles / strips / precipitation, the shadow mask / post / GUI, and the code of `eCFrustumDatabase::Execute` around the lists
(BeginZPass / BeginMaterial / module callbacks / refraction copy).

Sources: headless Ghidra on the clone `B:\DevDrive\risen-ghidra\clones\EngineA` (new dumps `optmesh_dump1..3.txt`) plus the earlier
dumps (`frameC_dump1..5`, `frameD_dump1..2`, `n52c_dump1..2`, `lmbind*_dump`, `zpass2_dump`), capstone on the test copy's
Engine.dll for prologues and RIP-relative references. RVAs are Engine.dll. [C] = read in the code, [I] = inferred, [?] = open.
Nothing here was checked in a running game yet (section 9 lists what to verify).

Classes: **S** simulation / animation that must run every frame or tick; **C** caching / streaming / first use (resource requests,
compiles, buffer creation) that must still be triggered; **K** engine state our composition (or later engine code) reads: keep it or
produce it ourselves; **D** droppable draw preparation.

---

## 0. The short list (everything that is not D)

| # | what | where (RVA) | class | reader | our composition today |
|---|---|---|---|---|---|
| 1 | LRU touch / async load of **image resources** of the bound material samplers (`QueueData(res, 1)` for state 1-2, `CacheResource` for 3) | `eCShaderBase::ExecuteShaderConstants` 0x6d2f90 (slots the PS samples, on a shader change or a dirty slot) | **C** (critical) | `eCResourceCache2::FlushBytes` 0x684250 evicts from the LRU tail every resource whose stamp `res +0x20` != cache frame `cache +0x28` (`Process` 0x6885c0 increments it per frame) and that is not busy (`+0x24`); untouched images get evicted, never re-queued (nobody else requests them), then stay on the 4x4 placeholder | reads the slot images; requests nothing |
| 2 | the same for **lightmaps** (`res +0x90 / +0x98`) | `eCMeshResource2::BindLightmaps` 0x764220 (also from decals, ExecuteRenderInstances) | **C** | as 1 | reads `+0x70`, requests nothing |
| 3 | the same for **materials** of sub-meshes, decals, billboards; **overlay materials** (burned / frozen / highlighted, `RequestData(res, 1)`); the **fallback material** (`RequestData(res, 0)` = synchronous) of an invalid shader | eCSubMesh::Render 0x5c7eb0, ExecuteRenderInstances 0x7607b0, RenderDecals 0x4fc220, RenderBillboards 0x4f0530 / RenderBillboard 0x4f0c00, `eCShaderBase::OnGetPasses` 0x6d3170, `GetPasses` 0x6d1ee0 | **C** | as 1 | mesh materials are also touched at request time (`eCRenderContextMesh::RequestRenderOps` 0x5c75e0, kept): only overlay / fallback / decal / billboard materials depend on the slots |
| 4 | **on-demand compile** of a missing permutation (VS `FUN_1806cc000`, PS `FUN_1806cb8b0`, special pass vtbl +0x50 = `FUN_1806ca6c0`), incl. the pool walk that re-allocates the graph's registers and a possible `ShaderMaterialPool_User.smp` write (hard rule 1) | version Execute `FUN_1806cc610`, `ExecuteSpecialPass` 0x6d1cd0 / `FUN_1806d9b40` | **C** | the next frames' draws | detected ("permutation not compiled"): the op is left to the engine |
| 5 | the **shader-invalid flag** `shader +0x2a8 = 1` when a pass fails, and the cleared compiled-flag byte `version +0x18 + idx` when a special pass fails to compile | ExecutePass 0x6d2060, ExecuteSpecialPass | **K** (rare) | GetPasses (fallback material / error indicator) | read (`+0x2a8` skip) |
| 6 | **stream-3 enable flag** of the visual's API primitive: RenderZPass sets it **off**, an instanced batch **on**; eCSubMesh::Render never touches it | 0x7603b0, 0x7607b0 (`EnableStream(3, x)` on our HalPrimitive, `eCDX9Primitive +0x60`) | **K** | every later draw of that visual (also next frames) | composed for z / batches; solid sub-mesh draws read the record's current flag, i.e. what the engine's last z op / batch left |
| 7 | the **render-instance list** sort and its stamp: `res +0xa4 != 0` -> qsort `res +0xb0` (count `+0xb8`) in place, `+0xa4 = 0` | eCRenderContextMesh::Render 0x5c7110 | **D** (with care) | the resource's other instanced sub-mesh ops of the same frame; RequestRenderOps of the next frame only compares `+0xa4` with the db frame (stamps of different dbs never meet, B 2.8) | sorts a copy when `+0xa4 != 0`: stays equal (the engine's qsort is deterministic, same unsorted input) |
| 8 | **opaque billboard queue reset** `admin +0x28 = 0` (the queue is filled at **request** time, section 4) | `eCBillboardAdmin::RenderBillboards` 0x4f0530 end | **S/K** (must do) | the next frame's requests append to it: skipped = unbounded growth + every old entry drawn again | reads the queue |
| 9 | **sky clouds driven by the weather**: scroller direction `SetDirection(sky +0x108, sky +0xb0)`, the two cloud **Scale** nodes (`sky +0x110 / +0x118` = 1 / CloudSize1 `+0xb8`, 1 / CloudSize2 `+0xbc`, through the property notify that writes their VS pool register), the dome shader's cloud colour / thickness fields (`dome +0x300 / +0x304 / +0x308 / +0x310` = sky `+0xa0 / +0xa4 / +0xa8 / +0x90`, read by the dome's next property notify into PS c6) | `eCSkydome_PS::Render` 0x6e4730 | **S** | `eCTexCoordSrcScroller::SetTime` 0x6e0c00 (admin Process, every frame), the dome's pools, `eCShaderSkyDome::OnNotifyPropertyValueChangedExitEx` 0x6d7840 | reads the pools at the op's entry (so today it sees what the previous frame's Render left) |
| 10 | **sky first use / rebuilds**: `CreateBuffers` (dome grid, sky objects), `CreateStars` (rebuild when StarCount `+0x28` != `+0x290`, draws from the global `bCMersenneTwister`), `CreateMoon` (rebuild after a MoonSize notify cleared it), the sun's billboard array (count 1, `SetBillboardRenderCount(1)` on its record) | 0x6e5f90, 0x6e3ee0, 0x6e3c30, 0x6e1680 | **C** | our composition (it reads the objects' billboards / records) | skipped frame left to the engine |
| 11 | **Bink video textures**: binding an image data whose `+0x70` holds a video object advances the video (`BinkWait` / `BinkDoFrame` / `BinkCopyToBuffer` / `BinkNextFrame`, loop by `BinkGoto`) | `eCGfxImageData::Execute` 0x7551c0 -> `FUN_18070fd00` 0x70fd00 (from the sampler bind `FUN_1806bc340` 0x6bc340) | **S** | the texture's content | not done (never seen in a material [I]) |
| 12 | **first-use creation** of the admins' dynamic buffers and default materials (billboards `CreateBuffers` 0x4f00a0 / `CreateDefaultMaterial` 0x4efdc0, decals `CreateBuffers` 0x4fbd80 / `CreateDefaultShader` 0x4fba60) | RenderBillboard(s), RenderDecals | **C** | our composition reads their visuals / declarations | billboards: also created by `PostInitialize` / `RestoreResources` and the request path; decals: only here |
| 13 | engine **render caches** (`eCShaderBase::ms_pActiveShader` 0xc62970, `ms_pActiveShaderConstants` 0xc62978, the HLSL stage cache 0xc62950 / 0xc62958, active lighting pool 0xc62990, the lighting pool's cached light list `[0xc62968] +0x48`, `[0xc62c10] +0x48`, `eCGfxImageData::ms_pActiveImageData` 0xc62b10, `eCGfxCamera::ms_pActiveCamera` 0xc62580, pool dirty ranges) | every slot | **K** for leftover engine draws | any engine draw that follows our draws inside the same pass (ops left to the engine, section 6.3) | today the device state is saved / restored around our draws, so the caches stay true |

Everything else in the slots is **D**: draw-call descriptions in the visual's record, stream frequencies (restored by the
function itself), world / fade into the global pool, the lighting pool fill, render states (masters via the mixer), sampler
states and texture binds, pool uploads and dirty-range resets, `SetSwitch` on the shared shaders, the default shaders'
BlendMode / image / colour rewrites, the dynamic VB / IB locks and copies, the sky's camera hack (restored bit for bit), the pass
array `eCShaderBase::ms_arrShaderPasses` 0xbfcae8, and the debug gauge callbacks (`ms_pRegister*ExecuteCall`, set only by
`eCGfxGaugeAdmin::SetModus` 0x508f90, read by `GetValues`: a debug statistics overlay).

---

## 1. The material path (shared by every slot below)

| function (RVA) | writes / calls | class | reader | notes |
|---|---|---|---|---|
| `eCShaderBase::GetPasses` 0x6d1ee0 | `ms_arrShaderPasses` count `0xbfcaf8 = 0`, then `OnGetPasses`; invalid shader (`+0x2a8`) with `admin +0x331`: `eCResource2::RequestData(fallback +0x2a0, 0)` (synchronous load) and the fallback's passes; else the error indicator passes (`+0x278` version, vtbl +0x80) when `admin +0x330` | D; fallback request **C** | - | in game `+0x2a8` stays 0 unless a compile failed |
| `eCShaderBase::OnGetPasses` 0x6d3170 / `eCShaderDefault::OnGetPasses` 0x6c9d50 | version = min(admin `+0x170`, `+0x68`) falling back; version GetPasses `FUN_1806cc970` (appends 0x20-byte passes; may grow the array); overlay flags 1 / 2 / 4: `RequestData(+0x88 / +0x80 / +0x90, 1)` then the overlay shader's own OnGetPasses | D; overlay requests **C** | - | overlays: every frame while the entity has the flag |
| `eCShaderBase::ExecutePass` 0x6d2060 | `SetSwitch(pass +0x1c)` on the shader or its switch elements (`+0x248` / `+0x250`: constant switch -> its pool register; sampler switch 0x6c5310 -> slot image + dirty bit `pool +0x120`); admin `+0x310` (depth-bias VP active) and admin VS pool c0 (`FUN_180225e30`) when it changes; version Execute; on failure `+0x2a8 = 1` | D; `+0x2a8` **K** (rare) | the shader's pools (next op of the shader, our mirror) | the switch state persists in the shared shader's pools: our composition applies the op's switch on its copy (`applySwitch`), so it never depends on what the engine left |
| version Execute `FUN_1806cc610` (eCShaderVersionDefault_3_0 vtbl +0x20) | compile on demand (VS `FUN_1806cc000(version, nonInst, variant)`, PS `FUN_1806cb8b0(version, sun, lm, kind)` + lights on the stack); lighting pool `FUN_1806cd1d0`; activate VS / PS (`FUN_1806bbec0`: HLSL cache 0xc62950 / 58); `FillBlendingRenderStates` 0x6d0430 + `SetCurrentRenderStates` 0x6b7c50; `FUN_1806d9a70` (ExecuteShaderConstants + lighting pool upload, `0xc62990`) | compile **C**; rest D | - | a compile resets the shader's pool counters `+0xc1 / +0xf1 / +0x21b` and re-walks the graph (vtbl +0x140) into the pools / sampler slots |
| `eCShaderBase::ExecuteSpecialPass` 0x6d1cd0 + `FUN_1806d9b40` | admin `+0x310` / VS c0 as above; masked: admin PS c5.w = MaskReference / 256; special pass `type = admin +0x314`, `idx = 2 type + instanced`: compile on demand (vtbl +0x50, failure clears `version +0x18 + idx`), activate, `FUN_1806d9a70` | compile **C**; flag byte K; rest D | - | used by the z pre-pass and every shadow view |
| `eCShaderBase::ExecuteShaderConstants` 0x6d2f90 | global pool upload (`ms_pActiveShaderConstants`), shader VS / PS pools (full on a new active shader: resets `+0xa4 / +0xa8`, `+0xd4 / +0xd8`), **per live, dirty-or-new sampler slot: `QueueData(image, 1)` / `CacheResource(image)`**, bind `image +0x70` or `GetPlaceHolderImageData` 0x68c650 with the slot states (`FUN_1806bc340`), dirty mask `+0x218` rewritten, `ms_pActiveShader = this`, admin pools | image touches **C**; rest D | resource cache LRU (0, item 1) | the only place material images are requested / touched while drawing [C: no other caller of their RequestData in this slice] |
| `FUN_1806bc340` 0x6bc340 (sampler bind) | 7 sampler states, null -> ErrorImage (`admin +0x348`), `imageData +0x4c = stage`, `eCGfxImageData::Execute` 0x7551c0: **video advance `FUN_18070fd00(imageData +0x70)`**, gauge callback, `ms_pActiveImageData[stage]` | video **S**; rest D | the video texture | Bink is time-gated (`BinkWait`), so one call per frame per video image equals the engine's per-bind calls |
| `eCShaderMaterialAdmin::SetCurrentRenderStates` 0x6b7c50 | PS c5.w (alpha test word), masters: alpha test off, blending, src / dst, depth write, render mode, depth func, colour / alpha write | D | - | |
| lighting pool `FUN_1806cd1d0` 0x6cd1d0 (+ `FUN_1806b5bb0`) | lighting pool c14..c37 (`[0xc62968]`), its cache: light list `+0x48`, f `+0x50`, static-SH flag `+0x54` | D | - | composed natively per pass (README 7); c14.w and lights beyond the pass's count are carry-over (6.1) |
| `eCShaderDefault::SetWorldMatrix` 0x6c9e00 / `SetGlobalAlpha` 0x6c9e30 | global pool c33..c35 + 4i / c32.w + 4i, dirty range | D | - | composed natively |
| `eCGfxVisual::Execute` 0x5069a0 | render mode / cull / depth / depth write (overridden by masters), attached `eCGfxShader`, `eCAPIPrimitive::Execute` = the draw | D | - | |

Property notifies used by the slots (`vtbl +0xb0 / +0xb8` around a field write): `eCShaderDefault` / `eCShaderBase`
`OnNotifyPropertyValueChangedExitEx` (0x6c81a0 / 0x6d27a0) react only to "FallbackMaterial": the per-frame **BlendMode** rewrites
of the billboard / star / moon shaders recompile nothing (their compiled XFX_ALPHABLEND stays the first compile's) [C].

---

## 2. Static meshes

### 2.1 The slots

`eCRenderContextMesh` vtable 0x9ae070: slot 0 Render(op, db) 0x5c73e0, slot 1 RenderAlpha 0x5c73f0, slot 2 RenderRefracted
0x5c7400 are 5-byte `jmp 0x5c7110` thunks (never detour them; they are what `engine/scene_hooks` patches as vtable slots); slot 3
RenderZPass 0x5c7410.

| function | non-D work | class | lean path |
|---|---|---|---|
| `eCRenderContextMesh::Render` 0x5c7110 (solid / alpha / refracted) | instanced op (`desc >> 8 == 0xffffff`): `if res +0xa4: qsort(res +0xb0, +0xb8, 0x58, SortRenderInstances 0x75f690); res +0xa4 = 0` (once per frame per resource); then ExecuteRenderInstances; else overlay flags from entity `+0x28`, illumination `entity +0xf8 + 0x78`, `eCMeshResource2::Render` | instance sort D (7 in section 0) | sort ourselves (copy, as today) or call `eCMeshResource2::SortRenderInstances(void)` 0x760780 (exported; = the same in-place qsort) and write `+0xa4 = 0` to mirror the engine exactly; neither has another reader |
| `eCRenderContextMesh::RenderZPass` 0x5c7410 | none (forwards: world `entity +0x60`, sub-mesh or -1, node, not instanced) | D | - |
| `eCMeshResource2::RenderZPass` 0x7603b0 (main z pre-pass **and every shadow view**: cascades, terrain map, point-light atlas cells) | needs `res +0x38 == 3` and a visual; **stream 3 of the visual's primitive off** (`EnableStream(3, false)`, slot 13; `eCDX9Primitive +0x60 = 0`); draw record (type 4, indexed, ranges, merged runs); per material: SetWorldMatrix / SetGlobalAlpha(1); `ExecuteSpecialPass(false)` (compile **C**, section 1) | stream 3 **K**; compile C; image touches of the z PS's samplers (masked materials) **C** via ExecuteShaderConstants | issue `EnableStream(3, false)` on our HalPrimitive exactly when the engine's RenderZPass would have reached that point (it is set before the sub-mesh loop, even when nothing is drawn, as long as `res +0x38 == 3` and the visual exists); touch the live sampler images; fall back to the engine slot when a special-pass permutation is missing |
| `eCMeshResource2::Render` 0x760ca0 | `BindLightmaps(1)` (C + admin s4 / s5 slots), then eCSubMesh::Render per sub-mesh | C (lightmaps) | `RequestData(lightmap, 1)` for `res +0x90 / +0x98` (or call `BindLightmaps` 0x764220 itself: it is cheap and keeps the admin slots true, see 6.2) |
| `eCMeshResource2::BindLightmaps` 0x764220 / `eCShaderMaterialAdmin::SetLightmap` 0x6b80d0 | per lightmap: QueueData / CacheResource; image data missing -> combi 0, stop; else admin slot `+0x2c0 + 0x28 i` image `+0x2e0` (`FUN_1806bc540`) and, in material mode (`admin +0x311`), an immediate bind to stage 4 + i | C; admin slots K (6.2) | as above |
| `eCSubMesh::Render` 0x5c7eb0 | material `QueueData / CacheResource` (C, duplicates the request-time touch); SetGlobalAlpha(fade) / SetWorldMatrix; draw record; GetPasses / ExecutePass / Execute | material touch C (already done at request); rest D | nothing extra for meshes (the request phase touched the material this frame); overlay / sampler / compile items via section 1 |
| `eCMeshResource2::ExecuteRenderInstances` 0x7607b0 | BindLightmaps(1) (C); material touch per batch (C, dup); hardware path: **stream 3 on** (`EnableStream(3, true)`), streams 0..2 frequency `n | 0x40000000` per batch and back to 1 (D: restored), SetWorldMatrix / SetGlobalAlpha per instance; fallback path = eCSubMesh::Render per instance (never touches stream 3) | stream 3 **K**; C | `EnableStream(3, true)` on the hardware path (set once before the loop, also when the first batch flushes nothing); lightmap touches |

Hierarchy-node ops (`desc = node << 8 | i`) and blended terrain / level-mesh layers (request bit 0x200: solid ops with fade) take
the same functions with other ranges / fade: no extra side effect [C]. The whole-mesh z op (desc 0xff) is the same RenderZPass
loop. Shadow views call the same RenderZPass slot (README 12): the side effects are the same, the special pass type differs (3
cascades, 4 terrain map, 2 point-light cells: other permutations to compile).

### 2.2 Entity overlay passes

`OnGetPasses` with flags (burned 1 `+0x88`, frozen 2 `+0x80`, highlighted 4 `+0x90`): `RequestData(overlay material, 1)` **every
frame** while the flag is set (C), then the overlay shader's passes (its permutations compile on demand: C; its sampler images are
touched in its ExecuteShaderConstants: C). The overlay's pools and blend mode are its own (D). Lean: for every flagged op,
`eCResource2::RequestData(overlay, 1)` and the image touches of the overlay shader's live samplers; fall back to the engine slot
while an overlay permutation is missing.

### 2.3 The request side stays (for reference)

`eCRenderContextMesh::RequestRenderOps` 0x5c75e0 runs in the request phase and is **not** skipped: it queues / touches the mesh
resource and every sub-mesh material each frame, builds the ops, and for instanced resources frees and rebuilds the render-instance
list once per db frame (`db +8 != res +0xa4`), `AddRenderInstance` 0x7602a0, `res +0xa4 = db +8`. The mesh LOD (`UpdateLoD`
0x5c74b0) and the busy counter `res +0x24` are request-side too. So for static meshes the only render-side C work is images,
lightmaps, overlays and compiles.

---

## 3. Decals (`eCDecalAdmin::RenderDecals` 0x4fc220, a non-op path of Execute)

| work | class | notes |
|---|---|---|
| `CreateBuffers` 0x4fbd80 / `CreateDefaultShader` 0x4fba60 (first use: VB 360,000 B, IB 20,000 B, visual `+0x58`, default shader `+0x48`, its sampler `+0x50`); both early-out when done | **C** | only called here [C: callers]; our composition needs the visual's declaration |
| render queue `+0x30 / +0x38 / +0x40`: count reset to 0 at entry, rebuilt from the decal registry `+0x20 / +0x28` (decals whose entity's visible stamp `+0xe4 == db +8`), qsort `SortRenderQueue` 0x4fcf80 | D | self-contained per call; the registry is never modified here |
| per record: material `QueueData / CacheResource` | **C** | decal materials are touched nowhere else in the frame |
| default shader: `eCColorSrcSampler::SetImageResource(admin +0x50, image)` (frees / rebuilds the sampler's image array, `+0xd4 = 0`, UpdateSampler -> pool slot image + dirty bit) | D (K for its own later readers only) | our composition sets the slot image on its copy |
| `BindLightmaps(lightmap mesh, 1)` | **C** (+ admin s4 / s5) | |
| GetPasses / ExecutePass (compiles **C**, image touches **C**), SetGlobalAlpha / SetWorldMatrix, VB / IB locks, vertex transform of dynamic decals into the locked buffer | D | no write back into the decal objects [C] |

Lean path: in the existing RenderDecals detour, call `CreateBuffers` / `CreateDefaultShader` (exported, idempotent), compose and
issue our draws, `RequestData(material, 1)` per drawn decal record, `RequestData(lightmap, 1)` for lightmap meshes, the image
touches of the bound slots (including the default shader's image), and **do not call the original**. Fall back to the original
while a decal permutation is missing.

---

## 4. Billboards

**New [C]: the opaque billboard queue is filled in the request phase.** The billboard render context (vtable 0x982810) slot +0x40
(RequestRenderOps) is `FUN_1804f1400` (its callers include the vtable entry 0x982850): no material yet -> `CreateDefaultMaterial`;
a blended material or fade != 1 with request bit 4 -> an alpha op (`eCRenderOp2::Create` on db `+0x14e8`, fade at `+0xc`); else
with request bit 2 -> `eCBillboard_PS::Render` 0x4f4bf0 -> `eCBillboardAdmin::RenderBillboard(..., alpha = false, 1.0)`, which for
fade >= `DAT_1809f1684` only **appends `{billboard, light set, blend, switch}` to the admin queue `+0x18 / +0x20 / +0x28`** and
returns. So an opaque billboard never has an op; the queue is the only carrier. The context's slot 1 (RenderAlpha) is
`FUN_1804f13c0` -> `eCBillboard_PS::RenderAlpha` 0x4f4c70 -> RenderBillboard(alpha = true, op fade): the alpha op's direct draw.

| function | non-D work | class |
|---|---|---|
| `eCBillboardAdmin::RenderBillboards` 0x4f0530 (Execute, after decals) | `CreateBuffers` 0x4f00a0 / `CreateDefaultMaterial` 0x4efdc0 (first use; also called by PostInitialize / RestoreResources / the request path); qsort of the queue in place (D); per record material `QueueData / CacheResource` (C); default shader `BlendMode` rewrite with notify + `SetImageResource(admin +0x38, image)` (D); passes (compile C, image touches C); **`admin +0x28 = 0` at the end** (only when buffers and the default material exist) | queue reset **S/K**; C |
| `eCBillboardAdmin::RenderBillboard` 0x4f0c00, alpha path (the alpha ops' slot) | `CreateBuffers` / `CreateDefaultMaterial` (C); `eCResource2::RequestData(material, 1)` (C; returns without drawing while the material is not loaded); default shader BlendMode / SetImageResource (D); SetGlobalAlpha(fade), passes (compile C, image touches C); VB / IB DISCARD locks and copies (D) | C |

Lean path: opaque queue = in the RenderBillboards detour compose from the queue (as today), issue, touch materials / images, then
**write `admin +0x28 = 0`** (only if `+0x40` visual and `+0x30` default material exist, as the engine), never call the original.
Alpha ops = skip the slot after `RequestData(material, 1)` + image touches. The request-time enqueue stays the engine's (it is
not render code), so nothing has to be produced for the queue itself.

---

## 5. The sky dome op, sky objects, the sky cube faces

`FUN_1806e8a10` (sky context slot 0, `mov rcx, [rcx+0x18]; test; jne 0x6e4730; ret`: a 4-instruction tail jump, never detour it)
-> `eCSkydome_PS::Render` 0x6e4730 (nothing unless `PS +0x18 & 1`). The sky's alpha slot `FUN_1806e8a00` and
`eCSkydome_PS::RenderAlpha` 0x6e4720 are `ret` [C].

| step in `eCSkydome_PS::Render` | class | reader / why |
|---|---|---|
| `CreateBuffers` 0x6e5f90 (screen grid, sky objects; early-out when done) | **C** | our composition reads the dome visual and the sky objects |
| camera hack on `ms_pActiveCamera` (or the cube camera `+0x3c0` when `+0x3cc`): view translation zeroed, `+0x120 = Invert(view)`, `+0xe0 = P * V`, projection `_33 = 1`, `_43 = 0`, dirty `+0x160 = 1`, `eCGfxCamera::Execute` (device transform slot 0x270, `SetCamera` -> admin pools c0..c11 / PS c0 c1, `ms_pActiveCamera`, gauge callback); restore: view / projection copied back, `Invert` / `operator*` again, `Execute` | D | the restore reproduces the camera's matrices bit for bit (README 9); `+0x160` is cleared by the second Execute; later ops of the view use their own composed camera |
| `CreateStars` 0x6e3ee0: rebuilds the star billboards when StarCount `+0x28` != `+0x290` (random positions / sizes / brightness from the global `bCMersenneTwister`), DestroyBuffers / FillIndexBuffers, dirty `+0x2c1` | **C** (and the shared RNG's sequence at that moment) | our composition reads `+0x280..+0x290`; today a rebuild frame is left to the engine |
| stars: shader `+0x148` BlendMode 7 (notify), `SetImageResource(+0x160, +0x140)`, `eCColorSrcConstant::SetColor(+0x168, +0x390)` (its pool register), `eCSkyObject::Render(+0x2d0)` | D (overridden in the composition) | the shared shader ends in the moon's state; no other reader |
| sun: `SetImageResource(+0x158, +0x138)`, `SetBillboardRenderCount(1)` 0x6e1680 (the record's primitive count = 2), the one-billboard array `+0x180` (realloc on first use, DestroyBuffers / FillIndexBuffers when the count changed), position (0, 0, -1), size = SunSize `+0x3a8` x `+0x50`, dirty `+0x1b9`, `eCSkyObject::Render(+0x310)` -> FillVertexBuffer (managed VB lock) | first use **C**; per frame D | our composition computes the vertices natively but reads the object's visual / record |
| `CreateMoon` 0x6e3c30 (builds the moon billboard; a "MoonSize" notify clears it, `eCSkydome_PS::OnNotifyPropertyValueChangedExitEx` 0x6e4420) | **C** | |
| moon: shader `+0x148` BlendMode 5, image `+0x130`, colour `+0x58`, `eCSkyObject::Render(+0x350)` | D | |
| cloud **Scale** nodes `+0x110 / +0x118`: `"Scale"` notify pair around `+0x48 / +0x4c = 1 / CloudSize1 (+0xb8)` and `1 / CloudSize2 (+0xbc)`; the node's notify `eCTexCoordSrcScale::OnNotifyPropertyValueChangedExitEx` 0x6e0850 writes its VS pool register (pool `+0x80`, register `+0x88`) | **S** | the dome's VS pool (cloud uv scale) follows the weather's cloud sizes only through this |
| `eCTexCoordSrcScroller::SetDirection(+0x108, sky +0xb0)` 0x24f630 (`"Direction"` notify pair around `+0x50 / +0x54`) | **S** | `eCTexCoordSrcScroller::SetTime` 0x6e0c00 (admin Process, every frame) builds the scroll offset from it: skipped = clouds keep the old direction |
| dome shader `sky +0xf0`: `+0x304 = +0xa4`, `+0x308 = +0xa8`, `+0x310 = +0x90`, `+0x300 = +0xa0` (no notify) | **S** | `eCShaderSkyDome::OnNotifyPropertyValueChangedExitEx` 0x6d7840 copies them into PS c6 at its next notify (the weather's property writes): skipped = the cloud colour / thickness never change |
| dome: GetPasses / ExecutePass (compile C, image touches C: cloud / absorption images) / `eCGfxVisual::Execute` | D (C parts) | |

**Sky cube faces** (`eCSkydome_PS::UpdateSkydomeEnvCubemap` 0x6e7ed0, from `eCIlluminationAdmin::Process`): the face state machine
(timer `+0x3b8` += `eCTimer +0x2c`, threshold `+0x3bc`, face counter `DAT_180c6ea90`), `CreateSkydomeEnvCubemapCamera`,
BeginMaterial, SetRenderTarget / ClearViewport, the cube camera's projection / `SetViewMatrix` / `Execute` and BeginScene / EndScene
stay the engine's (they are not slot code). Only the `Render(this)` with `+0x3cc = 1` inside each face is skippable; its non-D work
is the same as the main dome's (idempotent within a frame).

Lean path: in the sky's existing detour (`engine/scene_hooks.cpp detourSkydomeRender`, which already sees both the op and the faces):
if `PS +0x18 & 1`: `CreateBuffers`, `CreateStars`, `CreateMoon` (all exported, early-out when nothing changed), the sun object's
billboard count / array on first use (or let the engine run Render once in the first frame: today's "buffers not created yet"),
then the S writes of 9 (`eCTexCoordSrcScale::SetScale` 0x24e6a0 for both nodes with (1/size, 1/size) does exactly Render's notify
pair + write; `eCTexCoordSrcScroller::SetDirection` 0x24f630; the four dome fields), then compose and issue our draws, touch the
sky's sampler images, and do not call the original. Do the S writes **before** composing (the engine's order: Render writes them
before the dome's ExecutePass, so the dome draw of the same frame already uses the new scale register; the scroller and PS c6
take effect one frame / one notify later in both cases).

---

## 6. Engine state our composition reads today (K) and how skipping changes it

### 6.1 The constant-pool mirror is re-read at every op's entry

`StaticOps::initMirror` (src/native/static_ops.cpp) copies, **per op**, the admin VS / PS pools, the global pool
`ms_pGlobalShaderConstants` (c31..c231), the lighting pool (c14..c37), the admin lightmap slots and (lazily) each shader's VS / PS
pools and slot images from engine memory, then overwrites what the op writes natively. Today that memory holds what the engine's
previous slot wrote (the engine's code ran), so the "carry-over" registers nobody writes per draw are right: c32 + 4i `.xyz`
(SpeedTree instance data, D 2), c8..c10 `.w`, PS c2, c14 `.w`, the lighting registers beyond a pass's light count, the instance
slots beyond a batch, PS c5.w after a masked pass.

Once the slots are skipped, the engine's pools stop changing inside the passes and the mirror would read stale carry-over values.
Recommendation: make the mirror a **persistent register file of our own** (initialised once per pass from the engine pools,
updated by every composed draw of every group: world / fade / instance data / lighting / mask reference / tree data, carried across
ops exactly as the device's register file is), and re-read from engine memory only what the engine still writes outside the slots:
the shaders' own pools (SetTime in `eCShaderMaterialAdmin::Process` 0x6bb250, property notifies such as the sky's, SetSwitch is ours
per op anyway), the admin pools' per-view registers (camera / fog: composed natively already), and anything the other slices'
lean paths decide to keep in engine memory (SpeedTree / vegetation wind registers `SetSpeedTreeData` 0x6b7df0 /
`SetVegationWindData` 0x6b7f30 are written from inside the tree / grass render code: that slice must produce them). Whether a
carry-over register is CTAB-live in a shader decides whether a stale one is visible; the compare mode will flag every difference
either way (it compares every CTAB-live register).

### 6.2 Admin lightmap slots (s4 / s5)

`SetLightmap` keeps `admin +0x2e0 / +0x308`; `BeginMaterial` 0x6b9d20 binds them at every pass begin (ExecuteShaderSamplers). Our
composition binds s4 / s5 from the mirror for every solid draw: lightmapped draws always come after their own BindLightmaps (right
either way), non-lightmapped shaders do not sample s4 / s5 (not visible). If the lean path calls `BindLightmaps` itself (item 2), the
admin slots stay exactly the engine's and the compare stays clean; if it only requests the images, the composition must keep the
last lightmap per stage in its own state.

### 6.3 Leftover engine draws and the engine's caches

Ops left to the engine (a permutation not compiled yet, an unreadable op, an instanced emitter before its master in other slices)
run the full engine slot between our draws. The engine's caches (item 13) assume the device holds what the engine last set:
`FUN_1806bbec0` skips SetVertexShader / SetPixelShader when the stage cache matches, `ExecuteShaderConstants` uploads only dirty
ranges unless the active shader / constants changed, the lighting pool skips c14..c24 while the cached list matches,
`eCGfxImageData::Execute` / `eCGfxCamera::Execute` skip when `ms_bOptimize` and the object is active. Today the device state is
saved before and restored after our draws, so this holds. In a lean mode that drops the save / restore, before every leftover op
either restore the device state, or invalidate the caches the way `BeginMaterial` 0x6b9d20 does (`ms_pActiveShader`,
`ms_pActiveShaderConstants`, `0xc62990`, `0xc62950 / 0xc62958` = 0, `[0xc62968] +0x48` = 0, `[0xc62c10] +0x48` = 0, admin pools
force-uploaded with `FUN_1806b5d20(admin +0x228 / +0x208, 1)`), plus `eCGfxImageData::sResetCachedStates` 0x755180 /
`eCGfxCamera::sResetCachedStates` 0x500b40 / `eCGfxShader::sResetCachedStates` 0x504710 (these only force the *next* execute: one
global `ms_bReset` each), plus the mixer's master states the pass set (BeginZPass / BeginMaterial / SetCurrentRenderStates).

### 6.4 Draw-tag and phase hooks inside skipped code

Detours that live inside skipped functions stop firing: `eCSubMesh::Render` (draw tag: sub-mesh, material, water class),
`eCSkydome_PS::Render` (tag + the sky cube faces' compose point), `RenderDecals` / `RenderBillboards` (the non-op units' compose
points). The composition already sets the tags itself; the non-op units must move their "skip the original" decision into those
detours. The slot thunks' pass markers (`engine/scene_hooks.cpp contextSlot`: the refracted ops' draws must stay inside the
Refracted pass) must stay around our draws.

---

## 7. Recommended lean path per slot

| slot / path | call or re-do (before / instead of the engine) | drop | fall back to the engine slot when |
|---|---|---|---|
| mesh z op (main z pre-pass, every shadow view) | `EnableStream(3, false)` on the visual's HalPrimitive (when `res +0x38 == 3` and the visual exists); image touches of the z PS's live samplers | everything else | the special-pass permutation is missing (`version +0x18 + idx` set but element / HLSL shader null), the shader is not `eCShaderDefault` / `_3_0` |
| mesh solid / alpha / refracted op | `RequestData(lightmap, 1)` x2 (or `BindLightmaps(res, 1)`), image touches of the live samplers of every composed pass (incl. switch / animated images as bound), overlay materials `RequestData(…, 1)` + their images; keep the pass marker | everything else (material touch already done at request) | a pass permutation (VS / PS element) missing, an overlay permutation missing, `+0x2a8` invalid shader (fallback / error passes), a non-default shader class |
| instanced op | the instance sort (copy, or `SortRenderInstances(void)` 0x760780 + `+0xa4 = 0` once per frame per resource), `EnableStream(3, true)` on the hardware path, lightmap touches, image touches | the rest | as above |
| decals (RenderDecals detour) | `CreateBuffers`, `CreateDefaultShader`, material + lightmap + image touches | the original call | a permutation missing, buffers not created |
| opaque billboards (RenderBillboards detour) | `CreateBuffers`, `CreateDefaultMaterial`, material + image touches, **`admin +0x28 = 0`** after composing | the original call | as decals |
| alpha billboard op | `RequestData(material, 1)` (and not drawing while it is not loaded, as the engine), image touches | the slot | as above |
| sky dome op + cube faces (sky Render detour) | `CreateBuffers`, `CreateStars`, `CreateMoon`, sun billboard on first use, `SetScale(+0x110)`, `SetScale(+0x118)`, `SetDirection(+0x108, +0xb0)`, the four dome fields, image touches | the original Render | first use (buffers / sun array missing), a permutation missing |
| every image our draws bind | `FUN_18070fd00(imageData +0x70)` once per frame when `+0x70 != 0` (video) | | |

"Image touch" = `eCResource2::RequestData(image, 1)` 0x684100 (exported: state 1-2 `QueueData(res, 1)`, state 3 `CacheResource`),
once per frame per image is enough (the engine touches on shader change / dirty slot, i.e. at most a few times per frame; the LRU
only needs the stamp of the current cache frame). Batch them: collect the composed draws' images into a per-frame set and touch
each once at the end of the frame's composition (before `eCResourceCache2::Process` of the next frame), not per draw.

---

## 8. Risks (things that look droppable but are not)

1. **Texture eviction** (items 1-3): skipping ExecuteShaderConstants removes the only per-frame touch of material images. The
   resource cache evicts LRU-tail resources not stamped this cache frame when a load needs room (`FlushBytes` from `LoadData`);
   nothing re-queues them, so the composition would bind placeholders (or the ErrorImage) indefinitely. Not visible in short spot
   runs with free cache budget; shows after travelling (streaming pressure). Test: a long walk / teleport tour with the lean path
   and a texture-placeholder counter.
2. **Billboard queue growth** (item 8): the queue is appended at request time every frame; skipping RenderBillboards without the
   reset makes it grow without bound and re-draws every old entry.
3. **Stream 3 flag** (item 6): a lightmapped or not-capable instanced resource drawn by the per-instance fallback, or any solid
   sub-mesh of an instancing-capable visual, draws with whatever the last z op / batch of that visual left (this frame or an earlier
   one). If nothing sets it any more, a stale "on" binds the instance-index stream to a non-instanced draw (harmless for the
   non-instanced VS, which does not read TEXCOORD4 [I]) and a stale "off" breaks a later batch that relies on it [I: batches set it
   themselves]; the compare would flag both. Set it ourselves where the engine does.
4. **Clouds frozen to the first weather** (item 9): no error, just no reaction to weather changes (cloud size, scroll direction,
   cloud colour / thickness). Verify with a weather change (rain -> clear) or a long hold with the clock running.
5. **Carry-over registers** (6.1): the per-op mirror re-read becomes wrong once nobody writes the pools; switch the mirror to our
   own register file first, then skip.
6. **Leftover engine draws after ours** (6.3): stale engine caches give an engine draw our shaders / constants / textures. Keep the
   save / restore (or invalidate) for ops left to the engine.
7. **First-use creation inside the slot** (items 10, 12): the first frame of a session (and after `RestoreResources`) still needs
   the engine path or explicit Create calls; the sun's billboard array is only created inside Render.
8. **Compiles happen inside the draw** (item 4): keep "left to the engine" for missing permutations (the shader pool write of hard
   rule 1 stays the engine's behaviour, unchanged). Note that a compile re-walks the graph into the shader's pools / sampler slots
   (it can reset a slot image a SetImageResource had changed: the default shaders, stars / moon), which is why those compositions
   override blend / image / colour themselves.
9. **The shared RNG** (CreateStars): calling CreateStars from our detour keeps the engine's order of RNG draws only if it is called
   at the same point of the frame (inside the sky's Render position); it rebuilds only on a StarCount change, so the effect is
   limited to that frame.
10. **Video textures** (item 11): not met in materials [I]; a material bound to a Bink image would freeze without the explicit
    advance.

---

## 9. Entry points (exported unless marked; prologue bytes from the test copy's Engine.dll)

| RVA | name | use | first bytes |
|---|---|---|---|
| 0x684100 | `eCResource2::RequestData(eEResourceLoadMode)` | image / lightmap / material / overlay touch (mode 1) | `40 53 48 83 ec 20 48 8b d9 48 8b 49 30 48 85 c9` |
| 0x764220 | `eCMeshResource2::BindLightmaps(eEResourceLoadMode)` | lightmap touches + admin slots (returns the combi) | `48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57` |
| 0x760780 | `eCMeshResource2::SortRenderInstances(void)` | the in-place instance sort (then write `+0xa4 = 0`) | `48 63 81 b8 00 00 00 83 f8 01 7e 1c 48 8b 89 b0` |
| 0x4f00a0 / 0x4efdc0 | `eCBillboardAdmin::CreateBuffers` / `CreateDefaultMaterial` (protected) | first use | `48 89 5c 24 10 48 89 74 24 18 48 89 7c 24 20 55` / `40 55 48 83 ec 30 48 83 79 30 00 48 8b e9 0f 85` |
| 0x4fbd80 / 0x4fba60 | `eCDecalAdmin::CreateBuffers` / `CreateDefaultShader` | first use | `48 89 5c 24 10 48 89 74 24 18 48 89 7c 24 20 55` / `40 56 48 83 ec 30 48 83 79 48 00 48 8b f1 0f 85` |
| 0x6e5f90 | `eCSkydome_PS::CreateBuffers` (protected) | first use | `48 89 5c 24 10 48 89 74 24 18 48 89 7c 24 20 55` |
| 0x6e3ee0 | `eCSkydome_PS::CreateStars` (protected) | star rebuild | `4c 8b dc 41 56 48 81 ec d0 00 00 00 48 83 b9 40` |
| 0x6e3c30 | `eCSkydome_PS::CreateMoon` (protected) | moon rebuild | `40 57 48 83 ec 60 48 83 b9 30 01 00 00 00 48 8b` |
| 0x6e1680 | `eCSkyObject::SetBillboardRenderCount(unsigned long)` | the sun record | `40 53 48 83 ec 20 48 8b 41 40 8b da 48 85 c0 74` |
| 0x24e6a0 | `eCTexCoordSrcScale::SetScale(bCVector2 const &)` | cloud scale (notify pair + `+0x48 / +0x4c`) | `48 89 5c 24 08 57 48 83 ec 20 48 8b 01 48 8b da` |
| 0x24f630 | `eCTexCoordSrcScroller::SetDirection(bCVector2 const &)` | cloud direction (notify pair + `+0x50 / +0x54`) | `48 89 5c 24 08 57 48 83 ec 20 48 8b 01 48 8b da` (same prologue as SetScale, different body: check the string operand) |
| 0x70fd00 | `FUN_18070fd00` (**not exported**; Bink video frame advance, `rcx` = the video object at `imageData +0x70`) | video textures | `40 53 48 83 ec 40 48 8b d9 48 8b 49 08 48 85 c9` |
| 0x755180 / 0x500b40 / 0x504710 | `eCGfxImageData` / `eCGfxCamera` / `eCGfxShader` `::sResetCachedStates` | leftover coherence (6.3) | each `c6 05 <rel32> 01 c3` (`mov byte [ms_bReset], 1; ret`) |
| 0x6b9d20 | `eCShaderMaterialAdmin::BeginMaterial` | reference for the cache invalidation list (do not call inside a pass: it refuses while `+0x311` is set) | |

Do not detour: the 5-byte `jmp` thunks 0x5c73e0 / 0x5c73f0 / 0x5c7400, the sky slot 0x6e8a10, the empty 0x6e8a00 / 0x6e4720 (calling
the exported functions above is fine; SetScale / SetDirection are small but not COMDAT-folded: their string operands differ).

---

## 10. Open / to verify in game

* Image eviction under streaming pressure with the lean path (risk 1): count placeholder / ErrorImage binds per 5 s on a long walk.
* Whether any material image carries a Bink video (`imageData +0x70 != 0`) in the shipped data (log once per image).
* Cloud scale / direction / colour after a weather change with the sky's lean path, against the engine (water-compare with the clock
  running, `freeze = false` spots).
* The stream-3 flag: compare our own flag model against the record (`HalPrimitive::streamView(3).enabled`) at each solid draw for one
  run before relying on it.
* The decal / billboard default shaders after a compile in the fallback path (risk 8): their slot image is rebuilt by the compile's
  graph walk; our compositions override it, so this matters only for the engine's own draws.
