#include "vk/passes.h"

#include <spdlog/spdlog.h>

#include "shader/abi.h"
#include "vk/bindless.h"
#include "vk/context.h"
#include "vk/frames.h"

namespace rvk::vk {

namespace {

constexpr VkShaderStageFlags kPassStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

VkShaderModule module(Context& ctx, std::span<const std::uint32_t> spirv) {
    if (spirv.empty()) return VK_NULL_HANDLE;
    return createShaderModule(ctx, spirv);
}

std::uint32_t texelBytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R8G8_UNORM: return 2;
    case VK_FORMAT_R8_UNORM: return 1;
    default: return 4;
    }
}

}  // namespace

// ---- layout ----------------------------------------------------------------------------------------------------------------

bool PassLayout::create(Context& ctx, const Bindless& bindless, std::span<const VkDescriptorSetLayoutBinding> set1) {
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    lci.bindingCount = static_cast<std::uint32_t>(set1.size());
    lci.pBindings = set1.data();
    if (vkCreateDescriptorSetLayout(ctx.device(), &lci, nullptr, &set1_) != VK_SUCCESS) {
        spdlog::error("vulkan: pass descriptor set layout failed");
        return false;
    }
    const VkDescriptorSetLayout sets[2] = {bindless.setLayout(), set1_};
    const VkPushConstantRange pcr{kPassStages, 0, shader::abi::kPushConstantBudget};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 2;
    plci.pSetLayouts = sets;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(ctx.device(), &plci, nullptr, &layout_) != VK_SUCCESS) {
        spdlog::error("vulkan: pass pipeline layout failed");
        destroy(ctx);
        return false;
    }
    return true;
}

void PassLayout::destroy(Context& ctx) {
    if (layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(ctx.device(), layout_, nullptr);
    if (set1_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(ctx.device(), set1_, nullptr);
    layout_ = VK_NULL_HANDLE;
    set1_ = VK_NULL_HANDLE;
}

void PassLayout::bindSet0(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, const Bindless& bindless) const {
    const VkDescriptorSet set = bindless.set();
    vkCmdBindDescriptorSets(cmd, bindPoint, layout_, 0, 1, &set, 0, nullptr);
}

void PassLayout::pushStorageImage(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, std::uint32_t binding, VkImageView view) const {
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &ii;
    vkCmdPushDescriptorSetKHR(cmd, bindPoint, layout_, 1, 1, &w);
}

void PassLayout::pushStorageImages(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, std::span<const VkImageView> views) const {
    VkDescriptorImageInfo ii[8]{};
    VkWriteDescriptorSet w[8]{};
    std::uint32_t n = 0;
    for (std::size_t i = 0; i < views.size() && i < 8; ++i) {
        if (views[i] == VK_NULL_HANDLE) continue;
        ii[n] = {VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_GENERAL};
        w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[n].dstBinding = static_cast<std::uint32_t>(i);
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[n].pImageInfo = &ii[n];
        ++n;
    }
    if (n > 0) vkCmdPushDescriptorSetKHR(cmd, bindPoint, layout_, 1, n, w);
}

void PassLayout::push(VkCommandBuffer cmd, const void* data, std::uint32_t size) const {
    vkCmdPushConstants(cmd, layout_, kPassStages, 0, size, data);
}

// ---- pipelines -------------------------------------------------------------------------------------------------------------

VkPipeline createComputePipeline(Context& ctx, VkPipelineLayout layout, std::span<const std::uint32_t> spirv, const char* name,
                                 const VkSpecializationInfo* spec) {
    const VkShaderModule m = module(ctx, spirv);
    if (m == VK_NULL_HANDLE) {
        spdlog::error("vulkan: compute pass {}: no shader", name != nullptr ? name : "?");
        return VK_NULL_HANDLE;
    }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.stage.pSpecializationInfo = spec;
    ci.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateComputePipelines(ctx.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &p); r != VK_SUCCESS) {
        spdlog::error("vulkan: compute pass {}: {}", name != nullptr ? name : "?", resultString(r));
        p = VK_NULL_HANDLE;
    }
    vkDestroyShaderModule(ctx.device(), m, nullptr);
    if (p != VK_NULL_HANDLE && name != nullptr) ctx.setName(p, VK_OBJECT_TYPE_PIPELINE, name);
    return p;
}

VkPipeline createPassGraphicsPipeline(Context& ctx, VkPipelineLayout layout, const PassGraphicsDesc& desc) {
    const VkShaderModule vs = module(ctx, desc.vs);
    const VkShaderModule fs = module(ctx, desc.fs);
    VkPipeline p = VK_NULL_HANDLE;
    if (vs != VK_NULL_HANDLE && fs != VK_NULL_HANDLE) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = desc.cull;
        rs.frontFace = desc.frontFace;
        rs.lineWidth = 1.0f;
        rs.depthBiasEnable = desc.depthBiasConstant != 0.0f || desc.depthBiasSlope != 0.0f ? VK_TRUE : VK_FALSE;
        rs.depthBiasConstantFactor = desc.depthBiasConstant;
        rs.depthBiasSlopeFactor = desc.depthBiasSlope;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = desc.depthTest ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = desc.depthOp;
        VkPipelineColorBlendAttachmentState ba{};
        ba.blendEnable = desc.blend.enable;
        ba.srcColorBlendFactor = desc.blend.srcColor;
        ba.dstColorBlendFactor = desc.blend.dstColor;
        ba.colorBlendOp = desc.blend.colorOp;
        ba.srcAlphaBlendFactor = desc.blend.srcAlpha;
        ba.dstAlphaBlendFactor = desc.blend.dstAlpha;
        ba.alphaBlendOp = desc.blend.alphaOp;
        ba.colorWriteMask = desc.blend.writeMask;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = desc.color != VK_FORMAT_UNDEFINED ? 1u : 0u;
        cb.pAttachments = &ba;
        const VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = desc.dynamicCullMode ? 3u : 2u;
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = desc.color != VK_FORMAT_UNDEFINED ? 1u : 0u;
        ri.pColorAttachmentFormats = &desc.color;
        ri.depthAttachmentFormat = desc.depth;
        ri.stencilAttachmentFormat = desc.stencil;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = layout;
        if (const VkResult r = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &p); r != VK_SUCCESS) {
            spdlog::error("vulkan: pass pipeline {}: {}", desc.name != nullptr ? desc.name : "?", resultString(r));
            p = VK_NULL_HANDLE;
        }
    } else {
        spdlog::error("vulkan: pass pipeline {}: no shader", desc.name != nullptr ? desc.name : "?");
    }
    if (vs != VK_NULL_HANDLE) vkDestroyShaderModule(ctx.device(), vs, nullptr);
    if (fs != VK_NULL_HANDLE) vkDestroyShaderModule(ctx.device(), fs, nullptr);
    if (p != VK_NULL_HANDLE && desc.name != nullptr) ctx.setName(p, VK_OBJECT_TYPE_PIPELINE, desc.name);
    return p;
}

