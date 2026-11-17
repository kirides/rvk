# Shader ISA and GPU cost analysis (WS39, 2026-11-16)

Analysis + tooling only: no production shader was changed. Machine: RX 7900 XTX (RDNA3, driver AMD proprietary 26.11.1 (LLPC), Vulkan 1.4.359),
Radeon 680M (RDNA2 iGPU, gfx1035) reasoned from register / occupancy numbers. Raw dumps (ISA text, CSVs, prototypes, patches):
`B:\DevDrive\rvk-work\scratch\ws39` (game-derived ISA of material pipelines stays out of the repo).

## 1. Method and tooling

| what | how |
|---|---|
| GPU ms per pass | `scripts/perf-ab.ps1` sessions with tagged perf windows (240 frames, `perf window`), `RVK_PERF_WINDOW_ORDINAL=1` (new: rows of one shape get `#k`), player defaults, `RisenTest.exe`, vsync off, `tools/perf_passes.py` / `tools/perf_roles.py` (role table). Wrapper = `-Mode wrapper` session (A arm of a `renderer.mode` A/B); Full = `-Mode full` session started in Full (`lighting.clustered`, the light table and the other start-up Full features exist only there: the run-time switch Wrapper -> Full in a session is NOT the shipped Full, its numbers were discarded). Scenes: `jungle`, `hold` (savegame), `harbour_docks`, `harbour_night`, `pub_interior`, `sea_cliff` (water). |
| ISA, real driver (RX 7900 XTX) | **`RVK_ISA_DUMP=<dir>`** (new, `src/vk/isa_dump.*`; off = nothing exists): every pipeline the renderer builds gets `CAPTURE_STATISTICS | CAPTURE_INTERNAL_REPRESENTATIONS`; `<dir>/stats.csv` (VGPR / SGPR / LDS / scratch / instruction count / wave size per executable) + `<seq>_<label>.txt` with the driver's **ISA text** (`ISA.ps`, `ISA.gs` = the VS/NGG stage, `ISA.cs`; also `Intermediate.*` LLVM-like IR and the ELF as `.bin`). Pipeline libraries and the on-disk pipeline cache are bypassed while it is set. `RVK_ISA_DUMP_MB` caps the text (default 3 GiB; a jungle Full session wrote 3691 executables, 1.4 GB). `rvk_vktest --isa-dir <dir> --shader-stats` does the same for the water shaders without the game. |
| ISA, offline, every embedded shader | `rvk_shadertool dxil-check embedded-spv <dir>` (new: SPIR-V of every `shaders/**.{comp,vert,frag}.hlsl`, same text as the DXIL check) + `scripts/isa-survey.ps1` (RGA 2.9.1 `vk-spv-offline`, gfx1100 and gfx1035, CSV + ISA text; `-SpvDir` for other module sets, e.g. `rvk_shadertool dxil-check uber-spv`'s ubershader permutations; `uber-spv --variants clonly` is new: the light-table variant without the ambient / GI hooks). RGA cannot translate ray queries (all RT compute shaders, `post_dof`): use the driver dump for those. RGA's register numbers run ~15 % above the 26.11.1 driver's (water pixel shader: RGA 163 VGPR, driver 135). |
| summaries | `tools/isa_summary.py --driver <dump dir> | --rga <csv>` (occupancy model below). |

Occupancy model (VGPRs only, waves per SIMD): RDNA3 (7900 XTX) 1536 VGPRs per SIMD lane slot, RDNA2 (680M, gfx1035) 1024, allocation granule 8, max 16 waves.
**The driver compiles pixel and vertex stages as wave64 and compute mostly as wave32** (a few 64-thread-group kernels as wave64; measured from `exec` / `exec_lo` in the ISA): a wave64 shader costs twice the registers
per wave, so a 150-VGPR pixel shader has 5 waves per SIMD on the 7900 XTX and 3 on the 680M. LDS (64 KiB per CU RDNA3, 128 KiB per WGP RDNA2) and SGPR limits were never the limiter here.

Caveats: the machine's display is 1280x720 (a 1600x900 window does not fit: the game runs borderless 1280x720; rvk.log `window: ...`), so absolute ms are at 720p; scale
~1.56x for 1600x900. **GPU clock state is the dominant noise**: the 7900 XTX runs these 2-5 ms frames at very different clocks depending on how long the GPU has been loaded (the same
scene and mode gave a 2.3 ms frame in one session and 4.0 ms in another; sessions with a 6000-frame lead before the first window ran up to 45 % faster across *every* pass than 600-frame-lead sessions).
Only comparisons inside one session (the alternating A / B windows of one `perf-ab` run, ratios between passes of one session) are reliable; the replicate windows of one session agree within 2-3 %.
The pass tables of section 2 use 6000-frame leads (GPU warmed up) for both modes; separate-session deltas below ~45 % are not evidence, which is why section 5 normalises prototype timings by untouched passes of the same session.

## 2. Where the GPU time goes (player defaults, 1280x720, Vulkan)

Role table per scene (ms; Wrapper = D3D9 model on Vulkan, Full = native frame; rows < 0.02 ms omitted; `other` = post chain, bloom / lum / DoF, small passes, barriers):

**jungle** (GPU frame: Wrapper 3.68 ms, Full 5.33 ms)

| pass | Wrapper ms | Full ms |
|---|---|---|
| solid | 0.680 | 1.377 |
| water.rt at AfterRefractionCopy | 0.956 | 0.889 |
| light.shadow at AfterDepthPrepass | 0.000 | 0.761 |
| z pass | 0.548 | 0.569 |
| cascades | 0.405 | 0.438 |
| other (post, small passes, barriers) | 0.413 | 0.285 |
| alpha / refracted | 0.239 | 0.344 |
| water.ocean at AfterOpaque | 0.283 | 0.275 |
| rt.scene at AfterDepthPrepass | 0.141 | 0.185 |
| light.cluster at AfterDepthPrepass | 0.000 | 0.130 |
| post.bloom (now) | 0.000 | 0.026 |

**hold** (GPU frame: Wrapper 3.58 ms, Full 4.65 ms)

| pass | Wrapper ms | Full ms |
|---|---|---|
| solid | 0.556 | 1.330 |
| water.rt at AfterRefractionCopy | 1.250 | 1.319 |
| other (post, small passes, barriers) | 0.489 | 0.359 |
| water.ocean at AfterOpaque | 0.307 | 0.323 |
| z pass | 0.320 | 0.296 |
| cascades | 0.276 | 0.306 |
| alpha / refracted | 0.205 | 0.239 |
| rt.scene at AfterDepthPrepass | 0.161 | 0.232 |
| light.cluster at AfterDepthPrepass | 0.000 | 0.149 |
| post.bloom (now) | 0.000 | 0.033 |
| post.dofmid (now) | 0.000 | 0.021 |

**harbour_docks** (GPU frame: Wrapper 1.78 ms, Full 1.84 ms)

| pass | Wrapper ms | Full ms |
|---|---|---|
| water.rt at AfterRefractionCopy | 0.521 | 0.561 |
| other (post, small passes, barriers) | 0.363 | 0.164 |
| alpha / refracted | 0.297 | 0.310 |
| solid | 0.192 | 0.278 |
| water.ocean at AfterOpaque | 0.221 | 0.208 |
| z pass | 0.075 | 0.048 |
| rt.scene at AfterDepthPrepass | 0.051 | 0.072 |
| light.cluster at AfterDepthPrepass | 0.000 | 0.064 |
| cascades | 0.050 | 0.040 |
| light.shadow at AfterDepthPrepass | 0.000 | 0.026 |
| post.lum (now) | 0.000 | 0.026 |

**harbour_night** (GPU frame: Wrapper 1.58 ms, Full 4.00 ms)

| pass | Wrapper ms | Full ms |
|---|---|---|
| solid | 0.341 | 1.095 |
| light.shadow at AfterDepthPrepass | 0.000 | 0.632 |
| water.rt at AfterRefractionCopy | 0.272 | 0.389 |
| cascades | 0.165 | 0.369 |
| water.ocean at AfterOpaque | 0.221 | 0.342 |
| other (post, small passes, barriers) | 0.259 | 0.320 |
| rt.scene at AfterDepthPrepass | 0.068 | 0.248 |
| alpha / refracted | 0.095 | 0.190 |
| z pass | 0.149 | 0.171 |
| light.cluster at AfterDepthPrepass | 0.000 | 0.150 |
| post.bloom (now) | 0.000 | 0.034 |
| post.dofmid (now) | 0.000 | 0.021 |

**pub_interior** (GPU frame: Wrapper 0.39 ms, Full 1.26 ms)

| pass | Wrapper ms | Full ms |
|---|---|---|
| light.shadow at AfterDepthPrepass | 0.000 | 0.652 |
| other (post, small passes, barriers) | 0.237 | 0.168 |
| solid | 0.071 | 0.223 |
| light.cluster at AfterDepthPrepass | 0.000 | 0.065 |
| rt.scene at AfterDepthPrepass | 0.051 | 0.063 |
| post.bloom (now) | 0.000 | 0.021 |

**sea_cliff** (GPU frame: Wrapper 1.68 ms, Full 1.49 ms)

| pass | Wrapper ms | Full ms |
|---|---|---|
| alpha / refracted | 0.592 | 0.408 |
| water.rt at AfterRefractionCopy | 0.356 | 0.328 |
| other (post, small passes, barriers) | 0.270 | 0.160 |
| water.ocean at AfterOpaque | 0.232 | 0.230 |
| solid | 0.084 | 0.137 |
| rt.scene at AfterDepthPrepass | 0.073 | 0.067 |
| light.cluster at AfterDepthPrepass | 0.000 | 0.063 |
| z pass | 0.046 | 0.022 |
| post.dofmid (now) | 0.000 | 0.023 |

Pass roles: `solid` = the main opaque colour pass (the pass with the most draws that renders into the scene HDR target), `alpha / refracted` = the second main colour
pass (alpha blended, water surfaces of the engine, glass, particles), `z pass` = the engine's z / depth-map pass (Wrapper: R32F depth map attachment; Full: the colour-less `native.depth_z`),
`cascades` = the three 2048x2048 sun shadow maps, `light.cluster` / `light.shadow` = the light table's cluster build and the ray-traced lamp shadows (trace + filter), `rt.scene` = the RT scene's compute work (instance build, skinning) before the TLAS build.

Mean over the six scenes (GPU frame: Full 3.09 ms, Wrapper 2.11 ms; an earlier set of sessions with 600-frame leads gave 3.18 / 2.32 ms with the same ranking):

| rank | pass | Full mean ms | Wrapper mean ms | share of Full frame |
|---|---|---|---|---|
| 1 | solid | 0.740 | 0.321 | 24% |
| 2 | water.rt at AfterRefractionCopy | 0.581 | 0.559 | 19% |
| 3 | light.shadow at AfterDepthPrepass | 0.345 | 0.000 | 11% |
| 4 | alpha / refracted | 0.252 | 0.240 | 8% |
| 5 | other (post, small passes, barriers) | 0.243 | 0.339 | 8% |
| 6 | water.ocean at AfterOpaque | 0.230 | 0.211 | 7% |
| 7 | cascades | 0.194 | 0.152 | 6% |
| 8 | z pass | 0.187 | 0.192 | 6% |
| 9 | rt.scene at AfterDepthPrepass | 0.144 | 0.091 | 5% |
| 10 | light.cluster at AfterDepthPrepass | 0.103 | 0.000 | 3% |
| 11 | post.bloom (now) | 0.024 | 0.000 | 1% |
| 12 | post.dofmid (now) | 0.019 | 0.000 | 1% |
| 13 | post.lum (now) | 0.015 | 0.000 | 0% |

Top GPU consumers in Full, ranked: (1) the material **solid** pass (24 %; 1.5-3.2x Wrapper's solid pass in every lit scene: jungle 0.68 -> 1.38 ms, hold 0.56 -> 1.33, harbour_night 0.34 -> 1.10, pub 0.07 -> 0.22; an in-session A/B inside one Wrapper-started session, Wrapper vs the run-time switch to Full, gave the same 2-3x),
(2) **water.rt** (19 %; the same in both modes, up to 1.3 ms), (3) **light.shadow** (11 %; 0.63-0.76 ms in the three scenes with lamps / torches: *half of the pub_interior frame*),
(4) post / small passes, (5) alpha / refracted, (6) water.ocean, (7) cascades, (8) z pass, (9) rt.scene (Full +0.05-0.15 ms over Wrapper), (10) light.cluster (0.06-0.15 ms).
The Full-over-Wrapper gap (+0.98 ms mean GPU) is solid +0.42, light.shadow +0.35, light.cluster +0.10, rt.scene +0.05, cascades +0.04, minus the post-chain savings (-0.10).
Not default (so not in the table): GI (DDGI), screen probes, GTAO, FSR, RT materials, lamp shadows beyond the default scenes: their ISA is in section 3 only.

## 3. What the shaders compile to

### 3.1 The hot pipelines of a Full session (driver dump, jungle, 3691 executables)

| group | executables | VGPR median / max | SGPR median | instr median / max | wave64 share | scratch>0 | occ RDNA3 median | occ RDNA2 median |
|---|---|---|---|---|---|---|---|---|
| PS translated game shader | 1456 | 96 / 174 | 48 | 789 / 4747 | 97% | 0 | 7 | 4 |
| VS/GS (scene) | 1159 | 51 / 169 | 38 | 990 / 5486 | 90% | 0 | 13 | 9 |
| VS/GS (plain) | 670 | 30 / 80 | 56 | 748 / 1994 | 85% | 0 | 16 | 16 |
| PS ubershader scene (no hooks) | 336 | 83 / 141 | 42 | 617 / 814 | 100% | 0 | 8 | 5 |
| PS ubershader gi-cl (ambient hooks + light table) | 37 | 155 / 165 | 58 | 4396 / 4432 | 100% | 0 | 4 | 3 |
| compute | 29 | 39 / 256 | 46 | 472 / 22569 | 62% | 2 | 16 | 12 |
| pass pipeline | 4 | 9 / 35 | 15 | 48 / 485 | 75% | 0 | 16 | 16 |

Reading: the lit material shaders of Full are the **`gi-cl` ubershader variants** (ambient / GI hooks + light table, `enh/material_variant` `Gi` kind with `-cl`): 37 pipelines at
**4400 instructions and 155-165 VGPRs, wave64 = 4 waves per SIMD on the 7900 XTX (3 on the 680M)**, against 617 instructions / 83 VGPRs for the hook-free `scene` ubershader
(8 waves) and 789 / 96 for the translated D3D9 shaders (7 waves). That the `gi-cl` variant exists with `gi.enabled` off (log `enh: gi: off`) is because the hooks are compiled in and
branch on the run-time GI state; the register allocation pays for them anyway (below).
The dump lists pipelines, not how many pixels or draws each shades: the weights of the material permutations in a frame are not known (per-pipeline draw counters are the missing piece for "top-N pipelines by cost").
Hand-written compute / pass pipelines in the same dump (driver numbers; occupancy RDNA3 / RDNA2):

| pipeline (executable) | wave | VGPR | SGPR | LDS B | scratch B | instr | vmem | s_load | ds | branch | occ RDNA3 | occ RDNA2 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| compute gi.blend (cs) | 64 | 256 | 50 | 2560 | 320 | 2121 | 21 | 28 | 115 | 47 | 3 | 2 |
| compute gi.screen trace (cs) | 32 | 147 | 82 | 10240 | 0 | 16478 | 568 | 62 | 33 | 1228 | 10 | 6 |
| compute water.rt (cs) | 32 | 136 | 102 | 4096 | 528 | 22569 | 670 | 86 | 12 | 2073 | 11 | 7 |
| compute light.shadow trace (cs) | 32 | 109 | 73 | 4096 | 0 | 2204 | 57 | 20 | 1 | 163 | 13 | 9 |
| compute lighting images compose (cs) | 32 | 99 | 16 | 0 | 0 | 767 | 96 | 0 | 0 | 18 | 14 | 9 |
| compute gi.trace (cs) | 32 | 98 | 58 | 4608 | 0 | 12157 | 397 | 35 | 8 | 1101 | 14 | 9 |
| compute post/post_dofmid.comp (cs) | 64 | 76 | 26 | 6656 | 0 | 472 | 19 | 4 | 29 | 5 | 9 | 6 |
| compute owned draws compact (cs) | 64 | 67 | 62 | 7680 | 0 | 888 | 21 | 5 | 47 | 77 | 10 | 7 |
| compute gi.screen resolve (cs) | 64 | 59 | 91 | 0 | 0 | 1103 | 32 | 24 | 0 | 31 | 12 | 8 |
| compute gbuffer.gtao (cs) | 32 | 55 | 102 | 0 | 0 | 3564 | 78 | 12 | 0 | 110 | 16 | 16 |
| compute rt.instance_build (cs) | 32 | 55 | 46 | 0 | 0 | 316 | 80 | 4 | 0 | 9 | 16 | 16 |
| compute post/post_glow.comp (cs) | 32 | 47 | 54 | 0 | 0 | 304 | 24 | 9 | 0 | 2 | 16 | 16 |
| compute ocean.foam (cs) | 32 | 45 | 46 | 0 | 0 | 1015 | 9 | 14 | 0 | 7 | 16 | 16 |
| compute post/post_bloom.comp (cs) | 32 | 45 | 54 | 0 | 0 | 166 | 16 | 10 | 0 | 1 | 16 | 16 |

### 3.2 Ubershader permutations (RGA offline, 28 census requests x variants, medians; `out/matsrc/census_town + census_jungle`, stride 12)

| variant | instr gfx1100 | VGPR gfx1100 | instr gfx1035 | VGPR gfx1035 | waves/SIMD (wave64) RDNA3 / RDNA2 |
|---|---|---|---|---|---|
| `plain` (Wrapper-style, push-constant draw) | 668 | 79 | 541 | 66 | 9 / 6 |
| `scene2` (Full, draw record index as a varying) | 872 | 90 | 693 | 79 | 8 / 5 |
| `clonly` (scene2 + light table, no GI hooks) | 2219 | 129 | 1838 | 120 | 5 / 4 |
| `gi_cl` (scene2 + GI hooks + light table; what Full builds) | 4124 | 144 | 3478 | 131 | 5 / 3 |
| `gbuf_cl` (+ G-buffer outputs, GI on) | 4981 | 153 | 4259 | 135 | 5 / 3 |

Of the 28 requests 18 carry point lights (the cluster code): there `scene2` / `clonly` / `gi_cl` = 969 / 2732 / 4653 instructions and 92 / 141 / 149 VGPRs (gfx1100):
**the light-table prologue (`rvkMvCluster`) adds +1760 instructions and +48 VGPRs, the ambient hooks another +1900 instructions and +9 VGPRs**. No spills anywhere in the material
shaders. Z-pass shaders: 10-12 VGPRs, 170-230 instructions.

### 3.3 Hand-written shaders, every embedded shader (RGA gfx1100 / gfx1035; stage `frag`/`vert` = wave64, `comp` = wave32; test shaders and ray-query shaders omitted, see 3.1 for the latter)

| shader | stage | gfx1100 VGPR / SGPR / LDS / scratch / instr | occ | gfx1035 VGPR / SGPR / LDS / scratch / instr | occ |
|---|---|---|---|---|---|
| water_water__frag | frag | 163 / 106 / 0 / 0 / 16667 | 4 | 154 / 106 / 0 / 0 / 14008 | 3 |
| gi_gi_blend__comp | comp | 105 / 58 / 2560 / 0 / 2034 | 13 | 86 / 58 / 2560 / 0 / 1828 | 11 |
| gpuscene_lighting_compose__comp | comp | 99 / 16 / 0 / 0 / 722 | 14 | 99 / 16 / 0 / 0 / 688 | 9 |
| post_post_dof_tm__frag | frag | 92 / 58 / 0 / 0 / 1004 | 8 | 100 / 82 / 0 / 0 / 821 | 4 |
| post_fx_bloom_glowh__frag | frag | 75 / 22 / 0 / 0 / 338 | 9 | 62 / 22 / 0 / 0 / 291 | 8 |
| post_post_dofmid__comp | comp | 74 / 26 / 6656 / 0 / 488 | 16 | 74 / 26 / 6656 / 0 / 442 | 12 |
| post_fx_radial_radialblurmask__frag | frag | 70 / 34 / 0 / 0 / 381 | 10 | 66 / 34 / 0 / 0 / 316 | 7 |
| post_fx_radial_radialblurblood__frag | frag | 69 / 46 / 0 / 0 / 395 | 10 | 68 / 42 / 0 / 0 / 339 | 7 |
| shadows_shadow_mask__frag | frag | 67 / 46 / 0 / 0 / 3211 | 10 | 67 / 46 / 0 / 0 / 2729 | 7 |
| gpuscene_owned_compact__comp | comp | 66 / 62 / 7680 / 0 / 1006 | 16 | 59 / 62 / 7680 / 0 / 904 | 16 |
| post_fx_bloom_glowvaddbloom2__frag | frag | 65 / 42 / 0 / 0 / 356 | 10 | 54 / 42 / 0 / 0 / 289 | 9 |
| post_fx_radial_radialblur__frag | frag | 65 / 90 / 0 / 0 / 344 | 10 | 65 / 70 / 0 / 0 / 292 | 7 |
| post_fx_ssm_blurssao__frag | frag | 65 / 32 / 0 / 0 / 381 | 10 | 44 / 32 / 0 / 0 / 317 | 10 |
| post_fx_ssm_blur__frag | frag | 64 / 28 / 0 / 0 / 375 | 12 | 45 / 30 / 0 / 0 / 310 | 10 |
| post_post_dofdown_tm__frag | frag | 63 / 54 / 0 / 0 / 1006 | 12 | 62 / 54 / 0 / 0 / 754 | 8 |
| rt_rt_instance_build__comp | comp | 63 / 42 / 0 / 0 / 354 | 16 | 59 / 42 / 0 / 0 / 340 | 16 |
| water_water_ocean__vert | vert | 61 / 58 / 0 / 0 / 7104 | 12 | 68 / 46 / 5120 / 0 / 10937 | 7 |
| gi_sp_resolve__comp | comp | 53 / 88 / 0 / 0 / 1177 | 16 | 53 / 88 / 0 / 0 / 971 | 16 |
| post_fx_ssm_ssao__frag | frag | 53 / 42 / 0 / 0 / 544 | 13 | 41 / 42 / 0 / 0 / 501 | 10 |
| post_fx_radial_radialblurfast__frag | frag | 52 / 22 / 0 / 0 / 288 | 13 | 31 / 25 / 0 / 0 / 231 | 16 |
| post_post_bloom__comp | comp | 51 / 62 / 0 / 0 / 176 | 16 | 53 / 66 / 0 / 0 / 168 | 16 |
| rt_rt_tree_wind__comp | comp | 51 / 106 / 0 / 1296 / 3076 | 16 | 39 / 106 / 0 / 1296 / 2555 | 16 |
| post_post_dofdown__frag | frag | 50 / 30 / 0 / 0 / 186 | 13 | 31 / 31 / 0 / 0 / 164 | 16 |
| gbuffer_gtao__comp | comp | 49 / 106 / 0 / 0 / 788 | 16 | 50 / 106 / 0 / 0 / 717 | 16 |
| post_post_glow__comp | comp | 48 / 54 / 0 / 0 / 249 | 16 | 42 / 54 / 0 / 0 / 228 | 16 |
| post_fx_ssm_pssm_p2__frag | frag | 47 / 46 / 0 / 0 / 422 | 16 | 22 / 46 / 0 / 0 / 343 | 16 |
| post_post_dof__frag | frag | 47 / 62 / 0 / 0 / 205 | 16 | RGA failed | - |
| water_ocean_foam__comp | comp | 47 / 54 / 0 / 0 / 1035 | 16 | 37 / 54 / 0 / 0 / 871 | 16 |
| post_fx_blur7_blur7h__frag | frag | 46 / 38 / 0 / 0 / 297 | 16 | 31 / 50 / 0 / 0 / 245 | 16 |
| post_fx_ssm_pssm_low_p2__frag | frag | 45 / 34 / 0 / 0 / 362 | 16 | 20 / 34 / 0 / 0 / 292 | 16 |

Highlights: `water.frag` (16.7k instructions, 163 / 154 VGPRs: 3-4 waves), the post-process pixel shaders are all below 100 VGPRs (8-16 waves), no embedded shader spills in RGA
(`rt_tree_wind` has 1296 B of scratch: a local array).

### 3.4 Occupancy limiters and spills (driver numbers)

| shader | VGPR | scratch | waves RDNA3 / RDNA2 | note |
|---|---|---|---|---|
| `gi.blend` compute | 256 | 320 B (spills) | 3 / 2 | GI on only; 64-thread group, the SH projection runs on 27 lanes with a 64-iteration LDS loop |
| lit material `gi-cl` pixel shaders | 150-165 | 0 | 4 / 3 | light-table prologue + GI hooks |
| `water.frag` | 135 | 0 | 5 / 3 | 16k instructions |
| `water.rt` compute | 136 | **528 B** | 11 / 7 | the 528 B are the `WaterRtParams` copy (`plane[16]` / `box[16]` indexed dynamically) |
| `gi.screen trace` / `gi.trace` | 147 / 98 | 0 | 10 / 14 | GI on only |
| `light.shadow trace` | 109 | 0 | 13 / 9 | |

## 4. ISA findings that drive the candidates

1. **Full's scene pixel shaders load everything per lane.** The ISA of every `scene2` / `gi_cl` shader has 52 / 127 `global_load`s, 0-6 `s_load`s and 170 `v_readfirstlane`s: the draw record index
   (`rvk_sceneIdx`, a flat varying) is a per-lane value for the compiler, so the DrawRecord, the DrawBlock (texture / sampler indices), the stage-constant windows and the frame block
   are fetched with vector loads, **each texture sample is wrapped in a waterfall loop** (12 `v_readfirstlane` for the 8-dword image + 4-dword sampler descriptor, `s_and_saveexec`,
   `s_cbranch_execnz`), and every loop iteration waits `vmcnt(0)` for the descriptor load before the sample: the samples of a shader are serialised instead of batched. A wave-uniform record index
   (`WaveReadLaneFirst`) turns the record loads into scalar loads and removes most waterfalls (`v_readfirstlane` 172 -> 39 in `gi_cl`).
2. **LLPC only emits `s_load` for loads with a provably uniform address that sit in uniform control flow early in the shader.** Prototype P2 (record index) turned 10 loads into `s_load`;
   prototypes that made the cluster list / light record addresses uniform *inside the light loop* (waterfall, or an `if (WaveActiveAllTrue)` split with a duplicated loop; P4 / P4b) still produced
   `global_load`s (the offline compiler does not scalarise `vk::RawBufferLoad` through `OpConvertUToPtr` inside loops). A wave-uniform address is necessary, not sufficient: check the ISA (`s_load` count) before expecting a win.
3. **The light-table prologue is the register hog**, not the lit core: +48 VGPRs for the per-pixel candidate scoring (`ePos[3]`, `cScore[3]`, `cIdx[3]`, `twinSid[3]`, record loads in flight) and a per-light inner loop with
   a nested 3-way twin test and top-3 insertion. The clusters are 16 x 9 tiles (80 x 80 px at 720p) x 24 depth slices: almost every wave64 (8 x 8 px) sits in one cluster, so the list and the light records are wave-uniform.
4. **`water.rt` carries debug code and a struct copy in the production shader**: the `tweak 16 / 32` compare blocks (`RVK_BUF_STORE`, `vals[16]` / `ids[16]` arrays) and `WaterRtParams p = Get()` followed by dynamic `p.plane[i]` / `p.box[i]`
   make 22.6k instructions and 528 B of scratch (33 `scratch_store_b128` of the parameter block at the top of every thread).
5. **`light_shadow_filter` re-fetches** the same 25 depth + 25 entry texels once per active light entry (up to 4): 5 x 5 taps x (depth + entry) x entries inside `for e`.
6. Pixel stages run wave64: a wave-intrinsic in a material pixel shader operates on 64 lanes (8 x 8 pixels of mostly one primitive, several instances / primitives possible); compute is wave32 (8 x 4 pixels of an 8 x 8 group).
   All proposals below are wave-size agnostic (`WaveReadLaneFirst`, `WaveActiveAllTrue`, `WaveActiveBallot`, `WaveActiveSum`, `WavePrefixCountBits`) and compile to SPIR-V and DXIL SM 6.6
   (checked with `rvk_shadertool dxil-check` on the prototypes).
7. The ABI helper for D3D12 (`RvkAddr = {slot, offset}`, every `rvkRawLoad` goes through `ResourceDescriptorHeap[NonUniformResourceIndex(slot)]`) would need `rvkAddrFirstLane` /
   `rvkAddrEq` (prototype in `shaders-p3/include/rvk_abi.hlsli`: two `WaveReadLaneFirst`s); on D3D12 the same uniformity also removes the non-uniform-index waterfalls.

## 5. Measured prototypes (scratch copies of `shaders/` read through `RVK_SHADER_DIR`; patches in `scratch\ws39\patches`)

Timing method: Full sessions started with the prototype's `RVK_SHADER_DIR` (variants cold-compiled, 6000 frames lead), 2 identical-arm windows of 240 frames; the baseline is the `fullstart-*` session of
section 2. **GPU clock state differs by up to ~45 % between sessions** (a `harbour_night` session ran every pass 45 % faster than the baseline session, passes no prototype touched included), so a raw
session-to-session delta is not evidence. The "normalised" column divides the target pass by passes the prototype does not touch (water.ocean + z pass + cascades, or light.cluster + rt.scene + solid) in the same session;
effects below ~8 % are noise.

| id | change (patch in `scratch\ws39\patches`) | static ISA (driver / RGA) | measured, normalised | verdict |
|---|---|---|---|---|
| P1 | `water_rt.comp`: tweak 16 / 32 debug blocks under `#ifdef RVK_WATER_RT_DEBUG`, `plane[i]` / `box[i]` read with `rvkRawLoad` instead of the 528 B struct copy | driver: 22569 -> 16370 instr (-27 %), VGPR 136 -> 122, **scratch 528 B -> 0**, occupancy 11 -> 12 (RDNA3) / 7 -> 8 (RDNA2) | `water.rt`: harbour_docks 0.675 -> 0.392 ms (**-34 %**), hold 1.227 -> 1.040 (**-21 %**), jungle 0.949 -> 0.658 (**-31 %**; that session also carried P2, whose pixel-shader change cannot touch it; the P6 session without P1 shows +2 %) | real win |
| P2 | `ge_default.frag`: `rvkSceneBeginPs(WaveReadLaneFirst(rvk_sceneIdx))` (uniform record index; NOT correct when a wave mixes instances: measurement only) | RGA gfx1100 (27 requests): `scene2` 872 -> 716 instr, VGPR 90 -> 82; `gi_cl` 4124 -> 3773, VGPR 144 -> 132; `v_readfirstlane` 172 -> 39, `s_load` 6 -> 16 | `solid` jungle -1 %, hold +5 % | no effect on the 7900 XTX |
| P3 | `light_shadow_trace.comp`: waterfall on the cluster address (`rvkAddrFirstLane`), list + records read through uniform addresses | (RGA cannot compile ray queries) | `light.shadow` pub_interior -2 %, jungle +6 % | no effect; trace is ray-bound |
| P4 / P4b | `rvkMvCluster`: waterfall / `if (WaveActiveAllTrue(same cluster))` split with a duplicated loop | `gi_cl` +51 / +298 instr, VGPR +0, **`s_load` still 6**: the loop's loads stay `global_load` | not run in game | LLPC does not scalarise these loads (section 4.2) |
| P5 | `light_shadow_filter.comp`: loop interchange (taps outer, entries inner), bit-identical | RGA gfx1100: 36 VGPR, 2528 B ISA | `light.shadow` pub_interior **-7 %**, harbour_night -6 % (= -0.04 .. -0.05 ms) | small, safe |
| P6 | `rvkMvGi` returns the engine ambient at once (GI hooks compiled out: what a no-GI variant runs) | RGA: `gi_cl` 4124 -> 2333 instr (-43 %), VGPR 144 -> 133 (gfx1035 131 -> 123) | `solid` jungle +1 %, harbour_night -8 % (noise level) | static win, no 7900 XTX win |
| P7 | `light_shadow_trace.comp`: cluster lookup + list scan first, position / normal reconstruction only when a shadow-casting light is in the cluster | (RGA cannot compile ray queries) | `light.shadow` jungle +1 %, pub_interior -7 % | no effect; ray-bound |
| P26 | P6 + P2 | RGA: `gi_cl` 4124 -> 2013 instr (**-51 %**), VGPR 144 -> **120** (5 -> 6 waves RDNA3; gfx1035 131 -> 117: 3 -> 4 waves) | `solid` jungle +1 %, harbour_night -5 %, hold 0 % | no 7900 XTX win; candidate for the 680M |

Other ablations (same session design): `native.vertex_pull` off: `solid` -3.8 % (jungle, 3574 draws unchanged); `lighting.clustered` off (restart): `solid` 1.45 -> 2.49 ms (the table path is *cheaper* than the
engine's per-object lights), but it removes `light.cluster` + `light.shadow` (0.93 ms in jungle, 0.71 ms in the pub): clustered on = GPU frame 5.54 ms, off 5.98 ms in jungle; pub_interior 1.23 vs 0.41 ms.
The Full `solid` pass issues ~3x the draws of Wrapper's (jungle 3577 vs 1120, harbour_night 1584 vs 786): at ~0.4 us per draw it is **draw-count / front-end bound as much as shader bound** on this GPU at 720p.

## 6. Candidates, ranked

Gains are in ms of the 7900 XTX at 720p (the numbers of section 2 / 5) unless marked 680M. "Measured" = a prototype ran in the game (section 5); "ISA" = static only.
Every proposal below compiles from one text to SPIR-V and signed DXIL SM 6.6 (the prototypes pass `rvk_shadertool dxil-check`), is wave-size agnostic and writes nothing from a material pixel shader.

| # | candidate | gain | effort | risk | verify |
|---|---|---|---|---|---|
| 1 | **`water.rt`: compile out the tweak 16 / 32 compare blocks (`#ifdef RVK_WATER_RT_DEBUG`, the compare-readback buffer stores included) and read `plane[i]` / `box[i]` with `rvkRawLoad` at their offsets instead of copying the 528 B `WaterRtParams` into private memory (P1).** Not a wave-intrinsic change: the scratch copy (33 `scratch_store_b128` per thread) and the debug arrays were the cost. | **measured -21 .. -34 % of `water.rt` = 0.2-0.3 ms** in the water scenes (jungle, docks, hold), Wrapper and Full alike; 680M: occupancy 7 -> 8, no scratch traffic (larger) | S (30 lines) | very low (default code path identical; the debug tweaks keep working with the define) | `rvk_vktest --shader-stats --isa-dir` (scratch 0), `dxil-check embedded --filter water_rt`, `rvk_rhitest` parity (water RT hits), refcompare `beach_sun`, `sea_cliff`, `harbour_docks`: pixels identical |
| 2 | **`light_shadow_filter`: taps outer / entries inner loop interchange (P5)**, then an LDS tile (8 x 8 + 2 halo) for the 25 depth + 25 entry taps (one fetch per texel per group instead of per pixel x entry). `lsNormal` of the trace pass shares the 4 neighbour depth taps with it (the one place a `Quad*` / LDS neighbour exchange helps: 5 depth fetches per pixel, quad ops give only one of the two neighbours per axis, so use the LDS tile). | measured -6 .. -8 % of `light.shadow` = **-0.04 .. -0.05 ms** for the interchange alone (the tile: est. another -0.05 .. -0.1) | S / M | none (bit-identical result) | `rvk_rhitest` / picture check of `pub_interior`, `harbour_night` with `lighting.light_shadows` on: identical |
| 3 | `light.shadow` trace early-out (P7): the cluster lookup + a scan of its list for a shadow-casting light before the 5-depth-fetch position / normal reconstruction; plus skipping the whole pass in frames whose table holds no light with a shadow id. The pass is 0.65-0.82 ms in three of six scenes (11 % of the mean Full frame, 53 % of the pub_interior frame) but is **ray-query bound**: P3 (uniform cluster loads) and P7 (early-out) measured +1 .. -7 % = noise. The levers are the ray count per pixel, the resolution of the trace (half-res + the existing 5 x 5 filter) and temporal reuse, not ISA. | measured ~0 (P7 jungle +1 %, pub -7 %) | S | none | `light.shadow` image identical |
| 4 | **GI hooks only when GI is on: build the material light-table variant without `RVK_MV_AMBIENT` while `gi.enabled` is off (`clonly`, P6)**, keep the hooked variant for GI sessions. The 37 `gi-cl` pixel shaders of a default Full session carry 4400 instructions and 155-165 VGPRs for a lookup that returns the engine ambient. | ISA: -43 % instructions (4124 -> 2333), VGPR 144 -> 133 (gfx1035 131 -> 123); with #5: 144 -> 120 (RDNA3 5 -> 6 waves; **680M 3 -> 4 waves, +33 %**). Measured on the 7900 XTX: **0 .. -5 %** of `solid` (noise): the 7900 XTX solid pass is not ALU / occupancy bound at 720p. Expected on the 680M: the register-limited case, est. 5-15 % of `solid` (not measurable here) | M (variant selection in `enh/material_variant.cpp`, tag, GI on/off toggle rebuilds variants) | low (the fall-through is the engine's own ambient: P6 renders it) | refcompare Full spots with `gi.enabled=false`: identical; `rvk_shadertool dxil-check uber ... --variants clonly`; perf-ab `gi.enabled` |
| 5 | **Wave-uniform draw record in the scene pixel shaders (P2)**: `rvkSceneBeginPs(WaveReadLaneFirst(idx))` inside a quad-granular waterfall (the record index is constant per primitive, so per quad; a wave64 can hold several instances): record, DrawBlock, stage windows and the texture / sampler descriptors become scalar loads and the per-sample `v_readfirstlane` waterfalls + `vmcnt(0)` serialisation go away (`v_readfirstlane` 172 -> 39). | ISA: -18 % instructions (`scene2` 872 -> 716), -8 VGPR (`gi_cl` -12); measured 0 .. +5 % on the 7900 XTX (not bound); 680M: with #4 the 4-wave step | M | medium: a waterfall around the whole body with `discard` and derivatives inside needs care (the proper form is "if wave-uniform then scalar path, else the current path" = code x2, or the waterfall on quads); D3D12: `rvkAddrFirstLane` for the `{slot, offset}` pair (written in the prototype's `rvk_abi.hlsli`) | `rvk_shadereq` ubershader equality (183 requests), `rvk_rhitest` parity, `dxil-check uber`, ISA `s_load` count |
| 6 | **Light-table prologue restructure** (`rvkMvCluster`: +1760 instructions, +48 VGPRs over `scene2`): the engine-twin test (3 positions x every cluster light) is per pixel but depends only on (draw, light): hoist to a per-draw bit mask (CPU or the cluster pass), keep the 3 slots as 3 scalars; and read the cluster list / light records scalar. Wave scalarisation of this loop (P3 / P4 / P4b) did **not** work with LLPC 26.11.1: the loads stay `global_load` (section 4.2); try `StructuredBuffer` / `ConstantBuffer` typed reads (SMEM-eligible) instead of `vk::RawBufferLoad` before spending more. | 680M: 131 -> ~100 VGPRs would be 3 -> 5 waves; 7900 XTX: none measurable | L | medium (the table lighting is the Full look: `monastery_hall` / `crystal_cave` / `pub_interior` judged against Wrapper) | refcompare Full, `rvk_shadereq` |
| 7 | Validation passes: `picture_check` / `depth_z_compare`: one global atomic per geometry / differing pixel (`rvkBufAdd`) -> `WaveActiveCountBits` + one atomic per wave (64x fewer); `owned_compact`'s `InterlockedOr(gVis[j >> 5], 1 << (j & 31))` per visible row -> `WaveActiveBallot` word store. | ~0 (picture_check 0.033 ms, owned_compact < 0.02 ms): hygiene | S | none | existing counters equal |
| 8 | `gi.blend` (GI on only; 256 VGPRs, 320 B scratch, 3 waves): the 27-lane SH projection with a 64-iteration LDS loop (27 of 64 lanes working) -> per-ray terms + `WaveActiveSum` (9 x 3 sums), `sBack` `InterlockedAdd` -> `WaveActiveCountBits`; split the 7-register store so `regs[7]` / `oldL0` do not coexist. | unmeasured (GI off by default); likely 2-3x on the pass | M | low | `gi.reference` compare, `rvk_rhitest` GI loads |
| 9 | Pipeline hygiene, not a shader change: `gi.blend`, `gi.trace`, `gi.screen trace` / `resolve` pipelines are built with `gi.enabled` off ("Off = nothing exists"). | start-up compile time | S | none | `RVK_ISA_DUMP` list |

Not worth it (evaluated): `post_lum` / `post_lum_tile` reductions (0.014 ms; the groupshared tree is already 2 dispatches), `rt_instance_build` (no atomics, 55 VGPRs), ocean FFT (LDS butterflies, 36 VGPRs,
< 0.02 ms per pass), `Quad*` neighbour reads in the post chain (taps go through the sampler; the only explicit stencil is `lsNormal`), forcing wave32 for pixel shaders (no API for pixel stages: `subgroupSizeControl`
covers compute / mesh only; the driver picks wave64), GPU-side per-pipeline draw counters (`RVK_ISA_DUMP` lists pipelines, not their draw counts: add them before choosing material permutations by weight).

Order of work: 1 (cheapest, biggest measured), 2 (small, free), 4 (+5) as one low-end change set for the 680M with a RADV-free check on the real iGPU, then 6, 8.
Open question that ISA alone cannot answer: what limits the Full `solid` pass on the 7900 XTX (it is 2-3x Wrapper's, with ~3x the draws; ISA size, VGPRs, vertex pulling and uniform loads were tried and
do not move it; per-draw front-end cost and the draw count of the owned / instanced ops are the remaining suspect: `perf-ab -Key native.owned_instances`, or a pipeline-switch counter).

## 7. Reproduce

```powershell
$env:VCPKG_ROOT='E:\git\github\vcpkg'; cmake --preset default; cmake --build build --config RelWithDebInfo
# pass table (Wrapper: -Mode wrapper session; Full: -Mode full session; identical arms = replicates)
.\scripts\perf-ab.ps1 -Key overlay.compile_progress -A true -B true -Mode full -Scenes jungle,harbour_night -Pairs 2 -PlayerDefaults -Environment 'RVK_PERF_WINDOW_ORDINAL=1'
python tools\perf_roles.py jungle=<wrapper rvk.log>,<full rvk.log>
# the driver's ISA of every pipeline of a run
.\scripts\run.ps1 -Mode full -Exe RisenTest.exe -PlayerDefaults -Environment 'RVK_ISA_DUMP=B:\DevDrive\rvk-work\scratch\ws39\isa' ...
python tools\isa_summary.py --driver B:\DevDrive\rvk-work\scratch\ws39\isa --top 30
# offline, every embedded shader + the ubershader permutations, gfx1100 + gfx1035
.\scripts\isa-survey.ps1
rvk_shadertool dxil-check uber-spv <dir> --variants plain,scene2,clonly,gi_cl --half --stride 12 out\matsrc\census_town.jsonl out\matsrc\census_jungle.jsonl
.\scripts\isa-survey.ps1 -SpvDir <dir> -Tag uber
# a prototype: copy shaders\ to a scratch dir, edit, run with RVK_SHADER_DIR=<dir> (static numbers: rvk_vktest --isa-dir; DXIL parity: rvk_shadertool dxil-check embedded --filter <name>)
```

Findings that are not shader candidates:
- (WS40: done, section 8) With `gi.enabled` off the pipelines `gi.blend`, `gi.trace`, `gi.screen trace`, `gi.screen resolve` are still built (4 compute pipelines in the dump; DXC + driver compile time at start-up, ~0.05 s each): "Off = nothing exists" does not hold for them.
- (WS40: done, section 8) The material `gi-cl` variants were built and used with GI off (hooks branch at run time), see 6.
- `native.picture_check` / `depth_z_compare` use one global atomic per differing / geometry pixel (`rvkBufAdd`): wave-reduce them (`WaveActiveCountBits` + one atomic per wave) before anyone leaves them on in a perf run
  (they are validation passes: 0.03 ms when idle).

## 8. WS40: what was built from the candidates (branch worktree-agent-a0d265efaad100875)

| candidate | state | what |
|---|---|---|
| 1 `water.rt` | **done** | the tweak 32 compare blocks (shadow + trace compare, the readback-buffer stores) are under `#ifdef RVK_WATER_RT_DEBUG`; `plane[i]` / `box[i]` are `rvkRawLoad`s at `WATER_RT_PLANE_OFFSET` 256 / `WATER_RT_BOX_OFFSET` 512 (`static_assert(offsetof)` in `enh/water_rt.h`, the `abi_blocks.cpp` probe covers the fields). Tweak 16 (one-query trace) stays a run-time branch. `rt.reflection_tweaks` bit 32 builds the debug variant on first use on a worker (`WaterRt::buildPipeline(true)`, `spirvVariant(..., "RVK_WATER_RT_DEBUG=1")`); the pass skips until it is ready. SPIR-V 93530 -> 69805 words. |
| 2 `light_shadow_filter` | **already on main** | the taps-outer / entries-inner form (with the per-tap `nid[]` cache) was in `light_shadow_filter.comp.hlsl` before WS39's prototype (commit 42f3184); nothing to change. The LDS tile follow-up was not asked for. |
| 4 no GI hooks with GI off | **done** | new variant slots `GiCl` / `SceneGiCl` / `SceneGiCl2` (`kVariantSlots` 35, `variantUrgent` / `uberSlots` 64-bit), `MaterialVariant::ClusterOnly` (= the `clonly` form of `dxil-check uber-spv`: `RVK_MV_CLUSTER` only, no ambient overrides, nothing for shaders without point lights). With the light table on and no pack / physical style (`!pbrVariants()`) lit draws take `Device::giSlotPreferred()` = `GiCl` while `giDraws_.on` is false, `Gi` (hooks) once GI is on. `RVK_GI_HOOKS_ALWAYS=1` = the former behaviour (A arm below). |
| 9 gi.* pipelines | **done** | `Gi` builds nothing in its constructor; `startBuilds` (trace, blend) runs in `enable()`, `startScreenBuilds` (screen trace / resolve) when `gi.screen != 0`, `releasePipelines` in `disable()` and on every `apply` while off. A build still running at turn-off is dropped by the next `apply` (small known gap). |
| 5 (P2 uniform record index) | not done | not proven safe in the time available. |

### 8.1 Toggling GI at run time (what happens, what keeps drawing)

`setGiDraws` (GI on / off) bumps `variantEpoch_`; `ownedDrawSig` / the retained-draw refusals hash `Device::giVariantPeek` (the module the draw would take now), so retained and owned records are remade at a switch.
- A lit draw asks for the preferred slot (`giVariant`): its module when built, else the OTHER slot's module when that one is built (never requested: a session that starts with GI on builds no cluster-only forms, a session that starts with GI off builds no hooked forms), else the engine's own module (the first minutes of a session, as before). While the preferred module has no pipeline yet the push path tries the other slot's module (`usedAlt`, the draw is not retained). Scene forms: the preferred scene slot; pending = the draw goes through the push path (which has the fallback). So a toggle never skips a draw and never stalls the engine thread: the hooked variants build on the variant workers after the first draw that asks for them.
- `pipelines.keys`: a key naming `Gi` / `SceneGi` / `SceneGi2` is prewarmed as `GiCl` / `SceneGiCl` / `SceneGiCl2` while GI is off (keys of earlier sessions keep working), a `GiCl` key is skipped while GI is on.
- `gi.trace` / `gi.blend` / the screen probes' pipelines are built when GI turns on (the GI passes skip frames until they are ready, as at a session start), released when it turns off.

### 8.2 Measured (1600x900, Vulkan, Full, `-PlayerDefaults`, `perf-ab.ps1 -Restart`: one session per arm, 2 + 2 sessions of 240-frame windows; GPU clock state differs between sessions: read the untouched passes next to the target)

water.rt, A = main's shaders (`RVK_SHADER_DIR` = `git archive` of main), B = this branch (`scratch\ws40\pab_shader`):

| scene | water.rt A -> B | GPU frame A -> B |
|---|---|---|
| harbour_docks | 0.824 -> 0.569 ms (**-30.9 %**) | 2.500 -> 2.196 (-12.1 %) |
| beach_sun | 0.417 -> 0.143 ms (**-65.7 %**) | 2.017 -> 1.712 (-15.1 %) |
| sea_cliff | 0.542 -> 0.331 ms (**-39.0 %**) | 2.478 -> 2.223 (-10.3 %) |
| jungle | 0.650 -> 0.356 ms (**-45.2 %**) | 3.467 -> 3.123 (-9.9 %) |

Untouched passes: main solid colour pass -1.7 .. -2.4 %, `light.shadow` / `light.shadow filter` -3.5 .. +1 % (noise level). Wrapper uses the same shader (same effect, not timed separately).

GI hooks, A = `RVK_GI_HOOKS_ALWAYS=1` (hooked variant with GI off, the old behaviour), B = cluster-only (`scratch\ws40\pab_gi`): jungle GPU frame 3.340 -> 3.630 (+8.7 %, z 1.3), pub_interior 1.084 -> 1.102 (+1.7 %), harbour_night 2.268 -> 2.340 (+3.2 %); the main solid pass +7.7 / +0.4 / +3.3 % while `light.shadow` (untouched) moved +11 / +2 / +5 % in the same sessions: all inside the session-to-session clock noise, **no measurable change on the 7900 XTX** (as WS39 predicted: its `solid` pass is not occupancy bound). The static win (`gi_cl` 4124 -> 2333 instructions, VGPR 144 -> 133 on gfx1100; 131 -> 123 on gfx1035) is for the 680M and was not measurable here.

### 8.3 Checks

`rvk_tests` 371 / 0 failures; `rvk_d3d9test --validation` Vulkan and `--backend d3d12` 0 failures, 0 validation errors; `rvk_vktest --passes` 0 bad; `dxil-check embedded` 191 / 191; `dxil-check uber-spv --variants clonly --dxil` (27 jungle census requests) 27 / 27; `rvk_rhitest` PASS on both backends (water RT hits worst difference 2.9e-6). GPU-assisted validation, Full, GI off -> on -> off -> on at frames 4000 / 7000 / 10000 (jungle): 0 validation errors; the water.rt raw loads ran in it.

GI toggle, Full, `native.picture_check=true` + `RVK_PC_STRICT=1`, a frozen scene, 4 toggles: Vulkan GI off at start (jungle) windows ok 63, FAILED 0 of 77; GI on at start (pub_interior) 63 ok, FAILED 0 of 75; D3D12 (no debug layer: with GPU-based validation the run is 5 fps and never reaches the toggles) GI off at start (pub_interior) 31 ok, FAILED 0 of 40; DIFFERING 0 / MISS 0 in all. Final binary (jungle): 46 ok, FAILED 0 of 58. pcA x3 (6 spots, `RVK_PC_STRICT=1`): windows ok 14, FAILED 0 of 33 each, DIFFERING 0, MISS 0.

GI takes effect after a toggle: with `gi.debug=irradiance` (jungle) the toggled-on frames differ from the off frames by a mean 14.6 grey levels (71 % of the pixels), off vs off 0.33, on vs on 0.31, and the toggled-on frames match a session that started with GI on (mean difference 0.8 - 1.1, 2 % of the pixels over 8; on vs off in that session 15.1). Without the debug view GI barely changes the jungle / pub_interior frames (about 1 % luma: a shot review found no visible difference at those spots), so the debug view is the proof.

refcompare, Wrapper, spots beach_sun / sea_cliff / harbour_docks / jungle against `out\refcompare\20261022-093252`: beach_sun WARN (37.8 dB), sea_cliff FAIL (29.7 dB, bad32 2.7 %: the same verdict as the stored main reports of 2026-11-05, 30.4 dB / 2.2 %), harbour_docks and jungle PASS. A control: the same refcompare with main's shaders (`RVK_SHADER_DIR`) gives the same verdicts (sea_cliff FAIL 30.2 dB, beach_sun WARN 37.1 dB), and two runs of the new build differ from each other as much as new differs from main (mean 1.6 .. 5.8 grey levels between runs: water animation), so the water spots are unchanged within the run-to-run noise.

## 9. WS41: why the solid pass is 1.5-3x Wrapper's (answer in `docs/research/full-solid-pass.md`)

The open question of section 6 is answered there with per-pass pipeline statistics (`RVK_PASS_STATS=1`) and ablations: Full issues fewer GPU draws and pipeline binds than Wrapper (the "3x draws" is `passDraws_` counting
instances), shades the same fragments, and its whole extra cost is the light-table prologue in the material pixel shaders (jungle +0.87 of the +1.15 ms gap at 1600x900, hold +0.74 of +0.76; loads +0.23, footprint +0.36, executed +0.28 in jungle).
Wave-uniform record loads (P2) and the GI hooks (P6) were not the cost centres. Ranked fixes (lean variant for lamp-free draws, smaller heavy path, the 7x hidden-fragment shading of the EQUAL draws in both modes) are in that file.

**WS43 (2026-11-17), candidate 6 partly done:** the light-table prologue's register footprint was cut by running the selection at the shader entry in the no-ambient variants (`RVK_CL_EARLY`), packing its bookkeeping into one word and using the frame block as the header's fallback base: driver VGPR median of the lit `cl-cl` pixel shaders 140 -> 99 (waves / SIMD 6.0 -> 7.4 on the 7900 XTX), RGA gfx1035 mean 130 -> 95 (3 -> 5 waves on the 680M); the per-light loop's loads still stay `global_load` (waterfall over the wave's clusters tried again: unchanged). Numbers, ablations and gates: full-solid-pass.md 8.
