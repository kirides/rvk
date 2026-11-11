// F4-2 / F4-3 owned SpeedTrees (docs/research/frame-ownership.md 7.25, setting native.owned_trees): see tree_owned.h.
//
// treeCompBegin / treeCompBatch / treeCompInstance / treeCompEnd   hooks of composeTree + opEnterImpl: the composition's draws are kept as templates per bucket; on the
//                  composed frames the plan (treePlan) made at the op's entry is compared with the composition (rows, buckets, instance order) and the new draws with the
//                  retained templates (F4-2: DIFFERING must stay 0)
// treeViewBegin / treeSelect / treeTake   the main view begins (frame tap): the op records whose stamps are valid leave the engine's z / solid lists
// treePass         BeginZPass / DoBeginRenderSolid returned: the taken ops are planned from the engine's tree objects, their templates made ready (a persistent DrawBlock each),
//                  one row per instance and one DrawRecord per run written, the pass's trees drawn by a few indirect calls (F4-3); an op that cannot be drawn this way is composed
//                  right there (the engine's slot never runs for a taken op)

#include <windows.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "d3d9/defs.h"
#include "enh/light_table.h"
#include "engine/memread.h"
#include "gpuscene/illumination_table.h"
#include "gpuscene/lighting_images.h"
#include "gpuscene/material_table.h"
#include "gpuscene/view_table.h"
#include "hal/vulkan/hal_vulkan.h"
#include "native/owned_timers.h"
#include "native/static_ops.h"
#include "native/tree_rows.h"

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
inline std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}
inline std::uint64_t ns(Clock::time_point a, Clock::time_point b) { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()); }

template <std::size_t N>
struct ObjSnap {
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

inline void column(const float m[16], std::uint32_t k, float out[4]) {
    out[0] = m[k];
    out[1] = m[4 + k];
    out[2] = m[8 + k];
    out[3] = m[12 + k];
}

constexpr std::uint32_t kRampLen = 16384;           // instance positions per pass (the identity ramp's length)
constexpr std::uint32_t kViewSlotTreeZ = 14, kViewSlotTreeSolid = 15;
constexpr std::uint32_t kViewSlotTreeCascade0 = 8;  // F4-4: slots 8..11 (the view table's dynamic slots are not used by anything; every v2 slot must stay < 16)
constexpr std::uint32_t kRowRegs = 4;               // c32 .. c35 per instance

inline gpuscene::ComposeParams paramsOfTpl(const orec::DrawRec& r) {
    gpuscene::ComposeParams p;
    p.tableLit = r.lightTableLit;
    p.holdsAll = r.lightHoldsAll;
    p.n = r.lightN;
    p.firstLight = r.lightFirst;
    return p;
}

}  // namespace

void tro::Stats::add(const Stats& o) {
    views += o.views; ownedViews += o.ownedViews; opsSeen += o.opsSeen; opsTaken += o.opsTaken; opsDrawn += o.opsDrawn; opsLate += o.opsLate; instances += o.instances;
    items += o.items; observed += o.observed; flushes += o.flushes; records += o.records; compBatches += o.compBatches; compViews += o.compViews; packets += o.packets;
    lateNoRec += o.lateNoRec; lateStamp += o.lateStamp; lateView += o.lateView; lateMaterial += o.lateMaterial; latePlan += o.latePlan; lateSpecial += o.lateSpecial;
    lateNoTemplate += o.lateNoTemplate; lateTemplate += o.lateTemplate; lateFlush += o.lateFlush; lateRows += o.lateRows;
    for (int i = 0; i < 8; ++i) badBy[i] += o.badBy[i];
    built += o.built; replaced += o.replaced; notRetained += o.notRetained; badOps += o.badOps;
    derived += o.derived; reused += o.reused; refused += o.refused; tableFull += o.tableFull;
    checkOps += o.checkOps; checkInst += o.checkInst; checkRowDiff += o.checkRowDiff; checkKeyDiff += o.checkKeyDiff; checkCountDiff += o.checkCountDiff; checkDraws += o.checkDraws;
    checkTplDiff += o.checkTplDiff; checkMiss += o.checkMiss; checkMissCompared += o.checkMissCompared;
    emptyOps += o.emptyOps;
    planNs += o.planNs; tplNs += o.tplNs; rowNs += o.rowNs; flushNs += o.flushNs; sideNs += o.sideNs; takeNs += o.takeNs;
}

std::uint64_t StaticOps::treeBucketKey(std::uint32_t type, bool zpass, bool sun, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    std::uint64_t h = mix(0x7265655f6f776eull, type | (zpass ? 0x100u : 0u) | (sun ? 0x200u : 0u));
    h = mix(mix(mix(h, a), b), c);
    return h | 1u;
}

bool StaticOps::treeEnsureRamp() {
    if (!tro_) tro_ = std::make_unique<tro::State>();
    if (tro_->ramp) return true;
    using namespace d3d9;
    auto vb = dev_.createBuffer(kRampLen * 4, USAGE_WRITEONLY, POOL_DEFAULT, false, false);
    if (!vb) return false;
    void* p = vb->lock(0, kRampLen * 4, 0);
    if (p == nullptr) return false;
    auto* f = static_cast<float*>(p);
    for (std::uint32_t i = 0; i < kRampLen; ++i) f[i] = tro::rampValue(i);  // (the LOCAL instance index: rows are laid out in chunks of 50, tree_rows.h)
    vb->unlock();
    tro_->ramp = std::move(vb);
    tro_->rampLen = kRampLen;
    return true;
}

void StaticOps::setOwnedTrees(bool on) {
    if (on == ownedTrees_) return;
    ownedTrees_ = on;
    if (on) {
        treeEnsureRamp();
    } else if (tro_) {
        tro_->active = false;
        for (auto& v : tro_->taken) v.clear();
        tro_->pending.reset();
        tro_->cur = nullptr;
        tro_->ops.clear();
    }
    spdlog::info("native: owned trees {}", on ? "on (main view SpeedTree ops: templates per bucket, check frames compare, owned frames draw from the engine's tree objects)" : "off");
}

// ---- composition hooks ------------------------------------------------------------------------------------------------------------

void StaticOps::treeCompBegin(const Op& op, bool zpass, std::uint32_t type, const std::uint8_t* rb, const std::uint8_t* material, const std::uint8_t* shader, bool hardware, bool tableMerge,
                              const std::uint8_t* queue, std::int32_t n, const std::uint8_t* wrapper) {
    tro::State& T = *tro_;
    T.cur = nullptr;
    T.bad = false;
    T.pending = std::make_unique<tro::OpRec>();
    tro::OpRec& r = *T.pending;
    r.key = {op.raw.context, op.raw.desc, static_cast<std::uint8_t>(op.list), treeViewKey()};
    r.type = type;
    U8* ps = ptr(op.raw.context, 0x18);
    r.stamp.res = ptr(ps, 0x38);
    r.stamp.rb = rb;
    r.stamp.material = material;
    r.stamp.shader = shader;
    r.stamp.visual = ptr(rb, 0x18 + 8 * static_cast<std::size_t>(type));
    r.stamp.transformation = get<std::int32_t>(shader, 0x2b8);
    r.stamp.variantEpoch = dev_.variantEpoch();
    r.stamp.registryGen = enh::fullLitSkipsReads() ? enh::LightTable::registryGeneration() : 0;
    r.stamp.hardware = hardware;
    r.stamp.tableMerge = tableMerge;
    r.builtFrame = dev_.stats().frames;
    T.cur = &r;
    T.checkOp = true;
    T.composed.clear();
    T.plan.clear();
    leanTouches_.clear();
    recCollect_ = true;
    T.switchBefore = gpuscene::materialtable().stats().switchNotes;
    T.badWhy = 7;
    if (get<std::uint8_t>(shader, 0x78) != 0) {
        T.bad = true;
        T.badWhy = 2;
    }  // (a depth-bias material draws with the other view-projection)
    const auto t0 = tnow();
    int why = 0;
    if (!treePlan(zpass, type, tableMerge, rb, wrapper, queue, n, T.plan, why)) T.plan.clear(), T.checkOp = false;
    T.win().planNs += ns(t0, tnow());
}

void StaticOps::treeCompBatch(std::size_t drawsBefore, std::uint64_t key) {
    tro::State& T = *tro_;
    if (T.cur == nullptr) return;
    const std::size_t n = drawCount_ - drawsBefore;
    if (n != 1 || key == 0) {
        T.bad = true;
        T.badWhy = 0;
        return;
    }
    sc::DrawDesc& d = *draws_[drawsBefore];
    d.treeKey = key;
    if (!T.active) ++T.win().compBatches;
    const bool solid = op_ != nullptr ? op_->list == List::Solid : false;
    (void)solid;
    if (d.inheritMask != 0 || !d.vertexBytes.empty() || d.cpuStream != 0 || !d.indexed || d.states[sc::StBlend] != 0 || d.states[sc::StZWrite] == 0) {
        T.bad = true;
        T.badWhy = 1;
    }
}

