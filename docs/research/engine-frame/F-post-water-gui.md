# F. Post-processing chain, water pass, GUI and present (RE, 2026-10-27)

Slice F of the engine render-sequence decompile (A: frame, B: frustum database, C: materials, D: render contexts, E: shadows /
lights / sky). Headless Ghidra on Engine.dll (clone `clones\EngineF`), dumps `B:\DevDrive\risen-ghidra\frameF_dump1..7.txt`;
the game's effect data decoded offline: `frameF_data\library\compiled_imageeffects.bin` (from `data\compiled\library.pak`),
the post `.fx` HLSL sources (`frameF_data\effects\*.fx`, from `data\common\effects.pak`), parser
`B:\DevDrive\risen-ghidra\frameF_scripts\xieff_parse.py` (output `frameF_data\xieff_parsed.txt`), vtable scan
`frameF_scripts\vtables.py`. [C] = read in the decompilation / disassembly / data, [I] = inferred, [?] = open. RVAs are
Engine.dll. Singletons and device slot names as in A-frame.md (header); effect slots: `0x28` SetParameter, `0xc8`
SetTechnique, `0xd8` BeginTechnique, `0xe0` EndTechnique, `0xc0` IsParameterUsed (slots_generated.h). The admin, pool, job
builder and scene targets are summarised in A-frame.md 2.6 / 2.7; this file adds the graphs, the filters and their constants.

## 1. Call order

### 1.1 Frame tail [C]

```
eCApplication::DoRender 0x4c0d40 (A-frame.md 1)
  back buffer + depth bound, ClearViewport(7, colour 0x00000000, z 1.0, stencil 0), OnPreRender
  eCImageEffect::Execute(admin+0x90)  = the scene effect, default "DoF"           (2.1: 20 jobs, 19 post draws)
     job 1 = eCImageFilterScene::OnUpdateOutput 0x75cf10: the world (cull, shadow pre-passes, main Execute; A-frame.md 2.7-2.9)
     jobs 2..20 = post filters, the last one (Screen output) draws into the bound back buffer
  [eCImageEffect::Execute(admin+0x98)] = the overlay effect, when one runs          (2.6; frame-phases.md "Post{Post(1)}")
  back buffer + depth re-bound, OnPostRender, eCTimer::Update
eCApplication::Draw 0x4c13b0
  eCGuiModule2::Draw 0x519a60 (if module byte +0x3f)                                (4)
     BeginScene; CFXGui (GUI2.dll) update + draw -> eCGuiDevice2 BeginFrame / Draw* / EndFrame -> eCSprite batches
     EndScene
  DoSnapshot 0x4c0c50 if app+0x144                                                  (4.5)
  eCGfxMixerAdmin::SwapScreen 0x50fc90 -> device slot 0x108 SwapScreen(hwnd, NULL, NULL)
```

Without a scene effect (`+0x90` NULL, unknown DefaultEffect) `eCCameraBase::Render(NULL)` draws the world straight into the back
buffer: no scene targets, no refraction source (water refraction samples nothing), no post (A-frame.md 4).

### 1.2 Other presenting paths [C]

* **Loading screens** (frame-phases.md "None(9)"): `eCGuiModule2::RenderEdit(hwnd, wnd, clearColour)` 0x519c90, called from the
  synchronous load (menu-fps.md 3): save viewport; viewport = client rect; BeginScene; Clear(7, clearColour, 1.0); GetBackBuffer ->
  SetRenderTarget(bb); Clear(7, 0, 1.0); `ReSize`; `Draw(wnd)` (4); EndScene; windowed mode `0x90000000` -> mixer SwapScreen(hwnd,
  NULL, NULL), else device SwapScreen(hwnd, &clientRect, NULL); restore viewport. No DoRender, no image effect.
* **Videos** (intros, `eCApplication::PlayIntros` -> `PlayVideo(name, skippable)` 0x4c1b00): Bink (`binkw64.dll`; `BinkGoto`,
  `BinkSetSoundOnOff` imports). The video is an `eCGfxImageData` from `CreateImageFromFile` whose `+0x70` is the Bink player
  (`FUN_18070fbe0` open, `FUN_18070fd00` next frame into the texture [I]). Own loop with `PeekMessageA` (ESC up = skip when
  allowed): BeginScene, viewport = full window, Clear(target, black), `eCSprite::Begin(0)`, view = uniform scale (aspect fit,
  letterbox offsets 0.5 x the free space), one `eCSprite::Draw` of the video image, `Flush`; a second sprite for subtitles
  (two `eCGuiSystemFont2` "Arial", weight 600, heights -16 (-32 if the width > 2000) and -19 (-32 if the height > 2000));
  EndScene; SwapScreen [I: the tail of the loop read partly]. Not part of the HAL scene path, only sprites.
* `eCBenchmark::RunFillrateTest` / `RunVertexPerformanceTest`, editor `RenderAndDrawTo` / `DrawTo`: not in the game.

## 2. The image-effect graphs

### 2.1 Mechanism [C]

* An `eCImageEffect` (`+0x18` filter array, `+0x20` count) is a graph of `eCImageFilterBase` nodes with links
  `eSImageFilterLink {int srcFilter, int srcOutput, int dstFilter, int dstInput}` (`ApplyLinks` 0x662040 calls
  `dst->SetInputAt(dstInput, src, srcOutput)`, vtbl `+0x118`).
* `Execute` 0x662630 calls `AddJobs(-1)` on every filter: only **Screen-output** filters (`eCImageFilterRTBase` output mode `+0x38`
  == 1) and nested-effect / scene filters react, so the graph is pulled from its Screen node(s). `AddJobs` is depth-first over the
  inputs in input order, de-duplicated by (filter, output index), placeholder-protected against cycles (the luminance ping-pong,
  2.3), consumers after producers. Every link `AddRefOutputAt`s its source; `ExecuteJobs` runs `OnUpdateOutput(index)` per job, then
  `ReleaseOutputAt` on each input. **Inputs are pulled whether the technique samples them or not** (base `AddJobs` does not ask
  `IsInputUsed`).
