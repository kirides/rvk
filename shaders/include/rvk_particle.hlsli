// F11 (docs/research/frame-ownership.md 14.26, native.own_particles): the particle quads expanded on the GPU. A particle op's draw is ONE instanced draw:
// instance i = the i-th particle record of the op (in the engine's depth order: DepthSortParticles' sort, done on the CPU because the sorted array is the
// emitter's game state), vertex 0..3 = its corners (the engine's index order of a quad). The engine's particle vertex shader (ge_particle_2_0.fx, any variant:
// lit / unlit / one or two textures) is translated unchanged; its vertex inputs are replaced by the corner this function builds, so everything after the
// vertex fetch is the engine's program. The arithmetic is eCParticle_PS::UpdateVertices 0x5d58b0 (native/static_ops_fx.cpp particleVertices) with
// SharedBase's vector rules (Normalize = 1 / sqrt(len2) times x, y, z; the cross product; bCMatrix3::SetToRotation with CRT sinf / cosf, here from the record;
// the row-vector matrix products with their summation order; the affine / projective bCVector *= bCMatrix), every operation `precise` (no contraction).
// The C++ twin is native/static_compose.cpp sc::particleExpand (the compare mode checks it against the engine-rule vertices, rvk_tests against this file's rules).
//
// Per-op constants: VS c232..c247 (window 3; the op's draw uploads them, the engine's particle shaders read c0..c76):
//   c232..c234 the inverted active camera's rows 0..2 (right, up, look), c235 xyz = its row 3 (position), w = the orientation mode PS +0x170
//   c236..c239 the entity matrix (rows), c240..c243 its inverse without translation (SharedBase GetInverted, rows; coords 1 only)
//   c244 xyz = the fixed axis PS +0x178, w = flags (1 entity coordinates, 2 axis in entity space, 4 pinned corners; normal mode PS +0x2b0 x 16)
//   c245 xyz = the pinned corners' point, w = pivot A (+0x184); c246 xyz = Eo (the world emitter's origin), w = pivot B (+0x188); c247 xyz = bCVector::GetUp
// Per-particle record (instance stream, TEXCOORD8..15; native/static_compose.h ParticleRecord):
//   r0 = position, hx; r1 = d (+0x18 x +0x24), hy; r2 = sin / cos of the angles +0x48, +0x4c; r3 = sin / cos of +0x50, the age ratio, 1 - frame fraction;
//   r4 = u0, v0, u1, v1; r5 = the blend frame's u0, v0, u1, v1; colour1 = COLOR1 as the engine's D3DCOLOR; mask = which angles are not 0 (bits 1, 2, 4)
#pragma once

struct RvkParticleVertex {
    float4 pos, nrm, c0, c1, t0, t1;
};

float3 rvkPtNormalize(float3 v) {
    precise float l2 = (v.x * v.x + v.y * v.y) + v.z * v.z;
    precise float l = sqrt(l2);
    if (!(l > 0.0)) return v;
    precise float inv = 1.0 / l;
    precise float3 r = float3(v.x * inv, v.y * inv, v.z * inv);
    return r;
}
// SetCrossProduct(a, b)
float3 rvkPtCross(float3 a, float3 b) {
    precise float3 r = float3(b.z * a.y - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - b.x * a.y);
    return r;
}
// TransformDirection(m): the upper 3 x 3, row vector
float3 rvkPtDir(float3 v, float4 m0, float4 m1, float4 m2) {
    precise float3 r = float3((v.x * m0.x + v.y * m1.x) + v.z * m2.x, (v.x * m0.y + v.y * m1.y) + v.z * m2.y, (v.x * m0.z + v.y * m1.z) + v.z * m2.z);
    return r;
}
// bCVector *= bCMatrix: affine (m03 = m13 = m23 = 0, m33 = 1) or projective
float3 rvkPtMul4(float3 v, float4 m0, float4 m1, float4 m2, float4 m3) {
    precise float3 a = float3(((v.x * m0.x + v.y * m1.x) + v.z * m2.x) + m3.x, ((v.x * m0.y + v.y * m1.y) + v.z * m2.y) + m3.y,
                              ((v.x * m0.z + v.y * m1.z) + v.z * m2.z) + m3.z);
    if (m0.w == 0.0 && m1.w == 0.0 && m2.w == 0.0 && m3.w == 1.0) return a;
    precise float w = ((v.y * m1.w + v.x * m0.w) + v.z * m2.w) + m3.w;
    if (abs(w) < 1e-5) return float3(0.0, 0.0, 0.0);
    precise float inv = 1.0 / w;
    precise float3 r = float3(a.x * inv, a.y * inv, a.z * inv);
    return r;
}
// bCMatrix3::SetToRotation(axis, angle) with the record's sin / cos; v *= M (row vector)
struct RvkPtRot {
    float3 r0, r1, r2;
};
RvkPtRot rvkPtRotation(float3 axis, float s, float c) {
    float3 n = rvkPtNormalize(axis);
    precise float k = 1.0 - c;
    precise float x = n.x, y = n.y, z = n.z;
    RvkPtRot m;
    precise float3 a = float3((x * x) * k + c, (x * y) * k - z * s, (x * z) * k + y * s);
    precise float3 b = float3((x * y) * k + z * s, (y * y) * k + c, (y * z) * k - x * s);
    precise float3 d = float3((x * z) * k - y * s, (y * z) * k + x * s, (z * z) * k + c);
    m.r0 = a;
    m.r1 = b;
    m.r2 = d;
    return m;
}
float3 rvkPtMul3(float3 v, RvkPtRot m) {
    precise float3 r = float3((v.x * m.r0.x + v.y * m.r1.x) + v.z * m.r2.x, (v.x * m.r0.y + v.y * m.r1.y) + v.z * m.r2.y,
                              (v.x * m.r0.z + v.y * m.r1.z) + v.z * m.r2.z);
    return r;
}
// bCByteAlphaColor from a float channel: x 255, truncated, the low byte (no clamp), as the D3DCOLOR fetch delivers it
float rvkPtByte(float x) {
    precise float s = x * 255.0;
    uint b = (s >= -2147483648.0 && s < 2147483648.0) ? uint(int(s)) & 255u : 0u;
    return float(b) / 255.0;
}

