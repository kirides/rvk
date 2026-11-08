#pragma once

// Lighting register images (docs/research/frame-ownership.md 7.9, F1b): one entry of 24 float4 = the lighting registers c14 .. c37 per distinct
// (illumination slot, compose parameters) that StaticOps::writeLighting composed, kept in one persistent device buffer and composed ON THE GPU every
// frame from the illumination table's records (shaders/gpuscene/lighting_compose.comp.hlsl -> rvkIllumCompose, the twin of gpuscene::composeLighting).
// F1a points PS window 3 at an entry: the window base is `entry address - 14 x 16` (`windowBase`), so the shaders' `RVK_PSC(n)` (c[n] = base + n x 16)
// reads the entry's register n - 14.
//
// Layout of the images buffer: [0, kLightingPad) pad (no entry; D3D12's (heap slot, byte offset) references need offset >= 14 x 16 = 224 to form the window
// base, 4 KiB covers every window), then entries at kLightingPad + k x kLightingStride, kLightingStride = 384 B (24 x float4, 16-byte aligned).
// c14.w is not a register writeLighting writes (it keeps whatever the pool held): the image holds 0 there, the checks leave it out.
//
// Ordering (7.9): the pass is recorded into the UPLOAD command list at present, after the illumination / light-set tables' own flush of that present
// (same list, memory barrier between). The upload list runs before ALL of the frame's draws, so the records read are the end-of-CPU-frame state of THIS
// frame (not the previous frame's), and every entry first seen this frame is composed in this frame too. A draw that was composed earlier in the CPU frame
// than a record's last change differs from the image; the read-back check counts that (vs drawn registers).
//
// Setting `native.lighting_images` (Debug, restart, default off; Full; needs `native.illum_table`).

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "gpuscene/illumination_table.h"

namespace rvk::gpuscene {

inline constexpr std::uint32_t kLightingFirstReg = 14;
inline constexpr std::uint32_t kLightingRegs = 24;
inline constexpr std::uint32_t kLightingStride = kLightingRegs * 16;  // 384
inline constexpr std::uint32_t kLightingPad = 4096;
static_assert(kLightingPad >= kLightingFirstReg * 16 && kLightingPad % 16 == 0 && kLightingStride % 16 == 0);

// GPU: 16 bytes per entry slot (shaders/gpuscene/lighting_compose.comp.hlsl)
struct LightingJob {
    std::uint32_t illum;  // illumination slot, 0xffffffff = free entry
    std::uint32_t flags;  // bit 0 tableLit, bit 1 holdsAll
    std::uint32_t n;
    std::uint32_t firstLight;
};
static_assert(sizeof(LightingJob) == 16);

inline constexpr std::uint32_t kNoEntry = 0xffffffffu;

// the distinct-entry key: the illumination slot and every parameter composeLighting takes besides the records
inline std::uint64_t lightingKey(std::uint32_t illumSlot, const ComposeParams& p) {
    return static_cast<std::uint64_t>(illumSlot) << 32 | static_cast<std::uint64_t>(p.firstLight & 0xffffu) << 8 | static_cast<std::uint64_t>(p.n & 7u) << 2 |
           (p.holdsAll ? 2u : 0u) | (p.tableLit ? 1u : 0u);
}
inline LightingJob lightingJob(std::uint32_t illumSlot, const ComposeParams& p) {
    return {illumSlot, (p.tableLit ? 1u : 0u) | (p.holdsAll ? 2u : 0u), p.n, p.firstLight};
}

struct LightingStats {
    std::uint64_t entriesNew = 0, entriesFreed = 0, overflow = 0, noted = 0;
    std::uint64_t passes = 0, composed = 0, passSkipped = 0;
    std::uint64_t checks = 0, entriesChecked = 0, uncomparable = 0, cpuDiff = 0, drawnDiff = 0, drawnCompared = 0;
    std::uint32_t maxUlpCpu = 0, maxUlpDrawn = 0;
    std::uint64_t denormalOnly = 0, noRecord = 0, noSet = 0, drawnUncomparable = 0;  // words whose only difference is two denormals (GPU flush to zero)
};

class LightingImages {
public:
    static constexpr std::uint32_t kCapacity = 8192;  // entries (3 MiB)
    static constexpr std::uint64_t kIdleFrames = 600;
    static constexpr std::uint64_t kVerifyEvery = 60;

