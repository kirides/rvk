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
    // pixel shader inputs: varying location -> the GLSL variable holding it (v# / t#), for variant hooks that read a varying
    struct PixelInput {
        std::uint32_t location;
        std::string name;
    };
    std::vector<PixelInput> pixelInputs;
    std::array<TextureType, abi::kSamplerSlots> samplers{};        // TextureType::Unknown = unused
    std::uint32_t floatConstCount = 0;                              // highest c# read + 1 (256 with relative addressing)
    bool relativeConstants = false;
    // split constants (docs/research/vulkanize/plan.md V2): the translation reads its float registers through the window
    // blocks of the push constants (abi.h kVsWindowFirst / kPsWindowFirst; RVK_VSC / RVK_PSC), so the device can upload only
    // the windows that changed. windowEnd[w] = the highest register read in window w + 1 (a relative read: the base register's
    // whole window; 0 = the window is not read). splitConsts is false for hand-made reflections (hand-written GLSL reads
    // pc.vs / pc.ps, i.e. needs one full block) and for translations that read i# / b# (they come from the full block)
    std::array<std::uint16_t, abi::kConstWindows> windowEnd{};
    bool splitConsts = false;
    // the translation reads i# / b# from the stage block (an integer / bool constant without a defi / defb: loop / rep counts,
    // if / callnz conditions, any i# / b# operand). False: the upload skips the block's int / bool part (Device::uploadConstants;
    // no game shader reads them, docs/research/vulkanize/V0-constants.md 0). Hand-made reflections keep the default (copied)
    bool intBoolConstants = true;
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
    // GLSL right after the ABI declarations, before the generated globals: declarations and functions that constOverrides /
    // inputOverrides expressions call (they are used inside the generated helpers, e.g. rvk_cRel), e.g. the GI lookup
    // (shaders/include/rvk_gi.glsl, enh/gi)
    std::string prelude;
    // statements at the start of main(), before the translated program runs (uniform control flow: derivatives are defined;
    // vertex shaders: after the input overrides)
    std::string prologue;
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
    // operand captures: just before the first dp3 / dp4 that reads float constant `reg` directly, its other source operand
    // (as read, swizzle applied) is copied into the GLSL global `target` (a vec4 the prelude declares): the value a shader
    // dots with a named constant, e.g. the normal-mapped world normal its ambient SH is evaluated with (enh/gbuffer)
    struct DotCapture {
        std::uint32_t reg = 0;
        std::string target;
    };
    std::vector<DotCapture> dotCaptures;

    // material pixel shaders (enh/material_variant): the variant may be built from the GLSL material shader instead of the
    // translated bytecode (d3d9::Device ubershader setting, shader/uber.h). `uberDefines` = the RVK_MV_* defines of the hooks
    // (empty: the plain shader); the translator ignores both
    bool uber = false;
    std::string uberDefines;
};

GlslResult toGlsl(const Program& program, const GlslOptions& options = {});

}  // namespace rvk::shader
