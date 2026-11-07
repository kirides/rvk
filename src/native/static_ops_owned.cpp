// F2c-3 owned static draws (docs/research/frame-ownership.md 7.16, setting native.owned_static): see static_ops.h.
//
// ownedViewBegin  the main view begins (frame tap): the frame kind (a check frame / its warm-up frame run the old path), the prerequisites
// ownedSelect     per op of the raw z / solid list: the op's record is valid (eligibility, stamps, view stamp, audit) -> the op leaves the engine's list
// ownedTake       the ops that left (sorted by the tap with the engine's comparator)
// ownedPass       BeginZPass / DoBeginRenderSolid returned: every owned op is drawn from its record (Device::replayOwned: the DrawBlock derived, every register
//                 window from the tables, no register file), its side effects run; an op that cannot be drawn that way is composed right there instead
//                 (opEnter: what the engine's slot would have done, at the same place of the frame)

#include <windows.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <format>
#include <unordered_set>

#include <spdlog/spdlog.h>

#include "core/f2c4_measure.h"
#include "d3d9/defs.h"
#include "engine/memread.h"
#include "engine/scene_table.h"
#include "engine/symbols.h"
#include "gpuscene/illumination_table.h"
#include "gpuscene/lighting_images.h"
#include "gpuscene/material_table.h"
#include "gpuscene/object_table.h"
#include "gpuscene/view_table.h"
#include "gpuscene/window_tables.h"
#include "hal/vulkan/hal_vulkan.h"
#include "gpuscene/verify.h"
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

// (the same mix as static_ops_records.cpp: the materials stamp is compared with the record's)
inline std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}
inline std::uint64_t ns(Clock::time_point a, Clock::time_point b) { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()); }

using BindLightmapsFn = int (*)(const void* self, int mode);

// why an owned op was composed late (OwnedStats::fallback)
enum Fallback : int { kFbView = 0, kFbMaterials, kFbObject, kFbRefused, kFbNoRecord, kFbNoView, kFbLost, kFbAudit, kFbCount };
constexpr const char* kFallbackNames[] = {"view windows", "material stamp / poll", "object window", "a draw refused", "record gone", "view not composed", "not composable (lost this frame)", "read log changed"};
static_assert(std::size(kFallbackNames) == kFbCount);

}  // namespace

void StaticOps::OwnedStats::add(const OwnedStats& o) {
    views += o.views; owned += o.owned; drawnOps += o.drawnOps; draws += o.draws; lateOps += o.lateOps; lateDraws += o.lateDraws; undrawn += o.undrawn;
    checkViews += o.checkViews; warmupViews += o.warmupViews; gatedViews += o.gatedViews; leanFrames += o.leanFrames; seen += o.seen;
    for (std::size_t i = 0; i < orec::kExclCount; ++i) leftExcl[i] += o.leftExcl[i];
    for (std::size_t i = 0; i < orec::kStampFields; ++i) leftStamp[i] += o.leftStamp[i];
    leftNoRecord += o.leftNoRecord; leftView += o.leftView; leftAudit += o.leftAudit; leftNotOwnable += o.leftNotOwnable; leftScene += o.leftScene;
    for (std::size_t i = 0; i < kFallback; ++i) fallback[i] += o.fallback[i];
    touches += o.touches; lightmaps += o.lightmaps; stream3 += o.stream3;
    compactNs += o.compactNs; selectNs += o.selectNs; buildNs += o.buildNs; submitNs += o.submitNs; sideNs += o.sideNs; matNs += o.matNs; prepNs += o.prepNs;
}

void StaticOps::setOwnedStatic(bool on, int check) {
    check = std::max(check, 0);
    if (!on) ownedTripped_ = false;
    if (on && ownedTripped_) on = false;  // (a check frame found a difference: off until the setting is switched off and on again)
    if (on == ownedStatic_ && check == ownedCheck_) return;
    if (on != ownedStatic_) spdlog::info("native: owned static {} (check frames every {})", on ? "on" : "off", check);
    ownedStatic_ = on;
    ownedCheck_ = check;
    if (!on) {
        for (auto& v : ownedOps_) v.clear();
        ownedView_ = {};
    }
}

void StaticOps::ownedAutoOff(std::uint64_t differing) {
    if (!ownedStatic_) return;
    ownedStatic_ = false;
    ownedTripped_ = true;
    for (auto& v : ownedOps_) v.clear();
    ownedView_ = {};
    spdlog::error("native: owned static turned off: {} draws DIFFERING on the check frames (the record-derived draw is not the composed one)", differing);
}

std::uint32_t StaticOps::ownedSlotOfKey(std::uint64_t viewKey, bool solid) {
    return viewKey == 0 ? (solid ? shader::abi::kViewSlotMainSolid : shader::abi::kViewSlotMainZ) : shader::abi::kViewSlotCascade0 + static_cast<std::uint32_t>(viewKey - 1);
}

bool StaticOps::ownedViewBegin(void* /*db*/, void* desc) { return ownedBegin(desc, ViewKind::Main, -1); }

// F3-1 (7.36): a cascade view. Nothing happens (not even the event drain: the main view's begin does it) while the setting is off
bool StaticOps::ownedShadowViewBegin(void* /*db*/, ViewKind kind, std::int32_t cascade) {
    if (!ownedShadows_ || ownedShadowsTripped_ || !ownedStatic_ || !ownedPersistent_) return false;
    if (kind != ViewKind::Cascade || ownedKeyOf(kind, cascade) == 0) return false;
    return ownedBegin(nullptr, kind, cascade);
}

void StaticOps::setOwnedShadows(bool on) {
    if (!on) ownedShadowsTripped_ = false;
    if (on && ownedShadowsTripped_) on = false;
    if (on == ownedShadows_) return;
    ownedShadows_ = on;
    spdlog::info("native: owned shadows {}", on ? "on" : "off");
    if (!on) {
        treeDropShadow();
        if (tro_) tro_->shadowTripped = false;
    }
    if (!on && recs_) {
        // the cascades' records are dropped (their persistent slots go back); the main view's stay
        for (auto it = recs_->map.begin(); it != recs_->map.end();) {
            if (it->first.view != 0) it = recs_->map.erase(it);
            else ++it;
        }
    }
}

void StaticOps::ownedShadowsOff(const char* why, std::uint64_t n) {
    if (!ownedShadows_) return;
    ownedShadows_ = false;
    ownedShadowsTripped_ = true;
    treeDropShadow();
    if (ownedCur_ != ViewKind::Main) {
        ownedOps_[0].clear();
        ownedView_ = {};
    }
    if (recs_) {
        for (auto it = recs_->map.begin(); it != recs_->map.end();) {
            if (it->first.view != 0) it = recs_->map.erase(it);
            else ++it;
        }
    }
    spdlog::error("native: owned shadows turned off: {} {}", n, why);
}

bool StaticOps::ownedBegin(void* desc, ViewKind vkind, std::int32_t cascade) {
    ownedCur_ = vkind;
    ownedShadowCheck_ = false;
    ownedViewKey_ = ownedKeyOf(vkind, cascade);
    OwnedStats& W = ownedStats();
    depDrain();  // F2e-1: the engine's events since the last view mark their records (before any stamp is looked at)
    for (auto& v : ownedOps_) {
        W.undrawn += v.size();  // (ops of an earlier view whose pass never began: lost for that frame)
        v.clear();
    }
    ownedView_ = {};
    if (!ownedStatic_) return false;
    ++W.views;
    if (f2c4::enabled() && vkind == ViewKind::Main) {
        f2c4::beginFrame(dev_.stats().frames);
        ++f2c4::counters().views;
        ++f2c4::counters().curViews;
    }
    const bool ready = ownedRecords_ && recs_ && sceneDraws_ && dev_.sceneDrawsActive() && ownedWindows_ && gpuscene::windowtables().active() && ownedObjects_ &&
                       gpuscene::objecttable().ready() && gpuscene::lightingimages().active() && gpuscene::lightingimages().ready() && gpuscene::materialtable().active() &&
                       tableLit_ && dev_.ownedObserversOk() && verified_ > 0;
    if (!ready) {
        ++W.gatedViews;
        if (ownedLogged_ < 4) {
            ++ownedLogged_;
            spdlog::info("native: owned static: not this view (records {}, scene draws {} / {}, windows {}, objects {} / {}, lighting images {} / {}, material table {}, table lighting {}, observers ok {}, static ops verified {})",
                         ownedRecords_ && recs_, sceneDraws_, dev_.sceneDrawsActive(), ownedWindows_ && gpuscene::windowtables().active(), ownedObjects_, gpuscene::objecttable().ready(),
                         gpuscene::lightingimages().active(), gpuscene::lightingimages().ready(), gpuscene::materialtable().active(), tableLit_, dev_.ownedObserversOk(), verified_);
        }
        return false;
    }
    const std::uint64_t frame = dev_.stats().frames;
    std::uint64_t kindFrame = frame;
    if (vkind != ViewKind::Main) {
        if (frame != shadowFrameSeen_) {
            shadowFrameSeen_ = frame;
            ++shadowFrameSerial_;
        }
        kindFrame = shadowFrameSerial_;
    }
    const sc::LeanFrame kind = sc::leanFrameKind(kindFrame, ownedCheck_);
    ownedShadowCheck_ = vkind != ViewKind::Main && kind == sc::LeanFrame::Check;
    if (kind == sc::LeanFrame::Check) {
        ++W.checkViews;
        return false;
    }
    if (kind == sc::LeanFrame::Warmup) {
        ++W.warmupViews;
        return false;
    }
    OwnedView& V = ownedView_;
    V.active = true;
    V.frame = frame;
    U8* depthImage = ptr(desc, 8);
    V.zColor = depthImage != nullptr && ptr(depthImage, 0x60) != nullptr;
    return true;
}

