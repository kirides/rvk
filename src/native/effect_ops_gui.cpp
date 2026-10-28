// N5.7, group 65536 of native.scene_ops_groups: the sprite batcher (docs/research/engine-frame/README.md "15. N5.7 result").
// eCSprite::Flush (0x757d10) of any sprite (the GUI device's: HUD, menus, inventory, dialogue text, the cursor; loading screens;
// the video player's): its queued records (+0xa8, 0x48 each, count +0xb0) become batches by RenderBatches' rules (a blend change,
// a texture change, a full buffer; fxc::planSprites), each DrawBatch's state by DrawBatch (SetBlend's alpha blending + master
// blend factors, the texture attached + eCGfxEffect::Execute = the sprite effect's pass with g_mWorldViewProj = the sprite's
// projection +0x48 x the device's view matrix, as Flush computes it with SharedBase's bCMatrix::operator*), eCSpriteMesh::Draw's
// ranges and eCGfxVisual::Execute's states; the vertices (fxc::spriteQuad) and indices generated natively. Compared with the
// engine's draws (every field class + the vertex / index bytes); take-over issues them at Flush's entry from our own vertex /
// index buffers and drops the engine's. The engine's code still runs (its records, locks, effect state). Render thread.

#include "native/effect_ops.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "d3d9/defs.h"
#include "engine/memread.h"
#include "engine/symbols.h"
#include "hal/vulkan/hal_vulkan.h"

namespace rvk::native {

namespace {

using U8 = const std::uint8_t;
using engine::readAt;
using engine::safeRead;

template <class T>
inline T get(const void* p, std::size_t off, T fallback = T{}) {
    T v = fallback;
    if (!readAt(p, off, v)) return fallback;
    return v;
}
inline U8* ptr(const void* p, std::size_t off) { return get<U8*>(p, off, nullptr); }

std::uint32_t callTarget(U8* engine, std::uint32_t rva) {
    std::uint8_t op = 0;
    std::int32_t rel = 0;
    if (!readAt(engine, rva, op) || op != 0xe8 || !readAt(engine, rva + 1, rel)) return 0;
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(rva) + 5 + rel);
}

constexpr std::uint32_t kMaxQuads = 0x1000;  // eCSprite::Begin's mesh capacity (+0xe0)

}  // namespace

bool EffectOps::verifyGui() {
    const auto& s = engine::sym();
    if (s.spriteFlush != engine_ + 0x757d10 || s.matrixMul == nullptr || s.gfxEffectOptimize == nullptr || s.gfxEffectReset == nullptr ||
        s.gfxEffectActive == nullptr) {
        spdlog::warn("native: gui: eCSprite::Flush / symbols not as expected: the sprite batcher stays the engine's");
        return false;
    }
    // the code the composition follows: Flush -> RenderBatches -> DrawBatch (SetBlend, the image attach, eCGfxEffect::Execute)
    struct Site {
        std::uint32_t at, target;
    };
    static constexpr Site kSites[] = {{0x757e90, 0x757180}, {0x75720e, 0x7570b0}, {0x757234, 0x7570b0}, {0x75744e, 0x7570b0},
                                      {0x7570cc, 0x7568f0}, {0x75710c, 0x74fe80}, {0x757122, 0x74fef0}, {0x75713f, 0x750210}};
    for (const Site& site : kSites) {
        if (callTarget(engine_, site.at) != site.target) {
            spdlog::warn("native: gui: the call at {:#x} is not to {:#x}: off", site.at, site.target);
            return false;
        }
    }
    if (matMul_ == nullptr) matMul_ = reinterpret_cast<MatMulFn>(s.matrixMul);
    spdlog::info("native: gui: engine checks passed (eCSprite::Flush / RenderBatches / DrawBatch call sites), eCGfxEffect::ms_bOptimize {}",
                 *static_cast<const bool*>(s.gfxEffectOptimize));
    return true;
}

