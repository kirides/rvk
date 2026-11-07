// F10-0 (docs/research/frame-ownership.md 7.31): the engine's collection math (cull / fade / LOD / node walk) and the side-effect diff, pure.

#include <cmath>
#include <vector>

#include "native/f10_collect.h"
#include "native/f10_math.h"
#include "test.h"

using namespace rvk::native::f10;

namespace {
// a beam tree of two planes (x >= 0, then y >= 0); selector 0 = a plane whose normal is positive in every component
struct Tree {
    BeamNode n1{1, 0, nullptr, nullptr};
    BeamNode n0{0, 0, nullptr, &n1};
    BeamPlane planes[2] = {{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}};
    std::uint32_t in[3] = {0, 1, 2};   // the corner that is most inside: the box minimum
    std::uint32_t out[3] = {3, 4, 5};  // the box maximum
    Beam beam() const { return {&n0, planes, in, out}; }
};
}  // namespace

TEST(f10_beam_cull_box_three_ways) {
    Tree t;
    const float inside[6] = {1, 1, -5, 2, 2, 5};
    const float crossing[6] = {-1, 1, -5, 2, 2, 5};
    const float outside[6] = {-3, 1, -5, -1, 2, 5};
    const float outsideY[6] = {1, -3, -5, 2, -1, 5};
    CHECK(beamCullBox(t.beam(), inside) == kInside);
    CHECK(beamCullBox(t.beam(), crossing) == kPartial);
    CHECK(beamCullBox(t.beam(), outside) == kCulled);
    CHECK(beamCullBox(t.beam(), outsideY) == kCulled);
    CHECK(beamCullBox(Beam{}, inside) == kCulled);  // no tree: nothing is visible (the engine's root == 0)
}

TEST(f10_beam_cull_sphere_three_ways) {
    Tree t;
    const float in[3] = {5, 5, 0}, edge[3] = {0.5f, 5, 0}, out[3] = {-5, 5, 0};
    CHECK(beamCullSphere(t.beam(), 1.0f, in) == kInside);
    CHECK(beamCullSphere(t.beam(), 1.0f, edge) == kPartial);
    CHECK(beamCullSphere(t.beam(), 1.0f, out) == kCulled);
}

TEST(f10_camera_distance_and_full_cull) {
    Tree t;
    Filter f;
    f.beam = t.beam();
    f.sphereRadius = 1000.0f;
    f.camera[0] = f.camera[1] = f.camera[2] = 0.0f;
    const float c[3] = {50, 50, 0};
    CHECK(cullCameraSphere(f, 1.0f, c));
    const float far[3] = {5000, 0, 0};
    CHECK(!cullCameraSphere(f, 1.0f, far));
    CHECK(cullCameraDistance(f, 1.0f, c, 100.0f) == kInside);
    CHECK(cullCameraDistance(f, 1.0f, c, 70.0f) == kPartial);  // |c| = 70.7: the edge of the range sphere crosses it
    CHECK(cullCameraDistance(f, 1.0f, c, 10.0f) == kCulled);
    const float box[6] = {49, 49, -1, 51, 51, 1};
    CHECK(cullCameraFull(f, box, 1.0f, c, 0.0f) == kInside);
    CHECK(cullCameraFull(f, box, 1.0f, c, 10.0f) == kCulled);
    CHECK(cullCameraFull(f, box, 1.0f, c, 70.0f) == kPartial);  // the distance's partial wins over an inside beam result
    // box test: AABB around the frustum sphere first
    f.aabbMin[0] = f.aabbMin[1] = f.aabbMin[2] = -1000;
    f.aabbMax[0] = f.aabbMax[1] = f.aabbMax[2] = 1000;
    CHECK(cullCameraBox(f, box) == kInside);
    const float farBox[6] = {2000, 0, 0, 2001, 1, 1};
    CHECK(cullCameraBox(f, farBox) == kCulled);
}

TEST(f10_entity_gate_and_request_mask) {
    CHECK(entityExtractable(0x8026));
    CHECK(!entityExtractable(0x8026 | 8));
    CHECK(!entityExtractable(0x0026));
    CHECK(requestMask(false, false, false, false, false, false, false) == 2);
    CHECK(requestMask(true, false, false, false, false, false, false) == 6);
    CHECK(requestMask(true, true, true, true, true, true, true) == (6 | 0x20 | 1 | 0x40 | 0x80 | 8 | 0x100));
}

TEST(f10_fade_matches_the_engine_rule) {
    const float cam[3] = {0, 0, 0}, ctr[3] = {0, 0, 50};
    const float eps = 1e-6f;
    // no range / range 0 / noRange byte: the entity's own fade
    CHECK(calculateFade(0.5f, 0.0f, 1.0f, false, cam, ctr, eps, 0.8f, 1.0f) == 0.5f);
    CHECK(calculateFade(0.5f, 100.0f, 1.0f, true, cam, ctr, eps, 0.8f, 1.0f) == 0.5f);
    // inside 80 % of the range: 1; beyond the range: 0; in between linear
    CHECK(calculateFade(1.0f, 100.0f, 1.0f, false, cam, ctr, eps, 0.8f, 1.0f) == 1.0f);
    CHECK(calculateFade(1.0f, 40.0f, 1.0f, false, cam, ctr, eps, 0.8f, 1.0f) == 0.0f);
    const float f = calculateFade(1.0f, 55.55f, 1.0f, false, cam, ctr, eps, 0.8f, 1.0f);
    CHECK(f > 0.0f && f < 1.0f);
    // the view range factor multiplies the range (lod.view)
    CHECK(calculateFade(1.0f, 40.0f, 2.0f, false, cam, ctr, eps, 0.8f, 1.0f) == 1.0f);
}

