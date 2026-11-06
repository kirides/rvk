#include "core/f2c4_measure.h"

#include <algorithm>

#include <spdlog/spdlog.h>

extern "C" __declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char* name, char* buffer, unsigned long size);

namespace rvk::f2c4 {

bool enabled() {
    static const bool on = [] {
        char v[8] = {};
        return GetEnvironmentVariableA("RVK_F2C4_MEASURE", v, sizeof(v)) != 0 && v[0] == '1';
    }();
    return on;
}

Counters& counters() {
    static Counters c;
    return c;
}

void beginFrame(std::uint64_t frame) {
    Counters& c = counters();
    if (frame == c.curFrame) return;
    if (c.curFrame != ~0ull) {
        // the closing frame: views per frame, the contexts' LOD / desc comparison against the frame before it, the bucket hit count
        ++c.frames;
        ++c.viewHist[std::min<std::uint64_t>(c.curViews, 4)];
        for (const auto& [ctx, e] : c.ctxCur) {
            c.ctxOps += e.ops;
            const auto p = c.ctxPrev.find(ctx);
            if (p == c.ctxPrev.end()) {
                ++c.ctxNew;
                continue;
            }
            ++c.ctxBoth;
            if (e.res != p->second.res) {
                ++c.ctxResChanged;
                if (e.descSet != p->second.descSet) ++c.ctxResChangedDescSetDiffers;
                else ++c.ctxResChangedDescSetSame;
            } else if (e.descSet != p->second.descSet) {
                ++c.ctxDescSetChangedResSame;
            }
        }
        c.ctxPrev.swap(c.ctxCur);
        c.ctxCur.clear();
        if (c.bucketFrame != ~0ull) {
            ++c.bucketFrames;
            c.bucketHitSum += c.frameBuckets.size();
            c.bucketHitMax = std::max<std::uint64_t>(c.bucketHitMax, c.frameBuckets.size());
        }
        c.frameBuckets.clear();
    }
    c.curFrame = frame;
    c.bucketFrame = frame;
    c.curViews = 0;
}

void report() {
    Counters& c = counters();
    const auto per = [](std::uint64_t v, std::uint64_t n) { return n != 0 ? static_cast<double>(v) / static_cast<double>(n) : 0.0; };
    const auto pct = [](std::uint64_t a, std::uint64_t b) { return b != 0 ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; };
    spdlog::info(
        "native: f2c4 measure (5 s): (1) main views per frame: {} frames, {:.2f} views per frame (histogram 0 / 1 / 2 / 3 / 4+: {} {} {} {} {}), passes z {} solid {}, a view block differing from the same pass's "
        "earlier block of the frame: z {} solid {}; (2) view block vs the previous frame's [z / solid]: compared {} / {}, changed {} / {} (c0..c11 {} / {}, c12..c30 {} / {}, ps c0..c4 {} / {}, posFix.xy {} / {}, "
        "posFix.zw {} / {}, c5.xyz {} / {}, only c5 / posFix {} / {}); (3) replayOwned per call over {} calls: prep {:.0f} ns, derive {:.0f} ns, observers {:.0f} ns, windows (uploadConstants + served) {:.0f} ns, "
        "packet (DrawBlock, push, scene packet, submit) {:.0f} ns; (4) {} contexts seen in two consecutive frames ({} new): LOD resource changed {} ({} with another desc set, {} with the same), desc set changed with "
        "the same resource {}; ops {}; (6) buckets: live records {} / {} draws -> {} distinct buckets; per frame hit {:.1f} on average, max {} ({} frames); max owned draws per pass z {} solid {}, ops z {} solid {}; "
        "(7) compaction {} calls, per call: select {:.1f} us, list rewrite {:.1f} us, owned-op sort {:.1f} us, resolve {:.1f} us, total {:.1f} us",
        c.frames, per(c.views, c.frames), c.viewHist[0], c.viewHist[1], c.viewHist[2], c.viewHist[3], c.viewHist[4], c.passViews[0], c.passViews[1], c.viewDiffers[0], c.viewDiffers[1], c.cmp[0], c.cmp[1],
        c.chAny[0], c.chAny[1], c.chVs0[0], c.chVs0[1], c.chVs1[0], c.chVs1[1], c.chPs0[0], c.chPs0[1], c.chPosFixXy[0], c.chPosFixXy[1], c.chPosFixZw[0], c.chPosFixZw[1], c.chC5[0], c.chC5[1],
        c.chOnlyC5Posfix[0], c.chOnlyC5Posfix[1], c.replays, per(c.prepNs, c.replays), per(c.deriveNs, c.replays), per(c.observerNs, c.replays), per(c.windowNs, c.replays), per(c.packetNs, c.replays), c.ctxBoth,
        c.ctxNew, c.ctxResChanged, c.ctxResChangedDescSetDiffers, c.ctxResChangedDescSetSame, c.ctxDescSetChangedResSame, c.ctxOps, c.liveRecords, c.liveDraws, c.liveBuckets, per(c.bucketHitSum, c.bucketFrames),
        c.bucketHitMax, c.bucketFrames, c.maxDrawsPerPass[0], c.maxDrawsPerPass[1], c.maxOpsPerPass[0], c.maxOpsPerPass[1], c.compactCalls, per(c.compactSelectNs, c.compactCalls) / 1000.0,
        per(c.compactRewriteNs, c.compactCalls) / 1000.0, per(c.compactSortNs, c.compactCalls) / 1000.0, per(c.compactResolveNs, c.compactCalls) / 1000.0, per(c.compactTotalNs, c.compactCalls) / 1000.0);
    spdlog::info("native: f2c4 measure shares: compaction select {:.0f} %, rewrite {:.0f} %, sort {:.0f} %, resolve {:.0f} %; replayOwned derive {:.0f} %, observers {:.0f} %, windows {:.0f} %, packet {:.0f} %",
                 pct(c.compactSelectNs, c.compactTotalNs), pct(c.compactRewriteNs, c.compactTotalNs), pct(c.compactSortNs, c.compactTotalNs), pct(c.compactResolveNs, c.compactTotalNs),
                 pct(c.deriveNs, c.prepNs + c.deriveNs + c.observerNs + c.windowNs + c.packetNs), pct(c.observerNs, c.prepNs + c.deriveNs + c.observerNs + c.windowNs + c.packetNs),
                 pct(c.windowNs, c.prepNs + c.deriveNs + c.observerNs + c.windowNs + c.packetNs), pct(c.packetNs, c.prepNs + c.deriveNs + c.observerNs + c.windowNs + c.packetNs));
    // the window restarts; the per-context maps and the live totals carry on
    auto prev = std::move(c.ctxPrev);
    auto cur = std::move(c.ctxCur);
    const std::uint64_t curFrame = c.curFrame, curViews = c.curViews, bucketFrame = c.bucketFrame;
    auto buckets = std::move(c.frameBuckets);
    const std::uint64_t lh[2] = {c.lastHash[0], c.lastHash[1]}, lhf[2] = {c.lastHashFrame[0], c.lastHashFrame[1]};
    c = Counters{};
    c.ctxPrev = std::move(prev);
    c.ctxCur = std::move(cur);
    c.curFrame = curFrame;
    c.curViews = curViews;
    c.bucketFrame = bucketFrame;
    c.frameBuckets = std::move(buckets);
    for (int i = 0; i < 2; ++i) {
        c.lastHash[i] = lh[i];
        c.lastHashFrame[i] = lhf[i];
    }
}

}  // namespace rvk::f2c4
