#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "shader/abi.h"
#include "shader/sm_ir.h"

namespace rvk::shader {

// What the renderer needs to know about a translated shader.
struct Reflection {
    Version version;
    // vertex shader inputs: v# register (= location) with its declared semantic
    struct VertexInput {
        std::uint32_t location;
        Usage usage;
        std::uint8_t usageIndex;
    };
    std::vector<VertexInput> vertexInputs;
    std::uint32_t varyingMask = 0;                                  // locations written (vs) / read (ps)
    std::array<TextureType, abi::kSamplerSlots> samplers{};        // TextureType::Unknown = unused
    std::uint32_t floatConstCount = 0;                              // highest c# read + 1 (256 with relative addressing)
    bool relativeConstants = false;
    bool writesDepth = false;
    bool discards = false;                                          // texkill
    std::uint32_t colorOutputs = 0;                                 // oC# written (bit mask)
    bool writesPointSize = false;
};

struct GlslResult {
    std::string glsl;
    Reflection reflection;
    std::string error;  // empty on success
    bool ok() const { return error.empty(); }
};

GlslResult toGlsl(const Program& program);

}  // namespace rvk::shader
