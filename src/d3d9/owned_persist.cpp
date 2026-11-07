// F2c-4a persistent owned draws (docs/research/frame-ownership.md 7.21, setting native.owned_persistent): the device's half. See device.h (ownedEnvEpoch ...).
//
// replayOwned (scene_draws.cpp) derives, builds and queues every owned draw every frame. Here the derivation result of a draw is a plain value that lives in
// gpuscene::drawrecords() (a DrawRecord + a DrawBlock per slot) and is rewritten only when something it was derived from changed:
//   the device's state of the pass (ownedEnvEpoch), the draw's own inputs (ownedDrawSig), the retained draw itself (its op record is rebuilt = new draw records).
// A frame then writes a ref list + argument block (the CPU knows every count) and queues ONE indirect draw per bucket (ownedFlush).

#include <algorithm>
#include <bit>
#include <cstring>

#include "d3d9/device.h"
#include "d3d9/scene_hash.h"
#include "gpuscene/owned_compact.h"
#include "gpuscene/verify.h"

namespace rvk::d3d9 {

namespace {

inline std::uint64_t hmix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}
inline std::uint64_t hmixF(std::uint64_t h, float f) { return hmix(h, std::bit_cast<std::uint32_t>(f)); }

}  // namespace

// everything deriveScene / replayOwned read from the device that is not the draw's: a hash of it, as an epoch (moves when the hash differs from this pass kind's last)
std::uint64_t Device::ownedEnvHash(bool solid, std::uint32_t viewSlot, bool& ok) const {
    ok = sceneActive() && sceneViewsReady() && viewSlot < sceneViews_.slots && splitConstants_ && !ubershader_.load(std::memory_order_relaxed) && tint_ == draws::TintMode::Off &&
         !(gbuf_.active && !gbufArmed_) && stats_.totalDraws >= options_.logDraws && !wp0_.on && customVertices_ == 0 && !up_.active;
    std::uint64_t h = 0xcbf29ce484222325ull;
    const scene::Phase ph = phases_.current();
    h = hmix(h, static_cast<std::uint64_t>(ph.view) | static_cast<std::uint64_t>(ph.pass) << 8);
    h = hmix(h, variantEpoch_);
    h = hmix(h, (giDraws_.on ? 1u : 0u) | (giDraws_.replaceLightmaps ? 2u : 0u) | (packProvider_ && !packBypass_ ? 4u : 0u) | (gbuf_.active ? 8u : 0u) | (gbufArmed_ ? 16u : 0u) | (alphaDither_ ? 32u : 0u));
    h = hmixF(hmixF(hmixF(hmixF(h, geometryLodBias_), cutoutThicken_), renderRatio_), billboardFadeScale_);
    h = hmix(h, reinterpret_cast<std::uintptr_t>(substituteFrom_));
    h = hmix(h, viewport_.x | static_cast<std::uint64_t>(viewport_.y) << 32);
    h = hmix(h, viewport_.width | static_cast<std::uint64_t>(viewport_.height) << 32);
    h = hmixF(hmixF(h, viewport_.minZ), viewport_.maxZ);
    for (const std::int32_t s : scissor_) h = hmix(h, static_cast<std::uint32_t>(s));
    h = hmix(h, colorCount_);
    for (std::uint32_t i = 0; i < colorCount_ && i < kRenderTargets; ++i) h = hmix(h, static_cast<std::uint64_t>(colorFormats_[i]));
    h = hmix(h, static_cast<std::uint64_t>(depthFormat_) | static_cast<std::uint64_t>(stencilFormat_) << 32);
    h = hmix(h, boundDepthReversed() ? 1u : 0u);
    {
        float jx = 0.0f, jy = 0.0f, zs = 0.0f, zb = 0.0f;
        geometryJitter(jx, jy);
        depthRemap(zs, zb);
        h = hmixF(hmixF(hmixF(hmixF(h, jx), jy), zs), zb);
    }
    {
        // the sampler states the derivation does NOT take from the record (ADDRESSW, border colour, max anisotropy ...): the seven it owns are overwritten per draw
        constexpr std::uint32_t kOwnedMask = 1u << SAMP_ADDRESSU | 1u << SAMP_ADDRESSV | 1u << SAMP_MAGFILTER | 1u << SAMP_MINFILTER | 1u << SAMP_MIPFILTER | 1u << SAMP_MIPMAPLODBIAS | 1u << SAMP_SRGBTEXTURE;
        static_assert(SAMP_COUNT <= 32);
        for (const auto& row : samp_) {
            std::uint64_t part = 0;
            for (std::uint32_t i = 0; i < SAMP_COUNT; ++i) {
                if ((kOwnedMask >> i & 1u) == 0) part = part * 0x100000001B3ull + row[i];
            }
            h = hmix(h, part);
        }
    }
    h = hmix(h, packStamp_ ? packStamp_() : 0xffffffffull);
    h = hmix(h, viewSlot | (solid ? 0x100u : 0u));
    h = hmix(h, sceneViews_.base.bits);
    return h;
}

