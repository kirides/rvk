// F5-0 geometry (src/core/f5_measure.*, docs/research/frame-ownership.md 7.22): clusters, object-space frustum planes, normal cones, exact triangle classes.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/f5_measure.h"
#include "test.h"

using namespace rvk::f5;

namespace {
// a UV sphere (radius r around the origin), triangles all wound the same way
Mesh sphere(int rings, int segs, float r) {
    Mesh m;
    for (int i = 0; i <= rings; ++i) {
        const float th = 3.14159265f * static_cast<float>(i) / static_cast<float>(rings);
        for (int j = 0; j < segs; ++j) {
            const float ph = 6.2831853f * static_cast<float>(j) / static_cast<float>(segs);
            m.pos.push_back(r * std::sin(th) * std::cos(ph));
            m.pos.push_back(r * std::cos(th));
            m.pos.push_back(r * std::sin(th) * std::sin(ph));
        }
    }
    for (int i = 0; i < rings; ++i) {
        for (int j = 0; j < segs; ++j) {
            const std::uint32_t a = static_cast<std::uint32_t>(i * segs + j), b = static_cast<std::uint32_t>(i * segs + (j + 1) % segs);
            const std::uint32_t c = a + static_cast<std::uint32_t>(segs), d = b + static_cast<std::uint32_t>(segs);
            m.idx.insert(m.idx.end(), {a, b, c, b, d, c});
        }
    }
    return m;
}
// a left-handed perspective camera at the origin looking down +z (the rows of clip = M * (p, 1))
void camera(float regs[4][4], float fov = 1.0f, float n = 1.0f, float f = 1000.0f) {
    const float t = 1.0f / std::tan(fov * 0.5f);
    const float z[4][4] = {{t, 0, 0, 0}, {0, t, 0, 0}, {0, 0, f / (f - n), -f * n / (f - n)}, {0, 0, 1, 0}};
    std::memcpy(regs, z, sizeof(z));
}
}  // namespace

TEST(f5_clusters_cover_every_triangle_with_bounded_size) {
    const Mesh m = sphere(40, 64, 2.0f);
    for (int v = 0; v < kVariants; ++v) {
        const ClusterSet cs = buildClusters(m, static_cast<Variant>(v));
        std::size_t tris = 0;
        for (const Cluster& c : cs.list) {
            CHECK(c.tris >= 1 && c.tris <= 128);
            tris += c.tris;
        }
        CHECK(tris == m.triangles());
        CHECK(cs.tri.size() == m.idx.size());
        CHECK(cs.mortonOrder.size() == cs.list.size());
    }
}

TEST(f5_frustum_and_cone_are_conservative_against_the_exact_classes) {
    const Mesh m = sphere(40, 64, 2.0f);
    const ClusterSet cs = buildClusters(m, kMeshopt);
    float regs[4][4];
    camera(regs);
    const float world[12] = {1, 0, 0, 0.5f, 0, 1, 0, 0.0f, 0, 0, 1, 10.0f};
    ObjView v;
    CHECK(makeObjView(regs, world, 3, 1600, 900, v));
    CHECK(!v.ortho);
    TriExact e;
    evalExact(m, cs.tri.data(), cs.tri.size() / 3, v, e, &cs);
    CHECK(e.tris == m.triangles());
    CHECK(e.frustumOut == 0);
    CHECK(e.backface > e.tris / 3 && e.backface < 2 * e.tris / 3);  // a closed object: about half faces away
    // exactly one winding sign is free of violations, and it culls a real share
    const int good = e.coneViolations[0] == 0 ? 0 : 1;
    CHECK(e.coneViolations[good] == 0);
    CHECK(e.coneViolations[1 - good] > 0 || e.coneBackClusters[1 - good] == 0);
    ClusterEval ce;
    evalClusters(cs, v, ce);
    CHECK(ce.triF == 0);
    CHECK(ce.triBackOnly[good] > 0);
    CHECK(ce.triBackOnly[good] <= e.backface);  // never more than the hardware culls
    CHECK(ce.runs[good][0] >= 1);
    // behind the camera: every cluster is outside the frustum
    const float behind[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -10.0f};
    CHECK(makeObjView(regs, behind, 3, 1600, 900, v));
    ClusterEval cb;
    evalClusters(cs, v, cb);
    CHECK(cb.triF == cb.tris);
    // far to the side: outside too
    const float side[12] = {1, 0, 0, 500.0f, 0, 1, 0, 0, 0, 0, 1, 10.0f};
    CHECK(makeObjView(regs, side, 3, 1600, 900, v));
    ClusterEval cside;
    evalClusters(cs, v, cside);
    CHECK(cside.triF == cside.tris);
}

TEST(f5_cull_none_and_scale_and_histogram) {
    const Mesh m = sphere(20, 32, 1.0f);
    const ClusterSet cs = buildClusters(m, kMeshopt);
    float regs[4][4];
    camera(regs);
    const float world[12] = {3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 10.0f};
    ObjView v;
    CHECK(makeObjView(regs, world, 1, 1600, 900, v));
    TriExact e;
    evalExact(m, cs.tri.data(), cs.tri.size() / 3, v, e, &cs);
    CHECK(e.backface < e.tris / 20);  // double-sided: only the degenerate pole triangles
    std::uint64_t h = 0;
    for (int i = 0; i < kHistBins; ++i) h += e.hist[i];
    CHECK(h == e.visible + e.nearClipped);
    ClusterEval ce;
    evalClusters(cs, v, ce);
    CHECK(ce.triBackOnly[0] == 0 && ce.triBackOnly[1] == 0);
    // far away the triangles become sub-pixel
    const float far[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 900.0f};
    CHECK(makeObjView(regs, far, 1, 320, 180, v));
    TriExact t;
    evalExact(m, m.idx.data(), m.idx.size() / 3, v, t, nullptr);
    CHECK(t.hist[0] > t.visible / 2);
}

TEST(f5_ortho_views_use_the_view_direction) {
    const Mesh m = sphere(40, 64, 2.0f);
    const ClusterSet cs = buildClusters(m, kMeshopt);
    // an orthographic box 20 x 20 x 100 looking down +z: x' = x / 10, y' = y / 10, z' = z / 100, w = 1
    const float regs[4][4] = {{0.1f, 0, 0, 0}, {0, 0.1f, 0, 0}, {0, 0, 0.01f, 0}, {0, 0, 0, 1}};
    const float world[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 30.0f};
    ObjView v;
    CHECK(makeObjView(regs, world, 3, 1024, 1024, v));
    CHECK(v.ortho);
    TriExact e;
    evalExact(m, cs.tri.data(), cs.tri.size() / 3, v, e, &cs);
    const int good = e.coneViolations[0] == 0 ? 0 : 1;
    CHECK(e.coneViolations[good] == 0);
    CHECK(e.backface > e.tris / 3 && e.backface < 2 * e.tris / 3);
    ClusterEval ce;
    evalClusters(cs, v, ce);
    CHECK(ce.triF == 0);
    CHECK(ce.triBackOnly[good] > 0 && ce.triBackOnly[good] <= e.backface);
}
