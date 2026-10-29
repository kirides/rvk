// Tool API for settings (rvk_setting_*, rvk_ab: src/api/settings_api.*) on a private registry.

#include <cstring>
#include <string>

#include "api/settings_api.h"
#include "core/settings.h"
#include "rvk_api.h"
#include "test.h"

using namespace rvk;

TEST(settings_api_info_get_set) {
    settings::Registry reg;
    settings::registerRendererSettings(reg);
    const std::uint32_t n = api::settingCount(reg);
    CHECK(n == 115);  // 32 (debug.split_constants + debug.split_verify, debug.constant_compare, incl. renderer.mode, overlay.debug_mode, overlay.show_advanced, vulkan.fullscreen, vulkan.exclusive_acquire, vulkan.menu_fps, debug.pace_loading, debug.batch_draws, debug.record_threads, debug.prefetch_stride, debug.material_sources, debug.ubershader, debug.ubershader_skip, debug.shadow_stride, rt.reflections, rt.reflection_tweaks, rt.debug, rt.scene_views) + the 11 water.* + the 15 shadows.* + the 11 gi.* + the 5 ao.* / gbuffer.* + the 3 lod.* settings + debug.rt_shadow_sync + debug.rt_skin + debug.rt_skin_verify + compat.high_fps + debug.regions + upscale.mode + upscale.provider + upscale.object_motion + upscale.reactive + upscale.reactive_cutoff + upscale.reactive_scale + upscale.sharpness + upscale.exposure + upscale.jitter_flip + upscale.post_full_size + upscale.alpha_dither + upscale.cutout_thicken + native.frame_tap + native.static_ops + native.static_ops_compare + native.scene_ops + native.scene_ops_compare + native.scene_ops_groups + native.mirror + native.coverage + native.lean + native.lean_check + native.lean_restore + debug.reverse_z + debug.names + debug.crash_markers + framegen.mode + framegen.manual + framegen.debug
    struct rvk_setting_info info{};
    CHECK(api::settingInfo(reg, 0, &info, sizeof(info)) == 80);
    CHECK(info.size == 80);
    CHECK(std::string(info.key) == "vulkan.vsync");
    CHECK(info.type == RVK_SETTING_ENUM);
    CHECK(info.option_count == 3);
    CHECK(std::string(info.options) == "engine\non\noff\n");
    CHECK(api::settingInfo(reg, n, &info, sizeof(info)) == RVK_SETTING_BAD_ARGUMENT);
    // an older, smaller struct gets only its prefix
    struct rvk_setting_info small{};
    CHECK(api::settingInfo(reg, 1, &small, 16) == 16);
    CHECK(small.size == 16 && small.key == nullptr);

    char buf[16];
    CHECK(api::settingGet(reg, "vulkan.max_fps", buf, sizeof(buf)) == 1);
    CHECK(std::string(buf) == "0");
    CHECK(api::settingSet(reg, "vulkan.max_fps", "60", 0) == RVK_SETTING_OK);
    CHECK(api::settingGet(reg, "vulkan.max_fps", buf, sizeof(buf)) == 2);
    CHECK(std::string(buf) == "60");
    CHECK(api::settingSet(reg, "vulkan.vsync", "off", 0) == RVK_SETTING_OK);
    CHECK(api::settingGet(reg, "vulkan.vsync", buf, 3) == 3);  // truncated: "of"
    CHECK(std::string(buf) == "of");
    CHECK(api::settingSet(reg, "vulkan.vsync", "sometimes", 0) == RVK_SETTING_BAD_VALUE);
    CHECK(api::settingSet(reg, "vulkan.nope", "1", 0) == RVK_SETTING_UNKNOWN_KEY);
    CHECK(api::settingGet(reg, "vulkan.nope", buf, sizeof(buf)) == RVK_SETTING_UNKNOWN_KEY);
    CHECK(api::settingSet(reg, "vulkan.vsync", "on", 1) == RVK_SETTING_BAD_ARGUMENT);
    CHECK(api::settingSet(reg, nullptr, "on", 0) == RVK_SETTING_BAD_ARGUMENT);
    reg.lock(*reg.find("vulkan.adapter"), 0, "test");
    CHECK(api::settingSet(reg, "vulkan.adapter", "1", 0) == RVK_SETTING_LOCKED);
    CHECK(api::settingInfo(reg, 9, &info, sizeof(info)) == 80);  // vulkan.adapter (System tab, after renderer.mode and the menu settings)
    CHECK((info.flags & (RVK_SETTING_RESTART | RVK_SETTING_IS_LOCKED)) == (RVK_SETTING_RESTART | RVK_SETTING_IS_LOCKED));
    // capability resolution: rt.allow needs ray tracing hardware; this registry has no device report
    CHECK(api::settingInfo(reg, 11, &info, sizeof(info)) == 80);
    CHECK(std::string(info.key) == "rt.allow");
    CHECK((info.flags & RVK_SETTING_UNAVAILABLE) != 0);
    CHECK(api::settingGet(reg, "rt.allow", buf, sizeof(buf)) == 4);  // the request ("true") is kept
}

