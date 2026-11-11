#pragma once

// F2c-2 op records (docs/research/frame-ownership.md 7.14; setting `native.owned_records`, Debug, Full + native.owned_objects + native.lighting_images):
// the static ops' main-view z / solid mesh ops, "cut 1" of section 7.12, are snapshotted at op end into an OpRecord keyed (ctx, desc, list, viewKey)
// and, on a later frame whose stamps still hold, DERIVED again from the record + the per-frame inputs (no composition result of the frame is read) and
// COMPARED with the draws the composition makes. Nothing derived is submitted: F2c-3 does that once DIFFERING stays 0.
//
// Pure parts (this header): the key, the stamp and its field-wise comparison, the exclusion reasons, the statistics. The engine reads and the
// compare are in static_ops_records.cpp (StaticOps members).

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "d3d9/device.h"
#include "d3d9/owned_windows.h"
#include "gpuscene/bone_table.h"
#include "gpuscene/draw_record_table.h"
#include "gpuscene/row_table.h"
#include "native/static_ops_retained.h"

namespace rvk::native::orec {

struct Key {
    const void* ctx = nullptr;
    std::uint32_t desc = 0;
    std::uint8_t list = 0;
    std::uint64_t view = 0;
    bool operator==(const Key&) const = default;
};
struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept {
        std::uint64_t h = (reinterpret_cast<std::uintptr_t>(k.ctx) >> 3) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<std::uint64_t>(k.desc) * 0xC2B2AE3D27D4EB4Full;
        h ^= static_cast<std::uint64_t>(k.list) << 56;
        h ^= k.view * 0xD6E8FEB86659FD93ull;
        return static_cast<std::size_t>(h ^ (h >> 31));
    }
};

// everything an op's record is valid for (read at the op's entry; the material structure versions after the composition: the materials are polled by it)
struct Stamp {
    const void* res = nullptr;          // ctx +0x28: the LOD resource
    const void* entity = nullptr;       // ctx +0x10
    std::uint64_t geom = 0;             // the covered sub-meshes: ranges, material, shader (class, blend inputs, flags), cached state, vertex / index resource
    std::uint64_t light = 0;            // the illumination's pass-structure inputs: pointer, list, sun bit, no-sun, static / dynamic light counts
    std::uint64_t lightmaps = 0;        // res +0x90 / +0x98: image pointers + residency
    std::uint32_t entityFlags = 0;      // entity +0x28 & 0xe00000 (overlay flags)
    const void* illumPs = nullptr;      // entity +0xf8
    std::uint32_t fadeBits = 0;         // the op's fade
    std::int32_t switchValue = 0;       // ctx +0x18 +0x20
    std::uint32_t objSlot = 0xffffffffu, objGen = 0;  // the scene-table slot of the entity and its generation (the object record's identity)
    std::uint64_t variantEpoch = 0;     // Device::variantEpoch
    std::uint64_t registryGen = 0;      // LightTable::registryGeneration
    std::uint64_t view = 0;             // the view's admin inputs (version, type, z write, render mode, global alpha, ...)
    std::uint64_t materials = 0;        // the draws' material records' structure versions (filled after the composition)
    bool operator==(const Stamp&) const = default;
};
inline constexpr const char* kStampNames[] = {"LOD resource", "entity", "geometry / material", "illumination", "lightmaps", "overlay flags", "illum pointer", "fade",
                                              "switch value", "object slot", "variant epoch", "registry generation", "view", "material structure"};
inline constexpr std::size_t kStampFields = std::size(kStampNames);
// a logged word read that looked at a resource still loading: a 4-byte state (eCResource2 +0x38: 1 / 2 = requested / loading, 3 = resident) below 3 at an engine object's
// +0x38, or an 8-byte image data pointer (+0x70) still null at an object whose state is below 3
inline bool loadingRead(std::uint32_t size, std::uint64_t value, bool engineObjectAt38, bool engineObjectAt70, std::int32_t stateAt70Object) {
    return (size == 4 && value < 3 && engineObjectAt38) || (size == 8 && value == 0 && engineObjectAt70 && stateAt70Object < 3);
}
// the index (kStampNames) of the first field that differs; -1 = equal
inline int firstDifference(const Stamp& a, const Stamp& b) {
    if (a.res != b.res) return 0;
    if (a.entity != b.entity) return 1;
    if (a.geom != b.geom) return 2;
    if (a.light != b.light) return 3;
    if (a.lightmaps != b.lightmaps) return 4;
    if (a.entityFlags != b.entityFlags) return 5;
    if (a.illumPs != b.illumPs) return 6;
    if (a.fadeBits != b.fadeBits) return 7;
    if (a.switchValue != b.switchValue) return 8;
    if (a.objSlot != b.objSlot || a.objGen != b.objGen) return 9;
    if (a.variantEpoch != b.variantEpoch) return 10;
    if (a.registryGen != b.registryGen) return 11;
    if (a.view != b.view) return 12;
    if (a.materials != b.materials) return 13;
    return -1;
}