std::uint64_t Device::ownedEnvEpoch(bool solid, std::uint32_t viewSlot, bool& ok) {
    const std::uint64_t h = ownedEnvHash(solid, viewSlot, ok);
    const std::size_t k = solid ? 1 : 0;
    if (ownedEnvHash_[k] != h || ownedEnvEpoch_[k] == 0) {
        ownedEnvHash_[k] = h;
        ownedEnvEpoch_[k] = ++ownedEnvCounter_;
    }
    return ownedEnvEpoch_[k];
}

bool Device::ownedEnvSame(bool solid, std::uint32_t viewSlot, std::uint64_t epoch) const {
    bool ok = false;
    const std::uint64_t h = ownedEnvHash(solid, viewSlot, ok);
    const std::size_t k = solid ? 1 : 0;
    return h == ownedEnvHash_[k] && epoch == ownedEnvEpoch_[k] && epoch != 0;
}

std::uint64_t Device::ownedBucketKey(const CachedDraw& c, std::uint32_t viewSlot) const {
    std::uint64_t h = hashVi(c.vi);
    h = hmix(h, hashDyn(c.dyn));
    h = hmix(h, static_cast<std::uint64_t>(c.scenePipeline2));
    h = hmix(h, c.packet.indexBuffer);
    h = hmix(h, c.packet.indexOffset);
    h = hmix(h, c.packet.index32 ? 1u : 0u);
    return hmix(h, viewSlot);
}

// the draw's own inputs of the derivation (deriveScene's per-draw reads) + the four window bases the record holds
std::uint64_t Device::ownedDrawSig(const CachedDraw& c, const void* const* streams, const std::uint32_t* strides, const void* indices, const void* const* tex, bool ambientLit, const OwnedWindows& ow,
                                   bool& allowed) {
    allowed = c.valid && c.vs != nullptr && c.ps != nullptr && c.vs->module == c.vsModule && c.ps->module == c.fsModule && replayAllowed(c);
    std::uint64_t h = hmix(0x84222325cbf29ce4ull, ambientLit ? 1u : 0u);
    // the variant the draw was offered while it was still being built (a module that exists now = another pixel shader and pipeline)
    if (c.v.gate && c.v.wanted) {
        const std::size_t vi = static_cast<std::size_t>(VariantSlot::Gi);
        const rhi::ShaderModule now = c.ps->variantState[vi].load(std::memory_order_acquire) == 2 ? c.ps->variants[vi].load(std::memory_order_acquire) : rhi::ShaderModule::None;
        h = hmix(h, static_cast<std::uint64_t>(now));
    }
    for (std::uint32_t i = 0; i < 4; ++i) {
        if (((c.streamMask >> i) & 1u) == 0) continue;
        const Buffer* b = static_cast<const Buffer*>(streams[i]);
        if (b == nullptr) {
            h = hmix(h, ~0ull);
            continue;
        }
        const Buffer::Binding g = const_cast<Buffer*>(b)->gpu(strides[i]);
        h = hmix(hmix(h, g.buffer), g.offset);
    }
    {
        Buffer* ib = const_cast<Buffer*>(static_cast<const Buffer*>(indices));
        if (ib == nullptr) {
            h = hmix(h, ~0ull);
        } else {
            const Buffer::Binding g = ib->gpu(ib->index32() ? 4 : 2);
            h = hmix(hmix(h, g.buffer), g.offset);
        }
    }
    for (std::uint32_t mask = c.ps->samplerMask & 0xFFFFu; mask != 0; mask &= mask - 1) {
        const std::uint32_t slot = static_cast<std::uint32_t>(std::countr_zero(mask));
        const Texture* t = static_cast<const Texture*>(tex[slot]);
        if (t == nullptr) {
            h = hmix(h, 0x1ull);
            continue;
        }
        const bool ready = !t->isNull() && !t->pending_ && t->layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        h = hmix(h, (ready ? 0x100000000ull : 0x200000000ull) | t->bindlessSlot() | (t == substituteFrom_ ? 0x400000000ull : 0ull));
    }
    if (c.v.drawn && c.v.pack) {
        const Texture* t = c.v.packTex;
        const bool same = c.v.packStamped && packStamp_ && packStamp_() == c.v.packStamp &&
                          (t == nullptr || (!t->pending_ && t->layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && t->bindlessSlot() == c.v.packTexSlot));
        h = hmix(h, same ? 0xfeedfaceull : (0x100000000ull | packWord()));
    }
    h = hmix(hmix(h, c.packet.count | static_cast<std::uint64_t>(c.packet.first) << 32), static_cast<std::uint32_t>(c.packet.vertexOffset));  // (the record's args)
    static constexpr std::uint32_t kStage[4][2] = {{0, 2}, {0, 3}, {1, 2}, {1, 3}};
    for (const auto& sw : kStage) {
        const OwnedWindows::Win& w = ow.w[sw[0]][sw[1]];
        h = hmix(hmix(h, w.base.bits), static_cast<std::uint64_t>(w.kind) | static_cast<std::uint64_t>(w.covered) << 8);
    }
    return h;
}

