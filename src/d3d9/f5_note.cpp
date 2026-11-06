// F5-0 measurements, the Device side (docs/research/frame-ownership.md 7.22; core/f5_measure.h). Called per draw from prepareDraw while RVK_F5_MEASURE=1.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "core/f5_measure.h"
#include "d3d9/device.h"
#include "engine/memread.h"

namespace rvk::d3d9 {

namespace {

using Key = std::array<std::uint64_t, 6>;
struct KeyHash {
    std::size_t operator()(const Key& k) const {
        std::uint64_t h = 0x9E3779B97F4A7C15ull;
        for (std::uint64_t v : k) h = (h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2))) * 0xff51afd7ed558ccdull;
        return static_cast<std::size_t>(h);
    }
};

struct Entry {
    bool bad = false;
    bool large = false;
    f5::Mesh mesh;
    std::array<f5::ClusterSet, f5::kVariants> cs;
    std::array<bool, f5::kVariants> built{};
    std::size_t bytes = 0;
};

std::unordered_map<Key, std::unique_ptr<Entry>, KeyHash>& cache() {
    static std::unordered_map<Key, std::unique_ptr<Entry>, KeyHash> c;
    return c;
}

constexpr std::size_t kCacheLimit = 768ull << 20;
constexpr std::uint32_t kLargeTris = 1024;

std::uint64_t nowNs() { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }

void add(f5::ClusterEval& a, const f5::ClusterEval& b) {
    a.tris += b.tris;
    a.triF += b.triF;
    a.clusters += b.clusters;
    a.clustersF += b.clustersF;
    for (int s = 0; s < 2; ++s) {
        a.triBackOnly[s] += b.triBackOnly[s];
        a.clustersKept[s] += b.clustersKept[s];
        a.keptTris[s] += b.keptTris[s];
        for (int o = 0; o < 2; ++o) a.runs[s][o] += b.runs[s][o];
    }
}

void add(f5::TriExact& a, const f5::TriExact& b) {
    a.tris += b.tris;
    a.frustumOut += b.frustumOut;
    a.backface += b.backface;
    a.nearClipped += b.nearClipped;
    a.visible += b.visible;
    for (int i = 0; i < f5::kHistBins; ++i) a.hist[i] += b.hist[i];
    for (int s = 0; s < 2; ++s) {
        a.coneBackClusters[s] += b.coneBackClusters[s];
        a.coneViolations[s] += b.coneViolations[s];
    }
}

}  // namespace

