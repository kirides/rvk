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
    // the swamp's own colour (c6 of Special_Water_Swamp_01) stays the in-water colour; it is murkier than the sea
    const float swampC6[4] = {0.149f, 0.1725f, 0.1176f, 0.06f};
    const enh::WaterProfile swamp = enh::waterProfile(Water::Swamp, swampC6);
    CHECK(swamp.scatter[0] == swampC6[0] && swamp.scatter[1] == swampC6[1] && swamp.scatter[2] == swampC6[2]);
    const float seaC6[4] = {0.035f, 0.035f, 0.059f, 0.1f};
    const enh::WaterProfile sea = enh::waterProfile(Water::Ocean, seaC6);
    CHECK(swamp.absorb[1] > 10.0f * sea.absorb[1]);
    CHECK(swamp.glint < sea.glint && swamp.reflectivity < sea.reflectivity);
    // only the sea has foam; foam is tinted by the water colour, never pure white
    CHECK(sea.foamStrength > 0.0f && swamp.foamStrength == 0.0f);
    CHECK(enh::waterProfile(Water::Lake, nullptr).foamStrength == 0.0f);
    CHECK(sea.foam[0] < 0.9f && sea.foam[2] <= 0.83f);
    // a constant that is not a colour falls back to the kind's default
    const float junk[4] = {7.0f, -1.0f, 0.0f, 0.0f};
    CHECK(enh::waterProfile(Water::River, junk).scatter[0] > 0.0f);
    // the player's tweaks scale it
    CHECK(enh::waterProfile(Water::Ocean, seaC6, 2.0f).absorb[0] > sea.absorb[0] * 1.9f);
    CHECK(enh::waterProfile(Water::Ocean, seaC6, 1.0f, 0.0f).glint == 0.0f);
}
