#pragma once

// F10-0 (docs/research/frame-ownership.md 7.31): the main view's collection done natively, in COMPARE mode only. Enabled by the environment variable
// RVK_F10_COMPARE=1 (read once with GetEnvironmentVariableA at install; off = nothing exists: no hook, no memory). No behaviour change: after the engine's
// eCFrustumDatabase::Request of a view (0x66e8c0: ExtractFromFrustum's element list + the per-element fade / UpdateLoD / RequestRenderOps) the same view is
// collected from a persistent scene (one cache entry per scene-table slot) and the two results are compared:
//   * the extracted set (entity), fade, requested (fade > epsilon and the element's mask), the LOD pick of every mesh context, the hierarchy node set of every
//     (context, sub-mesh) the engine made ops for,
//   * the M3 side-effect snapshot of the engine's visible set (visible stamp, LOD resource state, busy counter, LRU touch, illumination dirty byte): the baseline a
//     takeover has to reproduce.
// Log lines: `native: f10 collect compare (5 s)` (+ per class lines, `native: f10 diff ...` for the first differences, `native: f10 cost (5 s)`,
// `native: f10 side effects (5 s)`). Engine thread only.

#include <cstdint>
#include <vector>

namespace rvk::native::f10 {

// once at start (bootstrap, before hooks::enableAll): the hooks, when RVK_F10_COMPARE=1
bool install();
bool enabled();
// once per present (engine thread): the 5 s report
void onPresent();

// ---- the M3 side-effect snapshot (pure data + diff: unit-tested) ----
// one per visible element the engine requested (fade > epsilon, mask matched): the fields Request / RequestRenderOps / UpdateLoD / UpdateIllumination / CacheResource write
struct SideEffectRec {
    std::uint64_t entity = 0;
    std::uint32_t visibleStamp = 0;   // item +0xc (entity +0xe4) == db +8 when the engine stamped it this frame
    std::uint64_t lodRes = 0;         // sum over the element's mesh contexts of ctx +0x28 (pointer), order independent
    std::uint32_t lodStateMask = 0;   // bit i: the i-th mesh context's LOD resource is resident (state 3)
    std::uint32_t busySum = 0;        // sum of res +0x24 (the busy counter; 0 between calls)
    std::uint32_t touched = 0;        // resources whose +0x20 (last use) equals the cache clock (cache +0x28): the CacheResource touch
    std::uint32_t resources = 0;      // mesh + sub-mesh material resources looked at
    std::uint8_t illumDirty = 0;      // illumination PS +0x188 after the element's request
    bool operator==(const SideEffectRec&) const = default;
};
struct SideEffectDiff {
    std::uint32_t onlyA = 0, onlyB = 0, common = 0;
    std::uint32_t stamp = 0, lod = 0, state = 0, busy = 0, touched = 0, illum = 0;  // records of common entities that differ per field
    bool zero() const { return onlyA == 0 && onlyB == 0 && stamp == 0 && lod == 0 && state == 0 && busy == 0 && touched == 0 && illum == 0; }
};
// compares two snapshots (any order: matched by entity)
SideEffectDiff diffSideEffects(const std::vector<SideEffectRec>& a, const std::vector<SideEffectRec>& b);
std::uint64_t digest(const std::vector<SideEffectRec>& s);  // order independent
// the snapshot of the last compared view (valid until the next Request)
const std::vector<SideEffectRec>& lastSnapshot();

}  // namespace rvk::native::f10
