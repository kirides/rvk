#version 460
// The enhanced open sea (src/enh/water.cpp): instead of the engine's flat sea plane (Levelmesh_Oceanmesh_01, a coarse mesh
// spanning the world) a camera-centred clipmap grid at the sea's height is drawn and displaced by waves. No vertex buffer:
// gl_VertexIndex -> level, quad, corner. Each level is N x N quads of spacing base * 2^level, snapped to twice its spacing
// (vertices stay fixed in the world while the camera moves); level > 0 leaves out the N/2 x N/2 quads the finer level
// covers; vertices on a level's outer edge between two vertices of the coarser level take their average (no cracks).
// Waves (water.ocean): the FFT ocean's cascades (ocean_surface.glsl: attenuated by the sea floor field towards the shore and
// inside the harbour, + a wave train rolling in along the travel time to the shore) or the M7 Gerstner sum (damped at the
// coast overlay, shore waves inside the coast band); both capped near the camera at the water's level. At the coast the
// waterline sits lower than the engine's and swashes up the beach. The outputs are the engine's ocean VS's (vs 14c1f115c1e58f30: TEXCOORD0/1 = the sea mesh's texture coordinates
// through its transforms vs c232..c237, TEXCOORD3 world + global alpha, TEXCOORD4..6 normal / tangent / binormal), so
// water.frag shades the grid with the engine's ocean material; TEXCOORD7 carries the grid's own values.

#include "water_common.glsl"

layout(location = 0) out vec4 oTc0;
layout(location = 1) out vec4 oTc1;
layout(location = 3) out vec4 oTc3;   // world position, w global alpha
layout(location = 4) out vec4 oTc4;   // wave normal, w outdoor factor
layout(location = 5) out vec4 oTc5;   // tangent (recomputed in the PS), w wave amplitude (FFT: height over the mean / Hs/2)
layout(location = 6) out vec4 oTc6;   // binormal (recomputed in the PS), w crest steepness
layout(location = 7) out vec4 oTc7;   // undisplaced xz, distance to the coast (FFT: signed, to the waterline), shore-wave weight
layout(location = 16) out vec4 oCol0;

const ivec2 CORNERS[6] = ivec2[](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 0), ivec2(1, 1), ivec2(0, 1));

struct SeaPoint {
    vec3 p;
    vec2 slope;
    float crest, coastDist, amp, shore;
};

// the vertex spacing at xz as a continuous function of the distance to the camera (the same world point gets the same
// displacement on both sides of a level border: no cracks; the finer waves come from the mips)
float footprintAt(WaterBlock w, vec2 xz, vec3 cam) {
    float camDist = length(vec3(xz.x - cam.x, w.ocean.x - cam.y, xz.y - cam.z));
    return max(w.grid.x, camDist * 6.0 / w.grid.z);
}

SeaPoint surfacePoint(WaterBlock w, vec2 xz, float t, vec3 cam) {
    SeaPoint r;
    if (fftMode(w)) {
        float footprint = footprintAt(w, xz, cam);
        // the FFT cascades, attenuated by the sea floor field (depth, distance to the waterline, shelter) and near the
        // camera, + the waves rolling in towards the shore (travel-time phase), + the lower, swashing waterline
        SeaFloor fl = seaFloor(w, xz);
        float cap = nearCameraCap(w, xz, cam);
        vec4 wt = cascadeWeights(w, fl) * cap;
        FftSample fs = fftSampleLod(w, xz, wt, footprint);
        ShoreTrain st = shoreTrain(w, fl, t);
        vec3 d = fs.d + st.d * cap;
        // towards the far plane the displacement fades (the sea meets the horizon without a jagged silhouette); the
        // shading keeps the waves' normals
        float camDist = length(xz - cam.xz);
        d *= 1.0 - smoothstep(0.45, 0.9, camDist / max(pc.vs.c[11].w, 1.0));
        float level = w.ocean.x;
        float band = max(w.misc.y, 1.0);
        float near = 1.0 - smoothstep(0.2 * band, band, fl.overlay);
        float along = waterNoised(xz * 0.00035).x * 0.8;
        float swash = sin(6.2831853 / max(w.shoreWave.x, 2.0) * t + along);
        d.y += near * (-w.ocean.z + w.ocean.w * swash);
        r.p = vec3(xz.x + d.x, level + d.y, xz.y + d.z);
        r.slope = fs.slope + st.slope * cap;
        // crest steepness: compression of the FFT surface (Jacobian) and the breaking shore waves
        r.crest = max(clamp((1.0 - fs.jac) * 1.5, 0.0, 1.0), max(st.crest, st.breaking * 0.8));
        r.coastDist = fl.sd;
        r.amp = clamp(d.y / max(0.5 * w.seaInfo.x, 10.0), -1.0, 1.0);  // height over the mean, ~ -1 .. 1 (crest light)
        r.shore = st.weight;
        return r;
    }
    vec4 field = shoreField(w, xz);
    r.coastDist = field.x;
    r.amp = waveScale(w, r.coastDist, xz, cam);
    vec2 slope;
    float crest;
    vec3 d = gerstner(xz, t, r.amp, slope, crest);
    // waves rolling in towards the coast (direction from the shore field), over the coast band
    r.shore = shoreWaveScale(w, r.coastDist, field.zw, xz, cam);
    if (r.shore > 0.0) {
        vec2 s2;
        float c2;
        vec2 toCoast = length(field.zw) > 1e-4 ? normalize(field.zw) : vec2(0.0);
        vec3 d2 = shoreWaves(r.coastDist, toCoast, t, r.shore, s2, c2);
        d += d2;
        slope += s2;
        crest = max(crest, c2);
    }
    d.y += coastOffset(w, r.coastDist, t, xz);
    r.p = vec3(xz.x + d.x, w.ocean.x + d.y, xz.y + d.z);
    r.slope = slope;
    r.crest = crest;
    return r;
}

