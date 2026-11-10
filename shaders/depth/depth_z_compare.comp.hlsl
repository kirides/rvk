// S-6 compare mode: the derivation of depth_z.hlsli against the depth map the engine's z pass wrote, per pixel; counts the pixels beyond a few relative tolerances.

#include "depth_z.hlsli"

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy;
    if (p.x >= pc.width || p.y >= pc.height) return;
    float a = rvkFetch2D(pc.mapTex, int2(p)).r;
    float b = rvkDepthZMap(rvkLoadFrame(pc.frame), rvkFetch2D(pc.depthTex, int2(p)).r);
    if (a < 0.9999) rvkBufAdd(pc.stats, DZ_GEOMETRY * 4u, 1u);
    if ((a >= 0.9999) != (b >= 0.9999)) rvkBufAdd(pc.stats, DZ_SKY_DIFF * 4u, 1u);
    float rel = (isnan(a) || isnan(b)) ? 1.0e6 : abs(a - b) / max(min(a, b), 1.0e-6);
    if (rel > 1.0e-6) rvkBufAdd(pc.stats, DZ_OVER_1E6 * 4u, 1u);
    if (rel > 1.0e-5) rvkBufAdd(pc.stats, DZ_OVER_1E5 * 4u, 1u);
    if (rel > 1.0e-4) rvkBufAdd(pc.stats, DZ_OVER_1E4 * 4u, 1u);
    if (rel > 1.0e-3) rvkBufAdd(pc.stats, DZ_OVER_1E3 * 4u, 1u);
    if (rel > 4.0e-3) rvkBufAdd(pc.stats, DZ_OVER_4E3 * 4u, 1u);
    if (rel > 0.0) {
        rvkRwAtomicMax(rvkAddrAdd(rvkAddr(pc.stats), DZ_MAX_REL * 4u), asuint(rel));
        if (a < 0.02) rvkRwAtomicMax(rvkAddrAdd(rvkAddr(pc.stats), DZ_NEAR_MAX * 4u), asuint(rel));
    }
}
