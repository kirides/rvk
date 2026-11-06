#include "core/caps.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <format>
#include <mutex>

namespace rvk::caps {
namespace {

struct Names {
    std::string_view name;
    std::string_view label;
};
constexpr std::array<Names, kCount> kNames = {{
    {"ray_tracing_hardware", "Ray tracing hardware"},
    {"ray_query", "Ray query"},
    {"position_fetch", "Hit position fetch"},
    {"opacity_micromap", "Opacity micromaps"},
    {"ray_tracing_pipeline", "Ray tracing pipelines"},
    {"extended_dynamic_state3", "Extended dynamic state 3"},
    {"vertex_input_dynamic_state", "Vertex input dynamic state"},
    {"graphics_pipeline_library", "Graphics pipeline libraries"},
    {"robustness2", "Robustness 2 (null descriptors)"},
    {"memory_budget", "Memory budget"},
    {"device_fault", "Device fault reports"},
    {"sampler_anisotropy", "Anisotropic filtering"},
    {"full_screen_exclusive", "Exclusive fullscreen"},
    {"shader_draw_parameters", "Shader draw parameters"},
    {"draw_indirect_count", "Indirect draw count"},
    {"frame_generation", "Frame generation (FSR 3)"},
    {"frame_generation_ml", "Frame generation (FSR 4)"},
    {"frame_generation_queues", "Frame generation queues"},
    {"cluster_acceleration_structure", "Clustered geometry (mega geometry)"},
    {"ray_tracing_reorder", "Shader execution reordering"},
}};

std::mutex g_mutex;
Report g_current;
std::atomic<bool> g_lowBandwidth{false};
LowBandwidth g_lowBandwidthDecision;

}  // namespace

std::string_view name(Cap c) {
    const auto i = static_cast<std::size_t>(c);
    return i < kCount ? kNames[i].name : std::string_view("?");
}

std::string_view label(Cap c) {
    const auto i = static_cast<std::size_t>(c);
    return i < kCount ? kNames[i].label : std::string_view("?");
}

Mask Report::enabledMask() const {
    Mask m = 0;
    if (!known) return 0;
    for (std::size_t i = 0; i < kCount; ++i) {
        if (entries[i].enabled) m |= bit(static_cast<Cap>(i));
    }
    return m;
}

std::string Report::unmetReason(Mask required) const {
    if (required == 0) return {};
    if (!known) return "no Vulkan device";
    std::string out;
    for (std::size_t i = 0; i < kCount; ++i) {
        const Cap c = static_cast<Cap>(i);
        if ((required & bit(c)) == 0 || entries[i].enabled) continue;
        const std::string& why = entries[i].reason;
        out += std::format("{}{}: {}", out.empty() ? "" : "; ", label(c), why.empty() ? "not available" : why);
    }
    // bits beyond Count: a requirement this build does not know
    if ((required >> kCount) != 0) out += std::format("{}unknown capability bits 0x{:x}", out.empty() ? "" : "; ", required >> kCount);
    return out;
}

std::vector<std::string> Report::lines() const {
    std::vector<std::string> out;
    if (!known) {
        out.emplace_back("no Vulkan device");
        return out;
    }
    out.push_back(std::format("{} | driver {} | Vulkan {}", gpu, driver, api));
    for (std::size_t i = 0; i < kCount; ++i) {
        const Entry& e = entries[i];
        std::string l = std::format("  {:<30} {}", label(static_cast<Cap>(i)),
                                    e.enabled ? "enabled" : e.available ? "available, off" : "unavailable");
        if (!e.enabled && !e.reason.empty()) l += std::format(" ({})", e.reason);
        if (!e.detail.empty()) l += std::format(" [{}]", e.detail);
        out.push_back(std::move(l));
    }
    for (const auto& [k, v] : properties) out.push_back(std::format("  {}: {}", k, v));
    return out;
}

void setCurrent(Report report) {
    const std::lock_guard lock(g_mutex);
    g_current = std::move(report);
}

Report current() {
    const std::lock_guard lock(g_mutex);
    return g_current;
}

LowBandwidth resolveLowBandwidth(std::string_view mode, const Report& report) {
    LowBandwidth d;
    if (mode == "on") {
        d.on = true;
        d.reason = "set by the player";
    } else if (mode == "off") {
        d.reason = "set by the player";
    } else {
        d.automatic = true;
        if (!report.known) {
            d.reason = "no device yet";
        } else {
            d.on = report.integratedGpu;  // the one criterion for now: integrated GPUs share the PC's memory
            d.reason = report.integratedGpu ? "integrated GPU" : "discrete GPU";
        }
    }
    return d;
}

bool lowBandwidth() { return g_lowBandwidth.load(std::memory_order_relaxed); }

bool lowBandwidth(unsigned item) {
    // -Environment 'RVK_LOWBW_OFF=<mask>' leaves single items out (bisecting pictures): 1 AO, 2 screen probes, 8 material RT, 16 water RT (4 = lamp shadows was tried and dropped: docs/research/low-bandwidth.md)
    static const unsigned off = [] {
        char buf[16] = {};
        return GetEnvironmentVariableA("RVK_LOWBW_OFF", buf, sizeof(buf)) > 0 ? static_cast<unsigned>(std::strtoul(buf, nullptr, 0)) : 0u;
    }();
    return lowBandwidth() && (off & item) == 0;
}

LowBandwidth lowBandwidthDecision() {
    const std::lock_guard lock(g_mutex);
    return g_lowBandwidthDecision;
}

bool setLowBandwidth(const LowBandwidth& decision) {
    const std::lock_guard lock(g_mutex);
    const bool changed = g_lowBandwidth.load() != decision.on || g_lowBandwidthDecision.reason != decision.reason ||
                         g_lowBandwidthDecision.automatic != decision.automatic;
    g_lowBandwidthDecision = decision;
    g_lowBandwidth.store(decision.on, std::memory_order_relaxed);
    return changed;
}

}  // namespace rvk::caps
