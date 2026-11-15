#pragma once

// Sample distribution shadow maps (shadows.fit = sdsm; docs/research/shadows.md 8.9): a compute reduction over the main view's depth, after the z pre-pass,
// finds per view-depth bin (128, log spaced) the light-space x / y bounds of the visible (non-sky) pixels. The result is copied into a read-back buffer
// (no wait: a slot is read when the GPU has completed its frame, one or two frames later, at the top of the next cascade update, engine/shadow_maps.cpp)
// and handed to the UpdateCamera pre-hook, which fits every cascade's box to it. Off = the pass is not registered, no pipeline / buffer exists.

#include <array>
#include <cstdint>
#include <future>
#include <memory>

#include "d3d9/device.h"
#include "engine/shadow_maps.h"
#include "rhi/rhi.h"
#include "shader/abi.h"

namespace rvk::enh {

// mirror of shaders/shadows/sdsm.hlsli SdsmPush
struct SdsmPush {
    shader::abi::PassPushHeader h;
    std::uint32_t depthTex, width, height, stride;
    std::uint32_t bins[2];
    std::uint32_t pad[2];
    float range[4];
    float lx[4];
    float ly[4];
};
static_assert(sizeof(SdsmPush) == 96 && sizeof(SdsmPush) <= shader::abi::kPushConstantBudget);

class Sdsm {
public:
    explicit Sdsm(d3d9::Device& device);
    ~Sdsm();
    Sdsm(const Sdsm&) = delete;
    Sdsm& operator=(const Sdsm&) = delete;

    void setEnabled(bool on);  // render thread (the settings callbacks run at the frame boundary)
    bool enabled() const { return on_; }

private:
    static constexpr std::uint32_t kSlots = 4;  // reductions in flight
    static constexpr std::uint32_t kBytes = engine::shadowmaps::SdsmFrame::kBins * 4 * 4;
    struct Slot {
        std::unique_ptr<rhi::Buffer> gpu, readback;
        std::uint64_t frame = 0;  // Device::frameValue() of the submission that copies (0 = free)
        engine::shadowmaps::SdsmFrame meta;
    };
    static void pollTrampoline();
    void poll();
    void record(d3d9::PassContext& ctx);
    bool ensureResources(rhi::Device& rdev);

    d3d9::Device& dev_;
    bool on_ = false;
    bool failed_ = false;
    std::uint32_t passId_ = 0;
    std::future<std::unique_ptr<rhi::Pipeline>> build_;
    std::unique_ptr<rhi::Pipeline> pipeline_;
    std::array<Slot, kSlots> slots_;
    std::uint64_t recorded_ = 0, skipped_ = 0;
};

}  // namespace rvk::enh
