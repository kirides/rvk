// Debug injection view, composite: the transient factor image over the scene HDR target (blend: dst * src).

#include "inject_view.hlsli"

float4 PSMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    return float4(rvkSample2DLod(pc.srcTex, pc.srcSampler, uv, 0.0).rgb, 1.0);
}
