// F4-1 tree billboard op records (docs/research/frame-ownership.md 7.17, native.owned_billboards): see bb_records.h. Part of StaticOps (static_ops.h).
//
// bbEnter     before the op's composition: eligibility, the stamps, the record valid at entry, the signal audit, the constants sources' validation, the read log for a build
// bbComposed  after it: draw-level exclusions, the stamps that need the composition (material structure versions, the view)
// bbDraw      after each issued draw: the draw's scene view as the device made it (probe) against the one DERIVED from the record + the per-frame inputs, the constants image
//             against the composed registers c32..
// bbEnd       after issue(): build / replace the record from the composition (constants image, side effects)
// bbReplay    (owned frames) the op's draws submitted from the record, nothing composed
// The code of recEnter / recDraw / recEnd (static_ops_records.cpp, F2c-2) is the pattern; it is copied, not shared, while F2c-3 changes those functions.

#include <windows.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "d3d9/defs.h"
#include "enh/light_table.h"
#include "engine/memread.h"
#include "gpuscene/billboard_table.h"
#include "gpuscene/illumination_table.h"
#include "gpuscene/lighting_images.h"
#include "gpuscene/material_table.h"
#include "gpuscene/window_tables.h"
#include "native/owned_timers.h"
#include "native/static_ops.h"

namespace rvk::native {

namespace {

using engine::readAt;
using U8 = const std::uint8_t;
using Clock = std::chrono::steady_clock;

template <class T>
inline T get(const void* p, std::size_t off, T fallback = T{}) {
    T v = fallback;
    return readAt(p, off, v) ? v : fallback;
}
inline U8* ptr(const void* p, std::size_t off) { return get<U8*>(p, off, nullptr); }

inline std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}
inline std::uint64_t mixPtr(std::uint64_t h, const void* p) { return mix(h, reinterpret_cast<std::uintptr_t>(p)); }
inline std::uint64_t ns(Clock::time_point a, Clock::time_point b) { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()); }

constexpr std::size_t kStampSource = orec::kStampFields;  // rebuildWhy index of "billboard source" (then draw count, read log, constants source, derive refused)

}  // namespace

void StaticOps::setOwnedBillboards(bool on) {
    if (on == ownedBb_) return;
    ownedBb_ = on;
    if (on && !bb_) bb_ = std::make_unique<bbrec::Store>();
    if (!on && bb_) {
        bb_->map.clear();
        bb_->active = false;
    }
    dev_.setSceneBillboards(on);
    gpuscene::bbtable().setActive(on);
    spdlog::info("native: owned billboards {}", on ? "on" : "off");
}

bool StaticOps::bbSceneIssue() const { return ownedBb_ && g_ == &groups_[static_cast<std::size_t>(Group::Trees)] && op_ != nullptr && op_->kind == OpKind::SpeedTreeBB; }

// the stamp of a billboard op: the mesh-like inputs of recStamp for the one sub-mesh + the billboard source
bool StaticOps::bbStamp(const Op& op, orec::Stamp& st, const void*& psOut, const void*& listOut, std::uint32_t& first, std::uint32_t& count) {
    st = {};
    U8* ctx = static_cast<U8*>(op.raw.context);
    U8* ps = ptr(ctx, 0x18);
    U8* res = ptr(ps, 0x30);
    U8* entity = ptr(ctx, 0x10);
    if (ctx == nullptr || ps == nullptr || res == nullptr) return false;
    const bool solid = op.list == List::Solid;
    const std::uint32_t sub = op.raw.desc & 0xffu;
    st.res = res;
    st.entity = entity;
    if (solid) {
        U8* illumPs = ptr(entity, 0xf8);
        st.illumPs = illumPs;
        U8* illum = illumPs != nullptr ? illumPs + 0x78 : nullptr;
        U8* list = ptr(illum, 0);
        st.light = mix(mix(mixPtr(mixPtr(0, illum), list), get<std::uint8_t>(list, 0) | (get<std::uint8_t>(illum, 0xc) << 8) | (get<std::uint8_t>(illum, 0xd) << 16)),
                       get<std::uint8_t>(list, 0x34));
        std::uint64_t h = 0;
        for (std::uint32_t i = 0; i < 2; ++i) {
            U8* lm = ptr(res, 0x90 + 8 * i);
            const bool ready = lm != nullptr && get<std::int32_t>(lm, 0x38) != 0 && ptr(lm, 0x70) != nullptr;
            h = mixPtr(mixPtr(h, lm), ready ? ptr(lm, 0x70) : nullptr);
        }
        st.lightmaps = h;
    }
    {
        U8* subs = ptr(res, 0x58);
        const std::int32_t n = get<std::int32_t>(res, 0x60);
        std::uint64_t h = mix(mixPtr(mixPtr(0, subs), ptr(res, 0x88)), static_cast<std::uint64_t>(n) | (static_cast<std::uint64_t>(get<std::int32_t>(res, 0x38)) << 32));
        if (sub < static_cast<std::uint32_t>(n) && subs != nullptr) {
            U8* s = subs + 0x48 * static_cast<std::size_t>(sub);
            U8* m = ptr(s, 0x38);
            U8* sh = ptr(m, 0x70);
            h = mix(h, get<std::uint32_t>(s, 0x28) | (static_cast<std::uint64_t>(get<std::uint32_t>(s, 0x2c)) << 32));
            h = mix(h, get<std::uint32_t>(s, 0x30) | (static_cast<std::uint64_t>(get<std::uint32_t>(s, 0x34)) << 32));
            h = mixPtr(mixPtr(mixPtr(h, m), sh), ptr(sh, 0));
            h = mix(h, static_cast<std::uint64_t>(get<std::int32_t>(m, 0x38)) | (static_cast<std::uint64_t>(get<std::int32_t>(sh, 0x50)) << 32));
            h = mix(h, get<std::uint8_t>(sh, 0x58) | (get<std::uint8_t>(sh, 0x79) << 8) | (get<std::uint8_t>(sh, 0x330) << 16) | (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x420)) << 24) |
                           (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x2d8)) << 32) | (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x78)) << 40) |
                           (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x2b8)) << 48) | (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x2a8)) << 56));
            h = mixPtr(mix(h, static_cast<std::uint32_t>(get<std::int32_t>(sh, 0x68))), ptr(sh, 0x220));
        }
        st.geom = h;
    }
    st.variantEpoch = dev_.variantEpoch();
    st.registryGen = enh::fullLitSkipsReads() ? enh::LightTable::registryGeneration() : 0;
    // the billboard source: UploadTexCoords' inputs (c32..c36 = the fixed table, c37 + j = the PS's list entries first .. first + count)
    psOut = ps;
    listOut = ptr(ps, 0x40);
    first = count = 0;
    if (get<std::int32_t>(res, 0x60) != 0) {
        U8* ranges = ptr(ps, 0x58);
        first = get<std::uint32_t>(ranges, 0xc * static_cast<std::size_t>(sub) + 4);
        count = get<std::uint32_t>(ranges, 0xc * static_cast<std::size_t>(sub) + 8);
    }
    return true;
}

