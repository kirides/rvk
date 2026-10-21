#pragma once

// Graphics pipelines for dynamic rendering. A PipelineDesc holds only what Vulkan 1.3 cannot set dynamically (shaders, vertex
// input, attachment formats, blend state and polygon mode unless VK_EXT_extended_dynamic_state3 is enabled); everything else
// (viewport, scissor, cull mode, front face, topology, depth/stencil state, depth bias, blend constants) is dynamic state the
// renderer sets per draw. Pipelines are cached by the byte image of the description.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "vk/vk.h"

namespace rvk::vk {

class Context;

struct BlendAttachment {
    std::uint32_t enable = 0;
    VkBlendFactor srcColor = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dstColor = VK_BLEND_FACTOR_ZERO;
    VkBlendOp colorOp = VK_BLEND_OP_ADD;
    VkBlendFactor srcAlpha = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dstAlpha = VK_BLEND_FACTOR_ZERO;
    VkBlendOp alphaOp = VK_BLEND_OP_ADD;
    VkColorComponentFlags writeMask = 0xF;
};

struct VertexBinding {
    std::uint32_t stride = 0;
    std::uint32_t perInstance = 0;  // VK_VERTEX_INPUT_RATE_INSTANCE
};

struct VertexAttribute {
    std::uint32_t location = 0;
    std::uint32_t binding = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t offset = 0;
};

inline constexpr std::uint32_t kMaxColorTargets = 4;
inline constexpr std::uint32_t kMaxVertexBindings = 8;
inline constexpr std::uint32_t kMaxVertexAttributes = 16;

// Plain data, compared and hashed bytewise: always value-initialise (PipelineDesc d{}) so padding is zero.
struct PipelineDesc {
    VkShaderModule vs = VK_NULL_HANDLE;
    VkShaderModule fs = VK_NULL_HANDLE;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  // only its class matters (topology is dynamic)
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;                     // ignored with EDS3 polygon mode
    std::uint32_t colorCount = 0;
    VkFormat colorFormats[kMaxColorTargets]{};
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
    BlendAttachment blend[kMaxColorTargets]{};  // ignored with EDS3
    std::uint32_t bindingCount = 0;
    VertexBinding bindings[kMaxVertexBindings]{};
    std::uint32_t attributeCount = 0;
    VertexAttribute attributes[kMaxVertexAttributes]{};
};

class PipelineCache {
public:
    bool create(Context& ctx, VkPipelineLayout layout, const std::filesystem::path& cacheFile);
    void destroy();

    // thread-safe; VK_NULL_HANDLE on failure (logged once per description)
    VkPipeline get(const PipelineDesc& desc);
    // writes the VkPipelineCache blob (called at shutdown and periodically)
    void save();
    std::size_t size() const;

    static std::vector<VkDynamicState> dynamicStates(const Context& ctx);

private:
    VkPipeline build(const PipelineDesc& desc);

    struct Hash {
        std::size_t operator()(const PipelineDesc& d) const;
    };
    struct Eq {
        bool operator()(const PipelineDesc& a, const PipelineDesc& b) const;
    };

    Context* ctx_ = nullptr;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::unordered_map<PipelineDesc, VkPipeline, Hash, Eq> pipelines_;
};

VkShaderModule createShaderModule(Context& ctx, std::span<const std::uint32_t> spirv);

}  // namespace rvk::vk