bool StaticOps::ownedListOk(void* db, List list) {
    if (!ownedView_.active) return false;
    if (ownedCur_ != ViewKind::Main) return list == List::ZPass;  // (ExecuteShadowMap runs its z list unconditionally: the shadow database's caps say no z pre-pass)
    std::uint8_t caps = 0;
    if (list == List::ZPass) return readAt(db, 0x1590, caps) && caps != 0;  // (the z pre-pass runs: BeginZPass brackets the list)
    return readAt(db, 0x15a0, caps) && caps == 0;                          // (DoBeginRenderSolid is called)
}

bool StaticOps::ownedSelect(const Op& op) {
    OwnedStats& W = ownedStats();
    ++W.seen;
    if (f2c4::enabled() && ownedCur_ == ViewKind::Main) {
        // F2c-4-0 item 4: the context's LOD resource (ctx + 0x28) and the set of its ops' descs of this frame
        f2c4::Counters::Ctx& e = f2c4::counters().ctxCur[op.raw.context];
        e.res = get<U8*>(op.raw.context, 0x28, nullptr);
        e.descSet += f2c4::mix64(op.raw.desc, static_cast<std::uint64_t>(op.list) + 1);
        ++e.ops;
    }
    struct T {
        std::uint64_t& ns;
        Clock::time_point t0 = tnow();
        ~T() { ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(tnow() - t0).count()); }
    } selTimer{W.selectNs};
    const orec::Key key{op.raw.context, op.raw.desc, static_cast<std::uint8_t>(op.list), ownedViewKey_};
    const auto it = recs_->map.find(key);
    if (it == recs_->map.end()) {
        // (not a candidate: the first reason it is not eligible, for the counters)
        const orec::Excl ex = recExclusion(op);
        if (ex != orec::Excl::Count) ++W.leftExcl[static_cast<std::size_t>(ex)];
        else ++W.leftNoRecord;
        return false;
    }
    const orec::OpRecord& rec = *it->second;
    rec.lastSeen = dev_.stats().frames;
    // a record exists only for an eligible op; of recExclusion's reasons the stamps hold the overlay flags, the fade, the light list, the shader's class / sort id /
    // depth bias; what they do not: the hierarchy flag of the context and the table lighting
    if (op.list == List::Solid && !tableLit_) {
        ++W.leftExcl[static_cast<std::size_t>(orec::Excl::NotTableLit)];
        return false;
    }
    if (!orec::hierarchyOwned() && get<std::int32_t>(op.raw.context, 0x34) > 0) {
        ++W.leftExcl[static_cast<std::size_t>(orec::Excl::Hierarchy)];
        return false;
    }
    if (!rec.ownable) {
        ++W.leftNotOwnable;
        return false;
    }
    // F2e-1 (7.30): a record no engine event touched is valid on its polled inputs alone (depFast); a marked one, or the 1 in 16 of the rolling scrub, runs the full stamp
    const bool events = depEvents();
    int fast = -2;
    if (events) fast = depFast(op, rec);
    if (fast >= 0) {
        ++W.leftStamp[static_cast<std::size_t>(fast)];
        return false;
    }
    const bool scrub = events && fast == -1 && (W.seen & 15u) == 0;
    if (events && fast == -1 && !scrub) {
        ++depW_.fast;
    } else {
        if (scrub) ++depW_.scrubbed;
        else if (events) ++depW_.slow;
        orec::Stamp st;
        if (!recStamp(op, st)) {
            ++W.leftNoRecord;
            return false;
        }
        orec::Stamp a = rec.stamp;
        a.view = a.materials = 0;
        if (const int d = orec::firstDifference(a, st); d >= 0) {
            // (a scrub finding with no mark = nothing announced it: an EVENT MISS; the polled-class fields never get here from the fast path)
            if (scrub) depEventMiss(op, rec, d, "rolling scrub");
            ++W.leftStamp[static_cast<std::size_t>(d)];
            return false;
        }
        if (events && !scrub) {
            ++depW_.slowClean;
            depVerified(rec);
        }
    }
    if (const int live = recLiveWhy(rec); live >= 0) {
        ++W.leftStamp[live == static_cast<int>(orec::kStampFields) + 4 ? 3 : 2];  // (illumination slot gone / a pending load completed: the engine draws it, the composition rebuilds the record)
        return false;
    }
    OwnedView& V = ownedView_;
    if (!V.adminKnown) {
        V.admin = ptr(rec.anchors[7], 0x220);  // (the shader's material admin: the view's; its pass-dependent bytes are stamped at the pass: ownedPass)
        V.adminKnown = true;
    }
    for (const orec::DrawRec& r : rec.draws) {
        if (!r.scene || r.cd == nullptr || !r.cd->valid) {
            ++W.leftScene;
            return false;
        }
    }
    return true;
}

void StaticOps::ownedTake(List list, std::vector<Op>&& ops, std::uint64_t compactNs) {
    ownedStats().owned += ops.size();
    ownedStats().compactNs += compactNs;
    ownedOps_[list == List::Solid ? 1 : 0] = std::move(ops);
}

// the record's read log against the engine's memory, at the pass (some of what the composition read is the pass's own state: the admin's pass type)
bool StaticOps::ownedAuditOk(const Op& op, const orec::OpRecord& rec) {
    if (rec.reads.empty() || ret::validate(rec.reads.data(), rec.reads.size())) return true;
    if (ownedLogged_ < 16) {
        ++ownedLogged_;
        for (const ret::ReadRec& r : rec.reads) {
            if (ret::validate(&r, 1)) continue;
            std::size_t cls = orec::kAuditCount - 1;
            for (std::size_t a = 0; a < 8; ++a) {
                const auto base = reinterpret_cast<std::uintptr_t>(rec.anchors[a]);
                const auto at = reinterpret_cast<std::uintptr_t>(r.addr);
                if (rec.anchors[a] != nullptr && at >= base && at < base + 0x600) {
                    cls = a;
                    break;
                }
            }
            std::uint64_t now = 0;
            if (r.kind == 0 && r.size <= 8) readAt(r.addr, 0, now);
            spdlog::info("native: owned static: the read log of {} says changed ({} read of {} bytes at {}, in the {} +{:#x}, was {:#x}, now {:#x})", describe(op), r.kind == 1 ? "block" : "word", r.size,
                         static_cast<const void*>(r.addr), orec::kAuditNames[cls], cls < 8 ? reinterpret_cast<std::uintptr_t>(r.addr) - reinterpret_cast<std::uintptr_t>(rec.anchors[cls]) : 0, r.value, now);
            break;
        }
    }
    return false;
}

// the materials' records polled (the composition's poll, once per frame) and their structure versions equal the record's stamp
bool StaticOps::ownedMaterialsOk(const orec::OpRecord& rec) {
    gpuscene::MaterialTable& mt = gpuscene::materialtable();
    const std::uint64_t frame = dev_.stats().frames + 1;
    std::uint64_t h = 0;
    const auto tm = tnow();
    struct Guard {
        std::uint64_t& ns;
        Clock::time_point t0;
        ~Guard() { ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(tnow() - t0).count()); }
    } guard{ownedStats().matNs, tm};
    for (const std::uint32_t slot : rec.matSlots) {
        if (slot >= gpuscene::MaterialTable::kCapacity) return false;
        if (slot >= ownedPolled_.size()) ownedPolled_.resize(gpuscene::MaterialTable::kCapacity, 0);
        if (ownedPolled_[slot] != frame) {
            if (!mt.pollOwned(slot, &StaticOps::materialTexture, this)) return false;
            ownedPolled_[slot] = frame;
        }
        h = mix(mix(h, slot), mt.structVersion(slot));
    }
    return h == rec.stamp.materials;
}

