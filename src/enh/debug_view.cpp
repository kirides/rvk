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
        dev_.releaseTransient("debug.inject_view");
        return;
    }
    dev_.setFrameParam(abi::kFrameParamDebugView, {mode == Mode::Depth ? 1.0f : 2.0f, kStrength, kGridSpacing, 0.0f});
    if (passId_ == 0) {
        passId_ = dev_.addPass(d3d9::inject::Point::AfterOpaque, "debug.inject_view", [this](d3d9::PassContext& ctx) { record(ctx); },
                               this);
    }
    frames_ = 0;
}

// (the pipelines are RHI objects: their destruction goes through Frames::retire, the GPU may still use them)
void DebugView::destroyPipelines() {
    compute_.reset();
    composite_.reset();
    compositeFormat_ = VK_FORMAT_UNDEFINED;
}

bool DebugView::ensurePipelines(rhi::Device& rdev, VkFormat target) {
    if (failed_) return false;
    if (!compute_) {
        rhi::ComputePipelineDesc cd;
        cd.cs.spirv = shader::embedded::spirv("debug/inject_view.comp");
        cd.name = "debug.inject_view compute";
        compute_ = rdev.createComputePipeline(cd);
    }
    if (composite_ && compositeFormat_ != target) composite_.reset();
    if (!composite_) {
        rhi::GraphicsPipelineDesc gd;
        gd.vs.spirv = shader::embedded::spirv("debug/fullscreen_rhi.vert");
        gd.ps.spirv = shader::embedded::spirv("debug/inject_view.frag");
        gd.colorFormats[0] = target;
        // dst * src: the factor image multiplies the scene (keeps the engine's exposure); alpha kept
        gd.blend.blendEnable = VK_TRUE;
        gd.blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        gd.blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        gd.blend.colorBlendOp = VK_BLEND_OP_ADD;
        gd.blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        gd.blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        gd.blend.alphaBlendOp = VK_BLEND_OP_ADD;
        gd.name = "debug.inject_view composite";
        composite_ = rdev.createGraphicsPipeline(gd);
        compositeFormat_ = target;
    }
    if (!compute_ || !composite_) {
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
    rhi::Device* rdev = dev_.rhi();
    if (rdev == nullptr) return;
    rhi::Texture& target = scene->rhi();
    if (!ensurePipelines(*rdev, target.desc().format)) return;
    const std::uint32_t w = scene->desc().width, h = scene->desc().height;
    rhi::Texture* factor = dev_.transient("debug.inject_view", {VK_FORMAT_R16G16B16A16_SFLOAT, {w, h}});
    if (factor == nullptr) return;
    rhi::CommandList& cmd = ctx.cmd;

    DebugViewPush push{};
    push.h.frame = ctx.frameBlock.bits;
    push.h.passId = 1;
    push.width = w;
    push.height = h;
    if (mode_ == Mode::Depth && ctx.depthMap != nullptr && dev_.readable(ctx.depthMap, cmd)) {
        push.depthTex = ctx.depthMap->bindlessSlot();
    }
    push.srcTex = factor->heapSampled();
    push.srcSampler = 0;  // the bindless heap's point / clamp sampler (same size: one texel per pixel)

    rhi::Texture* const factorTex = factor;
    rhi::Texture* const targetTex = &target;
    rhi::CommandList* const cl = &cmd;

    // compute: the factor image (its previous use was last frame's composite read: the tracked layout orders the write)
    cl->transition(*factorTex, VK_IMAGE_LAYOUT_GENERAL);
    cl->bindPipeline(*compute_);
    rhi::Texture* const storage[] = {factorTex};
    cl->setStorageTextures(storage);
    cl->setPushConstants(&push, sizeof(push));
    cl->dispatch((w + 7) / 8, (h + 7) / 8, 1);

    // composite: the scene HDR target *= factor
    cl->transition(*factorTex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl->transition(*targetTex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    rhi::RenderingInfo ri;
    ri.color[0].texture = targetTex;
    ri.color[0].load = VK_ATTACHMENT_LOAD_OP_LOAD;
    ri.color[0].store = VK_ATTACHMENT_STORE_OP_STORE;
    ri.colorCount = 1;
    ri.width = w;
    ri.height = h;
    cl->beginRendering(ri);
    cl->bindPipeline(*composite_);
    cl->setPushConstants(&push, sizeof(push));
    cl->draw(3, 1, 0, 0);
    cl->endRendering();
    if (frames_++ == 0) {
        spdlog::info("enh: debug.inject_view {} at {}: {}x{}, depth map {}", mode_ == Mode::Depth ? "depth" : "tint",
                     d3d9::inject::name(ctx.point), w, h, push.depthTex != 0 ? "slot " + std::to_string(push.depthTex) : "none");
    }
}

}  // namespace rvk::enh
