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
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "d3d9/device.h"
#include "enh/material_pack.h"
#include "enh/rt_material.h"
#include "enh/rt_lod.h"
#include "gpuscene/verify.h"
#include "enh/rt_registry.h"
#include "rhi/rhi.h"
#include "shader/abi.h"

namespace rvk::enh {

// F4-5 (native.tree_wind): for the stages line of the native frame (native/static_ops_stages.cpp): the compute is armed (mode on + pipeline ready) and how many
// tree instances it evaluated so far (the process total; the caller takes the window's difference)
struct TreeWindState {
    bool armed = false;
    bool requested = false;  // (S-9) a consumer asked for the compute (its pipeline build started); false = no ray-traced consumer of the swaying trees: idle, not off
    std::uint64_t jobs = 0, owned = 0;
};
TreeWindState treeWindState();
// F8-1 (native.rt_records): for the stages line: armed (setting on, scene active) and the owned draws whose sightings were replayed from their record script
struct RtRecordsState {
    bool armed = false;
    std::uint64_t replayed = 0;
};
RtRecordsState rtRecordsState();

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
    // rt.reflection_tweaks bit 64 (A/B only): hardware batches take the bytecode fallback material again (the pre-fix behaviour, frame-ownership.md 14.19.1)
    void setBatchHeuristic(bool on) { batchHeuristic_ = on; }
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
    // Animated SpeedTrees (rt-shadows.md 6): with characters on, the SpeedTree branches / fronds within `rangeUnits` of the camera
    // are captured like characters (their vertex shader writes the wind-animated world positions: the draw of the main
    // view's solid pass) and their BLAS is refitted from that every frame; the instance of the rest pose is left out of the
    // TLAS while the animated one stands in for it. Needs setDynamic.
    void setAnimated(const void* consumer, bool on, float rangeUnits);
    bool dynamicSupported() const;
    // Characters skinned by a compute pass (rt-shadows.md 10): the draw's own vertex buffer + the bone matrices of its vertex shader
    // constants (copied at the draw: the first skinned draw of a frame, in the z pre-pass or a sun cascade, has this frame's pose)
    // give the world positions before the TLAS is built, no vertex shader capture and no cascade redraw needed. `verify` also
    // runs the capture variant on the cascades' draws and compares both (rvk.log `rt: skin verify`). The engine's draws are untouched.
    void setGpuSkinning(bool on, bool verify);
    bool gpuSkinning() const { return skinWanted_ && skinReady_; }
    // F4-5 (docs/research/frame-ownership.md 14.17; native.tree_wind, Full): the animated SpeedTrees' wind / sway by a compute pass instead of the vertex
    // shader capture: per tree instance the position part of the SpeedTree vertex shader (rt_tree_wind.comp.hlsl, enh/tree_wind.h) runs over the draw's own
    // vertex buffer with the constants of its observation (wind matrices c14..c29, the instance's row c32..c35) into a capture-layout buffer, at AfterOpaque
    // of the frame the draw was observed in (= when the capture would have run), so the next frame's refit takes it as it took the capture. Needs no
    // composed draw: the owned tree runs (Device::ownedTreeObserve) feed it as well, so ownedTreesOk() holds with animated trees. Mode 2 (compare) also takes
    // the capture of the composed draws (not of owned runs) and compares both on the GPU (rvk.log `rt: tree wind (5 s)`: max error in world units).
    void setTreeWind(int mode);
    int treeWind() const { return treeWind_; }
    bool treeWindActive() const { return treeWind_ != 0 && treeWindReady_; }
    // the scene captures skinned draws right now (enh/object_motion leaves them to it)
    bool capturingCharacters() const { return active_ && dynamic_; }
    // F-VAR2 (frame-ownership.md 7.45): the solid pass's capture of a character is only needed for the motion chain (enh/object_motion reads its previous positions) and as the
    // fallback of a character without a skinning job; with the skinning pass on and no motion consumer a character that has a job this frame asks for no capture, so its solid
    // draw can stay an owned draw
    void setCharacterMotion(bool on) { characterMotion_ = on; }
    // Level of detail (docs/research/rt-lod.md; rt.lod_range / rt.lod_bias): beyond `rangeMetres` from the camera an instance of
    // a static mesh / SpeedTree branch BLAS is traced through a proxy of the same vertices with fewer triangles (index list from
    // meshoptimizer's simplifier on a worker thread, own BLAS; 1/2, 1/4, 1/8 of the triangles for bias 1..3). range 0 or bias 0
    // = off (every proxy is dropped). Nothing changes inside the range; alpha-tested and per-triangle-material instances keep
    // their BLAS.
    // Beyond `farRangeMetres` (0 = off) a static mesh / tree instance of bounded size (<= 20 m radius) is traced as the object's own
    // oriented bounding box (one shared 12-triangle BLAS scaled by the instance transform, trees thinned), and instances whose
    // bounds are tiny at that distance are left out.
    void setLod(float rangeMetres, int bias, float farRangeMetres, float scale = 1.0f);
    // rt.lod_scale multiplies both ranges (0 = the ladder is off), rt.memory_budget (0 = none): while the AS memory (BLAS + TLAS) is over
    // the limit the ladder's ranges shrink (x0.85 per 16 frames, down to 8 %; back up when it is under 80 %) even when the scale is 0
    // (base ranges then), and BLASes whose instances are all beyond rung 0 give up their detailed original (rebuilt when the camera
    // comes back). The AS memory the proxies hold counts: the ladder only saves where originals are released.
    void setMemoryBudget(std::uint32_t megabytes);
    // the GLSL options of a vertex shader's capture variant (d3d9::VariantSlot::Capture); public for rvk_shadertool --capture
    static bool captureOptions(const d3d9::Shader& s, shader::TranslateOptions& o);

