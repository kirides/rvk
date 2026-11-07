// F2c-2 op records (docs/research/frame-ownership.md 7.14, native.owned_records): see op_records.h. Part of StaticOps (static_ops.h).
//
// recEnter      before the op's composition: cut-1 eligibility (the reasons of 7.12), the stamps, the record valid at entry, the signal audit (the record's
//               read log against the engine's memory), the read log switched on when a record is to be (re)built
// recComposed   after it: draw-level exclusions, the stamps that need the composition (material structure versions, the view)
// recDraw       after each issued draw: the draw's scene view as the device made it (probe) against the one DERIVED from the record + the per-frame inputs
// recEnd        after issue(): build / replace the record from the composition
// Nothing derived is submitted.

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
#include "engine/scene_table.h"
#include "gpuscene/illumination_table.h"
#include "gpuscene/lighting_images.h"
#include "gpuscene/material_table.h"
#include "gpuscene/object_table.h"
#include "gpuscene/verify.h"
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

constexpr std::uint32_t kStaticOpNone = 0xffffffu;

// the vtable slot of a candidate object `back` bytes before a logged address, without faulting (the page before may be unmapped: the guarded read would log a first-chance fault)
const void* objectVtable(const std::uint8_t* addr, std::size_t back) {
    const std::uint8_t* base = addr - back;
    if ((reinterpret_cast<std::uintptr_t>(addr) & 0xfffu) < back) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(base, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return nullptr;
    }
    return ptr(base, 0);
}

// a pointer into Engine.dll's image (a vtable: the object is an engine object)
bool inEngineImage(const void* p) {
    static const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"Engine.dll"));
    static const std::uintptr_t size = [] {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        return base != 0 ? static_cast<std::uintptr_t>(reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew)->OptionalHeader.SizeOfImage) : std::uintptr_t{0};
    }();
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    return base != 0 && v >= base && v < base + size;
}

}  // namespace

void StaticOps::setOwnedRecords(bool on) {
    if (on == ownedRecords_) return;
    ownedRecords_ = on;
    if (on && !recs_) recs_ = std::make_unique<orec::Store>();
    if (!on && recs_) {
        recs_->map.clear();
        recs_->active = false;
    }
    spdlog::info("native: owned records {}", on ? "on" : "off");
}

// the stamp of an op (everything but the view and the material structure, which need the composition)
bool StaticOps::recStamp(const Op& op, orec::Stamp& st) {
    st = {};
    U8* ctx = static_cast<U8*>(op.raw.context);
    U8* res = ptr(ctx, 0x28);
    U8* entity = ptr(ctx, 0x10);
    if (ctx == nullptr || res == nullptr || entity == nullptr) return false;
    st.res = res;
    st.entity = entity;
    st.entityFlags = get<std::uint32_t>(entity, 0x28) & 0xe00000u;
    U8* illumPs = ptr(entity, 0xf8);
    st.illumPs = illumPs;
    U8* illum = illumPs != nullptr ? illumPs + 0x78 : nullptr;
    U8* list = ptr(illum, 0);
    st.light = mix(mix(mixPtr(mixPtr(0, illum), list), get<std::uint8_t>(list, 0) | (get<std::uint8_t>(illum, 0xc) << 8) | (get<std::uint8_t>(illum, 0xd) << 16)),
                   get<std::uint8_t>(list, 0x34));
    st.switchValue = get<std::int32_t>(ptr(ctx, 0x18), 0x20);
    st.fadeBits = std::bit_cast<std::uint32_t>(op.raw.fade);
    {
        std::uint64_t h = 0;
        for (std::uint32_t i = 0; i < 2; ++i) {
            U8* lm = ptr(res, 0x90 + 8 * i);
            const bool ready = lm != nullptr && get<std::int32_t>(lm, 0x38) != 0 && ptr(lm, 0x70) != nullptr;
            h = mixPtr(mixPtr(h, lm), ready ? ptr(lm, 0x70) : nullptr);
        }
        st.lightmaps = h;
    }
    {
        const std::uint32_t sub = op.raw.desc & 0xffu;
        U8* subs = ptr(res, 0x58);
        const std::int32_t count = get<std::int32_t>(res, 0x60);
        std::uint64_t h = mix(mixPtr(mixPtr(0, subs), ptr(res, 0x88)), static_cast<std::uint64_t>(count) | (static_cast<std::uint64_t>(get<std::int32_t>(res, 0x38)) << 32));
        const std::int32_t first = sub == 0xffu ? 0 : static_cast<std::int32_t>(sub);
        const std::int32_t last = sub == 0xffu ? count : std::min<std::int32_t>(static_cast<std::int32_t>(sub) + 1, count);
        for (std::int32_t i = first; i < last && i < 64; ++i) {
            U8* s = subs + 0x48 * static_cast<std::size_t>(i);
            U8* m = ptr(s, 0x38);
            U8* sh = ptr(m, 0x70);
            h = mix(h, get<std::uint32_t>(s, 0x28) | (static_cast<std::uint64_t>(get<std::uint32_t>(s, 0x2c)) << 32));
            h = mix(h, get<std::uint32_t>(s, 0x30) | (static_cast<std::uint64_t>(get<std::uint32_t>(s, 0x34)) << 32));
            h = mixPtr(mixPtr(mixPtr(h, m), sh), ptr(sh, 0));  // material, shader, its vtable (the class)
            h = mix(h, static_cast<std::uint64_t>(get<std::int32_t>(m, 0x38)) | (static_cast<std::uint64_t>(get<std::int32_t>(sh, 0x50)) << 32));
            h = mix(h, get<std::uint8_t>(sh, 0x58) | (get<std::uint8_t>(sh, 0x79) << 8) | (get<std::uint8_t>(sh, 0x330) << 16) | (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x420)) << 24) |
                           (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x2d8)) << 32) | (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x78)) << 40) |
                           (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x2b8)) << 48) | (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x2a8)) << 56));
            h = mixPtr(mix(h, static_cast<std::uint32_t>(get<std::int32_t>(sh, 0x68))), ptr(sh, 0x220));
        }
        st.geom = h;
    }
    const std::uint32_t slot = engine::scenetable::find(entity);
    if (slot != UINT32_MAX) {
        if (const engine::scenetable::Slot* s = engine::scenetable::slot(slot)) {
            st.objSlot = slot;
            st.objGen = s->generation;
        }
    }
    st.variantEpoch = dev_.variantEpoch();
    // (only the table's verdict "holdsAll" depends on the registry, and the shipped code never skips the per-object light reads: ComposeParams::holdsAll is always false)
    st.registryGen = enh::fullLitSkipsReads() ? enh::LightTable::registryGeneration() : 0;
    return true;
}

