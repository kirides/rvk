#version 460
// Native opaque pass, colour vertex stage (static_mesh.glsl): index (from the bound arena) -> vertex pulled by address -> the engine's
// clip space (nativeClip, identical to the depth pre-pass's: the colour pass tests depth EQUAL).

#extension GL_EXT_buffer_reference_uvec2 : require
#include "static_mesh.glsl"

invariant gl_Position;

layout(location = 0) out vec3 outWorld;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec3 outTangent;
layout(location = 3) out vec4 outColor0;
layout(location = 4) out vec4 outColor1;
layout(location = 5) out vec4 outUv;      // xy diffuse, zw normal map
layout(location = 6) out vec2 outLightmap;
layout(location = 7) out vec4 outViewPos;  // clip position before the half-pixel fix (the engine's vViewPos)
layout(location = 8) flat out uint outItem;

void main() {
    const uint itemIndex = uint(gl_InstanceIndex);
    NativeItem it = nativeItem(itemIndex);
    const uvec2 v = addrAdd(it.addr.xy, uint(gl_VertexIndex) * it.tex2.z);
    vec3 world;
    gl_Position = nativeClip(it, v, world, outViewPos);
    outWorld = world;
    outItem = itemIndex;

    // normal / tangent into world space (the rows' 3x3, as the engine's vertex shader)
    vec3 n = it.off0.x != 0xFFFFu ? loadV3(v, it.off0.x) : vec3(0.0, 1.0, 0.0);
    vec3 t = it.off0.y != 0xFFFFu ? loadV3(v, it.off0.y) : vec3(1.0, 0.0, 0.0);
    outNormal = normalize(vec3(dot(it.row0.xyz, n), dot(it.row1.xyz, n), dot(it.row2.xyz, n)));
    outTangent = normalize(vec3(dot(it.row0.xyz, t), dot(it.row1.xyz, t), dot(it.row2.xyz, t)));
    outColor0 = it.off0.z != 0xFFFFu ? loadColor(v, it.off0.z) : vec4(1.0);
    outColor1 = it.off0.w != 0xFFFFu ? loadColor(v, it.off0.w) : vec4(1.0);
    outUv = vec4(it.off1.x != 0xFFFFu ? loadV2(v, it.off1.x) : vec2(0.0), it.off1.y != 0xFFFFu ? loadV2(v, it.off1.y) : vec2(0.0));
    outLightmap = it.off1.z != 0xFFFFu ? loadV2(v, it.off1.z) : vec2(0.0);
}
