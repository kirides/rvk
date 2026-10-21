#include "api/settings_api.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>

#include <spdlog/spdlog.h>

namespace rvk::api {
namespace {

// rvk_setting_info::options strings: built once per setting, never freed (the pointers are handed out)
const char* optionsText(const settings::Setting& s) {
    static std::mutex mutex;
    static std::map<const settings::Setting*, std::string> texts;
    const std::lock_guard lock(mutex);
    auto it = texts.find(&s);
    if (it == texts.end()) {
        std::string t;
        for (const auto& o : s.spec().options) t += o + "\n";
        it = texts.emplace(&s, std::move(t)).first;
    }
    return it->second.c_str();
}

std::int32_t toResult(settings::Result r) { return static_cast<std::int32_t>(r); }

}  // namespace

std::uint32_t settingCount(settings::Registry& reg) { return static_cast<std::uint32_t>(reg.size()); }

std::int32_t settingInfo(settings::Registry& reg, std::uint32_t index, struct rvk_setting_info* out, std::uint32_t size) {
    static_assert(sizeof(struct rvk_setting_info) == 80, "rvk_setting_info v1 is 80 bytes; only append fields");
    const auto all = reg.all();
    if (out == nullptr || size < 8 || index >= all.size()) return RVK_SETTING_BAD_ARGUMENT;
    const settings::Setting& s = *all[index];
    const settings::Spec& sp = s.spec();
    struct rvk_setting_info i{};
    i.type = static_cast<std::uint32_t>(sp.type);
    i.flags = ((sp.flags & settings::Flag::Restart) != 0 ? RVK_SETTING_RESTART : 0u) |
              ((sp.flags & settings::Flag::NoAB) != 0 ? RVK_SETTING_NO_AB : 0u) |
              ((sp.flags & settings::Flag::Hidden) != 0 ? RVK_SETTING_HIDDEN : 0u) | (s.locked() ? RVK_SETTING_IS_LOCKED : 0u) |
              (s.restartPending() ? RVK_SETTING_RESTART_PENDING : 0u);
    i.option_count = static_cast<std::uint32_t>(sp.options.size());
    i.key = sp.key.c_str();
    i.label = sp.label.c_str();
    i.category = sp.category.c_str();
    i.description = sp.description.c_str();
    i.options = optionsText(s);
    i.default_value = sp.defaultValue;
    i.min = sp.min;
    i.max = sp.max;
    const std::uint32_t n = std::min<std::uint32_t>(size, sizeof(i));
    i.size = n;
    std::memcpy(out, &i, n);
    return static_cast<std::int32_t>(n);
}

std::int32_t settingGet(settings::Registry& reg, const char* key, char* value, std::uint32_t valueSize) {
    if (key == nullptr) return RVK_SETTING_BAD_ARGUMENT;
    const settings::Setting* s = reg.find(key);
    if (s == nullptr) return RVK_SETTING_UNKNOWN_KEY;
    const std::string text = reg.text(*s);
    if (value != nullptr && valueSize > 0) {
        const std::size_t n = std::min<std::size_t>(text.size(), valueSize - 1);
        std::memcpy(value, text.data(), n);
        value[n] = '\0';
    }
    return static_cast<std::int32_t>(text.size());
}

std::int32_t settingSet(settings::Registry& reg, const char* key, const char* value, std::uint32_t flags) {
    if (key == nullptr || value == nullptr || flags != 0) return RVK_SETTING_BAD_ARGUMENT;
    const settings::Result r = reg.setText(key, value);
    if (r == settings::Result::Ok) {
        spdlog::info("rvk_setting_set({}, {}) -> {}", key, value, reg.text(*reg.find(key)));
    } else {
        spdlog::warn("rvk_setting_set({}, {}): {}", key, value, settings::toString(r));
    }
    return toResult(r);
}

std::int32_t ab(settings::Registry& reg, std::int32_t op) {
    switch (op) {
    case RVK_AB_QUERY: break;
    case RVK_AB_FLIP: reg.flipAB(); break;
    case RVK_AB_SIDE_A: reg.setSide(false); break;
    case RVK_AB_SIDE_B: reg.setSide(true); break;
    case RVK_AB_COPY: reg.copyActiveToOther(); break;
    case RVK_AB_RESET: reg.resetOther(); break;
    default: return RVK_SETTING_BAD_ARGUMENT;
    }
    if (op != RVK_AB_QUERY) spdlog::info("rvk_ab({}): side {} active", op, reg.bActive() ? "B" : "A");
    return reg.bActive() ? 1 : 0;
}

}  // namespace rvk::api