// the view's inputs to a composed pass (the admin's version / type / z write / render mode, fog): F2c-3 asks it before the view's composition exists (viewStampOf)
std::uint64_t StaticOps::viewStampOf(const std::uint8_t* admin, bool zColorWrite, bool fog) {
    std::uint64_t h = mixPtr(0, admin);
    h = mix(h, get<std::uint8_t>(admin, 0x313) | (get<std::uint8_t>(admin, 0x314) << 8) | (zColorWrite ? 0x10000u : 0u) | (fog ? 0x20000u : 0u));
    h = mix(h, static_cast<std::uint64_t>(get<std::uint32_t>(admin, 0x340)) | (static_cast<std::uint64_t>(get<std::int32_t>(admin, 0x170)) << 32));
    h = mix(h, get<std::uint32_t>(admin, 0x31c));
    return h;
}
std::uint64_t StaticOps::recViewStamp() { return viewStampOf(view_.admin, view_.zColorWrite, view_.fog); }

// why an op is not in cut 1 (7.12 / 7.14), by reading the same engine fields the composition reads; Excl::Count = eligible
orec::Excl StaticOps::recExclusion(const Op& op) {
    U8* ctx = static_cast<U8*>(op.raw.context);
    U8* res = ptr(ctx, 0x28);
    U8* entity = ptr(ctx, 0x10);
    const std::uint32_t sub = op.raw.desc & 0xffu, node = op.raw.desc >> 8;
    const bool solid = op.list == List::Solid;
    orec::Excl ex = orec::Excl::Count;
    const auto reason = [&](orec::Excl e) {
        if (ex == orec::Excl::Count) ex = e;
    };
    if (res == nullptr || entity == nullptr) reason(orec::Excl::NotComposed);
    if (node == kStaticOpNone && res != nullptr) {
        const std::int32_t n = get<std::int32_t>(res, 0xb8);
        bool lightmaps = false;
        for (std::uint32_t i = 0; i < 2; ++i) {
            U8* lm = ptr(res, 0x90 + 8 * i);
            if (lm != nullptr && get<std::int32_t>(lm, 0x38) != 0 && ptr(lm, 0x70) != nullptr) lightmaps = true;
        }
        reason(get<std::uint8_t>(res, 0xa0) != 0 && n >= 2 && !lightmaps ? orec::Excl::Instanced : orec::Excl::InstanceList);
    }
    if ((get<std::uint32_t>(entity, 0x28) & 0xe00000u) != 0) reason(orec::Excl::Overlay);
    if (!(op.raw.fade >= 1.0f)) reason(orec::Excl::Fade);
    if (get<std::int32_t>(ctx, 0x34) > 0) reason(orec::Excl::Hierarchy);
    if (node != 0 && node != kStaticOpNone) reason(orec::Excl::NodeOp);
    if (solid) {
        U8* illumPs = ptr(entity, 0xf8);
        U8* illum = illumPs != nullptr ? illumPs + 0x78 : nullptr;
        if (illum == nullptr || ptr(illum, 0) == nullptr) reason(orec::Excl::NoList);
        if (!tableLit_) reason(orec::Excl::NotTableLit);
    }
    if (res != nullptr && ex == orec::Excl::Count) {
        U8* subs = ptr(res, 0x58);
        const std::int32_t count = get<std::int32_t>(res, 0x60);
        const bool all = sub == 0xffu;
        const std::int32_t first = all ? 0 : static_cast<std::int32_t>(sub);
        const std::int32_t last = all ? count : std::min<std::int32_t>(static_cast<std::int32_t>(sub) + 1, count);
        for (std::int32_t i = first; i < last && i < 64; ++i) {
            U8* shader = ptr(ptr(subs + 0x48 * static_cast<std::size_t>(i), 0x38), 0x70);
            if (shader == nullptr) continue;
            if (!shaderClassOk(shader)) {
                reason(orec::Excl::ShaderClass);
                continue;
            }
            if (get<std::uint8_t>(shader, 0x2b8) != 0) reason(orec::Excl::SortId);
            if (get<std::uint8_t>(shader, 0x78) != 0) reason(orec::Excl::DepthBias);  // (the owned view block holds the plain view-projection in c0..c3)
        }
    }
    return ex;
}

