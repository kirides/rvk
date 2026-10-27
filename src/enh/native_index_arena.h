#pragma once

// One device-local 32-bit index buffer for the native passes (docs/research/native-renderer.md 14): the index ranges of the static mesh
// parts the passes draw, widened to u32 once (from the engine's index buffer CPU copy) and appended; the opaque and alpha-tested lists are
// then one indirect call each (one index buffer binding) whatever buffers and index sizes the engine's meshes use. The vertices stay in the
// engine's buffers (pulled by address). Render thread.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "d3d9/device.h"
#include "vk/vk.h"

namespace rvk::enh {

struct NativePart;

class NativeIndexArena {
public:
    static constexpr std::uint32_t kCapacity = 16u << 20;  // indices (64 MiB)
    static constexpr std::uint32_t kNone = 0xFFFFFFFFu;

    explicit NativeIndexArena(d3d9::Device& device);
    ~NativeIndexArena();
    NativeIndexArena(const NativeIndexArena&) = delete;
    NativeIndexArena& operator=(const NativeIndexArena&) = delete;

    // First index (in u32 units) of the part's indices in the arena (stored now or on an earlier call); kNone when they cannot be had (the
    // CPU copy is unreadable, the arena is full: it is replaced at the next frame boundary). Indices are absolute (relative to the part's
    // vertex stream start), as in the engine's buffers.
    std::uint32_t get(const NativePart& part);
    // Frame boundary (once per frame, before get): replaces a full arena (the old buffer is retired behind the frames in flight)
    void beginFrame();
    // Copies what get() staged into the buffer, with the barrier for the index fetch (outside a rendering scope)
    void flush(VkCommandBuffer cmd);
    VkBuffer buffer() const { return buffer_; }
    bool ok() const { return buffer_ != VK_NULL_HANDLE; }

    struct Stats {
        std::uint64_t parts = 0, indices = 0, resets = 0, unreadable = 0, full = 0, suspect = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    struct Key {
        std::uint64_t ib = 0;
        std::uint32_t gen = 0, first = 0, count = 0, wide = 0;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            std::uint64_t h = k.ib * 0x9E3779B97F4A7C15ull;
            h ^= (static_cast<std::uint64_t>(k.gen) << 32 | k.first) + 0x7F4A7C15u + (h << 6) + (h >> 2);
            h ^= (static_cast<std::uint64_t>(k.count) << 1 | k.wide) + 0x9E3779B9u + (h << 6) + (h >> 2);
            return static_cast<std::size_t>(h);
        }
    };
    bool create();
    void destroyBuffer();

    d3d9::Device& dev_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    std::uint32_t used_ = 0;
    bool resetPending_ = false;
    std::unordered_map<Key, std::uint32_t, KeyHash> cache_;
    std::vector<std::uint32_t> staged_;       // widened indices not yet copied
    std::uint32_t stagedFirst_ = 0;           // arena offset of staged_[0]
    std::vector<std::uint8_t> raw_;
    Stats stats_;
};

}  // namespace rvk::enh
