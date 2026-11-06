#pragma once

// F4-1 tree billboard op records (docs/research/frame-ownership.md 7.17; setting `native.owned_billboards`, Debug, Full + owned windows + lighting images + scene draws):
// the main view's eCRenderContextSpeedTreeBB z / solid ops are snapshotted at op end into a bbrec::Record keyed (ctx, desc, list) and, while its stamps and the read logs hold,
//   (compare frames) DERIVED again from the record and compared with the draws the composition makes, or
//   (owned frames) submitted from the record with no composition at all (StaticOps::bbReplay).
// The record types of F2c-2 (op_records.h: key, stamp, DrawRec) are used read-only; the compare classes, exclusion reasons and statistics are the billboards' own.
// (Unification with the F2c-3 owned-op machinery is later work: this file has the billboard-specific parts only.)

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "d3d9/device.h"
#include "d3d9/owned_windows.h"
#include "native/op_records.h"
#include "native/static_ops_retained.h"

namespace rvk::native::bbrec {

// why an op is not recorded (first reason in this order; per op)
enum class Excl : std::uint8_t { NoSub, NoShader, ShaderClass, NotComposed, DrawLevel, NoRetain, NotRetained, NotLit, NotLean, OtherOwner, Count };
inline constexpr const char* kExclNames[] = {"no sub-mesh", "no shader (engine edge)", "shader class", "not composed (left to the engine)", "draw level (inherited stage / CPU stream / not indexed)",
                                             "no retain (compile at the slot)", "draw not retainable (no packet)", "no lighting entry", "not a lean frame / lean blocked (video)",
                                             "overlay / other owner shader"};
inline constexpr std::size_t kExclCount = static_cast<std::size_t>(Excl::Count);

// the compare's field classes
enum class Field : std::uint8_t { Scene, Pipeline, VertexInput, Geometry, Flags, Tag, Windows, Constants, BlockTex, BlockSmp, BlockPos, BlockAlpha, BlockPad, BlockC5, Count };
inline constexpr const char* kFieldNames[] = {"scene form", "pipeline", "vertex input / dynamic state", "geometry", "flags", "tag", "windows", "constants image (c32..)", "block tex", "block smp",
                                              "block posFix", "block alpha", "block pad", "block c5"};
inline constexpr std::size_t kFieldCount = static_cast<std::size_t>(Field::Count);

// rebuild reasons beyond the stamp fields (orec::kStampNames): [kStampFields + i]
inline constexpr const char* kExtraWhy[] = {"billboard source", "draw count", "read log", "constants source", "derive refused"};
inline constexpr std::size_t kExtraWhyCount = std::size(kExtraWhy);

// the owned replay's refusals (the op takes the composed path)
enum class Refuse : std::uint8_t { NoRecord, Stale, CheckFrame, NoView, Derive, Windows, NoConsts, Submit, Material, Consumers, Partial, Count };
inline constexpr const char* kRefuseNames[] = {"no record", "stamps / read log", "check frame", "no view block", "derive refused", "window offer", "constants table", "submit refused", "material / lighting poll",
                                               "consumers (observers)", "PARTIAL (a draw failed after others were submitted)"};
inline constexpr std::size_t kRefuseCount = static_cast<std::size_t>(Refuse::Count);

// the arguments of the composition's checkMaterial for one draw (the poll an owned op repeats)
struct MatArgs {
    const void* owner = nullptr;
    const void* material = nullptr;
    bool zPass = false;
    std::uint16_t psMask = 0;
    std::int32_t blendMode = 0, maskRef = -1;
    // the sampler inputs checkMaterial read: every stage of the material's pool slots (texture, the seven states), the inherit mask
    std::uint16_t smpMask = 0, inheritMask = 0;
    const void* tex[16] = {};
    std::uint32_t ss[16][7] = {};
};

struct Record {
    orec::Key key;
    orec::Stamp stamp;
    // the billboard source: PS (ctx +0x18), its tex-coord list (+0x40), the range entry of the sub-mesh (+0x58: first, count)
    const void* ps = nullptr;
    const void* list = nullptr;
    std::uint32_t first = 0, count = 0;
    std::vector<orec::DrawRec> draws;
    std::vector<std::uint32_t> matSlots;
    std::vector<MatArgs> mats;            // per draw: the material poll's arguments
    const void* shader = nullptr;         // the sub-mesh's eCShaderBase (its admin pools / depth-bias flag: the view windows)
    bool depthBias = false;
    std::vector<ret::ReadRec> reads;      // the composition's read log (signal audit)
    std::vector<ret::ReadRec> srcReads;   // the constants' sources (the fixed table, the PS's list entries, the range entry)
    const void* anchors[8] = {};
    // the constants image: VS c32 .. c(32 + nConst - 1) (5 fixed + the sub-mesh's list)
    std::uint32_t nConst = 0;
    std::vector<float> consts;            // nConst x 4
    std::uint32_t constSlot = 0xffffffffu;  // the persistent constants table's record (B)
    // the side effects the engine's slot would run (lean commit): the billboard mesh's touch (inline RequestData), the lean lists collected during the composition
    const void* touchFirst = nullptr;
    std::vector<const void*> touches, lightmaps;
    std::vector<std::pair<void*, bool>> stream3;
    std::string name;
    std::uint64_t builtFrame = 0;
};

struct Stats {
    std::uint64_t ops = 0, draws = 0, opsAll = 0;
    std::uint64_t excluded[kExclCount] = {};
    std::uint64_t validAtEntry = 0, built = 0, rebuilt = 0;
    std::uint64_t rebuiltWhy[orec::kStampFields + kExtraWhyCount] = {};
    std::uint64_t derived = 0, compared = 0;
    std::uint64_t refused[orec::kDeriveWhyCount] = {};
    std::uint64_t differing = 0, clean = 0;
    std::uint64_t byField[kFieldCount] = {};
    std::uint64_t alphaRefCarry = 0, c5wCarry = 0, viewDup = 0;
    std::uint64_t auditChecked = 0, auditDisagree = 0, auditCaught = 0;
    std::uint64_t auditBy[orec::kAuditCount] = {};
    std::uint64_t constChecks = 0, constStale = 0, constRegsCompared = 0;
    // owned frames
    std::uint64_t ownedOps = 0, ownedDraws = 0;
    std::uint64_t refuse[kRefuseCount] = {};
    std::uint64_t buildNs = 0, deriveNs = 0, stampNs = 0, auditNs = 0, replayNs = 0, builds = 0, derives = 0;
    void add(const Stats& o) {
        ops += o.ops; draws += o.draws; opsAll += o.opsAll;
        for (std::size_t i = 0; i < kExclCount; ++i) excluded[i] += o.excluded[i];
        validAtEntry += o.validAtEntry; built += o.built; rebuilt += o.rebuilt;
        for (std::size_t i = 0; i < std::size(rebuiltWhy); ++i) rebuiltWhy[i] += o.rebuiltWhy[i];
        derived += o.derived; compared += o.compared; differing += o.differing; clean += o.clean;
        for (std::size_t i = 0; i < orec::kDeriveWhyCount; ++i) refused[i] += o.refused[i];
        for (std::size_t i = 0; i < kFieldCount; ++i) byField[i] += o.byField[i];
        alphaRefCarry += o.alphaRefCarry; c5wCarry += o.c5wCarry; viewDup += o.viewDup;
        auditChecked += o.auditChecked; auditDisagree += o.auditDisagree; auditCaught += o.auditCaught;
        for (std::size_t i = 0; i < orec::kAuditCount; ++i) auditBy[i] += o.auditBy[i];
        constChecks += o.constChecks; constStale += o.constStale; constRegsCompared += o.constRegsCompared;
        ownedOps += o.ownedOps; ownedDraws += o.ownedDraws;
        for (std::size_t i = 0; i < kRefuseCount; ++i) refuse[i] += o.refuse[i];
        buildNs += o.buildNs; deriveNs += o.deriveNs; stampNs += o.stampNs; auditNs += o.auditNs; replayNs += o.replayNs; builds += o.builds; derives += o.derives;
    }
};

struct Store {
    std::unordered_map<orec::Key, std::unique_ptr<Record>, orec::KeyHash> map;
    Stats window, total;
    std::uint32_t logged = 0, auditLogged = 0, loggedList[2] = {};
    std::uint32_t loggedBy[kFieldCount] = {};
    std::uint64_t lastFrame = 0;
    // the current op
    Record* cur = nullptr;
    bool active = false;
    const void* op = nullptr;
    bool building = false;
    bool postDone = false;
    bool auditBad = false;
    std::size_t auditCls = 0;
    bool diffInOp = false;
    bool refused = false;
    orec::Key key;
    orec::Stamp stamp;
    const void* ps = nullptr;
    const void* list = nullptr;
    std::uint32_t first = 0, count = 0;
    int rebuildWhy = -1;
    std::uint64_t switchBefore = 0;
    d3d9::Device::SceneView probe;
    ret::ReadLog readLog;
    std::vector<d3d9::Device::SceneView> probes;
    std::vector<std::uint64_t> pkeys;
    std::vector<MatArgs> mats;            // the composition's checkMaterial calls of the op being built
    // the view-level windows (VS 0 / 1, PS 0) of the first composed draw of a view and list (the view block's slot is a per-view input)
    std::uint64_t viewSerial = ~0ull;
    d3d9::OwnedWindows viewWin;
};

}  // namespace rvk::native::bbrec
