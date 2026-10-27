#pragma once

// Phase markers (docs/research/frame-phases.md, roadmap do-now items 1, 7, 10): detours on the engine functions that structure
// a frame drive d3d9::Device's phase tracker (view / pass), tag engine images with their roles and read the main view's
// camera from the engine. Draw tags (docs/research/draw-tags.md, do-now item 2): the render-context vtable slots and a few
// render functions set the device's current draws::Tag (source, context, entity, sub-mesh, material) around the original
// calls. Installed in Vulkan mode only; with no device attached every detour just calls the original.

#include <cstdint>

#include "d3d9/draws.h"

namespace rvk::d3d9 {
class Device;
}

namespace rvk::engine::scene {

// Creates the MinHook detours (enabled with the others by hooks::enableAll). DllMain: no engine code runs here.
// Returns the number of detours created.
int install();

// The device the markers go to (HalDevice::create / ~HalDevice). The first attach also patches the render-context vtable
// slots (Render / RenderAlpha / RenderRefracted / RenderZPass of the mesh, animation, SpeedTree, SpeedTree billboard,
// vegetation and debug contexts; several are tail-jump thunks, not detoured).
void attach(d3d9::Device* device);
void detach(d3d9::Device* device);

// eCApplication::Draw is detoured by bootstrap (shared with [debug] snapshot_frames): the Gui view around the original
void appDrawEnter();
void appDrawLeave();

// "Main/Solid": the current view and pass of the attached device (for log prefixes); "-" without one
const char* currentLabel();

// the attached device when called on its render thread, else null (engine detours that need the frame state)
d3d9::Device* renderDevice();

// The N5.0 op tap (native/frame_tap.*): told about every main-view eCFrustumDatabase::Execute (before and after the original)
// and every render-context slot entry with the eCRenderOp2* the engine passes. Render thread; set / cleared on it.
class OpTap {
public:
    virtual ~OpTap() = default;
    virtual void executeBegin(void* db, void* desc) = 0;
    virtual void executeEnd(void* db) = 0;
    virtual void opEnter(const void* op) = 0;
    // a main-view non-op path of Execute begins / ends (eCDecalAdmin::RenderDecals: admin, db; eCBillboardAdmin::
    // RenderBillboards: admin, no db; inside its draw tag)
    // eCSkydome_PS::Render of a sky cube face (outside Execute: the property set, no db)
    enum class NonOp : std::uint8_t { Decals, Billboards, SkyCube };
    virtual void nonOpBegin(NonOp /*which*/, void* /*admin*/, void* /*db*/) {}
    virtual void nonOpEnd(NonOp /*which*/) {}
};
void setOpTap(OpTap* tap);

// Makes the entries of slot `slot` (0..3) of a render-context vtable reach the op tap and the draw tag's op: a slot that is
// already ours stays; any other is replaced by a pass-through thunk (only the tag's op around the original). Returns false
// when the slot cannot be patched (pool full, unreadable). `original` receives the engine's function of the slot.
bool ensureOpSlot(const void* vtable, int slot, const void** original);

// The water classification and material name eCSubMesh::Render's draw tag gets for `material` in the current phase (only in
// the Refracted / Alpha / AlphaLate passes of non-shadow views; else None / null): the N5.2 scene ops compose it for their
// own draws so the enhanced water's DrawReplacer acts on them as on the engine's. Render thread.
void subMeshWater(const void* material, const void* meshResource, d3d9::draws::Water& water, const char*& name);

}  // namespace rvk::engine::scene
