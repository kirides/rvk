// F4-3 owned SpeedTrees (docs/research/frame-ownership.md 7.25): the device's half. See device.h (setSceneTrees, ownedTreeFlush).
//
// The caller (native/static_ops_trees_owned.cpp) lays one row per instance out in a ring block (VS window 2: c32 + 4 p = row p) and hands one TreeItem per
// run of consecutive instances that share a DrawRecord (z: a bucket's whole run; solid: instances with the same lighting entry). A run of `instances` list
// positions p .. p + n - 1 draws as ONE indirect command with instanceCount n and firstInstance p: the ref stream (instance-rate: record index, view slot)
// repeats the run's record index n times, and the tree vertex shader's own instance-rate stream 1 (our identity ramp, element p + i = p + i) picks the row.

#include <algorithm>
#include <bit>
#include <cstring>

#include "d3d9/device.h"
#include "d3d9/scene_hash.h"
#include "gpuscene/lighting_images.h"

namespace rvk::d3d9 {

std::uint64_t Device::treeEnvEpoch(bool solid, std::uint32_t viewSlot, bool& ok) {
    const std::uint64_t h = ownedEnvHash(solid, viewSlot, ok);
    const std::size_t k = treeEnvIndex(solid, viewSlot);
    if (treeEnvHash_[k] != h || treeEnvEpoch_[k] == 0) {
        treeEnvHash_[k] = h;
        treeEnvEpoch_[k] = ++treeEnvCounter_;
    }
    return treeEnvEpoch_[k];
}

bool Device::treeEnvSame(bool solid, std::uint32_t viewSlot, std::uint64_t epoch) const {
    bool ok = false;
    const std::uint64_t h = ownedEnvHash(solid, viewSlot, ok);
    const std::size_t k = treeEnvIndex(solid, viewSlot);
    return h == treeEnvHash_[k] && epoch == treeEnvEpoch_[k] && epoch != 0;
}

// every observer of the main view's tree draws can be served by ownedTreeObserve (the RT scene without captures: no animated trees) and reads no more than the
// world rows + the material windows; anything else (a register reader, a capture wanter) keeps the trees composed
bool Device::ownedTreeObserversOk() const {
    for (const DrawObserver* o : observers_) {
        const DrawObserver::Registers r = o->registersRead();
        if (r == DrawObserver::Registers::All) return false;
        if (r == DrawObserver::Registers::MaterialLighting && !gpuscene::lightingimages().active()) return false;  // (F-VAR: the template's lighting window composes its CPU image for the observer)
        // (F4-4: the cascades' tree draws are served the same way, so an observer of those must accept owned trees too)
        if (o->observes(draws::Kind::SpeedTree, scene::Phase{scene::View::Main, scene::Pass::Solid}) ||
            o->observes(draws::Kind::SpeedTree, scene::Phase{scene::View::Main, scene::Pass::DepthPrepass}) ||
            o->observes(draws::Kind::SpeedTree, scene::Phase{scene::View::Shadow, scene::Pass::ShadowDepth})) {
            if (!o->ownedTreesOk()) return false;
        }
    }
    return true;
}

bool Device::ownedVegObserversOk() const {
    for (const DrawObserver* o : observers_) {
        if (o->observes(draws::Kind::Vegetation, scene::Phase{scene::View::Main, scene::Pass::Solid}) || o->observes(draws::Kind::Vegetation, scene::Phase{scene::View::Main, scene::Pass::DepthPrepass})) {
            // (F-VAR2: an observer that reads the material / lighting registers only and never asks for a capture is shown each owned batch through ownedVegObserve)
            if (!o->ownedVegOk()) return false;
            const DrawObserver::Registers r = o->registersRead();
            if (r == DrawObserver::Registers::All) return false;
            if (r == DrawObserver::Registers::MaterialLighting && !gpuscene::lightingimages().active()) return false;
        }
    }
    return true;
}

std::uint32_t Device::ownedVegObserve(const CachedDraw& c, const OwnedWindows& ow, std::uint32_t instances) {
    if (!c.valid || instances == 0) return 0;
    struct Scope {
        Device& d;
        const OwnedWindows* savedOwned;
        Scope(Device& dev, const OwnedWindows* o) : d(dev), savedOwned(dev.owned_) {
            d.owned_ = o;
            d.ownedReplay_ = true;
        }
        ~Scope() {
            d.owned_ = savedOwned;
            d.ownedReplay_ = false;
        }
    } scope(*this, &ow);
    if (vertexShader() != c.vs) setVertexShaderRef(ShaderRef(const_cast<Shader*>(c.vs)));
    if (pixelShader() != c.ps) setPixelShaderRef(ShaderRef(const_cast<Shader*>(c.ps)));
    loadOwnedRegisters(ow);
    ReplaceQuery q = c.query;
    q.tag = &tag_;
    q.textures = c.tex;
    q.mainView = firings_.mainViews();
    q.objectRows = nullptr;
    q.instances = instances;
    captureRequest_ = {};
    captureRequestRw_ = {};
    captureInstances_ = false;
    const CoverQuery cq = coverOf(q);
    for (std::size_t i = 0; i < observers_.size(); ++i) {
        DrawObserver* o = observers_[i];
        if (!o->observes(c.kind, c.phase)) continue;
        if (o->coversDraw(cq)) continue;
        o->observe(*this, q);
    }
    captureRequest_ = {};
    return 1;
}

bool Device::ownedTreeObserved(const CachedDraw& c) const {
    for (const DrawObserver* o : observers_) {
        if (o->observes(c.kind, c.phase)) return true;
    }
    return false;
}

std::uint32_t Device::ownedTreeObserve(const CachedDraw& c, const OwnedWindows& ow, Buffer* ramp, const float* rows, std::uint32_t count) {
    if (ramp == nullptr || rows == nullptr || count == 0 || !c.valid || !c.query.instanced) return 0;
    constexpr std::uint32_t kBatch = 50;  // (the engine's batch limit; the RT scene reads up to 64 instances of one draw)
    struct Scope {
        Device& d;
        const OwnedWindows* savedOwned;
        StreamSource savedStream;
        Scope(Device& dev, const OwnedWindows* o) : d(dev), savedOwned(dev.owned_), savedStream(dev.streamSource(1)) {
            d.owned_ = o;
            d.ownedReplay_ = true;
        }
        ~Scope() {
            d.owned_ = savedOwned;
            d.ownedReplay_ = false;
            d.setStreamSource(1, savedStream.buffer, savedStream.offset, savedStream.stride);
        }
    } scope(*this, &ow);
    if (vertexShader() != c.vs) setVertexShaderRef(ShaderRef(const_cast<Shader*>(c.vs)));
    if (pixelShader() != c.ps) setPixelShaderRef(ShaderRef(const_cast<Shader*>(c.ps)));
    setStreamSource(1, ramp, 0, 4);
    loadOwnedRegisters(ow);
    treeObservedFrame_ = frameValue();
    std::uint32_t shown = 0;
    for (std::uint32_t at = 0; at < count; at += kBatch) {
        const std::uint32_t n = std::min(kBatch, count - at);
        for (std::uint32_t i = 0; i < n; ++i) setVertexShaderConstantF(32 + 4 * i, rows + static_cast<std::size_t>(at + i) * 16, 4);
        ReplaceQuery q = c.query;
        q.tag = &tag_;
        q.textures = c.tex;
        q.mainView = firings_.mainViews();
        q.objectRows = nullptr;
        q.instances = n;
        captureRequest_ = {};
        captureRequestRw_ = {};
        captureInstances_ = false;
        const CoverQuery cq = coverOf(q);
        for (std::size_t i = 0; i < observers_.size(); ++i) {
            DrawObserver* o = observers_[i];
            if (!o->observes(c.kind, c.phase)) continue;
            if (o->coversDraw(cq)) continue;
            o->observe(*this, q);
        }
        if (!captureRequest_.null()) {
            ++treeObserveCaptures;
            captureRequest_ = {};
        }
        shown += n;
    }
    return shown;
}

bool Device::ownedTreeFlush(const std::vector<TreeItem>& items, const std::vector<shader::abi::DrawRecord>& recs, const std::vector<std::uint32_t>& posRec, std::uint32_t viewSlot, std::uint32_t positions,
                             const VegInstances* inst) {
    ++treeFlushStats.flushes;
    if (items.empty()) return true;
    const std::uint32_t n = static_cast<std::uint32_t>(items.size());
    const rhi::GpuRef frameBlock = frameBlockAddress();
    if (frameBlock.null() || !sceneViewsReady() || viewSlot >= sceneViews_.slots || positions == 0) return false;
    // (every address the shaders will read: refused before anything is written)
    std::uint64_t total = 0;
    for (const TreeItem& it : items) {
        if (it.cd == nullptr || it.instances == 0 || it.cd->scenePipeline2 == GamePipeline::None || !it.cd->valid) {
            ++treeFlushStats.refused;
            return false;
        }
        total += it.instances;
    }
    for (const TreeItem& it : items) ++sceneStats_.uberOwned[static_cast<std::size_t>(it.cd->kind)][it.cd->v.uberPs];  // (S-1a)
    for (const TreeItem& it : items) ++sceneStats_.uberVsOwned[static_cast<std::size_t>(it.cd->kind)][it.cd->v.uberVs];  // (S-1b)
    if (total != positions || posRec.size() != positions || recs.empty()) {
        ++treeFlushStats.refused;
        return false;
    }
    // vegetation: every item's template must name its stream-1 binding (the one this flush replaces), the instance bytes must be there
    std::vector<std::uint32_t> instBinding;
    if (inst != nullptr) {
        if (inst->data == nullptr || inst->stride == 0 || static_cast<std::uint64_t>(positions) * inst->stride > (8u << 20)) {
            ++treeFlushStats.refused;
            return false;
        }
        instBinding.resize(items.size(), UINT32_MAX);
        for (std::size_t i = 0; i < items.size(); ++i) {
            const CachedDraw& c = *items[i].cd;
            std::uint32_t found = UINT32_MAX, matches = 0;
            if ((c.streamMask & 2u) != 0 && c.stream[1].buffer != 0) {
                for (std::uint32_t b = 0; b < c.vi.bufferCount && b < rhi::kMaxVertexBindings; ++b) {
                    if (c.vi.buffers[b] == c.stream[1].buffer && c.vi.offsets[b] == c.stream[1].offset) {  // (a static buffer's binding = arena buffer + block offset: the arena holds other streams too)
                        found = b;
                        ++matches;
                    }
                }
            }
            if (matches != 1) {
                ++treeFlushStats.refused;
                return false;
            }
            instBinding[i] = found;
        }
    }
    for (const shader::abi::DrawRecord& r : recs) {
        if (r.draw == 0 || r.vsWin[2] == 0 || r.psWin[3] == 0) {
            ++treeFlushStats.refused;
            return false;
        }
    }
    for (const std::uint32_t r : posRec) {
        if (r >= recs.size()) {
            ++treeFlushStats.refused;
            return false;
        }
    }
    syncPhaseRegions();
    if (rendering_ && (scopeGbuf_ != gbufferScope() || scopeSecondary_ != wantSecondaryScope())) endRendering();
    if (!beginRendering()) return false;
    rhi::RingAlloc ra = game_->allocRing(static_cast<std::uint64_t>(recs.size()) * sizeof(shader::abi::DrawRecord), 16);
    rhi::RingAlloc la = game_->allocRing(static_cast<std::uint64_t>(positions) * 8, 16);
    rhi::RingAlloc aa = game_->allocRing(static_cast<std::uint64_t>(n) * rhi::kIndirectStride, 16);
    rhi::RingAlloc ia;
    if (inst != nullptr) ia = game_->allocRing(static_cast<std::uint64_t>(positions) * inst->stride, 16);
    if (ra.ptr == nullptr || la.ptr == nullptr || aa.ptr == nullptr || (inst != nullptr && ia.ptr == nullptr)) {
        ++sceneStats_.fbRingFull;
        return false;
    }
    if (inst != nullptr) std::memcpy(ia.ptr, inst->data, static_cast<std::size_t>(positions) * inst->stride);
    auto* recs_ = static_cast<shader::abi::DrawRecord*>(ra.ptr);
    auto* list = static_cast<std::uint32_t*>(la.ptr);
    auto* args = static_cast<std::uint32_t*>(aa.ptr);
    const std::uint64_t frame = frameValue();
    std::memcpy(recs_, recs.data(), recs.size() * sizeof(shader::abi::DrawRecord));
    for (std::uint32_t p = 0; p < positions; ++p) {
        list[2 * p] = posRec[p];
        list[2 * p + 1] = viewSlot;
    }
    std::uint32_t pos = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const TreeItem& it = items[i];
        const CachedDraw& c = *it.cd;
        args[5 * i + 0] = c.packet.count;
        args[5 * i + 1] = it.instances;
        args[5 * i + 2] = c.packet.first;
        args[5 * i + 3] = static_cast<std::uint32_t>(c.packet.vertexOffset);
        args[5 * i + 4] = pos;
        pos += it.instances;
        for (std::uint32_t mask = c.texMask; mask != 0; mask &= mask - 1) c.tex[std::countr_zero(mask)]->lastUseFrame_ = frame;
        if (c.v.giDraw) giDrawCount_ += it.instances;
        stats_.draws += it.instances;
        stats_.totalDraws += it.instances;
        passDraws_ += it.instances;
        for (std::uint32_t q = 0; q < it.instances; ++q) {
            phases_.countDraw();
            draws_.count(c.kind, c.phase.view);
        }
        replayStats.replayed += it.instances;
        sceneStats_.replayedSceneDraws += it.instances;
        sceneStats_.candidates += it.instances;
    }
    // runs of consecutive items with one bucket key: one indirect call each
    std::uint32_t start = 0;
    while (start < n) {
        std::uint32_t end = start + 1;
        const CachedDraw& c0 = *items[start].cd;
        while (end < n && items[end].bucket == items[start].bucket) {
            const CachedDraw& c = *items[end].cd;
            if (&c != &c0 && (c.scenePipeline2 != c0.scenePipeline2 || !viEqual(c.vi, c0.vi) || !dynEqual(c.dyn, c0.dyn) || c.packet.indexBuffer != c0.packet.indexBuffer ||
                              c.packet.indexOffset != c0.packet.indexOffset || c.packet.index32 != c0.packet.index32)) {
                break;  // (a bucket key that merges different pipelines / inputs: cut the run here)
            }
            ++end;
        }
        DrawPacket m = c0.packet;
        if (inst != nullptr) {
            // (the run's items share one vertex input apart from the stream-1 buffer, which is this frame's block for all of them)
            VertexInputValues patched = c0.vi;
            const std::uint32_t b = instBinding[start];
            patched.buffers[b] = ia.buffer;
            patched.offsets[b] = ia.offset;
            m.vi = internSceneViAt(patched, la.buffer, la.offset);
        } else {
            m.vi = internSceneViAt(c0.vi, la.buffer, la.offset);
        }
        m.dyn = internDyn(c0.dyn);
        m.pipeline = c0.scenePipeline2;
        m.op = DrawPacket::Op::DrawIndexedIndirect;
        m.scene = 0;
        m.count = end - start;
        m.instances = 1;
        m.first = 0;
        m.vertexOffset = 0;
        m.indirectBuffer = aa.buffer;
        m.indirectOffset = aa.offset + static_cast<std::uint64_t>(start) * rhi::kIndirectStride;
        shader::abi::ScenePush sp{};
        sp.records = ra.ref.bits;
        sp.frame = frameBlock.bits;
        sp.views = sceneViews_.base.bits;
        static_assert(sizeof(sp) == sizeof(m.push));
        std::memcpy(&m.push, &sp, sizeof(sp));
        submitInterned(m);
        ++sceneStats_.indirectCalls;
        sceneStats_.records += end - start;
        treeFlushStats.commands += end - start;
        ++treeFlushStats.packets;
        start = end;
    }
    treeFlushStats.items += n;
    treeFlushStats.records += recs.size();
    treeFlushStats.instances += positions;
    return true;
}

}  // namespace rvk::d3d9
