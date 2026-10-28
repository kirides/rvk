# Enhanced shadows show nothing (root cause CONFIRMED and fixed 2026-10-28; the original code-reading analysis follows)

## 0. Result (2026-10-29)

* **Cause (section 1, confirmed in game):** `shadow_mask.vert` read `pc.draw.posFix.zw` (the reversed-depth remap added by d120038,
  2026-10-25), but a replaced draw's `pc.draw` is the replacer's own block (`DrawReplacement::block`), here `enh::ShadowBlock`, not a
  DrawBlock: the "remap" was `cascade[10].z / .w` = row 2 of the third cascade's matrix. Measured (TEMP log, 7 rvkcompare spots,
  Wrapper): z + w = 0.71 .. 1.27 in every pose, so the triangle at z = 1 was pushed to clip z 1.7 .. 2.3 > w, clipped by the far plane
  (no depth clamp in any pipeline): no mask pixel, no stencil; the engine's cascade draws are skipped, so no sun shadows at all (what
  remains: the terrain variance term + lightmaps). Not Full-specific: Wrapper has the same replacer, shader and block; reverse-Z on /
  off, filter, map size are irrelevant (same vertex shader).
* **Proof (same tree, same dll otherwise, A/B on the shader line; `out/shadowfix/old_*`, `vwv_*`, `afe_*`, `crop_*`):** with the old
  line the enhanced-shadows image has **no** hero / tree / object shadows (`crop_harbour_old_new_engine.png`: the hero's shadow on the
  dock, `crop_inland_old_new_engine.png`: the palm / trunk shadows on the path are absent); with the fix both appear, matching the
  engine's own mask (`shadows.enabled=false`) with smoother edges (luma diff of fixed vs engine 0.5 at `harbour_docks`, 3.6 - 6 at
  `shadow_inland_b` where the hero's idle pose differs); `shadows.debug=cascades` changes the image by 25 / 2.7 luma (fixed) vs 3.8 /
  2.1 (old, no mask pixel). rvk.log has `enh: shadows: mask pass with 3 cascades` in both (the replacer is reached; the output is lost).
* **Not explained:** one build of the same sources from before the V4 commits (d3d9.dll 22:52, V4 work in progress) still showed the
  shadows with the old line (`out/shadowfix/bw_*`, `bwc_*`), i.e. the garbage read was harmless there; the tree at HEAD (e00d888) +
  working changes clips reliably. So the regression window is d120038 (the line) .. V4 (what made the clip happen); no bisect was
  needed since the fix is by construction independent of that.
* **Fix (5c4731f):** `ShadowBlock` gets a `depthRemap` vec4 (last member; filled from `Device::depthRemap`, as `enh/water` does for
  its `posFix`), the block's GLSL declaration moved to `shaders/shadows/shadow_block.glsl` (included by the vertex and fragment shader:
  one definition), `shadow_mask.vert` reads `ShadowBlock(uvec2(pc.draw)).depthRemap`. Regression guard: rvk_tests
  `shadows_block_glsl_matches_cpp` (SPIR-V member offsets of the shared block, via `shaders/test/shadow_layout.comp`, vs
  `enh::ShadowBlock`).
* **Verification of the fix** (1600x900, Wrapper and Full, spots `shadow_inland_b` + `harbour_docks`, `show_actors=1`, d3d9.dll
  ED1280B2): shadows present and equal to the engine's mask in Wrapper (`vwv_*`) and Full (`vfcmp_*`: 0.2 luma at the docks); cascades
  debug view works in both, Full = Wrapper (`vfc_*` vs `vwc_*`: 0.14 at the docks); reverse_z on / off: identical (`vwrz1c` vs `vwc`
  0.16 / `vwrz1` vs `vwv` 0.1 at the docks); filter low: 0.12; `shadows.rt=near` + validation at `shadow_inland_b`: RT shadows fine, 0
  validation messages, 0 DEVICE_LOST; Full `native.static_ops_compare` / `scene_ops_compare` / `lean_check=10` + `-Validation`:
  `mismatches: none` everywhere, no validation message; Wrapper `-Validation`: none.
* **Separate finding (not caused by this fix):** `shadows.rt=near` at the `harbour_docks` spot (Wrapper) ends in `VK_ERROR_DEVICE_LOST`
  during the run (2 of 2 runs, also with the OLD mask shader, so independent of the fix; without validation; with validation at
  `shadow_inland_b` clean). Tree state: V4 / debug-names work in progress. Log: `out/shadowfix/rvk_vwrt*.log`.
