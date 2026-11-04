#include "enh/light_table.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <spdlog/spdlog.h>

#include "enh/rhi_scope.h"
#include "shader/embedded.h"

namespace rvk::enh {

namespace {
constexpr float kSpecularShare = 0.5f;   // specular colour = diffuse x this (the engine's c27 of point lights is a share of c26; measured in the LogFrame constants)
constexpr std::uint32_t kClusters = kLightGridX * kLightGridY * kLightGridZ;
constexpr std::uint64_t kLightsOffset = kLightHeaderBytes;
constexpr std::uint64_t kClustersOffset = kLightsOffset + std::uint64_t{kLightMax} * kLightBytes;
constexpr std::uint64_t kBufferBytes = kClustersOffset + std::uint64_t{kClusters} * kLightClusterBytes;
}  // namespace

float lightSliceScale(float nearZ, float farZ, std::uint32_t slices) { return static_cast<float>(slices) / std::log(farZ / nearZ); }

std::uint32_t lightSlice(float viewZ, float nearZ, float slicesPerLn, std::uint32_t slices) {
    const float s = std::log(std::max(viewZ, nearZ) / nearZ) * slicesPerLn;
    return std::min(static_cast<std::uint32_t>(s), slices - 1u);
}

float lightSliceNear(std::uint32_t slice, float nearZ, float slicesPerLn) { return nearZ * std::exp(static_cast<float>(slice) / slicesPerLn); }

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
    configure(false, 0, 0);
    if (building_.valid()) building_.get();
}

void LightTable::configure(bool enabled, int style, int debug) {
    if (failed_) enabled = false;
    style_ = style;
    debug_ = debug;
    if (!failed_) dev_.setFrameParam(shader::abi::kFrameParamLights + 1, {enabled ? 1.0f : 0.0f, static_cast<float>(style), static_cast<float>(debug), 0.0f});
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
        ++skipped_;
        return;
    }
    const auto& cam = dev_.camera().valid ? dev_.camera() : dev_.lastCamera();
    if (!cam.valid) {
        ++skipped_;
        return;
    }
    // the world's lights within the grid's reach, nearest first when there are more than the table holds
    if (!engine::lights::collect(cam.position.data(), kLightFar, scratch_, stats_)) {
        ++skipped_;
        return;
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
    }
    // batch change detection against the previous upload (the statistics of how many lights move: static ones are the same every frame)
    lastChanged_ = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (i >= prev_.size() || std::memcmp(&prev_[i], &cur_[i], sizeof(GpuLight)) != 0) ++lastChanged_;
    }
    changedTotal_ += lastChanged_;
    prev_.swap(cur_);
    lastCount_ = count;

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

void LightTable::logStats() {
    const double now = dev_.secondsSinceCreate();
    if (now - lastLog_ < 5.0) return;
    lastLog_ = now;
    spdlog::info("enh: lights: {} lights in the table ({} changed since the last frame, {} dropped beyond {}), {} builds ({} skipped), style {}, debug {}",
                 lastCount_, lastChanged_, lastDropped_, kLightMax, passes_, skipped_, style_ == 1 ? "physical" : "original", debug_);
}

}  // namespace rvk::enh
