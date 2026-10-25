#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "engine/scene_mirror.h"
#include "enh/rt_scene.h"
#include "shader/abi.h"

// The scene mirror as the RT scene's source (docs/research/native-renderer.md N2, rt.source = mirror). The draw observation stays for what the mirror cannot
// describe (skinned characters, SpeedTrees, instanced batches without an entity, entities with blended / water parts); every other cached-in entity is
// fed from the mirror: one BLAS per sub-mesh range (the same cache and keys as the draws', so equal ranges merge), one pinned instance per (entity, part)
// with the entity's own matrix, the material from the engine's material graph. No aging, no quantised-transform keys, no owner guesses: the set of
// instances is recomputed from the mirror whenever it changes, and an instance of an unchanged part keeps its registry entry.

namespace rvk::enh {

namespace abi = shader::abi;
using d3d9::draws::Kind;

namespace {

constexpr std::uint64_t kMirrorDebounce = 8;     // frames between two rebuilds while the mirror keeps changing
constexpr std::uint64_t kMirrorRefresh = 120;    // frames: a rebuild at least this often (textures stream in, BLASes get evicted)
constexpr std::uint32_t kDeclFloat2 = 1, kDeclFloat3 = 2, kDeclColor = 4;
constexpr std::uint32_t kCmpGreater = 5;

std::uint32_t wordOf(std::uint32_t offset, std::uint32_t type) { return offset == 0xFFFFu ? abi::kRtAttribAbsent : (offset | (type << 16)); }

}  // namespace

void RtScene::setMirrorSource(NativeParts* parts) {
    if (parts == mirrorParts_) return;
    mirrorParts_ = parts;
    // a change of source: the instances of the old one go (the registry is rebuilt from the new source's sightings)
    registry_.removeIf([](const rt::Registry::Instance& in) { return in.pinned; });
    for (std::uint32_t id : mirrorMaterials_) materials_.release(id);
    mirrorMaterials_.clear();
    mirrorEntities_.clear();
    mirrorVersion_ = mirrorGeneration_ = mirrorFrame_ = 0;
    spdlog::info("rt: scene: source {}", parts != nullptr ? "the scene mirror (static meshes) + the engine's draws (the rest)" : "the engine's draws");
}

RtScene::Blas* RtScene::mirrorBlas(const NativePart& part) {
    rt::BlasKey key;
    key.vb = part.vbId;
    key.vbGen = part.vbGen;
    key.ib = part.ibId;
    key.ibGen = part.ibGen;
    key.vertexOffset = 0;
    key.stride = part.stride;
    key.first = part.first;
    key.primitives = part.indexCount / 3;
    key.baseVertex = 0;
    key.index32 = part.index32;
    key.strip = 0;
    const auto known = blasByKey_.find(key);
    if (known != blasByKey_.end() && known->second != nullptr) return known->second;
    if (waterKeys_.contains(key) || part.vertexBuffer == nullptr || part.indexBuffer == nullptr) return nullptr;
    const std::uint64_t id = nextBlasId_++;
    Blas& nb = blas_[id];
    nb.id = id;
    nb.key = key;
    nb.vb = part.vertexBuffer;
    nb.ib = part.indexBuffer;
    nb.vbAddress = part.vertices;
    nb.ibAddress = part.indices;
    nb.layout.positionOk = true;
    nb.layout.normal = wordOf(part.layout.normal, kDeclFloat3);
    nb.layout.texcoord0 = wordOf(part.layout.uv[0], kDeclFloat2);
    nb.layout.texcoord1 = wordOf(part.layout.uv[1], kDeclFloat2);
    nb.layout.texcoord6 = wordOf(part.layout.lightmap, kDeclFloat2);
    nb.layout.color0 = wordOf(part.layout.color0, kDeclColor);
    nb.kind = static_cast<std::uint32_t>(Kind::StaticMesh);
    blasByKey_[key] = &nb;
    blasByBuffer_.emplace(key.vb, id);
    if (key.ib != key.vb) blasByBuffer_.emplace(key.ib, id);
    pending_.push_back(id);
    return &nb;
}

void RtScene::feedMirror(std::uint64_t frame, double now, const float camera[3]) {
    if (mirrorParts_ == nullptr) return;
    scene::Mirror& mirror = engine::mirror::get();
    const std::uint64_t version = mirror.version(), generation = mirrorParts_->generation();
    const bool changed = version != mirrorVersion_ || generation != mirrorGeneration_;
    if (!(mirrorFrame_ == 0 || (changed && frame >= mirrorFrame_ + kMirrorDebounce) || frame >= mirrorFrame_ + kMirrorRefresh)) return;
    mirrorFrame_ = frame;
    mirrorVersion_ = version;
    mirrorGeneration_ = generation;
    ++mirrorRebuilds_;

    std::unordered_set<std::uint64_t> wanted;
    std::unordered_set<const void*> covered;
    std::vector<std::uint32_t> materials;
    std::uint64_t entities = 0, parts = 0, partial = 0;
    const std::uint32_t sampler = dev_.samplerFor(true, false);
    const std::uint32_t viewBits = (static_cast<std::uint32_t>(viewBit(d3d9::scene::View::Main)) | static_cast<std::uint32_t>(viewBit(d3d9::scene::View::Shadow)));
    mirror.forEach([&](const scene::Entity& e) {
        if (e.meshes.empty() || (e.state.flags & 0x8024u) != 0x8024u || (e.state.flags & 0x8u) != 0) return;  // cached in, enabled, boundary valid, not killed
        double sx = e.state.sphere[0], sy = e.state.sphere[1], sz = e.state.sphere[2], sr = e.state.sphere[3];
        if (!(sr > 0.0) || !std::isfinite(sr) || !std::isfinite(sx)) {
            sx = e.state.world[12];
            sy = e.state.world[13];
            sz = e.state.world[14];
            sr = 400.0;
        }
        if (limits_.radius > 0.0f) {
            const double dx = sx - camera[0], dy = sy - camera[1], dz = sz - camera[2];
            if (std::sqrt(dx * dx + dy * dy + dz * dz) - sr > limits_.radius) return;
        }
        // every part of the entity must be drawable opaquely, else the engine's draws stay its source (blended layers, water, parts without buffers)
        std::vector<NativePart> resolved;
        for (const scene::MeshInfo& mesh : e.meshes) {
            for (const scene::MeshPart& part : mesh.parts) {
                NativePart np;
                NativeParts::Reject why = NativeParts::Reject::None;
                if (!mirrorParts_->resolve(mesh, part, np, false, &why)) {
                    ++(why == NativeParts::Reject::Material ? mirrorRejectMaterial_ : why == NativeParts::Reject::Buffers ? mirrorRejectBuffers_ : mirrorRejectDecl_);
                    resolved.clear();
                    ++partial;
                    return;
                }
                resolved.push_back(np);
            }
        }
        if (resolved.empty()) return;
        rt::Rows rows;
        for (std::uint32_t r = 0; r < 3; ++r) {
            for (std::uint32_t c = 0; c < 4; ++c) rows[r * 4 + c] = e.state.world[c * 4 + r];  // row r of the 3x4 = column r of the row-vector matrix
        }
        if (!rt::finite(rows)) return;
        ++entities;
        bool allFed = true;
        for (const NativePart& np : resolved) {
            Blas* b = mirrorBlas(np);
            if (b == nullptr || b->state == BlasState::Failed) {
                allFed = false;
                continue;
            }
            // the material of the part: the graph's textures, an identity uv map on the diffuse's vertex uv set (an approximation: scrolling / scaled maps
            // of the engine's pixel shaders are not evaluated without a draw), the opacity role as the graph says
            RtMaterial m;
            m.albedo = np.diffuse;
            m.sampler = sampler;
            m.uvSet = (np.diffuseUvSet & abi::kRtUvSetMask) | abi::kRtUvApprox | (np.opacityChannel << abi::kRtUvOpacityShift);
            if (np.diffuse == nullptr) m.uvSet |= abi::kRtUvNoTexture;
            if (np.opacity != nullptr) {
                m.opacity = np.opacity;
                m.uvSet |= abi::kRtUvOpacityTexture;
            } else if (np.opacityOpaque) {
                m.uvSet |= abi::kRtUvOpacityOpaque;
            }
            m.masked = np.masked;
            if (np.masked) m.alpha = kCmpGreater | (static_cast<std::uint32_t>(np.alphaRef) << 8);
            if (np.name != nullptr) m.name = *np.name;
            const std::uint32_t mid = materials_.fromGraph(m);
            materials.push_back(mid);
            const std::uint64_t key = rt::instanceKey(b->id, rows);
            auto sr2 = registry_.see(key, b->id, rows, 0, (np.index32 != 0 ? abi::kRtRecordIndex32 : 0u) | (viewBits << abi::kRtRecordViewShift),
                                     static_cast<std::uint32_t>(Kind::StaticMesh) << 16, static_cast<std::uint8_t>(viewBits), frame, now);
            if (sr2.first == nullptr) continue;
            sr2.first->pinned = true;
            applyMaterial(*sr2.first, mid, 4, false, frame);
            wanted.insert(key);
            b->lastUsedFrame = frame;
            b->lastUsedTime = now;
            b->opaqueMainFrame = frame;
            ++parts;
        }
        if (allFed) covered.insert(e.ptr);
    });
    // the instances the mirror no longer wants (the entity left, its level of detail changed, it moved), then the materials of the last build
    registry_.removeIf([&](const rt::Registry::Instance& in) { return in.pinned && !wanted.contains(in.key); });
    for (std::uint32_t id : mirrorMaterials_) materials_.release(id);
    mirrorMaterials_ = std::move(materials);
    mirrorEntities_ = std::move(covered);
    mirrorEntityCount_ = entities;
    mirrorPartCount_ = parts;
    mirrorPartiallyRejected_ = partial;
    static double lastLog = 0.0;
    if (now - lastLog > 5.0) {
        lastLog = now;
        spdlog::info("rt: scene: mirror source: rebuild {} in frame {}: {} entities ({} fully fed), {} part instances, {} entities left to the draws (a part was blended / water / "
                     "without buffers: material {}, buffers {}, declaration {}), {} draws of fed entities skipped so far",
                     mirrorRebuilds_, frame, entities, mirrorEntities_.size(), parts, partial, mirrorRejectMaterial_, mirrorRejectBuffers_, mirrorRejectDecl_, mirrorSkippedDraws_);
    }
}

}  // namespace rvk::enh