// the engine slot's non-drawing side effects of the op (the lean commit's order): BindLightmaps(1), the resource touches, stream 3
void StaticOps::ownedSideEffects(const orec::OpRecord& rec) {
    OwnedStats& W = ownedStats();
    struct Guard {
        std::uint64_t& ns;
        Clock::time_point t0 = tnow();
        ~Guard() { ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(tnow() - t0).count()); }
    } guard{W.sideNs};
    if (rec.bindLightmaps && rec.lightmapRes != nullptr) {
        bindLightmaps(static_cast<U8*>(rec.lightmapRes));  // (our copy of the admin's lightmap slots)
        reinterpret_cast<BindLightmapsFn>(engine::sym().meshBindLightmaps)(rec.lightmapRes, 1);
        ++W.lightmaps;
    }
    for (const void* res : rec.touches) {
        touchNow(res);
        ++W.touches;
    }
    for (const orec::OpRecord::Stream3& x : rec.stream3Prims) {
        static_cast<hal::vulkan::HalPrimitive*>(x.primitive)->EnableStream(3, x.on);
        ++W.stream3;
    }
}

// one op from its record: false = not drawn (why: a Fallback code), anything queued before the refusal stays (opaque draws: a repeat changes nothing)
bool StaticOps::ownedDrawOp(const Op& op, const orec::OpRecord& rec, const d3d9::OwnedWindows& viewWin, bool viewWinOk, int& why) {
    OwnedStats& W = ownedStats();
    if (!viewWinOk) {
        why = kFbView;
        return false;
    }
    if (!ownedMaterialsOk(rec)) {
        why = kFbMaterials;
        return false;
    }
    U8* ctx = static_cast<U8*>(op.raw.context);
    U8* entity = ptr(ctx, 0x10);
    // the object record: the entity's matrix as the signals hold it (the draw's composed rows would be these), the fade 1 of a z draw / the op's
    // (7.27: off the check frames a record current by the scene table's signals is served without reading the entity's matrix)
    d3d9::OwnedWindows::Win objWin;
    if (entity == nullptr) {
        why = kFbObject;
        return false;
    }
    if (gpuscene::verifyFrame() || !gpuscene::objecttable().serveFast(entity, 0x1eu, op.list == List::ZPass ? 1.0f : op.raw.fade, objWin)) {
        float world[16];
        if (!engine::safeRead(entity + 0x60, world, sizeof(world))) {
            why = kFbObject;
            return false;
        }
        float composed[gpuscene::kObjectRegs][4] = {};
        composed[1][3] = op.list == List::ZPass ? 1.0f : op.raw.fade;
        {
            float rows[3][4];
            gpuscene::objectWorldRows(world, rows);
            std::memcpy(composed[2], rows, sizeof(rows));
        }
        if (gpuscene::objecttable().serve(entity, composed, 0x1eu, objWin) != gpuscene::ObjectWhy::Served) {
            why = kFbObject;
            return false;
        }
    }
    gpuscene::LightingImages& li = gpuscene::lightingimages();
    gpuscene::IllumTable& illum = gpuscene::illumtable();
    d3d9::draws::Tag& tag = dev_.drawTag();
    const d3d9::draws::Tag baseTag = tag;
    for (std::size_t k = 0; k < rec.draws.size(); ++k) {
        const orec::DrawRec& r = rec.draws[k];
        const d3d9::Device::CachedDraw& cd = *r.cd;
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
        in.alphaFunc = r.alphaTestState != 0 ? r.alphaFunc : 8u;
        in.alphaRef = static_cast<float>(r.alphaRef & 0xffu);
        in.psC5[0] = mirror_.adminPs[5][0];
        in.psC5[1] = mirror_.adminPs[5][1];
        in.psC5[2] = mirror_.adminPs[5][2];
        in.psC5[3] = r.maskSet ? r.maskValue : 0.0f;
        gpuscene::ComposeParams p;
        p.tableLit = r.lightTableLit;
        p.holdsAll = r.lightHoldsAll;
        p.n = r.lightN;
        p.firstLight = r.lightFirst;
        if (cd.v.gate && cd.v.staticOk && r.illumSlot != gpuscene::kNoSlot) {
            // the ambient test of the GI / pack variant decision, from the record's illumination (once per lighting state and frame)
            const auto& amb = cd.ps->semantics[d3d9::Semantic::AmbientLight];
            if (amb.present() && amb.count >= 7 && amb.reg >= 14 && amb.reg + 7 <= 14 + 24) {
                const std::uint64_t key = mix(mix(mix(mix(r.illumSlot, (p.tableLit ? 1u : 0u) | (p.holdsAll ? 2u : 0u)), p.n), p.firstLight), amb.reg);
                const auto hit = ownedAmbient_.find(key);
                if (hit != ownedAmbient_.end()) {
                    in.ambientLit = hit->second;
                } else {
                    float L[24][4]{};
                    bool lit = false;
                    if (illum.compose(r.illumSlot, p, L)) {
                        for (std::int32_t i = 0; i < 7 && !lit; ++i) {
                            const float* c = L[amb.reg - 14 + i];
                            lit = c[0] != 0.0f || c[1] != 0.0f || c[2] != 0.0f || c[3] != 0.0f;
                        }
                    }
                    ownedAmbient_.emplace(key, lit);
                    in.ambientLit = lit;
                }
            }
        }
        // the windows: the view's, the material record's, the object record's, the lighting entry's
        d3d9::OwnedWindows dw;
        dw.w[0][0] = viewWin.w[0][0];
        dw.w[1][0] = viewWin.w[1][0];
        dw.w[0][1] = r.vs1Offered ? viewWin.w[0][1] : d3d9::OwnedWindows::Win{};
        {
            // (once per material slot and pass: the record's windows do not depend on the draw)
            auto mc = ownedCache_.mat.find(r.matSlot);
            if (mc == ownedCache_.mat.end()) {
                d3d9::OwnedWindows mw;
                gpuscene::materialtable().fillWindows(r.matSlot, r.vsSet.data(), r.psSet.data(), mw, true);
                mc = ownedCache_.mat.emplace(r.matSlot, mw).first;
            }
            dw.w[0][3] = mc->second.w[0][3];
            dw.w[1][2] = mc->second.w[1][2];
        }
        if (r.vs1Offered) dw.w[0][2] = objWin;
        if (li.active() && r.illumSlot != gpuscene::kNoSlot) {
            const std::uint64_t lkey = mix(mix(mix(r.illumSlot, (p.tableLit ? 1u : 0u) | (p.holdsAll ? 2u : 0u)), p.n), p.firstLight);
            std::uint32_t entry;
            if (const auto lc = ownedCache_.light.find(lkey); lc != ownedCache_.light.end()) {
                entry = lc->second;
            } else {
                entry = li.entryFor(r.illumSlot, p);
                ownedCache_.light.emplace(lkey, entry);
            }
            if (entry != gpuscene::kNoEntry && li.ready()) {
                d3d9::OwnedWindows::Win& lw = dw.w[1][3];
                lw.kind = d3d9::OwnedWindows::Kind::Lighting;
                lw.base = li.windowBase(entry);
                lw.covered = gpuscene::kLightingRegs;
            }
        }
        // the device state the draw is validated against: its shaders, the tag the observers / replacers / the pack's word look at
        if (dev_.vertexShader() != cd.vs) dev_.setVertexShaderRef(d3d9::ShaderRef(const_cast<d3d9::Shader*>(cd.vs)));
        if (dev_.pixelShader() != cd.ps) dev_.setPixelShaderRef(d3d9::ShaderRef(const_cast<d3d9::Shader*>(cd.ps)));
        tag = baseTag;
        tag.subMesh = r.tagSubMesh;
        tag.material = r.tagMaterial;
        tag.meshResource = r.tagMeshResource;
        tag.water = static_cast<d3d9::draws::Water>(r.tagWater);
        tag.materialName = r.tagMaterialName;
        const auto ts = tnow();
        const int rc = dev_.replayOwned(cd, in, dw);
        W.submitNs += ns(ts, tnow());
        if (rc != 0) {
            why = kFbRefused;
            tag = baseTag;
            return false;
        }
    }
    tag = baseTag;
    return true;
}

