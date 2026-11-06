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

bool StaticOps::ownedViewBegin(void* /*db*/, void* desc) {
    OwnedStats& W = ownedW_;
    for (auto& v : ownedOps_) {
        W.undrawn += v.size();  // (ops of an earlier view whose pass never began: lost for that frame)
        v.clear();
    }
    ownedView_ = {};
    if (!ownedStatic_) return false;
    ++W.views;
    if (f2c4::enabled()) {
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
    const sc::LeanFrame kind = sc::leanFrameKind(frame, ownedCheck_);
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
    std::uint8_t caps = 0;
    if (list == List::ZPass) return readAt(db, 0x1590, caps) && caps != 0;  // (the z pre-pass runs: BeginZPass brackets the list)
    return readAt(db, 0x15a0, caps) && caps == 0;                          // (DoBeginRenderSolid is called)
}

bool StaticOps::ownedSelect(const Op& op) {
    OwnedStats& W = ownedW_;
    ++W.seen;
    if (f2c4::enabled()) {
        // F2c-4-0 item 4: the context's LOD resource (ctx + 0x28) and the set of its ops' descs of this frame
        f2c4::Counters::Ctx& e = f2c4::counters().ctxCur[op.raw.context];
        e.res = get<U8*>(op.raw.context, 0x28, nullptr);
        e.descSet += f2c4::mix64(op.raw.desc, static_cast<std::uint64_t>(op.list) + 1);
        ++e.ops;
    }
    struct T {
        std::uint64_t& ns;
        Clock::time_point t0 = Clock::now();
        ~T() { ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()); }
    } selTimer{W.selectNs};
    const orec::Key key{op.raw.context, op.raw.desc, static_cast<std::uint8_t>(op.list), 0};
    const auto it = recs_->map.find(key);
    if (it == recs_->map.end()) {
        // (not a candidate: the first reason it is not eligible, for the counters)
        const orec::Excl ex = recExclusion(op);
        if (ex != orec::Excl::Count) ++W.leftExcl[static_cast<std::size_t>(ex)];
        else ++W.leftNoRecord;
        return false;
    }
    const orec::OpRecord& rec = *it->second;
    // a record exists only for an eligible op; of recExclusion's reasons the stamps hold the overlay flags, the fade, the light list, the shader's class / sort id /
    // depth bias; what they do not: the hierarchy flag of the context and the table lighting
    if (op.list == List::Solid && !tableLit_) {
        ++W.leftExcl[static_cast<std::size_t>(orec::Excl::NotTableLit)];
        return false;
    }
    if (get<std::int32_t>(op.raw.context, 0x34) > 0) {
        ++W.leftExcl[static_cast<std::size_t>(orec::Excl::Hierarchy)];
        return false;
    }
    if (!rec.ownable) {
        ++W.leftNotOwnable;
        return false;
    }
    orec::Stamp st;
    if (!recStamp(op, st)) {
        ++W.leftNoRecord;
        return false;
    }
    orec::Stamp a = rec.stamp;
    a.view = a.materials = 0;
    if (const int d = orec::firstDifference(a, st); d >= 0) {
        ++W.leftStamp[static_cast<std::size_t>(d)];
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
    ownedW_.owned += ops.size();
    ownedW_.compactNs += compactNs;
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
    const auto tm = Clock::now();
    struct Guard {
        std::uint64_t& ns;
        Clock::time_point t0;
        ~Guard() { ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()); }
    } guard{ownedW_.matNs, tm};
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
    OwnedStats& W = ownedW_;
    struct Guard {
        std::uint64_t& ns;
        Clock::time_point t0 = Clock::now();
        ~Guard() { ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()); }
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
    OwnedStats& W = ownedW_;
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
    float world[16];
    if (entity == nullptr || !engine::safeRead(entity + 0x60, world, sizeof(world))) {
        why = kFbObject;
        return false;
    }
    // the object record: the entity's matrix as the signals hold it (the draw's composed rows would be these), the fade 1 of a z draw / the op's
    float composed[gpuscene::kObjectRegs][4] = {};
    composed[1][3] = op.list == List::ZPass ? 1.0f : op.raw.fade;
    {
        float rows[3][4];
        gpuscene::objectWorldRows(world, rows);
        std::memcpy(composed[2], rows, sizeof(rows));
    }
    d3d9::OwnedWindows::Win objWin;
    if (gpuscene::objecttable().serve(entity, composed, 0x1eu, objWin) != gpuscene::ObjectWhy::Served) {
        why = kFbObject;
        return false;
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
        const auto ts = Clock::now();
        const int rc = dev_.replayOwned(cd, in, dw);
        W.submitNs += ns(ts, Clock::now());
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
    OwnedStats& W = ownedW_;
    if (!inView_ || viewKind_ != ViewKind::Main) {
        W.undrawn += ops.size();
        ops.clear();
        return 0;
    }
    const auto t0 = Clock::now();
    const std::uint64_t submit0 = W.submitNs, side0 = W.sideNs, mat0 = W.matNs;
    ret::gLog = nullptr;
    finishOp();
    leanAbort();
    // the view as the first op's composition would have made it
    bool viewOk = view_.valid;
    if (!viewOk && ownedView_.admin != nullptr) viewOk = composeView(ownedView_.admin);
    const auto tPrep = Clock::now();
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
            if (f2c4::enabled()) {
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
            const std::uint32_t slot = solid ? shader::abi::kViewSlotMainSolid : shader::abi::kViewSlotMainZ;
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
    W.prepNs += ns(tPrep, Clock::now());
    ownedAmbient_.clear();
    ownedCache_.mat.clear();
    ownedCache_.light.clear();
    std::uint32_t drawn = 0;
    dev_.setSceneIssue(true);
    for (Op& op : ops) {
        const orec::Key key{op.raw.context, op.raw.desc, static_cast<std::uint8_t>(op.list), 0};
        const orec::OpRecord* recPtr = nullptr;
        if (recs_) {
            const auto it = recs_->map.find(key);
            if (it != recs_->map.end()) recPtr = it->second.get();
        }
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
                done = ownedDrawOp(op, rec, viewWin, viewWinOk, why);
                if (done) {
                    ownedSideEffects(rec);
                    ++W.drawnOps;
                    W.draws += rec.draws.size();
                    drawn += static_cast<std::uint32_t>(rec.draws.size());
                }
            }
        }
        if (!done) {
            ++W.fallback[why];
            // composed now, where the engine's slot would have run it (the record is rebuilt from this composition)
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
        }
    }
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
    if (f2c4::enabled()) {
        f2c4::Counters& m = f2c4::counters();
        const std::size_t k = solid ? 1 : 0;
        m.maxDrawsPerPass[k] = std::max<std::uint64_t>(m.maxDrawsPerPass[k], drawn);
        m.maxOpsPerPass[k] = std::max<std::uint64_t>(m.maxOpsPerPass[k], opsInPass);
    }
    const std::uint64_t total = ns(t0, Clock::now());
    const std::uint64_t sub = (W.submitNs - submit0) + (W.sideNs - side0) + (W.matNs - mat0);
    W.buildNs += total > sub ? total - sub : 0;
    return drawn;
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

}  // namespace rvk::native
