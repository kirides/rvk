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
