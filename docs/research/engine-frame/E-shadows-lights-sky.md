# Engine frame, slice E: shadows, lights, sky, weather visuals

RE of 2026-10-27 (headless Ghidra on Engine.dll, project clone `clones\EngineE`; dumps `B:\DevDrive\risen-ghidra\frameE_dump1..3.txt`
plus the older `rvk_sunshadow_dump*.txt`, `rvk_shadow_dump*.txt`, `gi\core_dump.txt`, `gi\ls_dump.txt`, `gi\sh_dump.txt`,
`lights*_dump.txt`; constants read from the test copy's `bin\Engine.dll` with `frameE_scripts\rd.py`). Effect sources:
`post_SSM.fx` / `post_sky.fx` extracted from effects.pak (`B:\DevDrive\risen-ghidra\gi\fx\`), the sky material shader
`data\compiled\effects\shadermaterial\ge_skydome_2_0.fx`. [C] = read in decompilation / source, [I] = inferred, [?] = open.
RVAs are Engine.dll. Builds on shadows.md (2, 3), frame-phases.md, rtgi.md 1 / 17 / 19, native-renderer.md 10 / 11 (not repeated
where they are complete). Device calls are named by HAL slot (`src/hal/slots_generated.h`: `+0x460` SetRenderTarget, `+0x468`
GetRenderTarget, `+0xf0/+0xf8` Set/GetViewport, `+0x100` ClearViewport, `+0x110/+0x118` Begin/EndScene, `+0x440` CreateDepthBuffer).

## 1. Call order in a frame

```
eCApplication::Process (modules)
  eCWeatherAdmin::Process 0x710730              weather state blend -> sun light, hemisphere, skydome, fog, rain emitter (4.4)  [C]
  eCIlluminationAdmin::Process 0x579260         [C]
    static-light sphere tree upkeep (+0x68)
    eCSkydome_PS::UpdateSkydomeEnvCubemap 0x6e7ed0 (illum+0x470 image)  -> SkyCube view, one cube face per frame (2.6)
    UpdateLightSets 0x575130                     every eCLightSet's eSShaderMaterialLightList rebuilt (3.2)
eCApplication::DoRender 0x4c0d40
  eCImageEffect::Execute (scene effect) -> ... eCImageFilterScene::OnUpdateOutput 0x75cf10                       [C]
    eCCameraBase::Update 0x4f7270 (main camera) = ExtractFromFrustum, PreExecute 0x66e790, Request          [C]
      PreExecute: entities with element flag 0x400 -> render-context vtbl+0x38 RequestPrePasses -> list db+0x14d0,
                  then every pre-pass op -> vtbl+0x28 RenderPrePass:
        eCRenderContextDynamicLight (vtbl 0x9a98b0): +0x38 0x59e7c0 (one op per db frame, global 0xc62884),
            +0x28 0x59e840 = light vtbl+0x2f0 UpdateShadowMaps
              eCDirectionalLight_PS::UpdateShadowMaps 0x591fb0 (sun):  terrain variance map (if re-aimed) -> cascades (2.1-2.3)
        eCRenderContextPointLight (vtbl 0x9a9908): +0x38 0x59e8c0 AddShadowCells (static + dynamic point lights),
            +0x28 0x59eaa0 = illum+0x380 eCPackedShadowMap::RenderShadowMap 0x5a08f0 (point-light shadow atlas, 2.4)
    GetRenderTarget, eCFrustumDatabase::Execute 0x66e990(main db, desc = filter+0x40)                           [C]
      ExecuteGfxCamera, eCGfxFog::Execute, clear, z pre-pass (depth map R32F)
      eCIlluminationAdmin::UpdateShadowMask 0x5748d0  -> mask filter chain (SSAO, cascades, VSSM, PSSM, Blur) (2.5)
      solid list +0x1500, debug, decals, billboards, list +0x1530 (the SKY, 2.7), refraction copy, refracted, alpha, +0x1548
  post filters, GUI, present
```

* So the sun cascades are rendered **inside `eCImageFilterScene::OnUpdateOutput`, by the main camera's pre-pass ops**, before the
  scene target is bound: this settles frame-phases.md 7's open "job" [C]. The terrain variance map, when re-aimed, renders before
  the cascades (same UpdateShadowMaps call) [C].
* `0x59e7c0` pushes at most **one dynamic-light pre-pass op per database frame** (whichever eCRenderContextDynamicLight requests
  first); the hemisphere (eCHemisphere_PS, same context class) has an empty UpdateShadowMaps (0x592b90). The sun, the hemisphere
  and the skydome are property sets of the one `World_MCP` entity (types 0x10, 0x13, 0x21) [C]; that the sun's context comes first
  in its context list is [I] (the cascades are rendered in every measured outdoor frame).
* Interiors (no sun in the database's light set) have no Shadow views (frame-phases.md 3) [C measured].

## 2. Steps

### 2.1 Sun direction, terrain variance map (`eCDirectionalLight_PS::UpdateShadowMaps` 0x591fb0) [C]

* `dir = light+0x80` (true direction the light travels, from `OnUpdateDirection` 0x591eb0: entity rotation x RotY(+0x74) x RotX(+0x78)).
  Shadow direction `light+0x8c` := dir only when `acos(dot(dir, +0x8c)) > 0.5 deg` (0.5 deg steps; our `trueSunVector` reads +0x80).
* Terrain map (`illum+0x358`, eCShadowMapDirectionalLight2VS, G32R32F, `TerrainShadowMapSize`): camera position in light view
  (`LookAt(+0x80)`, no translation) reduced by `fmodf(p, ...)`, re-aimed when `|dx| or |dy| > +0x9c (32) * 2 * +0x98 (30000) / size`
  (= 1875 units at 1024) or the shadow direction changed. Re-aim: split light view = LookAt(dir), box = light-space box of
  (camera pos +- camera far `cam+0xc0`, `bCBox::SetBox(c, r)` = c +- r), vtbl+0x170 RenderShadowMap.
* VS RenderShadowMap 0x5a1b40: base RenderShadowMap (2.3), then a separable blur with `post_blur.fx` techniques `Blur7H` (into a
  temp G32R32F image of the same size) and `Blur7V` (back into the map), output mode 1 [C]. Clear colour 0x00ffffff.
  Texel = 2 far / size [C formula; shadows.md 3.3's "~59 units" assumed far / size: [?] which far applies].
* Then the cascade object `illum+0x350` gets `LightDirection` = +0x8c and `RenderShadowMaps` 0x59f200 runs.
* Casters by db node flags (`ExtractShadowMap` 0x66f470 -> `OnQueryNodes`, `(node+0x18 & mask) == value`, db+0x15b0 / +0x15ac =
  camera+0x16a8 / +0x16a4): cascades value 8 mask 10, terrain map value 0x10 mask 0x12 (set in SetSplitCount / ApplyConfiguration).
  Entity predicates (`eCIlluminated_PS`): ShadowCasterType +0x38 (0 Terrain, 1 Building (default), 2 Object, None),
  CastDirLightShadows +0x40, its overwrite +0x50 (1 = true), CastPntLightShadows +0x41 / +0x60, CastStaticShadows +0x42 / +0x70.
  Detail (cascade) caster = (cast || ovr == 1) && type in {1, 2}; terrain-map caster = same && type in {0, 1}; point-light caster =
  (+0x41 || +0x60 == 1) && type == 2 (0x5951c0 / 0x5972a0 / 0x5951a0; cascade / terrain / packed CullEntityCallbacks identical) [C].
  AddShadowCasterEntity 0x66d860 also needs entity flags `& 0x8006 == 0x8006`, not `& 8` [C].

### 2.2 Cascades (`eCCascadeShadowMap::RenderShadowMaps` 0x59f200) [C]

Inputs: main camera `C = ms_pApplication+0x38` (else +0xc8), view `C+0x10`, projection `C+0x50`, position `C+0x90`, far `C+0xc0`.
* `MaxDistance +0x1c` < 1 -> set to camera far. Splits as shadows.md 3.1 (`start_i = ((Max-1) i/N + 1 - Max^(i/N)) * factor + Max^(i/N)`,
  `FUN_1804ac030` = powf); split element i (0x168 bytes, an embedded eCShadowMapDirectionalLight2, vtable 0x9768d0) `+0x160` = start.
* Per split: slice frustum = NDC unit box `[-1,1]^2 x [0,1]` inverse-transformed by the projection, near/far replaced by
  start_i / start_{i+1} (Max for the last), 8 corners merged into a `bCSphere` (incremental Merge, not minimal), centre to world
  (inverse view) then to light view (inverse of LookAt(dir)); light box = centre +- radius, **box.min.z = -100000** (an absolute
  light-space coordinate: light view has no translation); `split+0x60` = LookAt(dir), `+0xe0` box, `+0x100` world slice frustum;
  then split vtbl+0x170 = `eCShadowMapBase::RenderShadowMap`.
* `eCShadowMapDirectionalLight2::UpdateCamera` 0x5a13e0: shadow camera (`split+0x58`, eCCameraBase): orthographic; scale
  `(2/(maxx-minx), 2/(maxy-miny), 1/(maxz-minz))`, translation `-(min+max)/2 * scale` for x/y, `-minz * scale` for z; the x/y
  translation reduced by `fmodf(t, 2/size)` (FUN_1804ad148) = whole-texel snapping; ClipDepth near 0 / far = box.max.z; db+0x1694
  (shadow camera +0x1694) = max z. Texture matrix `+0xa0 = Bias x Proj x View^-1` with Bias = scale (0.5, -0.5, 1), translate
  (0.5 + 0.5/size, 0.5 + 0.5/size, -depthBias); `depthBias +0xf8 = 1e-4 + 3e-5 i` (SetSplitCount 0x59f650) [C].
* Map: `Create(size = cascade+0x24, fmt = eCShaderMaterialAdmin+0x178)`: the directional format = the **last supported** of
  {R32F, R16F, D16, D24X8, DF16, DF24} (PostInitialize 0x6ba9d0 probe table 0x9d2290 / 0x9d22b0) = DF24 on current drivers [C].
* Measured (High, beach): 3 views of 33 / 69 / 100 draws (frame-phases.md 3); every frame (our `debug.shadow_stride` skips when still).

### 2.3 One shadow map render (`eCShadowMapBase::RenderShadowMap` 0x5a1070 -> `eCRenderSystemAdmin::RenderShadowMap` 0x66ce00) [C]

```
CreateRenderTarget(false)                    eCDepthMap 0x59fd00: AllocateShadowMapImage (pooled per desc)
faces = cube ? 0..5 : one pass with face 6
  SetRenderTarget(face) 0x59fee0             GetRenderTarget/GetViewport saved at +0x30/+0x38/+0x40
     depth formats (D16/D24X8/DF16/DF24): colour = GetShadowMapWorkingBuffer(size) (FOURCC NULL, else R5G6B5, else X8R8G8B8),
                                          depth = the map;  colour formats: colour = map (cube face), depth = own D24S8/D24X8
                                          (+0x20) or the shared GetShadowMapDepthBuffer(size) 0x575a70
     SetViewport(0, 0, size, size, +0x50, +0x54)
  ClearViewPort 0x59fe50                     depth formats: z + stencil (flags 6), else colour + z + stencil (7); colour +0x28, z 1
  UpdateCamera(face)                         2.2 / 2.4
  RenderShadowScene: SetCurrentCamera(shadow cam); ExtractShadowMap; RequestShadowMap (RequestRenderOps flags 0x10 | db[0x15a5]);
                     OnPostExtraction; ExecuteShadowMap 0x66f630; restore camera
     ExecuteShadowMap: ExecuteGfxCamera(shadow cam) (sets the VS camera constants, shadows.md); BeginScene; qsort list +0x1518;
                       BeginZPass(map, true, false); every op vtbl+0x18 RenderZPass; EndZPass; EndScene
  ResetRenderTarget                          restores RT, depth, viewport
```
* `BeginZPass` 0x6b9720 render states (`eSShaderRenderStates`, SetCurrentRenderStates 0x6b7c50): alpha test from the material
  (struct default ref 0 / func ALWAYS), blending off, master depth write mode 2 (param true), depth func 4 (LESSEQUAL), colour /
  alpha writes = the map is **not** a depth format (DF24: off), SetMasterFogging(1), SetMasterCullingMode(0); material admin
  `+0x314` = pass type = `GetRenderPassType()`: main z pre-pass 0, point 1, spot / packed 2, **directional 3**; material shaders
  pick their shadow / z variant from it (static material slice) [C]. EndZPass resets the type to 6.

### 2.4 Point / spot light shadows [C]

* This build shadows point lights through **`eCPackedShadowMap`** (illum+0x380, one 1024^2 atlas, format = the directional one,
  created in ApplyConfiguration), not through eCShadowMapPointLight / SpotLight cubes (those classes exist: `Create(size, cube =
  true, fmt)`, perspective 90 deg / cone cameras; who creates them is [?], none seen in game).
* Request 0x59e8c0 (per light with a point-light context, camera db): eCStaticPointLight_PS with `+0x68` (CastDynamicShadows) set:
  level = int((dist - R) / R) + 1; eCPointLight_PS (dynamic): level = int((dist - R)/R) + 2, R = +0x88; level < 6 ->
  `AddShadowCells(level, camera frustum db+0xa8)` (eCPointLight_PS 0x598df0 needs CastShadows +0xc0). 6 faces, each a 90 deg
  perspective (near 10, `_33 = R/(R-10)`, `_43 = -10 R/(R-10)`) from the light position; `CullShadowCell` 0x574d50 drops faces whose
  frustum misses the camera frustum; `AddPackedShadowCell` 0x574a20 -> quadtree cell of that level; cell texture matrix maps into
  the cell with a 3-texel border ((size-6)/size), z bias 0.003, clip (10, R), colour flags 0xff00 (dynamic, CastShadows) / the
  static light's ShadowMaskIndex (+0x60) [I: the material shader's point-light selector channel].
* Render 0x5a08f0: one target bind for the atlas (face 6), clear, then per cell UpdateCamera (ortho flag off, the cell's view /
  projection, viewport = the cell rect) + RenderShadowMap(scene) into the cell; ClearCells at the end of UpdateShadowMask.
* Lit by the mask's PSSM pass (2.5 step 5). The doc frames measured contain no point-shadow work (frame-phases.md 3) [C measured].

### 2.5 Shadow mask (`UpdateShadowMask` 0x5748d0, `eCImageFilterShadowMask::OnUpdateOutput` 0x668560) [C]

Filters (ApplyConfiguration 0x577a50; inputs `(slot, filter)`): `+0x460` texture filter = the depth map (`eCShaderMaterialAdmin+0x290`,
set each call); `+0x450` R32F at 1/4 (FUN_18014f410, a copy/downscale [I]) of `+0x460`; `+0x430` post_SSM `SSAO` A8R8G8B8 1/2
(0 depth, 1 `+0x450`, 2 random texture `+0x468`); `+0x438` / `+0x440` `BlurSSAO` 1/2 (0 depth, 3 previous); `+0x448` the mask
(0 depth, 1 `+0x450`, 2 random, 3 `+0x440` only with SSAO; ClearColor 0xffffffff; `+0x118` cascades, `+0x120` terrain map,
`+0x128/+0x130` packed map x1); `+0x458` post_SSM `Blur` (0 depth, 3 mask). Output = `+0x458` when the mask's HighQuality
(`+0x114`) is set, else `+0x448`; stored as `eCShaderMaterialAdmin+0x298` = s0 of every material PS. No depth map ->
`+0x2b8` released, nothing drawn.

Mask pass (target = the mask image, depth = the bound main-view D24S8, viewport = output size; `eCImageFilterRTBase::SetRenderTarget`
0x668370):
1. SSAO chain runs through UpdateColorOutput's input updates (R32F 400x225, SSAO 800x450, BlurSSAO x2 at 800x450; 1600x900 window).
2. EnableSSAO (`+0xfc`): BeginScene, quad depth func ALWAYS, technique `UpscaleSSAO` (writes (ssao, 1, 1, 1), 5 bilinear-weighted
   taps); else ClearViewport(colour only, 0xffffffff) + BeginScene.
3. Quad depth func GREATER (visual +0x20 = 5). Cascades (if split count > 0): `DizzerKernelSize` = 2.5/size, `PCFFilterOffset` =
   (-0.5, -0.5, 0.5, 0.5)/size, `PCFFilterScale` = (size, size), `FadOut` = (camFar / (0.2 Max), -(0.8 Max)/(0.2 Max)); technique
   `SSM` with a terrain map (+ `DepthToVarianceShadowMatrix` = terrain `+0xa0` x R, `VarianceShadowTexture`), else `DSSM`; suffix `_Low`
   when HighQuality is off. Per cascade i: `DepthToShadowMatrix` = split `+0xa0` x R, `ZDistance` = (f P33 + P43) / f with f =
   start_{i+1} (Max for the last), `ShadowTexture`, filter constants of that map's size; ExecuteEffect (one quad). R = inverse view
   x diag(1/P11, 1/P22, 1) x scale(camFar) (built from `C+0x10`, `C+0x50`, `C+0xc0`; multiplication order [I]).
4. Terrain map: `VSSM` with ZDistance 1.0 (stencil ops ZERO: resets the stencil).
5. Packed point shadows (if cells): technique `PSSM`(`_Low`), `ViewProj` = main VP, `TexViewProj` = bias (0.5, -0.5, 0.5 + 0.5/vpW,
   0.5 + 0.5/vpH) x VP, `DizzerKernelSize` = 2/size; per cell a 5-vertex pyramid (light position + 4 far corners; vertex colour =
   ~flags, 4 texcoords = the cell matrix); 3 passes: (0) cull 2, LESSEQUAL, stencil INCR on z-fail when the camera's near sphere is
   inside the cell frustum, else on pass; (1) cull 3, DECR likewise; (2) cull 1, depth ALWAYS, stencil NOTEQUAL 0 with ZERO ops,
   colour write RGBA, blend ZERO / SRCCOLOR: `max(1 - lit, vertexColour)` -> a selected channel becomes 1 = shadowed.
6. Inputs unbound (`VarianceShadowTexture`, `ShadowTexture`, `BlurTexture` := null), EndScene, render target restored.
7. Blur (HighQuality): 9-tap depth-aware blur of the whole mask (`DepthScale = far/100`).

Shader math (post_SSM.fx): reconstruction `p = (2 t.x - 1, 1 - 2 t.y, 1) * depth`, `t = uv + 0.5/DepthTextureSize`; DSSM dithered = 8
point taps at `DizzerOffset = (rand*2-1) * DizzerKernelSize`, `lit = z < stored`, mean; `_Low` = 4-tap manual bilinear PCF
(`frac(uv * PCFFilterScale)` weights); `fade = saturate(depth * FadOut.x + FadOut.y)`, `shadow = max(cascade, fade) * VSSM`
(SSM); VSSM = Chebyshev `max(z <= mean, linestep(0.7, 1, var / (var + d^2)))`, var >= 1e-5. Stencil per cascade EQUAL 0, INCR on
pass/fail, KEEP on z-fail: first covering cascade wins. Channels: `.a` sun (1 lit), `.r` SSAO, `.g` / `.b` point-light shadow
(1 = shadowed) [C for writes, channel meaning per light [I]].
SSAO: 8 reflected kernel samples (16 in high quality) of the R32F depth at 1/4, `Scale = far / SSAOScale (1000)`, result
`saturate(lerp(sum/n*2 - 0.2, 1.3, 0.3))` (`SSAORadius` (1, 1, 0.01) default; mask setters `SetSSAOScale/Radius`).

### 2.6 Sky environment cube (`UpdateSkydomeEnvCubemap` 0x6e7ed0) [C]

* Image: `illum+0x470` image resource, created in PostInitialize 0x577720: 128^2 A8R8G8B8 cube, 1 level. Sampled by
  `eCColorSrcSkydomeSampler` (0x6c5ce0: binds illum+0x470 into the material's sampler pool) = the materials' sky reflection.
* Timer: `+0x3b8 += dt`; nothing while `+0x3b8 <= +0x3bc` (10 s; starts at 11 = refresh on the first frame) and no refresh is
  running. A refresh takes **6 consecutive frames** (counter 0xc6ea90): frame 0 binds face 3 (-Y) and clears it to 0x30303030 (no
  draw); frames 1..5 render faces 0 (+X), 1 (-X), 2 (+Y), 4 (+Z), 5 (-Z): SetRenderTarget(face, no depth), viewport (0, 0, size,
  size, 0, 1), cube camera `+0x3c0` (eCGfxCamera): projection rows (1,0,0,0) (0,1,0,0) (0,0,1.001,1) (0,0,-1.001,0) = 90 deg,
  near 1, far 1000; view = axis basis at the origin; ClearViewport(colour, black); BeginScene; `Render()` with `+0x3cc` = 1 (cube
  camera); EndScene. Afterwards the original RT / viewport / main camera are restored; all inside BeginMaterial / EndMaterial.
  So the cube holds sky dome + clouds + stars + sun + moon only (no world geometry).

### 2.7 Sky in the main view (`eCSkydome_PS`, render context vtbl 0x9ddd90) [C]

* Request 0x6e8a30: request flag 2 -> op in **db list +0x1530** (drawn in the Solid pass after billboards, before the refraction
  copy, so the copy holds the sky), flag 4 -> list +0x1548 (RenderAlpha 0x6e8a00 is empty). Render(op) 0x6e8a10 = `Render` 0x6e4730.
* Render (enabled flag `+0x18 & 1`): camera = active (main) or the cube camera; `CreateBuffers` 0x6e5f90 (once): a **screen-space
  grid**, 32 x (24+1) vertices (`+0xc0`, `+0xc4`), positions (x, y) in [-1, 1], z = 1, float3, triangle strip; then with the view's
  translation zeroed and projection `_33 = 1, _43 = 0` (depth 1.0, follows the camera): stars (`+0x278` sky object, blend mode 7,
  `+0x140` texture, colour `+0x390`, matrix `+0x2d0`, `StarCount +0x28` random billboards, size 0.01-0.03, random grey), sun
  (`+0x170`, texture `+0x138`, one billboard of size `+0x3a8 * +0x50`, matrix `+0x310`), moon (`+0x220`, blend mode 5, texture
  `+0x130`, colour `+0x58`, size `+0x78`, matrix `+0x350`); camera restored; then the dome: eCShaderSkyDome `+0xf0` (`+0x300` =
  skydome `+0xa0`, `+0x304` = `+0xa4`, `+0x308` = `+0xa8`, `+0x310` = `+0x90` cloud thickness) through GetPasses / ExecutePass +
  eCGfxVisual::Execute (`+0xe0`).
* Dome shader: `ge_skydome_2_0.fx` compiled at run time from the shader graph (CreateShader 0x6e4f90: cloud texture 1 `+0x80` with
  a scroller (speed 0.1, direction `+0xb0`) and scale 1/`+0xb8`, cloud texture 2 `+0x88` scale 1/`+0xbc`, absorption images made
  from them (CreateAbsorbationImage 0x6e2cb0), `XFX_PS_CLOUDS` / `XFX_PS_ABSORBATION`; `eCShaderSkyDome::SunOcclusion +0x324` selects
  the `XFX_SUN_OCCLUSION_BILLBOARD` variant (version slots `+0x1b8/+0x1d8`)). VS constants `g_vShaderData` c31.. : sun normal,
  sun colour, sky colour, haze colour, world 3x4 at c35 (`SetWorldMatrix` 0x6d7820 writes register 0x23); PS `c6` cloud colour
  (a = thickness), `c7` sun colour [C from the source; which eCShaderSkyDome field feeds which register [I]: sky `+0x2b8`, haze
  `+0x2d0`, sun `+0x2e8`, sun normal `+0x318`]. Per-vertex: normal from the inverse projection and inverse view, haze /
  sun terms; cloud uv `acos(normalize(n.x, 4 n.y, n.z).xz)`. States (0x6d9740): material blending (dome BlendMode 4), depth func
  LESSEQUAL, depth write off unless opaque [I from flags].
* `post_sky.fx` (a full-screen sky / cloud composite against the depth map) exists in effects.pak; no caller found [?].

## 3. Data structures

### 3.1 Shadow objects [C]

| object | fields |
|---|---|
| eCCascadeShadowMap (0x40) | +0x10 light dir, +0x1c MaxDistance, +0x20 LogSplitFactor (Invalidate 0.5), +0x24 map size, +0x28 split count, +0x30 split array (0x168 each), +0x38 count, +0x3c capacity |
| split / eCShadowMapDirectionalLight2 (0x120 as own object) | vtable, +0x08 format, +0x0c size, +0x10 cube flag, +0x18 image, +0x20 own depth buffer, +0x28 clear colour, +0x30/+0x38/+0x40 saved RT/depth/viewport, +0x50/+0x54 viewport z, +0x58 eCCameraBase (its db at +0xf8), +0x60 light view, +0x90 camera position, +0xa0 texture matrix, +0xe0 light box, +0xf8 depth bias, +0x100 slice frustum (cascades) / Ex frustum, +0x160 split start |
| eCShadowMapDirectionalLight2VS (terrain) | +0x100 valid flag, +0x108 blur temp image, +0x110 post_blur effect filter, +0x118 texture filter |
| eCDepthMap formats (SetupImageDesc 0x59fb20) | 0 A8R8G8B8, 1 R16F, 2 R32F, 3 D16, 4 D24S8, 5 DF16, 6 DF24 (3-6 depth usage), 7 G16R16F, 8 G32R32F |
| material admin formats | +0x178 directional (= probed, DF24), +0x17c variance (G32R32F with RT + autogen-mip usage, else 9), +0x180 point (probed with an extra flag [?]) |
| eCPackedShadowMap (illum+0x380, 0xb0) | +0x60 cell array (0x120 each: +0x00 rect, +0x10 view, +0x50 projection, +0x90 cell matrix, +0xd0 texture matrix, +0x110 z bias, +0x114/+0x118 clip, +0x11c flags), +0x70 count, +0x78 quadtree root |
| eCImageFilterShadowMask (0x140) | +0xfc EnableSSAO, +0x110 ClearColor, +0x114 HighQuality, +0x118 cascade, +0x120 terrain, +0x128/+0x130 packed maps, +0x138 packed visual |

### 3.2 Lights [C]

* `eCDirectionalLight_PS` (vtable 0x9a3dc8): Color +0x40 (values +0x48), SpecularColor +0x58 (+0x60), Intensity +0x70,
  DirectionOffset +0x74 (yaw, pitch, roll), dir +0x80, shadow dir +0x8c, terrain range +0x98 (30000), re-aim texels +0x9c (32),
  terrain centre +0xa0. `AddToLightList` 0x591d90 (first directional only, list byte +0 = 0): list +0x04 = colour x intensity
  (c22), +0x14 = specular x intensity (c23), +0x24 = -dir (c24), +0x30 = 0.
* Light sets: `QueryLightSet(entity)` 0x575b90 = every registered dynamic light (illum +0x38: sun, hemisphere, eCPointLight_PS
  registered at cache-in) that is Enabled (+0x20) and whose `IsBoxInRange(entity box)` holds, **in registration order**; a set is
  shared by checksum (sum of the light pointers). `eCLightSet::UpdateIllumination` 0x579c70 clears the list and calls each light's
  AddToLightList: point lights fill at most 2 entries (`count +0x34 < 2`): **the first two in-range enabled point lights in
  registration order, no distance or intensity ranking** [C]. Point entry: position +0x38 (light +0xe8), 1/R^2 +0x44 (R = +0x88),
  colour +0x58 = Color +0x48 combined with the colours at +0x60 / +0x78 [I: effect colour by the flicker value] x Intensity +0xa4,
  shadow selector +0x68 = (0, 1, 0, 0) with CastShadows (+0xc0), else 0 [C].
* Flicker (`eCPointLight_PS::OnProcess` 0x598920, `CalculateEffectIntensity` 0x592ba0): effect 1 = 0.39 sin(t/period + phase) +
  0.6; 2 = a noise of (t/period + 1 + phase); 3 = random >= 0.5 else 0; 4 = alternating 1 / 0; decay fade via sin.
* Static per-pixel lights / static SH (`eCIlluminated_PS::UpdateIllumination` 0x595300, `FindWightedStaticLights` 0x5765a0):
  candidates from the static sphere tree (<= 64), weight = `(1 - d^2/R^2) x (0.3 r + 0.59 g + 0.11 b)` from the entity box centre,
  kept > 0.01, lights of other portal rooms only if visible through portals, sorted, <= 8; the first two become the per-pixel
  lights (colour x intensity x 4 x factor; the second faded by `(w2 - w3) / w1`), the rest go into the static SH; the outdoor
  factor `f` eases at 1/0.3 per second towards the portal-room openings' solid-angle sum (native-renderer.md 10, rtgi.md 1).
* Hemisphere: rtgi.md 17 (SH at +0xdc, 7 registers into list +0xb8, indoor colour +0x128).

## 4. Config switches and weather

### 4.1 Shadow quality (`/Engine/Render/Shadows/<Quality>`, ConfigDefault.xml; ApplyConfiguration 0x577a50) [C]

| key | Off | Low | Medium | High | target |
|---|---|---|---|---|---|
| EnableDetailShadows | false | true | true | true | cascade object created / deleted (illum+0x350) |
| DetailShadowMapSize | 512 | 512 | 512 | 1024 | cascade +0x24 |
| DetailShadowMapCount | 2 | 2 | 3 | 3 | SetSplitCount |
| DetailShadowMapViewRange | 3500 | 3500 | 5000 | 7500 | MaxDistance |
| DetailShadowMapSplitFactor | 0.65 | 0.5 | 0.65 | 0.4 | LogSplitFactor |
| EnableTerrainShadows / TerrainShadowMapSize | false / 512 | true / 512 | true / 512 | true / 1024 | terrain map (illum+0x358) |
| ScreenSpaceAmbientOcclison | false | false | true | true | mask EnableSSAO + SSAO input |
| SoftShadows | false | false | true | true | mask HighQuality: SSM vs SSM_Low, Blur output [I: key -> field by read order] |

Shadow camera db flags per map: cascades +0x16a4/+0x16a8 = 8 / 10, terrain 0x10 / 0x12. Variance and directional formats
depend on device support (2.2). Terrain map without detail shadows: technique DSSM is not used then (no cascades) [I].

### 4.2 Weather -> render state (`eCWeatherAdmin::Process` 0x710730) [C]

Current blended state at weather +0x50 (eCWeatherState; zones blended by shape factor, environments by probability and duration).
Day time t (0..1): day when 0.25 < t < 0.9167; sun progress (t - 0.25) / 0.6667, night progress likewise; intensity factor
`k = 1 - max(|2 p - 1| - 0.9, 0) / 0.1` (fades the last 5 % of each arc); azimuth offset `2 x 50 deg x p - 50 deg`, elevation 60 deg
(day) / 50 deg (night): **sun and moon are the same eCDirectionalLight_PS** [C].
* Sun light (property set 0x10): Color = +0x58, SpecularColor = +0x70, Intensity = k x +0x80 x +0xb8, DirectionOffset = (azimuth, elevation, 0).
* Hemisphere (0x13): GeneralAmbient / BackLight / SunLight from +0x50, +0xa0, +0xc0 scaled by `(1 - +0xb8) + (1 - k)`, Intensity 1,
  axis offsets, IndoorAmbientColor = +0x88 (rtgi.md 17).
* Skydome (0x21): stars matrix RotY(2 pi t) RotX(elev), star colour `((t - 0.5) 2)^2`, sun matrix, sun size factor
  `0.3 + 0.3 min(2|p - 0.5|, 1)^2`, moon matrix, CloudThickness = +0x138, CloudColor = +0x140; eCShaderSkyDome SunColor = +0x50 x k,
  HazeColor = +0x120, SkyColor = +0x108, SunNormal = day / night vector.
* Fog: the main camera's eCGfxFog (`cam+0x15c0`): enable, colour = +0xe0 (ARGB), mode 3, start +0xf8 / end +0xfc (end clamped to the
  render admin's view distance `+0x7c`, start scaled with it), density +0x100; consumed by `eCGfxFog::Execute` -> SetFog (c3 / c4,
  native-renderer.md 10).
* Thunder: while the timer 0xc6eaec runs (started with probability +0x54, duration rand(+0x58)), +0x80 (light intensity) and
  +0x148..+0x150 (cloud colour) are multiplied by rand(+0x5c, +0x60) each frame: **lightning = a flash of the sun / moon light and
  the clouds, no point light** [C]; a random effect (+0x68, probability +0x70) is spawned through gCEffectModule at the player.
* Rain: the `World_MCP` property set 0x4f gets ParticlesPerSecond (2 (blend - 0.5) x +0x10), SpeedMinMax, Size, DirectionScale,
  CurrentTextureTile from the dominant weather state (blend > 0.5) [C]; its rendering belongs to the particle slice.

## 5. For a native implementation

Data to read per frame (all SEH-guarded, engine thread):
* Cascades: `illum = FUN_18014f330()` (the illumination admin; our `engine/lights` already holds its `this`), `+0x350` cascade
  object (null = detail shadows off), `+0x358` terrain map, `+0x380` packed map. Per split: `+0x160` start, `+0xa0` texture matrix,
  `+0x60` light view, `+0xe0` box, `+0xf8` bias, `+0x0c` size, `+0x18` image (-> `+0x70`/`+0x60` API texture). The mask filter
  `illum+0x448` (`+0xfc`, `+0x114`), material admin `+0x298` (current mask), `+0x290` (depth map).
* Sun: eCDirectionalLight_PS +0x80 / +0x8c, colour / intensity (or the light list +0x04..+0x24 of any light set).
* Sky: skydome via `GetSkydome` 0x574690 (World_MCP property 0x21) fields of 2.7, eCShaderSkyDome `+0xf0`, fog `cam+0x15c0`.
Hooks in place (`engine/symbols.h`): RenderShadowMaps pre-hook (size / range / split overrides, stride), UpdateShadowMaps
pre-hook (true sun), ExecuteShadowMap / BeginZPass / EndZPass / UpdateShadowMask / UpdateSkydomeEnvCubemap phase markers,
eCSkydome_PS::Render / CreateRenderContext, hemisphere AddToLightList post-hook, FindWightedStaticLights / RegisterDynamicLightPS
pass-throughs, the weather state getters. Missing for a full takeover: a pre-hook that can **skip** the cascades
(`RenderShadowMaps` returning true without rendering, or the RenderPrePass 0x59e840 / vtable slot of 0x9a98b0 `+0x28`), and the
skydome render context's slot (`0x9ddd90 +0x00`) to replace the sky draw at its list position.

Our native cascades (`enh/native_shadows.*`, native-renderer.md 11) vs the engine:

| aspect | engine | native |
|---|---|---|
| where | main camera pre-pass inside eCImageFilterScene, before the scene target | AfterDepthPrepass injection point |
| count / splits | 2-3, `((Max-1) i/N + 1 - Max^(i/N)) f + Max^(i/N)` from config (High 1012 / 2230 / 7500) | 3, fixed 1500 / 4000 / 10000 |
| fit | incremental bCSphere::Merge of the slice corners | centroid + max distance, radius ceil to 10 |
| depth range | light-space z from -100000 (absolute) to sphere max | eye = centre - dir (r + 15000), range 2r + 15000 |
| snapping | fmodf of the projected translation by 2/size | floor of the light-space centre by 2r/size |
| direction | shadow dir +0x8c (0.5 deg steps) | true dir +0x80 |
| map | DF24, size from config (1024 High) | D32F 2048 |
| bias | texture-space z offset 1e-4 + 3e-5 i, no slope | slope-scaled 2.5 texels + normal offset 1.5 texels |
| filter | SSM: 8 dithered point taps; SSM_Low: 4-tap bilinear PCF; + mask Blur | 3x3 PCF in the opaque shader |
| far fade | 0.8 Max .. Max (FadOut), x terrain VSM | last cascade fades over 15 % into the engine mask |
| casters | Building / Object with CastDirLightShadows, db node flag 8, per-entity view distance; every render context incl. skinned, SpeedTree, vegetation | static mirror parts only (trees, characters, instanced via the engine's cascades still) |
| terrain map | G32R32F VSM, Terrain / Building casters, blurred 7+7 | none (the engine's map stays in the mask) |
| output | mask `.a` sampled by every material | used only by the native opaque shader; the engine mask feeds everything else |

Deviations that matter for parity: split positions, filter (dither vs PCF), the missing terrain VSM term in native-only pixels, the
direction step, and the cascade far fade. Point-light shadows (packed atlas, PSSM channels), the sky cube, the sky grid and the weather
hookup are not reproduced natively yet.

## 6. Open

* Which dynamic-light context wins the single pre-pass op when the hemisphere is requested before the sun (1) [?]; whether the
  terrain map's half extent really is the camera far (2.1 vs shadows.md 3.3's 59 units/texel) [?].
* eCShadowMapPointLight / SpotLight cube users; point format flag (+0x180 probe) [?]. Live check of PSSM channels and the
  ShadowMaskIndex -> selector mapping in the material shaders [I].
* Exact eCShaderSkyDome field -> c31..c34 / c6..c7 mapping (shader version 0x9da420 constant upload not dumped) [I]; dome blend
  mode 4 and depth-write rule [I]; `post_sky.fx` caller [?].
* `+0x450` downscale filter class (FUN_18014f410) and the random (dizzer) texture size [?]; multiplication order of R [I].
* `eCShadowMapBase::UpdateFrustumFilter` 0x5a0ea0 (per-cascade caster culling volume) belongs to the frustum-database slice.
