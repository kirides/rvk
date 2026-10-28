#include "native/frame_tap.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "engine/memread.h"
#include "engine/shadow_maps.h"
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
// ExecuteShadowMap (0x66f630) sorts its z list the same way (disassembled): mov rcx, [rbx + 0x1520] (48 8b 8b) at +0x6b, the lea of
// the z comparator 0x66e670 at +0x72, then mov rdx, rax; mov r8d, 0x18; call qsort
constexpr std::uint32_t kExecuteShadowMapRva = 0x66f630;
constexpr std::uint32_t kShadowSortLea = 0x66f6a2 - kExecuteShadowMapRva;
// the shadow maps' vtables (Engine.dll exports ??_7eCShadowMapDirectionalLight2Ex (the cascade splits; checked in game: E 2.2's
// "eCShadowMapDirectionalLight2 0x9768d0" is the base class) / ...2VS / eCPackedShadowMap)
constexpr std::uint32_t kCascadeSplitVtable = 0x9a29e0, kTerrainMapVtable = 0x9a2848, kPackedMapVtable = 0x976738;
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
        for (auto& m : windowMismatches_) m.clear();
        cascadeViews_ = {};
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

// What a skinned op needs to draw (eCAnimation_PS::Render 0x4d6fe0 -> eCAnimation_HardRenderActor2::Render 0x4dd650 ->
// RenderPrimitive 0x4dd0a0), as bits read at the op's slot entry (just before the engine's code): 1 the actor's wrapper (actor 0:
// PS +0x30, else body part k - 1 +0x30) with its resource (+0x18) and render hook (+0x98), 2 UpdateRenderMatrices can run (the
// ActorInstance +8, the resource's actor +0x70), 4 the hook's resource cached (+0x38 == 3), 8 the LOD's primitive's material has
// its shader (+0x70), 16 the shader has passes there (the z pass: the depth-map type's special pass compiled in the admin's version;
// else a version object and a valid shader: GetPasses' conditions)
static std::int32_t animationState(const Op& op) {
    const std::uint8_t* ps = nullptr;
    if (!readAt(op.raw.context, 0x18, ps) || ps == nullptr) return -1;
    const std::uint32_t actorIdx = op.raw.desc & 0xffffu, primIdx = op.raw.desc >> 16;
    const std::uint8_t* wrapper = ps + 0x30;
    if (actorIdx != 0) {
        const std::uint8_t* parts = nullptr;
        const std::uint8_t* part = nullptr;
        if (!readAt(ps, 0x1b0, parts) || !readAt(parts, 8 * static_cast<std::size_t>(actorIdx - 1), part) || part == nullptr) return 0;
        wrapper = part + 0x30;
    }
    const std::uint8_t* res = nullptr;
    const std::uint8_t* hra = nullptr;
    if (!readAt(wrapper, 0x18, res) || res == nullptr || !readAt(res, 0x98, hra) || hra == nullptr) return 0;
    std::int32_t bits = 1;
    const std::uint8_t* ai = nullptr;
    const void* actor = nullptr;
    if (readAt(wrapper, 8, ai) && ai != nullptr && readAt(res, 0x70, actor) && actor != nullptr) bits |= 2;
    const std::uint8_t* hres = nullptr;
    std::int32_t state = 0;
    if (readAt(hra, 0, hres) && readAt(hres, 0x38, state) && state == 3) bits |= 4;
    std::uint32_t lod = 0;
    if (ai != nullptr) readAt(ai, 0x250, lod);
    lod = lod < 4 ? lod & 0xffffu : 4u;
    std::uint16_t first = 0;
    const std::uint8_t* prims = nullptr;
    const std::uint8_t* material = nullptr;
    const std::uint8_t* shader = nullptr;
    if (!readAt(hra, 0x38 + 0x14 * lod, first) || !readAt(hra, 0x90, prims) || prims == nullptr) return bits;
    const std::size_t prim = static_cast<std::uint16_t>(first + primIdx);
    if (!readAt(prims + 0x20 * prim, 0x10, material) || material == nullptr || !readAt(material, 0x70, shader) || shader == nullptr) return bits;
    bits |= 8;
    const std::uint8_t* admin = nullptr;
    std::int32_t adminVersion = 0, type = 6, shaderVersion = 0;
    if (!readAt(shader, 0x220, admin) || admin == nullptr || !readAt(admin, 0x170, adminVersion)) return bits;
    const auto versionAt = [&](std::int32_t v) {
        const void* p = nullptr;
        return v >= 0 && readAt(shader, 0x258 + 8 * static_cast<std::size_t>(v), p) ? p : nullptr;
    };
    if (op.list == List::ZPass) {
        std::uint8_t compiled = 0;
        const auto* version = static_cast<const std::uint8_t*>(versionAt(adminVersion));
        if (readAt(admin, 0x314, type) && type != 6 && version != nullptr && readAt(version, 0x18 + 2 * static_cast<std::size_t>(type), compiled) &&
            compiled != 0) {
            bits |= 16;
        }
    } else {
        std::int32_t v = adminVersion;
        if (readAt(shader, 0x68, shaderVersion)) v = std::min(v, shaderVersion);
        const void* version = versionAt(std::max(v, 0));
        while (version == nullptr && v > 0) version = versionAt(--v);
        std::uint8_t invalid = 0;
        std::uint32_t adminInvalid = 0;
        readAt(shader, 0x2a8, invalid);
        readAt(admin, 0x31c, adminInvalid);
        if (version != nullptr && invalid != 1 && adminInvalid == 0) bits |= 16;
    }
    return bits;
}

