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

// The engine's linear depth map (R32F, ge_default_ps.fx: view depth / far, cleared to 1) -> view depth
float rvkViewDepth(RvkFrame f, float depthMapValue) { return depthMapValue * f.cameraPos.w; }

// World position of a pixel from its view depth: the point of the pixel's ray at that depth (the far-plane point has the
// projection's far as its view depth; the view depth grows linearly along a ray from the camera)
vec3 rvkWorldFromViewDepth(RvkFrame f, vec2 uv, float viewDepth) {
    vec4 farPoint = f.invViewProj * vec4(rvkUvToNdc(uv), 1.0, 1.0);
    vec3 p = farPoint.xyz / farPoint.w;
    float projFar = max(f.depthParams.y, 1e-3);
    return f.cameraPos.xyz + (p - f.cameraPos.xyz) * (viewDepth / projFar);
}

#endif
