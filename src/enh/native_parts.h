#pragma once

// Mirror parts as GPU-ready draw data (docs/research/native-renderer.md N1 / N4): an entity's mesh context part (scene::MeshPart of a
// scene::MeshInfo) resolved to its buffers' device addresses, its vertex layout (from the engine's declaration handle = our
// d3d9::VertexDecl) and its material (the engine's material graph: diffuse texture, blend mode, mask reference), without any draw. Used by the
// native shadow cascades and (next) by the native opaque pass. The geometry resolution is cached per sub-mesh (validated by the identity of the
// resource's buffers, dropped when the device says a buffer changed); the textures are cached per material and dropped when the device releases
// one (this class is a DrawObserver that observes no draws). Render thread.

#include <cstdint>
#include <functional>
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
    bool masked = false;                        // blend mode masked: alpha tested against alphaRef
    float alphaRef = 0.0f;                      // 0..255
    const void* material = nullptr;
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
    bool resolve(const scene::MeshInfo& mesh, const scene::MeshPart& part, NativePart& out);

    struct Stats {
        std::uint64_t resolved = 0, cached = 0, noBuffers = 0, noDecl = 0, noMaterial = 0;
    };
    const Stats& stats() const { return stats_; }

    // DrawObserver: no draws are observed, only the lifetime callbacks
    bool observes(d3d9::draws::Kind, d3d9::scene::Phase) const override { return false; }
    void observe(d3d9::Device&, const d3d9::ReplaceQuery&) override {}
    void bufferChanged(std::uint64_t bufferId) override;
    void textureReleased(const d3d9::Texture* texture) override;

private:
    struct MaterialEntry {
        std::uint64_t frame = 0;
        bool ok = false;
        bool masked = false;
        float alphaRef = 0.0f;
        std::uint32_t uvSet = 0;
        d3d9::Texture* diffuse = nullptr;
    };
    struct Geometry {
        const void* vertexData = nullptr;
        const void* indexData = nullptr;
        const void* material = nullptr;
        std::uint32_t first = 0, count = 0;
        std::uint64_t vbId = 0, ibId = 0;
        std::uint64_t frame = 0;
        bool ok = false;
        NativePart part;  // geometry fields only (the diffuse is looked up per frame)
    };
    const MaterialEntry& material(const void* material);

    d3d9::Device& dev_;
    DeclResolver resolveDecl_;
    std::unordered_map<const void*, MaterialEntry> materials_;  // by eCMaterialResource2*
    std::unordered_map<const void*, Geometry> geometry_;        // by eCSubMesh*
    Stats stats_;
};

}  // namespace rvk::enh
