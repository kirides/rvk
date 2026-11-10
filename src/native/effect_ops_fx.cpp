// S-7d (docs/research/frame-ownership.md 14.18): every composed effect draw of the native frame on our own shaders. The draws of the shadow mask chain
// (UpscaleSSAO, the SSM / DSSM cascade quads, VSSM), the SSAO chain (SSAO, BlurSSAO x 2, Blur), the PSSM point-light pass, the terrain map's Blur7H / V and
// the post chain's units are drawn with the ports of their vertex and pixel shaders (shaders/post/fx, OwnPost::fxPort) while `native.own_post=on`. Compare
// draws the same ports into scratch copies of the targets next to the engine's draws and compares the images (`native: own post (5 s): image '<port>'`).

#include <format>

#include <spdlog/spdlog.h>

#include "d3d9/defs.h"
#include "native/effect_ops.h"

namespace rvk::native {

int EffectOps::fxKind() const {
    switch (unit_) {
    case Unit::Mask: return 0;
    case Unit::Chain: return 1;
    case Unit::Pssm: return 2;
    case Unit::TerrainBlur: return 3;
    case Unit::Post: return 4;
    default: return 5;
    }
}

void EffectOps::drawComposed(const fxc::EffectDraw& d) {
    using d3d9::Shader;
    const Shader* vs = nullptr;
    const Shader* ps = nullptr;
    if (own_->mode() == OwnPost::Mode::On && unit_ != Unit::Gui) {
        const char* whyVs = nullptr;
        const char* whyPs = nullptr;
        vs = own_->fxPort(d.vs, &whyVs);
        // (a pixel shader that is already ours: an S-7c unit port)
        const bool psOursAlready = d.ps != nullptr && static_cast<const Shader*>(d.ps)->bytecode.empty();
        ps = psOursAlready ? nullptr : own_->fxPort(d.ps, &whyPs);
        own_->countFx(fxKind(), vs != nullptr, ps != nullptr || psOursAlready, vs == nullptr && d.vs != nullptr, ps == nullptr && !psOursAlready && d.ps != nullptr,
                      vs == nullptr ? whyVs : whyPs);
    }
    if (vs != nullptr) dev_.setVertexShaderRef(d3d9::ShaderRef(const_cast<Shader*>(vs)));
    if (ps != nullptr) dev_.setPixelShaderRef(d3d9::ShaderRef(const_cast<Shader*>(ps)));
    if ((vs != nullptr || ps != nullptr) && unitCompare_) {
        // (the own-draw compare of check frames: the composition as drawn)
        if (!portPatched_) portPatched_ = std::make_unique<fxc::EffectDraw>();
        *portPatched_ = d;
        if (vs != nullptr) portPatched_->vs = vs;
        if (ps != nullptr) portPatched_->ps = ps;
        portActive_ = true;
    }
    if (d.indexed) dev_.drawIndexedPrimitive(d.primType, d.baseVertex, d.minIndex, d.numVertices, d.startIndex, d.primCount);
    else dev_.drawPrimitive(d.primType, d.startVertex, d.primCount);
    portActive_ = false;
    if (vs != nullptr) dev_.setVertexShaderRef(d3d9::ShaderRef(const_cast<Shader*>(static_cast<const Shader*>(d.vs))));
    if (ps != nullptr) dev_.setPixelShaderRef(d3d9::ShaderRef(const_cast<Shader*>(static_cast<const Shader*>(d.ps))));
}

void EffectOps::fxCompareBegin() {
    finishFxCompare();
    if (own_->mode() != OwnPost::Mode::Compare || drawCount_ == 0 || unit_ == Unit::Gui || unit_ == Unit::Copy) return;
    // (the S-7c single units compare themselves: finishOwnCompare)
    if (cmpUnit_ != PostUnit::None) return;
    const bool pssm = unit_ == Unit::Pssm;
    if (!pssm && drawCount_ != 1) {
        ++fxCmpSkipped_;
        return;
    }
    const fxc::EffectDraw& first = *draws_[0];
    auto* target = static_cast<d3d9::Surface*>(const_cast<void*>(first.rt0));
    if (target == nullptr) return;
    for (std::size_t k = 1; k < drawCount_; ++k) {
        if (draws_[k]->rt0 != first.rt0) {
            ++fxCmpSkipped_;
            return;
        }
    }
    // the ports of every draw; the compared name = the pixel shader's port (the last draw's: the PSSM pass's colour pass)
    const char* name = nullptr;
    bool any = false;
    for (std::size_t k = 0; k < drawCount_; ++k) {
        const fxc::EffectDraw& d = *draws_[k];
        const bool vs = own_->fxPort(d.vs) != nullptr, ps = own_->fxPort(d.ps) != nullptr;
        any |= vs || ps;
        if (ps && own_->fxPortName(d.ps) != nullptr) name = own_->fxPortName(d.ps);
    }
    if (!any) return;
    if (name == nullptr) name = "effect (vertex shader ports only)";
    if (pssm) {
        // the PSSM sequence writes the stencil (INCR / DECR per cell, the colour pass zeroes what it drew): only over the 0 everywhere VSSM leaves, and
        // without depth writes, so the engine's own sequence afterwards finds the stencil / depth as it was
        bool vssm = false;
        for (const std::string& p : plan_) vssm |= p == "VSSM";
        bool zwrite = false;
        for (std::size_t k = 0; k < drawCount_; ++k) zwrite |= draws_[k]->rs[d3d9::RS_ZENABLE] != 0 && draws_[k]->rs[d3d9::RS_ZWRITEENABLE] != 0;
        if (!vssm || zwrite) {
            ++fxCmpSkipped_;
            return;
        }
    }
    d3d9::Surface* scratch = own_->scratchFor(target, true);
    if (scratch == nullptr) return;
    if (!portPatched_) portPatched_ = std::make_unique<fxc::EffectDraw>();
    for (std::size_t k = 0; k < drawCount_; ++k) {
        fxc::EffectDraw& p = *portPatched_;
        p = *draws_[k];
        if (const d3d9::Shader* v = own_->fxPort(p.vs)) p.vs = v;
        if (const d3d9::Shader* s = own_->fxPort(p.ps)) p.ps = s;
        p.rt0 = scratch;
        if (!pssm) {
            // (the stencil and depth the engine's draw reads: tested, not written)
            p.rs[d3d9::RS_STENCILWRITEMASK] = 0;
            p.rs[d3d9::RS_ZWRITEENABLE] = 0;
        }
        issueOne(p);
    }
    fxCmpScratch_ = scratch;
    fxCmpTarget_ = target;
    fxCmpName_ = name;
}

void EffectOps::finishFxCompare() {
    if (fxCmpScratch_ == nullptr) return;
    own_->compareUnit(fxCmpName_, fxCmpScratch_, fxCmpTarget_);
    fxCmpScratch_ = nullptr;
    fxCmpTarget_ = nullptr;
    fxCmpName_ = nullptr;
}

}  // namespace rvk::native