    // F8-0 persistent entries (docs/research/frame-ownership.md 7.20; settings rt.table_entries / debug.rt_table_compare, Full only): the instance of a static,
    // non-instanced, opaque mesh draw is learned from its first full observation and then held by a table entry keyed by (entity, opDesc, draw range): while
    // its scene-table slot is live and unmoved the registry instance is pinned (never aged for being unseen) and the entry's draws skip observe()
    // (coversDraw). Every kTableRelearn-th frame per entry the draw is observed again (materials, uv maps); `compare` runs a check frame every
    // kTableCompare frames with the pre-filter off and diffs each observed draw against its entry.
    void setTableEntries(bool on, bool compare);
    bool tableEntries() const { return tableOn_; }
    // F8-1 (docs/research/frame-ownership.md 14.30; native.rt_records, Full, Debug): the RT scene from the owned record set. An owned persistent draw (static / instanced
    // mesh, any view; Device::ownedObsInfo or the bulk record view) is observed in full once; what that observation did to the scene (its sightings: instance key, rows,
    // owner, flags, data word, view; the BLAS, the material slot) is kept as the record's script, keyed by the persistent slot id. While the record is unchanged (retained draw,
    // derivation signature, env epoch, not rewritten), the rows its windows hold are byte-equal to the script's, the BLAS, its material slot (not due for a refresh) and the
    // engine material of a depth-only draw are what the script saw, the draw is not observed: the script's sightings are replayed (Registry::see with the same arguments, the
    // material applied as materialOf would), i.e. the registry and so the TLAS end in the same state. Every kRecordRelearn-th frame per record and on check frames
    // (every kRecordCheck-th) the draw is observed in full again; a check frame compares the fresh script with the stored one (DIFFERING must stay 0) and the TLAS's
    // world instances with the previous frame's when the camera did not move (rvk.log `rt: scene records (5 s)`).
    void setRecordReplay(bool on);
    bool recordReplay() const { return f81On_; }

    // d3d9::DrawObserver
    bool observes(d3d9::draws::Kind kind, d3d9::scene::Phase phase) const override;
    bool coversDraw(const d3d9::CoverQuery& draw) override;
    const char* observerName() const override { return "rt scene"; }
    Registers registersRead() const override { return Registers::Material; }  // (world rows: ReplaceQuery::objectRows; the uv map's material constants)
    void observe(d3d9::Device& device, const d3d9::ReplaceQuery& query) override;
    // F2c-4b (frame-ownership.md 7.23): with table entries on, the persistent owned static draws come once per pass; a record that was covered last time and whose retained
    // draw, signature and env epoch are unchanged, whose entry is alive and whose slot has not moved is skipped without a lookup (the F8-2 hand-over)
    bool bulkOwned() const override { return tableOn_ && active_; }
    void observeOwned(d3d9::Device& device, const d3d9::OwnedFrame& frame) override;
    // 14.44 (frame-ownership.md; native.ordered_bulk): the ordered owned draws (blended terrain layers, no-z-write draws, the alpha / refracted lists' meshes) come once per op;
    // a draw whose observation needs no register (the alpha list's blended meshes: only "this range is drawn blended" is learned; opaque cut-outs of the alpha list; water) is
    // done from the retained query alone, the rest (the solid list's layers: their BLAS material per instance; no-z-write opaque draws: the F8-1 scripts) takes the long way
    bool bulkOrdered() const override { return active_; }
    // F4-3 (frame-ownership.md 7.25b): the owned SpeedTree runs are shown to observe() through Device::ownedTreeObserve (rows in c32 + 4 i, one batch of <= 50 instances per call) as the
    // composed batches were; only the animated trees (their vertex capture needs the composed draw) keep them composed
    // (F4-5: with native.tree_wind the animated trees need no capture either: the compute pass takes them from the owned runs' observations)
    bool ownedTreesOk() const override { return !animated_ || treeWindActive(); }
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
    // a hardware-batched draw's material (eCMeshResource2::ExecuteRenderInstances draws without eCSubMesh::Render, so the tag has none): the op's sub-mesh
    static const void* batchMaterial(const d3d9::ReplaceQuery& q);

    enum class BlasState : std::uint8_t { Pending, Built, Failed };
    struct Blas {
        std::uint64_t id = 0;
        rt::BlasKey key;
        d3d9::Buffer* vb = nullptr;  // valid while the entry lives (bufferChanged drops it first)
        d3d9::Buffer* ib = nullptr;
        rhi::GpuAddr vbAddress, ibAddress;  // the buffers' starts (records add the offsets)
        Layout layout;               // merged over the draws that showed it (the shadow pass's declaration may be shorter)
        BlasState state = BlasState::Pending;
        std::unique_ptr<rhi::AccelerationStructure> as;
        rt::Aabb bounds;             // object space, from the CPU copy at build time
        std::uint32_t maxVertex = 0;
        // a triangle strip (SpeedTree branches / fronds): its indices converted to a list at build time (degenerate
        // triangles dropped), in a buffer of its own that the BLAS and the records read; triangles = the list's count
        std::unique_ptr<rhi::Buffer> listIndices;
        std::vector<std::uint8_t> listData;  // the converted indices until the build copies them (CPU, prepareInputs)
        std::uint32_t triangles = 0;
        std::uint32_t refs = 0;      // registry instances
        std::uint32_t serial = 0;    // build / compaction generation (stale compaction queries)
        bool compacted = false;
        bool released = false;       // the original AS was given up (rt.memory_budget): proxies / the box stand in
        bool rebuild = false;        // released and queued to be built again (state Pending): still in the TLAS through its proxy / box
        std::uint64_t lastUsedFrame = 0;
        double lastUsedTime = 0.0;
        std::uint64_t opaqueMainFrame = 0;  // the main view's last opaque draw of it (blended-only ranges stay out)
        std::uint32_t kind = 0;      // draws::Kind of the first draw (records)
        // churn (rt-shadows.md 5): a range whose instances are new almost every frame (grass blades fading by distance, rigid parts
        // of moving objects without an owner) leaves a trail of stale instances in the registry: while `volatileUntil` has
        // not passed, its instances count only while they are seen (this frame or the two before)
        bool animatedOnly = false;   // SpeedTree leaves: no rest pose, only the animated capture goes into the TLAS (observeTree)
        bool glass = false;          // a main-view blended draw of a pack glass material used this range (kRtRecordGlass)
        std::uint64_t churnStart = 0, volatileUntil = 0;
        std::uint32_t churnNew = 0;
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
        // the last materialOf answer of the BLAS: its range is sighted in several views and instances every frame with the same
        // signature, the repeats are answered from here (the slot's bookkeeping is idempotent within a frame)
        std::uint64_t memoFrame = 0, memoSig = 0;
        std::uint32_t memoId = 0;
        std::uint8_t memoRank = 0;
        bool memoBlending = false, memoOverlay = false;
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
        // the coarser stand-in beyond rt.lod_range: the same vertex buffer, an index list of its own (relative to the draw's base
        // vertex, as the original's), a BLAS of its own built without compaction
        struct Proxy {
            enum class State : std::uint8_t { None, Queued, Built, Failed } state = State::None;
            std::future<rt::lod::Result> job;
            std::unique_ptr<rhi::Buffer> indices;
            std::unique_ptr<rhi::AccelerationStructure> as;
            std::uint32_t triangles = 0;
            std::uint32_t level = 0;   // the bias it was made for
            std::uint64_t bytes = 0;   // AS storage + index buffer (counted in blasBytes_)
            double jobMs = 0.0;
            std::uint32_t serial = 0;  // the BLAS's serial when queued (a rebuilt original makes it stale)
        } proxy;
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
        std::unique_ptr<rhi::AccelerationStructure> as;
        std::uint32_t capacity = 0;
        std::uint32_t slot = 0;  // its slot in the bindless acceleration structure array (the backend's allocTlasSlot)
    };

