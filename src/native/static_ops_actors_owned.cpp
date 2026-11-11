// F6-1 owned skinned actors (docs/research/frame-ownership.md 7.41, setting native.owned_actors): the actors' z / solid ops of the main view and z ops of the sun cascades are
// recorded like the static meshes' (op records keyed (context, desc, list, view), persistent DrawRecords, the owned pass), with these differences, all in this file or at the
// branches it serves:
//   * the stamp (recStampActor) reads what composeActorImpl reads: the actor's wrapper, resource, hardware render actor (HRA), the LOD's primitive and its material / shader,
//     the primitive's bone remap, and the morph pick (the three strongest targets: their stream offsets are part of the retained draw);
//   * VS window 2 (c31..c231) of an actor draw is a persistent image in the bone table (gpuscene/bone_table.*): c31.xyz morph weights, c32.w fade, the primitive's bone rows
//     from the engine's palette. Composed draws are served from it (actorServeComposed, in setOwnedFor), owned draws get it written once per frame and key (ownedActorWindow);
//   * the engine slot's side effects an owned actor op replays: UpdateRenderMatrices (the palette refresh of the frame), the touches of the HRA's resource and the material.
// Ops with an overlay flag (the highlighted focus NPC), a fade < 1, a template property set or an uncached resource stay composed (exclusions of recExclusionActor).

#include <windows.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "engine/memread.h"
#include "engine/scene_table.h"
#include "enh/light_table.h"
#include "gpuscene/bone_table.h"
#include "gpuscene/material_table.h"
#include "gpuscene/object_table.h"
#include "hal/vulkan/hal_vulkan.h"
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
inline bool rd(const void* p, std::size_t off, void* out, std::size_t n) { return p != nullptr && engine::safeRead(static_cast<U8*>(p) + off, out, n); }

inline std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}
inline std::uint64_t mixPtr(std::uint64_t h, const void* p) { return mix(h, reinterpret_cast<std::uintptr_t>(p)); }
inline std::uint64_t nsOf(Clock::time_point a, Clock::time_point b) { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()); }

constexpr std::uint32_t kMaxMorphTargets = 4096;

// the 3x4 transpose SetBoneMatrix writes: row k of the register triple = (m[k], m[4 + k], m[8 + k], m[12 + k])
inline void column(const float m[16], std::uint32_t k, float out[4]) {
    out[0] = m[k];
    out[1] = m[4 + k];
    out[2] = m[8 + k];
    out[3] = m[12 + k];
}

}  // namespace

bool StaticOps::f62On() {
    static const bool off = [] {
        char b[8];
        return GetEnvironmentVariableA("RVK_F62_OFF", b, sizeof(b)) != 0;
    }();
    return !off;
}

bool StaticOps::switchVariantsOn() const { return f62On() && ownedActors_ && !ownedActorsTripped_ && gpuscene::materialtable().active(); }

bool StaticOps::actorOrderedNow(const Op* op) const {
    return op != nullptr && f62On() && ownedOrderedOn() && ownedActorsOn() && viewKind_ == ViewKind::Main && !inNonOp_ && op->kind == OpKind::Animation &&
           (op->list == List::Solid || op->list == List::Alpha) && g_ == &groups_[static_cast<std::size_t>(Group::Actors)];
}

// the fade a bone key / stamp names: 1 (the z pass, the cascades, an opaque op) or "fading" (any fade < 1: the value itself is the bone image's c32.w, served per frame)
static float fadeClass(float fade) { return fade >= 1.0f ? 1.0f : 0.5f; }

