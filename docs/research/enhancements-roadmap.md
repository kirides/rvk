# Enhancements roadmap: water, ocean, RT reflections, RT GI, player settings

Architecture note for the north star in `docs/PLAN.md`: the Vulkan backend is the platform for custom water and ocean, ray-traced
reflections and ray-traced GI. **Every enhancement is optional and player-configurable; the plain D3D9-parity path stays.**
Legend: **[C]** confirmed, **[I]** inferred or proposed. Engine facts (RVAs, struct layouts, VS/PS constants) live in
`engine-gfx.md` 8. Feature details: water.md, ocean.md, shadows.md, rt-reflections.md, rt-scene-facts.md, draw-tags.md, injection.md.

## 0. Summary

* Forward renderer, linear-depth z pre-pass, FP16 HDR scene target (`A16B16G16R16F`), tonemapped in `post_hdr.fx` [C].
* Water is a normal mesh with a refraction material, drawn from the refracted list; reflections come from a 128² sky-only cube [C].
* World matrices, bones and instance data are plain VS constants with CTAB names in every material shader; skinning is in the VS [C].
* Every engine draw goes through a persistent `eCAPIPrimitive` (= our `HalPrimitive`) owned by an `eCGfxVisual` per mesh
  resource: geometry identity is stable, VB / IB are reachable from any render context without drawing [C].
* Semantics: (1) phase markers, (2) draw tags, (3) constants by CTAB name, (4) a persistent RT instance registry from render
  contexts. Reading world data directly is only a supplement (water zones).
* RT = optional tier on the 1.3 floor: `VK_KHR_acceleration_structure` + `ray_query` (+ `deferred_host_operations`), inline ray
  queries in compute, no RT pipelines.

## 1. Scene semantics

### 1.1 What the engine gives us

Moved to `engine-gfx.md` 8 (render-op lists, contexts, meshes, sub-meshes, camera, VS/PS constants, original water, gameplay water).

### 1.2 Options and recommendation

| option | gives | cost / risk |
|---|---|---|
| (a) classify by shader / CTAB / material | camera, world matrix, bones, lights, fog per draw at the D3D9 level | cheap, no engine hooks; cannot tell water from other refracted materials, nor an object's identity |
| (b) engine-level hooks | phase, context type, entity, material, sub-mesh, LOD; off-screen objects through render contexts | needs RVAs, ICF thunk care |
| (c) read world data | water zones (`Game.dll`), entities via spass Lua | late (Lua ~19 s), gameplay data only |

**Chosen: (b) + (a), (c) for water zones.**

1. **Phase markers**: detours on large functions only. Done (frame-phases.md).
2. **Draw tags**: `eCSubMesh::Render`, `eCRenderContextMesh::Render(op)`, `eCAnimation_PS::Render`, SpeedTree / vegetation / sky /
   particle / decal / billboard functions set a "current tag" that `HalDevice::Draw` copies into the draw record. Never detour the
   16-byte-apart thunks (engine-gfx.md 8). Done (draw-tags.md).
3. **Constant harvesting by CTAB name** into a `ShaderSemantics` record cached on `d3d9::Shader`. Done (draw-tags.md 3).
4. **Geometry identity**: `Buffer` id + generation, start index, primitive count, base vertex. Done (draw-tags.md 2).
5. **RT instance registry** for off-screen geometry (the draw stream holds only the culled frustum): hook `RequestRenderOps`
   (runs for every view incl. the shadow cascades) and context creation / destruction; from a context reach mesh resource ->
   visual -> `HalPrimitive`, sub-mesh ranges, materials, world matrix: no draw needed for a BLAS. Optional extra extraction with a
   synthetic wide camera (`eCRenderSystemAdmin::Extract`). Skinned, wind-animated and instanced geometry comes from the current
   frame's draws (3.3). Built for static / instanced meshes + SpeedTree (rt-reflections.md 3).
