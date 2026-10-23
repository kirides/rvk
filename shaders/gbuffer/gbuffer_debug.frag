#version 460
// G-buffer debug view (gbuffer.debug, src/enh/gbuffer.cpp) over the frame at BeforeGui: 1 normals (world, 0.5 + 0.5 n;
// characters tinted red, no material black), 2 motion (hue = direction, brightness = length: 1 % of the screen = full),
// 3 ambient occlusion (the filtered history the material shaders read next frame).

#include "gbuffer_common.glsl"

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = inUv;
    if (pc.mode == 1u) {
        vec4 n = rvkSample2DLod(pc.normalTex, pc.linearSampler, uv, 0.0);
        float id = round(n.w * 3.0);
        vec3 c = id == 0.0 ? vec3(0.0) : rvkGiOctDecode(n.xy) * 0.5 + 0.5;
        if (id == 2.0) c = mix(c, vec3(1.0, 0.2, 0.2), 0.5);
        outColor = vec4(c, 1.0);
    } else if (pc.mode == 2u) {
        vec2 m = rvkSample2DLod(pc.motionTex, pc.linearSampler, uv, 0.0).xy;
        float len = clamp(length(m) * 100.0, 0.0, 1.0);
        float a = atan(m.y, m.x) / (2.0 * GB_PI) + 0.5;
        vec3 hue = clamp(abs(fract(a + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
        outColor = vec4(hue * len + vec3(0.05), 1.0);
    } else {
        float ao = pc.aoTex != 0u ? rvkSample2DLod(pc.aoTex, pc.linearSampler, uv, 0.0).r : 1.0;
        outColor = vec4(vec3(ao), 1.0);
    }
}
