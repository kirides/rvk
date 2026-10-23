// d3d9::Device draws: rendering scopes, texture preparation, pipeline state, vertex input, dynamic state, constants.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "d3d9/convert.h"
#include "d3d9/device.h"
#include "shader/abi.h"

namespace rvk::d3d9 {

// [debug] perf: CPU time inside a draw call
struct Device::DrawTimer {
    Device& d;
    std::chrono::steady_clock::time_point t0;
    explicit DrawTimer(Device& device) : d(device), t0(device.options_.perf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~DrawTimer() {
        if (d.options_.perf) {
            d.perf_.drawMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
    }
};

void Device::endRendering() {
    if (rendering_) {
        flushBatch();  // the batched draws belong to this rendering scope
        vkCmdEndRendering(frames_.cmd());
        rendering_ = false;
        if (gpuTimer_.enabled()) {
            const std::uint32_t slot = frames_.currentSlot();
            std::string label;
            if (gpuTimer_.details(slot)) {
                label = std::format("pass {}: {}x{}, {} colour ({}), depth {}, {} draws", passIndex_, targetWidth_, targetHeight_,
                                    colorCount_, colorCount_ > 0 ? static_cast<int>(colorFormats_[0]) : 0,
                                    static_cast<int>(depthFormat_), passDraws_);
            }
            gpuTimer_.mark(frames_.cmd(), slot, vk::GpuTimer::Stream::Main, std::move(label));
        }
    }
}

bool Device::beginRendering() {
    if (rendering_) {
        return true;
    }
    VkCommandBuffer cmd = frames_.cmd();
    VkRenderingAttachmentInfo colors[kRenderTargets]{};
    colorCount_ = 0;
    targetWidth_ = UINT32_MAX;
    targetHeight_ = UINT32_MAX;
    for (std::uint32_t i = 0; i < kRenderTargets; ++i) {
        Surface* s = rt_[i];
        if (s == nullptr || s->texture()->isNull() || s->texture()->isDepth()) {
            continue;
        }
        Texture* t = s->texture();
        t->image().transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        t->lastUseFrame_ = frameValue();
        VkRenderingAttachmentInfo& a = colors[colorCount_];
        a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        a.imageView = t->image().attachmentView(ctx_, s->level(), s->face());
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorFormats_[colorCount_] = t->image().format();
        ++colorCount_;
        targetWidth_ = std::min(targetWidth_, s->width());
        targetHeight_ = std::min(targetHeight_, s->height());
    }
    // the thin G-buffer (enh/gbuffer): normal + motion after the engine's target, cleared at the frame's first use
    engineColorCount_ = colorCount_;
    scopeGbuf_ = false;
    if (colorCount_ == 1 && gbufferScope()) {
        const bool first = gbufWrittenFrame_ != frameValue();
        for (vk::Image* img : {gbuf_.normal, gbuf_.motion}) {
            img->transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo& a = colors[colorCount_];
            a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            a.imageView = img->view();
            a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            a.loadOp = first ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
            colorFormats_[colorCount_] = img->format();
            ++colorCount_;
        }
        gbufWrittenFrame_ = frameValue();
        scopeGbuf_ = true;
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthFormat_ = VK_FORMAT_UNDEFINED;
    stencilFormat_ = VK_FORMAT_UNDEFINED;
    if (ds_ != nullptr && !ds_->texture()->isNull() && ds_->texture()->isDepth()) {
        Texture* t = ds_->texture();
        t->image().transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        t->lastUseFrame_ = frameValue();
        depth.imageView = t->image().attachmentView(ctx_, ds_->level(), ds_->face());
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthFormat_ = t->image().format();
        if (vk::hasStencil(depthFormat_)) {
            stencil = depth;
            stencilFormat_ = depthFormat_;
        }
        // D3D9 lets the depth buffer be larger than the render target
        targetWidth_ = colorCount_ > 0 ? targetWidth_ : ds_->width();
        targetHeight_ = colorCount_ > 0 ? targetHeight_ : ds_->height();
    }
    if (colorCount_ == 0 && depthFormat_ == VK_FORMAT_UNDEFINED) {
        // a NULL colour target without a depth buffer: nothing to render into
        targetWidth_ = targetHeight_ = 0;
        return false;
    }
    if (gpuTimer_.enabled()) {
        const std::uint32_t slot = frames_.currentSlot();
        gpuTimer_.mark(cmd, slot, vk::GpuTimer::Stream::Main,
                       gpuTimer_.details(slot) ? std::format("barriers / copies / inline uploads before pass {}", passIndex_ + 1)
                                               : std::string());
    }
    ++passIndex_;
    passDraws_ = 0;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {targetWidth_, targetHeight_}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = colorCount_;
    ri.pColorAttachments = colors;
    ri.pDepthAttachment = depthFormat_ != VK_FORMAT_UNDEFINED ? &depth : nullptr;
    ri.pStencilAttachment = stencilFormat_ != VK_FORMAT_UNDEFINED ? &stencil : nullptr;
    vkCmdBeginRendering(cmd, &ri);
    rendering_ = true;
    ++stats_.renderPasses;
    ++stats_.totalRenderPasses;
    return true;
}

// Uploads / transitions every texture the bound shaders sample. Must run before beginRendering (barriers are not allowed
// inside a rendering scope); textures not yet used in this frame go through the upload command buffer, which keeps the
// rendering scope open.
bool Device::prepareTextures() {
    const std::uint64_t frame = frameValue();
    auto isTarget = [&](Texture* t) {
        for (Surface* s : rt_) {
            if (s != nullptr && s->texture() == t) return true;
        }
        return ds_ != nullptr && ds_->texture() == t;
    };
    for (std::uint32_t slot = 0; slot < kSamplerCount; ++slot) {
        const shader::Reflection& r = slot < 16 ? ps_->reflection : vs_->reflection;
        if (r.samplers[slot] == shader::TextureType::Unknown) {
            continue;
        }
        Texture* t = tex_[slot];
        if (t == nullptr || t->isNull() || isTarget(t)) {
            continue;  // unbound, or a feedback loop (undefined in D3D9): the default texture is sampled
        }
        const bool ready = !t->pending_ && t->image().layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (ready) {
            t->lastUseFrame_ = frame;
            continue;
        }
        if (t->lastUseFrame_ != frame && !t->isRenderTarget() && !t->isDepth()) {
            t->prepareForSampling(uploadCommands());
        } else {
            endRendering();
            t->prepareForSampling(frames_.cmd());
        }
        t->lastUseFrame_ = frame;
    }
    return true;
}

// The draw's vertex buffers and layout (recorded by applyVertexInput). With dynamic vertex input the layout leaves `pd`
// (the pipeline does not depend on it: a smaller pipeline key).
bool Device::computeVertexInput(vk::PipelineDesc& pd, VertexInputValues& out) {
    std::uint32_t streamBinding[kStreams];
    std::fill(std::begin(streamBinding), std::end(streamBinding), UINT32_MAX);
    std::uint32_t nullBinding = UINT32_MAX;
    pd.bindingCount = 0;
    pd.attributeCount = 0;
    instanceCount_ = 1;
    if ((streams_[0].freq & STREAMSOURCE_INDEXEDDATA) != 0) {
        instanceCount_ = std::max(1u, streams_[0].freq & 0x3FFFFFFFu);
    }
    for (const auto& in : vs_->reflection.vertexInputs) {
        const VertexElement* el = nullptr;
        if (decl_ != nullptr) {
            for (const auto& e : decl_->elements()) {
                if (e.usage == static_cast<std::uint8_t>(in.usage) && e.usageIndex == in.usageIndex) {
                    el = &e;
                    break;
                }
            }
        }
        const DeclTypeInfo dt = el != nullptr ? declType(el->type) : DeclTypeInfo{};
        if (el != nullptr && dt.format != VK_FORMAT_UNDEFINED && el->stream < kStreams &&
            (streams_[el->stream].buffer != nullptr || (el->stream == 0 && up_.active))) {
            std::uint32_t& b = streamBinding[el->stream];
            if (b == UINT32_MAX) {
                if (pd.bindingCount >= vk::kMaxVertexBindings) {
                    return false;
                }
                b = pd.bindingCount++;
                const Stream& st = streams_[el->stream];
                if (el->stream == 0 && up_.active) {
                    pd.bindings[b] = {up_.stride, 0};
                    out.buffers[b] = up_.buffer;
                    out.offsets[b] = up_.offset;
                } else {
                    const Buffer::Binding g = st.buffer->gpu(st.stride);
                    if (g.buffer == VK_NULL_HANDLE) {
                        return false;
                    }
                    pd.bindings[b] = {st.stride, (st.freq & STREAMSOURCE_INSTANCEDATA) != 0 ? 1u : 0u};
                    out.buffers[b] = g.buffer;
                    out.offsets[b] = g.offset + st.offset;
                }
            }
            if (pd.attributeCount < vk::kMaxVertexAttributes) {
                pd.attributes[pd.attributeCount++] = {in.location, b, dt.format, el->offset};
            }
        } else {
            // the declaration does not provide this input: D3D9 reads (0,0,0,1)
            if (nullBinding == UINT32_MAX) {
                if (pd.bindingCount >= vk::kMaxVertexBindings) {
                    return false;
                }
                nullBinding = pd.bindingCount++;
                pd.bindings[nullBinding] = {0, 0};
                out.buffers[nullBinding] = nullVertexBuffer_;
                out.offsets[nullBinding] = 0;
            }
            if (pd.attributeCount < vk::kMaxVertexAttributes) {
                pd.attributes[pd.attributeCount++] = {in.location, nullBinding, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
            }
        }
    }
    out.bufferCount = pd.bindingCount;
    // one per-vertex stream whose offset is a multiple of its stride (static meshes in the mesh arena): bound at offset 0,
    // the offset goes into the draw's vertexOffset / firstVertex, so consecutive meshes of an arena block keep the binding
    // (not for procedural replacements: their vertices start at 0)
    out.vertexShift = 0;
    if (customVertices_ == 0) {
        std::uint32_t perVertex = UINT32_MAX, count = 0;
        for (std::uint32_t i = 0; i < pd.bindingCount; ++i) {
            if (pd.bindings[i].stride != 0 && !pd.bindings[i].perInstance) {
                perVertex = i;
                ++count;
            }
        }
        if (count == 1) {
            const VkDeviceSize stride = pd.bindings[perVertex].stride;
            if (out.offsets[perVertex] % stride == 0 && out.offsets[perVertex] / stride <= 0x3FFFFFFF) {
                out.vertexShift = static_cast<std::int32_t>(out.offsets[perVertex] / stride);
                out.offsets[perVertex] = 0;
            }
        }
    }
    vertexShift_ = out.vertexShift;
    out.dynamicLayout = ctx_.features().vertexInputDynamicState;
    if (out.dynamicLayout) {
        out.bindingCount = pd.bindingCount;
        out.attributeCount = pd.attributeCount;
        std::memcpy(out.bindings, pd.bindings, sizeof(pd.bindings[0]) * pd.bindingCount);
        std::memcpy(out.attributes, pd.attributes, sizeof(pd.attributes[0]) * pd.attributeCount);
        // the pipeline does not depend on the vertex layout then: keep the cache key small
        pd.bindingCount = 0;
        pd.attributeCount = 0;
        std::memset(pd.bindings, 0, sizeof(pd.bindings));
        std::memset(pd.attributes, 0, sizeof(pd.attributes));
    }
    return true;
}

// records the buffers / layout that differ from what this command buffer has
void Device::applyVertexInput(VkCommandBuffer cmd, const VertexInputValues& vi) {
    VertexInputCache& vc = viCache_;
    const std::uint32_t n = vi.bufferCount;
    if (n > 0 && (!vc.buffersValid || vc.bufferCount != n || std::memcmp(vc.buffers, vi.buffers, sizeof(VkBuffer) * n) != 0 ||
                  std::memcmp(vc.offsets, vi.offsets, sizeof(VkDeviceSize) * n) != 0)) {
        vkCmdBindVertexBuffers(cmd, 0, n, vi.buffers, vi.offsets);
        vc.bufferCount = n;
        std::memcpy(vc.buffers, vi.buffers, sizeof(VkBuffer) * n);
        std::memcpy(vc.offsets, vi.offsets, sizeof(VkDeviceSize) * n);
        vc.buffersValid = true;
    }
    if (!vi.dynamicLayout) return;
    if (vc.layoutValid && vc.bindingCount == vi.bindingCount && vc.attributeCount == vi.attributeCount &&
        std::memcmp(vc.bindings, vi.bindings, sizeof(vi.bindings[0]) * vi.bindingCount) == 0 &&
        std::memcmp(vc.attributes, vi.attributes, sizeof(vi.attributes[0]) * vi.attributeCount) == 0) {
        return;
    }
    VkVertexInputBindingDescription2EXT b[vk::kMaxVertexBindings]{};
    VkVertexInputAttributeDescription2EXT a[vk::kMaxVertexAttributes]{};
    for (std::uint32_t i = 0; i < vi.bindingCount; ++i) {
        b[i] = {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr, i, vi.bindings[i].stride,
                vi.bindings[i].perInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, 1};
    }
    for (std::uint32_t i = 0; i < vi.attributeCount; ++i) {
        a[i] = {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, nullptr, vi.attributes[i].location,
                vi.attributes[i].binding, vi.attributes[i].format, vi.attributes[i].offset};
    }
    vkCmdSetVertexInputEXT(cmd, vi.bindingCount, b, vi.attributeCount, a);
    vc.bindingCount = vi.bindingCount;
    vc.attributeCount = vi.attributeCount;
    std::memcpy(vc.bindings, vi.bindings, sizeof(vi.bindings[0]) * vi.bindingCount);
    std::memcpy(vc.attributes, vi.attributes, sizeof(vi.attributes[0]) * vi.attributeCount);
    vc.layoutValid = true;
}

namespace {

vk::BlendAttachment blendState(const std::array<std::uint32_t, RS_COUNT>& rs, std::uint32_t target) {
    static constexpr std::uint32_t writeStates[4] = {RS_COLORWRITEENABLE, RS_COLORWRITEENABLE1, RS_COLORWRITEENABLE2,
                                                     RS_COLORWRITEENABLE3};
    vk::BlendAttachment b;
    b.enable = rs[RS_ALPHABLENDENABLE] != 0 ? 1u : 0u;
    const bool separate = rs[RS_SEPARATEALPHABLENDENABLE] != 0;
    std::uint32_t src = rs[RS_SRCBLEND], dst = rs[RS_DESTBLEND];
    // BOTHSRCALPHA / BOTHINVSRCALPHA set both factors
    if (src == BLEND_BOTHSRCALPHA) {
        src = BLEND_SRCALPHA;
        dst = BLEND_INVSRCALPHA;
    } else if (src == BLEND_BOTHINVSRCALPHA) {
        src = BLEND_INVSRCALPHA;
        dst = BLEND_SRCALPHA;
    }
    b.srcColor = blendFactor(src, false);
    b.dstColor = blendFactor(dst, false);
    b.colorOp = blendOp(rs[RS_BLENDOP]);
    b.srcAlpha = separate ? blendFactor(rs[RS_SRCBLENDALPHA], true) : blendFactor(src, true);
    b.dstAlpha = separate ? blendFactor(rs[RS_DESTBLENDALPHA], true) : blendFactor(dst, true);
    b.alphaOp = separate ? blendOp(rs[RS_BLENDOPALPHA]) : b.colorOp;
    b.writeMask = rs[writeStates[target]] & 0xF;
    return b;
}

}  // namespace

// the blend state of colour attachment `index`: the engine's targets from its render states, the G-buffer's (after them)
// unblended, written only by the draws that write it (gbufWrite_)
vk::BlendAttachment Device::attachmentBlend(std::uint32_t index) const {
    if (index < engineColorCount_) return blendState(rs_, index);
    vk::BlendAttachment b;
    b.writeMask = gbufWrite_ ? 0xFu : 0u;
    return b;
}

namespace {

// true (and the cache updated) when `value` differs from the cached one or the cache is not valid yet
template <class T>
bool changed(bool valid, T& cached, const T& value) {
    if (valid && std::memcmp(&cached, &value, sizeof(T)) == 0) {
        return false;
    }
    std::memcpy(&cached, &value, sizeof(T));
    return true;
}

}  // namespace

// The draw's dynamic state from the D3D9 render states (recorded by applyDynamicState)
void Device::computeDynamicState(std::uint32_t primitiveType, DynamicStateValues& o) const {
    // D3D9 screen space is y-down with a y-up clip space: flip with a negative viewport height
    const float vx = static_cast<float>(viewport_.x), vy = static_cast<float>(viewport_.y);
    const float vw = static_cast<float>(std::max(1u, viewport_.width)), vh = static_cast<float>(std::max(1u, viewport_.height));
    o.viewport = VkViewport{vx, vy + vh, vw, -vh, viewport_.minZ, std::max(viewport_.minZ, viewport_.maxZ)};
    std::int32_t x0 = static_cast<std::int32_t>(viewport_.x), y0 = static_cast<std::int32_t>(viewport_.y);
    std::int32_t x1 = x0 + static_cast<std::int32_t>(viewport_.width), y1 = y0 + static_cast<std::int32_t>(viewport_.height);
    if (rs_[RS_SCISSORTESTENABLE] != 0) {
        x0 = std::max(x0, scissor_[0]);
        y0 = std::max(y0, scissor_[1]);
        x1 = std::min(x1, scissor_[2]);
        y1 = std::min(y1, scissor_[3]);
    }
    x0 = std::clamp(x0, 0, static_cast<std::int32_t>(targetWidth_));
    y0 = std::clamp(y0, 0, static_cast<std::int32_t>(targetHeight_));
    x1 = std::clamp(x1, x0, static_cast<std::int32_t>(targetWidth_));
    y1 = std::clamp(y1, y0, static_cast<std::int32_t>(targetHeight_));
    o.scissor = VkRect2D{{x0, y0}, {static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)}};
    o.cull = cullMode(rs_[RS_CULLMODE]);
    o.topology = topology(primitiveType);
    const bool depth = depthFormat_ != VK_FORMAT_UNDEFINED && rs_[RS_ZENABLE] != ZB_FALSE;
    o.depthTest = depth ? VK_TRUE : VK_FALSE;
    o.depthWrite = depth && rs_[RS_ZWRITEENABLE] != 0 ? VK_TRUE : VK_FALSE;
    o.depthCompare = compareOp(rs_[RS_ZFUNC]);
    const bool stencil = stencilFormat_ != VK_FORMAT_UNDEFINED && rs_[RS_STENCILENABLE] != 0;
    o.stencilTest = stencil ? VK_TRUE : VK_FALSE;
    // front = clockwise (kFrontFace); two-sided stencil gives the counter-clockwise (back) faces their own ops
    o.stencilFront[0] = stencilOp(rs_[RS_STENCILFAIL]);
    o.stencilFront[1] = stencilOp(rs_[RS_STENCILPASS]);
    o.stencilFront[2] = stencilOp(rs_[RS_STENCILZFAIL]);
    o.stencilFrontCompare = compareOp(rs_[RS_STENCILFUNC]);
    const bool two = rs_[RS_TWOSIDEDSTENCILMODE] != 0;
    o.stencilBack[0] = two ? stencilOp(rs_[RS_CCW_STENCILFAIL]) : o.stencilFront[0];
    o.stencilBack[1] = two ? stencilOp(rs_[RS_CCW_STENCILPASS]) : o.stencilFront[1];
    o.stencilBack[2] = two ? stencilOp(rs_[RS_CCW_STENCILZFAIL]) : o.stencilFront[2];
    o.stencilBackCompare = two ? compareOp(rs_[RS_CCW_STENCILFUNC]) : o.stencilFrontCompare;
    o.stencilMasks[0] = rs_[RS_STENCILMASK] & 0xFF;
    o.stencilMasks[1] = rs_[RS_STENCILWRITEMASK] & 0xFF;
    o.stencilMasks[2] = rs_[RS_STENCILREF] & 0xFF;
    // D3D9 depth bias is in depth units; Vulkan's constant factor is in units of the format's resolution (2^-24 for D24;
    // D32F (our D24S8 on AMD) resolves 2^-24 at depths 0.5..1, where most of a scene is)
    const float bias = asFloat(rs_[RS_DEPTHBIAS]);
    const float slope = asFloat(rs_[RS_SLOPESCALEDEPTHBIAS]);
    const float unit = depthFormat_ == VK_FORMAT_D16_UNORM ? 65535.0f : 16777215.0f;
    o.depthBiasEnable = bias != 0.0f || slope != 0.0f ? VK_TRUE : VK_FALSE;
    o.depthBias[0] = bias * unit;
    o.depthBias[1] = slope;
    colorToFloat4(rs_[RS_BLENDFACTOR], o.blendConstants);
    o.colorCount = 0;
    if (ctx_.features().extendedDynamicState3 && colorCount_ > 0) {
        o.colorCount = colorCount_;
        for (std::uint32_t i = 0; i < colorCount_; ++i) {
            const vk::BlendAttachment b = attachmentBlend(i);
            o.blendEnable[i] = b.enable;
            o.blendEquation[i] = {b.srcColor, b.dstColor, b.colorOp, b.srcAlpha, b.dstAlpha, b.alphaOp};
            o.writeMask[i] = b.writeMask;
        }
    }
}

// Records only the dynamic state that differs from what this command buffer has already set (dyn_)
void Device::applyDynamicState(VkCommandBuffer cmd, const DynamicStateValues& s) {
    DynamicStateCache& c = dyn_;
    const bool v = c.valid;
    if (!v) {
        // a new command buffer: nothing is set yet (the cached colour attachment count would carry over otherwise)
        c.colorCount = 0;
        // constant for every draw
        vkCmdSetFrontFace(cmd, kFrontFace);
        vkCmdSetPrimitiveRestartEnable(cmd, VK_FALSE);
    }
    if (changed(v, c.viewport, s.viewport)) vkCmdSetViewportWithCount(cmd, 1, &s.viewport);
    if (changed(v, c.scissor, s.scissor)) vkCmdSetScissorWithCount(cmd, 1, &s.scissor);
    if (changed(v, c.cull, s.cull)) vkCmdSetCullMode(cmd, s.cull);
    if (changed(v, c.topology, s.topology)) vkCmdSetPrimitiveTopology(cmd, s.topology);
    if (changed(v, c.depthTest, s.depthTest)) vkCmdSetDepthTestEnable(cmd, s.depthTest);
    if (changed(v, c.depthWrite, s.depthWrite)) vkCmdSetDepthWriteEnable(cmd, s.depthWrite);
    if (changed(v, c.depthCompare, s.depthCompare)) vkCmdSetDepthCompareOp(cmd, s.depthCompare);
    if (changed(v, c.stencilTest, s.stencilTest)) vkCmdSetStencilTestEnable(cmd, s.stencilTest);
    {
        const bool f1 = changed(v, c.stencilFront, s.stencilFront);
        if (changed(v, c.stencilFrontCompare, s.stencilFrontCompare) || f1) {
            vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_BIT, s.stencilFront[0], s.stencilFront[1], s.stencilFront[2], s.stencilFrontCompare);
        }
        const bool b1 = changed(v, c.stencilBack, s.stencilBack);
        if (changed(v, c.stencilBackCompare, s.stencilBackCompare) || b1) {
            vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_BACK_BIT, s.stencilBack[0], s.stencilBack[1], s.stencilBack[2], s.stencilBackCompare);
        }
    }
    if (changed(v, c.stencilMasks[0], s.stencilMasks[0])) vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, s.stencilMasks[0]);
    if (changed(v, c.stencilMasks[1], s.stencilMasks[1])) vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, s.stencilMasks[1]);
    if (changed(v, c.stencilMasks[2], s.stencilMasks[2])) vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, s.stencilMasks[2]);
    if (changed(v, c.depthBiasEnable, s.depthBiasEnable)) vkCmdSetDepthBiasEnable(cmd, s.depthBiasEnable);
    if (changed(v, c.depthBias, s.depthBias)) vkCmdSetDepthBias(cmd, s.depthBias[0], 0.0f, s.depthBias[1]);
    if (changed(v, c.blendConstants, s.blendConstants)) vkCmdSetBlendConstants(cmd, s.blendConstants);
    if (s.colorCount > 0) {
        const std::uint32_t n = s.colorCount;
        // attachments beyond the count keep stale values: only the first n are compared and recorded
        const bool sameCount = v && c.colorCount >= n;
        if (!sameCount || std::memcmp(c.blendEnable, s.blendEnable, sizeof(VkBool32) * n) != 0) {
            vkCmdSetColorBlendEnableEXT(cmd, 0, n, s.blendEnable);
            std::memcpy(c.blendEnable, s.blendEnable, sizeof(VkBool32) * n);
        }
        if (!sameCount || std::memcmp(c.blendEquation, s.blendEquation, sizeof(VkColorBlendEquationEXT) * n) != 0) {
            vkCmdSetColorBlendEquationEXT(cmd, 0, n, s.blendEquation);
            std::memcpy(c.blendEquation, s.blendEquation, sizeof(VkColorBlendEquationEXT) * n);
        }
        if (!sameCount || std::memcmp(c.writeMask, s.writeMask, sizeof(VkColorComponentFlags) * n) != 0) {
            vkCmdSetColorWriteMaskEXT(cmd, 0, n, s.writeMask);
            std::memcpy(c.writeMask, s.writeMask, sizeof(VkColorComponentFlags) * n);
        }
        if (!sameCount) {
            c.colorCount = n;
        }
    }
    c.valid = true;
}