RvkParticleVertex rvkParticleExpand(uint corner, float4 r0, float4 r1, float4 r2, float4 r3, float4 r4, float4 r5, float4 colour1, float mask) {
    const float4 R0 = RVK_VSC(232), R1 = RVK_VSC(233), R2 = RVK_VSC(234), cam = RVK_VSC(235);
    const float4 E0 = RVK_VSC(236), E1 = RVK_VSC(237), E2 = RVK_VSC(238), E3 = RVK_VSC(239);
    const float4 M0 = RVK_VSC(240), M1 = RVK_VSC(241), M2 = RVK_VSC(242), M3 = RVK_VSC(243);
    const float4 ax = RVK_VSC(244), pin = RVK_VSC(245), eo = RVK_VSC(246), up = RVK_VSC(247);
    const uint flags = uint(ax.w);
    const bool entityCoords = (flags & 1u) != 0u, axisInEntity = (flags & 2u) != 0u, pinned = (flags & 4u) != 0u;
    const uint normalMode = (flags >> 4) & 15u;
    const int mode = int(cam.w);
    const float3 pos = r0.xyz, d = r1.xyz;
    float3 L, R, U;
    bool side = false;
    if (mode == 1) {
        L = R2.xyz;
        R = rvkPtCross(d, L);
        U = rvkPtCross(L, R);
    } else if (mode == 3) {
        L = -ax.xyz;
        if (axisInEntity) L = rvkPtDir(L, E0, E1, E2);
        R = rvkPtCross(d, L);
        U = rvkPtCross(L, R);
    } else if (mode == 2 || mode == 4) {
        L = mode == 2 ? ax.xyz : d;
        if (axisInEntity) L = rvkPtDir(L, E0, E1, E2);
        side = true;
    } else {
        L = R2.xyz;
        R = R0.xyz;
        U = R1.xyz;
    }
    if (side) {
        const float nz = -L.z;
        if (!(L.x == up.x && L.y == up.y && L.z == up.z)) R = float3(nz, 0.0, L.x);
        else R = float3(0.0, nz, L.y);
        U = rvkPtCross(R, L);
    }
    if (entityCoords) {
        L = rvkPtMul4(L, M0, M1, M2, M3);
        U = rvkPtMul4(U, M0, M1, M2, M3);
        R = rvkPtMul4(R, M0, M1, M2, M3);
    }
    R = rvkPtNormalize(R);
    U = rvkPtNormalize(U);
    L = rvkPtNormalize(L);
    const float hx = r0.w, hy = r1.w;
    precise float am = (pin.w - 1.0) * hx, ap = (pin.w + 1.0) * hx, bm = (eo.w - 1.0) * hy, bp = (eo.w + 1.0) * hy;
    // this vertex's corner (0: am / bp, 1: am / bm, 2: ap / bm, 3: ap / bp) and normal (0: -R + U, 1: -R - U, 2: R - U, 3: R + U)
    const float a = (corner == 0u || corner == 1u) ? am : ap;
    const float b = (corner == 0u || corner == 3u) ? bp : bm;
    const float sr = (corner == 0u || corner == 1u) ? -1.0 : 1.0;
    precise float3 c = float3(R.x * a + U.x * b, R.y * a + U.y * b, R.z * a + U.z * b);
    precise float3 Rs = float3(R.x * sr, R.y * sr, R.z * sr);
    precise float3 n = (corner == 0u || corner == 3u) ? float3(Rs.x + U.x, Rs.y + U.y, Rs.z + U.z) : float3(Rs.x - U.x, Rs.y - U.y, Rs.z - U.z);
    const uint m = uint(mask);
    const float2 sc[3] = {r2.xy, r2.zw, r3.xy};
    [unroll] for (uint k = 0; k < 3; ++k) {
        if ((m & (1u << k)) == 0u) continue;
        const float3 axis = k == 0u ? R : k == 1u ? U : L;
        const RvkPtRot rot = rvkPtRotation(axis, sc[k].x, sc[k].y);
        c = rvkPtMul3(c, rot);
        n = rvkPtMul3(n, rot);
        L = rvkPtMul3(L, rot);
    }
    precise float3 cw = float3(c.x + pos.x, c.y + pos.y, c.z + pos.z);
    // the facing test: the look vector against the direction to the camera
    precise float3 P = entityCoords ? float3(pos.x + E3.x, pos.y + E3.y, pos.z + E3.z) : pos;
    float3 Lw = axisInEntity ? rvkPtDir(L, E0, E1, E2) : L;
    precise float3 toCam = float3(cam.x - P.x, cam.y - P.y, cam.z - P.z);
    const float3 nc = rvkPtNormalize(toCam);
    precise float dt = ((Lw.y * nc.y) + (Lw.x * nc.x)) + (Lw.z * nc.z);
    if (0.0 > dt) L = float3(L.x * -1.0, L.y * -1.0, L.z * -1.0);
    if (normalMode == 0u || normalMode == 1u) {
        n = L;
    } else if (normalMode == 2u) {
        precise float3 t = float3(n.x + L.x, n.y + L.y, n.z + L.z);
        n = t;
    } else if (normalMode == 3u) {
        precise float3 N = float3(pos.x - eo.x, pos.y - eo.y, pos.z - eo.z);
        n = N;
    }
    if (pinned && (corner == 2u || corner == 3u)) cw = pin.xyz;
    RvkParticleVertex o;
    o.pos = float4(cw, 1.0);
    o.nrm = float4(n, 1.0);
    o.c0 = float4(rvkPtByte(r3.z), rvkPtByte(abs(dt)), rvkPtByte(dt > 0.0 ? dt : 0.0), rvkPtByte(r3.w));
    o.c1 = colour1;
    const bool left = corner == 0u || corner == 1u, top = corner == 0u || corner == 3u;
    o.t0 = float4(left ? r4.x : r4.z, top ? r4.y : r4.w, 0.0, 1.0);
    o.t1 = float4(left ? r5.x : r5.z, top ? r5.y : r5.w, 0.0, 1.0);
    return o;
}

