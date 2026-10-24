#pragma once

// Enhanced sun shadows (settings category "Shadows"; docs/research/shadows.md). A draw replacer on d3d9::Device for the
// engine's screen-space shadow mask (eCIlluminationAdmin::UpdateShadowMask, post_SSM.fx):
//  * the engine draws one full-screen quad per sun cascade (technique SSM, near to far: each 8 dithered point taps, the
//    cascade chosen by the stencil, a hard switch at each split, the shadow lifted over the last fifth of the range, times
//    the terrain variance map); the replacer skips those draws and keeps what they set (the cascade's DepthToShadowMatrix,
//    its depth map, the split as the quad's depth, the variance map and its matrix);
//  * at the frame's last cascade (as many as the scene hooks saw cascade depth maps this frame) it draws the whole mask in
//    one pass (shaders/shadows/shadow_mask.*) on the engine's states: smooth PCF with a receiver-plane bias, the cascades
//    blended before each split, a long fade at the end of the last one, the engine's variance term on top. The engine's
//    terrain-only pass (VSSM) then has nothing left to draw; its blur of the mask runs unchanged.
// Off: the replacer is not registered; nothing changes.

#include <array>
#include <cstdint>
#include <memory>

#include "d3d9/device.h"

namespace rvk::enh {

// mirror of shaders/shadows/shadow_mask.frag ShadowBlock (std430)
struct ShadowBlock {
    float cascade[12][4];  // rows of each cascade's DepthToShadowMatrix
    float split[4];        // far end of each cascade, depth-map units
    float texel[3][4];     // x size, y 1 / size, z tap spacing (texels; softness, narrower in coarser cascades), w shadow depth per texel of a 45° slope
    std::uint32_t maps[4];  // bindless t2D of the cascade maps, w count
    std::uint32_t res[4];   // x depth map, y point clamp sampler, z variance map (0 = none), w linear clamp sampler
    float vsm[3][4];       // DepthToVarianceShadowMatrix rows
    float vsmParams[4];    // x MinVariance, y MinLightBleeding, z 1 / (1 - MinLightBleeding)
    float params[4];       // x blend band, y fade band, z constant bias (texels), w receiver slope limit (tan)
    float target[4];       // x 1 / width, y 1 / height, z width, w height
    std::uint32_t mode[4];  // x taps per axis, y debug view
    std::uint32_t rt[4];    // x the ray-traced visibility (bindless t2D, enh/rt_shadows; 0 = none)
    float rtParams[4];      // x range (depth-map units), y blend band
};
static_assert(sizeof(ShadowBlock) == 27 * 16);

// The view depth (in units of `engineFar`, the depth map's) of a hardware depth `z` under a D3D perspective projection
// (row-vector convention: z' = (z * m22 + m32) / z); false when the projection is not perspective or z is at infinity
bool viewDepthOfHardwareZ(const d3d9::scene::Mat4& projection, float z, float engineFar, float& depthMapUnits);

class RtShadows;

class Shadows final : public d3d9::DrawReplacer {
public:
    struct Settings {
        bool enabled = false;
        int taps = 3;           // shadows.filter: bilinear taps per axis (2 low, 3 medium, 4 high)
        float softness = 1.0f;  // shadows.softness: tap spacing in texels
        float blend = 0.2f;     // shadows.cascade_blend: fraction of a cascade's depth range blended into the next
        float fade = 0.35f;     // shadows.fade: fraction of the last cascade's range over which the shadow fades out
        float bias = 0.5f;      // shadows.bias: constant depth bias in texels of a 45° slope
        float maxSlope = 3.0f;  // steepest receiver slope (tan) the receiver-plane bias follows
        int debug = 0;          // shadows.debug: 0 off, 1 cascades (the mask shows the cascade: 25 / 50 / 75 %, lit beyond),
                                // 2 the cascade term only, 3 the terrain term only, 4 the ray-traced term only, 5 its coverage
        float rtBlend = 0.25f;  // fraction of the ray-traced range blended out to the cascades
    };

    explicit Shadows(d3d9::Device& device);
    ~Shadows() override;
    Shadows(const Shadows&) = delete;
    Shadows& operator=(const Shadows&) = delete;

    // render thread (settings callbacks run at the frame boundary)
    void apply(const Settings& s);
    const Settings& settings() const { return settings_; }
    // the ray-traced shadows whose result the mask takes inside their range (enh/rt_shadows; null = cascades only)
    void setRtShadows(const RtShadows* rt) { rt_ = rt; }

    // the shadow-mask pass of the main view
    bool handles(d3d9::draws::Kind kind, d3d9::scene::Phase phase, const d3d9::draws::Tag& tag) const override;
    void replace(d3d9::Device& device, const d3d9::ReplaceQuery& q, d3d9::DrawReplacement& out) override;

private:
    struct Cascade {
        float rows[4][4]{};
        float quadZ = 0.0f;  // the engine quad's depth (VS c2.x): the split as hardware depth
        d3d9::Texture* map = nullptr;
    };
    bool ensureShaders();
    void destroyShaders();
    // the mask draw over everything left in the stencil (the frame's captured cascades; `variance` from the current draw)
    void drawMask(d3d9::Device& device, const d3d9::ReplaceQuery& q, int varianceReg, int minVarianceReg, int minBleedingReg,
                  int varianceSampler, int depthSampler, d3d9::DrawReplacement& out);

    d3d9::Device& dev_;
    const RtShadows* rt_ = nullptr;
    Settings settings_;
    bool registered_ = false;
    bool failed_ = false;
    std::shared_ptr<d3d9::Shader> vs_, ps_;
    // this frame's capture
    std::uint64_t frame_ = ~0ull;
    std::array<Cascade, 3> cascades_{};
    int captured_ = 0;
    bool drawn_ = false;
    bool skipFrame_ = false;  // the frame's draws stay the engine's (no camera, unknown layout)
    bool loggedFirst_ = false;
};

}  // namespace rvk::enh
