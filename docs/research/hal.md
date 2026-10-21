# The engine's graphics HAL: layout, ownership, live usage

Findings of milestone M1 (tracing passthrough, `mode = "trace"`), verified in the game unless marked **[I]** (inferred).
All addresses are Engine.dll RVAs. Complements `engine-gfx.md`.

## 1. Generated interfaces

`tools/gen_hal.py` reads the vtables of the 12 `eCAPI*` interfaces (exports `??_7eCAPIx@@6B@`: slot count, the destructor =
the only non-`_purecall` slot) and of their `eCDX9*` implementations (names + signatures from the demangled exports) and
writes `src/hal/api_generated.h` (interfaces) and `src/hal/trace_generated.h` (forwarding wrappers). Overloads get unique names
(`_A`/`_W`/`_const`/`_<slot>`), because MSVC groups overloaded virtuals and would reorder the slots.

| interface | slots | destructor slot | notes |
|---|---|---|---|
| eCAPIVertexData | 5 | 4 | |
| eCAPIIndexData | 4 | 3 | |
| eCAPITexture | 17 | 16 | |
| eCAPICubeMap | 12 | 11 | |
| eCAPIVolume | 5 | 4 | |
| eCAPIPrimitive | 17 | 16 | **no reference count** |
| eCAPIHLSLShader | 26 | 25 | |
| eCAPIEffect | 30 | 29 | |
| eCAPISprite | 11 | 10 | |
| eCAPIFont | 10 | 9 | `DrawTextA(bCUnicodeString by value, ...)`: ABI = pointer to a caller copy, callee destroys |
| eCAPIDevice | 211 | 210 | `eCDX9Device` has 2 more slots (`SetFont/SpriteCreateCallback`), not part of the interface |
| eCAPIInstance | 8 | 7 | |

**Verified live**: the whole game (menu, new game, world) runs with every HAL object behind a generated wrapper (scenario
`testdemo` PASS). A wrong slot or signature would crash at once.

## 2. Object layout and ownership [C]

* `+0` vtable, `+8` `int` reference count (constructor sets 1; `eCAPIPrimitive` has none). `AddRef`/`Release` are
  **non-virtual** exports of the interface (`0x5004e0` / `0x5004a0` for the device): `Release` decrements and at zero calls the
  destructor slot with **flags 0** and frees with **`bCMemoryAdmin::GetInstance().DeleteObject(p)`** (SharedBase).
* `eCAPIPrimitive` (no count) is destroyed the same way by its owner (`eCGfxVisual::Destroy` `0x505fc0`: destructor slot, flags 0,
  then `DeleteObject`).
* HAL objects are allocated with `bCMemoryAdmin::NewObject(size, 0, 0, 0, -1)`. **Never `delete` an engine HAL object**: the
  flag-1 path of its deleting destructor frees with the CRT heap -> heap corruption (seen: `0xC0000374`). Our objects derive from
  `hal::EngineObject` (operator new/delete through bCMemoryAdmin), so the engine can free them.
* `bEResult`: 1 = success, 0 = failure.

## 3. Device creation and the 32-bit program ids [C]

* `Direct3DCreate9` runs **twice**: ~6 s before `eCGfxAdmin::InitializeAdmin` (another caller, not yet identified) and inside it.
* `eCDX9Device::CreateVertexProgram(void* unused, void* bytecode, unsigned long* id, bool)` and
  `CreatePixelProgram(void* bytecode, unsigned long* id)` store the `IDirect3D*Shader9*` in an **`unsigned long` (32 bit)**:
  the x64 port truncates the pointer (works only while D3D9 allocates them below 4 GiB). Our ids are our own 32-bit handles.
* `CreateVertexDeclaration` passes `eSGfxDclElement*` straight to D3D9: it is `D3DVERTEXELEMENT9` (8 bytes, ends with stream 0xFF).

## 4. Shaders and effects [C]

* Material shaders: `CreateHLSLShader` (4966 objects in a menu + new game run) then **`eCAPIHLSLShader::Read(bCIStream&)`** from the
  shader pool (2475) - record = `u32 size, bytecode, u32 shader type` - or `Compile` (8, runtime compile on a pool miss). The
  device-level `CreateVertexProgram`/`CreatePixelProgram` are **not used** by the game. Execution: `eCAPIHLSLShader::Execute`.
  `GetFunctionData(void**, unsigned long&)` returns the bytecode (ID3DXBuffer) without side effects.
* Captured corpus (one menu + new game run): 2330 `ps_3_0`, 146 `vs_3_0`, 4 `ps_2_0`, 3 `vs_2_0`.
* Effects: `CompileEffectFromFile` (34) -> aux buffer -> `CreateEffect(data, size)` (35) with a **compiled `fx_2_0` effect binary**
  (magic `0xFEFF0901`, techniques/passes/parameters/states + embedded shaders). Format known from Wine's d3dx9 (`effect.c`):
  our `eCAPIEffect` can parse it directly instead of running D3DX.

