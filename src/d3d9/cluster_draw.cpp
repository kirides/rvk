// F5-1 clusters, the device's half (docs/research/frame-ownership.md 7.33, setting native.clusters; gpuscene/cluster_table.h, core/cluster_build.h).
// drawIndexedPrimitive calls clusterApply for every indexed draw while the setting is on; it returns at once unless the draw is a static rigid opaque triangle-list
// draw of >= 1,024 triangles in a z / solid / shadow depth pass. For such a draw the range (vertex / index buffers, their copies' generations, the draw's own numbers) is
// looked up in the store: unknown = copies of the range's indices and positions go to the worker (the draw is drawn as the engine made it); built = the arena copy of the
// indices in cluster order is made and the packet is pointed at it. The vertex streams, base vertex and everything else of the draw are untouched.

#include <cmath>
#include <cstring>

#include "d3d9/device.h"
#include "gpuscene/cluster_table.h"

namespace rvk::d3d9 {

void Device::setClusters(bool on) {
    if (on == clusters_) return;
    clusters_ = on;
    gpuscene::clusterstore().setActive(*this, on);
}

void Device::clusterApply(std::uint32_t type, std::int32_t baseVertex, std::uint32_t minIndex, std::uint32_t numVertices, std::uint32_t startIndex, std::uint32_t primitives) {
    clusterWait_ = 0;
    clusterUsing_ = false;
    if (primitives < gpuscene::ClusterStore::kLargeTriangles || lastKind_ != draws::Kind::StaticMesh || instanceCount_ > 1 || indices_ == nullptr || numVertices == 0) return;
    Buffer* vb = streams_[0].buffer;
    Buffer* ib = indices_;
    if (vb == nullptr || vb->ringCopied() || ib->ringCopied() || streams_[0].stride < 12 || decl_ == nullptr || customVertices_ != 0 || up_.active) return;
    if (type != PT_TRIANGLELIST || rs_[RS_ALPHABLENDENABLE] != 0) return;
    const scene::Phase ph = phases_.current();
    const bool pass = (ph.view == scene::View::Main && (ph.pass == scene::Pass::DepthPrepass || ph.pass == scene::Pass::Solid)) ||
                      (ph.view == scene::View::Shadow && ph.pass == scene::Pass::ShadowDepth);
    if (!pass || baseVertex + static_cast<std::int64_t>(minIndex) < 0) return;
    // a rigid material vertex shader (g_vInstanceData at c32: world rows), POSITION as FLOAT3 at offset 0 of stream 0; no skinning / SpeedTree / vegetation / instancing
    const ShaderSemantics& sem = vs_->semantics;
    using S = ShaderSemantics;
    if ((sem.flags & S::Material) == 0 || (sem.flags & (S::Skinned | S::Instanced | S::SpeedTree | S::Vegetation)) != 0 || !sem[Semantic::InstanceData].present() ||
        sem[Semantic::InstanceData].reg != 32)
        return;
    bool posOk = false;
    for (const VertexElement& e : decl_->elements()) {
        if (e.stream == 0 && e.offset == 0 && e.usage == 0 && e.usageIndex == 0 && e.type == 2) posOk = true;
    }
    if (!posOk) return;

    gpuscene::ClusterStore& store = gpuscene::clusterstore();
    if (!store.active()) return;
    store.drain(*this);
    const bool index32 = ib->index32();
    const std::uint32_t isz = index32 ? 4 : 2;
    const ResourceRef vr = vb->ref(), ir = ib->ref();
    const gpuscene::ClusterStore::Key key = {vr.id ^ (static_cast<std::uint64_t>(vr.generation) << 48), ir.id ^ (static_cast<std::uint64_t>(ir.generation) << 48),
                                             (static_cast<std::uint64_t>(startIndex) << 32) | primitives,
                                             (static_cast<std::uint64_t>(static_cast<std::uint32_t>(baseVertex)) << 32) | minIndex,
                                             (static_cast<std::uint64_t>(streams_[0].offset) << 32) | streams_[0].stride, numVertices | (index32 ? 1ull << 40 : 0ull)};
    gpuscene::ClusterStore::Entry* e = store.find(key);
    if (e == nullptr) {
        // first sight: copies of the range's indices (as local ids) and positions go to the worker
        const std::uint64_t vbytes = static_cast<std::uint64_t>(numVertices) * streams_[0].stride;
        if (vbytes > (96ull << 20) || static_cast<std::uint64_t>(primitives) * 12 > (96ull << 20)) return;
        std::vector<std::uint8_t> ibytes(static_cast<std::size_t>(primitives) * 3 * isz), vbuf(static_cast<std::size_t>(vbytes));
        const std::uint32_t firstVertex = static_cast<std::uint32_t>(baseVertex + static_cast<std::int64_t>(minIndex));
        if (!ib->readCpu(startIndex * isz, static_cast<std::uint32_t>(ibytes.size()), ibytes.data()) ||
            !vb->readCpu(streams_[0].offset + firstVertex * streams_[0].stride, static_cast<std::uint32_t>(vbytes), vbuf.data())) {
            return;
        }
        std::vector<std::uint32_t> idx(static_cast<std::size_t>(primitives) * 3);
        bool bad = false;
        for (std::size_t i = 0; i < idx.size() && !bad; ++i) {
            std::uint32_t v = 0;
            if (isz == 4) {
                std::memcpy(&v, &ibytes[i * 4], 4);
            } else {
                std::uint16_t w = 0;
                std::memcpy(&w, &ibytes[i * 2], 2);
                v = w;
            }
            if (v < minIndex || v - minIndex >= numVertices) bad = true;
            else idx[i] = v - minIndex;
        }
        std::vector<float> pos(static_cast<std::size_t>(numVertices) * 3);
        for (std::uint32_t v = 0; v < numVertices && !bad; ++v) {
            std::memcpy(&pos[3 * static_cast<std::size_t>(v)], &vbuf[static_cast<std::size_t>(v) * streams_[0].stride], 12);
            for (int j = 0; j < 3; ++j) bad = bad || !std::isfinite(pos[3 * static_cast<std::size_t>(v) + j]);
        }
        const std::uint64_t srcHash = clusters::hash64(ibytes.data(), ibytes.size());
        if (bad) return;  // (not a range the builder can use: drawn as the engine made it; asked again at the next draw of it, cheaply refused here)
        e = store.request(key, ir.id, std::move(idx), std::move(pos), srcHash);
        if (e != nullptr) clusterWait_ = e->id;
        return;
    }
    using State = gpuscene::ClusterStore::State;
    State st = e->state.load(std::memory_order_acquire);
    if (st == State::Failed) return;
    if (st == State::Pending) {
        clusterWait_ = e->id;
        return;
    }
    if (st == State::Built) {
        // the arena copy: the index bytes as the engine's CPU copy holds them now
        std::vector<std::uint8_t> ibytes(static_cast<std::size_t>(primitives) * 3 * isz);
        if (!ib->readCpu(startIndex * isz, static_cast<std::uint32_t>(ibytes.size()), ibytes.data())) return;
        if (!store.upload(*this, *e, ibytes, isz)) {
            st = e->state.load(std::memory_order_acquire);
            if (st != State::Ready) {
                if (st != State::Failed) clusterWait_ = e->id;  // (the ring was full: the next draw tries again)
                return;
            }
        }
    }
    // Ready: the packet reads the copy (first index = the copy's offset in its arena block, a multiple of the index size)
    packet_.indexBuffer = e->alloc.native;
    packet_.first = foldIndexOffset(e->alloc.offset, index32, 0, packet_.indexOffset);
    clusterWait_ = e->id;
    clusterUsing_ = true;
    store.countUse();
}

}  // namespace rvk::d3d9
