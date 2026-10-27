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

// Debug info in the SPIR-V compileGlsl produces (config.toml [debug] shader_debug_info, run.ps1 -ShaderDebug): OpName / OpLine and the GLSL source
// text (OpSource; glslang's NonSemantic debug info fails spirv-val on some shaders), optimizer off: RenderDoc / PIX then show names, source lines and allow
// stepping through the translated D3D9 shaders and the GLSL material shaders. Set before the first compile; part of the SPIR-V cache keys.
void setDebugInfo(bool on);
bool debugInfo();

}  // namespace rvk::shader
