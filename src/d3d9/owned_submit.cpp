// F4-1: a retained draw submitted with NOTHING composed (docs/research/frame-ownership.md 7.17): the packet is the record's own (CachedDraw), its DrawBlock is
// derived (deriveScene: the per-frame inputs + the record's textures, sampler states, alpha test, pass c5), its register windows are the caller's persistent
// images (OwnedWindows: material / view / constants / lighting records), and it leaves as a scene draw (DrawRecord + indirect run). The register file of the
// device is not read and not written; the draw tag only matters for the pack word (the caller sets it). Every guard of the scene draws applies (null
// address, scene form, pipeline); a refusal draws nothing and leaves nothing behind (the caller composes the op normally).

#include <algorithm>
#include <bit>
#include <cstring>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"

namespace rvk::d3d9 {

bool Device::ownedTextureLive(const void* texture) {
    if (texture == nullptr) return true;
    const std::lock_guard lock(liveMutex_);
    return liveTextures_.count(const_cast<Texture*>(static_cast<const Texture*>(texture))) != 0;
}

bool Device::ownedBindingsLive(const DeriveIn& in) {
    const std::lock_guard lock(liveMutex_);
    for (std::uint32_t i = 0; i < 4; ++i) {
        if (in.streams[i] != nullptr && liveBuffers_.count(in.streams[i]) == 0) return false;
    }
    if (in.indices != nullptr && liveBuffers_.count(in.indices) == 0) return false;
    for (std::uint32_t st = 0; st < kSamplerCount; ++st) {
        if (in.tex[st] != nullptr && liveTextures_.count(in.tex[st]) == 0) return false;
    }
    return true;
}

Device::OwnedWhy Device::submitOwned(const CachedDraw& c, const DeriveIn& in, const OwnedWindows& win, bool dryRun, int* deriveWhy) {
    if (!c.valid || c.vs == nullptr || c.ps == nullptr || c.scenePipeline == GamePipeline::None || !sceneDrawsOn_ || !sceneAvail_) return OwnedWhy::NoScene;
    if (!ownedBindingsLive(in)) return OwnedWhy::Derive;
    // the shaders read their floats through the windows (a full-block shader would read the register file)
    if (!splitConstants_ || !c.vs->reflection.splitConsts || !c.ps->reflection.splitConsts || ubershader_.load(std::memory_order_relaxed)) return OwnedWhy::Split;
    // observers (RT scene, GI, object motion) are shown the draw at prepare / replay time with the device's tag and registers: draws they observe stay composed
    for (DrawObserver* o : observers_) {
        if (o->observes(c.kind, c.phase)) return OwnedWhy::Consumers;
    }
    // the windows: every window the shaders read needs an offer that covers the read extent (the splitStage rule); unread windows point at a zero block
    std::uint64_t bases[2][shader::abi::kConstWindows] = {};
    for (std::uint32_t stage = 0; stage < 2; ++stage) {
        const Shader& sh = stage == 0 ? *c.vs : *c.ps;
        const std::uint32_t* first = stage == 0 ? shader::abi::kVsWindowFirst : shader::abi::kPsWindowFirst;
        for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) {
            if (stage == 1 && w == 1) continue;  // (c5: the DrawBlock's psC5, patched in below)
            std::uint32_t end = sh.reflection.windowEnd[w];
            if (stage == 1 && w == 3) end = std::max(end, c.v.minEnd);
            const std::uint32_t need = end > first[w] ? end - first[w] : 0;
            if (need == 0) continue;  // (the zero block is made below)
            const OwnedWindows::Win& ow = win.w[stage][w];
            if (ow.kind == OwnedWindows::Kind::None || ow.covered < need || ow.base.null()) return OwnedWhy::Window;
            bases[stage][w] = ow.base.bits;
        }
    }
    if (dryRun) {
        // (the derivation validates the packet against the device now: modules, phase, consumers, variants, environment, buffers, textures)
        SceneView probe;
        const int why = deriveScene(c, in, probe);
        if (deriveWhy != nullptr) *deriveWhy = why;
        return why != 0 || !probe.scene ? OwnedWhy::Derive : OwnedWhy::Submitted;
    }
    syncPhaseRegions();
    if (rendering_ && (scopeGbuf_ != gbufferScope() || scopeSecondary_ != wantSecondaryScope())) endRendering();
    if (!beginRendering()) return OwnedWhy::Rendering;
    SceneView v;
    const int why = deriveScene(c, in, v);
    if (deriveWhy != nullptr) *deriveWhy = why;
    if (why != 0 || !v.scene) return OwnedWhy::Derive;
    if (zeroBlock_.null()) {
        rhi::RingAlloc z = game_->allocRing(4096, 16);
        if (z.ptr == nullptr) return OwnedWhy::Packet;
        std::memset(z.ptr, 0, 4096);
        zeroBlock_ = z.ref;
    }
    rhi::RingAlloc db = game_->allocRing(sizeof(shader::abi::DrawBlock), 16);
    if (db.ptr == nullptr) return OwnedWhy::Packet;
    std::memcpy(db.ptr, &v.block, sizeof(v.block));
    DrawPacket p = c.packet;
    p.vi = internVi(c.vi);
    p.dyn = internDyn(c.dyn);
    const std::uint32_t flags = p.push.flags;
    p.push = shader::abi::PushConstants{0, 0, db.ref.bits, frameBlockAddress().bits, 0, stats_.draws, flags};
    for (std::uint32_t stage = 0; stage < 2; ++stage) {
        const std::uint32_t* first = stage == 0 ? shader::abi::kVsWindowFirst : shader::abi::kPsWindowFirst;
        for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) {
            std::uint64_t b = bases[stage][w];
            if (b == 0) b = zeroBlock_.plus(-static_cast<std::int64_t>(first[w]) * 16).bits;
            (stage == 0 ? p.push.vsWin : p.push.psWin)[w] = b;
        }
    }
    p.push.psWin[1] = db.ref.plus(static_cast<std::int64_t>(offsetof(shader::abi::DrawBlock, psC5)) - 5 * 16).bits;
    if (!toScenePacket(p, c.scenePipeline)) return OwnedWhy::Packet;  // (nothing recorded: the guard refused before it pushed a record)
    // from here the draw is made: the bookkeeping of replayDraw
    const std::uint64_t frame = frameValue();
    for (std::uint32_t mask = c.texMask; mask != 0; mask &= mask - 1) c.tex[std::countr_zero(mask)]->lastUseFrame_ = frame;
    if (c.v.giDraw) ++giDrawCount_;
    ++stats_.draws;
    ++stats_.totalDraws;
    ++passDraws_;
    phases_.countDraw();
    draws_.count(c.kind, c.phase.view);
    ++replayStats.replayed;
    ++sceneStats_.replayedSceneDraws;
    ++sceneStats_.candidates;
    ++ownedSubmitted_;
    submitInterned(p);
    return OwnedWhy::Submitted;
}

}  // namespace rvk::d3d9
