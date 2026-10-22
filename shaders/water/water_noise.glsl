// Noise and the foam pattern of the enhanced water (docs/research/ocean.md 4.4). No bindings: included by water_common.glsl
// (the water draws) and by the FFT ocean's foam pass (ocean_foam.comp).

#ifndef WATER_NOISE_GLSL
#define WATER_NOISE_GLSL

// ---- gradient noise with analytic derivatives (after Inigo Quilez, MIT) ---------------------------------------------------

vec2 waterHash(vec2 p) {
    p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
    return -1.0 + 2.0 * fract(sin(p) * 43758.5453123);
}

// value, d/dx, d/dy
vec3 waterNoised(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    vec2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
    vec2 ga = waterHash(i + vec2(0.0, 0.0));
    vec2 gb = waterHash(i + vec2(1.0, 0.0));
    vec2 gc = waterHash(i + vec2(0.0, 1.0));
    vec2 gd = waterHash(i + vec2(1.0, 1.0));
    float va = dot(ga, f - vec2(0.0, 0.0));
    float vb = dot(gb, f - vec2(1.0, 0.0));
    float vc = dot(gc, f - vec2(0.0, 1.0));
    float vd = dot(gd, f - vec2(1.0, 1.0));
    return vec3(va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * (va - vb - vc + vd),
                ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd) +
                    du * (u.yx * (va - vb - vc + vd) + vec2(vb, vc) - va));
}

// value only
float waterNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float va = dot(waterHash(i), f);
    float vb = dot(waterHash(i + vec2(1.0, 0.0)), f - vec2(1.0, 0.0));
    float vc = dot(waterHash(i + vec2(0.0, 1.0)), f - vec2(0.0, 1.0));
    float vd = dot(waterHash(i + vec2(1.0, 1.0)), f - vec2(1.0, 1.0));
    return va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * (va - vb - vc + vd);
}

// cellular noise (Worley): distances to the nearest and second-nearest feature point (cells of size 1)
vec2 waterWorley(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float f1 = 8.0, f2 = 8.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 g = vec2(float(x), float(y));
            vec2 o = waterHash(i + g) * 0.45 + 0.5;
            vec2 r = g + o - f;
            float d = dot(r, r);
            if (d < f1) {
                f2 = f1;
                f1 = d;
            } else if (d < f2) {
                f2 = d;
            }
        }
    }
    return sqrt(vec2(f1, f2));
}

// the nearest feature point only
float waterWorleyF1(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float f1 = 8.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 g = vec2(float(x), float(y));
            vec2 r = g + waterHash(i + g) * 0.45 + 0.5 - f;
            f1 = min(f1, dot(r, r));
        }
    }
    return sqrt(f1);
}

// fractal sum of `octaves` noise octaves starting at `scale` world units, normalised (std ~0.1); octaves smaller than a few
// pixels (footprint = world units per pixel) fade out (no sparkle at distance)
float waterFbm(vec2 xz, float footprint, float scale, int octaves, float gain) {
    float s = 0.0, a = 1.0, n = 0.0;
    for (int i = 0; i < octaves; ++i) {
        float vis = smoothstep(2.0, 5.0, scale / footprint);
        s += a * vis * waterNoise(xz / scale + vec2(3.1, -1.7) * float(i));
        n += a;
        a *= gain;
        scale /= 2.1;
    }
    return s / n;
}

// ---- foam pattern ----------------------------------------------------------------------------------------------------------
// Sea foam is a sheet of bubbles torn open by round holes of many sizes: dense white water right after a wave broke (few,
// small holes), lacy residual foam as it decays (the holes grow and merge until only a lace between them is left). The
// pattern: a warped fractal sheet (5 octaves) thresholded by the coverage, with round holes of three sizes (cellular
// distance to scattered points: round, not a net) punched in, larger at low coverage. Attached to the water (the undisplaced
// position), evolving slowly. (The earlier patterns, cell borders and ridged-noise filaments, read as a hexagon net and as
// cracks / lightning.)

struct FoamField {
    float v;        // the sheet: ~uniform 0..1 (foam where v > 1 - coverage)
    vec3 hole;      // nearest-point distances of the three hole sizes (cells of 42 / 15 / 6 units)
    vec3 vis;       // visibility of each hole size at this footprint
    float var;      // slow variation of the hole sizes
    float far;      // 0 .. 1: the pattern is below a pixel, the foam is its mean coverage
};

FoamField foamField(vec2 xz, float t, float footprint) {
    FoamField ff;
    footprint = max(footprint, 1e-3);
    ff.far = smoothstep(25.0, 80.0, footprint);
    vec2 drift = vec2(0.6, -0.4) * t;
    vec2 w = vec2(waterFbm(xz + vec2(13.0, 0.0) + drift, footprint, 260.0, 3, 0.5),
                  waterFbm(xz + vec2(0.0, 71.0) - drift.yx, footprint, 260.0, 3, 0.5));
    vec2 p = xz + w * 90.0;
    // the sheet's fbm (std ~0.1) through a logistic: about uniform, so the coverage is the share of foam
    ff.v = 1.0 / (1.0 + exp(-17.0 * waterFbm(p, footprint, 130.0, 5, 0.6)));
    ff.var = clamp(0.55 + 1.35 * waterFbm(p + vec2(300.0, 0.0), footprint, 90.0, 2, 0.5) * 3.0, 0.2, 1.3);
    const vec3 SIZE = vec3(42.0, 15.0, 6.0);
    // the holes' own small warp: torn, irregular openings rather than circles (seen from close by)
    vec2 hw = vec2(waterNoise(p / 19.0 + vec2(2.3, 7.7)), waterNoise(p / 19.0 + vec2(9.1, 1.4))) * smoothstep(2.0, 5.0, 19.0 / footprint);
    vec2 ph = p + hw * 9.0;
    for (int i = 0; i < 3; ++i) {
        ff.vis[i] = smoothstep(2.0, 5.0, SIZE[i] / footprint);
        ff.hole[i] = ff.vis[i] > 0.0 ? waterWorleyF1(ph / SIZE[i] + vec2(17.3, 4.1) * float(i)) : 1.0;
    }
    return ff;
}

// foam (0..1) of the pattern at coverage c (0..1)
float foamAmount(FoamField ff, float c) {
    c = clamp(c, 0.0, 1.0);
    if (c <= 0.0) return 0.0;
    float sc = min(c * 1.2, 1.0);
    // soft towards its edges: thin foam is translucent, the thick middle of a patch opaque
    float sheet = smoothstep(1.0 - sc, 1.0 - sc + 0.12, ff.v);
    float r = (0.72 - 0.62 * c) * ff.var;
    const vec3 W = vec3(1.0, 0.9, 0.6);
    float holes = 1.0;
    for (int i = 0; i < 3; ++i) holes *= 1.0 - ff.vis[i] * W[i] * (1.0 - smoothstep(r - 0.1, r, ff.hole[i]));
    // far away (holes and sheet below a pixel) the foam fades to its mean coverage
    return mix(sheet * holes, 0.75 * c, ff.far);
}

#endif