// why an op is not in cut 1 (the first reason in this order; counted per op)
enum class Excl : std::uint8_t { Instanced, InstanceList, Overlay, Fade, Hierarchy, NodeOp, NoList, NotTableLit, ShaderClass, SortId, NotComposed, Switch, DrawLevel, NoRetain, NotRetained,
                                 DepthBias, Blended, LeanBlock, Count };
inline constexpr const char* kExclNames[] = {"hardware batch", "instance list", "overlay flags", "fade < 1", "hierarchy", "node op", "no light list", "not table-lit",
                                             "shader class", "sort id", "not composed (left to the engine)", "switch material", "draw level (inherited stage / CPU stream / not indexed)", "no retain (compile at the slot)",
                                             "draw not retainable (no packet)", "depth-bias material (F2c-3)", "blended / no z write draw (F2c-3)", "lean block (video / edge case)"};
inline constexpr std::size_t kExclCount = static_cast<std::size_t>(Excl::Count);

// F2d (frame-ownership.md 7.28): hierarchy meshes (ctx +0x34 > 0) and their node ops are owned like any other static op; `RVK_F2D_NO_HIERARCHY=1` (Win32
// environment) restores the exclusion for the A/B arm
inline bool f2dMeasure() {
    static const bool on = [] {
        char b[8];
        return GetEnvironmentVariableA("RVK_F2D_MEASURE", b, sizeof(b)) != 0;
    }();
    return on;
}
inline bool hierarchyOwned() {
    static const bool on = [] {
        char b[8];
        return GetEnvironmentVariableA("RVK_F2D_NO_HIERARCHY", b, sizeof(b)) == 0;
    }();
    return on;
}

// the compare's field classes
enum class Field : std::uint8_t { Scene, Pipeline, VertexInput, Geometry, Flags, Tag, Windows, BlockTex, BlockSmp, BlockPos, BlockAlpha, BlockPad, BlockC5, Count };
inline constexpr const char* kFieldNames[] = {"scene form", "pipeline", "vertex input / dynamic state", "geometry", "flags", "tag", "windows", "block tex", "block smp", "block posFix",
                                              "block alpha", "block pad", "block c5"};
inline constexpr std::size_t kFieldCount = static_cast<std::size_t>(Field::Count);

// read-log disagreement classes: where the first changed read lies
inline constexpr const char* kAuditNames[] = {"ctx", "mesh resource", "entity", "illumination", "light set", "sub-mesh table", "material", "shader", "other"};
inline constexpr std::size_t kAuditCount = std::size(kAuditNames);

inline constexpr const char* kDeriveWhy[] = {"derived", "stale module", "phase", "consumers", "variant gate", "env", "stream / index binding", "texture", "jitter flags", "scene form",
                                             "draw count", "no packet"};
inline constexpr std::size_t kDeriveWhyCount = std::size(kDeriveWhy);

// F2c-4a (native.owned_persistent): the draw's slot in gpuscene::drawrecords(), returned (deferred) when the draw record goes away
struct SlotHandle {
    std::uint32_t slot = 0xffffffffu;
    SlotHandle() = default;
    SlotHandle(const SlotHandle&) = delete;
    SlotHandle& operator=(const SlotHandle&) = delete;
    SlotHandle(SlotHandle&& o) noexcept : slot(o.slot) { o.slot = 0xffffffffu; }
    SlotHandle& operator=(SlotHandle&& o) noexcept {
        if (this != &o) {
            reset();
            slot = o.slot;
            o.slot = 0xffffffffu;
        }
        return *this;
    }
    ~SlotHandle() { reset(); }
    void reset() {
        if (slot != 0xffffffffu) gpuscene::drawrecords().release(slot);
        slot = 0xffffffffu;
    }
    bool has() const { return slot != 0xffffffffu; }
};

