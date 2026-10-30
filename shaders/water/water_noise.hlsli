// Noise and the foam pattern of the enhanced water (docs/research/ocean.md 4.4). No bindings: included by water_common.hlsli
// (the water draws) and by the FFT ocean's foam pass (ocean_foam.comp.hlsl). Port of water_noise.glsl.
#pragma once

#include "rvk_common.hlsli"

// ---- gradient noise with analytic derivatives (after Inigo Quilez, MIT) ---------------------------------------------------

// two random numbers in -1..1 for an integer lattice point (p = floor of something): the pcg2d hash (Jarzynski and Olano,
// "Hash Functions for GPU Rendering", 2020) - integer arithmetic, much cheaper than the classic sin-fract hash and without
// its precision artefacts far from the origin
float2 waterHash(float2 p) {
    uint2 v = uint2(int2(p));
    v = v * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    return float2(v >> 8u) * (2.0 / 16777216.0) - 1.0;
}

// value, d/dx, d/dy
float3 waterNoised(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
    float2 ga = waterHash(i + float2(0.0, 0.0));
    float2 gb = waterHash(i + float2(1.0, 0.0));
    float2 gc = waterHash(i + float2(0.0, 1.0));
    float2 gd = waterHash(i + float2(1.0, 1.0));
    float va = dot(ga, f - float2(0.0, 0.0));
    float vb = dot(gb, f - float2(1.0, 0.0));
    float vc = dot(gc, f - float2(0.0, 1.0));
    float vd = dot(gd, f - float2(1.0, 1.0));
    return float3(va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * (va - vb - vc + vd),
                  ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd) +
                      du * (u.yx * (va - vb - vc + vd) + float2(vb, vc) - va));
}

// value only
float waterNoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float va = dot(waterHash(i), f);
    float vb = dot(waterHash(i + float2(1.0, 0.0)), f - float2(1.0, 0.0));
    float vc = dot(waterHash(i + float2(0.0, 1.0)), f - float2(0.0, 1.0));
    float vd = dot(waterHash(i + float2(1.0, 1.0)), f - float2(1.0, 1.0));
    return va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * (va - vb - vc + vd);
}

// cellular noise (Worley): distances to the nearest and second-nearest feature point (cells of size 1)
float2 waterWorley(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float f1 = 8.0, f2 = 8.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float2 g = float2(float(x), float(y));
            float2 o = waterHash(i + g) * 0.45 + 0.5;
            float2 r = g + o - f;
            float d = dot(r, r);
            if (d < f1) {
                f2 = f1;
                f1 = d;
            } else if (d < f2) {
                f2 = d;
            }
        }
    }
    return sqrt(float2(f1, f2));
}

// the nearest feature point only
float waterWorleyF1(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float f1 = 8.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float2 g = float2(float(x), float(y));
            float2 r = g + waterHash(i + g) * 0.45 + 0.5 - f;
            f1 = min(f1, dot(r, r));
        }
    }
    return sqrt(f1);
}

// fractal sum of `octaves` noise octaves starting at `scale` world units, normalised (std ~0.1); octaves smaller than a few
// pixels (footprint = world units per pixel) fade out (no sparkle at distance)
float waterFbm(float2 xz, float footprint, float scale, int octaves, float gain) {
    float s = 0.0, a = 1.0, n = 0.0;
    for (int i = 0; i < octaves; ++i) {
        float vis = smoothstep(2.0, 5.0, scale / footprint);
        if (vis > 0.0) s += a * vis * waterNoise(xz / scale + float2(3.1, -1.7) * float(i));
        n += a;
        a *= gain;
        scale /= 2.1;
    }
    return s / n;
}

// the same with its gradient: x value, yz d value / d xz (world units^-1)
float3 waterFbmd(float2 xz, float footprint, float scale, int octaves, float gain) {
    float3 s = float3(0.0, 0.0, 0.0);
    float a = 1.0, n = 0.0;
    for (int i = 0; i < octaves; ++i) {
        float vis = smoothstep(2.0, 5.0, scale / footprint);
        if (vis > 0.0) {
            float3 nd = waterNoised(xz / scale + float2(3.1, -1.7) * float(i));
            s += a * vis * float3(nd.x, nd.yz / scale);
        }
        n += a;
        a *= gain;
        scale /= 2.1;
    }
    return s / n;
}

