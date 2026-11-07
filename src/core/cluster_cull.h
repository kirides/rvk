#pragma once

// F5-2 cluster cull, the pure part (docs/research/frame-ownership.md 7.34): the CPU reference of what shaders/gpuscene/owned_compact.comp.hlsl does per entry.
// A clustered draw's bounding spheres (object space) are tested against the six frustum planes of its view, made in object space from the view's clip rows
// (VS c0..c3: clip_i = dot(c_i, (world position, 1))) and the object's world rows (c33..c35) and widened by a margin (the camera's jitter is applied after the
// matrices, a few hundredths of a pixel of NDC): outside a plane when its distance is below -(r |n| + m (|w(c)| + r |row3.xyz|)); |w| over the sphere bounds the clip-space
// widening on both sides of the camera plane (a widening by m * w itself TIGHTENS a plane for a sphere that straddles it: a 12 km sphere behind the camera was culled in the game); the visible clusters become RUNS (maximal runs of consecutive visible clusters, at most kRunCap per entry, the
// rest merged into the last run: everything from its start to the last visible cluster).

#include <cstdint>

namespace rvk::clustercull {

inline constexpr float kMargin = 0.02f;           // in units of w: the planes are widened to x in [-(1 + m) w, (1 + m) w] ...
inline constexpr float kDepthMargin = 0.001f;   // near / far (jitter is XY only): z in [-m w, (1 + m) w]
inline constexpr std::uint32_t kRunCap = 8;       // runs (argument records) per culled entry
inline constexpr std::uint32_t kMaxRows = 4096;   // clusters an entry may have to be culled (the shader's visibility mask; larger = drawn whole)

struct View {
    float plane[6][4] = {};  // object space, un-normalised: inside = dot(n, p) + d >= 0 (left, right, bottom, top, near, far)
    float len[6] = {};       // |n|
    float row3[4] = {};      // the object-to-clip w row (xyz, constant): the margin is a fraction of |w| over the sphere, so it widens on either side of the camera plane
    float len3 = 0.0f;       // |row3.xyz|
    float margin[6] = {};    // per plane: the fraction (kMargin on the sides, kDepthMargin at the near plane)
    bool ok = false;         // every number finite
};

// regs = VS c0..c3, world = the three world rows (c33..c35) of 4; `depthPlanes` false = near / far left open (side planes only)
bool makeView(const float regs[4][4], const float world[12], float margin, bool depthPlanes, View& out);
// the same test as the shader: outside one plane by more than the radius (NaN = not outside)
bool sphereOutside(const View& v, const float c[3], float r);

struct Run {
    std::uint32_t first = 0, end = 0;  // clusters [first, end)
};
// `bits` = one bit per cluster (visible = 1), `rows` clusters; returns the runs (at most `cap`, the last one merged as above); 0 = nothing visible
std::uint32_t extractRuns(const std::uint32_t* bits, std::uint32_t rows, std::uint32_t cap, Run* out);

}  // namespace rvk::clustercull