bool EffectOps::spriteFlushBegin(void* spritePtr) {
    if (verified_ <= 0 || guiVerified_ <= 0 || !guiOn()) return false;
    auto* sprite = static_cast<U8*>(spritePtr);
    if (get<std::int32_t>(sprite, 0xb0) == 0) return false;  // Flush does nothing
    if (unit_ != Unit::None) finishUnit();
    unit_ = Unit::Gui;
    unitFilter_ = sprite;
    unitTake_ = guiTake_;
    unitCompare_ = guiCompare_;
    const auto view = dev_.phases().view();
    unitName_ = view == d3d9::scene::View::Gui ? "gui sprites" : std::format("gui sprites (view {})", d3d9::scene::name(view));
    stats_ = &window_[unitName_];
    ++stats_->counts.units;
    ++spriteFlushes_;
    const Skip s = composeSprite(sprite);
    if (s != Skip::None) {
        ++stats_->skips[static_cast<std::size_t>(s)];
        drawCount_ = 0;
        unit_ = Unit::None;
        leftUnit(sprite, s);
        return false;
    }
    ++stats_->counts.unitsComposed;
    engineDraws_ = 0;
    taken_ = false;
    leanStats_ = &leanWindow_[unitName_];
    if (unitTake_) {
        predictAfter(true);
        if (leanUnit()) {
            // O3 lean: the engine's Flush is not run (no record holds a reference; the image the effect attaches is released by
            // End / EndFrame only when attached: README.md 15, opt-plan.md "O2c / O3 result")
            issueSprite(true);
            adoptAfter();
            taken_ = true;
            leanSkipped_ = true;
            return true;
        }
        issueSprite(false);
        taken_ = true;
        afterPending_ = checkFrame_ && unitCompare_;
    }
    return false;
}

void EffectOps::spriteFlushEnd(void* sprite) {
    if (unit_ == Unit::Gui && unitFilter_ == sprite) {
        if (afterPending_) checkAfter(false);
        finishUnit();
    }
    if (sprite == leftUnit_) leftUnit_ = nullptr;
}