void StaticOps::treeCompInstance(const std::uint8_t* tree, const std::uint8_t* illum, std::uint64_t key, bool sun, std::uint32_t slot) {
    tro::State& T = *tro_;
    if (T.cur == nullptr || !T.checkOp) return;
    tro::Inst x;
    x.tree = tree;
    x.illum = illum;
    x.key = key;
    x.sun = sun;
    for (std::uint32_t j = 0; j < kRowRegs; ++j) std::memcpy(x.row + 4 * j, mirror_.global[1 + kRowRegs * slot + j], 16);
    T.composed.push_back(x);
}

void StaticOps::treeCompEnd(const Op& op) {
    if (!tro_ || !tro_->pending || tro_->cur == nullptr) return;
    tro::State& T = *tro_;
    tro::Stats& W = T.win();
    std::unique_ptr<tro::OpRec> pend = std::move(T.pending);
    T.cur = nullptr;
    const auto t0 = tnow();
    const bool solid = op.list == List::Solid;
    const auto erase = [&] { T.ops.erase(pend->key); };
    if (drawCount_ == 0 && !T.bad) {
        erase();
        return;  // (an op that draws nothing: no record)
    }
    if (T.bad || leanBlock_ != 0 || drawCount_ == 0 || recNoRetain_ || gpuscene::materialtable().stats().switchNotes != T.switchBefore) {
        ++W.badOps;
        ++W.badBy[T.bad ? T.badWhy : leanBlock_ != 0 || recNoRetain_ ? 3 : drawCount_ == 0 ? 6 : 4];
        erase();
        return;
    }
    // ---- check: the plan made at the op's entry against the composed instances
    if (T.checkOp && !T.plan.empty()) {
        ++W.checkOps;
        if (T.plan.size() != T.composed.size()) {
            ++W.checkCountDiff;
            if (T.logged < 8) {
                ++T.logged;
                spdlog::error("native: owned trees: DIFFERING: {} the plan has {} instances, the composition {}", describe(op), T.plan.size(), T.composed.size());
            }
        } else {
            for (std::size_t i = 0; i < T.plan.size(); ++i) {
                const tro::Inst& a = T.plan[i];
                const tro::Inst& b = T.composed[i];
                ++W.checkInst;
                if (a.tree != b.tree || a.key != b.key) {
                    ++W.checkKeyDiff;
                    if (T.logged < 8) {
                        ++T.logged;
                        // the same tree in another bucket = the accepted Full picture change (per-instance LOD start, not counted as DIFFERING): debug
                        // level, the 5 s line counts it; another tree object is a real error
                        spdlog::log(a.tree != b.tree ? spdlog::level::err : spdlog::level::debug, "native: owned trees: {}: {} instance {}: tree {} / {}, bucket {:#x} / {:#x}",
                                    a.tree != b.tree ? "DIFFERING" : "per-instance LOD start", describe(op), i, static_cast<const void*>(a.tree), static_cast<const void*>(b.tree), a.key, b.key);
                    }
                    continue;
                }
                // (c32.z is never written; branches / fronds: c32.w is the engine's global alpha of instance 0 only)
                bool diff = a.row[0] != b.row[0] || a.row[1] != b.row[1] || (pend->type >= 2 && a.row[3] != b.row[3]);
                for (std::size_t q = 4; q < 16 && !diff; ++q) diff = a.row[q] != b.row[q];
                if (diff) {
                    ++W.checkRowDiff;
                    if (T.logged < 8) {
                        ++T.logged;
                        spdlog::error("native: owned trees: DIFFERING: {} instance {} row: plan ({:g} {:g} {:g} {:g} | {:g} {:g} {:g} {:g}) composed ({:g} {:g} {:g} {:g} | {:g} {:g} {:g} {:g})", describe(op), i, a.row[0], a.row[1],
                                      a.row[2], a.row[3], a.row[4], a.row[5], a.row[6], a.row[7], b.row[0], b.row[1], b.row[2], b.row[3], b.row[4], b.row[5], b.row[6], b.row[7]);
                    }
                }
            }
        }
    }
    T.checkOp = false;
    T.composed.clear();
    T.plan.clear();
    // ---- the templates: the composition's draws with their retained packets
    std::vector<const void*> touches = leanTouches_;
    pend->touches.clear();
    for (const void* t : touches) {
        if (t != nullptr && std::find(pend->touches.begin(), pend->touches.end(), t) == pend->touches.end()) pend->touches.push_back(t);
    }
    if (pend->stamp.material != nullptr && std::find(pend->touches.begin(), pend->touches.end(), static_cast<const void*>(pend->stamp.material)) == pend->touches.end()) {
        pend->touches.push_back(pend->stamp.material);
    }
    gpuscene::MaterialTable& mt = gpuscene::materialtable();
    const std::uint32_t matSlot = draws_[0]->matSlot;
    for (std::size_t k = 0; k < drawCount_; ++k) {
        if (draws_[k]->matSlot != matSlot || draws_[k]->treeKey == 0) {
            ++W.badOps;
            ++W.badBy[5];
            erase();
            return;
        }
    }
    if (matSlot >= gpuscene::MaterialTable::kCapacity) {
        ++W.badOps;
        erase();
        return;
    }
    pend->matSlot = matSlot;
    pend->matHash = mix(mix(0, matSlot), mt.structVersion(matSlot));
    pend->viewStamp = recViewStamp();
    const std::uint64_t viewKey = packetViewKey();
    const std::size_t cacheGroup = static_cast<std::size_t>(g_ - groups_.data());
    tro::OpRec* old = nullptr;
    if (const auto it = T.ops.find(pend->key); it != T.ops.end()) old = it->second.get();
    const bool sameStamp = old != nullptr && old->stamp == pend->stamp;
    // an image's api texture re-created since the old templates (another d3d9 object): not a template error, the templates are replaced below
    bool texMoved = false;
    if (sameStamp) {
        for (const tro::TexMap& x : old->texMap) {
            if (!treeTexUnchanged(x)) {
                texMoved = true;
                break;
            }
        }
    }
    for (std::size_t k = 0; k < drawCount_; ++k) {
        sc::DrawDesc& d = *draws_[k];
        const std::uint64_t pkey = (reinterpret_cast<std::uintptr_t>(op.raw.context) * 0x9E3779B97F4A7C15ull) ^ (static_cast<std::uint64_t>(op.raw.desc) * 0xC2B2AE3D27D4EB4Full) ^
                                   (static_cast<std::uint64_t>(op.list) << 56) ^ (static_cast<std::uint64_t>(k) << 48) ^ (static_cast<std::uint64_t>(cacheGroup) << 40) ^ viewKey;
        const auto pe = packets_.find(pkey);
        if (pe == packets_.end() || !pe->second.cd.valid || pe->second.cd.scenePipeline2 == d3d9::GamePipeline::None) {
            // (not retained yet: the variants / pipelines are still being built, or the packet cache refused the draw)
            ++W.notRetained;
            if (old != nullptr && sameStamp) continue;
            continue;
        }
        if (pend->tpls.count(d.treeKey) != 0) continue;  // (a second batch of the same bucket: the same draw)
        // the check: the retained template of this bucket against this composition (the instance count of stream 0 is the only part that moves)
        if (sameStamp) {
            if (const auto ot = old->tpls.find(d.treeKey); ot != old->tpls.end() && ot->second->desc != nullptr) {
                ++W.checkDraws;
                const auto f = d.streams[0].freq;
                d.streams[0].freq = ot->second->desc->streams[0].freq;
                const bool same = packetSameDesc(d, *ot->second->desc) && d.lightN == ot->second->desc->lightN && d.lightFirst == ot->second->desc->lightFirst &&
                                  d.lightTableLit == ot->second->desc->lightTableLit && d.matSlot == ot->second->desc->matSlot;
                d.streams[0].freq = f;
                if (!same && !texMoved) {
                    ++W.checkTplDiff;
                    if (T.logged < 8) {
                        ++T.logged;
                        const sc::DrawDesc& o = *ot->second->desc;
                        std::string what;
                        const auto chk = [&](bool diff, const char* n) { if (diff) what += std::string(what.empty() ? "" : ", ") + n; };
                        chk(d.vs != o.vs || d.ps != o.ps, "shaders"); chk(d.decl != o.decl, "decl"); chk(d.indices != o.indices, "indices");
                        chk(d.baseVertex != o.baseVertex || d.minIndex != o.minIndex || d.numVertices != o.numVertices || d.startIndex != o.startIndex || d.primCount != o.primCount, "geometry");
                        chk(d.samplerMask != o.samplerMask || d.inheritMask != o.inheritMask, "sampler mask"); chk(d.passFlags != o.passFlags, "pass flags"); chk(d.states != o.states, "states");
                        for (int i = 0; i < 4; ++i) chk(!(d.streams[i] == o.streams[i]), "stream");
                        for (std::uint32_t m = d.samplerMask; m != 0; m &= m - 1) { const auto st = sc::lowBit(m); if (!(d.samplers[st] == o.samplers[st])) { chk(true, "sampler"); what += fmt::format(" [st {}{} tex {} / {} ss {} {} {} {} {} {} {} / {} {} {} {} {} {} {}]", st, (d.inheritMask >> st) & 1 ? " inherit" : "", d.samplers[st].texture, o.samplers[st].texture, d.samplers[st].ss[0], d.samplers[st].ss[1], d.samplers[st].ss[2], d.samplers[st].ss[3], d.samplers[st].ss[4], d.samplers[st].ss[5], d.samplers[st].ss[6], o.samplers[st].ss[0], o.samplers[st].ss[1], o.samplers[st].ss[2], o.samplers[st].ss[3], o.samplers[st].ss[4], o.samplers[st].ss[5], o.samplers[st].ss[6]); } }
                        chk(d.lightN != o.lightN || d.lightFirst != o.lightFirst || d.lightTableLit != o.lightTableLit, "light params"); chk(d.matSlot != o.matSlot, "mat slot");
                        chk(d.tagSubMesh != o.tagSubMesh || d.tagMaterial != o.tagMaterial || d.tagMaterialName != o.tagMaterialName, "tag");
                        spdlog::error("native: owned trees: DIFFERING: {} bucket {:#x}: the composed draw differs from the retained template ({})", describe(op), d.treeKey, what);
                    }
                }
                if (!same || texMoved || ot->second->stale || ot->second->r.cd->v.scenePending) {
                    // replaced below
                } else {
                    treeMissCheck(*old, *ot->second, solid);
                    pend->tpls[d.treeKey] = std::move(old->tpls[d.treeKey]);
                    continue;
                }
            }
        }
        auto tpl = std::make_unique<tro::Tpl>();
        orec::DrawRec& r = tpl->r;
        r.cd = std::make_unique<d3d9::Device::CachedDraw>(pe->second.cd);
        for (std::size_t i = 0; i < 4; ++i) {
            r.streams[i] = d.streams[i].buffer;
            r.strides[i] = d.streams[i].stride;
        }
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
        if ((d.vsSet[0] >> 31 & 1u) != 0) std::memcpy(tpl->c31, d.vsF[31], 16);
        pend->tpls[d.treeKey] = std::move(tpl);
        if (old != nullptr && sameStamp && old->tpls.count(d.treeKey) != 0) ++W.replaced;
        else ++W.built;
    }
    if (pend->tpls.empty()) {
        erase();
        return;
    }
    if (sameStamp && !texMoved) {
        // (buckets of the op's earlier compositions that this one did not draw stay)
        for (auto& kv : old->tpls) {
            if (kv.second && pend->tpls.count(kv.first) == 0) pend->tpls[kv.first] = std::move(kv.second);
        }
    }
    if (sameStamp && !texMoved) {
        // (the kept buckets' images stay verified too)
        for (const tro::TexMap& x : old->texMap) {
            if (std::none_of(pend->texMap.begin(), pend->texMap.end(), [&](const tro::TexMap& y) { return y.res == x.res && y.stage == x.stage; })) pend->texMap.push_back(x);
        }
    }
    T.ops[pend->key] = std::move(pend);
    W.tplNs += ns(t0, tnow());
}

