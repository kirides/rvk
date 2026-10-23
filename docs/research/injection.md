# Enhancement plumbing: GLSL shader pipeline, ABI extension slots, injection points (M6 do-now items 5, 6)

Where an enhancement plugs in. Hand-written GLSL shaders (`shaders/`) are compiled and validated while building and embedded
into `d3d9.dll`; they share one ABI header with the translated D3D9 shaders; every draw now carries a draw id, flags and the
address of a per-frame block (camera, previous camera, time, frame index, image roles, feature parameters); passes run at
well-defined points of the engine's frame (after the depth pre-pass, after the opaque scene, after the refraction copy, after
the main view, before the GUI) with a transient-image helper and sync2 barriers. A debug view (`debug.inject_view`) uses all
of it. With nothing enabled the frame renders as before (section 7).

Legend: **[C]** confirmed (measured, in the game or in tests), **[I]** inferred / proposed.

## 1. What exists

| piece | files |
|---|---|
| build-time GLSL compiler (glslang + spirv-val, depfile, C++ header output) | `tools/glslc/main.cpp` (`rvk_glslc`), `CMakeLists.txt` (`RVK_SHADERS`) |
| hand-written shaders | `shaders/include/rvk_abi.glsl` (the ABI), `shaders/include/rvk_common.glsl` (helpers), `shaders/debug/*`, `shaders/test/*` (layout probes for the tests) |
| embedded shaders + ABI text | `src/shader/embedded.{h,cpp.in}` (generated `build/generated/embedded_shaders.cpp`) |
| ABI (C++ side): 48-byte push constants, FrameBlock, pass push header, draw flags, reserved spec-constant ids | `src/shader/abi.h` |
| translator: shared ABI header, variant hooks (extra globals, epilogue, named-constant overrides) | `src/shader/glsl.{h,cpp}` |
| FrameBlock contents (pure) | `src/d3d9/scene.{h,cpp}` (`fillFrameBlock`) |
| FrameBlock upload, per-draw push constants | `src/d3d9/device.cpp` (`frameBlockAddress`), `src/d3d9/draw.cpp` |
| injection points: registry + once-per-view guard (pure), `Device::inject`, `PassContext` | `src/d3d9/inject.{h,cpp}`, `src/d3d9/device.{h,cpp}` |
| triggers | `src/engine/scene_hooks.cpp` (EndZPass, DoBeginRenderAlpha, Execute, eCApplication::Draw), `src/hal/vulkan/resources.cpp` (CopyCurrentRTToImage) |
| pass building blocks: pass pipeline layout (set 0 bindless + set 1 push descriptors + 128 B push constants), compute / fullscreen pipelines, transient images | `src/vk/passes.{h,cpp}` |
| the debug enhancement `debug.inject_view` | `src/enh/debug_view.{h,cpp}`, `shaders/debug/inject_view.{glsl,comp,frag}`, `shaders/debug/fullscreen.vert`; setting in `src/core/settings.cpp`; wired in `src/hal/vulkan/device.cpp` |
| overlay: registered passes in "Frame structure" | `src/overlay/overlay.cpp` |
| `[vulkan] sync_validation` (`run.ps1 -SyncValidation`) | `src/vk/context.cpp` (VkValidationFeaturesEXT + VK_EXT_layer_settings `validate_sync`) |
| tests | `tests/inject_tests.cpp`; `rvk_vktest --passes [--hazard]` (GPU, synchronization validation) |

## 2. GLSL build pipeline

**Decision: `rvk_glslc`, a small tool of this repo built from the vcpkg glslang and SPIRV-Tools libraries** the renderer
already links. Any developer with the vcpkg manifest can build; no Vulkan SDK (glslc / glslangValidator / spirv-val) is
needed on the build machine, and the compiler version is the one the runtime translator uses (vcpkg baseline). Shaders are
GLSL 4.60 only (user decision, PLAN.md); HLSL/DXC is not supported.

* `rvk_glslc compile <in> -o <out.h> --name <ident> -I shaders/include --depfile <out.h.d>`: stage from the extension
  (`.vert .frag .comp .geom .tesc .tese`), `#include "..."` relative to the including file, then `-I`
  (`GL_GOOGLE_include_directive` is enabled by a preamble), Vulkan 1.3 / SPIR-V 1.6, glslang's optimiser on, then
  `spvtools::SpirvTools::Validate` (Vulkan 1.3 env). Errors are printed as `file:line: message` and fail the build. Output:
  `build/generated/shaders/<path>.h` with `inline constexpr std::uint32_t <ident>[]`.