* Other replacers: only the water (`enh::WaterBlock`, begins with a DrawBlock-compatible prefix and fills posFix: `water.cpp:518`)
  and the shadow mask set `DrawReplacement::block`; no other shader reads `pc.draw.posFix` from a replaced draw (grep of shaders/).
  The WaterBlock's prefix is the old 192-byte DrawBlock (before V2's psC5 at 192): harmless, because no water shader reads
  `pc.draw.psC5` (split draws point `psw[1]` at the real DrawBlock, draw.cpp ~1236).
* The candidates of the coordinator's second hypothesis (material draws' mask sampler slot, V2 ABI change) were not needed: the
  materials sample the engine's mask target through their own DrawBlock, unchanged; the fixed mask image reaches them (shadows visible).
* Tooling notes: a hand-written replacer that sets `DrawReplacement::block` must give the block everything its shaders read from
  `pc.draw` (for the vertex stage too); `rvk_glslc` includes resolve against the including file's directory.

# Original analysis (code reading, 2026-10-28)

Status: code and docs reading only (no game run, no build). [C] = read in code, [I] = inferred, [G] = would need a game run.
Report (final form): with `shadows.enabled=true` there are **no enhanced sun shadows at all** (only baked / lightmap ones), and
`shadows.debug=cascades` shows nothing either; independent of reverse-Z, filter, map size. First framing ("NPC / object shadows
missing") is superseded; its candidate list is kept in section 4 as secondary.

## 1. Prime suspect: `shadow_mask.vert` reads a DrawBlock that the replacer does not supply (since d120038, 2026-10-25) [C]

`shaders/shadows/shadow_mask.vert` (the whole mask is one procedural triangle at z = 1):

```glsl
gl_Position = vec4(uv * 2.0 - 1.0, 1.0, 1.0);
gl_Position.z += pc.draw.posFix.z + pc.draw.posFix.w;   // added by d120038 (reverse-Z)
```

* `pc.draw` is declared as `RvkDraw` = the **DrawBlock** (`rvk_abi.glsl`: `tex[20]`, `smp[20]`, `vec4 posFix` at byte 160, ...).
* For a replacer draw, `Device::prepareDraw` sets `drawBlock = rep.block != 0 && replaced ? rep.block : db.address`
  (`src/d3d9/draw.cpp` ~1227). The shadow replacer's `rep.block` is its **`enh::ShadowBlock`** (`src/enh/shadows.cpp` drawMask:
  `out.block = a.address`), a 432-byte struct that starts with `float cascade[12][4]` and has no DrawBlock prefix.
* So the vertex shader reads `posFix.zw` from byte 168..175 of the ShadowBlock = **`cascade[10].z / .w`** = components of the
  depth row (row 2) of the *third* cascade's DepthToShadowMatrix (`z` = d(shadow depth)/d(depth-map value), `w` = the shadow depth
  at the camera, ~0.9 with z minimum forced to -100,000). Their sum is not 0, so the triangle's clip z = 1 + (about -1 ... +1.5):
  * sum > 0: z > w, clipped by the near/far test (no `depthClampEnable` anywhere: `grep depthClampEnable src` is empty);
  * -1 < sum < 0: z' = 1 + sum is far below the scene's hardware depth (non-reversed D3D depth is ~0.998 at 100 m), the z GREATER
    test fails for all geometry pixels;
  * sum < -1: clipped.
  Either way **the mask triangle draws no pixel**, and its stencil INCR does not happen either.
