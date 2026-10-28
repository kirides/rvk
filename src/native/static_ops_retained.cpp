// V3 retained composition (docs/research/vulkanize/plan.md "V3 result"): the sub-mesh draw groups of StaticOps::materialDraws /
// specialPassDraw kept across frames. The first composition of a group (key = its arguments + the environment) runs with the
// read log on; the next time the same key comes, the logged engine locations are compared and, when equal, the retained draws
// are taken with only the per-frame values redone (the switch writes into our pool copy, the lighting pool, the mask reference,
// the constants, the draw tag's water class, the image touches). native.retained_check composes fresh as well and compares.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

#include "d3d9/defs.h"
#include "engine/memread.h"
#include "engine/scene_hooks.h"
#include "hal/vulkan/hal_vulkan.h"
#include "native/static_ops.h"

namespace rvk::native {

namespace ret {

std::uint64_t hashBytes(const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    std::uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    while (n >= 8) {
        std::uint64_t w;
        std::memcpy(&w, b, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
        b += 8;
        n -= 8;
    }
    if (n != 0) {
        std::uint64_t t = 0;
        std::memcpy(&t, b, n);
        h = (h ^ t) * 0xC4CEB9FE1A85EC53ull;
        h ^= h >> 29;
    }
    return h;
}

void finalize(std::vector<ReadRec>& v) {
    std::sort(v.begin(), v.end(), [](const ReadRec& a, const ReadRec& b) {
        if (a.addr != b.addr) return a.addr < b.addr;
        if (a.size != b.size) return a.size < b.size;
        return a.kind < b.kind;
    });
    v.erase(std::unique(v.begin(), v.end(),
                        [](const ReadRec& a, const ReadRec& b) { return a.addr == b.addr && a.size == b.size && a.kind == b.kind; }),
            v.end());
    v.shrink_to_fit();
}

bool validate(const ReadRec* r, std::size_t n) {
    __try {
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint8_t* a = r[i].addr;
            if (r[i].kind == 0) {
                switch (r[i].size) {
                    case 1: if (*reinterpret_cast<const volatile std::uint8_t*>(a) != static_cast<std::uint8_t>(r[i].value)) return false; break;
                    case 2: if (*reinterpret_cast<const volatile std::uint16_t*>(a) != static_cast<std::uint16_t>(r[i].value)) return false; break;
                    case 4: if (*reinterpret_cast<const volatile std::uint32_t*>(a) != static_cast<std::uint32_t>(r[i].value)) return false; break;
                    default: if (*reinterpret_cast<const volatile std::uint64_t*>(a) != r[i].value) return false; break;
                }
            } else if (hashBytes(a, r[i].size) != r[i].value) {
                return false;
            }
        }
        return true;
    } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
        return false;
    }
}

}  // namespace ret

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t mix(std::uint64_t h, std::uint64_t w) {
    h = (h ^ w) * 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 31);
}

std::uint64_t ns(Clock::time_point a, Clock::time_point b) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

}  // namespace

void StaticOps::setRetained(bool on, int groups, bool check) {
    if (on != retain_ || static_cast<std::uint32_t>(groups) != retainGroups_ || check != retainCheck_) {
        spdlog::info("native: retained composition {} (groups mask {:#x}, check {})", on ? "on" : "off", groups, check ? "every replay" : "lean check frames");
    }
    if (!on || static_cast<std::uint32_t>(groups) != retainGroups_) {
        // (a group switched off: its entries are not looked at any more; all dropped at the next frame's sweep or here when off)
        retained_.clear();  // (the entries' descs stay in descStore_; unreferenced ones are recycled with the next list)
        if (on == false) {
            descFree_.clear();
            for (auto& d : descStore_) descFree_.push_back(d.get());
            transient_.clear();
        }
    }
    retain_ = on;
    retainGroups_ = static_cast<std::uint32_t>(groups);
    retainCheck_ = check;
}

