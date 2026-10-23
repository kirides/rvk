#include "enh/upscale.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include <spdlog/spdlog.h>

#include "core/paths.h"
#include "enh/gbuffer.h"
#include "shader/embedded.h"
#include "vk/context.h"
#include "vk/frames.h"

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

// mirror of shaders/upscale/prep.comp UpscalePush
struct PrepPush {
    shader::abi::PassPushHeader h;
    std::uint32_t depthTex, width, height, pad;
};
static_assert(sizeof(PrepPush) == 32 && sizeof(PrepPush) <= shader::abi::kPushConstantBudget);

constexpr VkFormat kDepthFormat = VK_FORMAT_R32_SFLOAT;
constexpr VkFormat kOutputFormat = VK_FORMAT_R16G16B16A16_SFLOAT;  // the scene HDR target's format (A16B16G16R16F)
constexpr VkImageUsageFlags kDepthUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageUsageFlags kOutputUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

}  // namespace

int upscaleJitterPhases(std::uint32_t renderWidth, std::uint32_t displayWidth) {
    const float ratio = static_cast<float>(displayWidth) / static_cast<float>(renderWidth > 0 ? renderWidth : 1);
    return static_cast<int>(std::ceil(8.0f * ratio * ratio));
}

std::array<float, 2> upscaleJitter(std::uint64_t frameIndex, int phases) {
    const int i = static_cast<int>(frameIndex % static_cast<std::uint64_t>(phases > 0 ? phases : 1)) + 1;
    return {halton(i, 2) - 0.5f, halton(i, 3) - 0.5f};
}

float upscaleLodBias(std::uint32_t renderWidth, std::uint32_t displayWidth) {
    if (renderWidth == 0 || displayWidth == 0) return 0.0f;
    return std::log2(static_cast<float>(renderWidth) / static_cast<float>(displayWidth)) - 1.0f;
}

Upscale::Upscale(d3d9::Device& device) : dev_(device) {}

Upscale::~Upscale() {
    dev_.setGeometryLodBias(0.0f);
    dev_.removePasses(this);
    dev_.setJitterSource(nullptr);
    dropFsr();
    vk::Context* ctx = &dev_.ctx();
    const VkPipeline prep = prep_;
    vk::PassLayout layout = layout_;
    dev_.frames().retire([ctx, prep, layout]() mutable {
        if (prep != VK_NULL_HANDLE) vkDestroyPipeline(ctx->device(), prep, nullptr);
        if (layout.valid()) layout.destroy(*ctx);
    });
    for (const char* k : {"upscale.depth", "upscale.out"}) dev_.transients().release(k);
}

