#pragma once

// Occlusion probe (measurement, docs/research/performance.md 15): after the main view's z pre-pass the depth map is complete.
// A max-depth pyramid of it (shaders/occlusion/hzb_down.comp) and a per-sphere test (probe.comp) say, for each sphere the RT
// scene holds (the world instances the engine drew), whether it lies entirely behind the depth the z pre-pass wrote, i.e. whether
// its solid-pass draw could not have produced a pixel. The result is read back two frames later (the frame slot's turn) and
// logged as a fraction of instances and triangles: the potential of culling occluded draws. Nothing is culled by it.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "d3d9/device.h"
#include "vk/passes.h"

namespace rvk::enh {

struct OccSphere {
    float x = 0.0f, y = 0.0f, z = 0.0f, r = 0.0f;  // world centre and radius (r = 0: not tested, counts as visible)
};

class OcclusionProbe {
public:
    explicit OcclusionProbe(d3d9::Device& device);
    ~OcclusionProbe();
    OcclusionProbe(const OcclusionProbe&) = delete;
    OcclusionProbe& operator=(const OcclusionProbe&) = delete;

    // inside an injected pass (render thread) of the main view, after the z pre-pass: tests `spheres` (`triangles`: each one's
    // triangle count, for the weighted statistics) and, from the frame slot's previous use, logs the finished result
    void run(d3d9::PassContext& ctx, std::span<const OccSphere> spheres, std::span<const std::uint32_t> triangles);

private:
    static constexpr std::uint32_t kMaxSpheres = 16384;
    static constexpr std::uint32_t kLevels = 8;
    struct Readback {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        std::uint32_t* mapped = nullptr;
        VkDeviceAddress address = 0;
        std::vector<std::uint32_t> triangles;  // of the frame that wrote it
        std::uint32_t count = 0;
        bool pending = false;
    };
    bool ensure();
    void collect(Readback& rb);
    void destroyAll();

    d3d9::Device& dev_;
    vk::PassLayout layout_;
    VkPipeline hzb_ = VK_NULL_HANDLE;
    VkPipeline probe_ = VK_NULL_HANDLE;
    std::array<Readback, vk::Frames::kInFlight> readback_{};
    bool failed_ = false;
    // statistics since the last log
    std::uint64_t frames_ = 0, instances_ = 0, hidden_ = 0, offscreen_ = 0, trianglesAll_ = 0, trianglesHidden_ = 0;
    double lastLog_ = 0.0;
};

}  // namespace rvk::enh
