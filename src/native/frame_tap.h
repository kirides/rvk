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

    // native.frame_tap (its report); the tap also runs while the static ops (N5.1) use it
    void apply(bool on);
    bool enabled() const { return on_; }
    void setStaticOps(class StaticOps* ops) { statics_ = ops; }
    void setStaticOpsActive(bool on);
    // N5.5c: the effect draws (native/effect_ops) take the draws of their units first
    void setEffectOps(class EffectOps* ops) { effects_ = ops; }
    void setEffectOpsActive(bool on);
    // the report (HalDevice, every 5 s): the last main view + the window since the last report
    void report();
    // the last main view's ops (valid until the next main view's Execute)
    const FrameDesc& last() const { return desc_; }

    // the HAL's CopyCurrentRTToImage into the API texture `api` in the main view (the refraction copy)
    void refractionCopy(const void* api);

    // engine::scene::OpTap
    void executeBegin(void* db, void* desc) override;
    void executeEnd(void* db) override;
    // N5.5: a shadow map's ExecuteShadowMap (its z list, sorted by the same qsort / comparator as the main z pass)
    void shadowExecuteBegin(void* db, void* shadowMap) override;
    void shadowExecuteEnd(void* db) override { executeEnd(db); }
    void opEnter(const void* op) override;
    void nonOpBegin(NonOp which, void* admin, void* db) override;
    void nonOpEnd(NonOp which) override;
    // d3d9::DrawTap
    bool onDraw(const d3d9::DrawTapInfo& draw) override;

private:
    using CompareFn = int (*)(const void*, const void*);
    using QsortFn = void (*)(void*, std::size_t, std::size_t, CompareFn);
    struct VtableInfo {
        OpKind kind = OpKind::Unknown;
        std::array<signed char, 4> emptySlot{-1, -1, -1, -1};  // -1 = slot not prepared, 0 / 1 = the engine's function is empty
    };

    bool verifySortSites();
    bool verifyShadowSortSite();
    // a view's snapshot (main: the seven lists; shadow views: the z list) and the start of its attribution
    void beginView(void* db, void* desc, ViewKind kind, void* shadowMap, std::int32_t cascade);
    ViewKind shadowKind(const void* shadowMap, std::int32_t& cascade);
    VtableInfo& vtableInfo(const void* vtable);
    void resolve(Op& op);
    void logOps() const;

    void update();
    d3d9::Device& dev_;
    bool on_ = false;
    bool wanted_ = false;         // native.frame_tap
    bool staticsWanted_ = false;  // the static ops are active
    class StaticOps* statics_ = nullptr;
    bool effectsWanted_ = false;  // the effect draws are active
    class EffectOps* effects_ = nullptr;
    int verified_ = 0;  // 0 not yet, 1 the sort sites are as expected, -1 not (the tap stays idle)
    int shadowVerified_ = 0;  // ExecuteShadowMap's sort site (N5.5): 1 as expected, -1 not (shadow views stay untapped)
    std::array<const void*, kViewKindCount> kindVtables_{};  // the shadow maps' vtables (Engine.dll RVAs 0x9a29e0 / 0x9a2848 / 0x976738)
    std::array<const void*, 8> otherVtables_{};  // the other shadow maps' vtables, logged once each
    QsortFn qsort_ = nullptr;
    std::array<CompareFn, kListCount> compare_{};
    const std::uint8_t* engineBase_ = nullptr;
    void* (*vegAdmin_)() = nullptr;  // eCVegetationAdmin's getter (FUN_180265df0), resolved at the first vegetation op

    bool executing_ = false;
    void* db_ = nullptr;
    std::uint32_t nested_ = 0;
    std::uint64_t viewFrame_ = 0;
    std::array<std::uint32_t, kViewKindCount> viewsInFrame_{};
    FrameDesc desc_;
    Attribution attr_;
    std::vector<RawOp> raw_;
    std::unordered_map<const void*, VtableInfo> vtables_;

    // per view kind: the last view, the window since the last report, its first view with mismatches
    std::array<TapCounts, kViewKindCount> last_{}, window_{};
    std::array<std::uint64_t, kViewKindCount> lastFrame_{};
    std::array<std::vector<std::string>, kViewKindCount> windowMismatches_{};
    std::array<std::uint32_t, 4> cascadeViews_{};  // cascade views of the window by split index (0..2, 3 = other)
};

}  // namespace rvk::native
