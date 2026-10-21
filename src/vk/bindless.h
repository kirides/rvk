#pragma once

// The one descriptor set of the renderer (shader/abi.h): bindless arrays of 2D / cube / 3D textures and samplers, updated
// after bind, partially bound. Slot 0 of each texture array is a 1x1 (0,0,0,1) texture (the value of an unbound D3D9
// sampler); sampler 0 is point/clamp. Also owns the pipeline layout shared by every pipeline (set 0 + 24 bytes of push
// constants).

#include <cstdint>
#include <mutex>
#include <vector>

#include "vk/vk.h"

namespace rvk::vk {

class Context;
class Frames;

enum class TextureKind : std::uint8_t { Tex2D = 0, Cube = 1, Volume = 2 };

class Bindless {
public:
    static constexpr std::uint32_t kMax2D = 16384;
    static constexpr std::uint32_t kMaxCube = 1024;
    static constexpr std::uint32_t kMax3D = 256;
    static constexpr std::uint32_t kMaxSamplers = 2048;

    bool create(Context& ctx, Frames& frames);
    void destroy();

    // slot for an image view (thread-safe); 0 = failure
    std::uint32_t addTexture(TextureKind kind, VkImageView view);
    // re-point a slot (e.g. after a texture's image was replaced); thread-safe
    void updateTexture(TextureKind kind, std::uint32_t slot, VkImageView view);
    // frees a slot once the frame being recorded has completed (it may still be referenced by recorded draws)
    void releaseTexture(TextureKind kind, std::uint32_t slot);

    std::uint32_t addSampler(VkSampler sampler);

    VkDescriptorSetLayout setLayout() const { return layout_; }
    VkDescriptorSet set() const { return set_; }
    VkPipelineLayout pipelineLayout() const { return pipelineLayout_; }

    // the default textures need their pixels uploaded and layouts transitioned: recorded into `cmd`
    void recordInit(VkCommandBuffer cmd);

private:
    struct Array {
        std::uint32_t binding = 0;
        std::uint32_t capacity = 0;
        std::uint32_t next = 1;
        std::vector<std::uint32_t> freeList;
    };
    Array& array(TextureKind kind) { return arrays_[static_cast<int>(kind)]; }
    void write(TextureKind kind, std::uint32_t slot, VkImageView view);
    bool createDefaults();

    Context* ctx_ = nullptr;
    Frames* frames_ = nullptr;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    std::mutex mutex_;
    Array arrays_[3];
    std::uint32_t nextSampler_ = 0;

    // defaults
    VkImage defaultImages_[3]{};
    VmaAllocation defaultAllocs_[3]{};
    VkImageView defaultViews_[3]{};
    VkSampler defaultSampler_ = VK_NULL_HANDLE;
    VkBuffer defaultStaging_ = VK_NULL_HANDLE;
    VmaAllocation defaultStagingAlloc_ = VK_NULL_HANDLE;
};

}  // namespace rvk::vk
