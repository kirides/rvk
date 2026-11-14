#include "enh/light_table.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <windows.h>

#include <spdlog/spdlog.h>

#include "enh/rhi_scope.h"
#include "gpuscene/verify.h"
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
    std::uint64_t evalGen = 0;  // the registry cell generation `matched` was evaluated at (record())
    std::int32_t matched = -1;  // index of the static registry light within 8 units (scratch_ order), -1 = none
};
std::vector<DrawnLight> g_drawn;  // lights of the lists drawn lately (kept while re-noted: replayed packets do not note again), merged into every build
std::uint64_t g_buildCounter = 0;
constexpr std::uint64_t kDrawnKeep = 900;  // builds
}  // namespace

std::atomic<bool>& lightBuildSkip() {
    static std::atomic<bool> on{true};
    return on;
}
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
std::atomic<std::uint64_t> g_knownSerial{1};  // WS11: bumped with every store of g_known (lightKnown's answers are memoised per serial)
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

std::uint64_t LightTable::knownSerial() { return g_knownSerial.load(std::memory_order_acquire); }

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
    {
        char v[8]{};
        verify_ = GetEnvironmentVariableA("RVK_LIGHTS_VERIFY", v, sizeof(v)) != 0 && v[0] == '1';
        char w[8]{};
        if (GetEnvironmentVariableA("RVK_LIGHTS_SKIP_OFF", w, sizeof(w)) != 0 && w[0] == '1') lightBuildSkip().store(false);
        if (verify_) spdlog::info("enh: lights: RVK_LIGHTS_VERIFY: every 8th build is compared with the uncached reference");
    }
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
        // (two passes so the per-pass GPU times tell the trace from the filter)
        dev_.addPass(d3d9::inject::Point::AfterDepthPrepass, "light.shadow filter", [this](d3d9::PassContext& ctx) { recordShadowFilter(ctx); }, &shadowPassId_, 6);
        shadowHeld_ = true;
        spdlog::info("enh: lights: lamp shadows on (mode {}, quality {}, lamp radius {:.0f} units)", shadow_.mode == 2 ? "many" : "nearby", shadow_.quality,
                     shadow_.radius);
    } else if (!want && shadowHeld_) {
        dev_.removePasses(&shadowPassId_);
        shadowPassId_ = 0;
        pendingShadow_ = {};
        pendingFilter_ = {};
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

namespace {
double nowMs() {
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / freq;
}
}  // namespace

// RVK_LIGHTS_VERIFY=1: every 8th build also runs the code as it was before the registry cache (uncached static walk, the cell index rebuilt and searched for
// every drawn light, the sqrt-comparator truncation, the table build, the known grid built from scratch) on the same inputs (this build's dynamic lights, a
// snapshot of the drawn lights) and compares the resulting table bytes and known grid with the cached path's. Logged every 5 s.
void LightTable::verifyReference(const float cam[3]) {
    using engine::lights::WorldLight;
    std::vector<WorldLight> ref;
    if (!engine::lights::collectStaticsReference(cam, collectRadius_, ref)) return;
    ref.insert(ref.end(), verifyDyn_.begin(), verifyDyn_.end());
    {
        // the merge of the drawn lights, as it was
        const std::size_t registry = ref.size();
        std::vector<std::pair<std::uint64_t, std::uint32_t>> cells;
        for (std::uint32_t i = 0; i < registry; ++i) {
            const auto& w = ref[i];
            cells.emplace_back(knownKey(static_cast<std::int32_t>(std::floor(w.pos[0] / 16.0f)), static_cast<std::int32_t>(std::floor(w.pos[1] / 16.0f)),
                                        static_cast<std::int32_t>(std::floor(w.pos[2] / 16.0f))),
                               i);
        }
        std::sort(cells.begin(), cells.end());
        for (const auto& d : verifyDrawn_) {
            const std::int32_t cx = static_cast<std::int32_t>(std::floor(d.pos[0] / 16.0f)), cy = static_cast<std::int32_t>(std::floor(d.pos[1] / 16.0f)),
                               cz = static_cast<std::int32_t>(std::floor(d.pos[2] / 16.0f));
            bool known = false;
            for (std::int32_t dx = -1; dx <= 1 && !known; ++dx) {
                for (std::int32_t dy = -1; dy <= 1 && !known; ++dy) {
                    for (std::int32_t dz = -1; dz <= 1 && !known; ++dz) {
                        const std::uint64_t cellKey = knownKey(cx + dx, cy + dy, cz + dz);
                        auto it = std::lower_bound(cells.begin(), cells.end(), std::pair<std::uint64_t, std::uint32_t>{cellKey, 0u});
                        for (; it != cells.end() && it->first == cellKey && !known; ++it) {
                            auto& w = ref[it->second];
                            if (!(std::abs(w.pos[0] - d.pos[0]) < 8.0f && std::abs(w.pos[1] - d.pos[1]) < 8.0f && std::abs(w.pos[2] - d.pos[2]) < 8.0f)) continue;
                            known = true;
                            if (!w.dynamic && w.col[0] > 1e-4f && w.col[1] > 1e-4f && w.col[2] > 1e-4f && d.col[0] + d.col[1] + d.col[2] > 1e-4f)
                                std::memcpy(w.col, d.col, sizeof(w.col));
                        }
                    }
                }
            }
            if (!known) ref.push_back(d);
        }
    }
    const auto reach = [&](const WorldLight& l) {
        const float dx = l.pos[0] - cam[0], dy = l.pos[1] - cam[1], dz = l.pos[2] - cam[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz) - l.range;
    };
    if (ref.size() > kLightMax) {
        const auto mid = std::stable_partition(ref.begin(), ref.end(), [&](const auto& l) { return reach(l) <= kLightFar; });
        const std::size_t nearCount = static_cast<std::size_t>(mid - ref.begin());
        const auto byReach = [&](const auto& a, const auto& b) { return reach(a) < reach(b); };
        if (nearCount >= kLightMax) {
            std::nth_element(ref.begin(), ref.begin() + kLightMax, mid, byReach);
        } else {
            std::nth_element(mid, ref.begin() + kLightMax, ref.end(), byReach);
        }
        ref.resize(kLightMax);
    }
    const std::uint32_t count = static_cast<std::uint32_t>(ref.size());
    std::vector<std::uint32_t> shadowId(count, 0);
    if (shadowHeld_ && shadow_.mode > 0) {
        const std::uint32_t maxLights = shadow_.mode == 2 ? kLightShadowMax : 12u;
        const float limit = shadow_.mode == 2 ? 6000.0f : 2500.0f;
        std::vector<std::pair<float, std::uint32_t>> nearLights;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto& w = ref[i];
            const float dx = w.pos[0] - cam[0], dy = w.pos[1] - cam[1], dz = w.pos[2] - cam[2];
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist < limit && w.range < 3000.0f) nearLights.emplace_back(dist, i);
        }
        std::sort(nearLights.begin(), nearLights.end());
        for (std::uint32_t k = 0; k < nearLights.size() && k < maxLights; ++k) shadowId[nearLights[k].second] = k + 1;
    }
    std::vector<GpuLight> table(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& w = ref[i];
        GpuLight& g = table[i];
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
    ++verifyBuilds_;
    bool differ = false;
    std::string what;
    if (table.size() != prev_.size()) {
        differ = true;
        what = "table size " + std::to_string(prev_.size()) + " (cached) vs " + std::to_string(table.size()) + " (reference)";
    } else {
        for (std::size_t i = 0; i < table.size(); ++i) {
            if (std::memcmp(&table[i], &prev_[i], sizeof(GpuLight)) != 0) {
                differ = true;
                what = "light " + std::to_string(i) + " differs: cached pos " + std::to_string(prev_[i].posRcp[0]) + "," + std::to_string(prev_[i].posRcp[1]) + "," +
                       std::to_string(prev_[i].posRcp[2]) + " col " + std::to_string(prev_[i].colRange[0]) + " range " + std::to_string(prev_[i].colRange[3]) +
                       " shadow id " + std::to_string(*reinterpret_cast<const std::uint32_t*>(&prev_[i].misc[1])) + "; reference pos " + std::to_string(table[i].posRcp[0]) + "," +
                       std::to_string(table[i].posRcp[1]) + "," + std::to_string(table[i].posRcp[2]) + " col " + std::to_string(table[i].colRange[0]) + " range " +
                       std::to_string(table[i].colRange[3]) + " shadow id " + std::to_string(*reinterpret_cast<const std::uint32_t*>(&table[i].misc[1]));
                break;
            }
        }
    }
    if (!differ) {
        // the known grid: the same multiset of (key, position)
        std::vector<std::pair<std::uint64_t, std::array<float, 3>>> a, b;
        for (const auto& g : table)
            a.emplace_back(knownKey(static_cast<std::int32_t>(std::floor(g.posRcp[0] / 16.0f)), static_cast<std::int32_t>(std::floor(g.posRcp[1] / 16.0f)),
                                    static_cast<std::int32_t>(std::floor(g.posRcp[2] / 16.0f))),
                           std::array<float, 3>{g.posRcp[0], g.posRcp[1], g.posRcp[2]});
        if (const auto kg = g_known.load()) b = kg->cells;
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        if (a != b) {
            differ = true;
            what = "known grid differs (" + std::to_string(b.size()) + " cached cells vs " + std::to_string(a.size()) + ")";
        }
    }
    if (differ) {
        ++verifyDiffering_;
        if (verifyFirst_.empty()) verifyFirst_ = "build " + std::to_string(passes_) + ": " + what;
    }
}

