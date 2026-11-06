// F2b-1 scene draws (docs/research/frame-ownership.md 7.11): the native static ops' main-view z / solid draws as DrawRecords + indirect runs.
//
// prepareDraw still composes every draw as before (registers, windows, DrawBlock); for an eligible draw it also resolves the scene form of its
// pipeline (scene vertex shader + the scene form of its pixel shader, the draw's vertex input + the ref stream). submitPacket turns the finished
// packet into a scene packet: the push block's window bases / DrawBlock / flags go into a DrawRecord, the packet waits in the batch. flushBatch
// (draw.cpp) groups the batch by pipeline as before and cuts it into runs of scene packets that agree in everything the recording needs (pipeline,
// vertex input, dynamic state, index binding, frame block): a run becomes ONE DrawIndexedIndirect packet over a ring block of its records and
// one of its VkDrawIndexedIndirectCommand arguments (firstInstance = the record's index in the block = ref.x of the instance-rate ref stream).

#include <algorithm>
#include <bit>
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

// F2c-2: content hashes of the interned vertex input / dynamic state (the arena pointers differ between frames, the contents do not)
std::uint64_t hashBytes(std::uint64_t h, const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

std::uint64_t hashVi(const rhi::VertexInputValues& v) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    h = hashBytes(h, &v.bufferCount, sizeof(v.bufferCount));
    h = hashBytes(h, &v.dynamicLayout, sizeof(v.dynamicLayout));
    h = hashBytes(h, &v.bindingCount, sizeof(v.bindingCount));
    h = hashBytes(h, &v.attributeCount, sizeof(v.attributeCount));
    h = hashBytes(h, &v.vertexShift, sizeof(v.vertexShift));
    h = hashBytes(h, v.buffers, sizeof(v.buffers[0]) * v.bufferCount);
    h = hashBytes(h, v.offsets, sizeof(v.offsets[0]) * v.bufferCount);
    h = hashBytes(h, v.bindings, sizeof(v.bindings[0]) * v.bindingCount);
    h = hashBytes(h, v.attributes, sizeof(v.attributes[0]) * v.attributeCount);
    return h;
}

std::uint64_t hashDyn(const rhi::DynamicStateValues& d) {
    constexpr std::size_t prefix = offsetof(rhi::DynamicStateValues, blendEnable);
    std::uint64_t h = hashBytes(0xcbf29ce484222325ull, &d, prefix);
    const std::uint32_t n = d.colorCount;
    h = hashBytes(h, d.blendEnable, sizeof(VkBool32) * n);
    h = hashBytes(h, d.blendEquation, sizeof(VkColorBlendEquationEXT) * n);
    return hashBytes(h, d.writeMask, sizeof(VkColorComponentFlags) * n);
}

}  // namespace

void Device::setSceneDraws(bool on, std::uint32_t checkEvery) {
    ++variantEpoch_;
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
    if (probe_ != nullptr) {
        SceneView& v = *probe_;
        v = {};
        v.valid = v.scene = true;
        v.pipeline = pipeline;
        v.viHash = hashVi(*p.vi);
        v.dynHash = p.dyn != nullptr ? hashDyn(*p.dyn) : 0;
        v.indexBuffer = p.indexBuffer;
        v.indexOffset = p.indexOffset;
        v.index32 = p.index32;
        v.first = p.first;
        v.count = p.count;
        v.vertexOffset = p.vertexOffset;
        v.flags = p.push.flags;
        v.block = probeBlock_;
    }
    const std::uint64_t frame = p.push.frame;
    p.push = {};
    p.push.frame = frame;
    ++sceneStats_.sceneDraws;
    return true;
}

