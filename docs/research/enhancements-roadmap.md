# Enhancements roadmap: water, ocean, RT reflections, RT GI, player settings

Research and architecture note for the north star in `docs/PLAN.md`: the Vulkan backend becomes the platform for custom water
and ocean simulation, ray-traced reflections and ray-traced global illumination. **Every enhancement is optional and
player-configurable, and the plain D3D9-parity path always stays available.**

Legend: **[C]** confirmed (decompiled, disassembled, shader source, or our code), **[I]** inferred or proposed.
RVAs are Engine.dll unless marked `Game.dll` or `Script.dll`. Dumps: `B:\DevDrive\risen-ghidra\rvk_enh_dump.txt`,
`rvk_enh_dump2.txt`, plus `B:\DevDrive\risen-gfx-re\frame.txt`. Shader sources (read only, through the test copy's junction):
`data\compiled\effects\shadermaterial\*.fx[h]`. GD3D11 facts come from a read of `E:\git\github\GD3D11` (`D3D11Engine\...`).

---

## 0. Summary

* The engine is a **forward renderer** with a linear-depth z pre-pass and an **FP16 HDR scene target** (`A16B16G16R16F`,
  hal.md 5), tonemapped in `post_hdr.fx`. Water is a normal mesh with an `eCShaderDefault` material that has a
  refraction element. It is drawn from the frustum database's **refracted list**, sampling a copy of the scene
  (`smpRefract` s1) and the linear depth map (`smpDepthmap` s2). Reflections come from a 128² sky-only cube map [C].
* **World transforms, bones and instance data are plain VS constants** (`g_vInstanceData` from c32, `g_vViewProjection` c0,
  ...). They have CTAB names in every material shader of the corpus. Skinning runs on the GPU in the material VS [C].
* **Each engine draw goes through a persistent `eCAPIPrimitive`**, which is our `HalPrimitive`, owned by an `eCGfxVisual`
  per mesh resource. So geometry identity is cheap and stable, and from any render context we can reach our own vertex
  and index buffers without drawing [C].
* Recommended semantics: (1) **phase markers** from detours on large engine functions, (2) **draw tags** from the
  render-context / sub-mesh render functions, (3) **constant harvesting by CTAB name** (camera, world, lights, fog),
  (4) a **persistent RT instance registry** fed from render contexts (they are reachable for off-screen objects too),
  with lifetime hooks. (c) "read the world directly" (spass-style entity walks) is only a supplement, for water zones.
* RT = optional tier on top of the 1.3 floor: `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` (+
  `deferred_host_operations`), inline ray queries in compute. No RT pipelines at first.

---

## 1. Scene semantics: what each draw is

### 1.1 What the engine gives us [C]

**Frame and passes** (`eCFrustumDatabase::Execute` `0x66e990`, frame.txt 985-1483). The render-op lists are
`bTValArray<eCRenderOp2>` with header `+0`, data `+8`, count `+0x10`:

| list (db offset) | pass | dispatch (render-context vtable) |
|---|---|---|
| `+0x1518` | z pre-pass into the depth map (`this[0x1590]`, between `BeginZPass` `0x6b9720` and `EndZPass` `0x6b9910`) | `+0x18` RenderZPass |
| `+0x1500` | solid, qsorted, after `UpdateShadowMask` `0x5748d0`, `BeginMaterial` `0x6b9d20`, `DoBeginRenderSolid` `0x4cb810` | `+0x00` Render |
| `+0x1560` | debug | `+0x00` |
| (none) | `eCModuleAdmin` modules (vtbl `+0x158`), `RenderDecals` `0x4fc220`, `RenderBillboards` `0x4f0530`, list `+0x1530` | |
| (none) | **refraction copy**: if the caller passed an `eSRenderTargetDesc` with a texture and gfx admin `+0x5b8` is set, then `EndMaterial`, `eCAPITexture::CopyCurrentRTToImage` (slot 11, ours: `src/hal/vulkan/resources.cpp:80`) of `desc->image+0x60`, and `eCShaderMaterialAdmin+0x248` (the refract map) = that image | |
| `+0x1578` | **refracted** (water), after `DoBeginRenderAlpha` `0x4cb710` | `+0x10` RenderRefracted |
| `+0x14e8` | alpha, depth-sorted | `+0x08` RenderAlpha |
| `+0x1548` | after `DoEndRenderAlpha` `0x4cb680` | `+0x08` |

* `Execute` runs for the main view (`eCCameraBase::Render` `0x4f8470`), for the image-filter scene RT
  (`eCImageFilterScene::OnUpdateOutput` `0x75cf10`, the normal in-game case with the default `DoF` image effect), for
  `RenderEntity` `0x66ceb0` (previews), and with variants for shadow maps (`ExecuteShadowMap` `0x66f630`). **A tag must
  record which view is running.** Compare the camera with `eCGfxCamera::ms_pMasterCamera` (`0xc62590`).
* **`eCRenderOp2`** (0x18 bytes, `Create` `0x67f680`):
  * `+0` `eCRenderContext*`
  * `+8` desc: bits 0-7 = sub-mesh index (`0xff` = all); bits 8-31 = hierarchy node, or `0xffffff` = instanced batch
  * `+0xc` fade (float, 1.0 by default)
  * `+0x10` 24-bit view-depth key
  * `+0x14` sort id: material id `& 0xfff`, the shader's additional sort id `<< 12`, and `shader+0x7c << 20`

  It does **not** keep the material pointer.
* **`eCRenderContextMesh`** (vtable `0x9ae070`; `RequestRenderOps` `0x5c75e0`):
  * `+0x10` the entity. Its `+0x60` is the **world matrix** (`bCMatrix`, also `eCGeometryEntity::GetWorldMatrix`
    `0x6aaff0`), `+0xf8` illumination, `+0x28` flags.
  * `+0x18` the mesh PS, with LOD at `+0x20`.
  * `+0x28` `eCMeshResource2*`.

  `RequestRenderOps` puts a sub-mesh into the refracted list when its material's flag mask has `0x20`, the alpha list for
  `4`, solid for `2`/`0x200`, and z-pass for `0x18`.
* **`eCMeshResource2`**:
  * `+0x58` sub-mesh array (stride 0x48), `+0x60` sub-mesh count
  * `+0x88` **`eCGfxVisual*`**
  * `+0xa0` instancing flag, `+0xa8..0xb8` render-instance array (0x58 bytes: matrix, LOD, illumination, flags;
    `AddRenderInstance` `0x7602a0`, `ExecuteRenderInstances` `0x7607b0`)

  **`eCSubMesh`** (`Render` `0x5c7eb0`) fields: `+0x28` start index, `+0x2c` index count (primitive count = /3), `+0x30`,
  `+0x34` (vertex range), `+0x38` `eCMaterialResource2*`, `+0x40` hierarchy (quad tree ranges).
* **`eCGfxVisual::Execute`** (`0x5069a0`) calls device slots `0x230/0x240/0x248/0x250` with visual `+0x18..+0x24`, then
  `eCGfxShader::Execute` (visual `+0x60`), then **`eCAPIPrimitive::Execute`** on visual `+0x10`. The primitive is ours
  (`HalPrimitive`, `src/hal/vulkan/hal_vulkan.h:163`, `resources.cpp:372`). `eCSubMesh::Render` writes the draw
  description per sub-mesh into it (`GetDrawCallDescription` slot 12) before each pass. It sets the world matrix through
  the shader (vtbl `+0x208`, `+0x210` = global alpha), and the passes through `GetPasses` + vtbl `+0x1d0` (= `ExecutePass`
  `0x6d2060`).
* **Material**: `eCMaterialResource2` `+0x70` is the `eCShaderBase*` and `+0x78` the material id. `IsRefracted`
  (`eCShaderDefault` `0x6c9d20`) is true when the shader's refraction colour-source GUID (`+0x410`) is valid. Base classes
  return false (`0x6d2f60`).
* **Skinned actors**: `eCRenderContextAnimation2` (`Render(op)` `0x4dd860`) calls `eCAnimation_PS::Render` `0x4d6fe0`, then
  `eCWrapper_emfx3ActorInstance::UpdateRenderMatrices`, then `eCAnimation_HardRenderActor2::Render` `0x4dd650` and
  `RenderPrimitive` `0x4dd0a0`.
* **Other contexts and emitters**:
  * SpeedTree `0x6ee320`, SpeedTree billboards `0x6ee8d0`, vegetation (instanced) `0x70d4b0`
  * sky `eCSkydome_PS::Render` `0x6e4730`
  * particles `eCParticle_PS::Render` `0x5e0080`, strips `0x6feae0`
  * decals, billboards (above), GUI in `eCApplication::Draw` `0x4c13b0`
* **Camera** (`eCShaderMaterialAdmin::SetCamera` `0x6ba1d0`) writes the VS pool:
  * c0 = view-projection (camera `+0xe0`, or `+0xa0 * +0x20` with the depth-bias flag at admin `+0x310`)
  * c4 = inverse of `+0x60` (projection)
  * c8 = camera `+0x120` (inverse view 3x3)
  * c11 = camera position (`+0x150..0x158`) and far

  The PS pool also gets the position and far plane, plus screen-size terms.
* **VS constant layout of material shaders** (`ge_common_vs.fxh`, `ge_default.fxh`, `ge_default_vs.fx` 213-247) [C]:

  | register | content |
  |---|---|
  | c0 | `g_vViewProjection` |
  | c4 | `g_vInvProjection` |
  | c8 | `g_vInvView` |
  | c11 | `g_vCamPosAndFar` |
  | c12-c29 | vegetation and wind |
  | c30 | fog |
  | c31 | `g_vInstanceMisc` |
  | c32+ | `g_vInstanceData[]`: `[0].a` global alpha, `[1..3]` world 3x4 (plain); instanced `[i*4+1..+3]`; **skinned: bone `i` at `[idx*3+1..+3]`, blended by BLENDWEIGHT/BLENDINDICES** |
  | c232 | `g_vShaderData` |

  PS registers:

  | register | content |
  |---|---|
  | c0 | camera / far |
  | c3 | fog colour |
  | c6 | `g_ShaderData` |
  | c14 | `g_IndoorAmbientColor` |
  | c15-c21 | `g_AmbientLight[7]` |
  | c22 | `g_DirLight` |
  | c25+ | point lights |
  | s0 | shadow mask |
  | s1 | refraction |
  | s2 | depth map (linear: `.r * g_FarDist`) |
  | s3-s5 | lightmaps |
  | s9-s15 | `g_SamplerData[7]` material samplers |

* **CTAB names survive in the shipped bytecode**. Corpus: `g_vInstanceData` in 141 of 149 VS, `g_vViewProjection` 148,
  `g_SamplerData` 2286 of 2334 PS, `smpRefract` 132 PS. `g_bSkinned` / `g_bInstanced` are compile-time and absent: detect
  skinning from the declaration (BLENDINDICES) and instancing from the stream frequency / TEXCOORD4 input.

### 1.2 Options compared

| option | gives | cost / risk |
|---|---|---|
| (a) classify by shader / CTAB / material | camera, world matrix, bones, lights, fog per draw, purely at the D3D9 level; skinned vs instanced vs static from the declaration; post-chain draws from effect names | cheap, robust, no engine hooks. Cannot tell water from other refracted materials, terrain from props, or an object's identity across frames on its own. |
| (b) engine-level hooks | phase (z / shadow / solid / refracted / alpha / sky / GUI / post), context type, entity, material, sub-mesh, LOD; **off-screen objects** through render contexts | needs RVAs and care with ICF thunks (below) |
| (c) read world data directly | water zones (`Game.dll gCNavigationAdmin::GetWaterZonePSList` `0x8fc830`, `gCWaterZone_PS` sub-zones: polygon points, `Min/MaxX/Z`, `FaceNormal`, `DepthRange`); entities by spass's Lua tooling | late (Lua at ~19 s), gameplay data only, not geometry |

**Recommendation: (b) + (a), with (c) for water zones.**

1. **Phase markers** (detours on large functions only):
   * `eCFrustumDatabase::Execute` `0x66e990` (entry/exit + view kind), `BeginZPass` / `EndZPass`
   * `eCIlluminationAdmin::UpdateShadowMask` `0x5748d0`
   * `eCModuleAdmin::DoBeginRenderSolid` `0x4cb810` / `DoBeginRenderAlpha` `0x4cb710` / `DoEndRenderAlpha` `0x4cb680`
   * `eCSkydome_PS::UpdateSkydomeEnvCubemap` `0x6e7ed0`, `eCImageFilterScene::OnUpdateOutput` `0x75cf10`
   * `eCApplication::Draw` `0x4c13b0` (GUI; spass hooks `DoRender`, not `Draw`)

   Refracted vs alpha follows from the order inside `Execute`: after the refraction copy, until the first alpha op.
   Alternatively, patch the **`eCRenderContextMesh` vtable slots** `+0x10` / `+0x08` (`0x9ae080` / `0x9ae078`). The
   frustum-database loops call these indirectly [C]. Patching the render-context vtables is safe; the "own vtables"
   rule only concerns the `eCDX9*` classes.
2. **Draw tags**: detour **`eCSubMesh::Render` `0x5c7eb0`** (static meshes: sub-mesh, material, world-matrix pointer,
   mesh resource), **`eCRenderContextMesh::Render(op)` `0x5c7110`** (context + op, including the instanced batch path),
   `eCAnimation_PS::Render` `0x4d6fe0` (skinned), and the SpeedTree / vegetation / sky / particle / decal / billboard
   functions above. Each sets a thread-local "current tag" (`kind`, context, material, sub-mesh, entity); our
   `HalDevice::Draw` (`src/hal/vulkan/device.cpp:1002`) copies it into the draw record and clears it when the function
   returns.
   **Do not detour** `eCRenderContextMesh::Render(op, db)` / `RenderAlpha` / `RenderRefracted` / `RenderZPass` at
   `0x5c73e0 / 0x5c73f0 / 0x5c7400 / 0x5c7410`. They are 16 bytes apart (tail-jump thunks into `0x5c7110`; Ghidra shows
   them inlined). The same applies to `eCRenderOp2::Render*` `0x67f5c0..0x67f660` and to `eCRenderContextMesh::Destroy`
   `0x5c72c0` (0x20 bytes): hook the vtable slot, or the caller.
3. **Constant harvesting** (a): per draw, the d3d9 device already has the VS/PS constants (`src/d3d9/device.h:421`) and
   the shader's CTAB (`src/fx/effect_binary.h:118`). Resolve the registers once per shader **by name** (`g_vInstanceData`,
   `g_vViewProjection`, `g_DirLight`, `g_AmbientLight`, `g_FogColor`, `smpRefract`, `smpDepthmap`, `g_SamplerData`) into
   a `ShaderSemantics` record cached on `d3d9::Shader`. Camera = c0/c4/c8/c11 of any material draw of the main view
   (cross-check `eCGfxCamera::ms_pActiveCamera` `0xc62580`).