// ---- the plan ------------------------------------------------------------------------------------------------------------------------

// every instance of the op as the composition would draw it (queue order; leaves LOD-major), from the engine's tree objects. false = not plannable (why: 1 reads, 2 tree, 3 entity,
// 4 no table merge, 5 an instance the owned path does not take (no light list, static ambient, dynamic or unknown static lights), 6 LOD out of range)
bool StaticOps::treePlan(bool zpass, std::uint32_t type, bool tableMerge, const std::uint8_t* rb, const std::uint8_t* wrapper, const std::uint8_t* queue, std::int32_t n, std::vector<tro::Inst>& out, int& why) {
    out.clear();
    why = 0;
    const bool shadow = tro_ != nullptr && tro_->shadowView;  // (F4-4: a cascade's z op: leaves cast from one LOD coarser at alpha 1)
    if (n < 0 || n > 65536 || queue == nullptr || rb == nullptr || type > 3) {
        why = 1;
        return false;
    }
    if (!zpass && !tableMerge) {
        why = 4;
        return false;
    }
    std::uint8_t rbb[0x138];
    if (!rd(rb, 0, rbb, sizeof(rbb))) {
        why = 1;
        return false;
    }
    const auto rb16 = [&](std::size_t off) {
        std::uint16_t v;
        std::memcpy(&v, rbb + off, 2);
        return v;
    };
    const auto rbf = [&](std::size_t off) {
        float v;
        std::memcpy(&v, rbb + off, 4);
        return v;
    };
    struct Snap {
        const U8* tree;
        const U8* illum;
        std::int32_t l0, l1;
        float r0, r1, f6c;
        float world[16];
        bool sun;
    };
    static thread_local std::vector<Snap> snaps;
    snaps.clear();
    snaps.reserve(static_cast<std::size_t>(n));
    const U8* sunList = nullptr;
    bool sunListValue = false, dynListValue = false;
    const std::uint32_t lodAt = type == 0 ? 0x48 : 0x50;
    for (std::int32_t i = 0; i < n; ++i) {
        Snap s{};
        std::memcpy(&s.tree, queue + 0x18 * static_cast<std::size_t>(i), 8);
        std::memcpy(&s.illum, queue + 0x18 * static_cast<std::size_t>(i) + 8, 8);
        ObjSnap<0x70> ts;
        ts.load(s.tree);
        if (!ts.ok || ts.at<U8*>(0x40) == nullptr) {
            why = 2;
            return false;
        }
        s.l0 = type <= 1 ? ts.at<std::int32_t>(lodAt) : ts.at<std::int32_t>(0x58);
        s.l1 = ts.at<std::int32_t>(0x5c);
        s.r0 = ts.at<float>(0x60);
        s.r1 = ts.at<float>(0x64);
        s.f6c = ts.at<float>(0x6c);
        const U8* entity = (ts.at<std::uint8_t>(0x18) & 0x10) == 0 ? ts.at<U8*>(0x10) : nullptr;
        if (entity == nullptr || !rd(entity, 0x60, s.world, 64)) {
            why = 3;
            return false;
        }
        if (!zpass) {
            ObjSnap<0x98> is;
            is.load(s.illum);
            const U8* list = is.ok ? is.at<U8*>(0) : nullptr;
            const bool staticSh = is.ok && is.at<std::uint8_t>(0x90) != 0;
            const bool noSun = is.ok && is.at<std::uint8_t>(0xc) != 0;
            const std::uint32_t ns = is.ok ? is.at<std::uint8_t>(0xd) : 0;
            if (!is.ok || list == nullptr || staticSh) {
                why = 5;
                return false;
            }
            if (list != sunList) {
                sunList = list;
                sunListValue = get<std::uint8_t>(list, 0) != 0;
                dynListValue = get<std::uint8_t>(list, 0x34) != 0;
            }
            if (dynListValue) {
                why = 5;
                return false;
            }
            if (ns != 0) {
                bool allKnown = ns >= 1 && ns <= 2 && enh::LightTable::registryComplete();
                for (std::uint32_t e = 0; allKnown && e < ns; ++e) {
                    float p[3];
                    for (std::uint32_t j = 0; j < 3; ++j) p[j] = is.at<float>(0x10 + 0x40 * e + 4 * j);
                    allKnown = enh::LightTable::lightKnown(p);
                }
                if (!allKnown) {
                    why = 5;
                    return false;
                }
            }
            s.sun = sunListValue && !noSun;
        }
        snaps.push_back(s);
    }
    const auto fillRow = [&](tro::Inst& x, const Snap& s, float v0, float v1, float alpha) {
        x.row[0] = v0;
        x.row[1] = v1;
        x.row[2] = 0.0f;
        x.row[3] = alpha;
        for (std::uint32_t k = 0; k < 3; ++k) column(s.world, k, x.row + 4 + 4 * k);
    };
    if (type <= 1) {
        const std::size_t stripAt = type == 0 ? 0x40 : 0x50, startAt = type == 0 ? 0xe8 : 0xf8;
        const float vy = rbf(type == 0 ? 0xa0 : 0xa4);
        for (const Snap& s : snaps) {
            if (s.l0 < 0) continue;
            if (s.l0 > 7) {
                why = 6;
                return false;
            }
            tro::Inst x;
            x.tree = s.tree;
            x.illum = s.illum;
            x.sun = s.sun;
            x.strip = rb16(stripAt + 2 * static_cast<std::size_t>(s.l0));
            const std::uint16_t start = rb16(startAt + 2 * static_cast<std::size_t>(s.l0));
            x.key = treeBucketKey(type, zpass, x.sun, x.strip, start, 0);
            fillRow(x, s, s.f6c, vy, 1.0f);
            out.push_back(x);
        }
        return true;
    }
    const U8* rt = ptr(wrapper, 0);
    const std::int32_t lods = get<std::int32_t>(ptr(rt, 8), 0xf4);
    float v[2];
    if (!rd(wrapper, 0x34, v, 8) || lods < 0 || lods > 8) {
        why = 1;
        return false;
    }
    for (std::int32_t lodi = 0; lodi < lods; ++lodi) {
        const std::size_t at = 0x40 + 0x10 * static_cast<std::size_t>(type) + 2 * static_cast<std::size_t>(lodi);
        const std::uint16_t indices = rb16(at);
        const std::uint16_t start = rb16(at + 0xa8);
        if (indices == 0) break;
        for (const Snap& s : snaps) {
            float a;
            if (shadow) {
                // RenderLeafsZPass in a shadow map (composeTree): the one leaf LOD min(+0x58 + 1, LODs - 1), global alpha 1
                const std::int32_t l = s.l0 < 0 ? -1 : std::min(s.l0 + 1, lods - 1);
                if (l != lodi) continue;
                a = 1.0f;
            } else if (s.l0 == lodi && s.l0 > -1) {
                a = 80.0f / s.r0;
            } else if (s.l1 == lodi && s.l1 > -1) {
                a = 80.0f / s.r1;
            } else {
                continue;
            }
            tro::Inst x;
            x.tree = s.tree;
            x.illum = s.illum;
            x.sun = s.sun;
            x.key = treeBucketKey(type, zpass, x.sun, static_cast<std::uint32_t>(lodi), start, indices);
            fillRow(x, s, v[0], v[1], a);
            out.push_back(x);
        }
    }
    return true;
}

