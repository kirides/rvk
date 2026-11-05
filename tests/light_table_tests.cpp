// The Full-owned light table (src/enh/light_table.*, docs/research/gpu-scene.md 9.7): the cluster math shared with shaders/include/rvk_light_model.hlsli.

#include <cmath>

#include "enh/light_table.h"
#include "test.h"

using namespace rvk;

TEST(light_table_slices_cover_the_range_exponentially) {
    using namespace enh;
    const float s = lightSliceScale(kLightNear, kLightFar, kLightGridZ);
    CHECK(lightSlice(1.0f, kLightNear, s, kLightGridZ) == 0);                       // before the near plane: slice 0
    CHECK(lightSlice(kLightNear, kLightNear, s, kLightGridZ) == 0);
    CHECK(lightSlice(kLightFar * 2.0f, kLightNear, s, kLightGridZ) == kLightGridZ - 1);  // beyond the far: the last slice
    std::uint32_t prev = 0;
    for (float z = kLightNear; z < kLightFar; z *= 1.07f) {
        const std::uint32_t k = lightSlice(z, kLightNear, s, kLightGridZ);
        CHECK(k >= prev && k < kLightGridZ);
        prev = k;
    }
    // a slice begins where the shader's box construction says (near x exp(k / s)) and the slice index of that depth is k
    for (std::uint32_t k = 0; k < kLightGridZ; ++k) {
        const float z0 = lightSliceNear(k, kLightNear, s) * 1.001f;
        CHECK(lightSlice(z0, kLightNear, s, kLightGridZ) == k);
    }
    CHECK(std::abs(lightSliceNear(kLightGridZ, kLightNear, s) - kLightFar) < kLightFar * 1e-4f);
}

TEST(light_table_physical_ratio_keeps_the_engine_brightness_at_035_range) {
    using namespace enh;
    const float R = 800.0f, rcp = 1.0f / (R * R);
    const float d = 0.35f * R;
    // phys(0.35 R) == orig(0.35 R): the ratio is 1 there, hotter close to the lamp, gentler far away, 0 at the reach
    CHECK(std::abs(lightPhysicalRatio(d * d, rcp) - 1.0f) < 2e-3f);
    CHECK(lightPhysicalRatio(100.0f * 100.0f, rcp) > 1.5f);
    CHECK(lightPhysicalRatio(0.8f * R * 0.8f * R, rcp) < 0.5f);
    CHECK(lightPhysicalRatio(R * R, rcp) == 0.0f);
    CHECK(lightPhysicalRatio(4.0f * R * R, rcp) == 0.0f);
    // the engine's own falloff
    CHECK(std::abs(lightEngineFalloff(0.0f, rcp) - 1.0f) < 1e-6f && lightEngineFalloff(R * R, rcp) == 0.0f);
    CHECK(std::abs(lightEngineFalloff(d * d, rcp) - (1.0f - 0.1225f)) < 1e-5f);
}

// the far level continues the near grid's slices without a gap and covers the world
TEST(light_table_far_level_continues_the_slices) {
    using namespace enh;
    const float s1 = lightSliceScale(kLightFar, kLightFar1, kLightGrid1Z);
    CHECK(lightSlice(kLightFar, kLightFar, s1, kLightGrid1Z) == 0);
    CHECK(lightSlice(kLightFar1 * 3.0f, kLightFar, s1, kLightGrid1Z) == kLightGrid1Z - 1);
    CHECK(std::abs(lightSliceNear(kLightGrid1Z, kLightFar, s1) - kLightFar1) < kLightFar1 * 1e-4f);
    for (std::uint32_t k = 0; k < kLightGrid1Z; ++k) CHECK(lightSlice(lightSliceNear(k, kLightFar, s1) * 1.001f, kLightFar, s1, kLightGrid1Z) == k);
    // the cluster budget: 16 x 9 x 24 + 32 x 18 x 8 clusters of 256 bytes
    CHECK(kLightGridX * kLightGridY * kLightGridZ + kLightGrid1X * kLightGrid1Y * kLightGrid1Z == 3456 + 4608);
}

// a lamp shadow entry survives the RGBA16F image: id + q / 64 is exact in a half float for every id 1..31 and q 0..63
static float roundToHalf(float v) {
    if (v == 0.0f) return 0.0f;
    int e = 0;
    std::frexp(v, &e);  // v = m 2^e, m in [0.5, 1): the half's ulp is 2^(e - 11)
    const float ulp = std::ldexp(1.0f, e - 11);
    return std::nearbyint(v / ulp) * ulp;
}

TEST(light_table_shadow_entries_are_exact_in_half_floats) {
    using namespace enh;
    for (std::uint32_t id = 1; id <= kLightShadowMax; ++id) {
        for (int q = 0; q <= 63; ++q) {
            const float vis = static_cast<float>(q) / 63.0f;
            const float e = lightShadowEncode(id, vis);
            CHECK(roundToHalf(e) == e);
            CHECK(lightShadowId(e) == id);
            CHECK(std::abs(lightShadowVis(e) - vis) < 1e-5f);
        }
    }
    CHECK(lightShadowEncode(5, 1.0f) < 6.0f && lightShadowEncode(5, 0.0f) == 5.0f);
}