void StaticOps::ActorOwnedStats::add(const ActorOwnedStats& o) {
    selected += o.selected; drawn += o.drawn; refusedWindow += o.refusedWindow; recordsBuilt += o.recordsBuilt; imageBuilds += o.imageBuilds; imageShared += o.imageShared;
    verified += o.verified; verifyDiffers += o.verifyDiffers; offeredDraws += o.offeredDraws; composedDraws += o.composedDraws;
    for (std::size_t i = 0; i < orec::kExclCount; ++i) excl[i] += o.excl[i];
    notRetained += o.notRetained; notOffered += o.notOffered; notComposable += o.notComposable; template_ += o.template_; notCached += o.notCached; rebuilds += o.rebuilds;
    for (std::size_t i = 0; i < 3; ++i) { byView[i] += o.byView[i]; seenByView[i] += o.seenByView[i]; }
    for (std::size_t v = 0; v < 3; ++v) {
        for (std::size_t l = 0; l < 4; ++l) {
            cenOwned[v][l] += o.cenOwned[v][l];
            for (std::size_t k = 0; k < std::size(cenWhy[v][l]); ++k) cenWhy[v][l][k] += o.cenWhy[v][l][k];
        }
    }
    ordSeen += o.ordSeen;
    pickSwaps += o.pickSwaps;
    pickMisses += o.pickMisses;
    pickKept += o.pickKept;
    ordOwned += o.ordOwned;
    morphOps += o.morphOps; morphPickChanged += o.morphPickChanged; imageNs += o.imageNs; stampNs += o.stampNs;
    drawsBuilt += o.drawsBuilt; drawNoScene += o.drawNoScene; drawCdInvalid += o.drawCdInvalid; morphActive += o.morphActive; stampRebuilds += o.stampRebuilds;
}

void StaticOps::setOwnedActors(bool on) {
    if (!on) ownedActorsTripped_ = false;
    if (on && ownedActorsTripped_) on = false;
    if (on == ownedActors_) return;
    ownedActors_ = on;
    spdlog::info("native: owned actors {}", on ? "on" : "off");
    if (!on && recs_) {
        // (the actors' records go: they hold bone table keys and retained draws that nothing refreshes any more)
        for (auto it = recs_->map.begin(); it != recs_->map.end();) {
            if (it->second->actor) it = recs_->map.erase(it);
            else ++it;
        }
    }
}

bool StaticOps::actorPick(const std::uint8_t* ai, const std::uint8_t* setup, std::uint32_t targets, float (&weights)[3], std::uint32_t (&pick)[3]) {
    const std::uint64_t frame = dev_.stats().frames;
    if (pickMemoFrame_ != frame) {
        pickMemo_.clear();
        pickMemoFrame_ = frame;
    }
    const auto hit = pickMemo_.find(ai);
    if (hit != pickMemo_.end() && hit->second.targets == targets) {
        std::memcpy(weights, hit->second.weights, sizeof(weights));
        std::memcpy(pick, hit->second.pick, sizeof(pick));
        return true;
    }
    thread_local std::vector<float> w;
    w.resize(targets);
    for (std::uint32_t k = 0; k < targets; ++k) {
        if (!rd(setup, 0xc * static_cast<std::size_t>(k) + 4, &w[k], 4)) return false;
    }
    const sc::MorphPick mp = sc::pickMorphTargets(w);
    PickMemo m;
    m.targets = targets;
    for (std::uint32_t i = 0; i < 3; ++i) {
        weights[i] = m.weights[i] = mp.weight[i];
        pick[i] = m.pick[i] = mp.target[i];
    }
    pickMemo_[ai] = m;
    return true;
}

