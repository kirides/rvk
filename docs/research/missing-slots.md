# Unimplemented HAL slots: who calls them, what D3D9 did, what to do

State of `hal/vulkan` after commit cae8530. Engine.dll RVAs unless noted (Game.dll / Script_Game.dll are marked).
**[C]** confirmed (decompiled, trace counts, our code read), **[I]** inferred.
Dumps: `B:\DevDrive\risen-ghidra\rvk_missing_slots_dump{,2..9}.txt`, `rvk_missing_slots_game_dump.txt` (Game.dll),
`rvk_missing_slots_sg_dump.txt` (Script_Game.dll). eCDX9 bodies: `B:\DevDrive\risen-gfx-re\dx9_layer.txt`.
Call counts: `B:\DevDrive\risen-test-vk\bin\rvk\trace-stats.txt` (trace mode, menu + new game, ~340 frames).

How the callers were found: the engine reaches these slots through `eCGfxImageData` / `eCGfxAssistantAdmin` /
`eCGfxMixerAdmin` / `eCGfxHLSLShader` / `eCGfxVisual` wrappers. I dumped each wrapper with its callers, checked which other
DLLs import a wrapper (pefile scan of `bin\*.dll`, `bin\scripts\*.dll`), and scanned Engine.dll for virtual calls through
`eCGfxContextAdmin::ms_pAPIDevice` (`0xc62750`). Trace mode wraps every object in our own vtables, so the speculative
devirtualisations go through the virtual path and the trace counts are complete for the run.

## Status (2026-10-21, after the slot completion commit)

| slot | status |
|---|---|
| FVF vertex buffers (`CreateVertexData` stride), Primitive `SetFVF`, Device 143 `GetFVFSize` | **done** (section 4) |
| CubeMap 2/3 `Lock/UnlockCubeMapLayer` | **done** (section 5) |
| Device 9/10 `CreateTexture/CubeMapFromMemory` | **done** for DDS as stored (section 5) |
| Texture 14 `LoadSurfaceFromSurface` | **done**, GPU blit + read-back (section 2) |
| Texture 9 `SaveTextureToFile` | **refused on purpose** (log + optional PNG dump, section 2) |
| everything in sections 6 and 7 | stubs kept (not called in the game) |

`testdemo` in Vulkan mode with validation: PASS, 0 unimplemented slots seen (was 2), no warnings from these paths.

## 1. Overview

What is still a stub (generated stub classes in `src/hal/stub_generated.h` that `src/hal/vulkan/hal_vulkan.h` and
`effect.cpp` do not override). VertexData, IndexData, Primitive, Effect and Instance have no stubs left.

| interface | stub slots | called by the game? |
|---|---|---|
| Texture | 6 ComputeNormalMap, 7 ConvertFrom, **9 SaveTextureToFile**, 10 SaveTextureToMemory, **14 LoadSurfaceFromSurface** | 9 and 14: yes, twice per run (sky absorption image, section 2). 6, 7, 10: only the resource import path (raw → compiled), not in the shipped game [I] |
| CubeMap | 2/3 Lock/UnlockCubeMapLayer, 6 ConvertFrom, 7 GenerateCubeMapMipMaps, 8 GetRenderTargetData, 9/10 SaveTextureTo* | not seen. 2/3 are reachable from `eCGfxImageData::OnRead/OnWrite/LockLayer` on a cube image (section 5) |
| Volume | 3 GenerateVolumeMipMaps | no |
| HLSLShader | 9-18 SetConstant* | **no**: the wrappers have no callers and no importers; the devirtualised sites call other slots (section 6) [C] |
| Sprite (10), Font (9) | all | unreachable: their only creators, device slots 14/15, have no callers. GUI text uses `eCFont` + `eCSprite` (fonts.md) [C] |
| Device (60) | 7-10 Create*FromFile/Memory, 14/15 CreateFont/Sprite, 16 GetImageInfoFromFile, 48/49/76 ZBias, 86/88/92-95 lights + material, 100-102/107-112/117-121 programs + assembler, 129/131-135 fixed-function texture stages, 143 GetFVFSize, 150-158 assistant + D3DX meshes, 160 SaveBackbufferToMemory, 161 DrawTextA, 171-180 sprite lists, 207 CreateUVAtlas | 159 SaveBackbufferToFile (snapshot key) is **implemented** (section 3). 9/10: reachable only through `eCGfxImageData::CreateImageFromMemory` (section 5). The rest: editor / legacy, section 7 |