TEST(f10_lod_pick_follows_updatelod) {
    // 3 LODs, ranges 100 / 300 (squared in the compare), resident: all
    const float ranges[2] = {100.0f, 300.0f};
    const float cam[3] = {0, 0, 0};
    auto all = [](int) { return true; };
    auto at = [&](float dist) {
        const float c[3] = {dist, 0, 0};
        return pickLod(3, ranges, cam, c, INFINITY, all);
    };
    CHECK(at(50.0f).chosen == 0);
    CHECK(at(200.0f).chosen == 1);
    CHECK(at(500.0f).chosen == 2);
    CHECK(at(50.0f).wanted == 0 && at(500.0f).wanted == 2);
    // the wanted LOD is not resident: the nearest resident one at or beyond it stays chosen (the highest index that holds the distance, else the first resident)
    auto only2 = [](int i) { return i == 2; };
    const float c[3] = {50.0f, 0, 0};
    const LodPick p = pickLod(3, ranges, cam, c, INFINITY, only2);
    CHECK(p.chosen == 2 && p.wanted == 0);
    auto none = [](int) { return false; };
    CHECK(pickLod(3, ranges, cam, c, INFINITY, none).chosen == -1);
    CHECK(pickLod(1, ranges, cam, c, INFINITY, all).chosen == -1);  // single LOD: UpdateLoD does nothing
}

TEST(f10_node_walk_emits_leaves_and_whole_subtrees) {
    // pre-order table: node 0 = root over nodes 1, 2 (leaves) and node 3 (a leaf), end sentinel 4
    RawNode n[5]{};
    auto box = [](RawNode& r, float x0, float x1) {
        r.box[0] = x0; r.box[1] = 0; r.box[2] = 0; r.box[3] = x1; r.box[4] = 1; r.box[5] = 1;
    };
    n[0].skip = &n[4]; box(n[0], 0, 30); n[0].ordinal = 0;
    n[1].skip = &n[2]; box(n[1], 0, 10); n[1].ordinal = 0;
    n[2].skip = &n[3]; box(n[2], 10, 20); n[2].ordinal = 5;
    n[3].skip = &n[4]; box(n[3], 20, 30); n[3].ordinal = 10;
    n[4].skip = nullptr; n[4].ordinal = 20;
    // a camera that sees boxes with x < 15
    auto cull = [](const float* b) { return b[3] <= 15 ? kInside : b[0] >= 15 ? kCulled : kPartial; };
    std::vector<std::uint32_t> got;
    walkNodes(n, 2, cull, [&](std::uint32_t i) { got.push_back(i); });
    // the root straddles (partial); its range 20 >= 2 * 3: descend; node 1 inside (leaf), node 2 straddles (leaf: emitted), node 3 culled
    CHECK(got == (std::vector<std::uint32_t>{1, 2}));
    // everything inside: the root is emitted as a whole
    got.clear();
    walkNodes(n, 2, [](const float*) { return kInside; }, [&](std::uint32_t i) { got.push_back(i); });
    CHECK(got == (std::vector<std::uint32_t>{0}));
    // partial and small (range 20 < level 10 * 3 = 30): emitted whole
    got.clear();
    walkNodes(n, 10, [](const float*) { return kPartial; }, [&](std::uint32_t i) { got.push_back(i); });
    CHECK(got == (std::vector<std::uint32_t>{0}));
    // everything culled
    got.clear();
    walkNodes(n, 2, [](const float*) { return kCulled; }, [&](std::uint32_t i) { got.push_back(i); });
    CHECK(got.empty());
}

TEST(f10_side_effect_diff_matches_by_entity) {
    std::vector<SideEffectRec> a(3), b;
    for (std::uint64_t i = 0; i < 3; ++i) {
        a[i].entity = 100 + i;
        a[i].visibleStamp = 1;
        a[i].lodRes = 7 * (i + 1);
        a[i].lodStateMask = 1;
        a[i].touched = 2;
        a[i].resources = 2;
    }
    b = {a[2], a[0], a[1]};  // another order
    CHECK(diffSideEffects(a, b).zero());
    CHECK(digest(a) == digest(b));
    b[1].lodRes = 99;
    b[2].illumDirty = 1;
    b.push_back(SideEffectRec{.entity = 500});
    a.push_back(SideEffectRec{.entity = 600});
    const SideEffectDiff d = diffSideEffects(a, b);
    CHECK(d.common == 3 && d.lod == 1 && d.illum == 1 && d.onlyA == 1 && d.onlyB == 1 && !d.zero());
    CHECK(digest(a) != digest(b));
}
