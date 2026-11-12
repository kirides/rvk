#include "enh/light_table.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "enh/rhi_scope.h"
#include "enh/rt_scene.h"
#include "shader/embedded.h"

namespace rvk::enh {

namespace {
constexpr float kSpecularShare = 0.5f;   // specular colour = diffuse x this (the engine's c27 of point lights is a share of c26; measured in the LogFrame constants)
constexpr std::uint32_t kClusters0 = kLightGridX * kLightGridY * kLightGridZ;
constexpr std::uint32_t kClusters1 = kLightGrid1X * kLightGrid1Y * kLightGrid1Z;
constexpr std::uint32_t kClusters = kClusters0 + kClusters1;
constexpr std::uint64_t kLightsOffset = kLightHeaderBytes;
constexpr std::uint64_t kClustersOffset = kLightsOffset + std::uint64_t{kLightMax} * kLightBytes;
constexpr std::uint64_t kClusters1Offset = kClustersOffset + std::uint64_t{kClusters0} * kLightClusterBytes;
constexpr std::uint64_t kBufferBytes = kClustersOffset + std::uint64_t{kClusters} * kLightClusterBytes;

std::atomic<bool> g_fullLit{false};
std::atomic<bool> g_registryThin{true};
std::atomic<bool> g_registryComplete{false};
std::atomic<std::uint64_t> g_registryGeneration{1};
std::atomic<std::uint64_t> g_knownHash{0};
std::mutex g_drawnMutex;
struct DrawnLight {
    engine::lights::WorldLight l;
    std::uint64_t stamp = 0;  // the build counter at the last note
};
std::vector<DrawnLight> g_drawn;  // lights of the lists drawn lately (kept while re-noted: replayed packets do not note again), merged into every build
std::uint64_t g_buildCounter = 0;
constexpr std::uint64_t kDrawnKeep = 900;  // builds
}  // namespace

bool fullLitActive() { return g_fullLit.load(std::memory_order_relaxed); }
void setFullLitActive(bool on) { g_fullLit.store(on, std::memory_order_relaxed); }
namespace {
std::atomic<bool> g_skipReads{true}, g_treeMerge{true};
}
bool fullLitSkipsReads() { return g_skipReads.load(std::memory_order_relaxed); }
bool fullLitTreeMerge() { return g_treeMerge.load(std::memory_order_relaxed); }
void setFullLitOptions(bool skipReads, bool treeMerge) {
    g_skipReads.store(skipReads, std::memory_order_relaxed);
    g_treeMerge.store(treeMerge, std::memory_order_relaxed);
}

namespace {
// the last build's light positions in 16-unit cells (lightKnown)
struct KnownGrid {
    std::vector<std::pair<std::uint64_t, std::array<float, 3>>> cells;  // sorted by key (binary search; no node allocations)
};
std::atomic<std::shared_ptr<const KnownGrid>> g_known;
std::uint64_t knownKey(std::int32_t x, std::int32_t y, std::int32_t z) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) * 73856093ull) ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) * 19349663ull) ^
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z)) * 83492791ull);
}
}  // namespace

bool LightTable::lightKnown(const float pos[3]) {
    const auto g = g_known.load();
    if (!g || g->cells.empty()) return false;
    const std::int32_t cx = static_cast<std::int32_t>(std::floor(pos[0] / 16.0f)), cy = static_cast<std::int32_t>(std::floor(pos[1] / 16.0f)),
                       cz = static_cast<std::int32_t>(std::floor(pos[2] / 16.0f));
    for (std::int32_t dx = -1; dx <= 1; ++dx) {
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            for (std::int32_t dz = -1; dz <= 1; ++dz) {
                const std::uint64_t key = knownKey(cx + dx, cy + dy, cz + dz);
                auto it = std::lower_bound(g->cells.begin(), g->cells.end(), key, [](const auto& c, std::uint64_t k) { return c.first < k; });
                for (; it != g->cells.end() && it->first == key; ++it) {
                    if (std::abs(it->second[0] - pos[0]) < 8.0f && std::abs(it->second[1] - pos[1]) < 8.0f && std::abs(it->second[2] - pos[2]) < 8.0f) return true;
                }
            }
        }
    }
    return false;
}

bool LightTable::registryComplete() { return g_registryComplete.load(std::memory_order_relaxed); }

