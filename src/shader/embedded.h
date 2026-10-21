#pragma once

// Shaders embedded at build time (CMakeLists.txt RVK_SHADERS, tools/glslc): the SPIR-V of the hand-written GLSL shaders in
// shaders/** (validated with spirv-val while building) and the text of shaders/include/rvk_abi.glsl, which the D3D9 translator
// puts into every translated shader. Nothing is deployed besides d3d9.dll.

#include <cstdint>
#include <span>
#include <string_view>

namespace rvk::shader::embedded {

struct Entry {
    std::string_view name;               // path below shaders/, e.g. "debug/inject_view.comp"
    std::span<const std::uint32_t> spirv;
};

// every embedded shader
std::span<const Entry> all();
// the SPIR-V of one (empty when unknown)
std::span<const std::uint32_t> spirv(std::string_view name);
// shaders/include/rvk_abi.glsl (the ABI declarations; shader/abi.h is its C++ mirror)
std::string_view abiGlsl();

}  // namespace rvk::shader::embedded
