# Full mode optimization plan (2026-10-28)

Inputs: `opt-profile.md` (where Full's CPU goes), `opt-side-effects-mesh.md`, `opt-side-effects-anim.md`, `opt-side-effects-frame.md`
(what the engine's render code does besides drawing). All side-effect findings are from code reading, not yet checked in game.

## Where the time goes (savegame Risen_Save_0009, uncapped, unprofiled)

Wrapper 3.30 ms CPU / frame, Full 8.52 ms (+5.2). GPU equal (1.5 ms). Engine.dll self time is the same in every mode: the engine
does no extra work in Full. Of Full's extra (profiled +5.7 ms):

| bucket | ms | removed by |
|---|---|---|
| our composition (`constantsInto` bit-by-bit masks 1.0, per-op pool re-read 0.3-0.4, SEH-guarded reads 0.4, CRT copies, heap, texture lookups) | ~3.9 | O1 |
| our issue path: per-op whole device state save / restore (~0.6), one setter per register (~0.7) | ~1.3 | O1 (setters, delta restore), O2 (no restore) |
| engine slot code + its HAL calls, dropped draws | ~1.3 | O2 |
| tap, attribution, coverage, second qsort | 0.3 | O1 (sort once), O2 |

Cost follows composed draws (~5-6 us each vs ~1.1 us per Wrapper draw), not the kind.

## Stages

**O1 - our own costs, engine still running** (verifiable per draw with the existing compare modes: 0 mismatches must hold):
word-wise live register masks; constants written straight into the draw packet; validate an engine object once per op then plain reads;
our own persistent constant register file and per-effect value store instead of re-reading the engine's pools; heap / buffer reuse;
cached texture-of-API lookups; per-op state save / restore reduced to the delta we changed; the double sort removed (Execute's qsort
sites redirected to sort-then-snapshot); coverage counter behind a debug switch. Target: Full ~ Wrapper + ~1.5 ms.

**O2 - lean executor** (the engine's render code no longer runs): our executor replaces `Execute`, `ExecuteShadowMap`, the PSSM loop at
the existing detours, walks the engine's live sorted lists (one in-place sort with the engine's qsort / comparators), calls the
engine's cheap brackets at Execute's places (camera, fog, Begin/EndZPass, Begin/EndMaterial, sampler holders, ClearCells, module
OnEndRenderSolid) and reverts its own state at each bracket end; runs the extracted side effects per reached op in op order:
resource touches (`eCResource2::RequestData(res,1)` for every image / mesh our draws bind; lightmaps), actor palette
(`UpdateRenderMatrices`), SpeedTree queue sort + stamp reset + MaskReference 80, vegetation buffer cursor, particles (master
instance, no-live-particle simulation, inactivity timer reset, in-place depth sort, buffer / material / image creation), strip and
streak buffer creation, billboard queue reset, sky cloud scale / scroller direction / cloud colour fields, stream-3 flag, Bink
textures; ops not ready (permutation not compiled, buffers missing) run the engine's slot after a bracket pair resets its caches.
Shadow views execute at their own `ExecuteShadowMap` (before the next Request). Culling / extraction stay whole (gameplay reads the
visible stamp: camera admin, AI). Verification: `native.lean_check=N` runs the full engine path every Nth frame and compares;
behaviour checks (no texture eviction of bound images, streaming after teleports, particles keep simulating, AI visibility, focus);
same-frame switch images.

**O3 - effects and GUI lean**: replace `ExecuteEffect` / `eCSprite::Flush` fully (graph walk, RT pool, CFXGui, Game.dll draw
callbacks stay); the focus indicators' draws (Game.dll `gCFocusCrossHairIndicator2`, `gCFocusScreenPointIndicator2`) composed.

**Then own modes** (A/B against Full): retained composition (cache composed draws across frames, only dynamic constants per frame),
merged indirect draws, GPU culling, the ubershader, bindless materials.

## O1 result (2026-10-28)

Commits e4bc34f (part 1), 3ae7c86 (part 2), 42e6a12 (part 3) on `feat/native-scene`. The engine's render code still runs; every
step was checked with the per-draw compare (`native.static_ops_compare` + `native.scene_ops_compare`, every group taken over) before
it was kept. Logs, scripts and the DLL / PDB of each profiled run: `out/o1/` (`batch.ps1` perf, `compare.ps1` compare runs,
`check.py` the compare totals, `table.py` = `out/optprof/table.py` with `RVK_DLL`).

**CPU per frame at the savegame** (Risen_Save_0009, `rvk_perfsave` hold 40, `-Exe RisenTest.exe -Vsync off -Perf`, test baseline,
1600x900, machine idle: `sysprobe` before the first batch, every batch line 0 % CPU, 41-44 GiB free; "engine+HAL" of the last 6
`perf:` windows, unprofiled):

| build | Wrapper | Full | Full - Wrapper | GPU |
|---|---|---|---|---|
| before (c338059) | 3.36 | 8.53 | +5.17 | 1.51 |
| part 1 (e4bc34f) | | 6.32 | | 1.51 |
| part 2 (3ae7c86) | | 6.02 | | 1.50 |
| part 3a (constants by region, effect units) | | 5.61 | | 1.50 |
| part 3 (42e6a12) | 3.33 | **5.42 - 5.48** (two runs) | **+2.15** | 1.47 - 1.49 |
| profiled (`-CpuProfile`) before / after | | 9.42 / 5.90 | | |

Full is 36 % cheaper (-3.05 ms); fps at the savegame 114 -> 175 (Wrapper 279). The target "Wrapper + ~1.5 ms" is not reached:
+2.15 remain, of which the engine's own double work (its slot / effect code, its HAL calls, its dropped draws: ~1.3 ms, D1 - D4 of
opt-profile.md) is O2's.

**Per item** (the saving is the step's measured difference; where a step held several items the split comes from the A/B
settings or the profile):

| item | what changed | saving (unprofiled) | verification |
|---|---|---|---|
| 1 `constantsInto` word-wise | part 1: the live sets as 64-bit words (`Live::words`), iterate set bits, copy the bitsets whole; part 3a: by source region with 256-bit masks (`sc::RegMask`, `regRange` / `andMask` / `andNot`, unit-tested) and copied as runs, the per-register sources (compare diagnostics) only while the group compares; `sc::lowBit` = `_BitScanForward64` (std::countr_zero is not inlined under /Ob1) | part 1 with items 2 / 3 / 5 / 6: -2.21; part 3a with the effect units' paths: -0.41. Profile: `constantsInto` 1.02 ms incl -> 0.40 self after part 2, below the profiler's cut after part 3a | c1 .. c4 compare runs |
| 2 constants / states straight into the device's register file as ranges | `DrawDesc::vsSet / psSet` (the composed registers as masks), `issue` writes one setter per run (`sc::forEachRun`), the sampler states of a stage in one call (`Device::setSamplerStates`, `samplerStateRow`, `stageTexture` / `setStageTexture`: inline, no slot map); the effect units (`captureDevice` / `applyDevice`) the same way, the differing float registers as ranges | in part 1 / part 3a | as above |
| 3 engine object reads | `engine::readAt` = one fixed-size guarded load (`safeLoad1/2/4/8`) instead of `safeRead`'s copy routine; the native files' `get` / `ptr` / `rd` inline; per op: the admin's sampler block (+0x248..+0x350), the per-stage override images and the default filter read once (`StaticOps::samplerSnap`, `engine::safeReadN<N>`), a material sampler slot's 0x28 bytes at once | in part 1; part 3b (with the pointer hash): -0.19 | c4 |
| 4 own register file | `native.mirror` 0 (default): the global pool c31..c231, the lighting pool c14..c37 and the admin's lightmap slots read once and then carried by our composition across ops, views and frames; engine code we do not compose is bracketed (snapshot before, the changed registers merged at the next use); the admin pools and the shaders' pools re-read once per view (an epoch; the pool map persists: no per-op nodes); PS c5.w stays ours across the re-read. 1 / 2 = the engine's pools per view / per op (A/B) | 0.15 (`native.mirror=2` 5.63 vs 5.48 at the final build) | c2 .. c6 and the final set, all with mode 0 |
| 5 heap / lookups | per-op vectors of the compositions as reused scratch (`StaticOps::scr_`); `textureOfImageData` behind a 4096-entry cache cleared by the HAL registry's generation (`HalDevice::apiTextureGeneration`); the shader / element / pool maps with a multiplicative pointer hash (`PtrHash`) | in part 1 / part 3b; profile: `RtlAllocateHeap` 0.17 -> 0.06, `textureOfApi` + its FNV hashing 0.11 -> `textureOfImageData` 0.03 | as above |
| 6 delta save / restore | `issue` saves / restores only the union of the op's composed register ranges and sampled stages (+ the shaders, declaration, streams, indices, ten owned states every draw sets); not removed: the compare needs the engine's own state after our draws (O2 drops it) | in part 1; profile: the 15 KB copy per op and 224 sampler-state calls gone | c1 |
| 7 the double sort | **not built**: measured 0.02 ms profiled (opt-profile.md D6: Engine.dll qsort self 0.035 -> 0.057 with the tap), below the run-to-run spread; redirecting Execute's / ExecuteShadowMap's qsort sites needs a code patch or a qsort detour for every engine caller. O2's executor sorts once by construction | - | - |
| 8 coverage counter | `native.coverage` (default off): the counter + the engine-reason strings run only with a compare, `native.frame_tap` or this setting (plain Full: off); the tap's attribution stays (its next-op cursor feeds the refraction copy) | 0.04 (`native.coverage=true` 5.52 vs 5.48) | the compare runs report coverage as before (99.97 - 99.99 % ours) |

Tried and not kept (no measurable gain, reverted): fixed-size guarded copies for every `rd` with a constant size + one read of a
tree instance's LOD fields + the effect units' parameter / texture buffers reused (5.50 vs 5.42); the fx constants' parameter
indices resolved once at parse instead of a name search per evaluation (5.44 unprofiled, -0.08 only profiled).

**Where the Full frame goes now** (profiled 5.90 ms; opt-profile.md section 4 had 9.36): `StaticOps::opEnter` 2.45 incl (was 5.70) of
which `issue` 1.20 (our draws' recording ~1.0 + setters / delta save-restore ~0.2) and the composition ~1.25 (SpeedTrees
`composeTree` 0.41, `materialDraws` ~0.4); the effect units `effectBegin` 0.52 (post chain, mask; the per-unit capture + parameters);
the engine's slot code behind the slot thunks (`contextSlot<7>` SpeedTree Render 0.75, `<8>` 0.54, `<0>` 0.46 incl, partly ours).
Module self: ours 2.59 (was 5.10), vcruntime140 0.51 (1.18), ntdll 0.53 (0.73), Engine.dll 1.27 (unchanged).

**Verification.** Compare after every step at the rvkcompare spots pub_interior + jungle (actors shown) and the savegame (hold 30):
0 mismatches in every group and field class, draws compared = equal, every own draw equal to its composition (c1 - c6, ~4 M draws
each). Final set at 42e6a12 (all take-over + compare, `out/o1/f_*.log`): `rvk_menufps`, `rvk_spells`, `rvk_pointshadow`, `rvk_rare`
(with validation), the storm set (beach_night_storm, beach_rain, hero_idle, pub_interior), the savegame with validation and
`rvk_pointshadow` with synchronization validation (RT shadows off): every group 0 mismatches, 0 own draws differing, effect draws
equal (78 k - 310 k per run); validation 0 messages, sync validation 0 messages / no SYNC- hazard; coverage 99.97 - 99.99 % ours
(10.0 M draws in `rvk_rare`). rvk_tests 177 / 177 (new: `static_register_runs_merge_across_words`), no warnings in our sources.

### The register file and O2

With `native.mirror=0` nothing of the composition reads back what an engine slot of a composed op wrote: the carry-over registers
(c32 + 4i .xyz of the SpeedTree instance data, the instance slots beyond a batch, c14 .w and the lighting registers beyond a pass's
light count, PS c5.w after a masked pass, the bone / morph registers) are now ours, carried as the device would carry them, and the
compare holds with that over every run above (the proof that our writes reproduce the engine's). What still comes from engine
memory, and why it stays right in O2:

| source | read | writer | O2 |
|---|---|---|---|
| global / lighting pool, lightmap slots | once (and after a reallocation) | the slots only | ours; ops / units left to the engine are merged by their diff (`native: register file: ... merged` in rvk.log: 4,710 left units, 5,107 registers at the savegame) - also right when their slot is the only engine code that runs |
| admin VS c0..c30 / PS c0..c5 | once per view | brackets (camera, fog, obstacle: composed natively anyway), Request (SpeedTree wind: composed natively per op), Process (vegetation wind: native) | the non-native rest (VS c8..c10 .w, c13, PS c2, c5.z) is read from engine memory per view: **to check in O2** that no slot writes them (the lean_check frames) |
| the shaders' own pools | once per view (epoch) | SetTime / notifies outside the slots; SetSwitch inside (applied to our copy per op) | as now |
| eCShaderParticle's pool c31..c60 (particles c35..c60, strips c41) | per op (`static_ops_fx.cpp`) | c31..c34 / c31..c40 composed natively | the rest per op from engine memory: **to check in O2** |

**Left for O2**: the engine's double work (~1.3 ms: its slot code, its HAL calls and dropped draws, the per-op save / restore that only
exists for the engine's code after ours, the effect units' state capture); the two "to check" rows above; the double sort; then the
remaining composition per draw (~1.4 us) and the effect units (~17 us each) as the next O1-like targets.

## O2a result (2026-10-28): lean executor framework + the mesh-like groups

Commits 073d57a (framework + side effects), 2a7ee69 (symbols.h layout), 63ed86a (report fix) on `feat/native-scene`. Settings
`native.lean` (Debug | NoAB, default **off**; acts per op only where the op's group takes over, i.e. in Full or with the
`native.static_ops` / `native.scene_ops` overrides) and `native.lean_check` (Debug, default 0). Code: `src/native/static_ops_lean.cpp`
(+ the collection points in `static_ops.cpp`, `static_ops_nonop.cpp`, `static_ops_sky.cpp`), pure rules in `static_compose.h`
(`sc::leanFrameKind`, `sc::TouchSet`; test `static_lean_check_frames_and_touch_set`). Logs, scripts: `out/o2a/` (`compare.ps1`,
`behaviour.ps1`, `switch.ps1`, `batch.ps1`, `check.py`, `table.py`).

### Design: per-slot lean thunks inside the engine's Execute (not a replacement executor)

The engine's `eCFrustumDatabase::Execute` keeps running; only the slot calls of the ops we compose return early. Reasons:
* Everything of Execute besides its slot loops is cheap and full of K duties (opt-side-effects-frame.md 4.1: camera / fog, the four
  brackets with their state blocks and cache resets, the depth / shadow-mask / refraction holders, the refraction copy, the module
  callbacks, the in-place qsorts); keeping it keeps every bracket, holder and the order for free, and the slots still receive
  `&data[i]` of the engine's own sorted live arrays (a fallback slot runs exactly where the engine would run it).
* The entry points already exist: the render-context vtable thunks (`contextSlot` / `dynamicSlot` / `opSlot` in
  `engine/scene_hooks.cpp`) and the non-op detours (`RenderDecals`, `RenderBillboards`, `eCSkydome_PS::Render` for the cube faces).
  `OpTap::opEnter` / `nonOpBegin` now return "skip"; the thunk then returns without calling the engine (the pass marker / draw tag
  scopes around it stay). Not skipping = the O1 behaviour (the engine's code runs, its draws are dropped).
* Per op: compose -> issue (as O1) -> if this is a lean frame, the group is one of O2a's and takes over, and every side effect is
  covered: run the collected side effects, skip the engine's code. Otherwise (composition skipped: not compiled, invalid shader, ...;
  a video texture bound; a check frame) the engine's code runs as before.
* What a replacement executor would add (one sort, no copy of the lists) is small: the double sort is 0.02 ms (O1 item 7); the
  tap's copy + resolve stays for now (it feeds the `Op` the composition reads; it could resolve lazily from the live op later).

**The per-op save / restore stays in O2a** (decided after measuring). With it the device after a lean op holds the state the
engine's last *run* code left, so every engine cache (`ms_pActiveShader`, the HLSL stage cache, the lighting pool's light list, the
pools' uploaded ranges, `ms_pActiveImageData`) stays true for the engine code that still follows: fallback ops, the slots of the
groups not yet lean (actors, trees, vegetation, particles, shadow views), the effect units' device capture, the brackets'
`EndRenderStateBlock` (it only knows HAL-set states). Its cost in the lean profile is in the noise (`issue`'s restore lambdas <=
0.02 ms per frame at the savegame); dropping it needs the cache invalidation of opt-side-effects-frame.md 9.1 (b) before every engine
code run, which only pays once most slots are lean (O2b / O2c). Consequence, by design: after a lean op the device "lags" (it holds
the last engine-run op's state, not the skipped op's). Our draws do not read it (own register file `native.mirror`; the bool / int
registers and the inherited stages are not written by the skipped code); the dropped engine draws of non-lean ops do, through
carry-over registers (below).

**Compare under lean**: with `native.lean_check = N`, frame f runs the engine's code when f % N == 0 (check frame) or (f + 1) % N
== 0 (warm-up frame); the static ops' compare (all StaticOps groups) runs only on check frames. Without the warm-up frame the first
z op of a view (the hero's head, the first shadow-view op) mismatched in "instance c32+" (c42.x): the engine's check-frame draw read
carry-over registers that the previous lean frame's instanced batches had not written into the engine's device (`l1_storm.log`: 330
actor + 596 shadow-view draws of 0.64 M); with the warm-up frame 0 (`l2_storm.log`). For the same reason the compare of the non-lean
groups is not meaningful on lean frames (their dropped engine draws see the lagging device).

### Per group: the side effects run by us ([G] seen in game, [C] code / RE only)

RE facts (re-lookup on the dumps + capstone, 2026-10-28): `eCResource2::RequestData(res, 1)` = QueueData(1) for state 1-2,
CacheResource for 3, nothing for <= 0 / > 3 (ExecuteShaderConstants' inline touch is the same switch, only for the slots the PS
samples, on a shader change or a dirty slot); RenderZPass disables stream 3 (`prim +0x60 = 0` / vtable +0x68) right after its
`res +0x38 == 3 && visual` gate, before the sub-mesh loop; ExecuteRenderInstances returns at count 0, then BindLightmaps(1)
unconditionally, the hardware path (`res +0xa0 != 0 && count >= 2 && combi != 1`) enables stream 3 once before its batch loop and
touches the material per flush; `eCRenderContextMesh::Render` sorts (count > 1) and writes `+0xa4 = 0`; `eCMeshResource2::Render`
returns at a null visual, else BindLightmaps(1) first; RenderDecals / RenderBillboards return false only when a Create failed,
Execute ignores both results (0x66f195 / 0x66f1a2); the billboard queue reset sits inside the `count != 0` block;
`eCSkydome_PS::Render`: all inside `+0x18 & 1`, CreateBuffers (false: return), the camera hack, CreateStars (stars drawn), the sun
(`+0x138`: SetBillboardRenderCount(+0x170, 1), the one-billboard array), CreateMoon (moon drawn), the two "Scale" notify pairs
inlined (+0x48 = +0x4c = 1.0f / CloudSize +0xb8 / +0xbc, what SetScale does), `SetDirection(+0x108, sky +0xb0)`, dome `+0x300 /
+0x304 / +0x308 / +0x310` = sky `+0xa0 / +0xa4 / +0xa8 / +0x90`, the restore, the dome draw. All functions called are unique
exports (none COMDAT-folded).

| group | side effects run at the op's place (before composing / after issuing) | verified |
|---|---|---|
| every composed draw | `RequestData(image, 1)` for every material sampler slot image the PS samples (once per frame per resource: `sc::TouchSet<8192>`); a bound image data with `+0x70` (a Bink video) keeps the engine's code | [G] 0.4 M touches / 0.13 M RequestData calls per 5 s at the savegame; placeholder binds as without lean (behaviour checks); no video image met ("kept (video) 0" in every run) |
| static z ops (sub-mesh, whole mesh, nodes) | `EnableStream(3, false)` on the visual's HalPrimitive (`res +0x38 == 3` and a visual); image touches of the masked z PS | [G] compare on check frames |
| static solid (sub-mesh, whole mesh, nodes, blended terrain layers) | `BindLightmaps(res, 1)` (the lightmaps' touches, admin s4 / s5, the material mode's stage bind), the sub-mesh material's touch, images | [G] |
| instanced | before composing `SortRenderInstances(res)` + `res +0xa4 = 0` (the composition then reads the sorted list); BindLightmaps, `EnableStream(3, true)` on the hardware path, the material touch, images | [G] ~2 k sorts per 5 s |
| alpha / refracted mesh ops | as solid (the refracted water through the enhanced water's replacer, unchanged) | [G] |
| entity overlay passes (static / mesh) | `RequestData(overlay material, 1)` every frame while flagged, the overlay shader's images | [C] (static-mesh overlays not met; the actors' highlighted overlays are O2b's) |
| decals | before composing `CreateBuffers` + `CreateDefaultShader` (early-out when made); every record's material touch (also without a shader), `BindLightmaps(lightmap mesh, 1)`, images; the original not called | [G] compare; first use [C] (the buffers existed before the first lean call in every run: "first-use creates 0") |
| opaque billboards' queue | `CreateBuffers` + `CreateDefaultMaterial` first; the records' material touches, images; `admin +0x28 = 0` when the queue was not empty | [G] `rvk_rare`: one queue of 1 reset, every other window empty (bounded) |
| blended billboard ops (alpha list) | `RequestData(material, 1)` (composed only when cached: else the engine's), images | [G] `rvk_rare` (832 ops per 5 s skipped, compare clean) |
| sky dome op + cube faces | before composing (inside `+0x18 & 1`): CreateBuffers (stop when false), CreateStars, `SetBillboardRenderCount(+0x170, 1)` (the sun, when its array exists), CreateMoon, `SetScale(+0x110 / +0x118, (1 / size, 1 / size))`, `SetDirection(+0x108, +0xb0)`, the four dome fields; images | [G] `rvk_rtwalk` with `weather=Stormy_Rain`: the dome fields follow the blend and equal their sources (rvk.log `native: lean: sky clouds`), sky + cube compare clean on the check frames; the cloud size is (2, 0.2) in every weather met, so scale / direction were only seen constant |

### Verification (lean on, `native.lean_check=10`, every group taken over + compared; `out/o2a/*.log`, `check.py`)

| run | draws | result |
|---|---|---|
| pub_interior + jungle, actors shown (`l1_spots`) | 4.1 M | every group 0 mismatches, own draws = composition |
| savegame Risen_Save_0009, hold 30 (`l1_save`) | 3.7 M | 0 mismatches |
| `rvk_rare` with validation (`l1_rare`) | 9.9 M | 0 mismatches, validation 0 |
| storm set (`l2_storm`) / `rvk_menufps` (`l2_menufps`) | 4.1 M / 4.1 M | 0 mismatches (with the warm-up frame), menufps PASS |
| the rvkcompare route, all spots (`r1_route`) | 10.5 M | 0 mismatches |
| `rvk_rtwalk`, new game, Stormy_Rain, 60 s (`w1_rtwalk`) | 6.4 M | 0 mismatches |
| savegame `-Validation` (`v1_validation`), `rvk_pointshadow` `-SyncValidation` (`v1_pointshadow_sync`, RT shadows off) | 2.5 M / 2.7 M | 0 messages, no SYNC- hazard |

Skipped per 5 s at the savegame: ~114 k static ops, 8 k alpha / refracted, 830 each decals / sky / billboards (~150 slots per frame).

**Behaviour checks.** Placeholder binds (images bound while not loaded, rvk.log `placeholder binds`) on the route: lean on 264 (load),
41, 59, 32, 13 after teleports, else 0; lean off 264, 33, 91, 32, 13, else 0: the same transients, settling to 0 in both (no eviction
of bound images). Billboard queue bounded, clouds follow the weather (above); decals' first use [C] only.

**Images.** Same-frame switch `native.lean` off -> on at frame 4660 (frozen holds, `out/o2a/switch.ps1`, `out/n56/switch6.py`):
pub_interior (21,766 static blocks) 0.0217 before / **0.0195** across / 0.0244 after; sea_horizon (22,400) 0.0000 / **0.0000** /
0.0000 (bit-identical). `shot-reviewer`: no placeholder / missing lightmap / decal / sky change, across within the noise.

**Perf** (savegame, `-Exe RisenTest.exe -Vsync off -Perf`, hold 40, machine idle: `sysprobe` 2 % CPU, batch lines 0 %; last 6
windows):

| run | engine+HAL ms | GPU ms |
|---|---|---|
| Wrapper | 3.29 | 1.46 |
| Full, lean off (3 runs) | 5.77 / 5.74 / 5.69 | 1.43 - 1.47 |
| Full, lean on (3 runs) | 5.75 / 5.74 / 5.76 | 1.43 - 1.46 |
| Full with the O1 DLL (42e6a12), same session | 5.82 | 1.49 |
| profiled Full / lean | 6.25 / 6.27; `eCFrustumDatabase::Execute` incl 3.94 / **3.78**; Engine.dll self 1.21 / 1.18 | |

The machine runs ~0.3 ms slower than at O1 (the O1 DLL: 5.48 then, 5.82 now), so Full - Wrapper is ~+2.4 ms here. **Lean O2a saves
nothing measurable unprofiled** (within the +-0.05 spread; profiled -0.16 ms in Execute): the mesh-like groups' slot code is cheap
(Engine.dll self -0.03 ms for ~150 skipped slots per frame, their HAL calls -0.02 ms of "draw calls"), and our side effects cost a
comparable amount. The engine's double work measured in opt-profile.md sits in the SpeedTree / vegetation / shadow-view slots
(contextSlot<7> 0.75, <8> 0.54 ms) and the effect units: O2b / O2c / O3. `native.lean` therefore stays default off; Full turns it
on once O2b gives a gain.

**Pre-existing (lean off)**: the route's lean-off run (`r0_route`) showed 20 SpeedTree draws of 2.77 M differing in "instance c32+"
(c40.x of bark draws, `ST_Bark_07 / 08`): a carry-over case of the O1 register file for trees (O2b's group), not caused by lean.

### Left for O2b / O2c / O3

* O2b: skinned actors (the palette stamp of `UpdateRenderMatrices`, the overlay requests), SpeedTrees (the queue sort, the stamp
  reset, MaskReference 80, the tree billboards' BindLightmaps), vegetation (the buffer cursor written back), particles (master
  instance, the no-live-particle simulation, inactivity timer, in-place depth sort, first-use creation), strips / streaks (buffer
  creation), precipitation; the SpeedTree c40 carry-over above; then dropping the per-op restore (cache invalidation before engine code).
* O2c: the shadow views (their z ops lean inside ExecuteShadowMap), the mask / PSSM (`UpdatePackedShadowMapVisual` -> return false).
* O3: the effect units / post chain and the sprite batcher without the engine's ExecuteEffect / Flush; the focus indicators' draws.

## O2b result (2026-10-28): lean animated groups + shadow views

Commits 9e582c4 (the groups + the pool sync + the c40 fix), f932309 (lean on by default in Full, the sampler memo, lazy tap notes),
f3056a2 (the tap's attribution of lean ops) on `feat/native-scene`. Logs, scripts, the DLL / PDB of every profiled run: `out/o2b/`
(`compare.ps1` / `check.py` compare runs, `behaviour.ps1`, `extra.ps1` coverage / tap, `switch.ps1`, `batch.ps1` + `table.py` perf).

**`native.lean` is now on by default** (Debug | NoAB; it acts only where a group takes over, i.e. Full; Wrapper unchanged). A compare
(`native.static_ops_compare` / `native.scene_ops_compare`) without `native.lean_check` keeps it off (rvk.log `native: lean ops off: a
compare is on ...`): the compare needs the engine's code. `native.lean_restore` (Debug, default on) is the A/B switch of the per-op
device restore (below).

### Per kind: the side effects run by us, at the slot's place ([G] seen in game, [C] code only)

All in the main view **and** in the shadow maps' z lists (group 512: cascades, terrain map, atlas cells use the same slots inside
ExecuteShadowMap; `leanGroup(ShadowViews)`), only for reached ops, in op order. The engine functions are bound by decorated name and
checked against their RVAs and call sites at first use (`verifyLeanAnim`: 15 sites, eCParticle_PS vtable +0x248 = OnProcess); a failed
check keeps those groups' engine code (O2a's groups stay lean).

| kind | side effects (engine function / write) | where | verified |
|---|---|---|---|
| skinned actors | `UpdateRenderMatrices(wrapper, frame id)` 0x4dfee0 when the resource has a HardRenderActor2 (palette + stamp, self-guarded: the composition then always reads the stored palette); when it returns true: `RequestData(HRA +0, 1)`, cached: `RequestData(LOD primitive's material, 1)`; overlay materials (OnGetPasses) as O2a | before composing (immediate touches: the composition reads the states after them, as HRA::Render does) | [G] 13 k - 33 k palette calls per 5 s; `rt: skin verify` lean on / off the same error distribution (max 0.0079 / 0.0117 units, `rt1` / `rt0`) |
| SpeedTree | the queue (`+0xb8` / `+0xd0`) sorted in place with the engine's qsort + SortInstances / SortInstancesZPass and the stamp `+0xe0` / `+0xe4` = 0 at the render buffer's first op (before the type dispatch, as the slot); the type material's `RequestData(1)` (after the queue / visual / wrapper checks, before the shader is read); MaskReference 80 with the two property notifies (vtable +0xb0 / +0xb8, property 0x97bb98) when the TransformationType check passes and the byte is not 80 yet; the shadow fronds rule (nothing) | in the composition (the sort replaces its copy sort; idempotent for a fallback slot: the stamp is 0) | [G] sorts 16 k - 28 k per 5 s; MaskReference: [G] 80 in every material met (0 stores: already 80 at the first lean op), the store path [C] |
| tree billboards | `RequestData(billboard mesh, 1)`; then the static z / solid paths' O2a side effects (BindLightmaps, stream 3, sub-mesh touches); no sub-mesh shader: the engine's code (edge) | composition | [G] (68 edge ops in one `rvk_rare` window while loading) |
| vegetation | the shared buffer's cursor `vegAdmin +0x40` = `planGrass`'s end cursor (only with the admin's buffer `+0x38`), written at the commit; per drawn op `RequestData(type mesh, 1)`, cached + visual + buffer: the first sub-mesh material's | composition / commit | [G] 7 k - 28 k cursor writes per 5 s, compare clean on the check frames (the batch splits depend on it) |
| particles | Render's steps: `+0x29d == 1`: `GetMasterInstance(module, PS)` (creates the hidden master; null: done); `+0x29d == 0` and no live particle: the PS's OnProcess (vtable +0x248); `+0x3ac = 0`; UpdateVertices(target): `CreateVertices` before composing (false: the engine's code), the live array's `DepthSortParticles` at the commit (the same qsort the composition ran on its copy: identical order), our own per-target vertex cache for the frame (`+0x3b0` untouched: a fallback slot fills the engine's buffer itself); with quads: `CreateMaterial`, `RequestData(+0x450, 1)`, for an eCShaderParticle `CreateImage` + the sampler material's SetImage; the class pool c31..c60 as the op wrote it written back into `eCShaderParticle::ms_pGlobalShaderConstants` (the next op's composition reads it); an already updated target without quads: the engine's code (edge) | composition / commit | [G] 2 k - 12 k inactivity resets / depth sorts per 5 s, masters 611 per 5 s in `rvk_rare` (instanced emitters, compare clean), in-slot simulations up to 1,140 per 5 s (spawned 0: their own OnProcess had run that tick), images created [G], vertex reuse [C] (no master drawn twice in a frame met) |
| strips | `CreateVertices` (false: engine's code), with the visual `RequestData(material, 1)` | composition | [G] strip creates 4 |
| light streaks | with material + illumination: `CreateBuffers` (false: done), `RequestData(+0x70, 1)`; the world c31..c33 written back into eCShaderLightStreaks' pool | composition / commit | [C] (buffers already made by earlier engine frames in every run; the engine draws none) |
| precipitation | nothing (FillVertexBuffer's dirty flag is read only by itself) | - | [G] |

**The engine's pools on lean frames.** The lean ops do not write the engine's global (c31..c231) / lighting (c14..c37) pools, so they
lag. Before engine code runs on a lean frame (an op / unit left to it: `markLeft`), the register file is copied into them (`syncEnginePools`,
one 3.6 KB copy, ~1 per frame at the savegame) and becomes the snapshot: the engine code starts from what lean off would have had and
every register it changes is merged back. The particle / light-streak class pools are written back per op instead (above).

**The c40 carry-over (O1 / O2a, lean off).** `markLeft` returned early while `view_.admin` was null: `viewBegin` resets the view and
`composeView` sets it at the view's first composed op, so an op left to the engine before that (after a teleport: a permutation not yet
compiled) wrote the global pool unseen and its writes never reached the register file (`ST_Bark_07 / 08` c40.x = the instance slot 2
carried from that op). Fixed: `markLeft` no longer needs the view; the lightmap slots come from the last view's admin (`poolAdmin_`).
The lean-off route (`r0_route`, 10.1 M draws) now has 0 mismatches (O2a: 20 of 2.77 M SpeedTree draws).

### The per-op save / restore: kept, its cost removed by a device cache

`native.lean_restore=false` (no restore around a lean op's draws: the engine's caches then lie for the code that still runs; A/B only)
measured 5.46 -> 5.24 ms at the savegame. Most of it was not the restore but its side effect: every op flipped each sampled stage
between the engine's states and the composed ones, so the device's per-slot sampler cache missed every draw (`lookupSamplerStates`:
hash + lock, 0.11 ms profiled). `d3d9::Device` now keeps the last two state rows per slot (`samplerMemo_`, a memcmp hit): restore on
5.34, off 5.25 (the remaining ~0.09 ms are the setters). Dropping the restore safely needs the engine's state caches invalidated
before every engine entry (fallback slots, brackets, effect units, HAL calls): not worth 0.09 ms; the restore stays.

Also found by the profile: the frame tap's attribution formatted a `std::format` note for every lean op ("0 draws, not explained",
~0.15 ms on lean frames; only the first 12 per view are kept): notes are now formatted only while kept, and a lean op is attributed
as such (0 engine draws expected: `native: frame tap` windows clean with lean on).

### Verification (lean on, `native.lean_check=10`, every group taken over + compared; `out/o2b/l1_*.log`, `check.py`)

| run | draws | result |
|---|---|---|
| pub_interior + jungle, actors shown (`l1_spots`) | 4.1 M | every group 0 mismatches in every field class, own draws = composition |
| savegame Risen_Save_0009 hold 30 (`l1_save`) | 4.7 M | 0 |
| `rvk_spells` / `rvk_rare` (validation) / `rvk_pointshadow` | 4.3 M / 11.6 M / 4.9 M | 0; validation 0 |
| storm set / `rvk_menufps` (PASS) | 4.1 M / 4.2 M | 0 |
| the rvkcompare route, all spots (`l1_route`) | 10.5 M | 0 |
| `rvk_rtwalk` / with RT shadows near + `debug.rt_skin_verify` lean on / off (`rt1` / `rt0`, Stormy_Rain, 60 s) | 2.2 M / 6.9 M / 6.1 M | 0; skin verify and `rt: scene:` tree captures equal |
| lean off: the route (`r0_route`) | 10.1 M | 0 (the c40 fix) |
| savegame `-Validation` / `rvk_pointshadow` `-SyncValidation` (no RT shadows; `v1_*`) | 2.6 M / 2.8 M | 0 messages, no SYNC- hazard |

**Behaviour.** Placeholder binds on the route lean on: 265 (load), 41, 59, 32, 13 after teleports, else 0 (O2a lean / off: the same
transients). Particles: `rvk_spells` lean on vs off (`l1_spells` / `s0_spells`): 84,678 / 82,210 particle ops, 16.7 / 16.4 quads per
op, the orientation / space / frame-blend split proportional; the emitters with no live particle at entry are now composed (0 quads)
instead of left. Same-frame switch lean off -> on at 4660 (frozen holds, `switch.ps1`): pub_interior static blocks 0.0062 before /
**0.0061** across / 0.0059 after, jungle 0.0006 / **0.0000** / 0.0000; `shot-reviewer` (sbs `out/o2b/review/`): no texture, lightmap,
tree / leaf / grass, shadow, candle or character change. Burst sequences lean off vs on at harbour_night / hero_idle / pub_idle
(`out/water/20261028-135321`): torches, candles, NPCs, the hero the same (exposure / clock differences only). RT: skin verify and tree
captures equal (above).

**Coverage leftovers** (engine draws per run, `native.coverage`, `cov0_*` lean off / `cov1_*` lean on): savegame 778 -> 578, `rvk_rare`
746 -> 699. Gone: particles "buffers not created yet" (25 / 57) and the "simulated inside the slot" / "master created in the slot" ops
(0-draw ops before, now composed). Left: "permutation not compiled" (the engine compiles inside its slot: 211 static, 132 shadow
views, 129 trees, 48 actors, 30 vegetation, 14 alpha at the savegame) and first-frame cases (sky / cube / particles "unreadable" 2 - 8).

### Perf (savegame, `-Exe RisenTest.exe -Vsync off -Perf`, hold 40, machine idle: `sysprobe` 3 % / nothing else busy; last 6 windows)

| run | engine+HAL ms | GPU ms |
|---|---|---|
| Wrapper (3 runs) | 3.41 / 3.29 / 3.46 | 1.49 - 1.63 |
| Full, lean off (3 runs) | 5.61 / 5.64 / 5.74 | 1.49 - 1.53 |
| Full, lean on (3 runs) | **4.83 / 5.10 / 4.98** | 1.49 - 1.55 |
| profiled Full lean off / on | 6.09 / 5.27; Engine.dll self 1.17 / 0.74, ours 2.89 / 2.62 | |
| jungle (rvkcompare spot hold, last 4 windows): Wrapper / Full lean off / lean on | 3.63 / 6.92 / **5.77** | 1.88 - 1.95 |

Full lean on is 0.69 ms (12 %) cheaper than lean off at the savegame (fps 169 -> 192; Full - Wrapper +2.27 -> +1.58 ms) and 1.15 ms
(17 %) in the jungle, where trees dominate. Steps (same session): O2b groups alone 5.69 -> 5.42; + sampler memo 5.34; + the tap's lazy
notes 4.97. Per frame at the savegame ~470 slots are skipped (main view + shadow views: 152 trees, 109 shadow-view z ops, 137
static, 22 actors, 28 vegetation, 12 particles, ...).

### Left for O2c / O3

* O2c: the shadow mask / PSSM (`UpdatePackedShadowMapVisual` -> return false: the engine's PSSM loop); the "permutation not compiled"
  leftovers (drive the engine's compile of a missing permutation where ExecutePass does it, so the op can be lean in the same frame).
* O3: the effect units / post chain (`effectBegin` 6.7 % of the profiled frame, the units' device capture) and the sprite batcher
  without the engine's ExecuteEffect / Flush; the focus indicators' draws. With most engine code gone, then the restore (cache
  invalidation at the engine's entries) and the tap's copy + resolve.

## O2c / O3 result (2026-10-28): lean effect units, sprite flushes, PSSM, on-demand compiles

Commits 61f86ec (O3: effect units / sprite flushes / PSSM lean), 6f7aee4 (O2c: permutations compiled at the slot's place, warm-up
pool sync), 3f46998 (rvk_d3d9test "load surface", below) on `feat/native-scene`. Logs, scripts, the DLL / PDB of the profiled run:
`out/o3/` (`compare.ps1` + `check.py` compare runs incl. `focus` / `luma0|1`, `switch.ps1`, `batch.ps1` / `perf.ps1` + `out/o2b/table.py`,
`luma.py`). RE: capstone on the test copy's Engine.dll (the Ghidra MCP had Risen.exe open), scratch in `B:\DevDrive\risen-ghidra\optframe\`.

### What runs now on a lean frame

`native.lean` (default on in Full) now also covers the effect units; `native.lean_effects` (Debug | NoAB, default on) is their A/B.
Code: `src/native/effect_ops_lean.cpp` + the units' entries (`effectBegin`, `spriteFlushBegin`, `packedVisual` return "skip"; the
detours in `engine/scene_hooks.cpp` then do not call the original; `UpdatePackedShadowMapVisual`'s detour returns false so
`eCImageFilterShadowMask::OnUpdateOutput` skips its PSSM loop). Still run by the engine: the thin graph walk (`eCImageEffect::Execute`,
AddJobs / ExecuteJobs, the render-target pool, UpdateColorOutput, UpdateVisual, UpdateEffect, Set / ResetRenderTarget), the mask's
OnUpdateOutput around the units (parameter objects, ClearCells, the `+0x298` holder), CFXGui Draw, the GUI device's BeginFrame /
EndFrame (+ Game.dll's entity renderer), `eCSprite::Begin` / `End`.

What the skipped code leaves behind (RE [C], opt-side-effects-frame.md 6 / 7 / 5.5) and how it is reproduced:

| skipped | side effect | reproduced |
|---|---|---|
| ExecuteEffect 0x75af70 (BeginEffectTechnique, passes, ExecuteVisual, EndEffectTechnique) | device: the HAL technique is a render-state block (every render state the passes and `eCGfxVisual::Execute`'s normal setters write reverts at EndTechnique); shaders, declaration, streams, indices, stage textures, sampler states, float / int / bool registers stay as the last pass set them | `issueLean`: our draws, then the render states of the unit's entry (no save / restore of the rest: our last draw's state is the engine's) |
| OnSetParams 0x75c740 / OnResetParams 0x75c8a0 | the API effect's values (Set = SetParameter, pure reads of their sources), each input's AttachImmImageData (AddRef + SetTexture), at the end SetTexture(handle, null) + Release; the Texture / Image parameters' Reset (0x2897f0 / 0x2893d0) the same; every other class's Reset is `xor al, al; ret` | `EffectInfo::adoptValues / adoptTexture / adoptTechnique` (the HAL effect's store = the carry-over the next composition starts from; every unit now starts from it): the composed values, the textures with the inputs' and Texture / Image parameters' nulls, the technique of the effect's `+0x20` (PSSM: the handle UpdateEffect would write); refcounts balanced by skipping both halves |
| `eCGfxEffect::Execute` 0x750210 | `ms_pActiveEffect` = the effect (always); `+0x2c = 0` and `ms_bReset = 0` **only while ms_bOptimize** (false in the game: the first build predicted them, the check frames showed `+0x2c` staying 1); the execute callback 0xc62af0 (none) | ms_pActiveEffect written; ms_bOptimize on or a callback = the unit keeps the engine's code |
| ExecuteEffectPass 0x75b1a0 | gfx effect `+0x28` = the pass | written (the last pass) |
| the PSSM loop (UpdateEffect, Begin, per cell ActivateStencilTesting + 3 passes, End) | the packed visual's `+0x1c` / `+0x20` (cull / depth func per pass), UpdateEffect's `+0x20` technique handle, the HAL's master stencil flag; UpdatePackedShadowMapVisual itself still runs (its vertex buffer is what our draws read) | written; master flags as predicted |
| `eCSprite::Flush` 0x757d10 (RenderBatches, DrawBatch, eCSpriteMesh::Draw) | record count `+0xb0 = 0` (only Flush resets it); the mesh ring `+0xd8` / `+0xdc`; SetBlend's SetMasterBlending (the HAL's master blend flag; its render states revert with the technique); DrawBatch's attach (`+0x100`, one AddRef released by End / EndFrame only when attached: skipped = nothing held); g_mWorldViewProj and the texture in the effect; `+0x88..+0x9c` are re-initialised by every RenderBatches; no RequestData / glyph / atlas work, DrawQuad holds no reference | `+0xb0`, the ring, the master blend flag, the store; our draws from our own buffers, then the engine's mesh buffers bound as the engine leaves them |

A unit keeps the engine's code when a render-state block is open (BeginTechnique would restart it: never met), ms_bOptimize is on or
an execute callback is installed (never), and on check / warm-up frames.

**Verification of the side effects**: on the check frames (`native.lean_check`) the engine's code runs and the prediction is compared
with what it left: the whole device (every float register, all 16 stages, 210 render states, bool / int, target / depth / viewport),
the HAL's master flags, the effect's store (values, textures, technique) and the written engine fields (the PSSM's at the HAL effect's
EndTechnique: its loop has no engine hook). rvk.log `native: effect units lean (5 s): engine code skipped: <unit> N ...; check (total):
after D / N differing (fields), store ..., fields ..., master flags differing M`. The first build's checks found the `+0x2c` /
`ms_bReset` rule above and the PSSM's technique (read from `+0x20` before UpdateEffect had run); after the fixes 0 everywhere.

### O2c: permutations compiled at the slot's place

The "permutation not compiled" leftovers are gone: where the group takes over, the composition calls the engine's own compile at the
point its slot would, with the slot's arguments (RE [C]): a pass with a missing element = the version Execute 0x6cc610's
`FUN_1806cc000(version, (f >> 9) & 1, (f >> 10) & 3)` then `FUN_1806cb8b0(version, f & 3, (f >> 4) & 3, (f >> 6) & 7, (f >> 2) & 3)`
(f = the pass flags; the same calls in the same order); a special pass = `ExecuteSpecialPass`'s version vtable +0x50 = 0x6ca6c0
`(version, admin +0x314, instanced, 1)`, a failure clears `version +0x18 + idx` (as the engine; no draw). Only for
eCShaderVersionDefault_3_0 with the vtable slots and call sites checked at first use (rvk.log `native: on-demand compiles: ...`). The
compile re-walks the shader's graph into its pools (`UpdateShaderConstants`): our copy of the version's shader's pools is re-read, and
the compile now runs before the op's constant switch is applied to that copy (the first build re-read after it and lost the switch:
4 skinned-actor draws in `rvk_spells` differed in a material texture / c6-13). **No compile path calls `SaveShaderPool`** (0x6b6260 has
no direct caller in Engine.dll): a pool miss compiles in memory and marks the user pool dirty, exactly as the engine's own call. The
test copy's `Data/Compiled/Materials/ShaderMaterialPool_User.smp` does not exist before or after the runs of this stage (stat only).
rvk.log `native: lean (5 s)` ends with `permutations compiled at the slot's place N + special M (failed K)` (savegame: 147 + 219 in the
first world frames, then 0; never a failure). Particles (their own shader version) keep the engine's compile: 1 - 2 ops per run.

With no op left to the engine, no lean frame syncs the engine's pools any more (O2b: `markLeft`, ~1 per frame), so a warm-up frame ran
the engine's code from pools many frames old: a carry-over register no slot writes (an instance slot beyond a batch) made 870 route
draws differ in "vs instance c32+" on the check frames (a compare artefact: lean frames never read those pools). The warm-up frame now
starts with `syncEnginePools()`.

**Coverage leftovers** (engine draws per run, `native.coverage`): savegame 578 (O2b) -> **13 - 16**, `rvk_rare` 699 -> **18 - 25**, route
53; every run 99.999 - 100.000 % ours. Left: sky cube faces / particles / billboards "unreadable" in the first frames after a load,
particles "buffers not created yet" (new emitters' first frame), particles "permutation not compiled".

### Verification (lean on, `native.lean_check=10`, every group taken over + compared; `out/o3/c4_*`, `c5*_*`, `b1_*`)

| run | result |
|---|---|
| savegame hold 30 / `-Validation` hold 20 | every group 0 mismatches, own draws = composition, effect draws equal (38 k / 21 k), after 0 / 15,425 and 0 / 8,222, store / fields / masters 0; validation 0 messages |
| `rvk_menufps` (PASS: menu limit, loading pacing) | 0; after 0 / 10,540 (GUI flushes of the menus, character screen, loading screens) |
| pub_interior + jungle (actors), + `upscale.mode=native_aa`, + `quality` (1066x600 -> 1600x900) | 0; after 0 / 9,808, 9,523, 9,693; FSR frames upscaled as before |
| `rvk_pointshadow` / with `-SyncValidation` (no RT shadows) | 0 incl. the PSSM pass (after 0 / 11,280); sync validation 0 messages, no SYNC- |
| `rvk_spells`, `rvk_rare` (validation), storm set, the 17-spot route | 0 (route 10.5 M draws, after 0 / 59,830) |
| focus (`-Param focus=1`, pub_interior + harbour_night, the focus pinned on an NPC) | 0; GUI-view engine draws outside the flushes 0 |

Not run: HDRI (DefaultEffect HDRI needs the copy's ConfigUser.xml edited; its jobs are the same filter classes).

**Focus indicators**: in the focus runs, `rvk_spells` (spells drawn and shot), the route and every compare run the coverage counter met
no engine draw outside the units (`gCFocusCrossHairIndicator2`'s mesh would count there); `gCFocusScreenPointIndicator2` draws through a
sprite Flush (it sets the view matrix to identity first; our composition reads the HAL's view matrix at the Flush), so it is the GUI
unit's. The crosshair indicator's mesh (ranged aiming) was not met; if it occurs it stays the engine's draw (counted, not composed).

**Behaviour.** Same-frame switch `native.lean_effects` off -> on at 4660 (frozen holds, `switch.ps1`): pub_interior (bloom, DoF, tone
map, SSAO / mask) static blocks 0.0213 before / **0.0218** across / 0.0164 after, jungle 0.0005 / **0.0001** / 0.0000; `shot-reviewer`
(x8 diff maps, candle-bloom / sky / dark-corner / DoF crops, `out/o3/review/`): nothing changes across the switch beyond the same-mode
pairs (max 2 levels; the pub's slow sub-level drift is the same before and after). Eye adaptation: the savegame luma traces
lean_effects off / on (`luma.py b1_luma0 b1_luma1`) follow the same curve until the storm's first lightning (-0.45 per 20 frames in
both, offset by the runs' 20-frame timing); the random lightning makes the rest not comparable. The adaptation is GPU-only and its
draws are compared equal per frame (above). Menus / loading screens / HUD: `rvk_menufps` PASS with every GUI flush checked.

### Perf (`-Exe RisenTest.exe -Vsync off -Perf`, test baseline 1600x900; savegame hold 40 last 6 windows, jungle spot last 4; one session, `sysprobe` before: 8.5 % CPU, nothing else busy; Full before = the O2b DLL f3056a2)

| run | savegame engine+HAL ms | jungle ms | GPU ms |
|---|---|---|---|
| Wrapper (3 runs) | 3.35 / 3.35 / 3.38 | 3.71 / 3.69 / 3.72 | 1.47 / 1.83 |
| Full before (O2b) | 4.82 / 4.85 / 4.87 | 5.83 / 5.75 / 5.80 | 1.48 / 1.83 |
| Full after (O3) | **4.73 / 4.75 / 4.79** | **5.72 / 5.68 / 5.66** | 1.49 / 1.83 |
| Full after, `native.lean_effects=false` | 4.90 / 4.93 | | |
| Full after, `native.lean_restore=false` | 4.71 / 4.64 | | |

O3 saves ~0.10 ms at the savegame and ~0.11 ms in the jungle (effects lean vs not, same DLL: 0.17 ms); fps 196 -> 200. Full - Wrapper:
savegame **+1.40 ms** (+1.49 before, same session), jungle +1.98 (+2.08). Profiled (`A_prof` vs `W_prof`): Full 5.06 vs Wrapper 3.55 ms;
by module ours (d3d9.dll) 2.38 vs 0.90 (+1.48), vcruntime (memcpy) 0.47 vs 0.19 (+0.28), **Engine.dll 0.93 vs 1.20 (-0.27: the engine
now does less than in Wrapper)**, the rest equal. So **what is left of Full - Wrapper is our own per-draw cost**, not the engine's: the
composition + issue of the static / scene ops (`StaticOps::opEnter` 2.54 incl, of which `issue` 1.15: the draws' recording) and the
effect units (`effectBegin` 0.42 incl for ~25 units and their ~45 draws). The engine's ExecuteEffect cost less than its share of the
O2b profile suggested.

**The per-op restore** (`native.lean_restore=false`): 0.07 ms (4.68 vs 4.76). Dropping it needs the engine's state caches invalidated
at every engine entry that still follows lean ops (fallback slots; the brackets' EndRenderStateBlock reverts only HAL-set states; the
effect units' composition reads the inherited device state): not worth 0.07 ms, kept.

### Full vs Wrapper per stage (savegame, engine+HAL ms, unprofiled; each row from its own session)

| stage | Wrapper | Full | Full - Wrapper |
|---|---|---|---|
| O0 (c338059) | 3.36 | 8.53 | +5.17 |
| O1 (42e6a12) | 3.33 | 5.42 - 5.48 | +2.15 |
| O2a (lean on; a ~0.3 ms slower session) | 3.29 | 5.74 - 5.76 | +2.46 |
| O2b (f3056a2, lean on by default) | 3.39 | 4.97 | +1.58 |
| O2b in the O3 session | 3.36 | 4.85 | +1.49 |
| **O3 (6f7aee4)** | **3.36** | **4.76** | **+1.40** |

### rvk_d3d9test "load surface from surface" (3f46998)

Pre-existing failure, root-caused in a worktree: `Device::loadSurfaceFromSurface` read a DXT1 source into a CPU texture by
`vkCmdBlitImage` into a temporary render target; on this GPU / driver the blit of an 8x8 DXT1 level yields only a 2x2 patch of the first
block (the source image itself reads back correct, validation clean, the code unchanged since the test passed in 9c468b6: a driver /
environment change [I]). Fix in our device: DXT1-5 sources into a 32-bit BGRA CPU destination with a nearest filter (the sky absorption
image's case) are decoded on the CPU from the read-back level, honouring both rects; other cases keep the blit. `rvk_d3d9test` PASS,
`rvk_tests` 179 / 179.

### Left

* The remaining Full - Wrapper (+1.4 ms) is our own per-draw composition / recording: the "own modes" (retained composition across
  frames, merged indirect draws) are the next lever, not more engine code removal.
* The per-op restore (0.07 ms) and the tap's copy + resolve stay; particles' own permutation compile and first-frame cases stay the
  engine's (13 - 25 draws per run); the crosshair indicator's mesh not met; HDRI not re-run under O3.
