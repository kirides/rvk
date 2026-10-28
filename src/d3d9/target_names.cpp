#include "d3d9/target_names.h"

#include <array>

#include "d3d9/device.h"

namespace rvk::d3d9::debugnames {
namespace {

// the frame's distinct targets of the pass, in bind order (render thread only)
struct FrameState {
    std::uint64_t frame = ~0ull;
    std::array<Texture*, 8> ssao{};
    std::uint32_t ssaoCount = 0;
    Texture* depthCopy = nullptr;
    std::array<Texture*, 4> full{};  // scene-sized colour targets
    std::uint32_t fullCount = 0;
};
FrameState g_state;

template <std::size_t N>
bool contains(const std::array<Texture*, N>& a, std::uint32_t count, const Texture* t) {
    for (std::uint32_t i = 0; i < count; ++i) {
        if (a[i] == t) return true;
    }
    return false;
}

}  // namespace

void onShadowMaskTarget(Device& device, Surface* surface) {
    if (surface == nullptr || surface == device.backBuffer()) return;
    Texture* t = surface->texture();
    if (t == nullptr || t->isNull() || t->isDepth()) return;
    FrameState& s = g_state;
    if (s.frame != device.stats().frames) {
        s = FrameState{};
        s.frame = device.stats().frames;
    }
    const TextureDesc& d = t->desc();
    if (d.format == vk::d3dfmt::R32F) {
        if (s.depthCopy != t) {
            s.depthCopy = t;
            t->setDebugLabel("shadow mask: 1/4 depth");
        }
        return;
    }
    // "scene-sized": the scene target's size when known (the mask is allocated at the scene size, the SSAO buffers at half)
    const Texture* hdr = device.imageWithRole(scene::ImageRole::SceneHdr);
    const bool full = hdr == nullptr || d.width * 4 > hdr->desc().width * 3;
    if (!full) {
        if (!contains(s.ssao, s.ssaoCount, t) && s.ssaoCount < s.ssao.size()) {
            s.ssao[s.ssaoCount] = t;
            t->setDebugLabel("shadow mask: SSAO buffer", s.ssaoCount);
            ++s.ssaoCount;
        }
        return;
    }
    if (contains(s.full, s.fullCount, t) || s.fullCount >= s.full.size()) {
        // rebound (the blur reads the mask, then writes the next one): the newest target stays the final mask
        return;
    }
    if (s.fullCount > 0) s.full[s.fullCount - 1]->setDebugLabel("shadow mask: before blur");
    s.full[s.fullCount++] = t;
    t->setDebugLabel("shadow mask");
}

}  // namespace rvk::d3d9::debugnames
