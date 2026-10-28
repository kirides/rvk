#version 460
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

#extension GL_EXT_buffer_reference_uvec2 : require

#include "rvk_abi.glsl"
#include "rvk_common.glsl"
#include "shadow_block.glsl"

layout(location = 0) out vec4 outColor;

ShadowBlock shadowBlock() { return ShadowBlock(uvec2(pc.draw)); }

// one bilinear PCF tap at `uv` (map texels: x size, y 1 / size): the 4 texels around it (one gather) compared with z
float pcfTap(uint map, uint smp, vec2 uv, float size, float z) {
    vec2 base = uv * size - 0.5;
    vec2 f = fract(base);
    vec4 d = textureGather(sampler2D(t2D[nonuniformEXT(map)], samp[nonuniformEXT(smp)]), (floor(base) + 1.0) / size, 0);
    vec4 lit = step(vec4(z), d);  // the engine's compare: lit where z < the stored depth
    // gather order: x (u0, v1), y (u1, v1), z (u1, v0), w (u0, v0)
    return mix(mix(lit.w, lit.z, f.x), mix(lit.x, lit.y, f.x), f.y);
}

struct CascadeCoord {
    vec3 s;       // texture coordinates, shadow depth
    vec2 slope;   // d depth / d texel (receiver plane, clamped)
};

// px / py: the change of p from this pixel to its right / lower neighbour on the same surface (shadowGradients)
CascadeCoord cascadeCoord(ShadowBlock b, int c, vec4 p, vec4 px, vec4 py) {
    CascadeCoord cc;
    mat4 m = mat4(b.cascade[c * 4], b.cascade[c * 4 + 1], b.cascade[c * 4 + 2], b.cascade[c * 4 + 3]);  // columns = rows
    vec4 t = p * m;
    cc.s = t.xyz / t.w;
    // receiver-plane depth bias: d depth / d uv of the surface (orthographic: the linear part of the rows)
    vec3 dx = (px * m).xyz, dy = (py * m).xyz;
    mat2 j = mat2(dx.xy, dy.xy);  // d uv / d screen
    float det = determinant(j);
    vec2 g = vec2(0.0);
    if (abs(det) > 1e-20) g = inverse(transpose(j)) * vec2(dx.z, dy.z);  // d depth / d uv
    float maxSlope = b.params.w * b.texel[c].w;
    cc.slope = clamp(g * b.texel[c].y, vec2(-maxSlope), vec2(maxSlope));
    return cc;
}

bool inside(CascadeCoord cc, ShadowBlock b, int c, float margin) {
    vec2 m = vec2(margin * b.texel[c].y);
    return all(greaterThanEqual(cc.s.xy, m)) && all(lessThanEqual(cc.s.xy, 1.0 - m)) && cc.s.z <= 1.0;
}

float cascadeShadow(ShadowBlock b, int c, CascadeCoord cc) {
    uint n = clamp(b.mode.x, 1u, 4u);
    float size = b.texel[c].x;
    float spacing = b.texel[c].z;
    float z0 = cc.s.z - b.params.z * b.texel[c].w;
    float sum = 0.0;
    float h = 0.5 * float(n - 1u);
    for (uint i = 0u; i < n; ++i) {
        for (uint k = 0u; k < n; ++k) {
            vec2 o = (vec2(float(k), float(i)) - h) * spacing;  // texels
            sum += pcfTap(b.maps[c], b.res.y, cc.s.xy + o * b.texel[c].y, size, z0 + dot(cc.slope, o));
        }
    }
    return sum / float(n * n);
}

// SSM's terrain term: the variance map (Chebyshev with the engine's light-bleeding cut)
float varianceShadow(ShadowBlock b, vec4 p) {
    if (b.res.z == 0u) return 1.0;
    vec3 v = vec3(dot(b.vsm[0], p), dot(b.vsm[1], p), dot(b.vsm[2], p));
    vec2 m = rvkSample2DLod(b.res.z, b.res.w, v.xy, 0.0).xy;
    if (v.z - m.x <= 0.0) return 1.0;
    float variance = max(m.y - m.x * m.x, b.vsmParams.x);
    float dz = v.z - m.x;
    return clamp((variance / (variance + dz * dz) - b.vsmParams.y) * b.vsmParams.z, 0.0, 1.0);
}