std::uint64_t LightTable::registryGeneration() { return g_registryGeneration.load(std::memory_order_relaxed); }

void registryNoteComplete(bool complete) {
    if (g_registryComplete.exchange(complete, std::memory_order_relaxed) != complete) g_registryGeneration.fetch_add(1, std::memory_order_relaxed);
}

void registryNoteKnown(std::uint64_t positionsHash) {
    if (g_knownHash.exchange(positionsHash, std::memory_order_relaxed) != positionsHash) g_registryGeneration.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t registryPositionHash(const float (*positions)[4], std::uint32_t count) {
    std::uint64_t sum = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t b[3];
        std::memcpy(b, positions[i], 12);
        std::uint64_t h = 0x9E3779B97F4A7C15ull;
        for (const std::uint32_t w : b) {
            h = (h ^ w) * 0xC2B2AE3D27D4EB4Full;
            h ^= h >> 29;
        }
        sum += h;
    }
    return sum ^ (static_cast<std::uint64_t>(count) << 48);
}

void registryResetForTest() {
    g_registryComplete.store(false);
    g_knownHash.store(0);
    g_registryGeneration.store(1);
}

bool LightTable::wantStaticNotes() { return true; }  // always: the drawn lights also give the registry's static lights their draw colour

void LightTable::noteDrawnLight(const float pr[4], const float col[4]) {
    if (!(pr[3] > 0.0f) || !std::isfinite(pr[3]) || !(col[0] + col[1] + col[2] > 1e-4f)) return;
    const std::lock_guard lock(g_drawnMutex);
    for (auto& d : g_drawn) {
        if (std::abs(d.l.pos[0] - pr[0]) < 1.0f && std::abs(d.l.pos[1] - pr[1]) < 1.0f && std::abs(d.l.pos[2] - pr[2]) < 1.0f) {
            std::memcpy(d.l.col, col, 12);
            d.stamp = g_buildCounter;
            return;
        }
    }
    if (g_drawn.size() >= 512) return;
    engine::lights::WorldLight l;
    std::memcpy(l.pos, pr, 12);
    l.range = 1.0f / std::sqrt(pr[3]);
    std::memcpy(l.col, col, 12);
    l.dynamic = true;
    g_drawn.push_back({l, g_buildCounter});
}

float lightSliceScale(float nearZ, float farZ, std::uint32_t slices) { return static_cast<float>(slices) / std::log(farZ / nearZ); }

std::uint32_t lightSlice(float viewZ, float nearZ, float slicesPerLn, std::uint32_t slices) {
    const float s = std::log(std::max(viewZ, nearZ) / nearZ) * slicesPerLn;
    return std::min(static_cast<std::uint32_t>(s), slices - 1u);
}

float lightSliceNear(std::uint32_t slice, float nearZ, float slicesPerLn) { return nearZ * std::exp(static_cast<float>(slice) / slicesPerLn); }

float lightShadowEncode(std::uint32_t id, float vis) { return static_cast<float>(id) + std::floor(std::clamp(vis, 0.0f, 1.0f) * 63.0f + 0.5f) / 64.0f; }
std::uint32_t lightShadowId(float e) { return static_cast<std::uint32_t>(std::floor(e + 0.0001f)); }
float lightShadowVis(float e) { return std::floor((e + 0.0001f - std::floor(e + 0.0001f)) * 64.0f) / 63.0f; }

float lightEngineFalloff(float d2, float rcpSqrRange) { return 1.0f - std::clamp(d2 * rcpSqrRange, 0.0f, 1.0f); }

float lightPhysicalRatio(float d2, float rcpSqrRange) {
    const float u = std::clamp(d2 * rcpSqrRange, 0.0f, 1.0f);
    const float r0sq = 0.04f / std::max(rcpSqrRange, 1e-12f);
    return 4.6297f * (1.0f - u) / (1.0f + d2 / r0sq);
}

