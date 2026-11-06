#pragma once

// The HLSL material pixel shader (shaders/material/ge_default.frag.hlsl, docs/research/shader-permutations.md 6-8) for one
// engine request: the macros the engine hashed to find its precompiled shader (d3d9/material_source.h) become #defines in front of
// the embedded source, so the shader is the same permutation in readable HLSL. Pure text: the caller compiles it.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rvk::shader {

using MacroList = std::vector<std::pair<std::string, std::string>>;

// effect / entry the port covers (ge_default_phong.fx: PS_Default, PS_DefaultZPass)
bool uberSupports(std::string_view effectFile, std::string_view entry);
// uberSupports + ge_default_leafs.fx (PS_Default / PS_DefaultZPass: the SpeedTree leaves, shader-permutations.md 11). Build those with the
// overload that takes the effect file; the run time (d3d9/device.cpp) still asks uberSupports and builds without it (leaves stay translated).
bool uberSupportsFile(std::string_view effectFile, std::string_view entry);

// One translation unit (shaders/material/ge_default.frag.hlsl, entry PSMain): the macros as #defines (true / false -> 1 / 0, a value on one line) (XFX_PASS = 1 for PS_DefaultZPass), the hook
// defines, then `#include "material/ge_default.frag.hlsl"` (resolved from the embedded sources by shader::dxc). Empty on an invalid macro name.
std::string uberPixelHlsl(std::string_view entry, const MacroList& macros, std::string_view hookDefines, bool sceneRecord = false,
                          bool sceneView = false);
// the same for a request of `effectFile` (ge_default_phong.fx or ge_default_leafs.fx: adds `#define RVK_LEAFS 1`)
std::string uberPixelHlsl(std::string_view effectFile, std::string_view entry, const MacroList& macros, std::string_view hookDefines,
                          bool sceneRecord = false, bool sceneView = false);
// a hash of the HLSL texts of the ubershader (and what it includes): cache names of compiled variants change when they do
std::uint64_t uberHlslHash();

// A bit per class of request (debug.ubershader_skip: classes kept on the translated shader, to bisect differences):
// 1 z pass, 2 masked, 4 alpha blend, 8 lightmap, 16 normal map, 32 specular, 64 distortion, 128 anything else (plain)
std::uint32_t uberClassMask(std::string_view entry, const MacroList& macros);

}  // namespace rvk::shader
