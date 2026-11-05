// The reflection image's filter (enh/material_rt.*, docs/research/material-rt.md): the trace pass's image (a quarter or all of the screen's pixels, one GGX
// lobe sample each) is gathered to every screen pixel with weights of distance, depth, normal and roughness (an edge stops the blur: a reflection never
// bleeds over the silhouette of a surface without one), then blended with the previous frame's result reprojected by the G-buffer's motion vectors,
// the history clamped to the gathered neighbourhood's range (a moving reflection leaves no trail). Output: the screen-sized reflection image the
// composite draw adds to the scene.

#include "material_rt_common.hlsli"

[[vk::image_format("rgba16f")]] RWTexture2D<float4> outImage : register(u0, space1);

struct MaterialRtPush {
    RVK_PASS_PUSH_HEADER
    uint2 params;
    uint2 pad;
};
RVK_PUSH(MaterialRtPush, pc);

struct Tap {
    float4 value;
    float weight;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 gid : SV_DispatchThreadID) {
    MaterialRtParams p = rvkPtr<MaterialRtParams, 16>(pc.params).Get();
    RvkFrame f = rvkLoadFrame(pc.frame);
    uint2 id = gid.xy;
    uint2 size = uint2(f.viewport.xy);
    if (id.x >= size.x || id.y >= size.y) return;
    float2 uv = (float2(id) + 0.5) * f.viewport.zw;
    float far = f.cameraPos.w;
    float4 gb = rvkSample2DLod(p.tex.y, p.tex2.x, uv, 0.0);
    float3 mat = rvkGbufferMaterialDecode(gb.z);
    float z0 = rvkSample2DLod(p.tex.x, p.tex2.x, uv, 0.0).r * far;
    float3 N0 = rvkGiOctDecode(gb.xy);
    // the pixels the trace pass works on: pack entries, and flat ground while it rains (puddles)
    bool rainy = p.strength.z > 0.02;
    if (gb.w <= 0.0 || z0 >= far * 0.995 || !(mat.z > 0.5 || (rainy && N0.y > 0.78))) {
        outImage[int2(id)] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    uint scale = p.info.z;
    int2 traceSize = int2(p.info.xy);
    // the trace pixels around this pixel: the 2x2 that bracket it (a quarter-rate image), or the 3x3 around it (a full-rate one)
    float2 tf = (float2(id) + 0.5) / float(scale) - 0.5;
    int2 t0 = int2(floor(tf));
    float2 fr = tf - float2(t0);
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float wsum = 0.0;
    float3 lo = float3(1e9, 1e9, 1e9), hi = float3(-1e9, -1e9, -1e9);
    const int first = scale > 1u ? 0 : -1;
    const int last = scale > 1u ? 1 : 1;
    if (scale == 1u) { t0 = int2(id); fr = float2(0.0, 0.0); }
    for (int dy = first; dy <= last; ++dy) {
        for (int dx = first; dx <= last; ++dx) {
            int2 tp = clamp(t0 + int2(dx, dy), int2(0, 0), traceSize - 1);
            float2 d = float2(float(dx), float(dy)) - fr;
            float w = exp(-dot(d, d) * (scale > 1u ? 0.5 : 0.7));
            float2 tuv = (float2(tp * int(scale)) + 0.5 * float(scale)) * f.viewport.zw;
            float4 g = rvkSample2DLod(p.tex.y, p.tex2.x, tuv, 0.0);
            float3 m = rvkGbufferMaterialDecode(g.z);
            float zt = rvkSample2DLod(p.tex.x, p.tex2.x, tuv, 0.0).r * far;
            float3 Nt = rvkGiOctDecode(g.xy);
            w *= (g.w > 0.0 && (m.z > 0.5 || (rainy && Nt.y > 0.78))) ? 1.0 : 0.0;
            w *= exp(-abs(zt - z0) / max(0.03 * z0, 8.0));
            w *= pow(saturate(dot(N0, Nt)), 8.0);
            if (m.z > 0.5 && mat.z > 0.5) w *= exp(-abs(m.x - mat.x) * 6.0);
            if (w <= 1e-4) continue;
            float4 v = rvkFetch2D(p.tex2.w, tp);
            sum += v * w;
            wsum += w;
            lo = min(lo, v.rgb);
            hi = max(hi, v.rgb);
        }
    }
    float4 cur = wsum > 1e-4 ? sum / wsum : float4(0.0, 0.0, 0.0, 1.0);
    float4 result = cur;
    if (p.tex2.z != 0u && wsum > 1e-4) {
        float2 mv = rvkSample2DLod(p.tex2.y, p.tex2.x, uv, 0.0).rg;
        float2 puv = uv + mv;
        if (all(puv > float2(0.0, 0.0)) && all(puv < float2(1.0, 1.0))) {
            float4 hist = rvkSample2DLod(p.tex2.z, p.tex.w, puv, 0.0);
            // (the neighbourhood's range, a little widened: noise of the lobe samples stays inside it)
            float3 mid = 0.5 * (lo + hi), ext = 0.5 * (hi - lo) * 1.3 + 0.003;
            hist.rgb = clamp(hist.rgb, mid - ext, mid + ext);
            // a mirror shows a reflection that does not move with the surface: trust the history less
            float keep = lerp(0.6, 0.88, saturate(mat.x * 2.5));
            result = lerp(cur, hist, keep);
        }
    }
    if (any(isnan(result)) || any(isinf(result))) result = float4(0.0, 0.0, 0.0, 1.0);
    outImage[int2(id)] = result;
}
