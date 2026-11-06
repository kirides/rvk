#pragma once

// Frame ownership stage F0, "observe hooks" (docs/research/frame-ownership.md 1.1, 2 row F0, 3.2; setting native.owned_observe,
// Debug, default off). Detours the render contexts' RequestRenderOps (vtable slot +0x40: Mesh 0x5c75e0, SpeedTree 0x6ee4e0,
// SpeedTreeBB 0x6ee9b0, Vegetation 0x70d4f0, Animation2 0x4ddd00; B-frustum-db.md 2.5) and records, per call, the database, the
// context + its entity, the LOD, the fade argument and the op list sizes before / after (the 8 lists of eCFrustumDatabase:
// +0x14d0 pre-pass, +0x14e8 alpha, +0x1500 solid, +0x1518 z, +0x1530 solid-late, +0x1548 alpha-late, +0x1560 debug, +0x1578
// refracted). The original always runs unchanged (observe only). The per-view visible list (a database's requests of the frame)
// is closed when the frame tap reports the view's Execute / ExecuteShadowMap (the database pointer is classified there), and
// verified against the database's live op lists (position by position: every op of the five classes comes from a recorded
// request of the same context in the same database; every request's appended range holds its context's ops) and against the
// frame tap's own snapshot. Render thread only.

#include <array>
#include <cstdint>
#include <vector>

#include "native/frame_desc.h"

namespace rvk::native::owned_observe {

// the eight op lists of eCFrustumDatabase that a request may append to (offsets as in kOffsets)
inline constexpr std::size_t kLists = 8;
inline constexpr std::array<std::uint32_t, kLists> kOffsets{0x14d0, 0x14e8, 0x1500, 0x1518, 0x1530, 0x1548, 0x1560, 0x1578};

// one RequestRenderOps call
struct Request {
    const void* ctx = nullptr;       // the render context
    const void* entity = nullptr;    // ctx +0x10
    const void* lodResource = nullptr;  // Mesh: ctx +0x28 at the call (the LOD's eCMeshResource2, picked by UpdateLoD for this database's camera)
    float fade = 1.0f;               // the call's fade argument
    std::uint16_t mask = 0;          // the call's caps & request mask
    OpKind kind = OpKind::Unknown;   // Mesh / Animation / SpeedTree / SpeedTreeBB / Vegetation
    std::uint32_t slot = UINT32_MAX;  // engine/scene_table slot of the entity (resolved when the view closes; UINT32_MAX = none)
    std::int32_t lod = -1;           // Mesh: the LOD index of lodResource (meshPS +0x40[]); Animation: the actor's +0x250; else -1
    std::array<std::uint32_t, kLists> before{}, after{};  // op list counts around the original
    std::uint32_t ops() const {
        std::uint32_t n = 0;
        for (std::size_t i = 0; i < kLists; ++i) n += after[i] - before[i];
        return n;
    }
};

// the closed visible list of one view
struct View {
    std::uint64_t frame = 0;
    const void* db = nullptr;
    ViewKind kind = ViewKind::Main;
    std::vector<Request> requests;
};

// Creates the detours (before hooks::enableAll); false when none could be created. Called at start only when native.owned_observe
// is on then: with the setting off at start nothing is hooked (turning it on later logs that a restart is needed).
bool install();
bool installed();
// the run-time switch (the hooks stay installed; off = one flag test per call)
void setEnabled(bool on);
bool enabled();

// the frame tap, at the start of a view's Execute / ExecuteShadowMap (before the engine sorts the lists): closes the database's
// requests as that view's visible list and verifies it. `desc` = the tap's snapshot of the same view.
void viewBegin(void* db, ViewKind kind, std::uint64_t frame, const FrameDesc& desc);

// the last closed visible list of a view kind (valid until the next view of that kind)
const View& lastView(ViewKind kind);

// every 5 s (HAL stats): `native: owned observe (5 s): ...`
void report();

}  // namespace rvk::native::owned_observe
