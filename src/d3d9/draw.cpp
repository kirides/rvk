// d3d9::Device draws: rendering scopes, texture preparation, pipeline state, vertex input, dynamic state, constants.

#include <cstring>

#include <spdlog/spdlog.h>

#include "d3d9/convert.h"
#include "d3d9/device.h"
#include "shader/abi.h"

namespace rvk::d3d9 {

void Device::endRendering() {
    if (rendering_) {
        vkCmdEndRendering(frames_.cmd());
        rendering_ = false;
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

bool Device::bindVertexInput(vk::PipelineDesc& pd) {
    VkCommandBuffer cmd = frames_.cmd();
    std::uint32_t streamBinding[kStreams];
    std::fill(std::begin(streamBinding), std::end(streamBinding), UINT32_MAX);
    VkBuffer buffers[vk::kMaxVertexBindings]{};
    VkDeviceSize offsets[vk::kMaxVertexBindings]{};
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
                    buffers[b] = up_.buffer;
                    offsets[b] = up_.offset;
                } else {
                    const Buffer::Binding g = st.buffer->gpu();
                    if (g.buffer == VK_NULL_HANDLE) {
                        return false;
                    }
                    pd.bindings[b] = {st.stride, (st.freq & STREAMSOURCE_INSTANCEDATA) != 0 ? 1u : 0u};
                    buffers[b] = g.buffer;
                    offsets[b] = g.offset + st.offset;
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
                buffers[nullBinding] = nullVertexBuffer_;
                offsets[nullBinding] = 0;
            }
            if (pd.attributeCount < vk::kMaxVertexAttributes) {
                pd.attributes[pd.attributeCount++] = {in.location, nullBinding, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
            }
        }
    }
    if (pd.bindingCount > 0) {
        vkCmdBindVertexBuffers(cmd, 0, pd.bindingCount, buffers, offsets);
    }
    if (ctx_.features().vertexInputDynamicState) {
        VkVertexInputBindingDescription2EXT b[vk::kMaxVertexBindings]{};
        VkVertexInputAttributeDescription2EXT a[vk::kMaxVertexAttributes]{};
        for (std::uint32_t i = 0; i < pd.bindingCount; ++i) {
            b[i] = {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr, i, pd.bindings[i].stride,
                    pd.bindings[i].perInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, 1};
        }
        for (std::uint32_t i = 0; i < pd.attributeCount; ++i) {
            a[i] = {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, nullptr, pd.attributes[i].location,
                    pd.attributes[i].binding, pd.attributes[i].format, pd.attributes[i].offset};
        }
        vkCmdSetVertexInputEXT(cmd, pd.bindingCount, b, pd.attributeCount, a);
        // the pipeline does not depend on the vertex layout then: keep the cache key small
        pd.bindingCount = 0;
        pd.attributeCount = 0;
        std::memset(pd.bindings, 0, sizeof(pd.bindings));
        std::memset(pd.attributes, 0, sizeof(pd.attributes));
    }
    return true;
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

void Device::setDynamicState(std::uint32_t primitiveType) {
    VkCommandBuffer cmd = frames_.cmd();
    // D3D9 screen space is y-down with a y-up clip space: flip with a negative viewport height
    const float vx = static_cast<float>(viewport_.x), vy = static_cast<float>(viewport_.y);
    const float vw = static_cast<float>(std::max(1u, viewport_.width)), vh = static_cast<float>(std::max(1u, viewport_.height));
    const VkViewport vp{vx, vy + vh, vw, -vh, viewport_.minZ, std::max(viewport_.minZ, viewport_.maxZ)};
    vkCmdSetViewportWithCount(cmd, 1, &vp);
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
    const VkRect2D sc{{x0, y0}, {static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)}};
    vkCmdSetScissorWithCount(cmd, 1, &sc);
    vkCmdSetCullMode(cmd, cullMode(rs_[RS_CULLMODE]));
    vkCmdSetFrontFace(cmd, kFrontFace);
    vkCmdSetPrimitiveTopology(cmd, topology(primitiveType));
    const bool depth = depthFormat_ != VK_FORMAT_UNDEFINED && rs_[RS_ZENABLE] != ZB_FALSE;
    vkCmdSetDepthTestEnable(cmd, depth ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthWriteEnable(cmd, depth && rs_[RS_ZWRITEENABLE] != 0 ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOp(cmd, compareOp(rs_[RS_ZFUNC]));
    const bool stencil = stencilFormat_ != VK_FORMAT_UNDEFINED && rs_[RS_STENCILENABLE] != 0;
    vkCmdSetStencilTestEnable(cmd, stencil ? VK_TRUE : VK_FALSE);
    // front = clockwise (kFrontFace); two-sided stencil gives the counter-clockwise (back) faces their own ops
    vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_BIT, stencilOp(rs_[RS_STENCILFAIL]), stencilOp(rs_[RS_STENCILPASS]),
                      stencilOp(rs_[RS_STENCILZFAIL]), compareOp(rs_[RS_STENCILFUNC]));
    if (rs_[RS_TWOSIDEDSTENCILMODE] != 0) {
        vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_BACK_BIT, stencilOp(rs_[RS_CCW_STENCILFAIL]), stencilOp(rs_[RS_CCW_STENCILPASS]),
                          stencilOp(rs_[RS_CCW_STENCILZFAIL]), compareOp(rs_[RS_CCW_STENCILFUNC]));
    } else {
        vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_BACK_BIT, stencilOp(rs_[RS_STENCILFAIL]), stencilOp(rs_[RS_STENCILPASS]),
                          stencilOp(rs_[RS_STENCILZFAIL]), compareOp(rs_[RS_STENCILFUNC]));
    }
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, rs_[RS_STENCILMASK] & 0xFF);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, rs_[RS_STENCILWRITEMASK] & 0xFF);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, rs_[RS_STENCILREF] & 0xFF);
    // D3D9 depth bias is in depth units; Vulkan's constant factor is in units of the format's resolution (2^-24 for D24;
    // D32F (our D24S8 on AMD) resolves 2^-24 at depths 0.5..1, where most of a scene is)
    const float bias = asFloat(rs_[RS_DEPTHBIAS]);
    const float slope = asFloat(rs_[RS_SLOPESCALEDEPTHBIAS]);
    const float unit = depthFormat_ == VK_FORMAT_D16_UNORM ? 65535.0f : 16777215.0f;
    vkCmdSetDepthBiasEnable(cmd, bias != 0.0f || slope != 0.0f ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthBias(cmd, bias * unit, 0.0f, slope);
    float bc[4];
    colorToFloat4(rs_[RS_BLENDFACTOR], bc);
    vkCmdSetBlendConstants(cmd, bc);
    vkCmdSetPrimitiveRestartEnable(cmd, VK_FALSE);
    if (ctx_.features().extendedDynamicState3 && colorCount_ > 0) {
        VkBool32 enables[kRenderTargets];
        VkColorBlendEquationEXT eqs[kRenderTargets];
        VkColorComponentFlags masks[kRenderTargets];
        for (std::uint32_t i = 0; i < colorCount_; ++i) {
            const vk::BlendAttachment b = blendState(rs_, i);
            enables[i] = b.enable;
            eqs[i] = {b.srcColor, b.dstColor, b.colorOp, b.srcAlpha, b.dstAlpha, b.alphaOp};
            masks[i] = b.writeMask;
        }
        vkCmdSetColorBlendEnableEXT(cmd, 0, colorCount_, enables);
        vkCmdSetColorBlendEquationEXT(cmd, 0, colorCount_, eqs);
        vkCmdSetColorWriteMaskEXT(cmd, 0, colorCount_, masks);
    }
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

bool Device::prepareDraw(std::uint32_t primitiveType) {
    if (!vs_ || !ps_ || vs_->module == VK_NULL_HANDLE || ps_->module == VK_NULL_HANDLE) {
        ++stats_.drawsSkipped;  // fixed-function draws are not emulated
        ++stats_.skippedNoShader;
        return false;
    }
    prepareTextures();
    if (!beginRendering()) {
        ++stats_.drawsSkipped;
        ++stats_.skippedNoTarget;
        return false;
    }
    VkCommandBuffer cmd = frames_.cmd();

    vk::PipelineDesc pd{};
    pd.vs = vs_->module;
    pd.fs = ps_->module;
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
            pd.blend[i] = blendState(rs_, i);
        }
    }
    pd.depthFormat = depthFormat_;
    pd.stencilFormat = stencilFormat_;
    if (!bindVertexInput(pd)) {
        ++stats_.drawsSkipped;
        ++stats_.skippedVertexInput;
        return false;
    }
    VkPipeline pipeline = pipelines_.get(pd);
    if (pipeline == VK_NULL_HANDLE) {
        ++stats_.drawsSkipped;
        ++stats_.skippedPipeline;
        return false;
    }
    if (pipeline != boundPipeline_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        boundPipeline_ = pipeline;
    }
    setDynamicState(primitiveType);
    uploadConstants();

    shader::abi::DrawBlock block{};
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
        block.tex[slot] = usable ? t->bindlessSlot() : 0;
        block.smp[slot] = samplerIndex(slot);
    }
    // half a pixel right/down in NDC units of the viewport (shader/abi.h)
    block.posFix[0] = 1.0f / static_cast<float>(std::max(1u, viewport_.width));
    block.posFix[1] = -1.0f / static_cast<float>(std::max(1u, viewport_.height));
    block.alphaFunc = rs_[RS_ALPHATESTENABLE] != 0 ? rs_[RS_ALPHAFUNC] : static_cast<std::uint32_t>(CMP_ALWAYS);
    block.alphaRef = static_cast<float>(rs_[RS_ALPHAREF] & 0xFF);
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
    const shader::abi::PushConstants push{vsConstAddr_, psConstAddr_, db.address};
    vkCmdPushConstants(cmd, bindless_.pipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    ++stats_.draws;
    ++stats_.totalDraws;
    return true;
}

