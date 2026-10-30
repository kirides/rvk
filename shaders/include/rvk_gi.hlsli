// The GI probe volume (M10 stage A, src/enh/gi.*, docs/research/rtgi.md 5), HLSL port of rvk_gi.glsl: camera-centred cascades of DDGI
// probes, each holding its irradiance as L2 spherical harmonics already in the engine's ambient layout (the material shaders'
// g_AmbientLight c15..c21: "stupid SH tricks" polynomial, rtgi.md 1) plus octahedral depth moments for the visibility test. A lookup
// blends the 8 probes around a point (trilinear x backface x Chebyshev visibility) into one set of those 7 registers; the engine's own
// CalcAmbientLightComplex then evaluates them with its normal-mapped normal.
//
// Units: "acc" units of the engine's lighting sum (rtgi.md 1): the outgoing radiance of a white Lambertian surface (irradiance / pi),
// before the material shader's pow(acc, 0.45).
//
// The GLSL original (rvk_gi.glsl) stays the text the D3D9 translator pastes into material pixel shaders; this is its twin for the HLSL
// shaders. The mirror of the header is enh::GiVolumeHeader (static_assert'ed size). Memory is read through the addresses stored in
// the header; every struct is made of float4 / int4 / uint4 only (layout independent of the packing rule).
#pragma once

#include "rvk_common.hlsli"
#include "rvk_oct.hlsli"  // rvkGiOctEncode / rvkGiOctDecode

static const uint RVK_GI_MAX_CASCADES = 4u;
static const uint RVK_GI_DEPTH_RES = 8u;          // depth moments per probe: 8 x 8 octahedral texels
static const uint RVK_GI_PROBE_VALID = 1u;        // RvkGiProbe.key.w: the probe has been traced at least once at its cell
static const uint RVK_GI_PROBE_INSIDE = 2u;       // ... it sees mostly back faces (inside geometry): never used by lookups
static const uint RVK_GI_PROBE_COUNT_SHIFT = 8u;  // ... bits 8-15: updates so far (saturating, the hysteresis ramp); bits 16-23 the
                                                  // smoothed back-face share (0..255, the inside flag's hysteresis)

// one probe (128 bytes): its absolute cell (xyz) + flags (w), the irradiance in the engine's 7 ambient registers
struct RvkGiProbe {
    int4 key;
    float4 reg[7];
};
typedef RVK_REF(RvkGiProbe) RvkGiProbeRef;
// probe `index` of the array at `a` (GLSL: RvkGiProbes(a).p[index]); read only (the lookups)
RvkGiProbeRef rvkGiProbeAt(uint2 a, uint index) { return rvkPtr<RvkGiProbe, 16>(rvkPairAdd(a, index * 128u)); }
// the probes' writer (gi_blend.comp) reaches the array through the pass's own writable pair (shaderRefRw on DX12)
int4 rvkGiProbeKeyRw(uint2 a, uint index) { return rvkRwLoad<int4, 16>(rvkAddrAdd(rvkAddr(a), index * 128u)); }
float4 rvkGiProbeRegRw(uint2 a, uint index, uint k) { return rvkRwLoad<float4, 16>(rvkAddrAdd(rvkAddr(a), index * 128u + 16u + k * 16u)); }
void rvkGiProbeStoreKey(uint2 a, uint index, int4 key) { rvkRwStore<int4, 16>(rvkAddrAdd(rvkAddr(a), index * 128u), key); }
void rvkGiProbeStoreReg(uint2 a, uint index, uint k, float4 v) { rvkRwStore<float4, 16>(rvkAddrAdd(rvkAddr(a), index * 128u + 16u + k * 16u), v); }

// the depth moments (mean, mean^2 of the ray distance, in cells of the probe's cascade, per octahedral texel): float2 m[] at an
// address, element i (GLSL: RvkGiDepth(a).m[i])
float2 rvkGiLoadMoment(RvkAddr depth, uint i) { return rvkRawLoad<float2, 8>(rvkAddrAdd(depth, i * 8u)); }
float2 rvkGiLoadMomentRw(RvkAddr depth, uint i) { return rvkRwLoad<float2, 8>(rvkAddrAdd(depth, i * 8u)); }
void rvkGiStoreMoment(RvkAddr depth, uint i, float2 m) { rvkRwStore<float2, 8>(rvkAddrAdd(depth, i * 8u), m); }

