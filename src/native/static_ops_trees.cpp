// N5.4 scene ops: the main view's SpeedTree ops (eCRenderContextSpeedTree: branches, fronds, leaf cards, leaf meshes), the tree
// billboards' ops (eCRenderContextSpeedTreeBB) and the vegetation ops (eCRenderContextVegetation); docs/research/engine-frame/
// README.md "11. N5.4 result", D-contexts.md 2-4. Re-written from eCSpeedTreeRenderBuffer::RenderBranches 0x6f1cd0 / RenderFronds
// 0x6f1880 / RenderLeafs 0x6f1390 and their z passes (0x6f1030 / 0x6f0cc0 / 0x6f0860), eCRenderContextSpeedTreeBB::Render 0x6ee8d0 /
// RenderZPass 0x6ee8e0 + eCSpeedTreeBBMesh_PS::UploadTexCoords 0x6ec150, eCVegetation_PS::Render 0x707800 / RenderMeshInstanced
// 0x707e40 (dumps B:\DevDrive\risen-ghidra\frameD_dump1..3.txt + capstone disassembly). Part of StaticOps (static_ops.h); the pass
// rules are static_ops.cpp's, the batch rules static_compose's (planTreeBatches, treeRange, planGrass).

#include <windows.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "d3d9/defs.h"
#include "engine/memread.h"
#include "engine/illum_census.h"
#include "engine/symbols.h"
#include "hal/vulkan/hal_vulkan.h"
#include "enh/light_table.h"
#include "native/f4_measure.h"
#include "native/static_ops.h"
#include "core/settings.h"

