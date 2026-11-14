// Soft sun shadows, stage 3: the spatial filter (src/enh/shadows.cpp, docs/research/shadows.md 8.10). A depth-plane bilateral blur of the
// accumulated visibility where the pixel is still uncertain (temporal variance = the penumbra, short history = new on screen) AND
// the penumbra is wide (the raw pass's filter radius): hard contact edges are never filtered. The window follows the penumbra.

#include "rt_shadow_common.hlsli"

// only .r is written / read: R8 where the GPU has the extended storage formats (SOFT_R8), else RGBA8
#ifdef SOFT_R8
[[vk::image_format("r8")]] RWTexture2D<float4> outFinal : register(u0, space1);
#else
[[vk::image_format("rgba8")]] RWTexture2D<float4> outFinal : register(u0, space1);
#endif

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    RtShadowParams p = rtsParams();
    if (q.x >= int(p.info.x) || q.y >= int(p.info.y)) return;
    RvkFrame f = rvkLoadFrame(pc.frame);
    float d = rtsDepthMap(p, q);
    float vz = d * f.cameraPos.w;
    float4 h = rvkFetch2D(p.tex.w, q);
    if (d >= RTS_SKY) {
        outFinal[q] = float4(1.0, 0.0, 0.0, 1.0);
        return;
    }
    float variance = max(h.w - h.x * h.x, 0.0);
    float need = max(smoothstep(0.004, 0.04, variance), 1.0 - clamp((h.z - 1.0) / 5.0, 0.0, 1.0));
    float result = h.x;
    if (need > 0.01) {
        float pen = 0.0;
        for (int j = -1; j <= 1; ++j) {
            for (int i = -1; i <= 1; ++i) pen = max(pen, rvkFetch2D(p.tex.y, rtsClamp(p, q + int2(i, j))).g);
        }
        pen *= 16.0;
        need *= smoothstep(1.0, 3.0, pen) * p.range.w;
        if (need > 0.01) {
            float3 P = rtsPos(f, p, q, d);
            float3 N = rtsNormal(f, p, q, d, P);
            int r = int(clamp(floor(pen * 0.5 + 0.5), 1.0, 3.0));
            float tol = 0.012 * vz + 2.0;
            float wsum = 0.0, vsum = 0.0;
            for (int j = -r; j <= r; ++j) {
                for (int i = -r; i <= r; ++i) {
                    int2 t = q + int2(i, j);
                    if (any(t < int2(0, 0)) || any(t >= int2(p.info.xy))) continue;
                    float dt = rtsDepthMap(p, t);
                    if (dt >= RTS_SKY) continue;
                    float4 ht = rvkFetch2D(p.tex.w, t);
                    float3 Q = rtsPos(f, p, t, dt);
                    float plane = abs(dot(N, Q - P));
                    float w = max(1.0 - plane / tol, 0.0) * exp(-0.5 * float(i * i + j * j) / float(r * r));
                    wsum += w;
                    vsum += w * ht.x;
                }
            }
            if (wsum > 1e-4) result = lerp(h.x, vsum / wsum, need);
        }
    }
    outFinal[q] = float4(clamp(result, 0.0, 1.0), 0.0, 0.0, 1.0);
}
