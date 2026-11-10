// d3d9::Device draws: rendering scopes, texture preparation, pipeline state, vertex input, dynamic state, constants.

#include <algorithm>
#include <bit>
#include <chrono>

#include "core/profile.h"
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <typeinfo>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "core/f5_measure.h"
#include "d3d9/convert.h"
#include "d3d9/device.h"
#include "gpuscene/cluster_table.h"
#include "d3d9/memeq.h"
#include "native/f2c_measure.h"
#include "shader/abi.h"

namespace rvk::d3d9 {
void diagAfterDraw(Device& d, const char* what);  // device.cpp (RVK_NAN_BISECT)

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
        if (scopeSecondary_) finishSecondaryScope();
        game_->endScope();  // (the backend ends the scope: waits for + executes the chunks, rebinds the frame's state)
        rendering_ = false;
        if (game_->timestampsEnabled()) {
            const std::uint32_t slot = game_->frameSlot();
            std::string label;
            if (game_->timestampDetails(slot)) {
                label = std::format("pass {}: {}x{}, {} colour ({}), depth {}, {} draws", passIndex_, targetWidth_, targetHeight_,
                                    colorCount_, colorCount_ > 0 ? static_cast<int>(colorFormats_[0]) : 0,
                                    static_cast<int>(depthFormat_), passDraws_);
            }
            game_->timestampMark(frameList(), slot, rhi::TimestampStream::Main, std::move(label));
        }
    }
}

