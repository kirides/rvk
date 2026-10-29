// Debug injection view, compute part: writes a multiplicative factor per pixel of the scene HDR target into a transient
// RGBA16F image. Depth mode reconstructs each pixel's world position from the engine's linear depth map and the FrameBlock
// camera (bands by distance, lines on a world-space grid: they must stay fixed in the world while the camera moves);
// tint mode cycles a colour with the FrameBlock time.

#include "inject_view.hlsli"

[[vk::image_format("rgba16f")]] RWTexture2D<float4> outFactor : register(u0, space1);

float3 ramp(float t) {
    // blue (near) -> green -> red (far)
    return saturate(float3(2.0 * t - 0.5, 1.0 - abs(2.0 * t - 1.0) * 1.2, 1.5 - 2.0 * t));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy;
    if (p.x >= pc.width || p.y >= pc.height) return;
    RvkFrame f = rvkLoadFrame(pc.frame);
    float4 params = f.params[DEBUG_VIEW_PARAM];
    uint mode = uint(params.x);
    float strength = params.y;
    float spacing = max(params.z, 1.0);
    float3 factor = float3(1.0, 1.0, 1.0);
    if (mode == DEBUG_VIEW_TINT) {
        float3 c = 0.5 + 0.5 * cos(6.2831853 * (f.time.x * 0.25 + float3(0.0, 0.33, 0.67)));
        factor = lerp(float3(1.0, 1.0, 1.0), c, strength);
    } else if (mode == DEBUG_VIEW_DEPTH && pc.depthTex != 0u && (f.frameInfo.y & RVK_FRAME_CAMERA_VALID) != 0u) {
        int2 dsize = rvkSize2D(pc.depthTex);
        float2 uv = (float2(p) + 0.5) / float2(pc.width, pc.height);
        float d01 = rvkFetch2D(pc.depthTex, int2(uv * float2(dsize))).r;
        if (d01 < 0.9999) {  // 1 = cleared (sky)
            float viewDepth = rvkViewDepth(f, d01);
            float3 world = rvkWorldFromViewDepth(f, uv, viewDepth);
            float t = clamp(log2(max(viewDepth, 1.0)) / 16.0, 0.0, 1.0);
            factor = lerp(float3(1.0, 1.0, 1.0), ramp(t) * 1.5, strength);
            // world grid on the horizontal plane (Risen: y up); the line width grows with the distance (pixel footprint)
            float2 g = abs(frac(world.xz / spacing + 0.5) - 0.5) * spacing;
            float w = 2.0 + viewDepth * 0.002;
            if (min(g.x, g.y) < w) factor *= 0.2;
        }
    }
    outFactor[p] = float4(factor, 1.0);
}
