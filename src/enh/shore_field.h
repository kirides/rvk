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

#include <algorithm>
#include <array>
#include <cmath>
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

// a top-down world grid (also enh/sea_floor)
struct RasterGrid {
    int width = 0, height = 0;
    float minX = 0.0f, minZ = 0.0f, cell = 1.0f;
    std::size_t index(int x, int z) const { return static_cast<std::size_t>(z) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x); }
    std::size_t count() const { return static_cast<std::size_t>(width) * static_cast<std::size_t>(height); }
};

// Visits every texel centre inside the triangle's xz projection with the height there, plus the texels of its corners, so
// slivers and small triangles still leave a mark (GD3D11 ShoreField.cpp RasterizeTopDown)
template <typename Write>
void rasterizeTopDown(const RasterGrid& g, const Triangle& t, Write&& write) {
    const float ax = (t.a[0] - g.minX) / g.cell - 0.5f, az = (t.a[2] - g.minZ) / g.cell - 0.5f;
    const float bx = (t.b[0] - g.minX) / g.cell - 0.5f, bz = (t.b[2] - g.minZ) / g.cell - 0.5f;
    const float cx = (t.c[0] - g.minX) / g.cell - 0.5f, cz = (t.c[2] - g.minZ) / g.cell - 0.5f;
    const auto splat = [&](float x, float z, float y) {
        const int ix = static_cast<int>(std::lround(x));
        const int iz = static_cast<int>(std::lround(z));
        if (ix >= 0 && iz >= 0 && ix < g.width && iz < g.height) write(g.index(ix, iz), y);
    };
    splat(ax, az, t.a[1]);
    splat(bx, bz, t.b[1]);
    splat(cx, cz, t.c[1]);
    const float area = (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
    if (std::abs(area) < 1e-6f) return;
    const int x0 = std::max(0, static_cast<int>(std::ceil(std::min({ax, bx, cx}))));
    const int x1 = std::min(g.width - 1, static_cast<int>(std::floor(std::max({ax, bx, cx}))));
    const int z0 = std::max(0, static_cast<int>(std::ceil(std::min({az, bz, cz}))));
    const int z1 = std::min(g.height - 1, static_cast<int>(std::floor(std::max({az, bz, cz}))));
    const float invArea = 1.0f / area;
    for (int z = z0; z <= z1; ++z) {
        const float pz = static_cast<float>(z);
        for (int x = x0; x <= x1; ++x) {
            const float px = static_cast<float>(x);
            const float w0 = ((cx - bx) * (pz - bz) - (cz - bz) * (px - bx)) * invArea;
            const float w1 = ((ax - cx) * (pz - cz) - (az - cz) * (px - cx)) * invArea;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            write(g.index(x, z), w0 * t.a[1] + w1 * t.b[1] + w2 * t.c[1]);
        }
    }
}

inline constexpr float kFarSite = 1e20f;
// in place: 0 at sites, kFarSite elsewhere -> squared distance in texels to the nearest site (Felzenszwalb-Huttenlocher)
void squaredDistanceTransform(std::vector<float>& field, int width, int height);

inline constexpr float kMaxDistance = 30000.0f;
inline constexpr float kNoCoast = -30000.0f;

// The coast triangles -> the field. margin: how far the grid reaches past the coast (out to sea); maxResolution: texels of
// the longer side; minCell: the finest texel (world units).
Field bake(std::span<const Triangle> coast, float margin = 6000.0f, int maxResolution = 1024, float minCell = 48.0f);

std::uint16_t floatToHalf(float f);
float halfToFloat(std::uint16_t h);

}  // namespace rvk::enh::shore
