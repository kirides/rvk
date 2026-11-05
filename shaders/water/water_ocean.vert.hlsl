// The enhanced open sea (src/enh/water.cpp): instead of the engine's flat sea plane (Levelmesh_Oceanmesh_01, a coarse mesh
// spanning the world) a camera-centred clipmap grid at the sea's height is drawn and displaced by waves. No vertex buffer:
// gl_VertexIndex -> level, quad, corner. Each level is N x N quads of spacing base * 2^level, snapped to twice its spacing
// (vertices stay fixed in the world while the camera moves); level > 0 leaves out the N/2 x N/2 quads the finer level
// covers; vertices on a level's outer edge between two vertices of the coarser level take their average (no cracks).
// Waves (water.ocean): the FFT ocean's cascades (ocean_surface.hlsli: attenuated by the sea floor field towards the shore and
// inside the harbour, + a wave train rolling in along the travel time to the shore) or the M7 Gerstner sum (damped at the
// coast overlay, shore waves inside the coast band); both capped near the camera at the water's level. At the coast the
// waterline sits lower than the engine's and swashes up the beach. The outputs are the engine's ocean VS's (vs 14c1f115c1e58f30: TEXCOORD0/1 = the sea mesh's texture coordinates
// through its transforms vs c232..c237, TEXCOORD3 world + global alpha, TEXCOORD4..6 normal / tangent / binormal), so
// water.frag shades the grid with the engine's ocean material; TEXCOORD7 carries the grid's own values.

#include "water_common.hlsli"

struct WaterVsOut {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 oTc0 : VARY0;
    [[vk::location(1)]] float4 oTc1 : VARY1;
#ifndef __spirv__
    [[vk::location(2)]] float4 oPad2 : VARY2;  // D3D12 links by register: the signature is dense
#endif
    [[vk::location(3)]] float4 oTc3 : VARY3;   // world position, w global alpha
    [[vk::location(4)]] float4 oTc4 : VARY4;   // wave normal, w outdoor factor
    [[vk::location(5)]] float4 oTc5 : VARY5;   // tangent (recomputed in the PS), w wave amplitude (FFT: height over the mean / Hs/2)
    [[vk::location(6)]] float4 oTc6 : VARY6;   // binormal (recomputed in the PS), w crest steepness
    [[vk::location(7)]] float4 oTc7 : VARY7;   // undisplaced xz, distance to the coast (FFT: signed, to the waterline), shore-wave weight
#ifndef __spirv__
    [[vk::location(8)]] float4 oPad8 : VARY8;
    [[vk::location(9)]] float4 oPad9 : VARY9;
    [[vk::location(10)]] float4 oPad10 : VARY10;
    [[vk::location(11)]] float4 oPad11 : VARY11;
    [[vk::location(12)]] float4 oPad12 : VARY12;
    [[vk::location(13)]] float4 oPad13 : VARY13;
    [[vk::location(14)]] float4 oPad14 : VARY14;
    [[vk::location(15)]] float4 oPad15 : VARY15;
#endif
    [[vk::location(16)]] float4 oCol0 : VARY16;
};

static const int2 CORNERS[6] = {int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 0), int2(1, 1), int2(0, 1)};

struct SeaPoint {
    float3 p;
    float2 slope;
    float crest, coastDist, amp, shore;
};

// the vertex spacing at xz as a continuous function of the distance to the camera (the same world point gets the same
// displacement on both sides of a level border: no cracks; the finer waves come from the mips)
float footprintAt(WaterBlock w, float2 xz, float3 cam) {
    float camDist = length(float3(xz.x - cam.x, w.Get().ocean.x - cam.y, xz.y - cam.z));
    return max(w.Get().grid.x, camDist * 6.0 / w.Get().grid.z);
}

