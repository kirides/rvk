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
    CHECK(n == 165);  // (overlay.compile_progress; 14.44: native.ordered_bulk; 14.43: native.rt_gpu_build; 14.42: native.tree_motion; 14.33: native.own_extract; 14.31: native.own_collection; 14.30: native.rt_records; 14.29: native.owned_shadow_views; 14.26: native.own_particles, native.owned_decals; 14.24: native.own_sky, native.own_gui; 14.20: native.owned_ordered; 14.19: native.owned_instances; 14.15: native.rigid_motion; F4-5: native.tree_wind; S-5: native.own_foliage; 14.13: native.bone_motion; S-4: native.vertex_pull) 172 before the settings clean-up (docs/research/settings-audit.md): 55 finished A/B switches / art coefficients are constants now
    struct rvk_setting_info info{};
    CHECK(api::settingInfo(reg, 0, &info, sizeof(info)) == 96);
    CHECK(info.size == 96);
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
    CHECK(api::settingInfo(reg, 11, &info, sizeof(info)) == 96);  // vulkan.adapter (System tab, after renderer.mode, the menu settings and vulkan.api)
    CHECK((info.flags & (RVK_SETTING_RESTART | RVK_SETTING_IS_LOCKED)) == (RVK_SETTING_RESTART | RVK_SETTING_IS_LOCKED));
    // capability resolution: rt.allow needs ray tracing hardware; this registry has no device report
    CHECK(api::settingInfo(reg, 14, &info, sizeof(info)) == 96);
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
