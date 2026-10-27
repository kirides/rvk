// N5.3 scene ops: the main view's skinned actor ops (eCRenderContextAnimation2; docs/research/engine-frame/README.md "10. N5.3
// result", D-contexts.md 1), re-written from the disassembly of eCAnimation_PS::Render 0x4d6fe0, eCWrapper_emfx3ActorInstance::
// UpdateRenderMatrices 0x4dfee0, eCAnimation_HardRenderActor2::Render 0x4dd650 / SetupMorphTargets 0x4dd220 / RenderPrimitive
// 0x4dd0a0 (dumps B:\DevDrive\risen-ghidra\skin_dump.txt, nativemesh_dump2.txt): the op's actor (the property set's own
// wrapper or a body part's), its LOD, the hardware primitive of that LOD, the primitive's bones from the frame's palette into the
// global pool (c33 + 3 i), the three strongest morph targets (c31.xyz + the stream offsets of streams 1..3), the global alpha,
// then the z pre-pass' special pass or the material's passes (GetPasses with the entity's overlay flags) over the actor's
// visual. The engine skins on the GPU only (eCAnimation_HardRenderActor2 is the one render hook; no CPU skinning). Part of
// StaticOps (static_ops.h); the pass rules are static_ops.cpp's.

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

void column(const float m[16], std::uint32_t k, float out[4]) {
    out[0] = m[k];
    out[1] = m[4 + k];
    out[2] = m[8 + k];
    out[3] = m[12 + k];
}

constexpr std::uint32_t kFrameAdmin = 0xa1c40;   // the getter eCAnimation_PS::Render calls first (its +0x4c = the frame id)
constexpr std::uint32_t kAffineMul = 0xa2710;    // FUN_1800a2710: a *= b (3x4 affine, row vectors), UpdateRenderMatrices' product
constexpr std::uint32_t kSetBoneMatrix = 0x6c9d80, kSetMorphWeights = 0x6c9da0;  // eCShaderDefault vtable +0x1f0 / +0x1f8
constexpr std::uint32_t kMaxBones = 66;          // eCShaderDefault::GetMaxBoneMatrixCount (c33 .. c230)
constexpr std::uint32_t kMaxMorphTargets = 4096;  // a sanity bound on HRA +0xa8

// instruction sites whose meaning the composition relies on (bytes checked before use; e8 = call rel32 to `target`)
struct Site {
    std::uint32_t rva;
    std::array<std::uint8_t, 8> b;
    std::uint32_t n;
    std::uint32_t target;  // e8 sites: the callee's RVA (0: compare the bytes only)
};
constexpr Site kSites[] = {
    {0x4d7005, {0xe8}, 1, kFrameAdmin},                                // eCAnimation_PS::Render: the frame admin
    {0x4d7014, {0x44, 0x8b, 0x50, 0x4c}, 4, 0},                        // mov r10d, [rax + 0x4c]: the frame id
    {0x4d7117, {0xe8}, 1, 0x4dfee0},                                   // UpdateRenderMatrices(wrapper, frame id)
    {0x4dd677, {0x44, 0x8b, 0x90, 0x50, 0x02, 0, 0}, 7, 0},  // HRA::Render: mov r10d, [ActorInstance + 0x250]
    {0x4dd709, {0x41, 0x0f, 0xb7, 0x44, 0x8e, 0x38}, 6, 0},            // movzx eax, word [hra + lod * 0x14 + 0x38]
    {0x4dd798, {0x41, 0xff, 0x91, 0xf0, 0x01, 0, 0}, 7, 0},            // call [r9 + 0x1f0] SetBoneMatrix
    {0x4dd7e2, {0xe8}, 1, 0x4dd220},                                   // SetupMorphTargets
    {0x4dd7f2, {0xff, 0x90, 0xf8, 0x01, 0, 0}, 6, 0},                  // SetMorphTargetWeights
    {0x4dd80a, {0xff, 0x90, 0x10, 0x02, 0, 0}, 6, 0},                  // SetGlobalAlpha
    {0x4dd836, {0xe8}, 1, 0x4dd0a0},                                   // RenderPrimitive
    {0x4dd30d, {0x44, 0x0f, 0xaf, 0x44, 0xae, 0x48}, 6, 0},            // SetupMorphTargets: imul r8d, [hra + lod * 0x14 + 0x48]
    {0x4dd35f, {0xff, 0x50, 0x30}, 3, 0},                              // ... the primitive's SetStreamOffset (slot 6)
    {0x4e006f, {0xe8}, 1, kAffineMul},                                 // UpdateRenderMatrices: palette[i] *= W[i]
};

}  // namespace

