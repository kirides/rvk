#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "shader/sm_tokens.h"

namespace rvk::shader {

struct SpirvResult {
    std::vector<std::uint32_t> words;
    std::string log;  // compiler messages (errors on failure)
    bool ok() const { return !words.empty(); }
};

// GLSL 4.60 -> SPIR-V 1.6 for Vulkan 1.3 (glslang). Thread-safe after the first call.
SpirvResult compileGlsl(Stage stage, std::string_view glsl);

}  // namespace rvk::shader
