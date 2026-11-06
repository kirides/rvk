#pragma once

// F2c-4-0 measurements (docs/research/frame-ownership.md 7.19; `RVK_F2C4_MEASURE=1` through the Win32 environment, read once: off = one bool test at each site).
// Counters of the owned path (native.owned_static) that decide the design of F2c-4a..c: how many main views a frame has and whether their contents differ,
// whether the view block's c5.xyz / posFix change from frame to frame (jitter), where the 'submit' time of the owned path goes, whether an op's `desc` varies
// with the LOD resource, how many (pipeline, vertex input, dynamic state, index binding) buckets the live records fall into and how many a frame hits, and how
// the compaction's time splits. Written by the engine thread only; `report()` logs one line per 5 s window (rvk.log `native: f2c4 measure (5 s)`).

#include <array>
#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace rvk::f2c4 {

bool enabled();

struct Counters {
    // 1: main views per frame (StaticOps::ownedViewBegin) and the content of their view blocks (z = 0 / solid = 1 pass)
    std::uint64_t curFrame = ~0ull, curViews = 0, frames = 0, views = 0, viewHist[5] = {};
    std::uint64_t passViews[2] = {}, viewDiffers[2] = {};
    std::uint64_t lastHash[2] = {}, lastHashFrame[2] = {~0ull, ~0ull};
    // 2: the view block of a pass against the same pass's block one frame earlier (ViewTable::set, first set of a frame)
    std::uint64_t cmp[2] = {}, chVs0[2] = {}, chVs1[2] = {}, chPs0[2] = {}, chPosFixXy[2] = {}, chPosFixZw[2] = {}, chC5[2] = {}, chAny[2] = {}, chOnlyC5Posfix[2] = {};
    // 3: Device::replayOwned, ns per piece
    std::uint64_t replays = 0, deriveNs = 0, windowNs = 0, observerNs = 0, packetNs = 0, prepNs = 0;
    // 4: contexts of the owned lists: res = ctx + 0x28 (the LOD resource), descSet = a hash of the descs of the context's ops this frame
    struct Ctx {
        const void* res = nullptr;
        std::uint64_t descSet = 0;
        std::uint32_t ops = 0;
    };
    std::unordered_map<const void*, Ctx> ctxCur, ctxPrev;
    std::uint64_t ctxBoth = 0, ctxResChanged = 0, ctxResChangedDescSetDiffers = 0, ctxResChangedDescSetSame = 0, ctxDescSetChangedResSame = 0, ctxNew = 0, ctxOps = 0;
    // 6: buckets (Device::f2c4BucketKey): distinct keys of the records drawn in a frame, over the live records, ids per list
    std::unordered_set<std::uint64_t> frameBuckets;
    std::uint64_t bucketFrame = ~0ull, bucketFrames = 0, bucketHitSum = 0, bucketHitMax = 0, bucketDraws = 0;
    std::uint64_t liveBuckets = 0, liveRecords = 0, liveDraws = 0;
    std::uint64_t maxDrawsPerPass[2] = {}, maxOpsPerPass[2] = {};
    // 7: compaction pieces (FrameTap::compactOwned), ns
    std::uint64_t compactCalls = 0, compactSelectNs = 0, compactRewriteNs = 0, compactSortNs = 0, compactResolveNs = 0, compactTotalNs = 0;
};

Counters& counters();

inline std::uint64_t nsBetween(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}
inline std::uint64_t mix64(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

// frame bookkeeping of items 1 / 4 / 6 (a new frame closes the previous one's histograms)
void beginFrame(std::uint64_t frame);
// logs the window (5 s) and resets it
void report();

}  // namespace rvk::f2c4