// composeActorImpl's reads, up to the primitive's shader (and the morph pick when asked)
int StaticOps::actorRead(const Op& op, ActorIn& in, bool weights) {
    in = {};
    U8* ps = ptr(op.raw.context, 0x18);  // eCAnimation_PS
    if (ps == nullptr) return 1;
    if ((get<std::uint8_t>(ps, 0x18) & 0x10) != 0) return 2;  // a template property set: no illumination (the engine would dereference null)
    in.ps = ps;
    in.entity = ptr(ps, 0x10);
    in.actorIdx = op.raw.desc & 0xffffu;
    in.primIdx = op.raw.desc >> 16;
    const std::uint32_t eflags = get<std::uint32_t>(in.entity, 0x28);
    if (eflags & 0x200000u) in.flags |= 2;
    if (eflags & 0x400000u) in.flags |= 1;
    if (eflags & 0x800000u) in.flags |= 4;
    if (in.actorIdx == 0) {
        in.wrapper = ps + 0x30;
        in.variant = get<std::int32_t>(ps, 0x20);
    } else {
        if (in.actorIdx > get<std::uint16_t>(ps, 0x1b8)) return 1;
        U8* part = ptr(ptr(ps, 0x1b0), 8 * static_cast<std::size_t>(in.actorIdx - 1));
        if (part == nullptr) return 1;
        in.wrapper = part + 0x30;
        in.variant = get<std::int32_t>(part, 0x20);
    }
    in.res = ptr(in.wrapper, 0x18);
    in.hra = ptr(in.res, 0x98);
    in.ai = ptr(in.wrapper, 8);
    if (in.res == nullptr || in.hra == nullptr || in.ai == nullptr || ptr(in.res, 0x70) == nullptr) return 1;
    in.lod = sc::actorLod(get<std::uint32_t>(in.ai, 0x250));
    if (get<std::int32_t>(ptr(in.hra, 0), 0x38) != 3) return 3;
    in.primIndex = sc::actorPrimitive(get<std::uint16_t>(in.hra, 0x38 + 0x14 * static_cast<std::size_t>(in.lod)), static_cast<std::uint16_t>(in.primIdx));
    U8* prims = ptr(in.hra, 0x90);
    if (prims == nullptr) return 1;
    in.prim = prims + 0x20 * static_cast<std::size_t>(in.primIndex);
    in.material = ptr(in.prim, 0x10);
    in.shader = ptr(in.material, 0x70);
    if (in.material == nullptr || in.shader == nullptr) return 1;
    in.boneStart = get<std::uint16_t>(in.prim, 0x18);
    in.bones = sc::primitiveBones(static_cast<std::uint16_t>(in.boneStart), get<std::uint16_t>(in.prim, 0x1a));
    in.remap = ptr(in.hra, 0x28);
    in.visual = ptr(in.hra, 0xa0);
    in.targets = get<std::uint32_t>(in.hra, 0xa8);
    if (in.targets != 0) {
        if (in.targets > kMaxMorphTargets) return 1;
        if (weights) {
            U8* setup = ptr(in.ai, 0x1d0);
            if (setup == nullptr) return 1;
            // (the weights are the actor instance's: every part of it and every view of the frame reads the same ones, kept per instance and frame)
            if (!actorPick(in.ai, setup, in.targets, in.weights, in.pick)) return 1;
        }
    }
    return 0;
}

std::string StaticOps::actorDiffText(const Op& op, const orec::DrawRec& r) {
    ActorIn in;
    if (actorRead(op, in, true) != 0) return " [actor: unreadable]";
    std::string t = std::format(" [actor: lod {} prim {} targets {} pick {} {} {} weights {} {} {}; visual streams", in.lod, in.primIndex, in.targets, in.pick[0], in.pick[1], in.pick[2], in.weights[0],
                                in.weights[1], in.weights[2]);
    if (const auto* hp = hal::vulkan::HalPrimitive::fromApi(ptr(in.visual, 0x10))) {
        for (std::uint32_t i = 0; i < 4; ++i) {
            const auto v = hp->streamView(i);
            t += std::format(" {}:[buf {} off {} stride {} freq {} on {}] record buf {}", i, static_cast<const void*>(v.buffer), v.offset, v.stride, v.freq, v.enabled, static_cast<const void*>(r.streams[i]));
        }
    }
    return t + "]";
}

orec::Excl StaticOps::recExclusionActor(const Op& op) {
    orec::Excl ex = orec::Excl::Count;
    const auto reason = [&](orec::Excl e) {
        if (ex == orec::Excl::Count) ex = e;
    };
    ActorIn in;
    if (const int rc = actorRead(op, in, false); rc != 0) {
        reason(orec::Excl::NotComposed);
        return ex;
    }
    if (in.bones > gpuscene::kBoneMaxBones) reason(orec::Excl::NotComposed);
    // F6-2: the overlay flags change only the solid / alpha passes (GetPasses: the highlight passes; the z pass is the special pass without them): with the ordered path
    // those draws are drawn at the op's place; the flags are in the stamp (entityFlags), a change rebuilds the record
    const bool f62 = f62On();
    const bool ordered = f62 && ownedOrderedOn() && op.list != List::ZPass && viewKind_ == ViewKind::Main;
    if ((in.flags != 0 || (get<std::uint32_t>(in.entity, 0x28) & 0xe00000u) != 0) && !(f62 && (op.list == List::ZPass || ordered))) reason(orec::Excl::Overlay);
    // F6-2: a fade < 1 is c32.w of the bone image (served per frame) + the pass states (blended: the ordered path); the stamp holds its class (fadeClass)
    if (!(op.raw.fade >= 1.0f) && !(f62 && (op.list == List::ZPass || ordered))) reason(orec::Excl::Fade);
    if (op.list == List::Alpha && !ordered) reason(orec::Excl::NotComposed);
    if (op.list == List::Solid || op.list == List::Alpha) {
        U8* illumPs = ptr(in.entity, 0xf8);
        U8* illum = illumPs != nullptr ? illumPs + 0x78 : nullptr;
        if (illum == nullptr || ptr(illum, 0) == nullptr) reason(orec::Excl::NoList);
        if (!tableLit_) reason(orec::Excl::NotTableLit);
    }
    U8* shader = in.shader;
    if (!shaderClassOk(shader)) {
        reason(orec::Excl::ShaderClass);
    } else {
        if (op.list == List::Solid) {
            sc::BlendInputs b;
            b.mode = get<std::int32_t>(shader, 0x50);
            b.hasOpacity = get<std::uint8_t>(shader, 0x330) != 0;
            b.refracted = get<std::uint8_t>(shader, 0x420) != 0;
            b.tradeAlphaAsSolid = get<std::uint8_t>(shader, 0x79) != 0;
            if ((sc::blendsAtFadeOne(b) || sc::isBlended(b)) && !ordered) reason(orec::Excl::Blended);
        }
        // (+0x2b8 is non-zero for every skinned material shader: the sort id of the static meshes' composition has no meaning for the actors' passes)
        if (get<std::uint8_t>(shader, 0x78) != 0 && !ordered) reason(orec::Excl::DepthBias);  // (F6-2: an ordered op reads its list's depth-bias view slot)
    }
    return ex;
}