void StaticOps::recEnter(const Op& op) {
    ret::gLog = nullptr;
    if (!ownedRecords_ || !recs_) return;
    orec::Store& R = *recs_;
    R.active = R.building = R.auditBad = R.diffInOp = R.refused = R.wantOwnable = false;
    R.cur = nullptr;
    R.postDone = false;
    R.rebuildWhy = -1;
    R.probes.clear();
    R.pkeys.clear();
    recNoRetain_ = false;
    if (viewKind_ != ViewKind::Main || g_ != &groups_[static_cast<std::size_t>(Group::Static)] || op.kind != OpKind::Mesh || inNonOp_ ||
        (op.list != List::ZPass && op.list != List::Solid)) {
        return;
    }
    const auto t0 = tnow();
    orec::Stats& w = R.window;
    ++w.opsAll;
    U8* ctx = static_cast<U8*>(op.raw.context);
    U8* res = ptr(ctx, 0x28);
    U8* entity = ptr(ctx, 0x10);
    R.key = {ctx, op.raw.desc, static_cast<std::uint8_t>(op.list), 0};
    const orec::Excl ex = recExclusion(op);
    if (ex != orec::Excl::Count) {
        ++w.excluded[static_cast<std::size_t>(ex)];
        R.map.erase(R.key);
        w.stampNs += ns(t0, tnow());
        return;
    }
    orec::Stamp st;
    if (!recStamp(op, st)) {
        ++w.excluded[static_cast<std::size_t>(orec::Excl::NotComposed)];
        return;
    }
    R.stamp = st;
    R.active = true;
    R.op = &op;
    R.switchBefore = gpuscene::materialtable().stats().switchNotes;
    const auto it = R.map.find(R.key);
    if (it != R.map.end()) {
        orec::OpRecord& rec = *it->second;
        orec::Stamp a = rec.stamp;
        a.view = a.materials = 0;
        const int d = orec::firstDifference(a, st);
        if (d < 0) {
            const int live = recLiveWhy(rec);
            if (live < 0) R.cur = &rec;
            else R.rebuildWhy = live;
        } else {
            R.rebuildWhy = d;
        }
    }
    w.stampNs += ns(t0, tnow());
    if (R.cur != nullptr) {
        recAudit(op);
    }
    R.wantOwnable = ownedStatic_ && R.cur != nullptr && !R.cur->ownable;
    R.building = R.cur == nullptr || R.auditBad || R.wantOwnable;
    recCollect_ = ownedStatic_;  // (the touches / stream 3 lists of every composed eligible op: a record can be made from any composition)
    if (R.building) {
        R.readLog = {};
        ret::gLog = &R.readLog;  // (the composition's reads: the signal audit of the next frames)
    }
}

// what the stamps do not see (frame-ownership.md 7.21 addendum): the illumination's table slot (kept alive while the op is looked at: the idle sweep must not free
// the slot of an op that is drawn without being composed) and the loads that were pending when the record was built. Resident records: one slot compare.
int StaticOps::recLiveWhy(const orec::OpRecord& rec) {
    if (rec.illumSlot != gpuscene::kNoSlot && !gpuscene::illumtable().slotLive(rec.anchors[3], rec.illumSlot)) return static_cast<int>(orec::kStampFields) + 4;
    if (!rec.watch.empty() && !ret::validate(rec.watch.data(), rec.watch.size())) return static_cast<int>(orec::kStampFields) + 5;
    return -1;
}

// the signal audit: the record's read log against the engine's memory; the stamps called the record valid
void StaticOps::recAudit(const Op& op) {
    orec::Store& R = *recs_;
    const orec::OpRecord& rec = *R.cur;
    if (rec.reads.empty()) return;
    const auto t0 = tnow();
    orec::Stats& w = R.window;
    ++w.auditChecked;
    if (!ret::validate(rec.reads.data(), rec.reads.size())) {
        R.auditBad = true;
        // the first changed read, classified by where it lies
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
        R.auditCls = cls;
        if (R.auditLogged < 24 && bad != nullptr) {
            ++R.auditLogged;
            const auto base = reinterpret_cast<std::uintptr_t>(rec.anchors[std::min<std::size_t>(cls, 7)]);
            spdlog::info("native: owned records: audit: the read log says changed while the stamps hold ({}): {} read of {} bytes at {} (offset {:#x} into the {}), was {:#x}, now {:#x}",
                         describe(op), bad->kind == 1 ? "block" : "word", bad->size, static_cast<const void*>(bad->addr), reinterpret_cast<std::uintptr_t>(bad->addr) - base,
                         orec::kAuditNames[cls], bad->value, [&] {
                             std::uint64_t v = 0;
                             if (bad->kind == 0 && bad->size <= 8) readAt(bad->addr, 0, v);
                             return v;
                         }());
        }
    }
    w.auditNs += ns(t0, tnow());
}