// what a persistent draw was derived from (frame-ownership.md 7.21): the slot holds the derivation of (env epoch, sig) for this very draw record
struct Persist {
    SlotHandle handle;
    bool valid = false;
    std::uint64_t epoch = 0, sig = 0, bucket = 0;
    std::uint32_t viewSlot = 0;   // F9 / F14: the view slot the content was derived for
    std::int32_t envIndex = -1;   // ... and its env index (-1 = the view slot's: Device::ownedEnvEpoch; else Device::ownedEnvEpochAt)
    d3d9::OwnedWindows ow;  // the draw's windows as the last frame built them (the observers' CPU twin; the view windows are patched per pass)
};

struct DrawRec {
    std::unique_ptr<d3d9::Device::CachedDraw> cd;  // the retained packet (own copy)
    mutable Persist persist;                       // F2c-4a
    // buffers (bindings re-validated at derive)
    const void* streams[4] = {};
    std::uint32_t strides[4] = {};
    const void* indices = nullptr;
    // sampled stages: the composed textures and the sampler states the composition owns
    const void* tex[16] = {};
    std::uint32_t ss[16][7] = {};
    std::uint16_t texMask = 0;
    // the pass's alpha test as the states say (never the device's carry-over) + the mask it writes into c5.w
    std::uint32_t alphaTestState = 0, alphaFunc = 8, alphaRef = 0;
    bool maskSet = false;
    float maskValue = 0.0f;
    // tables
    std::uint32_t matSlot = 0xffffffffu, illumSlot = 0xffffffffu, lightN = 0, lightFirst = 0;
    bool lightTableLit = false, lightHoldsAll = false;
    std::array<std::uint64_t, 4> vsSet{}, psSet{};
    bool vs1Offered = false;  // VS window 1 was offered (static, non-instanced)
    // the draw tag's sub-mesh fields (null for z draws)
    const void* tagSubMesh = nullptr;
    const void* tagMaterial = nullptr;
    const void* tagMeshResource = nullptr;
    std::uint8_t tagWater = 0;
    const char* tagMaterialName = nullptr;
    bool zPass = false;
    std::uint32_t passFlags = 0;
    bool scene = false;  // composed as a scene draw when the record was made
    // F2f (native.owned_instances, frame-ownership.md 14.19): an instance-list draw's VS window 2 image (the instance row table; immutable: the stamp holds the instance's matrix)
    // and the illumination its lighting slot belongs to (the render instance's, not the entity's)
    gpuscene::RowHandle row;
    const void* illumPtr = nullptr;
};

struct OpRecord {
    Key key;
    Stamp stamp;
    std::vector<DrawRec> draws;
    std::vector<std::uint32_t> matSlots;           // the draws' material slots (their structure versions are the stamp's `materials`)
    std::vector<ret::ReadRec> reads;               // the composition's read log (the signal audit)
    const void* anchors[8] = {};                   // ctx, mesh resource, entity, illumination, light set, sub-mesh table, material, shader: where a changed read is classified
    // the op's side effects the engine's slot would run (F2c-3 replays them): the mesh resource's lightmap binding (solid), stream 3 (z), the images the draws sample
    const void* lightmapRes = nullptr;
    bool bindLightmaps = false, stream3 = false;
    std::vector<const void*> images;
    // F2c-3: the engine resources the slot's code touches (RequestData(res, 1): every image a draw samples, the materials) and the visual's stream 3 switches
    // the composition collected (StaticOps::leanTouch / leanImage / leanStream3 while a record is built)
    std::vector<const void*> touches;
    struct Stream3 {
        void* primitive = nullptr;
        bool on = false;
    };
    std::vector<Stream3> stream3Prims;
    // frame-ownership.md 7.21 addendum (2026-11-07): what the stamps could not see. `watch` = the composition's logged word reads that looked at a resource still loading
    // (`loadingRead`: a state dword below 3, or image data still null, of an engine object): the record is stale once one of them changes (the load completed: the composition now
    // binds the real image / material). Only these are re-read per op (a resident record has none). `illumSlot` = the illumination's table slot the draws were composed with (the
    // lighting entries key on it): stale once the table freed / reused the slot (idle sweep).
    std::vector<ret::ReadRec> watch;
    std::uint32_t illumSlot = 0xffffffffu;
    // F6-1 (frame-ownership.md 7.40): a skinned actor part's record. `actorKey` = the bone table key of its image (wrapper, primitive, the fade the op's list uses), `actorBones` the
    // primitive's bone count (the image holds 2 + 3 bones registers)
    bool actor = false;
    gpuscene::BoneKey actorKey;
    std::uint32_t actorBones = 0;
    bool ordered = false;    // F9 / F14 (native.owned_ordered): an order-dependent op (blended / no z write / alpha / refracted list): never compacted, drawn at its place
    bool dbvp = false;       // F14: its draws read the depth-bias view-projection in c0..c3 (a depth-bias material): the view's dbvp slot
    bool instances = false;  // F2f: a render-instance op (node 0xffffff): its draws' window 2 are instance row images, its stamp holds the instance list
    bool ownable = false;  // the touches / stream 3 lists were collected while this record was built (a record built on a frame that did not collect them is rebuilt once)
    std::string name;                              // diagnostics: mesh / material name
    std::uint64_t builtFrame = 0;
    mutable std::uint64_t lastSeen = 0;            // F3-1: the frame the op was last looked at (a shadow view's records age out: a cascade sees different ops as the camera moves)
    // F2e-1 (frame-ownership.md 7.30): event-driven validity. `deps` = the engine objects the stamp's engine reads hang on (the LOD resource, the covered sub-meshes' materials,
    // the lightmap images, the illumination PS), registered in the DepIndex (object -> records); an engine event on one marks `dirty` and the next look re-verifies the full stamp
    // (clean again when it still holds). `shaders` = the covered shaders with their field fingerprint (no event exists for a shader's property fields: polled once per shader and
    // frame). `lightEpoch` = depevents::lightEpoch when the stamp was last verified (a changed light set re-verifies every record once).
    mutable bool dirty = false;
    std::uint64_t lightEpoch = 0;
    std::vector<const void*> deps;
    std::vector<std::pair<const void*, std::uint64_t>> shaders;
    OpRecord() = default;
    OpRecord(const OpRecord&) = delete;
    OpRecord& operator=(const OpRecord&) = delete;
    ~OpRecord();
};

