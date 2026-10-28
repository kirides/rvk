# V0: the constant data of the Full mode's draws, classified (2026-10-28)

Research for the "vulkanize" project (material data in GPU tables indexed per draw, per-object data in instance buffers,
per-frame / per-view data once, minimal per-draw CPU -> GPU traffic, later merged indirect draws). Scope: every float / int /
bool register and DrawBlock slot the Full mode composes (`renderer.mode = full`, engine-frame/README.md 6-17), where its value
comes from, how often it changes, how big it is, and what the translator would need for a split. The draw path itself
(recording, batching, indirect) is V0-draw-path.md. No game run was made for this doc.

Marks: **[C]** read in our code / the decompilation docs, **[D]** derived (arithmetic on [C] / [M]), **[M]** measured (rvk.log
of earlier runs, the shader corpus scan of this doc), **[E]** estimate, **[I]** inferred, **[?]** open.
Scratch (git-ignored `out/vk0c/`): `ctab_scan.py` + `ctab_scan.txt` (CTAB / register scan of the 2,483 exported game shaders in
`game-shaders/`), `estimate.py` + `estimate.txt` (the byte estimate of section 5), `fx_*.txt` (`rvk_fxtool dump` of the 35 effects).

Class names used below:

| class | meaning | one per |
|---|---|---|
| FRAME | the same for every draw of the frame (time, wind, sun, the application camera's rotation) | frame |
| VIEW | per camera / render view: main view, sky-dome view (translation-free), sky cube face, cascade i, terrain variance map, point-light atlas cell | view |
| MATERIAL | per `eCShaderBase` instance (its VS pool `+0x98`, PS pool `+0xc8`, sampler pool `+0xf8`; shared by all its passes / permutations) x the entity's switch value | material (x switch) |
| OBJECT | per entity / instance: world 3x4, fade, morph weights, bones, instance data, lightmaps | object (instance) |
| LIGHT | the lighting block PS c14..c37: an object's illumination (light list x outdoor factor f + static SH / lights) x the pass's first light | illumination x pass |
| DRAW | only this draw (c5.w alpha reference x fade; CPU-generated geometry) | draw |

---

## 0. Summary

1. **The engine's register file is laid out by pools, and the pools fix the class of almost every register** [C, C-materials.md
   3, D-contexts.md 0.5]. For `eCShaderDefault` (every mesh-like family): VS c0..c11 VIEW, c12 / c14..c29 FRAME, c30 VIEW (fog),
   c31..c231 OBJECT (`g_vInstanceMisc` + `g_vInstanceData[200]`: world, fade, instances, bones, SpeedTree instance data, tree
   billboard tex coords), c232..c251 MATERIAL; PS c0..c4 VIEW / FRAME, c5.xy VIEW, c5.w DRAW, c6..c13 MATERIAL, c14..c37 LIGHT.
   Particles, light streaks and the sky dome are `eCShaderBase` classes with their own pools (section 2.8 / 2.9).
2. **CTAB name -> register is identical in every game shader** [M: 2,330 PS + 149 VS + 4 ps_2_0 / 3 vs_2_0, `ctab_scan.txt`]:
   each name has exactly one base register (only the count varies with the permutation); the one exception is `g_vShaderData`
   (c232 in material VS, c31 in the sky dome's VS). No register is read that is not in the CTAB; PS c2 is always a `def` literal.
   A per-register remap at translation time (option (a)) is therefore well defined per (shader class, stage).
3. **Int / bool constants are dead**: 0 int / bool CTAB entries, no `if b# / loop i# / rep i#` in any of the 2,483 listings, no
   bool / int parameter in the 35 effects [M]; the material path never writes HAL slots 104 / 105 / 114 / 115 [C,
   static_compose.h:335]. Yet every uploaded StageConsts block copies `i[16]` + `b[16]` (320 B) [C, draw.cpp:566-567 / 576-577].
4. **Today's traffic** [C + M]: one 4,416-B ring block per stage per dirty draw (`Device::uploadConstants`, draw.cpp:560-581;
   every composed draw is dirty: static_ops.cpp:1880-1881), floats copied up to the shader's highest read register
   (`floatConstCount`, glsl.cpp:111-117) = **256 for every relative-addressing VS** (skinned, instanced, all SpeedTree VS: 101 of
   149 VS) and ~233-243 for a lit static VS (its material registers sit at c232+). Measured in Wrapper: 908 draws -> 7.8 MiB ring
   per frame (8.8 KB / draw) [M, out/o3/W1-3]; Full's 35 MiB is mostly our own CPU-geometry buffers, not constants
   (opt-profile.md 4) [M]. Estimate at the savegame (1,169 draws): 10.5 MB allocated, **5.1 MB written per frame**, of which
   0.75 MB int / bool zeros; the live data of the same draws is ~0.3-0.4 MB, the truly per-object part ~0.1-0.2 MB [D / E].
5. **Bytes are not the bottleneck by themselves**: trimming the VS copy to the read ranges gave no fps (performance.md 14 /
   15.1) [M]. What costs is the CPU composition around them in Full: `constantsInto` 0.78 ms, `issue` 0.42, register setters
   0.13, the per-op save / restore of 480 float registers (~0.6 [I]) per frame (opt-profile.md 4-5) [M]. A split removes that
   work and is the precondition for indirect merging (a per-draw state of a few indices instead of 9 KB).
