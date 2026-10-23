#pragma once

// Temporal anti-aliasing / upscaling with AMD FidelityFX Super Resolution 3.1 (docs/research/upscaling.md).
//
// Step 1 (this file so far): the camera jitter. `upscale.mode` != off hands the device a jitter source: each frame's first
// main view shifts the scene's geometry by FSR's Halton sub-pixel offset (d3d9::Device::geometryJitter -> DrawBlock::posFix,
// FrameBlock::jitter); the motion vectors stay jitter-free. "jitter_only" is the developer test: the shaking without the filter
// (a still camera's frames must average to the unjittered image).

#include <array>
#include <cstdint>

#include "d3d9/device.h"

namespace rvk::enh {

enum class UpscaleMode : std::uint8_t { Off, NativeAa, JitterOnly };

// FSR's jitter sequence: phase count = ceil(8 * (display width / render width)^2), offset = Halton(index % phases + 1; 2, 3) - 0.5
// (the FFX API's ffxQueryDescUpscaleGetJitterPhaseCount / GetJitterOffset compute the same; rvk_vktest --fsr compares them)
int upscaleJitterPhases(std::uint32_t renderWidth, std::uint32_t displayWidth);
std::array<float, 2> upscaleJitter(std::uint64_t frameIndex, int phases);

class Upscale {
public:
    explicit Upscale(d3d9::Device& device);
    ~Upscale();
    Upscale(const Upscale&) = delete;
    Upscale& operator=(const Upscale&) = delete;

    void apply(UpscaleMode mode);
    UpscaleMode mode() const { return mode_; }

private:
    d3d9::Device& dev_;
    UpscaleMode mode_ = UpscaleMode::Off;
};

}  // namespace rvk::enh
