# Native renderer: our own scene, our own frame (the GD3D11 way)

Status: design, 2026-10-25 (user decision). [C] = confirmed in the engine / game, [I] = inferred, [?] = open.

## 1. Why

Everything the RT features show as ghosts, seams, lag and churn has one root: **the scene is reconstructed from the D3D9 draw
stream**. A draw call has no object identity (we guess it from BLAS + transform + entity pointer), no lifecycle (instances are
aged out after 600 frames), no previous frame (poses are captured from vertex shaders one frame late or skinned a second time),
and only what the engine decided to draw this frame (off-screen shadow casters, the cached-in world behind the camera are
missing). Each fix so far was a patch on that guess (rt-shadows.md 5 - 10: volatile ranges, owner keys, captures, cascade syncs).
The 2026-10-25 bisect found one more instance: an entity's several ranges shared one owner key (`opDesc` 255) and evicted each
other's poses.

GD3D11 does not have this problem because it never looks at draws: it **replaces the engine's scene traversal** (gd3d11.md 2.1),
keeps its own mirror of the world (vobs, meshes, materials, lights), extracts meshes once, skins on the GPU and draws every pass
itself. We do the same for Risen: the engine stays the game (entities, animation, physics, streaming, scripts, GUI), we own the
picture.

## 2. Principle

```
engine (Genome)                         our renderer
-----------------                       -----------------------------------------------
entities / cache-in  --events/reads-->  SceneMirror   (stable ids, lifecycle, transforms)
mesh / material res. --extract once-->  MeshArena, MaterialTable (own formats)
animation palettes   --read per frame-> SkinPass (compute), attachments
lights, sky, weather --read per frame-> LightTable, SkyState
                                        FrameGraph: shadow / depth / opaque / sky / water / alpha / post
HAL (eCAPI*, d3d9::Device)   <--------  2D / GUI / fonts / not-yet-native passes keep using it
```

* **Identity is the entity.** `entity pointer x part (sub-mesh index, hierarchy node)` is the key of an instance for as long as the
  entity is cached in. No quantised transforms as keys, no aging as the only way to remove something.
* **Lifecycle is explicit.** Cache-in adds, cache-out removes, a transform change updates the instance in place (no new key, no
  ghost). A level-of-detail switch swaps the part's mesh, it is never a second instance.
* **The mirror is the single source** for the RT scene (shadows, GI, reflections), the shadow cascades' caster lists, culling and,
  later, the draws themselves. The draw-stream observer (`enh/rt_scene` observe) is deleted once the mirror feeds the TLAS.
* **Incremental and A/B-able.** Every stage lands behind a setting with the engine path as the reference (`native.*`, default off
  until it matches), as the water / shadows replacers did. The HAL path keeps working for everything not yet native.

## 3. Known engine hooks (from earlier research)

| need | where | source |
|---|---|---|
| entity enters / leaves the cached-in set | `eCGeometryEntity::OnCacheIn` 0x6a9720 (sets flag 0x20), a cache-out counterpart [?], driven by `eCPrefetchAdmin::CacheEntities` 0x6969c0 / `OnTestCacheInPos` 0x696530 | lod.md 6 |
| renderables of an entity | `eCFrustumDatabase::ExtractFromEntity` 0x66d750 -> `RequestRenderOps` -> render contexts: `eCRenderContextMesh` (`+0x10` entity, `+0x18` mesh PS, `+0x28` `eCMeshResource2`), `eCRenderContextAnimation2` | engine-gfx.md 8 |
| world matrix | entity `+0x60`; `eCGeometryEntity::GetWorldMatrix` 0x6aaff0 | engine-gfx.md 8 |
| mesh data | `eCMeshResource2`: sub-meshes `+0x58` (stride 0x48), `+0x60` count, `eCGfxVisual +0x88`; sub-mesh: start index `+0x28`, count `+0x2c`, vertex range `+0x30/34`, material `+0x38` | engine-gfx.md 8 |
| material | `eCMaterialResource2 +0x70` shader, `+0x78` id; graph / slots in `engine/materials.*`, rt-albedo-facts.md | materials.md |
| skinned pose | `eCWrapper_emfx3ActorInstance::UpdateRenderMatrices` 0x4dfee0: palette of 0x40-byte matrices at `+0x48`, count `+0x50`, frame stamp `+0x58` (only filled for actors the engine renders) | engine-gfx.md 8, rt-shadows.md 10 |
| lights, hemisphere, sky | `engine/lights.*`, `engine/hemisphere.*` (read-only readers exist) | rtgi.md 17, 19 |
| the sun | `engine::shadowmaps::trueSunVector` | rt-shadows.md 5 |

