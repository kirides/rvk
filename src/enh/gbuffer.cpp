#include "enh/gbuffer.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "enh/material_variant.h"
#include "enh/rhi_scope.h"
#include "enh/rt_scene.h"
#include "shader/embedded.h"
#include "vk/context.h"
#include "vk/frames.h"

namespace rvk::enh {

namespace {

constexpr VkFormat kNormalFormat = kGbufferNormalFormat;
constexpr VkFormat kMotionFormat = kGbufferMotionFormat;
constexpr VkImageUsageFlags kNormalUsage = kGbufferNormalUsage;
constexpr VkImageUsageFlags kMotionUsage = kGbufferMotionUsage;

bool gbufferVariant(const d3d9::Shader& s, shader::TranslateOptions& o) { return materialVariantOptions(s, MaterialVariant::Gbuffer, o); }
bool gbufferMotionVariant(const d3d9::Shader& s, shader::TranslateOptions& o) {
    return materialVariantOptions(s, MaterialVariant::GbufferMotion, o);
}

}  // namespace

Gbuffer::Gbuffer(d3d9::Device& device, RtScene* scene) : dev_(device), scene_(scene) {
    rhi::Device* rdev = dev_.rhi();
    if (rdev == nullptr) {
        failed_ = true;
        return;
    }
    const auto build = [rdev](const char* file, const char* name) {
        return [rdev, file, name]() {
            rhi::ComputePipelineDesc d;
            d.cs.spirv = shader::embedded::spirv(file);
            d.name = name;
            return rdev->createComputePipeline(d);
        };
    };
    gtaoBuild_ = std::async(std::launch::async, build("gbuffer/gtao.comp", "gbuffer.gtao"));
    filterBuild_ = std::async(std::launch::async, build("gbuffer/gtao_filter.comp", "gbuffer.gtao filter"));
    dev_.setVariantProvider(d3d9::VariantSlot::Gbuffer, {materialVariantTag("gbuf"), gbufferVariant});
    // the replacement of the game's SSAO draws (ao.mode = replace)
    const auto make = [&](const char* name, shader::Stage stage) {
        auto s = std::make_shared<d3d9::Shader>();
        s->stage = stage;
        s->module = vk::createShaderModule(dev_.ctx(), shader::embedded::spirv(name));
        return s;
    };
    whiteVs_ = make("debug/fullscreen.vert", shader::Stage::Vertex);
    whitePs_ = make("gbuffer/ao_white.frag", shader::Stage::Pixel);
    whitePs_->reflection.colorOutputs = 1;
    dev_.setVariantProvider(d3d9::VariantSlot::GbufferMotion, {materialVariantTag("gbufm"), gbufferMotionVariant});
}

Gbuffer::~Gbuffer() {
    disable();
    if (replacing_) dev_.removeDrawReplacer(this);
    vk::Context* ctx = &dev_.ctx();
    for (auto* s : {&whiteVs_, &whitePs_}) {
        if (*s && (*s)->module != VK_NULL_HANDLE) {
            const VkShaderModule m = (*s)->module;
            dev_.frames().retire([ctx, m] { vkDestroyShaderModule(ctx->device(), m, nullptr); });
        }
        s->reset();
    }
    // (a pipeline still being built is waited for, then retired with the others)
    for (std::future<std::unique_ptr<rhi::Pipeline>>* f : {&gtaoBuild_, &filterBuild_}) {
        if (f->valid()) f->get();
    }
}

void Gbuffer::apply(const Settings& s) {
    s_ = s;
    s_.aoStrength = std::clamp(s_.aoStrength, 0.0f, 1.0f);
    s_.aoRadius = std::clamp(s_.aoRadius, 10.0f, 1000.0f);
    if (s.ao && !dev_.ctx().features().storageImageExtendedFormats && !failed_) {
        spdlog::warn("enh: gbuffer: the GPU lacks shaderStorageImageExtendedFormats (RG16F storage images): AO is not available");
        failed_ = true;
    }
    const bool want = (s.ao || s.debug != 0 || s.forced) && !failed_;
    if (want && !on_) enable();
    if (!want && on_) disable();
    if (on_ && !s_.ao) dev_.setFrameAo({});
    // the game's SSAO off while ours replaces it
    const bool replace = on_ && s_.ao && s_.replaceEngineSsao && whiteVs_ && whitePs_ && whiteVs_->module != VK_NULL_HANDLE &&
                         whitePs_->module != VK_NULL_HANDLE;
    if (replace && !replacing_) dev_.addDrawReplacer(this);
    if (!replace && replacing_) dev_.removeDrawReplacer(this);
    replacing_ = replace;
    spdlog::info("enh: gbuffer: {} (ao {}, game SSAO {}, strength {:.2f}, radius {:.0f}, debug {})", on_ ? "on" : "off", s_.ao ? "on" : "off",
                 replace ? "replaced" : "kept", s_.aoStrength, s_.aoRadius, s_.debug);
}

void Gbuffer::enable() {
    on_ = true;
    historyValid_ = false;
    // the targets for the frame's solid pass (after the z pre-pass), and for the next frame (after the opaque scene)
    dev_.addPass(d3d9::inject::Point::AfterDepthPrepass, "gbuffer.targets", [this](d3d9::PassContext& ctx) { ensureTargets(ctx); }, this);
    // the AO right after the z pre-pass (the solid pass reads it this frame), the motion fill after the opaque scene
    dev_.addPass(d3d9::inject::Point::AfterDepthPrepass, "gbuffer.ao", [this](d3d9::PassContext& ctx) { recordAo(ctx); }, this, 20);
    dev_.addPass(d3d9::inject::Point::AfterOpaque, "gbuffer.motion", [this](d3d9::PassContext& ctx) { recordMotionFill(ctx); }, this, 20);
    dev_.addPass(d3d9::inject::Point::BeforeGui, "gbuffer.debug", [this](d3d9::PassContext& ctx) { recordDebug(ctx); }, this);
    // characters' own motion (the vertex capture runs while the RT scene is active: GI, RT reflections)
    if (scene_ != nullptr) scene_->setDynamic(this, true);
}

void Gbuffer::disable() {
    if (!on_) return;
    on_ = false;
    dev_.removePasses(this);
    dev_.setGbuffer({});
    dev_.setFrameAo({});
    if (scene_ != nullptr) scene_->setDynamic(this, false);
    for (const char* k : {"gbuffer.normal", "gbuffer.motion", "gtao.raw", "gtao.history0", "gtao.history1"}) dev_.transients().release(k);
    historyValid_ = false;
}

// ---- the game's SSAO (ao.mode = replace) ----

bool Gbuffer::handles(d3d9::draws::Kind, d3d9::scene::Phase phase, const d3d9::draws::Tag&) const {
    return replacing_ && phase.view == d3d9::scene::View::Main && phase.pass == d3d9::scene::Pass::ShadowMask;
}

// post_SSM.fx's SSAO and its two blurs render into an A8R8G8B8 target smaller than the scene (shadows.md 2: 800 x 450 at
// 1600 x 900); the depth downscale before them is R32F, the mask itself has the scene's size. Those draws write 1
// (unoccluded): the upscale puts 1 into the mask's red channel.
void Gbuffer::replace(d3d9::Device& device, const d3d9::ReplaceQuery&, d3d9::DrawReplacement& out) {
    out.action = d3d9::DrawReplacement::Action::Engine;
    const d3d9::Surface* rt = device.renderTarget(0);
    const d3d9::Texture* hdr = device.imageWithRole(d3d9::scene::ImageRole::SceneHdr);
    if (rt == nullptr || rt->texture() == nullptr || hdr == nullptr) return;
    constexpr std::uint32_t kA8R8G8B8 = 21;
    if (rt->texture()->desc().format != kA8R8G8B8 || rt->width() >= hdr->desc().width) return;
    out.action = d3d9::DrawReplacement::Action::Procedural;
    out.vs = whiteVs_;
    out.ps = whitePs_;
    out.vertices = 3;
    out.cullNone = true;
    out.opaque = true;
    if (ssaoReplaced_++ == 0) {
        spdlog::info("enh: gbuffer: the game's SSAO draws replaced ({}x{} target): only the GTAO shades", rt->width(), rt->height());
    }
}

// the G-buffer images at the scene target's size (Device::setGbuffer every frame: the transients keep their images while the
// size holds)
bool Gbuffer::ensureTargets(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || ctx.sceneHdr == nullptr) return false;
    const std::uint32_t w = ctx.sceneHdr->desc().width, h = ctx.sceneHdr->desc().height;
    vk::Transients::Entry* n = dev_.transients().get("gbuffer.normal", {kNormalFormat, {w, h}, kNormalUsage});
    vk::Transients::Entry* m = dev_.transients().get("gbuffer.motion", {kMotionFormat, {w, h}, kMotionUsage});
    if (n == nullptr || m == nullptr) {
        dev_.setGbuffer({});
        return false;
    }
    dev_.setGbuffer({&n->image, &m->image, w, h});
    return true;
}

