#include "enh/rt_debug.h"

#include <cstring>
#include <string>

#include <spdlog/spdlog.h>

#include "enh/rt_scene.h"
#include "shader/embedded.h"
#include "vk/context.h"

namespace rvk::enh {

RtDebug::Mode RtDebug::parse(std::string_view option) {
    return option == "instances" ? Mode::Instances : option == "depth" ? Mode::Depth : option == "normals" ? Mode::Normals : Mode::Off;
}

RtDebug::RtDebug(d3d9::Device& device, RtScene& scene) : dev_(device), scene_(scene) {}

RtDebug::~RtDebug() {
    setMode(Mode::Off);
    destroyPipelines();
}

void RtDebug::readProbes(std::uint32_t slot) {
    if (probeFrame_[slot] == 0 || probes_[slot].mapped == nullptr) return;
    const std::uint64_t frame = probeFrame_[slot];
    probeFrame_[slot] = 0;
    vmaInvalidateAllocation(dev_.ctx().allocator(), probes_[slot].allocation, 0, VK_WHOLE_SIZE);
    const auto* r = static_cast<const RtDebugProbe*>(probes_[slot].mapped);
    const auto now = std::chrono::steady_clock::now();
    if (now - probeLogged_ >= std::chrono::seconds(5)) {
        probeLogged_ = now;
        static constexpr const char* kCodes[] = {"nothing", "match", "traced farther", "traced closer", "missed (raster has geometry)",
                                                  "hit (raster has none)"};
        int counts[6]{};
        for (std::uint32_t k = 0; k < kRtDebugProbes; ++k) counts[std::min<std::uint32_t>(r[k].code, 5)]++;
        spdlog::info("enh: rt.debug probes of frame {} (5 x 5 grid): {} match, {} traced farther, {} traced closer, {} missed, {} hit sky",
                     frame, counts[1], counts[2], counts[3], counts[4], counts[5]);
        for (std::uint32_t k = 0; k < kRtDebugProbes; ++k) {
            if (r[k].code == 0 || r[k].code == 1) continue;
            spdlog::info("enh: rt.debug   probe {},{}: {}: t {:.1f}, raster {:.1f}{}", k % 5, k / 5, kCodes[std::min<std::uint32_t>(r[k].code, 5)],
                         r[k].t, r[k].rasterT, r[k].record != 0 ? "; " + scene_.describe(frame, r[k].record - 1) : std::string());
        }
    }
    std::memset(probes_[slot].mapped, 0, sizeof(RtDebugProbe) * kRtDebugProbes);
}

void RtDebug::setMode(Mode mode) {
    if (mode == mode_) return;
    mode_ = mode;
    if (mode == Mode::Off) {
        if (passId_ != 0) dev_.removePass(passId_);
        if (depthPassId_ != 0) dev_.removePass(depthPassId_);
        passId_ = depthPassId_ = 0;
        dev_.transients().release("rt.debug");
        dev_.transients().release("rt.debug.depth");
        scene_.keepContents(false);
        scene_.release(this);
        vk::Context* ctx = &dev_.ctx();
        for (std::uint32_t i = 0; i < vk::Frames::kInFlight; ++i) {
            vk::rt::AddressBuffer b = probes_[i];
            probes_[i] = {};
            probeFrame_[i] = 0;
            if (b.buffer != VK_NULL_HANDLE) dev_.frames().retire([ctx, b]() mutable { b.destroy(*ctx); });
        }
        return;
    }
    if (!scene_.supported()) {
        spdlog::warn("enh: rt.debug: the ray tracing tier is off: nothing to show");
        return;
    }
    scene_.acquire(this);
    scene_.keepContents(true);
    if (passId_ == 0) {
        passId_ = dev_.addPass(d3d9::inject::Point::BeforeGui, "rt.debug", [this](d3d9::PassContext& ctx) { record(ctx); }, this);
    }
    if (mode == Mode::Depth && depthPassId_ == 0) {
        // after the RT scene's pass (order -100) at the same point
        depthPassId_ = dev_.addPass(d3d9::inject::Point::AfterOpaque, "rt.debug.depth", [this](d3d9::PassContext& ctx) { captureDepth(ctx); },
                                    this, 10);
    } else if (mode != Mode::Depth && depthPassId_ != 0) {
        dev_.removePass(depthPassId_);
        depthPassId_ = 0;
        dev_.transients().release("rt.debug.depth");
    }
    frames_ = 0;
}

void RtDebug::destroyPipelines() {
    vk::Context* ctx = &dev_.ctx();
    const VkPipeline c = compute_, g = composite_, d = depthCompute_;
    vk::PassLayout layout = layout_;
    if (c != VK_NULL_HANDLE || g != VK_NULL_HANDLE || d != VK_NULL_HANDLE || layout.valid()) {
        dev_.frames().retire([ctx, c, g, d, layout]() mutable {
            for (VkPipeline p : {c, g, d}) {
                if (p != VK_NULL_HANDLE) vkDestroyPipeline(ctx->device(), p, nullptr);
            }
            layout.destroy(*ctx);
        });
    }
    compute_ = composite_ = depthCompute_ = VK_NULL_HANDLE;
    layout_ = vk::PassLayout{};
    compositeFormat_ = VK_FORMAT_UNDEFINED;
}

bool RtDebug::ensurePipelines(VkFormat target) {
    if (failed_) return false;
    vk::Context& ctx = dev_.ctx();
    if (!layout_.valid()) {
        const VkDescriptorSetLayoutBinding out{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        if (!layout_.create(ctx, dev_.bindless(), {&out, 1})) {
            failed_ = true;
            return false;
        }
    }
    if (compute_ == VK_NULL_HANDLE) {
        compute_ = vk::createComputePipeline(ctx, layout_.layout(), shader::embedded::spirv("rt/rt_debug.comp"), "rt.debug compute");
    }
    if (depthCompute_ == VK_NULL_HANDLE) {
        depthCompute_ = vk::createComputePipeline(ctx, layout_.layout(), shader::embedded::spirv("rt/rt_debug_depth.comp"), "rt.debug depth");
    }
    if (composite_ != VK_NULL_HANDLE && compositeFormat_ != target) {
        const VkPipeline old = composite_;
        vk::Context* c = &ctx;
        dev_.frames().retire([c, old] { vkDestroyPipeline(c->device(), old, nullptr); });
        composite_ = VK_NULL_HANDLE;
    }
    if (composite_ == VK_NULL_HANDLE) {
        vk::PassGraphicsDesc gd;
        gd.vs = shader::embedded::spirv("debug/fullscreen.vert");
        gd.fs = shader::embedded::spirv("rt/rt_debug.frag");
        gd.color = target;
        gd.blend.enable = 1;  // src alpha over; the target's alpha kept
        gd.blend.srcColor = VK_BLEND_FACTOR_SRC_ALPHA;
        gd.blend.dstColor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        gd.blend.srcAlpha = VK_BLEND_FACTOR_ZERO;
        gd.blend.dstAlpha = VK_BLEND_FACTOR_ONE;
        gd.name = "rt.debug composite";
        composite_ = vk::createPassGraphicsPipeline(ctx, layout_.layout(), gd);
        compositeFormat_ = target;
    }
    if (compute_ == VK_NULL_HANDLE || composite_ == VK_NULL_HANDLE || depthCompute_ == VK_NULL_HANDLE) {
        spdlog::error("enh: rt.debug: pipelines failed, the view stays off this session");
        failed_ = true;
        return false;
    }
    return true;
}

void RtDebug::captureDepth(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || !dev_.rtSceneValid() || failed_) return;
    d3d9::Surface* ds = dev_.depthStencil();
    d3d9::Texture* scene = ctx.sceneHdr;
    if (ds == nullptr || ds->texture() == nullptr || scene == nullptr || ctx.target != scene) return;
    d3d9::Surface* bb = dev_.backBuffer();
    if (bb == nullptr || !ensurePipelines(bb->texture()->image().format())) return;
    const std::uint32_t w = std::min(scene->desc().width, ds->width()), h = std::min(scene->desc().height, ds->height());
    vk::Transients::Entry* out = dev_.transients().get("rt.debug.depth", {VK_FORMAT_R32_SFLOAT, {w, h}});
    if (out == nullptr || !dev_.readable(ds->texture(), ctx.cmd)) return;
    RtDebugPush push{};
    push.h.frame = ctx.frameBlock;
    push.h.passId = 2;
    push.depthTex = ds->texture()->bindlessSlot();
    push.width = w;
    push.height = h;
    out->image.transition(ctx.cmd, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, depthCompute_);
    layout_.bindSet0(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev_.bindless());
    layout_.pushStorageImage(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, 0, out->image.view());
    layout_.push(ctx.cmd, &push, sizeof(push));
    vkCmdDispatch(ctx.cmd, (w + 7) / 8, (h + 7) / 8, 1);
    out->image.transition(ctx.cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    depthFrame_ = dev_.frameValue();
    depthExtent_ = {w, h};
}

void RtDebug::record(d3d9::PassContext& ctx) {
    // only frames whose RT scene was built in this submission (a frame without a main view has none)
    if (!dev_.rtSceneValid()) return;
    d3d9::Surface* bb = dev_.backBuffer();
    if (bb == nullptr || bb->texture() == nullptr) return;
    d3d9::Texture* targetTex = bb->texture();
    vk::Image& target = targetTex->image();
    if (!ensurePipelines(target.format())) return;
    const std::uint32_t w = bb->width(), h = bb->height();
    vk::Transients::Entry* color = dev_.transients().get("rt.debug", {VK_FORMAT_R16G16B16A16_SFLOAT, {w, h}});
    if (color == nullptr) return;
    VkCommandBuffer cmd = ctx.cmd;

    RtDebugPush push{};
    push.h.frame = ctx.frameBlock;
    push.h.passId = 1;
    push.width = w;
    push.height = h;
    push.mode = static_cast<std::uint32_t>(mode_);
    // the raster's depth: this frame's hardware depth capture (depth mode), else the engine's linear depth map (z-pass only)
    vk::Transients::Entry* captured = mode_ == Mode::Depth && depthFrame_ == dev_.frameValue()
                                          ? dev_.transients().get("rt.debug.depth", {VK_FORMAT_R32_SFLOAT, depthExtent_})
                                          : nullptr;
    if (captured != nullptr) {
        push.depthTex = captured->slot;
        push.mode |= kRtDebugLinearDepth;
    } else if (ctx.depthMap != nullptr && dev_.readable(ctx.depthMap, cmd)) {
        push.depthTex = ctx.depthMap->bindlessSlot();
        if (mode_ == Mode::Depth && fallbacks_++ % 600 == 0) {
            spdlog::info("enh: rt.debug: no hardware depth capture this frame: the engine's depth map (z-pass draws only) is the "
                         "reference ({} frames so far)", fallbacks_);
        }
    }
    push.srcTex = color->slot;
    push.srcSampler = 0;  // point / clamp (same size: one texel per pixel)
    // probe results: the slot's previous ones are complete (Frames::begin waited for that submission)
    const std::uint32_t slot = dev_.frames().currentSlot();
    readProbes(slot);
    if (probes_[slot].buffer == VK_NULL_HANDLE) {
        probes_[slot].create(dev_.ctx(), sizeof(RtDebugProbe) * kRtDebugProbes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, 16, "rt.debug probes");
        if (probes_[slot].mapped != nullptr) std::memset(probes_[slot].mapped, 0, sizeof(RtDebugProbe) * kRtDebugProbes);
    }
    if (probes_[slot].mapped != nullptr) {
        vmaFlushAllocation(dev_.ctx().allocator(), probes_[slot].allocation, 0, VK_WHOLE_SIZE);
        push.probes[0] = static_cast<std::uint32_t>(probes_[slot].address);
        push.probes[1] = static_cast<std::uint32_t>(probes_[slot].address >> 32);
        probeFrame_[slot] = dev_.frameValue();
    }

    color->image.transition(cmd, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_);
    layout_.bindSet0(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev_.bindless());
    layout_.pushStorageImage(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, 0, color->image.view());
    layout_.push(cmd, &push, sizeof(push));
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);

    color->image.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    target.transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = target.attachmentView(dev_.ctx(), 0, 0);
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_);
    layout_.bindSet0(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dev_.bindless());
    layout_.push(cmd, &push, sizeof(push));
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    if (frames_++ == 0) {
        spdlog::info("enh: rt.debug mode {} at {}: {}x{}, depth map {}, TLAS {} instances", static_cast<int>(mode_),
                     d3d9::inject::name(ctx.point), w, h, push.depthTex != 0 ? "slot " + std::to_string(push.depthTex) : "none",
                     dev_.rtScene().instances);
    }
}

}  // namespace rvk::enh