void Device::uploadConstants() {
    if (vsConstDirty_ || vsConstAddr_ == 0) {
        vk::RingAlloc a = frames_.allocate(shader::abi::kStageConstsSize, 16);
        const std::uint32_t n = std::min(vs_->reflection.floatConstCount, kVsFloatConsts);
        std::memcpy(a.ptr, vsF_, n * 16);
        auto* base = static_cast<std::uint8_t*>(a.ptr);
        std::memcpy(base + shader::abi::kStageConstsIntOffset, vsI_, sizeof(vsI_));
        std::memcpy(base + shader::abi::kStageConstsBoolOffset, vsB_, sizeof(vsB_));
        vsConstAddr_ = a.address;
        vsConstDirty_ = false;
    }
    if (psConstDirty_ || psConstAddr_ == 0) {
        vk::RingAlloc a = frames_.allocate(shader::abi::kStageConstsSize, 16);
        const std::uint32_t n = std::min(ps_->reflection.floatConstCount, kPsFloatConsts);
        std::memcpy(a.ptr, psF_, n * 16);
        auto* base = static_cast<std::uint8_t*>(a.ptr);
        std::memcpy(base + shader::abi::kStageConstsIntOffset, psI_, sizeof(psI_));
        std::memcpy(base + shader::abi::kStageConstsBoolOffset, psB_, sizeof(psB_));
        psConstAddr_ = a.address;
        psConstDirty_ = false;
    }
}