// What a SpeedTree op needs to draw (eCRenderContextSpeedTree::Render 0x6ee320 / RenderZPass 0x6ee3f0 -> eCSpeedTreeRenderBuffer::
// RenderBranches / RenderFronds / RenderLeafs and their z passes), as bits read at the op's slot entry: 1 the render buffer (PS +0x38
// -> +0x78) with the type's visual (+0x18 + 8 t), the wrapper (+8) and a non-empty queue (solid +0xc0, z +0xd8), 2 the type's
// material (+0x80 + 8 t) has its shader and (for eCShaderDefault) the matching TransformationType (+0x2b8: 3 / 4 / 5-6), 4 a queued
// tree has geometry the op draws: branches a LOD index >= 0 (tree +0x48), fronds one whose strip length (+0x50 [lod]) is not 0
// (RenderFronds 0x6f1b0f), leaves a tree at a leaf LOD (+0x58 / +0x5c) before the first LOD without indices. A shadow-map z op
// (desc bit 0x10 << 16, N5.5; RenderZPass 0x6ee3f0 passes it to the z passes): fronds only when the leaf cards' vertex count (+0x3c)
// is <= the fronds' (+0x3a) (RenderFrondsZPass 0x6f0cc0: else nothing, state 3), leaves at the one LOD min(+0x58 + 1, LODs - 1)
// (RenderLeafsZPass 0x6f0860, alpha 1)
static std::int32_t treeState(const Op& op, const void* shaderDefaultVtable) {
    const bool zpass = op.list == List::ZPass;
    const bool shadow = zpass && ((op.raw.desc >> 16) & 0x10u) != 0;
    const std::uint8_t* ps = nullptr;
    const std::uint8_t* res = nullptr;
    const std::uint8_t* rb = nullptr;
    if (!readAt(op.raw.context, 0x18, ps) || !readAt(ps, 0x38, res) || res == nullptr || !readAt(res, 0x78, rb) || rb == nullptr) return 0;
    const std::uint32_t type = zpass ? (op.raw.desc & 0xffffu) : op.raw.desc;
    if (type > 3) return 0;
    std::int32_t n = 0;
    const void* visual = nullptr;
    const std::uint8_t* wrapper = nullptr;
    const std::uint8_t* queue = nullptr;
    readAt(rb, zpass ? 0xd8 : 0xc0, n);
    readAt(rb, 0x18 + 8 * type, visual);
    readAt(rb, 8, wrapper);
    readAt(rb, zpass ? 0xd0 : 0xb8, queue);
    if (n <= 0 || n > 65536 || visual == nullptr || wrapper == nullptr || queue == nullptr) return 0;
    std::int32_t bits = 1;
    const std::uint8_t* material = nullptr;
    const std::uint8_t* shader = nullptr;
    if (readAt(rb, 0x80 + 8 * type, material) && readAt(material, 0x70, shader) && shader != nullptr) {
        const void* vt = nullptr;
        std::int32_t tt = -1;
        readAt(shader, 0, vt);
        readAt(shader, 0x2b8, tt);
        const bool ok = vt != shaderDefaultVtable || (type == 0 ? tt == 3 : type == 1 ? tt == 4 : (tt == 5 || tt == 6));
        if (ok) bits |= 2;
    }
    if (shadow && type == 1) {
        std::uint16_t frondVerts = 0, cardVerts = 0;
        readAt(rb, 0x3a, frondVerts);
        readAt(rb, 0x3c, cardVerts);
        if (cardVerts > frondVerts) return bits & ~4;
    }
    std::int32_t leafLods = 0, firstEmpty = 0;
    if (type >= 2) {
        const std::uint8_t* rt = nullptr;
        const std::uint8_t* rtData = nullptr;
        if (readAt(wrapper, 0, rt) && readAt(rt, 8, rtData)) readAt(rtData, 0xf4, leafLods);
        firstEmpty = std::max(leafLods, 0);
        for (std::int32_t l = 0; l < leafLods && l < 64; ++l) {
            std::uint16_t indices = 0;
            readAt(rb, 0x40 + 0x10 * static_cast<std::size_t>(type) + 2 * static_cast<std::size_t>(l), indices);
            if (indices == 0) {
                firstEmpty = l;
                break;
            }
        }
    }
    for (std::int32_t i = 0; i < n; ++i) {
        const std::uint8_t* tree = nullptr;
        if (!readAt(queue, 0x18 * static_cast<std::size_t>(i), tree) || tree == nullptr) continue;
        if (type <= 1) {
            std::int32_t lod = -1;
            readAt(tree, type == 0 ? 0x48 : 0x50, lod);
            if (lod < 0) continue;
            if (type == 1) {
                std::uint16_t strip = 0;
                readAt(rb, 0x50 + 2 * static_cast<std::size_t>(lod), strip);
                if (strip == 0) continue;
            }
            bits |= 4;
            break;
        }
        std::int32_t l0 = -1, l1 = -1;
        readAt(tree, 0x58, l0);
        readAt(tree, 0x5c, l1);
        if (shadow) {
            l0 = l0 < 0 ? -1 : std::min(l0 + 1, leafLods - 1);
            l1 = -1;
        }
        if ((l0 >= 0 && l0 < firstEmpty) || (l1 >= 0 && l1 < firstEmpty)) {
            bits |= 4;
            break;
        }
    }
    return bits;
}

