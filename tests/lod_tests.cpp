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

TEST(lod_cache_in_radius) {
    namespace lod = engine::lod;
    // ViewRange 7000, bound 300: the game's radius 7300 -> x3 = 300 + 21000
    CHECK(lod::scaledCacheInRadius(7300.0f, 0.0f, 7000.0f, 4000.0f, 3.0f) == 21300.0f);
    // CacheInRange set: the radius is CacheInRange alone -> scaled
    CHECK(lod::scaledCacheInRadius(9000.0f, 9000.0f, 7000.0f, 4000.0f, 2.0f) == 18000.0f);
    // neither: bound + the default range (EntityROI)
    CHECK(lod::scaledCacheInRadius(4100.0f, 0.0f, 0.0f, 4000.0f, 1.5f) == 6100.0f);
    // x1 and no range at all: unchanged
    CHECK(lod::scaledCacheInRadius(7300.0f, 0.0f, 7000.0f, 4000.0f, 1.0f) == 7300.0f);
    CHECK(lod::scaledCacheInRadius(100.0f, 0.0f, 0.0f, 0.0f, 3.0f) == 100.0f);
}

TEST(lod_scales_roundtrip) {
    namespace lod = engine::lod;
    const lod::Scales before = lod::scales();
    lod::setScales({2.0f, 1.5f, 1.25f});
    const lod::Scales s = lod::scales();
    CHECK(s.trees == 2.0f && s.meshes == 1.5f && s.view == 1.25f);
    lod::setScales(before);
}

// engine::fastsort: the CRT qsort's algorithm with inlined comparators (docs/research/performance.md 21)
#include <algorithm>
#include <cstdlib>
#include <random>
#include <vector>

#include "engine/fast_sort.h"

namespace {
struct Rec {
    std::uint64_t a, b;
    std::uint32_t key, sortId;
};
int cmpZ(const void* x, const void* y) {
    const auto *p = static_cast<const Rec*>(x), *q = static_cast<const Rec*>(y);
    return p->key < q->key ? -1 : p->key > q->key ? 1 : 0;
}
}  // namespace

TEST(fastsort_z_matches_crt_qsort_and_is_sorted) {
    std::mt19937 rng(7);
    for (const std::size_t n : {2u, 5u, 8u, 9u, 17u, 100u, 1000u, 5000u}) {
        for (const std::uint32_t range : {3u, 50u, 1000000u}) {
            std::vector<Rec> v(n);
            for (std::size_t i = 0; i < n; ++i) v[i] = {i, ~i, static_cast<std::uint32_t>(rng() % range), static_cast<std::uint32_t>(rng())};
            auto crt = v;
            auto mine = v;
            std::qsort(crt.data(), n, sizeof(Rec), cmpZ);
            rvk::engine::fastsort::sortOps(mine.data(), n, rvk::engine::fastsort::Cmp::Z);
            CHECK(std::is_sorted(mine.begin(), mine.end(), [](const Rec& x, const Rec& y) { return x.key < y.key; }));
            std::vector<std::uint64_t> ids;
            for (const Rec& r : mine) ids.push_back(r.a);
            std::sort(ids.begin(), ids.end());
            for (std::size_t i = 0; i < n; ++i) CHECK(ids[i] == i);
            // (the UCRT's qsort is the same algorithm: equal keys end up in the same order)
            CHECK(std::memcmp(crt.data(), mine.data(), n * sizeof(Rec)) == 0);
        }
    }
}

// engine::loadaccel: the cloud absorption image by sliding windows equals the engine's per-tap loop (docs/research/streaming.md)
#include "engine/load_accel.h"

TEST(absorb_fast_matches_naive) {
    std::mt19937 rng(11);
    struct Dim { std::uint32_t w, h, padPixels; };
    for (const Dim d : {Dim{1, 1, 0}, Dim{7, 5, 0}, Dim{32, 32, 0}, Dim{33, 31, 3}, Dim{64, 40, 0}, Dim{100, 77, 5}, Dim{256, 128, 0}, Dim{128, 256, 0}}) {
        const std::uint32_t pitch = (d.w + d.padPixels) * 4;
        std::vector<std::uint8_t> src(static_cast<std::size_t>(pitch) * d.h), a(src.size(), 0), b(src.size(), 0);
        for (auto& v : src) v = static_cast<std::uint8_t>(rng());
        for (const float scale : {1.0f / 32.0f, 0.03f}) {
            rvk::engine::loadaccel::absorbNaive(src.data(), a.data(), d.w, d.h, pitch, scale);
            rvk::engine::loadaccel::absorbFast(src.data(), b.data(), d.w, d.h, pitch, scale);
            bool same = true;
            for (std::uint32_t y = 0; y < d.h; ++y) same = same && std::memcmp(&a[std::size_t(y) * pitch], &b[std::size_t(y) * pitch], d.w * 4) == 0;
            CHECK(same);
        }
    }
}