// after the composition: draw-level exclusions, the stamps that need it
void StaticOps::recComposed(const Op& op, Skip s) {
    if (ret::gLog != nullptr && recs_ && ret::gLog == &recs_->readLog) ret::gLog = nullptr;
    if (!recs_ || !recs_->active) return;
    orec::Store& R = *recs_;
    orec::Stats& w = R.window;
    const auto t0 = tnow();
    const auto exclude = [&](orec::Excl e) {
        ++w.excluded[static_cast<std::size_t>(e)];
        R.map.erase(R.key);
        R.active = false;
        R.cur = nullptr;
    };
    if (s != Skip::None || drawCount_ == 0) {
        exclude(orec::Excl::NotComposed);
        return;
    }
    const bool solid = op.list == List::Solid;
    const bool lightImages = gpuscene::lightingimages().active();
    for (std::size_t k = 0; k < drawCount_; ++k) {
        const sc::DrawDesc& d = *draws_[k];
        if (d.inheritMask != 0 || !d.vertexBytes.empty() || d.cpuStream != 0 || !d.indexed || (solid && lightImages && d.lightEntry == gpuscene::kNoEntry)) {
            exclude(orec::Excl::DrawLevel);
            return;
        }
    }
    for (std::size_t k = 0; k < drawCount_; ++k) {
        // (F2c-3: an owned op's draws are opaque and write depth: drawn before the engine's other ops, in any order among themselves)
        const sc::DrawDesc& d = *draws_[k];
        if (d.states[sc::StBlend] != 0 || d.states[sc::StZWrite] == 0) {
            exclude(orec::Excl::Blended);
            return;
        }
    }
    if (leanBlock_ != 0) {
        exclude(orec::Excl::LeanBlock);
        return;
    }
    if (recNoRetain_) {
        exclude(orec::Excl::NoRetain);
        return;
    }
    if (gpuscene::materialtable().stats().switchNotes != R.switchBefore) {
        exclude(orec::Excl::Switch);
        return;
    }
    ++w.ops;
    w.draws += drawCount_;
    R.stamp.view = recViewStamp();
    {
        std::uint64_t h = 0;
        for (std::size_t k = 0; k < drawCount_; ++k) {
            const std::uint32_t slot = draws_[k]->matSlot;
            h = mix(mix(h, slot), gpuscene::materialtable().structVersion(slot));
        }
        R.stamp.materials = h;
    }
    if (R.cur != nullptr) {
        // post stamps: the view and the materials (polled by the composition), the draw count
        int why = -1;
        if (R.cur->stamp.view != R.stamp.view) why = 12;
        else if (R.cur->stamp.materials != R.stamp.materials) why = 13;
        else if (R.cur->draws.size() != drawCount_) why = static_cast<int>(orec::kStampFields);
        if (why >= 0) {
            R.rebuildWhy = why;
            R.cur = nullptr;
            R.building = true;
        }
    }
    if (R.auditBad) {
        // the read log says changed: the stamps that need the composition may have caught it (a material's structure, the view); else it is a disagreement
        if (R.cur == nullptr) {
            ++w.auditCaught;
        } else {
            ++w.auditDisagree;
            ++w.auditBy[R.auditCls];
        }
    }
    R.probes.assign(drawCount_, {});
    R.pkeys.assign(drawCount_, 0);
    w.stampNs += ns(t0, tnow());
}

namespace {
// two offers are the same window: kind, covered and base; view blocks (deduplicated by a bounded search) also when they hold the same registers at another slot
// (`live` = the registers the draw reads, bit i = register first + i: a view block's registers the shader never reads hold the register file's carry-over of
// whichever draw came before, and differ between blocks of one view)
bool sameWin(const d3d9::OwnedWindows::Win& a, const d3d9::OwnedWindows::Win& b, bool& dup, const std::uint64_t* live, std::uint32_t first) {
    if (a.kind != b.kind || a.covered != b.covered) return false;
    if (a.base.bits == b.base.bits) return true;
    if (a.kind == d3d9::OwnedWindows::Kind::View && a.image != nullptr && b.image != nullptr) {
        bool equal = true;
        for (std::uint32_t i = 0; i < a.covered && equal; ++i) {
            const std::uint32_t reg = first + i;
            if (live != nullptr && ((live[reg >> 6] >> (reg & 63)) & 1u) == 0) continue;
            equal = std::memcmp(a.image + 4u * i, b.image + 4u * i, 16) == 0;
        }
        if (equal) {
            dup = true;
            return true;
        }
    }
    return false;
}
}  // namespace

