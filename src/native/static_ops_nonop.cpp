// N5.2 scene ops: the main view's non-op paths of eCFrustumDatabase::Execute (docs/research/engine-frame/README.md "8. N5.2
// result"): eCDecalAdmin::RenderDecals 0x4fc220, re-written from the decompiled function (frameD_dump1.txt): the queue (decals
// whose entity was in this database's view), the engine's own qsort with the engine's comparator (SortRenderQueue 0x4fcf80),
// the lock cycles (<= 6000 vertices / 10000 indices), the records (material, image, lightmap mesh, light list, outdoor factor;
// a decal with static lights / static SH / a lightmap mesh alone), the CPU vertex data (dynamic decals transformed by the
// entity's world matrix with SharedBase's bCVector functions, COLOR0 = alpha x 255 << 24 | 0xffffff), and per record the
// material's (or the admin's default shader with the record's image) passes through the GetPasses / ExecutePass rules of
// static_ops.cpp. Part of StaticOps (static_ops.h).

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "d3d9/defs.h"
#include "engine/memread.h"
#include "engine/symbols.h"
#include "hal/vulkan/hal_vulkan.h"
#include "native/static_ops.h"

namespace rvk::native {

namespace {

using engine::readAt;
using engine::safeRead;
using U8 = const std::uint8_t;

constexpr std::uint32_t kQsortRva = 0x4a4aa0;          // Engine.dll's statically linked qsort (frame_tap.cpp)
constexpr std::uint32_t kAlphaScaleRva = 0x9f17b4;     // DAT_1809f17b4: COLOR0's alpha scale (255)
constexpr std::uint32_t kOneRva = 0x9f1698;            // DAT_1809f1698: 1.0f (global alpha, the static decals' alpha test)
constexpr std::uint32_t kDecalStride = 0x3c;           // the decal vertex (CreateBuffers 0x4fbd80)
constexpr std::int32_t kMaxVertices = 6000, kMaxIndices = 10000;

template <class T>
T get(const void* p, std::size_t off, T fallback = T{}) {
    T v = fallback;
    if (!readAt(p, off, v)) return fallback;
    return v;
}
U8* ptr(const void* p, std::size_t off) { return get<U8*>(p, off, nullptr); }

std::int32_t rel32(const std::uint8_t* p) {
    std::int32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

// a decal's entity (eCEntityProxy at decal +0x10): the engine's own fast path when the proxy's slot +8 is
// eCEntityProxy::ResolveEntity (the holder +0x18, its first field); null when the proxy is of another class (`ok` false)
U8* decalEntity(U8* decal, bool& ok) {
    U8* proxyVt = ptr(decal, 0x10);
    ok = proxyVt != nullptr && ptr(proxyVt, 8) == static_cast<U8*>(engine::sym().entityProxyResolve);
    if (!ok) return nullptr;
    U8* holder = ptr(decal, 0x18);
    return holder != nullptr ? ptr(holder, 0) : nullptr;
}

constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

}  // namespace

bool StaticOps::verifyDecals() {
    const auto& s = engine::sym();
    if (s.entityProxyResolve == nullptr || s.decalSortRenderQueue == nullptr || s.vectorTransform == nullptr ||
        s.vectorTransformDirection == nullptr || s.decalsRender == nullptr || engine_ == nullptr) {
        spdlog::warn("native: scene ops: decal symbols missing: decals stay the engine's");
        return false;
    }
    // RenderDecals' sort: lea r9, [rip + SortRenderQueue] ... call qsort (Engine.dll's own)
    std::array<std::uint8_t, 0x700> b{};
    if (!safeRead(s.decalsRender, b.data(), b.size())) return false;
    bool found = false;
    for (std::size_t i = 0; i + 7 < b.size() && !found; ++i) {
        if (b[i] != 0x4c || b[i + 1] != 0x8d || b[i + 2] != 0x0d) continue;
        const auto* at = static_cast<U8*>(s.decalsRender) + i;
        if (at + 7 + rel32(&b[i + 3]) != s.decalSortRenderQueue) continue;
        for (std::size_t j = i + 7; j + 5 <= std::min(b.size(), i + 40); ++j) {
            if (b[j] == 0xe8 && static_cast<U8*>(s.decalsRender) + j + 5 + rel32(&b[j + 1]) == engine_ + kQsortRva) found = true;
        }
    }
    if (!found) {
        spdlog::warn("native: scene ops: RenderDecals' qsort call site not found: decals stay the engine's");
        return false;
    }
    if (!readAt(engine_ + kAlphaScaleRva, 0, decalAlphaScale_) || !readAt(engine_ + kOneRva, 0, one_) || one_ != 1.0f) return false;
    transform_ = reinterpret_cast<VecFn>(s.vectorTransform);
    transformDirection_ = reinterpret_cast<VecFn>(s.vectorTransformDirection);
    decalCmp_ = reinterpret_cast<int (*)(const void*, const void*)>(s.decalSortRenderQueue);
    if (qsort_ == nullptr) qsort_ = reinterpret_cast<QsortFn>(const_cast<std::uint8_t*>(engine_ + kQsortRva));
    spdlog::info("native: scene ops: decal checks passed (qsort site, comparator {:#x}, alpha scale {})",
                 static_cast<U8*>(s.decalSortRenderQueue) - engine_, decalAlphaScale_);
    return true;
}

bool StaticOps::geometryOfVisual(const std::uint8_t* visual, Geom& g) {
    const auto* prim = hal::vulkan::HalPrimitive::fromApi(ptr(visual, 0x10));
    if (prim == nullptr) return false;
    g.decl = prim->vertexDecl();
    g.indices = prim->indexBuffer();
    for (std::uint32_t i = 0; i < 4; ++i) {
        const auto v = prim->streamView(i);
        g.streams[i] = v.enabled && v.buffer != nullptr ? sc::DrawDesc::Stream{v.buffer, v.offset, v.stride, v.freq} : sc::DrawDesc::Stream{};
    }
    g.visual.fill = get<std::uint32_t>(visual, 0x18, 3);
    g.visual.cull = get<std::uint32_t>(visual, 0x1c, 2);
    g.visual.depthFunc = get<std::uint32_t>(visual, 0x20, 4);
    g.visual.depthWrite = get<std::uint32_t>(visual, 0x24, 2);
    return true;
}

// GetPasses(shader, switch, illum, combi, not instanced, flags 0) + ExecutePass + the visual's states, per pass
StaticOps::Skip StaticOps::nonOpPasses(const std::uint8_t* shader, const std::uint8_t* illum, std::uint32_t combi, const Geom& geometry,
                                       std::int32_t switchValue) {
    if (!shaderClassOk(shader)) return Skip::ShaderClass;
    if (ptr(shader, 0x220) != view_.admin) return Skip::Unreadable;
    if (get<std::uint8_t>(shader, 0x2a8) == 1) return Skip::InvalidShader;
    if (get<std::uint32_t>(view_.admin, 0x31c) != 0) return Skip::InvalidShader;
    sc::PassInputs in;
    in.illum = illum;
    U8* list = ptr(illum, 0);
    in.hasList = list != nullptr;
    in.listSun = get<std::uint8_t>(list, 0);
    in.illumNoSun = get<std::uint8_t>(illum, 0xc);
    in.staticLights = get<std::uint8_t>(illum, 0xd);
    in.dynamicLights = get<std::uint8_t>(list, 0x34);
    in.combi = combi;
    in.instanced = false;
    in.switchValue = switchValue;
    std::vector<sc::Pass> passes;
    const std::uint8_t* version = nullptr;
    if (const Skip k = shaderPasses(shader, in, version, passes); k != Skip::None) return k;
    if (version == nullptr) return Skip::None;
    for (const sc::Pass& p : passes) {
        const std::size_t before = drawCount_;
        if (const Skip s = passDraw(shader, shader, version, p, geometry, false); s != Skip::None) return s;
        if (drawCount_ > before && draws_[drawCount_ - 1]->drawClass == sc::DrawClass::Base) draws_[drawCount_ - 1]->drawClass = sc::DrawClass::NonOp;
    }
    return Skip::None;
}

StaticOps::Skip StaticOps::composeDecals(const std::uint8_t* admin, const std::uint8_t* db) {
    // CreateBuffers / CreateDefaultShader made by the engine's first call (composed from the next frame on)
    U8* defaultShader = ptr(admin, 0x48);
    U8* sampler = ptr(admin, 0x50);
    U8* visual = ptr(admin, 0x58);
    if (ptr(admin, 0x60) == nullptr || ptr(admin, 0x68) == nullptr || defaultShader == nullptr || sampler == nullptr || visual == nullptr) {
        return Skip::NotCreated;
    }
    if (!view_.valid) {
        U8* a = ptr(defaultShader, 0x220);
        if (a == nullptr || !composeView(a)) return Skip::Unreadable;
    }
    if (ptr(visual, 0x60) != nullptr) return Skip::VisualShader;
    initMirror();
    Geom base;
    if (!geometryOfVisual(visual, base)) return Skip::Primitive;
    if (base.streams[0].buffer == nullptr || base.streams[0].stride != kDecalStride) return Skip::Primitive;

    // the queue: decals whose entity resolves, was in this database's view (+0xe4 == db +8), has vertices and indices and an
    // illumination holder (+0xf8): {decal, holder + 0x78}
    const std::int32_t count = get<std::int32_t>(admin, 0x28);
    U8* array = ptr(admin, 0x20);
    std::vector<std::uint8_t> queue;
    for (std::int32_t i = 0; i < count; ++i) {
        U8* decal = ptr(array, 8 * static_cast<std::size_t>(i));
        bool ok = false;
        U8* entity = decalEntity(decal, ok);
        if (!ok) return Skip::DecalProxy;
        if (entity == nullptr) continue;
        if (db != nullptr && get<std::int32_t>(entity, 0xe4) != get<std::int32_t>(db, 8)) continue;
        if (get<std::int32_t>(decal, 0x30) == 0 || get<std::int32_t>(decal, 0x48) == 0) continue;
        U8* holder = ptr(entity, 0xf8);
        if (holder == nullptr) continue;
        U8* entry[2] = {decal, holder + 0x78};
        const std::size_t at = queue.size();
        queue.resize(at + 16);
        std::memcpy(queue.data() + at, entry, 16);
    }
    const std::size_t n = queue.size() / 16;
    if (n == 0) return Skip::None;
    qsort_(queue.data(), n, 16, decalCmp_);
    const auto decalAt = [&](std::size_t q) { U8* p = nullptr; std::memcpy(&p, queue.data() + 16 * q, 8); return p; };
    const auto illumAt = [&](std::size_t q) { U8* p = nullptr; std::memcpy(&p, queue.data() + 16 * q + 8, 8); return p; };

    struct Record {
        U8* material = nullptr;
        U8* image = nullptr;
        U8* lightmapMesh = nullptr;
        U8* illum = nullptr;
        std::int32_t firstVertex = 0, vertexCount = 0, startIndex = 0, indexCount = 0;
    };
    std::vector<Record> records;
    std::vector<std::uint8_t> vb(static_cast<std::size_t>(kMaxVertices) * kDecalStride);
    std::vector<std::uint16_t> ib(kMaxIndices);
    std::int32_t vtx = 0, idx = 0;
    bool locked = false;
    // the record-break state (the previous record's keys)
    U8* prevMat = nullptr;
    U8* prevImg = nullptr;
    U8* prevLm = nullptr;
    U8* prevList = nullptr;
    float prevF = one_;
    bool alone = false;
    std::vector<std::uint8_t> src;
    const auto flush = [&]() -> Skip {
        for (const Record& r : records) {
            U8* shader = nullptr;
            if (r.material == nullptr) {
                if (r.image == nullptr) continue;
                shader = defaultShader;
                // eCColorSrcSampler::SetImageResource(admin +0x50, image): the sampler's only image, UpdateSampler writes it into
                // the pool slot (+0x90 pool, +0x98 slot)
                if (ptr(sampler, 0x90) != defaultShader + 0xf8) return Skip::Unreadable;
                const std::int32_t slot = get<std::int32_t>(sampler, 0x98, -1);
                if (slot < 0 || slot >= 16) return Skip::Unreadable;
                poolsOf(defaultShader)->slotImage[static_cast<std::size_t>(slot)] = r.image;
            } else {
                shader = ptr(r.material, 0x70);
                if (shader == nullptr) continue;
            }
            writeAlpha(0, one_);
            writeWorld(0, kIdentity);
            const std::uint32_t combi = r.lightmapMesh != nullptr ? bindLightmaps(r.lightmapMesh) : 0;
            Geom g = base;
            g.minIndex = static_cast<std::uint32_t>(r.firstVertex);
            g.numVertices = static_cast<std::uint32_t>(r.vertexCount);
            g.startIndex = static_cast<std::uint32_t>(r.startIndex);
            g.primCount = static_cast<std::uint32_t>(r.indexCount / 3);
            const std::size_t before = drawCount_;
            if (const Skip k = nonOpPasses(shader, r.illum, combi, g); k != Skip::None) return k;
            for (std::size_t d = before; d < drawCount_; ++d) {
                sc::DrawDesc& dd = *draws_[d];
                dd.vertexStride = kDecalStride;
                const auto* v0 = vb.data() + static_cast<std::size_t>(r.firstVertex) * kDecalStride;
                dd.vertexBytes.assign(v0, v0 + static_cast<std::size_t>(r.vertexCount) * kDecalStride);
                const auto* i0 = reinterpret_cast<const std::uint8_t*>(ib.data() + r.startIndex);
                dd.indexBytes.assign(i0, i0 + static_cast<std::size_t>(g.primCount) * 6);
            }
        }
        records.clear();
        locked = false;
        return Skip::None;
    };
    for (std::size_t q = 0; q < n; ++q) {
        U8* decal = decalAt(q);
        const std::int32_t vc = get<std::int32_t>(decal, 0x30);
        const std::int32_t ic = get<std::int32_t>(decal, 0x48);
        if (vc > kMaxVertices || ic > kMaxIndices) continue;  // too large: never drawn
        if (kMaxVertices < vc + vtx || kMaxIndices < ic + idx) {
            if (!records.empty()) {
                if (const Skip k = flush(); k != Skip::None) return k;
            }
        }
        if (!locked) {
            // VB / IB locked DISCARD at offset 0: a new cycle
            vtx = 0;
            idx = 0;
            locked = true;
        }
        U8* il = illumAt(q);
        U8* list = ptr(il, 0);
        const float f = get<float>(il, 8);
        if (records.empty() || ptr(decal, 0) != prevMat || ptr(decal, 8) != prevImg || ptr(decal, 0x58) != prevLm || list != prevList ||
            f != prevF || alone) {
            Record r;
            r.material = ptr(decal, 0);
            r.image = ptr(decal, 8);
            r.lightmapMesh = ptr(decal, 0x58);
            r.illum = il;
            r.firstVertex = vtx;
            r.startIndex = idx;
            records.push_back(r);
            prevMat = r.material;
            prevImg = r.image;
            prevLm = r.lightmapMesh;
            prevList = list;
            alone = get<std::uint8_t>(il, 0xd) != 0 || get<std::uint8_t>(il, 0x90) != 0 || r.lightmapMesh != nullptr;
            prevF = f;
        }
        // indices first (rebased to this decal's first vertex), then the vertices
        U8* srcIdx = ptr(decal, 0x40);
        std::vector<std::uint16_t> ind(static_cast<std::size_t>(ic));
        if (!safeRead(srcIdx, ind.data(), ind.size() * 2)) return Skip::Unreadable;
        for (std::int32_t k = 0; k < ic; ++k) ib[static_cast<std::size_t>(idx + k)] = static_cast<std::uint16_t>(ind[static_cast<std::size_t>(k)] + static_cast<std::uint16_t>(vtx));
        idx += ic;
        src.resize(static_cast<std::size_t>(vc) * kDecalStride);
        if (!safeRead(ptr(decal, 0x28), src.data(), src.size())) return Skip::Unreadable;
        const float alpha = get<float>(decal, 0x50);
        const std::uint32_t colour = static_cast<std::uint32_t>(static_cast<std::int32_t>(alpha * decalAlphaScale_)) << 24 | 0xffffffu;
        std::uint8_t* dst = vb.data() + static_cast<std::size_t>(vtx) * kDecalStride;
        if (get<std::uint8_t>(decal, 0x54) == 1) {
            bool ok = false;
            U8* entity = decalEntity(decal, ok);
            if (entity == nullptr) return Skip::Unreadable;
            float m[16];
            if (!safeRead(entity + 0x60, m, sizeof(m))) return Skip::Unreadable;
            for (std::int32_t k = 0; k < vc; ++k) {
                float v[15];
                std::memcpy(v, src.data() + static_cast<std::size_t>(k) * kDecalStride, sizeof(v));
                transform_(&v[0], m);
                transformDirection_(&v[3], m);
                transformDirection_(&v[6], m);
                std::memcpy(&v[9], &colour, 4);
                std::memcpy(dst + static_cast<std::size_t>(k) * kDecalStride, v, sizeof(v));
            }
        } else if (alpha < one_) {
            for (std::int32_t k = 0; k < vc; ++k) {
                std::uint8_t* d = dst + static_cast<std::size_t>(k) * kDecalStride;
                std::memcpy(d, src.data() + static_cast<std::size_t>(k) * kDecalStride, kDecalStride);
                std::memcpy(d + 0x24, &colour, 4);
            }
        } else {
            std::memcpy(dst, src.data(), src.size());
        }
        vtx += vc;
        records.back().vertexCount += vc;
        records.back().indexCount += ic;
    }
    if (!records.empty()) return flush();
    return Skip::None;
}

bool StaticOps::nonOpGroupBegin(Group group, void* admin) {
    finishOp();
    GroupState& gs = groups_[static_cast<std::size_t>(group)];
    if (!gs.active() || !inView_) return false;
    if (verified_ == 0) verified_ = verify() ? 1 : -1;
    if (verified_ < 0) return false;
    g_ = &gs;
    drawCount_ = 0;
    std::memcpy(entryVsB_.data(), dev_.vsBoolConstants(), sizeof(entryVsB_));
    std::memcpy(entryPsB_.data(), dev_.psBoolConstants(), sizeof(entryPsB_));
    std::memcpy(entryVsI_, dev_.vsIntConstants(), sizeof(entryVsI_));
    std::memcpy(entryPsI_, dev_.psIntConstants(), sizeof(entryPsI_));
    nonOp_ = {};
    nonOp_.raw.context = admin;
    nonOp_.list = List::Count;  // the per-list counts' "non-op" slot
    return true;
}

void StaticOps::nonOpGroupComposed(Skip s) {
    blendOverrideShader_ = nullptr;
    blendOverride_ = -1;
    if (g_ == nullptr) return;
    ++g_->window.ops;
    if (s != Skip::None) {
        ++g_->skips[static_cast<std::size_t>(s)];
        ++g_->window.opsNotComposed;
        drawCount_ = 0;
        g_ = nullptr;
        return;
    }
    ++g_->window.opsComposed;
    op_ = &nonOp_;
    inNonOp_ = true;
    engineDraws_ = 0;
    opDiffLogged_ = false;
    taken_ = false;
    if (g_->takeOver) {
        issue();
        taken_ = true;
    }
}

void StaticOps::decalsBegin(void* admin, void* db) {
    if (!nonOpGroupBegin(Group::Decals, admin)) return;
    if (decalsVerified_ == 0) decalsVerified_ = verifyDecals() ? 1 : -1;
    if (decalsVerified_ < 0) {
        g_ = nullptr;
        return;
    }
    nonOpGroupComposed(composeDecals(static_cast<U8*>(admin), static_cast<U8*>(db)));
}

void StaticOps::billboardsBegin(void* admin) {
    if (!nonOpGroupBegin(Group::Billboards, admin)) return;
    if (billboardsVerified_ == 0) billboardsVerified_ = verifyBillboards() ? 1 : -1;
    if (billboardsVerified_ < 0) {
        g_ = nullptr;
        return;
    }
    nonOpGroupComposed(composeBillboards(static_cast<U8*>(admin)));
}

void StaticOps::nonOpEnd() {
    if (!inNonOp_) return;
    finishOp();
    inNonOp_ = false;
    inSkyCube_ = false;
}

// ---- the opaque billboards' queue (eCBillboardAdmin::RenderBillboards 0x4f0530, read in the disassembly: the decompile lost
// the vertex / index buffer writes; n52c_dump1.txt) ----------------------------------------------------------------------------

namespace {
constexpr std::uint32_t kBillboardStride = 0x34;    // CreateBuffers 0x4f00a0: POSITION, NORMAL, TANGENT, TEXCOORD0, TEXCOORD5
constexpr std::uint32_t kBillboardMaxQuads = 1000;  // 0x4f0696: cmp ecx, 0x3e8 (VB DAT_18097115c 4000 vertices, IB 6000 indices)
struct BbSite {
    std::uint32_t rva;
    std::array<std::uint8_t, 8> b;
    std::uint32_t n;
};
constexpr BbSite kBillboardSites[] = {
    {0x4f0696, {0x81, 0xf9, 0xe8, 0x03, 0, 0, 0, 0}, 6},     // the cycle's quad limit
    {0x97115c, {0xa0, 0x0f, 0, 0, 0x70, 0x17, 0, 0}, 8},     // the buffers' sizes
};
}  // namespace

bool StaticOps::verifyBillboards() {
    const auto& s = engine::sym();
    if (s.billboardSortRenderQueue == nullptr || s.billboardsRender == nullptr || engine_ == nullptr) {
        spdlog::warn("native: scene ops: billboard symbols missing: the opaque billboards stay the engine's");
        return false;
    }
    // the sort: lea r9, [rip + SortRenderQueue] (+0x54) ... call qsort (+0x9c)
    std::array<std::uint8_t, 0xa4> b{};
    const auto* at = static_cast<U8*>(s.billboardsRender);
    if (at != engine_ + 0x4f0530 || !safeRead(at, b.data(), b.size())) return false;
    constexpr std::size_t kLea = 0x4f0584 - 0x4f0530, kCall = 0x4f05cc - 0x4f0530;
    if (b[kLea] != 0x4c || b[kLea + 1] != 0x8d || b[kLea + 2] != 0x0d || at + kLea + 7 + rel32(&b[kLea + 3]) != s.billboardSortRenderQueue ||
        b[kCall] != 0xe8 || at + kCall + 5 + rel32(&b[kCall + 1]) != engine_ + kQsortRva) {
        spdlog::warn("native: scene ops: RenderBillboards' qsort call site not found: the opaque billboards stay the engine's");
        return false;
    }
    for (const BbSite& x : kBillboardSites) {
        std::array<std::uint8_t, 8> got{};
        if (!safeRead(engine_ + x.rva, got.data(), x.n) || !std::equal(got.begin(), got.begin() + x.n, x.b.begin())) {
            spdlog::warn("native: scene ops: RenderBillboards differs at RVA {:#x}: the opaque billboards stay the engine's", x.rva);
            return false;
        }
    }
    billboardCmp_ = reinterpret_cast<int (*)(const void*, const void*)>(s.billboardSortRenderQueue);
    if (qsort_ == nullptr) qsort_ = reinterpret_cast<QsortFn>(const_cast<std::uint8_t*>(engine_ + kQsortRva));
    spdlog::info("native: scene ops: billboard checks passed (qsort site, comparator {:#x}, {} quads per cycle)",
                 static_cast<U8*>(s.billboardSortRenderQueue) - engine_, kBillboardMaxQuads);
    return true;
}

StaticOps::Skip StaticOps::composeBillboards(const std::uint8_t* admin) {
    // CreateBuffers / CreateDefaultMaterial run first in RenderBillboards: composed from the next call on
    U8* defaultMaterial = ptr(admin, 0x30);
    U8* sampler = ptr(admin, 0x38);
    U8* visual = ptr(admin, 0x40);
    U8* defaultShader = ptr(defaultMaterial, 0x70);
    if (visual == nullptr || ptr(admin, 0x48) == nullptr || ptr(admin, 0x50) == nullptr || defaultShader == nullptr || sampler == nullptr) {
        return Skip::NotCreated;
    }
    const std::int32_t count = get<std::int32_t>(admin, 0x28);
    if (count <= 0) return Skip::None;
    if (!view_.valid) {
        U8* a = ptr(defaultShader, 0x220);
        if (a == nullptr || !composeView(a)) return Skip::Unreadable;
    }
    if (ptr(visual, 0x60) != nullptr) return Skip::VisualShader;
    const auto* prim = hal::vulkan::HalPrimitive::fromApi(ptr(visual, 0x10));
    if (prim == nullptr || !prim->drawDesc().indexed) return Skip::Primitive;
    const auto primType = static_cast<std::uint32_t>(prim->drawDesc().type);
    initMirror();
    Geom base;
    if (!geometryOfVisual(visual, base)) return Skip::Primitive;
    if (base.streams[0].buffer == nullptr || base.streams[0].stride != kBillboardStride) return Skip::Primitive;
    // the queue (admin +0x20, entries 0x18: billboard, light set, blend mode, switch) sorted by the engine's qsort + comparator
    std::vector<std::uint8_t> queue(static_cast<std::size_t>(count) * 0x18);
    if (!safeRead(ptr(admin, 0x20), queue.data(), queue.size())) return Skip::Unreadable;
    qsort_(queue.data(), static_cast<std::size_t>(count), 0x18, billboardCmp_);
    std::vector<sc::BillboardEntry> entries(static_cast<std::size_t>(count));
    std::vector<U8*> boards(static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::uint8_t* q = queue.data() + 0x18 * i;
        U8* bb = nullptr;
        std::memcpy(&bb, q, 8);
        sc::BillboardEntry& e = entries[i];
        std::memcpy(&e.lightSet, q + 8, 8);
        std::memcpy(&e.blend, q + 0x10, 4);
        std::memcpy(&e.switchValue, q + 0x14, 4);
        e.material = ptr(bb, 0);
        e.image = ptr(bb, 8);
        const std::int32_t quads = get<std::int32_t>(bb, 0x20);
        // more than a cycle holds would overflow the engine's buffers
        if (quads < 0 || static_cast<std::uint32_t>(quads) > kBillboardMaxQuads) return Skip::Unreadable;
        e.quads = static_cast<std::uint32_t>(quads);
        boards[i] = bb;
    }
    std::vector<sc::BillboardRecord> records;
    std::vector<sc::BillboardPlace> places;
    sc::planBillboards(entries, kBillboardMaxQuads, records, places);
    // the cycles' vertex data: each billboard's prebuilt vertices (+0x18, 4 x 0x34 per quad) copied to its quad
    std::uint32_t cycles = 0;
    for (const sc::BillboardRecord& r : records) cycles = std::max(cycles, r.cycle + 1);
    std::vector<std::vector<std::uint8_t>> vb(cycles, std::vector<std::uint8_t>(static_cast<std::size_t>(kBillboardMaxQuads) * 4 * kBillboardStride));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (places[i].cycle < 0) continue;
        const std::size_t bytes = static_cast<std::size_t>(entries[i].quads) * 4 * kBillboardStride;
        std::uint8_t* dst = vb[static_cast<std::size_t>(places[i].cycle)].data() + static_cast<std::size_t>(places[i].quad) * 4 * kBillboardStride;
        if (!safeRead(ptr(boards[i], 0x18), dst, bytes)) return Skip::Unreadable;
    }
    static constexpr float kIdentityM[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (const sc::BillboardRecord& r : records) {
        // the record's shader: its material's (the resource touched / cached by the engine first), else the admin's default
        // shader with the record's BlendMode (+0x50) and image (eCColorSrcSampler::SetImageResource(admin +0x38))
        U8* shader = nullptr;
        blendOverrideShader_ = nullptr;
        blendOverride_ = -1;
        if (r.keys.material != nullptr) {
            shader = ptr(r.keys.material, 0x70);
        } else if (r.keys.image != nullptr) {
            shader = defaultShader;
            blendOverrideShader_ = defaultShader;
            blendOverride_ = r.keys.blend;
            if (ptr(sampler, 0x90) != defaultShader + 0xf8) return Skip::Unreadable;
            const std::int32_t slot = get<std::int32_t>(sampler, 0x98, -1);
            if (slot < 0 || slot >= 16) return Skip::Unreadable;
            poolsOf(defaultShader)->slotImage[static_cast<std::size_t>(slot)] = r.keys.image;
        }
        if (shader == nullptr) continue;
        // SetGlobalAlpha(1, 0), SetWorldMatrix(identity, 0)
        writeAlpha(0, 1.0f);
        writeWorld(0, kIdentityM);
        // the illumination built on the stack: list = light set +0x10 (or null), f 1, +0xc / +0xd 0, no static SH (+0x90 0)
        alignas(16) std::uint8_t illum[0x120]{};
        U8* list = r.keys.lightSet != nullptr ? static_cast<U8*>(r.keys.lightSet) + 0x10 : nullptr;
        const float one = 1.0f;
        std::memcpy(illum, &list, 8);
        std::memcpy(illum + 8, &one, 4);
        // the visual's record: min vertex 4 first, 4 n vertices, start index 6 first, 2 n primitives, base 0
        Geom g = base;
        g.minIndex = r.firstQuad * 4;
        g.numVertices = r.quads * 4;
        g.startIndex = r.firstQuad * 6;
        g.primCount = r.quads * 2;
        const std::size_t before = drawCount_;
        if (const Skip k = nonOpPasses(shader, illum, 0, g, r.keys.switchValue); k != Skip::None) return k;
        std::vector<std::uint8_t> idx;
        sc::billboardQuadIndices(r.firstQuad, r.quads, idx);
        const auto* v0 = vb[r.cycle].data() + static_cast<std::size_t>(r.firstQuad) * 4 * kBillboardStride;
        for (std::size_t d = before; d < drawCount_; ++d) {
            sc::DrawDesc& x = *draws_[d];
            x.primType = primType;
            x.vertexStride = kBillboardStride;
            x.vertexBytes.assign(v0, v0 + static_cast<std::size_t>(r.quads) * 4 * kBillboardStride);
            x.indexBytes = idx;
        }
    }
    blendOverrideShader_ = nullptr;
    blendOverride_ = -1;
    return Skip::None;
}

bool StaticOps::geometryBytesEqual(const sc::DrawDesc& c, const sc::DrawDesc& captured, std::string& why) {
    if (c.vertexBytes.empty()) return true;
    const std::uint32_t cs = c.cpuStream;
    auto* vb = const_cast<d3d9::Buffer*>(static_cast<const d3d9::Buffer*>(captured.streams[cs].buffer));
    auto* ib = const_cast<d3d9::Buffer*>(static_cast<const d3d9::Buffer*>(captured.indices));
    std::vector<std::uint8_t> got(c.vertexBytes.size());
    const std::uint32_t first = cs == 0 ? (captured.indexed ? captured.minIndex : captured.startIndex) : 0u;
    if (vb == nullptr || !vb->readCpu(captured.streams[cs].offset + first * c.vertexStride, static_cast<std::uint32_t>(got.size()), got.data())) {
        why = std::format("vertex data of stream {} unreadable", cs);
        return false;
    }
    if (got != c.vertexBytes) {
        std::size_t at = 0;
        while (at < got.size() && got[at] == c.vertexBytes[at]) ++at;
        why = std::format("vertex data of stream {} differs at vertex {} byte {:#x}", cs, first + at / c.vertexStride, at % c.vertexStride);
        return false;
    }
    if (c.indexBytes.empty()) return true;
    got.resize(c.indexBytes.size());
    if (ib == nullptr || !ib->readCpu(captured.startIndex * 2, static_cast<std::uint32_t>(got.size()), got.data())) {
        why = "index data unreadable";
        return false;
    }
    if (got != c.indexBytes) {
        why = "index data differs";
        return false;
    }
    return true;
}

}  // namespace rvk::native
