// Screen probes (M10 stage C P8, src/enh/gi.cpp, docs/research/rtgi.md 11; after GI-1.0, Boisse et al. 2023): one probe per
// screen tile at a jittered pixel of this frame's G-buffer, cosine-weighted hemisphere rays around its normal against the TLAS,
// shaded like the DDGI probes' rays (gi_shade.hlsli: sun + a point light with shadow rays, the DDGI probes at hits, the sky on
// misses). The probe stores the (luminance) ratio of its traced irradiance to the DDGI volume's irradiance at the same point
// (plus a chroma ratio for colour bleeding, gi.bleed) and normal: the material shaders scale the DDGI lookup by it (the exact per-surface visibility and near bounce the 1.5 m
// probe grid cannot hold; the DDGI keeps the direction-dependent SH). rgb = the ratio, a = the probe's view depth (0 = none).
//
// Runs right after the z pre-pass (this frame's depth, the TLAS built there): the material shaders of this frame's solid pass
// read the result at their own pixel (no reprojection lag). The probe's normal: from the depth.
//
// One ray per invocation (64 per workgroup = 64 / rays probes; a thread per probe looping over its rays left the GPU with a
// few hundred long waves: 28 ms in a dense town), the rays of a probe summed in shared memory. HLSL port of sp_trace.comp.

#include "rvk_common.hlsli"
#include "rvk_rt_trace.hlsli"
#include "rvk_gi.hlsli"
#include "gi_common.hlsli"
#include "gi_shade.hlsli"
#include "screen_common.hlsli"

[[vk::image_format("rgba16f")]] RWTexture2D<float4> outProbes : register(u0, space1);   // luminance ratio, chroma red / blue, view depth
[[vk::image_format("rgba16f")]] RWTexture2D<float4> outProbes2 : register(u1, space1);  // directional correction (xyz)
[[vk::image_format("rgba32f")]] RWTexture2D<float4> outGeom : register(u2, space1);     // position, packed normal

groupshared float3 sRad[64];
groupshared float3 sDir[64];

// diagnostics (RVK_GI_DIAG, rtgi.md 23): fixed-point sums of the probes' light terms, per sun class (0 shaded, 1 lit);
// GLSL: GiDiag(p.screen5.yz).v[i], a uint array at an address (element i = a BufferPointer<uint, 4>)
groupshared float4 sD0[64];  // luminance of: hit sun, hit point, hit bounce, hit emission
groupshared float4 sD1[64];  // ray type (0 far / sky, 1 hit, 2 back face), hit distance, albedo (linear) luminance, the engine's sky radiance along the ray
groupshared float4 sD2[64];  // far field towards +dir, 0, 0, 0