    void activate();
    void deactivate();
    void record(d3d9::PassContext& ctx);
    bool ensureResources();
    void destroyResources();
    Blas* findBlas(std::uint64_t id);
    // the instance's BLAS without a lookup (Registry::Instance::user, set when it was added)
    Blas* blasOf(const rt::Registry::Instance& in) {
        if (in.user == nullptr) in.user = findBlas(in.blas);
        return static_cast<Blas*>(in.user);
    }
    void dropBlas(std::uint64_t id, bool countEvicted);
    void learnExcluded(const d3d9::ReplaceQuery& q, bool water);  // water / alpha-blended meshes of the main view
    // the material of the draw `q` for the BLAS (0 = none / not eligible) and the draw's rank: 4 the main view's opaque
    // solid draw, 3 its alpha-over blended solid layers (the terrain), 2 its z pre-pass, 1 the cascades
    // `noResolve` (14.44's fast layers): where the material would be resolved (it reads the draw's registers) nothing is changed, *noResolve = true and 0 is returned
    std::uint32_t materialOf(d3d9::Device& dev, const d3d9::ReplaceQuery& q, Blas& b, std::uint8_t& rank, bool& overlay, bool* noResolve = nullptr);
    // the solid pass's blended layer over a BLAS: its material for the draw's instances (observe; 14.44's fast path with `rowsFrom` = the owned record's window images instead of
    // the register file: false = it would need the registers (a row outside the windows, a material resolve), nothing changed). `keys` (optional) collects the instance keys
    // `apply` false = only the instance keys of the rows (verify: nothing changes)
    bool layerMaterial(d3d9::Device& dev, const d3d9::ReplaceQuery& q, Blas& b, const d3d9::OwnedWindows* rowsFrom, std::vector<std::uint64_t>* keys, bool apply = true);
    // an instance takes it: the best rank wins, the first eligible draw per frame and rank (layers keep their base)
    static void applyMaterial(rt::Registry::Instance& in, std::uint32_t id, std::uint8_t rank, bool overlay, std::uint64_t frame);
    const RtMaterial* bestMaterial(const Blas& b) const;
    void refreshSegments(Blas& b);
    void learnEngineMaterials(d3d9::Device& dev, const d3d9::ReplaceQuery& q, Blas& b, d3d9::draws::Tag& tagOut, bool& single);
    void learnLayer(d3d9::Device& dev, const d3d9::ReplaceQuery& q, const rt::BlasKey& key);
    // the record / segment material fields of `m` (bindless slots of this frame); returns whether the opacity has a texture
    struct MaterialFields {
        std::uint32_t albedoTex = 0, albedoSampler = 0, uvSet = 0, opacityTex = 0, packWord = 0;
        std::uint32_t packLod = 0;  // the record alpha's bits 16-23 (abi::kRtAlphaPackLodShift; 0 = no map)
        float uvMap[2][4]{};
    };
    MaterialFields materialFields(const RtMaterial* m) const;
    // record(): the frame's materialFields per material (a pure function of the material and its textures' state at that moment: one evaluation per material and
    // frame instead of one per instance; frame-ownership.md 14.30)
    std::unordered_map<const RtMaterial*, MaterialFields> fieldsMemo_;
    const MaterialFields& fieldsOf(const RtMaterial* m) {
        auto [it, fresh] = fieldsMemo_.try_emplace(m);
        if (fresh) it->second = materialFields(m);
        return it->second;
    }
    matpack::MaterialPack* pack_ = nullptr;
    // glass panes (materials.glass, docs/research/material-rt.md 8): the main view's blended draws of pack entries with transmission > 0 enter the scene
    // under their own mask (they are in no other ray's way)
    std::unordered_set<const void*> glassConsumers_;
    std::uint64_t glassInstances_ = 0;
    bool isGlassDraw(const d3d9::ReplaceQuery& q) const;
public:
    void setGlass(const void* consumer, bool on) {
        if (on) glassConsumers_.insert(consumer);
        else glassConsumers_.erase(consumer);
    }
    // glass instances in the TLAS of the latest build
    std::uint64_t glassInstances() const { return glassInstances_; }
    // the material pack (enh/material_pack.h): its entries' maps reach the hit shading through the records' packWord
    void setMaterialPack(matpack::MaterialPack* p) { pack_ = p; }
private:
    static constexpr std::size_t kMaterialSlots = 8;
    // CPU side of a build: index range scan (max vertex, bounds); false = unusable (logged once per reason)
    bool prepareInputs(Blas& b);
    void processCompactions(rhi::CommandList& cl, std::uint32_t slot);
    void buildPending(rhi::CommandList& cl, std::uint32_t slot);
    void evictOverBudget();
    std::vector<std::uint64_t> staleScratch_;
    // level of detail: proxies (queued from record() for instances beyond the range, built when their job is done)
    void queueProxy(Blas& b);
    void buildProxies(rhi::CommandList& cl);
    void dropProxy(Blas& b);
    bool lodEligible(const Blas& b) const;
    float lodRangeUnits_ = 0.0f, lodFarUnits_ = 0.0f;    // effective (base x scale x budget scale)
    float lodBaseRange_ = 0.0f, lodBaseFar_ = 0.0f, lodScale_ = 0.0f, budgetScale_ = 1.0f;
    std::uint64_t budgetBytes_ = 0;
    std::uint32_t released_ = 0;                          // BLASes without their original AS right now
    std::uint64_t releasedTotal_ = 0, rebuiltTotal_ = 0;
    std::unordered_set<std::uint64_t> nearBlas_;          // BLASes with an instance on rung 0 this frame
    void applyLodEffective();
    void releaseOriginals(std::uint64_t target);
    void requestRebuild(Blas& b);
    std::uint64_t asBytes() const;
    bool lodActive() const { return (lodRangeUnits_ > 0.0f && lodBias_ > 0) || lodFarUnits_ > 0.0f; }
    // the shared box of rung 2: 8 vertices + 36 u16 indices in one buffer, one BLAS (built once, in record)
    struct Box {
        std::unique_ptr<rhi::Buffer> buffer;
        std::unique_ptr<rhi::AccelerationStructure> as;
        bool built = false, failed = false;
    } box_;
    bool ensureBox(rhi::CommandList& cl);
    std::uint64_t lastSkippedTiny_ = 0;
    int lodBias_ = 0;
    std::vector<std::uint64_t> proxyJobs_;       // BLAS ids with a queued job
    std::uint64_t proxyBytes_ = 0;               // of blasBytes_
    std::uint32_t proxiesBuilt_ = 0;
    std::uint64_t proxyTrianglesOriginal_ = 0, proxyTrianglesKept_ = 0, proxyJobsDone_ = 0, proxyJobsFailed_ = 0;  // totals over the proxies built
    double proxyGatherMs_ = 0.0, proxyBuildMs_ = 0.0;
    std::uint64_t proxyGatherTriangles_ = 0;     // this frame's gathers (budget)
    std::uint64_t proxyGatherFrame_ = 0;
    // the snapshot of the last frame's TLAS by distance band and kind (taken on the frame a stats line is due)
    struct BandStat {
        std::uint64_t instances = 0, triangles = 0, trianglesOriginal = 0, proxied = 0, boxed = 0, blasBytes = 0, blases = 0;
        std::uint64_t byKind[3] = {0, 0, 0};  // triangles: meshes, SpeedTree, other
    };
    BandStat bands_[rt::lod::kBands];
    bool bandsValid_ = false;
    void noteNewInstance(Blas& b, std::uint64_t frame);
    void barrier(rhi::CommandList& cl, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                 VkAccessFlags2 dstAccess);
    void logStats(bool force);
    std::uint8_t viewBit(d3d9::scene::View v) const;
    // characters (setDynamic)
    void observeSkinned(d3d9::Device& dev, const d3d9::ReplaceQuery& q);
    struct TreeBatch {  // the capture buffer of one draw (a plain draw or an instanced batch of trees)
        rhi::GpuAddr base;
        rhi::GpuRef baseRw;  // (F4-5 compare: the capture's writable reference, D3D12)
        std::uint32_t slots = 0;
        bool failed = false;
    };
    void observeTree(d3d9::Device& dev, const d3d9::ReplaceQuery& q, Blas& b, const rt::Registry::Instance& in, const rt::Rows& rows, TreeBatch& batch,
                     std::uint32_t index, std::uint32_t count);
    void buildDynamic(rhi::CommandList& cl, std::uint64_t frame);
    void dropDynamic(bool all, std::uint64_t frame);
    struct DynDraw {
        std::uint64_t key = 0;
        rhi::GpuAddr capture;          // the ring allocation: uvec4 header (slots) + 2 x vec4 per vertex index
        rhi::GpuAddr indices;          // the draw's first index
        std::uint32_t slots = 0;       // highest index + 1
        std::uint32_t triangles = 0;
        bool index32 = false;
        bool built = false;            // its BLAS was built / refit this frame
        bool tree = false;             // an animated SpeedTree (key = the registry instance of its rest pose, blas = its BLAS)
        std::uint64_t blas = 0;
        rhi::GpuRef requested;        // the reference the draw's capture was requested with, when `capture` lies inside it (instanced batches)
        bool main = true;              // requested for the main view's solid pass (else for a sun cascade's view: this frame's pose before the main view)
        bool skin = false;             // written by the skinning compute pass (`capture` = its output; no vertex shader capture)
    };
    // characters skinned by compute (setGpuSkinning)
    struct SkinJob {
        rhi::GpuRef vertices;      // vertex 0 of the draw's index range's slot 0 (base vertex and stream offset applied)
        rhi::GpuRef bones;         // c32.. of the draw's vertex shader constants (200 vec4, copied into the ring)
        rhi::GpuRef out;           // the capture-layout buffer (the writable reference)
        rhi::GpuRef outKey;        // ... its read reference: the identity the consumers ask for (markVertexCaptured)
        rhi::GpuRef ref;           // verify: the vertex shader capture of the same draw
        std::uint32_t slots = 0, stride = 0, offsets = 0;
        // 14.15 c: the morph targets POSITION1..3 (bit k of morphMask: stream k + 1 is read; its vertex 0, stride, byte offset of the position, the weight c31[k])
        std::uint32_t morphMask = 0;
        rhi::GpuRef morph[3];
        std::uint32_t morphStride[3] = {0, 0, 0}, morphOff[3] = {0, 0, 0};
        float morphWeight[3] = {0.0f, 0.0f, 0.0f};
    };
    struct SkinLayout {
        std::uint32_t offsets = 0, morphMask = 0;
        std::uint32_t morphOff[3] = {0, 0, 0};
        rhi::GpuRef morph[3];
    };
    bool skinReady();
    void recordSkin(rhi::CommandList& cl);
    // F4-5 tree wind by compute (setTreeWind)
    struct TreeJob {
        rhi::GpuRef vertices;      // vertex slot 0 of the draw (stream offset + base vertex applied)
        rhi::GpuRef consts;        // 20 float4: c14..c29, c32..c35 of the instance (the ring)
        rhi::GpuRef out;           // the capture-layout buffer (the writable reference)
        rhi::GpuRef outKey;        // ... its read reference (markVertexCaptured)
        rhi::GpuRef ref;           // compare: the vertex shader capture of the same instance (its first vertex slot, after the header)
        std::uint32_t slots = 0, stride = 0, cls = 0;
        std::uint32_t words[8] = {};
    };
    bool treeWindReady();
    void recordTreeWind(d3d9::PassContext& ctx);
    // the compute's verdict on a draw's declaration (cached by declaration id x op class)
    struct TreeLayoutEntry {
        bool ok = false;
        std::uint32_t cls = 0;
        std::uint32_t words[8] = {};
    };
    const TreeLayoutEntry& treeLayout(const d3d9::ReplaceQuery& q);
    std::unordered_map<std::uint64_t, TreeLayoutEntry> treeLayouts_;
    std::vector<TreeJob> treeJobs_;
    std::uint64_t treeJobsEpoch_ = ~0ull;  // the memory epoch (GameDevice::memoryEpoch) of treeJobs_
    int treeWind_ = 0;
    bool treeWindReady_ = false, treeWindFailed_ = false, treeWindStarted_ = false;
    std::future<std::unique_ptr<rhi::Pipeline>> treeWindBuild_;
    std::unique_ptr<rhi::Pipeline> treeWindPipe_;
    std::unique_ptr<rhi::Buffer> treeWindErr_;  // compare: treewind::kErrBytes per frame slot
    std::uint32_t passId3_ = 0;
    struct TreeWindStats {
        std::uint64_t jobs = 0, vertices = 0, dispatches = 0, owned = 0, composed = 0, compareJobs = 0, refused = 0, ringFull = 0, dropped = 0;
        std::uint64_t compared = 0, missing = 0, over01 = 0, over1 = 0, windFromWindow = 0, windRegisterStale = 0, rampAssumed = 0, rampMismatch = 0;
        float clsMax[3] = {0.0f, 0.0f, 0.0f};
        std::uint64_t ownedSkip[6] = {};  // owned runs: BLAS not built / no camera / beyond the range / cap / seen / no indices
        float maxErr = 0.0f;
    } tw_;
    void destroyTreeWind();
    bool skinEligible(const d3d9::ReplaceQuery& q, const float* morphWeights, SkinLayout& l) const;
    void destroySkin();
    std::vector<SkinJob> skinJobs_;
    std::unordered_set<std::uint64_t> skinLogged_;
    bool characterMotion_ = false;
    bool skinWanted_ = false, skinVerify_ = false, skinReady_ = false, skinFailed_ = false, skinStarted_ = false;
    std::future<std::unique_ptr<rhi::Pipeline>> skinBuild_;  // (built on a worker: Device::createComputePipeline is thread-safe)
    std::unique_ptr<rhi::Pipeline> skinPipe_;
    std::unique_ptr<rhi::Buffer> skinErr_;    // verify: 16 bytes per frame slot (max error bits, compared, missing)
    double skinVerifyAt_ = 0.0, skinLogAt_ = 0.0;
    std::uint64_t skinVerifyCompared_ = 0, skinVerifyMissing_ = 0;
    float skinVerifyMax_ = 0.0f;
    std::uint64_t skinJobsTotal_ = 0, skinVerticesTotal_ = 0, skinMorphTotal_ = 0;
    struct DynBlas {
        std::unique_ptr<rhi::AccelerationStructure> as;
        std::uint32_t slots = 0, triangles = 0;
        rhi::GpuAddr indices;
        std::uint64_t lastFrame = 0;
        std::uint32_t updates = 0;     // refits since the last full build
        bool built = false;
        std::uint32_t material = 0;    // RtMaterials id (a reference held)
        std::uint64_t sig = 0, resolved = 0;
        std::uint32_t id = 0;          // instance id (records, debug colours)
        std::uint32_t maxIndex = 0;    // scanned once from the index buffer's CPU copy
        bool scanned = false;
        rhi::GpuRef lastCapture;  // the last capture that ran (the next frame's vertex shader reads it: motion)
        std::uint32_t lastSlots = 0;
        std::uint64_t lastCaptureEpoch = 0;  // its memory epoch (GameDevice::memoryEpoch: valid while memoryLive)
        // the counts the structure was created for (it is sized by them and the index type only: created again when they change)
        std::uint32_t sizeTriangles = 0, sizeSlots = 0;
        bool sizeIndex32 = false;
    };
    std::unordered_set<const void*> dynamicConsumers_;
    std::unordered_map<const void*, float> animConsumers_;  // consumer -> its range; the largest one counts
    bool animated_ = false;
    float animRange_ = 4500.0f;                        // units from the camera
    std::uint32_t dynTrees_ = 0;                       // this frame's tree captures
    std::unordered_set<std::uint64_t> animatedKeys_;   // rest-pose instances whose animated BLAS was built this frame (left out of the TLAS)
    bool dynamic_ = false;
    bool captureRegistered_ = false;
    std::vector<DynDraw> dyn_;      // this frame's capture requests (cascades' views first, then the main view's)
    std::vector<DynDraw> dynPrev_;  // the previous frame's
    std::vector<DynDraw> dynUse_;   // what buildDynamic took for this frame's TLAS (one per key, built = its BLAS is up to date)
    void rollDyn(std::uint64_t frame);
    // the main view's SpeedTree sightings of one frame (count + order-independent hash of the instance keys), per frame class: frames whose trees were owned / composed (a
    // still camera must give the same set in both: the check frames' compare of the owned hand-over)
    struct TreeSet {
        std::uint64_t frame = ~0ull, count = 0, hash = 0;
        bool owned = false;
    } treeSet_;
    std::unordered_map<std::uint64_t, std::uint64_t> treeSets_[2];  // [owned] set id -> frames
    void treeSetNote(const d3d9::Device& dev, const rt::Registry::Instance& in);
    void treeSetClose(const d3d9::Device& dev);
    std::uint64_t dynFrame_ = 0;
    std::uint64_t dynEpoch_ = 0;      // the memory epoch of dyn_ (frame-ownership.md 14.40: a mid-frame submit moves the frame value, not the epoch)
    std::uint64_t dynJumpDrops_ = 0;  // frames whose previous-frame captures a frame value jump dropped (rollDyn)
    std::unordered_map<std::uint64_t, DynBlas> dynBlas_;
    std::uint32_t nextDynId_ = 1u << 30;
    std::uint64_t dynBuilt_ = 0, dynRefit_ = 0;  // interval counters

