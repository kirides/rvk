#pragma once

// Building blocks of injected passes (d3d9/inject.h, docs/research/injection.md 3): the pass pipeline layout, compute and
// fullscreen pipelines from embedded SPIR-V (shader/embedded.h), and transient images. No render graph: a pass records its
// own sync2 barriers through vk::Image::transition (the image's tracked layout decides the source scope).

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vk/image.h"
#include "vk/pipeline.h"
#include "vk/vk.h"

namespace rvk::vk {

class Bindless;
class Context;
class Frames;

// Pipeline layout of pass shaders (shader/abi.h): set 0 = the bindless set, set 1 = a push-descriptor set of the pass's own
// bindings (storage images, buffers; VK_KHR_push_descriptor is part of the floor), 128 bytes of push constants for every
// stage (the pass block begins with abi::PassPushHeader).
class PassLayout {
public:
    bool create(Context& ctx, const Bindless& bindless, std::span<const VkDescriptorSetLayoutBinding> set1);
    void destroy(Context& ctx);
    VkPipelineLayout layout() const { return layout_; }
    VkDescriptorSetLayout set1() const { return set1_; }
    bool valid() const { return layout_ != VK_NULL_HANDLE; }

    // binds the bindless set at `bindPoint` (set 0)
    void bindSet0(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, const Bindless& bindless) const;
    // vkCmdPushDescriptorSetKHR of set 1: storage image `view` (GENERAL layout) at `binding`
    void pushStorageImage(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, std::uint32_t binding, VkImageView view) const;
    // one push of storage images (GENERAL layout) at bindings 0 .. views.size() - 1 (a null view is left out)
    void pushStorageImages(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, std::span<const VkImageView> views) const;
    // vkCmdPushConstants over all stages
    void push(VkCommandBuffer cmd, const void* data, std::uint32_t size) const;

private:
    VkDescriptorSetLayout set1_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
};

// a compute pipeline from SPIR-V (null on failure, logged)
VkPipeline createComputePipeline(Context& ctx, VkPipelineLayout layout, std::span<const std::uint32_t> spirv, const char* name,
                                 const VkSpecializationInfo* spec = nullptr);

// a graphics pipeline for a pass without vertex input (fullscreen triangles: vkCmdDraw(3)): no depth, no culling, dynamic
// viewport + scissor, one colour attachment of `color` with `blend`
struct PassGraphicsDesc {
    std::span<const std::uint32_t> vs;
    std::span<const std::uint32_t> fs;
    VkFormat color = VK_FORMAT_UNDEFINED;
    BlendAttachment blend{};
    const char* name = nullptr;
    // optional depth (+ stencil) attachment of a geometry pass (enh/native_opaque); none: no depth state
    VkFormat depth = VK_FORMAT_UNDEFINED;
    VkFormat stencil = VK_FORMAT_UNDEFINED;
    bool depthTest = false;
    bool depthWrite = false;
    VkCompareOp depthOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool dynamicCullMode = false;  // the cull mode is set per draw (vkCmdSetCullMode), `cull` is the pipeline's default
};
VkPipeline createPassGraphicsPipeline(Context& ctx, VkPipelineLayout layout, const PassGraphicsDesc& desc);

// Transient images of passes, by key: created on first use, recreated when the description changes, destroyed (after the
// GPU is done: Frames::retire) by release / destroy. Contents are undefined at the start of each frame's use (they are
// discarded when recreated; a pass that keeps history across frames keeps the same key and description). Each has a default
// view and a bindless 2D slot for sampling. Render thread.
struct TransientDesc {
    VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkExtent2D extent{1, 1};
    VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    bool operator==(const TransientDesc& o) const {
        return format == o.format && extent.width == o.extent.width && extent.height == o.extent.height && usage == o.usage;
    }
};

class Transients {
public:
    struct Entry {
        Image image;
        TransientDesc desc;
        std::uint32_t slot = 0;  // bindless t2D slot (0 without SAMPLED usage)
        std::string name;
    };
    void init(Context& ctx, Frames& frames, Bindless& bindless);
    // the image of `key` with `desc` (null on failure)
    Entry* get(std::string_view key, const TransientDesc& desc);
    void release(std::string_view key);
    void destroy();  // all (deferred through Frames::retire)
    std::size_t count() const { return images_.size(); }
    std::uint64_t bytes() const;

private:
    void retire(Entry&& e);
    Context* ctx_ = nullptr;
    Frames* frames_ = nullptr;
    Bindless* bindless_ = nullptr;
    std::unordered_map<std::string, Entry> images_;
};

}  // namespace rvk::vk
