#include "enh/debug_view.h"

#include <spdlog/spdlog.h>

#include "shader/abi.h"
#include "shader/embedded.h"

namespace rvk::enh {

namespace {

namespace abi = shader::abi;

constexpr float kStrength = 0.6f;
constexpr float kGridSpacing = 500.0f;  // world units (cm): 5 m

}  // namespace

DebugView::Mode DebugView::parse(std::string_view option) {
    return option == "depth" ? Mode::Depth : option == "tint" ? Mode::Tint : Mode::Off;
}

DebugView::DebugView(d3d9::Device& device) : dev_(device) {}

DebugView::~DebugView() {
    setMode(Mode::Off);
    destroyPipelines();
}

void DebugView::setMode(Mode mode) {
    if (mode == mode_) return;
    mode_ = mode;
    if (mode == Mode::Off) {
        if (passId_ != 0) dev_.removePass(passId_);
        passId_ = 0;
        dev_.setFrameParam(abi::kFrameParamDebugView, {0.0f, 0.0f, 0.0f, 0.0f});
        dev_.transients().release("debug.inject_view");
        return;
    }
    dev_.setFrameParam(abi::kFrameParamDebugView, {mode == Mode::Depth ? 1.0f : 2.0f, kStrength, kGridSpacing, 0.0f});
    if (passId_ == 0) {
        passId_ = dev_.addPass(d3d9::inject::Point::AfterOpaque, "debug.inject_view", [this](d3d9::PassContext& ctx) { record(ctx); },
                               this);
    }
    frames_ = 0;
}

void DebugView::destroyPipelines() {
    vk::Context* ctx = &dev_.ctx();
    const VkPipeline c = compute_, g = composite_;
    vk::PassLayout layout = layout_;
    if (c != VK_NULL_HANDLE || g != VK_NULL_HANDLE || layout.valid()) {
        dev_.frames().retire([ctx, c, g, layout]() mutable {
            if (c != VK_NULL_HANDLE) vkDestroyPipeline(ctx->device(), c, nullptr);
            if (g != VK_NULL_HANDLE) vkDestroyPipeline(ctx->device(), g, nullptr);
            layout.destroy(*ctx);
        });
    }
    compute_ = composite_ = VK_NULL_HANDLE;
    layout_ = vk::PassLayout{};
    compositeFormat_ = VK_FORMAT_UNDEFINED;
}

bool DebugView::ensurePipelines(VkFormat target) {
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
        compute_ = vk::createComputePipeline(ctx, layout_.layout(), shader::embedded::spirv("debug/inject_view.comp"),
                                             "debug.inject_view compute");
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
        gd.fs = shader::embedded::spirv("debug/inject_view.frag");
        gd.color = target;
        // dst * src: the factor image multiplies the scene (keeps the engine's exposure); alpha kept
        gd.blend.enable = 1;
        gd.blend.srcColor = VK_BLEND_FACTOR_DST_COLOR;
        gd.blend.dstColor = VK_BLEND_FACTOR_ZERO;
        gd.blend.srcAlpha = VK_BLEND_FACTOR_ZERO;
        gd.blend.dstAlpha = VK_BLEND_FACTOR_ONE;
        gd.name = "debug.inject_view composite";
        composite_ = vk::createPassGraphicsPipeline(ctx, layout_.layout(), gd);
        compositeFormat_ = target;
    }
    if (compute_ == VK_NULL_HANDLE || composite_ == VK_NULL_HANDLE) {
        spdlog::error("enh: debug.inject_view: pipelines failed, the view stays off this session");
        failed_ = true;
        return false;
    }
    return true;
}

void DebugView::record(d3d9::PassContext& ctx) {
    d3d9::Texture* scene = ctx.sceneHdr;
    // the frame's first main view only, and only when it renders into the image effect's scene target
    if (ctx.mainView != 1 || scene == nullptr || ctx.target != scene) return;
    vk::Image& target = scene->image();
    if (!ensurePipelines(target.format())) return;
    const std::uint32_t w = scene->desc().width, h = scene->desc().height;
    vk::Transients::Entry* factor = dev_.transients().get("debug.inject_view", {VK_FORMAT_R16G16B16A16_SFLOAT, {w, h}});
    if (factor == nullptr) return;
    VkCommandBuffer cmd = ctx.cmd;

    DebugViewPush push{};
    push.h.frame = ctx.frameBlock;
    push.h.passId = 1;
    push.width = w;
    push.height = h;
    if (mode_ == Mode::Depth && ctx.depthMap != nullptr && dev_.readable(ctx.depthMap, cmd)) {
        push.depthTex = ctx.depthMap->bindlessSlot();
    }
    push.srcTex = factor->slot;
    push.srcSampler = 0;  // the bindless heap's point / clamp sampler (same size: one texel per pixel)

    // compute: the factor image (its previous use was last frame's composite read: the tracked layout orders the write)
    factor->image.transition(cmd, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_);
    layout_.bindSet0(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev_.bindless());
    layout_.pushStorageImage(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, 0, factor->image.view());
    layout_.push(cmd, &push, sizeof(push));
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);

    // composite: the scene HDR target *= factor
    factor->image.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
        spdlog::info("enh: debug.inject_view {} at {}: {}x{}, depth map {}", mode_ == Mode::Depth ? "depth" : "tint",
                     d3d9::inject::name(ctx.point), w, h, push.depthTex != 0 ? "slot " + std::to_string(push.depthTex) : "none");
    }
}

}  // namespace rvk::enh