// What a vegetation op needs to draw (eCRenderContextVegetation::Render 0x70d4b0 / RenderZPass 0x70d4d0 -> eCVegetation_PS::Render
// 0x707800 -> RenderMeshInstanced 0x707e40), as bits read at the slot's entry: 1 a visible grid node (PS +0x98, count +0xa0) holds
// instances of the op's type (grid node +0x30 [type] .count), 2 the type's mesh (PS +0x38 [type] +0x58) cached (+0x38 == 3) with its
// visual (+0x88), 4 the vegetation admin's shared buffer (+0x38), 8 the mesh's first sub-mesh's material has its shader
static std::int32_t vegetationState(const Op& op, void* (*vegAdmin)()) {
    const std::uint8_t* ps = nullptr;
    if (!readAt(op.raw.context, 0x18, ps) || ps == nullptr) return -1;
    const auto type = static_cast<std::int32_t>(op.raw.desc);
    std::int32_t bits = 0;
    std::int32_t count = 0;
    const std::uint8_t* nodes = nullptr;
    readAt(ps, 0xa0, count);
    readAt(ps, 0x98, nodes);
    for (std::int32_t i = 0; i < count && i < 100000 && nodes != nullptr; ++i) {
        const std::uint8_t* grid = nullptr;
        const std::uint8_t* ranges = nullptr;
        std::uint32_t n = 0;
        if (readAt(nodes + 0x118 * static_cast<std::size_t>(i), 8, grid) && readAt(grid, 0x30, ranges) &&
            readAt(ranges, 8 * static_cast<std::size_t>(static_cast<std::int64_t>(type)) + 4, n) && n != 0) {
            bits |= 1;
            break;
        }
    }
    const std::uint8_t* types = nullptr;
    const std::uint8_t* typeObj = nullptr;
    const std::uint8_t* mesh = nullptr;
    std::int32_t state = 0;
    const void* visual = nullptr;
    if (readAt(ps, 0x38, types) && readAt(types, 8 * static_cast<std::size_t>(static_cast<std::uint16_t>(type)), typeObj) && readAt(typeObj, 0x58, mesh) &&
        readAt(mesh, 0x38, state) && state == 3 && readAt(mesh, 0x88, visual) && visual != nullptr) {
        bits |= 2;
    }
    const std::uint8_t* admin = vegAdmin != nullptr ? static_cast<const std::uint8_t*>(vegAdmin()) : nullptr;
    const void* vd = nullptr;
    if (readAt(admin, 0x38, vd) && vd != nullptr) bits |= 4;
    const std::uint8_t* sub0 = nullptr;
    const std::uint8_t* material = nullptr;
    const void* shader = nullptr;
    if (readAt(mesh, 0x58, sub0) && readAt(sub0, 0x38, material) && readAt(material, 0x70, shader) && shader != nullptr) bits |= 8;
    return bits;
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
    beginView(db, desc, ViewKind::Main, nullptr, -1);
}

