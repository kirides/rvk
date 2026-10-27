#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <spdlog/spdlog.h>

#include "d3d9/convert.h"
#include "d3d9/defs.h"
#include "enh/native_opaque.h"
#include "enh/native_parts.h"
#include "engine/lod.h"
#include "engine/memread.h"
#include "engine/scene_mirror.h"

// The native opaque pass's mirror list mode (docs/research/native-renderer.md N3.2 / 10): the parts to draw come from the scene mirror (own frustum
// and fade / view range culling, the engine's rules), the lighting constants are composed from the entity's illumination structs and the
// engine's light list, the lightmaps / normal map / diffuse from the mesh resource and the material graph: no engine draw is needed. The engine's
// own draws of listed parts are skipped (and compared with what was composed: rvk.log `native opaque: lighting vs the engine`).

namespace rvk::enh {

namespace {

constexpr std::uint32_t kFlagMasked = 2, kFlagNoAlbedo = 4, kFlagLightmap = 8, kFlagAmbComplex = 16, kFlagNormalMap = 32, kFlagDxt5nm = 64;
constexpr std::uint32_t kMaxLights = 4;  // up to 2 static + 2 dynamic lights per entity (the engine splits them into passes of 3)

}  // namespace

// c14..c40 of an entity from its illumination (eCIlluminated_PS entity +0xf8, struct at +0x78) and the shared light list it points to
bool NativeOpaque::composeLighting(const void* entity, NativeItemBlock& blk, std::uint32_t& lights) const {
    const void* illumPs = nullptr;
    if (!engine::readAt(entity, 0xf8, illumPs) || illumPs == nullptr) return false;
    const auto* illum = static_cast<const std::uint8_t*>(illumPs) + 0x78;
    const void* listPtr = nullptr;
    if (!engine::readAt(illum, 0, listPtr) || listPtr == nullptr) return false;
    const auto* list = static_cast<const std::uint8_t*>(listPtr);
    float f = 1.0f;
    std::uint8_t nStatic = 0, hasSh = 0, sun = 0, nDyn = 0;
    if (!engine::readAt(illum, 8, f) || !engine::readAt(illum, 0x0d, nStatic) || !engine::readAt(illum, 0x90, hasSh) || !engine::readAt(list, 0, sun) ||
        !engine::readAt(list, 0x34, nDyn)) {
        return false;
    }
    if (!(f >= 0.0f && f <= 1.5f) || nStatic > 2 || nDyn > 2) return false;
    float indoor[3]{};
    if (!engine::safeRead(list + 0x128, indoor, sizeof(indoor))) return false;
    for (int c = 0; c < 3; ++c) blk.ps[14][c] = indoor[c];
    blk.ps[14][3] = 0.0f;
    // the SH ambient: the hemisphere's x f, the indoor colour in .w of the first three, plus the entity's static SH
    for (std::uint32_t k = 0; k < 7; ++k) {
        float v[4]{};
        if (!engine::safeRead(list + 0xb8 + 16 * k, v, 16)) return false;
        for (int c = 0; c < 4; ++c) blk.ps[15 + k][c] = v[c] * f;
        if (k < 3) blk.ps[15 + k][3] += (1.0f - f) * indoor[k];
        if (hasSh != 0) {
            float s[4]{};
            if (engine::safeRead(illum + 0x94 + 16 * k, s, 16)) {
                for (int c = 0; c < 4; ++c) blk.ps[15 + k][c] += s[c];
            }
        }
    }
    // the sun: c22 diffuse, c23 specular, c24 towards the sun, verbatim
    if (!engine::safeRead(list + 4, blk.ps[22], 48)) return false;
    if (sun == 0) {
        std::memset(blk.ps[22], 0, 32);
    }
    // the lights: the static ones first, then the dynamic ones
    lights = std::min<std::uint32_t>(static_cast<std::uint32_t>(nStatic) + nDyn, kMaxLights);
    for (std::uint32_t i = 0; i < lights; ++i) {
        const std::uint8_t* src = i < nStatic ? illum + 0x10 + 0x40 * static_cast<std::size_t>(i) : list + 0x38 + 0x40 * static_cast<std::size_t>(i - nStatic);
        if (!engine::safeRead(src, blk.ps[25 + 4 * i], 64)) return false;
    }
    return true;
}

// the composed block of a listed part against the constants of the engine's draw of it (render thread, inside the replacement)
void NativeOpaque::compareLighting(const Item& listed, const d3d9::Device& device) {
    static constexpr struct {
        std::uint32_t first, count;
        const char* name;
    } kGroups[6] = {{0, 2, "camera c0-c1"}, {3, 2, "fog c3-c4"}, {14, 1, "indoor c14"}, {15, 7, "SH c15-c21"}, {22, 3, "sun c22-c24"}, {25, 12, "lights c25-c36"}};
    ++stats_.cmpDraws;
    for (std::uint32_t g = 0; g < 6; ++g) {
        bool bad = false;
        std::uint32_t badReg = 0;
        // the light registers: only the lights in use (the others hold stale values in the engine's draws)
        const std::uint32_t used = g == 5 ? 4 * std::min<std::uint32_t>(listed.block.off1[3] & 255u, 3u) : kGroups[g].count;
        for (std::uint32_t r = kGroups[g].first; r < kGroups[g].first + used && !bad; ++r) {
            const float* eng = device.psConstant(r);
            for (int c = 0; c < 4; ++c) {
                const float ours = listed.block.ps[r][c];
                if (!(std::abs(ours - eng[c]) <= 2e-3f + 2e-3f * std::abs(eng[c]))) {
                    bad = true;
                    badReg = r;
                    break;
                }
            }
        }
        if (!bad) continue;
        ++stats_.cmpMismatch[g];
        static int logged = 0;
        if (logged < 12) {
            ++logged;
            const float* e = device.psConstant(badReg);
            const float* o = listed.block.ps[badReg];
            spdlog::info("enh: native opaque: lighting differs in {} (c{}): composed ({:.4f}, {:.4f}, {:.4f}, {:.4f}), the engine's ({:.4f}, {:.4f}, {:.4f}, {:.4f}), entity {}",
                         kGroups[g].name, badReg, o[0], o[1], o[2], o[3], e[0], e[1], e[2], e[3], listed.entity);
        }
    }
}

void NativeOpaque::buildList(d3d9::PassContext& ctx) {
    items_.clear();
    listIndex_.clear();
    listSeen_.clear();
    const d3d9::scene::CameraBlock& cam = dev_.camera();
    if (!cam.valid) return;
    ++stats_.listFrames;

    // the view frustum (row-vector D3D matrix: clip_j = (x, y, z, 1) . column j; inside = -w <= x <= w, -w <= y <= w, 0 <= z <= w)
    double planes[6][4];
    {
        const auto col = [&](int c, int i) { return static_cast<double>(cam.viewProjection.at(i, c)); };
        const double sign[6][2] = {{1, 0}, {-1, 0}, {1, 1}, {-1, 1}, {0, 2}, {-1, 2}};  // left, right, bottom, top, near, far
        for (int p = 0; p < 6; ++p) {
            const int axis = static_cast<int>(sign[p][1]);
            for (int i = 0; i < 4; ++i) {
                const double w = col(3, i);
                planes[p][i] = p == 4 ? col(2, i) : (p == 5 ? w - col(2, i) : w + sign[p][0] * col(axis, i));
            }
            const double len = std::sqrt(planes[p][0] * planes[p][0] + planes[p][1] * planes[p][1] + planes[p][2] * planes[p][2]);
            if (len > 1e-9) {
                for (int i = 0; i < 4; ++i) planes[p][i] /= len;
            }
        }
    }
    const float viewFactor = std::max(engine::lod::scales().view, 0.01f);
    const double cx = cam.position[0], cy = cam.position[1], cz = cam.position[2];

    // the per-view constants of the base block: the camera (c0, c1) natively, the fog (c3, c4) from the engine's latest draw
    NativeItemBlock base;
    base.ps[0][0] = cam.position[0];
    base.ps[0][1] = cam.position[1];
    base.ps[0][2] = cam.position[2];
    base.ps[0][3] = cam.engineFar;
    if (ctx.sceneHdr != nullptr) {
        base.ps[1][0] = 0.5f;
        base.ps[1][1] = -0.5f;
        base.ps[1][2] = 1.0f / static_cast<float>(std::max(1u, ctx.sceneHdr->desc().width)) + 0.5f;
        base.ps[1][3] = 1.0f / static_cast<float>(std::max(1u, ctx.sceneHdr->desc().height)) + 0.5f;
    }
    std::memcpy(base.ps[3], view_.fog[0], 16);
    std::memcpy(base.ps[4], view_.fog[1], 16);
    const std::uint32_t linear = dev_.samplerFor(true, false), linearClamp = dev_.samplerFor(true, true);

    engine::mirror::get().forEach([&](const scene::Entity& e) {
        if (e.meshes.empty() || (e.state.flags & 0x8026u) != 0x8026u || (e.state.flags & 0x8u) != 0) return;  // as eCFrustumDatabase::ExtractFromEntity
        double sx = e.state.sphere[0], sy = e.state.sphere[1], sz = e.state.sphere[2], sr = e.state.sphere[3];
        if (!(sr > 0.0) || !std::isfinite(sr) || !std::isfinite(sx)) {
            sx = e.state.world[12];
            sy = e.state.world[13];
            sz = e.state.world[14];
            sr = 400.0;
        }
        ++stats_.listEntities;
        for (const auto& p : planes) {
            if (p[0] * sx + p[1] * sy + p[2] * sz + p[3] < -sr) return;  // outside the frustum
        }
        // the fade (CalculateFadeOut): base alpha, the view range with the 10 % fade at its end; anything fading is a blended op of the engine
        const double dx = sx - cx, dy = sy - cy, dz = sz - cz;
        const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const double range = static_cast<double>(e.state.viewRange) * viewFactor;
        if (e.state.alpha < 0.999f || (range > 0.0 && dist > range * 0.9)) {
            ++stats_.listFading;
            return;
        }
        NativeItemBlock lit = base;
        std::uint32_t lights = 0;
        if (!composeLighting(e.ptr, lit, lights)) {
            ++stats_.listNoLight;
            return;
        }
        for (std::uint32_t r = 0; r < 3; ++r) {
            for (std::uint32_t c = 0; c < 4; ++c) lit.row[r][c] = e.state.world[c * 4 + r];  // row r of the 3x4 = column r of the row-vector matrix
        }
        for (const scene::MeshInfo& mesh : e.meshes) {
            // The mirror's mesh data is refreshed every few frames: the engine may have switched the level of detail or evicted the resource
            // since (its buffers are freed with it, and the GPU would read freed memory: flickering black triangles). Check the context's live
            // resource: loaded (eCResource2 +0x38 == 3), the same visual / vertex data / index data as the mirror holds; else the engine draws it.
            {
                const void* res = nullptr;
                std::int32_t state = 0;
                const void* visual = nullptr;
                const void* vdata = nullptr;
                const void* idata = nullptr;
                if (!engine::readAt(mesh.context, 0x28, res) || res == nullptr || res != mesh.meshResource || !engine::readAt(res, 0x38, state) || state != 3 ||
                    !engine::readAt(res, 0x88, visual) || visual == nullptr || visual != mesh.visual || !engine::readAt(visual, 0x30, vdata) || vdata != mesh.vertexData[0] ||
                    !engine::readAt(visual, 0x58, idata) || idata != mesh.indexData) {
                    ++stats_.listStale;
                    continue;
                }
            }
            for (const scene::MeshPart& part : mesh.parts) {
                NativePart np;
                if (!parts_->resolve(mesh, part, np)) continue;
                if (np.pending) {
                    ++stats_.listPending;
                    continue;
                }
                Item it;
                it.block = lit;
                NativeItemBlock& blk = it.block;
                std::uint32_t flags = kFlagAmbComplex;
                const std::uint32_t arenaFirst = arena_.get(np);
                if (arenaFirst == NativeIndexArena::kNone) {
                    ++stats_.listNoIndices;
                    continue;
                }
                it.masked = np.masked;
                if (np.masked) {
                    flags |= kFlagMasked;
                    blk.params[0] = np.alphaRef;
                    blk.off1[3] |= 7u << 8;  // >= the reference
                }
                it.diffuse = np.diffuse;
                it.diffuseSampler = linear;
                if (it.diffuse == nullptr) flags |= kFlagNoAlbedo;
                it.normal = np.normal;
                it.normalSampler = linear;
                if (it.normal != nullptr) flags |= kFlagNormalMap | kFlagDxt5nm;
                if (np.lightmap[0] != nullptr && np.lightmap[1] != nullptr && np.layout.lightmap != 0xFFFFu) {
                    it.lightmap[0] = np.lightmap[0];
                    it.lightmap[1] = np.lightmap[1];
                    it.lightmapSampler = linearClamp;
                    flags |= kFlagLightmap;
                }
                blk.off1[3] |= lights;
                blk.tex2[1] = flags;
                blk.tex2[2] = np.stride;
                blk.off0[0] = np.layout.normal;
                blk.off0[1] = np.layout.tangent;
                blk.off0[2] = np.layout.color0;
                blk.off0[3] = np.layout.color1;
                const std::uint32_t uvSet = std::min<std::uint32_t>(np.diffuseUvSet, 1u);
                blk.off1[0] = np.layout.uv[uvSet] != 0xFFFFu ? np.layout.uv[uvSet] : np.layout.uv[0];
                const std::uint32_t nSet = std::min<std::uint32_t>(np.normalUvSet, 1u);
                blk.off1[1] = np.layout.uv[nSet] != 0xFFFFu ? np.layout.uv[nSet] : np.layout.uv[0];
                blk.off1[2] = np.layout.lightmap;
                blk.addr[0] = static_cast<std::uint32_t>(np.vertices);
                blk.addr[1] = static_cast<std::uint32_t>(np.vertices >> 32);
                it.arenaFirst = arenaFirst;
                it.ibId = np.ibId;
                it.firstIndex = np.first;
                it.indexCount = np.indexCount;
                it.dist = static_cast<float>(dist);
                it.entity = e.ptr;
                it.subMesh = part.subMesh;
                items_.push_back(std::move(it));
            }
        }
    });
    // two classes, each front to back (the pre-pass rejects the most hidden fragments early): opaque first, then alpha tested
    std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) { return a.masked != b.masked ? !a.masked : a.dist < b.dist; });
    for (std::uint32_t i = 0; i < items_.size(); ++i) listIndex_[partKey(items_[i].entity, items_[i].ibId, items_[i].firstIndex, items_[i].indexCount)] = i;
    listSeen_.assign(items_.size(), 0);
    stats_.listParts += items_.size();
    ++stats_.listBuilt;
}

}  // namespace rvk::enh