void LightTable::record(d3d9::PassContext& ctx) {
    double tm0 = nowMs();
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
    const bool doVerify = verify_ && (passes_ % 8) == 0;
    // WS13: the build below (near count, drawn-light merge, truncation, shadow ids, table, known grid) is a pure function of the collected lights, the camera
    // position, the drawn lights (set + colours, hashed) and the shadow mode: a build whose signature equals the last full build's keeps its table (prev_) and
    // upload; check builds (RVK_LIGHTS_VERIFY every 8th, gpuscene::periodic 1 of 10) and every kFullEvery-th build run in full and compare with the kept table
    std::uint64_t drawnGen = 0;
    {
        const std::lock_guard lock(g_drawnMutex);
        ++g_buildCounter;
        std::erase_if(g_drawn, [](const DrawnLight& d) { return g_buildCounter - d.stamp > kDrawnKeep; });
        // (the drawn lights' content as the merge below reads it: the colours alternate with the draws that note them, so the state is hashed, not counted)
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (const auto& d : g_drawn) {
            std::uint32_t b[7];
            std::memcpy(b, d.l.pos, 12);
            std::memcpy(b + 3, d.l.col, 12);
            std::memcpy(b + 6, &d.l.range, 4);
            for (const std::uint32_t w : b) {
                h = (h ^ w) * 0x100000001b3ull;
                h ^= h >> 29;
            }
        }
        drawnGen = h ^ (static_cast<std::uint64_t>(g_drawn.size()) << 48);
    }
    std::uint64_t sig = 0xcbf29ce484222325ull;
    {
        const auto mix = [&sig](std::uint64_t w) { sig = (sig ^ w) * 0x100000001b3ull; sig ^= sig >> 31; };
        const auto bits = [](float f) { std::uint32_t u; std::memcpy(&u, &f, 4); return std::uint64_t{u}; };
        for (int c = 0; c < 3; ++c) mix(bits(cam.position[static_cast<std::size_t>(c)]));
        mix(drawnGen);
        mix(scratch_.size());
        mix(stats_.staticNear);
        mix(bits(collectRadius_));
        mix((shadowHeld_ ? 1u : 0u) | (static_cast<std::uint64_t>(shadow_.mode) << 1));
        for (const auto& w : scratch_) {
            mix(bits(w.pos[0]) | bits(w.pos[1]) << 32);
            mix(bits(w.pos[2]) | bits(w.range) << 32);
            mix(bits(w.col[0]) | bits(w.col[1]) << 32);
            mix(bits(w.col[2]) | static_cast<std::uint64_t>(w.ambient ? 1u : 0u) << 32 | static_cast<std::uint64_t>(w.dynamic ? 1u : 0u) << 33);
        }
    }
    const bool sigMatch = haveSig_ && sig == buildSig_;
    const bool checkBuild = doVerify || gpuscene::periodic(dev_.stats().frames, 10);
    const bool skipBuild = sigMatch && !checkBuild && sinceFull_ < kFullEvery && lightBuildSkip().load(std::memory_order_relaxed);
    if (doVerify) verifyDyn_.assign(scratch_.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(stats_.staticNear, scratch_.size())), scratch_.end());
    const double tm1 = nowMs();
    double tm2 = tm1, tm3 = tm1, tm4 = tm1;
    std::uint32_t count = lastCount_;
    if (skipBuild) {
        ++skipHits_;
        ++sinceFull_;
        lastChanged_ = 0;
        // (the registry's state transitions of a full build, with the kept values: an early return of another view may have flipped the flag in between)
        g_registryThin.store(lastNearStatic_ == 0, std::memory_order_relaxed);
        registryNoteComplete(lastNearAny_ != 0);
        registryNoteKnown(knownGridHash_);
    } else {
    sinceFull_ = 0;
    // "the registry holds lights" means lights that reach the NEAR level (the old collection range, kLightFar): the far lights of the whole island (the far level,
    // up to kLightFar1) are in the tree even where nothing is cached in around the camera, and must not hide the objects' own lights (a teleport into the
    // monastery: the candles are only in the drawn lists)
    std::uint32_t nearStatic = 0, nearAny = 0;
    for (const auto& w : scratch_) {
        const float dx = w.pos[0] - cam.position[0], dy = w.pos[1] - cam.position[1], dz = w.pos[2] - cam.position[2];
        const float r = kLightFar + w.range;
        if (dx * dx + dy * dy + dz * dz <= r * r) {
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
        // (collect appends the static lights first, then the dynamic ones)
        const std::size_t nStatic = std::min<std::size_t>(stats_.staticNear, registry);
        if (doVerify) {
            verifyDrawn_.clear();
            for (const auto& dl : g_drawn) verifyDrawn_.push_back(dl.l);
        }
        // The static lights of the registry in 16-unit cells: a drawn light is "the same" within 8 units. The static lights carry the colour the engine's
        // DRAWS give them: the registry's rgb x intensity x scale x 0.5 is a guess, the draws' register colour differs per light by 0.2 .. 2.0 (measured:
        // gpu-scene.md 9.10; the engine weights each static light), so a matched light takes the colour of the draw that was seen last.
        // The static set is the same from frame to frame (the engine caches cells in and out rarely): the sorted (cell key, light index) vector is rebuilt only
        // when the set differs (a hash of its positions; `cellsGen` counts the rebuilds), and every drawn light keeps its match from the last rebuild it was
        // evaluated at (a drawn light never moves: noteDrawnLight merges within 1 unit), so a steady frame costs one pass over the drawn lights.
        static std::vector<std::pair<std::uint64_t, std::uint32_t>> cells;  // under g_drawnMutex; equal keys stay in index order
        static std::uint64_t cellsHash = 0, cellsGen = 0;
        static std::size_t cellsCount = ~std::size_t{0};
        std::uint64_t h = 0;
        for (std::size_t i = 0; i < nStatic; ++i) {
            std::uint32_t b[3];
            std::memcpy(b, scratch_[i].pos, 12);
            std::uint64_t m = 0x9E3779B97F4A7C15ull;
            for (const std::uint32_t wd : b) {
                m = (m ^ wd) * 0xC2B2AE3D27D4EB4Full;
                m ^= m >> 29;
            }
            h += m;
        }
        if (h != cellsHash || nStatic != cellsCount) {
            cellsHash = h;
            cellsCount = nStatic;
            ++cellsGen;
            cells.clear();
            cells.reserve(nStatic);
            for (std::uint32_t i = 0; i < nStatic; ++i) {
                const auto& w = scratch_[i];
                cells.emplace_back(knownKey(static_cast<std::int32_t>(std::floor(w.pos[0] / 16.0f)), static_cast<std::int32_t>(std::floor(w.pos[1] / 16.0f)),
                                            static_cast<std::int32_t>(std::floor(w.pos[2] / 16.0f))),
                                   i);
            }
            std::sort(cells.begin(), cells.end());
        }
        for (auto& dl : g_drawn) {
            const auto& d = dl.l;
            if (dl.evalGen != cellsGen) {
                dl.evalGen = cellsGen;
                dl.matched = -1;
                const std::int32_t cx = static_cast<std::int32_t>(std::floor(d.pos[0] / 16.0f)), cy = static_cast<std::int32_t>(std::floor(d.pos[1] / 16.0f)),
                                   cz = static_cast<std::int32_t>(std::floor(d.pos[2] / 16.0f));
                for (std::int32_t dx = -1; dx <= 1 && dl.matched < 0; ++dx) {
                    for (std::int32_t dy = -1; dy <= 1 && dl.matched < 0; ++dy) {
                        for (std::int32_t dz = -1; dz <= 1 && dl.matched < 0; ++dz) {
                            const std::uint64_t cellKey = knownKey(cx + dx, cy + dy, cz + dz);
                            auto it = std::lower_bound(cells.begin(), cells.end(), std::pair<std::uint64_t, std::uint32_t>{cellKey, 0u});
                            for (; it != cells.end() && it->first == cellKey && dl.matched < 0; ++it) {
                                const auto& w = scratch_[it->second];
                                if (std::abs(w.pos[0] - d.pos[0]) < 8.0f && std::abs(w.pos[1] - d.pos[1]) < 8.0f && std::abs(w.pos[2] - d.pos[2]) < 8.0f)
                                    dl.matched = static_cast<std::int32_t>(it->second);
                            }
                        }
                    }
                }
            }
            bool known = false;
            if (dl.matched >= 0) {
                known = true;
                auto& w = scratch_[static_cast<std::size_t>(dl.matched)];
                if (w.col[0] > 1e-4f && w.col[1] > 1e-4f && w.col[2] > 1e-4f && d.col[0] + d.col[1] + d.col[2] > 1e-4f) {
                    for (int c = 0; c < 3; ++c) colourRatio_[c] += d.col[c] / w.col[c];
                    ++colourRatioN_;
                    std::memcpy(w.col, d.col, sizeof(w.col));
                }
            } else {
                // the dynamic lights move (carried torches): compared every frame (a handful)
                for (std::size_t j = nStatic; j < registry && !known; ++j) {
                    const auto& w = scratch_[j];
                    known = std::abs(w.pos[0] - d.pos[0]) < 8.0f && std::abs(w.pos[1] - d.pos[1]) < 8.0f && std::abs(w.pos[2] - d.pos[2]) < 8.0f;
                }
            }
            if (!known) scratch_.push_back(d);
        }
        lastDrawnAdded_ = static_cast<std::uint32_t>(scratch_.size() - registry);
    }
    tm2 = nowMs();
    const auto reach = [&](const engine::lights::WorldLight& l) {
        const float dx = l.pos[0] - cam.position[0], dy = l.pos[1] - cam.position[1], dz = l.pos[2] - cam.position[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz) - l.range;
    };
    lastDropped_ = 0;
    lastNear_ = 0;
    if (scratch_.size() > kLightMax) {
        lastDropped_ = static_cast<std::uint32_t>(scratch_.size()) - kLightMax;
        // the lights that reach the near level (the set the table held before the far level existed) come first and keep their order; the far lights of the
        // island only fill the capacity that is left, nearest first: they never push a local lamp out.
        // (The same algorithm calls on a (reach, index) key array computed once: the comparisons give the same results as on the lights, so the same
        // selection and order as the version that evaluated the sqrt in every comparison.)
        reachKeys_.resize(scratch_.size());
        for (std::uint32_t i = 0; i < scratch_.size(); ++i) reachKeys_[i] = {reach(scratch_[i]), i};
        const auto mid = std::stable_partition(reachKeys_.begin(), reachKeys_.end(), [](const ReachKey& k) { return k.reach <= kLightFar; });
        const std::size_t nearCount = static_cast<std::size_t>(mid - reachKeys_.begin());
        lastNear_ = static_cast<std::uint32_t>(nearCount);
        const auto byReach = [](const ReachKey& a, const ReachKey& b) { return a.reach < b.reach; };
        if (nearCount >= kLightMax) {
            std::nth_element(reachKeys_.begin(), reachKeys_.begin() + kLightMax, mid, byReach);
        } else {
            std::nth_element(mid, reachKeys_.begin() + kLightMax, reachKeys_.end(), byReach);
        }
        truncated_.resize(kLightMax);
        for (std::uint32_t i = 0; i < kLightMax; ++i) truncated_[i] = scratch_[reachKeys_[i].index];
        scratch_.swap(truncated_);
    }
    tm3 = nowMs();
    count = static_cast<std::uint32_t>(scratch_.size());
    // lamp shadows: the nearest lights within the mode's reach get the ids 1..N (the pass light.shadow traces them)
    std::vector<std::uint32_t>& shadowId = shadowIdScratch_;
    shadowId.assign(count, 0);
    shadowLights_ = 0;
    if (shadowHeld_ && shadow_.mode > 0) {
        const std::uint32_t maxLights = shadow_.mode == 2 ? kLightShadowMax : 12u;
        const float limit = shadow_.mode == 2 ? 6000.0f : 2500.0f;
        auto& nearLights = nearLightsScratch_;
        nearLights.clear();
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
    tm4 = nowMs();
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
    {
        const bool same = cur_.size() == prev_.size() && (cur_.empty() || std::memcmp(cur_.data(), prev_.data(), cur_.size() * sizeof(GpuLight)) == 0);
        if (sigMatch && lightBuildSkip().load(std::memory_order_relaxed)) {
            // (a full build whose inputs equal the last build's: its table must be the kept one)
            ++skipChecks_;
            if (!same) {
                ++skipDiffering_;
                if (skipFirst_.empty()) skipFirst_ = "build " + std::to_string(passes_) + ": table " + std::to_string(prev_.size()) + " -> " + std::to_string(cur_.size()) + " lights";
            }
        }
        if (!same) ++buildSerial_;
        buildSig_ = sig;
        haveSig_ = true;
    }
    prev_.swap(cur_);
    lastCount_ = count;
    {
        // the grid of `lightKnown` depends on the light POSITIONS only (an order-independent hash, the registry generation's own): a build with the same set as
        // the last one keeps the grid (steady frames: no allocation, no sort)
        std::uint64_t sum = 0;
        for (const auto& g : prev_) {
            std::uint32_t b[3];
            std::memcpy(b, g.posRcp, 12);
            std::uint64_t h = 0x9E3779B97F4A7C15ull;
            for (const std::uint32_t w : b) {
                h = (h ^ w) * 0xC2B2AE3D27D4EB4Full;
                h ^= h >> 29;
            }
            sum += h;
        }
        const std::uint64_t posHash = sum ^ (static_cast<std::uint64_t>(prev_.size()) << 48);  // (= registryPositionHash)
        if (posHash != knownGridHash_ || !g_known.load()) {
            knownGridHash_ = posHash;
            auto known = std::make_shared<KnownGrid>();
            known->cells.reserve(count);
            for (const auto& g : prev_) {
                known->cells.emplace_back(knownKey(static_cast<std::int32_t>(std::floor(g.posRcp[0] / 16.0f)), static_cast<std::int32_t>(std::floor(g.posRcp[1] / 16.0f)),
                                                   static_cast<std::int32_t>(std::floor(g.posRcp[2] / 16.0f))),
                                          std::array<float, 3>{g.posRcp[0], g.posRcp[1], g.posRcp[2]});
            }
            std::sort(known->cells.begin(), known->cells.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            g_known.store(std::move(known));
            g_knownSerial.fetch_add(1, std::memory_order_release);
        }
        registryNoteKnown(posHash);  // (the generation: a build whose light positions equal the last build's leaves it alone)
    }

    }
    if (doVerify) verifyReference(cam.position.data());
    const double tm5 = nowMs();
    rhi::Buffer& up = *upload_[dev_.frameValue() % rhi::kFramesInFlight];
    {
        // (the upload of a table the slot already holds is skipped: per slot in flight, the serial of the table it was written from)
        const std::size_t slot = dev_.frameValue() % rhi::kFramesInFlight;
        if (count > 0 && uploadSerial_[slot] != buildSerial_) {
            std::memcpy(up.map(), prev_.data(), std::size_t{count} * kLightBytes);
            up.flush();
            uploadSerial_[slot] = buildSerial_;
        } else {
            ++uploadsSkipped_;
        }
    }

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
        const bool half = true;  // the trace at half resolution (the filter upsamples; gpu-scene.md 9.14)
        const std::uint32_t rw = half ? (w + 1) / 2 : w, rh = half ? (h + 1) / 2 : h;
        rhi::Texture* raw = dev_.transient("light.shadow raw", {VK_FORMAT_R16G16B16A16_SFLOAT, {rw, rh}});
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
            sp.misc[2] = half ? 1u : 0u;  // bit 0: half resolution raw image
            pendingShadow_ = {dev_.game().memoryEpoch(), sp, raw, fin, (w + 7) / 8, (h + 7) / 8, (rw + 7) / 8, (rh + 7) / 8};  // (the memory epoch: 14.40)
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
    const double tm6 = nowMs();
    cpuMs_[0] += tm1 - tm0;  // collect (engine walk)
    cpuMs_[1] += tm2 - tm1;  // near count + drawn merge
    cpuMs_[2] += tm3 - tm2;  // truncation to the table size
    cpuMs_[3] += tm4 - tm3;  // shadow ids
    cpuMs_[4] += tm5 - tm4;  // GPU light build + change detection + known grid
    cpuMs_[5] += tm6 - tm5;  // upload + dispatch recording
    ++cpuN_;
    logStats();
}

// pass 2 of the lamp shadows: the trace (a frame the cluster build prepared); the filter is the pass after it
void LightTable::recordShadow(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || pendingShadow_.frame != dev_.game().memoryEpoch()) return;
    PendingShadow p = pendingShadow_;
    pendingShadow_ = {};
    d3d9::RegionScope region(dev_, "light.shadow");
    RhiScope rs(dev_, ctx.cmd);
    if (!rs) return;
    rhi::CommandList& cl = rs.cl();
    rhi::Texture* raw = rs.wrap(*p.raw);
    cl.transition(*raw, VK_IMAGE_LAYOUT_GENERAL);
    cl.bindPipeline(*shadowPipeline_[0]);
    rhi::Texture* const s0[] = {raw};
    cl.setStorageTextures(s0);
    cl.setPushConstants(&p.push, sizeof(p.push));
    cl.dispatch(p.gxT, p.gyT, 1);
    cl.transition(*raw, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    pendingFilter_ = p;
    if (shadowPasses_++ == 0) {
        spdlog::info("enh: lights: first lamp shadow frame {}x{}: {} shadow-casting lights, radius {:.0f} units, {} ray(s) per light", p.push.tex[2], p.push.tex[3],
                     shadowLights_, p.push.cfg[0], static_cast<int>(p.push.cfg[3]));
    }
}

void LightTable::recordShadowFilter(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || pendingFilter_.frame != dev_.game().memoryEpoch()) return;
    PendingShadow p = pendingFilter_;
    pendingFilter_ = {};
    d3d9::RegionScope region(dev_, "light.shadow");
    RhiScope rs(dev_, ctx.cmd);
    if (!rs) return;
    rhi::CommandList& cl = rs.cl();
    rhi::Texture* fin = rs.wrap(*p.fin);
    cl.transition(*fin, VK_IMAGE_LAYOUT_GENERAL);
    cl.bindPipeline(*shadowPipeline_[1]);
    rhi::Texture* const s1[] = {fin};
    cl.setStorageTextures(s1);
    cl.setPushConstants(&p.push, sizeof(p.push));
    cl.dispatch(p.gx, p.gy, 1);
    cl.transition(*fin, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void LightTable::logStats() {
    const double now = dev_.secondsSinceCreate();
    if (now - lastLog_ < 5.0) return;
    lastLog_ = now;
    spdlog::info("enh: lights: tree walk {} nodes ({} static lights seen), near lights {} (static near {} / any {} of the registry), {} lights in the table ({} changed since the last frame, {} dropped beyond {}, {} from drawn lists), {} builds ({} skipped), style {}, "
                 "debug {}, table lighting {}",
                 stats_.nodes, stats_.staticTotal, lastNear_, lastNearStatic_, lastNearAny_, lastCount_, lastChanged_, lastDropped_, kLightMax, lastDrawnAdded_, passes_, skipped_, style_ == 1 ? "physical" : "original", debug_,
                 fullLitActive() ? "on" : "off");
    if (verify_ && verifyBuilds_ > 0) {
        spdlog::info("enh: lights: verify (5 s): {} builds compared, DIFFERING {}{}{}", verifyBuilds_, verifyDiffering_, verifyFirst_.empty() ? "" : "; first: ", verifyFirst_);
    }
    if (skipHits_ + skipChecks_ > 0) {
        spdlog::info("enh: lights: unchanged builds (5 s): skipped {}, uploads skipped {}, full builds with an unchanged signature compared {} DIFFERING {}{}{}", skipHits_, uploadsSkipped_, skipChecks_,
                     skipDiffering_, skipFirst_.empty() ? "" : "; first: ", skipFirst_);
        skipHits_ = skipChecks_ = uploadsSkipped_ = 0;
    }
    if (cpuN_ > 0) {
        spdlog::info("enh: lights: CPU per build (ms, mean of {}): collect {:.3f}, near + drawn merge {:.3f}, truncate {:.3f}, shadow ids {:.3f}, table + known grid {:.3f}, upload + record {:.3f}",
                     cpuN_, cpuMs_[0] / cpuN_, cpuMs_[1] / cpuN_, cpuMs_[2] / cpuN_, cpuMs_[3] / cpuN_, cpuMs_[4] / cpuN_, cpuMs_[5] / cpuN_);
        std::fill(std::begin(cpuMs_), std::end(cpuMs_), 0.0);
        cpuN_ = 0;
    }
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