* Consequence in the frame: the replacer has `Skip`ped the engine's SSM cascade draws (no sun cascade shadows from the engine
  any more), the procedural mask draws nothing, the engine's following VSSM then sees stencil 0 everywhere and draws the
  **terrain variance term over the whole screen**. What remains = terrain variance shadows + lightmaps (+ SSAO red channel) =
  "only the baked ones"; `shadows.debug=cascades` (which only changes the mask shader's output) shows nothing; reverse-Z on/off,
  filter, map size are all irrelevant (the vertex shader is the same). This matches every user observation.
* Why nobody saw it: the reverse-Z commit (d120038, 2026-10-25) edited `shadow_mask.vert` together with `water_ocean.vert` /
  `static_mesh.vert`. The water replacer fills `posFix` in its own block (`water.cpp:510-518`, `device.depthRemap(w.posFix[2],
  w.posFix[3])`), the native opaque pass too; `Shadows::drawMask` / `ShadowBlock` were **not** touched (`git log d120038..HEAD --
  shaders/shadows src/enh/shadows.cpp` is empty). All later checks of the mask compared *draws* (README.md 12/13: "composed draws
  equal", "the enhanced mask acts on our draws as on the engine's" = the replacer is reached), never an image with the mask visibly
  contributing (the take-over image pairs had the enhanced mask on **both** sides, i.e. equally broken; the V2 "enhancements on,
  screenshots correct (viewed)" check would not have shown missing cascade shadows on a quick look).
* **This is not Full-specific**: Wrapper has the same replacer, shader and block. "Presumably fine in Wrapper" is unverified; the A/B
  below settles it. (If Wrapper looks fine, section 3's Full-only candidates move up.) All builds in `build/*` (Before/After, V1base,
  V1before, V3base, V4base, S1-S3, RelWithDebInfo) are dated 2026-10-28, i.e. after d120038: **none predates the bug, so the
  V1/V2 builds cannot bisect it**; a bisect needs a build of d120038's parent (c.f. `git log --format=%h -1 d120038~1`), see 5.

Discriminator that needs no code change (rvk.log of any Wrapper or Full run with `shadows.enabled=true`, once a shadow-casting
scene is up): `enh: shadows: mask pass with 3 cascades (map ...^2), splits ..., variance map yes` is printed from `drawMask` **right
before the procedural draw is handed to prepareDraw**. Present = the replacer is reached and decides correctly, the output is lost
afterwards (this section: vertex clip). Absent = the replacer never gets a mask cascade draw (section 3).

Fix direction (not applied, no source edits allowed here): do not read `pc.draw` in the vertex shader. Either (a) give `ShadowBlock`
a DrawBlock-compatible prefix / a `posFix` vec4 filled with `device.depthRemap(z, w)` and make the vert read `ShadowBlock`, or (b)
take the remap from the FrameBlock like `static_mesh.vert` does (`RVK_FRAME_REVERSE_Z`, `depthParams`): the mask quad sits at
z = 1, which under reversed depth is `w - k z` with k = 1 - near / far. Note the mask target's depth buffer must be the reversed one
(`boundDepthReversed()`), the same condition as for every other consumer. After the fix: a unit test cannot see it; check
`shadows.debug=cascades` shows the 25 / 50 / 75 % bands.

## 2. Answers to the extra questions about the user's settings.toml

Settings seen by the coordinator: `[shadows] enabled=false, filter=high, map_size=4096`; `[native] shadows=true, static_ops, scene_ops,
lean_restore=false, retained=true`; `[debug] reverse_z=false, shadow_stride=1, rt_shadow_sync=true`; `[rt] source="mirror"`; mode full.

1. **`native.shadows=true` with Full**: harmless to the engine's cascades / mask [C]. `enh::NativeShadows` (N4) only adds a pass
   `native.shadows` at AfterDepthPrepass that renders its own three 2048 D32 cascades of the *scene mirror's static mesh parts*
   (`native_shadows.cpp`); the engine's `RenderShadowMaps`, its cascade draws, the mask replacer and the take-over are untouched.
   The only reader of its block is `NativeOpaque::colourPass` (`native_opaque.cpp:417`), i.e. **only with `native.opaque=true`**
   (not in the settings seen). Without `native.opaque` the pass is pure cost (and it never shadows anything). **If `native.opaque`
   is also on** it is a strong alternative explanation of the *original* complaint: NativeOpaque's DrawReplacer would skip the
   composed StaticMesh solid draws of listed parts (`replace()` -> Skip) and its colour pass shades them with the native cascades
   (statics only: `Characters, trees and grass are not casters yet`, native_shadows.h) instead of the engine mask, so NPC,
   tree and grass shadows would be missing and the enhanced mask would not be sampled for statics. Check `native.opaque` in the real
   file. All `native.*` / `debug.*` keys are `Flag::Debug`: with `overlay.debug_mode=false` they apply as defaults
   (`Setting::effective()`), so the file's `native.shadows` etc. only count in debug mode (check `overlay.debug_mode`).
2. **`shadows.enabled` false despite the toggle**: no code path forces it [C]. It has no `requirements`, no `Flag::Debug`, no lock
   (bootstrap locks only `renderer.mode` and the vulkan keys), no mode-dependent rule; `Shadows::apply`/`HalDevice::applyShadows`
   follow the registry value in both modes. Ways the file can hold `false`: the **"Original" preset** (`graphicsPresets()`:
   `shadows.enabled=false`, `shadows.map_size=game`; the file's `filter=high, map_size=4096` looks like "Ultra" with enabled
   switched off afterwards, or preset Original then edited), a toggle in the overlay followed by an exit before the next session
   (save is debounced in `serviceSettings`), or A/B: the file stores **side A** under `[shadows]` and side B under `[ab.shadows]`;
   `flipAB` swaps only A/B-eligible settings (shadows.enabled is eligible). Look for an `[ab.shadows]` table and for rvk.log's
   `settings: N registered, non-default: [...]` and `enh: shadows: on|off (...)` lines. With `enabled=false` the engine's own cascades
   are drawn (composed, compare-clean), so "no sun shadows at all" cannot come from that file state alone: that report belongs to
   section 1 (enabled=true at the time of the report).
3. **`native.retained=true` + `lean_restore=false`** [C/I]: `native.retained_groups` default 0x2ff = bits 0-7 + 9 (static, mesh,
   decals, sky, billboards, sky cube, actors, trees; **9 = shadow views included**, 8 vegetation excluded). The shadow views' z ops
   (`materialDraws` / `specialPassDraw`) are therefore replayed from a memo keyed by arguments, geometry, view kind / cascade, sampler
   hash and validated by a read log of engine memory (V3: stale 0 over 75,142 checked replays at the savegame, `retained_check` only
   there; shadow-view replays were part of those checks but no run combined retained with `shadows.enabled`). A bad replay yields
   wrong or missing *caster* draws in a cascade, never "the mask draws nothing": it cannot explain the report, only the original
   "NPC / object shadows missing" framing. `lean_restore=false` leaves the device in the last lean draw's state; the effect units
   (mask chain) `applyDevice` all 210 render states themselves and the replacer reads the CPU register file, so no effect on the
   mask; the engine's HAL caches lie for any engine code that still runs (rare in Full).
4. **`debug.rt_shadow_sync=true`**: only acts with `shadows.rt != off` (`RtShadows::apply`: `setEveryFrame(settings_.sync)`; with RT
   shadows off `setEveryFrame(false)`, rt_shadows.cpp:86). `shadow_stride=1` = cascades every frame (the verified compare setting).
   Neither is a candidate.

## 3. Full-only candidates (if section 1's log line is absent, or Wrapper turns out fine), ranked

Mechanics [C]: the engine's mask draws are dropped at `prepareDraw` start (`drawTap_->onDraw`, draw.cpp:790); the composed ones
(`EffectOps::applyDevice` + `dev_.drawIndexedPrimitive`, effect_ops.cpp:294/871, lean: `issueLean` effect_ops_lean.cpp:125) pass
the same `prepareDraw` ("own draw" route -> `Route::Ours`), so the replacer is asked with `phase = Main/ShadowMask` (the lean unit
runs inside `UpdateShadowMask`'s `PassScope`). `Shadows::replace` needs: `ps->semantics[ShadowMatrix(count>=4), ShadowSampler,
MaskDepth]` (CTAB of the effect's pixel shader), `q.textures[..]`, `device.psConstant(ShadowMatrix + r)`, `device.vsConstant(2)[0]`
(quad z), a valid `device.camera()`, and `device.shadowDepthMaps()` (counted in `detourExecuteShadowMap` from the bound DF24 depth
texture; credited by the stride throttle).

| # | candidate | where | would give | log / check |
|---|---|---|---|---|
| F1 | `shadowDepthMaps()` smaller than the cascade draws' count (e.g. the DF24 depth surface bound by group 2048's `setupTarget` is not recognised at `ExecuteShadowMap`'s entry) -> mask drawn at cascade draw 1 with `captured_ < 3` (or `expected` larger than draws -> never drawn) | shadows.cpp:141, scene_hooks.cpp:189-193, effect_ops_setup.cpp:445 | few / no cascades in the mask | `scene: shadow map = texture ... DF24 depth` lines (3 per real frame) + `enh: shadows: mask pass with N cascades` (N must be 3) |
| F2 | replacer sees no cascade draw: wrong `phase` / `semantics` of composed draws | draw.cpp:869, effect_ops.cpp | engine cascade draws not skipped -> engine shadows visible (contradicts the report) | absent `mask pass` line |
| F3 | `skipFrame_` (camera invalid / depth sampler null) -> engine draws stay | shadows.cpp:123-131 | engine shadows visible | same |
| F4 | retained shadow-view replays / `lean_restore=false` | static_ops_retained.cpp | wrong casters, not an empty mask | run with `native.retained=false` |
| F5 | native.opaque + native.shadows (if native.opaque is on) | native_opaque.cpp:417 | statics-only shadows, no NPC / trees (the first framing) | `enh: native opaque: on` in rvk.log |

None of F1-F3 matches "engine cascades gone too" as well as section 1; F1 would be visible as `N != 3` in the `mask pass` line.
Stride / `RenderShadowMaps` overrides (`shadows.map_size/range/split`), group 512 (cascade z ops) and group 2048 (setup, compare-clean
with 4096 / 100 m in README 13) were checked and are not candidates for an *empty* mask.

## 4. Secondary (original framing: NPC and object shadows missing) - for after section 1 is fixed

(a) 512/2048 take-over and (b) lean of shadow views (`leanActorPre` UpdateRenderMatrices + touches, SpeedTree sorts, instance sorts):
compare runs were clean on check frames; lean + enhanced mask never imaged. (c) `shadow_stride` 2 default skips cascades while the
camera is still (NPC motion lags one frame; with the user's `shadow_stride=1` irrelevant). (d) draw tags of composed shadow draws are
compared equal (`ownKind_`). (e)-(h) RT / coverage / batching: `shadows.rt` off = nothing exists; batching and record threads treat
composed draws like engine draws (`!replaced`, `opaquePass`). Left-to-the-engine ops in shadow views keep the engine's draws (not
dropped). Test: compare Wrapper vs Full images **after** the mask works (section 1 fix), with `native.lean=false` as the A/B.

## 5. Experiments and commands (do not run while the V4 agent has the test copy; each run needs the test copy `B:\DevDrive\risen-test-vk`)

Baselines: `shadows.enabled=true` with `shadows.map_size=2048`, `shadows.filter=low`. Spots (rvkcompare, noon, sunny): `shadow_inland_b`
(open clearing inland of the beach, trees, rocks, eye height), `jungle`, and `hero_idle` (the hero in his idle animation, clock running;
add `show_actors=1` so the hero / NPCs are rendered). Hold a spot long enough for the world to settle (defaults settle 600 frames).

E0 - the discriminator, Wrapper vs Full, mask view (answers "is Wrapper broken too"):

```powershell
.\scripts\run.ps1 -Mode wrapper -Mods rvkcompare -Scenario rvkcompare -WaitForResult -Seconds 600 -Param 'spots=shadow_inland_b','tag=w' -Setting 'shadows.enabled=true','shadows.debug=cascades'
.\scripts\run.ps1 -Mode full    -Mods rvkcompare -Scenario rvkcompare -WaitForResult -Seconds 600 -Param 'spots=shadow_inland_b','tag=f' -Setting 'shadows.enabled=true','shadows.debug=cascades'
```
then `Select-String 'enh: shadows: mask pass' B:\DevDrive\risen-test-vk\bin\rvk\rvk.log` after each (with `-Screenshots '450,500'`
if pictures are wanted; rvkcompare writes its own pictures). Expected under section 1: **both** modes show no cascade bands and
both logs have the `mask pass with 3 cascades` line. Engine reference: `-Setting 'shadows.enabled=false'` in either mode must show
the engine's shadows (Full = Wrapper there, README 13).

E1 - which part: `shadows.debug=terrain_term` vs `cascade_term` (cascade_term all lit = the cascade term is lost), repeat in Wrapper.

E2 - Full-only separation (only if E0 shows Wrapper fine): same run with `-Setting 'native.lean_effects=false'`, then
`'native.lean=false'`, then `'native.scene_ops_groups=128511'` (default 131071 with bits 512 and 2048 cleared: 131071 - 512 - 2048),
then bit 1024 cleared (`130047`; the mask chain back to the engine), and `'native.retained=false','native.lean_restore=true'`, `'debug.shadow_stride=1'`. One switch per run,
look at the `mask pass` line and the picture.

E3 - NPC / tree casters (after the fix): `-Mode wrapper` vs `-Mode full` at `hero_idle` with `-Param 'spots=hero_idle,shadow_inland_b','show_actors=1'`
and `-Setting 'shadows.enabled=true'` via `.\scripts\water-compare.ps1 -Spots hero_idle,shadow_inland_b -Setting 'shadows.enabled=true' -EngineSetting 'shadows.enabled=true'`
(the engine side = Wrapper, `-Mode` as the script offers; confirm its parameters with `Get-Help`). Compare: the hero's / an NPC's shadow
on the ground, tree shadows; images side by side in `out\water\<stamp>\sbs_*.png` via `shot-reviewer`.

E4 - bisect the shader (cheap, no game): `git worktree add ../reg-bisect d120038~1` (the parent), build `rvk_core`+d3d9.dll there and run E0 in
Wrapper: shadows present before, absent from d120038 on = confirmed. Since the fix is two lines of GLSL / one field, applying (a) or (b)
above and re-running E0 is faster than the bisect.

Settings to read in the real file without touching it: `overlay.debug_mode`, `native.opaque`, `[ab.*]`, `shadows.rt`, `debug.shadow_stride`.
