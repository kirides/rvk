# Why Full's main solid colour pass costs 1.5-3x Wrapper's (WS41, 2026-11-17)

Question (user): in Full the main solid colour pass costs 1.5-3.2x Wrapper's GPU time in every lit scene (WS39, shader-isa.md 2) and Full "issues ~3x the draws". Is that reasonable?
Answer: **no draw / geometry cost is involved; the whole gap is the per-pixel cost of the light-table prologue (`rvkMvCluster`) in the material pixel shaders.** Both modes shade
the same fragments; with the light table removed from the material shaders Full's solid pass costs what Wrapper's costs (or less).

Machine / method: RX 7900 XTX, `RisenTest.exe`, 1600x900 (the window of the private copy; WS39's 720p numbers are ~0.64x of these), vsync off, `-PlayerDefaults`, `RVK_PERF_WINDOW_ORDINAL=1`,
`perf-ab.ps1` sessions (one session per mode and scene, 2 windows x 120 frames, ordinal 2500 frames lead; shader-ablation sessions 20000 frames lead after the variant queue reached 0),
`debug.record_threads=false` (the statistics queries need inline scopes), private copy `risen-test-vk-ws41`, scratch `B:\DevDrive\rvk-work\scratch\ws41` (logs, helper scripts `runstats.ps1`, `tab.py`, `summ.py`, the ablated `shaders-*` trees).
**Superseded by section 7: `RVK_PASS_STATS=1`'s occlusion query forces late Z, the invocation counts and the Full gap of this document are inflated; use `RVK_PASS_STATS=2`.** Timings of runs with `RVK_PASS_STATS=1` carry the cost of the queries (small: Wrapper jungle solid 0.60 ms with, 0.68 without in WS39 clocks): all comparisons below are inside the same kind of session.
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
