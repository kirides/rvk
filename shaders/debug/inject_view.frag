#version 460
// Debug injection view, composite: the transient factor image over the scene HDR target (blend: dst * src).

#include "inject_view.glsl"

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(rvkSample2DLod(pc.srcTex, pc.srcSampler, inUv, 0.0).rgb, 1.0);
}
