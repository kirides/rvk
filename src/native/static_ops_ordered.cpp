// F9 + F14 ordered owned draws (docs/research/frame-ownership.md 14.20, setting native.owned_ordered): see static_ops.h.
//
// The persistent owned path of F2c-4a draws the opaque static ops at the start of their pass, grouped by bucket: their order does not matter (they write depth and are
// tested against it). The order-dependent ops cannot go that way: the solid list's blended terrain layers (they blend over the base terrain at equal depth) and its
// no-z-write draws, and every mesh op of the alpha / refracted lists (sorted back to front by the engine each frame). For them:
//   recording  their compositions build op records like the static ops' (recEnter / recComposed / recEnd: `OpRecord::ordered`), the draws get their scene forms although they
//              are not batchable (Device::setSceneOrdered: CachedDraw::ordered)
//   drawing    on an owned frame the op stays in the engine's list (never compacted) and its slot is entered at the engine's place; orderedPlace checks the record (the
//              stamps, as ownedSelect does), derives / reuses its persistent DrawRecords (ownedDrawOpPersistent) and queues them with an ORDERED ownedFlush (one indirect
//              command per run of consecutive equal buckets, recorded after the pending batch); the slot is skipped and its side effects run (ownedSideEffects).
//              The engine's own sort result is the order: nothing is re-sorted.
//   checking   check / warm-up frames compose the op as before; recDraw compares the derivation with the composed draw (DIFFERING), ownedMissCheck the persistent content (MISS)
// Observers see an ordered draw per draw (the bulk contract of F2c-4b is plain opaque static draws only); a draw an observer wants a capture of is composed.
// The view: the solid list's ops read view slot 1 (the solid pass's, the same registers), the refracted / alpha ops slots 16 / 17 (set once per view and frame).

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "d3d9/defs.h"
#include "engine/memread.h"
#include "gpuscene/draw_record_table.h"
#include "gpuscene/view_table.h"
#include "gpuscene/window_tables.h"
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

// why a recorded ordered op was composed instead (OrderedStats::why)
enum OrdWhy : int { kOwNotTableLit, kOwNotOwnable, kOwStamp, kOwLive, kOwNotScene, kOwView, kOwViewStamp, kOwDraw, kOwFlush, kOwScope, kOwArena, kOwCount };
constexpr const char* kOrdWhyNames[] = {"not table-lit", "not ownable", "stamp", "illumination slot / load", "not a scene draw", "view windows", "view stamp", "a draw refused (derive / observer / table)",
                                        "flush refused", "no rendering scope / env", "arena"};
static_assert(std::size(kOrdWhyNames) == kOwCount);
constexpr const char* kListNames[] = {"solid", "refracted", "alpha"};

// the view table's slots of the ordered passes (the solid list's ops share the solid pass's slot 1): every v2 view slot below 16 is taken (trees 8..11 / 14 / 15, vegetation 12 / 13)
constexpr std::uint32_t kViewSlotRefracted = 16, kViewSlotAlpha = 17;
// ... and their depth-bias view-projection twins (c0..c3 = dbvp; the solid list's is slot 2, kViewSlotDbvp0)
constexpr std::uint32_t kViewSlotRefractedDb = 18, kViewSlotAlphaDb = 19;
constexpr std::uint32_t kRunCap = 256;  // draws per pending run (the ref list block reserved at its start)

void orderedHookFn(void* ctx) { static_cast<StaticOps*>(ctx)->orderedFlushRun(); }

}  // namespace

void StaticOps::setOwnedOrdered(bool on) {
    if (on == ownedOrdered_) return;
    orderedFlushRun();
    ownedOrdered_ = on;
    ordPass_ = {};
    dev_.setOrderedHook(on ? &orderedHookFn : nullptr, this);
    if (recs_ && !on) {
        // (the ordered records are dropped: nothing draws them any more)
        for (auto it = recs_->map.begin(); it != recs_->map.end();) {
            if (it->second->ordered) it = recs_->map.erase(it);
            else ++it;
        }
    }
    spdlog::info("native: owned ordered {}", on ? "on (the solid list's blended / no-z-write ops and the alpha / refracted lists' mesh ops drawn from records at their place)" : "off");
}

bool StaticOps::orderedGroupNow(const Op* op) const {
    return op != nullptr && ownedOrderedOn() && viewKind_ == ViewKind::Main && !inNonOp_ && op->kind == OpKind::Mesh && (op->list == List::Alpha || op->list == List::Refracted) &&
           g_ == &groups_[static_cast<std::size_t>(Group::Mesh)];
}

