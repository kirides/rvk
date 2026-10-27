#include "enh/native_opaque.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "d3d9/convert.h"
#include "d3d9/defs.h"
#include "engine/materials.h"
#include "enh/native_parts.h"
#include "enh/native_shadows.h"
#include "shader/embedded.h"
#include "vk/image.h"

namespace rvk::enh {

namespace {

namespace mats = engine::materials;

constexpr std::uint32_t kFlagNoAlbedo = 4, kFlagLightmap = 8, kFlagNormalMap = 32, kFlagDxt5nm = 64;

}  // namespace

NativeOpaque::NativeOpaque(d3d9::Device& device) : dev_(device), arena_(device) {}

NativeOpaque::~NativeOpaque() {
    apply(false);
    destroyPipelines();
}

void NativeOpaque::destroyPipelines() {
    vk::Context* ctx = &dev_.ctx();
    VkPipeline p[static_cast<std::size_t>(Pipe::Count)];
    bool any = false;
    for (std::size_t i = 0; i < static_cast<std::size_t>(Pipe::Count); ++i) {
        p[i] = pipes_[i];
        pipes_[i] = VK_NULL_HANDLE;
        any = any || p[i] != VK_NULL_HANDLE;
    }
    vk::PassLayout layout = layout_;
    if (any || layout.valid()) {
        dev_.frames().retire([ctx, p, layout]() mutable {
            for (VkPipeline x : p) {
                if (x != VK_NULL_HANDLE) vkDestroyPipeline(ctx->device(), x, nullptr);
            }
            layout.destroy(*ctx);
        });
    }
    layout_ = vk::PassLayout{};
    depthMapFormat_ = colorFormat_ = depthFormat_ = VK_FORMAT_UNDEFINED;
}

void NativeOpaque::setDebug(std::uint32_t mode) { debug_ = mode; }

void NativeOpaque::apply(bool enabled) {
    enabled_ = enabled && mats::available();
    if (enabled_ && !registered_) {
        dev_.addDrawReplacer(this);
        prepassId_ = dev_.addPass(d3d9::inject::Point::BeforeDepthPrepass, "native.opaque.prepass", [this](d3d9::PassContext& c) { prepass(c); }, this, 6);
        colorPassId_ = dev_.addPass(d3d9::inject::Point::BeforeSolid, "native.opaque", [this](d3d9::PassContext& c) { colorPass(c); }, this, 10);
        registered_ = true;
        failed_ = false;
        spdlog::info("enh: native opaque: on (scene mirror list, depth pre-pass + colour pass, one indirect call per class: shaders/native/static_*)");
    } else if (!enabled_ && registered_) {
        dev_.removeDrawReplacer(this);
        dev_.removePass(prepassId_);
        dev_.removePass(colorPassId_);
        prepassId_ = colorPassId_ = 0;
        registered_ = false;
        items_.clear();
        listIndex_.clear();
        listSeen_.clear();
        list_ = FrameList{};
        spdlog::info("enh: native opaque: off");
    }
}

bool NativeOpaque::handles(d3d9::draws::Kind kind, d3d9::scene::Phase phase, const d3d9::draws::Tag& tag) const {
    return enabled_ && !failed_ && !listIndex_.empty() && kind == d3d9::draws::Kind::StaticMesh && phase.view == d3d9::scene::View::Main &&
           (phase.pass == d3d9::scene::Pass::Solid || phase.pass == d3d9::scene::Pass::DepthPrepass) && tag.entity != nullptr;
}

void NativeOpaque::replace(d3d9::Device& device, const d3d9::ReplaceQuery& q, d3d9::DrawReplacement& out) {
    out.action = d3d9::DrawReplacement::Action::Engine;
    const bool solid = q.phase.pass == d3d9::scene::Pass::Solid;
    // the engine's per-view constants of its solid draws (the fog, the shadow mask: the colour pass takes them)
    if (solid && q.tag != nullptr && q.tag->material != nullptr && q.enginePs != nullptr) {
        std::memcpy(view_.fog[0], device.psConstant(3), 16);
        std::memcpy(view_.fog[1], device.psConstant(4), 16);
        if (q.textures != nullptr && q.textures[0] != nullptr && !q.textures[0]->isNull() && q.textures[0]->kind() == vk::TextureKind::Tex2D) {
            view_.mask = q.textures[0];
            view_.maskSampler = device.samplerOfSlot(0);
        }
        view_.frame = device.frameValue();
    }
    if (q.tag != nullptr) {
        const VkCullModeFlags cm = d3d9::cullMode(device.renderState(d3d9::RS_CULLMODE));
        const bool listedPart = q.indexed && q.indexBuffer != nullptr && listIndex_.contains(partKey(q.tag->entity, q.indexBuffer->id(), q.first, q.primitives * 3));
        ++stats_.cullHist[solid ? 0 : 1][listedPart ? 1 : 0][cm == VK_CULL_MODE_NONE ? 0 : cm == VK_CULL_MODE_FRONT_BIT ? 1 : cm == VK_CULL_MODE_BACK_BIT ? 2 : 3];
        const std::uint32_t zf = std::min<std::uint32_t>(device.renderState(d3d9::RS_ZFUNC), 8u);
        ++stats_.zfunc[solid ? 0 : 1][zf];
        stats_.zwrite[solid ? 0 : 1][q.depthWrite ? 1 : 0]++;
    }
    if (q.depthBias && q.tag != nullptr) {
        static int biasLogged = 0;
        if (biasLogged < 40) {
            ++biasLogged;
            float bias = 0.0f, slope = 0.0f;
            const std::uint32_t rb = device.renderState(d3d9::RS_DEPTHBIAS), rs = device.renderState(d3d9::RS_SLOPESCALEDEPTHBIAS);
            std::memcpy(&bias, &rb, 4);
            std::memcpy(&slope, &rs, 4);
            spdlog::info("enh: native opaque: the engine's {} draw with a depth bias: material '{}', bias {}, slope {}, listed {}", solid ? "solid" : "z pass",
                         mats::resourceName(q.tag->material), bias, slope, q.indexBuffer != nullptr && listIndex_.contains(partKey(q.tag->entity, q.indexBuffer->id(), q.first, q.primitives * 3)));
        }
    }
    // a listed part: the engine's draw is replaced by ours (in the solid pass compared with what was composed)
    if (q.tag != nullptr && q.indexed && q.indexBuffer != nullptr && q.primitiveType == 4) {
        if (const auto it = listIndex_.find(partKey(q.tag->entity, q.indexBuffer->id(), q.first, q.primitives * 3)); it != listIndex_.end()) {
            if (solid) {
                compareLighting(items_[it->second], device);
                listSeen_[it->second] = 1;
            }
            out.action = d3d9::DrawReplacement::Action::Skip;
            ++stats_.taken;
            return;
        }
    }
    if (solid) ++stats_.listEngineDrew;
}

bool NativeOpaque::ensurePipelines(VkFormat depthMap, VkFormat color, VkFormat depth) {
    if (failed_) return false;
    vk::Context& ctx = dev_.ctx();
    if (layout_.valid() && (depthMapFormat_ != depthMap || colorFormat_ != color || depthFormat_ != depth)) destroyPipelines();
    if (!layout_.valid() && !layout_.create(ctx, dev_.bindless(), {})) {
        failed_ = true;
        spdlog::error("enh: native opaque: pass layout failed, off for this session");
        return false;
    }
    if (pipes_[0] == VK_NULL_HANDLE) {
        // the main depth buffer is reversed (debug.reverse_z; the setting is read when the pipelines are built)
        const bool rev = dev_.reverseZ();
        vk::PassGraphicsDesc gd;
        gd.depth = depth;
        gd.stencil = vk::hasStencil(depth) ? depth : VK_FORMAT_UNDEFINED;
        gd.depthTest = true;
        gd.frontFace = VK_FRONT_FACE_CLOCKWISE;  // as the engine's draws (d3d9/convert.h kFrontFace with the flipped viewport)
        gd.dynamicCullMode = true;
        const auto make = [&](Pipe slot, const char* vs, const char* fs, VkFormat c, bool write, VkCompareOp op, VkCullModeFlags cull, const char* name) {
            gd.vs = shader::embedded::spirv(vs);
            gd.fs = shader::embedded::spirv(fs);
            gd.color = c;
            gd.depthWrite = write;
            gd.depthOp = op;
            gd.cull = cull;
            gd.name = name;
            pipes_[static_cast<std::size_t>(slot)] = vk::createPassGraphicsPipeline(ctx, layout_.layout(), gd);
        };
        const VkCompareOp nearer = rev ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
        // depth pre-pass: depth + the linear depth map; the opaque class has no discard, the alpha tested one clips (two-sided: leaf cards, fences)
        make(Pipe::PrepassOpaque, "native/static_prepass.vert", "native/static_prepass.frag", depthMap, true, nearer, VK_CULL_MODE_BACK_BIT, "native.prepass opaque");
        make(Pipe::PrepassMasked, "native/static_prepass_masked.vert", "native/static_prepass_masked.frag", depthMap, true, nearer, VK_CULL_MODE_NONE, "native.prepass masked");
        // colour: the pre-pass wrote its depth a hair farther (static_prepass.vert), so nearer-or-equal passes exactly the frontmost surface (and
        // anything within ~1e-5 of it); no depth write. The opaque class's shader has no clip at all; the alpha tested class keeps its alpha test (the texels the pre-pass cut would otherwise be drawn: black
        // outside a net's wires). (A depth EQUAL test needed bit-identical positions from two programs.)
        // (native.debug 9 'albedo_depth': a plain nearer-or-equal test instead, to tell a depth mismatch from missing geometry)
        const VkCompareOp colorOp = nearer;
        make(Pipe::ColorOpaque, "native/static_mesh.vert", "native/static_mesh_opaque.frag", color, false, colorOp, VK_CULL_MODE_BACK_BIT, "native.color opaque");
        make(Pipe::ColorMasked, "native/static_mesh.vert", "native/static_mesh.frag", color, false, colorOp, VK_CULL_MODE_NONE, "native.color masked");
        depthMapFormat_ = depthMap;
        colorFormat_ = color;
        depthFormat_ = depth;
    }
    for (VkPipeline p : pipes_) {
        if (p == VK_NULL_HANDLE) {
            failed_ = true;
            spdlog::error("enh: native opaque: pipelines failed, off for this session");
            return false;
        }
    }
    return true;
}

void NativeOpaque::drawClass(VkCommandBuffer cmd, std::uint32_t first, std::uint32_t count, VkPipeline pipeline, VkCullModeFlags cull, const NativeStaticPush& push) {
    if (count == 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    // the push constants after every pipeline bind: the classes' shaders read different parts of them, and a bind must never leave a stage with values it did not get
    layout_.push(cmd, &push, sizeof(push));
    vkCmdSetCullMode(cmd, cull);
    if (dev_.ctx().features().indirectDraws) {
        // ONE call for the whole class: command i = (index count, 1 instance, arena first index, 0, item i)
        vkCmdDrawIndexedIndirect(cmd, list_.commandBuffer, list_.commandOffset + static_cast<VkDeviceSize>(first) * sizeof(VkDrawIndexedIndirectCommand), count,
                                 sizeof(VkDrawIndexedIndirectCommand));
        ++stats_.indirectCalls;
    } else {
        for (std::uint32_t i = first; i < first + count; ++i) vkCmdDrawIndexed(cmd, items_[i].indexCount, 1, items_[i].arenaFirst, 0, i);
    }
}

namespace {

// Dynamic rendering has no implicit dependencies between render scopes: the native passes write the depth buffer (and the depth map) that the
// engine's scopes before and after test / write / sample, so each pass is bracketed by a full memory barrier (attachment writes -> everything)
void fullBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// makes the textures readable and writes their slots into the item blocks (barriers / uploads are not allowed inside a rendering scope);
// one that is not readable drops its feature
struct Readiness {
    d3d9::Device& dev;
    VkCommandBuffer cmd;
    std::unordered_map<d3d9::Texture*, bool> ready;
    void prepare(d3d9::Texture*& t) {
        if (t == nullptr) return;
        auto [entry, fresh] = ready.try_emplace(t, false);
        if (fresh) entry->second = dev.readable(t, cmd) && t->image().layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (!entry->second) t = nullptr;
    }
};

}  // namespace

void NativeOpaque::prepass(d3d9::PassContext& ctx) {
    list_ = FrameList{};
    items_.clear();
    listIndex_.clear();
    listSeen_.clear();
    if (ctx.mainView != 1 || parts_ == nullptr || failed_ || !enabled_) return;
    d3d9::Surface* ds = dev_.depthStencil();
    d3d9::Texture* depthMap = ctx.depthMap;
    d3d9::Texture* scene = ctx.sceneHdr;
    if (ds == nullptr || ds->texture() == nullptr || depthMap == nullptr || scene == nullptr || ctx.target != depthMap) return;
    if (!ensurePipelines(depthMap->image().format(), scene->image().format(), ds->texture()->image().format())) return;
    arena_.beginFrame();
    if (!arena_.ok()) return;
    {
        static int logged = 0;
        if (logged++ % 600 == 0) {
            const d3d9::scene::CameraBlock& cam = dev_.camera();
            spdlog::info("enh: native opaque: main depth buffer reversed: {} (setting {}), depth map {}x{}, hardware depth {}x{}; camera near {} far {} engine far {} valid {}, k {}", dev_.boundDepthReversed() ? "yes" : "NO",
                         dev_.reverseZ() ? "on" : "off", depthMap->desc().width, depthMap->desc().height, ds->width(), ds->height(), cam.nearZ, cam.farZ, cam.engineFar, cam.valid, dev_.reverseDepthK());
        }
    }
    buildList(ctx);
    const std::uint32_t n = static_cast<std::uint32_t>(items_.size());
    if (n == 0) return;
    VkCommandBuffer cmd = ctx.cmd;

    // textures readable, their slots into the blocks
    Readiness r{dev_, cmd, {}};
    for (Item& it : items_) {
        r.prepare(it.diffuse);
        r.prepare(it.normal);
        r.prepare(it.lightmap[0]);
        r.prepare(it.lightmap[1]);
        NativeItemBlock& blk = it.block;
        std::uint32_t& flags = blk.tex2[1];
        if (it.diffuse == nullptr) flags |= kFlagNoAlbedo;
        if (it.normal == nullptr) flags &= ~(kFlagNormalMap | kFlagDxt5nm);
        if (it.lightmap[0] == nullptr || it.lightmap[1] == nullptr) flags &= ~kFlagLightmap;
        blk.tex0[0] = it.diffuse != nullptr ? it.diffuse->bindlessSlot() : 0;
        blk.tex0[1] = it.diffuseSampler;
        blk.tex0[2] = it.normal != nullptr ? it.normal->bindlessSlot() : 0;
        blk.tex0[3] = it.normalSampler;
        blk.tex1[2] = it.lightmap[0] != nullptr ? it.lightmap[0]->bindlessSlot() : 0;
        blk.tex1[3] = it.lightmap[1] != nullptr ? it.lightmap[1]->bindlessSlot() : 0;
        blk.tex2[0] = it.lightmapSampler;
    }
    arena_.flush(cmd);

    // the frame's ring: the item blocks and one indexed indirect command per item (firstInstance = the item's index)
    const vk::RingAlloc itemRing = dev_.frames().allocate(static_cast<VkDeviceSize>(n) * sizeof(NativeItemBlock), 16);
    const vk::RingAlloc cmdRing = dev_.frames().allocate(static_cast<VkDeviceSize>(n) * sizeof(VkDrawIndexedIndirectCommand), 16);
    if (itemRing.ptr == nullptr || cmdRing.ptr == nullptr) {
        items_.clear();
        listIndex_.clear();
        listSeen_.clear();
        return;
    }
    auto* blocks = static_cast<NativeItemBlock*>(itemRing.ptr);
    auto* cmds = static_cast<VkDrawIndexedIndirectCommand*>(cmdRing.ptr);
    std::uint32_t opaque = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        blocks[i] = items_[i].block;
        cmds[i] = VkDrawIndexedIndirectCommand{items_[i].indexCount, 1, items_[i].arenaFirst, 0, i};
        if (!items_[i].masked) ++opaque;
    }
    list_.frame = dev_.frameValue();
    list_.opaque = opaque;
    list_.masked = n - opaque;
    list_.commandBuffer = cmdRing.buffer;
    list_.commandOffset = cmdRing.offset;
    list_.items = itemRing.address;
    list_.itemMemory = blocks;

    // the depth pre-pass: the hardware depth + the linear depth map
    const std::uint32_t w = std::min(depthMap->desc().width, ds->width()), h = std::min(depthMap->desc().height, ds->height());
    depthMap->image().transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ds->texture()->image().transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = depthMap->image().attachmentView(dev_.ctx(), 0, 0);
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = ds->texture()->image().attachmentView(dev_.ctx(), ds->level(), ds->face());
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    if (vk::hasStencil(ds->texture()->image().format())) ri.pStencilAttachment = &depth;
    fullBarrier(cmd);
    dev_.regionBegin("native.prepass");
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0.0f, static_cast<float>(h), static_cast<float>(w), -static_cast<float>(h), 0.0f, 1.0f};  // the engine's flipped viewport
    const VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    layout_.bindSet0(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dev_.bindless());
    vkCmdBindIndexBuffer(cmd, arena_.buffer(), 0, VK_INDEX_TYPE_UINT32);
    NativeStaticPush push{};
    push.frame = ctx.frameBlock;
    push.passFlags = debug_;
    push.items[0] = static_cast<std::uint32_t>(list_.items);
    push.items[1] = static_cast<std::uint32_t>(list_.items >> 32);
    drawClass(cmd, 0, list_.opaque, pipes_[static_cast<std::size_t>(Pipe::PrepassOpaque)], VK_CULL_MODE_FRONT_BIT, push);
    drawClass(cmd, list_.opaque, list_.masked, pipes_[static_cast<std::size_t>(Pipe::PrepassMasked)], VK_CULL_MODE_NONE, push);
    vkCmdEndRendering(cmd);
    fullBarrier(cmd);
    dev_.regionEnd();
    list_.drawn = true;
    stats_.drawnOpaque += list_.opaque;
    stats_.drawnMasked += list_.masked;
}

