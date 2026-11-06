#pragma once

// MeshPart + Layout records and the pure vertex-declaration logic behind them (docs/research/frame-ownership.md 1.2 / 1.4, F0 "MeshPart + Layout";
// src/gpuscene/mesh_parts.*, shaders/include/rvk_mesh.hlsli = the GPU side, keep them in step).
//
// Layout record (80 bytes, one per distinct vertex declaration, deduplicated by content): uint4 info (x live = 1, y content hash, z elements, w elements the
// table cannot express: other usages / indices, duplicates) + 16 semantic slot words. Slot word: bits 0..15 byte offset in the vertex, 16..23 D3DDECLTYPE + 1
// (0 = the declaration has no such element), 24..27 stream. Slots: 0 position, 1 blend weight, 2 blend indices, 3 normal, 4 tangent, 5 binormal,
// 6 / 7 colour 0 / 1, 8..15 texcoord 0..7.
//
// MeshPart record (128 bytes, one per (sub-mesh, index range) as the native frame draws it; addresses are the GPU copy's, 0 = not in the mesh arena):
//   info    x live (1), y layout id, z generation (bumped when the content changed), w flags (kPart*)
//   index   xy address of the index buffer's byte 0, z first index, w index count
//   range   x base vertex (int; the draws of the static path use 0: Risen's indices are absolute), y min index, z vertex count, w index size (2 / 4)
//   s0..s2  per stream: xy address of the stream's data (buffer byte 0 + the stream offset), z stride, w 1 = bound
//   material xy the sub-mesh's material pointer (the key the material table will use), zw the sub-mesh pointer
//   owner   xy the mesh resource pointer, zw 0 (local bounds: not available cheaply, the engine's sub-mesh has none)

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "d3d9/defs.h"
#include "shader/sm_ir.h"