// object -> the records that registered it (engine thread only; a record unregisters itself)
class DepIndex {
public:
    static DepIndex& get() {
        static DepIndex* p = new DepIndex;  // (leaked: records of a StaticOps destroyed at exit unregister into it)
        return *p;
    }
    void add(const void* obj, OpRecord* r) { map_[obj].push_back(r); }
    void remove(const void* obj, OpRecord* r) {
        const auto it = map_.find(obj);
        if (it == map_.end()) return;
        auto& v = it->second;
        const auto at = std::find(v.begin(), v.end(), r);
        if (at != v.end()) {
            *at = v.back();
            v.pop_back();
        }
        if (v.empty()) map_.erase(it);
    }
    // marks the records that depend on `obj`; returns how many were clean before
    std::uint32_t markDirty(const void* obj) {
        std::uint32_t n = 0;
        const auto it = map_.find(obj);
        if (it == map_.end()) return 0;
        for (OpRecord* r : it->second) {
            if (!r->dirty) {
                r->dirty = true;
                ++n;
            }
        }
        return n;
    }
    std::size_t objects() const { return map_.size(); }

private:
    std::unordered_map<const void*, std::vector<OpRecord*>> map_;
};

inline OpRecord::~OpRecord() {
    for (const void* d : deps) DepIndex::get().remove(d, this);
}

