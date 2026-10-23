#version 460
// Pass-through vertex shader of rvk_shadereq: the VS constants c[loc] are the varyings at location loc (0..7, 16..17), one
// full-screen triangle. Constant across the triangle, so every pixel of both compared fragment shaders sees the same inputs.
#extension GL_GOOGLE_include_directive : require
#include "rvk_abi.glsl"

layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec4 o3;
layout(location = 4) out vec4 o4;
layout(location = 5) out vec4 o5;
layout(location = 6) out vec4 o6;
layout(location = 7) out vec4 o7;
layout(location = 16) out vec4 o16;
layout(location = 17) out vec4 o17;

void main() {
    o0 = pc.vs.c[0];
    o1 = pc.vs.c[1];
    o2 = pc.vs.c[2];
    o3 = pc.vs.c[3];
    o4 = pc.vs.c[4];
    o5 = pc.vs.c[5];
    o6 = pc.vs.c[6];
    o7 = pc.vs.c[7];
    o16 = pc.vs.c[16];
    o17 = pc.vs.c[17];
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
