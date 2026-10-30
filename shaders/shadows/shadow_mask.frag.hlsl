// Enhanced shadows (src/enh/shadows.cpp, docs/research/shadows.md): the sun's screen-space shadow mask in one pass, in
// place of the engine's post_SSM.fx cascade draws (technique SSM: one draw per cascade, each 8 dithered point taps, the
// cascade picked by the stencil, a hard switch at each split and a short fade at the range's end).
//
// Per pixel: the engine's own reconstruction (p = depth-map value * the view ray, the matrices the engine set for its
// cascade draws: p -> cascade texture coordinates), a smooth PCF (a grid of bilinear taps: 4 compares from one gather each,
// with a receiver-plane depth bias), the cascades blended across a band before each split, a long fade at the end of the
// last cascade, times the engine's terrain variance map (VSM) exactly as SSM does. Output: the mask's alpha (the draw keeps
// the engine's colour write mask: alpha only).
// Mirror: enh::ShadowBlock (static_assert'ed size).

#include "shadow_block.hlsli"

// one bilinear PCF tap at `uv` (map texels: x size, y 1 / size): the 4 texels around it (one gather) compared with z
float pcfTap(uint map, uint smp, float2 uv, float size, float z) {
    float2 base = uv * size - 0.5;
    float2 f = frac(base);
    float4 d = g_t2D[NonUniformResourceIndex(map)].GatherRed(g_samp[NonUniformResourceIndex(smp)], (floor(base) + 1.0) / size);
    float4 lit = step(float4(z, z, z, z), d);  // the engine's compare: lit where z < the stored depth
    // gather order: x (u0, v1), y (u1, v1), z (u1, v0), w (u0, v0)
    return lerp(lerp(lit.w, lit.z, f.x), lerp(lit.x, lit.y, f.x), f.y);
}

struct CascadeCoord {
    float3 s;       // texture coordinates, shadow depth
    float2 slope;   // d depth / d texel (receiver plane, clamped)
};

// px / py: the change of p from this pixel to its right / lower neighbour on the same surface (shadowGradients)
CascadeCoord cascadeCoord(ShadowPtr b, int c, float4 p, float4 px, float4 py) {
    CascadeCoord cc;
    // GLSL: mat4 m = mat4(rows as columns); p * m = (dot(p, row0), .., dot(p, row3)) = mul(M, p) with the rows as rows
    float4x4 m = float4x4(b.Get().cascade[c * 4], b.Get().cascade[c * 4 + 1], b.Get().cascade[c * 4 + 2], b.Get().cascade[c * 4 + 3]);
    float4 t = mul(m, p);
    cc.s = t.xyz / t.w;
    // receiver-plane depth bias: d depth / d uv of the surface (orthographic: the linear part of the rows)
    float3 dx = mul(m, px).xyz, dy = mul(m, py).xyz;
    // GLSL: mat2 j = mat2(dx.xy, dy.xy) (columns); det = determinant(j); g = inverse(transpose(j)) * vec2(dx.z, dy.z)
    // transpose(j) = [[dx.x, dx.y], [dy.x, dy.y]]; its inverse = 1 / det * [[dy.y, -dx.y], [-dy.x, dx.x]]
    float det = dx.x * dy.y - dy.x * dx.y;
    float2 g = float2(0.0, 0.0);
    if (abs(det) > 1e-20) g = float2(dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z) / det;  // d depth / d uv
    float maxSlope = b.Get().params.w * b.Get().texel[c].w;
    cc.slope = clamp(g * b.Get().texel[c].y, float2(-maxSlope, -maxSlope), float2(maxSlope, maxSlope));
    return cc;
}

bool inside(CascadeCoord cc, ShadowPtr b, int c, float margin) {
    float mg = margin * b.Get().texel[c].y;
    float2 m = float2(mg, mg);
    return all(cc.s.xy >= m) && all(cc.s.xy <= 1.0 - m) && cc.s.z <= 1.0;
}

float cascadeShadow(ShadowPtr b, int c, CascadeCoord cc) {
    uint n = clamp(b.Get().mode.x, 1u, 4u);
    float size = b.Get().texel[c].x;
    float spacing = b.Get().texel[c].z;
    float z0 = cc.s.z - b.Get().params.z * b.Get().texel[c].w;
    float sum = 0.0;
    float h = 0.5 * float(n - 1u);
    for (uint i = 0u; i < n; ++i) {
        for (uint k = 0u; k < n; ++k) {
            float2 o = (float2(float(k), float(i)) - h) * spacing;  // texels
            sum += pcfTap(b.Get().maps[c], b.Get().res.y, cc.s.xy + o * b.Get().texel[c].y, size, z0 + dot(cc.slope, o));
        }
    }
    return sum / float(n * n);
}

// SSM's terrain term: the variance map (Chebyshev with the engine's light-bleeding cut)
float varianceShadow(ShadowPtr b, float4 p) {
    if (b.Get().res.z == 0u) return 1.0;
    float3 v = float3(dot(b.Get().vsm[0], p), dot(b.Get().vsm[1], p), dot(b.Get().vsm[2], p));
    float2 m = rvkSample2DLod(b.Get().res.z, b.Get().res.w, v.xy, 0.0).xy;
    if (v.z - m.x <= 0.0) return 1.0;
    float variance = max(m.y - m.x * m.x, b.Get().vsmParams.x);
    float dz = v.z - m.x;
    return clamp((variance / (variance + dz * dz) - b.Get().vsmParams.y) * b.Get().vsmParams.z, 0.0, 1.0);
}