namespace {

// the render states a replacement overrides (restored when prepareDraw returns)
constexpr std::uint32_t kReplacedStates[] = {RS_ALPHABLENDENABLE, RS_SRCBLEND, RS_DESTBLEND, RS_BLENDOP, RS_SEPARATEALPHABLENDENABLE,
                                             RS_COLORWRITEENABLE, RS_CULLMODE, RS_ALPHATESTENABLE};

}  // namespace

// GI (enh/gi): the main view's lit material draws whose ambient the engine set (its first pass: the later point-light passes
// add with ONE / ONE blending and no ambient)
bool Device::giEligible(const scene::Phase& phase) const {
    if (phase.view != scene::View::Main || tag_.water != draws::Water::None) return false;
    if (phase.pass != scene::Pass::Solid && phase.pass != scene::Pass::Alpha && phase.pass != scene::Pass::AlphaLate) return false;
    if ((ps_->reflection.colorOutputs & 1u) == 0) return false;
    const auto& amb = ps_->semantics[Semantic::AmbientLight];
    if (!amb.present() || amb.count < 7 || amb.reg + 7 > static_cast<int>(kPsFloatConsts)) return false;
    if (rs_[RS_ALPHABLENDENABLE] != 0 && rs_[RS_DESTBLEND] == BLEND_ONE) return false;
    for (std::int32_t i = 0; i < 7; ++i) {
        const float* c = psF_[amb.reg + i];
        if (c[0] != 0.0f || c[1] != 0.0f || c[2] != 0.0f || c[3] != 0.0f) return true;
    }
    return false;
}