EffectOps::Skip EffectOps::composeSprite(const std::uint8_t* sp) {
    drawCount_ = 0;
    spriteGeom_.clear();
    const std::int32_t n = get<std::int32_t>(sp, 0xb0);
    U8* recs = ptr(sp, 0xa8);
    if (n <= 0 || recs == nullptr) return Skip::Unreadable;
    // Flush sorts with qsort_s by depth (flags & 0xc0) / texture (0x20) first: never with Begin(0) (GUI, videos); left to the engine
    if ((get<std::uint32_t>(sp, 0) & 0xe0u) != 0) return Skip::Sorted;
    U8* gfx = ptr(sp, 0xe8);
    const auto* info = hal::vulkan::EffectInfo::fromApi(ptr(gfx, 0x18));
    if (gfx == nullptr || info == nullptr) return Skip::NoEffect;
    const fx::Effect& e = info->fxEffect();
    // Flush's SetTechnique(+0x20): the sprite effect's handle is null (eCSpriteEffect::Create never sets it), which the HAL
    // rejects (as D3DX): the technique stays the effect's current one (the first)
    const void* techHandle = ptr(gfx, 0x20);
    const int tech = techHandle != nullptr ? info->techniqueOf(techHandle) : info->currentTechnique();
    if (techHandle == nullptr) ++spriteNullTechnique_;
    const std::int32_t pass = get<std::int32_t>(gfx, 0x28, -1);
    if (tech < 0 || static_cast<std::size_t>(tech) >= e.techniques.size() || pass < 0 ||
        static_cast<std::size_t>(pass) >= e.techniques[static_cast<std::size_t>(tech)].passes.size()) {
        if (spriteLogged_++ < 4) spdlog::info("native: gui: sprite effect technique handle {} (index {}), pass {}: left to the engine", techHandle, tech, pass);
        return Skip::Technique;
    }
    const std::uint32_t wvpParam = info->parameterOf(ptr(sp, 0xf0));
    const std::uint32_t texParam = info->parameterOf(ptr(sp, 0xf8));
    if (wvpParam >= e.parameters.size() || texParam >= e.parameters.size()) return Skip::Unreadable;
    // the mesh (+0xb8): visual +0 (its record = our HalPrimitive), vertex data +0x10 (+0x20 the API buffer), count +0x20 (sprite
    // +0xd8), start +0x24 (+0xdc), capacity +0x28 (+0xe0)
    U8* visual = ptr(sp, 0xb8);
    const auto* prim = hal::vulkan::HalPrimitive::fromApi(ptr(visual, 0x10));
    if (visual == nullptr || prim == nullptr || ptr(visual, 0x60) != nullptr) return Skip::Visual;
    U8* vdata = ptr(sp, 0xc8);
    const std::uint32_t capacity = get<std::uint32_t>(sp, 0xe0);
    if (vdata == nullptr || ptr(vdata, 0x20) == nullptr || capacity == 0 || capacity > kMaxQuads) return Skip::Buffer;
    const auto sv = prim->streamView(0);
    if (sv.buffer == nullptr || sv.stride != fxc::kSpriteVertex) return Skip::Buffer;
    spriteExecuted_ = spriteBlendSet_ = false;
    std::vector<std::uint8_t> records(static_cast<std::size_t>(n) * fxc::kSpriteRecord);
    if (!safeRead(recs, records.data(), records.size())) return Skip::Unreadable;
    std::vector<fxc::SpriteKey> keys(static_cast<std::size_t>(n));
    for (std::size_t i = 0; i < keys.size(); ++i) {
        std::memcpy(&keys[i].image, records.data() + i * fxc::kSpriteRecord + 0x38, sizeof(void*));
        std::memcpy(&keys[i].blend, records.data() + i * fxc::kSpriteRecord + 0x44, 4);
    }
    const fxc::SpritePlan plan = fxc::planSprites(keys, capacity, get<std::uint32_t>(sp, 0xdc), get<std::uint32_t>(sp, 0xd8));
    // g_mWorldViewProj (Flush): the sprite's projection +0x48 x the device's view matrix (GetViewMatrix: the GUI's scale / offset)
    float proj[16], wvp[16];
    if (!readAt(sp, 0x48, proj)) return Skip::Unreadable;
    const auto view = hal_.viewMatrix();
    matMul_(proj, wvp, &view);
    fx::ParameterValues values = info->currentValues();
    std::memcpy(values.data[wvpParam].data(), wvp, std::min<std::size_t>(64, values.data[wvpParam].size()));
    std::vector<const void*> paramTextures(e.parameters.size(), nullptr);
    for (std::uint32_t i = 0; i < e.parameters.size(); ++i) paramTextures[i] = info->currentTexture(i);
    std::vector<const void*> shaders(e.shaders.size(), nullptr);
    for (std::uint32_t k = 0; k < e.shaders.size(); ++k) shaders[k] = info->fxShader(k);
    const fx::Pass& fxPass = e.techniques[static_cast<std::size_t>(tech)].passes[static_cast<std::size_t>(pass)];
    // eCGfxEffect::Execute's skip state, followed through the flush
    const auto& sy = engine::sym();
    const bool optimize = get<bool>(sy.gfxEffectOptimize, 0);
    const void* active = ptr(sy.gfxEffectActive, 0);
    std::uint8_t dirty = get<std::uint8_t>(gfx, 0x2c);
    bool reset = get<bool>(sy.gfxEffectReset, 0);
    if (!loggedOptimize_) {
        loggedOptimize_ = true;
        spdlog::info("native: gui: eCGfxEffect::ms_bOptimize {}, the sprite effect active {}, dirty {}, reset {}", optimize, active == gfx, dirty, reset);
    }
    const fxc::Masters masters = hal_.visualMasters();
    fxc::EffectDraw& state = *scratch_;
    captureDevice(state);
    entryRs_ = state.rs;
    spriteEndStart_ = plan.endStart;
    spriteEndCount_ = plan.endCount;
    const auto& dc = prim->drawDesc();
    (void)dc;
    for (const fxc::SpriteBatch& b : plan.batches) {
        if (b.why == fxc::SpriteBatch::Why::Blend) ++spriteBreaksBlend_;
        else if (b.why == fxc::SpriteBatch::Why::Texture) ++spriteBreaksTexture_;
        else if (b.why == fxc::SpriteBatch::Why::Full) ++spriteBreaksFull_;
        if (b.setBlend) {
            // SetBlend: SetAlphaBlending (slot 0x1e0) + eCGfxMixerAdmin::SetMasterBlending(src, dst) -> the HAL's master blend
            std::uint32_t enable = 0, src = 0, dst = 0;
            if (fxc::spriteBlend(b.blend, enable, src, dst)) {
                spriteBlendSet_ = true;
                state.rs[d3d9::RS_ALPHABLENDENABLE] = enable;
                state.rs[d3d9::RS_SRCBLEND] = src;
                state.rs[d3d9::RS_DESTBLEND] = dst;
                state.rsWritten.set(d3d9::RS_ALPHABLENDENABLE);
                state.rsWritten.set(d3d9::RS_SRCBLEND);
                state.rsWritten.set(d3d9::RS_DESTBLEND);
            }
        }
        if (b.setImage) {
            // DeAttach + AttachImmImageData(texture handle +0xf8, image) + eCGfxEffect::Execute (the pass, unless skipped)
            paramTextures[texParam] = b.image != nullptr ? textureOfImage(b.image) : nullptr;
            if (optimize && active == gfx && dirty == 0 && !reset) {
                ++spriteSkippedExecute_;
            } else {
                spriteExecuted_ = true;
                fxc::applyPass(e, fxPass, values, paramTextures, shaders, state);
                active = gfx;
                dirty = 0;
                reset = false;
            }
        }
        if (b.count == 0) continue;
        // eCSpriteMesh::Draw: the record (indexed list, base 0, min vertex 4 start, 4 count vertices, start index 6 start, 2 count
        // primitives) + eCGfxVisual::Execute (its states through the HAL's masters, then the primitive)
        fxc::applyVisual(state, get<std::uint32_t>(visual, 0x18), get<std::uint32_t>(visual, 0x1c), get<std::uint32_t>(visual, 0x20),
                         get<std::uint32_t>(visual, 0x24), masters);
        if (state.unsupported) return Skip::Technique;
        if (draws_.size() <= drawCount_) draws_.push_back(std::make_unique<fxc::EffectDraw>());
        fxc::EffectDraw& d = *draws_[drawCount_];
        d = state;
        d.decl = prim->vertexDecl();
        for (std::uint32_t i = 0; i < 4; ++i) {
            const auto v = prim->streamView(i);
            if (v.buffer == nullptr || !v.enabled) d.streams[i] = {nullptr, 0, 0, d.streams[i].freq};
            else d.streams[i] = {v.buffer, v.offset, v.stride, v.freq};
        }
        d.indices = prim->indexBuffer();
        d.indexed = true;
        d.primType = 4;
        d.baseVertex = 0;
        d.minIndex = b.start * 4;
        d.numVertices = b.count * 4;
        d.startIndex = b.start * 6;
        d.primCount = b.count * 2;
        d.startVertex = 0;
        SpriteGeom g;
        g.vbOffset = sv.offset + b.start * fxc::kSpriteQuad;
        g.vertices.resize(static_cast<std::size_t>(b.count) * fxc::kSpriteQuad);
        g.indices.resize(static_cast<std::size_t>(b.count) * 12);
        for (std::uint32_t q = 0; q < b.count; ++q) {
            const std::uint32_t r = b.firstRecord + q;
            if (r >= keys.size() || plan.quadOffset[r] != (b.start + q) * fxc::kSpriteQuad) return Skip::Unreadable;  // (plan consistency)
            fxc::spriteQuad(records.data() + static_cast<std::size_t>(r) * fxc::kSpriteRecord, 1.0f, g.vertices.data() + q * fxc::kSpriteQuad);
            std::uint16_t idx[6];
            fxc::spriteQuadIndices(b.start + q, idx);
            std::memcpy(g.indices.data() + q * 12, idx, 12);
        }
        spriteGeom_.push_back(std::move(g));
        ++drawCount_;
    }
    spriteRecords_ += static_cast<std::uint64_t>(n);
    spriteQuads_ += static_cast<std::uint64_t>(n);
    // O3: what Flush leaves in the sprite effect (predictAfter): g_mWorldViewProj, the last attached image's texture, the technique
    // (SetTechnique(null) is rejected: unchanged)
    unitInfo_ = hal::vulkan::EffectInfo::fromApiMutable(ptr(gfx, 0x18));
    unitValues_ = std::move(values);
    unitTextures_ = std::move(paramTextures);
    unitTech_ = info->currentTechnique();
    unitPasses_ = 0;
    return Skip::None;
}

