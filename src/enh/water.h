#pragma once

// Enhanced water (settings category "Water", default off; docs/research/water.md). A draw replacer on d3d9::Device:
//  * the engine's water draws (the Refracted pass, right after the refraction copy) are drawn with the GLSL water shaders
//    instead (shaders/water/*): the engine's own geometry, transform and constants, our shading (refraction + absorption,
//    SSR + sky cube reflection, sun glint, foam, the engine's fog), with a profile per body of water derived from the
//    material (enh/water_profile.h);
//  * the open sea (the engine's world-spanning sea plane, found by its geometry) is replaced by a camera-centred wave grid
//    (water.ocean_waves): waves offshore, damped at the coast and near the camera, a lower waterline with swash at the
//    coast, using the shore field baked from the engine's own coast overlay (enh/shore_field.h); that overlay (the "fake
//    shore", Special_Water_Shore_*) is then not drawn;
//  * waterfalls, glass, crystals and everything else keep the engine's shading.
// Off: the replacer answers "engine" for every draw; nothing else changes.

#include <array>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "d3d9/device.h"
#include "enh/shore_field.h"
#include "enh/water_profile.h"

namespace rvk::enh {

// mirror of shaders/water/water_common.glsl WaterBlock (all vec4 / uvec4: std430 = this layout)
struct WaterBlock {
    float posFix[4];
    float sunColor[4];
    float sunSpec[4];
    float sunDir[4];
    float fogColor[4];
    float fogParams[4];
    float ambient[3][4];
    float scatter[4];
    float absorb[4];
    float surface[4];
    float foam[4];
    float ocean[4];
    float grid[4];
    float shoreMap[4];
    float misc[4];
    std::uint32_t tex[4];
    std::uint32_t tex2[4];
    std::uint32_t smp[4];
    float falls[4][4];
    float lights[12][4];
    float look[4];
};
static_assert(sizeof(WaterBlock) == 37 * 16);

class Water final : public d3d9::DrawReplacer {
public:
    struct Settings {
        bool enabled = false;
        int quality = 1;           // 0 low, 1 medium, 2 high
        bool ssr = true;           // water.reflections: ssr (else the sky cube only)
        bool oceanWaves = true;    // the wave grid for the open sea + lower coast (else the sea is shaded on its own mesh)
        float waveHeight = 1.0f;   // multiplier
        float murkiness = 1.0f;    // multiplier on the absorption
        float reflections = 1.0f;  // multiplier on reflectivity + glint
        int debug = 0;             // water.debug: 0 off, 1 shore field, 2 depth, 3 normals, 4 reflection
    };

    explicit Water(d3d9::Device& device);
    ~Water() override;
    Water(const Water&) = delete;
    Water& operator=(const Water&) = delete;

    // render thread (settings callbacks run at the frame boundary)
    void apply(const Settings& s);
    const Settings& settings() const { return settings_; }

    void replace(d3d9::Device& device, const d3d9::ReplaceQuery& q, d3d9::DrawReplacement& out) override;

private:
    struct Body {
        bool sea = false;  // the world-spanning sea plane
        float minY = 0.0f, maxY = 0.0f, level = 0.0f;
        float extentX = 0.0f, extentZ = 0.0f;
        std::uint32_t vertices = 0;
    };
    bool ensureShaders();
    void destroyShaders();
    const Body& body(d3d9::Device& device, const d3d9::ReplaceQuery& q);
    void collectCoast(d3d9::Device& device, const d3d9::ReplaceQuery& q);
    void serviceBake(d3d9::Device& device);
    void prebuild();
    bool worldRows(d3d9::Device& device, const d3d9::ReplaceQuery& q, float rows[3][4]) const;
    VkDeviceAddress fillBlock(d3d9::Device& device, const d3d9::ReplaceQuery& q, d3d9::draws::Water kind, bool grid, const Body& b,
                              std::array<d3d9::Texture*, 6>& sampled);

    d3d9::Device& dev_;
    Settings settings_;
    bool failed_ = false;
    std::shared_ptr<d3d9::Shader> meshVs_, oceanVs_, ps_;
    std::unordered_map<std::uint64_t, Body> bodies_;  // by vertex buffer id
    float fallCentre_[2]{};                           // xz box centre of the body measured last (body())
    // the coast (the engine's shore overlay meshes): triangles in world space, baked into the shore field
    std::unordered_set<std::uint64_t> coastMeshes_;    // vertex buffer id ^ index buffer id << 1 seen
    std::vector<shore::Triangle> coast_;
    bool coastDirty_ = false;
    std::future<shore::Field> bake_;
    std::unique_ptr<d3d9::Texture> field_;
    std::array<float, 4> fieldMapping_{};
    // waterfalls seen (Alpha-pass water of kind Waterfall): base centre, radius, base height; by vertex buffer id
    struct Fall {
        float x, z, radius, baseY;
    };
    std::unordered_map<std::uint64_t, Fall> falls_;
    std::uint64_t gridFrame_ = ~0ull;      // device frame whose sea grid was drawn
    std::uint32_t gridMainView_ = 0;
    std::uint64_t replacedDraws_ = 0, skippedDraws_ = 0;
    bool loggedFirst_ = false, loggedNoInputs_ = false, prebuilt_ = false, pipelinesReady_ = false;
};

}  // namespace rvk::enh
