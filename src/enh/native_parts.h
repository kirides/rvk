#pragma once

// Mirror parts as GPU-ready draw data (docs/research/native-renderer.md N1 / N4): an entity's mesh context part (scene::MeshPart of a
// scene::MeshInfo) resolved to its buffers' device addresses, its vertex layout (from the engine's declaration handle = our
// d3d9::VertexDecl) and its material (the engine's material graph: diffuse texture, blend mode, mask reference), without any draw. Used by the
// native shadow cascades and (next) by the native opaque pass. The geometry resolution is cached per sub-mesh (validated by the identity of the
// resource's buffers, dropped when the device says a buffer changed); the textures are cached per material and dropped when the device releases
// one (this class is a DrawObserver that observes no draws). Render thread.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include "d3d9/device.h"
#include "scene/mirror.h"
#include "vk/vk.h"

namespace rvk::enh {

// byte offsets of a static mesh vertex's attributes in stream 0 (0xFFFF = absent)
struct NativeLayout {
    bool positionOk = false;
    std::uint32_t normal = 0xFFFFu, tangent = 0xFFFFu, color0 = 0xFFFFu, color1 = 0xFFFFu, lightmap = 0xFFFFu, uv[2]{0xFFFFu, 0xFFFFu};
};
NativeLayout nativeLayoutOf(const d3d9::VertexDecl* decl);

struct NativePart {
    VkDeviceAddress vertices = 0, indices = 0;  // stream 0 start, index buffer start
    std::uint32_t stride = 0, index32 = 0, first = 0, indexCount = 0;
    NativeLayout layout;
    d3d9::Texture* diffuse = nullptr;           // the material graph's diffuse image (null: none)
    std::uint32_t diffuseUvSet = 0;
    d3d9::Texture* normal = nullptr;            // its normal map (DXT5nm: the engine converts them), null: none
    std::uint32_t normalUvSet = 0;
    d3d9::Texture* lightmap[2]{};               // the mesh resource's directional lightmaps (res +0x90 / +0x98), null until loaded
    d3d9::Texture* opacity = nullptr;           // the graph's own opacity texture (a separate sampler), null: the diffuse's alpha / opaque
    std::uint32_t opacityChannel = 3;           // its channel (0 r .. 3 a)
    bool opacityOpaque = false;                 // the opacity role has no texture we can read: treat as opaque
    const std::string* name = nullptr;          // the material's diagnostic name (pool image of the diffuse), stable while NativeParts lives
    // the buffers behind the addresses (the RT scene's BLAS cache keys on them)
    d3d9::Buffer* vertexBuffer = nullptr;
    d3d9::Buffer* indexBuffer = nullptr;
    std::uint64_t vbId = 0, ibId = 0;
    std::uint32_t vbGen = 0, ibGen = 0;
    bool masked = false;                        // blend mode masked: alpha tested against alphaRef
    std::uint32_t opacitySwizzle = 0;           // the opacity role proxy's channel swizzle (0 none, 1 .rgb, 2 .r, 3 .g, 4 .b, 5 .a): a terrain layer's weight channel of the vertex colour
    bool layer = false;                         // a terrain layer: blended over other layers by the vertex colour (only with allowLayers: the RT scene takes it as a base surface)
    float alphaRef = 0.0f;                      // 0..255
    const void* material = nullptr;
    // an image of the part (diffuse, normal map, opacity, lightmaps) is not completely loaded yet (eCResource2 state != 3): the engine's loader fills a texture's
    // mip levels progressively and the engine itself only samples it once loaded; sampled earlier, the unfilled levels are black (and the picture flickers between
    // the mip levels with the camera). The native passes leave such a part to the engine.
    bool pending = false;
};

class NativeParts final : public d3d9::DrawObserver {
public:
    // resolves the engine's vertex declaration handle (visual +0x50) to our d3d9::VertexDecl (HalDevice::declFromHandle)
    using DeclResolver = std::function<d3d9::VertexDecl*(const void*)>;
    NativeParts(d3d9::Device& device, DeclResolver resolver);
    ~NativeParts() override;
    NativeParts(const NativeParts&) = delete;
    NativeParts& operator=(const NativeParts&) = delete;

