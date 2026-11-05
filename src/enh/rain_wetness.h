#pragma once

// The wetness of the world as the rain wets and dries it (docs/research/material-rt.md 5): a level 0..1 that rises while it rains
// (about 25 s to follow the rain's share) and falls slowly once it stopped (about 4 minutes from fully wet to dry). Pure, unit-tested.

#include <algorithm>

namespace rvk::enh::rain {

inline constexpr float kWetSeconds = 25.0f;   // time constant of the wetting
inline constexpr float kDrySeconds = 240.0f;  // seconds from 1 to 0 when it does not rain

// one step of dt seconds towards the rain's share `rain` (0..1)
inline float wetnessStep(float wet, float rain, float dt) {
    dt = std::clamp(dt, 0.0f, 10.0f);
    if (rain > wet) {
        wet += (rain - wet) * std::min(1.0f, dt / kWetSeconds);
    } else {
        wet -= dt / kDrySeconds;
        wet = std::max(wet, rain);
    }
    return std::clamp(wet, 0.0f, 1.0f);
}

}  // namespace rvk::enh::rain