// the G-buffer's scope: the frame's first main view, its solid pass, rendering into the scene HDR target alone at the
// G-buffer's size
bool Device::gbufferTargetsMatch() const {
    const scene::Phase ph = phases_.current();
    if (ph.view != scene::View::Main || ph.pass != scene::Pass::Solid || firings_.mainViews() != 1) return false;
    const Texture* hdr = imageWithRole(scene::ImageRole::SceneHdr);
    if (rt_[0] == nullptr || hdr == nullptr || rt_[0]->texture() != hdr) return false;
    for (std::uint32_t i = 1; i < kRenderTargets; ++i) {
        if (rt_[i] != nullptr) return false;
    }
    return rt_[0]->width() == gbuf_.width && rt_[0]->height() == gbuf_.height;
}

// a material draw of the solid pass that writes the G-buffer (its Gbuffer variant): depth tested, not an additive light pass,
// and when lit, a first pass (the engine set its ambient)
bool Device::gbufferEligible(const scene::Phase& phase, draws::Kind kind) const {
    if (phase.view != scene::View::Main || phase.pass != scene::Pass::Solid || tag_.water != draws::Water::None) return false;
    if (kind == draws::Kind::Sky || kind == draws::Kind::Particle || kind == draws::Kind::Precipitation) return false;
    if ((ps_->reflection.colorOutputs & 1u) == 0 || rs_[RS_ZENABLE] == ZB_FALSE) return false;
    if (rs_[RS_ALPHABLENDENABLE] != 0 && rs_[RS_DESTBLEND] == BLEND_ONE) return false;
    const auto& amb = ps_->semantics[Semantic::AmbientLight];
    if (!amb.present()) return ps_->semantics.has(ShaderSemantics::Material);
    if (amb.count < 7 || amb.reg + 7 > static_cast<int>(kPsFloatConsts)) return false;
    for (std::int32_t i = 0; i < 7; ++i) {
        const float* c = psF_[amb.reg + i];
        if (c[0] != 0.0f || c[1] != 0.0f || c[2] != 0.0f || c[3] != 0.0f) return true;
    }
    return false;
}

