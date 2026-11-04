#pragma once

// The Full-owned light table and its cluster grid (setting lighting.clustered, docs/research/gpu-scene.md 9.7).
//
// Every frame (main view, injection point AfterDepthPrepass) the world's lights (engine/lights: the cached-in static lights and the registered dynamic
// ones) are written into a CPU upload buffer, and one compute pass (shaders/lighting/light_cluster.comp.hlsl) copies them into the device buffer the
// material shaders read, builds a view-space cluster grid (16 x 9 x 24, exponential slices) and writes each cluster's light list. The material pixel
// shader variants (enh/material_variant RVK_MV_CLUSTER, shaders/include/rvk_light_model.hlsli) then fill the engine's point-light registers per PIXEL
// from the cluster instead of per object: lights beyond the engine's per-object reach (<= 2 static + 2 dynamic, 3 per pass) light the surfaces they
// reach, the engine's own shader evaluates them (falloff, N.L, colour, ambient SH, sun, encode: the engine's look). `lighting.style` picks the distance law
// (original = the engine's, physical = inverse square window); `lighting.debug` shows the grid. Nothing exists while the setting is off; the engine's
// per-draw lists and registers are never changed (the variant only reads them).
//
// Stage (b) (GPU-driven draws that consume the table instead of engine lists) and stage (c) (point-light shadow cubes) build on the same buffer.

#include <array>
#include <cstdint>
#include <future>
#include <memory>
#include <vector>

#include "d3d9/device.h"
#include "engine/lights.h"
#include "rhi/rhi.h"
#include "shader/abi.h"

namespace rvk::enh {

inline constexpr std::uint32_t kLightMax = 1024;               // RVK_LIGHT_MAX
inline constexpr std::uint32_t kLightBytes = 64;               // RVK_LIGHT_BYTES
inline constexpr std::uint32_t kLightHeaderBytes = 64;         // RVK_LIGHT_HEADER_BYTES
inline constexpr std::uint32_t kLightClusterBytes = 256;       // RVK_LIGHT_CLUSTER_BYTES
inline constexpr std::uint32_t kLightMaxPerCluster = 63;       // RVK_LIGHT_MAX_PER_CLUSTER
inline constexpr std::uint32_t kLightGridX = 16, kLightGridY = 9, kLightGridZ = 24;
inline constexpr float kLightNear = 20.0f, kLightFar = 16000.0f;  // the grid's depth range (engine units); lights are collected over the far range

// mirror of shaders/include/rvk_light_model.hlsli RvkLight (64 bytes)
struct GpuLight {
    float posRcp[4];     // xyz, 1 / R^2
    float colRange[4];   // rgb, R
    float specFlags[4];  // rgb specular, flags (uint bits)
    float misc[4];       // id (uint bits), 0, 0, 0
};
static_assert(sizeof(GpuLight) == kLightBytes);

// mirror of light_cluster.comp.hlsl LightClusterPush
struct LightClusterPush {
    shader::abi::PassPushHeader h;
    std::uint32_t src[2];
    std::uint32_t dst[2];
    std::uint32_t grid[4];
    float zs[4];
    std::uint32_t offs[4];
};
static_assert(sizeof(LightClusterPush) == 80 && sizeof(LightClusterPush) <= shader::abi::kPushConstantBudget);

// pure cluster math (rvk_tests; the same formulas as the shader)
float lightSliceScale(float nearZ, float farZ, std::uint32_t slices);                    // slices / ln(far / near)
std::uint32_t lightSlice(float viewZ, float nearZ, float slicesPerLn, std::uint32_t slices);
float lightSliceNear(std::uint32_t slice, float nearZ, float slicesPerLn);               // the view depth where `slice` begins
// the physical distance law relative to the engine's (rvkLmPhysicalRatio)
float lightPhysicalRatio(float d2, float rcpSqrRange);
// the engine's falloff 1 - sat(d^2 / R^2)
float lightEngineFalloff(float d2, float rcpSqrRange);

class LightTable {
public:
    explicit LightTable(d3d9::Device& device);
    ~LightTable();
    LightTable(const LightTable&) = delete;
    LightTable& operator=(const LightTable&) = delete;

    bool available() const { return !failed_; }
    // run time: on/off (the pass exists only while on), style 0 original / 1 physical, debug 0 off / 1 cluster counts / 2 slots filled
    void configure(bool enabled, int style, int debug);

private:
    void record(d3d9::PassContext& ctx);
    bool pipelineReady();
    void logStats();

    d3d9::Device& dev_;
    bool enabled_ = false, failed_ = false;
    int style_ = 0, debug_ = 0;
    std::uint32_t passId_ = 0;
    std::unique_ptr<rhi::Pipeline> pipeline_;
    std::future<std::unique_ptr<rhi::Pipeline>> building_;
    std::unique_ptr<rhi::Buffer> device_;                                          // header + lights + clusters
    std::array<std::unique_ptr<rhi::Buffer>, rhi::kFramesInFlight> upload_;        // the CPU's lights, per frame in flight
    std::vector<engine::lights::WorldLight> scratch_;
    std::vector<GpuLight> prev_, cur_;
    engine::lights::Stats stats_;
    std::uint64_t passes_ = 0, skipped_ = 0, changedTotal_ = 0;
    std::uint32_t lastCount_ = 0, lastChanged_ = 0, lastDropped_ = 0;
    double lastLog_ = -1e9;
};

}  // namespace rvk::enh
