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
    FullScreenExclusive,  // VK_EXT_full_screen_exclusive + VK_KHR_get_surface_capabilities2 (vulkan.fullscreen = exclusive)
    ShaderDrawParameters,  // shaderDrawParameters (gl_DrawID): merged indirect draws (vulkanize plan.md V4)
    DrawIndirectCount,     // drawIndirectCount: GPU-written draw counts (vulkanize plan.md V4)
    FrameGeneration,       // AMD frame generation (FSR 3): the effect DLL is installed and the API offers a provider on this device
    FrameGenerationMl,     // ... the ML provider (FSR 4 frame generation: Radeon RX 9000 and newer)
    FrameGenerationQueues, // the extra queues for the frame generation swapchain were created (framegen.mode != off at start)
    ClusterAccelerationStructure,  // VK_NV_cluster_acceleration_structure offered (clustered geometry; information only, never enabled)
    RayTracingReorder,             // VK_EXT / VK_NV_ray_tracing_invocation_reorder offered (shader execution reordering; information only)
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
    bool integratedGpu = false;  // the device shares the PC's memory (Vulkan: VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU; D3D12: <= 512 MiB dedicated)
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

// ---- Low bandwidth mode (vulkan.low_bandwidth; docs/research/low-bandwidth.md) ----
// The decision lives in ONE function so another criterion (video memory size, ...) can be added later.
struct LowBandwidth {
    bool on = false;
    bool automatic = false;  // the setting is "auto"
    std::string reason;      // "integrated GPU", "discrete GPU", "set by the player", "no device yet"
};
// mode: "auto" | "off" | "on" (anything else = auto). Pure.
LowBandwidth resolveLowBandwidth(std::string_view mode, const Report& report);
// the session's decision (any thread, lock-free read); the features read this where they create their images / configure their passes
bool lowBandwidth();
// the same for one item (1 AO, 2 screen probes, 8 material RT, 16 water RT (4 = lamp shadows was tried and dropped: docs/research/low-bandwidth.md)); developer env RVK_LOWBW_OFF=<mask> leaves items out
bool lowBandwidth(unsigned item);
LowBandwidth lowBandwidthDecision();
// stores the decision; true when it changed
bool setLowBandwidth(const LowBandwidth& decision);

// the process's report (set by the renderer when its device exists; any thread)
void setCurrent(Report report);
Report current();

}  // namespace rvk::caps
