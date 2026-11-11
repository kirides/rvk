#pragma once

// N5.1 "static ops" (docs/research/engine-frame/README.md 4 and "7. N5.1 result"; settings native.static_ops and
// native.static_ops_compare, Flag::Debug | NoAB, default off). The main view's z pre-pass and solid ops of static meshes
// (eCRenderContextMesh: single / whole-mesh z ops, sub-mesh and hierarchy-node solid ops, blended terrain layers, instanced
// ops) are composed from engine data at the op's slot entry: the engine's own translated shaders of the op's permutation,
// every constant register the shaders read, the render states, samplers, textures, streams and the draw ranges, by the
// engine's rules (native/static_compose.*).
//
// Compare (native.static_ops_compare): every draw the engine then makes for the op is diffed against the composed draw of
// the same position (rvk.log `native: static ops: compare ...` every 5 s, the first mismatches named; -LogFrame per op).
// Take-over (native.static_ops): the composed draws are issued by us at the op's slot entry (the device state is saved and
// restored around them, so the engine's own state caches stay valid) and the engine's draws of that op are dropped at
// prepareDraw; the engine's code still runs for every op (requests, LRU stamps, pools, lightmap slots), so its side effects
// are untouched. With both settings on, the dropped engine draws are still compared with what was drawn instead.
//
// N5.2 "scene ops" (README.md "8. N5.2 result"; settings native.scene_ops / native.scene_ops_compare, the groups in
// native.scene_ops_groups): the same machinery for the alpha and refracted lists' mesh ops (blend modes 2-8, the
// EnableAlphaZWrite z-fill + colour pair, fading statics; the refracted water through the enhanced water's DrawReplacer, which
// acts on the composed draw as on the engine's) and the entity overlay passes (burned / frozen / highlighted: the overlay
// materials' passes after the base passes, eCShaderBase::OnGetPasses 0x6d3170).
//
// Off (all settings false): nothing runs beyond the frame tap's pass-through (and the tap itself only when one of them or
// native.frame_tap is on). Render thread only.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "d3d9/device.h"
#include "d3d9/particle_gpu.h"
#include "native/static_ops_retained.h"
#include "native/frame_desc.h"
#include "native/static_compose.h"
#include "native/bb_records.h"
#include "native/tree_owned.h"
#include "native/veg_owned.h"
#include "native/op_records.h"
#include "gpuscene/material_table.h"

namespace rvk::hal::vulkan {
class HalDevice;
}

namespace rvk::native {

// a pointer key's hash (multiplicative, high bits folded down: the map masks the low bits) instead of std::hash's FNV over its
// 8 bytes (O1: the per-draw shader / pool lookups)
struct PtrHash {
    std::size_t operator()(const void* p) const noexcept {
        const std::uint64_t x = (reinterpret_cast<std::uintptr_t>(p) >> 3) * 0x9E3779B97F4A7C15ull;
        return static_cast<std::size_t>(x ^ (x >> 29));
    }
};

class StaticOps {
public:
    explicit StaticOps(hal::vulkan::HalDevice& hal);
    ~StaticOps();
    StaticOps(const StaticOps&) = delete;
    StaticOps& operator=(const StaticOps&) = delete;

    // the op groups, each with its own take-over / compare switches and counts
    enum class Group : std::uint8_t {
        Static,  // N5.1: z pre-pass + solid mesh ops (native.static_ops)
        Mesh,    // N5.2: alpha + refracted mesh ops (native.scene_ops, group bit 1)
        Decals,  // N5.2: eCDecalAdmin::RenderDecals (group bit 4)
        Sky,     // N5.2c: the sky dome op (list +0x1530: stars, sun, moon, dome; group bit 8)
        Billboards,  // N5.2c: eCBillboardAdmin::RenderBillboards, the opaque billboards' queue (group bit 16)
        SkyCube,     // N5.2c: the sky environment cube's faces (eCSkydome_PS::Render with the cube camera; group bit 32)
        Actors,      // N5.3: the skinned actors' ops (eCRenderContextAnimation2: z / solid / alpha / refracted lists; group bit 64)
        Trees,       // N5.4: SpeedTree ops (branches, fronds, leaf cards / meshes) + the tree billboards' ops (z / solid; group bit 128)
        Vegetation,  // N5.4: the grass / vegetation ops (eCRenderContextVegetation: z / solid; group bit 256)
        ShadowViews, // N5.5: every op of the shadow maps' z lists (ExecuteShadowMap: sun cascades, terrain map, point-light atlas
                     // cells; meshes, skinned actors, SpeedTrees, ...: group bit 512)
        Particles,   // N5.6: the particle emitters' alpha ops (eCParticleRenderContext: CPU quads; group bit 4096)
        Strips,      // N5.6: the strips' (trails) and light streaks' alpha ops (eCStripRenderContext, eCLightStreaksRenderContext;
                     // group bit 8192)
        Precipitation,  // N5.6: rain / snow (eCPrecipitationRenderContext, the late list +0x1548; group bit 16384)
        Count
    };
    static constexpr std::size_t kGroups = static_cast<std::size_t>(Group::Count);
    static const char* name(Group g);
    void setMode(Group g, bool takeOver, bool compare);
    // entity overlay passes composed (native.scene_ops_groups bit 2, while scene ops are on): else such ops stay the engine's
    void setOverlays(bool on) { overlays_ = on; }
    bool active() const;

    // driven by the frame tap (native/frame_tap): a main view's Execute begins / ends; the engine enters an op's slot
    void viewBegin(void* db, void* desc, const FrameDesc& view);
    void viewEnd();
    // true = lean (O2a): the op was composed and issued and its side effects done, the engine's slot is not to be run
    bool opEnter(const Op& op);
    // eCDecalAdmin::RenderDecals / eCBillboardAdmin::RenderBillboards of the main view begins (inside its draw tag, before the
    // engine's code) / either ends; true = lean (the original not to be called)
    bool decalsBegin(void* admin, void* db);
    bool billboardsBegin(void* admin);
    // eCSkydome_PS::Render of a sky cube face (UpdateSkydomeEnvCubemap, outside Execute: the cube camera +0x3c0) begins; its
    // draws (view SkyCube) go to cubeDraw; nonOpEnd ends it; true = lean
    bool skyCubeBegin(void* sky);
    // O2a lean ops (native.lean, native.lean_check; docs/research/engine-frame/opt-plan.md "O2a result"): for an op of the
    // mesh-like groups (static, alpha / refracted mesh, overlays, decals, sky, billboards, sky cube) that was composed and
    // issued, the engine's slot / non-op code is not run; its non-drawing side effects are run here instead (resource touches,
    // lightmaps, stream 3, the instance sort, the billboard queue reset, the sky's cloud fields and first-use creation:
    // opt-side-effects-mesh.md 7). check = N: every N-th frame the engine's code runs as before (its draws dropped, the
    // compare active), 0 never.
    void setLean(bool on, int check);
    // native.lean_restore (O2b measurement): false = no device save / restore around a lean op's draws
    void setLeanRestore(bool on) { leanRestore_ = on; }
    // Full-owned lighting (lighting.full_lit; gpu-scene.md 9.8): lit mesh / tree / vegetation draws take ONE base pass with the 3-light permutation, their
    // point lights come from the clustered table (the light registers are written zero; the material variant fills them per pixel), the additive passes of
    // objects with > 3 lights are gone, the batches no longer end on the illumination's static lights / alpha factor. Pictures differ from the engine's.
    void setTableLit(bool on);
    // packet retention (native.packet_cache; retained-signals.md 8): 0 off, 1 on, 2 on + verify every 10th frame, 3 on + verify every frame. Only
    // set when no compare / tap / coverage is active (they need prepareDraw's tap)
    void setPacketCache(int mode) {
        packetMode_ = mode;
        if (mode == 0) packets_.clear();
    }
    // native.owned_windows (frame-ownership.md 7.8): the issued draws' register windows from the material / view tables (needs the material table; Full)
    void setOwnedWindows(bool on) { ownedWindows_ = on; }
    // native.owned_objects (frame-ownership.md 7.13): VS windows 1 / 2 of the non-instanced static mesh draws from the view block / the object table
    void setOwnedObjects(bool on) { ownedObjects_ = on; }
    // native.owned_records (frame-ownership.md 7.14, F2c-2): op records built from the composition and compared (never submitted); needs the object table, the lighting images and table lighting
    void setOwnedRecords(bool on);
    // native.owned_static (frame-ownership.md 7.16, F2c-3; static_ops_owned.cpp): the main view's z / solid mesh ops with a valid op record leave the engine's op
    // lists at the view's start (frame tap, before Execute sorts) and are drawn from their records, without a composition, when the pass begins (BeginZPass /
    // DoBeginRenderSolid returned); their side effects run here. check: every N-th frame (and the one before it) the old path runs and the records are compared
    void setOwnedStatic(bool on, int check);
    bool ownedStaticOn() const { return ownedStatic_; }
    // native.owned_persistent (frame-ownership.md 7.21, F2c-4a): the owned draws' DrawRecords + DrawBlocks live in gpuscene::drawrecords() and are rewritten only when an
    // input of their derivation changed; a frame queues one indirect draw per bucket. Needs owned_static and the scene ABI v2 view table
    void setOwnedPersistent(bool on);
    bool ownedPersistentOn() const { return ownedPersistent_; }
    // a main view begins (frame tap, before the lists are read): false = no compaction this view (setting / prerequisites / a check frame)
    bool ownedViewBegin(void* db, void* desc);
    // native.owned_shadows (frame-ownership.md 7.36, F3-1): the sun cascades' static mesh z ops are owned like the main view's. A cascade view begins (before its list is read):
    // false = nothing leaves its list (setting / prerequisites / a check frame / a cascade the view table has no slot for)
    void setOwnedShadows(bool on);
    bool ownedShadowsOn() const { return ownedShadows_ && !ownedShadowsTripped_; }
    // `cell` = the view's index among the views of its kind in the frame (0-based; a point-light atlas cell, the terrain map)
    bool ownedShadowViewBegin(void* db, ViewKind kind, std::int32_t cascade, std::int32_t cell);
    // native.owned_shadow_views (frame-ownership.md 14.29, F3-4): the terrain shadow map and the point-light atlas's cells are owned like the cascades (op records keyed per
    // view: key 5 = the terrain map, 8 + i = atlas cell i of the frame; view table slots 32 / 33 + i, the trees' 47 / 48 + i), with their actor and SpeedTree ops.
    // Needs native.owned_shadows (its machinery and its MISS / DIFFERING switch-off)
    void setOwnedShadowViews(bool on);
    bool ownedShadowViewsOn() const { return ownedShadowViews_ && ownedShadowsOn(); }
    // the view kinds whose z lists the owned shadow path takes (the cascades with owned_shadows, the terrain map / atlas cells with owned_shadow_views)
    bool ownedShadowKind(ViewKind k) const { return ownedShadowsOn() && (k == ViewKind::Cascade || (ownedShadowViews_ && (k == ViewKind::Terrain || k == ViewKind::PointAtlas))); }
    static constexpr std::int32_t kAtlasCells = 14;  // atlas cells per frame with a view slot of their own (a later cell stays composed: `cells beyond the slots`)
    static constexpr std::uint64_t kKeyTerrain = 5, kKeyAtlas0 = 8;
    static constexpr std::uint32_t kSlotTerrain = 32, kSlotAtlas0 = 33, kSlotTreeTerrain = 47, kSlotTreeAtlas0 = 48;
    // native.owned_actors (frame-ownership.md 7.40, F6-1; static_ops_actors_owned.cpp): the skinned actors' z / solid ops (main view) and z ops (cascades, with owned_shadows) get op
    // records and are drawn from persistent DrawRecords; the per-frame part is the bone table's image of the part
    void setOwnedActors(bool on);
    bool ownedActorsOn() const { return ownedActors_ && !ownedActorsTripped_ && ownedPersistent_; }
    // native.owned_instances (frame-ownership.md 14.19, F2f; static_ops_inst_owned.cpp): the main view's render-instance ops (node 0xffffff) composed one draw per instance (the
    // hardware batches too) get op records whose stamp holds the instance list; their draws' VS window 2 are instance row images
    void setOwnedInstances(bool on);
    bool ownedInstancesOn() const { return ownedInstances_ && ownedPersistent_; }
    // native.owned_ordered (frame-ownership.md 14.20, F9 + F14; static_ops_ordered.cpp): the main view's order-dependent mesh ops (the solid list's blended terrain layers and
    // no-z-write draws, the alpha / refracted lists' mesh ops, sorted back to front by the engine every frame) get op records too and, on the owned frames, are drawn from their
    // persistent DrawRecords AT THEIR OP'S PLACE (the engine's slot is skipped; an ordered flush, one indirect command per run of consecutive equal buckets) instead of being
    // composed; the engine's own sort result is the order. Needs owned_persistent (+ owned_static, the scene ABI v2 view table)
    void setOwnedOrdered(bool on);
    // native.owned_decals (frame-ownership.md 14.26, roadmap row 13; static_ops_nonop.cpp): 0 off, 1 on (a decal record's geometry in persistent buffers, rewritten only
    // when what it is made from changed: drawn without reading / transforming a vertex), 2 compare (built every frame, checked against the persistent copy: MISS)
    void setOwnedDecals(int mode) {
        if (mode == 0 && ownedDecals_ != 0) releaseDecalCache();
        ownedDecals_ = mode;
    }
    int ownedDecals() const { return ownedDecals_; }
    std::uint64_t ownedDecalHits() const { return decalHitsTotal_; }
    bool ownedOrderedOn() const { return ownedOrdered_ && ownedPersistent_ && ownedStatic_; }
    // F6-2 (frame-ownership.md 14.23): the actors' exclusions of F6-1 lifted while native.owned_actors is on (overlay flags, fade < 1, the alpha list's actor ops: their
    // order-dependent draws through native.owned_ordered; switch materials through the material table's switch variants); `RVK_F62_OFF=1` = F6-1's exclusions (A/B)
    static bool f62On();
    bool actorOrderedNow(const Op* op) const;  // the op being composed is an actor op the ordered path records (Actors group, main view, solid / alpha)
    void orderedFlushRun();  // the pending run of ordered draws as one ordered flush (also the device's hook: Device::setOrderedHook)
    // may the list's ops leave it (the database's caps: the z list is executed, the solid pass is bracketed by DoBeginRenderSolid)
    bool ownedListOk(void* db, List list);
    // an op of the raw (unsorted) list: true = its record is valid, the op leaves the engine's list
    bool ownedSelect(const Op& op);
    // the ops that left `list`, in the engine's sort order; compactNs = the time the tap spent on the list
    void ownedTake(List list, std::vector<Op>&& ops, std::uint64_t compactNs);
    // BeginZPass / DoBeginRenderSolid of the main view returned: the pass's owned ops are drawn; returns the draws made (owned + late compositions)
    std::uint32_t ownedPass(bool solid);
    // native.owned_billboards (frame-ownership.md 7.17, F4-1): the tree billboard ops retained as records, compared / drawn from them (needs owned windows, lighting images, scene draws)
    void setOwnedBillboards(bool on);
    // native.owned_vegetation (frame-ownership.md 7.40, F7-1): the vegetation ops' instance streams are retained per op in a persistent GPU buffer (valid while the op's
    // source instance bytes are unchanged: an exact compare), so the grass draws no longer carry CPU bytes, become packet-cache replayable and are not converted / uploaded again
    void setOwnedVegetation(bool on);
    bool ownedVegetationOn() const { return ownedVeg_; }
    // native.own_foliage (frame-ownership.md 14.14, S-5): grass distance thinning tied to lod.view (sc::grassThin on every converted instance stream of the main view's grass:
    // composed, composed late and owned alike, so the z / colour pair stays bit-identical)
    void setOwnFoliage(bool on);
    bool ownFoliageOn() const { return ownFoliage_; }
    struct FoliageStats {
        std::uint64_t ops = 0, instances = 0, changed = 0, removed = 0;
    };
    const FoliageStats& foliageTotal() const { return foliageTotal_; }
    // native.owned_vegetation_ops (frame-ownership.md 7.42, F7-2; static_ops_veg_owned.cpp): the main view's vegetation z / solid ops retained as templates per (type, pass, sun) bucket,
    // compared on the check frames, taken out of the lists on the other frames and drawn at the pass (one record per op x lighting entry, the converted instance bytes of all
    // ops in one ring block, one indirect command per bucket). Needs owned_persistent, scene ABI v2, lighting images, table lighting.
    void setOwnedVegetationOps(bool on);
    bool ownedVegetationOpsOn() const { return ownedVegOps_; }
    void vegViewBegin(bool ownedFrame, ViewKind kind);
    bool vegKindOk(ViewKind kind) const { return ownedVegOps_ && kind == ViewKind::Main; }
    bool vegSelect(const Op& op);
    void vegTake(List list, std::vector<Op>&& ops, std::uint64_t ns);
    std::uint32_t vegPass(bool solid);
    // native.owned_trees (frame-ownership.md 7.25, F4-2 / F4-3; static_ops_trees_owned.cpp): the main view's SpeedTree z / solid ops are retained as templates per LOD bucket,
    // compared on the check frames, and on the other frames taken out of the lists at the view's beginning and drawn at the pass from the engine's tree objects (rows +
    // per-instance records, a few indirect calls). Needs owned_persistent (the DrawBlock slots), scene draws v2, lighting images, table lighting.
    void setOwnedTrees(bool on);
    bool ownedTreesOn() const { return ownedTrees_; }
    // a view begins (frame tap; every kind): the frame kind, the prerequisites. F4-4: a sun cascade's z list takes its SpeedTree ops too while native.owned_shadows is on
    void treeViewBegin(bool ownedFrame, ViewKind kind, std::int32_t cascade, std::int32_t cell = 0);
    void stagesReport();  // frame-ownership.md 7.47: every owned stage's state (setting / armed / acted) in one 5 s line
    void vertexPullReport();  // S-4 (frame-ownership.md 14.12): the `native: vertex pull (5 s)` line
    bool treeKindOk(ViewKind kind) const { return ownedTrees_ && (kind == ViewKind::Main || ownedShadowKind(kind)); }
    bool treeSelect(const Op& op);            // compaction: this tree op leaves the list
    void treeTake(List list, std::vector<Op>&& ops, std::uint64_t ns);
    std::uint32_t treePass(bool solid);       // BeginZPass / DoBeginRenderSolid returned: the taken ops are drawn; returns the draws made
    // native.scene_draws (frame-ownership.md 7.11): the main view's static draws are issued as scene draws (Device::setSceneIssue)
    void setSceneDraws(bool on) { sceneDraws_ = on; }
    // native.gpu_scene (docs/research/gpu-scene.md): >= 1 checks every mesh / tree / actor op's entity against the signal-fed instance table
    void setGpuScene(int mode) { gpuScene_ = mode; }
    // V3 retained composition (native.retained / native.retained_groups / native.retained_check)
    void setRetained(bool on, int groups, bool check);
    bool inSkyCube() const { return inSkyCube_; }
    void nonOpEnd();
    // an engine draw of the main view (op = the executing op, null: none); true = drop it (we drew the op ourselves)
    bool engineDraw(const d3d9::DrawTapInfo& draw, const Op* op);
    // true while our own draws are being issued (the tap must not attribute them to the engine)
    bool issuing() const { return issuing_; }
    // one of our own draws reached prepareDraw: its device state is compared with the composed draw (the recorder gets what
    // was composed; with compare on)
    void ownDraw(const d3d9::DrawTapInfo& draw);
    void report();
    // N5.8 coverage: why the engine's draws of `op` (null: of the non-op path running) stay the engine's (empty: no reason
    // recorded, i.e. the op was not met by the composition)
    std::string_view leftReason(const Op* op) const;

