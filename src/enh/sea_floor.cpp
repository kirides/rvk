#include "enh/sea_floor.h"

#include <algorithm>
#include <chrono>
#include <execution>
#include <numeric>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace rvk::enh::seafloor {

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr double kG = 9.81;
constexpr std::uint32_t kMagic = 0x464b5652u;  // "RVKF"
constexpr std::uint32_t kVersion = 1;

double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// rows 0 .. n-1 in parallel (the bake runs on a worker thread; the standard library's pool)
template <typename Fn>
void parallelRows(int n, Fn&& fn) {
    std::vector<int> rows(static_cast<std::size_t>(n));
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), fn);
}

void boxBlur(std::vector<float>& v, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<float> tmp(v.size());
    const float norm = 1.0f / static_cast<float>(2 * r + 1);
    for (int z = 0; z < h; ++z) {
        for (int x = 0; x < w; ++x) {
            float s = 0.0f;
            for (int k = -r; k <= r; ++k) s += v[static_cast<std::size_t>(z) * w + std::clamp(x + k, 0, w - 1)];
            tmp[static_cast<std::size_t>(z) * w + x] = s * norm;
        }
    }
    for (int z = 0; z < h; ++z) {
        for (int x = 0; x < w; ++x) {
            float s = 0.0f;
            for (int k = -r; k <= r; ++k) s += tmp[static_cast<std::size_t>(std::clamp(z + k, 0, h - 1)) * w + x];
            v[static_cast<std::size_t>(z) * w + x] = s * norm;
        }
    }
}

}  // namespace

float phaseSpeed(float depthMetres, float period) {
    // Fenton & McKee (1990): L = L0 tanh((2 pi d / L0)^(3/4))^(2/3), within ~1.5 % of the exact dispersion relation
    const double l0 = kG * period * period / (2.0 * 3.14159265358979);
    const double d = std::max(static_cast<double>(depthMetres), 0.01);
    const double l = l0 * std::pow(std::tanh(std::pow(2.0 * 3.14159265358979 * d / l0, 0.75)), 2.0 / 3.0);
    return static_cast<float>(l / period);
}

Grid gridAround(float minX, float minZ, float maxX, float maxZ, float margin, int maxResolution, float minCell) {
    Grid g;
    g.minX = minX - margin;
    g.minZ = minZ - margin;
    const float x1 = maxX + margin, z1 = maxZ + margin;
    g.cell = std::max(minCell, std::max(x1 - g.minX, z1 - g.minZ) / static_cast<float>(maxResolution));
    g.width = std::clamp(static_cast<int>(std::ceil((x1 - g.minX) / g.cell)), 1, maxResolution);
    g.height = std::clamp(static_cast<int>(std::ceil((z1 - g.minZ) / g.cell)), 1, maxResolution);
    return g;
}

void Heights::reset(const Grid& g) {
    grid = g;
    top.assign(g.count(), kUnknown);
}

void Heights::add(std::span<const shore::Triangle> tris) {
    shore::RasterGrid rg;
    rg.width = grid.width;
    rg.height = grid.height;
    rg.minX = grid.minX;
    rg.minZ = grid.minZ;
    rg.cell = grid.cell;
    for (const shore::Triangle& t : tris) {
        shore::rasterizeTopDown(rg, t, [&](std::size_t i, float y) { top[i] = std::max(top[i], y); });
    }
}

std::size_t Heights::known() const {
    std::size_t n = 0;
    for (float v : top) n += v != kUnknown ? 1 : 0;
    return n;
}

bool Heights::save(const std::filesystem::path& file) const {
    if (!grid.valid() || top.size() != grid.count()) return false;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        const std::uint32_t head[4] = {kMagic, kVersion, static_cast<std::uint32_t>(grid.width), static_cast<std::uint32_t>(grid.height)};
        const float geo[3] = {grid.minX, grid.minZ, grid.cell};
        f.write(reinterpret_cast<const char*>(head), sizeof(head));
        f.write(reinterpret_cast<const char*>(geo), sizeof(geo));
        f.write(reinterpret_cast<const char*>(top.data()), static_cast<std::streamsize>(top.size() * sizeof(float)));
        if (!f) return false;
    }
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