bool StaticOps::retainOn(std::uint8_t flags) const {
    if (!retain_ || g_ == nullptr || inNonOp_ || flags != 0 || rec_ != nullptr) return false;
    const auto gi = static_cast<std::size_t>(g_ - groups_.data());
    return (retainGroups_ >> gi & 1u) != 0;
}

// the entry's inputs still what they were: the logged engine locations, the pools' pre-state and the textures' registry mapping
bool StaticOps::retValid(RetEntry& e) {
    if (!ret::validate(e.reads.data(), e.reads.size())) return false;
    const Mirror::ShaderPools* p = poolsOf(e.shader);
    if (p->vsFirst != e.vsFirst || p->psFirst != e.psFirst || p->vsCount != e.vsCount || p->psCount != e.psCount || p->slots != e.slots) return false;
    if (std::memcmp(p->slotImage.data(), e.slotImage.data(), sizeof(void*) * 16) != 0) return false;
    if (const std::uint64_t gen = hal_.apiTextureGeneration(); gen != e.texGen) {
        for (const RetTex& t : e.tex) {
            bool ok = false;
            d3d9::Texture* now = textureOfImageData(t.imageData, ok);
            if (now != t.texture || ok != t.ok) return false;
        }
        e.texGen = gen;
    }
    return true;
}

void StaticOps::retReplay(RetEntry& e) {
    Mirror::ShaderPools* pools = poolsOf(e.shader);
    if (g_ != nullptr && e.lightsTotal < std::size(g_->lights) && !e.passes.empty() && !e.passes.front().special) ++g_->lights[e.lightsTotal];
    for (RetPass& p : e.passes) {
        if (!p.special) {
            for (std::uint32_t i = p.sw0; i < p.sw1; ++i) {
                const RetSwitchWrite& w = e.sw[i];
                if (w.kind == 0) std::memcpy(pools->vs[w.idx], w.v, 16);
                else if (w.kind == 1) std::memcpy(pools->ps[w.idx], w.v, 16);
                else pools->slotImage[w.idx] = w.image;
            }
            writeLighting(p.pass);
        }
        if (p.setMask) mirror_.adminPs[5][3] = p.mask;
        sc::DrawDesc& d = *p.desc;
        if (d.vsB != entryVsB_) d.vsB = entryVsB_;
        if (d.psB != entryPsB_) d.psB = entryPsB_;
        if (std::memcmp(d.vsI, entryVsI_, sizeof(d.vsI)) != 0) std::memcpy(d.vsI, entryVsI_, sizeof(d.vsI));
        if (std::memcmp(d.psI, entryPsI_, sizeof(d.psI)) != 0) std::memcpy(d.psI, entryPsI_, sizeof(d.psI));
        constantsInto(d, p.owner, p.bias);
        if (p.hasTag) {
            d3d9::draws::Water water = d3d9::draws::Water::None;
            engine::scene::subMeshWater(d.tagMaterial, d.tagMeshResource, water, d.tagMaterialName);
            d.tagWater = static_cast<std::uint8_t>(water);
        }
        for (std::uint32_t i = p.im0; i < p.im1; ++i) leanImage(e.imgs[i].res, e.imgs[i].data);
        if (drawCount_ == draws_.size()) draws_.push_back(&d);
        else draws_[drawCount_] = &d;
        ++drawCount_;
    }
}

void StaticOps::retFree(std::unique_ptr<RetEntry> e) {
    if (!e) return;
    for (const RetPass& p : e->passes) descFree_.push_back(p.desc);
}

