#include "enh/native_shadows.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "engine/scene_mirror.h"
#include "engine/shadow_maps.h"
#include "shader/embedded.h"
#include "vk/image.h"

namespace rvk::enh {

namespace {

constexpr std::uint32_t kCascades = 3;
constexpr std::uint32_t kMapSize = 2048;
constexpr double kSplits[kCascades] = {1500.0, 4000.0, 10000.0};  // far ends, world units (cm); the engine's: 1347 / 2945 / 10000
constexpr double kCasterRange = 15000.0;                         // how far toward the sun a caster may stand and still shadow the cascade
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr std::uint32_t kShadowIndex32 = 1, kShadowMasked = 2;

struct V3 {
    double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double length(V3 a) { return std::sqrt(dot(a, a)); }
V3 normalize(V3 a) {
    const double l = length(a);
    return l > 1e-12 ? a * (1.0 / l) : V3{0, 1, 0};
}

// mirror of shaders/native/shadow_mesh.glsl ShadowItem (96 bytes)
struct ShadowItem {
    float row[3][4];
    std::uint32_t tex[4];  // albedo slot, sampler, uv offset, flags
    std::uint32_t vtx[4];  // stride
    float params[4];       // alpha reference
};
static_assert(sizeof(ShadowItem) == 96);

struct ShadowPush {
    std::uint32_t cascade[2], item[2], vertices[2], indices[2];
};
static_assert(sizeof(ShadowPush) == 32);

}  // namespace

NativeShadows::NativeShadows(d3d9::Device& device, NativeParts& parts) : dev_(device), parts_(parts) {}

NativeShadows::~NativeShadows() {
    apply(false);
    destroyPipeline();
}

void NativeShadows::destroyPipeline() {
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
}

void NativeShadows::apply(bool enabled) {
    enabled_ = enabled;
    if (enabled_ && passId_ == 0) {
        passId_ = dev_.addPass(d3d9::inject::Point::AfterDepthPrepass, "native.shadows", [this](d3d9::PassContext& c) { pass(c); }, this, 5);
        failed_ = false;
        spdlog::info("enh: native shadows: on ({} cascades of {}x{}, splits {:.0f} / {:.0f} / {:.0f})", kCascades, kMapSize, kMapSize, kSplits[0], kSplits[1], kSplits[2]);
    } else if (!enabled_ && passId_ != 0) {
        dev_.removePass(passId_);
        passId_ = 0;
        blockFrame_ = ~0ull;
        spdlog::info("enh: native shadows: off");
    }
}

bool NativeShadows::ensurePipeline() {
    if (failed_) return false;
    vk::Context& ctx = dev_.ctx();
    if (!layout_.valid() && !layout_.create(ctx, dev_.bindless(), {})) {
        failed_ = true;
        spdlog::error("enh: native shadows: pass layout failed, off for this session");
        return false;
    }
    if (pipeline_ == VK_NULL_HANDLE) {
        vk::PassGraphicsDesc gd;
        gd.vs = shader::embedded::spirv("native/shadow_mesh.vert");
        gd.fs = shader::embedded::spirv("native/shadow_mesh.frag");
        gd.color = VK_FORMAT_UNDEFINED;  // depth only
        gd.depth = kDepthFormat;
        gd.depthTest = true;
        gd.depthWrite = true;
        gd.depthOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        gd.cull = VK_CULL_MODE_NONE;
        gd.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        gd.name = "native.shadow";
        pipeline_ = vk::createPassGraphicsPipeline(ctx, layout_.layout(), gd);
        pipelineMasked_ = pipeline_;  // one pipeline: the alpha test is a branch on the item's flags
    }
    if (pipeline_ == VK_NULL_HANDLE) {
        failed_ = true;
        spdlog::error("enh: native shadows: pipeline failed, off for this session");
        return false;
    }
    return true;
}

void NativeShadows::pass(d3d9::PassContext& ctx) {
    blockFrame_ = ~0ull;
    if (ctx.mainView != 1 || !enabled_ || failed_) return;
    const auto t0 = std::chrono::steady_clock::now();
    const d3d9::scene::CameraBlock& cam = dev_.camera();
    if (!cam.valid) {
        ++stats_.skippedNoCamera;
        return;
    }
    float sun[3];
    if (!engine::shadowmaps::trueSunVector(sun)) {
        ++stats_.skippedNoSun;
        return;
    }
    if (!ensurePipeline()) return;
    VkCommandBuffer cmd = ctx.cmd;

    // ---- the cascades: the camera's frustum slices, bounding spheres, texel-snapped light-space boxes ----
    const V3 camPos{cam.position[0], cam.position[1], cam.position[2]};
    const V3 fwd = normalize({cam.inverseView.at(2, 0), cam.inverseView.at(2, 1), cam.inverseView.at(2, 2)});
    const V3 right = normalize({cam.inverseView.at(0, 0), cam.inverseView.at(0, 1), cam.inverseView.at(0, 2)});
    const V3 up = normalize({cam.inverseView.at(1, 0), cam.inverseView.at(1, 1), cam.inverseView.at(1, 2)});
    const double tanX = cam.projection.at(0, 0) > 1e-6f ? 1.0 / cam.projection.at(0, 0) : 1.0;
    const double tanY = cam.projection.at(1, 1) > 1e-6f ? 1.0 / cam.projection.at(1, 1) : 0.6;
    const V3 travel = normalize({sun[0], sun[1], sun[2]});  // the direction the light travels
    const V3 up0 = std::abs(travel.y) > 0.99 ? V3{1, 0, 0} : V3{0, 1, 0};
    const V3 lr = normalize(cross(up0, travel));
    const V3 lu = cross(travel, lr);
    Cascade cascades[kCascades];
    double nearD = std::max<double>(cam.nearZ, 1.0);
    for (std::uint32_t i = 0; i < kCascades; ++i) {
        const double farD = kSplits[i];
        V3 corners[8];
        std::uint32_t k = 0;
        for (const double d : {nearD, farD}) {
            for (const double sx : {-1.0, 1.0}) {
                for (const double sy : {-1.0, 1.0}) corners[k++] = camPos + fwd * d + right * (sx * tanX * d) + up * (sy * tanY * d);
            }
        }
        V3 c{};
        for (const V3& p : corners) c = c + p;
        c = c * (1.0 / 8.0);
        double r = 0.0;
        for (const V3& p : corners) r = std::max(r, length(p - c));
        r = std::ceil(r / 10.0) * 10.0;
        // texel snapping: the box moves in whole texels, so the shadows do not shimmer when the camera moves
        const double texel = 2.0 * r / kMapSize;
        const double cx = dot(c, lr), cy = dot(c, lu);
        c = c + lr * (std::floor(cx / texel) * texel - cx) + lu * (std::floor(cy / texel) * texel - cy);
        const V3 eye = c - travel * (r + kCasterRange);
        const double zRange = 2.0 * r + kCasterRange;
        Cascade& cs = cascades[i];
        cs.center[0] = c.x;
        cs.center[1] = c.y;
        cs.center[2] = c.z;
        cs.radius = r;
        cs.zRange = zRange;
        // clip = M * (world, 1): x, y = the light-space offsets over the radius, z = the depth along the light over the range
        const double m[3][4] = {{lr.x / r, lr.y / r, lr.z / r, -dot(lr, eye) / r},
                                {lu.x / r, lu.y / r, lu.z / r, -dot(lu, eye) / r},
                                {travel.x / zRange, travel.y / zRange, travel.z / zRange, -dot(travel, eye) / zRange}};
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 3; ++row) cs.viewProj[col * 4 + row] = static_cast<float>(m[row][col]);
            cs.viewProj[col * 4 + 3] = col == 3 ? 1.0f : 0.0f;
        }
        nearD = farD;
    }

