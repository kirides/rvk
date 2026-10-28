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

**V2: split constants** (Wrapper and Full)
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
