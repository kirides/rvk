#version 460
// Native depth pre-pass, fragment stage: the engine's linear depth map (view depth / engine far, ge_default_ps.fx) and, for the
// alpha-tested class (NATIVE_PREPASS_MASKED), the alpha test: the only place a masked part discards, so the colour pass (depth EQUAL)
// needs no clip. The opaque class has no discard and no texture fetch (early depth test stays on).

#extension GL_EXT_buffer_reference_uvec2 : require
#include "static_mesh.glsl"
#include "rvk_common.glsl"

layout(location = 0) in float inViewZ;
#ifdef NATIVE_PREPASS_MASKED
layout(location = 1) in vec2 inUv;
layout(location = 2) flat in uint inItem;
#endif
layout(location = 0) out vec4 outDepth;

#ifdef NATIVE_PREPASS_MASKED
// D3D9 alpha test as the engine's state (func 2 less ... 8 always), on a 0..255 reference
bool alphaPass(float a, uint func, float ref) {
    const float q = round(clamp(a, 0.0, 1.0) * 255.0);
    switch (func) {
    case 1u: return false;
    case 2u: return q < ref;
    case 3u: return q == ref;
    case 4u: return q <= ref;
    case 5u: return q > ref;
    case 6u: return q != ref;
    case 7u: return q >= ref;
    default: return true;
    }
}
#endif

void main() {
#ifdef NATIVE_PREPASS_MASKED
    NativeItem it = nativeItem(inItem);
    if ((it.tex2.y & NATIVE_NO_ALBEDO) == 0u && !alphaPass(rvkSample2D(it.tex0.x, it.tex0.y, inUv).a, (it.off1.w >> 8) & 255u, it.params.x)) discard;
#endif
    outDepth = vec4(inViewZ * pc.frame.depthParams.w, 0.0, 0.0, 0.0);
}