struct RvkGiCascade {
    int4 origin;   // the cell of the grid's minimum corner (probe positions: cell * spacing)
    float4 spacing;   // x spacing (world units), y 1 / spacing, z depth cap (cells), w unused
};
static const uint RVK_GI_FLAG_KEEP_STATIC = 1u;  // RvkGiVolume.info.x bit 0: the draws' own ambient beyond the outdoor sky stays (indoor colour, static lights); bits 8-15 = gi.shadow_fill x 255

// the per-frame header (enh::GiVolumeHeader, 304 bytes)
struct RvkGiVolumeData {
    uint4 dims;     // probes per cascade along x, y, z; w cascades
    uint4 data;     // xy probes address, zw depth moments address
    float4 params;  // x strength (0 = the engine's ambient, 1 = GI), y normal bias, z view bias (cells), w cascade blend band (cells)
    uint4 info;     // x RVK_GI_FLAG_*, y debug view (gi.debug), z probes per cascade, w frame of the last update
    RvkGiCascade cascade[RVK_GI_MAX_CASCADES];
    float4 sky[7];  // the frame's outdoor ambient (g_AmbientLight of the outdoor draws): what the probes' sky stands for
};
// a reference to the header in memory: members are read through `.Get()`
typedef RVK_REF(RvkGiVolumeData) RvkGiVolume;

bool rvkGiValid(RvkFrame f) { return (f.gi.z & RVK_FRAME_GI_VALID) != 0u && any(f.gi.xy != uint2(0u, 0u)); }
RvkGiVolume rvkGiVolume(RvkFrame f) { return rvkPtr<RvkGiVolumeData, 16>(f.gi.xy); }

// the probe slot of an absolute cell inside its cascade (toroidal addressing: the grid scrolls with the camera without
// moving data; the bias keeps the operands of % positive)
uint rvkGiSlot(int3 dims, int3 cell) {
    int3 m = (cell + dims * 65536) % dims;
    return uint((m.y * dims.z + m.z) * dims.x + m.x);
}
// the absolute cell a slot holds for a grid origin
int3 rvkGiCellOfSlot(int3 dims, int3 origin, uint slot) {
    int s = int(slot);
    int3 m = int3(s % dims.x, s / (dims.x * dims.z), (s / dims.x) % dims.z);
    int3 o = (origin + dims * 65536) % dims;
    return origin + (m - o + dims) % dims;
}

// (the octahedral mapping with y as the pole axis: rvk_oct.hlsli)

// the engine's ambient polynomial at normal n (ge_common.fxh CalcAmbientLightComplex; water_rt.comp's shadeHit)
float3 rvkGiEvalRegs(float4 r[7], float3 n) {
    float4 n1 = float4(n, 1.0);
    float4 n2 = n.xyzz * n.yzzx;
    return float3(dot(r[0], n1) + dot(r[3], n2), dot(r[1], n1) + dot(r[4], n2), dot(r[2], n1) + dot(r[5], n2)) +
           r[6].rgb * (n.x * n.x - n.y * n.y);
}
// the average over all normals (the L0 term: what c14 g_IndoorAmbientColor stands for in the shaders' indoor lerp)
float3 rvkGiAverage(float4 r[7]) { return float3(r[0].w + r[3].z / 3.0, r[1].w + r[4].z / 3.0, r[2].w + r[5].z / 3.0); }

// radiance arriving from direction d for an ambient given as the engine's irradiance polynomial (the sky for rays that
// miss): the cosine lobe's convolution undone (L1 x 3/2, L2 x 4); probes that see only sky then give the engine's ambient back
float3 rvkGiSkyRadiance(float4 r[7], float3 d) {
    float3 c7 = float3(r[3].z, r[4].z, r[5].z) / 3.0;
    float3 l0 = float3(r[0].w, r[1].w, r[2].w) + c7;
    float3 l1 = float3(dot(r[0].xyz, d), dot(r[1].xyz, d), dot(r[2].xyz, d));
    float4 d2 = d.xyzz * d.yzzx;
    float3 l2 = float3(dot(r[3], d2), dot(r[4], d2), dot(r[5], d2)) + r[6].rgb * (d.x * d.x - d.y * d.y) - c7;
    return max(l0 + 1.5 * l1 + 4.0 * l2, float3(0.0, 0.0, 0.0));
}