    enum class Skip : std::uint8_t {
        None, Kind, Overlay, ShaderClass, Version, InvalidShader, NotCompiled, Switch, Primitive, ImageOverride, Unreadable, PassKind,
        Sort, Texture, OverlayShader, DecalProxy, NotCreated, VisualShader, SkyCamera, SkyRebuilt, Bones, Shadow, Simulated, Instanced, Count
    };
    static const char* name(Skip s);

private:
    struct Live {
        std::bitset<sc::kVsRegs> f;  // float registers in the CTAB
        std::array<std::uint64_t, 4> words{};  // the same as word masks (bit r of word r / 64)
        std::uint32_t boolInt = 0;   // bool / int registers in the CTAB
    };
    struct GroupState {
        bool takeOver = false, compare = false;
        sc::CompareCounts window, total;
        std::array<std::uint64_t, static_cast<std::size_t>(Skip::Count)> skips{}, skipsTotal{};
        std::uint64_t issuedDraws = 0, issuedOps = 0, droppedDraws = 0, ownChecked = 0, ownMismatch = 0;
        // -Perf: issue()'s cost split (ns): the whole call, the draw calls inside it (device path), draws timed (stage 0 of the packet retention)
        std::uint64_t issueNs = 0, deviceNs = 0, timedDraws = 0, timedOps = 0;
        // the lights of the composed sub-mesh draws (dynamic list +0x34 + static illum +0xd: 0..4; 4 = an additive light pass)
        std::array<std::uint64_t, 5> lights{};
        bool active() const { return takeOver || compare; }
    };
    // the engine's constant state at the op's entry, updated as the engine path would write it (C-materials.md 3)
    struct Mirror {
        float adminVs[31][4]{};   // admin VS pool c0..c30
        float adminPs[6][4]{};    // admin PS pool c0..c5
        float global[201][4]{};   // ms_pGlobalShaderConstants VS pool c31..c231
        float lighting[24][4]{};  // the lighting pool c14..c37
        struct ShaderPools {
            std::uint64_t epoch = 0;  // read in this mirror epoch (re-read when it differs)
            std::uint32_t vsFirst = 232, psFirst = 6, vsCount = 0, psCount = 0;
            float vs[32][4]{};
            float ps[16][4]{};
            std::array<const void*, 16> slotImage{};  // sampler pool slot images (switch samplers update them)
            std::uint32_t slots = 0;
        };
        std::unordered_map<const void*, ShaderPools, PtrHash> shaders;
        std::array<const void*, 2> lightmapSlot{};  // admin s4 / s5 slot images (BindLightmaps)
    };
    // O1: the mirror is our own register file, carried across ops, views and frames like the device's (the engine's pools are
    // read once; then only the writes of engine code we do not compose are merged in: native.mirror, docs/research/engine-frame/
    // opt-plan.md "O1 result"). Mode 0 persistent, 1 the engine's pools re-read per view, 2 per op (the N5.x behaviour).
public:
    void setMirrorMode(int mode) { mirrorMode_ = mode; }
    // native.coverage: the per-draw bookkeeping beyond the compare (see FrameTap)
private:
    int mirrorMode_ = 0;
    bool mirrorValid_ = false;      // global / lighting / lightmap slots read (else: read whole at the next use)
    std::uint64_t mirrorEpoch_ = 1;  // the shaders' pools and the admin pools are re-read once per epoch (a view; mode 2: an op)
    std::uint64_t adminEpoch_ = 0;
    static constexpr std::size_t mirrorShaderCap_ = 8192;
    const void* mirrorGlobalSrc_ = nullptr;
    const void* mirrorLightingSrc_ = nullptr;
    // the material admin the lightmap slots are read from (the last view's: a view's first ops run before composeView sets view_)
    const std::uint8_t* poolAdmin_ = nullptr;
    // engine code we do not compose ran (an op left to the engine, a non-op unit not composed): the engine's pools as they were
    // before it; the registers it changed are merged into the mirror at the next use
    struct EngineSnap {
        float global[201][4]{};
        float lighting[24][4]{};
        std::array<const void*, 2> lightmapSlot{};
    };
    EngineSnap leftSnap_, leftNow_;
    bool leftPending_ = false;
    std::uint64_t mergedRegs_ = 0, mergedLeft_ = 0;  // diagnostics: registers merged / left units seen
    void markLeft();
    bool readEnginePools(float (*global)[4], float (*lighting)[4], std::array<const void*, 2>& lightmaps) const;
    // per main view, natively composed (camera, fog, viewport, obstacle)
    struct View {
        bool valid = false;
        const std::uint8_t* admin = nullptr;
        const std::uint8_t* gfx = nullptr;
        float vp[4][4]{}, dbvp[4][4]{}, invProj[4][4]{}, invView[3][4]{};
        float camPos[4]{};
        float fogC3[4]{}, fogC4[4]{};
        bool fog = true;  // the view executes its fog (eCGfxFog::Execute); false: PS c3 / c4, VS c30 as the admin's pools hold them
        float obstacleX = 0, obstacleY = 0;
        bool zColorWrite = true;
        int dbOrder = -1;  // the depth-bias product's order matched against the admin's copy (0 P x V, 1 V x P, -1 neither)
    };

