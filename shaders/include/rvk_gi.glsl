// The GI probe volume (M10 stage A, src/enh/gi.*, docs/research/rtgi.md 5): camera-centred cascades of DDGI probes, each
// holding its irradiance as L2 spherical harmonics already in the engine's ambient layout (the material shaders' g_AmbientLight
// c15..c21: "stupid SH tricks" polynomial, rtgi.md 1) plus octahedral depth moments for the visibility test. A lookup blends
// the 8 probes around a point (trilinear x backface x Chebyshev visibility) into one set of those 7 registers; the engine's own
// CalcAmbientLightComplex then evaluates them with its normal-mapped normal.
//
// Units: "acc" units of the engine's lighting sum (rtgi.md 1): the outgoing radiance of a white Lambertian surface (irradiance
// / pi), before the material shader's pow(acc, 0.45).
//
// Self-contained (no #include): the D3D9 translator pastes this text into the GI variants of material pixel shaders after
// rvk_abi.glsl (GlslOptions::prelude, embedded at build time); hand-written shaders #include it after rvk_abi.glsl. The mirror
// of the header is enh::GiVolumeHeader (static_assert'ed size).

#ifndef RVK_GI_GLSL
#define RVK_GI_GLSL

#extension GL_EXT_buffer_reference_uvec2 : require

const uint RVK_GI_MAX_CASCADES = 4u;
const uint RVK_GI_DEPTH_RES = 8u;         // depth moments per probe: 8 x 8 octahedral texels
const uint RVK_GI_PROBE_VALID = 1u;       // RvkGiProbe.key.w: the probe has been traced at least once at its cell
const uint RVK_GI_PROBE_INSIDE = 2u;      // ... it sees mostly back faces (inside geometry): never used by lookups
const uint RVK_GI_PROBE_COUNT_SHIFT = 8u; // ... bits 8-15: updates so far (saturating, the hysteresis ramp); bits 16-23 the
                                          // smoothed back-face share (0..255, the inside flag's hysteresis)