// depth moments of a probe towards dir (bilinear inside the octahedral square, clamped at its edges); `depth` = the moments' address
float2 rvkGiDepthMoments(RvkAddr depth, uint probe, float3 dir) {
    float2 uv = rvkGiOctEncode(dir) * float(RVK_GI_DEPTH_RES) - 0.5;
    float2 f = frac(uv);
    int hiTexel = int(RVK_GI_DEPTH_RES) - 1;
    int2 i0 = clamp(int2(floor(uv)), int2(0, 0), int2(hiTexel, hiTexel));
    int2 i1 = min(i0 + 1, int2(hiTexel, hiTexel));
    uint b = probe * RVK_GI_DEPTH_RES * RVK_GI_DEPTH_RES;
    float2 m00 = rvkGiLoadMoment(depth, b + uint(i0.y) * RVK_GI_DEPTH_RES + uint(i0.x));
    float2 m10 = rvkGiLoadMoment(depth, b + uint(i0.y) * RVK_GI_DEPTH_RES + uint(i1.x));
    float2 m01 = rvkGiLoadMoment(depth, b + uint(i1.y) * RVK_GI_DEPTH_RES + uint(i0.x));
    float2 m11 = rvkGiLoadMoment(depth, b + uint(i1.y) * RVK_GI_DEPTH_RES + uint(i1.x));
    return lerp(lerp(m00, m10, f.x), lerp(m01, m11, f.x), f.y);
}

// The 8 probes of cascade c around P (normal N, V towards the viewer), weighted as in DDGI (Majercik et al. 2019 / 2021):
// trilinear x a wrap-around backface term x Chebyshev visibility from the depth moments, small weights crushed. Returns the
// weight sum (0: no usable probe) with the weighted register sum in `sum`; `edge` = the point's distance to the grid's
// border in cells (cascade blending)
float rvkGiCascadeSample(RvkGiVolume v, uint c, float3 P, float3 N, float3 V, out float4 sum[7], out float edge) {
    for (int k = 0; k < 7; ++k) sum[k] = float4(0.0, 0.0, 0.0, 0.0);
    edge = 0.0;
    RvkGiCascade cs = v.Get().cascade[c];
    float s = cs.spacing.x;
    int3 dims = int3(v.Get().dims.xyz);
    float3 Pb = P + (N * v.Get().params.y + V * v.Get().params.z) * s;
    float3 local = Pb * cs.spacing.y - float3(cs.origin.xyz);
    int3 base = int3(floor(local));
    if (any(base < int3(0, 0, 0)) || any(base > dims - 2)) return 0.0;
    float3 toEdge = min(local, float3(dims - 1) - local);
    edge = min(toEdge.x, min(toEdge.y, toEdge.z));
    float3 a = local - float3(base);
    uint2 probesAddr = v.Get().data.xy;
    RvkAddr depth = rvkAddr(v.Get().data.zw);
    uint first = c * v.Get().info.z;
    float wsum = 0.0;
    for (int i = 0; i < 8; ++i) {
        int3 o = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        int3 cell = cs.origin.xyz + base + o;
        uint index = first + rvkGiSlot(dims, cell);
        RvkGiProbeRef probe = rvkGiProbeAt(probesAddr, index);
        int4 key = probe.Get().key;
        if (any(key.xyz != cell) || (key.w & int(RVK_GI_PROBE_VALID)) == 0 || (key.w & int(RVK_GI_PROBE_INSIDE)) != 0) continue;
        float3 pp = float3(cell) * s;
        float3 toProbe = pp - P;
        float len = length(toProbe);
        float3 dirToProbe = len > 1e-3 ? toProbe / len : N;
        float w = (dot(dirToProbe, N) + 1.0) * 0.5;
        w = w * w + 0.2;
        // visibility: is the (biased) point nearer to the probe than what the probe saw in that direction?
        float3 pb = Pb - pp;
        float dist = length(pb);
        float2 m = rvkGiDepthMoments(depth, index, dist > 1e-3 ? pb / dist : float3(0.0, 1.0, 0.0));
        float dc = dist * cs.spacing.y;
        if (dc > m.x) {
            float variance = abs(m.y - m.x * m.x);
            float t = dc - m.x;
            float cheb = variance / max(variance + t * t, 1e-6);
            w *= max(cheb * cheb * cheb, 0.0);
        }
        w = max(w, 1e-6);
        if (w < 0.2) w *= w * w / 0.04;
        float3 tri = lerp(1.0 - a, a, float3(o));
        w *= tri.x * tri.y * tri.z;
        for (int k = 0; k < 7; ++k) sum[k] += probe.Get().reg[k] * w;
        wsum += w;
    }
    return wsum;
}