* **Dependency tracking**: each command writes a Makefile-style depfile of every file it included; CMake's `DEPFILE` (Visual
  Studio generators since CMake 3.21; CMP0116) re-runs exactly the commands whose includes changed [C: touching
  `rvk_common.glsl` rebuilt the 3 shaders that include it, not `fullscreen.vert` / `abi_layout.comp`].
* `rvk_glslc text shaders/include/rvk_abi.glsl ...` embeds the ABI header as text for the translator (section 3).
* Embedding: `src/shader/embedded.cpp.in` -> `build/generated/embedded_shaders.cpp` (the shader table), compiled into
  `rvk_core`: `shader::embedded::spirv("debug/inject_view.comp")` returns the words, `abiGlsl()` the header text. Nothing is
  deployed besides `d3d9.dll`.
* **Adding a shader**: put it under `shaders/<dir>/`, add the path to `RVK_SHADERS` in `CMakeLists.txt`, re-run
  `cmake --preset default`; load it with `shader::embedded::spirv("<dir>/<file>")`.

## 3. ABI extension slots (do-now item 5)

One header, `shaders/include/rvk_abi.glsl`, is the GLSL side of `src/shader/abi.h`: hand-written shaders `#include` it, the
translator pastes it verbatim after each translated shader's `#version` line (`GlslGenerator::header`; translator version
2, so the SPIR-V disk cache and the pipeline keys of earlier runs are rebuilt once). **Consistency check** [C]: rvk_tests
`abi_glsl_layout_matches_cpp` reads the member offsets (`OpMemberDecorate Offset`) of `RvkPush`, `RvkDraw`, `RvkStage`,
`RvkFrame` from the embedded SPIR-V of `shaders/test/abi_layout.comp` and compares them with `offsetof` of the C++ structs
(and the member count); `abi_glsl_pass_push_matches_cpp` does the same for the pass push header and the debug view's push
block; `glsl_abi_header_and_hooks` checks a translated shader's push block is the 48-byte one.

### 3.1 Push constants of draws (48 of the 128 bytes every device has; vertex + fragment)

| offset | C++ (`abi::PushConstants`) | GLSL (`pc.`) | content |
|---|---|---|---|
| 0 | `uint64 vsConsts` | `RvkStage vs` | vertex StageConsts (D3D9 c/i/b registers) |
| 8 | `uint64 psConsts` | `RvkStage ps` | pixel StageConsts |
| 16 | `uint64 draw` | `RvkDraw draw` | DrawBlock (bindless indices, half-pixel fix, alpha test, debug tint) |
| 24 | `uint64 frame` | `RvkFrame frame` | **FrameBlock** of the frame (always set) |
| 32 | `uint64 prevVsConsts` | `RvkStage prevVs` | last frame's vertex constants of the same geometry: **0 for now** (deferred, 3.4) |
| 40 | `uint32 drawId` | `uint drawId` | the draw's index in the frame = its `draws::Record` index |
| 44 | `uint32 flags` | `uint flags` | bits 0-7 `draws::Kind`, 8-11 `scene::Pass`, 12-15 `scene::View`, bit 16 tinted (`abi::drawFlags`) |

### 3.2 FrameBlock (std430, 896 bytes, `RvkFrame`)

| offset | field | content |
|---|---|---|
| 0..447 | `viewProj`, `invViewProj`, `view`, `proj`, `invView`, `invProj`, `prevViewProj`, `prevInvViewProj` | the engine's row-vector matrices stored as they are: GLSL reads the transpose, so `clip = f.viewProj * vec4(world, 1)` [C: unit test against the C++ row-vector product] |
| 512 | `cameraPos` | xyz, w = engine far (c11.w; the depth map stores view depth / w) |
| 528 | `prevCameraPos` | xyz |
| 544 | `depthParams` | projection near, projection far, engine far, 1 / engine far |
| 560 | `viewport` | scene HDR target width, height, 1/w, 1/h (back buffer size before it is known) |
| 576 | `jitter` | xy this frame's sub-pixel jitter (px), zw previous: 0 (no TAA yet) |
| 592 | `time` | seconds since device creation (at the main view), seconds since the previous frame's main view |
| 608 | `frameInfo` | frame index, flags (`RVK_FRAME_CAMERA_VALID` 1 / `_PREDICTED` 2 / `_PREV_VALID` 4), main views begun so far |
| 624 | `images` | bindless t2D slots of the scene HDR target, refraction copy, depth map; tCube slot of the sky cube (0 = unknown) |
| 640 | `params[16]` | feature parameters (`abi::kFrameParam*`; `kFrameParamDebugView` = 0) set with `Device::setFrameParam` |