namespace {

// the first difference of two composed draws of one group (empty: equal): everything the issue path reads
std::string diffDescs(const sc::DrawDesc& a, const sc::DrawDesc& b) {
    if (a.vs != b.vs) return "vs";
    if (a.ps != b.ps) return "ps";
    if (a.decl != b.decl) return "decl";
    for (int i = 0; i < 4; ++i) {
        if (!(a.streams[i] == b.streams[i])) return std::format("stream {}", i);
    }
    if (a.indices != b.indices || a.indexed != b.indexed) return "indices";
    if (a.primType != b.primType || a.baseVertex != b.baseVertex || a.minIndex != b.minIndex || a.numVertices != b.numVertices ||
        a.startIndex != b.startIndex || a.primCount != b.primCount) {
        return "range";
    }
    if (a.states != b.states) return "states";
    if (a.samplerMask != b.samplerMask || a.inheritMask != b.inheritMask) return std::format("sampler masks {:#x}/{:#x} vs {:#x}/{:#x}", a.samplerMask, a.inheritMask, b.samplerMask, b.inheritMask);
    for (std::uint32_t st = 0; st < sc::kStages; ++st) {
        if ((a.samplerMask >> st & 1u) == 0 || (a.inheritMask >> st & 1u) != 0) continue;
        if (!(a.samplers[st] == b.samplers[st])) return std::format("sampler s{} (texture {} vs {})", st, a.samplers[st].texture, b.samplers[st].texture);
    }
    if (a.tagSubMesh != b.tagSubMesh || a.tagMaterial != b.tagMaterial || a.tagMeshResource != b.tagMeshResource || a.tagWater != b.tagWater ||
        a.tagMaterialName != b.tagMaterialName) {
        return "tag";
    }
    if (a.passFlags != b.passFlags || a.zPass != b.zPass || a.drawClass != b.drawClass || a.subMesh != b.subMesh || a.material != b.material) return "class";
    if (a.vsLive != b.vsLive || a.psLive != b.psLive) return "live masks";
    if (a.vsSet != b.vsSet || a.psSet != b.psSet) return "register sets";
    std::string why;
    sc::forEachRun(a.vsSet, [&](std::uint32_t r, std::uint32_t n) {
        if (why.empty() && std::memcmp(a.vsF[r], b.vsF[r], 16 * n) != 0) {
            for (std::uint32_t k = r; k < r + n; ++k) {
                if (std::memcmp(a.vsF[k], b.vsF[k], 16) != 0) { why = std::format("vs c{} ({}, {}, {}, {}) vs ({}, {}, {}, {})", k, a.vsF[k][0], a.vsF[k][1], a.vsF[k][2], a.vsF[k][3], b.vsF[k][0], b.vsF[k][1], b.vsF[k][2], b.vsF[k][3]); break; }
            }
        }
    });
    if (!why.empty()) return why;
    sc::forEachRun(a.psSet, [&](std::uint32_t r, std::uint32_t n) {
        if (why.empty() && std::memcmp(a.psF[r], b.psF[r], 16 * n) != 0) {
            for (std::uint32_t k = r; k < r + n; ++k) {
                if (std::memcmp(a.psF[k], b.psF[k], 16) != 0) { why = std::format("ps c{} ({}, {}, {}, {}) vs ({}, {}, {}, {})", k, a.psF[k][0], a.psF[k][1], a.psF[k][2], a.psF[k][3], b.psF[k][0], b.psF[k][1], b.psF[k][2], b.psF[k][3]); break; }
            }
        }
    });
    if (!why.empty()) return why;
    if (a.vsB != b.vsB || a.psB != b.psB || std::memcmp(a.vsI, b.vsI, sizeof(a.vsI)) != 0 || std::memcmp(a.psI, b.psI, sizeof(a.psI)) != 0) return "bool / int registers";
    return {};
}

}  // namespace

