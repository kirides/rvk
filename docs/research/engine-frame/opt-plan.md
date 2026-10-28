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
