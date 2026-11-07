#include "core/cluster_cull.h"

#include <cmath>

namespace rvk::clustercull {

bool makeView(const float regs[4][4], const float world[12], float margin, bool depthPlanes, View& out) {
    out = View{};
    // object-to-clip rows: r_i = c_i.x * w0 + c_i.y * w1 + c_i.z * w2 + (0, 0, 0, c_i.w)
    float r[4][4];
    bool finite = true;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            r[i][j] = regs[i][0] * world[j] + regs[i][1] * world[4 + j] + regs[i][2] * world[8 + j] + (j == 3 ? regs[i][3] : 0.0f);
            finite = finite && std::isfinite(r[i][j]);
        }
    }
    const float dm = margin > 0.0f ? kDepthMargin : 0.0f;  // (margin 0 = the exact test)
    for (int j = 0; j < 4; ++j) {
        out.plane[0][j] = r[3][j] + r[0][j];
        out.plane[1][j] = r[3][j] - r[0][j];
        out.plane[2][j] = r[3][j] + r[1][j];
        out.plane[3][j] = r[3][j] - r[1][j];
        out.plane[4][j] = r[2][j];
        out.plane[5][j] = 0.0f;  // the far plane stays open: its normal is ~1e-3 of the others', its distance in float loses ~10 world units (a cluster 17 units inside it was culled in the game); the hardware clips what lies beyond it
        out.row3[j] = r[3][j];
    }
    out.plane[5][3] = 1.0f;
    for (int p = 0; p < 6; ++p) {
        out.len[p] = p == 5 ? 0.0f : std::sqrt(out.plane[p][0] * out.plane[p][0] + out.plane[p][1] * out.plane[p][1] + out.plane[p][2] * out.plane[p][2]);
        out.margin[p] = p < 4 ? margin : (p == 4 ? dm : 0.0f);
        finite = finite && std::isfinite(out.len[p]);
    }
    out.len3 = std::sqrt(r[3][0] * r[3][0] + r[3][1] * r[3][1] + r[3][2] * r[3][2]);
    finite = finite && std::isfinite(out.len3);
    if (!depthPlanes) {
        for (int j = 0; j < 4; ++j) out.plane[4][j] = 0.0f;
        out.plane[4][3] = 1.0f;
        out.len[4] = 0.0f;
    }
    out.ok = finite;
    return finite;
}

bool sphereOutside(const View& v, const float c[3], float r) {
    const float wc = v.row3[0] * c[0] + v.row3[1] * c[1] + v.row3[2] * c[2] + v.row3[3];
    const float wSpan = std::fabs(wc) + r * v.len3;
    for (int p = 0; p < 6; ++p) {
        const float d = v.plane[p][0] * c[0] + v.plane[p][1] * c[1] + v.plane[p][2] * c[2] + v.plane[p][3];
        if (d < -(r * v.len[p] + v.margin[p] * wSpan)) return true;
    }
    return false;
}

std::uint32_t extractRuns(const std::uint32_t* bits, std::uint32_t rows, std::uint32_t cap, Run* out) {
    if (rows == 0 || cap == 0) return 0;
    const auto get = [&](std::uint32_t i) { return (bits[i >> 5] >> (i & 31u) & 1u) != 0; };
    std::uint32_t last = ~0u;
    for (std::uint32_t i = rows; i-- > 0;) {
        if (get(i)) {
            last = i;
            break;
        }
    }
    if (last == ~0u) return 0;
    std::uint32_t n = 0, p = 0;
    while (n < cap && p <= last) {
        std::uint32_t s = p;
        while (!get(s)) ++s;  // (exists: p <= last)
        std::uint32_t e;
        if (n == cap - 1) {
            e = last + 1;
        } else {
            e = s + 1;
            while (e < rows && get(e)) ++e;
        }
        out[n++] = {s, e};
        p = e;
    }
    return n;
}

}  // namespace rvk::clustercull
