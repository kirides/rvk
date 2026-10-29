// d3d9::Device draws: rendering scopes, texture preparation, pipeline state, vertex input, dynamic state, constants.

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "d3d9/convert.h"
#include "d3d9/device.h"
#include "d3d9/memeq.h"
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
        const bool secondary = scopeSecondary_;
        if (secondary) finishSecondaryScope();
        vkCmdEndRendering(frames_.cmd());
        rendering_ = false;
        if (secondary) {
            // executing secondary command buffers leaves the primary's pipeline, dynamic state and descriptor sets undefined
            rec_.reset();
            const VkDescriptorSet set = bindless_.set();
            vkCmdBindDescriptorSets(frames_.cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, bindless_.pipelineLayout(), 0, 1, &set, 0, nullptr);
        }
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
    scopeSecondary_ = wantSecondaryScope();
    if (scopeSecondary_) ri.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
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
    for (std::uint32_t mask = boundSamplerMask(); mask != 0; mask &= mask - 1) {
        const std::uint32_t slot = static_cast<std::uint32_t>(std::countr_zero(mask));
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

// The shader's inputs against the declaration, computed once per pair (the declaration and the shader never change)
const Device::VertexPlan& Device::vertexPlan(const VertexDecl* decl, Shader& vs) {
    const std::uint64_t declId = decl != nullptr ? decl->id() : 0;
    if (plan_ != nullptr && planShader_ == &vs && planDeclId_ == declId) {
        return *plan_;
    }
    Shader& shader = vs;
    for (const Shader::PlanSlot& slot : shader.planSlots) {
        if (slot.plan != nullptr && slot.declId == declId) {
            planShader_ = &vs;
            planDeclId_ = declId;
            plan_ = static_cast<const VertexPlan*>(slot.plan);
            return *plan_;
        }
    }
    const std::uint64_t key = (declId * 0x9E3779B97F4A7C15ull) ^ vs.hash;
    auto [it, fresh] = vertexPlans_.try_emplace(key);
    VertexPlan& plan = it->second;
    if (fresh) {
        for (const auto& in : vs.reflection.vertexInputs) {
            VertexPlanInput pi;
            pi.location = in.location;
            const VertexElement* el = nullptr;
            if (decl != nullptr) {
                for (const auto& e : decl->elements()) {
                    if (e.usage == static_cast<std::uint8_t>(in.usage) && e.usageIndex == in.usageIndex) {
                        el = &e;
                        break;
                    }
                }
            }
            if (el != nullptr) {
                const DeclTypeInfo dt = declType(el->type);
                if (dt.format != VK_FORMAT_UNDEFINED && el->stream < kStreams) {
                    pi.stream = el->stream;
                    pi.offset = el->offset;
                    pi.format = dt.format;
                }
            }
            plan.inputs.push_back(pi);
        }
    }
    shader.planSlots[shader.planNext++ % shader.planSlots.size()] = {declId, &plan};
    planShader_ = &vs;
    planDeclId_ = declId;
    plan_ = &plan;  // unordered_map nodes are stable
    return plan;
}

// The draw's vertex buffers and layout (recorded by applyVertexInput). With dynamic vertex input the layout leaves `pd`
// (the pipeline does not depend on it: a smaller pipeline key).
// Two descriptions equal. With extended dynamic state 3 (blend is not in the description: it stays at its defaults) and dynamic
// vertex input (bindings / attributes are cleared by computeVertexInput) everything after `blend` is the same in every
// description, so only the 52 leading bytes are compared (the 512-byte compare ran for every draw, twice when the pipeline changed)
bool Device::sameDesc(const vk::PipelineDesc& a, const vk::PipelineDesc& b) const {
    if (ctx_.features().extendedDynamicState3 && ctx_.features().vertexInputDynamicState && a.bindingCount == 0 && a.attributeCount == 0 &&
        b.bindingCount == 0 && b.attributeCount == 0) {
        return memEqual(&a, &b, offsetof(vk::PipelineDesc, blend));
    }
    return memEqual(&a, &b, sizeof(a));
}

std::size_t Device::recentSlot(const vk::PipelineDesc& pd) {
    const std::uint64_t h = ((reinterpret_cast<std::uintptr_t>(pd.vs) >> 4) * 0x9E3779B97F4A7C15ull ^
                             (reinterpret_cast<std::uintptr_t>(pd.fs) >> 4) * 0xC2B2AE3D27D4EB4Full ^ pd.colorFormats[0] ^
                             (static_cast<std::uint64_t>(pd.depthFormat) << 8) ^ (static_cast<std::uint64_t>(pd.colorCount) << 16)) >>
                            40;
    return static_cast<std::size_t>(h % 256);
}

// the pipeline of `d` when it exists (a direct-mapped cache of recent descriptions first: no 520-byte hash, no lock), never builds
VkPipeline Device::findPipeline(const vk::PipelineDesc& d) {
    RecentPipeline& recent = recentPipelines_[recentSlot(d)];
    if (recent.pipeline != VK_NULL_HANDLE && sameDesc(recent.desc, d)) return recent.pipeline;
    const VkPipeline p = pipelines_.find(d);
    if (p != VK_NULL_HANDLE) {
        recent.desc = d;
        recent.pipeline = p;
    }
    return p;
}

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
    for (const VertexPlanInput& in : vertexPlan(decl_, *vs_).inputs) {
        if (in.stream != UINT32_MAX && (streams_[in.stream].buffer != nullptr || (in.stream == 0 && up_.active))) {
            std::uint32_t& b = streamBinding[in.stream];
            if (b == UINT32_MAX) {
                if (pd.bindingCount >= vk::kMaxVertexBindings) {
                    return false;
                }
                b = pd.bindingCount++;
                const Stream& st = streams_[in.stream];
                if (in.stream == 0 && up_.active) {
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
                pd.attributes[pd.attributeCount++] = {in.location, b, in.format, in.offset};
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
            const VkDeviceSize offset = out.offsets[perVertex];
            if (offset <= UINT32_MAX) {
                // (a 32-bit division: mesh arena blocks are far below 4 GiB; the 64-bit one was ~0.4 % of the engine thread)
                const std::uint32_t o = static_cast<std::uint32_t>(offset), s = static_cast<std::uint32_t>(stride);
                const std::uint32_t q = o / s;
                if (o - q * s == 0 && q <= 0x3FFFFFFF) {
                    out.vertexShift = static_cast<std::int32_t>(q);
                    out.offsets[perVertex] = 0;
                }
            } else if (offset % stride == 0 && offset / stride <= 0x3FFFFFFF) {
                out.vertexShift = static_cast<std::int32_t>(offset / stride);
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
void Device::applyVertexInput(RecordState& rs, VkCommandBuffer cmd, const VertexInputValues& vi) {
    VertexInputCache& vc = rs.vi;
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
    const bool reversed = boundDepthReversed();
    if (reversed) o.depthCompare = reverseCompareOp(o.depthCompare);
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
    o.depthBias[0] = reversed ? -bias * unit : bias * unit;  // reversed depth grows towards the camera
    o.depthBias[1] = reversed ? -slope : slope;
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
void Device::applyDynamicState(RecordState& rs, VkCommandBuffer cmd, const DynamicStateValues& s) {
    DynamicStateCache& c = rs.dyn;
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

void Device::uploadConstants(bool forceLegacyPs, std::uint32_t psMinEnd) {
    // per dirty stage a StageConsts block (shader/abi.h) from the ring: the floats up to the shader's highest read register, the
    // int / bool part only for shaders that read it (Reflection::intBoolConstants; the layout stays, the bytes are left as they are)
    const auto upload = [&](const Shader& sh, const float (*f)[4], std::uint32_t maxFloats, const std::int32_t (*i)[4],
                            const std::uint32_t* b, VkDeviceAddress& addr, std::uint32_t stage) {
        vk::RingAlloc a = frames_.allocate(shader::abi::kStageConstsSize, 16);
        const std::uint32_t n = std::min(sh.reflection.floatConstCount, maxFloats);
        std::memcpy(a.ptr, f, n * 16);
        std::uint32_t bytes = n * 16;
        if (sh.reflection.intBoolConstants) {
            auto* base = static_cast<std::uint8_t*>(a.ptr);
            std::memcpy(base + shader::abi::kStageConstsIntOffset, i, 16 * 16);
            std::memcpy(base + shader::abi::kStageConstsBoolOffset, b, 16 * 4);
            bytes += 16 * 16 + 16 * 4;
        } else {
            ++perf_.constIntBoolSkipped;
        }
        ++perf_.constUploads[stage];
        perf_.constBytes += bytes;
        addr = a.address;
    };
    // split constants (abi.h windows, plan.md V2): a shader that reads its floats through the window blocks gets only those
    // windows, each uploaded when a register of it changed since its last upload (or when it was uploaded shorter than this
    // shader reads). Everything else (hand-written shaders, effects that read i# / b#, the GLSL material shader, replaced
    // draws with their own DrawBlock) keeps the one full block per stage
    const bool uber = ubershader_.load(std::memory_order_relaxed);
    drawSplit_[0] = splitConstants_ && vs_->reflection.splitConsts && !uber;
    drawSplit_[1] = splitConstants_ && ps_->reflection.splitConsts && !forceLegacyPs && !uber;
    psWindowMinEnd_ = psMinEnd;
    if (drawSplit_[0]) {
        splitStage(true, *vs_, drawWin_[0]);
        ++perf_.splitDraws[0];
    } else {
        ++perf_.legacyDraws[0];
        if (vsConstDirty_ || vsConstAddr_ == 0) {
            upload(*vs_, vsF_, kVsFloatConsts, vsI_, vsB_, vsConstAddr_, 0);
            vsConstDirty_ = false;
        }
        for (auto& w : drawWin_[0]) w = vsConstAddr_;
    }
    if (drawSplit_[1]) {
        splitStage(false, *ps_, drawWin_[1]);
        ++perf_.splitDraws[1];
    } else {
        ++perf_.legacyDraws[1];
        if (psConstDirty_ || psConstAddr_ == 0) {
            upload(*ps_, psF_, kPsFloatConsts, psI_, psB_, psConstAddr_, 1);
            psConstDirty_ = false;
        }
        for (auto& w : drawWin_[1]) w = psConstAddr_;
    }
}

// The window blocks of a split stage: windows[w] = the biased base address (block base - first register * 16, so c[n] keeps
// the D3D9 register number). Pixel window 1 (c5) is the DrawBlock's psC5, patched in by prepareDraw
void Device::splitStage(bool vertex, const Shader& sh, VkDeviceAddress (&windows)[shader::abi::kConstWindows]) {
    const std::uint32_t stage = vertex ? 0 : 1;
    const std::uint32_t* first = vertex ? shader::abi::kVsWindowFirst : shader::abi::kPsWindowFirst;
    const float (*regs)[4] = vertex ? vsF_ : psF_;
    for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) {
        if (!vertex && w == 1) {
            windows[w] = 0;
            continue;
        }
        std::uint32_t end = sh.reflection.windowEnd[w];
        if (!vertex && w == 3) end = std::max(end, psWindowMinEnd_);  // an enhancement variant's extra reads (ambient)
        const std::uint32_t need = end > first[w] ? end - first[w] : 0;
        if (need == 0) {
            if (zeroBlock_ == 0) {
                vk::RingAlloc z = frames_.allocate(4096, 16);
                std::memset(z.ptr, 0, 4096);
                zeroBlock_ = z.address;
            }
            windows[w] = zeroBlock_ - static_cast<VkDeviceAddress>(first[w]) * 16;
            continue;
        }
        ConstWindow& cw = constWin_[stage][w];
        if ((constWinDirty_[stage] >> w & 1u) != 0 || cw.address == 0 || cw.extent < need) {
            vk::RingAlloc a = frames_.allocate(static_cast<VkDeviceSize>(need) * 16, 16);
            std::memcpy(a.ptr, regs[first[w]], static_cast<std::size_t>(need) * 16);
            cw.address = a.address;
            cw.ptr = static_cast<const std::uint8_t*>(a.ptr);
            cw.extent = need;
            constWinDirty_[stage] &= ~(1u << w);
            ++perf_.splitBlocks[stage][w];
            perf_.splitBytes[stage][w] += static_cast<std::uint64_t>(need) * 16;
            perf_.constBytes += static_cast<std::uint64_t>(need) * 16;
        } else {
            ++perf_.splitReused[stage][w];
        }
        windows[w] = cw.address - static_cast<VkDeviceAddress>(first[w]) * 16;
    }
    if (splitVerify_) verifySplit(vertex, sh);
}

// debug.split_verify: every window block this shader reads equals the register file now (dirty tracking, extents)
void Device::verifySplit(bool vertex, const Shader& sh) {
    const std::uint32_t stage = vertex ? 0 : 1;
    const std::uint32_t* first = vertex ? shader::abi::kVsWindowFirst : shader::abi::kPsWindowFirst;
    const float (*regs)[4] = vertex ? vsF_ : psF_;
    for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) {
        if (!vertex && w == 1) continue;
        std::uint32_t end = sh.reflection.windowEnd[w];
        if (!vertex && w == 3) end = std::max(end, psWindowMinEnd_);
        const std::uint32_t need = end > first[w] ? end - first[w] : 0;
        if (need == 0) continue;
        const ConstWindow& cw = constWin_[stage][w];
        bool bad = cw.ptr == nullptr || cw.extent < need;
        std::uint32_t reg = first[w];
        if (!bad) {
            for (std::uint32_t i = 0; i < need; ++i) {
                if (std::memcmp(cw.ptr + static_cast<std::size_t>(i) * 16, regs[first[w] + i], 16) != 0) {
                    bad = true;
                    reg = first[w] + i;
                    break;
                }
            }
        }
        if (bad && perf_.splitMismatches++ < 24) {
            spdlog::error("d3d9: split constants: {} window {} (shader {:016x}) differs from the register file at c{} "
                          "(need {} registers, block has {})",
                          vertex ? "VS" : "PS", w, sh.hash, reg, need, cw.extent);
        }
    }
}

namespace {

// the all-default pipeline description (padding zero), copied by prepareDraw
constexpr vk::PipelineDesc kBlankPipelineDesc{};

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
    if (drawTap_ != nullptr) {
        // the N5.0 op tap sees every draw, also the ones skipped below
        DrawTapInfo t;
        t.index = stats_.draws;
        t.phase = phases_.current();
        t.kind = draws::classify(tag_.source, tag_.opDesc, t.phase.view, t.phase.pass);
        t.tag = &tag_;
        t.device = this;
        t.indexed = geom.indexed && !geom.userPointer;
        t.primitiveType = primitiveType;
        t.first = geom.first;
        t.baseVertex = geom.baseVertex;
        t.minIndex = geom.minIndex;
        t.numVertices = geom.numVertices;
        t.primitives = geom.primitives;
        t.instanced = (streams_[0].freq & STREAMSOURCE_INDEXEDDATA) != 0;
        t.instances = t.instanced ? std::max(1u, streams_[0].freq & 0x3FFFFFFFu) : 1u;
        t.additive = rs_[RS_ALPHABLENDENABLE] != 0 && rs_[RS_DESTBLEND] == BLEND_ONE;
        t.colorWrite = rs_[RS_COLORWRITEENABLE] != 0;
        t.vsHash = vs_ ? vs_->hash : 0;
        t.psHash = ps_ ? ps_->hash : 0;
        t.blend = (rs_[RS_ALPHABLENDENABLE] != 0 ? 1u : 0u) | (rs_[RS_SRCBLEND] & 0xff) << 8 | (rs_[RS_DESTBLEND] & 0xff) << 16 |
                  (rs_[RS_ZFUNC] & 0xff) << 24;
        if (drawTap_->onDraw(t)) {
            ++stats_.drawsTakenOver;
            return false;
        }
    }
    if (!vs_ || !ps_ || vs_->module == VK_NULL_HANDLE || ps_->module == VK_NULL_HANDLE) {
        ++stats_.drawsSkipped;  // fixed-function draws are not emulated
        ++stats_.skippedNoShader;
        return false;
    }
    // what the draw is (the engine hooks' tag + the phase)
    const scene::Phase phase = phases_.current();
    syncPhaseRegions();
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
    captureInstances_ = false;
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
        ShaderRef vs, ps;
        std::uint32_t states[std::size(kReplacedStates)]{};
        ~Swap() {
            if (!active) return;
            d.vs_ = vs;
            d.ps_ = ps;
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
    if (rendering_ && (scopeGbuf_ != gbufferScope() || scopeSecondary_ != wantSecondaryScope())) endRendering();
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

    // the GLSL material shader (Device::setUbershader): the plain module of a material pixel shader whose request is known
    VkShaderModule uberBase = VK_NULL_HANDLE;
    if (!replaced && tint == 0 && ubershader_.load(std::memory_order_relaxed) && ps_->source.load(std::memory_order_acquire) != nullptr) {
        uberBase = variant(ps_, VariantSlot::Base);
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
    if (!replaced && captureRequest_ != 0 && geom.indexed && (notInstanced || captureInstances_) && vsModule == vs_->module) {
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

    // a copy of the blank description: `vk::PipelineDesc pd{}` initialised the 520 bytes member by member (blend defaults are
    // not zero; ~1.5 % of the engine thread in the V4 profile)
    vk::PipelineDesc pd = kBlankPipelineDesc;
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
    if (!computeVertexInput(pd, workVi_)) {
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
        if (findPipeline(vd) != VK_NULL_HANDLE) {
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
    // no enhancement variant took the draw: the GLSL material shader when it and its pipeline exist (else the translated one)
    if (uberBase != VK_NULL_HANDLE && pd.fs == fs) {
        vk::PipelineDesc ud = pd;
        ud.fs = uberBase;
        if (findPipeline(ud) != VK_NULL_HANDLE) {
            pd = ud;
        } else {
            pipelines_.prewarmOnce(ud);
        }
    }
    // consecutive draws mostly keep their pipeline: compare with the last description before hashing
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (lastPipelineValid_ && sameDesc(pd, lastPipelineDesc_)) {
        pipeline = lastPipeline_;
    } else {
        // a direct-mapped cache of recent descriptions in front of the pipeline map: no 520-byte hash, no lock, no node walk
        // (a pipeline never changes once made)
        pipeline = findPipeline(pd);
        if (pipeline == VK_NULL_HANDLE) {
            bool created = false;
            pipeline = pipelines_.get(pd, &created);
            if (created && pipeline != VK_NULL_HANDLE) {
                recordPipelineKey(pd);
            }
            if (pipeline != VK_NULL_HANDLE) {
                RecentPipeline& recent = recentPipelines_[recentSlot(pd)];
                recent.desc = pd;
                recent.pipeline = pipeline;
            }
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
    computeDynamicState(primitiveType, workDyn_);
    {
        // the pixel window of an enhancement variant that reads the ambient registers the base shader may not (enh/material_variant)
        std::uint32_t minEnd = 0;
        if (giDraw || gbufWrite_) {
            const auto& amb = ps_->semantics[Semantic::AmbientLight];
            if (amb.present() && amb.count >= 7) {
                minEnd = static_cast<std::uint32_t>(amb.reg + 7);
                const auto& ind = ps_->semantics[Semantic::IndoorAmbient];
                if (ind.present()) minEnd = std::max(minEnd, static_cast<std::uint32_t>(ind.reg + 1));
            }
        }
        uploadConstants(rep.block != 0 && replaced, minEnd);
    }

    shader::abi::DrawBlock block{};
    float jitterX = 0.0f, jitterY = 0.0f;
    geometryJitter(jitterX, jitterY);  // camera jitter of the main view's geometry (TAA / upscaling)
    const float lodBias = (jitterX != 0.0f || jitterY != 0.0f) ? geometryLodBias_ : 0.0f;
    for (std::uint32_t mask = boundSamplerMask(); mask != 0; mask &= mask - 1) {
        const std::uint32_t slot = static_cast<std::uint32_t>(std::countr_zero(mask));
        const shader::Reflection& r = slot < 16 ? ps_->reflection : vs_->reflection;
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
    depthRemap(block.posFix[2], block.posFix[3]);
    block.alphaFunc = rs_[RS_ALPHATESTENABLE] != 0 ? rs_[RS_ALPHAFUNC] : static_cast<std::uint32_t>(CMP_ALWAYS);
    block.alphaRef = static_cast<float>(rs_[RS_ALPHAREF] & 0xFF);
    // scaled with the render scale: none at native AA (strands are wide enough), half at 1.5x, full from 2x on
    const float thicken = cutoutThicken_ * std::clamp(renderRatio_ - 1.0f, 0.0f, 1.0f);
    if (thicken > 0.0f && (jitterX != 0.0f || jitterY != 0.0f)) {
        // the shader decides: its texkill (the z pre-pass cut-outs) and a GREATER / GREATEREQUAL alpha test (abi.h)
        block.alphaFunc |= shader::abi::kAlphaThicken | (static_cast<std::uint32_t>(thicken * 255.0f + 0.5f) << 24);
    }
    if (alphaDither_ && (jitterX != 0.0f || jitterY != 0.0f)) {
        // the shader decides: its texkill (the z pre-pass cut-outs) and a GREATER / GREATEREQUAL alpha test hash (abi.h)
        block.alphaFunc |= shader::abi::kAlphaHashed | static_cast<std::uint32_t>((stats_.frames & 63u) << 16);
        const std::uint32_t func = block.alphaFunc & 0xffu;
        const bool cuts = (ps_ && ps_->reflection.discards) || func == static_cast<std::uint32_t>(CMP_GREATER) ||
                          func == static_cast<std::uint32_t>(CMP_GREATEREQUAL);
        if (cuts && tag_.materialName != nullptr && hashedMaterials_.size() < 64 && hashedMaterials_.insert(tag_.material).second) {
            spdlog::trace("d3d9: hashed alpha test on material '{}' ({}/{}, {}, func {} ref {})", tag_.materialName,
                          scene::name(phases_.current().view), scene::name(phases_.current().pass),
                          ps_ && ps_->reflection.discards ? "texkill" : "alpha test", func, block.alphaRef);
        }
        if (cuts && !hashedAlphaLogged_) {
            hashedAlphaLogged_ = true;
            spdlog::info("d3d9: hashed alpha test (upscale.alpha_dither): first draw in {}/{}, func {} ref {}", scene::name(phases_.current().view), scene::name(phases_.current().pass),
                         block.alphaFunc & 0xffu, block.alphaRef);
        }
    }
    logTracedMaterial(kind, jitterX != 0.0f || jitterY != 0.0f);
    block.pad[0] = tint;
    std::memcpy(block.psC5, psF_[5], sizeof(block.psC5));  // pixel window 1 of split draws (abi.h)
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
    packet_.push = shader::abi::PushConstants{drawSplit_[0] ? 0 : vsConstAddr_, drawSplit_[1] ? 0 : psConstAddr_, drawBlock, frameBlockAddress(), captureDraw ? captureRequest_ : 0,
                                              stats_.draws, flags};
    for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) {
        packet_.push.vsWin[w] = drawWin_[0][w];
        packet_.push.psWin[w] = drawWin_[1][w];
    }
    if (drawSplit_[1]) {
        // c5 = the DrawBlock's psC5 (this draw's alpha reference): biased like the other windows
        packet_.push.psWin[1] = db.address + offsetof(shader::abi::DrawBlock, psC5) - 5 * 16;
    }
    // the opaque passes' draws may be recorded later, grouped by pipeline (flushBatch): their order only matters for surfaces
    // at exactly the same depth (docs/research/performance.md 12)
    {
        const draws::Kind k = kind;
        const bool meshKind = k == draws::Kind::StaticMesh || k == draws::Kind::InstancedMesh || k == draws::Kind::SpeedTree ||
                              k == draws::Kind::Skinned;
        const bool opaquePass = (phase.view == scene::View::Main && (phase.pass == scene::Pass::DepthPrepass || phase.pass == scene::Pass::Solid)) ||
                                (phase.view == scene::View::Shadow && phase.pass == scene::Pass::ShadowDepth);
        const VkCompareOp z = workDyn_.depthCompare;
        packet_.batchable = batching_ && meshKind && opaquePass && !replaced && tint == 0 && customVertices_ == 0 && geom.indexed &&
                            !geom.userPointer && rs_[RS_ALPHABLENDENABLE] == 0 && workDyn_.depthTest != 0 && workDyn_.stencilTest == 0 &&
                            (z == VK_COMPARE_OP_LESS || z == VK_COMPARE_OP_LESS_OR_EQUAL || z == VK_COMPARE_OP_EQUAL ||
                         z == VK_COMPARE_OP_GREATER || z == VK_COMPARE_OP_GREATER_OR_EQUAL);
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
        r->vsConsts = drawSplit_[0] ? drawWin_[0][2] : vsConstAddr_;  // split draws: the object window (biased)
        r->psConsts = drawSplit_[1] ? drawWin_[1][3] : psConstAddr_;
        r->drawBlock = drawBlock;
        for (std::uint32_t mask = ps_->samplerMask & 0xFFFFu; mask != 0; mask &= mask - 1) {
            const std::uint32_t slot = static_cast<std::uint32_t>(std::countr_zero(mask));
            if (tex_[slot] != nullptr) r->textures[slot] = tex_[slot]->ref().id;
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
void Device::bindIndexBuffer(RecordState& rs, VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize bindOffset, bool index32) {
    IndexBindingCache& c = rs.ib;
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

// ---- reversed infinite depth (docs/research/reverse-z.md) ----------------------------------------------------------------

void Device::setReverseZ(bool on) { reverseZ_ = on; }

float Device::reverseDepthK() const {
    const scene::CameraBlock* cam = camera_.valid ? &camera_ : lastCamera_.valid ? &lastCamera_ : nullptr;
    if (cam != nullptr && cam->nearZ > 0.0f && cam->farZ > cam->nearZ) reverseK_ = 1.0f - cam->nearZ / cam->farZ;
    return reverseK_;
}

bool Device::boundDepthReversed() const {
    return ds_ != nullptr && !ds_->texture()->isNull() && ds_->texture()->reverseZ_;
}

void Device::depthRemap(float& zScale, float& zBias) const {
    zScale = zBias = 0.0f;
    if (!boundDepthReversed()) return;
    zScale = -reverseDepthK() - 1.0f;  // z' = z (1 + zScale) + zBias w = w - k z
    zBias = 1.0f;
}

void Device::setBatching(bool on) {
    if (on == batching_) return;
    flushBatch();
    batching_ = on;
}

// the used parts only: the work blocks keep stale bytes beyond the counts
const Device::VertexInputValues* Device::internVi(const VertexInputValues& v) {
    const auto same = [](const VertexInputValues& a, const VertexInputValues& b) {
        return a.bufferCount == b.bufferCount && a.dynamicLayout == b.dynamicLayout && a.bindingCount == b.bindingCount &&
               a.attributeCount == b.attributeCount && a.vertexShift == b.vertexShift &&
               memEqual(a.buffers, b.buffers, sizeof(VkBuffer) * a.bufferCount) &&
               memEqual(a.offsets, b.offsets, sizeof(VkDeviceSize) * a.bufferCount) &&
               memEqual(a.bindings, b.bindings, sizeof(a.bindings[0]) * a.bindingCount) &&
               memEqual(a.attributes, b.attributes, sizeof(a.attributes[0]) * a.attributeCount);
    };
    if (lastVi_ == nullptr || !same(*lastVi_, v)) lastVi_ = viArena_.push(v);
    return lastVi_;
}

const Device::DynamicStateValues* Device::internDyn(const DynamicStateValues& v) {
    const auto same = [](const DynamicStateValues& a, const DynamicStateValues& b) {
        constexpr std::size_t prefix = offsetof(DynamicStateValues, blendEnable);
        const std::uint32_t n = a.colorCount;
        return memEqual(&a, &b, prefix) && memEqual(a.blendEnable, b.blendEnable, sizeof(VkBool32) * n) &&
               memEqual(a.blendEquation, b.blendEquation, sizeof(VkColorBlendEquationEXT) * n) &&
               memEqual(a.writeMask, b.writeMask, sizeof(VkColorComponentFlags) * n);
    };
    if (lastDyn_ == nullptr || !same(*lastDyn_, v)) lastDyn_ = dynArena_.push(v);
    return lastDyn_;
}

void Device::submitPacket(DrawPacket& p) {
    p.vi = internVi(workVi_);
    p.dyn = internDyn(workDyn_);
    if (p.batchable && rendering_) {
        batch_.push_back(p);
        // with record threads a full segment goes to a worker at once (the draws of a pass are recorded while the engine goes on
        // producing them, not all at the scope's end)
        if (scopeSecondary_ && batch_.size() >= kChunkPackets) flushBatch();
        return;
    }
    flushBatch();  // the batch's draws come before this one
    if (scopeSecondary_ && rendering_) {
        emitPacket(p);
        return;
    }
    applyPacket(rec_, frames_.cmd(), bindless_.pipelineLayout(), p);
}

void Device::applyPacket(RecordState& rs, VkCommandBuffer cmd, VkPipelineLayout layout, const DrawPacket& p) {
    if (p.vi != rs.lastVi) {
        applyVertexInput(rs, cmd, *p.vi);
        rs.lastVi = p.vi;
    }
    if (p.pipeline != rs.bound) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
        rs.bound = p.pipeline;
        ++rs.pipelineBinds;
    }
    if (p.dyn != rs.lastDyn) {
        applyDynamicState(rs, cmd, *p.dyn);
        rs.lastDyn = p.dyn;
    }
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p.push), &p.push);
    if (p.op == DrawPacket::Op::DrawIndexed) {
        bindIndexBuffer(rs, cmd, p.indexBuffer, p.indexOffset, p.index32);
        vkCmdDrawIndexed(cmd, p.count, p.instances, p.first, p.vertexOffset, 0);
    } else {
        vkCmdDraw(cmd, p.count, p.instances, p.first, 0);
    }
}

// The pending batch: grouped by pipeline (stable: the engine's order within a pipeline), the first-seen pipeline first
void Device::flushBatch() {
    if (batch_.empty()) return;
    const std::uint32_t n = static_cast<std::uint32_t>(batch_.size());
    VkCommandBuffer cmd = frames_.cmd();
    const bool threaded = scopeSecondary_ && rendering_;
    const auto record = [&](const DrawPacket& p) {
        if (threaded) emitPacket(p);
        else applyPacket(rec_, cmd, bindless_.pipelineLayout(), p);
    };
    if (n <= 2) {
        for (std::uint32_t i = 0; i < n; ++i) record(batch_[i]);
    } else {
        // rank of each pipeline = the index of its first draw: groups keep the order in which their pipelines first appeared.
        // An open-addressing table of (pipeline, rank) and one sort of (rank << 32 | index) keys: stable by construction
        std::uint32_t cap = 256;
        while (cap < 2 * n) cap <<= 1;
        if (rankTable_.size() < cap) rankTable_.assign(cap, {});
        else std::fill_n(rankTable_.begin(), cap, RankEntry{});
        const std::uint32_t mask = cap - 1;
        batchKeys_.resize(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            const VkPipeline p = batch_[i].pipeline;
            std::uint32_t h = static_cast<std::uint32_t>((reinterpret_cast<std::uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> 32) & mask;
            while (rankTable_[h].pipeline != VK_NULL_HANDLE && rankTable_[h].pipeline != p) h = (h + 1) & mask;
            if (rankTable_[h].pipeline == VK_NULL_HANDLE) rankTable_[h] = {p, i};
            batchKeys_[i] = (static_cast<std::uint64_t>(rankTable_[h].rank) << 32) | i;
        }
        std::sort(batchKeys_.begin(), batchKeys_.end());
        for (const std::uint64_t k : batchKeys_) record(batch_[static_cast<std::uint32_t>(k)]);
    }
    if (threaded) closeChunk();  // a flush is one chunk: a worker starts on it now
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