LightTable::LightTable(d3d9::Device& device) : dev_(device) {
    rhi::Device* rdev = dev_.rhi();
    if (rdev == nullptr) {
        failed_ = true;
        return;
    }
    device_ = rdev->createBuffer({kBufferBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Device, "light table"});
    for (auto& u : upload_) u = rdev->createBuffer({std::uint64_t{kLightMax} * kLightBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "light table upload"});
    if (!device_ || std::any_of(upload_.begin(), upload_.end(), [](const auto& u) { return !u; })) {
        failed_ = true;
        return;
    }
    // the reference never changes: it is in every FrameBlock from the next upload on
    const auto ref = device_->shaderRef(0);
    float a, b;
    std::memcpy(&a, &ref[0], 4);
    std::memcpy(&b, &ref[1], 4);
    refA_ = a;
    refB_ = b;
    dev_.setFrameParam(shader::abi::kFrameParamLights, {a, b, shadow_.mode > 0 ? shadow_.baked : 0.0f, 0.0f});
    dev_.setFrameParam(shader::abi::kFrameParamLights + 1, {0.0f, 0.0f, 0.0f, 0.0f});
    // the compute pipeline in the background from the start (Device::createComputePipeline is thread-safe)
    building_ = std::async(std::launch::async, [rdev]() {
        rhi::ComputePipelineDesc d;
        d.cs.spirv = shader::embedded::spirv("lighting/light_cluster.comp");
        d.cs.dxil = shader::embedded::dxilOnDemand("lighting/light_cluster.comp");
        d.name = "light.cluster";
        return rdev->createComputePipeline(d);
    });
}

LightTable::~LightTable() {
    shadow_.mode = 0;
    configure(false, 0, 0);
    for (auto& b : shadowBuilding_) {
        if (b.valid()) b.get();
    }
    if (building_.valid()) building_.get();
}

// ray-traced lamp shadows: held (RT scene, pass, pipelines) only while the table is on and the setting asks for them; off = nothing exists
void LightTable::configureShadows(const ShadowSettings& s) {
    shadow_ = s;
    if (!failed_ && (refA_ != 0.0f || refB_ != 0.0f)) dev_.setFrameParam(shader::abi::kFrameParamLights, {refA_, refB_, s.mode > 0 ? std::clamp(s.baked, 0.0f, 1.0f) : 0.0f, 0.0f});
    applyShadowHold();
}

void LightTable::applyShadowHold() {
    const bool want = enabled_ && shadow_.mode > 0 && scene_ != nullptr && scene_->supported() && !shadowFailed_ && !failed_;
    if (want && !shadowHeld_) {
        rhi::Device* rdev = dev_.rhi();
        if (rdev == nullptr) {
            shadowFailed_ = true;
            return;
        }
        static constexpr const char* kShaders[2] = {"lighting/light_shadow_trace.comp", "lighting/light_shadow_filter.comp"};
        static constexpr const char* kNames[2] = {"light.shadow trace", "light.shadow filter"};
        for (int i = 0; i < 2; ++i) {
            if (shadowPipeline_[i] || shadowBuilding_[i].valid()) continue;
            shadowBuilding_[i] = std::async(std::launch::async, [rdev, i]() {
                rhi::ComputePipelineDesc d;
                d.cs.spirv = shader::embedded::spirv(kShaders[i]);
                d.cs.dxil = shader::embedded::dxilOnDemand(kShaders[i]);
                d.name = kNames[i];
                return rdev->createComputePipeline(d);
            });
        }
        scene_->acquire(this);
        scene_->setDynamic(this, true);  // characters cast lamp shadows
        shadowPassId_ = dev_.addPass(d3d9::inject::Point::AfterDepthPrepass, "light.shadow", [this](d3d9::PassContext& ctx) { recordShadow(ctx); }, &shadowPassId_, 5);
        shadowHeld_ = true;
        spdlog::info("enh: lights: lamp shadows on (mode {}, quality {}, lamp radius {:.0f} units)", shadow_.mode == 2 ? "many" : "nearby", shadow_.quality,
                     shadow_.radius);
    } else if (!want && shadowHeld_) {
        dev_.removePasses(&shadowPassId_);
        shadowPassId_ = 0;
        pendingShadow_ = {};
        scene_->setDynamic(this, false);
        scene_->release(this);
        for (const char* k : {"light.shadow raw", "light.shadow final"}) dev_.releaseTransient(k);
        shadowHeld_ = false;
        shadowLights_ = 0;
        spdlog::info("enh: lights: lamp shadows off");
    }
}

bool LightTable::shadowPipelinesReady() {
    if (shadowFailed_) return false;
    bool all = true;
    for (int i = 0; i < 2; ++i) {
        if (shadowPipeline_[i]) continue;
        if (!shadowBuilding_[i].valid() || shadowBuilding_[i].wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            all = false;
            continue;
        }
        shadowPipeline_[i] = shadowBuilding_[i].get();
        if (!shadowPipeline_[i]) {
            spdlog::error("enh: lights: a lamp shadow pipeline failed: lamp shadows stay off this session");
            shadowFailed_ = true;
            return false;
        }
    }
    return all;
}