// ---- the frame tap's side ------------------------------------------------------------------------------------------------------------

// RVK_F4_NO_SHADOW_TREES=1 (A/B, GetEnvironmentVariableA once): the cascades' SpeedTree ops stay composed while the main view's are owned
static bool treeShadowEnvOff() {
    static const bool off = [] {
        char b[8] = {};
        return GetEnvironmentVariableA("RVK_F4_NO_SHADOW_TREES", b, sizeof(b)) != 0 && b[0] != '0';
    }();
    return off;
}

std::uint8_t StaticOps::treeViewKey() const { return tro_ ? tro_->viewKey : 255; }

std::uint32_t StaticOps::treeSlot(bool solid) const {
    if (tro_ && tro_->viewKey != 0 && tro_->viewKey != 255) return treeSlotOfKey(tro_->viewKey, solid);  // (F4-4 cascades 8..11, F3-4 terrain map / atlas cells)
    return solid ? kViewSlotTreeSolid : kViewSlotTreeZ;
}

// F3-4: the terrain map's / atlas cells' templates are dropped (native.owned_shadow_views went off)
void StaticOps::treeDropLightViews() {
    if (!tro_) return;
    for (auto it = tro_->ops.begin(); it != tro_->ops.end();) {
        if (it->first.view >= kKeyTerrain || it->second->empty) it = tro_->ops.erase(it);  // (the cascades' empty records belong to this stage too)
        else ++it;
    }
    if (tro_->shadowView && tro_->viewKey >= kKeyTerrain && tro_->viewKey != 255) {
        tro_->active = false;
        for (auto& v : tro_->taken) v.clear();
    }
}

void StaticOps::treeDropShadow() {
    if (!tro_) return;
    for (auto it = tro_->ops.begin(); it != tro_->ops.end();) {
        if (it->first.view != 0) it = tro_->ops.erase(it);
        else ++it;
    }
    if (tro_->shadowView) {
        tro_->active = false;
        for (auto& v : tro_->taken) v.clear();
    }
}

void StaticOps::treeViewBegin(bool ownedFrame, ViewKind kind, std::int32_t cascade, std::int32_t cell) {
    if (!ownedTrees_ || !tro_) return;
    tro::State& T = *tro_;
    for (auto& v : T.taken) {
        T.win().opsLate += v.size();  // (ops of an earlier view whose pass never began: lost for that frame)
        v.clear();
    }
    T.active = false;
    T.shadowView = kind != ViewKind::Main;
    T.lightView = kind == ViewKind::Terrain || kind == ViewKind::PointAtlas;
    T.cascade = cascade;
    // (F3-4: the terrain map / an atlas cell too while native.owned_shadow_views is on: their own keys and view slots)
    const std::uint64_t key = ownedKeyOf(kind, cascade, cell);
    T.viewKey = kind == ViewKind::Main ? 0 : (ownedShadowKind(kind) && key != 0 && key < 255 && !T.shadowTripped && !treeShadowEnvOff()) ? static_cast<std::uint8_t>(key) : 255;
    if (T.viewKey == 255) return;  // (a view the trees are not owned in: the terrain map, point-light cells, cascades with owned_shadows off)
    ++T.win().views;
    if (!ownedFrame) {
        ++T.win().compViews;
        return;
    }
    gpuscene::DrawRecordTable& tab = gpuscene::drawrecords();
    const bool ready = ownedPersistent_ && !ownedTripped_ && ownedStatic_ && tableLit_ && T.ramp != nullptr && tab.ready() && gpuscene::viewtable().ready() && dev_.sceneViewsReady() &&
                       dev_.ownedTreeObserversOk() && treesVerified_ > 0 && lean_ && leanAnimVerified_ > 0 && qsort_ != nullptr;
    if (!ready) {
        std::string obs;
        std::string why = !ownedPersistent_ ? "native.owned_persistent off" : ownedTripped_ ? "owned path tripped" : !ownedStatic_ ? "owned static off" : !tableLit_ ? "table lighting off"
                        : T.ramp == nullptr ? "no identity ramp" : !tab.ready() ? "draw record table not ready" : !(gpuscene::viewtable().ready() && dev_.sceneViewsReady()) ? "view table not ready"
                        : !dev_.ownedTreeObserversOk(&obs) ? obs : treesVerified_ <= 0 ? "trees not verified" : !lean_ ? "native.lean off" : leanAnimVerified_ <= 0 ? "lean animation not verified"
                        : "no qsort";
        if (why != T.gate) spdlog::info("native: owned trees: gated: {}", why);
        T.gate = std::move(why);
        if (T.logged < 6) {
            ++T.logged;
            spdlog::info("native: owned trees: not this view (persistent {}, tripped {}, owned static {}, table lighting {}, ramp {}, record table {}, view table {} / {}, observers ok {}, trees verified {}, lean {} / {}, qsort {})", ownedPersistent_,
                         ownedTripped_, ownedStatic_, tableLit_, T.ramp != nullptr, tab.ready(), gpuscene::viewtable().ready(), dev_.sceneViewsReady(), dev_.ownedTreeObserversOk(), treesVerified_, lean_, leanAnimVerified_,
                         qsort_ != nullptr);
        }
        return;
    }
    if (!T.gate.empty()) spdlog::info("native: owned trees: gate open (was: {})", T.gate);
    T.gate.clear();
    T.active = true;
    ++T.win().ownedViews;
}

// F3-4 (frame-ownership.md 14.29): a shadow view's fronds op that draws nothing (RenderFrondsZPass 0x6f0cc0: leaf card vertices rb +0x3c > frond vertices rb +0x3a) gets an
// empty record: the owned frames take it out of the list like a drawn op and run only the slot's side effect (the queue sort); the rule is read again at the pass
void StaticOps::treeEmptyRecord(const Op& op, const std::uint8_t* res, const std::uint8_t* rb) {
    tro::State& T = *tro_;
    const tro::OpKey key{op.raw.context, op.raw.desc, static_cast<std::uint8_t>(op.list), treeViewKey()};
    auto& slot = T.ops[key];
    if (slot && slot->empty && slot->stamp.res == res && slot->stamp.rb == rb) return;  // (unchanged)
    auto r = std::make_unique<tro::OpRec>();
    r->key = key;
    r->type = 1;
    r->stamp.res = res;
    r->stamp.rb = rb;
    r->empty = true;
    r->builtFrame = dev_.stats().frames;
    slot = std::move(r);
    ++T.win().built;
}