## 5. Resources seen [C]

`eSGfxLayersDesc`: `+0 width, +4 height, +8 0, +0xc ? (not initialised, garbage values), +0x10 levels, +0x14 usage
(0, 1 = render target, 2 = depth stencil, 0x200 = dynamic), +0x18 D3DFORMAT, +0x1c pool (0 default, 1 managed, 2 system mem)`.

| format | use |
|---|---|
| 21 A8R8G8B8 | render targets (back-buffer size, 1/2, 1/4; 1024 shadow), managed/system-mem textures |
| 113 A16B16G16R16F | scene HDR target (1600x900) |
| 114 R32F | depth map (z pre-pass), luminance chain 64..1 (eye adaptation) |
| 115 G32R32F | 1024x1024 render target (variance shadow map) |
| `NULL` FOURCC (`0x4C4C554E`) | 1024x1024 colour target without memory (depth-only shadow pass) |
| `DF24` FOURCC (`0x34324644`), usage 2 | 1024x1024 depth texture (shadow map, sampled) |
| `DXT5` usage 0x200 | 4x4 dynamic |
| depth buffer 75 (D24S8) | `CreateDepthBuffer` 1024x1024 |
| cube map A8R8G8B8 128x128 render target | (sky/reflection) |

Textures from files: `CreateImageFromMemory` (402 in the run). `engine-gfx.md` said no NULL/DF24 hacks: **wrong**, both are used.

## 6. Slot usage in a menu + new game run [C]

Per interface, the slots with calls (count over ~340 frames). Everything else had zero calls in this run.

* **VertexData** LockBuffer/UnlockBuffer (62k: dynamic buffers every frame). **IndexData** Lock/Unlock (294).
* **Texture** LockRect/UnlockRect (664), LockImageLayer (25), GetRenderTargetHandle (1408), SetTexture (303), GenerateTextureMipMaps,
  SaveTextureToFile (2), CopyCurrentRTToImage (46), CopyImageToCurrentRT (44), LoadSurfaceFromSurface (2), GetRenderTargetData (2).
* **CubeMap** GetRenderTargetHandle, SetCubeMap.
* **Primitive** SetStream, SetIndexData, SetVertexDecl, **SetStreamSrcFrequency (66k: instancing)**, SetStreamOffset,
  SetDrawCallDescription, GetDrawCallDescription (non-const), EnableStream, **Execute (70k draws)**.
* **HLSLShader** Compile, Create/DestroyConstantTable, GetFunctionData, GetShaderType (132k), GetConstantCount,
  GetConstantDescription, GetSamplerIndex, **Execute (44k: bind)**, Read.
* **Effect** SetTexture, SetParameter, GetParameter, GetParameterByName, GetEffectDescription, GetParameterDescription,
  GetTechniqueDescription, IsParameterUsed, SetTechnique, SetPassNumber, BeginTechnique, EndTechnique.
* **Device** CreatePrimititve, CreateIndexData, CreateVertexData, CreateEffect, PreprocessShader, CreateHLSLShader, CreateTexture,
  CreateImageFromMemory, CreateCubeMap, CheckDeviceFormat, IsDeviceNotReset, Set/GetViewport, ClearViewport, SwapScreen,
  Begin/EndScene, SetMaster{RenderMode,CullingMode,DepthTesting,DepthWrite,Blending,AlphaTesting,Fogging},
  GetMaster{DepthTesting,DepthWrite}, SetAlphaBlending, SetColorWriting, SetAlphaWriting, SetGammaRamp, SetRenderMode,
  SetCullingMode, SetDepthTesting, SetDepthWrite, SetViewAndProjectionMatrices, SetWorldMatrix, Set/GetViewMatrix, SetFog,
  Create/DestroyVertexDeclaration, GetVertexDeclarationData, **SetVertexProgramRegisterF (70k)**, **SetPixelProgramRegisterF (52k)**,
  CompileEffectFromFile, Get/DestroyAuxBuffer, **SetSamplerState (263k)**, CreateDepthBuffer, SetRenderTarget, GetRenderTarget,
  GetBackBuffer, SetTextureNULL, Begin/EndRenderStateBlock.
* **Instance** CreateDevice.
* Not seen (yet): sprites, fonts (`eCAPISprite`/`eCAPIFont`), DrawUP/DrawIndexedUP, fixed-function texture stages, occlusion
  queries, volumes, blending via `SetBlending` (only `SetMasterBlending`), stencil, scissor, clip planes. The GUI may use them in
  other screens (inventory, dialogues, map): trace those before declaring them unused.