// F2c-2: a record's draw as a SceneView, without the composition's results of this frame: the retained packet (`c`: the record's own copy) is validated
// like replayBuild does against the record's own shaders / buffers / textures, the DrawBlock is derived from the record's textures, sampler states,
// alpha test and pass-set c5 plus the per-frame inputs (viewport, jitter, depth remap, LOD bias)
int Device::deriveScene(const CachedDraw& c, const DeriveIn& in, SceneView& out) {
    out = {};
    if (!c.valid || c.vs == nullptr || c.ps == nullptr || c.vs->module != c.vsModule || c.ps->module != c.fsModule) return 1;
    if (phases_.current() != c.phase) return 2;
    if (!replayAllowed(c)) return 3;
    // the enhancement variant decision (replayBuild's, with the ambient test answered from the record's lighting instead of the device's registers)
    if (c.v.giOn != giDraws_.on || c.v.replaceLightmaps != giDraws_.replaceLightmaps || c.v.pack != (packProvider_ && !packBypass_) || c.v.gbufGate != gbuf_.active || gbuf_.active ||
        !c.v.sceneActive || c.v.scenePending) {
        return 4;
    }
    if (c.v.gate && ((c.v.staticOk && in.ambientLit) != c.v.wanted)) return 4;
    if (c.v.gate && c.v.wanted) {
        // the variant the draw was offered (None while it was still being built): a module that exists now makes the draw take another pixel shader and pipeline (variant(): read only)
        const std::size_t vi = static_cast<std::size_t>(VariantSlot::Gi);
        const rhi::ShaderModule now = c.ps->variantState[vi].load(std::memory_order_acquire) == 2 ? c.ps->variants[vi].load(std::memory_order_acquire) : rhi::ShaderModule::None;
        if (now != c.v.fsAsked) return 4;
    }
    if (c.v.vsUsed != c.vs->module || c.v.biasFor != geometryLodBias_) return 4;
    CachedDraw::Env env{};
    env.vp = viewport_;
    std::memcpy(env.scissor, scissor_, sizeof(scissor_));
    env.colorCount = colorCount_;
    for (std::uint32_t i = 0; i < colorCount_ && i < kRenderTargets; ++i) env.colorFormats[i] = colorFormats_[i];
    env.depthFormat = depthFormat_;
    env.stencilFormat = stencilFormat_;
    env.depthReversed = boundDepthReversed() ? 1u : 0u;
    if (std::memcmp(&env, &c.env, sizeof(env)) != 0) return 5;
    float jx = 0.0f, jy = 0.0f;
    geometryJitter(jx, jy);
    const bool jit = jx != 0.0f || jy != 0.0f;
    if (jit && (alphaDither_ || cutoutThicken_ * std::clamp(renderRatio_ - 1.0f, 0.0f, 1.0f) > 0.0f)) return 8;
    for (std::uint32_t i = 0; i < 4; ++i) {
        if (((c.streamMask >> i) & 1u) == 0) continue;
        if (in.streams[i] == nullptr) return 6;
        const Buffer::Binding g = in.streams[i]->gpu(in.strides[i]);
        if (g.buffer != c.stream[i].buffer || g.offset != c.stream[i].offset) return 6;
    }
    if (in.indices == nullptr) return 6;
    const Buffer::Binding ib = in.indices->gpu(in.indices->index32() ? 4 : 2);
    if (ib.buffer != c.ib.buffer || ib.offset != c.ib.offset) return 6;
    // the DrawBlock
    shader::abi::DrawBlock& b = out.block;
    b = {};
    const float lodBias = jit ? geometryLodBias_ : 0.0f;
    static constexpr std::uint32_t kSs[7] = {SAMP_ADDRESSU, SAMP_ADDRESSV, SAMP_MAGFILTER, SAMP_MINFILTER, SAMP_MIPFILTER, SAMP_MIPMAPLODBIAS, SAMP_SRGBTEXTURE};
    for (std::uint32_t mask = c.ps->samplerMask & 0xFFFFu; mask != 0; mask &= mask - 1) {
        const std::uint32_t slot = static_cast<std::uint32_t>(std::countr_zero(mask));
        const shader::Reflection& r = c.ps->reflection;
        Texture* t = in.tex[slot];
        if (t != nullptr && (t->isNull() || t->pending_ || t->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || t->bindlessSlot() != c.texSlot[slot])) return 7;
        const bool kindMatches = t != nullptr && ((r.samplers[slot] == shader::TextureType::Cube) == (t->desc().type == TextureType::Cube)) &&
                                 ((r.samplers[slot] == shader::TextureType::Volume) == (t->desc().type == TextureType::Volume));
        if (t != nullptr && t == substituteFrom_) return 7;
        b.tex[slot] = t != nullptr && kindMatches ? t->bindlessSlot() : game_->defaultTextureSlot(static_cast<std::uint32_t>(r.samplers[slot]));
        std::uint32_t states[SAMP_COUNT];
        // (the seven states the composition owns from the record; the others - ADDRESSW, border colour, max anisotropy, max mip level - are the device's settings of the stage)
        std::memcpy(states, samp_[slot].data(), sizeof(states));
        for (std::uint32_t i = 0; i < 7; ++i) states[kSs[i]] = in.ss[slot][i];
        if (lodBias != 0.0f) states[SAMP_MIPMAPLODBIAS] = std::bit_cast<std::uint32_t>(asFloat(states[SAMP_MIPMAPLODBIAS]) + lodBias);
        b.smp[slot] = lookupSamplerStates(states);
    }
    b.posFix[0] = 1.0f / static_cast<float>(std::max(1u, viewport_.width)) + jx;
    b.posFix[1] = -1.0f / static_cast<float>(std::max(1u, viewport_.height)) + jy;
    depthRemap(b.posFix[2], b.posFix[3]);
    b.alphaFunc = in.alphaFunc;
    b.alphaRef = in.alphaRef;
    if (c.v.giDraw && giDraws_.on && giDraws_.replaceLightmaps) {
        for (const Semantic sem : {Semantic::LightMap1, Semantic::LightMap2}) {
            const auto& r = c.ps->semantics[sem];
            if (r.present() && r.reg < 16) b.tex[r.reg] = 0;
        }
    }
    if (c.v.drawn && c.v.pack) b.pad[1] = packWord();  // (the pack's word of the draw's material: the tag is the one the caller set)
    std::memcpy(b.psC5, in.psC5, sizeof(b.psC5));
    // the packet: the retained one, in its scene form
    out.scene = c.scenePipeline != GamePipeline::None;
    out.pipeline = c.scenePipeline;
    if (out.scene) {
        if (!ensureSceneRef()) return 9;
        out.viHash = hashVi(*internSceneVi(c.vi));
    } else {
        out.viHash = hashVi(c.vi);
    }
    out.dynHash = hashDyn(c.dyn);
    out.indexBuffer = c.packet.indexBuffer;
    out.indexOffset = c.packet.indexOffset;
    out.index32 = c.packet.index32;
    out.first = c.packet.first;
    out.count = c.packet.count;
    out.vertexOffset = c.packet.vertexOffset;
    out.flags = c.packet.push.flags;
    out.valid = true;
    return 0;
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