void LightTable::configure(bool enabled, int style, int debug, float defaultRoughness) {
    defaultRoughness_ = defaultRoughness;
    if (failed_) enabled = false;
    if (!enabled) registryNoteComplete(false);
    style_ = style;
    debug_ = debug;
    if (!failed_) dev_.setFrameParam(shader::abi::kFrameParamLights + 1, {enabled ? 1.0f : 0.0f, static_cast<float>(style), static_cast<float>(debug), defaultRoughness_});
    if (enabled && !enabled_) {
        passId_ = dev_.addPass(d3d9::inject::Point::AfterDepthPrepass, "light.cluster", [this](d3d9::PassContext& ctx) { record(ctx); }, this);
        spdlog::info("enh: lights: light table on (style {}, {} x {} x {} clusters, up to {} lights)", style == 1 ? "physical" : "original", kLightGridX, kLightGridY,
                     kLightGridZ, kLightMax);
    } else if (!enabled && enabled_) {
        if (passId_ != 0) dev_.removePass(passId_);
        passId_ = 0;
        spdlog::info("enh: lights: light table off");
    }
    enabled_ = enabled;
    applyShadowHold();
}

bool LightTable::pipelineReady() {
    if (pipeline_) return true;
    if (failed_ || !building_.valid()) return false;
    if (building_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    pipeline_ = building_.get();
    if (!pipeline_) {
        spdlog::error("enh: lights: the cluster pipeline failed: the light table stays off this session");
        failed_ = true;
        return false;
    }
    spdlog::info("enh: lights: cluster pipeline ready");
    return true;
}

void LightTable::record(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || !enabled_ || !pipelineReady()) {
        registryNoteComplete(false);
        ++skipped_;
        return;
    }
    const auto& cam = dev_.camera().valid ? dev_.camera() : dev_.lastCamera();
    if (!cam.valid) {
        registryNoteComplete(false);
        ++skipped_;
        return;
    }
    // the world's lights within the grid's reach, nearest first when there are more than the table holds
    if (!engine::lights::collect(cam.position.data(), collectRadius_, scratch_, stats_)) {
        registryNoteComplete(false);
        ++skipped_;
        return;
    }
    // "the registry holds lights" means lights that reach the NEAR level (the old collection range, kLightFar): the far lights of the whole island (the far level,
    // up to kLightFar1) are in the tree even where nothing is cached in around the camera, and must not hide the objects' own lights (a teleport into the
    // monastery: the candles are only in the drawn lists)
    std::uint32_t nearStatic = 0, nearAny = 0;
    for (const auto& w : scratch_) {
        const float dx = w.pos[0] - cam.position[0], dy = w.pos[1] - cam.position[1], dz = w.pos[2] - cam.position[2];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) <= kLightFar + w.range) {
            ++nearAny;
            if (!w.dynamic) ++nearStatic;
        }
    }
    lastNearStatic_ = nearStatic;
    lastNearAny_ = nearAny;
    g_registryThin.store(nearStatic == 0, std::memory_order_relaxed);
    // the registry (static tree within reach + this frame's dynamic lights) is what the engine gives objects too: a non-empty registry is trusted whole
    registryNoteComplete(nearAny != 0);
    // the lights of the lists drawn since the last build that the registry did not return (same light = within 8 units)
    lastDrawnAdded_ = 0;
    {
        const std::lock_guard lock(g_drawnMutex);
        const std::size_t registry = scratch_.size();
        ++g_buildCounter;
        std::erase_if(g_drawn, [](const DrawnLight& d) { return g_buildCounter - d.stamp > kDrawnKeep; });
        // the registry's lights in 16-unit cells: a drawn light is "the same" within 8 units. The static lights of the registry carry the colour the
        // engine's DRAWS give them: the registry's rgb x intensity x scale x 0.5 is a guess, the draws' register colour differs per light by 0.2 .. 2.0
        // (measured: gpu-scene.md 9.10; the engine weights each static light), so a matched light takes the colour of the draw that was seen last
        // a sorted (cell key, light index) vector kept between builds (no node allocations); equal keys stay in index order = the multimap's insertion order
        static std::vector<std::pair<std::uint64_t, std::uint32_t>> cells;  // under g_drawnMutex
        cells.clear();
        cells.reserve(registry);
        for (std::uint32_t i = 0; i < registry; ++i) {
            const auto& w = scratch_[i];
            cells.emplace_back(knownKey(static_cast<std::int32_t>(std::floor(w.pos[0] / 16.0f)), static_cast<std::int32_t>(std::floor(w.pos[1] / 16.0f)),
                                        static_cast<std::int32_t>(std::floor(w.pos[2] / 16.0f))),
                               i);
        }
        std::sort(cells.begin(), cells.end());
        for (const auto& dl : g_drawn) {
            const auto& d = dl.l;
            const std::int32_t cx = static_cast<std::int32_t>(std::floor(d.pos[0] / 16.0f)), cy = static_cast<std::int32_t>(std::floor(d.pos[1] / 16.0f)),
                               cz = static_cast<std::int32_t>(std::floor(d.pos[2] / 16.0f));
            bool known = false;
            for (std::int32_t dx = -1; dx <= 1 && !known; ++dx) {
                for (std::int32_t dy = -1; dy <= 1 && !known; ++dy) {
                    for (std::int32_t dz = -1; dz <= 1 && !known; ++dz) {
                        const std::uint64_t cellKey = knownKey(cx + dx, cy + dy, cz + dz);
                        auto it = std::lower_bound(cells.begin(), cells.end(), std::pair<std::uint64_t, std::uint32_t>{cellKey, 0u});
                        for (; it != cells.end() && it->first == cellKey && !known; ++it) {
                            auto& w = scratch_[it->second];
                            if (!(std::abs(w.pos[0] - d.pos[0]) < 8.0f && std::abs(w.pos[1] - d.pos[1]) < 8.0f && std::abs(w.pos[2] - d.pos[2]) < 8.0f)) continue;
                            known = true;
                            if (!w.dynamic && w.col[0] > 1e-4f && w.col[1] > 1e-4f && w.col[2] > 1e-4f && d.col[0] + d.col[1] + d.col[2] > 1e-4f) {
                                for (int c = 0; c < 3; ++c) colourRatio_[c] += d.col[c] / w.col[c];
                                ++colourRatioN_;
                                std::memcpy(w.col, d.col, sizeof(w.col));
                            }
                        }
                    }
                }
            }
            if (!known) scratch_.push_back(d);
        }
        lastDrawnAdded_ = static_cast<std::uint32_t>(scratch_.size() - registry);
    }
    const auto reach = [&](const engine::lights::WorldLight& l) {
        const float dx = l.pos[0] - cam.position[0], dy = l.pos[1] - cam.position[1], dz = l.pos[2] - cam.position[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz) - l.range;
    };
    lastDropped_ = 0;
    lastNear_ = 0;
    if (scratch_.size() > kLightMax) {
        lastDropped_ = static_cast<std::uint32_t>(scratch_.size()) - kLightMax;
        // the lights that reach the near level (the set the table held before the far level existed) come first and keep their order; the far lights of the
        // island only fill the capacity that is left, nearest first: they never push a local lamp out
        const auto mid = std::stable_partition(scratch_.begin(), scratch_.end(), [&](const auto& l) { return reach(l) <= kLightFar; });
        const std::size_t nearCount = static_cast<std::size_t>(mid - scratch_.begin());
        lastNear_ = static_cast<std::uint32_t>(nearCount);
        const auto byReach = [&](const auto& a, const auto& b) { return reach(a) < reach(b); };
        if (nearCount >= kLightMax) {
            std::nth_element(scratch_.begin(), scratch_.begin() + kLightMax, mid, byReach);
        } else {
            std::nth_element(mid, scratch_.begin() + kLightMax, scratch_.end(), byReach);
        }
        scratch_.resize(kLightMax);
    }
    const std::uint32_t count = static_cast<std::uint32_t>(scratch_.size());
    // lamp shadows: the nearest lights within the mode's reach get the ids 1..N (the pass light.shadow traces them)
    std::vector<std::uint32_t> shadowId(count, 0);
    shadowLights_ = 0;
    if (shadowHeld_ && shadow_.mode > 0) {
        const std::uint32_t maxLights = shadow_.mode == 2 ? kLightShadowMax : 12u;
        const float limit = shadow_.mode == 2 ? 6000.0f : 2500.0f;
        std::vector<std::pair<float, std::uint32_t>> nearLights;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto& w = scratch_[i];
            const float dx = w.pos[0] - cam.position[0], dy = w.pos[1] - cam.position[1], dz = w.pos[2] - cam.position[2];
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist < limit && w.range < 3000.0f) nearLights.emplace_back(dist, i);
        }
        {
            static std::uint32_t diagCounter = 0;
            if (++diagCounter % 600 == 1) {
                std::uint32_t within = 0, bigRange = 0;
                float nearest = 1e9f;
                for (std::uint32_t i = 0; i < count; ++i) {
                    const auto& w = scratch_[i];
                    const float dx = w.pos[0] - cam.position[0], dy = w.pos[1] - cam.position[1], dz = w.pos[2] - cam.position[2];
                    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                    nearest = std::min(nearest, dist);
                    if (dist < limit) {
                        ++within;
                        if (w.range >= 3000.0f) ++bigRange;
                    }
                }
                for (std::uint32_t i = 0; i < count; ++i) {
                    const auto& w = scratch_[i];
                    const float dx = w.pos[0] - cam.position[0], dy = w.pos[1] - cam.position[1], dz = w.pos[2] - cam.position[2];
                    if (std::sqrt(dx * dx + dy * dy + dz * dz) < 4000.0f)
                        spdlog::debug("enh: lights:   lamp near camera: ({:.0f}, {:.0f}, {:.0f}) range {:.0f} col ({:.2f}, {:.2f}, {:.2f}) dynamic {} ambient {}", w.pos[0], w.pos[1],
                                     w.pos[2], w.range, w.col[0], w.col[1], w.col[2], w.dynamic, w.ambient);
                }
                spdlog::info("enh: lights: lamp shadow candidates: camera ({:.0f}, {:.0f}, {:.0f}), {} lights within {:.0f} units ({} with range >= 3000), nearest {:.0f}, {} get ids",
                             cam.position[0], cam.position[1], cam.position[2], within, limit, bigRange, nearest, nearLights.size());
            }
        }
        std::sort(nearLights.begin(), nearLights.end());
        for (std::uint32_t k = 0; k < nearLights.size() && k < maxLights; ++k) shadowId[nearLights[k].second] = k + 1;
        shadowLights_ = static_cast<std::uint32_t>(std::min<std::size_t>(nearLights.size(), maxLights));
    }
    cur_.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& w = scratch_[i];
        GpuLight& g = cur_[i];
        g.posRcp[0] = w.pos[0];
        g.posRcp[1] = w.pos[1];
        g.posRcp[2] = w.pos[2];
        g.posRcp[3] = 1.0f / (w.range * w.range);
        for (int c = 0; c < 3; ++c) {
            g.colRange[c] = w.col[c];
            g.specFlags[c] = w.col[c] * kSpecularShare;
        }
        g.colRange[3] = w.range;
        const std::uint32_t flags = w.dynamic ? 1u : 0u;
        std::memcpy(&g.specFlags[3], &flags, 4);
        // a stable id from the position (static lights do not move; a dynamic light's id follows its position, which is all the table promises)
        std::uint32_t id = 2166136261u;
        for (int c = 0; c < 3; ++c) {
            const std::int32_t q = static_cast<std::int32_t>(std::lround(w.pos[c]));
            id = (id ^ static_cast<std::uint32_t>(q)) * 16777619u;
        }
        g.misc[0] = 0;
        std::memcpy(&g.misc[0], &id, 4);
        g.misc[1] = g.misc[2] = g.misc[3] = 0.0f;
        std::memcpy(&g.misc[1], &shadowId[i], 4);
    }
    // batch change detection against the previous upload (the statistics of how many lights move: static ones are the same every frame)
    lastChanged_ = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (i >= prev_.size() || std::memcmp(&prev_[i], &cur_[i], sizeof(GpuLight)) != 0) ++lastChanged_;
    }
    changedTotal_ += lastChanged_;
    prev_.swap(cur_);
    lastCount_ = count;
    {
        auto known = std::make_shared<KnownGrid>();
        known->cells.reserve(count);
        for (const auto& g : prev_) {
            known->cells.emplace_back(knownKey(static_cast<std::int32_t>(std::floor(g.posRcp[0] / 16.0f)), static_cast<std::int32_t>(std::floor(g.posRcp[1] / 16.0f)),
                                               static_cast<std::int32_t>(std::floor(g.posRcp[2] / 16.0f))),
                                      std::array<float, 3>{g.posRcp[0], g.posRcp[1], g.posRcp[2]});
        }
        std::sort(known->cells.begin(), known->cells.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        {
            // (the generation: a build whose light positions equal the last build's leaves it alone)
            static std::vector<std::array<float, 4>> pos;  // kept between builds (render thread)
            pos.clear();
            pos.reserve(prev_.size());
            for (const auto& g : prev_) pos.push_back({g.posRcp[0], g.posRcp[1], g.posRcp[2], 0.0f});
            registryNoteKnown(registryPositionHash(reinterpret_cast<const float(*)[4]>(pos.data()), static_cast<std::uint32_t>(pos.size())));
        }
        g_known.store(std::move(known));
    }

    rhi::Buffer& up = *upload_[dev_.frameValue() % rhi::kFramesInFlight];
    if (count > 0) std::memcpy(up.map(), prev_.data(), std::size_t{count} * kLightBytes);
    up.flush();

    RhiScope rs(dev_, ctx.cmd);
    if (!rs) {
        ++skipped_;
        return;
    }
    rhi::CommandList& cl = rs.cl();
    LightClusterPush push{};
    push.h.frame = ctx.frameBlock.bits;
    push.h.passId = passId_;
    const auto src = up.shaderRef(0), dst = device_->shaderRefRw(0);
    push.src[0] = src[0];
    push.src[1] = src[1];
    push.dst[0] = dst[0];
    push.dst[1] = dst[1];
    push.grid[0] = kLightGridX;
    push.grid[1] = kLightGridY;
    push.grid[2] = kLightGridZ;
    push.grid[3] = count;
    push.zs[0] = kLightNear;
    push.zs[1] = kLightFar;
    push.zs[2] = lightSliceScale(kLightNear, kLightFar, kLightGridZ);
    push.offs[0] = static_cast<std::uint32_t>(kLightsOffset);
    push.offs[1] = static_cast<std::uint32_t>(kClustersOffset);
    push.offs[2] = static_cast<std::uint32_t>(style_);
    push.grid1[0] = kLightGrid1X;
    push.grid1[1] = kLightGrid1Y;
    push.grid1[2] = kLightGrid1Z;
    push.grid1[3] = static_cast<std::uint32_t>(kClusters1Offset);
    push.zs1[0] = kLightFar;
    push.zs1[1] = kLightFar1;
    push.zs1[2] = lightSliceScale(kLightFar, kLightFar1, kLightGrid1Z);
    // the lamp shadows of this frame: certain to run (the header names their image; the pass light.shadow fills it)
    pendingShadow_ = {};
    if (shadowHeld_ && shadowLights_ > 0 && ctx.depthMap != nullptr && dev_.rtSceneValid() && shadowPipelinesReady() && dev_.readable(ctx.depthMap, ctx.cmd)) {
        const std::uint32_t w = ctx.depthMap->desc().width, h = ctx.depthMap->desc().height;
        rhi::Texture* raw = dev_.transient("light.shadow raw", {VK_FORMAT_R16G16B16A16_SFLOAT, {w, h}});
        rhi::Texture* fin = dev_.transient("light.shadow final", {VK_FORMAT_R16G16B16A16_SFLOAT, {w, h}});
        if (raw != nullptr && fin != nullptr && w < 65536 && h < 65536) {
            LightShadowPush sp{};
            sp.h.frame = ctx.frameBlock.bits;
            const auto tab = device_->shaderRef(0);
            sp.table[0] = tab[0];
            sp.table[1] = tab[1];
            sp.tex[0] = ctx.depthMap->bindlessSlot();
            sp.tex[1] = raw->heapSampled();
            sp.tex[2] = w;
            sp.tex[3] = h;
            sp.cfg[0] = shadow_.radius;
            sp.cfg[1] = 12000.0f;
            sp.cfg[2] = 1.5f;
            sp.cfg[3] = shadow_.quality >= 3 ? 2.0f : 1.0f;
            sp.misc[0] = static_cast<std::uint32_t>(dev_.frameValue());
            sp.misc[1] = shadow_.quality <= 1 ? 1u : 0u;  // RVK_RT_TRACE_OPAQUE at low
            pendingShadow_ = {dev_.game().memoryEpoch(), sp, raw, fin, (w + 7) / 8, (h + 7) / 8};  // (the memory epoch: 14.40)
            push.shadow[0] = fin->heapSampled();
            push.shadow[1] = w;
            push.shadow[2] = h;
            push.shadow[3] = shadowLights_;
        }
    }
    if (shadowHeld_ && pendingShadow_.frame == ~0ull) ++shadowSkipped_;
    cl.bufferBarrier(*device_, rhi::BufferAccess::ShaderWrite);
    cl.bindPipeline(*pipeline_);
    cl.setPushConstants(&push, sizeof(push));
    cl.dispatch((kClusters + 63) / 64, 1, 1);
    cl.bufferBarrier(*device_, rhi::BufferAccess::ShaderRead);
    if (passes_++ == 0) {
        spdlog::info("enh: lights: first cluster build: {} lights ({} static in the tree, {} dynamic registered), {} clusters", count, stats_.staticTotal,
                     stats_.dynamicTotal, kClusters);
    }
    logStats();
}

