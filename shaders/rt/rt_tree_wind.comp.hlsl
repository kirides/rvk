// SpeedTrees in the wind for the ray tracing scene without a vertex capture (src/enh/rt_scene.cpp observeTree, docs/research/frame-ownership.md 14.17, F4-5):
// the position part of the engine's SpeedTree vertex shader (ge_default_vs.fx Transform with XFX_SPEEDTREE: the port in shaders/material/ge_default.vert.hlsl)
// evaluated per tree instance from the draw's own inputs: the vertex buffer (by address, the declaration's attribute words) and the constants the engine gives
// the shader (c14..c22 the wind matrices, c23..c25 the leaf angle matrix, c26..c29 the rock / rustle angles, c32..c35 the instance's row: c32.xy rock / rustle
// scales, c33..c35 the world rows). The result has the layout of the capture variant's buffer (16-byte header, then per vertex index two float4: the world
// position (w = 1) and TEXCOORD0), so the BLAS refits, the material vertex reads and everything downstream take it as they took the capture.
//
//   Position.xyz = lerp(pos, pos * WindMatrix[indices0.x], weights0.x)
//   leaf mesh:  offset = MeshMatrix(normal1, normal2) * RotX(rock) * weights2.xyz
//   leaf cards: offset = LeafAngleMatrix * RotY(weights1.z + rustle) * RotZ(weights1.w + rock) * (weights1.yx * weights2.yz, 0)
//   world = (dot(Position, r0), dot(Position, r1), dot(Position, r2)) + offset
//
// One dispatch for the frame's jobs (one job = one tree instance): workgroup g evaluates 64 vertex slots of the job whose chunk range holds g. Compare mode
// (native.tree_wind=compare): a job may carry the vertex shader capture of the same instance; slots it wrote are compared (max error, counts).

#include "rvk_common.hlsli"

struct TreeWindPush {
    RVK_PASS_PUSH_HEADER
    uint2 table;
    uint2 pad;
};
RVK_PUSH(TreeWindPush, pc);

// the job table: head = (jobs, chunks, the compare accumulator's address (0 = none)), then 5 uint4 per job:
// job[5 j + 0] = (vertex slot 0's address lo, hi, constants lo, hi)      constants: 20 float4 = c14..c29, c32..c35
// job[5 j + 1] = (output lo, hi, slots, vertex stride in bytes)
// job[5 j + 2] = (first chunk, class (0 wind only, 1 leaf mesh, 2 leaf cards), reference capture lo, hi)
// job[5 j + 3] = attribute words (offset | D3DDECLTYPE << 16, 0xFFFFFFFF = absent): POSITION0, BLENDINDICES0, BLENDWEIGHT0, BLENDWEIGHT1
// job[5 j + 4] = attribute words: BLENDWEIGHT2, NORMAL1, NORMAL2, TEXCOORD0
// the accumulator: maxBits, compared, missing, over 0.1 units, over 1 unit, max bits per class (wind, leaf mesh, leaf cards)
static const uint TW_ABSENT = 0xFFFFFFFFu;

uint4 jobWord(RvkAddr table, uint i) { return rvkRawLoad<uint4, 16>(rvkAddrAdd(table, 16u + i * 16u)); }
uint twWord(RvkAddr a) { return rvkRawLoad<uint, 4>(a); }
float twSext16(uint v) { return float(int(v << 16) >> 16); }
float2 twSnorm2(uint w) { return clamp(float2(twSext16(w), twSext16(w >> 16)) / 32767.0, -1.0, 1.0); }
float2 twUnorm2(uint w) { return float2(w & 0xFFFFu, w >> 16) / 65535.0; }
float2 twHalf2(uint w) { return float2(f16tof32(w & 0xFFFFu), f16tof32(w >> 16)); }
float4 twUnorm4(uint w) { return float4(w & 0xFFu, (w >> 8) & 0xFFu, (w >> 16) & 0xFFu, w >> 24) / 255.0; }