bool EffectOps::spriteGeometryEqual(std::size_t k, const d3d9::DrawTapInfo& /*t*/, bool /*own*/, std::string& why) {
    if (k >= spriteGeom_.size()) return true;
    const SpriteGeom& g = spriteGeom_[k];
    const fxc::EffectDraw& c = *scratch_;  // the captured draw (compareDraw's)
    auto* vb = const_cast<d3d9::Buffer*>(static_cast<const d3d9::Buffer*>(c.streams[0].buffer));
    auto* ib = const_cast<d3d9::Buffer*>(static_cast<const d3d9::Buffer*>(c.indices));
    std::vector<std::uint8_t> got(g.vertices.size());
    if (vb == nullptr || !vb->readCpu(c.streams[0].offset + c.minIndex * fxc::kSpriteVertex, static_cast<std::uint32_t>(got.size()), got.data())) {
        why = "vertex data unreadable";
        return false;
    }
    std::size_t at = 0;
    if (!fxc::spriteBytesEqual(g.vertices, got, at, spriteWLaneDiffers_)) {
        why = std::format("vertex data differs at quad {} vertex {} byte {:#x}", at / fxc::kSpriteQuad, at % fxc::kSpriteQuad / fxc::kSpriteVertex,
                          at % fxc::kSpriteVertex);
        return false;
    }
    got.resize(g.indices.size());
    if (ib == nullptr || !ib->readCpu(c.startIndex * 2, static_cast<std::uint32_t>(got.size()), got.data())) {
        why = "index data unreadable";
        return false;
    }
    if (got != g.indices) {
        why = "index data differs";
        return false;
    }
    return true;
}

