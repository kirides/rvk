#pragma once

// A minimal fixed-function emulation for the engine's debug drawing (docs/research/debug-mode.md 2.1, 8.3): the debug keys
// (bounding boxes, collision shapes, raycasts, skeletons) draw D3D9 FVF line lists with no shaders bound (eCAPIDevice
// SetVertexProgram(fvf) / SetPixelProgram(0)), coloured by texture stage 0 (SetColorCalc / SetAlphaCalc with the texture
// factor). The HAL replaces "no shader" by two generated SM 3.0 shaders that go through the normal translator:
//
//   vs_3_0: o0 = position * (world * view * projection) (c248..c251, transposed), o1 = the vertex colour (or white)
//   ps_3_0: oC0 = v0 * c220 + c221 (stage 0's colour and alpha operations reduced to a linear form of the vertex colour)
//
// Textures, lighting, fog and stages > 0 are not emulated (the debug draws use none of them).

#include <array>
#include <cstdint>
#include <vector>

namespace rvk::d3d9::ff {

inline constexpr std::uint32_t kVsMatrix = 248;  // c248..c251: rows of transpose(world * view * projection)
inline constexpr std::uint32_t kPsMul = 220;     // c220: factor of the vertex colour
inline constexpr std::uint32_t kPsAdd = 221;     // c221: constant term

// D3DFVF bits the emulation understands
inline constexpr std::uint32_t kFvfPositionMask = 0x400E;
inline constexpr std::uint32_t kFvfXyz = 0x002;
inline constexpr std::uint32_t kFvfDiffuse = 0x040;

// true for FVFs with an untransformed XYZ position (XYZRHW / blend weights are not emulated)
bool supportedFvf(std::uint32_t fvf);

std::vector<std::uint32_t> vertexShader(bool diffuse);
std::vector<std::uint32_t> pixelShader();

// texture stage 0's operations (D3DTSS_COLOROP, COLORARG1/2, ALPHAOP, ALPHAARG1/2; D3D9 defaults)
struct Stage0 {
    std::uint32_t colorOp = 4;  // D3DTOP_MODULATE
    std::uint32_t colorArg1 = 2;  // D3DTA_TEXTURE
    std::uint32_t colorArg2 = 1;  // D3DTA_CURRENT
    std::uint32_t alphaOp = 2;  // D3DTOP_SELECTARG1
    std::uint32_t alphaArg1 = 2;
    std::uint32_t alphaArg2 = 1;
};

// the stage's result as vertexColour * mul + add (RGBA, 0..1); the texture counts as white, the texture factor is a D3DCOLOR
struct Linear {
    std::array<float, 4> mul{};
    std::array<float, 4> add{};
};
Linear reduce(const Stage0& stage, std::uint32_t textureFactor);

// world * view * projection (row-major D3D matrices, row vectors) transposed: four rows for the dp4 of the vertex shader
std::array<float, 16> transposedProduct(const float* world, const float* view, const float* projection);

}  // namespace rvk::d3d9::ff
