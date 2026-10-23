// Upscaling (src/enh/upscale.*, docs/research/upscaling.md): FSR's jitter sequence.

#include <algorithm>
#include <cmath>
#include <set>

#include "enh/upscale.h"
#include "test.h"

using namespace rvk;

TEST(upscale_jitter_phase_count) {
    CHECK(enh::upscaleJitterPhases(1920, 1920) == 8);    // native AA
    CHECK(enh::upscaleJitterPhases(1280, 1920) == 18);   // 1.5x: 8 * 2.25
    CHECK(enh::upscaleJitterPhases(960, 1920) == 32);    // 2x
}

TEST(upscale_jitter_sequence) {
    // the first Halton(2, 3) points minus 0.5: (0, -1/6), (-1/4, 1/6), (1/4, -0.5 + 2/9) ... and a period of `phases`
    const auto a = enh::upscaleJitter(0, 8);
    CHECK(std::abs(a[0] - 0.0f) < 1e-6f && std::abs(a[1] - (1.0f / 3.0f - 0.5f)) < 1e-6f);
    const auto b = enh::upscaleJitter(1, 8);
    CHECK(std::abs(b[0] - (-0.25f)) < 1e-6f && std::abs(b[1] - (2.0f / 3.0f - 0.5f)) < 1e-6f);
    std::set<std::pair<float, float>> seen;
    for (std::uint64_t i = 0; i < 8; ++i) {
        const auto j = enh::upscaleJitter(i, 8);
        CHECK(std::abs(j[0]) < 0.5f && std::abs(j[1]) < 0.5f);
        CHECK(enh::upscaleJitter(i + 8, 8) == j);
        seen.insert({j[0], j[1]});
    }
    CHECK(seen.size() == 8);
    // never zero: the device treats (0, 0) as "no jitter"
    for (std::uint64_t i = 0; i < 64; ++i) {
        const auto j = enh::upscaleJitter(i, 32);
        CHECK(j[0] != 0.0f || j[1] != 0.0f);
    }
}