    // ---- F8-1 record scripts (setRecordReplay) ----
    struct F81Sight {
        std::uint64_t key = 0, owner = 0;
        rt::Rows rows{};
        std::uint32_t reg = 0;  // the VS register of the first row (c33 plain, c[32 + 4 v + 1] of a batch)
        std::uint32_t flags = 0, data = 0;
        std::uint8_t view = 0;
    };
    struct F81Script {
        const void* cd = nullptr;
        std::uint64_t sig = 0, epoch = 0, matSig = 0, learned = 0, state = 0;
        const void* entity = nullptr;
        const void* tagMaterial = nullptr;
        const void* engineMaterial = nullptr;  // a depth-only / batch draw's material from the engine's sub-mesh (learnEngineMaterials)
        std::uint32_t opDesc = 0;
        std::uint64_t blas = 0;
        std::uint32_t material = 0, exclEpoch = 0;
        std::uint8_t rank = 0, salt = 0, cls = 0;
        bool mainOpaque = false, engineDepth = false, engineBatch = false, valid = false, reached = false, bad = false;
        std::vector<F81Sight> sights;
        // an instanced draw: the instance stream observe() read (TEXCOORD4 of the device's stream binding at the observation): replayed only while it reads the same
        bool instanced = false, fromStream = false;
        int stream = -1;
        std::uint32_t streamOffset = 0, n = 0;
        std::vector<float> index;
    };
    // observe()'s read of an instanced draw's TEXCOORD4 instance indices (false: not readable)
    bool readInstanceIndex(d3d9::Device& dev, int stream, std::uint32_t offset, std::uint32_t n, float* index) const;
    struct F81Key {
        bool valid = false;
        std::uint32_t id = 0;
        const void* cd = nullptr;
        std::uint64_t sig = 0, epoch = 0;
        bool changed = false;
        const d3d9::OwnedWindows* ow = nullptr;
        bool composed = false;  // a composed draw's packet replay (Device::replayDraw): keyed by its retained draw, rows from the register file
        const d3d9::ReplaceQuery* imm = nullptr;  // a composed draw issued now (no retained draw): keyed by its identity hash (cd = that hash)
    };
    F81Script& f81Script(const F81Key& k);
    const float* f81Rows(const F81Key& k, std::uint32_t reg) const;
    static std::uint64_t f81State(const d3d9::ReplaceQuery& q);
    std::unordered_map<const void*, F81Script> f81c_;  // composed packet replays' scripts, by retained draw
    F81Key f81Key(const d3d9::CoverQuery& d) const;
    bool f81Cover(const d3d9::CoverQuery& d);
    bool f81Replay(F81Script& s, const d3d9::CoverQuery& d, const F81Key& k, bool apply);
    void f81Finish(d3d9::Device& dev);
    bool f81Same(const F81Script& a, const F81Script& b, const char*& what) const;
    static const float* windowRows(const d3d9::OwnedWindows& ow, std::uint32_t reg);
    static const void* batchMaterialOf(const d3d9::draws::Tag* tag, bool instanced, std::uint32_t primitiveType, std::uint32_t first, std::uint32_t primitives);
    struct F81Digest {
        std::uint32_t id = 0;
        std::uint64_t hash = 0;
        std::uint64_t as = 0;
        std::uint32_t mask = 0, flags = 0, albedo = 0, alpha = 0;
        float t[3]{};
        bool operator<(const F81Digest& o) const { return id < o.id; }
    };
    void f81TlasDigest(std::vector<F81Digest>& digest, const float camera[3]);
    static constexpr std::uint64_t kRecordRelearn = 32;  // frames: a replayed record is observed again (<= half the materials' refresh period)
    static constexpr std::uint64_t kRecordCheck = 10;    // frames between check frames (every record observed in full, scripts compared)
    bool f81On_ = false;
    std::vector<F81Script> f81_;   // by persistent record id
    F81Script f81Scratch_;         // the script of the observation in progress
    F81Script* f81Rec_ = nullptr;  // observe(): recording into f81Scratch_
    F81Key f81Arm_;                // coversDraw armed the next observe() to record (the draw's record)
    bool f81ArmCompare_ = false;
    F81Key f81Cur_;                // observeOwned: the record view in hand (the bulk path has no Device::ownedObsInfo)
    std::uint32_t exclEpoch_ = 1;  // water materials / keys learned, views changed: every script is stale
    struct DigestSrc {
        VkAccelerationStructureInstanceKHR vi;
        shader::abi::RtGeometryRecord r;
        std::uint64_t blas = 0;
        std::uint8_t lod = 0;
    };
    std::vector<DigestSrc> digestSrc_;  // the digest frame's world instances as written
    std::vector<F81Digest> f81PrevDigest_;  // the previous frame's TLAS world instances (id, content hash), sorted
    bool f81TlasAlways_ = false;           // RVK_F81_TLAS=1: the TLAS compare also with the records off (the observe path's own frame-to-frame noise)
    float f81PrevCamera_[3]{};
    // scene events (registry instances added / removed, BLASes built / dropped, proxies built): a TLAS pair across one changed legitimately (either path)
    std::uint64_t sceneEvents_ = 0, f81PrevEvents_ = 0;
    std::uint64_t f81PrevFrame_ = 0;
    struct F81Stats {
        std::uint64_t asked[3]{}, replayed[3]{}, sights = 0;  // by class: main solid, main z, cascades
        std::uint64_t composedAsked = 0, composedReplayed = 0;  // of them: composed packet replays
        std::uint64_t fallNew = 0, fallChanged = 0, fallRelearn = 0, fallCheck = 0, fallInvalid = 0, fallRows = 0, fallBlas = 0, fallMaterial = 0, fallEngine = 0, fallBlended = 0;
        std::uint64_t learned = 0, unreplayable = 0, noMaterial = 0, noWindow = 0, glass = 0;
        std::uint64_t compared = 0, matched = 0, differing = 0;
        std::uint64_t tlasPairs = 0, tlasSkipped = 0, tlasEvents = 0, tlasInstances = 0, tlasDiffering = 0, tlasLogged = 0;
        double ms = 0.0;
    } f8s_;
    const char* f81FirstDiff_ = nullptr;
    void logRecords(double frames);
    // F8-1 for the owned SpeedTree runs (Device::ownedTreeObserve: one template draw shown with up to 50 instances' rows at a time): the batch-invariant part of
    // observe() (shader verdict, declaration, BLAS key + lookup, engine material, material slot, flags) is kept per template draw; a run whose template is unchanged
    // goes straight to the per-instance sightings (the same loop as observe's). Check frames / relearn frames take the full observe() and compare.
    struct TreeTpl {
        bool valid = false, leaf = false, animated = false;
        rt::BlasKey key;
        std::uint64_t blas = 0, matSig = 0;
        std::uint32_t material = 0, flags = 0, alphaWord = 0, exclEpoch = 0;
        std::uint8_t rank = 0, view = 0, salt = 0;
        const void* tagMaterial = nullptr;
    };
    std::unordered_map<const void*, TreeTpl> treeTpl_;
    TreeTpl treeTplScratch_;
    const void* treeTplArm_ = nullptr;  // observe(): learn the template of this draw
    bool treeTplCompare_ = false;
    bool treeRunFast(d3d9::Device& dev, const d3d9::ReplaceQuery& q);
    bool treeTplGuards(const TreeTpl& t, const d3d9::ReplaceQuery& q, const rt::BlasKey& key, Blas*& b, Blas::MaterialSlot*& slot);
    struct TreeTplStats {
        std::uint64_t runs = 0, fast = 0, learned = 0, refused = 0, compared = 0, matched = 0, differing = 0, sightings = 0;
    } tts_;

