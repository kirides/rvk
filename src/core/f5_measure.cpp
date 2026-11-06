#include "core/f5_measure.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>

#include <meshoptimizer.h>
#include <spdlog/spdlog.h>

extern "C" __declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char* name, char* buffer, unsigned long size);

namespace rvk::f5 {

bool enabled() {
    static const bool on = [] {
        char v[8] = {};
        return GetEnvironmentVariableA("RVK_F5_MEASURE", v, sizeof(v)) != 0 && v[0] == '1';
    }();
    return on;
}

Counters& counters() {
    static Counters c;
    return c;
}

// ---- geometry -------------------------------------------------------------------------------------------------------------------

namespace {

std::uint32_t expandBits(std::uint32_t v) {
    v = (v * 0x00010001u) & 0xFF0000FFu;
    v = (v * 0x00000101u) & 0x0F00F00Fu;
    v = (v * 0x00000011u) & 0xC30C30C3u;
    v = (v * 0x00000005u) & 0x49249249u;
    return v;
}

float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

}  // namespace

ClusterSet buildClusters(const Mesh& mesh, Variant variant) {
    ClusterSet set;
    const std::size_t indexCount = mesh.idx.size();
    const std::size_t vertexCount = mesh.vertices();
    if (indexCount < 3 || vertexCount < 3) return set;
    constexpr std::size_t kMaxV = 128, kMaxT = 128, kMinT = 64;
    const std::size_t bound = meshopt_buildMeshletsBound(indexCount, kMaxV, variant == kSpatial ? kMinT : kMaxT);
    std::vector<meshopt_Meshlet> meshlets(bound);
    std::vector<unsigned int> mv(bound * kMaxV);
    std::vector<unsigned char> mt(bound * kMaxT * 3);
    std::size_t count = 0;
    if (variant == kSpatial) {
        count = meshopt_buildMeshletsSpatial(meshlets.data(), mv.data(), mt.data(), mesh.idx.data(), indexCount, mesh.pos.data(), vertexCount, 3 * sizeof(float), kMaxV, kMinT, kMaxT, 0.5f);
    } else {
        count = meshopt_buildMeshlets(meshlets.data(), mv.data(), mt.data(), mesh.idx.data(), indexCount, mesh.pos.data(), vertexCount, 3 * sizeof(float), kMaxV, kMaxT, 0.5f);
    }
    set.list.reserve(count);
    set.tri.reserve(indexCount);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (std::size_t i = 0; i < count; ++i) {
        const meshopt_Meshlet& m = meshlets[i];
        const meshopt_Bounds b = meshopt_computeMeshletBounds(&mv[m.vertex_offset], &mt[m.triangle_offset], m.triangle_count, mesh.pos.data(), vertexCount, 3 * sizeof(float));
        Cluster c;
        std::memcpy(c.c, b.center, sizeof(c.c));
        c.r = b.radius;
        std::memcpy(c.axis, b.cone_axis, sizeof(c.axis));
        c.cutoff = b.cone_cutoff;
        c.firstTri = static_cast<std::uint32_t>(set.tri.size() / 3);
        c.tris = m.triangle_count;
        for (unsigned t = 0; t < m.triangle_count; ++t) {
            for (int k = 0; k < 3; ++k) set.tri.push_back(mv[m.vertex_offset + mt[m.triangle_offset + 3 * t + k]]);
        }
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], c.c[k]);
            hi[k] = std::max(hi[k], c.c[k]);
        }
        set.list.push_back(c);
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> keys(set.list.size());
    for (std::size_t i = 0; i < set.list.size(); ++i) {
        std::uint32_t q[3];
        for (int k = 0; k < 3; ++k) {
            const float ext = hi[k] - lo[k];
            const float t = ext > 1e-6f ? (set.list[i].c[k] - lo[k]) / ext : 0.0f;
            q[k] = static_cast<std::uint32_t>(std::clamp(t, 0.0f, 1.0f) * 1023.0f);
        }
        keys[i] = {(expandBits(q[0]) << 2) | (expandBits(q[1]) << 1) | expandBits(q[2]), static_cast<std::uint32_t>(i)};
    }
    std::sort(keys.begin(), keys.end());
    set.mortonOrder.reserve(keys.size());
    for (const auto& k : keys) set.mortonOrder.push_back(k.second);
    return set;
}

