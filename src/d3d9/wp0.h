#pragma once

// WP0 spike of the DX12 game port (docs/research/dx12-game-port.md): what the D3D12 draw path has to cover, counted from the Vulkan
// model's draws. Enabled by the environment variable RVK_WP0=1 (run.ps1 -Environment 'RVK_WP0=1'); the numbers are logged every
// 600 frames as `wp0:` lines. Pure counting: no effect on rendering. Not hot: only when enabled.

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <unordered_set>

#include "rhi/game_device.h"

namespace rvk::d3d9 {

struct Wp0 {
    bool on = false;
    std::uint64_t draws = 0;
    std::uint64_t point = 0, fan = 0, fanIndexed = 0, strip = 0, lineList = 0, lineStrip = 0, stripIndexed = 0;
    std::uint64_t pointInstanced = 0;
    std::uint32_t skinLogs = 0;
    std::uint64_t capture = 0, gi = 0, gbuf = 0, replacedOrOther = 0;
    std::map<std::string, std::uint64_t> exotic;  // vertex formats the D3D12 input assembler has no equivalent for
    std::unordered_set<std::uint64_t> fullKeys, baseKeys, layoutKeys;

    static std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return h;
    }
    template <class T>
    static std::uint64_t bytes(std::uint64_t h, const T& v) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
        for (std::size_t i = 0; i < sizeof(T); ++i) h = mix(h, p[i]);
        return h;
    }
};

}  // namespace rvk::d3d9