std::uint32_t StaticOps::ownedPass(bool solid) {
    std::vector<Op>& ops = ownedOps_[solid ? 1 : 0];
    if (ops.empty()) return 0;
    OwnedStats& W = ownedStats();
    PersistStats& PS = persistStats();
    const bool mainView = ownedCur_ == ViewKind::Main;
    if (!inView_ || viewKind_ != ownedCur_) {
        W.undrawn += ops.size();
        ops.clear();
        return 0;
    }
    const auto t0 = tnow();
    const std::uint64_t submit0 = W.submitNs, side0 = W.sideNs, mat0 = W.matNs;
    ret::gLog = nullptr;
    finishOp();
    leanAbort();
    // the view as the first op's composition would have made it
    bool viewOk = view_.valid;
    if (!viewOk && ownedView_.admin != nullptr) viewOk = composeView(ownedView_.admin);
    const auto tPrep = tnow();
    ++opSerial_;  // (the sampler snapshot of the polls is read afresh)
    // the view's stamp as the admin is now (the pass's state: the admin's type / mode bytes differ between the z and the solid pass)
    const std::uint64_t viewStamp = viewOk ? recViewStamp() : 0;
    d3d9::OwnedWindows viewWin;
    bool viewWinOk = false;
    const std::size_t opsInPass = ops.size();
    if (viewOk) {
        initMirror();
        std::memcpy(mirror_.adminVs[0], view_.vp, 64);  // (no depth-bias material is owned: c0..c3 = the view-projection)
        // scene ABI v2 (7.19): the view goes into the view table's fixed slot of the pass (0 z, 1 solid) and the draws read it from there; without the table (or when the
        // slot was set otherwise this frame) the v1 view block of the window tables is used as before
        gpuscene::ViewTable& vt = gpuscene::viewtable();
        if (vt.active() && vt.ready() && dev_.sceneViewsReady()) {
            float posFix[4];
            dev_.viewPosFix(posFix);
            if (f2c4::enabled() && mainView) {
                // item 1: a view block that differs from the same pass's earlier one of this frame (a second main view with another camera / viewport)
                f2c4::Counters& m = f2c4::counters();
                std::uint64_t h = 0;
                const auto add = [&](const float* f, std::size_t n) {
                    for (std::size_t i = 0; i < n; ++i) h = f2c4::mix64(h, std::bit_cast<std::uint32_t>(f[i]));
                };
                add(mirror_.adminVs[0], 12 * 4);
                add(mirror_.adminVs[12], 19 * 4);
                add(mirror_.adminPs[0], 5 * 4);
                add(posFix, 4);
                add(mirror_.adminPs[5], 3);
                const std::size_t k = solid ? 1 : 0;
                ++m.passViews[k];
                if (m.lastHashFrame[k] == dev_.stats().frames && m.lastHash[k] != h) ++m.viewDiffers[k];
                m.lastHash[k] = h;
                m.lastHashFrame[k] = dev_.stats().frames;
            }
            const std::uint32_t slot = ownedSlotOfKey(ownedViewKey_, solid);
            gpuscene::ViewTable::Regs regs;
            regs.vs0 = mirror_.adminVs;
            regs.vs1 = mirror_.adminVs + 12;
            regs.ps0 = mirror_.adminPs;
            regs.posFix = posFix;
            regs.c5 = mirror_.adminPs[5];
            viewWinOk = vt.set(slot, regs) && vt.fillWindows(slot, viewWin);
            if (!viewWinOk) viewWin.clear();
        }
        if (!viewWinOk) viewWinOk = gpuscene::windowtables().fillViews(dev_, viewWin, mirror_.adminVs + 12, 0x7ffffu, mirror_.adminVs, mirror_.adminPs);
    }
    // the device state to give back: shaders, tag, the register file (loaded for the observers that read material constants)
    static float savedVs[d3d9::kVsFloatConsts][4], savedPs[d3d9::kPsFloatConsts][4];
    std::memcpy(savedVs, dev_.vsConstants(), sizeof(savedVs));
    std::memcpy(savedPs, dev_.psConstants(), sizeof(savedPs));
    const std::uint64_t loadedBefore = dev_.ownedReplayStats.registersLoaded;
    const d3d9::ShaderRef vsSaved = dev_.vertexShaderRef(), psSaved = dev_.pixelShaderRef();
    d3d9::draws::Tag& tag = dev_.drawTag();
    const d3d9::draws::Tag savedTag = tag;
    W.prepNs += ns(tPrep, tnow());
    ownedAmbient_.clear();
    ownedCache_.mat.clear();
    ownedCache_.light.clear();
    ++ownedPassSerial_;
    std::uint32_t drawn = 0;
    dev_.setSceneIssue(true);
    // F2c-4a: persistent draws (frame-ownership.md 7.21): the pass's env epoch, the reserved ref list / argument blocks, the rolling scrub's stride
    bool persistent = false;
    std::uint32_t persistBound = 0;
    std::uint64_t epoch = 0;
    std::uint32_t scrubStride = 0, scrubPhase = 0, viewSlotP = 0;
    ownedItems_.clear();
    ownedItemOp_.clear();
    ownedRecPtrs_.assign(ops.size(), nullptr);
    {
        std::uint32_t bound = 0;
        for (std::size_t i = 0; i < ops.size(); ++i) {
            if (!recs_) break;
            const Op& o = ops[i];
            const auto it = recs_->map.find(orec::Key{o.raw.context, o.raw.desc, static_cast<std::uint8_t>(o.list), ownedViewKey_});
            if (it != recs_->map.end()) {
                ownedRecPtrs_[i] = it->second.get();
                bound += static_cast<std::uint32_t>(it->second->draws.size());
            }
        }
        if (ownedPersistent_ && viewWinOk && viewWin.viewSlot != d3d9::OwnedWindows::kNoView && gpuscene::drawrecords().ready() && bound != 0) {
            bool ok = dev_.ownedBeginScope();
            if (ok) epoch = dev_.ownedEnvEpoch(solid, viewWin.viewSlot, ok);
            if (ok) {
                if (dev_.ownedReserve(bound, ownedArena_)) {
                    persistent = true;
                    persistBound = bound;
                    viewSlotP = viewWin.viewSlot;
                    const std::uint64_t perFrame = 16;  // (the rolling scrub: draws re-derived and compared with their slot per frame)
                    scrubStride = static_cast<std::uint32_t>(std::max<std::uint64_t>(16, (bound + perFrame / 2 - 1) / (perFrame / 2)));  // (at most one draw in 16, 8 per pass in the big ones)
                    scrubPhase = static_cast<std::uint32_t>((++persistScrubTick_ * 7919u) % scrubStride);
                    ++PS.passes;
                    persistPassDraw_ = 0;
                    dev_.ownedBulkBegin(solid, mainView);  // F2c-4b: the bulk observers' list of this pass (a cascade's pass: the per-draw observers' loop)
                    const std::size_t kk = std::min<std::size_t>(viewSlotP, 7);
                    if (epoch != persistEnvLast_[kk]) {
                        ++PS.envChanges;
                        persistEnvLast_[kk] = epoch;
                    }
                } else {
                    ++PS.arenaFailed;
                }
            }
        }
    }
    // an owned op composed where the engine's slot would have run it (the record is rebuilt from this composition)
    const auto composeLate = [&](Op& op, int why) {
        ++W.fallback[why];
        dev_.setSceneIssue(false);
        tag = savedTag;
        tag.source = d3d9::draws::Source::Mesh;
        tag.context = op.raw.context;
        tag.entity = op.entity != nullptr ? op.entity : ptr(op.raw.context, 0x10);
        tag.opDesc = op.raw.desc;
        tag.subMesh = tag.material = tag.meshResource = nullptr;
        tag.water = d3d9::draws::Water::None;
        tag.materialName = nullptr;
        opEnter(op);
        if (op_ == &op) {
            ++W.lateOps;
            W.lateDraws += drawCount_;
            drawn += static_cast<std::uint32_t>(drawCount_);
        } else {
            ++W.fallback[kFbLost];
        }
        finishOp();
        dev_.setSceneIssue(true);
    };
    // persistent frames: the items collected so far are queued (one indirect draw per bucket) before an op is composed late: a composition rebuilds / may drop op
    // records, which the pending items point into (and the late op's texture transition makes the next ops' derivations succeed, as in the per-draw path). A failed
    // flush queued nothing: the ops that contributed are composed instead
    std::uint32_t flushedDraws = 0;
    const auto flushItems = [&] {
        if (persistent && mainView) {
            // F2c-4b: the bulk observers see the draws collected so far (before an op is composed late: its record may be dropped)
            const auto tb = tnow();
            dev_.ownedBulkFlush(false);
            PS.bulkNs += ns(tb, tnow());
        }
        if (!persistent || ownedItems_.empty()) return;
        const auto tf = tnow();
        const bool ok = dev_.ownedFlush(ownedItems_, ownedArena_, viewSlotP, gpuscene::drawrecords().recordsRef(), gpuscene::drawrecords().recordsShadow(), gpuscene::DrawRecordTable::kCapacity);
        PS.flushNs += ns(tf, tnow());
        PS.items += ownedItems_.size();
        flushedDraws += static_cast<std::uint32_t>(ownedItems_.size());
        std::vector<std::uint32_t> failed;
        if (!ok) {
            ++PS.flushFailed;
            std::uint32_t last = ~0u;
            for (const std::uint32_t oi : ownedItemOp_) {
                if (oi == last) continue;
                last = oi;
                failed.push_back(oi);
            }
        }
        ownedItems_.clear();
        ownedItemOp_.clear();
        // (the next flush of this pass needs a block of its own: the queued indirect draws still read this one)
        if (flushedDraws < persistBound) {
            if (!dev_.ownedReserve(persistBound - flushedDraws, ownedArena_)) {
                ++PS.arenaFailed;
                persistent = false;  // (the rest of the pass: the per-draw path)
            }
        }
        for (const std::uint32_t oi : failed) composeLate(ops[oi], kFbRefused);
    };
    for (std::size_t opIndex = 0; opIndex < ops.size(); ++opIndex) {
        Op& op = ops[opIndex];
        const orec::OpRecord* recPtr = ownedRecPtrs_[opIndex];
        tag = savedTag;
        tag.source = d3d9::draws::Source::Mesh;
        tag.context = op.raw.context;
        tag.entity = op.entity != nullptr ? op.entity : ptr(op.raw.context, 0x10);
        tag.opDesc = op.raw.desc;
        tag.subMesh = tag.material = tag.meshResource = nullptr;
        tag.water = d3d9::draws::Water::None;
        tag.materialName = nullptr;
        int why = kFbNoRecord;
        bool done = false;
        if (recPtr != nullptr) {
            if (!viewOk) {
                why = kFbNoView;
            } else if (recPtr->stamp.view != viewStamp) {
                why = kFbView;
            } else {
                const orec::OpRecord& rec = *recPtr;
                const std::size_t itemsBefore = ownedItems_.size();
                if (persistent) {
                    done = ownedDrawOpPersistent(op, rec, viewWin, viewSlotP, epoch, scrubStride, scrubPhase, why);
                    if (done) ownedItemOp_.resize(ownedItems_.size(), static_cast<std::uint32_t>(opIndex));
                    else ownedItems_.resize(itemsBefore);
                } else {
                    done = ownedDrawOp(op, rec, viewWin, viewWinOk, why);
                }
                if (done) {
                    ownedSideEffects(rec);
                    ++W.drawnOps;
                    W.draws += rec.draws.size();
                    drawn += static_cast<std::uint32_t>(rec.draws.size());
                }
            }
        }
        if (!done) {
            flushItems();
            composeLate(op, why);
        }
    }
    flushItems();
    if (persistent && mainView) {
        const auto tb = tnow();
        dev_.ownedBulkFlush(true);  // F2c-4b: the end of the pass (the left list)
        PS.bulkNs += ns(tb, tnow());
    }
    ownedItems_.clear();
    ownedItemOp_.clear();
    dev_.setSceneIssue(false);
    tag = savedTag;
    dev_.setVertexShaderRef(vsSaved);
    dev_.setPixelShaderRef(psSaved);
    if (dev_.ownedReplayStats.registersLoaded != loadedBefore) {
        dev_.setVertexShaderConstantF(0, savedVs[0], d3d9::kVsFloatConsts);
        dev_.setPixelShaderConstantF(0, savedPs[0], d3d9::kPsFloatConsts);
    }
    finishOp();
    ops.clear();
    if (f2c4::enabled() && mainView) {
        f2c4::Counters& m = f2c4::counters();
        const std::size_t k = solid ? 1 : 0;
        m.maxDrawsPerPass[k] = std::max<std::uint64_t>(m.maxDrawsPerPass[k], drawn);
        m.maxOpsPerPass[k] = std::max<std::uint64_t>(m.maxOpsPerPass[k], opsInPass);
    }
    const std::uint64_t total = ns(t0, tnow());
    const std::uint64_t sub = (W.submitNs - submit0) + (W.sideNs - side0) + (W.matNs - mat0);
    W.buildNs += total > sub ? total - sub : 0;
    return drawn;
}

