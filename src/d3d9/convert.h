#pragma once

// D3D9 state values -> Vulkan.

#include <cstdint>

#include "d3d9/defs.h"
#include "vk/vk.h"

namespace rvk::d3d9 {

VkBlendFactor blendFactor(std::uint32_t d3dBlend, bool alphaChannel);
VkBlendOp blendOp(std::uint32_t d3dBlendOp);
VkCompareOp compareOp(std::uint32_t d3dCmp);
// the same test on reversed depth (LESS <-> GREATER, LESS_OR_EQUAL <-> GREATER_OR_EQUAL; the others unchanged)
VkCompareOp reverseCompareOp(VkCompareOp op);
VkStencilOp stencilOp(std::uint32_t d3dStencilOp);
// D3D9 culls by screen-space winding; with the y-flipped viewport Vulkan's framebuffer winding is the same, so front = CW
VkCullModeFlags cullMode(std::uint32_t d3dCull);
inline constexpr VkFrontFace kFrontFace = VK_FRONT_FACE_CLOCKWISE;
VkPolygonMode polygonMode(std::uint32_t d3dFill);
VkSamplerAddressMode addressMode(std::uint32_t d3dAddress);
VkFilter filter(std::uint32_t d3dFilter);
VkSamplerMipmapMode mipmapMode(std::uint32_t d3dMipFilter);

struct DeclTypeInfo {
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t size = 0;
};
DeclTypeInfo declType(std::uint32_t d3dDeclType);

VkPrimitiveTopology topology(std::uint32_t d3dPrimitiveType);
// vertices (or indices) consumed by `primitiveCount` primitives
std::uint32_t vertexCount(std::uint32_t d3dPrimitiveType, std::uint32_t primitiveCount);

// D3DCOLOR (0xAARRGGBB) -> normalised RGBA
void colorToFloat4(std::uint32_t argb, float out[4]);

}  // namespace rvk::d3d9
