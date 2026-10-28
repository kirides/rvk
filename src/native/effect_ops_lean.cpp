// O3 lean effect units (docs/research/engine-frame/opt-plan.md "O2c / O3 result"; settings native.lean, native.lean_check): a
// composed, taken-over unit of EffectOps - an effect filter's eCImageFilterEffect::ExecuteEffect (mask chain, terrain blur, post
// chain), a sprite batcher eCSprite::Flush, the mask's PSSM pass loop - does not run the engine's code on a lean frame. What that
// code leaves behind besides its draws (opt-side-effects-frame.md 6, 7, 5.5; this file's predictAfter) is reproduced instead:
//   * the device: the HAL's effect technique is a render-state block (BeginTechnique .. EndTechnique: every render state the
//     passes and eCGfxVisual::Execute set through the HAL reverts), everything else (shaders, declaration, streams, indices, stage
//     textures, sampler states, float / int / bool registers) stays as the last pass set it: our last draw's state with the
//     render states of the unit's entry;
//   * the API effect (hal::vulkan::EffectInfo, the carry-over the next composition starts from): the values OnSetParams pushed,
//     the textures (the inputs' detached again by OnResetParams), the technique SetTechnique selected;
//   * engine fields: eCGfxEffect +0x28 (the pass), ms_pActiveEffect (+0x2c / ms_bReset only change while ms_bOptimize); the
//     sprite's record count +0xb0 = 0 and its mesh ring (+0xd8 count, +0xdc start); the HAL's master blend flag (SetBlend's
//     SetMasterBlending).
// Check frames (native.lean_check) run the engine's code and compare that prediction with what it left: "after" (the device, every
// register / stage / state), "store" (the effect's values / textures / technique), "fields" and the master flags. Render thread.

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "engine/memread.h"
#include "engine/symbols.h"
#include "hal/vulkan/hal_vulkan.h"
#include "native/effect_ops.h"
#include "native/static_compose.h"

namespace rvk::native {

namespace {

using U8 = const std::uint8_t;
using engine::readAt;

template <class T>
inline T get(const void* p, std::size_t off, T fallback = T{}) {
    T v = fallback;
    if (!readAt(p, off, v)) return fallback;
    return v;
}
inline U8* ptr(const void* p, std::size_t off) { return get<U8*>(p, off, nullptr); }

bool storeField(void* at, std::uint64_t v, std::uint32_t size) {
    __try {
        switch (size) {
            case 1: *static_cast<volatile std::uint8_t*>(at) = static_cast<std::uint8_t>(v); break;
            case 4: *static_cast<volatile std::uint32_t*>(at) = static_cast<std::uint32_t>(v); break;
            default: *static_cast<volatile std::uint64_t*>(at) = v; break;
        }
        return true;
    } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
        return false;
    }
}

std::uint64_t loadField(const void* at, std::uint32_t size) {
    std::uint64_t v = 0;
    if (size == 1) v = get<std::uint8_t>(at, 0);
    else if (size == 4) v = get<std::uint32_t>(at, 0);
    else v = get<std::uint64_t>(at, 0);
    return v;
}

}  // namespace

void EffectOps::LeanStats::add(const LeanStats& o) {
    skipped += o.skipped;
    fallbackBlock += o.fallbackBlock;
    afterChecked += o.afterChecked;
    afterDiffer += o.afterDiffer;
    storeChecked += o.storeChecked;
    storeDiffer += o.storeDiffer;
    fieldsChecked += o.fieldsChecked;
    fieldsDiffer += o.fieldsDiffer;
    mastersDiffer += o.mastersDiffer;
    for (std::size_t i = 0; i < afterFields.size(); ++i) afterFields[i] += o.afterFields[i];
}

void EffectOps::setLean(bool on, int check) {
    if (on != lean_ || check != leanCheck_) spdlog::info("native: effect units lean {} (check every {} frames)", on ? "on" : "off", check);
    lean_ = on;
    leanCheck_ = std::max(check, 0);
    leanFrameNo_ = ~0ull;
}