template <class F>
StaticOps::Skip StaticOps::retainedRun(RetKey& key, const std::uint8_t* shader, bool special, F&& impl) {
    const std::size_t gi = static_cast<std::size_t>(g_ - groups_.data());
    RetStats& st = retWindow_[gi];
    const bool perf = dev_.perfEnabled();
    const auto t0 = perf ? Clock::now() : Clock::time_point{};
    const std::uint64_t frame = dev_.stats().frames;
    const std::size_t mark = drawCount_;
    RetEntry* old = nullptr;
    std::vector<sc::DrawDesc*> replayed;
    if (const auto it = retained_.find(key.hash); it != retained_.end() && it->second->key == key) old = it->second.get();
    if (old != nullptr) {
        if (old->usedSerial == opSerial_) {
            // the same key twice within one op (equal batches): the second is composed without retaining
            ++st.busy;
            return impl();
        }
        if (retValid(*old)) {
            retReplay(*old);
            old->usedSerial = opSerial_;
            old->lastFrame = frame;
            ++st.hits;
            st.draws += old->passes.size();
            bool check = retainCheck_;
            if (!check && lean_) {
                leanUpdateFrame();
                check = leanFrameKind_ == sc::LeanFrame::Check;
            }
            if (!check) {
                if (perf) st.replayNs += ns(t0, Clock::now());
                return Skip::None;
            }
            ++st.checks;
            replayed.assign(draws_.begin() + static_cast<std::ptrdiff_t>(mark), draws_.begin() + static_cast<std::ptrdiff_t>(drawCount_));
            drawCount_ = mark;
        } else {
            ++st.invalid;
        }
    }
    // compose (with the read log on): the entry to be
    auto ne = std::make_unique<RetEntry>();
    ne->key = key;
    ne->group = static_cast<std::uint8_t>(gi);
    ne->shader = shader;
    ne->texGen = hal_.apiTextureGeneration();
    {
        const ret::NoLog noLog;
        const Mirror::ShaderPools* p = poolsOf(shader);
        ne->vsFirst = p->vsFirst;
        ne->psFirst = p->psFirst;
        ne->vsCount = p->vsCount;
        ne->psCount = p->psCount;
        ne->slots = p->slots;
        ne->slotImage = p->slotImage;
    }
    rec_ = ne.get();
    ret::gLog = &ne->log;
    const Skip s = impl();
    rec_ = nullptr;
    ret::gLog = nullptr;
    const std::size_t made = drawCount_ - mark;
    const bool keep = s == Skip::None && !ne->noRetain && !ne->log.failed && made != 0 && ne->passes.size() == made;
    if (!keep) {
        if (old != nullptr) {
            auto it = retained_.find(key.hash);
            retFree(std::move(it->second));
            retained_.erase(it);
        }
        ++st.skipped;
        if (perf) st.recordNs += ns(t0, Clock::now());
        return s;
    }
    if (!replayed.empty()) {
        // the check: the replayed draws against the freshly composed ones
        std::string why;
        if (replayed.size() != made) {
            why = std::format("{} draws replayed, {} composed", replayed.size(), made);
        } else {
            for (std::size_t i = 0; i < made && why.empty(); ++i) why = diffDescs(*replayed[i], *draws_[mark + i]);
        }
        if (!why.empty()) {
            ++st.stale;
            if (retStaleLogged_ < 24) {
                ++retStaleLogged_;
                std::string mat;
                if (const void* m = draws_[mark]->material; m != nullptr) mat = engine::readCString(static_cast<const std::uint8_t*>(m) + 0x40);
                spdlog::info("native: retained: {} stale ({}), material '{}': {}", name(static_cast<Group>(gi)), special ? "special pass" : "passes", mat, why);
            }
        }
    }
    // adopt the fresh descs (they leave the transient list)
    for (RetPass& p : ne->passes) {
        for (std::size_t i = transient_.size(); i-- > 0;) {
            if (transient_[i] == p.desc) {
                transient_[i] = transient_.back();
                transient_.pop_back();
                break;
            }
        }
    }
    ret::finalize(ne->log.v);
    ne->reads = std::move(ne->log.v);
    ne->log = {};
    ne->usedSerial = opSerial_;
    ne->lastFrame = frame;
    if (old != nullptr) {
        auto it = retained_.find(key.hash);
        retFree(std::move(it->second));
        it->second = std::move(ne);
    } else if (const auto other = retained_.find(key.hash); other != retained_.end()) {
        retFree(std::move(other->second));  // (a different key with the same hash: replaced)
        other->second = std::move(ne);
    } else {
        retained_[key.hash] = std::move(ne);
    }
    ++st.built;
    if (perf) st.recordNs += ns(t0, Clock::now());
    return Skip::None;
}