6. **Recommendation** (section 6): option (a), a per-(class, stage) register remap into VIEW / MATERIAL / OBJECT / LIGHT blocks
   addressed through **biased block pointers** (so `blk.c[n]` keeps the D3D9 register number and Wrapper can point every block at
   its one StageConsts: one SPIR-V for both modes), persistent material / object / lighting tables with dirty tracking, effects /
   post / GUI left on the legacy StageConsts. Option (b) (GPU gather into per-draw StageConsts) keeps the shaders unchanged but
   still moves ~9 KB per draw through the GPU and needs the per-draw addresses anyway; (c) (content dedup) hits almost nothing
   while the camera moves (every VS block holds a world matrix and the view).

---

## 1. Where the constants live today

### 1.1 Engine pools (the source of truth the Full mode mirrors) [C]

| pool | registers | owner / writer | static_ops mirror (static_ops.h:145-160) |
|---|---|---|---|
| admin VS pool `admin +0x228` | VS c0..c30 | `SetCamera` (c0..c11), `SetVegationWindData` (c12), `SetSpeedTreeData` (c14..c29), `SetFog` (c30) | `adminVs[31]` |
| global pool `eCShaderDefault::ms_pGlobalShaderConstants +0x20` | VS c31..c231 | the slots: `SetWorldMatrix` / `SetGlobalAlpha` / `SetBoneMatrix` / `SetMorphTargetWeights` / `SetSpeedTreeInstanceData` / `SetVegationAlpha` / `SetSpeedTreeBillboardTexCoords` | `global[201]` |
| shader VS pool `shader +0x98` (`g_vShaderData`) | VS c232.. (<= 20) | the graph's tex-coord nodes (SetTime / SetSwitch) | `ShaderPools::vs[32]` per shader |
| admin PS pool `admin +0x208` | PS c0..c5 | `SetCamera` (c0, c1), `SetFog` (c3, c4), `SetCamObstacleDistances` (c5.xy), `SetCurrentRenderStates` / `ExecuteSpecialPass` (c5.w) | `adminPs[6]` |
| shader PS pool `shader +0xc8` (`g_ShaderData`) | PS c6..c13 (<= 7 handed out) | the graph's constant nodes | `ShaderPools::ps[16]` |
| lighting pool `DAT_180c62968` | PS c14..c37 | `FUN_1806cd1d0` per pass | `lighting[24]` |
| particle global pool `eCShaderParticle::ms_pGlobalShaderConstants` | VS c31..c60 | `ApplyRenderStates` (world, alpha), the particle version's light constants 0x6d7020 | `fxPool_` (static_ops_fx.cpp:936-975) |
| light-streak global pool | VS c31..c41 | Render (world), the passes (sun / light) | `fxPool_` (static_ops_fx.cpp:286-363) |
| effect parameters (fx_2_0) | per effect CTAB | `eCImageFilterEffect::OnSetParams`, `eCSprite` | `fxc::EffectDraw` (effect_compose.h:45-72) |

