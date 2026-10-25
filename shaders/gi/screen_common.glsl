// Shared by the screen probe passes (sp_trace.comp, sp_resolve.comp; src/enh/gi.cpp, docs/research/rtgi.md 11). Include
// after gi_common.glsl.

#ifndef SCREEN_COMMON_GLSL
#define SCREEN_COMMON_GLSL

// the pixel of a tile's probe this frame (jittered per tile and frame: the resolve's temporal accumulation covers the tile)
ivec2 spProbePixel(GiPassParams p, ivec2 tile) {
    uint t = max(p.screen2.x, 1u);
    uint h = giHash(uint(tile.x) * 73856093u ^ uint(tile.y) * 19349663u ^ p.volume.w * 83492791u);
    ivec2 px = tile * int(t) + ivec2(int(h % t), int((h >> 8) % t));
    return min(px, ivec2(p.screen2.zw) - 1);
}

// view depth from the hardware depth (the engine's row-vector projection: z' = (z a + b) / z); 0 for the sky
float spViewDepth(RvkFrame f, float z) {
    float a = f.proj[2][2], b = f.proj[3][2];
    return z < 1.0 && abs(z - a) > 1e-9 ? b / (z - a) : 0.0;
}

// world position of a pixel (D3D9 pixel centres as the material shaders: uv = pixel / size)
vec3 spWorldPos(RvkFrame f, ivec2 px, vec2 size, float z) {
    vec2 uv = (vec2(px) + 0.5) / size;
    vec4 w = f.invViewProj * vec4(rvkUvToNdc(uv) - rvkJitterNdc(f), z, 1.0);
    return w.xyz / w.w;
}

// the geometric normal of a pixel from the hardware depth (per axis the neighbour nearer in depth: no smearing across
// edges), towards the camera; the probes run after the z pre-pass, before the G-buffer exists
vec3 spDepthNormal(RvkFrame f, ivec2 px, vec2 size, uint depthTex) {
    ivec2 maxP = ivec2(size) - 1;
    float z0 = rvkHwDepthToD3D(f, rvkFetch2D(depthTex, px).r);
    vec3 P = spWorldPos(f, px, size, z0);
    ivec2 pl = max(px - ivec2(1, 0), ivec2(0)), pr = min(px + ivec2(1, 0), maxP);
    ivec2 pu = max(px - ivec2(0, 1), ivec2(0)), pd = min(px + ivec2(0, 1), maxP);
    float zl = rvkHwDepthToD3D(f, rvkFetch2D(depthTex, pl).r), zr = rvkHwDepthToD3D(f, rvkFetch2D(depthTex, pr).r);
    float zu = rvkHwDepthToD3D(f, rvkFetch2D(depthTex, pu).r), zd = rvkHwDepthToD3D(f, rvkFetch2D(depthTex, pd).r);
    vec3 dx = abs(zr - z0) < abs(z0 - zl) ? spWorldPos(f, pr, size, zr) - P : P - spWorldPos(f, pl, size, zl);
    vec3 dy = abs(zd - z0) < abs(z0 - zu) ? spWorldPos(f, pd, size, zd) - P : P - spWorldPos(f, pu, size, zu);
    vec3 n = cross(dx, dy);
    n = dot(n, n) > 1e-12 ? normalize(n) : vec3(0.0, 1.0, 0.0);
    if (dot(n, f.cameraPos.xyz - P) < 0.0) n = -n;
    return n;
}

// previous uv - this uv of a world point (the camera's motion)
vec2 spCameraMotion(RvkFrame f, vec3 P) {
    vec4 c = f.viewProj * vec4(P, 1.0);
    vec4 q = f.prevViewProj * vec4(P, 1.0);
    if (c.w <= 1e-6 || q.w <= 1e-6) return vec2(0.0);
    return (q.xy / q.w - c.xy / c.w) * vec2(0.5, -0.5);
}

#endif