    // what a draw takes from the mesh resource's visual + the sub-mesh (eCGfxVisual, its eCAPIPrimitive record)
    struct Geom {
        const void* decl = nullptr;
        std::array<sc::DrawDesc::Stream, 4> streams{};
        const void* indices = nullptr;
        std::uint32_t minIndex = 0, numVertices = 0, startIndex = 0, primCount = 0;
        std::uint32_t primType = 4;   // the static path: an indexed triangle list, base vertex 0; skinned primitives: the record's
        std::int32_t baseVertex = 0;
        sc::Visual visual;
        const void* subMesh = nullptr;
        const void* material = nullptr;
        const void* meshResource = nullptr;
    };
    // camera null: the database's (db +0x14c0 -> +0x1738), with its fog; else that camera, no fog of its own (the sky cube)
    bool composeView(const std::uint8_t* admin, const std::uint8_t* camera = nullptr);
    bool shaderClassOk(const void* shader);
    const Live& liveOf(const d3d9::Shader* s);
    d3d9::Shader* shaderOfElement(const void* element);
    d3d9::Texture* textureOfImageData(const void* imageData, bool& ok);
    struct TexCacheEntry {
        const void* api = nullptr;
        d3d9::Texture* texture = nullptr;
    };
    static constexpr std::uint32_t kTexCacheBits = 12;
    std::array<TexCacheEntry, 1u << kTexCacheBits> texCache_{};
    std::uint64_t texCacheGen_ = ~0ull;
    Mirror::ShaderPools* poolsOf(const void* shader);
    void initMirror();
    // compose the op into ops_' draws; Skip::None on success
    Skip composeOp(const Op& op);
    Skip composeZ(const Op& op);
    Skip composeSolid(const Op& op);
    Skip subMeshDraws(const std::uint8_t* res, const std::uint8_t* sub, std::uint32_t node, std::int32_t switchValue,
                      std::uint32_t combi, const std::uint8_t* illum, float fade, const float* world, bool instanced,
                      std::uint8_t flags, std::uint32_t batchCount);
    // eCShaderBase::GetPasses 0x6d1ee0 (-> OnGetPasses: the base shader's passes, then the overlay materials' for flags 1 / 2 / 4)
    // + ExecutePass per pass over `geometry` (the sub-mesh draws, the skinned primitives); world: for the 4-light log only
    Skip materialDraws(const std::uint8_t* shader, const std::uint8_t* illum, std::uint32_t combi, bool instanced, std::int32_t switchValue,
                       std::uint8_t flags, const Geom& geometry, const float* world);
    Skip materialDrawsImpl(const std::uint8_t* shader, const std::uint8_t* illum, std::uint32_t combi, bool instanced, std::int32_t switchValue,
                           std::uint8_t flags, const Geom& geometry, const float* world);
    // eCShaderBase::OnGetPasses' version GetPasses for `owner` (the base shader, or an overlay shader) appended to `out`
    Skip shaderPasses(const std::uint8_t* owner, const sc::PassInputs& in, const std::uint8_t*& version, std::vector<sc::Pass>& out);
    // ExecutePass: `shader` = the base shader (SetSwitch, depth bias), `owner` = the pass's version owner (states, pools, samplers)
    Skip passDraw(const std::uint8_t* shader, const std::uint8_t* owner, const std::uint8_t* version, const sc::Pass& pass,
                  const Geom& geometry, bool overlay);
    // ExecuteSpecialPass(option) 0x6d1cd0: the special pass type * 2 + option (SpeedTree z passes: option 1)
    Skip specialPassDraw(const std::uint8_t* shader, const Geom& geometry, bool option = false);
    Skip specialPassDrawImpl(const std::uint8_t* shader, const Geom& geometry, bool option);
    // eCMeshResource2::RenderZPass 0x7603b0 (world null: no SetWorldMatrix / SetGlobalAlpha; sub 0xff: the whole mesh)
    Skip meshZDraws(const std::uint8_t* res, std::uint32_t sub, std::uint32_t node, const float* world, bool option);
    Skip adminSamplers(sc::DrawDesc& d, const std::uint8_t* owner, const d3d9::Shader* ps);
    // O1: the engine state the samplers of every draw of an op read, read once per op (the composition runs at the op's entry:
    // nothing of the engine runs between its draws): the admin's sampler slots / images +0x248..+0x350, the per-stage override
    // images, the gfx admin's default filter
    struct SamplerSnap {
        std::uint64_t serial = ~0ull;
        const std::uint8_t* admin = nullptr;
        std::uint8_t block[0x108]{};  // admin +0x248 ..
        std::array<const void*, 16> overrides{};
        std::uint32_t defFilter = 0;
        std::uint64_t hash = 0;  // V3: of the block, the overrides and the default filter (the retained draws' environment)
        bool hashValid = false;  // V4: computed on first use (snapHash()): only the retained keys need it
        template <class T>
        T at(std::uint32_t off) const {
            T v;
            std::memcpy(&v, block + (off - 0x248), sizeof(T));
            return v;
        }
    } snap_;
    std::uint64_t opSerial_ = 0;  // bumped per op / unit (finishOp)
    const SamplerSnap& samplerSnap();
    bool treeTexUnchanged(const tro::TexMap& x);
    std::uint64_t snapHash();
    bool geometryOf(const std::uint8_t* res, Geom& g, int stream3);  // stream3: 0 off, 1 on, -1 as the record has it
    sc::DrawDesc& drawFrom(const Geom& g);
    // defaultClass: eCShaderDefault's register map (admin c0..c30, global c31..c231, the shader's pools from c232 / PS c6..c13,
    // the lighting pool PS c14..c37); else eCShaderBase's (no global constants, no lighting: the shader's pools where they are)
    void constantsInto(sc::DrawDesc& d, const std::uint8_t* shader, const std::uint8_t* biasShader = nullptr, bool defaultClass = true);
    Skip materialSamplers(sc::DrawDesc& d, const std::uint8_t* shader, const void* psElement);
    void applySwitch(const std::uint8_t* shader, std::int32_t switchValue, Skip& skip, bool variant = false);
    // frame-ownership.md F0 (native.material_table): the draw's material registers / texture bindings composed from the material's record and compared
    // with `d` (after constantsInto + materialSamplers); owner = the shader object whose pools / slots were used, psMask = the PS element's sampler mask
    void checkMaterial(sc::DrawDesc& d, const std::uint8_t* owner, const void* material, bool zPass, std::uint16_t psMask, std::int32_t blendMode,
                       std::int32_t maskRef);
    static std::uint32_t materialTexture(void* self, std::uint32_t stage, const void* imageRes);  // the table's texture resolution (materialSamplers')
    // 7.27: off the check frames the material table's slot of (owner, pixel shader, z pass) is looked up once per frame (the table's poll, graph note and lookup
    // run at the first draw; the register / texture compare only on check frames)
    struct MatMemo {
        const void* owner = nullptr;
        const void* ps = nullptr;
        std::uint64_t frame = ~0ull;
        std::uint32_t slot = 0xffffffffu;
        bool zPass = false;
        std::uint64_t sw = 0;  // (F6-2: the switch variant's hash; 0 = the base record)
    };
    // F6-2 (frame-ownership.md 14.23): switch materials. passDraw's SetSwitch writes (applySwitch with `variant`) give the pass's draws a switch variant record in the
    // material table (exact registers / slot images for the draw's switch value; the base record's are history dependent): such a composition is recordable (no
    // Excl::Switch) and its owned draws read the variant record's windows. On while native.owned_actors is on (F6-2), `RVK_F62_OFF=1` = off
    std::vector<gpuscene::MaterialTable::SwitchWrite> passSw_;
    const std::uint8_t* passSwShader_ = nullptr;  // (the shader the writes were applied to; null = no variant for the next checkMaterial)
    std::uint64_t passSwHash_ = 0;
    bool switchVariantsOn() const;
    std::array<MatMemo, 256> matMemo_{};
    // 7.27: off the check frames the illumination slot + lighting image entry of (illumination, pass parameters) is looked up once per frame
    struct LightMemo {
        const void* illum = nullptr;
        std::uint64_t frame = ~0ull;
        std::uint32_t slot = 0xffffffffu, entry = 0xffffffffu, n = 0, first = 0;
        bool tableLit = false, holdsAll = false;
    };
    std::array<LightMemo, 256> lightMemo_{};
    void writeWorld(std::uint32_t slot, const float* m);
    void writeAlpha(std::uint32_t slot, float a);
    void writeTreeInstance(std::uint32_t slot, const float v[2]);  // SetSpeedTreeInstanceData 0x6c9eb0: c32 + 4 slot .xy
    // returns the lighting image entry of the pass (gpuscene::LightingImages::note; kNoEntry without one): F1a points PS window 3 at it
    std::uint32_t writeLighting(const sc::Pass& pass);
    // the engine's lighting composition (FUN_1806cd1d0); `holdsAll` = the table-lit early exit was taken (nothing read for the point lights)
    void writeLightingRegisters(const sc::Pass& pass, bool& holdsAll);
    void noteListLights(const std::uint8_t* list);
    std::uint32_t bindLightmaps(const std::uint8_t* res);
    void capture(const d3d9::DrawTapInfo& t, sc::DrawDesc& out);
    // the decals (static_ops_nonop.cpp)
    Skip composeDecals(const std::uint8_t* admin, const std::uint8_t* db);
    Skip nonOpPasses(const std::uint8_t* shader, const std::uint8_t* illum, std::uint32_t combi, const Geom& geometry,
                     std::int32_t switchValue = 0);
    // a non-op path's unit begins (false: the group is off / the checks failed) / its composition's result (issued on take-over)
    bool nonOpGroupBegin(Group group, void* admin);
    void nonOpGroupComposed(Skip s);
    bool geometryOfVisual(const std::uint8_t* visual, Geom& g);
    bool verifyDecals();
    // a composed draw's vertex / index bytes against the engine buffer's (the vertex data the engine generated on the CPU)
    bool geometryBytesEqual(const sc::DrawDesc& c, const sc::DrawDesc& captured, std::string& why);
    // the opaque billboards' queue (static_ops_nonop.cpp)
    Skip composeBillboards(const std::uint8_t* admin);
    // N5.8: a billboard set's alpha op (RenderAlpha 0x4f4c70 -> eCBillboardAdmin::RenderBillboard(..., alpha = true, fade) 0x4f0c00)
    Skip composeBillboardAlpha(const Op& op);
    const std::uint8_t* billboardAdmin_ = nullptr;  // the eCBillboardAdmin of the last RenderBillboards (RenderAlpha's admin)
    std::uint64_t billboardAlphaOps_ = 0, billboardAlphaDraws_ = 0;
    std::uint32_t streakLogged_ = 0;
    // F1: the sky objects' billboards / converted vertices / quad indices, kept across calls (static_ops_sky.cpp)
    struct SkyCache {
        std::vector<std::uint8_t> key, verts, quads;
        std::uint32_t quadCount = ~0u;
    };
    std::vector<std::uint8_t> skyBb_;
    std::unordered_map<const void*, SkyCache, PtrHash> skyCaches_;
    SkyCache skyScratch_;
public:
    void setOpts(unsigned mask) { opts_ = mask; skyCache_ = (mask & 1u) != 0; }
    bool opt(unsigned bit) const { return (opts_ & bit) != 0; }
private:
    unsigned opts_ = 255;
    bool skyCache_ = true;
    bool verifyBillboards();
    // the skinned actors' ops (static_ops_actors.cpp)
    bool verifyActors();
    Skip composeActor(const Op& op);
    Skip composeActorImpl(const Op& op);  // (composeActor = the F6-0 measurement wrapper)
    // the bone b's matrix as the shader gets it: the palette entry the engine reads (stored this frame) or composed as
    // UpdateRenderMatrices will (X[i] *= W[i] with the engine's own affine product)
    bool paletteMatrix(const std::uint8_t* wrapper, std::uint32_t bone, float out[16]);
    void writeBone(std::uint32_t slot, const float* m);
    // the SpeedTree / tree billboard / vegetation ops (static_ops_trees.cpp)
    bool verifyTrees();
    Skip composeTree(const Op& op);
    Skip composeTreeBillboard(const Op& op);
    Skip composeVegetation(const Op& op);
    // the view from a material shader's admin (+0x220) + the mirror (false: another admin / not composable)
    bool viewAndMirror(const std::uint8_t* shader);
    // the wind registers of the admin's VS pool as their writers set them: c14..c29 (eCShaderMaterialAdmin::SetSpeedTreeData
    // 0x6b7df0 from eCSpeedTreeAdmin +0xac / +0x6c and the application camera), c12 (eCVegetationAdmin::Process 0x7016b0)
    void treeWind();
    void grassWind();
    // the sky dome op (static_ops_sky.cpp)
    Skip composeSky(const Op& op);
    Skip composeSkyDraws(const std::uint8_t* sky, const std::uint8_t* camera);
    bool verifySky();
    // the camera matrices eCGfxCamera::Execute -> SetCamera uploads for view / projection / depth-bias projection, computed with
    // the engine's own bCMatrix functions (the sky's camera hack and its restore): into view_ and the mirror's admin VS c4..c10
    void setCamera(const float view[16], const float proj[16], const float dbProj[16]);
    // eCSkyObject::Render 0x6e25e0 of a sky object (stars / sun / moon); bb: the billboards FillVertexBuffer writes (null: the
    // object's array), renderCount: the billboards drawn (0: the record's primitive count / 2)
    Skip skyObjectDraws(const std::uint8_t* object, const float* world, const std::uint8_t* bb, std::uint32_t renderCount);
    Skip skyDomeDraw(const std::uint8_t* shader, const std::uint8_t* visual);
    // F12 (frame-ownership.md 14.24, native.own_sky): around the issue of a sky dome draw (the device state set, before / after the draw): On = the ports' shaders
    // set (true: restore them after the draw); Compare = the dome drawn with the ports into a scratch copy of the target first, compared after the draw
    bool skyOwnBefore(const sc::DrawDesc& d);
    void skyOwnAfter(const sc::DrawDesc& d, bool ported);
    d3d9::Surface* skyCmpScratch_ = nullptr;
    d3d9::Surface* skyCmpTarget_ = nullptr;
    std::uint64_t skyCmpSkips_ = 0;
    int blendModeOf(const std::uint8_t* owner) const;
    // N5.6 (static_ops_fx.cpp): particles, strips, light streaks, precipitation
    bool verifyFx();
    Skip composeFx(const Op& op);
    Skip composeStrip(const Op& op);
    Skip composeStreaks(const Op& op);
    Skip composeParticle(const Op& op);
    Skip composePrecipitation(const Op& op);
    int fxVerified_ = 0;
    // a shader class's own global pool (eCShaderParticle::ms_pGlobalShaderConstants +0x20): its registers as the op's path writes
    // them; count 0 = none (constantsInto, non-default classes)
    float fxPool_[32][4]{};
    std::uint32_t fxPoolFirst_ = 31, fxPoolCount_ = 0, fxPoolNativeLast_ = 0;
    const std::uint8_t* particleGlobalVar_ = nullptr;  // &eCShaderParticle::ms_pGlobalShaderConstants (decoded from 0x6d5fc2)
    bool fogOverride_ = false;  // EnableFog (shader +0x2b0) as ApplyRenderStates writes it
    Skip particlePassDraw(const std::uint8_t* shader, const std::uint8_t* version, std::uint32_t key, const Geom& geometry, float alpha);
    bool precipitationRecords(const std::uint8_t* ps, std::uint32_t records, std::vector<std::uint8_t>& out);
    std::uint32_t precipLogged_ = 0;
    // the billboard records skyObjectDraws expands instead of the object's array (precipitation: re-done from the drops)
    const std::vector<std::uint8_t>* billboardsOverride_ = nullptr;
    bool particleVertices(const std::uint8_t* ps, const std::uint8_t* entity, std::vector<std::uint8_t>& out, std::uint32_t& quads);
    // ---- F11 (frame-ownership.md 14.26, native.own_particles): the particle quads expanded on the GPU (d3d9/particle_gpu.h) ----
    // the op's particle records (the engine's depth order) + its constants: what particleVertices builds, without the per-corner work
    bool particleRecords(const std::uint8_t* ps, const std::uint8_t* entity, std::vector<std::uint8_t>& out, std::uint32_t& quads, d3d9::particle::OpConsts& k);
    // compare: the C++ twin of the GPU expansion against the engine-rule vertices, byte for byte (counted)
    void particleTwinCheck(const std::vector<std::uint8_t>& recs, const d3d9::particle::OpConsts& k, const std::vector<std::uint8_t>& vb, std::uint32_t quads);
    // `x` (a composed particle draw) as its GPU form: the expansion vertex shader, the record stream (instances), VS c232..c247; false (`why`): stays as it is
    bool particleToGpu(sc::DrawDesc& x, const std::vector<std::uint8_t>& recs, std::uint32_t quads, const d3d9::particle::OpConsts* k, int kind, const char*& why);
    // compare: around the issue of a composed particle draw that has a GPU form (DrawDesc::particleGpu == 2): its GPU form drawn into a scratch copy of the target
    // before the composed draw, the two images compared after it
    void particleCompareBefore(const sc::DrawDesc& d);
    void particleCompareAfter();
    int ownParticlesMode() const;  // 0 off, 1 on, 2 compare (OwnPost::Scene::Particles)
    struct ParticleGpu {
        std::vector<std::uint8_t> recs;
        d3d9::particle::OpConsts k;
        std::uint32_t quads = 0;
        sc::DrawDesc gpu;  // the compare draw's GPU form
        bool armed = false;
        const char* name = "particles";  // the compared image
        d3d9::Surface* scratch = nullptr;
        d3d9::Surface* target = nullptr;
        std::uint64_t skips = 0;
    } ptCmp_;
    struct ParticleGpuStats {
        std::uint64_t ops = 0, quads = 0, recordBytes = 0, cpuOps = 0, cpuQuads = 0, cached = 0;
        std::uint64_t twinVertices = 0, twinExact = 0, twinDiffer = 0, compares = 0;
        std::uint64_t rainOps = 0, rainDrops = 0, rainCpuOps = 0;  // the precipitation (sky billboard records)
        std::uint64_t stripOps = 0, stripVertices = 0, stripCpuOps = 0;  // the strips (vertex records)
        std::string stripWhy;
        std::string rainWhy;
        float twinMaxAbs = 0.0f;
        std::string why;
        std::uint64_t cpuUs = 0, cpuTimed = 0;
    } ptWindow_;
    std::uint32_t ptTwinLogged_ = 0;
    std::unique_ptr<d3d9::Buffer> ptIb_, ptDummy_;
    std::array<std::uint16_t, 6> ptIndices_{};
    bool ptIndicesKnown_ = false;
    bool leanParticleGpu_ = false;
    d3d9::particle::OpConsts leanParticleConsts_;
    void particleGpuReport();
    // ---- native.owned_decals: the decal records' persistent geometry (composeDecals) ----
    struct DecalEntry {
        std::unique_ptr<d3d9::Buffer> vb, ib;  // static (POOL_MANAGED) buffers of the record's vertices (from vertex 0) / indices (record-relative)
        std::int32_t vertices = 0, indices = 0;
        std::vector<std::uint8_t> vbBytes;     // what they hold (the content checks)
        std::vector<std::uint16_t> ibBytes;
        std::uint64_t lastFrame = 0;
    };
    static constexpr std::size_t kDecalEntries = 2048;
    std::unordered_map<std::uint64_t, DecalEntry> decalCache_;
    struct DecalStats {
        std::uint64_t passes = 0, cpuUs = 0, records = 0, decals = 0, hits = 0, created = 0, rebuilt = 0, checked = 0, miss = 0, full = 0, evicted = 0, uploadBytes = 0;
    } decalW_;
    int ownedDecals_ = 0;
    std::uint32_t decalMissLogged_ = 0;
    std::uint64_t decalTotalMiss_ = 0, decalHitsTotal_ = 0;
    void decalsReport();
    void releaseDecalCache();
    struct FxStats {
        std::uint64_t particleOps = 0, particleQuads = 0, particleSkipped = 0, particleEngineBytes = 0, particleZeroQuads = 0, particleSorted = 0;
        std::array<std::uint64_t, 6> orient{};    // PS +0x170: 0..4, other
        std::array<std::uint64_t, 2> coords{};    // PS +0xb0: world / entity-relative
        std::uint64_t rotated = 0, frameBlend = 0, lit = 0, instancedEmitters = 0;
        std::uint64_t stripOps = 0, stripSegments = 0, streakOps = 0, precipOps = 0, precipDrops = 0, precipOff = 0;
        // particle ops left to the engine because no particle was alive at the entry, and those of them the engine then drew
        std::uint64_t simulatedOps = 0, simulatedDrew = 0;
        // the precipitation's billboard records re-done from the drops vs the engine's (eCPrecipitation_PS::Update's fill)
        std::uint64_t precipRecordsEqual = 0, precipRecordsDiffer = 0;
    } fxWindow_, fxTotal_;
    // a draw tag for the issued draws other than the slot's (particles: eCParticle_PS::Render's TagScope inside the slot)
    bool tagOverride_ = false;
    const Op* simulatedOp_ = nullptr;  // the last particle op skipped as Simulated (its engine draws are counted)
    bool simulatedDrew_ = false;
    d3d9::draws::Tag tagOverrideValue_{};
    void finishOp();
    void issue();
    // ---- O2a lean ops (static_ops_lean.cpp) ----
    bool lean_ = false;
    bool leanRestore_ = true;
    bool tableLit_ = false;
    std::uint64_t tableLitFrame_ = ~0ull;
    std::vector<const void*> tableLitSeen_;  // light lists whose lights were handed to the table this frame
    int leanCheck_ = 0;
    std::uint64_t leanFrameNo_ = ~0ull;
    sc::LeanFrame leanFrameKind_ = sc::LeanFrame::Lean;
    bool leanFrameOn_ = false;
    void leanUpdateFrame();
    // the compare runs (with lean on: only on check frames; on the other frames the engine's draws of the ops it still runs see
    // the device the lean ops left, not the engine's own: opt-plan.md "O2a result")
    bool comparing() {
        if (g_ == nullptr || !g_->compare) return false;
        if (!lean_) return true;
        leanUpdateFrame();
        return leanFrameKind_ == sc::LeanFrame::Check;
    }
    int leanVerified_ = 0;  // the engine functions the lean path calls: 1 resolved, -1 not (never lean)
    int leanAnimVerified_ = 0;  // O2b: the animated / simulated kinds' engine functions and sites (1 ok, -1: those groups not lean)
    bool leanFrame();       // lean on and this frame is not a check frame
    bool leanGroup(Group g) const;
    bool verifyLeanAnim();
    // the op being composed is a lean candidate: the side effects its engine code would have are collected while composing
    // (leanBegin) and run when its draws were issued by us (leanCommit: true = the engine's code is skipped)
    bool leanCollect_ = false;
    // kBlockEdge: an engine path whose side effects the lean path does not reproduce (rare first-use / failure cases): the op keeps
    // the engine's code
    enum LeanBlock : std::uint32_t { kBlockVideo = 1, kBlockEdge = 2 };
    std::uint32_t leanBlock_ = 0;
    std::vector<const void*> leanTouches_, leanLightmaps_;
    struct LeanStream3 {
        void* primitive;
        bool on;
    };
    std::vector<LeanStream3> leanStream3_;
    bool slotSkipped_ = false;  // the current op's / unit's engine code was skipped
    void leanBegin(Group g);
    bool leanCommit();
    void leanAbort() { leanCollect_ = false; }
    void leanTouch(const void* resource) {
        if ((leanCollect_ || recCollect_) && resource != nullptr) leanTouches_.push_back(resource);
    }
    void leanImage(const void* resource, const void* imageData);  // a bound image: touch + video check + placeholder count
    void touchNow(const void* resource);  // eCResource2::RequestData(resource, 1) once per frame
    sc::TouchSet<8192> touched_;
    // first use / the S writes the engine's code makes before its draws (run before composing)
    void leanDecalsPre(const std::uint8_t* admin);
    void leanBillboardsPre(const std::uint8_t* admin);
    void leanSkyPre(const std::uint8_t* sky);
    void leanBillboardsPost(const std::uint8_t* admin);
    void leanSortInstances(const std::uint8_t* res);
    void leanStream3(const std::uint8_t* res, bool on);
    // ---- O2b: the animated / simulated kinds (opt-side-effects-anim.md; run where the engine's code runs them, before / while
    // composing; the ones that would change what a fallback slot sees are deferred to the commit) ----
    // eCAnimation_PS::Render: UpdateRenderMatrices(wrapper, frame id) (the palette + its stamp), then HardRenderActor2::Render's
    // touches of the HRA resource and (cached) the LOD primitive's material
    void leanActorPre(const std::uint8_t* wrapper, std::uint16_t primIdx);
    // eCRenderContextSpeedTree::Render / RenderZPass: the queue sorted in place at the render buffer's first op + its stamp = 0
    void leanTreeSort(const std::uint8_t* rb, bool zpass);
    // RenderBranches / Fronds / Leafs (+ z passes): MaskReference = 80 with the property notifies (when it is not 80 yet)
    void leanMaskRef(const std::uint8_t* shader);
    // eCParticle_PS::Render's first steps (GetMasterInstance, the in-slot OnProcess, the inactivity timer): false = Render returns
    bool leanParticlePre(const std::uint8_t* ps);
    // ApplyRenderStates' creation / touches (CreateMaterial, RequestData, CreateImage + the sampler material's SetImage)
    void leanParticleStates(const std::uint8_t* ps);
    // committed with the op: the vegetation cursor, a particle target's in-place depth sort + our vertex cache, a class pool's
    // registers (eCShaderParticle / eCShaderLightStreaks global pools: the engine's composition reads them per op)
    struct LeanVeg {
        const std::uint8_t* admin = nullptr;
        std::uint32_t cursor = 0;
    } leanVeg_;
    const std::uint8_t* leanParticleTarget_ = nullptr;
    struct LeanPool {
        const std::uint8_t* var = nullptr;  // the class's ms_pGlobalShaderConstants (its pool +0x38 = the registers from +0x24)
        std::uint32_t count = 0;
        float regs[32][4]{};
    } leanPool_;
    void leanPoolFrom(const std::uint8_t* var, std::uint32_t count);  // fxPool_'s first `count` registers, written at the commit
    // a particle target's vertices of this frame, made by us (the engine's UpdateVertices did not run: its +0x3b0 stays)
    struct ParticleCache {
        std::uint64_t frame = ~0ull;
        std::uint32_t quads = 0;
        std::vector<std::uint8_t> vb;  // the vertices, or (gpu) the particle records
        bool gpu = false;
        d3d9::particle::OpConsts k;
    };
    std::unordered_map<const void*, ParticleCache, PtrHash> particleCache_;
    std::vector<std::uint8_t> leanParticleVb_;
    std::uint32_t leanParticleQuads_ = 0;
    // engine functions of the O2b path (resolved by verifyLeanAnim)
    bool (*updateRenderMatrices_)(const void* wrapper, std::uint32_t frame) = nullptr;
    void* (*particleModule_)() = nullptr;
    void* (*getMasterInstance_)(void* module, const void* ps) = nullptr;
    void (*depthSortParticles_)(const void* ps) = nullptr;
    bool (*particleCreateVertices_)(const void* ps) = nullptr;
    bool (*particleCreateMaterial_)(const void* ps) = nullptr;
    bool (*particleCreateImage_)(const void* ps) = nullptr;
    bool (*stripCreateVertices_)(const void* ps) = nullptr;
    bool (*streaksCreateBuffers_)(const void* ps) = nullptr;
    const void* maskRefProperty_ = nullptr;  // the MaskReference property the tree paths' notifies name (0x97bb98)
    // O2b: on a lean frame the engine's global / lighting pools lag behind (the lean ops do not write them): before engine code
    // runs, ours are copied into them (so it starts from what lean off would have, and its changes are found by the diff)
    bool syncEnginePools();
    struct LeanStats {
        std::uint64_t frames = 0, checkFrames = 0;
        std::array<std::uint64_t, kGroups> skipped{}, kept{};
        std::uint64_t blockedVideo = 0, blockedEdge = 0, touches = 0, touchCalls = 0, lightmaps = 0, stream3 = 0, instanceSorts = 0, queueResets = 0;
        std::uint64_t placeholderBinds = 0, skyWrites = 0, creates = 0;
        std::int32_t queueMax = 0;
        // O2b
        std::uint64_t palettes = 0, treeSorts = 0, maskRefs = 0, vegCursors = 0, poolWrites = 0, poolSyncs = 0;
        std::uint64_t masters = 0, noMaster = 0, kicks = 0, kicksSpawned = 0, inactivity = 0, particleSorts = 0, particleCached = 0;
        std::uint64_t particleCreates = 0, stripCreates = 0, streakCreates = 0;
        std::uint64_t compiles = 0, specialCompiles = 0, compileFailures = 0;  // O2c: permutations compiled at the slot's place
    } leanWindow_, leanTotal_;
    // O2c: a missing permutation compiled by the engine's own functions where its slot would (the version Execute 0x6cc610's VS
    // 0x6cc000 / PS 0x6cb8b0 calls; ExecuteSpecialPass's version vtable +0x50 = 0x6ca6c0); true = compiled (the shader's pools
    // re-walked: our copy re-read), false = not compiled / not ours to do (the op is left to the engine)
    int compileVerified_ = 0;
    bool compilePass(const std::uint8_t* shader, const std::uint8_t* version, std::uint32_t flags);
    bool compileSpecial(const std::uint8_t* shader, const std::uint8_t* version, std::int32_t kind, bool instanced, bool& failed);
    bool compileAllowed(const std::uint8_t* version);
    void invalidatePools(const std::uint8_t* shader, const std::uint8_t* version);
    std::vector<const void*> placeholderImages_;  // distinct resources bound as the placeholder in the window (bounded)
    std::vector<std::string> placeholderNames_;   // the first ones' names, read while bound (a load may free them before the report)
    std::uint32_t videoLogged_ = 0, placeholderLogged_ = 0;
    void leanReport();
    // the cloud fields as the lean path last wrote them (reported; the sky object itself may be gone by then: a load)
    struct SkySnap {
        bool valid = false;
        float size[2]{}, scale[2][2]{}, dir[2]{}, scroller[2]{}, dome[4]{}, src[4]{};
    } leanSky_;
    // per-op working storage of the compositions, kept across ops (O1: no heap traffic per op once warm)
    struct Scratch {
        std::vector<sc::Pass> passes;  // materialDraws
        std::vector<sc::ZSub> zs;      // meshZDraws
        std::vector<sc::ZDraw> zplan;
        std::vector<std::uint8_t> inst;  // composeSolid's instances
        std::vector<sc::Instance> list;
        std::vector<sc::Batch> batches;
        std::vector<std::uint8_t> treeQueue;  // composeTree
        std::vector<sc::TreeInstance> treeInst;
        std::vector<sc::TreeBatch> treeBatches;
        std::vector<float> treeAlpha;
        std::vector<sc::GrassNode> grassPlan;  // composeVegetation
        std::vector<const std::uint8_t*> grassSources;
        std::vector<sc::GrassDraw> grassDraws;
        std::vector<std::uint8_t> grassBytes, grassSrc;
    } scr_;
    // issue's saved device registers (only the ranges the op's draws write are saved and restored)
    float savedVs_[sc::kVsRegs][4]{}, savedPs_[sc::kPsRegs][4]{};
    // N5.8 coverage: the last op met (or the non-op unit) and why it was left to the engine
    void opEnterImpl(const Op& op);
    // F2c-0 (frame-ownership.md 7.12; RVK_F2C_MEASURE=1): the owned-eligibility classification of a main-view static mesh op (counters only)
    void f2cClassify(const Op& op);
    const Op* f2cOp_ = nullptr;  // the op being measured
    enum : int { kLeftCompareOnly = 100, kLeftGroupOff, kLeftNotCovered, kLeftChecks };
    std::string_view leftName(Group g, int code);
    const Op* leftOp_ = nullptr;
    Group leftGroup_ = Group::Count;
    int leftCode_ = -1;  // a Skip, or kLeft*
    std::string_view leftWhy_;
    bool leftNonOp_ = false;
    std::string_view leftNonOpWhy_;
    std::map<int, std::string> leftNames_;

