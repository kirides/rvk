#pragma once

// Crash markers (debug.crash_markers; VK_AMD_buffer_marker, else vkCmdFillBuffer behind full barriers): the GPU writes an event number into host-visible memory when its
// command processor reaches a region's start (top of pipe: slot 0) and when all earlier work finished at a region's end
// (bottom of pipe: slot 1). After a VK_ERROR_DEVICE_LOST the events between the last finished one and the last started one are
// where the GPU was when it hung or faulted (`report`). Events are the debug regions (d3d9::Device::regionBegin / End: views,
// passes, injected passes), so the markers need debug.regions.

#include <array>
#include <cstdint>
#include <string>

#include "vk/context.h"

namespace rvk::vk {

class CrashMarkers {
public:
    CrashMarkers() = default;
    CrashMarkers(const CrashMarkers&) = delete;
    CrashMarkers& operator=(const CrashMarkers&) = delete;
    ~CrashMarkers() { destroy(); }

    // false without VK_AMD_buffer_marker / on failure
    bool create(Context& ctx);
    void destroy();
    bool valid() const { return buffer_ != VK_NULL_HANDLE; }

    // record a start / end event of region `text` (frame = the frame value for the report) into `cmd`
    void begin(VkCommandBuffer cmd, std::uint64_t frame, const char* text);
    void end(VkCommandBuffer cmd, std::uint64_t frame, const char* text);

    // the text for the log after a device loss: the events the GPU had not finished yet
    std::string report() const;

private:
    struct Event {
        std::uint64_t frame = 0;
        char text[48] = {};
        bool start = false;
    };
    static constexpr std::uint32_t kEvents = 8192;
    void write(VkCommandBuffer cmd, std::uint64_t frame, const char* text, bool start);

    Context* ctx_ = nullptr;
    bool amd_ = false;  // VK_AMD_buffer_marker (else fill buffer + barriers)
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation alloc_ = nullptr;
    volatile std::uint32_t* mapped_ = nullptr;  // [0] last started event id, [1] last finished event id
    std::uint32_t next_ = 0;                    // ids start at 1
    std::array<Event, kEvents> events_{};
};

}  // namespace rvk::vk