bool Device::prepareDraw(std::uint32_t primitiveType, const DrawGeom& geom) {
    lastRecord_ = nullptr;
    customVertices_ = 0;
    if (!vs_ || !ps_ || vs_->module == VK_NULL_HANDLE || ps_->module == VK_NULL_HANDLE) {
        ++stats_.drawsSkipped;  // fixed-function draws are not emulated
        ++stats_.skippedNoShader;
        return false;
    }
    // what the draw is (the engine hooks' tag + the phase)
    const scene::Phase phase = phases_.current();
    if (phase.view == scene::View::Main && (phase.pass == scene::Pass::Alpha || phase.pass == scene::Pass::AlphaLate) &&
        passes_.any(inject::Point::BeforeAlpha)) {
        inject(inject::Point::BeforeAlpha);  // once per main view (inject.h): the scene without its translucent effects
    }
    const draws::Kind kind = draws::classify(tag_.source, tag_.opDesc, phase.view, phase.pass);

    // the engine's own state of the first water / refracted draw of each material (diagnostics)
    if ((tag_.water != draws::Water::None || kind == draws::Kind::Refracted) && waterLogged_.size() < 256 &&
        waterLogged_.insert(reinterpret_cast<std::uintptr_t>(tag_.material) ^ (ps_->hash * 0x9E3779B97F4A7C15ull)).second) {
        logWaterDraw(kind);
    }
    // draw replacement (enh/water, docs/research/water.md): the replacer decides before anything is recorded
    const auto query = [&](ReplaceQuery& q) {
        q.kind = kind;
        q.phase = phase;
        q.tag = &tag_;
        q.mainView = firings_.mainViews();
        q.vertexBuffer = geom.userPointer ? nullptr : streams_[0].buffer;
        q.vertexOffset = streams_[0].offset;
        q.vertexStride = streams_[0].stride;
        q.indexBuffer = geom.indexed && !geom.userPointer ? indices_ : nullptr;
        q.indexed = geom.indexed;
        q.primitiveType = primitiveType;
        q.first = geom.first;
        q.primitives = geom.primitives;
        q.baseVertex = geom.baseVertex;
        q.minIndex = geom.minIndex;
        q.numVertices = geom.numVertices;
        q.instanced = (streams_[0].freq & STREAMSOURCE_INDEXEDDATA) != 0;
        q.instances = q.instanced ? std::max(1u, streams_[0].freq & 0x3FFFFFFFu) : 1u;
        q.textures = tex_.data();
        q.engineVs = vs_.get();
        q.enginePs = ps_.get();
        q.engineVsRef = &vs_;
        q.decl = decl_;
        q.blended = rs_[RS_ALPHABLENDENABLE] != 0 && rs_[RS_SRCBLEND] == BLEND_SRCALPHA && rs_[RS_DESTBLEND] == BLEND_INVSRCALPHA;
        q.blending = rs_[RS_ALPHABLENDENABLE] != 0;
        q.srcBlend = static_cast<std::uint8_t>(rs_[RS_SRCBLEND]);
        q.dstBlend = static_cast<std::uint8_t>(rs_[RS_DESTBLEND]);
        q.alphaFunc = rs_[RS_ALPHATESTENABLE] != 0 ? rs_[RS_ALPHAFUNC] : static_cast<std::uint32_t>(CMP_ALWAYS);
        q.alphaTest = q.alphaFunc != static_cast<std::uint32_t>(CMP_ALWAYS);
        q.alphaRef = rs_[RS_ALPHAREF] & 0xFF;
        q.depthWrite = rs_[RS_ZENABLE] != ZB_FALSE && rs_[RS_ZWRITEENABLE] != 0;
        q.depthBias = rs_[RS_DEPTHBIAS] != 0 || rs_[RS_SLOPESCALEDEPTHBIAS] != 0;
    };
    // draw observers (enh/rt_scene, enh/gi): indexed draws from buffers of the kinds / phases they ask for
    captureRequest_ = 0;
    if (!observers_.empty() && geom.indexed && !geom.userPointer) {
        for (DrawObserver* o : observers_) {
            if (!o->observes(kind, phase)) continue;
            ReplaceQuery q;
            query(q);
            o->observe(*this, q);
        }
    }
    if (!replacers_.empty() && kind == draws::Kind::StaticMesh && phase.view == scene::View::Main && phase.pass == scene::Pass::Solid &&
        geom.indexed && !geom.userPointer) {
        for (DrawReplacer* r : replacers_) {
            if (!r->wantsOpaque()) continue;
            ReplaceQuery q;
            query(q);
            r->observeOpaque(*this, q);
        }
    }
    DrawReplacement rep;
    for (DrawReplacer* r : replacers_) {
        if (!r->handles(kind, phase, tag_)) continue;
        ReplaceQuery q;
        query(q);
        r->replace(*this, q, rep);
        if ((rep.action == DrawReplacement::Action::Shaders || rep.action == DrawReplacement::Action::Procedural) &&
            (!rep.vs || !rep.ps || rep.vs->module == VK_NULL_HANDLE || rep.ps->module == VK_NULL_HANDLE)) {
            rep.action = DrawReplacement::Action::Engine;
        }
        // the first replacer that takes the draw decides it; one that leaves it to the engine lets the next one look
        if (rep.action != DrawReplacement::Action::Engine) break;
    }
    if (rep.action == DrawReplacement::Action::Skip) {
        draws_.count(kind, phase.view);
        if (draws::Record* r = draws_.add()) {
            r->index = stats_.draws;
            r->view = phase.view;
            r->pass = phase.pass;
            r->kind = kind;
            r->source = tag_.source;
            r->opDesc = tag_.opDesc;
            r->context = tag_.context;
            r->entity = tag_.entity;
            r->subMesh = tag_.subMesh;
            r->material = tag_.material;
            r->meshResource = tag_.meshResource;
            r->water = rep.water != draws::Water::None ? rep.water : tag_.water;
            r->replaced = true;
            r->first = geom.first;
            r->primitives = geom.primitives;
        }
        return false;
    }
    // a replacement swaps in its shaders and opaque / cull states for this draw; everything is restored on return
    struct Swap {
        Device& d;
        bool active = false;
        std::shared_ptr<Shader> vs, ps;
        std::uint32_t states[std::size(kReplacedStates)]{};
        ~Swap() {
            if (!active) return;
            d.vs_ = std::move(vs);
            d.ps_ = std::move(ps);
            for (std::size_t i = 0; i < std::size(kReplacedStates); ++i) d.rs_[kReplacedStates[i]] = states[i];
            d.vsConstDirty_ = d.psConstDirty_ = true;
        }
    } swap{*this};
    const bool replaced = rep.action == DrawReplacement::Action::Shaders || rep.action == DrawReplacement::Action::Procedural;
    if (replaced) {
        swap.active = true;
        swap.vs = vs_;
        swap.ps = ps_;
        for (std::size_t i = 0; i < std::size(kReplacedStates); ++i) swap.states[i] = rs_[kReplacedStates[i]];
        vs_ = rep.vs;
        ps_ = rep.ps;
        vsConstDirty_ = psConstDirty_ = true;
        if (rep.opaque) {
            rs_[RS_ALPHABLENDENABLE] = 0;
            rs_[RS_SRCBLEND] = BLEND_ONE;
            rs_[RS_DESTBLEND] = BLEND_ZERO;
            rs_[RS_BLENDOP] = 1;  // D3DBLENDOP_ADD
            rs_[RS_SEPARATEALPHABLENDENABLE] = 0;
            rs_[RS_COLORWRITEENABLE] = 0xF;
        }
        rs_[RS_ALPHATESTENABLE] = 0;
        if (rep.cullNone) rs_[RS_CULLMODE] = CULL_NONE;
        if (rep.action == DrawReplacement::Action::Procedural) {
            primitiveType = 4;  // D3DPT_TRIANGLELIST
            customVertices_ = rep.vertices;
        }
        // the replacement's inputs (engine images: refraction copy, depth map, sky cube, ...) ready for sampling; barriers
        // are not allowed inside a rendering scope
        for (Texture* t : rep.sampled) {
            if (t == nullptr || t->isNull()) continue;
            if (t->pending_ || t->image().layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                endRendering();
                t->prepareForSampling(frames_.cmd());
            }
            t->lastUseFrame_ = frameValue();
        }
    }
    prepareTextures();
    // the G-buffer's attachments belong to the first main view's solid pass only: another scope when that changes
    if (rendering_ && scopeGbuf_ != gbufferScope()) endRendering();
    if (!beginRendering()) {
        ++stats_.drawsSkipped;
        ++stats_.skippedNoTarget;
        customVertices_ = 0;
        return false;
    }

    // the tint debug view swaps in the pixel shader's tint variant
    std::uint32_t tint = 0;
    VkShaderModule fs = ps_->module;
    if (!replaced && tint_ != draws::TintMode::Off && (ps_->reflection.colorOutputs & 1u) != 0) {
        tint = draws::tintColor(tint_, kind, phase.view, phase.pass);
        if (tint != 0) {
            const VkShaderModule t = tintModule(*ps_);
            fs = t != VK_NULL_HANDLE ? t : fs;
            tint = t != VK_NULL_HANDLE ? tint : 0;
        }
    }

    // detail distances (lod.trees): the billboards' baked fade-in distances scaled with the trees' LOD
    VkShaderModule vsModule = vs_->module;
    if (!replaced && kind == draws::Kind::SpeedTreeBillboard && billboardFadeScale_ != 1.0f) {
        const VkShaderModule m = lodModule(*vs_);
        vsModule = m != VK_NULL_HANDLE ? m : vsModule;
    }
    // enhancement variants (built in the background; the engine's modules until they and their pipeline exist): GI reads the
    // probe volume instead of the engine's ambient constants (enh/gi), the vertex capture writes a skinned draw's world
    // positions for the RT scene (enh/rt_scene), the G-buffer variants write the thin G-buffer (enh/gbuffer)
    gbufWrite_ = false;
    VkShaderModule fsVariant = VK_NULL_HANDLE, vsVariant = VK_NULL_HANDLE, fsPlain = VK_NULL_HANDLE;
    const bool notInstanced = (streams_[0].freq & STREAMSOURCE_INDEXEDDATA) == 0;
    if (!replaced && captureRequest_ != 0 && geom.indexed && notInstanced && vsModule == vs_->module) {
        vsVariant = variant(vs_, VariantSlot::Capture);
    }
    const bool gbufMaterial = !replaced && tint == 0 && gbuf_.normal != nullptr && gbufferEligible(phase, kind);
    if (gbufMaterial && scopeGbuf_) {
        fsPlain = variant(ps_, VariantSlot::Gbuffer);
        const VkShaderModule motion = vsVariant != VK_NULL_HANDLE ? variant(ps_, VariantSlot::GbufferMotion) : VK_NULL_HANDLE;
        fsVariant = motion != VK_NULL_HANDLE ? motion : fsPlain;
    } else if (!replaced && tint == 0 && giDraws_.on && giEligible(phase)) {
        fsVariant = variant(ps_, VariantSlot::Gi);
    }

    vk::PipelineDesc pd{};
    pd.vs = vsModule;
    pd.fs = fs;
    pd.topology = topology(primitiveType);
    // only the topology class matters (dynamic topology): normalise strips/fans to their list type for a smaller cache
    if (pd.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || pd.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN) {
        pd.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    } else if (pd.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP) {
        pd.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    }
    pd.polygonMode = polygonMode(rs_[RS_FILLMODE]);
    pd.colorCount = colorCount_;
    for (std::uint32_t i = 0; i < colorCount_; ++i) {
        pd.colorFormats[i] = colorFormats_[i];
        if (!ctx_.features().extendedDynamicState3) {
            pd.blend[i] = attachmentBlend(i);
        }
    }
    pd.depthFormat = depthFormat_;
    pd.stencilFormat = stencilFormat_;
    if (!computeVertexInput(pd, packet_.vi)) {
        ++stats_.drawsSkipped;
        ++stats_.skippedVertexInput;
        customVertices_ = 0;
        return false;
    }
    bool giDraw = false, captureDraw = false;
    const bool eds3 = ctx_.features().extendedDynamicState3;
    // a variant combination whose pipeline exists is taken; a missing one is built in the background (this draw keeps
    // the engine's shaders). `writes`: the pixel shader writes the G-buffer (without EDS3 its write mask is in the pipeline)
    const auto tryVariant = [&](VkShaderModule vsM, VkShaderModule fsM, bool writes) {
        vk::PipelineDesc vd = pd;
        if (vsM != VK_NULL_HANDLE) vd.vs = vsM;
        if (fsM != VK_NULL_HANDLE) vd.fs = fsM;
        if (writes && !eds3) {
            for (std::uint32_t i = engineColorCount_; i < colorCount_; ++i) vd.blend[i].writeMask = 0xF;
        }
        if (pipelines_.contains(vd)) {
            pd = vd;
            return true;
        }
        pipelines_.prewarmOnce(vd);
        return false;
    };
    if (gbufMaterial && scopeGbuf_) {
        // the G-buffer material pass: capture VS + motion PS, else the plain G-buffer PS, else at least the capture
        if (vsVariant != VK_NULL_HANDLE && fsVariant != VK_NULL_HANDLE && tryVariant(vsVariant, fsVariant, true)) {
            captureDraw = gbufWrite_ = true;
        } else if (fsPlain != VK_NULL_HANDLE && tryVariant(VK_NULL_HANDLE, fsPlain, true)) {
            gbufWrite_ = true;
        } else if (vsVariant != VK_NULL_HANDLE && tryVariant(vsVariant, VK_NULL_HANDLE, false)) {
            captureDraw = true;
        }
        giDraw = gbufWrite_ && giDraws_.on && ps_->semantics[Semantic::AmbientLight].present();
    } else if (fsVariant != VK_NULL_HANDLE || vsVariant != VK_NULL_HANDLE) {
        if (tryVariant(vsVariant, fsVariant, false)) {
            giDraw = fsVariant != VK_NULL_HANDLE;
            captureDraw = vsVariant != VK_NULL_HANDLE;
        } else if (vsVariant != VK_NULL_HANDLE && fsVariant != VK_NULL_HANDLE) {
            if (tryVariant(vsVariant, VK_NULL_HANDLE, false)) {
                captureDraw = true;
            } else if (tryVariant(VK_NULL_HANDLE, fsVariant, false)) {
                giDraw = true;
            }
        }
    }
    // arming the G-buffer (Device::setGbuffer): this solid draw's pipeline with the attachments, built in the background
    if (gbuf_.normal != nullptr && !gbufArmed_ && !scopeGbuf_ && colorCount_ == 1 && gbufferTargetsMatch()) {
        ++gbufSolid_;
        vk::PipelineDesc md = pd;
        md.colorCount = 3;
        md.colorFormats[1] = gbuf_.normal->format();
        md.colorFormats[2] = gbuf_.motion->format();
        bool writes = false;
        if (gbufMaterial) {
            const VariantSlot slot = captureDraw ? VariantSlot::GbufferMotion : VariantSlot::Gbuffer;
            const VkShaderModule m = variant(ps_, slot);
            if (m != VK_NULL_HANDLE) {
                md.fs = m;
                writes = true;
            } else if (ps_->variantState[static_cast<std::size_t>(slot)].load() != 3) {
                ++gbufMissing_;  // its variant is still being built
            }
        }
        if (!eds3) {
            for (std::uint32_t i = 1; i < 3; ++i) {
                md.blend[i] = vk::BlendAttachment{};
                md.blend[i].writeMask = writes ? 0xFu : 0u;
            }
        }
        if (!pipelines_.contains(md)) {
            pipelines_.prewarmOnce(md);
            ++gbufMissing_;
        }
    }
    if (kind == draws::Kind::Refracted && tag_.water != draws::Water::None && !replaced) {
        // the formats / topology class of water draws (prebuildReplacement); the first one starts the background builds
        const bool first = !waterTemplateValid_;
        waterTemplate_ = pd;
        waterTemplateValid_ = true;
        if (first) {
            for (const Registered& r : registered_) prebuildReplacement(*r.vs, *r.ps, r.procedural);
        }
    }
    // consecutive draws mostly keep their pipeline: compare with the last description before hashing
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (lastPipelineValid_ && std::memcmp(&pd, &lastPipelineDesc_, sizeof(pd)) == 0) {
        pipeline = lastPipeline_;
    } else {
        bool created = false;
        pipeline = pipelines_.get(pd, &created);
        if (created && pipeline != VK_NULL_HANDLE) {
            recordPipelineKey(pd);
        }
        lastPipelineDesc_ = pd;
        lastPipeline_ = pipeline;
        lastPipelineValid_ = true;
    }
    if (pipeline == VK_NULL_HANDLE) {
        ++stats_.drawsSkipped;
        ++stats_.skippedPipeline;
        customVertices_ = 0;
        return false;
    }
    packet_.pipeline = pipeline;
    computeDynamicState(primitiveType, packet_.dyn);
    uploadConstants();

    shader::abi::DrawBlock block{};
    float jitterX = 0.0f, jitterY = 0.0f;
    geometryJitter(jitterX, jitterY);  // camera jitter of the main view's geometry (TAA / upscaling)
    const float lodBias = (jitterX != 0.0f || jitterY != 0.0f) ? geometryLodBias_ : 0.0f;
    for (std::uint32_t slot = 0; slot < kSamplerCount; ++slot) {
        const shader::Reflection& r = slot < 16 ? ps_->reflection : vs_->reflection;
        if (r.samplers[slot] == shader::TextureType::Unknown) {
            continue;
        }
        Texture* t = tex_[slot];
        const bool kindMatches = t != nullptr &&
                                 ((r.samplers[slot] == shader::TextureType::Cube) == (t->kind() == vk::TextureKind::Cube)) &&
                                 ((r.samplers[slot] == shader::TextureType::Volume) == (t->kind() == vk::TextureKind::Volume));
        const bool usable = t != nullptr && !t->isNull() && kindMatches &&
                            t->image().layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        block.tex[slot] = !usable ? 0 : (t == substituteFrom_ && substituteSlot_ != 0) ? substituteSlot_ : t->bindlessSlot();
        block.smp[slot] = lodBias != 0.0f && slot < 16 ? samplerIndexBiased(slot, lodBias) : samplerIndex(slot);
    }
    // half a pixel right/down in NDC units of the viewport (shader/abi.h)
    block.posFix[0] = 1.0f / static_cast<float>(std::max(1u, viewport_.width));
    block.posFix[1] = -1.0f / static_cast<float>(std::max(1u, viewport_.height));
    block.posFix[0] += jitterX;
    block.posFix[1] += jitterY;
    block.alphaFunc = rs_[RS_ALPHATESTENABLE] != 0 ? rs_[RS_ALPHAFUNC] : static_cast<std::uint32_t>(CMP_ALWAYS);
    block.alphaRef = static_cast<float>(rs_[RS_ALPHAREF] & 0xFF);
    block.pad[0] = tint;
    if (giDraw) {
        ++giDrawCount_;
        if (giDraws_.on && giDraws_.replaceLightmaps) {
            for (const Semantic sem : {Semantic::LightMap1, Semantic::LightMap2}) {
                const auto& r = ps_->semantics[sem];
                if (r.present() && r.reg < 16) block.tex[r.reg] = 0;
            }
        }
    }
    if (captureDraw) {
        // the capture variant's slot = gl_VertexIndex - this draw's vertexOffset (= the index buffer value)
        block.pad[1] = static_cast<std::uint32_t>(geom.baseVertex + vertexShift_);
        captured_.push_back(captureRequest_);
    }
    vk::RingAlloc db = frames_.allocate(sizeof(block), 16);
    std::memcpy(db.ptr, &block, sizeof(block));

    if (stats_.totalDraws < options_.logDraws) {
        std::string tex;
        for (std::uint32_t slot = 0; slot < kSamplerCount; ++slot) {
            if (block.tex[slot] != 0 || tex_[slot] != nullptr) {
                Texture* t = tex_[slot];
                tex += std::format(" s{}=slot{}({}x{} {} {} levels, pending {})", slot, block.tex[slot], t ? t->desc().width : 0,
                                   t ? t->desc().height : 0, t ? vk::d3dFormatName(t->desc().format) : "-", t ? t->levels() : 0,
                                   t ? t->pending_.load() : false);
            }
        }
        spdlog::info("d3d9: draw #{} vs {:016x} ps {:016x} target {}x{} ({} colour) viewport {},{} {}x{} blend {} {}/{} alphatest {} "
                     "{}/{} zenable {} cull {} writemask {:x};{}",
                     stats_.totalDraws, vs_->hash, ps_->hash, targetWidth_, targetHeight_, colorCount_, viewport_.x, viewport_.y,
                     viewport_.width, viewport_.height, rs_[RS_ALPHABLENDENABLE], rs_[RS_SRCBLEND], rs_[RS_DESTBLEND],
                     rs_[RS_ALPHATESTENABLE], rs_[RS_ALPHAFUNC], rs_[RS_ALPHAREF], rs_[RS_ZENABLE], rs_[RS_CULLMODE],
                     rs_[RS_COLORWRITEENABLE], tex);
    }
    // drawId = the draw's record index; prevVsConsts stays 0 until motion vectors track geometry (injection.md 4)
    const std::uint32_t flags =
        shader::abi::drawFlags(static_cast<std::uint32_t>(kind), static_cast<std::uint32_t>(phase.pass), static_cast<std::uint32_t>(phase.view)) |
        (tint != 0 ? shader::abi::kDrawFlagTinted : 0u);
    const VkDeviceAddress drawBlock = rep.block != 0 && replaced ? rep.block : db.address;
    packet_.push = shader::abi::PushConstants{vsConstAddr_, psConstAddr_, drawBlock, frameBlockAddress(), captureDraw ? captureRequest_ : 0,
                                              stats_.draws, flags};
    // the opaque passes' draws may be recorded later, grouped by pipeline (flushBatch): their order only matters for surfaces
    // at exactly the same depth (docs/research/performance.md 12)
    {
        const draws::Kind k = kind;
        const bool meshKind = k == draws::Kind::StaticMesh || k == draws::Kind::InstancedMesh || k == draws::Kind::SpeedTree ||
                              k == draws::Kind::Skinned;
        const bool opaquePass = (phase.view == scene::View::Main && (phase.pass == scene::Pass::DepthPrepass || phase.pass == scene::Pass::Solid)) ||
                                (phase.view == scene::View::Shadow && phase.pass == scene::Pass::ShadowDepth);
        const VkCompareOp z = packet_.dyn.depthCompare;
        packet_.batchable = batching_ && meshKind && opaquePass && !replaced && tint == 0 && customVertices_ == 0 && geom.indexed &&
                            !geom.userPointer && rs_[RS_ALPHABLENDENABLE] == 0 && packet_.dyn.depthTest != 0 && packet_.dyn.stencilTest == 0 &&
                            (z == VK_COMPARE_OP_LESS || z == VK_COMPARE_OP_LESS_OR_EQUAL || z == VK_COMPARE_OP_EQUAL);
    }
    ++stats_.draws;
    ++stats_.totalDraws;
    ++passDraws_;
    phases_.countDraw();
    const auto recordStart = options_.perf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    draws_.count(kind, phase.view);
    if (tint != 0) draws_.countTinted();
    if (draws::Record* r = draws_.add()) {
        lastRecord_ = r;
        r->index = stats_.draws - 1;
        r->view = phase.view;
        r->pass = phase.pass;
        r->kind = kind;
        r->source = tag_.source;
        r->opDesc = tag_.opDesc;
        r->context = tag_.context;
        r->entity = tag_.entity;
        r->subMesh = tag_.subMesh;
        r->material = tag_.material;
        r->meshResource = tag_.meshResource;
        r->water = rep.water != draws::Water::None ? rep.water : tag_.water;
        r->replaced = replaced;
        std::uint16_t f = 0;
        f |= geom.indexed ? draws::Record::Indexed : 0;
        f |= geom.userPointer ? draws::Record::UserPointer : 0;
        f |= (streams_[0].freq & STREAMSOURCE_INDEXEDDATA) != 0 ? draws::Record::Instanced : 0;
        f |= decl_ != nullptr && decl_->hasBlendIndices() ? draws::Record::DeclBlendIndices : 0;
        f |= decl_ != nullptr && decl_->hasTexcoord4() ? draws::Record::DeclTexcoord4 : 0;
        f |= tint != 0 ? draws::Record::Tinted : 0;
        r->flags = f;
        r->primitiveType = static_cast<std::uint8_t>(primitiveType);
        if (!geom.userPointer && streams_[0].buffer != nullptr) {
            r->vertexBuffer = streams_[0].buffer->ref();
            r->vertexOffset = streams_[0].offset;
            r->vertexStride = streams_[0].stride;
        }
        r->first = geom.first;
        r->primitives = geom.primitives;
        r->baseVertex = geom.baseVertex;
        r->instances = instanceCount_;
        r->vsHash = vs_->hash;
        r->psHash = ps_->hash;
        r->vsFlags = vs_->semantics.flags;
        r->psFlags = ps_->semantics.flags;
        r->vsConsts = vsConstAddr_;
        r->psConsts = psConstAddr_;
        r->drawBlock = drawBlock;
        for (std::uint32_t slot = 0; slot < 16; ++slot) {
            if (ps_->reflection.samplers[slot] != shader::TextureType::Unknown && tex_[slot] != nullptr) {
                r->textures[slot] = tex_[slot]->ref().id;
            }
        }
    }
    if (options_.perf) {
        perf_.drawRecordMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - recordStart).count();
    }
    // the camera cross-check: the main view's camera constants as the first solid material draw sees them
    if (camera_.valid && !camera_.constantsCaptured && phases_.view() == scene::View::Main && phases_.pass() == scene::Pass::Solid) {
        camera_.constantsCaptured = true;
        for (std::size_t i = 0; i < camera_.vsConstants.size(); ++i) {
            std::memcpy(camera_.vsConstants[i].data(), vsF_[i], sizeof(float) * 4);
        }
    }
    return true;
}

