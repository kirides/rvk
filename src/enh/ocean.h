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

// mirror of shaders/water/ocean_foam.comp FoamPush
struct OceanFoamPush {
    shader::abi::PassPushHeader h;
    float window[4];        // first cell (cell index), cell size, N
    float prev[4];          // last frame's first cell, dt, history valid
    float floorMap[4];      // the floor field's mapping
    std::uint32_t tex[4];   // floor A, floor B, history, linear clamp sampler
    std::uint32_t info[4];  // linear wrap sampler
    float shoreWave[4];     // period, amplitude, breaker index, time
};
static_assert(sizeof(OceanFoamPush) == 112 && sizeof(OceanFoamPush) <= shader::abi::kPushConstantBudget);

// what the foam pass needs from the water module (the floor field, the waves rolling in)
struct OceanFoamInputs {
    float camX = 0.0f, camZ = 0.0f;
    float floorMap[4]{};
    std::uint32_t floorA = 0, floorB = 0, smpClamp = 0;
    float shoreWave[4]{};  // period (s), amplitude (world units), breaker index, time (s: the water's)
    double seconds = 0.0;  // for dt
};

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
    // the surf's foam coverage around the camera (ocean_foam.comp, docs/research/ocean.md 4.4): records this frame's step
    // (outside a rendering scope, after record); the image is then readable by the vertex / fragment stages
    void recordFoam(VkCommandBuffer cmd, VkDeviceAddress frameBlock, const OceanFoamInputs& in);
    bool foamReady() const { return foamValid_; }
    // the tiles' temporal foam (whitecaps) and the foam buffer start over (a jump of the water's clock: their history is of
    // another moment)
    void resetHistory() {
        for (auto& t : tiles_) t.cleared = false;
        foamValid_ = false;
    }
    std::uint32_t foamSlot() const { return foamSlot_[static_cast<std::size_t>(foamCur_)]; }  // bindless t2D (wrap sampling)
    std::array<float, 4> foamMap() const;  // xy the window's first cell (world), z its extent, w 1 / extent

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
    bool createFoam();
    void foamStep(VkCommandBuffer cmd, VkDeviceAddress frameBlock, const OceanFoamInputs& in, double t, float dt, const int prevFirst[2]);
    void destroyFoam();
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
    // the foam buffer: two images (this frame's, last frame's = the history), toroidal window of cells around the camera
    vk::PassLayout foamLayout_;
    VkPipeline foam_ = VK_NULL_HANDLE;
    std::array<vk::Image, 2> foamImg_{};
    std::array<std::uint32_t, 2> foamSlot_{};
    int foamCur_ = 0;
    bool foamValid_ = false, foamCreated_ = false, foamFailed_ = false;
    int foamFirst_[2]{};
    double foamSeconds_ = -1.0;
    std::uint32_t foamPrerolls_ = 0;
};

}  // namespace rvk::enh