int Device::ownedBuild(const CachedDraw& c, const SceneView& sv, const OwnedWindows& ow, rhi::GpuRef blockAddress, rhi::GpuRef zeroBlock, OwnedPersist& out) {
    if (blockAddress.null() || zeroBlock.null() || c.vs == nullptr || c.ps == nullptr) return 15;
    out = {};
    shader::abi::DrawRecord& r = out.rec;
    // the window bases as splitStage makes them when every window the shaders read is the caller's: a window nobody reads points at the zero block
    for (std::uint32_t stage = 0; stage < 2; ++stage) {
        const Shader& sh = stage == 0 ? *c.vs : *c.ps;
        const std::uint32_t* first = stage == 0 ? shader::abi::kVsWindowFirst : shader::abi::kPsWindowFirst;
        for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) {
            std::uint64_t base;
            if (stage == 1 && w == 1) {
                base = 0;  // (c5: the DrawBlock's psC5 in v1; the view block's + the record's c5w in v2)
            } else {
                std::uint32_t end = sh.reflection.windowEnd[w];
                if (stage == 1 && w == 3) end = std::max(end, c.v.minEnd);
                const std::uint32_t need = end > first[w] ? end - first[w] : 0;
                if (need == 0) {
                    base = zeroBlock.plus(-static_cast<std::int64_t>(first[w]) * 16).bits;
                } else {
                    const OwnedWindows::Win& x = ow.w[stage][w];
                    if (x.kind == OwnedWindows::Kind::None || x.covered < need || x.base.null()) return 14;
                    base = x.base.bits;
                }
            }
            (stage == 0 ? r.vsWin : r.psWin)[w] = base;
        }
    }
    // the guard of toScenePacket on the bases, as the push block held them (c5's window is patched in there)
    for (std::uint32_t w = 0; w < shader::abi::kConstWindows; ++w) {
        if (r.vsWin[w] == 0 && !(w < 2)) return 15;
        if (r.psWin[w] == 0 && w != 1 && w != 0) return 15;
    }
    out.blk = sv.block;
    const float c5w = out.blk.psC5[3];
    std::memset(out.blk.posFix, 0, sizeof(out.blk.posFix));  // (the v2 shaders read the view block's posFix / c5.xyz)
    std::memset(out.blk.psC5, 0, sizeof(out.blk.psC5));
    r.vsWin[0] = r.vsWin[1] = r.psWin[0] = r.psWin[1] = 0;  // (the view's windows come from the view table)
    r.draw = blockAddress.bits;
    r.prevVs = 0;
    r.drawId = 0;  // (no shader of the corpus / the variants reads it)
    r.flags = c.packet.push.flags;
    r.c5w = std::bit_cast<std::uint32_t>(c5w);
    // the indirect arguments the GPU compaction copies (F2c-4c; the CPU path writes the same numbers into the ring block)
    r.args[0] = c.packet.count;
    r.args[1] = c.packet.first;
    r.args[2] = static_cast<std::uint32_t>(c.packet.vertexOffset);
    r.args[3] = 0;
    out.bucketKey = ownedBucketKey(c, ow.viewSlot);
    return 0;
}