// ---- F2c-4a persistent owned draws (frame-ownership.md 7.21, native.owned_persistent) ---------------------------------------------------------------------------

void StaticOps::PersistStats::add(const PersistStats& o) {
    ops += o.ops; draws += o.draws; reused += o.reused; derived += o.derived; derivedChanged += o.derivedChanged; tableFull += o.tableFull; refused += o.refused;
    observerRefused += o.observerRefused; injected += o.injected; scrubbed += o.scrubbed; scrubMiss += o.scrubMiss; checkCompared += o.checkCompared; checkSkipped += o.checkSkipped;
    checkMiss += o.checkMiss; passes += o.passes; envChanges += o.envChanges; flushFailed += o.flushFailed; arenaFailed += o.arenaFailed;
    sigNs += o.sigNs; deriveNs += o.deriveNs; observeNs += o.observeNs; flushNs += o.flushNs; windowNs += o.windowNs; items += o.items; bulkNs += o.bulkNs;
}

void StaticOps::setOwnedPersistent(bool on) {
    if (on == ownedPersistent_) return;
    ownedPersistent_ = on;
    gpuscene::drawrecords().setActive(on);
    if (on) {
        char v[16] = {};
        if (GetEnvironmentVariableA("RVK_F2C4A_INJECT", v, sizeof(v)) != 0) persistInjectEvery_ = static_cast<std::uint64_t>(std::strtoull(v, nullptr, 10));
        if (persistInjectEvery_ != 0) spdlog::warn("native: owned persistent: FAULT INJECTION on (RVK_F2C4A_INJECT={}): every {}th dependency change is not noticed", persistInjectEvery_, persistInjectEvery_);
    } else if (recs_) {
        for (auto& kv : recs_->map) {
            for (orec::DrawRec& d : kv.second->draws) d.persist.valid = false;
        }
    }
    spdlog::info("native: owned persistent {}", on ? "on" : "off");
}

// the ambient test of the GI / pack variant decision of a draw, from its record's illumination (once per lighting state and pass)
bool StaticOps::ownedAmbientLit(const orec::DrawRec& r, const gpuscene::ComposeParams& p) {
    const d3d9::Device::CachedDraw& cd = *r.cd;
    if (!(cd.v.gate && cd.v.staticOk && r.illumSlot != gpuscene::kNoSlot)) return false;
    const auto& amb = cd.ps->semantics[d3d9::Semantic::AmbientLight];
    if (!(amb.present() && amb.count >= 7 && amb.reg >= 14 && amb.reg + 7 <= 14 + 24)) return false;
    const std::uint64_t key = mix(mix(mix(mix(r.illumSlot, (p.tableLit ? 1u : 0u) | (p.holdsAll ? 2u : 0u)), p.n), p.firstLight), amb.reg);
    const auto hit = ownedAmbient_.find(key);
    if (hit != ownedAmbient_.end()) return hit->second;
    float L[24][4]{};
    bool lit = false;
    if (gpuscene::illumtable().compose(r.illumSlot, p, L)) {
        for (std::int32_t i = 0; i < 7 && !lit; ++i) {
            const float* c = L[amb.reg - 14 + i];
            lit = c[0] != 0.0f || c[1] != 0.0f || c[2] != 0.0f || c[3] != 0.0f;
        }
    }
    ownedAmbient_.emplace(key, lit);
    return lit;
}

void StaticOps::ownedMakeIn(const orec::DrawRec& r, bool ambientLit, d3d9::Device::DeriveIn& in) {
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
    in.ambientLit = ambientLit;
}

// the draw's eight windows: the view's (per pass), the material record's, the object record's, the lighting entry's. `cached` = the per-pass memos (the owned path);
// else everything afresh (the check frames)
void StaticOps::ownedBuildWindows(const orec::DrawRec& r, const d3d9::OwnedWindows& viewWin, const d3d9::OwnedWindows::Win& objWin, const gpuscene::ComposeParams& p, bool cached, d3d9::OwnedWindows& dw) {
    using Win = d3d9::OwnedWindows::Win;
    dw.viewSlot = viewWin.viewSlot;
    dw.w[0][0] = viewWin.w[0][0];
    dw.w[1][0] = viewWin.w[1][0];
    dw.w[0][1] = r.vs1Offered ? viewWin.w[0][1] : Win{};
    dw.w[1][1] = Win{};
    if (cached && r.matSlot < gpuscene::MaterialTable::kCapacity) {
        if (ownedMatWin_.empty()) ownedMatWin_.resize(gpuscene::MaterialTable::kCapacity);
        MatWinEntry& e = ownedMatWin_[r.matSlot];
        if (e.pass != ownedPassSerial_) {
            d3d9::OwnedWindows mw;
            gpuscene::materialtable().fillWindows(r.matSlot, r.vsSet.data(), r.psSet.data(), mw, true);
            e.v3 = mw.w[0][3];
            e.p2 = mw.w[1][2];
            e.pass = ownedPassSerial_;
        }
        dw.w[0][3] = e.v3;
        dw.w[1][2] = e.p2;
    } else {
        d3d9::OwnedWindows mw;
        gpuscene::materialtable().fillWindows(r.matSlot, r.vsSet.data(), r.psSet.data(), mw, true);
        dw.w[0][3] = mw.w[0][3];
        dw.w[1][2] = mw.w[1][2];
    }
    dw.w[0][2] = r.vs1Offered ? objWin : Win{};
    dw.w[1][3] = Win{};
    gpuscene::LightingImages& li = gpuscene::lightingimages();
    if (li.active() && r.illumSlot != gpuscene::kNoSlot) {
        std::uint32_t entry;
        if (cached) {
            const std::uint64_t lkey = mix(mix(mix(r.illumSlot, (p.tableLit ? 1u : 0u) | (p.holdsAll ? 2u : 0u)), p.n), p.firstLight);
            if (const auto lc = ownedCache_.light.find(lkey); lc != ownedCache_.light.end()) {
                entry = lc->second;
            } else {
                entry = li.entryFor(r.illumSlot, p);
                ownedCache_.light.emplace(lkey, entry);
            }
        } else {
            entry = li.entryFor(r.illumSlot, p);
        }
        if (entry != gpuscene::kNoEntry && li.ready()) {
            Win& lw = dw.w[1][3];
            lw.kind = d3d9::OwnedWindows::Kind::Lighting;
            lw.base = li.windowBase(entry);
            lw.covered = gpuscene::kLightingRegs;
        }
    }
}

