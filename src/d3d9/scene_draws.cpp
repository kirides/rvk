// F2b-1 scene draws (docs/research/frame-ownership.md 7.11): the native static ops' main-view z / solid draws as DrawRecords + indirect runs.
//
// prepareDraw still composes every draw as before (registers, windows, DrawBlock); for an eligible draw it also resolves the scene form of its
// pipeline (scene vertex shader + the scene form of its pixel shader, the draw's vertex input + the ref stream). submitPacket turns the finished
// packet into a scene packet: the push block's window bases / DrawBlock / flags go into a DrawRecord, the packet waits in the batch. flushBatch
// (draw.cpp) groups the batch by pipeline as before and cuts it into runs of scene packets that agree in everything the recording needs (pipeline,
// vertex input, dynamic state, index binding, frame block): a run becomes ONE DrawIndexedIndirect packet over a ring block of its records and
// one of its VkDrawIndexedIndirectCommand arguments (firstInstance = the record's index in the block = ref.x of the instance-rate ref stream).

#include <algorithm>
#include <cstring>

#include "d3d9/device.h"

namespace rvk::d3d9 {

namespace {

constexpr std::uint32_t kSceneRunMax = 1024;  // records per indirect call (the ref stream's length)

bool viEqual(const rhi::VertexInputValues& a, const rhi::VertexInputValues& b) {
    const auto eq = [](const void* x, const void* y, std::size_t n) { return n == 0 || std::memcmp(x, y, n) == 0; };
    return a.bufferCount == b.bufferCount && a.dynamicLayout == b.dynamicLayout && a.bindingCount == b.bindingCount && a.attributeCount == b.attributeCount &&
           a.vertexShift == b.vertexShift && eq(a.buffers, b.buffers, sizeof(a.buffers[0]) * a.bufferCount) && eq(a.offsets, b.offsets, sizeof(a.offsets[0]) * a.bufferCount) &&
           eq(a.bindings, b.bindings, sizeof(a.bindings[0]) * a.bindingCount) && eq(a.attributes, b.attributes, sizeof(a.attributes[0]) * a.attributeCount);
}

bool dynEqual(const rhi::DynamicStateValues& a, const rhi::DynamicStateValues& b) {
    constexpr std::size_t prefix = offsetof(rhi::DynamicStateValues, blendEnable);
    const std::uint32_t n = a.colorCount;
    if (a.colorCount != b.colorCount || std::memcmp(&a, &b, prefix) != 0) return false;
    return n == 0 || (std::memcmp(a.blendEnable, b.blendEnable, sizeof(VkBool32) * n) == 0 && std::memcmp(a.blendEquation, b.blendEquation, sizeof(VkColorBlendEquationEXT) * n) == 0 &&
                      std::memcmp(a.writeMask, b.writeMask, sizeof(VkColorComponentFlags) * n) == 0);
}

std::uint64_t alignUp16(std::uint64_t v) { return (v + 15) & ~std::uint64_t{15}; }

}  // namespace

void Device::setSceneDraws(bool on, std::uint32_t checkEvery) {
    sceneCheckEvery_ = checkEvery;
    if (!game_) return;
    bool avail = false;
    if (on) {
        const rhi::DeviceCaps& c = game_->caps();
        const rhi::GameCaps& g = game_->gameCaps();
        avail = g.maxVertexInputAttributes > shader::abi::kSceneRefLocation && c.drawIndirectFirstInstance && c.multiDrawIndirect;
        if (!avail) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) {
                spdlog::warn("d3d9: scene draws: not available (max vertex input attributes {} (needs {}), indirect first instance {}, multi draw indirect {})",
                             g.maxVertexInputAttributes, shader::abi::kSceneRefLocation + 1, c.drawIndirectFirstInstance, c.multiDrawIndirect);
            }
        }
    }
    if (on != sceneDrawsOn_ || avail != sceneAvail_) spdlog::info("native: scene draws {}", on ? (avail ? "on" : "requested, not available") : "off");
    sceneDrawsOn_ = on;
    sceneAvail_ = avail;
}