    // ---- V3 retained composition (static_ops_retained.cpp) ----
    struct RetKey {
        static constexpr std::size_t kWords = 44;
        std::array<std::uint64_t, kWords> w{};
        std::uint64_t hash = 0;
        bool operator==(const RetKey& o) const { return w == o.w; }
    };
    struct RetSwitchWrite {
        std::uint8_t kind = 0;  // 0 VS pool register, 1 PS pool register, 2 sampler slot image
        std::uint32_t idx = 0;
        float v[4]{};
        const void* image = nullptr;
    };
    struct RetImage {
        const void* res = nullptr;
        const void* data = nullptr;
    };
    struct RetTex {
        const void* imageData = nullptr;
        const void* texture = nullptr;
        bool ok = false;
    };
    struct RetPass {
        const std::uint8_t* owner = nullptr;
        const std::uint8_t* bias = nullptr;
        sc::Pass pass;
        sc::DrawDesc* desc = nullptr;
        std::uint32_t sw0 = 0, sw1 = 0, im0 = 0, im1 = 0;
        bool special = false, hasTag = false, setMask = false;
        float mask = 0;
    };
    struct RetEntry {
        RetKey key;
        std::vector<ret::ReadRec> reads;
        std::vector<RetPass> passes;
        std::vector<RetSwitchWrite> sw;
        std::vector<RetImage> imgs;
        std::vector<RetTex> tex;
        ret::ReadLog log;
        const std::uint8_t* shader = nullptr;
        std::uint32_t vsFirst = 0, psFirst = 0, vsCount = 0, psCount = 0, slots = 0, lightsTotal = 0;
        std::array<const void*, 16> slotImage{};
        std::uint64_t texGen = 0, usedSerial = ~0ull, lastFrame = 0;
        std::uint8_t group = 0;
        bool noRetain = false;
    };
    struct RetStats {
        std::uint64_t hits = 0, built = 0, invalid = 0, busy = 0, skipped = 0, checks = 0, stale = 0, draws = 0, evicted = 0;
        std::uint64_t replayNs = 0, recordNs = 0;
    };
    bool retain_ = false, retainCheck_ = false;
    std::uint32_t retainGroups_ = 0x2ff;
    RetEntry* rec_ = nullptr;  // the entry being recorded (null: not recording)
    std::unordered_map<std::uint64_t, std::unique_ptr<RetEntry>> retained_;
    std::array<RetStats, kGroups> retWindow_{}, retTotal_{};
    std::uint64_t retFrame_ = ~0ull, retWindowFrames_ = 0, retTotalFrames_ = 0, retStaleLogged_ = 0;
    bool retainOn(std::uint8_t flags) const;
    template <class F>
    Skip retainedRun(RetKey& key, const std::uint8_t* shader, bool special, F&& impl);
    bool retValid(RetEntry& e);
    void retReplay(RetEntry& e);
    void retFree(std::unique_ptr<RetEntry> e);
    void retFrameBegin();
    void retReport();

