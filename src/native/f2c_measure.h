#pragma once

// Frame ownership stage F2c-0 (docs/research/frame-ownership.md 9, 7.12): measurement counters only, no behaviour change.
// Enabled by the environment variable RVK_F2C_MEASURE=1 (read once; off = one bool test at every call site). Render thread only.
//   * owned-eligible share of the main view's static z / solid mesh ops and draws, split by exclusion reason,
//   * the draw observers / replacers active on static main-view draws,
//   * invalidation rates of the inputs a retained op record would be stamped with,
//   * the float registers c12..c31 the static vertex shaders read.
// Log: `native: f2c measure (5 s): ...` every 5 s, `(run)` every 30 s.

#include <cstdint>

namespace rvk::d3d9 {
class Shader;
}

namespace rvk::native::f2c {

namespace detail {
extern bool g_on;
}
inline bool on() { return detail::g_on; }
void init();  // reads the environment once (call at a view begin)

// exclusion reasons of an op (bits; the order is the priority of the exclusive attribution)
enum Reason : std::uint32_t {
    kInstanced,     // a render-instance op drawn by hardware instancing (batches of up to 50: res +0xa0, >= 2 instances, no lightmaps)
    kInstanceList,  // a render-instance op drawn one instance at a time (node 0xffffff, the resource's own instance list, no entity)
    kOverlay,       // entity overlay flags (+0x28 & 0xe00000)
    kFade,          // fade < 1
    kSwitch,        // a material with a switch element
    kNoList,        // solid: no illumination / light list
    kNoTableLit,    // solid: table lighting off (the pass count follows the dynamic light count)
    kHierarchy,     // hierarchy mesh (ctx +0x34 > 0)
    kNodeOp,        // a node op of a hierarchy (desc node != 0)
    kShaderClass,   // the material's shader is not eCShaderDefault / leafs
    kVideo,         // a sampler slot's image carries a video
    kSortId,        // eCShaderDefault +0x2b8 (additional sort id / transformation type) non-zero
    kDrawInherit,   // a draw inherits a stage (texture not ready)
    kDrawNoLight,   // solid: a draw without a lighting image entry (illumination table off / no entry)
    kDrawCpuStream, // a draw with CPU vertex bytes / a non-indexed draw
    kNoRetain,      // the op's composition cannot be retained (on-demand compile)
    kExclusions,
    // information only (never an exclusion): what the records would have to carry
    kInfoMasked = kExclusions,  // alpha-tested material (mask reference, PS c5.w)
    kInfoDepthBias,             // UseDepthBias material (the dbvp view variant)
    kInfoTradeAlpha,            // TradeAlphaAsSolid
    kInfoLightmap,              // the mesh binds lightmaps (BindLightmaps combi)
    kBits
};
const char* name(Reason r);

// ---- ops ----
void opBegin(int list);  // 0 z, 1 solid; a main-view static mesh op is about to be composed
bool opActive();
void bit(Reason r);
void opComposed(bool composed, int skipCode, const char* skipName);
void noteInstances(bool batch, std::uint32_t n);  // a render-instance op with n instances
void engineDraw();                   // an engine draw of the active op (counted when the op was not composed)
void opEnd(std::uint32_t ourDraws);  // the op is finished (next op / view end)
void frameBegin();                   // a main view begins (frame serial for the trackers)

// ---- invalidation trackers ----
enum Tracker : std::uint32_t {
    kLod,          // ctx +0x28 per solid part
    kOverlayFlags, // entity +0x28 & 0xe00000
    kIllumPtr,     // entity +0xf8
    kIllumBytes,   // the illumination record's bytes [0, 0x110) (outdoor factor / light set pointer / static lights ...)
    kIllumDirty,   // the engine's dirty byte (PS + 0x188 = illum + 0x110): states[1] = drawn while dirty
    kSunBit,       // light list +0 (sun bit)
    kHoldsAll,     // hypothetical holdsAll of the light table (1 holds all, 0 a static light unknown, 2 dynamic lights)
    kRegistry,     // LightTable::registryComplete()
    kMatBytes,     // a material shader's VS / PS pool bytes (read at the main view's first use)
    kTexReady,     // a material's sampler images (resource, image data, resolved)
    kLightmap,     // a mesh resource's lightmap slots (res +0x90 / +0x98, residency)
    kTrackers
};
const char* name(Tracker t);
void track(Tracker t, const void* key, std::uint64_t value);
std::uint64_t hashBytes(const void* p, std::size_t n);

// ---- devices ----
// an observer / replacer was shown a static main-view z / solid draw (path: 0 prepareDraw, 1 replayDraw)
void noteObserver(const char* typeName, int path);
void noteReplacer(const char* typeName, int what);  // 0 handles() true, 1 wantsOpaque() true

// ---- CTAB: the vertex shader of a static main-view draw ----
void vsDraw(const d3d9::Shader* vs, bool zPass);

// 5 s tick from StaticOps::report
void report(std::uint64_t frames);

}  // namespace rvk::native::f2c