// the ref stream: (i, 0) for i < kSceneRunMax in a ring block of this frame
static bool ensureRef(rhi::GameDevice& game, std::uint64_t& buffer, std::uint64_t& offset, bool& valid) {
    if (valid) return true;
    rhi::RingAlloc a = game.allocRing(static_cast<std::uint64_t>(kSceneRunMax) * 8, 16);
    if (a.ptr == nullptr) return false;
    auto* w = static_cast<std::uint32_t*>(a.ptr);
    for (std::uint32_t i = 0; i < kSceneRunMax; ++i) {
        w[2 * i] = i;
        w[2 * i + 1] = 0;
    }
    buffer = a.buffer;
    offset = a.offset;
    valid = true;
    return true;
}

bool Device::ensureSceneRef() { return ensureRef(*game_, sceneRef_.buffer, sceneRef_.offset, sceneRef_.valid); }

// what prepareDraw decides for an eligible draw (the main view's static mesh z / solid draws issued by the static ops): the pipeline of its scene form
// when its scene shaders and that pipeline exist, else None (the draw goes the normal way; the missing pieces are requested in the background)
void Device::sceneDecide(const PipelineDesc& pd, bool giDraw, bool captureDraw, bool uberTaken, draws::Kind kind, const scene::Phase& phase, bool geomOk) {
    if (capture_ != nullptr) {
        lastVariant_.sceneActive = true;
        lastVariant_.scenePending = false;
    }
    if (kind != draws::Kind::StaticMesh || phase.view != scene::View::Main || (phase.pass != scene::Pass::Solid && phase.pass != scene::Pass::DepthPrepass)) return;
    ++sceneStats_.candidates;
    const auto pending = [&] {
        if (capture_ != nullptr) lastVariant_.scenePending = true;
    };
    if (!packet_.batchable || !rendering_ || !geomOk) {
        ++sceneStats_.fbNotBatchable;
        return;
    }
    // which pixel shader form the draw takes (computed like prepareDraw's choice): the G-buffer variant, the GI variant or the plain module; the
    // capture / motion variants, the tint view and the ubershader have no scene form
    VariantSlot base = VariantSlot::Base;
    if (captureDraw || uberTaken || (!gbufWrite_ && !giDraw && pd.fs != ps_->module)) {
        ++sceneStats_.fbVariantKind;
        return;
    }
    if (gbufWrite_) base = gbufferSlot(false);
    else if (giDraw) base = VariantSlot::Gi;
    const VariantSlot sceneFs = sceneSlotOf(base);
    if (sceneFs == VariantSlot::Count) {
        ++sceneStats_.fbVariantKind;
        return;
    }
    const rhi::ShaderModule vsM = variant(vs_, VariantSlot::SceneVs);
    const rhi::ShaderModule fsM = variant(ps_, sceneFs);
    if (vsM == rhi::ShaderModule::None || fsM == rhi::ShaderModule::None) {
        const bool failed = vs_->variantState[static_cast<std::size_t>(VariantSlot::SceneVs)].load() == 3 || ps_->variantState[static_cast<std::size_t>(sceneFs)].load() == 3;
        if (failed) {
            ++sceneStats_.fbVariantFailed;
        } else {
            ++sceneStats_.fbVariantPending;
            pending();
        }
        return;
    }
    PipelineDesc sd = pd;
    sd.vs = vsM;
    sd.fs = fsM;
    if (!workVi_.dynamicLayout) {
        // the vertex layout is part of the pipeline: one more binding (the ref stream, per instance) + attribute 24
        if (pd.bindingCount >= rhi::kMaxVertexBindings || pd.attributeCount >= rhi::kMaxVertexAttributes) {
            ++sceneStats_.fbNoRoom;
            return;
        }
        sd.bindings[sd.bindingCount] = {8, 1};
        sd.attributes[sd.attributeCount++] = {shader::abi::kSceneRefLocation, sd.bindingCount, VK_FORMAT_R32G32_UINT, 0};
        ++sd.bindingCount;
    } else if (workVi_.bindingCount >= rhi::kMaxVertexBindings || workVi_.attributeCount >= rhi::kMaxVertexAttributes) {
        ++sceneStats_.fbNoRoom;
        return;
    }
    const GamePipeline sp = findPipeline(sd);
    if (sp == GamePipeline::None) {
        game_->prewarmOncePipeline(sd);
        ++sceneStats_.fbPipelinePending;
        pending();
        return;
    }
    if (!ensureSceneRef()) {
        ++sceneStats_.fbRingFull;
        return;
    }
    scenePipe_ = sp;
}

