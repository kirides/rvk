#pragma once

// The sea floor field (docs/research/ocean.md 4): what the FFT ocean knows about the land and the water depth around the
// island. A top-down height raster of the engine's static geometry (the highest surface per texel: terrain, rocks, quays,
// ships, the ground under the water) around the coast, accumulated from the meshes the engine draws (enh::Water observes
// the main view's static mesh draws and rasterises their triangles; cached on disk under bin\rvk\cache\ocean), baked into:
//
//   A (RGBA16F): R signed distance to the waterline (world units, + over water, - on land), G water depth (sea level - the
//                highest surface; negative on land; unknown texels count as open, deep sea), BA the direction the waves
//                travel towards the shore (unit, times the coherence of the wave fronts: 0 where two fronts meet)
//   B (RGBA16F): R the travel time of a wave from here to the shore (s; the eikonal |grad T| = 1 / c(depth) with the
//                shallow-water-aware phase speed, solved by fast sweeping: the crests' spacing shrinks and the fronts turn
//                parallel to the depth contours as they approach the coast = shoaling + refraction), G exposure (0..1: how
//                open the water is to the sea; harbour basins and coves are low), B the distance to the engine's coast
//                overlay (the M7 shore field: beaches), A 1 where geometry was seen
//
// Pure CPU logic (unit tests: tests/ocean_tests.cpp). The signed distance uses the Felzenszwalb-Huttenlocher transform of
// enh/shore_field (after GD3D11's ShoreField.cpp, by the author of this project).

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "enh/shore_field.h"

namespace rvk::enh::seafloor {

inline constexpr float kUnknown = -1e30f;     // a texel no geometry covered
inline constexpr float kDeepDefault = 3000.0f;  // the depth of unknown texels (world units): the open sea
inline constexpr float kMaxTravel = 60.0f;      // s
inline constexpr float kRefPeriod = 8.0f;       // s: the wave period the travel time is computed for
inline constexpr float kMaxSigned = 30000.0f;

struct Grid {
    int width = 0, height = 0;
    float minX = 0.0f, minZ = 0.0f, cell = 1.0f;
    bool valid() const { return width > 0 && height > 0; }
    std::size_t count() const { return static_cast<std::size_t>(width) * static_cast<std::size_t>(height); }
    std::size_t index(int x, int z) const { return static_cast<std::size_t>(z) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x); }
    // xy corner, zw 1 / extent
    std::array<float, 4> mapping() const {
        return {minX, minZ, 1.0f / (static_cast<float>(width) * cell), 1.0f / (static_cast<float>(height) * cell)};
    }
    bool operator==(const Grid&) const = default;
};

// the grid around a box (xz) with a margin out to sea
Grid gridAround(float minX, float minZ, float maxX, float maxZ, float margin, int maxResolution, float minCell);

// The accumulated highest surface per texel (kUnknown where nothing was seen)
struct Heights {
    Grid grid;
    std::vector<float> top;
    void reset(const Grid& g);
    // rasterises world-space triangles top-down (texel centres inside + the corners), keeping the maximum
    void add(std::span<const shore::Triangle> tris);
    std::size_t known() const;
    // disk cache (bin\rvk\cache\ocean): the grid + the heights; load merges (max) when the grid matches
    bool save(const std::filesystem::path& file) const;
    bool load(const std::filesystem::path& file);
};

struct Field {
    Grid grid;
    std::vector<std::uint16_t> a, b;  // RGBA16F texels
    std::size_t landTexels = 0, waterTexels = 0, knownTexels = 0;
    float maxTravel = 0.0f;
    float msDistance = 0.0f, msTravel = 0.0f, msExposure = 0.0f, msPack = 0.0f;  // bake stages (diagnostics)
    bool valid() const { return grid.valid() && !a.empty(); }
    // decoded (tests, diagnostics): A then B
    std::array<float, 8> at(float x, float z) const;
};

// overlayDistance: the M7 shore field (the distance to the engine's coast overlay; may be invalid)
Field bake(const Heights& h, float seaLevel, const shore::Field& overlay);

// phase speed of a wave of period `period` (s) at depth (m): the dispersion w^2 = g k tanh(k d) solved for k (deep water ->
// g T / 2 pi, shallow -> sqrt(g d))
float phaseSpeed(float depthMetres, float period);

}  // namespace rvk::enh::seafloor
