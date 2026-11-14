// Enhanced shadows (src/enh/shadows.*, src/engine/shadow_maps.*, docs/research/shadows.md): the engine's split formula and
// the hardware depth of a cascade's quad back to depth-map units.

#include <cmath>
#include <cstring>

#include "d3d9/scene.h"
#include "engine/shadow_maps.h"
#include "enh/rt_shadows.h"
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

TEST(shadows_minimal_sphere_covers_slice) {
    namespace sm = engine::shadowmaps;
    for (float tx : {0.6f, 1.0f, 1.8f}) {
        for (float ty : {0.5f, 0.75f, 1.0f}) {
            const float n = 1.0f, f = 1347.0f;
            const sm::SliceSphere s = sm::minimalSphere(tx, ty, n, f);
            CHECK(s.centreZ >= n && s.centreZ <= f);
            // every corner of the slice lies inside the sphere, and the farthest sits ON it
            float maxd = 0.0f;
            for (float z : {n, f})
                for (float sx : {-1.0f, 1.0f})
                    for (float sy : {-1.0f, 1.0f}) {
                        const float dx = sx * tx * z, dy = sy * ty * z, dz = z - s.centreZ;
                        maxd = std::fmax(maxd, std::sqrt(dx * dx + dy * dy + dz * dz));
                    }
            CHECK(std::fabs(maxd - s.radius) < 1e-3f * s.radius);
            // never larger than the sphere around the far plane's centre
            const float k = tx * tx + ty * ty;
            CHECK(s.radius <= std::fmax(f * std::sqrt(k), std::sqrt((f - n) * (f - n) + n * n * k)) + 1e-3f);
        }
    }
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

TEST(shadows_rt_mode_ranges) {
    // the ray-traced range replaces the enhanced cascades 0 (1 / 1347 units) and 1 (up to 2945): near 15 m, extended 35 m
    CHECK(enh::RtShadows::modeRangeMetres(0) == 0.0f);
    CHECK(enh::RtShadows::modeRangeMetres(1) == 15.0f);
    CHECK(enh::RtShadows::modeRangeMetres(2) == 35.0f);
    CHECK(sizeof(enh::RtShadowParams) == 80);
}

// ---- SDSM fit math (shadows.md 8.9) ----

namespace {
engine::shadowmaps::SdsmFrame emptySdsm() {
    engine::shadowmaps::SdsmFrame f;
    for (auto& b : f.bounds) {
        b[0] = 1.0f;
        b[1] = -1.0f;
        b[2] = 1.0f;
        b[3] = -1.0f;
    }
    return f;
}
int binOf(const engine::shadowmaps::SdsmFrame& f, float z) {
    return static_cast<int>(std::floor(std::log(z / f.zMin) * static_cast<float>(f.kBins) / std::log(f.zMax / f.zMin)));
}
}  // namespace

TEST(sdsm_merge_takes_the_bins_of_the_slice) {
    namespace sm = engine::shadowmaps;
    sm::SdsmFrame f = emptySdsm();
    const auto put = [&](float z, float x0, float x1, float y0, float y1) {
        float* b = f.bounds[binOf(f, z)];
        b[0] = x0;
        b[1] = x1;
        b[2] = y0;
        b[3] = y1;
    };
    put(100.0f, -10.0f, 10.0f, 0.0f, 5.0f);
    put(500.0f, -50.0f, 20.0f, -8.0f, 30.0f);
    put(5000.0f, -900.0f, 700.0f, -400.0f, 650.0f);
    // a slice 1 .. 1347 sees the first two bins, not the far one
    const sm::sdsm::Bounds a = sm::sdsm::merge(f, 1.0f, 1347.0f);
    CHECK(a.any && a.minX == -50.0f && a.maxX == 20.0f && a.minY == -8.0f && a.maxY == 30.0f);
    CHECK(a.zLo <= 100.0f && a.zHi > 500.0f && a.zHi < 1347.0f * 1.2f);
    // the far slice only the last
    const sm::sdsm::Bounds b = sm::sdsm::merge(f, 2945.0f, 10000.0f);
    CHECK(b.any && b.minX == -900.0f && b.maxY == 650.0f);
    // a slice between the two groups is empty; so is one before the data's range
    CHECK(!sm::sdsm::merge(f, 1200.0f, 2000.0f).any);
    CHECK(!sm::sdsm::merge(f, 0.0f, 5.0f).any);
    // the bin edges are log spaced: edge 0 = zMin, edge kBins = zMax
    CHECK(std::fabs(sm::sdsm::binEdge(f, 0) - f.zMin) < 1e-3f);
    CHECK(std::fabs(sm::sdsm::binEdge(f, sm::SdsmFrame::kBins) - f.zMax) < 1e-1f);
}

TEST(sdsm_extent_steps_and_hysteresis) {
    namespace sm = engine::shadowmaps;
    // the grid: 2^(k / 16), never below the value, within 4.4 % above it
    for (float v : {1.0f, 37.5f, 1234.5f, 31303.0f}) {
        const float q = sm::sdsm::quantUp(v);
        CHECK(q >= v * 0.9999f && q <= v * 1.0443f);
        CHECK(std::fabs(sm::sdsm::quantUp(q) - q) < 1e-4f * q);  // idempotent: a grid value stays
    }
    sm::sdsm::Axis a;
    const float e0 = sm::sdsm::step(a, 1000.0f, 10);
    CHECK(e0 >= 1000.0f);
    // smaller needs keep the extent (the texel grid stays constant) for `calm` frames
    for (int i = 0; i < 9; ++i) CHECK(sm::sdsm::step(a, 800.0f, 10) == e0);
    // the 10th calm frame shrinks it to the grid value above the window's largest need
    const float e1 = sm::sdsm::step(a, 800.0f, 10);
    CHECK(e1 < e0 && e1 >= 800.0f && e1 <= 800.0f * 1.0443f);
    // growth is immediate and resets the window
    const float e2 = sm::sdsm::step(a, 1500.0f, 10);
    CHECK(e2 >= 1500.0f && e2 <= 1500.0f * 1.0443f);
    // a need that wobbles inside the window never shrinks below its peak
    sm::sdsm::Axis w;
    sm::sdsm::step(w, 1000.0f, 6);
    float last = 0.0f;
    for (int i = 0; i < 40; ++i) last = sm::sdsm::step(w, i % 3 == 0 ? 990.0f : 700.0f, 6);
    CHECK(last >= 990.0f);
}

TEST(sdsm_fit_whole_cascade) {
    namespace sm = engine::shadowmaps;
    sm::SdsmFrame d = emptySdsm();
    for (int i = 0; i < 3; ++i) d.lightA[i * 4] = 1.0f;  // identity basis
    d.tanX = 1.0f;
    d.tanY = 0.6f;
    d.axes[0] = d.axes[4] = d.axes[8] = 1.0f;
    float* b = d.bounds[binOf(d, 500.0f)];
    b[0] = -300.0f;
    b[1] = 200.0f;
    b[2] = -100.0f;
    b[3] = 400.0f;
    sm::sdsm::FitIn in;
    in.data = &d;
    in.n = 1.0f;
    in.f = 1347.0f;
    std::memcpy(in.lightA, d.lightA, sizeof in.lightA);
    std::memcpy(in.axes, d.axes, sizeof in.axes);
    in.tanX = d.tanX;
    in.tanY = d.tanY;
    in.size = 4096;
    in.sphereLo[0] = in.sphereLo[1] = -2000.0f;
    in.sphereHi[0] = in.sphereHi[1] = 2000.0f;
    sm::sdsm::Axis ext[2];
    sm::sdsm::FitOut out;
    CHECK(sm::sdsm::fit(in, ext, 90, out));
    // the box contains the visible bounds plus the margin and is smaller than the sphere's
    CHECK(out.lo[0] <= -300.0f - 30.0f && out.hi[0] >= 200.0f + 30.0f && out.lo[1] <= -100.0f - 30.0f && out.hi[1] >= 400.0f + 30.0f);
    CHECK(out.hi[0] - out.lo[0] < 4000.0f && out.hi[1] - out.lo[1] < 4000.0f);
    // the sides are within 2:1 and the centre sits on whole texels of the extent
    const float sx = out.hi[0] - out.lo[0], sy = out.hi[1] - out.lo[1];
    CHECK(sx >= sm::sdsm::kMinAspect * sy * 0.999f && sy >= sm::sdsm::kMinAspect * sx * 0.999f);
    const float texel = sx / 4096.0f, c = 0.5f * (out.lo[0] + out.hi[0]);
    CHECK(std::fabs(c / texel - std::round(c / texel)) < 1e-2f);
    // the same inputs give the same box (the grid does not walk), a camera that moved widens it
    sm::sdsm::FitOut again;
    CHECK(sm::sdsm::fit(in, ext, 90, again) && again.lo[0] == out.lo[0] && again.hi[1] == out.hi[1]);
    in.pos[0] = 100.0f;
    sm::sdsm::FitOut moved;
    CHECK(sm::sdsm::fit(in, ext, 90, moved) && moved.margin > out.margin);
    // fallbacks: a turned sun, another field of view, a camera that jumped, an empty slice
    in.pos[0] = 0.0f;
    sm::sdsm::FitIn sun = in;
    sun.lightA[1] = 0.01f;  // a small turn: still an sdsm fit, the margin grows by turn * 1.5 * f
    sm::sdsm::FitOut turned;
    CHECK(sm::sdsm::fit(sun, ext, 90, turned) && turned.sun > 0.0099f && turned.margin > again.margin + 0.01f * 1.5f * 1347.0f * 0.99f);
    sun.lightA[1] = 0.1f;
    CHECK(!sm::sdsm::fit(sun, ext, 90, out) && out.fail == sm::sdsm::FitFail::Sun);
    sm::sdsm::FitIn fov = in;
    fov.tanX = 1.2f;
    CHECK(!sm::sdsm::fit(fov, ext, 90, out) && out.fail == sm::sdsm::FitFail::Fov);
    sm::sdsm::FitIn jump = in;
    jump.pos[2] = 1000.0f;
    CHECK(!sm::sdsm::fit(jump, ext, 90, out) && out.fail == sm::sdsm::FitFail::Motion);
    sm::sdsm::FitIn empty = in;
    empty.n = 3000.0f;
    empty.f = 9000.0f;
    CHECK(!sm::sdsm::fit(empty, ext, 90, out) && out.fail == sm::sdsm::FitFail::Empty);
}

TEST(sdsm_snap_and_margin) {
    namespace sm = engine::shadowmaps;
    CHECK(std::fabs(sm::sdsm::snap(10.4f, 1.0f) - 10.0f) < 1e-6f);
    CHECK(std::fabs(sm::sdsm::snap(10.6f, 1.0f) - 11.0f) < 1e-6f);
    // the snapped centre is a whole number of texels, whatever the extent (the box of a constant extent walks on one grid)
    const float texel = 1.03f;
    for (float c : {-345.67f, 0.01f, 9999.9f}) {
        const float s = sm::sdsm::snap(c, texel);
        CHECK(std::fabs(s / texel - std::round(s / texel)) < 1e-3f);
    }
    // the margin grows with the camera's motion since the reduction and the slice's far end
    const float still = sm::sdsm::margin(0.0f, 0.0f, 1347.0f, 30.0f, 0.01f);
    const float moved = sm::sdsm::margin(50.0f, 0.05f, 1347.0f, 30.0f, 0.01f);
    CHECK(still > 30.0f && moved > still + 50.0f + 0.05f * 1347.0f - 1.0f);
    // axes: no turn = 0, a quarter turn about y = pi / 2 on the axes that move
    const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const float turn[9] = {0, 0, 1, 0, 1, 0, -1, 0, 0};
    CHECK(sm::sdsm::axesAngle(id, id) < 1e-3f);
    CHECK(std::fabs(sm::sdsm::axesAngle(id, turn) - 1.5707963f) < 1e-3f);
}