// the scene vertex input of a draw: its normal one + the ref stream (instance-rate binding after the draw's own, attribute 24). vertexShift is zeroed: the
// per-draw vertexOffset of the indirect arguments already carries it
const VertexInputValues* Device::internSceneVi(const VertexInputValues& n) {
    VertexInputValues& v = sceneViWork_;
    v = n;
    v.vertexShift = 0;
    const std::uint32_t idx = v.bufferCount;
    v.buffers[idx] = sceneRef_.buffer;
    v.offsets[idx] = sceneRef_.offset;
    v.bufferCount = idx + 1;
    if (v.dynamicLayout) {
        v.bindings[idx] = {8, 1};
        v.bindingCount = idx + 1;
        v.attributes[v.attributeCount++] = {shader::abi::kSceneRefLocation, idx, VK_FORMAT_R32G32_UINT, 0};
    }
    if (lastSceneVi_ == nullptr || !viEqual(*lastSceneVi_, v)) lastSceneVi_ = viArena_.push(v);
    return lastSceneVi_;
}

// a finished packet (pipeline, vertex input, dynamic state, push block set) becomes a scene packet: its push data goes into a DrawRecord
bool Device::toScenePacket(DrawPacket& p, GamePipeline pipeline) {
    // guard: every address a scene shader can read (the eight window bases, the DrawBlock, the frame block) must be set; else the normal path draws it
    bool nullAddress = p.push.draw == 0 || p.push.frame == 0 || sceneFaultInject_;
    for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) nullAddress |= p.push.vsWin[w] == 0 || p.push.psWin[w] == 0;
    if (nullAddress || !sceneRef_.valid || sceneRef_.buffer == 0 || p.vi == nullptr || p.pipeline == GamePipeline::None) {
        ++sceneStats_.fbNullAddress;
        if (sceneStats_.fbNullAddress <= 4) spdlog::warn("d3d9: scene draw refused: a null address in the push block (draw {:#x}, frame {:#x}, vs windows {:#x} {:#x} {:#x} {:#x}, ps windows {:#x} {:#x} {:#x} {:#x})", p.push.draw, p.push.frame, p.push.vsWin[0], p.push.vsWin[1], p.push.vsWin[2], p.push.vsWin[3], p.push.psWin[0], p.push.psWin[1], p.push.psWin[2], p.push.psWin[3]);
        return false;
    }
    shader::abi::DrawRecord r{};
    for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) {
        r.vsWin[w] = p.push.vsWin[w];
        r.psWin[w] = p.push.psWin[w];
    }
    r.draw = p.push.draw;
    r.prevVs = p.push.prevVsConsts;
    r.drawId = p.push.drawId;
    r.flags = p.push.flags;
    sceneRecs_.push_back(r);
    p.scene = static_cast<std::uint32_t>(sceneRecs_.size());
    p.pipeline = pipeline;
    p.vi = internSceneVi(*p.vi);
    const std::uint64_t frame = p.push.frame;
    p.push = {};
    p.push.frame = frame;
    ++sceneStats_.sceneDraws;
    return true;
}

// -1: `b` can join the run of `a`; else the reason (SceneDrawStats::breaks), 5 = another pipeline (the end of a pipeline group: not a break)
int Device::sceneMergeBreak(const DrawPacket& a, const DrawPacket& b) const {
    if (a.pipeline != b.pipeline) return 5;
    if (a.vi != b.vi && !viEqual(*a.vi, *b.vi)) return 0;
    if (a.dyn != b.dyn && !dynEqual(*a.dyn, *b.dyn)) return 1;
    if (a.indexBuffer != b.indexBuffer || a.indexOffset != b.indexOffset || a.index32 != b.index32) return 2;
    if (a.push.frame != b.push.frame) return 3;
    return -1;
}