// first water / refracted draw of each material: what the engine draws it with (docs/research/water.md)
void Device::logWaterDraw(draws::Kind kind) {
    const scene::Phase phase = phases_.current();
    std::string decl;
    if (decl_ != nullptr) {
        for (const VertexElement& e : decl_->elements()) {
            decl += std::format(" {}{}@{}+{}:t{}", e.usage, e.usageIndex, e.stream, e.offset, e.type);
        }
    }
    std::string tex;
    for (std::uint32_t slot = 0; slot < 16; ++slot) {
        if (ps_->reflection.samplers[slot] == shader::TextureType::Unknown) continue;
        const Texture* t = tex_[slot];
        tex += std::format(" s{}={}", slot, t != nullptr ? std::format("{}x{} {}{}{}", t->desc().width, t->desc().height,
                                                                        vk::d3dFormatName(t->desc().format),
                                                                        t->desc().type == TextureType::Cube ? " cube" : "",
                                                                        t->role() != scene::ImageRole::None
                                                                            ? std::string(" ") + scene::name(t->role())
                                                                            : std::string())
                                                         : std::string("-"));
    }
    const auto v4 = [](const float* c) { return std::format("({:.4g} {:.4g} {:.4g} {:.4g})", c[0], c[1], c[2], c[3]); };
    spdlog::info("water: draw of material {} '{}' ({}): kind {} in {}/{}, vs {:016x} ({} consts) ps {:016x} ({} consts), "
                 "stride {} freq {:x}, decl [{} ], blend {} {}/{} op {}, z {} write {} func {}, cull {}, alphatest {}, "
                 "samplers [{} ]",
                 tag_.material, tag_.materialName != nullptr ? tag_.materialName : "?", draws::name(tag_.water), draws::name(kind),
                 scene::name(phase.view), scene::name(phase.pass), vs_->hash, vs_->reflection.floatConstCount, ps_->hash,
                 ps_->reflection.floatConstCount, streams_[0].stride, streams_[0].freq, decl, rs_[RS_ALPHABLENDENABLE],
                 rs_[RS_SRCBLEND], rs_[RS_DESTBLEND], rs_[RS_BLENDOP], rs_[RS_ZENABLE], rs_[RS_ZWRITEENABLE], rs_[RS_ZFUNC],
                 rs_[RS_CULLMODE], rs_[RS_ALPHATESTENABLE], tex);
    spdlog::info("water:   vs c11 {} c30 {} c32 {} c33 {} c34 {} c35 {}; ps c0 {} c1 {} c3 {} c4 {} c6 {} c7 {} c22 {} c23 {} c24 {}",
                 v4(vsF_[11]), v4(vsF_[30]), v4(vsF_[32]), v4(vsF_[33]), v4(vsF_[34]), v4(vsF_[35]), v4(psF_[0]), v4(psF_[1]),
                 v4(psF_[3]), v4(psF_[4]), v4(psF_[6]), v4(psF_[7]), v4(psF_[22]), v4(psF_[23]), v4(psF_[24]));
    // the material's own constants: g_ShaderData (ps c6..c13) and g_vShaderData (vs c232.., the texcoord transforms)
    spdlog::info("water:   ps c8 {} c9 {} c10 {} c11 {} c12 {} c13 {} c14 {}; vs c232 {} c233 {} c234 {} c235 {} c236 {} c237 {} c238 {} "
                 "c239 {} c240 {} c241 {}",
                 v4(psF_[8]), v4(psF_[9]), v4(psF_[10]), v4(psF_[11]), v4(psF_[12]), v4(psF_[13]), v4(psF_[14]), v4(vsF_[232]),
                 v4(vsF_[233]), v4(vsF_[234]), v4(vsF_[235]), v4(vsF_[236]), v4(vsF_[237]), v4(vsF_[238]), v4(vsF_[239]),
                 v4(vsF_[240]), v4(vsF_[241]));
    // the shaders' D3D9 bytecode next to the shader cache (rvk_shadertool show / refdisasm)
    if (!options_.cacheDir.empty()) {
        std::error_code ec;
        const auto dir = options_.cacheDir / "water";
        std::filesystem::create_directories(dir, ec);
        for (const Shader* sh : {vs_.get(), ps_.get()}) {
            std::ofstream out(dir / std::format("{}_{:016x}.bin", sh->stage == shader::Stage::Vertex ? "vs" : "ps", sh->hash),
                              std::ios::binary);
            out.write(reinterpret_cast<const char*>(sh->bytecode.data()), static_cast<std::streamsize>(sh->bytecode.size() * 4));
        }
    }
}