namespace {
inline gpuscene::ComposeParams paramsOf(const orec::DrawRec& r) {
    gpuscene::ComposeParams p;
    p.tableLit = r.lightTableLit;
    p.holdsAll = r.lightHoldsAll;
    p.n = r.lightN;
    p.firstLight = r.lightFirst;
    return p;
}
}  // namespace

// MISS: the persistent content of a draw differs from a fresh derivation although the reuse conditions held. The first few are logged with the differing fields
void StaticOps::ownedMissReport(const char* where, const Op* op, const orec::DrawRec& r, const d3d9::Device::OwnedPersist& fresh, std::uint32_t slot) {
    if (persistMissLogged_ >= 12) return;
    ++persistMissLogged_;
    gpuscene::DrawRecordTable& tab = gpuscene::drawrecords();
    const shader::abi::DrawRecord& a = tab.record(slot);
    const shader::abi::DrawRecord& b = fresh.rec;
    std::string what;
    const auto add = [&](const std::string& s) { what += (what.empty() ? "" : "; ") + s; };
    for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) {
        if (a.vsWin[w] != b.vsWin[w]) add(std::format("VS window {} {:#x} vs {:#x}", w, a.vsWin[w], b.vsWin[w]));
        if (a.psWin[w] != b.psWin[w]) add(std::format("PS window {} {:#x} vs {:#x}", w, a.psWin[w], b.psWin[w]));
    }
    if (a.draw != b.draw) add("block address");
    if (a.flags != b.flags) add(std::format("flags {:#x} vs {:#x}", a.flags, b.flags));
    if (a.c5w != b.c5w) add(std::format("c5.w {:#x} vs {:#x}", a.c5w, b.c5w));
    const shader::abi::DrawBlock& x = tab.block(slot);
    const shader::abi::DrawBlock& y = fresh.blk;
    for (std::uint32_t i = 0; i < shader::abi::kSamplerSlots; ++i) {
        if (x.tex[i] != y.tex[i]) add(std::format("tex[{}] {} vs {}", i, x.tex[i], y.tex[i]));
        if (x.smp[i] != y.smp[i]) add(std::format("smp[{}] {} vs {}", i, x.smp[i], y.smp[i]));
    }
    if (x.alphaFunc != y.alphaFunc || x.alphaRef != y.alphaRef) add(std::format("alpha {} {} vs {} {}", x.alphaFunc, x.alphaRef, y.alphaFunc, y.alphaRef));
    if (x.pad[0] != y.pad[0] || x.pad[1] != y.pad[1]) add(std::format("pad ({}, {}) vs ({}, {})", x.pad[0], x.pad[1], y.pad[0], y.pad[1]));
    spdlog::error("native: owned persistent: MISS ({}): the persistent draw of {} (slot {}, material '{}') differs from a fresh derivation although nothing it was derived from moved: {}", where,
                  op != nullptr ? describe(*op) : std::string("an op"), slot, r.tagMaterialName != nullptr ? r.tagMaterialName : "", what.empty() ? "(bytes)" : what);
}

// one op from its persistent draw records: false = not drawn (why: a Fallback code); the draws it queued stay in ownedItems_ (the caller drops them)
bool StaticOps::ownedDrawOpPersistent(const Op& op, const orec::OpRecord& rec, const d3d9::OwnedWindows& viewWin, std::uint32_t viewSlot, std::uint64_t epoch, std::uint32_t scrubStride,
                                      std::uint32_t scrubPhase, int& why) {
    PersistStats& S = persistStats();
    const bool timers = f2c4::enabled();
    if (!ownedMaterialsOk(rec)) {
        why = kFbMaterials;
        return false;
    }
    ++S.ops;
    U8* ctx = static_cast<U8*>(op.raw.context);
    U8* entity = ptr(ctx, 0x10);
    // the object record: the entity's matrix as the signals hold it (the draw's composed rows would be these), the fade 1 of a z draw / the op's
    // (7.27: off the check frames a record current by the scene table's signals is served without reading the entity's matrix)
    d3d9::OwnedWindows::Win objWin;
    if (entity == nullptr) {
        why = kFbObject;
        return false;
    }
    if (gpuscene::verifyFrame() || !gpuscene::objecttable().serveFast(entity, 0x1eu, op.list == List::ZPass ? 1.0f : op.raw.fade, objWin)) {
        float world[16];
        if (!engine::safeRead(entity + 0x60, world, sizeof(world))) {
            why = kFbObject;
            return false;
        }
        float composed[gpuscene::kObjectRegs][4] = {};
        composed[1][3] = op.list == List::ZPass ? 1.0f : op.raw.fade;
        {
            float rows[3][4];
            gpuscene::objectWorldRows(world, rows);
            std::memcpy(composed[2], rows, sizeof(rows));
        }
        if (gpuscene::objecttable().serve(entity, composed, 0x1eu, objWin) != gpuscene::ObjectWhy::Served) {
            why = kFbObject;
            return false;
        }
    }
    (void)viewSlot;
    gpuscene::DrawRecordTable& tab = gpuscene::drawrecords();
    d3d9::draws::Tag& tag = dev_.drawTag();
    const d3d9::draws::Tag baseTag = tag;
    const auto restoreTag = [&] { tag = baseTag; };
    for (std::size_t k = 0; k < rec.draws.size(); ++k) {
        const orec::DrawRec& r = rec.draws[k];
        const d3d9::Device::CachedDraw& cd = *r.cd;
        orec::Persist& P = r.persist;
        ++S.draws;
        auto t0 = timers ? tnow() : Clock::time_point{};
        const auto lap = [&](std::uint64_t& acc) {
            if (!timers) return;
            const auto now = tnow();
            acc += ns(t0, now);
            t0 = now;
        };
        const gpuscene::ComposeParams p = paramsOf(r);
        const bool amb = ownedAmbientLit(r, p);
        ownedBuildWindows(r, viewWin, objWin, p, true, P.ow);
        // the device state the draw is derived under: its shaders, the tag the observers / replacers / the pack's word look at
        if (dev_.vertexShader() != cd.vs) dev_.setVertexShaderRef(d3d9::ShaderRef(const_cast<d3d9::Shader*>(cd.vs)));
        if (dev_.pixelShader() != cd.ps) dev_.setPixelShaderRef(d3d9::ShaderRef(const_cast<d3d9::Shader*>(cd.ps)));
        tag = baseTag;
        tag.subMesh = r.tagSubMesh;
        tag.material = r.tagMaterial;
        tag.meshResource = r.tagMeshResource;
        tag.water = static_cast<d3d9::draws::Water>(r.tagWater);
        tag.materialName = r.tagMaterialName;
        lap(S.windowNs);
        bool allowed = false;
        const std::uint64_t sig = dev_.ownedDrawSig(cd, r.streams, r.strides, r.indices, r.tex, amb, P.ow, allowed);
        lap(S.sigNs);
        if (!allowed) {
            ++S.refused;
            restoreTag();
            why = kFbRefused;
            return false;
        }
        bool reuse = P.valid && P.handle.has() && tab.live(P.handle.slot) && P.epoch == epoch && P.sig == sig;
        const bool scrub = reuse && scrubStride != 0 && (persistPassDraw_++ % scrubStride) == scrubPhase;
        if (!reuse || scrub) {
            if (!P.handle.has()) {
                const std::uint32_t slot = tab.alloc();
                if (slot == gpuscene::kNoSlot) {
                    ++S.tableFull;
                    restoreTag();
                    why = kFbRefused;
                    return false;
                }
                P.handle.slot = slot;
            }
            d3d9::Device::DeriveIn in;
            ownedMakeIn(r, amb, in);
            d3d9::Device::OwnedPersist out;
            const int rc = dev_.ownedDerive(cd, in, P.ow, tab.blockRef(P.handle.slot), tab.zeroRef(), true, out);
            lap(S.deriveNs);
            if (rc != 0) {
                P.valid = false;
                ++S.refused;
                restoreTag();
                why = kFbRefused;
                return false;
            }
            if (scrub) {
                ++S.scrubbed;
                if (std::memcmp(&tab.record(P.handle.slot), &out.rec, sizeof(out.rec)) != 0 || std::memcmp(&tab.block(P.handle.slot), &out.blk, sizeof(out.blk)) != 0) {
                    ++S.scrubMiss;
                    ownedMissReport("rolling scrub", &op, r, out, P.handle.slot);
                }
            } else {
                ++S.derived;
            }
            // FAULT INJECTION (RVK_F2C4A_INJECT=n): every n-th change of a slot's content that a moved dependency makes necessary is not made (the slot keeps its stale content and
            // looks up to date from now on): the check frames / the scrub must find it (MISS)
            bool skipWrite = false;
            if (!scrub && P.valid && persistInjectEvery_ != 0 &&
                (std::memcmp(&tab.record(P.handle.slot), &out.rec, sizeof(out.rec)) != 0 || std::memcmp(&tab.block(P.handle.slot), &out.blk, sizeof(out.blk)) != 0) &&
                ++persistInjectSeen_ % persistInjectEvery_ == 0) {
                skipWrite = true;
                ++S.injected;
            }
            if (!skipWrite && tab.write(P.handle.slot, out.rec, out.blk) && !scrub) ++S.derivedChanged;
            P.valid = true;
            P.epoch = epoch;
            P.sig = sig;
            P.bucket = out.bucketKey;
        } else {
            ++S.reused;
        }
        d3d9::Device::OwnedObsInfo obs;
        obs.id = P.handle.slot;
        obs.sig = P.sig;
        obs.epoch = P.epoch;
        obs.changed = !reuse;
        if (!dev_.ownedObserve(cd, P.ow, obs)) {
            ++S.observerRefused;
            restoreTag();
            why = kFbRefused;
            return false;
        }
        lap(S.observeNs);
        ownedItems_.push_back({&cd, P.bucket, P.handle.slot});
    }
    restoreTag();
    return true;
}