void StaticOps::retFrameBegin() {
    if (!retain_) return;
    const std::uint64_t frame = dev_.stats().frames;
    if (frame == retFrame_) return;
    retFrame_ = frame;
    ++retWindowFrames_;
    if (frame % 32 != 0 && retained_.size() < 40000) return;
    for (auto it = retained_.begin(); it != retained_.end();) {
        if (it->second->lastFrame + 96 < frame || retained_.size() >= 40000) {
            ++retWindow_[it->second->group].evicted;
            auto e = std::move(it->second);
            it = retained_.erase(it);
            retFree(std::move(e));
        } else {
            ++it;
        }
    }
}

void StaticOps::retReport() {
    if (!retain_) return;
    std::string out;
    RetStats all;
    for (std::size_t gi = 0; gi < kGroups; ++gi) {
        RetStats& w = retWindow_[gi];
        RetStats& t = retTotal_[gi];
        if (w.hits + w.built + w.invalid + w.busy + w.skipped == 0) continue;
        out += std::format("{}{}: hits {} built {} invalid {} busy {} skipped {} evicted {} checks {} stale {}", out.empty() ? "" : "; ", name(static_cast<Group>(gi)), w.hits,
                           w.built, w.invalid, w.busy, w.skipped, w.evicted, w.checks, w.stale);
        for (RetStats* x : {&all, &t}) {
            x->hits += w.hits;
            x->built += w.built;
            x->invalid += w.invalid;
            x->busy += w.busy;
            x->skipped += w.skipped;
            x->checks += w.checks;
            x->stale += w.stale;
            x->draws += w.draws;
            x->evicted += w.evicted;
            x->replayNs += w.replayNs;
            x->recordNs += w.recordNs;
        }
        w = {};
    }
    const double fr = static_cast<double>(std::max<std::uint64_t>(retWindowFrames_, 1));
    spdlog::info("native: retained (5 s, {} frames): {} entries; {}; replay {:.3f} ms / compose+record {:.3f} ms per frame", retWindowFrames_, retained_.size(),
                 out.empty() ? "nothing" : out, static_cast<double>(all.replayNs) * 1e-6 / fr, static_cast<double>(all.recordNs) * 1e-6 / fr);
    retTotalFrames_ += retWindowFrames_;
    retWindowFrames_ = 0;
    RetStats tot;
    for (const RetStats& t : retTotal_) {
        tot.hits += t.hits;
        tot.built += t.built;
        tot.invalid += t.invalid;
        tot.checks += t.checks;
        tot.stale += t.stale;
    }
    spdlog::info("native: retained (total): hits {} built {} invalid {} checks {} stale {}", tot.hits, tot.built, tot.invalid, tot.checks, tot.stale);
}

