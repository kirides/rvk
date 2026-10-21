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
    CHECK(n == 19);  // 11 + the 8 water.* settings
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
    CHECK(api::settingInfo(reg, 2, &info, sizeof(info)) == 80);
    CHECK((info.flags & (RVK_SETTING_RESTART | RVK_SETTING_IS_LOCKED)) == (RVK_SETTING_RESTART | RVK_SETTING_IS_LOCKED));
    // capability resolution: rt.allow needs ray tracing hardware; this registry has no device report
    CHECK(api::settingInfo(reg, 10, &info, sizeof(info)) == 80);
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
