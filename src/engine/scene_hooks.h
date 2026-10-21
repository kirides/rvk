#pragma once

// Phase markers (docs/research/frame-phases.md, roadmap do-now items 1, 7, 10): detours on the engine functions that structure
// a frame drive d3d9::Device's phase tracker (view / pass), tag engine images with their roles and read the main view's
// camera from the engine. Draw tags (docs/research/draw-tags.md, do-now item 2): the render-context vtable slots and a few
// render functions set the device's current draws::Tag (source, context, entity, sub-mesh, material) around the original
// calls. Installed in Vulkan mode only; with no device attached every detour just calls the original.

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

}  // namespace rvk::engine::scene
