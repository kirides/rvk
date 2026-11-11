// F7-2 owned vegetation draws (docs/research/frame-ownership.md 7.42, setting native.owned_vegetation_ops): see veg_owned.h.
//
// vegCompBegin / vegCompNote / vegCompDraw / vegCompEnd   hooks of composeVegetation + opEnterImpl: the composition's draws are kept as templates per bucket (type, pass, sun); on the
//                  composed frames the plan made at the op's entry is compared with the composition (batches, buckets, c31 / c32, the view registers, the converted instances) and the
//                  new draws with the retained templates (DIFFERING must stay 0)
// vegViewBegin / vegSelect / vegTake   the main view begins (frame tap): the op records whose stamps are valid leave the engine's z / solid lists
// vegPass          BeginZPass / DoBeginRenderSolid returned: the taken ops are planned from the engine's grid nodes, their templates made ready (a persistent DrawBlock each), one
//                  DrawRecord per op x lighting entry written, every op's retained converted instance bytes copied into one ring block, one indirect command per bucket; an op that
//                  cannot be drawn this way is composed right there (the engine's slot never runs for a taken op)

#include <windows.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <format>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "d3d9/defs.h"
#include "enh/light_table.h"
#include "engine/memread.h"
#include "gpuscene/illumination_table.h"
#include "gpuscene/lighting_images.h"
#include "gpuscene/material_table.h"
#include "gpuscene/view_table.h"
#include "hal/vulkan/hal_vulkan.h"
#include "native/static_ops.h"