bool makeObjView(const float regs[4][4], const float world[12], int cull, float vpW, float vpH, ObjView& out) {
    out = ObjView{};
    out.cull = cull;
    out.vpW = vpW;
    out.vpH = vpH;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (!std::isfinite(regs[i][j])) return false;
    for (int i = 0; i < 12; ++i)
        if (!std::isfinite(world[i])) return false;
    const float* w0 = world;
    const float* w1 = world + 4;
    const float* w2 = world + 8;
    // object-to-clip rows
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) out.clip[i][j] = regs[i][0] * w0[j] + regs[i][1] * w1[j] + regs[i][2] * w2[j] + (j == 3 ? regs[i][3] : 0.0f);
    }
    // world-space frustum planes from the rows (inside: >= 0): left / right / bottom / top / near / far
    float wp[6][4];
    for (int j = 0; j < 4; ++j) {
        wp[0][j] = regs[3][j] + regs[0][j];
        wp[1][j] = regs[3][j] - regs[0][j];
        wp[2][j] = regs[3][j] + regs[1][j];
        wp[3][j] = regs[3][j] - regs[1][j];
        wp[4][j] = regs[2][j];
        wp[5][j] = regs[3][j] - regs[2][j];
    }
    for (int p = 0; p < 6; ++p) {
        const float len = std::sqrt(dot3(wp[p], wp[p]));
        if (!(len > 1e-12f)) {
            // a degenerate plane (never for a real projection): keep it open
            for (int j = 0; j < 4; ++j) out.planes[p][j] = 0.0f;
            out.planes[p][3] = 1.0f;
            out.normalLen[p] = 0.0f;
            continue;
        }
        for (int j = 0; j < 4; ++j) wp[p][j] /= len;
        // object space: n_o = A^T n_w, d_o = n_w . t + d_w
        for (int j = 0; j < 3; ++j) out.planes[p][j] = wp[p][0] * w0[j] + wp[p][1] * w1[j] + wp[p][2] * w2[j];
        out.planes[p][3] = wp[p][0] * w0[3] + wp[p][1] * w1[3] + wp[p][2] * w2[3] + wp[p][3];
        out.normalLen[p] = std::sqrt(dot3(out.planes[p], out.planes[p]));
    }
    // A^-1
    const float a[9] = {w0[0], w0[1], w0[2], w1[0], w1[1], w1[2], w2[0], w2[1], w2[2]};
    const float det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
    if (!(std::fabs(det) > 1e-20f)) return false;
    out.mirrored = det < 0.0f;
    const float inv[9] = {(a[4] * a[8] - a[5] * a[7]) / det, (a[2] * a[7] - a[1] * a[8]) / det, (a[1] * a[5] - a[2] * a[4]) / det,
                          (a[5] * a[6] - a[3] * a[8]) / det, (a[0] * a[8] - a[2] * a[6]) / det, (a[2] * a[3] - a[0] * a[5]) / det,
                          (a[3] * a[7] - a[4] * a[6]) / det, (a[1] * a[6] - a[0] * a[7]) / det, (a[0] * a[4] - a[1] * a[3]) / det};
    // eye in world: clip x = y = w = 0 (rows 0, 1, 3); ortho when rows 0 / 1 / 3 are singular in xyz
    const float* r0 = regs[0];
    const float* r1 = regs[1];
    const float* r3 = regs[3];
    const float d3 = r0[0] * (r1[1] * r3[2] - r1[2] * r3[1]) - r0[1] * (r1[0] * r3[2] - r1[2] * r3[0]) + r0[2] * (r1[0] * r3[1] - r1[1] * r3[0]);
    const float scale = std::sqrt(dot3(r0, r0)) * std::sqrt(dot3(r1, r1)) * std::sqrt(dot3(r3, r3));
    float eyeW[3] = {}, dirW[3] = {};
    if (std::fabs(d3) > 1e-6f * std::max(scale, 1e-20f)) {
        const float b[3] = {-r0[3], -r1[3], -r3[3]};
        // Cramer's rule
        const float m[3][3] = {{r0[0], r0[1], r0[2]}, {r1[0], r1[1], r1[2]}, {r3[0], r3[1], r3[2]}};
        for (int k = 0; k < 3; ++k) {
            float mm[3][3];
            std::memcpy(mm, m, sizeof(m));
            for (int row = 0; row < 3; ++row) mm[row][k] = b[row];
            const float dk = mm[0][0] * (mm[1][1] * mm[2][2] - mm[1][2] * mm[2][1]) - mm[0][1] * (mm[1][0] * mm[2][2] - mm[1][2] * mm[2][0]) +
                             mm[0][2] * (mm[1][0] * mm[2][1] - mm[1][1] * mm[2][0]);
            eyeW[k] = dk / d3;
        }
        out.ortho = false;
    } else {
        out.ortho = true;
        const float* r2 = regs[2];
        const float l = std::sqrt(dot3(r2, r2));
        if (!(l > 1e-12f)) return false;
        for (int k = 0; k < 3; ++k) dirW[k] = r2[k] / l;
    }
    // to object space: p_o = A^-1 (p_w - t); directions: A^-1 d
    const float tW[3] = {w0[3], w1[3], w2[3]};
    if (!out.ortho) {
        const float d[3] = {eyeW[0] - tW[0], eyeW[1] - tW[1], eyeW[2] - tW[2]};
        for (int i = 0; i < 3; ++i) out.eye[i] = inv[3 * i] * d[0] + inv[3 * i + 1] * d[1] + inv[3 * i + 2] * d[2];
    } else {
        for (int i = 0; i < 3; ++i) out.dir[i] = inv[3 * i] * dirW[0] + inv[3 * i + 1] * dirW[1] + inv[3 * i + 2] * dirW[2];
    }
    out.valid = true;
    return true;
}

