#pragma once

// F10-2 (docs/research/frame-ownership.md 14.33): the main view's eCFrustumDatabase::ExtractFromFrustum 0x66fa60 done by the renderer.
// Setting `native.own_extract` (Debug, Full, run time, default off): off | on | compare.
//
// The port (src/native/f10_extract.cpp) is the engine's function written down, on live engine data, in the engine's order:
//   1. the camera's portal room (eCPortalAdmin::FindPortalRoom): from the frustum beam the engine built in UpdateFrameDependencies, the room walk
//      (ExtractFromPortalRooms, f10_portal.h) builds the final beam, the near plane added at its root; outdoors the final beam is the frustum;
//   2. the occluders (portal admin +0x38): CullNode of the occluder's entity, eCOccluder_PS::AddOccluder ported (f10_occluder.h), the descs sorted with the
//      engine's own qsort (its comparator ported), ceil(n / 2) of them added to the final beam (the engine's loop);
//   3. every portal room seen from outside through a portal that leads outdoors (CullNode of the room's entity, the portal polygon clipped through the final
//      beam into a fresh tree, the walk);
//   4. the sector tree query (FUN_18015c4a0 + OnQueryNodes + FUN_18066da10 ported: the walker's stack order, the leaf gate `flags & (db +0x15b0 | 6) ==
//      db +0x15ac`, CullCamera(sphere), a fully inside subtree extracted unculled).
// ExtractFromEntity (dedupe stamp entity +0xe0 == the global extract counter, contexts, flags 0x8026 / 8, CullNode unless the status is "inside") appends the
// element. Live: the element list (db +0x18 / +0x20), the stamps and the final beam (db +0x118: nodes, plane array) are written as the engine leaves them, the
// engine's function is not called; any doubt (a beam that would overflow the engine's 200 nodes, an occluder that is not an eCOccluder_PS, the extract
// filter callback, the occluder debug list, a fault) runs the engine's function instead (nothing written before). Check frames (`native.owned_check`) and
// `compare` run the port dry (stamps undone), then the engine's function, and compare: the element list (identities and order), the final beam (planes by
// value, nodes by shape) and every occluder's descs / indices (recorded from the engine's AddOccluder calls). A difference turns the stage off until the
// setting changes. rvk.log `native: own extract (5 s)`.

#include <cstdint>

namespace rvk::native::f10x {

// once at start (bootstrap, Vulkan mode): the detours (pass through while the setting is off); false when RVK_F10_COMPARE=1 holds the same function
bool install();
// once per present (engine thread): the 5 s report
void onPresent();

struct StageState {
    int setting = 0;          // native.own_extract
    int armed = 0;            // the mode the code runs (0 = off: not installed, tripped)
    std::uint64_t live = 0;   // views extracted natively (run)
    std::uint64_t compared = 0, differing = 0;
};
StageState stageState();

}  // namespace rvk::native::f10x