void StaticOps::bbEnter(const Op& op) {
    if (!ownedBb_ || !bb_) return;
    bbrec::Store& B = *bb_;
    B.readLog = {};  // (a fresh log per op: a record rebuilt after the composition must not take an earlier op's reads; see recEnter)
    B.active = B.building = B.auditBad = B.diffInOp = B.refused = false;
    B.cur = nullptr;
    B.postDone = false;
    B.rebuildWhy = -1;
    B.probes.clear();
    B.pkeys.clear();
    B.mats.clear();
    recNoRetain_ = false;
    if (viewKind_ != ViewKind::Main || g_ != &groups_[static_cast<std::size_t>(Group::Trees)] || op.kind != OpKind::SpeedTreeBB || inNonOp_ ||
        (op.list != List::ZPass && op.list != List::Solid)) {
        return;
    }
    const auto t0 = tnow();
    bbrec::Stats& w = B.window;
    ++w.opsAll;
    U8* ctx = static_cast<U8*>(op.raw.context);
    U8* ps = ptr(ctx, 0x18);
    U8* res = ptr(ps, 0x30);
    B.key = {ctx, op.raw.desc, static_cast<std::uint8_t>(op.list), 0};
    bbrec::Excl ex = bbrec::Excl::Count;
    const auto reason = [&](bbrec::Excl e) {
        if (ex == bbrec::Excl::Count) ex = e;
    };
    if (ctx == nullptr || ps == nullptr || res == nullptr) {
        reason(bbrec::Excl::NotComposed);
    } else {
        const std::uint32_t sub = op.raw.desc & 0xffu;
        const std::int32_t subs = get<std::int32_t>(res, 0x60);
        if (sub == 0xffu || static_cast<std::int32_t>(sub) >= subs) {
            reason(bbrec::Excl::NoSub);
        } else {
            U8* shader = ptr(ptr(ptr(res, 0x58) + 0x48 * static_cast<std::size_t>(sub), 0x38), 0x70);
            if (shader == nullptr) reason(bbrec::Excl::NoShader);
            else if (!shaderClassOk(shader)) reason(bbrec::Excl::ShaderClass);
        }
    }
    if (ex != bbrec::Excl::Count) {
        ++w.excluded[static_cast<std::size_t>(ex)];
        B.map.erase(B.key);
        w.stampNs += ns(t0, tnow());
        return;
    }
    orec::Stamp st;
    const void *psv = nullptr, *listv = nullptr;
    std::uint32_t first = 0, count = 0;
    if (!bbStamp(op, st, psv, listv, first, count)) {
        ++w.excluded[static_cast<std::size_t>(bbrec::Excl::NotComposed)];
        return;
    }
    B.stamp = st;
    B.ps = psv;
    B.list = listv;
    B.first = first;
    B.count = count;
    B.active = true;
    B.op = &op;
    B.switchBefore = gpuscene::materialtable().stats().switchNotes;
    const auto it = B.map.find(B.key);
    if (it != B.map.end()) {
        bbrec::Record& rec = *it->second;
        orec::Stamp a = rec.stamp;
        a.view = a.materials = 0;
        int d = orec::firstDifference(a, st);
        if (d < 0 && (rec.ps != psv || rec.list != listv || rec.first != first || rec.count != count)) d = static_cast<int>(kStampSource);
        if (d < 0 && rec.nConst != 0) {
            // the constants' sources: the fixed table, the PS's list entries, the range entry (the read log of UploadTexCoords)
            ++w.constChecks;
            if (!ret::validate(rec.srcReads.data(), rec.srcReads.size())) {
                ++w.constStale;
                d = static_cast<int>(kStampSource + 3);
            }
        }
        if (d < 0) B.cur = &rec;
        else B.rebuildWhy = d;
    }
    w.stampNs += ns(t0, tnow());
    if (B.cur != nullptr) bbAudit(op);
    B.building = B.cur == nullptr || B.auditBad;
    if (B.building) {
        B.readLog = {};
        ret::gLog = &B.readLog;
    }
}

void StaticOps::bbAudit(const Op& op) {
    bbrec::Store& B = *bb_;
    const bbrec::Record& rec = *B.cur;
    if (rec.reads.empty()) return;
    const auto t0 = tnow();
    bbrec::Stats& w = B.window;
    ++w.auditChecked;
    if (!ret::validate(rec.reads.data(), rec.reads.size())) {
        B.auditBad = true;
        std::size_t cls = orec::kAuditCount - 1;
        const ret::ReadRec* bad = nullptr;
        for (const ret::ReadRec& r : rec.reads) {
            if (ret::validate(&r, 1)) continue;
            bad = &r;
            for (std::size_t a = 0; a < 8; ++a) {
                const auto base = reinterpret_cast<std::uintptr_t>(rec.anchors[a]);
                const auto at = reinterpret_cast<std::uintptr_t>(r.addr);
                if (rec.anchors[a] != nullptr && at >= base && at < base + 0x600) {
                    cls = a;
                    break;
                }
            }
            break;
        }
        B.auditCls = cls;
        if (B.auditLogged < 16 && bad != nullptr) {
            ++B.auditLogged;
            const auto base = reinterpret_cast<std::uintptr_t>(rec.anchors[std::min<std::size_t>(cls, 7)]);
            spdlog::info("native: owned billboards: audit: the read log says changed while the stamps hold ({}): {} read of {} bytes at {} (offset {:#x} into the {}), was {:#x}", describe(op),
                         bad->kind == 1 ? "block" : "word", bad->size, static_cast<const void*>(bad->addr), reinterpret_cast<std::uintptr_t>(bad->addr) - base, orec::kAuditNames[cls], bad->value);
        }
    }
    w.auditNs += ns(t0, tnow());
}

void StaticOps::bbComposed(const Op& op, Skip s) {
    if (ret::gLog != nullptr && bb_ && ret::gLog == &bb_->readLog) ret::gLog = nullptr;
    if (!bb_ || !bb_->active) return;
    bbrec::Store& B = *bb_;
    bbrec::Stats& w = B.window;
    const auto t0 = tnow();
    const auto exclude = [&](bbrec::Excl e) {
        ++w.excluded[static_cast<std::size_t>(e)];
        B.map.erase(B.key);
        B.active = false;
        B.cur = nullptr;
    };
    if (s != Skip::None || drawCount_ == 0) {
        exclude(bbrec::Excl::NotComposed);
        return;
    }
    const bool solid = op.list == List::Solid;
    const bool lightImages = gpuscene::lightingimages().active();
    for (std::size_t k = 0; k < drawCount_; ++k) {
        const sc::DrawDesc& d = *draws_[k];
        if (d.inheritMask != 0 || !d.vertexBytes.empty() || d.cpuStream != 0 || !d.indexed) {
            exclude(bbrec::Excl::DrawLevel);
            return;
        }
        if (solid && lightImages && d.lightEntry == gpuscene::kNoEntry) {
            exclude(bbrec::Excl::NotLit);
            return;
        }
    }
    if (recNoRetain_) {
        exclude(bbrec::Excl::NoRetain);
        return;
    }
    if (B.mats.size() != drawCount_) {
        exclude(bbrec::Excl::NotComposed);
        return;
    }
    ++w.ops;
    w.draws += drawCount_;
    B.stamp.view = bbViewStamp();
    {
        std::uint64_t h = 0;
        for (std::size_t k = 0; k < drawCount_; ++k) {
            const std::uint32_t slot = draws_[k]->matSlot;
            h = mix(mix(h, slot), gpuscene::materialtable().structVersion(slot));
        }
        B.stamp.materials = h;
    }
    if (B.cur != nullptr) {
        int why = -1;
        if (B.cur->stamp.view != B.stamp.view) why = 12;
        else if (B.cur->stamp.materials != B.stamp.materials) why = 13;
        else if (B.cur->draws.size() != drawCount_) why = static_cast<int>(kStampSource + 1);
        if (why >= 0) {
            B.rebuildWhy = why;
            B.cur = nullptr;
            B.building = true;
        }
    }
    if (B.auditBad) {
        if (B.cur == nullptr) {
            ++w.auditCaught;
        } else {
            ++w.auditDisagree;
            ++w.auditBy[B.auditCls];
        }
    }
    B.probes.assign(drawCount_, {});
    B.pkeys.assign(drawCount_, 0);
    w.stampNs += ns(t0, tnow());
}