SeaPoint surfacePoint(WaterBlock w, float2 xz, float t, float3 cam) {
    SeaPoint r;
    if (fftMode(w)) {
        float footprint = footprintAt(w, xz, cam);
        // the FFT cascades, attenuated by the sea floor field (depth, distance to the waterline, shelter) and near the
        // camera, + the waves rolling in towards the shore (travel-time phase), + the lower, swashing waterline
        SeaFloor fl = seaFloor(w, xz);
        float cap = nearCameraCap(w, xz, cam);
        float4 wt = cascadeWeights(w, fl) * cap;
        FftSample fs = fftSampleLod(w, xz, wt, footprint);
        ShoreTrain st = shoreTrain(w, fl, t);
        float3 d = fs.d + st.d * cap;
        // towards the far plane the displacement fades (no jagged silhouette where the far plane cuts the sea; only the long
        // swell, which the coarse levels still resolve, rises above the horizon line); the shading keeps the waves' normals
        float camDist = length(xz - cam.xz);
        d *= 1.0 - smoothstep(0.6, 0.97, camDist / max(RVK_VSC(11).w, 1.0));
        float level = w.Get().ocean.x;
        float band = max(w.Get().misc.y, 1.0);
        float near = 1.0 - smoothstep(0.2 * band, band, fl.overlay);
        float along = waterNoised(xz * 0.00035).x * 0.8;
        float swash = sin(6.2831853 / max(w.Get().shoreWave.x, 2.0) * t + along);
        d.y += near * (-w.Get().ocean.z + w.Get().ocean.w * swash);
        r.p = float3(xz.x + d.x, level + d.y, xz.y + d.z);
        r.slope = fs.slope + st.slope * cap;
        // crest steepness: compression of the FFT surface (Jacobian) and the breaking shore waves
        r.crest = max(clamp((1.0 - fs.jac) * 1.5, 0.0, 1.0), max(st.crest, st.breaking * 0.8));
        r.coastDist = fl.sd;
        r.amp = clamp(d.y / max(0.5 * w.Get().seaInfo.x, 10.0), -1.0, 1.0);  // height over the mean, ~ -1 .. 1 (crest light)
        r.shore = st.weight;
        return r;
    }
    float4 field = shoreField(w, xz);
    r.coastDist = field.x;
    r.amp = waveScale(w, r.coastDist, xz, cam);
    float2 slope;
    float crest;
    float3 d = gerstner(xz, t, r.amp, slope, crest);
    // waves rolling in towards the coast (direction from the shore field), over the coast band
    r.shore = shoreWaveScale(w, r.coastDist, field.zw, xz, cam);
    if (r.shore > 0.0) {
        float2 s2;
        float c2;
        float2 toCoast = length(field.zw) > 1e-4 ? normalize(field.zw) : float2(0.0, 0.0);
        float3 d2 = shoreWaves(r.coastDist, toCoast, t, r.shore, s2, c2);
        d += d2;
        slope += s2;
        crest = max(crest, c2);
    }
    d.y += coastOffset(w, r.coastDist, t, xz);
    r.p = float3(xz.x + d.x, w.Get().ocean.x + d.y, xz.y + d.z);
    r.slope = slope;
    r.crest = crest;
    return r;
}

