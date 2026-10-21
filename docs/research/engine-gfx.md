# Risen (Enhanced, x64) engine graphics architecture

Research for a pure-Vulkan renderer (dynamic rendering, no D3D9) that replaces the game's D3D9 rendering.
Target: `B:\SteamLibrary\steamapps\common\Risen\bin\Engine.dll` (x64, image base `0x180000000`). **All addresses are RVAs.**
The game install was only read.

Legend: **[C]** confirmed (decompiled / disassembled / file contents), **[I]** inferred.

Working material (outside the repo): `B:\DevDrive\risen-gfx-re\`
* `dx9_layer.txt`: headless Ghidra decompile of the whole DX9 layer (502 functions, RVA `0x73bf00-0x74e500` and `0x284f00-0x287400`)
* `frame.txt`: decompile of the frame, render admins, shader admin, depth map, context admin (74 functions)
* `dx9_calls.txt`: every indirect (vtable) call in the DX9 layer with its receiver expression
* `scan*_out.txt`, `refs_*.txt`: capstone scans (vtable references, globals, IAT users, lock flags)
* `ghidra\DumpRange.java` (decompile by RVA / RVA range / name, no 20-function cap), `py\*.py` (pefile + capstone scanners)

---

## 1. Layering

### 1.1 The abstraction is `eCAPIDevice`, not `eCGfxShared`

`eCGfxShared` is **not** a facade: its 419 exports are only nested **enums and structs** (`eEColorFormat`, `eEBlending`,
`eECmpFunc`, `eSGfxLayersDesc`, `eSGfxDrawCallDesc`, `eSGfxVertexBufDesc`, `eSGfxHLSLShaderSrc`, `eSGfxEffectSource`, `eSGfxViewport`,
`eSGfxStencilData`, ...) plus their ctors/operators. It is the "types" header of the HAL. **[C]**

The real hardware abstraction layer is a set of **pure-virtual interfaces** with exactly one implementation each:

| interface (vtable) | D3D9 implementation (vtable) | virtual slots | wraps |
|---|---|---|---|
| `eCAPIInstance` (`0x9775e8`) | `eCDX9Instance` (`0x977060`) | 7 | `IDirect3D9` (`eCDX9Device::ms_p3DBase`, `0xc62a50`) |
| `eCAPIDevice` (`0x977720`) | `eCDX9Device` (`0x9ea550`) | **213** | `IDirect3DDevice9` at `this+0x10` |
| `eCAPITexture` (`0x977500`) | `eCDX9Texture` (`0x9ea4c0`) | 16 | `IDirect3DTexture9` at `+0x18`, owning `eCDX9Device*` at `+0x10` |
| `eCAPICubeMap` | `eCDX9CubeMap` (`0x9eacb0`) | 11 | `IDirect3DCubeTexture9` |
| `eCAPIVolume` | `eCDX9Volume` (`0x9eafa8`) | 4 | `IDirect3DVolumeTexture9` |
| `eCAPIVertexData` | `eCDX9VertexData` (`0x9eb030`) | 4 (Lock/Unlock/GetDesc/SetStride) | `IDirect3DVertexBuffer9` |
| `eCAPIIndexData` | `eCDX9IndexData` (`0x9eaf80`) | 3 (Lock/Unlock/GetDesc) | `IDirect3DIndexBuffer9` |
| `eCAPIPrimitive` | `eCDX9Primitive` (`0x9758c0`) | 16 (streams, decl/FVF, instancing freq, draw desc, `Execute`) | a draw-call record |
| `eCAPIHLSLShader` | `eCDX9HLSLShader` (`0x9eaea8`) | 25 (Compile, constant table, bytecode, CreateShader, Read/Write) | `ID3DXBuffer` + VS/PS |
| `eCAPIEffect` | `eCDX9Effect` (`0x9ead50`) | 29 (params, techniques, passes, Begin/End) | `ID3DXEffect` |
| `eCAPIFont` | `eCDX9Font` (`0x9eafd8`) | 9 (glyph data, text extents) | `ID3DXFont` |
| `eCAPISprite` | `eCDX9Sprite` (`0x9eae48`) | 10 | `ID3DXSprite` |

Every `eCAPI*` vtable slot is `_purecall` (`0x48f6b0`); the full slot lists with names are reproduced by `py\vt.py`.
`eCAPIDevice`'s 213 virtuals group into:

* **device / modes / reset**: `CheckDeviceFormat`, `ToggleNewMode`, `IsResetNecessary`, `IsResetNeeded`, `IsDeviceNotReset`, `Reset`,
  `SwapScreen` (= Present), `BeginScene`/`EndScene`, `SaveBackbufferToFile/Memory`, gamma ramp
* **resource creation**: `CreateTexture/FromFile/FromMemory`, `CreateCubeMap*`, `CreateVolume`, `CreateImageFromMemory`, `CreateVertexData`,
  `CreateIndexData`, `CreatePrimititve` (sic), `CreateDepthBuffer`, `CreateAuxBuffer`, `CreateFontA`, `CreateSprite`, `CreateEffect`,
  `CreateHLSLShader`, `CreateVertexDeclaration`, `CreateVertexProgram`, `CreatePixelProgram`, `CreateOcclusionQuery`/`CreateQuery`
* **"master" state overrides** (`Set/GetMaster{RenderMode,ShadeMode,CullingMode,DepthTesting,DepthWrite,DepthBias,ZBias,StencilTesting,Blending,AlphaTesting,Fogging}`) on top of the normal setters
* **render state**: `SetRenderMode` (fill), `SetShadeMode`, `SetCullingMode`, `SetDepthTesting`, `SetDepthWrite`, `SetDepthBias`, `SetZBias`,
  `SetStencilTesting`, `SetBlending`, `SetAlphaBlending`, `SetAlphaTesting`, `SetColorWriting`, `SetAlphaWriting`, `SetFog`, `SetScissor`/`EnableScissor`,
  `SetClipPlane*`/`EnableClipping`, `SetColorFactor`, `BeginRenderStateBlock`/`EndRenderStateBlock`
* **samplers / texture stages**: `SetTextureAdressing`, `SetTextureFiltering`, `SetSamplerState`, `SetTextureNULL`, `SetForceLowestMipMapLevel`,
  fixed-function stage ops `SetColorCalc`/`SetAlphaCalc`/`SetResultArgument`/`SetTextureCoordinateIndexAndCalc`/`SetTextureCoordinateTransform`/`SetTextureMatrix`
* **fixed-function T&L**: `SetLighting`, `SetLights`, `SetMaterial`, `Set*MaterialSource`, `SetSpecularEffect`, `SetWorldMatrix`, `SetViewMatrix`,
  `SetViewAndProjectionMatrices`
* **shaders / constants**: `Set/GetVertexProgramRegisterF/I/B`, `Set/GetPixelProgramRegisterF/I/B`, `SetVertexProgram(id, ...)`,
  `SetPixelProgram(id)`, `SetVertexDeclaration`, `AssembleShaderProgram*`, `CompileEffect*`, `PreprocessShader`
* **draw**: `Draw(eSGfxDrawCallDesc)` (DrawPrimitive / DrawIndexedPrimitive), `DrawUP`, `DrawIndexedUP`, `SetVertexDataNULL`, `SetIndexDataNULL`
* **render targets**: `SetRenderTarget(rt, ds)`, `SetRenderTargetAt(rt, index)` (MRT, up to 4), `GetRenderTarget*`, `GetBackBuffer`, `SetViewport`, `ClearViewport`
* **queries**: `CreateOcclusionQuery`, `BeginOcclusionTest`, `EndOcclusionTest`, `GetOcclusionTestInfo`
* **2D/text**: `BeginSpriteList`/`AddSpriteToSpriteList`/`FlushSpriteList`/`EndSpriteList`, `DrawTextA`, `DrawTextViaSpriteList`, `CalcTextRect`
* **D3DX helpers**: `CreateBox/Cylinder/Polygon/Sphere/Teapot/Text/Torus`, `GenerateLODMesh`, `CreateUVAtlas` (editor / lightmapper)
* **profiling callbacks** (`Set*CreateCallback`, `Set*ActCallback`, `SetDrawCallback`, `SetMemoryAlloc/FreeCallback`) feeding `eCGfxGaugeAdmin`

Handles that cross the interface are **opaque**: render-target/depth surfaces and vertex declarations are `void*`, vertex/pixel programs
are `unsigned long` ids, queries `void*`. A replacement implementation defines them freely. **[C]** (signatures)

Above the HAL sit the engine-level "Gfx objects" (`eCGfxImageData`, `eCGfxVertexData`, `eCGfxIndexData`, `eCGfxVisual`, `eCGfxVertexDecl`,
`eCGfxVertexProgram`, `eCGfxPixelProgram`, `eCGfxHLSLShader`, `eCGfxEffect`, `eCGfxCamera`, `eCGfxFog`, `eCGfxLight`, `eCGfxMaterial`,
`eCGfxTextureBlock`), which hold an `eCAPIDevice*` (copied from `eCGfxContextAdmin::ms_pAPIDevice`) and an `eCAPI*` object, and
`eCGfxAdmin` with sub-admins `eCGfxContextAdmin` (`+0x18`), `eCGfxMixerAdmin` (`+0x5e8`, a state/immediate-mode facade used by
Game.dll, holds the device at mixer`+0x10` = admin`+0x5f8`), `eCGfxAssistantAdmin`, `eCGfxGaugeAdmin` (statistics).

### 1.2 Device creation, present parameters, reset

* `eCApplication::CreateGraphicsSubsystem` (`0x4c4330`) -> `eCGfxAdmin::Create(eEAPIType)` (`0x74f0d0`) -> `eCGfxAdmin::InitializeAdmin`
  (`0x74e510`): if `eEAPIType == 3` it `new`s an `eCDX9Instance` into **`eCGfxAdmin::ms_pAPIInstance`** (exported, `0xc62ad8`) and
  calls **`Direct3DCreate9(0x20)`** into `eCDX9Device::ms_p3DBase`; then enumerates adapters and modes (`GetAdapterModeCount(.., D3DFMT_X8R8G8B8=0x16)`). **[C]**
  Other `Direct3DCreate9` call sites: `eCDX9Device::sInit` (`0x73dd90`), `eCDX9Instance::Init` (`0x74b140`), two chunks of `eCGfxAdmin`. **[C]**
* `eCGfxContextAdmin::Create(desc, hwnd, eEContextCreate, windowed?, eEMultiSample, eEDepthFormat)` (`0x507db0`) calls
  `eCAPIInstance::CreateDevice` (devirtualized to `eCDX9Device::sCreateDevice`, `0x73d420`) which writes the result to
  **`eCGfxContextAdmin::ms_pAPIDevice`** (exported, `0xc62750`) - the single global `eCAPIDevice*` (referenced by 136 functions). **[C]**
* `eCDX9Device::sCreateDevice` fills the static `eCDX9Device::ms_PresentationParameters` (`0xc62a60`) **[C]**:
  `BackBufferWidth/Height/Format` from the start parameters, `BackBufferCount = 1`, `EnableAutoDepthStencil = TRUE`,
  `AutoDepthStencilFormat` from the request, `Flags = 0`, `PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE (0x80000000)` or `ONE` when vsync.
  Fullscreen path: `Windowed = FALSE`, refresh rate from the request, `MultiSampleType = 2..16` with `SwapEffect = DISCARD`, else `FLIP`.
  Windowed path: `Windowed = TRUE`, refresh 0, `SwapEffect = COPY` (no MSAA). Then `IDirect3D9::CreateDevice(adapter, D3DDEVTYPE_HAL,
  hFocus, behaviorFlags, &pp, &dev)` and `new eCDX9Device(dev)` (0xe60 bytes).
* `eCDX9Device` ctor (`0x73ddd0`): stores the device at `+0x10`, creates an `ID3DXFont` (`+0x18`), an `ID3DXSprite` (`+0x20`),
  an `ID3DXEffectPool` (`+0x28`), an `eCDX9EffectStateManager` (`+0xa8`, implements `ID3DXEffectStateManager` and routes effect state
  changes into the device's state cache), `GetDeviceCaps`, primes the state cache (`GetCachedStates`), `GetBackBuffer` -> `+0xe40`,
  `GetDepthStencilSurface` -> `+0xe48`, `CreateQuery(D3DQUERYTYPE_EVENT)` -> `+0xe58`. Render-state cache at `+0xb8` (256 dwords),
  dirty bits `+0x8bc`, cached textures/streams/decl/VS/PS at `+0xcc0..+0xdf8`. **[C]**
* **Frame latency limiter** [C]: `SwapScreen` (`0x7407b0`) = `Present(src, dst, hwnd, NULL)` then sets `+0xe50`; the next `BeginScene`
  (`0x740900`) issues the event query and **spins on `GetData` until the GPU finished the previous frame** (max 1 frame in flight).
* **Lost device** [C]: `eCApplication::DoRender` (`0x4c0d40`) asks the device `TestCooperativeLevel`; on `D3DERR_DEVICENOTRESET` it calls
  `eCApplication::Reset` (`0x4c11d0`): `eCModuleAdmin` `ResetResources` (vtable `+0xe8`) -> `eCGfxContextAdmin::ToggleNewMode` (`0x507f30`) ->
  `eCDX9Device::ToggleNewMode` / `Reset` (`0x73f690` / `0x73fde0`: release D3DX font/sprite, occlusion queries, unbind everything,
  `IDirect3DDevice9::Reset`, re-create queries) -> `eCModuleAdmin` `RestoreResources` (`+0xf0`). Each admin (`eCShaderMaterialAdmin`,
  `eCImageFilterAdmin`, `eCRenderSystemAdmin`, `eCImageResource2`, ...) has `ResetResources`/`RestoreResources` for its default-pool objects.

### 1.3 Who touches D3D directly

* **No D3D outside the DX9 layer, except speculative devirtualization.** The build is PGO/LTCG-optimized: many call sites do
  `if (obj->vftable == eCDX9Device::vftable) { inlined eCDX9Device body (raw fields, raw IDirect3DDevice9 calls) } else { virtual call }`. **[C]**
  Code referencing a DX9 vtable for such checks (capstone, `refs_vft.txt`): `eCDX9Device` 106 functions, `eCDX9Primitive` 73, `eCDX9Effect` 33,
  `eCDX9HLSLShader` 16, `eCDX9IndexData` 10, `eCDX9CubeMap` 9, `eCDX9Texture` 8, `eCDX9VertexData` 6, `eCDX9Instance` 5, others 4-5.
  Examples: `DoRender` reads `eCDX9Device+0xe40/+0xe48` (back buffer / depth) directly and calls `IDirect3DDevice9::TestCooperativeLevel`;
  `eCFrustumDatabase::Execute` calls `eCDX9Device::SetCachedRenderState` directly; `eCGfxMixerAdmin::SetMaster*` call the `Activate*` helpers.
  **Consequence:** an object with **its own vtable** sends all of these through the virtual `eCAPI*` path, so the inlined raw-D3D paths
  are dead code for a replacement. Patching `eCDX9Device`'s vtable *in place* would **not** be safe (the inlined fast paths would still run).
* **Direct calls into the DX9 layer from outside** (`scan2_out.txt`): only `eCGfxMixerAdmin::SetMaster{AlphaTesting,Blending,StencilTesting,DepthBias,DepthWrite}`
  and `eCShaderMaterialAdmin::SetCurrentRenderStates` (inline `eCDX9Device::Activate*`, in devirtualized branches), `eCFrustumDatabase::Execute`
  (`SetCachedRenderState`), `eCGfxContextAdmin::Create` (`sCreateDevice`), `eCGfxAdmin` (`sGetAdapterDesc`, `sGetModeDesc`, `ms_p3DBase`),
  and `eCLightmapPreprocessor::PackLightmap` (`0x581340`, editor lightmapper) which constructs `eCDX9Texture/CubeMap/Volume` directly. **[C]**
* `eCDX9Device::GetHandle()` (returns the raw `IDirect3DDevice9*`) has **no direct callers**; RVA `0x8cd20` is an ICF-folded `mov rax,[rcx+0x10]; ret`
  shared with unrelated getters (e.g. `eCRenderContext::GetEntity`). **[C]**
* Other DLLs: none import d3d9; `Game.dll` uses `eCGfxMixerAdmin` (DrawLine/Sphere, master depth/gamma, view/world matrix, viewport),
  `eCGfxAdmin`, `eCGfxImageData`, `eCGfxCamera`, GUI classes; `Importer.dll` (resource compiler) uses `eCShaderDefault`, `eCGfxVisual`,
  `eCGfxMixerAdmin::{Begin,End}Scene/ClearViewport/SetRenderTarget` and `D3DXVec3Normalize`. **[C]**

**Caution for hooking**: many tiny exported getters are COMDAT-folded (e.g. `0x7f8c0` is `eCGfxCamera::GetViewMatrix` *and*
`eCMesh_PS::GetMaterialSwitch` *and* `eCCascadeShadowMap::GetLogSplitFactor`; `0x7faa0` is the empty base for every `eCRenderContext::Render*`).
Never detour a 1-3 instruction export; hook the caller or the data.

---

## 2. D3DX (`d3dx9_39.dll`) imports and users **[C]** (IAT users from capstone)

| purpose | functions | used by |
|---|---|---|
| **HLSL shaders** | `D3DXCompileShader`, `D3DXPreprocessShader`, `D3DXGetShaderConstantTable`, `D3DXGetShaderSize`, `D3DXCreateBuffer`, `D3DXAssembleShader`, `D3DXAssembleShaderFromFileA` | `eCDX9HLSLShader::Compile/CreateConstantTable/CreateShader/Read`, `eCDX9Device::PreprocessShader/AssembleShaderProgram*` |
| **Effects (.fx)** | `D3DXCreateEffect`, `D3DXCreateEffectPool`, `D3DXCreateEffectCompiler` | `eCDX9Effect` ctor, `eCDX9Device` ctor, `CompileEffect/FromFile` |
| **Texture I/O** | `D3DXCreateTextureFromFileExA`, `D3DXCreateTextureFromFileInMemoryEx`, `D3DXCreateCubeTextureFromFileExA`, `D3DXCreateCubeTextureFromFileInMemoryEx`, `D3DXGetImageInfoFromFileA`, `D3DXGetImageInfoFromFileInMemory`, `D3DXFilterTexture`, `D3DXLoadSurfaceFromSurface`, `D3DXComputeNormalMap` | `eCDX9Texture`/`eCDX9CubeMap` ctors, `CreateImageFromMemory`, `ConvertFrom`, `CopySurface`, `GenerateTextureMipMaps` |
| **Saving** | `D3DXSaveTextureToFileInMemory`, `D3DXSaveSurfaceToFileInMemory` | `SaveTextureToFile/Memory`, `SaveBackbufferToFile/Memory` (snapshots) |
| **Text / 2D** | `D3DXCreateFontA`, `D3DXCreateFontW`, `D3DXCreateSprite` | `eCDX9Device` ctor (debug font + sprite), `eCDX9Font`, `eCDX9Sprite` (GUI system fonts rasterize glyphs through `ID3DXFont::GetGlyphData`) |
| **Mesh tools** | `D3DXCreateMesh`, `D3DXCreateMeshFVF`, `D3DXCleanMesh`, `D3DXSimplifyMesh`, `D3DXValidMesh`, `D3DXUVAtlasPartition` | `GenerateLODMesh`, `CreateUVAtlas` (editor / lightmapper) |
| **Debug shapes** | `D3DXCreateBox/Cylinder/Polygon/Sphere/Teapot/TextA/Torus` | `eCDX9Device::Create*` (gizmos, `eCGfxMixerAdmin::DrawCylinder/DrawTorus`) |
| **Math** | `D3DXMatrixOrthoOffCenterLH`, `D3DXGetFVFVertexSize` | `eCSprite::Begin`, `eCGfxMixerAdmin`, VB ctors |

Game texture loading goes through D3DX: an `._ximg` holds a plain **DDS** at offset 265 (risen-spass RE.md), passed to
`eCAPIDevice::CreateImageFromMemory` -> `D3DXGetImageInfoFromFileInMemory` + `D3DXCreateTextureFromFileInMemoryEx`. **[C]**

---

## 3. Shaders

### 3.1 Sources on disk **[C]**

`data\` = `common\*.pak`, `compiled\*.pak` (G3V0 archives, entries partly compressed), `compiled\effects\shadermaterial\` (loose), `extern\videos`, `ini\`.

* **Shader-material HLSL** (loose and in `compiled\effects.pak`): `ge_common.fxh`, `ge_common_ps.fxh`, `ge_common_vs.fxh`, `ge_default.fxh`,
  `ge_default.fx`, `ge_default_ps.fx`, `ge_default_vs.fx`, `ge_default_phong.fx`, `ge_default_leafs.fx`, `ge_default_indicator.fx`,
  `ge_default_preview.fx`, `ge_globals.fx`, `ge_indicator_1_1.fx`, `ge_lightstreaks_2_0.fx`, `ge_particle_2_0.fx`, `ge_skydome_2_0.fx`,
  `VH_VL_OB_V.fx`. Plain functions (no techniques) with **explicit registers**: VS `g_vViewProjection c0`, `g_vInvProjection c4`,
  `g_vInvView c8`, `g_vCamPosAndFar c11`, vegetation/wind `c12-c29`, fog `c30`, instance data `c31..`, shader data `c232..`;
  PS `g_CamPosAndFar c0`, `g_FogColor c3`, ambient `c14-21`, `g_DirLight c22`, point lights `c25..`, samplers `s0` shadow mask,
  `s1` refraction, `s2` depth, `s3-s5` lightmaps, `s9..s15` material samplers. Hook points `custum_FragmentInit/...` are filled by generated code.
* **Image-filter (post-processing) effects** - D3DX effect files with techniques/passes (`compile vs_3_0/ps_3_0`, also `_2_0`, one `ps_1_1`)
  in `common\effects.pak`: `post_bloom.fx`, `post_blur.fx`, `post_desaturate.fx`, `post_distort.fx`, `post_dof.fx`, `post_edgesmooth.fx`,
  `post_hdr.fx` (EyeAdapt, ToneMapFactor), `post_radialBlur.fx`, `post_sky.fx`, `post_SSM.fx` (screen-space shadow mask + SSAO: `BlurSSAO`,
  `UpscaleSSAO`, `DepthToShadowMatrix`, `DepthToVarianceShadowMatrix`), `post_streaks.fx`, `post_wobble.fx`, `overlay_color.fx`,
  `editor_copy.fx`, `pre_lightmap.fx`. The image-effect graphs (`eCImageEffect`: filters, render targets, which .fx/technique) are data in
  `compiled\library.pak` (`compiled_imageeffects`; strings DoF, Bloom, SmallBlur, CalcNearCoc, ...). Default effect `DoF` (ConfigDefault.xml).
* One embedded HLSL effect in Engine.dll: the `eCBenchmark` fill-rate test (`technique FillRateTest ... compile ps_3_0`).

### 3.2 Runtime pipeline **[C]**

* Materials (`._xmat`, `eCShaderDefault` / `eCShaderLeafs` / `eCShaderParticle` / `eCShaderSkyDome` / `eCShaderLightStreaks` with
  `eCShaderEllement*`, `eCColorSrc*`, `eCTexCoordSrc*` nodes) are turned into HLSL **macros + generated expressions** (`XFX_*` defines:
  `XFX_LIGHTMAP`, `XFX_DIRLIGHTSHADOWS`, `XFX_PNTLIGHTCOUNT`, `XFX_INSTANCED`, `XFX_MORPHED`, `XFX_MASKED`, `XFX_ALPHABLEND`, `XFX_FOG`,
  `XFX_PS_DIFFUSE/NORMAL/SPECULAR/...`, strings like `lerp( %s, %s, %s )`) - `eCShaderMaterialAdmin::SetupShaderMacros` (`0x6b8ac0`).
* `eCShaderMaterialAdmin::CompileShaderPoolEllement` (`0x6b8140`) keys every shader by **MD5(source file, entry function, profile, macros)** and
  a second MD5 of the whitespace-stripped **preprocessed** text (`eCAPIDevice::PreprocessShader` -> `D3DXPreprocessShader`), looks it up in the
  **shader pool**, and only on a miss compiles with `eCAPIHLSLShader::Compile` -> `D3DXCompileShader(src, macros, include, entry, profile, flags)`
  (`0x749810`; profile table `0x9eb060`: `ps_1_1..ps_3_0`, `vs_1_1..vs_3_0`).
* **Shader pools**: `Data/Compiled/Materials/ShaderMaterialPool_%s.smp` (`eCShaderMaterialAdmin::ReadShaderPool`, `0x6b6ee0`/`0x6b6880`;
  "Some shaders couldn't be loaded due to newer *.fx file on disk!"). `compiled\materials.pak` ships **`ShaderMaterialPool_Master.smp`
  (~13 MB uncompressed, ~2.6 MB packed)** and an empty `ShaderMaterialPool_User.smp` (22 bytes). So in practice almost all material shaders
  are **precompiled D3D9 SM bytecode** loaded from the Master pool; the HLSL is only compiled for misses. **[C]** (file listing; pool content not decoded)
* Shader model: `ConfigDefault.xml` `<Shader CurrentShaderVersion="3_0">`; versions `1_1`, `1_4`, `2_0`, `3_0` (`eEShaderMaterialVersion`),
  clamped to the device's min/max (`SetCurrentShaderVersion`, `0x6b7970`). The game runs **vs_3_0/ps_3_0**. **[C]**
* Image-filter effects: `eCGfxEffect` -> `eCAPIDevice::CreateEffect/CompileEffect` -> `ID3DXEffect` with the engine's
  `ID3DXEffectStateManager` (states/samplers/constants go through the device cache). Compiled at load. **[C]**/**[I]** (load path not traced)

For Vulkan: either translate the D3D9 SM2/3 token bytecode (`CreateVertexProgram(void* bytecode, ...)`/`CreatePixelProgram`) to SPIR-V
(DXVK `dxso`-style), or recompile the shipped HLSL to SPIR-V (needs legacy-syntax support: `tex2D`, `sampler2D`, `COLOR`, `register(cN)`).
The D3DX effects need an own effect runtime (or a hand port of the ~15 post-process files).

---

## 4. Frame structure

### 4.1 Call graph **[C]**

```
eCApplication::Run (0x4c0490) -> OnRun (0x4c0570)
  Process (0x4c0820)                       game/module update (eCProcessor), kill list
  DoRender (0x4c0d40)                      per frame (also title screen / loading)
    lost device? -> Reset (0x4c11d0)
    camera Update, viewport from client rect
    device->SetRenderTarget(backbuffer, depth)       [eCAPIDevice slot 0x460]
    eCGfxMixerAdmin::ClearViewport(7, ...)
    OnPreRender (vtbl +0x5b8)
    if image effect active (eCImageFilterAdmin +0x90): eCImageEffect::Execute   <- post-process chain; its eCImageFilterScene
                                                                                   (OnUpdateOutput 0x75cf10) calls eCFrustumDatabase::Execute into an RT
    else eCCameraBase::Render(NULL) (0x4f8470)
    overlay effect (eCImageFilterAdmin +0x98): eCImageEffect::Execute
    device->SetRenderTarget(backbuffer, depth)
    OnPostRender (vtbl +0x5b0), eCTimer::Update
  Draw(HWND) (0x4c13b0)
    eCGuiModule2::Draw                     GUI (eCGuiDevice2 / eCSprite / eCDeferredGui*)
    DoSnapshot if requested
    eCGfxMixerAdmin::SwapScreen (0x50fc90) -> eCDX9Device::SwapScreen -> IDirect3DDevice9::Present
