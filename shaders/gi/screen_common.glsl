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
    vec4 w = f.invViewProj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, z, 1.0);
    return w.xyz / w.w;
}

#endif
