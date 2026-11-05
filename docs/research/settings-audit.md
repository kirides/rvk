# Settings audit (2026-11-05)

Scope: the 172 settings registered by `registerRendererSettings` (`src/core/settings.cpp`; count asserted in
`tests/settings_api_tests.cpp`). Method: every key's `RendererSettings` accessor was grepped in `src/`; every key string in `scripts/`,
`tests/`, `tools/`, `mods/`, `package/`, `docs/`, `CLAUDE.md` (the count comment of `settings_api_tests.cpp` ignored). Nothing was
run in the game: verdicts rest on code, docs and the measurements the docs record.

## Status of the clean-up (2026-11-05)

Implemented in code; the Table's last column ("status") records the outcome per key. **172 -> 117 registered settings**
(`tests/settings_tests.cpp`).

* **55 removed**: the value is a constant (namespace `frozen` in `src/hal/vulkan/device.cpp`) or the always-on / always-off behaviour of a finished
  A/B switch. Old keys in a `settings.toml` still load, with one "was removed, ignoring" warning per key (table `kRemovedSettings`,
  `src/core/settings.cpp`; `lighting.table_complete` is listed too). `materials.puddles` was folded (puddles = `materials.rain_wetness` x 0.7:
  one "Rain" slider); `native.retained*` code is kept behind constants (V3, no CPU gain); the `fast_sort` compare mode 2 is reachable from code only.
  Four REMOVE rows were not removed: `debug.ubershader` / `ubershader_skip` (kept, label fixed: an unfinished feature, not an A/B),
  `shadows.rt_range` (Hidden), `upscale.reactive` (Advanced).
* **FIX rows: 15 of 17 done** (`rt.preset` category "Ray tracing", re-matched after load by `syncRtPreset`, `custom` a state, hidden members
  `rt.lod_scale` / `shadows.rt_range` and `lighting.light_shadow_quality` handled; the graphics presets no longer write `rt.reflections`;
  `native.packet_cache` an Enum; labels; `lighting.style` Restart; Flag::Debug on `vulkan.exclusive_acquire`, `rt.reflection_tweaks`,
  `rt.scene_views`; `upscale.object_motion` / `exposure` / `cutout_thicken` moved to Debug; `overlay.debug_mode` and `water.ocean_quality`
  Advanced). `lighting.clustered` is kept (Debug + Restart master); `water.reflections` is only partly done (greyed through the dependency
  model, not merged).
* **Dependency model** (Top 10 item 10): `Spec::needs` = `{key, value, negate, autoApply}`; the setting acts only while the prerequisites hold.
  `Registry::setWithNeeds` (a player's change: turning a dependent on also switches its unmet prerequisites on, chains followed, cycles cut,
  restart-only ones show restart-needed, the changes are returned for a toast "Also switched on: ...") never touches dependents when a
  prerequisite is turned off: they grey with `Registry::needsReason`. Unsatisfiable prerequisites (locked, capability missing, `autoApply = false` =
  `renderer.mode=full`) are `blocked` and grey the dependent. Tool API: `rvk_setting_info` 96 bytes (version 2: `needs`, `needs_reason`), new export
  `rvk_setting_set_with_needs` (`include/rvk_api.h`); tests `settings_needs_*`.
* **Deferred**: one `debug.view` enum for the `*.debug` views (each module parses its own enum, scripts / docs use `water.debug` / `shadows.debug`);
  `water.reflections` + `rt.reflections` into one enum; `gi.quality` / `gi.screen` merge; `lighting.clustered` stays a Debug + Restart master;
  the remaining "candidate R" Debug A/Bs (`debug.record_threads`, `batch_draws`, `reverse_z`, `rt_skin`); `lighting.full_lit` kept (the baseline
  pins it); the refcompare arm with Full's player defaults.

Top 10: 1 done (`native.gpu_scene` too); 2 done; 3 done (`materials.wetness` / `puddles` folded into the Rain slider); 4 done; 5 done;
6 deferred; 7 done; 8 partly (the needs model greys them with the reason in Wrapper; `lighting.style` is Restart; the table stays a Debug master);
9 done (`gi.screen` "Screen-space light detail", `materials.quality` "Material reflections", `vulkan.adapter` "Graphics card number", `ao.mode`,
HLSL ubershader labels; `rt.reflections` "Ray-traced water reflections" in Ray tracing); 10 done through the dependency model (above).

The sections below are the audit as written before the clean-up (counts are of the 172).

Finding 0: **no registered setting is dead**: all 172 have a live reader (almost all in `hal/vulkan/device.cpp`, applied at start and
through `onChange`; the exceptions are read at use: `menu_fps`, `high_fps`, the overlay keys). `native.gpu_scene` is read but has no consumer
(it only verifies / uploads a table nobody uses). The problem is not dead code, it is **finished A/B and bisect switches, art
coefficients and duplicated controls that are still registered**.

Verdicts: KEEP-PLAYER (a player would change it), KEEP-DEBUG (should carry `Flag::Debug`, so outside debug mode it applies as its
default), REMOVE (finished A/B / bisect switch, a constant in disguise, a tuning coefficient to freeze, superseded), FIX (keep, but the name /
category / default / restart / requirement is wrong). Flags column: Restart, NoAB, Hidden, Debug, Advanced; `RT` = requires ray query,
`FG` = frame generation.

## Summary

| verdict | count |
|---|---|
| KEEP-PLAYER | 45 |
| KEEP-DEBUG | 52 |
| REMOVE | 58 |
| FIX | 17 |
| total | 172 |

After the clean-up: 45 + 17 = 62 player-facing, 52 debug-only (about 10 more can go: the "candidate R" rows), so about 114 keys; merging the
`*.debug` view enums into one gets about 108.

### Top 10 clean-up wins

1. **Remove the finished Full-mode optimisation switches (11):** `native.mirror`, `native.cpu_opts`, `native.lean_restore`, `native.lean_effects`,
   `native.gpu_scene`, `native.retained`, `native.retained_check`, `native.retained_groups` (V3: "no CPU gain, stays off"), plus
   `debug.fast_sort`, `debug.fast_absorb`, `debug.ocean_spd`. Each is an A/B of a shipped, bit-identical feature or of a dead end.