// a vertex attribute as the D3D9 vertex shader reads it (the input assembler's conversions: missing components = 0, w = 1; absent = (0, 0, 0, 1))
float4 twAttr(RvkAddr vertex, uint word) {
    if (word == TW_ABSENT) return float4(0.0, 0.0, 0.0, 1.0);
    uint type = word >> 16;
    RvkAddr a = rvkAddrAdd(vertex, word & 0xFFFFu);
    uint w0 = twWord(a);
    switch (type) {
    case 0u: return float4(asfloat(w0), 0.0, 0.0, 1.0);
    case 1u: return float4(asfloat(w0), asfloat(twWord(rvkAddrAdd(a, 4u))), 0.0, 1.0);
    case 2u: return float4(asfloat(w0), asfloat(twWord(rvkAddrAdd(a, 4u))), asfloat(twWord(rvkAddrAdd(a, 8u))), 1.0);
    case 3u: return float4(asfloat(w0), asfloat(twWord(rvkAddrAdd(a, 4u))), asfloat(twWord(rvkAddrAdd(a, 8u))), asfloat(twWord(rvkAddrAdd(a, 12u))));
    case 4u: return twUnorm4(w0).zyxw;  // D3DCOLOR: BGRA bytes
    case 5u: return float4(uint4(w0, w0 >> 8, w0 >> 16, w0 >> 24) & 0xFFu);  // UBYTE4
    case 6u: return float4(twSext16(w0), twSext16(w0 >> 16), 0.0, 1.0);
    case 7u: {
        uint w1 = twWord(rvkAddrAdd(a, 4u));
        return float4(twSext16(w0), twSext16(w0 >> 16), twSext16(w1), twSext16(w1 >> 16));
    }
    case 8u: return twUnorm4(w0);
    case 9u: return float4(twSnorm2(w0), 0.0, 1.0);
    case 10u: return float4(twSnorm2(w0), twSnorm2(twWord(rvkAddrAdd(a, 4u))));
    case 11u: return float4(twUnorm2(w0), 0.0, 1.0);
    case 12u: return float4(twUnorm2(w0), twUnorm2(twWord(rvkAddrAdd(a, 4u))));
    case 15u: return float4(twHalf2(w0), 0.0, 1.0);
    case 16u: return float4(twHalf2(w0), twHalf2(twWord(rvkAddrAdd(a, 4u))));
    default: return float4(0.0, 0.0, 0.0, 1.0);
    }
}

// the job's constants: index 0..15 = c14..c29, 16..19 = c32..c35; any other register the shader would read is outside them (0, as the port's vsC for registers it
// does not hold: the wind index / leaf angle index of a real vertex stays inside)
float4 twC(RvkAddr consts, int i) {
    if (i < 0 || i >= 20) return float4(0.0, 0.0, 0.0, 0.0);
    return rvkRawLoad<float4, 16>(rvkAddrAdd(consts, uint(i) * 16u));
}
// c(reg) of the vertex shader for reg in 14..29
float4 twReg(RvkAddr consts, int reg) { return reg >= 14 && reg <= 29 ? twC(consts, reg - 14) : float4(0.0, 0.0, 0.0, 0.0); }

float3x3 twRotX(float a) {
    float2 sc = float2(sin(a), cos(a));
    return float3x3(float3(1.0, 0.0, 0.0), float3(0.0, sc.y, -sc.x), float3(0.0, sc.x, sc.y));
}
float3x3 twRotY(float a) {
    float2 sc = float2(sin(a), cos(a));
    return float3x3(float3(sc.y, 0.0, sc.x), float3(0.0, 1.0, 0.0), float3(-sc.x, 0.0, sc.y));
}
float3x3 twRotZ(float a) {
    float2 sc = float2(sin(a), cos(a));
    return float3x3(float3(sc.y, -sc.x, 0.0), float3(sc.x, sc.y, 0.0), float3(0.0, 0.0, 1.0));
}

