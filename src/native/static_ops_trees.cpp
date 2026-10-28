// N5.4 scene ops: the main view's SpeedTree ops (eCRenderContextSpeedTree: branches, fronds, leaf cards, leaf meshes), the tree
// billboards' ops (eCRenderContextSpeedTreeBB) and the vegetation ops (eCRenderContextVegetation); docs/research/engine-frame/
// README.md "11. N5.4 result", D-contexts.md 2-4. Re-written from eCSpeedTreeRenderBuffer::RenderBranches 0x6f1cd0 / RenderFronds
// 0x6f1880 / RenderLeafs 0x6f1390 and their z passes (0x6f1030 / 0x6f0cc0 / 0x6f0860), eCRenderContextSpeedTreeBB::Render 0x6ee8d0 /
// RenderZPass 0x6ee8e0 + eCSpeedTreeBBMesh_PS::UploadTexCoords 0x6ec150, eCVegetation_PS::Render 0x707800 / RenderMeshInstanced
// 0x707e40 (dumps B:\DevDrive\risen-ghidra\frameD_dump1..3.txt + capstone disassembly). Part of StaticOps (static_ops.h); the pass
// rules are static_ops.cpp's, the batch rules static_compose's (planTreeBatches, treeRange, planGrass).

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
using U8 = const std::uint8_t;

template <class T>
T get(const void* p, std::size_t off, T fallback = T{}) {
    T v = fallback;
    if (!readAt(p, off, v)) return fallback;
    return v;
}
U8* ptr(const void* p, std::size_t off) { return get<U8*>(p, off, nullptr); }
bool rd(const void* p, std::size_t off, void* out, std::size_t n) { return p != nullptr && safeRead(static_cast<U8*>(p) + off, out, n); }

constexpr std::uint32_t kQsort = 0x4a4aa0;
constexpr std::uint32_t kTreeAdmin = 0x25a6d0, kVegAdmin = 0x265df0;  // FUN_18025a6d0 / FUN_180265df0 (module singletons)
constexpr std::uint32_t kSortInstances = 0x6f2180, kSortInstancesZ = 0x6f2120;
constexpr std::uint32_t kBbTexCoords = 0xbfcbd0;
constexpr std::uint32_t kLeafsVtable = 0x9d7be8;  // ??_7eCShaderLeafs@@6B@  // UploadTexCoords: c32..c36 = 5 vec4 (lea rdi, [rip + 0xbfcbd8]; reads rdi - 8)
constexpr std::uint32_t kSetTreeInstance = 0x6c9eb0, kSetVegetationAlpha = 0x6c9f20, kSetBbTexCoords = 0x6ca050;  // vtable +0x220 / 0x228 / 0x238

struct Site {
    std::uint32_t rva;
    std::array<std::uint8_t, 8> b;
    std::uint32_t n;
    std::uint32_t target;  // e8 call / 4c 8d 0d / 48 8d 3d lea sites: the RVA they reach (0: bytes only)
};
constexpr Site kSites[] = {
    {0x6ee35e, {0x4c, 0x8d, 0x0d}, 3, kSortInstances},   // eCRenderContextSpeedTree::Render: lea r9, SortInstances
    {0x6ee36e, {0xe8}, 1, kQsort},                       // ... call qsort (the solid queue, once per frame: stamp +0xe0)
    {0x6ee42d, {0x4c, 0x8d, 0x0d}, 3, kSortInstancesZ},  // RenderZPass: lea r9, SortInstancesZPass
    {0x6ee43d, {0xe8}, 1, kQsort},                       // ... call qsort (the z queue: stamp +0xe4)
    {0x6f12b7, {0xb2, 0x01}, 2, 0},                      // RenderBranchesZPass: ExecuteSpecialPass(true)
    {0x6f0f51, {0xb2, 0x01}, 2, 0},                      // RenderFrondsZPass: ExecuteSpecialPass(true)
    {0x6f0b7c, {0xb2, 0x01}, 2, 0},                      // RenderLeafsZPass: ExecuteSpecialPass(true)
    {0x707fd2, {0x33, 0xd2}, 2, 0},                      // RenderMeshInstanced: ExecuteSpecialPass(false)
    {0x6f1e18, {0x8b, 0x80, 0x20, 0x03, 0, 0}, 6, 0},    // RenderBranches: mov eax, [admin + 0x320] (the instancing mode)
    {0x6f14c9, {0x8b, 0x80, 0x20, 0x03, 0, 0}, 6, 0},    // RenderLeafs: the same
    {0x6ec1bf, {0x48, 0x8d, 0x3d}, 3, kBbTexCoords + 8}, // UploadTexCoords: lea rdi, the tex-coord table + 8
    {kTreeAdmin, {0x48, 0x83, 0xec, 0x38, 0x80, 0x3d}, 6, 0},  // the getters: sub rsp, 0x38; cmp byte [ms_bInitialised], 1
    {kVegAdmin, {0x48, 0x83, 0xec, 0x38, 0x80, 0x3d}, 6, 0},
};

}  // namespace

