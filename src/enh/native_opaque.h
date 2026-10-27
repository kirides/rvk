#pragma once

// The native opaque scene (docs/research/native-renderer.md N3 / 14, setting `native.opaque`, default off): the world's static meshes drawn
// by our own passes instead of the engine's z pass and solid pass.
//
// * BeforeDepthPrepass (the engine's z pass is about to start): the part list is built from the scene mirror (the engine's own entity rules,
//   frustum and view range culling, the lighting composed from the entities' illumination; no engine draw is needed), split into two classes
//   (opaque, alpha tested), sorted front to back, their indices put into the native index arena; one item block per part and one indirect
//   command per part go into the frame's ring. The depth pre-pass draws each class with ONE indirect call into the hardware depth (reversed,
//   debug.reverse_z) and the engine's linear depth map: the opaque class has no discard and no texture fetch, only the alpha tested class
//   clips. The engine's own z pass and solid pass draws of listed parts are skipped.
// * BeforeSolid (under everything the engine's solid pass blends on top: terrain layers, decals): the colour pass draws the same two classes,
//   again one indirect call each, depth tested nearer-or-equal against the pre-pass (written a hair farther), no alpha test in the opaque class: static_mesh.* with the engine's material lighting
//   composed natively (shaders/native/).
// Left to the engine: fading parts, blended terrain layers, instanced meshes, characters, SpeedTree, vegetation, particles, water, sky.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "d3d9/device.h"
#include "enh/native_index_arena.h"
#include "vk/passes.h"
#include "vk/vk.h"

namespace rvk::enh {

// mirror of shaders/native/static_mesh.glsl NativeItem (928 bytes): one per drawn part, in the frame's ring
struct NativeItemBlock {
    float ps[48][4]{};             // pixel shader constants c0..c47 of the part (composed natively)
    float row[3][4]{};             // object -> world, 3x4 row-major
    std::uint32_t tex0[4]{};       // diffuse slot, sampler, normal slot, sampler
    std::uint32_t tex1[4]{};       // shadow mask slot, sampler, lightmap 1 slot, lightmap 2 slot
    std::uint32_t tex2[4]{};       // lightmap sampler, flags, stride, 0
    std::uint32_t off0[4]{0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};  // normal, tangent, color0, color1
    std::uint32_t off1[4]{0xFFFFu, 0xFFFFu, 0xFFFFu, 0};        // uv (diffuse), uv (normal), lightmap uv, point count | alpha func << 8
    float params[4]{};             // x alpha reference 0..255
    std::uint32_t addr[4]{};       // xy: stream 0 start (device address)
};
static_assert(sizeof(NativeItemBlock) == 928);

// mirror of shaders/native/static_mesh.glsl NativePush (32 bytes)
struct NativeStaticPush {
    VkDeviceAddress frame = 0;
    std::uint32_t passId = 0, passFlags = 0;
    std::uint32_t items[2]{};   // the NativeItem array
    std::uint32_t shadow[2]{};  // NativeShadowBlock (0: none)
};
static_assert(sizeof(NativeStaticPush) == 32);

// An engine draw of a listed part: its entity, index buffer and index range (its z pass draws carry the entity but no sub-mesh; the solid pass draws both)
inline std::uint64_t partKey(const void* entity, std::uint64_t indexBufferId, std::uint32_t firstIndex, std::uint32_t indexCount) {
    std::uint64_t h = reinterpret_cast<std::uintptr_t>(entity) * 0x9E3779B97F4A7C15ull;
    h ^= (indexBufferId + 0x7F4A7C15ull) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
    h ^= (static_cast<std::uint64_t>(firstIndex) << 32 | indexCount) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
    return h;
}

class NativeShadows;
class NativeParts;

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
    void setParts(NativeParts* parts) { parts_ = parts; }
    // the native sun cascades the shader samples (null: the engine's mask)
    // native.debug: 0 off, 1 albedo, 2 normal, 3 mask shadow, 4 mask ao, 5 ambient, 6 lightmap, 7 sun, 8 flags
    void setShadows(const NativeShadows* shadows) { shadows_ = shadows; }