Open and being looked up (2026-10-25): the cache-out counterpart and whether an entity can die without it, an enumeration of all
cached-in entities (for validating the mirror), whether mesh vertex / index data has CPU copies or only GPU-side buffers, the
palette of actors outside every view, how attachments (the hero's shield and weapon) get their world matrices, SpeedTree and
vegetation instance sources. Results go into section 8 below.

## 4. Stages

| stage | delivers | verification | replaces |
|---|---|---|---|
| **N0 mirror** | `scene/mirror.*`: entity registry from the hooks above (id, part list, world matrix, bounds, LOD, flags), updated per frame; read-only, no rendering change | every draw's tag (entity, `opDesc`) resolves in the mirror (rvk.log counters: unresolved = 0 or explained); the mirror also holds off-screen entities; pointer reuse after free is handled (generation counter) | nothing (observer) |
| **N1 extract** | `scene/mesh_arena.*` + `scene/material_table.*`: each mesh resource's vertex / index data copied once into our arena (own packed vertex format), sub-mesh ranges, materials resolved once per sub-mesh; async on workers | arena contents equal the engine's GPU buffers (hash compare on a sample); material table equals `engine/materials` results | per-draw BLAS / material resolution in `rt_scene` |
| **N2 RT scene from the mirror** | TLAS built from the mirror: one instance per (entity, part), updated in place; characters skinned from palettes of the mirror's actors; attachments through their own entities; trees / leaves from wind state | the 2026-10-25 failures gone by construction: no ghost poses, no volatile ranges, no aging; rt.debug shows no red (missing) for cached-in entities; shield / weapon / NPC held items single | `RtScene::observe`, registry owner keys, volatile hack, vertex captures |
| **N3 native opaque** | our depth pre-pass + opaque pass from the mirror (frustum + Hi-Z culling on the GPU, indirect draws from the arena, bindless materials), A/B against the engine's solid pass, sky / water / alpha / particles / GUI still HAL | refcompare vs the engine renderer: no difference beyond noise; draw-call count and CPU time well below the HAL path (the stated reason for the arena) | the engine's z-pass and solid-pass draws |
| **N4 native shadows and lights** | cascades culled and drawn from the mirror (no engine cascade redraw, no `shadow_stride`), point-light shadows, light culling | shadows.md comparisons | `engine/shadow_maps` overrides, cascade draws |
| **N5 native frame** | alpha / water / sky / particles / post from our graph; the engine only provides data | full parity then enhancements | the HAL draw path |

Order of value: N0 -> N2 first (it removes the whole class of RT scene defects), N3 for performance and the stable image, then
the rest. N1 is needed by N2 only for skinned and wind-animated geometry; N2 can start with the existing BLAS cache keyed by the
mirror's part instead of by the draw (the BLAS cache already stores vertex data from the draws' buffers).

## 5. Rules

* The engine keeps running its own render path until a stage is verified equal: the HAL has to stay correct (D3D9 parity is the
  A/B reference and the fallback).
* Nothing is guessed that the engine can be asked: each mirror field has a hook or a read with an SEH guard (`engine/memread`).
* Everything optional and player-configurable, as in PLAN.md; the mirror itself costs a few entity reads per frame and is always on
  once N0 is verified.
* Each stage ends verified in the game and committed (CLAUDE.md rule 3). GPU test runs: one run at a time, with validation, never
  in parallel with another GPU load (a watchdog TDR was seen on 2026-10-25 during an unattended survey run).

## 6. First steps (N0)

1. Hook / binding for cache-in and cache-out (section 8 results), `scene::Mirror` with generation-checked ids.
2. Per-frame refresh of the entities' world matrices and flags from the engine's own list (not from draws).
3. Draw-tag cross-check: counters of draws whose (entity, `opDesc`) is unknown to the mirror; log of mirror entities never drawn.
4. Unit tests for the mirror (add / remove / reuse of a freed pointer / LOD swap / transform update).

## 7. What this does not change

The shader translator, the GLSL ubershader, the D3D9 device model, the overlay, settings, upscaling and the other enhancements stay.
They become consumers of the mirror where that helps (RT scene, GI, shadows) and keep working on the HAL path elsewhere.