// the record holds for the op now (ownedSelect's checks, without its counters): -1, else an OrdWhy
int StaticOps::orderedValid(const Op& op, const orec::OpRecord& rec) {
    rec.lastSeen = dev_.stats().frames;
    if (!tableLit_) return kOwNotTableLit;
    if (!rec.ownable) return kOwNotOwnable;
    // F2e-1: a record no engine event touched is valid on its polled inputs alone; a marked one (or 1 in 16 of the rolling scrub) runs the full stamp
    const bool events = depEvents() && !rec.instances;  // (F2f: an instance list changes without an engine event: its full stamp every frame)
    int fast = -2;
    if (events) fast = depFast(op, rec);
    if (fast >= 0) return kOwStamp;
    static std::uint32_t scrubTick = 0;
    const bool scrub = events && fast == -1 && (++scrubTick & 15u) == 0;
    if (events && fast == -1 && !scrub) {
        ++depW_.fast;
    } else {
        if (scrub) ++depW_.scrubbed;
        else if (events) ++depW_.slow;
        orec::Stamp st;
        if (!recStamp(op, st)) return kOwStamp;
        orec::Stamp a = rec.stamp;
        a.view = a.materials = 0;
        if (const int d = orec::firstDifference(a, st); d >= 0) {
            if (scrub) depEventMiss(op, rec, d, "rolling scrub (ordered)");
            return kOwStamp;
        }
        if (events && !scrub) {
            ++depW_.slowClean;
            depVerified(rec);
        }
    }
    if (recLiveWhy(rec) >= 0) return kOwLive;
    for (const orec::DrawRec& r : rec.draws) {
        if (!r.scene || r.cd == nullptr || !r.cd->valid) return kOwNotScene;
    }
    return -1;
}

// once per list and view: the view (composed if no op of the view composed it yet), the view table's slot of the list, its windows
bool StaticOps::orderedPassBegin(const Op& op, const orec::OpRecord& rec) {
    const std::uint64_t frame = dev_.stats().frames;
    if (ordPass_.frame == frame && ordPass_.viewSerial == recViewSerial_ && ordPass_.list == static_cast<std::uint8_t>(op.list)) return ordPass_.ok;
    orderedFlushRun();  // (a run of the previous segment)
    ordPass_ = {};
    ordPass_.serial = ++ordSerial_;
    ordPass_.frame = frame;
    ordPass_.viewSerial = recViewSerial_;
    ordPass_.list = static_cast<std::uint8_t>(op.list);
    ++ordW_.passes;
    if (!view_.valid) {
        U8* admin = ptr(rec.anchors[7], 0x220);  // (the shader's material admin: the view's)
        if (admin == nullptr || !composeView(admin)) return false;
    }
    initMirror();
    std::memcpy(mirror_.adminVs[0], view_.vp, 64);  // (no depth-bias material is recorded: c0..c3 = the view-projection; every composition sets its own c0..c3 again)
    gpuscene::ViewTable& vt = gpuscene::viewtable();
    if (!vt.active() || !vt.ready() || !dev_.sceneViewsReady() || !gpuscene::drawrecords().ready()) return false;
    const bool solid = op.list == List::Solid;
    ordPass_.slot[0] = solid ? shader::abi::kViewSlotMainSolid : op.list == List::Refracted ? kViewSlotRefracted : kViewSlotAlpha;
    ordPass_.slot[1] = solid ? shader::abi::kViewSlotDbvp0 : op.list == List::Refracted ? kViewSlotRefractedDb : kViewSlotAlphaDb;
    ordPass_.envIndex = d3d9::Device::kOwnedEnvOrdered + (solid ? 0u : op.list == List::Refracted ? 1u : 2u);
    float posFix[4];
    dev_.viewPosFix(posFix);
    gpuscene::ViewTable::Regs regs;
    regs.vs0 = mirror_.adminVs;
    regs.vs1 = mirror_.adminVs + 12;
    regs.ps0 = mirror_.adminPs;
    regs.posFix = posFix;
    regs.c5 = mirror_.adminPs[5];
    ordPass_.ok = vt.set(ordPass_.slot[0], regs) && vt.fillWindows(ordPass_.slot[0], ordPass_.viewWin[0]);
    {
        // the depth-bias twin: the same registers with c0..c3 = the depth-bias view-projection (what composeSolid writes for a depth-bias material)
        static float dbVs[12][4];
        std::memcpy(dbVs, mirror_.adminVs, sizeof(dbVs));
        std::memcpy(dbVs[0], view_.dbvp, sizeof(view_.dbvp));
        gpuscene::ViewTable::Regs db = regs;
        db.vs0 = dbVs;
        ordPass_.okDb = ordPass_.ok && view_.dbOrder >= 0 && vt.set(ordPass_.slot[1], db) && vt.fillWindows(ordPass_.slot[1], ordPass_.viewWin[1]);
    }
    // (the per-pass memos of the windows / lighting / ambient tests: this list is a pass of its own)
    ownedAmbient_.clear();
    ownedCache_.mat.clear();
    ownedCache_.light.clear();
    ++ownedPassSerial_;
    return ordPass_.ok;
}

