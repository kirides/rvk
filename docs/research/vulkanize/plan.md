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
