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
#include "engine/scene_mirror.h"
#include "enh/native_shadows.h"
#include "shader/embedded.h"
#include "vk/image.h"

namespace rvk::enh {

namespace {

namespace mats = engine::materials;

constexpr std::uint32_t kInstanceDataBase = 32;  // g_vInstanceData: c32 (header), the plain draw's world rows c33..c35
constexpr std::uint32_t kFlagIndex32 = 1, kFlagMasked = 2, kFlagNoAlbedo = 4, kFlagLightmap = 8, kFlagAmbComplex = 16, kFlagNormalMap = 32,
                        kFlagDxt5nm = 64, kFlagNoAmbient = 128, kFlagNoSun = 256, kFlagNoMask = 512;
constexpr std::uint32_t kPsConstants = 48;
constexpr std::uint8_t kUsagePosition = 0, kUsageNormal = 3, kUsageTexcoord = 5, kUsageTangent = 6, kUsageColor = 10;
constexpr std::uint8_t kTypeFloat2 = 1, kTypeFloat3 = 2, kTypeColor = 4;

enum Reason : std::size_t { kNotPlain, kBuffers, kBlended, kDecl, kMaterial, kAlign, kBaseVertex, kOther, kReasons };
constexpr const char* kReasonNames[kReasons] = {"not a plain indexed triangle list", "dynamic / missing buffers", "blended / state", "declaration",
                                               "material unreadable", "unaligned vertices", "base vertex", "other"};

struct Layout {
    bool positionOk = false;
    std::uint32_t normal = 0xFFFFu, tangent = 0xFFFFu, color0 = 0xFFFFu, color1 = 0xFFFFu, lightmap = 0xFFFFu, uv[2]{0xFFFFu, 0xFFFFu};
};

Layout layoutOf(const d3d9::VertexDecl* decl) {
    Layout l;
    if (decl == nullptr) return l;
    for (const d3d9::VertexElement& e : decl->elements()) {
        if (e.stream == d3d9::kDeclEndStream) break;
        if (e.stream != 0) continue;
        if (e.usage == kUsagePosition && e.usageIndex == 0 && e.type == kTypeFloat3 && e.offset == 0) l.positionOk = true;
        else if (e.usage == kUsageNormal && e.usageIndex == 0 && e.type == kTypeFloat3) l.normal = e.offset;
        else if (e.usage == kUsageTangent && e.usageIndex == 0 && e.type == kTypeFloat3) l.tangent = e.offset;
        else if (e.usage == kUsageColor && e.usageIndex == 0 && e.type == kTypeColor) l.color0 = e.offset;
        else if (e.usage == kUsageColor && e.usageIndex == 1 && e.type == kTypeColor) l.color1 = e.offset;
        else if (e.usage == kUsageTexcoord && e.usageIndex < 2 && e.type == kTypeFloat2) l.uv[e.usageIndex] = e.offset;
        else if (e.usage == kUsageTexcoord && e.usageIndex == 6 && e.type == kTypeFloat2) l.lightmap = e.offset;
    }
    return l;
}

std::uint64_t partKey(const void* entity, const void* subMesh) {
    return (reinterpret_cast<std::uintptr_t>(entity) * 0x9E3779B97F4A7C15ull) ^ reinterpret_cast<std::uintptr_t>(subMesh);
}

}  // namespace

NativeOpaque::NativeOpaque(d3d9::Device& device) : dev_(device) {}

NativeOpaque::~NativeOpaque() {
    apply(false);
    destroyPipeline();
}

void NativeOpaque::destroyPipeline() {
    vk::Context* ctx = &dev_.ctx();
    const VkPipeline a = pipeline_, b = pipelineMasked_;
    vk::PassLayout layout = layout_;
    if (a != VK_NULL_HANDLE || b != VK_NULL_HANDLE || layout.valid()) {
        dev_.frames().retire([ctx, a, b, layout]() mutable {
            for (VkPipeline p : {a, b}) {
                if (p != VK_NULL_HANDLE) vkDestroyPipeline(ctx->device(), p, nullptr);
            }
            layout.destroy(*ctx);
        });
    }
    pipeline_ = pipelineMasked_ = VK_NULL_HANDLE;
    layout_ = vk::PassLayout{};
    colorFormat_ = depthFormat_ = VK_FORMAT_UNDEFINED;
}

// the depth / cull state of a debug mode: both on, except 9 = depth test only, 10 = culling only (the albedo views that isolated the culling fault)
static std::uint32_t stateOf(std::uint32_t mode) { return mode == 9 ? 2u : mode == 10 ? 3u : 0u; }

void NativeOpaque::setDebug(std::uint32_t mode) {
    debug_ = mode;
    if (pipeline_ != VK_NULL_HANDLE && pipelineState_ != stateOf(mode)) destroyPipeline();  // rebuilt with the other state at the next pass
}

void NativeOpaque::apply(bool enabled) {
    enabled_ = enabled && mats::available();
    if (enabled_ && !registered_) {
        dev_.addDrawReplacer(this);
        passId_ = dev_.addPass(d3d9::inject::Point::AfterOpaque, "native.opaque", [this](d3d9::PassContext& c) { pass(c); }, this, 10);
        registered_ = true;
        failed_ = false;
        spdlog::info("enh: native opaque: on (static meshes drawn by shaders/native/static_mesh.*)");
        if (listMode_ == 1) setListMode(1);
    } else if (!enabled_ && registered_) {
        dev_.removeDrawReplacer(this);
        dev_.removePass(passId_);
        passId_ = 0;
        if (listPassId_ != 0) {
            dev_.removePass(listPassId_);
            listPassId_ = 0;
        }
        registered_ = false;
        items_.clear();
        mirrorItems_.clear();
        listIndex_.clear();
        listSeen_.clear();
        spdlog::info("enh: native opaque: off");
    }
}

bool NativeOpaque::handles(d3d9::draws::Kind kind, d3d9::scene::Phase phase, const d3d9::draws::Tag& tag) const {
    return enabled_ && !failed_ && kind == d3d9::draws::Kind::StaticMesh && phase.view == d3d9::scene::View::Main &&
           phase.pass == d3d9::scene::Pass::Solid && tag.entity != nullptr && tag.subMesh != nullptr && tag.material != nullptr;
}

void NativeOpaque::replace(d3d9::Device& device, const d3d9::ReplaceQuery& q, d3d9::DrawReplacement& out) {
    out.action = d3d9::DrawReplacement::Action::Engine;
    const std::uint64_t frame = device.frameValue();
    if (frame != itemsFrame_) {
        items_.clear();
        itemsFrame_ = frame;
    }
    // what the engine's static mesh draws use: the cull mode per material (the mirror list takes it), the per-view constants (fog, the shadow mask),
    // the permutations (to settle the native rules)
    if (q.tag != nullptr && q.tag->material != nullptr && q.enginePs != nullptr) {
        const VkCullModeFlags cm = d3d9::cullMode(device.renderState(d3d9::RS_CULLMODE));
        cullByMaterial_[q.tag->material] = cm;
        ++stats_.cullModes[cm == VK_CULL_MODE_NONE ? 0 : cm == VK_CULL_MODE_FRONT_BIT ? 1 : 2];
        std::memcpy(view_.fog[0], device.psConstant(3), 16);
        std::memcpy(view_.fog[1], device.psConstant(4), 16);
        if (q.textures != nullptr && q.textures[0] != nullptr && !q.textures[0]->isNull() && q.textures[0]->kind() == vk::TextureKind::Tex2D) {
            view_.mask = q.textures[0];
            view_.maskSampler = device.samplerOfSlot(0);
        }
        view_.frame = frame;
        const auto& sm = q.enginePs->semantics;
        ++stats_.permLmAmb[sm[d3d9::Semantic::LightMap1].present() ? 1 : 0][sm[d3d9::Semantic::AmbientLight].present() ? 1 : 0];
        ++stats_.permMask[sm[d3d9::Semantic::ShadowMask].present() ? 1 : 0];
        ++stats_.permSun[sm.has(d3d9::ShaderSemantics::DirLight) ? 1 : 0];
        if (q.alphaTest) ++stats_.alphaFunc[std::min<std::uint32_t>(q.alphaFunc, 8u)];
    }
    if (listMode_ == 1) {
        // the list comes from the mirror: the engine's draw of a listed part is replaced by ours (compared with what we composed), the rest stays the engine's
        if (q.tag != nullptr && q.tag->subMesh != nullptr) {
            if (const auto it = listIndex_.find(partKey(q.tag->entity, q.tag->subMesh)); it != listIndex_.end()) {
                compareLighting(mirrorItems_[it->second], device);
                listSeen_[it->second] = 1;
                out.action = d3d9::DrawReplacement::Action::Skip;
                ++stats_.taken;
                return;
            }
        }
        ++stats_.listEngineDrew;
        return;
    }
    const auto decline = [&](Reason r) { ++stats_.declined[r]; };
    if (!q.indexed || q.instanced || q.primitiveType != 4 || q.primitives == 0 || q.tag == nullptr) return decline(kNotPlain);
    if (q.vertexBuffer == nullptr || q.indexBuffer == nullptr || q.vertexBuffer->ringCopied() || q.indexBuffer->ringCopied()) return decline(kBuffers);
    if (q.blending) return decline(kBlended);
    if (q.baseVertex != 0) return decline(kBaseVertex);
    if (q.vertexStride < 12 || q.vertexStride % 4 != 0 || q.vertexOffset % 4 != 0) return decline(kAlign);
    const Layout layout = layoutOf(q.decl);
    if (!layout.positionOk || q.enginePs == nullptr || q.textures == nullptr) return decline(kDecl);
    // the material graph of the draw's material: its roles (diffuse, normal) and the blend mode (no draw state needed)
    mats::ShaderInfo info;
    if (!mats::active(info, q.tag->material) || (info.cls != mats::ShaderClass::Default && info.cls != mats::ShaderClass::Leafs)) return decline(kMaterial);
    if (info.blendMode > 1) return decline(kBlended);  // 0 normal, 1 masked: the rest is blended
    Item it;
    NativeItemBlock& blk = it.block;
    const auto usable = [](const d3d9::Texture* t) { return t != nullptr && !t->isNull() && t->kind() == vk::TextureKind::Tex2D; };
    const auto bound = [&](int slot) -> d3d9::Texture* {
        return slot >= 0 && slot < 16 && usable(q.textures[slot]) ? q.textures[slot] : nullptr;
    };
    std::uint32_t flags = 0;
    // masked: the engine's alpha test (func / reference)
    it.masked = q.alphaTest || info.blendMode == 1;
    if (it.masked) {
        flags |= kFlagMasked;
        blk.params[0] = q.alphaTest ? static_cast<float>(q.alphaRef) : static_cast<float>(info.maskReference);
        blk.off1[3] |= (q.alphaTest ? q.alphaFunc : 7u) << 8;  // a masked material without the test state: >= the reference
    }
    // diffuse: the texture the engine bound at the graph's slot, else the graph's pool image
    if (info.diffuse.source == mats::Source::Sampler && info.diffuse.slot >= 0) {
        const int slot = info.samplerBase + info.diffuse.slot;
        it.diffuse = bound(slot);
        if (it.diffuse != nullptr) {
            it.diffuseSampler = device.samplerOfSlot(static_cast<std::uint32_t>(slot));
        } else if (info.diffuse.image != nullptr) {
            d3d9::Texture* t = device.engineTexture(mats::apiTextureOf(info.diffuse.image));
            if (usable(t)) {
                it.diffuse = t;
                it.diffuseSampler = device.samplerFor(true, false);
            }
        }
    }
    if (it.diffuse == nullptr) flags |= kFlagNoAlbedo;
    if (info.normal.source == mats::Source::Sampler && info.normal.slot >= 0) {
        const int slot = info.samplerBase + info.normal.slot;
        it.normal = bound(slot);
        if (it.normal != nullptr) {
            it.normalSampler = device.samplerOfSlot(static_cast<std::uint32_t>(slot));
            flags |= kFlagNormalMap | kFlagDxt5nm;  // the engine converts its normal maps to DXT5nm (x in alpha, y in green)
        }
    }
    // the screen-space shadow mask (s0) and the directional lightmaps (s4 / s5)
    it.mask = bound(0);
    if (it.mask != nullptr) it.maskSampler = device.samplerOfSlot(0);
    const auto& sem = q.enginePs->semantics;
    if (sem[d3d9::Semantic::LightMap1].present() && sem[d3d9::Semantic::LightMap2].present() && layout.lightmap != 0xFFFFu) {
        it.lightmap[0] = bound(4);
        it.lightmap[1] = bound(5);
        if (it.lightmap[0] != nullptr && it.lightmap[1] != nullptr) {
            flags |= kFlagLightmap;
            it.lightmapSampler = device.samplerOfSlot(4);
        } else {
            it.lightmap[0] = it.lightmap[1] = nullptr;
        }
    }
    // what the pixel shader permutation reads (its constant table lists only what is used): the sun, the mask, the SH ambient (3 or 7
    // registers), the point lights (4 registers each, up to 3)
    if (!sem.has(d3d9::ShaderSemantics::DirLight)) flags |= kFlagNoSun;
    if (!sem[d3d9::Semantic::ShadowMask].present()) flags |= kFlagNoMask;
    const auto& amb = sem[d3d9::Semantic::AmbientLight];
    if (!amb.present()) flags |= kFlagNoAmbient;
    else if (amb.count >= 7) flags |= kFlagAmbComplex;
    const auto& pnt = sem[d3d9::Semantic::PointLights];
    const std::uint32_t pointCount = !pnt.present() ? 0u : std::min<std::uint32_t>(pnt.count >= 4 ? pnt.count / 4u : pnt.count, 3u);
    blk.off1[3] |= pointCount;
    for (std::uint32_t r = 0; r < kPsConstants; ++r) std::memcpy(blk.ps[r], device.psConstant(r), sizeof(float) * 4);
    // the buffers' static GPU copies (the engine draw would make them: it is skipped)
    q.vertexBuffer->gpu(q.vertexStride);
    q.indexBuffer->gpu(q.indexBuffer->index32() ? 4 : 2);
    const VkDeviceAddress vb = q.vertexBuffer->deviceAddress(), ib = q.indexBuffer->deviceAddress();
    if (vb == 0 || ib == 0) return decline(kBuffers);
    it.vertices = vb + q.vertexOffset;
    it.indices = ib;
    it.first = q.first;
    it.indexCount = q.primitives * 3;
    if (q.indexBuffer->index32()) flags |= kFlagIndex32;
    blk.tex2[1] = flags;
    blk.tex2[2] = q.vertexStride;
    blk.off0[0] = layout.normal;
    blk.off0[1] = layout.tangent;
    blk.off0[2] = layout.color0;
    blk.off0[3] = layout.color1;
    const std::uint32_t uvSet = std::min<std::uint32_t>(info.diffuse.uvSet, 1u);
    blk.off1[0] = layout.uv[uvSet] != 0xFFFFu ? layout.uv[uvSet] : layout.uv[0];
    const std::uint32_t nSet = std::min<std::uint32_t>(info.normal.uvSet, 1u);
    blk.off1[1] = layout.uv[nSet] != 0xFFFFu ? layout.uv[nSet] : layout.uv[0];
    blk.off1[2] = layout.lightmap;
    for (std::uint32_t r = 0; r < 3; ++r) std::memcpy(blk.row[r], device.vsConstant(kInstanceDataBase + 1 + r), sizeof(float) * 4);
    it.cull = d3d9::cullMode(device.renderState(d3d9::RS_CULLMODE));
    it.entity = q.tag->entity;
    it.subMesh = q.tag->subMesh;
    // the mirror's matrix of the entity against the draw's rows (the next stage takes the transform from the mirror)
    {
        ::rvk::scene::Matrix world;
        if (engine::mirror::get().worldOf(it.entity, world)) {
            ++stats_.rowsCompared;
            bool same = true;
            for (std::uint32_t r = 0; r < 3 && same; ++r) {
                for (std::uint32_t c = 0; c < 4; ++c) {
                    const float expected = world[c * 4 + r];  // row-vector matrix M: world = v * M; row r of the 3x4 = column r of M
                    if (std::abs(expected - blk.row[r][c]) > 1e-3f * (1.0f + std::abs(expected))) same = false;
                }
            }
            if (!same) ++stats_.rowsMismatch;
        }
    }
    items_.push_back(it);
    ++stats_.taken;
    out.action = d3d9::DrawReplacement::Action::Skip;
}

bool NativeOpaque::ensurePipeline(VkFormat color, VkFormat depth) {
    if (failed_) return false;
    vk::Context& ctx = dev_.ctx();
    if (layout_.valid() && (colorFormat_ != color || depthFormat_ != depth)) destroyPipeline();
    if (!layout_.valid() && !layout_.create(ctx, dev_.bindless(), {})) {
        failed_ = true;
        spdlog::error("enh: native opaque: pass layout failed, off for this session");
        return false;
    }
    if (pipeline_ == VK_NULL_HANDLE) {
        vk::PassGraphicsDesc gd;
        gd.vs = shader::embedded::spirv("native/static_mesh.vert");
        gd.fs = shader::embedded::spirv("native/static_mesh.frag");
        gd.color = color;
        gd.depth = depth;
        gd.stencil = vk::hasStencil(depth) ? depth : VK_FORMAT_UNDEFINED;
        // a debug view shows every fragment of the parts (no depth test, no culling): where the geometry lands, independent of the depth
        pipelineState_ = stateOf(debug_);
        gd.depthTest = pipelineState_ == 0 || pipelineState_ == 2;
        gd.depthWrite = gd.depthTest;
        // the main depth buffer is reversed (debug.reverse_z; the setting is read when the pipelines are built)
        gd.depthOp = dev_.reverseZ() ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
        gd.frontFace = VK_FRONT_FACE_CLOCKWISE;  // as the engine's draws (d3d9/convert.h kFrontFace with the flipped viewport)
        gd.cull = pipelineState_ == 0 || pipelineState_ == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        gd.dynamicCullMode = true;
        gd.name = "native.opaque";
        pipeline_ = vk::createPassGraphicsPipeline(ctx, layout_.layout(), gd);
        gd.cull = VK_CULL_MODE_NONE;  // masked (leaf cards, fences): two-sided
        gd.name = "native.opaque masked";
        pipelineMasked_ = vk::createPassGraphicsPipeline(ctx, layout_.layout(), gd);
        colorFormat_ = color;
        depthFormat_ = depth;
    }
    if (pipeline_ == VK_NULL_HANDLE || pipelineMasked_ == VK_NULL_HANDLE) {
        failed_ = true;
        spdlog::error("enh: native opaque: pipelines failed, off for this session");
        return false;
    }
    return true;
}

void NativeOpaque::pass(d3d9::PassContext& ctx) {
    if (listMode_ == 1 ? mirrorItems_.empty() : items_.empty()) return;
    std::vector<Item> items;
    if (listMode_ == 1) {
        for (const std::uint8_t seen : listSeen_) stats_.listUnseen += seen == 0 ? 1u : 0u;
        items.swap(mirrorItems_);
        listIndex_.clear();
        listSeen_.clear();
        // the per-view constants of this frame's engine draws (fog, the shadow mask)
        const bool fresh = view_.frame == dev_.frameValue();
        for (Item& it : items) {
            if (!fresh) break;
            std::memcpy(it.block.ps[3], view_.fog[0], 16);
            std::memcpy(it.block.ps[4], view_.fog[1], 16);
            it.mask = view_.mask;
            it.maskSampler = view_.maskSampler;
        }
    } else {
        items.swap(items_);
    }
    d3d9::Surface* ds = dev_.depthStencil();
    d3d9::Texture* scene = ctx.sceneHdr;
    if (ds == nullptr || ds->texture() == nullptr || scene == nullptr || ctx.target != scene) return;
    if (!ensurePipeline(scene->image().format(), ds->texture()->image().format())) return;
    VkCommandBuffer cmd = ctx.cmd;
    // every texture readable (barriers / uploads are not allowed inside the rendering scope); one that is not drops its feature
    std::unordered_map<d3d9::Texture*, bool> ready;
    const auto prepare = [&](d3d9::Texture*& t) {
        if (t == nullptr) return;
        auto [entry, fresh] = ready.try_emplace(t, false);
        if (fresh) entry->second = dev_.readable(t, cmd) && t->image().layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (!entry->second) t = nullptr;
    };
    for (Item& it : items) {
        prepare(it.diffuse);
        prepare(it.normal);
        prepare(it.mask);
        prepare(it.lightmap[0]);
        prepare(it.lightmap[1]);
        NativeItemBlock& blk = it.block;
        std::uint32_t& flags = blk.tex2[1];
        if (it.diffuse == nullptr) flags |= kFlagNoAlbedo;
        if (it.normal == nullptr) flags &= ~(kFlagNormalMap | kFlagDxt5nm);
        if (it.lightmap[0] == nullptr || it.lightmap[1] == nullptr) flags &= ~kFlagLightmap;
        blk.tex0[0] = it.diffuse != nullptr ? it.diffuse->bindlessSlot() : 0;
        blk.tex0[1] = it.diffuseSampler;
        blk.tex0[2] = it.normal != nullptr ? it.normal->bindlessSlot() : 0;
        blk.tex0[3] = it.normalSampler;
        blk.tex1[0] = it.mask != nullptr ? it.mask->bindlessSlot() : 0;
        blk.tex1[1] = it.maskSampler;
        blk.tex1[2] = it.lightmap[0] != nullptr ? it.lightmap[0]->bindlessSlot() : 0;
        blk.tex1[3] = it.lightmap[1] != nullptr ? it.lightmap[1]->bindlessSlot() : 0;
        blk.tex2[0] = it.lightmapSampler;
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
    dev_.regionBegin("native.opaque");
    vkCmdBeginRendering(cmd, &ri);
    // the engine's flipped viewport (d3d9/draw.cpp): y up as in D3D
    const VkViewport vp{0.0f, static_cast<float>(h), static_cast<float>(w), -static_cast<float>(h), 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    layout_.bindSet0(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dev_.bindless());
    const VkDeviceAddress shadowBlock = shadows_ != nullptr ? shadows_->block() : 0;
    VkPipeline bound = VK_NULL_HANDLE;
    VkCullModeFlags boundCull = 0xFFFFFFFFu;
    for (const Item& it : items) {
        const vk::RingAlloc a = dev_.frames().allocate(sizeof(NativeItemBlock), 16);
        if (a.ptr == nullptr) break;
        std::memcpy(a.ptr, &it.block, sizeof(NativeItemBlock));
        const VkPipeline want = it.masked ? pipelineMasked_ : pipeline_;
        if (want != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
            bound = want;
        }
        // the engine's cull mode of the draw; the debug views without culling draw both sides
        const VkCullModeFlags cull = pipelineState_ == 0 || pipelineState_ == 3 ? it.cull : VK_CULL_MODE_NONE;
        if (cull != boundCull) {
            vkCmdSetCullMode(cmd, cull);
            boundCull = cull;
        }
        NativeStaticPush push{};
        push.frame = ctx.frameBlock;
        push.passId = 3;
        push.passFlags = debug_;
        push.item[0] = static_cast<std::uint32_t>(a.address);
        push.item[1] = static_cast<std::uint32_t>(a.address >> 32);
        push.vertices[0] = static_cast<std::uint32_t>(it.vertices);
        push.vertices[1] = static_cast<std::uint32_t>(it.vertices >> 32);
        push.indices[0] = static_cast<std::uint32_t>(it.indices);
        push.indices[1] = static_cast<std::uint32_t>(it.indices >> 32);
        push.shadow[0] = static_cast<std::uint32_t>(shadowBlock);
        push.shadow[1] = static_cast<std::uint32_t>(shadowBlock >> 32);
        layout_.push(cmd, &push, sizeof(push));
        vkCmdDraw(cmd, it.indexCount, 1, it.first, 0);
        ++stats_.drawn;
    }
    vkCmdEndRendering(cmd);
    dev_.regionEnd();
    ++stats_.passes;
    const double now = dev_.secondsSinceCreate();
    if (now - lastReport_ > 5.0) {
        lastReport_ = now;
        report();
    }
}

void NativeOpaque::report() {
    std::string why;
    for (std::size_t i = 0; i < kReasons; ++i) {
        if (stats_.declined[i] != 0) why += std::format("{}{} {}", why.empty() ? "" : ", ", stats_.declined[i], kReasonNames[i]);
    }
    spdlog::info("enh: native opaque: {} draws taken from the engine, {} drawn in {} passes; left to the engine: {}; the mirror's matrix differs from the "
                 "draw's rows in {} of {} compared",
                 stats_.taken, stats_.drawn, stats_.passes, why.empty() ? "nothing" : why, stats_.rowsMismatch, stats_.rowsCompared);
    if (listMode_ == 1 && stats_.listFrames != 0) {
        const double n = static_cast<double>(stats_.listFrames);
        spdlog::info("enh: native opaque: mirror list {} frames: per frame {:.0f} entities seen, {:.0f} parts listed, {} fading entities left to the engine, {} without "
                     "lighting data; the engine drew {} static parts that are not in the list, {} listed parts it did not draw; lighting vs the engine ({} draws): "
                     "differs in camera {} / fog {} / indoor {} / SH {} / sun {} / lights {}",
                     stats_.listFrames, static_cast<double>(stats_.listEntities) / n, static_cast<double>(stats_.listParts) / n, stats_.listFading, stats_.listNoLight,
                     stats_.listEngineDrew, stats_.listUnseen, stats_.cmpDraws, stats_.cmpMismatch[0], stats_.cmpMismatch[1], stats_.cmpMismatch[2],
                     stats_.cmpMismatch[3], stats_.cmpMismatch[4], stats_.cmpMismatch[5]);
    }
    spdlog::info("enh: native opaque: the engine's static draws: lightmap x ambient [no/no {}, no/yes {}, yes/no {}, yes/yes {}], shadow mask no/yes {} / {}, sun no/yes {} / {}, "
                 "alpha test funcs [1:{} 2:{} 3:{} 4:{} 5:{} 6:{} 7:{} 8:{}], cull none {} / front {} / back {}",
                 stats_.permLmAmb[0][0], stats_.permLmAmb[0][1], stats_.permLmAmb[1][0], stats_.permLmAmb[1][1], stats_.permMask[0], stats_.permMask[1], stats_.permSun[0],
                 stats_.permSun[1], stats_.alphaFunc[1], stats_.alphaFunc[2], stats_.alphaFunc[3], stats_.alphaFunc[4], stats_.alphaFunc[5], stats_.alphaFunc[6],
                 stats_.alphaFunc[7], stats_.alphaFunc[8], stats_.cullModes[0], stats_.cullModes[1], stats_.cullModes[2]);
}

}  // namespace rvk::enh