4. **Geometry identity**: (`HalPrimitive*`, or `Buffer` id + generation for VB/IB, plus start index, primitive count, base
   vertex, and the declaration's position element). Stable across frames; the engine creates visuals at mesh load. Our
   static `Buffer` gets a **new `VkBuffer` on every update** (`src/d3d9/resources.cpp:401-421`): give it a generation
   counter.
5. **RT instance registry** (for off-screen geometry; the draw stream only contains the frustum, after culling):
   * Hook `eCRenderContextMesh::RequestRenderOps` `0x5c75e0`. It runs for every view including the shadow cascades, which
     cover a large area towards the sun.
   * Hook context creation and destruction (`Create` `0x5c7360`; `Destroy` / `Invalidate` through vtable slots).
   * Optionally run an **extra extraction** with a synthetic wide camera: `eCRenderSystemAdmin::Extract(db)` `0x66cd80` on
     an own `eCFrustumDatabase`, without `Execute`, the way the shadow-map path does.
   * From a context: `+0x28` mesh resource → `+0x88` visual → `+0x10` **our `HalPrimitive`** (VB/IB/declaration), sub-mesh
     ranges at `+0x58`, materials at sub-mesh `+0x38`, world matrix at entity `+0x60`. **No draw is needed to build a BLAS.**
   * Skinned, wind-animated and instanced-vegetation geometry comes from the draws of the current frame (section 3.3).
6. **Water identity**: draws tagged *refracted mesh* (phase markers + `eCSubMesh::Render` tag). Water vs other refracted
   materials (ice, glass) is decided by a **configurable material-name list**, like GD3D11's `OceanIdentifiers` (default
   `"NW_WATER_LAKE*|*OCEAN*"`, `WaterProfile.h:45`). The material name comes from `eCMaterialResource2` (resource name)
   [I: getter to bind]. Ocean vs lake: material name + `gCWaterZone_PS` membership (Game.dll).

