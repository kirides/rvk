#pragma once

// D3D9 SM 1.x-3.0 bytecode -> HLSL (SM 6.6; docs/research/hlsl-rhi.md 1, P1d): replaced the GLSL translator. Same contract (shader/abi.h),
// same Reflection, same variant hooks; the hook strings (TranslateOptions::prelude / prologue / epilogue / extraGlobals / constOverrides /
// inputOverrides / dotCaptures) are HLSL here. The text goes to shader::dxc (entry VSMain / PSMain, profile vs_6_6 / ps_6_6) with
// `#include "rvk_draw.hlsli"` resolved from the embedded sources.

#include <string>

#include "shader/translate.h"

namespace rvk::shader {

struct HlslResult {
    std::string hlsl;
    Reflection reflection;
    std::string error;  // empty on success
    bool ok() const { return error.empty(); }
};

// Hook environment of the generated entry point:
//  - pixel shader: `rvk_out` (RvkPsOut: oC0..oC3 as SV_Target, depth, plus TranslateOptions::extraOutputs members) is declared before the prologue; the
//    translated program writes the globals oC# / oDepth; the epilogue runs after the alpha test and the tint, before they are copied to rvk_out.
//    Varyings v# / t# are static globals; `rvk_draw` is the draw's RvkDrawData (the frame block: rvkLoadFrame(pc.frame)).
//  - vertex shader: `rvk_out` (RvkVsOut: position, varyings, point size, extraOutputs) and `rvk_pos` (the clip position, a float4 local) as in GLSL.
HlslResult toHlsl(const Program& program, const TranslateOptions& options = {});

// a hash of the embedded HLSL the translated shaders include (rvk_draw / rvk_abi / rvk_common / rvk_oct): part of the device's SPIR-V cache keys
std::uint64_t hlslIncludesHash();

}  // namespace rvk::shader
