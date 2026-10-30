// Frame generation debug view (framegen.debug, src/enh/framegen.cpp) over the finished frame at BeforePresent: 1 the motion vectors
// (uv, previous - current; hue = direction, brightness = length, 1 % of the screen = full), 2 the depth prepared for the generator
// (D3D convention 0 near .. 1 far: shown as a log distance ramp, blue near .. red far, sky black), 3 a colour image as it is (the
// HUD-less copy, the generated frame).

#include "rvk_common.hlsli"

struct FramegenPush {
    RVK_PASS_PUSH_HEADER
    uint srcTex;   // bindless t2D slot of the image shown
    uint sampler0; // linear clamp
    uint mode;
    uint pad;
    float nearZ;   // the camera's planes (world units) for the depth ramp
    float farZ;
    float pad1;
    float pad2;
};
RVK_PUSH(FramegenPush, pc);

static const float PI = 3.14159265;

float3 ramp(float t) {
    return clamp(float3(2.0 * t - 0.5, 1.0 - abs(2.0 * t - 1.0) * 1.2, 1.5 - 2.0 * t), 0.0, 1.0);
}

float4 PSMain(float4 position : SV_Position, float2 inUv : TEXCOORD0) : SV_Target0 {
    if (pc.mode == 1u) {
        float2 m = rvkSample2DLod(pc.srcTex, pc.sampler0, inUv, 0.0).xy;
        float len = clamp(length(m) * 100.0, 0.0, 1.0);
        float a = atan2(m.y, m.x) / (2.0 * PI) + 0.5;
        float3 hue = clamp(abs(frac(a + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
        return float4(hue * len + float3(0.05, 0.05, 0.05), 1.0);
    } else if (pc.mode == 2u) {
        float d = rvkSample2DLod(pc.srcTex, pc.sampler0, inUv, 0.0).x;
        if (d >= 0.99999) {
            return float4(0.0, 0.0, 0.0, 1.0);
        } else {
            // view distance of a D3D depth value: z = n f / (f - d (f - n))
            float dist = pc.nearZ * pc.farZ / max(pc.farZ - d * (pc.farZ - pc.nearZ), 1e-6);
            float t = clamp(log2(max(dist, 1.0)) / 16.0, 0.0, 1.0);
            return float4(ramp(t), 1.0);
        }
    } else {
        return float4(rvkSample2DLod(pc.srcTex, pc.sampler0, inUv, 0.0).rgb, 1.0);
    }
}