**Upload**: `Device::frameBlockAddress()` fills it (`scene::fillFrameBlock`, pure, unit-tested) into the frame's ring on first
use and again after the camera, an image role or a parameter changed; `beginFrame` (also mid-frame flushes) forgets the
address because ring memory belongs to one submission. **Before the main view** of a frame (sun cascades, sky cube) the
camera fields hold the last frame's camera with `CAMERA_PREDICTED`; from the main view on this frame's (`CAMERA_VALID`).
Measured [C]: ~3-4 uploads per frame (start, main camera, role changes), see section 8.

### 3.3 Pass shaders

Pass shaders (`#define RVK_PASS_SHADER` before the include) get no draw push block; theirs starts with
`RVK_PASS_PUSH_HEADER` (`RvkFrame frame; uint passId; uint passFlags;` = `abi::PassPushHeader`, 16 bytes) followed by their
own fields (<= 128 bytes). Pipeline layout (`vk::PassLayout`): set 0 = the bindless set (now also visible to compute), set 1
= the pass's push-descriptor set (storage images / buffers; `VK_KHR_push_descriptor` is part of the floor), one 128-byte
push range for vertex + fragment + compute.

### 3.4 Translator hooks (`shader::GlslOptions`)

Implemented now (inert when empty: the output is byte-identical, unit-tested):
* `extraGlobals`: GLSL after the generated globals (extra outputs, specialisation constants, helpers).
* `epilogue`: statements at the end of `main()`: pixel shaders after the alpha test / tint, before `oC#` are copied out;
  vertex shaders after the clip position `rvk_pos` is final, before the half-pixel fix (`rvk_pos` and the `o#` registers in
  scope). This is the hook for a **VS capture variant** (write `rvk_pos` / world position to an extra output or buffer) and
  for **extra MRT outputs** (motion = `rvk_pos` vs the same transform with `pc.prevVs`, normal from a varying).
* `constOverrides`: register -> GLSL expression (`$c` = the original value) for every read of that float constant, direct
  and relative; registers come from `ShaderSemantics` (CTAB names), e.g. `g_AmbientLight` c15..c21 -> a DDGI lookup.
* Specialisation-constant ids reserved: `abi::kSpecMotionOutput` 0, `kSpecNormalOutput` 1.

**Deferred** (needs an enhancement to justify it, roadmap 3.4 / M9-M10): the actual motion / normal outputs (extra colour
attachments change the pipeline keys and the draw path), the per-geometry previous-constants map behind `prevVsConsts`
(the geometry key must be verified in game: static meshes, instanced, skinned), TAA jitter (needs a TAA), wiring
`constOverrides` to CTAB names in `Device` (variants are translated like the tint variant: on first use, cached).

## 4. Injection points (do-now item 6)

### 4.1 Points [C: firing verified in game, frame-phases.md 3 for where they sit]

| point | trigger | state |
|---|---|---|
| `AfterDepthPrepass` | `EndZPass` in the main view | the linear depth map (R32F, view depth / engine far) complete |
| `AfterOpaque` | `CopyCurrentRTToImage` in Main/Solid (before the copy), else `DoBeginRenderAlpha` in Main | scene HDR = sky + solid + decals; water / alpha not yet; once per main view |
| `AfterRefractionCopy` | right after that copy | the refraction copy holds the opaque scene (incl. what `AfterOpaque` passes drew) |
| `BeforeAlpha` | `Device::prepareDraw` at the first draw of the Main view's Alpha or AlphaLate pass (refracted ops draw before the alpha list) | scene HDR = sky + solid + decals + refracted (water), no translucent effect (particles are in Alpha, precipitation in AlphaLate); once per main view; used by `upscale.opaque` (upscaling.md 8) |
| `AfterMainView` | the main view's `eCFrustumDatabase::Execute` returns | scene HDR complete; the post chain (eye adaptation, tonemap, DoF) not yet |
| `BeforeGui` | `eCApplication::Draw` entry | the back buffer holds the post-processed frame; no GUI yet |