// the stamp of an actor op: the same fields as recStamp, with the actor's own geometry (the HRA's primitive, its remap, the morph pick)
bool StaticOps::recStampActor(const Op& op, orec::Stamp& st) {
    st = {};
    ActorIn in;
    if (actorRead(op, in, true) != 0) return false;
    st.res = in.wrapper;
    st.entity = in.entity;
    st.entityFlags = get<std::uint32_t>(in.entity, 0x28) & 0xe00000u;
    U8* illumPs = ptr(in.entity, 0xf8);
    st.illumPs = illumPs;
    U8* illum = illumPs != nullptr ? illumPs + 0x78 : nullptr;
    U8* list = ptr(illum, 0);
    st.light = mix(mix(mixPtr(mixPtr(0, illum), list), get<std::uint8_t>(list, 0) | (get<std::uint8_t>(illum, 0xc) << 8) | (get<std::uint8_t>(illum, 0xd) << 16)),
                   get<std::uint8_t>(list, 0x34));
    st.switchValue = in.variant;
    // (F6-2: the fade's class: the value itself is the bone image's c32.w, written per frame; the pass states depend only on fade < 1)
    st.fadeBits = std::bit_cast<std::uint32_t>(f62On() ? fadeClass(op.list == List::ZPass ? 1.0f : op.raw.fade) : op.raw.fade);
    st.lightmaps = 0;
    {
        U8* hres = ptr(in.hra, 0);
        std::uint64_t h = mixPtr(mixPtr(mixPtr(mixPtr(0, in.hra), ptr(in.hra, 0x90)), in.remap), in.visual);
        h = mixPtr(h, ptr(in.visual, 0x10));  // the visual's record (our HalPrimitive: declaration, streams, indices)
        h = mixPtr(h, in.res);
        h = mix(h, static_cast<std::uint64_t>(in.lod) | (static_cast<std::uint64_t>(in.primIndex) << 8) | (static_cast<std::uint64_t>(in.targets) << 24) |
                       (static_cast<std::uint64_t>(get<std::int32_t>(hres, 0x38)) << 48));
        h = mix(h, get<std::uint32_t>(in.prim, 0) | (static_cast<std::uint64_t>(get<std::uint32_t>(in.prim, 4)) << 32));
        h = mix(h, get<std::uint32_t>(in.prim, 8) | (static_cast<std::uint64_t>(get<std::uint32_t>(in.prim, 0xc)) << 32));
        h = mix(h, get<std::uint16_t>(in.prim, 0x18) | (static_cast<std::uint64_t>(get<std::uint16_t>(in.prim, 0x1a)) << 16));
        h = mix(h, get<std::uint32_t>(in.hra, 0x48 + 0x14 * static_cast<std::size_t>(in.lod)));  // the LOD's vertex count (the morph stream offsets)
        for (std::uint32_t i = 0; i < in.bones && i < gpuscene::kBoneMaxBones; ++i) h = mix(h, get<std::uint16_t>(in.remap, 2 * (static_cast<std::size_t>(in.boneStart) + i), 0xffff));
        // (F6-2: the pick is the stamp's `lightmaps` field (an actor has no lightmaps): a pick seen before swaps its retained draws in, actorPickSwap)
        if (in.targets != 0 && !f62On()) h = mix(mix(mix(h, in.pick[0]), in.pick[1]), in.pick[2]);
        if (in.targets != 0 && f62On()) st.lightmaps = mix(mix(mix(0x51ed270b27f3c6a1ull, in.pick[0]), in.pick[1]), in.pick[2]);
        U8* m = in.material;
        U8* sh = in.shader;
        h = mixPtr(mixPtr(mixPtr(h, m), sh), ptr(sh, 0));  // material, shader, its vtable (the class)
        h = mix(h, static_cast<std::uint64_t>(get<std::int32_t>(m, 0x38)) | (static_cast<std::uint64_t>(get<std::int32_t>(sh, 0x50)) << 32));
        h = mix(h, get<std::uint8_t>(sh, 0x58) | (get<std::uint8_t>(sh, 0x79) << 8) | (get<std::uint8_t>(sh, 0x330) << 16) | (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x420)) << 24) |
                       (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x2d8)) << 32) | (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x78)) << 40) |
                       (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x2b8)) << 48) | (static_cast<std::uint64_t>(get<std::uint8_t>(sh, 0x2a8)) << 56));
        h = mixPtr(mix(h, static_cast<std::uint32_t>(get<std::int32_t>(sh, 0x68))), ptr(sh, 0x220));
        st.geom = h;
    }
    U8* entity = ptr(op.raw.context, 0x10);
    const std::uint32_t slot = engine::scenetable::find(entity);
    if (slot != UINT32_MAX) {
        if (const engine::scenetable::Slot* s = engine::scenetable::slot(slot)) {
            st.objSlot = slot;
            st.objGen = s->generation;
        }
    }
    st.variantEpoch = dev_.variantEpoch();
    st.registryGen = enh::fullLitSkipsReads() ? enh::LightTable::registryGeneration() : 0;
    return true;
}

