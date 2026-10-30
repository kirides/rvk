#pragma once

// The RHI side of one injected pass call (docs/research/hlsl-rhi.md 8, dx12-game-port.md WP2b): the pass's command list (PassContext::cmd, an
// rhi::CommandList over the frame's commands) and the device it belongs to, with `wrap` as one spelling for "the RHI texture of this": engine textures
// (d3d9::Texture::rhi()) and the transient images (already rhi::Texture) alike. Nothing is allocated or released any more: the textures are the
// device's own, their layouts tracked by the RHI texture.

#include "d3d9/device.h"
#include "rhi/rhi.h"

namespace rvk::enh {

class RhiScope {
public:
    RhiScope(d3d9::Device& dev, rhi::CommandList& cmd) : rdev_(dev.rhi()), cl_(&cmd) {}
    explicit operator bool() const { return rdev_ != nullptr; }
    rhi::Device& device() { return *rdev_; }
    rhi::CommandList& cl() { return *cl_; }

    rhi::Texture* wrap(rhi::Texture& t) { return &t; }
    rhi::Texture* wrap(d3d9::Texture& t) { return &t.rhi(); }

private:
    rhi::Device* rdev_ = nullptr;
    rhi::CommandList* cl_ = nullptr;
};

}  // namespace rvk::enh