    hal::vulkan::HalDevice& hal_;
    d3d9::Device& dev_;
    const void* curInstance_ = nullptr;  // 14.26: the render instance being composed (composeSolid's instance-list loop), copied into its draws
    std::array<GroupState, kGroups> groups_;
    GroupState* g_ = nullptr;  // the composed op's group (null: none)
    bool overlays_ = false;
    // the device's bool / int registers at the op's entry (the composition of those registers)
    std::array<std::uint32_t, 16> entryVsB_{}, entryPsB_{};
    std::int32_t entryVsI_[16][4]{};
    std::int32_t entryPsI_[16][4]{};
    int verified_ = 0;  // engine checks at first use: 1 ok, -1 failed (nothing composed)
    bool verify();
    const std::uint8_t* engine_ = nullptr;  // Engine.dll base
    const std::uint8_t* lightingPoolVar_ = nullptr;  // &DAT_180c62968 (decoded from FUN_1806cd1d0)
    const std::uint8_t* placeholderTable_ = nullptr;  // eCImageResource2's per-stage placeholder images (from 0x68c650)
    const std::uint8_t* imageOverride_ = nullptr;     // eCGfxImageData::Execute's per-stage override table (from 0x7551c0)
    using QsortFn = void (*)(void*, std::size_t, std::size_t, int (*)(const void*, const void*));
    QsortFn qsort_ = nullptr;
    int (*instanceCmp_)(const void*, const void*) = nullptr;

    void* db_ = nullptr;
    void* desc_ = nullptr;
    bool inView_ = false;
    // the view being executed (N5.5): the main view or a shadow map's ExecuteShadowMap (its eCShadowMapBase, eCDepthMap format +8)
    ViewKind viewKind_ = ViewKind::Main;
    const void* shadowMap_ = nullptr;
    std::int32_t shadowFormat_ = -1, cascade_ = -1;
    // the shadow views' compared / equal draws and ops per view kind x op kind (window, total)
    struct ShadowStats {
        std::array<std::array<std::uint64_t, kOpKindCount>, kViewKindCount> ops{}, draws{}, clean{}, composed{};
        std::array<std::uint64_t, kViewKindCount> views{};
        void add(const ShadowStats& o);
    } shadowWindow_, shadowTotal_;
    View view_;
    Mirror mirror_;
    std::unique_ptr<sc::DrawDesc> scratch_;    // a capture
    std::vector<sc::DrawDesc*> draws_;  // the op's composed draws (not owning: transient ones from descStore_, or a retained entry's)
    std::vector<std::unique_ptr<sc::DrawDesc>> descStore_;  // every DrawDesc made (owning)
    std::vector<sc::DrawDesc*> descFree_, transient_;       // unused / handed out by newDraw for the current list
    sc::DrawDesc* takeDesc();
    void recycleTransient();
    std::size_t drawCount_ = 0;
    sc::DrawDesc& newDraw();
    const Op* op_ = nullptr;       // the op whose draws were composed (null: none / not composed)
    bool taken_ = false;           // its draws were issued by us
    std::uint32_t engineDraws_ = 0;
    bool issuing_ = false;
    bool opDiffLogged_ = false;
    std::uint32_t frameLogged_ = 0;  // -LogFrame: composed draws described
    std::unordered_map<const d3d9::Shader*, Live, PtrHash> live_;
    std::unordered_map<const void*, d3d9::Shader*, PtrHash> elementShader_;
    // V4: direct-mapped memos in front of the three pointer-keyed maps above / the mirror's (a draw looks up the same few shaders;
    // map nodes are stable, the pools memo is dropped where the map is cleared)
    template <class V>
    struct PtrMemo {
        struct Entry {
            const void* key = nullptr;
            V value{};
        };
        std::array<Entry, 16> e{};
        static std::size_t slot(const void* k) { return ((reinterpret_cast<std::uintptr_t>(k) >> 4) * 0x9E3779B97F4A7C15ull) >> 60; }
        V* find(const void* k) { Entry& x = e[slot(k)]; return x.key == k ? &x.value : nullptr; }
        void put(const void* k, V v) { e[slot(k)] = {k, v}; }
        void clear() { e = {}; }
    };
    PtrMemo<const Live*> liveMemo_;
    PtrMemo<d3d9::Shader*> elementMemo_;
    PtrMemo<Mirror::ShaderPools*> poolsMemo_;

    // frame-ownership.md 7.8 (F1a, native.owned_windows): the issued draws' Material windows (PS window 2, VS window 3) and view windows (window 0 of both
    // stages) point into persistent tables instead of ring uploads (d3d9::Device::setOwnedWindows); the override of the draw being issued
    bool ownedWindows_ = false;
    bool ownedObjects_ = false;
    bool sceneDraws_ = false;
    d3d9::OwnedWindows ownedWin_;
    void setOwnedFor(const sc::DrawDesc& d);

