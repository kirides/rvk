#pragma once

// The RHI side of one injected pass call (docs/research/hlsl-rhi.md 8): the frame's command buffer as an rhi::CommandList and non-owning rhi::Textures
// over the renderer's images (transients, engine textures), all released at the end of the call. The images keep their vk::Image layout bookkeeping,
// so the pass's RHI transitions and the renderer's own barriers (Device::readable, the engine draws) agree.

#include <memory>
#include <vector>

#include "d3d9/device.h"
#include "rhi/rhi.h"
#include "rhi/vulkan/wrap.h"
#include "vk/passes.h"

namespace rvk::enh {

class RhiScope {
public:
    RhiScope(d3d9::Device& dev, VkCommandBuffer cmd) : rdev_(dev.rhi()) {
        if (rdev_ != nullptr) cl_ = rhi::vulkan::wrapCommandBuffer(*rdev_, cmd);
    }
    explicit operator bool() const { return cl_ != nullptr; }
    rhi::Device& device() { return *rdev_; }
    rhi::CommandList& cl() { return *cl_; }

    rhi::Texture* wrap(vk::Image& image, std::uint32_t sampledSlot = 0, const char* name = nullptr) {
        textures_.push_back(rhi::vulkan::wrapImage(*rdev_, image, sampledSlot, name));
        return textures_.back().get();
    }
    rhi::Texture* wrap(vk::Transients::Entry& e) { return wrap(e.image, e.slot, e.name.c_str()); }
    rhi::Texture* wrap(d3d9::Texture& t) { return wrap(t.image(), t.bindlessSlot()); }

private:
    rhi::Device* rdev_ = nullptr;
    std::unique_ptr<rhi::CommandList> cl_;
    std::vector<std::unique_ptr<rhi::Texture>> textures_;
};

}  // namespace rvk::enh