Separate from the stubs: **FVF vertex buffers** (section 4). `CreateVertexData` takes the stride from the wrong field, and
`HalPrimitive::SetFVF` drops the vertex layout.

## 2. Sky absorption image: `LoadSurfaceFromSurface` (Texture 14) + `SaveTextureToFile` (Texture 9)

**Caller** [C]: `eCSkydome_PS::CreateAbsorbationImage(eCImageResource2* sky)` `0x6e2cb0`. It is called from
`eCSkydome_PS::CreateShader`, `GetDependencies` and `OnNotifyPropertyValueChangedExitEx`. That happens whenever a sky dome is
created: the main menu scene and the game world. This is the 2 + 2 calls in the trace, and the two warnings in rvk.log
right after frame 1.

1. It looks up the resource `<sky image name>_Absorption` and compares file times: the absorption image against the sky
   image (`CFXVirtualFile::GetFileTimes`). If the absorption image is newer, it returns the existing one. Otherwise, or when
   either path is empty, it regenerates the image. Packed files probably carry equal times, so it regenerates on every
   creation [I].
2. `img1 = CreateLayers({w, h of the sky image, levels 1, usage 0, A8R8G8B8 (21), pool 2 SYSTEMMEM})`, then
   **`img1->LoadSurfaceFromSurface(0, NULL, NULL, sky, 0, NULL, NULL, 1 /*D3DX_FILTER_NONE*/, 0)`** (inlined virtual call via `img1+0x60`, vtable `+0x70`). This is a
   format conversion of the sky image (usually DXT [I]) to A8R8G8B8. The return value is ignored.
3. `img2 = CreateLayers(same, pool 1 MANAGED)`. It locks both through `LockImageLayer`, computes a wrapping 32-tap
   horizontal + vertical box blur of the red channel (byte 2) of img1, writes it to img2, and unlocks.
4. **`SaveImageToFile(img2, "#G3:/Data/Raw/Images/Sky/" + <name>_Absorption.tga)`** through
   `eCGfxAssistantAdmin::SaveImageToFile` `0x756780` → `eCAPITexture::SaveTextureToFile` (slot 9). Then
   `eCResourceCache2::ReloadResource(<absorption resource>)`.

**The computed image is never used directly**. It only reaches the game if the file write succeeds and the reload picks it
up. In the test copy no `*_Absorption*.tga` exists anywhere: there is no `data\raw`, and nothing was written through the
`data\compiled` / `data\common` junctions (checked read-only after the D3D9 trace runs). So with D3D9 the write already
failed ("Couldn't open file to write texture") and the shipped absorption image was reloaded [I, strong]. Our stubs return
`bEResult_False` (`stub_generated.h:153`, `:124`), and the result is the same as with D3D9: **no visible effect, only two
log warnings.**

**D3D9 semantics** [C]:
* `eCDX9Texture::LoadSurfaceFromSurface` `0x74d920`: dst = **this** level `a0`, src = `a3` level `a4` (RTTI-cast to
  eCDX9Texture). Calls `D3DXLoadSurfaceFromSurface(dst, dstPalette a1, dstRect a2, src, srcPalette a5, srcRect a6,
  filter a7, colorKey a8)`. Rects are `bCRect` copied to RECT. Any format can be converted and scaled on the CPU (D3DX).
* `eCDX9Texture::SaveTextureToFile` `0x74cf80`: the extension picks the format: `.JPG` 1, `.BMP` 0, `.TGA` 2, `.DDS` 4,
  anything else DDS with a warning. Calls `D3DXSaveTextureToFileInMemory(&buf, fmt, tex, NULL)`, then
  `eCArchiveManager::CreateFileA(path)` + `eCArchiveFile::Open(2 = write)` + a write through the engine VFS. The other
  caller of slot 9 is `eCLightmapPreprocessor::PackLightmap` `0x581340` ("%s%s_%i.dds", editor lightmap baking).