void Device::drawPrimitive(std::uint32_t type, std::uint32_t startVertex, std::uint32_t primitiveCount) {
    const DrawTimer timer(*this);
    if (primitiveCount == 0 || !prepareDraw(type, DrawGeom{.first = startVertex, .primitives = primitiveCount})) {
        return;
    }
    packet_.op = DrawPacket::Op::Draw;
    if (customVertices_ != 0) {
        packet_.count = customVertices_;
        packet_.instances = 1;
        packet_.first = 0;
        customVertices_ = 0;
    } else {
        packet_.count = vertexCount(type, primitiveCount);
        packet_.instances = instanceCount_;
        packet_.first = startVertex + static_cast<std::uint32_t>(vertexShift_);
    }
    submitPacket(packet_);
}

// an offset that is a multiple of the index size goes into firstIndex: the arena block stays bound
std::uint32_t Device::foldIndexOffset(VkDeviceSize offset, bool index32, std::uint32_t firstIndex, VkDeviceSize& bindOffset) {
    const VkDeviceSize size = index32 ? 4 : 2;
    bindOffset = offset;
    if (offset % size == 0 && offset / size <= 0x3FFFFFFF) {
        firstIndex += static_cast<std::uint32_t>(offset / size);
        bindOffset = 0;
    }
    return firstIndex;
}

