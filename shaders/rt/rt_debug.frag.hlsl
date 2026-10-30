// RT scene debug view, composite: the transient colour image over the back buffer (blend: src alpha over).

#include "rt_debug.hlsli"

float4 PSMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    return rvkSample2DLod(pc.srcTex, pc.srcSampler, uv, 0.0);
}