    // F2c-3 (static_ops_owned.cpp)
    bool ownedStatic_ = false, ownedTripped_ = false;
    int ownedCheck_ = 10;
    struct OwnedView {
        bool active = false, adminKnown = false, zColor = false;
        std::uint64_t frame = 0;
        const std::uint8_t* admin = nullptr;
        std::uint64_t viewStamp = 0;
    } ownedView_;
    std::array<std::vector<Op>, 2> ownedOps_;  // the ops that left the engine's z / solid list (sorted), until their pass begins
    // F3-1 (7.36): the owned view in progress (set at its begin): Main, or a Cascade whose op records carry `ownedViewKey_` = 1 + the cascade index (0 = the main view's) and whose
    // draws read view table slot kViewSlotCascade0 + index. Its statistics are kept apart from the main view's.
    bool ownedShadows_ = false, ownedShadowsTripped_ = false;
    // F6-1 (static_ops_actors_owned.cpp)
    bool ownedActors_ = false, ownedActorsTripped_ = false;
    struct ActorIn {  // one actor op's engine inputs (composeActorImpl's reads)
        const std::uint8_t *ps = nullptr, *entity = nullptr, *wrapper = nullptr, *res = nullptr, *hra = nullptr, *ai = nullptr, *prim = nullptr, *material = nullptr, *shader = nullptr,
                           *remap = nullptr, *visual = nullptr;
        std::uint32_t actorIdx = 0, primIdx = 0, lod = 0, primIndex = 0, boneStart = 0, bones = 0, targets = 0;
        std::int32_t variant = 0;
        std::uint8_t flags = 0;
        float weights[3] = {0.0f, 0.0f, 0.0f};
        std::uint32_t pick[3] = {0, 0, 0};
    };
    // the composition's state for the draws it is about to issue (setOwnedFor serves the bone window from it): set by composeActorImpl
    struct ActorCur {
        bool valid = false;
        const void* wrapper = nullptr;
        std::uint32_t prim = 0, bones = 0;
        std::uint32_t draws = 0, offered = 0;  // the op's draws / those whose window 2 was served from the bone table
        // (F6-2: per draw index, the last setOwnedFor call decides: a draw whose packet replay was refused is set up twice, the fresh path's call counts)
        std::uint64_t drawMask = 0, offeredMask = 0;
        float weights[3] = {0.0f, 0.0f, 0.0f};  // c31.xyz as the composition wrote them
        float fade = 1.0f;                      // c32.w
        std::uint64_t histTag = 0;              // 14.13: the bone history's tag (actorHistTag)
    } actorCur_;
    // 14.13 (native.bone_motion): what makes two frames' bone images of a part comparable: the HRA (resource), its LOD, the morph pick (the streams the weights refer to)
    static std::uint64_t actorHistTag(const void* hra, std::uint32_t lod, const std::uint32_t (&pick)[3]) {
        std::uint64_t h = (reinterpret_cast<std::uintptr_t>(hra) >> 3) * 0x9E3779B97F4A7C15ull;
        for (const std::uint32_t v : {lod, pick[0], pick[1], pick[2]}) h = (h ^ v) * 0x100000001B3ull;
        return h;
    }
    struct PickMemo {
        std::uint32_t targets = 0;
        float weights[3] = {};
        std::uint32_t pick[3] = {};
    };
    std::unordered_map<const void*, PickMemo> pickMemo_;
    std::uint64_t pickMemoFrame_ = ~0ull;
    // the actor instance's three strongest morph targets of this frame (memoised per instance and frame: stamps, records and the composition see ONE pick; F-INT 7.43)
    bool actorPick(const std::uint8_t* ai, const std::uint8_t* setup, std::uint32_t targets, float (&weights)[3], std::uint32_t (&pick)[3]);
    int actorRead(const Op& op, ActorIn& in, bool weights);  // 0 = ok, 1 = not composable, 2 = a template property set, 3 = the resource is not cached
    std::string actorDiffText(const Op& op, const orec::DrawRec& r);  // the DIFFERING detail of an actor draw: lod, pick, the visual's streams, the record's
    orec::Excl recExclusionActor(const Op& op);
    bool recStampActor(const Op& op, orec::Stamp& st);
    // the bone image of an op from the engine's palette (frame id as the composition's): c31 weights, c32.w fade, the bone rows; false = a palette entry is unreadable
    bool actorImage(const Op& op, const ActorIn& in, float (*regs)[4], std::uint32_t& count);
    gpuscene::BoneKey actorKeyOf(const Op& op, const ActorIn& in) const;
    // the bone window the owned draw reads (serves the image once per frame and key); false = refused (`why`: a Fallback code)
    bool ownedActorWindow(const Op& op, const orec::OpRecord& rec, d3d9::OwnedWindows::Win& out, int& why);
    // the object / bone window of a record's draws for the check frames' derivations
    bool ownedObjWindow(const orec::OpRecord& rec, const orec::DrawRec& r, d3d9::OwnedWindows::Win& out) const;
    // F2f (static_ops_inst_owned.cpp)
    std::uint64_t instanceStamp(const std::uint8_t* res);              // the render instance list as the composition will read it (sorted), hashed
    orec::Excl instanceExclusion(const std::uint8_t* res);              // Excl::Count = an ownable instance op
    bool instanceUnbatch() const;                                       // composeSolid draws the hardware batches per instance
    void instanceServeComposed(const sc::DrawDesc& d);                  // setOwnedFor: window 2 of a composed instance draw = its record's row image
    bool instanceRows(orec::OpRecord& rec);                             // recEnd: every draw's row image (false: the op is not retainable)
    void instanceSideEffects(const orec::OpRecord& rec);                // the slot's render-instance sort
    void instReport();
    struct InstStats {
        std::uint64_t ops = 0, unbatchedOps = 0, unbatchedInstances = 0, recordsBuilt = 0, rowsRefused = 0, served = 0, notServed = 0, sorts = 0, ownedOps = 0, ownedDraws = 0;
        std::uint64_t excluded[orec::kExclCount] = {};
        std::uint64_t stampNs = 0;
    };
    InstStats instW_;
    bool ownedInstances_ = false;
    // F9 / F14 (static_ops_ordered.cpp)
    bool ownedOrdered_ = false;
    bool orderedGroupNow(const Op* op) const;                             // the op being composed is one the ordered path records (Mesh group alpha / refracted, main view)
    bool orderedPlace(const Op& op);                                     // opEnterImpl: true = drawn from its records at its place (the slot is skipped)
    int orderedValid(const Op& op, const orec::OpRecord& rec);           // -1 = the record holds for this op now, else the reason (OrdWhy)
    bool orderedPassBegin(const Op& op, const orec::OpRecord& rec);      // the view windows / slot / scope of the op's list (once per list and view)
    void orderedReport();
    struct OrderedRun {
        bool active = false;
        std::uint64_t serial = 0, epoch = 0;
        std::uint32_t slot = 0;
    } ordRun_;
    std::vector<d3d9::Device::OwnedItem> ordItems_;
    d3d9::Device::OwnedArena ordArena_;
    std::uint64_t ordSerial_ = 0;
    struct OrderedPass {
        std::uint64_t serial = 0;
        std::uint64_t frame = ~0ull, viewSerial = 0;
        std::uint8_t list = 0xff;
        bool ok = false;
        std::uint32_t slot[2] = {};       // the plain view's slot, the depth-bias view-projection's (F14: depth-bias materials)
        bool okDb = false;
        std::size_t envIndex = 0;
        std::uint64_t viewStamp = 0;
        d3d9::OwnedWindows viewWin[2];
    } ordPass_;
    struct OrderedStats {
        // per list: 0 solid, 1 refracted, 2 alpha
        std::uint64_t seen[3] = {}, owned[3] = {}, draws[3] = {}, composed[3] = {}, noRecord[3] = {}, notOrdered[3] = {};
        std::uint64_t why[3][12] = {};      // OrdWhy: why a recorded op was composed instead
        std::uint64_t excluded[3][orec::kExclCount] = {};  // recEnter / recComposed: not recorded, by reason
        std::uint64_t flushes = 0, buckets = 0, flushFailed = 0, envNew = 0, passes = 0, recordsBuilt[3] = {};
        std::uint64_t drawWhy[3][4] = {};   // a draw refused: ownedDrawOpPersistent's fallback (0 view, 1 materials, 2 object window, 3 a draw refused)
        std::uint64_t devWhy[3][24] = {};   // ... of the refused draws: the device's derivation codes (ownedReplayStats::why)
        std::uint64_t observer[3] = {}, tableFull[3] = {}, notRetained[3] = {};
        std::uint64_t runs = 0, lostDraws = 0, flushNs = 0, validNs = 0, prepNs = 0, drawNs = 0, sideNs = 0;
        std::uint64_t ns = 0;
    } ordW_;
    std::uint64_t ordTotalOwned_ = 0;  // (the run's ordered ops owned: the stage line's window difference)
    std::int32_t persistEnvIndex_ = -1;  // (ownedDrawOpPersistent: the env index the draw's Persist is stamped with; -1 = the view slot's)
    std::vector<const void*> recLightmaps_;  // (F9 / F14: the BindLightmaps(1) calls of a composition a record is built from)
    std::uint32_t ownedDrawIndex_ = 0;  // (the composed draw setOwnedFor is called for)
    void actorServeComposed(const sc::DrawDesc& d, d3d9::OwnedWindows::Win& out, bool& served);
    // check frames (recDraw): the image the composed draw wrote into the bone table equals one built from the engine's palette now
    void actorVerify(const Op& op, const orec::OpRecord& rec, bool& differs, std::string& detail);
    void actorReport();
    struct ActorOwnedStats {
        std::uint64_t selected = 0, drawn = 0, refusedWindow = 0, recordsBuilt = 0, imageBuilds = 0, imageShared = 0, verified = 0, verifyDiffers = 0, offeredDraws = 0, composedDraws = 0;
        std::uint64_t excl[orec::kExclCount] = {};
        std::uint64_t notRetained = 0, notOffered = 0, notComposable = 0, template_ = 0, notCached = 0, rebuilds = 0;
        std::uint64_t byView[3] = {};  // drawn ops: main z / main solid / cascades
        std::uint64_t seenByView[3] = {};
        std::uint64_t morphOps = 0, morphPickChanged = 0, imageNs = 0, stampNs = 0, drawsBuilt = 0, drawNoScene = 0, drawCdInvalid = 0, morphActive = 0, stampRebuilds = 0;
        // F6-2 census (frame-ownership.md 14.23): every actor op per view class (main / cascade / other) x list (z / solid / alpha / refracted): owned, or composed with its reason
        // (orec::Excl, then kActorCensusExtra)
        std::uint64_t cenOwned[3][4] = {};
        std::uint64_t ordSeen = 0, ordOwned = 0;  // actor ops with an ordered record looked at / drawn at their place (orderedPlace)
        std::uint64_t pickSwaps = 0, pickMisses = 0, pickKept = 0;  // morph pick sets (actorPickSwap / actorPickKeep)
        std::uint64_t cenWhy[3][4][orec::kExclCount + 6] = {};
        void add(const ActorOwnedStats& o);
    } actorW_, actorT_;
    // the census's reasons after orec::Excl: why an actor op was composed
    enum ActorCensusExtra : std::uint8_t { kCenCheck, kCenRecordRefused, kCenNoRecord, kCenAlphaList, kCenRefractedEmpty, kCenNotRecordedView, kCenExtraCount };
    void actorCensus(const Op& op);           // opEnterImpl: an actor op that is composed (the owned ones never get there)
    bool actorPickSwap(orec::OpRecord& rec, std::uint64_t pick);     // F6-2: the draws of an earlier morph pick swapped in (the stamp's lightmaps = the pick)
    void actorPickKeep(orec::OpRecord& fresh, orec::OpRecord& old);  // F6-2: a record rebuilt for a new pick keeps the old pick's draws
    void actorCensusOwned(const Op& op);      // an actor op drawn from its records
    ViewKind ownedCur_ = ViewKind::Main;
    // F3-1: the cascades' check / warm-up frames count the frames a cascade was rendered in (debug.shadow_stride renders them every 2nd frame while the camera stands still: the
    // frame number's check frames (multiples of native.owned_check) would never meet a cascade view); `ownedShadowCheck_` = the cascade view in progress is a check view
    std::uint64_t shadowFrameSerial_ = 0, shadowFrameSeen_ = ~0ull;
    bool ownedShadowCheck_ = false;
    std::uint64_t ownedViewKey_ = 0;
    // the op records' view key of a view: 0 main, 1 + c sun cascade c, kKeyTerrain the terrain map, kKeyAtlas0 + i atlas cell i (F3-4: only while owned_shadow_views is on); 0 = not owned
    std::uint64_t ownedKeyOf(ViewKind k, std::int32_t cascade, std::int32_t cell = 0) const {
        if (k == ViewKind::Cascade) return cascade >= 0 && cascade < 4 ? static_cast<std::uint64_t>(cascade) + 1 : 0;
        if (!ownedShadowViews_) return 0;
        if (k == ViewKind::Terrain) return cell == 0 ? kKeyTerrain : 0;
        if (k == ViewKind::PointAtlas) return cell >= 0 && cell < kAtlasCells ? kKeyAtlas0 + static_cast<std::uint64_t>(cell) : 0;
        return 0;
    }
    std::uint64_t ownedKeyNow() const { return ownedKeyOf(viewKind_, cascade_, cell_); }
    // the packet cache's view part of a draw key (the same entity is drawn into several cascades / maps; F3-4: the atlas cells share one map object, so with
    // owned_shadow_views the cell is part of the key and each cell's records find their own packets)
    std::uint64_t packetViewKey() const {
        return (static_cast<std::uint64_t>(viewKind_) << 32) ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cascade_)) << 36) ^
               (reinterpret_cast<std::uintptr_t>(shadowMap_) * 0xD6E8FEB86659FD93ull) ^ (ownedShadowViews_ && viewKind_ == ViewKind::PointAtlas ? static_cast<std::uint64_t>(cell_ + 1) << 52 : 0u);
    }
    // (F3-4: the retained compositions' key of an atlas cell, while owned_shadow_views is on: each cell keeps its own)
    std::uint64_t retainCellBits() const { return ownedShadowViews_ && viewKind_ == ViewKind::PointAtlas ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(cell_ + 1) & 0xffu) << 48 : 0u; }
    // the view kind of a record key (Main for 0)
    static ViewKind kindOfKey(std::uint64_t key) { return key == 0 ? ViewKind::Main : key < kKeyTerrain ? ViewKind::Cascade : key == kKeyTerrain ? ViewKind::Terrain : ViewKind::PointAtlas; }
    static std::uint32_t ownedSlotOfKey(std::uint64_t viewKey, bool solid);
    static std::uint32_t treeSlotOfKey(std::uint64_t viewKey, bool solid);
    bool ownedBegin(void* desc, ViewKind kind, std::int32_t cascade, std::int32_t cell = 0);
    bool ownedShadowViews_ = false;
    std::int32_t cell_ = 0;  // the view's index among the frame's views of its kind (FrameDesc::view - 1)
    std::uint64_t atlasBeyond_ = 0;  // (atlas cells of the window beyond kAtlasCells: composed)
    // why a view is not owned (ownedBegin's readiness; the first unmet condition): counted per 5 s window, logged once per change, named in the stage line
    enum class OwnedGate : std::uint8_t { Records, SceneDraws, Windows, ObjectsOff, ObjectsPending, LightingOff, LightingPending, MaterialTable, TableLighting, Observer, NotVerified, Count };
    static constexpr std::size_t kGateCount = static_cast<std::size_t>(OwnedGate::Count);
    static constexpr const char* kGateNames[kGateCount] = {"native.owned_records off", "scene draws off (native.scene_draws)", "owned windows off (native.owned_windows)",
                                                           "native.owned_objects off", "object table not ready", "native.lighting_images off", "lighting images pending",
                                                           "native.material_table off", "table lighting off (lighting.clustered + lighting.full_lit)", "observer", "static ops not verified"};
    int ownedGateCur_[2] = {-2, -2};  // per view kind (main, cascades): the last gate logged (-1 = open, -2 = none yet)
    std::string ownedGateObs_;        // the gating observer's reason (OwnedGate::Observer)
    std::uint64_t ownedGateSince_[2] = {};
    struct OwnedStats;
    std::string ownedGateText(const OwnedStats& w) const;  // "" = no view gated in the window
    struct OwnedStats {
        static constexpr std::size_t kFallback = 8;
        std::uint64_t views = 0, owned = 0, drawnOps = 0, draws = 0, lateOps = 0, lateDraws = 0, undrawn = 0;
        std::uint64_t checkViews = 0, warmupViews = 0, gatedViews = 0, leanFrames = 0;
        std::uint64_t gated[kGateCount] = {};                     // gated views by reason
        std::uint64_t seen = 0;                                   // ops of the lists that were looked at (mesh ops)
        std::uint64_t leftExcl[orec::kExclCount] = {};            // not eligible, by reason
        std::uint64_t leftStamp[orec::kStampFields] = {};         // the record's stamp differs, by first field
        std::uint64_t leftNoRecord = 0, leftView = 0, leftAudit = 0, leftNotOwnable = 0, leftScene = 0, leftOrdered = 0;
        std::uint64_t fallback[kFallback] = {};                   // owned ops composed late: materials, view, object window, refused draw, ...
        static constexpr std::size_t kLate = 20;
        std::uint64_t late[kLate] = {};                           // F15-1: the same ops by root reason (static_ops_owned.cpp kLateNames)
        std::uint64_t touches = 0, lightmaps = 0, stream3 = 0;
        std::uint64_t compactNs = 0, selectNs = 0, buildNs = 0, submitNs = 0, sideNs = 0, matNs = 0, prepNs = 0;
        void add(const OwnedStats& o);
    } ownedW_, ownedT_, ownedShadowW_, ownedShadowT_;
    // F3-4: the terrain map's [0] and the point-light atlas's [1] views
    OwnedStats ownedLightW_[2], ownedLightT_[2];
    static std::size_t lightIndex(ViewKind k) { return k == ViewKind::Terrain ? 0 : 1; }
    OwnedStats& ownedStats() { return ownedCur_ == ViewKind::Main ? ownedW_ : ownedCur_ == ViewKind::Cascade ? ownedShadowW_ : ownedLightW_[lightIndex(ownedCur_)]; }
    // F15-1 (frame-ownership.md 14.20): the root reason of the refusal being made (set where an owned op is refused, read by composeLate) and the 1 s census
    int lateDetail_ = 0;
    struct LateSecond {
        std::uint64_t ops[2] = {}, views[2] = {}, why[OwnedStats::kLate] = {};
    } late1s_;
    std::chrono::steady_clock::time_point late1sAt_{}, lateStart_{};
    void lateSecondReport();
    d3d9::Device::OwnedReplayStats ownedDevLast_{};
    std::unordered_map<std::uint64_t, bool> ownedAmbient_;    // (lighting key -> ambient registers non-zero), per pass
    std::vector<std::uint64_t> ownedPolled_;                  // material slot -> frame + 1 of the last poll made for an owned op
    std::uint32_t ownedLogged_ = 0;
    std::uint64_t ownedIndirectLast_ = 0;
    struct OwnedWinCache {  // per pass: the material windows of a slot, the lighting window base of a (slot, params)
        std::uint64_t frame = ~0ull;
        std::unordered_map<std::uint32_t, d3d9::OwnedWindows> mat;
        std::unordered_map<std::uint64_t, std::uint32_t> light;
    } ownedCache_;
    bool ownedDrawOp(const Op& op, const orec::OpRecord& rec, const d3d9::OwnedWindows& viewWin, bool viewWinOk, int& why);
    // F2c-4a (static_ops_owned.cpp)
    bool ownedPersistent_ = false;
    struct PersistStats {
        std::uint64_t ops = 0, draws = 0, reused = 0, derived = 0, derivedChanged = 0, tableFull = 0, refused = 0, observerRefused = 0, injected = 0;
        std::uint64_t scrubbed = 0, scrubMiss = 0, checkCompared = 0, checkSkipped = 0, checkMiss = 0, passes = 0, envChanges = 0, flushFailed = 0, arenaFailed = 0;
        std::uint64_t sigNs = 0, deriveNs = 0, observeNs = 0, flushNs = 0, windowNs = 0, items = 0, bulkNs = 0;
        void add(const PersistStats& o);
    } persistW_, persistT_, persistShadowW_, persistShadowT_;
    PersistStats persistLightW_[2], persistLightT_[2];  // F3-4: terrain map / atlas cells
    PersistStats& persistOfKind(ViewKind k) { return k == ViewKind::Main ? persistW_ : k == ViewKind::Cascade ? persistShadowW_ : persistLightW_[lightIndex(k)]; }
    PersistStats& persistStats() { return persistOfKind(ownedCur_); }
    std::uint64_t persistEnvLast_[64] = {};
    void ownedShadowsOff(const char* why, std::uint64_t n);
    void ownedShadowReport();
    void ownedShadowReportOne(ViewKind kind, OwnedStats& w, OwnedStats& t, PersistStats& q, PersistStats& qt);
    void dropLightViewRecords();  // F3-4: the terrain map's / atlas cells' records and tree templates (owned_shadow_views went off)
    std::uint64_t persistMissReported_ = 0, persistInjectSeen_ = 0, persistInjectEvery_ = 0, persistMissLogged_ = 0;
    std::uint64_t persistScrubTick_ = 0;
    std::uint32_t persistPassDraw_ = 0;
    void ownedAutoOffMiss(std::uint64_t miss);
    struct MatWinEntry {  // per material slot and pass: the two windows the material record serves (a flat cache in front of ownedCache_.mat)
        d3d9::OwnedWindows::Win v3, p2;
        std::uint64_t pass = 0;
    };
    std::vector<MatWinEntry> ownedMatWin_;
    std::uint64_t ownedPassSerial_ = 0;
    std::vector<d3d9::Device::OwnedItem> ownedItems_;
    std::vector<std::uint32_t> ownedItemOp_;
    std::vector<const orec::OpRecord*> ownedRecPtrs_;
    d3d9::Device::OwnedArena ownedArena_;
    bool ownedDrawOpPersistent(const Op& op, const orec::OpRecord& rec, const d3d9::OwnedWindows& viewWin, std::uint32_t viewSlot, std::uint64_t epoch, std::uint32_t scrubStride, std::uint32_t scrubPhase, int& why);
    void ownedBuildWindows(const orec::DrawRec& r, const d3d9::OwnedWindows& viewWin, const d3d9::OwnedWindows::Win& objWin, const gpuscene::ComposeParams& p, bool cached, d3d9::OwnedWindows& dw);
    bool ownedAmbientLit(const orec::DrawRec& r, const gpuscene::ComposeParams& p);
    void ownedMakeIn(const orec::DrawRec& r, bool ambientLit, d3d9::Device::DeriveIn& in);
    void ownedMissReport(const char* where, const Op* op, const orec::DrawRec& r, const d3d9::Device::OwnedPersist& fresh, std::uint32_t slot);