namespace {
// the view windows (0): the registers the draw composed (liveMask) are the ones that count; the others are the register file's carry-over on the composed side
bool sameViewWin(const d3d9::OwnedWindows::Win& a, const d3d9::OwnedWindows::Win& b, std::uint64_t liveMask, bool& dup) {
    if (a.kind != b.kind || a.covered != b.covered) return false;
    if (a.base.bits == b.base.bits) return true;
    if (a.image == nullptr || b.image == nullptr) return false;
    for (std::uint32_t q = 0; q < a.covered && q < 64; ++q) {
        if (((liveMask >> q) & 1u) != 0 && std::memcmp(a.image + 4 * q, b.image + 4 * q, 16) != 0) return false;
    }
    dup = true;
    return true;
}
bool sameWin(const d3d9::OwnedWindows::Win& a, const d3d9::OwnedWindows::Win& b, bool& dup) {
    if (a.kind != b.kind || a.covered != b.covered) return false;
    if (a.base.bits == b.base.bits) return true;
    if (a.kind == d3d9::OwnedWindows::Kind::View && a.image != nullptr && b.image != nullptr && std::memcmp(a.image, b.image, a.covered * 16u) == 0) {
        dup = true;
        return true;
    }
    return false;
}
}  // namespace

// the derivation inputs of a record's draw (shared by the compare and the owned replay)
void StaticOps::bbDeriveIn(const orec::DrawRec& r, d3d9::Device::DeriveIn& in) {
    for (std::size_t i = 0; i < 4; ++i) {
        in.streams[i] = const_cast<d3d9::Buffer*>(static_cast<const d3d9::Buffer*>(r.streams[i]));
        in.strides[i] = r.strides[i];
    }
    in.indices = const_cast<d3d9::Buffer*>(static_cast<const d3d9::Buffer*>(r.indices));
    for (std::uint32_t m = r.texMask; m != 0; m &= m - 1) {
        const std::uint32_t st = static_cast<std::uint32_t>(std::countr_zero(m));
        in.tex[st] = const_cast<d3d9::Texture*>(static_cast<const d3d9::Texture*>(r.tex[st]));
        for (std::uint32_t i = 0; i < 7; ++i) in.ss[st][i] = r.ss[st][i];
    }
    in.alphaFunc = r.alphaTestState != 0 ? r.alphaFunc : 8u;
    in.alphaRef = static_cast<float>(r.alphaRef & 0xffu);
    in.psC5[0] = mirror_.adminPs[5][0];
    in.psC5[1] = mirror_.adminPs[5][1];
    in.psC5[2] = mirror_.adminPs[5][2];
    in.psC5[3] = r.maskSet ? r.maskValue : 0.0f;
    if (r.cd->v.needsAmbient() && r.illumSlot != gpuscene::kNoSlot) {
        gpuscene::ComposeParams p;
        p.tableLit = r.lightTableLit;
        p.holdsAll = r.lightHoldsAll;
        p.n = r.lightN;
        p.firstLight = r.lightFirst;
        float L[24][4]{};
        const auto& amb = r.cd->ps->semantics[d3d9::Semantic::AmbientLight];
        if (gpuscene::illumtable().compose(r.illumSlot, p, L) && amb.present() && amb.count >= 7 && amb.reg >= 14 && amb.reg + 7 <= 14 + 24) {
            for (std::int32_t i = 0; i < 7 && !in.ambientLit; ++i) {
                const float* c = L[amb.reg - 14 + i];
                in.ambientLit = c[0] != 0.0f || c[1] != 0.0f || c[2] != 0.0f || c[3] != 0.0f;
            }
        }
    }
}