// one probe (128 bytes): its absolute cell (xyz) + flags (w), the irradiance in the engine's 7 ambient registers
struct RvkGiProbe {
    ivec4 key;
    vec4 reg[7];
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RvkGiProbes {
    RvkGiProbe p[];
};
// mean, mean^2 of the ray distance (in cells of the probe's cascade) per octahedral texel
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer RvkGiDepth {
    vec2 m[];
};

struct RvkGiCascade {
    ivec4 origin;   // the cell of the grid's minimum corner (probe positions: cell * spacing)
    vec4 spacing;   // x spacing (world units), y 1 / spacing, z depth cap (cells), w unused
};
const uint RVK_GI_FLAG_KEEP_STATIC = 1u;  // RvkGiVolume.info.x: the draws' own ambient beyond the outdoor sky stays (indoor colour, static lights)

// the per-frame header (enh::GiVolumeHeader, 304 bytes)
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RvkGiVolume {
    uvec4 dims;     // probes per cascade along x, y, z; w cascades
    uvec4 data;     // xy probes address, zw depth moments address
    vec4 params;    // x strength (0 = the engine's ambient, 1 = GI), y normal bias, z view bias (cells), w cascade blend band (cells)
    uvec4 info;     // x RVK_GI_FLAG_*, y debug view (gi.debug), z probes per cascade, w frame of the last update
    RvkGiCascade cascade[RVK_GI_MAX_CASCADES];
    vec4 sky[7];    // the frame's outdoor ambient (g_AmbientLight of the outdoor draws): what the probes' sky stands for
};

bool rvkGiValid(RvkFrame f) { return (f.gi.z & RVK_FRAME_GI_VALID) != 0u && f.gi.xy != uvec2(0u); }
RvkGiVolume rvkGiVolume(RvkFrame f) { return RvkGiVolume(f.gi.xy); }

// the probe slot of an absolute cell inside its cascade (toroidal addressing: the grid scrolls with the camera without
// moving data; the bias keeps the operands of % positive)
uint rvkGiSlot(ivec3 dims, ivec3 cell) {
    ivec3 m = (cell + dims * 65536) % dims;
    return uint((m.y * dims.z + m.z) * dims.x + m.x);
}
// the absolute cell a slot holds for a grid origin
ivec3 rvkGiCellOfSlot(ivec3 dims, ivec3 origin, uint slot) {
    int s = int(slot);
    ivec3 m = ivec3(s % dims.x, s / (dims.x * dims.z), (s / dims.x) % dims.z);
    ivec3 o = (origin + dims * 65536) % dims;
    return origin + (m - o + dims) % dims;
}

// octahedral mapping with y as the pole axis
vec2 rvkGiOctEncode(vec3 n) {
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    vec2 o = n.xz;
    if (n.y < 0.0) o = (1.0 - abs(o.yx)) * vec2(o.x >= 0.0 ? 1.0 : -1.0, o.y >= 0.0 ? 1.0 : -1.0);
    return o * 0.5 + 0.5;
}
vec3 rvkGiOctDecode(vec2 uv) {
    vec2 o = uv * 2.0 - 1.0;
    vec3 n = vec3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);
    if (n.y < 0.0) n.xz = (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

// the engine's ambient polynomial at normal n (ge_common.fxh CalcAmbientLightComplex; water_rt.comp's shadeHit)
vec3 rvkGiEvalRegs(vec4 r[7], vec3 n) {
    vec4 n1 = vec4(n, 1.0);
    vec4 n2 = n.xyzz * n.yzzx;
    return vec3(dot(r[0], n1) + dot(r[3], n2), dot(r[1], n1) + dot(r[4], n2), dot(r[2], n1) + dot(r[5], n2)) +
           r[6].rgb * (n.x * n.x - n.y * n.y);
}
// the average over all normals (the L0 term: what c14 g_IndoorAmbientColor stands for in the shaders' indoor lerp)
vec3 rvkGiAverage(vec4 r[7]) { return vec3(r[0].w + r[3].z / 3.0, r[1].w + r[4].z / 3.0, r[2].w + r[5].z / 3.0); }

// radiance arriving from direction d for an ambient given as the engine's irradiance polynomial (the sky for rays that
// miss): the cosine lobe's convolution undone (L1 x 3/2, L2 x 4); probes that see only sky then give the engine's ambient back
vec3 rvkGiSkyRadiance(vec4 r[7], vec3 d) {
    vec3 c7 = vec3(r[3].z, r[4].z, r[5].z) / 3.0;
    vec3 l0 = vec3(r[0].w, r[1].w, r[2].w) + c7;
    vec3 l1 = vec3(dot(r[0].xyz, d), dot(r[1].xyz, d), dot(r[2].xyz, d));
    vec4 d2 = d.xyzz * d.yzzx;
    vec3 l2 = vec3(dot(r[3], d2), dot(r[4], d2), dot(r[5], d2)) + r[6].rgb * (d.x * d.x - d.y * d.y) - c7;
    return max(l0 + 1.5 * l1 + 4.0 * l2, vec3(0.0));
}

// depth moments of a probe towards dir (bilinear inside the octahedral square, clamped at its edges)
vec2 rvkGiDepthMoments(RvkGiDepth depth, uint probe, vec3 dir) {
    vec2 uv = rvkGiOctEncode(dir) * float(RVK_GI_DEPTH_RES) - 0.5;
    vec2 f = fract(uv);
    ivec2 i0 = clamp(ivec2(floor(uv)), ivec2(0), ivec2(int(RVK_GI_DEPTH_RES) - 1));
    ivec2 i1 = min(i0 + 1, ivec2(int(RVK_GI_DEPTH_RES) - 1));
    uint b = probe * RVK_GI_DEPTH_RES * RVK_GI_DEPTH_RES;
    vec2 m00 = depth.m[b + uint(i0.y) * RVK_GI_DEPTH_RES + uint(i0.x)];
    vec2 m10 = depth.m[b + uint(i0.y) * RVK_GI_DEPTH_RES + uint(i1.x)];
    vec2 m01 = depth.m[b + uint(i1.y) * RVK_GI_DEPTH_RES + uint(i0.x)];
    vec2 m11 = depth.m[b + uint(i1.y) * RVK_GI_DEPTH_RES + uint(i1.x)];
    return mix(mix(m00, m10, f.x), mix(m01, m11, f.x), f.y);
}

// The 8 probes of cascade c around P (normal N, V towards the viewer), weighted as in DDGI (Majercik et al. 2019 / 2021):
// trilinear x a wrap-around backface term x Chebyshev visibility from the depth moments, small weights crushed. Returns the
// weight sum (0: no usable probe) with the weighted register sum in `sum`; `edge` = the point's distance to the grid's
// border in cells (cascade blending)
float rvkGiCascadeSample(RvkGiVolume v, uint c, vec3 P, vec3 N, vec3 V, out vec4 sum[7], out float edge) {
    for (int k = 0; k < 7; ++k) sum[k] = vec4(0.0);
    edge = 0.0;
    RvkGiCascade cs = v.cascade[c];
    float s = cs.spacing.x;
    ivec3 dims = ivec3(v.dims.xyz);
    vec3 Pb = P + (N * v.params.y + V * v.params.z) * s;
    vec3 local = Pb * cs.spacing.y - vec3(cs.origin.xyz);
    ivec3 base = ivec3(floor(local));
    if (any(lessThan(base, ivec3(0))) || any(greaterThan(base, dims - 2))) return 0.0;
    vec3 toEdge = min(local, vec3(dims - 1) - local);
    edge = min(toEdge.x, min(toEdge.y, toEdge.z));
    vec3 a = local - vec3(base);
    RvkGiProbes probes = RvkGiProbes(v.data.xy);
    RvkGiDepth depth = RvkGiDepth(v.data.zw);
    uint first = c * v.info.z;
    float wsum = 0.0;
    for (int i = 0; i < 8; ++i) {
        ivec3 o = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        ivec3 cell = cs.origin.xyz + base + o;
        uint index = first + rvkGiSlot(dims, cell);
        ivec4 key = probes.p[index].key;
        if (key.xyz != cell || (key.w & int(RVK_GI_PROBE_VALID)) == 0 || (key.w & int(RVK_GI_PROBE_INSIDE)) != 0) continue;
        vec3 pp = vec3(cell) * s;
        vec3 toProbe = pp - P;
        float len = length(toProbe);
        vec3 dirToProbe = len > 1e-3 ? toProbe / len : N;
        float w = (dot(dirToProbe, N) + 1.0) * 0.5;
        w = w * w + 0.2;
        // visibility: is the (biased) point nearer to the probe than what the probe saw in that direction?
        vec3 pb = Pb - pp;
        float dist = length(pb);
        vec2 m = rvkGiDepthMoments(depth, index, dist > 1e-3 ? pb / dist : vec3(0.0, 1.0, 0.0));
        float dc = dist * cs.spacing.y;
        if (dc > m.x) {
            float variance = abs(m.y - m.x * m.x);
            float t = dc - m.x;
            float cheb = variance / max(variance + t * t, 1e-6);
            w *= max(cheb * cheb * cheb, 0.0);
        }
        w = max(w, 1e-6);
        if (w < 0.2) w *= w * w / 0.04;
        vec3 tri = mix(1.0 - a, a, vec3(o));
        w *= tri.x * tri.y * tri.z;
        for (int k = 0; k < 7; ++k) sum[k] += probes.p[index].reg[k] * w;
        wsum += w;
    }
    return wsum;
}

// The irradiance at P (normal N, V towards the viewer) in the engine's 7 ambient registers: the finest cascade holding P
// with usable probes, blended into the next one over the border band. False: no cascade has usable probes there.
// `cascadeOut` = the cascade used (debug views)
bool rvkGiLookupVolume(RvkGiVolume v, vec3 P, vec3 N, vec3 V, out vec4 regs[7], out uint cascadeOut) {
    for (int k = 0; k < 7; ++k) regs[k] = vec4(0.0);
    cascadeOut = 0xFFu;
    uint n = min(v.dims.w, RVK_GI_MAX_CASCADES);
    for (uint c = 0u; c < n; ++c) {
        vec4 a[7];
        float edge;
        float w = rvkGiCascadeSample(v, c, P, N, V, a, edge);
        if (w <= 1e-5) continue;
        for (int k = 0; k < 7; ++k) a[k] /= w;
        float t = clamp(edge / max(v.params.w, 1e-3), 0.0, 1.0);
        if (t < 1.0 && c + 1u < n) {
            vec4 b[7];
            float edge2;
            float w2 = rvkGiCascadeSample(v, c + 1u, P, N, V, b, edge2);
            if (w2 > 1e-5) {
                for (int k = 0; k < 7; ++k) a[k] = mix(b[k] / w2, a[k], t);
            }
        }
        for (int k = 0; k < 7; ++k) regs[k] = a[k];
        cascadeOut = c;
        return true;
    }
    return false;
}

// A cheap lookup for ray hits (the probes' own multi-bounce, the screen probes' hits): the finest cascade holding P, its 8
// probes trilinear x valid only (no backface / visibility weights: a hit's second bounce tolerates leaks)
bool rvkGiLookupFast(RvkGiVolume v, vec3 P, out vec4 regs[7]) {
    for (int k = 0; k < 7; ++k) regs[k] = vec4(0.0);
    uint n = min(v.dims.w, RVK_GI_MAX_CASCADES);
    ivec3 dims = ivec3(v.dims.xyz);
    RvkGiProbes probes = RvkGiProbes(v.data.xy);
    for (uint c = 0u; c < n; ++c) {
        RvkGiCascade cs = v.cascade[c];
        vec3 local = P * cs.spacing.y - vec3(cs.origin.xyz);
        ivec3 base = ivec3(floor(local));
        if (any(lessThan(base, ivec3(0))) || any(greaterThan(base, dims - 2))) continue;
        vec3 a = local - vec3(base);
        float wsum = 0.0;
        uint first = c * v.info.z;
        for (int i = 0; i < 8; ++i) {
            ivec3 o = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
            ivec3 cell = cs.origin.xyz + base + o;
            uint index = first + rvkGiSlot(dims, cell);
            ivec4 key = probes.p[index].key;
            if (key.xyz != cell || (key.w & int(RVK_GI_PROBE_VALID)) == 0 || (key.w & int(RVK_GI_PROBE_INSIDE)) != 0) continue;
            vec3 tri = mix(1.0 - a, a, vec3(o));
            float w = tri.x * tri.y * tri.z;
            for (int k = 0; k < 7; ++k) regs[k] += probes.p[index].reg[k] * w;
            wsum += w;
        }
        if (wsum <= 1e-4) continue;
        for (int k = 0; k < 7; ++k) regs[k] /= wsum;
        return true;
    }
    return false;
}

// The lookup of a probe ray's hit (the DDGI's own multi-bounce, rtgi.md 16): the finest cascade holding P, its 8 probes
// weighted as in rvkGiCascadeSample (trilinear x wrap-around backface x Chebyshev visibility) without the view bias and the
// cascade blend: a probe behind a wall from the hit gets no weight, so the second bounce does not leak through thin walls
bool rvkGiLookupHit(RvkGiVolume v, vec3 P, vec3 N, out vec4 regs[7]) {
    for (int k = 0; k < 7; ++k) regs[k] = vec4(0.0);
    uint n = min(v.dims.w, RVK_GI_MAX_CASCADES);
    for (uint c = 0u; c < n; ++c) {
        vec4 a[7];
        float edge;
        float w = rvkGiCascadeSample(v, c, P, N, N, a, edge);
        if (w <= 1e-5) continue;
        for (int k = 0; k < 7; ++k) regs[k] = a[k] / w;
        return true;
    }
    return false;
}

bool rvkGiLookup(RvkFrame f, vec3 P, vec3 N, vec3 V, out vec4 regs[7], out uint cascadeOut) {
    if (!rvkGiValid(f)) {
        for (int k = 0; k < 7; ++k) regs[k] = vec4(0.0);
        cascadeOut = 0xFFu;
        return false;
    }
    return rvkGiLookupVolume(rvkGiVolume(f), P, N, V, regs, cascadeOut);
}

#endif
