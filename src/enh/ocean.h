#pragma once

// The FFT ocean's GPU simulation (docs/research/ocean.md 2): per cascade an N x N tile of displacement (dx, dy, dz, foam)
// and derivatives (slopes, horizontal stretch), recomputed every frame by compute passes (spectrum when the sea changes,
// time evolution, 2D inverse FFT, resolve + foam accumulation, mip chain). The textures are sampled through the bindless
// set by the sea grid (water_ocean.vert) and the water shading (water.frag). The spectrum is enh/ocean_spectrum.h.
// Owned by enh::Water; recorded from its AfterOpaque pass (before the water is drawn). Render thread.

#include <array>
#include <cstdint>

#include "enh/ocean_spectrum.h"
#include "shader/abi.h"
#include "vk/image.h"
#include "vk/passes.h"

namespace rvk::d3d9 {
class Device;
}

namespace rvk::enh {

// mirror of shaders/water/ocean_compute.glsl OceanPush
struct OceanPush {
    shader::abi::PassPushHeader h;
    float tile[4];
    float wind[4];
    float wind2[4];
    float swell[4];
    float time[4];
    float foam[4];
};
static_assert(sizeof(OceanPush) == 112 && sizeof(OceanPush) <= shader::abi::kPushConstantBudget);

class Ocean {
public:
    explicit Ocean(d3d9::Device& device);
    ~Ocean();
    Ocean(const Ocean&) = delete;
    Ocean& operator=(const Ocean&) = delete;

    // quality 0..2 (ocean_spectrum.h cascadesFor); recreates the tiles when it changes
    void setQuality(int quality);
    // frees the tiles (the sea simulation is off); setQuality creates them again
    void release();
    // the sea to simulate (re-runs the spectrum pass when it changed noticeably)
    void setSea(const ocean::SeaState& sea);
    // records the simulation of this frame (outside a rendering scope); t in seconds (any epoch)
    void record(VkCommandBuffer cmd, VkDeviceAddress frameBlock, double seconds);

    bool ready() const { return ready_; }  // the tiles hold a simulated frame
    const ocean::Cascades& cascades() const { return cascades_; }
    const ocean::SeaState& sea() const { return sea_; }
    std::uint32_t dispSlot(int c) const { return tiles_[static_cast<std::size_t>(c)].dispSlot; }
    std::uint32_t derivSlot(int c) const { return tiles_[static_cast<std::size_t>(c)].derivSlot; }
    std::uint32_t sampler() const { return samplerSlot_; }  // trilinear + anisotropic (when available), wrap
    float significantHeight() const { return hs_; }         // m, the whole spectrum (4 sqrt(sum of the cascades' variances))
    float peakWavelength() const { return peakLength_; }    // m, the wind sea's peak
    bool failed() const { return failed_; }

private:
    struct Tile {
        vk::Image h0, workA, workB, disp, deriv;
        std::uint32_t dispSlot = 0, derivSlot = 0;
        bool cleared = false;
    };
    bool createPipelines();
    bool createTiles();
    void destroyTiles();
    void push(VkCommandBuffer cmd, VkDeviceAddress frameBlock, int cascade, float t, float dt, float direction);
    void bindImages(VkCommandBuffer cmd, Tile& tile);

    d3d9::Device& dev_;
    bool failed_ = false, ready_ = false;
    int quality_ = -1;
    ocean::Cascades cascades_{};
    ocean::SeaState sea_{};
    ocean::SpectrumParams params_{};
    bool spectrumDirty_ = true;
    float hs_ = 0.0f, peakLength_ = 0.0f;
    std::array<Tile, ocean::kMaxCascades> tiles_{};
    vk::PassLayout layout_;
    VkPipeline spectrum_ = VK_NULL_HANDLE, evolve_ = VK_NULL_HANDLE, resolve_ = VK_NULL_HANDLE;
    std::array<VkPipeline, 3> fft_{};  // N = 128, 256, 512
    VkSampler sampler_ = VK_NULL_HANDLE;
    std::uint32_t samplerSlot_ = 0;
    double lastSeconds_ = -1.0;
    std::uint64_t frames_ = 0;
};

}  // namespace rvk::enh