* `eCImageFilterImageEffect` (`+0x30` effect name) exposes the outputs of the nested effect's **Public** filters (`+0x18` byte) in
  filter order, concatenated (`GetOutputCount` 0x6662f0 = sum; `GetOutputAt` / `AddJobs` / `AddRefOutputAt` /
  `ReleaseOutputAt` 0x6661f0 / 0x666460 / 0x666120 / 0x666050 subtract and forward). Output counts: scene 2 (ColorOut, DepthOut),
  RT filter 1 if output mode 0 else 0, texture / image filter 1.
* Output targets (`eCImageFilterRTBase::UpdateColorOutput` 0x6667f0): size mode `+0x48` 1 = ceil(input[`+0x58`].size x scale
  `+0x50/+0x54`), 0 = the stored `OutputSize` (`+0x80/+0x84`); format `+0x68`; from the admin pool (`AllocRenderTarget` 0x663950:
  first free entry with equal format + size, else a new `eCGfxImageData` with `eSGfxLayersDesc{w, h, depth 0, levels 1, usage 1
  (render target), format, pool 0}`). `ShareTexture` `+0x90` true: the target goes back to the pool after its last consumer
  (`ReleaseOutputAt` 0x6681f0 at refcount 1); **false: the filter keeps its target across frames** (temporal state: the
  luminance and bloom accumulators). Screen-mode filters have no target: `SetRenderTarget` 0x668370 only saves RT + viewport and
  draws into what is bound (the back buffer); texture-mode filters bind their target with the **saved depth surface** and
  viewport (0, 0, w, h, 0, 1); `ResetRenderTarget` restores both.
* `eCImageFilterEffect::OnUpdateOutput` 0x7597a0: `UpdateColorOutput`, `UpdateVisual` (`+0x178` 0x75adf0), `UpdateEffect`
  (`+0x180` 0x75aec0), `SetRenderTarget`, BeginScene, `ExecuteEffect` 0x75af70, EndScene, `ResetRenderTarget`. `ExecuteEffect`:
  `BeginEffectTechnique` 0x75b090 (`OnSetParams` 0x75c740, SetTechnique(`techniques[+0xf0]`), BeginTechnique -> pass count), per
  pass `ExecuteEffectPass` 0x75b1a0 (gfx effect `+0x28` = pass, `eCGfxEffect::Execute`) + `ExecuteVisual` 0x75ca60
  (`eCGfxVisual::Execute`: the quad), `EndEffectTechnique` 0x75b230 (`OnResetParams`, EndTechnique). Every technique used has
  one pass.
* **The quad** (`CreateVertexAndIndexData` 0x75c160, `CreateVertexDecl` 0x75bff0): TileCount `+0x98/+0x9c` (1, 1) -> a 2 x 2
  vertex grid, stride 0x14 = FLOAT3 POSITION (x = 2u - 1, y = 1 - 2v, z 0) + FLOAT2 TEXCOORD0 (u, v in 0..1); INDEX16 buffer;
  `TRIANGLESTRIP`, 2 primitives. No half-pixel shift in the positions: **every post VS adds `0.5 / TextureSize` to its texture
  coordinates** (D3D9 pixel-centre convention); a Vulkan port drops that offset.
* `.fx` handling (`SetEffectFileName` 0x75cb10): `eCGfxAssistantAdmin::CompileEffectFromFile` (macro `ATIF4` when the material
  admin's `+0x178` == 6, another macro at 4 [?]), the binary from `GetAuxBuffer` -> `eCGfxEffect`. `SetEffect` 0x758d50 lists the
  techniques (`+0xc8`, 0x18 each: name, handle, passes; index = declaration order) and parameters (`OnParseEffectParam`
  0x75bd00): **a texture parameter whose semantic starts with `TEXTURE` becomes an input** (`+0xb8`, 0x60 each: `+0` source
  filter, `+0x10` source output, `+0x18` desc copy, `+0x20` handle, `+0x58` semantic suffix = the "n" of `TEXTUREn`); input index
  = the texture parameter's declaration order. Every other parameter becomes an `eSImageEffectParam` (`+0xd8` array, `+0xe8`
  count) with a per-technique "used" mask (`+0x50`, IsParameterUsed); values stored in the `.xieff` override the `.fx` defaults
  (`OnRead` 0x75b4f0: name, type, class, elements, members must match).

### 2.2 The 13 effects (data, [C]: `xieff_parsed.txt`)

Formats: 21 A8R8G8B8, 113 A16B16G16R16F, 114 R32F. "Rel s" = input-relative x s, "Abs" = fixed size, "persist" = ShareTexture
false. Techniques by index (declaration order): `post_hdr.fx` 0 LogLuminance, 1 BrightPass, 2 BrightPassAlphaMask, 3 DownSample,
4 AdaptiveLiminance, 5 Copy, 6 ToneMap, 7 ExpToneMap, 8 HDRI, 9 ExpHDRI, 10 ColorOutput; `post_dof.fx` 0 DoFDown, 1 DoFBlurCocH,
2 DoFBlurCocV, 3 DoFNearCoc, 4 DoFSmallBlur, 5 DoF; `post_bloom.fx` 0 DownSample, 1 GlowH, 2 GlowV, 3 GlowVAddBloom2;
`post_streaks.fx` 0 Streak; `overlay_color.fx` 0 TOverlayColor, 1 OverlayRadialColor, 2 OverlayRadialBlur, 3 OverlayImage,
4 OverlayMovieStrips, 5 OverlayMovieStripsAbs; `post_radialBlur.fx` 0 RadialBlur; `post_wobble.fx` 0 TWobble;
`post_edgesmooth.fx` 0 EdgeSmooth.

