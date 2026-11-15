// SDSM depth reduction (docs/research/shadows.md 8.9): per view-depth bin (log spaced between zMin and zMax) the light-space x / y bounds of the visible
// (non-sky) pixels of the main view's depth. Level 1: a 16x16 thread group (each thread one sampled pixel) reduces into groupshared words with atomics;
// level 2: the group's non-empty words go to the result buffer with a global atomic max. Four words per bin: max of x, of -x, of y, of -y, each as an
// order-preserving uint (a float's bits, flipped); 0 = the bin is empty (the buffer is cleared to 0). The CPU merges the bins of a cascade's slice.

#include "sdsm.hlsli"

groupshared uint gsBins[SDSM_BINS * 4];

uint sdsmEnc(float v) {
    uint b = asuint(v);
    return (b & 0x80000000u) != 0u ? ~b : (b | 0x80000000u);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    for (uint i = gi; i < SDSM_BINS * 4u; i += 256u) gsBins[i] = 0u;
    GroupMemoryBarrierWithGroupSync();

    uint2 p = (gid.xy * 16u + gtid.xy) * pc.stride;
    if (p.x < pc.width && p.y < pc.height) {
        RvkFrame f = rvkLoadFrame(pc.frame);
        float z = rvkHwDepthToD3D(f, rvkFetch2D(pc.depthTex, int2(p)).r);
        if (z < 0.99999) {
            float a = f.proj[2][2], b = f.proj[3][2];
            float vz = abs(z - a) > 1e-9 ? b / (z - a) : 0.0;
            if (vz > 0.0 && vz <= pc.range.z) {
                float2 uv = (float2(p) + 0.5) / float2(pc.width, pc.height);
                float4 w = mul(float4(rvkUvToNdc(uv) - rvkJitterNdc(f), z, 1.0), f.invViewProj);
                float4 wp = float4(w.xyz / w.w, 1.0);
                float x = dot(wp, pc.lx), y = dot(wp, pc.ly);
                int bin = clamp(int(floor(log(max(vz, 1e-3) / pc.range.x) * pc.range.y)), 0, int(SDSM_BINS) - 1);
                uint o;
                InterlockedMax(gsBins[bin * 4 + 0], sdsmEnc(x), o);
                InterlockedMax(gsBins[bin * 4 + 1], sdsmEnc(-x), o);
                InterlockedMax(gsBins[bin * 4 + 2], sdsmEnc(y), o);
                InterlockedMax(gsBins[bin * 4 + 3], sdsmEnc(-y), o);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint j = gi; j < SDSM_BINS * 4u; j += 256u) {
        uint v = gsBins[j];
        if (v != 0u) rvkRwAtomicMax(rvkAddrAdd(rvkAddr(pc.bins), j * 4u), v);
    }
}
