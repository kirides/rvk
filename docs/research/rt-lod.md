# RT scene level of detail, memory limit and the RT quality preset

Status (2026-11-05): built and measured. **Result: the far-LOD ladder gains no GPU time on the RX 7900 XTX at any scene the game has**
(triangles traced -7 .. -28 %, every RT pass unchanged within noise) and **costs AS memory** (+4 .. +10 MiB, the proxies are additional BLASes)
and, on D3D12, +0.3 - 0.4 ms of `rt.scene` GPU time. It therefore stays **off in Medium / High** (`rt.lod_scale` 0). What pays is
(a) the other cost knobs of the **Low preset** (water RT, shadow rays, GI) and (b) the **memory limit** `rt.memory_budget`, which actually
holds the AS memory down by releasing the originals of far objects. Code: `src/enh/rt_lod.*` (pure: bands, rungs, hysteresis, simplifier
wrapper; tests `rt_lod_*` in tests/rt_tests.cpp), `src/enh/rt_scene.*` (`setLod`, `setMemoryBudget`, `queueProxy` / `buildProxies`,
`ensureBox`, `releaseOriginals`, the rung pre-pass in `record`), settings in `src/core/settings.cpp` (`rtPresets`, tests `rt_presets_*`),
wiring in `src/hal/vulkan/device.cpp`.

## The ladder (per instance of the RT scene, by distance of its bounding sphere from the camera)

| rung | what the TLAS instance traces | enters beyond | leaves below |
|---|---|---|---|
| 0 | the engine's draw (the BLAS of its draw range): near field, **unchanged**. The engine's own lower LODs need no code: the registry's owner replacement already puts whatever LOD the engine draws into the TLAS (at player defaults lod 2x, so the engine's own LODs are coarser than stock only farther out) | - | - |
| 1 | a **proxy**: the same vertex buffer + an index list of its own (meshoptimizer `meshopt_simplify`, positions welded across uv seams, error 3 % of the extent, `rt.lod_bias` 1 / 2 / 3 = 1/2, 1/4, 1/8 of the triangles), a BLAS of its own (no compaction) | `rt.lod_range x rt.lod_scale` m | 85 % of it |
| 2 | the object's **oriented box**: one shared 12-triangle unit cube BLAS, the instance's rows scaled to the object-space AABB (x 0.92; SpeedTree: the two short axes x 0.45 = a thin hull), untextured record (constant stone grey-brown, geometric normal) | `rt.lod_far_range x rt.lod_scale` m | 85 % |
| (3) | left out: on rung 2 an object whose bounding radius / distance is below 0.003 | | |

Proxies and boxes only for static, opaque, one-material instances of kind StaticMesh / InstancedMesh / SpeedTree branches with >= 128
triangles (box: >= 64, bounding radius <= 20 m: cliffs, terrain and level meshes never become boxes). Alpha-tested / masked instances (leaves,
fences), per-triangle-material ranges, characters and animated trees keep their BLAS (a conservative-opacity blob / dropping cut-outs was not
built). Simplification: gather on the render thread (<= 200k triangles / frame), `std::async` worker, <= 8 jobs, <= 16 BLAS builds / frame;
a proxy not built yet leaves the instance on rung 0. Hysteresis 15 % per rung.

## Memory limit (`rt.memory_budget` MB, 0 = none)

Every 16 frames the AS memory (BLAS + proxies + the 3 TLAS slots; not the shared build scratch) is compared with the limit: over it
the ladder's ranges shrink x0.85 per check (down to 8 %; also with `rt.lod_scale` 0, which then uses the base ranges), and
`releaseOriginals` gives up the original AS of BLASes that no instance of the frame traces at rung 0 (largest first, until 90 % of the
limit); the proxy or the box stands in. A released BLAS is queued for a rebuild as soon as an instance of it is on rung 0 again
(the instance keeps its stand-in meanwhile); with the ladder off every released BLAS is rebuilt. Under 80 % the ranges relax again.
Unused BLASes are evicted (LRU) at min(limit, 512 MiB) as before. rvk.log `rt: scene: lod bands ...`: `memory limit N MB (scale, originals
released, released / rebuilt in total, AS MiB)`.

