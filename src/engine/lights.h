#pragma once

// The world's lights (eCIlluminationAdmin, docs/research/rtgi.md 19): every static light the cached-in world holds (the
// admin's sphere tree at +0x68) and every registered dynamic light (the array at +0x38), not only the <= 4 a drawn object
// carries. The GI uses them so lights beyond the engine's per-object reach still light the traced scene. The admin is
// found through the `this` of two of its calls (hooks, Vulkan mode only); the walk runs on the engine's thread with
// SEH-guarded reads.

#include <cstdint>
#include <vector>

namespace rvk::engine::lights {

struct WorldLight {
    float pos[3]{};
    float range = 0.0f;      // the light's radius (the engine's falloff 1 - d^2 / R^2 reaches 0 there)
    float col[3]{};          // diffuse colour x intensity, as the engine hands it to the shaders (c26 + 4 i)
    bool ambient = false;    // an eCStaticAmbientLight_PS (diagnostics: its draw colour is scaled differently)
    bool dynamic = false;    // eCPointLight_PS (flicker, spells, torches carried by the world), else a static light
};

struct Stats {
    std::uint32_t nodes = 0;       // tree nodes visited
    std::uint32_t staticTotal = 0; // static lights in the tree
    std::uint32_t staticNear = 0;  // ... within the query
    std::uint32_t dynamicRaw = 0;   // the admin's registered dynamic lights of any class
    std::uint32_t dynamicTotal = 0;
    std::uint32_t dynamicNear = 0;
    std::uint32_t unknown = 0;     // payloads of an unknown class
    std::uintptr_t dynamicVt[4]{};  // vtables of the first registered dynamic lights (diagnostics)
    bool adminSeen = false;
};

// Appends the lights whose sphere reaches within `radius` of `center` to `out` (cleared first); false while the admin is
// not known yet. Cheap enough per frame (the tree walk is pruned by the spheres)
bool collect(const float center[3], float radius, std::vector<WorldLight>& out, Stats& stats);

// collect() in its two halves (the static half only reads the engine's light tree and payloads; the dynamic half calls the engine's AddToLightList and must stay on the
// engine thread): `collect` = collectStatics then collectDynamics (appended after the statics, same order, same stats)
bool collectStatics(const float center[3], float radius, std::vector<WorldLight>& out, Stats& stats);
bool collectDynamics(const float center[3], float radius, std::vector<WorldLight>& out, Stats& stats);

// WS22 (frame-ownership.md 16.8): collectStatics on a persistent worker thread, kicked when the scene job starts (the previous frame's camera position + a margin; the take filters to the real centre) and taken by the light table at EndZPass. Engine thread only (kick /
// take / configure); exact: the take succeeds only for the same frame, centre and radius, otherwise the caller runs the inline path.
using StaticsHashFn = std::uint64_t (*)(const WorldLight* lights, std::size_t count);  // run on the worker over the statics it collected (the light table's signature chain)
void asyncConfigure(bool on, float radius, StaticsHashFn hash);
bool asyncActive();
void asyncKick(const float center[3], std::uint64_t frame);
// true: `out` / `stats` hold collectStatics' result and `hash` the StaticsHashFn's value; false: nothing taken, run collect()
bool asyncTake(const float center[3], float radius, std::uint64_t frame, std::vector<WorldLight>& out, Stats& stats, std::uint64_t& hash);
struct AsyncCounters {
    std::uint64_t kicks = 0, hits = 0, waits = 0, timedOut = 0, notStarted = 0, noKick = 0, mismatch = 0, discarded = 0, kickBusy = 0, refiltered = 0;
    double waitUs = 0.0, waitMaxUs = 0.0, workerUs = 0.0;  // the take's waits for a running job; the worker's time of the taken jobs
};
AsyncCounters asyncCountersTake();  // reads and resets (engine thread)
void asyncShutdown();               // joins the worker (graceful exit); a later kick needs asyncConfigure(on) again

// The uncached static walk (the version before the cache), for RVK_LIGHTS_VERIFY: appends the static lights within `radius` of `center` to `out` (cleared first)
bool collectStaticsReference(const float center[3], float radius, std::vector<WorldLight>& out);

// WS15 (frame-ownership.md 14.57): the kept static lights re-read in one guarded region (default on); `debug lightsfast 0|1` (overlay command) switches it inside one run
void setFastStatic(bool on);

// Creates the detours that find the admin; false when the symbols are missing
bool install();

}  // namespace rvk::engine::lights
