#pragma once

// The engine's sun cascades (eCCascadeShadowMap, docs/research/shadows.md 3): player overrides of the cascade map size, the
// shadow range (MaxDistance) and the split factor (LogSplitFactor), applied in a pre-hook of RenderShadowMaps, which fits and
// renders every cascade each frame. The game's own values (its Shadow Quality: High = 1024, 75 m, 0.4) are tracked, so
// "game" restores them and a changed quality option is followed. Vulkan mode only (installed with the scene hooks).

namespace rvk::engine::shadowmaps {

struct Overrides {
    int mapSize = 0;            // texels per side, 0 = the game's
    float range = 0.0f;         // world units (cm), 0 = the game's
    float splitFactor = -1.0f;  // 0 (logarithmic) .. 1 (uniform) splits, < 0 = the game's
    int fit = 0;                // the cascade fit (shadows.fit): 0 = the engine's (bounding sphere merged from the slice's corners), 1 = the minimal sphere
};

// any thread; applies at the next RenderShadowMaps
void setOverrides(const Overrides& o);

// The cascades are drawn every frame while on (the RT shadows: the characters' cascade draws are where their pose of this frame is
// captured before the main view, rt-shadows.md 7); false = the stride below decides. Any thread
void setEveryFrame(bool on);

// the cascades are redrawn every n-th frame (1 = every frame) while the camera moves / turns little; any thread
void setUpdateStride(int stride);

// The light direction the cascade object holds (+0x10, as stored: the direction the light travels) at the last RenderShadowMaps
// call; false before the first call (and in interiors without sun shadows, where it is not called). Any thread
bool sunVector(float out[3]);

// The sun's TRUE direction (eCDirectionalLight_PS +0x80, the direction the light travels), read each frame in a pre-hook of
// eCDirectionalLight_PS::UpdateShadowMaps; the cascade object's vector (sunVector) is a copy that follows only after the sun
// turned ~0.5 degree, so anything that wants the sun to move smoothly (RT shadows) reads this one; false before the first call
bool trueSunVector(float out[3]);

// The eCCascadeShadowMap of the last RenderShadowMaps call (null before the first one): its split array (+0x30, 0x168 bytes per
// split, count +0x38) names a cascade view's index (native/frame_tap, N5.5). Render thread
const void* cascadeObject();

// N5.5c (native/effect_ops, group 2048): called on the render thread right before the engine's RenderShadowMaps runs the fit and
// renders the cascades (after the overrides above were applied), with the eCCascadeShadowMap; null = none
void setFitTap(void (*tap)(void* cascadeObject));

// shadows.md 8: the minimal sphere of a view-space frustum slice (symmetric projection: half extents tanX / tanY per unit of depth,
// depths n .. f). The centre is on the view axis at depth centreZ, radius the farthest corner's distance (GD3D11's fit); pure
struct SliceSphere {
    float centreZ = 0.0f;
    float radius = 0.0f;
};
SliceSphere minimalSphere(float tanX, float tanY, float n, float f);

// true while the cascade fit is not the engine's (the UpdateCamera pre-hook rewrote the light box): checks that recompute the
// engine's camera from its own fit stand down. Any thread
bool fitOverridden();

// The last fit per cascade (render thread / log): the engine's box side and ours, cm
struct FitInfo {
    float engineSide = 0.0f, side = 0.0f;
    int size = 0;
};
FitInfo lastFit(int cascade);

// Creates the detour (enabled with the others by hooks::enableAll); false when the symbols are missing
bool install();

// The split distances of the engine's formula (eCCascadeShadowMap::RenderShadowMaps, shadows.md 3.1): split i of n starts
// at (lin - log) * factor + log with lin = (range - 1) * i / n + 1, log = range ^ (i / n). Pure (unit-tested).
float splitStart(float range, float factor, int i, int n);

}  // namespace rvk::engine::shadowmaps
