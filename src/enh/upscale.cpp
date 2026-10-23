#include "enh/upscale.h"

#include <cmath>

#include <spdlog/spdlog.h>

namespace rvk::enh {

namespace {

float halton(int index, int base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

}  // namespace

int upscaleJitterPhases(std::uint32_t renderWidth, std::uint32_t displayWidth) {
    const float ratio = static_cast<float>(displayWidth) / static_cast<float>(renderWidth > 0 ? renderWidth : 1);
    return static_cast<int>(std::ceil(8.0f * ratio * ratio));
}

std::array<float, 2> upscaleJitter(std::uint64_t frameIndex, int phases) {
    const int i = static_cast<int>(frameIndex % static_cast<std::uint64_t>(phases > 0 ? phases : 1)) + 1;
    return {halton(i, 2) - 0.5f, halton(i, 3) - 0.5f};
}

Upscale::Upscale(d3d9::Device& device) : dev_(device) {}

Upscale::~Upscale() { dev_.setJitterSource(nullptr); }

void Upscale::apply(UpscaleMode mode) {
    if (mode == mode_) return;
    mode_ = mode;
    if (mode == UpscaleMode::Off) {
        dev_.setJitterSource(nullptr);
        spdlog::info("enh: upscale: off");
        return;
    }
    // native AA: render size = display size, so 8 phases
    const int phases = upscaleJitterPhases(1, 1);
    dev_.setJitterSource([phases](std::uint64_t frame) { return upscaleJitter(frame, phases); });
    spdlog::info("enh: upscale: {}, jitter {} phases", mode == UpscaleMode::NativeAa ? "native AA" : "jitter only", phases);
}

}  // namespace rvk::enh