    LightingImages();
    ~LightingImages();

    void setActive(bool on) { active_ = on; }
    bool active() const { return active_; }

    // StaticOps::writeLighting after the registers were written: finds / creates the entry of (illumination slot, params) and keeps `drawn` (c14 .. c37 as
    // drawn) for the check. Returns the entry (kNoEntry: table full).
    std::uint32_t note(std::uint32_t illumSlot, const ComposeParams& p, const float drawn[24][4]);

    // F2c-1: the same entry (created / kept alive like note's) without registers to check: for draws derived from records. kNoEntry: inactive / table full
    std::uint32_t entryFor(std::uint32_t illumSlot, const ComposeParams& p);

    // F-VAR: the CPU image (c14 .. c37) of entry `e`, composed from the illumination table's records now (an observer of an owned draw that reads the lighting registers);
    // false: not live / no record. Cached per (entry, frame).
    bool cpuImage(std::uint32_t e, float out[24][4]);

    // once per present, after illumtable().flush (engine thread): entry uploads, idle sweep, the compose pass, the read-back check, the 5 s log
    void flush(d3d9::Device& dev, bool verify);

    // shader references of entry `e` (read form): its first register c14 / the window base for PS window 3 (c14 at offset 14 x 16: `RVK_PSC(n)` reads base + n x 16)
    rhi::GpuRef entryRef(std::uint32_t e) const { return imagesRef_.plus(static_cast<std::int64_t>(kLightingPad) + static_cast<std::int64_t>(e) * kLightingStride); }
    rhi::GpuRef windowBase(std::uint32_t e) const { return entryRef(e).plus(-static_cast<std::int64_t>(kLightingFirstReg) * 16); }
    bool ready() const { return !imagesRef_.null() && pipelineReady_; }

    const LightingStats& stats() const { return stats_; }
    std::uint32_t liveEntries() const { return live_; }

private:
    struct Entry {
        std::uint64_t key = 0, seen = 0, drawnFrame = ~0ull;
        std::uint32_t illum = kNoEntry;
        ComposeParams p;
        float drawn[24][4];
        bool live = false;
    };
    struct Pipeline;
    struct Pending {
        std::unique_ptr<rhi::Buffer> buffer;
        std::uint64_t frameValue = 0;
        bool busy = false;
        struct Expect {
            std::uint32_t entry = 0;
            bool hasCpu = false, hasDrawn = false;
            float cpu[24][4];
            float drawn[24][4];
            bool written[24][4];  // registers the composition writes (the others are 0 in the image, stale in the register file)
        };
        std::vector<Expect> expect;
    };
    std::uint32_t entryForLocked(std::uint32_t illumSlot, const ComposeParams& p);  // (mutex_ held)
    void flushLocked(d3d9::Device& dev, bool verify);
    void startCheck(d3d9::Device& dev, std::uint32_t used);
    void collect(d3d9::Device& dev);
    void report();

    bool active_ = false;
    std::mutex mutex_;
    GpuTable<LightingJob> jobs_;
    std::unique_ptr<rhi::Buffer> images_;
    rhi::GpuRef imagesRef_;  // read form (shaderRef) of the buffer start
    bool pipelineReady_ = false;
    std::unique_ptr<Pipeline> pipeline_;
    std::unordered_map<std::uint64_t, std::uint32_t> byKey_;
    std::vector<Entry> entries_;
    std::vector<std::uint32_t> free_;
    std::uint32_t used_ = 0, live_ = 0, peak_ = 0;
    std::uint64_t frame_ = 0;
    Pending pending_[rhi::kFramesInFlight];
    LightingStats stats_;
    double lastReport_ = 0.0;
    std::uint64_t lastFrame_ = 0, lastComposed_ = 0, lastPasses_ = 0;
    int differingLogged_ = 0;
    bool gpuFailed_ = false;
};

LightingImages& lightingimages();

}  // namespace rvk::gpuscene