| effect | filters (index name class: fx / tech, size, format) | links (src.out -> dst.in) | public |
|---|---|---|---|
| **RenderScene** | 0 RenderScene `eCImageFilterScene` EnabledHDR, EnabledDepthOutput | - | 0 (out0 ColorOut, out1 DepthOut) |
| **AvgLuminance** | 0 Down64x64 hdr/0 Abs 64² R32F; 1..4 Down32/16/8/4 hdr/3 Abs 32² .. 4² R32F; 5 Scene = RenderScene; 6 Down2x2 hdr/3 Abs 2²; 7 AdaptiveLiminance hdr/4 Abs 1² R32F persist; 8 AvgLuminance hdr/5 (Copy) Rel 1 (1²) R32F persist | Scene.0->Down64.0, Down64->32->16->8->4->Down2.0 chain, Down2->Adaptive.0, **AvgLum8.0->Adaptive.1**, Adaptive->AvgLum8.0 | 5 (2 outs), 8 -> outputs: 0 scene colour, 1 scene depth, 2 avg luminance |
| **Bloom** | 0 BrightPass hdr/2 **Abs 309x210** A8R8G8B8; 1 GlowH bloom/1 Rel 1; 2 AvgLuminance (nested); 3 Bloom bloom/3 Rel 1 **persist**; 4 Streaks streaks/0 Rel 1 | Avg.0->BP.0 (Src), Avg.2->BP.1 (ToneMap), Avg.1->BP.3 (Depth); BP->GlowH.0; GlowH->Bloom.0; Streaks->Bloom.2 (BloomTexture1); BP->Streaks.0 | 3 -> out0 bloom |
| **HDRI** | 0 HDRI hdr/8 **Screen**; 1 AvgLuminance (nested, public); 2 Bloom (nested); 3 ToneMap hdr/6 Rel 1 A8R8G8B8 (public) | Avg.0->HDRI.0, Avg.2->HDRI.1, Bloom.0->HDRI.2; Avg.0->ToneMap.0, Avg.2->ToneMap.1, Bloom.0->ToneMap.2 | 1 (3 outs), 3 -> outputs: 0 colour, 1 depth, 2 avg lum, 3 tonemapped |
| **DoF** (default) | 0 DownSampleCoC dof/0 Rel 0.25; 1 HDRI (nested); 2 BlurH dof/1 Rel 1; 3 BlurV dof/2 Rel 1; 4 CalcNearCoc dof/3 Rel 1; 5 SmallBlur dof/4 Rel 1; 6 DoF dof/5 **Screen**; 7 Bloom (nested); all A8R8G8B8 | HDRI.3->DSC.0, HDRI.1->DSC.1; DSC->BlurH->BlurV; BlurV->Near.0, DSC->Near.2; Near->Small.0; HDRI.3->DoF.0, HDRI.1->DoF.1, Small->DoF.2, BlurV->DoF.3, Bloom.0->DoF.4 | - |
| EdgeSmooth | 0 edgesmooth/0 Screen; 1 `eCImageFilterTexture` mode 2; 2 Texture mode 1 | T2->0.0 (scene copy), T1->0.1 (depth map) | - |
| RadialBlur | 0 radialBlur/0 Screen (Center (0.5, 0.5), BlurStart 1, BlurWidth -0.1); 1 HDRI (nested) | **HDRI.0** (the raw HDR colour, not out3) -> 0.0 | 0 |
| Wobble | 0 wobble/0 Rel 1 (texture, unused); 1 Scene = RenderScene; 2 wobble/0 Screen (Wobble 0.01, Speed 1, Amplitude 10) | Scene.0 -> 0.0 and -> 2.0 (no tone mapping) | - |
| Indicator | 0 RenderScene (nested); 1 `eCImageFilterCopy` Screen | Scene.0 -> Copy.0 (the raw scene to the screen) | - |
| OverlayBlackScreen / WhiteScreen | 0 overlay/0 Screen, OverlayColor (0,0,0,1) / (1,1,1,1) | - | - |
| OverlayMovieStrips | 0 overlay/5 Screen, colour (0,0,0,1), MovieStripsFactor 0.75 | - | - |
| OverlayHit | 0 overlay/2 Screen (colour (1, 0.6, 0.6, 0.1), BlurScale 0.1); 1 `eCImageFilterCopy` Abs 320x256 | Copy.0 -> 0.0 (Copy has no input: copies the **current RT** = the finished frame) | - |

"Rel" sizes at a 1600 x 900 scene: ToneMap 1600x900, DownSampleCoC / BlurH / BlurV / CalcNearCoc / SmallBlur 400x225, GlowH /
Bloom / Streaks 309x210 (follow BrightPass). The Absolute 309x210 of BrightPass is an editor leftover that the game uses as is
[C data; I: verify with `-LogFrame`].

### 2.3 The default frame (DoF), job order [C: derived from AddJobs + data; matches frame-phases.md `Post(19)`]