namespace rvk::native {

namespace {

using engine::readAt;
using engine::safeRead;
using U8 = const std::uint8_t;
using Clock = std::chrono::steady_clock;

template <class T>
inline T get(const void* p, std::size_t off, T fallback = T{}) {
    T v = fallback;
    return readAt(p, off, v) ? v : fallback;
}
inline U8* ptr(const void* p, std::size_t off) { return get<U8*>(p, off, nullptr); }
inline bool rd(const void* p, std::size_t off, void* out, std::size_t n) { return p != nullptr && safeRead(static_cast<U8*>(p) + off, out, n); }
inline std::uint64_t ns(Clock::time_point a, Clock::time_point b) { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()); }
inline std::uint64_t mixh(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

constexpr std::uint32_t kViewSlotVegZ = 12, kViewSlotVegSolid = 13;  // (trees: 14 / 15 and 8..11; every v2 slot < 16)
constexpr std::uint32_t kPadRegs = 32;                               // registers in front of the op blocks: window 2 starts at c31 (D3D12: offset >= first x 16)
constexpr std::uint32_t kBlockRegs = 2;                              // c31, c32 per op
constexpr std::uint32_t kStandBytes = 0x2c * 64;

inline gpuscene::ComposeParams paramsOf(const orec::DrawRec& r) {
    gpuscene::ComposeParams p;
    p.tableLit = r.lightTableLit;
    p.holdsAll = r.lightHoldsAll;
    p.n = r.lightN;
    p.firstLight = r.lightFirst;
    return p;
}

template <std::size_t N>
struct Snap {
    alignas(8) std::uint8_t b[N];
    bool ok = false;
    void load(const void* p) { ok = p != nullptr && engine::safeReadN<N>(p, b); }
    template <class T>
    T at(std::size_t off) const {
        T v;
        std::memcpy(&v, b + off, sizeof(T));
        return v;
    }
};

// the vertex shader's registers of window 2 the owned block gives: c31.xy = the fade, c32.w = 1 (the others are not read by the grass vertex shader)
inline bool sameBits(float a, float b) { return std::memcmp(&a, &b, 4) == 0; }

}  // namespace

void StaticOps::setOwnedVegetationOps(bool on) {
    if (on == ownedVegOps_) return;
    ownedVegOps_ = on;
    if (on) {
        if (!vro_) vro_ = std::make_unique<vro::State>();
    } else if (vro_) {
        vro_->active = false;
        for (auto& v : vro_->taken) v.clear();
        vro_->pend = {};
        vro_->ops.clear();
        vro_->tripped = false;
    }
    spdlog::info("native: owned vegetation draws {}", on ? "on (main view vegetation ops: templates per bucket, check frames compare, owned frames draw every batch from the engine's grid nodes)" : "off");
}

bool StaticOps::vegEnsureStand() {
    if (!vro_) vro_ = std::make_unique<vro::State>();
    if (vro_->stand) return true;
    using namespace d3d9;
    auto vb = dev_.createBuffer(kStandBytes, USAGE_WRITEONLY, POOL_DEFAULT, false, false);
    if (!vb) return false;
    void* p = vb->lock(0, kStandBytes, 0);
    if (p == nullptr) return false;
    std::memset(p, 0, kStandBytes);
    vb->unlock();
    vro_->stand = std::move(vb);
    return true;
}

// what an op's templates are valid for: the chain of engine objects from the context to the draw, read like composeVegetation reads them
bool StaticOps::vegStampOf(const Op& op, vro::Stamp& st, const std::uint8_t*& shader) {
    st = {};
    shader = nullptr;
    U8* ps = ptr(op.raw.context, 0x18);
    if (ps == nullptr) return false;
    const std::uint16_t type = static_cast<std::uint16_t>(op.raw.desc);
    U8* typeObj = ptr(ptr(ps, 0x38), 8 * static_cast<std::size_t>(type));
    if (typeObj == nullptr) return false;
    U8* mesh = ptr(typeObj, 0x58);
    if (mesh == nullptr) return false;
    U8* visual = ptr(mesh, 0x88);
    U8* sub0 = ptr(mesh, 0x58);
    U8* material = ptr(sub0, 0x38);
    U8* sh = ptr(material, 0x70);
    if (visual == nullptr || sub0 == nullptr || material == nullptr || sh == nullptr) return false;
    const auto* record = hal::vulkan::HalPrimitive::fromApi(ptr(visual, 0x10));
    if (record == nullptr) return false;
    st.ps = ps;
    st.typeObj = typeObj;
    st.mesh = mesh;
    st.visual = visual;
    st.sub0 = sub0;
    st.material = material;
    st.shader = sh;
    st.transformation = get<std::int32_t>(sh, 0x2b8);
    st.meshState = get<std::uint32_t>(mesh, 0x38);
    st.ranges[0] = get<std::uint32_t>(sub0, 0x28);
    st.ranges[1] = get<std::uint32_t>(sub0, 0x2c);
    st.ranges[2] = get<std::uint32_t>(sub0, 0x30);
    st.ranges[3] = get<std::uint32_t>(sub0, 0x34);
    st.baseVertex = static_cast<std::uint32_t>(record->drawDesc().baseVertexIndex);
    {
        Geom g;
        if (!geometryOfVisual(visual, g)) return false;
        st.vb = g.streams[0].buffer;
        st.ib = g.indices;
        st.decl = g.decl;
    }
    st.twoSided = get<std::uint8_t>(typeObj, 0x30);
    st.depthBias = get<std::uint8_t>(sh, 0x78);
    st.variantEpoch = dev_.variantEpoch();
    st.registryGen = enh::fullLitSkipsReads() ? enh::LightTable::registryGeneration() : 0;
    shader = sh;
    return true;
}

// the solid pass's batch lighting as the trees' plan reads it (treePlan): a static-ambient or dynamic-lit illumination, or static lights the light table does not know, are not owned
bool StaticOps::vegIllumSun(const std::uint8_t* illum, bool& sun) {
    sun = false;
    Snap<0x98> is;
    is.load(illum);
    const U8* list = is.ok ? is.at<U8*>(0) : nullptr;
    if (!is.ok || list == nullptr || is.at<std::uint8_t>(0x90) != 0) return false;
    if (get<std::uint8_t>(list, 0x34) != 0) return false;
    const std::uint32_t nl = is.at<std::uint8_t>(0xd);
    if (nl != 0) {
        bool allKnown = nl >= 1 && nl <= 2 && enh::LightTable::registryComplete();
        for (std::uint32_t e = 0; allKnown && e < nl; ++e) {
            float p[3];
            for (std::uint32_t j = 0; j < 3; ++j) p[j] = is.at<float>(0x10 + 0x40 * e + 4 * j);
            allKnown = enh::LightTable::lightKnown(p);
        }
        if (!allKnown) return false;
    }
    sun = get<std::uint8_t>(list, 0) != 0 && is.at<std::uint8_t>(0xc) == 0;
    return true;
}

// the op's converted instance stream: rebuilt when the flattened source entries (or the type's flags) changed; check frames convert again and compare
void StaticOps::vegConvert(vro::OpRec& rec, const std::vector<std::uint8_t>& raw, bool wind, bool lift, bool check, vro::Stats& W) {
    const std::size_t n = raw.size();
    const auto convert = [&](std::vector<std::uint8_t>& out) {
        out.resize(n);
        for (std::size_t k = 0; k * sc::kGrassInstanceSize < n; ++k) sc::grassInstance(raw.data() + k * sc::kGrassInstanceSize, wind, lift, out.data() + k * sc::kGrassInstanceSize);
    };
    bool same = rec.conv.size() == n && rec.raw.size() == n && rec.wind == wind && rec.lift == lift && std::memcmp(rec.raw.data(), raw.data(), n) == 0;
    if (same && check) {
        static thread_local std::vector<std::uint8_t> fresh;
        convert(fresh);
        if (fresh != rec.conv) {
            ++W.checkConvDiff;
            if (vro_->logged < 8) {
                ++vro_->logged;
                spdlog::error("native: owned vegetation draws: DIFFERING: the retained converted instances differ from a fresh conversion ({} bytes)", n);
            }
            same = false;
        }
    }
    if (same) {
        ++W.convReused;
        return;
    }
    convert(rec.conv);
    rec.raw = raw;
    rec.wind = wind;
    rec.lift = lift;
    ++W.convRebuilt;
}

// ---- composition hooks ------------------------------------------------------------------------------------------------------------

void StaticOps::vegCompBegin(const Op& op) {
    vro::State& V = *vro_;
    vro::Pend& P = V.pend;
    P = {};
    P.rec = std::make_unique<vro::OpRec>();
    P.key = {op.raw.context, op.raw.desc, static_cast<std::uint8_t>(op.list), 0};
    P.rec->key = P.key;
    P.zpass = op.list == List::ZPass;
    P.rec->type = static_cast<std::uint16_t>(op.raw.desc);
    P.rec->builtFrame = dev_.stats().frames;
    P.switchBefore = gpuscene::materialtable().stats().switchNotes;
    recCollect_ = true;
    recNoRetain_ = false;
    leanTouches_.clear();
}

void StaticOps::vegCompNote(const Op& op, const std::vector<sc::GrassDraw>& draws, const std::uint8_t* nodes, std::uint32_t total, bool wind, bool lift, float ax, float ay) {
    vro::State& V = *vro_;
    vro::Pend& P = V.pend;
    if (!P.rec) return;
    P.noted = true;
    P.total = total;
    P.ax = ax;
    P.ay = ay;
    P.wind = wind;
    P.lift = lift;
    if (vegRaw_.size() != static_cast<std::size_t>(total) * sc::kGrassInstanceSize || total == 0) {
        P.bad = true;
        P.badWhy = 7;
        return;
    }
    P.raw = vegRaw_;
    P.plan.clear();
    for (const sc::GrassDraw& d : draws) {
        vro::Batch b;
        b.first = d.firstInstance;
        b.count = d.count;
        b.illumNode = d.illumNode;
        if (!P.zpass) {
            if (d.illumNode < 0 || !vegIllumSun(nodes + 0x118 * static_cast<std::size_t>(d.illumNode) + 0x10, b.sun)) {
                P.bad = true;
                P.badWhy = 7;
            }
        }
        b.key = treeBucketKey(P.rec->type + 0x40, P.zpass, b.sun, 0, 0, 0);
        P.plan.push_back(b);
    }
}

void StaticOps::vegCompDraw(std::size_t drawsBefore, std::size_t k) {
    vro::Pend& P = vro_->pend;
    if (!P.rec || !P.noted || P.bad) return;
    if (drawCount_ - drawsBefore != 1 || k >= P.plan.size()) {
        P.bad = true;
        P.badWhy = 0;
        return;
    }
    draws_[drawsBefore]->treeKey = P.plan[k].key;
}

void StaticOps::vegCompEnd(const Op& op) {
    if (!vro_) return;
    vro::State& V = *vro_;
    if (!V.pend.rec) return;
    vro::Pend P = std::move(V.pend);
    V.pend = {};
    if (!P.noted) return;
    vro::Stats& W = V.window;
    const auto t0 = Clock::now();
    const auto erase = [&] { V.ops.erase(P.key); };
    std::unique_ptr<vro::OpRec> pend = std::move(P.rec);
    if (drawCount_ == 0 && !P.bad) {
        erase();
        return;  // (an op that draws nothing: no record)
    }
    if (P.bad || leanBlock_ != 0 || drawCount_ == 0 || recNoRetain_ || gpuscene::materialtable().stats().switchNotes != P.switchBefore) {
        ++W.badOps;
        ++W.badBy[P.bad ? P.badWhy : leanBlock_ != 0 || recNoRetain_ ? 3 : drawCount_ == 0 ? 6 : 4];
        erase();
        return;
    }
    const bool solid = op.list == List::Solid;
    const char* const what = "owned vegetation draws";
    if (drawCount_ != P.plan.size()) {
        ++W.checkCountDiff;
        if (V.logged < 8) {
            ++V.logged;
            spdlog::error("native: {}: DIFFERING: {} the plan has {} batches, the composition {} draws", what, describe(op), P.plan.size(), drawCount_);
        }
        ++W.badOps;
        ++W.badBy[7];
        erase();
        return;
    }
    // ---- the composed draws against the plan and the registers the owned block / view block will give
    float c31[4] = {}, c32[4] = {};
    bool haveC = false;
    for (std::size_t k = 0; k < drawCount_; ++k) {
        const sc::DrawDesc& d = *draws_[k];
        if (d.inheritMask != 0 || !d.vertexBytes.empty() || d.cpuStream != 0 || !d.indexed || d.states[sc::StBlend] != 0 || d.states[sc::StZWrite] == 0 || d.treeKey != P.plan[k].key) {
            ++W.badOps;
            ++W.badBy[d.treeKey != P.plan[k].key ? 7 : 1];
            erase();
            return;
        }
        if (solid && (!d.lightTableLit || d.lightHoldsAll)) {
            ++W.badOps;
            ++W.badBy[7];
            erase();
            return;
        }
        ++W.checkBatches;
        if ((d.streams[0].freq & 0x3fffffffu) != P.plan[k].count || (d.streams[0].freq & 0x40000000u) == 0) {
            ++W.checkPlanDiff;
            if (V.logged < 8) {
                ++V.logged;
                spdlog::error("native: {}: DIFFERING: {} batch {}: plan {} instances, composed freq {:#x}", what, describe(op), k, P.plan[k].count, d.streams[0].freq);
            }
        }
        // registers: window 0 / 1 = the view block (the pass's mirror), c31 / c32 = the op's block, nothing in c33 .. c231
        bool regDiff = false;
        for (std::uint32_t r = 0; r < 31; ++r) {
            if (((d.vsSet[r >> 6] >> (r & 63)) & 1u) == 0) continue;
            const float* want = r < 4 ? static_cast<const float*>(view_.vp[r]) : static_cast<const float*>(mirror_.adminVs[r]);
            if (std::memcmp(d.vsF[r], want, 16) != 0) regDiff = true;
        }
        if ((d.vsSet[0] >> 31 & 1u) != 0) {
            if (!sameBits(d.vsF[31][0], P.ax) || !sameBits(d.vsF[31][1], P.ay)) regDiff = true;
            if (!haveC) {
                std::memcpy(c31, d.vsF[31], 16);
                std::memcpy(c32, d.vsF[32], 16);
                haveC = true;
            }
        }
        if ((d.vsSet[0] >> 32 & 1u) != 0 && !sameBits(d.vsF[32][3], 1.0f)) regDiff = true;
        for (std::uint32_t r = 33; r < 232; ++r) {
            if (((d.vsSet[r >> 6] >> (r & 63)) & 1u) != 0) regDiff = true;
        }
        for (std::uint32_t r = 0; r < 5; ++r) {
            if (((d.psSet[0] >> r) & 1u) != 0 && std::memcmp(d.psF[r], mirror_.adminPs[r], 16) != 0) regDiff = true;
        }
        if (regDiff) {
            ++W.checkRegDiff;
            if (V.logged < 8) {
                ++V.logged;
                spdlog::error("native: {}: DIFFERING: {} batch {}: a composed register differs from the view block / op block (c12 {:g} vs {:g})", what, describe(op), k, d.vsF[12][0], mirror_.adminVs[12][0]);
            }
        }
    }
    if (!haveC) {
        ++W.badOps;
        ++W.badBy[7];
        erase();
        return;
    }
    std::memcpy(pend->c31, c31, 16);
    std::memcpy(pend->c32, c32, 16);
    ++W.checkOps;
    // the retained converted instances (check: a fresh conversion, every composed frame)
    {
        const auto old = V.ops.find(P.key);
        if (old != V.ops.end()) {
            pend->raw = std::move(old->second->raw);
            pend->conv = std::move(old->second->conv);
            pend->wind = old->second->wind;
            pend->lift = old->second->lift;
        }
        vegConvert(*pend, P.raw, P.wind, P.lift, true, W);
    }
    // ---- the templates
    std::vector<const void*> touches = leanTouches_;
    pend->touches.clear();
    const auto addTouch = [&](const void* t) {
        if (t != nullptr && std::find(pend->touches.begin(), pend->touches.end(), t) == pend->touches.end()) pend->touches.push_back(t);
    };
    for (const void* t : touches) addTouch(t);
    const std::uint8_t* shader = nullptr;
    if (!vegStampOf(op, pend->stamp, shader)) {
        ++W.badOps;
        ++W.badBy[7];
        erase();
        return;
    }
    if (pend->stamp.depthBias != 0) {
        ++W.badOps;
        ++W.badBy[2];
        erase();
        return;
    }
    addTouch(pend->stamp.mesh);
    addTouch(pend->stamp.material);
    gpuscene::MaterialTable& mt = gpuscene::materialtable();
    const std::uint32_t matSlot = draws_[0]->matSlot;
    for (std::size_t k = 0; k < drawCount_; ++k) {
        if (draws_[k]->matSlot != matSlot) {
            ++W.badOps;
            ++W.badBy[5];
            erase();
            return;
        }
    }
    if (matSlot >= gpuscene::MaterialTable::kCapacity || !vegEnsureStand()) {
        ++W.badOps;
        ++W.badBy[7];
        erase();
        return;
    }
    pend->matSlot = matSlot;
    pend->matHash = mixh(mixh(0, matSlot), mt.structVersion(matSlot));
    pend->viewStamp = recViewStamp();
    const std::uint64_t viewKey = (static_cast<std::uint64_t>(viewKind_) << 32) ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cascade_)) << 36) ^
                                  (reinterpret_cast<std::uintptr_t>(shadowMap_) * 0xD6E8FEB86659FD93ull);
    const std::size_t cacheGroup = static_cast<std::size_t>(g_ - groups_.data());
    vro::OpRec* old = nullptr;
    if (const auto it = V.ops.find(P.key); it != V.ops.end()) old = it->second.get();
    const bool sameStamp = old != nullptr && old->stamp == pend->stamp;
    bool texMoved = false;
    if (sameStamp) {
        for (const tro::TexMap& x : old->texMap) {
            if (!treeTexUnchanged(x)) {
                texMoved = true;
                break;
            }
        }
    }
    const d3d9::Buffer::Binding sb = V.stand->gpu(sc::kGrassInstanceSize);
    for (std::size_t k = 0; k < drawCount_; ++k) {
        sc::DrawDesc& d = *draws_[k];
        const std::uint64_t pkey = (reinterpret_cast<std::uintptr_t>(op.raw.context) * 0x9E3779B97F4A7C15ull) ^ (static_cast<std::uint64_t>(op.raw.desc) * 0xC2B2AE3D27D4EB4Full) ^
                                   (static_cast<std::uint64_t>(op.list) << 56) ^ (static_cast<std::uint64_t>(k) << 48) ^ (static_cast<std::uint64_t>(cacheGroup) << 40) ^ viewKey;
        const auto pe = packets_.find(pkey);
        if (sb.buffer == 0 || pe == packets_.end() || !pe->second.cd.valid || pe->second.cd.scenePipeline2 == d3d9::GamePipeline::None || pe->second.cd.v.scenePending) {
            ++W.notRetained;  // (the variants / pipelines are still being built, or the packet cache refused the draw)
            continue;
        }
        if (pend->tpls.count(d.treeKey) != 0) continue;  // (a second batch of the same bucket: the same draw)
        if (sameStamp) {
            if (const auto ot = old->tpls.find(d.treeKey); ot != old->tpls.end() && ot->second->desc != nullptr) {
                // (the instance count of stream 0 and the stream-1 buffer / offset move with the batches: the template's own are compared)
                const auto f = d.streams[0].freq;
                const auto s1 = d.streams[1];
                d.streams[0].freq = ot->second->desc->streams[0].freq;
                d.streams[1] = ot->second->desc->streams[1];
                const bool same = packetSameDesc(d, *ot->second->desc) && d.lightN == ot->second->desc->lightN && d.lightFirst == ot->second->desc->lightFirst &&
                                  d.lightTableLit == ot->second->desc->lightTableLit && d.matSlot == ot->second->desc->matSlot;
                d.streams[0].freq = f;
                d.streams[1] = s1;
                // a resource reloaded under the same object pointers (a teleport frees and re-creates a mesh's buffers / material records): the template is replaced, not an error
                const bool movedOnly = !same && !texMoved && d.vs == ot->second->desc->vs && d.ps == ot->second->desc->ps && d.lightN == ot->second->desc->lightN && d.lightFirst == ot->second->desc->lightFirst &&
                                       d.states == ot->second->desc->states && d.samplerMask == ot->second->desc->samplerMask;
                if (movedOnly) ++W.moved;
                else if (!same && !texMoved) ++W.checkTplDiff;
                if (!same && !texMoved && !movedOnly && V.logged < 8) {
                    ++V.logged;
                    const sc::DrawDesc& o = *ot->second->desc;
                    std::string why;
                    const auto chk = [&](bool diff, const char* n) {
                        if (diff) why += std::string(why.empty() ? "" : ", ") + n;
                    };
                    chk(d.vs != o.vs || d.ps != o.ps, "shaders");
                    chk(d.decl != o.decl, "decl");
                    chk(d.indices != o.indices, "indices");
                    chk(d.baseVertex != o.baseVertex || d.minIndex != o.minIndex || d.numVertices != o.numVertices || d.startIndex != o.startIndex || d.primCount != o.primCount, "geometry");
                    chk(d.samplerMask != o.samplerMask || d.inheritMask != o.inheritMask, "sampler mask");
                    chk(d.passFlags != o.passFlags, "pass flags");
                    chk(d.states != o.states, "states");
                    for (int i = 0; i < 4; ++i) chk(!(d.streams[i] == o.streams[i]), "stream");
                    for (std::uint32_t m = d.samplerMask; m != 0; m &= m - 1) {
                        const auto st = sc::lowBit(m);
                        chk(!(d.samplers[st] == o.samplers[st]), "sampler");
                    }
                    chk(d.lightN != o.lightN || d.lightFirst != o.lightFirst || d.lightTableLit != o.lightTableLit, "light params");
                    chk(d.matSlot != o.matSlot, "mat slot");
                    chk(d.tagSubMesh != o.tagSubMesh || d.tagMaterial != o.tagMaterial || d.tagMaterialName != o.tagMaterialName, "tag");
                    spdlog::error("native: {}: DIFFERING: {} bucket {:#x}: the composed draw differs from the retained template ({})", what, describe(op), d.treeKey, why);
                }
                if (same && !texMoved && !ot->second->stale && !ot->second->r.cd->v.scenePending) {
                    pend->tpls[d.treeKey] = std::move(old->tpls[d.treeKey]);
                    continue;
                }
            }
        }
        auto tpl = std::make_unique<tro::Tpl>();
        orec::DrawRec& r = tpl->r;
        r.cd = std::make_unique<d3d9::Device::CachedDraw>(pe->second.cd);
        // stream 1 = the shared stand-in (a static buffer: its binding never changes); the flush swaps in the frame's instance block
        {
            d3d9::Device::CachedDraw& c = *r.cd;
            std::uint32_t found = UINT32_MAX, matches = 0;
            if ((c.streamMask & 2u) != 0 && c.stream[1].buffer != 0) {
                for (std::uint32_t b = 0; b < c.vi.bufferCount && b < rhi::kMaxVertexBindings; ++b) {
                    if (c.vi.buffers[b] == c.stream[1].buffer && c.vi.offsets[b] == c.stream[1].offset + d.streams[1].offset) {  // (vi offset = the binding + the draw's own stream offset: the batch's first instance)
                        found = b;
                        ++matches;
                    }
                }
            }
            if (matches != 1) {
                ++W.notRetained;
                continue;
            }
            c.vi.buffers[found] = sb.buffer;
            c.vi.offsets[found] = sb.offset;
            c.stream[1] = sb;
        }
        for (std::size_t i = 0; i < 4; ++i) {
            r.streams[i] = d.streams[i].buffer;
            r.strides[i] = d.streams[i].stride;
        }
        r.streams[1] = V.stand.get();
        r.strides[1] = sc::kGrassInstanceSize;
        r.indices = d.indices;
        for (std::uint32_t m = static_cast<std::uint32_t>(d.samplerMask & ~d.inheritMask) & 0xffffu; m != 0; m &= m - 1) {
            const std::uint32_t st = static_cast<std::uint32_t>(std::countr_zero(m));
            r.texMask |= static_cast<std::uint16_t>(1u << st);
            r.tex[st] = d.samplers[st].texture;
            for (std::uint32_t i = 0; i < 7; ++i) r.ss[st][i] = d.samplers[st].ss[i];
        }
        r.alphaTestState = d.states[sc::StAlphaTest];
        r.alphaFunc = d.alphaFunc;
        r.alphaRef = d.alphaRef;
        r.maskSet = d.maskSet;
        r.maskValue = d.maskValue;
        r.matSlot = d.matSlot;
        r.illumSlot = d.illumSlot;
        r.lightN = d.lightN;
        r.lightFirst = d.lightFirst;
        r.lightTableLit = d.lightTableLit;
        r.lightHoldsAll = d.lightHoldsAll;
        r.vsSet = d.vsSet;
        r.psSet = d.psSet;
        r.vs1Offered = true;
        r.tagSubMesh = d.tagSubMesh;
        r.tagMaterial = d.tagMaterial;
        r.tagMeshResource = d.tagMeshResource;
        r.tagWater = d.tagWater;
        r.tagMaterialName = d.tagMaterialName;
        r.zPass = d.zPass;
        r.passFlags = d.passFlags;
        r.scene = true;
        tpl->desc = std::make_unique<sc::DrawDesc>(d);
        tpl->key = d.treeKey;
        tpl->builtFrame = dev_.stats().frames;
        pend->tpls[d.treeKey] = std::move(tpl);
        if (old != nullptr && sameStamp && old->tpls.count(d.treeKey) != 0) ++W.replaced;
        else ++W.built;
    }
    if (pend->tpls.empty()) {
        erase();
        return;
    }
    if (sameStamp && !texMoved) {
        for (auto& kv : old->tpls) {
            if (kv.second && pend->tpls.count(kv.first) == 0) pend->tpls[kv.first] = std::move(kv.second);
        }
        for (const tro::TexMap& x : old->texMap) {
            if (std::none_of(pend->texMap.begin(), pend->texMap.end(), [&](const tro::TexMap& y) { return y.res == x.res && y.stage == x.stage; })) pend->texMap.push_back(x);
        }
    }
    pend->lastFrame = dev_.stats().frames;
    V.ops[P.key] = std::move(pend);
    W.recNs += ns(t0, Clock::now());
}

