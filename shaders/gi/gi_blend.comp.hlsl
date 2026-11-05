// GI probe update (M10 stage A, src/enh/gi.cpp, docs/research/rtgi.md 5): one workgroup per probe of the update list folds
// its 64 rays (gi_trace.comp) into the probe: the radiance projected onto L2 spherical harmonics (uniform directions:
// 4 pi / N per ray), convolved with the cosine lobe in the engine's units (irradiance / pi: A0 = 1, A1 = 2/3, A2 = 1/4), stored
// as the engine's 7 ambient registers (CalcAmbientLightComplex's layout, rtgi.md 1) and blended with the probe's history
// (hysteresis; a probe new at its cell takes the rays as they are, then ramps up). The 8 x 8 octahedral depth moments
// (mean, mean^2 of the distance in cells, a cosine-power lobe per texel) feed the lookups' visibility test. A probe that
// sees mostly back faces is inside geometry: flagged, never used by lookups. HLSL port of gi_blend.comp.

#include "rvk_common.hlsli"
#include "rvk_gi.hlsli"
#include "gi_common.hlsli"

// (the writable views of the probe data are the same RvkGiProbeRef / rvkGiStoreMoment of rvk_gi.hlsli)

groupshared float3 sRad[GI_RAYS];
groupshared float4 sDir[GI_RAYS];  // xyz direction, w distance in cells (negative: back face)
groupshared float sCoef[27];       // 9 coefficients x rgb
groupshared uint sBack;
groupshared int4 sKey;

// the real spherical harmonics basis k (0 .. 8) in the engine's order of the projection (l = 0; l = 1: y, z, x; l = 2: xy, yz, z^2, xz, x^2 - y^2)
float giShBasis(uint k, float3 d) {
    switch (k) {
    case 0u: return 0.282095;
    case 1u: return 0.488603 * d.y;
    case 2u: return 0.488603 * d.z;
    case 3u: return 0.488603 * d.x;
    case 4u: return 1.092548 * d.x * d.y;
    case 5u: return 1.092548 * d.y * d.z;
    case 6u: return 0.315392 * (3.0 * d.z * d.z - 1.0);
    case 7u: return 1.092548 * d.x * d.z;
    default: return 0.546274 * (d.x * d.x - d.y * d.y);
    }
}

