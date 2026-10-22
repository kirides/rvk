#include "enh/water_profile.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace rvk::enh {

using d3d9::draws::Water;
using shader::TextureType;

namespace {

bool layoutIs(const TextureType* s, std::initializer_list<std::pair<int, TextureType>> want) {
    TextureType expect[16]{};
    for (const auto& [slot, type] : want) expect[slot] = type;
    for (int i = 0; i < 16; ++i) {
        if (s[i] != expect[i]) return false;
    }
    return true;
}

}  // namespace

WaterModel engineWaterModel(const TextureType* s) {
    if (s == nullptr) return WaterModel::None;
    constexpr TextureType T = TextureType::Tex2D, C = TextureType::Cube;
    if (layoutIs(s, {{0, T}, {1, T}, {2, T}, {9, T}, {10, T}, {11, T}, {12, C}, {13, T}, {14, T}})) return WaterModel::Ocean;
    if (layoutIs(s, {{1, T}, {2, T}, {9, C}, {10, T}, {11, T}, {12, T}, {13, T}, {14, T}})) return WaterModel::Lake;
    // the same with point lights (caves): s0 = the point lights' shadow mask
    if (layoutIs(s, {{0, T}, {1, T}, {2, T}, {9, C}, {10, T}, {11, T}, {12, T}, {13, T}, {14, T}})) return WaterModel::Lake;
    if (layoutIs(s, {{1, T}, {2, T}, {9, T}, {10, T}})) return WaterModel::Swamp;
    if (layoutIs(s, {{1, T}, {2, T}, {9, T}, {10, T}, {11, T}, {12, T}, {13, C}, {14, T}, {15, T}})) return WaterModel::River;
    return WaterModel::None;
}

const char* name(WaterModel m) {
    switch (m) {
    case WaterModel::Ocean: return "ocean";
    case WaterModel::Lake: return "lake";
    case WaterModel::Swamp: return "swamp";
    case WaterModel::River: return "river";
    default: return "none";
    }
}

WaterProfile waterProfile(Water kind, float murkiness, float reflections, float waveHeight) {
    WaterProfile p;
    p.reflection = std::clamp(reflections, 0.0f, 4.0f);
    p.murkiness = std::clamp(murkiness, 0.05f, 20.0f);
    if (kind == Water::Swamp) p.extraReflection = 0.45f * p.reflection;  // still, dark water: the trees, dimly
    if (kind == Water::Ocean) {
        p.foam = 1.0f;
        p.shoreWaves = 25.0f * std::clamp(waveHeight, 0.0f, 3.0f);
    }
    return p;
}

bool fitAffineXZ(const float* x, const float* z, const float* v, std::size_t n, float out[3], float* maxError) {
    if (n < 3) return false;
    // centred sums (x, z can be ~1e5: centring keeps the normal equations well conditioned)
    double mx = 0, mz = 0, mv = 0;
    for (std::size_t i = 0; i < n; ++i) {
        mx += x[i];
        mz += z[i];
        mv += v[i];
    }
    mx /= static_cast<double>(n);
    mz /= static_cast<double>(n);
    mv /= static_cast<double>(n);
    double sxx = 0, sxz = 0, szz = 0, sxv = 0, szv = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double dx = x[i] - mx, dz = z[i] - mz, dv = v[i] - mv;
        sxx += dx * dx;
        sxz += dx * dz;
        szz += dz * dz;
        sxv += dx * dv;
        szv += dz * dv;
    }
    const double det = sxx * szz - sxz * sxz;
    if (!(std::abs(det) > 1e-9 * std::max(1.0, sxx * szz))) return false;
    const double a = (sxv * szz - szv * sxz) / det;
    const double b = (szv * sxx - sxv * sxz) / det;
    const double c = mv - a * mx - b * mz;
    double err = 0;
    for (std::size_t i = 0; i < n; ++i) err = std::max(err, std::abs(a * x[i] + b * z[i] + c - v[i]));
    out[0] = static_cast<float>(a);
    out[1] = static_cast<float>(b);
    out[2] = static_cast<float>(c);
    if (maxError != nullptr) *maxError = static_cast<float>(err);
    return true;
}

}  // namespace rvk::enh