bool Device::beginRendering(const ScopeClear* clear) {
    scopeClearFlags_ = 0;
    if (rendering_) {
        return true;
    }
    colorCount_ = 0;
    targetWidth_ = UINT32_MAX;
    targetHeight_ = UINT32_MAX;
    rhi::RenderingInfo ri{};
    for (std::uint32_t i = 0; i < kRenderTargets; ++i) {
        Surface* s = rt_[i];
        if (s == nullptr || s->texture()->isNull() || s->texture()->isDepth()) {
            continue;
        }
        Texture* t = s->texture();
        t->lastUseFrame_ = frameValue();
        rhi::RenderingInfo::Attachment& a = ri.color[colorCount_];
        a.texture = &t->rhi();
        a.mip = s->level();
        a.layer = s->face();
        colorFormats_[colorCount_] = t->rhi().desc().format;
        ++colorCount_;
        targetWidth_ = std::min(targetWidth_, s->width());
        targetHeight_ = std::min(targetHeight_, s->height());
    }
    ri.colorCount = colorCount_;
    // the thin G-buffer (enh/gbuffer): the targets a consumer reads (normal, motion) after the engine's target, cleared at the frame's
    // first use (motion to kGbufferMotionUnwritten: the fill pass writes the camera's motion where no material did)
    engineColorCount_ = colorCount_;
    scopeGbuf_ = false;
    if (colorCount_ == 1 && gbufferScope()) {
        const bool first = gbufWrittenFrame_ != frameValue();
        for (rhi::Texture* img : {gbuf_.normal, gbuf_.motion}) {
            if (img == nullptr) continue;
            rhi::RenderingInfo::Attachment& a = ri.color[colorCount_];
            a.texture = img;
            a.load = first ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            if (img == gbuf_.motion) a.clear.float32[0] = a.clear.float32[1] = gbuf_.motionClear;
            colorFormats_[colorCount_] = img->desc().format;
            ++colorCount_;
        }
        ri.colorCount = colorCount_;
        gbufWrittenFrame_ = frameValue();
        scopeGbuf_ = true;
    }
    depthFormat_ = VK_FORMAT_UNDEFINED;
    stencilFormat_ = VK_FORMAT_UNDEFINED;
    if (ds_ != nullptr && !ds_->texture()->isNull() && ds_->texture()->isDepth()) {
        Texture* t = ds_->texture();
        t->lastUseFrame_ = frameValue();
        rhi::RenderingInfo::Attachment& a = ri.depth;
        a.texture = &t->rhi();
        a.mip = ds_->level();
        a.layer = ds_->face();
        depthFormat_ = t->rhi().desc().format;
        // a D3D9 surface without stencil bits (D24X8, DF24, ...) may live in a D24S8 / D32S8 image: it gets no stencil attachment (the plane is
        // never loaded or stored; a stencil test there passes, as in D3D9)
        if (rhi::hasStencil(depthFormat_) && (t->format().stencil || bwBaseline())) {
            stencilFormat_ = depthFormat_;
        }
        ri.stencil = stencilFormat_ != VK_FORMAT_UNDEFINED;
        // D3D9 lets the depth buffer be larger than the render target
        targetWidth_ = colorCount_ > 0 ? targetWidth_ : ds_->width();
        targetHeight_ = colorCount_ > 0 ? targetHeight_ : ds_->height();
    }
    if (colorCount_ == 0 && depthFormat_ == VK_FORMAT_UNDEFINED) {
        // a NULL colour target without a depth buffer: nothing to render into
        targetWidth_ = targetHeight_ = 0;
        return false;
    }
    if (game_->timestampsEnabled()) {
        const std::uint32_t slot = game_->frameSlot();
        game_->timestampMark(frameList(), slot, rhi::TimestampStream::Main,
                       game_->timestampDetails(slot) ? std::format("barriers / copies / inline uploads before pass {}", passIndex_ + 1)
                                                     : std::string());
    }
    ++passIndex_;
    passDraws_ = 0;
    ri.width = targetWidth_;
    ri.height = targetHeight_;
    if (clear != nullptr && !bwBaseline()) {
        // D3D9 clears the viewport rectangle (and the scissor rect when the scissor test is on): when that is the whole render area, a load op
        // does it (the same rectangle clamp as Device::clear)
        std::int32_t x0 = static_cast<std::int32_t>(viewport_.x), y0 = static_cast<std::int32_t>(viewport_.y);
        std::int32_t x1 = x0 + static_cast<std::int32_t>(viewport_.width), y1 = y0 + static_cast<std::int32_t>(viewport_.height);
        if (rs_[RS_SCISSORTESTENABLE] != 0) {
            x0 = std::max(x0, scissor_[0]);
            y0 = std::max(y0, scissor_[1]);
            x1 = std::min(x1, scissor_[2]);
            y1 = std::min(y1, scissor_[3]);
        }
        if (x0 <= 0 && y0 <= 0 && x1 >= static_cast<std::int32_t>(targetWidth_) && y1 >= static_cast<std::int32_t>(targetHeight_)) {
            if ((clear->flags & CLEAR_TARGET) != 0 && engineColorCount_ > 0) {
                for (std::uint32_t i = 0; i < engineColorCount_; ++i) {
                    ri.color[i].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    std::memcpy(ri.color[i].clear.float32, clear->color, sizeof(clear->color));
                }
                scopeClearFlags_ |= CLEAR_TARGET;
            }
            if ((clear->flags & CLEAR_ZBUFFER) != 0 && depthFormat_ != VK_FORMAT_UNDEFINED) {
                ri.depth.load = VK_ATTACHMENT_LOAD_OP_CLEAR;
                ri.depthClear.depth = std::clamp(clearDepthValue(clear->z), 0.0f, 1.0f);
                scopeClearFlags_ |= CLEAR_ZBUFFER;
            }
            if ((clear->flags & CLEAR_STENCIL) != 0 && stencilFormat_ != VK_FORMAT_UNDEFINED) {
                ri.stencilLoad = VK_ATTACHMENT_LOAD_OP_CLEAR;
                ri.depthClear.stencil = clear->stencil & 0xFF;
                scopeClearFlags_ |= CLEAR_STENCIL;
            }
            if (scopeClearFlags_ != 0) ++stats_.scopeClears;
        }
    }
    scopeSecondary_ = wantSecondaryScope();
    game_->beginScope(ri, scopeSecondary_);  // (the backend transitions + views the attachments, secondary contents when threaded)
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
        const bool ready = !t->pending_ && t->layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (ready) {
            t->lastUseFrame_ = frame;
            continue;
        }
        if (t->lastUseFrame_ != frame && !t->isRenderTarget() && !t->isDepth()) {
            t->prepareForSampling(uploadList());
        } else {
            endRendering();
            t->prepareForSampling(frameList());
        }
        t->lastUseFrame_ = frame;
    }
    // material pack: the variant reflects the sky cube, which the engine draws into (a render target layout until something samples it)
    if (packProvider_) {
        if (Texture* cube = imageWithRole(scene::ImageRole::SkyEnvCube); cube != nullptr && !cube->isNull() &&
            (cube->pending_ || cube->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
            endRendering();
            cube->prepareForSampling(frameList());
        }
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
bool Device::sameDesc(const PipelineDesc& a, const PipelineDesc& b) const {
    if (game_->gameCaps().extendedDynamicState3 && game_->gameCaps().vertexInputDynamicState && a.bindingCount == 0 && a.attributeCount == 0 &&
        b.bindingCount == 0 && b.attributeCount == 0) {
        return memEqual(&a, &b, offsetof(PipelineDesc, blend));
    }
    return memEqual(&a, &b, sizeof(a));
}

std::size_t Device::recentSlot(const PipelineDesc& pd) {
    const std::uint64_t h = ((static_cast<std::uint64_t>(pd.vs) >> 4) * 0x9E3779B97F4A7C15ull ^
                             (static_cast<std::uint64_t>(pd.fs) >> 4) * 0xC2B2AE3D27D4EB4Full ^ pd.colorFormats[0] ^
                             (static_cast<std::uint64_t>(pd.depthFormat) << 8) ^ (static_cast<std::uint64_t>(pd.colorCount) << 16)) >>
                            40;
    return static_cast<std::size_t>(h % 256);
}

// the pipeline of `d` when it exists (a direct-mapped cache of recent descriptions first: no 520-byte hash, no lock), never builds
GamePipeline Device::findPipeline(const PipelineDesc& d) {
    RecentPipeline& recent = recentPipelines_[recentSlot(d)];
    if (recent.pipeline != GamePipeline::None && sameDesc(recent.desc, d)) return recent.pipeline;
    const GamePipeline p = game_->findPipeline(d);
    if (p != GamePipeline::None) {
        recent.desc = d;
        recent.pipeline = p;
    }
    return p;
}

// WP0 (d3d9/wp0.h): what a D3D12 draw path has to cover
void Device::wp0Note(const PipelineDesc& pd, std::uint32_t primitiveType, bool indexed, bool captureDraw, bool giDraw) {
    Wp0& w = wp0_;
    ++w.draws;
    switch (primitiveType) {
    case PT_POINTLIST:
        ++w.point;
        if (instanceCount_ > 1) ++w.pointInstanced;
        break;
    case PT_TRIANGLEFAN:
        ++w.fan;
        if (indexed) ++w.fanIndexed;
        break;
    case PT_TRIANGLESTRIP:
        ++w.strip;
        if (indexed) ++w.stripIndexed;
        break;
    case PT_LINELIST: ++w.lineList; break;
    case PT_LINESTRIP: ++w.lineStrip; break;
    default: break;
    }
    if (captureDraw) ++w.capture;
    if (giDraw) ++w.gi;
    if (gbufWrite_) ++w.gbuf;
    // the layout the draw would bake into a D3D12 PSO: the pipeline description's own (D3D12 has no dynamic vertex input) or the dynamic one
    const std::uint32_t nb = pd.bindingCount != 0 ? pd.bindingCount : workVi_.bindingCount;
    const std::uint32_t na = pd.attributeCount != 0 ? pd.attributeCount : workVi_.attributeCount;
    const rhi::VertexBinding* bs = pd.bindingCount != 0 ? pd.bindings : workVi_.bindings;
    const rhi::VertexAttribute* as = pd.attributeCount != 0 ? pd.attributes : workVi_.attributes;
    std::uint64_t layout = 0;
    for (std::uint32_t i = 0; i < nb; ++i) layout = Wp0::bytes(layout, bs[i]);
    for (std::uint32_t i = 0; i < na; ++i) {
        layout = Wp0::bytes(layout, as[i]);
        const char* name = nullptr;
        switch (as[i].format) {
        case VK_FORMAT_R8G8B8A8_USCALED: name = "UBYTE4 (R8G8B8A8_USCALED)"; break;
        case VK_FORMAT_R16G16_SSCALED: name = "SHORT2 (R16G16_SSCALED)"; break;
        case VK_FORMAT_R16G16B16A16_SSCALED: name = "SHORT4 (R16G16B16A16_SSCALED)"; break;
        case VK_FORMAT_A2B10G10R10_USCALED_PACK32: name = "UDEC3 (A2B10G10R10_USCALED)"; break;
        case VK_FORMAT_A2B10G10R10_SNORM_PACK32: name = "DEC3N (A2B10G10R10_SNORM)"; break;
        case VK_FORMAT_R32G32B32_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16B16A16_SFLOAT:
            break;
        default: name = "other"; break;
        }
        if (name != nullptr) ++w.exotic[name];
    }
    std::uint64_t base = 0;
    base = Wp0::mix(base, static_cast<std::uint64_t>(pd.vs));
    base = Wp0::mix(base, static_cast<std::uint64_t>(pd.fs));
    base = Wp0::mix(base, static_cast<std::uint64_t>(pd.topology));
    base = Wp0::mix(base, static_cast<std::uint64_t>(pd.polygonMode));
    base = Wp0::mix(base, pd.colorCount);
    for (std::uint32_t i = 0; i < pd.colorCount; ++i) {
        base = Wp0::mix(base, static_cast<std::uint64_t>(pd.colorFormats[i]));
        base = Wp0::bytes(base, attachmentBlend(i));
    }
    base = Wp0::mix(base, static_cast<std::uint64_t>(pd.depthFormat));
    base = Wp0::mix(base, static_cast<std::uint64_t>(pd.stencilFormat));
    std::uint64_t withLayout = Wp0::mix(base, layout);
    std::uint64_t full = withLayout;
    const DynamicStateValues& d = workDyn_;
    full = Wp0::mix(full, static_cast<std::uint64_t>(d.cull));
    full = Wp0::mix(full, static_cast<std::uint64_t>(d.depthTest) | static_cast<std::uint64_t>(d.depthWrite) << 1 | static_cast<std::uint64_t>(d.depthCompare) << 2);
    if (d.stencilTest != 0) {
        full = Wp0::bytes(full, d.stencilFront);
        full = Wp0::bytes(full, d.stencilBack);
        full = Wp0::mix(full, static_cast<std::uint64_t>(d.stencilFrontCompare) | static_cast<std::uint64_t>(d.stencilBackCompare) << 8);
        full = Wp0::mix(full, d.stencilMasks[0] | static_cast<std::uint64_t>(d.stencilMasks[1]) << 8);
    }
    if (d.depthBiasEnable != 0) {
        full = Wp0::bytes(full, d.depthBias);
    }
    if (decl_ != nullptr && decl_->hasBlendIndices() && w.layoutKeys.insert(withLayout).second && w.skinLogs++ < 8) {
        std::string d;
        for (std::uint32_t i = 0; i < na; ++i) d += std::format(" loc{}:b{}+{}:fmt{}", as[i].location, as[i].binding, as[i].offset, static_cast<int>(as[i].format));
        for (std::uint32_t i = 0; i < nb; ++i) d += std::format(" [b{} stride {} inst {}]", i, bs[i].stride, bs[i].perInstance);
        spdlog::info("wp0: skinned layout:{} (ubyte mask {:x}, instances {})", d, ubyteInputs_, instanceCount_);
    }
    w.baseKeys.insert(base);
    w.layoutKeys.insert(withLayout);
    w.fullKeys.insert(full);
}

void Device::wp0Log() {
    const Wp0& w = wp0_;
    spdlog::info("wp0: frame {}: {} draws counted; PSO keys: {} distinct shaders+targets+blend, {} with the vertex layout, {} with the raster/depth/stencil state (the D3D12 PSO count)",
                 stats_.frames, w.draws, w.baseKeys.size(), w.layoutKeys.size(), w.fullKeys.size());
    spdlog::info("wp0:   topology: point lists {} (instanced {}), triangle fans {} (indexed {}), strips {} (indexed {}), line lists {}, line strips {}",
                 w.point, w.pointInstanced, w.fan, w.fanIndexed, w.strip, w.stripIndexed, w.lineList, w.lineStrip);
    std::string ex;
    for (const auto& [k, n] : w.exotic) ex += std::format(" {} x{};", k, n);
    spdlog::info("wp0:   vertex attributes without a DXGI equivalent:{}", ex.empty() ? " none" : ex);
    spdlog::info("wp0:   variants: capture draws {}, gi draws {}, g-buffer-writing draws {}", w.capture, w.gi, w.gbuf);
}

bool Device::computeVertexInput(PipelineDesc& pd, VertexInputValues& out) {
    std::uint32_t streamBinding[kStreams];
    std::fill(std::begin(streamBinding), std::end(streamBinding), UINT32_MAX);
    std::uint32_t nullBinding = UINT32_MAX;
    pd.bindingCount = 0;
    pd.attributeCount = 0;
    ubyteInputs_ = 0;
    instanceCount_ = 1;
    if ((streams_[0].freq & STREAMSOURCE_INDEXEDDATA) != 0) {
        instanceCount_ = std::max(1u, streams_[0].freq & 0x3FFFFFFFu);
    }
    for (const VertexPlanInput& in : vertexPlan(decl_, *vs_).inputs) {
        if (in.stream != UINT32_MAX && (streams_[in.stream].buffer != nullptr || (in.stream == 0 && up_.active))) {
            std::uint32_t& b = streamBinding[in.stream];
            if (b == UINT32_MAX) {
                if (pd.bindingCount >= kMaxVertexBindings) {
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
                    if (g.buffer == 0) {
                        return false;
                    }
                    pd.bindings[b] = {st.stride, (st.freq & STREAMSOURCE_INSTANCEDATA) != 0 ? 1u : 0u};
                    out.buffers[b] = g.buffer;
                    out.offsets[b] = g.offset + st.offset;
                }
            }
            if (pd.attributeCount < kMaxVertexAttributes) {
                if (in.format == VK_FORMAT_R8G8B8A8_USCALED && in.location < 15) ubyteInputs_ |= 1u << in.location;
                pd.attributes[pd.attributeCount++] = {in.location, b, in.format, in.offset};
            }
        } else {
            // the declaration does not provide this input: D3D9 reads (0,0,0,1)
            if (nullBinding == UINT32_MAX) {
                if (pd.bindingCount >= kMaxVertexBindings) {
                    return false;
                }
                nullBinding = pd.bindingCount++;
                pd.bindings[nullBinding] = {0, 0};
                out.buffers[nullBinding] = game_->nullVertexBuffer();
                out.offsets[nullBinding] = 0;
            }
            if (pd.attributeCount < kMaxVertexAttributes) {
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
    out.dynamicLayout = game_->gameCaps().vertexInputDynamicState;
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

namespace {

BlendAttachment blendState(const std::array<std::uint32_t, RS_COUNT>& rs, std::uint32_t target) {
    static constexpr std::uint32_t writeStates[4] = {RS_COLORWRITEENABLE, RS_COLORWRITEENABLE1, RS_COLORWRITEENABLE2,
                                                     RS_COLORWRITEENABLE3};
    BlendAttachment b;
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
BlendAttachment Device::attachmentBlend(std::uint32_t index) const {
    if (index < engineColorCount_) return blendState(rs_, index);
    BlendAttachment b;
    b.writeMask = gbufWrite_ ? 0xFu : 0u;
    return b;
}

// The draw's dynamic state from the D3D9 render states (recorded by the backend's applyDynamicState)
void Device::computeDynamicState(std::uint32_t primitiveType, DynamicStateValues& o) const {
    // D3D9 screen space is y-down with a y-up clip space: flip with a negative viewport height
    const float vx = static_cast<float>(viewport_.x), vy = static_cast<float>(viewport_.y);
    const float vw = static_cast<float>(std::max(1u, viewport_.width)), vh = static_cast<float>(std::max(1u, viewport_.height));
    // depth range clamped to [0, 1] (D3D9 allows any floats, Vulkan without VK_EXT_depth_range_unrestricted only [0, 1]; DXVK clamps the same way)
    const float minZ = std::clamp(viewport_.minZ, 0.0f, 1.0f);
    o.viewport = VkViewport{vx, vy + vh, vw, -vh, minZ, std::clamp(viewport_.maxZ, minZ, 1.0f)};
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
    if (rs_[RS_STENCILENABLE] != 0 && depthFormat_ != VK_FORMAT_UNDEFINED) ++(stencilFormat_ != VK_FORMAT_UNDEFINED ? stats_.stencilPlaneDraws : stats_.stencilNoPlaneDraws);
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
    // reversed depth: the constant part is in the vertex remap (depthRemap, exact), the slope part stays here (a float format's
    // slope bias does not depend on the depth's exponent)
    o.depthBiasEnable = (!reversed && bias != 0.0f) || slope != 0.0f ? VK_TRUE : VK_FALSE;
    o.depthBias[0] = reversed ? 0.0f : bias * unit;
    o.depthBias[1] = reversed ? -slope : slope;
    colorToFloat4(rs_[RS_BLENDFACTOR], o.blendConstants);
    o.colorCount = 0;
    if (game_->gameCaps().extendedDynamicState3 && colorCount_ > 0) {
        o.colorCount = colorCount_;
        for (std::uint32_t i = 0; i < colorCount_; ++i) {
            const BlendAttachment b = attachmentBlend(i);
            o.blendEnable[i] = b.enable;
            o.blendEquation[i] = {b.srcColor, b.dstColor, b.colorOp, b.srcAlpha, b.dstAlpha, b.alphaOp};
            o.writeMask[i] = b.writeMask;
        }
    }
}

// the dynamic state itself is recorded by the backend (GameRecord::applyDynamicState): it sets only what differs from
// what this command buffer has already recorded

void Device::uploadConstants(bool forceLegacyPs, std::uint32_t psMinEnd) {
    // per dirty stage a StageConsts block (shader/abi.h) from the ring: the floats up to the shader's highest read register, the
    // int / bool part only for shaders that read it (Reflection::intBoolConstants; the layout stays, the bytes are left as they are)
    const auto upload = [&](const Shader& sh, const float (*f)[4], std::uint32_t maxFloats, const std::int32_t (*i)[4],
                            const std::uint32_t* b, rhi::GpuRef& addr, std::uint32_t stage) {
        rhi::RingAlloc a = game_->allocRing(shader::abi::kStageConstsSize, 16);
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
        addr = a.ref;
    };
    // split constants (abi.h windows, plan.md V2): a shader that reads its floats through the window blocks gets only those
    // windows, each uploaded when a register of it changed since its last upload (or when it was uploaded shorter than this
    // shader reads). Everything else (hand-written shaders, effects that read i# / b#, the GLSL material shader, replaced
    // draws with their own DrawBlock) keeps the one full block per stage
    // (S-1a: the HLSL material shader reads the same register windows as the translated one, RVK_PSC: it splits like any translated shader)
    drawSplit_[0] = splitConstants_ && vs_->reflection.splitConsts;
    drawSplit_[1] = splitConstants_ && ps_->reflection.splitConsts && !forceLegacyPs;
    psWindowMinEnd_ = psMinEnd;
    if (drawSplit_[0]) {
        splitStage(true, *vs_, drawWin_[0]);
        ++perf_.splitDraws[0];
    } else {
        ++perf_.legacyDraws[0];
        if (vsConstDirty_ || vsConstAddr_.null()) {
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
        if (psConstDirty_ || psConstAddr_.null()) {
            upload(*ps_, psF_, kPsFloatConsts, psI_, psB_, psConstAddr_, 1);
            psConstDirty_ = false;
        }
        for (auto& w : drawWin_[1]) w = psConstAddr_;
    }
}

// The window blocks of a split stage: windows[w] = the biased base address (block base - first register * 16, so c[n] keeps
// the D3D9 register number). Pixel window 1 (c5) is the DrawBlock's psC5, patched in by prepareDraw
void Device::splitStage(bool vertex, const Shader& sh, rhi::GpuRef (&windows)[shader::abi::kConstWindows]) {
    const std::uint32_t stage = vertex ? 0 : 1;
    const std::uint32_t* first = vertex ? shader::abi::kVsWindowFirst : shader::abi::kPsWindowFirst;
    const float (*regs)[4] = vertex ? vsF_ : psF_;
    ownedServed_[stage] = 0;
    if (owned_ != nullptr && !vertex) ++ownedStats_.draws;  // (counted once per draw: the pixel stage is split second)
    for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) {
        if (!vertex && w == 1) {
            windows[w] = {};
            continue;
        }
        std::uint32_t end = sh.reflection.windowEnd[w];
        if (!vertex && w == 3) end = std::max(end, psWindowMinEnd_);  // an enhancement variant's extra reads (ambient)
        const std::uint32_t need = end > first[w] ? end - first[w] : 0;
        if (need == 0) {
            if (zeroBlock_.null()) {
                rhi::RingAlloc z = game_->allocRing(4096, 16);
                std::memset(z.ptr, 0, 4096);
                zeroBlock_ = z.ref;
            }
            windows[w] = zeroBlock_.plus(-static_cast<std::int64_t>(first[w]) * 16);
            continue;
        }
        // window override: a persistent register image of the caller's (Material record, view block) instead of a ring block
        if (owned_ != nullptr) {
            const OwnedWindows::Win& ow = owned_->w[stage][w];
            if (ow.kind != OwnedWindows::Kind::None) {
                if (ow.covered >= need && !ow.base.null()) {
                    windows[w] = ow.base;
                    ownedServed_[stage] |= 1u << w;
                    if (ow.kind == OwnedWindows::Kind::Material) ++ownedStats_.material[stage];
                    else if (ow.kind == OwnedWindows::Kind::Lighting) ++ownedStats_.lighting;
                    else if (ow.kind == OwnedWindows::Kind::Object) ++ownedStats_.object;
                    else if (ow.kind == OwnedWindows::Kind::Bones) ++ownedStats_.bones;
                    else if (vertex && w == 1) ++ownedStats_.view1;
                    else ++ownedStats_.view[stage];
                    if (splitVerify_ && ow.image != nullptr && !ownedReplay_) verifyOwned(vertex, w, need, ow, regs, sh);
                    continue;
                }
                ++ownedStats_.fallbackShort;
                if (vertex && w == 1) ++ownedStats_.shortVs1;
                if (vertex && w == 2) ++ownedStats_.shortVs2;  // (the image is shorter than this shader reads: the normal block)
            } else if (ow.why == OwnedWindows::WhySwitch) {
                ++ownedStats_.fallbackSwitch;
            } else if (ow.why == OwnedWindows::WhyOther) {
                ++ownedStats_.fallbackOther;
            }
        }
        ConstWindow& cw = constWin_[stage][w];
        if ((constWinDirty_[stage] >> w & 1u) != 0 || cw.address.null() || cw.extent < need) {
            rhi::RingAlloc a = game_->allocRing(static_cast<VkDeviceSize>(need) * 16, 16);
            copyRegisters(a.ptr, regs[first[w]], need);
            cw.address = a.ref;
            cw.ptr = static_cast<const std::uint8_t*>(a.ptr);
            cw.extent = need;
            constWinDirty_[stage] &= ~(1u << w);
            ++perf_.splitBlocks[stage][w];
            perf_.splitBytes[stage][w] += static_cast<std::uint64_t>(need) * 16;
            perf_.constBytes += static_cast<std::uint64_t>(need) * 16;
            if (owned_ != nullptr) ++ownedStats_.uploaded;
        } else {
            ++perf_.splitReused[stage][w];
            if (owned_ != nullptr) ++ownedStats_.reused;
        }
        windows[w] = cw.address.plus(-static_cast<std::int64_t>(first[w]) * 16);
    }
    if (splitVerify_ && !ownedReplay_) verifySplit(vertex, sh);
}

// debug.split_verify, owned windows: the CPU twin of the image equals the register file for the registers the draw composed (verifyMask)
void Device::verifyOwned(bool vertex, std::uint32_t w, std::uint32_t need, const OwnedWindows::Win& ow, const float (*regs)[4], const Shader& sh) {
    const std::uint32_t* first = vertex ? shader::abi::kVsWindowFirst : shader::abi::kPsWindowFirst;
    ++ownedStats_.verified;
    bool bad = ow.image == nullptr;
    std::uint32_t reg = first[w];
    for (std::uint32_t i = 0; !bad && i < need && i < 32; ++i) {
        if ((ow.verifyMask >> i & 1u) == 0) continue;
        const bool wOnly = (ow.wOnlyMask >> i & 1u) != 0;
        if (std::memcmp(ow.image + static_cast<std::size_t>(i) * 4 + (wOnly ? 3 : 0), regs[first[w] + i] + (wOnly ? 3 : 0), wOnly ? 4 : 16) != 0) {
            bad = true;
            reg = first[w] + i;
        }
    }
    // (diagnostic: registers inside the shader's extent that the draw did not compose and whose image differs from the register file: the old path read the
    // device's stale value there, the table gives the record's)
    if (!bad) {
        for (std::uint32_t i = 0; i < need && i < 32; ++i) {
            if ((ow.verifyMask >> i & 1u) != 0 || (ow.ignoreMask >> i & 1u) != 0) continue;
            if (std::memcmp(ow.image + static_cast<std::size_t>(i) * 4, regs[first[w] + i], 16) != 0) {
                ++ownedStats_.uncomposedDiffer;
                if (ownedStats_.uncomposedDiffer <= 12) {
                    spdlog::warn("d3d9: owned window: {} window {} (shader {:016x}) register c{} is not composed by the draw and differs (image {:g} {:g} {:g} {:g}, file {:g} {:g} {:g} {:g}; need {}, mask {:#x})",
                                 vertex ? "VS" : "PS", w, sh.hash, first[w] + i, ow.image[i * 4], ow.image[i * 4 + 1], ow.image[i * 4 + 2], ow.image[i * 4 + 3],
                                 regs[first[w] + i][0], regs[first[w] + i][1], regs[first[w] + i][2], regs[first[w] + i][3], need, ow.verifyMask);
                }
                break;
            }
        }
    }
    if (bad) {
        ++ownedStats_.mismatches;
        if (perf_.splitMismatches++ < 24) {
            spdlog::error("d3d9: owned window: {} window {} (shader {:016x}) image differs from the register file at c{} (need {} registers, image holds {})",
                          vertex ? "VS" : "PS", w, sh.hash, reg, need, ow.covered);
        }
    }
}

// debug.split_verify: every window block this shader reads equals the register file now (dirty tracking, extents)
void Device::verifySplit(bool vertex, const Shader& sh) {
    const std::uint32_t stage = vertex ? 0 : 1;
    const std::uint32_t* first = vertex ? shader::abi::kVsWindowFirst : shader::abi::kPsWindowFirst;
    const float (*regs)[4] = vertex ? vsF_ : psF_;
    for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) {
        if (!vertex && w == 1) continue;
        if ((ownedServed_[stage] >> w & 1u) != 0) continue;  // (verified against its image: verifyOwned)
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
constexpr PipelineDesc kBlankPipelineDesc{};

// the render states a replacement overrides (restored when prepareDraw returns)
constexpr std::uint32_t kReplacedStates[] = {RS_ALPHABLENDENABLE, RS_SRCBLEND, RS_DESTBLEND, RS_BLENDOP, RS_SEPARATEALPHABLENDENABLE,
                                             RS_COLORWRITEENABLE, RS_CULLMODE, RS_ALPHATESTENABLE};

}  // namespace

// GI (enh/gi): the main view's lit material draws whose ambient the engine set (its first pass: the later point-light passes
// add with ONE / ONE blending and no ambient)
bool Device::giStaticEligible(const scene::Phase& phase) const {
    if (phase.view != scene::View::Main || tag_.water != draws::Water::None) return false;
    if (phase.pass != scene::Pass::Solid && phase.pass != scene::Pass::Alpha && phase.pass != scene::Pass::AlphaLate) return false;
    if ((ps_->reflection.colorOutputs & 1u) == 0) return false;
    const auto& amb = ps_->semantics[Semantic::AmbientLight];
    if (!amb.present() || amb.count < 7 || amb.reg + 7 > static_cast<int>(kPsFloatConsts)) return false;
    if (rs_[RS_ALPHABLENDENABLE] != 0 && rs_[RS_DESTBLEND] == BLEND_ONE) return false;
    return true;
}

// (only after giStaticEligible: the ambient semantic is present with 7 registers)
bool Device::ambientLit() const {
    const auto& amb = ps_->semantics[Semantic::AmbientLight];
    for (std::int32_t i = 0; i < 7; ++i) {
        const float* c = psF_[amb.reg + i];
        if (c[0] != 0.0f || c[1] != 0.0f || c[2] != 0.0f || c[3] != 0.0f) return true;
    }
    return false;
}

bool Device::giEligible(const scene::Phase& phase) const { return giStaticEligible(phase) && ambientLit(); }

// the material pack's word of the current draw's material (0 = none); its map must be uploaded and sampleable (DrawBlock pad[1])
std::uint32_t Device::packWord(CachedDraw::Variant* note) {
    // (skinned draws carry no material tag: the provider then reads the executing shader, which holds the derived gloss; word 0 without it)
    if (note != nullptr) {
        // (the stamp is read before the lookup: a snapshot published in between makes the packet look stale, never the other way round)
        note->packStamped = static_cast<bool>(packStamp_);
        note->packStamp = packStamp_ ? packStamp_() : 0;
    }
    const PackHit hit = packProvider_(tag_);
    if (note != nullptr) {
        note->packTex = hit.texture;
        note->packTexSlot = hit.texture != nullptr ? hit.texture->bindlessSlot() : 0;
    }
    if (hit.word != 0 && hit.texture != nullptr) {
        Texture* t = hit.texture;
        if (t->pending_ || t->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) t->prepareForSampling(uploadList());
        const std::uint32_t word = t->layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? hit.word : 0;
        static bool logged[2] = {false, false};
        const int which = word != 0 ? 1 : 0;
        if (!logged[which]) {
            logged[which] = true;
            spdlog::info("d3d9: material pack: first draw with a map {} (word {:08x}, slot {}, layout {})", which ? "applied" : "NOT READY", hit.word,
                         t->bindlessSlot(), static_cast<int>(t->layout()));
        }
        return word;
    }
    return hit.word;
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
bool Device::gbufferStaticEligible(const scene::Phase& phase, draws::Kind kind) const {
    if (phase.view != scene::View::Main || phase.pass != scene::Pass::Solid || tag_.water != draws::Water::None) return false;
    if (kind == draws::Kind::Sky || kind == draws::Kind::Particle || kind == draws::Kind::Precipitation) return false;
    if ((ps_->reflection.colorOutputs & 1u) == 0 || rs_[RS_ZENABLE] == ZB_FALSE) return false;
    if (rs_[RS_ALPHABLENDENABLE] != 0 && rs_[RS_DESTBLEND] == BLEND_ONE) return false;
    return true;
}

// when lit, a first pass (the engine set its ambient)
bool Device::gbufferLit() const {
    const auto& amb = ps_->semantics[Semantic::AmbientLight];
    if (!amb.present()) return ps_->semantics.has(ShaderSemantics::Material);
    if (amb.count < 7 || amb.reg + 7 > static_cast<int>(kPsFloatConsts)) return false;
    for (std::int32_t i = 0; i < 7; ++i) {
        const float* c = psF_[amb.reg + i];
        if (c[0] != 0.0f || c[1] != 0.0f || c[2] != 0.0f || c[3] != 0.0f) return true;
    }
    return false;
}

bool Device::gbufferEligible(const scene::Phase& phase, draws::Kind kind) const { return gbufferStaticEligible(phase, kind) && gbufferLit(); }

bool Device::prepareDraw(std::uint32_t primitiveType, const DrawGeom& geom) {
    lastRecord_ = nullptr;
    customVertices_ = 0;
    scenePipe_ = GamePipeline::None;
    scenePipe2_ = GamePipeline::None;
    packet_.scene = 0;  // (packet_ is reused: a scene draw's record index must not leak into the next draw's packet)
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
    if (!vs_ || !ps_ || vs_->module == rhi::ShaderModule::None || ps_->module == rhi::ShaderModule::None) {
        ++stats_.drawsSkipped;
        // a shader still compiling on the workers (a cold shader cache): the draw is left out, its shader goes to the front of the queue (the start menu's
        // UI first, the world while the save loads); no module and nothing compiling = fixed-function draws are not emulated / the compile failed
        bool compiling = false;
        for (Shader* sh : {vs_.get(), ps_.get()}) {
            if (sh != nullptr && sh->module == rhi::ShaderModule::None && sh->compiling.load(std::memory_order_acquire)) {
                prioritizeShader(*sh);
                compiling = true;
            }
        }
        if (compiling) ++stats_.skippedCompiling;
        else ++stats_.skippedNoShader;
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
        q.objectRows = ownedObjectRows();
    };
    // draw observers (enh/rt_scene, enh/gi): indexed draws from buffers of the kinds / phases they ask for
    const bool skipObs = skipObservers_;  // (a refused replay ran them already: their capture request stands)
    skipObservers_ = false;
    if (!skipObs) {
        captureRequest_ = {};
        captureRequestRw_ = {};
        captureInstances_ = false;
    }
    if (!skipObs && !observers_.empty() && geom.indexed && !geom.userPointer) {
        const bool f2cStatic = native::f2c::on() && kind == draws::Kind::StaticMesh && phase.view == scene::View::Main &&
                               (phase.pass == scene::Pass::Solid || phase.pass == scene::Pass::DepthPrepass);
        const CoverQuery cq{kind, phase, &tag_, geom.userPointer ? nullptr : streams_[0].buffer, geom.indexed && !geom.userPointer ? indices_ : nullptr,
                            streams_[0].offset, streams_[0].stride, primitiveType, geom.first, geom.primitives, geom.baseVertex,
                            (streams_[0].freq & STREAMSOURCE_INDEXEDDATA) != 0, rs_[RS_ALPHABLENDENABLE] != 0, vs_.get(), ps_.get(), tex_.data()};
        for (DrawObserver* o : observers_) {
            if (!o->observes(kind, phase)) continue;
            if (o->coversDraw(cq)) continue;  // F8-0: a persistent RT table entry holds what this draw would teach
            if (f2cStatic) native::f2c::noteObserver(typeid(*o).name(), 0);
            ReplaceQuery q;
            query(q);
            o->observe(*this, q);
        }
    }
    if (!skipObs && !replacers_.empty() && kind == draws::Kind::StaticMesh && phase.view == scene::View::Main && phase.pass == scene::Pass::Solid &&
        geom.indexed && !geom.userPointer) {
        for (DrawReplacer* r : replacers_) {
            if (!r->wantsOpaque()) continue;
            if (native::f2c::on()) native::f2c::noteReplacer(typeid(*r).name(), 1);
            ReplaceQuery q;
            query(q);
            r->observeOpaque(*this, q);
        }
    }
    DrawReplacement rep;
    for (DrawReplacer* r : replacers_) {
        if (!r->handles(kind, phase, tag_)) continue;
        if (native::f2c::on() && kind == draws::Kind::StaticMesh && phase.view == scene::View::Main &&
            (phase.pass == scene::Pass::Solid || phase.pass == scene::Pass::DepthPrepass)) {
            native::f2c::noteReplacer(typeid(*r).name(), 0);
        }
        ReplaceQuery q;
        query(q);
        r->replace(*this, q, rep);
        if ((rep.action == DrawReplacement::Action::Shaders || rep.action == DrawReplacement::Action::Procedural) &&
            (!rep.vs || !rep.ps || rep.vs->module == rhi::ShaderModule::None || rep.ps->module == rhi::ShaderModule::None)) {
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
            if (t->pending_ || t->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                endRendering();
                t->prepareForSampling(frameList());
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
    rhi::ShaderModule fs = ps_->module;
    if (!replaced && tint_ != draws::TintMode::Off && (ps_->reflection.colorOutputs & 1u) != 0) {
        tint = draws::tintColor(tint_, kind, phase.view, phase.pass);
        if (tint != 0) {
            const rhi::ShaderModule t = tintModule(*ps_);
            fs = t != rhi::ShaderModule::None ? t : fs;
            tint = t != rhi::ShaderModule::None ? tint : 0;
        }
    }

    // the GLSL material shader (Device::setUbershader): the plain module of a material pixel shader whose request is known
    rhi::ShaderModule uberBase = rhi::ShaderModule::None;  // (requested below, once it is known that no enhancement variant is wanted for the draw)

    // detail distances (lod.trees): the billboards' baked fade-in distances scaled with the trees' LOD
    rhi::ShaderModule vsModule = vs_->module;
    if (!replaced && kind == draws::Kind::SpeedTreeBillboard && billboardFadeScale_ != 1.0f) {
        const rhi::ShaderModule m = lodModule(*vs_);
        vsModule = m != rhi::ShaderModule::None ? m : vsModule;
    }
    // enhancement variants (built in the background; the engine's modules until they and their pipeline exist): GI reads the
    // probe volume instead of the engine's ambient constants (enh/gi), the vertex capture writes a skinned draw's world
    // positions for the RT scene (enh/rt_scene), the G-buffer variants write the thin G-buffer (enh/gbuffer)
    gbufWrite_ = false;
    CachedDraw::Variant cv;
    cv.giOn = giDraws_.on;
    cv.replaceLightmaps = giDraws_.replaceLightmaps;
    cv.pack = packProvider_ && !packBypass_;
    cv.gate = cv.giOn || cv.pack;
    cv.vsUsed = vsModule;
    rhi::ShaderModule fsVariant = rhi::ShaderModule::None, vsVariant = rhi::ShaderModule::None, fsPlain = rhi::ShaderModule::None;
    const bool notInstanced = (streams_[0].freq & STREAMSOURCE_INDEXEDDATA) == 0;
    if (!replaced && !captureRequest_.null() && geom.indexed && (notInstanced || captureInstances_) && vsModule == vs_->module) {
        vsVariant = variant(vs_, VariantSlot::Capture);
    }
    cv.gbufGate = gbuf_.active;
    cv.gbufStatic = !replaced && tint == 0 && cv.gbufGate && gbufferStaticEligible(phase, kind);
    const bool gbufMaterial = cv.gbufStatic && gbufferLit();
    if (gbufMaterial && scopeGbuf_) {
        cv.gbufWanted = true;
        fsPlain = variant(ps_, gbufferSlot(false));
        const rhi::ShaderModule motion =
            vsVariant != rhi::ShaderModule::None && gbuf_.motion != nullptr ? variant(ps_, gbufferSlot(true)) : rhi::ShaderModule::None;
        fsVariant = motion != rhi::ShaderModule::None ? motion : fsPlain;
    } else if (!replaced && tint == 0 && cv.gate) {
        cv.staticOk = giStaticEligible(phase);
        cv.wanted = cv.staticOk && ambientLit();
        if (cv.wanted) fsVariant = variant(ps_, VariantSlot::Gi);
    }
    cv.fsAsked = fsVariant;
    // (S-1a: the plain module is only built for draws no GI / pack / G-buffer variant is wanted for: those take the variant's own ubershader module)
    if (!replaced && tint == 0 && !cv.wanted && !cv.gbufWanted && ubershader_.load(std::memory_order_relaxed) && ps_->source.load(std::memory_order_acquire) != nullptr) {
        uberBase = variant(ps_, VariantSlot::Base);
    }

    // a copy of the blank description: `PipelineDesc pd{}` initialised the 520 bytes member by member (blend defaults are
    // not zero; ~1.5 % of the engine thread in the V4 profile)
    PipelineDesc pd = kBlankPipelineDesc;
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
        if (!game_->gameCaps().extendedDynamicState3) {
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
    const bool eds3 = game_->gameCaps().extendedDynamicState3;
    // a variant combination whose pipeline exists is taken; a missing one is built in the background (this draw keeps
    // the engine's shaders). `writes`: the pixel shader writes the G-buffer (without EDS3 its write mask is in the pipeline)
    const auto tryVariant = [&](rhi::ShaderModule vsM, rhi::ShaderModule fsM, bool writes) {
        PipelineDesc vd = pd;
        if (vsM != rhi::ShaderModule::None) vd.vs = vsM;
        if (fsM != rhi::ShaderModule::None) vd.fs = fsM;
        if (writes && !eds3) {
            for (std::uint32_t i = engineColorCount_; i < colorCount_; ++i) vd.blend[i].writeMask = 0xF;
        }
        if (findPipeline(vd) != GamePipeline::None) {
            pd = vd;
            return true;
        }
        game_->prewarmOncePipeline(vd);
        return false;
    };
    if (gbufMaterial && scopeGbuf_) {
        // the G-buffer material pass: capture VS + motion PS, else the plain G-buffer PS, else at least the capture
        if (vsVariant != rhi::ShaderModule::None && fsVariant != rhi::ShaderModule::None && tryVariant(vsVariant, fsVariant, true)) {
            captureDraw = gbufWrite_ = true;
        } else if (fsPlain != rhi::ShaderModule::None && tryVariant(rhi::ShaderModule::None, fsPlain, true)) {
            gbufWrite_ = true;
        } else if (vsVariant != rhi::ShaderModule::None && tryVariant(vsVariant, rhi::ShaderModule::None, false)) {
            captureDraw = true;
        }
        giDraw = gbufWrite_ && giDraws_.on && ps_->semantics[Semantic::AmbientLight].present();
    } else if (fsVariant != rhi::ShaderModule::None || vsVariant != rhi::ShaderModule::None) {
        if (tryVariant(vsVariant, fsVariant, false)) {
            giDraw = fsVariant != rhi::ShaderModule::None;
            captureDraw = vsVariant != rhi::ShaderModule::None;
        } else if (vsVariant != rhi::ShaderModule::None && fsVariant != rhi::ShaderModule::None) {
            if (tryVariant(vsVariant, rhi::ShaderModule::None, false)) {
                captureDraw = true;
            } else if (tryVariant(rhi::ShaderModule::None, fsVariant, false)) {
                giDraw = true;
            }
        }
    }
    // arming the G-buffer (Device::setGbuffer): this solid draw's pipeline with the attachments, built in the background
    if (gbuf_.active && gbuf_.attachments() > 0 && !gbufArmed_ && !scopeGbuf_ && colorCount_ == 1 && gbufferTargetsMatch()) {
        ++gbufSolid_;
        PipelineDesc md = pd;
        md.colorCount = 1;
        for (rhi::Texture* img : {gbuf_.normal, gbuf_.motion}) {
            if (img != nullptr) md.colorFormats[md.colorCount++] = img->desc().format;
        }
        bool writes = false;
        if (gbufMaterial) {
            const VariantSlot slot = gbufferSlot(captureDraw);
            const rhi::ShaderModule m = variant(ps_, slot);
            if (m != rhi::ShaderModule::None) {
                md.fs = m;
                writes = true;
            } else if (ps_->variantState[static_cast<std::size_t>(slot)].load() != 3) {
                ++gbufMissing_;  // its variant is still being built
            }
        }
        if (!eds3) {
            for (std::uint32_t i = 1; i < md.colorCount; ++i) {
                md.blend[i] = BlendAttachment{};
                md.blend[i].writeMask = writes ? 0xFu : 0u;
            }
        }
        if (!game_->pipelineContains(md)) {
            game_->prewarmOncePipeline(md);
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
    if (uberBase != rhi::ShaderModule::None && pd.fs == fs) {
        PipelineDesc ud = pd;
        ud.fs = uberBase;
        if (findPipeline(ud) != GamePipeline::None) {
            pd = ud;
        } else {
            game_->prewarmOncePipeline(ud);
        }
    }
    cv.uberAsked = uberBase;
    // consecutive draws mostly keep their pipeline: compare with the last description before hashing
    GamePipeline pipeline = GamePipeline::None;
    if (lastPipelineValid_ && sameDesc(pd, lastPipelineDesc_)) {
        pipeline = lastPipeline_;
    } else {
        // a direct-mapped cache of recent descriptions in front of the backend's pipeline map: no 520-byte hash, no lock, no node walk
        // (a pipeline never changes once made)
        pipeline = findPipeline(pd);
        if (pipeline == GamePipeline::None) {
            bool created = false;
            pipeline = game_->resolvePipeline(pd, &created);
            if (created && pipeline != GamePipeline::None) {
                recordPipelineKey(pd);
            }
            if (pipeline != GamePipeline::None) {
                RecentPipeline& recent = recentPipelines_[recentSlot(pd)];
                recent.desc = pd;
                recent.pipeline = pipeline;
            }
        }
        lastPipelineDesc_ = pd;
        lastPipeline_ = pipeline;
        lastPipelineValid_ = true;
    }
    if (pipeline == GamePipeline::None) {
        ++stats_.drawsSkipped;
        ++stats_.skippedPipeline;
        customVertices_ = 0;
        return false;
    }
    packet_.pipeline = pipeline;
    computeDynamicState(primitiveType, workDyn_);
    if (wp0_.on) wp0Note(pd, primitiveType, geom.indexed, captureDraw, giDraw);
    {
        // the pixel window of an enhancement variant that reads the ambient registers the base shader may not (enh/material_variant)
        std::uint32_t& minEnd = cv.minEnd;
        if (giDraw || gbufWrite_) {
            const auto& amb = ps_->semantics[Semantic::AmbientLight];
            if (amb.present() && amb.count >= 7) {
                minEnd = static_cast<std::uint32_t>(amb.reg + 7);
                const auto& ind = ps_->semantics[Semantic::IndoorAmbient];
                if (ind.present()) minEnd = std::max(minEnd, static_cast<std::uint32_t>(ind.reg + 1));
            }
        }
        uploadConstants(!rep.block.null() && replaced, minEnd);
    }

    shader::abi::DrawBlock block{};
    float jitterX = 0.0f, jitterY = 0.0f;
    geometryJitter(jitterX, jitterY);  // camera jitter of the main view's geometry (TAA / upscaling)
    const float lodBias = (jitterX != 0.0f || jitterY != 0.0f) ? geometryLodBias_ : 0.0f;
    cv.biasFor = geometryLodBias_;
    for (std::uint32_t mask = boundSamplerMask(); mask != 0; mask &= mask - 1) {
        const std::uint32_t slot = static_cast<std::uint32_t>(std::countr_zero(mask));
        const shader::Reflection& r = slot < 16 ? ps_->reflection : vs_->reflection;
        Texture* t = tex_[slot];
        const bool kindMatches = t != nullptr &&
                                 ((r.samplers[slot] == shader::TextureType::Cube) == (t->desc().type == TextureType::Cube)) &&
                                 ((r.samplers[slot] == shader::TextureType::Volume) == (t->desc().type == TextureType::Volume));
        const bool usable = t != nullptr && !t->isNull() && kindMatches &&
                            t->layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        block.tex[slot] = !usable ? game_->defaultTextureSlot(static_cast<std::uint32_t>(r.samplers[slot])) : (t == substituteFrom_ && substituteSlot_ != 0) ? substituteSlot_ : t->bindlessSlot();
        block.smp[slot] = lodBias != 0.0f && slot < 16 ? samplerIndexBiased(slot, lodBias) : samplerIndex(slot);
        if (capture_ != nullptr && slot < 16) {
            // (a retained packet serves frames with and without jitter: both sampler indexes)
            cv.smpPlain[slot] = samplerIndex(slot);
            cv.smpBias[slot] = geometryLodBias_ != 0.0f ? samplerIndexBiased(slot, geometryLodBias_) : cv.smpPlain[slot];
        }
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
    if (packProvider_ && !packBypass_ && (giDraw || gbufWrite_)) block.pad[1] = packWord(&cv);  // material pack: this material's word (0 = none)
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
    if (probe_ != nullptr) probeBlock_ = block;
    rhi::RingAlloc db = game_->allocRing(sizeof(block), 16);
    std::memcpy(db.ptr, &block, sizeof(block));
    lastKind_ = kind;  // (F5-1's clusterApply reads it in every mode; the rest of the retained-draw state is captured below only when a capture is asked for)
    if (capture_ != nullptr) {
        // a plain draw: no enhancement took part in it (the retained packet replays exactly this)
        // (a draw with an enhancement variant is retained with its decision: lastVariant_; one offered a variant that is not built / has no pipeline yet is not)
        const bool variantPending = (fsVariant != rhi::ShaderModule::None && !giDraw && !gbufWrite_) || (!retainVariants_ && (giDraw || cv.vsUsed != vs_->module));
        captureOk_ = !replaced && tint == 0 && !captureDraw && !variantPending && customVertices_ == 0 && geom.indexed && !geom.userPointer &&
                     rep.action == DrawReplacement::Action::Engine && (uberBase == rhi::ShaderModule::None || pd.fs != fs) && fs == ps_->module && captureRequest_.null();  // (S-1a: with the ubershader on, a draw that took a module of ours (it, or a variant) is retained; one still on the translated module while the ubershader's pipeline builds is not)
        if (!captureOk_) {
            ++replayStats.ncWhy[replaced ? 0 : tint != 0 ? 1 : captureDraw ? 2 : variantPending ? 3 : customVertices_ != 0 ? 5 : !geom.indexed || geom.userPointer ? 6
                                : rep.action != DrawReplacement::Action::Engine ? 7 : (uberBase != rhi::ShaderModule::None && pd.fs == fs) ? 8 : fs != ps_->module ? 9 : 10];
        }
        cv.giDraw = giDraw;
        cv.drawn = giDraw || gbufWrite_;
        lastVariant_ = cv;
        lastBlock_ = block;
        lastKind_ = kind;
        query(lastQuery_);
    }

    if (stats_.totalDraws < options_.logDraws) {
        std::string tex;
        for (std::uint32_t slot = 0; slot < kSamplerCount; ++slot) {
            if (block.tex[slot] != 0 || tex_[slot] != nullptr) {
                Texture* t = tex_[slot];
                tex += std::format(" s{}=slot{}({}x{} {} {} levels, pending {})", slot, block.tex[slot], t ? t->desc().width : 0,
                                   t ? t->desc().height : 0, t ? rhi::d3dFormatName(t->desc().format) : "-", t ? t->levels() : 0,
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
        (tint != 0 ? shader::abi::kDrawFlagTinted : 0u) | (options_.backend == rhi::Backend::D3D12 ? ubyteInputs_ << 17 : 0u);
    const rhi::GpuRef drawBlock = !rep.block.null() && replaced ? rep.block : db.ref;
    packet_.push = shader::abi::PushConstants{drawSplit_[0] ? 0 : vsConstAddr_.bits, drawSplit_[1] ? 0 : psConstAddr_.bits, drawBlock.bits, frameBlockAddress().bits,
                                              captureDraw ? captureRequestRw_.bits : 0, stats_.draws, flags};
    for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) {
        packet_.push.vsWin[w] = drawWin_[0][w].bits;
        packet_.push.psWin[w] = drawWin_[1][w].bits;
    }
    if (drawSplit_[1]) {
        // c5 = the DrawBlock's psC5 (this draw's alpha reference): biased like the other windows
        packet_.push.psWin[1] = db.ref.plus(static_cast<std::int64_t>(offsetof(shader::abi::DrawBlock, psC5)) - 5 * 16).bits;
    }
    // the opaque passes' draws may be recorded later, grouped by pipeline (flushBatch): their order only matters for surfaces
    // at exactly the same depth (docs/research/performance.md 12)
    {
        const draws::Kind k = kind;
        const bool meshKind = k == draws::Kind::StaticMesh || k == draws::Kind::InstancedMesh || k == draws::Kind::SpeedTree ||
                              k == draws::Kind::Skinned || (sceneBb_ && k == draws::Kind::SpeedTreeBillboard) || (sceneVeg_ && k == draws::Kind::Vegetation);
        const bool opaquePass = (phase.view == scene::View::Main && (phase.pass == scene::Pass::DepthPrepass || phase.pass == scene::Pass::Solid)) ||
                                (phase.view == scene::View::Shadow && phase.pass == scene::Pass::ShadowDepth);
        const VkCompareOp z = workDyn_.depthCompare;
        packet_.batchable = batching_ && meshKind && opaquePass && !replaced && tint == 0 && customVertices_ == 0 && geom.indexed &&
                            !geom.userPointer && rs_[RS_ALPHABLENDENABLE] == 0 && workDyn_.depthTest != 0 && workDyn_.stencilTest == 0 &&
                            (z == VK_COMPARE_OP_LESS || z == VK_COMPARE_OP_LESS_OR_EQUAL || z == VK_COMPARE_OP_EQUAL ||
                         z == VK_COMPARE_OP_GREATER || z == VK_COMPARE_OP_GREATER_OR_EQUAL);
    }
    // F2b-1: the main view's static draws issued by the static ops may be drawn as scene draws (scene_draws.cpp); submitPacket makes the packet
    if (sceneActive()) {
        sceneDecide(pd, giDraw, captureDraw, uberBase != rhi::ShaderModule::None && pd.fs == uberBase, kind, phase, !replaced && tint == 0 && customVertices_ == 0 && geom.indexed && !geom.userPointer);
    }
    ++stats_.draws;
    ++stats_.totalDraws;
    ++passDraws_;
    phases_.countDraw();
    const auto recordStart = options_.perf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    draws_.count(kind, phase.view);
    if (f5::enabled()) f5Note(primitiveType, geom, kind, phase);
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
        r->vsConsts = drawSplit_[0] ? drawWin_[0][2].bits : vsConstAddr_.bits;  // split draws: the object window (biased)
        r->psConsts = drawSplit_[1] ? drawWin_[1][3].bits : psConstAddr_.bits;
        r->drawBlock = drawBlock.bits;
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

// ---- packet retention ---------------------------------------------------------------------------------------------------

void Device::fillCapture(const DrawPacket& p) {
    CachedDraw& c = *capture_;
    c.valid = false;
    if (!captureOk_ || p.op != DrawPacket::Op::DrawIndexed || vs_.get() == nullptr || ps_.get() == nullptr) {
        ++replayStats.notCaptured;
        return;
    }
    c.packet = p;
    c.clusterWait = clusterWait_;
    c.clusterUsing = clusterUsing_;
    c.vi = *p.vi;
    c.dyn = *p.dyn;
    c.block = lastBlock_;
    c.query = lastQuery_;
    c.kind = lastKind_;
    c.phase = phases_.current();
    c.vs = vs_.get();
    c.ps = ps_.get();
    c.scenePipeline = scenePipe_;
    c.scenePipeline2 = scenePipe2_;
    c.vsModule = vs_->module;
    c.fsModule = ps_->module;
    c.v = lastVariant_;
    c.streamMask = 0;
    for (std::uint32_t i = 0; i < 4; ++i) {
        if (streams_[i].buffer == nullptr) continue;
        c.streamMask |= 1u << i;
        c.stream[i] = streams_[i].buffer->gpu(streams_[i].stride);
    }
    c.ib = indices_->gpu(indices_->index32() ? 4 : 2);
    c.texMask = 0;
    for (std::uint32_t mask = boundSamplerMask(); mask != 0; mask &= mask - 1) {
        const std::uint32_t slot = static_cast<std::uint32_t>(std::countr_zero(mask));
        Texture* t = tex_[slot];
        // only draws whose textures were all sampled as such (the block holds their bindless slots)
        if (t == nullptr || t->isNull() || t->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || t == substituteFrom_) {
            ++replayStats.notCaptured;
            return;
        }
        c.texMask |= 1u << slot;
        c.tex[slot] = t;
        c.texSlot[slot] = t->bindlessSlot();  // (not the block's: GI's replaced lightmaps are zero there)
    }
    c.env = {};
    c.env.vp = viewport_;
    std::memcpy(c.env.scissor, scissor_, sizeof(scissor_));
    c.env.colorCount = colorCount_;
    for (std::uint32_t i = 0; i < colorCount_ && i < kRenderTargets; ++i) c.env.colorFormats[i] = colorFormats_[i];
    c.env.depthFormat = depthFormat_;
    c.env.stencilFormat = stencilFormat_;
    c.env.depthReversed = boundDepthReversed() ? 1u : 0u;
    c.valid = replayAllowed(c);
    if (c.valid) ++replayStats.captured;
    else ++replayStats.notCaptured;
}

// F2c-2: the world rows (c33..c35) of the draw being prepared as the object table holds them (null: not an object-window draw); counted against the register file
const float* Device::ownedObjectRows() {
    if (owned_ == nullptr) return nullptr;
    const OwnedWindows::Win& w = owned_->w[0][2];
    if (w.kind != OwnedWindows::Kind::Object || w.image == nullptr) return nullptr;
    const float* rows = w.image + 8;  // image = c31 (5 float4): c33 starts at the third
    ++objectRowStats.given;
    if (ownedReplay_) return rows;  // (an owned draw has no composed register file to compare with)
    for (std::uint32_t r = 0; r < 3; ++r) {
        if (std::memcmp(rows + 4 * r, vsF_[33 + r], 16) != 0) {
            ++objectRowStats.differing;
            break;
        }
    }
    return rows;
}

bool Device::replayAllowed(const CachedDraw& c) const {
    if (tint_ != draws::TintMode::Off || (gbuf_.active && !gbufArmed_)) return false;  // (arming counts the solid draws in prepareDraw)
    if (stats_.totalDraws < options_.logDraws || wp0_.on || customVertices_ != 0 || up_.active) return false;
    // (the GI / material pack variant and the billboards' fade variant are part of the packet: its decision is re-derived by replayBuild)
    // observers (RT scene, GI, object motion) and opaque-observing replacers (water) are shown the cached query at replay (replayDraw)
    for (DrawReplacer* r : replacers_) {
        if (r->handles(c.kind, c.phase, tag_)) return false;
    }
    return true;
}

bool Device::replayBuild(const CachedDraw& c, const ReplayInputs& in, DrawPacket& p, shader::abi::DrawBlock& b) {
    const auto fail = [&](int why) {
        ++replayStats.why[why];
        replayStale_ = why == 0 || why == 5 || why == 6 || why == 7 || why == 10 || why == 11;  // the packet is out of date: made again by the caller
        return false;
    };
    if (!c.valid || vs_.get() != c.vs || ps_.get() != c.ps || vs_->module != c.vsModule || ps_->module != c.fsModule) return fail(0);
    if (phases_.current() != c.phase) return fail(1);
    if (!replayAllowed(c)) return fail(2);
    // the enhancement variant decision (prepareDraw): its inputs are the settings, the ambient registers (the engine's lighting of this draw) and the
    // variants' build state; a change in any of them makes the draw take another pipeline / pixel window / pack word
    if (c.v.giOn != giDraws_.on || c.v.replaceLightmaps != giDraws_.replaceLightmaps || c.v.pack != (packProvider_ && !packBypass_)) return fail(10);
    if (c.v.gbufGate != gbuf_.active) return fail(10);
    // S-1a: the plain ubershader module the draw was offered (its build / pipeline state moved since: the packet is made again)
    if (!c.v.wanted && !c.v.gbufWanted && uberAskedNow(*ps_) != c.v.uberAsked) return fail(10);  // (a draw that took a variant never asked for the plain module)
    // F2b-1: scene draws switched on / off, or the scene form of the draw was not ready when the packet was made: remade
    if (c.v.sceneActive != sceneActive() || (c.v.sceneActive && c.v.scenePending)) return fail(10);
    bool gbufTaken = false;
    if (c.v.gbufGate) {
        // the G-buffer's material pass (its variant reads the ambient too): taken when eligible, lit and in the G-buffer's rendering scope
        gbufTaken = c.v.gbufStatic && scopeGbuf_ && gbufferLit();
        if (gbufTaken != c.v.gbufWanted) return fail(10);
        if (gbufTaken && variant(ps_, gbufferSlot(false)) != c.v.fsAsked) return fail(10);
    }
    if (c.v.gate && !gbufTaken) {
        const bool wanted = c.v.staticOk && ambientLit();
        if (wanted != c.v.wanted) return fail(10);
        if (wanted && variant(ps_, VariantSlot::Gi) != c.v.fsAsked) return fail(10);
    }
    {
        rhi::ShaderModule vsM = vs_->module;
        if (c.kind == draws::Kind::SpeedTreeBillboard && billboardFadeScale_ != 1.0f) {
            const rhi::ShaderModule m = lodModule(*vs_);
            vsM = m != rhi::ShaderModule::None ? m : vsM;
        }
        if (vsM != c.v.vsUsed) return fail(10);
    }
    if (c.v.drawn && packProvider_) {
        const Texture* cube = imageWithRole(scene::ImageRole::SkyEnvCube);
        if (cube != nullptr && !cube->isNull() && (cube->pending_ || cube->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) return fail(11);
    }
    CachedDraw::Env env{};
    env.vp = viewport_;
    std::memcpy(env.scissor, scissor_, sizeof(scissor_));
    env.colorCount = colorCount_;
    for (std::uint32_t i = 0; i < colorCount_ && i < kRenderTargets; ++i) env.colorFormats[i] = colorFormats_[i];
    env.depthFormat = depthFormat_;
    env.stencilFormat = stencilFormat_;
    env.depthReversed = boundDepthReversed() ? 1u : 0u;
    if (std::memcmp(&env, &c.env, sizeof(env)) != 0) {
        static int logged = 0;
        if (logged < 8) {
            ++logged;
            spdlog::info("d3d9: replay env differs: viewport {},{} {}x{} vs {},{} {}x{}; scissor {},{},{},{} vs {},{},{},{}; colors {} vs {}; depth {} vs {}; stencil {} vs {}; reversed {} vs {}; z {} {} vs {} {}; phase {}/{}",
                         env.vp.x, env.vp.y, env.vp.width, env.vp.height, c.env.vp.x, c.env.vp.y, c.env.vp.width, c.env.vp.height, env.scissor[0], env.scissor[1],
                         env.scissor[2], env.scissor[3], c.env.scissor[0], c.env.scissor[1], c.env.scissor[2], c.env.scissor[3], env.colorCount, c.env.colorCount,
                         static_cast<int>(env.depthFormat), static_cast<int>(c.env.depthFormat), static_cast<int>(env.stencilFormat), static_cast<int>(c.env.stencilFormat),
                         env.depthReversed, c.env.depthReversed, env.vp.minZ, env.vp.maxZ, c.env.vp.minZ, c.env.vp.maxZ, scene::name(c.phase.view), scene::name(c.phase.pass));
        }
        return fail(3);
    }  // (Env is built from the same fields in the same order; padding is zeroed by value-init)
    float jx = 0.0f, jy = 0.0f;
    geometryJitter(jx, jy);
    const bool jit = jx != 0.0f || jy != 0.0f;
    if (c.v.biasFor != geometryLodBias_) return fail(10);
    // (the thickened / hashed cut-outs put the draw's alpha reference and the frame number into the block: not retained)
    if (jit && (alphaDither_ || cutoutThicken_ * std::clamp(renderRatio_ - 1.0f, 0.0f, 1.0f) > 0.0f)) return fail(4);
    for (std::uint32_t i = 0; i < 4; ++i) {
        if (((c.streamMask >> i) & 1u) == 0) continue;
        if (in.streams[i] == nullptr) return fail(5);
        const Buffer::Binding g = in.streams[i]->gpu(in.strides[i]);
        if (g.buffer != c.stream[i].buffer || g.offset != c.stream[i].offset) return fail(5);
    }
    if (in.indices == nullptr) return fail(6);
    const Buffer::Binding ib = in.indices->gpu(in.indices->index32() ? 4 : 2);
    if (ib.buffer != c.ib.buffer || ib.offset != c.ib.offset) return fail(6);
    // F5-1: captured while the range's cluster build was pending, and that entry has settled since (built / failed): made again (it takes the clustered copy now)
    if (c.clusterWait != 0 && gpuscene::clusterstore().stale(c.clusterWait, c.clusterUsing)) return fail(10);
    for (std::uint32_t mask = c.texMask; mask != 0; mask &= mask - 1) {
        const std::uint32_t slot = static_cast<std::uint32_t>(std::countr_zero(mask));
        Texture* t = c.tex[slot];
        if (t->isNull() || t->pending_ || t->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || t->bindlessSlot() != c.texSlot[slot]) {
            static int logged = 0;
            if (logged < 6) {
                ++logged;
                spdlog::info("d3d9: replay texture differs: slot {} null {} pending {} layout {} slot {} vs {}, role {}, kind {}", slot, t->isNull(), t->pending_.load(), static_cast<int>(t->layout()),
                             t->bindlessSlot(), c.texSlot[slot], scene::name(t->role()), draws::name(c.kind));
            }
            return fail(7);
        }
    }
    b = c.block;
    std::memcpy(b.psC5, psF_[5], sizeof(b.psC5));
    depthRemap(b.posFix[2], b.posFix[3]);
    // half a pixel of the viewport (+ the camera jitter, + the LOD bias of the jittered geometry: prepareDraw's arithmetic)
    b.posFix[0] = 1.0f / static_cast<float>(std::max(1u, c.env.vp.width));
    b.posFix[1] = -1.0f / static_cast<float>(std::max(1u, c.env.vp.height));
    b.posFix[0] += jx;
    b.posFix[1] += jy;
    if (geometryLodBias_ != 0.0f) {
        for (std::uint32_t mask = c.texMask & 0xFFFFu; mask != 0; mask &= mask - 1) {
            const std::uint32_t slot = static_cast<std::uint32_t>(std::countr_zero(mask));
            b.smp[slot] = jit ? c.v.smpBias[slot] : c.v.smpPlain[slot];
        }
    }
    if (c.v.drawn && c.v.pack) {
        // the material's word: unchanged while the pack's stamp is and its map is still sampleable in the same slot (else the exact lookup)
        const Texture* t = c.v.packTex;
        const bool same = c.v.packStamped && packStamp_ && packStamp_() == c.v.packStamp &&
                          (t == nullptr || (!t->pending_ && t->layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && t->bindlessSlot() == c.v.packTexSlot));
        if (!same) b.pad[1] = packWord();
    }
    p = c.packet;
    p.vi = internVi(c.vi);
    p.dyn = internDyn(c.dyn);
    return true;
}

bool Device::replayMatches(const CachedDraw& cached, const CachedDraw& fresh, const ReplayInputs& in, bool& valid, std::string& why) {
    DrawPacket p;
    shader::abi::DrawBlock b{};
    valid = replayBuild(cached, in, p, b);
    if (!valid) return true;
    const DrawPacket& f = fresh.packet;
    const auto bad = [&](const char* what) {
        why = what;
        return false;
    };
    if (p.pipeline != f.pipeline) return bad("pipeline");
    if (p.vi != f.vi) return bad("vertex input");
    if (p.dyn != f.dyn) return bad("dynamic state");
    if (p.op != f.op || p.index32 != f.index32 || p.batchable != f.batchable) return bad("packet kind / batchable");
    if (p.indexBuffer != f.indexBuffer || p.indexOffset != f.indexOffset || p.first != f.first || p.count != f.count || p.instances != f.instances ||
        p.vertexOffset != f.vertexOffset) {
        return bad("ranges");
    }
    if (p.push.flags != f.push.flags) return bad("push flags");
    {
        const CachedDraw::Variant& a = cached.v;
        const CachedDraw::Variant& r = fresh.v;
        if (a.gbufGate != r.gbufGate || a.gbufStatic != r.gbufStatic || a.gbufWanted != r.gbufWanted || a.drawn != r.drawn || a.gate != r.gate || a.giOn != r.giOn || a.replaceLightmaps != r.replaceLightmaps || a.pack != r.pack || a.staticOk != r.staticOk || a.wanted != r.wanted ||
            a.giDraw != r.giDraw || a.fsAsked != r.fsAsked || a.minEnd != r.minEnd || a.vsUsed != r.vsUsed) {
            return bad("variant decision");
        }
    }
    {
        // the observers' input: the cached query against the one the fresh draw showed them (tag / textures / mainView are refreshed at replay)
        const ReplaceQuery& a = cached.query;
        const ReplaceQuery& r = fresh.query;
        if (a.kind != r.kind || !(a.phase == r.phase) || a.vertexBuffer != r.vertexBuffer || a.vertexOffset != r.vertexOffset || a.vertexStride != r.vertexStride ||
            a.indexBuffer != r.indexBuffer || a.indexed != r.indexed || a.primitiveType != r.primitiveType || a.first != r.first || a.primitives != r.primitives ||
            a.baseVertex != r.baseVertex || a.minIndex != r.minIndex || a.numVertices != r.numVertices || a.instanced != r.instanced || a.instances != r.instances ||
            a.engineVs != r.engineVs || a.enginePs != r.enginePs || a.decl != r.decl || a.blended != r.blended || a.blending != r.blending || a.srcBlend != r.srcBlend ||
            a.dstBlend != r.dstBlend || a.alphaTest != r.alphaTest || a.alphaFunc != r.alphaFunc || a.alphaRef != r.alphaRef || a.depthWrite != r.depthWrite ||
            a.depthBias != r.depthBias) {
            return bad("observer query");
        }
        for (std::uint32_t mask = cached.texMask; mask != 0; mask &= mask - 1) {
            const std::uint32_t slot = static_cast<std::uint32_t>(std::countr_zero(mask));
            if (fresh.tex[slot] != cached.tex[slot]) return bad("observer textures");
        }
    }
    if (std::memcmp(&b, &fresh.block, sizeof(b)) != 0) return bad("draw block");
    return true;
}

bool Device::replayDraw(const CachedDraw& c, const ReplayInputs& in) {
    replayStale_ = false;
    if (!c.valid || vs_.get() == nullptr || ps_.get() == nullptr || vs_->module == rhi::ShaderModule::None || ps_->module == rhi::ShaderModule::None) {
        ++replayStats.refused;
        return false;
    }
    syncPhaseRegions();
    if (rendering_ && (scopeGbuf_ != gbufferScope() || scopeSecondary_ != wantSecondaryScope())) endRendering();
    if (!beginRendering()) {
        ++replayStats.why[9];
        ++replayStats.refused;
        return false;
    }
    DrawPacket p;
    shader::abi::DrawBlock b{};
    if (!replayBuild(c, in, p, b)) {
        ++replayStats.refused;
        return false;
    }
    {
        bool opaqueObserver = false;
        if (c.kind == draws::Kind::StaticMesh && c.phase.view == scene::View::Main && c.phase.pass == scene::Pass::Solid) {
            for (DrawReplacer* r : replacers_) opaqueObserver |= r->wantsOpaque();
        }
        if (!observers_.empty() || opaqueObserver) {
            ReplaceQuery q = c.query;
            q.tag = &tag_;
            q.textures = c.tex;
            q.mainView = firings_.mainViews();
            q.objectRows = ownedObjectRows();
            captureRequest_ = {};
            captureRequestRw_ = {};
            captureInstances_ = false;
            const bool f2cStatic = native::f2c::on() && c.kind == draws::Kind::StaticMesh && c.phase.view == scene::View::Main &&
                                   (c.phase.pass == scene::Pass::Solid || c.phase.pass == scene::Pass::DepthPrepass);
            const CoverQuery cq = coverOf(q);
            for (DrawObserver* o : observers_) {
                if (!o->observes(c.kind, c.phase)) continue;
                if (o->coversDraw(cq)) continue;  // F8-0
                if (f2cStatic) native::f2c::noteObserver(typeid(*o).name(), 1);
                o->observe(*this, q);
            }
            if (opaqueObserver) {
                for (DrawReplacer* r : replacers_) {
                    if (!r->wantsOpaque()) continue;
                    if (native::f2c::on()) native::f2c::noteReplacer(typeid(*r).name(), 1);
                    r->observeOpaque(*this, q);
                }
            }
            if (!captureRequest_.null()) {
                // an observer wants this draw's vertex capture (a different pipeline): the normal path draws it, without asking them again
                skipObservers_ = true;
                ++replayStats.why[8];
                ++replayStats.refused;
                return false;
            }
        }
    }
    const std::uint64_t frame = frameValue();
    for (std::uint32_t mask = c.texMask; mask != 0; mask &= mask - 1) c.tex[std::countr_zero(mask)]->lastUseFrame_ = frame;
    uploadConstants(false, c.v.minEnd);
    if (c.v.giDraw) ++giDrawCount_;
    rhi::RingAlloc db = game_->allocRing(sizeof(b), 16);
    std::memcpy(db.ptr, &b, sizeof(b));
    const std::uint32_t flags = p.push.flags;
    p.push = shader::abi::PushConstants{drawSplit_[0] ? 0 : vsConstAddr_.bits, drawSplit_[1] ? 0 : psConstAddr_.bits, db.ref.bits, frameBlockAddress().bits,
                                        0, stats_.draws, flags};
    for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) {
        p.push.vsWin[w] = drawWin_[0][w].bits;
        p.push.psWin[w] = drawWin_[1][w].bits;
    }
    if (drawSplit_[1]) p.push.psWin[1] = db.ref.plus(static_cast<std::int64_t>(offsetof(shader::abi::DrawBlock, psC5)) - 5 * 16).bits;
    ++stats_.draws;
    ++stats_.totalDraws;
    ++passDraws_;
    phases_.countDraw();
    draws_.count(c.kind, c.phase.view);
    ++replayStats.replayed;
    if (probe_ != nullptr) probeBlock_ = b;
    if (c.scenePipeline != GamePipeline::None && sceneActive() && p.batchable && rendering_ && ensureSceneRef()) {
        if (toScenePacket(p, c.scenePipeline)) ++sceneStats_.replayedSceneDraws;
        ++sceneStats_.candidates;
    }
    submitInterned(p);
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
                                                                        rhi::d3dFormatName(t->desc().format),
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
    struct Diag { Device& d; ~Diag() { diagAfterDraw(d, "plain"); } } diag{*this};
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

void Device::drawIndexedPrimitive(std::uint32_t type, std::int32_t baseVertex, std::uint32_t minIndex, std::uint32_t numVertices,
                                  std::uint32_t startIndex, std::uint32_t primitiveCount) {
    struct Diag { Device& d; ~Diag() { diagAfterDraw(d, "indexed"); } } diag{*this};
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
    if (ib.buffer == 0) {
        return;
    }
    packet_.op = DrawPacket::Op::DrawIndexed;
    packet_.index32 = index32;
    packet_.indexBuffer = ib.buffer;
    packet_.first = foldIndexOffset(ib.offset, index32, startIndex, packet_.indexOffset);
    packet_.count = vertexCount(type, primitiveCount);
    packet_.instances = instanceCount_;
    packet_.vertexOffset = baseVertex + vertexShift_;
    clusterWait_ = 0;
    clusterUsing_ = false;
    if (clusters_) clusterApply(type, baseVertex, minIndex, numVertices, startIndex, primitiveCount);  // F5-1: the clustered copy of a large static range (cluster_draw.cpp)
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
    const float k = reverseDepthK();
    zScale = -k - 1.0f;  // z' = z (1 + zScale) + zBias w = w - k z
    // D3D9's constant depth bias b is added to the D3D depth z / w: z' / w = 1 - k (z / w + b), so it is exact in this term.
    // In the rasterizer it was not: Vulkan scales a float format's constant bias by the exponent of the primitive's depth,
    // and reversed depth is small far away (n / viewZ), so a decal 100 m away kept ~1/1000 of its bias (computeDynamicState
    // leaves the constant part out for reversed buffers; DXVK uses VK_EXT_depth_bias_control's float representation, which
    // the AMD driver does not offer)
    zBias = 1.0f - k * asFloat(rs_[RS_DEPTHBIAS]);
}

void Device::setBatching(bool on) {
    if (on == batching_) return;
    flushBatch();
    batching_ = on;
}

// the used parts only: the work blocks keep stale bytes beyond the counts
const rhi::VertexInputValues* Device::internVi(const rhi::VertexInputValues& v) {
    const auto same = [](const rhi::VertexInputValues& a, const rhi::VertexInputValues& b) {
        return a.bufferCount == b.bufferCount && a.dynamicLayout == b.dynamicLayout && a.bindingCount == b.bindingCount &&
               a.attributeCount == b.attributeCount && a.vertexShift == b.vertexShift &&
               memEqual(a.buffers, b.buffers, sizeof(a.buffers[0]) * a.bufferCount) &&
               memEqual(a.offsets, b.offsets, sizeof(a.offsets[0]) * a.bufferCount) &&
               memEqual(a.bindings, b.bindings, sizeof(a.bindings[0]) * a.bindingCount) &&
               memEqual(a.attributes, b.attributes, sizeof(a.attributes[0]) * a.attributeCount);
    };
    if (lastVi_ == nullptr || !same(*lastVi_, v)) lastVi_ = viArena_.push(v);
    return lastVi_;
}

const rhi::DynamicStateValues* Device::internDyn(const rhi::DynamicStateValues& v) {
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
    if (capture_ != nullptr) fillCapture(p);
    if (scenePipe_ != GamePipeline::None) {
        toScenePacket(p, scenePipe_);
        scenePipe_ = GamePipeline::None;
    }
    submitInterned(p);
    p.scene = 0;
}

void Device::submitInterned(DrawPacket& p) {
    if (p.scene != 0 && !(p.batchable && rendering_)) {
        // (not expected: scene draws are only made for batchable draws inside a scope) a run of its own
        batch_.push_back(p);
        flushBatch();
        return;
    }
    if (p.batchable && rendering_) {
        batch_.push_back(p);
        // with record threads a full segment goes to a worker at once (the draws of a pass are recorded while the engine goes on
        // producing them, not all at the scope's end)
        // (scene draws: a longer batch = longer indirect runs; frame-ownership.md 7.11)
        if (scopeSecondary_ && batch_.size() >= (sceneRecs_.empty() ? kChunkPackets : 1024u)) flushBatch();
        return;
    }
    flushBatch();  // the batch's draws come before this one
    if (scopeSecondary_ && rendering_) {
        emitPacket(p);
        return;
    }
    game_->applyDrawPacket(p);
}

// the pending batch is recorded by the backend (game_->applyDrawPacket or emitPacket + chunks)

// The upload ring could not take a user-pointer draw's data (a ring chunk could not be created): the draw is dropped
void Device::flushBatch() {
    if (batch_.empty()) return;
    RVK_ZONE("Device::flushBatch");
    const std::uint32_t n = static_cast<std::uint32_t>(batch_.size());
    const bool threaded = scopeSecondary_ && rendering_;
    const auto record = [&](const DrawPacket& p) {
        if (threaded) emitPacket(p);
        else game_->applyDrawPacket(p);
    };
    // the packets in recording order; runs of scene packets become one indirect call each (scene_draws.cpp)
    const auto recordOrdered = [&](auto&& index, std::uint32_t count) {
        if (sceneRecs_.empty()) {
            for (std::uint32_t i = 0; i < count; ++i) record(batch_[index(i)]);
            return;
        }
        sceneRun_.clear();
        const auto flushRun = [&] {
            if (sceneRun_.empty()) return;
            ++sceneStats_.groups;
            sceneFlushRun(sceneRun_, record);
            sceneRun_.clear();
        };
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t at = index(i);
            const DrawPacket& p = batch_[at];
            if (p.scene == 0) {
                flushRun();
                record(p);
                continue;
            }
            if (!sceneRun_.empty()) {
                const int why = sceneMergeBreak(batch_[sceneRun_[0]], p);
                if (why >= 0 && why < 5) ++sceneStats_.breaks[why];
                if (why >= 0) flushRun();
                else if (sceneRun_.size() >= 1024) {
                    ++sceneStats_.breaks[4];
                    flushRun();
                }
            }
            sceneRun_.push_back(at);
        }
        flushRun();
    };
    if (n <= 2) {
        recordOrdered([](std::uint32_t i) { return i; }, n);
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
            const GamePipeline p = batch_[i].pipeline;
            std::uint32_t h = static_cast<std::uint32_t>((static_cast<std::uint64_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> 32) & mask;
            while (rankTable_[h].pipeline != GamePipeline::None && rankTable_[h].pipeline != p) h = (h + 1) & mask;
            if (rankTable_[h].pipeline == GamePipeline::None) rankTable_[h] = {p, i};
            batchKeys_[i] = (static_cast<std::uint64_t>(rankTable_[h].rank) << 32) | i;
        }
        std::sort(batchKeys_.begin(), batchKeys_.end());
        recordOrdered([&](std::uint32_t i) { return static_cast<std::uint32_t>(batchKeys_[i]); }, n);
    }
    sceneRecs_.clear();
    if (threaded) closeChunk();  // a flush is one chunk: the backend's workers start on it now
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
    rhi::RingAlloc a = game_->allocRing(static_cast<VkDeviceSize>(vertices) * stride, 16);
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
    rhi::RingAlloc ia = game_->allocRing(static_cast<VkDeviceSize>(count) * isize, 16);
    const std::uint32_t vcount = minIndex + numVertices;
    rhi::RingAlloc va = game_->allocRing(static_cast<VkDeviceSize>(vcount) * stride, 16);
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
