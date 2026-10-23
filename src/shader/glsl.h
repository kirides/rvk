#pragma once

#include <array>
#include <cstdint>
#include <map>
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

struct GlslOptions {
    // pixel shaders: blend oC0 towards the debug tint of the DrawBlock (abi.h pad[0], 0 = off) after the alpha test. A
    // separate variant: the default output is unchanged (the tint debug view, docs/research/draw-tags.md 5)
    bool tintEpilogue = false;

    // ---- variant hooks for enhancements (docs/research/injection.md 4). All empty = the default translation, unchanged.
    // GLSL added after the generated globals: extra outputs (e.g. `layout(location = 4) out vec4 rvk_motion;`), specialisation
    // constants (`layout(constant_id = N) const bool ...`, ids from abi.h kSpec*), helper functions
    std::string extraGlobals;
    // statements at the end of main(). Pixel shaders: after the alpha test (and the tint), before oC# are copied to the
    // outputs (oC0..oC3 and the varyings v#/t# are in scope). Vertex shaders: after the clip position `rvk_pos` is final and
    // before the half-pixel fix (the o# / oT# / oD# registers and rvk_pos are in scope)
    std::string epilogue;
    // named-constant overrides: float constant register -> GLSL expression that replaces every read of it (direct and
    // relative); `$c` in the expression stands for the original value (e.g. pc.ps.c[15]). Callers pick registers by CTAB
    // name through d3d9::ShaderSemantics (g_AmbientLight, ...). Registers defined in the shader (def c#) are not overridden.
    std::map<std::uint32_t, std::string> constOverrides;
    // vertex shader input overrides: the declared input of this semantic is read through the GLSL expression (`$v` = the
    // attribute as fetched), e.g. the SpeedTree billboards' fade distances scaled (engine/lod.h)
    struct InputOverride {
        Usage usage = Usage::Position;
        std::uint8_t usageIndex = 0;
        std::string expr;
    };
    std::vector<InputOverride> inputOverrides;
};

GlslResult toGlsl(const Program& program, const GlslOptions& options = {});

}  // namespace rvk::shader
