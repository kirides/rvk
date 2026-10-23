// Detail distances (src/engine/lod.*, docs/research/lod.md): the mesh LOD range scaling around eCRenderContextMesh::UpdateLoD.

#include "engine/lod.h"
#include "test.h"

using namespace rvk;

TEST(lod_mesh_ranges_scale_and_restore) {
    namespace lod = engine::lod;
    // 3 LODs: two ranges, the third slot is not a range (the last LOD has none) and stays untouched
    float ranges[3] = {1500.0f, 4000.0f, 123.0f};
    float saved[4]{};
    const int n = lod::scaleRanges(ranges, 3, 2.0f, saved);
    CHECK(n == 2);
    CHECK(ranges[0] == 3000.0f && ranges[1] == 8000.0f && ranges[2] == 123.0f);
    lod::restoreRanges(ranges, n, saved);
    CHECK(ranges[0] == 1500.0f && ranges[1] == 4000.0f && ranges[2] == 123.0f);
    // one LOD: nothing to scale
    float one[1] = {5.0f};
    CHECK(lod::scaleRanges(one, 1, 3.0f, saved) == 0 && one[0] == 5.0f);
    // four LODs: three ranges
    float four[3] = {1.0f, 2.0f, 3.0f};
    CHECK(lod::scaleRanges(four, 4, 1.5f, saved) == 3 && four[2] == 4.5f);
}

TEST(lod_scales_roundtrip) {
    namespace lod = engine::lod;
    const lod::Scales before = lod::scales();
    lod::setScales({2.0f, 1.5f, 1.25f});
    const lod::Scales s = lod::scales();
    CHECK(s.trees == 2.0f && s.meshes == 1.5f && s.view == 1.25f);
    lod::setScales(before);
}