void StaticOps::bbDraw(std::size_t k, const sc::DrawDesc& d, std::uint64_t pkey) {
    if (!bb_ || !bb_->active || k >= bb_->probes.size()) return;
    bbrec::Store& B = *bb_;
    B.probes[k] = B.probe;
    B.pkeys[k] = pkey;
    if (B.cur == nullptr || k >= B.cur->draws.size()) return;
    const auto t0 = tnow();
    bbrec::Stats& w = B.window;
    const bbrec::Record& rec = *B.cur;
    const orec::DrawRec& r = rec.draws[k];
    d3d9::Device::DeriveIn in;
    bbDeriveIn(r, in);
    d3d9::Device::SceneView dv;
    const int why = dev_.deriveScene(*r.cd, in, dv);
    ++w.derives;
    ++w.refused[static_cast<std::size_t>(why)];
    if (why != 0) {
        if (why == 1 || why == 4 || why == 6 || why == 7) B.refused = true;
        w.deriveNs += ns(t0, tnow());
        return;
    }
    ++w.derived;
    const d3d9::Device::SceneView& pv = B.probes[k];
    std::array<bool, bbrec::kFieldCount> diff{};
    std::string detail;
    const auto fail = [&](bbrec::Field f, const std::string& what) {
        diff[static_cast<std::size_t>(f)] = true;
        if (detail.empty()) detail = std::format("{}: {}", bbrec::kFieldNames[static_cast<std::size_t>(f)], what);
    };
    const std::string cdv = std::format("[cd: gate {} staticOk {} wanted {} giDraw {} drawn {} scenePending {}, ambient from the record {}]", r.cd->v.gate, r.cd->v.staticOk, r.cd->v.wanted,
                                        r.cd->v.giDraw, r.cd->v.drawn, r.cd->v.scenePending, in.ambientLit);
    if (dv.scene != pv.valid) {
        const auto& ss = dev_.sceneStats();
        fail(bbrec::Field::Scene, std::format("derived {} vs composed {} {} (variant kind {}, failed {}, pending {}, pipeline pending {}, no room {}, ring full {}, null address {}, not batchable {})",
                                              dv.scene, pv.valid, cdv, ss.fbVariantKind, ss.fbVariantFailed, ss.fbVariantPending, ss.fbPipelinePending, ss.fbNoRoom, ss.fbRingFull,
                                              ss.fbNullAddress, ss.fbNotBatchable));
    } else if (pv.valid) {
        ++w.compared;
        if (dv.pipeline != pv.pipeline) fail(bbrec::Field::Pipeline, std::format("{} vs {} {}", static_cast<std::uint32_t>(dv.pipeline), static_cast<std::uint32_t>(pv.pipeline), cdv));
        if (dv.viHash != pv.viHash) fail(bbrec::Field::VertexInput, "vertex input");
        if (dv.dynHash != pv.dynHash) fail(bbrec::Field::VertexInput, "dynamic state");
        if (dv.first != pv.first || dv.count != pv.count || dv.vertexOffset != pv.vertexOffset || dv.indexBuffer != pv.indexBuffer || dv.indexOffset != pv.indexOffset || dv.index32 != pv.index32) {
            fail(bbrec::Field::Geometry, std::format("first {} / {}, count {} / {}, vertex offset {} / {}", dv.first, pv.first, dv.count, pv.count, dv.vertexOffset, pv.vertexOffset));
        }
        if (dv.flags != pv.flags) fail(bbrec::Field::Flags, std::format("{:#x} vs {:#x}", dv.flags, pv.flags));
        const shader::abi::DrawBlock& a = dv.block;
        const shader::abi::DrawBlock& b = pv.block;
        for (std::uint32_t i = 0; i < shader::abi::kSamplerSlots; ++i) {
            if (a.tex[i] != b.tex[i]) fail(bbrec::Field::BlockTex, std::format("slot {}: {} vs {}", i, a.tex[i], b.tex[i]));
            if (a.smp[i] != b.smp[i]) fail(bbrec::Field::BlockSmp, std::format("slot {}: {} vs {}", i, a.smp[i], b.smp[i]));
        }
        if (std::memcmp(a.posFix, b.posFix, sizeof(a.posFix)) != 0) {
            fail(bbrec::Field::BlockPos, std::format("({}, {}, {}, {}) vs ({}, {}, {}, {})", a.posFix[0], a.posFix[1], a.posFix[2], a.posFix[3], b.posFix[0], b.posFix[1], b.posFix[2], b.posFix[3]));
        }
        if (a.alphaFunc != b.alphaFunc) fail(bbrec::Field::BlockAlpha, std::format("func {} vs {}", a.alphaFunc, b.alphaFunc));
        else if (a.alphaRef != b.alphaRef) {
            if (a.alphaFunc != 8u) fail(bbrec::Field::BlockAlpha, std::format("ref {} vs {}", a.alphaRef, b.alphaRef));
            else ++w.alphaRefCarry;
        }
        if (a.pad[0] != b.pad[0] || a.pad[1] != b.pad[1]) fail(bbrec::Field::BlockPad, std::format("({}, {}) vs ({}, {})", a.pad[0], a.pad[1], b.pad[0], b.pad[1]));
        const bool c5Live = ((r.psSet[0] >> 5) & 1u) != 0;
        if (!c5Live) {
            if (std::memcmp(a.psC5, b.psC5, 16) != 0) ++w.c5wCarry;
        } else if (std::memcmp(a.psC5, b.psC5, 12) != 0) {
            fail(bbrec::Field::BlockC5, std::format("({}, {}, {}) vs ({}, {}, {})", a.psC5[0], a.psC5[1], a.psC5[2], b.psC5[0], b.psC5[1], b.psC5[2]));
        } else if (a.psC5[3] != b.psC5[3]) {
            if (r.maskSet) fail(bbrec::Field::BlockC5, std::format("w {} vs {}", a.psC5[3], b.psC5[3]));
            else ++w.c5wCarry;
        }
    }
    {
        const d3d9::draws::Tag& tag = dev_.drawTag();
        if (tag.subMesh != r.tagSubMesh || tag.material != r.tagMaterial || tag.meshResource != r.tagMeshResource || static_cast<std::uint8_t>(tag.water) != r.tagWater ||
            tag.materialName != r.tagMaterialName) {
            fail(bbrec::Field::Tag, "sub-mesh fields");
        }
    }
    // the constants image against the composed registers (those the draw composed: the shader's live ones)
    for (std::uint32_t i = 0; i < rec.nConst; ++i) {
        const std::uint32_t reg = 32 + i;
        if (reg >= sc::kVsRegs || ((d.vsSet[reg >> 6] >> (reg & 63)) & 1u) == 0) continue;
        ++w.constRegsCompared;
        if (std::memcmp(rec.consts.data() + 4 * static_cast<std::size_t>(i), d.vsF[reg], 16) != 0) {
            fail(bbrec::Field::Constants, std::format("c{}: ({}, {}, {}, {}) vs composed ({}, {}, {}, {})", reg, rec.consts[4 * i], rec.consts[4 * i + 1], rec.consts[4 * i + 2], rec.consts[4 * i + 3],
                                                      d.vsF[reg][0], d.vsF[reg][1], d.vsF[reg][2], d.vsF[reg][3]));
            break;
        }
    }
    // the windows by table slot (the offer the draw's override carries)
    if (ownedWindows_ && gpuscene::windowtables().active()) {
        d3d9::OwnedWindows dw;
        bbWindowsFor(rec, k, dw);  // (as an owned draw builds them; VS windows 1 / 2 are not offered to a composed billboard draw: their registers are compared below)
        for (std::uint32_t st = 0; st < 2; ++st) {
            for (std::uint32_t wi = 0; wi < shader::abi::kConstWindows; ++wi) {
                if (st == 0 && (wi == 1 || wi == 2)) continue;
                bool dup = false;
                const std::uint64_t live = wi == 0 ? (st == 0 ? r.vsSet[0] : r.psSet[0]) : ~0ull;
                const bool same = wi == 0 ? sameViewWin(dw.w[st][wi], ownedWin_.w[st][wi], live, dup) : sameWin(dw.w[st][wi], ownedWin_.w[st][wi], dup);
                if (dup) ++w.viewDup;
                if (!same) {
                    std::string regs;
                    if (dw.w[st][wi].image != nullptr && ownedWin_.w[st][wi].image != nullptr) {
                        for (std::uint32_t q = 0; q < dw.w[st][wi].covered && regs.empty(); ++q) {
                            if (((live >> q) & 1u) == 0) continue;
                            if (std::memcmp(dw.w[st][wi].image + 4 * q, ownedWin_.w[st][wi].image + 4 * q, 16) != 0) {
                                regs = std::format(" (first differing register +{}: ({}, {}, {}, {}) vs ({}, {}, {}, {}))", q, dw.w[st][wi].image[4 * q], dw.w[st][wi].image[4 * q + 1],
                                                   dw.w[st][wi].image[4 * q + 2], dw.w[st][wi].image[4 * q + 3], ownedWin_.w[st][wi].image[4 * q], ownedWin_.w[st][wi].image[4 * q + 1],
                                                   ownedWin_.w[st][wi].image[4 * q + 2], ownedWin_.w[st][wi].image[4 * q + 3]);
                            }
                        }
                    }
                    fail(bbrec::Field::Windows, std::format("{} window {}: kind {} base {:#x} covered {} vs composed kind {} base {:#x} covered {}", st == 0 ? "VS" : "PS", wi,
                                                            static_cast<int>(dw.w[st][wi].kind), dw.w[st][wi].base.bits, dw.w[st][wi].covered, static_cast<int>(ownedWin_.w[st][wi].kind),
                                                            ownedWin_.w[st][wi].base.bits, ownedWin_.w[st][wi].covered) + regs);
                }
            }
        }
        // VS window 1: the view block's c12..c30 (the admin pool's values) against the registers the draw composed
        if (dw.w[0][1].image != nullptr) {
            for (std::uint32_t q = 0; q < 19; ++q) {
                const std::uint32_t reg = 12 + q;
                if (((d.vsSet[0] >> reg) & 1u) == 0) continue;
                if (std::memcmp(dw.w[0][1].image + 4 * q, d.vsF[reg], 16) != 0) {
                    fail(bbrec::Field::Windows, std::format("VS window 1: c{} ({}, {}, {}, {}) vs composed ({}, {}, {}, {})", reg, dw.w[0][1].image[4 * q], dw.w[0][1].image[4 * q + 1],
                                                            dw.w[0][1].image[4 * q + 2], dw.w[0][1].image[4 * q + 3], d.vsF[reg][0], d.vsF[reg][1], d.vsF[reg][2], d.vsF[reg][3]));
                    break;
                }
            }
        }
        // VS window 2: c31 is zero in the image; a draw that reads it must have composed zero
        if (dw.w[0][2].image != nullptr && ((d.vsSet[0] >> 31) & 1u) != 0 && std::memcmp(dw.w[0][2].image, d.vsF[31], 16) != 0) {
            fail(bbrec::Field::Constants, "c31 is live and differs from the image's zero");
        }
    }
    bool any = false;
    for (std::size_t f = 0; f < bbrec::kFieldCount; ++f) {
        if (diff[f]) {
            ++w.byField[f];
            any = true;
        }
    }
    if (any) {
        ++w.differing;
        B.diffInOp = true;
        std::size_t firstField = 0;
        while (firstField < bbrec::kFieldCount && !diff[firstField]) ++firstField;
        if (B.loggedBy[firstField] < 6) {
            ++B.loggedBy[firstField];
            const d3d9::draws::Tag& tag = dev_.drawTag();
            spdlog::info("native: owned billboards: DIFFERING in {} draw {} ({}), material '{}': {}", describe(*op_), k, r.zPass ? "z" : "solid",
                         tag.materialName != nullptr ? tag.materialName : (d.tagMaterialName != nullptr ? d.tagMaterialName : ""), detail);
        }
    } else {
        ++w.clean;
    }
    w.deriveNs += ns(t0, tnow());
}