void Device::f5Note(std::uint32_t primitiveType, const DrawGeom& geom, draws::Kind kind, scene::Phase phase) {
    using namespace f5;
    Counters& c = counters();
    const std::uint64_t frame = stats_.frames;
    if (frame != c.curFrame) {
        if (c.curFrame != ~0ull) closeLodFrame();
        c.curFrame = frame;
        c.frameKeyMask = 0;
        if ((frame & 3) == 0) ++c.sampledFrames;
        const std::uint64_t now = nowNs();
        if (c.lastReportNs == 0) c.lastReportNs = now;
        if (now - c.lastReportNs >= 5'000'000'000ull) {
            report();
            c.lastReportNs = now;
        }
    }
    int key = 4;
    switch (phase.view) {
    case scene::View::Main:
        key = phase.pass == scene::Pass::DepthPrepass ? 0
              : phase.pass == scene::Pass::Solid      ? 1
              : (phase.pass == scene::Pass::Refracted || phase.pass == scene::Pass::Alpha || phase.pass == scene::Pass::AlphaLate) ? 2
                                                      : 4;
        break;
    case scene::View::Shadow: {
        const std::uint32_t idx = phases_.viewsEntered(scene::View::Shadow);
        key = phase.pass == scene::Pass::ShadowDepth ? 5 + static_cast<int>(std::min<std::uint32_t>(idx > 0 ? idx - 1 : 0, kViewKeys - 6)) : 4;
        break;
    }
    case scene::View::SkyCube: key = 3; break;
    default: break;
    }
    ViewAgg& a = c.view[key];
    if ((c.frameKeyMask & (1u << key)) == 0) {
        c.frameKeyMask |= 1u << key;
        ++a.frames;
    }
    const bool triangles = primitiveType == 4 || primitiveType == 5 || primitiveType == 6;
    const std::uint32_t inst = std::max<std::uint32_t>(1, instanceCount_);
    const std::uint32_t perInstance = triangles ? geom.primitives : 0;
    const std::uint64_t tris = static_cast<std::uint64_t>(perInstance) * inst;
    ++a.draws;
    a.tris += tris;
    a.indexCount += triangles && geom.indexed ? tris * 3 : 0;
    a.vertexRange += geom.indexed ? static_cast<std::uint64_t>(geom.numVertices) * inst : static_cast<std::uint64_t>(geom.primitives) * 3 * inst;
    if (inst > 1) a.instancedTris += tris;
    const int cls = sizeClass(perInstance);
    ++a.classDraws[cls];
    a.classTris[cls] += tris;
    const int k = std::min<int>(static_cast<int>(kind), kKinds - 1);
    ++a.kindDraws[k];
    a.kindTris[k] += tris;
    if (a.vpW == 0) {
        a.vpW = viewport_.width;
        a.vpH = viewport_.height;
    }

    if (kind != draws::Kind::StaticMesh || !geom.indexed || geom.userPointer || primitiveType != 4 || inst != 1 || indices_ == nullptr || streams_[0].buffer == nullptr) return;
    const bool mainZ = key == 0, mainSolid = key == 1, shadow = key >= 5;
    if (!mainZ && !mainSolid && !shadow) return;

    // LOD memory: the main view's contexts (res + 0x2c = the engine's byte accounting of a loaded resource, res + 0x38 its state, 3 = loaded)
    if ((mainZ || mainSolid) && tag_.context != nullptr && c.frameCtx.insert(tag_.context).second) {
        const void* ps = nullptr;
        std::uint8_t n = 0;
        const void* chosenRes = nullptr;
        engine::readAt(tag_.context, 0x28, chosenRes);  // ctx + 0x28 = the LOD resource UpdateLoD picked
        if (engine::readAt(tag_.context, 0x18, ps) && ps != nullptr && engine::readAt(ps, 0x6c, n) && c.frameLods.find(ps) == c.frameLods.end()) {
            Counters::LodSnap s;
            s.n = static_cast<std::uint8_t>(std::min<int>(n, 4));
            if (n <= 1) {
                s.n = 1;
                const void* res = chosenRes != nullptr ? chosenRes : tag_.meshResource;
                std::uint32_t st = 0, sz = 0;
                if (res != nullptr && engine::readAt(res, 0x38, st) && engine::readAt(res, 0x2c, sz)) {
                    s.size[0] = sz;
                    s.loaded[0] = st == 3;
                    s.chosen = 0;
                }
            } else {
                for (int b = 0; b < s.n; ++b) {
                    const void* res = nullptr;
                    std::uint32_t st = 0, sz = 0;
                    if (!engine::readAt(ps, 0x40 + 8u * static_cast<std::size_t>(b), res) || res == nullptr) continue;
                    if (!engine::readAt(res, 0x38, st) || !engine::readAt(res, 0x2c, sz)) continue;
                    s.size[b] = sz;
                    s.loaded[b] = st == 3;
                    if (res == chosenRes) s.chosen = static_cast<std::int8_t>(b);
                }
            }
            c.frameLods.emplace(ps, s);
        }
    }
    if (mainZ || mainSolid) {
        c.frameBuffers[streams_[0].buffer->id()] = streams_[0].buffer->size();
        c.frameBuffers[indices_->id()] = indices_->size();
    }

    // geometry analysis: rigid static draws with POSITION as FLOAT3 at offset 0 of stream 0
    const bool large = geom.primitives >= kLargeTris;
    if (shadow && c.shadowMarkFrame != frame) {
        c.shadowMarkFrame = frame;
        ++c.shadowSeq;
    }
    const bool exactFrame = (mainZ && (frame & 3) == 0) || (shadow && large && (c.shadowSeq & 1) == 0);
    if (!large && !exactFrame) return;
    if (!large && !mainZ) return;
    Buffer* vb = streams_[0].buffer;
    Buffer* ib = indices_;
    if (vb->ringCopied() || ib->ringCopied() || streams_[0].stride < 12 || decl_ == nullptr) return;
    bool posOk = false;
    for (const VertexElement& e : decl_->elements()) {
        if (e.stream == 0 && e.offset == 0 && e.usage == 0 && e.usageIndex == 0 && e.type == 2) posOk = true;
    }
    // a rigid material vertex shader: g_vInstanceData at c32 (world rows c33..c35), no skinning / SpeedTree / vegetation / instancing
    const ShaderSemantics& sem = vs_->semantics;
    using S = ShaderSemantics;
    if ((sem.flags & S::Material) == 0 || (sem.flags & (S::Skinned | S::Instanced | S::SpeedTree | S::Vegetation)) != 0 || !sem[Semantic::InstanceData].present() ||
        sem[Semantic::InstanceData].reg != 32)
        return;
    if (!posOk || geom.numVertices == 0 || geom.baseVertex + static_cast<std::int64_t>(geom.minIndex) < 0) return;

    const ResourceRef vr = vb->ref(), ir = ib->ref();
    const Key ck = {vr.id ^ (static_cast<std::uint64_t>(vr.generation) << 48), ir.id ^ (static_cast<std::uint64_t>(ir.generation) << 48),
                    (static_cast<std::uint64_t>(geom.first) << 32) | geom.primitives,
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(geom.baseVertex)) << 32) | geom.minIndex,
                    (static_cast<std::uint64_t>(streams_[0].offset) << 32) | streams_[0].stride, geom.numVertices | (ib->index32() ? 1ull << 40 : 0ull)};
    auto& cc = cache();
    auto it = cc.find(ck);
    if (it == cc.end()) {
        auto e = std::make_unique<Entry>();
        const std::uint32_t isz = ib->index32() ? 4 : 2;
        const std::uint64_t vbytes = static_cast<std::uint64_t>(geom.numVertices) * streams_[0].stride;
        if (vbytes > (96ull << 20) || static_cast<std::uint64_t>(geom.primitives) * 12 > (96ull << 20)) {
            e->bad = true;
        } else {
            std::vector<std::uint8_t> ibytes(static_cast<std::size_t>(geom.primitives) * 3 * isz), vbytesBuf(static_cast<std::size_t>(vbytes));
            const std::uint32_t firstVertex = static_cast<std::uint32_t>(geom.baseVertex + static_cast<std::int64_t>(geom.minIndex));
            if (!ib->readCpu(geom.first * isz, static_cast<std::uint32_t>(ibytes.size()), ibytes.data()) ||
                !vb->readCpu(streams_[0].offset + firstVertex * streams_[0].stride, static_cast<std::uint32_t>(vbytes), vbytesBuf.data())) {
                e->bad = true;
            } else {
                e->mesh.idx.resize(static_cast<std::size_t>(geom.primitives) * 3);
                for (std::size_t i = 0; i < e->mesh.idx.size() && !e->bad; ++i) {
                    std::uint32_t v = 0;
                    if (isz == 4) std::memcpy(&v, &ibytes[i * 4], 4);
                    else {
                        std::uint16_t w = 0;
                        std::memcpy(&w, &ibytes[i * 2], 2);
                        v = w;
                    }
                    if (v < geom.minIndex || v - geom.minIndex >= geom.numVertices) e->bad = true;
                    else e->mesh.idx[i] = v - geom.minIndex;
                }
                e->mesh.pos.resize(static_cast<std::size_t>(geom.numVertices) * 3);
                for (std::uint32_t v = 0; v < geom.numVertices; ++v) {
                    std::memcpy(&e->mesh.pos[3 * v], &vbytesBuf[static_cast<std::size_t>(v) * streams_[0].stride], 12);
                    for (int j = 0; j < 3; ++j)
                        if (!std::isfinite(e->mesh.pos[3 * v + j])) e->bad = true;
                }
            }
        }
        e->large = large;
        e->bytes = e->mesh.pos.size() * 4 + e->mesh.idx.size() * 4;
        if (c.exactMeshBytes + e->bytes > kCacheLimit) {
            ++c.buildSkipped;
            e->bad = true;
            e->mesh = {};
            e->bytes = 0;
        }
        c.exactMeshBytes += e->bytes;
        it = cc.emplace(ck, std::move(e)).first;
    }
    Entry& e = *it->second;
    if (e.bad) {
        if (exactFrame) ++c.exactBad;
        return;
    }
    // the draw's object-to-clip data: VS c0..c3 and the world rows c33..c35
    float regs[4][4];
    for (int r = 0; r < 4; ++r) std::memcpy(regs[r], vsConstant(static_cast<std::uint32_t>(r)), sizeof(float) * 4);
    float world[12];
    for (int r = 0; r < 3; ++r) std::memcpy(&world[r * 4], vsConstant(33u + static_cast<std::uint32_t>(r)), sizeof(float) * 4);
    ObjView ov;
    if (!makeObjView(regs, world, static_cast<int>(rs_[RS_CULLMODE]), static_cast<float>(viewport_.width), static_cast<float>(viewport_.height), ov)) {
        if (exactFrame) ++c.exactBad;
        return;
    }
    if (mainZ && camera_.valid && c.vpMatchChecks < 100000 && (frame & 3) == 0) {
        float d = 0.0f;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) d = std::max(d, std::fabs(regs[i][j] - camera_.viewProjection.at(j, i)));
        ++c.vpMatchChecks;
        if (d > 0.01f * std::max(1.0f, std::fabs(camera_.viewProjection.at(0, 0)))) ++c.vpMatchBad;
        c.vpMatchMax = std::max<double>(c.vpMatchMax, d);
    }
    if (ov.mirrored) ++a.mirrored;
    if (large) {
        for (int v = 0; v < kVariants; ++v) {
            if (!e.built[static_cast<std::size_t>(v)]) {
                const std::uint64_t t0 = nowNs();
                e.cs[static_cast<std::size_t>(v)] = buildClusters(e.mesh, static_cast<Variant>(v));
                e.built[static_cast<std::size_t>(v)] = true;
                ++c.builds;
                c.buildNs += nowNs() - t0;
                c.buildTris += e.mesh.triangles();
                e.bytes += e.cs[static_cast<std::size_t>(v)].tri.size() * 4 + e.cs[static_cast<std::size_t>(v)].list.size() * sizeof(Cluster);
                c.exactMeshBytes += e.cs[static_cast<std::size_t>(v)].tri.size() * 4 + e.cs[static_cast<std::size_t>(v)].list.size() * sizeof(Cluster);
            }
        }
        ++a.cDraws;
        for (int v = 0; v < kVariants; ++v) {
            ClusterEval ce;
            evalClusters(e.cs[static_cast<std::size_t>(v)], ov, ce);
            add(a.cev[v], ce);
            for (int s = 0; s < 2; ++s)
                if (ce.clustersKept[s] == 0) ++a.cDrawsAllOut[v][s];
        }
    }
    if (exactFrame) {
        ++c.exactDraws;
        TriExact te;
        if (large) {
            evalExact(e.mesh, e.cs[0].tri.data(), e.cs[0].tri.size() / 3, ov, te, &e.cs[0]);
            for (int v = 0; v < kVariants; ++v) {
                ClusterEval ce;
                evalClusters(e.cs[static_cast<std::size_t>(v)], ov, ce);
                add(shadow ? c.sampledShadow[v] : c.sampled[v], ce);
            }
            add(shadow ? c.exactShadow : c.exactLarge, te);
        } else {
            evalExact(e.mesh, e.mesh.idx.data(), e.mesh.idx.size() / 3, ov, te, nullptr);
        }
        if (!shadow) add(c.exactAll, te);
    }
}

}  // namespace rvk::d3d9
