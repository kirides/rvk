// Ray-traced soft sun shadows, stage 3: the spatial filter (src/enh/rt_shadows.cpp, docs/research/rt-shadows.md). A depth-plane
// bilateral blur of the accumulated visibility, applied only where the pixel is still uncertain: its temporal variance (second
// moment - mean^2: the penumbra, where rays alternate between lit and blocked) and its short history (new on screen). Pixels
// that converged to fully lit / blocked keep their value, so hard contact edges stay hard. The wider window for new pixels.

#include "rt_shadow_common.hlsli"

[[vk::image_format("rgba8")]] RWTexture2D<float4> outFinal : register(u0, space1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    RtShadowParams p = rtsParams();
    if (q.x >= int(p.info.x) || q.y >= int(p.info.y)) return;
    RvkFrame f = rvkLoadFrame(pc.frame);
    float d = rtsDepthMap(p, q);
    float vz = d * f.cameraPos.w;
    float4 h = rvkFetch2D(p.tex.w, q);
    if (d >= RTS_SKY || vz >= p.range.x) {
        outFinal[q] = float4(1.0, 0.0, 0.0, 1.0);
        return;
    }
    float variance = max(h.w - h.x * h.x, 0.0);
    // 0 = keep, 1 = fully filtered
    float need = max(smoothstep(0.004, 0.04, variance), 1.0 - clamp((h.z - 1.0) / 5.0, 0.0, 1.0)) * p.range.w;
    float result = h.x;
    if (need > 0.01) {
        float3 P = rtsPos(f, p, q, d);
        float3 N = rtsNormal(f, p, q, d, P);
        int r = h.z < 4.0 ? 3 : 2;
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
    outFinal[q] = float4(clamp(result, 0.0, 1.0), 0.0, 0.0, 1.0);
}