void StaticOps::bbEnd(const Op& op) {
    if (!bb_ || !bb_->active) return;
    bbrec::Store& B = *bb_;
    bbrec::Stats& w = B.window;
    if (B.cur != nullptr && !B.auditBad && !B.refused) {
        ++w.validAtEntry;
        return;
    }
    const auto t0 = tnow();
    if (!leanCollect_ || leanBlock_ != 0) {
        // (the side effects of the engine's slot are collected on lean frames only: no record is made from a frame that runs the engine's code; an existing one stays for the compare)
        if (B.cur == nullptr) {
            ++w.excluded[static_cast<std::size_t>(bbrec::Excl::NotLean)];
            B.map.erase(B.key);
        }
        B.active = false;
        return;
    }
    auto rec = std::make_unique<bbrec::Record>();
    rec->key = B.key;
    rec->stamp = B.stamp;
    rec->ps = B.ps;
    rec->list = B.list;
    rec->first = B.first;
    rec->count = B.count;
    rec->draws.reserve(drawCount_);
    for (std::size_t k = 0; k < drawCount_; ++k) {
        const sc::DrawDesc& d = *draws_[k];
        const std::uint64_t pkey = k < B.pkeys.size() ? B.pkeys[k] : 0;
        const auto pe = pkey != 0 ? packets_.find(pkey) : packets_.end();
        if (pe == packets_.end() || !pe->second.cd.valid) {
            ++w.excluded[static_cast<std::size_t>(bbrec::Excl::NotRetained)];
            B.map.erase(B.key);
            B.active = false;
            w.buildNs += ns(t0, tnow());
            return;
        }
        orec::DrawRec r;
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
        r.vs1Offered = false;
        r.tagSubMesh = d.tagSubMesh;
        r.tagMaterial = d.tagMaterial;
        r.tagMeshResource = d.tagMeshResource;
        r.tagWater = d.tagWater;
        r.tagMaterialName = d.tagMaterialName;
        r.zPass = d.zPass;
        r.passFlags = d.passFlags;
        r.scene = k < B.probes.size() && B.probes[k].valid;
        rec->matSlots.push_back(d.matSlot);
        rec->draws.push_back(std::move(r));
    }
    U8* ctx = static_cast<U8*>(op.raw.context);
    U8* ps = ptr(ctx, 0x18);
    U8* res = ptr(ps, 0x30);
    const std::uint32_t sub = op.raw.desc & 0xffu;
    {
        U8* subs0 = ptr(res, 0x58);
        rec->shader = ptr(ptr(subs0 != nullptr ? subs0 + 0x48 * static_cast<std::size_t>(sub) : nullptr, 0x38), 0x70);
        rec->depthBias = get<std::uint8_t>(rec->shader, 0x78) != 0;
        rec->mats = B.mats;
        for (const bbrec::MatArgs& m : rec->mats) {
            if (m.owner != rec->shader) {
                ++w.excluded[static_cast<std::size_t>(bbrec::Excl::OtherOwner)];
                B.map.erase(B.key);
                B.active = false;
                w.buildNs += ns(t0, tnow());
                return;
            }
        }
    }
    {
        U8* entity = ptr(ctx, 0x10);
        U8* illumPs = ptr(entity, 0xf8);
        U8* illum = illumPs != nullptr ? illumPs + 0x78 : nullptr;
        U8* subs = ptr(res, 0x58);
        U8* s0 = subs != nullptr ? subs + 0x48 * static_cast<std::size_t>(sub) : nullptr;
        U8* mat = ptr(s0, 0x38);
        rec->anchors[0] = ctx;
        rec->anchors[1] = res;
        rec->anchors[2] = entity;
        rec->anchors[3] = illum;
        rec->anchors[4] = ptr(illum, 0);
        rec->anchors[5] = subs;
        rec->anchors[6] = mat;
        rec->anchors[7] = ptr(mat, 0x70);
    }
    // the constants image and its sources (UploadTexCoords: the fixed table, the range entry, the PS's list entries)
    if (get<std::int32_t>(res, 0x60) != 0 && bbTexCoords_ != nullptr) {
        bool ok = true;
        float fixed[5][4];
        std::uint8_t rangeBytes[12];
        U8* ranges = ptr(ps, 0x58);
        const auto src = [&](const void* base, std::size_t off, const void* data, std::size_t n) {
            rec->srcReads.push_back({static_cast<U8*>(base) + off, ret::hashBytes(data, n), static_cast<std::uint32_t>(n), 1});
        };
        ok = ok && engine::safeRead(bbTexCoords_, fixed, sizeof(fixed)) && ranges != nullptr && engine::safeRead(ranges + 0xc * static_cast<std::size_t>(sub), rangeBytes, sizeof(rangeBytes));
        if (ok) {
            src(bbTexCoords_, 0, fixed, sizeof(fixed));
            src(ranges, 0xc * static_cast<std::size_t>(sub), rangeBytes, sizeof(rangeBytes));
            rec->consts.assign(static_cast<std::size_t>(5 + std::min<std::uint32_t>(B.count, 195)) * 4, 0.0f);
            std::memcpy(rec->consts.data(), fixed, sizeof(fixed));
            ok = 5 + B.count <= 200;
            for (std::uint32_t j = 0; ok && j < B.count; ++j) {
                float v[4];
                U8* list = ptr(ps, 0x40);
                ok = engine::safeRead(list + 16 * static_cast<std::size_t>(static_cast<std::int32_t>(B.first + j)), v, 16);
                if (ok) {
                    std::memcpy(rec->consts.data() + 4 * (5 + static_cast<std::size_t>(j)), v, 16);
                    src(list, 16 * static_cast<std::size_t>(static_cast<std::int32_t>(B.first + j)), v, 16);
                }
            }
        }
        if (!ok) {
            ++w.excluded[static_cast<std::size_t>(bbrec::Excl::NotComposed)];
            B.map.erase(B.key);
            B.active = false;
            w.buildNs += ns(t0, tnow());
            return;
        }
        rec->nConst = static_cast<std::uint32_t>(rec->consts.size() / 4);
        ret::finalize(rec->srcReads);
        rec->constSlot = gpuscene::bbtable().intern(rec->consts.data(), rec->nConst);
    }
    // side effects of the engine's slot (what leanCommit runs): collected during this composition
    rec->touchFirst = res;
    rec->touches = leanTouches_;
    rec->lightmaps = leanLightmaps_;
    for (const LeanStream3& x : leanStream3_) rec->stream3.emplace_back(x.primitive, x.on);
    rec->builtFrame = dev_.stats().frames;
    if (B.loggedList[op.list == List::Solid ? 1 : 0] < 8) {
        ++B.loggedList[op.list == List::Solid ? 1 : 0];
        // diagnostics: which windows the draws' shaders read (the composed register sets) and whether the composed c12..c30 are the admin pool's
        std::string draws;
        for (std::size_t k = 0; k < drawCount_; ++k) {
            const sc::DrawDesc& d = *draws_[k];
            bool eq = true;
            for (std::uint32_t r = 12; r < 31; ++r) {
                if ((d.vsSet[r >> 6] >> (r & 63) & 1u) != 0 && std::memcmp(d.vsF[r], mirror_.adminVs[r], 16) != 0) eq = false;
            }
            draws += std::format(" [{} vs {:#x} {:#x} {:#x} {:#x} ps {:#x} {:#x} {:#x} {:#x}; c12..c30 equal to the admin pool {}; light entry {}]", d.zPass ? "z" : "solid", d.vsSet[0], d.vsSet[1], d.vsSet[2], d.vsSet[3],
                                 d.psSet[0], d.psSet[1], d.psSet[2], d.psSet[3], eq, d.lightEntry != gpuscene::kNoEntry);
        }
        spdlog::info("native: owned billboards: record of {}: {} draws, {} constants (first {} count {}), touches {}, lightmaps {}, stream3 {}{}", describe(op), drawCount_, rec->nConst, B.first, B.count,
                     rec->touches.size(), rec->lightmaps.size(), rec->stream3.size(), draws);
    }
    if (ret::gLog == nullptr && !B.readLog.v.empty()) {
        ret::finalize(B.readLog.v);
        if (!B.readLog.failed) rec->reads = std::move(B.readLog.v);
    }
    B.readLog = {};
    const bool existed = B.map.find(B.key) != B.map.end();
    if (existed) {
        ++w.rebuilt;
        ++w.rebuiltWhy[B.rebuildWhy >= 0 ? static_cast<std::size_t>(B.rebuildWhy) : B.auditBad ? kStampSource + 2 : kStampSource + 4];
    } else {
        ++w.built;
    }
    if (B.map.size() > 4096) B.map.clear();
    B.map[B.key] = std::move(rec);
    ++w.builds;
    w.buildNs += ns(t0, tnow());
}

