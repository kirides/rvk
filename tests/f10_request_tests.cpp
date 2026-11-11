// F10-1 (docs/research/frame-ownership.md 14.31): the port of eCRenderContextMesh::RequestRenderOps's op creation, the depth key, the merge of the deferred ops.

#include <vector>

#include "native/f10_request.h"
#include "test.h"

using namespace rvk::native;
using namespace rvk::native::f10req;

namespace {
SubIn sub(bool refracted, bool blended, std::uint32_t sortId) { return SubIn{true, refracted, blended, sortId}; }
const void* const kCtx = reinterpret_cast<const void*>(0x1000);
// the main view's request mask: solid 2 | alpha 4 | z 8 | refracted 0x20 (+ 1 LOD, 0x40 stamp, 0x80 illumination)
constexpr std::uint16_t kMask = 0xef;
}  // namespace

TEST(f10req_opaque_mesh_whole_z_op) {
    // two opaque sub-meshes, fade 1: a solid op each, one whole-mesh z op (desc 0xff) with the first sub-mesh's sort id
    const SubIn s[2] = {sub(false, false, 0x11), sub(false, false, 0x22)};
    std::vector<OpOut> out;
    meshOps(s, 2, kMask, 1.0f, 1.0f, kCtx, 0x123, out);
    CHECK(out.size() == 3);
    CHECK(out[0].list == kListSolid && out[0].op.desc == 0 && out[0].op.fade == 1.0f && out[0].op.sortId == 0x11 && out[0].op.key == 0x123 && out[0].op.context == kCtx);
    CHECK(out[1].list == kListSolid && out[1].op.desc == 1 && out[1].op.sortId == 0x22);
    CHECK(out[2].list == kListZ && out[2].op.desc == 0xff && out[2].op.sortId == 0x11);
}

TEST(f10req_blended_sub_mesh_splits_z_ops) {
    // a blended sub-mesh: an alpha op with the fade, no z op for it; the opaque ones get a z op each (no whole-mesh op)
    const SubIn s[3] = {sub(false, false, 1), sub(false, true, 2), sub(false, false, 3)};
    std::vector<OpOut> out;
    meshOps(s, 3, kMask, 1.0f, 1.0f, kCtx, 7, out);
    CHECK(out.size() == 5);
    CHECK(out[0].list == kListSolid && out[0].op.desc == 0);
    CHECK(out[1].list == kListAlpha && out[1].op.desc == 1);
    CHECK(out[2].list == kListSolid && out[2].op.desc == 2);
    CHECK(out[3].list == kListZ && out[3].op.desc == 0);
    CHECK(out[4].list == kListZ && out[4].op.desc == 2);
}

TEST(f10req_refracted_and_fade) {
    // refracted: only the refracted list; a fade < 1: the opaque sub-mesh becomes an alpha op with the fade, no z ops at all
    const SubIn r[1] = {sub(true, false, 5)};
    std::vector<OpOut> out;
    meshOps(r, 1, kMask, 1.0f, 1.0f, kCtx, 0, out);
    CHECK(out.size() == 1 && out[0].list == kListRefracted);
    out.clear();
    const SubIn o[1] = {sub(false, false, 6)};
    meshOps(o, 1, kMask, 0.5f, 1.0f, kCtx, 0, out);
    CHECK(out.size() == 1 && out[0].list == kListAlpha && out[0].op.fade == 0.5f);
    // a sub-mesh without material: skipped, the whole-mesh z op still comes from the next one
    out.clear();
    const SubIn m[2] = {SubIn{}, sub(false, false, 9)};
    meshOps(m, 2, kMask, 1.0f, 1.0f, kCtx, 0, out);
    CHECK(out.size() == 2 && out[0].list == kListSolid && out[0].op.desc == 1 && out[1].list == kListZ && out[1].op.desc == 0xff && out[1].op.sortId == 9);
}

TEST(f10req_depth_key_clamps) {
    const float plane[4] = {0, 0, 1, 0};
    const float near[4] = {1, 0, 0, 10};
    CHECK(depthKey(near, plane, 2.0f) == 18);  // (10 - 1) * 2
    const float behind[4] = {1, 0, 0, -10};
    CHECK(depthKey(behind, plane, 2.0f) == 0);
    const float far[4] = {0, 0, 0, 1e9f};
    CHECK(depthKey(far, plane, 1.0f) == 0xffffff);
    CHECK(sortIdOf(0x1234, true, 0x13, 3) == ((0x234u) | (0x13u << 12) | (3u << 20)));
    CHECK(sortIdOf(0x1234, false, 0x13, 3) == 0x234u);
}

TEST(f10req_merge_reconstructs_engine_order) {
    // the engine's full list A0 d0 A1 d1 d2 A2 d3: the deferred ops carry the count of engine ops before them
    auto op = [](std::uint32_t id) {
        RawOp o;
        o.desc = id;
        return o;
    };
    const std::vector<RawOp> full = {op(100), op(1), op(101), op(2), op(3), op(102), op(4)};
    const std::vector<std::uint32_t> taken = {1, 3, 4, 6};
    std::vector<RawOp> engine;
    std::vector<Deferred> d;
    splitDeferred(full, taken, engine, d);
    CHECK(engine.size() == 3 && d.size() == 4);
    CHECK(d[0].pos == 1 && d[1].pos == 2 && d[2].pos == 2 && d[3].pos == 3);
    std::vector<RawOp> merged;
    mergeDeferred(engine.data(), static_cast<std::uint32_t>(engine.size()), d, merged);
    CHECK(merged.size() == full.size());
    bool same = true;
    for (std::size_t i = 0; i < full.size(); ++i) same = same && sameBytes(full[i], merged[i]);
    CHECK(same);
    // an empty engine list
    mergeDeferred(nullptr, 0, d, merged);
    CHECK(merged.size() == 4 && merged[0].desc == 1 && merged[3].desc == 4);
}

TEST(f10req_lru_head) {
    int a, b, c;
    std::vector<const void*> head;
    expectedHead({&a, &b, &c, &b}, head);
    CHECK(head.size() == 3 && head[0] == &b && head[1] == &c && head[2] == &a);
}
