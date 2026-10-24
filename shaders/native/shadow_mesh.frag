#version 460
// Native shadow cascade, fragment stage (shadow_mesh.glsl): the alpha test of masked parts (leaf cards, fences), nothing else.

#extension GL_EXT_buffer_reference_uvec2 : require
#include "shadow_mesh.glsl"
#include "rvk_common.glsl"

layout(location = 0) in vec2 inUv;

void main() {
    ShadowItem it = ShadowItem(pc.item);
    if ((it.tex.w & SHADOW_MASKED) != 0u && it.tex.x != 0u) {
        const float a = rvkSample2D(it.tex.x, it.tex.y, inUv).a;
        if (round(clamp(a, 0.0, 1.0) * 255.0) < it.params.x) discard;
    }
}
