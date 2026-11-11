#include "gpuscene/material_table.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"
#include "d3d9/material_source.h"
#include "engine/memread.h"
#include "gpuscene/verify.h"
#include "gpuscene/window_compose.h"

namespace rvk::gpuscene {

namespace {
constexpr std::uint64_t kVerifyEvery = 240;
constexpr std::uint32_t kNone = 0xffffffffu;

std::uint64_t nowNs() { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
}  // namespace

MaterialTable& materialtable() {
    static MaterialTable t;
    return t;
}

void MaterialTable::releaseGpu() {
    std::lock_guard lock(mutex_);
    table_.release();
}

std::uint32_t MaterialTable::lookupOrCreate(const void* key, const void* material) {
    const auto it = slotOf_.find(key);
    if (it != slotOf_.end()) return it->second;
    const std::uint32_t slot = allocSlot(key, material);
    if (slot == kNone) return kNone;
    slotOf_[key] = slot;
    if (const auto pend = pendingSw_.find(key); pend != pendingSw_.end()) {
        MaterialRecord& r = table_.shadow()[slot];
        r.sw[0] = pend->second[0];
        r.sw[1] = pend->second[1];
        pendingSw_.erase(pend);
    }
    return slot;
}

std::uint32_t MaterialTable::lookupOrCreateVariant(const Draw& d) {
    const std::uint64_t vk = (reinterpret_cast<std::uintptr_t>(d.owner) * 0x9E3779B97F4A7C15ull) ^ (d.swHash | 1u);
    if (const auto it = variantOf_.find(vk); it != variantOf_.end()) {
        // (the same key with other writes: a hash collision or element data that moved: the overlay follows the composition, the structure version moves)
        std::vector<SwitchWrite>& o = overlay_[it->second];
        if (o.size() != d.swCount || std::memcmp(o.data(), d.sw, d.swCount * sizeof(SwitchWrite)) != 0) {
            o.assign(d.sw, d.sw + d.swCount);
            polled_[it->second] = 0;
            structVer_[it->second] = ++structSerial_;
        }
        return it->second;
    }
    const std::uint32_t slot = allocSlot(d.owner, d.material);
    if (slot == kNone) return kNone;
    variantOf_[vk] = slot;
    varKey_[slot] = vk;
    overlay_[slot].assign(d.sw, d.sw + d.swCount);
    ++stats_.variantSlots;
    return slot;
}

std::uint32_t MaterialTable::allocSlot(const void* key, const void* material) {
    if (key_.empty()) {
        varKey_.assign(kCapacity, 0);
        overlay_.assign(kCapacity, {});
        key_.assign(kCapacity, nullptr);
        seen_.assign(kCapacity, 0);
        polled_.assign(kCapacity, 0);
        lastPs_[0].assign(kCapacity, nullptr);
        lastPs_[1].assign(kCapacity, nullptr);
        lastGraph_[0].assign(kCapacity, 0);
        lastGraph_[1].assign(kCapacity, 0);
        name_.assign(kCapacity, std::string());
        changes_.assign(kCapacity, 0);
        structVer_.assign(kCapacity, 0);
        changeWhat_.assign(kCapacity, std::string());
        table_.init(kCapacity);
    }
    std::uint32_t slot = kNone;
    if (!free_.empty()) {
        slot = free_.back();
        free_.pop_back();
    } else if (used_ < kCapacity) {
        slot = used_++;
    }
    if (slot == kNone) {
        ++stats_.overflow;
        return kNone;
    }
    std::memset(&table_.shadow()[slot], 0, sizeof(MaterialRecord));
    key_[slot] = key;
    varKey_[slot] = 0;
    overlay_[slot].clear();
    seen_[slot] = frame_;
    polled_[slot] = 0;
    lastPs_[0][slot] = lastPs_[1][slot] = nullptr;
    lastGraph_[0][slot] = lastGraph_[1][slot] = 0;
    changes_[slot] = 0;
    structVer_[slot] = ++structSerial_;
    changeWhat_[slot].clear();
    name_[slot] = material != nullptr ? engine::readCString(static_cast<const std::uint8_t*>(material) + 0x40) : std::string();
    ++live_;
    peak_ = std::max(peak_, live_);
    return slot;
}

void MaterialTable::noteSwitchWrite(const void* shader, int kind, std::uint32_t idx, bool variant) {
    if (!active_ || shader == nullptr) return;
    std::lock_guard lock(mutex_);
    ++(variant ? stats_.switchVariantNotes : stats_.switchNotes);
    std::uint32_t m0 = 0, m1 = 0;
    if (kind == 0 && idx < 32) m0 = 1u << idx;
    else if (kind == 1 && idx < 16) m1 = 1u << idx;
    else if (kind == 2 && idx < 16) m1 = 1u << (16 + idx);
    const auto it = slotOf_.find(shader);
    if (it == slotOf_.end()) {
        // (a shader the composition has not drawn through the static path yet: kept until its record exists)
        auto& p = pendingSw_[shader];
        p[0] |= m0;
        p[1] |= m1;
        return;
    }
    MaterialRecord& r = table_.shadow()[it->second];
    if ((r.sw[0] | m0) != r.sw[0] || (r.sw[1] | m1) != r.sw[1]) {
        r.sw[0] |= m0;
        r.sw[1] |= m1;
        ++r.info[1];
        table_.mark(it->second);
    }
}

bool MaterialTable::refresh(std::uint32_t slot, const void* key, const Draw& d) {
    std::uint8_t img[kMatRawBytes];
    if (!engine::safeRead(key, img, sizeof(img))) {
        ++stats_.readFailed;
        return false;
    }
    const void* vsPtr;
    const void* psPtr;
    std::memcpy(&vsPtr, img + 0xb0, 8);
    std::memcpy(&psPtr, img + 0xe0, 8);
    float vs[kMatVsRegs][4]{}, ps[kMatPsRegs][4]{};
    const std::uint32_t nVs = std::min<std::uint32_t>(img[0xc1], kMatVsRegs), nPs = std::min<std::uint32_t>(img[0xf1], kMatPsRegs);
    if ((nVs != 0 && (vsPtr == nullptr || !engine::safeRead(vsPtr, vs, nVs * 16u))) || (nPs != 0 && (psPtr == nullptr || !engine::safeRead(psPtr, ps, nPs * 16u)))) {
        ++stats_.readFailed;
        return false;
    }
    ++stats_.polls;
    MaterialRecord& cur = table_.shadow()[slot];
    MaterialRecord n = cur;
    fillMaterialRecord(n, img, vs, ps, [&](std::uint32_t stage, const void* res) { return d.tex != nullptr ? d.tex(d.texCtx, stage, res) : kMatNoTex; });
    // F6-2: a switch variant: its value's SetSwitch writes on top of the polled pools (the registers / slot images the switch elements own)
    for (const SwitchWrite& w : overlay_[slot]) {
        if (w.kind == 0 && w.idx < n.counts[1]) {
            std::memcpy(n.vs[w.idx], w.v, 16);
        } else if (w.kind == 1 && w.idx < n.counts[3]) {
            std::memcpy(n.ps[w.idx], w.v, 16);
        } else if (w.kind == 2 && w.idx < kMatSlots && w.idx < ((n.state[2] >> 8) & 0xffu)) {
            n.slots[w.idx].tex = d.tex != nullptr ? d.tex(d.texCtx, (n.state[2] & 0xffu) + w.idx, w.image) : kMatNoTex;
        }
    }
    const bool isNew = cur.info[0] == 0;
    const bool slotsChanged = std::memcmp(n.slots, cur.slots, sizeof(n.slots)) != 0;
    const bool restChanged = std::memcmp(n.counts, cur.counts, sizeof(n.counts)) != 0 || std::memcmp(n.state, cur.state, sizeof(n.state)) != 0 || n.info[3] != cur.info[3] ||
                             std::memcmp(n.vs, cur.vs, sizeof(n.vs)) != 0 || std::memcmp(n.ps, cur.ps, sizeof(n.ps)) != 0;
    if (!isNew && !slotsChanged && !restChanged) {
        ++stats_.pollsUnchanged;
        return true;
    }
    if (!isNew) {
        ++changes_[slot];
        if (slotsChanged) {
            changeWhat_[slot] = "texture / sampler state";
        } else if (std::memcmp(n.vs, cur.vs, sizeof(n.vs)) != 0) {
            for (std::uint32_t i = 0; i < kMatVsRegs; ++i) {
                if (std::memcmp(n.vs[i], cur.vs[i], 16) != 0) {
                    changeWhat_[slot] = std::format("VS c{}", cur.counts[0] + i);
                    break;
                }
            }
        } else if (std::memcmp(n.ps, cur.ps, sizeof(n.ps)) != 0) {
            for (std::uint32_t i = 0; i < kMatPsRegs; ++i) {
                if (std::memcmp(n.ps[i], cur.ps[i], 16) != 0) {
                    changeWhat_[slot] = std::format("PS c{}", cur.counts[2] + i);
                    break;
                }
            }
        } else {
            changeWhat_[slot] = "counts / state / flags";
        }
    }
    // the structure (texture slots / sampler states, counts, state words, flags): not the pool bytes (the time-driven ones change every frame)
    if (isNew || slotsChanged || std::memcmp(n.counts, cur.counts, sizeof(n.counts)) != 0 || std::memcmp(n.state, cur.state, sizeof(n.state)) != 0 || n.info[3] != cur.info[3]) {
        structVer_[slot] = ++structSerial_;
    }
    n.info[0] = 1;
    n.info[1] = cur.info[1] + 1;
    cur = n;
    table_.mark(slot);
    if (isNew) ++stats_.uploadNew;
    else if (slotsChanged) ++stats_.uploadTexture;
    else ++stats_.uploadPoll;
    return true;
}

const MaterialTable::GraphInfo& MaterialTable::graphOf(const d3d9::Shader* ps) {
    static const GraphInfo kNoGraph{};
    const d3d9::MaterialSource* src = ps != nullptr ? ps->source.load(std::memory_order_acquire) : nullptr;
    if (src == nullptr) return kNoGraph;
    const auto it = graphs_.find(src);
    if (it != graphs_.end()) return it->second;
    GraphInfo g;
    MacroList macros;
    macros.reserve(src->macros.size());
    for (const auto& m : src->macros) macros.emplace_back(m.first, m.second);
    const std::string text = graphText(macros, ps->stage == shader::Stage::Vertex);
    g.graph = graphId(src->file, src->entry, text);
    g.pipe = pipelineId(g.graph, staticText(macros));
    g.zPass = src->entry.find("ZPass") != std::string::npos;
    for (const auto& m : macros) {
        if (m.first == "XFX_DOUBLESIDED" && !m.second.empty() && m.second != "0" && m.second != "false") g.doubleSided = true;
    }
    auto [st, inserted] = graphStats_.try_emplace(g.graph);
    if (inserted) {
        st->second.text = std::format("{}:{} {}", src->file, src->entry, text.size() > 90 ? text.substr(0, 87) + "..." : text);
    } else if (!st->second.text.starts_with(src->file + ":" + src->entry)) {
        spdlog::warn("native: material table: graph id {:#x} collides ({} vs {}:{})", g.graph, st->second.text, src->file, src->entry);
    }
    return graphs_.emplace(src, g).first->second;
}

void MaterialTable::applyGraph(std::uint32_t slot, const Draw& d) {
    const std::uint32_t cache = d.zPass ? 1u : 0u;
    MaterialRecord& r = table_.shadow()[slot];
    if (d.ps != nullptr && lastPs_[cache][slot] != d.ps) {
        lastPs_[cache][slot] = d.ps;
        const GraphInfo& g = graphOf(d.ps);
        lastGraph_[cache][slot] = g.graph;
        if (g.graph != 0) {
            // (the slot by the request's entry: a blended material's z-fill pass is a color pass with the PS_DefaultZPass shader)
            const std::uint32_t idx = g.zPass ? 1u : 0u;
            bool changed = false;
            if (r.graph[idx] == 0) {
                r.graph[idx] = g.graph;
                changed = true;
            } else if (r.graph[idx] != g.graph) {
                ++stats_.graphVariants;  // (a material's special passes / permutations name other graphs: the record keeps the first one seen)
            }
            if (g.doubleSided && (r.info[3] & MatDoubleSided) == 0) {
                r.info[3] |= MatDoubleSided;
                changed = true;
            }
            if (changed) {
                ++r.info[1];
                structVer_[slot] = ++structSerial_;
                table_.mark(slot);
                ++stats_.uploadGraph;
            }
            graphsRun_.insert(g.graph);
            pipelinesRun_.insert(g.pipe);
        }
    }
    if (lastGraph_[cache][slot] != 0) frameGraphs_.insert(lastGraph_[cache][slot]);
}

std::uint32_t MaterialTable::checkDraw(const Draw& d, bool verify) {
    if (!active_ || d.owner == nullptr) return kNoSlot;
    const std::uint64_t t0 = verify ? nowNs() : 0;
    std::lock_guard lock(mutex_);
    ++stats_.draws;
    const std::uint32_t slot = d.swCount != 0 && d.sw != nullptr ? lookupOrCreateVariant(d) : lookupOrCreate(d.owner, d.material);
    if (d.swCount != 0 && slot != kNone) ++stats_.variantDraws;
    if (slot == kNone) {
        ++stats_.unchecked;
        return kNoSlot;
    }
    seen_[slot] = frame_;
    if (polled_[slot] != frame_ + 1) {
        polled_[slot] = frame_ + 1;
        if (!refresh(slot, d.owner, d)) {
            ++stats_.unchecked;
            return kNoSlot;
        }
    }
    applyGraph(slot, d);
    const MaterialRecord& rec = table_.shadow()[slot];
    if (rec.info[0] == 0) {
        ++stats_.unchecked;
        return kNoSlot;
    }
    if (!verify) return slot;
    const MaterialDiff df = diffMaterial(rec, d.draw);
    ++stats_.checked;
    stats_.regsCompared += df.regsCompared;
    stats_.regsSwitch += df.regsSwitch;
    stats_.texCompared += df.texCompared;
    stats_.texUnresolved += df.texUnresolved;
    stats_.stateCompared += df.stateCompared;
    stats_.blendCompared += df.blendCompared;
    if (df.regsSwitch != 0) ++stats_.switchDraws;
    if (df.regsDiffer != 0) ++stats_.regsDiffering;
    if (df.texDiffer != 0) ++stats_.texDiffering;
    if (df.stateDiffer != 0) ++stats_.stateDiffering;
    if (df.blendDiffer != 0) ++stats_.blendDiffering;
    if (df.differs() && differingLogged_ < 8) {
        ++differingLogged_;
        std::string what;
        if (df.regsDiffer != 0) {
            const bool ps = df.firstReg >= 1000;
            const std::uint32_t reg = static_cast<std::uint32_t>(ps ? df.firstReg - 1000 : df.firstReg);
            const std::uint32_t first = ps ? rec.counts[2] : rec.counts[0];
            const float* want = (ps ? rec.ps : rec.vs)[reg - first];
            const float* got = ps ? d.draw.psF[reg] : d.draw.vsF[reg];
            what += std::format(" {} c{}: record ({:g} {:g} {:g} {:g}) vs drawn ({:g} {:g} {:g} {:g}) [{} regs differ];", ps ? "PS" : "VS", reg, want[0], want[1], want[2], want[3], got[0],
                                got[1], got[2], got[3], df.regsDiffer);
        }
        if (df.texDiffer != 0) what += std::format(" texture at stage {} differs [{}];", df.firstStage, df.texDiffer);
        if (df.stateDiffer != 0) what += std::format(" sampler state of stage {} (word {}) differs [{}];", df.firstStage, df.firstSsWord, df.stateDiffer);
        if (df.blendDiffer != 0) what += std::format(" blend / mask: record {} / {} vs drawn {} / {};", rec.state[0], rec.state[1], d.draw.blendMode, d.draw.maskRef);
        spdlog::error("native: material table: DIFFERING draw: material '{}' shader {} slot {} version {} (z {}, switch regs vs {:#x} ps {:#x}):{}", name_[slot], d.owner, slot, rec.info[1],
                      d.zPass, rec.sw[0], rec.sw[1], what);
    }
    stats_.checkNs += nowNs() - t0;
    return slot;
}

bool MaterialTable::pollOwned(std::uint32_t slot, TexFn tex, void* texCtx) {
    if (!active_ || slot >= kCapacity) return false;
    std::lock_guard lock(mutex_);
    if (slot >= used_ || key_[slot] == nullptr) return false;
    seen_[slot] = frame_;
    if (polled_[slot] != frame_ + 1) {
        polled_[slot] = frame_ + 1;
        Draw d;
        d.tex = tex;
        d.texCtx = texCtx;
        if (!refresh(slot, key_[slot], d)) return false;
    }
    return table_.shadow()[slot].info[0] != 0;
}

void MaterialTable::fillWindows(std::uint32_t slot, const std::uint64_t* vsSet, const std::uint64_t* psSet, d3d9::OwnedWindows& ow, bool pad) {
    using d3d9::OwnedWindows;
    OwnedWindows::Win& vsWin = ow.w[0][3];
    OwnedWindows::Win& psWin = ow.w[1][2];
    vsWin = OwnedWindows::Win{};
    psWin = OwnedWindows::Win{};
    vsWin.why = psWin.why = OwnedWindows::WhyOther;
    if (!active_ || slot >= kCapacity) return;
    std::lock_guard lock(mutex_);
    if (!table_.ready() || slot >= used_ || key_[slot] == nullptr) return;
    const MaterialRecord& r = table_.shadow()[slot];
    if (r.info[0] == 0) return;
    ++windowDraws_;
    const rhi::GpuRef rec = table_.ref().plus(static_cast<std::int64_t>(slot) * static_cast<std::int64_t>(sizeof(MaterialRecord)));
    const auto maskOf = [](const std::uint64_t* set, std::uint32_t firstReg, std::uint32_t n) {
        std::uint32_t m = 0;
        for (std::uint32_t i = 0; i < n && i < 32; ++i) {
            const std::uint32_t reg = firstReg + i;
            if ((set[reg >> 6] >> (reg & 63) & 1u) != 0) m |= 1u << i;
        }
        return m;
    };
    if (materialVsSwitched(r)) {
        vsWin.why = OwnedWindows::WhySwitch;
    } else if (const WindowImage w = materialVsWindow(r); w.covered != 0) {
        vsWin.kind = OwnedWindows::Kind::Material;
        vsWin.why = OwnedWindows::WhyNone;
        vsWin.base = rec.plus(w.baseDelta);
        vsWin.covered = pad ? std::min<std::uint32_t>(shader::abi::kVsWindowFirst[4] - shader::abi::kVsWindowFirst[3], kMatVsRegs - w.imageIndex) : w.covered;
        vsWin.image = r.vs[w.imageIndex];
        if (windowMasksWanted()) vsWin.verifyMask = maskOf(vsSet, shader::abi::kVsWindowFirst[3], w.covered);
    }
    if (materialPsSwitched(r)) {
        psWin.why = OwnedWindows::WhySwitch;
    } else if (const WindowImage w = materialPsWindow(r); w.covered != 0) {
        psWin.kind = OwnedWindows::Kind::Material;
        psWin.why = OwnedWindows::WhyNone;
        psWin.base = rec.plus(w.baseDelta);
        psWin.covered = pad ? std::min<std::uint32_t>(shader::abi::kPsWindowFirst[3] - shader::abi::kPsWindowFirst[2], kMatPsRegs - w.imageIndex) : w.covered;
        psWin.image = r.ps[w.imageIndex];
        if (windowMasksWanted()) psWin.verifyMask = maskOf(psSet, shader::abi::kPsWindowFirst[2], w.covered);
    }
}

void MaterialTable::flush(d3d9::Device& dev, bool verify) {
    ++frame_;
    if (!active_) return;
    std::lock_guard lock(mutex_);
    if (!frameGraphs_.empty()) {
        frameGraphSum_ += frameGraphs_.size();
        frameGraphMax_ = std::max<std::uint64_t>(frameGraphMax_, frameGraphs_.size());
        ++frames_;
        frameGraphs_.clear();
    }
    if (used_ == 0) return;
    if (!gpuFailed_ && !table_.ready()) {
        if (!table_.create(dev, kCapacity, "material table", kTablePad)) {
            gpuFailed_ = true;
            spdlog::warn("native: material table: the GPU buffer could not be created (the CPU records and the draw check keep running)");
        } else {
            table_.markAll(used_);
        }
    }
    if (frame_ % 300 == 0) {
        for (std::uint32_t s = 0; s < used_; ++s) {
            if (key_[s] == nullptr || frame_ - seen_[s] < kIdleFrames) continue;
            if (varKey_[s] != 0) {
                variantOf_.erase(varKey_[s]);
                varKey_[s] = 0;
                overlay_[s].clear();
            } else if (const auto it = slotOf_.find(key_[s]); it != slotOf_.end() && it->second == s) {
                slotOf_.erase(it);
            }
            key_[s] = nullptr;
            std::memset(&table_.shadow()[s], 0, sizeof(MaterialRecord));
            table_.mark(s);
            free_.push_back(s);
            --live_;
            ++stats_.freed;
        }
    }
    if (!gpuFailed_) {
        table_.flush(dev, verify && frame_ % kVerifyEvery == 0, used_);
        table_.collect(dev.game(), "material table");
    }
    report();
}

void MaterialTable::report() {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - lastReport_ < 5.0) return;
    lastReport_ = now;
    ++reports_;
    const auto& ts = table_.stats();
    const std::uint64_t frames = std::max<std::uint64_t>(frame_ - lastFrame_, 1);
    const MaterialStats& s = stats_;
    const MaterialStats& p = lastStats_;
    spdlog::info(
        "native: material table (5 s): {} records (peak {}, slots {}); uploads: new {}, poll {}, texture {}, graph {} (other graph variants seen {}) ({:.2f} records / {:.2f} KiB per frame, {} flushed); polls {} ({} unchanged), switch "
        "notes {} (switch variants: {} slots made, {} draws, {} writes held); freed {}; draws {} checked {} (unchecked {}, with switch registers {}): registers {} compared, {} switch-skipped, DIFFERING {}; textures {} compared ({} unresolved), "
        "sampler states {}, DIFFERING {} + {}; blend / mask {} compared, DIFFERING {}; verify: {} read-backs ({} slots) DIFFERING {}; graphs: {} ids, {} pipelines (run), {:.1f} per frame (max {}); "
        "overflow {} gpu {}, read failures {}; check {:.3f} ms per frame",
        live_, peak_, used_, s.uploadNew, s.uploadPoll, s.uploadTexture, s.uploadGraph, s.graphVariants, static_cast<double>(ts.slotsUploaded - lastSlots_) / static_cast<double>(frames),
        static_cast<double>(ts.bytes - lastBytes_) / 1024.0 / static_cast<double>(frames), ts.slotsUploaded, s.polls, s.pollsUnchanged, s.switchNotes, s.variantSlots, s.variantDraws, s.switchVariantNotes, s.freed, s.draws, s.checked, s.unchecked,
        s.switchDraws, s.regsCompared, s.regsSwitch, s.regsDiffering, s.texCompared, s.texUnresolved, s.stateCompared, s.texDiffering, s.stateDiffering, s.blendCompared, s.blendDiffering,
        ts.verifyDone, ts.verifySlots, ts.verifyBad, graphsRun_.size(), pipelinesRun_.size(), frames_ != 0 ? static_cast<double>(frameGraphSum_) / static_cast<double>(frames_) : 0.0, frameGraphMax_,
        s.overflow, table_.stats().overflow, s.readFailed, static_cast<double>(s.checkNs - p.checkNs) / 1e6 / static_cast<double>(frames));
    lastStats_ = stats_;
    lastFrame_ = frame_;
    lastBytes_ = ts.bytes;
    lastSlots_ = ts.slotsUploaded;
    if (reports_ % 6 == 0) reportGraphs();
}

void MaterialTable::reportGraphs() {
    std::unordered_map<std::uint32_t, std::uint32_t> perGraph, perZ;
    for (std::uint32_t s = 0; s < used_; ++s) {
        if (key_[s] == nullptr) continue;
        const MaterialRecord& r = table_.shadow()[s];
        if (r.graph[0] != 0) ++perGraph[r.graph[0]];
        if (r.graph[1] != 0) ++perZ[r.graph[1]];
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> top(perGraph.begin(), perGraph.end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    std::string line = std::format("native: material graphs: {} live records, {} distinct color-pass graph ids ({} z-pass), top 10 by materials sharing:", live_, perGraph.size(), perZ.size());
    for (std::size_t i = 0; i < top.size() && i < 10; ++i) {
        line += std::format("\n  {:>4} materials  {:#010x}  {}", top[i].second, top[i].first, graphStats_[top[i].first].text);
    }
    // the materials re-uploaded most often (a poll found other bytes)
    std::vector<std::pair<std::uint32_t, std::uint32_t>> chg;
    std::uint32_t changing = 0;
    for (std::uint32_t s = 0; s < used_; ++s) {
        if (key_[s] == nullptr || changes_[s] == 0) continue;
        chg.emplace_back(changes_[s], s);
        ++changing;
    }
    std::sort(chg.begin(), chg.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    line += std::format("\n  materials re-uploaded after their first upload: {} of {} live; most often:", changing, live_);
    for (std::size_t i = 0; i < chg.size() && i < 6; ++i) {
        line += std::format("\n  {:>8} x '{}' ({})", chg[i].first, name_[chg[i].second].empty() ? std::string("?") : name_[chg[i].second], changeWhat_[chg[i].second]);
    }
    spdlog::info("{}", line);
}

}  // namespace rvk::gpuscene