// the engine's quad: texcoord t = the D3D9 pixel position / size (pixel centres at integers), p = d * the view ray
float4 reconstruct(ShadowPtr b, float2 fragCoord, float d) {
    float2 t = (fragCoord - 0.5) * b.Get().target.xy;
    // the depth map was rendered with the camera jitter: the surface at this pixel is at the pixel's NDC - J (rvk_common.hlsli)
    return float4(d * float3(float2(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0) - rvkJitterNdc(rvkLoadFrame(pc.frame)), 1.0), 1.0);
}

float depthAt(ShadowPtr b, float2 fragCoord) { return rvkSample2DLod(b.Get().res.x, b.Get().res.y, fragCoord * b.Get().target.xy, 0.0).r; }

// p(neighbour) - p along +axis, taken on the side whose depth is closer (one-sided difference)
float4 surfaceStep(ShadowPtr b, float2 fragCoord, float d, float4 p, float2 axis) {
    float dPlus = depthAt(b, fragCoord + axis);
    float dMinus = depthAt(b, fragCoord - axis);
    if (abs(dPlus - d) <= abs(dMinus - d)) return reconstruct(b, fragCoord + axis, dPlus) - p;
    return p - reconstruct(b, fragCoord - axis, dMinus);
}

float4 PSMain(float4 position : SV_Position) : SV_Target0 {
    ShadowPtr b = rvkShadowBlock();
    float2 fragCoord = position.xy;
    float d = depthAt(b, fragCoord);  // the engine samples the depth at t + half a pixel
    float4 p = reconstruct(b, fragCoord, d);
    // the surface's screen gradient from the neighbour on the same surface on each axis (the smaller depth step): screen
    // derivatives (dFdx) jump across every stone and leaf edge, and a wrong receiver plane shadows the surface itself
    float4 px = surfaceStep(b, fragCoord, d, p, float2(1.0, 0.0));
    float4 py = surfaceStep(b, fragCoord, d, p, float2(0.0, 1.0));

    int count = int(clamp(b.Get().maps.w, 1u, 3u));
    CascadeCoord cc[3];
    for (int ci = 0; ci < 3; ++ci) cc[ci] = cascadeCoord(b, ci, p, px, py);

    float margin = 0.5 * float(clamp(b.Get().mode.x, 1u, 4u)) * max(b.Get().texel[0].z, 1.0) + 1.0;
    // the first cascade whose range holds the pixel (and whose map covers it)
    int c = 0;
    while (c < count && (d > b.Get().split[c] || !inside(cc[c], b, c, margin))) ++c;

    float shadow = 1.0;
    float level = 1.0;  // debug: the cascade
    if (c < count) {
        shadow = cascadeShadow(b, c, cc[c]);
        level = 0.25 * float(c + 1);
        float nearEnd = c > 0 ? b.Get().split[c - 1] : 0.0;
        float range = b.Get().split[c] - nearEnd;
        if (c + 1 < count) {
            // the band before the split: towards the next cascade
            float start = b.Get().split[c] - b.Get().params.x * range;
            if (d > start && inside(cc[c + 1], b, c + 1, margin)) {
                float w = smoothstep(start, b.Get().split[c], d);
                shadow = lerp(shadow, cascadeShadow(b, c + 1, cc[c + 1]), w);
                level = lerp(level, 0.25 * float(c + 2), w);
            }
        } else {
            // the last cascade: fade out towards its end (the terrain map alone beyond)
            float start = b.Get().split[c] - b.Get().params.y * range;
            float w = smoothstep(start, b.Get().split[c], d);
            shadow = lerp(shadow, 1.0, w);
            level = lerp(level, 1.0, w);
        }
    }
    float terrain = varianceShadow(b, p);
    float mask = shadow * terrain;
    // the ray-traced shadow within its range (rt_shadow_*.comp: same depth map, same reconstruction), blended out to the
    // cascades over the last band; its rays see the terrain as well, so the variance map's term goes with it
    float rtWeight = 0.0, rtVis = 1.0;
    if (b.Get().rt.x != 0u && d < b.Get().rtParams.x) {
        rtWeight = 1.0 - smoothstep(b.Get().rtParams.x * (1.0 - b.Get().rtParams.y), b.Get().rtParams.x, d);
        if (rtWeight > 0.0) rtVis = rvkFetch2D(b.Get().rt.x, int2(fragCoord)).r;
        mask = lerp(mask, rtVis, rtWeight);
    }
    uint dbg = b.Get().mode.y;
    if (dbg == 1u) mask = level;
    else if (dbg == 2u) mask = shadow;
    else if (dbg == 3u) mask = terrain;
    else if (dbg == 4u) mask = lerp(1.0, rtVis, rtWeight);
    else if (dbg == 5u) mask = rtWeight;
    return float4(mask, mask, mask, mask);
}
