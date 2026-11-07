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

#include "core/f2c4_measure.h"
#include "d3d9/device.h"
#include "d3d9/scene_hash.h"

namespace rvk::d3d9 {

namespace {

constexpr std::uint32_t kSceneRunMax = 1024;  // records per indirect call (the ref stream's length)

std::uint64_t alignUp16(std::uint64_t v) { return (v + 15) & ~std::uint64_t{15}; }

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

// scene ABI v2: the ref stream of one view slot: (i, slot) for i < kSceneRunMax (a draw's view slot is the ref's y)
bool Device::ensureSceneRefV2(std::uint32_t slot) {
    if (slot >= kSceneRefSlots) return false;
    SceneRefStream& r = sceneRefV2_[slot];
    if (r.valid) return true;
    rhi::RingAlloc a = game_->allocRing(static_cast<std::uint64_t>(kSceneRunMax) * 8, 16);
    if (a.ptr == nullptr) return false;
    auto* w = static_cast<std::uint32_t*>(a.ptr);
    for (std::uint32_t i = 0; i < kSceneRunMax; ++i) {
        w[2 * i] = i;
        w[2 * i + 1] = slot;
    }
    r.buffer = a.buffer;
    r.offset = a.offset;
    r.valid = true;
    return true;
}

// what prepareDraw decides for an eligible draw (the main view's static mesh z / solid draws issued by the static ops): the pipeline of its scene form
// when its scene shaders and that pipeline exist, else None (the draw goes the normal way; the missing pieces are requested in the background)
void Device::sceneDecide(const PipelineDesc& pd, bool giDraw, bool captureDraw, bool uberTaken, draws::Kind kind, const scene::Phase& phase, bool geomOk) {
    if (capture_ != nullptr) {
        lastVariant_.sceneActive = true;
        lastVariant_.scenePending = false;
    }
    const bool tree = sceneTrees_ && sceneV2_ && kind == draws::Kind::SpeedTree;  // F4-3: the scene ABI v2 form only (scenePipe_ stays None: the composed draw is drawn as it was)
    if ((kind != draws::Kind::StaticMesh && !(sceneBb_ && kind == draws::Kind::SpeedTreeBillboard) && !tree) || phase.view != scene::View::Main ||
        (phase.pass != scene::Pass::Solid && phase.pass != scene::Pass::DepthPrepass)) {
        return;
    }
    // the billboards' vertex module: the plain one or the fade variant (lod.trees != 1: Device::lodModule), each with a scene form
    const bool lodVs = kind == draws::Kind::SpeedTreeBillboard && pd.vs != vs_->module && pd.vs == vs_->lodModule;
    if (kind == draws::Kind::SpeedTreeBillboard && pd.vs != vs_->module && !lodVs) {
        ++sceneStats_.fbVariantKind;
        return;
    }
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
    const rhi::ShaderModule vsM = tree ? rhi::ShaderModule::None : variant(vs_, lodVs ? VariantSlot::SceneVsLod : VariantSlot::SceneVs);
    const rhi::ShaderModule fsM = tree ? rhi::ShaderModule::None : variant(ps_, sceneFs);
    if (!tree && (vsM == rhi::ShaderModule::None || fsM == rhi::ShaderModule::None)) {
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
    if (!tree) {
        sd.vs = vsM;
        sd.fs = fsM;
    }
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
    if (!tree) {
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
    // F2c-4-0: the scene ABI v2 form of the same draw (retained draws that the owned path replays: the main view's static meshes); the v1 form stays what the composed
    // path draws. Not ready yet = the draw is not retained as replayable (scenePending), the v1 form still draws it now
    if (sceneV2_ && (kind == draws::Kind::StaticMesh || tree)) {
        const rhi::ShaderModule vs2 = variant(vs_, VariantSlot::SceneVs2);
        const rhi::ShaderModule fs2 = variant(ps_, sceneView2Of(sceneFs));
        if (vs2 == rhi::ShaderModule::None || fs2 == rhi::ShaderModule::None) {
            const bool failed = vs_->variantState[static_cast<std::size_t>(VariantSlot::SceneVs2)].load() == 3 || ps_->variantState[static_cast<std::size_t>(sceneView2Of(sceneFs))].load() == 3;
            if (!failed) {
                ++sceneStats_.fbVariantPending;
                pending();
            }
            return;
        }
        PipelineDesc sd2 = sd;
        sd2.vs = vs2;
        sd2.fs = fs2;
        const GamePipeline sp2 = findPipeline(sd2);
        if (sp2 == GamePipeline::None) {
            game_->prewarmOncePipeline(sd2);
            ++sceneStats_.fbPipelinePending;
            pending();
            return;
        }
        scenePipe2_ = sp2;
    }
}

// the scene vertex input of a draw: its normal one + the ref stream (instance-rate binding after the draw's own, attribute 24). vertexShift is zeroed: the
// per-draw vertexOffset of the indirect arguments already carries it
const VertexInputValues* Device::internSceneVi(const VertexInputValues& n, std::int32_t viewSlot) {
    const SceneRefStream& ref = viewSlot >= 0 ? sceneRefV2_[static_cast<std::uint32_t>(viewSlot) % kSceneRefSlots] : sceneRef_;
    return internSceneViAt(n, ref.buffer, ref.offset);
}

// ... with the ref stream at an explicit buffer / offset (F2c-4a: the frame's ref list of the persistent records)
const VertexInputValues* Device::internSceneViAt(const VertexInputValues& n, std::uint64_t refBuffer, std::uint64_t refOffset) {
    VertexInputValues& v = sceneViWork_;
    v = n;
    v.vertexShift = 0;
    const std::uint32_t idx = v.bufferCount;
    v.buffers[idx] = refBuffer;
    v.offsets[idx] = refOffset;
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
bool Device::toScenePacket(DrawPacket& p, GamePipeline pipeline, std::int32_t viewSlot, std::uint32_t c5w) {
    // guard: every address a scene shader can read (the eight window bases, the DrawBlock, the frame block) must be set; else the normal path draws it
    // (scene ABI v2: the view windows come from the view table, whose base and slot are checked instead of the record's three view window bases)
    const bool v2 = viewSlot >= 0;
    bool nullAddress = p.push.draw == 0 || p.push.frame == 0 || sceneFaultInject_;
    for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) nullAddress |= p.push.vsWin[w] == 0 || p.push.psWin[w] == 0;
    if (v2) nullAddress |= !sceneViewsReady() || static_cast<std::uint32_t>(viewSlot) >= sceneViews_.slots || static_cast<std::uint32_t>(viewSlot) >= kSceneRefSlots;
    const SceneRefStream& refStream = v2 && static_cast<std::uint32_t>(viewSlot) < kSceneRefSlots ? sceneRefV2_[viewSlot] : sceneRef_;
    if (nullAddress || !refStream.valid || refStream.buffer == 0 || p.vi == nullptr || p.pipeline == GamePipeline::None) {
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
    if (v2) {
        // ABI v2: the per-frame inputs live in the view block (the record keeps the material / lighting windows, the DrawBlock, the id and the mask bits of c5.w)
        r.vsWin[0] = r.vsWin[1] = r.psWin[0] = r.psWin[1] = 0;
        r.c5w = c5w;
    }
    sceneRecs_.push_back(r);
    sceneRecSlots_.resize(sceneRecs_.size(), 0);
    sceneRecSlots_.back() = v2 ? static_cast<std::uint32_t>(viewSlot) + 1 : 0;
    p.scene = static_cast<std::uint32_t>(sceneRecs_.size());
    p.pipeline = pipeline;
    p.vi = internSceneVi(*p.vi, viewSlot);
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

// the DrawBlock's posFix of the view bound now: the half pixel, the camera jitter, the depth remap (a view block of scene ABI v2 holds exactly this)
void Device::currentPosFix(float out[4], float& jx, float& jy) {
    geometryJitter(jx, jy);
    out[0] = 1.0f / static_cast<float>(std::max(1u, viewport_.width)) + jx;
    out[1] = -1.0f / static_cast<float>(std::max(1u, viewport_.height)) + jy;
    depthRemap(out[2], out[3]);
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
    {
        // (replayBuild's vertex module choice: the billboards' fade variant while lod.trees != 1)
        rhi::ShaderModule vsM = c.vs->module;
        if (c.kind == draws::Kind::SpeedTreeBillboard && billboardFadeScale_ != 1.0f && c.vs->lodModule != rhi::ShaderModule::None) vsM = c.vs->lodModule;
        if (c.v.vsUsed != vsM || c.v.biasFor != geometryLodBias_) return 4;
    }
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
        if (t != nullptr && (t->isNull() || t->pending_ || t->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || t->bindlessSlot() != c.texSlot[slot])) {
            static int logged = 0;
            if (logged < 8) {
                ++logged;
                spdlog::info("d3d9: owned replay: texture differs: slot {} null {} pending {} layout {} bindless {} vs {}, {}x{} role {}", slot, t->isNull(), t->pending_.load(), static_cast<int>(t->layout()),
                             t->bindlessSlot(), c.texSlot[slot], t->desc().width, t->desc().height, scene::name(t->role()));
            }
            return 7;
        }
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
    {
        float jx2, jy2;
        currentPosFix(b.posFix, jx2, jy2);
    }
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

// F2c-3: every window the shaders of the draw being prepared read came from the caller's tables (splitStage's `ownedServed_`): a window that is not served
// would be read from the register file, which an owned draw never sets
bool Device::ownedWindowsServed(std::uint32_t* stageOut, std::uint32_t* windowOut, std::uint32_t* needOut) const {
    if (!drawSplit_[0] || !drawSplit_[1]) return false;
    for (std::uint32_t stage = 0; stage < 2; ++stage) {
        const Shader& sh = stage == 0 ? *vs_ : *ps_;
        const std::uint32_t* first = stage == 0 ? shader::abi::kVsWindowFirst : shader::abi::kPsWindowFirst;
        for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) {
            if (stage == 1 && w == 1) continue;  // (c5: the DrawBlock's psC5)
            std::uint32_t end = sh.reflection.windowEnd[w];
            if (stage == 1 && w == 3) end = std::max(end, psWindowMinEnd_);
            const std::uint32_t need = end > first[w] ? end - first[w] : 0;
            if (need != 0 && (ownedServed_[stage] >> w & 1u) == 0) {
                if (stageOut != nullptr) *stageOut = stage;
                if (windowOut != nullptr) *windowOut = w;
                if (needOut != nullptr) *needOut = need;
                return false;
            }
        }
    }
    return true;
}

bool Device::ownedObserversOk() const {
    for (const DrawObserver* o : observers_) {
        if (o->registersRead() == DrawObserver::Registers::All) return false;
    }
    return true;
}

// the register images the windows hold (their CPU twins) into the register file, for the observers that read material constants of the draw (the RT scene's
// uv map); the caller restores the file after its owned draws
void Device::loadOwnedRegisters(const OwnedWindows& ow) {
    for (std::uint32_t stage = 0; stage < 2; ++stage) {
        const std::uint32_t* first = stage == 0 ? shader::abi::kVsWindowFirst : shader::abi::kPsWindowFirst;
        for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) {
            const OwnedWindows::Win& x = ow.w[stage][w];
            if (x.kind == OwnedWindows::Kind::None || x.image == nullptr || x.covered == 0) continue;
            if (stage == 0) setVertexShaderConstantF(first[w], x.image, x.covered);
            else setPixelShaderConstantF(first[w], x.image, x.covered);
            ++ownedReplayStats.registersLoaded;
        }
    }
}

std::uint64_t Device::f2c4BucketKey(const CachedDraw& c) const {
    std::uint64_t h = hashVi(c.vi);
    h = f2c4::mix64(h, hashDyn(c.dyn));
    h = f2c4::mix64(h, static_cast<std::uint64_t>(c.scenePipeline2));
    h = f2c4::mix64(h, c.packet.indexBuffer);
    h = f2c4::mix64(h, c.packet.indexOffset);
    return f2c4::mix64(h, c.packet.index32 ? 1u : 0u);
}

int Device::replayOwned(const CachedDraw& c, const DeriveIn& in, const OwnedWindows& ow) {
    OwnedReplayStats& st = ownedReplayStats;
    ++st.attempts;
    // F2c-4-0 item 3 (RVK_F2C4_MEASURE): the time per piece of an owned draw's submit
    using MClock = std::chrono::steady_clock;
    const bool measure = f2c4::enabled();
    f2c4::Counters& mc = f2c4::counters();
    MClock::time_point mT = measure ? MClock::now() : MClock::time_point{};
    const auto lap = [&](std::uint64_t& acc) {
        if (!measure) return;
        const MClock::time_point now = MClock::now();
        acc += f2c4::nsBetween(mT, now);
        mT = now;
    };
    if (measure) ++mc.replays;
    const auto refuse = [&](int why) {
        ++st.why[why];
        return why;
    };
    if (!sceneActive()) return refuse(10);
    if (!c.valid || vs_.get() != c.vs || ps_.get() != c.ps || vs_->module == rhi::ShaderModule::None || ps_->module == rhi::ShaderModule::None) return refuse(11);
    syncPhaseRegions();
    if (rendering_ && (scopeGbuf_ != gbufferScope() || scopeSecondary_ != wantSecondaryScope())) endRendering();
    if (!beginRendering()) return refuse(12);
    lap(mc.prepNs);
    // scene ABI v2 (the caller's view windows point into a view table slot): the pipeline, the table, the slot and the slot's windows must all be the v2 ones
    const bool v2 = ow.viewSlot != OwnedWindows::kNoView;
    if (v2) {
        if (c.scenePipeline2 == GamePipeline::None || !sceneViewsReady()) return refuse(16);
        if (ow.viewSlot >= sceneViews_.slots || ow.viewSlot >= kSceneRefSlots) return refuse(17);
        using VT = std::uint64_t;
        const rhi::GpuRef t = sceneViews_.base;
        const auto base = [&](std::int64_t off) { return static_cast<VT>(t.plus(static_cast<std::int64_t>(ow.viewSlot) * shader::abi::kViewBlock2Size + off).bits); };
        if (ow.w[0][0].base.bits != base(shader::abi::kViewVs0Offset) ||
            ow.w[0][1].base.bits != base(static_cast<std::int64_t>(shader::abi::kViewVs1Offset) - static_cast<std::int64_t>(shader::abi::kVsWindowFirst[1]) * 16) ||
            ow.w[1][0].base.bits != base(shader::abi::kViewPs0Offset)) {
            return refuse(18);
        }
    }
    SceneView sv;
    const int deriveWhy = deriveScene(c, in, sv);
    lap(mc.deriveNs);
    if (deriveWhy != 0) return refuse(deriveWhy);
    if (!(v2 ? c.scenePipeline2 != GamePipeline::None : sv.scene) || !c.packet.batchable) return refuse(9);
    if (v2) {
        // the view block holds what the draw's own derivation would have put in the DrawBlock (posFix incl. jitter + depth remap, c5.xyz): a view changed since the slot was set refuses
        const shader::abi::ViewBlock2& vb = sceneViews_.shadow[ow.viewSlot];
        if (std::memcmp(vb.posFix, sv.block.posFix, sizeof(vb.posFix)) != 0 || std::memcmp(vb.c5, in.psC5, 3 * sizeof(float)) != 0) return refuse(19);
    }
    DrawPacket p = c.packet;
    p.vi = internVi(c.vi);
    p.dyn = internDyn(c.dyn);
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
    lap(mc.prepNs);
    {
        bool opaqueObserver = false;
        if (c.kind == draws::Kind::StaticMesh && c.phase.view == scene::View::Main && c.phase.pass == scene::Pass::Solid) {
            for (DrawReplacer* r : replacers_) opaqueObserver |= r->wantsOpaque();
        }
        if (!observers_.empty() || opaqueObserver) {
            ReplaceQuery q = c.query;
            q.tag = &tag_;
            q.textures = c.tex;
            q.mainView = firings_.mainViews();
            q.objectRows = ownedObjectRows();
            captureRequest_ = {};
            captureRequestRw_ = {};
            captureInstances_ = false;
            bool loaded = false;
            const CoverQuery cq = coverOf(q);
            for (DrawObserver* o : observers_) {
                if (!o->observes(c.kind, c.phase)) continue;
                if (o->coversDraw(cq)) continue;  // F8-0
                if (!loaded) {
                    loadOwnedRegisters(ow);
                    loaded = true;
                }
                o->observe(*this, q);
            }
            if (opaqueObserver) {
                for (DrawReplacer* r : replacers_) {
                    if (!r->wantsOpaque()) continue;
                    if (!loaded) {
                        loadOwnedRegisters(ow);
                        loaded = true;
                    }
                    r->observeOpaque(*this, q);
                }
            }
            if (!captureRequest_.null()) return refuse(13);
        }
    }
    lap(mc.observerNs);
    uploadConstants(false, c.v.minEnd);
    {
        std::uint32_t stage = 0, window = 0, need = 0;
        if (!ownedWindowsServed(&stage, &window, &need)) {
            static std::uint32_t logged = 0;
            if (logged < 8) {
                ++logged;
                const OwnedWindows::Win& x = ow.w[stage][window];
                spdlog::info("d3d9: owned replay: the {} window {} is not served (the shader reads {} registers; offered kind {} covered {} base {:#x}, why {}; vs {:016x} ps {:016x}, split {} / {})",
                             stage == 0 ? "VS" : "PS", window, need, static_cast<int>(x.kind), x.covered, x.base.bits, static_cast<int>(x.why), vs_->hash, ps_->hash, drawSplit_[0], drawSplit_[1]);
            }
            return refuse(14);
        }
    }
    lap(mc.windowNs);
    shader::abi::DrawBlock& b = sv.block;
    // the packet's guard (toScenePacket) refuses a null address: nothing may be queued then
    rhi::RingAlloc db = game_->allocRing(sizeof(b), 16);
    if (db.ptr == nullptr) return refuse(15);
    std::memcpy(db.ptr, &b, sizeof(b));
    const std::uint32_t flags = p.push.flags;
    p.push = shader::abi::PushConstants{0, 0, db.ref.bits, frameBlockAddress().bits, 0, stats_.draws, flags};
    for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) {
        p.push.vsWin[w] = drawWin_[0][w].bits;
        p.push.psWin[w] = drawWin_[1][w].bits;
    }
    p.push.psWin[1] = db.ref.plus(static_cast<std::int64_t>(offsetof(shader::abi::DrawBlock, psC5)) - 5 * 16).bits;
    if (v2) {
        if (!ensureSceneRefV2(ow.viewSlot) || !toScenePacket(p, c.scenePipeline2, static_cast<std::int32_t>(ow.viewSlot), std::bit_cast<std::uint32_t>(b.psC5[3]))) return refuse(15);
        ++st.drawnV2;
    } else if (!ensureSceneRef() || !toScenePacket(p, c.scenePipeline)) {
        return refuse(15);
    }
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
    submitInterned(p);
    ++st.drawn;
    if (measure) {
        lap(mc.packetNs);
        // item 6: the distinct buckets of the frame's owned draws
        f2c4::beginFrame(stats_.frames);
        mc.frameBuckets.insert(f2c4BucketKey(c));
        ++mc.bucketDraws;
    }
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
    // (scene ABI v2: a run holds records of ONE view slot, which is the ref stream's y: the vertex input of its packets carries that stream)
    const std::uint32_t runSlot = sceneRecSlots_[first.scene - 1];
    for (const std::uint32_t at : run) {
        if (sceneRecSlots_[batch_[at].scene - 1] != runSlot || (runSlot != 0 && !sceneViewsReady())) {
            ++sceneStats_.fbNullAddress;
            spdlog::error("d3d9: scene run dropped: records of different ABI forms / view slots in one run");
            return;
        }
    }
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
    if (runSlot != 0) sp.views = sceneViews_.base.bits;
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
    sceneRecSlots_.clear();
    for (SceneRefStream& r : sceneRefV2_) r.valid = false;
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
