# Why Full's main solid colour pass costs 1.5-3x Wrapper's (WS41, 2026-11-17)

Question (user): in Full the main solid colour pass costs 1.5-3.2x Wrapper's GPU time in every lit scene (WS39, shader-isa.md 2) and Full "issues ~3x the draws". Is that reasonable?
Answer: **no draw / geometry cost is involved; the whole gap is the per-pixel cost of the light-table prologue (`rvkMvCluster`) in the material pixel shaders.** Both modes shade
the same fragments; with the light table removed from the material shaders Full's solid pass costs what Wrapper's costs (or less).

Machine / method: RX 7900 XTX, `RisenTest.exe`, 1600x900 (the window of the private copy; WS39's 720p numbers are ~0.64x of these), vsync off, `-PlayerDefaults`, `RVK_PERF_WINDOW_ORDINAL=1`,
`perf-ab.ps1` sessions (one session per mode and scene, 2 windows x 120 frames, ordinal 2500 frames lead; shader-ablation sessions 20000 frames lead after the variant queue reached 0),
`debug.record_threads=false` (the statistics queries need inline scopes), private copy `risen-test-vk-ws41`, scratch `B:\DevDrive\rvk-work\scratch\ws41` (logs, helper scripts `runstats.ps1`, `tab.py`, `summ.py`, the ablated `shaders-*` trees).
**Section 8 (WS43) re-measures the light-table prologue without queries (0.16-0.26 ms of the solid pass) and shrinks it (driver VGPR median 140 -> 99 on the lit light-table pixel shaders).** **Superseded by section 7: `RVK_PASS_STATS=1`'s occlusion query forces late Z, the invocation counts and the Full gap of this document are inflated; use `RVK_PASS_STATS=2`.** Timings of runs with `RVK_PASS_STATS=1` carry the cost of the queries (small: Wrapper jungle solid 0.60 ms with, 0.68 without in WS39 clocks): all comparisons below are inside the same kind of session.
No Radeon GPU Profiler / Radeon Developer Panel is installed on the machine (searched Program Files, scoop, `B:\D`, `C:\`): the evidence is query counters + ablations.

## 1. Tooling added (debug only, off = nothing exists / one predictable branch)

| switch | what |
|---|---|
| `RVK_PASS_STATS=1` | enables `pipelineStatisticsQuery` + `occlusionQueryPrecise` (when the device has them) and wraps every inline rendering scope (`rhi::GameDevice::beginScope/endScope`) in a pipeline-statistics query (IA vertices / primitives, VS invocations, clipped primitives, FS invocations) + a precise occlusion query (samples passed). The perf windows log `perf: window X pass stats per frame: iaVerts .. vs .. clipPrims .. fs .. samples .. calls .. indPk .. indRec .. binds .. | <pass key>` next to each `pass mean` row; `-PerfFrame` lines carry the same in brackets; a `perf: passz draws: EQUAL w0 .. w1 ..` line per scope (draw records by depth compare class x depth write; reverse-z turns the engine's LESS_EQUAL into GREATER_OR_EQUAL = "other"). `calls / indPk / indRec / binds` = GPU draw calls, indirect draw packets, indirect records (upper bound, the count buffer decides) and pipeline binds of the scope. Needs `debug.record_threads=false`. A sample count of all ones (the counter returned nothing) is reported as 0. `src/vk/gputimer.*`, `core/perf_window.*`, `rhi/vulkan/vulkan_device.cpp`, `vk/game_record.*`. `rvk_d3d9test --validation` (with and without it) 0 failures / 0 validation errors, first game run `-Validation`: 0 validation messages. |
| `RVK_ABL_HDR_NOPS=1` | every pipeline that writes the scene HDR format (RGBA16F: solid, alpha, sky) is built without a fragment shader (`vk::ablateNoPs`, pipeline.cpp / pipeline_library.cpp): the vertex / front end / raster / ROP floor of those passes. Works for composed, retained and owned draws alike. |
| `RVK_ABL_SOLID_NOPS=1` | the same for the composed path only (`prepareDraw`; owned draws do not take it: use HDR_NOPS). |
| `RVK_ABL_SKIP=eq|ne` | the scene-HDR scope skips the draws with an EQUAL depth compare (`eq`: those that follow the z pre-pass) / without it (`ne`). Picture is wrong, used for attribution only. |
| `RVK_ZEQ_LEQUAL=1`, `RVK_ZEQ_NOWRITE=1`, `RVK_EQ_NOWRITE=1` | depth-state experiments of 4.2 (main solid pass, EQUAL draws: test (reversed) LESS_OR_EQUAL; additionally no depth write; EQUAL kept, no depth write). |

Shader ablations are `RVK_SHADER_DIR` copies of `shaders/` (not in the repo, in the scratch dir): `shaders-cl0` (`rvkMvCluster` returns at its first line: no table work, every slot stays the engine's own, = what an empty cluster does),
`shaders-cl1` (header + cluster id + occupancy loads kept, everything after dropped: low registers), `shaders-cl2` (the same early return behind a run-time-true / compile-time-unknown condition: the whole heavy path stays compiled in, never runs), `shaders-p2` (`rvkSceneBeginPs(WaveReadLaneFirst(index))`, WS39's P2 on the translated shaders too).

## 2. The solid pass, Wrapper vs Full (ms at 1600x900; jungle / hold / harbour_night / pub_interior)

| | jungle | hold | harbour_night | pub_interior |
|---|---|---|---|---|
| **Wrapper solid** | 0.605 | 0.570 | 0.446 | 0.071 |
| **Full solid** | 1.753 | 1.330 | 0.804 | 0.300 |
| Full / Wrapper | 2.9x | 2.3x | 1.8x | 4.2x |
| Wrapper, no pixel shader (floor) | 0.267 | 0.189 | 0.138 | 0.009 |
| Full, no pixel shader (floor) | 0.192 | 0.355 | 0.203 | 0.011 |
| **Full without the light table in the material shaders** (`shaders-cl0`) | **0.881** | **0.588** | **0.407** | **0.126** |
| => cost of the light-table prologue (Full - Full without it) | **0.872** | **0.742** | **0.397** | **0.174** |
| => share of the Full-Wrapper gap | 76 % | 98 % | 111 % | 76 % |
| fragment shader invocations, Wrapper / Full | 13.47 M / 13.31 M | 7.22 M / 7.27 M | 3.83 M / 3.74 M | 1.78 M / 1.80 M |
| samples passed, Wrapper / Full | 1.87 M / 1.87 M | 1.63 M / 1.66 M | 1.36 M / 1.37 M | 1.42 M / 1.44 M |
| vertex shader invocations, Wrapper / Full | 1.59 M / 2.18 M | 1.21 M / 1.62 M | 1.09 M / 1.17 M | 0.03 M / 0.02 M |
| GPU draw calls, Wrapper / Full | 1116 / 6 + 180 indirect packets (~965 records) | 938 / 27 + 166 packets | 786 / 38 + 149 packets | 32 / 11 packets |
| pipeline binds, Wrapper / Full | 213 / 84 | 159 / 80 | 229 / 106 | 18 / 11 |

(jungle Wrapper / Full = the `st2` sessions; the others `st3`; the rest of the gap in jungle, 0.28 ms, is 4.2.) The jungle Full-without-table value is 0.88 ms against Wrapper's 0.61: the remaining +0.28 ms is Full's scene-form material shaders' own cost (below); hold / night / pub are inside +-0.05 ms of Wrapper.

## 3. The causes, quantified

### 3.1 "Full issues ~3x the draws": a counting artefact, the opposite is true
The `N draws` of the pass key is `Device::passDraws_`: it counts D3D9-level draws, and the owned tree / vegetation path adds one per *instance* (`owned_trees.cpp: passDraws_ += it.instances`), owned records one each.
The GPU sees **6 direct calls + 180 `vkCmdDrawIndexedIndirect(Count)` packets (~965 records, upper bound) and 84 pipeline binds in Full against 1116 draws and 213 binds in Wrapper** (jungle). With the pixel shaders removed
(`RVK_ABL_HDR_NOPS`) Full's solid pass is **0.19 ms, Wrapper's 0.27 ms** (jungle; hold 0.36 vs 0.19, harbour_night 0.20 vs 0.14: the owned path's vertex pulling + 30 % more vertex shader invocations (no post-transform cache reuse across instanced records, 2.18 M vs 1.59 M in jungle) cost +0.07 .. +0.17 ms there, jungle's 24 % fewer pipeline binds win it back). So geometry, draw count and front end explain **at most +0.17 ms (hold) and nothing in the other scenes**.

### 3.2 Same fragments, no extra overdraw, no lost early-Z relative to Wrapper
FS invocations, samples passed, clipped primitives and IA primitives agree between the modes within 1-2 % (table above): Full does not shade more fragments than Wrapper, it shades the same ones more expensively. Depth state: both modes draw the same solid draws with the same state (jungle, from the `passz` histogram: ~668 EQUAL + ~445 (reversed) LESS_OR_EQUAL records in Wrapper, 376 + 592 in Full, depth write on for all of them: the engine's state, passed through unchanged).

### 3.3 The light-table prologue is the gap (jungle, same session design, Full):

| variant of the material shaders | solid ms | what it is |
|---|---|---|
| `shaders-cl0`: no table work | 0.881 | what Wrapper-like shading costs in Full's scene forms |
| `shaders-cl1`: header, cluster id, occupancy loads, then return (low registers) | 1.115 | **+0.234 ms: the dependent load chain** (header, grid, depth -> cluster id, occupancy bit) |
| `shaders-cl2`: the whole heavy path compiled in, never executed | 1.470 | **+0.355 ms: the static footprint** of the heavy path (VGPR 129 vs ~90 = 5 vs 8 waves per SIMD, longer shader), paid by every pixel |
| unchanged Full | 1.753 | **+0.283 ms: the heavy path executing** for pixels in occupied clusters (candidate scoring, 3 x per-light engine-twin tests, top-3 insert, baked shade, shadow lookups) |

hold: none 0.588, heavy compiled-in-never-run 1.031 (+0.443 = loads + footprint), full 1.330 (+0.299 executed). So in jungle **13 % of the pass is the prologue's loads, 20 % its register / code footprint, 16 % its executed work**, the rest (50 %) is the material itself (0.88 ms, = Wrapper's 0.61 + the scene-form residual).
Consequence for WS39's measurements: P2 (uniform record index, -1 .. +5 %), P6 / P26 (GI hooks out, -5 .. +1 %) did not touch the cost centres above; the cluster prologue was never ablated as a whole. `lighting.clustered = false` agrees: jungle solid ~0.8 ms (normalised by the untouched passes of the same session; A 1.80 vs B 1.48 raw ms in a session whose untouched passes ran 1.8x slower), at the price of `light.cluster` + `light.shadow` (which the table path needs).
Why the prologue costs when most pixels take the early-out (occupancy bit clear): the registers and the code are allocated for the heavy path in every pixel's shader regardless, and the early-out is reached only after the load chain.

### 3.4 What Full's scene forms cost on their own (jungle +0.28 ms, hold / night ~0, pub +0.055)
Full-without-table minus Wrapper = jungle +0.28 ms = Full's floor is 0.075 ms *lower*, so the pixel side is +0.35 ms: the scene-form shaders read the DrawRecord / DrawBlock / texture indices per lane (WS39 section 4.1: waterfalls around the samples). Not attributed further here (the P2 variant `shaders-p2` was prepared but not measured: WS39's P2 on the ubershader alone showed nothing, the translated shaders (97 % of the pipelines) never got it).

### 3.5 Not a cause, but the largest lever of the pass in both modes: most shaded fragments are hidden ones
Jungle: **13.4 M fragment shader invocations for 1.87 M samples passed (7x)**; hold 4.4x, harbour_night 2.8x, pub_interior 1.25x; identical in both modes. With `RVK_ABL_SKIP`: the EQUAL draws (the z pre-pass followed) alone give 11.9 M invocations for 0.85 M samples (14x), the others 1.6 M for 0.95 M (1.7x); their GPU time (Wrapper / Full): EQUAL draws 0.51 / 1.2 ms, others 0.35 / 0.66 ms. So the z pre-pass does **not** buy early rejection for the EQUAL draws on this GPU / driver.
Tried (jungle, both modes): `RVK_EQ_NOWRITE` (EQUAL, no depth write: a pixel shader with discard then needs no late depth write): invocations unchanged (13.3 M), time unchanged. `RVK_ZEQ_LEQUAL` (EQUAL -> LESS_OR_EQUAL, write kept): invocations 13.4 -> 8.9 M (Full) / 9.6 M (Wrapper), solid -21 % (1.85 -> 1.46) / -18 % (0.86 -> 0.70), **but samples passed rise 1.87 -> 2.45 / 2.65 M: surfaces nearer than the z pass's depth exist (draws or cut-outs the pre-pass did not write), so it changes the picture: not neutral**. `RVK_ZEQ_LEQUAL + NOWRITE`: worse (14.0 M invocations, 4.6 M samples: coplanar duplicates pass and are shaded). Why EQUAL draws are not early-Z rejected is open (see 5, item 3).

## 4. Ranked fixes

Gains in ms at 1600x900 (x0.64 for 720p) of the solid pass; "gap" = Full - Wrapper.

| # | fix | gain | effort | risk | picture |
|---|---|---|---|---|---|
| 1 | **Lamp-free draws get the lean material variant** (no `rvkMvCluster` code: = `shaders-cl0`, or the `Gi` / scene variant without `RVK_MV_CLUSTER`), the rest keep the table path. The test is per draw: object bounds against the frame's table lights (the owned object table has the bounds and the light table is built every frame by `light.cluster`; hysteresis against flapping; the owned path's GPU compaction can split a draw bucket per variant, composed / retained draws are remade through `variantEpoch_` like the GI toggle of WS40). Exact by construction: for a pixel no table light reaches the prologue returns the engine's own slots (src 0, factor 1), which is what `cl0` leaves. | jungle -0.87, hold -0.74, harbour_night -0.40 (the whole gap, in lamp-free areas by day); pub_interior (lamps everywhere) little. | L (variant slot + per-draw decision + GPU bucket split + toggles) | medium (a missed light = a visibly dark object: test conservatively, `native.picture_check`) | neutral |
| 2 | **Shrink the heavy path's footprint** (VGPR 129 -> <= 96 = 8 waves): drop `ePos[3]` (re-load), keep cScore / cIdx packed, split the twin test out of the light loop (hoist it to a per-(draw, light) mask: shader-isa.md candidate 6), `[loop]` instead of the nested unrolls; iterate with `isa-survey.ps1` / `rvk_shadertool uber-spv --variants clonly` (static VGPR) and the cl-ablation timings above. | up to the 0.36 ms footprint term (jungle) + part of the 0.28 executed term | M-L | low-medium (the table lighting is the Full look) | neutral if the algorithm is unchanged |
| 3 | **Find why the EQUAL draws are not early-Z rejected** (13.4 M vs 1.87 M, both modes): per-material-class statistics (cut-out vs opaque: scan the SPIR-V for OpKill / OpTerminateInvocation at pipeline build and split `RVK_ABL_HDR_NOPS` by it), check the engine's z pre-pass really writes every EQUAL draw's depth (the 2.45 M vs 1.87 M samples under LEQUAL say some surfaces are missing from it, or differ), try a stencil-or-depth "second pre-pass" only for the draws that fail, `[earlydepthstencil]` variants for the EQUAL draws (legal: an EQUAL pass writes the value it tests). | 0.5 .. 1.2 ms of the pass in jungle in BOTH modes if the 7x collapses (Full pays 2.4x per wasted invocation: the largest single lever once item 1 is done) | M (diagnosis) + S-M | none for diagnosis; LEQUAL-type fixes change the picture where the pre-pass is incomplete | see 3.5 |
| 4 | The load chain (+0.23 jungle): read the table header through scalar loads (`base` is wave-uniform, `rvkPsWin(3)`-derived fallbacks make it per-lane in the scene form: use the frame block's address as the fallback; WS39's P2 did not move the pass, so expect < 0.1) and issue the occupancy test at the top of the shader so its latency overlaps the texture fetches. | <= 0.1 ms | S | none | neutral |
| 5 | Scene-form residual (+0.28 ms jungle only): `WaveReadLaneFirst(record index)` inside a quad-granular waterfall for the translated scene shaders too (shader-isa.md candidate 5; measurement-only `shaders-p2` ready, not run). | <= 0.3 ms jungle | M | medium | neutral |
| 6 | Do nothing about draws / draw count: Full already issues fewer GPU draws and binds than Wrapper. The `N draws` label should say records / instances (rename `passDraws_` output or count GPU calls) so nobody chases it again. | 0 | S | none | none |

## 5. Not prototyped, why
Fix 1 is the answer but not small; fix 3 is a diagnosis task (the experiments above only exclude the discard / depth-write explanation); the small neutral candidates (4, the label) are below the noise or cosmetic. What was built is the tooling (section 1), which is the missing piece of WS39 ("per-pipeline draw counters ... are the missing piece"): `RVK_PASS_STATS` per-pass counts, GPU draw call counts, the depth-state histogram, and the ablation switches. No production shader or state changed (all switches default off; `RVK_PASS_STATS` off = no pools, no queries; the per-packet counters are behind one cached boolean).

## 6. Reproduce
```powershell
# stats + pass table of one mode / scene (the logs carry `pass stats per frame` rows)
.\scripts\perf-ab.ps1 -Key overlay.compile_progress -A true -B true -Mode full -Scenes jungle -Pairs 1 -Settle 120 -Window 120 -Lead 2500 -PlayerDefaults -Setting 'debug.record_threads=false' -Environment 'RVK_PERF_WINDOW_ORDINAL=1','RVK_PASS_STATS=1'
# floors: add RVK_ABL_HDR_NOPS=1; shader ablation: RVK_SHADER_DIR=<copy of shaders with rvkMvCluster returning early>, -Lead 20000 (cold variants), same env
# first run of new query code: add -Validation
```

## 7. WS42: the "7x shading" is an artefact of RVK_PASS_STATS's occlusion query (2026-11-17)

Question: why are the EQUAL draws (after the z pre-pass) not rejected before shading in both modes (jungle 13.4 M FS invocations for 1.87 M samples)? **Answer: in the shipped
configuration they are. The surplus appeared only while an occlusion query was active (`RVK_PASS_STATS=1`, section 1); with it, this driver (AMD proprietary 26.11.1, RX 7900 XTX) runs the depth
test after a shader that can `discard` (late Z) for the whole scope. The game itself never issues occlusion queries (occlusion.md), nor does the renderer.** Sections 3.5 / 4 item 3 / 5 above are superseded by this one.

Method: Wrapper jungle 1600x900, `perf-ab.ps1` sessions, `debug.record_threads=false` where counters are read. New `RVK_PASS_STATS=2` = pipeline statistics only (no occlusion query; `samples` reads 0).
Hypotheses tried with the experiment build (commit ccad4a8, reverted in the next commit): `PipelineDesc::earlyZ` = a twin of every fragment module that has a discard + `OpExecutionMode EarlyFragmentTests`, used for draws testing EQUAL
without stencil (legal and picture-identical: a fragment passing EQUAL writes the depth already there; verified: in-run anchored `shot` A/B at 7 spots in Wrapper and Full, difference between flag off and on = the drift between two identical shots, <= 2 levels),
`RVK_EARLY_ALL=1` (every fragment module early: bound), `RVK_ABL_SKIP=eqk|eqo` (EQUAL draws with / without a discarding shader).

| jungle, Wrapper (Full) | FS invocations | solid ms |
|---|---|---|
| `RVK_PASS_STATS=1` (occlusion on), EQUAL draws as the engine has them | 13.4 M (13.3 M) | 0.66-0.78 (1.74) |
| the same + early-tests twin on EQUAL draws | 2.46 M (2.47 M) | 0.51-0.60 (0.96) |
| all fragment modules early (bound) | 1.79 M = samples passed | 0.555 |
| `RVK_PASS_STATS=2` (statistics only), engine state | **2.46 M (2.46 M)** | **0.59 (0.84)** |
| `RVK_PASS_STATS=2`, early twin on | 2.46 M | 0.59 |
| no query at all, early twin off / on (in-session, 3 pairs, record threads on) | - | 0.603 / 0.610 (+1.2 %, z +0.9); Full 0.8135 / 0.8120; hold, night, pub within +-2 % |

So hypotheses 1-3 are answered: the late Z was forced by the active occlusion query (not by discard + depth write, not by the dynamic depth state, not by HTILE, not by an incomplete pre-pass:
removing the query gives the early reject; the early-tests twin cannot add anything). The 7x artefact costs jungle +0.17 ms in Wrapper (EQUAL class 0.29 -> 0.46 ms) and ~0.9 ms in Full (heavier shader pays more per wasted invocation),
which inflated sections 2-3 of WS41 (Full solid 1.753 ms with the query; 0.84 without, RVK_PASS_STATS=2). The Full / Wrapper solid gap without queries (sessions of `RVK_PASS_STATS=2`, separate sessions, clocks differ by session): jungle 0.53 / 0.84, hold 0.32 / 0.57,
harbour_night 0.34 / 0.64, pub_interior 0.079 / 0.32 ms: 1.6x, 1.8x, 1.9x, 4.1x; with default settings (record threads on, `pf` sessions) 1.35x, 1.27x, 1.22x, 3.9x. It is real, smaller in vegetation scenes than WS41 said, and its
attribution (light-table prologue) was measured under late Z: **redo the cl0 ablation without a query before building fix 1 / 2 of section 4** (an in-session `lighting.clustered=false` A/B is not usable: the old light path is far slower, jungle 0.96 -> 2.4 ms).

### Remaining surplus, decomposed (jungle Wrapper, `RVK_PASS_STATS=2` for fs / ms, `=1` for samples, `RVK_ABL_SKIP`)
| class | draws | FS invocations | samples passed | solid ms (alone) |
|---|---|---|---|---|
| EQUAL (all are cut-out shaders with discard: `eqk` removes exactly the same draws as `eq`) | 667 | 0.93 M (11.9 M with the occlusion query) | 0.87 M | 0.29 (0.46 with the query) |
| not EQUAL (LEQUAL, discard + depth write: no pre-pass, late Z is required) | 449 | 1.64 M | 0.94 M | 0.28 |
| whole pass | | 2.46 M | 1.84 M | 0.59 |

EQUAL draws shade 1.06x their passing samples in production; the whole pass shades 1.31x (hold 1.35x, harbour_night 1.34x, pub 1.2x). The 0.65 M surplus is the non-EQUAL cut-outs (shaded, then discarded or
hidden; an early test is illegal for discard + depth write): at most ~0.12 ms (surplus share of that class's 0.28 ms), only reachable by giving those draws a pre-pass (changes the engine's order / picture risk): not done.
Quad overshading: the FS counter counts covered pixels only (it equals samples when everything is early), helper lanes are invisible to it. Jungle averages 2.3 samples per clipped primitive (1.84 M / 0.78 M), so lanes launched are
a multiple of the counted ones; it is inherent to the foliage meshes (detail distances) and was not measured (no lane counter without a material-shader write, forbidden by the rules).
D3D12: the PSO bakes depth state, the game issues no occlusion queries there either; nothing to change (not measured: no pipeline statistics on that backend).

### What changed in the code
`RVK_PASS_STATS=2` (statistics without the occlusion query; `=1` keeps both: timings and fs counts of `=1` runs are NOT the shipped ones, use `=2` for invocation counts). Everything else of the experiment is reverted
(`debug.early_equal_z`, `PipelineDesc::earlyZ`, twins): zero effect without queries, see the table. Recipe: `.\scripts\perf-ab.ps1 ... -Environment 'RVK_PERF_WINDOW_ORDINAL=1','RVK_PASS_STATS=2' -Setting 'debug.record_threads=false'`.


## 8. WS43: the light-table prologue re-measured without queries, and shrunk (2026-11-17)

Task (fix 2 of section 4, "shrink the lamp-light code"), measure first. Machine / method: RX 7900 XTX, `RisenTest.exe`, 1600x900 windowed, vsync off, `-PlayerDefaults`, record threads on (default sessions, **no occlusion / statistics query**), private copy `risen-test-vk-ws43`, `perf-ab.ps1` sessions with `-Key overlay.compile_progress -A true -B true -Pairs 3 -Settle 120 -Window 120 -Lead 20000 -Seconds 800` (6 windows of 120 frames per session; 20000 frames lead = the variant queue is empty; a cold shader tree needs the longer watchdog: the default 300 s killed two cold sessions), **one session per arm and scene, arms interleaved scene by scene** (main, ablations, Wrapper, new), 2-3 rounds (sessions below). Arms are `RVK_SHADER_DIR` trees (no rebuild; the text equals the embedded one): `main` = the tree before this work, `cl0` = `rvkMvCluster` returns at its first line (what Wrapper-like shading costs in Full's forms), `cl1` = header + cluster id + occupancy loads, then return, `cl2` = the whole heavy path compiled in behind a run-time-true / compile-time-unknown early return (registers and code paid, never run), `e3` = the change of this section. The trees and drivers are in `B:\DevDrive\rvk-work\scratch\ws43` (`runms.ps1`, `sum43.py`, `shaders-*`). GPU clock state moves whole sessions (hold: the passes this work does not touch, "frame minus solid", were 1.8 / 2.0 / 2.3 ms in different sessions), so the table gives the sessions' values and the ratio solid / (frame - solid).

### 8.1 Decomposition (solid pass ms; session values in brackets; Wrapper in the last column)

| scene | main | cl0 (no table) | cl1 (+ loads) | cl2 (+ footprint) | **e3 (new)** | Wrapper |
|---|---|---|---|---|---|---|
| jungle (2 sessions) | 0.767 (0.794 0.741) | 0.507 (0.558 0.455) | 0.483 (r1) | 0.601 (0.559 0.642) | **0.638** (0.631 0.645) | 0.587 |
| hold (3) | 0.596 (0.525 0.589 0.676) | 0.436 (0.400 0.455 0.452) | 0.445 (r1) | 0.541 (r1) | **0.595** (0.564 0.628 0.593) | 0.536 (0.439 0.562 0.607) |
| harbour_night (3) | 0.585 (0.567 0.611 0.577) | 0.405 (0.362 0.399 0.454) | 0.431 (r1) | 0.507 (0.461 0.553) | **0.535** (0.515 0.545 0.545) | 0.539 (0.455 0.589 0.573) |
| pub_interior (2) | 0.306 (0.310 0.302) | 0.129 (0.129 0.129) | 0.156 (r1) | 0.198 (0.197 0.199) | **0.234** (0.232 0.235) | 0.076 |
| solid / (frame - solid), main -> e3 -> cl0 -> Wrapper | 0.347 -> 0.295 -> 0.195 -> 0.227 | hold 0.308 -> 0.263 -> 0.212 -> 0.198 | harbour 0.413 -> 0.348 -> 0.243 -> 0.281 | pub 0.470 -> 0.365 -> 0.201 -> 0.243 | | |

Split of the prologue (round 1, one session per arm; noise of one session pair +-0.03 .. 0.06 ms in the vegetation scenes, +-0.003 in pub_interior):

| ms | (a) table header / cluster / occupancy loads (cl1 - cl0) | (b) code + register footprint of the heavy path, compiled in, not executed (cl2 - cl1) | (c) the heavy path executing (main - cl2) | total (main - cl0, round 1) | total over all sessions |
|---|---|---|---|---|---|
| jungle | -0.075 (noise: the loads cost less than the noise) | +0.076 | **+0.235** | 0.236 | 0.26 |
| hold | +0.045 | +0.096 | -0.016 (noise) | 0.125 | 0.16 |
| harbour_night | +0.069 | +0.030 | +0.106 | 0.205 | 0.18 |
| pub_interior | +0.027 | +0.041 | +0.113 | 0.181 | 0.18 |

So **in default sessions (no query) the whole light-table prologue is 0.16-0.26 ms of the solid pass (0.18 in pub, where Wrapper's solid is 0.076)**, not 0.4-0.9 ms as WS41 measured under the occlusion query; (b) + (c) is above 0.1 ms in jungle (0.31), harbour_night (0.14), pub_interior (0.15), about 0.08 in hold: the stop condition ("(b)+(c) below 0.1 ms in every scene AND no RDNA2 occupancy gain") was not met, the RDNA2 gain being large (8.3). (c), the executing path, is the largest part wherever lamps exist; (a) is small everywhere (a dependent chain, but 3 of its 5 loads are uniform), (b) is a 7900 XTX effect only through occupancy of the executing waves.

### 8.2 Registers and occupancy (what the driver and RGA say)

Driver numbers (`RVK_ISA_DUMP`, 7900 XTX, wave64, `rvkcompare` jungle spot, Full, 26.11.1; lit light-table pixel shaders = the `cl-cl` variants, the GI-off variant of a default session; occupancy model waves/SIMD = floor(768 / VGPRs rounded up to 8) as in shader-isa.md):

| | main | e3 |
|---|---|---|
| lit `cl-cl` pixel shaders | 507 | 596 (a different session builds a different set) |
| VGPR mean / median / p90 / max | 127.0 / 140 / 151 / 170 | **103.1 / 99 / 142 / 148** |
| share above 128 VGPRs / at most 96 | 67 % / 20 % | **25 % / 41 %** |
| waves per SIMD, mean (histogram) | 5.99 (4: 32, 5: 306, 6: 67, 8: 25, >=9: 77) | **7.41** (5: 151, 6: 111, 7: 89, 8: 129, >=9: 116) |
| instructions, median | 2798 | 2759 |

RDNA2 estimate (Radeon 680M, gfx1035, wave64 = floor(512 / VGPRs rounded up to 8) waves per SIMD): RGA 2.9.1 on the 18 lit requests of the 28-request census (`scripts/rga-quick.ps1`-style, `--half`): `clonly` mean 130.0 -> 94.7 (max 163 -> 142), i.e. **3 -> 5 waves per SIMD on average** (the unlit-prologue `scene2` form of the same requests: 84.4). On gfx1100 RGA (runs ~15 % above the driver): 142.5 -> 98.0 mean (max 187 -> 144), `scene2` 97.6: **against its unlit form the lit shader now spans -13 .. +16 VGPRs (median about -1 for the 18 lit requests; 4 of 18 still +10 or more: the prologue's own peak), on average the table no longer adds registers**. The ablation says the 7900 XTX itself gains little from occupancy (WS39), so this is mostly a 680M result (not measurable here).

### 8.3 What changed (shaders only; picture-neutral)

1. **`RVK_CL_EARLY` defaults to 1 when the variant has no ambient hooks** (`ge_default.frag.hlsl`): the cluster selection runs at the entry, where little else is live, and leaves 6 compact words (3 x source / factor) for the texture phase; with the hooks (`gi_cl`) the entry stays crowded by the GI lookup and the old place stays (measured: `gi_cl` 126.4 -> 133.5 mean VGPR with it on). Biggest single step: `clonly` 130 -> 98.6 (gfx1035).
2. **The selection's bookkeeping is one word** (`rvk_clWord`: count, used, kept state, shadow tested / minimum visibility): seven statics (count, used, kept state, shadow, shadow tested, physical flag, default roughness) stayed live through the whole shader because the debug views read them in the epilogue. `rvkMvPbr` reads the physical flag and the default roughness from the frame block. Shadow minimum is quantised to 8 bits (debug view only). -4 VGPR (gfx1035 98.6 -> 94.7).
3. **The table header's fallback base is the frame block address** (`pc.frame`), not the per-lane draw window: `base` / the header, grid and slice loads are wave-uniform by construction; RGA ISA of one request: `s_load` 9 -> 21, `global_load` 112 -> 91, `v_readfirstlane` 163 -> 100, ISA -4 % (WS41 fix 4).
Not changed: the algorithm, the twin test, the scoring, the slot filling, the occupancy shortcut.

### 8.4 Measured gain (Full, solid pass, sessions as above)
jungle -0.129 ms (-17 %; frame GPU 2.97 -> 2.80 ms, the untouched passes 2.21 -> 2.17), hold -0.001 raw but its sessions ran on different clocks (solid / rest 0.308 -> 0.263 = -15 %), harbour_night -0.050 (-8.5 %; solid / rest 0.413 -> 0.348), pub_interior -0.072 (-24 %; frame 0.958 -> 0.874, untouched 0.652 / 0.640). That is **28 .. 50 % of the prologue's total cost removed** (50 % jungle, 41 % pub, 38 % harbour normalised, 47 % hold normalised); Full / Wrapper solid: jungle 1.31 -> 1.09, harbour_night 1.08 -> 0.99, pub_interior 4.0 -> 3.1 (Wrapper's solid there 0.076 ms), hold ~1.1 -> 1.1 (raw, clock-skewed). What is left against `cl0`: +0.13 jungle, +0.16 hold, +0.13 harbour_night, +0.105 pub = the executing path (c) and the loads (a).

### 8.5 Tried and dropped (measured, no gain)
* **Software-pipelined light records** (next light's index -> position / colour / shadow id requested at the top of the iteration; `e4`): +1 VGPR, solid jungle 0.630 vs 0.631, harbour_night 0.516 vs 0.515, pub 0.229 vs 0.232, hold 0.509 vs 0.564 (noise): not kept.
* **Waterfall over the wave's clusters so the loop's loads see a wave-uniform address** (`WaveActiveMin` + `WaveReadLaneFirst`, `rvkLightClusterAddrOfId`; the P4b idea of WS39 on a uniform base): RGA ISA unchanged (`s_load` 21 / `global_load` 95): LLPC still emits `global_load` with an SGPR base + VGPR offset for the loop's reads (confirms shader-isa.md 4.2); not kept.
* Re-reading the engine positions instead of the `ePos` array: not tried again (S-2b: it hangs the GPU-assisted validation layer, 14.7b).

### 8.6 Picture evidence and gates
* **In-run anchored shot A/B** (`scratch/ws43/ab_shots.ps1`, a toggle tree: both call sites compiled, early when `lighting.occupancy` is on, late when off; `freeze+100/130` on, `+150: set lighting.occupancy off`, `+210/240` off, `+260` on, `+320/350` on; the occupancy shortcut is exact by WS20, so on vs off = early-without-shortcut vs late): the share of pixels differing by more than 2 levels, between the two arms / between two shots of the same arm (drift): pub_interior 0.03 % vs 0.04-0.07 %, monastery_hall 0.46 % vs 0.47-0.59 %, harbour_night 0.002 % vs 0.004-0.009 %, crystal_cave 0.06 % vs 0.07-0.09 % (the third pair of crystal_cave shows a scene change between the shots `b2` and `c1`, 4.6 % in `a1 vs c1` as well, i.e. not the toggle). Maximum differences 8 .. 47 levels in single pixels, the same as the drift. The lamp lighting is the same (pub_interior shot checked by eye).
* `native.picture_check` with `RVK_PC_STRICT=1` (`pcA`, six spots, `pc43.ps1`): three runs PASS (`pcA1`, `pcA2`, `pcA3b`: 8 checks, 0 failed; windows FAILED 0 of 33 / 33 / 47; `DIFFERING` 0, `MISS` 0, 0 error lines; the control with the `main` tree `pcM1` the same, worst "ok" window 0.47 % vs 0.16 / 0.45 / 0.49 %). **One run (`pcA3`, the third try) crashed 7 s after the start**: `Texture::prepareForSampling` (`resources.cpp:460`, `tex_` null) through `Device::ownedPrepTexture` <- `StaticOps::ownedPass`, right after owned-trees "DIFFERING ... retained template" errors; a CPU-side owned-path race, not shader text (a null texture object), not reproduced in the rerun (`pcA3b`); not investigated further here, `ownedTextureNeedsPrep` / `valid_` with a null `tex_` needs its own look.
* Builds / tests: `rvk_tests` 371 / 0 failures; `rvk_d3d9test --validation` Vulkan and `--backend d3d12` PASS 0 / 0; `rvk_rhitest` PASS (D3D12 and Vulkan); `dxil-check embedded` 191 / 191; `dxil-check uber-spv --variants clonly,gi_cl,scene2 --dxil` (stride 2 of the census, float and `--half`) 489 / 489 each; `dxil-check corpus corpus --gi` 2246 / 2246; a short Full run with `-Validation` + GPU-assisted validation (jungle, pub_interior, monastery_hall): PASS, 0 errors, no VUIDs. No C++ changed, so the pedantic clang-cl build was not rerun.

### 8.7 Left
The executing path (c) is the remaining cost (+0.1 .. 0.2 ms over `cl0`): the per-light loop is latency bound (index -> record -> shadow id per light, up to 63 lights) and LLPC will not scalarise it (8.5); the twin test per (draw, light), the per-light baked-shade read and the sorted insertion are per pixel. Candidates: a per-(draw, light) twin mask from the cluster pass, a typed / buffer-descriptor read of the cluster list (SMEM-eligible), or per-wave clusters scored once (a compute pre-pass writing the 3 slots per cluster x draw class is not possible: the slots depend on the pixel's position and normal). Reproduce: `runms.ps1 -Arms main,e3,wrapper,cl0 -Scenes jungle,hold -Rounds 1,2` (scratch `ws43`), `python sum43.py ms`; static: `static.ps1 -Name x -Asic gfx1100 -Variants clonly -Tree <tree>` + `lit.py`.
