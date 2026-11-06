// The illumination table (src/gpuscene/illumination_table.*, illum_compose.h; frame-ownership.md F0): one record per illumination object of the world
// (288 bytes, RvkIllum) and one per light set (336 bytes, RvkLightSet), kept on the GPU by batched uploads. The records hold the engine's RAW bytes
// that StaticOps::writeLighting reads; `rvkIllumCompose` rebuilds the lighting registers c14 .. c37 of a lit draw from them exactly as that function
// (src/gpuscene/illum_compose.h `composeLighting`, the CPU twin) does. Raw word reads from address pairs (S1 rules): no typed block arrays.
#pragma once

#include "rvk_common.hlsli"

static const uint RVK_ILLUM_STRIDE = 288u;
static const uint RVK_LIGHTSET_STRIDE = 336u;
static const uint RVK_ILLUM_INFO = 16u;       // the raw bytes start after the uint4 info
static const uint RVK_ILLUM_NO_SET = 0xffffffffu;

struct RvkIllumParams {
    uint tableLit;    // 1 = the table's 3-light permutation
    uint holdsAll;    // 1 = the table holds every light of this object (the light registers stay zero)
    uint n;           // lights the pass takes
    uint firstLight;
};

RvkAddr rvkIllumRecord(uint2 table, uint slot) { return rvkAddrAdd(rvkAddr(table), slot * RVK_ILLUM_STRIDE); }
RvkAddr rvkLightSetRecord(uint2 table, uint slot) { return rvkAddrAdd(rvkAddr(table), slot * RVK_LIGHTSET_STRIDE); }

// raw field reads: `rec` = a record's address, `off` = the byte offset in the engine object (a multiple of 4)
uint rvkIllumRawU32(RvkAddr rec, uint off) { return rvkRawLoad<uint, 4>(rvkAddrAdd(rec, RVK_ILLUM_INFO + off)); }
float rvkIllumRawF32(RvkAddr rec, uint off) { return asfloat(rvkIllumRawU32(rec, off)); }
float4 rvkIllumRawF4(RvkAddr rec, uint off) {
    return float4(rvkIllumRawF32(rec, off), rvkIllumRawF32(rec, off + 4u), rvkIllumRawF32(rec, off + 8u), rvkIllumRawF32(rec, off + 12u));
}
uint rvkIllumRawByte(RvkAddr rec, uint off) { return (rvkIllumRawU32(rec, off & ~3u) >> ((off & 3u) * 8u)) & 0xffu; }

// L[r - 14] = the lighting register r (c14 .. c37); registers the engine function does not write are left as they are (c14.w).
// Returns false when the pass reads beyond the raw bytes (a light index past them).
bool rvkIllumCompose(uint2 illumTable, uint illumSlot, uint2 setTable, RvkIllumParams p, inout float4 L[24]) {
    const RvkAddr ir = rvkIllumRecord(illumTable, illumSlot);
    const uint setSlot = rvkRawLoad<uint, 4>(rvkAddrAdd(ir, 4u));
    if (setSlot == RVK_ILLUM_NO_SET) return false;
    const RvkAddr sr = rvkLightSetRecord(setTable, setSlot);

    const float3 indoor = float3(rvkIllumRawF32(sr, 0x128u), rvkIllumRawF32(sr, 0x12cu), rvkIllumRawF32(sr, 0x130u));
    L[0].xyz = indoor;
    const float f = rvkIllumRawF32(ir, 8u);
    const float g = 1.0 - f;
    const bool hasStatic = rvkIllumRawByte(ir, 0x90u) != 0u;
    [unroll] for (uint k = 0u; k < 7u; ++k) {
        // (precise: no fused multiply-add, the CPU twin rounds the product and the sum separately: bit-exact registers)
        precise float4 v = rvkIllumRawF4(sr, 0xb8u + 16u * k) * f;
        if (k < 3u) v.w = g * indoor[k] + v.w;
        if (hasStatic) v += rvkIllumRawF4(ir, 0x94u + 16u * k);
        L[1 + k] = v;
    }
    [unroll] for (uint s = 0u; s < 3u; ++s) L[8 + s] = rvkIllumRawF4(sr, 4u + 16u * s);
    if (p.holdsAll != 0u) {
        [unroll] for (uint z = 0u; z < 12u; ++z) L[11 + z] = float4(0, 0, 0, 0);
        return true;
    }
    const uint nStatic = rvkIllumRawByte(ir, 0x0du);
    const uint nDynamic = rvkIllumRawByte(sr, 0x34u);
    const uint have = (p.tableLit != 0u) ? min(3u, nStatic + nDynamic) : p.n;
    if (p.tableLit != 0u) {
        [unroll] for (uint z = 0u; z < 12u; ++z) L[11 + z] = float4(0, 0, 0, 0);
    }
    bool ok = true;
    const uint count = min(p.n, have);
    [unroll] for (uint j = 0u; j < 3u; ++j) {
        if (j >= count) continue;
        const uint idx = p.firstLight + j;
        RvkAddr rec = ir;
        uint off = 0x10u + 0x40u * idx;
        uint limit = 0x110u;
        if (idx >= nStatic) {
            rec = sr;
            off = 0x38u + 0x40u * (idx - nStatic);
            limit = 0x140u;
        }
        if (off + 64u > limit) {
            ok = false;
            continue;
        }
        L[11 + 4 * j + 0] = rvkIllumRawF4(rec, off);
        L[11 + 4 * j + 1] = rvkIllumRawF4(rec, off + 16u);
        L[11 + 4 * j + 2] = rvkIllumRawF4(rec, off + 32u);
        L[11 + 4 * j + 3] = rvkIllumRawF4(rec, off + 48u);
    }
    return ok;
}
