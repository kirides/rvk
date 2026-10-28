#pragma once

// Debug object names (VK_EXT_debug_utils vkSetDebugUtilsObjectNameEXT) for graphics debuggers: RenderDoc's resource inspector
// shows them, the validation layer prints them in its messages. Names are made at creation / retag only, never per draw, and
// cost nothing without the extension or with `debug.names` off (docs/research/capture-regions.md "Resource names").

#include <cstdint>
#include <string>
#include <string_view>

#include "vk/vk.h"

namespace rvk::vk {

class Context;
class Image;

namespace names {

// `debug.names` (restart setting, default on): set before the device is created; the Context gates every name on it
void setEnabled(bool on);
bool enabled();

// one object (any thread); no-op when names are off
void object(const Context& ctx, VkObjectType type, std::uint64_t handle, std::string_view name);
template <class H>
void object(const Context& ctx, H handle, VkObjectType type, std::string_view name) {
    object(ctx, type, reinterpret_cast<std::uint64_t>(handle), name);
}

// the image and its default view ("<name>" / "<name> (view)")
void image(const Context& ctx, const Image& image, std::string_view name);

// a shader module; the name is remembered so pipelines built from it can be named after their shaders
void shaderModule(const Context& ctx, VkShaderModule module, std::string_view name);
// a graphics pipeline: "pipe <vs name> + <fs name>"
void pipeline(const Context& ctx, VkPipeline pipeline, VkShaderModule vs, VkShaderModule fs);

// What names a texture / render target of the D3D9 device model: the role label when the scene tracker knows one ("depth map",
// "shadow mask", "cascade shadow map" + index), the engine resource identity (d3d9/identity.h id), size, D3D format and the
// bindless slot the shaders sample it through (the DrawBlock's tex[] entry).
struct TextureInfo {
    const char* label = nullptr;       // role / purpose, null = none known
    std::uint32_t labelIndex = ~0u;    // numbered purposes (cascade k); ~0 = none
    std::uint64_t id = 0;              // Texture::ref().id
    std::uint32_t width = 0, height = 0, depth = 1, levels = 1;
    const char* format = "";           // D3D format name (vk::d3dFormatName)
    bool cube = false, volume = false;
    bool renderTarget = false, depthFormat = false;
    bool dynamic = false;              // D3DUSAGE_DYNAMIC: rewritten by the engine (DISCARD locks)
    std::uint32_t slot = 0;            // bindless slot, 0 = none
};
std::string textureName(const TextureInfo& info);
void texture(const Context& ctx, const Image& image, const TextureInfo& info);

}  // namespace names
}  // namespace rvk::vk