void EffectOps::leanUpdateFrame() {
    const std::uint64_t f = dev_.stats().frames;
    if (f == leanFrameNo_) return;
    leanFrameNo_ = f;
    const sc::LeanFrame k = sc::leanFrameKind(f, leanCheck_);
    leanFrameOn_ = lean_ && k == sc::LeanFrame::Lean;
    checkFrame_ = lean_ && k == sc::LeanFrame::Check;
}

bool EffectOps::leanUnit() {
    leanUpdateFrame();
    if (!leanFrameOn_ || !unitTake_ || drawCount_ == 0 || !after_) return false;
    if (leanVerified_ == 0) {
        // the statics eCGfxEffect::Execute writes / reads: ms_pActiveEffect 0xc62ae0, ms_bReset 0xbfbf6e, ms_bOptimize 0xc62ad1 (false
        // in the game: every pass applied), the execute callback 0xc62af0 (sSetEffectExecuteCallback: none)
        const auto& s = engine::sym();
        leanVerified_ = engine_ != nullptr && s.gfxEffectActive == engine_ + 0xc62ae0 && s.gfxEffectReset == engine_ + 0xbfbf6e &&
                                s.gfxEffectOptimize == engine_ + 0xc62ad1
                            ? 1
                            : -1;
        if (leanVerified_ < 0) spdlog::warn("native: effect units lean: eCGfxEffect's statics not at the expected RVAs: the engine's code keeps running");
    }
    if (leanVerified_ < 0) return false;
    if (get<bool>(engine_, 0xc62ad1) || ptr(engine_, 0xc62af0) != nullptr) {
        // ms_bOptimize on (Execute may skip a pass) or an execute callback installed: not modelled, the engine's code runs
        if (leanStats_ != nullptr) ++leanStats_->fallbackBlock;
        return false;
    }
    if (hal_.stateBlockOpen()) {
        // BeginTechnique would restart an open block (the HAL does not nest them): the engine's code keeps that behaviour
        if (leanStats_ != nullptr) ++leanStats_->fallbackBlock;
        return false;
    }
    return true;
}

void EffectOps::issueLean() {
    if (drawCount_ == 0) return;
    d3d9::draws::Tag& tag = dev_.drawTag();
    const d3d9::draws::Tag savedTag = tag;
    issuing_ = true;
    for (std::size_t k = 0; k < drawCount_; ++k) {
        const fxc::EffectDraw& d = *draws_[k];
        applyDevice(d);
        issueIndex_ = k;
        if (d.indexed) dev_.drawIndexedPrimitive(d.primType, d.baseVertex, d.minIndex, d.numVertices, d.startIndex, d.primCount);
        else dev_.drawPrimitive(d.primType, d.startVertex, d.primCount);
        ++stats_->issuedDraws;
    }
    issuing_ = false;
    tag = savedTag;
    // EndTechnique's EndRenderStateBlock: the render states as at the entry
    for (std::uint32_t rs = 0; rs < fxc::kRenderStates; ++rs) {
        if (dev_.renderState(rs) != entryRs_[rs]) dev_.setRenderState(rs, entryRs_[rs]);
    }
}