    bool handles(d3d9::draws::Kind kind, d3d9::scene::Phase phase, const d3d9::draws::Tag& tag) const override;
    void replace(d3d9::Device& device, const d3d9::ReplaceQuery& q, d3d9::DrawReplacement& out) override;

private:
    struct Item {
        std::uint32_t arenaFirst = 0, indexCount = 0;
        NativeItemBlock block;                 // everything the shaders read except the textures' bindless slots (set in the pre-pass)
        d3d9::Texture* diffuse = nullptr;      // the textures, made readable and given their slots in the pre-pass
        d3d9::Texture* normal = nullptr;
        d3d9::Texture* lightmap[2]{};
        std::uint32_t diffuseSampler = 0, normalSampler = 0, lightmapSampler = 0;
        bool masked = false;
        float dist = 0.0f;                     // the entity's distance from the camera (front to back order)
        std::uint64_t ibId = 0;                // the part's index buffer and range: how the engine's own draws of it are recognised (its z pass carries no sub-mesh)
        std::uint32_t firstIndex = 0;
        const void* entity = nullptr;
        const void* subMesh = nullptr;
    };
    // the frame's list as the GPU sees it
    struct FrameList {
        std::uint64_t frame = 0;
        std::uint32_t opaque = 0, masked = 0;  // counts; the commands / items are opaque first, then masked
        VkBuffer commandBuffer = VK_NULL_HANDLE;
        VkDeviceSize commandOffset = 0;
        VkDeviceAddress items = 0;
        NativeItemBlock* itemMemory = nullptr;  // the ring memory behind `items` (the colour pass patches the per-view constants in place)
        bool drawn = false;                     // the pre-pass ran
    };
    enum class Pipe : std::uint32_t { PrepassOpaque, PrepassMasked, ColorOpaque, ColorMasked, Count };

    void prepass(d3d9::PassContext& ctx);       // BeforeDepthPrepass: list, arena, ring data, the depth pre-pass
    void colorPass(d3d9::PassContext& ctx);     // BeforeSolid
    void buildList(d3d9::PassContext& ctx);     // the part list from the mirror into items_
    bool composeLighting(const void* entity, NativeItemBlock& blk, std::uint32_t& lights) const;  // c14..c40 from the entity's illumination
    void compareLighting(const Item& listed, const d3d9::Device& device);  // the composed block against the constants of the engine's own draw
    bool ensurePipelines(VkFormat depthMapFormat, VkFormat color, VkFormat depth);
    void destroyPipelines();
    void drawClass(VkCommandBuffer cmd, std::uint32_t first, std::uint32_t count, VkPipeline pipeline, VkCullModeFlags cull, const NativeStaticPush& push);
    void report();

    d3d9::Device& dev_;
    bool enabled_ = false;
    std::uint32_t debug_ = 0;
    const NativeShadows* shadows_ = nullptr;
    NativeParts* parts_ = nullptr;
    std::uint32_t colorPassId_ = 0, prepassId_ = 0;
    bool registered_ = false;
    bool failed_ = false;

    NativeIndexArena arena_;
    std::vector<Item> items_;                                             // this frame's list: opaque first, then masked, each front to back
    std::vector<std::uint8_t> listSeen_;                                  // the engine drew this listed part too (in the pass it has not been skipped)
    std::unordered_map<std::uint64_t, std::uint32_t> listIndex_;          // key(entity, sub-mesh) -> items_
    FrameList list_;
    struct ViewConsts {                                                   // the engine's per-view constants, taken from its latest main-view solid draw
        float fog[2][4]{};                                                // c3, c4
        d3d9::Texture* mask = nullptr;                                    // s0: the screen-space shadow mask
        std::uint32_t maskSampler = 0;
        std::uint64_t frame = 0;
    } view_;

    vk::PassLayout layout_;
    VkPipeline pipes_[static_cast<std::size_t>(Pipe::Count)]{};
    VkFormat depthMapFormat_ = VK_FORMAT_UNDEFINED, colorFormat_ = VK_FORMAT_UNDEFINED, depthFormat_ = VK_FORMAT_UNDEFINED;

    // diagnostics (rvk.log `enh: native opaque:` every 5 s)
    struct Stats {
        std::uint64_t taken = 0, passes = 0, drawnOpaque = 0, drawnMasked = 0, indirectCalls = 0;
        std::uint64_t listBuilt = 0, listParts = 0, listEntities = 0, listFading = 0, listNoLight = 0, listNoIndices = 0, listStale = 0, listPending = 0, listEngineDrew = 0, listUnseen = 0,
                      listFrames = 0;
        std::uint64_t cullHist[2][2][4]{};  // the engine's static draws: [solid / z pass][unlisted / listed][none / front / back / other]
        std::uint64_t zfunc[2][9]{}, zwrite[2][2]{};  // the engine's static draws: z function / depth write, solid (0) and z pass (1)
        std::uint64_t cmpDraws = 0, cmpMismatch[6]{};  // per register group: camera, fog, indoor, SH, sun, lights
    } stats_;
    double lastReport_ = 0.0;
};

}  // namespace rvk::enh
