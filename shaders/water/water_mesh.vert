#version 460
// Enhanced water on the engine's own water mesh (lakes, rivers, swamps; the sea when water.ocean_waves is off): the draw's
// vertex buffer and VS constants, transformed like ge_default_vs.fx (world = g_vInstanceData[1..3] rows, clip =
// g_vViewProjection c0..c3). Inputs by D3D9 semantic (enh::Water's reflection): POSITION0, NORMAL0, TEXCOORD0, TEXCOORD4.

#include "water_common.glsl"

layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inNormal;
layout(location = 2) in vec4 inUv;
layout(location = 3) in vec4 inInstance;

layout(location = 0) out vec4 vWorld;   // xyz world position, w crest foam
layout(location = 1) out vec4 vNormal;  // xyz geometric world normal, w wave amplitude (0: no waves)
layout(location = 2) out vec4 vExtra;   // xy undisplaced xz, z distance to the coast, w unused

void main() {
    WaterBlock w = waterBlock();
    int base = 33;
    if ((w.tex2.z & WATER_INSTANCED) != 0u) base = 32 + int(inInstance.x + 0.5) * 4 + 1;
    vec4 p = vec4(inPos.xyz, 1.0);
    vec3 wp = vec3(dot(pc.vs.c[base], p), dot(pc.vs.c[base + 1], p), dot(pc.vs.c[base + 2], p));
    vec3 n = vec3(dot(pc.vs.c[base].xyz, inNormal.xyz), dot(pc.vs.c[base + 1].xyz, inNormal.xyz), dot(pc.vs.c[base + 2].xyz, inNormal.xyz));
    n = dot(n, n) > 1e-8 ? normalize(n) : vec3(0.0, 1.0, 0.0);
    vec4 wp4 = vec4(wp, 1.0);
    vec4 clip = vec4(dot(pc.vs.c[0], wp4), dot(pc.vs.c[1], wp4), dot(pc.vs.c[2], wp4), dot(pc.vs.c[3], wp4));
    vWorld = vec4(wp, 0.0);
    vNormal = vec4(n, 0.0);
    vExtra = vec4(wp.xz, 30000.0, 0.0);
    clip.xy += w.posFix.xy * clip.w;
    gl_Position = clip;
}