2. **Remove the Vulkanize / lighting A/B pairs:** `debug.constant_compare`, `debug.split_constants`, `lighting.tree_merge`,
   `lighting.trust_table` (default off, "a small CPU saving only") and its rename shim `kRenamedSettings`, `debug.draw_records`, `debug.pace_loading`.
3. **Freeze about 22 art coefficients** (all Advanced, none referenced by a script or test): shadows softness / cascade_blend / fade / split /
   rt_softness, water murkiness / reflection_strength, gi bleed / directional / shadow_fill / emissive, materials specular / environment /
   default_roughness / derive_min / derive_max / rt_strength, lighting light_shadow_baked / light_shadow_radius, upscale reactive_cutoff /
   reactive_scale, ao.radius. They bloat the Advanced list; keep the values in code (a `[debug]` config key if tuning is needed again).
4. **Collapse the RT ladder knobs:** `rt.lod_range`, `rt.lod_far_range`, `rt.lod_bias` go, `rt.lod_scale` becomes Hidden (rt-lod.md: the ladder
   gained no GPU time; only the Low preset uses it). `rt.memory_budget` stays.
5. **One owner for the RT quality family:** `rt.preset` and the graphics presets both write `rt.reflections`; `rt.preset` is not re-matched after a
   load (`custom` can be wrong), and it sits in "Lighting" although it drives water, shadows, GI and materials. Give it a "Ray tracing" category, make
   `custom` a read-only state, add `lighting.light_shadow_quality` to it, keep the graphics presets off RT keys.
6. **Merge the `*.debug` view enums** (`water`, `shadows`, `gi`, `gbuffer`, `lighting`, `materials`, `framegen`, plus `rt.debug`,
   `debug.inject_view`, `debug.tint_draws`) into one `debug.view`: one row instead of ten in the Debug tab (scripts only pass `water.debug` /
   `shadows.debug`).
7. **Close the Hidden-but-ungated hole:** `debug.draw_records`, `debug.pace_loading`, `rt.reflection_tweaks`, `rt.scene_views`
   (and `vulkan.exclusive_acquire`) are `Hidden` but not `Flag::Debug`, so a stray value in a player's `settings.toml` applies outside debug mode.
8. **Mark Full-only player settings as such:** `lighting.style`, `lighting.light_shadows`, `lighting.light_shadow_quality` do nothing in Wrapper (the
   default) and need `lighting.clustered`, a Debug + Restart master switch players cannot see. Add a "Full mode" requirement (greyed with the
   reason) or make the table a constant of Full; `lighting.style` is also a half-restart setting.
9. **Rename the misleading labels:** `debug.ubershader*` ("GLSL ..." since the HLSL switch), `ao.mode` ("Game's ambient occlusion"), `gi.screen`
   ("Lighting detail" next to "Lighting quality"), `materials.quality` ("Material pack quality" selects the traced reflection tier),
   `rt.reflections` (rt prefix, Water category), `vulkan.adapter` (shows an index).
10. **Grey the settings that act only inside another one:** `materials.glass / rain_wetness / puddles / rt_reflections` act only at
    `materials.quality=high` (+ RT); `water.reflections=sky` disables the RT reflections silently; `gi.directional` needs `gi.screen`; Water has two
    quality knobs (`water.quality`, `water.ocean_quality`) for one feature.

### What a player sees without debug mode

Without `overlay.debug_mode` the overlay hides the Debug tab and every `Flag::Debug` setting; without "Show advanced settings" it also hides the
Advanced ones. Today that leaves about 40 settings: Display 4, System 9, Water 6, Shadows 5, Lighting 9, Materials 8, Image 4, Detail 3.
Target after this audit, about 30: Display (vsync, frame rate limit, menu limit, fullscreen mode), System (render mode, menu key, show frame
rate, high-fps fixes), Water (enabled, quality, sea waves, sea state), Shadows (enabled, quality, resolution, ray-traced), Ray tracing (preset,
ray-traced lighting, lighting quality, reflections, lamp shadows), Lighting (ambient occlusion, light style), Materials (pack, reflections quality,
derived gloss), Image (upscale mode, sharpness, frame generation), Detail (trees, objects, view distance). Advanced keeps only sensible fine
tuning (shadow distance / offset, wave height, wind, GI strength, baked light, foliage, AO strength, RT memory limit, and the restart-level
hardware settings: api, adapter, rt.allow, materials.budget, upscale provider).

## Overlaps and contradictions