bool FrameTap::verifyShadowSortSite() {
    const auto* fn = static_cast<const std::uint8_t*>(engine::sym().frustumDbExecuteShadowMap);
    if (fn == nullptr || engineBase_ == nullptr || fn != engineBase_ + kExecuteShadowMapRva) {
        spdlog::warn("native: frame tap: ExecuteShadowMap not at RVA {:#x}: the shadow views stay untapped", kExecuteShadowMapRva);
        return false;
    }
    std::array<std::uint8_t, 7 + 21> b{};
    if (!safeRead(fn + kShadowSortLea - 7, b.data(), b.size())) return false;
    const bool loadOk = b[0] == 0x48 && b[1] == 0x8b && b[2] == 0x8b && static_cast<std::uint32_t>(rel32(&b[3])) == kListOffset[0] + 8;
    const bool leaOk = b[7] == 0x4c && b[8] == 0x8d && b[9] == 0x0d;
    const bool movOk = b[14] == 0x48 && b[15] == 0x8b && b[16] == 0xd0 && b[17] == 0x41 && b[18] == 0xb8 && b[19] == 0x18 && b[20] == 0 &&
                       b[21] == 0 && b[22] == 0;
    const std::uint8_t* cmp = fn + kShadowSortLea + 7 + rel32(&b[10]);
    const std::uint8_t* q = fn + kShadowSortLea + 16 + 5 + rel32(&b[24]);
    if (!loadOk || !leaOk || !movOk || b[23] != 0xe8 || reinterpret_cast<CompareFn>(const_cast<std::uint8_t*>(cmp)) != compare_[0] ||
        reinterpret_cast<QsortFn>(const_cast<std::uint8_t*>(q)) != qsort_) {
        spdlog::warn("native: frame tap: ExecuteShadowMap's sort site (+{:#x}) differs from the main z pass's: the shadow views stay untapped",
                     kShadowSortLea);
        return false;
    }
    kindVtables_[static_cast<std::size_t>(ViewKind::Cascade)] = engineBase_ + kCascadeSplitVtable;
    kindVtables_[static_cast<std::size_t>(ViewKind::Terrain)] = engineBase_ + kTerrainMapVtable;
    kindVtables_[static_cast<std::size_t>(ViewKind::PointAtlas)] = engineBase_ + kPackedMapVtable;
    spdlog::info("native: frame tap: ExecuteShadowMap's sort verified (the main z pass's qsort and comparator): shadow views tapped");
    return true;
}