---

## 2. Water and ocean

### 2.1 How Risen renders water today [C unless marked]

* **Geometry**: ordinary `eCMesh_PS` meshes. Their material (`eCShaderDefault`) has a refraction colour source. The VS is
  the normal material VS (`ge_default_vs.fx`). Waves come from scrolling texture coordinates and normal maps; there is no
  geometric simulation [I: animation via `eCTexCoordSrc*` scroll elements, not traced].
* **Pass**:
  1. Solid pass into the FP16 scene RT of the image effect.
  2. `CopyCurrentRTToImage` into the refraction image (`eCShaderMaterialAdmin::SetRefractmap` `0x6b9a10` = admin `+0x248`).
  3. Refracted list (`+0x1578`).
  4. Alpha list.

  The copy only happens when `Execute` got a render-target description (the image-effect path). Without an active image
  effect there is no refraction source.
* **Shader** (`ge_default.fxh` `Refract`, lines 559-581):
  * Distort the screen position by `m_vecDistortionOffset`.
  * Above low quality, reject the offset when the depth map sample there is in front of the water (`smpDepthmap.r *
    g_FarDist >= srcDepth`).
  * `lerp(refract, shaded, alpha)`.
  * Reflection: `m_vecWorldReflect = reflect(-eye, normal)` (`ge_default_ps.fx:255`) feeds a reflect texcoord source
    (`eCTexCoordSrcReflect`, `eEReflectType`).
  * The environment is the **sky-only cube map** (`eCIlluminationAdmin::Process` / `UpdateSkydomeCubemap` `0x574630`,
    then `eCSkydome_PS::UpdateSkydomeEnvCubemap` `0x6e7ed0`: 6 faces with the sky dome only, 128² A8R8G8B8 cube RT,
    hal.md 5).
  * **No planar or screen-space reflections, no shore foam, no depth fog.**
