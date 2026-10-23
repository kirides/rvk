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
constexpr VkImageUsageFlags kOutputUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkFormat kReactiveFormat = VK_FORMAT_R32_SFLOAT;  // what the DLL's auto-reactive shader declares (rw_output_autoreactive)
constexpr VkImageUsageFlags kReactiveUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageUsageFlags kOpaqueUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

}  // namespace

float upscaleRatio(UpscaleMode m) {
    switch (m) {
        case UpscaleMode::Quality: return 1.5f;
        case UpscaleMode::Balanced: return 1.7f;
        case UpscaleMode::Performance: return 2.0f;
        case UpscaleMode::UltraPerformance: return 3.0f;
        default: return 1.0f;
    }
}

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
    dev_.setRenderRatio(1.0f);
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
    for (const char* k : {"upscale.depth", "upscale.out", "upscale.opaque", "upscale.reactive"}) dev_.transients().release(k);
}

void Upscale::apply(UpscaleMode mode, unsigned flip) {
    if (mode == mode_ && flip == flip_) return;
    flip_ = flip;
    const bool wasFsr = usesFsr(mode_);
    mode_ = mode;
    if (wasFsr) {
        dev_.setGeometryLodBias(0.0f);
        dev_.removePasses(this);
        dropFsr();
        for (const char* k : {"upscale.depth", "upscale.out", "upscale.opaque", "upscale.reactive"}) dev_.transients().release(k);
    }
    // the render scale (the scene hook shrinks the camera rect by it from the next frame on; the scene image follows)
    dev_.setRenderRatio(usesFsr(mode) && !failed_ ? upscaleRatio(mode) : 1.0f);
    if (mode == UpscaleMode::Off) {
        dev_.setJitterSource(nullptr);
        spdlog::info("enh: upscale: off");
        return;
    }
    const float ratio = upscaleRatio(mode);
    phases_ = static_cast<int>(std::ceil(8.0f * ratio * ratio));
    const float sx = (flip & 1u) != 0 ? -1.0f : 1.0f, sy = (flip & 2u) != 0 ? -1.0f : 1.0f;
    dev_.setJitterSource([this, sx, sy](std::uint64_t frame) {
        const std::array<float, 2> j = upscaleJitter(frame, phases_.load());
        return std::array<float, 2>{j[0] * sx, j[1] * sy};
    });
    if (mode == UpscaleMode::JitterOnly) {
        spdlog::info("enh: upscale: jitter only, jitter {} phases", phases_.load());
        return;
    }
    if (failed_) {
        spdlog::warn("enh: upscale: FSR is not available this session (see earlier messages): jitter only");
        return;
    }
    if (!layout_.valid()) {
        const VkDescriptorSetLayoutBinding bind{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        if (!layout_.create(dev_.ctx(), dev_.bindless(), {&bind, 1})) {
            failed_ = true;
            dev_.setRenderRatio(1.0f);
            return;
        }
        prep_ = vk::createComputePipeline(dev_.ctx(), layout_.layout(), shader::embedded::spirv("upscale/prep.comp"), "upscale.prep");
        if (prep_ == VK_NULL_HANDLE) {
            failed_ = true;
            dev_.setRenderRatio(1.0f);
            return;
        }
    }
    needReset_ = true;
    // FSR's texture LOD bias for the geometry it accumulates (set from the actual sizes every frame in record())
    dev_.setGeometryLodBias(upscaleLodBias(1, 1));
    dev_.addPass(d3d9::inject::Point::BeforeAlpha, "upscale.opaque", [this](d3d9::PassContext& ctx) { recordOpaque(ctx); }, this);
    dev_.addPass(d3d9::inject::Point::AfterMainView, "upscale.fsr", [this](d3d9::PassContext& ctx) { record(ctx); }, this, 100);
    spdlog::info("enh: upscale: FSR 3.1 at 1/{:.2f} of the output, jitter {} phases", ratio, phases_.load());
}

void Upscale::dropFsr() {
    if (!fsr_) return;
    // the context's internal images may still be in use by frames in flight
    dev_.frames().retire([f = std::move(fsr_)]() mutable { f->destroy(); });
    fsr_.reset();
    fsrRender_ = fsrDisplay_ = {};
}

bool Upscale::ensureFsr(VkExtent2D render, VkExtent2D display) {
    if (fsr_ && fsrRender_.width == render.width && fsrRender_.height == render.height && fsrDisplay_.width == display.width &&
        fsrDisplay_.height == display.height) {
        return true;
    }
    dropFsr();
    auto f = std::make_shared<vk::Fsr>();
    const std::filesystem::path dll = paths::dataDir() / "amd_fidelityfx_vk.dll";
    vk::FsrCreateInfo ci;
    ci.maxRenderSize = render;
    ci.maxUpscaleSize = display;
    ci.hdr = true;  // the scene HDR target: linear, before the post chain's tonemapping
    ci.autoExposure = true;
    if (!f->load(dll) || !f->createContext(dev_.ctx(), ci)) {
        spdlog::error("enh: upscale: FSR is not available ({}): it stays off this session", dll.string());
        failed_ = true;
        dev_.setRenderRatio(1.0f);
        return false;
    }
    fsr_ = std::move(f);
    fsrRender_ = render;
    fsrDisplay_ = display;
    needReset_ = true;
    spdlog::info("enh: upscale: FSR provider '{}': {}x{} -> {}x{}", fsr_->providerName(), render.width, render.height, display.width,
                 display.height);
    return true;
}

// the opaque (and refracted) scene as it is when the first translucent effect is about to be drawn: FSR derives its reactive
// mask from the difference to the finished frame
void Upscale::recordOpaque(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || !usesFsr(mode_) || failed_ || !reactive_ || ctx.sceneHdr == nullptr) return;
    d3d9::Texture* hdr = ctx.sceneHdr;
    const VkExtent2D render{hdr->desc().width, hdr->desc().height};
    vk::Transients::Entry* opaque = dev_.transients().get("upscale.opaque", {kOutputFormat, render, kOpaqueUsage});
    if (opaque == nullptr || hdr->image().format() != kOutputFormat) return;
    VkCommandBuffer cmd = ctx.cmd;
    hdr->image().transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    opaque->image.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageCopy region{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                             {render.width, render.height, 1}};
    vkCmdCopyImage(cmd, hdr->image().image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, opaque->image.image(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    opaque->image.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    opaqueValid_ = true;
}

void Upscale::record(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || !usesFsr(mode_) || failed_) return;
    d3d9::Surface* ds = dev_.depthStencil();
    d3d9::Texture* hdr = ctx.sceneHdr;
    VkCommandBuffer cmd = ctx.cmd;
    if (hdr == nullptr || ds == nullptr || ds->texture() == nullptr || !dev_.gbufferWritten()) {
        ++skipped_;
        needReset_ = true;
        return;
    }
    // render size = the scene target, display size = the main camera's real rect (the output)
    const VkExtent2D render{hdr->desc().width, hdr->desc().height};
    const std::array<std::uint32_t, 2> disp = dev_.displaySize();
    const VkExtent2D display = disp[0] != 0 && disp[1] != 0 ? VkExtent2D{disp[0], disp[1]} : render;
    if (ds->width() < render.width || ds->height() < render.height || !ensureFsr(render, display)) {
        ++skipped_;
        needReset_ = true;
        return;
    }
    // the jitter sequence and the texture bias follow the actual sizes (the next frame's geometry)
    phases_ = upscaleJitterPhases(render.width, display.width);
    dev_.setGeometryLodBias(upscaleLodBias(render.width, display.width));
    vk::Transients::Entry* motion =
        dev_.transients().get("gbuffer.motion", {kGbufferMotionFormat, {render.width, render.height}, kGbufferMotionUsage});
    vk::Transients::Entry* depth = dev_.transients().get("upscale.depth", {kDepthFormat, render, kDepthUsage});
    vk::Transients::Entry* out = dev_.transients().get("upscale.out", {kOutputFormat, display, kOutputUsage});
    if (motion == nullptr || depth == nullptr || out == nullptr || !dev_.readable(ds->texture(), cmd) || !dev_.readable(hdr, cmd)) {
        ++skipped_;
        needReset_ = true;
        return;
    }

    // hardware depth -> R32F
    PrepPush push{};
    push.h.frame = ctx.frameBlock;
    push.depthTex = ds->texture()->bindlessSlot();
    push.width = render.width;
    push.height = render.height;
    depth->image.transition(cmd, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prep_);
    layout_.bindSet0(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev_.bindless());
    layout_.pushStorageImage(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, 0, depth->image.view());
    layout_.push(cmd, &push, sizeof(push));
    vkCmdDispatch(cmd, (render.width + 7) / 8, (render.height + 7) / 8, 1);

    // the automatic reactive mask: translucent effects (particles, rain, spells, spray) drawn after the BeforeAlpha snapshot
    vk::Transients::Entry* reactive = nullptr;
    if (reactive_ && opaqueValid_) {
        vk::Transients::Entry* opaque = dev_.transients().get("upscale.opaque", {kOutputFormat, render, kOpaqueUsage});
        reactive = dev_.transients().get("upscale.reactive", {kReactiveFormat, render, kReactiveUsage});
        if (opaque != nullptr && reactive != nullptr && !fsr_->generateReactiveMask(cmd, opaque->image, hdr->image(), reactive->image, render, reactiveScale_, reactiveCutoff_)) {
            reactive = nullptr;
        }
    }
    opaqueValid_ = false;

    const d3d9::scene::CameraBlock& cam = dev_.camera();
    const d3d9::scene::CameraBlock& last = dev_.lastCamera();
    vk::FsrDispatch d;
    d.reactive = reactive != nullptr ? &reactive->image : nullptr;
    d.cmd = cmd;
    d.color = &hdr->image();
    d.depth = &depth->image;
    d.motion = &motion->image;
    d.output = &out->image;
    d.renderSize = render;
    d.upscaleSize = display;
    const std::array<float, 2> j = dev_.jitter();
    d.jitterX = (flip_ & 1u) != 0 ? -j[0] : j[0];  // what FSR is told: the unflipped sequence
    d.jitterY = (flip_ & 2u) != 0 ? -j[1] : j[1];
    d.sharpness = sharpness_;
    d.motionScaleX = static_cast<float>(render.width);  // the G-buffer's motion is in uv (previous - current)
    d.motionScaleY = static_cast<float>(render.height);
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
        dev_.setRenderRatio(1.0f);
        spdlog::error("enh: upscale: the FSR dispatch failed: FSR stays off this session");
        return;
    }
    needReset_ = false;

    // the post chain samples the upscaled image where it would sample the engine's scene image (both by uv)
    out->image.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    dev_.setSceneSubstitute(hdr, out->slot);
    ++frames_;
    const double now = dev_.secondsSinceCreate();
    if (now - lastLog_ > 5.0) {
        lastLog_ = now;
        spdlog::info("enh: upscale: {} frames upscaled ({} skipped), provider '{}', {}x{} -> {}x{}, jitter ({:.3f}, {:.3f}) px, reset {}",
                     frames_, skipped_, fsr_->providerName(), render.width, render.height, display.width, display.height, d.jitterX,
                     d.jitterY, d.reset ? "yes" : "no");
    }
}

}  // namespace rvk::enh