gpuscene::BoneKey StaticOps::actorKeyOf(const Op& op, const ActorIn& in) const {
    gpuscene::BoneKey k;
    k.wrapper = in.wrapper;
    k.prim = in.primIdx;
    const float fade = op.list == List::ZPass ? 1.0f : op.raw.fade;
    k.fadeBits = std::bit_cast<std::uint32_t>(f62On() ? fadeClass(fade) : fade);
    return k;
}

bool StaticOps::actorImage(const Op& op, const ActorIn& in, float (*regs)[4], std::uint32_t& count) {
    if (in.bones > gpuscene::kBoneMaxBones || frameAdmin_ == nullptr) return false;
    std::memset(regs, 0, 2 * 16);
    frameId_ = get<std::uint32_t>(frameAdmin_(), 0x4c);
    // c31.xyz = the three morph weights, c32.w = SetGlobalAlpha (the z pass draws with 1.0)
    std::memcpy(regs[0], in.weights, 12);
    regs[1][3] = op.list == List::ZPass ? 1.0f : op.raw.fade;
    for (std::uint32_t i = 0; i < in.bones; ++i) {
        float m[16];
        const std::uint16_t b = get<std::uint16_t>(in.remap, 2 * (static_cast<std::size_t>(in.boneStart) + i), 0xffff);
        if (!paletteMatrix(in.wrapper, b, m)) return false;
        const std::uint32_t r = sc::boneRegister(i) - gpuscene::kBoneFirstReg;
        for (std::uint32_t k = 0; k < 3; ++k) column(m, k, regs[r + k]);
    }
    count = 2 + 3 * in.bones;
    if (in.targets != 0) {
        ++actorW_.morphOps;
        if (in.weights[0] != 0.0f || in.weights[1] != 0.0f || in.weights[2] != 0.0f) ++actorW_.morphActive;
    }
    return true;
}

