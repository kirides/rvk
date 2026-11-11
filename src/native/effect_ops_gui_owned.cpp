// F13 owned records (docs/research/frame-ownership.md 14.34; native.own_gui = owned): the sprite batcher's flushes (eCSprite::Flush: HUD, menus, the
// inventory / map / log / character pages, text, cursor, loading screens, videos) drawn from pulled records on the O3 lean frames.
//
// What a flush is: its queued records (eCSprite +0xa8, 0x48 each) in queue order, RenderBatches' batches (blend / texture / buffer-full breaks; fxc::planSprites),
// per batch DrawBatch's transitions (SetBlend's three blend states; the image attached + eCGfxEffect::Execute = the sprite effect's pass; eCGfxVisual::Execute's
// states) and one draw of the batch's quads. The composition (effect_ops_gui.cpp) re-does all of that per batch from the device state at the entry.
//
// Owned: the records go as they are into an instance stream (fxc::guiRecord: + the batch image's bindless slot), shaders/post/fx/gui_main_pull.* build each
// corner from its record (no CPU quads) and sample the record's texture; consecutive batches with equal blend states are ONE instanced draw (fxc::guiRuns), the
// draw order of the records is the queue order, so the blending is the engine's. The device state the pass + the visual write is a template (fxc::WriteSet,
// derived from the composition's own functions once per key: the effect, technique, pass, every parameter value, the masters, the visual's states); the blend
// states per batch follow SetBlend / the pass (fxc::spriteTrios); the clip rect is the scissor at Flush's entry (the composition never sets one). After the draws
// the device / the API effect / the engine fields are left as the engine's Flush leaves them (the O3 lean path's prediction: effect_ops_lean.cpp).
// Check frames (native.lean_check, the engine's Flush runs): the composed batches are compared with the template's model of them (fxc::compare, DIFFERING) and the
// owned form is drawn into a scratch copy of the target and compared with the composed flush (image 'gui_main.frag (owned records)'). Render thread.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "d3d9/defs.h"
#include "engine/memread.h"
#include "engine/symbols.h"
#include "hal/vulkan/hal_vulkan.h"
#include "native/effect_ops.h"

