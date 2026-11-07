#pragma once

// The GPU material table (docs/research/frame-ownership.md F0 / 1.2 row Material; the pattern of illumination_table.*): one 1104-byte record per engine
// material instance as N5 draws it (material_compose.h: VS / PS pool registers, sampler slots -> bindless texture + sampler states, mask reference, blend
// mode, flags, graph ids), in a persistent device buffer updated in batches once per present on both backends (the illumination table's GpuTable).
// Setting `native.material_table` (Debug, restart, default off; Full only: only the static composition's passDraw / specialPassDrawImpl allocate records):
//   * slot at the first sight of a shader object in the static composition (key = the eCShaderDefault whose pools / sampler slots the composition reads);
//   * every record is re-read from the engine object once per frame, at its first draw (a poll: the pools are rewritten per frame by SetTime and by
//     property handlers; no hook is installed: the material pool's per-frame writers are Process -> SetTime and the switches, retained-signals.md 9.3 / 9.4,
//     and the illumination table showed that hooks alone miss in-place rewrites), and uploaded when the bytes changed (reason: poll = registers / state,
//     texture = a slot's bindless index or sampler state, graph = a PS request's graph id appeared / changed);
//   * the texture resolution is the composition's own (image data -> placeholder -> error image -> bindless slot of the HAL texture), done by a callback;
//   * freed when not drawn for kIdleFrames frames (a destroyed material's address may be reused: the next poll re-reads the content anyway).
// Verification (F0 gate): `checkDraw` composes the material-dependent registers and bindings of each static draw FROM THE RECORD and compares them with what
// the composition set (rvk.log `native: material table (5 s): ... registers DIFFERING N, textures DIFFERING M`, both must stay 0); every 240th frame the
// buffer is copied back and compared with the CPU copy.

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "d3d9/owned_windows.h"
#include "gpuscene/illumination_table.h"
#include "gpuscene/material_compose.h"
#include "gpuscene/material_graph.h"
#include "rhi/game_device.h"

namespace rvk::d3d9 {
class Device;
class Shader;
}  // namespace rvk::d3d9

namespace rvk::gpuscene {

struct MaterialStats {
    std::uint64_t uploadNew = 0, uploadPoll = 0, uploadTexture = 0, uploadGraph = 0, graphVariants = 0;  // records (re-)uploaded by cause
    std::uint64_t polls = 0, pollsUnchanged = 0, switchNotes = 0;
    std::uint64_t freed = 0;
    std::uint64_t draws = 0, checked = 0, unchecked = 0, switchDraws = 0;
    std::uint64_t regsCompared = 0, regsSwitch = 0, texCompared = 0, texUnresolved = 0, stateCompared = 0, blendCompared = 0;
    std::uint64_t regsDiffering = 0, texDiffering = 0, stateDiffering = 0, blendDiffering = 0;  // DRAWS with a difference, by kind
    std::uint64_t overflow = 0, readFailed = 0;
    std::uint64_t checkNs = 0;  // time spent in checkDraw
};

class MaterialTable {
public:
    static constexpr std::uint32_t kCapacity = 4096;  // slots (4.3 MiB)
    static constexpr std::uint64_t kIdleFrames = 1800;

    // bootstrap: the setting is on
    void setActive(bool on) { active_ = on; }
    bool active() const { return active_; }

    // resolves a sampler slot's image resource (the engine's eCImageResource2*) at `stage` into the bindless slot the composition binds (kMatNoTex: none)
    using TexFn = std::uint32_t (*)(void* ctx, std::uint32_t stage, const void* imageRes);

    // StaticOps::applySwitch: a SetSwitch element writes `kind` (0 VS pool register, 1 PS pool register, 2 sampler slot) `idx` of `shader`'s pools
    void noteSwitchWrite(const void* shader, int kind, std::uint32_t idx);

    struct Draw {
        const void* owner = nullptr;     // the shader object whose pools / slots the composition used (constantsInto / materialSamplers)
        const void* material = nullptr;  // eCMaterialResource2 of the draw (name; null for overlay owners)
        const d3d9::Shader* ps = nullptr;
        bool zPass = false;
        MaterialDraw draw;
        TexFn tex = nullptr;
        void* texCtx = nullptr;
    };
    // after the draw's constants and samplers are composed (engine thread)
    // returns the record's slot (kNoSlot: none: not active / overflow / unreadable)
    static constexpr std::uint32_t kNoSlot = 0xffffffffu;
    // `verify` false (7.27, off the check frames): the lookup, the once-per-frame poll and the graph note only; `d.draw` is not read
    std::uint32_t checkDraw(const Draw& d, bool verify = true);