public:
    // check frames (static_ops_records.cpp recDraw): the persistent content of the draw against the one the composition's derivation gives, when the owned path would reuse it
    void ownedMissCheck(const orec::OpRecord& rec, std::size_t k, const d3d9::Device::DeriveIn& in, const d3d9::Device::SceneView& derived);
private:
    bool ownedMaterialsOk(const orec::OpRecord& rec);
    bool ownedAuditOk(const Op& op, const orec::OpRecord& rec);
    void ownedSideEffects(const orec::OpRecord& rec);
    void ownedReport();
    void ownedAutoOff(std::uint64_t differing);
    orec::Excl recExclusion(const Op& op);
    static std::uint64_t viewStampOf(const std::uint8_t* admin, bool zColorWrite, bool fog);

    // F2c-2 (static_ops_records.cpp)
    bool ownedRecords_ = false;
    std::unique_ptr<orec::Store> recs_;
    struct LightKey {
        std::uint32_t slot = 0xffffffffu;
        bool tableLit = false, holdsAll = false;
        std::uint32_t n = 0, first = 0;
    } lightKey_;
    bool recNoRetain_ = false;
    bool recCollect_ = false;  // F2c-3: the composition of the eligible op being made collects its touches / stream 3 switches for the record (also off the lean frames)
    std::uint64_t recViewSerial_ = 0;
    void recEnter(const Op& op);          // before the composition: eligibility, stamps, the record valid at entry, the read log
    void recComposed(const Op& op, Skip s);  // after the composition
    bool recStamp(const Op& op, orec::Stamp& st);
    void recDraw(std::size_t k, const sc::DrawDesc& d, std::uint64_t pkey);  // after each issued draw: derive + compare (or collect for the build)
    void recEnd(const Op& op);            // after issue(): build / refresh the record
    void recReport();
    bool recPost(const Op& op);           // at the first draw: stamps that need the composition (material versions, view)
    void recAudit(const Op& op);
    int recLiveWhy(const orec::OpRecord& rec);  // what the stamps do not see: -1 = fine, else the rebuild reason index (kStampFields + 4: illumination slot gone, + 5: a load completed)
    std::uint64_t recViewStamp();

    // F2e-1 step A (static_ops_deps.cpp, frame-ownership.md 7.30): event-driven validity of the op records
    struct DepStats {
        std::uint64_t objects = 0, marked = 0, fast = 0, slow = 0, slowClean = 0, scrubbed = 0, eventMiss = 0, shaderPolls = 0, shaderHits = 0, polledLeft = 0, epochs = 0, checkSeen = 0;
        void add(const DepStats& o) {
            objects += o.objects; marked += o.marked; fast += o.fast; slow += o.slow; slowClean += o.slowClean; scrubbed += o.scrubbed; eventMiss += o.eventMiss;
            shaderPolls += o.shaderPolls; shaderHits += o.shaderHits; polledLeft += o.polledLeft; epochs += o.epochs; checkSeen += o.checkSeen;
        }
    } depW_, depT_;
    struct ShaderMemo {
        const void* p = nullptr;
        std::uint64_t frame = ~0ull, fp = 0;
    };
    ShaderMemo shaderMemo_[256];
    std::vector<const void*> depDrained_;
    std::uint64_t depEpochSeen_ = 0, depEventMissLogged_ = 0;
    bool depEvents() const;                                    // the dependency events are installed (and not switched off): the fast path may run
    std::uint64_t shaderFp(const std::uint8_t* shader);        // a shader's property fields as the stamp reads them, memoised per frame
    void depDrain();                                           // start of a main view: the queued engine events mark the records that registered the object
    void depRegister(orec::OpRecord& rec, const Op& op);       // at the build: the engine objects the stamp hangs on, the covered shaders' fingerprints
    int depFast(const Op& op, const orec::OpRecord& rec);      // the polled inputs of a record no event touched: -1 valid, -2 verify the full stamp, else the stamp field that differs
    bool depShadersChanged(const orec::OpRecord& rec);         // a covered shader's fields differ from the fingerprints (a polled input, never an event miss)
    void depEventMiss(const Op& op, const orec::OpRecord& rec, int field, const char* where);
    void depReport();
public:
    bool depClean(const orec::OpRecord& rec) const;            // no engine event marked the record since its stamp was last verified
    void depVerified(const orec::OpRecord& rec);               // the full stamp held: clear the mark
    bool depNoteCheckFrame(const Op& op, const orec::OpRecord& rec, int field);  // recEnter: a stamp field differs on a full compare; true = it was an event miss (counted)