bool sphereOutside(const ObjView& v, const float c[3], float r) {
    for (int p = 0; p < 6; ++p) {
        const float d = dot3(v.planes[p], c) + v.planes[p][3];
        if (d < -r * v.normalLen[p]) return true;
    }
    return false;
}

bool coneBackFacing(const ObjView& v, const Cluster& cl, int sign) {
    if (v.cull == 1) return false;  // double-sided: nothing is back-facing
    float s = static_cast<float>(sign);
    if (v.mirrored) s = -s;
    if (v.cull == 2) s = -s;
    if (v.ortho) return s * dot3(v.dir, cl.axis) >= cl.cutoff;
    const float d[3] = {cl.c[0] - v.eye[0], cl.c[1] - v.eye[1], cl.c[2] - v.eye[2]};
    const float len = std::sqrt(dot3(d, d));
    return s * dot3(d, cl.axis) >= cl.cutoff * len + cl.r;
}

void evalClusters(const ClusterSet& set, const ObjView& v, ClusterEval& out) {
    const std::size_t n = set.list.size();
    std::vector<std::uint8_t> state(n * 2);  // 0 kept, 1 frustum, 2 back
    for (std::size_t i = 0; i < n; ++i) {
        const Cluster& c = set.list[i];
        out.tris += c.tris;
        ++out.clusters;
        const bool f = sphereOutside(v, c.c, c.r);
        if (f) {
            out.triF += c.tris;
            ++out.clustersF;
        }
        for (int s = 0; s < 2; ++s) {
            const bool b = !f && coneBackFacing(v, c, s == 0 ? 1 : -1);
            state[i * 2 + s] = f ? 1 : b ? 2 : 0;
            if (b) out.triBackOnly[s] += c.tris;
            if (!f && !b) {
                out.keptTris[s] += c.tris;
                ++out.clustersKept[s];
            }
        }
    }
    for (int s = 0; s < 2; ++s) {
        for (int o = 0; o < 2; ++o) {
            bool prev = false;
            for (std::size_t k = 0; k < n; ++k) {
                const std::size_t i = o == 0 ? k : set.mortonOrder[k];
                const bool vis = state[i * 2 + s] == 0;
                if (vis && !prev) ++out.runs[s][o];
                prev = vis;
            }
        }
    }
}

