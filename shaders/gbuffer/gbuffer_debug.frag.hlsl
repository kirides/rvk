// G-buffer debug view (gbuffer.debug, src/enh/gbuffer.cpp) over the frame at BeforeGui: 1 normals (world, 0.5 + 0.5 n;
// characters tinted red, no material black), 2 motion (hue = direction, brightness = length: 1 % of the screen = full),
// 3 ambient occlusion (the filtered history the material shaders read next frame).

#include "gbuffer_common.hlsli"

float4 PSMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    if (pc.mode == 1u) {
        float4 n = rvkSample2DLod(pc.normalTex, pc.linearSampler, uv, 0.0);
        float id = round(n.w * 3.0);
        float3 c = id == 0.0 ? float3(0.0, 0.0, 0.0) : rvkGiOctDecode(n.xy) * 0.5 + 0.5;
        if (id == 2.0) c = lerp(c, float3(1.0, 0.2, 0.2), 0.5);
        return float4(c, 1.0);
    } else if (pc.mode == 2u) {
        float2 m = rvkSample2DLod(pc.motionTex, pc.linearSampler, uv, 0.0).xy;
        float len = clamp(length(m) * 100.0, 0.0, 1.0);
        float a = atan2(m.y, m.x) / (2.0 * GB_PI) + 0.5;
        float3 hue = clamp(abs(frac(a + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
        return float4(hue * len + float3(0.05, 0.05, 0.05), 1.0);
    } else if (pc.mode == 4u) {
        // the screen probes' factor on the DDGI: 0.5 = 1 (no change), darker = less light than the probe volume says
        float4 s = pc.aoTex != 0u ? rvkSample2DLod(pc.aoTex, pc.linearSampler, uv, 0.0) : float4(1.0, 1.0, 1.0, 0.0);
        return float4(s.a > 0.5 ? clamp(s.rgb * 0.5, 0.0, 1.0) : float3(0.3, 0.0, 0.3), 1.0);
    } else {
        float ao = pc.aoTex != 0u ? rvkSample2DLod(pc.aoTex, pc.linearSampler, uv, 0.0).r : 1.0;
        return float4(ao, ao, ao, 1.0);
    }
}