// ---- transient images ------------------------------------------------------------------------------------------------------

void Transients::init(Context& ctx, Frames& frames, Bindless& bindless) {
    ctx_ = &ctx;
    frames_ = &frames;
    bindless_ = &bindless;
}

void Transients::retire(Entry&& e) {
    if (e.slot != 0) bindless_->releaseTexture(TextureKind::Tex2D, e.slot);  // deferred by the bindless heap itself
    Image img = e.image;
    Context* ctx = ctx_;
    frames_->retire([ctx, img]() mutable { img.destroy(*ctx); });
}

Transients::Entry* Transients::get(std::string_view key, const TransientDesc& desc) {
    if (ctx_ == nullptr) return nullptr;
    auto it = images_.find(std::string(key));
    if (it != images_.end()) {
        if (it->second.desc == desc && it->second.image.valid()) return &it->second;
        retire(std::move(it->second));
        images_.erase(it);
    }
    // the entry first: its name (a node of the map, stable) is the image's debug name
    auto [pos, inserted] = images_.emplace(std::string(key), Entry{});
    Entry& e = pos->second;
    e.desc = desc;
    e.name = pos->first;
    ImageDesc id;
    id.format = desc.format;
    id.extent = {std::max(1u, desc.extent.width), std::max(1u, desc.extent.height), 1};
    id.usage = desc.usage;
    id.name = pos->first.c_str();
    if (!e.image.create(*ctx_, id)) {
        spdlog::error("vulkan: transient image {} ({}x{}) failed", e.name, desc.extent.width, desc.extent.height);
        images_.erase(pos);
        return nullptr;
    }
    if ((desc.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0) e.slot = bindless_->addTexture(TextureKind::Tex2D, e.image.view());
    spdlog::info("vulkan: transient image {} {}x{} format {} (bindless slot {})", e.name, desc.extent.width, desc.extent.height,
                 static_cast<int>(desc.format), e.slot);
    return &e;
}

void Transients::release(std::string_view key) {
    auto it = images_.find(std::string(key));
    if (it == images_.end()) return;
    retire(std::move(it->second));
    images_.erase(it);
}

void Transients::destroy() {
    for (auto& [k, e] : images_) retire(std::move(e));
    images_.clear();
}

std::uint64_t Transients::bytes() const {
    std::uint64_t n = 0;
    for (const auto& [k, e] : images_) n += std::uint64_t{e.desc.extent.width} * e.desc.extent.height * texelBytes(e.desc.format);
    return n;
}

}  // namespace rvk::vk
