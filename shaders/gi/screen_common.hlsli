// Shared by the screen probe passes (sp_trace.comp, sp_resolve.comp; src/enh/gi.cpp, docs/research/rtgi.md 11). HLSL port of
// screen_common.glsl. Include after gi_common.hlsli and rvk_gi.hlsli.
#pragma once

// the pixel of a tile's probe this frame (jittered per tile and frame: the resolve's temporal accumulation covers the tile)
int2 spProbePixel(GiPassParams p, int2 tile) {
    uint t = max(p.Get().screen2.x, 1u);
    uint h = giHash(uint(tile.x) * 73856093u ^ uint(tile.y) * 19349663u ^ p.Get().volume.w * 83492791u);
    int2 px = tile * int(t) + int2(int(h % t), int((h >> 8) % t));
    return min(px, int2(p.Get().screen2.zw) - 1);
}

// view depth from the hardware depth (the engine's row-vector projection: z' = (z a + b) / z); 0 for the sky
float spViewDepth(RvkFrame f, float z) {
    float a = f.proj[2][2], b = f.proj[3][2];
    return z < 1.0 && abs(z - a) > 1e-9 ? b / (z - a) : 0.0;
}

// world position of a pixel (D3D9 pixel centres as the material shaders: uv = pixel / size)
float3 spWorldPos(RvkFrame f, int2 px, float2 size, float z) {
    float2 uv = (float2(px) + 0.5) / size;
    float4 w = mul(float4(rvkUvToNdc(uv) - rvkJitterNdc(f), z, 1.0), f.invViewProj);
    return w.xyz / w.w;
}

// the geometric normal of a pixel from the hardware depth (per axis the neighbour nearer in depth: no smearing across
// edges), towards the camera; the probes run after the z pre-pass, before the G-buffer exists
float3 spDepthNormal(RvkFrame f, int2 px, float2 size, uint depthTex) {
    int2 maxP = int2(size) - 1;
    float z0 = rvkHwDepthToD3D(f, rvkFetch2D(depthTex, px).r);
    float3 P = spWorldPos(f, px, size, z0);
    int2 pl = max(px - int2(1, 0), int2(0, 0)), pr = min(px + int2(1, 0), maxP);
    int2 pu = max(px - int2(0, 1), int2(0, 0)), pd = min(px + int2(0, 1), maxP);
    float zl = rvkHwDepthToD3D(f, rvkFetch2D(depthTex, pl).r), zr = rvkHwDepthToD3D(f, rvkFetch2D(depthTex, pr).r);
    float zu = rvkHwDepthToD3D(f, rvkFetch2D(depthTex, pu).r), zd = rvkHwDepthToD3D(f, rvkFetch2D(depthTex, pd).r);
    float3 dx = abs(zr - z0) < abs(z0 - zl) ? spWorldPos(f, pr, size, zr) - P : P - spWorldPos(f, pl, size, zl);
    float3 dy = abs(zd - z0) < abs(z0 - zu) ? spWorldPos(f, pd, size, zd) - P : P - spWorldPos(f, pu, size, zu);
    float3 n = cross(dx, dy);
    n = dot(n, n) > 1e-12 ? normalize(n) : float3(0.0, 1.0, 0.0);
    if (dot(n, f.cameraPos.xyz - P) < 0.0) n = -n;
    return n;
}

// the gradient (d / dn of the unconstrained polynomial) of one colour channel of the engine's irradiance polynomial at n
// (rvkGiEvalRegs): how the ambient changes when a pixel's normal tilts away from the surface's (directional correction, rtgi.md 20)
float3 spGradRegs(float4 r[7], float3 n, int ch) {
    float4 a = r[ch], b = r[3 + ch];
    return a.xyz + float3(b.x * n.y + b.w * n.z, b.x * n.x + b.y * n.z, b.y * n.y + 2.0 * b.z * n.z + b.w * n.x) +
           r[6][ch] * float3(2.0 * n.x, -2.0 * n.y, 0.0);
}

// a normal in one float of the probe geometry image (octahedral, 11 bits per axis: exact in a float)
float spPackNormal(float3 n) {
    float2 o = rvkGiOctEncode(n);
    return floor(o.x * 2047.0 + 0.5) * 2048.0 + floor(o.y * 2047.0 + 0.5);
}
float3 spUnpackNormal(float w) {
    float ix = floor(w / 2048.0);
    return rvkGiOctDecode(float2(ix, w - ix * 2048.0) / 2047.0);
}

// previous uv - this uv of a world point (the camera's motion)
float2 spCameraMotion(RvkFrame f, float3 P) {
    float4 c = mul(float4(P, 1.0), f.viewProj);
    float4 q = mul(float4(P, 1.0), f.prevViewProj);
    if (c.w <= 1e-6 || q.w <= 1e-6) return float2(0.0, 0.0);
    return (q.xy / q.w - c.xy / c.w) * float2(0.5, -0.5);
}