bool Heights::load(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    std::uint32_t head[4]{};
    float geo[3]{};
    f.read(reinterpret_cast<char*>(head), sizeof(head));
    f.read(reinterpret_cast<char*>(geo), sizeof(geo));
    if (!f || head[0] != kMagic || head[1] != kVersion || static_cast<int>(head[2]) != grid.width ||
        static_cast<int>(head[3]) != grid.height || geo[0] != grid.minX || geo[1] != grid.minZ || geo[2] != grid.cell) {
        return false;
    }
    std::vector<float> v(grid.count());
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(float)));
    if (!f) return false;
    if (top.size() != v.size()) top.assign(v.size(), kUnknown);
    for (std::size_t i = 0; i < v.size(); ++i) top[i] = std::max(top[i], v[i]);
    return true;
}

std::array<float, 8> Field::at(float x, float z) const {
    std::array<float, 8> r{};
    if (!valid()) return r;
    const int ix = static_cast<int>(std::floor((x - grid.minX) / grid.cell));
    const int iz = static_cast<int>(std::floor((z - grid.minZ) / grid.cell));
    if (ix < 0 || iz < 0 || ix >= grid.width || iz >= grid.height) return r;
    const std::size_t i = grid.index(ix, iz) * 4;
    for (int c = 0; c < 4; ++c) {
        r[static_cast<std::size_t>(c)] = shore::halfToFloat(a[i + static_cast<std::size_t>(c)]);
        r[static_cast<std::size_t>(c) + 4] = shore::halfToFloat(b[i + static_cast<std::size_t>(c)]);
    }
    return r;
}

