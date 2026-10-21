#pragma once

// Frames in flight on one timeline semaphore, deferred deletion, the transient upload ring.
//
// Frame k (k = 1, 2, ...) signals timeline value k when its submission completes. Frame k reuses the slot of frame k - N, so
// begin() waits for value k - N. Anything retired while recording frame k is destroyed once value k has completed.
//
// The ring hands out persistently mapped, device-addressable memory that lives until the frame that allocated it has completed
// (shader constants, D3D9 DISCARD/NOOVERWRITE dynamic buffers, DrawUP data, staging of render-thread uploads). It belongs to the
// render thread; uploads from loader threads use their own staging buffers (Frames::retire keeps them alive).

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "vk/vk.h"

namespace rvk::vk {

class Context;

struct RingAlloc {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    void* ptr = nullptr;
    VkDeviceAddress address = 0;
};

class Frames {
public:
    static constexpr std::uint32_t kInFlight = 2;
    static constexpr VkDeviceSize kChunkSize = 32ull << 20;

    bool create(Context& ctx);
    void destroy();

    // starts frame recording: waits for the slot, runs due deletions, recycles ring chunks, begins the command buffers
    void begin();
    // ends and submits the frame's upload + main command buffers. waitAcquire / signalRendered may be VK_NULL_HANDLE.
    bool submit(VkSemaphore waitAcquire, VkSemaphore signalRendered);

    VkCommandBuffer cmd() const { return slots_[slot()].cmd; }
    // recorded and submitted before cmd(): copies that must land before the frame's draws (begun lazily)
    VkCommandBuffer uploadCmd();
    bool recording() const { return recording_; }

    std::uint64_t currentValue() const { return frameValue_; }  // value the frame being recorded will signal
    std::uint64_t completedValue() const;
    void waitFor(std::uint64_t value) const;
    void waitIdle();

    // destroy `fn` once the frame being recorded (or the last submitted one, between frames) has completed; thread-safe
    void retire(std::function<void()> fn);

    // transient memory for the frame being recorded (render thread); alignment must be a power of two
    RingAlloc allocate(VkDeviceSize size, VkDeviceSize alignment = 256);

private:
    struct Chunk {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        std::uint8_t* ptr = nullptr;
        VkDeviceAddress address = 0;
        VkDeviceSize size = 0;
        VkDeviceSize used = 0;
    };
    struct Slot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBuffer upload = VK_NULL_HANDLE;
        bool uploadBegun = false;
        std::vector<std::size_t> chunks;  // indices into chunks_ used by this slot
    };
    std::uint32_t slot() const { return static_cast<std::uint32_t>(frameValue_ % kInFlight); }
    bool newChunk(VkDeviceSize minSize);
    void runRetired(std::uint64_t completed);

    Context* ctx_ = nullptr;
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    Slot slots_[kInFlight];
    std::uint64_t frameValue_ = 1;  // value of the frame being recorded / next to record
    bool recording_ = false;

    std::vector<Chunk> chunks_;
    std::vector<std::size_t> freeChunks_;
    std::size_t currentChunk_ = SIZE_MAX;

    std::mutex retireMutex_;
    std::vector<std::pair<std::uint64_t, std::function<void()>>> retired_;
};

}  // namespace rvk::vk