* **Gameplay water** is separate (Game.dll):
  * `gCWaterZone_PS` (ctor `0x945780`; sub-zones `gCWaterSubZone_PS`; `AccessDepthRange`, `AccessEffectMaterial`,
    `UseWadeMovement`, `OnContact`)
  * registered in `gCNavigationAdmin` (`RegisterWaterZonePS` `0x8fc8d0`, `GetWaterZonePSList` `0x8fc830`)
  * character state: `gCCharacterMovement_PS` (`WaterWadeDepth`, `WaterDeathDepth`, `TreatWaterAsSolid`)
  * Script.dll: `PSNavigation::GetUnderWaterDepth` `0x2283b0`, `Entity::IsSwimming` `0x209060`
  * splash emitters: `gCWaterZone_PS::CreateEmitter` `0x945550`
* **Underwater**: no engine-side water post effect was found. Underwater look is presumably an overlay image effect
  (`eCImageFilterAdmin+0x98`; `post_wobble` / `post_distort`) switched by game scripts [I, to verify in game].

### 2.2 Custom water (M7): replace the shading, keep the engine's geometry and gameplay

* **Hook point**: in `HalDevice::Draw`, a draw tagged *water* (refracted mesh, material on the water list) while the
  `water.enabled` setting is on is **not executed**. Its packet goes into a per-frame water list instead: VB/IB, ranges,
  world matrix, and harvested constants (camera, `g_DirLight`, ambient, fog, time). At the end of the refracted phase
  (the first alpha op, or `DoEndRenderAlpha` as a fallback), the water module records its own pass into the same scene
  RT and depth, outside our implicit rendering scope (`endRendering()`, `src/d3d9/draw.cpp:13`).
* **Inputs we already have**:
  * the refraction copy (the texture that just received `CopyCurrentRTToImage` in the main view; tag it there)
  * linear depth (the depth map, s2 of the water draw)
  * hardware depth (`D32_SFLOAT`, `SAMPLED` already: `src/d3d9/resources.cpp:45`)
  * the sky env cube (the 128² cube RT; tag it in `UpdateSkydomeEnvCubemap`)
  * sun and fog from the harvested PS constants, which keeps lighting and fog consistent with the engine without more RE
