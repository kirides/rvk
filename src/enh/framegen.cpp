#include "enh/framegen.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>

#include <spdlog/spdlog.h>

#include "core/paths.h"
#include "enh/gbuffer.h"
#include "shader/embedded.h"
#include "vk/context.h"
#include "vk/frames.h"

namespace rvk::enh {

namespace {

// mirror of shaders/upscale/prep.comp UpscalePush
struct PrepPush {
    shader::abi::PassPushHeader h;
    std::uint32_t depthTex, width, height, pad;
};
static_assert(sizeof(PrepPush) == 32 && sizeof(PrepPush) <= shader::abi::kPushConstantBudget);

// mirror of shaders/framegen/debug.frag FramegenPush
struct DebugPush {
    shader::abi::PassPushHeader h;
    std::uint32_t srcTex, sampler0, mode, pad;
    float nearZ, farZ, pad1, pad2;
};
static_assert(sizeof(DebugPush) == 48 && sizeof(DebugPush) <= shader::abi::kPushConstantBudget);

constexpr VkFormat kDepthFormat = VK_FORMAT_R32_SFLOAT;
constexpr VkImageUsageFlags kDepthUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
// colour images: read by the generator (sampled / storage loads), copied from and to, blitted to the swapchain
constexpr VkImageUsageFlags kColorUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;

void copyImage(VkCommandBuffer cmd, vk::Image& src, vk::Image& dst, VkExtent2D size) {
    src.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    dst.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageCopy region{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                             {size.width, size.height, 1}};
    vkCmdCopyImage(cmd, src.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

}  // namespace

// ---- the rules ------------------------------------------------------------------------------------------------------------

FrameGenBlock frameGenBlocker(const FrameGenFacts& f) {
    if (!f.providerReady) return FrameGenBlock::NoProvider;
    if (f.unpaced) return FrameGenBlock::Loading;
    if (!f.gameplay) return FrameGenBlock::Menu;
    if (f.paused) return FrameGenBlock::Paused;
    if (!f.mainView) return FrameGenBlock::NoMainView;
    if (!f.inputs) return FrameGenBlock::NoInputs;
    if (!f.sizeOk) return FrameGenBlock::Size;
    return FrameGenBlock::None;
}

const char* frameGenBlockName(FrameGenBlock b) {
    switch (b) {
        case FrameGenBlock::None: return "running";
        case FrameGenBlock::NoProvider: return "the frame generation library or context is not available";
        case FrameGenBlock::Menu: return "a menu page is open";
        case FrameGenBlock::Loading: return "a loading screen";
        case FrameGenBlock::Paused: return "the game is paused";
        case FrameGenBlock::NoMainView: return "the frame has no world view";
        case FrameGenBlock::NoInputs: return "no depth / motion vectors for this frame";
        case FrameGenBlock::Size: return "the render size is larger than the display";
    }
    return "?";
}

bool frameGenCameraCut(const FrameGenPose& prev, const FrameGenPose& cur, const FrameGenCutLimits& limits) {
    if (!prev.valid || !cur.valid) return true;
    const float dx = cur.position[0] - prev.position[0], dy = cur.position[1] - prev.position[1], dz = cur.position[2] - prev.position[2];
    if (dx * dx + dy * dy + dz * dz > limits.teleport * limits.teleport) return true;
    const float dot = prev.forward[0] * cur.forward[0] + prev.forward[1] * cur.forward[1] + prev.forward[2] * cur.forward[2];
    const float len = std::sqrt((prev.forward[0] * prev.forward[0] + prev.forward[1] * prev.forward[1] + prev.forward[2] * prev.forward[2]) *
                                (cur.forward[0] * cur.forward[0] + cur.forward[1] * cur.forward[1] + cur.forward[2] * cur.forward[2]));
    if (len > 1e-12f) {
        const float cosine = std::clamp(dot / len, -1.0f, 1.0f);
        if (std::acos(cosine) * 57.29578f > limits.turnDegrees) return true;
    }
    if (prev.fovY > 0.0f && cur.fovY > 0.0f) {
        const float ratio = cur.fovY > prev.fovY ? cur.fovY / prev.fovY : prev.fovY / cur.fovY;
        if (ratio > limits.fovRatio) return true;
    }
    return false;
}

FrameGenState::Step FrameGenState::advance(FrameGenBlock blocker, bool cameraCut) {
    Step s;
    if (blocker != FrameGenBlock::None) {
        wasActive_ = false;  // the next active frame starts over
        return s;
    }
    s.active = true;
    s.reset = !wasActive_ || cameraCut;
    s.show = !s.reset;  // a reset frame passes the real frame through: nothing to interpolate to
    s.frameId = ++frameId_;
    wasActive_ = true;
    return s;
}

// ---- the feature ----------------------------------------------------------------------------------------------------------

FrameGeneration::FrameGeneration(d3d9::Device& device) : dev_(device) {}

FrameGeneration::~FrameGeneration() {
    disable();
    vk::Context* ctx = &dev_.ctx();
    const VkPipeline prep = prep_, debug = debug_;
    vk::PassLayout layout = layout_, debugLayout = debugLayout_;
    dev_.frames().retire([ctx, prep, debug, layout, debugLayout]() mutable {
        if (prep != VK_NULL_HANDLE) vkDestroyPipeline(ctx->device(), prep, nullptr);
        if (debug != VK_NULL_HANDLE) vkDestroyPipeline(ctx->device(), debug, nullptr);
        if (layout.valid()) layout.destroy(*ctx);
        if (debugLayout.valid()) debugLayout.destroy(*ctx);
    });
}

void FrameGeneration::apply(const Settings& s) {
    const bool wasOn = on_;
    const bool wantOn = s.mode != FrameGenMode::Off && !failed_;
    const bool modeChanged = s.mode != s_.mode;
    s_ = s;
    if (modeChanged && fg_) {
        dropContext();  // the other provider's context is built with the next frame
    }
    if (wantOn && !wasOn) {
        enable();
    } else if (!wantOn && wasOn) {
        disable();
    }
    if (wantOn && !s.manual && !manualNoticed_) {
        manualNoticed_ = true;
        spdlog::info("enh: framegen: nothing is presented yet: the AMD swapchain (stage F3) is not implemented; framegen.manual = true "
                     "generates and shows the frames through our own swapchain (test)");
    }
}

void FrameGeneration::enable() {
    if (on_) return;
    if (!layout_.valid()) {
        const VkDescriptorSetLayoutBinding bind{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        if (!layout_.create(dev_.ctx(), dev_.bindless(), {&bind, 1})) {
            failed_ = true;
            return;
        }
        prep_ = vk::createComputePipeline(dev_.ctx(), layout_.layout(), shader::embedded::spirv("upscale/prep.comp"), "framegen.depth");
        if (prep_ == VK_NULL_HANDLE) {
            failed_ = true;
            return;
        }
    }
    on_ = true;
    state_.invalidate();
    dev_.addPass(d3d9::inject::Point::AfterMainView, "framegen.depth", [this](d3d9::PassContext& c) { recordDepth(c); }, this, 300);
    dev_.addPass(d3d9::inject::Point::BeforeGui, "framegen.hudless", [this](d3d9::PassContext& c) { recordHudless(c); }, this, 100);
    dev_.addPass(d3d9::inject::Point::BeforePresent, "framegen.generate", [this](d3d9::PassContext& c) { recordGenerate(c); }, this, 0);
    dev_.addPass(d3d9::inject::Point::BeforePresent, "framegen.debug", [this](d3d9::PassContext& c) { recordDebug(c); }, this, 100);
    spdlog::info("enh: framegen: on ({}{})", s_.mode == FrameGenMode::Fsr4 ? "FSR 4" : "FSR 3", s_.manual ? ", manual presentation" : "");
}

void FrameGeneration::disable() {
    if (!on_) return;
    on_ = false;
    dev_.removePasses(this);
    dev_.setGeneratedFrame(nullptr);
    generated_ = nullptr;
    dropContext();
    for (const char* k : {"framegen.depth", "framegen.hudless", "framegen.color", "framegen.out"}) dev_.transients().release(k);
    depthFrame_ = 0;
    hudlessFrame_ = false;
    lastPose_ = {};
    spdlog::info("enh: framegen: off ({} frames generated, {} shown)", frames_, shown_);
}

void FrameGeneration::dropContext() {
    if (!fg_) return;
    // the context's internal images may still be in use by frames in flight
    dev_.frames().retire([f = std::move(fg_)]() mutable { f->destroy(); });
    fg_.reset();
    ctxDisplay_ = {};
    state_.invalidate();
}

bool FrameGeneration::ensureContext(VkExtent2D display, VkFormat format) {
    if (fg_ && fg_->hasContext() && ctxDisplay_.width == display.width && ctxDisplay_.height == display.height) return true;
    dropContext();
    auto f = std::make_shared<vk::FrameGen>();
    const std::filesystem::path dll = paths::dataDir() / "amd_fidelityfx_loader_vk.dll";
    vk::FrameGenCreateInfo ci;
    ci.ml = s_.mode == FrameGenMode::Fsr4;
    ci.displaySize = ci.maxRenderSize = display;
    ci.backBufferFormat = ci.hudlessFormat = format;
    bool ok = f->load(dll) && f->createContext(dev_.ctx(), ci);
    if (!ok && ci.ml && f->loaded()) {
        spdlog::warn("enh: framegen: the FSR 4 (ML) frame generation is not available on this GPU: FSR 3");
        ci.ml = false;
        ok = f->createContext(dev_.ctx(), ci);
    }
    if (!ok) {
        spdlog::error("enh: framegen: frame generation is not available ({}): it stays off this session", dll.string());
        failed_ = true;
        return false;
    }
    mlActive_ = ci.ml;
    fg_ = std::move(f);
    ctxDisplay_ = display;
    state_.invalidate();
    spdlog::info("enh: framegen: provider '{}' for {}x{}", fg_->providerName(), display.width, display.height);
    return true;
}

// ---- AfterMainView: the depth ----------------------------------------------------------------------------------------------

void FrameGeneration::recordDepth(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || !on_ || failed_) return;
    depthFrame_ = 0;
    d3d9::Surface* ds = dev_.depthStencil();
    d3d9::Texture* hdr = ctx.sceneHdr;
    VkCommandBuffer cmd = ctx.cmd;
    if (hdr == nullptr || ds == nullptr || ds->texture() == nullptr || !dev_.gbufferWritten()) return;
    const VkExtent2D render{hdr->desc().width, hdr->desc().height};
    if (ds->width() < render.width || ds->height() < render.height) return;
    vk::Transients::Entry* depth = dev_.transients().get("framegen.depth", {kDepthFormat, render, kDepthUsage});
    vk::Transients::Entry* motion =
        dev_.transients().get("gbuffer.motion", {kGbufferMotionFormat, {render.width, render.height}, kGbufferMotionUsage});
    if (depth == nullptr || motion == nullptr || !dev_.readable(ds->texture(), cmd)) return;
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
    depthFrame_ = dev_.stats().frames + 1;
    renderSize_ = render;
}

// ---- BeforeGui: the frame without the GUI ----------------------------------------------------------------------------------

void FrameGeneration::recordHudless(d3d9::PassContext& ctx) {
    hudlessFrame_ = false;
    if (!on_ || failed_ || !((s_.manual && gameplay_ && !paused_) || s_.view == FrameGenView::Hudless)) return;
    d3d9::Surface* bb = dev_.backBuffer();
    if (bb == nullptr || bb->texture() == nullptr) return;
    vk::Image& src = bb->texture()->image();
    const VkExtent2D size{bb->width(), bb->height()};
    vk::Transients::Entry* h = dev_.transients().get("framegen.hudless", {src.format(), size, kColorUsage});
    if (h == nullptr) return;
    copyImage(ctx.cmd, src, h->image, size);
    h->image.transition(ctx.cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    hudlessFrame_ = true;
}

// ---- BeforePresent: the interpolated frame ----------------------------------------------------------------------------------

void FrameGeneration::logBlock(FrameGenBlock b) {
    if (b == lastBlock_) return;
    lastBlock_ = b;
    if (b == FrameGenBlock::None) {
        spdlog::info("enh: framegen: running again at present {}", dev_.stats().frames + 1);
    } else {
        spdlog::info("enh: framegen: not generating at present {}: {}", dev_.stats().frames + 1, frameGenBlockName(b));
    }
}

void FrameGeneration::recordGenerate(d3d9::PassContext& ctx) {
    generated_ = nullptr;
    generatedShown_ = false;
    if (!on_ || failed_ || !s_.manual) return;
    d3d9::Surface* bb = dev_.backBuffer();
    if (bb == nullptr || bb->texture() == nullptr) return;
    vk::Image& real = bb->texture()->image();
    const VkExtent2D display{bb->width(), bb->height()};
    const d3d9::scene::CameraBlock& cam = dev_.camera();
    const d3d9::scene::CameraBlock& last = dev_.lastCamera();

    FrameGenFacts facts;
    facts.providerReady = !failed_;
    facts.gameplay = gameplay_;
    facts.paused = paused_;
    facts.unpaced = dev_.unpaced();
    facts.mainView = cam.valid;
    facts.inputs = depthFrame_ == dev_.stats().frames + 1;
    facts.sizeOk = renderSize_.width <= display.width && renderSize_.height <= display.height;

    // the camera of this frame against the last one (cut detection); kept up to date through blocked frames too
    FrameGenPose pose;
    if (cam.valid) {
        pose.valid = true;
        pose.position = cam.position;
        pose.forward = {cam.inverseView.at(2, 0), cam.inverseView.at(2, 1), cam.inverseView.at(2, 2)};
        pose.fovY = cam.projection.at(1, 1) > 0.0f ? 2.0f * std::atan(1.0f / cam.projection.at(1, 1)) : 1.0f;
    }
    const bool cut = cam.valid && lastPose_.valid && frameGenCameraCut(lastPose_, pose);
    lastPose_ = pose;

    FrameGenBlock blocker = frameGenBlocker(facts);
    if (blocker == FrameGenBlock::None && !ensureContext(display, real.format())) blocker = FrameGenBlock::NoProvider;
    const FrameGenState::Step step = state_.advance(blocker, cut);
    logBlock(blocker);
    if (!step.active) {
        ++blocked_;
        return;
    }
    if (cut) ++cuts_;
    if (step.reset) ++resets_;

    VkCommandBuffer cmd = ctx.cmd;
    // The DLL's Vulkan backend advances a small ring of resource-view slots per ffxDispatch and destroys the views of the slot it
    // enters (the same as the upscaler's, see vk/fsr.h): configure / prepare / dispatch of one frame use the ring up, so the previous
    // frame's commands must have finished before this frame's are recorded (validation: vkDestroyImageView in use). Cheap while the
    // GPU keeps up; a wait (counted in the 5 s line) when it does not.
    {
        const std::uint64_t previous = dev_.frames().currentValue() - 1;
        if (previous > 0 && dev_.frames().completedValue() < previous) {
            const auto w0 = std::chrono::steady_clock::now();
            dev_.frames().waitFor(previous);
            ++ringWaits_;
            ringWaitMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
        }
    }
    const VkFormat fmt = real.format();
    vk::Transients::Entry* color = dev_.transients().get("framegen.color", {fmt, display, kColorUsage});
    vk::Transients::Entry* out = dev_.transients().get("framegen.out", {fmt, display, kColorUsage});
    vk::Transients::Entry* depth = dev_.transients().get("framegen.depth", {kDepthFormat, renderSize_, kDepthUsage});
    vk::Transients::Entry* motion =
        dev_.transients().get("gbuffer.motion", {kGbufferMotionFormat, {renderSize_.width, renderSize_.height}, kGbufferMotionUsage});
    vk::Transients::Entry* hudless =
        hudlessFrame_ ? dev_.transients().get("framegen.hudless", {fmt, display, kColorUsage}) : nullptr;
    if (color == nullptr || out == nullptr || depth == nullptr || motion == nullptr) return;

    // the finished frame (GUI included): the colour the interpolation reads and keeps for the next frame
    copyImage(cmd, real, color->image, display);

    vk::FrameGenConfigure cfg;
    cfg.enabled = true;
    cfg.hudless = hudless != nullptr ? &hudless->image : nullptr;
    cfg.frameId = step.frameId;
    if (hudless != nullptr) hudless->image.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    bool ok = fg_->configure(cfg);

    vk::FrameGenPrepare p;
    p.cmd = cmd;
    p.frameId = step.frameId;
    p.renderSize = renderSize_;
    const std::array<float, 2> j = dev_.jitter();  // what the upscaler was told (its jitter_flip is a test switch of the geometry only)
    p.jitterX = j[0];
    p.jitterY = j[1];
    p.motionScaleX = static_cast<float>(renderSize_.width);  // the G-buffer's motion is in uv (previous - current)
    p.motionScaleY = static_cast<float>(renderSize_.height);
    p.reset = step.reset;
    p.frameTimeMs = cam.valid && last.valid ? static_cast<float>(std::clamp((cam.time - last.time) * 1000.0, 1.0, 100.0)) : 16.6f;
    p.cameraNear = cam.nearZ > 0.0f ? cam.nearZ : 10.0f;
    p.cameraFar = cam.farZ > p.cameraNear ? cam.farZ : 100000.0f;
    p.cameraFovY = pose.fovY;
    p.viewSpaceToMeters = 0.01f;  // the engine's units are centimetres
    p.depth = &depth->image;
    p.motion = &motion->image;
    for (int i = 0; i < 3; ++i) {
        p.position[i] = cam.position[static_cast<std::size_t>(i)];
        p.right[i] = cam.inverseView.at(0, i);  // the inverse view's rows are the camera's axes (D3D row vectors)
        p.up[i] = cam.inverseView.at(1, i);
        p.forward[i] = cam.inverseView.at(2, i);
    }
    ok = ok && fg_->prepare(p);

    vk::FrameGenDispatch d;
    d.cmd = cmd;
    d.frameId = step.frameId;
    d.color = &color->image;
    d.output = &out->image;
    d.reset = step.reset;
    ok = ok && fg_->dispatch(d);
    if (!ok) {
        spdlog::error("enh: framegen: a call of the generator failed: frame generation stays off this session");
        failed_ = true;
        dev_.setGeneratedFrame(nullptr);
        return;
    }
    ++frames_;
    generated_ = &out->image;
    generatedShown_ = step.show;
    generatedWidth_ = display.width;
    generatedHeight_ = display.height;
    if (step.show) {
        ++shown_;
        dev_.setGeneratedFrame(&out->image);
    }
    const double now = dev_.secondsSinceCreate();
    if (now - lastLog_ > 5.0) {
        lastLog_ = now;
        spdlog::info("enh: framegen: {} frames generated ({} shown, {} resets, {} camera cuts, {} frames without generation), provider '{}', "
                     "{}x{} (render {}x{}), jitter ({:.3f}, {:.3f}) px, frame time {:.1f} ms, generated frames presented {}, previous-frame waits {} ({:.1f} ms)",
                     frames_, shown_, resets_, cuts_, blocked_, fg_->providerName(), display.width, display.height, renderSize_.width,
                     renderSize_.height, p.jitterX, p.jitterY, p.frameTimeMs, dev_.stats().generatedPresented, ringWaits_, ringWaitMs_);
    }
}

// ---- BeforePresent: the debug view -------------------------------------------------------------------------------------------

bool FrameGeneration::ensurePipelines(VkFormat target) {
    vk::Context& vctx = dev_.ctx();
    if (!debugLayout_.valid() && !debugLayout_.create(vctx, dev_.bindless(), {})) return false;
    if (debug_ != VK_NULL_HANDLE && debugFormat_ != target) {
        const VkPipeline old = debug_;
        vk::Context* c = &vctx;
        dev_.frames().retire([c, old] { vkDestroyPipeline(c->device(), old, nullptr); });
        debug_ = VK_NULL_HANDLE;
    }
    if (debug_ == VK_NULL_HANDLE) {
        vk::PassGraphicsDesc gd;
        gd.vs = shader::embedded::spirv("debug/fullscreen.vert");
        gd.fs = shader::embedded::spirv("framegen/debug.frag");
        gd.color = target;
        gd.name = "framegen.debug";
        debug_ = vk::createPassGraphicsPipeline(vctx, debugLayout_.layout(), gd);
        debugFormat_ = target;
    }
    return debug_ != VK_NULL_HANDLE;
}

void FrameGeneration::recordDebug(d3d9::PassContext& ctx) {
    if (!on_ || failed_ || s_.view == FrameGenView::Off) return;
    d3d9::Surface* bb = dev_.backBuffer();
    if (bb == nullptr || bb->texture() == nullptr) return;
    vk::Image& target = bb->texture()->image();
    const VkExtent2D size{bb->width(), bb->height()};
    const std::uint64_t frame = dev_.stats().frames + 1;
    vk::Transients::Entry* src = nullptr;
    std::uint32_t mode = 3;
    switch (s_.view) {
        case FrameGenView::Motion:
            if (depthFrame_ == frame) {
                src = dev_.transients().get("gbuffer.motion", {kGbufferMotionFormat, {renderSize_.width, renderSize_.height}, kGbufferMotionUsage});
            }
            mode = 1;
            break;
        case FrameGenView::Depth:
            if (depthFrame_ == frame) src = dev_.transients().get("framegen.depth", {kDepthFormat, renderSize_, kDepthUsage});
            mode = 2;
            break;
        case FrameGenView::Hudless:
            if (hudlessFrame_) src = dev_.transients().get("framegen.hudless", {target.format(), size, kColorUsage});
            break;
        case FrameGenView::Generated:
            if (generated_ != nullptr) src = dev_.transients().get("framegen.out", {target.format(), size, kColorUsage});
            break;
        case FrameGenView::Off: break;
    }
    if (src == nullptr || !ensurePipelines(target.format())) return;
    VkCommandBuffer cmd = ctx.cmd;
    src->image.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    DebugPush push{};
    push.h.frame = ctx.frameBlock;
    push.h.passId = 4;
    push.srcTex = src->slot;
    push.sampler0 = dev_.samplerFor(mode == 3, true);  // data views: point; colour: linear
    push.mode = mode;
    const d3d9::scene::CameraBlock& cam = dev_.camera();
    push.nearZ = cam.nearZ > 0.0f ? cam.nearZ : 10.0f;
    push.farZ = cam.farZ > push.nearZ ? cam.farZ : 100000.0f;
    target.transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = target.attachmentView(dev_.ctx(), 0, 0);
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, size};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0.0f, 0.0f, static_cast<float>(size.width), static_cast<float>(size.height), 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, size};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, debug_);
    debugLayout_.bindSet0(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dev_.bindless());
    debugLayout_.push(cmd, &push, sizeof(push));
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

bool FrameGeneration::readGenerated(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height) {
    if (generated_ == nullptr || !generatedShown_) return false;
    width = generatedWidth_;
    height = generatedHeight_;
    const VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;
    return dev_.readImageLevel(*generated_, 0, 0, {width, height, 1}, size, bgra);
}

}  // namespace rvk::enh
