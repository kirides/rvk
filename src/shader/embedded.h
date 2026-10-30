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

// every embedded (GLSL) shader
std::span<const Entry> all();
// the SPIR-V of one (empty when unknown): an HLSL shader (hlslSource(name + ".hlsl"), compiled on first use by shader/dxc with the
// entry point of the stage: .comp CSMain, .vert VSMain, .frag PSMain; thread-safe, the span stays valid for the process lifetime),
// else the build-time GLSL one. docs/research/hlsl-rhi.md 3 (the migration)
std::span<const std::uint32_t> spirv(std::string_view name);

// the same with preprocessor defines ("NAME=VALUE;NAME2=VALUE2"): HLSL shaders that are one module per size / mode (the ocean FFT). Cached by
// (name, defines) like spirv(); the HLSL source only (no GLSL variant exists)
std::span<const std::uint32_t> spirvVariant(std::string_view name, std::string_view defines);

// the build-time GLSL SPIR-V of a shader only (no HLSL fall-through: tests of the GLSL ABI and A/B tools); empty when unknown
std::span<const std::uint32_t> glslSpirv(std::string_view name);

// HLSL sources embedded from shaders/** (*.hlsl, *.hlsli); name = path below shaders/ with '/' ("include/rvk_abi.hlsli")
struct SourceFile {
    std::string_view name;
    std::string_view text;
};
std::span<const SourceFile> hlslSources();
// the text of one (empty when unknown)
std::string_view hlslSource(std::string_view name);
// shaders/include/rvk_abi.glsl (the ABI declarations; shader/abi.h is its C++ mirror)
std::string_view abiGlsl();
// shaders/include/rvk_gi.glsl (the GI probe lookup; enh/gi pastes it into the material pixel shaders' GI variants)
std::string_view giGlsl();
// shaders/include/rvk_material_variant.glsl (the material pixel shaders' enhancement variants, enh/material_variant)
std::string_view materialVariantGlsl();

// shaders/material/ge_default.glsl / ge_default.frag: the GLSL material pixel shader (shader/uber.h)
std::string_view uberGlsl();
std::string_view uberFrag();

}  // namespace rvk::shader::embedded
