#include "enh/rt_lod.h"

#include <algorithm>

#include <meshoptimizer.h>

namespace rvk::enh::rt::lod {

std::size_t bandOf(float distanceMetres) {
    for (std::size_t i = 0; i < kBandEdgesMetres.size(); ++i) {
        if (distanceMetres < kBandEdgesMetres[i]) return i;
    }
    return kBands - 1;
}

const char* bandName(std::size_t band) {
    static constexpr const char* kNames[kBands] = {"0-20m", "20-50m", "50-100m", "100-200m", "200m+"};
    return band < kBands ? kNames[band] : "?";
}

float targetRatio(int bias) {
    switch (bias) {
    case 1: return 0.5f;
    case 2: return 0.25f;
    case 3: return 0.125f;
    default: return 1.0f;
    }
}

int rungFor(int current, float d, float proxyRange, float boxRange, float h) {
    constexpr float kOff = 3.0e38f;
    const float t0 = proxyRange > 0.0f ? proxyRange : kOff, t1 = boxRange > 0.0f ? boxRange : kOff;
    if (d > t1) return 2;
    if (current == 2 && d >= t1 * (1.0f - h)) return 2;
    if (d > t0) return 1;
    if (current >= 1 && d >= t0 * (1.0f - h)) return 1;
    return 0;
}

bool tooSmall(float radius, float d) { return d > 1.0f && radius < kTinyAngle * d; }

Result simplify(const float* positions, std::size_t vertexCount, std::size_t strideBytes, const std::vector<std::uint32_t>& indices, float ratio,
                float errorFraction) {
    Result res;
    if (positions == nullptr || vertexCount == 0 || strideBytes < 12 || strideBytes % 4 != 0 || indices.size() < 12 || indices.size() % 3 != 0 ||
        ratio >= 0.999f) {
        return res;
    }
    // the position-only weld: vertices split by uv / normal seams must collapse together, or the simplifier keeps every seam's
    // border; `remap[i]` is the first vertex with the same position, still an index of the same array
    std::vector<unsigned> remap(vertexCount);
    meshopt_generatePositionRemap(remap.data(), positions, vertexCount, strideBytes);
    std::vector<unsigned> welded(indices.size());
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] >= vertexCount) return res;
        welded[i] = remap[indices[i]];
    }
    const std::size_t target = std::max<std::size_t>(3, static_cast<std::size_t>(static_cast<double>(indices.size()) * ratio) / 3 * 3);
    std::vector<unsigned> out(welded.size());
    float err = 0.0f;
    const std::size_t n = meshopt_simplify(out.data(), welded.data(), welded.size(), positions, vertexCount, strideBytes, target, errorFraction, 0, &err);
    // no use when the error limit kept most of the mesh
    if (n < 3 || static_cast<double>(n) > 0.85 * static_cast<double>(indices.size())) return res;
    res.indices.assign(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n));
    res.error = err;
    return res;
}

}  // namespace rvk::enh::rt::lod