// ---- owned frames (B) ----------------------------------------------------------------------------------------------------------

namespace {
}

// writeLightingRegisters' hand-over of a table-lit solid draw's illumination to the light table (the list's own lights; the static lights while the registry holds none)
void StaticOps::bbNoteLights(const std::uint8_t* illum) {
    const ret::NoLog noLog;
    U8* list = ptr(illum, 0);
    if (list == nullptr) return;
    noteListLights(list);
    const std::uint32_t ns = std::min<std::uint32_t>(get<std::uint8_t>(illum, 0x0d), 4u);
    if (ns != 0 && enh::LightTable::wantStaticNotes() && std::find(tableLitSeen_.begin(), tableLitSeen_.end(), illum) == tableLitSeen_.end() && tableLitSeen_.size() < 400) {
        tableLitSeen_.push_back(illum);
        for (std::uint32_t i = 0; i < ns; ++i) {
            float e[8]{};
            engine::safeRead(illum + 0x10 + 0x40 * i, &e[0], 16);
            engine::safeRead(illum + 0x10 + 0x40 * i + 0x10, &e[4], 16);
            enh::LightTable::noteDrawnLight(e, e + 4);
        }
    }
}

bool StaticOps::bbCheckFrame() {
    // the check period is native.owned_check (shared with F2c-3: every N-th frame composes and compares, 0 = never);
    // developer override: RVK_BB_CHECK=<n> (1 composes every frame = the scene-draw path without owned replays, 0 = owned only)
    static const long long override = [] {
        char e[16] = {};
        const DWORD n = GetEnvironmentVariableA("RVK_BB_CHECK", e, sizeof(e));  // (-Environment sets the Win32 environment, not the CRT's)
        return n != 0 && n < sizeof(e) ? static_cast<long long>(std::strtoull(e, nullptr, 10)) : -1ll;
    }();
    const std::uint64_t every = override >= 0 ? static_cast<std::uint64_t>(override) : static_cast<std::uint64_t>(std::max(0, ownedCheck_));
    return every != 0 && dev_.stats().frames % every == 0;
}

// the composition's checkMaterial call of a billboard op (collected for the record: an owned op repeats the poll)
void StaticOps::bbNoteMat(const sc::DrawDesc& d, const std::uint8_t* owner, const void* material, bool zPass, std::uint16_t psMask, std::int32_t blendMode, std::int32_t maskRef) {
    if (!bb_ || !bb_->active) return;
    bbrec::MatArgs m;
    m.owner = owner;
    m.material = material;
    m.zPass = zPass;
    m.psMask = psMask;
    m.blendMode = blendMode;
    m.maskRef = maskRef;
    m.inheritMask = d.inheritMask;
    // (checkMaterial reads the stages of the material's pool slots)
    const std::uint32_t base = get<std::uint8_t>(owner, 0x219);
    const std::uint32_t slots = std::min<std::uint32_t>(poolsOf(owner)->slots, sc::kStages);
    for (std::uint32_t k = 0; k < slots && base + k < sc::kStages; ++k) {
        const std::uint32_t st = base + k;
        m.smpMask |= static_cast<std::uint16_t>(1u << st);
        m.tex[st] = d.samplers[st].texture;
        for (std::uint32_t i = 0; i < 7; ++i) m.ss[st][i] = d.samplers[st].ss[i];
    }
    bb_->mats.push_back(m);
}

// the view's inputs to a composed pass (the admin's version / type / z write / render mode, the depth-bias order, fog): what recViewStamp holds without the composition's global alpha
std::uint64_t StaticOps::bbViewStamp() {
    U8* admin = view_.admin;
    std::uint64_t h = mixPtr(0, admin);
    h = mix(h, get<std::uint8_t>(admin, 0x313) | (get<std::uint8_t>(admin, 0x314) << 8) | (view_.zColorWrite ? 0x10000u : 0u) | (view_.fog ? 0x20000u : 0u));
    h = mix(h, static_cast<std::uint64_t>(get<std::uint32_t>(admin, 0x340)) | (static_cast<std::uint64_t>(get<std::int32_t>(admin, 0x170)) << 32));
    h = mix(h, static_cast<std::uint64_t>(get<std::uint32_t>(admin, 0x31c)) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(view_.dbOrder)) << 32));
    return h;
}

// the windows of a record's draw as the tables serve them: Material windows (VS 3, PS 2), the lighting image (PS 3), the view block written from the view's mirror (VS 0 / 1, PS 0),
// the billboard constants image (VS 2). The mirror must hold this view (viewAndMirror)
void StaticOps::bbWindowsFor(const bbrec::Record& rec, std::size_t k, d3d9::OwnedWindows& dw) {
    const orec::DrawRec& r = rec.draws[k];
    dw.clear();
    gpuscene::materialtable().fillWindows(r.matSlot, r.vsSet.data(), r.psSet.data(), dw);
    gpuscene::LightingImages& li = gpuscene::lightingimages();
    if (li.active() && r.illumSlot != gpuscene::kNoSlot) {
        gpuscene::ComposeParams p;
        p.tableLit = r.lightTableLit;
        p.holdsAll = r.lightHoldsAll;
        p.n = r.lightN;
        p.firstLight = r.lightFirst;
        const std::uint32_t entry = li.entryFor(r.illumSlot, p);
        if (entry != gpuscene::kNoEntry && li.ready()) {
            d3d9::OwnedWindows::Win& lw = dw.w[1][3];
            lw.kind = d3d9::OwnedWindows::Kind::Lighting;
            lw.base = li.windowBase(entry);
            lw.covered = gpuscene::kLightingRegs;
            lw.lightEntry = entry;
        }
    }
    float vs0[12][4];
    std::memcpy(vs0, mirror_.adminVs, sizeof(vs0));
    std::memcpy(vs0[0], rec.depthBias ? view_.dbvp : view_.vp, 64);
    const std::uint32_t vs1Mask = static_cast<std::uint32_t>((r.vsSet[0] >> 12) & ((1u << 19) - 1u));
    gpuscene::windowtables().fillViews(dev_, dw, mirror_.adminVs + 12, vs1Mask, vs0, mirror_.adminPs);
    gpuscene::bbtable().windowFor(rec.constSlot, dw.w[0][2]);
}