// pass 2 of the lamp shadows: the trace and the filter (a frame the cluster build prepared)
void LightTable::recordShadow(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || pendingShadow_.frame != dev_.game().memoryEpoch()) return;
    PendingShadow p = pendingShadow_;
    pendingShadow_ = {};
    d3d9::RegionScope region(dev_, "light.shadow");
    RhiScope rs(dev_, ctx.cmd);
    if (!rs) return;
    rhi::CommandList& cl = rs.cl();
    rhi::Texture* raw = rs.wrap(*p.raw);
    rhi::Texture* fin = rs.wrap(*p.fin);
    cl.transition(*raw, VK_IMAGE_LAYOUT_GENERAL);
    cl.bindPipeline(*shadowPipeline_[0]);
    rhi::Texture* const s0[] = {raw};
    cl.setStorageTextures(s0);
    cl.setPushConstants(&p.push, sizeof(p.push));
    cl.dispatch(p.gx, p.gy, 1);
    cl.transition(*raw, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cl.transition(*fin, VK_IMAGE_LAYOUT_GENERAL);
    cl.bindPipeline(*shadowPipeline_[1]);
    rhi::Texture* const s1[] = {fin};
    cl.setStorageTextures(s1);
    cl.setPushConstants(&p.push, sizeof(p.push));
    cl.dispatch(p.gx, p.gy, 1);
    cl.transition(*fin, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (shadowPasses_++ == 0) {
        spdlog::info("enh: lights: first lamp shadow frame {}x{}: {} shadow-casting lights, radius {:.0f} units, {} ray(s) per light", p.push.tex[2], p.push.tex[3],
                     shadowLights_, p.push.cfg[0], static_cast<int>(p.push.cfg[3]));
    }
}

void LightTable::logStats() {
    const double now = dev_.secondsSinceCreate();
    if (now - lastLog_ < 5.0) return;
    lastLog_ = now;
    spdlog::info("enh: lights: tree walk {} nodes ({} static lights seen), near lights {} (static near {} / any {} of the registry), {} lights in the table ({} changed since the last frame, {} dropped beyond {}, {} from drawn lists), {} builds ({} skipped), style {}, "
                 "debug {}, table lighting {}",
                 stats_.nodes, stats_.staticTotal, lastNear_, lastNearStatic_, lastNearAny_, lastCount_, lastChanged_, lastDropped_, kLightMax, lastDrawnAdded_, passes_, skipped_, style_ == 1 ? "physical" : "original", debug_,
                 fullLitActive() ? "on" : "off");
    if (colourRatioN_ > 0) {
        spdlog::info("enh: lights: drawn lists' register colour / registry colour of the same light: {:.3f} {:.3f} {:.3f} (mean over {} matches)", colourRatio_[0] / colourRatioN_,
                     colourRatio_[1] / colourRatioN_, colourRatio_[2] / colourRatioN_, colourRatioN_);
    }
    if (shadowHeld_) {
        spdlog::info("enh: lights: lamp shadows: {} lights with a shadow id, {} frames traced, {} skipped (no RT scene / pipelines / depth)", shadowLights_, shadowPasses_,
                     shadowSkipped_);
    }
}

}  // namespace rvk::enh