Field bake(const Heights& h, float seaLevel, const shore::Field& overlay) {
    Field out;
    const Grid& g = h.grid;
    if (!g.valid() || h.top.size() != g.count()) return out;
    const int w = g.width, ht = g.height;
    const std::size_t n = g.count();
    std::vector<float> depth(n);
    std::vector<std::uint8_t> land(n);
    for (std::size_t i = 0; i < n; ++i) {
        const bool known = h.top[i] != kUnknown;
        depth[i] = known ? seaLevel - h.top[i] : kDeepDefault;
        land[i] = depth[i] <= 0.0f ? 1 : 0;
        out.knownTexels += known ? 1 : 0;
        out.landTexels += land[i];
    }
    out.waterTexels = n - out.landTexels;

    double t0 = nowMs();
    // ---- signed distance to the waterline ----
    std::vector<float> toLand(n), toWater(n);
    for (std::size_t i = 0; i < n; ++i) {
        toLand[i] = land[i] ? 0.0f : shore::kFarSite;
        toWater[i] = land[i] ? shore::kFarSite : 0.0f;
    }
    const bool anyLand = out.landTexels > 0, anyWater = out.waterTexels > 0;
    if (anyLand) shore::squaredDistanceTransform(toLand, w, ht);
    if (anyWater) shore::squaredDistanceTransform(toWater, w, ht);
    std::vector<float> sd(n);
    for (std::size_t i = 0; i < n; ++i) {
        // the waterline lies half a texel between a land and a water texel
        const float dl = anyLand ? std::sqrt(toLand[i]) : 1e9f;
        const float dw = anyWater ? std::sqrt(toWater[i]) : 1e9f;
        const float v = land[i] ? -(dw - 0.5f) * g.cell : (dl - 0.5f) * g.cell;
        sd[i] = std::clamp(v, -kMaxSigned, kMaxSigned);
    }

    out.msDistance = static_cast<float>(nowMs() - t0);
    t0 = nowMs();
    // ---- travel time to the shore: |grad T| = 1 / c(depth), fast sweeping (Zhao 2005, Godunov upwind) ----
    // solved on a grid of half the resolution (the field is smooth; 4 x fewer texels), then interpolated over the water
    const int f2 = w >= 1024 && ht >= 1024 ? 2 : 1;
    const int tw = (w + f2 - 1) / f2, th = (ht + f2 - 1) / f2;
    const std::size_t tn = static_cast<std::size_t>(tw) * th;
    std::vector<std::uint8_t> tland(tn, 0);
    std::vector<float> tdepth(tn, 0.0f), tcount(tn, 0.0f);
    for (int z = 0; z < ht; ++z) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = g.index(x, z);
            const std::size_t j = static_cast<std::size_t>(z / f2) * tw + x / f2;
            if (land[i]) {
                tland[j] = 1;
            } else {
                tdepth[j] += depth[i];
                tcount[j] += 1.0f;
            }
        }
    }
    // the slowness from a table of the phase speed (depth in whole world units up to the open sea's default)
    std::vector<float> slowLut(static_cast<std::size_t>(kDeepDefault) + 2);
    const float hm = g.cell * static_cast<float>(f2) / 100.0f;  // coarse texel size in metres
    for (std::size_t d = 0; d < slowLut.size(); ++d) slowLut[d] = hm / phaseSpeed(static_cast<float>(d) / 100.0f, kRefPeriod);
    std::vector<float> Tc(tn, kInf), slow(tn, 0.0f);
    for (int z = 0; z < th; ++z) {
        for (int x = 0; x < tw; ++x) {
            const std::size_t i = static_cast<std::size_t>(z) * tw + x;
            if (tland[i]) continue;
            const float d = tcount[i] > 0.0f ? tdepth[i] / tcount[i] : kDeepDefault;
            slow[i] = slowLut[static_cast<std::size_t>(std::clamp(d, 0.0f, kDeepDefault))];
            const bool coast = (x > 0 && tland[i - 1]) || (x + 1 < tw && tland[i + 1]) || (z > 0 && tland[i - static_cast<std::size_t>(tw)]) ||
                               (z + 1 < th && tland[i + static_cast<std::size_t>(tw)]);
            if (coast) Tc[i] = 0.0f;
        }
    }
    if (anyLand) {
        for (int round = 0; round < 2; ++round) {
            for (int order = 0; order < 4; ++order) {
                const bool fx = (order & 1) != 0, fz = (order & 2) != 0;
                for (int zz = 0; zz < th; ++zz) {
                    const int z = fz ? th - 1 - zz : zz;
                    for (int xx = 0; xx < tw; ++xx) {
                        const int x = fx ? tw - 1 - xx : xx;
                        const std::size_t i = static_cast<std::size_t>(z) * tw + x;
                        if (tland[i] || Tc[i] == 0.0f) continue;
                        const float a = std::min(x > 0 ? Tc[i - 1] : kInf, x + 1 < tw ? Tc[i + 1] : kInf);
                        const float b = std::min(z > 0 ? Tc[i - static_cast<std::size_t>(tw)] : kInf,
                                                 z + 1 < th ? Tc[i + static_cast<std::size_t>(tw)] : kInf);
                        if (a == kInf && b == kInf) continue;
                        const float fs = slow[i];
                        float t;
                        if (a == kInf || b == kInf || std::abs(a - b) >= fs) {
                            t = std::min(a, b) + fs;
                        } else {
                            t = 0.5f * (a + b + std::sqrt(2.0f * fs * fs - (a - b) * (a - b)));
                        }
                        Tc[i] = std::min(Tc[i], t);
                    }
                }
            }
        }
    }
    // back to the full grid: bilinear over the coarse water texels (land carries no time)
    std::vector<float> T(n, kInf);
    parallelRows(ht, [&](int z) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = g.index(x, z);
            if (land[i]) continue;
            const float cx = (static_cast<float>(x) + 0.5f) / static_cast<float>(f2) - 0.5f;
            const float cz = (static_cast<float>(z) + 0.5f) / static_cast<float>(f2) - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(cx)), 0, tw - 1), z0 = std::clamp(static_cast<int>(std::floor(cz)), 0, th - 1);
            const int x1 = std::min(x0 + 1, tw - 1), z1 = std::min(z0 + 1, th - 1);
            const float tx = std::clamp(cx - static_cast<float>(x0), 0.0f, 1.0f), tz = std::clamp(cz - static_cast<float>(z0), 0.0f, 1.0f);
            float sum = 0.0f, wsum = 0.0f;
            const int xs[2] = {x0, x1}, zs[2] = {z0, z1};
            const float wx[2] = {1.0f - tx, tx}, wz[2] = {1.0f - tz, tz};
            for (int a2 = 0; a2 < 2; ++a2) {
                for (int b2 = 0; b2 < 2; ++b2) {
                    const std::size_t j = static_cast<std::size_t>(zs[b2]) * tw + xs[a2];
                    const float wt = wx[a2] * wz[b2];
                    if (tland[j] || Tc[j] == kInf || wt <= 0.0f) continue;
                    sum += Tc[j] * wt;
                    wsum += wt;
                }
            }
            T[i] = wsum > 0.0f ? sum / wsum : 0.0f;  // only coarse land around: right at the coast
        }
    });
    for (float& t : T) t = std::min(t, kMaxTravel);
    for (std::size_t i = 0; i < n; ++i) {
        if (land[i]) T[i] = 0.0f;
        out.maxTravel = std::max(out.maxTravel, T[i]);
    }

    // ---- direction of travel (towards the shore: -grad T) and the fronts' coherence ----
    std::vector<float> dx(n, 0.0f), dz(n, 0.0f);
    parallelRows(ht, [&](int z) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = g.index(x, z);
            if (land[i] || T[i] >= kMaxTravel) continue;
            const auto at = [&](int xi, int zi) -> float {
                xi = std::clamp(xi, 0, w - 1);
                zi = std::clamp(zi, 0, ht - 1);
                const std::size_t j = g.index(xi, zi);
                return land[j] ? T[i] : T[j];
            };
            const float gx = at(x + 1, z) - at(x - 1, z);
            const float gz = at(x, z + 1) - at(x, z - 1);
            const float len = std::sqrt(gx * gx + gz * gz);
            if (len > 1e-6f) {
                dx[i] = -gx / len;
                dz[i] = -gz / len;
            }
        }
    });
    std::vector<float> bx = dx, bz = dz;
    boxBlur(bx, w, ht, 3);
    boxBlur(bz, w, ht, 3);

    out.msTravel = static_cast<float>(nowMs() - t0);
    t0 = nowMs();
    // ---- exposure: the share of directions in which the water stays open for kExposureReach (coarse grid) ----
    const int f = std::max(1, static_cast<int>(std::lround(600.0f / g.cell)));
    const int cw = (w + f - 1) / f, ch = (ht + f - 1) / f;
    std::vector<std::uint8_t> cland(static_cast<std::size_t>(cw) * ch, 0);
    for (int z = 0; z < ht; ++z) {
        for (int x = 0; x < w; ++x) {
            if (land[g.index(x, z)]) cland[static_cast<std::size_t>(z / f) * cw + x / f] = 1;
        }
    }
    constexpr int kDirs = 16;
    constexpr float kExposureReach = 40000.0f;
    const float ccell = g.cell * static_cast<float>(f);
    const int steps = std::max(1, static_cast<int>(kExposureReach / ccell));
    std::vector<float> cexp(static_cast<std::size_t>(cw) * ch, 1.0f);
    parallelRows(ch, [&](int z) {
        for (int x = 0; x < cw; ++x) {
            if (cland[static_cast<std::size_t>(z) * cw + x]) {
                cexp[static_cast<std::size_t>(z) * cw + x] = 0.0f;
                continue;
            }
            int open = 0;
            for (int d = 0; d < kDirs; ++d) {
                const float ang = 6.2831853f * (static_cast<float>(d) + 0.5f) / kDirs;
                const float sx = std::cos(ang), sz = std::sin(ang);
                bool blocked = false;
                for (int s = 1; s <= steps; ++s) {
                    const int px = static_cast<int>(std::lround(static_cast<float>(x) + sx * static_cast<float>(s)));
                    const int pz = static_cast<int>(std::lround(static_cast<float>(z) + sz * static_cast<float>(s)));
                    if (px < 0 || pz < 0 || px >= cw || pz >= ch) break;
                    if (cland[static_cast<std::size_t>(pz) * cw + px]) {
                        blocked = true;
                        break;
                    }
                }
                open += blocked ? 0 : 1;
            }
            const float e = static_cast<float>(open) / kDirs;
            // an open beach sees half of the directions: full exposure from ~0.45, a basin behind a narrow mouth ~0.1
            const float t = std::clamp((e - 0.05f) / 0.4f, 0.0f, 1.0f);
            cexp[static_cast<std::size_t>(z) * cw + x] = t * t * (3.0f - 2.0f * t);
        }
    });
    boxBlur(cexp, cw, ch, 1);
    out.msExposure = static_cast<float>(nowMs() - t0);
    t0 = nowMs();  // long waves bend into the lee a little (diffraction)

    // ---- pack ----
    out.grid = g;
    out.a.resize(n * 4);
    out.b.resize(n * 4);
    parallelRows(ht, [&](int z) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = g.index(x, z);
            float coh = std::sqrt(bx[i] * bx[i] + bz[i] * bz[i]);
            coh = std::clamp((coh - 0.3f) / 0.6f, 0.0f, 1.0f);
            // the coarse exposure, bilinear
            const float cxf = (static_cast<float>(x) + 0.5f) / static_cast<float>(f) - 0.5f;
            const float czf = (static_cast<float>(z) + 0.5f) / static_cast<float>(f) - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(cxf)), 0, cw - 1), z0 = std::clamp(static_cast<int>(std::floor(czf)), 0, ch - 1);
            const int x1 = std::min(x0 + 1, cw - 1), z1 = std::min(z0 + 1, ch - 1);
            const float tx = std::clamp(cxf - static_cast<float>(x0), 0.0f, 1.0f), tz = std::clamp(czf - static_cast<float>(z0), 0.0f, 1.0f);
            const auto ce = [&](int xi, int zi) { return cexp[static_cast<std::size_t>(zi) * cw + xi]; };
            const float ex = (ce(x0, z0) * (1 - tx) + ce(x1, z0) * tx) * (1 - tz) + (ce(x0, z1) * (1 - tx) + ce(x1, z1) * tx) * tz;
            const float wx = g.minX + (static_cast<float>(x) + 0.5f) * g.cell, wz = g.minZ + (static_cast<float>(z) + 0.5f) * g.cell;
            const float od = overlay.valid() ? overlay.at(wx, wz)[0] : shore::kMaxDistance;
            out.a[i * 4 + 0] = shore::floatToHalf(sd[i]);
            out.a[i * 4 + 1] = shore::floatToHalf(std::clamp(depth[i], -30000.0f, 30000.0f));
            out.a[i * 4 + 2] = shore::floatToHalf(dx[i] * coh);
            out.a[i * 4 + 3] = shore::floatToHalf(dz[i] * coh);
            out.b[i * 4 + 0] = shore::floatToHalf(T[i]);
            out.b[i * 4 + 1] = shore::floatToHalf(land[i] ? 0.0f : ex);
            out.b[i * 4 + 2] = shore::floatToHalf(od);
            out.b[i * 4 + 3] = shore::floatToHalf(h.top[i] != kUnknown ? 1.0f : 0.0f);
        }
    });
    out.msPack = static_cast<float>(nowMs() - t0);
    return out;
}

}  // namespace rvk::enh::seafloor
