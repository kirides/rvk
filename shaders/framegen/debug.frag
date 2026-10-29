#version 460
// Frame generation debug view (framegen.debug, src/enh/framegen.cpp) over the finished frame at BeforePresent: 1 the motion vectors
// (uv, previous - current; hue = direction, brightness = length, 1 % of the screen = full), 2 the depth prepared for the generator
// (D3D convention 0 near .. 1 far: shown as a log distance ramp, blue near .. red far, sky black), 3 a colour image as it is (the
// HUD-less copy, the generated frame).

#define RVK_PASS_SHADER 1
#include "rvk_abi.glsl"
#include "rvk_common.glsl"

layout(push_constant, std430) uniform FramegenPush {
    RVK_PASS_PUSH_HEADER
    uint srcTex;   // bindless t2D slot of the image shown
    uint sampler0; // linear clamp
    uint mode;
    uint pad;
    float nearZ;   // the camera's planes (world units) for the depth ramp
    float farZ;
    float pad1;
    float pad2;
} pc;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265;

vec3 ramp(float t) {
    return clamp(vec3(2.0 * t - 0.5, 1.0 - abs(2.0 * t - 1.0) * 1.2, 1.5 - 2.0 * t), 0.0, 1.0);
}

void main() {
    if (pc.mode == 1u) {
        vec2 m = rvkSample2DLod(pc.srcTex, pc.sampler0, inUv, 0.0).xy;
        float len = clamp(length(m) * 100.0, 0.0, 1.0);
        float a = atan(m.y, m.x) / (2.0 * PI) + 0.5;
        vec3 hue = clamp(abs(fract(a + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
        outColor = vec4(hue * len + vec3(0.05), 1.0);
    } else if (pc.mode == 2u) {
        float d = rvkSample2DLod(pc.srcTex, pc.sampler0, inUv, 0.0).x;
        if (d >= 0.99999) {
            outColor = vec4(0.0, 0.0, 0.0, 1.0);
        } else {
            // view distance of a D3D depth value: z = n f / (f - d (f - n))
            float dist = pc.nearZ * pc.farZ / max(pc.farZ - d * (pc.farZ - pc.nearZ), 1e-6);
            float t = clamp(log2(max(dist, 1.0)) / 16.0, 0.0, 1.0);
            outColor = vec4(ramp(t), 1.0);
        }
    } else {
        outColor = vec4(rvkSample2DLod(pc.srcTex, pc.sampler0, inUv, 0.0).rgb, 1.0);
    }
}