// one run of scene packets (batch_ indices) -> one DrawIndexedIndirect packet over its records / arguments
void Device::sceneFlushRun(const std::vector<std::uint32_t>& run, const std::function<void(const DrawPacket&)>& record) {
    const std::uint32_t n = static_cast<std::uint32_t>(run.size());
    if (n == 0) return;
    for (const std::uint32_t at : run) {
        // (a scene packet always has its record: a stale index would make the shader read somebody else's addresses)
        if (batch_[at].scene == 0 || batch_[at].scene > sceneRecs_.size()) {
            ++sceneStats_.fbNullAddress;
            spdlog::error("d3d9: scene run dropped: packet without a record (index {} of {})", batch_[at].scene, sceneRecs_.size());
            return;
        }
    }
    const DrawPacket& first = batch_[run[0]];
    rhi::RingAlloc ra = game_->allocRing(static_cast<std::uint64_t>(n) * sizeof(shader::abi::DrawRecord), 16);
    rhi::RingAlloc aa = game_->allocRing(static_cast<std::uint64_t>(n) * rhi::kIndirectStride, 16);
    if (ra.ptr == nullptr || aa.ptr == nullptr) {
        ++sceneStats_.fbRingFull;
        return;
    }
    auto* rec = static_cast<shader::abi::DrawRecord*>(ra.ptr);
    auto* args = static_cast<std::uint32_t*>(aa.ptr);
    for (std::uint32_t k = 0; k < n; ++k) {
        const DrawPacket& p = batch_[run[k]];
        rec[k] = sceneRecs_[p.scene - 1];
        if (rec[k].draw == 0) {
            rec[k].draw = 0;  // (checked in toScenePacket; kept as a tripwire for the check mode)
            ++sceneStats_.fbNullAddress;
        }
        args[5 * k + 0] = p.count;
        args[5 * k + 1] = 1;
        args[5 * k + 2] = p.first;
        args[5 * k + 3] = static_cast<std::uint32_t>(p.vertexOffset);
        args[5 * k + 4] = k;
    }
    DrawPacket m = first;
    m.op = DrawPacket::Op::DrawIndexedIndirect;
    m.scene = 0;
    m.count = n;
    m.instances = 1;
    m.first = 0;
    m.vertexOffset = 0;
    m.indirectBuffer = aa.buffer;
    m.indirectOffset = aa.offset;
    shader::abi::ScenePush sp{};
    sp.records = ra.ref.bits;
    sp.frame = first.push.frame;
    static_assert(sizeof(sp) == sizeof(m.push));
    std::memcpy(&m.push, &sp, sizeof(sp));
    record(m);
    ++sceneStats_.indirectCalls;
    sceneStats_.records += n;
    sceneStats_.maxRun = std::max<std::uint64_t>(sceneStats_.maxRun, n);
    // the check: the GPU copies the blocks into a read-back buffer (read after the frame completed) and they are compared with what was written here
    if (sceneCheckFrame_ && !sceneCheck_.busy) {
        const std::uint64_t recBytes = static_cast<std::uint64_t>(n) * sizeof(shader::abi::DrawRecord), argBytes = static_cast<std::uint64_t>(n) * rhi::kIndirectStride;
        const std::uint64_t at = sceneCheck_.bytes, argAt = at + alignUp16(recBytes), end = argAt + alignUp16(argBytes);
        if (sceneCheck_.buffer == nullptr) {
            sceneCheck_.capacity = 8ull << 20;
            sceneCheck_.buffer = rhi()->createBuffer({sceneCheck_.capacity, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "scene draws verify"});
        }
        if (sceneCheck_.buffer == nullptr || end > sceneCheck_.capacity) {
            ++sceneStats_.checkSkipped;
        } else {
            rhi::CommandList& list = uploadList();
            list.copyBuffer(*sceneCheck_.buffer, at, game_->ringBuffer(ra), ra.offset, recBytes);
            list.copyBuffer(*sceneCheck_.buffer, argAt, game_->ringBuffer(aa), aa.offset, argBytes);
            sceneCheck_.expect.resize(end, 0);
            std::memcpy(sceneCheck_.expect.data() + at, rec, recBytes);
            std::memcpy(sceneCheck_.expect.data() + argAt, args, argBytes);
            sceneCheck_.segments.emplace_back(at, recBytes);
            sceneCheck_.segments.emplace_back(argAt, argBytes);
            sceneCheck_.bytes = end;
            sceneCheck_.frameValue = game_->frameValue();
            sceneStats_.checkedRecords += n;
        }
    }
}

