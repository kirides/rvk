#include "enh/water_profile.h"

#include <algorithm>
#include <cmath>

namespace rvk::enh {

using d3d9::draws::Water;

namespace {

struct KindDefaults {
    std::array<float, 3> scatter;  // when the material gives no usable colour
    std::array<float, 3> absorb;   // per world unit
    float reflectivity, roughness, glint, ripple, rippleScale, refraction, foam, turbidity;
};

// absorption: T = exp(-absorb * path); the green channel reaches ~5 % after the engine's opaque depth (c7 of the shaders)
constexpr KindDefaults kOcean{{0.035f, 0.045f, 0.06f}, {0.0040f, 0.0016f, 0.0013f}, 1.0f, 0.07f, 1.0f, 0.55f, 700.0f, 1.0f, 1.0f, 0.12f};
constexpr KindDefaults kLake{{0.11f, 0.15f, 0.18f}, {0.014f, 0.009f, 0.009f}, 0.9f, 0.06f, 0.8f, 0.35f, 420.0f, 0.7f, 0.0f, 0.25f};
constexpr KindDefaults kRiver{{0.13f, 0.14f, 0.15f}, {0.012f, 0.008f, 0.008f}, 0.85f, 0.10f, 0.7f, 0.65f, 260.0f, 0.8f, 0.0f, 0.25f};
constexpr KindDefaults kSwamp{{0.15f, 0.17f, 0.12f}, {0.050f, 0.034f, 0.060f}, 0.55f, 0.22f, 0.2f, 0.15f, 520.0f, 0.3f, 0.0f, 0.55f};

const KindDefaults& defaults(Water kind) {
    switch (kind) {
    case Water::Ocean: return kOcean;
    case Water::River: return kRiver;
    case Water::Swamp: return kSwamp;
    default: return kLake;
    }
}

}  // namespace

WaterProfile waterProfile(Water kind, const float* materialColour, float murkiness, float reflections) {
    const KindDefaults& d = defaults(kind);
    WaterProfile p;
    p.scatter = d.scatter;
    if (materialColour != nullptr) {
        // c6.rgb is a colour in every water shader seen (ocean, lake, river, swamp); anything outside 0..2 is not
        bool usable = true;
        float maxc = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const float c = materialColour[i];
            usable = usable && std::isfinite(c) && c >= 0.0f && c <= 2.0f;
            maxc = std::max(maxc, c);
        }
        if (usable && maxc > 1e-3f) p.scatter = {materialColour[0], materialColour[1], materialColour[2]};
    }
    const float m = std::clamp(murkiness, 0.05f, 20.0f);
    for (int i = 0; i < 3; ++i) p.absorb[static_cast<std::size_t>(i)] = d.absorb[static_cast<std::size_t>(i)] * m;
    const float r = std::clamp(reflections, 0.0f, 4.0f);
    p.reflectivity = d.reflectivity * r;
    p.glint = d.glint * r;
    p.roughness = d.roughness;
    p.ripple = d.ripple;
    p.rippleScale = d.rippleScale;
    p.refraction = d.refraction;
    p.foamStrength = d.foam;
    p.turbidity = std::clamp(d.turbidity * m, 0.0f, 0.9f);
    // foam: mostly white, tinted by the water's own colour (a murky body never gets bright white foam)
    const float maxc = std::max({p.scatter[0], p.scatter[1], p.scatter[2], 1e-3f});
    for (int i = 0; i < 3; ++i) {
        const auto k = static_cast<std::size_t>(i);
        p.foam[k] = 0.82f * (0.8f + 0.2f * p.scatter[k] / maxc);
    }
    return p;
}

}  // namespace rvk::enh