namespace rvk::native {

namespace {

using U8 = const std::uint8_t;
using Clock = std::chrono::steady_clock;

template <class T>
inline T get(const void* p, std::size_t off, T fallback = T{}) {
    T v = fallback;
    if (!engine::readAt(p, off, v)) return fallback;
    return v;
}
inline U8* ptr(const void* p, std::size_t off) { return get<U8*>(p, off, nullptr); }

constexpr std::uint32_t kGuiRingBytes = 4u << 20;  // the records of a frame's flushes (one flush: <= 4096 x 0x40 = 256 KiB)
constexpr std::uint32_t kTrio[3] = {d3d9::RS_ALPHABLENDENABLE, d3d9::RS_SRCBLEND, d3d9::RS_DESTBLEND};

std::uint64_t mix(std::uint64_t h, const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}
template <class T>
std::uint64_t mixv(std::uint64_t h, const T& v) {
    return mix(h, &v, sizeof(v));
}

}  // namespace

std::uint64_t EffectOps::guiKey(const SpriteInputs& in, const fx::ParameterValues& values) const {
    std::uint64_t h = 0xcbf29ce484222325ull;
    h = mixv(h, in.info);
    h = mixv(h, in.tech);
    h = mixv(h, in.pass);
    h = mixv(h, in.wvpParam);
    h = mixv(h, in.texParam);
    h = mix(h, in.visualStates, sizeof(in.visualStates));
    const std::uint8_t m[4] = {in.masters.fill, in.masters.cull, in.masters.depthFunc, in.masters.depthWrite};
    h = mix(h, m, 4);
    for (std::size_t i = 0; i < values.data.size(); ++i) {
        h = mixv(h, values.data[i].size());
        if (!values.data[i].empty()) h = mix(h, values.data[i].data(), values.data[i].size());
    }
    for (std::size_t i = 0; i < in.paramTextures.size(); ++i) {
        if (i != in.texParam) h = mixv(h, in.paramTextures[i]);
    }
    for (const void* s : in.shaders) h = mixv(h, s);
    return h;
}

const fxc::WriteSet* EffectOps::guiTemplate(const SpriteInputs& in, const fx::ParameterValues& values, const char*& why, bool derive) {
    const std::uint64_t key = guiKey(in, values);
    if (auto it = guiTemplates_.find(key); it != guiTemplates_.end()) {
        it->second.lastUse = dev_.stats().frames;
        if (!it->second.w.ok) {
            why = "the sprite effect's transitions are not representable";
            return nullptr;
        }
        return &it->second.w;
    }
    if (!derive) {
        why = "no template";
        return nullptr;
    }
    if (guiTemplates_.size() >= 64) guiTemplates_.clear();  // (keys of old resolutions / effects: bounded)
    const fx::Effect& e = in.info->fxEffect();
    const fx::Pass& pass = e.techniques[static_cast<std::size_t>(in.tech)].passes[static_cast<std::size_t>(in.pass)];
    static const char sentinel = 0;
    std::vector<const void*> textures = in.paramTextures;
    textures[in.texParam] = &sentinel;
    GuiTemplate& t = guiTemplates_[key];
    t.lastUse = dev_.stats().frames;
    t.w = fxc::deriveWrites(
        [&](fxc::EffectDraw& d) {
            fxc::applyPass(e, pass, values, textures, in.shaders, d);
            fxc::applyVisual(d, in.visualStates[0], in.visualStates[1], in.visualStates[2], in.visualStates[3], in.masters);
        },
        &sentinel);
    if (t.w.ok && (t.w.texStage != 0 || !t.w.vsSet || !t.w.psSet || t.w.vs == nullptr || t.w.ps == nullptr)) {
        t.w.ok = false;
        t.w.why = std::format("the batch image binds to stage {} / the pass sets no shaders", t.w.texStage);
    }
    ++guiOwnedW_.derived;
    spdlog::info("native: own gui: template {:016x} derived: {}{} ({} render states, {} sampler states, {} + {} float registers, blend states written {:#x})", key,
                 t.w.ok ? "ok" : "not representable: ", t.w.ok ? "" : t.w.why, t.w.rs.size(), t.w.ss.size(), t.w.vsF.size(), t.w.psF.size(), t.w.trioMask);
    if (!t.w.ok) {
        why = "the sprite effect's transitions are not representable";
        return nullptr;
    }
    return &t.w;
}

d3d9::VertexDecl* EffectOps::guiPullDecl() {
    if (!guiDecl_) {
        using namespace d3d9;
        const auto tc = static_cast<std::uint8_t>(shader::Usage::TexCoord);
        std::vector<VertexElement> e;
        e.push_back({1, 0x00, DECLTYPE_FLOAT4, 0, tc, 8});
        e.push_back({1, 0x10, DECLTYPE_FLOAT4, 0, tc, 9});
        e.push_back({1, 0x20, DECLTYPE_FLOAT4, 0, tc, 10});
        e.push_back({1, 0x30, DECLTYPE_FLOAT2, 0, tc, 11});
        e.push_back({1, 0x38, DECLTYPE_D3DCOLOR, 0, tc, 12});
        e.push_back({kDeclEndStream, 0, DECLTYPE_UNUSED, 0, 0, 0});
        guiDecl_ = dev_.createVertexDecl(e);
    }
    return guiDecl_.get();
}

bool EffectOps::guiOwnedPrepare(const SpriteInputs& in, const fxc::WriteSet& w, GuiOwned& f, const char*& why) {
    f.w = &w;
    if (!own_->guiPulled(w.vs, w.ps, f.vs, f.ps, &why)) return false;
    if (in.optimize) {
        why = "eCGfxEffect::ms_bOptimize (a pass may be skipped)";
        return false;
    }
    // the first drawn batch must come after the first image (before it the pass has not run: the entry's shaders would draw)
    bool seenImage = false;
    for (const fxc::SpriteBatch& b : in.plan.batches) {
        seenImage |= b.setImage;
        if (b.count != 0 && !seenImage) {
            why = "a batch drawn before the first image";
            return false;
        }
    }
    if (guiPullDecl() == nullptr) {
        why = "declaration";
        return false;
    }
    if (!guiIb_) {
        // one quad in the engine's index order (eCSpriteMesh::CreateIndexBuffer: 0 2 3 0 1 2); vertex ids 0..3 = the corners
        guiIb_ = dev_.createBuffer(64, d3d9::USAGE_WRITEONLY, d3d9::POOL_MANAGED, true, false);
        guiDummy_ = dev_.createBuffer(64, d3d9::USAGE_WRITEONLY, d3d9::POOL_MANAGED, false, false);
        if (guiIb_) {
            if (void* p = guiIb_->lock(0, 12, 0)) {
                fxc::spriteQuadIndices(0, static_cast<std::uint16_t*>(p));
                guiIb_->unlock();
            }
        }
        if (guiDummy_) {
            if (void* p = guiDummy_->lock(0, 64, 0)) {
                std::memset(p, 0, 64);
                guiDummy_->unlock();
            }
        }
    }
    if (!guiIb_ || !guiDummy_) {
        why = "own buffers";
        return false;
    }
    const std::array<std::uint32_t, 3> entry = {dev_.renderState(kTrio[0]), dev_.renderState(kTrio[1]), dev_.renderState(kTrio[2])};
    fxc::spriteTrios(w, in.plan.batches, entry, f.trios);
    f.runs = fxc::guiRuns(in.plan.batches, f.trios);
    f.anyBlend = false;
    f.firstTexture = f.lastTexture = nullptr;
    f.records.resize(static_cast<std::size_t>(in.n) * fxc::kGuiRecordStride);
    bool first = true;
    const void* curImage = nullptr;
    d3d9::Texture* curTex = nullptr;
    std::uint32_t curSlot = 0;
    bool slotKnown = false;
    for (const fxc::SpriteBatch& b : in.plan.batches) {
        if (b.setBlend) {
            std::uint32_t e = 0, s = 0, d = 0;
            f.anyBlend |= fxc::spriteBlend(b.blend, e, s, d);
        }
        if (b.setImage) {
            // (the composition's texture of the batch: AttachImmImageData(image) -> its API texture)
            curImage = b.image;
            curTex = b.image != nullptr ? textureOfImage(b.image) : nullptr;
            slotKnown = false;
            f.lastTexture = curTex;
        }
        if (b.count == 0) continue;
        if (!slotKnown) {
            curSlot = dev_.sampleSlot2D(curTex);  // (made ready for sampling now: before any draw of the flush)
            slotKnown = true;
        }
        if (first) {
            f.firstTexture = curTex;
            first = false;
        }
        for (std::uint32_t q = 0; q < b.count; ++q) {
            const std::uint32_t r = b.firstRecord + q;
            if (r >= static_cast<std::uint32_t>(in.n) || in.keys[r].image != curImage) {
                why = "a record outside its batch's image";
                return false;
            }
            fxc::guiRecord(in.records.data() + static_cast<std::size_t>(r) * fxc::kSpriteRecord, curSlot, f.records.data() + static_cast<std::size_t>(r) * fxc::kGuiRecordStride);
        }
    }
    std::memcpy(f.scissor, dev_.scissorRect(), sizeof(f.scissor));
    return true;
}

void EffectOps::guiOwnedIssue(const GuiOwned& f, d3d9::Surface* rtOverride) {
    using namespace d3d9;
    const fxc::WriteSet& w = *f.w;
    const auto size = static_cast<std::uint32_t>(f.records.size());
    if (size == 0 || f.runs.empty()) return;
    if (!guiRecVb_) guiRecVb_ = dev_.createBuffer(kGuiRingBytes, USAGE_DYNAMIC | USAGE_WRITEONLY, POOL_DEFAULT, false, false);
    if (!guiRecVb_ || size > guiRecVb_->size()) return;
    // the records: appended within the frame (NOOVERWRITE), DISCARD at the frame's first flush or when full
    const std::uint64_t frame = dev_.stats().frames;
    std::uint32_t flags = LOCK_NOOVERWRITE;
    if (frame != guiRecFrame_ || guiRecCursor_ + size > guiRecVb_->size()) {
        flags = LOCK_DISCARD;
        guiRecCursor_ = 0;
        guiRecFrame_ = frame;
    }
    const std::uint32_t base = guiRecCursor_;
    if (void* p = guiRecVb_->lock(base, size, flags)) {
        std::memcpy(p, f.records.data(), size);
        guiRecVb_->unlock();
    } else {
        return;
    }
    guiRecCursor_ += (size + 255u) & ~255u;
    // the template's writes (the pass + the visual), on top of the device's entry state
    for (const auto& [i, v] : w.rs) {
        if (dev_.renderState(i) != v) dev_.setRenderState(i, v);
    }
    for (const auto& s : w.ss) {
        if (dev_.samplerStateRow(s.stage)[s.state] != s.value) dev_.setSamplerState(s.stage, s.state, s.value);
    }
    for (const auto& [st, t] : w.textures) {
        if (dev_.stageTexture(st) != t) dev_.setStageTexture(st, const_cast<Texture*>(static_cast<const Texture*>(t)));
    }
    if (w.texStage >= 0) {
        auto* t = const_cast<Texture*>(static_cast<const Texture*>(f.firstTexture));
        if (dev_.stageTexture(static_cast<std::uint32_t>(w.texStage)) != t) dev_.setStageTexture(static_cast<std::uint32_t>(w.texStage), t);
    }
    const auto setRegs = [](const float (*cur)[4], const std::vector<fxc::WriteSet::Reg>& regs, auto&& set) {
        for (const fxc::WriteSet::Reg& g : regs) {
            float v[4];
            std::memcpy(v, cur[g.reg], 16);
            for (std::uint32_t c = 0; c < 4; ++c) {
                if ((g.mask >> c & 1u) != 0) v[c] = g.v[c];
            }
            if (std::memcmp(cur[g.reg], v, 16) != 0) set(g.reg, v);
        }
    };
    setRegs(dev_.vsConstants(), w.vsF, [&](std::uint32_t r, const float* v) { dev_.setVertexShaderConstantF(r, v, 1); });
    setRegs(dev_.psConstants(), w.psF, [&](std::uint32_t r, const float* v) { dev_.setPixelShaderConstantF(r, v, 1); });
    for (const auto& [r, v] : w.vsB) {
        const auto b = static_cast<std::int32_t>(v);
        dev_.setVertexShaderConstantB(r, &b, 1);
    }
    for (const auto& [r, v] : w.psB) {
        const auto b = static_cast<std::int32_t>(v);
        dev_.setPixelShaderConstantB(r, &b, 1);
    }
    for (const auto& [r, v] : w.vsI) dev_.setVertexShaderConstantI(r, v.data(), 1);
    for (const auto& [r, v] : w.psI) dev_.setPixelShaderConstantI(r, v.data(), 1);
    // ours: the pulled shaders, the record stream, one quad's indices
    dev_.setVertexShaderRef(ShaderRef(const_cast<Shader*>(f.vs)));
    dev_.setPixelShaderRef(ShaderRef(const_cast<Shader*>(f.ps)));
    dev_.setVertexDecl(guiPullDecl());
    dev_.setStreamSource(0, guiDummy_.get(), 0, 16);
    dev_.setIndices(guiIb_.get());
    if (rtOverride != nullptr) {
        // (into a scratch copy of the target: SetRenderTarget resets the viewport and the scissor (D3D9): both put back)
        const Viewport vp = dev_.viewport();
        dev_.setRenderTarget(0, rtOverride);
        dev_.setViewport(vp);
        dev_.setScissorRect(f.scissor[0], f.scissor[1], f.scissor[2], f.scissor[3]);
    }
    d3d9::draws::Tag& tag = dev_.drawTag();
    const d3d9::draws::Tag savedTag = tag;
    const bool savedIssuing = issuing_;
    issuing_ = true;
    issueIndex_ = ~std::size_t{0};  // (no composition of its own: the own-draw compare skips it)
    for (const fxc::GuiRun& run : f.runs) {
        for (std::uint32_t c = 0; c < 3; ++c) {
            if (dev_.renderState(kTrio[c]) != run.trio[c]) dev_.setRenderState(kTrio[c], run.trio[c]);
        }
        dev_.setStreamSourceFreq(0, STREAMSOURCE_INDEXEDDATA | run.count);
        dev_.setStreamSource(1, guiRecVb_.get(), base + run.firstRecord * fxc::kGuiRecordStride, fxc::kGuiRecordStride);
        dev_.setStreamSourceFreq(1, STREAMSOURCE_INSTANCEDATA | 1u);
        dev_.drawIndexedPrimitive(4, 0, 0, 4, 0, 2);
        own_->countScene(OwnPost::Scene::Gui, true, nullptr);
    }
    issuing_ = savedIssuing;
    tag = savedTag;
}

bool EffectOps::guiOwnedFlush(const std::uint8_t* sp) {
    leanUpdateFrame();
    if (!leanFrameOn_ || !guiTake_) return false;
    const Clock::time_point t0 = Clock::now();
    const auto composed = [&](const char* why) {
        ++guiOwnedW_.composed;
        if (guiOwnedW_.why.empty() && why != nullptr) guiOwnedW_.why = why;
        return false;
    };
    if (!leanSkippable()) return composed("the engine's Flush cannot be skipped (a state block open / ms_bOptimize / an execute callback)");
    SpriteInputs& in = spriteIn_;
    if (const Skip s = spriteInputs(sp, in); s != Skip::None) return composed(name(s));
    const char* why = nullptr;
    const fxc::WriteSet* w = guiTemplate(in, valuesScratch_, why, true);
    if (w == nullptr) return composed(why);
    GuiOwned& f = guiOwned_;
    if (!guiOwnedPrepare(in, *w, f, why)) return composed(why);
    const auto* prim = static_cast<const hal::vulkan::HalPrimitive*>(in.prim);
    // the device's values the draws change and the engine's Flush would leave as at the entry (its technique's render-state block) or as its last pass
    std::uint32_t freq[4];
    for (std::uint32_t i = 0; i < 4; ++i) freq[i] = dev_.streamSource(i).freq;
    std::array<std::uint32_t, fxc::kRenderStates> rsEntry;
    for (std::uint32_t i = 0; i < fxc::kRenderStates; ++i) rsEntry[i] = dev_.renderState(i);
    guiOwnedIssue(f, nullptr);
    // the device as the engine's Flush leaves it: the last batch's state (the pass's shaders / stages / registers, the mesh's buffers), the entry's render states
    using namespace d3d9;
    for (std::uint32_t i = 0; i < fxc::kRenderStates; ++i) {
        if (dev_.renderState(i) != rsEntry[i]) dev_.setRenderState(i, rsEntry[i]);
    }
    dev_.setVertexShaderRef(ShaderRef(const_cast<Shader*>(static_cast<const Shader*>(w->vs))));
    dev_.setPixelShaderRef(ShaderRef(const_cast<Shader*>(static_cast<const Shader*>(w->ps))));
    dev_.setVertexDecl(const_cast<VertexDecl*>(static_cast<const VertexDecl*>(prim->vertexDecl())));
    for (std::uint32_t i = 0; i < 4; ++i) {
        const auto v = prim->streamView(i);
        if (v.buffer == nullptr || !v.enabled) {
            dev_.setStreamSource(i, nullptr, 0, 0);
            dev_.setStreamSourceFreq(i, freq[i]);
        } else {
            dev_.setStreamSource(i, const_cast<Buffer*>(static_cast<const Buffer*>(v.buffer)), v.offset, v.stride);
            dev_.setStreamSourceFreq(i, v.freq);
        }
    }
    dev_.setIndices(const_cast<Buffer*>(static_cast<const Buffer*>(prim->indexBuffer())));
    if (w->texStage >= 0) dev_.setStageTexture(static_cast<std::uint32_t>(w->texStage), const_cast<Texture*>(static_cast<const Texture*>(f.lastTexture)));
    // the API effect and the engine fields as the Flush leaves them (effect_ops_lean.cpp predictAfter / adoptAfter)
    unitInfo_ = hal::vulkan::EffectInfo::fromApiMutable(ptr(in.gfx, 0x18));
    unitValues_ = valuesScratch_;
    unitTextures_ = in.paramTextures;
    if (in.texParam < unitTextures_.size()) unitTextures_[in.texParam] = f.lastTexture;
    unitTech_ = in.info->currentTechnique();
    unitPasses_ = 0;
    afterFields_.clear();
    auto* unit = const_cast<std::uint8_t*>(sp);
    afterFields_.push_back({engine::sym().gfxEffectActive, reinterpret_cast<std::uint64_t>(in.gfx), 8});
    afterFields_.push_back({unit + 0xb0, 0, 4});
    afterFields_.push_back({unit + 0xd8, in.plan.endCount, 4});
    afterFields_.push_back({unit + 0xdc, in.plan.endStart, 4});
    afterMasters_ = hal_.masterBits() | (f.anyBlend ? 1u << 7 : 0u);
    adoptAfter();
    ++guiOwnedW_.flushes;
    guiOwnedW_.records += static_cast<std::uint64_t>(in.n);
    guiOwnedW_.runs += f.runs.size();
    for (const fxc::SpriteBatch& b : in.plan.batches) guiOwnedW_.batches += b.count != 0 ? 1 : 0;
    guiOwnedW_.ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
    own_->countOwned(OwnPost::Scene::Gui);
    return true;
}

void EffectOps::guiOwnedCheck() {
    if (!guiEntryValid_ || drawCount_ == 0) return;
    const SpriteInputs& in = spriteIn_;
    const char* why = nullptr;
    const fxc::WriteSet* w = guiTemplate(in, valuesScratch_, why, true);
    if (w == nullptr) {
        if (guiOwnedW_.why.empty() && why != nullptr) guiOwnedW_.why = why;
        return;
    }
    if (!guiSim_) guiSim_ = std::make_unique<fxc::EffectDraw>();
    const fxc::EffectDraw& entry = *guiEntry_;
    std::vector<std::array<std::uint32_t, 3>> trios;
    fxc::spriteTrios(*w, in.plan.batches, {entry.rs[kTrio[0]], entry.rs[kTrio[1]], entry.rs[kTrio[2]]}, trios);
    fxc::Live all;
    all.vs.set();
    all.ps.set();
    all.stages = 0xffff;
    ++guiOwnedW_.checked;
    const void* tex = entry.textures[static_cast<std::size_t>(std::max(w->texStage, 0))];
    std::size_t k = 0;
    for (std::size_t b = 0; b < in.plan.batches.size(); ++b) {
        const fxc::SpriteBatch& batch = in.plan.batches[b];
        if (batch.setImage) tex = batch.image != nullptr ? textureOfImage(batch.image) : nullptr;
        if (batch.count == 0) continue;
        if (k >= drawCount_) break;
        const fxc::EffectDraw& composed = *draws_[k++];
        fxc::EffectDraw& sim = *guiSim_;
        sim = entry;
        fxc::applyWrites(*w, tex, sim);
        for (std::uint32_t c = 0; c < 3; ++c) sim.rs[kTrio[c]] = trios[b][c];
        // (the vertex input is the engine mesh's in both: the owned draw's own one is ours by design)
        sim.decl = composed.decl;
        sim.streams = composed.streams;
        sim.indices = composed.indices;
        sim.indexed = composed.indexed;
        sim.primType = composed.primType;
        sim.baseVertex = composed.baseVertex;
        sim.minIndex = composed.minIndex;
        sim.numVertices = composed.numVertices;
        sim.startIndex = composed.startIndex;
        sim.primCount = composed.primCount;
        sim.startVertex = composed.startVertex;
        const fxc::Diff d = fxc::compare(composed, sim, all);
        ++guiOwnedW_.checkedBatches;
        if (d.fields.any()) {
            ++guiOwnedW_.differing;
            ++guiOwnedDifferingTotal_;
            if (guiOwnedW_.firstDiff.empty()) guiOwnedW_.firstDiff = d.first;
            if (guiOwnedLogged_++ < 12) spdlog::warn("native: own gui: owned records: batch {} of a flush DIFFERS from the template's model: {}", b, d.first);
        }
    }
}

bool EffectOps::guiOwnedCompareDraw(d3d9::Surface* scratch) {
    const char* why = nullptr;
    const fxc::WriteSet* w = guiTemplate(spriteIn_, valuesScratch_, why, true);
    if (w == nullptr || !guiOwnedPrepare(spriteIn_, *w, guiOwned_, why)) return false;
    guiOwnedIssue(guiOwned_, scratch);
    return true;
}

void EffectOps::reportGuiOwned() {
    GuiOwnedStats& s = guiOwnedW_;
    if (own_->sceneMode(OwnPost::Scene::Gui) != OwnPost::Mode::Off || s.flushes != 0 || s.checked != 0) {
        spdlog::info("native: own gui: owned records (5 s): {} flushes from records ({} records, {} batches in {} draws, {:.2f} us per flush; {} lean composed flushes {:.2f} us each), "
                     "{} composed{}; templates {} (derived in the window {}); check frames: {} flushes / {} batches compared with the template's model, DIFFERING {} (run {}){}",
                     s.flushes, s.records, s.batches, s.runs, s.flushes != 0 ? static_cast<double>(s.ns) / 1000.0 / static_cast<double>(s.flushes) : 0.0,
                     s.composedTimed, s.composedTimed != 0 ? static_cast<double>(s.composeNs) / 1000.0 / static_cast<double>(s.composedTimed) : 0.0, s.composed,
                     s.why.empty() ? std::string() : std::format(" (first reason: {})", s.why), guiTemplates_.size(), s.derived, s.checked, s.checkedBatches,
                     s.differing, guiOwnedDifferingTotal_, s.firstDiff.empty() ? std::string() : std::format(" (first: {})", s.firstDiff));
    }
    s = {};
}

}  // namespace rvk::native
