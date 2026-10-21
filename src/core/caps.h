#pragma once

// Device capabilities beyond the Vulkan floor (docs/PLAN.md section 1) and how player settings resolve against them
// (docs/research/raytracing.md 3). The Vulkan context fills a Report once its device exists; the settings registry
// (core/settings.h, Spec::requires) greys out settings whose requirement is unmet and falls back to their parity value;
// the overlay shows the report ("Capabilities"). Pure: no Vulkan here, unit-tested (tests/caps_tests.cpp).

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rvk::caps {

// What a setting can require. Order = the overlay's order; append only (names are logged and shown).
enum class Cap : std::uint8_t {
    RayTracingHardware,  // the GPU + driver offer acceleration structures + ray query (what `rt.allow` itself needs)
    RayQuery,            // acceleration structures + ray query enabled on this session's device (`rt.allow` was on at start)
    PositionFetch,       // VK_KHR_ray_tracing_position_fetch enabled (hit triangle positions from the BLAS)
    OpacityMicromap,     // VK_EXT_opacity_micromap enabled (alpha-tested foliage in RT)
    RayTracingPipeline,  // VK_KHR_ray_tracing_pipeline exists (information only: never enabled, ray query is the path)
    ExtendedDynamicState3,
    VertexInputDynamicState,
    GraphicsPipelineLibrary,
    Robustness2,
    MemoryBudget,
    DeviceFault,
    SamplerAnisotropy,
    Count
};
inline constexpr std::size_t kCount = static_cast<std::size_t>(Cap::Count);

using Mask = std::uint32_t;
constexpr Mask bit(Cap c) { return 1u << static_cast<unsigned>(c); }

std::string_view name(Cap c);   // stable id, e.g. "ray_query"
std::string_view label(Cap c);  // overlay text, e.g. "Ray query"

struct Entry {
    bool available = false;  // the GPU / driver offers it
    bool enabled = false;    // enabled / in use in this session
    std::string reason;      // why it is not enabled (empty when enabled)
    std::string detail;      // extension names / properties for the report (optional)
};

struct Report {
    bool known = false;  // false until the renderer's Vulkan device exists (passthrough, trace, before InitializeAdmin)
    std::string gpu;     // device name
    std::string driver;  // driver name + info, e.g. "AMD proprietary driver 25.9.1"
    std::string api;     // Vulkan API version of the device, e.g. "1.4.315"
    std::array<Entry, kCount> entries{};
    std::vector<std::pair<std::string, std::string>> properties;  // extra report lines (acceleration structure limits, ...)

    const Entry& operator[](Cap c) const { return entries[static_cast<std::size_t>(c)]; }
    Entry& operator[](Cap c) { return entries[static_cast<std::size_t>(c)]; }
    Mask enabledMask() const;
    bool satisfies(Mask required) const { return required == 0 || (known && (enabledMask() & required) == required); }
    // "" when `required` is met, else why not: "Ray query: the GPU has no VK_KHR_ray_query" (one line per unmet capability,
    // joined with "; "); an unknown report gives "no Vulkan device"
    std::string unmetReason(Mask required) const;
    // the report as log lines (startup log, rvk.log)
    std::vector<std::string> lines() const;
};

// the process's report (set by the renderer when its device exists; any thread)
void setCurrent(Report report);
Report current();

}  // namespace rvk::caps