bool StaticOps::verifyActors() {
    if (engine_ == nullptr) return false;
    for (const Site& s : kSites) {
        std::array<std::uint8_t, 8> got{};
        const std::uint32_t n = s.target != 0 ? 5 : s.n;
        if (!safeRead(engine_ + s.rva, got.data(), n) || !std::equal(got.begin(), got.begin() + s.n, s.b.begin())) {
            spdlog::warn("native: scene ops: skinned actors: the code at RVA {:#x} differs: the actors stay the engine's", s.rva);
            return false;
        }
        if (s.target != 0) {
            std::int32_t rel = 0;
            std::memcpy(&rel, got.data() + 1, 4);
            if (static_cast<std::int64_t>(s.rva) + 5 + rel != s.target) {
                spdlog::warn("native: scene ops: skinned actors: the call at RVA {:#x} does not go to {:#x}: the actors stay the engine's", s.rva,
                             s.target);
                return false;
            }
        }
    }
    const void* bone = nullptr;
    const void* morph = nullptr;
    if (!readAt(engine_ + 0x97bdb0, 0x1f0, bone) || bone != engine_ + kSetBoneMatrix || !readAt(engine_ + 0x97bdb0, 0x1f8, morph) ||
        morph != engine_ + kSetMorphWeights) {
        spdlog::warn("native: scene ops: skinned actors: eCShaderDefault's SetBoneMatrix / SetMorphTargetWeights slots differ: the actors stay "
                     "the engine's");
        return false;
    }
    frameAdmin_ = reinterpret_cast<void* (*)()>(const_cast<std::uint8_t*>(engine_ + kFrameAdmin));
    affineMul_ = reinterpret_cast<void (*)(float*, const float*)>(const_cast<std::uint8_t*>(engine_ + kAffineMul));
    spdlog::info("native: scene ops: skinned actor checks passed (eCAnimation_PS::Render, UpdateRenderMatrices, HardRenderActor2::Render / "
                 "SetupMorphTargets sites, SetBoneMatrix / SetMorphTargetWeights slots)");
    return true;
}

void StaticOps::writeBone(std::uint32_t slot, const float* m) {
    const std::uint32_t r = sc::boneRegister(slot) - 31;
    if (r + 3 > 201) return;
    for (std::uint32_t k = 0; k < 3; ++k) column(m, k, mirror_.global[r + k]);
}

// UpdateRenderMatrices 0x4dfee0: when the wrapper's stamp (+0x58) is not this frame's id or its palette is empty (+0x50 == 0), the
// palette is rebuilt: n = the resource's actor (+0x18 -> +0x70) bone count +0xd8, palette[i] = X[i] (ActorInstance +0x28), then
// FUN_1800a2710(palette[i], W[i]) (ActorInstance +0x18); otherwise the stored palette (+0x48) is what the bones read
bool StaticOps::paletteMatrix(const std::uint8_t* wrapper, std::uint32_t bone, float out[16]) {
    const bool stored = get<std::uint32_t>(wrapper, 0x58) == frameId_ && get<std::uint32_t>(wrapper, 0x50) != 0;
    if (stored) {
        if (bone >= get<std::uint32_t>(wrapper, 0x50)) return false;
        return rd(ptr(wrapper, 0x48), 0x40 * static_cast<std::size_t>(bone), out, 64);
    }
    U8* actor = ptr(ptr(wrapper, 0x18), 0x70);
    if (bone >= get<std::uint32_t>(actor, 0xd8)) return false;
    U8* ai = ptr(wrapper, 8);
    float w[16];
    if (!rd(ptr(ai, 0x28), 0x40 * static_cast<std::size_t>(bone), out, 64) || !rd(ptr(ai, 0x18), 0x40 * static_cast<std::size_t>(bone), w, 64)) {
        return false;
    }
    affineMul_(out, w);
    return true;
}