void StaticOps::recDraw(std::size_t k, const sc::DrawDesc& d, std::uint64_t pkey) {
    if (!recs_ || !recs_->active || k >= recs_->probes.size()) return;
    orec::Store& R = *recs_;
    R.probes[k] = R.probe;
    R.pkeys[k] = pkey;
    if (R.cur == nullptr || k >= R.cur->draws.size()) return;
    // 7.27: the derivation + compare of a valid record is verification: check frames only (the record's validity is the stamps' job, the audit's net and the
    // owned path's own derivation; a refused derive on another frame remakes the record at the next check frame)
    if (!gpuscene::verifyFrame()) return;
    const auto t0 = tnow();
    orec::Stats& w = R.window;
    const orec::OpRecord& rec = *R.cur;
    const orec::DrawRec& r = rec.draws[k];
    // ---- the derivation: the record + the per-frame inputs
    d3d9::Device::DeriveIn in;
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
    in.alphaFunc = r.alphaTestState != 0 ? r.alphaFunc : 8u;  // (deviceStates: SetMasterAlphaTesting(1), the test is off whatever the pass says)
    in.alphaRef = static_cast<float>(r.alphaRef & 0xffu);
    in.psC5[0] = mirror_.adminPs[5][0];
    in.psC5[1] = mirror_.adminPs[5][1];
    in.psC5[2] = mirror_.adminPs[5][2];
    in.psC5[3] = r.maskSet ? r.maskValue : 0.0f;
    if (r.cd->v.gate && r.cd->v.staticOk && r.illumSlot != gpuscene::kNoSlot) {
        // the ambient test of the GI / pack variant decision, from the record's illumination instead of the device's registers
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
    d3d9::Device::SceneView dv;
    const auto tc = tnow();
    const int why = dev_.deriveScene(*r.cd, in, dv);
    w.deriveCoreNs += ns(tc, tnow());
    ++w.derives;
    ++w.refused[static_cast<std::size_t>(why)];
    if (why != 0) {
        // (a module / variant / binding / texture the record was made for is gone: the record is remade from this composition)
        if (why == 1 || why == 4 || why == 6 || why == 7) R.refused = true;
        w.deriveNs += ns(t0, tnow());
        return;
    }
    ++w.derived;
    if (ownedPersistent_) ownedMissCheck(rec, k, in, dv);  // F2c-4a: the persistent draw (if the owned path would reuse it now) against this derivation
    const d3d9::Device::SceneView& pv = R.probes[k];
    std::array<bool, orec::kFieldCount> diff{};
    std::string detail;
    const auto fail = [&](orec::Field f, const std::string& what) {
        diff[static_cast<std::size_t>(f)] = true;
        if (detail.empty()) detail = std::format("{}: {}", orec::kFieldNames[static_cast<std::size_t>(f)], what);
    };
    const std::string cdv = std::format("[cd: gate {} staticOk {} wanted {} giDraw {} drawn {} scenePending {}, ambient from the record {}]", r.cd->v.gate, r.cd->v.staticOk, r.cd->v.wanted, r.cd->v.giDraw,
                                        r.cd->v.drawn, r.cd->v.scenePending, in.ambientLit);
    if (dv.scene != pv.valid) {
        {
            const auto& ss = dev_.sceneStats();
            fail(orec::Field::Scene, std::format("derived {} vs composed {} {} (variant kind {}, failed {}, pending {}, pipeline pending {}, no room {}, ring full {}, null address {}, not batchable {})", dv.scene, pv.valid, cdv,
                                                 ss.fbVariantKind, ss.fbVariantFailed, ss.fbVariantPending, ss.fbPipelinePending, ss.fbNoRoom, ss.fbRingFull, ss.fbNullAddress, ss.fbNotBatchable));
        }
    } else if (pv.valid) {
        ++w.compared;
        if (dv.pipeline != pv.pipeline) fail(orec::Field::Pipeline, std::format("{} vs {} {}", static_cast<std::uint32_t>(dv.pipeline), static_cast<std::uint32_t>(pv.pipeline), cdv));
        if (dv.viHash != pv.viHash) fail(orec::Field::VertexInput, "vertex input");
        if (dv.dynHash != pv.dynHash) fail(orec::Field::VertexInput, "dynamic state");
        if (dv.first != pv.first || dv.count != pv.count || dv.vertexOffset != pv.vertexOffset || dv.indexBuffer != pv.indexBuffer || dv.indexOffset != pv.indexOffset ||
            dv.index32 != pv.index32) {
            fail(orec::Field::Geometry, std::format("first {} / {}, count {} / {}, vertex offset {} / {}", dv.first, pv.first, dv.count, pv.count, dv.vertexOffset, pv.vertexOffset));
        }
        if (dv.flags != pv.flags) fail(orec::Field::Flags, std::format("{:#x} vs {:#x}", dv.flags, pv.flags));
        const shader::abi::DrawBlock& a = dv.block;
        const shader::abi::DrawBlock& b = pv.block;
        for (std::uint32_t i = 0; i < shader::abi::kSamplerSlots; ++i) {
            if (a.tex[i] != b.tex[i]) fail(orec::Field::BlockTex, std::format("slot {}: {} vs {}", i, a.tex[i], b.tex[i]));
            if (a.smp[i] != b.smp[i]) fail(orec::Field::BlockSmp, std::format("slot {}: {} vs {}", i, a.smp[i], b.smp[i]));
        }
        if (std::memcmp(a.posFix, b.posFix, sizeof(a.posFix)) != 0) {
            fail(orec::Field::BlockPos, std::format("({}, {}, {}, {}) vs ({}, {}, {}, {})", a.posFix[0], a.posFix[1], a.posFix[2], a.posFix[3], b.posFix[0], b.posFix[1], b.posFix[2], b.posFix[3]));
        }
        if (a.alphaFunc != b.alphaFunc) fail(orec::Field::BlockAlpha, std::format("func {} vs {}", a.alphaFunc, b.alphaFunc));
        else if (a.alphaRef != b.alphaRef) {
            if (a.alphaFunc != 8u) fail(orec::Field::BlockAlpha, std::format("ref {} vs {}", a.alphaRef, b.alphaRef));
            else ++w.alphaRefCarry;  // (the test is off: the shader reads no reference)
        }
        if (a.pad[0] != b.pad[0] || a.pad[1] != b.pad[1]) fail(orec::Field::BlockPad, std::format("({}, {}) vs ({}, {})", a.pad[0], a.pad[1], b.pad[0], b.pad[1]));
        // c5 is only composed (and read) when the pixel shader's live registers hold it; else the register is the device's carry-over
        const bool c5Live = ((r.psSet[0] >> 5) & 1u) != 0;
        if (!c5Live) {
            if (std::memcmp(a.psC5, b.psC5, 16) != 0) ++w.c5wCarry;
        } else if (std::memcmp(a.psC5, b.psC5, 12) != 0) {
            fail(orec::Field::BlockC5, std::format("({}, {}, {}) vs ({}, {}, {})", a.psC5[0], a.psC5[1], a.psC5[2], b.psC5[0], b.psC5[1], b.psC5[2]));
        } else if (a.psC5[3] != b.psC5[3]) {
            if (r.maskSet) fail(orec::Field::BlockC5, std::format("w {} vs {}", a.psC5[3], b.psC5[3]));
            else ++w.c5wCarry;  // (no mask this pass: the register keeps the previous pass's value, nothing reads it)
        }
    }
    // the draw tag
    {
        const d3d9::draws::Tag& tag = dev_.drawTag();
        if (tag.subMesh != r.tagSubMesh || tag.material != r.tagMaterial || tag.meshResource != r.tagMeshResource || static_cast<std::uint8_t>(tag.water) != r.tagWater ||
            tag.materialName != r.tagMaterialName) {
            fail(orec::Field::Tag, "sub-mesh fields");
        }
    }
    // the windows by table slot (the offer the draw's override carries): the record's material / lighting / object entries, the view's block
    if (ownedWindows_ && gpuscene::windowtables().active()) {
        d3d9::OwnedWindows dw;
        gpuscene::materialtable().fillWindows(r.matSlot, r.vsSet.data(), r.psSet.data(), dw);
        if (ownedObjects_ && gpuscene::objecttable().active() && r.vs1Offered) {
            if (!gpuscene::objecttable().windowFor(rec.stamp.objSlot, dw.w[0][2])) dw.w[0][2] = d3d9::OwnedWindows::Win{};
        }
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
            }
        }
        // view-level windows (VS 0 / 1, PS 0): the view block's slot is a per-view input; taken from the first draw of the view (and list), compared with the others
        const std::uint64_t vkey = recViewSerial_ * 2 + (rec.key.list == static_cast<std::uint8_t>(List::Solid) ? 1u : 0u);
        if (R.viewSerial != vkey) {
            R.viewSerial = vkey;
            R.viewWin = ownedWin_;
            R.viewWinValid = true;
        }
        dw.w[0][0] = R.viewWin.w[0][0];
        dw.w[1][0] = R.viewWin.w[1][0];
        dw.w[0][1] = r.vs1Offered ? R.viewWin.w[0][1] : d3d9::OwnedWindows::Win{};
        for (std::uint32_t st = 0; st < 2; ++st) {
            for (std::uint32_t wi = 0; wi < shader::abi::kConstWindows; ++wi) {
                bool dup = false;
                const bool same = sameWin(dw.w[st][wi], ownedWin_.w[st][wi], dup, st == 0 ? r.vsSet.data() : r.psSet.data(), (st == 0 ? shader::abi::kVsWindowFirst : shader::abi::kPsWindowFirst)[wi]);
                if (dup) ++w.viewDup;
                if (!same) {
                    std::string regs;
                    if (dw.w[st][wi].image != nullptr && ownedWin_.w[st][wi].image != nullptr) {
                        for (std::uint32_t q = 0; q < dw.w[st][wi].covered && regs.empty(); ++q) {
                            if (std::memcmp(dw.w[st][wi].image + 4 * q, ownedWin_.w[st][wi].image + 4 * q, 16) != 0) {
                                regs = std::format(" (first differing register +{}: ({}, {}, {}, {}) vs ({}, {}, {}, {}))", q, dw.w[st][wi].image[4 * q], dw.w[st][wi].image[4 * q + 1],
                                                   dw.w[st][wi].image[4 * q + 2], dw.w[st][wi].image[4 * q + 3], ownedWin_.w[st][wi].image[4 * q], ownedWin_.w[st][wi].image[4 * q + 1],
                                                   ownedWin_.w[st][wi].image[4 * q + 2], ownedWin_.w[st][wi].image[4 * q + 3]);
                            }
                        }
                    }
                    fail(orec::Field::Windows, std::format("{} window {}: kind {} base {:#x} covered {} vs composed kind {} base {:#x} covered {}", st == 0 ? "VS" : "PS", wi,
                                                           static_cast<int>(dw.w[st][wi].kind), dw.w[st][wi].base.bits, dw.w[st][wi].covered, static_cast<int>(ownedWin_.w[st][wi].kind),
                                                           ownedWin_.w[st][wi].base.bits, ownedWin_.w[st][wi].covered) + regs);
                }
            }
        }
    }
    bool any = false;
    for (std::size_t f = 0; f < orec::kFieldCount; ++f) {
        if (diff[f]) {
            ++w.byField[f];
            any = true;
        }
    }
    if (any) {
        ++w.differing;
        R.diffInOp = true;
        std::size_t firstField = 0;
        while (firstField < orec::kFieldCount && !diff[firstField]) ++firstField;
        if (R.loggedBy[firstField] < 6) {
            ++R.loggedBy[firstField];
            const d3d9::draws::Tag& tag = dev_.drawTag();
            spdlog::info("native: owned records: DIFFERING in {} draw {} ({}), material '{}': {}", describe(*op_), k, r.zPass ? "z" : "solid",
                         tag.materialName != nullptr ? tag.materialName : (d.tagMaterialName != nullptr ? d.tagMaterialName : ""), detail);
        }
    } else {
        ++w.clean;
    }
    w.deriveNs += ns(t0, tnow());
}