void main() {
    WaterBlock w = waterBlock();
    int n = int(w.grid.z);
    int levels = int(w.grid.y);
    int quad = gl_VertexIndex / 6;
    ivec2 corner = CORNERS[gl_VertexIndex % 6];
    int perLevel = n * n;
    int level = quad / perLevel;
    int q = quad - level * perLevel;
    ivec2 iq = ivec2(q % n, q / n);
    float s = w.grid.x * exp2(float(level));
    vec3 cam = pc.vs.c[11].xyz;  // g_vCamPosAndFar of the engine's sea draw
    vec2 origin = floor(cam.xz / (2.0 * s)) * (2.0 * s) - float(n / 2) * s;
    oCol0 = vec4(1.0);
    if (level > 0) {
        // the finer level's footprint: [inner, inner + n * s / 2)
        float si = s * 0.5;
        vec2 inner = floor(cam.xz / (2.0 * si)) * (2.0 * si) - float(n / 2) * si;
        vec2 q0 = origin + vec2(iq) * s;
        vec2 rel = (q0 - inner) / s;  // in quads of this level
        if (all(greaterThanEqual(rel, vec2(-0.01))) && all(lessThan(rel, vec2(float(n / 2) - 0.01)))) {
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // covered by the finer level: degenerate
            oTc0 = oTc1 = oTc3 = oTc4 = oTc5 = oTc6 = oTc7 = vec4(0.0);
            return;
        }
    }
    ivec2 iv = iq + corner;
    vec2 xz = origin + vec2(iv) * s;
    float t = w.grid.w;
    SeaPoint sp = surfacePoint(w, xz, t, cam);
    // T-junctions with the coarser level: odd vertices on the outer edge take the average of their even neighbours
    if (level < levels - 1) {
        bool edgeX = iv.x == 0 || iv.x == n;
        bool edgeZ = iv.y == 0 || iv.y == n;
        vec2 along = vec2(0.0);
        if (edgeX && (iv.y & 1) == 1) along = vec2(0.0, s);
        else if (edgeZ && (iv.x & 1) == 1) along = vec2(s, 0.0);
        if (along != vec2(0.0)) {
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
    vec2 slope = fftMode(w) ? sp.slope : sp.slope / (1.0 + dist / 25000.0);
    vec3 N = normalize(vec3(-slope.x, 1.0, -slope.y));
    // the sea mesh's texture coordinates (an affine fit of its vertices) through the engine ocean VS's transforms
    vec3 x1 = vec3(xz, 1.0);
    vec2 uv = vec2(dot(x1, w.uvMapU.xyz), dot(x1, w.uvMapV.xyz));
    oTc0 = vec4(uv * w.vsTc[5].xy + w.vsTc[4].xy, uv);                    // c237 / c236
    oTc1 = vec4(uv * w.vsTc[3].xy + w.vsTc[2].xy, uv * w.vsTc[1].xy + w.vsTc[0].xy);  // c235 / c234, c233 / c232
    oTc3 = vec4(sp.p, w.meshExtra.y);
    oTc4 = vec4(N, w.meshExtra.x);
    oTc5 = vec4(w.meshFrame.xyz, sp.amp);
    oTc6 = vec4(cross(N, w.meshFrame.xyz) * w.meshFrame.w, sp.crest);
    oTc7 = vec4(xz, sp.coastDist, sp.shore);
    // the engine's own g_vViewProjection (c0..c3 of its sea draw): the same depth as the engine's geometry at the coast
    vec4 p4 = vec4(sp.p, 1.0);
    vec4 clip = vec4(dot(pc.vs.c[0], p4), dot(pc.vs.c[1], p4), dot(pc.vs.c[2], p4), dot(pc.vs.c[3], p4));
    clip.xy += w.posFix.xy * clip.w;
    gl_Position = clip;
}
