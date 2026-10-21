#pragma once

// GPU timestamps per frame slot ([debug] perf). Each submission writes a "begin" mark at the start of its command buffers and
// an "end" mark before it is submitted; marks in between split the frame into labelled segments (a render pass, a copy, the
// present blit). The results of a slot are read after the slot's previous submission has completed (Frames::begin waited
// for it), so reading never stalls. Needs hostQueryReset (core 1.2 feature) and a queue that writes timestamps; otherwise
// the timer stays disabled.

#include <cstdint>
#include <string>
#include <vector>

#include "vk/vk.h"

namespace rvk::vk {

class Context;

class GpuTimer {
public:
    static constexpr std::uint32_t kQueriesPerSlot = 1024;

    bool create(Context& ctx, std::uint32_t slots);
    void destroy();
    bool enabled() const { return pool_ != VK_NULL_HANDLE; }

    // the queue of one command buffer of a submission: the upload buffer runs before the main one
    enum class Stream : std::uint8_t { Main, Upload };

    struct Segment {
        std::string label;
        double ms = 0.0;
    };
    struct Result {
        bool valid = false;
        double mainMs = 0.0;    // first to last mark of the main command buffer
        double uploadMs = 0.0;  // the same for the upload command buffer
        std::vector<Segment> segments;  // main command buffer, only when the submission asked for details
    };

    // render thread. Reads the slot's previous submission (it must have completed) and resets the slot for reuse.
    Result collect(std::uint32_t slot);
    // starts recording a slot; `details` keeps the labels of the main stream's marks for Result::segments
    void start(std::uint32_t slot, bool details);
    // writes a timestamp after all earlier commands of `cmd`; the label names the work since the previous mark of the stream
    void mark(VkCommandBuffer cmd, std::uint32_t slot, Stream stream, const char* label);
    void mark(VkCommandBuffer cmd, std::uint32_t slot, Stream stream, std::string&& label);
    bool details(std::uint32_t slot) const { return slot < slots_.size() && slots_[slot].details; }

private:
    struct Mark {
        std::uint32_t query = 0;
        Stream stream = Stream::Main;
        std::string label;
    };
    struct Slot {
        std::vector<Mark> marks;
        bool details = false;
    };
    void write(VkCommandBuffer cmd, std::uint32_t slot, Stream stream, std::string&& label);

    Context* ctx_ = nullptr;
    VkQueryPool pool_ = VK_NULL_HANDLE;
    double nsPerTick_ = 1.0;
    std::vector<Slot> slots_;
    std::vector<std::uint64_t> scratch_;
};

}  // namespace rvk::vk