void Device::drawPrimitive(std::uint32_t type, std::uint32_t startVertex, std::uint32_t primitiveCount) {
    if (primitiveCount == 0 || !prepareDraw(type)) {
        return;
    }
    vkCmdDraw(frames_.cmd(), vertexCount(type, primitiveCount), instanceCount_, startVertex, 0);
}

void Device::drawIndexedPrimitive(std::uint32_t type, std::int32_t baseVertex, std::uint32_t, std::uint32_t, std::uint32_t startIndex,
                                  std::uint32_t primitiveCount) {
    if (primitiveCount == 0 || indices_ == nullptr || !prepareDraw(type)) {
        return;
    }
    const Buffer::Binding ib = indices_->gpu();
    if (ib.buffer == VK_NULL_HANDLE) {
        return;
    }
    vkCmdBindIndexBuffer(frames_.cmd(), ib.buffer, ib.offset, indices_->index32() ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(frames_.cmd(), vertexCount(type, primitiveCount), instanceCount_, startIndex, baseVertex, 0);
}

void Device::drawPrimitiveUP(std::uint32_t type, std::uint32_t primitiveCount, const void* data, std::uint32_t stride) {
    if (primitiveCount == 0 || data == nullptr) {
        return;
    }
    const std::uint32_t vertices = vertexCount(type, primitiveCount);
    vk::RingAlloc a = frames_.allocate(static_cast<VkDeviceSize>(vertices) * stride, 16);
    std::memcpy(a.ptr, data, static_cast<std::size_t>(vertices) * stride);
    up_ = {true, a.buffer, a.offset, stride};
    if (prepareDraw(type)) {
        vkCmdDraw(frames_.cmd(), vertices, 1, 0, 0);
    }
    up_.active = false;
    // D3D9 unbinds stream 0 and the index buffer after a UP draw
    streams_[0] = {};
    streams_[0].freq = 1;
}

void Device::drawIndexedPrimitiveUP(std::uint32_t type, std::uint32_t minIndex, std::uint32_t numVertices, std::uint32_t primitiveCount,
                                    const void* indices, bool index32, const void* vertices, std::uint32_t stride) {
    if (primitiveCount == 0 || indices == nullptr || vertices == nullptr) {
        return;
    }
    const std::uint32_t count = vertexCount(type, primitiveCount);
    const std::uint32_t isize = index32 ? 4 : 2;
    vk::RingAlloc ia = frames_.allocate(static_cast<VkDeviceSize>(count) * isize, 16);
    std::memcpy(ia.ptr, indices, static_cast<std::size_t>(count) * isize);
    const std::uint32_t vcount = minIndex + numVertices;
    vk::RingAlloc va = frames_.allocate(static_cast<VkDeviceSize>(vcount) * stride, 16);
    std::memcpy(va.ptr, vertices, static_cast<std::size_t>(vcount) * stride);
    up_ = {true, va.buffer, va.offset, stride};
    if (prepareDraw(type)) {
        vkCmdBindIndexBuffer(frames_.cmd(), ia.buffer, ia.offset, index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(frames_.cmd(), count, 1, 0, 0, 0);
    }
    up_.active = false;
    streams_[0] = {};
    streams_[0].freq = 1;
    indices_ = nullptr;
}

}  // namespace rvk::d3d9