// the nearest feature point (cells of size 1), each point with its own size (0.6 .. 1.25): x the distance divided by the
// point's size (round holes of varied sizes), yz its gradient
float3 waterWorleyF1d(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float f1 = 8.0;
    float2 best = float2(1.0, 0.0);
    float bestK = 1.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float2 g = float2(float(x), float(y));
            float2 hsh = waterHash(i + g);
            float2 r = g + hsh * 0.45 + 0.5 - f;
            float k = 1.0 / (0.92 + 0.33 * frac(hsh.x * 7.31 + hsh.y * 3.17) * 2.0 - 0.33);
            float d = dot(r, r) * k * k;
            if (d < f1) {
                f1 = d;
                best = r;
                bestK = k;
            }
        }
    }
    float d = sqrt(f1);
    return float3(d, -best * (bestK * bestK) / max(d, 1e-4));
}

// ---- foam pattern ----------------------------------------------------------------------------------------------------------
// Sea foam is a layer of bubbles of many sizes: dense, thick white water right after a wave broke, torn open by holes that
// grow and merge as it ages until a thin lace is left between them, then streaks and specks. The pattern (prototyped
// offline, docs/research/ocean.md 4.4): a warped fractal sheet thresholded softly by the coverage (the soft band is wider
// for thin, old foam), round holes of three sizes with soft rims (cellular distance to scattered points: round, not a
// net), a slow variation of the coverage over the water (no large uniform areas) and a thickness variation (bubbles of
// different sizes). It returns a thickness (0..1, the shading turns it into opacity and light) and the thickness's gradient
// (the foam's relief: lit on the sun's side, shaded in its lee). Attached to the water; the caller advects it with the flow.

struct FoamField {
    float sheet;       // ~uniform 0..1 (the logistic of a warped fbm): foam where sheet > 1 - coverage
    float2 sheetGrad;  // its gradient (world units^-1)
    float macro;       // 0..1 slow variation of the coverage over the water
    float3 hole;       // nearest-point distances of the three hole sizes (cells of 48 / 17 / 7 units)
    float2 holeGrad[3];  // gradients of the holes' distances (world units^-1)
    float3 vis;        // visibility of each hole size at this footprint
    float var;         // slow variation of the hole sizes
    float detail;      // 0..1 thickness variation (bubbles of different sizes)
    float far;         // 0..1: the pattern is below a pixel, the foam is its mean
};

// fpMin / fpMax: the pixel's shortest and longest extent on the water (world units; at grazing views the pattern is squashed
// along the view: the fractal sheet follows the longest extent, the holes keep their detail along the shortest and are
// filtered per pixel by foamDensity)
FoamField foamField(float2 xz, float fpMin, float fpMax) {
    FoamField ff;
    float footprint = max(fpMax, 1e-3);
    fpMin = max(fpMin, 1e-3);
    float fpMean = sqrt(fpMin * footprint);
    ff.far = smoothstep(30.0, 90.0, fpMean);
    float2 w = float2(waterNoise(xz / 300.0 + float2(0.04, 0.0)), waterNoise(xz / 300.0 + float2(0.0, 0.24))) * 0.75;
    float2 p = xz + w * 110.0;
    // the sheet's fbm (std ~0.1) through a logistic: about uniform, so the coverage is the share of foam
    float3 s = waterFbmd(p, footprint, 150.0, 4, 0.58);
    ff.sheet = 1.0 / (1.0 + exp(-11.0 * s.x));
    ff.sheetGrad = 11.0 * ff.sheet * (1.0 - ff.sheet) * s.yz;
    ff.macro = clamp(0.5 + 0.8 * waterNoise(xz / 900.0 + float2(5.2, -1.3)), 0.0, 1.0);
    ff.var = clamp(0.55 + 2.7 * waterFbm(p + float2(300.0, 0.0), footprint, 90.0, 1, 0.5), 0.3, 1.3);
    ff.detail = clamp(0.5 + 2.0 * waterFbm(p + float2(51.0, -17.0), footprint, 22.0, 1, 0.5), 0.0, 1.0);
    // the holes' own small warp: torn, irregular openings rather than circles (seen from close by)
    float2 ph = p;
    float hwVis = smoothstep(2.0, 5.0, 19.0 / footprint);
    if (hwVis > 0.0) ph += float2(waterNoise(p / 19.0 + float2(2.3, 7.7)), waterNoise(p / 19.0 + float2(9.1, 1.4))) * (14.0 * hwVis);
    const float3 SIZE = float3(48.0, 17.0, 7.0);
    for (int i = 0; i < 3; ++i) {
        // (a hole thinner than ~2 pixels along the longest extent would flicker as a dash: gone)
        ff.vis[i] = smoothstep(2.5, 6.0, SIZE[i] / fpMin) * smoothstep(1.0, 2.5, SIZE[i] / footprint);
        ff.hole[i] = 1.0;
        ff.holeGrad[i] = float2(0.0, 0.0);
        if (ff.vis[i] > 0.0) {
            float3 h = waterWorleyF1d(ph / SIZE[i] + float2(17.3, 4.1) * float(i));
            ff.hole[i] = h.x;
            ff.holeGrad[i] = h.yz / SIZE[i];
        }
    }
    return ff;
}