// 5 s window (and run totals) of the log line
struct Stats {
    std::uint64_t ops = 0, draws = 0;                 // eligible ops / their composed draws
    std::uint64_t excluded[kExclCount] = {};          // ops of the main view's static z / solid lists that are not in cut 1, by first reason
    std::uint64_t opsAll = 0;                         // ops of the Static group's main view lists (the denominator of the share)
    std::uint64_t validAtEntry = 0, built = 0, rebuilt = 0;
    std::uint64_t rebuiltWhy[kStampFields + 6] = {};  // by first differing stamp field; + draw count, read log, not retained, other, illumination slot gone, resource load completed
    std::uint64_t derived = 0, compared = 0;          // draws derived / of those compared with a scene draw
    std::uint64_t refused[kDeriveWhyCount] = {};
    std::uint64_t differing = 0;                      // draws with a difference
    std::uint64_t byField[kFieldCount] = {};          // ... by class (a draw counts in every class that differs)
    std::uint64_t clean = 0;
    std::uint64_t alphaRefCarry = 0, c5wCarry = 0;    // ignored differences: the device's carry-over where the shader reads nothing
    std::uint64_t captureSkipped = 0;                 // draws composed with a capture variant (not compared: no scene form by design)
    std::uint64_t viewDup = 0;                        // view-block windows at another slot with the same registers (the dedupe search window)
    std::uint64_t auditChecked = 0, auditDisagree = 0, auditDisagreeDiff = 0, auditCaught = 0;
    std::uint64_t auditBy[kAuditCount] = {};
    std::uint64_t buildNs = 0, deriveNs = 0, deriveCoreNs = 0, stampNs = 0, auditNs = 0, builds = 0, derives = 0;
    void add(const Stats& o) {
        ops += o.ops; draws += o.draws; opsAll += o.opsAll;
        for (std::size_t i = 0; i < kExclCount; ++i) excluded[i] += o.excluded[i];
        validAtEntry += o.validAtEntry; built += o.built; rebuilt += o.rebuilt;
        for (std::size_t i = 0; i < std::size(rebuiltWhy); ++i) rebuiltWhy[i] += o.rebuiltWhy[i];
        derived += o.derived; compared += o.compared; differing += o.differing; clean += o.clean;
        for (std::size_t i = 0; i < kDeriveWhyCount; ++i) refused[i] += o.refused[i];
        for (std::size_t i = 0; i < kFieldCount; ++i) byField[i] += o.byField[i];
        alphaRefCarry += o.alphaRefCarry; c5wCarry += o.c5wCarry; viewDup += o.viewDup; captureSkipped += o.captureSkipped;
        auditChecked += o.auditChecked; auditDisagree += o.auditDisagree; auditDisagreeDiff += o.auditDisagreeDiff; auditCaught += o.auditCaught;
        for (std::size_t i = 0; i < kAuditCount; ++i) auditBy[i] += o.auditBy[i];
        buildNs += o.buildNs; deriveNs += o.deriveNs; deriveCoreNs += o.deriveCoreNs; stampNs += o.stampNs; auditNs += o.auditNs; builds += o.builds; derives += o.derives;
    }
};

struct Store {
    std::unordered_map<Key, std::unique_ptr<OpRecord>, KeyHash> map;
    Stats window, total;
    Stats shadowWindow, shadowTotal;         // F3-1: the cascades' ops (keys with view != 0)
    Stats& stats(bool shadow) { return shadow ? shadowWindow : window; }
    std::uint32_t logged = 0, auditLogged = 0;
    std::uint32_t loggedBy[kFieldCount] = {};
    std::uint64_t lastFrame = 0;
    // the current op
    OpRecord* cur = nullptr;                 // the record valid at entry (null: building)
    bool active = false;                     // the op is eligible
    const void* op = nullptr;                // ... and this is it (the issue path checks it)
    bool building = false;
    bool postOk = true;                      // checked at the op's first draw (material versions, view)
    bool postDone = false;
    bool auditBad = false;                   // the read log disagreed at entry
    std::size_t auditCls = 0;
    bool diffInOp = false;                   // a draw of the op differed (attributes an audit disagreement)
    bool wantOwnable = false;                // the record valid at entry was built without the side-effect lists: rebuilt from this composition (F2c-3)
    bool refused = false;                    // a derivation was refused for a reason that makes the record stale (module, variant, binding, texture)
    bool ordered = false;                    // F9 / F14: the record being built is an ordered one
    Key key;
    Stamp stamp;                             // the stamp read at entry
    int rebuildWhy = -1;                     // the first stamp field that differed at entry (-1: none)
    std::uint64_t switchBefore = 0;          // the material table's switch writes before the composition
    d3d9::Device::SceneView probe;           // the scene view of the draw being issued (filled by the device)
    std::vector<ret::ReadRec> log;
    ret::ReadLog readLog;
    bool logging = false;                    // the current op's composition was logged into readLog (a record rebuilt for a post-composition stamp was not: it takes no reads)
    std::vector<d3d9::Device::SceneView> probes;
    std::vector<std::uint64_t> pkeys;
    // F2d measurement (RVK_F2D_MEASURE=1): the wall time from an op's entry to the next op's entry (composition + issue), by exclusion class (kExclCount = eligible)
    std::uint64_t f2dNs[kExclCount + 1] = {}, f2dOps[kExclCount + 1] = {};
    int f2dCls = -1;
    std::chrono::steady_clock::time_point f2dT0;
    std::string opName;
    // the per-view window cache (the view-level windows VS 0 / 1, PS 0: the view block's slot is a per-view input)
    std::uint64_t viewSerial = ~0ull;
    d3d9::OwnedWindows viewWin;
    bool viewWinValid = false;
};

}  // namespace rvk::native::orec