bool StaticOps::treeEmptyHolds(const Op& op, const tro::OpRec& rec, const std::uint8_t*& rb) const {
    rb = nullptr;
    if (op.list != List::ZPass || ((op.raw.desc >> 16) & 0x10u) == 0 || (op.raw.desc & 0xffffu) != 1u) return false;
    U8* ps = ptr(op.raw.context, 0x18);
    U8* res = ptr(ps, 0x38);
    U8* b = ptr(res, 0x78);
    if (res == nullptr || b == nullptr || res != rec.stamp.res || b != rec.stamp.rb) return false;
    if (get<std::uint16_t>(b, 0x3c) <= get<std::uint16_t>(b, 0x3a)) return false;
    rb = b;
    return true;
}

bool StaticOps::treeSelect(const Op& op) {
    if (!tro_ || !tro_->active || op.kind != OpKind::SpeedTree) return false;
    if (op.list != List::ZPass && op.list != List::Solid) return false;
    tro::State& T = *tro_;
    ++T.win().opsSeen;
    const bool zpass = op.list == List::ZPass;
    // a shadow map's z op (desc bit 0x10 << 16) belongs to the cascades' views, the others to the main view; the shadow views have no solid list
    if ((zpass && ((op.raw.desc >> 16) & 0x10u) != 0) != T.shadowView || (T.shadowView && !zpass)) return false;
    const std::uint32_t type = zpass ? (op.raw.desc & 0xffffu) : op.raw.desc;
    if (type > 3) return false;
    const auto it = T.ops.find(tro::OpKey{op.raw.context, op.raw.desc, static_cast<std::uint8_t>(op.list), treeViewKey()});
    if (it == T.ops.end()) {
        ++T.win().lateNoRec;
        return false;
    }
    return true;
}

void StaticOps::treeTake(List list, std::vector<Op>&& ops, std::uint64_t nsTake) {
    if (!tro_) return;
    tro_->win().opsTaken += ops.size();
    tro_->win().takeNs += nsTake;
    tro_->taken[list == List::Solid ? 1 : 0] = std::move(ops);
}

bool StaticOps::treeStampOf(const Op& op, tro::Stamp& st, std::uint32_t& type, const std::uint8_t*& rb, const std::uint8_t*& wrapper, const std::uint8_t*& shader, bool& zpass) {
    st = {};
    zpass = op.list == List::ZPass;
    type = zpass ? (op.raw.desc & 0xffffu) : op.raw.desc;
    if (type > 3) return false;
    U8* ps = ptr(op.raw.context, 0x18);
    U8* res = ptr(ps, 0x38);
    rb = ptr(res, 0x78);
    if (rb == nullptr) return false;
    wrapper = ptr(rb, 8);
    U8* material = ptr(rb, 0x80 + 8 * static_cast<std::size_t>(type));
    shader = ptr(material, 0x70);
    if (wrapper == nullptr || shader == nullptr) return false;
    U8* admin = ptr(shader, 0x220);
    st.res = res;
    st.rb = rb;
    st.material = material;
    st.shader = shader;
    st.visual = ptr(rb, 0x18 + 8 * static_cast<std::size_t>(type));
    st.transformation = get<std::int32_t>(shader, 0x2b8);
    st.variantEpoch = dev_.variantEpoch();
    st.registryGen = enh::fullLitSkipsReads() ? enh::LightTable::registryGeneration() : 0;
    st.hardware = get<std::int32_t>(admin, 0x320) != 0;
    st.tableMerge = tableLit_ && !zpass && enh::fullLitTreeMerge();
    return true;
}

// a template made ready for the pass: its derivation is a persistent DrawBlock slot (rewritten only when an input moved: the owned_persistent recipe)
int StaticOps::treeTemplate(tro::Tpl& t, const d3d9::OwnedWindows& viewWin, std::uint64_t epoch, bool solid, std::uint32_t scrubStride, std::uint32_t scrubPhase, tro::Stats& W, std::uint64_t& scrubTick,
                            const char* what) {
    gpuscene::DrawRecordTable& tab = gpuscene::drawrecords();
    const orec::DrawRec& r = t.r;
    orec::Persist& P = r.persist;
    const d3d9::Device::CachedDraw& cd = *r.cd;
    const gpuscene::ComposeParams p = paramsOfTpl(r);
    const bool amb = ownedAmbientLit(r, p);
    d3d9::OwnedWindows::Win objWin;
    objWin.kind = d3d9::OwnedWindows::Kind::Object;
    objWin.base = tab.zeroRef().plus(-static_cast<std::int64_t>(shader::abi::kVsWindowFirst[2]) * 16);
    objWin.covered = shader::abi::kVsWindowFirst[3] - shader::abi::kVsWindowFirst[2];
    ownedBuildWindows(r, viewWin, objWin, p, true, P.ow);
    if (dev_.vertexShader() != cd.vs) dev_.setVertexShaderRef(d3d9::ShaderRef(const_cast<d3d9::Shader*>(cd.vs)));
    if (dev_.pixelShader() != cd.ps) dev_.setPixelShaderRef(d3d9::ShaderRef(const_cast<d3d9::Shader*>(cd.ps)));
    d3d9::draws::Tag& tag = dev_.drawTag();
    tag.subMesh = r.tagSubMesh;
    tag.material = r.tagMaterial;
    tag.meshResource = r.tagMeshResource;
    tag.water = static_cast<d3d9::draws::Water>(r.tagWater);
    tag.materialName = r.tagMaterialName;
    bool allowed = false;
    const std::uint64_t sig = dev_.ownedDrawSig(cd, r.streams, r.strides, r.indices, r.tex, amb, P.ow, allowed);
    if (!allowed) return 1;
    const bool reuse = P.valid && P.handle.has() && tab.live(P.handle.slot) && P.epoch == epoch && P.sig == sig;
    const bool scrub = reuse && scrubStride != 0 && (++scrubTick % scrubStride) == scrubPhase;
    if (reuse && !scrub) {
        ++W.reused;
        return 0;
    }
    if (!P.handle.has()) {
        const std::uint32_t slot = tab.alloc();
        if (slot == gpuscene::kNoSlot) {
            ++W.tableFull;
            return 2;
        }
        P.handle.slot = slot;
    }
    d3d9::Device::DeriveIn in;
    ownedMakeIn(r, amb, in);
    d3d9::Device::OwnedPersist out;
    const int rc = dev_.ownedDerive(cd, in, P.ow, tab.blockRef(P.handle.slot), tab.zeroRef(), true, out);
    if (rc != 0) {
        P.valid = false;
        ++W.refused;
        if (rc == 1 || rc == 4 || rc == 6 || rc == 7) t.stale = true;
        return 3;
    }
    if (scrub) {
        if (std::memcmp(&tab.record(P.handle.slot), &out.rec, sizeof(out.rec)) != 0 || std::memcmp(&tab.block(P.handle.slot), &out.blk, sizeof(out.blk)) != 0) {
            ++W.checkMiss;
            spdlog::error("native: {}: MISS (rolling scrub): the persistent block of bucket {:#x} differs from a fresh derivation although nothing it was derived from moved", what, t.key);
        }
        ++W.checkMissCompared;
    } else {
        ++W.derived;
    }
    if (!scrub) tab.write(P.handle.slot, out.rec, out.blk);
    P.valid = true;
    P.epoch = epoch;
    P.sig = sig;
    P.bucket = out.bucketKey;
    (void)solid;
    return 0;
}