bool Gbuffer::pipelinesReady() {
    if (gtao_ && filter_) return true;
    if (failed_) return false;
    for (auto [future, out] : {std::pair{&gtaoBuild_, &gtao_}, std::pair{&filterBuild_, &filter_}}) {
        if (*out) continue;
        if (!future->valid() || future->wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        *out = future->get();
        if (!*out) {
            spdlog::error("enh: gbuffer: a compute pipeline failed: AO stays off this session");
            failed_ = true;
            return false;
        }
    }
    return true;
}

// the camera's motion in the G-buffer's motion image where no material wrote one (after the opaque scene)
void Gbuffer::recordMotionFill(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || !on_) return;
    ensureTargets(ctx);  // for the next frame's solid pass (the z pre-pass point may not fire)
    d3d9::Surface* ds = dev_.depthStencil();
    VkCommandBuffer cmd = ctx.cmd;
    if (!dev_.gbufferWritten() || ds == nullptr || ds->texture() == nullptr || ctx.sceneHdr == nullptr || !pipelinesReady() ||
        !dev_.readable(ds->texture(), cmd)) {
        return;
    }
    const std::uint32_t w = ctx.sceneHdr->desc().width, h = ctx.sceneHdr->desc().height;
    vk::Transients::Entry* normal = dev_.transients().get("gbuffer.normal", {kNormalFormat, {w, h}, kNormalUsage});
    vk::Transients::Entry* motion = dev_.transients().get("gbuffer.motion", {kMotionFormat, {w, h}, kMotionUsage});
    vk::Transients::Entry* raw = dev_.transients().get("gtao.raw", {VK_FORMAT_R32_SFLOAT, {w, h}});
    if (normal == nullptr || motion == nullptr || raw == nullptr) return;
    GbufferPush push{};
    push.h.frame = ctx.frameBlock;
    push.h.passId = 2;
    push.depthTex = ds->texture()->bindlessSlot();
    push.normalTex = normal->slot;
    push.width = std::min(w, ds->width());
    push.height = std::min(h, ds->height());
    push.flags = 2u;  // gtao.comp: the motion fill only
    RhiScope rs(dev_, cmd);
    if (!rs) return;
    rhi::CommandList& cl = rs.cl();
    rhi::Texture* normalT = rs.wrap(*normal);
    rhi::Texture* motionT = rs.wrap(*motion);
    rhi::Texture* rawT = rs.wrap(*raw);
    cl.transition(*normalT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.transition(*motionT, VK_IMAGE_LAYOUT_GENERAL);
    cl.transition(*rawT, VK_IMAGE_LAYOUT_GENERAL);
    cl.bindPipeline(*gtao_);
    rhi::Texture* const storage[] = {rawT, motionT};
    cl.setStorageTextures(storage);
    cl.setPushConstants(&push, sizeof(push));
    cl.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);
    cl.transition(*motionT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// the AO of this frame from the depth after the z pre-pass (the material shaders of the solid pass read it at their pixel)
void Gbuffer::recordAo(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || !on_) return;
    ensureTargets(ctx);
    if (!s_.ao) return;
    d3d9::Surface* ds = dev_.depthStencil();
    VkCommandBuffer cmd = ctx.cmd;
    if (ds == nullptr || ds->texture() == nullptr || ctx.sceneHdr == nullptr || !pipelinesReady() ||
        !dev_.readable(ds->texture(), cmd)) {
        ++skipped_;
        dev_.setFrameAo({});
        historyValid_ = false;
        return;
    }
    const std::uint32_t w = ctx.sceneHdr->desc().width, h = ctx.sceneHdr->desc().height;
    vk::Transients::Entry* normal = dev_.transients().get("gbuffer.normal", {kNormalFormat, {w, h}, kNormalUsage});
    vk::Transients::Entry* motion = dev_.transients().get("gbuffer.motion", {kMotionFormat, {w, h}, kMotionUsage});
    vk::Transients::Entry* raw = dev_.transients().get("gtao.raw", {VK_FORMAT_R32_SFLOAT, {w, h}});
    const std::uint32_t cur = history_ ^ 1u;
    vk::Transients::Entry* hist[2] = {dev_.transients().get("gtao.history0", {VK_FORMAT_R16G16_SFLOAT, {w, h}}),
                                      dev_.transients().get("gtao.history1", {VK_FORMAT_R16G16_SFLOAT, {w, h}})};
    if (normal == nullptr || motion == nullptr || raw == nullptr || hist[0] == nullptr || hist[1] == nullptr) {
        ++skipped_;
        return;
    }
    GbufferPush push{};
    push.h.frame = ctx.frameBlock;
    push.h.passId = 1;
    push.depthTex = ds->texture()->bindlessSlot();
    push.normalTex = normal->slot;
    push.motionTex = motion->slot;
    push.linearSampler = dev_.samplerFor(true, true);
    push.width = std::min(w, ds->width());
    push.height = std::min(h, ds->height());
    push.radius = s_.aoRadius;
    push.falloff = 0.6f;
    push.strength = s_.aoStrength;
    push.blend = 0.85f;
    push.frameIndex = static_cast<std::uint32_t>(dev_.frameValue());
    push.flags = 1u;  // gtao.comp: the AO

    RhiScope rs(dev_, cmd);
    if (!rs) return;
    rhi::CommandList& cl = rs.cl();
    rhi::Texture* motionT = rs.wrap(*motion);
    rhi::Texture* rawT = rs.wrap(*raw);
    rhi::Texture* histRead = rs.wrap(*hist[history_]);
    rhi::Texture* histWrite = rs.wrap(*hist[cur]);

    // the raw AO (the motion image is bound but not written in this mode)
    cl.transition(*motionT, VK_IMAGE_LAYOUT_GENERAL);
    cl.transition(*rawT, VK_IMAGE_LAYOUT_GENERAL);
    cl.bindPipeline(*gtao_);
    rhi::Texture* const storage1[] = {rawT, motionT};
    cl.setStorageTextures(storage1);
    cl.setPushConstants(&push, sizeof(push));
    cl.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);

    // filter + temporal into this frame's history
    cl.transition(*rawT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.transition(*histRead, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.transition(*histWrite, VK_IMAGE_LAYOUT_GENERAL);
    push.aoTex = raw->slot;
    push.historyTex = historyValid_ ? hist[history_]->slot : 0u;
    cl.bindPipeline(*filter_);
    rhi::Texture* const storage2[] = {histWrite};
    cl.setStorageTextures(storage2);
    cl.setPushConstants(&push, sizeof(push));
    cl.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);
    cl.transition(*histWrite, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    history_ = cur;
    historyValid_ = true;
    // -> this frame's material shaders (Gbuffer variants: the AO at their pixel scales their ambient)
    dev_.setFrameAo({hist[cur]->slot, dev_.samplerFor(true, true), shader::abi::kFrameAoValid, std::bit_cast<std::uint32_t>(s_.aoStrength)});
    if (aoFrames_++ == 0) {
        spdlog::info("enh: gbuffer: first AO frame {}x{} (radius {:.0f}, strength {:.2f})", push.width, push.height, s_.aoRadius, s_.aoStrength);
    }
    const double now = dev_.secondsSinceCreate();
    if (now - lastLog_ >= 5.0) {
        lastLog_ = now;
        spdlog::info("enh: gbuffer: {} AO frames ({} skipped: no G-buffer this frame / inputs / pipelines), g-buffer {}", aoFrames_, skipped_,
                     dev_.gbufferArmed() ? "armed" : "building pipelines");
    }
}

void Gbuffer::recordDebug(d3d9::PassContext& ctx) {
    if (s_.debug == 0 || !dev_.gbufferWritten()) return;
    d3d9::Surface* bb = dev_.backBuffer();
    if (bb == nullptr || bb->texture() == nullptr) return;
    vk::Image& target = bb->texture()->image();
    RhiScope rs(dev_, ctx.cmd);
    if (!rs) return;
    if (debug_ && debugFormat_ != target.format()) debug_.reset();
    if (!debug_) {
        rhi::GraphicsPipelineDesc gd;
        gd.vs.spirv = shader::embedded::spirv("debug/fullscreen_rhi.vert");
        gd.ps.spirv = shader::embedded::spirv("gbuffer/gbuffer_debug.frag");
        gd.colorFormats[0] = target.format();
        gd.name = "gbuffer.debug";
        debug_ = rs.device().createGraphicsPipeline(gd);
        debugFormat_ = target.format();
        if (!debug_) return;
    }
    vk::Transients::Entry* normal = nullptr;
    vk::Transients::Entry* motion = nullptr;
    if (ctx.sceneHdr != nullptr) {
        const std::uint32_t w = ctx.sceneHdr->desc().width, h = ctx.sceneHdr->desc().height;
        normal = dev_.transients().get("gbuffer.normal", {kNormalFormat, {w, h}, kNormalUsage});
        motion = dev_.transients().get("gbuffer.motion", {kMotionFormat, {w, h}, kMotionUsage});
    }
    if (normal == nullptr || motion == nullptr) return;
    rhi::CommandList& cl = rs.cl();
    cl.transition(*rs.wrap(*normal), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.transition(*rs.wrap(*motion), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    GbufferPush push{};
    push.h.frame = ctx.frameBlock;
    push.h.passId = 3;
    push.normalTex = normal->slot;
    push.motionTex = motion->slot;
    push.linearSampler = dev_.samplerFor(false, true);
    push.mode = static_cast<std::uint32_t>(s_.debug);
    if (s_.debug == 4) push.aoTex = dev_.frameSgi()[0];  // the screen probes' factor (enh/gi)
    if (s_.debug != 4 && s_.ao && historyValid_) {
        vk::Transients::Entry* hst = dev_.transients().get(history_ == 0 ? "gtao.history0" : "gtao.history1",
                                                           {VK_FORMAT_R16G16_SFLOAT, normal->desc.extent});
        if (hst != nullptr) push.aoTex = hst->slot;
    }
    const std::uint32_t w = bb->width(), h = bb->height();
    rhi::Texture* targetT = rs.wrap(target);
    cl.transition(*targetT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    rhi::RenderingInfo ri;
    ri.color[0].texture = targetT;
    ri.color[0].load = VK_ATTACHMENT_LOAD_OP_LOAD;
    ri.color[0].store = VK_ATTACHMENT_STORE_OP_STORE;
    ri.colorCount = 1;
    ri.width = w;
    ri.height = h;
    cl.beginRendering(ri);
    cl.bindPipeline(*debug_);
    cl.setPushConstants(&push, sizeof(push));
    cl.draw(3, 1, 0, 0);
    cl.endRendering();
    if (debugFrames_++ == 0) spdlog::info("enh: gbuffer: debug view {} ({}x{})", s_.debug, w, h);
}

}  // namespace rvk::enh
