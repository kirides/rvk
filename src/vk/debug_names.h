#pragma once

// Debug object names (VK_EXT_debug_utils vkSetDebugUtilsObjectNameEXT) for graphics debuggers: RenderDoc's resource inspector
// shows them, the validation layer prints them in its messages. Names are made at creation / retag only, never per draw, and
// cost nothing without the extension or with `debug.names` off (docs/research/capture-regions.md "Resource names").

#include <cstdint>
#include <string>
#include <string_view>

#include "rhi/names.h"
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

// (rhi/names.h: the pure name formatter of a D3D9 model texture)
using rhi::names::TextureInfo;
using rhi::names::textureName;
void texture(const Context& ctx, const Image& image, const TextureInfo& info);

}  // namespace names
}  // namespace rvk::vk