// the composed draws (setOwnedFor): VS window 2 served from the bone table with the registers the composition wrote
void StaticOps::actorServeComposed(const sc::DrawDesc& d, d3d9::OwnedWindows::Win& out, bool& served) {
    served = false;
    ++actorCur_.draws;
    const std::uint64_t bit = ownedDrawIndex_ < 64 ? 1ull << ownedDrawIndex_ : 0;
    actorCur_.drawMask |= bit;
    actorCur_.offeredMask &= ~bit;
    if (!actorCur_.valid || actorCur_.bones > gpuscene::kBoneMaxBones) return;
    // (what the composition wrote into the register pool, not the draw's snapshot: a shader that does not read c31 / c32 has no live copy of them)
    static thread_local float regs[gpuscene::kBoneRegs][4];
    std::memset(regs, 0, 2 * 16);
    std::memcpy(regs[0], actorCur_.weights, 12);
    regs[1][3] = actorCur_.fade;
    for (std::uint32_t i = 0; i < actorCur_.bones; ++i) {
        const std::uint32_t r = sc::boneRegister(i) - gpuscene::kBoneFirstReg;
        std::memcpy(regs[r], mirror_.global[r], 48);
    }
    (void)d;
    gpuscene::BoneKey key;
    key.wrapper = actorCur_.wrapper;
    key.prim = actorCur_.prim;
    key.fadeBits = std::bit_cast<std::uint32_t>(f62On() ? fadeClass(actorCur_.fade) : actorCur_.fade);
    if (gpuscene::bonetable().serve(key, regs, 2 + 3 * actorCur_.bones, out, actorCur_.histTag) == gpuscene::BoneWhy::Served) {
        served = true;
        ++actorCur_.offered;
        actorCur_.offeredMask |= bit;
        ++actorW_.offeredDraws;
    } else {
        ++actorW_.composedDraws;
    }
}

bool StaticOps::ownedObjWindow(const orec::OpRecord& rec, const orec::DrawRec& r, d3d9::OwnedWindows::Win& out) const {
    if (rec.actor) return gpuscene::bonetable().windowFor(rec.actorKey, out);
    if (rec.instances) return gpuscene::rowtable().windowFor(r.row.slot, r.row.bits, out);  // F2f: the draw's instance row image
    return gpuscene::objecttable().windowFor(rec.stamp.objSlot, out);
}

// the owned draw's window 2: the bone image of the part, written once per frame and key (the second view of the frame finds it served)
bool StaticOps::ownedActorWindow(const Op& op, const orec::OpRecord& rec, d3d9::OwnedWindows::Win& out, int& why) {
    constexpr int kFbObjectWhy = 2;  // (static_ops_owned.cpp's kFbObject)
    why = kFbObjectWhy;
    gpuscene::BoneTable& bt = gpuscene::bonetable();
    if (updateRenderMatrices_ == nullptr || frameAdmin_ == nullptr || !bt.ready()) return false;
    const gpuscene::BoneKey& key = rec.actorKey;
    if (bt.servedNow(key, out)) {
        ++actorW_.imageShared;
        return true;
    }
    const auto t0 = tnow();
    ActorIn in;
    if (actorRead(op, in, true) != 0 || in.wrapper != rec.stamp.res || in.bones != rec.actorBones) return false;
    // the engine slot's side effects (the lean commit's: UpdateRenderMatrices once per frame and actor, the touches of the resource and the material)
    frameId_ = get<std::uint32_t>(frameAdmin_(), 0x4c);
    if (!updateRenderMatrices_(in.wrapper, frameId_)) return false;
    touchNow(ptr(in.hra, 0));
    touchNow(in.material);
    static thread_local float regs[gpuscene::kBoneRegs][4];
    std::uint32_t count = 0;
    if (!actorImage(op, in, regs, count) || count != 2 + 3 * rec.actorBones) return false;
    if (bt.serve(key, regs, count, out, actorHistTag(in.hra, in.lod, in.pick)) != gpuscene::BoneWhy::Served) return false;
    ++actorW_.imageBuilds;
    actorW_.imageNs += nsOf(t0, tnow());
    return true;
}

// check frames: the composed draws wrote the image into the table (actorServeComposed); the engine's palette now must give the same bytes
void StaticOps::actorVerify(const Op& op, const orec::OpRecord& rec, bool& differs, std::string& detail) {
    differs = false;
    ActorIn in;
    if (actorRead(op, in, true) != 0 || in.wrapper != rec.stamp.res) return;
    static thread_local float regs[gpuscene::kBoneRegs][4];
    std::uint32_t count = 0;
    if (!actorImage(op, in, regs, count)) return;
    const gpuscene::BoneRecord* got = gpuscene::bonetable().image(rec.actorKey);
    ++actorW_.verified;
    if (got == nullptr) {
        differs = true;
        detail = "no bone table slot";
        return;
    }
    // (the carry-over registers are zero in both: compare the whole image)
    for (std::uint32_t i = 0; i < count; ++i) {
        float want[4];
        std::memcpy(want, regs[i], 16);
        if (i == 0) want[3] = 0.0f;
        if (i == 1) want[0] = want[1] = want[2] = 0.0f;
        if (std::memcmp(want, got->regs[i], 16) != 0) {
            differs = true;
            detail = std::format("bone image register c{}: engine ({:g} {:g} {:g} {:g}) vs table ({:g} {:g} {:g} {:g})", gpuscene::kBoneFirstReg + i, want[0], want[1], want[2], want[3], got->regs[i][0],
                                 got->regs[i][1], got->regs[i][2], got->regs[i][3]);
            ++actorW_.verifyDiffers;
            return;
        }
    }
}

