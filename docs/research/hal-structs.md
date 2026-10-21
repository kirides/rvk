# HAL structs, enums and the eCDX9* semantics a Vulkan HAL must reproduce

What the engine's only HAL implementation (`eCDX9*` in Engine.dll) does with every argument of the slots the game uses
(live trace, `hal.md` section 6), plus the layouts of the structs/enums that cross the interface. The C++ definitions are in
`src/hal/types.h` (static_asserts for every confirmed size/offset), checked against the Windows SDK's d3d9 headers by
`tests/hal_types_tests.cpp`.

Evidence: headless-Ghidra decompile of the DX9 layer (`B:\DevDrive\risen-gfx-re\dx9_layer.txt`) and of callers
(`frame.txt`, `structs_dump1..3.txt` in the same folder: `eCApplication::CreateGraphicsSubsystem`, `eCGfxMixerAdmin::SetMaster*`,
`eCGfxImageData::*`, `eCGfxGaugeAdmin::GetNameFromFormat`, `eCImageResource2::StaticConstructor`). All addresses are Engine.dll
RVAs; D3D9 vtable slots were resolved by index (`py/d3dnames.py`). **[C]** confirmed by code, **[I]** inferred.

## 0. The big picture (read this first)

* **Every enum is a D3D9 value passed through unchanged** [C]. `eEBlending` *is* `D3DBLEND`, `eECmpFunc` *is* `D3DCMPFUNC`,
  `eETexFilter` *is* `D3DTEXTUREFILTERTYPE`, formats are raw `D3DFORMAT` (incl. FOURCCs), pools `D3DPOOL`, lock flags
  `D3DLOCK_*`, usages `D3DUSAGE_*`, clear flags `D3DCLEAR_*`, `eECopyFilter` is `D3DX_FILTER_*`. The only engine-specific
  encodings are the on/off enums (`eEWrite`, `eEStencil`, `eEAlphaTest`: **2 = enabled**, anything else = disabled) and the
  `eEMaster*` variants (**0 = no master**, otherwise the base enum's value).
* **All render states go through one cached setter**, `eCDX9Device::SetCachedRenderState` (`0x286300`): cache at device
  `+0xb8 + 4*D3DRS` (256 dwords), skip if equal. Samplers: `SetCachedSamplerState` (`0x2863b0`), cache `+0x93c + 4*(14*stage
  + D3DSAMP)`, 16 stages. Textures `+0xcc0 + 8*stage` (16), streams `+0xd68` (VB), `+0xda8` (stride), `+0xdc8` (offset),
  `+0xd48` (frequency) (8 each), indices `+0xd40`, vertex decl `+0xde8`, VS `+0xdf0`, PS `+0xdf8`. A Vulkan device needs
  the same "current D3D9 state" model; the caches themselves are invisible to the engine (it only reads them in
  devirtualized code, which is dead for an object with its own vtable).
* **Master states** (section 3.4) are a per-state override flag; **effects** bypass it and their render-state changes are
  **undone at EndTechnique** (render-state block, section 3.5).
* **Handles** (`void*`) are raw D3D9 COM pointers or small integers: render-target/depth handles = `IDirect3DSurface9*`
  (borrowed, never owned by the caller), vertex declaration = `IDirect3DVertexDeclaration9*`, programs = the shader pointer
  truncated to 32 bit, occlusion queries = 1-based index, effect parameters/techniques = `D3DXHANDLE`, constant tables =
  `ID3DXConstantTable*`, aux buffers = `ID3DXBuffer*`. A replacement defines them freely but must keep **pointer identity
  stable** (the engine compares and caches them).
* Return values: `1` ok, `0` fail; file-reading slots also return `4` (open failed) and `10` (file not found).

## 1. eCAPIInstance (`eCDX9Instance`, 0x18 bytes, `+0x10` = eEAPIType)

| slot | DX9 behaviour |
|---|---|
| `Init(eEAPIType)` `0x74b140` | stores the type; only `3` (DX9) succeeds, then `Direct3DCreate9(D3D_SDK_VERSION=0x20)` once into `eCDX9Device::ms_p3DBase`. Note `eCGfxAdmin::InitializeAdmin` (`0x74e510`) creates the instance itself and, for a foreign vtable, calls slot 0 (`Init`); for `eCDX9Instance` it inlines it. |
| `GetAdapterCount(ulong*)` `0x74b1c0` | `IDirect3D9::GetAdapterCount`. |
| `GetModeCount(adapter, ulong*)` `0x74b210` | `GetAdapterModeCount(adapter, D3DFMT_X8R8G8B8)`: **only X8R8G8B8 modes** exist for the engine. |
| `GetAdapterDesc(adapter, eSGfxAdapterDesc*)` `0x74b260` -> `sGetAdapterDesc` `0x73d7e0` | `GetAdapterIdentifier(adapter, 0)` -> Driver/Description/DriverVersion/VendorId/DeviceId/SubSysId/Revision; `GetDeviceCaps(adapter, HAL)` -> 0x128 bytes of `D3DCAPS9` from offset 8 to desc `+0x418`; `+0x540` = video memory in MB read through **DxDiag** (`IDxDiagProvider`, `DxDiag_DisplayDevices`, matching `iAdapter`, `szDisplayMemoryLocalized`). The caller zeroes the 0x548 bytes first. |
| `GetModeDesc(adapter, mode, eSGfxModeDesc*)` `0x74b280` -> `sGetModeDesc` `0x73d6f0` | `EnumAdapterModes(adapter, X8R8G8B8, mode)`: writes width `+0`, height `+4`, refresh `+8`, format `+0x14`; leaves `+0xc`/`+0x10` (the engine fills them later). |
| `CreateDevice(adapter, eSAPIStartParameters const&, eCAPIDevice**)` `0x74b2b0` -> `sCreateDevice` `0x73d420` | see 1.1 |

The engine then builds one `eSGfxContextDesc` (0x570: `+0` adapter, `+8` adapter desc, `+0x550` mode index, `+0x554` mode
desc) per (adapter, mode) and `eCApplication::CreateGraphicsSubsystem` (`0x4c4330`) picks the one whose adapter, width,
height (exact when windowed, `<=` when fullscreen) and refresh match the setup and whose **format is 22 (X8R8G8B8)**; if
none matches it raises "can't find correct context in gfx-system". **A Vulkan instance must therefore report at least one
X8R8G8B8 mode with the window's exact client size** (e.g. enumerate the monitor modes and add the window size).

What the engine reads from the caps [C]: `eCShaderMaterialAdmin::PostInitialize` (`0x6ba9d0`) takes the material shader
model from `PixelShaderVersion` (major byte; <4 and 0 -> 1, >3 -> 3) and `PS20Caps.Caps & D3DPS20CAPS_PREDICATION`.
Fill `D3DCaps9Tail` like a real SM3 driver (VertexShaderVersion `0xFFFE0300`, PixelShaderVersion `0xFFFF0300`,
MaxVertexShaderConst 256, NumSimultaneousRTs 4, MaxSimultaneousTextures 8, MaxTextureWidth/Height 8192+, MaxAnisotropy 16,
MaxStreams 16, PS20Caps/VS20Caps max, ...). Best: capture the real caps once in passthrough/trace mode and replay them.

### 1.1 sCreateDevice: D3DPRESENT_PARAMETERS from eSAPIStartParameters [C]

The start parameters are built on the stack by `eCGfxContextAdmin::Create` (`0x507db0`): window = game HWND,
behaviorFlags = `0x40` (`D3DCREATE_HARDWARE_VERTEXPROCESSING`) `+2` (`FPU_PRESERVE`) when requested, windowed flag,
multiSample = `1 << config` (default 0), mode width/height/refresh/format from the context desc, depth format **D24S8 (75)**,
vsync = mode desc `+0xc`, `+0x2c` = mode desc `+0x10` (unused).

| D3DPRESENT_PARAMETERS | value |
|---|---|
| BackBufferWidth/Height | `+0x14`/`+0x18` |
| BackBufferFormat | `+0x20` (X8R8G8B8) |
| BackBufferCount | 1 |
| MultiSampleType | fullscreen and `+0x10` in 2..16: that value, SwapEffect DISCARD; fullscreen without MSAA: NONE, SwapEffect FLIP; **windowed: NONE, SwapEffect COPY** |
| MultiSampleQuality | 0 |
| hDeviceWindow | `+0` |
| Windowed | `+0xc` |
| EnableAutoDepthStencil / AutoDepthStencilFormat | TRUE / `+0x24` |
| Flags | 0 (back buffer not lockable) |
| FullScreen_RefreshRateInHz | fullscreen: `+0x1c`, windowed: 0 |
| PresentationInterval | `+0x28` (vsync) == 1 ? ONE : IMMEDIATE |

`IDirect3D9::CreateDevice(adapter, D3DDEVTYPE_HAL, hFocusWindow = +0, BehaviorFlags = +8, &pp)`, then `new eCDX9Device`
(0xe60 bytes). The device ctor (`0x73ddd0`) creates a debug `ID3DXFont` (`+0x18`), `ID3DXSprite` (`+0x20`),
`ID3DXEffectPool` (`+0x28`), the effect state manager (`+0xa8`), primes the state caches from the device, caches the
back buffer (`+0xe40`) and the auto depth surface (`+0xe48`) **without holding a reference**, and creates an event query
(`+0xe58`). Vulkan: create the swapchain for `window`, size = back buffer size, FIFO when vsync else MAILBOX/IMMEDIATE,
an auto depth image (D24S8 -> `D24_UNORM_S8_UINT`, fallback `D32_SFLOAT_S8_UINT`), MSAA only fullscreen (ignore windowed).

## 2. eCAPIDevice: device, modes, frame

| slot | DX9 behaviour [C] |
|---|---|
| `ToggleNewMode(eSAPIModeParameters const&)` `0x73f690` | fills the same D3DPRESENT_PARAMETERS from the mode parameters (`+0` windowed, `+4` MSAA, `+8`/`+0xc` size, `+0x10` refresh, `+0x14` format, `+0x18` depth, `+0x1c` vsync), releases D3DX font/sprite resources, all occlusion queries and the event query, unbinds indices, 8 streams, 16 textures, VS, PS, then `IDirect3DDevice9::Reset`, re-reads the state caches, recreates queries/fonts/sprites, re-caches back buffer and depth surface. Called by `eCGfxContextAdmin::ToggleNewMode` (`0x507f30`) from `eCApplication::Reset` (`0x4c11d0`) between the modules' `ResetResources` and `RestoreResources`. Vulkan: recreate swapchain + auto depth at the new size; default-pool resources are recreated by the engine itself. |
| `Reset()` `0x73fde0` | same as ToggleNewMode with the stored parameters. |
| `IsDeviceNotReset(bool*)` `0x73fda0` | `TestCooperativeLevel() == D3DERR_DEVICENOTRESET`. **Called every frame by DoRender**; Vulkan: always `false` unless a swapchain recreation needs the engine's reset path (then return true once; the engine calls ToggleNewMode). |
| `IsResetNeeded(bool*)` `0x73fd60` | `TestCooperativeLevel() != D3D_OK`. Vulkan: false. |
| `IsResetNecessary(bool*)` `0x73fd10` | `hr == D3D_OK || hr == D3DERR_DEVICENOTRESET` (sic: true when the device is fine). Vulkan: true. |
| `CheckDeviceFormat(eSGfxLayersDesc const&, bool blend)` `0x73f5b0` | `IDirect3D9::CheckDeviceFormat(adapter, devtype, display format, usage = desc.usage \| (blend ? D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING : 0), rtype, desc.format)`, rtype = VOLUMETEXTURE if `depth > 1`, else CUBETEXTURE if `cubeMap`, else TEXTURE; returns 1 only for `D3D_OK`. Callers: `eCDepthMap::IsFormatSupported` (`0x59f810`, shadow-map formats of `eCDepthMap::SetupImageDesc` `0x59fb20`: A8R8G8B8, R16F, R32F, D16/D24S8 as usage-2 textures, FOURCC `DF16`/`DF24` usage 2, G16R16F, G32R32F), `eCShaderMaterialAdmin::PostInitialize` (512x512 RT/depth probes, cube probes). The answers select the shadow format: the last supported of R32F, R16F, D16, D24X8, DF16, DF24 wins (engine-gfx.md 5); `post_SSM.fx` compares in the shader (no fetch4, no hardware PCF), so the Vulkan HAL answers "yes" for 2D DF16/DF24 (sampled depth images) and "no" for D16/D24 textures and depth cube maps. |
| `SetViewport(eSGfxViewport const&)` `0x740530` | D3DVIEWPORT9 X=left, Y=top, W=right-left, H=bottom-top, **0 width/height -> 32**, MinZ/MaxZ. Vulkan: negative-height viewport (or flip in the shader) and `minDepth/maxDepth`. |
| `GetViewport(eSGfxViewport&)` `0x740630` | `IDirect3DDevice9::GetViewport` converted back (right = X+W, bottom = Y+H). Must return what D3D9 would: the last SetViewport **or the full RT size after SetRenderTarget** (see 2.2). |
| `ClearViewport(eEViewportClear, color, z, stencil)` `0x740710` | `Clear(0, NULL, flags, color, z, stencil)`: color is D3DCOLOR **ARGB** (`0xff000000` = opaque black). D3D9 semantics: clears the **viewport rectangle** (and the scissor rect if scissor test is enabled) of **all bound color targets**. |
| `SwapScreen(HWND, bCRect const* src, bCRect const* dst)` `0x7407b0` | `Present(src, dst, hDestWindowOverride = hwnd, NULL)`, re-caches the back buffer, sets the "frame presented" flag `+0xe50`. |
| `BeginScene()` `0x740900` | if `+0xe50`: clear it, `eventQuery->Issue(D3DISSUE_END)` and **spin on `GetData(NULL, 0, D3DGETDATA_FLUSH)` until done** = the CPU waits until the GPU finished all work submitted so far (frame latency 1). Then a nesting counter `+0xa0`: only the outermost BeginScene calls D3D. Vulkan: frame pacing point (wait for the previous frame's fence); keep the counter. |
| `EndScene()` `0x7409e0` | outermost only (`+0xa0 == 1`), decrements. |
| `SetGammaRamp(bool noCalibrate, eSGfxGammaRamp const*)` `0x741550` | `IDirect3DDevice9::SetGammaRamp(0, Flags = !noCalibrate (D3DSGR_CALIBRATE), ramp)`. Driven by `eCGfxMixerAdmin::UpdateGammaRamp` (brightness/contrast/gamma options). D3D9 ignores it in windowed mode; Vulkan: apply as a final LUT pass or ignore windowed. `GetGammaRamp` = `IDirect3DDevice9::GetGammaRamp`. |

### 2.1 Resources

| slot | DX9 behaviour [C] |
|---|---|
| `CreatePrimititve(eCAPIPrimitive**)` `0x73e4d0` | `new eCDX9Primitive` (0xa8, no ref count): 4 streams enabled, offset 0, stride 0, frequency 1; no D3D object. |
| `CreateIndexData(eSGfxIndexBufDesc const&, out)` `0x73e5c0` -> ctor `0x74ad80` | `CreateIndexBuffer(length, usage, format, pool)`; the object holds 2 COM refs (released twice in the dtor). |
| `CreateVertexData(eSGfxVertexBufDesc const&, out)` `0x73e650` -> ctor `0x74dcf0` | stride = `D3DXGetFVFVertexSize(fvf)`, or `desc.stride` when `fvf == 0`; `CreateVertexBuffer(length, usage, fvf, pool)`. Object: `+0x18` VB, `+0x20` stride, `+0x24` FVF. |
| `CreateEffect(void const* data, uint size, out)` `0x73e6e0` -> ctor `0x747df0` | `D3DXCreateEffect(dev, data, size, NULL, NULL, 0, effectPool, &fx, NULL)` (data = a compiled fx_2_0 binary from CompileEffectFromFile), AddRef, `fx->SetStateManager(device's eCDX9EffectStateManager)`. |
| `CreateHLSLShader(out)` `0x73ec30` | `new eCDX9HLSLShader` (0x38): empty. |
| `CreateTexture(eSGfxLayersDesc const&, out)` `0x73ecc0` -> ctor `0x74c380` | `CreateTexture(width, height, levels, usage, format, pool)`; desc not written back. |
| `CreateCubeMap` `0x73f210` -> `0x73ce50` | `CreateCubeTexture(width, levels, usage, format, pool)`. |
| `CreateVolume` `0x73f2a0` -> `0x74e120` | `CreateVolumeTexture(width, height, depth, levels, usage, format, pool)`. |
| `CreateImageFromMemory(eEPoolType pool, int levels, uchar skipMips, void const* data, uint size, eSGfxLayersDesc& desc, void*& out)` `0x73f030` | `D3DXGetImageInfoFromFileInMemory`; levels `-1` -> DDS: `D3DX_FROM_FILE (-3)` else 0 (full chain); `-2` -> DDS: from file else 1; for DDS with `skipMips >= mips` skip = mips-1. Writes desc `format` = file format, `pool`, `levels`. **Texture if the file's resource type is TEXTURE, else cube map** (sets `desc.cubeMap = 1`); `out` = `eCAPITexture*` or `eCAPICubeMap*` (the engine knows which from `desc.cubeMap`). Ctor `0x74c650`/`0x73caa0`: `D3DXCreate(Cube)TextureFromFileInMemoryEx(dev, data, size, desc.width, desc.height (cube: width), desc.levels, desc.usage, desc.format, desc.pool, Filter = D3DX_DEFAULT, MipFilter = D3DX_FILTER_BOX \| D3DX_SKIP_DDS_MIP_LEVELS(skip), ColorKey 0)` (width/height 0 = from file; the engine passes its stored desc, normally 0); then writes back `levels` (GetLevelCount), `format`, `width`, `height` of level 0. Game data are DDS (DXT1/3/5, A8R8G8B8, L8, A8; `engine-gfx.md` 5). Vulkan: parse the DDS, honour skip, upload, fill the desc exactly like this. |
| `GetImageInfoFromMemory(data, size, eSGfxImageInfo&)` `0x73f530` | `D3DXGetImageInfoFromFileInMemory` -> the 7 dwords of D3DXIMAGE_INFO. |
| `CreateDepthBuffer(w, h, eEDepthFormat, void** out)` `0x744ad0` | `CreateDepthStencilSurface(w, h, fmt, D3DMULTISAMPLE_NONE, 0, Discard = FALSE)`; `*out = IDirect3DSurface9*` (owned by the caller). `DestroyDepthBuffer(void*)` `0x744bb0` = `Release`. |
| `CreateVertexDeclaration(eSGfxDclElement const*, void** out)` `0x742e20` | `CreateVertexDeclaration(elements)` then an extra `AddRef` (2 refs). `DestroyVertexDeclaration` `0x742ec0` releases until 0. `GetVertexDeclarationData(decl, elements, uint* count)` `0x742f00` = `IDirect3DVertexDeclaration9::GetDeclaration` (count includes D3DDECL_END; elements may be NULL to query the count). |
| `CompileEffectFromFile(eSGfxEffectFileSource const&, void** errors, void** compiled)` `0x743cf0` | reads the file through `eCArchiveManager` (10 = not found, 4 = read error), `D3DXCreateEffectCompiler(text, len, macros, NULL, flags, &compiler, errors)`, `compiler->CompileEffect(0, compiled, errors)` with FP exceptions masked; out = `ID3DXBuffer*` aux buffers. **Vulkan: the shipped .fx are HLSL source; either keep a CPU-side fx compiler (d3dx9/D3DCompiler `fx_2_0`) or precompile them offline and hand out the binaries.** |
| `CompileEffect(eSGfxEffectSource const&, errors, compiled)` `0x743b60` | same with text in memory. |
| `GetAuxBuffer(void* buf, void** data, ulong* size)` `0x744100` / `DestroyAuxBuffer` `0x7440d0` / `CreateAuxBuffer(size, out)` `0x744050` | `ID3DXBuffer` GetBufferPointer/GetBufferSize, Release, `D3DXCreateBuffer`. |
| `PreprocessShader(eSGfxHLSLShaderSrc const&, bCString& out)` `0x73e780` | source = text or file (`fromFile`, archive), `D3DXPreprocessShader(src, len, macros, eCGfxDX9Include, &text, &errors)`, `out = text` (bCString assignment). The engine MD5s the result for the shader-pool lookup (`engine-gfx.md` 3.2), so **the preprocessed text must be byte-identical to D3DX's** or every pool lookup misses. |

### 2.2 Render targets [C]

| slot | DX9 behaviour |
|---|---|
| `SetRenderTarget(void* rt, void* ds)` `0x744dd0` | **first `SetTextureNULL(0..15)`** (via the vtable), then `SetRenderTarget(0, rt)` and `SetDepthStencilSurface(ds)` (`ds == NULL` = no depth); if inside a scene (`+0xa0 != 0`) it calls `IDirect3DDevice9::BeginScene` again. RT1..3 are **not** touched. D3D9 itself resets the **viewport (and scissor rect) to the full size of RT0** on `SetRenderTarget(0, ...)`. |
| `SetRenderTargetAt(void* rt, int index)` `0x744be0` | unbinds the 16 textures, `SetRenderTarget(index, rt)` (MRT, `rt == NULL` unbinds), BeginScene again if in a scene. |
| `GetRenderTarget(void** rt, void** ds)` `0x744ed0` | `GetRenderTarget(0)` / `GetDepthStencilSurface`, both released immediately: **borrowed** pointers. |
| `GetRenderTargetAt(void** rt, int index)` `0x744d80` | same for RT `index`. |
| `GetBackBuffer(void** rt, void** ds)` `0x744f50` | the cached back buffer and auto depth surface (`+0xe40`/`+0xe48`), borrowed. |
| `eCAPITexture::GetRenderTargetHandle(void**)` `0x74cb30` | `GetSurfaceLevel(0)`, released immediately: the handle lives as long as the texture. |
| `eCAPICubeMap::GetRenderTargetHandle(face, void**)` `0x73c790` | `GetCubeMapSurface(face, 0)`, borrowed. |

So a render-target handle is "a 2D view of mip 0 of a texture / cube face, the swapchain image, or a depth buffer". The
same object must come back for the same surface every time. The 16-texture unbind on every RT change is part of the
contract: the engine re-binds what it needs (do the same, it also avoids feedback loops).

### 2.3 Fixed render states, master states, setters [C]

Normal setters (`SetRenderMode` `0x7415d0`, `SetShadeMode` `0x741670`, `SetCullingMode` `0x741710`, `SetDepthTesting`
`0x7417b0`, `SetDepthWrite` `0x741850`, `SetDepthBias` `0x7418f0`, `SetStencilTesting` `0x7419f0`, `SetBlending` `0x742960`,
`SetAlphaTesting` `0x742a50`, `SetFog` `0x7424e0`) **do nothing (and return 1) while the matching master flag is set**.
Otherwise:

| setter | render states |
|---|---|
| SetRenderMode(m) | FILLMODE = m |
| SetShadeMode(m) | SHADEMODE = m |
| SetCullingMode(m) | CULLMODE = m |
| SetDepthTesting(f) | ZFUNC = f (note: **ZENABLE is never touched by the HAL**; it stays as the device default D3DZB_TRUE since an auto depth buffer exists; with `ds == NULL` there is no depth test) |
| SetDepthWrite(w) | ZWRITEENABLE = (w == 2) |
| SetDepthBias(bias, slope) | DEPTHBIAS = bias, SLOPESCALEDEPTHBIAS = slope (floats). Vulkan: `depthBiasConstantFactor = bias * 2^24` for D24 formats (D3D9's bias is in depth units), `depthBiasSlopeFactor = slope` |
| SetZBias | not implemented (returns 0) |
| SetStencilTesting(s, data) | STENCILENABLE = (s == 2); if enabled: STENCILFAIL, ZFAIL, PASS, FUNC, REF, MASK, WRITEMASK = the 7 dwords (one-sided; TWOSIDEDSTENCILMODE never set) |
| SetBlending(src, dst) | SRCBLEND, DESTBLEND (BLENDOP stays ADD; ALPHABLENDENABLE is separate) |
| SetAlphaBlending(b) `0x7411f0` | ALPHABLENDENABLE = b (not master-protected) |
| SetAlphaTesting(a, data) | ALPHATESTENABLE = (a == 2); if enabled ALPHAREF = data.ref, ALPHAFUNC = data.func. Vulkan: shader discard (`src/shader` already emits `rvk_alphaTest`) |
| SetColorWriting(b) `0x7412c0` | COLORWRITEENABLE = (old & 8) \| (b ? 7 : 0) (RGB bits) |
| SetAlphaWriting(b) `0x7413a0` | COLORWRITEENABLE = (old & ~8) \| (b ? 8 : 0) (alpha bit); applies to RT0 only (D3D9 COLORWRITEENABLE1..3 untouched) |
| SetFog(eSGfxFogData&) | ignored when master fog is disabled. FOGENABLE = enable; if enabled: RANGEFOGENABLE, FOGCOLOR, then vertexFog == 1 ? (FOGTABLEMODE = NONE, FOGVERTEXMODE = mode) : (FOGVERTEXMODE = NONE, FOGTABLEMODE = mode); LINEAR: FOGSTART/FOGEND, else FOGDENSITY. Fixed-function fog only affects vs/ps <= 2.x in D3D9; the material shaders (SM3) compute fog themselves (`g_FogColor c3`, `c30`). |
| SetLighting `0x741120`, SetSpecularEffect, Set*MaterialSource, SetColorFactor (TEXTUREFACTOR), EnableClipping (CLIPPING), SetClipPlane(mask) (CLIPPLANEENABLE), EnableScissor (SCISSORTESTENABLE) | one render state each, not master-protected |

### 2.4 Master states [C]

`SetMasterX(v)` (`0x740a90`..`0x741040`, also inlined into `eCGfxMixerAdmin::SetMaster*` `0x50abe0`.. and
`eCShaderMaterialAdmin::SetCurrentRenderStates` `0x6b7c50`):

* `v != 0` (blending: both factors non-zero; depth bias: the bool): set the flag (device `+0x30` render mode, `+0x31`
  shade, `+0x32` cull, `+0x33` depth func, `+0x34` depth write, `+0x35` depth bias, `+0x36` stencil, `+0x37` blending,
  `+0x38` alpha test) **and apply v immediately** through the same render-state writes as the normal setter.
* `v == 0`: clear the flag only. **Nothing is restored**: the forced value stays until the next normal setter call.
* `SetMasterFogging(1)` sets FOGENABLE = 0 and flag `+0x39`; any other value clears the flag. `SetMasterZBias` returns 0.
* `GetMasterX`: 0 if the flag is clear, else the *current* render state re-encoded (`GetMasterDepthWrite` returns
  `(ZWRITEENABLE == 1) + 1`, stencil/alpha test likewise, blending returns SRCBLEND/DESTBLEND, depth func ZFUNC).
  `GetMasterDepthBias` only returns the bool (floats unset when active). `GetMasterFogging` returns the flag.
* Not protected by masters: SetAlphaBlending, SetColorWriting, SetAlphaWriting, scissor, clip planes, samplers, and
  **everything an effect sets** (the effect state manager writes the render-state cache directly).
* `eCGfxMixerAdmin::SetMasterAlphaBlending/ColorWriting/AlphaWriting/Lighting` are plain calls of the non-master slots.

Implementation: keep `masterFlag[9]` beside the D3D9 render-state array; normal setters check the flag, master setters
set flag + state, effect state writes ignore flags.

### 2.5 Render-state blocks and effects [C]

`BeginRenderStateBlock()` `0x747bf0` sets `+0x4b8`. While set, the first change of each render state stores its old value
(`+0x4bc + 4*RS`) and a dirty bit (`+0x8bc`, 256 bits). `EndRenderStateBlock()` `0x747c00` clears the flag and **restores
every dirty render state to the saved value** (only render states: samplers, textures, shaders, constants are not
restored). `eCAPIEffect::BeginTechnique` calls BeginRenderStateBlock, `EndTechnique` calls EndRenderStateBlock, so all
render states an effect pass sets are undone after the technique.

### 2.6 Shaders and constants [C]

| slot | DX9 behaviour |
|---|---|
| `Set{Vertex,Pixel}ProgramRegister{F,I,B}(start, data, count)` `0x743190`.. | `IDirect3DDevice9::Set{Vertex,Pixel}ShaderConstant{F,I,B}(start, data, count)` directly (no cache, no master). Float registers are vec4 (`count` = vec4 count), I = int4, B = one BOOL per register. Register files: VS c0..c255, i0..i15, b0..b15; PS c0..c223, i0..i15, b0..b15. The register file is **global device state, independent of the bound shader** (persists across shader switches). |
| `Get...Register{F,I}` | `Get...ShaderConstant{F,I}`. **`GetVertexProgramRegisterB` calls `SetVertexShaderConstantB`** (engine bug, `0x7433c0`); `GetPixelProgramRegisterB` is correct. |
| `SetVertexProgram(ulong id, bool ff, bool idIsFVF)` `0x743430` | `ff && idIsFVF`: vertex decl cache = NULL, `SetFVF(id)`, unbind VS; `ff && !idIsFVF`: unbind VS (keep decl); `!ff`: `SetFVF(0)` then bind VS `id` through the cache. |
| `SetPixelProgram(ulong id)` `0x743960` | `SetPixelShader(id)` through the cache (`0` = none = fixed-function pixel pipeline). |
| `Create/DestroyVertexProgram`, `Create/DestroyPixelProgram` | `CreateVertexShader/CreatePixelShader(bytecode)` + AddRef, id = pointer truncated to 32 bit; Destroy releases to 0. Not used by the game (`hal.md` 3). |

### 2.7 Draws [C]

| slot | DX9 behaviour |
|---|---|
| `Draw(eSGfxDrawCallDesc const&)` `0x741f40` | `indexed ? DrawIndexedPrimitive(type, baseVertexIndex, minVertexIndex, numVertices, startIndex, primitiveCount) : DrawPrimitive(type, startVertex, primitiveCount)` with whatever streams/indices/decl are bound. |
| `DrawUP(type, primCount, data, stride)` `0x742080` | unbinds stream 0 in the cache, `DrawPrimitiveUP`. Vertex layout = current FVF/declaration. |
| `DrawIndexedUP(type, minIndex, numVertices, primCount, indices, vertices, stride)` `0x742160` | unbinds stream 0 and the index buffer, `DrawIndexedPrimitiveUP(..., D3DFMT_INDEX16, ...)`: **always 16-bit indices**. |
| `SetVertexDeclaration(void*)` `0x742f90` | cached `SetVertexDeclaration`. |
| `SetTextureNULL(stage)` `0x745110` | `SetTexture(stage, NULL)` if bound. `SetIndexDataNULL` / `SetVertexDataNULL(stream)` likewise. |

Primitive counts -> vertex/index counts: POINTLIST n, LINELIST 2n, LINESTRIP n+1, TRIANGLELIST 3n, STRIP/FAN n+2.

### 2.8 Transforms, samplers, misc [C]

| slot | DX9 behaviour |
|---|---|
| `SetViewAndProjectionMatrices(view, proj)` `0x741c00` | `SetTransform(D3DTS_VIEW)`, caches view at `+0xe00`, `SetTransform(D3DTS_PROJECTION)`. |
| `SetViewMatrix` `0x741e30` / `GetViewMatrix` `0x741f00` | VIEW + cache / returns the cache. `SetWorldMatrix` `0x741d10` / `GetWorldMatrix` = `D3DTS_WORLD` (256) Set/GetTransform. Only the fixed-function pipeline uses them; shaders get matrices as constants. Vulkan: store them (GetViewMatrix is called by the game); feed only FF emulation. bCMatrix = D3DMATRIX (row-major memory, row vectors). |
| `SetTextureAdressing(stage, u, v, w)` `0x744170` | sampler ADDRESSU/V/W. |
| `SetTextureFiltering(stage, min, mag, mip)` `0x744300` | **arg order min, mag, mip** -> MINFILTER (6), MAGFILTER (5), MIPFILTER (7). |
| `SetSamplerState(stage, eESamplerState, value)` `0x744540` | `SetSamplerState(stage, state, value)` (D3DSAMP_* 1..13). Stages 0..15 (cache size); D3D9 vertex-texture samplers (257..) are never used [I]. 263k calls per run: make it cheap (dirty bits). |
| `SetScissor(bCRect const&)` `0x740310` / `GetScissor` | `SetScissorRect` / `GetScissorRect`. `EnableScissor(b)` = SCISSORTESTENABLE. `BeginSpriteList` forces SCISSORTESTENABLE = 1 and `EndSpriteList` = 0. |
| `SetClipPlaneData(i, plane)` `0x745030` / `GetClipPlaneData` | `SetClipPlane(i, float[4])` (clip space when shaders are bound). `SetClipPlane(mask)` = CLIPPLANEENABLE. |
| `BeginRenderStateBlock` / `EndRenderStateBlock` | section 2.5. |
| `CreateOcclusionQuery(void*&)` `0x746730` | `CreateQuery(D3DQUERYTYPE_OCCLUSION)`, stored in a list; handle = **1-based index** (free slots reused), max 0xffff. `DestroyOcclusionQuery` releases and frees the index. |
| `BeginOcclusionTest(h)` / `EndOcclusionTest(h)` `0x7469b0`/`0x746a40` | `Issue(D3DISSUE_BEGIN)` / `Issue(D3DISSUE_END)`. |
| `GetOcclusionTestInfo(h, ulong& pixels, bool wait)` `0x746ad0` | `GetData(&pixels, 4, 0)`, if `wait` spin while S_FALSE; returns 1 only when the result is available; invalid handle -> pixels = `0xffffffff`, returns 0. |
| `EnableOptimization`/`DisableOptimization` | static flags only. `SetForceLowestMipMapLevel(b)`: textures bound later get `SetLOD(levels - 1)` (b) or `SetLOD(0)` (managed textures only). |
| `GetMethodSupport` | 20 bools = 1. `GetFVFSize` = `D3DXGetFVFVertexSize`. |
| `Set*Callback` (profiling) | stored only; `ms_pRegDxMemoryAlloc/Free` are never called by the DX9 layer. |

## 3. eCAPITexture / eCAPICubeMap / eCAPIVolume [C]

Objects: `+0x10` eCDX9Device*, `+0x18` the D3D texture (2 refs), texture `+0x20` "lowest mip forced" flag.

| slot | DX9 behaviour |
|---|---|
| `LockRect(level, eSGfxLockedRect*, RECT const*, flags)` `0x74c910` | `IDirect3DTexture9::LockRect(level, (D3DLOCKED_RECT*), rect, flags)`; flags `D3DLOCK_*` (READONLY 0x10, NOSYSLOCK 0x800, DISCARD 0x2000 (dynamic), NO_DIRTY_UPDATE 0x8000, DONOTWAIT 0x4000). Pitch for DXTn = bytes per row of 4x4 blocks. |
| `UnlockRect(level)` `0x74c9d0` | tail-calls `UnlockImageLayer` (`IDirect3DTexture9::UnlockRect(level)`). |
| `LockImageLayer(level, flags, int* pitch, void** bits)` `0x74c9e0` | `LockRect(level, &lr, NULL, flags)`. |
| `SetTexture(stage)` `0x74cbd0` | applies the force-lowest-mip LOD, then cached `SetTexture(stage, tex)`. |
| `GenerateTextureMipMaps(srcLevel, eECopyFilter)` `0x74cef0` | `D3DXFilterTexture(tex, NULL, srcLevel, filter)`: rebuild levels > srcLevel from srcLevel (box/linear per flag). Vulkan: blit chain (or compute). |
| `SaveTextureToFile(path)` `0x74cf80` | extension `.JPG` 1 / `.BMP` 0 / `.TGA` 2 / `.DDS` 4 (else DDS + warning) -> `D3DXSaveTextureToFileInMemory`, written with `eCArchiveManager::CreateFile` (snapshots, 2 calls per run). `SaveTextureToMemory` = DDS into a bCMemoryStream. |
| `CopyCurrentRTToImage(eETexFilter, bCRect* src, bCRect* dst)` `0x74d450` | `StretchRect(current RT0, src, this level 0, dst, filter)` (NULL rect = whole surface). Vulkan: `vkCmdBlitImage` (or a copy when sizes match), NEAREST for POINT/NONE, LINEAR otherwise; must end/restart the current rendering. |
| `CopyImageToCurrentRT(filter, src, dst)` `0x74d610` | `StretchRect(this level 0, src, current RT0, dst, filter)`. |
| `CopySurface(eCAPITexture* src)` `0x74d7d0` | `D3DXLoadSurfaceFromSurface(this level 0, -, -, src level 0, -, -, D3DX_FILTER_NONE, 0)` (format conversion allowed). |
| `LoadSurfaceFromSurface(dstLevel, dstPalette, dstRect, src, srcLevel, srcPalette, srcRect, filter, colorKey)` `0x74d920` | `D3DXLoadSurfaceFromSurface` (CPU conversion/scale between any formats; 2 calls per run). |
| `GetRenderTargetData(eCAPITexture* dst, int level)` `0x74db30` | src = this level `level`, dst = `dst` level 0; if `dst` has usage RENDERTARGET: `StretchRect(src, NULL, dst, NULL, POINT)` else `IDirect3DDevice9::GetRenderTargetData` (GPU -> system-memory readback, sync). |
| `ConvertFrom(eCAPITexture* src, eECopyFilter, bool, int srcLevelOffset)` `0x74cd70` | per level i < min(levels): `D3DXLoadSurfaceFromSurface(this level i, src level i + offset, filter)`; the bool is unused. |
| `ComputeNormalMap(src, eENormalMapGen, eEChannel, amplitude)` `0x74ccd0` | `D3DXComputeNormalMap` (editor/tools). |
| cube `LockRect(face, level, lr, rect, flags)` `0x73c990` | `IDirect3DCubeTexture9::LockRect(face, level, lr, **NULL**, flags)`: the rect argument is ignored. |
| cube `SetCubeMap(stage)` `0x73c5a0` | cached `SetTexture`. `GetRenderTargetData(face, dst, level)` = `GetRenderTargetData(face level, dst level 0)`. |
| volume `LockVolumeBox(level, flags, int* rowPitch, int* slicePitch, void** bits)` `0x74e250` | `LockBox(level, &box, NULL, flags)`. |

## 4. eCAPIVertexData / eCAPIIndexData [C]

* `LockBuffer(uint offset, uint size, ulong flags, uchar** out)` (`0x74deb0` / `0x74af00`) = `Lock(offset, size, out, flags)`;
  size 0 = whole buffer; flags D3DLOCK_DISCARD (0x2000) / NOOVERWRITE (0x1000) on dynamic buffers (62k locks per run).
  Vulkan: DISCARD -> rename (new allocation from a ring), NOOVERWRITE -> write in place, 0 on a buffer in use -> wait or
  copy-on-write.
* `UnlockBuffer()` = `Unlock`.
* `GetDesc` (`0x74dfe0` / `0x74b030`): length, usage, FVF (vertex) / format (index), pool from `GetDesc`; the vertex
  desc's `stride` is **not** written.
* `eCAPIVertexData::SetStride(stride)` `0x74e0b0` sets the object's stride (`+0x20`), read by `eCAPIPrimitive::SetStream`.

## 5. eCAPIPrimitive (eCDX9Primitive, 0xa8 bytes, no ref count) [C]

Layout: `+8` device, `+0x10..+0x28` 4 vertex buffers, `+0x30 + 16*i` per stream {bool enabled, offset, stride, frequency},
`+0x70` index buffer, `+0x78` bool "decl is an FVF", `+0x80` decl pointer / FVF, `+0x88` eSGfxDrawCallDesc.
**Only streams 0..3 exist** (a higher index would corrupt the object).

| slot | behaviour |
|---|---|
| `SetStream(i, eCAPIVertexData*)` `0x74b380` | stores the VB **and copies the vertex data's current stride** (later `SetStride` on the data does not affect it). NULL clears. |
| `SetIndexData(eCAPIIndexData*)` | stores the IB (NULL allowed). |
| `SetVertexDecl(void*)` / `SetFVF(ulong)` | decl or FVF mode. |
| `Set/GetStreamSrcFrequency(i, f)` | frequency = D3D9 `SetStreamSourceFreq` value: `D3DSTREAMSOURCE_INDEXEDDATA (0x40000000) \| instanceCount` on the geometry stream, `D3DSTREAMSOURCE_INSTANCEDATA (0x80000000) \| 1` on instance streams, 1 = no instancing (66k calls: vegetation/instanced materials). |
| `Set/GetStreamOffset`, `Set/GetStreamStride`, `EnableStream`, `IsStreamEnabled` | per-stream fields. |
| `SetDrawCallDescription(desc)` / `GetDrawCallDescription` (const and non-const return `+0x88`) | the engine edits the desc in place through the non-const getter. |
| `Execute()` `0x74b5b0` | for streams 0..3: if no VB or disabled -> `SetStreamSource(i, NULL, 0, 0)` (if bound); else `SetStreamSource(i, vb, offset, stride)` and `SetStreamSourceFreq(i, freq)` (both cached; **freq is only set for enabled streams**, disabled streams keep the device's previous frequency). Then `SetIndices(ib)` (cached, NULL too), then `SetVertexDeclaration(decl)` (cached) or `SetFVF(fvf)` (uncached), then the draw exactly as `eCAPIDevice::Draw` (70k per run). Streams 4..7 are never touched. |

Vulkan for instancing: instanceCount = `freq(stream 0) & 0x3fffffff` when bit 30 is set; streams with bit 31 use
`VK_VERTEX_INPUT_RATE_INSTANCE` (divisor 1); D3D9 ignores the frequency for non-indexed draws.

## 6. eCAPIHLSLShader (eCDX9HLSLShader, 0x38) [C]

Fields: `+0x10` device, `+0x18` IDirect3DDevice9*, `+0x20` ID3DXBuffer* bytecode, `+0x28` eEGfxHLSLShaderType, `+0x30` the
VS/PS object.

| slot | behaviour |
|---|---|
| `Compile(eSGfxHLSLShaderSrc const&)` `0x749810` | text or file (archive; 10/4 on errors), `D3DXCompileShader(src, len, macros, eCGfxDX9Include, function, profile string[profile], flags, &code, &errors, NULL)`, type from the profile (`0..7` -> `0x101..0x801` PS, `8..11` -> `0x100..0x400` VS), then `Create` = keep the buffer and call slot `CreateShader`. 8 calls per run (pool misses). |
| `Read(bCIStream&)` `0x74a9a0` | record `u32 size` (0 = empty, nothing follows), `size` bytes of D3D9 shader bytecode, `u32 eEGfxHLSLShaderType` -> Create (2475 per run). `Write` `0x74aa40` writes the same. |
| `CreateShader()` `0x74a8b0` | `CreateVertexShader` / `CreatePixelShader` by `type & 1`. `DestroyShader()` releases the VS/PS only. |
| `GetShaderType(eEGfxHLSLShaderType&)` `0x749ef0` | **returns `type & 1`: 0 = vertex, 1 = pixel**. |
| `GetFunctionData(void**, ulong&)` / `GetShaderByteCodeSize(long&)` | buffer pointer and size / `D3DXGetShaderSize`. |
| `CreateConstantTable()` `0x749da0` | `D3DXGetShaderConstantTable(bytecode)` -> `ID3DXConstantTable*` (the `void*` of all constant slots). Vulkan: parse the bytecode's CTAB comment block. `DestroyConstantTable` releases. |
| `GetConstantCount(table, uint&)` | `D3DXCONSTANTTABLE_DESC.Constants`. `GetShaderVersion(table, ulong&)` = `.Version`. |
| `GetConstantDescription(table, parent, index, desc&, bool element)` `0x74a000` | handle = `element ? GetConstantElement(parent, index) : GetConstant(parent, index)` (parent NULL = top level), then `GetConstantDesc(handle, &d, &1)` -> eSGfxHLSLConstantDesc (name pointer owned by the table). |
| `GetSamplerIndex(table, handle, uint&)` | `ID3DXConstantTable::GetSamplerIndex` (sampler register). |
| `SetConstant*(table, handle, data, count)` | `ID3DXConstantTable::Set*(device, handle, ...)`: writes the device's **VS or PS register file (the table knows which, from the shader type)** at the constant's registers, with D3DX's matrix packing (row/column major per the constant's class) and type conversion. |
| `Execute()` `0x74a790` | binds the shader: VS via the `+0xdf0` cache (`SetVertexShader`), PS via `+0xdf8` (`SetPixelShader`). Nothing else (no decl, no constants). 44k per run. |

## 7. eCAPIEffect (eCDX9Effect, 0x28: `+0x10` device, `+0x18` ID3DXEffect*, `+0x20` current pass or -1) [C]

The `void*` handles are `D3DXHANDLE`s returned by the effect (parameters, techniques).

| slot | ID3DXEffect call |
|---|---|
| `SetTexture(h, eCAPITexture*)` / `SetCubeMap` / `SetVolume` | `SetTexture(h, tex ? tex->d3dTexture : NULL)` |
| `SetParameter(h, data, bytes)` / `GetParameter` | `SetValue` / `GetValue` |
| `Set/Get{Bool,Float,Int}Array(h, data, count)`, `Set/GetMatrixArray`, `Set/GetMatrixTransposeArray`, `Set/GetVectorArray` | the same-named ID3DXEffect methods |
| `GetParameterByName(name)` | `GetParameterByName(NULL, name)` |
| `GetEffectDescription(desc&)` | `GetDesc` -> Parameters, Techniques |
| `GetParameterDescription(index, desc&, parent)` | `h = GetParameter(parent, index)`; `GetParameterDesc(h)` -> name/semantic assigned to the engine's bCStrings ("" for no semantic), handle, class, type, rows, columns, elements, annotations, struct members, flags, bytes |
| `GetTechniqueDescription(index, desc&)` | `h = GetTechnique(index)`; `GetTechniqueDesc` -> name, handle, passes, annotations |
| `IsTechniqueValid(index, bool&)` | `ValidateTechnique(GetTechnique(index))` |
| `IsParameterUsed(param, technique, bool&)` | `IsParameterUsed(param, technique)` |
| `SetTechnique(h)` | `SetTechnique(h)` |
| `BeginTechnique()` `0x748b10` | device `BeginRenderStateBlock`, `Begin(&passes, D3DXFX_DONOTSAVESTATE)` |
| `SetPassNumber(n)` `0x7489f0` | n == current pass: `CommitChanges`; else `EndPass` (if a pass is active), `BeginPass(n)` |
| `EndTechnique()` `0x748bb0` | `EndPass` if active, `End`, device `EndRenderStateBlock` (restores the render states the passes set) |
| `OnLostDevice` / `OnResetDevice` | same-named methods |

The effect's state manager (`eCDX9EffectStateManager`, vtable `0x9eac00`, functions `0x2850a0..0x285380`) routes pass
states into the device: SetRenderState -> `SetCachedRenderState` (**ignores master flags**, recorded by the render-state
block), SetTexture / SetSamplerState / SetVertexShader / SetPixelShader -> the device caches, transforms/lights/material/
TSS/FVF/NPatch and shader constants straight to the device. A Vulkan effect runtime (fx_2_0 parser, `src/fx`) must apply
pass states to the same device state with the same rules.

## 8. Struct and enum table

| type | size | layout (offset: member) | evidence |
|---|---|---|---|
| eSGfxLayersDesc | 0x30 | 0 width, 4 height, 8 depth (0 = 2D/cube), 0xc bool cubeMap (+3 uninitialised pad bytes), 0x10 levels, 0x14 usage, 0x18 D3DFORMAT, 0x1c D3DPOOL, 0x20 byte size (engine), 0x24 ?, 0x28 const char* debug name (engine) | `eCGfxImageData::CreateLayers` `0x751fc0` (copies 0x30, dispatches on cubeMap/depth), `CreateImageFromMemory` `0x73f030` (cubeMap = 1), `CheckDeviceFormat` `0x73f5b0`, texture/cube/volume ctors `0x74c380`/`0x73ce50`/`0x74e120`, `FillResourceSizeAndLevelCount` `0x752140` (+0x20), `CreateImageFromMemoryExt` `0x7527f0` (+0x28), `eCDepthMap::IsFormatSupported` `0x59f810` (0x30 zeroed) |
| eSGfxImageInfo | 0x1c | D3DXIMAGE_INFO | `GetImageInfoFromMemory` `0x73f530` |
| eCAPIDevice::eSImageInfo | 0x18 [I size] | 0 w, 4 h, 8 depth, 0xc mips, 0x10 format, 0x14 bool cube, 0x15 bool volume | `GetImageInfoFromFile` `0x73f470` |
| eSGfxAdapterDesc | 0x548 | 0 Driver[512], 0x200 Description[512], 0x400 u64 DriverVersion, 0x408 VendorId, 0x40c DeviceId, 0x410 SubSysId, 0x414 Revision, 0x418 D3DCAPS9[8..0x130), 0x540 int VRAM MB | `sGetAdapterDesc` `0x73d7e0`, memset 0x548 in `InitializeAdmin` `0x74e510`, caps read in `PostInitialize` `0x6ba9d0` |
| eSGfxModeDesc | 0x18 | 0 w, 4 h, 8 refresh, 0xc bool vsync [I name], 0x10 u32 ? [I], 0x14 D3DFORMAT | `sGetModeDesc` `0x73d6f0`, `InitializeAdmin`, `eCGfxContextAdmin::Create` `0x507db0` |
| eSAPIModeParameters | 0x24 | 0 bool windowed, 4 MSAA, 8 w, 0xc h, 0x10 refresh, 0x14 format, 0x18 depth fmt, 0x1c bool vsync, 0x20 ? | `ToggleNewMode` `0x73f690`, `eCGfxContextAdmin::ToggleNewMode` `0x507f30` |
| eSAPIStartParameters | 0x30 | 0 HWND, 8 behavior flags, then 0xc..0x30 = the mode parameters | `sCreateDevice` `0x73d420`, `eCGfxContextAdmin::Create` `0x507db0` |
| eSGfxVertexBufDesc | 0x14 | length, usage, FVF, pool, stride (if FVF 0) | `0x74dcf0`, `GetDesc` `0x74dfe0` |
| eSGfxIndexBufDesc | 0x10 | length, usage, format, pool | `0x74ad80`, `0x74b030` |
| eSGfxDrawCallDesc | 0x20 | 0 bool indexed, 4 type, 8 primCount, 0xc startVertex, 0x10 minIndex, 0x14 numVertices, 0x18 startIndex, 0x1c baseVertexIndex | `Draw` `0x741f40`, `eCDX9Primitive::SetDrawCallDescription` `0x74b530`, `Execute` `0x74b5b0` |
| eSGfxDclElement | 8 | D3DVERTEXELEMENT9 | `CreateVertexDeclaration` `0x742e20` |
| eSGfxLockedRect | 0x10 | int pitch, 4 pad, void* bits (D3DLOCKED_RECT) | `0x74c910` |
| eSGfxViewport | 0x18 | left, top, right, bottom (ints, exclusive), minZ, maxZ | `0x740530`, `0x740630` |
| eSGfxGammaRamp | 0x600 | D3DGAMMARAMP | `0x741550` |
| eSGfxStencilData | 0x1c | fail, zfail, pass, func, ref, mask, writemask | `0x7419f0`, `0x2860e0`, `GetMasterStencilTesting` `0x740ea0` |
| eSGfxAlphaTestData | 8 | ref, func | `0x742a50`, `0x740ff0` |
| eSGfxFogData | 0x1c | 0 bool enable, 1 bool range, 4 color, 8 vertexFog (1), 0xc mode, 0x10 start, 0x14 end, 0x18 density | `SetFog` `0x7424e0` |
| eSGfxPlaneData | 0x10 [I] | float a,b,c,d | `0x745030` |
| eSGfxMacro | 0x10 | D3DXMACRO | `Compile` `0x749810` |
| eSGfxHLSLShaderSrc | 0x28 | 0 bCString source/path, 8 macros, 0x10 bCString function, 0x18 profile index, 0x1c flags, 0x20 bool fromFile | `Compile` `0x749810`, `PreprocessShader` `0x73e780` |
| eSGfxHLSLConstantDesc | 0x38 | 0 name, 8 handle, 0x10 regset, 0x14 class, 0x18 type, 0x1c rows, 0x20 cols, 0x24 elements, 0x28 struct members, 0x2c bytes, 0x30 reg index, 0x34 reg count | `0x74a000` |
| eSGfxEffectSource | 0x18 | 0 bCString text, 8 length, 0xc flags, 0x10 macros | `0x743b60` |
| eSGfxEffectFileSource | 0x18 | 0 bCString path, 8 flags, 0x10 macros | `0x743cf0` |
| eSGfxEffectDesc | 8 [I size] | parameters, techniques | `0x7486e0` |
| eSGfxParameterDesc | 0x40 | 0 bCString name, 8 handle, 0x10 bCString semantic, 0x18 class, 0x1c type, 0x20 rows, 0x24 cols, 0x28 elements, 0x2c annotations, 0x30 struct members, 0x34 flags, 0x38 bytes | `0x748730` |
| eSGfxTechniqueDesc | 0x18 | 0 bCString name, 8 handle, 0x10 passes, 0x14 annotations | `0x748820` |
| eSFontDesc | 0x30 | height, width, weight, mips, 0x10 bool italic, 0x14 charset, 0x18 out precision, 0x1c quality, 0x20 pitch&family, 0x28 bCUnicodeString face | `eCDX9Font` ctor `0x748d80` |
| eSAPIMaterial | 0x88 | 4 colors (diffuse, ambient, specular, emissive) of 0x20 (r,g,b at +8, a at +0x18), power at 0x80 | `SetMaterial` `0x742730` |
| eSGfxLightData | 0xa0 | 0 type, 8/0x28/0x48 diffuse/specular/ambient colors, 0x68 position, 0x74 direction, 0x80 range, falloff, att0..2, theta, 0x98 phi | `SetLights` `0x7422a0` |
| bCRect | 0x10 | int left, top, right, bottom | `SetScissor` `0x740310` |
| bCMatrix | 0x40 | D3DMATRIX | `SetWorldMatrix` `0x741d10` |

| enum | values | evidence |
|---|---|---|
| eEBlending / eEMasterBlending | D3DBLEND 1..17 (master 0 = none) | `SetBlending` `0x742960`, `SetMasterBlending` `0x740f20` |
| eECmpFunc / eEMasterCmpFunc | D3DCMPFUNC 1..8 | `0x7417b0`, `0x740ca0` |
| eECullingMode / Master | D3DCULL 1 none, 2 CW, 3 CCW | `0x741710` |
| eERenderMode / Master | D3DFILLMODE 1..3 | `0x7415d0` |
| eEShadeMode / Master | D3DSHADEMODE 1..3 | `0x741670` |
| eEWrite, eEStencil, eEAlphaTest (+Master) | 1 disabled, 2 enabled (master 0 none) | `== 2` tests in `0x741850`, `0x7419f0`, `0x742a50`; Get* re-encode `+1` |
| eEMasterFogging | 0 none, 1 disable fog | `0x741040` |
| eEViewportClear | D3DCLEAR 1/2/4 | `0x740710` |
| eEPrimitiveType | D3DPRIMITIVETYPE 1..6 | `0x741f40` |
| eETexAddress | D3DTEXTUREADDRESS 1..5 | `0x744170` |
| eETexFilter | D3DTEXTUREFILTERTYPE 0..3, 6..8 | `0x744300`, `0x74d450` |
| eESamplerState | D3DSAMPLERSTATETYPE 1..13 | `0x744540` |
| eECubeMapFace | D3DCUBEMAP_FACES 0..5 | `0x73c790` |
| eECopyFilter | D3DX_FILTER_* flags | `0x74cef0`, `0x74cd70` |
| eEDepthFormat | D3DFORMAT (D24S8 75 used) | `0x744ad0`, `0x73d420`, `0x4c4330` |
| eEPoolType | D3DPOOL 0..3 | `0x73f030` |
| eEColorFormat (not in signatures) | D3DFORMAT; names `eEColorFormat_Unknown/A8R8G8B8/X8R8G8B8/X1R5G5B5/A1R5G5B5/A4R4G4B4/R16F/G16R16F/A16B16G16R16F/R32F/G32R32F/A32B32G32R32F/DXT1..5` | reflection registration `0x666d00`, `0x68ca40` |
| eEMultiSample (not in signatures) | D3DMULTISAMPLE_TYPE 0, 2..16 | `0x73d420` |
| eEContextCreate (not in signatures) | D3DCREATE_* (0x40, 0x42) | `0x4c4330` |
| eEGfxHLSLShaderType | bit0 = PS, version << 8 (`0x100..0x400` VS, `0x101..0x801` PS) | `Compile` `0x749810`, `GetShaderType` `0x749ef0`, `GetNameFromFormat` `0x508670` |
| eEAPIType | 3 = DX9 (0/1 [I]) | `0x74b140`, `0x74e510` |
| eEMaterialColorSrc, eETexArgument, eETexOperation, eETexCoordCalc | D3DMCS / D3DTA / D3DTOP / D3DTSS_TCI | `0x742ba0`, `0x744870`, `0x7449a0`, `0x744490` |
| eESpriteRenderOpt, eENormalMapGen, eEChannel | D3DXSPRITE / D3DX_NORMALMAP / D3DX_CHANNEL flags [I] (passed through) | `0x746bd0`, `0x74ccd0` |
| eEGraphicMemoryType | 4 VS, 5 PS (shader formats), 2..5 no mip sizes; 0..3 names [I]; never used by the DX9 layer | `0x508670`, `0x5082e0` |

## 9. Engine quirks a replacement must reproduce or knowingly drop

1. `BeginScene` blocks until the GPU finished the previous frame (event query spin).
2. `SetRenderTarget`/`SetRenderTargetAt` unbind all 16 texture stages; D3D9 resets viewport/scissor to the new RT0 size.
3. Master states: apply-on-set, no restore on release; normal setters silently ignored while active; effects bypass them.
4. Render states set inside an effect technique are restored at `EndTechnique` (samplers/textures/shaders are not).
5. `IsResetNecessary` is true when the device is healthy; `IsDeviceNotReset` is polled every frame.
6. `GetVertexProgramRegisterB` writes instead of reading (never called by the game [I]).
7. `GetMasterDepthBias` does not return the floats.
8. `DrawIndexedUP` is 16-bit only; `DrawUP`/`DrawIndexedUP` unbind stream 0 (and the IB).
9. `eCAPIPrimitive` has 4 streams; `SetStream` snapshots the stride; `Execute` sets frequencies only for enabled streams.
10. Cube `LockRect` ignores the rect; `ConvertFrom`'s bool is unused.
11. `SetViewport` turns a zero width/height into 32.
12. The 3 pad bytes after `eSGfxLayersDesc::cubeMap` are uninitialised: compare the bool, never the dword.
13. `CreateVertexDeclaration` returns an object with an extra reference; `DestroyVertexDeclaration` releases until 0.

## 10. Open points

* `eSGfxModeDesc +0x10` / `eSAPIModeParameters +0x20` / start parameters `+0x2c`: source is `eSSetupWindow +0x18`; meaning
  unknown, ignored by DX9.
* `eSGfxLayersDesc +0x24`: never seen written or read (padding?).
* `eSGfxEffectDesc`, `eSImageInfo`, `eSAPIMaterial` sizes are the minimum covering the written members.
* The engine names of all enumerators except `eEColorFormat_*` (no strings in Engine.dll); the names in `types.h` are
  descriptive.
* `bCFloatAlphaColor` layout inside material/light (vtable + rgb + pad + a) is inferred from the read offsets.
* Not analysed (editor/tool only, never called at runtime): D3DX shape/mesh slots (`CreateBox`..`CreateTorus`,
  `GenerateLODMesh`, `CreateUVAtlas`) and their structs, `eSSpriteDesc`; fonts/sprites (`eCAPIFont`, `eCAPISprite`) only as far
  as `eSFontDesc`.
