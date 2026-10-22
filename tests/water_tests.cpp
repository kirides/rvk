// Enhanced water: the shore field bake (enh/shore_field.h) and the water profiles (enh/water_profile.h).

#include <cmath>
#include <string>
#include <vector>

#include "enh/shore_field.h"
#include "enh/water_profile.h"
#include "test.h"

using namespace rvk;

TEST(water_half_float_round_trip) {
    for (float f : {0.0f, 1.0f, -2.5f, 1234.0f, 30000.0f, 0.001f, -30000.0f}) {
        const float r = enh::shore::halfToFloat(enh::shore::floatToHalf(f));
        CHECK(std::abs(r - f) <= std::abs(f) * 1e-3f + 1e-4f);
    }
    CHECK(enh::shore::halfToFloat(enh::shore::floatToHalf(1e9f)) == 65504.0f);  // clamped to the largest finite half
}

// a straight coast strip along x = 0 (z from -5000 to 5000, 200 wide): the distance grows away from it on both sides, the
// direction points towards it
TEST(water_shore_field_distance_and_direction) {
    std::vector<enh::shore::Triangle> strip;
    strip.push_back({{-100.0f, 0.0f, -5000.0f}, {100.0f, 10.0f, -5000.0f}, {100.0f, 10.0f, 5000.0f}});
    strip.push_back({{-100.0f, 0.0f, -5000.0f}, {100.0f, 10.0f, 5000.0f}, {-100.0f, 0.0f, 5000.0f}});
    const enh::shore::Field f = enh::shore::bake(strip, 4000.0f, 256, 32.0f);
    CHECK(f.valid());
    CHECK(f.coastTexels > 0);
    const auto on = f.at(0.0f, 0.0f);
    CHECK(on[0] < 1.0f);
    CHECK(on[1] > -1.0f && on[1] < 11.0f);
    const auto east = f.at(2000.0f, 0.0f);
    CHECK(std::abs(east[0] - 1900.0f) < 2.0f * f.cell);
    CHECK(east[2] < -0.5f);  // towards the coast = -x
    const auto west = f.at(-3000.0f, 0.0f);
    CHECK(std::abs(west[0] - 2900.0f) < 2.0f * f.cell);
    CHECK(west[2] > 0.5f);
    const auto outside = f.at(1e6f, 0.0f);
    CHECK(outside[0] == enh::shore::kMaxDistance);
    const auto m = f.mapping();
    CHECK(m[0] == f.minX && m[2] > 0.0f);
    CHECK(!enh::shore::bake({}).valid());
}

TEST(water_profiles_follow_the_material) {
    using d3d9::draws::Water;
    // only the open sea gets foam and waves rolling in; the swamp (its shader reflects nothing) a dim reflection
    const enh::WaterProfile sea = enh::waterProfile(Water::Ocean);
    const enh::WaterProfile swamp = enh::waterProfile(Water::Swamp);
    CHECK(sea.foam > 0.0f && sea.shoreWaves > 0.0f);
    CHECK(swamp.foam == 0.0f && swamp.shoreWaves == 0.0f);
    CHECK(enh::waterProfile(Water::Lake).foam == 0.0f && enh::waterProfile(Water::River).shoreWaves == 0.0f);
    CHECK(swamp.extraReflection > 0.0f && swamp.extraReflection < 1.0f && sea.extraReflection == 0.0f);
    // the player's tweaks scale it
    CHECK(enh::waterProfile(Water::Ocean, 2.0f).murkiness == 2.0f);
    CHECK(enh::waterProfile(Water::Swamp, 1.0f, 0.0f).extraReflection == 0.0f);
    CHECK(enh::waterProfile(Water::Ocean, 1.0f, 1.0f, 0.0f).shoreWaves == 0.0f);
}

// the engine's water shaders are told apart by their sampler layout (docs/research/water.md 2.3); anything else is not
// shaded by the enhanced water
TEST(water_engine_models_from_the_sampler_layout) {
    using shader::TextureType;
    constexpr TextureType T = TextureType::Tex2D, C = TextureType::Cube, U = TextureType::Unknown;
    const TextureType ocean[16] = {T, T, T, U, U, U, U, U, U, T, T, T, C, T, T, U};
    const TextureType lake[16] = {U, T, T, U, U, U, U, U, U, C, T, T, T, T, T, U};
    const TextureType swamp[16] = {U, T, T, U, U, U, U, U, U, T, T, U, U, U, U, U};
    const TextureType river[16] = {U, T, T, U, U, U, U, U, U, T, T, T, T, C, T, T};
    const TextureType crystal[16] = {U, T, T, U, U, U, U, U, U, T, T, T, U, U, U, U};
    CHECK(enh::engineWaterModel(ocean) == enh::WaterModel::Ocean);
    CHECK(enh::engineWaterModel(lake) == enh::WaterModel::Lake);
    const TextureType caveLake[16] = {T, T, T, U, U, U, U, U, U, C, T, T, T, T, T, U};  // + point lights (the crystal cave)
    CHECK(enh::engineWaterModel(caveLake) == enh::WaterModel::Lake);
    CHECK(enh::engineWaterModel(swamp) == enh::WaterModel::Swamp);
    CHECK(enh::engineWaterModel(river) == enh::WaterModel::River);
    CHECK(enh::engineWaterModel(crystal) == enh::WaterModel::None);
    CHECK(enh::engineWaterModel(nullptr) == enh::WaterModel::None);
}

TEST(water_affine_fit_of_the_sea_mapping) {
    // v = 0.002 x - 0.0005 z + 3 at points far from the origin (the sea plane's corners are ~3e5 away)
    const float x[5] = {-336000.0f, 336000.0f, 336000.0f, -336000.0f, 1000.0f};
    const float z[5] = {-286000.0f, -286000.0f, 286000.0f, 286000.0f, -5000.0f};
    float v[5];
    for (int i = 0; i < 5; ++i) v[i] = 0.002f * x[i] - 0.0005f * z[i] + 3.0f;
    float m[3], err = 1.0f;
    CHECK(enh::fitAffineXZ(x, z, v, 5, m, &err));
    CHECK(std::abs(m[0] - 0.002f) < 1e-6f && std::abs(m[1] + 0.0005f) < 1e-6f && std::abs(m[2] - 3.0f) < 1e-2f);
    CHECK(err < 1e-2f);
    const float lx[3] = {0.0f, 1.0f, 2.0f}, lz[3] = {0.0f, 1.0f, 2.0f}, lv[3] = {0.0f, 1.0f, 2.0f};
    CHECK(!enh::fitAffineXZ(lx, lz, lv, 3, m));  // collinear
}
