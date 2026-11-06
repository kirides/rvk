#pragma once

// Content equality / hashes of the interned vertex input and dynamic state values (scene_draws.cpp, owned_persist.cpp): the arena pointers differ between
// frames, the contents do not.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "rhi/game_device.h"

namespace rvk::d3d9 {

inline bool viEqual(const rhi::VertexInputValues& a, const rhi::VertexInputValues& b) {
    const auto eq = [](const void* x, const void* y, std::size_t n) { return n == 0 || std::memcmp(x, y, n) == 0; };
    return a.bufferCount == b.bufferCount && a.dynamicLayout == b.dynamicLayout && a.bindingCount == b.bindingCount && a.attributeCount == b.attributeCount &&
           a.vertexShift == b.vertexShift && eq(a.buffers, b.buffers, sizeof(a.buffers[0]) * a.bufferCount) && eq(a.offsets, b.offsets, sizeof(a.offsets[0]) * a.bufferCount) &&
           eq(a.bindings, b.bindings, sizeof(a.bindings[0]) * a.bindingCount) && eq(a.attributes, b.attributes, sizeof(a.attributes[0]) * a.attributeCount);
}

inline bool dynEqual(const rhi::DynamicStateValues& a, const rhi::DynamicStateValues& b) {
    constexpr std::size_t prefix = offsetof(rhi::DynamicStateValues, blendEnable);
    const std::uint32_t n = a.colorCount;
    if (a.colorCount != b.colorCount || std::memcmp(&a, &b, prefix) != 0) return false;
    return n == 0 || (std::memcmp(a.blendEnable, b.blendEnable, sizeof(VkBool32) * n) == 0 && std::memcmp(a.blendEquation, b.blendEquation, sizeof(VkColorBlendEquationEXT) * n) == 0 &&
                      std::memcmp(a.writeMask, b.writeMask, sizeof(VkColorComponentFlags) * n) == 0);
}


// F2c-2: content hashes of the interned vertex input / dynamic state (the arena pointers differ between frames, the contents do not)
inline std::uint64_t hashBytes(std::uint64_t h, const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

inline std::uint64_t hashVi(const rhi::VertexInputValues& v) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    h = hashBytes(h, &v.bufferCount, sizeof(v.bufferCount));
    h = hashBytes(h, &v.dynamicLayout, sizeof(v.dynamicLayout));
    h = hashBytes(h, &v.bindingCount, sizeof(v.bindingCount));
    h = hashBytes(h, &v.attributeCount, sizeof(v.attributeCount));
    h = hashBytes(h, &v.vertexShift, sizeof(v.vertexShift));
    h = hashBytes(h, v.buffers, sizeof(v.buffers[0]) * v.bufferCount);
    h = hashBytes(h, v.offsets, sizeof(v.offsets[0]) * v.bufferCount);
    h = hashBytes(h, v.bindings, sizeof(v.bindings[0]) * v.bindingCount);
    h = hashBytes(h, v.attributes, sizeof(v.attributes[0]) * v.attributeCount);
    return h;
}

inline std::uint64_t hashDyn(const rhi::DynamicStateValues& d) {
    constexpr std::size_t prefix = offsetof(rhi::DynamicStateValues, blendEnable);
    std::uint64_t h = hashBytes(0xcbf29ce484222325ull, &d, prefix);
    const std::uint32_t n = d.colorCount;
    h = hashBytes(h, d.blendEnable, sizeof(VkBool32) * n);
    h = hashBytes(h, d.blendEquation, sizeof(VkColorBlendEquationEXT) * n);
    return hashBytes(h, d.writeMask, sizeof(VkColorComponentFlags) * n);
}


}  // namespace rvk::d3d9
