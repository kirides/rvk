// The GPU scene's MeshPart + Layout tables (src/gpuscene/mesh_layout.h records, mesh_parts.*; frame-ownership.md 1.2 / 1.4): vertex pulling from the mesh
// arena by address. A part = one sub-mesh's index range (index buffer address, first index, count, per-stream vertex addresses + strides, base vertex, layout
// id, material key); a layout = the declaration as per-semantic words (offset | type + 1 << 16 | stream << 24, 0 = absent). Draws use vertexOffset = 0 (D3D12's
// SV_VertexID ignores it): the part's base vertex is added here. Raw word reads from address pairs (S1 rules: no typed block arrays, no uint64 in push blocks);
// vertex data is only 2-byte aligned in general, so every element is read through rvkMeshWord (any alignment). Values follow D3D9's vertex shader input rules
// (src/gpuscene/mesh_layout.h `decodeElement` is the CPU twin).
#pragma once

#include "rvk_common.hlsli"

static const uint RVK_MESH_PART_STRIDE = 128u;
static const uint RVK_MESH_LAYOUT_STRIDE = 80u;
static const uint RVK_MESH_PART_INARENA = 1u;
static const uint RVK_MESH_PART_INDEX32 = 2u;

static const uint RVK_MESH_SLOT_POSITION = 0u;
static const uint RVK_MESH_SLOT_BLENDWEIGHT = 1u;
static const uint RVK_MESH_SLOT_BLENDINDICES = 2u;
static const uint RVK_MESH_SLOT_NORMAL = 3u;
static const uint RVK_MESH_SLOT_TANGENT = 4u;
static const uint RVK_MESH_SLOT_BINORMAL = 5u;
static const uint RVK_MESH_SLOT_COLOR0 = 6u;
static const uint RVK_MESH_SLOT_COLOR1 = 7u;
static const uint RVK_MESH_SLOT_TEXCOORD0 = 8u;

struct RvkMeshPart {
    uint4 info;      // x live, y layout id, z generation, w flags
    uint4 index;     // xy index buffer address, z first index, w index count
    uint4 range;     // x base vertex (int bits), y min index, z vertex count, w index size
    uint4 s0;        // stream 0..2: xy address of the stream's data, z stride, w bound
    uint4 s1;
    uint4 s2;
    uint4 material;  // xy material pointer, zw sub-mesh pointer
    uint4 owner;     // xy mesh resource pointer
};

struct RvkMeshLayout {
    RvkAddr at;  // the record's address
};

RvkMeshPart rvkMeshLoadPart(uint2 table, uint slot) {
    const RvkAddr a = rvkAddrAdd(rvkAddr(table), slot * RVK_MESH_PART_STRIDE);
    RvkMeshPart p;
    p.info = rvkRawLoad<uint4, 16>(a);
    p.index = rvkRawLoad<uint4, 16>(rvkAddrAdd(a, 16u));
    p.range = rvkRawLoad<uint4, 16>(rvkAddrAdd(a, 32u));
    p.s0 = rvkRawLoad<uint4, 16>(rvkAddrAdd(a, 48u));
    p.s1 = rvkRawLoad<uint4, 16>(rvkAddrAdd(a, 64u));
    p.s2 = rvkRawLoad<uint4, 16>(rvkAddrAdd(a, 80u));
    p.material = rvkRawLoad<uint4, 16>(rvkAddrAdd(a, 96u));
    p.owner = rvkRawLoad<uint4, 16>(rvkAddrAdd(a, 112u));
    return p;
}

RvkMeshLayout rvkMeshLoadLayout(uint2 table, uint id) {
    RvkMeshLayout l;
    l.at = rvkAddrAdd(rvkAddr(table), id * RVK_MESH_LAYOUT_STRIDE);
    return l;
}

uint rvkMeshLayoutSlot(RvkMeshLayout l, uint slot) { return rvkRawLoad<uint, 4>(rvkAddrAdd(l.at, 16u + 4u * slot)); }

// 32 bits at any byte address
uint rvkMeshWord(RvkAddr a) {
    const uint sh = (rvkAddrLow(a) & 3u) * 8u;
    const RvkAddr b = rvkAddrAlign4(a);
    const uint lo = rvkRawLoad<uint, 4>(b);
    if (sh == 0u) return lo;
    const uint hi = rvkRawLoad<uint, 4>(rvkAddrAdd(b, 4u));
    return (lo >> sh) | (hi << (32u - sh));
}

float rvkMeshS16(uint v) { return float(int(v << 16u) >> 16); }
float rvkMeshU16(uint v) { return float(v & 0xffffu); }
float rvkMeshSnorm16(uint v) { return max(rvkMeshS16(v) / 32767.0, -1.0); }