bool StaticOps::orderedPlace(const Op& op) {
    if (viewKind_ != ViewKind::Main || op.kind != OpKind::Mesh || inNonOp_ || !recs_ || !ownedOrderedOn()) return false;
    const bool solid = op.list == List::Solid;
    if (solid ? g_ != &groups_[static_cast<std::size_t>(Group::Static)]
              : (g_ != &groups_[static_cast<std::size_t>(Group::Mesh)] || (op.list != List::Alpha && op.list != List::Refracted))) {
        return false;
    }
    if (!ownedView_.active) return false;  // (a check / warm-up frame, or the owned path is not ready: composed, and compared)
    const std::size_t li = solid ? 0 : op.list == List::Refracted ? 1 : 2;
    const auto it = recs_->map.find(orec::Key{op.raw.context, op.raw.desc, static_cast<std::uint8_t>(op.list), 0});
    if (it == recs_->map.end()) {
        if (!solid) {
            ++ordW_.seen[li];
            ++ordW_.noRecord[li];
        }
        return false;
    }
    const orec::OpRecord& rec = *it->second;
    if (!rec.ordered) {
        // (a solid list op with an opaque record that the compaction did not take: its stamp / view moved; composed as before)
        if (!solid) ++ordW_.notOrdered[li];
        return false;
    }
    ++ordW_.seen[li];
    const auto t0 = Clock::now();
    struct Timer {
        std::uint64_t& ns;
        Clock::time_point t0;
        ~Timer() { ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()); }
    } timer{ordW_.ns, t0};
    const auto refuse = [&](int why) {
        ++ordW_.why[li][why];
        ++ordW_.composed[li];
        return false;
    };
    const auto lap = [&](std::uint64_t& acc, Clock::time_point& from) {
        const auto now = Clock::now();
        acc += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - from).count());
        from = now;
    };
    auto tl = t0;
    const int valid = orderedValid(op, rec);
    lap(ordW_.validNs, tl);
    if (valid >= 0) return refuse(valid);
    if (!orderedPassBegin(op, rec) || (rec.dbvp && !ordPass_.okDb)) return refuse(kOwView);
    const std::size_t vw = rec.dbvp ? 1 : 0;
    const std::uint32_t slot = ordPass_.slot[vw];
    if (rec.stamp.view != recViewStamp()) return refuse(kOwViewStamp);
    // the device state to give back: shaders, tag; the register file only when an observer loaded a draw's windows into it (Device::ownedRegsArm)
    const d3d9::ShaderRef vsSaved = dev_.vertexShaderRef(), psSaved = dev_.pixelShaderRef();
    d3d9::draws::Tag& tag = dev_.drawTag();
    const d3d9::draws::Tag savedTag = tag;
    const auto restore = [&] {
        tag = savedTag;
        dev_.setVertexShaderRef(vsSaved);
        dev_.setPixelShaderRef(psSaved);
    };
    // the pending run (consecutive ordered ops of one list segment: one ordered flush for all of them); a run of another segment or a full one is queued first
    const std::size_t need = rec.draws.size();
    if (ordRun_.active && (ordRun_.serial != ordPass_.serial || ordRun_.slot != slot || ordItems_.size() + need > ordArena_.capacity)) orderedFlushRun();
    if (!ordRun_.active) {
        dev_.setSceneIssue(true);
        bool ok = dev_.ownedBeginScope();
        const std::uint64_t epoch = ok ? dev_.ownedEnvEpochAt(ordPass_.envIndex, true, slot, ok) : 0;
        dev_.setSceneIssue(false);
        if (!ok) return refuse(kOwScope);
        if (!dev_.ownedReserve(std::max<std::uint32_t>(kRunCap, static_cast<std::uint32_t>(need)), ordArena_)) return refuse(kOwArena);
        ordRun_.active = true;
        ordRun_.serial = ordPass_.serial;
        ordRun_.slot = slot;
        ordRun_.epoch = epoch;
        ordItems_.clear();
        ++ordW_.runs;
    }
    tag.source = d3d9::draws::Source::Mesh;
    tag.context = op.raw.context;
    tag.entity = op.entity != nullptr ? op.entity : ptr(op.raw.context, 0x10);
    tag.opDesc = op.raw.desc;
    tag.subMesh = tag.material = tag.meshResource = nullptr;
    tag.water = d3d9::draws::Water::None;
    tag.materialName = nullptr;
    dev_.setSceneIssue(true);  // (ownedDerive's view check)
    dev_.setOwnedBulkSuspended(true);
    dev_.ownedRegsArm();
    persistEnvIndex_ = static_cast<std::int32_t>(ordPass_.envIndex);
    int why = 0;
    static std::uint32_t scrubPhase = 0;
    scrubPhase = (scrubPhase + 7) & 15u;
    std::uint64_t devWhy0[24];
    std::memcpy(devWhy0, dev_.ownedReplayStats.why, sizeof(devWhy0));
    const std::uint64_t obs0 = persistStats().observerRefused, full0 = persistStats().tableFull;
    const std::size_t before = ordItems_.size();
    ownedItems_.swap(ordItems_);  // (ownedDrawOpPersistent appends to ownedItems_: here the run's list)
    lap(ordW_.prepNs, tl);
    bool done = ownedDrawOpPersistent(op, rec, ordPass_.viewWin[vw], ordRun_.slot, ordRun_.epoch, 16, scrubPhase, why);
    lap(ordW_.drawNs, tl);
    ownedItems_.swap(ordItems_);
    persistEnvIndex_ = -1;
    dev_.setOwnedBulkSuspended(false);
    dev_.setSceneIssue(false);
    dev_.ownedRegsRestore();
    restore();
    if (!done) {
        ordItems_.resize(before);
        ++ordW_.drawWhy[li][std::min(why, 3)];
        for (std::size_t i = 0; i < 24; ++i) ordW_.devWhy[li][i] += dev_.ownedReplayStats.why[i] - devWhy0[i];
        ordW_.observer[li] += persistStats().observerRefused - obs0;
        ordW_.tableFull[li] += persistStats().tableFull - full0;
        return refuse(kOwDraw);
    }
    {
        // GPU safety: the flush's own guards, per item now (a refusal at the run's flush would lose ops whose slots were skipped): a written record with a block address,
        // window 2 / PS window 3 bases, a v2 pipeline
        const shader::abi::DrawRecord* shadow = gpuscene::drawrecords().recordsShadow();
        for (std::size_t i = before; i < ordItems_.size(); ++i) {
            const d3d9::Device::OwnedItem& it = ordItems_[i];
            if (shadow == nullptr || it.cd == nullptr || it.slot >= gpuscene::DrawRecordTable::kCapacity || shadow[it.slot].draw == 0 || shadow[it.slot].vsWin[2] == 0 ||
                shadow[it.slot].psWin[3] == 0 || it.cd->scenePipeline2 == d3d9::GamePipeline::None) {
                ordItems_.resize(before);
                return refuse(kOwFlush);
            }
        }
    }
    const std::size_t drawn = ordItems_.size() - before;
    dev_.setOrderedPending(true);  // (anything else the device records queues the run first)
    // the engine's slot is skipped: its side effects at the op's place (BindLightmaps, the touches)
    ownedSideEffects(rec);
    lap(ordW_.sideNs, tl);
    ++ordW_.owned[li];
    ++ordTotalOwned_;
    ordW_.draws[li] += drawn;
    if (solid) {
        // (the owned share of the main view's static ops counts it: the op was looked at by the compaction and left there as ordered)
        OwnedStats& W = ownedStats();
        ++W.owned;
        ++W.drawnOps;
        W.draws += drawn;
    }
    ++g_->window.opsComposed;
    g_->issuedDraws += drawn;
    ++g_->issuedOps;
    op_ = &op;
    engineDraws_ = 0;
    opDiffLogged_ = false;
    taken_ = true;
    drawCount_ = 0;
    leftCode_ = static_cast<int>(Skip::None);
    slotSkipped_ = true;
    return true;
}