**Recommendation (P1, safety rather than visuals)**:
* `SaveTextureToFile`: **never write through the engine paths.** `#G3:/Data/...` resolves into the game's data tree, and in
  the test copy part of that tree is a junction into the real install. Implement it as "log once, return `bEResult_False`"
  (D3D9's de facto result here), so the engine reloads the shipped image. Optionally, behind a `[debug]` switch, write the
  image as PNG to `bin/rvk/dumps/<basename>.png`. Do not return Ok without writing: nothing depends on Ok, and False matches
  D3D9.
* `LoadSurfaceFromSurface`: low priority. To be complete, implement it on the GPU, because the CPU copy of a managed file
  texture is gone after its first upload (`src/d3d9/resources.cpp:189-192`):
  1. upload the source if it is pending (`prepareForSampling` logic).
  2. `stretchRect` (`src/d3d9/device.cpp:750`, `vkCmdBlitImage`; BC formats must support `BLIT_SRC` when
     `textureCompressionBC` is on) from the src level/rect into a temporary B8G8R8A8 render target of the dst rect's size.
     Filter NONE/POINT → nearest, otherwise linear.
  3. if dst is a render target, blit straight into it. Otherwise `readRenderTarget` (`device.cpp:773`; extend it with a dst
     rect) into the dst CPU level.

  Ignore palettes and the colour key: always NULL/0 here. With the fixed False from `SaveTextureToFile` the result is unused
  anyway, so this only silences the warning. Not worth more than ~40 lines.

**Done (2026-10-21)** [C, testdemo run]:
* `HalTexture::SaveTextureToFile` returns `bEResult_False` and logs the engine path
  (`#G3:/Data/Raw/Images/Sky/Sky_Clouds_01_Absorption.tga`, `..._02_...`). It never writes the engine path. With
  `[debug] dump_saved_textures = true` (`run.ps1 -DumpSavedTextures`) an A8R8G8B8/X8R8G8B8 image is also written as an
  opaque PNG to `bin/rvk/dumps/<name>.png`. The dumps show the expected blurred cloud image.
* `HalTexture::LoadSurfaceFromSurface` → `d3d9::Device::loadSurfaceFromSurface` (`src/d3d9/device.cpp`). It uploads the
  source's pending data, then blits (`vkCmdBlitImage2`, BC decompression, nearest for D3DX_FILTER_NONE/POINT, linear
  otherwise) into the destination. For a render target the blit goes straight into the level. Otherwise it goes into a
  temporary render target of the destination format and is read back into the destination's CPU copy (dst rect honoured).
  The game's calls: `1024x1024 DXT1 level 0 -> 1024x1024 A8R8G8B8 level 0 (filter 0x1)`, twice, on the render thread, about 0.25 s
  each including the engine's blur. It refuses sources with a view swizzle (L8, A8L8, ...), depth sources, cube or volume
  textures, compressed or converted destinations, formats without `BLIT_SRC`, and calls off the render thread. Colour key:
  ignored, with a warning.
* Visual result: unchanged by construction. The image is still only used through the refused file write and
  `ReloadResource`, as with D3D9.

## 3. Snapshots: `SaveBackbufferToFile` (Device 159)

**Callers** [C]:
* Script_Game.dll `OnDebugTakeSnapshot` `0xe8540` (a debug-action script: `Input::GetCurrentDebugEvent` state 1) and the
  importers of `eCApplication::Snapshot` `0x4bfc90` / `SnapshotEx` `0x4bfba0` (only Script_Game.dll). They build
  `#G3:/snapshots/<config prefix>_<short date>_<NNNN>.bmp` (counter wraps at a config maximum), `MakeDir` it, and set
  `eCApplication+0x144 = 1`, `+0x148 = path`.
* `eCApplication::Draw` `0x4c13b0`: after the GUI is drawn, **before `SwapScreen`**, `DoSnapshot` `0x4c0c50` calls
  `eCGfxAdmin+0x5b8 → device slot 159 (path)`. On failure it logs "eCApplication::Snapshot - Could not save snapshot !".
