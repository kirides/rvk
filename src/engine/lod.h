#pragma once

// Detail distances (docs/research/lod.md): player multipliers on the engine's level-of-detail and view distances.
//  * trees: SpeedTree LOD (full tree -> lower detail -> billboard). eCWraper_SpeedTreeRT::UpdateLoD and
//    GetBillboardFadeValues scale the tree's LOD limits by its factor (+0x30) for the computation: the factor is multiplied
//    for the call. The billboard meshes carry fade distances baked from the same limits (BLENDWEIGHT1.xy): the device scales
//    them in a vertex shader variant (billboardScale), so the billboard still fades in where the tree drops out.
//  * meshes: eCRenderContextMesh::UpdateLoD compares the camera distance with eCMesh_PS's LoD ranges: scaled for the call.
//  * view: the frustum database's view range factor (+0x15b4, multiplies every entity's ViewRange in CullNode /
//    CalculateFadeOut; the game leaves it at 1.0), written after each UpdateFromAppTemplate.
// 1.0 = the game's distances (the detours call straight through). Vulkan mode only (installed with the scene hooks).

namespace rvk::engine::lod {

struct Scales {
    float trees = 1.0f;
    float meshes = 1.0f;
    float view = 1.0f;
};

// any thread; trees / meshes apply at the next LOD update, view at the next UpdateFromAppTemplate (each frame)
void setScales(const Scales& s);
Scales scales();

// Creates the detours (enabled with the others by hooks::enableAll); false when a symbol is missing
bool install();

// Scales `ranges[0 .. count - 2]` (eCMesh_PS: the last LOD has no upper range) by `scale` and returns the originals in
// `saved` (same size, at most 8); restore() writes them back. Pure (unit-tested).
int scaleRanges(float* ranges, int count, float scale, float* saved);
void restoreRanges(float* ranges, int n, const float* saved);

}  // namespace rvk::engine::lod
