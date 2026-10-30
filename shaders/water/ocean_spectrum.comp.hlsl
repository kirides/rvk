// FFT ocean: the initial spectrum of one cascade (Tessendorf's h0(k) and conj(h0(-k))), re-run when the sea state changes
#include "ocean_compute.hlsli"

OCEAN_H0(h0Image);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    int2 p = int2(id.xy);
    int n = int(pc.tile.w);
    if (p.x >= n || p.y >= n) return;
    int2 m = (int2(n, n) - p) % n;  // the texel of -k
    h0Image[p] = float4(oceanH0(p), cconj(oceanH0(m)));
}