    // ---- F8-0 persistent entries ----
    struct TableEntry {
        const void* entity = nullptr;
        std::uint32_t opDesc = 0, first = 0, primitives = 0;
        std::uint32_t slot = 0, slotGen = 0, slotVersion = 0;  // the scene-table slot of the entity at learn time
        std::uint64_t slotStamp = 0;                           // scenetable::stamp(slot) then: differs when the slot was freed / reallocated / moved
        rt::BlasKey key;                                       // the draw's BLAS key (buffer ids + generations, range)
        std::uint64_t blas = 0, instKey = 0;                   // the BLAS id and the registry instance key (BLAS + quantised rows)
        rt::Rows rows{};
        std::uint32_t flags = 0, data = 0, material = 0;       // the instance's, as learned (the compare's snapshot)
        std::uint64_t matSig[3]{};                             // RtMaterials::signature of the draw (engine shaders + the sampled textures): a change = observe again
        std::uint8_t classMask = 0;                            // draw classes fully observed since the learn: 1 main solid, 2 main z pre-pass, 4 cascades
        std::uint8_t salt = 0;
        std::uint64_t comparedFrame = 0;
        std::uint32_t idx = 0;                                 // F2c-4b: the liveness slot (entryGen_[idx] changes when the entry dies)
    };
    // F2c-4b: per owned record (persistent slot id) what the last covered sighting found: a skip needs the same retained draw / signature / epoch / entity and a live entry
    struct OwnedCover {
        const void* cd = nullptr;
        const void* entity = nullptr;
        std::uint64_t sig = 0, epoch = 0;
        const TableEntry* entry = nullptr;      // (identity only: the checks below read the copies, not the entry's heap node)
        std::uint64_t slotStamp = 0;            // the entry's scenetable stamp, its slot and relearn salt
        std::uint32_t gen = 0, idx = 0, slot = 0;
        std::uint32_t opDesc = 0;
        std::uint8_t cls = 0, salt = 0;
    };
    std::vector<OwnedCover> ownedCover_;
    std::vector<std::uint32_t> entryGen_, entryFree_;
    const TableEntry* coveredEntry_ = nullptr;  // coversDraw's answer: the entry that covered the draw
    void tableDropInstance(std::uint64_t instKey);
    bool tableCover(const d3d9::CoverQuery& d, bool& entryDecides);  // F8-0's part of coversDraw
    struct BulkStats {
        std::uint64_t calls = 0, records = 0, entered = 0, left = 0, skipped = 0, slow = 0, covered = 0, observed = 0;
    } bs_;
    // 14.44: the ordered owned draws in bulk (observeOrdered). The outcome of observe() for such a draw, as the classifier predicts it from the retained query (kOrdFull = the long
    // way) and as the observation reached it (ordProbe_: verify frames run both and compare)
    enum OrdOutcome : int { kOrdNothing = 0, kOrdExclBlended = 1, kOrdExclWater = 2, kOrdFull = 3, kOrdLayer = 4, kOrdOpaque = 5, kOrdOutcomes = 6 };
    int ordClassify(const d3d9::ReplaceQuery& q);
    void observeOrdered(d3d9::Device& dev, const d3d9::OwnedFrame& f);
    void logOrdered(double frames);
    int* ordProbe_ = nullptr;
    std::vector<std::uint64_t>* ordKeys_ = nullptr;  // (verify: the instance keys the long way's layer took)
    std::vector<std::uint64_t> ordKeysA_, ordKeysB_;
    d3d9::ReplaceQuery ordQ_;
    rt::BlasKey ordKey_{};
    Blas* ordBlas_ = nullptr;
    struct OrdBulkStats {
        std::uint64_t calls = 0, records = 0, entered = 0, left = 0, fast[kOrdOutcomes] = {}, layerFallback = 0, full = 0, covered = 0, observed = 0, verified = 0, differing = 0;
        std::uint64_t by[kOrdOutcomes] = {};  // (the long way's outcomes)
        double ms = 0.0;
    } ob_;
    int ordDiffLogged_ = 0;
    std::uint64_t layerWhy_[2] = {};
    std::uint64_t matPath_[6] = {};  // (diagnostics: materialOf's paths: memo, slot, refresh due, no slot, slot material gone, dead)
    static std::uint8_t tableClass(d3d9::scene::Phase p);
    static std::uint64_t tableKey(const void* entity, std::uint32_t opDesc, std::uint32_t first, std::uint32_t primitives);
    void tableObserved(const d3d9::ReplaceQuery& q, const Blas& b, rt::Registry::Instance& in, const rt::Rows& rows, bool fresh,
                       const rt::Registry::Instance* pre);
    void tableMaintain(std::uint64_t frame);
    void tableErase(std::unordered_map<std::uint64_t, TableEntry, IdHash>::iterator it, bool unpin);
    void tableInstanceRemoved(std::uint64_t instKey);
    void tableClear();
    void logTable(double frames);  // (+ logOrdered)
    bool tableCompareFrame(std::uint64_t frame) const { return tableCompare_ && frame % kTableCompare == 0 && !gpuscene::checksOff(); }
    static constexpr std::uint64_t kTableRelearn = 32;  // frames: a covered draw is observed again (<= half of the materials' refresh period)
    static constexpr std::uint64_t kTableCompare = 10;  // frames between check frames
    bool tableOn_ = false, tableCompare_ = false, tableTripped_ = false, tableNoTrip_ = false;
    std::unordered_map<std::uint64_t, TableEntry, IdHash> table_;
    std::unordered_multimap<std::uint64_t, std::uint64_t> tableByInst_;      // registry instance key -> entry keys (z / solid / cascade entries of one range)
    std::unordered_multimap<std::uint64_t, std::uint64_t> tableByOp_;        // hash(entity, opDesc) -> entry key (level-of-detail switches)
    std::uint64_t tableCheckFrame_ = 0, tableExtraCounted_ = 0;
    std::unordered_map<std::uint64_t, std::uint32_t, IdHash> tableMatPre_;  // check frame: instance key -> its material before the frame's first sighting
    struct TableStats {
        std::uint64_t asked[3]{}, covered[3]{}, observed[3]{};  // by class (main solid, main z, cascades)
        std::uint64_t fallNone = 0, fallClass = 0, fallMoved = 0, fallIdent = 0, fallMaterial = 0, fallRelearn = 0, fallCompare = 0;
        std::uint64_t learned = 0, relearned = 0, refusedNoSlot = 0, refusedRows = 0, refusedShared = 0, lodErased = 0, slotDropped = 0, instGone = 0;
        std::uint64_t refusedAmbiguous = 0, ambiguousDropped = 0, ambiguousSkipped = 0;  // F2c-4b: instances two unlike draws gave different materials
        std::uint64_t checkFrames = 0, matched = 0, missing = 0, extra = 0, differing = 0, diffKey = 0, diffRows = 0, diffFlags = 0, diffData = 0, diffMaterial = 0, materialIds = 0;
        double coverMs = 0.0;
    } ts_;