void evalExact(const Mesh& mesh, const std::uint32_t* tris, std::size_t triCount, const ObjView& v, TriExact& out, const ClusterSet* set) {
    const std::size_t vc = mesh.vertices();
    struct Clip {
        float x, y, w;
        std::uint8_t code;
    };
    std::vector<Clip> clip(vc);
    for (std::size_t i = 0; i < vc; ++i) {
        const float p[4] = {mesh.pos[3 * i], mesh.pos[3 * i + 1], mesh.pos[3 * i + 2], 1.0f};
        float c[4];
        for (int r = 0; r < 4; ++r) c[r] = v.clip[r][0] * p[0] + v.clip[r][1] * p[1] + v.clip[r][2] * p[2] + v.clip[r][3];
        std::uint8_t code = 0;
        if (c[0] < -c[3]) code |= 1;
        if (c[0] > c[3]) code |= 2;
        if (c[1] < -c[3]) code |= 4;
        if (c[1] > c[3]) code |= 8;
        if (c[2] < 0.0f) code |= 16;
        if (c[2] > c[3]) code |= 32;
        clip[i] = {c[0], c[1], c[3], code};
    }
    const float pixScale = 0.5f * (v.vpW * 0.5f) * (v.vpH * 0.5f);
    std::vector<std::uint8_t> frontInside(set != nullptr ? triCount : 0);
    for (std::size_t t = 0; t < triCount; ++t) {
        const Clip& a = clip[tris[3 * t]];
        const Clip& b = clip[tris[3 * t + 1]];
        const Clip& c = clip[tris[3 * t + 2]];
        ++out.tris;
        if ((a.code & b.code & c.code) != 0) {
            ++out.frustumOut;
            continue;
        }
        if (a.w <= 1e-5f || b.w <= 1e-5f || c.w <= 1e-5f) {
            ++out.nearClipped;
            ++out.hist[kHistBins - 1];
            continue;
        }
        const float x0 = a.x / a.w, y0 = a.y / a.w, x1 = b.x / b.w, y1 = b.y / b.w, x2 = c.x / c.w, y2 = c.y / c.w;
        const float area2 = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);  // > 0: counter-clockwise on screen (y up)
        const bool culled = v.cull == 3 ? area2 >= 0.0f : v.cull == 2 ? area2 <= 0.0f : area2 == 0.0f;
        if (culled) {
            ++out.backface;
            continue;
        }
        ++out.visible;
        const float px = std::fabs(area2) * pixScale;
        const int bin = px < 1.0f ? 0 : px < 4.0f ? 1 : px < 16.0f ? 2 : px < 64.0f ? 3 : 4;
        ++out.hist[bin];
        if (set != nullptr && (a.code | b.code | c.code) == 0) frontInside[t] = 1;
    }
    if (set != nullptr && tris == set->tri.data()) {
        for (const Cluster& cl : set->list) {
            if (sphereOutside(v, cl.c, cl.r)) continue;
            for (int s = 0; s < 2; ++s) {
                if (!coneBackFacing(v, cl, s == 0 ? 1 : -1)) continue;
                ++out.coneBackClusters[s];
                bool bad = false;
                for (std::uint32_t t = cl.firstTri; t < cl.firstTri + cl.tris && !bad; ++t) bad = frontInside[t] != 0;
                if (bad) ++out.coneViolations[s];
            }
        }
    }
}