// the poll the composition makes at each draw's checkMaterial (once per frame per material record in the table): the draw's registers from the shader's pools, its samplers from the
// record, the same arguments; true = every draw's record is the one the build saw (slot, structure version)
bool StaticOps::bbPollMaterials(const bbrec::Record& rec) {
    if (!bbScratch_) bbScratch_ = std::make_unique<sc::DrawDesc>();
    sc::DrawDesc& d = *bbScratch_;
    std::uint64_t h = 0;
    for (std::size_t k = 0; k < rec.draws.size() && k < rec.mats.size(); ++k) {
        const orec::DrawRec& r = rec.draws[k];
        const bbrec::MatArgs& m = rec.mats[k];
        // (once per frame per material and pass kind: the table's own poll is once per frame, the later calls would only repeat the verification)
        const std::uint64_t pk = reinterpret_cast<std::uintptr_t>(m.owner) ^ (m.zPass ? 1ull : 0ull) ^ (m.blendMode == 0 ? 0ull : 2ull);
        if (bbPolledFrame_ != dev_.stats().frames) {
            bbPolledFrame_ = dev_.stats().frames;
            bbPolled_.clear();
        }
        if (std::find(bbPolled_.begin(), bbPolled_.end(), pk) != bbPolled_.end()) {
            h = mix(mix(h, r.matSlot), gpuscene::materialtable().structVersion(r.matSlot));
            continue;
        }
        bbPolled_.push_back(pk);
        d.vs = r.cd->vs;
        d.ps = r.cd->ps;
        d.vsSet = {};
        d.psSet = {};
        d.inheritMask = m.inheritMask;
        for (std::uint32_t mk = m.smpMask; mk != 0; mk &= mk - 1) {
            const std::uint32_t st = static_cast<std::uint32_t>(std::countr_zero(mk));
            d.samplers[st].texture = m.tex[st];
            for (std::uint32_t i = 0; i < 7; ++i) d.samplers[st].ss[i] = m.ss[st][i];
        }
        constantsInto(d, static_cast<U8*>(const_cast<void*>(m.owner)));
        checkMaterial(d, static_cast<U8*>(const_cast<void*>(m.owner)), m.material, m.zPass, m.psMask, m.blendMode, m.maskRef);
        if (d.matSlot != r.matSlot) return false;
        h = mix(mix(h, d.matSlot), gpuscene::materialtable().structVersion(d.matSlot));
    }
    return h == rec.stamp.materials;
}

bool StaticOps::bbReplay(const Op& op) {
    if (!bb_ || !leanCollect_ || leanBlock_ != 0) return false;
    if (viewKind_ != ViewKind::Main || g_ != &groups_[static_cast<std::size_t>(Group::Trees)] || op.kind != OpKind::SpeedTreeBB || inNonOp_ || (op.list != List::ZPass && op.list != List::Solid)) {
        return false;
    }
    bbrec::Store& B = *bb_;
    B.active = false;
    bbrec::Stats& w = B.window;
    const auto t0 = tnow();
    const auto refuse = [&](bbrec::Refuse r) {
        ++w.refuse[static_cast<std::size_t>(r)];
        w.replayNs += ns(t0, tnow());
        return false;
    };
    if (bbCheckFrame()) return refuse(bbrec::Refuse::CheckFrame);
    U8* ctx = static_cast<U8*>(op.raw.context);
    const orec::Key key{ctx, op.raw.desc, static_cast<std::uint8_t>(op.list), 0};
    const auto it = B.map.find(key);
    if (it == B.map.end()) return refuse(bbrec::Refuse::NoRecord);
    bbrec::Record& rec = *it->second;
    // the stamps, the constants' sources and the composition's read log: the record is the composition of this frame as long as they hold
    orec::Stamp st;
    const void *psv = nullptr, *listv = nullptr;
    std::uint32_t first = 0, count = 0;
    if (!bbStamp(op, st, psv, listv, first, count)) return refuse(bbrec::Refuse::Stale);
    {
        orec::Stamp a = rec.stamp;
        a.view = a.materials = 0;
        if (orec::firstDifference(a, st) >= 0 || rec.ps != psv || rec.list != listv || rec.first != first || rec.count != count) return refuse(bbrec::Refuse::Stale);
    }
    if (rec.nConst != 0 && !ret::validate(rec.srcReads.data(), rec.srcReads.size())) return refuse(bbrec::Refuse::Stale);
    if (!rec.reads.empty() && !ret::validate(rec.reads.data(), rec.reads.size())) return refuse(bbrec::Refuse::Stale);
    if (rec.nConst == 0 || rec.shader == nullptr || rec.draws.empty() || rec.mats.size() != rec.draws.size()) return refuse(bbrec::Refuse::Stale);
    if (!viewAndMirror(static_cast<U8*>(const_cast<void*>(rec.shader)))) return refuse(bbrec::Refuse::NoView);
    if (bbViewStamp() != rec.stamp.view) return refuse(bbrec::Refuse::Stale);
    if (rec.constSlot == gpuscene::BbTable::kNone) rec.constSlot = gpuscene::bbtable().intern(rec.consts.data(), rec.nConst);
    if (rec.constSlot == gpuscene::BbTable::kNone) return refuse(bbrec::Refuse::NoConsts);
    // every buffer / texture the draws read is still alive (the composition would have re-resolved them), then the material poll
    std::vector<d3d9::Device::DeriveIn>& ins = bbIns_;
    ins.assign(rec.draws.size(), {});
    for (std::size_t k = 0; k < rec.draws.size(); ++k) {
        bbDeriveIn(rec.draws[k], ins[k]);
        if (!dev_.ownedBindingsLive(ins[k])) return refuse(bbrec::Refuse::Stale);
    }
    for (const bbrec::MatArgs& m : rec.mats) {
        for (std::uint32_t mk = m.smpMask; mk != 0; mk &= mk - 1) {
            if (!dev_.ownedTextureLive(m.tex[std::countr_zero(mk)])) return refuse(bbrec::Refuse::Stale);
        }
    }
    // the lighting of a solid op: the illumination's record is the one drawn from, polled as the composition's checkDraw does (bytes against the record, once per frame), and its
    // lights handed to the light table as writeLighting does
    if (op.list == List::Solid) {
        U8* illumPs = ptr(ptr(ctx, 0x10), 0xf8);
        U8* illum = illumPs != nullptr ? illumPs + 0x78 : nullptr;
        for (const orec::DrawRec& r : rec.draws) {
            if (r.illumSlot == gpuscene::kNoSlot) continue;
            if (!gpuscene::illumtable().pollDraw(illum, r.illumSlot)) return refuse(bbrec::Refuse::Stale);
        }
        if (tableLit_ && illum != nullptr) bbNoteLights(illum);
    }
    if (!bbPollMaterials(rec)) return refuse(bbrec::Refuse::Material);
    // the draw tag of each draw (the pack word reads it): the op's tag + the draw's sub-mesh fields, as issue() sets it
    d3d9::draws::Tag& tag = dev_.drawTag();
    const d3d9::draws::Tag savedTag = tag;
    const auto setTag = [&](const orec::DrawRec& r) {
        tag = savedTag;
        if (r.tagSubMesh != nullptr) {
            tag.subMesh = r.tagSubMesh;
            tag.material = r.tagMaterial;
            tag.meshResource = r.tagMeshResource;
            tag.water = static_cast<d3d9::draws::Water>(r.tagWater);
            tag.materialName = r.tagMaterialName;
        }
    };
    // all draws are checked before one is submitted (a refusal after the first would draw it twice)
    bbrec::Refuse why = bbrec::Refuse::Count;
    std::vector<d3d9::OwnedWindows>& wins = bbWins_;
    wins.resize(rec.draws.size());
    for (std::size_t k = 0; k < rec.draws.size() && why == bbrec::Refuse::Count; ++k) {
        const orec::DrawRec& r = rec.draws[k];
        setTag(r);
        bbWindowsFor(rec, k, wins[k]);
        int dwhy = 0;
        switch (dev_.submitOwned(*r.cd, ins[k], wins[k], true, &dwhy)) {
            case d3d9::Device::OwnedWhy::Submitted: break;
            case d3d9::Device::OwnedWhy::Window: why = bbrec::Refuse::Windows; break;
            case d3d9::Device::OwnedWhy::Consumers: why = bbrec::Refuse::Consumers; break;
            case d3d9::Device::OwnedWhy::Derive: why = bbrec::Refuse::Derive; if (dwhy > 0 && dwhy < static_cast<int>(orec::kDeriveWhyCount)) ++w.refused[dwhy]; break;
            default: why = bbrec::Refuse::Submit; break;
        }
    }
    if (why != bbrec::Refuse::Count) {
        tag = savedTag;
        return refuse(why);
    }
    std::size_t made = 0;
    for (std::size_t k = 0; k < rec.draws.size(); ++k) {
        const orec::DrawRec& r = rec.draws[k];
        setTag(r);
        if (dev_.submitOwned(*r.cd, ins[k], wins[k], false) == d3d9::Device::OwnedWhy::Submitted) ++made;
        else ++w.refuse[static_cast<std::size_t>(bbrec::Refuse::Partial)];  // (not expected: the dry run passed; the draw is lost for this frame)
    }
    tag = savedTag;
    // the engine's slot is skipped: its side effects at the op's place (leanCommit): the billboard mesh's inline touch, the images the draws sample, BindLightmaps (solid), stream 3 (z)
    U8* res = static_cast<U8*>(const_cast<void*>(rec.stamp.res));
    touchNow(rec.touchFirst);
    leanTouches_ = rec.touches;
    if (op.list == List::Solid) bindLightmaps(res);
    for (const auto& s3 : rec.stream3) leanStream3(res, s3.second);
    ++g_->window.opsComposed;
    g_->issuedDraws += made;
    ++g_->issuedOps;
    op_ = &op;
    engineDraws_ = 0;
    opDiffLogged_ = false;
    taken_ = true;
    drawCount_ = 0;
    leftCode_ = static_cast<int>(Skip::None);
    maskRefShader_ = nullptr;
    ++w.ownedOps;
    w.ownedDraws += made;
    w.replayNs += ns(t0, tnow());
    leanCommit();
    return true;
}