int Device::ownedDerive(const CachedDraw& c, const DeriveIn& in, const OwnedWindows& ow, rhi::GpuRef blockAddress, rhi::GpuRef zeroBlock, bool checkView, OwnedPersist& out) {
    OwnedReplayStats& st = ownedReplayStats;
    ++st.attempts;
    ++ownedPersistStats.derived;
    const auto refuse = [&](int why) {
        ++st.why[why];
        ++ownedPersistStats.refused;
        return why;
    };
    if (checkView && !sceneActive()) return refuse(10);
    if (!c.valid || vs_.get() != c.vs || ps_.get() != c.ps || vs_->module == rhi::ShaderModule::None || ps_->module == rhi::ShaderModule::None) return refuse(11);
    if (ow.viewSlot == OwnedWindows::kNoView || c.scenePipeline2 == GamePipeline::None || !sceneViewsReady()) return refuse(16);
    if (ow.viewSlot >= sceneViews_.slots) return refuse(17);
    {
        const rhi::GpuRef t = sceneViews_.base;
        const auto base = [&](std::int64_t off) { return t.plus(static_cast<std::int64_t>(ow.viewSlot) * shader::abi::kViewBlock2Size + off).bits; };
        if (ow.w[0][0].base.bits != base(shader::abi::kViewVs0Offset) ||
            ow.w[0][1].base.bits != base(static_cast<std::int64_t>(shader::abi::kViewVs1Offset) - static_cast<std::int64_t>(shader::abi::kVsWindowFirst[1]) * 16) ||
            ow.w[1][0].base.bits != base(shader::abi::kViewPs0Offset)) {
            return refuse(18);
        }
    }
    SceneView sv;
    const int why = deriveScene(c, in, sv);
    if (why != 0) return refuse(why);
    if (c.scenePipeline2 == GamePipeline::None || !c.packet.batchable) return refuse(9);
    if (checkView) {
        const shader::abi::ViewBlock2& vb = sceneViews_.shadow[ow.viewSlot];
        if (std::memcmp(vb.posFix, sv.block.posFix, sizeof(vb.posFix)) != 0 || std::memcmp(vb.c5, in.psC5, 3 * sizeof(float)) != 0) return refuse(19);
    }
    const int rc = ownedBuild(c, sv, ow, blockAddress, zeroBlock, out);
    return rc != 0 ? refuse(rc) : 0;
}

// F2c-4b: an owned draw is "bulk eligible" when the bulk observers' contract fits it: a plain static mesh draw (the RT scene's table entries cover these; every other kind keeps the
// per-draw path, where an observer may still ask for a capture before the draw is queued)
static bool ownedBulkEligible(const Device::CachedDraw& c) {
    return c.kind == draws::Kind::StaticMesh && !c.query.instanced && !c.query.blending && c.query.primitiveType == 4;
}

