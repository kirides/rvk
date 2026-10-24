#pragma once

// The GLSL material pixel shader (shaders/material/ge_default.{glsl,frag}, docs/research/shader-permutations.md 6-8) for one
// engine request: the macros the engine hashed to find its precompiled shader (d3d9/material_source.h) become #defines in front of
// the embedded source, so the shader is the same permutation in readable GLSL. Pure text: the caller compiles it.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rvk::shader {

using MacroList = std::vector<std::pair<std::string, std::string>>;

// effect / entry the port covers (ge_default_phong.fx: PS_Default, PS_DefaultZPass)
bool uberSupports(std::string_view effectFile, std::string_view entry);

// One translation unit: #version, the macros (true / false -> 1 / 0, a value on one line; XFX_PASS = 1 for PS_DefaultZPass),
// `hookDefines` (RVK_MV_* of enh/material_variant: ambient from the GI probes, the thin G-buffer, motion; empty = none), the
// ABI declarations, the enhancement code when there are hooks, then the shader. Empty when the macro list has an invalid name.
std::string uberPixelGlsl(std::string_view entry, const MacroList& macros, std::string_view hookDefines);

// a hash of the embedded source text (cache names of compiled variants change when the shader does)
std::uint64_t uberSourceHash();

}  // namespace rvk::shader