| topic | problem | fix |
|---|---|---|
| `rt.preset` ("custom") vs graphics presets | both write `rt.reflections`; choosing Graphics Medium sets reflections medium, but the RT preset medium means reflections HIGH (and `materials.rt_reflections` high), so it flips to `custom`; a `settings.toml` with `rt.preset=medium` + `rt.reflections=medium` (run.ps1's baseline) loads as "medium" although it is not (the match runs only in `onChange`); choosing `custom` itself does nothing; hidden members (`rt.lod_*`, `memory_budget`) can make it custom without the user seeing why | one preset owner; recheck after load; read-only `custom`; unit test `matchRtPreset` with the baseline file |
| `rt.preset` members (`gi.quality`, `gi.screen`, `shadows.rt_quality` / `rt_range`, `materials.rt_reflections` / `rt_range`, `rt.reflections`) | the same values are individually editable (some basic) while the preset is the intended control | keep them editable only under Advanced |
| `water.reflections` (sky / ssr) vs `rt.reflections` (off..high) | three overlapping states for one effect: "off = what is on screen", "Sky only", traced | one enum `water.reflections = sky / screen / traced low..high` |
| `water.quality` vs `water.ocean_quality` | two quality tiers for one feature | one tier, ocean detail Advanced |
| `gi.quality` vs `gi.screen` | indistinguishable labels, both preset members | rename or merge |
| `gi.foliage` vs `gi.cutouts` | two controls for how leaves block traced light | keep `gi.foliage`, remove `gi.cutouts` |
| `materials.wetness` vs `rain_wetness` / `puddles` | pack wetness vs rain wetness vs puddles | one "Rain" slider; remove `wetness` |
| `materials.pack` vs `materials.quality=off` | two ways to switch the pack off | keep both, label quality "reflections" |
| `materials.specular / environment / rt_strength / default_roughness / derive_*` | seven strengths for one material model | freeze |
| `native.static_ops` / `scene_ops` vs `renderer.mode=full` | the mode turns the groups on; the booleans are bisect overrides that also turn them on in Wrapper | keep as Debug bisect only (they are) |
| `lighting.enabled` / `lighting.clustered` / `lighting.full_lit` | restart master, run-time A/B, second A/B | keep clustered + enabled; remove full_lit once Full is its only user (the baseline pins it, so needs a script change) |
| `lighting.clustered` / `full_lit` default on vs baseline off | tests pin both off, so the player defaults of Full are not covered by refcompare | add a refcompare arm with player defaults for Full |
| `native.packet_cache` Int 0..4 | one Int = off / on / verify every 10th / verify every frame / without variant draws | bool + verify + A/B bit |
| `native.lean_check` vs the compare switches | "a compare without lean_check turns lean off": hidden coupling | say so in the overlay description |

### Defaults that differ from the test baseline (`scripts/run.ps1`, without `-PlayerDefaults`)

| key | player default | baseline | note |
|---|---|---|---|
| `water.enabled` | on | false | parity with D3D9 |
| `water.quality` / `water.ocean_quality` | high | medium | |
| `rt.reflections` | high | medium | also breaks `rt.preset` consistency (above) |
| `shadows.enabled` | on | false | |
| `shadows.filter` | low | high | the baseline is the better filter, the opposite of parity |
| `shadows.map_size` | 2048 (Medium) | auto | |
| `lod.trees` / `lod.meshes` / `lod.view` | 2 | 1 | |
| `lighting.clustered` / `lighting.full_lit` | on | false | |
| `overlay.debug_mode` | off | true | Flag::Debug settings apply their values in tests; players get the defaults |
| not pinned | `materials.pack`, `materials.quality` medium, `materials.derive_from_specular`, `lighting.light_shadows` nearby, `debug.shadow_stride` 2, `debug.prefetch_stride` 4 | | the first three need a pack folder with files, so they do nothing in the baseline |

### Restart / requirement gaps

* `lighting.style`: half restart (see FIX), no Full requirement. `lighting.light_shadows`, `light_shadow_quality`: Full only without saying so.
* `debug.reverse_z`: "applies from the next frame (native passes read it when built)", in effect a restart for the RT / FSR consumers.
* `water.reflections=sky`, `materials.*` (only at High): no greying.
* `vulkan.exclusive_acquire`, `rt.reflection_tweaks`, `rt.scene_views`, `debug.draw_records`, `debug.pace_loading`: Hidden without `Flag::Debug`.
* `lighting.light_shadow_quality`, `light_shadow_baked`, `light_shadow_radius` lack the RT requirement their parent has.

## Table

Order = registration order. Evidence names scripts / tests / docs that use the key; every key has a live reader. "-" in the action column = leave.

| key | tab / category | default | flags | verdict | reason / evidence | proposed action | status |
|---|---|---|---|---|---|---|---|
| `vulkan.vsync` | Display/Display | engine | - | KEEP-PLAYER | read device/swapchain; tests settings(_api); basic option | - | kept |
| `vulkan.max_fps` | Display/Display | 0 | - | KEEP-PLAYER | run.ps1 -MaxFps writes it; tests; basic | - | kept |
| `vulkan.menu_fps` | Display/Display | 60 | - | KEEP-PLAYER | read at the pacer; docs menu-fps.md; default 60 sensible | - | kept |
| `vulkan.fullscreen` | Display/Display | borderless | NoAB | KEEP-PLAYER | window_mode.cpp; borderless/exclusive; REQ none needed | - | kept |
| `renderer.mode` | System/Renderer | wrapper | - | KEEP-PLAYER | run.ps1 -Mode, water-compare; restartGroups handle original <-> wrapper/full | - | kept |
| `overlay.toggle_key` | System/Menu | F11 | NoAB | KEEP-PLAYER | overlay.cpp; tests settings_tests | - | kept |
| `overlay.fps_counter` | System/Menu | 0 | NoAB | KEEP-PLAYER | overlay.cpp only | - | kept |
| `overlay.debug_mode` | System/Menu | 0 | NoAB | FIX | the gate for everything marked Debug; shown as a normal checkbox in System; run.ps1 pins true | make it Advanced (shown only with "Show advanced settings") so the plain menu has no Debug entry | done |
| `overlay.show_advanced` | System/Menu | 0 | NoAB+Hidden | KEEP-PLAYER | the Hidden checkbox at the top of the menu | - | kept |
| `vulkan.api` | System/Hardware | vulkan | Restart+Advanced | KEEP-PLAYER | 71 uses (config + HAL); Restart+Advanced right; d3d12 falls back with a log line | - | kept |
| `vulkan.adapter` | System/Hardware | -1 | Restart+Advanced | FIX | Int index -1 ("Graphics card") shows a number, not a name; Restart+Advanced; tests settings_tests | Enum built at start from the adapter list (names) or keep Int but label "Graphics card index" | done |
| `vulkan.exclusive_acquire` | System/Hardware | 1 | NoAB+Hidden | KEEP-DEBUG | Hidden diagnostic (window-mode.md 8.3), default on, not Flag::Debug | add Flag::Debug instead of Hidden | done: Flag::Debug added |
| `rt.allow` | System/Hardware | 1 | Restart+Advanced | KEEP-PLAYER | device.cpp; Restart+Advanced; tests caps_tests / settings_api; refcompare.ps1 | - | kept |
| `compat.high_fps` | System/Gameplay | 1 | NoAB | KEEP-PLAYER | high_fps.cpp reads at use; default on, off = engine behaviour; documented | - | kept |
| `overlay.ab_key` | Debug/A/B options | F10 | NoAB+Debug | KEEP-DEBUG | A/B hotkey; Debug flag set; no refs outside overlay | - | kept |
| `overlay.ab_badge` | Debug/A/B options | 1 | NoAB+Debug | KEEP-DEBUG | A/B indicator; Debug flag | - | kept |
| `debug.tint_draws` | Debug/Debug views | off | NoAB+Debug | KEEP-DEBUG | CLAUDE.md debug view; Debug; none in tests | - | kept |
| `debug.draw_records` | Debug/Debug views | 1 | NoAB+Hidden | REMOVE | Hidden, not Debug-flagged; "Off only to measure their cost"; read in device.cpp + overlay.cpp; many features need records | remove the switch (always on) | done: removed, constant in code |
| `debug.pace_loading` | Debug/Debug views | 0 | NoAB+Hidden | REMOVE | Hidden, not Debug-flagged; the old behaviour (loading screens paced) for A/B; menu-fps.md | remove | done: removed, constant in code |
| `debug.crash_markers` | Debug/Debug views | 0 | NoAB+Debug | KEEP-DEBUG | device-lost diagnostics; serializes GPU; Debug | - | kept |
| `debug.regions` | Debug/Debug views | 1 | NoAB+Debug | KEEP-DEBUG | capture labels for RenderDoc/PIX; Debug; default on | - | kept |
| `debug.names` | Debug/Debug views | 1 | NoAB+Debug+Restart | KEEP-DEBUG | restart; tests main.cpp; rvk_vktest --names | - | kept |
| `debug.record_threads` | Debug/Debug views | 1 | Debug | KEEP-DEBUG | A/B of the record threads (performance.md 14); Debug | candidate R once nothing needs the single-thread path | deferred: candidate R |
| `debug.ubershader_skip` | Debug/Debug views | 0 | Debug+NoAB | REMOVE | label says GLSL (stale: HLSL since 2026-10-29); bit mask for classes | D at most; rename or remove with debug.ubershader | kept: with debug.ubershader (label fixed) |
| `debug.ubershader` | Debug/Debug views | 0 | Debug | REMOVE | label "GLSL material shaders" stale (HLSL ubershader); default off; docs shader-permutations.md 8-10; 183/183 PS proven EQUAL | D: rename "HLSL material shaders" or remove when the ubershader is the default | kept: unfinished feature, not an A/B (label fixed to HLSL) |
| `debug.material_sources` | Debug/Debug views | 0 | Debug+NoAB | KEEP-DEBUG | tools ubershader_check.py / uber_hlsl_check.py read its output | - | kept |
| `debug.reverse_z` | Debug/Debug views | 1 | Debug+NoAB | KEEP-DEBUG | Debug+NoAB default on; shadow-bug.md bisects with false; off = game's 24 bit depth, other passes (RT, FSR, GI) assume reversed | candidate R once proven; mark Restart (native passes read it when built) | deferred: candidate R |
| `debug.batch_draws` | Debug/Debug views | 1 | Debug | KEEP-DEBUG | A/B (default on, grouped by pipeline) | candidate R | deferred: candidate R |
| `debug.constant_compare` | Debug/Debug views | 1 | Debug | REMOVE | V1 result of vulkanize/plan.md; default on, "a register write that changes nothing keeps the stage block"; off = old; no test | remove, always on | done: removed, constant in code |
| `debug.split_constants` | Debug/Debug views | 1 | Debug | REMOVE | V2 finished (plan.md 6); default on; off = V1 upload; kTranslatorVersion 4 | remove, always on | done: removed, constant in code |
| `debug.split_verify` | Debug/Debug views | 0 | Debug | KEEP-DEBUG | verification mode (mismatch counter must stay 0) | - | kept |
| `debug.inject_view` | Debug/Debug views | off | Debug | KEEP-DEBUG | injection test view; docs injection.md | - | kept |
| `rt.debug` | Debug/Debug views | off | NoAB+Debug RT | KEEP-DEBUG | rt-reflections.md debug views; REQ RT; Debug | - | deferred: one debug.view enum |
| `rt.reflection_tweaks` | Debug/Debug views | 0 | NoAB+Hidden | KEEP-DEBUG | Int bit mask, Hidden but NOT Flag::Debug: a stray value in settings.toml applies for players | add Flag::Debug (gated to default outside debug mode) | done: Flag::Debug added |
| `rt.scene_views` | Debug/Debug views | all | NoAB+Hidden | KEEP-DEBUG | Hidden, not Flag::Debug (same gating hole) | add Flag::Debug | done: Flag::Debug added |
| `water.enabled` | Graphics/Water | 1 | - | KEEP-PLAYER | run.ps1 baseline pins false, water-compare, mods main.lua; basic | - | kept |
| `water.quality` | Graphics/Water | high | - | KEEP-PLAYER | run.ps1 baseline pins medium (default high); basic | - | kept |
| `rt.reflections` | Graphics/Water | high | - RT | FIX | key prefix rt, category Water; also in rt.preset (medium preset = high) AND in the Original/Low/Medium graphics presets (Original sets off, Low off, Medium medium): a graphics preset flips rt.preset to custom; run.ps1 baseline pins medium while default is high | one owner: rt.preset or the graphics preset, not both; rename water.rt_reflections or move category to Ray tracing | done |
| `water.ocean` | Graphics/Water | fft | - | KEEP-PLAYER | run.ps1; scripts; basic (off / gerstner / fft) | - | kept |
| `water.ocean_quality` | Graphics/Water | high | - | KEEP-PLAYER | run.ps1 baseline pins medium (default high) | merge with water.quality (two quality knobs for one feature) or keep Advanced | kept |
| `water.sea_state` | Graphics/Water | auto | - | KEEP-PLAYER | auto / calm / moderate / rough / storm; docs water.md / ocean.md | - | kept |
| `water.reflections` | Graphics/Water | ssr | Advanced | FIX | sky / ssr: "Sky only" silently disables the RT reflections (rt.reflections acts "with water.reflections=ssr") with no greying; two overlapping knobs (also rt.reflections off = SSR) | grey rt.reflections when sky; or fold into rt.reflections (off = sky, low..high = traced; ssr = screen reuse) | partly: rt.reflections greys with the reason (needs); merge into one enum deferred |
| `water.wind_direction` | Graphics/Water | -1 | Advanced | KEEP-PLAYER | Int -1 = auto; Advanced; used by waves | check the overlay shows the compass / degrees, not a raw int | kept |
| `water.wave_scale` | Graphics/Water | 1 | Advanced | KEEP-PLAYER | wave height; Advanced, sensible knob | - | kept |
| `water.murkiness` | Graphics/Water | 1 | Advanced | REMOVE | Advanced, default 1; art coefficient, no refs outside docs | freeze (the water profile per body already sets the look) | done: removed, constant in code |
| `water.reflection_strength` | Graphics/Water | 1 | Advanced | REMOVE | Advanced, default 1; art coefficient | freeze | done: removed, constant in code |
| `water.debug` | Debug/Debug views | off | NoAB+Debug | KEEP-DEBUG | water-compare.ps1, water_stats.py; Debug | merge the seven *.debug views into one `debug.view` enum (water / shadows / gi / gbuffer / materials / lighting / framegen) | deferred: one debug.view enum |
| `shadows.enabled` | Graphics/Shadows | 1 | - | KEEP-PLAYER | run.ps1 baseline pins false (default true); shadow-compare | - | kept |
| `shadows.filter` | Graphics/Shadows | low | - | KEEP-PLAYER | settings_tests, shadow-compare; baseline pins high (default low) | - | kept |
| `shadows.map_size` | Graphics/Shadows | 2048 | - | KEEP-PLAYER | settings_tests; baseline pins auto (default 2048); Low/Medium/High labels | - | kept |
| `shadows.softness` | Graphics/Shadows | 1 | Advanced | REMOVE | Advanced coefficient, no script / test; docs shadows.md 2 refs | freeze default | done: removed, constant in code |
| `shadows.cascade_blend` | Graphics/Shadows | 0.2 | Advanced | REMOVE | Advanced coefficient, no refs outside docs | freeze 0.2 | done: removed, constant in code |
| `shadows.fade` | Graphics/Shadows | 0.35 | Advanced | REMOVE | Advanced coefficient | freeze 0.35 | done: removed, constant in code |
| `shadows.bias` | Graphics/Shadows | 0.5 | Advanced | KEEP-PLAYER | Advanced; acne vs peter-panning fix a player can need | - | kept |
| `shadows.range` | Graphics/Shadows | 0 | Advanced | KEEP-PLAYER | Advanced, meters; auto 0; useful | - | kept |
| `shadows.split` | Graphics/Shadows | 0.4 | Advanced | REMOVE | Advanced; cascade split (engine's own cascades via shadows.map_size/range) | freeze 0.4 | done: removed, constant in code |
| `shadows.rt` | Graphics/Shadows | off | - RT | KEEP-PLAYER | Shadows RT tier; REQ ray query; default off; docs rt-shadows.md | - | kept |
| `shadows.rt_quality` | Graphics/Shadows | medium | - RT | KEEP-PLAYER | member of rt.preset; REQ RT | - | kept |
| `shadows.rt_softness` | Graphics/Shadows | 1 | Advanced RT | REMOVE | Advanced, sun cone half angle, REQ RT | freeze (or Debug) | done: removed, constant in code |
| `shadows.rt_range` | Graphics/Shadows | 0 | Advanced RT | REMOVE | Advanced, REQ RT; also an rt.preset member (Low sets 15) | internal to the preset: Hidden | done: Hidden, internal to the preset |
| `rt.lod_range` | Graphics/Lighting | 60 | Advanced RT | REMOVE | ladder: measured no gain (rt-lod.md); only the Low preset (0.3 scale) uses it; Advanced | internal constant | done: removed, constant in code |
| `rt.lod_far_range` | Graphics/Lighting | 150 | Advanced RT | REMOVE | same ladder | internal constant | done: removed, constant in code |
| `rt.lod_bias` | Graphics/Lighting | 2 | Advanced RT | REMOVE | same ladder | internal constant | done: removed, constant in code |
| `rt.lod_scale` | Graphics/Lighting | 0 | Advanced RT | FIX | semantics 0 = off but 1 = base distances (0.3 = starts close): inverted-looking; measured no gain except the Low preset (rt-lod.md); Advanced but only the Low preset needs it | Hidden / Debug; the Low preset sets it | done |
| `rt.memory_budget` | Graphics/Lighting | 0 | Advanced RT | KEEP-PLAYER | MiB limit of the traced world, Advanced, preset member; settings_tests | - | kept |
| `rt.preset` | Graphics/Lighting | medium | - RT | FIX | category Lighting but drives water, shadows, GI, materials; "custom" is set only by onChange (a loaded settings.toml with rt.preset=medium + rt.reflections=medium shows medium although it is not): the match is not re-checked at load; selecting custom itself does nothing; rt.lod_* members make a changed value "custom" although the user never sees them | category "Ray tracing"; re-run matchRtPreset after load; make "custom" read-only (shown, not selectable); drop the hidden-from-player members from the match | done |
| `shadows.rt_wind` | Graphics/Shadows | 1 | Advanced RT | REMOVE | Advanced default on; off only for A/B | remove | done: removed, constant in code |
| `shadows.debug` | Debug/Debug views | off | NoAB+Debug | KEEP-DEBUG | shadow-compare.ps1; Debug | see water.debug | deferred: one debug.view enum |
| `gi.enabled` | Graphics/Lighting | 0 | - RT | KEEP-PLAYER | gi-reference.ps1, mods main.lua; REQ RT; default off | - | kept |
| `gi.quality` | Graphics/Lighting | medium | - | KEEP-PLAYER | member of rt.preset; tests settings_tests | - | kept: gi.quality / gi.screen merge deferred |
| `gi.strength` | Graphics/Lighting | 1 | Advanced | KEEP-PLAYER | Advanced; "Bounced light strength" a reasonable taste knob | - | kept |
| `gi.lightmaps` | Graphics/Lighting | keep | Advanced | KEEP-PLAYER | gi-reference.ps1; keep / replace baked light; Advanced | - | kept |
| `gi.characters` | Graphics/Lighting | 1 | Advanced | REMOVE | Advanced default on; off only for A/B | remove | done: removed, constant in code |
| `gi.screen` | Graphics/Lighting | medium | Advanced RT | FIX | label "Lighting detail" vs gi.quality "Lighting quality": indistinguishable to a player; Advanced yet a rt.preset member; REQ RT | rename "Screen-space light detail"; or merge into gi.quality | done |
| `gi.bleed` | Graphics/Lighting | 0.5 | Advanced RT | REMOVE | Advanced 0.5 tuning | freeze | done: removed, constant in code |
| `gi.directional` | Graphics/Lighting | 0.5 | Advanced RT | REMOVE | Advanced 0.5 tuning | freeze | done: removed, constant in code |
| `gi.lights` | Graphics/Lighting | world | Advanced RT | REMOVE | Advanced nearby / world; world is the replace-mode default, nearby the old mode; gi-reference.ps1 passes it | keep world; script passes -> Debug if still needed | done: removed, constant in code |
| `gi.emissive` | Graphics/Lighting | 1.0 | Advanced RT | REMOVE | Advanced 0..4 (lava as a light source); default 1 | freeze | done: removed, constant in code |
| `gi.foliage` | Graphics/Lighting | off | Advanced RT | KEEP-PLAYER | Advanced off/near/far; costs CPU+GPU, a real perf trade | rename away from "Leaves in the traced light" overlap with gi.cutouts (gi.cutouts removed) | kept |
| `gi.shadow_fill` | Graphics/Lighting | 0.5 | Advanced RT | REMOVE | Advanced 0.5 tuning | freeze | done: removed, constant in code |
| `gi.cutouts` | Debug/Debug views | tested | Debug+NoAB RT | REMOVE | Debug: solid (cheap) vs tested (default); overlaps gi.foliage; no refs | remove (tested) | done: removed, constant in code |
| `gi.reference` | Debug/Debug views | off | NoAB+Debug RT | KEEP-DEBUG | gi-reference.ps1 + tools/gi_reference.py; REQ RT; Debug | - | kept |
| `gi.reference_spp` | Debug/Debug views | 4 | Debug+NoAB | KEEP-DEBUG | gi-reference.ps1; Debug | - | kept |
| `gi.reference_max` | Debug/Debug views | 1024 | Debug+NoAB | KEEP-DEBUG | gi-reference.ps1; Debug | - | kept |
| `gi.reference_bounces` | Debug/Debug views | 6 | Debug+NoAB | KEEP-DEBUG | gi-reference.ps1; Debug | collapse spp/max/bounces into script args if the layout allows | kept |
| `gi.light_count` | Debug/Debug views | 128 | Debug+NoAB | REMOVE | Debug Int <= 128; gi.lights world mostly covers it | remove | done: removed, constant in code |
| `gi.debug` | Debug/Debug views | off | NoAB+Debug RT | KEEP-DEBUG | Debug view | see water.debug | deferred: one debug.view enum |
| `ao.enabled` | Graphics/Lighting | 0 | - | KEEP-PLAYER | basic toggle; works without RT | - | kept |
| `ao.mode` | Graphics/Lighting | replace | Advanced | FIX | label "Game's ambient occlusion" but the options are Replace/Add | label "Ambient occlusion mode" | done |
| `ao.strength` | Graphics/Lighting | 1 | Advanced | KEEP-PLAYER | Advanced; read in gbuffer.cpp | - | kept |
| `ao.radius` | Graphics/Lighting | 1 | Advanced | REMOVE | Advanced, no refs; ao.strength remains | freeze | done: removed, constant in code |
| `lighting.clustered` | Debug/Lighting | 1 | Restart+Debug | FIX | Debug+Restart master switch of the Full light table, default on, invisible to players; lighting.style / light_shadows silently need it; run.ps1 pins false | decide: constant on in Full (remove) or a player toggle "Light table (Full)"; tests unaffected | kept: Debug + Restart master; the needs model switches it on for the player |
| `lighting.enabled` | Debug/Lighting | 1 | Debug | KEEP-DEBUG | run-time A/B of the light table; Debug; docs only | - | kept |
| `lighting.full_lit` | Debug/Lighting | 1 | Debug | KEEP-DEBUG | run.ps1 baseline pins false (parity with D3D9) so tests depend on it; docs gpu-scene.md; default on in Full | - | kept: the test baseline pins it |
| `lighting.trust_table` | Debug/Lighting | 0 | Debug | REMOVE | Debug, default OFF, "a small CPU saving only"; the renamed lighting.table_complete (kRenamedSettings maps the old key); settings_tests checks the rename | remove, or make it the default and drop the switch; drop the rename shim | done: constant (off), old name lighting.table_complete in kRemovedSettings |
| `lighting.tree_merge` | Debug/Lighting | 1 | Debug | REMOVE | Debug default on, "Run-time switch (A/B)"; finished measured feature (gpu-scene.md 9.9) | remove, always on | done: removed, constant in code |
| `lighting.style` | Graphics/Lighting | original | - | FIX | player setting but acts in Full + clustered only, no requirement or greying in Wrapper; "physical" shader code compiled only if the session STARTS with it: a later switch changes only the lamp distance law, yet it is not Restart | Restart (or two-level: restartGroups like renderer.mode); show only in Full | done |
| `lighting.debug` | Debug/Debug views | off | NoAB+Debug | KEEP-DEBUG | Debug view | see water.debug | deferred: one debug.view enum |
| `debug.light_collect` | Debug/Lighting | 96000 | Advanced+Debug | REMOVE | Debug+Advanced Float 96000 (clamp 16,000..96,000): a constant | remove | done: removed, constant in code |
| `lighting.light_shadows` | Graphics/Lighting | nearby | - RT | KEEP-PLAYER | REQ RT; scripts mods main.lua; player toggle for lamp shadows | see FIX list (Full only, no requirement) | kept |
| `lighting.light_shadow_quality` | Graphics/Lighting | medium | Advanced | FIX | Advanced, not a member of rt.preset, no RT requirement (acts only through light_shadows RT) | add to rt.preset; REQ RT | done |
| `lighting.light_shadow_baked` | Graphics/Lighting | 0.8 | Advanced | REMOVE | Advanced 0.8 (added with the baked-light darkening commit) | freeze | done: removed, constant in code |
| `lighting.light_shadow_radius` | Graphics/Lighting | 20 | Advanced | REMOVE | Advanced 20 cm lamp size | freeze | done: removed, constant in code |
| `gbuffer.debug` | Debug/Debug views | off | NoAB+Debug | KEEP-DEBUG | Debug view; docs rtgi.md | see water.debug | deferred: one debug.view enum |
| `materials.pack` | Graphics/Materials | 1 | Restart | KEEP-PLAYER | Restart; matpack-*.ps1; default on | - | kept |
| `materials.quality` | Graphics/Materials | medium | - RT | FIX | label "Material pack quality" also selects the traced reflections tier (High needs RT: optionRequirements) and overlaps materials.pack (off) | rename "Material reflections"; materials.pack stays the loader switch | done |
| `materials.rt_reflections` | Graphics/Materials | high | - | FIX | rt.preset member; acts only at materials.quality=high, name overlaps materials.quality / rt.reflections; no greying when quality < high | grey while quality != high; rename "Traced material reflection detail" | done |
| `materials.rt_range` | Graphics/Materials | 150 | Advanced | KEEP-PLAYER | member of rt.preset (Advanced, m) | - | kept |
| `materials.rt_strength` | Graphics/Materials | 1 | Advanced | REMOVE | Advanced 1.0 | freeze | done: removed, constant in code |
| `materials.specular` | Graphics/Materials | 1 | Advanced | REMOVE | Advanced 1.0; pack strength scaling | freeze (the pack itself carries strength) | done: removed, constant in code |
| `materials.environment` | Graphics/Materials | 1 | Advanced | REMOVE | Advanced 1.0 | freeze | done: removed, constant in code |
| `materials.default_roughness` | Graphics/Materials | 0.7 | Advanced | REMOVE | Advanced 0.7 | freeze | done: removed, constant in code |
| `materials.derive_from_specular` | Graphics/Materials | 1 | - | KEEP-PLAYER | derive-perf.ps1; default on; no player harm | - | kept |
| `materials.derive_min` | Graphics/Materials | 0.25 | Advanced | REMOVE | Advanced 0.25 | freeze | done: removed, constant in code |
| `materials.derive_max` | Graphics/Materials | 0.9 | Advanced | REMOVE | Advanced 0.9 | freeze | done: removed, constant in code |
| `materials.budget` | Graphics/Materials | 256 | Restart+Advanced | KEEP-PLAYER | Restart+Advanced, MiB; useful on small GPUs | - | kept |
| `materials.wetness` | Graphics/Materials | 1 | Advanced | REMOVE | Advanced; scales the pack's own wetness; overlaps materials.rain_wetness | remove | done: removed, constant in code |
| `materials.glass` | Graphics/Materials | 1 | - | FIX | acts only with quality high + RT; basic-visible toggle that does nothing otherwise | fold into quality high or grey; default on | done |
| `materials.rain_wetness` | Graphics/Materials | 1 | - | FIX | non-Advanced, acts only with quality high (traced pass); two near-twins (wetness, puddles) | fold rain_wetness + puddles into one "Rain" slider; grey below High | done |
| `materials.puddles` | Graphics/Materials | 0.7 | - | FIX | same: acts only with the traced pass at High | fold into one "Rain" setting | done: folded (puddles = rain_wetness x 0.7) |
| `materials.rain_force` | Debug/Debug views | 0 | NoAB+Debug | KEEP-DEBUG | Debug; fakes rain; docs material-rt.md | - | kept |
| `materials.alternate` | Debug/Debug views | 0 | NoAB+Debug | KEEP-DEBUG | matpack-alternate.ps1 proof tool | - | kept |
| `materials.debug` | Debug/Debug views | off | NoAB+Debug | KEEP-DEBUG | Debug view | see water.debug | deferred: one debug.view enum |
| `upscale.mode` | Graphics/Image | off | - | KEEP-PLAYER | package.ps1, tests; central | - | kept |
| `upscale.provider` | Graphics/Image | fsr3 | Advanced | KEEP-PLAYER | fsr3 / fsr4 (greyed w/o RX 9000?); Advanced | - | kept |
| `upscale.object_motion` | Graphics/Image | characters | Advanced | KEEP-DEBUG | Advanced characters / all / off; default characters, an A/B of what feeds FSR; no tests | move to Debug | done: moved to Debug |
| `upscale.reactive` | Graphics/Image | 1 | - | REMOVE | non-Advanced "Clean up effects": the FSR reactive mask; default on, no player reason to turn off | demote to Advanced / Debug | done: demoted to Advanced |
| `upscale.reactive_cutoff` | Graphics/Image | 0.08 | Advanced | REMOVE | Advanced FSR mask tuning 0.08; docs upscaling.md | freeze | done: removed, constant in code |
| `upscale.reactive_scale` | Graphics/Image | 1 | Advanced | REMOVE | Advanced FSR mask tuning 1 | freeze | done: removed, constant in code |
| `upscale.sharpness` | Graphics/Image | 0.4 | - | KEEP-PLAYER | RCAS strength; a standard upscaler slider | - | kept |
| `upscale.exposure` | Graphics/Image | engine | Advanced | KEEP-DEBUG | Advanced engine / auto; engine is the validated path, auto a fallback | move to Debug | done: moved to Debug |
| `upscale.jitter_flip` | Debug/Debug views | none | NoAB+Debug | KEEP-DEBUG | Debug test | - | kept |
| `upscale.post_full_size` | Debug/Debug views | 1 | Debug | REMOVE | Debug default on; off = "the old blocky path" (upscaling.md 16) | remove | done: removed, constant in code |
| `upscale.cutout_thicken` | Graphics/Image | 0 | Advanced | KEEP-DEBUG | Advanced, default 0: experimental, "steadier but heavier nets" | move to Debug | done: moved to Debug |
| `upscale.alpha_dither` | Debug/Debug views | 0 | Debug | KEEP-DEBUG | Debug, default off; experimental hashed cut-out (softer) | keep as experiment or remove if upscaling.md 17 concluded | kept |
| `framegen.mode` | Graphics/Image | off | - FG | KEEP-PLAYER | REQ FG; optionRequirements; framegen_tests, run.ps1 | - | kept |
| `framegen.manual` | Debug/Frame generation | 0 | Debug FG | KEEP-DEBUG | Debug; framegen_tests; capture path for <name>_gen.png | - | kept |
| `framegen.debug` | Debug/Debug views | off | NoAB+Debug | KEEP-DEBUG | Debug view | see water.debug | deferred: one debug.view enum |
| `lod.trees` | Graphics/Detail | 2 | - | KEEP-PLAYER | run.ps1 baseline pins 1 (default 2); docs lod.md | - | kept |
| `lod.meshes` | Graphics/Detail | 2 | - | KEEP-PLAYER | run.ps1 baseline pins 1 (default 2) | - | kept |
| `debug.sky_cube_interval` | Debug/Debug views | 100 | Debug | REMOVE | Debug Int 100 ms: a constant; one doc ref | remove | done: removed, constant in code |
| `debug.shadow_stride` | Debug/Debug views | 2 | Debug | KEEP-DEBUG | Debug Int default 2 but changes what players see (sun cascades update every 2nd frame): applies as default when debug mode is off, fine; 13 docs; tests | keep as Debug; consider a player-facing "Shadow update rate" only if wanted | kept |
| `debug.rt_shadow_sync` | Debug/Debug views | 0 | Debug | KEEP-DEBUG | Debug, default off; rt-shadows.md 7 | - | kept |
| `debug.rt_skin` | Debug/Debug views | 1 | Debug | KEEP-DEBUG | Debug, default on; off = old capture path | candidate R | deferred: candidate R |
| `debug.rt_skin_verify` | Debug/Debug views | 0 | Debug | KEEP-DEBUG | Debug verification | - | kept |
| `native.frame_tap` | Debug/Debug views | 0 | NoAB+Debug | KEEP-DEBUG | N5.0 op tap; Debug; docs | - | kept |
| `native.static_ops` | Debug/Debug views | 0 | NoAB+Debug | KEEP-DEBUG | bisect switch of Full's groups (renderer.mode=full turns them on); docs engine-frame README | - | kept |
| `native.static_ops_compare` | Debug/Debug views | 0 | NoAB+Debug | KEEP-DEBUG | compare must stay "mismatches: none"; used in CI-like runs | - | kept |
| `native.scene_ops` | Debug/Debug views | 0 | NoAB+Debug | KEEP-DEBUG | bisect; mods main.lua (rvk_*), docs; renderer.mode=full overlaps | - | kept |
| `native.scene_ops_compare` | Debug/Debug views | 0 | NoAB+Debug | KEEP-DEBUG | mods main.lua; docs | - | kept |
| `native.scene_ops_groups` | Debug/Debug views | 131071 | NoAB+Debug | KEEP-DEBUG | Int mask, default 131071; docs README 6 | - | kept |
| `native.mirror` | Debug/Debug views | 0 | NoAB+Debug | REMOVE | Int 0/1/2, default 0; 1 and 2 = old N5.x behaviours "for A/B" (opt-plan.md O1) | remove | done: removed, constant in code |
| `native.lean` | Debug/Debug views | 1 | NoAB+Debug | KEEP-DEBUG | A/B of lean ops, default on; native_tests | - | kept |
| `native.lean_check` | Debug/Debug views | 0 | NoAB+Debug | KEEP-DEBUG | Int interval; compare requires it; native_tests | - | kept |
| `native.packet_cache` | Debug/Debug views | 1 | NoAB+Debug | FIX | Int 0..4 mixes on/off, verify every 10th, verify every frame, "without the variant draws"; read in device.cpp; docs retained-signals.md 8 | split: bool + debug.packet_cache_verify (+ bit for A/B); otherwise D | done: Enum (off / on / verify_10 / verify_all / no_variants), old integers load as the index |
| `native.gpu_scene` | Debug/Debug views | 0 | NoAB+Debug | REMOVE | default 0; 1 = verify, 2 = upload with NO consumer ("costs ~0.07-0.1 ms for nothing"; gpu-scene.md); one doc | remove until the consumer exists | done: removed, constant in code |
| `native.cpu_opts` | Debug/Debug views | 255 | NoAB+Debug | REMOVE | bit mask default 255, every bit an A/B (performance.md 22) | remove | done: removed, constant in code |
| `native.lean_restore` | Debug/Debug views | 1 | NoAB+Debug | REMOVE | measurement-only A/B of the per-op device restore; shadow-bug.md lists it among suspects | remove | done: removed, constant in code |
| `native.retained` | Debug/Debug views | 0 | NoAB+Debug | REMOVE | default off since V3: "no CPU gain" (retained-signals.md, plan.md 7) | remove the feature code + switch | done: removed, constant in code |
| `native.retained_check` | Debug/Debug views | 0 | NoAB+Debug | REMOVE | belongs to native.retained | remove with it | done: removed, constant in code |
| `native.retained_groups` | Debug/Debug views | 0x2ff | NoAB+Debug | REMOVE | belongs to native.retained | remove with it | done: removed, constant in code |
| `native.lean_effects` | Debug/Debug views | 1 | NoAB+Debug | REMOVE | Debug default on = A/B of O2c/O3 | remove, always on | done: removed, constant in code |
| `native.coverage` | Debug/Debug views | 0 | NoAB+Debug | KEEP-DEBUG | diagnostic counter; also forced by frame_tap / compares | - | kept |
| `debug.fs_read_cache` | Debug/Debug views | 64 | Debug | REMOVE | KiB, default 64: load_accel constant; docs | remove (hardcode) | done: removed, constant in code |
| `debug.fast_absorb` | Debug/Debug views | 1 | Debug | REMOVE | Int 0/1/2; bit-identical, 2 = self-compare | remove the switch (keep compare as test) | done: removed, constant in code |
| `debug.sound_compressed` | Debug/Debug views | 0 | NoAB+Debug | REMOVE | default off; "audio unchecked": never verified | decide: verify then enable, else remove | done: removed, constant in code |
| `debug.stream_stats` | Debug/Debug views | 0 | NoAB+Debug | KEEP-DEBUG | 1 = timings, 2 = verify; load_accel diagnostics | - | kept |
| `debug.xinput_interval` | Debug/Debug views | 1000 | Debug | REMOVE | Int ms, default 1000: constant | remove | done: removed, constant in code |
| `debug.ocean_spd` | Debug/Debug views | 1 | NoAB+Debug | REMOVE | A/B of a finished feature (compute mips vs blits) | remove | done: removed, constant in code |
| `debug.fast_sort` | Debug/Debug views | 1 | Debug | REMOVE | 0/1/2 finished (bit-identical); 2 = compare mode | remove (compare via test) | done: removed, constant in code |
| `debug.prefetch_stride` | Debug/Debug views | 4 | Debug | KEEP-DEBUG | Int default 4: CacheEntities stride, gameplay-affecting (pop-in), gated to default w/o debug; performance.md 14 | keep Debug | kept |
| `lod.view` | Graphics/Detail | 2 | - | KEEP-PLAYER | run.ps1 baseline pins 1 (default 2); mods main.lua | - | kept |
