// Characters skinned for the ray tracing scene (src/enh/rt_scene.cpp, docs/research/rt-shadows.md 10): the engine's skinned vertex
// shader (ge_default_vs.fx, hardware skinning) evaluated once per frame per draw from the draw's own inputs: the vertex buffer
// (POSITION F3, BLENDWEIGHT0 F4, BLENDINDICES UBYTE4, TEXCOORD0 F2) and the bone matrices the engine put into the vertex shader
// constants (bone b at c33 + 3 b: three rows of a 3x4 matrix, `g_vInstanceData`), copied at the draw. The result has the layout
// of the capture variant's buffer (vertex shader writes of the draw: 16-byte header, then per vertex index i two float4s: the
// world position (w = 1) and TEXCOORD0), so the BLAS builds, the material vertex reads and the motion code take it as it is.
//
//   world = sum_k w_k * (bone row_j(b_k) . (position, 1)),  j = 0..2          (the VS: `dp4 r.x, v0, r0` with r0 = blended c33[a0])
//
// One dispatch for all of the frame's jobs: workgroup g skins 64 vertex slots of the job whose chunk range holds g.

#include "rvk_common.hlsli"

struct SkinPush {
    RVK_PASS_PUSH_HEADER
    uint2 table;
    uint2 pad;
};
RVK_PUSH(SkinPush, pc);

// the job table: head = (jobs, chunks, the error accumulator's address (0 = none: not verifying)), then 4 uint4 per job (below)
struct SkinErr {
    uint maxBits;
    uint compared;
    uint missing;
    uint pad;
};

// job[8 j + 0] = (vertex 0's address lo, hi, bones lo, hi)
// job[8 j + 1] = (output lo, hi, slots, vertex stride in bytes)
// job[8 j + 2] = (first chunk, morph targets, reference capture lo, hi)   (the reference: a capture variant's output of the same draw)
// job[8 j + 3] = (offsets: position | blend weight << 8 | blend indices << 16 | texcoord0 << 24, 0, 0, 0)
// job[8 j + 4] = (morph target 1's vertex 0 lo, hi, target 2's lo, hi)         (14.15 c: POSITION1..3 F3 in streams 1..3, the heads')
// job[8 j + 5] = (target 3's lo, hi, strides s1 | s2 << 8 | s3 << 16, position offsets o1 | o2 << 8 | o3 << 16)
// job[8 j + 6] = (weights c31.x, .y, .z as float bits, 0); job[8 j + 2].y = the targets read (bit k = target k + 1)
// job[8 j + 7] = reserved
//   position = v0 + d1 * w.x + d2 * w.y + d3 * w.z   (the VS: `mad r0.xyz, v8, c31.x, r0` in that order), then skinned
static const uint BONE_COUNT = 66;  // c33 .. c230: 3 b + 3 <= 199 constants after c32

uint4 jobWord(RvkAddr table, uint i) { return rvkRawLoad<uint4, 16>(rvkAddrAdd(table, 16u + i * 16u)); }

float vfloat(RvkAddr v, uint byteOffset) { return asfloat(rvkRawLoad<uint, 4>(rvkAddrAdd(v, byteOffset))); }