// the engine's quad: texcoord t = the D3D9 pixel position / size (pixel centres at integers), p = d * the view ray
vec4 reconstruct(ShadowBlock b, vec2 fragCoord, float d) {
    vec2 t = (fragCoord - 0.5) * b.target.xy;
    // the depth map was rendered with the camera jitter: the surface at this pixel is at the pixel's NDC - J (rvk_common.glsl)
    return vec4(d * vec3(vec2(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0) - rvkJitterNdc(pc.frame), 1.0), 1.0);
}

float depthAt(ShadowBlock b, vec2 fragCoord) { return rvkSample2DLod(b.res.x, b.res.y, fragCoord * b.target.xy, 0.0).r; }

// p(neighbour) - p along +axis, taken on the side whose depth is closer (one-sided difference)
vec4 surfaceStep(ShadowBlock b, vec2 fragCoord, float d, vec4 p, vec2 axis) {
    float dPlus = depthAt(b, fragCoord + axis);
    float dMinus = depthAt(b, fragCoord - axis);
    if (abs(dPlus - d) <= abs(dMinus - d)) return reconstruct(b, fragCoord + axis, dPlus) - p;
    return p - reconstruct(b, fragCoord - axis, dMinus);
}

void main() {
    ShadowBlock b = shadowBlock();
    float d = depthAt(b, gl_FragCoord.xy);  // the engine samples the depth at t + half a pixel
    vec4 p = reconstruct(b, gl_FragCoord.xy, d);
    // the surface's screen gradient from the neighbour on the same surface on each axis (the smaller depth step): screen
    // derivatives (dFdx) jump across every stone and leaf edge, and a wrong receiver plane shadows the surface itself
    vec4 px = surfaceStep(b, gl_FragCoord.xy, d, p, vec2(1.0, 0.0));
    vec4 py = surfaceStep(b, gl_FragCoord.xy, d, p, vec2(0.0, 1.0));

    int count = int(clamp(b.maps.w, 1u, 3u));
    CascadeCoord cc[3];
    for (int c = 0; c < 3; ++c) cc[c] = cascadeCoord(b, c, p, px, py);

    float margin = 0.5 * float(clamp(b.mode.x, 1u, 4u)) * max(b.texel[0].z, 1.0) + 1.0;
    // the first cascade whose range holds the pixel (and whose map covers it)
    int c = 0;
    while (c < count && (d > b.split[c] || !inside(cc[c], b, c, margin))) ++c;

    float shadow = 1.0;
    float level = 1.0;  // debug: the cascade
    if (c < count) {
        shadow = cascadeShadow(b, c, cc[c]);
        level = 0.25 * float(c + 1);
        float near = c > 0 ? b.split[c - 1] : 0.0;
        float range = b.split[c] - near;
        if (c + 1 < count) {
            // the band before the split: towards the next cascade
            float start = b.split[c] - b.params.x * range;
            if (d > start && inside(cc[c + 1], b, c + 1, margin)) {
                float w = smoothstep(start, b.split[c], d);
                shadow = mix(shadow, cascadeShadow(b, c + 1, cc[c + 1]), w);
                level = mix(level, 0.25 * float(c + 2), w);
            }
        } else {
            // the last cascade: fade out towards its end (the terrain map alone beyond)
            float start = b.split[c] - b.params.y * range;
            float w = smoothstep(start, b.split[c], d);
            shadow = mix(shadow, 1.0, w);
            level = mix(level, 1.0, w);
        }
    }
    float terrain = varianceShadow(b, p);
    float mask = shadow * terrain;
    // the ray-traced shadow within its range (rt_shadow_*.comp: same depth map, same reconstruction), blended out to the
    // cascades over the last band; its rays see the terrain as well, so the variance map's term goes with it
    float rtWeight = 0.0, rtVis = 1.0;
    if (b.rt.x != 0u && d < b.rtParams.x) {
        rtWeight = 1.0 - smoothstep(b.rtParams.x * (1.0 - b.rtParams.y), b.rtParams.x, d);
        if (rtWeight > 0.0) rtVis = rvkFetch2D(b.rt.x, ivec2(gl_FragCoord.xy)).r;
        mask = mix(mask, rtVis, rtWeight);
    }
    if (b.mode.y == 1u) mask = level;
    else if (b.mode.y == 2u) mask = shadow;
    else if (b.mode.y == 3u) mask = terrain;
    else if (b.mode.y == 4u) mask = mix(1.0, rtVis, rtWeight);
    else if (b.mode.y == 5u) mask = rtWeight;
    outColor = vec4(mask);
}