void NativeOpaque::colorPass(d3d9::PassContext& ctx) {
    if (!list_.drawn || list_.frame != dev_.frameValue() || items_.empty()) {
        list_ = FrameList{};
        listIndex_.clear();  // nothing of ours was drawn: the engine's solid draws are not skipped
        return;
    }
    for (const std::uint8_t seen : listSeen_) stats_.listUnseen += seen == 0 ? 1u : 0u;
    const std::uint32_t n = static_cast<std::uint32_t>(items_.size());
    d3d9::Surface* ds = dev_.depthStencil();
    d3d9::Texture* scene = ctx.sceneHdr;
    if (ds == nullptr || ds->texture() == nullptr || scene == nullptr || ctx.target != scene ||
        !ensurePipelines(depthMapFormat_, scene->image().format(), ds->texture()->image().format())) {
        list_ = FrameList{};
        listIndex_.clear();
        return;
    }
    VkCommandBuffer cmd = ctx.cmd;
    // the per-view constants of this frame's engine draws (fog, the shadow mask), patched into the ring memory of the items
    Readiness r{dev_, cmd, {}};
    // (this pass runs before the frame's own solid draws: the fog is the last frame's, the mask texture is the same object every frame and holds this frame's mask)
    d3d9::Texture* mask = view_.mask;
    r.prepare(mask);
    const bool fresh = view_.frame != 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        NativeItemBlock& blk = list_.itemMemory[i];
        if (fresh) {
            std::memcpy(blk.ps[3], view_.fog[0], 16);
            std::memcpy(blk.ps[4], view_.fog[1], 16);
        }
        blk.tex1[0] = mask != nullptr ? mask->bindlessSlot() : 0;
        blk.tex1[1] = mask != nullptr ? view_.maskSampler : 0;
    }
    const std::uint32_t w = std::min(scene->desc().width, ds->width()), h = std::min(scene->desc().height, ds->height());
    scene->image().transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ds->texture()->image().transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = scene->image().attachmentView(dev_.ctx(), 0, 0);
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = ds->texture()->image().attachmentView(dev_.ctx(), ds->level(), ds->face());
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    if (vk::hasStencil(ds->texture()->image().format())) ri.pStencilAttachment = &depth;
    fullBarrier(cmd);
    dev_.regionBegin("native.opaque");
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0.0f, static_cast<float>(h), static_cast<float>(w), -static_cast<float>(h), 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    layout_.bindSet0(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dev_.bindless());
    vkCmdBindIndexBuffer(cmd, arena_.buffer(), 0, VK_INDEX_TYPE_UINT32);
    const VkDeviceAddress shadowBlock = shadows_ != nullptr ? shadows_->block() : 0;
    NativeStaticPush push{};
    push.frame = ctx.frameBlock;
    push.passId = 3;
    push.passFlags = debug_;
    push.items[0] = static_cast<std::uint32_t>(list_.items);
    push.items[1] = static_cast<std::uint32_t>(list_.items >> 32);
    push.shadow[0] = static_cast<std::uint32_t>(shadowBlock);
    push.shadow[1] = static_cast<std::uint32_t>(shadowBlock >> 32);
    drawClass(cmd, 0, list_.opaque, pipes_[static_cast<std::size_t>(Pipe::ColorOpaque)], VK_CULL_MODE_FRONT_BIT, push);
    drawClass(cmd, list_.opaque, list_.masked, pipes_[static_cast<std::size_t>(Pipe::ColorMasked)], VK_CULL_MODE_NONE, push);
    vkCmdEndRendering(cmd);
    fullBarrier(cmd);
    dev_.regionEnd();
    ++stats_.passes;
    list_ = FrameList{};
    // listIndex_ stays until the next frame's list: the engine's solid draws of these parts come AFTER this pass and are skipped
    const double now = dev_.secondsSinceCreate();
    if (now - lastReport_ > 5.0) {
        lastReport_ = now;
        report();
    }
}