// one declaration element (D3DDECLTYPE `type`) at byte address `a`, as a D3D9 vertex shader input
float4 rvkMeshDecode(uint type, RvkAddr a) {
    float4 o = float4(0.0, 0.0, 0.0, 1.0);
    const uint w0 = rvkMeshWord(a);
    switch (type) {
    case 0u: o.x = asfloat(w0); break;
    case 1u: o.xy = float2(asfloat(w0), asfloat(rvkMeshWord(rvkAddrAdd(a, 4u)))); break;
    case 2u: o.xyz = float3(asfloat(w0), asfloat(rvkMeshWord(rvkAddrAdd(a, 4u))), asfloat(rvkMeshWord(rvkAddrAdd(a, 8u)))); break;
    case 3u: o = float4(asfloat(w0), asfloat(rvkMeshWord(rvkAddrAdd(a, 4u))), asfloat(rvkMeshWord(rvkAddrAdd(a, 8u))), asfloat(rvkMeshWord(rvkAddrAdd(a, 12u)))); break;
    case 4u: o = float4(float((w0 >> 16u) & 0xffu), float((w0 >> 8u) & 0xffu), float(w0 & 0xffu), float(w0 >> 24u)) / 255.0; break;  // D3DCOLOR
    case 5u: o = float4(float(w0 & 0xffu), float((w0 >> 8u) & 0xffu), float((w0 >> 16u) & 0xffu), float(w0 >> 24u)); break;           // UBYTE4
    case 6u: o.xy = float2(rvkMeshS16(w0), rvkMeshS16(w0 >> 16u)); break;                                                            // SHORT2
    case 7u: {
        const uint w1 = rvkMeshWord(rvkAddrAdd(a, 4u));
        o = float4(rvkMeshS16(w0), rvkMeshS16(w0 >> 16u), rvkMeshS16(w1), rvkMeshS16(w1 >> 16u));
        break;
    }
    case 8u: o = float4(float(w0 & 0xffu), float((w0 >> 8u) & 0xffu), float((w0 >> 16u) & 0xffu), float(w0 >> 24u)) / 255.0; break;  // UBYTE4N
    case 9u: o.xy = float2(rvkMeshSnorm16(w0), rvkMeshSnorm16(w0 >> 16u)); break;                                                    // SHORT2N
    case 10u: {
        const uint w1 = rvkMeshWord(rvkAddrAdd(a, 4u));
        o = float4(rvkMeshSnorm16(w0), rvkMeshSnorm16(w0 >> 16u), rvkMeshSnorm16(w1), rvkMeshSnorm16(w1 >> 16u));
        break;
    }
    case 11u: o.xy = float2(rvkMeshU16(w0), rvkMeshU16(w0 >> 16u)) / 65535.0; break;                                                 // USHORT2N
    case 12u: {
        const uint w1 = rvkMeshWord(rvkAddrAdd(a, 4u));
        o = float4(rvkMeshU16(w0), rvkMeshU16(w0 >> 16u), rvkMeshU16(w1), rvkMeshU16(w1 >> 16u)) / 65535.0;
        break;
    }
    case 13u: o.xyz = float3(float(w0 & 0x3ffu), float((w0 >> 10u) & 0x3ffu), float((w0 >> 20u) & 0x3ffu)); break;                   // UDEC3
    case 14u:                                                                                                                         // DEC3N
        o.xyz = max(float3(float(int(w0 << 22u) >> 22), float(int(w0 << 12u) >> 22), float(int(w0 << 2u) >> 22)) / 511.0, float3(-1.0, -1.0, -1.0));
        break;
    case 15u: o.xy = float2(f16tof32(w0 & 0xffffu), f16tof32(w0 >> 16u)); break;                                                      // FLOAT16_2
    case 16u: {
        const uint w1 = rvkMeshWord(rvkAddrAdd(a, 4u));
        o = float4(f16tof32(w0 & 0xffffu), f16tof32(w0 >> 16u), f16tof32(w1 & 0xffffu), f16tof32(w1 >> 16u));
        break;
    }
    default: break;
    }
    return o;
}

// the byte address of vertex `vertexIndex` (an absolute index from the index buffer, before the part's base vertex) of stream `stream`
RvkAddr rvkMeshVertexAddress(RvkMeshPart part, uint stream, uint vertexIndex) {
    const uint4 s = stream == 0u ? part.s0 : (stream == 1u ? part.s1 : part.s2);
    const uint v = uint(int(vertexIndex) + int(part.range.x));
    return rvkAddrAdd(rvkAddr(s.xy), v * s.z);
}

// the semantic slot's element of a vertex; float4(0) when the layout has none (or its stream is not one of the part's three)
float4 rvkMeshFetch(RvkMeshPart part, RvkMeshLayout layout, uint semantic, uint vertexIndex) {
    const uint w = rvkMeshLayoutSlot(layout, semantic);
    if (w == 0u) return float4(0.0, 0.0, 0.0, 0.0);
    const uint stream = w >> 24u;
    if (stream > 2u) return float4(0.0, 0.0, 0.0, 0.0);
    return rvkMeshDecode(((w >> 16u) & 0xffu) - 1u, rvkAddrAdd(rvkMeshVertexAddress(part, stream, vertexIndex), w & 0xffffu));
}

float3 rvkMeshFetchPosition(RvkMeshPart part, RvkMeshLayout layout, uint vertexIndex) { return rvkMeshFetch(part, layout, RVK_MESH_SLOT_POSITION, vertexIndex).xyz; }
float3 rvkMeshFetchNormal(RvkMeshPart part, RvkMeshLayout layout, uint vertexIndex) { return rvkMeshFetch(part, layout, RVK_MESH_SLOT_NORMAL, vertexIndex).xyz; }
float2 rvkMeshFetchUv(RvkMeshPart part, RvkMeshLayout layout, uint set, uint vertexIndex) {
    return rvkMeshFetch(part, layout, RVK_MESH_SLOT_TEXCOORD0 + min(set, 7u), vertexIndex).xy;
}

// the k-th index of the part's range (an absolute vertex index)
uint rvkMeshFetchIndex(RvkMeshPart part, uint k) {
    const uint size = part.range.w;
    const RvkAddr a = rvkAddrAdd(rvkAddr(part.index.xy), (part.index.z + k) * size);
    const uint w = rvkMeshWord(a);
    return size == 4u ? w : (w & 0xffffu);
}