* **Shading**:
  * Fresnel; SSR against the HDR scene copy and depth, with the env cube as fallback, RT later
  * refraction with depth-based absorption and colour extinction
  * shore foam by depth difference (GD3D11 "SIMPLE" style), sun glint
  * all in linear HDR (the scene target is FP16, so there is no gamma-space hack like GD3D11's `WaterShading.hlsl`)
* **Underwater**: camera position against the water-zone polygons and the surface height. Apply an own fog and
  absorption post (toggle). Leave the engine's overlay effect alone.
* **Licence**: our project is MIT. GD3D11's *D3D11-era* water shaders are GPL with mixed authorship (gd3d11.md 1):
  re-implement those. **GD3D11's new D3D12 and Vulkan code is the user's own and may be reused** (user, 2026-10-21; `git log`
  shows a single author): `D3D11Engine/D3D12Engine/*` (`D3D12Water`, `D3D12Ssr`, `D3D12RayTracing` inline ray queries,
  `D3D12RenderGraph`, `D3D12GpuScene`/`D3D12MeshArena`, `D3D12Motion`, `D3D12GTAO`, `D3D12Fsr`, `D3D12PointShadows`),
  `D3D11Engine/VulkanEngine/*` (RHI, `VulkanImGui`) and `Shaders/D3D12/*` (`WaterRT`, `RtShadows`, `RtScene`, `XeGTAO`). Port
  from HLSL/D3D12 to our GLSL/Vulkan where it fits; credit the origin in a comment. Check `git log` authorship before reusing
  any older D3D11-era file.

### 2.3 Ocean simulation (M8)

* **Simulation**: Tessendorf FFT, 3 cascades (e.g. 256², with tile sizes ~250 / 37 / 5 m), JONSWAP spectrum with
  wind from the weather state. Compute passes produce displacement, slope and Jacobian (foam) textures, with foam
  accumulated over time.
  * Floor-only features suffice: compute + storage images, 1.3 core.
  * Quality: cascade count, resolution, update rate.
* **Mesh**: the engine's sea meshes are too coarse to displace. For *ocean* materials, draw an **own camera-centred
  clipmap / projected grid** clipped to the ocean zones. The engine's water draw is skipped (as in M7); its world bounds
  and height define the plane. Lakes and rivers keep the M7 shading on their own meshes (normal-map detail from the FFT
  slopes, no displacement).
* **Shoreline**:
  * Attenuate amplitude and add foam with (1) the per-pixel depth difference and (2) a **baked shore distance field**
    per ocean zone. Bake it once per area by rendering the static geometry from above (orthographic, from the RT
    registry's static instances or the captured static draws) into a heightfield, then a distance transform (GD3D11
    `ShoreField.cpp` idea: Felzenszwalb).
  * Cache it in `bin\rvk\cache\` (not in `data\`).
* **Physics and gameplay**: visual only. The engine keeps its flat water plane for swimming, wading, splashes and
  buoyancy. Cap the displacement near the camera and the player (e.g. ±30 cm, a setting), so feet and the swim line stay
  plausible. Splash and emitter positions stay the engine's.

---

## 3. RT reflections and RT GI

### 3.1 Feature tier and device

* **RT tier** (optional, probed in `src/vk/probe.*`, enabled in `Context::createDevice` `src/vk/context.cpp:206`):
  * `VK_KHR_acceleration_structure` + `VK_KHR_deferred_host_operations` + `VK_KHR_ray_query` (+ `bufferDeviceAddress`,
    already on: `context.cpp:242`).
  * Optional: `VK_KHR_ray_tracing_position_fetch` (hit positions without own vertex fetch), `VK_EXT_opacity_micromap`
    (alpha-tested foliage), `VK_KHR_ray_tracing_maintenance1`.
* **Ray query (inline, compute) rather than ray-tracing pipelines**: no SBT, it fits our bindless + push-constant model,
  works from fragment shaders too (water, glossy), and GD3D11's DXR path is inline-only too (`D3D12RayTracing.cpp`). Add
  `VK_KHR_ray_tracing_pipeline` later only if GI hit shading needs it.
* **Enable the extensions at device creation whenever the GPU has them** (setting `rt.allow`, default true). The feature
  toggles then work at runtime without a restart. Device creation is one-time and early (`InitializeAdmin`).
* **Buffer usage decision**: when the AS extension is enabled, **always** create static VB/IB with
  `SHADER_DEVICE_ADDRESS | ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_KHR | STORAGE_BUFFER`
  (`src/d3d9/resources.cpp:405`), and add the build-input bit to the frame ring (`src/vk/frames.cpp:189`; it already has
  BDA + storage). The flags cost nothing measurable on desktop drivers and avoid re-uploading all geometry when the player
  turns RT on. Without the extension they are not set (the floor path stays exactly as it is).

### 3.2 Static geometry (BLAS)

* Source: registry instances (section 1.2 item 5) → `HalPrimitive` → `Buffer`.
  * Position is the declaration's POSITION0 element (FLOAT3 at offset 0 of the 52-byte static vertex, engine-gfx.md 5).
    Other types need a conversion compute pass.
  * Indices are 16 or 32 bit.
  * One BLAS per (VB gen, IB gen, sub-mesh range set) of a mesh resource, one geometry per sub-mesh (material). Build
    with `PREFER_FAST_TRACE | ALLOW_COMPACTION`, then compact.
  * Budget per frame, LRU eviction, memory cap: GD3D11 uses 48 builds and 400k triangles per frame, a 384 MB cache, and
    eviction after 120 unused frames.
  * **Skip** water, refracted, alpha-blended, decal and particle materials. Skip LOD > 0, or use the lowest LOD for
    distant instances (RT LOD bias setting).
* Instance transform: entity `+0x60` (static), or `AddRenderInstance` matrices for instanced batches.
* Hit data: a per-geometry record (BDA of VB/IB, vertex stride and offsets of UV / normal, bindless indices of albedo /
  normal / lightmap, flags), reachable via `instanceCustomIndex + geometryIndex`.
* **Albedo slot**: s9-s15 (`g_SamplerData`) are generated per material. Which sampler is the diffuse map needs RE of
  `eCShaderBase::GetDeclarations` `0x6d29b0` / `eSShaderMaterialDeclaration` [open]. Interim heuristic: the first
  `g_SamplerData` sampler with a BC1/BC3 colour texture.

### 3.3 Dynamic geometry

* **Skinned characters**: bones are VS constants, and skinning is in the material VS [C]. Two options:
  1. A compute skinning pass that re-implements `GetWorldMatrix` (4 bones, 3x4 matrices from c33+) over the draw's VB.
     It is simple, but tied to this vertex layout.
  2. **VS capture** [I, preferred because it is generic]. The GLSL generator emits a capture variant of any VS: rasteriser
     discard, `gl_Position` written to an SSBO indexed by `gl_VertexIndex`, and the draw replayed with **c0..c3 = identity**.
     `g_vViewProjection` is the only clip transform, so `oPos` becomes the world position.
     * It also covers wind (SpeedTree, vegetation), morph targets, billboards and instancing exactly as the engine animates
       them.
     * Needs `vertexPipelineStoresAndAtomics`.
     * Then rebuild a `PREFER_FAST_BUILD | ALLOW_UPDATE` BLAS per frame (refit when the topology is unchanged).

  Off-screen characters are dropped from the TLAS. A setting keeps their last pose for N frames.
* **Alpha-tested foliage** (SpeedTree leaves, `ge_default_leafs.fx`, vegetation): non-opaque geometry.
  * Ray query candidate loop with a UV fetch and an alpha test against the material's alpha texture.
  * Opacity micromaps when available.
  * Grass (instanced vegetation) is excluded at low or medium quality (instance mask).
* **TLAS**: rebuilt every frame (`PREFER_FAST_TRACE`). Instance masks: static world, props, dynamic, foliage, water. Limit
  to a radius around the camera (setting).

### 3.4 Shading inputs, G-buffer, motion

* **Forward renderer**, no G-buffer [C]. Available today:
  * the FP16 HDR scene colour
  * hardware depth
  * the engine's **linear R32F depth map** (when its z pre-pass is on: SSM / DoF / refraction quality)
* **Normals and roughness**: add optional extra outputs to the *translated* material PS, behind specialisation constants
  (`src/shader/glsl.cpp:800-830` emits the outputs):
  * the interpolated world normal (material VS output `vNormalAndAO.xyz`, found by semantic)
  * a gloss proxy from the specular constants

  Bump detail and the exact specular power are not recoverable from bytecode generically: use geometric normals plus
  depth-reconstructed normals for SSR / RT reflections first.
* **Motion vectors** (needed by temporal denoisers, TAA, DDGI stability):
  * Keep last frame's camera (harvested c0) and, **per geometry identity, last frame's VS constant block address**.
  * A generated VS variant transforms with both and writes `prevClip - curClip` to an RG16F target.
  * Static geometry falls back to camera-only reprojection from depth.
  * TAA jitter goes into the existing `posFix` (`src/shader/abi.h:13`, `draw.cpp:404`).

### 3.5 Reflections (M9)

* Ray-query reflection pass in compute at half or full resolution:
  * rays from depth + normal for water (first consumer, replacing SSR misses) and for glossy or wet surfaces (roughness
    gate, setting)
  * hit shading: albedo × (sun with a shadow ray + harvested ambient + lightmap at the hit UV2 if present), plus fog
  * miss: the env cube
* **Denoise**: temporal accumulation with motion vectors + a spatial bilateral filter. Candidates: AMD FidelityFX SSSR /
  Denoiser (MIT, HLSL: would have to be ported to GLSL — no HLSL in this project, PLAN.md) or own SVGF-lite. NVIDIA NRD: check its licence before use.
* Composite in linear HDR before the engine's post chain, at the end of the main view's `Execute`.

### 3.6 Global illumination (M10)

* **Engine lighting today** [C]:
  * direct sun (`g_DirLight` c22) + point lights (c25+)
  * hemispherical ambient (`g_AmbientLight[7]` c15-21, `g_IndoorAmbientColor` c14)
  * baked lightmaps (s3-s5; `eCMeshResource2::BindLightmaps` `0x764220`; the editor's radiosity baker
    `eCLightmapPreprocessor`)
  * vertex AO
* **Recommended: DDGI probe volumes** (Majercik et al. 2019, implement from the paper):
  * probe grids around the camera, plus per-interior volumes from portal / zone bounds [I]
  * N rays per probe per frame via ray query, irradiance + visibility octahedral atlases, temporal hysteresis
  * stable without a G-buffer, cheap, and it works in a forward renderer
* **Integration (the main ABI point)**:
  * When RT GI is on, the generated material PS replaces the ambient term: the reads of `g_AmbientLight` /
    `g_IndoorAmbientColor` (found by CTAB name) become a DDGI irradiance lookup at the world position and normal.
  * Lightmap samplers are rebound to a neutral texture for lit-by-GI materials, or kept (setting "GI mode:
    replace ambient | replace ambient + lightmaps").
  * Direct lights stay the engine's, so there is no double counting of the sun.
* **Later / alternatives**:
  * ReSTIR GI (screen-space reservoirs, needs the motion and normal targets and a heavier denoiser) as an "ultra" tier
  * screen-space GI / GTAO (Intel XeGTAO, MIT) as the **non-RT fallback**
* **HDR / tonemapping**: everything above runs in the FP16 scene target, before `post_hdr.fx` (eye adaptation, `ToneMap`).
  A later option replaces the tonemapper (ACES / AgX) and adds HDR10 output (GD3D11's swapchain code is reusable,
  gd3d11.md 3.6).

---

## 4. Player configuration

* **Two files** in `<game>\bin\rvk\` (`bin` is not junctioned; never `data\`):
  * `config.toml`: developer and debug settings (existing, `src/core/config.h:18`)
  * **`settings.toml`**: player settings written by the overlay (atomic: write `.tmp`, then `MoveFileEx`). Optional
    per-world overrides `settings.<world>.toml` later.

  Caches (pipelines, shore fields) go in `bin\rvk\cache\`.
* **Settings registry** (X-macro table, like `src/engine/symbols.h`). Each entry has key, type, default, range / enum,
  category, label / tooltip, `restart` flag, required capability bits, and the preset values. One table drives TOML I/O,
  the overlay UI, validation, and a C API for tools and mods (`rvk_setting_get/set/list` in `include/rvk_api.h`; spass Lua
  can build an in-game menu page later).
* **Presets**: Off (parity), Low, Medium, High, Ultra, Custom. Editing any value switches to Custom (GD3D11 compares the
  whole struct each frame).
* **Feature switches**:
  * per feature: `water.mode = engine|enhanced`, `ocean.enabled`, `rt.reflections`, `rt.gi`, ...
  * qualities: `water.ssr_steps`, `ocean.cascades`, `rt.max_distance`, `rt.foliage`, `gi.probe_density`, ...
* **Capabilities**:
  * the probe produces `caps` (RT tier, ray query, position fetch, OMM, EDS3, memory budget)
  * each setting resolves to *requested → effective + reason* ("ray tracing: off, the GPU has no
    `VK_KHR_ray_query`"), logged at startup and shown greyed out in the UI
  * GD3D11 does the same with `GraphicsDeviceCapabilities` / `ApplyDeviceCapabilities`
* **Runtime switching**: features apply at a frame boundary (`d3d9::Device::beginFrame`): create or destroy their
  resources, flip the shader variant (specialisation constant). With the RT extensions enabled at startup (section 3.1),
  only the swapchain format (HDR10) and the RT extension allowance need a restart.
* **In-game UI**: a Dear ImGui overlay (vcpkg `imgui[vulkan-binding,win32-binding]`, `imgui_impl_vulkan` with
  `UseDynamicRendering`). It is drawn in `HalDevice::SwapScreen` (`src/hal/vulkan/device.cpp:580`) after the GUI and
  before the present.
  * Input: subclass the game window's WndProc.
  * Hotkey: configurable, default `F10` [I: check the game's key bindings; GD3D11 uses F11].
  * While open, swallow keyboard and mouse messages. Engine.dll imports USER32 and XINPUT only (no DINPUT), so a WndProc
    filter should suffice [C: verified, raw input = WM_INPUT through the same procedure; docs/research/overlay.md 3].
  * ImGui draws its own cursor.

  Integration into the game's own options menu is optional and later, via spass + the C API.

---

## 5. "Do now": prerequisites the D3D9-parity work should adopt

Ranked by value / cost. None changes the parity output. Each is testable with all features off: screenshots must stay
pixel-identical to the current renderer.

| # | item | why | touches | cost |
|---|---|---|---|---|
| 1 | **Done** (docs/research/frame-phases.md). **Phase markers** (frame / view / pass enum on `d3d9::Device`) from detours: `eCFrustumDatabase::Execute`, `Begin/EndZPass`, `UpdateShadowMask`, `DoBeginRenderSolid/Alpha`, `DoEndRenderAlpha`, `UpdateSkydomeEnvCubemap`, `eCImageFilterScene::OnUpdateOutput`, `eCApplication::Draw` | every feature needs "where are we in the frame"; also great for `-LogFrame` and debugging parity | `src/engine/symbols.h`, `src/bootstrap.cpp`, new `src/engine/scene_hooks.cpp`, `src/d3d9/device.h` | S |
| 2 | **Done** (docs/research/draw-tags.md). **Draw tags + per-frame draw records** (kind, view, context, material, sub-mesh, entity; geometry identity; VS/PS constant ring addresses; textures) | semantics for water / RT / motion; debug overlay ("tint by kind") | `src/hal/vulkan/device.cpp:1002` (Draw), `src/d3d9/draw.cpp:336-433` (keep `vsConstAddr_`, `psConstAddr_`, `db.address` per draw), detours on `eCSubMesh::Render`, `eCRenderContextMesh::Render(op)`, `eCAnimation_PS::Render` | M |
| 3 | **Done** (draw-tags.md 2). **Persistent resource identity**: 64-bit id + generation on `Buffer` / `Texture`, bumped when the `VkBuffer` is replaced | BLAS cache keys, previous-frame data, debug | `src/d3d9/device.h:152` (`Buffer`), `src/d3d9/resources.cpp:401-421` | S |
| 4 | **Done** (draw-tags.md 3). **Shader semantics from CTAB** cached on `d3d9::Shader` (register of `g_vInstanceData`, `g_vViewProjection`, `g_DirLight`, `g_AmbientLight`, `smpRefract`, `smpDepthmap`, `g_SamplerData`; skinned / instanced from the declaration) | camera, world, lights per draw without engine RE | `src/d3d9/device.h:202` (`Shader`), `src/fx/effect_binary.h:118` | S |
| 5 | **Done** (docs/research/injection.md 3; motion / normal outputs, `prevVsConsts` tracking, jitter deferred to their first consumer). **ABI extension slots**. Push constants: add `drawId`, `flags`, `prevVsConsts` (u64), `frame` (u64 → FrameBlock: camera, previous camera, jitter, time, feature params), within 128 bytes. GLSL generator: specialisation-constant hooks for extra MRT outputs (motion, normal), a VS capture variant, and named-constant overrides (GI ambient) | avoids re-translating and re-validating the 2483-shader corpus contract later | `src/shader/abi.h:48-63`, `src/shader/glsl.cpp:768-770, 800-830, 900`, `src/vk/bindless.cpp:59-61`, `src/d3d9/draw.cpp:428` | M |
| 6 | **Done** (injection.md 4: points AfterDepthPrepass / AfterOpaque / AfterRefractionCopy / AfterMainView / BeforeGui, `Device::addPass`, `vk/passes.h`; proof `debug.inject_view`). **Injection points** (pass hooks): `Device::onPhase(Phase, callback(cmd))` executed after `endRendering()`; tiny transient-texture helper with sync2 barriers (no full render graph yet) | water / ocean / RT passes plug in without touching the HAL | `src/d3d9/device.h/.cpp`, `src/d3d9/draw.cpp:13` | S-M |
| 7 | **Done** (frame-phases.md 4). **Role tagging of engine images**: scene HDR RT (first RT of the image-filter scene), refraction copy (`CopyCurrentRTToImage` destination in the main view), depth map (`BeginZPass` target), sky env cube | inputs of water / SSR / RT without guessing | `src/hal/vulkan/resources.cpp:80`, phase hooks | S |
| 8 | **Optional RT capability**: probe AS / ray-query / position-fetch / OMM; enable when present (`rt.allow`); conditional buffer usage flags | buffers allocated once, ready for BLAS; RT toggles at runtime | `src/vk/probe.h/.cpp`, `src/vk/context.h:19-27`, `src/vk/context.cpp:206-320`, `src/d3d9/resources.cpp:405`, `src/vk/frames.cpp:189` | S |
| 9 | **Settings registry + `settings.toml` + capability resolution** (no UI yet) | every feature is born configurable | `src/core/config.h/.cpp` (new `settings.*`) | S |
| 10 | **Done** (frame-phases.md 5; no GPU buffer yet). **Camera / frame block**: current + previous view-projection, inverse, position, far, frame index, time (harvested c0 / c4 / c8 / c11 + `ms_pActiveCamera`) | motion vectors, TAA, temporal RT | `src/d3d9/device.*`, `src/hal/vulkan/device.cpp` | S |

Not now: a full render graph with aliasing (GD3D11 lost devices with aliasing on NVIDIA), GPU-driven rendering, DGC.

---

## 6. Phased roadmap (after M4 / M5 parity)

| milestone | content | verify (measurable) | effort |
|---|---|---|---|
| **M6 Platform** | do-now items 1-10; ImGui overlay with presets, capability report, live toggles; debug view "tint draws by kind / view"; per-frame tag histogram in `rvk.log` | all features off: the reference-compare suite is pixel-identical to the pre-M6 build; tint view at `beach_sun` shows water = refracted, palms = SpeedTree, hero = skinned; overlay opens and closes without input leaking into the game; fps cost < 2 % | 3-4 weeks |
| **M7 Enhanced water** | skip engine water draws; own water pass (SSR + env cube, absorption, depth foam, sun glint, underwater fog); lake / ocean classification by material name + water zones | `beach_sun`, `beach_rain`, `harbour_docks`, `waterfall_jungle` screenshots, engine vs enhanced side by side; runtime toggle with no hitch > 1 frame; GPU time < 1.5 ms at 1080p (GPU timestamps); validation clean | 3-4 weeks |
| **M8 Ocean** | FFT cascades, clipmap ocean bound to ocean zones, shore distance-field bake + cache, Jacobian foam; displacement cap near the player | no seams at the shore and the horizon at the 4 water poses; swim line within the cap while swimming (spass scenario); bake < 2 s per zone and cached; ocean GPU time < 2 ms (High) | 3-5 weeks |
| **M9 RT reflections** | RT tier; RT instance registry + lifetime hooks; BLAS cache (static), VS capture (dynamic), TLAS per frame; ray-query reflections for water, then glossy surfaces; motion vectors + temporal / spatial denoise | reflections of off-screen geometry in `harbour_docks` (ships, piers); no BLAS leaks over a 30-minute session (memory stats); toggle on/off at runtime; graceful "unsupported" on a non-RT GPU (log + greyed UI); GPU time budget per preset | 6-8 weeks |
| **M10 RT GI** | DDGI volumes (ray query), hit shading, ambient / lightmap replacement in the material PS, GTAO fallback for non-RT | interiors (`pub`, monastery poses) and night scenes against baseline: no light leaks at probe-visibility test spots; temporal stability (frame-to-frame luma noise metric of reference-compare.md 6.2 under a frozen time); fps per preset; parity path unchanged | 8-12 weeks |

Order rationale: M6 is required by everything. M7 / M8 need only the floor (compute) and reach every player. M9 builds
the RT infrastructure with a contained consumer (water first). M10 reuses it.

## 7. Open questions (to verify in game, CLAUDE.md rule 4)

* Material name / resource name getter on `eCMaterialResource2` for the water list; which sampler of `g_SamplerData` is
  the albedo map (`eCShaderBase::GetDeclarations` `0x6d29b0`).
* Whether gfx admin `+0x5b8` (the refraction-copy gate) is a quality option; refraction quality vs the depth-map z
  pre-pass (`this[0x1590]`).
* The underwater effect path (overlay image effect?) and its trigger.
* ~~Input API of the game (WM_INPUT?) for the overlay; a free hotkey.~~ Answered: window messages for keys / buttons,
  WM_INPUT raw mouse motion, XInput polled; F10 / F11 are free (docs/research/overlay.md 3).
* Size of `eCRenderContextMesh::Create` / `Invalidate` and of the `Destroy` vtable slot before detouring (ICF / thunk check).
* Water draws per frame at the beach (expected: few, large meshes); whether lakes use different materials than the sea.