namespace {
constexpr const char* kCenExtraNames[] = {"check / warm-up / gated view", "record refused at selection", "no record yet", "alpha list (not recorded)", "refracted (empty slot)", "view not recorded"};
std::size_t cenList(List l) { return l == List::ZPass ? 0 : l == List::Solid ? 1 : l == List::Alpha ? 2 : 3; }
}  // namespace

void StaticOps::actorCensusOwned(const Op& op) {
    const std::size_t v = viewKind_ == ViewKind::Main ? 0 : viewKind_ == ViewKind::Cascade ? 1 : 2;
    ++actorW_.cenOwned[v][cenList(op.list)];
}

void StaticOps::actorCensus(const Op& op) {
    if (!ownedActors_ || !recs_) return;
    const std::size_t v = viewKind_ == ViewKind::Main ? 0 : viewKind_ == ViewKind::Cascade ? 1 : 2;
    const std::size_t l = cenList(op.list);
    std::size_t why = orec::kExclCount;
    if (op.list == List::Refracted) {
        why += kCenRefractedEmpty;
    } else if (!ownedView_.active) {
        why += kCenCheck;
    } else if (op.list == List::Alpha && !actorOrderedNow(&op)) {
        why += kCenAlphaList;
    } else if ((v == 2 && !ownedShadowKind(viewKind_)) || (v == 1 && !ownedShadows_)) {  // (F3-4: the terrain map / atlas cells are recorded with owned_shadow_views)
        why += kCenNotRecordedView;
    } else if (recs_->map.find(orec::Key{op.raw.context, op.raw.desc, static_cast<std::uint8_t>(op.list), ownedViewKey_}) != recs_->map.end()) {
        why += kCenRecordRefused;
    } else {
        const orec::Excl ex = recExclusionActor(op);
        why = ex == orec::Excl::Count ? orec::kExclCount + kCenNoRecord : static_cast<std::size_t>(ex);
    }
    ++actorW_.cenWhy[v][l][why];
}

// F6-2: the record's draws for the pick `pick` (its stamp's `lightmaps`): swapped in from the pick sets when this pick was seen before; true = the record now holds them
bool StaticOps::actorPickSwap(orec::OpRecord& rec, std::uint64_t pick) {
    if (!rec.actor || !f62On() || rec.stamp.lightmaps == pick) return false;
    for (orec::OpRecord::PickSet& ps : rec.pickSets) {
        if (ps.pick != pick) continue;
        std::swap(ps.draws, rec.draws);
        ps.pick = rec.stamp.lightmaps;
        ps.used = dev_.stats().frames;
        rec.stamp.lightmaps = pick;
        ++actorW_.pickSwaps;
        return true;
    }
    ++actorW_.pickMisses;
    return false;
}

// F6-2: a record rebuilt for a new pick keeps the old one's draws (and its other pick sets) for the picks they were made for
void StaticOps::actorPickKeep(orec::OpRecord& fresh, orec::OpRecord& old) {
    if (!fresh.actor || !old.actor || !f62On() || old.stamp.lightmaps == fresh.stamp.lightmaps || old.draws.size() != fresh.draws.size()) return;
    orec::Stamp a = old.stamp, b = fresh.stamp;
    a.lightmaps = b.lightmaps = 0;
    a.view = a.materials = b.view = b.materials = 0;
    if (orec::firstDifference(a, b) >= 0 || old.stamp.view != fresh.stamp.view || old.stamp.materials != fresh.stamp.materials) return;  // (only the pick differs)
    fresh.pickSets = std::move(old.pickSets);
    std::erase_if(fresh.pickSets, [&](const orec::OpRecord::PickSet& p) { return p.pick == fresh.stamp.lightmaps || p.draws.size() != fresh.draws.size(); });
    if (fresh.pickSets.size() >= orec::OpRecord::kPickSets) {
        const auto lru = std::min_element(fresh.pickSets.begin(), fresh.pickSets.end(), [](const auto& x, const auto& y) { return x.used < y.used; });
        fresh.pickSets.erase(lru);
    }
    orec::OpRecord::PickSet ps;
    ps.pick = old.stamp.lightmaps;
    ps.draws = std::move(old.draws);
    ps.used = dev_.stats().frames;
    fresh.pickSets.push_back(std::move(ps));
    ++actorW_.pickKept;
}