// ---- the frame tap's side ----------------------------------------------------------------------------------------------------------

void StaticOps::vegViewBegin(bool ownedFrame, ViewKind kind) {
    if (!ownedVegOps_ || !vro_) return;
    vro::State& V = *vro_;
    for (auto& v : V.taken) {
        V.window.opsLate += v.size();  // (ops of an earlier view whose pass never began: lost for that frame)
        v.clear();
    }
    V.active = false;
    if (kind != ViewKind::Main) return;
    ++V.window.views;
    if (!ownedFrame) {
        ++V.window.compViews;
        return;
    }
    gpuscene::DrawRecordTable& tab = gpuscene::drawrecords();
    const bool ready = ownedPersistent_ && !ownedTripped_ && ownedStatic_ && tableLit_ && tab.ready() && gpuscene::viewtable().ready() && dev_.sceneViewsReady() && dev_.ownedVegObserversOk() &&
                       lean_ && !V.tripped && vegAdmin_ != nullptr && vegEnsureStand() && packetMode_ != 0;
    if (!ready) {
        std::string obs;
        std::string why = !ownedPersistent_ ? "native.owned_persistent off" : ownedTripped_ ? "owned path tripped" : !ownedStatic_ ? "owned static off" : !tableLit_ ? "table lighting off"
                        : !tab.ready() ? "draw record table not ready" : !(gpuscene::viewtable().ready() && dev_.sceneViewsReady()) ? "view table not ready"
                        : !dev_.ownedVegObserversOk(&obs) ? obs : !lean_ ? "native.lean off" : V.tripped ? "vegetation tripped (DIFFERING)" : vegAdmin_ == nullptr ? "no vegetation admin"
                        : packetMode_ == 0 ? "native.packet_cache off" : "no stream-1 stand-in";
        if (why != V.gate) spdlog::info("native: owned vegetation draws: gated: {}", why);
        V.gate = std::move(why);
        return;
    }
    if (!V.gate.empty()) spdlog::info("native: owned vegetation draws: gate open (was: {})", V.gate);
    V.gate.clear();
    V.active = true;
    ++V.window.ownedViews;
}