Main-view points fire once per main view (`inject::Firings`; `PassContext::mainView` = 1 for the frame's first). A frame
without a main view (loading screens) fires only `BeforeGui`.

### 4.2 API

```cpp
// in a feature's setup (render thread, e.g. a settings callback); returns the pass id
std::uint32_t id = device.addPass(d3d9::inject::Point::AfterOpaque, "water.ssr",
                                  [this](d3d9::PassContext& ctx) { record(ctx); }, /*owner*/ this, /*order*/ 0);
device.setFrameParam(abi::kFrameParamWater, {strength, 0, 0, 0});   // read in GLSL as pc.frame.params[i]
...
void Water::record(d3d9::PassContext& ctx) {             // ctx.cmd is outside any rendering scope
    auto* tmp = ctx.device.transients().get("water.ssr", {VK_FORMAT_R16G16B16A16_SFLOAT, {w, h}});
    ctx.device.readable(ctx.depthMap, ctx.cmd);           // engine image -> SHADER_READ (sync2 barrier from its last use)
    tmp->image.transition(ctx.cmd, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);   // vk::createComputePipeline(.., embedded::spirv(..))
    layout_.bindSet0(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.device.bindless());
    layout_.pushStorageImage(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, 0, tmp->image.view());
    layout_.push(ctx.cmd, &push, sizeof(push));          // push.h.frame = ctx.frameBlock
    vkCmdDispatch(...);
    tmp->image.transition(ctx.cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ctx.sceneHdr->image().transition(ctx.cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);   // then vkCmdBeginRendering ...
}
device.removePass(id);                                    // or removePasses(owner); never from inside a pass
```

`Device::inject(point)`: returns after one bit test when nothing is registered there; otherwise ends the open rendering
scope, builds the `PassContext` (command buffer, point, main-view index, FrameBlock address, the tagged scene HDR / depth map /
refraction textures, render target 0's texture), runs the passes in order, and `invalidateRecordingState()`s (the draw path
re-binds its pipeline, set 0, vertex input, dynamic state and constants; the next draw re-begins rendering with LOAD). D3D9
state (bound targets, textures) is never touched by a pass. With `[debug] perf`, the GPU timer marks each pass
("pass <name> at <point>" in `-PerfFrame` output) and `perf:` lines show points run + CPU time.

**Sync**: no render graph. Images track their layout (`vk::Image::transition`: sync2 barrier whose source scope derives from
the current layout); `SHADER_READ_ONLY` now includes the compute stage, so engine images sampled by compute passes are
ordered after their writers. Transient images (`vk::Transients`, keyed by name, recreated when the description changes,
destroyed through `Frames::retire`, a bindless slot each) keep their layout across frames, so next frame's write is ordered
after this frame's read. The layout-based scope is coarse for `GENERAL` (all commands) but correct.

## 5. The debug enhancement `debug.inject_view` (proof)

Setting `debug.inject_view` (overlay category Debug, default off, A/B-eligible; `run.ps1 -Setting 'debug.inject_view=depth'`,
or `tint`). On: a pass at `AfterOpaque` of the first main view:
1. compute `shaders/debug/inject_view.comp` (8x8 groups over the scene HDR size) reads the FrameBlock: mode / strength /
   grid spacing from `params[0]`, **depth mode**: the engine's depth map (bindless slot via push) -> view depth
   (`* cameraPos.w`) -> world position (`invViewProj`, camera position, projection far: `rvkWorldFromViewDepth`) -> distance
   colour bands + a 5 m world-space grid; **tint mode**: a colour cycled by `time.x`; writes a factor into the transient
   RGBA16F image `debug.inject_view`;
2. fullscreen `fullscreen.vert` + `inject_view.frag` multiplies the scene HDR target by it (blend dst * src, dynamic
   rendering, LOAD/STORE) before the refraction copy, water, alpha and the post chain (so eye adaptation still applies).
Off: the pass is removed, the transient released: the frame is exactly the plain one.

Screenshots [C] (`rvk_camera`, `-Screenshots`): depth mode at pose 1 shows the grid lying on the beach in perspective,
vertical lines on the cliff faces, the grid continuing under the shallow water through the refraction copy, sky untouched;
during the camera flight (top-down frames 1600 / 1700) the squares stay attached to the ground and the drowned bodies;
the same pose rendered in two runs gives the same lines; tint mode tints everything incl. the sky (yellow-green at t ~ 60 s).
`rvk.log`: `d3d9: pass 'debug.inject_view' at AfterOpaque`, `vulkan: transient image debug.inject_view 1600x900`,
`enh: debug.inject_view depth at AfterOpaque: 1600x900, depth map slot 97`; `-Commands '1500: set debug.inject_view tint',
'1900: set debug.inject_view off'` switched modes live and removed the pass (`d3d9: pass 1 removed (0 passes)`). The
overlay (`-OverlayInScreenshots -Commands '540: overlay open'`) lists "Injection test view" under Debug and in the A/B
table (A depth, B off); its "Frame structure" header names the registered passes.

## 6. Validation

* `-Validation` runs with the view on (depth, tint, live switching): no messages [C].
* `[vulkan] sync_validation` (`-SyncValidation`, new): enables the layer's synchronization validation through
  `VkValidationFeaturesEXT` + `VK_EXT_layer_settings` (`validate_sync`); environment variables do not reach the game
  (risen-spass starts it). In-game run with the view on: no hazards [C]. Liveness: `rvk_vktest --passes --hazard` leaves a
  barrier out between two copies and the layer reports `WRITE_AFTER_WRITE` [C]. Limit [C]: this layer (SDK 1.4.363) does
  **not** report a missing barrier between two dispatches writing the pass's storage image (push descriptor) nor accesses
  through the bindless arrays, so sync validation covers attachments, transfers and layout transitions only; the
  storage / sampled accesses are covered by `rvk_vktest --passes` checking the pixels.
* `rvk_vktest --passes`: the debug view's compute + composite on a 64x64 target cleared to 0.5, tint mode at t = 0:
  every pixel = 0.5 x the expected colour, 0 validation messages with sync validation on [C].

## 7. Parity with everything off

* Translated corpus: `rvk_shadertool corpus corpus` and `--tint` 2483 / 2483 valid; `compare_disasm.py` 2483 / 2483 [C].
* **`refcompare.ps1 -ReferenceDir out\refcompare\20261021-190139`: 13 / 13 PASS** (SSIM 0.990-1.000, dluma -0.42..+0.65,
  no WARN, beach_sun psnr_q 39.4), run `out\refcompare\20261021-222140` [C]. The translated shaders now carry the larger
  push block and the FrameBlock declaration; the bindless set is also visible to compute; `SHADER_READ_ONLY` barriers include
  the compute stage; none of it changes a pixel.
* rvk_tests 74 / 74, `rvk_vktest` (contract), `rvk_vktest --passes`, `rvk_d3d9test` pass.

## 8. Costs [C]

`rvk_shadows` with `-Perf -Vsync off`, ~1450 draws per frame; baseline = the previous commit built in a worktree; runs
interleaved base / new / depth / base / new. The session stays at 80 fps (the display path caps it), so fps cannot show a
delta: compare the CPU / GPU times.

| run | fps | CPU engine+HAL / frame | of it draw calls | GPU / frame |
|---|---|---|---|---|
| base 1 / 2 | 79.9 / 79.9 | 5.19-5.22 / 5.21-5.26 ms | 2.15-2.19 / 2.12-2.16 ms | 1.96-1.98 / 1.98-1.99 ms |
| new 1 / 2 (everything off) | 79.9 / 79.9 | 5.07-5.27 / 5.07-5.24 ms | 2.16-2.21 / 2.19-2.22 ms | 1.93-1.96 / 1.93-1.95 ms |
| new, `debug.inject_view = depth` | 79.9 | 5.25-5.34 ms | 2.20-2.22 ms | 2.00-2.02 ms |

* Everything off: **+0.03-0.05 ms CPU per frame** in the draw calls (48 instead of 24 bytes of push constants per draw, the
  flags, the FrameBlock address check), 4.0 FrameBlock uploads per frame (3.5 KiB of ring), injection points 0 ms (a bit
  test); GPU unchanged (within noise). Against the frame interval (12.5 ms) that is 0.3-0.4 %, against the CPU frame
  0.6-1.0 %: within the M6 budget (< 2 % fps).
* Debug view on: the injected pass costs **0.044 ms GPU** at 1600x900 (compute + composite; `-PerfFrame 900`: "pass
  debug.inject_view at AfterOpaque"), 0.008 ms CPU per frame to record.

## 9. Deferred / open

* Motion / normal outputs, the previous-constants map (`prevVsConsts` = 0), TAA jitter, CTAB-name overrides in `Device`
  (3.4).
* A pass that needs the D3D9 draw state of the moment (e.g. replacing water draws) is not an injection point but a draw
  hook (draw tags + a skip / replace decision in `prepareDraw`): done in M7 as `d3d9::DrawReplacer` (docs/research/water.md
  4.1).
* Transients are not aliased and not size-budgeted; a render graph waits for a real need (GD3D11 lost devices with aliasing).
* Injection points inside views other than Main (shadow maps, sky cube) and around the post chain's individual filters.