void EffectOps::predictAfter(bool sprite) {
    afterFields_.clear();
    if (drawCount_ == 0) return;
    if (!after_) after_ = std::make_unique<fxc::EffectDraw>();
    *after_ = *draws_[drawCount_ - 1];
    after_->rs = entryRs_;
    afterMasters_ = hal_.masterBits();
    const auto& s = engine::sym();
    auto* unit = const_cast<std::uint8_t*>(static_cast<U8*>(unitFilter_));
    auto* gfx = const_cast<std::uint8_t*>(ptr(unit, sprite ? 0xe8 : 0xa8));
    const auto field = [&](void* at, std::uint64_t v, std::uint32_t size) { afterFields_.push_back({at, v, size}); };
    // eCGfxEffect::Execute 0x750210: ms_pActiveEffect = the effect (its +0x2c = 0 / ms_bReset = 0 only while ms_bOptimize, which is
    // false: leanUnit; disassembly 0x750236 .. 0x75026d, the check frames showed +0x2c staying 1)
    if (!sprite || spriteExecuted_) field(s.gfxEffectActive, reinterpret_cast<std::uint64_t>(gfx), 8);
    if (sprite) {
        // Flush: the record count; the mesh ring as the last eCSpriteMesh::Draw / RenderBatches' wrap leave it; SetBlend's
        // SetMasterBlending (both factors non-zero in every mode: the flag on)
        field(unit + 0xb0, 0, 4);
        field(unit + 0xd8, spriteEndCount_, 4);
        field(unit + 0xdc, spriteEndStart_, 4);
        if (spriteBlendSet_) afterMasters_ |= 1u << 7;
        return;
    }
    // ExecuteEffectPass: the pass number of the last pass
    field(gfx + 0x28, unitPasses_ != 0 ? unitPasses_ - 1 : 0, 4);
    if (unit_ == Unit::Pssm) {
        // the PSSM loop (OnUpdateOutput 0x668560): UpdateEffect's technique handle, the packed visual's cull / depth func as the last
        // pass set them, ActivateStencilTesting(2, ..) per pass = the HAL's master stencil flag
        U8* techs = ptr(unit, 0xc8);
        const auto ti = get<std::uint32_t>(unit, 0xf0);
        if (techs != nullptr) {
            const void* handle = ptr(techs + 0x18 * static_cast<std::size_t>(ti), 8);
            field(gfx + 0x20, reinterpret_cast<std::uint64_t>(handle), 8);
            // ... which BeginEffectTechnique's SetTechnique then selects (not yet the effect's +0x20 at the composition)
            if (unitInfo_ != nullptr && unitInfo_->techniqueOf(handle) >= 0) unitTech_ = unitInfo_->techniqueOf(handle);
        }
        if (auto* visual = const_cast<std::uint8_t*>(ptr(unit, 0x138))) {
            field(visual + 0x1c, pssmLastCull_, 4);
            field(visual + 0x20, pssmLastDepthFunc_, 4);
        }
        afterMasters_ |= 1u << 6;
    }
}

void EffectOps::adoptAfter() {
    if (unitInfo_ != nullptr) {
        unitInfo_->adoptValues(unitValues_);  // (swapped: unitValues_ now holds the old values, unused)
        for (std::uint32_t i = 0; i < unitTextures_.size(); ++i) unitInfo_->adoptTexture(i, static_cast<const d3d9::Texture*>(unitTextures_[i]));
        unitInfo_->adoptTechnique(unitTech_);
    }
    for (const FieldWrite& w : afterFields_) storeField(w.at, w.value, w.size);
    if (hal_.masterBits() != afterMasters_) hal_.setMasterBits(afterMasters_);
    if (leanStats_ != nullptr) ++leanStats_->skipped;
}