    // Window override (frame-ownership.md 7.8, F1a): the Material windows of a draw of record `slot` (VS window 3, PS window 2) as register images of the GPU
    // record, into `ow` (windows left as they are when the record cannot serve them: `why` set). `vsSet` / `psSet` = the registers the composition wrote
    // (4 x 64 bit masks): the verification compares only those. The GPU copy of a record is the shadow at the present of the frame (the upload list runs
    // before the frame's draws), and a record only changes at its first draw of the frame (the poll), so the draws of a frame see the value they were
    // composed with; see frame-ownership.md 7.8 for the ordering argument.
    // `pad` (F2c-3, owned draws): the images are served over their whole windows when the shader reads registers beyond the pool's count (the record's arrays hold zeros
    // there; a composed draw reads the register file's carry-over)
    void fillWindows(std::uint32_t slot, const std::uint64_t* vsSet, const std::uint64_t* psSet, d3d9::OwnedWindows& ow, bool pad = false);
    std::uint64_t windowDraws() const { return windowDraws_; }
    // F2c-3: the poll of slot `slot`'s record without a composed draw (an owned op): re-reads the record once per frame like checkDraw's poll; false: no such record / unreadable
    bool pollOwned(std::uint32_t slot, TexFn tex, void* texCtx);
    // F2c-2: the record's structure version (texture slots / sampler states, counts, state words, flags, graph ids; not the pool bytes); unique per change
    std::uint32_t structVersion(std::uint32_t slot) const { return slot < structVer_.size() ? structVer_[slot] : 0; }

    // once per present: uploads, read-back verification, idle sweep, the 5 s log
    void flush(d3d9::Device& dev, bool verify);

    rhi::GpuRef ref() const { return table_.ref(); }
    const MaterialStats& stats() const { return stats_; }
    std::uint32_t liveRecords() const { return live_; }

private:
    struct GraphInfo {
        std::uint32_t graph = 0, pipe = 0;
        bool doubleSided = false, zPass = false;
    };
    // all under mutex_
    std::uint32_t lookupOrCreate(const void* key, const void* material);
    // re-reads the record from the engine object; returns false when it could not be read
    bool refresh(std::uint32_t slot, const void* key, const Draw& d);
    const GraphInfo& graphOf(const d3d9::Shader* ps);
    void applyGraph(std::uint32_t slot, const Draw& d);
    void report();
    void reportGraphs();

    bool active_ = false;
    std::uint64_t windowDraws_ = 0;
    std::mutex mutex_;
    GpuTable<MaterialRecord> table_;
    std::unordered_map<const void*, std::uint32_t> slotOf_;
    std::vector<const void*> key_;                 // slot -> key (nullptr = free)
    std::vector<std::uint64_t> seen_;              // slot -> frame last drawn
    std::vector<std::uint64_t> polled_;            // slot -> frame + 1 of the last poll
    std::vector<const void*> lastPs_[2];           // slot -> the last PS shader whose graph was applied (cache by the draw's zPass flag)
    std::vector<std::uint32_t> lastGraph_[2];      // ... and its graph id
    std::vector<std::string> name_;
    std::vector<std::uint32_t> structVer_;
    std::uint32_t structSerial_ = 0;
    std::vector<std::uint32_t> changes_;  // slot -> poll / texture re-uploads (not the first)
    std::vector<std::string> changeWhat_;  // slot -> what changed last ("PS c7", "texture", ...)
    std::vector<std::uint32_t> free_;
    std::uint32_t used_ = 0, live_ = 0, peak_ = 0;
    std::unordered_map<const void*, std::array<std::uint32_t, 2>> pendingSw_;  // switch / native writes noted before the shader's first draw
    std::unordered_map<const void*, GraphInfo> graphs_;  // MaterialSource* -> ids
    struct GraphStat {
        std::string text;  // "file:entry graph..." (truncated) for the log
        bool z = false;
    };
    std::unordered_map<std::uint32_t, GraphStat> graphStats_;  // graph id -> text (also collision check)
    std::unordered_set<std::uint32_t> pipelinesRun_, graphsRun_, frameGraphs_;
    std::uint64_t frameGraphSum_ = 0, frameGraphMax_ = 0, frames_ = 0;
    std::uint64_t frame_ = 0;
    MaterialStats stats_, lastStats_;
    std::uint64_t lastFrame_ = 0, lastBytes_ = 0, lastSlots_ = 0;
    double lastReport_ = 0.0;
    int reports_ = 0;
    int differingLogged_ = 0;
    bool gpuFailed_ = false;
};

MaterialTable& materialtable();

}  // namespace rvk::gpuscene
