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
    float4 d = RVK_TEX2D(map).GatherRed(RVK_SAMP(smp), (floor(base) + 1.0) / size);
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

// The n x n grid of bilinear taps as ONE weighted pass over the texels they touch (taps spaced `spacing` <= 1 texel apart, so the
// footprints overlap): a bilinear tap is 4 texel compares with weights (1 - f, f) per axis, the grid's sum is separable
// (weight(texel) = wx(x) * wy(y), both summed over the taps), so every texel is fetched once, by gathers of 2 x 2 texels: n = 3 takes
// 4 gathers instead of 9, n = 4 takes 4 (spacing <= 0.5) or 9 instead of 16. The receiver-plane depth is taken at the texel
// itself instead of at the tap it belongs to (a tap's 4 texels used the tap's depth): the same plane, 1/2 texel of bias less
// averaged over the kernel's own width. (n = 2 keeps the exact per-tap form: 4 gathers either way.)
float gridWeight(float base, float lo, float h, float spacing, uint n, float m) {
    float w = 0.0;
    [unroll] for (uint k = 0u; k < 4u; ++k) {
        if (k < n) {
            float p = base + (float(k) - h) * spacing;
            float fl = floor(p);
            float f = p - fl;
            float at = fl - lo;
            w += (at == m ? 1.0 - f : 0.0) + (at + 1.0 == m ? f : 0.0);
        }
    }
    return w;
}

