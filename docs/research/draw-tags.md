# Draw tags, draw records, resource identity, shader semantics (M6 do-now items 2, 3, 4)

Every draw of a frame now says what it is (water, palm, hero, rock, grass, particle, sky, post, GUI, ...), where in the frame it
happens (view / pass of docs/research/frame-phases.md), which engine objects issued it (render context, entity, sub-mesh,
material), which data it draws (buffer ids + generations, ranges), where its constants and draw block live on the GPU and which
textures it samples. Shaders carry the registers of the engine's named constants (camera, world / bones, lights, fog,
refraction / depth samplers) from their CTAB. A debug view tints draws by kind or pass. None of it changes the parity output
(section 7).

Legend: **[C]** confirmed (decompiled, or measured in the running game), **[I]** inferred. RVAs are Engine.dll.

## 1. What exists

| piece | files |
|---|---|
| sources, kinds, tag, classification, records, histogram, tint palette | `src/d3d9/draws.{h,cpp}` |
| resource ids + generations, the buffer copy rules | `src/d3d9/identity.{h,cpp}` (`Buffer::ref()`, `Texture::ref()`) |
| shader semantics from the CTAB + vertex inputs | `src/d3d9/semantics.{h,cpp}` (`Shader::semantics`) |
| the tag on the device, a record per draw, the tint variant of pixel shaders | `src/d3d9/device.{h,cpp}`, `src/d3d9/draw.cpp` (`prepareDraw`), `Device::tintModule` |
| GLSL tint epilogue (a separate variant) | `src/shader/glsl.{h,cpp}` (`GlslOptions::tintEpilogue`), `src/shader/abi.h` (DrawBlock `pad[0]`) |
| hooks: render-context vtable slots, render-function detours, CreateRenderContext detours | `src/engine/scene_hooks.cpp`, `src/engine/symbols.h` |
| settings `debug.tint_draws`, `debug.draw_records` | `src/core/settings.cpp`, applied in `src/hal/vulkan/device.cpp` |
| histogram in rvk.log (every 5 s) and in the overlay ("Frame structure") | `src/hal/vulkan/device.cpp` (`logFrameStructure`), `src/overlay/overlay.cpp` |
| unit tests | `tests/draws_tests.cpp` (identity rules, semantics, classification, histogram, tint, epilogue) |
| `rvk_shadertool corpus <dir> --tint` | the tint variants of the corpus translate and validate |
| `mods/rvkcompare` `-Param show_actors=1` | a spot with the player and NPCs visible (the tint check) |

## 2. Persistent resource identity (item 3) [C]

`ResourceRef = {id, generation}`. Ids come from one process-wide counter (never reused, 0 = none). The generation names a
version of the GPU data:

| resource | generation changes when | unchanged when |
|---|---|---|
| static buffer (managed / default pool) | a new `VkBuffer` is made: the first use, and the first use after any write lock | used again without a write; read-only locks |
| dynamic / system-memory buffer | a new ring copy is made: the first use in a frame, after a plain lock, after a DISCARD | NOOVERWRITE appends / writes patched into the frame's copy |
| texture | CPU data reached the image (`Texture::upload`: file data, locks) | render-target writes; the `VkImage` itself is never replaced (a resize makes a new `Texture` = a new id) |

The copy decisions of `Buffer` moved into `BufferCopyState` (pure logic; `Buffer::gpu()` executes its plan), unchanged in
behaviour (refcompare section 7) and unit-tested (`identity_*`). Measured: the precipitation mesh (a dynamic VB) gains one
generation per frame (`187#1498`, `#1898`, `#2298` 400 frames apart); static meshes keep theirs.

## 3. Shader semantics from the CTAB (item 4)

### 3.1 Corpus survey [C] (`corpus/`: 149 VS, 2334 PS; CTAB parsed with a script equivalent to `fx::parseConstantTable`)

VS constants: `g_vViewProjection` c0x4 (148), `g_vInstanceData` c32 (141: x200 in 101, x4 in 40), `g_vShaderData` c232 (79),
`g_vCamPosAndFar` c11 (35), `g_vInstanceMisc` c31 (23), `g_vWindMatrix` c14x9 (20), `g_vLeafAngles` c26x4 (12), `g_vInvView`
c8x3 (10), `g_vLeafAngleMatrix` c23x3 (6), `g_vVegetationData1` c12 (5), `g_vFogSettings` c30, `g_vGlobalAlpha` c34,
`g_vWorldMatrix` c31x3 (2 each), `g_vInvProjection` c4x4 (1).

