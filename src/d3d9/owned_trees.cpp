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

namespace rvk::d3d9 {

std::uint64_t Device::treeEnvEpoch(bool solid, std::uint32_t viewSlot, bool& ok) {
    const std::uint64_t h = ownedEnvHash(solid, viewSlot, ok);
    const std::size_t k = solid ? 1 : 0;
    if (treeEnvHash_[k] != h || treeEnvEpoch_[k] == 0) {
        treeEnvHash_[k] = h;
        treeEnvEpoch_[k] = ++treeEnvCounter_;
    }
    return treeEnvEpoch_[k];
}

bool Device::treeEnvSame(bool solid, std::uint32_t viewSlot, std::uint64_t epoch) const {
    bool ok = false;
    const std::uint64_t h = ownedEnvHash(solid, viewSlot, ok);
    const std::size_t k = solid ? 1 : 0;
    return h == treeEnvHash_[k] && epoch == treeEnvEpoch_[k] && epoch != 0;
}

// no observer wants the main view's tree draws (the RT scene takes them while a consumer holds it: then the trees stay composed)
bool Device::ownedTreeObserversOk() const {
    for (const DrawObserver* o : observers_) {
        if (o->registersRead() == DrawObserver::Registers::All) return false;
        if (o->observes(draws::Kind::SpeedTree, scene::Phase{scene::View::Main, scene::Pass::Solid}) ||
            o->observes(draws::Kind::SpeedTree, scene::Phase{scene::View::Main, scene::Pass::DepthPrepass})) {
            return false;
        }
    }
    return true;
}

bool Device::ownedTreeFlush(const std::vector<TreeItem>& items, std::uint32_t viewSlot, std::uint32_t positions) {
    ++treeFlushStats.flushes;
    if (items.empty()) return true;
    const std::uint32_t n = static_cast<std::uint32_t>(items.size());
    const rhi::GpuRef frameBlock = frameBlockAddress();
    if (frameBlock.null() || !sceneViewsReady() || viewSlot >= sceneViews_.slots || positions == 0) return false;
    // (every address the shaders will read: refused before anything is written)
    std::uint64_t total = 0;
    for (const TreeItem& it : items) {
        if (it.cd == nullptr || it.instances == 0 || it.cd->scenePipeline2 == GamePipeline::None || !it.cd->valid || it.rec.draw == 0 || it.rec.vsWin[2] == 0 || it.rec.psWin[3] == 0) {
            ++treeFlushStats.refused;
            return false;
        }
        total += it.instances;
    }
    if (total != positions) {
        ++treeFlushStats.refused;
        return false;
    }
    syncPhaseRegions();
    if (rendering_ && (scopeGbuf_ != gbufferScope() || scopeSecondary_ != wantSecondaryScope())) endRendering();
    if (!beginRendering()) return false;
    rhi::RingAlloc ra = game_->allocRing(static_cast<std::uint64_t>(n) * sizeof(shader::abi::DrawRecord), 16);
    rhi::RingAlloc la = game_->allocRing(static_cast<std::uint64_t>(positions) * 8, 16);
    rhi::RingAlloc aa = game_->allocRing(static_cast<std::uint64_t>(n) * rhi::kIndirectStride, 16);
    if (ra.ptr == nullptr || la.ptr == nullptr || aa.ptr == nullptr) {
        ++sceneStats_.fbRingFull;
        return false;
    }
    auto* recs = static_cast<shader::abi::DrawRecord*>(ra.ptr);
    auto* list = static_cast<std::uint32_t*>(la.ptr);
    auto* args = static_cast<std::uint32_t*>(aa.ptr);
    const std::uint64_t frame = frameValue();
    std::uint32_t pos = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const TreeItem& it = items[i];
        const CachedDraw& c = *it.cd;
        recs[i] = it.rec;
        for (std::uint32_t q = 0; q < it.instances; ++q) {
            list[2 * (pos + q)] = i;
            list[2 * (pos + q) + 1] = viewSlot;
        }
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
        m.vi = internSceneViAt(c0.vi, la.buffer, la.offset);
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
        ++treeFlushStats.packets;
        start = end;
    }
    treeFlushStats.items += n;
    treeFlushStats.instances += positions;
    return true;
}

}  // namespace rvk::d3d9
