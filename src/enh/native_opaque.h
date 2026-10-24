#pragma once

// The native opaque pass (docs/research/native-renderer.md N3, setting `native.opaque`, default off): our own drawing of the world's
// static meshes. A draw replacer skips the engine's main-view solid draws of plain indexed static meshes it can draw itself (the
// mesh's buffers, material and transform known), keeps what each draw was, and an injected pass at AfterOpaque draws them again with
// shaders/native/static_mesh.* (vertices pulled from the engine's own buffers by address, the diffuse texture of the engine's material
// graph) into the scene HDR target and the hardware depth. The z pre-pass, the sun cascades, the shadow mask and everything else
// the engine draws stay the engine's. First light: the draws are the engine's (culling, level of detail, fade); the next stages cull
// and list the parts from the scene mirror (scene/mirror.h) instead.

#include <cstdint>
#include <vector>

#include "d3d9/device.h"
#include "vk/passes.h"
#include "vk/vk.h"

namespace rvk::enh {

// mirror of shaders/native/static_mesh.glsl NativeItem (912 bytes): one per drawn part, in the frame's ring
struct NativeItemBlock {
    float ps[48][4]{};             // the engine's pixel shader constants c0..c47 of the draw
    float row[3][4]{};             // object -> world, 3x4 row-major
    std::uint32_t tex0[4]{};       // diffuse slot, sampler, normal slot, sampler
    std::uint32_t tex1[4]{};       // shadow mask slot, sampler, lightmap 1 slot, lightmap 2 slot
    std::uint32_t tex2[4]{};       // lightmap sampler, flags, stride, 0
    std::uint32_t off0[4]{0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};  // normal, tangent, color0, color1
    std::uint32_t off1[4]{0xFFFFu, 0xFFFFu, 0xFFFFu, 0};        // uv (diffuse), uv (normal), lightmap uv, point count | alpha func << 8
    float params[4]{};             // x alpha reference 0..255
};
static_assert(sizeof(NativeItemBlock) == 912);

// mirror of shaders/native/static_mesh.glsl NativePush (48 bytes)
struct NativeStaticPush {
    VkDeviceAddress frame = 0;
    std::uint32_t passId = 0, passFlags = 0;
    std::uint32_t item[2]{};
    std::uint32_t vertices[2]{};  // stream 0 start (address)
    std::uint32_t indices[2]{};   // index buffer start (address)
    std::uint32_t shadow[2]{};    // NativeShadowBlock (0: none)
};
static_assert(sizeof(NativeStaticPush) == 48);

class NativeShadows;

class NativeOpaque final : public d3d9::DrawReplacer {
public:
    explicit NativeOpaque(d3d9::Device& device);
    ~NativeOpaque() override;
    NativeOpaque(const NativeOpaque&) = delete;
    NativeOpaque& operator=(const NativeOpaque&) = delete;

    // render thread (settings callbacks run at the frame boundary)
    void apply(bool enabled);
    bool enabled() const { return enabled_; }
    void setDebug(std::uint32_t mode);
    void setShadows(const NativeShadows* shadows) { shadows_ = shadows; }  // the native sun cascades the shader samples (null: the engine's mask)  // native.debug: 0 off, 1 albedo, 2 normal, 3 mask shadow, 4 mask ao, 5 ambient, 6 lightmap, 7 sun, 8 flags

    bool handles(d3d9::draws::Kind kind, d3d9::scene::Phase phase, const d3d9::draws::Tag& tag) const override;
    void replace(d3d9::Device& device, const d3d9::ReplaceQuery& q, d3d9::DrawReplacement& out) override;

private:
    struct Item {
        VkDeviceAddress vertices = 0, indices = 0;
        std::uint32_t first = 0, indexCount = 0;
        NativeItemBlock block;                 // everything the shaders read except the textures' bindless slots (set in the pass)
        d3d9::Texture* diffuse = nullptr;      // the textures, made readable and given their slots in the pass
        d3d9::Texture* normal = nullptr;
        d3d9::Texture* mask = nullptr;
        d3d9::Texture* lightmap[2]{};
        std::uint32_t diffuseSampler = 0, normalSampler = 0, maskSampler = 0, lightmapSampler = 0;
        bool masked = false;
        VkCullModeFlags cull = VK_CULL_MODE_BACK_BIT;  // the engine's cull mode of the draw (D3DRS_CULLMODE)
        const void* entity = nullptr;
        const void* subMesh = nullptr;
    };
    void pass(d3d9::PassContext& ctx);
    bool ensurePipeline(VkFormat color, VkFormat depth);
    void destroyPipeline();
    void report();

    d3d9::Device& dev_;
    bool enabled_ = false;
    std::uint32_t debug_ = 0;
    const NativeShadows* shadows_ = nullptr;
    std::uint32_t passId_ = 0;
    bool registered_ = false;
    bool failed_ = false;

    std::vector<Item> items_;  // this view's skipped draws, drawn at AfterOpaque
    std::uint64_t itemsFrame_ = 0;

    vk::PassLayout layout_;
    VkPipeline pipeline_ = VK_NULL_HANDLE, pipelineMasked_ = VK_NULL_HANDLE;
    VkFormat colorFormat_ = VK_FORMAT_UNDEFINED, depthFormat_ = VK_FORMAT_UNDEFINED;
    std::uint32_t pipelineState_ = 0;  // 0 depth test + culling, 1 neither, 2 depth test only, 3 culling only (the debug views)

    // diagnostics (rvk.log `enh: native opaque:` every 5 s)
    struct Stats {
        std::uint64_t taken = 0, drawn = 0, passes = 0;
        std::uint64_t declined[8]{};  // by reason (kReasons)
        std::uint64_t rowsMismatch = 0, rowsCompared = 0;
    } stats_;
    double lastReport_ = 0.0;
    int loggedDecl_ = 0;
};

}  // namespace rvk::enh