PS: `g_CamPosAndFar` c0 (2297), `g_SamplerData` s9.. (2286, 1-7 samplers), `g_FogColor` c3, `g_FogSettings` c4,
`g_AmbientLight` c15x7, `g_IndoorAmbientColor` c14, `g_ViewToScreenPos` c1, `g_ShaderData` c6 (1-7 registers),
`smpShadowMask` s0 (2168), `g_PointLights` c25 (1599, 2-12 registers), `g_DirLight` c22x3 (1148), `smpLightMap1/2` s4/s5
(576), `smpDepthmap` s2 (156), `smpRefract` s1 (132), `g_CamObstacleAndMaskReference` c5 (40). The registers are the same in
every shader that has the name (the engine's fixed layout, engine-gfx.md 8).

VS inputs (`dcl_*`): **TEXCOORD4 = the instance index** (with `g_vInstanceData` x200); **BLENDINDICES = bones**, except in
SpeedTree shaders, which use BLENDINDICES / BLENDWEIGHT for wind and leaf data (they have `g_vWindMatrix`). Grass
(`g_vVegetationData1`) has BLENDWEIGHT0..2 and no `g_vInstanceData`.

### 3.2 What is cached [C]

`Shader::semantics` (`shaderSemantics(tokens, reflection)` at creation, any thread): per `Semantic` the first register and
count (names of both stages map to one semantic, e.g. `g_vCamPosAndFar` / `g_CamPosAndFar`), and flags: `Material` (VS
`g_vViewProjection`, PS `g_SamplerData`), `Skinned` (BLENDINDICES + `g_vInstanceData`, no wind), `Instanced` (TEXCOORD4 +
`g_vInstanceData`), `SpeedTree`, `Leaves`, `Vegetation`, `Refracts` (`smpRefract`), `ReadsDepth`, `Lightmapped`,
`DirLight`. Draw records carry both shaders' flags; `VertexDecl` knows whether it has BLENDINDICES / TEXCOORD4 (record
flags `DeclBlendIndices`, `DeclTexcoord4`; `Instanced` = stream 0 frequency INDEXEDDATA).

## 4. Draw tags (item 2)

### 4.1 Hooks [C]

The tag (`draws::Tag`: source, op desc, context, entity, sub-mesh, material, mesh resource) lives on `d3d9::Device`; each
hook sets it around the original call and restores the enclosing tag afterwards (nesting: a mesh context -> `eCSubMesh::Render`
adds sub-mesh and material).

**Render-context vtable slots** (eCRenderContext slot 0 `Render(op, db)`, 1 `RenderAlpha`, 2 `RenderRefracted`, 3
`RenderZPass`; the frustum database's loops call them indirectly, frame.txt). Patched at the first device attach when the
slot holds the class's exported function (14 of 14 found); many are 16-byte tail-jump thunks (`0x5c73e0`, `0x5c73f0`,
`0x6ee8d0`) or tiny forwarders (`0x70d4b0`), so they are not detoured:

| class (vtable) | slots | source |
|---|---|---|
| `eCRenderContextMesh` (`0x9ae070`) | 0, 1, 2 (also the Refracted pass marker), 3 | Mesh |
| `eCRenderContextAnimation2` (`0x97b1f8`) | 0, 1, 3 | Skinned |
| `eCRenderContextSpeedTree` (`0x9debe8`) | 0, 3 | SpeedTree (op desc 0 branches, 1 fronds, 2 / 3 leaves) |
| `eCRenderContextSpeedTreeBB` (`0x9df3f0`) | 0, 3 | SpeedTreeBillboard |
| `eCRenderContextVegetation` (`0x9e31f0`) | 0, 3 | Vegetation |
| `eCRenderContextDebug` (`0x9c9b00`) | 0 | Debug |