bool StaticOps::vegSelect(const Op& op) {
    if (!vro_ || !vro_->active || op.kind != OpKind::Vegetation) return false;
    if (op.list != List::ZPass && op.list != List::Solid) return false;
    vro::State& V = *vro_;
    ++V.window.opsSeen;
    return V.ops.find(tro::OpKey{op.raw.context, op.raw.desc, static_cast<std::uint8_t>(op.list), 0}) != V.ops.end();
}

void StaticOps::vegTake(List list, std::vector<Op>&& ops, std::uint64_t nsTake) {
    if (!vro_) return;
    vro_->window.opsTaken += ops.size();
    vro_->window.takeNs += nsTake;
    vro_->taken[list == List::Solid ? 1 : 0] = std::move(ops);
}

// ---- the pass ----------------------------------------------------------------------------------------------------------------------

std::uint32_t StaticOps::vegPass(bool solid) {
    if (!vro_) return 0;
    vro::State& V = *vro_;
    std::vector<Op>& ops = V.taken[solid ? 1 : 0];
    if (ops.empty()) return 0;
    vro::Stats& W = V.window;
    if (!inView_ || viewKind_ != ViewKind::Main || !V.active) {
        W.opsLate += ops.size();
        ops.clear();
        return 0;
    }
    const auto tPass = Clock::now();
    ret::gLog = nullptr;
    finishOp();
    leanAbort();
    d3d9::draws::Tag& tag = dev_.drawTag();
    const d3d9::draws::Tag savedTag = tag;
    const d3d9::ShaderRef vsSaved = dev_.vertexShaderRef(), psSaved = dev_.pixelShaderRef();
    std::uint32_t drawn = 0;
    U8* vadmin = static_cast<U8*>(vegAdmin_ != nullptr ? vegAdmin_() : nullptr);
    std::uint32_t cursor = get<std::uint32_t>(vadmin, 0x40);
    const auto storeCursor = [&](std::uint32_t v) {
        if (vadmin != nullptr) engine::safeWrite(const_cast<std::uint8_t*>(vadmin) + 0x40, &v, 4);
    };
    const auto composeLate = [&](Op& op, std::uint64_t& counter) {
        ++counter;
        ++W.opsLate;
        dev_.setSceneIssue(false);
        tag = savedTag;
        tag.source = d3d9::draws::Source::Vegetation;
        tag.context = op.raw.context;
        tag.entity = op.entity != nullptr ? op.entity : ptr(op.raw.context, 0x10);
        tag.opDesc = op.raw.desc;
        tag.subMesh = tag.material = tag.meshResource = nullptr;
        tag.water = d3d9::draws::Water::None;
        tag.materialName = nullptr;
        storeCursor(cursor);  // (the composition reads the cursor from the engine: the owned ops before it moved it)
        opEnter(op);
        if (op_ == &op) drawn += static_cast<std::uint32_t>(drawCount_);
        finishOp();
        cursor = get<std::uint32_t>(vadmin, 0x40);
        dev_.setSceneIssue(true);
    };
    // (F-VAR2: the observers of the main view's vegetation, GI / material RT, are shown each owned batch with the lighting entry it was composed with)
    static float savedVs[d3d9::kVsFloatConsts][4], savedPs[d3d9::kPsFloatConsts][4];
    bool regsSaved = false;
    const auto restore = [&] {
        dev_.setSceneIssue(false);
        if (regsSaved) {
            dev_.setVertexShaderConstantF(0, savedVs[0], d3d9::kVsFloatConsts);
            dev_.setPixelShaderConstantF(0, savedPs[0], d3d9::kPsFloatConsts);
            regsSaved = false;
        }
        tag = savedTag;
        dev_.setVertexShaderRef(vsSaved);
        dev_.setPixelShaderRef(psSaved);
        finishOp();
        ops.clear();
    };
    // the view as the first op's composition would make it
    vro::Stamp st0;
    const std::uint8_t* sh0 = nullptr;
    bool viewOk = vegStampOf(ops[0], st0, sh0) && viewAndMirror(sh0);
    if (viewOk) {
        std::memcpy(mirror_.adminVs[0], view_.vp, 64);  // (no depth-bias material is owned)
        grassWind();
    }
    ++opSerial_;
    const std::uint64_t viewStamp = viewOk ? recViewStamp() : 0;
    d3d9::OwnedWindows viewWin;
    bool viewWinOk = false;
    const std::uint32_t viewSlot = solid ? kViewSlotVegSolid : kViewSlotVegZ;
    if (viewOk) {
        gpuscene::ViewTable& vt = gpuscene::viewtable();
        float posFix[4];
        dev_.viewPosFix(posFix);
        gpuscene::ViewTable::Regs regs;
        regs.vs0 = mirror_.adminVs;
        regs.vs1 = mirror_.adminVs + 12;
        regs.ps0 = mirror_.adminPs;
        regs.posFix = posFix;
        regs.c5 = mirror_.adminPs[5];
        viewWinOk = vt.set(viewSlot, regs) && vt.fillWindows(viewSlot, viewWin);
    }
    dev_.setSceneIssue(true);
    bool scopeOk = false;
    std::uint64_t epoch = 0;
    if (viewWinOk) {
        scopeOk = dev_.ownedBeginScope();
        if (scopeOk) epoch = dev_.treeEnvEpoch(solid, viewSlot, scopeOk);
    }
    if (!viewWinOk || !scopeOk) {
        for (Op& op : ops) composeLate(op, W.lateView);
        restore();
        return drawn;
    }
    ownedAmbient_.clear();
    ownedCache_.mat.clear();
    ownedCache_.light.clear();
    ++ownedPassSerial_;
    const std::uint64_t serial = ownedPassSerial_;
    gpuscene::MaterialTable& mt = gpuscene::materialtable();
    gpuscene::LightingImages& li = gpuscene::lightingimages();
    gpuscene::IllumTable& illum = gpuscene::illumtable();
    gpuscene::DrawRecordTable& tab = gpuscene::drawrecords();
    const std::uint64_t pollFrame = dev_.stats().frames + 1;
    const std::uint32_t scrubStride = 64;
    const std::uint32_t scrubPhase = static_cast<std::uint32_t>((++V.scrubTick * 7919u) % scrubStride);
    const std::uint64_t frame = dev_.stats().frames;

    // phase 1: each taken op planned from the engine's grid nodes, its templates made ready
    struct OpPlan {
        std::size_t opIndex = 0;
        vro::OpRec* rec = nullptr;
        std::vector<vro::Batch> batches;
        std::vector<std::uint32_t> entries;   // per batch: the lighting entry (solid)
        std::vector<tro::Tpl*> tpls;          // per batch
        float ax = 0, ay = 0;
        sc::GrassThin thin;  // S-5 (native.own_foliage): as the composition of the op would thin it
        std::uint32_t cursorAfter = 0;
    };
    static thread_local std::vector<OpPlan> plans;
    plans.clear();
    std::vector<std::uint32_t> drawnOps;
    std::unordered_map<std::uint64_t, std::uint32_t> entryMemo;
    std::vector<std::uint8_t>& flat = V.flat;
    for (std::size_t oi = 0; oi < ops.size(); ++oi) {
        Op& op = ops[oi];
        const auto it = V.ops.find(tro::OpKey{op.raw.context, op.raw.desc, static_cast<std::uint8_t>(op.list), 0});
        if (it == V.ops.end()) {
            composeLate(op, W.lateNoRec);
            continue;
        }
        vro::OpRec& rec = *it->second;
        vro::Stamp st;
        const std::uint8_t* shader = nullptr;
        if (!vegStampOf(op, st, shader) || !(st == rec.stamp) || !shaderClassOk(shader)) {
            composeLate(op, W.lateStamp);
            continue;
        }
        if (rec.viewStamp != viewStamp) {
            composeLate(op, W.lateView);
            continue;
        }
        {
            std::uint64_t h = 0;
            bool ok = rec.matSlot < gpuscene::MaterialTable::kCapacity;
            if (ok) {
                if (rec.matSlot >= ownedPolled_.size()) ownedPolled_.resize(gpuscene::MaterialTable::kCapacity, 0);
                if (ownedPolled_[rec.matSlot] != pollFrame) {
                    ok = mt.pollOwned(rec.matSlot, &StaticOps::materialTexture, this);
                    if (ok) ownedPolled_[rec.matSlot] = pollFrame;
                }
                h = mixh(mixh(0, rec.matSlot), mt.structVersion(rec.matSlot));
            }
            if (!ok || h != rec.matHash) {
                composeLate(op, W.lateMaterial);
                continue;
            }
        }
        {
            bool same = true;
            for (const tro::TexMap& x : rec.texMap) {
                if (!treeTexUnchanged(x)) {
                    same = false;
                    break;
                }
            }
            if (!same) {
                composeLate(op, W.lateMaterial);
                continue;
            }
        }
        // ---- the nodes (composeVegetation's walk)
        const auto tN0 = Clock::now();
        U8* ps = st.ps != nullptr ? static_cast<U8*>(const_cast<void*>(st.ps)) : nullptr;
        const std::int32_t nodeCount = get<std::int32_t>(ps, 0xa0);
        const std::int64_t type = static_cast<std::int32_t>(op.raw.desc);
        U8* nodes = ptr(ps, 0x98);
        U8* vd = ptr(vadmin, 0x38);
        if (nodeCount < 1) {
            drawnOps.push_back(static_cast<std::uint32_t>(oi));
            continue;  // (no visible node: the slot draws nothing)
        }
        if (nodeCount > 100000 || nodes == nullptr || vd == nullptr || cursor > sc::kGrassBufferInstances) {
            composeLate(op, W.latePlan);
            continue;
        }
        std::vector<sc::GrassNode>& plan = scr_.grassPlan;
        std::vector<U8*>& sources = scr_.grassSources;
        plan.assign(static_cast<std::size_t>(nodeCount), sc::GrassNode{});
        sources.assign(static_cast<std::size_t>(nodeCount), nullptr);
        std::uint32_t total = 0;
        bool bad = false;
        for (std::int32_t i = 0; i < nodeCount && !bad; ++i) {
            U8* e = nodes + 0x118 * static_cast<std::size_t>(i);
            sc::GrassNode& gn = plan[static_cast<std::size_t>(i)];
            gn.lightSet = ptr(e, 0);
            gn.alone = get<std::uint8_t>(e, 0xa0) != 0 || get<std::uint8_t>(e, 0x1d) != 0;
            U8* grid = ptr(e, 8);
            U8* range = ptr(grid, 0x30);
            if (grid == nullptr || range == nullptr) {
                bad = true;
                break;
            }
            range += 8 * static_cast<std::size_t>(type);
            gn.count = get<std::uint32_t>(range, 4);
            if (gn.count != 0) {
                sources[static_cast<std::size_t>(i)] = ptr(grid, 0x18) + sc::kGrassInstanceSize * static_cast<std::size_t>(get<std::int32_t>(range, 0));
                total += gn.count;
            }
        }
        W.nodeNs += ns(tN0, Clock::now());
        if (bad || total > 200000) {
            composeLate(op, W.latePlan);
            continue;
        }
        const auto tR0 = Clock::now();
        flat.resize(static_cast<std::size_t>(total) * sc::kGrassInstanceSize);
        {
            std::size_t at = 0;
            for (std::int32_t i = 0; i < nodeCount && !bad; ++i) {
                const sc::GrassNode& gn = plan[static_cast<std::size_t>(i)];
                if (gn.count == 0) continue;
                const std::size_t n = static_cast<std::size_t>(gn.count) * sc::kGrassInstanceSize;
                if (!safeRead(sources[static_cast<std::size_t>(i)], flat.data() + at, n)) bad = true;
                at += n;
            }
        }
        W.readNs += ns(tR0, Clock::now());
        if (bad) {
            composeLate(op, W.lateRead);
            continue;
        }
        const auto tP0 = Clock::now();
        std::vector<sc::GrassDraw>& draws = scr_.grassDraws;
        const std::uint32_t cursorAfter = sc::planGrass(plan, cursor, !solid, draws);
        OpPlan pl;
        pl.opIndex = oi;
        pl.rec = &rec;
        pl.cursorAfter = cursorAfter;
        std::uint64_t* lateWhy = nullptr;
        for (const sc::GrassDraw& d : draws) {
            vro::Batch b;
            b.first = d.firstInstance;
            b.count = d.count;
            b.illumNode = d.illumNode;
            std::uint32_t entry = gpuscene::kNoEntry;
            const U8* illumPtr = d.illumNode >= 0 ? nodes + 0x118 * static_cast<std::size_t>(d.illumNode) + 0x10 : nullptr;
            if (solid && (illumPtr == nullptr || !vegIllumSun(illumPtr, b.sun))) {
                lateWhy = &W.lateSpecial;
                break;
            }
            b.key = treeBucketKey(rec.type + 0x40, !solid, b.sun, 0, 0, 0);
            const auto f = rec.tpls.find(b.key);
            if (f == rec.tpls.end()) {
                lateWhy = &W.lateNoTemplate;
                break;
            }
            tro::Tpl* tpl = f->second.get();
            if (tpl->passSerial != serial) {
                tpl->passSerial = serial;
                tpl->passRun = 0;
                const int rc = treeTemplate(*tpl, viewWin, epoch, solid, scrubStride, scrubPhase, V.tw, V.scrubTick, "owned vegetation draws");
                if (rc != 0) {
                    tpl->passSerial = 0;
                    lateWhy = &W.lateTemplate;
                    ++W.rcBy[std::min(rc, 3)];
                    if (V.logged < 12) {
                        ++V.logged;
                        const d3d9::Device::CachedDraw& c = *tpl->r.cd;
                        spdlog::info("native: owned vegetation draws: template refused (rc {}): valid {}, vs module {}, ps module {}, scene pipeline {}, stale {}", rc, c.valid, c.vs != nullptr && c.vs->module == c.vsModule,
                                     c.ps != nullptr && c.ps->module == c.fsModule, static_cast<int>(c.scenePipeline2), tpl->stale);
                    }
                    break;
                }
                tpl->devBucket = dev_.ownedBucketKey(*tpl->r.cd, viewSlot);
            }
            if (solid) {
                const gpuscene::ComposeParams p = paramsOf(tpl->r);
                if (!p.tableLit || p.holdsAll) {
                    lateWhy = &W.lateSpecial;
                    break;
                }
                // (the illumination object is what the lighting entry derives from)
                const std::uint64_t lk = mixh(reinterpret_cast<std::uintptr_t>(illumPtr), (static_cast<std::uint64_t>(p.n) << 8) | p.firstLight);
                const auto m = entryMemo.find(lk);
                if (m != entryMemo.end()) {
                    entry = m->second;
                } else {
                    const std::uint32_t slot = illum.ensureSlot(illumPtr);
                    entry = slot == gpuscene::kNoSlot ? gpuscene::kNoEntry : li.entryFor(slot, p);
                    entryMemo[lk] = entry;
                }
                if (entry == gpuscene::kNoEntry || !li.ready()) {
                    lateWhy = &W.lateSpecial;
                    break;
                }
            }
            pl.batches.push_back(b);
            pl.entries.push_back(entry);
            pl.tpls.push_back(tpl);
        }
        W.planNs += ns(tP0, Clock::now());
        if (lateWhy != nullptr) {
            composeLate(op, *lateWhy);
            continue;
        }
        // the retained converted instances of the op (rebuilt when the source moved)
        {
            const bool wind = get<float>(st.typeObj, 0x2c) != 0.0f;
            const bool lift = get<std::int32_t>(st.typeObj, 0x20) != 0;
            vegConvert(rec, flat, wind, lift, false, W);
        }
        // c31.xy = the fade of the grid (SetVegationAlpha)
        {
            U8* grid = ptr(ps, 0x90);
            sc::vegetationAlpha(get<float>(grid, 0x50), get<float>(grid, 0x54), maskRefOf(shader), pl.ax, pl.ay);
            pl.thin = grassThinOf(get<float>(grid, 0x54));
        }
        cursor = cursorAfter;
        if (pl.batches.empty()) {
            drawnOps.push_back(static_cast<std::uint32_t>(oi));  // (nothing to draw)
            continue;
        }
        drawnOps.push_back(static_cast<std::uint32_t>(oi));
        plans.push_back(std::move(pl));
    }
    // phase 2: the op blocks (c31 / c32), the records, the instance block, the flush
    bool flushed = true;
    std::uint32_t items = 0, positions = 0;
    if (!plans.empty()) {
        const auto tF0 = Clock::now();
        flushed = false;
        // runs: the batches of each template (in op order), templates ordered by their device bucket
        struct Run {
            tro::Tpl* tpl;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> bat;  // (plan index, batch index)
        };
        static thread_local std::vector<Run> runs;
        runs.clear();
        for (std::uint32_t p = 0; p < plans.size(); ++p) {
            for (std::uint32_t b = 0; b < plans[p].batches.size(); ++b) {
                tro::Tpl* tpl = plans[p].tpls[b];
                if (tpl->passRun == 0) {
                    tpl->passRun = static_cast<std::uint32_t>(runs.size()) + 1;
                    runs.push_back({tpl, {}});
                }
                runs[tpl->passRun - 1].bat.push_back({p, b});
            }
        }
        std::vector<std::uint32_t> order(runs.size());
        for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return runs[a].tpl->devBucket < runs[b].tpl->devBucket; });
        const std::uint32_t nOps = static_cast<std::uint32_t>(plans.size());
        rhi::RingAlloc wins = dev_.allocOwnedRing((static_cast<std::uint64_t>(kPadRegs) + static_cast<std::uint64_t>(kBlockRegs) * nOps) * 16, 16);
        bool ok = wins.ptr != nullptr;
        std::uint32_t P = 0;
        for (const OpPlan& pl : plans)
            for (const vro::Batch& b : pl.batches) P += b.count;
        if (ok && P != 0 && static_cast<std::uint64_t>(P) * sc::kGrassInstanceSize <= (8u << 20)) {
            auto* wbase = static_cast<std::uint8_t*>(wins.ptr);
            std::memset(wbase, 0, static_cast<std::size_t>(kPadRegs) * 16);
            for (std::uint32_t p = 0; p < nOps; ++p) {
                float* c = reinterpret_cast<float*>(wbase + (static_cast<std::size_t>(kPadRegs) + kBlockRegs * p) * 16);
                std::memcpy(c, plans[p].rec->c31, 16);
                std::memcpy(c + 4, plans[p].rec->c32, 16);
                c[0] = plans[p].ax;
                c[1] = plans[p].ay;
                c[7] = 1.0f;
            }
            V.inst.resize(static_cast<std::size_t>(P) * sc::kGrassInstanceSize);
            V.items.clear();
            V.recs.clear();
            V.posRec.clear();
            V.posRec.reserve(P);
            std::uint32_t pos = 0;
            struct RecKey {
                std::uint32_t plan, entry, rec;
            };
            std::vector<RecKey> recKeys;
            for (const std::uint32_t ri : order) {
                Run& run = runs[ri];
                tro::Tpl& tpl = *run.tpl;
                const std::uint32_t slot = tpl.r.persist.handle.slot;
                if (!tab.live(slot)) {
                    ok = false;
                    break;
                }
                const shader::abi::DrawRecord base0 = tab.record(slot);
                d3d9::Device::TreeItem item;
                item.cd = tpl.r.cd.get();
                item.bucket = tpl.devBucket;
                recKeys.clear();
                const bool observed = solid && tpl.r.persist.valid && dev_.ownedTreeObserved(*tpl.r.cd);
                for (const auto& [p, b] : run.bat) {
                    const OpPlan& pl = plans[p];
                    const vro::Batch& bt = pl.batches[b];
                    const std::uint32_t entry = pl.entries[b];
                    if (observed) {
                        if (!regsSaved) {
                            std::memcpy(savedVs, dev_.vsConstants(), sizeof(savedVs));
                            std::memcpy(savedPs, dev_.psConstants(), sizeof(savedPs));
                            regsSaved = true;
                        }
                        const Op& vop = ops[pl.opIndex];
                        tag.source = d3d9::draws::Source::Vegetation;
                        tag.context = vop.raw.context;
                        tag.entity = vop.entity != nullptr ? vop.entity : ptr(vop.raw.context, 0x10);
                        tag.opDesc = vop.raw.desc;
                        tag.subMesh = tpl.r.tagSubMesh;
                        tag.material = tpl.r.tagMaterial;
                        tag.meshResource = tpl.r.tagMeshResource;
                        tag.water = static_cast<d3d9::draws::Water>(tpl.r.tagWater);
                        tag.materialName = tpl.r.tagMaterialName;
                        d3d9::OwnedWindows vow = tpl.r.persist.ow;
                        d3d9::OwnedWindows::Win& lw = vow.w[1][3];
                        lw.kind = d3d9::OwnedWindows::Kind::Lighting;
                        lw.image = nullptr;
                        lw.lightEntry = entry;
                        W.observed += dev_.ownedVegObserve(*tpl.r.cd, vow, bt.count);
                    }
                    std::uint32_t rec = ~0u;
                    for (const RecKey& k : recKeys) {
                        if (k.plan == p && k.entry == entry) {
                            rec = k.rec;
                            break;
                        }
                    }
                    if (rec == ~0u) {
                        shader::abi::DrawRecord r = base0;
                        // window 2: the op's block (c31 at the block's first register: base = block - 31 x 16)
                        r.vsWin[2] = wins.ref.plus(static_cast<std::int64_t>((kPadRegs + kBlockRegs * p) * 16) - static_cast<std::int64_t>(shader::abi::kVsWindowFirst[2]) * 16).bits;
                        if (solid) r.psWin[3] = li.windowBase(entry).bits;
                        rec = static_cast<std::uint32_t>(V.recs.size());
                        V.recs.push_back(r);
                        recKeys.push_back({p, entry, rec});
                    }
                    const std::size_t srcAt = static_cast<std::size_t>(bt.first) * sc::kGrassInstanceSize;
                    const std::size_t n = static_cast<std::size_t>(bt.count) * sc::kGrassInstanceSize;
                    if (srcAt + n > pl.rec->conv.size()) {
                        ok = false;
                        break;
                    }
                    std::memcpy(V.inst.data() + static_cast<std::size_t>(pos) * sc::kGrassInstanceSize, pl.rec->conv.data() + srcAt, n);
                    if (pl.thin.on) {
                        std::uint32_t removed = 0;
                        const std::uint32_t changed = sc::grassThin(V.inst.data() + static_cast<std::size_t>(pos) * sc::kGrassInstanceSize, bt.count, pl.thin, &removed);
                        foliageCount(pl.thin, bt.count, changed, removed);
                    }
                    for (std::uint32_t q = 0; q < bt.count; ++q) V.posRec.push_back(rec);
                    pos += bt.count;
                    item.instances += bt.count;
                }
                if (!ok) break;
                V.items.push_back(item);
                run.bat.clear();
                tpl.passRun = 0;
            }
            if (ok && pos == P) {
                items = static_cast<std::uint32_t>(V.items.size());
                const d3d9::Device::VegInstances vi{V.inst.data(), sc::kGrassInstanceSize};
                flushed = dev_.ownedTreeFlush(V.items, V.recs, V.posRec, viewSlot, P, &vi);
                W.records += V.recs.size();
                positions = P;
            }
        }
        for (Run& run : runs) {
            run.tpl->passRun = 0;
            run.bat.clear();
        }
        W.flushNs += ns(tF0, Clock::now());
        if (!flushed) {
            // nothing was queued: the ops that contributed are composed instead (the cursor back to where the first of them found it)
            std::vector<std::uint32_t> lateOps;
            for (const OpPlan& pl : plans) lateOps.push_back(static_cast<std::uint32_t>(pl.opIndex));
            for (const std::uint32_t oi : lateOps) std::erase(drawnOps, oi);
            for (const std::uint32_t oi : lateOps) composeLate(ops[oi], W.lateFlush);
        }
    }
    if (flushed) {
        // the engine slots' side effects: the resources touched, the cursor where the last op left it
        const auto tS0 = Clock::now();
        for (const OpPlan& pl : plans) {
            for (const void* r : pl.rec->touches) touchNow(r);
            pl.rec->lastFrame = frame;
        }
        storeCursor(cursor);
        W.sideNs += ns(tS0, Clock::now());
        for (const OpPlan& pl : plans) {
            W.batches += pl.batches.size();
        }
        W.instances += positions;
        W.items += items;
        if (items != 0) ++W.flushes;
        drawn += items;
    }
    W.opsDrawn += drawnOps.size();
    restore();
    W.passNs += ns(tPass, Clock::now());
    return drawn;
}