## 8. Findings of the N0 research (2026-10-25, headless Ghidra on Engine.dll, dumps in `B:\DevDrive\risen-ghidra\scenemirror_dump*.txt`, `nativemesh_dump*.txt`)

All RVAs Engine.dll. [C] read in decompilation, [I] inferred.

**Lifecycle** [C]
* `eCGeometryEntity::OnCacheIn` 0x6a9720 (vslot +0x1d0, shared with `eCDynamicEntity`): property sets' OnCacheIn, `eCFrustumElementItem::CacheIn`
  0x670e60 (builds the render contexts), `flags |= 0x20`, then `eCPrefetchAdmin::OnEntityCachedIn` 0x696120. `OnCacheOut` 0x6a97b0 (+0x1d8):
  `OnEntityCachedOut` 0x696040, `eCFrustumElementItem::CacheOut` 0x670de0 (frees the contexts), `flags &= ~0x20`. Only `eCPrefetchAdmin`'s static
  tree callbacks (0x696590 / 0x696550) call them, on a 0 -> 1 / 1 -> 0 transition of a per-node counter, from `CacheEntities` 0x6969c0.
* `eCEntity::Destroy` 0x6a4180 is fatal ("still cached in") when flag 0x20 is set: an engine destroy always follows a cache-out.
  `eCEntityAdmin::NotifyDeleteEntities` 0x695310 (start of Destroy) is our delete net. Hooks (all real functions): `OnEntityCachedIn` (post: the
  contexts exist), `OnEntityCachedOut` (pre), `NotifyDeleteEntities`. Do not detour the 1-3 instruction getters or the tiny static callbacks.
* Flags `+0x28`: 0x20 cached in, 0x10 in processing range, 0x08 killed, 0x04 enabled, 0x8000 / 0x10000 boundary valid. `ExtractFromEntity` needs
  `(flags & 0x8026) == 0x8026`, 0x08 clear, `+0xf0 != 0`.
* The authoritative set: the scene sphere tree `pa+0x2a8` (nodes `pa+0x2d8`, stride 0x58) holds exactly the cached-in entities; counters at
  `pa`: `+0x528` cached in, `+0x52c` of them with render contexts, `+0x524` in the prefetch tree, `+0x530` in processing range (the mirror's
  validators). `pa` = scene admin (`FUN_1800957e0`) `+0xe8`.

**Entity layout** [C]: `+0x10` parent, `+0x18` next sibling, `+0x20` first child, `+0x28` flags, `+0x40/+0x48` property sets, `+0x60` world matrix
(4x4 row-major, translation = elements 12..14, rotation x scale in the 3x3; for a plain geometry entity local = world; dynamic entities keep the local
matrix at `+0x140` and `+0x60` is the composed world [I]), `+0xa0` world tree box, `+0xb8` radius + `+0xbc` centre, `+0xcc` ViewRange, `+0xd0`
CacheInRange, `+0xd8` embedded `eCFrustumElementItem`, `+0xf0` head of the render context list (context: `+0` vtable = its kind, `+8` next, `+0x10`
entity, `+0x18` property set, `+0x20` caps), `+0xf8` illumination, `+0x118` prefetch node id, `+0x1e0` dirty flags (bit 2 = world matrix updated:
the cheap "moved" signal for later stages). Contexts: `eCMesh_PS::CreateRenderContext` 0x5c5740 (`eCRenderContextMesh`), `eCSpeedTree_PS` 0x6e8c60,
animation / vegetation analogous.

**Mesh data** [C]: no CPU copy of vertices / indices in `eCMeshResource2`; `LoadDataV2` 0x75e080 reads the file payload straight into locked HAL
buffers. The visual (`res +0x88`) holds vertex data streams 0..2 (`+0x30/38/40`), stream 3 instancing (`+0x48`), declaration (`+0x50`), index data
(`+0x58`); each `eCGfxVertexData` has the HAL object at `+0x20` (**ours in Vulkan mode: the native extractor reads its Vulkan buffer / the Lock
data**) and the desc at `+0x28`. Engine readback also exists (`GetVertexData` 0xdbb10, `GetPositionData` / `GetNormalData` / `GetUVData` /
`GetIndexData` 0xdbc60). File format `GR01MS02` (risen-spass `docs/RE.md` 53). Declarations: rt-scene-facts.md 3. Sub-meshes: `res +0x58` (stride
0x48): firstIndex `+0x28`, indexCount `+0x2c`, firstVertex `+0x30`, vertexCount `+0x34`, material `+0x38`, hierarchy `+0x40`; indices absolute.
LOD: `eCMesh_PS` has up to 4 resources at `+0x40 + 8 i`, ranges `+0x60 + 4 i`, count `+0x6c`; `UpdateLoD` 0x5c74b0 stores the choice in the
render context `+0x28`; `eCMesh_PS +0x20` is the material switch, not LOD. Instancing: only `res +0xa0 == 1` meshes, render-instance array
(0x58-byte entries) filled by `RequestRenderOps`.