    // ---- the casters: every mirrored static part that can shadow the cascades' volume ----
    casters_.clear();
    const double reach = kSplits[kCascades - 1] + kCasterRange + 5000.0;
    engine::mirror::get().forEach([&](const scene::Entity& e) {
        if (e.meshes.empty() || (e.state.flags & 0x4u) == 0) return;  // not enabled / no mesh contexts
        V3 sc{e.state.sphere[0], e.state.sphere[1], e.state.sphere[2]};
        double sr = e.state.sphere[3];
        if (!(sr > 0.0) || !std::isfinite(sr) || !std::isfinite(sc.x)) {
            sc = {e.state.world[12], e.state.world[13], e.state.world[14]};
            sr = 400.0;
        }
        if (length(sc - camPos) - sr > reach) return;
        for (const scene::MeshInfo& mesh : e.meshes) {
            for (const scene::MeshPart& part : mesh.parts) {
                Caster c;
                if (!parts_.resolve(mesh, part, c.part)) continue;
                for (std::uint32_t r = 0; r < 3; ++r) {
                    for (std::uint32_t col = 0; col < 4; ++col) c.rows[r][col] = e.state.world[col * 4 + r];  // row r of the 3x4 = column r of the row-vector matrix
                }
                c.sphere[0] = static_cast<float>(sc.x);
                c.sphere[1] = static_cast<float>(sc.y);
                c.sphere[2] = static_cast<float>(sc.z);
                c.sphere[3] = static_cast<float>(sr);
                casters_.push_back(c);
            }
        }
    });
    if (casters_.empty()) return;