// ---- report ---------------------------------------------------------------------------------------------------------------------

namespace {

const char* keyName(int k) {
    static const char* const names[] = {"main z", "main solid", "main alpha", "sky cube", "other"};
    static char buf[16];
    if (k < 5) return names[k];
    std::snprintf(buf, sizeof(buf), "shadow %d", k - 5);
    return buf;
}

double pct(std::uint64_t a, std::uint64_t b) { return b != 0 ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; }
double per(std::uint64_t a, std::uint64_t n) { return n != 0 ? static_cast<double>(a) / static_cast<double>(n) : 0.0; }

}  // namespace

void closeLodFrame() {
    Counters& c = counters();
    c.frameCtx.clear();
    if (c.frameLods.empty()) {
        c.frameBuffers.clear();
        return;
    }
    ++c.lodFrames;
    for (const auto& [id, bytes] : c.frameBuffers) c.drawnBufferBytesSum += bytes;
    c.frameBuffers.clear();
    for (const auto& [ps, s] : c.frameLods) {
        ++c.lodPs;
        if (s.n > 1) ++c.lodPsMulti;
        if (s.n > 1 && s.n <= 4 && s.loaded[0] != 0) {
            for (int b = 1; b < s.n; ++b) {
                if (s.loaded[b] != 0 && s.size[0] != 0) {
                    c.ratioSum[s.n][b] += static_cast<double>(s.size[b]) / static_cast<double>(s.size[0]);
                    ++c.ratioCnt[s.n][b];
                }
            }
        }
    }
    // memory: chosen LODs / loaded siblings / every sibling resident (unloaded ones from the running size ratios)
    const auto avg = [&](int n, int b) {
        if (b == 0) return 1.0;
        return c.ratioCnt[n][b] != 0 ? c.ratioSum[n][b] / static_cast<double>(c.ratioCnt[n][b]) : 0.0;
    };
    for (const auto& [ps, s] : c.frameLods) {
        if (s.n < 1 || s.n > 4) continue;
        int ref = s.chosen >= 0 ? s.chosen : 0;
        if (s.loaded[ref] == 0) {
            for (int b = 0; b < s.n; ++b)
                if (s.loaded[b] != 0) {
                    ref = b;
                    break;
                }
        }
        if (s.chosen >= 0) c.chosenBytesSum += s.size[s.chosen];
        for (int b = 0; b < s.n; ++b) {
            ++c.siblingsTotal;
            if (s.loaded[b] != 0) {
                c.loadedBytesSum += s.size[b];
                c.estAllBytesSum += s.size[b];
            } else {
                ++c.unloadedSiblings;
                const double ra = avg(s.n, ref), rb = avg(s.n, b);
                if (s.loaded[ref] != 0 && ra > 0.0 && rb > 0.0) c.estAllBytesSum += static_cast<std::uint64_t>(s.size[ref] * rb / ra);
            }
        }
    }
    c.lastLods.swap(c.frameLods);
    c.frameLods.clear();
}

