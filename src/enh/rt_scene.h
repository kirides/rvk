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
// * Materials (M9 P2, enh/rt_material.h): each draw's material is resolved (cached per BLAS by the draw's material
//   signature) and each instance keeps the best-ranked one (the main view's opaque solid draw > its first alpha-over
//   blended solid layer > its z pre-pass > the cascades); overlays (caustics, decals, wet specular layers) are never a base
//   and instances known only through one are left out; merged ranges (the cascades draw a level mesh in one range, the
//   main view per material) get per-triangle materials from the sub-ranges inside them (segments). Records carry the
//   albedo's bindless slot of the frame, sampler, uv map, opacity and alpha test.
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
#include "enh/rt_material.h"
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
    // Characters (M10 P3, docs/research/rtgi.md 6): the main view's skinned solid draws join the scene every frame while a
    // consumer asks for them (and the GPU lets vertex shaders write buffers): their vertex shader's capture variant
    // (d3d9::VariantSlot::Capture) writes the world positions + TEXCOORD0 into the frame's ring, the pass builds / refits one
    // BLAS per draw from that (PREFER_FAST_BUILD | ALLOW_UPDATE) and adds it with an identity transform under the instance
    // mask kRtMaskDynamic (rays can leave characters out: RVK_RT_TRACE_NO_DYNAMIC); records have no normals (geometric)
    void setDynamic(const void* consumer, bool on);
    bool dynamicSupported() const;
    // the GLSL options of a vertex shader's capture variant (d3d9::VariantSlot::Capture); public for rvk_shadertool --capture
    static bool captureOptions(const d3d9::Shader& s, shader::GlslOptions& o);

    // d3d9::DrawObserver
    bool observes(d3d9::draws::Kind kind, d3d9::scene::Phase phase) const override;
    void observe(d3d9::Device& device, const d3d9::ReplaceQuery& query) override;
    void bufferChanged(std::uint64_t bufferId) override;
    void textureReleased(const d3d9::Texture* texture) override;

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
    // the BLAS key of a draw (its buffers' ids + generations, the range, a strip flag)
    static rt::BlasKey keyOf(const d3d9::ReplaceQuery& q);
    struct EngineSub {
        std::uint32_t firstPrimitive = 0, primitives = 0;
        const void* material = nullptr;
    };
    static bool engineSubMeshes(const d3d9::ReplaceQuery& q, std::vector<EngineSub>& out);

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
        // a triangle strip (SpeedTree branches / fronds): its indices converted to a list at build time (degenerate
        // triangles dropped), in a buffer of its own that the BLAS and the records read; triangles = the list's count
        vk::rt::AddressBuffer listIndices;
        std::vector<std::uint8_t> listData;  // the converted indices until the build copies them (CPU, prepareInputs)
        std::uint32_t triangles = 0;
        std::uint32_t refs = 0;      // registry instances
        std::uint32_t serial = 0;    // build / compaction generation (stale compaction queries)
        bool compacted = false;
        std::uint64_t lastUsedFrame = 0;
        double lastUsedTime = 0.0;
        std::uint64_t opaqueMainFrame = 0;  // the main view's last opaque draw of it (blended-only ranges stay out)
        std::uint32_t kind = 0;      // draws::Kind of the first draw (records)
        // the materials its draws showed (enh/rt_material ids, M9 P2), by the draw's material signature: one range may be
        // drawn with several materials (instanced variants, terrain layers); each instance picks its own (Instance::material),
        // records fall back to the best one here. Resolved again when a texture died or every kMaterialRefresh frames
        // (animated uv maps); at most kMaterialSlots, least recently used out.
        struct MaterialSlot {
            std::uint64_t sig = 0;
            std::uint32_t id = 0;
            std::uint64_t resolved = 0, used = 0;  // frames
            std::uint8_t rank = 0;                 // the best rank of the draws that used it
            bool overlay = false;                  // alpha-blended with a texture opacity (caustics, decals)
        };
        std::vector<MaterialSlot> materials;
        // a range without a main-view material (the cascades' merged range of a level mesh): the BLASes of the main view's
        // sub-ranges inside it (same buffers and base vertex) give its triangles their materials (RtMaterialSegment)
        struct Segment {
            std::uint32_t firstPrimitive = 0, primitiveCount = 0;
            std::uint64_t blas = 0;         // a sub-range BLAS (its best material) ...
            std::uint32_t material = 0;     // ... or a blended-only layer range's material (layers_)
        };
        std::vector<Segment> segments;
        std::uint64_t segmentsFrame = 0;
        // depth-only draws (z pre-pass / cascades: eCMeshResource2::RenderZPass, no material in the tag): the engine's
        // sub-meshes of the range: one = its material (engineSingle), several = per-triangle materials (engineSegments,
        // material ids with a reference each), checked again every 4 x kMaterialRefresh frames or when one died
        std::vector<Segment> engineSegments;
        const void* engineSingle = nullptr;
        std::uint64_t engineChecked = 0;
        bool engineDead = false;
        const void* material = nullptr;  // the first draw's engine objects (diagnostics; may be stale)
        const void* entity = nullptr;
        std::uint32_t opDesc = 0;
    };
    // what the TLAS of a frame held (diagnostics: describe)
    struct Entry {
        std::uint64_t blas = 0;
        std::uint32_t instanceId = 0, flags = 0, material = 0;
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
    // the material of the draw `q` for the BLAS (0 = none / not eligible) and the draw's rank: 4 the main view's opaque
    // solid draw, 3 its alpha-over blended solid layers (the terrain), 2 its z pre-pass, 1 the cascades
    std::uint32_t materialOf(d3d9::Device& dev, const d3d9::ReplaceQuery& q, Blas& b, std::uint8_t& rank, bool& overlay);
    // an instance takes it: the best rank wins, the first eligible draw per frame and rank (layers keep their base)
    static void applyMaterial(rt::Registry::Instance& in, std::uint32_t id, std::uint8_t rank, bool overlay, std::uint64_t frame);
    const RtMaterial* bestMaterial(const Blas& b) const;
    void refreshSegments(Blas& b);
    void learnEngineMaterials(d3d9::Device& dev, const d3d9::ReplaceQuery& q, Blas& b, d3d9::draws::Tag& tagOut, bool& single);
    void learnLayer(d3d9::Device& dev, const d3d9::ReplaceQuery& q, const rt::BlasKey& key);
    // the record / segment material fields of `m` (bindless slots of this frame); returns whether the opacity has a texture
    struct MaterialFields {
        std::uint32_t albedoTex = 0, albedoSampler = 0, uvSet = 0, opacityTex = 0;
        float uvMap[2][4]{};
    };
    static MaterialFields materialFields(const RtMaterial* m);
    static constexpr std::size_t kMaterialSlots = 8;
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
    // characters (setDynamic)
    void observeSkinned(d3d9::Device& dev, const d3d9::ReplaceQuery& q);
    void buildDynamic(VkCommandBuffer cmd, std::uint64_t frame);
    void dropDynamic(bool all, std::uint64_t frame);
    struct DynDraw {
        std::uint64_t key = 0;
        VkDeviceAddress capture = 0;   // the ring allocation: uvec4 header (slots) + 2 x vec4 per vertex index
        VkDeviceAddress indices = 0;   // the draw's first index
        std::uint32_t slots = 0;       // highest index + 1
        std::uint32_t triangles = 0;
        bool index32 = false;
        bool built = false;            // its BLAS was built / refit this frame
    };
    struct DynBlas {
        vk::rt::AccelerationStructure as;
        std::uint32_t slots = 0, triangles = 0;
        VkDeviceAddress indices = 0;
        std::uint64_t lastFrame = 0;
        std::uint32_t updates = 0;     // refits since the last full build
        bool built = false;
        std::uint32_t material = 0;    // RtMaterials id (a reference held)
        std::uint64_t sig = 0, resolved = 0;
        std::uint32_t id = 0;          // instance id (records, debug colours)
        std::uint32_t maxIndex = 0;    // scanned once from the index buffer's CPU copy
        bool scanned = false;
    };
    std::unordered_set<const void*> dynamicConsumers_;
    bool dynamic_ = false;
    bool captureRegistered_ = false;
    std::vector<DynDraw> dyn_;  // this frame's captures
    std::uint64_t dynFrame_ = 0;
    std::unordered_map<std::uint64_t, DynBlas> dynBlas_;
    vk::rt::AddressBuffer dynScratch_;
    std::uint32_t nextDynId_ = 1u << 30;
    std::uint64_t dynBuilt_ = 0, dynRefit_ = 0;  // interval counters

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
    RtMaterials materials_;
    // not part of the scene: water (it reflects, it is not reflected) and alpha-blended meshes: materials and exact draw
    // ranges seen in the main view's refracted / water / blended draws; the sun cascades draw the same meshes untagged and
    // unblended
    std::unordered_set<const void*> waterMaterials_;
    std::unordered_set<rt::BlasKey, rt::BlasKeyHash> waterKeys_;
    std::unordered_map<rt::BlasKey, std::uint64_t, rt::BlasKeyHash> blendedKeys_;  // -> the frame the main view last drew it blended
    // ranges the main view draws only as blended solid layers (no BLAS): the first layer's material (a reference held), a
    // segment source for the merged ranges covering them
    struct Layer {
        std::uint32_t material = 0;
        std::uint64_t sig = 0, seen = 0, resolved = 0;
    };
    std::unordered_map<rt::BlasKey, Layer, rt::BlasKeyHash> layers_;

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
        std::uint64_t textured = 0, untextured = 0, cutOut = 0, segmented = 0, segments = 0, speedTree = 0;  // records of the last frame
        std::uint64_t dynamic = 0;  // characters in the last frame's TLAS
        std::uint64_t engineSegments = 0, engineMerged = 0;
        std::uint64_t gpuSamples = 0;
    } iv_;
    std::deque<std::pair<std::uint64_t, std::vector<Entry>>> contents_;  // frame value -> the TLAS's instances (last 4)
    std::uint32_t lastInstances_ = 0;
    std::uint64_t lastTriangles_ = 0;
    std::chrono::steady_clock::time_point lastLog_{};
    std::unordered_set<const void*> declLogged_;
    std::unordered_map<std::uint64_t, const char*> vsVerdict_;  // VS hash (^ instanced) -> why its draws are not taken (null = taken)
    std::unordered_set<std::uint64_t> blendLogged_;
    std::unordered_set<const char*> speedTreeLogged_;  // reasons (string literals)
    std::unordered_set<std::uint32_t> reasonsLogged_;
    bool instanceIndexChecked_ = false;
    std::uint64_t indexDeviations_ = 0;
};

}  // namespace rvk::enh
