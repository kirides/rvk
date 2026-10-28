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

// true inside eCImageFilterScene::OnUpdateOutput (the scene job of the image effect: the world incl. its off-screen pre-passes,
// which run before Execute's Main view scope; N5.7 tells the post chain's filters from the terrain map's blur by it). Render thread.
bool inSceneJob();

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
    // N5.5: a shadow map's eCFrustumDatabase::ExecuteShadowMap (0x66f630) begins (inside its Shadow view scope, before the
    // original: its z list is not sorted yet) / ends
    virtual void shadowExecuteBegin(void* /*db*/, void* /*shadowMap*/) {}
    virtual void shadowExecuteEnd(void* /*db*/) {}
};
void setOpTap(OpTap* tap);

// N5.5c (native/effect_ops): the shadow mask's image filters and the terrain variance map's blur. Render thread; set / cleared on it.
class EffectTap {
public:
    virtual ~EffectTap() = default;
    // eCIlluminationAdmin::UpdateShadowMask (inside its ShadowMask pass marker) begins (before the original) / ends
    virtual void maskBegin(void* illum) = 0;
    virtual void maskEnd() = 0;
    // eCImageFilterEffect::ExecuteEffect (0x75af70) of any effect filter begins / ends
    virtual void effectBegin(void* filter) = 0;
    virtual void effectEnd(void* filter) = 0;
    // eCImageFilterCopy::OnUpdateOutput (0x665640) begins / ends
    virtual void copyBegin(void* filter) = 0;
    virtual void copyEnd(void* filter) = 0;
    // eCShadowMapBase::RenderShadowMap (0x5a1070: target, clear, UpdateCamera, the scene, reset) begins / ends; its
    // eCFrustumDatabase::ExecuteShadowMap (the scene, after UpdateCamera) begins / ends
    virtual void shadowMapBegin(void* map) = 0;
    virtual void shadowMapEnd(void* map) = 0;
    virtual void shadowSceneBegin(void* map) = 0;
    virtual void shadowSceneEnd(void* map) = 0;
    // N5.7: eCImageEffect::Execute (0x662630: the scene effect, then the overlay effect; inside the Post view scope) begins /
    // ends; eCSprite::Flush (0x757d10: the sprite batcher's queued records -> batches: GUI, fonts, cursor, videos) begins / ends
    virtual void postBegin(void* /*effect*/) {}
    virtual void postEnd(void* /*effect*/) {}
    virtual void spriteFlushBegin(void* /*sprite*/) {}
    virtual void spriteFlushEnd(void* /*sprite*/) {}
    // N5.8: eCImageFilterShadowMask::UpdatePackedShadowMapVisual (0x669e00) returned `ok` for the packed map `map` (the PSSM pass of
    // that map follows: BeginEffectTechnique, per cell 3 passes, EndEffectTechnique)
    virtual void packedVisual(void* /*mask*/, void* /*map*/, bool /*ok*/) {}
};
void setEffectTap(EffectTap* tap);

// Makes the entries of slot `slot` (0..3) of a render-context vtable reach the op tap and the draw tag's op: a slot that is
// already ours stays; any other is replaced by a pass-through thunk (only the tag's op around the original). Returns false
// when the slot cannot be patched (pool full, unreadable). `original` receives the engine's function of the slot.
bool ensureOpSlot(const void* vtable, int slot, const void** original);

// The water classification and material name eCSubMesh::Render's draw tag gets for `material` in the current phase (only in
// the Refracted / Alpha / AlphaLate passes of non-shadow views; else None / null): the N5.2 scene ops compose it for their
// own draws so the enhanced water's DrawReplacer acts on them as on the engine's. Render thread.
void subMeshWater(const void* material, const void* meshResource, d3d9::draws::Water& water, const char*& name);

}  // namespace rvk::engine::scene
