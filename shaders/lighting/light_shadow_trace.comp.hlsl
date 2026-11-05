// Ray-traced point-light shadows, pass 1 (src/enh/light_table.cpp, docs/research/gpu-scene.md 9.10): per pixel of the engine's depth map, the shadow-casting
// lights (table lights with a shadow id, misc.y) of the pixel's cluster are scored like the material variants do (luminance x falloff x (0.25 + 0.75 N.L)),
// the best RVK_LS_ENTRIES get shadow rays against the frame's TLAS towards a random point of a small disc around the light (a torch flame, a candle: the
// penumbra grows with the occluder's distance), cut-outs tested. The raw visibilities go to an RGBA16F image (entries: light shadow id + q / 64).
// Pixels whose cluster holds no shadow-casting light cost nothing.

#include "light_shadow_common.hlsli"
#include "rvk_rt_trace.hlsli"

[[vk::image_format("rgba16f")]] RWTexture2D<float4> outRaw : register(u0, space1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    if (q.x >= int(pc.tex.z) || q.y >= int(pc.tex.w)) return;
    float4 result = float4(0.0, 0.0, 0.0, 0.0);
    RvkFrame f = rvkLoadFrame(pc.frame);
    float d = lsDepth(q);
    RvkAddr base = rvkAddr(pc.table);
    uint4 h0 = rvkRawLoad<uint4, 16>(base);
    if (d < LS_SKY && rvkRtValid(f) && h0.x == f.frameInfo.x + 1u) {
        uint4 offs = rvkRawLoad<uint4, 16>(rvkAddrAdd(base, 48u));
        float vz = d * f.cameraPos.w;
        float3 P = lsPos(f, q, d);
        float3 N = lsNormal(f, q, d, P);
        float2 uv = (float2(q) + 0.5) / float2(pc.tex.zw);
        RvkAddr cb = rvkLightClusterAt(base, vz, uv);
        uint cnt = min(rvkRawLoad<uint, 4>(cb), RVK_LIGHT_MAX_PER_CLUSTER);
        RvkAddr lb = rvkAddrAdd(base, offs.x);
        float score[RVK_LS_ENTRIES];
        uint pick[RVK_LS_ENTRIES];
        [unroll] for (uint i = 0; i < RVK_LS_ENTRIES; ++i) {
            score[i] = 0.0;
            pick[i] = 0xFFFFFFFFu;
        }
        for (uint j = 0; j < cnt; ++j) {
            uint idx = rvkRawLoad<uint, 4>(rvkAddrAdd(cb, 4u + 4u * j));
            uint sid = rvkRawLoad<uint, 4>(rvkAddrAdd(lb, idx * RVK_LIGHT_BYTES + 52u));  // misc.y
            if (sid == 0u) continue;
            float4 pr = rvkRawLoad<float4, 16>(rvkAddrAdd(lb, idx * RVK_LIGHT_BYTES));
            float3 lv = pr.xyz - P;
            float fall = 1.0 - saturate(dot(lv, lv) * pr.w);
            if (fall <= 0.0) continue;
            float4 cr = rvkRawLoad<float4, 16>(rvkAddrAdd(lb, idx * RVK_LIGHT_BYTES + 16u));
            float sc = rvkLmLum(cr.rgb) * fall * (0.25 + 0.75 * saturate(dot(N, normalize(lv))));
            [unroll] for (uint i = 0; i < RVK_LS_ENTRIES; ++i) {
                if (sc > score[i]) {
                    float ts = score[i];
                    uint ti = pick[i];
                    score[i] = sc;
                    pick[i] = idx;
                    sc = ts;
                    idx = ti;
                    if (idx == 0xFFFFFFFFu) sc = 0.0;
                }
            }
        }
        const uint rays = max(uint(pc.cfg.w), 1u);
        float3 o = P + N * (pc.cfg.z + vz * 0.0006);
        [unroll] for (uint e = 0; e < RVK_LS_ENTRIES; ++e) {
            if (pick[e] == 0xFFFFFFFFu) continue;
            float4 pr = rvkRawLoad<float4, 16>(rvkAddrAdd(lb, pick[e] * RVK_LIGHT_BYTES));
            uint sid = rvkRawLoad<uint, 4>(rvkAddrAdd(lb, pick[e] * RVK_LIGHT_BYTES + 52u));
            float3 toL = pr.xyz - o;
            float dist = length(toL);
            float vis = 1.0;
            // within a flame's reach of the light the surface is the lamp's own holder: no shadow
            if (dist > 40.0) {
                float3 L = toL / dist;
                float3 T = normalize(cross(L, abs(L.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0)));
                float3 B = cross(L, T);
                float sum = 0.0;
                for (uint r = 0u; r < rays; ++r) {
                    uint fr = pc.misc.x * rays + r + e * 7u;
                    float u1 = lsIgn(float2(q), fr);
                    float u2 = lsIgn(float2(q) + float2(37.0, 17.0), fr);
                    float rad = sqrt(u1) * pc.cfg.x;
                    float phi = 6.2831853 * u2;
                    float3 target = pr.xyz + (T * cos(phi) + B * sin(phi)) * rad;
                    float3 toT = target - o;
                    float dt = length(toT);
                    float3 dir = toT / max(dt, 1e-3);
                    // a ray below the surface's tangent plane is no light path (the material's N.L darkens the terminator)
                    if (dot(dir, N) < 0.02) {
                        sum += 1.0;
                        continue;
                    }
                    // stop short of the light: the flame sits inside its own holder / brazier
                    float tmax = max(dt - 18.0, 1.0);
                    sum += rvkRtVisibility(f, o, dir, 0.25, min(tmax, pc.cfg.y), 0.0, 0.0, pc.misc.y);
                }
                vis = sum / float(rays);
            }
            result[e] = lsEncode(sid, vis);
        }
    }
    outRaw[q] = result;
}
