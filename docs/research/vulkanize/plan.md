# Vulkanize: GPU-resident data and retained draws (plan, 2026-10-28)

Goal (user, 2026-10-28): optimize our own render pipeline and objects: mesh arenas, material information in structured GPU
buffers indexed per draw, minimal CPU <-> GPU traffic. Builds on the Full mode after O1-O3 (engine-frame/opt-plan.md); the
three modes stay (Original / Wrapper / Full); every stage is a Debug A/B switch first, default on once verified.

Facts: V0-draw-path.md (cost, traffic, churn: measured), V0-constants.md (register classes), V0-gpu-resident.md (geometry,
pipelines, device features, consumers). Savegame Risen_Save_0009, 1x, uncapped (O3): Wrapper 3.36 ms, Full 4.76 ms CPU,
GPU 1.48 ms: **CPU-bound**, so the work is CPU work removed, not GPU work rearranged.

## 1. Where the time goes (Full, engine thread, profiled 5.17 ms; V0-draw-path.md)

| item | ms | what removes it |
|---|---|---|
| composition (opEnter minus issue), ~1.6 us x 881 draws | 1.43 | retained composition (V3) |
| device draw path (`drawIndexedPrimitive` / `prepareDraw`), ~1.2 us per draw | 1.08 | retained packets recorded without `prepareDraw` (V3), merged indirect draws (V4) |
| memcpy (constants composed + copied 3x, ~10 MB CPU-side) | 0.55 | split constants (V2), retained (V3) |
| effect units (~25 units, ~28 draws) | 0.41 | retained units (V5) |
| ring: DISCARD reserves the whole buffer (35 MiB / frame, +95 MiB GPU memory) | memory | V1 |

