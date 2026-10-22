#pragma once

// The ray tracing scene (M9 P1, docs/research/rt-reflections.md 3): one TLAS per frame over the engine's world geometry,
// for every RT consumer (the debug view rt.debug now; the water's reflections next). Active only while a consumer holds it
// (acquire / release) and only with the ray tracing tier (caps::Cap::RayQuery); inactive it is not registered anywhere
// and holds no GPU memory.
//
// * A d3d9::DrawObserver sees the opaque world draws of the main view's depth pre-pass / solid pass and of the sun cascades
//   (StaticMesh, InstancedMesh, SpeedTree branches + fronds in their rest pose): POSITION FLOAT3 at offset 0 of stream 0,
//   the engine's static VB / IB (their BLAS-input usage of the tier), world rows from the VS constants (g_vInstanceData:
//   c33..c35, or c[32 + 4 i + 1 ..] per instance of a batch, i from the TEXCOORD4 instance stream).
// * BLAS cache keyed by (VB id + generation, IB id + generation, range, base vertex, stride, index size), built
//   PREFER_FAST_TRACE | ALLOW_COMPACTION within a per-frame budget and compacted a frame-slot later; LRU eviction under a
//   memory budget; dropped at once when a source buffer gets a new generation or dies (DrawObserver::bufferChanged).
// * A persistent instance registry (enh/rt_registry.h): what the cascades and earlier frames saw stays in the TLAS.
// * At AfterOpaque (the first main view): builds, compaction copies, the instance + geometry-record arrays in the frame's
//   ring, the TLAS (one per frame slot, sized to the cap), sync2 barriers to fragment / compute ray queries, and the
//   FrameBlock's rtScene / rtInfo (Device::setRtScene). GLSL side: shaders/include/rvk_rt.glsl.
// * Stats every 5 s in rvk.log (`rt: scene: ...`), a status line for the overlay's performance panel, GPU time of the
//   pass with [debug] perf (`pass rt.scene at AfterOpaque` in -PerfFrame).
//
// Ported in spirit from GD3D11's D3D12 RT scene (E:\git\github\GD3D11\D3D11Engine\D3D12Engine\D3D12RayTracing.cpp, the user's
// own code): cache by source buffers, build budget, compaction, LRU eviction, a TLAS rebuilt every frame.

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "d3d9/device.h"
#include "enh/rt_registry.h"
#include "shader/abi.h"
#include "vk/raytracing.h"

namespace rvk::enh {

class RtScene final : public d3d9::DrawObserver {
public:
    explicit RtScene(d3d9::Device& device);
    ~RtScene() override;
    RtScene(const RtScene&) = delete;
    RtScene& operator=(const RtScene&) = delete;

    // the tier is on (Context::features().rayTracing.enabled)
    bool supported() const;
    // consumers (render thread, e.g. settings callbacks): the scene is active while at least one holds it
    void acquire(const void* consumer);
    void release(const void* consumer);
    bool active() const { return active_; }
    const rt::Limits& limits() const { return limits_; }
    // which views feed it (diagnostics, rt.scene_views): bit 1 main view, bit 2 sun cascades; instances of the other are dropped
    void setViews(std::uint8_t mask);
    // diagnostics: what record `index` of the TLAS built in frame `frame` (Device::frameValue) was (kept for 4 frames)
    std::string describe(std::uint64_t frame, std::uint32_t index) const;
    void keepContents(bool on) {
        keepContents_ = on;
        if (!on) contents_.clear();
    }

    // d3d9::DrawObserver
    bool observes(d3d9::draws::Kind kind, d3d9::scene::Phase phase) const override;
    void observe(d3d9::Device& device, const d3d9::ReplaceQuery& query) override;
    void bufferChanged(std::uint64_t bufferId) override;

private:
    // which vertex attributes the declaration of a draw has (record words: offset | D3DDECLTYPE << 16)
    struct Layout {
        bool positionOk = false;  // POSITION0 FLOAT3 at offset 0 of stream 0
        bool unsupported = false; // morph targets / screen-aligned billboards (camera-dependent) / no position
        std::uint32_t normal = shader::abi::kRtAttribAbsent;
        std::uint32_t texcoord0 = shader::abi::kRtAttribAbsent;
        std::uint32_t texcoord1 = shader::abi::kRtAttribAbsent;
        std::uint32_t texcoord6 = shader::abi::kRtAttribAbsent;
        std::uint32_t color0 = shader::abi::kRtAttribAbsent;
        int instanceStream = -1;  // TEXCOORD4 (FLOAT1): the instance index
        std::uint32_t instanceOffset = 0;
    };
    static Layout layoutOf(const d3d9::VertexDecl* decl);