* Other callers of slot 159: `eCLightmapPreprocessor::Render*` (editor), and `eCGfxAssistantAdmin::SaveImageToFile(NULL, path)`.
* The VFS mount from the embedded `[Physical]` config: `snapshots=$(localappdata)/Risen/ScreenShots` (FUN 0x284660). This is
  outside the game tree: safe to write.

**D3D9 semantics** `eCDX9Device::SaveBackbufferToFile` `0x746260` [C]: `GetRenderTarget(0)` (the current RT0, which is the
back buffer at this point), `D3DXSaveSurfaceToFileInMemory(&buf, D3DXIFF_BMP, surf, NULL, NULL)`, then the same
`eCArchiveManager::CreateFileA` + `Open(2)` + write as above. The format is always BMP, whatever the extension.
`SaveBackbufferToMemory` (slot 160) puts the same data into a `bCMemoryStream`; it has no game caller.

**Done (2026-10-21)**: `HalDevice::SaveBackbufferToFile` (`src/hal/vulkan/device.cpp`). Verified in game with
`[debug] snapshot_frames` (`run.ps1 -Snapshots 500,700`: a detour of `eCApplication::Draw` calls `eCApplication::Snapshot()`
before the original, so `DoSnapshot` runs in the same Draw) [C]:
* The engine passes the **virtual** path `#G3:/snapshots/Snapshot__21.10.2026_0000.bmp` (prefix `Snapshot_`, an empty name
  part, `dd.mm.yyyy`, a 4-digit counter that continues over the files already present). `eCVirtualFileSystem::MakeAbsPhysicalPath`
  resolves it to `<test copy>\localappdata\Local\Risen\ScreenShots\...` (spass's `%LOCALAPPDATA%` redirect).
* **D3DX's BMP, measured in a D3D9 (passthrough) run**: 32 bpp `BI_RGB`, bottom-up, the 4th byte as stored in the
  X8R8G8B8 back buffer (mostly 255, some 204/254: the game writes alpha), `biSizeImage` 0, 2834 px/m, plus 2 zero bytes after
  the pixels counted in `bfSize` (1600x900 -> 5 760 056 bytes). Not the 24-bit file assumed above. Our writer
  (`api::encodeBmpLikeD3DX`) produces the same header byte for byte; the alpha distribution of a Vulkan snapshot matches too.
* The snapshot of a frame is pixel-identical (RGB) to `rvk_screenshot`'s PNG/BMP of the same present (reference-compare.md 3.9).
* Differences to `eCDX9Device`: it reads the back buffer (RT0 is the back buffer at `DoSnapshot`; a warning is logged
  otherwise), refuses physical paths inside `<game>\data` (`api::refuseDataPath`, junction-aware), writes through
  `<file>.tmp` + rename, and returns `False` when the file cannot be written (D3D9 returns `Ok` once the surface was read).
* `eCApplication::Draw` calls and presents differ (Draw 500 = present 793 in `testdemo`): loading screens present
  without `eCApplication::Draw` [I].

**Sketch** (as planned; implemented like this except the BMP format):
1. `dev_.readBackBuffer(bgra, w, h)` (`src/d3d9/device.cpp:819`) at call time. We are still inside the frame before
   `SwapScreen`, so this is the image with the GUI, as with D3D9.
2. Encode a 24-bit bottom-up BMP (54-byte header). A PNG would be smaller, but the engine named the file `.bmp`.
3. Resolve the path through the engine: bind `eCVirtualFileSystem::GetInstance` + `MakeAbsPhysicalPath(bCString&)`
   `0x733990` (with the bCString ctor/dtor/GetText from SharedBase), then `std::ofstream`.
4. **Guard**: write only if the physical path lies outside `<game>\data` (or equivalently, the virtual path starts with
   `#G3:/snapshots/`). Otherwise return False. Do not use `eCArchiveManager` from our code: `eCArchiveFile` is a stack object
   with several vtables; not worth reproducing.

## 4. FVF vertex buffers (`CreateVertexData` with `fvf != 0`, `Primitive::SetFVF`)

**D3D9 semantics** [C]:
* `eCDX9VertexData` ctor `0x74dcf0` stores `+0x24 = desc.fvf` and `+0x20 stride = D3DXGetFVFVertexSize(desc.fvf)`.
  **`desc.stride` is used only when `fvf == 0`.** It calls `CreateVertexBuffer(length, usage, fvf, pool)`.
* `eCDX9Primitive::SetFVF` `0x74b480` sets `+0x78 = 1, +0x80 = fvf`. `Execute` `0x74b5b0` then calls
  `IDirect3DDevice9::SetFVF(fvf)` (`+0x2c8`) instead of `SetVertexDeclaration`. The stream stride comes from the
  primitive's stream entry (`+0x34 + 16*i`), which `eCGfxVisual` fills from the vertex data's stride.

**Ours**:
* `HalDevice::CreateVertexData` (`src/hal/vulkan/device.cpp:117-125`) warns and keeps `desc.stride`.
  `HalVertexData` (`src/hal/vulkan/resources.cpp:166-167`) takes `stride_(desc.stride)`.
  **Bug**: for an FVF buffer the engine may leave `desc.stride` 0 or garbage → stream stride wrong → garbage or skipped draws.
* `HalPrimitive::SetFVF` (`resources.cpp:238-246`) sets `decl_ = nullptr` → `Execute` draws without a layout (skipped as
  "vertex input").

**Callers**: SetFVF has **0 calls** in the trace, and `eCGfxVisual::EnableFVF` `0x506480` has no caller or importer [C].
The buffer creators I decompiled set `fvf = 0` with an explicit stride: billboards `0x4f00a0` (52 B), sky objects `0x6e1ee0`
(32 B), decals `0x4fbd80`, sky dome `0x6e5f90` (12 B) [C]. The creator of the FVF buffer that rvk.log reports is not
identified. The warning prints the FVF: take it from a log and set a breakpoint, or log `_ReturnAddress()` in
`CreateVertexData`.

**Fix (P2, cheap)**:
1. `stride = fvf ? fvfVertexSize(fvf) : desc.stride`, with D3DX's rule:
   * position: XYZ 12, XYZRHW 16, XYZW 16, XYZB1..5 = 12 + 4·n (the LASTBETA_* flags count the last beta).
   * NORMAL 12, PSIZE 4, DIFFUSE 4, SPECULAR 4.
   * TEXCOUNT = bits 8-11; texture coordinate i has the size from bits 16+2i: 0 → 8, 1 → 12, 2 → 16, 3 → 4 bytes.

   Unit-testable against `D3DXGetFVFVertexSize`.
2. `SetFVF`: synthesise and cache (per FVF, in `HalDevice`, like `decls_`) a `d3d9::VertexDecl` in `D3DXDeclaratorFromFVF`
   order: POSITION/POSITIONT, BLENDWEIGHT, BLENDINDICES, NORMAL, PSIZE, COLOR0, COLOR1, TEXCOORD0..7, all on stream 0.
   `GetDesc` already returns the FVF.

**Done (2026-10-21)**:
* `src/d3d9/fvf.{h,cpp}`: `fvfVertexSize` and `fvfDeclaration`. `tests/fvf_tests.cpp` compares both with d3dx9_39's
  `D3DXGetFVFVertexSize` / `D3DXDeclaratorFromFVF` for over 1300 FVFs: every position type, the LASTBETA flags, all
  component combinations, and 0-8 texture sets with mixed TEXCOORDSIZE. They are identical. Deviations: d3dx9_39 rejects
  `D3DFVF_XYZW` (it predates it), we accept it as FLOAT4 POSITION like the D3D9 runtime. XYZB5 without LASTBETA (5 weights)
  and the reserved bit 0 are rejected like D3DX does.
* `HalDevice::CreateVertexData` uses `fvfVertexSize(fvf)` when `fvf != 0`. `HalPrimitive::SetFVF` binds
  `HalDevice::fvfDecl(fvf)`, cached per FVF. Device 143 `GetFVFSize` is implemented.
* **The creator** [C, logged stack in game + decompiled, `B:\DevDrive\risen-ghidra\rvk_strip_fvf_dump.txt`]:
  `eCStrip_PS::CreateVertices` `0x6fde70`, called from `UpdateVertices` `0x6fd960`, `OnDeviceReset` `0x7010d0`,
  `eCFrustumDatabase::Execute` and `eCImageFilterScene::OnUpdateOutput`. These are strip particle systems (ribbons /
  trails). Desc = `{length = maxSegments * 64, usage 0x208 WRITEONLY|DYNAMIC, fvf 0x242 (XYZ | DIFFUSE | TEX2), pool 0,
  stride 0}`. testdemo creates two (6400 bytes each). The strip does **not** use `SetFVF`: it attaches an explicit
  declaration equal to the FVF (POSITION f3 @0, COLOR d3dcolor @12, TEXCOORD0 f2 @16, TEXCOORD1 f2 @24) and draws a
  triangle strip with 2 vertices per segment (`2n - 2` primitives). With D3D9 the stream stride was 32. **Ours was 0**: every
  vertex read vertex 0, so the strips were degenerate and invisible. Now the stride is 32.
* No strip was drawn in the `testdemo` run (a one-time log in `Primitive::Execute` for stream-0 FVF buffers did not fire),
  so the strip rendering itself is not yet seen in game. The `rvk_d3d9test` case "fvf vertex buffer" draws an 0x242 buffer
  with the FVF's declaration and stride.
* **XYZRHW (pre-transformed, POSITIONT)**: no fixed-function emulation exists. `d3d9::Device::prepareDraw` skips draws
  without a vertex and pixel shader (`skippedNoShader`). With a shader bound, the POSITIONT element reaches the shader like
  any other usage (`dcl_positiont`), which is what D3D9 does with a programmable vertex shader. No game path seen uses
  XYZRHW: SetFVF has 0 calls, and the only FVF is 0x242.

## 5. Cheap completions that are reachable but not seen

* **CubeMap `LockCubeMapLayer` / `UnlockCubeMapLayer` (2/3)** [C reachability, not in trace]. Callers:
  `eCGfxImageData::LockLayer` `0x753d30` (cube branch), `OnWrite` `0x7547e0` (lock flags `0x10` READONLY per face and level),
  and `OnRead` `0x753fb0`. `OnRead` writes the stream data to the locked pointer, so our stub's NULL `bits` could crash a
  deserialised cube image [I]. Signature: `LockCubeMapLayer(level, face, flags, int* pitch, void** bits)` and
  `UnlockCubeMapLayer(face, level)`. Implementation: the same `lockLevel(tex, level, face, nullptr, flags, &lr)` as
  `HalCubeMap::LockRect`. About 10 lines.
* **Device `CreateTextureFromMemory` / `CreateCubeMapFromMemory` (9/10)** [C].
  * Caller: `eCGfxImageData::CreateImageFromMemory` `0x750bf0`, which picks slot 9 for textures and slot 10 for cube maps
    (by `+0x24`).
  * That is reached from `eCGfxImageData::CreateImageFromFile` `0x750d20` for non-`.bik` files. `.bik` files take the
    `eCVideoPlayer` path, which uses `CreateLayers` → `CreateTexture`, already implemented.
  * The callers of `CreateImageFromFile` are `eCApplication::PlayVideo` `0x4c1b00` (always `.bik`) and
    `eCGfxAssistantAdmin::CreateImageFromFile` / `CreateImageFromMemory`, which have no callers or importers. Not seen in
    the trace.
  * D3D9: the `eCDX9Texture` / `eCDX9CubeMap` "from memory" ctors run `D3DXCreate(Cube)TextureFromFileInMemoryEx` with the
    descriptor and fill it in.
  * Ours: share the DDS path of `HalDevice::CreateImageFromMemory` (`device.cpp:219`) and wrap the result in
    `HalTexture` / `HalCubeMap`. Low priority.

**Done (2026-10-21)**:
* `HalCubeMap::LockCubeMapLayer` / `UnlockCubeMapLayer` go through the same `d3d9::Texture::lock(level, face)` as `LockRect`.
  A cube face that was locked keeps its CPU copy, and only dirty regions are uploaded. A dropped face is read back from its
  layer. `rvk_d3d9test` "cube map face lock" covers sampling, a read-only lock of another face after the upload (read
  back), then writing a face and sampling it.
* `HalDevice::CreateTextureFromMemory` / `CreateCubeMapFromMemory` share `HalDevice::textureFromDds` with
  `CreateImageFromMemory`. They follow the eCDX9 ctors (`0x74c650` / `0x73caa0`): the skip count is clamped to `levels - 1`
  when `0 < levels <= skip`. Levels 0 / D3DX_DEFAULT / D3DX_FROM_FILE mean the file's chain. They write back width, height,
  levels and format. A request that needs D3DX resampling or conversion is refused with a warning: an explicit size or
  format different from the file, or RENDERTARGET / DEPTHSTENCIL usage. A cube file passed to the texture slot, or a 2D file
  passed to the cube slot, is refused too. Still not called in the game.

## 6. `eCAPIHLSLShader::SetConstant*` (9-18): dead in the game

* Wrappers `eCGfxHLSLShader::SetConstant*` `0x501a20-0x501b40`: their only xrefs are the export table entries. No DLL
  imports them [C].
* The `eCDX9HLSLShader::vftable` devirtualisation sites `0x6bb360` (CTAB walk via `0x6bb440`), `0x6bb5a0` and `0x6bbec0`
  (material bind, also `eCShaderBase::ExecuteSpecialPass` / `ExecutePreviewShader`) use only CreateConstantTable(1),
  DestroyConstantTable(2), GetShaderType(5), GetConstantCount(7) and Execute(20) [C]. The trace has 0 calls.
* Material constants go through `SetVertex/PixelProgramRegisterF` (device 103/113).
* D3D9 semantics, for completeness: `ID3DXConstantTable::SetValue/SetMatrix/.../SetVectorArray(device = this+0x18, handle,
  ...)` (`0x74a130..0x74a6b0`; vtable `+0x60` SetValue ... `+0xa0` SetVectorArray). D3DX writes the VS or PS register file
  of the table, with D3DX type conversion and row/column-major matrix packing per constant class.
* If ever needed: resolve the handle in our `src/fx` CTAB (already parsed for `GetConstantDescription`,
  `resources.cpp:485`), convert, and call `dev().setVS/PSConstantF/I/B`. **No action needed.**

## 7. Not called in the game (keep the stubs)

Each of these has no caller outside the export table, or only editor / legacy callers, and 0 calls in the trace [C],
unless marked [I].

* **Texture 6 `ComputeNormalMap`, 7 `ConvertFrom`, 10 `SaveTextureToMemory`; CubeMap 6 `ConvertFrom`**: only through
  `eCGfxImageData::ComputeNormalMap(InPlace)` / `Convert*` / `SaveToStream`. Their callers are `eCGfxImageData::Import`,
  `eCImageResource2::OnImportData`, `CompressNormalMapToDXT5InPlace` and `eCLightmapPreprocessor` (resource import and
  lightmap baking; the shipped game loads compiled `._ximg` through device 11) [I for "never at runtime"].
  * D3D9: `D3DXComputeNormalMap(this, src, NULL, flags, channel, amplitude)`.
  * `ConvertFrom`: per level `i < min(levels)`, `D3DXLoadSurfaceFromSurface(this level i, src level i + offset, filter)`.
  * `SaveTextureToMemory`: a DDS from `D3DXSaveTextureToFileInMemory`.
* **CubeMap 7 `GenerateCubeMapMipMaps`, Volume 3**: only through `eCGfxImageData::GenerateMipMaps` `0x750bb0` ←
  `eCGfxAssistantAdmin::GenerateMipMaps` `0x756230`, which has no caller (the one `GenerateTextureMipMaps` call in the trace
  is a texture). **CubeMap 8 `GetRenderTargetData`**: only through `eCGfxImageData::GetRenderTargetData` with a cube image;
  its game caller (the savegame thumbnail) passes a texture. **CubeMap 9/10**: no cube image is ever saved.
* **Device 14/15 `CreateFontA`/`CreateSprite`, 171-180 sprite lists / text, 161 `DrawTextA`**: no engine caller.
  `eCGuiDevice2::CreateSprite` `0x516ad0` creates the engine's own `eCSprite`, and `eCGuiSystemFont2::CreateFontA`
  `0x53e220` an `eCFont` (stb_truetype). See fonts.md.
* **Device 7/8 `Create*FromFile`, 16 `GetImageInfoFromFile`**: files are read by the engine VFS and passed to device 11 or
  9/10; no caller found.
* **Device 48/49 `Set/GetMasterZBias`** (wrappers `0x50adf0`/`0x50add0`, no callers), **76 `SetZBias`**,
  **86/88/92-95** (FF lights and material), **129/131-135** (`eCGfxTextureBlock` FF texture stages), **100-102,
  107-112, 117-121** (`eCGfxVertexProgram`/`eCGfxPixelProgram` assembly programs; hal.md confirms that the device programs
  are unused): the legacy Gothic 3 fixed-function / asm material path. **Correction (2026-10-23, debug-mode.md 8.3):** 109
  `SetVertexProgram` (FVF form), 119 `SetPixelProgram(0)`, 134 `SetColorCalc`, 135 `SetAlphaCalc` *are* called by the debug
  keys' line drawing (`eCGfxMixerAdmin::DrawLineList`); they are implemented now (generated fixed-function shaders).
* **Device 143 `GetFVFSize`** (wrapper `0x50b360`, no caller), **150 `GetMethodSupport`** (`0x756130`, no caller),
  **151-158** (`GenerateLODMesh`, `CreateBox/Cylinder/Polygon/Sphere/Teapot/Text/Torus`: D3DX meshes for editor gizmos and
  `eCGfxMixerAdmin::Draw*` debug shapes), **160 `SaveBackbufferToMemory`**, **207 `CreateUVAtlas`** (lightmapper).

## 8. Situations checked (no stub on the path)

* **Savegame thumbnail** (Game.dll) [C]:
  * Creating: `gCSaveGameManager::UpdateThumbnail` `0x99c820` (from `QuickSave` and `gCHUDFileManager::UpdateScreenshot`).
    It creates a 128x128 A8R8G8B8 render target, then `CopyCurrentRTToImage(rt, LINEAR, NULL, NULL)` (Texture 11). Then
    `GetRenderTargetData(face 6, thumbnail, 0)` (Texture 15) into the 128x128 SYSTEMMEM image from `EnsureThumbnail`
    `0x99b760`.
  * Saving: `gCSaveGame::CopyThumbnail` `0x99af10` → `eCGfxImageData::OnWrite`, which reads every level with
    `LockImageLayer(level, 0x10 READONLY)`.
  * Loading: `OnRead` → `CreateLayers` + `LockImageLayer`. All implemented, but `OnRead` writes the texels under a
    `D3DLOCK_READONLY` lock, which uploaded nothing: the thumbnails were empty / garbage in Vulkan mode until a READONLY lock
    of a fresh level marked it dirty (fixed 2026-10-22, docs/research/savegame-thumbnails.md).
  * Risk to verify in game: `CopyCurrentRTToImage` reads `renderTarget(0)` outside the scene. Check that it is the
    persistent back-buffer image and not a swapchain image already presented (a black thumbnail would show up there).
* **Intro / videos**: `PlayVideo` → `.bik` → `eCVideoPlayer` + `CreateLayers` (CreateTexture, LockRect). Implemented.
* **Character screen / GUI text**: no stub involved. The text corruption is the CPU-copy drop (fonts.md H1).
* **Main menu and world loading**: only section 2 (sky).

## 9. Correction

`docs/research/hal-structs.md` (table row `SaveTextureToFile`) says "snapshots, 2 calls per run". Wrong: the 2 calls are the
sky absorption image (section 2). Snapshots go through device slot 159 `SaveBackbufferToFile` (section 3).