// check frames (recDraw): would the owned path reuse this draw's slot now? then its content must be what the composition's derivation gives
void StaticOps::ownedMissCheck(const orec::OpRecord& rec, std::size_t k, const d3d9::Device::DeriveIn& in, const d3d9::Device::SceneView& derived) {
    if (!ownedPersistent_ || !ownedStatic_ || k >= rec.draws.size()) return;  // (owned_static tripped by a MISS: the slots are not used any more)
    const orec::DrawRec& r = rec.draws[k];
    orec::Persist& P = r.persist;
    PersistStats& S = rec.key.view != 0 ? persistShadowW_ : persistW_;
    if (!P.valid || !P.handle.has()) return;  // (no persistent derivation of this draw yet)
    gpuscene::DrawRecordTable& tab = gpuscene::drawrecords();
    gpuscene::ViewTable& vt = gpuscene::viewtable();
    const bool solid = rec.key.list == static_cast<std::uint8_t>(List::Solid);
    const std::uint32_t viewSlot = ownedSlotOfKey(rec.key.view, solid);
    d3d9::OwnedWindows viewWin;
    d3d9::OwnedWindows::Win objWin;
    if (!tab.ready() || !tab.live(P.handle.slot) || !vt.ready() || !vt.fillWindows(viewSlot, viewWin) || (r.vs1Offered && !gpuscene::objecttable().windowFor(rec.stamp.objSlot, objWin))) {
        ++S.checkSkipped;
        return;
    }
    const gpuscene::ComposeParams p = paramsOf(r);
    d3d9::OwnedWindows dw;
    ownedBuildWindows(r, viewWin, objWin, p, false, dw);
    bool allowed = false;
    const std::uint64_t sig = dev_.ownedDrawSig(*r.cd, r.streams, r.strides, r.indices, r.tex, in.ambientLit, dw, allowed);
    if (!allowed || P.sig != sig || !dev_.ownedEnvSame(solid, viewSlot, P.epoch)) {
        ++S.checkSkipped;  // (the owned path would derive this draw again)
        return;
    }
    d3d9::Device::OwnedPersist fresh;
    if (dev_.ownedBuild(*r.cd, derived, dw, tab.blockRef(P.handle.slot), tab.zeroRef(), fresh) != 0) {
        ++S.checkSkipped;
        return;
    }
    ++S.checkCompared;
    if (std::memcmp(&tab.record(P.handle.slot), &fresh.rec, sizeof(fresh.rec)) != 0 || std::memcmp(&tab.block(P.handle.slot), &fresh.blk, sizeof(fresh.blk)) != 0) {
        ++S.checkMiss;
        ownedMissReport("check frame", op_, r, fresh, P.handle.slot);
    }
}

void StaticOps::ownedAutoOffMiss(std::uint64_t miss) {
    if (!ownedStatic_) return;
    ownedStatic_ = false;
    ownedTripped_ = true;
    for (auto& v : ownedOps_) v.clear();
    ownedView_ = {};
    spdlog::error("native: owned static turned off: {} MISS (a persistent draw judged valid differs from a fresh derivation: a dependency change was not noticed)", miss);
}

void StaticOps::ownedReport() {
    if (f2c4::enabled() && ownedStatic_) {
        // item 6: the distinct buckets of every live record (every 5 s: a walk over the record map)
        f2c4::Counters& m = f2c4::counters();
        std::unordered_set<std::uint64_t> keys;
        std::uint64_t records = 0, draws = 0;
        if (recs_) {
            for (const auto& kv : recs_->map) {
                ++records;
                for (const orec::DrawRec& r : kv.second->draws) {
                    if (r.cd == nullptr || !r.cd->valid) continue;
                    ++draws;
                    keys.insert(dev_.f2c4BucketKey(*r.cd));
                }
            }
        }
        m.liveRecords = records;
        m.liveDraws = draws;
        m.liveBuckets = keys.size();
        f2c4::report();
    }
    if (ownedPersistent_ || persistW_.passes != 0 || persistW_.checkCompared != 0) {
        PersistStats& q = persistW_;
        persistT_.add(q);
        const double nv = static_cast<double>(std::max<std::uint64_t>(1, ownedW_.views));
        static d3d9::Device::OwnedPersistStats lastDs;
        const d3d9::Device::OwnedPersistStats& ds = dev_.ownedPersistStats;
        const gpuscene::DrawRecordTable& tab = gpuscene::drawrecords();
        const std::uint64_t miss = q.scrubMiss + q.checkMiss;
        spdlog::info("native: owned persistent (5 s): {} passes ({} env epoch changes, {} reserve failures, {} flush failures), {} ops / {} draws: reused {} ({:.1f} %), derived {} ({} rewrote the slot), refused {} "
                     "(table full {}, an observer wanted a capture {}); rolling scrub {} draws, MISS {}; check frames: {} compared, {} skipped (the owned path would derive them), MISS {}; injected {}; "
                     "buckets {} / indirect packets {} / items {} (device total {}); slots {} live (peak {}), table writes {} changed / {} unchanged; CPU per main view: windows {:.1f} us, signature {:.1f} us, derive {:.1f} us, "
                     "observers {:.1f} us, flush {:.1f} us{}",
                     q.passes, q.envChanges, q.arenaFailed, q.flushFailed, q.ops, q.draws, q.reused, q.draws != 0 ? 100.0 * static_cast<double>(q.reused) / static_cast<double>(q.draws) : 0.0, q.derived + q.scrubbed,
                     q.derivedChanged, q.refused, q.tableFull, q.observerRefused, q.scrubbed, q.scrubMiss, q.checkCompared, q.checkSkipped, q.checkMiss, q.injected, ds.buckets - lastDs.buckets, ds.packets - lastDs.packets, ds.items - lastDs.items,
                     persistT_.items, tab.liveCount(), tab.peak(), tab.stats().writes, tab.stats().same, static_cast<double>(q.windowNs) / nv / 1000.0, static_cast<double>(q.sigNs) / nv / 1000.0,
                     static_cast<double>(q.deriveNs) / nv / 1000.0, static_cast<double>(q.observeNs) / nv / 1000.0, static_cast<double>(q.flushNs) / nv / 1000.0,
                     f2c4::enabled() ? "" : " (fine timers need RVK_F2C4_MEASURE=1)");
        {
            // F2c-4b: the bulk observers' pass lists (frame-ownership.md 7.23)
            static d3d9::Device::OwnedBulkStats lastBs;
            const d3d9::Device::OwnedBulkStats& bs = dev_.ownedBulkStats;
            if (bs.flushes != lastBs.flushes || bs.perDrawCovered != lastBs.perDrawCovered) {
                spdlog::info("native: owned bulk observers (5 s): {} flushes, {} records ({} entered, {} left), per-draw pre-filter covered {}, late capture requests {}; CPU per main view: bulk observers {:.1f} us{}",
                             bs.flushes - lastBs.flushes, bs.records - lastBs.records, bs.entered - lastBs.entered, bs.left - lastBs.left, bs.perDrawCovered - lastBs.perDrawCovered,
                             bs.lateCapture - lastBs.lateCapture, static_cast<double>(q.bulkNs) / nv / 1000.0, dev_.ownedBulkOn() ? "" : " (bulk off: RVK_F2C4B_PERDRAW)");
            }
            lastBs = bs;
        }
        q = {};
        lastDs = ds;
        if (miss != 0) ownedAutoOffMiss(miss);
    }
    ownedShadowReport();
    if (!ownedStatic_ && ownedW_.views == 0) return;
    OwnedStats& w = ownedW_;
    ownedT_.add(w);
    const d3d9::Device::OwnedReplayStats& d = dev_.ownedReplayStats;
    const d3d9::Device::OwnedReplayStats& l = ownedDevLast_;
    const std::uint64_t frames = std::max<std::uint64_t>(1, w.views);
    const auto list = [](const std::uint64_t* v, const char* const* names, std::size_t n) {
        std::string out;
        for (std::size_t i = 0; i < n; ++i) {
            if (v[i] != 0) out += std::format("{}{} {}", out.empty() ? "" : ", ", names[i], v[i]);
        }
        return out.empty() ? std::string("none") : out;
    };
    std::string refused;
    static constexpr const char* kWhy[] = {"-", "stale module", "phase", "consumers", "variant", "env", "binding", "texture", "jitter", "scene form", "scene off", "shaders", "no scope", "observer capture", "window not served", "guard", "no v2 pipeline / table", "view slot range", "view windows not the slot's", "view block differs"};
    for (std::size_t i = 1; i < std::size(kWhy); ++i) {
        if (d.why[i] != l.why[i]) refused += std::format("{}{} {}", refused.empty() ? "" : ", ", kWhy[i], d.why[i] - l.why[i]);
    }
    std::string left = list(w.leftExcl, orec::kExclNames, orec::kExclCount);
    {
        std::string stamp = list(w.leftStamp, orec::kStampNames, orec::kStampFields);
        if (stamp != "none") left += "; stamp: " + stamp;
        if (w.leftNoRecord != 0) left += std::format("; no record {}", w.leftNoRecord);
        if (w.leftNotOwnable != 0) left += std::format("; not ownable {}", w.leftNotOwnable);
        if (w.leftView != 0) left += std::format("; view stamp {}", w.leftView);
        if (w.leftScene != 0) left += std::format("; not a scene draw {}", w.leftScene);
        if (w.leftAudit != 0) left += std::format("; read log {}", w.leftAudit);
    }
    const double nv = static_cast<double>(frames);
    spdlog::info(
        "native: owned static (5 s): {} main views ({} compacted, {} check frames, {} warm-up, {} gated off), ops looked at {}, owned {} ({:.1f} %), drawn from records {} ops / {} draws, composed late {} ops / {} draws "
        "(fallbacks: {}), undrawn {}; left to the engine path: {}; replay refusals: {}; scene indirect calls {}; check frames DIFFERING {} draws; side effects: {} touches, {} lightmap binds, {} stream 3; "
        "CPU per main view: compaction {:.1f} us (of it the stamp / record checks {:.1f}), view + windows setup {:.1f} us, material polls + stamps {:.1f} us, build {:.1f} us (the rest: derivation inputs, windows, object records), side effects {:.1f} us, submit {:.1f} us (replayOwned incl. observers)",
        w.views, w.views - w.checkViews - w.warmupViews - w.gatedViews, w.checkViews, w.warmupViews, w.gatedViews, w.seen, w.owned, w.seen != 0 ? 100.0 * static_cast<double>(w.owned) / static_cast<double>(w.seen) : 0.0, w.drawnOps,
        w.draws, w.lateOps, w.lateDraws, list(w.fallback, kFallbackNames, kFbCount), w.undrawn, left, refused.empty() ? "none" : refused, dev_.sceneStats().indirectCalls - ownedIndirectLast_,
        recs_ ? recs_->total.differing : 0, w.touches, w.lightmaps, w.stream3, static_cast<double>(w.compactNs) / nv / 1000.0, static_cast<double>(w.selectNs) / nv / 1000.0, static_cast<double>(w.prepNs) / nv / 1000.0, static_cast<double>(w.matNs) / nv / 1000.0,
        static_cast<double>(w.buildNs) / nv / 1000.0, static_cast<double>(w.sideNs) / nv / 1000.0, static_cast<double>(w.submitNs) / nv / 1000.0);
    ownedIndirectLast_ = dev_.sceneStats().indirectCalls;
    ownedDevLast_ = d;
    w = {};
}

