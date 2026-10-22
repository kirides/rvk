#include "enh/water_rt.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include <spdlog/spdlog.h>

#include "enh/rt_scene.h"
#include "shader/embedded.h"
#include "vk/context.h"

namespace rvk::enh {

namespace {

// the tiers (docs/research/rt-reflections.md 3.3)
struct Tier {
    std::uint32_t scale;        // screen pixels per reflection pixel (per axis)
    std::uint32_t traceFlags;   // RVK_RT_TRACE_OPAQUE (1): cut-outs solid
    std::uint32_t shadows;      // 0 none, 1 cut-outs solid, 2 alpha-tested
    float reach;                // world units
};
constexpr Tier kTiers[3] = {
    {2, 1, 0, 15000.0f},  // low
    {2, 0, 1, 20000.0f},  // medium
    {1, 0, 2, 30000.0f},  // high
};

}  // namespace

WaterRt::WaterRt(d3d9::Device& device, RtScene* scene) : dev_(device), scene_(scene) {
    if (scene_ == nullptr || !scene_->supported()) return;
    const VkDescriptorSetLayoutBinding out{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    if (!layout_.create(dev_.ctx(), dev_.bindless(), {&out, 1})) {
        failed_ = true;
        return;
    }
    // the compute pipeline in the background from the start: turning the tier on later costs no frame
    vk::Context* ctx = &dev_.ctx();
    const VkPipelineLayout layout = layout_.layout();
    building_ = std::async(std::launch::async, [ctx, layout]() {
        return vk::createComputePipeline(*ctx, layout, shader::embedded::spirv("water/water_rt.comp"), "water.rt");
    });
}

WaterRt::~WaterRt() {
    configure(0, 0, 0);
    if (building_.valid()) {
        const VkPipeline p = building_.get();
        if (p != VK_NULL_HANDLE) vkDestroyPipeline(dev_.ctx().device(), p, nullptr);
    }
    vk::Context* ctx = &dev_.ctx();
    const VkPipeline p = pipeline_;
    vk::PassLayout layout = layout_;
    if (p != VK_NULL_HANDLE || layout.valid()) {
        dev_.frames().retire([ctx, p, layout]() mutable {
            if (p != VK_NULL_HANDLE) vkDestroyPipeline(ctx->device(), p, nullptr);
            layout.destroy(*ctx);
        });
    }
}

bool WaterRt::available() const { return scene_ != nullptr && scene_->supported() && !failed_; }

void WaterRt::configure(int tier, int tweaks, int debug) {
    tier_ = available() ? std::clamp(tier, 0, 3) : 0;
    tweaks_ = tweaks;
    debug_ = debug;
    const bool want = tier_ > 0;
    if (want && !held_) {
        scene_->acquire(this);
        held_ = true;
        passId_ = dev_.addPass(d3d9::inject::Point::AfterRefractionCopy, "water.rt", [this](d3d9::PassContext& ctx) { record(ctx); }, this);
        imageFrame_ = ~0ull;
    } else if (!want && held_) {
        if (passId_ != 0) dev_.removePass(passId_);
        passId_ = 0;
        scene_->release(this);
        held_ = false;
        dev_.transients().release("water.rt");
        imageFrame_ = ~0ull;
        imageSlot_ = 0;
    }
}

void WaterRt::seeBody(std::uint64_t id, const Body& b, std::uint64_t frame) {
    if (tier_ == 0) return;
    Seen& s = bodies_[id];
    s.body = b;
    s.frame = frame;
}

bool WaterRt::pipelineReady() {
    if (pipeline_ != VK_NULL_HANDLE) return true;
    if (failed_ || !building_.valid()) return false;
    if (building_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    pipeline_ = building_.get();
    if (pipeline_ == VK_NULL_HANDLE) {
        spdlog::error("enh: water.rt: the compute pipeline failed: rt.reflections stays off this session");
        failed_ = true;
        return false;
    }
    spdlog::info("enh: water.rt: compute pipeline ready");
    return true;
}

void WaterRt::record(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || tier_ == 0) return;
    const std::uint64_t frame = dev_.frameValue();
    // the RT scene of this submission, the engine's depth map and refraction copy, the compute pipeline
    if (!dev_.rtSceneValid() || ctx.depthMap == nullptr || ctx.refraction == nullptr || ctx.sceneHdr == nullptr || !pipelineReady()) {
        ++skipped_;
        return;
    }
    // the bodies drawn lately (the sea grid and each lake / river / pond), nearest first
    const auto& cam = dev_.camera().valid ? dev_.camera() : dev_.lastCamera();
    std::vector<std::pair<float, const Body*>> list;
    for (auto it = bodies_.begin(); it != bodies_.end();) {
        if (frame > it->second.frame + 120) {
            it = bodies_.erase(it);
            continue;
        }
        const Body& b = it->second.body;
        const float dx = std::max({b.box[0] - cam.position[0], 0.0f, cam.position[0] - b.box[2]});
        const float dz = std::max({b.box[1] - cam.position[2], 0.0f, cam.position[2] - b.box[3]});
        list.emplace_back(dx * dx + dz * dz, &b);
        ++it;
    }
    if (list.empty()) {
        ++skipped_;
        return;
    }
    std::sort(list.begin(), list.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
    VkCommandBuffer cmd = ctx.cmd;
    if (!dev_.readable(ctx.depthMap, cmd) || !dev_.readable(ctx.refraction, cmd)) {
        ++skipped_;
        return;
    }
    const Tier& t = kTiers[tier_ - 1];
    const std::uint32_t sw = ctx.sceneHdr->desc().width, sh = ctx.sceneHdr->desc().height;
    const std::uint32_t w = (sw + t.scale - 1) / t.scale, h = (sh + t.scale - 1) / t.scale;
    vk::Transients::Entry* out = dev_.transients().get("water.rt", {VK_FORMAT_R16G16B16A16_SFLOAT, {w, h}});
    const vk::RingAlloc pa = dev_.frames().allocate(sizeof(WaterRtParams), 16);
    if (out == nullptr || pa.ptr == nullptr) {
        ++skipped_;
        return;
    }
    WaterRtParams p{};
    p.tex[0] = ctx.depthMap->bindlessSlot();
    p.tex[1] = ctx.refraction->bindlessSlot();
    p.tex[2] = dev_.samplerFor(true, true);
    p.tex[3] = dev_.samplerFor(false, true);
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(list.size(), kWaterRtMaxBodies));
    p.info[0] = w;
    p.info[1] = h;
    p.info[2] = t.scale;
    p.info[3] = n;
    p.trace[0] = t.traceFlags | ((tweaks_ & 1) != 0 ? 1u : 0u);
    p.trace[1] = (tweaks_ & 2) != 0 ? 0u : t.shadows;
    p.trace[2] = static_cast<std::uint32_t>(tweaks_);
    p.trace[3] = static_cast<std::uint32_t>(debug_);
    p.reach[0] = t.reach;
    p.reach[1] = 0.0015f;  // a slightly wider cone than a mirror's (the water is never quite still)
    p.reach[2] = 20000.0f;
    std::memcpy(p.fogColor, light_.fogColor, 16);
    std::memcpy(p.fogParams, light_.fogParams, 16);
    std::memcpy(p.indoor, light_.indoor, 16);
    std::memcpy(p.sh, light_.sh, sizeof(p.sh));
    std::memcpy(p.sunColor, light_.sunColor, 16);
    std::memcpy(p.sunDir, light_.sunDir, 16);
    for (std::uint32_t i = 0; i < n; ++i) {
        const Body& b = *list[i].second;
        p.plane[i][0] = b.plane[0];
        p.plane[i][1] = b.plane[1];
        p.plane[i][2] = b.plane[2];
        p.plane[i][3] = b.outdoor;
        std::memcpy(p.box[i], b.box, 16);
    }
    std::memcpy(pa.ptr, &p, sizeof(p));
    WaterRtPush push{};
    push.h.frame = ctx.frameBlock;
    push.h.passId = passId_;
    push.params[0] = static_cast<std::uint32_t>(pa.address);
    push.params[1] = static_cast<std::uint32_t>(pa.address >> 32);

    out->image.transition(cmd, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    layout_.bindSet0(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev_.bindless());
    layout_.pushStorageImage(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, 0, out->image.view());
    layout_.push(cmd, &push, sizeof(push));
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    // -> the water draws' pixel shaders
    out->image.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    imageFrame_ = frame;
    imageSlot_ = out->slot;
    lastBodies_ = n;
    lastWidth_ = w;
    lastHeight_ = h;
    if (passes_++ == 0) {
        spdlog::info("enh: water.rt: first reflection image {}x{} (tier {}, 1 ray per {}x{} pixels), {} water bodies", w, h, tier_, t.scale,
                     t.scale, n);
    }
    logStats();
}

void WaterRt::logStats() {
    const double now = dev_.secondsSinceCreate();
    if (now - lastLog_ < 5.0) return;
    lastLog_ = now;
    spdlog::info("enh: water.rt: tier {}, image {}x{}, {} bodies, {} passes ({} skipped: no RT scene / inputs / pipeline yet)", tier_, lastWidth_,
                 lastHeight_, lastBodies_, passes_, skipped_);
}

}  // namespace rvk::enh
