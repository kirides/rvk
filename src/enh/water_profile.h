#pragma once

// Water profiles (docs/research/water.md 6): what a body of water looks like in the enhanced pass. The look comes from the
// engine's own material first, so each body keeps its original art (the swamp at Don's camp stays murky olive, the sea
// clear and blue-grey): the material's colour constant (g_ShaderData c6.rgb, the diffuse colour every Risen water shader
// multiplies with its lighting) is the in-water scattering colour; the per-kind table below adds what the engine has no
// constant for (absorption with depth, roughness, ripples, foam) with values chosen to match the engine's own depth fade
// (ocean opaque after ~14 m of depth difference, swamp after ~1 m: c7 of their shaders). Player tweaks (water.murkiness,
// water.reflections_strength) scale the result. Pure logic (tests/water_tests.cpp).

#include <array>
#include <cstdint>

#include "d3d9/draws.h"

namespace rvk::enh {

struct WaterProfile {
    std::array<float, 3> scatter{};   // in-water scattering colour (albedo, lit by the engine's ambient + sun)
    std::array<float, 3> absorb{};    // extinction per world unit (cm) along the refracted path
    float reflectivity = 1.0f;        // scale of the Fresnel reflection
    float roughness = 0.1f;           // sun glint lobe (GGX alpha)
    float glint = 1.0f;               // sun glint strength
    float ripple = 0.5f;              // detail normal strength
    float rippleScale = 400.0f;       // world units per ripple tile
    float refraction = 1.0f;          // refraction distortion strength
    float turbidity = 0.2f;           // how much of the lit water colour lies over what is seen through it (suspended
                                      // particles: the engine's water always shows its colour, also in shallows)
    std::array<float, 3> foam{};      // foam tint (from the scattering colour: never pure white)
    float foamStrength = 0.0f;        // 0 = no foam (lakes, rivers, swamps never get shore foam)
};

// the profile of a body of water; materialColour = the draw's pixel constant c6 (rgba), null when not known; murkiness
// scales the absorption (1 = as designed), reflections scales reflectivity + glint
WaterProfile waterProfile(d3d9::draws::Water kind, const float* materialColour, float murkiness = 1.0f, float reflections = 1.0f);

}  // namespace rvk::enh
