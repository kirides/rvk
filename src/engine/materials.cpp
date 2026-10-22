#include "engine/materials.h"

#include <unordered_map>

#include "engine/memread.h"
#include "engine/symbols.h"

namespace rvk::engine::materials {
namespace {

// eCShaderDefault proxies (rt-albedo-facts 1.1): 0x30 bytes each, the element pointer at +8, swizzle +0x28
constexpr std::size_t kDiffuseProxy = 0x2e0, kOpacityProxy = 0x310, kNormalProxy = 0x3d0;
constexpr std::size_t kProxyElement = 8, kProxySwizzle = 0x28;
constexpr std::size_t kBlendMode = 0x50, kMaskReference = 0x58;
constexpr std::size_t kSamplerBase = 0x219, kSamplerCount = 0x21b;
constexpr std::size_t kPoolImage = 0xf8 + 0x20, kPoolStride = 0x28;
// eCColorSrcSampler
constexpr std::size_t kSamplerSlot = 0x98, kSamplerTexCoordSource = 0xa8, kSamplerUvSet = 0xc8;

struct Walk {
    Source source = Source::None;
    const void* sampler = nullptr;  // the first eCColorSrcSampler
    bool cube = false;
};

const void* vtableOf(const void* object) {
    const void* v = nullptr;
    return readAt(object, 0, v) ? v : nullptr;
}

// depth first, in the order the engine hands out slots (combiner +0x58 / +0x88, blend +0x48 / +0x78 / +0xa8, mask +0x50)
void walk(const void* element, int depth, Walk& w) {
    if (element == nullptr || depth > 8 || w.sampler != nullptr) return;
    const auto& s = sym();
    const void* vt = vtableOf(element);
    if (vt == nullptr) return;
    const auto child = [&](std::size_t proxy) {
        const void* e = nullptr;
        if (readAt(element, proxy + kProxyElement, e)) walk(e, depth + 1, w);
    };
    if (vt == s.colorSrcSamplerVftable) {
        w.sampler = element;
        w.source = Source::Sampler;
    } else if (vt == s.colorSrcCombinerVftable) {
        child(0x58);
        child(0x88);
    } else if (vt == s.colorSrcBlendVftable) {
        child(0x48);
        child(0x78);
        child(0xa8);
    } else if (vt == s.colorSrcMaskVftable) {
        child(0x50);
    } else if (vt == s.colorSrcCubeSamplerVftable) {
        w.cube = true;
    } else if (w.source == Source::None) {
        w.source = vt == s.colorSrcConstantVftable ? Source::Constant : vt == s.colorSrcVertexColorVftable ? Source::VertexColor : Source::Other;
    }
}

struct Cached {
    const void* vtable = nullptr;
    const void* elements[3]{};  // diffuse, opacity, normal role elements at the walk
    const void* samplers[3]{};  // their first samplers
    ShaderInfo info;
};
std::unordered_map<const void*, Cached> g_cache;

void fillRole(const void* shader, std::size_t proxy, const void* element, const void* sampler, Role& r) {
    std::uint8_t swz = 0;
    if (readAt(shader, proxy + kProxySwizzle, swz)) r.swizzle = swz;
    if (sampler == nullptr) return;
    std::int32_t slot = -1;
    std::uint32_t uvSet = 0;
    const void* source = nullptr;
    readAt(sampler, kSamplerSlot, slot);
    readAt(sampler, kSamplerUvSet, uvSet);
    readAt(sampler, kSamplerTexCoordSource, source);
    r.slot = slot >= 0 && slot < 16 ? static_cast<std::int8_t>(slot) : -1;
    r.uvSet = static_cast<std::uint8_t>(uvSet & 3);
    r.uvSource = source != nullptr;
    (void)element;
}

}  // namespace

bool available() {
    const auto& s = sym();
    return s.shaderBaseActiveShader != nullptr && s.shaderDefaultVftable != nullptr && s.colorSrcSamplerVftable != nullptr;
}

bool active(ShaderInfo& out, const void* material) {
    if (!available()) return false;
    const auto& s = sym();
    const void* shader = material != nullptr ? shaderOfMaterial(material) : nullptr;
    if (shader == nullptr && (!safeRead(s.shaderBaseActiveShader, &shader, sizeof(shader)) || shader == nullptr)) return false;
    const void* vt = vtableOf(shader);
    if (vt == nullptr) return false;
    const ShaderClass cls = vt == s.shaderDefaultVftable ? ShaderClass::Default
                            : (s.shaderLeafsVftable != nullptr && vt == s.shaderLeafsVftable) ? ShaderClass::Leafs
                                                                                                : ShaderClass::Other;
    Cached& c = g_cache[shader];
    if (cls == ShaderClass::Other) {
        c = {};
        c.vtable = vt;
        c.info.shader = shader;
        c.info.cls = cls;
        out = c.info;
        return true;
    }
    const void* elements[3]{};
    readAt(shader, kDiffuseProxy + kProxyElement, elements[0]);
    readAt(shader, kOpacityProxy + kProxyElement, elements[1]);
    readAt(shader, kNormalProxy + kProxyElement, elements[2]);
    if (c.vtable != vt || c.elements[0] != elements[0] || c.elements[1] != elements[1] || c.elements[2] != elements[2]) {
        // a new shader object at this address, or its graph changed: walk again
        c = {};
        c.vtable = vt;
        Role* roles[3] = {&c.info.diffuse, &c.info.opacity, &c.info.normal};
        for (int i = 0; i < 3; ++i) {
            c.elements[i] = elements[i];
            Walk w;
            walk(elements[i], 0, w);
            c.samplers[i] = w.sampler;
            roles[i]->source = w.sampler != nullptr ? Source::Sampler : w.cube ? Source::Cube : w.source;
        }
    }
    ShaderInfo& info = c.info;
    info.shader = shader;
    info.cls = cls;
    std::uint8_t bytes[3]{};
    readAt(shader, kBlendMode, info.blendMode);
    readAt(shader, kMaskReference, info.maskReference);
    if (safeRead(static_cast<const std::uint8_t*>(shader) + kSamplerBase, bytes, 3)) {
        info.samplerBase = bytes[0];
        info.samplerCount = bytes[2];
    }
    // the slots are handed out by UpdateShaderConstants: read them every time
    const std::size_t proxies[3] = {kDiffuseProxy, kOpacityProxy, kNormalProxy};
    Role* roles[3] = {&info.diffuse, &info.opacity, &info.normal};
    for (int i = 0; i < 3; ++i) {
        fillRole(shader, proxies[i], c.elements[i], c.samplers[i], *roles[i]);
        roles[i]->image = nullptr;
        if (roles[i]->slot >= 0) {
            readAt(shader, kPoolImage + kPoolStride * static_cast<std::size_t>(roles[i]->slot), roles[i]->image);
        }
    }
    out = info;
    return true;
}

const void* apiTextureOf(const void* imageResource) {
    const void* data = nullptr;
    const void* api = nullptr;
    if (!readAt(imageResource, 0x70, data) || data == nullptr || !readAt(data, 0x60, api)) return nullptr;
    return api;
}

const void* shaderOfMaterial(const void* material) {
    const void* shader = nullptr;
    return readAt(material, 0x70, shader) ? shader : nullptr;
}

std::string resourceName(const void* resource) {
    if (resource == nullptr) return {};
    return readCString(static_cast<const std::uint8_t*>(resource) + 0x40);
}

std::string poolImageName(const ShaderInfo& info, int slot) {
    if (info.shader == nullptr || slot < 0 || slot >= 7) return {};
    const void* image = nullptr;
    if (!readAt(info.shader, kPoolImage + kPoolStride * static_cast<std::size_t>(slot), image)) return {};
    return resourceName(image);
}

const char* name(Source s) {
    switch (s) {
    case Source::None: return "none";
    case Source::Sampler: return "sampler";
    case Source::Cube: return "cube";
    case Source::Constant: return "constant";
    case Source::VertexColor: return "vertex colour";
    default: return "other";
    }
}

const char* name(ShaderClass c) {
    switch (c) {
    case ShaderClass::Default: return "Default";
    case ShaderClass::Leafs: return "Leafs";
    case ShaderClass::Other: return "other";
    default: return "?";
    }
}

}  // namespace rvk::engine::materials