**Skinned actors** [C unless marked]: `eCAnimation_PS +0x30` embeds `eCWrapper_emfx3ActorInstance` (`+8` EMotionFX ActorInstance); body parts
(armour, heads) are `eCBodyPart_PS` in an array at `+0x1b0`. Palette: `UpdateRenderMatrices` 0x4dfee0: `palette[i] = X[i] * W[i]` with X =
`ActorInstance +0x28` (inverse bind [I]) and W = `+0x18` world matrices, stamp-guarded, **only called from `eCAnimation_PS::Render`** (so
off-screen actors are not refreshed; whether W is current for them [?]: `UpdateTransformations` 0x4e0560 runs from `Process`). Vertex buffer
per actor resource in bind pose, LODs concatenated, stride 0x48, BLENDINDICES are slots of the sub-primitive's bone list (remap `u16` at `this+0x28`),
per-LOD table `this+0x38` (stride 0x14), `eSHardPrimitive` array `this+0x90` (stride 0x20). Morphs: 3 largest weights into c31.xyz, 0x10 bytes per
vertex per target at `this+0x10`.

**Attachments (the hero's shield / weapon)** [C]: `eCAnimation_PS::AddAttachment` 0x4d8530 creates an `eCActorAttachment` (PS `+0x1a0`);
`UpdateAttachments` 0x4d5070 -> `eCActorAttachment::Process` 0x4d0f90 sets the attached entity's **world matrix** from `W[bone] [* offset]`. The attached
entity is then an ordinary `eCMesh_PS` entity: with the mirror it is simply another entity with a moving world matrix (stable id, updated in place).
This is exactly what the draw-stream guess got wrong.

**SpeedTree / vegetation** [C]: `eCSpeedTree_PS +0x38` resource, `+0x40` per-instance LOD state; geometry built once per resource
(`CreateGeometry` 0x6f8090: branches stride 0x3c, fronds, leaf cards / meshes, up to 50 software-instance copies); **wind is global**, once per frame in
`eCSpeedTreeAdmin::UpdateWindMatrices` 0x6efa10 (3 matrices at admin `+0xac`, 4 angle pairs `+0x6c..`), fed to c14-c25: a native renderer reads
these fields (no per-tree vertex capture needed). Grass: ordinary meshes, instances in `eCVegetation_GridNode` entries (0x2c bytes: position,
4 + 2 floats) gathered into one dynamic vertex buffer per frame by `eCVegetation_PS::Render` 0x707800.

**N0 result** [C, 2026-10-25, user's savegame Risen_Save_0009, 60 s with hero walking, `-SyncValidation`, 17 reports]: the mirror equals the
engine's own cached-in set in every report (e.g. 1996 entities / 1280 with render contexts on both sides; peak 2091 / 1364), repeated cache-in 0,
**draws resolved 1851 / unresolved 0** (every drawn entity is in the mirror: the draw tag's entity pointer is the mirror's key), no validation
message. Refresh of ~2000 entities (world matrix, flags, sphere, context list; SEH-guarded reads): 0.08 - 0.55 ms every 8th frame. `deleted while
mirrored` always equals `cache-out of unknown` (54 = 54): a killed entity that is still cached in reaches `NotifyDeleteEntities` (the start of
`Destroy`) first and its cache-out comes from inside `Destroy`: the delete net removes it a moment early and the late cache-out is harmless (this
answers the lifecycle question below). `deleted / unknown` (6836) counts entities that were never cached in (templates, off-range).

**Open** (carry into the stages): the scene
tree's size field; dynamic entities' node box overrides; whether `+0x60` is final for bone-attached entities [I: yes, `Process` writes it];
`ActorInstance +0x28` = inverse bind (check at a rest pose); off-screen actors' W; vegetation entry fields `+0x10..0x27`; SpeedTree billboards;
property-set type ids (use the context vtable as the kind instead); the `bool` of `GetVertexData`.

## 9. N3 first light: the native opaque pass (2026-10-25, `native.opaque`, default off) [C]

`src/enh/native_opaque.*`, `shaders/native/static_mesh.{glsl,vert,frag}`. A draw replacer skips the engine's main-view solid draws of plain
indexed static meshes (kind StaticMesh, triangle lists from static buffers, not blended; alpha-tested ones included) and records them; a pass
at `AfterOpaque` draws them into the scene HDR target + the hardware depth: vertices pulled from the engine's buffers by device address (one
pipeline for every static layout, `vkCmdDraw(indexCount, first = firstIndex)`), the engine's clip space (same matrix, half-pixel fix, jitter,
flipped viewport), the engine's per-draw cull mode (`D3DRS_CULLMODE`, dynamic state). The z pre-pass, cascades and shadow mask stay the
engine's; the native shader samples the mask (s0: `.a` sun shadow, `.r` occlusion).

