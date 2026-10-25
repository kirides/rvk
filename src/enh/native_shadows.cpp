#include "enh/native_shadows.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>

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

// mirror of shaders/native/shadow_mesh.glsl ShadowItem (48 bytes)
struct ShadowItem {
    std::uint32_t tex[4];  // albedo slot, sampler, uv offset, flags
    std::uint32_t vtx[4];  // stride
    float params[4];       // alpha reference
};
static_assert(sizeof(ShadowItem) == 48);

struct ShadowPush {
    std::uint32_t cascade[2], item[2], instances[2], vertices[2], indices[2];
};
static_assert(sizeof(ShadowPush) == 40);

constexpr std::uint64_t kRebuildDebounce = 8;    // frames between two rebuilds while the mirror keeps changing
constexpr std::uint64_t kRefreshFrames = 4;      // frames between two in-place transform refreshes
constexpr std::uint64_t kRebuildFrames = 90;     // the groups are rebuilt at least this often (a texture arrived, a part streamed in)
constexpr double kRebuildCameraMove = 1500.0;    // units: a camera moved this far since the last build rebuilds (the casters' reach follows it)

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

void NativeShadows::rebuildGroups(const double camPos[3], double reach) {
    const auto t0 = std::chrono::steady_clock::now();
    groups_.clear();
    casterCount_ = 0;
    ++rebuilds_;
    builtStructure_ = engine::mirror::get().structureVersion();
    builtTransform_ = engine::mirror::get().transformVersion();
    builtParts_ = parts_.generation();
    builtFrame_ = dev_.frameValue();
    refreshedFrame_ = builtFrame_;
    for (int i = 0; i < 3; ++i) builtCamera_[i] = camPos[i];
    struct Key {
        VkDeviceAddress vertices, indices;
        std::uint32_t first, count;
        const d3d9::Texture* diffuse;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            std::uint64_t h = k.vertices * 0x9E3779B97F4A7C15ull;
            h ^= k.indices + 0x517CC1B727220A95ull + (h << 6) + (h >> 2);
            h ^= (static_cast<std::uint64_t>(k.first) << 32 | k.count) * 0xC2B2AE3D27D4EB4Full;
            h ^= reinterpret_cast<std::uintptr_t>(k.diffuse) * 0x165667B19E3779F9ull;
            return static_cast<std::size_t>(h);
        }
    };
    std::unordered_map<Key, std::size_t, KeyHash> index;
    engine::mirror::get().forEach([&](const ::rvk::scene::Entity& e) {
        if (e.meshes.empty() || (e.state.flags & 0x4u) == 0) return;  // not enabled / no mesh contexts
        V3 sc{e.state.sphere[0], e.state.sphere[1], e.state.sphere[2]};
        double sr = e.state.sphere[3];
        if (!(sr > 0.0) || !std::isfinite(sr) || !std::isfinite(sc.x)) {
            sc = {e.state.world[12], e.state.world[13], e.state.world[14]};
            sr = 400.0;
        }
        const double dx = sc.x - camPos[0], dy = sc.y - camPos[1], dz = sc.z - camPos[2];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) - sr > reach) return;
        Inst proto;
        for (std::uint32_t r = 0; r < 3; ++r) {
            for (std::uint32_t col = 0; col < 4; ++col) proto.rows[r][col] = e.state.world[col * 4 + r];  // row r of the 3x4 = column r of the row-vector matrix
        }
        proto.sphere[0] = static_cast<float>(sc.x);
        proto.sphere[1] = static_cast<float>(sc.y);
        proto.sphere[2] = static_cast<float>(sc.z);
        proto.sphere[3] = static_cast<float>(sr);
        proto.entity = e.ptr;
        for (const ::rvk::scene::MeshInfo& mesh : e.meshes) {
            for (const ::rvk::scene::MeshPart& part : mesh.parts) {
                NativePart np;
                if (!parts_.resolve(mesh, part, np, false)) continue;
                const Key key{np.vertices, np.indices, np.first, np.indexCount, np.masked ? np.diffuse : nullptr};
                auto [it, fresh] = index.try_emplace(key, groups_.size());
                if (fresh) {
                    groups_.emplace_back();
                    groups_.back().part = np;
                }
                groups_[it->second].instances.push_back(proto);
                ++casterCount_;
            }
        }
    });
    rebuildMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// entities that moved: their instances' rows and spheres from the mirror again (the groups stay)