namespace rvk::gpuscene {

inline constexpr std::uint32_t kSlotPosition = 0, kSlotBlendWeight = 1, kSlotBlendIndices = 2, kSlotNormal = 3, kSlotTangent = 4, kSlotBinormal = 5,
                               kSlotColor0 = 6, kSlotColor1 = 7, kSlotTexcoord0 = 8, kSlotCount = 16;

inline constexpr std::uint32_t kPartInArena = 1u, kPartIndex32 = 2u;  // flags; bits 2..4: stream 0..2 bound

struct alignas(16) LayoutRecord {
    std::uint32_t info[4];
    std::uint32_t slot[kSlotCount];
};
static_assert(sizeof(LayoutRecord) == 80);

struct alignas(16) PartRecord {
    std::uint32_t info[4];
    std::uint32_t index[4];
    std::uint32_t range[4];
    std::uint32_t stream[3][4];
    std::uint32_t material[4];
    std::uint32_t owner[4];
};
static_assert(sizeof(PartRecord) == 128);

// the semantic slot of a declaration element (kSlotCount = the table has no place for it)
inline std::uint32_t slotOfElement(std::uint8_t usage, std::uint8_t usageIndex) {
    using U = shader::Usage;
    switch (static_cast<U>(usage)) {
    case U::Position: return usageIndex == 0 ? kSlotPosition : kSlotCount;
    case U::BlendWeight: return usageIndex == 0 ? kSlotBlendWeight : kSlotCount;
    case U::BlendIndices: return usageIndex == 0 ? kSlotBlendIndices : kSlotCount;
    case U::Normal: return usageIndex == 0 ? kSlotNormal : kSlotCount;
    case U::Tangent: return usageIndex == 0 ? kSlotTangent : kSlotCount;
    case U::Binormal: return usageIndex == 0 ? kSlotBinormal : kSlotCount;
    case U::Color: return usageIndex < 2 ? kSlotColor0 + usageIndex : kSlotCount;
    case U::TexCoord: return usageIndex < 8 ? kSlotTexcoord0 + usageIndex : kSlotCount;
    default: return kSlotCount;
    }
}

// bytes of one element of a D3DDECLTYPE (0 for an unknown type)
inline std::uint32_t declTypeBytes(std::uint32_t t) {
    switch (t) {
    case d3d9::DECLTYPE_FLOAT1: case d3d9::DECLTYPE_D3DCOLOR: case d3d9::DECLTYPE_UBYTE4: case d3d9::DECLTYPE_SHORT2: case d3d9::DECLTYPE_UBYTE4N:
    case d3d9::DECLTYPE_SHORT2N: case d3d9::DECLTYPE_USHORT2N: case d3d9::DECLTYPE_UDEC3: case d3d9::DECLTYPE_DEC3N: case d3d9::DECLTYPE_FLOAT16_2: return 4;
    case d3d9::DECLTYPE_FLOAT2: case d3d9::DECLTYPE_SHORT4: case d3d9::DECLTYPE_SHORT4N: case d3d9::DECLTYPE_USHORT4N: case d3d9::DECLTYPE_FLOAT16_4: return 8;
    case d3d9::DECLTYPE_FLOAT3: return 12;
    case d3d9::DECLTYPE_FLOAT4: return 16;
    default: return 0;
    }
}

inline std::uint64_t hashElements(std::span<const d3d9::VertexElement> els) {
    std::uint64_t h = 1469598103934665603ull;
    for (const auto& e : els) {
        std::uint64_t v = 0;
        std::memcpy(&v, &e, sizeof(v));
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

inline LayoutRecord buildLayout(std::span<const d3d9::VertexElement> els) {
    LayoutRecord r{};
    r.info[0] = 1;
    r.info[1] = static_cast<std::uint32_t>(hashElements(els) ^ (hashElements(els) >> 32));
    r.info[2] = static_cast<std::uint32_t>(els.size());
    for (const auto& e : els) {
        const std::uint32_t s = slotOfElement(e.usage, e.usageIndex);
        if (s >= kSlotCount || r.slot[s] != 0 || declTypeBytes(e.type) == 0 || e.stream > 15) {
            ++r.info[3];
            continue;
        }
        r.slot[s] = static_cast<std::uint32_t>(e.offset) | (static_cast<std::uint32_t>(e.type) + 1u) << 16 | static_cast<std::uint32_t>(e.stream) << 24;
    }
    return r;
}

inline std::uint32_t slotOffset(std::uint32_t w) { return w & 0xffffu; }
inline std::uint32_t slotType(std::uint32_t w) { return (w >> 16 & 0xffu) - 1u; }  // (only for w != 0)
inline std::uint32_t slotStream(std::uint32_t w) { return w >> 24 & 0xfu; }

inline float halfToFloat(std::uint16_t h) {
    const std::uint32_t sign = static_cast<std::uint32_t>(h >> 15) << 31;
    const std::uint32_t exp = h >> 10 & 0x1fu, man = h & 0x3ffu;
    std::uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;
        } else {
            std::uint32_t e = 127 - 15 + 1, m = man;  // renormalise a subnormal
            while ((m & 0x400u) == 0) {
                m <<= 1;
                --e;
            }
            bits = sign | e << 23 | (m & 0x3ffu) << 13;
        }
    } else if (exp == 31) {
        bits = sign | 0x7f800000u | man << 13;
    } else {
        bits = sign | (exp + 127 - 15) << 23 | man << 13;
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// One declaration element as a D3D9 vertex shader input sees it: missing components 0, 0, 0, 1; D3DCOLOR as (R, G, B, A) / 255; UBYTE4 / SHORT unscaled;
// the N types normalised (signed: clamped at -1). `bytes` = declTypeBytes(type) bytes (any alignment).
inline void decodeElement(std::uint32_t type, const std::uint8_t* b, float out[4]) {
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    const auto f32 = [&](std::uint32_t i) {
        float f;
        std::memcpy(&f, b + 4 * i, 4);
        return f;
    };
    const auto u16 = [&](std::uint32_t i) {
        std::uint16_t v;
        std::memcpy(&v, b + 2 * i, 2);
        return v;
    };
    const auto s16 = [&](std::uint32_t i) { return static_cast<std::int16_t>(u16(i)); };
    const auto snorm16 = [&](std::uint32_t i) { return std::max(static_cast<float>(s16(i)) / 32767.0f, -1.0f); };
    std::uint32_t w = 0;
    std::memcpy(&w, b, 4);
    switch (type) {
    case d3d9::DECLTYPE_FLOAT1: out[0] = f32(0); break;
    case d3d9::DECLTYPE_FLOAT2: out[0] = f32(0); out[1] = f32(1); break;
    case d3d9::DECLTYPE_FLOAT3: out[0] = f32(0); out[1] = f32(1); out[2] = f32(2); break;
    case d3d9::DECLTYPE_FLOAT4: for (std::uint32_t i = 0; i < 4; ++i) out[i] = f32(i); break;
    case d3d9::DECLTYPE_D3DCOLOR:
        out[0] = static_cast<float>(w >> 16 & 0xffu) / 255.0f;
        out[1] = static_cast<float>(w >> 8 & 0xffu) / 255.0f;
        out[2] = static_cast<float>(w & 0xffu) / 255.0f;
        out[3] = static_cast<float>(w >> 24) / 255.0f;
        break;
    case d3d9::DECLTYPE_UBYTE4: for (std::uint32_t i = 0; i < 4; ++i) out[i] = static_cast<float>(b[i]); break;
    case d3d9::DECLTYPE_UBYTE4N: for (std::uint32_t i = 0; i < 4; ++i) out[i] = static_cast<float>(b[i]) / 255.0f; break;
    case d3d9::DECLTYPE_SHORT2: out[0] = s16(0); out[1] = s16(1); break;
    case d3d9::DECLTYPE_SHORT4: for (std::uint32_t i = 0; i < 4; ++i) out[i] = s16(i); break;
    case d3d9::DECLTYPE_SHORT2N: out[0] = snorm16(0); out[1] = snorm16(1); break;
    case d3d9::DECLTYPE_SHORT4N: for (std::uint32_t i = 0; i < 4; ++i) out[i] = snorm16(i); break;
    case d3d9::DECLTYPE_USHORT2N: out[0] = static_cast<float>(u16(0)) / 65535.0f; out[1] = static_cast<float>(u16(1)) / 65535.0f; break;
    case d3d9::DECLTYPE_USHORT4N: for (std::uint32_t i = 0; i < 4; ++i) out[i] = static_cast<float>(u16(i)) / 65535.0f; break;
    case d3d9::DECLTYPE_UDEC3:
        out[0] = static_cast<float>(w & 0x3ffu);
        out[1] = static_cast<float>(w >> 10 & 0x3ffu);
        out[2] = static_cast<float>(w >> 20 & 0x3ffu);
        break;
    case d3d9::DECLTYPE_DEC3N:
        for (std::uint32_t i = 0; i < 3; ++i) {
            const std::int32_t v = static_cast<std::int32_t>(w << (22 - 10 * i)) >> 22;
            out[i] = std::max(static_cast<float>(v) / 511.0f, -1.0f);
        }
        break;
    case d3d9::DECLTYPE_FLOAT16_2: out[0] = halfToFloat(u16(0)); out[1] = halfToFloat(u16(1)); break;
    case d3d9::DECLTYPE_FLOAT16_4: for (std::uint32_t i = 0; i < 4; ++i) out[i] = halfToFloat(u16(i)); break;
    default: break;
    }
}

}  // namespace rvk::gpuscene