void StaticOps::actorReport() {
    if (!ownedActorsOn() && actorW_.drawn == 0 && actorW_.offeredDraws == 0) return;
    ActorOwnedStats& w = actorW_;
    actorT_.add(w);
    static const char* const kViews[3] = {"main z", "main solid", "cascades"};
    std::string views;
    for (std::size_t i = 0; i < 3; ++i) {
        if (w.seenByView[i] == 0) continue;
        views += std::format("{}{} {} of {} ops ({:.1f} %)", views.empty() ? "" : ", ", kViews[i], w.byView[i], w.seenByView[i], 100.0 * static_cast<double>(w.byView[i]) / static_cast<double>(w.seenByView[i]));
    }
    spdlog::info("native: owned actors (5 s): owned share: {}; images built {} (shared with another view of the frame {}), window refused {}; composed draws served from the bone table {} (not {}); "
                 "check frames: {} images compared, DIFFERING {}; images of morph actors {} (active weights {}); records built {} ({} draws: not a scene draw {}, not replayable {}), rebuilt by a stamp difference {}; CPU: image {:.1f} us per 5 s window",
                 views.empty() ? "none" : views, w.imageBuilds, w.imageShared, w.refusedWindow, w.offeredDraws, w.composedDraws, w.verified, w.verifyDiffers, w.morphOps, w.morphActive, w.recordsBuilt, w.drawsBuilt, w.drawNoScene, w.drawCdInvalid, w.stampRebuilds,
                 static_cast<double>(w.imageNs) / 1000.0);
    {
        static const char* const kV[3] = {"main", "cascades", "other"};
        static const char* const kL[4] = {"z", "solid", "alpha", "refracted"};
        std::string cen;
        for (std::size_t v = 0; v < 3; ++v) {
            for (std::size_t l = 0; l < 4; ++l) {
                std::uint64_t composed = 0;
                std::string why;
                for (std::size_t k = 0; k < std::size(w.cenWhy[v][l]); ++k) {
                    const std::uint64_t n = w.cenWhy[v][l][k];
                    if (n == 0) continue;
                    composed += n;
                    why += std::format("{}{} {}", why.empty() ? "" : ", ", k < orec::kExclCount ? orec::kExclNames[k] : kCenExtraNames[k - orec::kExclCount], n);
                }
                const std::uint64_t all = composed + w.cenOwned[v][l];
                if (all == 0) continue;
                cen += std::format("{}{} {}: {} ops, owned {} ({:.1f} %), composed {}{}{}{}", cen.empty() ? "" : "; ", kV[v], kL[l], all, w.cenOwned[v][l], 100.0 * static_cast<double>(w.cenOwned[v][l]) / static_cast<double>(all),
                                   composed, why.empty() ? "" : " (", why, why.empty() ? "" : ")");
            }
        }
        std::string ex;
        for (std::size_t k = 0; k < orec::kExclCount; ++k) {
            if (w.excl[k] != 0) ex += std::format("{}{} {}", ex.empty() ? "" : ", ", orec::kExclNames[k], w.excl[k]);
        }
        if (!cen.empty()) {
            spdlog::info("native: actor census (5 s): {}; not recorded after the composition: {}; records not built: draws not offered the bone table {}, packets not retained {}; ordered (drawn at their place): {} of {}; morph picks: {} swapped in from earlier picks, {} new (composed), {} kept for later",
                         cen, ex.empty() ? "none" : ex, w.notOffered, w.notRetained, w.ordOwned, w.ordSeen, w.pickSwaps, w.pickMisses, w.pickKept);
        }
    }
    if (w.verifyDiffers != 0) {
        ownedActorsTripped_ = true;
        ownedActors_ = false;
        spdlog::error("native: owned actors turned off: {} bone images DIFFERING on the check frames (the table's image is not the engine's palette)", w.verifyDiffers);
    }
    w = {};
}

}  // namespace rvk::native
