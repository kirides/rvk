#include "native/frame_tap.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "engine/memread.h"
#include "engine/symbols.h"
#include "hal/vulkan/hal_vulkan.h"
#include "native/static_ops.h"

namespace rvk::native {

namespace {

using engine::readAt;
using engine::safeRead;

constexpr std::uint32_t kMaxListOps = 1u << 16;  // a sanity bound on a list's count

// Execute (0x66e990) qsorts three lists; each site is (B-frustum-db.md 2.6, disassembled):
//   mov rcx, [rdi + list + 8]   48 8b 8f <disp32>          site - 7
//   lea r9, [rip + comparator]  4c 8d 0d <rel32>           site
//   mov rdx, rax                48 8b d0                   site + 7
//   mov r8d, 0x18               41 b8 18 00 00 00          site + 10
//   call qsort                  e8 <rel32>                 site + 16
struct SortSite {
    std::uint32_t lea;  // offset of the lea in Execute
    std::uint32_t comparatorRva;
    List list;
    std::array<std::uint8_t, 10> comparatorPrologue;
};
constexpr std::uint32_t kExecuteRva = 0x66e990;
constexpr std::uint32_t kQsortRva = 0x4a4aa0;
constexpr SortSite kSortSites[] = {
    {0x66ec51 - kExecuteRva, 0x66e670, List::ZPass, {0x44, 0x8b, 0x41, 0x10, 0x8b, 0x4a, 0x10, 0x44, 0x3b, 0xc1}},
    {0x66edbe - kExecuteRva, 0x66e6a0, List::Solid, {0x4c, 0x8b, 0x41, 0x10, 0x48, 0x8b, 0x4a, 0x10, 0x4c, 0x3b}},
    {0x66f306 - kExecuteRva, 0x66e600, List::Alpha, {0x44, 0x8b, 0x51, 0x14, 0x45, 0x8b, 0xca, 0x41, 0x81, 0xe1}},
};
// the CRT qsort statically linked into Engine.dll (Engine.dll imports no CRT): push r12, r13, r14; sub rsp, 0x450
constexpr std::array<std::uint8_t, 13> kQsortPrologue{0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x81, 0xec, 0x50, 0x04, 0x00, 0x00};

std::int32_t rel32(const std::uint8_t* p) {
    std::int32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

// render contexts whose class exports no vtable (D-contexts.md 0.1): vtable RVA, the RequestRenderOps slot (+0x40) it must hold
struct UnexportedContext {
    std::uint32_t vtableRva, requestRva;
    OpKind kind;
};
constexpr UnexportedContext kUnexported[] = {
    {0x9b7180, 0x5d33a0, OpKind::Particle},     {0x9e2448, 0x701170, OpKind::Strip},
    {0x9abd10, 0x5c1880, OpKind::LightStreaks}, {0x982810, 0x4f1400, OpKind::Billboard},
    {0x9e7a18, 0x719b40, OpKind::Precipitation}, {0x9ddd90, 0x6e8a30, OpKind::Skydome},
};

// an engine function that does nothing: `ret` / `ret 0` (eCRenderContext's empty slots 0x7faa0, the sky's RenderAlpha 0x6e8a00)
bool emptyFunction(const void* fn) {
    std::uint8_t b[3] = {};
    if (fn == nullptr || !safeRead(fn, b, sizeof(b))) return false;
    return b[0] == 0xc3 || (b[0] == 0xc2 && b[1] == 0 && b[2] == 0);
}

}  // namespace

FrameTap::FrameTap(d3d9::Device& device) : dev_(device) {}

FrameTap::~FrameTap() { apply(false); }

void FrameTap::apply(bool on) {
    wanted_ = on;
    update();
}

void FrameTap::setStaticOpsActive(bool on) {
    staticsWanted_ = on;
    update();
}

void FrameTap::update() {
    const bool on = wanted_ || staticsWanted_;
    if (on == on_) return;
    on_ = on;
    if (on) {
        engine::scene::setOpTap(this);
        dev_.setDrawTap(this);
        window_ = {};
        windowMismatches_.clear();
        spdlog::info("native: frame tap on (the main view's render ops at Execute, attribution of its draws)");
    } else {
        engine::scene::setOpTap(nullptr);
        dev_.setDrawTap(nullptr);
        executing_ = false;
        attr_.begin(nullptr);
        spdlog::info("native: frame tap off");
    }
}

bool FrameTap::verifySortSites() {
    const auto* execute = static_cast<const std::uint8_t*>(engine::sym().frustumDbExecute);
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(L"Engine.dll"));
    if (execute == nullptr || base == nullptr) {
        spdlog::warn("native: frame tap: eCFrustumDatabase::Execute / Engine.dll not found: the tap stays idle");
        return false;
    }
    engineBase_ = base;
    if (execute != base + kExecuteRva) {
        spdlog::warn("native: frame tap: Execute at RVA {:#x}, expected {:#x}: the tap stays idle", execute - base, kExecuteRva);
        return false;
    }
    const std::uint8_t* qsortAddr = nullptr;
    for (const SortSite& s : kSortSites) {
        std::array<std::uint8_t, 7 + 21> b{};
        const std::uint8_t* at = execute + s.lea - 7;
        if (!safeRead(at, b.data(), b.size())) return false;
        const std::uint32_t disp = static_cast<std::uint32_t>(rel32(&b[3]));
        const bool loadOk = b[0] == 0x48 && b[1] == 0x8b && b[2] == 0x8f && disp == kListOffset[static_cast<std::size_t>(s.list)] + 8;
        const bool leaOk = b[7] == 0x4c && b[8] == 0x8d && b[9] == 0x0d;
        const bool movOk = b[14] == 0x48 && b[15] == 0x8b && b[16] == 0xd0 && b[17] == 0x41 && b[18] == 0xb8 && b[19] == 0x18 &&
                           b[20] == 0 && b[21] == 0 && b[22] == 0;
        const bool callOk = b[23] == 0xe8;
        if (!loadOk || !leaOk || !movOk || !callOk) {
            spdlog::warn("native: frame tap: the qsort site of the {} list (Execute +{:#x}) has unexpected bytes: the tap stays idle",
                         name(s.list), s.lea);
            return false;
        }
        const std::uint8_t* cmp = execute + s.lea + 7 + rel32(&b[10]);
        const std::uint8_t* q = execute + s.lea + 16 + 5 + rel32(&b[24]);
        std::array<std::uint8_t, 10> prologue{};
        if (cmp != base + s.comparatorRva || !safeRead(cmp, prologue.data(), prologue.size()) || prologue != s.comparatorPrologue) {
            spdlog::warn("native: frame tap: the {} list's comparator is at RVA {:#x} (expected {:#x}) or differs: the tap stays idle",
                         name(s.list), cmp - base, s.comparatorRva);
            return false;
        }
        if (qsortAddr != nullptr && q != qsortAddr) {
            spdlog::warn("native: frame tap: the qsort sites call different functions: the tap stays idle");
            return false;
        }
        qsortAddr = q;
        compare_[static_cast<std::size_t>(s.list)] = reinterpret_cast<CompareFn>(const_cast<std::uint8_t*>(cmp));
    }
    std::array<std::uint8_t, kQsortPrologue.size()> qp{};
    if (qsortAddr != base + kQsortRva || !safeRead(qsortAddr, qp.data(), qp.size()) || qp != kQsortPrologue) {
        spdlog::warn("native: frame tap: Execute's qsort is at RVA {:#x} (expected {:#x}) or differs: the tap stays idle", qsortAddr - base,
                     kQsortRva);
        return false;
    }
    qsort_ = reinterpret_cast<QsortFn>(const_cast<std::uint8_t*>(qsortAddr));
    spdlog::info("native: frame tap: Execute's sorts verified (qsort RVA {:#x}, comparators z {:#x}, solid {:#x}, alpha {:#x}): the copies "
                 "are sorted by the same functions",
                 kQsortRva, kSortSites[0].comparatorRva, kSortSites[1].comparatorRva, kSortSites[2].comparatorRva);
    return true;
}

FrameTap::VtableInfo& FrameTap::vtableInfo(const void* vtable) {
    auto [it, inserted] = vtables_.try_emplace(vtable);
    VtableInfo& v = it->second;
    if (!inserted) return v;
    const auto& s = engine::sym();
    const std::pair<void*, OpKind> exported[] = {
        {s.ctxMeshVftable, OpKind::Mesh},          {s.ctxAnimationVftable, OpKind::Animation},
        {s.ctxSpeedTreeVftable, OpKind::SpeedTree}, {s.ctxSpeedTreeBBVftable, OpKind::SpeedTreeBB},
        {s.ctxVegetationVftable, OpKind::Vegetation}, {s.rcDebugVftable, OpKind::Debug},
    };
    for (const auto& [vt, kind] : exported) {
        if (vt != nullptr && vt == vtable) v.kind = kind;
    }
    const char* how = "exported vtable";
    if (v.kind == OpKind::Unknown && engineBase_ != nullptr) {
        for (const UnexportedContext& u : kUnexported) {
            if (vtable != engineBase_ + u.vtableRva) continue;
            // the RequestRenderOps slot (+0x40, never patched by us) must hold the documented function
            const void* request = nullptr;
            if (readAt(vtable, 0x40, request) && request == engineBase_ + u.requestRva) {
                v.kind = u.kind;
                how = "vtable RVA, RequestRenderOps slot verified";
            } else {
                how = "vtable RVA matched, but its RequestRenderOps slot differs: Unknown";
            }
        }
    }
    spdlog::info("native: frame tap: context vtable {} (RVA {:#x}) = {} ({})", vtable,
                 engineBase_ != nullptr ? static_cast<const std::uint8_t*>(vtable) - engineBase_ : 0, name(v.kind),
                 v.kind == OpKind::Unknown && std::strcmp(how, "exported vtable") == 0 ? "not a known context class" : how);
    return v;
}

// What a mesh op needs to draw (eCMeshResource2::RenderZPass 0x7603b0 / Render 0x760ca0 -> eCSubMesh::Render 0x5c7eb0), as
// bits: 1 the LOD's resource cached (eCResource2 +0x38 == 3), 2 its visual (+0x88), 4 the op's sub-mesh materials cached, 8 their
// shaders (+0x70) present. A whole-mesh op (desc 0xff) checks every sub-mesh.
static std::int32_t meshState(const void* ctx, std::uint32_t desc) {
    const std::uint8_t* res = nullptr;
    std::int32_t state = 0;
    if (!readAt(ctx, 0x28, res) || res == nullptr || !readAt(res, 0x38, state)) return -1;
    std::int32_t bits = state == 3 ? 1 : 0;
    const void* visual = nullptr;
    if (readAt(res, 0x88, visual) && visual != nullptr) bits |= 2;
    const std::uint8_t* table = nullptr;
    std::int32_t count = 0;
    if (!readAt(res, 0x58, table) || !readAt(res, 0x60, count) || table == nullptr || count <= 0) return bits;
    const std::uint32_t first = desc == 0xff ? 0 : desc & 0xff;
    const std::uint32_t last = desc == 0xff ? static_cast<std::uint32_t>(count) : first + 1;
    bool cached = true, shaders = true;
    for (std::uint32_t i = first; i < last && i < static_cast<std::uint32_t>(count); ++i) {
        const std::uint8_t* material = nullptr;
        if (!readAt(table + i * 0x48, 0x38, material) || material == nullptr) continue;  // no material: the engine skips it
        std::int32_t ms = 0;
        const void* shader = nullptr;
        if (!readAt(material, 0x38, ms) || ms != 3) cached = false;
        if (!readAt(material, 0x70, shader) || shader == nullptr) shaders = false;
    }
    return bits | (cached ? 4 : 0) | (shaders ? 8 : 0);
}

void FrameTap::resolve(Op& op) {
    const void* ctx = op.raw.context;
    if (ctx == nullptr || !readAt(ctx, 0, op.vtable) || op.vtable == nullptr) return;
    readAt(ctx, 0x10, op.entity);
    VtableInfo& v = vtableInfo(op.vtable);
    op.kind = v.kind;
    const int slot = kListSlot[static_cast<std::size_t>(op.list)];
    if (v.emptySlot[slot] < 0) {
        // the slot Execute calls for this list reports its op to the tap (patched before Execute's loops run)
        const void* original = nullptr;
        if (engine::scene::ensureOpSlot(op.vtable, slot, &original)) {
            v.emptySlot[slot] = emptyFunction(original) ? 1 : 0;
        } else {
            v.emptySlot[slot] = 0;
            spdlog::warn("native: frame tap: slot {} of vtable {} could not be prepared: its ops are not seen", slot, op.vtable);
        }
    }
    op.emptySlot = v.emptySlot[slot] == 1;
    if (op.kind == OpKind::Mesh || op.kind == OpKind::Animation) {
        std::uint32_t flags = 0;
        if (readAt(op.entity, 0x28, flags)) op.overlay = flags & 0xe00000u;
        if (op.kind == OpKind::Mesh) op.state = meshState(ctx, op.raw.desc);
    } else if (op.kind == OpKind::SpeedTreeBB) {
        // the billboard mesh (eCSpeedTreeBBMesh_PS +0x30 = its eCMeshResource2): eCMeshResource2::RenderZPass / Render need it
        // cached (eCResource2 +0x38 == 3)
        const std::uint8_t* ps = nullptr;
        const std::uint8_t* res = nullptr;
        std::int32_t state = 0;
        if (readAt(ctx, 0x18, ps) && readAt(ps, 0x30, res) && readAt(res, 0x38, state)) op.state = state == 3 ? 1 : 0;
    }
    if (op.kind == OpKind::Mesh && op.raw.desc != 0xff) {
        // eCRenderContextMesh +0x28 = the eCMeshResource2 of the requested LOD; its sub-mesh table +0x58 (stride 0x48, count +0x60);
        // desc = sub-mesh | node << 8 (0xffffff00 | i: the instanced op of sub-mesh i); sub-mesh +0x38 material, material +0x70 shader
        const std::uint8_t* res = nullptr;
        const std::uint8_t* table = nullptr;
        std::int32_t count = 0;
        const std::uint32_t sub = op.raw.desc & 0xff;
        if (readAt(ctx, 0x28, res) && readAt(res, 0x58, table) && readAt(res, 0x60, count) && table != nullptr &&
            sub < static_cast<std::uint32_t>(std::max(count, 0))) {
            op.subMesh = table + sub * 0x48;
            if (readAt(op.subMesh, 0x38, op.material)) readAt(op.material, 0x70, op.shader);
        }
    }
}

void FrameTap::executeBegin(void* db, void* desc) {
    if (!on_) return;
    if (executing_) {
        ++nested_;
        return;
    }
    if (verified_ == 0) verified_ = verifySortSites() ? 1 : -1;
    if (verified_ < 0 || db == nullptr) return;
    const std::uint64_t frame = dev_.stats().frames + 1;  // the frame being recorded
    if (frame != viewFrame_) {
        viewFrame_ = frame;
        viewsInFrame_ = 0;
    }
    desc_.clear();
    desc_.frame = frame;
    desc_.view = ++viewsInFrame_;
    desc_.db = db;
    std::uint8_t zPrepass = 1;
    readAt(db, 0x1590, zPrepass);
    desc_.zPrepass = zPrepass != 0;
    for (std::size_t l = 0; l < kListCount; ++l) {
        const std::uint8_t* data = nullptr;
        std::int32_t count = 0;
        desc_.first[l] = static_cast<std::uint32_t>(desc_.ops.size());
        if (!readAt(db, kListOffset[l] + 8, data) || !readAt(db, kListOffset[l] + 0x10, count) || data == nullptr || count <= 0 ||
            static_cast<std::uint32_t>(count) > kMaxListOps) {
            continue;
        }
        raw_.resize(static_cast<std::size_t>(count));
        if (!safeRead(data, raw_.data(), raw_.size() * sizeof(RawOp))) continue;
        // the engine's own qsort with the engine's comparator, on the copy: the order Execute will produce (MSVC's qsort is
        // deterministic, the input is byte-identical; attribution checks every position against the executed array)
        if (kListSorted[l] && count > 1) qsort_(raw_.data(), raw_.size(), sizeof(RawOp), compare_[l]);
        desc_.liveData[l] = data;
        desc_.count[l] = static_cast<std::uint32_t>(count);
        for (std::uint32_t i = 0; i < desc_.count[l]; ++i) {
            Op op;
            op.raw = raw_[i];
            op.list = static_cast<List>(l);
            op.index = i;
            resolve(op);
            desc_.ops.push_back(op);
        }
    }
    // the refraction copy Execute makes after the +0x1530 list: into the render-target desc's image desc +0 (API texture +0x60)
    if (desc != nullptr && readAt(desc, 0, desc_.refraction.image) && desc_.refraction.image != nullptr) {
        readAt(desc_.refraction.image, 0x60, desc_.refraction.api);
    }
    desc_.valid = true;
    db_ = db;
    executing_ = true;
    attr_.begin(&desc_);
    if (statics_ != nullptr && staticsWanted_) statics_->viewBegin(db, desc);
}

void FrameTap::opEnter(const void* op) {
    if (!executing_) return;
    RawOp live;
    attr_.enterOp(op, safeRead(op, &live, sizeof(live)) ? &live : nullptr);
    if (statics_ != nullptr && staticsWanted_) {
        if (const Op* o = desc_.find(op)) statics_->opEnter(*o);
    }
}

void FrameTap::nonOpBegin(NonOp which, void* admin, void* db) {
    if (which == NonOp::SkyCube) {
        // outside Execute (eCIlluminationAdmin::Process -> UpdateSkydomeEnvCubemap)
        if (!executing_ && statics_ != nullptr && staticsWanted_) statics_->skyCubeBegin(admin);
        return;
    }
    if (!executing_ || nested_ > 0 || statics_ == nullptr || !staticsWanted_) return;
    if (which == NonOp::Decals && db == db_) statics_->decalsBegin(admin, db);
    // RenderBillboards takes no database: it runs inside the main view's Execute (not a nested one)
    if (which == NonOp::Billboards) statics_->billboardsBegin(admin);
}

void FrameTap::nonOpEnd(NonOp which) {
    if (statics_ == nullptr || !staticsWanted_) return;
    if (which == NonOp::SkyCube) {
        statics_->nonOpEnd();
        return;
    }
    if (!executing_) return;
    if (which == NonOp::Decals || which == NonOp::Billboards) statics_->nonOpEnd();
}

void FrameTap::refractionCopy(const void* api) {
    if (!executing_) return;
    FrameDesc::Refraction& r = desc_.refraction;
    if (api != nullptr && api == r.api) {
        ++r.copies;
        if (refractionCopyInPlace(desc_, attr_.nextOp())) ++r.inPlace;
    } else {
        ++r.other;
    }
}

bool FrameTap::onDraw(const d3d9::DrawTapInfo& d) {
    // the static ops' own draws (N5.1) are not the engine's
    if (statics_ != nullptr && statics_->issuing()) {
        statics_->ownDraw(d);
        return false;
    }
    // a sky cube face's draws (N5.2c, outside Execute)
    if (statics_ != nullptr && staticsWanted_ && statics_->inSkyCube() && d.phase.view == d3d9::scene::View::SkyCube) {
        return statics_->engineDraw(d, nullptr);
    }
    if (!executing_ || d.phase.view != d3d9::scene::View::Main) return false;
    DrawInfo info;
    info.index = d.index;
    info.pass = d.phase.pass;
    info.kind = d.kind;
    if (d.tag != nullptr) {
        info.op = d.tag->op;
        info.entity = d.tag->entity;
        info.subMesh = d.tag->subMesh;
        info.material = d.tag->material;
    }
    info.instances = d.instances;
    info.instanced = d.instanced;
    info.additive = d.additive;
    info.colorWrite = d.colorWrite;
    info.vsHash = d.vsHash;
    info.psHash = d.psHash;
    info.blend = d.blend;
    attr_.draw(info);
    if (statics_ != nullptr && staticsWanted_) return statics_->engineDraw(d, d.tag != nullptr ? desc_.find(d.tag->op) : nullptr);
    return false;
}

void FrameTap::executeEnd(void* db) {
    if (!executing_) return;
    if (nested_ > 0 && db != db_) {
        --nested_;
        return;
    }
    std::array<std::uint32_t, kListCount> liveCounts{};
    for (std::size_t l = 0; l < kListCount; ++l) {
        std::int32_t count = 0;
        readAt(db_, kListOffset[l] + 0x10, count);
        liveCounts[l] = static_cast<std::uint32_t>(std::max(count, 0));
    }
    // the draw conditions of the contexts that may draw nothing (read after their slots ran)
    for (Op& op : desc_.ops) {
        if (op.kind != OpKind::Strip && op.kind != OpKind::Precipitation && op.kind != OpKind::Particle) continue;
        const std::uint8_t* ps = nullptr;
        if (!readAt(op.raw.context, 0x18, ps) || ps == nullptr) continue;
        if (op.kind == OpKind::Strip) {
            // RenderAlpha 0x701120 draws from 2 segments (PS+0xe8) on; RenderVertices 0x6fd800 needs the visual (PS+0x118) and the
            // material (PS+0x120) with its shader (+0x70: null while the material resource is not cached)
            std::int32_t segments = 0;
            const void* visual = nullptr;
            const std::uint8_t* material = nullptr;
            const void* shader = nullptr;
            if (readAt(ps, 0xe8, segments) && readAt(ps, 0x118, visual) && readAt(ps, 0x120, material)) {
                if (material != nullptr) readAt(material, 0x70, shader);
                op.state = visual != nullptr && shader != nullptr ? segments : 0;
            }
        } else if (op.kind == OpKind::Precipitation) {
            // RenderAlpha draws only when PS+0x18 & 1; eCPrecipitation_PS::GetNumParticlesAlive (0x71d1e0) = PS+0xe0 - PS+0xc8
            std::uint8_t flags = 0;
            std::int32_t end = 0, begin = 0;
            if (readAt(ps, 0x18, flags) && readAt(ps, 0xe0, end) && readAt(ps, 0xc8, begin)) op.state = (flags & 1) != 0 ? 1 + std::max(end - begin, 0) : 0;
        } else {
            std::int32_t live = 0;
            if (readAt(ps, 0x390, live)) op.state = live;
        }
    }
    if (statics_ != nullptr && staticsWanted_) statics_->viewEnd();
    attr_.end(liveCounts);
    executing_ = false;
    last_ = attr_.counts();
    lastFrame_ = desc_.frame;
    window_.add(last_);
    if (windowMismatches_.empty() && !attr_.mismatches().empty()) {
        windowMismatches_.push_back(std::format("(first mismatches of the window, frame {} view {})", desc_.frame, desc_.view));
        windowMismatches_.insert(windowMismatches_.end(), attr_.mismatches().begin(), attr_.mismatches().end());
    }
    if (hal::vulkan::logThisFrame() && wanted_) logOps();
}

void FrameTap::logOps() const {
    spdlog::info("native: frame tap: frame {} main view {}: {} ops (z pre-pass {}), in Execute's order:", desc_.frame, desc_.view, desc_.ops.size(),
                 desc_.zPrepass ? "on" : "off");
    for (const Op& op : desc_.ops) {
        std::string material;
        if (op.material != nullptr) material = " '" + engine::readCString(static_cast<const std::uint8_t*>(op.material) + 0x40) + "'";
        spdlog::info("native:   {}{}", describe(op), material);
    }
    for (const std::string& line : reportLines(last_, lastFrame_, last_, attr_.mismatches())) spdlog::info("{}", line);
}

void FrameTap::report() {
    if (statics_ != nullptr) statics_->report();
    if (!on_ || !wanted_) return;
    if (verified_ < 0) {
        spdlog::info("native: frame tap: idle (Execute's sort sites did not verify)");
        return;
    }
    if (window_.views == 0) {
        spdlog::info("native: frame tap: no main view since the last report");
        return;
    }
    for (const std::string& line : reportLines(last_, lastFrame_, window_, windowMismatches_)) spdlog::info("{}", line);
    window_ = {};
    windowMismatches_.clear();
}

}  // namespace rvk::native
