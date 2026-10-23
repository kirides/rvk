#pragma once

// Temporal anti-aliasing / upscaling with AMD FidelityFX Super Resolution 3.1 (docs/research/upscaling.md).
//
// * Camera jitter: `upscale.mode` != off hands the device a jitter source; each frame's first main view shifts the scene's
//   geometry by FSR's Halton sub-pixel offset (d3d9::Device::geometryJitter -> DrawBlock::posFix, FrameBlock::jitter); the
//   motion vectors stay jitter-free. "jitter_only" is the developer test: the shaking without the filter (a still camera's
//   frames must average to the unjittered image).
// * native_aa: render size = display size. A pass at AfterMainView (the scene HDR target is complete, linear, before the post
//   chain's eye adaptation / tonemapping) copies the hardware depth into an R32F image (shaders/upscale/prep.comp), runs
//   vk::Fsr on {scene HDR, depth, the G-buffer's motion} into a transient and copies the result back into the scene HDR target,
//   which the engine's post chain then reads. Needs the G-buffer (motion vectors; forced on by the HAL while this mode is on).

#include <array>
#include <cstdint>
#include <memory>

#include "d3d9/device.h"
#include "vk/fsr.h"
#include "vk/passes.h"

namespace rvk::enh {

enum class UpscaleMode : std::uint8_t { Off, NativeAa, JitterOnly };

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

private:
    void record(d3d9::PassContext& ctx);
    bool ensureFsr(std::uint32_t width, std::uint32_t height);
    void dropFsr();

    d3d9::Device& dev_;
    UpscaleMode mode_ = UpscaleMode::Off;
    unsigned flip_ = 0;
    float sharpness_ = 0.4f;
    bool failed_ = false;  // the DLL / the context / the pipeline is not available: native AA stays off this session
    vk::PassLayout layout_;  // set 1: one storage image (the R32F depth)
    VkPipeline prep_ = VK_NULL_HANDLE;
    std::shared_ptr<vk::Fsr> fsr_;  // retired with the frame that last used it
    std::uint32_t fsrWidth_ = 0, fsrHeight_ = 0;
    bool needReset_ = true;  // the next dispatch drops the history (first frame, size change, skipped frames)
    std::uint64_t frames_ = 0, skipped_ = 0;
    double lastLog_ = -1e9;
};

}  // namespace rvk::enh
