// FFT ocean: one pass of the 2D inverse FFT (rows: pc.time.w == 0, columns: 1) over both work images (4 complex fields) of
// one cascade, in place. One workgroup per line, N / 2 threads (one butterfly each per stage), the line in shared memory:
// load in bit-reversed order, log2(N) radix-2 stages (Cooley-Tukey, decimation in time, + sign for the inverse). The same
// algorithm as enh::ocean::ifft1D (unit-tested against a direct DFT). Shared memory: 2 x N x 16 bytes (16 KiB at 512).
//
// One module per size: compiled with OCEAN_N (128 / 256 / 512) and OCEAN_LOG2N defined (shader::embedded::spirvVariant); HLSL has no
// specialisation constants for numthreads / groupshared sizes.
#include "ocean_compute.hlsli"

#ifndef OCEAN_N
#define OCEAN_N 256
#define OCEAN_LOG2N 8
#endif

OCEAN_WORK_A(workA);
OCEAN_WORK_B(workB);

groupshared float4 sa[OCEAN_N];
groupshared float4 sb[OCEAN_N];

int2 texel(int ln, int i) { return pc.time.w < 0.5 ? int2(i, ln) : int2(ln, i); }

[numthreads(OCEAN_N / 2, 1, 1)]
void CSMain(uint3 groupId : SV_GroupID, uint3 localId : SV_GroupThreadID) {
    int ln = int(groupId.x);
    int t = int(localId.x);
    for (int j = 0; j < 2; ++j) {
        int i = t + j * (OCEAN_N / 2);
        int r = int(reversebits(uint(i)) >> uint(32 - OCEAN_LOG2N));
        int2 p = texel(ln, i);
        sa[r] = workA[p];
        sb[r] = workB[p];
    }
    GroupMemoryBarrierWithGroupSync();
    for (int half_ = 1; half_ < OCEAN_N; half_ *= 2) {
        int group = t / half_;
        int pos = t - group * half_;
        int i0 = group * 2 * half_ + pos;
        int i1 = i0 + half_;
        float ang = OCEAN_PI * float(pos) / float(half_);
        float2 w = float2(cos(ang), sin(ang));
        float4 a = sa[i0], b = sa[i1];
        float4 bw = float4(cmul(b.xy, w), cmul(b.zw, w));
        sa[i0] = a + bw;
        sa[i1] = a - bw;
        a = sb[i0];
        b = sb[i1];
        bw = float4(cmul(b.xy, w), cmul(b.zw, w));
        sb[i0] = a + bw;
        sb[i1] = a - bw;
        GroupMemoryBarrierWithGroupSync();
    }
    for (int j2 = 0; j2 < 2; ++j2) {
        int i = t + j2 * (OCEAN_N / 2);
        int2 p = texel(ln, i);
        workA[p] = sa[i];
        workB[p] = sb[i];
    }
}