## RT quality preset (`rt.preset` = low | medium | high | custom, Lighting; default medium)

A preset writes these settings (`settings::rtPresets()`); a hand change of any of them flips it to Custom (matched by value in the
registry's change callbacks); **Medium = every member's default** (a fresh install matches it, nothing changes), High traces more:

| setting | Low | Medium (defaults) | High |
|---|---|---|---|
| `rt.lod_scale` (x `rt.lod_range` 60 m / `rt.lod_far_range` 150 m; 0 = ladder off) | 0.3 | 0 | 0 |
| `rt.lod_bias` | 3 | 2 | 2 |
| `rt.memory_budget` | 384 MB | none | none |
| `shadows.rt_quality` / `shadows.rt_range` | low / 15 m | medium / auto | high / auto |
| `rt.reflections` (water RT) | low | high | high |
| `gi.quality` / `gi.screen` | low / low | medium / medium | high / high |
| `materials.rt_reflections` / `materials.rt_range` | low / 60 m | high / 150 m | high / 300 m |

(The user's idea was Medium at scale 0.6; the measurements below show the ladder alone costs memory and on D3D12 time, so Medium keeps it
off: one table row to change, `rtPresets()`.) The overlay's graphics presets set `rt.reflections` too; either flips the other to Custom.
Feature toggles (`shadows.rt`, `water.enabled`, `gi.enabled`) are not part of the preset.

## Measurements (RX 7900 XTX, `-PerfFrame` 24 frames, rvkcompare spot held, shadows.rt extended + water RT + GI on; other agents' games share the GPU: +-5 % noise)

Triangles of the TLAS by distance band, ladder off (jungle, harbour_docks): 185k / 35k / 39k / 0 / 0 = 259k (127 instances, AS 53.8 MiB) at the docks;
290k / 94k / 177k / 142k / 70k = 774k (1327 instances, AS 67.1 MiB, BLAS 27.9) at the jungle (trees 9 / 34 / 57 / 33 / 36k of those; characters
15k); lake_jungle 490k (656 inst, 62.8 MiB); monastery_hall (interior) 413k (340 inst, 91.2 MiB). About half of the forest's triangles lie
beyond 50 m, the near 20 m is the biggest band.

Ladder alone (jungle, GPU ms of the passes; A off / B proxy 40 m bias 2 / C + boxes 100 m / D proxy 15 m bias 3 + boxes 30 m):

| cfg | traced triangles | shadows.rt trace | gi.screen | gi | water.rt | rt.scene | frame |
|---|---|---|---|---|---|---|---|
| A | 774k | 1.103 | 1.276 | 0.852 | 0.797 | 0.114 | 6.89 |
| B | 712k -> 660k | 1.105 | 1.253 | 0.833 | 0.796 | 0.100 | 6.82 |
| C | 712k -> 626k | 1.177 | 1.408 | 1.029 | 0.812 | 0.122 | 7.30 |
| D | 712k -> 554k | 1.194 | 1.374 | 0.931 | 0.761 | 0.118 | 7.12 |

Presets (`rt.preset=low|high` on top of the same consumers; Medium = A; frame GPU ms / traced triangles [of original] / AS MiB):

| spot | Low | Medium | High | notes |
|---|---|---|---|---|
| jungle (Vulkan) | 6.47 / 593k [740k] / 75.8 | 7.19 / 712k / 66.8 | 11.38 / 712k / 66.8 | Low: water.rt 0.80 -> 0.25, shadows.rt 1.15 -> 0.70, gi 1.01 -> 0.87; High: gi.screen 4.13 |
| harbour_docks | 6.05 / 247k [260k] / 54.5 | 6.45 / 259k / 53.8 | - | water.rt 1.21 -> 0.50, shadows.rt 0.53 -> 0.28 |
| monastery_hall (interior) | 1.64 | 2.05 | - | gi.screen 0.73 -> 0.54, gi 0.50 -> 0.29 |
| lake_jungle | 6.56 / 379k [493k] / 66.3 | 6.49 / 490k / 62.8 | - | water.rt 1.47 -> 0.42, gi.screen worse (1.09 -> 1.32) |
| jungle (D3D12) | 7.94 / 593k / 106.1 | 10.65 / 712k / 91.2 | - | rt.scene 0.28 -> 0.62; water.rt 1.20 -> 0.12 |
| harbour_docks (D3D12) | 4.93 / 390k [440k] / 106.5 | 4.72 / 259k / 96.9 | - | rt.scene 0.23 -> 0.62; water.rt 0.30 -> 0.11 |

(A run's scene differs a little: streaming, characters; the monastery's low run had 524 instances against 340.) Low saves 6 - 10 % of the
frame's GPU time on this card at the outdoor spots (2.7 ms of 10.7 on D3D12 at the jungle), nearly all from the water RT tier, the shadow
rays / range and GI quality; the ladder part of Low is +0.04 ms (Vulkan) / +0.35 ms (D3D12) of `rt.scene` and +9 MiB AS memory.

Memory limit (`rt.lod_scale 0`, limit only): jungle 45 MB: counted AS (BLAS + TLAS) 66.8 -> 37.9 MiB, 96 originals released (229 released /
2 rebuilt in the run), traced triangles 712k -> 568k, frame GPU 7.22 ms (= 7.19 off; no cost); monastery_hall 60 MB: 52.5 MiB (6 released);
D3D12 jungle at 60 MB: the floor (ranges at 8 %) is reached with 75.5 MiB, the D3D12 BLASes are 1.5x larger (40.8 vs 27.5 MiB) so that limit is
not reachable there. Pictures of limit runs were not compared.

Large draw distances (lod.view / trees / meshes 3x, jungle): 990k triangles, 1819 instances (2x: 712k / 1157): `shadows.rt trace` 1.197 ms
(2x: 1.151), `rt.scene` 0.120 (0.099), frame 7.55 (7.19): the RT passes still do not scale with triangles. With Low (ladder 0.3, bias 3, box):
793k [1017k] traced, frame 6.80 ms; AS 81.5 MiB against 72.5 MiB without.

**Where would the ladder pay?** Not here: this GPU traces ~1M triangles / ~2k instances at no measurable cost in the three consumers; their
time is set by ray / pixel counts and hit shading. It would pay on (1) cards whose AS memory or BVH build time matters (8 GB cards, older RT
hardware: not available here; use the `lod bands` line and `rt.memory_budget` there) and (2) scenes 10x bigger. Memory is where the data are
interesting: triangles per band and AS bytes are logged every 5 s, and the memory limit demonstrably holds the AS under its limit.
(D3D12's extra `rt.scene` cost with the ladder on is unexplained: the pass has a constant +0.35 ms at both spots, independent of the instance
count; not investigated further.)

Picture impact: `water-compare` A/B (left `rt.preset=high`, right `low`, shadows.rt extended, GI on) at lake_jungle, jungle, harbour_docks
(`out/water/20261105-174745/sbs_*.png`; looked at jungle and lake_jungle): the same scene; Low's water reflection / GI is a little darker
and softer (luma +0.1 / +0.1 / -7.5 at harbour_docks / jungle / lake_jungle). Ladder alone (D: 15 m bias 3, boxes 30 m) vs off:
`out/water/20261105-162229` read the same (luma +0.1 / -0.9 / -4.2). GI error against a converged 256 spp reference
(`gi-reference.ps1 -Spp 8 -Max 256`, DDGI mean relative error): jungle 52 % without the ladder, 47 % with it (0.3, bias 3, boxes 45 m);
monastery_hall 93 % / 87 % (`out/gi-reference/20261105-172819` and `-173859`): the ladder does not change GI beyond noise.

Gates (merged tree on main with the D3D12 game backend): rvk_tests 236 / 0 failures, rvk_d3d9test --validation (Vulkan and --backend d3d12)
0 validation errors, rvk_rhitest PASS, rvk_vktest --rt --passes PASS, testdemo 7/7 on Vulkan and on D3D12 (`-Api d3d12`, RVK_NO_GBV=1); no
shader changed (no dxil-check). Not measured: a perf savegame (none exists on this machine: `rvk_perfsave` needs a player's save), and
weaker hardware.