* **Shading = the engine's material lighting for static meshes** (shaders/material/ge_default.frag): SH ambient (3 or 7 registers c15..21,
  mixed with the indoor colour c14 by the vertex AO = COLOR1.a), the directional lightmaps (s4 / s5, TEXCOORD6, alpha = ambient light direction),
  the sun (c22..c24, Lambert x mask.a), up to 3 point lights (c25..), `pow(acc, 0.45)`, x mask.r, radial fog (c3 / c4), tangent frame
  (`B = cross(N, T)`), DXT5nm normal maps. Which terms a draw uses = what its pixel shader's constant table lists (CTAB semantics: sun,
  mask, lightmaps, ambient registers, point light count). First light takes the per-draw inputs (PS constants c0..c47, bound textures s0 / s4 / s5 /
  the material's diffuse + normal slots) from the skipped engine draw; N3.2 replaces them by native sources.
* **The mirror's matrix = the draw's world rows** in 0 of ~134,000 compared draws (row r of the 3x4 = column r of the entity's row-vector
  matrix): the mirror can supply every transform.
* **Result** (savegame, hero standing, sync validation clean): the native image equals the engine's in geometry, textures, the door facade /
  stairs / cart, and the sun-shadow pattern (through the engine's mask); the difference left is colour warmth of the lit walls (lightmap /
  sRGB handling not yet checked).
* **What it took to find** (the method is worth keeping): `native.debug` views (albedo, normal, mask channels, ambient / lightmap / sun alone,
  feature flags, and two raster-state views). The first lit versions looked "right" while showing none of our fragments: the picture was
  the engine's remaining draws, and the black facade was a hole where we drew nothing. The no-depth / no-cull view proved the transforms; the
  depth-test-only view proved the depth; the culling-only view exposed **culling**: Risen's materials set their own cull mode, a fixed
  `BACK` + `CLOCKWISE` removed the front faces of level meshes. Rule: per-draw state comes from the engine's state, never a default.
* **Left to the engine** (about 2/3 of the draws counted by "blended / state"): blended passes (terrain layers, decals, overlays), instanced
  meshes, skinned characters, SpeedTree, vegetation, particles, water, sky. The terrain's cobbles in the first pictures were such blended
  layers drawn over the engine's base: the native pass does not draw them yet.
* Next (N3.2 - N4): list the parts from the mirror (own frustum / distance cull, level of detail from the contexts, no engine draw needed),
  take the lighting inputs natively (hemisphere / lights readers, per-object nearest lights, lightmaps from the mesh resource), then our own
  cascades from the mirror for the native shadow term.

## 10. Where the engine's per-entity lighting comes from (RE, 2026-10-25; dumps `B:\DevDrive\risen-ghidra\native_light_dump*.txt`) [C unless marked]

So a native pass can build the pixel shader constants without the engine's draw:

* **Camera / fog (once per view, `eCFrustumDatabase::Execute`)**: c0 = camera position, `.w` = far (`eCShaderMaterialAdmin::SetCamera` 0x6ba1d0);
  c1 = (0.5, -0.5, 1/vpW + 0.5, 1/vpH + 0.5); c3 = fog colour / 255, c4 = (-start k, k, density) with k = density / (end - start)
  (`SetFog` 0x6b9fc0 from `eCGfxFog::Execute`, fog data `+0x18 enable, +0x1c colour bytes, +0x28 start, +0x2c end, +0x30 density`).
* **Illumination of an entity**: `eCIlluminated_PS` = entity `+0xf8`, its `eSShaderMaterialIllumination` at `PS + 0x78`:
  `+0x00` pointer to the shared light list, `+0x08` float f (outdoor factor, eased by +-dt / 0.3), `+0x0d` count of static per-pixel lights (0..2),
  `+0x10 + 0x40 i` static light i (4 vec4 = the point light registers c25 + 4 i), `+0x90` byte: static SH present, `+0x94 + 0x10 k` (k < 7) static SH.
  Static data (`StaticIlluminated` = Static, the default) is baked into the world file (`ComputeInstanceLighting`), valid right after load;
  Dynamic ones are recomputed only when requested for drawing (`eCFrustumElementItem::Request` with request bit 0x80, `UpdateIllumination`
  0x595300), so a mirrored Dynamic entity needs one request before its data is real.
* **Light list (per light set, shared; refreshed every frame by `eCIlluminationAdmin::Process` -> `UpdateLightSets`)**: `+0x00` byte sun present,
  `+0x04 / +0x14 / +0x24` = c22 diffuse x intensity, c23 specular, c24 towards-sun direction (verbatim), `+0x34` byte dynamic light count (0..2),
  `+0x38 + 0x40 i` dynamic light i (4 vec4 = c25 + 4 i ...), `+0xb8 + 0x10 k` (k < 7) hemisphere SH (c15..c21 layout), `+0x128` indoor ambient (c14.rgb).
* **Copy to registers** (`FUN_1806cd1d0` 0x6cd1d0 / `FUN_1806b5bb0` 0x6b5bb0): c14 = list `+0x128`; **c15..c21 = hemisphere SH x f, `.w` of c15..c17 += (1 - f) x
  indoor.rgb, + the static SH when `illum+0x90`** (whether the add covers all four components: [I]); c22..c24 verbatim from the list; c25 + 4 i: the
  static lights first (illum), then the dynamic ones (list), at most 3 per pass (up to 2 + 2 lights per entity = 2 passes). Nothing depends on the camera, no
  shadow flag multiplies the sun (the shadow mask is a texture). Validation plan: compose the block natively and compare it with the constants of the
  engine's own draws of the same entity (counter in rvk.log).
* **Lightmaps are per mesh resource**: `res +0x90` (s4, `_1.dds`) and `res +0x98` (s5, `_2.dds`) are `eCImageResource2*`; `mats::apiTextureOf(image)` (+0x70 image data,
  +0x60 API texture) -> `Device::engineTexture`; null until loaded. The uv is the mesh's TEXCOORD6.
* **Fade** (`CalculateFadeOut` 0x671220, per frame, not stored): base = entity `+0xc8`, R = `+0xcc` (ViewRange) x `db +0x15b4` (the view factor, `lod.view`); d = distance
  camera -> entity sphere centre; R <= 0 (or db `+0x15a9`): base; d > R: 0 (dropped); d > 0.9 R: base x (1 - (d - 0.9 R) / 0.1 R). **A fade < 1 moves the mesh out of
  the opaque list into the alpha-blended list** (global alpha = fade; not a dither): the native opaque list must leave such parts to the engine.
* Open: the weather path that sets the fog fields; `bCVector4::Translate`'s component count; `eCMesh_PS::Lightmaped`; a runtime comparison of `illum+0x0d / +0x90 / +0x08`
  of Static vs Dynamic entities with the draw constants.

## 11. N4: native sun shadow cascades (2026-10-25, `native.shadows`, default off) [C]

`src/enh/native_shadows.*`, `src/enh/native_parts.*`, `shaders/native/shadow_mesh.*`. Three sphere-fitted, texel-snapped cascades (2048², splits 1500 / 4000 /
10000 units, light = `engine::shadowmaps::trueSunVector`) rendered at `AfterDepthPrepass` into transient D32 images from **the scene mirror's static mesh parts**
(every cached-in entity within reach of the cascades, off-screen casters included, masked parts alpha-tested through the material graph's diffuse),
depth-only, vertices pulled by address like the opaque pass. The native opaque shader samples them (3x3 PCF, slope-scaled bias + normal offset, the last
cascade fading into the engine's mask) in place of the mask's `.a`.

* `NativeParts` turns a mirror part into draw data without a draw: buffers (HAL objects -> `d3d9::Buffer`, `buffer_lookup`), the vertex layout from the engine's declaration
  handle (visual `+0x50` = our `VertexDecl*`), the material graph (blend mode, mask reference, diffuse image -> `Device::engineTexture`). Geometry cached per sub-mesh
  (validated by the identity of the resource's buffers and the part's numbers), textures per material; it is a `DrawObserver` that observes no draws, only
  `bufferChanged` / `textureReleased` (no stale pointer survives a release).
* Verified (savegame, sync validation clean): the term view equals the engine's mask in the shadow shapes (cobbles, the right-hand wall's tree shadow), the lit
  image equals the engine's. First version: shadow acne on walls at grazing sun (a fine hatch) - fixed by slope-scaled depth bias and normal offset.
* Cost: ~1100 casters, ~2450 depth draws per frame over the 3 cascades; 7 ms CPU with synchronization validation on (draw recording dominates; 58 -> 40 fps
  in that run, 58 -> 36 before the part cache). Open: merge / cache the cascade draw lists while the sun and the camera barely move (the engine updates its own
  every other frame), instancing, a GPU culling pass.
* Not casters yet: characters, trees, grass, instanced meshes (not in the native parts): the engine's cascades keep drawing for them and the engine's mask still
  carries their shadows beyond / besides ours. Next: skinned characters from the compute skinning (rt-shadows.md 10), SpeedTree.

## 12. N3.2: the opaque pass driven by the scene mirror (2026-10-25, `native.list = mirror`, default `engine`) [C]

`src/enh/native_opaque_list.cpp`, `native_parts.*`. At `AfterDepthPrepass` the pass builds its list from the mirror: the engine's own entity rules (`flags & 0x8026 == 0x8026`,
not killed), a frustum test on the entity sphere, the fade rule (alpha < 1 or beyond 0.9 x ViewRange x `lod.view` = a blended op: left to the engine), the lighting composed
natively from the entity's illumination structs and the shared light list (section 10), parts / buffers / layout / material / lightmaps from `NativeParts`. The engine's own draws
of listed parts are skipped (and compared with what was composed: rvk.log `lighting vs the engine`).

* **Verified against the engine's constants** over ~135,000 replaced draws: camera c0 / c1 0 differences, indoor c14 0, SH ambient c15..c21 0, light registers 0, sun c22..c24 11 (0.01 %),
  fog c3 / c4 only before the first capture (taken from the engine's latest main-view draw: the weather path of `eCGfxFog` is not read natively yet). The native image
  equals the engine's: textures, lightmaps, normal maps, ambient, sun with the engine's mask or with the native cascades, the shadow shapes.
* **Bugs found on the way** (each worth remembering):
  1. **The engine's vertex declaration handle is wrapped**: the pointer at `visual +0x50` is an engine object that holds our HAL handle, not the handle; an unchecked cast read
     garbage layouts (no uv, no AO: flat tan facades, white ambient). Now `HalDevice::findDecl` validates against the live declarations and looks inside the wrapper.
  2. **No face flip**: the engine's viewport is flipped, so every triangle it keeps counts as back-facing; flipping normals by `gl_FrontFacing` negated every normal (sun term 0, ambient
     saturated). Its materials flip the tangent frame only when double sided; every static draw (2.3 M) uses cull mode FRONT, none is double sided.
  3. **Textures must be touched every frame**: the engine keeps a resource loaded by calling `RequestData` (queue, or refresh the cache's LRU stamp) in each draw that uses it (`BindLightmaps`,
     `ExecuteShaderConstants`, `eCSubMesh::Render`); with its draws skipped, its LRU unloads what only we use. `NativeParts::touchResource` = `eCResource2::RequestData(res, 1)` once per
     frame per material, diffuse, normal and lightmap image (`resourceRequestData` symbol, 0x684100).
  4. Water materials (constant diffuse, refracted) are never drawn natively.
* Left to the engine: fading parts (alpha / view range), blended layers (terrain, decals), instanced meshes, skinned characters, SpeedTree, vegetation, particles, water, sky.
* Cost with sync validation on: native lists + native cascades ~39 fps against ~40 for the engine's own frame; the cascade draw lists (~2450 draws / frame, 7 ms CPU) are the
  next thing to cache / instance.

## 13. Instanced static meshes and the RT scene from the mirror (2026-10-25) [C]

**Instancing** (`native_shadows.*`, `shaders/native/shadow_mesh.*`). The cascades' casters are grouped by identical geometry + masked-diffuse (vertex / index address,
first index, count): one draw per group per cascade, the per-instance transforms in a ring array read by `gl_InstanceIndex`, the instances culled per cascade (the
cylinder toward the sun) while they are copied. Measured (savegame, ~1100 casters): **2,442 draws per frame -> 931 draws (426 groups), CPU 7 ms -> 3 ms**, frame rate unchanged,
image unchanged. The groups are cached: rebuilt only when entities / meshes / resolved parts changed (debounced to 8 frames, 0.34 ms each; 121 rebuilds where the first version
rebuilt every frame: the device's `textureReleased` / `bufferChanged` fire for the engine's dynamic resources every frame and bumped the parts' generation: now only a release of
something a part uses counts), or the camera moved 15 m, or 90 frames passed; entities that only moved update their instances in place (`Mirror::transformVersion`, 0.06 ms).
The mirror's version is split in `structureVersion` (add / remove / level of detail) and `transformVersion` (matrices of entities with meshes; characters do not count).
The native opaque pass is **not** instanced: the mirror list holds ~160 frustum-culled parts per frame (one draw each), nothing to gain.