StaticOps::Skip StaticOps::materialDraws(const std::uint8_t* shader, const std::uint8_t* illum, std::uint32_t combi, bool instanced,
                                         std::int32_t switchValue, std::uint8_t flags, const Geom& geometry, const float* world) {
    if (!retainOn(flags)) return materialDrawsImpl(shader, illum, combi, instanced, switchValue, flags, geometry, world);
    RetKey key;
    {
        std::size_t i = 0;
        auto put = [&](std::uint64_t v) { key.w[i++] = v; };
        auto putp = [&](const void* p) { put(reinterpret_cast<std::uintptr_t>(p)); };
        put(0u | (instanced ? 1u << 8 : 0u) | static_cast<std::uint64_t>(combi) << 32);
        put(static_cast<std::uint32_t>(switchValue));
        putp(shader);
        putp(illum);
        putp(geometry.decl);
        putp(geometry.indices);
        for (const auto& s : geometry.streams) {
            putp(s.buffer);
            put(s.offset | static_cast<std::uint64_t>(s.stride) << 32);
            put(s.freq);
        }
        put(geometry.minIndex | static_cast<std::uint64_t>(geometry.numVertices) << 32);
        put(geometry.startIndex | static_cast<std::uint64_t>(geometry.primCount) << 32);
        put(geometry.primType | static_cast<std::uint64_t>(static_cast<std::uint32_t>(geometry.baseVertex)) << 32);
        put(geometry.visual.fill | static_cast<std::uint64_t>(geometry.visual.cull) << 32);
        put(geometry.visual.depthFunc | static_cast<std::uint64_t>(geometry.visual.depthWrite) << 32);
        putp(geometry.subMesh);
        putp(geometry.material);
        putp(geometry.meshResource);
        std::uint32_t alphaBits;
        std::memcpy(&alphaBits, &mirror_.global[1][3], 4);
        put(alphaBits | static_cast<std::uint64_t>(viewKind_) << 32 | static_cast<std::uint64_t>(static_cast<std::uint32_t>(cascade_) & 0xff) << 40);
        putp(mirror_.lightmapSlot[0]);
        putp(mirror_.lightmapSlot[1]);
        put(snapHash());
        putp(view_.admin);
        put((view_.zColorWrite ? 1u : 0u) | (overlays_ ? 2u : 0u) | (adminStagesUnbound_ ? 4u : 0u) | (g_->compare ? 8u : 0u) |
            static_cast<std::uint64_t>(mirrorMode_) << 8);
        putp(maskRefShader_);
        put(maskRefOverride_);
        putp(blendOverrideShader_);
        put(static_cast<std::uint32_t>(blendOverride_));
        std::uint64_t h = 0x1234567;
        for (std::size_t j = 0; j < i; ++j) h = mix(h, key.w[j]);
        key.hash = h;
    }
    return retainedRun(key, shader, false, [&] { return materialDrawsImpl(shader, illum, combi, instanced, switchValue, flags, geometry, world); });
}

StaticOps::Skip StaticOps::specialPassDraw(const std::uint8_t* shader, const Geom& geometry, bool option) {
    if (!retainOn(0)) return specialPassDrawImpl(shader, geometry, option);
    RetKey key;
    {
        std::size_t i = 0;
        auto put = [&](std::uint64_t v) { key.w[i++] = v; };
        auto putp = [&](const void* p) { put(reinterpret_cast<std::uintptr_t>(p)); };
        put(1u | (option ? 2u << 8 : 0u));
        put(0);
        putp(shader);
        putp(nullptr);
        putp(geometry.decl);
        putp(geometry.indices);
        for (const auto& s : geometry.streams) {
            putp(s.buffer);
            put(s.offset | static_cast<std::uint64_t>(s.stride) << 32);
            put(s.freq);
        }
        put(geometry.minIndex | static_cast<std::uint64_t>(geometry.numVertices) << 32);
        put(geometry.startIndex | static_cast<std::uint64_t>(geometry.primCount) << 32);
        put(geometry.primType | static_cast<std::uint64_t>(static_cast<std::uint32_t>(geometry.baseVertex)) << 32);
        put(geometry.visual.fill | static_cast<std::uint64_t>(geometry.visual.cull) << 32);
        put(geometry.visual.depthFunc | static_cast<std::uint64_t>(geometry.visual.depthWrite) << 32);
        putp(geometry.subMesh);
        putp(geometry.material);
        putp(geometry.meshResource);
        put(static_cast<std::uint64_t>(viewKind_) << 32 | static_cast<std::uint64_t>(static_cast<std::uint32_t>(cascade_) & 0xff) << 40);
        putp(mirror_.lightmapSlot[0]);
        putp(mirror_.lightmapSlot[1]);
        put(snapHash());
        putp(view_.admin);
        put((view_.zColorWrite ? 1u : 0u) | (overlays_ ? 2u : 0u) | (adminStagesUnbound_ ? 4u : 0u) | (g_->compare ? 8u : 0u) |
            static_cast<std::uint64_t>(mirrorMode_) << 8);
        putp(maskRefShader_);
        put(maskRefOverride_);
        putp(blendOverrideShader_);
        put(static_cast<std::uint32_t>(blendOverride_));
        std::uint64_t h = 0x7654321;
        for (std::size_t j = 0; j < i; ++j) h = mix(h, key.w[j]);
        key.hash = h;
    }
    return retainedRun(key, shader, true, [&] { return specialPassDrawImpl(shader, geometry, option); });
}

}  // namespace rvk::native
