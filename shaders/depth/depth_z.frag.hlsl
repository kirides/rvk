// S-6: the linear depth map derived from the hardware depth (one full-screen triangle into the engine's R32F depth map). See depth_z.hlsli.

#include "depth_z.hlsli"

float PSMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    return rvkDepthZMap(rvkLoadFrame(pc.frame), rvkFetch2D(pc.depthTex, int2(position.xy)).r);
}
