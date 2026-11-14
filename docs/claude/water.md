# Enhanced water, FFT ocean, RT water reflections

Moved out of CLAUDE.md (2026-11-13: it exceeded Claude Code's size limit). Read the file for the area you work on.

Enhanced water (docs/research/water.md; settings category Water, player default on, test baseline off): `-Setting 'water.enabled=true'` (+
`water.quality=low|medium|high`, `water.reflections=sky|ssr`, `water.ocean=off|gerstner|fft`, `water.ocean_quality=low|medium|high`, `water.sea_state=auto|calm|moderate|rough|storm`,
`water.wind_direction`, `water.wave_scale`; debug view `water.debug=shore_field|depth|normals|reflection|foam|sea_floor|waves`). The look is the engine's
own water material (its pixel shader's model ported in `shaders/water/water.frag`, picked by sampler layout; the engine's
VS stays on its meshes; water.md 2.3, 4.3) + traced reflections, waves, foam, wet beach. rvk.log: `water: material ...`
(each water material's name / mesh / kind once), `water: draw of material ...` (per material x pixel shader: its engine
shaders, declaration, states, constants c6..c14 and VS c232..c241; bytecode in `bin/rvk/cache/water/`), `enh: water:
material ... -> model ...` (`none` = an unknown water shader: engine draw), `enh: water: body ...`, `sea mesh mapping`,
`coast overlay` / `shore field` lines. Side by side engine vs enhanced: `.\scripts\water-compare.ps1 [-Spots a,b]
[-Setting k=v] [-EngineDir out\water\<stamp>] [-Mask | -MaskDir out\water\<stamp>]` -> `out\water\<stamp>\sbs_<spot>.png` +
water luma / hue table (`tools/water_stats.py`; `-Mask` = a `water.debug=normals` run for the water masks; `-Burst 4
-Interval 8` = a motion sequence per spot -> `motion_<spot>.png`).
FFT ocean (docs/research/ocean.md, M8; `water.ocean=fft`): compute passes at AfterOpaque (`pass water.ocean` in
`-PerfFrame`), the sea floor field from the main view's static meshes (`DrawReplacer::observeOpaque`), baked on a worker
and cached in `bin\rvk\cache\ocean\floor_v1.bin` (delete it to re-learn the coast); rvk.log `enh: ocean: ...` (cascades,
sea state / Hs, weather -> sea state, `sea floor field ... ms`, `foam buffer`, `player position`). The surf's foam: a
camera-centred buffer (`ocean_foam.comp`, ocean.md 4.4) + the pattern in `shaders/water/water_noise.hlsli`; tune foam
patterns offline first (a numpy port of the noise renders coverage ramps to PNG in seconds; game runs take minutes);
`water.debug=foam` shows the terms (green white water, blue residual, red whitecaps, grey contact). Judge foam in-game at
the spots' grazing views (crops of 2-5x, consecutive frames: `-Burst 4 -Interval 1`): the pattern is filtered per pixel
axis (`foamDensity` dX / dY); lit foam must stay below clipping (shoulder in water.frag). The water's clock is the engine's
game time (ocean.md 4.5: rvkcompare's pinned game time pins waves and foam; rvk.log `enh: water: clock ...`); what still
varies between runs at the beaches is the engine's eye adaptation and splash particles (luma +-5). Ocean spots of rvkcompare: `sea_cliff`, `sea_horizon`,
`harbour_mouth` (the camera's distance is capped near 6,000 units: give poses as target + distance <= 5,500); spot fields
`thunder = true` (lightning kept), `freeze = false` (the clock runs through the capture), `explicit = true` (only by name,
not in "all" / refcompare): `beach_night_storm` reproduces the sky cube seam (ocean.md 9.8; while the water is on the
sky cube is redrawn whole every frame, rvk.log `scene: sky cube refreshed every frame`). `water-compare.ps1 -Instance 1`
runs beside another Risen. Swim check:
scenario `rvk_swim` (the hero in the sea, his swim line logged, pictures `swim_follow_b*` / `swim_water_b*`). Survey
scenario `rvk_water` (water level-mesh entities' boxes + a look at each; `-Param 'only=Levelmesh_Lakemesh_06','find=SP_Don'`;
`'around=x;y;z','radius=2500'` = 17 pictures around a place; with `-RvkLogLevel trace` rvk.log also names opaque materials).
Ray-traced water reflections (M9 P3, rt-reflections.md 3.3): `-Setting 'rt.reflections=low|medium|high'` (default medium,
acts with `water.enabled` + `water.reflections=ssr`; off = SSR): compute pass `water.rt` at AfterRefractionCopy
(`enh/water_rt`, `shaders/water/water_rt.comp`) -> a reflection image `water.frag` looks up; rvk.log `enh: water.rt: ...`
(image size, bodies), `enh: water: body ... plane ...` (each body's fitted plane; error > 150 = no RT). Debug:
`water.debug=reflection_source` (green screen reuse, blue shaded hit, yellow SSR, black cube) / `reflection_shaded` (every
hit shaded: exposure check) / `reflection`; hidden `rt.reflection_tweaks` (1 cut-outs solid, 2 no shadow rays, 4 no SSR
fallback, 8 unshaded, 16 one-query traces instead of opaque-first (= `RVK_RT_ONEQUERY=1`), 32 compare both on the same ray (magenta / rvk.log `enh: water.rt: compare`), 64 the pre-fix batch material, 128 RT sun shadows opaque first (A/B, no gain: rt-shadows.md 14); rt-reflections.md 3.3.1 / 3.3.2: on Vulkan `rvkSize2D` of a lane-varying index is wrong, the RT records carry texture sizes and the pack map's mip offset (audit of every size query: 3.3.2)). Compare SSR vs RT: `.\scripts\water-compare.ps1 -EngineSetting 'water.enabled=true','rt.reflections=off'
-Setting 'rt.reflections=medium'` (add `'water.debug=reflection'` to both for the reflection alone). Costs: A/B inside one
spot hold (`run.ps1 -Mods rvkcompare -Param 'spots=lake_jungle','freeze_wait=4800' -Perf -WindowSize 1920x1080
-Commands '4400: set rt.reflections off','5200: set rt.reflections medium'`; the hold starts near frame 3,800; `perf:`
windows) and `-PerfFrame` (`pass water.rt`). `rvk_vktest --shader-stats` prints VGPRs / LDS / scratch of `water.frag` and
`water_rt.comp` (`VK_KHR_pipeline_executable_properties`).
Ocean mip chains (docs/research/performance.md 22; always on, the blit path is gone from the settings): `shaders/water/ocean_mips.comp.hlsl`, an SPD-style two-dispatch compute downsampler per image (ocean CPU 0.215 -> 0.048 ms Vulkan, 0.355 -> 0.099 ms D3D12; `rvk_rhitest` test "ocean mip chain by compute"; rvk.log `enh: ocean: first simulated frame ... mip chains by compute|blits`; bool settings take lowercase `true` / `false` from `-Setting`).