**RT scene from the mirror** (`rt.source = mirror`, default `draws`; `rt_scene_mirror.cpp`). Every cached-in entity whose mesh parts are all drawable opaquely
(`NativeParts`) feeds the RT scene directly: one BLAS per sub-mesh range (the draws' cache and keys, equal ranges merge), one **pinned** registry instance per (entity, part)
with the entity's own matrix (`Registry::Instance::pinned`: aging by time / distance / the cap never takes it; a rebuild keeps the instance of an unchanged part, removes the
ones the mirror no longer wants, so a moving rigid object has exactly one instance and an LOD switch leaves no ghost), materials from the engine's material graph
(`RtMaterials::fromGraph`: textures, an identity uv map on the diffuse's uv set, the opacity role: an approximation of the draws' CPU-evaluated uv maps). The draws of fed entities
are no longer observed. Entities with a part that is blended / water / without a buffer (terrain layers, water, ~165 of ~900), skinned characters, SpeedTrees and instanced batches
without an entity stay on the draw observation; rt.log `rt: scene: mirror source: ...`.
* Result (savegame): TLAS **438 -> 1,320 instances, 480k -> 908k triangles** (the mirror knows the off-screen and the far entities the draws never showed), BLAS 522 -> 540, pass CPU 0.84 -> 1.0 ms,
  frame rate unchanged. The RT shadow term (extended, high) with the mirror source equals the one fed by the draws (correlation 0.993 over the ground area, mean difference 2.6 / 255, the
  rest are walking NPCs), validation clean.