// ---- report ----------------------------------------------------------------------------------------------------------------------

void StaticOps::vegReport() {
    if (!ownedVegOps_ || !vro_) return;
    vro::State& V = *vro_;
    {
        // records of ops that left (a world change / chunks behind the player) give their record slots back
        const std::uint64_t frame = dev_.stats().frames;
        for (auto it = V.ops.begin(); it != V.ops.end();) it = frame - it->second->lastFrame > 1200 ? V.ops.erase(it) : std::next(it);
    }
    vro::Stats& q = V.window;
    V.total.add(q);
    vro::Stats& t = V.total;
    tro::Stats& d = V.tw;
    V.tt.add(d);
    const double ms = 1e-6;
    spdlog::info("native: owned vegetation draws (5 s): views {} ({} owned, {} composed); ops: {} seen, {} taken, {} drawn, {} late (no record {}, stamp {}, view {}, material {}, plan {}, read {}, special {}, no template {}, template {} [rc 1 {} 2 {} 3 {}], flush {}); "
                 "{} batches / {} instances in {} flushes ({} commands, {} records, {} batches shown to observers); records built {} replaced {} not retained {} bad {} (by reason: {} {} {} {} {} {} {} {}); "
                 "templates derived {} reused {} refused {}; converted instances reused {} rebuilt {}; resources moved {}; check frames: {} ops {} batches, DIFFERING plan {} template {} registers {} converted {} count {}, MISS {} (of {}); "
                 "CPU ms: pass {:.2f} (nodes {:.2f} read {:.2f} plan {:.2f} flush {:.2f} side {:.2f}) record {:.2f}; total ops {} drawn {} late {}, DIFFERING {}",
                 q.views, q.ownedViews, q.compViews, q.opsSeen, q.opsTaken, q.opsDrawn, q.opsLate, q.lateNoRec, q.lateStamp, q.lateView, q.lateMaterial, q.latePlan, q.lateRead, q.lateSpecial, q.lateNoTemplate, q.lateTemplate, q.rcBy[1], q.rcBy[2], q.rcBy[3],
                 q.lateFlush, q.batches, q.instances, q.flushes, q.items, q.records, q.observed, q.built, q.replaced, q.notRetained, q.badOps, q.badBy[0], q.badBy[1], q.badBy[2], q.badBy[3], q.badBy[4], q.badBy[5], q.badBy[6], q.badBy[7],
                 d.derived, d.reused, d.refused, q.convReused, q.convRebuilt, q.moved, q.checkOps, q.checkBatches, q.checkPlanDiff, q.checkTplDiff, q.checkRegDiff, q.checkConvDiff, q.checkCountDiff, d.checkMiss, d.checkMissCompared,
                 static_cast<double>(q.passNs) * ms, static_cast<double>(q.nodeNs) * ms, static_cast<double>(q.readNs) * ms, static_cast<double>(q.planNs) * ms, static_cast<double>(q.flushNs) * ms, static_cast<double>(q.sideNs) * ms,
                 static_cast<double>(q.recNs) * ms, t.opsSeen, t.opsDrawn, t.opsLate,
                 t.checkPlanDiff + t.checkTplDiff + t.checkRegDiff + t.checkConvDiff + t.checkCountDiff);
    const std::uint64_t differing = q.checkPlanDiff + q.checkTplDiff + q.checkRegDiff + q.checkConvDiff + q.checkCountDiff;
    if ((differing != 0 || d.checkMiss != 0) && !V.tripped) {
        V.tripped = true;
        V.active = false;
        for (auto& v : V.taken) v.clear();
        spdlog::error("native: owned vegetation draws turned off: {} DIFFERING / {} MISS on the check frames", differing, d.checkMiss);
    }
    V.window = {};
    V.tw = {};
}

}  // namespace rvk::native
