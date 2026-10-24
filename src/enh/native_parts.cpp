#include "enh/native_parts.h"

#include <algorithm>

#include "engine/materials.h"
#include "engine/memread.h"
#include "hal/vulkan/buffer_lookup.h"

namespace rvk::enh {

namespace {

namespace mats = engine::materials;

constexpr std::uint64_t kMaterialRefresh = 120;   // frames: the graph's pool image may appear later
constexpr std::uint64_t kGeometryKeep = 600;      // frames a resolved sub-mesh stays cached
constexpr std::uint64_t kGeometryRetry = 60;      // frames before a failed one is tried again (buffers / textures stream in)
constexpr std::uint8_t kUsagePosition = 0, kUsageNormal = 3, kUsageTexcoord = 5, kUsageTangent = 6, kUsageColor = 10;
constexpr std::uint8_t kTypeFloat2 = 1, kTypeFloat3 = 2, kTypeColor = 4;

}  // namespace

NativeLayout nativeLayoutOf(const d3d9::VertexDecl* decl) {
    NativeLayout l;
    if (decl == nullptr) return l;
    for (const d3d9::VertexElement& e : decl->elements()) {
        if (e.stream == d3d9::kDeclEndStream) break;
        if (e.stream != 0) continue;
        if (e.usage == kUsagePosition && e.usageIndex == 0 && e.type == kTypeFloat3 && e.offset == 0) l.positionOk = true;
        else if (e.usage == kUsageNormal && e.usageIndex == 0 && e.type == kTypeFloat3) l.normal = e.offset;
        else if (e.usage == kUsageTangent && e.usageIndex == 0 && e.type == kTypeFloat3) l.tangent = e.offset;
        else if (e.usage == kUsageColor && e.usageIndex == 0 && e.type == kTypeColor) l.color0 = e.offset;
        else if (e.usage == kUsageColor && e.usageIndex == 1 && e.type == kTypeColor) l.color1 = e.offset;
        else if (e.usage == kUsageTexcoord && e.usageIndex < 2 && e.type == kTypeFloat2) l.uv[e.usageIndex] = e.offset;
        else if (e.usage == kUsageTexcoord && e.usageIndex == 6 && e.type == kTypeFloat2) l.lightmap = e.offset;
    }
    return l;
}

NativeParts::NativeParts(d3d9::Device& device, DeclResolver resolver) : dev_(device), resolveDecl_(std::move(resolver)) {
    dev_.addDrawObserver(this);
}

NativeParts::~NativeParts() { dev_.removeDrawObserver(this); }

void NativeParts::bufferChanged(std::uint64_t bufferId) {
    std::erase_if(geometry_, [&](const auto& kv) { return kv.second.vbId == bufferId || kv.second.ibId == bufferId; });
}

void NativeParts::textureReleased(const d3d9::Texture* texture) {
    for (auto& kv : materials_) {
        if (kv.second.diffuse == texture) kv.second = MaterialEntry{};  // looked up again at the next use
    }
}

const NativeParts::MaterialEntry& NativeParts::material(const void* material) {
    const std::uint64_t frame = dev_.frameValue();
    MaterialEntry& m = materials_[material];
    if (m.frame != 0 && frame < m.frame + kMaterialRefresh && !(m.ok && m.diffuse == nullptr && frame >= m.frame + 8)) return m;
    m = MaterialEntry{};
    m.frame = frame;
    mats::ShaderInfo info;
    if (material != nullptr && mats::active(info, material) && (info.cls == mats::ShaderClass::Default || info.cls == mats::ShaderClass::Leafs) &&
        info.blendMode <= 1) {
        m.ok = true;
        m.masked = info.blendMode == 1;
        m.alphaRef = static_cast<float>(info.maskReference);
        m.uvSet = std::min<std::uint32_t>(info.diffuse.uvSet, 1u);
        if (info.diffuse.source == mats::Source::Sampler && info.diffuse.image != nullptr) {
            d3d9::Texture* t = dev_.engineTexture(mats::apiTextureOf(info.diffuse.image));
            if (t != nullptr && !t->isNull() && t->kind() == vk::TextureKind::Tex2D) m.diffuse = t;
        }
    }
    return m;
}

bool NativeParts::resolve(const scene::MeshInfo& mesh, const scene::MeshPart& part, NativePart& out) {
    out = NativePart{};
    const MaterialEntry& m = material(part.material);
    if (!m.ok) {
        ++stats_.noMaterial;
        return false;
    }
    const std::uint64_t frame = dev_.frameValue();
    Geometry& g = geometry_[part.subMesh];
    const bool same = g.frame != 0 && g.vertexData == mesh.vertexData[0] && g.indexData == mesh.indexData && g.material == part.material &&
                      g.first == part.firstIndex && g.count == part.indexCount;
    if (!same || frame >= g.frame + (g.ok ? kGeometryKeep : kGeometryRetry)) {
        g = Geometry{};
        g.frame = frame;
        g.vertexData = mesh.vertexData[0];
        g.indexData = mesh.indexData;
        g.material = part.material;
        g.first = part.firstIndex;
        g.count = part.indexCount;
        // the buffers: eCGfxVertexData / eCGfxIndexData +0x20 = our HAL objects
        const void* vHal = nullptr;
        const void* iHal = nullptr;
        if (mesh.vertexData[0] != nullptr) engine::readAt(mesh.vertexData[0], 0x20, vHal);
        if (mesh.indexData != nullptr) engine::readAt(mesh.indexData, 0x20, iHal);
        hal::vulkan::VertexBufferRef vb;
        d3d9::Buffer* ib = nullptr;
        if (vHal == nullptr || iHal == nullptr || !hal::vulkan::vertexBufferOf(vHal, vb) || !hal::vulkan::indexBufferOf(iHal, ib) || vb.buffer == nullptr ||
            ib == nullptr || vb.stride < 12 || vb.stride % 4 != 0 || part.indexCount == 0 || part.indexCount % 3 != 0 || vb.buffer->ringCopied() ||
            ib->ringCopied()) {
            ++stats_.noBuffers;
            return false;
        }
        d3d9::VertexDecl* decl = mesh.declaration != nullptr ? resolveDecl_(mesh.declaration) : nullptr;
        const NativeLayout layout = nativeLayoutOf(decl);
        if (!layout.positionOk) {
            ++stats_.noDecl;
            return false;
        }
        vb.buffer->gpu(vb.stride);
        ib->gpu(ib->index32() ? 4 : 2);
        const VkDeviceAddress va = vb.buffer->deviceAddress(), ia = ib->deviceAddress();
        if (va == 0 || ia == 0) {
            ++stats_.noBuffers;
            return false;
        }
        g.vbId = vb.buffer->ref().id;
        g.ibId = ib->ref().id;
        g.part.vertices = va;  // the mesh resource's stream 0 starts at the buffer's start (verified: stream offset 0 in every compared draw)
        g.part.indices = ia;
        g.part.stride = vb.stride;
        g.part.index32 = ib->index32() ? 1u : 0u;
        g.part.first = part.firstIndex;
        g.part.indexCount = part.indexCount;
        g.part.layout = layout;
        g.part.material = part.material;
        g.ok = true;
        ++stats_.resolved;
    } else {
        ++stats_.cached;
    }
    if (!g.ok) return false;
    out = g.part;
    out.diffuse = m.diffuse;
    out.diffuseUvSet = m.uvSet;
    out.masked = m.masked;
    out.alphaRef = m.alphaRef;
    return true;
}

}  // namespace rvk::enh
