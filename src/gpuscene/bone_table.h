#pragma once

// The bone table (docs/research/frame-ownership.md 7.41, F6-1; setting `native.owned_actors`, Debug, Full + native.owned_persistent): one persistent register image per
// actor part (wrapper x primitive x fade) holding what the skinned vertex shader reads from VS register window 2 (c31..c231): c31.xyz = the three morph weights,
// c32.w = the global alpha (the draw's fade), c33 + 3 i .. = the primitive's bone matrices i < bones as 3 rows each (the 3x4 transpose of the palette entry
// `remap[start + i]`, composed exactly like `StaticOps::writeBone`). The owned actor draws read window 2 from it instead of a per-draw ring block of 3.2 KiB.
//   * One slot per key, shared by every view of the frame (main z, main solid with fade 1, the cascades): the palette is the same in all of them (the engine rebuilds it
//     once per frame at the actor's first Render). `serve` writes the image into the CPU shadow and marks the slot; a slot an earlier draw of THIS frame was served
//     from can no longer change (the GPU sees one value per frame): another content = a conflict, the draw keeps its upload.
//   * Slots idle for kIdleFrames are released; a released slot is not handed out again for kFramesInFlight + 1 presents (frames in flight still read it).
//   * `flush` (once per present, FIRST in the table block of SwapScreen: 7.35) uploads the marked slots into the frame's upload list, a GPU read-back compares the
//     whole table every 240th frame (0 differing), the 5 s log `native: bone table (5 s)`.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "d3d9/owned_windows.h"
#include "gpuscene/illumination_table.h"
#include "gpuscene/window_compose.h"

namespace rvk::d3d9 {
class Device;
}

namespace rvk::gpuscene {

inline constexpr std::uint32_t kBoneFirstReg = 31;
inline constexpr std::uint32_t kBoneRegs = 201;  // c31 .. c231: the whole VS window 2
inline constexpr std::uint32_t kBoneMaxBones = 66;  // eCShaderDefault::GetMaxBoneMatrixCount (c33 .. c230)
static_assert(shader::abi::kVsWindowFirst[2] == kBoneFirstReg && shader::abi::kVsWindowFirst[3] - shader::abi::kVsWindowFirst[2] == kBoneRegs, "the record is VS window 2");

struct alignas(16) BoneRecord {
    float regs[kBoneRegs][4];  // [0] = c31, [1] = c32, [2 + 3 i + k] = c(33 + 3 i + k)
};
static_assert(sizeof(BoneRecord) == kBoneRegs * 16);

struct BoneKey {
    const void* wrapper = nullptr;  // the actor's eCWrapper_emfx3ActorInstance (palette owner)
    std::uint32_t prim = 0;         // the op's primitive index (desc >> 16)
    std::uint32_t fadeBits = 0;     // c32.w
    bool operator==(const BoneKey&) const = default;
};
struct BoneKeyHash {
    std::size_t operator()(const BoneKey& k) const noexcept {
        std::uint64_t h = (reinterpret_cast<std::uintptr_t>(k.wrapper) >> 3) * 0x9E3779B97F4A7C15ull;
        h ^= (static_cast<std::uint64_t>(k.prim) << 32 | k.fadeBits) * 0xC2B2AE3D27D4EB4Full;
        return static_cast<std::size_t>(h ^ (h >> 29));
    }
};

enum class BoneWhy : std::uint8_t { Served, NotReady, Full, Conflict, TooLong, Count };

struct BoneStats {
    std::uint64_t requests = 0, served = 0, unchanged = 0, rewrites = 0, newSlots = 0, released = 0;
    std::uint64_t why[static_cast<std::size_t>(BoneWhy::Count)] = {};
};

class BoneTable {
public:
    static constexpr std::uint32_t kCapacity = 1024;  // records (3.1 MiB); the last one is never handed out (a zero tail pad), so 1,023 slots
    static constexpr std::uint64_t kIdleFrames = 600;

    void setActive(bool on) { active_ = on; }
    bool active() const { return active_; }

    // The key's image := `regs` (`count` float4 from c31: c31, c32, then the bone rows; count <= kBoneRegs), the rest zero. Fills `out` (kind Bones, base, covered, CPU twin)
    // and returns Served, else the reason (out untouched). `verifyMask` of the window: c32 (.w only) and the bone registers inside the first 32.
    BoneWhy serve(const BoneKey& key, const float (*regs)[4], std::uint32_t count, d3d9::OwnedWindows::Win& out);
    // the window of the key's slot without writing anything (false: no slot / not ready)
    bool windowFor(const BoneKey& key, d3d9::OwnedWindows::Win& out) const;
    // the key's slot was served earlier in this frame: its window (the image is final for the frame)
    bool servedNow(const BoneKey& key, d3d9::OwnedWindows::Win& out) const;
    // the image of the key's slot (CPU twin), null without a slot
    const BoneRecord* image(const BoneKey& key) const;

    // once per present (engine thread, before the frame's draws are submitted): creates the buffer, releases idle slots, uploads the marked ones, read-back, 5 s log
    void flush(d3d9::Device& dev);

    bool ready() const { return table_.ready(); }
    void destroy();  // tests: the buffer and all slots go
    const TableStats& tableStats() const { return table_.stats(); }
    const BoneStats& stats() const { return stats_; }
    std::size_t liveSlots() const { return map_.size(); }
    std::uint64_t frame() const { return frame_; }

private:
    struct Entry {
        std::uint32_t slot = 0;
        std::uint64_t lastUse = 0;     // frame_ + 1 of the last serve / lookup
    };
    void window(std::uint32_t slot, d3d9::OwnedWindows::Win& out) const;
    void report(d3d9::Device& dev);

    bool active_ = false;
    GpuTable<BoneRecord> table_;
    bool created_ = false, gpuFailed_ = false;
    std::unordered_map<BoneKey, Entry, BoneKeyHash> map_;
    std::vector<std::uint32_t> free_;
    struct Pending {
        std::uint32_t slot;
        std::uint64_t at;  // frame_ at which the slot may be reused
    };
    std::vector<Pending> pending_;
    std::vector<std::uint64_t> servedFrame_;  // per slot: frame_ + 1 of the last serve
    std::vector<std::uint32_t> usedRegs_;     // per slot: the register count of its image (the registers behind are zero)
    std::uint32_t used_ = 0;
    std::uint32_t conflictLogged_ = 0;
    std::uint64_t frame_ = 0;
    BoneStats stats_, last_;
    std::uint64_t lastFrame_ = 0, lastBytes_ = 0, lastSlots_ = 0, lastDeviceBones_ = 0;
    double lastReport_ = 0.0;
};

BoneTable& bonetable();

}  // namespace rvk::gpuscene
