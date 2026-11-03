#pragma once

// Debug names of the D3D9 model's textures for graphics debuggers (the backends attach them to their objects). Pure formatting: no API object.

#include <cstdint>
#include <string>

namespace rvk::rhi::names {

// What names a texture / render target of the D3D9 device model: the role label when the scene tracker knows one ("depth map",
// "shadow mask", "cascade shadow map" + index), the engine resource identity (d3d9/identity.h id), size, D3D format and the
// bindless slot the shaders sample it through (the DrawBlock's tex[] entry).
struct TextureInfo {
    const char* label = nullptr;       // role / purpose, null = none known
    std::uint32_t labelIndex = ~0u;    // numbered purposes (cascade k); ~0 = none
    std::uint64_t id = 0;              // Texture::ref().id
    std::uint32_t width = 0, height = 0, depth = 1, levels = 1;
    const char* format = "";           // D3D format name (rhi::d3dFormatName)
    bool cube = false, volume = false;
    bool renderTarget = false, depthFormat = false;
    bool dynamic = false;              // D3DUSAGE_DYNAMIC: rewritten by the engine (DISCARD locks)
    std::uint32_t slot = 0;            // bindless slot, 0 = none
};
std::string textureName(const TextureInfo& info);

}  // namespace rvk::rhi::names
