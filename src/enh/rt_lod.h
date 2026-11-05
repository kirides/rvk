#pragma once

// Level of detail of the ray tracing scene (docs/research/rt-lod.md): pure logic, unit tests in tests/rt_tests.cpp.
// Beyond `rt.lod_range` an instance is traced through a coarser proxy of its BLAS: the same vertex buffer, another (shorter)
// index list built by meshoptimizer's edge-collapse simplifier on a worker thread (collapses go to existing vertices, so the
// geometry records' vertex layout, the attribute fetch and the materials stay as they are).

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rvk::enh::rt::lod {

constexpr std::size_t kBands = 5;
constexpr std::array<float, kBands - 1> kBandEdgesMetres = {20.0f, 50.0f, 100.0f, 200.0f};  // 0-20, 20-50, 50-100, 100-200, 200+
std::size_t bandOf(float distanceMetres);
const char* bandName(std::size_t band);

// target index-count fraction of a level: 0 = off, 1 = 1/2, 2 = 1/4, 3 = 1/8 of the triangles
float targetRatio(int bias);

// The ladder: rung 0 = the engine's draw (full detail), 1 = the simplified proxy (rt.lod_range), 2 = the shared box (rt.lod_far_range).
// A rung is entered beyond its range and left again below `range * (1 - hysteresis)` (the distance is the camera to the
// nearest point of the instance's bounding sphere, world units); a range of 0 disables its rung (rung 2 can follow rung 0 directly).
int rungFor(int current, float distanceUnits, float proxyRange, float boxRange, float hysteresis = 0.15f);
// an object this small on screen (bounding radius / distance) is left out of the scene on rung 2
bool tooSmall(float radius, float distanceUnits);
constexpr float kTinyAngle = 0.003f;

// the decimated index list of a triangle list: `positions` = float3 at `strideBytes`, `indices` index them (a tight window of the
// draw's vertices). Returns indices into the same vertices (a subset of the input's), at most about `ratio` of the triangles;
// the result is empty when the simplifier cannot reach 0.9 x ratio... (the caller keeps the original then). `errorFraction`:
// the allowed deviation relative to the mesh's extent.
struct Result {
    std::vector<std::uint32_t> indices;
    float error = 0.0f;  // the simplifier's relative error of the result
};
Result simplify(const float* positions, std::size_t vertexCount, std::size_t strideBytes, const std::vector<std::uint32_t>& indices, float ratio,
                float errorFraction);

}  // namespace rvk::enh::rt::lod