bool StaticOps::verifyTrees() {
    if (engine_ == nullptr) return false;
    for (const Site& s : kSites) {
        std::array<std::uint8_t, 8> got{};
        const bool rel = s.target != 0;
        const std::uint32_t len = rel ? s.n + 4 : s.n;
        if (!safeRead(engine_ + s.rva, got.data(), len) || !std::equal(got.begin(), got.begin() + s.n, s.b.begin())) {
            spdlog::warn("native: scene ops: trees: the code at RVA {:#x} differs: the trees / vegetation stay the engine's", s.rva);
            return false;
        }
        if (rel) {
            std::int32_t r = 0;
            std::memcpy(&r, got.data() + s.n, 4);
            if (static_cast<std::int64_t>(s.rva) + len + r != s.target) {
                spdlog::warn("native: scene ops: trees: RVA {:#x} does not reach {:#x}: the trees / vegetation stay the engine's", s.rva, s.target);
                return false;
            }
        }
    }
    constexpr std::pair<std::uint32_t, std::uint32_t> kSlots[] = {{0x220, kSetTreeInstance}, {0x228, kSetVegetationAlpha}, {0x238, kSetBbTexCoords}};
    for (const auto& [off, fn] : kSlots) {
        const void* v = nullptr;
        if (!readAt(engine_ + 0x97bdb0, off, v) || v != engine_ + fn) {
            spdlog::warn("native: scene ops: trees: eCShaderDefault vtable +{:#x} is not RVA {:#x}: the trees / vegetation stay the engine's", off, fn);
            return false;
        }
    }
    // eCShaderLeafs (the leaves' material class): its vtable must hold eCShaderDefault's functions in every slot the composition
    // relies on (+0xf0 .. +0x238 but +0x230; it overrides GetObjectType, Create, GetEllementTypeName and GetShaderFileName only)
    const auto* leafs = static_cast<const std::uint8_t*>(engine::sym().shaderLeafsVftable);
    if (leafs == engine_ + kLeafsVtable) {
        bool same = true;
        for (std::uint32_t off = 0xf0; off <= 0x238 && same; off += 8) {
            if (off == 0x230) continue;  // GetShaderFileName (its own .fx: compiling only)
            const void* a = nullptr;
            const void* b = nullptr;
            same = readAt(engine_ + 0x97bdb0, off, a) && readAt(leafs, off, b) && a == b;
        }
        if (same) leafsVtable_ = leafs;
    }
    if (leafsVtable_ == nullptr) spdlog::warn("native: scene ops: trees: eCShaderLeafs' vtable differs: the leaves stay the engine's");
    treeAdmin_ = reinterpret_cast<void* (*)()>(const_cast<std::uint8_t*>(engine_ + kTreeAdmin));
    vegAdmin_ = reinterpret_cast<void* (*)()>(const_cast<std::uint8_t*>(engine_ + kVegAdmin));
    treeCmp_ = reinterpret_cast<int (*)(const void*, const void*)>(const_cast<std::uint8_t*>(engine_ + kSortInstances));
    treeCmpZ_ = reinterpret_cast<int (*)(const void*, const void*)>(const_cast<std::uint8_t*>(engine_ + kSortInstancesZ));
    bbTexCoords_ = engine_ + kBbTexCoords;
    if (qsort_ == nullptr) return false;
    spdlog::info("native: scene ops: tree / vegetation checks passed (the queue sorts, the special-pass options, the instancing mode reads, "
                 "the billboard tex-coord table, the admins' getters, SetSpeedTreeInstanceData / SetVegationAlpha / "
                 "SetSpeedTreeBillboardTexCoords slots)");
    return true;
}

bool StaticOps::viewAndMirror(const std::uint8_t* shader) {
    U8* admin = ptr(shader, 0x220);
    if (!view_.valid) {
        if (admin == nullptr || !composeView(admin)) return false;
        if (view_.dbOrder < 0) spdlog::warn("native: static ops: the depth-bias view-projection matches neither product order");
    }
    if (admin != view_.admin) return false;
    initMirror();
    return true;
}

