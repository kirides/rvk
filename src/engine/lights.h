#pragma once

// The world's lights (eCIlluminationAdmin, docs/research/rtgi.md 19): every static light the cached-in world holds (the
// admin's sphere tree at +0x68) and every registered dynamic light (the array at +0x38), not only the <= 4 a drawn object
// carries. The GI uses them so lights beyond the engine's per-object reach still light the traced scene. The admin is
// found through the `this` of two of its calls (hooks, Vulkan mode only); the walk runs on the engine's thread with
// SEH-guarded reads.

#include <cstdint>
#include <vector>

namespace rvk::engine::lights {

struct WorldLight {
    float pos[3]{};
    float range = 0.0f;      // the light's radius (the engine's falloff 1 - d^2 / R^2 reaches 0 there)
    float col[3]{};          // diffuse colour x intensity, as the engine hands it to the shaders (c26 + 4 i)
    bool dynamic = false;    // eCPointLight_PS (flicker, spells, torches carried by the world), else a static light
};

struct Stats {
    std::uint32_t nodes = 0;       // tree nodes visited
    std::uint32_t staticTotal = 0; // static lights in the tree
    std::uint32_t staticNear = 0;  // ... within the query
    std::uint32_t dynamicRaw = 0;   // the admin's registered dynamic lights of any class
    std::uint32_t dynamicTotal = 0;
    std::uint32_t dynamicNear = 0;
    std::uint32_t unknown = 0;     // payloads of an unknown class
    std::uintptr_t dynamicVt[4]{};  // vtables of the first registered dynamic lights (diagnostics)
    bool adminSeen = false;
};

// Appends the lights whose sphere reaches within `radius` of `center` to `out` (cleared first); false while the admin is
// not known yet. Cheap enough per frame (the tree walk is pruned by the spheres)
bool collect(const float center[3], float radius, std::vector<WorldLight>& out, Stats& stats);

// Creates the detours that find the admin; false when the symbols are missing
bool install();

}  // namespace rvk::engine::lights