Churn (static camera, 990 frames): states, samplers, geometry ranges, draw keys changed for **0 %** of the composed draws;
78 % differ only in constants; those constants are per-view data (PS c4, c15..c24; VS c0..c11 + fog when moving) copied into
every draw, plus the actors' bone palettes (real per-object data). Content dedup of today's blocks is useless (leftover
registers beyond a batch's count differ).

## 2. Target data model

* **Register classes** (V0-constants.md 1, measured over all 2,483 game shaders: one constant name = one register):
  VS c0..c11 VIEW, c12 / c14..c29 FRAME (c23..c25 the main camera's rotation, also in shadow views), c30 fog VIEW,
  c31..c231 OBJECT, c232..c251 MATERIAL; PS c0..c4 VIEW / fog, c5.xy VIEW + c5.w DRAW (-> the object block), c6..c13
  MATERIAL, c14..c37 LIGHT. Particles / streaks / sky dome have their own pools (tables in V0-constants.md). Int / bool
  constants: never read by any shader.
* **Blocks**: VIEW (per view and pass: main, cascade i, terrain map, atlas cell, refraction...), FRAME (per frame),
  MATERIAL (persistent table: one entry per material instance x version, rewritten only when its values change), OBJECT
  (per object instance and view kind: world, fade, instances, bones; reused across z pass / solid / cascades where equal),
  LIGHT (per illumination x pass, reused by every draw that shares it), DrawBlock (textures / samplers / alpha: persistent
  per op in Full).
* **Shader side**: the translator reads `c[n]` of each class through that class's block pointer, offset so the register
  numbers stay (`block - first * 16`). In Wrapper every block pointer = the draw's StageConsts: one SPIR-V for both modes.
  Relative addressing stays inside one class (V0-constants.md 2).
* **Per-draw record** (GPU buffer): the block addresses + DrawBlock + prev VS block, ~64-96 B. Push constants carry the
  record (direct draws) or the record table base (indirect draws: `gl_DrawID` + base -> record; the vertex shader passes the
  index to the fragment shader flat). FrameBlock stays as it is.

## 3. Stages

Each stage: built, game-verified (the native compare `mismatches: none` with `native.lean_check`, `-Validation`, a
`-SyncValidation` run, savegame + jungle perf A/B, spells / rare / point shadows / route where touched), committed, documented
(a "Vn result" section here, performance.md, CLAUDE.md switches).

**V1: cheap fixes, no architecture change** (Wrapper and Full)
* DISCARD locks reserve only what is written (lazy reservation at unlock / first write extent; `resources.cpp:423-443`,
  `static_ops.cpp:1850-1871`): ring 35 -> ~8 MiB per frame, -95 MiB GPU memory.
* No int / bool upload for shaders that declare none (all of them today; keep the layout, copy nothing).
* Setters in Full mark a stage dirty only when a register really changed (cheap compare where the composition writes).
* Optional features `shaderDrawParameters` + `drawIndirectCount` enabled when present (needed by V4); check the overlay
  still draws (enabling DGC once hid it: performance.md 12).

**V2: split constants** (Wrapper and Full; done 2026-10-28, section 6)
* Translator: per-class block reads (above), `kTranslatorVersion` bump, ABI (abi.h + rvk_abi.glsl + the layout test) gets
  the record; `rvk_shadertool corpus` + `compare_disasm` stay 100 %; the shader equivalence tools re-run.
* Device: per-class dirty tracking; a class block is uploaded only when its registers changed since its last upload, else
  the previous block's address is reused (Wrapper gains too: the view / material / light blocks repeat between draws).
* Consumers that read the CPU register file at draw time (water, rt_scene, gi, shadows, rt_material, object_motion) keep
  working: the register file is still filled.
* Registers a shader reads but the engine never wrote for that draw (carried over) must keep their values: the compare
  proves it.

**V3: retained composition** (Full)
* Per op: the composed draw (pipeline, vertex input, geometry, DrawBlock, material entry) kept across frames, keyed by op
  identity + the inputs the composition reads (material instance / version, mesh, LOD, lights generation...). Per frame only
  the OBJECT data of objects that moved / animate and the VIEW / LIGHT blocks are written.
* Retained packets are recorded without `prepareDraw` (pre-resolved pipeline handle, vertex binding, record address).
* Consumers get an op-level feed (object identity + per-object data, sighting per view) instead of `prepareDraw`'s
  observers; until a consumer implements it, the draws it observes take today's path (player defaults have water + RT on:
  the feed is needed for the gain there).
* Check: `native.lean_check` frames compose fresh and compare field by field with the retained entry (any difference = a
  missing invalidation key, logged); plus the existing engine compare.
* Switch `native.retained` (Debug).

**V4: merged indirect draws** (Full; batchable passes only)
* Today's batches (opaque z / solid / shadow depth, grouped by pipeline) become `vkCmdDrawIndexedIndirect` per group of
  equal (pipeline, dynamic state, vertex input, bindings, index type); records via `gl_DrawID`; vegetation made batchable
  (instance cursor aligned to its stride). Flush boundaries stay (coplanar terrain layers / decals keep their order).
* Indirect commands + records persistent where the retained set did not change.
* Switch `native.indirect` (Debug).

**V5: effect units and GUI retained** (Full): composed effect / post / sprite draws kept per unit; per frame only the
values that change.

**Later (GPU not the bottleneck today):** GPU culling, CPU-built geometry (particles, grass instances, sprites) generated on
the GPU, vertex pulling, the ubershader at run time (fewer pipelines), mesh shaders.

## 4. Rules

* Parity first: the engine's translated shaders stay; the compare must stay `mismatches: none`; every new cache has a
  check mode that recomposes and compares.
* Wrapper stays the D3D9 model: V1 / V2 may make it faster, never different.
* Floor unchanged (Vulkan 1.3 + push descriptor): new features optional with today's path as the fallback.

## 5. V1 result (2026-10-28)

Built and verified; commit "V1: ring blocks of only the bytes needed ...". Measured at savegame Risen_Save_0009 (`rvk_perfsave`
hold 40, `-Exe RisenTest.exe -Vsync off -Perf`, 3 runs each, last 6 `perf:` windows, `out/v1/summ.py`) and the held `jungle` spot
(1 run each); before = build of 4906cba, after = this change.

| run | CPU ms before -> after | fps p50 | ring KiB / frame | GPU allocations MiB |
|---|---|---|---|---|
| Wrapper, savegame | 3.35 -> 3.31 | 270.9 -> 274.2 | 7,879 -> 7,120 | 600 -> 600 |
| Full, savegame | 4.70 -> 4.66 | 194.5 -> 195.2 | 36,344 -> 8,131 | 695 / 663 / 695 -> 599 |
| Wrapper, jungle | 3.91 -> 3.91 | 232.6 -> 233.1 | 11,435 -> 11,117 | 590 -> 590 |
| Full, jungle | 5.88 -> 5.60 | 157.3 -> 163.2 | 124,763 -> 13,229 | 724 -> 596 |

* **Ring reservation (kept)**: `d3d9::RingBlock` (identity.*): a dynamic buffer's frame copy is a ring block of the written
  extent [lo, hi); a Patch outside it (NOOVERWRITE append) moves to a new block with the size doubled; `Frames::allocate`
  got `minOffset` so the binding (block offset - lo) stays valid. Full: 36 -> 8 MiB per frame (jungle 125 -> 13), GPU
  allocations -96 MiB / -128 MiB. Wrapper only -10 % (its dynamic buffers are small). `static_ops.cpp`'s own buffers already
  lock DISCARD ranges, so they profit with no change there. Unit tests in `draws_tests`.
* **Int / bool constants (kept)**: `Reflection::intBoolConstants` (the translator sets it when an `i#` / `b#` read without
  defi / defb is emitted): no game shader does, so the 320-byte copy is skipped for every draw (`perf: stage constants`:
  int / bool skipped in ~all blocks; ~1.4 - 2.6 MiB less copying per frame). Layout unchanged.
* **Changed-only register writes (kept, `debug.constant_compare`, default on, Debug)**: on a clean stage the written range is
  compared first; Full counts 724 - 1,975 unchanged writes per frame that no longer trigger a new block (Wrapper ~130).
  CPU gain is small (Full jungle -0.3 ms, savegame -0.04 ms: within the 1 % run noise), kept because it is cheap and V2 needs
  the same dirty tracking.
* **shaderDrawParameters + drawIndirectCount**: enabled when the device has them; caps lines `Shader draw parameters` /
  `Indirect draw count`, rvk.log `vulkan: device ... shaderDrawParameters=, drawIndirectCount=, multiDrawIndirect=`. The
  ImGui overlay still draws (screenshot in `out/v1/overlay_2.png`).
* **Verification**: rvk_tests 181/181, rvk_d3d9test, rvk_vktest, `--passes`, shader corpus 2,483 OK; Full compare (all groups,
  `native.lean_check=10`) at savegame, spots, 17-spot route, spells, point shadow, rare, storm: `mismatches: none` everywhere,
  0 differing own draws; `-Validation` (savegame, rare) and `-SyncValidation` (point shadow) 0 messages. Wrapper refcompare vs
  `20261022-093252`: pub_interior, jungle, beach_rain PASS; harbour_night failed once (dluma -3.3) and passed twice on re-run
  (+0.6 / +0.2; the previous build +0.4): night-spot noise.
* **Honest finding**: V1 is a memory / traffic win (Full ring -78 %, memory -96 to -128 MiB), not a CPU win: CPU stays within
  ~1 - 5 % (best Full jungle). The CPU cost is in the per-draw composition and constant copies that V2 / V3 address.

## 6. V2 result (2026-10-28)

Built and verified; commit ae10596 "V2: split constants ...". Code: `src/shader/abi.h`, `glsl.cpp`, `d3d9/draw.cpp`
(`uploadConstants` / `splitStage` / `verifySplit`), `d3d9/device.cpp` (setters). Measured as in section 5 (savegame
Risen_Save_0009 `rvk_perfsave` hold 40, `-Exe RisenTest.exe -Vsync off -Perf`, 3 runs, last 6 `perf:` windows,
`out/v2/summ` via `out/v1/summ.py`; jungle 1 run each); before = V1 build 7109548 (`build/V1base`), after = V2.

**Design (confirmed)**
* A stage's float registers are partitioned into 4 windows **by register number** (not by CTAB name): VS c0-11 view,
  c12-30 frame (wind, app-camera rotation, fog), c31-231 object, c232-255 material; PS c0-4 view / fog, c5 (a new `vec4 psC5`
  in the DrawBlock: xy view + w per-draw alpha ref; DrawBlock 192 -> 208 B), c6-13 material, c14-223 light.
* Translated shaders read float `c[n]` through the macros `RVK_VSC` / `RVK_PSC` = `pc.vsw[win(n)].c[n]` / `pc.psw[..]`; push
  constants 48 -> 112 B (`vsWin[4]`, `psWin[4]`). Window pointers are **biased** (block base - firstReg * 16), so register
  numbers stay. Legacy draws set every window to the one full StageConsts, so one SPIR-V serves both paths.
* Relative addressing: the translator marks the base register's whole window (generic `rvk_cRel` selects the window); windows
  a shader does not read point to a zeroed 4 KiB ring block. `Reflection::windowEnd[4]` = highest read + 1 per window;
  `splitConsts` is false for hand-made reflections and shaders reading `i#` / `b#`.
* Device: register setters set per-window dirty bits (the memcmp compare now also runs on an already dirty stage when split is
  on). A window block is written to the ring only when dirty, never uploaded, or uploaded shorter than the shader reads; extents
  = what the shader reads (enhancement variants that read ambient c15..21 / indoor get window 3 extended). Ring memory is per
  frame: all windows dirty at `beginFrame`.
* The legacy full block stays for hand-written GLSL (water, ocean ..., they read `pc.vs` / `pc.ps`), the GLSL ubershader
  (`debug.ubershader`), replacers with their own DrawBlock and `i#` / `b#` readers. `kTranslatorVersion` 3 -> 4; the variant
  text `rvk_material_variant.glsl` reads `pc.psw[3]`; tools (shadereq, vktest) call `PushConstants::legacyWindows()`.
* Consumers: the CPU register file is still filled by every setter, so water, rt_scene, gi, shadows, rt_material,
  object_motion, rt_skin read it unchanged. Nothing reads the GPU StageConsts address (`draws::Record` vsConsts / psConsts now
  hold the object / light window address of split draws, only logged); `prevVsConsts` in the push is a capture address, untouched.
* Settings: `debug.split_constants` (Debug, default on; off = V1 upload behaviour incl. full blocks), `debug.split_verify`
  (Debug, default off: after each split upload every window block the shader reads is compared with the CPU register file;
  rvk.log `d3d9: split constants: ... differs`, `verify mismatches` in the perf line). Perf line `perf: split constants`
  (blocks written + reused per frame: VS view / frame / object / material, PS view / material / light; draws split / legacy;
  verify mismatches). The V1 counter `unchanged register writes` now also counts writes on dirty stages.

**Numbers**

| run | CPU ms before -> after | fps p50 | ring KiB / frame | constants written KiB / frame |
|---|---|---|---|---|
| Wrapper, savegame | 3.31 / 3.76 / 3.31 -> 3.33 / 3.35 / 3.38 (V1 mean ~3.46) | ~274 -> ~270 | 7,117 -> 2,789 | 2,488-2,516 -> 1,630-1,656 |
| Full, savegame | 4.67 / 4.65 / 4.61 -> 4.74 / 4.72 / 4.71 (+0.07, +1.5 %) | 196.8 -> 192.0 | 8,126 -> 2,773 | 2,607 -> 1,728 |
| Wrapper, jungle | 3.90 -> 3.94 | | 11,139 -> 3,772 | 4,138 -> 2,424 |
| Full, jungle | 5.63 -> 5.67 | | 13,215 -> 4,148 | 4,344 -> 2,690 |

GPU time unchanged (savegame 1.48 -> 1.49-1.50 ms). Blocks per frame (Full savegame, V2, written + reused): VS view 56 + 859,
frame 6 + 482, object 777 + 110 (1,619 KiB = ~94 % of the VS bytes), material 164 + 11; PS view 22 + 618, material 191 + 262,
light 478 + 47 (105 KiB). Draws split 916 / 916 on both stages.

**Verification**
* rvk_tests 182/182 (new `const_windows_match_glsl_macros`; the layout test extended), rvk_d3d9test, rvk_vktest + `--passes`
  (PASS, 0 validation messages), shader corpus 2,483 OK, `compare_disasm` 100 % (2,483 of 2,483).
* Game: Wrapper at the savegame and Full with `debug.split_verify=true`: 0 verify mismatches (1,000+ split draws per frame).
  Full native compare (`native.scene_ops_compare`, `static_ops_compare`, `lean_check=10`, split_verify on): savegame, spots
  pub_interior + jungle, 17-spot route, spells, point shadow (also under `-SyncValidation`), rare (`-Validation`), storm set,
  focus, savegame under `-Validation`: `mismatches: none`, 0 differing own draws, 0 validation messages, 0 errors. One
  transient `particles: vs instance c32+ 1` mismatch at the very start of one spots run (first 7 compared draws, "permutation
  not compiled"), never in 9 later runs: judged a startup race (inferred).
* Wrapper refcompare (6 spots vs `out/refcompare/20261022-093252`, DLL hash of the snapshot = deployed build): beach_rain,
  jungle, gui_inventory PASS; hero_closeup, pub_interior WARN (pub_interior dluma -1.3..-1.5 also with
  `debug.split_constants=false`: not from V2); harbour_night FAIL on dluma -4.2 (V1 failed it once too: night noise). Split on
  vs off vs on vs on (noise floor) give the same differences (jungle ssim 0.89-0.91, hero_closeup bad32 1-2 % both): no
  measurable image change.
* Enhancements on (water + RT reflections + enhanced shadows with `shadows.rt=near` + GI), Wrapper pub_interior / jungle, split
  on: screenshots correct (viewed), verify mismatches 0.
* **Finding (pre-existing, not V2)**: the same enhancement combination at refcompare jungle ends in `VK_ERROR_DEVICE_LOST`
  with split constants **off** and with the V1 build (2 of 2 runs each), while split on completed 2 of 2; cause not
  investigated (the ring traffic cut may be involved), so there is no off / on image A/B for the enhancements. Open: a Full-mode
  enhancement run was not done.

**Honest result**: bytes and ring traffic -60 to -65 %, GPU memory unchanged, **CPU no gain** (within noise, ~+1 % in Full: the
always-on memcmp compare + window bookkeeping about cancels the smaller copies). V1's conclusion stands: the CPU is in the
per-draw composition (V3).

**Dropped / not done**: persistent MATERIAL / LIGHT tables and OBJECT dedup (no CPU gain to expect before V3). The object
window of relative-addressing shaders (instanced, skinned, SpeedTree) still uploads the whole c31..c231 window (3.2 KB);
trimming needs the instance / bone count from the composition (V3).

**What V3 can build on**: per-window blocks with dirty bits are the retained-block structure: a retained op keeps its object /
material / light window blocks (persistent addresses) and the per-draw record can be the 8 window addresses + DrawBlock. The
`psC5` trick shows how per-draw scalars of a shared window move into the DrawBlock. `debug.split_verify` is the check mode for
retained blocks (recompose and compare against the register file); the Full compare / `lean_check` frames remain the parity proof.

## 7. V3 result (2026-10-28, partial)

Commit "V3: retained composition of sub-mesh draws". `native.retained` (Debug, **default off**), `native.retained_check`,
`native.retained_groups` (default 0x2ff: vegetation excluded). Code: `src/native/static_ops_retained.{h,cpp}`.

* Design: `materialDraws` / `specialPassDraw` (shared by static meshes, alpha meshes, actors, trees, shadow views) are memoized
  by key (arguments, geometry, fade alpha, lightmaps, view kind / cascade, sampler snapshot hash) + a **read log** of every
  engine-memory read of the first composition (`get` / `rd`), validated each frame; replay redoes switch writes, lighting,
  mask ref, `constantsInto`, water tag, image touches. No hand-written invalidation keys.
* Parity: Full compare at the savegame with `retained_check` + `lean_check=10`: all groups `mismatches: none`, stale 0 over
  75,142 checked replays.
* Perf (savegame, 3 runs each, Full, retained on vs off): CPU 4.83 vs 4.87 ms (-1 %, noise level); jungle 6.27 vs 6.04 ms
  (worse: vegetation keys churn, 82k rebuilds / 5 s, hence excluded). Replay costs 0.46 ms/frame, i.e. nearly what the composition
  it replaces cost: validation + lighting + constants dominate. **No real gain; stays default off.**
* NOT done: device-path replay without `prepareDraw`, persistent window blocks, validation runs (-Validation / -SyncValidation),
  the other compare scenarios, enhancements-on runs / DEVICE_LOST investigation, screenshot A/B.

### 7.1 Profile and decision (2026-10-28)

Measured (`out/v3p/`): savegame Risen_Save_0009 hold 40, Full, `RisenTest.exe`, `-Vsync off -Perf -CpuProfile`, lod 1x, one run
each (Pon = `native.retained` on, Poff = off), aggregated with `tools/pcprofile.py`. Percentages are of the engine thread's
samples; the thread takes ~4.8 - 5.2 ms per frame.

* Module shares, Pon / Poff: d3d9 (ours) 43.8 / 47.4 %, Engine.dll 17.2 / 17.9, vcruntime140 12.4 / 9.4 (memcpy / memcmp),
  ntdll 9.3 / 8.3, sharedbase 7.7 / 7.5, amdvlk64 4.6 / 4.5. Retention takes ~3.6 points out of d3d9 and puts ~3 points into
  vcruntime (the replay's memcpy / memcmp / constant copies): net zero.
* Inclusive (stack samples), same in both runs unless noted: detourSceneUpdate 60 %, detourExecute 41 % (Poff 46), opEnter 37 %
  (composition + issue), issue() 16.7 %, drawIndexedPrimitive 14.7 %, prepareDraw 10.4 %, composeTree 5.9 %, materialDraws 6.4 %
  (off; = materialDrawsImpl 6.3) vs 5.6 % retained on. The part retention can replace (the sub-mesh draw groups) is ~0.33 ms in
  the unretained build; retained on it still costs 5.6 % (-0.8 points = ~0.04 ms).
* Replay by its own timer: 0.43 ms per frame for ~754 hits (~0.57 us per hit, includes two `steady_clock::now` per hit under
  `-Perf`); hit rate 100 %, built 0, invalid 0 once warm. Leaf costs (share of thread): validate + hashBytes ~1.0 % (~0.05 ms:
  validation is cheap, a cheaper validation has nothing to give), key build + mix hash 0.56 %, `retained_` map find 0.41 %,
  retainedRun 0.41 %, retReplay 0.31 %.
* Leaf profile: our module's top 120 buckets are only 15.6 % (on) / 16.4 % (off) of the thread and flat: safeLoad8/4 1.4/0.7
  (on) vs 1.9/0.9 (off), computeVertexInput ~1.0, prepareDraw 0.6, issue 0.6, sky billboard vertices 0.5, splitStage 0.26;
  nothing above ~1.4 %. constantsInto + forEachRun ~0.27 %.
* Reading: the V0 estimate "composition 1.43 ms" (opEnter minus issue) is mostly **not** the sub-mesh draw groups
  (materialDraws is 0.33 ms); the rest is composeTree's other parts, actors / trees / shadow views / lean bookkeeping, spread
  over many functions. Retaining materialDraws alone has a ceiling of ~0.3 ms, and replay costs about what composing cost,
  because composition mostly consisted of the per-frame redo (switch writes, lighting, mask reference, constants, tag, touches),
  not of the engine reads.
* Remaining time per the profile: issue / device draw path ~0.8 ms (drawIndexedPrimitive 14.7 % incl. prepareDraw 10.4 %: flat,
  many small items; a replay path skipping prepareDraw's classification could save perhaps 0.2 - 0.3 ms but must keep feeding
  the RT scene sightings, object_motion, rt_skin, water / shadow replacers, G-buffer / material variants, tint, coverage:
  V0-gpu-resident.md 7), engine + vcruntime memcpy (~0.5 - 0.6 ms), ntdll, driver 4.6 %.

**Decision.** Step 2 (cheaper validation: ~0.05 ms) and step 3 (skip the per-frame redo: ~0.3 ms together with everything else in
materialDraws, below the 0.15 ms gain bar once key / lookup / replay costs remain) were **not built**. Step 4 (replay without
`prepareDraw`) is the only item with a possible gain (<= ~0.3 ms of 4.8, ~5 %) at high risk to the consumers: not done.
`native.retained` stays default off; the layer is kept as the check-mode-proven foundation (stale 0 over 75,142 checked hits).

Proposed instead: (a) a flat-profile attack on the device path (prepareDraw classification caches keyed on the DrawDesc:
computeVertexInput, splitStage, internVi, prepareTextures); (b) find the memcpy / memcmp callers in vcruntime (12 % = ~0.55 ms;
needs a profiler that symbolizes the CRT's callers, e.g. caller-of-memcpy sampling); (c) V4 merged indirect draws only if it
removes per-draw CPU in prepareDraw, otherwise skip; (d) Wrapper / Full CPU is 4.8 ms vs GPU 1.5 ms: the engine's own code
(17 % Engine.dll + 8 % sharedbase) is the other big block.

## 8. V4 result: per-draw device path (2026-10-29)

Commits: 09be2c5 (profiler), c8e1f86 (composition), e00d888 (device path), 6442f98 (effect passes), 9de3a21 (lod diagnostics). No new
switches (every change is behaviour-neutral; the compare tools are the check). Merged indirect draws (the old V4 idea) were **not built**:
see 8.4. Tools: the sampler's new `diag: profile site / site2 / crt` lines and `tools/pcsites.py` (call sites inclusive of the CRT /
ntdll / driver calls they make, CRT callers, own functions symbolized with llvm-symbolizer; `tools/pcprofile.py` for the line-level
self time). Logs, scripts and numbers: `out/v4/` (git-ignored).

### 8.1 Profile (base = 38c4081 + profiler, `build/V4base`)

Savegame `Risen_Save_0009`, `rvk_perfsave` hold 40, `-Exe RisenTest.exe -Vsync off -Perf -CpuProfile`, 9 windows of 5 s, shares of the
engine thread (Full ~5.0, Wrapper ~3.5 ms per frame; 1 % = ~0.05 ms). Still flat: no item above ~2.4 %.

| module | Full | Wrapper |
|---|---|---|
| d3d9.dll (ours) | 45.4 | 23.8 |
| Engine.dll | 18.2 | 33.5 |
| ntdll | 10.0 (1.5 of it `RtlQueryPerformanceCounter` = the `-Perf` timers) | 13.5 |
| vcruntime140 (memcpy / memcmp / memmove) | 9.1 | 5.4 |
| sharedbase | 7.5 | 10.9 |
| amdvlk64 | 4.5 | 6.0 |

**memcpy / memcmp callers** (first frame outside the CRT; % of the thread; the profiler could not name them before):

| caller | Full | Wrapper |
|---|---|---|
| `safeRead` (engine reads: vegetation instance sources, tree `rd`, HAL primitive) | 1.16 | - |
| `setVertexShaderConstantF` (compare + copy, 2 call sites) / PS | 1.05 / 0.34 | 0.37 / 0.31 |
| `constantsInto` run copies (mirror -> DrawDesc) | 0.88 | - |
| `splitStage` (window blocks into the ring) | 0.59 | 0.80 |
| `Buffer::gpu` (dynamic buffer ring copy: real data) | 0.56 | 0.71 |
| `prepareDraw` (pipeline description compare / copy, 512 B) | 0.47 | 0.80 |
| `internDyn` / `internVi` / `findPipeline` / `computeVertexInput` | 0.44 / 0.13 / 0.14 / 0.23 | 0.63 / 0.27 / 0.18 / 0.46 |
| `issue` save / restore runs | 0.41 + 0.31 + 0.17 | - |
| vector<byte> copies / zeroing (`std::copy`, `_Zero_range`) | 0.37 + 0.27 | - |
| `newDraw` clears | 0.20 | - |
| total | ~8.8 (0.43 ms) | ~5.3 |

**Line level in the device path** (`tools/pcprofile.py`, self): `vk::PipelineDesc pd{}` in `prepareDraw` (pipeline.h:65 +
draw.cpp:977) 1.41 % Full / 2.1 % Wrapper (member-wise initialisation of 512 bytes with non-zero blend defaults, plus
`__builtin_array_init_helper` 0.25); `computeVertexInput` line 330 (the 64-bit `%` and `/` of the stream shift) 0.4; `internVi`
compare 0.26 / 0.37; `prepareTextures` 0.23; `wantSecondaryScope` 0.21; `setRenderState` 0.17; `Frames::allocate` 0.15;
`syncPhaseRegions` 0.25. Composition (Full only): `safeLoad8/4/1/2` 3.1 % self (cache misses on engine memory; one SEH call
each), `hashBytes` 0.6 (retained keys while `native.retained` is off), the three pointer-keyed maps 0.9, `newDraw` + array
inits 0.9, `textureOfImageData` 0.7, `skyBillboardVertices` 0.6. Allocation: operator new 1.1 % + malloc / free ~1.1 % in Full
(`vector<vector<byte>>` = `ParameterValues` copies of the effect units, `vector<float>` = effect constants, the LOD
diagnostics' `std::set::emplace`), sync: `std::mutex` in `closeChunk` 0.7 % (worker contention), `NtAlertThreadByThreadId` 0.7
(worker wake-ups), the engine thread's own `vkCmd*` through `applyPacket` for the non-batched passes ~2.3 % (amdvlk64 4.5 % in all).

### 8.2 Fixes (each a commit; kept = measured or profile-proven)

| fix | evidence | |
|---|---|---|
| blank `PipelineDesc` copy (`kBlankPipelineDesc`) instead of `pd{}` | 1.4 / 2.1 % self | kept |
| `sameDesc`: 52-byte compare when EDS3 + dynamic vertex input make the tail constant; inline SSE2 `memEqual` in `internVi` / `internDyn` / `findPipeline` (`d3d9/memeq.h`) | CRT calls of these 1.0 / 2.1 % | kept |
| VS / PS constant setters: first / last differing register inline (`diffRegisters`), copy and window-dirty only that span | setters 1.4 % CRT in Full | kept |
| 32-bit stream-shift division | 0.4 % | kept |
| `syncPhaseRegions` early exit on `PhaseTracker::generation()` | 0.25 % | kept |
| composition: `snapHash` lazy (retained keys only), 16-entry pointer memos in front of `poolsOf` / `liveOf` / `shaderOfElement`, `newDraw` memsets | 0.6 + 0.9 + 0.9 % | kept |
| effect passes: `Constant::paramIndexCache` (string compare per constant per pass), VM / constant registers and `ParameterValues` copies reuse capacity | allocation ~2 % in Full; Wrapper vcruntime 5.4 -> 3.1 % | kept |
| LOD diagnostics: find before emplace, per-thread seen list (no node, no mutex per call) | small | kept |

**Numbers** (savegame hold 40, `-Exe RisenTest.exe -Vsync off -Perf`, 3 interleaved runs each; jungle = held `rvkcompare` spot,
1 run; base `build/V4base` vs the final build `build/S5` = HEAD 9de3a21; the machine drifts between batches by up to 0.15 ms, only
interleaved pairs are comparable):

| run | CPU ms before -> after | fps p50 | GPU ms |
|---|---|---|---|
| Full, savegame | 5.03 -> 4.95 (runs 5.02 / 5.04 / 5.02 -> 4.91 / 4.88 / 5.06; median -0.11) | 182.9 -> 184.4 | 1.52 -> 1.52 |
| Wrapper, savegame | 3.45 -> 3.38 (3.44 / 3.46 / 3.44 -> 3.36 / 3.40 / 3.38) | 263.1 -> 268.4 | 1.50 -> 1.49 |
| Full, jungle | 6.05 -> 5.92 | 148.6 -> 152.8 | 1.82 -> 1.88 |
| Wrapper, jungle | 4.08 -> 3.95 | 226.0 -> 231.1 | 1.82 -> 1.85 |

Earlier batches of the same series (all vs V4base, interleaved): S1 (pipeline copy, memEqual, setters, maps, `snapHash`, `newDraw`)
Full 4.90 -> 4.81, Wrapper 3.37 -> 3.34, jungle 6.05 -> 5.97 / 3.94 -> 3.92; S3 (+ division, phase generation, `sameDesc`)
Full 4.81, Wrapper 3.30, jungle Full 5.80; S4 (+ effects, lod) vs V4base in one batch Full 5.16 -> 4.95, Wrapper 3.52 -> 3.47.
Profile after (S5 vs base): vcruntime140 9.1 -> 7.5 % (Full), 5.4 -> 3.1 % (Wrapper). **Total: ~2 % of the engine-thread CPU**
(-0.08 ms Full, -0.07 ms Wrapper, -0.13 ms at the jungle in both): real but small, as the flat profile predicted; individual fixes are
below the +-0.05 ms noise, so they were measured in groups and are justified by their profile shares (the instructions' "clearly
reduces memcpy" clause: Wrapper vcruntime -2.3 points).

### 8.3 Verification

* rvk_tests 187/187 (HEAD), rvk_d3d9test, `rvk_vktest --passes` (0 validation errors).
* Full compare (`native.scene_ops_compare`, `static_ops_compare`, `lean_check=10`, `debug.split_verify`) on the final build
  (`out/v4/c3_*`): spots pub_interior + jungle, spells, point shadow, rare (`-Validation`), storm set, focus, menufps, savegame
  under `-Validation`, point shadow under `-SyncValidation`, the 17-spot route (22 checks PASS, 7.8 M draws, 3.06 M own draws
  checked): every group `mismatches: none`, 0 differing own draws, 0 validation messages, effect draws (31 unit kinds,
  117,312 compared on the route) `mismatches: none`, `split_verify` 0 mismatches. (The first route run under the 890 s scenario
  deadline stopped at lava_cave; the later one ran with a 2,700 s deadline.)
* Wrapper refcompare, 6 spots vs `out/refcompare/20261022-093252` (snapshot DLL hash = the build): beach_rain, jungle PASS;
  pub_interior WARN dluma -1.05 (V4base -1.00; V2 saw -1.3 .. -1.5 too: not from V4), gui_inventory WARN (ssim 0.978),
  hero_closeup WARN bad32 0.23 % (V4base FAIL bad32 1.84 % in its run: varies run to run, as in V2), harbour_night FAIL dluma -3.4 once
  and PASS twice on re-run (+0.48 / +0.23; V4base +0.58): night noise as in V1 / V2. No measurable image change.
* Enhancements on (`water.enabled`, `water.reflections=ssr`, `rt.reflections=medium`, `shadows.enabled`, `gi.enabled`; spots
  pub_interior + jungle, screenshots viewed): Wrapper and Full on the final build, no device loss, correct pictures.
* **`shadows.rt=near` -> `VK_ERROR_DEVICE_LOST` is pre-existing, not V4**: with it, all of {Wrapper V4base, Wrapper final, Full
  final} at pub_interior / jungle and {V4base, final} at harbour_docks lose the device within ~50 s (5,400 error lines each); without
  `shadows.rt` the same combination runs clean in both modes. (This explains the V2 note: the loss followed the RT shadows, not split
  constants.) Not investigated further (the shadow-fix agent has the reproduction: out/shadowfix/rvk_vwrt*.log).

### 8.4 Merged indirect draws: not built

The engine thread's driver time is amdvlk64 4.5 % (Full) / 6.0 % (Wrapper) = 0.2 ms: the inline `vkCmd*` of the non-batched passes
(applyPacket 2.3 %: alpha, post, GUI, sky, mask draws, which merging would not cover: their state changes between draws), clears 0.6,
image transitions 0.55, `beginRendering` 0.34, blits 0.2. The batchable passes' ~94 % of the packets are recorded on the two record
threads, off the engine thread; the engine thread only builds the packets (`prepareDraw`, now ~0.7 us per draw) and notifies
(`closeChunk` 0.7 % mutex + 0.7 % wake-ups = ~0.07 ms). Indirect draws would not remove `prepareDraw`'s per-draw work (the record
needs its pipeline, push block and dynamic state first), so the engine-thread gain would be below the noise; GPU time is 1.5 ms vs 4.9
ms CPU. Not worth building.

### 8.5 Left (each below ~0.05 ms of the thread)

`closeChunk` mutex contention with the workers (~0.035 ms; a lock-free chunk queue), SEH-guarded engine reads (`safeLoad8/4/1/2` 3 %:
the cost is cache misses on engine memory, fewer reads need composition-level caching = V3's result), `textureOfImageData` 0.7 %,
`skyBillboardVertices` 0.6 %, `computeVertexInput` stream checks (~1.3 % self), `prepareTextures` + the DrawBlock texture loop
(0.4 + 0.3 %: two walks over the same Texture objects), `Frames::allocate` (0.4 %, ~6 per draw), the draw record (`Record::Record`),
`-Perf`'s own `steady_clock` calls (1.5 - 2 % of the thread under `-Perf`, absent in normal play). A draw-state fingerprint fast path
(skip pipeline / dynamic-state building when nothing but constants changed since the previous draw) is the one structural idea left
for the device path: projected <= ~0.1 ms for a change with many invalidation sources (every setter), not attempted.