[numthreads(64, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID) {
    RvkAddr table = rvkAddr(pc.table);
    uint4 head = rvkRawLoad<uint4, 16>(table);
    uint jobs = head.x;
    uint g = gid.x;
    uint lo = 0u, hi = jobs;
    while (hi - lo > 1u) {
        uint mid = (lo + hi) >> 1;
        if (jobWord(table, 5u * mid + 2u).x <= g) lo = mid; else hi = mid;
    }
    uint4 j0 = jobWord(table, 5u * lo), j1 = jobWord(table, 5u * lo + 1u), j2 = jobWord(table, 5u * lo + 2u), j3 = jobWord(table, 5u * lo + 3u),
          j4 = jobWord(table, 5u * lo + 4u);
    uint slot = (g - j2.x) * 64u + lid.x;
    uint slots = j1.z;
    if (slot >= slots) return;
    RvkAddr v = rvkAddrAdd(rvkAddr(j0.xy), slot * j1.w);
    RvkAddr k = rvkAddr(j0.zw);

    float4 inPos = twAttr(v, j3.x);
    int windIndex = int(twAttr(v, j3.y).x);  // (the port: int4(BLENDINDICES0); UBYTE4 = the byte values)
    float4 w0 = twAttr(v, j3.z);
    // wind (mulWindMatrix: the 3x3 with the columns c(14 + 3 i), c(15 + 3 i), c(16 + 3 i), v * M)
    float3 windPos = float3(dot(inPos.xyz, twReg(k, 14 + 3 * windIndex).xyz), dot(inPos.xyz, twReg(k, 15 + 3 * windIndex).xyz), dot(inPos.xyz, twReg(k, 16 + 3 * windIndex).xyz));
    float4 position = inPos;
    position.xyz = lerp(inPos.xyz, windPos, w0.x);

    float3 offset = float3(0.0, 0.0, 0.0);
    uint cls = j2.y;
    if (cls != 0u) {
        float4 w1 = twAttr(v, j3.w);
        float4 w2 = twAttr(v, j4.x);
        float4 inst = twC(k, 16);  // c32 of the instance (g_vInstanceData(vInstance * 4))
        float2 rockAndRustle = inst.xy * twReg(k, 26 + int(w2.w)).xy;
        if (cls == 1u) {
            // leaf mesh: placement using SpeedWind leaf rocking
            float3 n1 = twAttr(v, j4.y).xyz, n2 = twAttr(v, j4.z).xyz;
            float3x3 meshMatrix = float3x3(n1, -cross(n1, n2), n2);
            offset = mul(twRotX(rockAndRustle.x), w2.xyz);
            offset = mul(meshMatrix, offset);
        } else {
            // leaf cards
            offset = float3(w1.yx * w2.yz, 0.0);
            offset = mul(twRotZ(w1.w + rockAndRustle.x), offset);
            offset = mul(twRotY(w1.z + rockAndRustle.y), offset);
            // g_vLeafAngleMatrix = the 3x3 with the columns c23, c24, c25: M * v
            offset = twReg(k, 23).xyz * offset.x + twReg(k, 24).xyz * offset.y + twReg(k, 25).xyz * offset.z;
        }
    }
    float4 r0 = twC(k, 17), r1 = twC(k, 18), r2 = twC(k, 19);
    float3 world = float3(dot(position, r0), dot(position, r1), dot(position, r2)) + offset;
    float4 tc = j4.w == TW_ABSENT ? float4(0.0, 0.0, 0.0, 0.0) : twAttr(v, j4.w);

    uint2 refAddr = j2.zw;
    if (any(refAddr != uint2(0u, 0u)) && any(head.zw != uint2(0u, 0u))) {
        // compare: the vertex shader's own result for this instance (its capture variant), where it wrote this slot
        RvkAddr r = rvkAddr(refAddr);
        float4 rp = rvkRwLoad<float4, 16>(rvkAddrAdd(r, slot * 32u));
        RvkAddr e = rvkAddr(head.zw);
        if (rp.w == 1.0) {
            float err = length(rp.xyz - world);
            rvkRwAtomicMax(e, asuint(err));
            rvkRwAtomicAdd(rvkAddrAdd(e, 4u), 1u);
            if (err > 0.1) rvkRwAtomicAdd(rvkAddrAdd(e, 12u), 1u);
            if (err > 1.0) rvkRwAtomicAdd(rvkAddrAdd(e, 16u), 1u);
            rvkRwAtomicMax(rvkAddrAdd(e, 20u + 4u * min(cls, 2u)), asuint(err));
        } else {
            rvkRwAtomicAdd(rvkAddrAdd(e, 8u), 1u);
        }
    }
    RvkAddr o = rvkAddr(j1.xy);
    rvkRwStore<float4, 16>(rvkAddrAdd(o, 16u + slot * 32u), float4(world, 1.0));
    rvkRwStore<float4, 16>(rvkAddrAdd(o, 16u + slot * 32u + 16u), tc);
}
