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
std::atomic<std::uint64_t> g_completeStamp{0};
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
    std::unordered_multimap<std::uint64_t, std::array<float, 3>> cells;
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
                const auto range = g->cells.equal_range(knownKey(cx + dx, cy + dy, cz + dz));
                for (auto it = range.first; it != range.second; ++it) {
                    if (std::abs(it->second[0] - pos[0]) < 8.0f && std::abs(it->second[1] - pos[1]) < 8.0f && std::abs(it->second[2] - pos[2]) < 8.0f) return true;
                }
            }
        }
    }
    return false;
}

bool LightTable::registryComplete() { return g_registryComplete.load(std::memory_order_relaxed); }

bool LightTable::wantStaticNotes() { return g_registryThin.load(std::memory_order_relaxed); }

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
    dev_.setFrameParam(shader::abi::kFrameParamLights, {a, b, 0.0f, 0.0f});
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
    if (!enabled) g_registryComplete.store(false, std::memory_order_relaxed);
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
        g_registryComplete.store(false, std::memory_order_relaxed);
        ++skipped_;
        return;
    }
    const auto& cam = dev_.camera().valid ? dev_.camera() : dev_.lastCamera();
    if (!cam.valid) {
        g_registryComplete.store(false, std::memory_order_relaxed);
        ++skipped_;
        return;
    }
    // the world's lights within the grid's reach, nearest first when there are more than the table holds
    if (!engine::lights::collect(cam.position.data(), kLightFar1, scratch_, stats_)) {
        g_registryComplete.store(false, std::memory_order_relaxed);
        ++skipped_;
        return;
    }
    g_registryThin.store(stats_.staticTotal == 0, std::memory_order_relaxed);
    // the registry (static tree within reach + this frame's dynamic lights) is what the engine gives objects too: a non-empty registry is trusted whole
    g_registryComplete.store(!scratch_.empty(), std::memory_order_relaxed);
    // the lights of the lists drawn since the last build that the registry did not return (same light = within 8 units)
    lastDrawnAdded_ = 0;
    {
        const std::lock_guard lock(g_drawnMutex);
        const std::size_t registry = scratch_.size();
        ++g_buildCounter;
        std::erase_if(g_drawn, [](const DrawnLight& d) { return g_buildCounter - d.stamp > kDrawnKeep; });
        for (const auto& dl : g_drawn) {
            const auto& d = dl.l;
            bool known = false;
            for (std::size_t i = 0; i < scratch_.size() && !known; ++i) {
                const auto& w = scratch_[i];
                known = std::abs(w.pos[0] - d.pos[0]) < 8.0f && std::abs(w.pos[1] - d.pos[1]) < 8.0f && std::abs(w.pos[2] - d.pos[2]) < 8.0f;
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
    if (scratch_.size() > kLightMax) {
        lastDropped_ = static_cast<std::uint32_t>(scratch_.size()) - kLightMax;
        std::nth_element(scratch_.begin(), scratch_.begin() + kLightMax, scratch_.end(), [&](const auto& a, const auto& b) { return reach(a) < reach(b); });
        scratch_.resize(kLightMax);
    }
    const std::uint32_t count = static_cast<std::uint32_t>(scratch_.size());
    // lamp shadows: the nearest lights within the mode's reach get the ids 1..N (the pass light.shadow traces them)
    std::vector<std::uint32_t> shadowId(count, 0);
    shadowLights_ = 0;
    if (shadowHeld_ && shadow_.mode > 0) {
        const std::uint32_t maxLights = shadow_.mode == 2 ? kLightShadowMax : 8u;
        const float limit = shadow_.mode == 2 ? 6000.0f : 2500.0f;
        std::vector<std::pair<float, std::uint32_t>> nearLights;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto& w = scratch_[i];
            const float dx = w.pos[0] - cam.position[0], dy = w.pos[1] - cam.position[1], dz = w.pos[2] - cam.position[2];
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist < limit && w.range < 3000.0f) nearLights.emplace_back(dist, i);
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
            known->cells.emplace(knownKey(static_cast<std::int32_t>(std::floor(g.posRcp[0] / 16.0f)), static_cast<std::int32_t>(std::floor(g.posRcp[1] / 16.0f)),
                                          static_cast<std::int32_t>(std::floor(g.posRcp[2] / 16.0f))),
                                 std::array<float, 3>{g.posRcp[0], g.posRcp[1], g.posRcp[2]});
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
            pendingShadow_ = {dev_.frameValue(), sp, raw, fin, (w + 7) / 8, (h + 7) / 8};
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
    if (ctx.mainView != 1 || pendingShadow_.frame != dev_.frameValue()) return;
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
    spdlog::info("enh: lights: {} lights in the table ({} changed since the last frame, {} dropped beyond {}, {} from drawn lists), {} builds ({} skipped), style {}, "
                 "debug {}, table lighting {}",
                 lastCount_, lastChanged_, lastDropped_, kLightMax, lastDrawnAdded_, passes_, skipped_, style_ == 1 ? "physical" : "original", debug_,
                 fullLitActive() ? "on" : "off");
    if (shadowHeld_) {
        spdlog::info("enh: lights: lamp shadows: {} lights with a shadow id, {} frames traced, {} skipped (no RT scene / pipelines / depth)", shadowLights_, shadowPasses_,
                     shadowSkipped_);
    }
}

}  // namespace rvk::enh