void NativeOpaque::report() {
    spdlog::info("enh: native opaque: {} engine draws replaced, {} passes, {} opaque + {} alpha tested parts drawn ({} indirect calls); the engine drew {} static parts that "
                 "are not in the list",
                 stats_.taken, stats_.passes, stats_.drawnOpaque, stats_.drawnMasked, stats_.indirectCalls, stats_.listEngineDrew);
    spdlog::info("enh: native opaque: cull modes of the engine's static draws [none front back both]: solid unlisted [{} {} {} {}] listed [{} {} {} {}], z pass unlisted [{} {} {} {}] listed [{} {} {} {}]",
                 stats_.cullHist[0][0][0], stats_.cullHist[0][0][1], stats_.cullHist[0][0][2], stats_.cullHist[0][0][3], stats_.cullHist[0][1][0], stats_.cullHist[0][1][1], stats_.cullHist[0][1][2], stats_.cullHist[0][1][3],
                 stats_.cullHist[1][0][0], stats_.cullHist[1][0][1], stats_.cullHist[1][0][2], stats_.cullHist[1][0][3], stats_.cullHist[1][1][0], stats_.cullHist[1][1][1], stats_.cullHist[1][1][2], stats_.cullHist[1][1][3]);
    spdlog::info("enh: native opaque: the engine's static draws, z func counts [1 never .. 8 always]: solid [{} {} {} {} {} {} {} {}], z pass [{} {} {} {} {} {} {} {}]; depth write solid off/on {} / {}, z pass off/on {} / {}",
                 stats_.zfunc[0][1], stats_.zfunc[0][2], stats_.zfunc[0][3], stats_.zfunc[0][4], stats_.zfunc[0][5], stats_.zfunc[0][6], stats_.zfunc[0][7], stats_.zfunc[0][8],
                 stats_.zfunc[1][1], stats_.zfunc[1][2], stats_.zfunc[1][3], stats_.zfunc[1][4], stats_.zfunc[1][5], stats_.zfunc[1][6], stats_.zfunc[1][7], stats_.zfunc[1][8],
                 stats_.zwrite[0][0], stats_.zwrite[0][1], stats_.zwrite[1][0], stats_.zwrite[1][1]);
    if (stats_.listFrames != 0) {
        const double n = static_cast<double>(stats_.listFrames);
        const NativeIndexArena::Stats& as = arena_.stats();
        spdlog::info("enh: native opaque: mirror list {} frames: per frame {:.0f} entities seen, {:.0f} parts listed, {} fading entities left to the engine, {} without "
                     "lighting data, {} parts without indices, {} meshes whose resource changed under the mirror, {} parts with an image still loading; {} listed parts the engine did not draw; index arena {} parts / {} indices ({} resets); lighting vs the "
                     "engine ({} draws): differs in camera {} / fog {} / indoor {} / SH {} / sun {} / lights {}",
                     stats_.listFrames, static_cast<double>(stats_.listEntities) / n, static_cast<double>(stats_.listParts) / n, stats_.listFading, stats_.listNoLight,
                     stats_.listNoIndices, stats_.listStale, stats_.listPending, stats_.listUnseen, as.parts, as.indices, as.resets, stats_.cmpDraws, stats_.cmpMismatch[0], stats_.cmpMismatch[1],
                     stats_.cmpMismatch[2], stats_.cmpMismatch[3], stats_.cmpMismatch[4], stats_.cmpMismatch[5]);
    }
}

}  // namespace rvk::enh
