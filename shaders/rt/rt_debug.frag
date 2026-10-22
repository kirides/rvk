#version 460
// RT scene debug view, composite: the transient colour image over the back buffer (blend: src alpha over).

#include "rt_debug.glsl"

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = rvkSample2DLod(pc.srcTex, pc.srcSampler, inUv, 0.0);
}