bool Device::ownedObserve(const CachedDraw& c, const OwnedWindows& ow, const OwnedObsInfo& info) {
    ++ownedPersistStats.observed;
    bool opaqueObserver = false;
    if (c.kind == draws::Kind::StaticMesh && c.phase.view == scene::View::Main && c.phase.pass == scene::Pass::Solid) {
        for (DrawReplacer* r : replacers_) opaqueObserver |= r->wantsOpaque();
    }
    if (observers_.empty() && !opaqueObserver) return true;
    // the observers' turn: bulk observers (observeOwned, once per pass) get a record view, the others see the draw now
    const bool eligible = ownedBulkOn_ && ownedBulkEligible(c);
    std::uint32_t wanted = 0;
    bool perDraw = opaqueObserver;
    for (std::size_t i = 0; i < observers_.size(); ++i) {
        DrawObserver* o = observers_[i];
        if (!o->observes(c.kind, c.phase)) continue;
        if (eligible && i < 32 && o->bulkOwned()) wanted |= 1u << i;
        else perDraw = true;
    }
    if (wanted != 0) {
        OwnedRecordView& v = ownedBulk_.emplace_back();
        v.cd = &c;
        v.ow = &ow;
        v.tag = tag_;
        v.id = info.id;
        v.wanted = wanted;
        v.sig = info.sig;
        v.epoch = info.epoch;
        v.changed = info.changed;
        const std::uint32_t k = ownedBulkKind_;
        std::vector<std::uint32_t>& seen = ownedBulkSeen_[k];
        if (seen.size() <= info.id) seen.resize(static_cast<std::size_t>(info.id) + 256, 0);
        v.entered = ownedBulkPrevSerial_[k] == 0 || seen[info.id] != ownedBulkPrevSerial_[k];
        seen[info.id] = ownedBulkSerial_[k];
        ownedBulkIds_[k].push_back(info.id);
    }
    if (!perDraw) return true;
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
    for (std::size_t i = 0; i < observers_.size(); ++i) {
        DrawObserver* o = observers_[i];
        if (!o->observes(c.kind, c.phase)) continue;
        if (eligible && i < 32 && o->bulkOwned()) continue;
        if (o->coversDraw(cq)) {  // F8-0 pre-filter (the persistent loop had none)
            ++ownedBulkStats.perDrawCovered;
            continue;
        }
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
    return captureRequest_.null();
}

// ---- F2c-4b bulk observers (frame-ownership.md 7.23) -------------------------------------------------------------------------------------------------

void Device::ownedBulkBegin(bool solid) {
    static const bool perDraw = [] {
        char v[8];
        return GetEnvironmentVariableA("RVK_F2C4B_PERDRAW", v, sizeof(v)) != 0;
    }();
    ownedBulkOn_ = !perDraw;
    const std::uint32_t k = solid ? 1u : 0u;
    ownedBulkKind_ = k;
    ownedBulk_.clear();
    // the previous pass of this kind is what `entered` / `left` are measured against
    ownedBulkPrevIds_[k].swap(ownedBulkIds_[k]);
    ownedBulkIds_[k].clear();
    ownedBulkPrevSerial_[k] = ownedBulkSerial_[k];
    ownedBulkSerial_[k] = ownedBulkSerial_[k] + 1;
    if (ownedBulkSerial_[k] == 0) {  // (wrapped: forget everything)
        std::fill(ownedBulkSeen_[k].begin(), ownedBulkSeen_[k].end(), 0u);
        ownedBulkSerial_[k] = 1;
        ownedBulkPrevSerial_[k] = 0;
    }
}

void Device::ownedBulkFlush(bool last) {
    const std::uint32_t k = ownedBulkKind_;
    ownedBulkLeft_.clear();
    if (last) {
        const std::vector<std::uint32_t>& seen = ownedBulkSeen_[k];
        for (const std::uint32_t id : ownedBulkPrevIds_[k]) {
            if (id < seen.size() && seen[id] != ownedBulkSerial_[k]) ownedBulkLeft_.push_back(id);
        }
    }
    if (ownedBulk_.empty() && ownedBulkLeft_.empty()) return;
    ++ownedBulkStats.flushes;
    ownedBulkStats.records += ownedBulk_.size();
    ownedBulkStats.left += ownedBulkLeft_.size();
    for (const OwnedRecordView& v : ownedBulk_) ownedBulkStats.entered += v.entered ? 1 : 0;
    OwnedFrame f;
    f.phase = ownedBulk_.empty() ? phases_.current() : ownedBulk_.front().cd->phase;
    f.records = ownedBulk_.data();
    f.count = ownedBulk_.size();
    f.left = ownedBulkLeft_.data();
    f.leftCount = ownedBulkLeft_.size();
    f.last = last;
    // (an observer may not change the registered observers; the list is walked by index)
    for (std::size_t i = 0; i < observers_.size() && i < 32; ++i) {
        DrawObserver* o = observers_[i];
        if (!o->bulkOwned()) continue;
        f.observerBit = 1u << i;
        o->observeOwned(*this, f);
    }
    ownedBulk_.clear();
}

Device::OwnedDrawScope::OwnedDrawScope(Device& d, const OwnedRecordView& r) : d_(d), savedOwned_(d.owned_), savedTag_(d.tag_), ow_(r.ow) {
    const CachedDraw& c = *r.cd;
    d.owned_ = r.ow;
    d.ownedReplay_ = true;
    d.tag_ = r.tag;
    // (the draw's shaders: what the per-draw path had bound while the observers looked at it)
    if (d.vertexShader() != c.vs) d.setVertexShaderRef(ShaderRef(const_cast<Shader*>(c.vs)));
    if (d.pixelShader() != c.ps) d.setPixelShaderRef(ShaderRef(const_cast<Shader*>(c.ps)));
    q_ = c.query;
    q_.tag = &d.tag_;
    q_.textures = c.tex;
    q_.mainView = d.firings_.mainViews();
    q_.objectRows = d.ownedObjectRows();
    d.captureRequest_ = {};
    d.captureRequestRw_ = {};
    d.captureInstances_ = false;
}

Device::OwnedDrawScope::~OwnedDrawScope() {
    if (!d_.captureRequest_.null()) ++d_.ownedBulkStats.lateCapture;
    d_.owned_ = savedOwned_;
    d_.ownedReplay_ = false;
    d_.tag_ = savedTag_;
}

void Device::OwnedDrawScope::loadRegisters() {
    if (loaded_) return;
    loaded_ = true;
    d_.loadOwnedRegisters(*ow_);
}

// the default of DrawObserver::observeOwned: the per-draw loop over the pass's records (what ownedObserve does for an observer that is not bulk)
void DrawObserver::observeOwned(Device& device, const OwnedFrame& frame) {
    for (std::size_t i = 0; i < frame.count; ++i) {
        const Device::OwnedRecordView& r = frame.records[i];
        if ((r.wanted & frame.observerBit) == 0) continue;
        Device::OwnedDrawScope scope(device, r);
        const ReplaceQuery& q = scope.query();
        if (!observes(r.cd->kind, r.cd->phase)) continue;
        if (coversDraw(coverOf(q))) continue;
        scope.loadRegisters();
        observe(device, q);
    }
}

bool Device::ownedBeginScope() {
    syncPhaseRegions();
    if (rendering_ && (scopeGbuf_ != gbufferScope() || scopeSecondary_ != wantSecondaryScope())) endRendering();
    return beginRendering();
}

bool Device::ownedReserve(std::uint32_t maxDraws, OwnedArena& arena) {
    arena = {};
    if (maxDraws == 0) return true;
    arena.list = game_->allocRing(static_cast<std::uint64_t>(maxDraws) * 8, 16);  // (the argument block of the CPU path is taken in the flush that needs it)
    if (arena.list.ptr == nullptr) {
        arena = {};
        ++sceneStats_.fbRingFull;
        return false;
    }
    arena.capacity = maxDraws;
    return true;
}

bool Device::ownedFlush(const std::vector<OwnedItem>& items, OwnedArena& arena, std::uint32_t viewSlot, rhi::GpuRef recordsRef, const shader::abi::DrawRecord* shadow, std::uint32_t slotCount) {
    if (items.empty()) return true;
    const std::uint32_t n = static_cast<std::uint32_t>(items.size());
    if (!arena.valid() || n > arena.capacity || recordsRef.null() || !sceneViewsReady() || viewSlot >= sceneViews_.slots) return false;
    const rhi::GpuRef frameBlock = frameBlockAddress();
    if (frameBlock.null() || shadow == nullptr) return false;
    for (const OwnedItem& it : items) {
        // (every address the shaders will read from the slot: it must hold a written record with a block address and its draw must be a scene draw of ours)
        if (it.cd == nullptr || it.slot >= slotCount || shadow[it.slot].draw == 0 || shadow[it.slot].vsWin[2] == 0 || shadow[it.slot].psWin[3] == 0 || it.cd->scenePipeline2 == GamePipeline::None) {
            ++ownedPersistStats.refused;
            return false;
        }
    }
    // F2c-4c: the GPU compaction copies each record's own args: they must be the draw's (the signature covers them; this is the per-item tripwire)
    gpuscene::OwnedCompact& compact = gpuscene::ownedcompact();
    bool gpu = compact.usable(*this);
    if (gpu) {
        // (every item on a check frame, else every 8th: the args are in the signature, and the record line they sit on is a cache line more per item than the guard above reads)
        const std::uint32_t step = compact.checkThisFrame(*this) ? 1u : 8u;
        for (std::uint32_t i = static_cast<std::uint32_t>(frameValue() & 7u) % step; i < n; i += step) {
            const OwnedItem& it = items[i];
            const shader::abi::DrawRecord& r = shadow[it.slot];
            if (r.args[0] != it.cd->packet.count || r.args[1] != it.cd->packet.first || r.args[2] != static_cast<std::uint32_t>(it.cd->packet.vertexOffset)) {
                ++compact.stats().argsMismatch;
                ++ownedPersistStats.refused;
                return false;
            }
        }
    }
    syncPhaseRegions();
    if (rendering_ && (scopeGbuf_ != gbufferScope() || scopeSecondary_ != wantSecondaryScope())) endRendering();
    if (!beginRendering()) return false;
    // buckets in the order they first appear; the items keep their order inside a bucket (the engine's order)
    ownedBucketKeys_.clear();
    ownedBucketStart_.clear();
    ownedBucketOf_.resize(n);
    std::uint32_t buckets = 0;
    {
        std::uint32_t cap = 64;
        while (cap < 2 * n) cap <<= 1;
        struct Slot {
            std::uint64_t key = 0;
            std::uint32_t bucket = 0xffffffffu;
        };
        static thread_local std::vector<Slot> table;
        table.assign(cap, {});
        const std::uint32_t mask = cap - 1;
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint64_t key = items[i].bucket;
            std::uint32_t at = static_cast<std::uint32_t>((key * 0x9E3779B97F4A7C15ull) >> 40) & mask;
            while (table[at].bucket != 0xffffffffu && table[at].key != key) at = (at + 1) & mask;
            if (table[at].bucket == 0xffffffffu) {
                table[at] = {key, buckets++};
                ownedBucketKeys_.push_back(key);
                ownedBucketStart_.push_back(i);  // (the first item: its draw describes the bucket)
            }
            ownedBucketOf_[i] = table[at].bucket;
            if ((i & 7u) == 0 && items[i].cd != items[ownedBucketStart_[table[at].bucket]].cd) {
                // (1 of 8 verified exactly: a bucket key that merges different pipelines / inputs would draw with the wrong state)
                const CachedDraw& a = *items[ownedBucketStart_[table[at].bucket]].cd;
                const CachedDraw& b = *items[i].cd;
                if (a.scenePipeline2 != b.scenePipeline2 || !viEqual(a.vi, b.vi) || !dynEqual(a.dyn, b.dyn) || a.packet.indexBuffer != b.packet.indexBuffer ||
                    a.packet.indexOffset != b.packet.indexOffset || a.packet.index32 != b.packet.index32) {
                    ++ownedPersistStats.bucketKeyClash;
                    spdlog::error("d3d9: owned persistent: two different buckets share the key {:#x}", key);
                    return false;
                }
            }
        }
    }
    // the start of each bucket in the list: prefix sums of its counts
    ownedBucketCursor_.assign(buckets + 1, 0);
    for (std::uint32_t i = 0; i < n; ++i) ++ownedBucketCursor_[ownedBucketOf_[i] + 1];
    for (std::uint32_t b = 0; b < buckets; ++b) ownedBucketCursor_[b + 1] += ownedBucketCursor_[b];
    std::vector<std::uint32_t>& cursor = ownedBucketCursor_;
    static thread_local std::vector<std::uint32_t> fill;
    fill.assign(cursor.begin(), cursor.end() - 1);
    auto* list = static_cast<std::uint32_t*>(arena.list.ptr);
    std::uint32_t* args = nullptr;
    gpuscene::OwnedCompact::Range range;
    rhi::RingAlloc jobsBlock;
    if (gpu) {
        // (a refusal below = the CPU path for this flush: nothing was written yet)
        gpu = compact.reserve(*this, n, buckets, range);
        if (gpu) {
            jobsBlock = game_->allocRing(static_cast<std::uint64_t>(buckets) * 16, 16);
            if (jobsBlock.ptr == nullptr) {
                gpu = false;
                ++sceneStats_.fbRingFull;
            }
        }
    }
    static thread_local std::vector<std::uint32_t> posItem;  // (the item at each list position: the check frames' expectation)
    if (!gpu) {
        if (arena.args.ptr == nullptr) arena.args = game_->allocRing(static_cast<std::uint64_t>(n) * rhi::kIndirectStride, 16);
        if (arena.args.ptr == nullptr) {
            ++sceneStats_.fbRingFull;
            return false;
        }
        args = static_cast<std::uint32_t*>(arena.args.ptr);
    } else {
        posItem.resize(n);
    }
    const std::uint64_t frame = frameValue();
    for (std::uint32_t i = 0; i < n; ++i) {
        const OwnedItem& it = items[i];
        const CachedDraw& c = *it.cd;
        const std::uint32_t j = fill[ownedBucketOf_[i]]++;
        list[2 * j] = it.slot;
        list[2 * j + 1] = viewSlot;
        if (!gpu) {
            args[5 * j + 0] = c.packet.count;
            args[5 * j + 1] = 1;
            args[5 * j + 2] = c.packet.first;
            args[5 * j + 3] = static_cast<std::uint32_t>(c.packet.vertexOffset);
            args[5 * j + 4] = j;
        } else {
            posItem[j] = i;
        }
        // the bookkeeping of a drawn draw (replayOwned's)
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
    }
    if (gpu) {
        // the jobs (first entry, entries, first argument index, count index per bucket) + the check frames' expectation (what the CPU path would have written)
        auto* jobs = static_cast<std::uint32_t*>(jobsBlock.ptr);
        gpuscene::OwnedCompact::Job job;
        job.list = arena.list.ref;
        job.jobs = jobsBlock.ref;
        job.items = n;
        job.buckets = buckets;
        job.range = range;
        const bool check = compact.checkThisFrame(*this);
        std::vector<std::uint32_t> recs;
        if (check) {
            job.expect.resize(2 * static_cast<std::size_t>(buckets));
            recs.reserve(static_cast<std::size_t>(n) * 5);
        }
        for (std::uint32_t b = 0; b < buckets; ++b) {
            jobs[4 * b + 0] = cursor[b];
            jobs[4 * b + 1] = cursor[b + 1] - cursor[b];
            jobs[4 * b + 2] = range.firstArg + cursor[b];
            jobs[4 * b + 3] = range.firstCount + b;
            if (!check) continue;
            std::uint32_t want = 0;
            for (std::uint32_t k = cursor[b]; k < cursor[b + 1]; ++k) {
                const OwnedItem& it = items[posItem[k]];
                if (it.cd->packet.count == 0) continue;
                ++want;
                recs.insert(recs.end(), {it.cd->packet.count, 1u, it.cd->packet.first, static_cast<std::uint32_t>(it.cd->packet.vertexOffset), k});
            }
            job.expect[2 * b] = cursor[b];
            job.expect[2 * b + 1] = want;
        }
        if (check) job.expect.insert(job.expect.end(), recs.begin(), recs.end());
        compact.queue(*this, std::move(job));
    }
    for (std::uint32_t b = 0; b < buckets; ++b) {
        const CachedDraw& c = *items[ownedBucketStart_[b]].cd;
        const std::uint32_t count = cursor[b + 1] - cursor[b];
        DrawPacket m = c.packet;
        m.vi = internSceneViAt(c.vi, arena.list.buffer, arena.list.offset);
        m.dyn = internDyn(c.dyn);
        m.pipeline = c.scenePipeline2;
        m.op = DrawPacket::Op::DrawIndexedIndirect;
        m.scene = 0;
        m.count = count;
        m.instances = 1;
        m.first = 0;
        m.vertexOffset = 0;
        if (gpu) {
            compact.fillPacket(*this, range, cursor[b], b, count, m);
        } else {
            m.indirectBuffer = arena.args.buffer;
            m.indirectOffset = arena.args.offset + static_cast<std::uint64_t>(cursor[b]) * rhi::kIndirectStride;
        }
        shader::abi::ScenePush sp{};
        sp.records = recordsRef.bits;
        sp.frame = frameBlock.bits;
        sp.views = sceneViews_.base.bits;
        static_assert(sizeof(sp) == sizeof(m.push));
        std::memcpy(&m.push, &sp, sizeof(sp));
        submitInterned(m);
        ++sceneStats_.indirectCalls;
        sceneStats_.records += count;
        sceneStats_.maxRun = std::max<std::uint64_t>(sceneStats_.maxRun, count);
        ++ownedPersistStats.packets;
    }
    ownedPersistStats.buckets += buckets;
    ownedPersistStats.items += n;
    return true;
}

}  // namespace rvk::d3d9