ViewKind FrameTap::shadowKind(const void* shadowMap, std::int32_t& cascade) {
    cascade = -1;
    const void* vt = nullptr;
    if (!readAt(shadowMap, 0, vt) || vt == nullptr) return ViewKind::OtherShadow;
    for (std::size_t k = 1; k < kViewKindCount; ++k) {
        if (vt != kindVtables_[k] || kindVtables_[k] == nullptr) continue;
        const auto kind = static_cast<ViewKind>(k);
        if (kind == ViewKind::Cascade) {
            // the split's index in the cascade object's array (+0x30, 0x168 bytes per split, count +0x38)
            const auto* cascades = static_cast<const std::uint8_t*>(engine::shadowmaps::cascadeObject());
            const std::uint8_t* splits = nullptr;
            std::int32_t count = 0;
            const auto* m = static_cast<const std::uint8_t*>(shadowMap);
            if (readAt(cascades, 0x30, splits) && readAt(cascades, 0x38, count) && splits != nullptr && m >= splits &&
                (m - splits) % 0x168 == 0 && (m - splits) / 0x168 < count) {
                cascade = static_cast<std::int32_t>((m - splits) / 0x168);
            }
        }
        return kind;
    }
    for (const void*& logged : otherVtables_) {
        if (logged == vt) break;
        if (logged != nullptr) continue;
        logged = vt;
        spdlog::info("native: frame tap: a shadow map of vtable {} (RVA {:#x}): other shadow map", vt,
                     static_cast<const std::uint8_t*>(vt) - engineBase_);
        break;
    }
    return ViewKind::OtherShadow;
}

void FrameTap::shadowExecuteBegin(void* db, void* shadowMap) {
    if (!on_) return;
    if (executing_) {
        ++nested_;  // a shadow map inside another view (not met: the shadow views are pre-passes of the main database)
        return;
    }
    if (verified_ == 0) verified_ = verifySortSites() ? 1 : -1;
    if (verified_ < 0 || db == nullptr || shadowMap == nullptr) return;
    if (shadowVerified_ == 0) shadowVerified_ = verifyShadowSortSite() ? 1 : -1;
    if (shadowVerified_ < 0) return;
    std::int32_t cascade = -1;
    const ViewKind kind = shadowKind(shadowMap, cascade);
    if (kind == ViewKind::Cascade) ++cascadeViews_[cascade >= 0 && cascade < 3 ? static_cast<std::size_t>(cascade) : 3u];
    beginView(db, nullptr, kind, shadowMap, cascade);
}