// the index buffer binding, recorded only when it differs from this command buffer's last one
void Device::bindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize bindOffset, bool index32) {
    IndexBindingCache& c = ibCache_;
    if (!c.valid || c.buffer != buffer || c.offset != bindOffset || c.index32 != index32) {
        vkCmdBindIndexBuffer(cmd, buffer, bindOffset, index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        c = {true, index32, buffer, bindOffset};
    }
}

void Device::drawIndexedPrimitive(std::uint32_t type, std::int32_t baseVertex, std::uint32_t minIndex, std::uint32_t numVertices,
                                  std::uint32_t startIndex, std::uint32_t primitiveCount) {
    const DrawTimer timer(*this);
    if (primitiveCount == 0 || indices_ == nullptr ||
        !prepareDraw(type, DrawGeom{.indexed = true, .first = startIndex, .primitives = primitiveCount, .baseVertex = baseVertex,
                                    .minIndex = minIndex, .numVertices = numVertices})) {
        return;
    }
    if (customVertices_ != 0) {
        packet_.op = DrawPacket::Op::Draw;
        packet_.count = customVertices_;
        packet_.instances = 1;
        packet_.first = 0;
        packet_.batchable = false;
        customVertices_ = 0;
        submitPacket(packet_);
        return;
    }
    const bool index32 = indices_->index32();
    const Buffer::Binding ib = indices_->gpu(index32 ? 4 : 2);
    if (lastRecord_ != nullptr) {
        lastRecord_->indexBuffer = indices_->ref();
    }
    if (ib.buffer == VK_NULL_HANDLE) {
        return;
    }
    packet_.op = DrawPacket::Op::DrawIndexed;
    packet_.index32 = index32;
    packet_.indexBuffer = ib.buffer;
    packet_.first = foldIndexOffset(ib.offset, index32, startIndex, packet_.indexOffset);
    packet_.count = vertexCount(type, primitiveCount);
    packet_.instances = instanceCount_;
    packet_.vertexOffset = baseVertex + vertexShift_;
    submitPacket(packet_);
}

void Device::setBatching(bool on) {
    if (on == batching_) return;
    flushBatch();
    batching_ = on;
}

void Device::submitPacket(const DrawPacket& p) {
    if (p.batchable && rendering_) {
        batch_.push_back(p);
        return;
    }
    flushBatch();  // the batch's draws come before this one
    applyPacket(frames_.cmd(), p);
}

void Device::applyPacket(VkCommandBuffer cmd, const DrawPacket& p) {
    applyVertexInput(cmd, p.vi);
    if (p.pipeline != boundPipeline_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
        boundPipeline_ = p.pipeline;
        ++perf_.pipelineBinds;
    }
    applyDynamicState(cmd, p.dyn);
    vkCmdPushConstants(cmd, bindless_.pipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p.push), &p.push);
    if (p.op == DrawPacket::Op::DrawIndexed) {
        bindIndexBuffer(cmd, p.indexBuffer, p.indexOffset, p.index32);
        vkCmdDrawIndexed(cmd, p.count, p.instances, p.first, p.vertexOffset, 0);
    } else {
        vkCmdDraw(cmd, p.count, p.instances, p.first, 0);
    }
}

// The pending batch: grouped by pipeline (stable: the engine's order within a pipeline), the first-seen pipeline first
void Device::flushBatch() {
    if (batch_.empty()) return;
    const std::uint32_t n = static_cast<std::uint32_t>(batch_.size());
    batchOrder_.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) batchOrder_[i] = i;
    if (n > 2) {
        // rank of each pipeline = the index of its first draw: groups keep the order in which their pipelines first appeared
        std::unordered_map<VkPipeline, std::uint32_t> rank;
        rank.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) rank.try_emplace(batch_[i].pipeline, i);
        std::stable_sort(batchOrder_.begin(), batchOrder_.end(),
                         [&](std::uint32_t a, std::uint32_t b) { return rank[batch_[a].pipeline] < rank[batch_[b].pipeline]; });
    }
    VkCommandBuffer cmd = frames_.cmd();
    for (const std::uint32_t i : batchOrder_) applyPacket(cmd, batch_[i]);
    perf_.batchedDraws += n;
    ++perf_.batches;
    batch_.clear();
}

// the upload ring could not take a user-pointer draw's data (a ring chunk could not be created): the draw is dropped
void Device::upAllocFailed(std::uint64_t bytes) {
    ++stats_.drawsSkipped;
    ++stats_.skippedVertexInput;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) spdlog::warn("d3d9: user-pointer draw of {} bytes dropped: no upload ring memory", bytes);
}

void Device::drawPrimitiveUP(std::uint32_t type, std::uint32_t primitiveCount, const void* data, std::uint32_t stride) {
    const DrawTimer timer(*this);
    if (primitiveCount == 0 || data == nullptr) {
        return;
    }
    const std::uint32_t vertices = vertexCount(type, primitiveCount);
    vk::RingAlloc a = frames_.allocate(static_cast<VkDeviceSize>(vertices) * stride, 16);
    if (a.ptr == nullptr) {
        upAllocFailed(static_cast<std::uint64_t>(vertices) * stride);
        return;
    }
    std::memcpy(a.ptr, data, static_cast<std::size_t>(vertices) * stride);
    up_ = {true, a.buffer, a.offset, stride};
    if (prepareDraw(type, DrawGeom{.userPointer = true, .primitives = primitiveCount})) {
        packet_.op = DrawPacket::Op::Draw;
        packet_.count = vertices;
        packet_.instances = 1;
        packet_.first = static_cast<std::uint32_t>(vertexShift_);
        submitPacket(packet_);
    }
    up_.active = false;
    // D3D9 unbinds stream 0 and the index buffer after a UP draw
    streams_[0] = {};
    streams_[0].freq = 1;
}

void Device::drawIndexedPrimitiveUP(std::uint32_t type, std::uint32_t minIndex, std::uint32_t numVertices, std::uint32_t primitiveCount,
                                    const void* indices, bool index32, const void* vertices, std::uint32_t stride) {
    const DrawTimer timer(*this);
    if (primitiveCount == 0 || indices == nullptr || vertices == nullptr) {
        return;
    }
    const std::uint32_t count = vertexCount(type, primitiveCount);
    const std::uint32_t isize = index32 ? 4 : 2;
    vk::RingAlloc ia = frames_.allocate(static_cast<VkDeviceSize>(count) * isize, 16);
    const std::uint32_t vcount = minIndex + numVertices;
    vk::RingAlloc va = frames_.allocate(static_cast<VkDeviceSize>(vcount) * stride, 16);
    if (ia.ptr == nullptr || va.ptr == nullptr) {
        upAllocFailed(static_cast<std::uint64_t>(count) * isize + static_cast<std::uint64_t>(vcount) * stride);
        return;
    }
    std::memcpy(ia.ptr, indices, static_cast<std::size_t>(count) * isize);
    std::memcpy(va.ptr, vertices, static_cast<std::size_t>(vcount) * stride);
    up_ = {true, va.buffer, va.offset, stride};
    if (prepareDraw(type, DrawGeom{.indexed = true, .userPointer = true, .primitives = primitiveCount})) {
        packet_.op = DrawPacket::Op::DrawIndexed;
        packet_.index32 = index32;
        packet_.indexBuffer = ia.buffer;
        packet_.first = foldIndexOffset(ia.offset, index32, 0, packet_.indexOffset);
        packet_.count = count;
        packet_.instances = 1;
        packet_.vertexOffset = vertexShift_;
        submitPacket(packet_);
    }
    up_.active = false;
    streams_[0] = {};
    streams_[0].freq = 1;
    indices_ = nullptr;
}

}  // namespace rvk::d3d9
