// Enhanced shadows (src/enh/shadows.*, src/engine/shadow_maps.*, docs/research/shadows.md): the engine's split formula and
// the hardware depth of a cascade's quad back to depth-map units.

#include <cmath>

#include "d3d9/scene.h"
#include "engine/shadow_maps.h"
#include "enh/shadows.h"
#include "test.h"

using namespace rvk;

TEST(shadows_engine_split_formula) {
    // High: 3 cascades, range 7500, factor 0.4 -> 1 / 1012 / 2230 (the splits rvk.log measured)
    namespace sm = engine::shadowmaps;
    CHECK(std::fabs(sm::splitStart(7500.0f, 0.4f, 0, 3) - 1.0f) < 1e-3f);
    CHECK(std::fabs(sm::splitStart(7500.0f, 0.4f, 1, 3) - 1012.0f) < 2.0f);
    CHECK(std::fabs(sm::splitStart(7500.0f, 0.4f, 2, 3) - 2230.0f) < 2.0f);
    // Medium: 5000, 0.65 -> 1090 / 2269
    CHECK(std::fabs(sm::splitStart(5000.0f, 0.65f, 1, 3) - 1090.0f) < 2.0f);
    CHECK(std::fabs(sm::splitStart(5000.0f, 0.65f, 2, 3) - 2269.0f) < 2.0f);
}

TEST(shadows_hardware_z_to_depth_map) {
    // a D3D left-handed perspective projection, near 25, far 60000 (High): z' = (z f / (f - n) - n f / (f - n)) / z
    d3d9::scene::Mat4 p{};
    const float n = 25.0f, f = 60000.0f;
    p.at(0, 0) = 1.0f;
    p.at(1, 1) = 1.7f;
    p.at(2, 2) = f / (f - n);
    p.at(2, 3) = 1.0f;
    p.at(3, 2) = -n * f / (f - n);
    for (float z : {1012.0f, 2230.0f, 7500.0f}) {
        const float hw = (z * p.at(2, 2) + p.at(3, 2)) / z;
        float d = 0.0f;
        CHECK(enh::viewDepthOfHardwareZ(p, hw, f, d));
        CHECK(std::fabs(d * f - z) < z * 1e-3f);
    }
    float d = 0.0f;
    d3d9::scene::Mat4 ortho = d3d9::scene::Mat4::identity();
    CHECK(!enh::viewDepthOfHardwareZ(ortho, 0.5f, f, d));
}
