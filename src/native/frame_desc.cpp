#include "native/frame_desc.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>

namespace rvk::native {

namespace {

using d3d9::scene::Pass;
using Kind = d3d9::draws::Kind;

constexpr std::size_t kMaxMismatches = 12;

std::size_t idx(List l) { return static_cast<std::size_t>(l); }
std::size_t idx(OpKind k) { return static_cast<std::size_t>(k); }

std::string where(const Op& op) {
    return std::format("{} #{} {} entity {} desc {:#x} key {:#x} sort {:#x}", name(op.list), op.index, name(op.kind), op.entity, op.raw.desc,
                       op.raw.key, op.raw.sortId);
}

}  // namespace

const char* name(List l) {
    switch (l) {
        case List::ZPass: return "ZPass";
        case List::Solid: return "Solid";
        case List::Debug: return "Debug";
        case List::Sky: return "Sky";
        case List::Refracted: return "Refracted";
        case List::Alpha: return "Alpha";
        case List::Late: return "Late";
        case List::Count: break;
    }
    return "?";
}

Pass listPass(List l) {
    switch (l) {
        case List::ZPass: return Pass::DepthPrepass;
        case List::Solid:
        case List::Debug:
        case List::Sky: return Pass::Solid;
        case List::Refracted: return Pass::Refracted;
        case List::Alpha: return Pass::Alpha;
        case List::Late: return Pass::AlphaLate;
        case List::Count: break;
    }
    return Pass::None;
}

const char* name(OpKind k) {
    switch (k) {
        case OpKind::Unknown: return "Unknown";
        case OpKind::Mesh: return "Mesh";
        case OpKind::Animation: return "Animation";
        case OpKind::SpeedTree: return "SpeedTree";
        case OpKind::SpeedTreeBB: return "SpeedTreeBB";
        case OpKind::Vegetation: return "Vegetation";
        case OpKind::Particle: return "Particle";
        case OpKind::Strip: return "Strip";
        case OpKind::LightStreaks: return "LightStreaks";
        case OpKind::Billboard: return "Billboard";
        case OpKind::Precipitation: return "Precipitation";
        case OpKind::Skydome: return "Skydome";
        case OpKind::Debug: return "Debug";
        case OpKind::Count: break;
    }
    return "?";
}

bool nothingToDraw(const Op& op) {
    if (op.state < 0) return false;
    switch (op.kind) {
        case OpKind::Strip: return op.state < 2;          // RenderAlpha 0x701120: cmp [PS+0xe8], 1; jle -> no draw
        case OpKind::Precipitation: return op.state <= 1;  // 0x719b00: test byte [PS+0x18], 1; je -> no draw (0); else 1 + the
                                                           // drops alive (GetNumParticlesAlive 0x71d1e0: PS+0xe0 - PS+0xc8)
        case OpKind::Particle: return op.state == 0;       // no live particle
        case OpKind::SpeedTreeBB: return op.state == 0;    // the billboard mesh resource is not cached (eCResource2 +0x38 != 3)
        case OpKind::Mesh: return op.state != 0xf;         // resource / visual / materials / shaders not all ready (meshState)
        default: return false;
    }
}

bool cannotDraw(const Op& op) {
    if (op.state < 0) return false;
    switch (op.kind) {
        case OpKind::Strip: return op.state < 2;
        case OpKind::Precipitation: return op.state == 0;
        case OpKind::Particle: return op.state == 0;
        default: return false;  // a billboard mesh may be cached between the z pass and the solid pass
    }
}

bool sameBytes(const RawOp& a, const RawOp& b) { return std::memcmp(&a, &b, sizeof(RawOp)) == 0; }

// 0x66e670: cmp a.key, b.key (unsigned): below -> -1, else (above ? 1 : 0)
int compareZ(const RawOp& a, const RawOp& b) { return a.key < b.key ? -1 : a.key > b.key ? 1 : 0; }

// 0x66e6a0: the u64 at +0x10 (key low, sortId high) compared unsigned
int compareSolid(const RawOp& a, const RawOp& b) {
    const std::uint64_t x = static_cast<std::uint64_t>(a.sortId) << 32 | a.key;
    const std::uint64_t y = static_cast<std::uint64_t>(b.sortId) << 32 | b.key;
    return x < y ? -1 : x > y ? 1 : 0;
}

// 0x66e600: L = sortId & 0xfff00000. When either L >= 1000 << 20: L descending first. Then key descending, then sortId ascending.
int compareAlpha(const RawOp& a, const RawOp& b) {
    constexpr std::uint32_t kBand = 0xfff00000u, kHigh = 0x3e800000u;
    const std::uint32_t la = a.sortId & kBand, lb = b.sortId & kBand;
    if (la >= kHigh || lb >= kHigh) {
        if (la > lb) return -1;
        if (la < lb) return 1;
    }
    if (b.key < a.key) return -1;
    if (b.key > a.key) return 1;
    if (a.sortId < b.sortId) return -1;
    if (a.sortId > b.sortId) return 1;
    return 0;
}

std::string noteNames(std::uint16_t notes) {
    static constexpr std::pair<std::uint16_t, const char*> kNames[] = {
        {NoteEmptySlot, "empty slot"},     {NoteNotEntered, "not entered"}, {NoteInstanced, "instanced"},
        {NoteTreeBatch, "tree batches"},   {NoteLightPasses, "light passes"}, {NoteWholeMesh, "whole mesh"},
        {NoteSkyParts, "sky parts"},       {NoteZeroGeometry, "no geometry"}, {NoteHierarchy, "hierarchy node"},
        {NoteBillboardPair, "billboard pair"}, {NoteGrassBatches, "grass batches"}, {NoteZFill, "z-fill pass"},
        {NoteNothingToDraw, "nothing to draw"}, {NoteTreeLod, "tree LOD without geometry"}, {NoteNoQuads, "no quad built"},
        {NoteOverlay, "overlay material passes"},
    };
    std::string s;
    for (const auto& [bit, text] : kNames) {
        if ((notes & bit) == 0) continue;
        if (!s.empty()) s += '+';
        s += text;
    }
    return s.empty() ? std::string("-") : s;
}

void FrameDesc::clear() {
    frame = 0;
    view = 0;
    db = nullptr;
    zPrepass = true;
    valid = false;
    liveData.fill(nullptr);
    count.fill(0);
    first.fill(0);
    ops.clear();
}

Op* FrameDesc::find(const void* opPointer) {
    const auto* p = static_cast<const std::uint8_t*>(opPointer);
    for (std::size_t l = 0; l < kListCount; ++l) {
        const std::uint8_t* base = liveData[l];
        if (base == nullptr || p < base) continue;
        const std::size_t off = static_cast<std::size_t>(p - base);
        if (off >= static_cast<std::size_t>(count[l]) * sizeof(RawOp) || off % sizeof(RawOp) != 0) continue;
        return &ops[first[l] + off / sizeof(RawOp)];
    }
    return nullptr;
}

void TapCounts::add(const TapCounts& o) {
    views += o.views;
    for (std::size_t l = 0; l < kListCount; ++l) {
        ops[l] += o.ops[l];
        for (std::size_t k = 0; k < kOpKindCount; ++k) {
            Bucket& b = buckets[l][k];
            const Bucket& c = o.buckets[l][k];
            b.ops += c.ops;
            b.draws += c.draws;
            b.zero += c.zero;
            b.multi += c.multi;
            b.maxDraws = std::max(b.maxDraws, c.maxDraws);
            b.zeroEmpty += c.zeroEmpty;
            b.zeroNothing += c.zeroNothing;
            b.zeroRule += c.zeroRule;
            b.multiOverlay += c.multiOverlay;
            b.zeroOther += c.zeroOther;
            b.multiBillboard += c.multiBillboard;
            b.multiGrass += c.multiGrass;
            b.multiZFill += c.multiZFill;
            b.multiInstanced += c.multiInstanced;
            b.multiTree += c.multiTree;
            b.multiLights += c.multiLights;
            b.multiWholeMesh += c.multiWholeMesh;
            b.multiSky += c.multiSky;
            b.multiOther += c.multiOther;
        }
    }
    opsTotal += o.opsTotal;
    entered += o.entered;
    notEntered += o.notEntered;
    outOfOrder += o.outOfOrder;
    repeated += o.repeated;
    contentMismatch += o.contentMismatch;
    unknownOps += o.unknownOps;
    countMismatch += o.countMismatch;
    for (std::size_t p = 0; p < drawsByPass.size(); ++p) drawsByPass[p] += o.drawsByPass[p];
    draws += o.draws;
    matched += o.matched;
    drawOrder += o.drawOrder;
    passMismatch += o.passMismatch;
    entityMismatch += o.entityMismatch;
    materialMismatch += o.materialMismatch;
    subMeshMismatch += o.subMeshMismatch;
    unknownOpDraws += o.unknownOpDraws;
    stateContradiction += o.stateContradiction;
    for (std::size_t k = 0; k < noOp.size(); ++k) noOp[k] += o.noOp[k];
    noOpTotal += o.noOpTotal;
}

bool explainedWithoutOp(Kind kind) { return kind == Kind::ShadowMask || kind == Kind::Decal || kind == Kind::Billboard; }

bool TapCounts::clean() const {
    std::uint32_t unexplained = 0;
    for (std::size_t k = 0; k < noOp.size(); ++k) {
        if (!explainedWithoutOp(static_cast<Kind>(k))) unexplained += noOp[k];
    }
    std::uint32_t other = 0;
    for (const auto& list : buckets) {
        for (const Bucket& b : list) other += b.zeroOther + b.multiOther;
    }
    return notEntered == 0 && outOfOrder == 0 && repeated == 0 && contentMismatch == 0 && unknownOps == 0 && countMismatch == 0 &&
           drawOrder == 0 && passMismatch == 0 && entityMismatch == 0 && materialMismatch == 0 && subMeshMismatch == 0 &&
           unknownOpDraws == 0 && stateContradiction == 0 && unexplained == 0 && other == 0;
}

void Attribution::note(std::string text) {
    if (mismatches_.size() < kMaxMismatches) mismatches_.push_back(std::move(text));
}

void Attribution::begin(FrameDesc* frame) {
    frame_ = frame;
    counts_ = {};
    mismatches_.clear();
    current_ = nullptr;
    next_ = 0;
    if (frame_ == nullptr) return;
    counts_.views = 1;
    for (std::size_t l = 0; l < kListCount; ++l) counts_.ops[l] = frame_->count[l];
    counts_.opsTotal = static_cast<std::uint32_t>(frame_->ops.size());
    for (Op& op : frame_->ops) {
        op.entered = op.draws = op.firstDraw = op.instancedDraws = op.additiveDraws = op.noColorDraws = op.otherVsDraws = 0;
        op.firstVs = 0;
        op.brief = {};
        op.notes = 0;
    }
    // without the z pre-pass (caps 0x1590) Execute does not run the z list
    if (!frame_->zPrepass) next_ = frame_->first[idx(List::ZPass)] + frame_->count[idx(List::ZPass)];
}

void Attribution::enterOp(const void* opPointer, const RawOp* live) {
    if (frame_ == nullptr) return;
    Op* op = frame_->find(opPointer);
    if (op == nullptr) {
        ++counts_.unknownOps;
        note(std::format("slot entered with op {} outside the snapshot", opPointer));
        return;
    }
    const std::size_t g = static_cast<std::size_t>(op - frame_->ops.data());
    if (op->entered++ > 0) {
        ++counts_.repeated;
        note(std::format("entered again: {}", where(*op)));
    } else {
        ++counts_.entered;
    }
    if (g != next_) {
        ++counts_.outOfOrder;
        note(std::format("out of order: {} entered, expected {}", where(*op),
                         next_ < frame_->ops.size() ? where(frame_->ops[next_]) : std::string("the end")));
    }
    next_ = g + 1;
    if (live != nullptr && !sameBytes(*live, op->raw)) {
        ++counts_.contentMismatch;
        note(std::format("content differs from the sorted copy: {} (live desc {:#x} key {:#x} sort {:#x} ctx {})", where(*op), live->desc,
                         live->key, live->sortId, live->context));
    }
    current_ = op;
}

void Attribution::draw(const DrawInfo& d) {
    if (frame_ == nullptr) return;
    ++counts_.draws;
    ++counts_.drawsByPass[static_cast<std::size_t>(d.pass)];
    if (d.op == nullptr) {
        ++counts_.noOp[static_cast<std::size_t>(d.kind)];
        ++counts_.noOpTotal;
        if (!explainedWithoutOp(d.kind)) {
            note(std::format("draw #{} without an op: {} in {}, entity {}", d.index, d3d9::draws::name(d.kind), d3d9::scene::name(d.pass),
                             d.entity));
        }
        return;
    }
    Op* op = frame_->find(d.op);
    if (op == nullptr) {
        ++counts_.unknownOpDraws;
        note(std::format("draw #{} with op {} outside the snapshot ({})", d.index, d.op, d3d9::draws::name(d.kind)));
        return;
    }
    ++counts_.matched;
    if (op != current_) {
        ++counts_.drawOrder;
        note(std::format("draw #{} of {} while {} executes", d.index, where(*op), current_ != nullptr ? where(*current_) : std::string("no op")));
    }
    if (op->draws++ == 0) {
        op->firstDraw = d.index;
        op->firstVs = d.vsHash;
    } else if (d.vsHash != op->firstVs) {
        ++op->otherVsDraws;
    }
    if (op->draws <= op->brief.size()) op->brief[op->draws - 1] = {d.vsHash, d.psHash, d.blend, d.colorWrite};
    if (d.instanced) ++op->instancedDraws;
    if (d.additive) ++op->additiveDraws;
    if (!d.colorWrite) ++op->noColorDraws;
    if (d.pass != listPass(op->list)) {
        ++counts_.passMismatch;
        note(std::format("draw #{} of {} in pass {}", d.index, where(*op), d3d9::scene::name(d.pass)));
    }
    if (d.entity != nullptr && op->entity != nullptr && d.entity != op->entity) {
        ++counts_.entityMismatch;
        note(std::format("draw #{} of {}: tag entity {}", d.index, where(*op), d.entity));
    }
    if (d.material != nullptr && op->material != nullptr && d.material != op->material) {
        ++counts_.materialMismatch;
        note(std::format("draw #{} of {}: material {} (op {})", d.index, where(*op), d.material, op->material));
    }
    if (d.subMesh != nullptr && op->subMesh != nullptr && d.subMesh != op->subMesh) {
        ++counts_.subMeshMismatch;
        note(std::format("draw #{} of {}: sub-mesh {} (op {})", d.index, where(*op), d.subMesh, op->subMesh));
    }
}

void Attribution::end(const std::array<std::uint32_t, kListCount>& liveCounts) {
    if (frame_ == nullptr) return;
    for (std::size_t l = 0; l < kListCount; ++l) {
        if (liveCounts[l] != frame_->count[l]) {
            ++counts_.countMismatch;
            note(std::format("list {} has {} ops at Execute's end, {} at its entry", name(static_cast<List>(l)), liveCounts[l], frame_->count[l]));
        }
    }
    for (Op& op : frame_->ops) {
        TapCounts::Bucket& b = counts_.buckets[idx(op.list)][idx(op.kind)];
        ++b.ops;
        b.draws += op.draws;
        b.maxDraws = std::max(b.maxDraws, op.draws);
        const bool skippedList = op.list == List::ZPass && !frame_->zPrepass;
        if (skippedList) continue;  // Execute does not run the z list without the z pre-pass
        if (op.entered == 0) {
            ++counts_.notEntered;
            op.notes |= NoteNotEntered;
            note(std::format("never entered: {}", where(op)));
        }
        if (op.draws > 0 && cannotDraw(op)) {
            ++counts_.stateContradiction;
            note(std::format("drew {} times although its draw condition {} says nothing: {}", op.draws, op.state, where(op)));
        }
        const std::uint32_t node = op.raw.desc >> 8;
        if (op.kind == OpKind::Mesh && node != 0 && node != 0xffffff) op.notes |= NoteHierarchy;
        if (op.draws == 0) {
            ++b.zero;
            if (op.emptySlot) {
                op.notes |= NoteEmptySlot;
                ++b.zeroEmpty;
            } else if (op.entered != 0 && nothingToDraw(op)) {
                op.notes |= NoteNothingToDraw;
                ++b.zeroNothing;
            } else if (op.entered != 0 && op.kind == OpKind::SpeedTree) {
                op.notes |= NoteTreeLod;
                ++b.zeroRule;
            } else if (op.entered != 0 && op.kind == OpKind::Particle) {
                op.notes |= NoteNoQuads;
                ++b.zeroRule;
            } else {
                if (op.entered != 0) op.notes |= NoteZeroGeometry;
                ++b.zeroOther;
                note(std::format("0 draws, not explained: {} (state {:#x})", where(op), op.state));
            }
        } else if (op.draws > 1) {
            ++b.multi;
            bool explained = false;
            if (op.kind == OpKind::Mesh && node == 0xffffff) {
                op.notes |= NoteInstanced;
                ++b.multiInstanced;
                explained = true;
            }
            if (op.kind == OpKind::SpeedTree) {
                op.notes |= NoteTreeBatch;
                ++b.multiTree;
                explained = true;
            }
            if (op.additiveDraws > 0 && op.additiveDraws < op.draws) {
                op.notes |= NoteLightPasses;
                ++b.multiLights;
                explained = true;
            }
            if (op.kind == OpKind::Mesh && op.list == List::ZPass && op.raw.desc == 0xff) {
                op.notes |= NoteWholeMesh;
                ++b.multiWholeMesh;
                explained = true;
            }
            if (op.kind == OpKind::Skydome) {
                op.notes |= NoteSkyParts;
                ++b.multiSky;
                explained = true;
            }
            if (op.kind == OpKind::SpeedTreeBB && op.draws % 2 == 0 && op.otherVsDraws == op.draws / 2) {
                op.notes |= NoteBillboardPair;
                ++b.multiBillboard;
                explained = true;
            }
            if (op.kind == OpKind::Vegetation && op.instancedDraws == op.draws) {
                op.notes |= NoteGrassBatches;
                ++b.multiGrass;
                explained = true;
            }
            if ((op.kind == OpKind::Mesh || op.kind == OpKind::Animation) && op.overlay != 0) {
                op.notes |= NoteOverlay;
                ++b.multiOverlay;
                explained = true;
            }
            if (op.noColorDraws > 0 && op.noColorDraws < op.draws && op.list != List::ZPass) {
                op.notes |= NoteZFill;
                ++b.multiZFill;
                explained = true;
            }
            if (!explained) {
                ++b.multiOther;
                std::string draws;
                for (std::size_t i = 0; i < std::min<std::size_t>(op.draws, op.brief.size()); ++i) {
                    const Op::Brief& br = op.brief[i];
                    draws += std::format(" [vs {:016x} ps {:016x} blend {} {}/{} zfunc {}{}]", br.vs, br.ps, br.blend & 1, (br.blend >> 8) & 0xff,
                                         (br.blend >> 16) & 0xff, br.blend >> 24, br.colorWrite ? "" : " no colour");
                }
                note(std::format("{} draws, not explained: {} (overlay {:#x}, {} instanced, {} additive, {} without colour, {} with another VS){}", op.draws,
                                 where(op), op.overlay, op.instancedDraws, op.additiveDraws, op.noColorDraws, op.otherVsDraws, draws));
            }
        }
    }
    frame_ = nullptr;
    current_ = nullptr;
}

std::string describe(const Op& op) {
    std::string s = std::format("op [{} #{}] {} ctx {} entity {} desc {:#x} fade {:.3f} key {:#08x} sort {:#010x}", name(op.list), op.index,
                                name(op.kind), op.raw.context, op.entity, op.raw.desc, op.raw.fade, op.raw.key, op.raw.sortId);
    if (op.material != nullptr) s += std::format(" material {} shader {}", op.material, op.shader);
    if (op.emptySlot) s += " (empty slot)";
    s += std::format(" -> {} draw{}", op.draws, op.draws == 1 ? "" : "s");
    if (op.draws != 0) s += std::format(" from #{}", op.firstDraw);
    if (op.instancedDraws != 0) s += std::format(", {} instanced", op.instancedDraws);
    if (op.additiveDraws != 0) s += std::format(", {} additive", op.additiveDraws);
    if (op.noColorDraws != 0) s += std::format(", {} without colour", op.noColorDraws);
    if (op.otherVsDraws != 0) s += std::format(", {} with another VS", op.otherVsDraws);
    if (op.state >= 0) s += std::format(", state {}", op.state);
    if (op.overlay != 0) s += std::format(", overlay flags {:#x}", op.overlay);
    if (op.notes != 0) s += " [" + noteNames(op.notes) + "]";
    if (op.entered != 1) s += std::format(" (entered {}x)", op.entered);
    return s;
}

std::vector<std::string> reportLines(const TapCounts& last, std::uint64_t lastFrame, const TapCounts& window,
                                     const std::vector<std::string>& mismatches) {
    std::vector<std::string> out;
    const auto passes = [](const TapCounts& c) {
        std::string s;
        for (std::size_t p = 0; p < c.drawsByPass.size(); ++p) {
            if (c.drawsByPass[p] == 0) continue;
            if (!s.empty()) s += ", ";
            s += std::format("{} {}", d3d9::scene::name(static_cast<Pass>(p)), c.drawsByPass[p]);
        }
        return s.empty() ? std::string("none") : s;
    };
    const auto noOps = [](const TapCounts& c) {
        std::string s;
        for (std::size_t k = 0; k < c.noOp.size(); ++k) {
            if (c.noOp[k] == 0) continue;
            if (!s.empty()) s += ", ";
            s += std::format("{} {}", d3d9::draws::name(static_cast<Kind>(k)), c.noOp[k]);
        }
        return s.empty() ? std::string("none") : s;
    };
    const auto checks = [](const TapCounts& c) {
        return std::format("out of order {}, content differs {}, not entered {}, repeated {}, unknown ops {}, list counts changed {}, "
                           "draw order {}, pass / entity / material / sub-mesh mismatches {} / {} / {} / {}, unknown-op draws {}, "
                           "draw-condition contradictions {}",
                           c.outOfOrder, c.contentMismatch, c.notEntered, c.repeated, c.unknownOps, c.countMismatch, c.drawOrder,
                           c.passMismatch, c.entityMismatch, c.materialMismatch, c.subMeshMismatch, c.unknownOpDraws,
                           c.stateContradiction);
    };
    std::string lists;
    for (std::size_t l = 0; l < kListCount; ++l) {
        lists += std::format("{}{} {}", l == 0 ? "" : ", ", name(static_cast<List>(l)), last.ops[l]);
    }
    out.push_back(std::format("native: frame tap: frame {}: ops {} = {}; draws {} ({}); matched {}, without op {} ({}); {} -> {}", lastFrame,
                              lists, last.opsTotal, last.draws, passes(last), last.matched, last.noOpTotal, noOps(last), checks(last),
                              last.clean() ? "clean" : "NOT clean"));
    out.push_back(std::format("native: frame tap: window of {} main views: ops {}, entered {}, draws {} (matched {}, without op {}: {}); {} -> {}",
                              window.views, window.opsTotal, window.entered, window.draws, window.matched, window.noOpTotal, noOps(window),
                              checks(window), window.clean() ? "clean" : "NOT clean"));
    // per list x kind of the last view: ops, draws, 0-draw and multi-draw ops with their reasons
    std::string buckets;
    for (std::size_t l = 0; l < kListCount; ++l) {
        for (std::size_t k = 0; k < kOpKindCount; ++k) {
            const TapCounts::Bucket& b = last.buckets[l][k];
            if (b.ops == 0) continue;
            std::string zero, multi;
            if (b.zero != 0) {
                zero = std::format(", 0 draws {}", b.zero);
                zero += std::format(" (empty slot {}, nothing to draw {}, by rule {}, OTHER {})", b.zeroEmpty, b.zeroNothing, b.zeroRule,
                                    b.zeroOther);
            }
            if (b.multi != 0) {
                multi = std::format(", >1 draws {} (max {}:", b.multi, b.maxDraws);
                if (b.multiInstanced != 0) multi += std::format(" instanced {}", b.multiInstanced);
                if (b.multiTree != 0) multi += std::format(" tree batches {}", b.multiTree);
                if (b.multiLights != 0) multi += std::format(" light passes {}", b.multiLights);
                if (b.multiWholeMesh != 0) multi += std::format(" whole mesh {}", b.multiWholeMesh);
                if (b.multiSky != 0) multi += std::format(" sky parts {}", b.multiSky);
                if (b.multiBillboard != 0) multi += std::format(" billboard pairs {}", b.multiBillboard);
                if (b.multiGrass != 0) multi += std::format(" grass batches {}", b.multiGrass);
                if (b.multiZFill != 0) multi += std::format(" z-fill {}", b.multiZFill);
                if (b.multiOverlay != 0) multi += std::format(" overlay passes {}", b.multiOverlay);
                if (b.multiOther != 0) multi += std::format(" OTHER {}", b.multiOther);
                multi += ")";
            }
            buckets += std::format("{}{}/{} {} ops {} draws{}{}", buckets.empty() ? "" : "; ", name(static_cast<List>(l)),
                                   name(static_cast<OpKind>(k)), b.ops, b.draws, zero, multi);
        }
    }
    out.push_back("native: frame tap: per list / kind: " + (buckets.empty() ? std::string("no ops") : buckets));
    for (const std::string& m : mismatches) out.push_back("native: frame tap:   " + m);
    return out;
}

}  // namespace rvk::native