Contexts **without an exported vtable**: found by logging the late-alpha list's contexts (`db+0x1550`) at the beach: vtables
`0x9ddd90` (the sky dome's context: slot 0 `0x6e8a10` renders stars, sun, moon and calls `eCSkydome_PS::Render`) and
`0x9e7a18` = `eCPrecipitationRenderContext` (slot 1 `0x719b00` = `eCPrecipitation_PS::RenderAlpha` inlined: the rain / snow
mesh, one indexed draw of ~6000 triangles from a dynamic buffer, **drawn every frame even when it is dry**). Their exported
`eCPrecipitation_PS::CreateRenderContext` (`0x71b9a0`) / `eCSkydome_PS::CreateRenderContext` (`0x6e4d40`) are detoured; the
vtable of the first context they return gets its slots that are not the empty base `eCRenderContext::Render` (`0x7faa0`)
patched (precipitation: 1 slot, sky: 2).

**Detours** (normal prologues, unique RVAs, capstone-checked): `eCSubMesh::Render` (`0x5c7eb0`: sub-mesh, its material at
`+0x38`, the mesh resource), `eCSkydome_PS::Render` (`0x6e4730`, also inside the SkyCube view), `eCParticle_PS::Render`
(`0x5e0080`), `eCStrip_PS::Render` (`0x6feae0`), `eCDecalAdmin::RenderDecals` (`0x4fc220`), `eCBillboardAdmin::RenderBillboards`
(`0x4f0530`). The entity is read at `+0x10` of render contexts and property sets.

### 4.2 Kinds and their reliability [C] (`draws::classify`)

| kind | rule | seen (draws per frame) |
|---|---|---|
| StaticMesh | Mesh source, op node != 0xffffff | beach 363 (Main 279, Shadow 84); jungle 493 |
| InstancedMesh | Mesh source, op node 0xffffff (`eCMeshResource2::ExecuteRenderInstances`) | rocks, skeletons, shells: 88; jungle 177 |
| Refracted | Mesh source in the Refracted pass | the sea: 6 at the beach (1-2 at other poses), jungle stream 9 |
| Skinned | Animation2 context | hero, NPCs, drowned bodies: 97 (Main 62, Shadow 35) |
| SpeedTree / SpeedTreeBillboard | SpeedTree contexts | palms and jungle trees: 615 / 136 |
| Vegetation | Vegetation context | grass, seaweed: 113 |
| Sky | sky dome context / `eCSkydome_PS::Render` | 4 in Main, 4 per SkyCube update |
| Particle, Strip, Decal, Billboard | the detours | particles 6-15; decal 1 (jungle); strips / billboards not seen at these spots |
| Precipitation | precipitation context | 1 (late alpha) |
| Debug | debug context | not seen |
| ShadowMask / Post / Gui | untagged draws of the ShadowMask pass / Post view / Gui view | 9 / 19-21 / 1-9 |
| Unknown | anything else | **0** at beach_sun, beach_rain, jungle (before the precipitation hook: 1, the rain mesh) |

The kind is reliable by construction: it comes from the engine object that issued the draw, not from a heuristic.
"Refracted" means "a refracting mesh material in the refracted list": at the beach that is the sea; glass or ice would be
Refracted too (the roadmap's water list by material name decides later, the material pointer is in the record). Terrain is
an ordinary static mesh in Risen (no terrain class): StaticMesh. Shadow-map draws keep their kind (palms in the Shadow
view are SpeedTree).

### 4.3 Records

`draws::Record` per recorded draw (`FrameDraws`, cap 8192 per frame, dropped draws counted; `Device::drawRecords().last()` =
the last presented frame): index, view, pass, kind, source, flags (Indexed, UserPointer, Instanced, DeclBlendIndices,
DeclTexcoord4, Tinted), primitive type, op desc, context, entity, sub-mesh, material, mesh resource, stream-0 vertex buffer
`ResourceRef` + offset + stride, index buffer `ResourceRef`, first index / vertex, primitives, base vertex, instances, VS / PS
hash + semantics flags, the device addresses of the VS / PS `StageConsts` and the `DrawBlock` in the frame's ring (valid while
the frame's ring slot lives: the GPU-side constants of exactly this draw), and the ids of the PS samplers s0..s15. The
histogram (kind x view) is counted for every draw, also with recording off.

## 5. Debug view "tint draws"

Setting `debug.tint_draws` (overlay category Debug, `off` / `kind` / `pass`, NoAB; `run.ps1 -Setting 'debug.tint_draws=kind'`).
Implementation: **a second variant of each pixel shader**, translated with `GlslOptions::tintEpilogue` on its first tinted
draw (`Device::tintModule`, SPIR-V cached as `<hash>_tint2.spv`): after the alpha test it blends `oC0.rgb` towards the tint in
`DrawBlock.pad[0]` (RGBA8, alpha = strength), keeping the brightness (`tint * (0.25 * min(16 l, 1) + 0.75 * min(1.5 l, 1))`;
black stays black, so additive draws keep their shape). With the view off nothing changes: the normal modules, pipelines and
SPIR-V are untouched and `pad[0]` stays 0. Only colour draws of camera views (Main / Preview / Other, passes Setup, Solid,
Refracted, Alpha, AlphaLate) are tinted; depth / shadow maps, the shadow mask, the post chain and the GUI never are.

Colours (kind): StaticMesh light grey, InstancedMesh cyan, Refracted blue, Skinned red, SpeedTree green, SpeedTreeBillboard
dark green, Vegetation yellow-green, Sky violet, Particle orange, Strip pink, Decal brown, Billboard yellow, Precipitation light
blue, Debug white, Unknown magenta. Pass: Solid grey, Refracted blue, Alpha orange, AlphaLate red, Setup magenta.

## 6. Verification

* **M6 tint criterion** (`rvkcompare` `spots=beach_sun,show_actors=1,mode=calibrate`, `-Setting debug.tint_draws=kind`):
  the spot's view and the four yaws around it show the sea blue (Refracted), the palms green (SpeedTree, yaw 0), the hero and the
  drowned bodies red (Skinned), grass yellow-green, rocks / skeletons cyan (instanced), sky violet [C]. beach_rain (rain light
  blue) and jungle (trees green, stream blue) likewise.
* Unknown draws in camera views: 0 at the three spots (rvk.log "draw kinds" lines, "untagged draw" lines list any).
* `-Validation` runs with the tint view on: no validation messages.
* `rvk_shadertool corpus corpus` and `... --tint`: 2483 / 2483; `compare_disasm.py`: 2483 / 2483 match.
* **CPU cost** (`[debug] perf`, opening beach `rvk_shadows`, ~1430 draws per frame): histogram + record **0.083-0.091 ms per
  frame, 58-63 ns per draw including the timer itself** (two `steady_clock::now` per draw); switching records off at runtime
  (`[debug] commands 'set debug.draw_records false'`) lowered the draw-call time by ~0.04 ms per frame (2.21 -> 2.17 ms). Against
  the CPU frame (engine + HAL 5.2 ms) that is 0.8-1.6 %, against the frame interval (12.5 ms, the session is capped at 80 fps)
  0.3-0.7 %; the hooks add a tag copy (~60 bytes) per render op / sub-mesh. Within the M6 budget (< 2 % fps).
* Tint by pass (`debug.tint_draws=pass`, beach_sun): solid grey, the sea blue, alpha (spray) orange [C].
* refcompare with the view off: section 7.

## 7. Regression check

`refcompare.ps1 -ReferenceDir out\refcompare\20261021-190139` (tint view off, records on, all hooks active): **12 PASS + 1 WARN,
0 FAIL** (run `out\refcompare\20261021-212745`; SSIM 0.989-1.000, dluma -0.62..+0.62). The WARN is beach_sun (psnr_q 37.9 <
38, the pass threshold; the differences sit on the animated sea, spray and clouds); a rerun of the two beach spots
(`20261021-213137`) gave beach_sun **PASS** (psnr_q 40.2) and beach_rain WARN (bad32 0.28 % > 0.2 %, rain): the beach spots
alternate within their temporal noise, as before this change (run `20261021-190928`: beach_sun WARN, psnr_q 35.0). With the
view off the normal shader modules, pipelines and SPIR-V are unchanged and the draw block's `pad[0]` stays 0; the buffer copy
rules moved into `BufferCopyState` without changing a decision.

## 8. Open

* Render contexts of `eCLightStreaks_PS`, `eCDynamicLight_PS`, `eCPointLight_PS`, `eCBillboard_PS` (their own
  `CreateRenderContext` exports; same mechanism as precipitation) were not seen drawing at the tested spots; they would show
  as Unknown with an "untagged draw" line.
* ~~The material's name is not read yet~~ Done (docs/research/water.md 2): in the Refracted / Alpha passes the sub-mesh hook
  reads the material's name (`eCResource2 +0x40`) and sets `Tag::water` / `Tag::materialName`; records carry `water` (the
  body of water) and `replaced` (drawn by an enhancement's draw replacer, or skipped by it).
* Records hold engine pointers of the frame: valid during the frame only (the engine may free them later).
* No GPU copy of the records yet (the ABI extension slots, do-now item 5, add `drawId`).
