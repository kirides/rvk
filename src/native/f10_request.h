#pragma once

// F10-1 (docs/research/frame-ownership.md 14.31, 15.5): the main view's request of the static mesh contexts done natively, and their z / solid ops handed to
// the owned path without passing through the engine's op lists. Setting `native.own_collection` (Debug, Full, run time, default off): off | on | compare.
//
// The engine's eCFrustumDatabase::RequestRenderOps 0x66cfe0 calls, per extracted element with a fade > 0, eCFrustumElementItem::Request 0x671370
// (decompiled; section 14.31): the illumination update (mask bit 0x80), the visible stamp (item +0xc = db +8, mask bit 0x40), and per render context the
// LOD update (vtable +0x30) and the context's RequestRenderOps (vtable +0x40). For eCRenderContextMesh (0x5c75e0) that is: the LRU touch of the LOD resource
// and of every sub-mesh material (eCResourceCache2::CacheResource; QueueData when not resident), the view depth key from the entity's sphere, and the ops
// (eCRenderOp2::Create 0x67f680: ctx, desc, fade 1, key, sort id = material +0x78 & 0xfff | GetAdditionalSortID << 12 | shader +0x7c << 20) into the
// solid (+0x1500), alpha (+0x14e8), refracted (+0x1578) and z (+0x1518) lists by the shaders' IsRefracted / IsBlended.
//
// With the stage on, a detour of Request runs that function for the main database itself: the illumination update, the LOD update and every other
// context class stay the engine's calls; a mesh context without hierarchy nodes and render instances whose resources are all resident (only CacheResource
// touches) is requested by the port below: the same touches in the same order (engine calls), the same op bytes. On a frame predicted owned its z / solid
// ops are DEFERRED: they never enter the engine's lists; at the main view's Execute (native/frame_tap.cpp beginView) they are merged with the engine's list
// at the exact positions the engine would have written them (insertion index = the list's count at creation), and the owned path takes the owned ones
// before the engine sees the list; the rest (refused, or a frame that turns out not owned) is written into the engine's list in that merged order, so the
// engine executes byte for byte the list it would have built itself. Check frames (`native.owned_check`) and `compare` mode run the engine's own
// RequestRenderOps for every context and compare it with the port run dry just before: the appended op bytes per list and the LRU state the touches leave
// (head order + clock stamps per cache); rvk.log `native: own collection (5 s)`, DIFFERING must stay 0 (a difference turns the stage off).

#include <cstdint>
#include <functional>
#include <vector>

#include "native/frame_desc.h"

namespace rvk::native::f10req {

// ---- pure parts (unit-tested, tests/f10_request_tests.cpp) ----

// the engine's lists a mesh context writes (kListOffset index: ZPass 0, Solid 1, Refracted 4, Alpha 5)
inline constexpr std::uint8_t kListZ = 0, kListSolid = 1, kListRefracted = 4, kListAlpha = 5;

// one sub-mesh of the LOD resource as the engine's RequestRenderOps sees it
struct SubIn {
    bool material = false;   // sub-mesh +0x38 non-null
    bool refracted = false;  // shader non-null and IsRefracted (vtable +0x1a8)
    bool blended = false;    // shader non-null and IsBlended (vtable +0x198)
    std::uint32_t sortId = 0;  // what eCRenderOp2::Create writes at +0x14 for this material
};
struct OpOut {
    std::uint8_t list = 0;  // kListOffset index
    RawOp op;
};
// eCRenderContextMesh::RequestRenderOps's op creation for a context without hierarchy nodes (ctx +0x34 == 0) and without render instances (res +0xa0 == 0),
// resource resident: the ops in creation order. `one` = the engine's 1.0 (DAT_1809f1698), `mask` = the context's mask & the request's.
void meshOps(const SubIn* subs, int n, std::uint16_t mask, float fade, float one, const void* ctx, std::uint32_t key, std::vector<OpOut>& out);
// the 24-bit view depth key: ((c.y * p.y + p.x * c.x) + c.z * p.z + p.w - r) * scale, truncated, clamped to [0, 0xffffff] (sphere = {r, cx, cy, cz},
// plane = db +0x94 .. +0xa0, scale = db +0xa4; the engine's float order)
std::uint32_t depthKey(const float sphere[4], const float plane[4], float scale);
// eCRenderOp2::Create's sort id
inline std::uint32_t sortIdOf(std::uint32_t material78, bool hasShader, std::uint8_t additional, std::int32_t priority) {
    std::uint32_t s = material78 & 0xfffu;
    if (hasShader) s |= (static_cast<std::uint32_t>(additional) << 12) | (static_cast<std::uint32_t>(priority) << 20);
    return s;
}
// a deferred op: it would have been written at position `pos` of its list (the list's count when it was created)
struct Deferred {
    std::uint32_t pos = 0;
    RawOp op;
};
// the engine's list as it would have been: `engine` (n ops, without the deferred ones) with every deferred op inserted at its position (deferred ops of one
// position in creation order, before the engine op that was created after them)
void mergeDeferred(const RawOp* engine, std::uint32_t n, const std::vector<Deferred>& d, std::vector<RawOp>& out);
// the reverse (compare mode): `full` = the engine's list, `taken` = positions (ascending) of ops a deferral would have kept out -> the engine part + the
// deferred ops with their insertion positions
void splitDeferred(const std::vector<RawOp>& full, const std::vector<std::uint32_t>& taken, std::vector<RawOp>& engine, std::vector<Deferred>& d);

// the LRU state a sequence of CacheResource calls leaves at the head of a list: the distinct resources, the last touched first
void expectedHead(const std::vector<const void*>& touches, std::vector<const void*>& head);

// ---- engine side ----

// once at start (bootstrap, Vulkan mode): the detour of eCFrustumElementItem::Request (passes through while the stage is off)
bool install();
// FrameTap::beginView of a main view: the database, whether the view is owned, whether the owned path's gate was open, the device's frame number and
// native.owned_check (the next frames' prediction)
void noteMainView(void* db, bool ownedFrame, bool gateOpen, std::uint64_t frame, int checkInterval);
// FrameTap::beginView: the deferred z / solid ops of `db` for list `list` (kListZ / kListSolid), moved out (empty when none); `pass` = the request pass
// they belong to is checked against the database's frame counter
bool takeDeferred(void* db, std::uint8_t list, std::vector<Deferred>& out);
// the engine's list grow function (bTValArray<eCRenderOp2>::SetCount, FUN_1800ae210): makes room for `count` ops and sets the count; false = not bound
bool listSetCount(void* db, std::uint8_t list, std::int32_t count);
// once per present (engine thread): the 5 s report
void onPresent();
// F10-2 (native.own_extract): the main view's database of the last Execute (null before the first) and whether the next main view falls on a check /
// warm-up frame of native.owned_check (the same prediction the request pass uses)
void* mainDb();
bool nextViewIsCheck();
// the stage line (static_ops_stages.cpp): setting / armed / acted
struct StageState {
    int setting = 0;   // native.own_collection
    int armed = 0;     // the mode the code runs (0 = off: not installed, tripped, ...)
    std::uint64_t acted = 0;  // contexts requested by the port in the window (live)
    std::uint64_t deferred = 0, compared = 0, differing = 0;
};
StageState stageState();

}  // namespace rvk::native::f10req