void report() {
    Counters& c = counters();
    for (int k = 0; k < kViewKeys; ++k) {
        const ViewAgg& a = c.view[k];
        if (a.frames == 0) continue;
        std::string cls;
        for (int i = 0; i < kClasses; ++i) {
            static const char* const nm[kClasses] = {"<256", "256-1K", "1K-4K", "4K-16K", ">16K"};
            cls += std::format(" {} {:.0f}d/{:.0f}t", nm[i], per(a.classDraws[i], a.frames), per(a.classTris[i], a.frames));
        }
        std::string kinds;
        for (int i = 0; i < kKinds; ++i) {
            if (a.kindTris[i] != 0) kinds += std::format(" k{} {:.0f}d/{:.0f}t", i, per(a.kindDraws[i], a.frames), per(a.kindTris[i], a.frames));
        }
        spdlog::info("native: f5 measure (5 s) [{}] {} frames: {:.0f} draws, {:.0f} triangles ({:.0f} in instanced draws), {:.0f} index entries, vertex ranges {:.0f} per frame; "
                     "per frame by triangles per draw:{}; by draw kind:{}; viewport {}x{}",
                     keyName(k), a.frames, per(a.draws, a.frames), per(a.tris, a.frames), per(a.instancedTris, a.frames), per(a.indexCount, a.frames), per(a.vertexRange, a.frames), cls,
                     kinds, a.vpW, a.vpH);
        if (a.cDraws != 0) {
            for (int v = 0; v < kVariants; ++v) {
                const ClusterEval& e = a.cev[v];
                const char* vn = v == 0 ? "meshopt" : "spatial";
                std::string sg;
                for (int s = 0; s < 2; ++s) {
                    sg += std::format(" [cone sign {}: back-only {:.1f} %, either {:.1f} %, kept clusters {:.1f} %, runs/draw {:.2f} (builder order) {:.2f} (morton)]", s == 0 ? "+" : "-",
                                      pct(e.triBackOnly[s], e.tris), pct(e.triF + e.triBackOnly[s], e.tris), pct(e.clustersKept[s], e.clusters), per(e.runs[s][0], a.cDraws),
                                      per(e.runs[s][1], a.cDraws));
                }
                spdlog::info("native: f5 clusters [{}] {}: {:.1f} large static draws per frame, {:.0f} triangles ({:.0f} clusters, {:.1f} per draw): outside the frustum {:.1f} %,{}; draws all-out {:.1f} %",
                             keyName(k), vn, per(a.cDraws, a.frames), per(e.tris, a.frames), per(e.clusters, a.frames), per(e.clusters, a.cDraws), pct(e.triF, e.tris), sg,
                             pct(a.cDrawsAllOut[v][0], a.cDraws));
            }
        }
    }
    {
        const auto line = [](const char* what, const TriExact& e) {
            if (e.tris == 0) return;
            static const char* const bn[kHistBins] = {"<1", "1-4", "4-16", "16-64", ">64"};
            std::string h;
            for (int i = 0; i < kHistBins; ++i) h += std::format(" {} px {:.1f} %", bn[i], pct(e.hist[i], e.visible + e.nearClipped));
            spdlog::info("native: f5 exact [{}]: {} triangles: frustum-out {:.1f} %, back-facing {:.1f} %, near-clipped {:.1f} %, drawn {:.1f} %; of the drawn:{}; cone verdicts: +: {} back clusters, {} violated, -: {} back clusters, {} violated",
                         what, e.tris, pct(e.frustumOut, e.tris), pct(e.backface, e.tris), pct(e.nearClipped, e.tris), pct(e.visible, e.tris), h, e.coneBackClusters[0], e.coneViolations[0],
                         e.coneBackClusters[1], e.coneViolations[1]);
        };
        line("main z, all rigid draws", c.exactAll);
        line("main z, large static draws", c.exactLarge);
        const auto cmp = [&](const char* what, const TriExact& e, const ClusterEval* ce) {
            if (e.tris == 0) return;
            for (int v = 0; v < kVariants; ++v) {
                const ClusterEval& x = ce[v];
                spdlog::info("native: f5 exact vs clusters [{}, {}]: exact (per triangle, {} tris): frustum-out {:.1f} %, back-facing {:.1f} %; clusters of the same draws: sphere frustum-out {:.1f} %, cone back-only +: {:.1f} % -: {:.1f} %",
                             what, v == 0 ? "meshopt" : "spatial", e.tris, pct(e.frustumOut, e.tris), pct(e.backface, e.tris), pct(x.triF, x.tris), pct(x.triBackOnly[0], x.tris), pct(x.triBackOnly[1], x.tris));
            }
        };
        cmp("main z", c.exactLarge, c.sampled);
        line("shadow views, large static draws", c.exactShadow);
        cmp("shadow", c.exactShadow, c.sampledShadow);
        if (c.exactAll.tris != 0) {
            spdlog::info("native: f5 exact: {} sampled frames, {} draws analysed, {} unusable, mesh cache {:.1f} MiB; cluster builds {} ({} triangles, {:.1f} ms), {} skipped (cache full); camera VP vs VS c0..c3: {} checks, {} bad, max |diff| {:.4g}",
                         c.sampledFrames, c.exactDraws, c.exactBad, static_cast<double>(c.exactMeshBytes) / 1048576.0, c.builds, c.buildTris, static_cast<double>(c.buildNs) * 1e-6,
                         c.buildSkipped, c.vpMatchChecks, c.vpMatchBad, c.vpMatchMax);
        }
    }
    if (c.lodFrames != 0) {
        const auto mib = [&](std::uint64_t v) { return static_cast<double>(v) / 1048576.0 / static_cast<double>(c.lodFrames); };
        spdlog::info("native: f5 lod memory (main view, {} frames): {:.0f} meshes per frame, {:.0f} with several LODs; resource bytes (engine accounting, res + 0x2c) of the chosen LODs {:.1f} MiB, "
                     "of every loaded sibling {:.1f} MiB, estimated with every sibling loaded {:.1f} MiB; siblings not loaded {} of {}; unique drawn vertex + index buffers {:.1f} MiB",
                     c.lodFrames, per(c.lodPs, c.lodFrames), per(c.lodPsMulti, c.lodFrames), mib(c.chosenBytesSum), mib(c.loadedBytesSum), mib(c.estAllBytesSum), c.unloadedSiblings,
                     c.siblingsTotal, mib(c.drawnBufferBytesSum));
        std::string r;
        for (int n = 2; n <= 4; ++n)
            for (int b = 1; b < n; ++b)
                if (c.ratioCnt[n][b] != 0) r += std::format(" n{} b{}: {:.3f} ({})", n, b, c.ratioSum[n][b] / static_cast<double>(c.ratioCnt[n][b]), c.ratioCnt[n][b]);
        spdlog::info("native: f5 lod size ratios (size_b / size_0, both loaded):{}", r.empty() ? " none" : r);
    }
    // the window restarts; the caches (mesh data, ratios, last LOD frame) carry on
    const auto keepLods = std::move(c.lastLods);
    const double rs[5][4] = {{c.ratioSum[0][0], c.ratioSum[0][1], c.ratioSum[0][2], c.ratioSum[0][3]}, {c.ratioSum[1][0], c.ratioSum[1][1], c.ratioSum[1][2], c.ratioSum[1][3]},
                             {c.ratioSum[2][0], c.ratioSum[2][1], c.ratioSum[2][2], c.ratioSum[2][3]}, {c.ratioSum[3][0], c.ratioSum[3][1], c.ratioSum[3][2], c.ratioSum[3][3]},
                             {c.ratioSum[4][0], c.ratioSum[4][1], c.ratioSum[4][2], c.ratioSum[4][3]}};
    std::uint64_t rc[5][4];
    std::memcpy(rc, c.ratioCnt, sizeof(rc));
    const std::uint64_t curFrame = c.curFrame, lastReport = c.lastReportNs, builds = c.builds, buildNs = c.buildNs, buildTris = c.buildTris, skipped = c.buildSkipped, meshBytes = c.exactMeshBytes;
    c = Counters{};
    c.lastLods = keepLods;
    std::memcpy(c.ratioCnt, rc, sizeof(rc));
    for (int n = 0; n < 5; ++n)
        for (int b = 0; b < 4; ++b) c.ratioSum[n][b] = rs[n][b];
    c.curFrame = curFrame;
    c.lastReportNs = lastReport;
    c.builds = builds;
    c.buildNs = buildNs;
    c.buildTris = buildTris;
    c.buildSkipped = skipped;
    c.exactMeshBytes = meshBytes;
}

}  // namespace rvk::f5
