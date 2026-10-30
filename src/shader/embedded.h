#pragma once

// The shaders of the renderer (docs/research/hlsl-rhi.md): HLSL sources embedded at build time (CMakeLists.txt: shaders/**/*.hlsl|hlsli through rvk_embed) and compiled
// to SPIR-V on first use by shader/dxc (DXC from dxcompiler.dll, an on-disk cache under bin\rvk\cache\hlsl; RVK_SHADER_DIR reads the sources from disk instead).
// Nothing is deployed besides d3d9.dll and dxcompiler.dll.

#include <cstdint>
#include <span>
#include <string_view>

namespace rvk::shader::embedded {

// The SPIR-V of the shader `name` = a path below shaders/ with its stage extension ("water/water.frag" = shaders/water/water.frag.hlsl; entry CSMain / VSMain / PSMain
// by .comp / .vert / .frag, SPIR-V entry point renamed main). Compiled on first use (thread-safe, serialized), the span stays valid for the process lifetime; empty when
// the name is unknown or DXC fails (logged).
std::span<const std::uint32_t> spirv(std::string_view name);

// the same with preprocessor defines ("NAME=VALUE;NAME2=VALUE2"): shaders that are one module per size / mode (the ocean FFT). Cached by (name, defines).
std::span<const std::uint32_t> spirvVariant(std::string_view name, std::string_view defines);

// The DXIL (SM 6.6, signed by dxil.dll next to dxcompiler.dll) of the same HLSL text for the DX12 backend: same names, defines and caching (shader/dxc disk cache, the process's map;
// the spans stay valid for the process lifetime). Every shader of the renderer compiles to both (docs/research/hlsl-rhi.md "DXIL parity (S1)"; rvk_shadertool dxil-check).
std::span<const std::uint8_t> dxil(std::string_view name);
std::span<const std::uint8_t> dxilVariant(std::string_view name, std::string_view defines);

// HLSL sources embedded from shaders/** (*.hlsl, *.hlsli); name = path below shaders/ with '/' ("include/rvk_abi.hlsli")
struct SourceFile {
    std::string_view name;
    std::string_view text;
};
std::span<const SourceFile> hlslSources();
// the text of one (empty when unknown)
std::string_view hlslSource(std::string_view name);

}  // namespace rvk::shader::embedded
