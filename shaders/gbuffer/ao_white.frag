#version 460
// ao.mode = replace (src/enh/gbuffer.cpp): the game's SSAO draws (post_SSM.fx SSAO / BlurSSAO into the half-size mask) write
// "unoccluded", so the shadow mask's red channel is 1 and only the GTAO (on the ambient light) shades.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() { outColor = vec4(1.0); }