// eCSpeedTreeAdmin::UpdateWindMatrices 0x6efa10 -> eCShaderMaterialAdmin::SetSpeedTreeData 0x6b7df0 (during the database's Request,
// once per execution; nothing when no eCSpeedTreeWind_PS exists: +0x58 == 0): c14 + 3 i .. = the 3x3 transposes of the wind matrices
// (+0xac, stride 0x40), c23..c25 = that of the application camera's matrix (+0x10 of eCApplication +0x38, else +0xc8), c26..c29 the
// rock / rustle angles (+0x6c, 4 x vec4); the .w of c14..c25 is not written
void StaticOps::treeWind() {
    U8* admin = static_cast<U8*>(treeAdmin_ != nullptr ? treeAdmin_() : nullptr);
    if (admin == nullptr || get<std::int32_t>(admin, 0x58) == 0) return;
    U8* app = ptr(engine::sym().appInstancePtr, 0);
    U8* cam = ptr(app, 0x38);
    if (cam == nullptr) cam = ptr(app, 0xc8);
    float m[4][16];
    if (!rd(admin, 0xac, m, 3 * 64) || !rd(cam, 0x10, m[3], 64)) return;
    const auto transpose3 = [&](const float* a, std::uint32_t reg) {
        for (std::uint32_t k = 0; k < 3; ++k) {
            mirror_.adminVs[reg + k][0] = a[k];
            mirror_.adminVs[reg + k][1] = a[4 + k];
            mirror_.adminVs[reg + k][2] = a[8 + k];
        }
    };
    for (std::uint32_t i = 0; i < 3; ++i) transpose3(m[i], 14 + 3 * i);
    transpose3(m[3], 23);
    if (!rd(admin, 0x6c, mirror_.adminVs[26], 64)) return;
    nativeVs_ |= 0xffffu << 14;  // c14..c29
}

// eCVegetationAdmin::Process 0x7016b0 (every frame, before rendering): c12 = (game time (eCTimer +0x10, ms) / 1000, admin +0x44,
// +0x50, +0x54)
void StaticOps::grassWind() {
    U8* admin = static_cast<U8*>(vegAdmin_ != nullptr ? vegAdmin_() : nullptr);
    const auto getTimer = reinterpret_cast<U8* (*)()>(engine::sym().timerGetInstance);
    if (admin == nullptr || getTimer == nullptr) return;
    U8* timer = getTimer();
    const std::uint32_t ms = get<std::uint32_t>(timer, 0x10);
    const float c12[4] = {static_cast<float>(ms) / 1000.0f, get<float>(admin, 0x44), get<float>(admin, 0x50), get<float>(admin, 0x54)};
    // Process wrote the game time of its own moment: when the clock moved between Process and this op (a scenario setting the game
    // time, the time freeze) the pool's copy is the value the shader gets, so it stays (counted)
    if (std::memcmp(c12, mirror_.adminVs[12], 16) != 0) {
        ++treeWindow_.grassClockMoved;
        return;
    }
    nativeVs_ |= 1u << 12;
}

// ---- SpeedTree ------------------------------------------------------------------------------------------------------------------