void FrameTap::beginView(void* db, void* desc, ViewKind kind, void* shadowMap, std::int32_t cascade) {
    const std::uint64_t frame = dev_.stats().frames + 1;  // the frame being recorded
    if (frame != viewFrame_) {
        viewFrame_ = frame;
        viewsInFrame_ = {};
    }
    const bool shadow = kind != ViewKind::Main;
    desc_.clear();
    desc_.frame = frame;
    desc_.view = ++viewsInFrame_[static_cast<std::size_t>(kind)];
    desc_.db = db;
    desc_.kind = kind;
    desc_.shadowMap = shadowMap;
    desc_.cascade = cascade;
    if (shadow) {
        readAt(shadowMap, 8, desc_.mapFormat);
        readAt(shadowMap, 0xc, desc_.mapSize);
    }
    std::uint8_t zPrepass = 1;
    // a shadow database's caps say no z pre-pass, but ExecuteShadowMap runs its z list unconditionally
    if (!shadow) readAt(db, 0x1590, zPrepass);
    desc_.zPrepass = zPrepass != 0;
    for (std::size_t l = 0; l < kListCount; ++l) {
        const std::uint8_t* data = nullptr;
        std::int32_t count = 0;
        desc_.first[l] = static_cast<std::uint32_t>(desc_.ops.size());
        // ExecuteShadowMap runs the z list only (the shadow request fills no other list it executes)
        if (shadow && l != 0) continue;
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
    if (statics_ != nullptr && staticsWanted_) statics_->viewBegin(db, desc, desc_);
}

void FrameTap::opEnter(const void* op) {
    if (!executing_) return;
    if (Op* o = desc_.find(op); o != nullptr && !o->emptySlot) {
        if (o->kind == OpKind::Animation) o->state = animationState(*o);
        // N5.4: the SpeedTree / vegetation ops' draw conditions (the 0-draw ops checked at run time)
        if (o->kind == OpKind::SpeedTree && (o->list == List::ZPass || o->list == List::Solid)) o->state = treeState(*o, engine::sym().shaderDefaultVftable);
        if (o->kind == OpKind::Vegetation && (o->list == List::ZPass || o->list == List::Solid)) {
            if (vegAdmin_ == nullptr && engineBase_ != nullptr) {
                // FUN_180265df0, the vegetation admin's getter: sub rsp, 0x38; cmp byte [eCApplication::ms_bInitialised], 1
                static constexpr std::uint8_t kPrologue[6] = {0x48, 0x83, 0xec, 0x38, 0x80, 0x3d};
                std::uint8_t b[6] = {};
                if (safeRead(engineBase_ + 0x265df0, b, sizeof(b)) && std::memcmp(b, kPrologue, sizeof(b)) == 0) {
                    vegAdmin_ = reinterpret_cast<void* (*)()>(const_cast<std::uint8_t*>(engineBase_ + 0x265df0));
                }
            }
            o->state = vegetationState(*o, vegAdmin_);
        }
    }
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
    if (desc_.kind != ViewKind::Main) return;
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
    if (!executing_ || d.phase.view != (desc_.kind == ViewKind::Main ? d3d9::scene::View::Main : d3d9::scene::View::Shadow)) return false;
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
        // a shadow view runs its z list only (the others are not snapshotted)
        if (desc_.kind != ViewKind::Main && l != 0) liveCounts[l] = desc_.count[l];
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
    const auto k = static_cast<std::size_t>(desc_.kind);
    last_[k] = attr_.counts();
    lastFrame_[k] = desc_.frame;
    window_[k].add(last_[k]);
    if (windowMismatches_[k].empty() && !attr_.mismatches().empty()) {
        windowMismatches_[k].push_back(std::format("(first mismatches of the window, frame {} {} view {}{})", desc_.frame, name(desc_.kind), desc_.view,
                                                   desc_.cascade >= 0 ? std::format(" = cascade {}", desc_.cascade) : std::string()));
        windowMismatches_[k].insert(windowMismatches_[k].end(), attr_.mismatches().begin(), attr_.mismatches().end());
    }
    if (hal::vulkan::logThisFrame() && wanted_) logOps();
}

void FrameTap::logOps() const {
    spdlog::info("native: frame tap: frame {} {} view {}{}: {} ops (z pre-pass {}), in {}'s order:", desc_.frame, name(desc_.kind), desc_.view,
                 desc_.cascade >= 0 ? std::format(" (cascade {}, map format {} size {})", desc_.cascade, desc_.mapFormat, desc_.mapSize)
                 : desc_.kind != ViewKind::Main ? std::format(" (map format {} size {})", desc_.mapFormat, desc_.mapSize) : std::string(),
                 desc_.ops.size(), desc_.zPrepass ? "on" : "off", desc_.kind == ViewKind::Main ? "Execute" : "ExecuteShadowMap");
    for (const Op& op : desc_.ops) {
        std::string material;
        if (op.material != nullptr) material = " '" + engine::readCString(static_cast<const std::uint8_t*>(op.material) + 0x40) + "'";
        spdlog::info("native:   {}{}", describe(op), material);
    }
    const auto k = static_cast<std::size_t>(desc_.kind);
    for (const std::string& line : reportLines(last_[k], lastFrame_[k], last_[k], attr_.mismatches(), desc_.kind)) spdlog::info("{}", line);
}

void FrameTap::report() {
    if (statics_ != nullptr) statics_->report();
    if (!on_ || !wanted_) return;
    if (verified_ < 0) {
        spdlog::info("native: frame tap: idle (Execute's sort sites did not verify)");
        return;
    }
    if (window_[0].views == 0) spdlog::info("native: frame tap: no main view since the last report");
    for (std::size_t k = 0; k < kViewKindCount; ++k) {
        if (window_[k].views == 0) continue;
        const auto kind = static_cast<ViewKind>(k);
        for (const std::string& line : reportLines(last_[k], lastFrame_[k], window_[k], windowMismatches_[k], kind)) spdlog::info("{}", line);
        if (kind == ViewKind::Cascade) {
            spdlog::info("native: frame tap [cascade]: views by split index: 0 {}, 1 {}, 2 {}, unknown {}", cascadeViews_[0], cascadeViews_[1],
                         cascadeViews_[2], cascadeViews_[3]);
        }
        window_[k] = {};
        windowMismatches_[k].clear();
    }
    cascadeViews_ = {};
}

}  // namespace rvk::native