// F3-1 (7.36): the cascades' owned ops, 5 s
void StaticOps::ownedShadowReport() {
    OwnedStats& w = ownedShadowW_;
    PersistStats& q = persistShadowW_;
    if (w.views == 0 && q.passes == 0 && q.checkCompared == 0 && !(recs_ && recs_->shadowWindow.opsAll != 0)) {
        ownedShadowW_ = {};
        persistShadowW_ = {};
        return;
    }
    ownedShadowT_.add(w);
    persistShadowT_.add(q);
    const auto list = [](const std::uint64_t* v, const char* const* names, std::size_t n) {
        std::string out;
        for (std::size_t i = 0; i < n; ++i) {
            if (v[i] != 0) out += std::format("{}{} {}", out.empty() ? "" : ", ", names[i], v[i]);
        }
        return out.empty() ? std::string("none") : out;
    };
    std::string left = list(w.leftExcl, orec::kExclNames, orec::kExclCount);
    {
        std::string stamp = list(w.leftStamp, orec::kStampNames, orec::kStampFields);
        if (stamp != "none") left += "; stamp: " + stamp;
        if (w.leftNoRecord != 0) left += std::format("; no record {}", w.leftNoRecord);
        if (w.leftNotOwnable != 0) left += std::format("; not ownable {}", w.leftNotOwnable);
        if (w.leftScene != 0) left += std::format("; not a scene draw {}", w.leftScene);
    }
    std::uint64_t shadowRecs = 0, shadowDraws = 0;
    if (recs_) {
        for (const auto& kv : recs_->map) {
            if (kv.first.view == 0) continue;
            ++shadowRecs;
            shadowDraws += kv.second->draws.size();
        }
    }
    const double nv = static_cast<double>(std::max<std::uint64_t>(1, w.views));
    const std::uint64_t miss = q.scrubMiss + q.checkMiss;
    spdlog::info("native: owned shadows (5 s): {} cascade views ({} compacted, {} check frames, {} warm-up, {} gated off), ops looked at {}, owned {} ({:.1f} %), drawn from records {} ops / {} draws, composed late {} ops / {} draws "
                 "(fallbacks: {}), undrawn {}; left to the engine path: {}; persistent: {} passes ({} env epoch changes, {} flush failures, {} reserve failures), {} draws: reused {} ({:.1f} %), derived {} ({} rewrote the slot), refused {} "
                 "(table full {}, observer {}), rolling scrub {} draws MISS {}; check frames: {} compared, {} skipped, MISS {}; {} cascade records / {} draws; CPU per cascade view: compaction {:.1f} us, view + windows {:.1f} us, "
                 "material polls {:.1f} us, build {:.1f} us, side effects {:.1f} us, submit {:.1f} us, flush {:.1f} us",
                 w.views, w.views - w.checkViews - w.warmupViews - w.gatedViews, w.checkViews, w.warmupViews, w.gatedViews, w.seen, w.owned, w.seen != 0 ? 100.0 * static_cast<double>(w.owned) / static_cast<double>(w.seen) : 0.0, w.drawnOps,
                 w.draws, w.lateOps, w.lateDraws, list(w.fallback, kFallbackNames, kFbCount), w.undrawn, left, q.passes, q.envChanges, q.flushFailed, q.arenaFailed, q.draws, q.reused,
                 q.draws != 0 ? 100.0 * static_cast<double>(q.reused) / static_cast<double>(q.draws) : 0.0, q.derived + q.scrubbed, q.derivedChanged, q.refused, q.tableFull, q.observerRefused, q.scrubbed, q.scrubMiss,
                 q.checkCompared, q.checkSkipped, q.checkMiss, shadowRecs, shadowDraws, static_cast<double>(w.compactNs) / nv / 1000.0, static_cast<double>(w.prepNs) / nv / 1000.0, static_cast<double>(w.matNs) / nv / 1000.0,
                 static_cast<double>(w.buildNs) / nv / 1000.0, static_cast<double>(w.sideNs) / nv / 1000.0, static_cast<double>(w.submitNs) / nv / 1000.0, static_cast<double>(q.flushNs) / nv / 1000.0);
    if (recs_) {
        // the cascades' records age out: a cascade sees other ops as the camera moves, a record that was not looked at for 1,200 frames gives its slots back
        const std::uint64_t now = dev_.stats().frames;
        std::uint64_t swept = 0;
        for (auto it = recs_->map.begin(); it != recs_->map.end();) {
            if (it->first.view != 0 && it->second->lastSeen + 1200 < now && it->second->builtFrame + 1200 < now) {
                it = recs_->map.erase(it);
                ++swept;
            } else {
                ++it;
            }
        }
        if (swept != 0) spdlog::info("native: owned shadows: {} cascade records not seen for 1200 frames dropped", swept);
    }
    ownedShadowW_ = {};
    persistShadowW_ = {};
    if (miss != 0) ownedShadowsOff("MISS (a cascade's persistent draw judged valid differs from a fresh derivation)", miss);
}

}  // namespace rvk::native