void EffectOps::issueSprite(bool lean) {
    if (drawCount_ == 0) return;
    if (!spriteVb_) spriteVb_ = dev_.createBuffer(kMaxQuads * fxc::kSpriteQuad, d3d9::USAGE_DYNAMIC | d3d9::USAGE_WRITEONLY, d3d9::POOL_DEFAULT, false, false);
    if (!spriteIb_) {
        // the static index buffer, generated (eCSpriteMesh::CreateIndexBuffer's contents)
        spriteIb_ = dev_.createBuffer(kMaxQuads * 12, d3d9::USAGE_WRITEONLY, d3d9::POOL_DEFAULT, true, false);
        if (void* p = spriteIb_->lock(0, kMaxQuads * 12, 0)) {
            auto* idx = static_cast<std::uint16_t*>(p);
            for (std::uint32_t q = 0; q < kMaxQuads; ++q) fxc::spriteQuadIndices(q, idx + 6 * q);
            spriteIb_->unlock();
        }
    }
    if (!lean) captureDevice(*saved_);
    d3d9::draws::Tag& tag = dev_.drawTag();
    const d3d9::draws::Tag savedTag = tag;
    issuing_ = true;
    for (std::size_t k = 0; k < drawCount_; ++k) {
        fxc::EffectDraw& d = *draws_[k];
        const SpriteGeom& g = spriteGeom_[k];
        if (g.vbOffset + g.vertices.size() > spriteVb_->size()) continue;
        // DISCARD of just the range the draw reads (the fresh GPU copy takes only those bytes)
        if (void* p = spriteVb_->lock(g.vbOffset, static_cast<std::uint32_t>(g.vertices.size()), d3d9::LOCK_DISCARD)) {
            std::memcpy(p, g.vertices.data(), g.vertices.size());
            spriteVb_->unlock();
        }
        // our buffers for this draw (the composition keeps the engine's for the engine draws' comparison)
        const fxc::Stream engineStream = d.streams[0];
        const void* engineIndices = d.indices;
        d.streams[0].buffer = spriteVb_.get();
        d.indices = spriteIb_.get();
        applyDevice(d);
        issueIndex_ = k;
        dev_.drawIndexedPrimitive(d.primType, d.baseVertex, d.minIndex, d.numVertices, d.startIndex, d.primCount);
        ++stats_->issuedDraws;
        d.streams[0] = engineStream;
        d.indices = engineIndices;
    }
    issuing_ = false;
    tag = savedTag;
    if (!lean) {
        applyDevice(*saved_);
        return;
    }
    // O3 lean: the device as the engine's Flush leaves it: the last batch's state with the engine's mesh buffers, the render states
    // of the entry (EndTechnique's block)
    applyDevice(*draws_[drawCount_ - 1]);
    for (std::uint32_t rs = 0; rs < fxc::kRenderStates; ++rs) {
        if (dev_.renderState(rs) != entryRs_[rs]) dev_.setRenderState(rs, entryRs_[rs]);
    }
}

void EffectOps::reportGui() {
    if (!guiOn() || guiVerified_ <= 0) return;
    spdlog::info("native: gui (5 s): {} flushes, {} records / quads, batch breaks: blend {} texture {} buffer full {}; executes skipped {}; "
                 "w lanes differing (not compared) {}; Gui-view engine draws outside the flushes {}; flushes with a null technique handle {}",
                 spriteFlushes_, spriteRecords_, spriteBreaksBlend_, spriteBreaksTexture_, spriteBreaksFull_, spriteSkippedExecute_, spriteWLaneDiffers_,
                 guiOutside_, spriteNullTechnique_);
    spriteNullTechnique_ = 0;
    spriteFlushes_ = spriteRecords_ = spriteQuads_ = spriteBreaksBlend_ = spriteBreaksTexture_ = spriteBreaksFull_ = spriteSkippedExecute_ = 0;
    spriteWLaneDiffers_ = guiOutside_ = 0;
}

}  // namespace rvk::native