void Device::sceneBeginFrame() {
    sceneRef_.valid = false;
    lastSceneVi_ = nullptr;
    sceneRecs_.clear();
    sceneCollectCheck();
    sceneCheckFrame_ = sceneActive() || (sceneDrawsOn_ && sceneAvail_);
    sceneCheckFrame_ = sceneCheckFrame_ && sceneCheckEvery_ > 0 && !sceneCheck_.busy && stats_.frames % sceneCheckEvery_ == 0;
    if (sceneCheckFrame_) {
        sceneCheck_.bytes = 0;
        sceneCheck_.expect.clear();
        sceneCheck_.segments.clear();
    }
}

// the frame's check data was copied by the GPU: compare, once the frame has completed
void Device::sceneCollectCheck() {
    if (sceneCheck_.busy) {
        if (game_->completedFrame() < sceneCheck_.frameValue) return;
        sceneCheck_.busy = false;
        sceneCheck_.buffer->invalidate();
        const auto* got = static_cast<const std::uint8_t*>(sceneCheck_.buffer->map());
        const std::uint64_t n = sceneCheck_.expect.size();
        std::uint64_t bad = 0, firstBad = UINT64_MAX;
        for (const auto& [off, size] : sceneCheck_.segments) {
            for (std::uint64_t i = off; i + 4 <= off + size && i + 4 <= n; i += 4) {
                if (std::memcmp(got + i, sceneCheck_.expect.data() + i, 4) != 0) {
                    ++bad;
                    if (firstBad == UINT64_MAX) firstBad = i;
                }
            }
        }
        ++sceneStats_.checks;
        sceneStats_.checkedBytes += n;
        if (bad != 0) {
            ++sceneStats_.checkDiffering;
            if (sceneStats_.checkDiffering <= 8) {
                spdlog::error("native: scene draws check: DIFFERING: {} of {} words differ (first at byte {})", bad, n / 4, firstBad);
            }
        }
        sceneCheck_.expect.clear();
        sceneCheck_.segments.clear();
        sceneCheck_.bytes = 0;
        return;
    }
    if (sceneCheckFrame_ && sceneCheck_.bytes != 0) sceneCheck_.busy = true;
}

void Device::sceneReport() {
    if (!sceneDrawsOn_) return;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - sceneLastReport_ < 5.0) return;
    const bool first = sceneLastReport_ == 0.0;
    sceneLastReport_ = now;
    const SceneDrawStats& s = sceneStats_;
    const SceneDrawStats& l = sceneLast_;
    if (first) {
        sceneLast_ = s;
        return;
    }
    const std::uint64_t calls = s.indirectCalls - l.indirectCalls, recs = s.records - l.records;
    spdlog::info(
        "native: scene draws (5 s): {} static main-view draws seen, {} scene draws ({} of them replayed from the packet cache), {} indirect calls, {} records, mean batch {:.1f}, max batch {} "
        "(run total); runs cut in one pipeline: vertex input {}, dynamic state {}, index binding {}, frame block {}, run size {}; pipeline groups {}; fallbacks: not batchable / no scope {}, variant kind {}, "
        "variant failed {}, variant pending {}, pipeline pending {}, no room {}, ring full {}, null address / bad record {}; check: {} read-backs, {} KiB, DIFFERING {}, skipped {}",
        s.candidates - l.candidates, s.sceneDraws - l.sceneDraws, s.replayedSceneDraws - l.replayedSceneDraws, calls, recs, calls != 0 ? static_cast<double>(recs) / static_cast<double>(calls) : 0.0, s.maxRun,
        s.breaks[0] - l.breaks[0], s.breaks[1] - l.breaks[1], s.breaks[2] - l.breaks[2], s.breaks[3] - l.breaks[3], s.breaks[4] - l.breaks[4], s.groups - l.groups, s.fbNotBatchable - l.fbNotBatchable,
        s.fbVariantKind - l.fbVariantKind, s.fbVariantFailed - l.fbVariantFailed, s.fbVariantPending - l.fbVariantPending, s.fbPipelinePending - l.fbPipelinePending, s.fbNoRoom - l.fbNoRoom,
        s.fbRingFull - l.fbRingFull, s.fbNullAddress - l.fbNullAddress, s.checks, s.checkedBytes / 1024, s.checkDiffering, s.checkSkipped);
    sceneLast_ = s;
}

}  // namespace rvk::d3d9