void StaticOps::bbReport() {
    if (!ownedBb_ || !bb_) return;
    bbrec::Store& B = *bb_;
    bbrec::Stats& w = B.window;
    B.total.add(w);
    const std::uint64_t frames = std::max<std::uint64_t>(1, dev_.stats().frames - B.lastFrame);
    B.lastFrame = dev_.stats().frames;
    const auto list = [](const std::uint64_t* v, const char* const* names, std::size_t n, std::size_t from = 0) {
        std::string out;
        for (std::size_t i = from; i < n; ++i) {
            if (v[i] != 0) out += std::format("{}{} {}", out.empty() ? "" : ", ", names[i], v[i]);
        }
        return out.empty() ? std::string("none") : out;
    };
    std::string rebuilt;
    for (std::size_t i = 0; i < orec::kStampFields + bbrec::kExtraWhyCount; ++i) {
        if (w.rebuiltWhy[i] == 0) continue;
        const char* n = i < orec::kStampFields ? orec::kStampNames[i] : bbrec::kExtraWhy[i - orec::kStampFields];
        rebuilt += std::format("{}{} {}", rebuilt.empty() ? "" : ", ", n, w.rebuiltWhy[i]);
    }
    const double frame = static_cast<double>(frames);
    spdlog::info("native: owned billboards (5 s): {} recordable ops of {} ({:.1f} %), {} draws; not recorded: {}; valid at entry {} ({:.1f} % of recordable ops), built {}, rebuilt {} ({}), {} records; "
                 "derived {} draws (not derived: {}), compared with a scene draw {}, DIFFERING {} draws ({}), clean {}; constants: {} sources checked, {} stale, {} registers compared; "
                 "ignored carry-over: alphaRef {}, c5.w {}, view blocks at another slot {}; audit: {} records checked, {} caught by the post stamps, {} DISAGREEMENTS ({}); "
                 "OWNED ops {} draws {} (refused: {}); CPU per frame: build {:.1f} us ({} builds), derive + compare {:.1f} us ({} draws), stamps {:.1f} us, audit {:.1f} us, owned replay {:.1f} us",
                 w.ops, w.opsAll, w.opsAll != 0 ? 100.0 * static_cast<double>(w.ops) / static_cast<double>(w.opsAll) : 0.0, w.draws, list(w.excluded, bbrec::kExclNames, bbrec::kExclCount), w.validAtEntry,
                 w.ops != 0 ? 100.0 * static_cast<double>(w.validAtEntry) / static_cast<double>(w.ops) : 0.0, w.built, w.rebuilt, rebuilt.empty() ? "none" : rebuilt, B.map.size(), w.derived,
                 list(w.refused, orec::kDeriveWhy, orec::kDeriveWhyCount, 1), w.compared, w.differing, list(w.byField, bbrec::kFieldNames, bbrec::kFieldCount), w.clean, w.constChecks, w.constStale,
                 w.constRegsCompared, w.alphaRefCarry, w.c5wCarry, w.viewDup, w.auditChecked, w.auditCaught, w.auditDisagree, list(w.auditBy, orec::kAuditNames, orec::kAuditCount), w.ownedOps, w.ownedDraws,
                 list(w.refuse, bbrec::kRefuseNames, bbrec::kRefuseCount), static_cast<double>(w.buildNs) / frame / 1000.0, w.builds, static_cast<double>(w.deriveNs) / frame / 1000.0, w.derives,
                 static_cast<double>(w.stampNs) / frame / 1000.0, static_cast<double>(w.auditNs) / frame / 1000.0, static_cast<double>(w.replayNs) / frame / 1000.0);
    spdlog::info("native: owned billboards (total): {} recordable ops, {} draws, valid at entry {}, built {}, rebuilt {}, derived {}, compared {}, DIFFERING {} ({}), audit DISAGREEMENTS {} ({} more caught by the post stamps) of {} checks, constants stale {} of {}, owned ops {} draws {}; constants table {} of {} images, FULL refusals {}",
                 B.total.ops, B.total.draws, B.total.validAtEntry, B.total.built, B.total.rebuilt, B.total.derived, B.total.compared, B.total.differing,
                 list(B.total.byField, bbrec::kFieldNames, bbrec::kFieldCount), B.total.auditDisagree, B.total.auditCaught, B.total.auditChecked, B.total.constStale, B.total.constChecks, B.total.ownedOps,
                 B.total.ownedDraws, gpuscene::bbtable().used(), gpuscene::BbTable::kCapacity, gpuscene::bbtable().fullRefusals());
    w = {};
}

}  // namespace rvk::native
