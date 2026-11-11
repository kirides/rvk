// F11 (frame-ownership.md 14.26): the C++ twin of shaders/include/rvk_particle.hlsli (keep the two in step: same operations, same order).
#include "d3d9/particle_gpu.h"

#include <cmath>

namespace rvk::d3d9::particle {

namespace {

struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 v3(const float* p) { return {p[0], p[1], p[2]}; }
V3 normalize(V3 v) {
    const float l2 = (v.x * v.x + v.y * v.y) + v.z * v.z;
    const float l = std::sqrt(l2);
    if (!(l > 0.0f)) return v;
    const float inv = 1.0f / l;
    return {v.x * inv, v.y * inv, v.z * inv};
}
V3 cross(V3 a, V3 b) { return {b.z * a.y - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - b.x * a.y}; }
V3 dir(V3 v, const float* m0, const float* m1, const float* m2) {
    return {(v.x * m0[0] + v.y * m1[0]) + v.z * m2[0], (v.x * m0[1] + v.y * m1[1]) + v.z * m2[1], (v.x * m0[2] + v.y * m1[2]) + v.z * m2[2]};
}
V3 mul4(V3 v, const float* m0, const float* m1, const float* m2, const float* m3) {
    const V3 a = {((v.x * m0[0] + v.y * m1[0]) + v.z * m2[0]) + m3[0], ((v.x * m0[1] + v.y * m1[1]) + v.z * m2[1]) + m3[1],
                  ((v.x * m0[2] + v.y * m1[2]) + v.z * m2[2]) + m3[2]};
    if (m0[3] == 0.0f && m1[3] == 0.0f && m2[3] == 0.0f && m3[3] == 1.0f) return a;
    const float w = ((v.y * m1[3] + v.x * m0[3]) + v.z * m2[3]) + m3[3];
    if (std::fabs(w) < 1e-5f) return {};
    const float inv = 1.0f / w;
    return {a.x * inv, a.y * inv, a.z * inv};
}
struct Rot {
    V3 r0, r1, r2;
};
Rot rotation(V3 axis, float s, float c) {
    const V3 n = normalize(axis);
    const float k = 1.0f - c;
    const float x = n.x, y = n.y, z = n.z;
    return {{(x * x) * k + c, (x * y) * k - z * s, (x * z) * k + y * s},
            {(x * y) * k + z * s, (y * y) * k + c, (y * z) * k - x * s},
            {(x * z) * k - y * s, (y * z) * k + x * s, (z * z) * k + c}};
}
V3 mul3(V3 v, const Rot& m) {
    return {(v.x * m.r0.x + v.y * m.r1.x) + v.z * m.r2.x, (v.x * m.r0.y + v.y * m.r1.y) + v.z * m.r2.y, (v.x * m.r0.z + v.y * m.r1.z) + v.z * m.r2.z};
}

}  // namespace

void expand(const OpConsts& k, const Record& r, std::uint32_t corner, std::uint8_t out[kVertexStride]) {
    const float *R0 = k.c[0], *R1 = k.c[1], *R2 = k.c[2], *cam = k.c[3];
    const float *E0 = k.c[4], *E1 = k.c[5], *E2 = k.c[6], *E3 = k.c[7];
    const float *M0 = k.c[8], *M1 = k.c[9], *M2 = k.c[10], *M3 = k.c[11];
    const float *ax = k.c[12], *pin = k.c[13], *eo = k.c[14], *up = k.c[15];
    const auto flags = static_cast<std::uint32_t>(ax[3]);
    const bool entityCoords = (flags & kFlagEntityCoords) != 0, axisInEntity = (flags & kFlagAxisInEntity) != 0, pinned = (flags & kFlagPinned) != 0;
    const std::uint32_t normalMode = (flags >> 4) & 15u;
    const int mode = static_cast<int>(cam[3]);
    const V3 pos = v3(r.r[0]), d = v3(r.r[1]);
    V3 L, R, U;
    bool side = false;
    if (mode == 1) {
        L = v3(R2);
        R = cross(d, L);
        U = cross(L, R);
    } else if (mode == 3) {
        L = {-ax[0], -ax[1], -ax[2]};
        if (axisInEntity) L = dir(L, E0, E1, E2);
        R = cross(d, L);
        U = cross(L, R);
    } else if (mode == 2 || mode == 4) {
        L = mode == 2 ? v3(ax) : d;
        if (axisInEntity) L = dir(L, E0, E1, E2);
        side = true;
    } else {
        L = v3(R2);
        R = v3(R0);
        U = v3(R1);
    }
    if (side) {
        const float nz = -L.z;
        if (!(L.x == up[0] && L.y == up[1] && L.z == up[2])) R = {nz, 0.0f, L.x};
        else R = {0.0f, nz, L.y};
        U = cross(R, L);
    }
    if (entityCoords) {
        L = mul4(L, M0, M1, M2, M3);
        U = mul4(U, M0, M1, M2, M3);
        R = mul4(R, M0, M1, M2, M3);
    }
    R = normalize(R);
    U = normalize(U);
    L = normalize(L);
    const float hx = r.r[0][3], hy = r.r[1][3];
    const float am = (pin[3] - 1.0f) * hx, ap = (pin[3] + 1.0f) * hx, bm = (eo[3] - 1.0f) * hy, bp = (eo[3] + 1.0f) * hy;
    const bool left = corner == 0 || corner == 1, top = corner == 0 || corner == 3;
    const float a = left ? am : ap, b = top ? bp : bm, sr = left ? -1.0f : 1.0f;
    V3 c = {R.x * a + U.x * b, R.y * a + U.y * b, R.z * a + U.z * b};
    const V3 Rs = {R.x * sr, R.y * sr, R.z * sr};
    V3 n = top ? V3{Rs.x + U.x, Rs.y + U.y, Rs.z + U.z} : V3{Rs.x - U.x, Rs.y - U.y, Rs.z - U.z};
    const auto m = static_cast<std::uint32_t>(r.mask);
    const float sc[3][2] = {{r.r[2][0], r.r[2][1]}, {r.r[2][2], r.r[2][3]}, {r.r[3][0], r.r[3][1]}};
    for (std::uint32_t i = 0; i < 3; ++i) {
        if ((m & (1u << i)) == 0) continue;
        const V3 axis = i == 0 ? R : i == 1 ? U : L;
        const Rot rot = rotation(axis, sc[i][0], sc[i][1]);
        c = mul3(c, rot);
        n = mul3(n, rot);
        L = mul3(L, rot);
    }
    V3 cw = {c.x + pos.x, c.y + pos.y, c.z + pos.z};
    const V3 P = entityCoords ? V3{pos.x + E3[0], pos.y + E3[1], pos.z + E3[2]} : pos;
    const V3 Lw = axisInEntity ? dir(L, E0, E1, E2) : L;
    const V3 nc = normalize({cam[0] - P.x, cam[1] - P.y, cam[2] - P.z});
    const float dt = ((Lw.y * nc.y) + (Lw.x * nc.x)) + (Lw.z * nc.z);
    if (0.0f > dt) L = {L.x * -1.0f, L.y * -1.0f, L.z * -1.0f};
    if (normalMode == 0 || normalMode == 1) n = L;
    else if (normalMode == 2) n = {n.x + L.x, n.y + L.y, n.z + L.z};
    else if (normalMode == 3) n = {pos.x - eo[0], pos.y - eo[1], pos.z - eo[2]};
    if (pinned && (corner == 2 || corner == 3)) cw = v3(pin);
    const std::uint32_t c0 = argbOf(r.r[3][2], std::fabs(dt), dt > 0.0f ? dt : 0.0f, r.r[3][3]);
    const float uv[4] = {left ? r.r[4][0] : r.r[4][2], top ? r.r[4][1] : r.r[4][3], left ? r.r[5][0] : r.r[5][2], top ? r.r[5][1] : r.r[5][3]};
    std::memcpy(out, &cw, 12);
    std::memcpy(out + 0xc, &n, 12);
    std::memcpy(out + 0x18, &c0, 4);
    std::memcpy(out + 0x1c, &r.colour1, 4);
    std::memcpy(out + 0x20, uv, 16);
}

}  // namespace rvk::d3d9::particle