    // false when the part cannot be drawn opaquely by the native passes (no buffers, not a material shader of the default / leaf class, blended,
    // unsupported layout). `out` is valid for this frame.
    // `touch`: refresh the part's material and images in the engine's resource cache (every draw of the engine does: without it its LRU unloads what only
    // we use); the opaque pass wants it, the shadow cascades only need what is loaded
    // Reject: Material = a terrain layer where layers are not allowed; Skipped = a material that is never drawn opaquely (water, overlays, other shader classes, additive
    // blends); Buffers / Declaration = the part cannot be drawn at all
    enum class Reject : std::uint8_t { None, Material, Buffers, Declaration, Skipped };
    bool resolve(const scene::MeshInfo& mesh, const scene::MeshPart& part, NativePart& out, bool touch = true, Reject* why = nullptr, bool allowLayers = false);

    struct Stats {
        std::uint64_t resolved = 0, cached = 0, noBuffers = 0, noDecl = 0, noMaterial = 0;
        std::uint64_t uvAbsent = 0, color1Absent = 0, tangentAbsent = 0, normalAbsent = 0;  // resolved parts whose declaration lacks the attribute
        std::uint64_t layers = 0, skippedMaterials = 0;  // materials classified as terrain layers / never opaque (counted per classification)
        std::uint64_t lightmapParts = 0, lightmapNoResource = 0, lightmapNotLoaded = 0, water = 0;  // parts with a lightmap uv: no image resource / not loaded yet; water left out
        std::uint64_t pendingImages = 0;  // resolved parts with an image not completely loaded yet
        std::uint64_t diffuseNoImage = 0, diffuseNoApi = 0, diffuseNoTexture = 0;  // materials whose diffuse could not be had (no image / not loaded / not ours)
    };
    const Stats& stats() const { return stats_; }
    // bumped when a cached pointer of a resolved part may have died (a buffer changed, a texture was released): caches of NativePart copies rebuild
    std::uint64_t generation() const { return generation_; }
    // eCResource2 state == 3 (data fully loaded)
    static bool imageLoaded(const void* imageResource);

    // DrawObserver: no draws are observed, only the lifetime callbacks
    bool observes(d3d9::draws::Kind, d3d9::scene::Phase) const override { return false; }
    void observe(d3d9::Device&, const d3d9::ReplaceQuery&) override {}
    void bufferChanged(std::uint64_t bufferId) override;
    void textureReleased(const d3d9::Texture* texture) override;

private:
    struct MaterialEntry {
        std::uint64_t frame = 0;
        bool ok = false;
        bool layer = false;  // a terrain layer (blend mode 2, diffuse sampler, opacity from the vertex colour)
        bool masked = false;
        float alphaRef = 0.0f;
        std::uint32_t uvSet = 0;
        d3d9::Texture* diffuse = nullptr;
        std::uint32_t normalUvSet = 0;
        d3d9::Texture* normal = nullptr;
        const void* diffuseImage = nullptr;  // the graph's image resources (kept alive by touch())
        const void* normalImage = nullptr;
        const void* opacityImage = nullptr;
        d3d9::Texture* opacity = nullptr;
        std::uint32_t opacityChannel = 3;
        std::uint32_t opacitySwizzle = 0;
        bool opacityOpaque = false;
        std::string name;
    };
    struct Geometry {
        const void* vertexData = nullptr;
        const void* indexData = nullptr;
        const void* material = nullptr;
        std::uint32_t first = 0, count = 0;
        std::uint64_t vbId = 0, ibId = 0;
        std::uint64_t frame = 0;
        bool ok = false;
        NativePart part;  // geometry fields only (the textures are looked up per frame)
    };
    const MaterialEntry& material(const void* material);
    // eCResource2::RequestData(resource, 1) as every engine draw does for its material and images: queues the data when it is not loaded (async), else
    // refreshes the resource's LRU stamp; once per frame per resource
    void touchResource(const void* resource);
    std::unordered_map<const void*, std::uint64_t> touched_;
    int diagLogged_ = 0, layoutLogged_ = 0, declLogged_ = 0, classLogged_ = 0;
    std::uint64_t generation_ = 1;

    d3d9::Device& dev_;
    DeclResolver resolveDecl_;
    std::unordered_map<const void*, MaterialEntry> materials_;  // by eCMaterialResource2*
    std::unordered_map<const void*, Geometry> geometry_;        // by eCSubMesh*
    Stats stats_;
};

}  // namespace rvk::enh
