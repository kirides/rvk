#pragma once

// Per-object motion vectors for FSR (docs/research/upscaling.md): the G-buffer's motion is the camera's motion of a surface
// point, which is wrong for everything that moves by itself. Characters (skinned meshes) and rigid parts that move (a weapon
// in a hand) get their own: the vertex shader's capture variant (d3d9::VariantSlot::Capture, enh/rt_scene's) writes each
// vertex's world position to a ring buffer every frame and the G-buffer's pixel variant (GbufferMotion) reads the previous
// frame's capture of the same draw: motion = where that world point was on the previous screen - where it is now.
//
// The RT scene does the same for skinned draws while it is active (GI / ray-traced reflections); this observer is the
// light-weight version without acceleration structures: on while an FSR mode is, it leaves skinned draws to the RT scene
// when that captures them. Rigid draws (static meshes) are captured only while their world constants change (c31..c37 of
// the vertex shader: g_vInstanceMisc / g_vInstanceData's world matrix; a fade's alpha changes them too, harmlessly: the
// previous position then equals the current one and the motion is the camera's).

#include <cstdint>
#include <unordered_map>

#include "d3d9/device.h"

namespace rvk::enh {

class RtScene;

class ObjectMotion final : public d3d9::DrawObserver {
public:
    ObjectMotion(d3d9::Device& device, RtScene* scene);
    ~ObjectMotion() override;
    ObjectMotion(const ObjectMotion&) = delete;
    ObjectMotion& operator=(const ObjectMotion&) = delete;

    // `skinned` / `rigid`: which draws get captured (both off = unregistered)
    void apply(bool skinned, bool rigid);

    // d3d9::DrawObserver
    bool observes(d3d9::draws::Kind kind, d3d9::scene::Phase phase) const override;
    void observe(d3d9::Device& device, const d3d9::ReplaceQuery& query) override;
    void bufferChanged(std::uint64_t bufferId) override;

private:
    struct State {
        std::uint64_t vb = 0, ib = 0;  // buffer ids (bufferChanged)
        std::uint32_t maxIndex = 0;
        bool scanned = false;
        std::uint64_t hash = 0;
        bool haveHash = false;
        int moving = 0;  // frames left of capturing a rigid draw after its constants changed
        VkDeviceAddress pending = 0;  // requested last time: used when it ran
        std::uint32_t pendingSlots = 0;
        std::uint64_t pendingFrame = 0;
        VkDeviceAddress last = 0;  // the last capture that ran (its ring memory lives one frame more)
        std::uint32_t lastSlots = 0;
        std::uint64_t lastFrame = 0;
        std::uint64_t seenFrame = 0;
    };

    d3d9::Device& dev_;
    RtScene* rt_ = nullptr;
    bool skinned_ = false, rigid_ = false, registered_ = false;
    std::unordered_map<std::uint64_t, State> states_;
    std::uint64_t frame_ = 0;
    std::uint32_t perFrame_ = 0;
    std::uint64_t captures_ = 0, rigidMoving_ = 0;
    double lastLog_ = -1e9;
    std::uint64_t diagRigid_ = 0, diagNoEntity_ = 0, diagInstanced_ = 0, diagRing_ = 0;
    double diagLog_ = -1e9;
};

}  // namespace rvk::enh
