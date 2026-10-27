#include "enh/native_parts.h"

#include <algorithm>
#include <format>
#include <string>

#include <spdlog/spdlog.h>

#include "engine/materials.h"
#include "engine/memread.h"
#include "engine/symbols.h"
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

// the device tells about every buffer / texture it releases (the engine's dynamic ones every frame): only ones a resolved part uses matter
void NativeParts::bufferChanged(std::uint64_t bufferId) {
    const std::size_t erased = std::erase_if(geometry_, [&](const auto& kv) { return kv.second.vbId == bufferId || kv.second.ibId == bufferId; });
    if (erased != 0) ++generation_;
}

void NativeParts::textureReleased(const d3d9::Texture* texture) {
    for (auto& kv : materials_) {
        if (kv.second.diffuse == texture || kv.second.normal == texture || kv.second.opacity == texture) {
            kv.second = MaterialEntry{};  // looked up again at the next use
            ++generation_;
        }
    }
}

bool NativeParts::imageLoaded(const void* imageResource) {
    std::int32_t state = 0;
    return imageResource != nullptr && engine::readAt(imageResource, 0x38, state) && state == 3;
}

void NativeParts::touchResource(const void* resource) {
    if (resource == nullptr || engine::sym().resourceRequestData == nullptr) return;
    const std::uint64_t frame = dev_.frameValue();
    std::uint64_t& last = touched_[resource];
    if (last == frame) return;
    last = frame;
    if (touched_.size() > 16384) touched_.clear();
    using RequestFn = int (*)(const void* self, int mode);
    reinterpret_cast<RequestFn>(engine::sym().resourceRequestData)(resource, 1);
}

