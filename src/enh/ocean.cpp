#include "enh/ocean.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"
#include "shader/embedded.h"
#include "vk/context.h"

namespace rvk::enh {

namespace {

constexpr VkFormat kWorkFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkFormat kOutFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// foam: decay rate (1/s), the Jacobian below which the surface folds into foam, coverage per unit of (threshold - J)
constexpr float kFoamDecay = 0.35f;
constexpr float kFoamThreshold = 0.8f;
constexpr float kFoamGain = 2.0f;
// the surf's foam buffer: N x N cells of kFoamCell world units around the camera (245 m)
constexpr int kFoamSize = 1024;
constexpr float kFoamCell = 24.0f;
// the spin-up without history: 10 s of the water's time in steps of 0.25 s (the bores move ~75 units per step, their white
// fronts are ~300 units wide: no gaps)
constexpr int kFoamPrerollSteps = 40;
constexpr double kFoamPrerollStep = 0.25;
constexpr VkFormat kFoamFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

int log2i(int n) {
    int b = 0;
    while ((1 << b) < n) ++b;
    return b;
}

int mipCount(int n) { return log2i(n) + 1; }

}  // namespace

Ocean::Ocean(d3d9::Device& device) : dev_(device) {
    if (!createPipelines()) {
        failed_ = true;
        spdlog::error("enh: ocean: compute pipelines failed, the FFT ocean stays off this session");
    }
}

Ocean::~Ocean() {
    // (the RHI objects are retired with the frame: no GPU wait; the sampler lives as long as the device)
    destroyTiles();
    destroyFoam();
}

bool Ocean::createPipelines() {
    rhi::Device* rdev = dev_.rhi();
    if (rdev == nullptr) return false;
    const auto compute = [&](std::span<const std::uint32_t> spirv, const char* name) {
        rhi::ComputePipelineDesc d;
        d.cs.spirv = spirv;
        d.name = name;
        return rdev->createComputePipeline(d);
    };
    spectrum_ = compute(shader::embedded::spirv("water/ocean_spectrum.comp"), "ocean.spectrum");
    evolve_ = compute(shader::embedded::spirv("water/ocean_evolve.comp"), "ocean.evolve");
    resolve_ = compute(shader::embedded::spirv("water/ocean_resolve.comp"), "ocean.resolve");
    const VkPhysicalDeviceLimits& lim = dev_.ctx().properties().limits;
    for (int i = 0; i < 3; ++i) {
        const std::int32_t n = 128 << i;
        // one workgroup of N / 2 threads per line, 2 x N x 16 bytes of shared memory
        if (static_cast<std::uint32_t>(n / 2) > lim.maxComputeWorkGroupInvocations ||
            static_cast<std::uint32_t>(n / 2) > lim.maxComputeWorkGroupSize[0] ||
            static_cast<std::uint32_t>(n) * 32u > lim.maxComputeSharedMemorySize) {
            spdlog::info("enh: ocean: FFT size {} not supported by this GPU (limits)", n);
            continue;
        }
        // one module per size (HLSL has no specialisation constants for numthreads / groupshared sizes): OCEAN_N / OCEAN_LOG2N defines
        const std::string defines = std::format("OCEAN_N={};OCEAN_LOG2N={}", n, log2i(n));
        fft_[static_cast<std::size_t>(i)] = compute(shader::embedded::spirvVariant("water/ocean_fft.comp", defines), "ocean.fft");
    }
    // trilinear (+ anisotropic where the device has it: the distant sea at grazing angles keeps its waves instead of blurring flat), wrapping: the
    // tiles repeat
    rhi::SamplerDesc sd;
    sd.filter = VK_FILTER_LINEAR;
    sd.address = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sd.maxAnisotropy = 8.0f;
    samplerSlot_ = rdev->createSampler(sd);
    // the foam pass (one storage image; its history and the floor field through the bindless set). Optional: without it the
    // shading evaluates the surf's foam in place
    foam_ = compute(shader::embedded::spirv("water/ocean_foam.comp"), "ocean.foam");
    return spectrum_ && evolve_ && resolve_ && fft_[0] && samplerSlot_ != 0;
}

void Ocean::destroyTiles() {
    for (Tile& t : tiles_) t = Tile{};  // (retired with the frame; the sampled slots are released with the textures)
    ready_ = false;
}

bool Ocean::createTiles() {
    rhi::Device* rdev = dev_.rhi();
    const auto n = static_cast<std::uint32_t>(cascades_.size);
    for (int c = 0; c < cascades_.count; ++c) {
        Tile& t = tiles_[static_cast<std::size_t>(c)];
        rhi::TextureDesc d;
        d.extent = {n, n, 1};
        d.format = kWorkFormat;
        d.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        d.name = "ocean.h0";
        t.h0 = rdev->createTexture(d);
        d.name = "ocean.workA";
        t.workA = rdev->createTexture(d);
        d.name = "ocean.workB";
        t.workB = rdev->createTexture(d);
        d.format = kOutFormat;
        d.mips = static_cast<std::uint32_t>(mipCount(cascades_.size));
        d.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        d.name = "ocean.disp";
        t.disp = rdev->createTexture(d);
        d.name = "ocean.deriv";
        t.deriv = rdev->createTexture(d);
        if (!t.h0 || !t.workA || !t.workB || !t.disp || !t.deriv) {
            spdlog::error("enh: ocean: tile images {}x{} failed", n, n);
            destroyTiles();
            return false;
        }
        t.cleared = false;
    }
    spectrumDirty_ = true;
    frames_ = 0;
    lastSeconds_ = -1.0;
    const double mib = cascades_.count * double(n) * n * (3 * 16 + 2 * 8 * 4.0 / 3.0) / (1024.0 * 1024.0);
    spdlog::info("enh: ocean: {} cascades of {}x{} ({:.0f} MiB): tiles {:.0f} / {:.0f} / {:.0f} / {:.0f} m", cascades_.count, n, n, mib,
                 cascades_.c[0].length, cascades_.c[1].length, cascades_.c[2].length, cascades_.count > 3 ? cascades_.c[3].length : 0.0f);
    return true;
}

bool Ocean::createFoam() {
    rhi::Device* rdev = dev_.rhi();
    for (int i = 0; i < 2; ++i) {
        rhi::TextureDesc d;
        d.extent = {static_cast<std::uint32_t>(kFoamSize), static_cast<std::uint32_t>(kFoamSize), 1};
        d.format = kFoamFormat;
        d.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        d.name = "ocean.foam";
        foamImg_[static_cast<std::size_t>(i)] = rdev->createTexture(d);
        if (!foamImg_[static_cast<std::size_t>(i)]) {
            spdlog::error("enh: ocean: foam buffer {}x{} failed, the surf's foam is evaluated in place", kFoamSize, kFoamSize);
            destroyFoam();
            return false;
        }
    }
    foamCreated_ = true;
    foamValid_ = false;
    foamSeconds_ = -1.0;
    spdlog::info("enh: ocean: foam buffer {}x{} cells of {:.0f} units ({:.0f} MiB)", kFoamSize, kFoamSize, kFoamCell,
                 2.0 * kFoamSize * kFoamSize * 8.0 / (1024.0 * 1024.0));
    return true;
}

void Ocean::destroyFoam() {
    for (auto& img : foamImg_) img.reset();
    foamCreated_ = false;
    foamValid_ = false;
}

std::array<float, 4> Ocean::foamMap() const {
    const float extent = kFoamCell * static_cast<float>(kFoamSize);
    return {static_cast<float>(foamFirst_[0]) * kFoamCell, static_cast<float>(foamFirst_[1]) * kFoamCell, extent, 1.0f / extent};
}

void Ocean::recordFoam(rhi::CommandList& cl, rhi::GpuRef frameBlock, const OceanFoamInputs& in) {
    if (failed_ || foamFailed_ || !foam_ || quality_ < 0 || in.floorA == 0 || in.floorB == 0) {
        foamValid_ = false;
        return;
    }
    if (!foamCreated_ && !createFoam()) {
        foamFailed_ = true;  // not again this session
        return;
    }
    const double dtRaw = foamSeconds_ < 0.0 ? 1e9 : in.seconds - foamSeconds_;
    foamSeconds_ = in.seconds;
    const int prevFirst[2] = {foamFirst_[0], foamFirst_[1]};
    foamFirst_[0] = static_cast<int>(std::floor(in.camX / kFoamCell)) - kFoamSize / 2;
    foamFirst_[1] = static_cast<int>(std::floor(in.camZ / kFoamCell)) - kFoamSize / 2;
    // the history continues while the pass runs every frame and the window overlaps the last one
    const bool overlap = std::abs(foamFirst_[0] - prevFirst[0]) < kFoamSize / 2 && std::abs(foamFirst_[1] - prevFirst[1]) < kFoamSize / 2;
    if (foamValid_ && dtRaw >= 0.0 && dtRaw < 0.5 && overlap) {
        foamStep(cl, frameBlock, in, in.seconds, static_cast<float>(std::clamp(dtRaw, 0.0, 0.1)), prevFirst);
        return;
    }
    // no history (the sea comes into view, a pause, a jump of the water's clock, a teleport): the buffer is spun up over the
    // last kFoamPreroll seconds of the water's time, so the surf shows its steady state at once (no foam fading in; a capture
    // at a pinned game time sees the same foam in every run)
    foamValid_ = false;
    for (int i = kFoamPrerollSteps; i >= 0; --i) {
        const double ti = in.seconds - static_cast<double>(i) * kFoamPrerollStep;
        foamStep(cl, frameBlock, in, ti, i == kFoamPrerollSteps ? 0.0f : static_cast<float>(kFoamPrerollStep), foamFirst_);
    }
    if (foamPrerolls_++ < 4) {
        spdlog::info("enh: ocean: foam buffer spun up over {:.1f} s ({} steps)", kFoamPrerollSteps * kFoamPrerollStep, kFoamPrerollSteps + 1);
    }
}

// one step of the foam buffer at the water's time t (s) after dt (0: no history), the history's window prevFirst
void Ocean::foamStep(rhi::CommandList& cl, rhi::GpuRef frameBlock, const OceanFoamInputs& in, double t, float dt, const int prevFirst[2]) {
    const bool history = foamValid_ && dt > 0.0f;
    const int cur = foamValid_ ? 1 - foamCur_ : 0;
    const int prev = 1 - cur;
    rhi::Texture& out = *foamImg_[static_cast<std::size_t>(cur)];
    rhi::Texture& hist = *foamImg_[static_cast<std::size_t>(prev)];
    // this step's image: all of it is rewritten (after its last use: the water's sampling two frames ago, the foam pass
    // reading it as the history in the step before)
    cl.transition(out, VK_IMAGE_LAYOUT_GENERAL);
    // the first step: the history image gets a defined layout (never read: prev.w = 0)
    if (!foamValid_) cl.transition(hist, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    OceanFoamPush p{};
    p.h.frame = frameBlock.bits;
    p.window[0] = static_cast<float>(foamFirst_[0]);
    p.window[1] = static_cast<float>(foamFirst_[1]);
    p.window[2] = kFoamCell;
    p.window[3] = static_cast<float>(kFoamSize);
    p.prev[0] = static_cast<float>(prevFirst[0]);
    p.prev[1] = static_cast<float>(prevFirst[1]);
    p.prev[2] = history ? dt : 0.0f;
    p.prev[3] = history ? 1.0f : 0.0f;
    for (int i = 0; i < 4; ++i) {
        p.floorMap[i] = in.floorMap[i];
        p.shoreWave[i] = in.shoreWave[i];
    }
    p.shoreWave[3] = static_cast<float>(std::fmod(std::max(t, 0.0), 16384.0));
    p.tex[0] = in.floorA;
    p.tex[1] = in.floorB;
    p.tex[2] = hist.heapSampled();
    p.tex[3] = in.smpClamp;
    p.info[0] = samplerSlot_;
    cl.bindPipeline(*foam_);
    rhi::Texture* const storage[] = {&out};
    cl.setStorageTextures(storage);
    cl.setPushConstants(&p, sizeof(p));
    const std::uint32_t groups = (kFoamSize + 7) / 8;
    cl.dispatch(groups, groups, 1);
    cl.transition(out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    foamCur_ = cur;
    foamValid_ = true;
}

void Ocean::setQuality(int quality) {
    if (failed_) return;
    quality = std::clamp(quality, 0, 2);
    // the largest FFT this GPU runs
    while (quality > 0 && !fft_[static_cast<std::size_t>(quality)]) --quality;
    if (quality == quality_) return;
    destroyTiles();
    quality_ = quality;
    cascades_ = ocean::cascadesFor(quality);
    if (!createTiles()) {
        failed_ = true;
        return;
    }
    // the foam buffer with the tiles (not at its first use: no allocation in a later frame)
    if (!foamCreated_ && !foamFailed_ && foam_ && !createFoam()) foamFailed_ = true;
    setSea(sea_);
    spectrumDirty_ = true;
}

void Ocean::release() {
    if (quality_ < 0) return;
    destroyTiles();
    if (foamCreated_) destroyFoam();
    quality_ = -1;
    spdlog::info("enh: ocean: tiles released");
}

void Ocean::setSea(const ocean::SeaState& sea) {
    const auto diff = [](float a, float b, float rel) { return std::abs(a - b) > rel * std::max(std::abs(a), std::abs(b)) + 1e-4f; };
    const bool changed = spectrumDirty_ || diff(sea.windSpeed, sea_.windSpeed, 0.01f) || diff(sea.fetch, sea_.fetch, 0.02f) ||
                         diff(sea.windAngle, sea_.windAngle, 0.002f) || diff(sea.swellHeight, sea_.swellHeight, 0.01f) ||
                         diff(sea.swellPeriod, sea_.swellPeriod, 0.01f) || diff(sea.swellAngle, sea_.swellAngle, 0.002f) ||
                         diff(sea.scale, sea_.scale, 0.01f) || diff(sea.spreadXi, sea_.spreadXi, 0.01f) ||
                         diff(sea.swellSpread, sea_.swellSpread, 0.01f);
    sea_.choppiness = sea.choppiness;  // applied in the resolve pass every frame
    if (!changed) return;
    sea_ = sea;
    params_ = ocean::spectrumParams(sea_);
    ocean::Cascades stats = cascades_;
    // the variance sums on a coarser grid are close enough for the shading's roughness (the full grid costs ms at 512)
    stats.size = std::min(stats.size, 64);
    ocean::cascadeStats(stats, params_);
    double var = 0.0;
    for (int c = 0; c < cascades_.count; ++c) {
        cascades_.c[static_cast<std::size_t>(c)].heightVar = stats.c[static_cast<std::size_t>(c)].heightVar;
        cascades_.c[static_cast<std::size_t>(c)].slopeVar = stats.c[static_cast<std::size_t>(c)].slopeVar;
        var += stats.c[static_cast<std::size_t>(c)].heightVar;
    }
    const float hsOld = hs_;
    hs_ = static_cast<float>(4.0 * std::sqrt(var));
    if (std::abs(hs_ - hsOld) > 0.25f) {
        spdlog::info("enh: ocean: sea state: Hs {:.2f} m (U10 {:.1f} m/s, fetch {:.0f} km, swell {:.2f} m {:.1f} s, wind {:.0f} deg, scale {:.2f})",
                     hs_, sea_.windSpeed, sea_.fetch / 1000.0f, sea_.swellHeight, sea_.swellPeriod, sea_.windAngle * 57.2958f, sea_.scale);
    }
    peakLength_ = params_.windOmegaP > 0.0f
                      ? static_cast<float>(2.0 * ocean::kPi * ocean::kGravity / (double(params_.windOmegaP) * params_.windOmegaP))
                      : 0.0f;
    spectrumDirty_ = true;
}

void Ocean::push(rhi::CommandList& cl, rhi::GpuRef frameBlock, int cascade, float t, float dt, float direction) {
    const ocean::Cascade& c = cascades_.c[static_cast<std::size_t>(cascade)];
    OceanPush p{};
    p.h.frame = frameBlock.bits;
    p.tile[0] = c.length;
    p.tile[1] = c.kLow;
    p.tile[2] = c.kHigh;
    p.tile[3] = static_cast<float>(cascades_.size);
    p.wind[0] = params_.windAlpha;
    p.wind[1] = params_.windOmegaP;
    p.wind[2] = params_.windGamma;
    p.wind[3] = params_.windSpeed;
    p.wind2[0] = params_.windAngle;
    p.wind2[1] = params_.spreadXi;
    p.wind2[2] = params_.shortWaveCut;
    p.wind2[3] = static_cast<float>(cascade);
    p.swell[0] = params_.swellAlpha;
    p.swell[1] = params_.swellOmegaP;
    p.swell[2] = params_.swellAngle;
    p.swell[3] = params_.swellSpread;
    p.time[0] = t;
    p.time[1] = dt;
    p.time[2] = sea_.choppiness;
    p.time[3] = direction;
    p.foam[0] = kFoamDecay;
    p.foam[1] = kFoamThreshold;
    p.foam[2] = kFoamGain;
    p.foam[3] = ocean::kRepeatTime;
    cl.setPushConstants(&p, sizeof(p));
}

void Ocean::bindImages(rhi::CommandList& cl, Tile& tile) {
    // (the outputs: mip 0 of the mip chains)
    const rhi::StorageView views[5] = {{tile.h0.get(), 0, 0}, {tile.workA.get(), 0, 0}, {tile.workB.get(), 0, 0}, {tile.disp.get(), 0, 0}, {tile.deriv.get(), 0, 0}};
    cl.setStorageViews(views);
}

void Ocean::record(rhi::CommandList& cl, rhi::GpuRef frameBlock, double seconds) {
    if (failed_ || quality_ < 0 || !tiles_[0].disp) return;
    rhi::Pipeline* fft = fft_[static_cast<std::size_t>(quality_)].get();
    if (fft == nullptr) return;
    const float dt = lastSeconds_ < 0.0 ? 0.0f : static_cast<float>(std::clamp(seconds - lastSeconds_, 0.0, 0.1));
    lastSeconds_ = seconds;
    const float t = static_cast<float>(std::fmod(seconds, static_cast<double>(ocean::kRepeatTime)));
    const auto n = static_cast<std::uint32_t>(cascades_.size);
    const int count = cascades_.count;
    const std::uint32_t mips = static_cast<std::uint32_t>(mipCount(cascades_.size));

    // into GENERAL: the work images every frame (contents rewritten), h0 kept, the outputs' mip 0 from last frame's sampling (the other levels stay
    // readable until the blits); the same-layout transitions are the barriers after last frame's FFT. The first time and after resetHistory the foam
    // history in disp is cleared.
    // (one barrier for all of it)
    std::vector<rhi::TextureTransition> batch;
    for (int c = 0; c < count; ++c) {
        Tile& tl = tiles_[static_cast<std::size_t>(c)];
        for (rhi::Texture* x : {tl.h0.get(), tl.workA.get(), tl.workB.get()}) batch.push_back({x, VK_IMAGE_LAYOUT_GENERAL});
        for (rhi::Texture* x : {tl.disp.get(), tl.deriv.get()}) {
            if (tl.cleared) batch.push_back({x, VK_IMAGE_LAYOUT_GENERAL, 0, 1});
            else batch.push_back({x, VK_IMAGE_LAYOUT_GENERAL});
        }
    }
    cl.transitions(batch);
    bool cleared = false;
    for (int c = 0; c < count; ++c) {
        Tile& tl = tiles_[static_cast<std::size_t>(c)];
        if (tl.cleared) continue;
        // the foam history starts at zero
        const VkClearColorValue zero{};
        cl.clearTexture(*tl.disp, zero, 0, 1);
        cleared = true;
        tl.cleared = true;
        spectrumDirty_ = true;
    }
    if (cleared) cl.computeBarrier();  // (the clears' writes are visible to the passes)
    const std::uint32_t groups = (n + 7) / 8;
    if (spectrumDirty_) {
        cl.bindPipeline(*spectrum_);
        for (int c = 0; c < count; ++c) {
            bindImages(cl, tiles_[static_cast<std::size_t>(c)]);
            push(cl, frameBlock, c, t, dt, 0.0f);
            cl.dispatch(groups, groups, 1);
        }
        cl.computeBarrier();
        spectrumDirty_ = false;
    }
    cl.bindPipeline(*evolve_);
    for (int c = 0; c < count; ++c) {
        bindImages(cl, tiles_[static_cast<std::size_t>(c)]);
        push(cl, frameBlock, c, t, dt, 0.0f);
        cl.dispatch(groups, groups, 1);
    }
    cl.computeBarrier();
    cl.bindPipeline(*fft);
    for (int pass = 0; pass < 2; ++pass) {
        for (int c = 0; c < count; ++c) {
            bindImages(cl, tiles_[static_cast<std::size_t>(c)]);
            push(cl, frameBlock, c, t, dt, static_cast<float>(pass));
            cl.dispatch(n, 1, 1);
        }
        cl.computeBarrier();
    }
    cl.bindPipeline(*resolve_);
    for (int c = 0; c < count; ++c) {
        bindImages(cl, tiles_[static_cast<std::size_t>(c)]);
        push(cl, frameBlock, c, t, dt, 0.0f);
        cl.dispatch(groups, groups, 1);
    }
    // the mip chains (blits): mip 0 -> source; the other levels written one after the other (one barrier per step for all images)
    std::vector<rhi::Texture*> chains;
    for (int c = 0; c < count; ++c) {
        chains.push_back(tiles_[static_cast<std::size_t>(c)].disp.get());
        chains.push_back(tiles_[static_cast<std::size_t>(c)].deriv.get());
    }
    batch.clear();
    for (rhi::Texture* img : chains) {
        batch.push_back({img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1});
        if (mips > 1) batch.push_back({img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, mips - 1});
    }
    cl.transitions(batch);
    for (std::uint32_t m = 1; m < mips; ++m) {
        batch.clear();
        for (rhi::Texture* img : chains) {
            rhi::TextureBlit region;
            region.src = {m - 1, 0};
            region.dst = {m, 0};
            cl.blitTexture(*img, *img, std::span<const rhi::TextureBlit>(&region, 1), VK_FILTER_LINEAR);
            batch.push_back({img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m, 1});
        }
        cl.transitions(batch);
    }
    batch.clear();
    for (rhi::Texture* img : chains) batch.push_back({img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    cl.transitions(batch);
    ready_ = true;
    if (frames_++ == 0) {
        spdlog::info("enh: ocean: first simulated frame: Hs {:.2f} m, wind sea peak {:.0f} m, U10 {:.1f} m/s, fetch {:.0f} km, swell {:.2f} m "
                     "{:.1f} s, choppiness {:.2f}",
                     hs_, peakLength_, sea_.windSpeed, sea_.fetch / 1000.0f, sea_.swellHeight, sea_.swellPeriod, sea_.choppiness);
    }
}

}  // namespace rvk::enh
