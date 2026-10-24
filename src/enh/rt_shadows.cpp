#include "enh/rt_shadows.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <spdlog/spdlog.h>

#include "enh/rt_scene.h"
#include "engine/shadow_maps.h"
#include "shader/embedded.h"
#include "vk/context.h"

namespace rvk::enh {

namespace {

constexpr const char* kShaders[3] = {"shadows/rt_shadow_trace.comp", "shadows/rt_shadow_temporal.comp", "shadows/rt_shadow_spatial.comp"};
constexpr const char* kNames[3] = {"shadows.rt trace", "shadows.rt temporal", "shadows.rt spatial"};
constexpr float kRayReach = 12000.0f;  // 120 m: the longest sun ray (the engine's own cascades reach 100 m)
constexpr float kPi = 3.14159265f;

}  // namespace

float RtShadows::modeRangeMetres(int mode) { return mode >= 2 ? 35.0f : mode == 1 ? 15.0f : 0.0f; }

RtShadows::RtShadows(d3d9::Device& device, RtScene* scene) : dev_(device), scene_(scene) {}

// first use only (off = nothing exists: no layout, no pipelines, no threads): the compute pipelines build in the background,
// the passes wait for them (the cascades give the mask meanwhile)
void RtShadows::startBuild() {
    if (built_ || failed_ || !available()) return;
    built_ = true;
    const VkDescriptorSetLayoutBinding out{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    if (!layout_.create(dev_.ctx(), dev_.bindless(), {&out, 1})) {
        failed_ = true;
        return;
    }
    vk::Context* ctx = &dev_.ctx();
    const VkPipelineLayout layout = layout_.layout();
    for (int i = 0; i < 3; ++i) {
        building_[i] = std::async(std::launch::async, [ctx, layout, i]() {
            return vk::createComputePipeline(*ctx, layout, shader::embedded::spirv(kShaders[i]), kNames[i]);
        });
    }
}

RtShadows::~RtShadows() {
    apply(Settings{});
    vk::Context* ctx = &dev_.ctx();
    for (auto& b : building_) {
        if (!b.valid()) continue;
        const VkPipeline p = b.get();
        if (p != VK_NULL_HANDLE) vkDestroyPipeline(ctx->device(), p, nullptr);
    }
    VkPipeline pipes[3] = {pipeline_[0], pipeline_[1], pipeline_[2]};
    vk::PassLayout layout = layout_;
    if (layout.valid() || pipes[0] != VK_NULL_HANDLE) {
        dev_.frames().retire([ctx, pipes, layout]() mutable {
            for (VkPipeline p : pipes) {
                if (p != VK_NULL_HANDLE) vkDestroyPipeline(ctx->device(), p, nullptr);
            }
            layout.destroy(*ctx);
        });
    }
}

bool RtShadows::available() const { return scene_ != nullptr && scene_->supported() && !failed_; }

void RtShadows::apply(const Settings& s) {
    settings_ = s;
    if (!available()) settings_.mode = 0;
    const bool want = settings_.mode > 0;
    if (want) startBuild();
    if (want && !held_) {
        scene_->acquire(this);
        scene_->setDynamic(this, true);  // characters cast RT shadows
        held_ = true;
        // three injected passes (own GPU times in -PerfFrame): the later two run only for a frame the first one prepared
        passId_ = dev_.addPass(d3d9::inject::Point::AfterDepthPrepass, "shadows.rt trace", [this](d3d9::PassContext& ctx) { record(ctx, 0); }, this, 12);
        dev_.addPass(d3d9::inject::Point::AfterDepthPrepass, "shadows.rt temporal", [this](d3d9::PassContext& ctx) { record(ctx, 1); }, this, 13);
        dev_.addPass(d3d9::inject::Point::AfterDepthPrepass, "shadows.rt spatial", [this](d3d9::PassContext& ctx) { record(ctx, 2); }, this, 14);
        historyValid_ = false;
    } else if (!want && held_) {
        dev_.removePasses(this);
        passId_ = 0;
        pending_ = {};
        scene_->setAnimated(this, false, 0.0f);
        scene_->setDynamic(this, false);
        scene_->release(this);
        held_ = false;
        for (const char* k : {"shadows.rt raw", "shadows.rt history0", "shadows.rt history1", "shadows.rt final"}) dev_.transients().release(k);
        frame_ = ~0ull;
        info_ = Frame{};
        historyValid_ = false;
    }
    if (held_) {
        // the trees in the wind (RtScene::observeTree) within the range + the cascade blend and a tree's reach
        const float range = (settings_.rangeMetres > 0.0f ? settings_.rangeMetres : modeRangeMetres(settings_.mode)) * 100.0f + 1500.0f;
        scene_->setAnimated(this, settings_.wind, range);
    }
    if (want) {
        spdlog::info("enh: shadows.rt: mode {} (range {:.0f} m), softness {:.2f} deg, quality {}, filter {:.2f}, bias {:.1f}, debug {}", settings_.mode,
                     settings_.rangeMetres > 0.0f ? settings_.rangeMetres : modeRangeMetres(settings_.mode), settings_.softness, settings_.quality,
                     settings_.filter, settings_.bias, settings_.debug);
    }
}

bool RtShadows::pipelinesReady() {
    if (failed_) return false;
    bool all = true;
    for (int i = 0; i < 3; ++i) {
        if (pipeline_[i] != VK_NULL_HANDLE) continue;
        if (!building_[i].valid() || building_[i].wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            all = false;
            continue;
        }
        pipeline_[i] = building_[i].get();
        if (pipeline_[i] == VK_NULL_HANDLE) {
            spdlog::error("enh: shadows.rt: the compute pipeline {} failed: ray-traced shadows stay off this session", kNames[i]);
            failed_ = true;
            return false;
        }
    }
    return all;
}

void RtShadows::record(d3d9::PassContext& ctx, int stage) {
    if (ctx.mainView != 1 || settings_.mode == 0) return;
    const std::uint64_t frame = dev_.frameValue();
    if (stage > 0) {
        // the temporal and spatial passes of a frame the trace pass prepared
        if (pending_.frame != frame || pending_.next != stage) return;
        VkCommandBuffer c = ctx.cmd;
        d3d9::RegionScope r(dev_, kNames[stage]);
        vk::Transients::Entry* out = stage == 1 ? pending_.hist : pending_.fin;
        out->image.transition(c, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_[stage]);
        layout_.bindSet0(c, VK_PIPELINE_BIND_POINT_COMPUTE, dev_.bindless());
        layout_.pushStorageImage(c, VK_PIPELINE_BIND_POINT_COMPUTE, 0, out->image.view());
        layout_.push(c, &pending_.push, sizeof(pending_.push));
        vkCmdDispatch(c, pending_.gx, pending_.gy, 1);
        out->image.transition(c, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        pending_.next = stage + 1;
        if (stage == 2) {
            history_ = pending_.cur;
            historyValid_ = true;
            frame_ = frame;
            info_ = {pending_.fin->slot, pending_.w, pending_.h, pending_.range};
        }
        return;
    }
    pending_ = {};
    frame_ = ~0ull;
    info_ = Frame{};
    float sunStored[3];
    // no cascades this frame (interiors without sun shadows, loading): nothing to replace
    if (dev_.shadowDepthMaps() == 0 || !dev_.rtSceneValid() || ctx.depthMap == nullptr || !engine::shadowmaps::sunVector(sunStored) ||
        !pipelinesReady()) {
        ++skipped_;
        historyValid_ = false;
        return;
    }
    VkCommandBuffer cmd = ctx.cmd;
    if (!dev_.readable(ctx.depthMap, cmd)) {
        ++skipped_;
        historyValid_ = false;
        return;
    }
    const std::uint32_t w = ctx.depthMap->desc().width, h = ctx.depthMap->desc().height;
    if (w != lastWidth_ || h != lastHeight_) historyValid_ = false;  // new images: no history
    vk::Transients::Entry* raw = dev_.transients().get("shadows.rt raw", {VK_FORMAT_R8G8B8A8_UNORM, {w, h}});
    vk::Transients::Entry* fin = dev_.transients().get("shadows.rt final", {VK_FORMAT_R8G8B8A8_UNORM, {w, h}});
    vk::Transients::Entry* hist[2] = {dev_.transients().get("shadows.rt history0", {VK_FORMAT_R16G16B16A16_SFLOAT, {w, h}}),
                                      dev_.transients().get("shadows.rt history1", {VK_FORMAT_R16G16B16A16_SFLOAT, {w, h}})};
    const vk::RingAlloc pa = dev_.frames().allocate(sizeof(RtShadowParams), 16);
    if (raw == nullptr || fin == nullptr || hist[0] == nullptr || hist[1] == nullptr || pa.ptr == nullptr) {
        ++skipped_;
        return;
    }
    // the stored vector is the direction the light travels (shadows.md 3): the rays go the other way. The cascade object's copy
    // moves in steps of ~0.5 degree (the engine re-aims it only after such a turn: shadows jumped in chunks), the light's own
    // direction is exact every frame
    float trueSun[3];
    if (engine::shadowmaps::trueSunVector(trueSun)) std::memcpy(sunStored, trueSun, sizeof(sunStored));
    float toSun[3] = {-sunStored[0], -sunStored[1], -sunStored[2]};
    const float len = std::sqrt(toSun[0] * toSun[0] + toSun[1] * toSun[1] + toSun[2] * toSun[2]);
    if (!(len > 1e-4f)) {
        ++skipped_;
        return;
    }
    for (float& v : toSun) v /= len;
    std::memcpy(lastSun_, toSun, sizeof(lastSun_));

    const float rangeM = settings_.rangeMetres > 0.0f ? settings_.rangeMetres : modeRangeMetres(settings_.mode);
    const std::uint32_t cur = history_ ^ 1u;
    RtShadowParams p{};
    p.tex[0] = ctx.depthMap->bindlessSlot();
    p.tex[1] = raw->slot;
    p.tex[2] = hist[history_]->slot;
    p.tex[3] = hist[cur]->slot;
    p.info[0] = w;
    p.info[1] = h;
    p.info[2] = settings_.quality >= 3 ? 2u : 1u;
    p.info[3] = settings_.quality <= 1 ? 1u : 0u;  // RVK_RT_TRACE_OPAQUE: cut-outs solid at low
    p.mode[0] = static_cast<std::uint32_t>(frame);
    p.mode[1] = historyValid_ ? 1u : 0u;
    p.mode[2] = static_cast<std::uint32_t>(settings_.debug);
    std::memcpy(p.sun, toSun, 12);
    p.sun[3] = std::tan(std::clamp(settings_.softness, 0.05f, 10.0f) * kPi / 180.0f);
    p.range[0] = rangeM * 100.0f;
    p.range[1] = kRayReach;
    p.range[2] = settings_.bias;
    p.range[3] = settings_.filter;
    std::memcpy(pa.ptr, &p, sizeof(p));
    RtShadowPush push{};
    push.h.frame = ctx.frameBlock;
    push.h.passId = 0;
    push.params[0] = static_cast<std::uint32_t>(pa.address);
    push.params[1] = static_cast<std::uint32_t>(pa.address >> 32);
    const std::uint32_t gx = (w + 7) / 8, gy = (h + 7) / 8;
    {
        d3d9::RegionScope r(dev_, kNames[0]);
        // the previous frame's history is sampled (it is read-only since its own frame)
        hist[history_]->image.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        raw->image.transition(cmd, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_[0]);
        layout_.bindSet0(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev_.bindless());
        layout_.pushStorageImage(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, 0, raw->image.view());
        layout_.push(cmd, &push, sizeof(push));
        vkCmdDispatch(cmd, gx, gy, 1);
        raw->image.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    pending_ = {frame, 1, push, gx, gy, hist[cur], fin, cur, w, h, p.range[0]};
    lastWidth_ = w;
    lastHeight_ = h;
    if (passes_++ == 0) {
        spdlog::info("enh: shadows.rt: first frame {}x{}: range {:.0f} m, {} ray(s) per pixel, cone half angle {:.2f} deg, towards the sun ({:.3f}, {:.3f}, {:.3f}) "
                     "(the cascade object stores ({:.3f}, {:.3f}, {:.3f}))",
                     w, h, rangeM, p.info[2], settings_.softness, toSun[0], toSun[1], toSun[2], sunStored[0], sunStored[1], sunStored[2]);
    }
    logStats();
}

void RtShadows::logStats() {
    const double now = dev_.secondsSinceCreate();
    if (now - lastLog_ < 5.0) return;
    lastLog_ = now;
    spdlog::info("enh: shadows.rt: {} frames ({} skipped: no cascades / RT scene / pipelines), {}x{}, sun ({:.3f}, {:.3f}, {:.3f})", passes_, skipped_,
                 lastWidth_, lastHeight_, lastSun_[0], lastSun_[1], lastSun_[2]);
}

}  // namespace rvk::enh