void StaticOps::recEnd(const Op& op) {
    if (!recs_ || !recs_->active) return;
    orec::Store& R = *recs_;
    orec::Stats& w = R.window;
    if (R.cur != nullptr && !R.auditBad && !R.refused && !R.wantOwnable) {
        ++w.validAtEntry;
        return;
    }
    if (R.auditBad && R.cur != nullptr && R.diffInOp) ++w.auditDisagreeDiff;
    const auto t0 = tnow();
    // the packets the draws were issued with (the packet cache's entries: the record keeps its own copy)
    auto rec = std::make_unique<orec::OpRecord>();
    rec->key = R.key;
    rec->stamp = R.stamp;
    rec->draws.reserve(drawCount_);
    for (std::size_t k = 0; k < drawCount_; ++k) {
        const sc::DrawDesc& d = *draws_[k];
        const std::uint64_t pkey = k < R.pkeys.size() ? R.pkeys[k] : 0;
        const auto pe = pkey != 0 ? packets_.find(pkey) : packets_.end();
        if (pe == packets_.end() || !pe->second.cd.valid) {
            ++w.excluded[static_cast<std::size_t>(orec::Excl::NotRetained)];
            R.map.erase(R.key);
            R.active = false;
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
        r.vs1Offered = ownedObjects_ && op_ != nullptr && op_->entity != nullptr && !(d.streams[0].buffer != nullptr && d.streams[0].freq != 1);
        r.tagSubMesh = d.tagSubMesh;
        r.tagMaterial = d.tagMaterial;
        r.tagMeshResource = d.tagMeshResource;
        r.tagWater = d.tagWater;
        r.tagMaterialName = d.tagMaterialName;
        r.zPass = d.zPass;
        r.passFlags = d.passFlags;
        r.scene = k < R.probes.size() && R.probes[k].valid;
        rec->matSlots.push_back(d.matSlot);
        for (std::uint32_t m = r.texMask; m != 0; m &= m - 1) {
            const void* t = r.tex[std::countr_zero(m)];
            if (t != nullptr && std::find(rec->images.begin(), rec->images.end(), t) == rec->images.end()) rec->images.push_back(t);
        }
        rec->draws.push_back(std::move(r));
    }
    {
        U8* ctx = static_cast<U8*>(op.raw.context);
        U8* res = ptr(ctx, 0x28);
        U8* entity = ptr(ctx, 0x10);
        U8* illumPs = ptr(entity, 0xf8);
        U8* illum = illumPs != nullptr ? illumPs + 0x78 : nullptr;
        U8* subs = ptr(res, 0x58);
        const std::uint32_t sub = op.raw.desc & 0xffu;
        U8* s0 = subs != nullptr ? subs + 0x48 * (sub == 0xffu ? 0u : sub) : nullptr;
        U8* mat = ptr(s0, 0x38);
        rec->anchors[0] = ctx;
        rec->anchors[1] = res;
        rec->anchors[2] = entity;
        rec->anchors[3] = illum;
        rec->anchors[4] = ptr(illum, 0);
        rec->anchors[5] = subs;
        rec->anchors[6] = mat;
        rec->anchors[7] = ptr(mat, 0x70);
        rec->lightmapRes = res;
        rec->bindLightmaps = op.list == List::Solid;
        rec->stream3 = op.list == List::ZPass;
        // F2c-3: what the engine's slot code touches (collected by leanTouch / leanImage / leanStream3 while the record was built)
        rec->touches = leanTouches_;
        for (const orec::DrawRec& r : rec->draws) {
            if (r.illumSlot != gpuscene::kNoSlot) {
                rec->illumSlot = r.illumSlot;
                break;
            }
        }
        for (const LeanStream3& x : leanStream3_) rec->stream3Prims.push_back({x.primitive, x.on});
    }
    rec->builtFrame = dev_.stats().frames;
    rec->ownable = recCollect_ || leanCollect_;
    if (ret::gLog == nullptr && !R.readLog.v.empty()) {
        ret::finalize(R.readLog.v);
        if (!R.readLog.failed) rec->reads = std::move(R.readLog.v);
    }
    {
        // the reads that looked at a resource still loading (7.21 addendum): re-read per op until the load completes
        for (const ret::ReadRec& q : rec->reads) {
            if (q.kind != 0 || rec->watch.size() >= 16) continue;
            const bool at38 = q.size == 4 && inEngineImage(objectVtable(q.addr, 0x38));
            const bool at70 = q.size == 8 && inEngineImage(objectVtable(q.addr, 0x70));
            if (orec::loadingRead(q.size, q.value, at38, at70, at70 ? get<std::int32_t>(q.addr - 0x38, 0) : 3)) rec->watch.push_back(q);
        }
    }
    R.readLog = {};
    const bool existed = R.map.find(R.key) != R.map.end();
    if (existed) {
        ++w.rebuilt;
        ++w.rebuiltWhy[R.rebuildWhy >= 0 ? static_cast<std::size_t>(R.rebuildWhy) : R.auditBad ? orec::kStampFields + 1 : orec::kStampFields + 2];
    } else {
        ++w.built;
    }
    if (R.map.size() > 30000) R.map.clear();
    R.map[R.key] = std::move(rec);
    ++w.builds;
    w.buildNs += ns(t0, tnow());
}

void StaticOps::recReport() {
    if (!ownedRecords_ || !recs_) return;
    orec::Store& R = *recs_;
    orec::Stats& w = R.window;
    R.total.add(w);
    if (ownedStatic_ && w.differing != 0) ownedAutoOff(w.differing);  // F2c-3: a check frame's derived draw differs from the composed one
    // an audit disagreement = a dependency the stamps do not cover (the audit caught it and the record was rebuilt: non-fatal, but a MISS-like finding the soak counts)
    if (w.auditDisagree != 0) spdlog::warn("native: owned records: MISS (audit): {} read log disagreements in 5 s while the stamps held", w.auditDisagree);
    const std::uint64_t frames = std::max<std::uint64_t>(1, dev_.stats().frames - R.lastFrame);
    R.lastFrame = dev_.stats().frames;
    const auto list = [](const std::uint64_t* v, const char* const* names, std::size_t n, std::size_t from = 0) {
        std::string out;
        for (std::size_t i = from; i < n; ++i) {
            if (v[i] != 0) out += std::format("{}{} {}", out.empty() ? "" : ", ", names[i], v[i]);
        }
        return out.empty() ? std::string("none") : out;
    };
    std::string rebuilt;
    for (std::size_t i = 0; i < orec::kStampFields + 6; ++i) {
        if (w.rebuiltWhy[i] == 0) continue;
        constexpr const char* kExtra[] = {"draw count", "read log", "derive refused", "other", "illumination slot gone", "resource load completed"};
        const char* n = i < orec::kStampFields ? orec::kStampNames[i] : kExtra[i - orec::kStampFields];
        rebuilt += std::format("{}{} {}", rebuilt.empty() ? "" : ", ", n, w.rebuiltWhy[i]);
    }
    const double frame = static_cast<double>(frames);
    spdlog::info("native: owned records (5 s): {} eligible ops of {} ({:.1f} %), {} draws; not in cut 1: {}; valid at entry {} ({:.1f} % of eligible ops), built {}, rebuilt {} ({}), {} records; "
                 "derived {} draws (not derived: {}), compared with a scene draw {}, DIFFERING {} draws ({}), clean {}; ignored carry-over: alphaRef {}, c5.w {}, view blocks at another slot with the same registers {}; "
                 "audit: {} records checked, read log changed while the stamps held: {} caught by the material / view stamps after the composition, {} DISAGREEMENTS ({}), of them with a compare difference {}; CPU per frame: build {:.1f} us ({} builds), derive + compare {:.1f} us (of it the device's derivation {:.1f} us; {} draws), stamps {:.1f} us, audit {:.1f} us; "
                 "object rows handed to observers {}, differing from the register file {}",
                 w.ops, w.opsAll, w.opsAll != 0 ? 100.0 * static_cast<double>(w.ops) / static_cast<double>(w.opsAll) : 0.0, w.draws, list(w.excluded, orec::kExclNames, orec::kExclCount),
                 w.validAtEntry, w.ops != 0 ? 100.0 * static_cast<double>(w.validAtEntry) / static_cast<double>(w.ops) : 0.0, w.built, w.rebuilt, rebuilt.empty() ? "none" : rebuilt,
                 R.map.size(), w.derived, list(w.refused, orec::kDeriveWhy, orec::kDeriveWhyCount, 1), w.compared, w.differing, list(w.byField, orec::kFieldNames, orec::kFieldCount), w.clean,
                 w.alphaRefCarry, w.c5wCarry, w.viewDup, w.auditChecked, w.auditCaught, w.auditDisagree, list(w.auditBy, orec::kAuditNames, orec::kAuditCount), w.auditDisagreeDiff, static_cast<double>(w.buildNs) / frame / 1000.0, w.builds,
                 static_cast<double>(w.deriveNs) / frame / 1000.0, static_cast<double>(w.deriveCoreNs) / frame / 1000.0, w.derives, static_cast<double>(w.stampNs) / frame / 1000.0, static_cast<double>(w.auditNs) / frame / 1000.0,
                 dev_.objectRowStats.given, dev_.objectRowStats.differing);
    spdlog::info("native: owned records (total): {} eligible ops, {} draws, valid at entry {}, built {}, rebuilt {}, derived {}, compared {}, DIFFERING {} ({}), audit DISAGREEMENTS {} ({} more caught by the post stamps) of {} checks ({})",
                 R.total.ops, R.total.draws, R.total.validAtEntry, R.total.built, R.total.rebuilt, R.total.derived, R.total.compared, R.total.differing,
                 list(R.total.byField, orec::kFieldNames, orec::kFieldCount), R.total.auditDisagree, R.total.auditCaught, R.total.auditChecked, list(R.total.auditBy, orec::kAuditNames, orec::kAuditCount));
    w = {};
}

}  // namespace rvk::native