// The irradiance at P (normal N, V towards the viewer) in the engine's 7 ambient registers: the finest cascade holding P
// with usable probes, blended into the next one over the border band. False: no cascade has usable probes there.
// `cascadeOut` = the cascade used (debug views)
bool rvkGiLookupVolume(RvkGiVolume v, float3 P, float3 N, float3 V, out float4 regs[7], out uint cascadeOut) {
    for (int k = 0; k < 7; ++k) regs[k] = float4(0.0, 0.0, 0.0, 0.0);
    cascadeOut = 0xFFu;
    uint n = min(v.Get().dims.w, RVK_GI_MAX_CASCADES);
    for (uint c = 0u; c < n; ++c) {
        float4 a[7];
        float edge;
        float w = rvkGiCascadeSample(v, c, P, N, V, a, edge);
        if (w <= 1e-5) continue;
        for (int k = 0; k < 7; ++k) a[k] /= w;
        float t = clamp(edge / max(v.Get().params.w, 1e-3), 0.0, 1.0);
        if (t < 1.0 && c + 1u < n) {
            float4 b[7];
            float edge2;
            float w2 = rvkGiCascadeSample(v, c + 1u, P, N, V, b, edge2);
            if (w2 > 1e-5) {
                for (int k = 0; k < 7; ++k) a[k] = lerp(b[k] / w2, a[k], t);
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
bool rvkGiLookupFast(RvkGiVolume v, float3 P, out float4 regs[7]) {
    for (int k = 0; k < 7; ++k) regs[k] = float4(0.0, 0.0, 0.0, 0.0);
    uint n = min(v.Get().dims.w, RVK_GI_MAX_CASCADES);
    int3 dims = int3(v.Get().dims.xyz);
    uint2 probesAddr = v.Get().data.xy;
    for (uint c = 0u; c < n; ++c) {
        RvkGiCascade cs = v.Get().cascade[c];
        float3 local = P * cs.spacing.y - float3(cs.origin.xyz);
        int3 base = int3(floor(local));
        if (any(base < int3(0, 0, 0)) || any(base > dims - 2)) continue;
        float3 a = local - float3(base);
        float wsum = 0.0;
        uint first = c * v.Get().info.z;
        for (int i = 0; i < 8; ++i) {
            int3 o = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
            int3 cell = cs.origin.xyz + base + o;
            uint index = first + rvkGiSlot(dims, cell);
            RvkGiProbeRef probe = rvkGiProbeAt(probesAddr, index);
            int4 key = probe.Get().key;
            if (any(key.xyz != cell) || (key.w & int(RVK_GI_PROBE_VALID)) == 0 || (key.w & int(RVK_GI_PROBE_INSIDE)) != 0) continue;
            float3 tri = lerp(1.0 - a, a, float3(o));
            float w = tri.x * tri.y * tri.z;
            for (int k = 0; k < 7; ++k) regs[k] += probe.Get().reg[k] * w;
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
bool rvkGiLookupHit(RvkGiVolume v, float3 P, float3 N, out float4 regs[7]) {
    for (int k = 0; k < 7; ++k) regs[k] = float4(0.0, 0.0, 0.0, 0.0);
    uint n = min(v.Get().dims.w, RVK_GI_MAX_CASCADES);
    for (uint c = 0u; c < n; ++c) {
        float4 a[7];
        float edge;
        float w = rvkGiCascadeSample(v, c, P, N, N, a, edge);
        if (w <= 1e-5) continue;
        for (int k = 0; k < 7; ++k) regs[k] = a[k] / w;
        return true;
    }
    return false;
}

bool rvkGiLookup(RvkFrame f, float3 P, float3 N, float3 V, out float4 regs[7], out uint cascadeOut) {
    if (!rvkGiValid(f)) {
        for (int k = 0; k < 7; ++k) regs[k] = float4(0.0, 0.0, 0.0, 0.0);
        cascadeOut = 0xFFu;
        return false;
    }
    return rvkGiLookupVolume(rvkGiVolume(f), P, N, V, regs, cascadeOut);
}