// the pending run as one ordered flush (Device::setOrderedHook: before anything else the device records; StaticOps::viewEnd; a run of another list segment)
void StaticOps::orderedFlushRun() {
    dev_.setOrderedPending(false);
    if (!ordRun_.active) return;
    ordRun_.active = false;
    if (ordItems_.empty()) return;
    const auto t0 = Clock::now();
    const std::uint64_t buckets0 = dev_.ownedPersistStats.buckets;
    const bool ok = dev_.ownedFlush(ordItems_, ordArena_, ordRun_.slot, gpuscene::drawrecords().recordsRef(), gpuscene::drawrecords().recordsShadow(), gpuscene::DrawRecordTable::kCapacity, true);
    if (ok) {
        ++ordW_.flushes;
        ordW_.buckets += dev_.ownedPersistStats.buckets - buckets0;
    } else {
        // (the guards were checked per op: not expected; the run's draws are missing from this frame)
        ++ordW_.flushFailed;
        ordW_.lostDraws += ordItems_.size();
        static int logged = 0;
        if (logged < 8) {
            ++logged;
            spdlog::warn("native: owned ordered: a run of {} draws was refused at its flush (frame {}): missing from this frame", ordItems_.size(), dev_.stats().frames);
        }
    }
    ordItems_.clear();
    ordW_.flushNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
}