void NativeShadows::refreshTransforms() {
    const auto t0 = std::chrono::steady_clock::now();
    builtTransform_ = engine::mirror::get().transformVersion();
    refreshedFrame_ = dev_.frameValue();
    ++refreshes_;
    for (Group& g : groups_) {
        for (Inst& in : g.instances) {
            ::rvk::scene::EntityState st;
            if (!engine::mirror::get().stateOf(in.entity, st)) continue;
            for (std::uint32_t r = 0; r < 3; ++r) {
                for (std::uint32_t col = 0; col < 4; ++col) in.rows[r][col] = st.world[col * 4 + r];
            }
            if (st.sphere[3] > 0.0f && std::isfinite(st.sphere[3])) {
                in.sphere[0] = st.sphere[0];
                in.sphere[1] = st.sphere[1];
                in.sphere[2] = st.sphere[2];
                in.sphere[3] = st.sphere[3];
            } else {
                in.sphere[0] = st.world[12];
                in.sphere[1] = st.world[13];
                in.sphere[2] = st.world[14];
            }
        }
    }
    refreshMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
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

    // ---- the casters: the cached groups of mirrored static parts that can shadow the cascades' volume ----
    const double reach = kSplits[kCascades - 1] + kCasterRange + 5000.0;
    const double camMove = std::sqrt((camPos.x - builtCamera_[0]) * (camPos.x - builtCamera_[0]) + (camPos.y - builtCamera_[1]) * (camPos.y - builtCamera_[1]) +
                                     (camPos.z - builtCamera_[2]) * (camPos.z - builtCamera_[2]));
    const std::uint64_t frameNow = dev_.frameValue();
    // the groups are rebuilt when entities / their meshes / resolved parts changed (debounced), or when the camera moved far or time passed; entities that only
    // moved update their instances in place (every few frames)
    const bool structureChanged = engine::mirror::get().structureVersion() != builtStructure_ || parts_.generation() != builtParts_;
    if (groups_.empty() || (structureChanged && frameNow >= builtFrame_ + kRebuildDebounce) || frameNow >= builtFrame_ + kRebuildFrames || camMove > kRebuildCameraMove) {
        const double cp[3] = {camPos.x, camPos.y, camPos.z};
        rebuildGroups(cp, reach);
    } else if (engine::mirror::get().transformVersion() != builtTransform_ && frameNow >= refreshedFrame_ + kRefreshFrames) {
        refreshTransforms();
    }
    if (groups_.empty()) return;

    // ---- rings: the cascades' matrices, the groups' items, the visible instances per cascade ----
    std::size_t instanceTotal = 0;
    for (const Group& g : groups_) instanceTotal += g.instances.size();
    const vk::RingAlloc cas = dev_.frames().allocate(kCascades * 64, 16);
    const vk::RingAlloc items = dev_.frames().allocate(groups_.size() * sizeof(ShadowItem), 16);
    const vk::RingAlloc blk = dev_.frames().allocate(sizeof(NativeShadowBlock), 16);
    vk::RingAlloc inst[kCascades];
    for (std::uint32_t i = 0; i < kCascades; ++i) inst[i] = dev_.frames().allocate(std::max<std::size_t>(instanceTotal, 1) * 48, 16);
    bool ok = cas.ptr != nullptr && items.ptr != nullptr && blk.ptr != nullptr;
    for (std::uint32_t i = 0; i < kCascades; ++i) ok = ok && inst[i].ptr != nullptr;
    if (!ok) return;
    for (std::uint32_t i = 0; i < kCascades; ++i) std::memcpy(static_cast<std::uint8_t*>(cas.ptr) + i * 64, cascades[i].viewProj, 64);
    auto* itemMem = static_cast<ShadowItem*>(items.ptr);
    const std::uint32_t sampler = dev_.samplerFor(true, false);
    // the masked groups' albedo textures readable (before any rendering scope); a texture that is not drops the test
    for (std::size_t i = 0; i < groups_.size(); ++i) {
        Group& g = groups_[i];
        d3d9::Texture* alpha = g.part.masked ? g.part.diffuse : nullptr;
        if (alpha != nullptr && (!dev_.readable(alpha, cmd) || alpha->image().layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) alpha = nullptr;
        ShadowItem& it = itemMem[i];
        it.tex[0] = alpha != nullptr ? alpha->bindlessSlot() : 0;
        it.tex[1] = sampler;
        const std::uint32_t set = std::min<std::uint32_t>(g.part.diffuseUvSet, 1u);
        it.tex[2] = g.part.layout.uv[set] != 0xFFFFu ? g.part.layout.uv[set] : g.part.layout.uv[0];
        it.tex[3] = (g.part.index32 != 0 ? kShadowIndex32 : 0u) | (g.part.masked && alpha != nullptr ? kShadowMasked : 0u);
        it.vtx[0] = g.part.stride;
        it.vtx[1] = it.vtx[2] = it.vtx[3] = 0;
        it.params[0] = g.part.alphaRef;
        it.params[1] = it.params[2] = it.params[3] = 0.0f;
    }

    // ---- the depth passes ----
    NativeShadowBlock block;
    std::uint64_t draws = 0, instancesDrawn = 0;
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
        auto* out = static_cast<float*>(inst[ci].ptr);
        std::uint32_t written = 0;
        for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
            const Group& g = groups_[gi];
            const std::uint32_t first = written;
            for (const Inst& in : g.instances) {
                // the instance's bounding sphere against the cascade's cylinder toward the sun
                const V3 d = V3{in.sphere[0], in.sphere[1], in.sphere[2]} - cc;
                const double along = dot(d, travel);
                const V3 lateral = d - travel * along;
                if (length(lateral) > cs.radius + in.sphere[3] || along < -kCasterRange - in.sphere[3] || along > cs.radius + in.sphere[3]) continue;
                std::memcpy(out + 12 * static_cast<std::size_t>(written), in.rows, 48);
                ++written;
            }
            const std::uint32_t count = written - first;
            if (count == 0) continue;
            ShadowPush push{};
            const VkDeviceAddress cascadeAddr = cas.address + ci * 64, itemAddr = items.address + gi * sizeof(ShadowItem);
            push.cascade[0] = static_cast<std::uint32_t>(cascadeAddr);
            push.cascade[1] = static_cast<std::uint32_t>(cascadeAddr >> 32);
            push.item[0] = static_cast<std::uint32_t>(itemAddr);
            push.item[1] = static_cast<std::uint32_t>(itemAddr >> 32);
            push.instances[0] = static_cast<std::uint32_t>(inst[ci].address);
            push.instances[1] = static_cast<std::uint32_t>(inst[ci].address >> 32);
            push.vertices[0] = static_cast<std::uint32_t>(g.part.vertices);
            push.vertices[1] = static_cast<std::uint32_t>(g.part.vertices >> 32);
            push.indices[0] = static_cast<std::uint32_t>(g.part.indices);
            push.indices[1] = static_cast<std::uint32_t>(g.part.indices >> 32);
            layout_.push(cmd, &push, sizeof(push));
            vkCmdDraw(cmd, g.part.indexCount, count, g.part.first, first);
            ++draws;
            instancesDrawn += count;
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
    stats_.casters += casterCount_;
    stats_.draws += draws;
    stats_.instances += instancesDrawn;
    stats_.groups += groups_.size();
    stats_.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const double now = dev_.secondsSinceCreate();
    if (now - lastReport_ > 5.0) {
        lastReport_ = now;
        const double n = static_cast<double>(std::max<std::uint64_t>(stats_.frames, 1));
        const NativeParts::Stats& ps = parts_.stats();
spdlog::info("enh: native shadows: {} frames, per frame {:.0f} casters in {:.0f} groups, {:.0f} instanced draws ({:.0f} instances) over {} cascades, {:.2f} ms CPU, "
                     "{} group rebuilds ({:.2f} ms each), {} transform refreshes ({:.2f} ms each); parts resolved {} (no buffers {}, no declaration {}, no material / blended {}); sun ({:.2f}, {:.2f}, {:.2f})",
                     stats_.frames, static_cast<double>(stats_.casters) / n, static_cast<double>(stats_.groups) / n, static_cast<double>(stats_.draws) / n,
                     static_cast<double>(stats_.instances) / n, kCascades, stats_.ms / n, rebuilds_, rebuilds_ != 0 ? rebuildMs_ / static_cast<double>(rebuilds_) : 0.0, refreshes_,
                     refreshes_ != 0 ? refreshMs_ / static_cast<double>(refreshes_) : 0.0, ps.resolved,
                     ps.noBuffers, ps.noDecl, ps.noMaterial, travel.x, travel.y, travel.z);
        spdlog::info("enh: native parts: diffuse missing: no sampler image {}, image not loaded (no API texture) {}, no texture of ours {}; water left out {}; declaration lacks uv0 {} / color1 {} / tangent {} / normal {}; parts with a lightmap uv {}: no lightmap resource {}, not loaded {}", ps.diffuseNoImage, ps.diffuseNoApi, ps.diffuseNoTexture, ps.water, ps.uvAbsent, ps.color1Absent, ps.tangentAbsent, ps.normalAbsent, ps.lightmapParts, ps.lightmapNoResource, ps.lightmapNotLoaded);
        stats_ = Stats{};
    }
}

}  // namespace rvk::enh
