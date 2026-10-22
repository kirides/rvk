#include "enh/ocean.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"
#include "shader/embedded.h"
#include "vk/bindless.h"
#include "vk/context.h"
#include "vk/frames.h"

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
constexpr VkFormat kFoamFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

int log2i(int n) {
    int b = 0;
    while ((1 << b) < n) ++b;
    return b;
}

int mipCount(int n) { return log2i(n) + 1; }

struct Barriers {
    std::vector<VkImageMemoryBarrier2> images;
    std::vector<VkMemoryBarrier2> memory;
    void image(VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
               VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, std::uint32_t baseMip = 0,
               std::uint32_t mips = VK_REMAINING_MIP_LEVELS) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mips, 0, 1};
        images.push_back(b);
    }
    void computeToCompute() {
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        memory.push_back(m);
    }
    void flush(VkCommandBuffer cmd) {
        if (images.empty() && memory.empty()) return;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = static_cast<std::uint32_t>(images.size());
        di.pImageMemoryBarriers = images.data();
        di.memoryBarrierCount = static_cast<std::uint32_t>(memory.size());
        di.pMemoryBarriers = memory.data();
        vkCmdPipelineBarrier2(cmd, &di);
        images.clear();
        memory.clear();
    }
};

constexpr VkPipelineStageFlags2 kSampleStages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkAccessFlags2 kStorageRW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

}  // namespace

Ocean::Ocean(d3d9::Device& device) : dev_(device) {
    if (!createPipelines()) {
        failed_ = true;
        spdlog::error("enh: ocean: compute pipelines failed, the FFT ocean stays off this session");
    }
}

Ocean::~Ocean() {
    destroyTiles();
    destroyFoam();
    vk::Context* ctx = &dev_.ctx();
    std::vector<VkPipeline> pipes = {spectrum_, evolve_, resolve_, fft_[0], fft_[1], fft_[2], foam_};
    vk::PassLayout layout = layout_;
    vk::PassLayout foamLayout = foamLayout_;
    const VkSampler smp = sampler_;
    dev_.frames().retire([ctx, pipes, layout, foamLayout, smp]() mutable {
        for (VkPipeline p : pipes) {
            if (p != VK_NULL_HANDLE) vkDestroyPipeline(ctx->device(), p, nullptr);
        }
        if (layout.valid()) layout.destroy(*ctx);
        if (foamLayout.valid()) foamLayout.destroy(*ctx);
        // the sampler's bindless slot is never re-used (one per session)
        if (smp != VK_NULL_HANDLE) vkDestroySampler(ctx->device(), smp, nullptr);
    });
}

