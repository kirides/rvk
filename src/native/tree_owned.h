#pragma once

// F4-2 / F4-3 owned SpeedTrees (docs/research/frame-ownership.md 7.25, setting native.owned_trees): the retained tree records and their per-frame plans.
//
// A tree op (eCRenderContextSpeedTree z / solid op of the main view, desc = geometry type 0..3) draws every queued tree of one render buffer. The composition
// (static_ops_trees.cpp composeTree) cuts the queue into batches (same strip length, same sun bit under table lighting, <= the render buffer's limit) and makes
// one hardware-instanced draw per batch. Here, per op, the composition's draws are kept as TEMPLATES keyed by BUCKET (type, z / solid, LOD geometry, sun): the
// draw is the same for every instance of a bucket, only the row of the instance (VS c32 + 4 p .. c35 + 4 p: world matrix, tree data, alpha) and, in the solid pass,
// the lighting image differ. An owned frame plans every instance of the queue from the engine's tree objects, writes the rows and one DrawRecord per run, and draws
// the pass's trees with a few indirect calls (Device::ownedTreeFlush); the engine's ops never run (they left the lists at the view's beginning).

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "d3d9/device.h"
#include "native/frame_desc.h"
#include "native/op_records.h"
#include "native/static_compose.h"

namespace rvk::native::tro {

struct OpKey {
    const void* ctx = nullptr;
    std::uint32_t desc = 0;
    std::uint8_t list = 0;
    bool operator==(const OpKey&) const = default;
};
struct OpKeyHash {
    std::size_t operator()(const OpKey& k) const noexcept {
        std::uint64_t h = (reinterpret_cast<std::uintptr_t>(k.ctx) >> 3) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<std::uint64_t>(k.desc) * 0xC2B2AE3D27D4EB4Full;
        h ^= static_cast<std::uint64_t>(k.list) << 56;
        return static_cast<std::size_t>(h ^ (h >> 31));
    }
};

// one bucket's retained draw
struct Tpl {
    orec::DrawRec r;                    // the retained packet + everything the owned derivation needs (its persist slot holds the DrawBlock)
    std::unique_ptr<sc::DrawDesc> desc; // the composed draw (check frames compare the next composition with it)
    std::uint64_t key = 0;              // the bucket key (StaticOps::treeBucketKey)
    std::uint64_t builtFrame = 0;
    float c31[4] = {};                  // the composed VS c31 (the row block's header)
    bool stale = false;                 // a derivation was refused for a reason a new composition fixes
    std::uint64_t passSerial = 0;       // the pass this template was made ready in (scratch)
    std::uint32_t passRun = 0;          // its run in the pass's list (scratch)
    std::uint64_t devBucket = 0;        // Device::ownedBucketKey (scratch, per pass)
};

// what an op's templates are valid for (read at the op's entry; the pass compares)
struct Stamp {
    const void* res = nullptr;          // ps +0x38
    const void* rb = nullptr;           // res +0x78
    const void* material = nullptr;     // rb +0x80 + 8 t
    const void* shader = nullptr;       // material +0x70
    const void* visual = nullptr;       // rb +0x18 + 8 t
    std::int32_t transformation = 0;    // shader +0x2b8
    std::uint64_t variantEpoch = 0;
    std::uint64_t registryGen = 0;
    bool hardware = false, tableMerge = false;
    bool operator==(const Stamp&) const = default;
};

struct OpRec {
    OpKey key;
    Stamp stamp;
    std::uint32_t type = 0;
    std::uint32_t matSlot = 0xffffffffu;
    std::uint64_t matHash = 0;                    // the material record's structure version, as the composition saw it
    std::uint64_t viewStamp = 0;                  // StaticOps::recViewStamp at the composition
    std::vector<const void*> touches;             // the engine resources the slot's code touches
    std::unordered_map<std::uint64_t, std::unique_ptr<Tpl>> tpls;
    std::uint64_t builtFrame = 0;
};

// one planned instance (queue order, leaves: LOD-major as the composition draws them)
struct Inst {
    const std::uint8_t* tree = nullptr;
    const std::uint8_t* illum = nullptr;
    std::uint64_t key = 0;
    float row[16] = {};                 // c32 + 4 p .. c35 + 4 p
    bool sun = false;
    std::uint16_t strip = 0;            // branches / fronds: the LOD's strip length (a fronds instance with 0 is not drawn)
};

struct Stats {
    std::uint64_t views = 0, ownedViews = 0, opsSeen = 0, opsTaken = 0, opsDrawn = 0, opsLate = 0, instances = 0, items = 0, records = 0, observed = 0, flushes = 0, compBatches = 0, compViews = 0, packets = 0;
    // why an op taken from the list was composed late
    std::uint64_t lateNoRec = 0, lateStamp = 0, lateView = 0, lateMaterial = 0, latePlan = 0, lateSpecial = 0, lateNoTemplate = 0, lateTemplate = 0, lateFlush = 0, lateRows = 0;
    std::uint64_t built = 0, replaced = 0, notRetained = 0, badOps = 0;
    std::uint64_t badBy[8] = {};  // bad ops by reason: draws per batch, draw level, depth bias, lean block / no retain, switch, material slots, no draws, other
    std::uint64_t derived = 0, reused = 0, refused = 0, tableFull = 0;
    // check frames: the composition against the plan and the templates
    std::uint64_t checkOps = 0, checkInst = 0, checkRowDiff = 0, checkKeyDiff = 0, checkCountDiff = 0, checkDraws = 0, checkTplDiff = 0, checkMiss = 0, checkMissCompared = 0;
    std::uint64_t planNs = 0, tplNs = 0, rowNs = 0, flushNs = 0, sideNs = 0, takeNs = 0;
    void add(const Stats& o);
};

struct State {
    std::unordered_map<OpKey, std::unique_ptr<OpRec>, OpKeyHash> ops;
    Stats window, total;
    bool active = false;                // this view is an owned frame for trees
    std::array<std::vector<Op>, 2> taken;  // z / solid ops that left the lists
    std::unique_ptr<d3d9::Buffer> ramp;
    std::uint32_t rampLen = 0;
    // composition in progress
    OpRec* cur = nullptr;               // the op being composed (null: not a tree op of the main view)
    OpRec* building = nullptr;
    std::unique_ptr<OpRec> pending;
    bool bad = false;
    int badWhy = 0;
    std::uint64_t switchBefore = 0;
    std::vector<Inst> composed;         // check frames: the composed instances in order
    std::vector<Inst> plan;             // check frames: the plan made at the op's entry
    bool checkOp = false;
    std::uint32_t logged = 0;
    std::uint64_t lastFrameChecked = ~0ull;
    // pass scratch
    std::vector<Inst> inst;
    std::vector<Tpl*> instTpl;
    std::vector<std::uint32_t> instEntry;
    std::vector<std::uint32_t> opOfInst;
    std::vector<d3d9::Device::TreeItem> items;
    std::vector<shader::abi::DrawRecord> recs;   // one per (template, lighting entry)
    std::vector<std::uint32_t> posRec;           // the record of each list position
    std::vector<std::uint32_t> order;
    std::uint64_t scrubTick = 0;
    std::uint64_t differingSeen = 0;
};

}  // namespace rvk::native::tro
