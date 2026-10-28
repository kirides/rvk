# V0: what stands between today's Full mode and a GPU-resident, indirect-drawn Full mode

Research for the "vulkanize" project (2026-10-28, branch feat/native-scene, HEAD cf5c636). No game runs: the numbers come from the
code, from existing logs (`out/o3/*_noprof.log`: Risen_Save_0009 + `rvk_perfsave hold=40`, test baseline = 1x detail distances,
uncapped `RisenTest.exe`, vsync off; `out/o2b/`; the test copy's current `rvk.log`) and from `vulkaninfoSDK` on this machine
(`out/vk0r/vulkaninfo.txt`). Sibling docs: `V0-draw-path.md` (the per-draw CPU path), `V0-constants.md` (constants / register file).

Marks: **[C]** read in the code (file:line), **[M]** measured (a log line or tool output), **[D]** derived / estimated / my judgement.

## 0. Summary

* **The ABI is already half GPU-driven** [C]. A draw's state is 48 bytes of push constants holding device addresses: the vertex /
  pixel constants (`vsConsts`, `psConsts`, 4,416 B each in the ring), the `DrawBlock` (192 B: 16 texture + 16 sampler bindless
  indices, `posFix`, alpha test, tint), the FrameBlock, `prevVsConsts` (capture buffer), `drawId`, `flags` (`src/shader/abi.h:66-97`).
  Textures and samplers are bindless with stable indices. With EDS3 + dynamic vertex input on this GPU the pipeline key is just
  (VS module, PS module, attachment formats, topology class) (`src/vk/pipeline.h:57-71`, `src/vk/pipeline.cpp:51-70`).
* **What blocks multi-draw indirect (MDI)** [C/D]:
  1. Translated shaders read per-draw data from push constants (`pc.vs`, `pc.ps`, `pc.draw`). Inside one MDI call push
     constants cannot change. They would need a per-draw record table indexed by `gl_DrawID`, and the FS would need it as a flat
     varying.
  2. Per-draw vertex bindings. Instance-rate streams (stream 3 instance index, the vegetation's stream 1) rule out the
     `firstInstance` trick, so MDI needs `shaderDrawParameters` or vertex pulling.
  3. Per-draw dynamic state (cull, depth bias, viewport) and index type splits merge groups.
  4. Per-draw CPU work in `prepareDraw` (~1.1 us / draw) is what costs CPU, not the `vkCmdDraw*` calls themselves (DGC gave no
     gain: performance.md 12).
* **A GPU-resident Full mode gains CPU only if the per-draw CPU work disappears** [D]. That means retained per-object records
  (geometry, material indices, per-object constants), a per-frame list of record indices, and per-pass constants. MDI alone
  saves little. At the savegame the frame is CPU-bound: engine + HAL 4.70 ms vs GPU 1.48 ms [M].
* **Geometry** [C]: static mesh / tree / skinned / sky dome data already sits in `vk::MeshArena` (1 block of 64 MiB + 1
  dedicated buffer, 43-48 MiB used at 1x [M]). Everything CPU-generated (decals, billboards, sky objects, rain, particles,
  strips, grass instances, GUI sprites, PSSM) goes through dynamic `d3d9::Buffer`s into the per-frame ring. A ring quirk
  reserves the **whole** buffer per DISCARD (4 MiB `ownVb_`), so Full mode uses 35 MiB of ring per frame vs 7.8 MiB in Wrapper
  (126 vs 11.5 MiB at the jungle spot) [M/D].
* **Device** [M]: the RX 7900 XTX / AMD 26.10.1 has everything this needs. Enabled today: `multiDrawIndirect`,
  `drawIndirectFirstInstance`, BDA, descriptor indexing, EDS3, VIDS, GPL. Available but **not enabled**: `drawIndirectCount`,
  `shaderDrawParameters`, mesh / task shaders, shader object, descriptor buffer, DGC. `VK_EXT_multi_draw` is **absent**.
* **Recommended order** (section 8):
  1. Ring fix.
  2. Enable `shaderDrawParameters` + `drawIndirectCount` (optional).
  3. Per-draw record table + translator path behind a define.
  4. MDI of the existing batches, grouped by (pipeline, dyn, vi, ib).
  5. Retained records for Full's composed ops, fed from the native composition and not from `prepareDraw`.
  6. Consumers fed from the op stream.
  7. Vertex pulling / GPU culling last, if at all.

## 1. Today's path in one paragraph [C]

The native frame composes each engine op (`src/native/static_ops*.cpp`, `effect_ops*.cpp`). It then **replays it through the D3D9
state model**: `setStreamSource` / `setIndices` / render and sampler states / constants, then `dev_.drawIndexedPrimitive`
(`static_ops.cpp:1822-1894`, `effect_ops.cpp:879`, `effect_ops_gui.cpp:347`, `effect_ops_pssm.cpp:278`).

So every composed draw runs the whole `Device::prepareDraw` (`src/d3d9/draw.cpp:637-1166`):

1. Tap, observers, replacers, variants.
2. Pipeline lookup.
3. `computeDynamicState`.
4. `uploadConstants`: 2 x 4,416 B ring copies when dirty (`draw.cpp:560-581`).
5. `DrawBlock` into the ring (`:1022-1061`).
6. Push constants (`:1085`).
7. Draw record (`:1109-1153`).

The resulting `DrawPacket` (`device.h:1087-1102`) is recorded by `applyPacket` (`draw.cpp:1365-1386`), either at once or batched.
Batchable packets are main-view z pre-pass / solid and shadow-depth draws of StaticMesh / InstancedMesh / SpeedTree / Skinned,
indexed, unblended, depth-tested, no stencil (`:1093-1101`). They wait in `batch_` and are grouped by pipeline in `flushBatch`
(`:1389-1424`). Inside record-thread scopes the packets go out in chunks of 160 to 2 workers (`device.h:1314-1350`,
`record.cpp`).

## 2. Geometry

### 2.1 Where each kind's vertex / index data lives (Full mode)

| Kind | VB / IB source | Bytes written natively | Index | Stride | Instancing | Backing |
|---|---|---|---|---|---|---|
| Static meshes (incl. terrain + layers) | engine mesh VB / IB (`geometryOf` static_ops.cpp:692-710) | no | 16 or 32 per buffer | 52 / 60 / 68 | hardware batches <= 50 (`planBatches` :1285): streams 0-2 freq `n\|0x40000000`, stream 3 = the visual's instance-index stream (TC4 F1); per-instance world / fade in **VS constants c32+4i** (:1288) | arena [C] |
| Decals | engine admin dynamic VB 360,000 / IB 20,000 | yes, vertices transformed + alpha (static_ops_nonop.cpp:237-320) -> `ownVb_` (4 MiB) / `ownIb_` (64 KiB), DISCARD per draw (static_ops.cpp:1850-1871) | 16 | 0x3c | no | ring |
| Billboards (opaque / blended) | engine dynamic 208,000 / 12,000 | yes (nonop:512-656) -> ownVb_ / ownIb_ | 16 | 0x34 | no | ring |
| Sky dome | engine static (static_ops_sky.cpp:245-260) | no | engine's | engine's | no | arena |
| Sky objects (stars, sun, moon), rain | engine dynamic / eCSkyObject | yes (sky:167-207, static_ops_fx.cpp:1115-1154) -> ownVb_ / ownIb_ | 16 | 0x20 | no | ring |
| Skinned actors | engine static VB / IB; morph targets on streams 1-3 into the morph VB at `target*lodVerts*0x10` (static_ops_actors.cpp:237-256) | no (bones in VS constants c33+, 3 per bone) | engine's | 72 (+0x10 morph streams) | no | arena |
| SpeedTree (branches, fronds, leaf cards / meshes) | engine static (static_ops_trees.cpp:246-272) | no (instance world + tree data in VS constants :300-372) | engine's | 60 / 88 / 96 | stream 0 `count\|0x40000000`, s1 TC4 instance index | arena |
| Tree billboards | static mesh path (trees:387-441) | no (uv rows in VS constants c32+) | engine's | 64 | no | arena |
| Vegetation (grass) | engine mesh IB + **instance data** | yes: `grassInstance` (trees:496-581) -> `ownInstVb_` (2 MiB), DISCARD at the frame's first write, NOOVERWRITE appends, 16-B aligned cursor (static_ops.cpp:1827-1849) | engine's | 52 + s1 instance 0x2c | stream 1 INSTANCEDATA | VB arena, instances ring |
| Particles | engine particle IB; VB native (`particleVertices`, fx:829-852) | yes -> ownVb_ | engine's | 0x30 | no | ring |
| Strips | engine dynamic | yes -> ownVb_, non-indexed (fx:164-225) | none | 0x20 | no | ring |
| Light streaks | engine static | no | engine's | engine's | no | arena |
| GUI sprites | `spriteVb_` 0x1000 x 0x70 B, DISCARD per draw; `spriteIb_` static, generated once (effect_ops_gui.cpp:259-347) | yes | 16 | 0x1c | no | ring / arena |
| Post / mask chain / terrain blur quads | filter visual's buffers (effect_ops.cpp:760-845) | no | engine's | 20 | no | arena |
| PSSM cells | `pssmOwnVb_` 48 x 5 x 0x50, one DISCARD (effect_ops_pssm.cpp:90-266) | yes | **32** | 0x50 | no | ring |

The table comes from a code survey of `src/native/*` [C]. The 18 distinct vertex declarations seen in a trace (strides 12 to 96,
types FLOAT1-4, D3DCOLOR, UBYTE4, all 4-byte aligned) are listed in `docs/research/rt-scene-facts.md` 3 [C, trace].

### 2.2 How d3d9 buffers reach the GPU [C]

| Buffer class | Rule | Where |
|---|---|---|
| All | full CPU shadow `cpu_`; `lock` returns a pointer into it and records the range (`BufferCopyState`); `unlock` is a no-op; nothing moves until a draw calls `Buffer::gpu()` | resources.cpp:400-410 |
| `USAGE_DYNAMIC` or `POOL_SYSTEMMEM` | first use per frame / after a DISCARD: **a new ring block of the whole buffer size** (`frames().allocate(size_, 16)`), only the dirty [lo, hi) copied; NOOVERWRITE in the same frame patches in place | resources.cpp:423-443, identity.cpp:15-80 |
| everything else | every non-read-only update: a **new arena block** of the whole buffer, staged through the ring, `vkCmdCopyBuffer` on the upload command buffer; the old block retired; `notifyBufferChanged(id)` (observers drop BLASes); `cleanStatic_` fast path when untouched | resources.cpp:446-478 |
| Ring | 32 MiB chunks, 2 frames in flight, VMA `AUTO_PREFER_DEVICE` + host sequential write + mapped (ReBAR); usage VB / IB / SSBO / indirect / BDA (+ AS input) | frames.h:32-33, frames.cpp:193-202 |
| Mesh arena | 64 MiB VMA virtual blocks, > 8 MiB dedicated; device-local, no host access; offset = multiple of lcm(first use's stride or index size, 4); one arena for vertex **and** index data | mesh_arena.h, resources.cpp:451 |

Draw binding [C]:
* A single per-vertex stream whose offset is a multiple of its stride is bound at offset 0. The offset goes into
  `vertexOffset` (`vertexShift`, draw.cpp:315-335).
* The index offset is folded into `firstIndex` (`foldIndexOffset` :1239). Bindings are recorded only when they change.

So consecutive arena meshes **already share one VB / IB binding** as long as they are in the same 64 MiB block and have the
same stride and index type. At 1x everything fits one block. At 2x it is 69.5 MiB = 1 block + 1 dedicated buffer
(performance.md 11) [M].

**Ring waste in Full** [M/D]:

| Spot | Ring per frame, Full | Ring per frame, Wrapper |
|---|---|---|
| Savegame (`A1` / `W1`) | 35.4 MiB | 7.8 MiB |
| Jungle (`Aj1` / `Wj1`) | 126 MiB | 11.5 MiB |

* The Wrapper figure is about what the constants explain: 909 draws x (2 x 4,416 + 192) B ≈ 8 MiB.
* The difference matches ~7 per frame (savegame) to ~29 (jungle) DISCARD locks of the 4 MiB `ownVb_`. Each reserves 4 MiB even
  though only the written range is copied.
* It is not a copy cost: only [lo, hi) is copied. It is address space and chunk churn. Fix: allocate `hi - lo` and bias
  `binding_.offset` (watch the offset staying >= 0). Alternatively give `ownVb_` NOOVERWRITE appends with a cursor, the way
  `ownInstVb_` works.

### 2.3 Vertex input: fixed function vs vertex pulling

* Translated VS read `layout(location = N) in vec4 vN` (`src/shader/glsl.cpp:817-819`). The vertex input stage converts D3DCOLOR
  (swizzled), UBYTE4 and FLOATn. A missing input reads (0,0,0,1) from a null binding (draw.cpp:300-312) [C].
* With `VK_EXT_vertex_input_dynamic_state` (on here) the layout is **not** in the pipeline key. It is recorded per draw as
  `vkCmdSetVertexInputEXT` when it changes (`VertexInputValues::dynamicLayout`, draw.cpp:336-347) [C]. Without VIDS (the floor)
  the layout is in the pipeline key: more pipelines, the same MDI rules.
* **MDI does not need vertex pulling** if the draws of a group share the layout, the bindings and the index type. Arena placement
  already gives one binding per (block, stride). Per-draw differences then live in `vertexOffset` / `firstIndex` of each indirect
  command [D].
* What does break MDI with fixed-function input [C/D]:
  * **Instance-rate streams.** Static instanced batches read stream 3 (TC4 instance index) and the vegetation reads stream 1
    (0x2c records at a 16-B aligned cursor in the ring).
    * Instance-rate attributes are fetched at `firstInstance + i`. `firstInstance` therefore cannot carry a record index for
      those draws (the old `enh/native_opaque` trick, native_opaque.cpp:286, only worked because its shaders had no
      instance-rate input).
    * A per-draw instance-buffer offset would have to become `firstInstance = offset / stride`. The 16-B cursor is not a
      multiple of 44 today: align the cursor to the stride and it works.
  * **Morph streams** of skinned actors: streams 1-3 point at per-target offsets in the morph VB, so each draw has different
    binding offsets. They are mergeable only if those offsets fold into a shader-side base (pulling) or the morph targets get a
    fixed layout.
  * **Dedicated arena buffers** (> 8 MiB) and the second 64 MiB block: a different binding, so a group split.
* **Vertex pulling** (BDA reads + decode in the VS) would remove the binding / layout from the merge key entirely. It is also the
  precondition for meshlets / GPU culling at sub-mesh level. Costs [D]:
  * A translator path that decodes per declaration (18 layouts; the VS module is already per (VS, decl) through `vertexPlan`,
    draw.cpp:192-240).
  * The capture variants (RT tree capture, object motion, skin verify) index by `gl_VertexIndex - pad1` (rt_scene.cpp:1293-1329)
    and would need the same base.
  * Risk to the bit-exact parity the native frame keeps today: the UBYTE4 / D3DCOLOR conversions must match the fixed-function
    ones.
  * Recommendation: **not needed for the first MDI step**.

### 2.4 Index formats [C]

16 or 32 bit per buffer (`Buffer::index32_`, from `D3DFMT_INDEX32`, hal/vulkan/device.cpp:779). They are bound as UINT16 /
UINT32 (draw.cpp:1250-1256). The PSSM uses 32-bit. Everything natively generated except the PSSM is 16-bit.

An MDI group needs a single index type. The options:
* Split groups by type (cheap).
* Widen to u32 once into an index arena. `enh/native_index_arena.*` did exactly that for N3.3: a 64 MiB device-local u32 buffer,
  widened from the CPU copies, cached by (buffer id, generation, first, count). It still exists and is compiled
  (CMakeLists.txt:283-291), but is only used by `native.opaque` (Debug, default off).

## 3. Pipelines and state

### 3.1 The pipeline key on this device [C]

`PipelineDesc` (`pipeline.h:57-71`) = VS / FS module, topology class, polygon mode, colour / depth / stencil formats, blend
(ignored with EDS3), vertex bindings / attributes (emptied with VIDS, draw.cpp:338-346).

Everything else is dynamic state, the same set for every pipeline (`pipeline.cpp:51-70`): viewport / scissor with count, cull,
front face, topology, depth test / write / compare, stencil test / op / masks / reference, depth bias + enable, blend constants,
primitive restart, + EDS3 blend enable / equation / write mask, + VIDS vertex input.

Alpha test is **not** state: it is `DrawBlock::alphaFunc / alphaRef`, evaluated in the shader (glsl.cpp:902). That is per-draw
data, which merges well.

What multiplies pipelines beyond the engine's (VS, PS) pairs [C]:
* The **variant modules** chosen per draw: tint (`tintModule`), billboard fade (`lodModule`), G-buffer / GI material variants
  (draw.cpp:865-939), the RT / object-motion capture VS variants (:862-864), the ubershader (`debug.ubershader`).
* The **G-buffer's extra attachments** (`scopeGbuf_`, formats in the key).

### 3.2 Counts at the savegame [M] (`out/o3/A1_noprof.log`, Full, 1x)

| Quantity | Value |
|---|---|
| Draws per frame | 909 (1,000 in a cascade frame, 817 in a frame without cascade updates; `debug.shadow_stride` 2) |
| Batched draws | 622 in 8.5 batches per frame |
| Pipeline binds | 220 per frame (Wrapper identical: 908 / 622 / 220) |
| Render passes (rendering scopes) | 34.5 per frame |
| CPU | engine + HAL 4.70 ms, of it draw calls 0.98 ms |
| GPU | 1.48 ms per frame |
| Uploads | buffer uploads 540 KiB, texture 0 KiB per frame (steady state) |
| Phase structure (cascade frame) | `Shadow{ShadowDepth(37)} Shadow{(40)} Shadow{(106)} Main{DepthPrepass(260) ShadowMask(9) Solid(506) Refracted(2) Alpha(20)} Post(19) Gui(1)` |
| Kinds (817-draw frame) | SpeedTree 325, StaticMesh 184, Vegetation 112, InstancedMesh 92, Skinned 38, SpeedTreeBillboard 28, Post 19, ShadowMask 9, Sky 4, Refracted 2, Particle 2, Decal 1, Gui 1 |
| Jungle spot (`Aj1`) | 1,481 draws, 1,022 batched in 13 batches, 349 binds |

Run lengths:
* At 2x detail: ~2.3 draws per pipeline in the engine's order. Grouping halved the binds (825 -> 410 for ~1,950 draws:
  performance.md 11, 12) [M, older build].
* Today 909 / 220 = 4.1 draws per bind over everything [M].
* Distinct pipelines per pass are **not logged**. The bind count of a grouped batch equals its number of distinct pipelines, so
  the opaque passes hold roughly 100-150 distinct pipelines per frame at 1x [D, from 220 binds minus the unbatched draws' binds].
  A `-LogFrame` / perf counter "distinct (pipeline, dyn, vi, ib) keys per batch" would make it [M]. That is the number that
  bounds the MDI count.

**Not batched today** [C, :1093-1101]:
* Vegetation (112 / frame), SpeedTreeBillboard (28): not in the batchable kind set.
* Everything blended (terrain layers / decals / alpha / particles), the shadow mask, sky, post, GUI.
* Replaced draws (water, shadow mask), tinted draws, user-pointer draws.

### 3.3 What varies per draw and splits a merge group

| Item | Where | Splits MDI? | Note |
|---|---|---|---|
| VS / PS module (incl. variants) | pipeline | yes | the irreducible key [C] |
| attachment formats (G-buffer on / off) | pipeline + scope | yes | per scope anyway |
| cull mode (two-sided foliage, mirrored instances) | dyn | yes | sort by dyn within a pipeline group [D] |
| depth bias (decals, z-fill pairs) | dyn | yes | those are blended / unbatched today |
| depth compare (pre-pass LESS vs solid EQUAL / LESS_EQUAL) | dyn | yes, but per pass | constant inside a pass [D] |
| stencil (shadow mask) | dyn | not batched | |
| viewport / scissor | dyn | per scope | GUI scissor only |
| blend enable / equation / write mask (EDS3) | dyn | yes | z pre-pass colour writes vary by target format |
| vertex layout + bindings + offsets | vi | yes (fixed-function input) | see 2.3 |
| index buffer + type | ib | yes | see 2.4 |
| push constants (vs / ps / draw / prevVs / drawId / flags) | per draw | **must move into a table** | section 4 |
| textures / samplers | DrawBlock (memory) | no | already per-draw data |
| alpha test, tint colour, `posFix` (half pixel + jitter + reverse-z remap) | DrawBlock | no | `posFix` is really per pass [C, draw.cpp:1038-1043] |

* `internDyn` / `internVi` only deduplicate against the **previous** packet (draw.cpp:1322-1345). A merge needs a content hash
  of (pipeline, dyn, vi, ib) per batch [C/D].
* Today's grouping sorts only by pipeline rank (:1404-1418).

### 3.4 Order constraints [C/D]

* **Solid pass**: terrain layers and decals blend over the base surface at equal depth. Those draws are not batchable and flush
  the batch first, so the order base -> layer is kept. N3.3 proved that a base drawn after its layers paints over them (black
  wedges, native-renderer.md 14) [C].
  * Any MDI must stay **inside the flush boundaries the current batcher already has**.
  * Re-ordering opaque draws changes winners only for exactly coplanar opaque surfaces (performance.md 12) [M].
* **Z pre-pass vs solid**: the solid pass tests EQUAL / LESS_EQUAL against the pre-pass depth. The two draws use the same VS
  program, so invariance holds. MDI keeps the shaders, so no new risk [D]. N3.3's EQUAL failure came from two different shader
  programs.
* **Alpha / refracted / particles**: the engine sorts back to front (frame tap, engine-frame/README.md 6). Only consecutive draws
  with equal state can merge, which is rare [D]. Leave them direct.
* **Rendering scopes**: 34.5 per frame. An MDI cannot cross a scope (render-target change, a clear outside the scope, an
  injected pass, a texture transition: `prepareTextures` ends rendering when a sampled image needs a layout change,
  draw.cpp:178-185) [C].

## 4. Textures and samplers [C]

* **One descriptor set** for every draw pipeline (`vk/bindless.h`): t2D[16384], tCube[1024], t3D[256], samplers[2048].
  Update-after-bind, partially bound. Slot 0 = the (0,0,0,1) default.
* `Texture` takes its slot **at creation** (`resources.cpp:57`) and releases it only in the destructor / orphan, deferred past
  the frames in flight (`:76`, `:107`). **Indices are stable for a texture's lifetime.** `Bindless::updateTexture` exists but has
  no users.
* **Per-draw resolution** (draw.cpp:1026-1037), for each sampler bit the shader uses:
  1. A kind check.
  2. `layout == SHADER_READ_ONLY` (else 0, the default texture).
  3. The upscale substitution (`substituteFrom_` -> `substituteSlot_`, the post chain's input).
  4. The GI lightmap override (`block.tex[...] = 0`, :1047-1055).
  5. The sampler: `samplerIndex(slot)`, cached per stage (`samplerSlotCache_`, a two-entry memo per stage since O2b), else a
     hashed lookup of the 13 sampler states (`lookupSamplerStates`, device.cpp:2372-2411; samplers deduplicated by state).
  6. A mip LOD-bias variant while jittered (`samplerIndexBiased`).
  The cost is small per draw but it runs every draw.
* **Uploads are lazy** (`prepareTextures`, draw.cpp:161-189):
  * Loader threads only fill the CPU copy (`setLevelData` -> `pending_`, resources.cpp:274).
  * The upload runs at the first draw that samples the texture: in the upload command buffer when outside a scope, otherwise it
    ends the scope.
  * A retained / MDI design needs a **per-frame pending list** (textures that got data) flushed before the frame's first scope,
    not a per-draw scan.
  * Render targets transition between attachment and sampled inside the frame. The per-draw scan handles that today; retained
    draws need the transitions per scope from the frame graph (scene image roles).
* **Engine streaming** [C]:
  * While an image is not loaded the engine binds its per-stage **placeholder** image (`eCImageResource2::GetPlaceHolderImageData`,
    static_ops.cpp:64, 839; counted as `placeholder binds` in lean mode, static_ops_lean.cpp:457-461). When the image loads, the
    bound texture (and so its index) changes.
  * Retained material records must be re-resolved when a slot's image state changes (`eCResource2 +0x38 == 3`; N3.3 used exactly
    that guard, native-renderer.md 14), or the indices must be resolved per frame from a small per-material table.
  * Every composed draw also calls `RequestData` on its images once per frame (opt-plan.md, O2a). That side effect stays CPU work
    per visible material.

## 5. Device capabilities (RX 7900 XTX, AMD 26.10.1, Vulkan 1.4.359) [M: vulkaninfo; enablement C: context.cpp]

| Capability | Available | Enabled today | Floor / optional | Use for vulkanize |
|---|---|---|---|---|
| `multiDrawIndirect` + `drawIndirectFirstInstance` | yes | yes, `features().indirectDraws` (context.cpp:316-318) | optional (1.0 feature) | MDI; fallback = a loop of direct draws |
| `drawIndirectCount` (1.2) | yes, `maxDrawIndirectCount` 2^32-1 | **no** (not set in `f12`, :326-340) | optional | GPU culling / compaction |
| `shaderDrawParameters` (1.1: `gl_DrawID`, `gl_BaseInstance`) | yes | **no** (no `VkPhysicalDeviceVulkan11Features` in the chain) | optional in 1.3 | per-draw record index inside MDI with instance-rate streams |
| `bufferDeviceAddress`, `scalarBlockLayout` | yes | yes (:335-336) | floor in practice (ABI) | record table, vertex pulling |
| descriptor indexing (runtime arrays, non-uniform sampled image, UAB, partially bound, variable count) | yes, UAB limits 2^32-1 | yes (:327-333) | floor in practice | bindless |
| `shaderStorageBufferArrayNonUniformIndexing` | yes | no | optional | not needed (BDA) |
| `vertexPipelineStoresAndAtomics` | yes | yes (`vertexStores`) | optional | captures |
| push constants | 256 B (`maxPushConstantsSize`) | 48 B used, budget 128 | floor 128 | |
| `VK_EXT_extended_dynamic_state3` (blend enable / eq / mask, polygon mode) | yes, most of EDS3 | yes, a subset (:352-369) | optional | fewer pipelines |
| `VK_EXT_vertex_input_dynamic_state` | yes | yes (:371-376) | optional | layout out of the key |
| `VK_EXT_graphics_pipeline_library` | yes | yes | optional | |
| `VK_EXT_multi_draw` | **no** (not in the device list) | no | | none (and MDI covers it) |
| `VK_EXT_shader_object` | yes (driver list, line 1203; also the SDK layer) | no | optional | not needed: the pipeline key is already small |
| `VK_EXT_mesh_shader` (task + mesh) | yes, 256 verts / prims, 16 KiB payload | no | optional | only with full vertex pulling + meshlets; the translated D3D9 VS would need a mesh-shader form (large) |
| `VK_EXT_descriptor_buffer` / `VK_EXT_descriptor_heap` | yes / yes | no | optional | not needed: one UAB set |
| `VK_EXT_device_generated_commands` | yes | **no**: tried and removed (no gain; enabling the feature hid the ImGui overlay on 26.10.1; no pipeline switching inside a sequence: performance.md 12) | | avoid |
| `indexTypeUint8`, `maintenance5-9`, `dynamicRenderingLocalRead`, unused attachments | yes | no | optional | maintenance5 `vkCmdBindIndexBuffer2` (sized bindings) optional |

PLAN.md 1: the floor is Vulkan 1.3 + `VK_KHR_push_descriptor`. Everything vulkanize needs beyond that (`multiDrawIndirect`,
`drawIndirectCount`, `shaderDrawParameters`) must be optional, with the per-draw loop as the fallback [C].

## 6. Record threads and command buffers [C]

* Scopes of the main z pre-pass, solid and shadow depth (`wantSecondaryScope`, record.cpp:41-48) begin with
  `VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT` (draw.cpp:150).
* Packets go into `RecordChunk`s of 160 (`kChunkPackets`), recorded by 2 workers + the engine thread into transient-pool
  secondary command buffers per frame slot, with `VkCommandBufferInheritanceRenderingInfo` (record.cpp:155-162).
* `finishSecondaryScope` waits and runs `vkCmdExecuteCommands` in order, labels between (record.cpp:256-300).
* A full `batch_` (>= 160) flushes into a chunk at once (draw.cpp:1350-1354). Grouping by pipeline is therefore **per chunk** in
  threaded scopes: a pipeline that spans chunks is bound once per chunk.

What a retained / indirect design changes [D]:
* With MDI per (pipeline, state) group a pass records tens of commands, not hundreds of draws. The **secondary command buffers
  lose their purpose** (they parallelise the driver's per-draw cost: pipeline bind, push, draw). Record the few MDI calls inline
  instead, which also removes the 160-packet chunk boundary that splits groups.
* The indirect commands + per-draw records are written to the ring (host-visible) by the engine thread or by workers. With
  retained records only the per-frame index list and the indirect commands are written: ~20 B (command) + 4-16 B (index) per draw
  instead of ~9 KiB of constants.
* GPU culling (later) adds a compute pass before each scope and a `drawIndirectCount` buffer. It needs memory barriers between
  the compute and the scope (dynamic rendering has no implicit dependencies: N3.3 used full barriers, native-renderer.md 14).

## 7. Consumers that rely on per-draw CPU knowledge

Hooks (device.h):
* `ReplaceQuery` :325-354, `DrawReplacement` :356-371, `DrawReplacer` :373-384, `DrawTap` :389-413, `DrawObserver` :419-431.
* Call order inside `prepareDraw` (draw.cpp): tap 640-667, observers 720-730, `observeOpaque` 731-739, replacers 740-821,
  tint 832-842, capture variant 862-864, G-buffer / GI variants 865-968, jitter + DrawBlock 1022-1061, push 1085, record
  1109-1153.

| Consumer | Needs per draw | Hook | Could it use GPU-resident data / the op stream? |
|---|---|---|---|
| RT scene, static (enh/rt_scene.cpp) | VB / IB id + generation, offset, stride, range, base vertex, index32 (BLAS key :239); decl; world rows VS c33-35 or per instance c32+4i+1.. (:563-611); the instanced TEXCOORD4 stream from the CPU copy (:586-594); blend / alpha test / bias; tag entity / material; the material resolve by the CPU shader evaluator (rt_material.cpp:197-260) | `observe` :343 | The rows are already in the ring `vsConsts`, so a compute gather could replace the copies. BLAS identity, decl and material resolve stay CPU, but **once per object**, not per draw. In Full they can come from the composed op (entity, mesh resource, world), as the mirror source did (`rt.source=mirror`). |
| RT scene, skinned | bone constants c32..c231 copied into the ring (:1600-1604); morph weights c31; an IB max-index scan once per key (:1553); capture request (:1612) | `observeSkinned` :1489 | Bones: read through `vsConsts + 32` instead of copying. The capture address must exist before the draw's record is written (per-draw field). |
| RT scene, trees (wind capture) | instance rows (distance cull), capture buffer header with the previous capture's address | `observeTree` :1409 | The capture address is a per-draw record field (the `prevVsConsts` slot is reused, draw.cpp:1086). Keep it in the record table. |
| Object motion (enh/object_motion.cpp) | key = entity + buffers + range; VS c31-37 hashed to detect rigid motion (:111-129); capture with header {slots, prev capture} (:150-160); `prevVsConsts` is never set (always 0, draw.cpp:1081) | `observe` :68 | The motion test could run on the GPU against retained previous per-object constants. Object identity stays CPU, and is free in the op stream. |
| Water replacer (enh/water.cpp) | tag water class, PS sampler layout (:1016); VB `readCpu` body box / plane (:223-264); copies ~40 PS / VS registers + textures into its own block (:491-606); applies jitter itself (:514) | `replace` :981, `observeOpaque` :832 | Its decisions (skip / grid / shader swap) are structural and low-frequency. The block could point at `psConsts`. Water draws are few (Refracted 2): keep them direct. |
| Shadows replacer (enh/shadows.cpp) | PS semantics + cascade rows from PS constants across the mask draws, then one procedural draw (:100-240) | `replace` :100 | ~9 draws per frame: keep direct. |
| G-buffer / material variants / GI | phase, kind, water tag, PS outputs, ZENABLE, additive blend, non-zero ambient PS constants (draw.cpp:593-634); extra attachments in the scope + pipeline | device side | Variant (= pipeline) selection stays CPU, **per object** in a retained record. GI's ambient tallies (gi.cpp:190) read only PS constants, which a GPU reduction could replace. |
| Debug tint | kind / pass / view (already in `flags`), the `tintModule` swap, colour in `DrawBlock.pad0` (draw.cpp:1046, glsl.cpp:976-984) | draw.cpp:832-842 | The colour could come from a FrameBlock palette by `flags`. The module swap is a pipeline (CPU). Debug only. |
| Upscale jitter | main view + passes DepthPrepass / Solid / Refracted / Alpha + the target equals SceneHdr; `posFix.xy` += jitter (draw.cpp:1041-1042, glsl.cpp:971); sampler LOD bias | device.cpp:171-207 | Per frame / pass. Move `posFix` into a per-pass block. The LOD bias stays a sampler-index choice per material record. |
| Frame tap / coverage / static-ops compare (native/frame_tap.cpp:688-732, coverage.h, static_ops.cpp:1604-1692) | the full device state per draw, the engine op pointer | `DrawTap::onDraw` | Debug and verification tools. They need an **equivalence mode**: a retained / MDI draw expands its record back to a `DrawTapInfo`-like view on compare / check frames (`native.lean_check` already runs the engine every N-th frame). |
| Camera cross-check | all VS constants at the first main solid draw (draw.cpp:1157-1163) | device | Once per frame: unaffected. |
| draws::Record (rt.debug probes, `-LogDraws`, histogram) | 65 ns per draw [M] | draw.cpp:1109 | Keep for direct draws; retained draws can expose the record table. |

The pattern [D]: every consumer needs **object** identity + per-object data, and a few need a per-draw capture address. In Full
mode the native composition already has the identity (entity, sub-mesh, resource, op) before the draw exists. Feeding the
consumers from the composition (an op-level observer interface) instead of from `prepareDraw`'s `ReplaceQuery` is the
precondition for skipping `prepareDraw`. Wrapper keeps the draw-level path.

## 8. Blockers, risks, order

### 8.1 Blockers (must be solved for MDI / retained draws)

| # | Blocker | Kind | Size |
|---|---|---|---|
| B1 | Translated shaders read `pc.*` (push constants) for vs / ps / draw / prevVs / flags; an MDI group shares one push block | translator + ABI (`abi.h`, `rvk_abi.glsl`, glsl.cpp, `kTranslatorVersion`), ubershader, `rvk_material_variant.glsl`, capture variants (rt_scene.cpp:1293-1329) | medium [D] |
| B2 | the record index inside MDI: `firstInstance` is taken by instance-rate streams (stream 3 instance index, vegetation stream 1) -> needs `gl_DrawID` (`shaderDrawParameters`, not enabled) or vertex pulling; the FS needs it as a flat varying | context + translator | small (enable) + part of B1 [D] |
| B3 | per-draw `prepareDraw` work (~1.1 us / draw [M: 0.98 ms / 909 draws]): state translation, constants copies (2 x 4,416 B per dirty draw), DrawBlock, observers, records | architecture: the native frame replays through the D3D9 model | large; this is where the CPU win is [D] |
| B4 | per-frame VS constants mix camera and object data (one 256-register file per draw) | V0-constants.md | large [D] |
| B5 | consumers fed per draw (section 7) | an op-level observer interface for Full | medium [D] |
| B6 | lazy texture uploads + layout transitions per draw (`prepareTextures`); placeholder -> loaded switches change texture indices | a per-frame pending list + per-scope transitions; record invalidation on image state | medium [D] |
| B7 | static buffer re-locks make a new arena copy (`notifyBufferChanged`); retained geometry refs must follow generations | identity.h generations already exist | small [D] |
| B8 | verification tooling (compare / coverage / lean_check) assumes device draws | an expansion of retained records on check frames | medium [D] |

### 8.2 Risks

* **Gain risk** [M/D]:
  * DGC (GPU-generated push + draw) measured **no gain** (performance.md 12). Recording a push + draw directly is already cheap
    on AMD; pipeline binds were the expensive part, and grouping already halved them.
  * MDI on top of today's `prepareDraw` would save ~0.2-0.4 ms (binds + calls of 622 draws) at best [D].
  * The real target is B3 + B4: Full - Wrapper = +1.4 ms of our own per-draw composition / recording (performance.md 16:
    Full 4.76 vs Wrapper 3.36 ms).
* **Parity risk** [C]: N3.3 failed on hand-written shaders, not on the indirect plumbing. Vulkanize must keep the engine's
  translated shaders and constants bit-exact (the native frame's compare tooling proves it today; B8).
* **Order risk** [C]: coplanar solid layers / decals (keep flush boundaries); alpha order (keep direct).
* **Driver risk** [M]: enabling the DGC feature broke the overlay on 26.10.1. Re-check the overlay after enabling any new feature
  (`scripts/capture-window.ps1`), e.g. `shaderDrawParameters`, `drawIndirectCount`.
* **Spec point** [D]: inside a multi-draw, each draw is its own invocation group (Vulkan glossary), so texture indices from a
  `gl_DrawID`-indexed record are dynamically uniform. No `nonuniformEXT` is needed. Verify with validation + `rvk_vktest
  --passes`-style GPU self-tests.
* **Variant explosion** [D]: tint / G-buffer / capture / lod-fade variants already fork pipelines per draw. With MDI each fork
  splits a group. Keep variant selection per retained object (stable), not per frame.
* **Index type / arena blocks** [C]: groups split by u16 / u32, by arena block and by dedicated buffers. At 2x the scene spans 2
  buffers. Option: larger arena blocks (e.g. 256 MiB) or a u32 index arena (`native_index_arena` exists).

### 8.3 Recommended order

| Step | What | Expected | Dependencies |
|---|---|---|---|
| 0 | Ring: DISCARD of a range reserves only `hi - lo` (or `ownVb_` as a NOOVERWRITE cursor buffer) | ring 35 -> ~8 MiB / frame at the savegame, 126 -> ~12 at the jungle [D]; no CPU change | none |
| 1 | Enable `shaderDrawParameters` + `drawIndirectCount` as optional features (+ caps report lines); re-check the overlay | no behaviour change | none |
| 2 | Per-draw **record table**: `struct DrawRec { vs, ps, draw, prevVs (capture), flags }` in the ring, the translator's `pc.X` -> `rec.X` behind a define, `drawId` as a flat varying to the FS; fallback / direct draws keep push constants (`drawId` + table base in the push block) | the shaders become MDI-ready; parity provable with the existing compare runs (`static_ops_compare`, refcompare) | B1, B2; translator version bump; `rvk_shadertool corpus` |
| 3 | **MDI of today's batches**: group by (pipeline, dyn content hash, vi content hash, ib buffer + type) inside `flushBatch`; one `vkCmdDrawIndexedIndirect` per group, commands in the ring; inline recording for those scopes (record threads optional) | pipeline binds unchanged, draw calls 622 -> ~100-200 [D]; small CPU gain (measure: `perf:` lines + `-CpuProfile`) | step 2 |
| 4 | Vegetation / instanced streams: align `ownInstVb_`'s cursor to the instance stride so `firstInstance = offset / stride` replaces the per-draw binding offset; add Vegetation / SpeedTreeBillboard to the batchable kinds | +140 draws mergeable [D] | step 3 |
| 5 | **Retained records for Full**: per composed op (entity, sub-mesh, resource generation, material, variant) a persistent record (geometry ref, material texture / sampler indices, per-object constants block, pipeline); per frame only the visible list + per-pass / per-frame constants; invalidation by buffer generation, texture release, placeholder -> loaded, material change. Composed ops bypass `prepareDraw` (their own packet builder) | the per-draw CPU path (B3) gone for the bulk of 909 draws; the target of the project [D] | V0-constants (B4), V0-draw-path, steps 2-3 |
| 6 | Consumers from the op stream (RT scene / object motion / GI / tint / jitter per pass), the compare / coverage expansion on check frames | the Full path without the draw-level hooks; Wrapper unchanged | step 5 |
| 7 | (optional) GPU culling with `drawIndirectCount`, vertex pulling, meshlets | GPU-side gains only; the GPU is at 1.5 ms vs CPU 4.7 ms at the savegame [M]: low priority | steps 5-6 |

Reusable from the existing code:
* `enh/native_opaque.*`: the indirect plumbing, i.e. ring commands, the `indirectDraws` fallback loop (native_opaque.cpp:196,
  286) and memory barriers around the scopes.
* `native_index_arena.*`: the u32 widening.
* `vk::MeshArena`, the `vertexShift` / `foldIndexOffset` binding folding, and the interned `VertexInputValues` /
  `DynamicStateValues`.
* All of it compiled, default off.

## Appendix: sources

* Logs: `out/o3/A1_noprof.log` (Full, savegame), `W1_noprof.log` (Wrapper), `Aj1` / `Wj1` (jungle). Test copy `rvk.log` 17:01
  (Wrapper, cascade-frame structure).
* `out/vk0r/vulkaninfo.txt`: `vulkaninfoSDK` on this machine (AMD Radeon RX 7900 XTX, driver 26.10.1 LLPC, API 1.4.359).
* Code: `src/d3d9/draw.cpp`, `device.h`, `resources.cpp`, `identity.cpp`, `record.cpp`; `src/vk/context.cpp`, `pipeline.*`,
  `bindless.h`, `frames.*`, `mesh_arena.h`; `src/shader/abi.h`, `glsl.cpp`, `shaders/include/rvk_abi.glsl`; `src/native/*`;
  `src/enh/rt_scene.cpp`, `object_motion.cpp`, `water.cpp`, `shadows.cpp`, `gi.cpp`, `native_opaque.cpp`.
* Docs: performance.md 11, 12, 16; native-renderer.md 14; rt-scene-facts.md 3; engine-gfx.md (vertex formats, locking);
  engine-frame/opt-plan.md.
