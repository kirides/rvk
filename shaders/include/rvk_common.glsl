// Helpers for hand-written shaders (not part of the translated shaders): include after rvk_abi.glsl.

#ifndef RVK_COMMON_GLSL
#define RVK_COMMON_GLSL

#extension GL_EXT_samplerless_texture_functions : require

// bindless sampling
vec4 rvkSample2D(uint tex, uint smp, vec2 uv) { return texture(sampler2D(t2D[nonuniformEXT(tex)], samp[nonuniformEXT(smp)]), uv); }
vec4 rvkSample2DLod(uint tex, uint smp, vec2 uv, float lod) {
    return textureLod(sampler2D(t2D[nonuniformEXT(tex)], samp[nonuniformEXT(smp)]), uv, lod);
}
vec4 rvkFetch2D(uint tex, ivec2 p) { return texelFetch(t2D[nonuniformEXT(tex)], p, 0); }
ivec2 rvkSize2D(uint tex) { return textureSize(t2D[nonuniformEXT(tex)], 0); }

// Texture coordinates (0,0 = top left, D3D9 / Vulkan image order) -> D3D NDC (y up)
vec2 rvkUvToNdc(vec2 uv) { return vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0); }

// This frame's camera jitter as an NDC offset (src/enh/upscale: the main view's geometry is shifted by it in clip space, the
// FrameBlock's matrices stay the unjittered camera's; zero without jitter). The surface rasterised at a pixel whose centre is
// at NDC c has the unjittered NDC c - J: every reconstruction of a position from the main view's depth (depth map / hardware
// depth) takes `rvkUvToNdc(uv) - rvkJitterNdc(f)`.
vec2 rvkJitterNdc(RvkFrame f) { return vec2(2.0 * f.jitter.x, -2.0 * f.jitter.y) * f.viewport.zw; }

// The main view's hardware depth (docs/research/reverse-z.md). With RVK_FRAME_REVERSE_Z the buffer holds near / view z (1 at the
// near plane, 0 at infinity and for the sky); the engine's projection wrote D3D depth z = f / (f - n) (1 - n / viewZ). This
// returns the D3D depth (0 near .. 1 far, the sky and everything beyond the far plane 1), so code written for the engine's
// convention keeps working; without the flag it is the identity.
float rvkHwDepthToD3D(RvkFrame f, float d) {
    if ((f.frameInfo.y & RVK_FRAME_REVERSE_Z) == 0u) return d;
    float k = 1.0 - f.depthParams.x / max(f.depthParams.y, 1e-3);
    return min((1.0 - d) / max(k, 1e-6), 1.0);
}

// View depth (distance along the view axis) of a hardware depth value; 0 for the sky / infinity
float rvkHwDepthToViewZ(RvkFrame f, float d) {
    if ((f.frameInfo.y & RVK_FRAME_REVERSE_Z) != 0u) return d > 0.0 ? f.depthParams.x / d : 0.0;
    float a = f.proj[2][2], b = f.proj[3][2];
    return (d < 1.0 && abs(d - a) > 1e-9) ? max(b / (d - a), 0.0) : 0.0;
}

// The engine's linear depth map (R32F, ge_default_ps.fx: view depth / far, cleared to 1) -> view depth
float rvkViewDepth(RvkFrame f, float depthMapValue) { return depthMapValue * f.cameraPos.w; }

// World position of a pixel from its view depth: the point of the pixel's ray at that depth (the far-plane point has the
// projection's far as its view depth; the view depth grows linearly along a ray from the camera)
vec3 rvkWorldFromViewDepth(RvkFrame f, vec2 uv, float viewDepth) {
    vec4 farPoint = f.invViewProj * vec4(rvkUvToNdc(uv) - rvkJitterNdc(f), 1.0, 1.0);
    vec3 p = farPoint.xyz / farPoint.w;
    float projFar = max(f.depthParams.y, 1e-3);
    return f.cameraPos.xyz + (p - f.cameraPos.xyz) * (viewDepth / projFar);
}

#endif