// eCRenderContextSpeedTree::Render 0x6ee320 (desc = the geometry type) / RenderZPass 0x6ee3f0 (desc = request mask << 16 | type): the
// tree resource's render buffer (PS +0x38 -> +0x78) draws every queued tree of the resource for that type
StaticOps::Skip StaticOps::composeTree(const Op& op) {
    TreeStats& st = treeWindow_;
    ++st.ops;
    const bool zpass = op.list == List::ZPass;
    if (!zpass && op.list != List::Solid) return Skip::Kind;
    // a shadow-map z op (desc bit 0x10 << 16, the shadow views of N5.5): RenderZPass 0x6ee3f0 passes it to the z passes
    const bool shadow = zpass && ((op.raw.desc >> 16) & 0x10u) != 0;
    U8* ps = ptr(op.raw.context, 0x18);
    U8* res = ptr(ps, 0x38);
    if (res == nullptr) return Skip::None;
    U8* rb = ptr(res, 0x78);
    if (rb == nullptr) return Skip::Unreadable;
    const std::uint32_t type = zpass ? (op.raw.desc & 0xffffu) : op.raw.desc;
    if (type > 3) return Skip::None;
    ++st.types[type];
    const std::size_t t = type;
    // RenderFrondsZPass 0x6f0cc0 in a shadow map: nothing unless the leaf cards' vertex count (+0x3c) is <= the fronds' (+0x3a)
    if (shadow && type == 1 && get<std::uint16_t>(rb, 0x3c) > get<std::uint16_t>(rb, 0x3a)) {
        ++st.shadowFrondsSkipped;
        ++st.zeroOps;
        return Skip::None;
    }
    // the queue (solid +0xb8 / +0xc0, z +0xd0 / +0xd8; entries 0x18: tree PS, illumination, depth key), sorted at the first Render /
    // RenderZPass of the frame (the stamp +0xe0 / +0xe4 is non-zero until then)
    const std::int32_t n = get<std::int32_t>(rb, zpass ? 0xd8 : 0xc0);
    U8* visual = ptr(rb, 0x18 + 8 * t);
    U8* wrapper = ptr(rb, 8);
    if (n == 0 || visual == nullptr || wrapper == nullptr) return Skip::None;
    if (n < 0 || n > 65536) return Skip::Unreadable;
    U8* material = ptr(rb, 0x80 + 8 * t);
    U8* shader = ptr(material, 0x70);  // after RequestData(material, 1)
    if (shader == nullptr) return Skip::None;
    if (!shaderClassOk(shader)) {
        skipShader_ = get<const void*>(shader, 0);
        return Skip::ShaderClass;
    }  // the engine draws other classes without the TransformationType check
    const std::int32_t transformation = get<std::int32_t>(shader, 0x2b8);
    const bool ttOk = type == 0 ? transformation == 3 : type == 1 ? transformation == 4 : (transformation == 5 || transformation == 6);
    if (!ttOk) {
        // "Cant render speedtree ... TransformationType is not set to ...": logged, nothing drawn
        ++st.wrongTransformation;
        return Skip::None;
    }
    if (!viewAndMirror(shader)) return Skip::Unreadable;
    std::vector<std::uint8_t> queue(static_cast<std::size_t>(n) * 0x18);
    if (!rd(ptr(rb, zpass ? 0xd0 : 0xb8), 0, queue.data(), queue.size())) return Skip::Unreadable;
    if (get<std::int32_t>(rb, zpass ? 0xe4 : 0xe0) != 0 && n > 0) {
        qsort_(queue.data(), static_cast<std::size_t>(n), 0x18, zpass ? treeCmpZ_ : treeCmp_);
        ++st.sortedCopies;
    }
    const auto entry = [&](std::uint32_t i) { return queue.data() + 0x18 * static_cast<std::size_t>(i); };
    const auto treeOf = [&](std::uint32_t i) { U8* p = nullptr; std::memcpy(&p, entry(i), 8); return p; };
    const auto illumOf = [&](std::uint32_t i) { U8* p = nullptr; std::memcpy(&p, entry(i) + 8, 8); return p; };
    // MaskReference forced to 80 (with the property notifies) before any draw
    maskRefShader_ = shader;
    maskRefOverride_ = 0x50;
    U8* admin = view_.admin;
    const bool hardware = get<std::int32_t>(admin, 0x320) != 0;
    ++(hardware ? st.hardware : st.software);
    const std::uint8_t limit = get<std::uint8_t>(rb, 0x128 + t);
    Geom g;
    const auto* record = hal::vulkan::HalPrimitive::fromApi(ptr(visual, 0x10));
    if (record == nullptr || !geometryOfVisual(visual, g)) return Skip::Primitive;
    const auto& dd = record->drawDesc();
    if (!dd.indexed) return Skip::Primitive;
    g.primType = static_cast<std::uint32_t>(dd.type);
    g.baseVertex = dd.baseVertexIndex;
    g.minIndex = dd.minVertexIndex;
    g.material = material;
    g.subMesh = nullptr;  // no eCSubMesh::Render: the slot's tag only
    g.meshResource = nullptr;
    treeWind();
    const std::uint16_t verts = get<std::uint16_t>(rb, 0x38 + 2 * t);
    const auto entityWorld = [&](U8* tree, float out[16]) {
        U8* entity = (get<std::uint8_t>(tree, 0x18) & 0x10) == 0 ? ptr(tree, 0x10) : nullptr;
        return entity != nullptr && rd(entity, 0x60, out, 64);
    };
    const auto drawBatch = [&](const sc::TreeRange& r, U8* illum) -> Skip {
        Geom gg = g;
        gg.numVertices = r.numVertices;
        gg.startIndex = r.startIndex;
        gg.primCount = r.primCount;
        if (r.freq0 != 0 && gg.streams[0].buffer != nullptr) gg.streams[0].freq = r.freq0;
        if (zpass) return specialPassDraw(shader, gg, true);
        // GetPasses(shader, 0, illumination, 0, instanced, 0)
        return materialDraws(shader, illum, 0, true, 0, 0, gg, nullptr);
    };
    std::vector<sc::TreeInstance> inst;
    std::vector<sc::TreeBatch> batches;
    if (type <= 1) {
        // branches / fronds: SetGlobalAlpha(1, 0) once (RenderBranches, RenderBranchesZPass, RenderFronds; RenderFrondsZPass does not);
        // per instance SetWorldMatrix + SetSpeedTreeInstanceData((tree +0x6c, rb +0xa0 / +0xa4))
        if (!(zpass && type == 1)) writeAlpha(0, one_);
        const std::size_t lodAt = type == 0 ? 0x48 : 0x50, stripAt = type == 0 ? 0x40 : 0x50, startAt = type == 0 ? 0xe8 : 0xf8;
        const float vy = get<float>(rb, type == 0 ? 0xa0 : 0xa4);
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(n); ++i) {
            U8* tree = treeOf(i);
            if (ptr(tree, 0x40) == nullptr) return Skip::Unreadable;  // the engine reads the LOD values through it
            const std::int32_t lod = get<std::int32_t>(tree, lodAt, -1);
            if (lod < 0) continue;
            sc::TreeInstance x;
            x.queueIndex = i;
            x.strip = get<std::uint16_t>(rb, stripAt + 2 * static_cast<std::size_t>(lod));
            x.start = get<std::uint16_t>(rb, startAt + 2 * static_cast<std::size_t>(lod));
            if (!zpass) {
                U8* il = illumOf(i);
                x.list = ptr(il, 0);
                x.alone = get<std::uint8_t>(il, 0x90) != 0 || get<std::uint8_t>(il, 0xd) != 0;
            }
            inst.push_back(x);
        }
        sc::planTreeBatches(inst, limit, !zpass, true, batches);
        for (const sc::TreeBatch& b : batches) {
            for (std::uint32_t k = 0; k < b.count; ++k) {
                U8* tree = treeOf(inst[b.first + k].queueIndex);
                float world[16];
                if (!entityWorld(tree, world)) return Skip::Unreadable;
                writeWorld(k, world);
                const float v[2] = {get<float>(tree, 0x6c), vy};
                writeTreeInstance(k, v);
            }
            st.instances += b.count;
            ++st.batches;
            // fronds: a batch whose strip length is 0 is not drawn (RenderFronds 0x6f1b0f / RenderFrondsZPass)
            if (type == 1 && b.strip == 0) {
                ++st.undrawnBatches;
                continue;
            }
            const sc::TreeRange r = sc::treeRange(type == 0 ? sc::TreeGeometry::Branches : sc::TreeGeometry::Fronds, hardware, b.count, b.strip, b.start, verts, 0);
            if (const Skip s = drawBatch(r, zpass ? nullptr : illumOf(inst[b.first + b.count - 1].queueIndex)); s != Skip::None) return s;
        }
    } else {
        // leaves: per leaf LOD of the tree (FUN_1802afb00 = CSpeedTreeRT +0xf4 count) the trees showing it (+0x58 alpha +0x60, else
        // +0x5c alpha +0x64), SetGlobalAlpha(80 / alpha), SetSpeedTreeInstanceData(wrapper +0x34); a LOD without indices ends the op
        U8* rt = ptr(wrapper, 0);
        const std::int32_t lods = get<std::int32_t>(ptr(rt, 8), 0xf4);
        float v[2];
        if (!rd(wrapper, 0x34, v, 8)) return Skip::Unreadable;
        const float fixedRef = 80.0f;  // DAT_1809f1790
        for (std::int32_t lodi = 0; lodi < lods; ++lodi) {
            const std::size_t at = 0x40 + 0x10 * t + 2 * static_cast<std::size_t>(lodi);
            const std::uint16_t indices = get<std::uint16_t>(rb, at);
            const std::uint16_t start = get<std::uint16_t>(rb, at + 0xa8);
            if (indices == 0) break;
            ++st.leafLods;
            inst.clear();
            std::vector<float> alpha;
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(n); ++i) {
                U8* tree = treeOf(i);
                if (ptr(tree, 0x40) == nullptr) return Skip::Unreadable;
                float a = 0.0f;
                const std::int32_t l0 = get<std::int32_t>(tree, 0x58, -1), l1 = get<std::int32_t>(tree, 0x5c, -1);
                if (shadow) {
                    // RenderLeafsZPass 0x6f0860 in a shadow map: the one leaf LOD min(+0x58 + 1, LODs - 1), global alpha 1
                    const std::int32_t l = l0 < 0 ? -1 : std::min(l0 + 1, lods - 1);
                    if (l != lodi) continue;
                    a = one_;
                } else if (l0 == lodi && l0 > -1) {
                    a = fixedRef / get<float>(tree, 0x60);
                } else if (l1 == lodi && l1 > -1) {
                    a = fixedRef / get<float>(tree, 0x64);
                } else {
                    continue;
                }
                sc::TreeInstance x;
                x.queueIndex = i;
                if (!zpass) {
                    U8* il = illumOf(i);
                    x.list = ptr(il, 0);
                    x.alone = get<std::uint8_t>(il, 0x90) != 0 || get<std::uint8_t>(il, 0xd) != 0;
                }
                inst.push_back(x);
                alpha.push_back(a);
            }
            sc::planTreeBatches(inst, limit, !zpass, false, batches);
            for (const sc::TreeBatch& b : batches) {
                for (std::uint32_t k = 0; k < b.count; ++k) {
                    U8* tree = treeOf(inst[b.first + k].queueIndex);
                    float world[16];
                    if (!entityWorld(tree, world)) return Skip::Unreadable;
                    writeWorld(k, world);
                    writeAlpha(k, alpha[b.first + k]);
                    writeTreeInstance(k, v);
                }
                st.instances += b.count;
                ++st.batches;
                const sc::TreeRange r = sc::treeRange(sc::TreeGeometry::Leaves, hardware, b.count, 0, start, verts, indices);
                if (const Skip s = drawBatch(r, zpass ? nullptr : illumOf(inst[b.first + b.count - 1].queueIndex)); s != Skip::None) return s;
            }
        }
    }
    if (drawCount_ == 0) ++st.zeroOps;
    return Skip::None;
}