* Open: the terrain / level meshes (blended layers) are still draw-fed; uv maps of animated / scaled materials; skinned characters and trees from the mirror's own data (the
  palette of off-screen actors, the global wind state: native-renderer.md 8); the GI's characters.

### 13.1 Terrain and level meshes from the mirror (2026-10-25) [C]

`NativeParts` classifies a part's material: **opaque / masked** (blend mode 0 / 1), **terrain layer** (alpha blend, a diffuse sampler, the opacity in the vertex colour: the engine blends such sub-meshes over
each other; `NativePart::layer`, accepted with `allowLayers` as a base surface by the RT feed, never by the native passes), **never opaque** (water, overlays with an opacity texture of their own such as the
cobbled-path decal, additive / specular layers with a cube diffuse, other shader classes: the part is ignored by the RT feed, as the draw path leaves them out, and no longer disqualifies its entity).
Only a part without buffers / declaration keeps an entity on the draws. Measured (savegame): fully fed entities 737 -> **834 of 874** (1,222 part instances; 40 entities left to the draws, all without buffers),
59k terrain-layer parts counted, validation clean. The RT depth check (`rt.debug=depth`, 25 probes) is identical for both sources: 18 match, 7 'traced farther' (the hero in the grid, a tree, the cobbled-path
overlay), 0 closer, 0 missed. Per-material classification log: rvk.log `native parts: material '...' is a terrain layer / never opaque`.