[numthreads(64, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID) {
    RvkAddr table = rvkAddr(pc.table);
    uint4 head = rvkRawLoad<uint4, 16>(table);
    uint jobs = head.x;
    uint g = gid.x;
    // the job: the last one whose first chunk is <= g
    uint lo = 0u, hi = jobs;
    while (hi - lo > 1u) {
        uint mid = (lo + hi) >> 1;
        if (jobWord(table, 8u * mid + 2u).x <= g) lo = mid; else hi = mid;
    }
    uint4 j0 = jobWord(table, 8u * lo), j1 = jobWord(table, 8u * lo + 1u), j2 = jobWord(table, 8u * lo + 2u), j3 = jobWord(table, 8u * lo + 3u);
    uint slot = (g - j2.x) * 64u + lid.x;
    uint slots = j1.z;
    if (slot >= slots) return;
    RvkAddr v = rvkAddrAdd(rvkAddr(j0.xy), slot * j1.w);
    uint oPos = j3.x & 0xFFu, oBw = (j3.x >> 8) & 0xFFu, oBi = (j3.x >> 16) & 0xFFu, oTc = (j3.x >> 24) & 0xFFu;
    float4 p = float4(vfloat(v, oPos), vfloat(v, oPos + 4u), vfloat(v, oPos + 8u), 1.0);
    if (j2.y != 0u) {
        // the morph targets (14.15 c), in the vertex shader's order
        uint4 j4 = jobWord(table, 8u * lo + 4u), j5 = jobWord(table, 8u * lo + 5u), j6 = jobWord(table, 8u * lo + 6u);
        [unroll] for (uint k = 0u; k < 3u; ++k) {
            if ((j2.y >> k & 1u) == 0u) continue;
            uint2 base = k == 0u ? j4.xy : k == 1u ? j4.zw : j5.xy;
            if (all(base == uint2(0u, 0u))) continue;
            RvkAddr m = rvkAddrAdd(rvkAddr(base), slot * ((j5.z >> (8u * k)) & 0xFFu));
            uint mo = (j5.w >> (8u * k)) & 0xFFu;
            float wk = asfloat(j6[k]);
            float3 d = float3(vfloat(m, mo), vfloat(m, mo + 4u), vfloat(m, mo + 8u));
            p.xyz = mad(d, float3(wk, wk, wk), p.xyz);
        }
    }
    float4 bw = float4(vfloat(v, oBw), vfloat(v, oBw + 4u), vfloat(v, oBw + 8u), vfloat(v, oBw + 12u));
    uint bi = rvkRawLoad<uint, 4>(rvkAddrAdd(v, oBi));
    RvkAddr bones = rvkAddr(j0.zw);
    // rows of the blended 3x4 matrix (the VS blends c33 / c34 / c35 [3 b])
    float4 r0 = float4(0.0, 0.0, 0.0, 0.0), r1 = float4(0.0, 0.0, 0.0, 0.0), r2 = float4(0.0, 0.0, 0.0, 0.0);
    for (uint k = 0u; k < 4u; ++k) {
        uint b = (bi >> (8u * k)) & 0xFFu;
        float w = bw[k];
        if (b >= BONE_COUNT || w == 0.0) continue;
        uint c = 1u + 3u * b;  // c33 + 3 b, relative to c32
        r0 += rvkRawLoad<float4, 16>(rvkAddrAdd(bones, c * 16u)) * w;
        r1 += rvkRawLoad<float4, 16>(rvkAddrAdd(bones, (c + 1u) * 16u)) * w;
        r2 += rvkRawLoad<float4, 16>(rvkAddrAdd(bones, (c + 2u) * 16u)) * w;
    }
    float3 world = float3(dot(p, r0), dot(p, r1), dot(p, r2));
    RvkAddr o = rvkAddr(j1.xy);  // the capture: a 16-byte header, then float4 v[]
    float4 tc = float4(vfloat(v, oTc), vfloat(v, oTc + 4u), 0.0, 1.0);
    uint2 refAddr = j2.zw;
    if (any(refAddr != uint2(0u, 0u)) && any(head.zw != uint2(0u, 0u))) {
        // verification (debug.rt_skin_verify): against the vertex shader's own result of this draw, where it wrote this slot
        RvkAddr r = rvkAddr(refAddr);
        float4 rp = rvkRwLoad<float4, 16>(rvkAddrAdd(r, 16u + slot * 32u));  // (the capture buffers are written by the vertex shaders: writable views)
        RvkAddr e = rvkAddr(head.zw);  // SkinErr: maxBits +0, compared +4, missing +8
        if (rp.w == 1.0) {
            float err = length(rp.xyz - world);
            rvkRwAtomicMax(e, asuint(err));
            rvkRwAtomicAdd(rvkAddrAdd(e, 4u), 1u);
        } else {
            rvkRwAtomicAdd(rvkAddrAdd(e, 8u), 1u);
        }
    }
    rvkRwStore<float4, 16>(rvkAddrAdd(o, 16u + slot * 32u), float4(world, 1.0));
    rvkRwStore<float4, 16>(rvkAddrAdd(o, 16u + slot * 32u + 16u), tc);
}
