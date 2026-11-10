#pragma once

// Depth-only z passes (S-6, docs/research/frame-ownership.md 14.10; setting native.depth_z, Debug, Full, default off).
// The engine's main z pre-pass draws the view's opaque geometry into the R32F "depth map" (view depth / engine far, cleared to 1) with a pixel shader that writes
// nothing else, while the hardware depth holds the same triangles' depth. With the setting on
//   * the pre-pass loses its colour attachment (Device::DepthZ::Main: no R32F write per fragment, no pixel shader launch for non-cut-out draws, early-Z) and this class
//     derives the map ONCE from the hardware depth at AfterDepthPrepass (first of the pass order: every consumer of the map reads it afterwards), a full-screen triangle
//     into the engine's own image (shaders/depth/depth_z.frag.hlsl: view depth from the hardware depth, 1 for the sky);
//   * colour-less scopes (the sun cascades) draw their opaque casters with no pixel shader at all (Device::DepthZ::PsLess, d3d9/draw.cpp).
// Compare: the engine's z pass is left alone and a compute reduction (depth_z_compare.comp.hlsl) checks the derivation against the map it wrote: pixels beyond relative
// tolerances, the largest error (rvk.log `native: depth_z compare`). Off = the pass is not registered, no pipeline or buffer exists, nothing changes.

#include <array>
#include <cstdint>
#include <memory>

#include "d3d9/device.h"
#include "shader/abi.h"
#include "rhi/rhi.h"

namespace rvk::enh {

// mirror of shaders/depth/depth_z.hlsli DepthZPush
struct DepthZPush {
    shader::abi::PassPushHeader h;
    std::uint32_t depthTex;
    std::uint32_t mapTex;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t stats[2];
    std::uint32_t pad[2];
};
static_assert(sizeof(DepthZPush) == 48 && sizeof(DepthZPush) <= shader::abi::kPushConstantBudget);

class DepthZ {
public:
    enum class Mode : std::uint8_t { Off, On, Compare };

    explicit DepthZ(d3d9::Device& device);
    ~DepthZ();
    DepthZ(const DepthZ&) = delete;
    DepthZ& operator=(const DepthZ&) = delete;

    void setMode(Mode mode);  // render thread (the settings callbacks run at the frame boundary)
    Mode mode() const { return mode_; }

private:
    static constexpr std::uint32_t kSlots = 4;  // compare results in flight (read back once their frame completed)
    static constexpr std::uint32_t kStatsBytes = 64;
    struct Slot {
        std::unique_ptr<rhi::Buffer> gpu, readback;
        std::uint64_t frame = 0;  // Device::frameValue() of the compare (0 = free)
        std::uint32_t pixels = 0;
    };
    void record(d3d9::PassContext& ctx);
    void recordDerive(d3d9::PassContext& ctx);
    void recordCompare(d3d9::PassContext& ctx);
    bool ensureDerive(rhi::Device& rdev, VkFormat target);
    bool ensureCompare(rhi::Device& rdev);
    void collect();
    void report();

    d3d9::Device& dev_;
    Mode mode_ = Mode::Off;
    std::uint32_t passId_ = 0;
    std::unique_ptr<rhi::Pipeline> derive_, compare_;
    VkFormat deriveFormat_ = VK_FORMAT_UNDEFINED;
    bool failed_ = false;
    bool armed_ = false;  // the device drops the main z pass's depth map attachment
    std::array<Slot, kSlots> slots_;
    // the window of the 5 s line
    struct Window {
        std::uint64_t views = 0, pixels = 0, geometry = 0, over[5] = {}, skyDiff = 0;
        float maxRel = 0.0f, nearMax = 0.0f;
        std::uint64_t worstGeometry = 0;
    } win_;
    std::uint64_t derived_ = 0, skipped_ = 0, skippedKept_ = 0, derivedTotal_ = 0;
    double lastReport_ = 0.0;
};

}  // namespace rvk::enh