| # | job (filter, tech) | target | inputs (sampler) | notes |
|---|---|---|---|---|
| 1 | RenderScene | scene `+0x38` FP16, `+0x40` copy, `+0x48` R32F | - | the world (A-frame.md 2.7-2.9) |
| 2 | Down64x64 LogLuminance | 64² R32F | scene colour (point) | `log(1e-5 + dot(avg of 4 taps, (0.3, 0.59, 0.11)))`; the 4 taps are adjacent full-res texels: sparse |
| 3-7 | Down32 .. Down2 DownSample | 32² .. 2² R32F | previous | 2x2 box |
| 8 | AdaptiveLiminance | 1² R32F persist | Down2 (Src), AvgLum8 (ToneMap = last frame) | `L = exp(avg4)`; `L = old + (L - old)(1 - 0.98^(EyeAdapt x FrameTime))`, clamp to AvgLumClamp (0.4, 0.8); NaN -> old / clamp.x |
| 9 | AvgLuminance Copy | 1² R32F persist | Adaptive | copy for this frame's consumers and next frame's Adaptive |
| 10 | BrightPass BrightPassAlphaMask | 309x210 | scene colour, avg lum, depth (`TEXTURE3`) | 4 adjacent taps x ToneMapFactor 0.6 / avgLum; `c *= 1 + c / WhitePoint(1)`; `max(c - BLOOMTHRESHOLD, 0)`, `/= (c + BLOOMOFFSET)`; **a = 1 where depth >= 1 (sky), else 0** |
| 11 | GlowH | 309x210 | BrightPass | 14-tap horizontal Gaussian (weights 0.266 .. 0.0175, taps at 0.5 + 2i texels) |
| 12 | Streaks Streak | 309x210 | BrightPass | 64 samples towards `SUNLIGHTSCREENVECTOR.xy x (0.5, -0.5) + 0.5`, colour x alpha (sky only), Density 0.7, Weight 0.2, Decay 0.98, x `SUNFLAREINTENSITY`: sun god rays |
| 13 | Bloom GlowVAddBloom2 | 309x210 **persist** | GlowH (Src), Streaks (`BloomTexture1`); in1 unconnected | vertical Gaussian + streaks, **alpha blend SRCALPHA / INVSRCALPHA onto last frame's bloom**, a = min(FrameRedundancy 30 x FrameTime, MaxOpacity 0.8) |
| 14 | ToneMap | 1600x900 A8R8G8B8 | scene colour, avg lum, (bloom: pulled, unused) | 2.4 |
| 15 | DownSampleCoC DoFDown | 400x225 | ToneMap, depth | colour = 4 taps; CoC = max over 4x4 depth taps of `sat(DoFEqWorld.x d + DoFEqWorld.y)` -> alpha |
| 16, 17 | BlurH, BlurV DoFBlurCoc | 400x225 | previous | 4-tap separable (taps at 1 and 3 texels, both sides) |
| 18 | CalcNearCoc DoFNearCoc | 400x225 | BlurV (`ColorTexture`), DownSampleCoC (`ColorTexture1`) | `coc = 2 max(blurred.a, shrunk.a) - shrunk.a`, rgb = shrunk |
| 19 | SmallBlur DoFSmallBlur | 400x225 | CalcNearCoc | 2x2 box |
| 20 | DoF | **back buffer** | ToneMap (src), depth, SmallBlur (med), BlurV (large), Bloom | 2.4; the post's only back-buffer draw |

Then the overlay effect (if any): 1 draw (2.6).

### 2.4 Shader maths of the two output stages [C: post_hdr.fx / post_dof.fx]

* **ToneMap** (`ToneMapV1` + `PS_ToneMap`, also `PS_HDRI` / Low preset): `L = dot(c, (0.3, 0.59, 0.11))`; blue shift `c = lerp(L
  (0.8, 0.8, 1.4), c, sat(4 L))`; `r = ToneMapFactor(0.6) / avgLum x L`; `c *= r (1 + r / WhitePoint(1)) / (1 + r)` (the colour is
  multiplied by the compressed luminance, not by compressed / L); `out.rgb = (dot(c, ColMatrix[0]), dot(c, ColMatrix[1]), dot(c,
  ColMatrix[2]))`, alpha = scene alpha. `PS_HDRI` adds `bloom x BLOOMINTENSITY` before the matrix. Written to A8R8G8B8 (UNORM
  clamp, no sRGB): the FP16 scene is in the material shaders' gamma-like space (water.md 2.3: `pow(.., 0.45)` in the lighting).
  `Gamma`, `ChromaticAdaption` are declared but unused.
* **DoF**: `small` = 5-tap cross of src; `med` = SmallBlur, `large` = BlurV (both 400x225, sampled at `TexCoord + 0.5 /
  TextureSize1`); `nearCoc = med.a`; depth `d` (linear / far, 1 = sky): sky -> `coc = nearCoc`, else `coc = max(nearCoc,
  sat(DoFEqFar.x d + DoFEqFar.y) x DoFNearFarRatio(0.4))`; weights `w = sat(coc x (-3, -3, -3, 3) + (1, 2, 3, -2))`, `w.yz =
  min(w.yz, 1 - w.xy)`; `colour = w.x src + w.y small + w.z med + w.w large`; `+= dot3(bloom x BLOOMINTENSITY, ColMatrix rows)`.
  Note: the scene colour already went through `ColMatrix` in ToneMap; bloom gets it here.

### 2.5 Constants and their sources [C]

Semantic parameters (`FUN_180759f00` 0x759f00 picks the class by semantic; Set = vtbl `+0x28`; vtables 0x9ee280 .. 0x9ee900):