void StaticOps::orderedReport() {
    OrderedStats& w = ordW_;
    if (!ownedOrdered_ && w.seen[0] + w.seen[1] + w.seen[2] == 0) return;
    std::string lists;
    for (std::size_t l = 0; l < 3; ++l) {
        std::string why;
        for (int k = 0; k < kOwCount; ++k) {
            if (w.why[l][k] != 0) why += std::format("{}{} {}", why.empty() ? "" : ", ", kOrdWhyNames[k], w.why[l][k]);
        }
        std::string ex;
        for (std::size_t k = 0; k < orec::kExclCount; ++k) {
            if (w.excluded[l][k] != 0) ex += std::format("{}{} {}", ex.empty() ? "" : ", ", orec::kExclNames[k], w.excluded[l][k]);
        }
        static constexpr const char* kDrawWhy[] = {"view", "materials", "object window", "derive / signature"};
        static constexpr const char* kDev[] = {"-", "stale module", "phase", "consumers", "variant", "env", "binding", "texture", "jitter", "scene form", "scene off", "shaders", "no scope",
                                               "observer capture", "window not served", "guard", "no v2 pipeline / table", "view slot range", "view windows not the slot's", "view block differs"};
        for (std::size_t k = 0; k < 4; ++k) {
            if (w.drawWhy[l][k] != 0) why += std::format(" [{} {}]", kDrawWhy[k], w.drawWhy[l][k]);
        }
        for (std::size_t k = 1; k < std::size(kDev); ++k) {
            if (w.devWhy[l][k] != 0) why += std::format(" <{} {}>", kDev[k], w.devWhy[l][k]);
        }
        if (w.observer[l] != 0) why += std::format(" <observer wanted a capture {}>", w.observer[l]);
        if (w.tableFull[l] != 0) why += std::format(" <record table full {}>", w.tableFull[l]);
        if (w.notRetained[l] != 0) why += std::format("; not retained (no packet: water / replaced) {}", w.notRetained[l]);
        const std::uint64_t looked = w.seen[l];
        lists += std::format("{}{}: {} ops looked at, owned {} ({:.1f} %) / {} draws, composed {} ({}), no record {}, opaque record {}, records built {}; not recorded: {}", lists.empty() ? "" : "; ", kListNames[l],
                             looked, w.owned[l], looked != 0 ? 100.0 * static_cast<double>(w.owned[l]) / static_cast<double>(looked) : 0.0, w.draws[l], w.composed[l], why.empty() ? "-" : why,
                             w.noRecord[l], w.notOrdered[l], w.recordsBuilt[l], ex.empty() ? "none" : ex);
    }
    const double nv = static_cast<double>(std::max<std::uint64_t>(1, ownedW_.views));
    spdlog::info("native: owned ordered (5 s): {}; {} runs, {} ordered flushes ({} buckets = indirect commands, {} refused, {} draws lost), {} list segments; CPU per main view {:.1f} us at the ops (stamps {:.1f}, setup {:.1f}, draws {:.1f}, side effects {:.1f}) + {:.1f} us in the run flushes", lists, w.runs, w.flushes,
                 w.buckets, w.flushFailed, w.lostDraws, w.passes,
                 static_cast<double>(w.ns) / nv / 1000.0, static_cast<double>(w.validNs) / nv / 1000.0, static_cast<double>(w.prepNs) / nv / 1000.0, static_cast<double>(w.drawNs) / nv / 1000.0,
                 static_cast<double>(w.sideNs) / nv / 1000.0, static_cast<double>(w.flushNs) / nv / 1000.0);
    w = {};
}

}  // namespace rvk::native
