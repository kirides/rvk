#pragma once

// Illumination census (docs/research/frame-ownership.md F0, 6.2 step 1): how many light sets / illuminated PSs the engine updates per frame and
// how many distinct light sets / lists the natively composed draws reference. A measurement: installed only with the environment variable
// RVK_ILLUM_CENSUS=1 (read from the Win32 environment); off = no hooks, noteDraw is one relaxed bool load.
// Log: `engine: illum census (5 s): ...` per 5 s.

namespace rvk::engine::illumcensus {

// Creates the detours (enabled with the others by hooks::enableAll); false and no hooks when the variable is not set / a symbol is missing
bool install();

extern bool g_on;

// Called by the native lighting composition for every pass that writes an illumination's lighting (illum = eSShaderMaterialIllumination*,
// list = its light list = the light set + 0x10)
void noteDrawImpl(const void* illum, const void* list);
inline void noteDraw(const void* illum, const void* list) {
    if (g_on) noteDrawImpl(illum, list);
}

// kind of the draws made while alive: 1 = SpeedTree batch, 2 = vegetation (0 = everything else)
void setKind(int kind);
struct KindScope {
    explicit KindScope(int kind) { if (g_on) setKind(kind); }
    ~KindScope() { if (g_on) setKind(0); }
    KindScope(const KindScope&) = delete;
    KindScope& operator=(const KindScope&) = delete;
};

}  // namespace rvk::engine::illumcensus