// a composed frame: would the owned path reuse this template's slot now? then its content must be what a fresh derivation gives
void StaticOps::treeMissCheck(tro::OpRec& rec, tro::Tpl& t, bool solid) {
    (void)rec;
    const orec::DrawRec& r = t.r;
    orec::Persist& P = r.persist;
    if (!P.valid || !P.handle.has()) return;
    if ((++tro_->scrubTick & 7u) != 0) return;  // (one template in eight per composed frame: each check is a full derivation)
    gpuscene::DrawRecordTable& tab = gpuscene::drawrecords();
    gpuscene::ViewTable& vt = gpuscene::viewtable();
    const std::uint32_t viewSlot = treeSlot(solid);
    d3d9::OwnedWindows viewWin;
    if (!tab.ready() || !tab.live(P.handle.slot) || !vt.ready() || !vt.fillWindows(viewSlot, viewWin)) return;
    const gpuscene::ComposeParams p = paramsOfTpl(r);
    bool amb = false;
    {
        const d3d9::Device::CachedDraw& cd = *r.cd;
        if (cd.v.needsAmbient() && r.illumSlot != gpuscene::kNoSlot) amb = ownedAmbientLit(r, p);
    }
    d3d9::OwnedWindows::Win objWin;
    objWin.kind = d3d9::OwnedWindows::Kind::Object;
    objWin.base = tab.zeroRef().plus(-static_cast<std::int64_t>(shader::abi::kVsWindowFirst[2]) * 16);
    objWin.covered = shader::abi::kVsWindowFirst[3] - shader::abi::kVsWindowFirst[2];
    d3d9::OwnedWindows dw;
    ownedBuildWindows(r, viewWin, objWin, p, false, dw);
    bool allowed = false;
    const std::uint64_t sig = dev_.ownedDrawSig(*r.cd, r.streams, r.strides, r.indices, r.tex, amb, dw, allowed);
    if (!allowed || P.sig != sig || !dev_.treeEnvSame(solid, viewSlot, P.epoch)) return;  // (the owned path would derive it again)
    d3d9::Device::DeriveIn in;
    ownedMakeIn(r, amb, in);
    d3d9::Device::SceneView sv;
    if (dev_.deriveScene(*r.cd, in, sv) != 0) return;
    d3d9::Device::OwnedPersist fresh;
    if (dev_.ownedBuild(*r.cd, sv, dw, tab.blockRef(P.handle.slot), tab.zeroRef(), fresh) != 0) return;
    ++tro_->win().checkMissCompared;
    if (std::memcmp(&tab.record(P.handle.slot), &fresh.rec, sizeof(fresh.rec)) != 0 || std::memcmp(&tab.block(P.handle.slot), &fresh.blk, sizeof(fresh.blk)) != 0) {
        ++tro_->win().checkMiss;
        if (tro_->logged < 8) {
            ++tro_->logged;
            spdlog::error("native: owned trees: MISS (check frame): the persistent block of bucket {:#x} differs from a fresh derivation although nothing it was derived from moved", t.key);
        }
    }
}

// ---- the pass ------------------------------------------------------------------------------------------------------------------------