// ---- tree billboards ------------------------------------------------------------------------------------------------------------

// eCRenderContextSpeedTreeBB: desc = cell << 8 | sub-mesh of the area's billboard mesh (eCSpeedTreeBBMesh_PS +0x30)
StaticOps::Skip StaticOps::composeTreeBillboard(const Op& op) {
    TreeStats& st = treeWindow_;
    ++st.billboardOps;
    const bool zpass = op.list == List::ZPass;
    if (!zpass && op.list != List::Solid) return Skip::Kind;
    U8* ps = ptr(op.raw.context, 0x18);
    if (ps == nullptr) return Skip::None;
    U8* res = ptr(ps, 0x30);
    if (res == nullptr) return Skip::Unreadable;
    const std::uint32_t sub = op.raw.desc & 0xffu, cell = op.raw.desc >> 8;
    const std::int32_t subs = get<std::int32_t>(res, 0x60);
    if (sub == 0xff || static_cast<std::int32_t>(sub) >= subs) return Skip::Unreadable;
    U8* subMesh = ptr(res, 0x58) + 0x48 * static_cast<std::size_t>(sub);
    U8* material = ptr(subMesh, 0x38);
    U8* shader = ptr(material, 0x70);
    if (shader == nullptr) {
        // UploadTexCoords and the mesh paths draw nothing without the sub-mesh's shader
        return Skip::None;
    }
    if (!shaderClassOk(shader)) {
        skipShader_ = get<const void*>(shader, 0);
        return Skip::ShaderClass;
    }
    if (!viewAndMirror(shader)) return Skip::Unreadable;
    // UploadTexCoords(sub-mesh): c32..c36 = the 5 fixed vec4 (0xbfcbd0), c37 + j = the PS's vec4 list (+0x40) from the sub-mesh's
    // range (+0x58: 0xc-byte entries, +4 first, +8 count)
    if (subs != 0) {
        float fixed[5][4];
        if (!rd(bbTexCoords_, 0, fixed, sizeof(fixed))) return Skip::Unreadable;
        for (std::uint32_t k = 0; k < 5; ++k) std::memcpy(mirror_.global[1 + k], fixed[k], 16);
        U8* ranges = ptr(ps, 0x58);
        const std::uint32_t first = get<std::uint32_t>(ranges, 0xc * static_cast<std::size_t>(sub) + 4);
        const std::uint32_t count = get<std::uint32_t>(ranges, 0xc * static_cast<std::size_t>(sub) + 8);
        if (5 + count > 200) return Skip::Unreadable;
        for (std::uint32_t j = 0; j < count; ++j) {
            if (!rd(ptr(ps, 0x40), 16 * static_cast<std::size_t>(static_cast<std::int32_t>(first + j)), mirror_.global[6 + j], 16)) return Skip::Unreadable;
        }
        ++st.billboardTexCoords;
    }
    if (zpass) {
        // RenderZPass(null world, sub-mesh, cell, false) then (..., true): two special-pass options (VS variants)
        if (const Skip s = meshZDraws(res, sub, cell, nullptr, false); s != Skip::None) return s;
        return meshZDraws(res, sub, cell, nullptr, true);
    }
    // eCMeshResource2::Render(null world, sub-mesh, cell, switch 0, illumination = entity +0xf8 +0x78, null fade, flags 0)
    U8* entity = ptr(op.raw.context, 0x10);
    U8* illumPs = ptr(entity, 0xf8);
    U8* illum = illumPs != nullptr ? illumPs + 0x78 : nullptr;
    if (ptr(res, 0x88) == nullptr) return Skip::None;
    const std::uint32_t combi = bindLightmaps(res);
    return subMeshDraws(res, subMesh, cell, 0, combi, illum, 1.0f, nullptr, false, 0, 0);
}