// ---- the precipitation (eCSkyObject billboards; native/static_compose.cpp sc::skyBillboardVertices): one record per drop (0x30 bytes: position, size.x Rt.xy
// as s, size.y V.xy as r, colour, the uv rect u0 v0 u1 v1) -> corner k = (-s + r, s + r, s - r, -s - r), its uv ((u0, v0), (u1, v0), (u1, v1), (u0, v1)).
// The record: r0 = position, s.x; r1 = s.y, r.x, r.y; colour = the D3DCOLOR; r3 = the uv rect. No constants.
struct RvkSkyBbVertex {
    float4 pos, c, uv, corner;
};
RvkSkyBbVertex rvkSkyBillboardExpand(uint corner, float4 r0, float4 r1, float4 colour, float4 r3) {
    const float2 s = float2(r0.w, r1.x), r = r1.yz;
    precise float2 c;
    if (corner == 0u) c = float2(-s.x + r.x, -s.y + r.y);
    else if (corner == 1u) c = float2(s.x + r.x, s.y + r.y);
    else if (corner == 2u) c = float2(s.x - r.x, s.y - r.y);
    else c = float2(-s.x - r.x, -s.y - r.y);
    RvkSkyBbVertex o;
    o.pos = float4(r0.xyz, 1.0);
    o.c = colour;
    o.uv = float4(corner == 0u || corner == 3u ? r3.x : r3.z, corner <= 1u ? r3.y : r3.w, 0.0, 1.0);
    o.corner = float4(c, 0.0, 1.0);
    return o;
}

// ---- strips (eCStrip_PS::UpdateVertices 0x6fd960): one record per vertex (position, the accumulated u / v, the segment's alpha factor, the side 0 / 1); the
// colours A (c232) and B (c233) lerped as bCFloatAlphaColor's Sub / Multiply / Add do (per channel, alpha too), the alpha times the factor, SharedBase's bytes.
// The C++ twin: d3d9/particle_gpu.cpp expandStrip.
struct RvkStripVertex {
    float4 pos, c, t0, t1;
};
RvkStripVertex rvkStripExpand(float4 r0, float4 r1) {
    const float4 A = RVK_VSC(232), B = RVK_VSC(233);
    const float t = r0.w;
    precise float4 c = float4((B.x - A.x) * t + A.x, (B.y - A.y) * t + A.y, (B.z - A.z) * t + A.z, (B.w - A.w) * t + A.w);
    precise float a = c.w * r1.x;
    RvkStripVertex o;
    o.pos = float4(r0.xyz, 1.0);
    o.c = float4(rvkPtByte(c.x), rvkPtByte(c.y), rvkPtByte(c.z), rvkPtByte(a));
    o.t0 = float4(t, r1.y, 0.0, 1.0);
    o.t1 = o.t0;
    return o;
}