| semantic | class (Set RVA) | value |
|---|---|---|
| `TEXTUREn` | input | the linked source's output image (`AttachImmImageData`) |
| `TEXTURESIZEn` | ImageSize (0x28a310) | (w, h) of the input whose semantic suffix is "n" (first match) |
| `FRAMETIME` | FrameTime (0x289aa0) | `eCTimer +0x2c` = `GetFrameTimeInSeconds` |
| `TIME` | Time (0x2899c0) | `eCTimer +0xc` (`GetTotalTimeFloat`, ms) / 1000 |
| `SCENEEXPOSURE` | (0x758ad0) | `eCWeatherAdmin +0x158` (`GetSceneExposure` 0x274410) |
| `BLOOMINTENSITY` | (0x758900) | weather `+0x15c` / `+0x158` (`GetBloomIntensity` 0x274420 / exposure) |
| `BLOOMTHRESHOLD` / `BLOOMOFFSET` | (0x758880 / 0x758800) | weather `+0x160` / `+0x164` |
| `COLORMATRIX` (float3[3]) | (0x758980) | row i = E t_i (s e_i + (1 - s)(0.3, 0.59, 0.11)); E = weather `+0x158`, s = `+0x170` (`GetSaturation` 0x274450), t = `+0x180..+0x188` (tint) |
| `BLENDVALUE` | OverlayBlendValue (0x758780) | image filter admin `+0xb0` (2.6) |
| `VIEW` / `PROJ` / `VIEWPROJ` / `INVVIEW` / `INVPROJ` / `INVVIEWPROJ` | Camera (0x289b10) kinds 0..5 | app camera (`app+0x38`, else `+0xc8`) view `+0x10`, projection `+0x50` (products / inverses) |
| `NEARFAR` | Camera 6 | camera `+0xbc` (near, far) |
| `SCREENSIZE` | Camera 7 | camera rect `+0xc4` (w, h), read live (the upscale hook's scaled rect only during the scene job) |
| `DEPTHTOWORLD` / `DEPTHTOVIEW` | Camera 8 / 9 | built as inv(view), `*=` inv(diag(P00 `+0x50`, P11 `+0x64`, 1, 1)), `*=` scale(far) / the same without the view (unused by the 13 effects) |
| `FOCALRANGENEAR` | Camera 10 | n = camera `+0xe4` / far: (-1/n, 1) (0 if n <= eps): near CoC = sat(1 - d / n) |
| `FOCALRANGEFAR` | Camera 11 | s = `+0xe8`, e = `+0xec` (far if e <= s), r = (e - s) / far: (1/r, -(s/far)/r) (0 if r >= 1) |
| `DEPTHTEXTURE` / `BACKBUFFERTEXTURE` | Texture (0x289790) mode 2 / 1 | material admin `+0x290` (the depth map) / `+0x268` (the refraction copy) |
| `IMAGE` | Image (0x289390) | a resource image (`+0x58` -> `+0x70`) |
| `SUNMATRIX` / `MOONMATRIX` / `STARSMATRIX` | SkyDome (0x289fc0) 0..2 | skydome `+0x310` / `+0x350` / `+0x2d0` |
| `SUNLIGHTVECTOR` | SkyDome 3 | skydome `+0xf0` -> `+0x318..+0x320` |
| `SUNLIGHTSCREENVECTOR` | SkyDome 4 | that direction to view space x far, x projection, x / y clamped to [-1, 1] |
| `SUNFLAREINTENSITY` | SkyDome 5 | max(0, -z_view(sun dir))² |

Camera `+0xe4..+0xec` is `eCCameraBase::FocalRange` (bCVector: near range, far start, far end; `AccessFocalRange` 0x81010 = `lea
+0xe4`), set every camera solve by Game.dll from the camera type's `FocalRange` / `EnableAutoFocus` properties
(`B:\DevDrive\risen-ghidra\camera_solver2_dump.txt`, `camera_types_dump.txt`) [C]; this answers A-frame.md 6 "+0xe4". The weather
admin values come from the weather states (`eCWeatherStates::GetExposure`, `GetBloomIntensity`, `GetBloomThreshold`,
`GetBloomOffset`, `GetSaturation`, `GetTintColor`: eCFloatScale / eCColorScale over the day; slice E).

Stored per-filter values (non-semantic, `.xieff` over `.fx`): ToneMap EyeAdapt 0.1, ToneMapFactor 0.6, WhitePoint 1.0,
AvgLumClamp (0.4, 0.8); AvgLuminance chain ToneMapFactor 1.0, WhitePoint 0.8; DoF DoFNearFarRatio 0.4 (the DoF-chain filters
0.0, unused there); Bloom FrameRedundancy 30, MaxOpacity 0.8 (GlowH 0.1 / 0.6, unused by GlowH).

**Eye adaptation in practice** [I: arithmetic]: with FrameTime in seconds (`GetFrameTimeInSeconds`) the per-frame weight
`1 - 0.98^(0.1 dt)` is 3e-5 at 60 fps (~10 min time constant), and the result is clamped to 0.4..0.8: the "average luminance" is
almost a constant set by its start value (persistent 1x1 target, first contents [?]) and the clamp. Measure before porting.

### 2.6 Overlay effects (admin `+0x98`) [C]

`StartOverlayEffect(effect | name, duration, fadeIn, fadeOut)` 0x663e40 / 0x663db0: `+0xa8` duration, `+0xa0` fade-in, `+0xa4`
fade-out, elapsed `+0xac` = 0. `Process` 0x663080 (each update): elapsed += `eCTimer +0x18` (last frame, s); weight `+0xb0` = 1,
x elapsed / fadeIn while fading in, x (duration - elapsed) / fadeOut in the last fadeOut seconds; elapsed > duration > 0 -> stop.
`StopOverlayEffect(fade)` 0x663d30: immediately or by setting duration = elapsed + fadeOut. `ReadSaveGame` 0x662e80 stops it.
Callers: Game.dll and Script.dll import Start / Stop (`EffectSystem::StartImageEffect` Script 0x205b80, `StopImageEffect`
0x205b60, Game.dll `gCEffectCommandStartImageEffect` with BlendIn / BlendOut times) [C exports; the names they pass: ?].
The overlay techniques (`overlay_color.fx`, vs_2_0 / ps_2_0, Z off): TOverlayColor `OverlayColor` with a x BLENDVALUE,
SRCALPHA / INVSRCALPHA; OverlayRadialBlur (OverlayHit) 8 taps scaled around the centre by `1 + dot(p, p) BLENDVALUE BlurScale
i/7` of the 320x256 copy of the frame, x OverlayColor, a = radial x RadialFactor, blended; OverlayMovieStripsAbs: no blending,
`clip(|y| / MovieStripsFactor - 1)`: opaque black bars (cinematic letterbox, 12.5 % top and bottom at 0.75).

### 2.7 Configuration [C]

* `/Engine/Render/PostProcessing/DefaultEffect` (`eSRenderOptions +0xf0`, default **"Bloom"** if the key is missing):
  ConfigDefault.xml and `ConfigPerformanceHigh/Normal.xml` "DoF", `ConfigPerformanceLow.xml` **"HDRI"** (tone map + bloom, HDRI
  filter to the screen, no DoF). `ActivateDefaultEffect` 0x663be0 at PostInitialize, `ApplyConfiguration` 0x663460 on options
  changes; unknown names log "image effect:%s nopt found!" and keep the old effect.
* `PostProcessingEnabled` (ConfigDefault.xml `<Render>`): the string is not in Engine.dll: unused.
* Effect data: `compiled_imageeffects.bin` (`#G3:/data/compiled/library/`); the raw `.xieff` (`#G3:/data/raw/library/imageeffects/`)
  are imported only in the editor or when the binary is invalid (`PostInitialize` 0x663470).
* Gamma ramp (not a shader): `eCGfxMixerAdmin::UpdateGammaRamp` 0x509bb0 -> device slot 0x220 SetGammaRamp: per channel
  `ramp[i] = clamp(((pow(i/255, g_c) 255 - 127) k + 127 + 255 b) 257, 0, 65535)` with `b` = brightness (`+0x30` master +
  `+0x44` effect), `k = exp(3 (contrast +0x34 + +0x48))`, `g_c = exp(-3 (gamma_c))` (R `+0x38 / +0x4c`, G `+0x3c / +0x50`, B
  `+0x40 / +0x54`); master from config `<Graphics><Mixer Brightness Contrast GammaRed/Green/Blue>` (all 0 = identity) [I key
  path], effect values by `SetEffect*` (scripts [?]).

## 3. Water and the refracted ops [C unless marked]

* `eCRenderContextMesh::Render` / `RenderAlpha` / `RenderRefracted` (0x5c73e0 / 0x5c73f0 / 0x5c7400) are all `jmp 0x5c7110`: a
  refracted op renders exactly like a solid one; the difference is the material (a refraction colour source makes the
  `ge_default` shader sample `smpRefract`, engine-gfx.md 8).
* Refraction source, in `eCFrustumDatabase::Execute` after the Solid lists (frame.txt): only when a render-target desc with an
  image is passed (the scene filter's `{+0x40, +0x48}`): EndScene, `EndMaterial`, `image->CopyCurrentRTToImage(1, vp, vp)` (vp =
  the viewport saved at Execute entry; the scene filter's FP16 `+0x38` is the current RT), material admin record `+0x248` =
  that image (`FUN_1806bc540`), BeginScene, `BeginMaterial` -> `ExecuteShaderSamplers` 0x6b7bc0 binds the admin's global
  samplers: **s0** shadow mask (record `+0x298`, image `+0x2b8`), **s1** refraction copy (`+0x248` / `+0x268`), **s2** depth map
  (`+0x270` / `+0x290`, set after the z pass to desc+8, cleared at every Execute entry), s4 / s5 (`+0x2c0`, `+0x2e8`); each record
  carries its sampler states (address U/V, min/mag/mip, LOD bias, sRGB; `0xabababab` = device default) and a fallback image
  (admin `+0x348`) when empty. Then `DoBeginRenderAlpha`, the refracted list `+0x1578`, the alpha list.
* In the non-HDR path (EnabledHDR false) the scene is the back buffer itself; the copy is A8R8G8B8 and the scene filter copies
  the back buffer once more after Execute to produce ColorOut (A-frame.md 2.7).
* Water material inputs (water.md 2.3, engine-gfx.md 8): s1 refraction (distorted by the normal map, rejected where the depth
  map is in front of the water), s2 depth map (depth fade), sky cube (128², `UpdateSkydomeEnvCubemap`) for the reflection, s0 the
  sun shadow mask (ocean). **No planar or screen-space reflection exists**; no image effect deals with water. Shore overlay =
  `Special_Water_Shore_*`, Alpha pass, additive, no z write; ocean = the flat 672,672-unit `Levelmesh_Oceanmesh_01` (water.md 2.2).
* **Underwater**: none of the 13 effects is an underwater effect and no engine code switches one on the camera's depth;
  `UnderWaterDepth` exists only as gameplay data (Game.dll / Script.dll strings) [C for the effect list, I: no underwater fog or
  tint anywhere in the renderer; water.md 6].

## 4. GUI and present [C unless marked]

### 4.1 `eCGuiModule2::Draw` 0x519a60

Timestamp; cursor position (GetCursorPos / ScreenToClient) mapped to GUI space `((x - +0x2c) / +0x20, (y - +0x30) / +0x24)` ->
CFXGui (GUI2.dll, `FFGui2_QueryModule`) vtbl `+0xe8`; CFXGui `+0x68` (update with `eCTimer +0x2c`); **BeginScene**; game: copy
the module's scale / offset into the GUI device (`gui+0x1e0` enable = module byte `+0x3c`, `+0x1e4/+0x1e8/+0x1ec` scale = module
`+0x20/+0x24/+0x28`, `+0x1f0/+0x1f4` offset = `+0x2c/+0x30`), CFXGui `+0x70` Draw(window) (GUI2.dll walks its window tree and
calls `eCGuiDevice2`); editor: another path; **EndScene**. Draws into the back buffer DoRender left bound (no clear, no depth
use).

### 4.2 `eCGuiDevice2` (Engine's implementation of the GUI2 device)

* `BeginFrame` 0x517290: `CreateSprite`; `eCSprite::Begin(0)` on `gui+0x1c0`; with scaling (`+0x1e0`): save the device view matrix
  (`+0x1f8`) and set view = Scale(`+0x1e4`) x Translate(`+0x1f0`, `+0x1f4`) (slot 0x288 SetViewMatrix); notify listeners
  (`+0x1d0` array: deferred fonts / bitmaps). `EndFrame` 0x517130: `Flush`, restore the view matrix, listeners, `Flush` again,
  sprite count `+0xd8` = 0, detach the sprite texture.
* `DrawImage(bitmap, rect, colour, flags, z)` 0x5173d0 -> `eCDeferredGuiBitmap2::Draw` -> `eCGuiTexture2::Draw` -> `eCSprite::
  DrawQuad`; `DrawImage(cursor, pos, ..)` 0x518080 -> `eCGuiCursor2::Draw` 0x537f40 (up to two bitmaps `-0x48 / -0x40`, size
  mode `-0x50`: 1 bitmap rect, 2 explicit `-0x60`, else 32 x 32; hotspot `-0x68 / -0x64`): **the cursor is a sprite of the same
  batch**; `DrawText*` -> fonts (fonts.md 1: stb_truetype atlas, `eCFont::DrawGlyph` -> `DrawQuad`); `DrawRect` 0x517260 is
  empty. Fallback cursor `GetFallbackCursor`.

### 4.3 `eCSprite` batcher (shared by GUI, fonts, videos)

* `Begin(flags)` 0x757ff0: `eCSpriteEffect::Create` 0x7584f0 = an **inline HLSL effect** (`g_mWorldViewProj`, `g_texDiffuse:
  DIFFUSE`, sampler clamp, linear min/mag/mip; technique `Main`: vs_2_0 `mul(pos, WVP)`, ps_2_0 `Diffuse x tex`, AlphaTest off
  (ref 0, GreaterEqual), Cull None, Fog off, Z off). Flags bit 2 clear (GUI): world = identity (slot 0x278), view = identity
  (0x288), GetViewport, sprite matrix `+0x48` = `D3DXMatrixOrthoOffCenterLH` (arguments not recovered; [I] 0..w, h..0, 0..1
  from the viewport); bit 2 set: `+0x48` = active camera projection. `SetBlend(0)`.
* `Draw(img, cx, cy, hw, hh, u0, v0, u1, v1, colour, flags)` 0x756d30 appends a 0x48-byte record: 4 corners (centre +- the
  half extents rotated by the sprite's 2x2 `+8..+0x1c`), uv rect `+0x20..+0x2c`, z `+0x30` (= sprite `+0x40`), image `+0x38`,
  colour `+0x40`, blend `+0x44` = flags & 0x7000. `DrawQuad(img, srcRect, dstRect, colour, flags)` 0x7575c0: uv = (texel + 0.5) /
  size, alignment by flags & 7 / & 0x38, tiling by repeat (`DrawClippedQuad`).
* `Flush` 0x757d10: optional sort (flags `0xc0` depth, `0x20` texture); SetTechnique + BeginTechnique; `g_mWorldViewProj` =
  the device view matrix (GUI scaling, slot 0x290 GetViewMatrix) combined with `+0x48` (SetParameter, 0x40 bytes; operand order
  garbled in the decompile, [I] view x ortho for D3D row vectors); `RenderBatches` 0x757180; EndTechnique.
* `RenderBatches`: per sprite, a new batch when the blend mode or the texture changes or the buffer is full (`DrawBatch`
  0x7570b0: `SetBlend`, `AttachImmImageData` + `eCGfxEffect::Execute` on texture change, `eCSpriteMesh::Draw`); one quad per
  `LockBuffer(n x 0x70, 0x70, n == 0 ? DISCARD : NOOVERWRITE)` (fonts.md 1). Vertex 0x1c: FLOAT4 position (x, y, z, w), D3DCOLOR,
  FLOAT2 uv (`CreateDelcaration` 0x528cf0); corners (p0, uv0,v0), (p1, u1,v0), (p3, u1,v1), (p2, u0,v1); static INDEX16 buffer
  `4q + {0, 2, 3, 0, 1, 2}` (`CreateIndexBuffer` 0x528a80, capacity 0x1000 quads); `DrawIndexed(TRIANGLELIST, 2n prims)`.
* `SetBlend(mode)` 0x7568f0 (SetAlphaBlending slot 0x1e0 + `SetMasterBlending(src, dst)`, D3DBLEND values): 0 SRCALPHA /
  INVSRCALPHA; 0x1000 ONE / ONE; 0x2000 SRCALPHA / ONE; 0x3000 DESTCOLOR / ZERO; 0x4000 DESTCOLOR / SRCCOLOR; 0x5000 ONE / ZERO,
  blending off.

### 4.4 Present

`eCGfxMixerAdmin::SwapScreen` 0x50fc90: optional `ms_pRegisterScreenSwapCall`, device `+0x108` SwapScreen(hwnd, src, dst)
(`eCApplication::Draw` passes NULL, NULL; our `HalDevice::SwapScreen` presents). Clears go through `ClearViewport` 0x50fbd0 with
the optional `ms_pRegisterViewportClearCall`.

### 4.5 Snapshot

`DoSnapshot` 0x4c0c50 (flag app `+0x144`, file name `+0x148`): device `+0x4f8` SaveBackbufferToFile(name) **after the GUI and
before the present** (the snapshot contains the GUI); logs "Saved snapshot".

## 5. Data structures [C]

| object | fields |
|---|---|
| `eCImageFilterAdmin` | `+0x38/+0x40/+0x48` registered filters; `+0x50/+0x58/+0x60` RT pool (0x20: image, format, w, h, last-use frame, in-use); `+0x68` effect map; `+0x90` scene effect; `+0x98` overlay; `+0xa0` fade-in; `+0xa4` fade-out; `+0xa8` duration; `+0xac` elapsed; `+0xb0` blend value; `+0xb4` frame counter |
| `eCImageEffect` | `+0x18` filters, `+0x20` count; serialized links 0x10 each (`OnRead` 0x662b50: u16 version, u8, u32 n, n x 16 bytes) |
| `eCImageFilterBase` | `+0x10` name, `+0x18` Public, `+0x1c` editor rect, `+0x2c` bool |
| `eCImageFilterRTBase` | `+0x38` output mode (0 texture, 1 screen), `+0x48` size mode (0 absolute, 1 input-relative), `+0x50` input size scale, `+0x58` input index, `+0x68` format, `+0x70` eSOutput (`+0x70` image, `+0x78` name, `+0x80/+0x84` size), `+0x88` refcount, `+0x90` ShareTexture |
| `eCImageFilterEffect` | `+0x98` tiles, `+0xa0` fx name, `+0xa8` `eCGfxEffect` (`+0x18` API effect, `+0x20` technique handle, `+0x28` pass), `+0xb0` visual, `+0xb8/+0xc0` inputs (0x60), `+0xc8/+0xd0` techniques (0x18), `+0xd8/+0xe0/+0xe8` params, `+0xf0` technique index |
| `eSImageEffectParam` | `+8` name, `+0x10` handle, `+0x18` semantic, `+0x20` class, `+0x24` type, `+0x28` rows, `+0x2c` columns, `+0x30` elements, `+0x48` owner filter, `+0x50` technique mask, `+0x58..` per class (kind, size suffix, image, texture mode `+0x60`) |
| `eCImageFilterScene` | `+0x30` EnabledHDR, `+0x31` EnabledDepthOutput, `+0x38` FP16 scene RT, `+0x40` copy image (= eSRenderTargetDesc +0), `+0x48` R32F depth map (= desc +8) |
| `eCImageFilterTexture` | `+0x38` mode (0 own `+0x40`, 1 material admin `+0x268` refraction copy, 2 `+0x290` depth map) |
| `eCImageFilterCopy` | `+0x98` input (source, output `+0xa8`); output image or screen: `CopyImageToCurrentRT` (texture `+0x60`) from the input, or `CopyCurrentRTToImage` (`+0x58`) into its target when it has no input |
| `eCSprite` | `+0` flags, `+8..+0x1c` 2x2 transform, `+0x40` z, `+0x48` projection, `+0x88/+0x90` bound / pending image, `+0x98/+0x9c` blend, `+0xa8/+0xb0` records (0x48), `+0xb8` `eCSpriteMesh` (`+0xd8` count, `+0xdc` start, `+0xe0` capacity), `+0xe8` sprite effect, `+0xf0` WVP handle, `+0xf8` texture handle |
| `eCGfxMixerAdmin` | `+0x10` device; `+0x30..+0x40` master brightness, contrast, gamma R/G/B; `+0x44..+0x54` effect values |

## 6. For a native implementation

**Data to read per frame**: admin `+0x90` / `+0x98` / `+0xb0` (which graphs run, overlay weight); the active graph's filters
(the decoded `compiled_imageeffects.bin` is static: the 13 graphs above can be hard-coded, or read from the live objects:
`eCImageEffect +0x18`, filter `+0xb8` inputs / `+0xe0` params / `+0xf0` technique); weather admin `+0x158` exposure, `+0x15c`
bloom intensity, `+0x160` threshold, `+0x164` offset, `+0x170` saturation, `+0x180..+0x188` tint; app camera `+0xbc/+0xc0`
near/far, `+0xe4..+0xec` focal range, `+0xc4` rect, view `+0x10`, projection `+0x50`; skydome (`eCIlluminationAdmin::GetSkydome`)
`+0xf0 -> +0x318` sun direction; `eCTimer +0x2c` frame time (s), `+0xc` total time; the persistent state is ours (1x1
luminance, 309x210 bloom accumulator) and must survive frames, be reset on device loss / resize like the engine's pool
(`ResetResources` frees all pool entries; non-shared targets are re-created).

**Port** (GLSL, `shaders/post/*`, injection point AfterMainView -> back buffer before BeforeGui): 1 LogLuminance + 5 DownSample
+ Adaptive + Copy (or a single compute reduction with the same log / exp / clamp maths), BrightPassAlphaMask, GlowH,
GlowVAddBloom2 (blend onto the persistent target), Streak, ToneMap, the five DoF stages and DoF (13 shaders, the HDRI and the
overlay techniques optional). Remove the `+0.5 / size` texel offsets; keep the engine's sparse 4-tap sampling only for bit
parity (refcompare), otherwise a proper filter is free. Sizes: Rel from the (render-scaled) scene size, BrightPass 309 x 210
fixed. The DoF pass is the only writer of the back buffer; GUI, overlay and snapshot follow unchanged.

**Hooks**: `eCImageEffect::Execute` 0x662630 (hooked: Post view) — for the scene effect run the engine's scene job (or the
native scene) and then our chain instead of `ExecuteJobs`; `eCImageFilterScene::OnUpdateOutput` 0x75cf10 (hooked: render scale);
overlays can stay the engine's (one draw through the HAL) or be ported (`overlay_color.fx` 3 techniques). Read-only state needs
no further hooks; `StartOverlayEffect` 0x663e40 / `ActivateEffect` 0x663f80 only if we want to log names. Gamma ramp: `HalDevice::SetGammaRamp` stores
the ramp and ignores it (as windowed D3D9 does); identity at default settings.

**What we have**: the HAL runs all of this today through our effect runtime (fx_2_0 binaries, `src/hal/vulkan/effect.cpp`,
fx2.md), the sprite path and fonts (fonts.md), phase markers Post / Gui (`engine/scene_hooks.cpp`), injection points
AfterMainView (FSR `upscale.fsr` upscales the FP16 scene there, before the tone map) and BeforeGui, the render-scale hook in
`OnUpdateOutput` (upscaling.md 7), the refraction-copy role tag (frame-phases.md 4) and the water replacement (water.md 4).

## 7. Open

* Which overlay effect produces the 1-draw `Post{Post(1)}` seen in every refcompare spot (frame-phases.md 3): read
  `admin+0x98`'s name in a run [?]; which names Game.dll (`gCEffectCommandStartImageEffect`) and Script.dll
  (`EffectSystem::StartImageEffect`) pass, and who activates RadialBlur / Wobble / Indicator / EdgeSmooth (`ActivateEffect`) [?].
* FrameTime unit / eye adaptation speed and the luminance value at run time (read the 1x1 targets) [?]; BrightPass's 309 x 210
  at run time (`-LogFrame`) [I].
* `eCImageFilterEffect::UpdateVisual` 0x75adf0 / `UpdateEffect` 0x75aec0 (visual render / cull / depth states of the quad,
  lazy compile) not dumped [?]; the macro at material admin `+0x178` == 4 [?].
* Sprite ortho matrix arguments and any -0.5 pixel shift (`D3DXMatrixOrthoOffCenterLH` call in `Begin`) [?].
* GUI2.dll's draw order inside `CFXGui +0x70` (cursor last?, clipping / scissor use) — GUI2.dll not opened [?].
* Bink frame upload (`FUN_18070fbe0` / `FUN_18070fd00`: texture format, YUV conversion) [?].
* Mixer gamma: exact config keys; who sets the effect gamma values (`SetEffect*`) [?]. (Vulkan mode stores and ignores the ramp,
  like windowed D3D9 [C code].)
* `post_sky.fx` (slice E), `post_SSM.fx` (shadow mask, slice E), `post_blur` / `post_desaturate` / `post_distort` / `editor_copy`
  / `pre_lightmap`: not referenced by any of the 13 effects (lightmap baking and editor) [C for the effect data].