    enum class BlasState : std::uint8_t { Pending, Built, Failed };
    struct Blas {
        std::uint64_t id = 0;
        rt::BlasKey key;
        d3d9::Buffer* vb = nullptr;  // valid while the entry lives (bufferChanged drops it first)
        d3d9::Buffer* ib = nullptr;
        VkDeviceAddress vbAddress = 0, ibAddress = 0;  // the buffers' starts (records add the offsets)
        Layout layout;               // merged over the draws that showed it (the shadow pass's declaration may be shorter)
        BlasState state = BlasState::Pending;
        vk::rt::AccelerationStructure as;
        rt::Aabb bounds;             // object space, from the CPU copy at build time
        std::uint32_t maxVertex = 0;
        std::uint32_t refs = 0;      // registry instances
        std::uint32_t serial = 0;    // build / compaction generation (stale compaction queries)
        bool compacted = false;
        std::uint64_t lastUsedFrame = 0;
        double lastUsedTime = 0.0;
        std::uint64_t opaqueMainFrame = 0;  // the main view's last opaque draw of it (blended-only ranges stay out)
        std::uint32_t kind = 0;      // draws::Kind of the first draw (records)
        const void* material = nullptr;  // the first draw's engine objects (diagnostics; may be stale)
        const void* entity = nullptr;
        std::uint32_t opDesc = 0;
    };
    // what the TLAS of a frame held (diagnostics: describe)
    struct Entry {
        std::uint64_t blas = 0;
        std::uint32_t instanceId = 0, flags = 0;
        float translation[3]{};
    };
    struct Compaction {
        std::uint64_t blas = 0;
        std::uint32_t serial = 0;
        std::uint32_t query = 0;
    };
    struct Tlas {
        vk::rt::AccelerationStructure as;
        std::uint32_t capacity = 0;
    };

    void activate();
    void deactivate();
    void record(d3d9::PassContext& ctx);
    bool ensureResources();
    void destroyResources();
    Blas* findBlas(std::uint64_t id);
    void dropBlas(std::uint64_t id, bool countEvicted);
    void learnExcluded(const d3d9::ReplaceQuery& q, bool water);  // water / alpha-blended meshes of the main view
    // CPU side of a build: index range scan (max vertex, bounds); false = unusable (logged once per reason)
    bool prepareInputs(Blas& b);
    void processCompactions(VkCommandBuffer cmd, std::uint32_t slot);
    void buildPending(VkCommandBuffer cmd, std::uint32_t slot);
    void evictOverBudget();
    void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                 VkAccessFlags2 dstAccess);
    bool ensureScratch(VkDeviceSize bytes);
    void logStats(bool force);
    std::uint8_t viewBit(d3d9::scene::View v) const;

    d3d9::Device& dev_;
    rt::Limits limits_;
    std::unordered_set<const void*> consumers_;
    bool active_ = false;
    bool failed_ = false;
    std::uint8_t views_ = 3;
    bool keepContents_ = false;
    std::uint32_t passId_ = 0;

    // BLAS cache
    std::unordered_map<rt::BlasKey, std::uint64_t, rt::BlasKeyHash> blasByKey_;
    std::unordered_map<std::uint64_t, Blas> blas_;
    std::unordered_multimap<std::uint64_t, std::uint64_t> blasByBuffer_;  // buffer id -> BLAS id
    std::deque<std::uint64_t> pending_;
    std::uint64_t nextBlasId_ = 1;
    std::uint64_t blasBytes_ = 0;
    rt::Registry registry_;
    // not part of the scene: water (it reflects, it is not reflected) and alpha-blended meshes: materials and exact draw
    // ranges seen in the main view's refracted / water / blended draws; the sun cascades draw the same meshes untagged and
    // unblended
    std::unordered_set<const void*> waterMaterials_;
    std::unordered_set<rt::BlasKey, rt::BlasKeyHash> waterKeys_;
    std::unordered_map<rt::BlasKey, std::uint64_t, rt::BlasKeyHash> blendedKeys_;  // -> the frame the main view last drew it blended

    // GPU resources (while active)
    vk::rt::AddressBuffer scratch_;
    VkDeviceSize scratchAlign_ = 256;
    Tlas tlas_[vk::Frames::kInFlight];
    VkDeviceSize tlasScratchSize_ = 0;
    VkQueryPool compactPool_ = VK_NULL_HANDLE;
    static constexpr std::uint32_t kQueriesPerSlot = 64;  // >= buildsPerFrame
    std::vector<Compaction> compactions_[vk::Frames::kInFlight];
    VkQueryPool timePool_ = VK_NULL_HANDLE;  // [debug] perf: begin / end of the pass per slot
    bool timeWritten_[vk::Frames::kInFlight]{};

    // stats (interval since the last log line)
    struct Interval {
        std::uint64_t frames = 0, built = 0, builtTriangles = 0, compacted = 0, evicted = 0, failed = 0, observed = 0, skipped = 0;
        std::uint64_t compactSaved = 0;
        double buildCpuMs = 0.0, passCpuMs = 0.0, observeMs = 0.0, gpuMs = 0.0;
        std::uint64_t gpuSamples = 0;
    } iv_;
    std::deque<std::pair<std::uint64_t, std::vector<Entry>>> contents_;  // frame value -> the TLAS's instances (last 4)
    std::uint32_t lastInstances_ = 0;
    std::uint64_t lastTriangles_ = 0;
    std::chrono::steady_clock::time_point lastLog_{};
    std::unordered_set<const void*> declLogged_;
    std::unordered_set<std::uint32_t> reasonsLogged_;
    bool instanceIndexChecked_ = false;
    std::uint64_t indexDeviations_ = 0;
};

}  // namespace rvk::enh