bool Ocean::createPipelines() {
    vk::Context& ctx = dev_.ctx();
    VkDescriptorSetLayoutBinding b[5]{};
    for (std::uint32_t i = 0; i < 5; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    if (!layout_.create(ctx, dev_.bindless(), b)) return false;
    spectrum_ = vk::createComputePipeline(ctx, layout_.layout(), shader::embedded::spirv("water/ocean_spectrum.comp"), "ocean.spectrum");
    evolve_ = vk::createComputePipeline(ctx, layout_.layout(), shader::embedded::spirv("water/ocean_evolve.comp"), "ocean.evolve");
    resolve_ = vk::createComputePipeline(ctx, layout_.layout(), shader::embedded::spirv("water/ocean_resolve.comp"), "ocean.resolve");
    const VkPhysicalDeviceLimits& lim = ctx.properties().limits;
    for (int i = 0; i < 3; ++i) {
        const std::int32_t n = 128 << i;
        // one workgroup of N / 2 threads per line, 2 x N x 16 bytes of shared memory
        if (static_cast<std::uint32_t>(n / 2) > lim.maxComputeWorkGroupInvocations ||
            static_cast<std::uint32_t>(n / 2) > lim.maxComputeWorkGroupSize[0] ||
            static_cast<std::uint32_t>(n) * 32u > lim.maxComputeSharedMemorySize) {
            spdlog::info("enh: ocean: FFT size {} not supported by this GPU (limits)", n);
            continue;
        }
        const std::int32_t data[3] = {n, log2i(n), n / 2};
        const VkSpecializationMapEntry e[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
        VkSpecializationInfo si{3, e, sizeof(data), data};
        fft_[static_cast<std::size_t>(i)] =
            vk::createComputePipeline(ctx, layout_.layout(), shader::embedded::spirv("water/ocean_fft.comp"), "ocean.fft", &si);
    }
    // trilinear (+ anisotropic), wrapping: the tiles repeat
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    // anisotropic where the device has it: the distant sea at grazing angles keeps its waves instead of blurring flat
    if (ctx.features().samplerAnisotropy) {
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = std::min(8.0f, ctx.features().maxAnisotropy);
    }
    if (vkCreateSampler(ctx.device(), &sci, nullptr, &sampler_) == VK_SUCCESS) samplerSlot_ = dev_.bindless().addSampler(sampler_);
    // the foam pass (one storage image; its history and the floor field through the bindless set). Optional: without it the
    // shading evaluates the surf's foam in place
    VkDescriptorSetLayoutBinding fb{};
    fb.binding = 0;
    fb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    fb.descriptorCount = 1;
    fb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    if (foamLayout_.create(ctx, dev_.bindless(), std::span<const VkDescriptorSetLayoutBinding>(&fb, 1))) {
        foam_ = vk::createComputePipeline(ctx, foamLayout_.layout(), shader::embedded::spirv("water/ocean_foam.comp"), "ocean.foam");
    }
    return spectrum_ != VK_NULL_HANDLE && evolve_ != VK_NULL_HANDLE && resolve_ != VK_NULL_HANDLE && fft_[0] != VK_NULL_HANDLE &&
           samplerSlot_ != 0;
}

void Ocean::destroyTiles() {
    vk::Context* ctx = &dev_.ctx();
    for (Tile& t : tiles_) {
        if (t.dispSlot != 0) dev_.bindless().releaseTexture(vk::TextureKind::Tex2D, t.dispSlot);
        if (t.derivSlot != 0) dev_.bindless().releaseTexture(vk::TextureKind::Tex2D, t.derivSlot);
        std::array<vk::Image, 5> imgs = {t.h0, t.workA, t.workB, t.disp, t.deriv};
        dev_.frames().retire([ctx, imgs]() mutable {
            for (vk::Image& i : imgs) {
                if (i.valid()) i.destroy(*ctx);
            }
        });
        t = Tile{};
    }
    ready_ = false;
}

bool Ocean::createTiles() {
    vk::Context& ctx = dev_.ctx();
    const auto n = static_cast<std::uint32_t>(cascades_.size);
    for (int c = 0; c < cascades_.count; ++c) {
        Tile& t = tiles_[static_cast<std::size_t>(c)];
        vk::ImageDesc d;
        d.extent = {n, n, 1};
        d.format = kWorkFormat;
        d.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        d.name = "ocean.h0";
        bool ok = t.h0.create(ctx, d);
        d.name = "ocean.workA";
        ok = ok && t.workA.create(ctx, d);
        d.name = "ocean.workB";
        ok = ok && t.workB.create(ctx, d);
        d.format = kOutFormat;
        d.mips = static_cast<std::uint32_t>(mipCount(cascades_.size));
        d.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        d.name = "ocean.disp";
        ok = ok && t.disp.create(ctx, d);
        d.name = "ocean.deriv";
        ok = ok && t.deriv.create(ctx, d);
        if (!ok) {
            spdlog::error("enh: ocean: tile images {}x{} failed", n, n);
            destroyTiles();
            return false;
        }
        t.dispSlot = dev_.bindless().addTexture(vk::TextureKind::Tex2D, t.disp.view());
        t.derivSlot = dev_.bindless().addTexture(vk::TextureKind::Tex2D, t.deriv.view());
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
    vk::Context& ctx = dev_.ctx();
    for (int i = 0; i < 2; ++i) {
        vk::ImageDesc d;
        d.extent = {static_cast<std::uint32_t>(kFoamSize), static_cast<std::uint32_t>(kFoamSize), 1};
        d.format = kFoamFormat;
        d.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        d.name = "ocean.foam";
        if (!foamImg_[static_cast<std::size_t>(i)].create(ctx, d)) {
            spdlog::error("enh: ocean: foam buffer {}x{} failed, the surf's foam is evaluated in place", kFoamSize, kFoamSize);
            destroyFoam();
            return false;
        }
        foamSlot_[static_cast<std::size_t>(i)] = dev_.bindless().addTexture(vk::TextureKind::Tex2D, foamImg_[static_cast<std::size_t>(i)].view());
    }
    foamCreated_ = true;
    foamValid_ = false;
    foamSeconds_ = -1.0;
    spdlog::info("enh: ocean: foam buffer {}x{} cells of {:.0f} units ({:.0f} MiB)", kFoamSize, kFoamSize, kFoamCell,
                 2.0 * kFoamSize * kFoamSize * 8.0 / (1024.0 * 1024.0));
    return true;
}

void Ocean::destroyFoam() {
    vk::Context* ctx = &dev_.ctx();
    for (auto& slot : foamSlot_) {
        if (slot != 0) dev_.bindless().releaseTexture(vk::TextureKind::Tex2D, slot);
        slot = 0;
    }
    std::array<vk::Image, 2> imgs = foamImg_;
    foamImg_ = {};
    dev_.frames().retire([ctx, imgs]() mutable {
        for (vk::Image& i : imgs) {
            if (i.valid()) i.destroy(*ctx);
        }
    });
    foamCreated_ = false;
    foamValid_ = false;
}

std::array<float, 4> Ocean::foamMap() const {
    const float extent = kFoamCell * static_cast<float>(kFoamSize);
    return {static_cast<float>(foamFirst_[0]) * kFoamCell, static_cast<float>(foamFirst_[1]) * kFoamCell, extent, 1.0f / extent};
}

void Ocean::recordFoam(VkCommandBuffer cmd, VkDeviceAddress frameBlock, const OceanFoamInputs& in) {
    if (failed_ || foamFailed_ || foam_ == VK_NULL_HANDLE || quality_ < 0 || in.floorA == 0 || in.floorB == 0) {
        foamValid_ = false;
        return;
    }
    if (!foamCreated_ && !createFoam()) {
        foamFailed_ = true;  // not again this session
        return;
    }
    const double dtRaw = foamSeconds_ < 0.0 ? 1e9 : in.seconds - foamSeconds_;
    foamSeconds_ = in.seconds;
    // the history continues while the pass runs every frame; after a pause (the sea out of view) it starts over
    const bool history = foamValid_ && dtRaw >= 0.0 && dtRaw < 0.5;
    const float dt = history ? static_cast<float>(std::clamp(dtRaw, 0.0, 0.1)) : 0.0f;
    const int cur = foamValid_ ? 1 - foamCur_ : 0;
    const int prev = 1 - cur;
    const int prevFirst[2] = {foamFirst_[0], foamFirst_[1]};
    foamFirst_[0] = static_cast<int>(std::floor(in.camX / kFoamCell)) - kFoamSize / 2;
    foamFirst_[1] = static_cast<int>(std::floor(in.camZ / kFoamCell)) - kFoamSize / 2;

    Barriers bar;
    vk::Image& out = foamImg_[static_cast<std::size_t>(cur)];
    vk::Image& hist = foamImg_[static_cast<std::size_t>(prev)];
    // this frame's image: all of it is rewritten (after its last use: the water's sampling two frames ago, the foam pass
    // reading it as the history last frame)
    bar.image(out.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kSampleStages | kCompute, VK_ACCESS_2_NONE, kCompute,
              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    if (!foamValid_) {
        // the first frame: the history image gets a defined layout (never read: prev.w = 0)
        bar.image(hist.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
                  VK_ACCESS_2_NONE, kCompute | kSampleStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        hist.setTrackedLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    bar.flush(cmd);
    OceanFoamPush p{};
    p.h.frame = frameBlock;
    p.window[0] = static_cast<float>(foamFirst_[0]);
    p.window[1] = static_cast<float>(foamFirst_[1]);
    p.window[2] = kFoamCell;
    p.window[3] = static_cast<float>(kFoamSize);
    p.prev[0] = static_cast<float>(prevFirst[0]);
    p.prev[1] = static_cast<float>(prevFirst[1]);
    p.prev[2] = dt;
    p.prev[3] = history ? 1.0f : 0.0f;
    for (int i = 0; i < 4; ++i) {
        p.floorMap[i] = in.floorMap[i];
        p.shoreWave[i] = in.shoreWave[i];
    }
    p.tex[0] = in.floorA;
    p.tex[1] = in.floorB;
    p.tex[2] = foamSlot_[static_cast<std::size_t>(prev)];
    p.tex[3] = in.smpClamp;
    p.info[0] = samplerSlot_;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, foam_);
    foamLayout_.bindSet0(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev_.bindless());
    foamLayout_.pushStorageImage(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, 0, out.view());
    foamLayout_.push(cmd, &p, sizeof(p));
    const std::uint32_t groups = (kFoamSize + 7) / 8;
    vkCmdDispatch(cmd, groups, groups, 1);
    bar.image(out.image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
              kSampleStages | kCompute, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    out.setTrackedLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    bar.flush(cmd);
    foamCur_ = cur;
    foamValid_ = true;
}

void Ocean::setQuality(int quality) {
    if (failed_) return;
    quality = std::clamp(quality, 0, 2);
    // the largest FFT this GPU runs
    while (quality > 0 && fft_[static_cast<std::size_t>(quality)] == VK_NULL_HANDLE) --quality;
    if (quality == quality_) return;
    destroyTiles();
    quality_ = quality;
    cascades_ = ocean::cascadesFor(quality);
    if (!createTiles()) {
        failed_ = true;
        return;
    }
    // the foam buffer with the tiles (not at its first use: no allocation in a later frame)
    if (!foamCreated_ && !foamFailed_ && foam_ != VK_NULL_HANDLE && !createFoam()) foamFailed_ = true;
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

void Ocean::push(VkCommandBuffer cmd, VkDeviceAddress frameBlock, int cascade, float t, float dt, float direction) {
    const ocean::Cascade& c = cascades_.c[static_cast<std::size_t>(cascade)];
    OceanPush p{};
    p.h.frame = frameBlock;
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
    layout_.push(cmd, &p, sizeof(p));
}

void Ocean::bindImages(VkCommandBuffer cmd, Tile& tile) {
    vk::Context& ctx = dev_.ctx();
    const VkImageView views[5] = {tile.h0.view(), tile.workA.view(), tile.workB.view(), tile.disp.attachmentView(ctx, 0, 0),
                                  tile.deriv.attachmentView(ctx, 0, 0)};
    layout_.pushStorageImages(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, views);
}

void Ocean::record(VkCommandBuffer cmd, VkDeviceAddress frameBlock, double seconds) {
    if (failed_ || quality_ < 0 || !tiles_[0].disp.valid()) return;
    const VkPipeline fft = fft_[static_cast<std::size_t>(quality_)];
    if (fft == VK_NULL_HANDLE) return;
    const float dt = lastSeconds_ < 0.0 ? 0.0f : static_cast<float>(std::clamp(seconds - lastSeconds_, 0.0, 0.1));
    lastSeconds_ = seconds;
    const float t = static_cast<float>(std::fmod(seconds, static_cast<double>(ocean::kRepeatTime)));
    const auto n = static_cast<std::uint32_t>(cascades_.size);
    const int count = cascades_.count;
    const std::uint32_t mips = static_cast<std::uint32_t>(mipCount(cascades_.size));
    Barriers bar;

    // into GENERAL: work images every frame (contents rewritten), h0 kept, the outputs from last frame's sampling
    for (int c = 0; c < count; ++c) {
        Tile& tl = tiles_[static_cast<std::size_t>(c)];
        if (!tl.cleared) {
            bar.image(tl.h0.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0, kCompute, kStorageRW);
            bar.image(tl.workA.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0, kCompute, kStorageRW);
            bar.image(tl.workB.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0, kCompute, kStorageRW);
            bar.image(tl.disp.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                      VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            bar.image(tl.deriv.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0, kCompute, kStorageRW);
        } else {
            // last frame's FFT used the work images, its sampling read the outputs
            bar.image(tl.workA.image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, kCompute, kStorageRW, kCompute, kStorageRW);
            bar.image(tl.workB.image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, kCompute, kStorageRW, kCompute, kStorageRW);
            bar.image(tl.h0.image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, kCompute, kStorageRW, kCompute, kStorageRW);
            bar.image(tl.disp.image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, kSampleStages,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kCompute, kStorageRW, 0, 1);
            bar.image(tl.deriv.image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, kSampleStages,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kCompute, kStorageRW, 0, 1);
        }
    }
    bar.flush(cmd);
    for (int c = 0; c < count; ++c) {
        Tile& tl = tiles_[static_cast<std::size_t>(c)];
        if (tl.cleared) continue;
        // the foam history starts at zero
        const VkClearColorValue zero{};
        const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, tl.disp.image(), VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
        bar.image(tl.disp.image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, kCompute, kStorageRW, 0, 1);
        tl.cleared = true;
        spectrumDirty_ = true;
    }
    bar.flush(cmd);
    layout_.bindSet0(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev_.bindless());
    const std::uint32_t groups = (n + 7) / 8;
    if (spectrumDirty_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, spectrum_);
        for (int c = 0; c < count; ++c) {
            bindImages(cmd, tiles_[static_cast<std::size_t>(c)]);
            push(cmd, frameBlock, c, t, dt, 0.0f);
            vkCmdDispatch(cmd, groups, groups, 1);
        }
        bar.computeToCompute();
        bar.flush(cmd);
        spectrumDirty_ = false;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, evolve_);
    for (int c = 0; c < count; ++c) {
        bindImages(cmd, tiles_[static_cast<std::size_t>(c)]);
        push(cmd, frameBlock, c, t, dt, 0.0f);
        vkCmdDispatch(cmd, groups, groups, 1);
    }
    bar.computeToCompute();
    bar.flush(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fft);
    for (int pass = 0; pass < 2; ++pass) {
        for (int c = 0; c < count; ++c) {
            bindImages(cmd, tiles_[static_cast<std::size_t>(c)]);
            push(cmd, frameBlock, c, t, dt, static_cast<float>(pass));
            vkCmdDispatch(cmd, n, 1, 1);
        }
        bar.computeToCompute();
        bar.flush(cmd);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_);
    for (int c = 0; c < count; ++c) {
        bindImages(cmd, tiles_[static_cast<std::size_t>(c)]);
        push(cmd, frameBlock, c, t, dt, 0.0f);
        vkCmdDispatch(cmd, groups, groups, 1);
    }
    // the mip chains (blits): mip 0 -> source; the other levels written one after the other
    for (int c = 0; c < count; ++c) {
        Tile& tl = tiles_[static_cast<std::size_t>(c)];
        for (vk::Image* img : {&tl.disp, &tl.deriv}) {
            bar.image(img->image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 0, 1);
            if (mips > 1) {
                bar.image(img->image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kSampleStages,
                          VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 1, mips - 1);
            }
        }
    }
    bar.flush(cmd);
    for (std::uint32_t m = 1; m < mips; ++m) {
        const auto src = static_cast<std::int32_t>(std::max(1u, n >> (m - 1)));
        const auto dst = static_cast<std::int32_t>(std::max(1u, n >> m));
        for (int c = 0; c < count; ++c) {
            Tile& tl = tiles_[static_cast<std::size_t>(c)];
            for (vk::Image* img : {&tl.disp, &tl.deriv}) {
                VkImageBlit2 region{VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
                region.srcOffsets[1] = {src, src, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
                region.dstOffsets[1] = {dst, dst, 1};
                VkBlitImageInfo2 bi{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
                bi.srcImage = img->image();
                bi.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                bi.dstImage = img->image();
                bi.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                bi.regionCount = 1;
                bi.pRegions = &region;
                bi.filter = VK_FILTER_LINEAR;
                vkCmdBlitImage2(cmd, &bi);
                bar.image(img->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                          VK_ACCESS_2_TRANSFER_READ_BIT, m, 1);
            }
        }
        bar.flush(cmd);
    }
    for (int c = 0; c < count; ++c) {
        Tile& tl = tiles_[static_cast<std::size_t>(c)];
        for (vk::Image* img : {&tl.disp, &tl.deriv}) {
            bar.image(img->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, kSampleStages,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            img->setTrackedLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }
    bar.flush(cmd);
    ready_ = true;
    if (frames_++ == 0) {
        spdlog::info("enh: ocean: first simulated frame: Hs {:.2f} m, wind sea peak {:.0f} m, U10 {:.1f} m/s, fetch {:.0f} km, swell {:.2f} m "
                     "{:.1f} s, choppiness {:.2f}",
                     hs_, peakLength_, sea_.windSpeed, sea_.fetch / 1000.0f, sea_.swellHeight, sea_.swellPeriod, sea_.choppiness);
    }
}

}  // namespace rvk::enh
