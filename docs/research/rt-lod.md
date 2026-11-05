# RT scene level of detail (rt.lod_range / rt.lod_bias / rt.lod_far_range)

Status: built, measured, **default off** (the measurements below show no GPU gain at the scenes the game has). Code: `src/enh/rt_lod.*`
(pure: bands, rungs, hysteresis, the simplifier wrapper; tests `rt_lod_*` in tests/rt_tests.cpp), `src/enh/rt_scene.*` (`setLod`,
`queueProxy` / `buildProxies`, `ensureBox`, the rung pre-pass in `record`), settings in `src/core/settings.cpp`.

## The ladder (per instance of the RT scene, by distance of its bounding sphere from the camera)

| rung | what the TLAS instance traces | enters beyond | leaves below |
|---|---|---|---|
| 0 | the engine's draw (the BLAS of its draw range): near field, **unchanged** | - | - |
| 0b | the engine's lower LOD: nothing to build, the registry already follows the engine's own switch (owner key per entity part replaces the old LOD's instance; lod.md; at player defaults lod.meshes / trees / view are 2x, so the engine's own LODs are already coarser than stock at mid distance) | the engine | the engine |
| 1 | a **proxy**: the same vertex buffer + an index list of its own (meshoptimizer `meshopt_simplify`, positions welded across uv seams, error 3 % of the mesh extent, `rt.lod_bias` 1 / 2 / 3 = 1/2, 1/4, 1/8 of the triangles) and a BLAS of its own (no compaction) | `rt.lod_range` m | 85 % of it |
| 2 | the object's **oriented box**: one shared 12-triangle unit cube BLAS, the instance's rows scaled to the object-space AABB (x 0.92; SpeedTree: the two short axes x 0.45 = a thin hull); untextured record (constant stone grey-brown, geometric normal) | `rt.lod_far_range` m | 85 % of it |
| (3) | left out: on rung 2 an object whose bounding radius / distance is below 0.003 | | |

Proxies and boxes are only for static, opaque, one-material instances of kind StaticMesh / InstancedMesh / SpeedTree (branches), >= 128
triangles (box: >= 64, bounding radius <= 20 m: cliffs, terrain and level meshes never become boxes). Alpha-tested / masked instances (leaves,
fences), per-triangle-material ranges (segments), characters and animated trees keep their BLAS. The proxy's index list uses the original's
index type and base vertex, so the geometry record (vertex fetch, uv, normal, material) is the original's with only `indices` swapped; the
simplifier work (gather on the render thread: <= 200k triangles / frame, simplify on a `std::async` worker, <= 8 jobs in flight, <= 16 BLAS
builds / frame). Pops: hysteresis 15 % of each range; a proxy that is not built yet leaves the instance on rung 0.

Settings (Lighting, Advanced, need RayQuery): `rt.lod_range` (m, 0 = off), `rt.lod_bias` (0 = off, 1..3), `rt.lod_far_range` (m, 0 = no boxes).
Changing the bias drops every proxy. rvk.log: `rt: scene: lod proxy on (...) box ...`, and every 5 s
`rt: scene: lod bands (...)`: per band (0-20, 20-50, 50-100, 100-200, 200+ m) instances, triangles traced [original], mesh / tree / other
triangles, proxied / boxed instances, BLAS count and storage (by nearest instance), tiny skipped, characters' and animated trees' triangles,
proxy statistics.

## Measurements (RX 7900 XTX, Vulkan, player defaults = lod 2x, `RisenTest`-less run, rvkcompare spot held, all RT consumers on: shadows.rt extended, water RT high, GI + screen probes; `-PerfFrame` 24 frames averaged; other agents used the GPU: +-5 % noise)

Triangles of the TLAS by distance band (one frame, rung 0 everywhere):

| spot | 0-20 m | 20-50 | 50-100 | 100-200 | 200+ | total | instances | AS memory |
|---|---|---|---|---|---|---|---|---|
| harbour_docks | 185k (46 inst) | 35k | 39k | 0k | 0 | 259k | 127 | 53.8 MiB (BLAS 14.6) |
| jungle | 290k (154) | 94k | 177k | 142k | 70k (112) | 774k (+15k characters, 46k animated trees inside) | 1327 | 67.1 MiB (BLAS 27.9) |
| lake_jungle | - | - | - | - | - | 490k | 656 | 62.8 MiB (BLAS 23.6) |

(jungle: trees are 9k / 34k / 57k / 33k / 36k of the bands; the rest meshes.) Roughly half of the triangles at the forest spot lie beyond 50 m,
but the near 20 m (houses, rocks, terrain pieces) are the biggest band and stay.