private:

    // F4-1 (static_ops_bb.cpp)
    bool ownedBb_ = false;
    std::unique_ptr<bbrec::Store> bb_;
    bool bbStamp(const Op& op, orec::Stamp& st, const void*& ps, const void*& list, std::uint32_t& first, std::uint32_t& count);
    void bbEnter(const Op& op);               // before the composition: eligibility, stamps, the record valid at entry, sources, the read log
    void bbComposed(const Op& op, Skip s);    // after the composition
    void bbDraw(std::size_t k, const sc::DrawDesc& d, std::uint64_t pkey);  // after each issued draw: derive + compare
    void bbEnd(const Op& op);                 // after issue(): build / refresh the record
    void bbAudit(const Op& op);
    void bbDeriveIn(const orec::DrawRec& r, d3d9::Device::DeriveIn& in);
    bool bbSceneIssue() const;                // the op being issued is a billboard op that takes scene draws
    void bbReport();
    bool bbReplay(const Op& op);              // owned frames: the op's draws submitted from its record (true: done, the engine's slot is skipped by the commit)
    void bbNoteMat(const sc::DrawDesc& d, const std::uint8_t* owner, const void* material, bool zPass, std::uint16_t psMask, std::int32_t blendMode, std::int32_t maskRef);  // the composition's checkMaterial call
    bool bbPollMaterials(const bbrec::Record& rec);                              // the material poll of an owned op (checkMaterial without composing)
    void bbWindowsFor(const bbrec::Record& rec, std::size_t k, d3d9::OwnedWindows& dw);  // the draw's windows from tables / the view's mirror
    std::uint64_t bbViewStamp();
    void bbNoteLights(const std::uint8_t* illum);  // writeLighting's hand-over of an illumination's lights to the light table (what an owned solid op still does)
    bool bbCheckFrame();
    std::unique_ptr<sc::DrawDesc> bbScratch_;
    std::vector<d3d9::Device::DeriveIn> bbIns_;
    std::vector<d3d9::OwnedWindows> bbWins_;
    std::vector<std::uint64_t> bbPolled_;
    std::uint64_t bbPolledFrame_ = ~0ull;
    // packet retention: a composed draw's finished packet by (op, draw index), valid while the draw description equals the captured one
    int packetMode_ = 0;
    int gpuScene_ = 0;
    struct PacketEntry {
        d3d9::Device::CachedDraw cd;
        std::unique_ptr<sc::DrawDesc> desc;
    };
    std::unordered_map<std::uint64_t, PacketEntry> packets_;
    struct PacketStats {
        std::uint64_t replayed = 0, refused = 0, built = 0, notBuilt = 0, verified = 0, invalid = 0, mismatches = 0, changed = 0, fresh = 0, rebuilt = 0, staleDropped = 0;
    } pktWindow_, pktTotal_;
    std::uint32_t pktMismatchLogged_ = 0;
    std::uint64_t pktWhyPrev_[12] = {}, pktNcPrev_[12] = {};  // the device's refusal counters at the last report (the window = the difference)
    std::uint64_t pktGroup_[3][2] = {};                       // per cached group (main-view static, trees, shadow views) in the window: replayed / drawn fresh
    std::unique_ptr<d3d9::Device::CachedDraw> pktFresh_;
    static bool packetSameDesc(const sc::DrawDesc& a, const sc::DrawDesc& b);
    std::size_t issueIndex_ = 0;
    std::vector<std::uint8_t> ownKind_;
    // non-op paths (decals): the unit whose draws are composed in place of an op
    Op nonOp_;
    bool inNonOp_ = false;
    bool inSkyCube_ = false;
    // the sky cube's faces: UpdateSkydomeEnvCubemap's BeginMaterial binds the admin stages, then SetRenderTarget(face) unbinds all
    // 16 stages (eCDX9Device / HalDevice::SetRenderTarget): s0 / s1 / s2 / s4 / s5 are empty at the faces' draws
    bool adminStagesUnbound_ = false;
    int decalsVerified_ = 0, billboardsVerified_ = 0, skyVerified_ = 0, actorsVerified_ = 0, treesVerified_ = 0;
    // N5.4 (static_ops_trees.cpp): the admins' getters (FUN_18025a6d0 eCSpeedTreeAdmin, FUN_180265df0 eCVegetationAdmin), the queue
    // comparators (SortInstances 0x6f2180, SortInstancesZPass 0x6f2120), the billboards' fixed tex-coord table (0xbfcbd0)
    void* (*treeAdmin_)() = nullptr;
    void* (*vegAdmin_)() = nullptr;
    int (*treeCmp_)(const void*, const void*) = nullptr;
    int (*treeCmpZ_)(const void*, const void*) = nullptr;
    const std::uint8_t* bbTexCoords_ = nullptr;
    // the MaskReference a path writes into a shader before its draws (SpeedTree: 80): null none
    const std::uint8_t* maskRefShader_ = nullptr;
    std::uint8_t maskRefOverride_ = 0;
    std::uint8_t maskRefOf(const std::uint8_t* owner) const;
    const void* leafsVtable_ = nullptr;  // ??_7eCShaderLeafs (0x9d7be8), accepted by shaderClassOk once verifyTrees checked its slots
    std::string skipWhy_;                // the reason of the last skip (diagnostics)
    const void* skipShader_ = nullptr;   // the vtable of the last ShaderClass skip's shader
    std::uint32_t treeSkipsLogged_ = 0;
    const std::uint8_t* pendingVersion_ = nullptr;  // diagnostics of uncompiled special passes
    std::size_t pendingIdx_ = 0;
    std::uint32_t pendingLogged_ = 0;
    // admin VS registers c0..c30 composed natively in this op (bit r; c0..c11 and c30 always)
    std::uint32_t nativeVs_ = 0;
    struct TreeStats {
        std::uint64_t ops = 0, zeroOps = 0, hardware = 0, software = 0, instances = 0, batches = 0, sortedCopies = 0, undrawnBatches = 0;
        std::array<std::uint64_t, 4> types{};  // branches, fronds, leaf cards, leaf meshes
        std::array<std::uint64_t, 7> splits{};  // solid-pass tree batch cuts by reason: limit, strip, list, sun, static ambient / lights, other
        std::uint64_t knownLights = 0;  // tree instances whose static lights the table holds (their batch no longer ends on them)
        std::uint64_t leafLods = 0, wrongTransformation = 0, billboardOps = 0, billboardTexCoords = 0, shadowFrondsSkipped = 0;
        std::uint64_t grassOps = 0, grassZeroOps = 0, grassNodes = 0, grassInstances = 0, grassDraws = 0, grassWraps = 0, grassNotCached = 0, grassClockMoved = 0;
        std::uint64_t vegNodesNs = 0, vegConvNs = 0, vegIssueNs = 0, vegIssueOps = 0;  // F7-1 measurement: node walk, read + convert, issue() of vegetation ops
        std::array<std::uint64_t, 6> compNs{}, compOps{};  // F4-4: composeTree (main z / main solid / cascades) / vegetation (z / solid) / tree billboards: CPU ns + ops composed
    } treeWindow_, treeTotal_;
    // F4-2 / F4-3 (static_ops_trees_owned.cpp)
    bool ownedTrees_ = false;
    std::unique_ptr<tro::State> tro_;
    std::uint8_t treeViewKey() const;                  // the view in progress: 0 main, 1 + c a sun cascade, 255 none
    std::uint32_t treeSlot(bool solid) const;          // its tree view table slot (main 14 / 15, cascade c: 8 + c)
    void treeDropShadow();                             // the cascades' templates are dropped (native.owned_shadows went off)
    void treeDropLightViews();                         // F3-4: the terrain map's / atlas cells' templates are dropped (native.owned_shadow_views went off)
    void treeEmptyRecord(const Op& op, const std::uint8_t* res, const std::uint8_t* rb);  // F3-4: a shadow view's fronds op that draws nothing: an empty record (tro::OpRec::empty)
    bool treeEmptyHolds(const Op& op, const tro::OpRec& rec, const std::uint8_t*& rb) const;  // ... its rule still holds (the owned pass)
    bool treeSceneIssue() const { return ownedTrees_ && g_ == &groups_[static_cast<std::size_t>(Group::Trees)]; }
    static std::uint64_t treeBucketKey(std::uint32_t type, bool zpass, bool sun, std::uint32_t a, std::uint32_t b, std::uint32_t c);
    bool treeEnsureRamp();
    void treeCompBegin(const Op& op, bool zpass, std::uint32_t type, const std::uint8_t* rb, const std::uint8_t* material, const std::uint8_t* shader, bool hardware, bool tableMerge, const std::uint8_t* queue, std::int32_t n, const std::uint8_t* wrapper);
    void treeCompBatch(std::size_t drawsBefore, std::uint64_t key);
    void treeCompInstance(const std::uint8_t* tree, const std::uint8_t* illum, std::uint64_t key, bool sun, std::uint32_t slot);
    void treeCompEnd(const Op& op);
    bool treePlan(bool zpass, std::uint32_t type, bool tableMerge, const std::uint8_t* rb, const std::uint8_t* wrapper, const std::uint8_t* queue, std::int32_t n, std::vector<tro::Inst>& out, int& why);
    bool treeStampOf(const Op& op, tro::Stamp& st, std::uint32_t& type, const std::uint8_t*& rb, const std::uint8_t*& wrapper, const std::uint8_t*& shader, bool& zpass);
    int treeTemplate(tro::Tpl& t, const d3d9::OwnedWindows& viewWin, std::uint64_t epoch, bool solid, std::uint32_t scrubStride, std::uint32_t scrubPhase, tro::Stats& W, std::uint64_t& scrubTick, const char* what);
    void treeMissCheck(tro::OpRec& rec, tro::Tpl& t, bool solid);
    void treeReport();
    // F7-2 (static_ops_veg_owned.cpp)
    bool ownedVegOps_ = false;
    std::unique_ptr<vro::State> vro_;
    bool vegSceneIssue() const { return ownedVegOps_ && g_ == &groups_[static_cast<std::size_t>(Group::Vegetation)]; }
    bool vegStampOf(const Op& op, vro::Stamp& st, const std::uint8_t*& shader);
    bool vegIllumSun(const std::uint8_t* illum, bool& sun);
    bool vegEnsureStand();
    void vegCompBegin(const Op& op);
    void vegCompNote(const Op& op, const std::vector<sc::GrassDraw>& draws, const std::uint8_t* nodes, std::uint32_t total, bool wind, bool lift, float ax, float ay);
    void vegCompDraw(std::size_t drawsBefore, std::size_t k);
    void vegCompEnd(const Op& op);
    void vegConvert(vro::OpRec& rec, const std::vector<std::uint8_t>& raw, bool wind, bool lift, bool check, vro::Stats& W);
    void vegReport();
    void treeReportOne(tro::Stats& q, tro::Stats& total, const char* what, bool shadow);
    // the skinned actors (static_ops_actors.cpp): the frame admin getter eCAnimation_PS::Render calls first (frame id at +0x4c), the
    // affine product of UpdateRenderMatrices (FUN_1800a2710: a *= b)
    void* (*frameAdmin_)() = nullptr;
    void (*affineMul_)(float* a, const float* b) = nullptr;
    struct ActorStats {
        std::uint64_t ops = 0, palettesStored = 0, palettesComposed = 0, morphActive = 0, morphOps = 0, bodyParts = 0, bones = 0;
        std::array<std::uint64_t, 5> lod{};
        std::uint64_t emptySlot = 0;  // refracted-list ops: eCRenderContextAnimation2 has no RenderRefracted (0 draws)
        std::uint64_t overlayOps = 0;
        std::array<std::uint64_t, 3> overlayKinds{};  // burned, frozen, highlighted
    } actorWindow_, actorTotal_;
    std::uint32_t frameId_ = 0;
    int (*billboardCmp_)(const void*, const void*) = nullptr;
    // the BlendMode a non-op path writes into a shader before its passes (SetCurrentRenderStates reads +0x50): -1 none
    const std::uint8_t* blendOverrideShader_ = nullptr;
    std::int32_t blendOverride_ = -1;
    using MatInvertFn = void (*)(void* m);
    using MatMulFn = void* (*)(const void* a, void* ret, const void* b);
    using MatMulAssignFn = void* (*)(void* a, const void* b);
    using MatInvertedFn = void* (*)(const void* m, void* ret);
    MatInvertFn matInvert_ = nullptr;
    MatMulFn matMul_ = nullptr;
    MatMulAssignFn matMulAssign_ = nullptr;
    MatInvertedFn matInverted_ = nullptr;
    std::uint32_t skyLogged_ = 0;
    using VecFn = void (*)(void* vector, const void* matrix);
    VecFn transform_ = nullptr, transformDirection_ = nullptr;
    int (*decalCmp_)(const void*, const void*) = nullptr;
    float decalAlphaScale_ = 255.0f, one_ = 1.0f;  // DAT_1809f17b4, DAT_1809f1698
    std::unique_ptr<d3d9::Buffer> ownVb_, ownIb_;  // take-over: our own copies of the CPU-generated vertex / index data
    // take-over: the vegetation's instance data, appended within a frame (one ring copy per frame instead of one per DISCARD)
    static constexpr std::uint32_t kOwnInstBytes = 2u << 20;
    // take-over: our own dynamic VB for CPU-generated geometry on stream 0 (decals, billboards, sky objects, particles, strips, rain)
    static constexpr std::uint32_t kOwnVbBytes = 4u << 20;
    std::unique_ptr<d3d9::Buffer> ownInstVb_;
    // F7-1 retained vegetation instance streams (static_ops_trees.cpp): per (context, type, list)
    struct VegRet {
        std::vector<std::uint8_t> raw, conv;     // the flattened source entries of the last build, their converted bytes (check frames compare)
        std::unique_ptr<d3d9::Buffer> buf;       // the converted instances (dynamic buffer, DISCARD-rewritten when the source changed)
        std::uint32_t cap = 0;
        bool wind = false, lift = false;
        sc::GrassThin thin;                      // S-5: the thinning the buffer's bytes were made with
        std::uint64_t lastFrame = 0;
    };
    struct VegStats {
        std::uint64_t ops = 0, retained = 0, reused = 0, rebuilt = 0, rebuiltBytes = 0, fallback = 0, checks = 0, differing = 0, drawsRetained = 0, grown = 0;
    } vegStat_, vegStatTotal_;
    bool ownedVeg_ = false;
    std::map<std::tuple<const void*, std::uint32_t, std::uint32_t>, VegRet> vegRet_;
    std::vector<std::uint8_t> vegRaw_;
    VegRet* vegRetain(const Op& op, std::uint32_t total, bool wind, bool lift, const sc::GrassThin& thin);
    // S-5 (native.own_foliage)
    bool ownFoliage_ = false;
    FoliageStats foliageW_, foliageTotal_;
    sc::GrassThin grassThinOf(float gridEnd) const;  // off unless own_foliage acts in this view (main view, no engine compare)
    void foliageCount(const sc::GrassThin& t, std::size_t instances, std::uint32_t changed, std::uint32_t removed);
    void foliageReport();
    std::uint32_t ownInstCursor_ = 0;
    std::uint64_t ownInstFrame_ = ~0ull;
    std::vector<std::uint32_t> ownOffset_;  // per issued draw: its instance data's offset in ownInstVb_
    std::vector<std::string> firstMismatches_;
    std::uint32_t mismatchLogged_ = 0;
    std::uint32_t fourLightsLogged_ = 0;
};

}  // namespace rvk::native