[numthreads(64, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID) {
    GiPassParams p = giParams();
    RvkFrame f = rvkLoadFrame(pc.frame);
    uint rays = clamp(p.Get().screen2.y, 1u, 64u);
    uint perGroup = 64u / rays;
    uint lane = lid.x;
    uint local = lane / rays;  // the probe within the group
    uint ray = lane % rays;
    uint probe = gid.x * perGroup + local;
    uint gw = p.Get().screen.z, gh = p.Get().screen.w;
    bool inRange = local < perGroup && probe < gw * gh;
    int2 tile = int2(int(probe % max(gw, 1u)), int(probe / max(gw, 1u)));
    float2 size = float2(p.Get().screen2.zw);

    // the probe's surface (every lane of the probe reads the same texels)
    bool valid = false;
    float3 P = float3(0.0, 0.0, 0.0), N = float3(0.0, 1.0, 0.0), V = float3(0.0, 1.0, 0.0);
    float viewZ = 0.0;
    float z = 1.0;
    int2 px = int2(0, 0);
    if (inRange) {
        px = spProbePixel(p, tile);
        z = rvkHwDepthToD3D(f, rvkFetch2D(p.Get().screen.x, px).r);
        viewZ = spViewDepth(f, z);
        if (viewZ > 0.0) {
            P = spWorldPos(f, px, size, z);
            N = spDepthNormal(f, px, size, p.Get().screen.x);
            V = normalize(f.cameraPos.xyz - P);
            valid = true;
        }
    }
    RvkGiVolume v = rvkPtr<RvkGiVolumeData, 16>(p.Get().volume.xy);
    float3 radiance = float3(0.0, 0.0, 0.0);
    float3 dirOut = N;
    if (valid) {
        // cosine-weighted directions from a rank-1 lattice (u1 stratified, u2 a golden-ratio sequence) shifted per probe and frame:
        // the shift is interleaved-gradient noise of the tile (neighbouring probes get unrelated sets: the resolve's filter over
        // their 3 x 3 averages the error) plus an R2 step per frame (the frames together cover the hemisphere evenly)
        float3 t = normalize(abs(N.y) < 0.99 ? cross(N, float3(0.0, 1.0, 0.0)) : cross(N, float3(1.0, 0.0, 0.0)));
        float3 b = cross(N, t);
        float2 tf = float2(tile);
        float fr = float(p.Get().volume.w & 4095u);
        float2 shift = frac(float2(frac(52.9829189 * frac(dot(tf, float2(0.06711056, 0.00583715)))),
                                   frac(52.9829189 * frac(dot(tf + 41.0, float2(0.00583715, 0.06711056))))) +
                            fr * float2(0.7548776662, 0.5698402909));
        float u1 = frac((float(ray) + 0.5) / float(rays) + shift.x);
        float u2 = frac(float(ray) * 0.6180339887 + shift.y);
        float r = sqrt(u1), phi = 6.2831853 * u2;
        float3 dir = normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + N * sqrt(max(0.0, 1.0 - u1)));
        // the point-light pick of the hit: stratified over the probe's rays as well (rtgi.md 20)
        float u = frac(float(ray) * 0.5698402909 + float(giHash(uint(tile.x) * 9781u + uint(tile.y) * 6271u + p.Get().volume.w * 26699u) & 0xFFFFFFu) / 16777216.0);
        dirOut = dir;
        gGiTwoSided = true;  // the probe starts on a visible surface: a back face is a surface seen from behind (rtgi.md 23)
        float4 hit = giShadeRay(p, f, v, P + N * 3.0 + V * 2.0, dir, p.Get().screen4.x, u);
        radiance = hit.rgb;
        const float3 lw0 = float3(0.2126, 0.7152, 0.0722);
        float4 d2 = float4(0.0, 0.0, 0.0, 0.0);
        float4 d1 = float4(hit.w < 0.0 ? 2.0 : 1.0, hit.w, dot(gDiagLin, lw0), dot(rvkGiSkyRadiance(p.Get().sky, dir), lw0));
        // a ray that ran out of reach without a hit: the probe volume's light at its end (the far field) instead of the sky
        if (hit.w >= p.Get().screen4.x) {
            float4 regs[7];
            d1.x = 0.0;
            if (rvkGiLookupFast(v, P + dir * p.Get().screen4.x, regs)) {
                d2.x = dot(max(rvkGiEvalRegs(regs, dir), float3(0.0, 0.0, 0.0)), lw0);
                float3 deconv = rvkGiSkyRadiance(regs, dir);
                d2.y = dot(deconv, lw0);
                // the radiance arriving from beyond the reach is what the probe at the ray's end sees looking along `dir`: the
                // probe's irradiance SH with the cosine convolution undone (as for the engine's sky), not its irradiance towards
                // -dir (the light of a surface facing us: for a ray into the open sky the dim ground light). rtgi.md 23
                radiance = deconv;
            }
        }
        sD0[lane] = float4(dot(gDiagSun, lw0), dot(gDiagPoint, lw0), dot(gDiagBounce, lw0), dot(gDiagEmis, lw0));
        sD1[lane] = d1;
        sD2[lane] = d2;
    }
    sRad[lane] = radiance;
    sDir[lane] = dirOut;
    GroupMemoryBarrierWithGroupSync();
    if (!inRange || ray != 0u) return;
    if (!valid) {
        outProbes[tile] = float4(1.0, 1.0, 1.0, 0.0);
        outProbes2[tile] = float4(0.0, 0.0, 0.0, 0.0);
        outGeom[tile] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    outGeom[tile] = float4(P, spPackNormal(N));
    // the DDGI's irradiance here (the denominator)
    float4 regs[7];
    uint hc;
    if (!rvkGiLookupVolume(v, P, N, V, regs, hc)) {
        outProbes[tile] = float4(1.0, 1.0, 1.0, 0.0);
        outProbes2[tile] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    float3 ddgi = max(rvkGiEvalRegs(regs, N), float3(0.0, 0.0, 0.0));
    const float3 lw = float3(0.2126, 0.7152, 0.0722);
    float dl = dot(ddgi, lw);
    // fireflies (rtgi.md 20): a ray's radiance is capped relative to the DDGI's light here (a sunlit wall seen from shade is
    // a few times the ambient; far beyond that is one lucky light sample or a bright cut-out, which the average would carry
    // for the next frames)
    float cap = 10.0 * dl + 0.3;
    float3 traced = float3(0.0, 0.0, 0.0);
    // the tangential gradient of the traced irradiance (the cosine-weighted estimate of (1/pi) integral L w_tangent dw, the
    // weight 1 / cos kept at cos >= 0.3: a little biased towards no correction, never wild near the horizon)
    float3 flow = float3(0.0, 0.0, 0.0);
    for (uint i = 0u; i < rays; ++i) {
        float3 rad = sRad[local * rays + i];
        float rl = dot(rad, lw);
        if (rl > cap) {
            rad *= cap / rl;
            rl = cap;
        }
        traced += rad;
        float3 d = sDir[local * rays + i];
        float c = dot(d, N);
        flow += rl * (d - N * c) / max(c, 0.3);
    }
    traced /= float(rays);
    flow /= float(rays);
    if (p.Get().screen5.w != 0u) {
        // diagnostics: per probe the averages over its rays, into the class of the probe's sun term (shaded: the sun's shadow ray
        // is blocked or the surface faces away). Fixed point 1 / 1024, wrapping counters read as deltas by the host
        uint64_t dg = rvkAddr(p.Get().screen5.yz);
        float vis = 0.0;
        float nlp = dot(N, p.Get().sunDir.xyz);
        if (nlp > 0.05 && dot(p.Get().sunColor.rgb, float3(1.0, 1.0, 1.0)) > 1e-3) vis = rvkRtVisibility(f, P + N * 3.0, p.Get().sunDir.xyz, 1.0, p.Get().shade.z, 0.0, 0.0, RVK_RT_TRACE_OPAQUE);
        uint b = vis > 0.5 ? 32u : 0u;
        float inv = 1.0 / float(rays);
        float4 a0 = float4(0.0, 0.0, 0.0, 0.0), a1 = float4(0.0, 0.0, 0.0, 0.0);
        float raw = 0.0, nFar = 0.0, nHit = 0.0, nBack = 0.0, nNear = 0.0, nMid = 0.0, alb = 0.0, skyAll = 0.0, hitRad = 0.0, farRad = 0.0;
        for (uint i = 0u; i < rays; ++i) {
            float4 q0 = sD0[local * rays + i], q1 = sD1[local * rays + i], q2 = sD2[local * rays + i];
            float rl = dot(sRad[local * rays + i], lw);
            raw += rl;
            skyAll += q1.w;
            if (q1.x < 0.5) {
                nFar += 1.0;
                farRad += rl;     // far field used
                a1.y += q1.w;     // the engine's sky along the same rays
                a1.z += q2.x;     // far field evaluated towards +dir
                a1.w += q2.y;     // far field as radiance (the SH's cosine convolution undone) towards +dir
            } else if (q1.x < 1.5) {
                nHit += 1.0;
                a0 += q0;
                alb += q1.z;
                hitRad += rl;
                if (q1.y < 200.0) nNear += 1.0;
                if (q1.y < 1000.0) nMid += 1.0;
            } else {
                nBack += 1.0;
            }
        }
        const float S = 1024.0 * inv;
        float val[21];
        val[20] = a1.w * S;
        val[0] = 1024.0;
        val[1] = dot(max(rvkGiEvalRegs(p.Get().sky, N), float3(0.0, 0.0, 0.0)), lw) * 1024.0;
        val[2] = dl * 1024.0;
        val[3] = dot(traced, lw) * 1024.0;
        val[4] = raw * S;
        val[5] = farRad * S;
        val[6] = a0.x * S;
        val[7] = a0.y * S;
        val[8] = a0.z * S;
        val[9] = a0.w * S;
        val[10] = nFar * S;
        val[11] = nHit * S;
        val[12] = nBack * S;
        val[13] = nNear * S;
        val[14] = nMid * S;
        val[15] = a1.y * S;
        val[16] = a1.z * S;
        val[17] = skyAll * S;
        val[18] = alb * S;
        val[19] = hitRad * S;
        for (uint i = 0u; i < 21u; ++i) {
            vk::BufferPointer<uint, 4> counter = vk::BufferPointer<uint, 4>(dg + uint64_t(b + i) * 4ull);
            uint orig;
            InterlockedAdd(counter.Get(), uint(clamp(val[i], 0.0, 4.0e6)), orig);
        }
    }
    // a luminance ratio (per colour channel the noise of few rays turns into colour noise: measured, grain 3.7 -> 4.3); where
    // the DDGI has almost nothing (deep interiors) it is meaningless: neutral
    float tl = dot(traced, lw);
    float k = dl > 2e-3 ? clamp(tl / dl, 0.0, p.Get().screen4.y) : 1.0;
    if (isnan(k) || isinf(k)) k = 1.0;
    // colour bleeding (gi.bleed, rtgi.md 15): the traced light's chromaticity relative to the DDGI's, a unit-luminance colour
    // ratio kept within 0.6 .. 1.6 and blended by the setting; the resolve filters it wider and longer than the luminance
    float3 chroma = float3(1.0, 1.0, 1.0);
    if (dl > 2e-3 && tl > 2e-3 && p.Get().screen4.w > 0.0) {
        chroma = clamp((traced / tl) / max(ddgi / dl, float3(1e-3, 1e-3, 1e-3)), float3(0.6, 0.6, 0.6), float3(1.6, 1.6, 1.6));
        chroma = lerp(float3(1.0, 1.0, 1.0), chroma / dot(chroma, lw), p.Get().screen4.w);
        if (any(isnan(chroma)) || any(isinf(chroma))) chroma = float3(1.0, 1.0, 1.0);
    }
    // rgb = (luminance ratio, chroma red, chroma blue); green follows from the unit luminance
    outProbes[tile] = float4(k, chroma.r, chroma.b, viewZ);
    // directional correction (rtgi.md 20): how the traced irradiance changes with a tilt of the normal minus how the DDGI's
    // (scaled by the ratio) does; the material shaders add this slope to their DDGI registers, so normal-mapped surfaces and
    // curved ones follow the traced light's direction. A magnitude beyond 0.6 x the irradiance is noise
    float3 corr = float3(0.0, 0.0, 0.0);
    if (dl > 2e-3) {
        float3 gd = float3(0.0, 0.0, 0.0);
        for (int ch = 0; ch < 3; ++ch) gd += lw[ch] * spGradRegs(regs, N, ch);
        gd -= N * dot(gd, N);
        corr = flow - k * gd;
        float lim = 0.6 * max(tl, 0.0);
        float len = length(corr);
        if (len > lim && len > 1e-6) corr *= lim / len;
        if (any(isnan(corr)) || any(isinf(corr))) corr = float3(0.0, 0.0, 0.0);
    }
    outProbes2[tile] = float4(corr, 1.0);
}
