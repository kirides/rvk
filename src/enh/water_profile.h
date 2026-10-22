#pragma once

// Water profiles (docs/research/water.md 4.4): what the enhanced pass adds to a body of water. The look itself is the
// engine's: each Risen water material has its own pixel shader (a ge_default variant), and water.frag evaluates that
// shader's model with the material's own textures and constants (engineWaterModel picks the model from the shader's
// sampler layout; an unknown layout keeps the engine's draw). The profile only scales what is added on top: the traced
// reflections, a dim reflection for the swamp (whose shader reflects nothing), foam and shore waves for the open sea, the
// player's murkiness (the engine's depth fade rate, the sea's tint). Pure logic (tests/water_tests.cpp).

#include <array>
#include <cstddef>
#include <cstdint>

#include "d3d9/draws.h"
#include "shader/sm_ir.h"

namespace rvk::enh {

// the engine's water pixel shaders (shaders/water/water_common.glsl WATER_MODEL_*)
enum class WaterModel : std::uint32_t {
    None = 0,   // not a known water shader: the engine draws it
    Ocean = 1,  // Special_Water_Ocean (ps 6e22a01a6cf0ac0a): s0 shadow mask, s9 blend mask, s10 / s11 diffuse, s12 sky cube, s13 / s14 normal maps
    Lake = 2,   // Special_Water_Island (ps 912d3cc09d934354): s9 sky cube, s10 / s11 reflection masks, s12..s14 normal maps
    Swamp = 3,  // Special_Water_Swamp (ps 2cd5ceec7eb11593): s9 / s10 normal maps
    River = 4,  // Special_Water_River (ps 11b69f874dfbacdb): s9 reflection mask, s10..s12 whitewater, s13 sky cube, s14 / s15 normal maps
};

// the model of a pixel shader from the texture types of its samplers s0..s15 (s1 = refraction copy, s2 = depth map in all)
WaterModel engineWaterModel(const shader::TextureType* samplers16);
const char* name(WaterModel m);

struct WaterProfile {
    float reflection = 1.0f;       // scale of the engine's reflection term (its Fresnel), which the traced reflection feeds
    float extraReflection = 0.0f;  // a reflection for a body whose shader has none (Schlick Fresnel x this): the swamp, dim
    float foam = 0.0f;             // foam strength (the open sea only: lakes, rivers, swamps never get shore foam)
    float shoreWaves = 0.0f;       // height of the waves rolling in towards the coast (world units; the open sea only)
    float murkiness = 1.0f;        // x the engine's depth fade rate, x the sea's tint
};

// the profile of a body of water; murkiness / reflections = the player's water.murkiness / water.reflection_strength,
// waveHeight = water.wave_height
WaterProfile waterProfile(d3d9::draws::Water kind, float murkiness = 1.0f, float reflections = 1.0f, float waveHeight = 1.0f);

// least-squares affine map (x, z) -> v = out[0] x + out[1] z + out[2] over n points (the sea mesh's texture coordinates as
// a function of its world position, for the wave grid); false when degenerate (fewer than 3 points, collinear); maxError =
// the largest residual
bool fitAffineXZ(const float* x, const float* z, const float* v, std::size_t n, float out[3], float* maxError = nullptr);

}  // namespace rvk::enh
