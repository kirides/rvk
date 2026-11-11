#pragma once

// Ray-traced reflections of the material pack's smooth surfaces (docs/research/material-rt.md; settings materials.quality = high +
// materials.rt_reflections): after the opaque scene a compute pass reads the G-buffer (the normal and the roughness / metalness the pack
// entries' draws wrote into its spare channel) and traces one reflection ray per pixel of smooth surfaces against the frame's TLAS (enh/rt_scene),
// the direction drawn from the GGX lobe of the roughness; a second pass gathers + accumulates the noisy image over time (depth / normal / roughness
// aware, motion reprojected, history clamped); a composite draw adds it onto the scene HDR target (and multiplies the scene where a surface darkens:
// puddles). The pack's raster sky reflection is switched off while this runs (the traced result includes the sky: a miss reads the sky cube).
//
// Why not the ray query in the material pixel shaders: it would put the ray-query code into ~2300 shader variants x 3 (register pressure, compile
// time); a screen-space pass over the G-buffer costs one dispatch and works for every material.
//
// Off (the setting, no ray tracing, no G-buffer): nothing is registered, no GPU memory, the raster's sky cube reflection as before.

#include <array>
#include <cstdint>
#include <future>
#include <memory>

#include "d3d9/device.h"
#include "rhi/rhi.h"
#include "shader/abi.h"

namespace rvk::enh {

class RtScene;

// mirror of shaders/materials/material_rt_common.hlsli MaterialRtParams (std430)
struct MaterialRtParams {
    std::uint32_t tex[4];    // depth map, G-buffer normal, scene HDR, linear clamp sampler
    std::uint32_t tex2[4];   // point clamp sampler, G-buffer motion, history (0 = none), raw reflection image
    std::uint32_t info[4];   // trace image width, height, pixels per trace pixel, frame index
    std::uint32_t trace[4];  // RVK_RT_TRACE_*, shadow rays, tweaks, debug
    float reach[4];          // reach, extra cone spread, shadow reach, roughness limit
    float strength[4];       // sky reflection, reflection strength, rain wetness, puddle strength
    float fogColor[4];
    float fogParams[4];
    float indoor[4];
    float sh[7][4];
    float sunColor[4];
    float sunDir[4];
};
static_assert(sizeof(MaterialRtParams) == 16 * (4 + 2 + 3 + 7 + 2));

// mirror of material_rt.comp / material_rt_filter.comp MaterialRtPush
struct MaterialRtPush {
    shader::abi::PassPushHeader h;
    std::uint32_t params[2];
    std::uint32_t pad[2];
};
static_assert(sizeof(MaterialRtPush) == 32 && sizeof(MaterialRtPush) <= shader::abi::kPushConstantBudget);

// mirror of material_rt.frag MaterialRtCompositePush
struct MaterialRtCompositePush {
    shader::abi::PassPushHeader h;
    std::uint32_t image, sampler, scale, depth;
};
static_assert(sizeof(MaterialRtCompositePush) == 32 && sizeof(MaterialRtCompositePush) <= shader::abi::kPushConstantBudget);

class MaterialRt final : public d3d9::DrawObserver {
public:
    struct Settings {
        int tier = 0;                // 0 off, 1 low (quarter-rate rays, cut-outs solid), 2 medium, 3 high (every pixel)
        float environment = 1.0f;    // materials.environment: the sky's share
        float strength = 1.0f;       // materials.rt_strength
        float rangeMetres = 150.0f;  // materials.rt_range
        float roughnessLimit = 0.7f; // surfaces rougher than this keep the raster's response only
        bool glass = true;           // materials.glass: panes of the pack's glass entries (windows, bottles) reflect / refract through the RT scene
        float rain = 1.0f;           // materials.rain_wetness: how strongly rain wets the pack's surfaces (0 = dry world)
        float puddles = 0.7f;        // materials.puddles: puddle coverage on flat ground while it rains / is wet
        float forceRain = 0.0f;      // materials.rain_force (debug): > 0 = this rain share instead of the weather's
        int tweaks = 0;              // 8: unshaded hits (cost measurements)
        int debug = 0;               // 1 hit / miss, 2 the raw image
    };

