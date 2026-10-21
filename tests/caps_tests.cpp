// Capability report + capability resolution of settings (src/core/caps.*, Registry::setCapabilities; docs/research/raytracing.md 3).

#include <string>

#include "core/caps.h"
#include "core/settings.h"
#include "test.h"

using namespace rvk;
using namespace rvk::settings;
using caps::Cap;

namespace {

caps::Report rtReport(bool hardware, bool enabled) {
    caps::Report r;
    r.known = true;
    r.gpu = "Test GPU";
    r[Cap::RayTracingHardware] = {hardware, hardware, hardware ? "" : "the GPU / driver has no VK_KHR_ray_query", ""};
    r[Cap::RayQuery] = {hardware, enabled,
                        enabled ? "" : hardware ? "disabled by the setting rt.allow" : "the GPU / driver has no VK_KHR_ray_query", ""};
    r[Cap::MemoryBudget] = {true, true, "", ""};
    return r;
}

}  // namespace

TEST(caps_report_mask_and_reasons) {
    const caps::Report unknown;
    CHECK(!unknown.satisfies(caps::bit(Cap::RayQuery)));
    CHECK(unknown.satisfies(0));  // no requirement: always met
    CHECK(unknown.unmetReason(0).empty());
    CHECK(unknown.unmetReason(caps::bit(Cap::RayQuery)) == "no Vulkan device");

    const caps::Report r = rtReport(true, false);
    CHECK(r.enabledMask() == (caps::bit(Cap::RayTracingHardware) | caps::bit(Cap::MemoryBudget)));
    CHECK(r.satisfies(caps::bit(Cap::RayTracingHardware)));
    CHECK(!r.satisfies(caps::bit(Cap::RayQuery)));
    CHECK(r.unmetReason(caps::bit(Cap::RayQuery)) == "Ray query: disabled by the setting rt.allow");
    // several unmet: joined; an unmet one without a reason says "not available"
    const std::string two = r.unmetReason(caps::bit(Cap::RayQuery) | caps::bit(Cap::OpacityMicromap));
    CHECK_MSG(two == "Ray query: disabled by the setting rt.allow; Opacity micromaps: not available", two);
    CHECK(r.unmetReason(1u << 31).find("unknown capability") != std::string::npos);
    CHECK(caps::name(Cap::RayQuery) == "ray_query");
    CHECK(!r.lines().empty() && r.lines().front().find("Test GPU") != std::string::npos);
}

TEST(caps_setting_unavailable_until_device) {
    Registry reg;
    Setting& refl = reg.add({.key = "rt.reflections", .label = "RT reflections", .category = "Ray tracing", .type = Type::Enum,
                             .defaultValue = 0, .options = {"off", "low", "high"},
                             .requirements = caps::bit(Cap::RayQuery)});
    Setting& plain = reg.add({.key = "water.enhanced", .label = "Water", .category = "Water", .type = Type::Bool});
    // no report yet: unavailable, the player's value is kept but the effective value is the default (parity)
    CHECK(!refl.available());
    CHECK(plain.available());
    CHECK(reg.unavailableReason(refl) == "no Vulkan device");
    CHECK(reg.set(refl, 2) == Result::Ok);
    CHECK(refl.value() == 2.0);
    CHECK(refl.effective() == 0.0);
    CHECK(refl.asEnum() == "off");
    CHECK(reg.text(refl) == "high");  // the file / API keep the request
    CHECK(reg.toToml().find("reflections = \"high\"") != std::string::npos);
    reg.dispatch();

    // ray tracing on: available, the request applies, and the change of the effective value is dispatched
    int calls = 0;
    reg.onChange(refl, [&](const Setting& s) {
        ++calls;
        CHECK(s.asEnum() == "high");
    });
    reg.setCapabilities(rtReport(true, true));
    CHECK(refl.available());
    CHECK(refl.asEnum() == "high");
    CHECK(reg.unavailableReason(refl).empty());
    CHECK(reg.dispatch() == 1);
    CHECK(calls == 1);
    // the same report again: no change, no callback
    reg.setCapabilities(rtReport(true, true));
    CHECK(reg.dispatch() == 0);

    // a GPU without it: back to the fallback with the reason
    reg.setCapabilities(rtReport(false, false));
    CHECK(!refl.available());
    CHECK(refl.asEnum() == "off");
    CHECK(reg.unavailableReason(refl) == "Ray query: the GPU / driver has no VK_KHR_ray_query");
    CHECK(reg.capabilities().gpu == "Test GPU");
}

TEST(caps_unmet_value_and_registration_after_report) {
    Registry reg;
    reg.setCapabilities(rtReport(false, false));
    // registered after the report: resolved at once; default on, unmet value off (rt.allow)
    Setting& allow = reg.add({.key = "rt.allow", .label = "Allow", .category = "Ray tracing", .type = Type::Bool,
                              .defaultValue = 1, .flags = Flag::Restart,
                              .requirements = caps::bit(Cap::RayTracingHardware), .unmetValue = 0});
    CHECK(!allow.available());
    CHECK(allow.value() == 1.0);
    CHECK(!allow.asBool());
    CHECK(allow.isDefault());  // the request is still the default: settings.toml stays empty
    CHECK(reg.toToml().find("allow") == std::string::npos);
    reg.setCapabilities(rtReport(true, false));
    CHECK(allow.available());
    CHECK(allow.asBool());
    // an unmet value outside the range is clamped like any value
    Setting& steps = reg.add({.key = "rt.steps", .label = "Steps", .category = "Ray tracing", .type = Type::Int,
                              .defaultValue = 4, .min = 1, .max = 8, .requirements = caps::bit(Cap::RayQuery),
                              .unmetValue = 100});
    CHECK(!steps.available());
    CHECK(steps.asInt() == 8);
}

TEST(caps_renderer_rt_allow_registered) {
    Registry reg;
    const RendererSettings rs = registerRendererSettings(reg);
    CHECK(rs.rtAllow != nullptr);
    CHECK(rs.rtAllow->key() == "rt.allow");
    CHECK(rs.rtAllow->restart());
    CHECK(rs.rtAllow->value() == 1.0);
    CHECK(rs.rtAllow->spec().requirements == caps::bit(Cap::RayTracingHardware));
    // the existing renderer settings have no requirements: unaffected by a missing device
    CHECK(rs.vsync->available() && rs.maxFps->available() && rs.injectView->available());
}