    d3d9::Device& dev_;
    rt::Limits limits_;
    std::unordered_set<const void*> consumers_;
    bool active_ = false;
    bool failed_ = false;
    std::uint8_t views_ = 3;
    bool batchHeuristic_ = false;
    bool keepContents_ = false;
    std::uint32_t passId_ = 0, passId2_ = 0;
    std::uint64_t builtFrame_ = 0;  // the frame whose TLAS is built (once: at the z pre-pass, else at AfterOpaque)

    // BLAS cache
    std::unordered_map<rt::BlasKey, Blas*, rt::BlasKeyHash> blasByKey_;  // Blas nodes of blas_ are stable until dropBlas erases both
    std::unordered_map<std::uint64_t, Blas, IdHash> blas_;
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
    Tlas tlas_[rhi::kFramesInFlight];
    std::unique_ptr<rhi::SizeQuery> compactQuery_;  // compacted sizes of each slot's builds
    static constexpr std::uint32_t kQueriesPerSlot = 64;  // >= buildsPerFrame
    std::vector<Compaction> compactions_[rhi::kFramesInFlight];

    // stats (interval since the last log line)
    struct Interval {
        std::uint64_t holeFrames = 0, holes = 0;  // frames whose dynamic instances fell short of the count (TLAS built over fewer), and by how many
        std::uint64_t treeDraws = 0, treeFar = 0, treeCaptured = 0;  // observeTree: calls, beyond the range, captures requested
        std::uint64_t solidWithSkin = 0, solidNoSkin = 0, solidCaptureSkipped = 0;
        std::uint64_t skinIneligible = 0, skinDraws = 0;  // skinned draws of the cascades / pre-pass the skinning pass could not take (morph targets, dynamic buffer) / took
        std::uint64_t charCurrent = 0, charPrevious = 0, charSkinned = 0;  // characters' BLAS refits: from this frame's capture / the previous frame's / of them by the skinning pass
        std::uint64_t capCurrent = 0, capPrevious = 0;  // captures a frame's BLAS refits used: this frame's (the cascades' views) / the previous frame's
        std::uint64_t animatedTrees = 0;  // animated SpeedTrees in the last TLAS
        std::uint64_t volatileBlas = 0;  // ranges that turned volatile (churning instances) in the interval
        std::uint64_t frames = 0, built = 0, builtTriangles = 0, compacted = 0, evicted = 0, failed = 0, observed = 0, skipped = 0, sightings = 0;
        std::uint64_t compactSaved = 0;
        double buildCpuMs = 0.0, passCpuMs = 0.0, observeMs = 0.0, gpuMs = 0.0;
        std::uint64_t textured = 0, untextured = 0, cutOut = 0, segmented = 0, segments = 0, speedTree = 0;  // records of the last frame
        std::uint64_t dynamic = 0;  // characters in the last frame's TLAS
        std::uint64_t engineSegments = 0, engineMerged = 0, batchMaterials = 0;
        std::uint64_t gpuSamples = 0;
    } iv_;
    std::deque<std::pair<std::uint64_t, std::vector<Entry>>> contents_;  // frame value -> the TLAS's instances (last 4)
    // F8-1 census (frame-ownership.md 14.30; -Perf): observe() by kind x view / pass x source (composed / owned), observeOwned, the pass's parts
    struct ObsCensus {
        std::uint64_t calls = 0, sightings = 0;
        double ms = 0.0;
    };
    ObsCensus census_[5][4][2]{};
    double bulkMs_ = 0.0;
    double recMs_[6]{};
    std::uint32_t lastInstances_ = 0;
    std::uint64_t lastTriangles_ = 0;
    std::uint64_t lastCharTriangles_ = 0, lastAnimTreeTriangles_ = 0;
    std::chrono::steady_clock::time_point lastLog_{};
    std::unordered_set<const void*> declLogged_;
    std::unordered_map<std::uint64_t, const char*, IdHash> vsVerdict_;  // VS hash (^ instanced) -> why its draws are not taken (null = taken)
    std::unordered_set<std::uint64_t> blendLogged_;
    std::unordered_set<const char*> speedTreeLogged_;  // reasons (string literals)
    std::unordered_set<std::uint32_t> reasonsLogged_;
    bool instanceIndexChecked_ = false;
    std::uint64_t indexDeviations_ = 0;
};

}  // namespace rvk::enh