std::uint32_t StaticOps::treePass(bool solid) {
    if (!tro_) return 0;
    tro::State& T = *tro_;
    std::vector<Op>& ops = T.taken[solid ? 1 : 0];
    if (ops.empty()) return 0;
    tro::Stats& W = T.win();
    if (!inView_ || (viewKind_ != ViewKind::Main) != T.shadowView || (T.shadowView && ownedKeyNow() != T.viewKey) || !T.active) {
        W.opsLate += ops.size();
        ops.clear();
        return 0;
    }
    const auto t0 = tnow();
    ret::gLog = nullptr;
    finishOp();
    leanAbort();
    d3d9::draws::Tag& tag = dev_.drawTag();
    const d3d9::draws::Tag savedTag = tag;
    const d3d9::ShaderRef vsSaved = dev_.vertexShaderRef(), psSaved = dev_.pixelShaderRef();
    // the register file, given back when the observers were shown instances (ownedTreeObserve loads window images + rows into it)
    static float savedVs[d3d9::kVsFloatConsts][4], savedPs[d3d9::kPsFloatConsts][4];
    bool regsSaved = false;
    // an op composed where the engine's slot would have run it (a taken op's slot never runs)
    std::uint32_t drawn = 0;
    const auto composeLate = [&](Op& op, std::uint64_t& counter) {
        ++counter;
        ++W.opsLate;
        dev_.setSceneIssue(false);
        tag = savedTag;
        tag.source = d3d9::draws::Source::SpeedTree;
        tag.context = op.raw.context;
        tag.entity = op.entity != nullptr ? op.entity : ptr(op.raw.context, 0x10);
        tag.opDesc = op.raw.desc;
        tag.subMesh = tag.material = tag.meshResource = nullptr;
        tag.water = d3d9::draws::Water::None;
        tag.materialName = nullptr;
        opEnter(op);
        if (op_ == &op) drawn += static_cast<std::uint32_t>(drawCount_);
        finishOp();
        dev_.setSceneIssue(true);
    };
    const auto restore = [&] {
        dev_.setSceneIssue(false);
        tag = savedTag;
        dev_.setVertexShaderRef(vsSaved);
        dev_.setPixelShaderRef(psSaved);
        if (regsSaved) {
            dev_.setVertexShaderConstantF(0, savedVs[0], d3d9::kVsFloatConsts);
            dev_.setPixelShaderConstantF(0, savedPs[0], d3d9::kPsFloatConsts);
        }
        finishOp();
        ops.clear();
    };
    // the view as the first op's composition would make it
    tro::Stamp st0;
    std::uint32_t type0 = 0;
    const std::uint8_t *rb0 = nullptr, *wr0 = nullptr, *sh0 = nullptr;
    bool z0 = false;
    bool viewOk = false;
    if (treeStampOf(ops[0], st0, type0, rb0, wr0, sh0, z0)) viewOk = viewAndMirror(sh0);
    if (viewOk) std::memcpy(mirror_.adminVs[0], view_.vp, 64);  // (no depth-bias material is owned)
    if (viewOk) treeWind();
    ++opSerial_;
    const std::uint64_t viewStamp = viewOk ? recViewStamp() : 0;
    d3d9::OwnedWindows viewWin;
    bool viewWinOk = false;
    const std::uint32_t viewSlot = treeSlot(solid);
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
    T.inst.clear();
    T.instTpl.clear();
    T.instEntry.clear();
    T.opOfInst.clear();
    std::vector<std::uint32_t> drawnOps;
    struct MemoSlot {
        std::uint64_t key = 0;
        std::uint32_t entry = 0, gen = 0;
    };
    static thread_local std::vector<MemoSlot> memo(8192);
    static thread_local std::uint32_t memoGen = 0;
    ++memoGen;
    const auto memoFind = [&](std::uint64_t k) -> MemoSlot& {
        std::size_t at = static_cast<std::size_t>((k * 0x9E3779B97F4A7C15ull) >> 40) & (memo.size() - 1);
        while (memo[at].gen == memoGen && memo[at].key != k) at = (at + 1) & (memo.size() - 1);
        return memo[at];
    };
    const std::uint32_t scrubStride = 64;
    const std::uint32_t scrubPhase = static_cast<std::uint32_t>((++T.scrubTick * 7919u) % scrubStride);
    std::vector<std::uint8_t>& queueBytes = scr_.treeQueue;
    for (std::size_t oi = 0; oi < ops.size(); ++oi) {
        Op& op = ops[oi];
        const auto it = T.ops.find(tro::OpKey{op.raw.context, op.raw.desc, static_cast<std::uint8_t>(op.list), treeViewKey()});
        if (it == T.ops.end()) {
            composeLate(op, W.lateNoRec);
            continue;
        }
        tro::OpRec& rec = *it->second;
        if (rec.empty) {
            // F3-4: an empty record (a shadow fronds op that draws nothing): the rule holds = the slot's only effect, the queue sort; else the composition decides
            const std::uint8_t* erb = nullptr;
            if (!treeEmptyHolds(op, rec, erb)) {
                composeLate(op, W.lateStamp);
                continue;
            }
            const auto tSide = tnow();
            leanTreeSort(erb, true);
            W.sideNs += ns(tSide, tnow());
            drawnOps.push_back(static_cast<std::uint32_t>(oi));
            ++W.emptyOps;
            continue;
        }
        tro::Stamp st;
        std::uint32_t type = 0;
        const std::uint8_t *rb = nullptr, *wrapper = nullptr, *shader = nullptr;
        bool zpass = false;
        if (!treeStampOf(op, st, type, rb, wrapper, shader, zpass) || zpass == solid || !(st == rec.stamp) || !shaderClassOk(shader)) {
            composeLate(op, W.lateStamp);
            continue;
        }
        if (rec.viewStamp != viewStamp) {
            composeLate(op, W.lateView);
            continue;
        }
        // the material's record polled (once per frame) and its structure as the templates saw it
        {
            std::uint64_t h = 0;
            bool ok = rec.matSlot < gpuscene::MaterialTable::kCapacity;
            if (ok) {
                if (rec.matSlot >= ownedPolled_.size()) ownedPolled_.resize(gpuscene::MaterialTable::kCapacity, 0);
                if (ownedPolled_[rec.matSlot] != pollFrame) {
                    ok = mt.pollOwned(rec.matSlot, &StaticOps::materialTexture, this);
                    if (ok) ownedPolled_[rec.matSlot] = pollFrame;
                }
                h = mix(mix(0, rec.matSlot), mt.structVersion(rec.matSlot));
            }
            if (!ok || h != rec.matHash) {
                composeLate(op, W.lateMaterial);
                continue;
            }
        }
        // the templates' textures: a re-created api texture is another d3d9 object (the template holds the old one): compose late, the new template follows
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
        // the engine slot's side effects (they run first in the slot's code): the queue sorted in place, MaskReference 80, the resources touched
        const auto tSide = tnow();
        leanTreeSort(rb, zpass);
        leanMaskRef(shader);
        for (const void* r : rec.touches) touchNow(r);
        W.sideNs += ns(tSide, tnow());
        // the queue as the engine sorted it, then the plan
        const std::int32_t n = get<std::int32_t>(rb, zpass ? 0xd8 : 0xc0);
        U8* qp = ptr(rb, zpass ? 0xd0 : 0xb8);
        if (n <= 0) {
            drawnOps.push_back(static_cast<std::uint32_t>(oi));
            continue;  // (nothing queued: the slot draws nothing)
        }
        if (n > 65536 || qp == nullptr) {
            composeLate(op, W.latePlan);
            continue;
        }
        queueBytes.resize(static_cast<std::size_t>(n) * 0x18);
        if (!rd(qp, 0, queueBytes.data(), queueBytes.size())) {
            composeLate(op, W.latePlan);
            continue;
        }
        const auto tPlan = tnow();
        const std::size_t first = T.inst.size();
        static thread_local std::vector<tro::Inst> plan;
        int why = 0;
        const bool planned = treePlan(zpass, type, st.tableMerge, rb, wrapper, queueBytes.data(), n, plan, why);
        W.planNs += ns(tPlan, tnow());
        if (!planned) {
            composeLate(op, why == 5 ? W.lateSpecial : W.latePlan);
            continue;
        }
        // the templates (made ready once per pass) and, in the solid pass, each instance's lighting entry
        bool fail = false;
        std::uint64_t failCounter = 0;
        std::uint64_t* failWhy = &failCounter;
        tro::Tpl* lastTpl = nullptr;
        std::uint64_t lastKey = 0;
        const auto tTpl = tnow();
        for (const tro::Inst& x : plan) {
            if (type == 1 && x.strip == 0) continue;  // (RenderFronds: a batch whose strip length is 0 is not drawn)
            tro::Tpl* tpl = lastTpl;
            if (tpl == nullptr || x.key != lastKey) {
                const auto f = rec.tpls.find(x.key);
                if (f == rec.tpls.end()) {
                    failWhy = &W.lateNoTemplate;
                    fail = true;
                    break;
                }
                tpl = f->second.get();
                lastTpl = tpl;
                lastKey = x.key;
                if (tpl->passSerial != serial) {
                    tpl->passSerial = serial;
                    tpl->passRun = 0;
                    const int rc = treeTemplate(*tpl, viewWin, epoch, solid, scrubStride, scrubPhase, T.win(), T.scrubTick, "owned trees");
                    if (rc != 0) {
                        tpl->passSerial = 0;
                        failWhy = &W.lateTemplate;
                        fail = true;
                        break;
                    }
                    tpl->devBucket = dev_.ownedBucketKey(*tpl->r.cd, viewSlot);
                }
            }
            std::uint32_t entry = gpuscene::kNoEntry;
            if (solid) {
                const gpuscene::ComposeParams p = paramsOfTpl(tpl->r);
                if (!p.tableLit || p.holdsAll) {
                    failWhy = &W.lateSpecial;
                    fail = true;
                    break;
                }
                const std::uint64_t lk = mix(reinterpret_cast<std::uintptr_t>(x.illum), (static_cast<std::uint64_t>(p.n) << 8) | p.firstLight);
                MemoSlot& m = memoFind(lk);
                if (m.gen == memoGen) {
                    entry = m.entry;
                } else {
                    const std::uint32_t slot = illum.ensureSlot(x.illum);
                    entry = slot == gpuscene::kNoSlot ? gpuscene::kNoEntry : li.entryFor(slot, p);
                    if (memoGen != 0 && T.inst.size() < memo.size() / 2) m = {lk, entry, memoGen};
                }
                if (entry == gpuscene::kNoEntry || !li.ready()) {
                    failWhy = &W.lateSpecial;
                    fail = true;
                    break;
                }
            }
            T.inst.push_back(x);
            T.instTpl.push_back(tpl);
            T.instEntry.push_back(entry);
            T.opOfInst.push_back(static_cast<std::uint32_t>(oi));
        }
        W.tplNs += ns(tTpl, tnow());
        if (fail) {
            T.inst.resize(first);
            T.instTpl.resize(first);
            T.instEntry.resize(first);
            T.opOfInst.resize(first);
            composeLate(op, *failWhy);
            continue;
        }
        drawnOps.push_back(static_cast<std::uint32_t>(oi));
    }
    // ---- rows and records, then the indirect calls
    const std::uint32_t P = static_cast<std::uint32_t>(T.inst.size());
    bool flushed = true;
    std::uint32_t items = 0;
    if (P != 0) {
        const auto tRows = tnow();
        flushed = false;
        if (P <= T.rampLen) {
            // runs: the instances of each template, templates ordered by their device bucket (consecutive templates of one bucket become one indirect call)
            struct Run {
                tro::Tpl* tpl;
                std::vector<std::uint32_t> idx;
            };
            static thread_local std::vector<Run> runs;
            runs.clear();
            for (std::uint32_t i = 0; i < P; ++i) {
                tro::Tpl* tpl = T.instTpl[i];
                if (tpl->passRun == 0) {
                    tpl->passRun = static_cast<std::uint32_t>(runs.size()) + 1;
                    runs.push_back({tpl, {}});
                }
                runs[tpl->passRun - 1].idx.push_back(i);
            }
            std::vector<std::uint32_t>& order = T.order;
            order.resize(runs.size());
            for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return runs[a].tpl->devBucket < runs[b].tpl->devBucket; });
            rhi::RingAlloc rows = dev_.allocOwnedRing(static_cast<std::uint64_t>(tro::chunkCount(P)) * tro::kChunkBytes, 16);
            if (rows.ptr != nullptr) {
                auto* base = static_cast<std::uint8_t*>(rows.ptr);
                // chunks of 50 positions (tree_rows.h): each = the c0 .. c31 prefix (zero, c31 = the header: the composed value of the first template that has one) + 50 rows
                for (std::uint32_t k = 0; k < tro::chunkCount(P); ++k) {
                    std::uint8_t* chunk = base + tro::chunkBase(k);
                    std::memset(chunk, 0, static_cast<std::size_t>(tro::kChunkPrefixRegs) * 16);
                    std::memcpy(chunk + 31 * 16, runs[order[0]].tpl->c31, 16);
                }
                std::uint32_t pos = 0;
                T.items.clear();
                T.recs.clear();
                T.posRec.clear();
                T.posRec.reserve(P);
                bool ok = true;
                // per-instance lighting in ONE draw: a DrawRecord per (template, chunk, lighting entry) and the ref list gives every position its own record index; an item (= one
                // indirect command) is the part of a template's run that lies in one chunk of 50 positions (its records' window 2 base is that chunk's)
                struct EntSlot {
                    std::uint32_t entry = 0, gen = 0, rec = 0;
                };
                static thread_local std::vector<EntSlot> ents(4096);
                static thread_local std::vector<float> obsRows;
                static thread_local std::uint32_t entGen = 0;
                for (const std::uint32_t ri : order) {
                    Run& run = runs[ri];
                    tro::Tpl& tpl = *run.tpl;
                    const std::uint32_t slot = tpl.r.persist.handle.slot;
                    if (!tab.live(slot)) {
                        ok = false;
                        break;
                    }
                    const shader::abi::DrawRecord base0 = tab.record(slot);
                    const std::uint32_t runN = static_cast<std::uint32_t>(run.idx.size());
                    // the observers (the RT scene) see the run as they saw the composed batches: the tag of the op, the rows as registers, the template's windows
                    if (dev_.ownedTreeObserved(*tpl.r.cd) && tpl.r.persist.valid) {
                        if (!regsSaved) {
                            std::memcpy(savedVs, dev_.vsConstants(), sizeof(savedVs));
                            std::memcpy(savedPs, dev_.psConstants(), sizeof(savedPs));
                            regsSaved = true;
                        }
                        const Op& top = ops[T.opOfInst[run.idx[0]]];
                        tag.source = d3d9::draws::Source::SpeedTree;
                        tag.context = top.raw.context;
                        tag.entity = top.entity != nullptr ? top.entity : ptr(top.raw.context, 0x10);
                        tag.opDesc = top.raw.desc;
                        tag.subMesh = tpl.r.tagSubMesh;
                        tag.material = tpl.r.tagMaterial;
                        tag.meshResource = tpl.r.tagMeshResource;
                        tag.water = static_cast<d3d9::draws::Water>(tpl.r.tagWater);
                        tag.materialName = tpl.r.tagMaterialName;
                        obsRows.clear();
                        for (const std::uint32_t i : run.idx) obsRows.insert(obsRows.end(), T.inst[i].row, T.inst[i].row + 16);
                        W.observed += dev_.ownedTreeObserve(*tpl.r.cd, tpl.r.persist.ow, T.ramp.get(), obsRows.data(), runN);
                    }
                    d3d9::Device::TreeItem item;
                    item.cd = tpl.r.cd.get();
                    item.bucket = tpl.devBucket;
                    item.instances = 0;
                    std::uint32_t chunk = ~0u, distinct = 0, zRec = 0;
                    std::uint64_t chunkBits = 0;  // window 2's base of the item's chunk
                    for (std::uint32_t q = 0; q < runN && ok; ++q) {
                        const std::uint32_t i = run.idx[q];
                        if (tro::chunkOf(pos) != chunk) {
                            if (item.instances != 0) T.items.push_back(item);
                            item.instances = 0;
                            chunk = tro::chunkOf(pos);
                            chunkBits = rows.ref.plus(static_cast<std::int64_t>(tro::chunkBase(chunk))).bits;  // c[n] = base + 16 n: c32 = row 0 of the chunk (the prefix holds c31)
                            ++entGen;
                            distinct = 0;
                            if (!solid) {
                                shader::abi::DrawRecord r = base0;
                                r.vsWin[2] = chunkBits;
                                zRec = static_cast<std::uint32_t>(T.recs.size());
                                T.recs.push_back(r);
                            }
                        }
                        std::memcpy(base + tro::rowOffset(pos), T.inst[i].row, 64);
                        ++pos;
                        ++item.instances;
                        if (!solid) {
                            T.posRec.push_back(zRec);
                            continue;
                        }
                        const std::uint32_t entry = T.instEntry[i];
                        std::size_t at = static_cast<std::size_t>((entry * 0x9E3779B1u) >> 8) & (ents.size() - 1);
                        while (ents[at].gen == entGen && ents[at].entry != entry) at = (at + 1) & (ents.size() - 1);
                        if (ents[at].gen != entGen) {
                            if (++distinct > ents.size() / 2) {
                                ok = false;
                                break;
                            }
                            shader::abi::DrawRecord r = base0;
                            r.vsWin[2] = chunkBits;
                            r.psWin[3] = li.windowBase(entry).bits;
                            ents[at] = {entry, entGen, static_cast<std::uint32_t>(T.recs.size())};
                            T.recs.push_back(r);
                        }
                        T.posRec.push_back(ents[at].rec);
                    }
                    if (!ok) break;
                    if (item.instances != 0) T.items.push_back(item);
                    run.idx.clear();
                    tpl.passRun = 0;
                }
                W.rowNs += ns(tRows, tnow());
                if (ok && pos == P) {
                    items = static_cast<std::uint32_t>(T.items.size());
                    const auto tf = tnow();
                    const std::uint64_t pk0 = dev_.treeFlushStats.packets;
                    flushed = dev_.ownedTreeFlush(T.items, T.recs, T.posRec, viewSlot, P);
                    W.records += T.recs.size();
                    W.packets += dev_.treeFlushStats.packets - pk0;
                    W.flushNs += ns(tf, tnow());
                }
            }
            for (Run& run : runs) {
                run.tpl->passRun = 0;
                run.idx.clear();
            }
        }
        if (!flushed) {
            // nothing was queued: the ops that contributed are composed instead
            std::uint32_t last = ~0u;
            std::vector<std::uint32_t> lateOps;
            for (const std::uint32_t oi : T.opOfInst) {
                if (oi == last) continue;
                last = oi;
                lateOps.push_back(oi);
            }
            for (const std::uint32_t oi : lateOps) {
                std::erase(drawnOps, oi);
                composeLate(ops[oi], W.lateFlush);
            }
        }
    }
    for (const std::uint32_t oi : drawnOps) {
        (void)oi;
        ++W.opsDrawn;
    }
    if (flushed) {
        W.instances += P;
        W.items += items;
        ++W.flushes;
        drawn += items;
    }
    restore();
    W.tplNs += 0;
    (void)t0;
    return drawn;
}