namespace rvk::native {

namespace {

using engine::readAt;
using engine::safeRead;
using U8 = const std::uint8_t;

template <class T>
inline T get(const void* p, std::size_t off, T fallback = T{}) {
    T v = fallback;
    if (!readAt(p, off, v)) return fallback;
    return v;
}
inline U8* ptr(const void* p, std::size_t off) { return get<U8*>(p, off, nullptr); }
inline bool rd(const void* p, std::size_t off, void* out, std::size_t n) { return p != nullptr && safeRead(static_cast<U8*>(p) + off, out, n); }

// F2: an engine object's first N bytes read once (one SEH frame, one copy) instead of one guarded load per field (native.cpu_opts bit 2)
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
    const f4m::OpScope f4scope(&drawCount_, dev_.stats().frames, static_cast<std::uint32_t>(viewKind_), cascade_, db_, desc_, zpass, shadow);
    U8* ps = ptr(op.raw.context, 0x18);
    U8* res = ptr(ps, 0x38);
    if (res == nullptr) return Skip::None;
    U8* rb = ptr(res, 0x78);
    if (rb == nullptr) return Skip::Unreadable;
    // O2b lean: the queue sorted in place at the render buffer's first op (before the type dispatch, as the slot does it)
    if (leanCollect_) {
        const f4m::PhaseScope f4q(f4m::kQueue);
        leanTreeSort(rb, zpass);
    }
    const std::uint32_t type = zpass ? (op.raw.desc & 0xffffu) : op.raw.desc;
    if (type > 3) return Skip::None;
    ++st.types[type];
    const std::size_t t = type;
    // RenderFrondsZPass 0x6f0cc0 in a shadow map: nothing unless the leaf cards' vertex count (+0x3c) is <= the fronds' (+0x3a)
    if (shadow && type == 1 && get<std::uint16_t>(rb, 0x3c) > get<std::uint16_t>(rb, 0x3a)) {
        ++st.shadowFrondsSkipped;
        ++st.zeroOps;
        // F3-4 (frame-ownership.md 14.29): an owned shadow view keeps an empty record of the op, so its owned frames take it out of the list too (its slot does nothing but the sort)
        if (ownedTrees_ && tro_ != nullptr && ownedShadowViews_ && ownedShadowKind(viewKind_) && tro_->viewKey == ownedKeyNow() && tro_->viewKey != 255) treeEmptyRecord(op, res, rb);
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
    if (leanCollect_) touchNow(material);  // O2b lean: RequestData(material, 1) before the shader is read
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
    if (leanCollect_) leanMaskRef(shader);  // O2b lean: MaskReference = 80 (the RT material reads it) before any draw
    if (!viewAndMirror(shader)) return Skip::Unreadable;
    std::vector<std::uint8_t>& queue = scr_.treeQueue;
    {
        const f4m::PhaseScope f4q(f4m::kQueue);
        queue.resize(static_cast<std::size_t>(n) * 0x18);
        if (!rd(ptr(rb, zpass ? 0xd0 : 0xb8), 0, queue.data(), queue.size())) return Skip::Unreadable;
        if (get<std::int32_t>(rb, zpass ? 0xe4 : 0xe0) != 0 && n > 0) {
            qsort_(queue.data(), static_cast<std::size_t>(n), 0x18, zpass ? treeCmpZ_ : treeCmp_);
            ++st.sortedCopies;
        }
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
    const bool fastReads = opt(2);
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
    // F4-2 / F4-3: a main-view tree op is retained (templates per bucket) / planned for the check; our identity ramp replaces the engine's stream 1 (the same values)
    // (F4-4: a sun cascade's z op too, while native.owned_shadows is on and the cascades' trees are not tripped)
    // (F3-4: the terrain map's / an atlas cell's z op too, while native.owned_shadow_views is on)
    const bool trOn = ownedTrees_ && tro_ != nullptr && hardware && ((viewKind_ == ViewKind::Main && !shadow) || (ownedShadowKind(viewKind_) && shadow)) &&
                      tro_->viewKey == ownedKeyNow();
    if (trOn && tro_->ramp != nullptr && g.streams[1].buffer != nullptr && g.streams[1].freq == 0x80000001u && g.streams[1].stride == 4u) {
        g.streams[1].buffer = tro_->ramp.get();
        g.streams[1].offset = 0;
    }
    if (f4m::on()) {
        f4m::OpInfo oi;
        oi.type = type;
        oi.hardware = hardware;
        oi.limit = limit;
        oi.rb = rb;
        oi.admin = admin;
        oi.queue = queue.data();
        oi.n = n;
        oi.leafLods = type >= 2 ? get<std::int32_t>(ptr(ptr(wrapper, 0), 8), 0xf4) : 0;
        oi.shader = shader;
        oi.vb = g.streams[0].buffer;
        oi.ib = g.indices;
        oi.primType = g.primType;
        f4m::census(oi);
    }
    treeWind();
    const std::uint16_t verts = get<std::uint16_t>(rb, 0x38 + 2 * t);
    const auto entityWorld = [&](U8* tree, float out[16]) {
        U8* entity = (get<std::uint8_t>(tree, 0x18) & 0x10) == 0 ? ptr(tree, 0x10) : nullptr;
        return entity != nullptr && rd(entity, 0x60, out, 64);
    };
    const auto drawBatch = [&](const sc::TreeRange& r, U8* illum, std::uint64_t treeKey) -> Skip {
        Geom gg = g;
        gg.numVertices = r.numVertices;
        gg.startIndex = r.startIndex;
        gg.primCount = r.primCount;
        if (r.freq0 != 0 && gg.streams[0].buffer != nullptr) gg.streams[0].freq = r.freq0;
        const f4m::PhaseScope f4d(f4m::kDraw);
        const std::size_t f4before = drawCount_;
        Skip res = Skip::None;
        if (zpass) {
            res = specialPassDraw(shader, gg, true);
        } else {
            // GetPasses(shader, 0, illumination, 0, instanced, 0)
            const engine::illumcensus::KindScope census(1);
            res = materialDraws(shader, illum, 0, true, 0, 0, gg, nullptr);
        }
        if (f4m::on()) {
            for (std::size_t k = f4before; k < drawCount_; ++k) {
                const sc::DrawDesc& dd = *draws_[k];
                f4m::draw(dd.vs, dd.ps, dd.streams[1].buffer, dd.streams[1].offset, dd.streams[1].stride, dd.streams[1].freq, dd.streams[0].freq);
            }
        }
        if (trOn) treeCompBatch(f4before, treeKey);
        return res;
    };
    // the batch rules' light inputs of an instance (its illumination: list, per-object flags; with table lighting also the sun bit of the pass)
    const bool tableMerge = tableLit_ && !zpass && enh::fullLitTreeMerge();
    if (trOn) treeCompBegin(op, zpass, type, rb, material, shader, hardware, tableMerge, queue.data(), n, wrapper);
    const U8* sunList = nullptr;
    bool sunListValue = false, dynListValue = false;
    const auto readLight = [&](sc::TreeInstance& x, U8* il) {
        const f4m::PhaseScope f4l(f4m::kLight);
        bool noSun = false, statLights = false;
        std::uint64_t buf[16]{};  // the illumination's first 0x80 bytes (static light entries 0 and 1 start at +0x10 / +0x50)
        if (fastReads) {
            ObjSnap<0x98> is;
            is.load(il);
            x.list = is.ok ? is.at<U8*>(0) : nullptr;
            x.alone = is.ok && (is.at<std::uint8_t>(0x90) != 0 || is.at<std::uint8_t>(0xd) != 0);
            x.staticSh = is.ok && is.at<std::uint8_t>(0x90) != 0;
            noSun = is.ok && is.at<std::uint8_t>(0xc) != 0;
            statLights = is.ok && is.at<std::uint8_t>(0xd) != 0;
            if (statLights) for (std::size_t k = 0; k < 16; ++k) buf[k] = is.at<std::uint64_t>(8 * k);
        } else {
            x.list = ptr(il, 0);
            x.alone = get<std::uint8_t>(il, 0x90) != 0 || get<std::uint8_t>(il, 0xd) != 0;
            x.staticSh = get<std::uint8_t>(il, 0x90) != 0;
            noSun = get<std::uint8_t>(il, 0xc) != 0;
            statLights = get<std::uint8_t>(il, 0xd) != 0;
            if (statLights) rd(il, 0, buf, sizeof(buf));
        }
        if (tableMerge && x.list == nullptr) x.ownLights = true;  // no list: no table-lit pass, stays a batch of its own
        if (tableMerge && x.list != nullptr) {
            if (x.list != sunList) {
                sunList = static_cast<const U8*>(x.list);
                sunListValue = get<std::uint8_t>(sunList, 0) != 0;
                dynListValue = get<std::uint8_t>(sunList, 0x34) != 0;
            }
            x.sun = sunListValue && !noSun;
            // the registers keep the engine's own lights (the fallback of table lighting): instances share a batch only when those are the same ones - no dynamic
            // lights of the list, equal static light entries (up to two: position, range, colour), no more than two
            if (dynListValue) x.ownLights = true;
        }
        if (tableMerge && statLights) {
            const std::uint32_t ns = static_cast<std::uint32_t>(buf[1] >> 40 & 0xff);  // illum +0xd
            // lights the table holds (the far level reaches 96,000 units) light this instance's pixels by position: they no longer tie it to its batch
            bool allKnown = ns >= 1 && ns <= 2 && enh::LightTable::registryComplete();
            for (std::uint32_t e = 0; allKnown && e < ns; ++e) {
                float p[3];
                std::memcpy(p, &buf[2 + 8 * e], sizeof(p));
                allKnown = enh::LightTable::lightKnown(p);
            }
            if (allKnown) {
                x.lightSig = 0;
                ++st.knownLights;
            } else if (ns > 2) {
                x.ownLights = true;
            } else {
                std::uint64_t sig = 1469598103934665603ull ^ ns;
                for (std::uint32_t e = 0; e < ns; ++e) {
                    for (std::size_t k = 0; k < 6; ++k) sig = (sig ^ buf[2 + 8 * e + k]) * 1099511628211ull;
                }
                x.lightSig = sig;
            }
        }
    };
    std::vector<sc::TreeInstance>& inst = scr_.treeInst;
    std::vector<sc::TreeBatch>& batches = scr_.treeBatches;
    // the reasons the solid pass's batches end (diagnostics: which rule cuts them)
    const auto countSplits = [&](const std::vector<sc::TreeBatch>& bs) {
        if (zpass) return;
        for (std::size_t k = 1; k < bs.size(); ++k) {
            const sc::TreeInstance& a = inst[bs[k].first - 1];
            const sc::TreeInstance& b = inst[bs[k].first];
            std::size_t r = 5;  // own lights / other
            if (bs[k - 1].count >= limit) r = 0;
            else if (type <= 1 && a.strip != b.strip) r = 1;
            else if (!tableMerge && a.list != b.list) r = 2;
            else if (tableMerge && a.sun != b.sun) r = 3;
            else if (tableMerge ? (a.staticSh || b.staticSh) : (a.alone || b.alone)) r = 4;
            else if (a.list == nullptr || b.list == nullptr) r = 6;
            ++st.splits[r];
        }
    };
    inst.clear();
    batches.clear();
    if (type <= 1) {
        // branches / fronds: SetGlobalAlpha(1, 0) once (RenderBranches, RenderBranchesZPass, RenderFronds; RenderFrondsZPass does not);
        // per instance SetWorldMatrix + SetSpeedTreeInstanceData((tree +0x6c, rb +0xa0 / +0xa4))
        if (!(zpass && type == 1)) writeAlpha(0, one_);
        const std::size_t lodAt = type == 0 ? 0x48 : 0x50, stripAt = type == 0 ? 0x40 : 0x50, startAt = type == 0 ? 0xe8 : 0xf8;
        const float vy = get<float>(rb, type == 0 ? 0xa0 : 0xa4);
        {
        const f4m::PhaseScope f4s(f4m::kSnapshot);
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(n); ++i) {
            U8* tree = treeOf(i);
            std::int32_t lod;
            if (fastReads) {
                ObjSnap<0x70> ts;
                ts.load(tree);
                if (!ts.ok || ts.at<U8*>(0x40) == nullptr) return Skip::Unreadable;  // the engine reads the LOD values through it
                lod = ts.at<std::int32_t>(lodAt);
            } else {
                if (ptr(tree, 0x40) == nullptr) return Skip::Unreadable;
                lod = get<std::int32_t>(tree, lodAt, -1);
            }
            if (lod < 0) continue;
            sc::TreeInstance x;
            x.queueIndex = i;
            x.strip = get<std::uint16_t>(rb, stripAt + 2 * static_cast<std::size_t>(lod));
            x.start = get<std::uint16_t>(rb, startAt + 2 * static_cast<std::size_t>(lod));
            if (!zpass) {
                readLight(x, illumOf(i));
            }
            inst.push_back(x);
        }
        }
        {
            const f4m::PhaseScope f4p(f4m::kPlan);
            sc::planTreeBatches(inst, limit, !zpass, true, batches, tableMerge);
            countSplits(batches);
            f4m::batches(static_cast<std::uint32_t>(batches.size()), static_cast<std::uint32_t>(inst.size()));
        }
        for (const sc::TreeBatch& b : batches) {
            const std::uint64_t bkey = trOn ? treeBucketKey(type, zpass, tableMerge && inst[b.first].sun, b.strip, b.start, 0) : 0;
            {
            const f4m::PhaseScope f4r(f4m::kRows);
            for (std::uint32_t k = 0; k < b.count; ++k) {
                U8* tree = treeOf(inst[b.first + k].queueIndex);
                float world[16];
                if (!entityWorld(tree, world)) return Skip::Unreadable;
                writeWorld(k, world);
                const float v[2] = {get<float>(tree, 0x6c), vy};
                writeTreeInstance(k, v);
                if (trOn && tro_->checkOp) treeCompInstance(tree, illumOf(inst[b.first + k].queueIndex), bkey, tableMerge && inst[b.first + k].sun, k);
            }
            }
            st.instances += b.count;
            ++st.batches;
            // fronds: a batch whose strip length is 0 is not drawn (RenderFronds 0x6f1b0f / RenderFrondsZPass)
            if (type == 1 && b.strip == 0) {
                ++st.undrawnBatches;
                continue;
            }
            const sc::TreeRange r = sc::treeRange(type == 0 ? sc::TreeGeometry::Branches : sc::TreeGeometry::Fronds, hardware, b.count, b.strip, b.start, verts, 0);
            if (const Skip s = drawBatch(r, zpass ? nullptr : illumOf(inst[b.first + b.count - 1].queueIndex), bkey); s != Skip::None) return s;
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
            std::vector<float>& alpha = scr_.treeAlpha;
            alpha.clear();
            {
            const f4m::PhaseScope f4s(f4m::kSnapshot);
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(n); ++i) {
                U8* tree = treeOf(i);
                float a = 0.0f;
                std::int32_t l0, l1;
                float r0 = 0, r1 = 0;
                if (fastReads) {
                    ObjSnap<0x70> ts;
                    ts.load(tree);
                    if (!ts.ok || ts.at<U8*>(0x40) == nullptr) return Skip::Unreadable;
                    l0 = ts.at<std::int32_t>(0x58);
                    l1 = ts.at<std::int32_t>(0x5c);
                    r0 = ts.at<float>(0x60);
                    r1 = ts.at<float>(0x64);
                } else {
                    if (ptr(tree, 0x40) == nullptr) return Skip::Unreadable;
                    l0 = get<std::int32_t>(tree, 0x58, -1);
                    l1 = get<std::int32_t>(tree, 0x5c, -1);
                }
                if (shadow) {
                    // RenderLeafsZPass 0x6f0860 in a shadow map: the one leaf LOD min(+0x58 + 1, LODs - 1), global alpha 1
                    const std::int32_t l = l0 < 0 ? -1 : std::min(l0 + 1, lods - 1);
                    if (l != lodi) continue;
                    a = one_;
                } else if (l0 == lodi && l0 > -1) {
                    a = fixedRef / (fastReads ? r0 : get<float>(tree, 0x60));
                } else if (l1 == lodi && l1 > -1) {
                    a = fixedRef / (fastReads ? r1 : get<float>(tree, 0x64));
                } else {
                    continue;
                }
                sc::TreeInstance x;
                x.queueIndex = i;
                if (!zpass) {
                    readLight(x, illumOf(i));
                }
                inst.push_back(x);
                alpha.push_back(a);
            }
            }
            {
                const f4m::PhaseScope f4p(f4m::kPlan);
                sc::planTreeBatches(inst, limit, !zpass, false, batches, tableMerge);
                countSplits(batches);
                f4m::batches(static_cast<std::uint32_t>(batches.size()), static_cast<std::uint32_t>(inst.size()));
            }
            for (const sc::TreeBatch& b : batches) {
                const std::uint64_t bkey = trOn ? treeBucketKey(type, zpass, tableMerge && inst[b.first].sun, static_cast<std::uint32_t>(lodi), start, indices) : 0;
                {
                const f4m::PhaseScope f4r(f4m::kRows);
                for (std::uint32_t k = 0; k < b.count; ++k) {
                    U8* tree = treeOf(inst[b.first + k].queueIndex);
                    float world[16];
                    if (!entityWorld(tree, world)) return Skip::Unreadable;
                    writeWorld(k, world);
                    writeAlpha(k, alpha[b.first + k]);
                    writeTreeInstance(k, v);
                    if (trOn && tro_->checkOp) treeCompInstance(tree, illumOf(inst[b.first + k].queueIndex), bkey, tableMerge && inst[b.first + k].sun, k);
                }
                }
                st.instances += b.count;
                ++st.batches;
                const sc::TreeRange r = sc::treeRange(sc::TreeGeometry::Leaves, hardware, b.count, 0, start, verts, indices);
                if (const Skip s = drawBatch(r, zpass ? nullptr : illumOf(inst[b.first + b.count - 1].queueIndex), bkey); s != Skip::None) return s;
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
    const f4m::BbScope f4bb(&drawCount_, dev_.stats().frames, static_cast<std::uint32_t>(viewKind_), cascade_, db_, desc_, zpass, zpass && ((op.raw.desc >> 16) & 0x10u) != 0);
    U8* ps = ptr(op.raw.context, 0x18);
    if (ps == nullptr) return Skip::None;
    U8* res = ptr(ps, 0x30);
    if (res == nullptr) return Skip::Unreadable;
    if (leanCollect_) touchNow(res);  // O2b lean: Render / RenderZPass touch the billboard mesh first (inline RequestData)
    const std::uint32_t sub = op.raw.desc & 0xffu, cell = op.raw.desc >> 8;
    const std::int32_t subs = get<std::int32_t>(res, 0x60);
    if (sub == 0xff || static_cast<std::int32_t>(sub) >= subs) return Skip::Unreadable;
    U8* subMesh = ptr(res, 0x58) + 0x48 * static_cast<std::size_t>(sub);
    U8* material = ptr(subMesh, 0x38);
    U8* shader = ptr(material, 0x70);
    if (shader == nullptr) {
        // UploadTexCoords and the mesh paths draw nothing without the sub-mesh's shader (O2b lean: their side effects, BindLightmaps and
        // the sub-mesh's touches, are left to the engine's code)
        leanBlock_ |= kBlockEdge;
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
        if (f4m::on()) f4m::bbConstants(ps, op.raw.desc, mirror_.global[1], (5 + static_cast<std::size_t>(count)) * 16);
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
    // F7-2: the op's retained record is made / compared from this composition
    const bool vroOn = ownedVegOps_ && viewKind_ == ViewKind::Main && packetMode_ != 0 && g_ != nullptr && !g_->compare && vro_ != nullptr && !vro_->tripped;
    if (vroOn) vegCompBegin(op);
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
    const auto tN0 = std::chrono::steady_clock::now();
    // the nodes and the instance data in Render's order
    std::vector<sc::GrassNode>& plan = scr_.grassPlan;
    std::vector<U8*>& sources = scr_.grassSources;
    plan.assign(static_cast<std::size_t>(nodeCount), sc::GrassNode{});
    sources.assign(static_cast<std::size_t>(nodeCount), nullptr);
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
    st.vegNodesNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tN0).count());
    st.grassNodes += static_cast<std::uint32_t>(nodeCount);
    st.grassInstances += total;
    const std::uint32_t cursor = get<std::uint32_t>(vadmin, 0x40);
    if (cursor > sc::kGrassBufferInstances) return Skip::Unreadable;
    std::vector<sc::GrassDraw>& draws = scr_.grassDraws;
    draws.clear();
    const std::uint32_t cursorAfter = sc::planGrass(plan, cursor, zpass, draws);
    // O2b lean: Render leaves the shared buffer's cursor where the op's locks moved it (no buffer: no lock, no move); written at the commit
    if (leanCollect_ && vd != nullptr) leanVeg_ = {vadmin, cursorAfter};
    // the instance bytes, flattened in the nodes' order
    const bool wind = get<float>(typeObj, 0x2c) != 0.0f;
    const bool lift = get<std::int32_t>(typeObj, 0x20) != 0;
    std::vector<std::uint8_t>& bytes = scr_.grassBytes;
    const auto tC0 = std::chrono::steady_clock::now();
    // F7-1: the op's source entries flattened (nodes in order) and handed to the retained stream of this op: the converted instances stay in a persistent GPU buffer
    // while the source bytes are unchanged (an exact compare), so the draws carry no CPU bytes
    VegRet* ret = nullptr;
    // S-5 (native.own_foliage): the grid's range decides the thinning (the z op and the solid op of a context read the same grid: the same decision per instance)
    const sc::GrassThin thin = grassThinOf(get<float>(ptr(ps, 0x90), 0x54));
    if (ownedVeg_ && packetMode_ != 0 && viewKind_ == ViewKind::Main && g_ != nullptr && !g_->compare && total != 0) {
        vegRaw_.resize(static_cast<std::size_t>(total) * sc::kGrassInstanceSize);
        std::size_t at = 0;
        for (std::int32_t i = 0; i < nodeCount; ++i) {
            const sc::GrassNode& gn = plan[static_cast<std::size_t>(i)];
            if (gn.count == 0) continue;
            const std::size_t n = static_cast<std::size_t>(gn.count) * sc::kGrassInstanceSize;
            if (!safeRead(sources[static_cast<std::size_t>(i)], vegRaw_.data() + at, n)) return Skip::Unreadable;
            at += n;
        }
        ret = vegRetain(op, total, wind, lift, thin);
    }
    if (ret == nullptr) {
        bytes.assign(static_cast<std::size_t>(total) * sc::kGrassInstanceSize, 0);
        std::size_t at = 0;
        std::vector<std::uint8_t>& src = scr_.grassSrc;
        for (std::int32_t i = 0; i < nodeCount; ++i) {
            const sc::GrassNode& gn = plan[static_cast<std::size_t>(i)];
            if (gn.count == 0) continue;
            src.resize(static_cast<std::size_t>(gn.count) * sc::kGrassInstanceSize);
            if (!safeRead(sources[static_cast<std::size_t>(i)], src.data(), src.size())) return Skip::Unreadable;
            for (std::uint32_t k = 0; k < gn.count; ++k, at += sc::kGrassInstanceSize) {
                sc::grassInstance(src.data() + sc::kGrassInstanceSize * static_cast<std::size_t>(k), wind, lift, bytes.data() + at);
            }
        }
        if (thin.on) {
            std::uint32_t removed = 0;
            const std::uint32_t changed = sc::grassThin(bytes.data(), total, thin, &removed);
            foliageCount(thin, total, changed, removed);
        }
    }
    st.vegConvNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tC0).count());
    for (const sc::GrassDraw& d : draws) {
        if (d.bufferOffset + d.count == sc::kGrassBufferInstances) ++st.grassWraps;
    }
    // O2b lean: RenderMeshInstanced per drawn batch: the type's mesh touched (state 0: nothing); cached with its visual and the shared
    // buffer: the first sub-mesh's material touched
    if (leanCollect_ && !draws.empty() && mesh != nullptr) {
        touchNow(mesh);
        if (get<std::int32_t>(mesh, 0x38) == 3 && ptr(mesh, 0x88) != nullptr && vd != nullptr) touchNow(ptr(ptr(mesh, 0x58), 0x38));
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
    if (ret != nullptr && !stream1) {  // (no stream 1 in the record: the old CPU-bytes path; its bytes from the flattened source)
        bytes.assign(vegRaw_.size(), 0);
        for (std::size_t k = 0; k * sc::kGrassInstanceSize < vegRaw_.size(); ++k) sc::grassInstance(vegRaw_.data() + k * sc::kGrassInstanceSize, wind, lift, bytes.data() + k * sc::kGrassInstanceSize);
        if (thin.on) {
            std::uint32_t removed = 0;
            const std::uint32_t changed = sc::grassThin(bytes.data(), vegRaw_.size() / sc::kGrassInstanceSize, thin, &removed);
            foliageCount(thin, vegRaw_.size() / sc::kGrassInstanceSize, changed, removed);
        }
        ret = nullptr;
    }
    if (vroOn) vegCompNote(op, draws, nodes, total, wind, lift, ax, ay);
    std::size_t drawIndex = 0;
    for (const sc::GrassDraw& d : draws) {
        mirror_.global[0][0] = ax;  // c31.xy
        mirror_.global[0][1] = ay;
        writeAlpha(0, one_);        // c32.w = 1
        Geom gg = g;
        if (gg.streams[0].buffer != nullptr) gg.streams[0].freq = d.count | 0x40000000u;
        // AttachVertexData(1, the shared buffer): the record's stream 1 = its buffer with its stride, at the batch's byte offset,
        // INSTANCEDATA | 1
        if (ret != nullptr) {
            // the retained buffer holds the op's instances flattened: this batch starts at its first instance
            gg.streams[1] = sc::DrawDesc::Stream{ret->buf.get(), d.firstInstance * sc::kGrassInstanceSize, sc::kGrassInstanceSize, 0x80000001u};
            ++vegStat_.drawsRetained;
        } else if (stream1) {
            gg.streams[1] = sc::DrawDesc::Stream{data->buffer(), d.bufferOffset * sc::kGrassInstanceSize, data->stride(), 0x80000001u};
        }
        U8* illum = d.illumNode >= 0 ? nodes + 0x118 * static_cast<std::size_t>(d.illumNode) + 0x10 : nullptr;
        const std::size_t before = drawCount_;
        const engine::illumcensus::KindScope census(2);
        const Skip s = zpass ? specialPassDraw(shader, gg, false) : materialDraws(shader, illum, 0, false, 0, 0, gg, nullptr);
        if (s != Skip::None) return s;
        if (vroOn) vegCompDraw(before, drawIndex);
        ++drawIndex;
        for (std::size_t k = before; ret == nullptr && k < drawCount_; ++k) {
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

// ---- F7-1 retained vegetation streams (frame-ownership.md 7.40) ----------------------------------------------------------------------

void StaticOps::setOwnedVegetation(bool on) {
    if (on == ownedVeg_) return;
    ownedVeg_ = on;
    if (!on) vegRet_.clear();
    spdlog::info("native: owned vegetation {}", on ? "on (the vegetation ops' instance streams retained in persistent GPU buffers, compared exactly with the source entries)" : "off");
}

// the op's flattened source entries are in vegRaw_ (total instances): the retained stream of (context, type, list), rebuilt only when the source bytes (or the type's
// wind / lift flags) changed. nullptr = not retainable (the caller converts and uploads as before).
StaticOps::VegRet* StaticOps::vegRetain(const Op& op, std::uint32_t total, bool wind, bool lift, const sc::GrassThin& thin) {
    ++vegStat_.ops;
    const std::size_t n = static_cast<std::size_t>(total) * sc::kGrassInstanceSize;
    if (n == 0 || n > (8u << 20) || vegRaw_.size() != n) {
        ++vegStat_.fallback;
        return nullptr;
    }
    const std::uint64_t frame = dev_.stats().frames;
    if (frame % 600 == 0) {  // forget the streams of contexts that left (a world change)
        for (auto it = vegRet_.begin(); it != vegRet_.end();) it = frame - it->second.lastFrame > 1200 ? vegRet_.erase(it) : std::next(it);
    }
    VegRet& r = vegRet_[{op.raw.context, op.raw.desc, static_cast<std::uint32_t>(op.list)}];
    r.lastFrame = frame;
    const auto convert = [&](std::vector<std::uint8_t>& out) {
        out.resize(n);
        for (std::size_t k = 0; k < static_cast<std::size_t>(total); ++k) {
            sc::grassInstance(vegRaw_.data() + k * sc::kGrassInstanceSize, wind, lift, out.data() + k * sc::kGrassInstanceSize);
        }
    };
    bool same = r.buf != nullptr && r.raw.size() == n && r.wind == wind && r.lift == lift && r.thin == thin && std::memcmp(r.raw.data(), vegRaw_.data(), n) == 0;
    if (same && frame % 10 == 0) {
        // check frame: the converted bytes of this source must be the retained ones (the stream's content is a pure function of the source + the flags)
        ++vegStat_.checks;
        std::vector<std::uint8_t> fresh;
        convert(fresh);
        if (fresh != r.conv) {
            ++vegStat_.differing;
            same = false;
            spdlog::warn("native: owned vegetation: retained stream differs from a fresh conversion ({} instances): rebuilt", total);
        }
    }
    if (same) {
        ++vegStat_.reused;
        ++vegStat_.retained;
        return &r;
    }
    const std::uint32_t need = std::max<std::uint32_t>(4096u, std::bit_ceil(static_cast<std::uint32_t>(n)));
    if (!r.buf || r.cap < need) {
        r.buf = dev_.createBuffer(need, d3d9::USAGE_DYNAMIC | d3d9::USAGE_WRITEONLY, d3d9::POOL_DEFAULT, false, false);
        r.cap = r.buf ? need : 0;
        r.raw.clear();
        ++vegStat_.grown;
    }
    if (!r.buf) {
        ++vegStat_.fallback;
        return nullptr;
    }
    convert(r.conv);
    void* p = r.buf->lock(0, static_cast<std::uint32_t>(n), d3d9::LOCK_DISCARD);
    if (p == nullptr) {
        r.raw.clear();
        ++vegStat_.fallback;
        return nullptr;
    }
    if (thin.on) {
        // S-5: the buffer holds the thinned bytes; r.conv stays the plain conversion (the check frames compare it with a fresh one)
        static thread_local std::vector<std::uint8_t> thinned;
        thinned.assign(r.conv.begin(), r.conv.end());
        std::uint32_t removed = 0;
        const std::uint32_t changed = sc::grassThin(thinned.data(), total, thin, &removed);
        foliageCount(thin, total, changed, removed);
        std::memcpy(p, thinned.data(), n);
    } else {
        std::memcpy(p, r.conv.data(), n);
    }
    r.buf->unlock();
    r.raw.assign(vegRaw_.begin(), vegRaw_.end());
    r.wind = wind;
    r.lift = lift;
    r.thin = thin;
    ++vegStat_.rebuilt;
    vegStat_.rebuiltBytes += n;
    ++vegStat_.retained;
    return &r;
}

// ---- S-5 grass distance thinning (native.own_foliage, frame-ownership.md 14.14) ------------------------------------------------------------

void StaticOps::setOwnFoliage(bool on) {
    if (on == ownFoliage_) return;
    ownFoliage_ = on;
    vegRet_.clear();  // (the retained streams hold the bytes of the old state)
    spdlog::info("native: own foliage {}", on ? "on (grass distance thinning tied to lod.view on every converted grass stream of the main view)" : "off");
}

sc::GrassThin StaticOps::grassThinOf(float gridEnd) const {
    // (not while the engine's bytes are compared: scene_ops_compare; the owned pass has no g_, so the group's own state decides for every path alike)
    if (!ownFoliage_ || viewKind_ != ViewKind::Main || groups_[static_cast<std::size_t>(Group::Vegetation)].compare) return {};
    static const bool all = [] {
        char v[4] = {};
        return GetEnvironmentVariableA("RVK_S5_GRASS_OFF", v, sizeof(v)) != 0 && v[0] == '1';
    }();
    const auto& rs = settings::renderer();
    const float lodView = rs.lodView != nullptr ? rs.lodView->asFloat() : 1.0f;
    return sc::grassThinParams(view_.camPos, gridEnd, lodView, all);
}

void StaticOps::foliageCount(const sc::GrassThin&, std::size_t instances, std::uint32_t changed, std::uint32_t removed) {
    ++foliageW_.ops;
    foliageW_.instances += instances;
    foliageW_.changed += changed;
    foliageW_.removed += removed;
}

void StaticOps::foliageReport() {
    FoliageStats& w = foliageW_;
    foliageTotal_.ops += w.ops;
    foliageTotal_.instances += w.instances;
    foliageTotal_.changed += w.changed;
    foliageTotal_.removed += w.removed;
    {
        // S-5 measurement: what vertex pulling of the trees' / grass' own flushes could merge (always logged while those flushes run)
        auto& f = dev_.treeFlushStats;
        static std::uint64_t last[4][2] = {};
        const std::uint64_t* now[4] = {f.kindFlushes, f.kindItems, f.kindCalls, f.kindPullKeys};
        std::uint64_t d[4][2];
        for (int a = 0; a < 4; ++a)
            for (int k = 0; k < 2; ++k) {
                d[a][k] = now[a][k] - last[a][k];
                last[a][k] = now[a][k];
            }
        if (d[0][0] + d[0][1] != 0) {
            const auto per = [](std::uint64_t x, std::uint64_t n) { return static_cast<double>(x) / static_cast<double>(std::max<std::uint64_t>(1, n)); };
            spdlog::info("native: foliage flush calls (5 s): trees {} flushes, {:.1f} items / {:.1f} indirect calls / {:.1f} pulled calls at best per flush; grass {} flushes, {:.1f} items / {:.1f} calls / {:.1f} pulled at best",
                         d[0][0], per(d[1][0], d[0][0]), per(d[2][0], d[0][0]), per(d[3][0], d[0][0]), d[0][1], per(d[1][1], d[0][1]), per(d[2][1], d[0][1]), per(d[3][1], d[0][1]));
        }
    }
    if (ownFoliage_ || w.ops != 0) {
        const auto& rs = settings::renderer();
        spdlog::info("native: own foliage (5 s): grass thinning (lod.view {:g}): {} streams thinned, {} instances, {} shrunk or removed ({:.1f} %), {} removed ({:.1f} %)",
                     rs.lodView != nullptr ? rs.lodView->asFloat() : 1.0f, w.ops, w.instances, w.changed, w.instances ? 100.0 * static_cast<double>(w.changed) / static_cast<double>(w.instances) : 0.0,
                     w.removed, w.instances ? 100.0 * static_cast<double>(w.removed) / static_cast<double>(w.instances) : 0.0);
    }
    w = {};
}

}  // namespace rvk::native
