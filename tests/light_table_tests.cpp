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
