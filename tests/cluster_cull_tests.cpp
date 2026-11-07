// F5-2 cluster cull, the pure reference (src/core/cluster_cull.*, docs/research/frame-ownership.md 7.34): the frustum test is conservative against the exact
// per-vertex test of a sphere's extreme points, the margin widens it, and the run extraction covers every visible cluster with at most kRunCap runs.

#include <cmath>
#include <cstdint>
#include <vector>

#include "core/cluster_cull.h"
#include "core/f5_measure.h"
#include "test.h"

using namespace rvk::clustercull;

namespace {
// a D3D9-style perspective view: eye at the origin looking down +z, clip rows c0..c3 (clip_i = dot(c_i, (p, 1)) for world p)
void perspective(float regs[4][4], float fovY, float aspect, float zn, float zf) {
    const float f = 1.0f / std::tan(fovY * 0.5f);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) regs[i][j] = 0.0f;
    regs[0][0] = f / aspect;
    regs[1][1] = f;
    regs[2][2] = zf / (zf - zn);
    regs[2][3] = -zn * zf / (zf - zn);
    regs[3][2] = 1.0f;
}
void worldAt(float world[12], float s, float x, float y, float z) {
    for (int i = 0; i < 12; ++i) world[i] = 0.0f;
    world[0] = world[5] = world[10] = s;
    world[3] = x;
    world[7] = y;
    world[11] = z;
}
}  // namespace

TEST(cluster_cull_matches_the_exact_frustum_test_and_only_widens_it) {
    float regs[4][4], world[12];
    perspective(regs, 1.0f, 16.0f / 9.0f, 10.0f, 5000.0f);
    worldAt(world, 2.5f, 100.0f, -30.0f, 400.0f);
    View v, w;
    CHECK(makeView(regs, world, 0.0f, true, v));
    CHECK(makeView(regs, world, kMargin, true, w));
    rvk::f5::ObjView ov;
    CHECK(rvk::f5::makeObjView(regs, world, 3, 1600.0f, 900.0f, ov));
    int outside = 0, widened = 0;
    for (int i = 0; i < 20000; ++i) {
        // a deterministic spread of spheres around the view, in object space
        const float a = static_cast<float>(i) * 0.7548776662f, b = static_cast<float>(i) * 0.5698402909f, c = static_cast<float>(i) * 0.6180339887f;
        const float p[3] = {(a - std::floor(a) - 0.5f) * 3000.0f, (b - std::floor(b) - 0.5f) * 3000.0f, (c - std::floor(c) - 0.3f) * 3000.0f};
        float r = 1.0f + 60.0f * (a * 7.0f - std::floor(a * 7.0f));
        if (i % 4 == 0) r *= 300.0f;  // (spheres of terrain size: they straddle the camera plane)
        const bool exact = rvk::f5::sphereOutside(ov, p, r);
        const bool ours = sphereOutside(v, p, r);
        const bool wide = sphereOutside(w, p, r);
        if (ours) CHECK(exact);        // no margin: outside by our test = outside by the independent exact reference (the far plane is left open here, so not the converse)
        if (wide) CHECK(ours);          // the margin only keeps more
        if (wide) CHECK(exact);         // ... in both directions: culled with the margin = outside by the exact test
        outside += ours ? 1 : 0;
        widened += (ours && !wide) ? 1 : 0;
    }
    CHECK(outside > 1000 && outside < 19000);
    (void)widened;
    CHECK(widened > 0);
    // a sphere well inside is never outside; one behind the eye always is
    const float inside[3] = {40.0f, 0.0f, 120.0f};
    CHECK(!sphereOutside(w, inside, 1.0f));
    const float behind[3] = {0.0f, 0.0f, -300.0f};
    CHECK(sphereOutside(w, behind, 1.0f));
    // side planes only: far away along the view direction is kept
    View s;
    CHECK(makeView(regs, world, kMargin, false, s));
    const float far[3] = {40.0f, 0.0f, 90000.0f};
    CHECK(!sphereOutside(s, far, 1.0f));
    CHECK(!sphereOutside(w, far, 1.0f));  // (the far plane is open)
    // non-finite input: not ok
    float bad[4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) bad[i][j] = regs[i][j];
    bad[0][0] = NAN;
    CHECK(!makeView(bad, world, kMargin, true, s));
}

TEST(cluster_cull_runs_cover_every_visible_cluster_within_the_cap) {
    std::uint32_t seed = 12345u;
    const auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    for (int t = 0; t < 2000; ++t) {
        const std::uint32_t rows = 1 + rnd() % 300;
        std::vector<std::uint32_t> bits((rows + 31) / 32, 0u);
        const std::uint32_t density = 1 + rnd() % 6;
        std::uint32_t visible = 0;
        for (std::uint32_t i = 0; i < rows; ++i) {
            if (rnd() % 7 < density) {
                bits[i >> 5] |= 1u << (i & 31u);
                ++visible;
            }
        }
        Run runs[kRunCap];
        const std::uint32_t n = extractRuns(bits.data(), rows, kRunCap, runs);
        CHECK(n <= kRunCap);
        CHECK((n == 0) == (visible == 0));
        std::vector<bool> covered(rows, false);
        std::uint32_t prevEnd = 0;
        for (std::uint32_t k = 0; k < n; ++k) {
            CHECK(runs[k].first < runs[k].end && runs[k].end <= rows);
            CHECK(runs[k].first >= prevEnd);  // ordered, disjoint
            prevEnd = runs[k].end;
            for (std::uint32_t i = runs[k].first; i < runs[k].end; ++i) covered[i] = true;
            CHECK((bits[runs[k].first >> 5] >> (runs[k].first & 31u) & 1u) != 0);  // a run starts on a visible cluster
        }
        for (std::uint32_t i = 0; i < rows; ++i) {
            if ((bits[i >> 5] >> (i & 31u) & 1u) != 0) CHECK(covered[i]);
        }
        if (n != 0) {
            // below the cap the runs are exact (no invisible cluster inside, none before / after)
            std::uint32_t exactRuns = 0;
            for (std::uint32_t i = 0; i < rows; ++i) {
                const bool v = (bits[i >> 5] >> (i & 31u) & 1u) != 0;
                const bool pv = i > 0 && (bits[(i - 1) >> 5] >> ((i - 1) & 31u) & 1u) != 0;
                if (v && !pv) ++exactRuns;
            }
            if (exactRuns <= kRunCap) {
                CHECK(n == exactRuns);
                for (std::uint32_t k = 0; k < n; ++k) {
                    for (std::uint32_t i = runs[k].first; i < runs[k].end; ++i) CHECK((bits[i >> 5] >> (i & 31u) & 1u) != 0);
                }
            } else {
                CHECK(n == kRunCap);
            }
        }
    }
}
