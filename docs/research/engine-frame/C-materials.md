# Engine frame, slice C: the shader material system and static mesh rendering

Reverse-engineering spec of how Risen Enhanced (x64) draws a static mesh with an `eCShaderDefault` material in every pass of the
main view: call sequence, render / sampler states, every VS / PS constant register and its source, the shader permutation rules,
the data structures, and what our early native passes (`enh/native_opaque*`, `shaders/native/static_*`, removed in 23d60f2, 2026-10-29: `git show 23d60f2^:<path>`) reproduced and the N5 static ops (`src/native/static_ops*`) reproduce. Headless Ghidra on
Engine.dll (clone `clones/EngineC`), 2026-10-27; dumps `B:\DevDrive\risen-ghidra\frameC_dump1..6.txt`, `frameC_vt1.txt`, disassembly
helper `frameC_dis.py` (reads the test copy's Engine.dll). RVAs are Engine.dll (image base 0x180000000).

Legend: **[C]** read in decompilation / disassembly, **[I]** inferred, **[?]** open. Neighbouring facts: engine-gfx.md 3, 4, 8
(frame, op lists), materials.md (maps, quality), shader-permutations.md (the ubershader and its port), rt-albedo-facts.md (graph,
sampler slots, blend modes), rt-scene-facts.md 1-4 (VS registers, declarations, uv sets), native-renderer.md 9-14 (lighting
sources, native passes). Frustum lists, skinned / SpeedTree / particles, shadows, post and GUI belong to the other slices.

Engine-wide conventions used below:
* `admin` = `eCShaderMaterialAdmin` (singleton `FUN_1800adda0()`; vtable 0x97b028). `dev` = the API device `gfx+0x5f8` (gfx =
  `FUN_1800956c0()` = `admin+0x38`); `mixer` = `gfx+0x5e8` (`eCGfxMixerAdmin`). HAL slot numbers = vtable offset / 8
  (`src/hal/api_generated.h`).
* **Constant pool** (`eCShaderConstantPool`, 0x30 bytes) [C]: `+0` type (0 = VS -> slot 103 `SetVertexProgramRegisterF`, 1 = PS ->
  slot 113 `SetPixelProgramRegisterF`), `+4` first register, `+8` register count, `+0xc` / `+0x10` dirty range [min, max) (relative),
  `+0x18` float4 data, `+0x20` bCString HLSL name, `+0x28` byte "initial allocation count", `+0x29` byte allocation counter.
  Writers mark the dirty range; `FUN_1806b5d20(pool, force)` (0x6b5d20) uploads `[min, max)` (whole pool when `force`) and resets
  the range. The D3D9 register file is global device state: a register keeps its last value across shaders.
* **Sampler slot state** (0x28 bytes, used by the admin's slots and by a material's sampler pool) [C]: `+0` AddressU, `+4`
  AddressV, `+8` MINFILTER, `+0xc` MAGFILTER, `+0x10` MIPFILTER, `+0x14` byte SRGBTEXTURE, `+0x18` MIPMAPLODBIAS, `+0x20` image.
  `FUN_1806bc340(slot, stage, image)` (0x6bc340) issues `SetSamplerState` (slot 130) for states 1, 2, 5, 6, 7, 8, 11 in that
  order, a value `0xABABABAB` meaning "the global default" `gfx+0x640` (the configured texture filter: linear or anisotropic,
  materials.md 6.1), then `eCGfxImageData::Execute` (0x7551c0) at that stage; a null image binds the admin's **ErrorImage** (`admin
  +0x348`, a 1x1 0xFFFF00FF magenta texture made in `PostInitialize` 0x6ba9d0). MAXANISOTROPY is set once per stage at device
  init, never per bind (materials.md 6.1).

---

## 1. Call sequence of a static mesh op, per pass

### 1.1 Op creation (`eCRenderContextMesh::RequestRenderOps` 0x5c75e0) [C]

Inputs: database, fade `param_2` (`CalculateFadeOut`, native-renderer.md 10), request flags `param_3`. The mesh resource
(context `+0x28`, chosen by `UpdateLoD` 0x5c74b0) must be loaded (`eCResource2 +0x38 == 3`); every sub-mesh's material is
`RequestData`'d. View-depth key = `(dot(db plane +0x94..+0xa0, entity sphere centre) - radius) * db+0xa4`, clamped to 24 bits.

* `fade != 1`: request &= `0xffe5` (no solid 2, no z pass 8 / 0x10).
* Per sub-mesh by its material's shader: **IsRefracted** (vtbl +0x1a8) -> `& 0xffe1` (refracted 0x20 only); else **IsBlended**
  (vtbl +0x198) or fade != 1 -> `& 0xffc5` (alpha 4 / 0x200); else `& 0xffdb` (solid 2, z pass 0x18, 0x200).
* Bit 2 -> solid list `db+0x1500` (instanced resources `res+0xa0`: once per database frame, desc `0xffffff00 | sub-mesh`, then
  `AddRenderInstance` 0x7602a0 adds this entity: matrix, material switch, illumination, overlay flags); bit 4 -> alpha list
  `+0x14e8` (op `+0xc` = fade); bit 0x20 -> refracted `+0x1578` (fade); bit 0x200 -> **solid list again with fade** (the blended
  layers drawn inside the solid pass: terrain / level-mesh layers, rt-albedo-facts.md 1.4; who sets 0x200 [?], slice A).
* Z pass (`(request & 0x18)`, context `+0x34 == 0`): one op per sub-mesh whose material is neither blended nor refracted, or a
  single op with desc 0xff (whole mesh) when every sub-mesh qualifies. **Instanced resources get per-entity z-pass ops** (not
  instanced). Context `+0x34 != 0` (sub-mesh quadtree): per visible node, desc `node << 8 | sub-mesh`.
* Blend mode decides the lists through `IsBlended` 0x6d2ef0: `TradeAlphaAsSolid` (`+0x79`) -> false; mode 0 -> `HasOpacity` (Opacity
  proxy guid valid, 0x6c9d00) or `IsRefracted`; mode 1 (Masked) -> false; other modes -> true.

### 1.2 Z pre-pass (main view, `eCFrustumDatabase::Execute` 0x66e990, `db+0x1590`) [C]

```
GetRenderTarget, SetRenderTarget(depth-map image of the eSRenderTargetDesc, scene depth), SetViewport, Clear(7, 0xFFFFFFFF, 1.0, 0)
admin.BeginZPass(NULL depth map, zwrite=true, colorWrite = depth-map target present)            0x6b9720
  qsort z list (+0x1518) by op +0x10 (view depth, ascending = front to back)                     cmp 0x66e670
  per op: ctx vtbl +0x18 RenderZPass (0x5c7410) -> eCMeshResource2::RenderZPass(world +0x60, sub-mesh, node, instanced=false) 0x7603b0
admin.EndZPass                                                                                   0x6b9910
EndScene?, restore RT / viewport; admin s2 slot (+0x270) := the desc's depth-map image (FUN_1806bc540)
```
`BeginZPass` [C]: refuses while in material / z mode (`admin+0x311` / `+0x312`); `+0x313` = zwrite; `BeginRenderStateBlock` (208);
`SetCurrentRenderStates({ref 0, func 8, alphatest 1, blend 0, colorWrite = alphaWrite = colorWrite arg, src 2 dst 1, depthWrite
zwrite+1, renderMode 0, depthFunc 4})`; `SetMasterFogging(1)` (56); `SetMasterCullingMode(0)` (40); `admin+0x314` = special pass
type = depth map `vtbl+0xf8` (0 for the null z pre-pass); clears the active shader / pool caches (`ms_pActiveShader`,
`ms_pActiveShaderConstants` 0xc62978, `DAT_180c62990`, the shader caches `0xc62950/58`, the lighting pool's cached list); forces a
full upload of the admin VS and PS pools. With a depth map argument the colour writes are on unless its format (`+8`) is 3..6.

`eCMeshResource2::RenderZPass` 0x7603b0 [C]: needs `res+0x38 == 3` and a visual. Draw description = indexed, triangle list. **It
merges consecutive non-masked sub-meshes into one draw** (first sub-mesh's firstVertex / firstIndex, summed vertex counts and
`indexCount / 3`), sets the world matrix (shader vtbl +0x208, slot 0) and global alpha 1.0 (+0x210) once, and draws a **masked**
sub-mesh on its own; blended / refracted / opacity materials are skipped (`TradeAlphaAsSolid` keeps them). A draw = shader
`ExecuteSpecialPass(instanced)` (vtbl +0x1d8, 0x6d1cd0) then `eCGfxVisual::Execute` 0x5069a0. Node ops draw the node's index range.

`ExecuteSpecialPass` 0x6d1cd0 [C]: re-selects VS c0 (depth-bias VP if the shader's `+0x78`, see 3.1); **mode 1 (Masked): PS
c5.w = maskReference / 256**; version object `shader + 0x258 + 8 * min(admin+0x170, shader+0x68)`; `FUN_1806d9b40`: type =
`admin+0x314`, slot `type * 2 + instanced`; compiles on demand (CompileSpecialPass, vtbl +0x50), sets VS and PS
(`FUN_1806bbec0` 0x6bbec0 = activate a pool element's HLSL shader, cached per stage in `0xc62950[2]`), then `FUN_1806d9a70` 0x6d9a70
= shader `ExecuteShaderConstants(vs, ps)` + upload of the lighting pool. **No render state is set per draw in the z pass**: everything
comes from `BeginZPass`'s masters and the visual (cull, 2.1).

`EndZPass` [C]: `EndRenderStateBlock` (209), `SetMasterRenderMode(0)`, `SetMasterDepthTesting(0)`, master depth write released,
`+0x312 = 0`, `+0x314 = 6` (no special pass).

### 1.3 Solid pass [C]

```
eCIlluminationAdmin::UpdateShadowMask (s0 image into admin +0x298, slice E); BeginScene
admin.BeginMaterial                                                                                 0x6b9d20
  BeginRenderStateBlock, SetMasterFogging(1), SetMasterCullingMode(0), clear caches, force admin pool upload,
  ExecuteShaderSamplers (0x6b7bc0): s0 shadow mask (+0x298), s1 refraction (+0x248), s2 depth map (+0x270), s4 / s5 lightmaps
  (+0x2c0 / +0x2e8); s3 is never bound here; admin +0x311 = 1
eCModuleAdmin::DoBeginRenderSolid (unless db +0x15a0)
qsort solid list (+0x1500) by the 64-bit key (op +0x10 depth | op +0x14 sort id << 32)                cmp 0x66e6a0
per op: ctx vtbl +0 Render -> eCRenderContextMesh::Render(op) 0x5c7110
```
`eCRenderContextMesh::Render` 0x5c7110 [C]: instanced op (desc `>> 8 == 0xffffff`): qsort the render instances
(`SortRenderInstances`) once per frame, `ExecuteRenderInstances(sub-mesh)` 0x7607b0. Else illumination = entity `+0xf8` + 0x78
(or null), overlay flags from entity `+0x28` (0x400000 -> 1 burned, 0x200000 -> 2 frozen, 0x800000 -> 4 highlighted), fade = op
`+0xc`, then `eCMeshResource2::Render(world = entity +0x60, sub-mesh or -1, node, switch = mesh PS +0x20, illum, &fade, flags)`.

`eCMeshResource2::Render` 0x760ca0 [C]: `combi = BindLightmaps(1)` 0x764220 (for `res+0x90` / `+0x98`: queue / cache the image;
**if a lightmap resource exists but its image data is not loaded: combi 0 and nothing more is bound**; else
`admin.SetLightmap(image, i)` 0x6b80d0, which rebinds stage 4 + i immediately in material mode, combi 1). Then
`eCSubMesh::Render` per sub-mesh.

`eCSubMesh::Render` 0x5c7eb0 [C]: material = sub-mesh `+0x38` (queued / cached), shader = material `+0x70`. Unless instanced:
`SetGlobalAlpha(fade, 0)` (vtbl +0x210 -> VS c32.w) and `SetWorldMatrix(world, 0)` (+0x208 -> VS c33..c35). Draw description in
the visual's primitive (`visual+0x10`, `GetDrawCallDescription` slot 12): indexed, type 4, minIndex = `+0x30`, numVertices =
`+0x34`, startIndex = `+0x28`, primCount = `+0x2c / 3`, baseVertex 0 (node ops: the node's sub-range). `n = GetPasses(switch,
illum, combi, instanced=false, flags)`; per pass `ExecutePass(i)` (vtbl +0x1d0, 0x6d2060) and on success `eCGfxVisual::Execute`.

`eCShaderBase::GetPasses` 0x6d1ee0 [C]: an invalid shader (`+0x2a8`) uses its fallback material (`+0x2a0`) when
`admin+0x331` (fallback enabled), else the error indicator (overwrite indicator 9) when `admin+0x330`; `admin+0x31c`
(overwrite indicator, 0 in game) adds the indicator passes. `OnGetPasses` 0x6d3170: version = `min(admin+0x170, shader+0x68)`
falling back to a lower existing one; version `vtbl+0x18` = **`eCShaderVersionDefault_3_0::GetPasses` 0x6cc970**; then the
overlay materials' passes (flag 1: `+0x88` burned, 2: `+0x80` frozen, 4: `+0x90` highlighted), each with `RequestData`.

`eCShaderVersionDefault_3_0::GetPasses` 0x6cc970 (vtable 0x9d7e40) [C] appends 0x20-byte `eSShaderMaterialPass`es
(5.4) to `eCShaderBase::ms_arrShaderPasses` (0xbfcae8):
1. `EnableAlphaZWrite` (`+0x2d8`) and IsBlended: a **z-fill pass**, kind 2, no lights, no lightmap.
2. No illumination or no light list: one pass, kind 0, lightmap = combi, no sun, 0 lights.
3. Else `L = list+0x34 (dynamic) + illum+0xd (static)` lights in passes of up to 3: the first pass is kind 0 with
   `sun = list[0] && !illum[0xc]` and lightmap = combi; further passes kind 1 (additive), no sun, no lightmap. First light index of
   each pass = pass `+0x18`.
4. Transformation 11 (SpeedTree billboards): every pass duplicated, VS variant 1 / 2 (bits 10 / 11).

`eCShaderBase::ExecutePass` 0x6d2060 [C]: `SetSwitch(pass+0x1c)` on the shader (or on its switch elements `+0x248/+0x250`);
re-selects VS c0 for the shader's depth-bias flag (3.1); version pass `vtbl+0x20` = **`FUN_1806cc610` (Execute)**; failure sets
`+0x2a8` (shader invalid).

`FUN_1806cc610` (eCShaderVersionDefault_3_0 Execute) [C]:
```
vsIdx = nonInstanced * 3 + billboardVariant;  psIdx = lights + 4 * (kind + 3 * (lightmap + 2 * sun))     (4.2)
compile VS (FUN_1806cc000) / PS (FUN_1806cb8b0) on demand
FUN_1806cd1d0(pass): lighting constants into the lighting pool (3.3)
activate VS (version +0x1a8 + 16 vsIdx), PS (+0x208 + 16 psIdx)                                        FUN_1806bbec0
states (2.2): depthFunc 4; depthWrite = IsBlended && !IsRefracted ? 1 : 0 (master); fade = GetGlobalAlpha(0) for non-instanced
  default VS else 1; kind 0 / 1: FillBlendingRenderStates(additive = kind 1, fade) + colour / alpha write on; kind 2: fixed
  z-fill states; renderMode 0; admin.SetCurrentRenderStates                                           0x6b7c50
FUN_1806d9a70: shader.ExecuteShaderConstants(vs, ps) (0x6d2f90) + upload the lighting pool
```
`eCShaderBase::ExecuteShaderConstants` 0x6d2f90 [C]: upload the global shader constants (`GetGlobalShaderConstants` vtbl
+0x1b8 = `eCShaderDefault::ms_pGlobalShaderConstants` 0xc62960: VS c31..c231; forced when it changed); a new active shader
re-uploads its whole VS pool (`+0x98`, c232..) and PS pool (`+0xc8`, c6..); **material samplers**: for each sampler-pool slot
`k < +0x21b` whose register `9 + k` the PS uses (pool element `+0x38` = sampler mask) and that is dirty or the shader is new:
`RequestData` / cache the image resource (entry `+0x20`), bind `image +0x70` (`eCGfxImageData`) or
`eCImageResource2::GetPlaceHolderImageData` (the 4x4 DXT5 while streaming) with the entry's states; then
`ms_pActiveShader = this` and the admin's VS / PS pools (dirty ranges).

`eCGfxVisual::Execute` 0x5069a0 [C]: `SetRenderMode(+0x18)` (70), `SetCullingMode(+0x1c)` (72), `SetDepthTesting(+0x20)` (73),
`SetDepthWrite(+0x24)` (74) — all overridden by masters where set — then the attached `eCGfxShader` (none for meshes) and
`eCAPIPrimitive::Execute` (vtbl +0x78) = the draw.

### 1.4 Instanced static meshes (`eCMeshResource2::ExecuteRenderInstances` 0x7607b0) [C]

* A resource is instancing-capable when its declaration has `stream 3, offset 0, FLOAT1, TEXCOORD4` (`SetVisual` 0x760090):
  `res+0xa0 = 1`, stream 3 = `admin.GetHardwareInstancingVertexData()` (0x6b6c80: a shared 0x400-byte VB of 256 floats 0, 1, 2 ...
  255, stride 4), stream 3 frequency `0x80000001` (INSTANCEDATA | 1) set once.
* Non-instanced path (not instancing-capable, < 2 instances, or lightmapped `combi == 1`): `eCSubMesh::Render` per instance
  (matrix, switch `+0x40`, illum `+0x48`, flags `+0x50`, fade 1).
* Hardware path: up to **50 instances per draw** (shader vtbl +0x200 `GetMaxInstanceCount` = 0x32); instance i of the batch
  writes `SetWorldMatrix(m, i)` -> VS `c33 + 4i .. c35 + 4i`, `SetGlobalAlpha(1, i)` -> `c32 + 4i .w`. A batch breaks when the
  light list pointer, the switch, the overlay flags or the illumination's outdoor factor (`illum+8`) differ, or an instance has
  static lights / static SH (`illum+0xd`, `+0x90`: such instances draw alone). Flush: streams 0..2 frequency `n | 0x40000000`
  (INDEXEDDATA), draw description of the sub-mesh, `GetPasses(switch, illum of the last instance added (all members share light
  list and f), combi 0, instanced=true, flags)`, passes, then frequency 1 again.

### 1.5 Refracted and alpha passes [C]

After the solid list, the debug list and the modules / decals / billboards (slice A): if the caller's desc has a refraction
target and `gfx+0x5b8`: `EndMaterial`, `CopyCurrentRTToImage` (HAL slot 11) into `desc+0` image, admin s1 (`+0x248`) := it,
`BeginMaterial` (binds s1). `DoBeginRenderAlpha`; refracted list (`+0x1578`, unsorted) via ctx vtbl +0x10 (`RenderRefracted`
0x5c7400 -> 0x5c7110); alpha list (`+0x14e8`) qsorted by 0x66e600 (op +0x14 priority band `>= 0x3e8 << 20`, then view depth) via
vtbl +8 (`RenderAlpha` -> 0x5c7110); `DoEndRenderAlpha`; list `+0x1548`; `EndMaterial` (0x6b9e40: `EndRenderStateBlock`,
`SetMasterRenderMode(0)`, `SetMasterDepthTesting(0)`, master depth write released, `SetTextureNULL(0..5)`); admin s1 released.
The per-op path is the same as the solid pass (1.3); only the op's fade and the material's blend mode differ.

---

## 2. State tables

### 2.1 Fixed per static mesh [C]

| state | value | source |
|---|---|---|
| culling | **D3DCULL_CW** (2) for every mesh visual (`eCGfxVisual` default 1 = none, overwritten to 2 in `LoadDataV2` 0x75e080 / `LoadDataV1`); no master cull (BeginMaterial / BeginZPass release it). `XFX_DOUBLESIDED` only flips the normal in the PS | visual `+0x1c` |
| fill | solid (3), master = `admin+0x340` (wireframe toggle) when the state struct says 0 | visual `+0x18` |
| depth test | visual 4 (LESSEQUAL), always overridden by a master: 4, or 3 (EQUAL) for masked materials at fade 1 | 2.2 |
| depth write | visual 2 (on); master 1 (off) for blended non-refracted materials, 2 in the z pre-pass and the z-fill pass | 2.2 |
| fixed-function fog | master disabled (`SetMasterFogging(1)`): fog is in the shaders | Begin* |
| **alpha test** | **always master-disabled**: `SetCurrentRenderStates` calls `SetMasterAlphaTesting(1, data)` (disassembly 0x6b7d0c: `mov edx, 1`). Masked cut-outs come from the z pre-pass's `clip` + depth EQUAL, SpeedTree billboards clip in the colour PS too | 0x6b7c50 |
| stencil, depth bias states, scissor, clip planes | not touched by the material path [C for absence in these functions] | |

### 2.2 `eSShaderRenderStates` per blend mode (`FillBlendingRenderStates` 0x6d0430 + `FUN_1806cc610`) [C]

Struct (0x24): `+0` alpha ref (int 0..255), `+4` alpha func, `+8` alpha test (1 off, 2 on), `+0xc` blend enable, `+0xd` colour
write, `+0xe` alpha write, `+0x10` src, `+0x14` dst (eEBlending = D3DBLEND), `+0x18` master depth write (0 none, 1 off, 2 on),
`+0x1c` master render mode, `+0x20` master depth func. `SetCurrentRenderStates` 0x6b7c50 applies: PS c5.w = ref / 256 when `+8 ==
2`; `SetMasterAlphaTesting(1)`; `SetAlphaBlending(+0xc)` (60); `mixer.SetMasterBlending(src, dst)` only when blending is on;
`mixer.SetMasterDepthWrite(+0x18)`; `SetMasterRenderMode(+0x1c or admin+0x340)` (36); `SetMasterDepthTesting(+0x20)` (42);
`SetColorWriting(+0xd)` (62); `SetAlphaWriting(+0xe)` (64).

Enum `eEShaderMaterialBlendMode` (`eCShaderBase::StaticConstructor` 0x6cef70) and the base pass (kind 0; "add" = kind 1 additive
light pass):

| mode | name | blend | src / dst (kind 0) | kind 1 dst | alpha test word | depth func | depth write master |
|---|---|---|---|---|---|---|---|
| 0 | Normal, no opacity node (or refracted), fade 1 | off | One / Zero | One (blend on) | off | 4 | 0 (visual: on) |
| 0 | Normal with opacity node or fade < 1 | on | SrcAlpha / InvSrcAlpha | One | off | 4 | 1 if IsBlended (opacity node), else 0 |
| 1 | Masked, fade 1 | off | One / Zero | One (on) | on, GREATER, ref = MaskReference (`+0x58`) -> c5.w | **3 EQUAL** | 0 |
| 1 | Masked, fade < 1 | on | SrcAlpha / InvSrcAlpha | One | on, ref = MaskReference x fade | 4 | 0 |
| 2 | AlphaBlend | on | SrcAlpha / InvSrcAlpha | One | off | 4 | 1 |
| 3 | Modulate | on | DestColor / SrcColor (2x modulate) | same | off | 4 | 1 |
| 4 | AlphaModulate | on | One / InvSrcAlpha | same | off | 4 | 1 |
| 5 | Translucent | on | One / InvSrcColor | same | off | 4 | 1 |
| 6 | Darken | on | Zero / InvSrcColor | same | off | 4 | 1 |
| 7 | Brighten | on | SrcAlpha / One | same | off | 4 | 1 |
| 8 | Invisible | on | Zero / One, alpha test on with func NEVER | same | (on, NEVER, 0) | 4 | 1 |

"Alpha test word" only feeds c5.w (the fixed-function test is always off, 2.1). Refracted materials (water) are mode 0 with
IsRefracted: drawn **unblended** (One / Zero) with depth write on: the PS mixes the refraction itself (`Refract`, engine-gfx.md 8).
Kind 2 (z-fill of `EnableAlphaZWrite` blended materials): blend off, colour and alpha write **off**, depth write master 2, depth
func 4. Colour / alpha write are on for kinds 0 and 1. Z pre-pass: blend off, colour = alpha write = "depth-map target present",
depth write master 2, depth func 4, alpha test off, no per-draw states.

### 2.3 Samplers [C]

| stage | content | set by | U / V | MIN / MAG / MIP | sRGB | bias |
|---|---|---|---|---|---|---|
| s0 | shadow mask + SSAO (`.a` sun, `.r` AO) | admin `+0x298` (`SetShadowMask` 0x6b9fa0, `UpdateShadowMask`) | clamp | point / point / none | 0 | 0 |
| s1 | refraction copy (`smpRefract`) | admin `+0x248` (Execute's copy) | clamp | point / point / none | 0 | 0 |
| s2 | linear depth map (`smpDepthmap`, `.r * far`) | admin `+0x270` (Execute after the z pass) | clamp | point / point / none | 0 | 0 |
| s3 | light index map (`smpLightMap0`) | never bound by this path [C absence] | | | | |
| s4 / s5 | lightmaps (`_1.dds` / `_2.dds`, `.a` = ambient direction x / y) | `BindLightmaps` -> admin `+0x2c0` / `+0x2e8` | clamp | linear / linear / linear | 0 | 0 |
| s9 + k | material `g_SamplerData[k]` | shader sampler pool `+0xf8` | node TexRepeatU / V (`+0x58` / `+0x68`: 0 wrap, 1 clamp, 2 mirror) | **MIN = global default** (`gfx+0x640`, TextureFilterMode), MAG linear, MIP linear | node `+0x8c` | 0 |

Defaults from the admin ctor 0x6b8ec0 / `Create` 0x6ba780. Cube samplers (`eCColorSrcCubeSampler`) take a pool slot too; their
states are the pool entry's (not set by `UpdateSampler` [I]). Unused stages keep their previous binding; `EndMaterial` unbinds 0..5.

---

## 3. Constant register maps (static mesh, `ge_default_phong.fx` / `ge_default_vs.fx`)

### 3.1 Vertex shader [C]

| reg | HLSL | written by (RVA) | value |
|---|---|---|---|
| c0..c3 | `g_vViewProjection` | `SetCamera` 0x6ba1d0 (via `eCGfxCamera::Execute`) | transpose of camera `+0xe0` (view x proj); **with the material's UseDepthBias (`+0x78`)**: transpose(camera `+0xa0` (depth-bias projection) x camera `+0x20` (view)), switched in `ExecutePass` / `ExecuteSpecialPass` / `EnableDepthBias` 0x6b6c20 (admin `+0x310`) |
| c4..c7 | `g_vInvProjection` | `SetCamera` | transpose(inverse(camera `+0x60`)) |
| c8..c10 | `g_vInvView` (3x3) | `SetCamera` (`FUN_180225ec0`) | transposed 3x3 of camera `+0x120`; `.w` untouched |
| c11 | `g_vCamPosAndFar` | `SetCamera` | camera `+0x150..+0x158`, `.w` = the application camera's far (`app+0x38` or `app+200`, `+0xc0`) |
| c12 | `g_vVegetationData1` | `SetVegationWindData` 0x6b7f30 | grass wind (not read by static VS) |
| c14..c22, c23..c25, c26..c29 | SpeedTree wind / leaf matrices / angles | `SetSpeedTreeData` 0x6b7df0 | slice B |
| c30 | `g_vFogSettings` | `SetFog` 0x6b9fc0 | = PS c4 |
| c31 | `g_vInstanceMisc` | `SetMorphTargetWeights` 0x6c9da0 / `SetVegationAlpha` 0x6c9f20 | morph weights / grass dissolve (not read by static VS) |
| c32 (+4i) | `g_vInstanceData[4i]` | `SetGlobalAlpha` 0x6c9e30 | `.w` = fade (non-instanced: op fade; instanced: 1.0); `.xyz` = whatever SpeedTree wrote last |
| c33..c35 (+4i) | `g_vInstanceData[4i+1..3]` | `SetWorldMatrix` 0x6c9e00 (`FUN_180238390`) | rows of the 3x4 object -> world (`m[0..2][r]` per register r: the row-vector matrix's columns) |
| c232..c251 | `g_vShaderData[20]` | the material graph's tex-coord nodes (3.4) | uv transforms, allocated per shader |

Pools: the admin VS pool `+0x228` (c0..c30, 31 registers), the global pool `ms_pGlobalShaderConstants +0x20` (c31..c231, 201
registers; created by the first `eCShaderDefault` ctor 0x6c7860), the shader's own VS pool `+0x98` (c232.., 20; `Create` 0x6c7ff0
names it `g_vShaderData`). Static VS read c0..c3, c11, c30, c32.w, c33..c35, c232+ (and c8..c10 only for transformation 7,
screen-aligned billboards).

### 3.2 Pixel shader, per view / per pass [C]

| reg | HLSL | written by | value |
|---|---|---|---|
| c0 | `g_CamPosAndFar` | `SetCamera` | camera position, far (= VS c11) |
| c1 | `g_ViewToScreenPos` | `SetCamera` (viewport from `dev` slot 31) | (0.5, -0.5, 0.5 + 0.5 / vpW, 0.5 + 0.5 / vpH) (`DAT_1809f1630` = 0.5: half-pixel offset; native-renderer.md 10 wrote 1 / vpW) |
| c2 | `g_Reserved` | nobody | |
| c3 | `g_FogColor` | `SetFog` (from `eCGfxFog::Execute` 0x501500) | fog `+0x1c` D3DCOLOR as (R, G, B) / 255, `.w` 0; all 0 when fog `+0x18 != 1` |
| c4 | `g_FogSettings` | `SetFog` | (-start k, k, density, 0) with k = density / (end - start) (fog `+0x28` start, `+0x2c` end, `+0x30` density); zeros when disabled or density <= 0 |
| c5 | `g_CamObstacleAndMaskReference` | `SetCamObstacleDistances` 0x6b7f90 (from `SetCamera`, app camera `+0xe4`), `SetCurrentRenderStates`, `ExecuteSpecialPass` | `.x` = 1/150, `.y` = (d - 50) * (-1/150) with d = camera obstacle distance `.x`; `.z` unset; `.w` = alpha ref / 256 (`DAT_1809f1560` = 1/256) |
| c6..c13 | `g_ShaderData[8]` | the graph's constant nodes (3.4) | shader PS pool `+0xc8` (base 6, 8 registers, at most 7 handed out) |

### 3.3 Pixel shader lighting (the lighting pool `DAT_180c62968`: PS c14..c37, created by `eCShaderDefault::CreateShaderVersion` 0x6c9ab0) [C]

Filled per pass by `FUN_1806cd1d0` 0x6cd1d0 from the pass's illumination (native-renderer.md 10 has the fields):

| reg | value | refreshed |
|---|---|---|
| c14 | `g_IndoorAmbientColor` = list `+0x128` (rgb) | when the light list changes |
| c15..c21 | `g_AmbientLight[7]` (`FUN_1806b5bb0` 0x6b5bb0): list `+0xb8 + 16k` x f (`illum+8`, all four components scaled), `.w` of c15..c17 += (1 - f) x indoor.rgb, + illum `+0x94 + 16k` when `illum+0x90` | when list / f / static-SH flag change, or every pass with static SH |
| c22..c24 | `g_DirLight`: list `+4`, `+0x14`, `+0x24` (diffuse x intensity, specular, towards the sun) | when the list changes |
| c25 + 4j .. +3 | `g_PointLights[j]` (pos + 1/range², diffuse, specular, shadow mask channel): light `first + j` of the pass; static lights first (`illum + 0x10 + 0x40 i`, `i < illum+0xd`), then dynamic (`list + 0x38 + 0x40 (i - static)`) | every pass, j < the pass's light count |

### 3.4 Material graph constants (`UpdateShaderConstants` per element, `eCShaderBase::UpdateShaderConstants` 0x6d28a0) [C]

Allocation happens when a permutation is compiled (each `Compile*` resets the counters `+0xc1` / `+0xf1` / `+0x21b` and walks the
graph, so the register a node gets is fixed for that shader). Each node takes the next register of its pool (`counter + needed <
size`), else -1 (then its snippet has no constant). The generated HLSL references `g_ShaderData[i]` / `g_vShaderData[i]` /
`g_SamplerData[k]` by the allocated index.

| node | pool | data | snippet (GetImplementation / Get*Implementation) | time |
|---|---|---|---|---|
| `eCColorSrcConstant` 0x6bfe90 | PS | colour `+0x50..+0x5c` | `g_ShaderData[i]` | static |
| `eCColorSrcConstantAnimation` 0x6c0630 | PS | `eCColorScale` (`+0x50`) at t / period (`+0x48`), wrapped | `g_ShaderData[i]` | SetTime 0x6c0480 |
| `eCColorSrcConstantSwitch` 0x6c0e20 | PS | the switch's colour (`+0x50` array, `+0x58` count) | `g_ShaderData[i]` | SetSwitch |
| `eCColorSrcCamDistance` 0x6bda10 | PS | (`+0x5c`, `+0x58`, `+0x60`) | `pow(D * c.x + c.y, c.z)`, D = CAMDISTANCE / SCENEDEPTH / SCENEDEPTH - PROJDISTANCE (`+0x50`) | static |
| `eCColorSrcCamObstacle` 0x6be860 | PS | `SetShaderConstants` | **only in the z pass at quality 2**: `lerp(saturate(CAMDISTANCE * g_CamObstacleDistances.x + .y), 1, saturate(dot(V, V) + c.z))`, V = `XFX_PS_VIEWPOS.xy * .w * 0.2 * c.xy`; else `1.0f` | static |
| `eCColorSrcFresnel` 0x6c2280 | PS | (`+0x58`, `+0x5c`, `+0x60`, 1) | `c.x + c.y * f`, f = `(1-FRESNEL)`, `(1-FRESNEL)^2` or `pow(1-FRESNEL, c.z)` (`+0x50`) | static |
| `eCColorSrcMask` 0x6c2d70 | PS | `.x` = threshold `+0x48` | `( a > thr ) ? 1 : 0` | static |
| `eCColorSrcSampler` 0x6c54a0 | sampler | pool slot k; `UpdateSampler` 0x6c3ab0 writes the slot state (2.3) and image 0 | `tex2D( g_SamplerData[k], XFX_PS_TEXj )` (j = its uv set index, `+0xd0`, from `GetDeclarations` 0x6c5540: `XFX_VS_TEXj` = its tex-coord source's VS expression or `XFX_VS_INPUT_TEX<n>` (`+0xc8`)) | type 1 (`+0xd4`, animated): SetTime 0x6c53d0 picks image `int(t * fps(+0x70)) % count` |
| `eCColorSrcCubeSampler` 0x6c17b0 | sampler | slot + cube image (`+0x50`) | `texCUBE( g_SamplerData[k], XFX_PS_REFLECT )` | |
| `eCTexCoordSrcScale` 0x6e0630 | VS | `.xy` = `+0x48` | VS `( uv * g_vShaderData[i].xy )` | static |
| `eCTexCoordSrcScroller` 0x6e0f20 | VS | `.xy` = frac(t x speed `+0x58` x dir `+0x50/+0x54` / 1024) x 1024 + offset `+0x48/+0x4c` | VS `( uv + g_vShaderData[i].xy )` | SetTime 0x6e0c00 |
| `eCTexCoordSrcRotator` 0x6df680 | VS (3 regs) | 3x3 rotation (constant / speed x t / sine wobble, `+0x50` mode) with pivot `+0x64/+0x68` | VS `mul( float3( uv, 1 ), M ).xy` | SetTime 0x6df170 |
| `eCTexCoordSrcOscillator` 0x6dd590 | VS (3 regs) | 3x3 from sine / saw / random per axis (`+0x50`, `+0x60`), amplitude, frequency, phase, pivot | VS `mul( float3( uv, 1 ), M ).xy` | SetTime 0x6dcf20 |
| `eCTexCoordSrcBumpOffset` 0x6dc1e0 | PS | (h, -h, -0.5 h, 0.5 h), h = `+0x48` | PS `(( H * c.xy + c.zw ) * XFX_PS_TANGENTEYE + XFX_PS_TEXj )` (parallax offset; H = its colour proxy, swizzle forced to `.a` when unset) | static |
| `eCTexCoordSrcReflect` | none | | PS `( REFLECT / WORLDEYE / WORLDNORMAL / TANGENTNORMAL / TANGENTEYE .xy * 0.5f + 0.5f )` (`+0x50`) | |
| `eCTexCoordSrcColor` | none | | PS `float2( a, b )` from two colour proxies | |

Combiner / blend / mask / vertex colour nodes: rt-albedo-facts.md 1.1. Every proxy wraps its child's snippet with the swizzle
table (`+0x28`: "", .rgb, .r, .g, .b, .a) and operator table (`+0x2c`: "", -, 1-, saturate, ceil, floor, abs, frac) (`eCColorSrcProxy::
GetImplementation` 0x6c36b0).

**Time** [C]: `eCShaderMaterialAdmin::Process` 0x6bb250 (per frame): `t = eCTimer +0x10 (uint) / 1000` and `SetTime(t)` (vtbl +0x110)
on every registered timed element (`admin+0x58` array, `RegisterTimedShaderEllement` 0x6b7b00: constant animation, animated
samplers, oscillator, rotator, scroller, light streaks). Whether `eCTimer +0x10` is game or real time [?] (ocean.md 4.5 found the
water follows the game clock). It also re-reads `/Engine/Render/Shader/Quality` every frame (a change reloads the pool).

---

## 4. Shader selection / permutation rules

### 4.1 Sources [C]

`eCShaderDefault::GetShaderFileName` 0x6ca040 = `ge_default_phong.fx` (eCShaderLeafs: `ge_default_leafs.fx`). Compile through
`FUN_1806bbff0` -> `CompileShaderPoolEllement` 0x6b8140 (shader-permutations.md 8) with version 3, system dependencies `0x100 <<
quality` (`GetSystemDependencies` 0x6b6c60), profile 7 (ps_3_0) / 0xb (vs_3_0), flags 0. Macro list = the pass's engine macros +
the material's `GetDeclarations` + `eCShaderBase::SetupShaderMacros` 0x6d06b0 (dedupe, renumber, sort) + `XFX_QUALITY`
(`admin+0x350`, 0x6b8ac0).

### 4.2 Main passes (eCShaderVersionDefault_3_0, vtable 0x9d7e40) [C]

| shader | function | engine macros | material declarations | stored at |
|---|---|---|---|---|
| VS (6) `FUN_1806cc000` | `VS_Default` | `XFX_INSTANCED=true` when instanced; `XFX_ST_BILLBOARDS_VERT1/2` for variant 1/2; `XFX_SHADERMODEL_3` | `GetDeclarations(normalMap = true)` | version `+0x1a8 + 16 (nonInstanced * 3 + variant)` |
| PS (48) `FUN_1806cb8b0` | `PS_Default` | unless DisableLighting: kind 0 adds `XFX_AMBIENTLIGHT`, `XFX_DIRLIGHT` (sun), `XFX_LIGHTMAP` (combi 1); always `XFX_PNTLIGHTCOUNT=n`, `XFX_AMBIENTLIGHTTYPE=2`; `XFX_SHADERMODEL_3` | `GetDeclarations(true)` | `+0x208 + 16 (n + 4 (kind + 3 (lm + 2 sun)))` |

Kind 1 (additive light pass) and kind 2 (z-fill) PS therefore have no ambient, no sun, no lightmap (kind 2's colour writes are off).
**Fog is not removed in additive passes** (only `DisableFog` turns it off), so a second light pass adds a fogged contribution [I
from the macro list]. `CompileAll` (`FUN_1806cb680`, vtbl +0x40) precompiles: all 6 VS; PS for kind 0 always, kind 2 only for
EnableAlphaZWrite blended materials, kind 1 never (compiled when first executed); lightmap variants only for transformation 0 and
only with 0 / 1 point lights (the corpus' lightmapped set, shader-permutations.md 2).

`eCShaderDefault::GetDeclarations` 0x6c8650 [C] (shader-permutations.md 8): transformation (`+0x2b8`, enum: 0 Default, 1 Instanced,
2 Skinned, 3 Tree_Branches, 4 Tree_Fronds, 5 Tree_LeafCards, 6 Tree_LeafMeshes, 7 Billboard, 8 Morphed, 9 Skinned_Morphed, 10
Vegetation, 11 Tree_Billboards) -> `XFX_SKINNED` / `XFX_ST_BRANCH` (3, 4) / `XFX_ST_LEAFCARDS` / `XFX_ST_LEAFMESH` / `XFX_BILLBOARD`
/ `XFX_MORPHED` / both / `XFX_VEGETATION` / `XFX_ST_BILLBOARDS` (0 and 1 add nothing); `XFX_ALPHABLEND=true` for mode 0 with an
opacity node or modes 1, 2, 4, 7 (mask 0x96); `XFX_SPECULAR` (`+0x2c0`), `XFX_DISABLELIGHTING` (`+0x2c1`),
`XFX_COMPRESSEDNORMALMAP` (`+0x2c6`), `XFX_FOG=false` (`+0x2c2` DisableFog), `XFX_DIRLIGHTSHADOWS=false` (`+0x2c3`),
`XFX_PNTLIGHTSHADOWS=false` (`+0x2c4`), `XFX_AMBIENTOCCLUSION=false` (`+0x2c5`), `XFX_LIGHTINGTYPE=<BRDF +0x2d0>` (0 Simple, 1
Complex = default, 2 WrapAround), `XFX_DOUBLESIDED` (`+0x2d9`), the role snippets `XFX_PS_DIFFUSE` .. `XFX_PS_SPECULARPOWER`,
`XFX_PS_NORMAL` + `XFX_TANGENTSPACE=1` (only when normal maps are allowed), `XFX_PS_DISTORTION`, `XFX_PS_EMISSION`, then
`eCShaderBase::GetDeclarations` 0x6d29b0: `XFX_MASKED=true` for mode 1. The sampler / tex-coord nodes add their `XFX_VS_TEXj`.

### 4.3 Special passes (`FUN_1806ca6c0` = CompileSpecialPass, vtbl +0x50) [C]

Index `type * 2 + option`, VS at version `+0x28 + 16 idx`, PS at `+0xe8 + 16 idx`, compiled flag bytes `+0x18 + idx`; entries
`VS_DefaultZPass` / `PS_DefaultZPass`; `GetDeclarations(normalMap = false)`.

| type (`admin+0x314`, depth map `vtbl+0xf8`) | macros |
|---|---|
| 0 z pre-pass (null depth map) | `XFX_ZPASS` |
| 1 point light shadow map | `XFX_SHADOWMAPPASS_POINT`, `XFX_SHADOWMAPPASS` |
| 2 projected (spot) | `XFX_SHADOWMAPPASS_PROJECTED`, `XFX_SHADOWMAPPASS` |
| 3 (sun cascades [I]) | `XFX_SHADOWMAPPASS` |
| 4 variance | `XFX_VARIANCESHADOWMAP`, `XFX_SHADOWMAPPASS` |
| 5 radiosity (editor) | `XFX_LIGHTMAP`, `XFX_STATICSHADOWS=false`; `VS_Default` / `PS_DefaultRadiosityPass` |
| 6 | none (outside a special pass) |

Option 1 = `XFX_INSTANCED` (or `XFX_ST_BILLBOARDS_VERT1` / `2` for transformation 11). Static meshes in the z pre-pass always use
option 0 (1.2). The z PS writes view z (`PassData.m_vecViewPos.z`) and, when masked, clips `diffuse.a (or opacity) x globalAlpha -
c5.w`. Radiosity passes (`FUN_1806cb060`, 5 each at `+0x508` / `+0x558`) are editor only.

### 4.4 Uber port coverage

`shaders/material/ge_default.{glsl,frag,vert}` reproduce every permutation above from the same macro vector (31 / 31 PS and 17 / 17
VS vectors equal on the GPU; 183 / 183 captured material PS: shader-permutations.md 6-9). What a native renderer needs on top is
the **macro vector itself**, derivable entirely from engine data: the pass (4.2 / 4.3) + `GetDeclarations` (fields above) + the
graph's snippets (regenerate them from the node tree, or capture them once per shader with `engine/material_source.*`).

---

## 5. Data structures [C unless marked]

### 5.1 `eCShaderMaterialAdmin`

| offset | field |
|---|---|
| `+0x38` | gfx admin (`FUN_1800956c0`) |
| `+0x50 / +0x58 / +0x60` | timed elements array |
| `+0x70.. / +0x90..` | shader pools (Master / User maps) |
| `+0x168` | max shader version from caps; `+0x170` current version |
| `+0x184` / `+0x1c4` | view-projection copy / depth-bias VP (camera +0xa0 x +0x20) |
| `+0x208` | PS pool c0..c5 (type 1) |
| `+0x228` | VS pool c0..c30 (type 0) |
| `+0x248` / `+0x270` / `+0x298` / `+0x2c0` / `+0x2e8` | sampler slots s1 / s2 / s0 / s4 / s5 (0x28 each, image at +0x20) |
| `+0x310` | depth-bias VP active; `+0x311` in material; `+0x312` in z / special pass; `+0x313` z write; `+0x314` special pass type |
| `+0x31c` | overwrite indicator (editor views, 0 in game); `+0x320` instancing support |
| `+0x328` | hardware instancing VB; `+0x330` error handling; `+0x331` material fallbacks; `+0x334` / `+0x338` (0x100 / 50 on SM3: max VS constants / instances [I]) |
| `+0x33c` | active shader pool; `+0x340` master render mode (wireframe toggle); `+0x348` ErrorImage; `+0x350` quality |

### 5.2 `eCShaderBase` / `eCShaderDefault`

| offset | field |
|---|---|
| `+0x48 / +0x50` | blend mode container / value (2.2) |
| `+0x58` | MaskReference (byte) |
| `+0x68` | MaxShaderVersion |
| `+0x78` | UseDepthBias; `+0x79` TradeAlphaAsSolid; `+0x7c` render priority -> sort id bits 20+ [I] |
| `+0x80 / +0x88 / +0x90` | frozen / burned / highlighted overlay materials |
| `+0x98` | VS pool (`g_vShaderData`, c232, 20) |
| `+0xc8` | PS pool (`g_ShaderData`, c6, 8) |
| `+0xf8` | sampler pool: 7 slots x 0x28, `+0x218` dirty mask (pool +0x120), `+0x219` base 9, `+0x21a` max 7, `+0x21b` count, `+0x210` name |
| `+0x220` | admin |
| `+0x230 / +0x238` | elements; `+0x248 / +0x250` switch elements |
| `+0x258 + 8 v` | shader version objects (v 3 = `+0x270`: `eCShaderVersionDefault_3_0`); `+0x278` indicator version |
| `+0x2a0` | fallback material; `+0x2a8` invalid flag |
| `+0x2b0 / +0x2b8` | transformation (4.2) |
| `+0x2c0..+0x2c6` | Specular, DisableLighting, DisableFog, DisableDirLightShadows, DisablePntLightShadows, DisableAmbientOcclusion, CompressedNormalMap |
| `+0x2c8 / +0x2d0` | BRDF type |
| `+0x2d8` | EnableAlphaZWrite; `+0x2d9` DoubleSided |
| `+0x2e0 .. +0x430` | role proxies (0x30 each): Diffuse, Opacity, SelfIllumination, Specular, SpecularPower, Normal, Distortion, LightEmission (rt-albedo-facts.md 1.1) |

### 5.3 `eCShaderVersionDefault_3_0` (0x5a8, vtable 0x9d7e40)

`+8` admin, `+0x10` shader, `+0x18` 12 special-pass flags, `+0x28` / `+0xe8` special VS / PS proxies (12 each), `+0x1a8` main VS (6),
`+0x208` main PS (48), `+0x508` / `+0x558` radiosity VS / PS (5). Proxy = {vtable, `eCShaderPoolEllement*`}; element `+0x18` =
`eCGfxHLSLShader` (its `+0x18` = the API shader), `+0x38` = sampler mask. Vtable: +0x18 GetPasses 0x6cc970, +0x20 Execute 0x6cc610,
+0x28 ExecuteSpecial 0x6d9b40, +0x38 render states only 0x6cc850, +0x40 CompileAll 0x6cb680, +0x48 compile one pass 0x6cafe0,
+0x50 CompileSpecialPass 0x6ca6c0, +0x58 lighting pool 0x6cce70, +0x60 set constants 0x6d9a70.

### 5.4 `eSShaderMaterialPass` (0x20)

`+0` version object, `+8` flags, `+0x10` illumination, `+0x18` byte first light index, `+0x1c` switch. Flags: bit 0 sun, bit 1 (always 0),
bits 2-3 point light count, bits 4-5 lightmap combi, bits 6-8 kind (0 base, 1 additive light pass, 2 z-fill), bit 9 **non**-instanced,
bits 10-11 SpeedTree billboard VS variant.

### 5.5 `eSShaderMaterialIllumination` (entity `+0xf8` + 0x78; native-renderer.md 10)

`+0` light list, `+8` outdoor factor f, `+0xc` byte: **no sun** for this entity (gates `XFX_DIRLIGHT`) [C use, I meaning], `+0xd`
static light count, `+0x10 + 0x40 i` static lights, `+0x90` static SH present, `+0x94 + 0x10 k` static SH. Light list: `+0` sun
present, `+4 / +0x14 / +0x24` sun registers, `+0x34` dynamic light count, `+0x38 + 0x40 i` dynamic lights, `+0xb8 + 0x10 k` SH,
`+0x128` indoor ambient.

### 5.6 Mesh side

`eCMeshResource2`: `+0x38` load state, `+0x58 / +0x60` sub-meshes (0x48), `+0x88` visual, `+0x90 / +0x98` lightmaps, `+0xa0`
instancing capable, `+0xa4` frame stamp of the instance list, `+0xa8..+0xb8` render instances (0x58: matrix `+0`, switch `+0x40`,
illum `+0x48`, flags `+0x50`). `eCSubMesh`: `+0x28` start index, `+0x2c` index count, `+0x30` first vertex, `+0x34` vertex count,
`+0x38` material, `+0x40` hierarchy (node 0x28: `+0x20` first index). `eCGfxVisual` (0x68): `+0x10` API primitive, `+0x18` fill,
`+0x1c` cull, `+0x20` depth func, `+0x24` depth write, `+0x28` device, `+0x30..+0x48` streams 0..3, `+0x50` declaration, `+0x58`
index data, `+0x60` eCGfxShader. Declarations: rt-scene-facts.md 3 (static: stride 52 / 60 / 68, TC6 = lightmap uv, s3 TC4 = the
instance index).

---

## 6. For a native implementation

### 6.1 What `enh/native_opaque*` + `shaders/native/static_*` reproduced already (removed in 23d60f2, 2026-10-29: `git show 23d60f2^:<path>`) (native-renderer.md 9-14) [C]

* Part selection by the engine's rules (entity flags, frustum, fade < 1 and the view-range fade left to the engine), opaque and
  masked classes, front-to-back; the z pre-pass into hardware depth + linear depth map; colour pass after `BeginSolid`.
* Camera c0 / c1 (natively), fog c3 / c4 (copied from the engine's last main-view draw), c14..c36 composed from the illumination
  exactly like `FUN_1806cd1d0` / `FUN_1806b5bb0` (0 differences measured); world rows = entity `+0x60`.
* Shading: SH ambient type 2, indoor mix by COLOR1.a, directional lightmaps (s4 / s5, TEXCOORD6), sun Lambert x mask.a with the
  normal-map self-shadow term, up to 3 point lights, `pow 0.45`, x mask.r, radial fog, DXT5nm normal maps, cull mode from the
  engine (`D3DCULL_CW` for all static visuals, 2.1).

### 6.2 Gaps and deviations (all against sections 1-4)

1. **Material graph**: only the diffuse sampler (and normal) is used. Missing: `g_ShaderData` constant / animated / switch colours
   multiplying or replacing nodes, combiner / blend / mask nodes (detail maps, lerps), vertex colour (`XFX_PS_VERTEXCOLOR`),
   Fresnel, CamDistance, cube reflections (`texCUBE`), self-illumination, emission, specular (`XFX_SPECULAR`, sun c23 / point
   specular, specular power, lightmap specular at quality > low), BRDF types 0 / 2, `DisableLighting` (unlit), `DisableFog`,
   `DisableAmbientOcclusion`, `DisableDirLightShadows`, `CompressedNormalMap` (native assumes DXT5nm for every normal map),
   `DoubleSided`. Fix: drive the native pass with the ported ubershader and the engine's macro vector (4.4) instead of a
   hand-written subset.
2. **UV transforms**: native reads TEXCOORD0 / 1 raw. Scale, scroller, rotator, oscillator (VS, `g_vShaderData` c232+, time
   driven) and BumpOffset parallax (PS) are missing; each sampler has its own uv set index j (`XFX_VS_TEXj`), not "the diffuse set".
3. **Animated textures** (sampler type 1, frame = `int(t x fps) % count`) and texture switches (`SetSwitch` from mesh PS `+0x20`).
4. **Alpha test**: native tests `round(a x 255) >= ref` on the diffuse alpha. The engine never uses the fixed-function test: its z
   pass clips `alpha x globalAlpha - ref / 256` (`>= 0` passes) with alpha = the opacity role (`XFX_PS_OPACITY`, any graph) or the
   diffuse alpha, and the colour pass relies on depth EQUAL. Off by one step at the threshold (`/256` vs `/255`) and graph-dependent
   opacity.
5. **Lights**: native caps at 3 (`kMaxLights`); the engine draws a 4th light (2 static + 2 dynamic) in a second additive pass
   (One / One or SrcAlpha / One, no ambient / sun / lightmap). Native ignores `illum+0xc` (per-entity "no sun", 5.5): it only checks
   the list's sun byte.
6. **Lightmap gating**: the engine drops the lightmap permutation for the whole mesh when a lightmap resource exists but is not loaded
   (`BindLightmaps` returns 0), and never uses lightmaps for instanced batches; native leaves parts with loading images to the engine
   (fine) but must not combine lightmaps with instancing.
7. **Instancing**: native colour / z passes draw per part; the engine batches up to 50 instances that share light list, switch,
   flags and f, and draws instances with static lighting alone. Equal results: batch members share the light list and f, so a
   per-entity composition gives the same constants (instanced batches always use combi 0 and fade 1).
8. **Depth-bias materials** (`UseDepthBias +0x78`: c0 = depth-bias projection x view) are not handled: decal-like meshes would z-fight.
9. **Overlays** (burned / frozen / highlighted, entity flags 0x200000 / 0x400000 / 0x800000) add passes with another material over
   the same geometry; native does not draw them (the focus highlight would vanish on natively drawn parts).
10. **Error / placeholder textures**: the engine binds a magenta 1x1 for a null image and the 4x4 DXT5 placeholder while streaming;
    native skips such parts (left to the engine), which is the right choice while both renderers coexist.
11. **Blended static parts** (terrain layers via request bit 0x200, decals, AlphaBlend / Modulate / Brighten ...) and the alpha pass
    (fading parts, depth write on for fading *opaque* materials, off for blended ones): not native yet; 2.2 gives every state.
12. **Fog** comes from the engine's latest draw; natively it is `eCGfxFog` `+0x18 / +0x1c / +0x28..+0x30` through `SetFog`'s formula
    (3.2), read at `eCFrustumDatabase::Execute` time (db `+0x14c8`). c5 (camera obstacle, `.w` mask ref) is not provided natively;
    materials with a CamObstacle node read it only in the z pass at quality High.
13. **Sampler states**: native uses one linear (or linear-clamp) sampler; the engine has per-node wrap / clamp / mirror, sRGB per node,
    the configured MIN filter (anisotropic) with linear MAG / MIP, and point-clamp for s0..s2.

---

## 7. Open questions

* Who sets request bit 0x200 (blended sub-meshes into the solid list) and when it replaces bit 4: `eCFrustumElementItem::Request` /
  the database (slice A).
* `illum+0xc` meaning (indoor / portal room "no sun"?), and `eCTimer +0x10` game vs real time.
* The z pre-pass off (`db+0x1590 == 0`) with masked materials drawn at depth EQUAL: which views run without it, and do masked
  materials then vanish (or does another path cover them)?
* Cube sampler slot states (not written by `UpdateSampler`; `eCColorSrcCubeSampler` sets only the image).
* `admin+0x334 / +0x338` (0x100 / 50 vs 0x60 / 45 for SM < 3): use not traced.
* Fog in additive light passes (inferred double fog contribution) is untested in game.
* SpeedTree / skinned / particle shader versions (other `CreateShaderVersion` classes, `FUN_1806d4aa0`, `FUN_1806d6c30`,
  `FUN_1806d90d0`): slice B.