void EffectOps::checkAfter(bool late) {
    afterPending_ = false;
    if (leanStats_ == nullptr || !after_) return;
    LeanStats& st = *leanStats_;
    std::string first;
    // the device: every register, stage and render state (a late check, after the engine's target reset: not the target / viewport)
    if (!afterGot_) afterGot_ = std::make_unique<fxc::EffectDraw>();
    captureDevice(*afterGot_);
    fxc::EffectDraw& got = *afterGot_;
    const fxc::EffectDraw& want = *after_;
    got.indexed = want.indexed;
    got.primType = want.primType;
    got.primCount = want.primCount;
    got.baseVertex = want.baseVertex;
    got.minIndex = want.minIndex;
    got.numVertices = want.numVertices;
    got.startIndex = want.startIndex;
    got.startVertex = want.startVertex;
    if (late) {
        got.rt0 = want.rt0;
        got.depth = want.depth;
        got.viewport = want.viewport;
    }
    fxc::Live all;
    all.vs.set();
    all.ps.set();
    all.stages = 0xffff;
    const fxc::Diff d = fxc::compare(want, got, all);
    ++st.afterChecked;
    if (d.fields.any()) {
        ++st.afterDiffer;
        for (std::size_t i = 0; i < fxc::kFieldCount; ++i) st.afterFields[i] += d.fields.test(i) ? 1 : 0;
        first = "device: " + d.first;
    }
    if (hal_.masterBits() != afterMasters_) {
        ++st.mastersDiffer;
        first += std::format("{}master flags {:#x} vs engine {:#x}", first.empty() ? "" : "; ", afterMasters_, hal_.masterBits());
    }
    // the effect's store
    if (unitInfo_ != nullptr) {
        ++st.storeChecked;
        const fx::ParameterValues& cur = unitInfo_->currentValues();
        const fx::Effect& e = unitInfo_->fxEffect();
        std::string why;
        for (std::size_t i = 0; i < cur.data.size() && i < unitValues_.data.size() && why.empty(); ++i) {
            if (cur.data[i] != unitValues_.data[i]) why = std::format("value of '{}'", i < e.parameters.size() ? e.parameters[i].name : "?");
        }
        for (std::uint32_t i = 0; i < unitTextures_.size() && why.empty(); ++i) {
            if (unitInfo_->currentTexture(i) != unitTextures_[i]) {
                why = std::format("texture of '{}': {} vs engine {}", i < e.parameters.size() ? e.parameters[i].name : "?", unitTextures_[i],
                                  static_cast<const void*>(unitInfo_->currentTexture(i)));
            }
        }
        if (why.empty() && unitInfo_->currentTechnique() != unitTech_) why = std::format("technique {} vs engine {}", unitTech_, unitInfo_->currentTechnique());
        if (!why.empty()) {
            ++st.storeDiffer;
            first += (first.empty() ? "store: " : "; store: ") + why;
        }
    }
    for (const FieldWrite& w : afterFields_) {
        ++st.fieldsChecked;
        const std::uint64_t v = loadField(w.at, w.size);
        if (v != w.value) {
            ++st.fieldsDiffer;
            first += std::format("{}field {} = {:#x} vs engine {:#x}", first.empty() ? "" : "; ", w.at, w.value, v);
        }
    }
    if (!first.empty() && (afterLogged_ < 24 || hal::vulkan::logThisFrame())) {
        ++afterLogged_;
        spdlog::info("native: effect units lean: {}: the prediction differs from the engine's code: {}", unitName_, first);
    }
}

void EffectOps::reportLean() {
    if (!lean_ && leanWindow_.empty()) return;
    std::string line;
    for (auto& [name, w] : leanWindow_) {
        LeanStats& t = leanTotal_[name];
        t.add(w);
        line += std::format("{}{} {}", line.empty() ? "" : ", ", name, w.skipped);
        if (w.fallbackBlock != 0) line += std::format(" (kept {}: a state block open / ms_bOptimize / an execute callback)", w.fallbackBlock);
    }
    std::uint64_t checked = 0, differ = 0, store = 0, storeDiffer = 0, fields = 0, fieldsDiffer = 0, masters = 0;
    std::array<std::uint64_t, fxc::kFieldCount> by{};
    for (const auto& [name, t] : leanTotal_) {
        checked += t.afterChecked;
        differ += t.afterDiffer;
        store += t.storeChecked;
        storeDiffer += t.storeDiffer;
        fields += t.fieldsChecked;
        fieldsDiffer += t.fieldsDiffer;
        masters += t.mastersDiffer;
        for (std::size_t i = 0; i < by.size(); ++i) by[i] += t.afterFields[i];
    }
    std::string fieldsTxt;
    for (std::size_t i = 0; i < by.size(); ++i) {
        if (by[i] != 0) fieldsTxt += std::format("{}{} {}", fieldsTxt.empty() ? "" : ", ", fxc::name(static_cast<fxc::Field>(i)), by[i]);
    }
    spdlog::info("native: effect units lean (5 s): engine code skipped: {}; check (total): after {} / {} differing{}, store {} / {} differing, "
                 "fields {} / {} differing, master flags differing {}",
                 line.empty() ? "none" : line, differ, checked, fieldsTxt.empty() ? "" : " (" + fieldsTxt + ")", storeDiffer, store, fieldsDiffer,
                 fields, masters);
    for (auto& [name, w] : leanWindow_) w = {};
}

}  // namespace rvk::native