GPU time of the passes, ms (`-PerfFrame`), A = off, B = proxy beyond 40 m / bias 2, C = B + boxes beyond 100 m, D = aggressive (proxy 15 m / bias 3, boxes 30 m):

| spot | cfg | triangles traced | rt.scene | shadows.rt trace | gi.screen | gi | water.rt | frame GPU |
|---|---|---|---|---|---|---|---|---|
| jungle | A | 774k | 0.114 | 1.103 | 1.276 | 0.852 | 0.797 | 6.885 |
| jungle | B | 712k -> 660k | 0.100 | 1.105 | 1.253 | 0.833 | 0.796 | 6.819 |
| jungle | C | 712k -> 626k | 0.122 | 1.177 | 1.408 | 1.029 | 0.812 | 7.296 |
| jungle | D | 712k -> 554k | 0.118 | 1.194 | 1.374 | 0.931 | 0.761 | 7.121 |
| lake_jungle | A | 490k | 0.114 | 0.990 | 1.047 | 0.703 | 1.493 | 6.645 |
| lake_jungle | B | 490k -> 457k | 0.125 | 0.820 | 1.066 | 0.746 | 1.499 | 6.712 |
| harbour_docks | A | 259k | 0.145 | 0.536 | 1.028 | 0.810 | 1.240 | 6.476 |
| harbour_docks | D | 259k -> 226k | 0.125 | 0.496 | 0.984 | 0.727 | 1.092 | 6.551 |

(The triangle column of B..D is the second run's own original count -> traced; a run's scene differs a little: characters, streaming.)

Findings:

* **No rung pays.** Cutting the traced triangles by 7 - 28 % (B .. D) moves no RT pass outside the noise (+-5 %: jungle's C / D are slower, the
  docks' D is 5 - 12 % faster, lake_jungle's shadow trace 17 % faster in one run and nothing else). The passes are limited by their ray / pixel
  count, the TLAS traversal over 130 - 1300 instances and the shading of hits, not by triangle counts of a few hundred thousand. The RT
  scene is small for this hardware: the whole TLAS costs `rt.scene` 0.10 - 0.15 ms.
* The proxies **cost memory**, not save it: AS memory +9 - 10 MiB (BLAS 27.9 -> 37.7 MiB at the jungle: proxies are additional BLASes, the
  originals are still needed for the near field and are not evicted while any instance uses them). The compaction-less proxy BLASes are small
  (87k triangles of 134k kept).
* Roughly half of the proxy jobs end with nothing to gain (`jobs failed / nothing to gain`): the simplifier keeps > 85 % of the
  triangles of many small / thin / open meshes at an error of 3 %.
* Boxes: only about 80 of 390 instances beyond 100 m are boxable (the bounding radius limit, the kinds, >= 64 triangles) and trees are mostly cut-outs
  that stay; the shared box BLAS cannot be a gain in BLAS memory either (the originals stay for the near field).
* Picture impact: `water-compare` A/B (left off, right D = proxy 15 m / bias 3, boxes 30 m, RT reflections high + RT shadows extended) at lake_jungle, jungle, harbour_docks (`out/water/20261105-162229/sbs_*.png`, looked at the lake_jungle one): reflections, shadows and the scene read the same; luma differences +0.1 / -0.9 / -4.2 are animation + eye adaptation (creatures moved). Gates: rvk_tests 209 / 0 failures, rvk_d3d9test --validation (Vulkan and --backend d3d12) 0 validation errors, rvk_rhitest PASS, rvk_vktest --rt --passes PASS, testdemo 7/7 on Vulkan; no shaders changed (no dxil-check).

The decision: the scheme stays in the code, off by default (`rt.lod_range = 0`), because a scene with 10 - 30 times more instances / triangles
(a future higher draw distance, `lod.view` 3x+, other games' content) is where it would start to pay and the rungs, the hysteresis and the
stats exist; **enable only after measuring such a scene** (`rt: scene: lod bands` + `-PerfFrame` A / B as above).

Not done / limits: the engine's own lower LOD meshes are not looked up separately (they are what the engine draws when it draws them: the
owner replacement already puts them in the TLAS; `lod.meshes` 2x keeps higher LODs longer, so a *smaller* `lod.*` factor for the RT scene alone is
not possible without the engine's draws); D3D12: the game does not run on the D3D12 backend in this branch (dx12-game-port WP4+ pending), the
RT scene code only uses RHI calls (`createBlas`, `buildBlases`, `createBuffer`), so it carries over; GI ground truth numbers (`gi-reference.ps1`)
were not run: the script is not in this branch.