[numthreads(64, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID) {
    GiPassParams p = giParams();
    uint item = gid.x;
    if (item >= p.Get().volume.z) return;
    uint t = lid.x;
    RvkGiVolume v = rvkPtr<RvkGiVolumeData, 16>(p.Get().volume.xy);
    uint e = giListEntry(p, item);
    uint c = e >> 24;
    uint slot = e & 0xFFFFFFu;
    int3 dims = int3(v.Get().dims.xyz);
    int3 cell = rvkGiCellOfSlot(dims, v.Get().cascade[c].origin.xyz, slot);
    float invS = v.Get().cascade[c].spacing.y;
    float cap = v.Get().cascade[c].spacing.z;
    uint index = c * v.Get().info.z + slot;

    float4 ray = giRayLoad(p, item * GI_RAYS + t);
    sRad[t] = ray.rgb;
    float dc = ray.w < 0.0 ? -min(-ray.w * invS, cap) : min(ray.w * invS, cap);
    sDir[t] = float4(giRayDir(p, t), dc);
    if (t == 0u) {
        sBack = 0u;
        sKey = rvkGiProbeKeyRw(v.Get().data.xy, index);
    }
    GroupMemoryBarrierWithGroupSync();
    if (ray.w < 0.0) {
        uint orig;
        InterlockedAdd(sBack, 1u, orig);
    }

    // SH projection: invocation t < 27 sums coefficient t / 3 of channel t % 3 over the rays
    if (t < 27u) {
        uint k = t / 3u, ch = t % 3u;
        float sum = 0.0;
        for (uint i = 0u; i < GI_RAYS; ++i) sum += sRad[i][ch] * giShBasis(k, sDir[i].xyz);
        // the lights' direct light (gi_trace.comp saw which ones): a delta light of colour c x falloff is the radiance
        // pi c fall delta(w - w_j) (acc = irradiance / pi, the engine's c x N.L), projected like a ray
        float direct = 0.0;
        if ((p.Get().flags.z & 1u) != 0u) {
            uint4 vw = asuint(giRayLoad(p, GI_MAX_PROBE_ITEMS * GI_RAYS + item));
            float3 P = float3(cell) * v.Get().cascade[c].spacing.x;
            uint nL = min(p.Get().flags.y, GI_MAX_LIGHTS);
            for (uint j = 0u; j < nL; ++j) {
                if (((vw[j >> 5u] >> (j & 31u)) & 1u) == 0u) continue;
                float4 lp = giLight(j * 2u);
                float3 d = lp.xyz - P;
                float d2 = dot(d, d);
                float fall = 1.0 - clamp(d2 * lp.w, 0.0, 1.0);
                direct += 3.14159265 * giLight(j * 2u + 1u).rgb[ch] * fall * giShBasis(k, d * rsqrt(max(d2, 1e-4)));
            }
        }
        // 4 pi / N (uniform directions) x the cosine lobe's band factor in acc units (irradiance / pi: A0 = 1, A1 = 2/3, A2 = 1/4)
        float band = k == 0u ? 1.0 : k < 4u ? 2.0 / 3.0 : 0.25;
        sCoef[t] = (sum * (12.566371 / float(GI_RAYS)) + direct) * band;
    }
    GroupMemoryBarrierWithGroupSync();

    bool fresh = any(sKey.xyz != cell) || (sKey.w & int(RVK_GI_PROBE_VALID)) == 0;
    uint count = fresh ? 0u : (uint(sKey.w) >> RVK_GI_PROBE_COUNT_SHIFT) & 0xFFu;
    if (t == 0u) {
        // the engine's layout per channel: (L1x, L1y, L1z, L0 - C7), (C6, C5, 3 C7, C4) for (xy, yz, zz, zx), C8 for x^2 - y^2
        float4 regs[7];
        for (uint ch = 0u; ch < 3u; ++ch) {
            float a0 = sCoef[0u * 3u + ch], a1m1 = sCoef[1u * 3u + ch], a10 = sCoef[2u * 3u + ch], a11 = sCoef[3u * 3u + ch];
            float a2m2 = sCoef[4u * 3u + ch], a2m1 = sCoef[5u * 3u + ch], a20 = sCoef[6u * 3u + ch], a21 = sCoef[7u * 3u + ch];
            float a22 = sCoef[8u * 3u + ch];
            float L0 = 0.282095 * a0;
            float C7 = 0.315392 * a20;
            regs[ch] = float4(0.488603 * a11, 0.488603 * a1m1, 0.488603 * a10, L0 - C7);
            regs[3u + ch] = float4(1.092548 * a2m2, 1.092548 * a2m1, 3.0 * C7, 1.092548 * a21);
            regs[6][ch] = 0.546274 * a22;
        }
        regs[6].w = 0.0;
        const uint2 probes = v.Get().data.xy;  // (the pass's own copy of the header holds writable pairs)
        // adaptive history (rtgi.md 20): the long hysteresis (a steady probe averages many updates: little grain) gives way to
        // a fast blend where the light really changed (a door opened, the sun came out, a lamp lit): the relative change of
        // the probe's average luminance, beyond what 64 rays' noise explains, raises the new rays' weight
        float alpha = 1.0;
        if (!fresh) {
            const float3 lw = float3(0.2126, 0.7152, 0.0722);
            float3 oldL0, newL0;
            for (uint ch = 0u; ch < 3u; ++ch) {
                oldL0[ch] = rvkGiProbeRegRw(probes, index, ch).w + rvkGiProbeRegRw(probes, index, 3u + ch).z / 3.0;
                newL0[ch] = regs[ch].w + regs[3u + ch].z / 3.0;
            }
            float lo = dot(max(oldL0, float3(0.0, 0.0, 0.0)), lw), ln = dot(max(newL0, float3(0.0, 0.0, 0.0)), lw);
            float rel = abs(ln - lo) / (max(ln, lo) + 0.03);
            alpha = lerp(1.0 - p.Get().blend.x, 0.15, smoothstep(0.45, 0.9, rel));
            alpha = max(alpha, 1.0 / float(count + 1u));
        }
        for (int k = 0; k < 7; ++k) {
            float4 old = fresh ? regs[k] : rvkGiProbeRegRw(probes, index, uint(k));
            rvkGiProbeStoreReg(probes, index, uint(k), lerp(old, regs[k], alpha));
        }
        // inside geometry: the back-face share smoothed over the updates (bits 16-23, 0..255) with hysteresis: a probe near
        // a wall whose share hovers around the threshold does not flip between used and ignored with each ray rotation
        float share = float(sBack) / float(GI_RAYS);
        float oldShare = fresh ? share : float((uint(sKey.w) >> 16) & 0xFFu) / 255.0;
        float smoothed = fresh ? share : lerp(oldShare, share, 0.25);
        bool wasInside = !fresh && (sKey.w & int(RVK_GI_PROBE_INSIDE)) != 0;
        bool inside = wasInside ? smoothed > p.Get().blend.w * 0.7 : smoothed > p.Get().blend.w;
        int flags = int(RVK_GI_PROBE_VALID) | (inside ? int(RVK_GI_PROBE_INSIDE) : 0) |
                    int(min(count + 1u, 255u) << RVK_GI_PROBE_COUNT_SHIFT) |
                    int(uint(clamp(smoothed, 0.0, 1.0) * 255.0 + 0.5) << 16);
        rvkGiProbeStoreKey(probes, index, int4(cell, flags));
    }

    // depth moments: invocation t = texel t of the 8 x 8 octahedral map
    float3 texelDir = rvkGiOctDecode((float2(float(t % RVK_GI_DEPTH_RES), float(t / RVK_GI_DEPTH_RES)) + 0.5) / float(RVK_GI_DEPTH_RES));
    float wsum = 0.0, m1 = 0.0, m2 = 0.0;
    for (uint i = 0u; i < GI_RAYS; ++i) {
        float w = pow(max(dot(texelDir, sDir[i].xyz), 0.0), p.Get().blend.z);
        float d = abs(sDir[i].w);
        wsum += w;
        m1 += w * d;
        m2 += w * d * d;
    }
    RvkAddr depth = rvkAddr(v.Get().data.zw);
    uint di = index * RVK_GI_DEPTH_RES * RVK_GI_DEPTH_RES + t;
    float2 moments = wsum > 1e-5 ? float2(m1, m2) / wsum : float2(cap, cap * cap);
    float alphaD = fresh ? 1.0 : max(1.0 - p.Get().blend.y, 1.0 / float(count + 1u));
    float2 old = fresh ? moments : rvkGiLoadMomentRw(depth, di);
    rvkGiStoreMoment(depth, di, lerp(old, moments, alphaD));
}