void StaticOps::treeReport() {
    if (!ownedTrees_ || !tro_) return;
    tro::State& T = *tro_;
    treeReportOne(T.window, T.total, "main view", false);
    if (T.shadowWindow.views != 0 || T.shadowWindow.opsSeen != 0 || T.shadowWindow.compBatches != 0) treeReportOne(T.shadowWindow, T.shadowTotal, "cascades", true);
    else T.shadowWindow = {};
    if (T.lightWindow.views != 0 || T.lightWindow.opsSeen != 0 || T.lightWindow.compBatches != 0) treeReportOne(T.lightWindow, T.lightTotal, "terrain map / atlas cells", true);  // F3-4
    else T.lightWindow = {};
}

void StaticOps::treeReportOne(tro::Stats& q, tro::Stats& total, const char* what, bool shadow) {
    tro::State& T = *tro_;
    total.add(q);
    const double nv = static_cast<double>(std::max<std::uint64_t>(1, q.ownedViews));
    // (checkKeyDiff is reported, not fatal: the engine draws a whole batch with its FIRST instance's start index, the plan gives each instance its own LOD: ~0.25 % of the instances, frame-ownership.md 7.25)
    // (a template that differs from the next composition of its bucket is replaced and counted, not fatal: inputs outside the stamps move rarely, e.g. a texture streaming in)
    const std::uint64_t differing = q.checkRowDiff + q.checkCountDiff + q.checkMiss;
    spdlog::info("native: owned trees [{}] (5 s): {} views ({} owned), tree ops looked at {}, taken {}, drawn from templates {} (of them empty records {}), composed late {} (no record {}, stamp {}, view {}, material {}, plan {}, special lighting {}, "
                 "no template {}, template refused {}, flush {}); {} instances in {} items over {} flushes; templates built {} replaced {} (not retained {}, bad ops {}: batch draws {} / draw level {} / depth bias {} / lean+retain {} / switch {} / material {} / none {}); persistent blocks derived {} reused {} refused {} (table full {}); "
                 "check frames: {} ops / {} instances compared, {} draws against templates: DIFFERING {} (rows {}, counts {}, templates {}; per-instance LOD start differs from the engine batch's first instance {}), MISS {} of {} compared; "
                 "observers shown {} instances; GPU commands: {} commands ({} records, {} indirect calls) per {} flushes = {:.1f} / flush, the engine's composed batches {:.1f} / composed view ({} views); "
                 "CPU per owned view: take {:.1f} us, side effects {:.1f} us, plan {:.1f} us, templates {:.1f} us, rows {:.1f} us, flush {:.1f} us",
                 what, q.views, q.ownedViews, q.opsSeen, q.opsTaken, q.opsDrawn, q.emptyOps, q.opsLate, q.lateNoRec, q.lateStamp, q.lateView, q.lateMaterial, q.latePlan, q.lateSpecial, q.lateNoTemplate, q.lateTemplate, q.lateFlush,
                 q.instances, q.items, q.flushes, q.built, q.replaced, q.notRetained, q.badOps, q.badBy[0], q.badBy[1], q.badBy[2], q.badBy[3], q.badBy[4], q.badBy[5], q.badBy[6], q.derived, q.reused, q.refused, q.tableFull, q.checkOps, q.checkInst, q.checkDraws, differing, q.checkRowDiff,
                 q.checkCountDiff, q.checkTplDiff, q.checkKeyDiff, q.checkMiss, q.checkMissCompared, q.observed, q.items, q.records, q.packets, q.flushes,
                 static_cast<double>(q.items) / static_cast<double>(std::max<std::uint64_t>(1, q.flushes)), static_cast<double>(q.compBatches) / static_cast<double>(std::max<std::uint64_t>(1, q.compViews)), q.compViews, static_cast<double>(q.takeNs) / nv / 1000.0, static_cast<double>(q.sideNs) / nv / 1000.0, static_cast<double>(q.planNs) / nv / 1000.0,
                 static_cast<double>(q.tplNs) / nv / 1000.0, static_cast<double>(q.rowNs) / nv / 1000.0, static_cast<double>(q.flushNs) / nv / 1000.0);
    q = {};
    if (differing != 0) {
        if (shadow) {
            // only the cascades' trees go back to the engine's path (the main view's stay)
            T.shadowTripped = true;
            treeDropShadow();
            spdlog::error("native: owned trees [cascades] turned off: {} DIFFERING on the composed frames (the plan / template is not what the composition drew)", differing);
            return;
        }
        ownedTrees_ = false;
        T.active = false;
        for (auto& v : T.taken) v.clear();
        spdlog::error("native: owned trees turned off: {} DIFFERING on the composed frames (the plan / template is not what the composition drew)", differing);
    }
}

}  // namespace rvk::native
