#pragma once

// The sea's shore field (docs/research/water.md 5): a top-down world-space grid around the island that tells the enhanced
// ocean where the coast is. Risen marks its own sea coast: the "fake shore" overlay meshes (Levelmesh_Ocean_Shore_01/02,
// materials Special_Water_Shore_*: scrolling foam strips laid along every sea beach, and only there; lakes, rivers and
// swamps have none). Their triangles are rasterised top-down into the grid as the coast; a distance transform gives every
// texel its distance to the nearest coast texel, and the blurred distance's gradient the direction towards the coast.
//
// Per texel (RGBA16F): R distance to the coast in world units (0 on the overlay, capped), G the overlay's height there (or
// -30000 off the coast), BA the unit direction towards the coast (xz), shorter where two coasts meet.
//
// Adapted from GD3D11's ShoreField.cpp (D3D11Engine/ShoreField.cpp, by kirides, MIT here with the author's permission):
// the top-down rasteriser, the Felzenszwalb-Huttenlocher distance transform and the blurred-gradient direction. Pure CPU
// logic, no Vulkan, no engine access (unit tests: tests/water_tests.cpp); bake() runs on a worker thread.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace rvk::enh::shore {

struct Triangle {
    float a[3], b[3], c[3];  // world space (y up)
};

struct Field {
    int width = 0, height = 0;
    float minX = 0.0f, minZ = 0.0f;  // world xz of the grid's corner
    float cell = 1.0f;               // world units per texel
    std::vector<std::uint16_t> texels;  // RGBA16F, row-major (z rows)
    std::size_t coastTexels = 0;
    bool valid() const { return width > 0 && height > 0 && !texels.empty(); }
    // xy = corner, zw = 1 / extent: uv = (xz - xy) * zw
    std::array<float, 4> mapping() const {
        return {minX, minZ, 1.0f / (static_cast<float>(width) * cell), 1.0f / (static_cast<float>(height) * cell)};
    }
    // decoded texel (tests, diagnostics): {distance, overlay height, dir x, dir z}
    std::array<float, 4> at(float x, float z) const;
};

inline constexpr float kMaxDistance = 30000.0f;
inline constexpr float kNoCoast = -30000.0f;

// The coast triangles -> the field. margin: how far the grid reaches past the coast (out to sea); maxResolution: texels of
// the longer side; minCell: the finest texel (world units).
Field bake(std::span<const Triangle> coast, float margin = 6000.0f, int maxResolution = 1024, float minCell = 48.0f);

std::uint16_t floatToHalf(float f);
float halfToFloat(std::uint16_t h);

}  // namespace rvk::enh::shore