const NativeParts::MaterialEntry& NativeParts::material(const void* material) {
    const std::uint64_t frame = dev_.frameValue();
    MaterialEntry& m = materials_[material];
    if (m.frame != 0 && frame < m.frame + kMaterialRefresh && !((m.ok || m.layer) && m.diffuse == nullptr && frame >= m.frame + 8)) return m;
    m = MaterialEntry{};
    m.frame = frame;
    mats::ShaderInfo info;
    // water (refracted, a constant diffuse): the engine's own pass, never opaque
    if (material != nullptr && mats::resourceName(material).find("Water") != std::string::npos) {
        ++stats_.water;
        return m;
    }
    // classes: opaque / masked (blend mode 0 / 1: drawn natively), terrain layers (alpha blend with the opacity in the vertex colour and a diffuse texture: blended over
    // each other by the engine, a base surface for the RT scene), everything else (overlays with their own opacity texture, additive blends, other shader classes)
    const bool known = material != nullptr && mats::active(info, material) && (info.cls == mats::ShaderClass::Default || info.cls == mats::ShaderClass::Leafs);
    const bool layer = known && info.blendMode == 2 && info.diffuse.source == mats::Source::Sampler && info.opacity.source == mats::Source::VertexColor;
    if (material != nullptr && !(known && info.blendMode <= 1)) {
        (layer ? stats_.layers : stats_.skippedMaterials)++;
        if (classLogged_ < 24) {
            ++classLogged_;
            spdlog::info("enh: native parts: material '{}' is {}: {}, blend mode {}, diffuse {}, opacity {} (swizzle {}), normal {}", mats::resourceName(material),
                         layer ? "a terrain layer" : "never opaque", known ? mats::name(info.cls) : "unreadable / other class", known ? info.blendMode : 0,
                         known ? mats::name(info.diffuse.source) : "-", known ? mats::name(info.opacity.source) : "-", known ? info.opacity.swizzle : 0, known ? mats::name(info.normal.source) : "-");
        }
    }
    if (known && (info.blendMode <= 1 || layer)) {
        m.ok = info.blendMode <= 1;
        m.layer = layer;
        m.masked = info.blendMode == 1;
        m.alphaRef = static_cast<float>(info.maskReference);
        m.uvSet = std::min<std::uint32_t>(info.diffuse.uvSet, 1u);
        if (info.diffuse.source == mats::Source::Sampler && info.diffuse.image != nullptr) {
            const void* api = mats::apiTextureOf(info.diffuse.image);
            d3d9::Texture* t = dev_.engineTexture(api);
            if (t != nullptr && !t->isNull() && t->kind() == vk::TextureKind::Tex2D) {
                m.diffuse = t;
            } else {
                ++(api == nullptr ? stats_.diffuseNoApi : stats_.diffuseNoTexture);
                if (diagLogged_ < 10) {
                    ++diagLogged_;
                    spdlog::info("enh: native parts: diffuse of material {} '{}' unavailable: image '{}', API texture {}, our texture {}", material,
                                 mats::resourceName(material), mats::resourceName(info.diffuse.image), api, static_cast<const void*>(t));
                }
            }
        } else {
            ++stats_.diffuseNoImage;
            if (diagLogged_ < 10) {
                ++diagLogged_;
                spdlog::info("enh: native parts: diffuse of material {} '{}' has no sampler image (source {}, slot {}, image {})", material, mats::resourceName(material),
                             mats::name(info.diffuse.source), info.diffuse.slot, info.diffuse.image);
            }
        }
        m.normalUvSet = std::min<std::uint32_t>(info.normal.uvSet, 1u);
        m.opacitySwizzle = info.opacity.swizzle;
        if (info.diffuse.source == mats::Source::Sampler) m.diffuseImage = info.diffuse.image;
        if (info.normal.source == mats::Source::Sampler) m.normalImage = info.normal.image;
        // opacity of masked materials: the Opacity tree's sampler (channel by the proxy's swizzle; a sampler other than the diffuse's is a texture of its own), else the diffuse alpha
        if (info.opacity.source == mats::Source::Sampler && info.opacity.slot >= 0) {
            m.opacityChannel = info.opacity.swizzle >= 2 && info.opacity.swizzle <= 5 ? static_cast<std::uint32_t>(info.opacity.swizzle - 2) : 3u;
            if (info.opacity.slot != info.diffuse.slot) {
                m.opacityImage = info.opacity.image;
                d3d9::Texture* t = m.opacityImage != nullptr ? dev_.engineTexture(mats::apiTextureOf(m.opacityImage)) : nullptr;
                if (t != nullptr && !t->isNull() && t->kind() == vk::TextureKind::Tex2D) m.opacity = t;
                else m.opacityOpaque = true;
            }
        } else if (info.opacity.source != mats::Source::None) {
            m.opacityOpaque = true;
        }
        m.name = info.diffuse.slot >= 0 ? mats::poolImageName(info, info.diffuse.slot) : std::string();
        if (m.name.empty()) m.name = mats::resourceName(material);
        if (info.normal.source == mats::Source::Sampler && info.normal.image != nullptr) {
            d3d9::Texture* t = dev_.engineTexture(mats::apiTextureOf(info.normal.image));
            if (t != nullptr && !t->isNull() && t->kind() == vk::TextureKind::Tex2D) m.normal = t;
        }
    }
    return m;
}