void Upscale::apply(UpscaleMode mode, unsigned flip) {
    if (mode == mode_ && flip == flip_) return;
    flip_ = flip;
    const bool wasAa = mode_ == UpscaleMode::NativeAa;
    mode_ = mode;
    if (wasAa) {
        dev_.setGeometryLodBias(0.0f);
        dev_.removePasses(this);
        dropFsr();
        for (const char* k : {"upscale.depth", "upscale.out"}) dev_.transients().release(k);
    }
    if (mode == UpscaleMode::Off) {
        dev_.setJitterSource(nullptr);
        spdlog::info("enh: upscale: off");
        return;
    }
    // render size = display size: 8 phases
    const int phases = upscaleJitterPhases(1, 1);
    const float sx = (flip & 1u) != 0 ? -1.0f : 1.0f, sy = (flip & 2u) != 0 ? -1.0f : 1.0f;
    dev_.setJitterSource([phases, sx, sy](std::uint64_t frame) {
        const std::array<float, 2> j = upscaleJitter(frame, phases);
        return std::array<float, 2>{j[0] * sx, j[1] * sy};
    });
    if (mode == UpscaleMode::JitterOnly) {
        spdlog::info("enh: upscale: jitter only, jitter {} phases", phases);
        return;
    }
    if (failed_) {
        spdlog::warn("enh: upscale: native AA is not available this session (see earlier messages): jitter only");
        return;
    }
    if (!layout_.valid()) {
        const VkDescriptorSetLayoutBinding bind{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        if (!layout_.create(dev_.ctx(), dev_.bindless(), {&bind, 1})) {
            failed_ = true;
            return;
        }
        prep_ = vk::createComputePipeline(dev_.ctx(), layout_.layout(), shader::embedded::spirv("upscale/prep.comp"), "upscale.prep");
        if (prep_ == VK_NULL_HANDLE) {
            failed_ = true;
            return;
        }
    }
    needReset_ = true;
    // FSR's texture LOD bias for the geometry it accumulates (set from the actual sizes every frame in record())
    dev_.setGeometryLodBias(upscaleLodBias(1, 1));
    dev_.addPass(d3d9::inject::Point::AfterMainView, "upscale.fsr", [this](d3d9::PassContext& ctx) { record(ctx); }, this, 100);
    spdlog::info("enh: upscale: native AA (FSR 3.1), jitter {} phases", phases);
}

void Upscale::dropFsr() {
    if (!fsr_) return;
    // the context's internal images may still be in use by frames in flight
    dev_.frames().retire([f = std::move(fsr_)]() mutable { f->destroy(); });
    fsr_.reset();
    fsrWidth_ = fsrHeight_ = 0;
}

bool Upscale::ensureFsr(std::uint32_t width, std::uint32_t height) {
    if (fsr_ && fsrWidth_ == width && fsrHeight_ == height) return true;
    dropFsr();
    auto f = std::make_shared<vk::Fsr>();
    const std::filesystem::path dll = paths::dataDir() / "amd_fidelityfx_vk.dll";
    vk::FsrCreateInfo ci;
    ci.maxRenderSize = ci.maxUpscaleSize = {width, height};
    ci.hdr = true;  // the scene HDR target: linear, before the post chain's tonemapping
    ci.autoExposure = true;
    if (!f->load(dll) || !f->createContext(dev_.ctx(), ci)) {
        spdlog::error("enh: upscale: FSR is not available ({}): native AA stays off this session", dll.string());
        failed_ = true;
        return false;
    }
    fsr_ = std::move(f);
    fsrWidth_ = width;
    fsrHeight_ = height;
    needReset_ = true;
    spdlog::info("enh: upscale: FSR provider '{}' at {}x{}", fsr_->providerName(), width, height);
    return true;
}

void Upscale::record(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || mode_ != UpscaleMode::NativeAa || failed_) return;
    d3d9::Surface* ds = dev_.depthStencil();
    d3d9::Texture* hdr = ctx.sceneHdr;
    VkCommandBuffer cmd = ctx.cmd;
    if (hdr == nullptr || ds == nullptr || ds->texture() == nullptr || !dev_.gbufferWritten()) {
        ++skipped_;
        needReset_ = true;
        return;
    }
    const std::uint32_t w = hdr->desc().width, h = hdr->desc().height;
    if (ds->width() < w || ds->height() < h || !ensureFsr(w, h)) {
        ++skipped_;
        needReset_ = true;
        return;
    }
    // render size = the scene target, display size = the output (native AA: equal; a render scale makes the bias follow)
    dev_.setGeometryLodBias(upscaleLodBias(w, w));
    vk::Transients::Entry* motion = dev_.transients().get("gbuffer.motion", {kGbufferMotionFormat, {w, h}, kGbufferMotionUsage});
    vk::Transients::Entry* depth = dev_.transients().get("upscale.depth", {kDepthFormat, {w, h}, kDepthUsage});
    vk::Transients::Entry* out = dev_.transients().get("upscale.out", {kOutputFormat, {w, h}, kOutputUsage});
    if (motion == nullptr || depth == nullptr || out == nullptr || !dev_.readable(ds->texture(), cmd) || !dev_.readable(hdr, cmd)) {
        ++skipped_;
        needReset_ = true;
        return;
    }

    // hardware depth -> R32F
    PrepPush push{};
    push.h.frame = ctx.frameBlock;
    push.depthTex = ds->texture()->bindlessSlot();
    push.width = w;
    push.height = h;
    depth->image.transition(cmd, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prep_);
    layout_.bindSet0(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev_.bindless());
    layout_.pushStorageImage(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, 0, depth->image.view());
    layout_.push(cmd, &push, sizeof(push));
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);

    const d3d9::scene::CameraBlock& cam = dev_.camera();
    const d3d9::scene::CameraBlock& last = dev_.lastCamera();
    vk::FsrDispatch d;
    d.cmd = cmd;
    d.color = &hdr->image();
    d.depth = &depth->image;
    d.motion = &motion->image;
    d.output = &out->image;
    d.renderSize = d.upscaleSize = {w, h};
    const std::array<float, 2> j = dev_.jitter();
    d.jitterX = (flip_ & 1u) != 0 ? -j[0] : j[0];  // what FSR is told: the unflipped sequence
    d.jitterY = (flip_ & 2u) != 0 ? -j[1] : j[1];
    d.sharpness = sharpness_;
    d.motionScaleX = static_cast<float>(w);  // the G-buffer's motion is in uv (previous - current)
    d.motionScaleY = static_cast<float>(h);
    d.reset = needReset_ || !cam.prevValid;
    d.frameTimeMs = cam.valid && last.valid ? static_cast<float>(std::clamp((cam.time - last.time) * 1000.0, 1.0, 100.0)) : 16.6f;
    d.cameraNear = cam.nearZ > 0.0f ? cam.nearZ : 10.0f;
    d.cameraFar = cam.farZ > d.cameraNear ? cam.farZ : 100000.0f;
    d.cameraFovY = cam.valid && cam.projection.at(1, 1) > 0.0f ? 2.0f * std::atan(1.0f / cam.projection.at(1, 1)) : 1.0f;
    d.viewSpaceToMeters = 0.01f;  // the engine's units are centimetres
    if (!fsr_->dispatch(d)) {
        ++skipped_;
        needReset_ = true;
        failed_ = true;  // a dispatch the DLL rejects will be rejected again
        spdlog::error("enh: upscale: the FSR dispatch failed: native AA stays off this session");
        return;
    }
    needReset_ = false;

    // the result replaces the scene HDR target (the post chain reads it next)
    out->image.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    hdr->image().transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageCopy region{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImage(cmd, out->image.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hdr->image().image(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    hdr->image().transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ++frames_;
    const double now = dev_.secondsSinceCreate();
    if (now - lastLog_ > 5.0) {
        lastLog_ = now;
        spdlog::info("enh: upscale: {} frames upscaled ({} skipped), provider '{}', jitter ({:.3f}, {:.3f}) px, reset {}", frames_,
                     skipped_, fsr_->providerName(), d.jitterX, d.jitterY, d.reset ? "yes" : "no");
    }
}

}  // namespace rvk::enh