float cascadeShadowGrid(ShadowPtr b, int c, CascadeCoord cc, uint n, float z0) {
    float size = b.Get().texel[c].x;
    float spacing = b.Get().texel[c].z;
    float h = 0.5 * float(n - 1u);
    float2 base = cc.s.xy * size - 0.5;
    float2 lo = floor(base - h * spacing);  // the first tap's first texel (floors grow with the tap index)
    // gathers per axis: the taps' floors differ by at most ceil((n - 1) * spacing), +2 texels
    uint g = (uint(ceil(float(n - 1u) * spacing)) + 3u) / 2u;
    float wx[6], wy[6];
    [unroll] for (uint m = 0u; m < 6u; ++m) {
        wx[m] = gridWeight(base.x, lo.x, h, spacing, n, float(m));
        wy[m] = gridWeight(base.y, lo.y, h, spacing, n, float(m));
    }
    uint map = b.Get().maps[c], smp = b.Get().res.y;
    float sum = 0.0;
    [unroll] for (uint gy = 0u; gy < 3u; ++gy) {
        [unroll] for (uint gx = 0u; gx < 3u; ++gx) {
            if (gx < g && gy < g) {
                float2 cell = lo + float2(float(2u * gx), float(2u * gy));  // texel (x, y) of the gather's lower left
                float4 d = RVK_TEX2D(map).GatherRed(RVK_SAMP(smp), (cell + 1.0) / size);
                float2 o = cell - base;  // texel offset from the filter's centre (u0, v0)
                float zl = z0 + dot(cc.slope, o);
                // gather order: x (u0, v1), y (u1, v1), z (u1, v0), w (u0, v0)
                float4 lit = step(float4(zl + cc.slope.y, zl + cc.slope.x + cc.slope.y, zl + cc.slope.x, zl), d);
                float a = wx[2u * gx], bb = wx[2u * gx + 1u], c0 = wy[2u * gy], c1 = wy[2u * gy + 1u];
                sum += c0 * (a * lit.w + bb * lit.z) + c1 * (a * lit.x + bb * lit.y);
            }
        }
    }
    return sum / float(n * n);
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
    if (n >= 3u) return cascadeShadowGrid(b, c, cc, n, z0);
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

// the pixel's depth and its four neighbours' from two gathers (the 2 x 2 blocks whose corner is the pixel's lower left / upper right
// corner) instead of five fetches; the point-clamp sampler gives the same texels at the border
struct DepthCross { float d, left, right, up, down; };
DepthCross depthCross(ShadowPtr b, float2 fragCoord) {
    uint t = b.Get().res.x, s = b.Get().res.y;
    float4 a = RVK_TEX2D(t).GatherRed(RVK_SAMP(s), (fragCoord - 0.5) * b.Get().target.xy);  // (x-1,y) (x,y) (x,y-1) (x-1,y-1)
    float4 c = RVK_TEX2D(t).GatherRed(RVK_SAMP(s), (fragCoord + 0.5) * b.Get().target.xy);  // (x,y+1) (x+1,y+1) (x+1,y) (x,y)
    DepthCross r;
    r.d = a.y;
    r.left = a.x;
    r.up = a.z;     // y - 1
    r.right = c.z;
    r.down = c.x;   // y + 1
    return r;
}

// p(neighbour) - p along +axis, taken on the side whose depth is closer (one-sided difference)
float4 surfaceStep(ShadowPtr b, float2 fragCoord, float d, float4 p, float2 axis, float dPlus, float dMinus) {
    if (abs(dPlus - d) <= abs(dMinus - d)) return reconstruct(b, fragCoord + axis, dPlus) - p;
    return p - reconstruct(b, fragCoord - axis, dMinus);
}

float4 PSMain(float4 position : SV_Position) : SV_Target0 {
    ShadowPtr b = rvkShadowBlock();
    float2 fragCoord = position.xy;
    DepthCross dc = depthCross(b, fragCoord);
    float d = dc.d;  // the engine samples the depth at t + half a pixel
    float4 p = reconstruct(b, fragCoord, d);
    // the surface's screen gradient from the neighbour on the same surface on each axis (the smaller depth step): screen
    // derivatives (dFdx) jump across every stone and leaf edge, and a wrong receiver plane shadows the surface itself
    float4 px = surfaceStep(b, fragCoord, d, p, float2(1.0, 0.0), dc.right, dc.left);
    float4 py = surfaceStep(b, fragCoord, d, p, float2(0.0, 1.0), dc.down, dc.up);

    int count = int(clamp(b.Get().maps.w, 1u, 3u));
    uint dbg = b.Get().mode.y;
    float margin = 0.5 * float(clamp(b.Get().mode.x, 1u, 4u)) * max(b.Get().texel[0].z, 1.0) + 1.0;

    // the ray-traced shadow within its range (rt_shadow_*.comp: same depth map, same reconstruction), blended out to the
    // cascades over the last band; its rays see the terrain as well, so the variance map's term goes with it. Where it covers
    // the pixel completely the cascades and the terrain term are not evaluated at all (the debug views want them)
    float rtWeight = 0.0, rtVis = 1.0;
    if (b.Get().rt.x != 0u && d < b.Get().rtParams.x) {
        rtWeight = 1.0 - smoothstep(b.Get().rtParams.x * (1.0 - b.Get().rtParams.y), b.Get().rtParams.x, d);
        if (rtWeight > 0.0) rtVis = rvkFetch2D(b.Get().rt.x, int2(fragCoord)).r;
    }
    if (rtWeight >= 1.0 && dbg == 0u) return float4(rtVis, rtVis, rtVis, rtVis);

    // the first cascade whose range holds the pixel (and whose map covers it); only the cascades the pixel can reach have their
    // texture coordinates and receiver plane computed
    int c = 0;
    CascadeCoord cur = (CascadeCoord)0;
    while (c < count) {
        if (d <= b.Get().split[c]) {
            cur = cascadeCoord(b, c, p, px, py);
            if (inside(cur, b, c, margin)) break;
        }
        ++c;
    }

    float shadow = 1.0;
    float level = 1.0;  // debug: the cascade
    if (c < count) {
        shadow = cascadeShadow(b, c, cur);
        level = 0.25 * float(c + 1);
        float nearEnd = c > 0 ? b.Get().split[c - 1] : 0.0;
        float range = b.Get().split[c] - nearEnd;
        if (c + 1 < count) {
            // the band before the split: towards the next cascade
            float start = b.Get().split[c] - b.Get().params.x * range;
            if (d > start) {
                CascadeCoord nxt = cascadeCoord(b, c + 1, p, px, py);
                if (inside(nxt, b, c + 1, margin)) {
                    float w = smoothstep(start, b.Get().split[c], d);
                    shadow = lerp(shadow, cascadeShadow(b, c + 1, nxt), w);
                    level = lerp(level, 0.25 * float(c + 2), w);
                }
            }
        } else {
            // the last cascade: fade out towards its end (the terrain map alone beyond)
            float start = b.Get().split[c] - b.Get().params.y * range;
            float w = smoothstep(start, b.Get().split[c], d);
            shadow = lerp(shadow, 1.0, w);
            level = lerp(level, 1.0, w);
        }
    }
    // fully shadowed by the cascades: the product is 0 whatever the terrain term is (no variance map fetch)
    float terrain = (shadow == 0.0 && dbg != 3u) ? 1.0 : varianceShadow(b, p);
    float mask = shadow * terrain;
    mask = lerp(mask, rtVis, rtWeight);
    if (dbg == 1u) mask = level;
    else if (dbg == 2u) mask = shadow;
    else if (dbg == 3u) mask = terrain;
    else if (dbg == 4u) mask = lerp(1.0, rtVis, rtWeight);
    else if (dbg == 5u) mask = rtWeight;
    return float4(mask, mask, mask, mask);
}