TEST(settings_api_ab) {
    settings::Registry reg;
    settings::registerRendererSettings(reg);
    CHECK(api::settingSet(reg, "vulkan.max_fps", "45", 0) == RVK_SETTING_OK);
    CHECK(api::ab(reg, RVK_AB_QUERY) == 0);
    CHECK(api::ab(reg, RVK_AB_FLIP) == 1);
    char buf[16];
    api::settingGet(reg, "vulkan.max_fps", buf, sizeof(buf));
    CHECK(std::string(buf) == "0");  // side B = defaults
    CHECK(api::ab(reg, RVK_AB_SIDE_B) == 1);
    CHECK(api::ab(reg, RVK_AB_SIDE_A) == 0);
    api::settingGet(reg, "vulkan.max_fps", buf, sizeof(buf));
    CHECK(std::string(buf) == "45");
    CHECK(api::ab(reg, RVK_AB_COPY) == 0);
    CHECK(reg.abDifferences().empty());
    CHECK(api::ab(reg, RVK_AB_RESET) == 0);
    CHECK(reg.abDifferences().size() == 1);
    CHECK(api::ab(reg, 99) == RVK_SETTING_BAD_ARGUMENT);
}

// settings of earlier builds that were removed (native.opaque / native.shadows / native.debug, rt.source: the early native prototypes):
// an old settings.toml loads, one warning per key (also for the A/B twin), the rest of the file applies, the next save leaves them out
TEST(settings_removed_keys_load_and_drop) {
    settings::Registry reg;
    settings::registerRendererSettings(reg);
    std::vector<std::string> warnings;
    CHECK(reg.fromToml(R"(
[native]
opaque = true
shadows = true
debug = "albedo"
[rt]
source = "mirror"
reflections = "high"
[ab.native]
opaque = true
[ab.rt]
source = "draws"
[vulkan]
max_fps = 75
)",
                       &warnings));
    int removed = 0;
    for (const auto& w : warnings) {
        if (w.find("was removed, ignoring") != std::string::npos) ++removed;
        else CHECK_MSG(false, w);
    }
    CHECK_MSG(removed == 4, std::to_string(removed));
    CHECK(warnings.size() == 4);
    for (const char* key : {"native.opaque", "native.shadows", "native.debug", "rt.source"}) {
        CHECK(reg.find(key) == nullptr);
        bool named = false;
        for (const auto& w : warnings) named = named || w.find(std::string("'") + key + "'") != std::string::npos;
        CHECK_MSG(named, key);
    }
    CHECK(reg.find("vulkan.max_fps") != nullptr && reg.find("vulkan.max_fps")->value() == 75.0);
    const std::string saved = reg.toToml();
    CHECK(saved.find("opaque") == std::string::npos && saved.find("mirror") == std::string::npos && saved.find("[ab.native]") == std::string::npos);
    CHECK(saved.find("max_fps = 75") != std::string::npos);
    std::vector<std::string> again;
    CHECK(reg.fromToml(saved, &again) && again.empty());
}
