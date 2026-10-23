// High fps compatibility (src/engine/high_fps.*, docs/research/fps-limit.md 12): the per-frame scale of the fixes.

#include <cmath>

#include "engine/high_fps.h"
#include "test.h"

using namespace rvk;

TEST(high_fps_frame_scale) {
    namespace hf = engine::highfps;
    CHECK(hf::frameScale(1.0f / 30.0f) == 1.0f);                     // the patch's reference rate
    CHECK(std::fabs(hf::frameScale(1.0f / 120.0f) - 4.0f) < 1e-4f);  // 4 frames per 30 fps frame
    CHECK(std::fabs(hf::frameScale(1.0f / 240.0f) - 8.0f) < 1e-4f);
    CHECK(hf::frameScale(1.0f / 20.0f) == 1.0f);                     // below 30 fps: the game's behaviour
    CHECK(hf::frameScale(0.0f) == 1.0f);                             // time freeze
    CHECK(hf::frameScale(-1.0f) == 1.0f);
    CHECK(hf::frameScale(1e-7f) == 100.0f);                          // clamped
}
