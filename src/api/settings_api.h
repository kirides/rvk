#pragma once

// The settings part of the tool API (include/rvk_api.h: rvk_setting_*, rvk_ab, rvk_overlay) on a settings registry; the
// exports in src/proxy/d3d9_exports.cpp call these with settings::registry() (tests with their own registry).

#include <cstdint>

#include "core/settings.h"
#include "rvk_api.h"

namespace rvk::api {

std::uint32_t settingCount(settings::Registry& reg);
std::int32_t settingInfo(settings::Registry& reg, std::uint32_t index, struct rvk_setting_info* out, std::uint32_t size);
std::int32_t settingGet(settings::Registry& reg, const char* key, char* value, std::uint32_t valueSize);
std::int32_t settingSet(settings::Registry& reg, const char* key, const char* value, std::uint32_t flags);
std::int32_t ab(settings::Registry& reg, std::int32_t op);

}  // namespace rvk::api