```

`eCCameraBase::Render` (`0x4f8470`) = `eCFrustumFilter::UpdateFrameDependencies` -> `eCFrustumDatabase::ExtractFromFrustum` (culling, portals,
`0x66fa60`) -> `PreExecute` (`0x66e790`) -> `Request` (`0x66e8c0`, render contexts emit `eCRenderOp2`s) -> **`eCFrustumDatabase::Execute`** (`0x66e990`).

### 4.2 Passes inside `eCFrustumDatabase::Execute` (`0x66e990`) **[C]**

The database owns sorted `bTValArray<eCRenderOp2>` lists (array header `+0`, data `+8`, count `+0x10`; getters `0x80a70..0x80ae0`):
pre-passes `+0x14d0`, alpha `+0x14e8`, solid `+0x1500`, z-pass `+0x1518`, `+0x1530`, `+0x1548`, debug `+0x1560`, refracted `+0x1578`.

1. `eCCameraBase::ExecuteGfxCamera` (view/projection to the device and `eCGfxCamera::ms_pActiveCamera`), `eCGfxFog::Execute`.
2. Optional caller render target (`eSRenderTargetDesc`), `ClearViewport` (color `0xff000000`, depth, stencil).
3. **Z pre-pass / depth map** (if enabled, `this+0x1590`): set the depth-map RT, clear to white, `eCShaderMaterialAdmin::BeginZPass`,
   z-pass ops sorted (`qsort`) and `RenderZPass` (vtbl `+0x18`), `EndZPass`, restore RT.
4. `eCIlluminationAdmin::UpdateShadowMask` - shadow maps (`eCCascadeShadowMap::RenderShadowMaps` `0x59f200` ->
   `eCRenderSystemAdmin::RenderShadowMap` `0x66ce00` -> `eCFrustumDatabase::ExtractShadowMap/RequestShadowMap/ExecuteShadowMap`;
   2-3 cascades of 512/1024, point/spot `eCShadowMapPointLight/SpotLight`) and the **screen-space shadow mask + SSAO** (`post_SSM.fx`).
5. `BeginMaterial`, `eCModuleAdmin::DoBeginRenderSolid`, **solid** ops sorted by sort id, `Render` (vtbl `+0`).
6. Debug list (bounding boxes, `eCRenderSystemAdmin` debug), `eCDecalAdmin::RenderDecals`, `eCBillboardAdmin::RenderBillboards`, list `+0x1530`.
7. `DoBeginRenderAlpha`; **refracted** ops (`+0x1578`, water, using the refraction copy `smpRefract`); **alpha** ops depth-sorted (`RenderAlpha`, vtbl `+8`);
   `DoEndRenderAlpha`; list `+0x1548`; `EndMaterial`.

Sky (`eCSkydome_PS` / `eCSkyObject` / `eCShaderSkyDome`), particles (`eCParticle_PS` / `eCShaderParticle`), light streaks, strips, SpeedTree
(`eCRenderContextSpeedTree`, `...BB`), vegetation (`eCRenderContextVegetation`, instanced) and skinned actors (`eCRenderContextAnimation2`,
`eCAnimation_HardRenderActor2`) are **render contexts that emit ops into these lists** (+ module hooks). **[I]** (from class names and op dispatch)

Post-processing (bloom, HDR/eye adaptation, DoF, radial blur, wobble, distort, desaturate, edge smoothing) runs **around** the scene: the active
`eCImageEffect` renders the scene into an `eCImageFilterAdmin::AllocRenderTarget(eEColorFormat, size)` target (`0x663950`, pooled
`eCGfxImageData`), runs its filters, and outputs to the back buffer; GUI is drawn afterwards in `eCApplication::Draw`. Present is in
`eCDX9Device::SwapScreen` (`0x7407b0`).

### 4.3 Hook points

**(a) Low-level: replace the HAL with a Vulkan backend (recommended seam)**
* Install: replace `eCGfxAdmin::InitializeAdmin` (`0x74e510`) so that it puts **your `eCAPIInstance`** into `eCGfxAdmin::ms_pAPIInstance`
  (`0xc62ad8`) and skips `Direct3DCreate9`; `eCGfxContextAdmin::Create` then calls your `CreateDevice` (vtable slot 6) and stores your
  `eCAPIDevice` in `ms_pAPIDevice` (`0xc62750`). (Alternatively hook `eCDX9Device::sCreateDevice` `0x73d420` / `eCDX9Instance::CreateDevice` `0x74b2b0`.)
  Keep `eCGfxAdmin+0x648` (API type) non-zero (validity check), and also route `eCGfxAdmin::sGetAdapterCount/sEnumAdapterDesc/Destroy`
  (they use `ms_p3DBase` only in the devirtualized branch).
* Implement `eCAPIDevice` (213 slots; many are editor/debug-only and can be stubs: D3DX shapes, UV atlas, LOD mesh, save-to-file, profiling
  callbacks), `eCAPITexture/CubeMap/Volume/VertexData/IndexData/Primitive/HLSLShader/Effect/Font/Sprite`. Own vtables => every
  speculatively devirtualized fast path falls back to your virtuals.
* Must-solve pieces: SM2/3 bytecode -> SPIR-V (constants are a flat `c0..c255` register file per stage set via `Set*ProgramRegisterF/I/B`),
  D3D9 render-state model -> Vulkan dynamic state / pipeline cache (`VK_EXT_extended_dynamic_state*`), vertex declarations
  (D3DVERTEXELEMENT9 + FVF for UP/sprite paths) -> vertex input (dynamic `VK_EXT_vertex_input_dynamic_state`), instancing via
  `SetStreamSourceFreq`, `Lock` with DISCARD/NOOVERWRITE on dynamic buffers, `DrawUP/DrawIndexedUP` (ring buffer), D3DX effect runtime,
  DDS loading, glyph rasterization for `eCAPIFont::GetGlyphData`, `StretchRect`/`GetRenderTargetData`, event/occlusion queries,
  fixed-function stage states (used by GUI/sprite/debug paths **[I]**).
* Keep the engine's `eCDX9EffectStateManager` semantics in mind: effects set states through the device cache.

**(b) High-level: scene extraction**
* Frame: `eCApplication::DoRender` (`0x4c0d40`) start, `eCApplication::Draw` (`0x4c13b0`) GUI + present (risen-spass already hooks `DoRender` for `ui.frame`).
* Per view: `eCFrustumDatabase::Execute` (`0x66e990`, also called for scene RTs by `eCImageFilterScene::OnUpdateOutput` `0x75cf10`, for
  shadow maps via `ExecuteShadowMap` `0x66f630`) - read the sorted `eCRenderOp2` lists (each op: `eCRenderContext*` + `eCMaterialResource2*`
  + sort id/fade; `eCRenderOp2::Create(mat, ctx, ...)` `0x67f680`). Contexts: `eCRenderContextMesh` (`eCMesh_PS` -> `eCMeshResource2` ->
  `eCSubMesh` with D3D9 vertex decl, VB/IB, material name), `eCRenderContextAnimation2` (skinned), SpeedTree, vegetation.
* Camera: `eCGfxCamera::ms_pActiveCamera` (`0xc62580`) / `ms_pMasterCamera` (`0xc62590`) (view, projection, view-projection, inverse view
  matrices), `eCShaderMaterialAdmin::SetCamera` (`0x6ba1d0`), `eCApplication::GetCurrentCamera`.
* Lights/shadows: `eCIlluminationAdmin`, `eCDirectionalLight_PS`, `eCPointLight_PS`, `eCStaticLight_PS`, `eCCascadeShadowMap` (split
  distances, light direction), lightmaps per mesh (`LightmapName`). Fog: `eCGfxFog`, `eCWeatherAdmin`.
* Textures: `eCImageResource2` / `eCGfxImageData` (`OnRead` `0x753fb0`).

---

## 5. Resource formats and usage

* **Textures** [C]: `eEColorFormat` = D3DFMT values: `A8R8G8B8`, `X8R8G8B8`, `A1R5G5B5`, `X1R5G5B5`, `A4R4G4B4`, `DXT1..DXT5`, `R16F`, `G16R16F`,
  `A16B16G16R16F`, `R32F`, `G32R32F`, `A32B32G32R32F`; shipped images: DDS DXT1/3/5, a few A8R8G8B8, L8, A8. `eSGfxLayersDesc`:
  `+0 width, +4 height, +0xc flag, +0x10 levels, +0x14 usage (1 RENDERTARGET, 2 DEPTHSTENCIL), +0x18 format, +0x1c pool`
  (`eCDX9Texture` ctor `0x74c380` -> `CreateTexture(w, h, levels, usage, fmt, pool)`). No `CreateRenderTarget`: all RTs are render-target textures.
* **Depth / shadow maps** [C] (`eCDepthMap::SetupImageDesc`, `0x59fb20`; `eEDepthMapFormat` 0..8): `A8R8G8B8`, `R16F`, `R32F`,
  **`D16` and `D24S8` as depth-stencil textures (NVIDIA hardware PCF)**, **FOURCC `DF16` / `DF24` (ATI fetch4 depth)**, `G16R16F` / `G32R32F`
  (variance shadow maps). Color-format shadow maps get an extra `CreateDepthBuffer` (`D24S8`, fallback `D24X8`). Format support probed with
  `CheckDeviceFormat` (`eCDepthMap::IsFormatSupported`, `0x59f810`). No `INTZ`/`RAWZ`/`RESZ`/`ATOC`; **correction (M1 trace): a `NULL`
  FOURCC colour target and a `DF24` depth texture are created at 1024x1024** (docs/research/hal.md 5). Scene depth for SSM/DoF comes from the z pre-pass
  into an `R32F`-style depth map (`eEIFTextureMode_DepthBuffer`) **[I]**.
* **How the shadow-map format is chosen** [C, decompiled + verified in game 2026-10-21; dumps `B:\DevDrive\risen-ghidra\rvk_shadow_dump*.txt`]:
  * `eEDepthMapFormat` -> D3DFORMAT (`SetupImageDesc` `0x59fb20`): 0 `A8R8G8B8`, 1 `R16F`, 2 `R32F`, 3 `D16` (usage 2), 4 `D24S8` (usage 2),
    5 `DF16` (usage 2), 6 `DF24` (usage 2), 7 `G16R16F`, 8 `G32R32F`; mips 1, usage 1 (render target) unless noted.
  * `eCShaderMaterialAdmin::PostInitialize` (`0x6ba9d0`) probes the table at Engine.dll `0x9d2290` (formats) / `0x9d22b0` (enum values):
    `R32F`=2, `R16F`=1, `D16`=3, `D24X8`=4, `DF16`=5, `DF24`=6, 512x512, usage 2 for 3..6 else 1, and keeps the **last** supported entry:
    probe with `cubeMap = 0` -> `+0x178` directional (`GetDirectionalLightShadowMapFormat` `0x6b9f80`), with `cubeMap = 1` -> `+0x180`
    point lights (`GetPointLightShadowMapFormat` `0x6b6eb0`); default 9 (none). `+0x17c` variance format = 8 (`G32R32F`, usage `0x401`)
    or 9 if unsupported. **Order matters: without depth textures the last hit is `R16F`, not `R32F`.**
  * `eCDepthMap::SetFormat` (`0x59f8d0`) walks down from the requested enum value to the first supported one (0 is never probed).
    `eCCascadeShadowMap::SetSplitCount` (`0x59f650`) creates the splits with the directional format; `eCIlluminationAdmin::ApplyConfiguration`
    (`0x577a50`) creates `eCShadowMapDirectionalLight2VS` with format 8 (the 1024 `G32R32F` VSM). `eCDepthMap::CreateRenderTarget` (`0x59fd00`)
    adds a `D24S8`/`D24X8` depth buffer for formats 0..2 and 7..8. `BeginZPass` (`0x6b9720`) turns colour writes off for formats 3..6.
  * The shadow pass pixel shader is `oC0 = texcoord2.z` (light-space z, `ps30_30b5b90954fbc60b`): for colour formats the map stores z
    without any bias; for depth formats the rasterised depth is used. No `SetDepthBias` / `SetMasterDepthBias` calls in a frame of the beach.
  * `post_SSM.fx` (`SSM`, `SSM_Low`, `PSSM`, `DSSM`, `VSSM`) samples `ShadowTexture` with point filtering and compares `.x` itself
    (8 `texldp` taps, `cmp`): no hardware compare, no fetch4. `DF24` therefore needs plain depth sampling.
  * System D3D9 on the AMD test machine (`IDirect3D9::CheckDeviceFormat`, X8R8G8B8): `D16`/`D24X8`/`D24S8`/`DF16`/`DF24`/`INTZ` as 2D
    textures with usage 2 yes, as cube maps no; `R16F`/`R32F`/`G16R16F`/`G32R32F` yes (2D and cube). So the reference uses `DF24` for the
    sun cascades (3 x 1024, `NULL` colour target) and `R16F` cube maps for point lights.
  * Vulkan HAL: `DF24` = sampled `D32_SFLOAT`, `DF16` = `D16_UNORM` (2D only, like D3D9); `D16`/`D24` textures and `INTZ` stay unsupported
    (they would need NVIDIA's implicit compare on `tex2Dproj`). Before 2026-10-21 all depth textures were refused and the sun got `R16F`
    colour maps: 10-bit mantissa depth -> diagonal acne stripes and gaps (docs/images/shadows-r16f-vs-df24.png).
* Back buffer: `X8R8G8B8` modes, auto depth-stencil (format from `eEDepthFormat`), optional MSAA 2..16 in fullscreen (ConfigDefault `MSAA="0"`).
* **Vertex formats** [C]: meshes carry a D3D9 `D3DVERTEXELEMENT9` declaration (static 52-byte vertex: position, normal, tangent, 2 colors, UV;
  variants 28/60/68 bytes; a stream-3 FLOAT1 element; FLOAT4 billboards), indices 16 or 32 bit. Hardware instancing via
  `SetStreamSourceFreq` + `eCShaderMaterialAdmin::GetHardwareInstancingVertexData` (`XFX_INSTANCED`). FVF is used by `DrawUP`, sprites and
  D3DX shapes (`D3DXGetFVFVertexSize`).
* **Pools / locking**: `CreateVertexBuffer/IndexBuffer(length, usage, fvf/format, pool)` straight from `eSGfx*BufDesc`. Dynamic buffers with
  `D3DLOCK_DISCARD` (`0x2000`): billboards, decals, sprite meshes, particles (`eCParticle_PS::CreateVertices`), strips, image filters;
  `D3DLOCK_NOOVERWRITE` (`0x1000`): `eCSpriteMesh::LockAppend`, `eCVegetationAdmin::LockVertexStream`. Textures are locked via
  `eCGfxImageData::LockRect/LockLayer` (`eCDX9Texture::LockRect` -> `IDirect3DTexture9::LockRect`). **[C]** (lock-flag scan is heuristic)
  Pool values come from `eEPoolType` (6 values); which resources are MANAGED vs DEFAULT was not traced. **[I]**

---

## 6. Size of the D3D9 surface actually used

Distinct **`IDirect3DDevice9` methods called** (decompiled DX9 layer, `dx9_calls.txt`) **[C]**: **74 of 119**:

`TestCooperativeLevel, GetDeviceCaps, Reset, Present, GetBackBuffer, SetGammaRamp, GetGammaRamp, CreateTexture, CreateVolumeTexture,
CreateCubeTexture, CreateVertexBuffer, CreateIndexBuffer, CreateDepthStencilSurface, GetRenderTargetData, StretchRect, SetRenderTarget,
GetRenderTarget, SetDepthStencilSurface, GetDepthStencilSurface, BeginScene, EndScene, Clear, SetTransform, GetTransform, SetViewport,
GetViewport, SetMaterial, SetLight, LightEnable, SetClipPlane, GetClipPlane, SetRenderState, GetRenderState, GetTexture, SetTexture,
SetTextureStageState, GetSamplerState, SetSamplerState, SetScissorRect, GetScissorRect, SetNPatchMode, DrawPrimitive, DrawIndexedPrimitive,
DrawPrimitiveUP, DrawIndexedPrimitiveUP, CreateVertexDeclaration, SetVertexDeclaration, GetVertexDeclaration, SetFVF, CreateVertexShader,
SetVertexShader, GetVertexShader, SetVertexShaderConstantF/I/B (+Get), SetStreamSource, GetStreamSource, SetStreamSourceFreq,
GetStreamSourceFreq, SetIndices, GetIndices, CreatePixelShader, SetPixelShader, GetPixelShader, SetPixelShaderConstantF/I/B (+Get),
CreateQuery, Release`.

About 20 are getters (state-cache priming, `GetCachedStates`), ~10 are fixed-function (transform/material/light/TSS/FVF/NPatch, partly only
reached via the D3DX effect state manager), leaving ~45 "real" ones. Plus `IDirect3D9`: `CreateDevice, GetAdapterCount, GetAdapterIdentifier,
GetAdapterModeCount, EnumAdapterModes, GetAdapterDisplayMode, CheckDeviceFormat, GetDeviceCaps`; resource interfaces: texture/cube/volume
`LockRect/UnlockRect/LockBox/GetSurfaceLevel/GetCubeMapSurface/GetLevelDesc`, VB/IB `Lock/Unlock/GetDesc`, query `Issue/GetData`.

**But** the engine also relies on D3DX objects that call the d3d9 COM API internally with their own (larger, undocumented) usage:
`ID3DXEffect` + effect pool + state manager (all post-processing), `ID3DXFont` (GUI system font glyphs), `ID3DXSprite`, D3DX texture
loaders/filters/savers, `ID3DXMesh` (tools). A COM-level D3D9->Vulkan implementation therefore has to either be complete enough for
d3dx9_39's internals (state blocks, `ProcessVertices`-free but still broad) or replace D3DX as well - i.e. it degenerates into DXVK-sized work.

**Conclusion:** the engine's own HAL (`eCAPIInstance` / `eCAPIDevice` + 10 resource interfaces, all pure virtual, single global device pointer,
opaque handles, devirtualization-safe when using new vtables) is the natural replacement layer. Implementing it on Vulkan avoids d3d9.dll and
d3dx9 at runtime; the hard parts are the same under either approach (SM3 shader translation or HLSL recompilation, D3DX-effect replacement for
~15 post-process .fx files, D3D9 state -> Vulkan pipeline mapping), but at the HAL level there is no need to emulate D3DX or COM semantics.

## 7. Open questions / next steps
* Decode `ShaderMaterialPool_Master.smp` (pak entry compression, record layout: MD5 ids -> bytecode) to count shaders and profiles.
* Trace which `eCAPIDevice` slots are hit at runtime (counting proxy over the vtable) to prune the 213-slot surface; confirm fixed-function use
  (GUI `eCGuiDevice2`, `eCSprite`, debug draw).
* Trace image-effect graphs in `compiled_imageeffects` (RT formats, e.g. `A16B16G16R16F` for HDR) and `eCImageFilterScene`.
* Map `eEPoolType` and the pool used for each resource class; confirm MSAA/windowed flags of `eSAPIStartParameters`.
