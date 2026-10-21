#pragma once

// The contract between translated D3D9 shaders (shader/glsl.cpp) and the Vulkan renderer. Change both sides together.
//
// Push constants (24 bytes, all stages):
//   uint64 vsConsts   device address of the vertex shader's StageConsts
//   uint64 psConsts   device address of the pixel shader's StageConsts
//   uint64 draw       device address of the DrawBlock
//
// StageConsts (std430): vec4 c[256]; ivec4 i[16]; uint b[16]          -> c at 0, i at 4096, b at 4352, 4416 bytes
// DrawBlock (std430):   uint tex[20]; uint smp[20]; vec4 posFix; uint alphaFunc; float alphaRef; uint pad[2]
//                       tex/smp: bindless texture / sampler indices of D3D9 samplers s0..s15 (pixel) and 16..19 (vertex s0..s3)
//                       posFix.xy: added to gl_Position.xy * w (D3D9 half-pixel offset: (-1/width, 1/height))
//                       alphaFunc: D3DCMPFUNC (1 never .. 8 always), alphaRef: 0..1
//
// Descriptor set 0 (bindless, update-after-bind, partially bound):
//   binding 0: texture2D t2D[]   binding 1: textureCube tCube[]   binding 2: texture3D t3D[]   binding 3: sampler samp[]
//   index 0 of every texture array is a 1x1 (0,0,0,1) texture (D3D9's value of an unbound sampler).
//
// Varyings: location = varyingLocation(usage, usageIndex); vertex inputs: location = v# register index.

#include <cstddef>
#include <cstdint>

#include "shader/sm_ir.h"

namespace rvk::shader::abi {

inline constexpr std::uint32_t kBindingTex2D = 0;
inline constexpr std::uint32_t kBindingTexCube = 1;
inline constexpr std::uint32_t kBindingTex3D = 2;
inline constexpr std::uint32_t kBindingSamplers = 3;

inline constexpr std::uint32_t kFloatConsts = 256;
inline constexpr std::uint32_t kIntConsts = 16;
inline constexpr std::uint32_t kBoolConsts = 16;
inline constexpr std::size_t kStageConstsIntOffset = kFloatConsts * 16;
inline constexpr std::size_t kStageConstsBoolOffset = kStageConstsIntOffset + kIntConsts * 16;
inline constexpr std::size_t kStageConstsSize = kStageConstsBoolOffset + kBoolConsts * 4;

inline constexpr std::uint32_t kPixelSamplers = 16;
inline constexpr std::uint32_t kVertexSamplers = 4;
inline constexpr std::uint32_t kVertexSamplerBase = 16;  // vertex shader s0..s3 -> DrawBlock slots 16..19
inline constexpr std::uint32_t kSamplerSlots = kPixelSamplers + kVertexSamplers;

struct DrawBlock {
    std::uint32_t tex[kSamplerSlots];
    std::uint32_t smp[kSamplerSlots];
    float posFix[4];
    std::uint32_t alphaFunc;
    float alphaRef;
    std::uint32_t pad[2];
};
static_assert(sizeof(DrawBlock) == 192);

struct PushConstants {
    std::uint64_t vsConsts;
    std::uint64_t psConsts;
    std::uint64_t draw;
};
static_assert(sizeof(PushConstants) == 24);

inline constexpr std::uint32_t kMaxVaryings = 32;
inline constexpr std::uint32_t kInvalidLocation = 0xFFFFFFFFu;

// Location of an interpolated value between the vertex and pixel shader (both sides use this mapping, so they link
// by semantic, as D3D9 does). kInvalidLocation for semantics that are not varyings (POSITION0, PSIZE).
constexpr std::uint32_t varyingLocation(Usage usage, std::uint32_t index) {
    switch (usage) {
    case Usage::TexCoord: return index < 16 ? index : kInvalidLocation;
    case Usage::Color: return index < 2 ? 16 + index : kInvalidLocation;
    case Usage::Fog: return 18;
    case Usage::Normal: return index < 2 ? 19 + index : kInvalidLocation;
    case Usage::Tangent: return index < 2 ? 21 + index : kInvalidLocation;
    case Usage::Binormal: return index < 2 ? 23 + index : kInvalidLocation;
    case Usage::Position: return index >= 1 && index < 3 ? 24 + index : kInvalidLocation;  // POSITION1/2: 25, 26
    case Usage::BlendWeight: return index == 0 ? 27 : kInvalidLocation;
    case Usage::BlendIndices: return index == 0 ? 28 : kInvalidLocation;
    case Usage::Depth: return index == 0 ? 29 : kInvalidLocation;
    case Usage::PSize: return kInvalidLocation;
    default: return kInvalidLocation;
    }
}

}  // namespace rvk::shader::abi
