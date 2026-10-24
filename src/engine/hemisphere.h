#pragma once

// The engine's hemisphere light (eCHemisphere_PS, docs/research/rtgi.md 17): the weather-driven sky ambient every lit object
// starts from (SH * f + static SH, L0 += (1 - f) * indoor: FUN_1806b5bb0). A post-hook of its AddToLightList reads what the
// engine hands every light set: the SH folded into the 7 shader registers (c15..c21 layout, f = 1) and the indoor colour.
// Vulkan mode only (installed with the scene hooks); the detour runs on the engine's thread, readers on any thread.

#include <cstdint>

namespace rvk::engine::hemisphere {

struct Light {
    float regs[7][4]{};      // g_AmbientLight of an outdoor object (f = 1, no static lights): c15..c21
    float indoor[3]{};       // eCHemisphere_PS IndoorAmbientColor
    float intensity = 0.0f;  // the SH's intensity factor (this +0x94)
    const void* object = nullptr;
    std::uint64_t calls = 0;  // AddToLightList calls so far (all hemispheres)
    std::uint32_t objects = 0;  // distinct hemisphere objects seen
};

// the latest hemisphere light; false until one was seen
bool latest(Light& out);

// Creates the detour (enabled with the others by hooks::enableAll); false when the symbol is missing
bool install();

}  // namespace rvk::engine::hemisphere
