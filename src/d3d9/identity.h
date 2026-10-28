#pragma once

// Persistent resource identity (roadmap do-now item 3, docs/research/draw-tags.md 2): every Buffer / Texture gets a process-wide
// 64-bit id that is never reused, plus a generation that changes whenever its GPU contents were replaced from the CPU. A
// (id, generation) pair names one version of the data: cache keys of later features (BLAS per vertex buffer version,
// previous-frame data) and the geometry identity of draw records.
//
// Rules (unit-tested: tests/draws_tests.cpp):
//   * Buffer, static (managed / default pool): a new VkBuffer per update; the generation counts them (1 = the first upload).
//     Using it again without a write keeps the generation.
//   * Buffer, dynamic or system memory: a ring copy per frame (and after a plain lock or a DISCARD); the generation counts the
//     copies. NOOVERWRITE appends and repeated writes into the frame's copy patch it in place: same generation (the
//     earlier draws of the frame read other ranges, D3D9's NOOVERWRITE contract).
//   * Texture: its VkImage is never replaced (a resize makes a new Texture = a new id); the generation counts the uploads of
//     CPU data (file data, locks). Render-target writes do not change it.
// Pure logic, no Vulkan.

#include <cstdint>

namespace rvk::d3d9 {

struct ResourceRef {
    std::uint64_t id = 0;          // 0 = none
    std::uint32_t generation = 0;  // 0 = never on the GPU
    bool operator==(const ResourceRef&) const = default;
};

// process-wide, thread-safe, starts at 1
std::uint64_t newResourceId();

// The GPU-copy bookkeeping of a d3d9::Buffer: which writes happened since the last copy, and what the next use must copy.
class BufferCopyState {
public:
    // a D3D9 Lock (LOCK_* flags; offset / size 0 = whole buffer; READONLY changes nothing)
    void lock(std::uint32_t offset, std::uint32_t size, std::uint32_t flags, std::uint32_t bufferSize);

    struct Plan {
        enum class Op : std::uint8_t { None, NewCopy, Patch };
        Op op = Op::None;
        std::uint32_t lo = 0, hi = 0;  // bytes to copy from the CPU copy, [lo, hi)
    };
    // the next GPU use: ring = dynamic / system-memory buffer (one ring copy per frame), else a static VkBuffer
    // (hasStatic: one exists already)
    Plan plan(bool ring, std::uint64_t frame, std::uint32_t bufferSize, bool hasStatic) const;
    // the plan was carried out (not called when the allocation failed): a NewCopy bumps the generation
    void committed(const Plan& plan, bool ring, std::uint64_t frame);

    std::uint32_t generation() const { return generation_; }
    bool dirty() const { return dirty_; }

private:
    bool dirty_ = true;
    std::uint32_t dirtyLo_ = 0, dirtyHi_ = 0;  // ring buffers: bytes written since the last copy
    bool discarded_ = false;                    // a DISCARD since the last copy: a fresh copy needs only the dirty range
    std::uint64_t gpuFrame_ = 0;                // frame of the ring copy (0 = none / invalidated)
    std::uint32_t generation_ = 0;
};

// The ring block behind a dynamic buffer's frame copy (Buffer::gpu; docs/research/vulkanize/plan.md V1). A block holds the
// buffer bytes [lo, hi) only, not the whole buffer: a NewCopy reserves the plan's range (after a DISCARD: what was written
// since, e.g. one draw's vertices of our 4 MiB native vertex buffer); a Patch inside the current block [curLo, curHi) is copied
// in place; a Patch outside it (NOOVERWRITE appends past the written range, or below it) moves the copy to a new block over
// both, with the block size doubled (appends grow it O(log n) times). Draws recorded before keep the old block; the new one is
// filled from the CPU copy, which holds every byte. lo is 16-aligned: the binding offset (block offset - lo) keeps the ring's
// 16-byte alignment (index buffers, vertex shifts).
struct RingBlock {
    bool fresh = false;                     // a new block of the extent [lo, hi)
    std::uint32_t lo = 0, hi = 0;           // the block's extent in buffer bytes
    std::uint32_t copyLo = 0, copyHi = 0;   // the CPU copy's bytes to copy into it (empty = nothing)
};
RingBlock ringBlock(const BufferCopyState::Plan& plan, std::uint32_t curLo, std::uint32_t curHi, std::uint32_t bufferSize);

// What a D3D9 texture lock (LOCK_* flags) adds to the level's next upload. D3D9's managed / system-memory pools keep a level
// dirty as a whole from creation until its first upload, so the data written under a READONLY lock of a fresh level reaches
// the GPU: eCGfxImageData::OnRead fills the savegame thumbnails that way (docs/research/savegame-thumbnails.md). Once the GPU
// image holds data (uploaded, or written by a blit), READONLY adds nothing.
enum class LockDirty : std::uint8_t {
    None,        // nothing to upload
    Locked,      // the locked rect (null = the whole level)
    WholeLevel,  // the whole level, whatever the rect
};
LockDirty textureLockDirty(std::uint32_t flags, bool levelHoldsGpuData);

}  // namespace rvk::d3d9