    // ---- rings: the cascades' matrices, the casters' items ----
    const vk::RingAlloc cas = dev_.frames().allocate(kCascades * 64, 16);
    const vk::RingAlloc items = dev_.frames().allocate(casters_.size() * sizeof(ShadowItem), 16);
    const vk::RingAlloc blk = dev_.frames().allocate(sizeof(NativeShadowBlock), 16);
    if (cas.ptr == nullptr || items.ptr == nullptr || blk.ptr == nullptr) return;
    for (std::uint32_t i = 0; i < kCascades; ++i) std::memcpy(static_cast<std::uint8_t*>(cas.ptr) + i * 64, cascades[i].viewProj, 64);
    auto* itemMem = static_cast<ShadowItem*>(items.ptr);
    // the casters' albedo textures readable (before any rendering scope)
    for (std::size_t i = 0; i < casters_.size(); ++i) {
        Caster& c = casters_[i];
        if (c.part.masked && c.part.diffuse != nullptr && (!dev_.readable(c.part.diffuse, cmd) || c.part.diffuse->image().layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
            c.part.diffuse = nullptr;
        }
        ShadowItem& it = itemMem[i];
        std::memcpy(it.row, c.rows, sizeof(it.row));
        it.tex[0] = c.part.masked && c.part.diffuse != nullptr ? c.part.diffuse->bindlessSlot() : 0;
        it.tex[1] = dev_.samplerFor(true, false);
        const std::uint32_t uv = c.part.layout.uv[std::min<std::uint32_t>(c.part.diffuseUvSet, 1u)] != 0xFFFFu ? c.part.layout.uv[std::min<std::uint32_t>(c.part.diffuseUvSet, 1u)] : c.part.layout.uv[0];
        it.tex[2] = uv;
        it.tex[3] = (c.part.index32 != 0 ? kShadowIndex32 : 0u) | (c.part.masked ? kShadowMasked : 0u);
        it.vtx[0] = c.part.stride;
        it.vtx[1] = it.vtx[2] = it.vtx[3] = 0;
        it.params[0] = c.part.alphaRef;
        it.params[1] = it.params[2] = it.params[3] = 0.0f;
    }

    // ---- the depth passes ----
    NativeShadowBlock block;
    std::uint64_t draws = 0;
    for (std::uint32_t ci = 0; ci < kCascades; ++ci) {
        const Cascade& cs = cascades[ci];
        vk::Transients::Entry* map = dev_.transients().get(std::format("native.shadow.{}", ci),
                                                           {kDepthFormat, {kMapSize, kMapSize}, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
        if (map == nullptr) return;
        map->image.transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = map->image.view();
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {kMapSize, kMapSize}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &depth;
        dev_.regionBegin("native.shadow", ci);
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport vp{0.0f, 0.0f, static_cast<float>(kMapSize), static_cast<float>(kMapSize), 0.0f, 1.0f};
        const VkRect2D sc{{0, 0}, {kMapSize, kMapSize}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        layout_.bindSet0(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dev_.bindless());
        const V3 cc{cs.center[0], cs.center[1], cs.center[2]};
        for (std::size_t i = 0; i < casters_.size(); ++i) {
            const Caster& c = casters_[i];
            // the caster's bounding sphere against the cascade's cylinder toward the sun
            const V3 d = V3{c.sphere[0], c.sphere[1], c.sphere[2]} - cc;
            const double along = dot(d, travel);
            const V3 lateral = d - travel * along;
            if (length(lateral) > cs.radius + c.sphere[3] || along < -kCasterRange - c.sphere[3] || along > cs.radius + c.sphere[3]) continue;
            ShadowPush push{};
            const VkDeviceAddress cascadeAddr = cas.address + ci * 64, itemAddr = items.address + i * sizeof(ShadowItem);
            push.cascade[0] = static_cast<std::uint32_t>(cascadeAddr);
            push.cascade[1] = static_cast<std::uint32_t>(cascadeAddr >> 32);
            push.item[0] = static_cast<std::uint32_t>(itemAddr);
            push.item[1] = static_cast<std::uint32_t>(itemAddr >> 32);
            push.vertices[0] = static_cast<std::uint32_t>(c.part.vertices);
            push.vertices[1] = static_cast<std::uint32_t>(c.part.vertices >> 32);
            push.indices[0] = static_cast<std::uint32_t>(c.part.indices);
            push.indices[1] = static_cast<std::uint32_t>(c.part.indices >> 32);
            layout_.push(cmd, &push, sizeof(push));
            vkCmdDraw(cmd, c.part.indexCount, 1, c.part.first, 0);
            ++draws;
        }
        vkCmdEndRendering(cmd);
        dev_.regionEnd();
        map->image.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        std::memcpy(block.viewProj[ci], cs.viewProj, 64);
        block.split[ci] = static_cast<float>(kSplits[ci]);
        block.maps[ci] = map->slot;
        block.texel[ci] = static_cast<float>(2.0 * cs.radius / kMapSize);
        block.range[ci] = static_cast<float>(cs.zRange);
    }
    block.split[3] = 1.0f;
    block.maps[3] = dev_.samplerFor(false, true);
    block.params[0] = 1.0f / kMapSize;
    block.params[1] = 2.5f;   // depth bias, in texels of world size
    block.params[2] = 1.5f;   // normal offset, in texels
    block.params[3] = 0.15f;  // the last cascade fades out over this fraction
    std::memcpy(blk.ptr, &block, sizeof(block));
    block_ = blk.address;
    blockFrame_ = dev_.frameValue();

    ++stats_.frames;
    stats_.casters += casters_.size();
    stats_.draws += draws;
    stats_.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const double now = dev_.secondsSinceCreate();
    if (now - lastReport_ > 5.0) {
        lastReport_ = now;
        const double n = static_cast<double>(std::max<std::uint64_t>(stats_.frames, 1));
        const NativeParts::Stats& ps = parts_.stats();
        spdlog::info("enh: native shadows: {} frames, per frame {:.0f} casters, {:.0f} draws over {} cascades, {:.2f} ms CPU; parts resolved {} (no buffers {}, "
                     "no declaration {}, no material / blended {}); sun ({:.2f}, {:.2f}, {:.2f})",
                     stats_.frames, static_cast<double>(stats_.casters) / n, static_cast<double>(stats_.draws) / n, kCascades, stats_.ms / n, ps.resolved,
                     ps.noBuffers, ps.noDecl, ps.noMaterial, travel.x, travel.y, travel.z);
        stats_ = Stats{};
    }
}

}  // namespace rvk::enh
