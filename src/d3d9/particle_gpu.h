// F11 (docs/research/frame-ownership.md 14.26, native.own_particles): particle quads expanded on the GPU. A particle op is drawn as ONE instanced
// draw: instance i = particle record i (the engine's depth order), vertex 0..3 = its corners; the engine's translated particle vertex shader runs on
// the corner the HLSL twin of `expand` (shaders/include/rvk_particle.hlsli rvkParticleExpand) builds from the record and the op's constants
// (VS c232..c247). Pure data + the C++ twin (bit-exact with SharedBase's rules when the record's sin / cos are SharedBase's).
#pragma once

#include <cstdint>
#include <cstring>

namespace rvk::d3d9::particle {

inline constexpr std::uint32_t kRecordStride = 0x68;
inline constexpr std::uint32_t kConstFirst = 232;  // VS window 3 (abi.h kVsWindowFirst[3])
inline constexpr std::uint32_t kConstCount = 16;
inline constexpr std::uint32_t kVertexStride = 0x30;  // the engine's particle vertex (POSITION, NORMAL, COLOR0, COLOR1, TEXCOORD0, TEXCOORD1)
// the record's locations (TEXCOORD8..15 of the declaration; the engine's particle shaders read v0..v5)
inline constexpr std::uint32_t kRecordLocation = 8;

// the expansion kinds (Device::particleVs): the particle vertex; an eCSkyObject billboard (the precipitation: 0x30-byte records, sc::skyBillboardVertices' rule)
inline constexpr int kKindParticle = 0, kKindSkyBillboard = 1;
inline constexpr std::uint32_t kSkyRecordStride = 0x30;

// flags (c244.w)
inline constexpr std::uint32_t kFlagEntityCoords = 1, kFlagAxisInEntity = 2, kFlagPinned = 4;

struct Record {
    float r[6][4];             // pos + hx; d + hy; sin / cos of angles 0, 1; sin / cos of angle 2, age ratio, 1 - frame fraction; uv; uv of the blend frame
    std::uint32_t colour1 = 0;  // COLOR1 (0xAARRGGBB, the engine's D3DCOLOR)
    float mask = 0.0f;          // which angles are not 0 (1, 2, 4)
};
static_assert(sizeof(Record) == kRecordStride);

struct OpConsts {
    float c[kConstCount][4]{};
};

// bCByteAlphaColor's channel rule: x 255 truncated (cvttss2si: out of range / NaN = 0x80000000), the low byte
inline std::uint32_t byteOf(float x) {
    const float s = x * 255.0f;
    if (!(s >= -2147483648.0f && s < 2147483648.0f)) return 0u;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(s)) & 255u;
}
inline std::uint32_t argbOf(float r, float g, float b, float a) { return byteOf(a) << 24 | byteOf(r) << 16 | byteOf(g) << 8 | byteOf(b); }
// bCFloatAlphaColor::Clamp of one channel: (x <= 1 ? x : 1), then < 0 -> 0 (NaN -> 1)
inline float clampOf(float x) {
    float t = x <= 1.0f ? x : 1.0f;
    if (t < 0.0f) t = 0.0f;
    return t;
}

// the C++ twin of rvkParticleExpand: vertex `corner` (0..3) of the record in the engine's 0x30-byte layout
void expand(const OpConsts& k, const Record& r, std::uint32_t corner, std::uint8_t out[kVertexStride]);

}  // namespace rvk::d3d9::particle
