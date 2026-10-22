#include "enh/shore_field.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <execution>

namespace rvk::enh::shore {
namespace {

constexpr float kFar = kFarSite;  // distance transform: not a site
constexpr int kBlurRadius = 2;   // texels; smooths the direction towards the coast

using Grid = RasterGrid;

// Felzenszwalb-Huttenlocher squared distance transform of one row or column, in texels
void distanceTransform1D(const float* f, float* d, int n, int* v, double* z) {
    int k = 0;
    v[0] = 0;
    z[0] = -1e30;
    z[1] = 1e30;
    for (int q = 1; q < n; ++q) {
        double s = 0.0;
        for (;;) {
            const int p = v[k];
            s = ((static_cast<double>(f[q]) + static_cast<double>(q) * q) - (static_cast<double>(f[p]) + static_cast<double>(p) * p)) /
                (2.0 * q - 2.0 * p);
            if (s > z[k]) break;
            --k;  // z[0] is -inf, so this stops at 0
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = 1e30;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q) ++k;
        const double dq = static_cast<double>(q - v[k]);
        d[q] = static_cast<float>(dq * dq + f[v[k]]);
    }
}

// in place: 0 at sites, kFar elsewhere -> squared distance in texels to the nearest site; columns, then rows, each line
// independent (in parallel)
void distanceTransform2D(std::vector<float>& field, const Grid& g) {
    const int n = std::max(g.width, g.height);
    const auto lines = [&](int count, int length, auto&& get, auto&& put) {
        std::vector<int> idx(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) idx[static_cast<std::size_t>(i)] = i;
        std::for_each(std::execution::par, idx.begin(), idx.end(), [&](int line) {
            std::vector<float> f(static_cast<std::size_t>(n)), d(static_cast<std::size_t>(n));
            std::vector<int> v(static_cast<std::size_t>(n));
            std::vector<double> z(static_cast<std::size_t>(n) + 1);
            for (int i = 0; i < length; ++i) f[static_cast<std::size_t>(i)] = get(line, i);
            distanceTransform1D(f.data(), d.data(), length, v.data(), z.data());
            for (int i = 0; i < length; ++i) put(line, i, d[static_cast<std::size_t>(i)]);
        });
    };
    lines(g.width, g.height, [&](int x, int iz) { return field[g.index(x, iz)]; }, [&](int x, int iz, float v) { field[g.index(x, iz)] = v; });
    lines(g.height, g.width, [&](int iz, int x) { return field[g.index(x, iz)]; }, [&](int iz, int x, float v) { field[g.index(x, iz)] = v; });
}

void boxBlur(const std::vector<float>& src, std::vector<float>& dst, const Grid& g, int radius) {
    std::vector<float> tmp(src.size());
    const float norm = 1.0f / static_cast<float>(2 * radius + 1);
    for (int iz = 0; iz < g.height; ++iz) {
        for (int x = 0; x < g.width; ++x) {
            float sum = 0.0f;
            for (int k = -radius; k <= radius; ++k) sum += src[g.index(std::clamp(x + k, 0, g.width - 1), iz)];
            tmp[g.index(x, iz)] = sum * norm;
        }
    }
    for (int iz = 0; iz < g.height; ++iz) {
        for (int x = 0; x < g.width; ++x) {
            float sum = 0.0f;
            for (int k = -radius; k <= radius; ++k) sum += tmp[g.index(x, std::clamp(iz + k, 0, g.height - 1))];
            dst[g.index(x, iz)] = sum * norm;
        }
    }
}

}  // namespace

void squaredDistanceTransform(std::vector<float>& field, int width, int height) {
    Grid g;
    g.width = width;
    g.height = height;
    distanceTransform2D(field, g);
}

std::uint16_t floatToHalf(float f) {
    std::uint32_t x = 0;
    std::memcpy(&x, &f, 4);
    const std::uint32_t sign = (x >> 16) & 0x8000u;
    const std::int32_t exp = static_cast<std::int32_t>((x >> 23) & 0xff) - 127 + 15;
    std::uint32_t mant = x & 0x7fffffu;
    if (((x >> 23) & 0xff) == 0xff) return static_cast<std::uint16_t>(sign | 0x7c00u | (mant != 0 ? 0x200u : 0u));
    if (exp >= 31) return static_cast<std::uint16_t>(sign | 0x7bffu);  // clamp to the largest finite
    if (exp <= 0) {
        if (exp < -10) return static_cast<std::uint16_t>(sign);
        mant |= 0x800000u;
        const std::uint32_t shift = static_cast<std::uint32_t>(14 - exp);
        std::uint32_t h = mant >> shift;
        if ((mant >> (shift - 1)) & 1u) ++h;  // round
        return static_cast<std::uint16_t>(sign | h);
    }
    std::uint32_t h = sign | (static_cast<std::uint32_t>(exp) << 10) | (mant >> 13);
    if (mant & 0x1000u) ++h;  // round half up (carries into the exponent correctly)
    return static_cast<std::uint16_t>(h);
}

float halfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (h & 0x8000u) << 16;
    std::uint32_t exp = (h >> 10) & 0x1fu;
    std::uint32_t mant = h & 0x3ffu;
    std::uint32_t x = 0;
    if (exp == 0) {
        if (mant == 0) {
            x = sign;
        } else {
            exp = 127 - 15 + 1;
            while ((mant & 0x400u) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3ffu;
            x = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        x = sign | 0x7f800000u | (mant << 13);
    } else {
        x = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f = 0.0f;
    std::memcpy(&f, &x, 4);
    return f;
}

std::array<float, 4> Field::at(float x, float z) const {
    if (!valid()) return {kMaxDistance, kNoCoast, 0.0f, 0.0f};
    const int ix = static_cast<int>(std::floor((x - minX) / cell));
    const int iz = static_cast<int>(std::floor((z - minZ) / cell));
    if (ix < 0 || iz < 0 || ix >= width || iz >= height) return {kMaxDistance, kNoCoast, 0.0f, 0.0f};
    const std::size_t i = (static_cast<std::size_t>(iz) * static_cast<std::size_t>(width) + static_cast<std::size_t>(ix)) * 4;
    return {halfToFloat(texels[i]), halfToFloat(texels[i + 1]), halfToFloat(texels[i + 2]), halfToFloat(texels[i + 3])};
}

Field bake(std::span<const Triangle> coast, float margin, int maxResolution, float minCell) {
    Field out;
    if (coast.empty()) return out;
    float lo[2] = {FLT_MAX, FLT_MAX}, hi[2] = {-FLT_MAX, -FLT_MAX};
    for (const Triangle& t : coast) {
        for (const float* p : {t.a, t.b, t.c}) {
            lo[0] = std::min(lo[0], p[0]);
            lo[1] = std::min(lo[1], p[2]);
            hi[0] = std::max(hi[0], p[0]);
            hi[1] = std::max(hi[1], p[2]);
        }
    }
    Grid g;
    g.minX = lo[0] - margin;
    g.minZ = lo[1] - margin;
    const float maxX = hi[0] + margin, maxZ = hi[1] + margin;
    g.cell = std::max(minCell, std::max(maxX - g.minX, maxZ - g.minZ) / static_cast<float>(maxResolution));
    g.width = std::clamp(static_cast<int>(std::ceil((maxX - g.minX) / g.cell)), 1, maxResolution);
    g.height = std::clamp(static_cast<int>(std::ceil((maxZ - g.minZ) / g.cell)), 1, maxResolution);

    std::vector<float> height(g.count(), kNoCoast);
    for (const Triangle& t : coast) {
        rasterizeTopDown(g, t, [&](std::size_t i, float y) { height[i] = std::max(height[i], y); });
    }
    std::vector<float> dist(g.count());
    std::size_t sites = 0;
    for (std::size_t i = 0; i < g.count(); ++i) {
        const bool site = height[i] != kNoCoast;
        dist[i] = site ? 0.0f : kFar;
        sites += site ? 1 : 0;
    }
    if (sites == 0) return out;
    distanceTransform2D(dist, g);
    for (float& d : dist) d = std::min(std::sqrt(d) * g.cell, kMaxDistance);
    // direction towards the coast: minus the gradient of the blurred distance; it shortens where two coasts meet
    std::vector<float> blurred(g.count());
    boxBlur(dist, blurred, g, kBlurRadius);
    out.texels.resize(g.count() * 4);
    for (int iz = 0; iz < g.height; ++iz) {
        for (int x = 0; x < g.width; ++x) {
            const std::size_t i = g.index(x, iz);
            const int xl = std::max(x - 1, 0), xr = std::min(x + 1, g.width - 1);
            const int zd = std::max(iz - 1, 0), zu = std::min(iz + 1, g.height - 1);
            const float gx = (blurred[g.index(xr, iz)] - blurred[g.index(xl, iz)]) / (static_cast<float>(std::max(1, xr - xl)) * g.cell);
            const float gz = (blurred[g.index(x, zu)] - blurred[g.index(x, zd)]) / (static_cast<float>(std::max(1, zu - zd)) * g.cell);
            out.texels[i * 4 + 0] = floatToHalf(dist[i]);
            out.texels[i * 4 + 1] = floatToHalf(height[i]);
            out.texels[i * 4 + 2] = floatToHalf(std::clamp(-gx, -1.0f, 1.0f));
            out.texels[i * 4 + 3] = floatToHalf(std::clamp(-gz, -1.0f, 1.0f));
        }
    }
    out.width = g.width;
    out.height = g.height;
    out.minX = g.minX;
    out.minZ = g.minZ;
    out.cell = g.cell;
    out.coastTexels = sites;
    return out;
}

}  // namespace rvk::enh::shore
