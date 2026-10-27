#pragma once

// The N5.0 op tap (docs/research/engine-frame/README.md 4, "N5.0 result"; setting native.frame_tap, Flag::Debug, default off).
// At every main-view eCFrustumDatabase::Execute (0x66e990) it copies the database's seven op lists, sorts the copies of the
// z / solid / alpha lists with the engine's own qsort and comparators (the very functions Execute calls: their call sites in
// Execute are decoded and checked at the first use) and resolves each op (context kind from its vtable, entity, mesh
// sub-mesh / material / shader) into a native::FrameDesc. While Execute runs, every render-context slot entry (the op
// pointer the engine passes) and every HAL draw of the main view go to a native::Attribution: the op sequence must equal the
// sorted copies byte for byte, every draw is mapped to its op. Read-only: nothing it does changes a draw.
// rvk.log: `native: frame tap: ...` every 5 s; with -LogFrame the frame's op lists (one line per op).

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "d3d9/device.h"
#include "engine/scene_hooks.h"
#include "native/frame_desc.h"

namespace rvk::native {

class FrameTap final : public d3d9::DrawTap, public engine::scene::OpTap {
public:
    explicit FrameTap(d3d9::Device& device);
    ~FrameTap() override;
    FrameTap(const FrameTap&) = delete;
    FrameTap& operator=(const FrameTap&) = delete;

    void apply(bool on);
    bool enabled() const { return on_; }
    // the report (HalDevice, every 5 s): the last main view + the window since the last report
    void report();
    // the last main view's ops (valid until the next main view's Execute)
    const FrameDesc& last() const { return desc_; }

    // engine::scene::OpTap
    void executeBegin(void* db, void* desc) override;
    void executeEnd(void* db) override;
    void opEnter(const void* op) override;
    // d3d9::DrawTap
    void onDraw(const d3d9::DrawTapInfo& draw) override;

private:
    using CompareFn = int (*)(const void*, const void*);
    using QsortFn = void (*)(void*, std::size_t, std::size_t, CompareFn);
    struct VtableInfo {
        OpKind kind = OpKind::Unknown;
        std::array<signed char, 4> emptySlot{-1, -1, -1, -1};  // -1 = slot not prepared, 0 / 1 = the engine's function is empty
    };

    bool verifySortSites();
    VtableInfo& vtableInfo(const void* vtable);
    void resolve(Op& op);
    void logOps() const;

    d3d9::Device& dev_;
    bool on_ = false;
    int verified_ = 0;  // 0 not yet, 1 the sort sites are as expected, -1 not (the tap stays idle)
    QsortFn qsort_ = nullptr;
    std::array<CompareFn, kListCount> compare_{};
    const std::uint8_t* engineBase_ = nullptr;

    bool executing_ = false;
    void* db_ = nullptr;
    std::uint32_t nested_ = 0;
    std::uint64_t viewFrame_ = 0;
    std::uint32_t viewsInFrame_ = 0;
    FrameDesc desc_;
    Attribution attr_;
    std::vector<RawOp> raw_;
    std::unordered_map<const void*, VtableInfo> vtables_;

    TapCounts last_, window_;
    std::uint64_t lastFrame_ = 0;
    std::vector<std::string> windowMismatches_;  // the first view of the window with mismatches
};

}  // namespace rvk::native