bool NativeParts::resolve(const scene::MeshInfo& mesh, const scene::MeshPart& part, NativePart& out, bool touch, Reject* why, bool allowLayers) {
    out = NativePart{};
    if (why != nullptr) *why = Reject::None;
    const MaterialEntry& m = material(part.material);
    if (!m.ok && !(m.layer && allowLayers)) {
        ++stats_.noMaterial;
        if (why != nullptr) *why = m.layer ? Reject::Material : Reject::Skipped;
        return false;
    }
    if (touch) {
        touchResource(part.material);
        touchResource(m.diffuseImage);
        touchResource(m.normalImage);
        touchResource(m.opacityImage);
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
            if (why != nullptr) *why = Reject::Buffers;
            return false;
        }
        d3d9::VertexDecl* decl = mesh.declaration != nullptr ? resolveDecl_(mesh.declaration) : nullptr;
        const NativeLayout layout = nativeLayoutOf(decl);
        if (!layout.positionOk) {
            ++stats_.noDecl;
            if (declLogged_ < 6) {
                ++declLogged_;
                std::string d;
                if (decl != nullptr) {
                    for (const d3d9::VertexElement& e : decl->elements()) {
                        if (e.stream == d3d9::kDeclEndStream) break;
                        d += std::format(" s{}+{} t{} u{}.{}", e.stream, e.offset, e.type, e.usage, e.usageIndex);
                    }
                }
                spdlog::info("enh: native parts: declaration rejected: material '{}', stride {}, {} [{} ]", mats::resourceName(part.material), vb.stride,
                             decl == nullptr ? std::format("no declaration object found for visual +0x50 = {}", mesh.declaration) : std::string("declaration:"), d);
            }
            if (why != nullptr) *why = Reject::Declaration;
            return false;
        }
        vb.buffer->gpu(vb.stride);
        ib->gpu(ib->index32() ? 4 : 2);
        const VkDeviceAddress va = vb.buffer->deviceAddress(), ia = ib->deviceAddress();
        if (va == 0 || ia == 0) {
            ++stats_.noBuffers;
            if (why != nullptr) *why = Reject::Buffers;
            return false;
        }
        g.vbId = vb.buffer->ref().id;
        g.ibId = ib->ref().id;
        g.part.vertexBuffer = vb.buffer;
        g.part.indexBuffer = ib;
        g.part.vbId = g.vbId;
        g.part.ibId = g.ibId;
        g.part.vbGen = static_cast<std::uint32_t>(vb.buffer->ref().generation);
        g.part.ibGen = static_cast<std::uint32_t>(ib->ref().generation);
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
        stats_.uvAbsent += layout.uv[0] == 0xFFFFu ? 1u : 0u;
        stats_.color1Absent += layout.color1 == 0xFFFFu ? 1u : 0u;
        stats_.tangentAbsent += layout.tangent == 0xFFFFu ? 1u : 0u;
        stats_.normalAbsent += layout.normal == 0xFFFFu ? 1u : 0u;
        if ((layout.uv[0] == 0xFFFFu || layout.color1 == 0xFFFFu) && layoutLogged_ < 6) {
            ++layoutLogged_;
            std::string d;
            for (const d3d9::VertexElement& e : decl->elements()) {
                if (e.stream == d3d9::kDeclEndStream) break;
                d += std::format(" s{}+{} t{} u{}.{}", e.stream, e.offset, e.type, e.usage, e.usageIndex);
            }
            spdlog::info("enh: native parts: a part with no uv0 / color1 in its declaration: stride {}, entity-less sub-mesh {}, material '{}', declaration [{} ]", vb.stride,
                         part.subMesh, mats::resourceName(part.material), d);
        }
    } else {
        ++stats_.cached;
    }
    if (!g.ok) {
        if (why != nullptr) *why = Reject::Buffers;
        return false;
    }
    out = g.part;
    out.diffuse = m.diffuse;
    out.diffuseUvSet = m.uvSet;
    out.normal = m.normal;
    out.normalUvSet = m.normalUvSet;
    out.opacity = m.opacity;
    out.opacityChannel = m.opacityChannel;
    out.opacityOpaque = m.opacityOpaque;
    out.name = &m.name;
    // the lightmaps belong to the mesh resource: its image resources at +0x90 / +0x98 (null until the images are loaded)
    for (int i = 0; i < 2; ++i) {
        const void* image = nullptr;
        if (mesh.meshResource != nullptr && engine::readAt(mesh.meshResource, 0x90 + 8 * static_cast<std::size_t>(i), image) && image != nullptr) {
            d3d9::Texture* t = dev_.engineTexture(mats::apiTextureOf(image));
            if (t != nullptr && !t->isNull() && t->kind() == vk::TextureKind::Tex2D) out.lightmap[i] = t;
            if (touch) touchResource(image);
        }
        if (i == 0 && g.part.layout.lightmap != 0xFFFFu) {
            ++stats_.lightmapParts;
            if (image == nullptr) ++stats_.lightmapNoResource;
            else if (out.lightmap[0] == nullptr) ++stats_.lightmapNotLoaded;
        }
    }
    // every image the part samples must be completely loaded (see NativePart::pending)
    for (const void* image : {m.diffuseImage, m.normalImage, m.opacityImage}) {
        if (image != nullptr && !imageLoaded(image)) out.pending = true;
    }
    for (int i = 0; i < 2; ++i) {
        const void* image = nullptr;
        if (mesh.meshResource != nullptr && engine::readAt(mesh.meshResource, 0x90 + 8 * static_cast<std::size_t>(i), image) && image != nullptr && !imageLoaded(image)) out.pending = true;
    }
    if (out.pending) ++stats_.pendingImages;
    out.masked = m.masked;
    out.layer = m.layer;
    out.opacitySwizzle = m.opacitySwizzle;
    out.alphaRef = m.alphaRef;
    return true;
}

}  // namespace rvk::enh
