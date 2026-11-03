// GI probe rays (M10 stage A, src/enh/gi.cpp, docs/research/rtgi.md 5, 9): one workgroup per probe of this frame's update
// list, one ray per invocation (64 spherical-Fibonacci directions, randomly rotated each frame) against the frame's TLAS,
// shaded by gi_shade.hlsli (engine-style lighting at hits, the sky on misses, back faces marked). gi_blend.comp then folds the
// rays into the probe. HLSL port of gi_trace.comp.

#include "rvk_common.hlsli"
#include "rvk_rt_trace.hlsli"
#include "rvk_gi.hlsli"
#include "gi_common.hlsli"
#include "gi_shade.hlsli"

groupshared uint sVis[4];

[numthreads(64, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID) {
    GiPassParams p = giParams();
    RvkFrame f = rvkLoadFrame(pc.frame);
    uint item = gid.x;
    if (item >= p.Get().volume.z) return;
    uint ray = lid.x;
    RvkGiVolume v = rvkPtr<RvkGiVolumeData, 16>(p.Get().volume.xy);
    uint e = giListEntry(p, item);
    uint c = e >> 24;
    uint slot = e & 0xFFFFFFu;
    int3 dims = int3(v.Get().dims.xyz);
    int3 cell = rvkGiCellOfSlot(dims, v.Get().cascade[c].origin.xyz, slot);
    float3 P = float3(cell) * v.Get().cascade[c].spacing.x;
    // the point-light pick of the hit (giPointLights): stratified over the probe's rays (a golden-ratio sequence from a random
    // per-probe, per-frame start) instead of independent draws: the rays together sample the lights evenly (rtgi.md 20)
    float u = frac(float(ray) * 0.6180339887 + float(giHash(item * 2654435761u + p.Get().volume.w * 7919u) & 0xFFFFFFu) / 16777216.0);
    // the lights this probe sees (their direct light enters the probe in gi_blend.comp: rtgi.md 19): light j by invocation
    // j % 64, one visibility ray each
    if ((p.Get().flags.z & 1u) != 0u) {
        if (ray < 4u) sVis[ray] = 0u;
        GroupMemoryBarrierWithGroupSync();
        uint nL = min(p.Get().flags.y, GI_MAX_LIGHTS);
        for (uint j = ray; j < nL; j += GI_RAYS) {
            float4 lp = giLight(j * 2u);
            float3 d = lp.xyz - P;
            float d2 = dot(d, d);
            if (d2 * lp.w >= 1.0 || dot(giLight(j * 2u + 1u).rgb, float3(1.0, 1.0, 1.0)) <= 1e-4) continue;
            float dist = sqrt(max(d2, 1e-4));
            if (rvkRtVisibility(f, P, d / dist, 1.0, max(dist - 8.0, 1.0), 0.0, 0.0, RVK_RT_TRACE_OPAQUE) > 0.5) {
                uint orig;
                InterlockedOr(sVis[j >> 5u], 1u << (j & 31u), orig);
            }
        }
        GroupMemoryBarrierWithGroupSync();
        if (ray == 0u) {
            giRayStore(p, GI_MAX_PROBE_ITEMS * GI_RAYS + item, asfloat(uint4(sVis[0], sVis[1], sVis[2], sVis[3])));
        }
    }
    giRayStore(p, item * GI_RAYS + ray, giShadeRay(p, f, v, P, giRayDir(p, ray), p.Get().reach[c], u));
}
