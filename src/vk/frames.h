#pragma once

// Frames in flight on one timeline semaphore, deferred deletion, the transient upload ring.
//
// Frame k (k = 1, 2, ...) signals timeline value k when its submission completes. Frame k reuses the slot of frame k - N, so
// begin() waits for value k - N. Anything retired while recording frame k is destroyed once value k has completed.
//
// The ring hands out persistently mapped, device-addressable memory that lives until the frame that allocated it has completed
// (shader constants, D3D9 DISCARD/NOOVERWRITE dynamic buffers, DrawUP data, staging of render-thread uploads). It belongs to the
// render thread; uploads from loader threads use their own staging buffers (Frames::retire keeps them alive).
//
// Ring memory rotates per *frame* (a present), not per submission (frame-ownership.md 14.40): a mid-frame submit (a read-back's flush, the manual
// frame generation's first present) begins a continuation (begin(true)): a new command slot and timeline value, but the ring keeps its memory slot and
// its blocks. So what a frame allocated (vertex captures, constants) stays valid through the whole next frame however many values the frames took.

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "rhi/game_device.h"
#include "vk/vk.h"

namespace rvk::vk {

class Context;

// the ring block's VkBuffer (rhi::RingAlloc::buffer is the opaque backend handle)
inline VkBuffer vkBuffer(const rhi::RingAlloc& a) { return reinterpret_cast<VkBuffer>(a.buffer); }

class Frames {
public:
    static constexpr std::uint32_t kInFlight = 2;
    static constexpr VkDeviceSize kChunkSize = 32ull << 20;

    bool create(Context& ctx);
    void destroy();

    // starts frame recording: waits for the slot, runs due deletions, recycles ring chunks, begins the command buffers.
    // continuation: the same frame goes on after a mid-frame submit: the ring is not recycled (its blocks stay valid), the memory epoch stays.
    void begin(bool continuation = false);
    // ends and submits the frame's upload + main command buffers. waitAcquire / signalRendered may be VK_NULL_HANDLE.
    bool submit(VkSemaphore waitAcquire, VkSemaphore signalRendered);

    VkCommandBuffer cmd() const { return slots_[slot()].cmd; }
    // recorded and submitted before cmd(): copies that must land before the frame's draws (begun lazily)
    VkCommandBuffer uploadCmd();
    bool recording() const { return recording_; }

    std::uint64_t currentValue() const { return frameValue_; }  // value the frame being recorded will signal
    // counts the non-continuation begins: ring memory allocated in epoch E is valid through epoch E + kInFlight - 1
    std::uint64_t memoryEpoch() const { return memoryEpoch_; }
    std::uint32_t currentSlot() const { return slot(); }
    bool uploadBegun() const { return slots_[slot()].uploadBegun; }
    // statistics: ms the last begin() waited for its slot (the CPU ran a full slot ahead of the GPU); ring bytes handed out
    double lastSlotWaitMs() const { return lastSlotWaitMs_; }
    std::uint64_t ringBytes() const { return ring_->bytes; }
    // the ring's current-chunk state lives in a rhi::RingChunkState (the game device's inline allocRing front end shares it); default: an own one
    void attachRing(rhi::RingChunkState* state) { ring_ = state != nullptr ? state : &ownRing_; }
    std::uint64_t completedValue() const;
    void waitFor(std::uint64_t value) const;
    void waitIdle();

    // destroy `fn` once the frame being recorded (or the last submitted one, between frames) has completed; thread-safe
    void retire(std::function<void()> fn);

    // transient memory for the frame being recorded (render thread); alignment must be a power of two. minOffset: the block
    // starts at least that far into its chunk (a dynamic buffer's block of bytes [lo, hi) is bound at offset - lo: d3d9::RingBlock)
    rhi::RingAlloc allocate(VkDeviceSize size, VkDeviceSize alignment = 256, VkDeviceSize minOffset = 0);

private:
    struct Chunk {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        std::uint8_t* ptr = nullptr;
        VkDeviceAddress address = 0;
        VkDeviceSize size = 0;
    };
    struct Slot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBuffer upload = VK_NULL_HANDLE;
        bool uploadBegun = false;
    };
    // the ring's memory slots (rotated by non-continuation begins): chunks used, the last timeline value that recorded with the slot
    struct MemSlot {
        std::vector<std::size_t> chunks;  // indices into chunks_
        std::uint64_t lastValue = 0;
    };
    static rhi::RingAlloc makeAlloc(const Chunk& c, VkDeviceSize offset);
    std::uint32_t slot() const { return static_cast<std::uint32_t>(frameValue_ % kInFlight); }
    bool newChunk(VkDeviceSize minSize);
    void runRetired(std::uint64_t completed);

    Context* ctx_ = nullptr;
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    Slot slots_[kInFlight];
    MemSlot mem_[kInFlight];
    std::uint32_t memSlot_ = 0;
    std::uint64_t memoryEpoch_ = 0;
    std::uint64_t frameValue_ = 1;  // value of the frame being recorded / next to record
    bool recording_ = false;
    double lastSlotWaitMs_ = 0.0;
    rhi::RingChunkState ownRing_;
    rhi::RingChunkState* ring_ = &ownRing_;

    std::vector<Chunk> chunks_;
    std::vector<std::size_t> freeChunks_;
    std::size_t currentChunk_ = SIZE_MAX;

    std::mutex retireMutex_;
    std::vector<std::pair<std::uint64_t, std::function<void()>>> retired_;
};

}  // namespace rvk::vk