// thickness (0..1) of the foam at coverage c (0..1) and freshness (0 old lace .. 1 fresh white water); dX / dY = the screen
// derivatives of the pattern's position (world units per pixel along x / y): every transition is at least ~2 pixels wide
// along the screen (soft edges also where the view squashes the pattern, at grazing views: no hard lines, no sparkle);
// grad = the thickness's gradient (world units^-1)
float foamDensity(FoamField ff, float c, float fresh, float2 dX, float2 dY, out float2 grad) {
    grad = float2(0.0, 0.0);
    c = clamp(c, 0.0, 1.0);
    if (c <= 0.0) return 0.0;
    // the coverage varies slowly over the water: no large uniform areas
    float ce = clamp(c * (0.5 + ff.macro), 0.0, 1.0);
    // the sheet: a soft threshold, wider for thin, old foam (no cut-out edges)
    float sc = clamp(ce * 1.2, 0.0, 1.0);
    float pxS = length(float2(dot(ff.sheetGrad, dX), dot(ff.sheetGrad, dY)));  // the sheet's change per pixel
    float wv = max(0.14 + 0.12 * (1.0 - ce), 1.2 * pxS);
    float e0 = 1.0 - sc - wv;
    float ts = clamp((ff.sheet - e0) / (2.0 * wv), 0.0, 1.0);
    float S = ts * ts * (3.0 - 2.0 * ts);
    float2 gS = (6.0 * ts * (1.0 - ts) / (2.0 * wv)) * ff.sheetGrad;
    // the big holes: large in old, thin foam (only a lace between them is left), small in fresh white water; soft rims
    float r1 = (0.68 - 0.62 * ce) * ff.var * lerp(1.0, 0.7, fresh);
    float pxH = length(float2(dot(ff.holeGrad[0], dX), dot(ff.holeGrad[0], dY)));
    float w1 = max(0.18, 2.4 * pxH);
    float t1 = clamp((ff.hole.x - (r1 - 0.35 * w1)) / w1, 0.0, 1.0);
    float L = lerp(1.0, t1 * t1 * (3.0 - 2.0 * t1), ff.vis.x);
    float2 gL = (ff.vis.x * 6.0 * t1 * (1.0 - t1) / w1) * ff.holeGrad[0];
    float r2 = (0.72 - 0.6 * ce) * ff.var * lerp(1.0, 0.75, fresh);
    float w2 = max(0.2, 2.4 * length(float2(dot(ff.holeGrad[1], dX), dot(ff.holeGrad[1], dY))));
    float M = lerp(1.0, smoothstep(r2 - 0.4 * w2, r2 + 0.6 * w2, ff.hole.y), ff.vis.y);
    float r3 = 0.6 - 0.45 * ce;
    float w3 = max(0.25, 2.4 * length(float2(dot(ff.holeGrad[2], dX), dot(ff.holeGrad[2], dY))));
    float B = lerp(1.0, smoothstep(r3 - 0.4 * w3, r3 + 0.6 * w3, ff.hole.z), ff.vis.z);
    // inside: bubbles of different sizes make the layer thicker and thinner
    float thick = (0.6 + 0.4 * ff.detail) * lerp(0.55, 1.0, fresh);
    float inner = (0.35 + 0.65 * M) * (0.55 + 0.45 * B) * thick * smoothstep(0.0, 0.06, c);
    float h = S * L * inner;
    grad = (gS * L + S * gL) * inner;
    // far away (the pattern below a pixel) the foam is its mean thickness
    grad *= 1.0 - ff.far;
    return lerp(h, 0.55 * c, ff.far);
}
