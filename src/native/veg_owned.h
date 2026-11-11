#pragma once

// F7-2 owned vegetation draws (docs/research/frame-ownership.md 7.42, setting native.owned_vegetation_ops): the retained vegetation (grass) op records and their per-frame state.
//
// A vegetation op (eCRenderContextVegetation z / solid op of the main view, desc = the grass type) draws the visible grid nodes' instances of ONE type: the nodes are cut into
// batches (planGrass: light set / alone / buffer wrap), each one hardware-instanced draw of the type's mesh. Here, per op, the composition's draws are kept as TEMPLATES keyed
// by BUCKET (type, z / solid, sun bit): the draw is the same for every batch of a bucket, only the batch's instances (stream 1: the converted instance bytes), the op's
// c31 / c32 registers (the vegetation fade, VS window 2) and in the solid pass the lighting image differ. An owned frame plans every op from the engine's grid nodes, copies
// the retained converted instance bytes of all ops into ONE ring block and draws the whole bucket with one indirect command (one DrawRecord per op x lighting entry, the ref list
// names every instance's record: Device::ownedTreeFlush with its vegetation instance block). The engine's ops never run (they left the lists at the view's beginning).

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "native/frame_desc.h"
#include "native/static_compose.h"
#include "native/tree_owned.h"