    MaterialRt(d3d9::Device& device, RtScene* scene);
    ~MaterialRt() override;
    MaterialRt(const MaterialRt&) = delete;
    MaterialRt& operator=(const MaterialRt&) = delete;

    // the ray tracing tier exists (the setting can act)
    bool available() const;
    void apply(const Settings& s);
    // the traced result replaces the raster's sky reflection (the G-buffer and the pipelines are ready and the tier is on)
    bool active() const { return tier_ > 0 && !failed_; }
    int tier() const { return tier_; }
    float wetness() const { return wet_; }

    // DrawObserver: the sun and the fog of the lit draws (the hits are lit with them)
    bool observes(d3d9::draws::Kind kind, d3d9::scene::Phase phase) const override;
    bool ownedTreesOk() const override { return true; }  // (F-VAR: reads the sun / fog registers only, never asks for a capture)
    bool ownedVegOk() const override { return true; }  // (F-VAR2: tallies registers only; the owned vegetation batches are shown through ownedVegObserve)
    const char* observerName() const override { return "material rt"; }
    Registers registersRead() const override { return Registers::MaterialLighting; }  // F-VAR: the sun (DirLight, PS window 3) and the fog constants (PS window 0)
    void observe(d3d9::Device& dev, const d3d9::ReplaceQuery& q) override;

private:
    void enable();
    void disable();
    void record(d3d9::PassContext& ctx);
    void recordGlass(d3d9::PassContext& ctx);
    void recordFilter(d3d9::PassContext& ctx);
    void recordComposite(d3d9::PassContext& ctx);
    // what the trace stage hands to the filter and composite stages of the same frame (three injected passes: -PerfFrame times each)
    struct Stage {
        std::uint64_t frame = ~0ull;
        d3d9::Texture* scene = nullptr;
        rhi::Texture *raw = nullptr, *histRead = nullptr, *histWrite = nullptr;
        MaterialRtPush push{};
        std::uint32_t w = 0, h = 0, tw = 0, th = 0, cur = 0, image = 0;
        bool filtered = false;
        std::uint32_t scale = 1, depth = 0;  // the trace image's scale; the depth map's slot
        bool halfOut = false;                // low bandwidth mode: the history has the trace image's size (scale > 1)
    } stage_;
    bool pipelinesReady(VkFormat target);
    void updateRain();
    void logStats();

    d3d9::Device& dev_;
    RtScene* scene_ = nullptr;
    Settings s_;
    int tier_ = 0;
    bool on_ = false, failed_ = false;
    std::unique_ptr<rhi::Pipeline> trace_, filter_, composite_, glassPipe_;
    std::future<std::unique_ptr<rhi::Pipeline>> glassBuild_;
    std::uint64_t glassPasses_ = 0;
    VkFormat compositeFormat_ = VK_FORMAT_UNDEFINED;
    std::future<std::unique_ptr<rhi::Pipeline>> traceBuild_, filterBuild_;
    // the frame's light (the draws' registers): sun, fog
    std::uint64_t lightFrame_ = ~0ull;
    float sunColor_[4]{}, sunDir_[4]{0.0f, 1.0f, 0.0f, 0.0f}, fogColor_[4]{}, fogParams_[4]{};
    bool haveFog_ = false;
    // the rain: the weather's rain share is read about once a second, the wetness follows it (enh/rain_wetness.h)
    float wet_ = 0.0f, rainShare_ = 0.0f;
    double lastWeather_ = -1e9, lastWetTime_ = -1.0;
    std::uint32_t history_ = 0;
    bool historyValid_ = false;
    std::uint32_t historyW_ = 0, historyH_ = 0;
    bool historyHalf_ = false;
    std::uint64_t passes_ = 0, skipped_ = 0;
    std::uint32_t lastTraceW_ = 0, lastTraceH_ = 0;
    double lastLog_ = -1e9;
};

}  // namespace rvk::enh
