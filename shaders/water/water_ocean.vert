#version 460
// The enhanced open sea (src/enh/water.cpp): instead of the engine's flat sea plane (Levelmesh_Oceanmesh_01, a coarse mesh
// spanning the world) a camera-centred clipmap grid at the sea's height is drawn and displaced by waves. No vertex buffer:
// gl_VertexIndex -> level, quad, corner. Each level is N x N quads of spacing base * 2^level, snapped to twice its spacing
// (vertices stay fixed in the world while the camera moves); level > 0 leaves out the N/2 x N/2 quads the finer level
// covers; vertices on a level's outer edge between two vertices of the coarser level take their average (no cracks).
// Waves are damped at the coast (shore field: the engine's own coast overlay) and near the camera; at the coast the
// waterline sits lower than the engine's and swashes up the beach.

#include "water_common.glsl"

layout(location = 0) out vec4 vWorld;   // xyz world position, w crest foam
layout(location = 1) out vec4 vNormal;  // xyz geometric normal (up), w wave amplitude at this vertex
layout(location = 2) out vec4 vExtra;   // xy undisplaced xz, z distance to the coast, w unused

const ivec2 CORNERS[6] = ivec2[](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 0), ivec2(1, 1), ivec2(0, 1));

vec3 surfacePoint(WaterBlock w, vec2 xz, float t, vec3 cam, out float crest, out float coastDist, out float amp) {
    vec4 field = shoreField(w, xz);
    coastDist = field.x;
    amp = waveScale(w, coastDist, xz, cam);
    vec3 n;
    vec3 d = gerstner(xz, t, amp, n, crest);
    d.y += coastOffset(w, coastDist, t, xz);
    return vec3(xz.x + d.x, w.ocean.x + d.y, xz.y + d.z);
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
    if (level > 0) {
        // the finer level's footprint: [inner, inner + n * s / 2)
        float si = s * 0.5;
        vec2 inner = floor(cam.xz / (2.0 * si)) * (2.0 * si) - float(n / 2) * si;
        vec2 q0 = origin + vec2(iq) * s;
        vec2 rel = (q0 - inner) / s;  // in quads of this level
        if (all(greaterThanEqual(rel, vec2(-0.01))) && all(lessThan(rel, vec2(float(n / 2) - 0.01)))) {
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // covered by the finer level: degenerate
            vWorld = vec4(0.0);
            vNormal = vec4(0.0);
            vExtra = vec4(0.0);
            return;
        }
    }
    ivec2 iv = iq + corner;
    vec2 xz = origin + vec2(iv) * s;
    float t = w.grid.w;
    float crest, coastDist, amp;
    vec3 p = surfacePoint(w, xz, t, cam, crest, coastDist, amp);
    // T-junctions with the coarser level: odd vertices on the outer edge take the average of their even neighbours
    if (level < levels - 1) {
        bool edgeX = iv.x == 0 || iv.x == n;
        bool edgeZ = iv.y == 0 || iv.y == n;
        vec2 along = vec2(0.0);
        if (edgeX && (iv.y & 1) == 1) along = vec2(0.0, s);
        else if (edgeZ && (iv.x & 1) == 1) along = vec2(s, 0.0);
        if (along != vec2(0.0)) {
            float c0, c1, d0, d1, a0, a1;
            vec3 p0 = surfacePoint(w, xz - along, t, cam, c0, d0, a0);
            vec3 p1 = surfacePoint(w, xz + along, t, cam, c1, d1, a1);
            p = 0.5 * (p0 + p1);
            crest = 0.5 * (c0 + c1);
            coastDist = 0.5 * (d0 + d1);
            amp = 0.5 * (a0 + a1);
        }
    }
    vWorld = vec4(p, crest);
    vNormal = vec4(0.0, 1.0, 0.0, amp);
    vExtra = vec4(xz, coastDist, 0.0);
    // the engine's own g_vViewProjection (c0..c3 of its sea draw): the same depth as the engine's geometry at the coast
    vec4 p4 = vec4(p, 1.0);
    vec4 clip = vec4(dot(pc.vs.c[0], p4), dot(pc.vs.c[1], p4), dot(pc.vs.c[2], p4), dot(pc.vs.c[3], p4));
    clip.xy += w.posFix.xy * clip.w;
    gl_Position = clip;
}