namespace rvk::native::vro {

// what an op's templates are valid for (read at the op's entry; the pass compares)
struct Stamp {
    const void* ps = nullptr;           // ctx +0x18
    const void* typeObj = nullptr;      // the type's object (PS +0x38 -> + 8 type)
    const void* mesh = nullptr;         // typeObj +0x58
    const void* visual = nullptr;       // mesh +0x88
    const void* sub0 = nullptr;         // mesh +0x58
    const void* material = nullptr;     // sub0 +0x38
    const void* shader = nullptr;       // material +0x70
    std::int32_t transformation = 0;    // shader +0x2b8
    std::uint32_t meshState = 0;        // mesh +0x38 (3 = cached)
    std::uint32_t ranges[4] = {};       // sub0 +0x28 / +0x2c / +0x30 / +0x34
    std::uint32_t baseVertex = 0;
    const void* vb = nullptr;           // the visual's vertex / index buffers and declaration (a reloaded mesh keeps its object pointers)
    const void* ib = nullptr;
    const void* decl = nullptr;
    std::uint8_t twoSided = 0;          // typeObj +0x30
    std::uint8_t depthBias = 0;         // shader +0x78
    std::uint64_t variantEpoch = 0;
    std::uint64_t registryGen = 0;
    bool operator==(const Stamp&) const = default;
};

struct OpRec {
    tro::OpKey key;
    Stamp stamp;
    std::vector<tro::TexMap> texMap;
    std::uint32_t type = 0;
    std::uint32_t matSlot = 0xffffffffu;
    std::uint64_t matHash = 0;
    std::uint64_t viewStamp = 0;
    std::vector<const void*> touches;
    std::unordered_map<std::uint64_t, std::unique_ptr<tro::Tpl>> tpls;
    // the retained converted instance stream of the op (the source entries flattened in node order, their conversion)
    std::vector<std::uint8_t> raw, conv;
    bool wind = false, lift = false;
    float c31[4] = {}, c32[4] = {};   // the composed c31 / c32 (the pass overwrites c31.xy and c32.w: the fade, 1)
    std::uint64_t builtFrame = 0, lastFrame = 0;
};

// one batch of an op's plan
struct Batch {
    std::uint32_t first = 0, count = 0;  // the op's flattened instances
    std::int32_t illumNode = -1;
    bool sun = false;
    std::uint64_t key = 0;
};

// the composition's plan of the op in progress (made at its entry, compared with the composed draws at its end)
struct Pend {
    std::unique_ptr<OpRec> rec;
    tro::OpKey key;
    bool zpass = false;
    bool bad = false;
    int badWhy = 0;
    std::vector<Batch> plan;
    float ax = 0, ay = 0;
    std::uint32_t total = 0;
    std::vector<std::uint8_t> raw;       // the flattened source entries
    bool wind = false, lift = false;
    std::uint64_t switchBefore = 0;
    bool noted = false;                  // composeVegetation reached its draws
};

struct Stats {
    std::uint64_t views = 0, ownedViews = 0, opsSeen = 0, opsTaken = 0, opsDrawn = 0, opsLate = 0, batches = 0, instances = 0, items = 0, records = 0, observed = 0, flushes = 0, compViews = 0;
    std::uint64_t lateNoRec = 0, lateStamp = 0, lateView = 0, lateMaterial = 0, latePlan = 0, lateSpecial = 0, lateNoTemplate = 0, lateTemplate = 0, lateFlush = 0, lateRead = 0;
    std::uint64_t built = 0, replaced = 0, notRetained = 0, badOps = 0;
    std::uint64_t badBy[8] = {};  // bad ops by reason: draws per batch, draw level, depth bias, lean block / no retain, switch, material slots, draw count, other
    std::uint64_t checkOps = 0, checkBatches = 0, checkPlanDiff = 0, checkTplDiff = 0, checkRegDiff = 0, checkConvDiff = 0, checkCountDiff = 0;
    std::uint64_t convReused = 0, convRebuilt = 0, moved = 0;  // moved: the composed draw differs from the template only by a resource that was reloaded (the template is replaced)
    std::uint64_t rcBy[4] = {};  // template refusals by treeTemplate's code (1 not allowed, 2 table full, 3 derivation refused)
    std::uint64_t nodeNs = 0, readNs = 0, planNs = 0, tplNs = 0, recNs = 0, flushNs = 0, sideNs = 0, takeNs = 0, passNs = 0;
    void add(const Stats& o) {
        views += o.views; ownedViews += o.ownedViews; opsSeen += o.opsSeen; opsTaken += o.opsTaken; opsDrawn += o.opsDrawn; opsLate += o.opsLate; batches += o.batches;
        instances += o.instances; items += o.items; records += o.records; observed += o.observed; flushes += o.flushes; compViews += o.compViews;
        lateNoRec += o.lateNoRec; lateStamp += o.lateStamp; lateView += o.lateView; lateMaterial += o.lateMaterial; latePlan += o.latePlan; lateSpecial += o.lateSpecial;
        lateNoTemplate += o.lateNoTemplate; lateTemplate += o.lateTemplate; lateFlush += o.lateFlush; lateRead += o.lateRead;
        built += o.built; replaced += o.replaced; notRetained += o.notRetained; badOps += o.badOps;
        for (int i = 0; i < 8; ++i) badBy[i] += o.badBy[i];
        checkOps += o.checkOps; checkBatches += o.checkBatches; checkPlanDiff += o.checkPlanDiff; checkTplDiff += o.checkTplDiff; checkRegDiff += o.checkRegDiff;
        checkConvDiff += o.checkConvDiff; checkCountDiff += o.checkCountDiff; convReused += o.convReused; convRebuilt += o.convRebuilt; moved += o.moved;
        for (int i = 0; i < 4; ++i) rcBy[i] += o.rcBy[i];
        nodeNs += o.nodeNs; readNs += o.readNs; planNs += o.planNs; tplNs += o.tplNs; recNs += o.recNs; flushNs += o.flushNs; sideNs += o.sideNs; takeNs += o.takeNs; passNs += o.passNs;
    }
};

struct State {
    std::unordered_map<tro::OpKey, std::unique_ptr<OpRec>, tro::OpKeyHash> ops;
    Stats window, total;
    tro::Stats tw, tt;                   // the templates' derivation counters (treeTemplate: derived / reused / refused / MISS)
    bool active = false;                 // this view is an owned frame for vegetation
    std::array<std::vector<Op>, 2> taken;  // z / solid ops that left the lists
    std::unique_ptr<d3d9::Buffer> stand; // the shared stream-1 stand-in of the templates' packets (a static buffer: its binding never changes)
    Pend pend;
    bool tripped = false;                // a DIFFERING: vegetation stays composed
    std::uint32_t logged = 0;
    std::string gate;                    // the last view's unmet condition of the vegetation's own readiness ("" = none; the stage line names it)
    std::uint64_t scrubTick = 0;
    std::uint64_t differingSeen = 0;
    // pass scratch
    std::vector<std::uint8_t> inst;      // the instance block (positions x 0x2c), list order
    std::vector<d3d9::Device::TreeItem> items;
    std::vector<shader::abi::DrawRecord> recs;
    std::vector<std::uint32_t> posRec;
    std::vector<std::uint8_t> flat;      // scratch: the flattened source entries
};

}  // namespace rvk::native::vro