The Full mode (`StaticOps::constantsInto`, static_ops.cpp:732-802) copies, per draw, the shader's live registers from these
mirrors as runs (`forEachRun`), after composing natively: the view (`initMirror`, static_ops.cpp:563-581), world / alpha / tree
instance data (`writeWorld` / `writeAlpha` / `writeTreeInstance`, :584-600), lighting (`writeLighting`, :603-623), switch
elements into the shader pools (`applySwitch`, :641-690), bones (`writeBone`, static_ops_actors.cpp:113-117), morph weights
(:259), wind (`treeWind` / `grassWind`, static_ops_trees.cpp:138-176), tree-billboard tex coords (:414-427), vegetation fade
(:560-565), particle / streak pools (static_ops_fx.cpp). The material pools are read from the engine (SetTime ran in the
admin's Process before the frame) [C, README.md 7 "constants"].

### 1.2 Our upload [C]

* Device register file `vsF_[256]`, `psF_[224]`, `vsI_ / psI_[16]`, `vsB_ / psB_[16]` (defs.h:47-48); every setter marks the
  stage dirty (device.cpp:1652-1705), as does a shader change.
* At the draw: `uploadConstants` (draw.cpp:560-581) allocates **4,416 B** per dirty stage from the frame ring, copies
  `min(floatConstCount, 256 / 224) x 16` B of floats and always the 256 + 64 B of int / bool; the shader reads it through
  `pc.vs` / `pc.ps` (buffer reference, `RvkStage`, rvk_abi.glsl:12-17). DrawBlock (192 B) per draw (draw.cpp:1061).
* `floatConstCount` = highest c# read + 1, **256 with any relative addressing** (glsl.cpp:111-117).
* Translator hooks that already exist: `GlslOptions::constOverrides` (glsl.h:70: a per-register GLSL expression replacing
  `pc.X.c[n]`, `$c` = the original read; glsl.cpp:277-280) and the generated `rvk_cRel(i)` switch for relative reads
  (glsl.cpp:884-892).

---

## 2. Register classes per family

Common register map of `eCShaderDefault` (all families of 2.1-2.7, 2.10) first; the family tables list only what differs or
what the family adds. "reads" = registers live in the family's corpus shaders (`ctab_scan.txt`) [M].

### 2.0 Common `eCShaderDefault` registers

| stage | reg | CTAB name | class | value / source |
|---|---|---|---|---|
| VS | c0..c3 | `g_vViewProjection` | VIEW | transposed camera VP (`+0xe0`); **depth-bias VP** (proj `+0xa0` x view `+0x20`) when the material's `UseDepthBias` `+0x78` (static_ops.cpp:741-742) -> VIEW x material flag (2 matrices per view) |
| VS | c4..c7 | `g_vInvProjection` | VIEW | transposed inverse projection (only the sky VS reads it) |
| VS | c8..c10 | `g_vInvView` | VIEW | 3x3 inverse view (`.w` carry-over); read by billboard VS (transformation 7) only |
| VS | c11 | `g_vCamPosAndFar` | VIEW | camera position, `.w` = the application camera's far |
| VS | c12 | `g_vVegetationData1` | FRAME | (game time s, wind admin `+0x44`, `+0x50`, `+0x54`) (static_ops_trees.cpp:161-176) |
| VS | c14..c22 | `g_vWindMatrix` | FRAME | 3 SpeedTree wind matrices (`eCSpeedTreeAdmin +0xac`), once per db execution (static_ops_trees.cpp:138-158) |
| VS | c23..c25 | `g_vLeafAngleMatrix` | FRAME | 3x3 of the **application** camera (also in shadow views) |
| VS | c26..c29 | `g_vLeafAngles` | FRAME | rock / rustle angles (`admin +0x6c`) |
| VS | c30 | `g_vFogSettings` | VIEW (fog) | = PS c4; read only by particle-class VS in the corpus |
| VS | c31 | `g_vInstanceMisc` | OBJECT | morph weights (skinned) / vegetation fade (per grass type) |
| VS | c32 + 4i | `g_vInstanceData` | OBJECT | `.w` fade / global alpha of instance i; `.xy` SpeedTree instance data; `.xyz` carry-over elsewhere (static VS read only `.w` [M]) |
| VS | c33..c35 + 4i | `g_vInstanceData` | OBJECT | world 3x4 of instance i (i < 50) |
| VS | c232..c251 | `g_vShaderData` | MATERIAL | uv scale / scroller / rotator / oscillator (C-materials.md 3.4); corpus: 1..11 registers, mean 4.4 [M] |
| PS | c0 | `g_CamPosAndFar` | VIEW | = VS c11 |
| PS | c1 | `g_ViewToScreenPos` | VIEW | (0.5, -0.5, 0.5 + 0.5 / vpW, 0.5 + 0.5 / vpH) of the bound viewport (static_ops.cpp:570-575) |
| PS | c2 | - | - | never in a CTAB; a `def` literal where used [M] |
| PS | c3 / c4 | `g_FogColor` / `g_FogSettings` | VIEW (fog of the main view's camera, weather-driven) | `SetFog` (db `+0x14c8` / camera `+0x15c0`); sky cube faces keep the pools' values |
| PS | c5.xy | `g_CamObstacleAndMaskReference` | VIEW | camera obstacle fade (application camera `+0xe4`) |
| PS | c5.w | same | DRAW | alpha reference / 256: MaskReference (material) or MaskReference x fade (masked at fade < 1), SpeedTree forced 80, particles the emitter's `+0x29a` (static_ops.cpp:865, :945) |
| PS | c6..c13 | `g_ShaderData` | MATERIAL | constant / animated / switch colours, Fresnel, CamDistance, mask threshold, bump offset; corpus 1..7, mean 1.6 [M] |
| PS | c14 | `g_IndoorAmbientColor` | LIGHT | light list `+0x128` |
| PS | c15..c21 | `g_AmbientLight` | LIGHT | list SH x f, `.w` += (1 - f) indoor, + static SH (`sc::ambientSh`) |
| PS | c22..c24 | `g_DirLight` | LIGHT (in practice FRAME: every list holds the one sun) | list `+4 / +0x14 / +0x24` |
| PS | c25 + 4j | `g_PointLights` | LIGHT x pass | light `firstLight + j`: static (illum `+0x10`) then dynamic (list `+0x38`) |

Samplers / DrawBlock slots (C-materials.md 2.3; all material shaders, [M] corpus counts):

| slot | content | class | changes |
|---|---|---|---|
| s0 `smpShadowMask` (2,168 PS) | sun mask `.a` + SSAO `.r` + point shadows `.g/.b` | VIEW (main view's mask) | the image per view; content per frame |
| s1 `smpRefract` (132) | refraction copy | VIEW | per main view (after the copy) |
| s2 `smpDepthmap` (156) | linear depth map R32F | VIEW | per main view |
| s3 | never bound by the material path | - | - |
| s4 / s5 `smpLightMap1/2` (576) | directional lightmaps (`BindLightmaps`) | OBJECT (mesh resource; decals: the receiver's) | on streaming (no lightmap permutation while not loaded) |
| s9..s15 `g_SamplerData` | material slots; cube slots (s9..s13, 350 dcl_cube) incl. the sky cube (`eCColorSrcSkydomeSampler`) | MATERIAL (x switch, x time for animated samplers) | placeholder -> real image on streaming; animated = per frame |
| 16..19 (vertex samplers) | unused: no game VS samples a texture [M] | - | - |
| sampler states | per slot (U / V / filters / sRGB), `0xABABABAB` -> the configured filter `gfx +0x640` | MATERIAL + config | on options change |
| posFix.xy / .zw | half-pixel fix of the target / reverse-z | VIEW / FRAME | on resize / upscale / setting |
| alphaFunc / alphaRef | fixed-function alpha test: **always off** for materials (C-materials.md 2.1) | constant | - |

### 2.1 Static meshes, with / without lightmaps (incl. terrain / level meshes) [C: C-materials.md 1-3, README.md 7]

Terrain and level meshes are static meshes (lightmapped, blended layers as solid ops with fade, request bit 0x200); same map.

| pass | VS reads (class) | PS reads (class) | per-object | notes |
|---|---|---|---|---|
| z pre-pass (`XFX_ZPASS`, special pass 0) | c0-3 V, c32.w O, c33-35 O (+ c232.. M with uv transforms) | c0 V, c5.w D, c6.. M (opacity graph), s9.. M | world + fade | whole-mesh merge draws with the **last** sub-mesh's shader (static_compose.h:112-114) |
| solid / alpha / refracted, kind 0 | c0-3 V (+ c8-10 V billboards), c11 V, c32.w O, c33-35 O, c232-242 M | c0, c1, c3, c4 V; c6-12 M; c14-21 L; c22-24 L (sun permutation); c25-36 L (lights 1-3); s0-s2 V, s4/s5 O (lightmap permutation), s9.. M | world, fade, illumination | lightmap only for non-instanced, transformation 0, 0/1 lights (C-materials.md 4.2) |
| kind 1 (additive 4th+ light) | as kind 0 | c14.. L with the pass's lights only | + pass light index | not met in game runs (README.md 6) |
| kind 2 (z-fill of `EnableAlphaZWrite`) | as kind 0 | colour writes off | | |
| overlay passes (burned / frozen / highlighted) | as kind 0 | the overlay material's M | | another material over the same object |

PS family sizes [M]: lit + sun + mat 25 regs live (343 shaders), + points 27-37 (753 + 753), lightmapped variants 138 each.

### 2.2 Instanced meshes [C: C-materials.md 1.4]

VS reads c0-3, c8-11 V, `c32..c35[a0]` (relative: TEXCOORD4 = instance index x 4), c232-242 M; floatConstCount 256 [M, 36 VS].
Per instance i < 50: c32+4i.w = 1 (instanced batches always fade 1), c33..c35+4i world. A batch shares light list, f, switch,
overlay flags (the **last** instance's illumination draws it) -> LIGHT per batch, MATERIAL x switch per batch, OBJECT array of n
records. No lightmaps (combi 0). Z pre-pass: per-entity non-instanced draws (C-materials.md 1.1).

### 2.3 Decals [C: D-contexts.md 9, static_ops_nonop.cpp]

World identity, global alpha 1 (OBJECT = a shared identity record), vertices CPU-built per frame (dynamic decals transformed by
their entity), lighting of the batch's illumination (LIGHT), lightmaps of the receiver mesh (s4/s5 OBJECT), material or the
default shader + texture slot (MATERIAL). Batches <= 6,000 vertices.

### 2.4 Sky dome, sky objects, sky cube faces [C: E-shadows-lights-sky.md 2.6-2.7, static_ops_sky.cpp]

| part | VS | PS | class notes |
|---|---|---|---|
| dome (`eCShaderSkyDome`, `ge_skydome_2_0.fx`) | c0-3 / c4-10 of the **sky view** (view translation zeroed, proj `_33 = 1, _43 = 0`; setCamera static_ops_sky.cpp:125-151), shader pool c31..c40 (`g_vShaderData`: sun normal, sun / sky / haze colour, world 3x4 at c35) | c6 cloud colour, c7 sun colour | sky VIEW; the "MATERIAL" pool here changes **every frame** (weather / time of day) |
| stars / sun / moon (`eCSkyObject` billboards, `eCShaderDefault` transformation 7) | sky VIEW, world per object (matrices `+0x2d0 / +0x310 / +0x350`), c8-10 | material colour via `eCColorSrcConstant` (static_ops_sky.cpp:363) | OBJECT changes per frame (sun / moon move); stars' vertices CPU-built |
| sky cube faces (6 frames every 10 s) | the cube camera (90 deg, near 1, far 1000) = a VIEW per face; admin stages s0..s5 unbound | | 1 face per frame |

### 2.5 Opaque billboards (`eCBillboardAdmin`) [C: D-contexts.md 8]

World identity, alpha 1, batches per (material / texture, light set, blend, lights): LIGHT from the light set, MATERIAL = the
material or the default shader + the batch's texture; vertices copied from the billboards' prebuilt arrays (CPU, per frame).

### 2.6 Skinned actors (`eCRenderContextAnimation2`) [C: D-contexts.md 1, static_ops_actors.cpp]

| reg | content | class | changes |
|---|---|---|---|
| c31.xyz | morph weights (3 largest of the actor's morph setup; zeros without morphs) | OBJECT | per frame (blink, lip / face) |
| c32.w | fade | OBJECT | fading actors |
| c33 + 3b .. c35 + 3b (b < 66) | bone b of the **primitive's** bone list: `palette[remap[start + b]]` (`HRA +0x28` u16 remap) | OBJECT x primitive | every frame (animation) |
| streams 1..3 offsets | morph targets | OBJECT | per frame |

VS floatConstCount 256 (relative) [M, 36 VS]; PS as 2.1 (no lightmaps). The palette (`wrapper +0x48`, 0x40-byte matrices) is
per actor per frame (`UpdateRenderMatrices`); each primitive uploads its subset again (z, solid, each cascade): the same bones
2-5 times per frame [D]. Body parts are separate actors.

### 2.7 SpeedTree (branches, fronds, leaf cards / meshes) and tree billboards [C: D-contexts.md 2-3, static_ops_trees.cpp]

| geometry | VS reads [M] | per instance slot k (OBJECT) | frame / view |
|---|---|---|---|
| branches / fronds (8 VS) | c0-3, c11, `c14-16[a0]` (wind matrix by BLENDINDICES), `c32-35[a0]`, c232 | world c33-35+4k, c32+4k.xy = (tree LOD level `tree +0x6c`, `rb +0xa0` / `+0xa4`) (static_ops_trees.cpp:300-307) | wind c14-22 FRAME |
| leaf cards (6) | + c23-25, `c26[a0]` | + c32+4k.w = 80 / leaf-LOD alpha (:366-371) | c23-25 app camera FRAME |
| leaf meshes (6) | + `c26[a0]` | as leaf cards | |
| tree billboards (6 VS, `eCMeshResource2` path) | c0-3, c11, `c32[a0]`..c36 | c32..c36 = 5 fixed vec4 (Engine data `0xbfcbd0`), c37 + j = the BB mesh's per-sub-mesh vec4 list (`PS +0x40`, range `PS +0x58`) (:414-427) | always 2 draws (VS variants 1 / 2) |

MaskReference forced to 80 (c5.w) [C, opt-side-effects-anim.md]. Batches of <= `rb +0x128 + t` instances by LOD / strip /
illumination; measured **1,517 instance slots in 616 batches per frame** (2.46 per batch) at the savegame [M, Fl1 `native: trees`
1,504,864 / 611,568 per 992 frames]; each batch uploads a 256-register VS block (4.4 KB) for ~2.5 x 64 B of instance data.

### 2.8 Vegetation / grass [C: D-contexts.md 4, static_ops_trees.cpp:540-565]

VS reads c0-3, c11, c12 (FRAME wind), c31.xy (fade `-1 / R, (R + start) / R` from grid `+0x50 / +0x54` and MaskReference:
per grass type, constant -> MATERIAL-like), c32.w = 1 [M, 5 VS]. **The per-object data is already an instance stream**: stream 1
= 0x2c-byte records (position + wind flag, rotation quaternion, scale) copied into a shared 1,000-instance ring every frame;
measured 8,722 instances per frame = 384 KB / frame at the savegame [M, Fl1: 8,652,224 instances / 992 frames]. Lighting per
visible grid node (LIGHT; a draw breaks on another illumination).

### 2.9 Particles, strips, light streaks, precipitation [C: D-contexts.md 5-7, 10, static_ops_fx.cpp]

| family | VS registers | PS | per object |
|---|---|---|---|
| particles (`eCShaderParticle`, own global pool c31..c60) | c0-3, c11 V, c30 V (fog, `EnableFog = !PS +0x298`), c31-33 world (identity or entity), c34.w fade; lit: c35-41 SH x intensity, c42-44 sun, c45 + 4i lights (i < 4); shader pool `g_vShaderData` c57..c76 (overlaps the light registers c57..c60: the material pool wins, constantsInto order) | c6.. M, c5.w the emitter's mask ref | quads CPU-built every frame (`UpdateVertices` re-done natively); blend mode from the emitter |
| strips (`eCStrip_PS`) | world identity, alpha = fade x age fade-in | material | vertices CPU-built per frame (2 per segment) |
| light streaks (`eCShaderLightStreaks`, pool c31..c41) | c31-33 world, c34-36 sun, c37-40 one light; `g_vShaderData` c38..c58, time c38.y | c6.. M | never drawn in vanilla (MaxShaderVersion 0, README.md 16) |
| precipitation | identity world, AlphaBlend, DisableLighting | one sampler | the drops' billboard records CPU-built per frame |

Particle VS floatConstCount 35 [M, the 2 corpus VS]: these draws are cheap in the current scheme; the per-draw data is the CPU
geometry (Full: our own ring copies, opt-profile.md 4).

### 2.10 Shadow views: cascades, terrain variance map, point-light atlas cells [C: E 2.2-2.4, README.md 12-13]

Same object data and material data as the main view's z draws (world, fade, bones, instance data, wind, uv transforms, opacity
graph for masked materials), only the VIEW differs: `ExecuteGfxCamera(shadow camera)` sets VS c0..c11 / PS c0, c1 of the
cascade / terrain / cell camera; special pass type 3 (cascades), 1 / 2 (cells); c23..c25 stay the **application** camera
(leaf cards face the main camera in the cascades) [C, D-contexts.md 0.5]. DF24 maps: colour writes off. So in a split scheme a
static object's record serves the z pre-pass, the solid pass and every cascade it is in (today 2-5 uploads of the same world
matrix per frame) [D].

### 2.11 Effect units: post_SSM mask chain, blur, post chain, overlay, GUI sprites, PSSM [C: F 2.5, effect_compose.h]

Per-effect CTAB layouts (fx_2_0 preshaders); values per job from semantics: `TEXTURESIZEn` (per job), `FRAMETIME` / `TIME` /
weather `SCENEEXPOSURE` / `BLOOM*` / `COLORMATRIX` (FRAME), camera kinds `VIEW` .. `FOCALRANGEFAR` (VIEW), stored filter values
(static), mask: `DepthToShadowMatrix` / `ZDistance` per cascade, `FadOut`, PCF constants (VIEW x cascade); GUI `g_mWorldViewProj`
(FRAME: GUI scale x ortho), texture per batch. ~30-40 draws per frame (Post 19, ShadowMask 9, GUI 1-7, PSSM cells) [M, draw
kinds]. Every unit is a one-off: nothing to share; keep them on the legacy StageConsts (~0.3 MB / frame) [D].

---

## 3. Rate of change

| data | class | changes | evidence |
|---|---|---|---|
| main camera c0..c11, PS c0 / c1 / c5.xy | VIEW | every frame the camera moves (also idle: the hero's breathing camera [?]); c1 on resize / upscale | [C] SetCamera per view |
| cascade / terrain / cell cameras | VIEW | per update (cascades every frame or every 2nd with `debug.shadow_stride`; terrain map when camera / sun moved) | [C] E 2.1-2.2, performance.md 15 |
| game time (c12.x, timed material elements, effect TIME / FRAMETIME) | FRAME | every frame | [C] C-materials.md 3.4 "Time" |
| SpeedTree wind c14..c22, c26..c29 | FRAME | every frame while an `eCSpeedTreeWind_PS` exists | [C] D 2.4 |
| c23..c25 application camera rotation | FRAME | every frame the camera turns | [C] |
| fog PS c3 / c4, VS c30 | VIEW (weather) | continuously with the weather / time of day (slow) [I]; thunder | [C] A-frame / E 4.2 |
| sun (c22..c24 in every light list), hemisphere SH (list `+0xb8`) | LIGHT, effectively FRAME | when the light sets update (`UpdateIllumination`; the sun moves in 0.5 deg steps in the cascade copy) [I: rate not measured] | [C] rtgi.md 17 |
| material graph constants c6..c13 / c232.. | MATERIAL | static after compile; **SetTime** elements every frame (scroller, rotator, oscillator, constant animation, animated sampler frame); **SetSwitch** per entity (constant / sampler switch); a recompile re-walks the graph (quality change, missing permutation) | [C] C 3.4, static_ops.cpp:641-690 |
| sky dome pool c31..c40, c6 / c7 | MATERIAL (sky) | every frame (weather, sun) | [C] E 2.7 |
| world matrix c33..c35 | OBJECT | static entities: never; NPCs / movers / physics: per frame | [C] entity `+0x60` |
| fade c32.w | OBJECT | while an object crosses its view-range fade band; actors fading out | [C] B / C 1.1 |
| bones, morph weights | OBJECT | every frame for animated actors (LOD clamps bone sets) | [C] D 1.2 |
| SpeedTree instance data c32.xy (LOD level), leaf alpha | OBJECT | per frame with the camera distance (continuous LOD), static at rest | [C] D 2.2, 2.5 |
| lighting c14..c37 | LIGHT | light list change (object or light moved, cache-in), f easing (portals, 1/0.3 per s), **point-light flicker every frame** (torches: effect 1-4), static SH / lights on illumination update | [C] E 3.2 |
| lightmaps s4 / s5, material images | OBJECT / MATERIAL | streaming events (placeholder -> image; no lightmap permutation until loaded) | [C] C 1.3, 6.2 |
| vegetation instances, particle / strip / rain / decal / billboard / star vertices | OBJECT / DRAW | rebuilt every frame on the CPU | [C] D 4-10 |

---

## 4. What the translator needs for a split

### 4.1 Register assignment consistency (decides option (a)) [M: `out/vk0c/ctab_scan.txt`]

| stage | name | base | counts seen (shaders) |
|---|---|---|---|
| VS | `g_vViewProjection` | c0 | 4 (148) |
| VS | `g_vInvProjection` / `g_vInvView` / `g_vCamPosAndFar` | c4 / c8 / c11 | 4 / 3 / 1 |
| VS | `g_vVegetationData1` / `g_vWindMatrix` / `g_vLeafAngleMatrix` / `g_vLeafAngles` | c12 / c14 / c23 / c26 | 1 / 9 / 3 / 4 |
| VS | `g_vFogSettings` / `g_vInstanceMisc` / `g_vWorldMatrix` / `g_vGlobalAlpha` | c30 / c31 / c31 / c34 | 1 / 1 / 3 / 1 |
| VS | `g_vInstanceData` | c32 | 200 (101) or 4 (40) |
| VS | `g_vShaderData` | **c232** (77 shaders, 1..11) / **c31** (1: the sky dome, 10) | |
| PS | `g_CamPosAndFar` / `g_ViewToScreenPos` / `g_FogColor` / `g_FogSettings` / `g_CamObstacleAndMaskReference` | c0 / c1 / c3 / c4 / c5 | 1 each |
| PS | `g_ShaderData` | c6 | 1..7 (2,180) |
| PS | `g_IndoorAmbientColor` / `g_AmbientLight` / `g_DirLight` / `g_PointLights` | c14 / c15 / c22 / c25 | 1 / 7 / 3 / 2..12 |
| PS samplers | `smpShadowMask` s0, `smpRefract` s1, `smpDepthmap` s2, `smpLightMap1/2` s4 / s5, `g_SamplerData` s9.. (1..7) | fixed | |

* Every name has one base in every shader; no register is read outside the CTAB entries; relative addressing occurs only inside
  `g_vInstanceData` (bases c32..c35), `g_vWindMatrix` (c14..c16) and `g_vLeafAngles` (c26): each array lies inside one class
  window, so a window remap keeps relative reads valid [M].
* The **register numbers inside MATERIAL are per material** (a graph node's index in its pool, allocated at compile, C 3.4) but
  the window is fixed: c6..c13 / c232..c251 = "this material's PS / VS pool". All permutations / special passes of one shader read
  the same pools (the N5.1 composition uses one `poolsOf(shader)` for every pass and compares at 0 mismatches over millions of
  draws) [M, README.md 7].
* Effect shaders (post, mask, GUI) have their own layouts per effect: classify them as LEGACY.

### 4.2 Options

| | (a) per-register remap at translation | (b) GPU gather into per-draw StageConsts | (c) content-addressed dedup |
|---|---|---|---|
| idea | `pc.vs.c[n]` -> `view.c[n]` / `mat.c[n]` / `obj.c[n]` / `light.c[n]` by the (shader class, stage, n) table of 2.0 / 4.1 | a compute pass writes each draw's 2 x 4,416 B from (view, material, object, light) indices + a per-shader gather table | hash each composed block, reuse an identical one (this frame or earlier) |
| translator | new `GlslOptions` mode (generalises `constOverrides`; `rvk_cRel` switches on the window); c5 needs a component split (`vec4(view.c[5].xyz, draw.alphaRef)`) | none (or only: fetch `vsConsts` from a per-draw table by draw index for MDI) | none |
| per-draw CPU -> GPU | a record of indices / addresses (~32 B) + whatever OBJECT / LIGHT data changed | the same record + inputs; GPU writes ~9 KB / draw (~10-15 MB / frame of GPU writes + reads [D]) | the full 2 x 4.4 KB for every miss + hashing 9 KB / draw on the CPU (~1-2 ms / frame at 1,200 draws [E]) |
| hit rate / sharing | VIEW once per view, MATERIAL persistent, OBJECT reused by z / solid / cascades | as (a) for the inputs | VS: ~0 while the camera moves (view + world in every block); PS: z-pass blocks (view + material) and same-material same-light-list blocks only [I] |
| ordering constraint | none | the gather must run before the render pass that reads it: per recorded scope (record threads collect a scope before recording it, record.cpp [I]) | none |
| Wrapper mode | the same SPIR-V if the block pointers are **biased** (`base - first x 16`) and all point at the one StageConsts in Wrapper [I: BDA only constrains accessed addresses] | unchanged | unchanged |
| risk | carry-over semantics: registers the engine never writes per draw (c32.xyz in non-SpeedTree VS, c8..c10.w, c14.w, lights beyond the pass's count) must get defined values; only .w of c32 is read by static VS [M], others are never live [I] | GPU-side format bugs invisible to the CPU compare | none functional |

### 4.3 Int / bool constants

Never used (section 0.3). For any split, drop them from the per-draw blocks; for the current scheme, `uploadConstants` can skip
the 320-B copy when the shader's reflection has no int / bool read (0.75 MB / frame of zeros at the savegame [D]); the ABI layout
can stay.

### 4.4 Consumers of the register file (must keep working)

Observers and replacers read the CPU register file at draw time (`Device::vsConstant / psConstant`): `enh/water.cpp` (19 reads:
VS c232..c241, PS c6..c14), `enh/rt_scene.cpp` (6: world / bones / instance data captures, the skin pass's c33 + 3b),
`enh/gi.cpp` (5: ambient c15..c21; `giEligible`, draw.cpp:593-605, reads `psF_` too), `enh/shadows.cpp` (5: mask pass),
`enh/rt_material.cpp` (the CPU shader evaluator gets `vsConstants()` / `psConstants()`), `enh/object_motion.cpp`, `native_opaque*`
[C, grep]. `PushConstants::prevVsConsts` (motion: last frame's StageConsts of the same geometry) is still 0 (draw.cpp:1081). A
split must either keep filling the CPU file for observed draws (cheap: no upload) or hand these consumers the composed sources
(world, bones, material pools, lighting) directly, which is the better API for RT / GI anyway [I].

---

## 5. Sizes and totals

### 5.1 Live vec4 per draw by class (typical permutation) [M corpus reads, D]

| family / pass | VIEW + FRAME | MATERIAL | OBJECT | LIGHT | uploaded today (floats) |
|---|---|---|---|---|---|
| static z | VS 4, PS 1 | 0-5 + c5.w | 4 | 0 | VS 36 (233+ with uv transforms), PS ~6 |
| static solid lit | VS 5 (+3), PS 4 | VS 4.4 + PS 1.6 (means) | 4 | 11 (SH + sun) .. 23 (+ 3 lights) | VS ~243, PS 25-37 |
| instanced batch of n | VS 8 | 4.4 + 1.6 | 4n | 11-23 | VS 256, PS 25-37 |
| skinned primitive | VS 5 | ~6 + 1.6 | 2 + 3 x bones (<= 198) | 11-23 | VS 256 |
| SpeedTree batch of n (2.46 avg) | VS 5 + 9 wind (+ 7 leaves) | 0-1 | 4n | 11 | VS 256 |
| tree billboard | VS 5 | 0 | 5 fixed + per-sub-mesh list | 11 | VS 256 |
| vegetation | VS 6 | 0 | 1 (c31 per type) + instance stream | 11 | VS 32 |
| particle | VS 6 | 1-2 | 4 (+ 26 lit) | 0 | VS 35 |

Per object, the truly per-object data is 4 vec4 (64 B) for every rigid draw; the shared VIEW / MATERIAL data of a draw is 10-20
vec4; the LIGHT block (11-23 vec4) is per illumination and shared by all draws of the object (and by instances / grass nodes of a
batch).

### 5.2 Per frame [D / E: `out/vk0c/estimate.py`]

| | savegame (frame 6518, 1,169 draws, with cascades) | jungle (frame 9672, 1,643 draws) |
|---|---|---|
| ring allocated for constants (2 x 4,416 + 192 per draw) | 10.5 MB | 14.8 MB |
| written by `uploadConstants` + DrawBlocks | 5.1 MB | 6.7 MB |
| of it int / bool zeros | 0.75 MB | 1.05 MB |
| check: Wrapper measured | 908 draws -> 7.8 MiB ring = 8.8 KB / draw [M] | 1,481 draws -> 11.5 MiB [M, out/o3/Wj1-3] |
| split, steady state (lighting rewritten for 20 % / 100 % of 300 illuminations) [E] | 104 / 192 KB | 119 / 207 KB |
| of it: draw records 32 B / draw, 6 views x 768 B, 40 timed materials, 8 actors x 60 bones, 5 % of 600 objects + 10 % of 1,500 tree instances | 37 + 5 + 5 + 23 + 12 KB | 53 + 5 + 5 + 23 + 12 KB |
| unchanged: vegetation instance stream (already per instance) | 384 KB [M] | |

Draw counts per kind: rvk.log `draw kinds` (out/o2b/Fl1_noprof.log, out/o3/Aj1_noprof.log) [M]; the estimate's assumptions
(objects, illuminations, timed materials, dirty shares) are [E] and want a counting run (section 7).

---

## 6. Recommendation

**Build (a) with persistent tables, in four steps; keep everything outside the material families on the legacy path.**

1. **Data model** (CPU, Full mode, no shader change; measurable alone): in the composition, emit per draw a record
   `{view, material (shader x switch), object (offset, count), light (illumination x pass), drawBlock}` instead of register runs;
   keep VIEW blocks per view (VS c0..c31 + PS c0..c5 + the depth-bias VP), a material table keyed by (`eCShaderBase*`, switch)
   with dirty bits from SetTime / SetSwitch / recompile, object records keyed by entity / instance with dirty-on-change (world,
   fade, bones, instance data), lighting records keyed by (illumination, first light) with a change check. Count hit rates
   (the [E] rows of 5.2 become [M]). Expected CPU: most of `constantsInto` (0.78 ms) + the per-register setters + the per-op
   register save / restore (~0.6 ms [I]) go away [M profile, E gain].
2. **Translator remap** (`GlslOptions` window mode): per (shader class, stage) the window table of 2.0 / 2.8 / 2.9 / 4.1; block
   pointers biased so that `blk.c[n]` keeps the register number; Wrapper sets all pointers to its StageConsts (one SPIR-V,
   `kTranslatorVersion` bump); push constants carry one address of the draw's record (or the 4-6 block addresses: 48 of 128 B
   used today, abi.h:76-88). LEGACY = effects, post, GUI, any shader whose CTAB has an unclassified name.
3. **Drop the dead parts**: int / bool not uploaded; DrawBlock split into MATERIAL texture / sampler indices (table) + VIEW slots
   (s0..s2) + OBJECT lightmap indices.
4. **Indirect** (V0-draw-path.md): the record array indexed by draw index; same-pipeline draws merge once their per-draw state is
   a record index.

Trade-offs:

| | for | against |
|---|---|---|
| (a) remap | shared data uploaded once; object records shared across z / solid / cascades; no extra pass; values bit-identical (the same floats, other addresses) | translator + ABI change; a class table to maintain per engine shader class; c5 component split; consumers of 4.4 need the sources; Wrapper / Full share the shaders only with the biased-pointer trick [I] |
| (b) gather | shaders and enhancement variants untouched; the CPU compare of the current Full mode stays as is | still ~9 KB per draw (GPU), a compute dispatch + barrier per recorded scope, per-draw addresses still needed for MDI; does not shrink the shader's working set |
| (c) dedup | trivial | ~no hits while the camera moves; costs CPU hashing [E] |
| do nothing but trim | int / bool skip and range trimming are cheap | measured: no fps from trimming (performance.md 14) [M]; no step towards indirect |

---

## 7. Open

* Counts the estimate needs (a counting run, no image check): distinct (material, switch) per frame and how many have SetTime
  elements; distinct illuminations x passes per frame and the share whose lighting block changes frame to frame (flicker, f
  easing); unique trees vs instance slots; actors and bones per primitive.
* Whether the hemisphere SH / sun registers of the light lists change every frame (time of day) or only on `UpdateIllumination`
  [?]; if every frame, LIGHT is better split into a shared per-list part (SH, indoor, sun) + a per-entity part (f, static SH,
  static lights) composed in the shader (a translator change beyond the window remap: the engine pre-multiplies SH x f).
* Skinned: upload the actor palette once + a per-primitive remap (shader indirection) vs per-primitive bone blocks (no shader
  change, the bones 2-5x per frame); measure bones per primitive.
* Carry-over registers (4.2 risk row): prove with the compare that defining them (zeros) changes no pixel / no compared draw.
* Particle pool overlap c57..c60 (material pool vs 4th light) and light streak overlap c38..c40: which wins in the engine when both
  are live (never met: lit particles 0 in the measured runs, streaks never drawn) [?].
* Vulkan rule for biased buffer-device addresses (pointer arithmetic below the allocation, only in-range elements accessed): check
  the spec wording and the validation layer's GPU-AV before relying on it [I].