6. **Water identity**: refracted draws + signals (sea plane, fake-shore overlay) with a material-name list as override
   (GD3D11's `OceanIdentifiers`). Done (water.md 2-3).

## 2. Water and ocean

### 2.1 Water today

Moved to `engine-gfx.md` 8 ("Water in the original engine", "Gameplay water").

### 2.2 Custom water (M7): done, water.md

Draw replacer: water draws are drawn with our shaders at their own place after the refraction copy (not collected and replayed).
Requirements (water.md 0): waves and a real shore for the open sea only, the water fits its environment, the engine's signals
first. Underwater post (camera against the water zones; the engine's overlay effect stays) and area profiles are open.
**Licence:** our code is MIT. GD3D11's D3D11-era water shaders are GPL with mixed authors: re-implement. GD3D11's D3D12 / Vulkan
code (`D3D12Engine/*`, `VulkanEngine/*`, `Shaders/D3D12/*`) has a single author, the user (2026-10-21, gd3d11.md 1): port
HLSL/D3D12 to GLSL/Vulkan and credit the origin in a comment; check `git log` before reusing older files.

### 2.3 Ocean (M8): done, ocean.md

FFT cascades (JONSWAP + swell, weather-driven sea state) on a camera-centred clipmap, a sea floor field from the main view's
static meshes (shore distance, depth, travel time) for shoaling / breaking / surf. Differences from the first plan: the floor comes
from the main view's draws (not the RT registry); cascades 310 / 87 / 23 / 6 m; displacement cap tied to the camera height.
Visual only: the engine keeps its flat plane for swimming, wading, splashes.

## 3. RT reflections and RT GI

### 3.1 Feature tier (done, raytracing.md)

* Optional: AS + `deferred_host_operations` + `ray_query` (+ `bufferDeviceAddress`); position fetch, `VK_EXT_opacity_micromap`,
  maintenance1 where present. Enabled at device creation whenever the GPU has them (`rt.allow`, default on), so toggles work at runtime.
* Inline ray query in compute / fragment shaders, no SBT; `VK_KHR_ray_tracing_pipeline` only if GI hit shading needs it.
* With the extension on, static VB / IB always get `SHADER_DEVICE_ADDRESS | AS_BUILD_INPUT_READ_ONLY | STORAGE`, the upload ring
  the build-input bit (no measurable cost).

### 3.2 Static geometry (BLAS): done (rt-reflections.md 3, rt-scene-facts.md)

One BLAS per (VB gen, IB gen, sub-mesh range set), `PREFER_FAST_TRACE | ALLOW_COMPACTION`, per-frame budget, LRU, memory cap.
Skipped: water, refracted, blended, decals, particles. Hit data: per-geometry record (BDA of VB / IB, strides, bindless texture
indices, flags) via `instanceCustomIndex + geometryIndex`. Albedo slot found (rt-albedo-facts.md).

### 3.3 Dynamic geometry (open)

* **Skinned characters**: bones are VS constants. Preferred [I]: a **VS capture** variant of any VS (rasteriser discard,
  `gl_Position` to an SSBO by `gl_VertexIndex`, replay with c0..c3 = identity so `oPos` = world position). It also covers wind,
  morph targets, billboards, instancing as the engine animates them. Needs `vertexPipelineStoresAndAtomics`; then a
  `PREFER_FAST_BUILD | ALLOW_UPDATE` BLAS per frame. Alternative: compute skinning (4 bones, 3x4 from c33+), tied to the layout.
* **Alpha-tested foliage**: ray query candidate loop with UV fetch + alpha test (done for the scene); grass excluded at low / medium.
* **TLAS** per frame (`PREFER_FAST_TRACE`), instance masks (static, props, dynamic, foliage, water), radius limit as a setting.

### 3.4 Shading inputs, G-buffer, motion (open)

* No G-buffer. Available: FP16 HDR colour, hardware depth, the engine's linear R32F depth map (z pre-pass on).
* Normals / roughness: optional extra outputs of the translated material PS behind specialisation constants
  (`src/shader/glsl.cpp`). The per-pixel normal-mapped normal is available (83 % of material PS decode a DXT5nm normal into
  `m_vecWorldNormal`; materials.md 2, 3, 6); specular power is per material. First step: geometric + depth-reconstructed normals.
* Motion vectors: previous camera + per geometry identity the previous VS constant block address; a VS variant writes
  `prevClip - curClip` to RG16F; static geometry reprojects from depth; TAA jitter goes into `posFix` (`src/shader/abi.h`).

### 3.5 Reflections (M9)

Water first (done: rt-reflections.md 3.3), then glossy / wet surfaces (roughness gate). Hit shading: albedo x (sun with shadow
ray + ambient + lightmap) + fog; miss = env cube. Denoise [open]: temporal accumulation with motion vectors + spatial bilateral;
candidates FidelityFX Denoiser (MIT, HLSL: port to GLSL) or own SVGF-lite; check NRD's licence. Composite in linear HDR before
the post chain.

### 3.6 Global illumination (M10, open)

* Engine lighting [C]: sun (`g_DirLight` c22) + point lights (c25+), hemispherical ambient (`g_AmbientLight[7]` c15-21,
  `g_IndoorAmbientColor` c14), baked lightmaps (s3-s5), vertex AO.
* **Plan: DDGI probe volumes** (Majercik 2019): grids around the camera plus per-interior volumes from portal / zone bounds [I];
  N rays per probe per frame via ray query, irradiance + visibility octahedral atlases, temporal hysteresis.
* **Integration (ABI point)**: with GI on, the generated material PS replaces the ambient term (`g_AmbientLight` /
  `g_IndoorAmbientColor` found by CTAB name) with a DDGI lookup; lightmaps rebound or kept (setting "replace ambient | replace
  ambient + lightmaps"); direct lights stay the engine's.
* Later: ReSTIR GI as an "ultra" tier; GTAO (XeGTAO, MIT) as the non-RT fallback. Everything runs in the FP16 target before
  `post_hdr.fx`; replacing the tonemapper (ACES / AgX) and HDR10 output are later options (gd3d11.md 3.6).

## 4. Player configuration (done: overlay.md)

* Files in `<game>\bin\rvk\` (never `data\`): `config.toml` (developer / debug), `settings.toml` (player, written by the overlay
  atomically), caches in `cache\`.
* Settings registry: key, type, default, range, category, label, `restart` flag, required capabilities, presets; drives TOML I/O,
  the overlay, validation and the `rvk_setting_*` C API.
* Presets Off (parity) / Low / Medium / High / Ultra / Custom: open, come with the first enhancement that needs them.
* Capability resolution (requested -> effective + reason, greyed in the UI): done (raytracing.md 3).
* Features apply at a frame boundary (`d3d9::Device::beginFrame`); only the swapchain format (HDR10) and `rt.allow` need a restart.
* UI: Dear ImGui overlay drawn in `SwapScreen` after the GUI, input through a WndProc filter (the engine uses window messages,
  WM_INPUT, XInput only), F11 opens it (overlay.md 3). Integration into the game's own options menu via spass: optional, later.

## 5. "Do now" prerequisites (all done)

Parity output unchanged with every feature off.

| # | item | status / doc |
|---|---|---|
| 1 | Phase markers (frame / view / pass on `d3d9::Device`) | done, frame-phases.md |
| 2 | Draw tags + per-frame draw records | done, draw-tags.md |
| 3 | Persistent resource identity (id + generation on `Buffer` / `Texture`) | done, draw-tags.md 2 |
| 4 | Shader semantics from CTAB on `d3d9::Shader` | done, draw-tags.md 3 |
| 5 | ABI extension slots (`drawId`, `flags`, `prevVsConsts`, `frame` = FrameBlock; translator hooks); motion / normal outputs, `prevVsConsts` tracking, jitter deferred to their first consumer | done, injection.md 3 |
| 6 | Injection points (`Device::addPass`, `vk/passes.h`) | done, injection.md 4 |
| 7 | Role tagging of engine images (scene HDR, refraction copy, depth map, sky cube) | done, frame-phases.md 4 |
| 8 | Optional RT capability (`rt.allow`, conditional buffer usage) | done, raytracing.md |
| 9 | Settings registry + `settings.toml` + capability resolution | done, overlay.md 2, raytracing.md 3 |
| 10 | Camera / frame block | done, frame-phases.md 5 |

Not now: a full render graph with aliasing (GD3D11 lost devices with aliasing on NVIDIA), GPU-driven rendering, DGC.

## 6. Phased roadmap

| milestone | content | acceptance | effort | status |
|---|---|---|---|---|
| **M6 Platform** | do-now 1-10, overlay, capability report, tint view, tag histogram | refcompare pixel-identical with features off; tint view classifies water / palms / hero; no input leak; fps cost < 2 % | 3-4 wk | done |
| **M7 Enhanced water** | own water pass (engine material evaluated, SSR + env cube, absorption, foam, glint, wave grid, shore) | engine vs enhanced at the water spots; toggle without hitch > 1 frame; GPU < 1.5 ms at 1080p; validation clean | 3-4 wk | done (water.md); open: underwater post, area profiles |
| **M8 Ocean** | FFT cascades on the wave grid, sea floor field, breaking waves, foam | no seams at shore / horizon; swim line within the cap; bake < 2 s, cached; ocean GPU < 2 ms (High) | 3-5 wk | done (ocean.md) |
| **M9 RT reflections** | RT instance registry, BLAS cache, TLAS, ray-query water reflections, then glossy surfaces; motion vectors + denoise | off-screen reflections at `harbour_docks`; no BLAS leaks in 30 min; runtime toggle; graceful unsupported path; GPU budget per preset | 6-8 wk | P1-P3 done (rt-reflections.md); P4, dynamic geometry, denoise open |
| **M10 RT GI** | DDGI volumes, hit shading, ambient / lightmap replacement, GTAO fallback | interiors (`pub`, monastery) and night: no light leaks; temporal stability (reference-compare.md 6.2); fps per preset; parity unchanged | 8-12 wk | open |

Order: M6 is required by everything; M7 / M8 need only the floor and reach every player; M9 builds the RT infrastructure with a
contained consumer (water); M10 reuses it.

## 7. Open questions

* ~~Material name getter~~ answered (`eCResource2 +0x40`, water.md 2-3). ~~Albedo sampler~~ answered (rt-albedo-facts.md).
* Whether gfx admin `+0x5b8` (the refraction-copy gate) is a quality option; refraction quality vs the depth-map z pre-pass (`this[0x1590]`).
* The underwater effect path (overlay image effect?) and its trigger.
* ~~Input API / free hotkey~~ answered (overlay.md 3).
* Size of `eCRenderContextMesh::Create` / `Invalidate` and the `Destroy` slot before detouring (ICF / thunk check).
* ~~Water draws at the beach~~ answered (water.md 2.2).