WaterVsOut VSMain(uint vertexId : SV_VertexID) {
    WaterVsOut o = (WaterVsOut)0;
    WaterBlock w = waterBlock();
    int vertexIndex = int(vertexId);
    int n = int(w.Get().grid.z);
    int levels = int(w.Get().grid.y);
    int quad = vertexIndex / 6;
    int2 corner = CORNERS[vertexIndex % 6];
    int perLevel = n * n;
    int level = quad / perLevel;
    int q = quad - level * perLevel;
    int2 iq = int2(q % n, q / n);
    float s = w.Get().grid.x * exp2(float(level));
    float3 cam = RVK_VSC(11).xyz;  // g_vCamPosAndFar of the engine's sea draw
    float2 origin = floor(cam.xz / (2.0 * s)) * (2.0 * s) - float(n / 2) * s;
    o.oCol0 = float4(1.0, 1.0, 1.0, 1.0);
    if (level > 0) {
        // the finer level's footprint: [inner, inner + n * s / 2)
        float si = s * 0.5;
        float2 inner = floor(cam.xz / (2.0 * si)) * (2.0 * si) - float(n / 2) * si;
        float2 q0 = origin + float2(iq) * s;
        float2 rel = (q0 - inner) / s;  // in quads of this level
        if (all(rel >= float2(-0.01, -0.01)) && all(rel < float2(float(n / 2) - 0.01, float(n / 2) - 0.01))) {
            o.position = float4(2.0, 2.0, 2.0, 1.0);  // covered by the finer level: degenerate
            o.oTc0 = o.oTc1 = o.oTc3 = o.oTc4 = o.oTc5 = o.oTc6 = o.oTc7 = float4(0.0, 0.0, 0.0, 0.0);
            return o;
        }
    }
    int2 iv = iq + corner;
    float2 xz = origin + float2(iv) * s;
    float t = w.Get().grid.w;
    SeaPoint sp = surfacePoint(w, xz, t, cam);
    // T-junctions with the coarser level: odd vertices on the outer edge take the average of their even neighbours
    if (level < levels - 1) {
        bool edgeX = iv.x == 0 || iv.x == n;
        bool edgeZ = iv.y == 0 || iv.y == n;
        float2 along = float2(0.0, 0.0);
        if (edgeX && (iv.y & 1) == 1) along = float2(0.0, s);
        else if (edgeZ && (iv.x & 1) == 1) along = float2(s, 0.0);
        if (any(along != float2(0.0, 0.0))) {
            SeaPoint a = surfacePoint(w, xz - along, t, cam);
            SeaPoint b = surfacePoint(w, xz + along, t, cam);
            sp.p = 0.5 * (a.p + b.p);
            sp.slope = 0.5 * (a.slope + b.slope);
            sp.crest = 0.5 * (a.crest + b.crest);
            sp.coastDist = 0.5 * (a.coastDist + b.coastDist);
            sp.amp = 0.5 * (a.amp + b.amp);
            sp.shore = 0.5 * (a.shore + b.shore);
        }
    }
    // far away the waves are smaller than a pixel: towards the mean normal (no glittering noise at the horizon)
    float dist = length(sp.p - cam);
    float2 slope = fftMode(w) ? sp.slope : sp.slope / (1.0 + dist / 25000.0);
    float3 N = normalize(float3(-slope.x, 1.0, -slope.y));
    // the sea mesh's texture coordinates (an affine fit of its vertices) through the engine ocean VS's transforms
    float3 x1 = float3(xz, 1.0);
    float2 uv = float2(dot(x1, w.Get().uvMapU.xyz), dot(x1, w.Get().uvMapV.xyz));
    o.oTc0 = float4(uv * w.Get().vsTc[5].xy + w.Get().vsTc[4].xy, uv);                    // c237 / c236
    o.oTc1 = float4(uv * w.Get().vsTc[3].xy + w.Get().vsTc[2].xy, uv * w.Get().vsTc[1].xy + w.Get().vsTc[0].xy);  // c235 / c234, c233 / c232
    o.oTc3 = float4(sp.p, w.Get().meshExtra.y);
    o.oTc4 = float4(N, w.Get().meshExtra.x);
    o.oTc5 = float4(w.Get().meshFrame.xyz, sp.amp);
    o.oTc6 = float4(cross(N, w.Get().meshFrame.xyz) * w.Get().meshFrame.w, sp.crest);
    o.oTc7 = float4(xz, sp.coastDist, sp.shore);
    // the engine's own g_vViewProjection (c0..c3 of its sea draw): the same depth as the engine's geometry at the coast
    float4 p4 = float4(sp.p, 1.0);
    float4 clip = float4(dot(RVK_VSC(0), p4), dot(RVK_VSC(1), p4), dot(RVK_VSC(2), p4), dot(RVK_VSC(3), p4));
    clip.xy += w.Get().posFix.xy * clip.w;
    clip.z += w.Get().posFix.z * clip.z + w.Get().posFix.w * clip.w;  // reversed depth (abi.h)
    o.position = clip;
    return o;
}