StaticOps::Skip StaticOps::composeActor(const Op& op) {
    ActorStats& st = actorWindow_;
    ++st.ops;
    // eCRenderContextAnimation2 has no RenderRefracted: Execute calls eCRenderContext's empty slot for a refracted actor primitive
    if (op.list == List::Refracted) {
        if (!op.emptySlot) return Skip::Kind;
        ++st.emptySlot;
        return Skip::None;
    }
    if (op.list != List::ZPass && op.list != List::Solid && op.list != List::Alpha) return Skip::Kind;
    U8* ps = ptr(op.raw.context, 0x18);  // eCAnimation_PS
    if (ps == nullptr) return Skip::Unreadable;
    // the slots: Render / RenderAlpha (actor, primitive, false, the op's fade), RenderZPass (actor, primitive, true, 1.0)
    const auto actorIdx = static_cast<std::uint16_t>(op.raw.desc & 0xffffu);
    const auto primIdx = static_cast<std::uint16_t>(op.raw.desc >> 16);
    const bool zpass = op.list == List::ZPass;
    const float fade = zpass ? 1.0f : op.raw.fade;
    // eCAnimation_PS::Render: the overlay flags of the entity (PS +0x10) +0x28: 0x200000 -> 2, 0x400000 -> 1, 0x800000 -> 4
    U8* entity = ptr(ps, 0x10);
    const std::uint32_t eflags = get<std::uint32_t>(entity, 0x28);
    std::uint8_t flags = 0;
    if (eflags & 0x200000u) flags |= 2;
    if (eflags & 0x400000u) flags |= 1;
    if (eflags & 0x800000u) flags |= 4;
    // the actor: 0 = the property set's own wrapper (+0x30, switch +0x20), else body part k - 1 (PS +0x1b0)
    U8* wrapper = nullptr;
    std::int32_t variant = 0;
    if (actorIdx == 0) {
        wrapper = ps + 0x30;
        variant = get<std::int32_t>(ps, 0x20);
    } else {
        if (actorIdx > get<std::uint16_t>(ps, 0x1b8)) return Skip::Unreadable;
        U8* part = ptr(ptr(ps, 0x1b0), 8 * static_cast<std::size_t>(actorIdx - 1));
        if (part == nullptr) return Skip::Unreadable;  // the engine reads a wrapper at 0x30 (it never happens)
        wrapper = part + 0x30;
        variant = get<std::int32_t>(part, 0x20);
        ++st.bodyParts;
    }
    U8* res = ptr(wrapper, 0x18);  // eCMotionActorResource2
    U8* hra = ptr(res, 0x98);      // eCAnimation_HardRenderActor2
    if (res == nullptr || hra == nullptr) return Skip::None;
    // UpdateRenderMatrices returns false without the ActorInstance (+8) or the resource's actor (+0x70): no draw
    U8* ai = ptr(wrapper, 8);
    if (ai == nullptr || ptr(res, 0x70) == nullptr) return Skip::None;
    // the illumination of the entity (null for a template property set: PS +0x18 & 0x10; the engine would dereference null + 0xf8)
    if ((get<std::uint8_t>(ps, 0x18) & 0x10) != 0) return Skip::Unreadable;
    U8* illumPs = ptr(entity, 0xf8);
    // HardRenderActor2::Render: LOD, the resource cached, the primitive of the LOD
    const std::uint32_t lod = sc::actorLod(get<std::uint32_t>(ai, 0x250));
    ++st.lod[lod];
    if (get<std::int32_t>(ptr(hra, 0), 0x38) != 3) return Skip::None;
    U8* illum = !zpass && illumPs != nullptr ? illumPs + 0x78 : nullptr;
    const std::uint32_t primIndex = sc::actorPrimitive(get<std::uint16_t>(hra, 0x38 + 0x14 * static_cast<std::size_t>(lod)), primIdx);
    U8* prims = ptr(hra, 0x90);
    if (prims == nullptr) return Skip::Unreadable;
    U8* prim = prims + 0x20 * static_cast<std::size_t>(primIndex);  // eSHardPrimitive
    U8* material = ptr(prim, 0x10);
    U8* shader = ptr(material, 0x70);
    if (material == nullptr) return Skip::Unreadable;
    if (shader == nullptr) return Skip::None;
    if (!shaderClassOk(shader)) return Skip::ShaderClass;
    U8* admin = ptr(shader, 0x220);
    if (!view_.valid) {
        if (admin == nullptr || !composeView(admin)) return Skip::Unreadable;
        if (view_.dbOrder < 0) spdlog::warn("native: static ops: the depth-bias view-projection matches neither product order");
    }
    if (admin != view_.admin) return Skip::Unreadable;
    initMirror();
    if (flags != 0) {
        ++st.overlayOps;
        for (std::uint32_t k = 0; k < 3; ++k) {
            if (flags >> k & 1u) ++st.overlayKinds[k];
        }
    }
    // the bones: SetBoneMatrix(i, palette[remap[start + i]]) -> c33 + 3 i
    frameId_ = get<std::uint32_t>(frameAdmin_(), 0x4c);
    const bool stored = get<std::uint32_t>(wrapper, 0x58) == frameId_ && get<std::uint32_t>(wrapper, 0x50) != 0;
    ++(stored ? st.palettesStored : st.palettesComposed);
    const std::uint16_t boneStart = get<std::uint16_t>(prim, 0x18);
    const std::uint32_t bones = sc::primitiveBones(boneStart, get<std::uint16_t>(prim, 0x1a));
    if (bones > kMaxBones) return Skip::Bones;
    U8* remap = ptr(hra, 0x28);
    for (std::uint32_t i = 0; i < bones; ++i) {
        float m[16];
        const std::uint16_t b = get<std::uint16_t>(remap, 2 * (static_cast<std::size_t>(boneStart) + i), 0xffff);
        if (!paletteMatrix(wrapper, b, m)) return Skip::Bones;
        writeBone(i, m);
    }
    st.bones += bones;
    // the actor's visual (HRA +0xa0): its record (our HalPrimitive) for the declaration, streams, indices, the draw description
    U8* visual = ptr(hra, 0xa0);
    const auto* record = hal::vulkan::HalPrimitive::fromApi(ptr(visual, 0x10));
    Geom g;
    if (record == nullptr || !geometryOfVisual(visual, g)) return Skip::Primitive;
    const auto& dd = record->drawDesc();
    if (!dd.indexed) return Skip::Primitive;
    // the morph targets (HRA +0xa8 != 0): the three strongest weights of the instance's setup (+0x1d0, 0xc-byte entries) ->
    // c31.xyz, and the stream offsets of streams 1..3 (the morph VB) = target x the LOD's vertex count x 0x10
    float weights[3] = {0.0f, 0.0f, 0.0f};
    const std::uint32_t targets = get<std::uint32_t>(hra, 0xa8);
    if (targets != 0) {
        if (targets > kMaxMorphTargets) return Skip::Unreadable;
        ++st.morphOps;
        U8* setup = ptr(ai, 0x1d0);
        if (setup == nullptr) return Skip::Unreadable;
        std::vector<float> w(targets);
        for (std::uint32_t k = 0; k < targets; ++k) {
            if (!rd(setup, 0xc * static_cast<std::size_t>(k) + 4, &w[k], 4)) return Skip::Unreadable;
        }
        const sc::MorphPick pick = sc::pickMorphTargets(w);
        const std::uint32_t lodVertices = get<std::uint32_t>(hra, 0x48 + 0x14 * static_cast<std::size_t>(lod));
        for (std::uint32_t i = 0; i < 3; ++i) {
            weights[i] = pick.weight[i];
            if (g.streams[i + 1].buffer != nullptr) g.streams[i + 1].offset = sc::morphStreamOffset(pick.target[i], lodVertices);
        }
        if (weights[0] != 0.0f || weights[1] != 0.0f || weights[2] != 0.0f) ++st.morphActive;
    }
    // SetMorphTargetWeights (always: zeros without targets) -> c31.xyz; SetGlobalAlpha(fade, 0) -> c32.w
    std::memcpy(mirror_.global[0], weights, 12);
    writeAlpha(0, fade);
    // RenderPrimitive: start index +0, primitives +4, min vertex +8, vertices +0xc into the record (the rest as created)
    g.startIndex = get<std::uint32_t>(prim, 0);
    g.primCount = get<std::uint32_t>(prim, 4);
    g.minIndex = get<std::uint32_t>(prim, 8);
    g.numVertices = get<std::uint32_t>(prim, 0xc);
    g.primType = static_cast<std::uint32_t>(dd.type);
    g.baseVertex = dd.baseVertexIndex;
    g.material = material;
    g.subMesh = nullptr;  // no eCSubMesh::Render: the slot's tag (Skinned, context, op, entity) only
    g.meshResource = nullptr;
    if (get<std::uint8_t>(admin, 0x312) == 1) return specialPassDraw(shader, g);  // the z pass: ExecuteSpecialPass(false) + one draw
    return materialDraws(shader, illum, 0, false, variant, flags, g, nullptr);
}

}  // namespace rvk::native
