#pragma once

// Temporal anti-aliasing / upscaling with AMD FidelityFX Super Resolution (FSR 3.1.x, optionally the FSR 4 ML upscaler; SDK 2.4.0;
// docs/research/upscaling.md).
//
// * Camera jitter: `upscale.mode` != off hands the device a jitter source; each frame's first main view shifts the scene's
//   geometry by FSR's Halton sub-pixel offset (d3d9::Device::geometryJitter -> DrawBlock::posFix, FrameBlock::jitter); the
//   motion vectors stay jitter-free. "jitter_only" is the developer test: the shaking without the filter (a still camera's
//   frames must average to the unjittered image).
// * Render scale: the quality modes render the engine's main view at 1 / ratio of the output (native AA 1, Quality 1.5,
//   Balanced 1.7, Performance 2, Ultra performance 3): the scene hook (engine/scene_hooks) shrinks the main camera's screen
//   rect while the engine builds its scene image, so every scene-sized target, viewport and mask follows; the GUI, the final
//   image effect pass and the back buffer stay at the output size.
// * The FSR pass at AfterMainView (the scene HDR target is complete, linear, before the post chain's eye adaptation /
//   tonemapping) copies the hardware depth into an R32F image (shaders/upscale/prep.comp) and runs vk::Fsr on {scene HDR,
//   depth, the G-buffer's motion} into an output-sized transient, which the engine's post chain then samples instead of the
//   scene image (Device::setSceneSubstitute: both are sampled by uv). Needs the G-buffer (motion vectors; forced on by the
//   HAL while an FSR mode is on).

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

#include "d3d9/device.h"
#include "vk/fsr.h"
#include "vk/passes.h"

namespace rvk::enh {

enum class UpscaleMode : std::uint8_t { Off, JitterOnly, NativeAa, Quality, Balanced, Performance, UltraPerformance };

constexpr bool usesFsr(UpscaleMode m) { return m >= UpscaleMode::NativeAa; }
// the per-dimension ratio output / render of an FSR mode (FfxApiUpscaleQualityMode); 1 for the others
float upscaleRatio(UpscaleMode m);

// FSR's jitter sequence: phase count = ceil(8 * (display width / render width)^2), offset = Halton(index % phases + 1; 2, 3) - 0.5
// (the FFX API's ffxQueryDescUpscaleGetJitterPhaseCount / GetJitterOffset compute the same; rvk_vktest --fsr compares them)
int upscaleJitterPhases(std::uint32_t renderWidth, std::uint32_t displayWidth);
std::array<float, 2> upscaleJitter(std::uint64_t frameIndex, int phases);
// the texture LOD bias for geometry rendered at `renderWidth` and upscaled to `displayWidth` (FSR's recommendation, what
// GD3D11 applies too): log2(render / display) - 1, i.e. -1 at native resolution, about -1.58 at 1.5x, -2 at 2x
float upscaleLodBias(std::uint32_t renderWidth, std::uint32_t displayWidth);

class Upscale {
public:
    explicit Upscale(d3d9::Device& device);
    ~Upscale();
    Upscale(const Upscale&) = delete;
    Upscale& operator=(const Upscale&) = delete;

    // flip: bit 0 = x, bit 1 = y: the jitter's sign on the geometry (a test of the convention; FSR is told the unflipped one)
    void apply(UpscaleMode mode, unsigned flip = 0);
    UpscaleMode mode() const { return mode_; }
    void setSharpness(float s) { sharpness_ = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s; }
    void setReactive(bool on) { reactive_ = on; }
    // upscale.provider: the ML upscaler (FSR 4) instead of FSR 3.1; the context is rebuilt when it changes. Falls back to FSR 3.1
    // (logged) when the FFX API does not offer an ML upscaler on this GPU.
    void setMl(bool on);
    bool mlActive() const { return mlActive_; }
    void setReactiveTuning(float cutoff, float scale) { reactiveCutoff_ = cutoff; reactiveScale_ = scale; }

private:
    void record(d3d9::PassContext& ctx);
    void recordOpaque(d3d9::PassContext& ctx);
    bool ensureFsr(VkExtent2D render, VkExtent2D display);
    void dropFsr();

    d3d9::Device& dev_;
    UpscaleMode mode_ = UpscaleMode::Off;
    unsigned flip_ = 0;
    float sharpness_ = 0.4f;
    bool reactive_ = true;
    bool ml_ = false;        // upscale.provider = fsr4
    bool mlActive_ = false;  // the current context is the ML upscaler
    float reactiveCutoff_ = 0.08f, reactiveScale_ = 1.0f;
    bool opaqueValid_ = false;  // this frame's BeforeAlpha snapshot (the scene without translucent effects) was taken
    std::atomic<int> phases_{8};  // the jitter sequence's length (the jitter source runs on the render thread; set from the sizes)
    bool failed_ = false;  // the DLL / the context / the pipeline is not available: FSR stays off this session
    vk::PassLayout layout_;  // set 1: one storage image (the R32F depth)
    VkPipeline prep_ = VK_NULL_HANDLE;
    std::shared_ptr<vk::Fsr> fsr_;  // retired with the frame that last used it
    VkExtent2D fsrRender_{}, fsrDisplay_{};
    bool needReset_ = true;  // the next dispatch drops the history (first frame, size change, skipped frames)
    std::uint64_t frames_ = 0, skipped_ = 0;
    double lastLog_ = -1e9;
};

}  // namespace rvk::enh
