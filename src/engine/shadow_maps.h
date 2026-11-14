#pragma once

#include <cstdint>

// The engine's sun cascades (eCCascadeShadowMap, docs/research/shadows.md 3): player overrides of the cascade map size, the
// shadow range (MaxDistance) and the split factor (LogSplitFactor), applied in a pre-hook of RenderShadowMaps, which fits and
// renders every cascade each frame. The game's own values (its Shadow Quality: High = 1024, 75 m, 0.4) are tracked, so
// "game" restores them and a changed quality option is followed. Vulkan mode only (installed with the scene hooks).

namespace rvk::engine::shadowmaps {

struct Overrides {
    int mapSize = 0;            // texels per side, 0 = the game's
    float range = 0.0f;         // world units (cm), 0 = the game's
    float splitFactor = -1.0f;  // 0 (logarithmic) .. 1 (uniform) splits, < 0 = the game's
    int fit = 0;                // the cascade fit (shadows.fit): 0 = the engine's (bounding sphere merged from the slice's corners), 1 = the minimal sphere,
                                // 2 = sdsm (the visible pixels' bounds, sphere where there is no usable reduction)
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
    float sideY = 0.0f;   // the box's y side (sdsm: not square)
    bool sdsm = false;    // the last fit of this cascade came from the reduction (false: the sphere's)
};
FitInfo lastFit(int cascade);

// ---- SDSM (shadows.fit = sdsm; shadows.md 8.9) ----
// enh/sdsm reduces the main view's depth into SdsmFrame::kBins view-depth bins (log spaced zMin .. zMax) holding the light-space x / y bounds of the
// visible pixels; the frame arrives one or two frames late (a read-back that completed) through submitSdsm. The UpdateCamera pre-hook merges the bins
// of a cascade's slice, widens them by the camera's motion since the reduction and a safety margin, quantises the extent (steps, shrink only after a
// calm window) and snaps the centre to whole texels. Everything below the hook is pure (unit-tested).
struct SdsmFrame {
    static constexpr int kBins = 128;
    std::uint64_t counter = 0;  // renderCounter() of the frame the reduction ran in
    double time = 0.0;          // the device's seconds at that frame
    float zMin = 10.0f, zMax = 10000.0f;  // bin i covers zMin * r^i .. zMin * r^(i+1), r = (zMax / zMin)^(1 / kBins)
    float tanX = 0.0f, tanY = 0.0f;       // the projection's half extents per unit depth
    float lightA[9]{};          // the light view's inverse (world -> light): l = w * A + t, A row-major 3x3
    float lightT[3]{};
    float pos[3]{};             // camera position and axes (inverse view rows 0..2) at the reduction
    float axes[9]{};
    float bounds[kBins][4]{};   // per bin: min x, max x, min y, max y (light space of that frame); min > max = empty bin
};
// render thread: the newest completed reduction (older than the one held is dropped)
void submitSdsm(const SdsmFrame& frame);
// enh/sdsm's poll, called at the top of every cascade update while the fit is sdsm (collects completed read-backs, calls submitSdsm); null = none
void setSdsmPoll(void (*poll)());
// The numbers enh/sdsm needs to record a reduction (render thread, valid after the cascades were updated this frame): the light view's inverse
// rows (world -> light, 3x4: A row-major + t), the shadow range, the counter (increments at every cascade update). False before the first update
struct SdsmSetup {
    float lightA[9]{}, lightT[3]{};
    float range = 0.0f;
    std::uint64_t counter = 0;
};
bool sdsmSetup(SdsmSetup& out);
// true while the fit is sdsm (enh/sdsm records its pass only then)
bool sdsmWanted();

namespace sdsm {
struct Bounds {
    float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;
    float zLo = 0.0f, zHi = 0.0f;  // the depth range of the bins merged (their edges)
    bool any = false;
};
float binEdge(const SdsmFrame& f, int i);  // lower edge of bin i (i = kBins: zMax)
// the bounds of the visible pixels whose view depth lies in [n, f] (whole bins that overlap: conservative), any = false when none
Bounds merge(const SdsmFrame& frame, float n, float f);
// the smallest value of the grid 2^(k / 16) not below v (v > 0): the extent steps (4.4 %)
float quantUp(float v);
// the extent of one box axis: grows at once to the grid value above `need`, shrinks to the grid value above the largest need of the last `calm` frames
struct Axis {
    float cur = 0.0f, peak = 0.0f;
    int calm = 0;
};
float step(Axis& a, float need, int calmFrames);
// c snapped to a whole texel
float snap(float c, float texel);
// the widening of a box for motion: the camera moved `translation` cm and turned `angle` rad since the reduction (the slice far end swings with the
// angle), plus a fixed margin and one proportional to the slice
float margin(float translation, float angle, float farZ, float fixedCm, float relative);
// the largest angle between corresponding camera axes (rows of two row-major 3x3 axis sets), rad
float axesAngle(const float a[9], const float b[9]);
// The whole fit of one cascade from plain inputs (no engine access, no globals: written to be ported 1:1 to a compute shader; shadows.md 8.9): the reduction, the
// slice, the current light view's inverse and camera, the sphere box (fallback and cap) and the per-axis extent state. False = keep the sphere's box (out.fail)
inline constexpr float kMinAspect = 0.5f;  // a box side is at least this fraction of the other (texels stay within 2:1)
enum class FitFail : std::uint8_t { None, Sun, Fov, Motion, Empty };
struct FitIn {
    const SdsmFrame* data = nullptr;
    float n = 0.0f, f = 0.0f;                  // the slice's view depths
    float lightA[9]{}, lightT[3]{};            // the current light view's inverse (world -> light)
    float pos[3]{}, axes[9]{}, tanX = 0.0f, tanY = 0.0f;  // the current camera
    int size = 0;                              // the cascade map's texels per side
    float sphereLo[2]{}, sphereHi[2]{};        // the minimal sphere's x / y box
    float driftCm = 0.0f, driftRad = 0.0f;     // the camera's allowed drift while the cascades are not redrawn (the throttle)
};
struct FitOut {
    float lo[2]{}, hi[2]{};                    // the box's x / y
    FitFail fail = FitFail::None;
    float margin = 0.0f, dp = 0.0f, angle = 0.0f;
};
bool fit(const FitIn& in, Axis ext[2], int calmFrames, FitOut& out);
}  // namespace sdsm

// Creates the detour (enabled with the others by hooks::enableAll); false when the symbols are missing
bool install();

// The split distances of the engine's formula (eCCascadeShadowMap::RenderShadowMaps, shadows.md 3.1): split i of n starts
// at (lin - log) * factor + log with lin = (range - 1) * i / n + 1, log = range ^ (i / n). Pure (unit-tested).
float splitStart(float range, float factor, int i, int n);

}  // namespace rvk::engine::shadowmaps