// ---- vegetation -----------------------------------------------------------------------------------------------------------------

// eCVegetation_PS::Render(type, zpass) 0x707800: the visible grid nodes (PS +0x98, count +0xa0, 0x118-byte entries) in order, each
// node's instances of the type copied into the admin's shared buffer, RenderMeshInstanced 0x707e40 per batch
StaticOps::Skip StaticOps::composeVegetation(const Op& op) {
    TreeStats& st = treeWindow_;
    ++st.grassOps;
    const bool zpass = op.list == List::ZPass;
    if (!zpass && op.list != List::Solid) return Skip::Kind;
    U8* ps = ptr(op.raw.context, 0x18);
    U8* vadmin = static_cast<U8*>(vegAdmin_ != nullptr ? vegAdmin_() : nullptr);
    if (ps == nullptr || vadmin == nullptr) return Skip::Unreadable;
    const std::int32_t type = static_cast<std::int32_t>(op.raw.desc);
    const std::int32_t nodeCount = get<std::int32_t>(ps, 0xa0);
    if (nodeCount < 1) {
        ++st.grassZeroOps;
        return Skip::None;
    }
    if (nodeCount > 100000) return Skip::Unreadable;
    U8* typeObj = ptr(ptr(ps, 0x38), 8 * static_cast<std::size_t>(static_cast<std::uint16_t>(type)));
    if (typeObj == nullptr) return Skip::Unreadable;  // Render reads the type's flags unconditionally
    U8* mesh = ptr(typeObj, 0x58);
    U8* vd = ptr(vadmin, 0x38);
    U8* nodes = ptr(ps, 0x98);
    // the nodes and the instance data in Render's order
    std::vector<sc::GrassNode> plan(static_cast<std::size_t>(nodeCount));
    std::vector<U8*> sources(static_cast<std::size_t>(nodeCount));
    std::uint32_t total = 0;
    for (std::int32_t i = 0; i < nodeCount; ++i) {
        U8* e = nodes + 0x118 * static_cast<std::size_t>(i);
        sc::GrassNode& gn = plan[static_cast<std::size_t>(i)];
        gn.lightSet = ptr(e, 0);
        gn.alone = get<std::uint8_t>(e, 0xa0) != 0 || get<std::uint8_t>(e, 0x1d) != 0;
        U8* grid = ptr(e, 8);
        U8* range = ptr(grid, 0x30) + 8 * static_cast<std::size_t>(static_cast<std::int64_t>(type));
        gn.count = get<std::uint32_t>(range, 4);
        if (gn.count != 0) {
            sources[static_cast<std::size_t>(i)] = ptr(grid, 0x18) + sc::kGrassInstanceSize * static_cast<std::size_t>(get<std::int32_t>(range, 0));
            total += gn.count;
        }
    }
    st.grassNodes += static_cast<std::uint32_t>(nodeCount);
    st.grassInstances += total;
    const std::uint32_t cursor = get<std::uint32_t>(vadmin, 0x40);
    if (cursor > sc::kGrassBufferInstances) return Skip::Unreadable;
    std::vector<sc::GrassDraw> draws;
    sc::planGrass(plan, cursor, zpass, draws);
    // the instance bytes, flattened in the nodes' order
    const bool wind = get<float>(typeObj, 0x2c) != 0.0f;
    const bool lift = get<std::int32_t>(typeObj, 0x20) != 0;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(total) * sc::kGrassInstanceSize);
    {
        std::size_t at = 0;
        std::vector<std::uint8_t> src;
        for (std::int32_t i = 0; i < nodeCount; ++i) {
            const sc::GrassNode& gn = plan[static_cast<std::size_t>(i)];
            if (gn.count == 0) continue;
            src.resize(static_cast<std::size_t>(gn.count) * sc::kGrassInstanceSize);
            if (!safeRead(sources[static_cast<std::size_t>(i)], src.data(), src.size())) return Skip::Unreadable;
            for (std::uint32_t k = 0; k < gn.count; ++k, at += sc::kGrassInstanceSize) {
                sc::grassInstance(src.data() + sc::kGrassInstanceSize * static_cast<std::size_t>(k), wind, lift, bytes.data() + at);
            }
        }
    }
    for (const sc::GrassDraw& d : draws) {
        if (d.bufferOffset + d.count == sc::kGrassBufferInstances) ++st.grassWraps;
    }
    // RenderMeshInstanced: the mesh cached with its visual, the shared buffer, the first sub-mesh's shader
    if (mesh == nullptr || get<std::int32_t>(mesh, 0x38) != 3 || ptr(mesh, 0x88) == nullptr || vd == nullptr) {
        if (!draws.empty()) ++st.grassNotCached;
        ++st.grassZeroOps;
        return Skip::None;
    }
    U8* sub0 = ptr(mesh, 0x58);
    U8* material = ptr(sub0, 0x38);
    U8* shader = ptr(material, 0x70);
    if (draws.empty() || shader == nullptr) {
        ++st.grassZeroOps;
        return Skip::None;
    }
    if (!shaderClassOk(shader)) {
        skipShader_ = get<const void*>(shader, 0);
        return Skip::ShaderClass;
    }
    if (!viewAndMirror(shader)) return Skip::Unreadable;
    grassWind();
    U8* visual = ptr(mesh, 0x88);
    Geom g;
    if (!geometryOfVisual(visual, g)) return Skip::Primitive;
    const auto* record = hal::vulkan::HalPrimitive::fromApi(ptr(visual, 0x10));
    const auto* data = hal::vulkan::asVertexData(const_cast<hal::eCAPIVertexData*>(reinterpret_cast<const hal::eCAPIVertexData*>(ptr(vd, 0x20))));
    if (record == nullptr || data == nullptr || data->buffer() == nullptr) return Skip::Primitive;
    // cull 2 - two-sided (type +0x30); the draw description of the first sub-mesh (indexed list); SetVegationAlpha(grid +0x50, +0x54)
    g.visual.cull = 2u - (get<std::uint8_t>(typeObj, 0x30) != 0 ? 1u : 0u);
    g.primType = 4;
    g.baseVertex = record->drawDesc().baseVertexIndex;
    g.minIndex = get<std::uint32_t>(sub0, 0x30);
    g.startIndex = get<std::uint32_t>(sub0, 0x28);
    g.numVertices = get<std::uint32_t>(sub0, 0x34);
    g.primCount = get<std::uint32_t>(sub0, 0x2c) / 3;
    g.material = material;
    g.subMesh = nullptr;  // no eCSubMesh::Render: the slot's tag only
    g.meshResource = nullptr;
    U8* grid = ptr(ps, 0x90);
    float ax = 0.0f, ay = 0.0f;
    sc::vegetationAlpha(get<float>(grid, 0x50), get<float>(grid, 0x54), maskRefOf(shader), ax, ay);
    const bool stream1 = record->streamView(1).enabled;
    for (const sc::GrassDraw& d : draws) {
        mirror_.global[0][0] = ax;  // c31.xy
        mirror_.global[0][1] = ay;
        writeAlpha(0, one_);        // c32.w = 1
        Geom gg = g;
        if (gg.streams[0].buffer != nullptr) gg.streams[0].freq = d.count | 0x40000000u;
        // AttachVertexData(1, the shared buffer): the record's stream 1 = its buffer with its stride, at the batch's byte offset,
        // INSTANCEDATA | 1
        if (stream1) gg.streams[1] = sc::DrawDesc::Stream{data->buffer(), d.bufferOffset * sc::kGrassInstanceSize, data->stride(), 0x80000001u};
        U8* illum = d.illumNode >= 0 ? nodes + 0x118 * static_cast<std::size_t>(d.illumNode) + 0x10 : nullptr;
        const std::size_t before = drawCount_;
        const Skip s = zpass ? specialPassDraw(shader, gg, false) : materialDraws(shader, illum, 0, false, 0, 0, gg, nullptr);
        if (s != Skip::None) return s;
        for (std::size_t k = before; k < drawCount_; ++k) {
            sc::DrawDesc& dd = *draws_[k];
            dd.cpuStream = 1;
            dd.vertexStride = sc::kGrassInstanceSize;
            const auto* b0 = bytes.data() + static_cast<std::size_t>(d.firstInstance) * sc::kGrassInstanceSize;
            dd.vertexBytes.assign(b0, b0 + static_cast<std::size_t>(d.count) * sc::kGrassInstanceSize);
        }
        ++st.grassDraws;
    }
    return Skip::None;
}

}  // namespace rvk::native
