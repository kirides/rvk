#include "vk/pipeline.h"

#include <cstring>
#include <fstream>

#include <spdlog/spdlog.h>

#include "vk/context.h"

namespace rvk::vk {

std::size_t PipelineCache::Hash::operator()(const PipelineDesc& d) const {
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto* p = reinterpret_cast<const unsigned char*>(&d);
    for (std::size_t i = 0; i < sizeof(d); ++i) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return static_cast<std::size_t>(h);
}

bool PipelineCache::Eq::operator()(const PipelineDesc& a, const PipelineDesc& b) const {
    return std::memcmp(&a, &b, sizeof(PipelineDesc)) == 0;
}

VkShaderModule createShaderModule(Context& ctx, std::span<const std::uint32_t> spirv) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size_bytes();
    ci.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateShaderModule(ctx.device(), &ci, nullptr, &m); r != VK_SUCCESS) {
        spdlog::error("vulkan: vkCreateShaderModule: {}", resultString(r));
        return VK_NULL_HANDLE;
    }
    return m;
}

std::vector<VkDynamicState> PipelineCache::dynamicStates(const Context& ctx) {
    std::vector<VkDynamicState> s = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
    };
    if (ctx.features().extendedDynamicState3) {
        s.push_back(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT);
        s.push_back(VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT);
        s.push_back(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT);
    }
    if (ctx.features().vertexInputDynamicState) {
        s.push_back(VK_DYNAMIC_STATE_VERTEX_INPUT_EXT);
    }
    return s;
}

bool PipelineCache::create(Context& ctx, VkPipelineLayout layout, const std::filesystem::path& cacheFile) {
    ctx_ = &ctx;
    layout_ = layout;
    file_ = cacheFile;
    std::vector<char> blob;
    if (!file_.empty()) {
        std::ifstream in(file_, std::ios::binary);
        blob.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = blob.size();
    ci.pInitialData = blob.empty() ? nullptr : blob.data();
    if (vkCreatePipelineCache(ctx.device(), &ci, nullptr, &cache_) != VK_SUCCESS) {
        // a stale or foreign blob: start empty
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        if (vkCreatePipelineCache(ctx.device(), &ci, nullptr, &cache_) != VK_SUCCESS) {
            return false;
        }
    }
    spdlog::info("vulkan: pipeline cache {} ({} KiB loaded)", file_.string(), blob.size() / 1024);
    return true;
}

void PipelineCache::save() {
    if (cache_ == VK_NULL_HANDLE || file_.empty()) {
        return;
    }
    std::size_t size = 0;
    vkGetPipelineCacheData(ctx_->device(), cache_, &size, nullptr);
    std::vector<char> blob(size);
    if (vkGetPipelineCacheData(ctx_->device(), cache_, &size, blob.data()) != VK_SUCCESS) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    std::ofstream out(file_, std::ios::binary | std::ios::trunc);
    out.write(blob.data(), static_cast<std::streamsize>(size));
}

void PipelineCache::destroy() {
    if (ctx_ == nullptr) {
        return;
    }
    save();
    for (auto& [d, p] : pipelines_) {
        if (p != VK_NULL_HANDLE) {
            vkDestroyPipeline(ctx_->device(), p, nullptr);
        }
    }
    pipelines_.clear();
    if (cache_ != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(ctx_->device(), cache_, nullptr);
        cache_ = VK_NULL_HANDLE;
    }
    ctx_ = nullptr;
}

std::size_t PipelineCache::size() const {
    const std::lock_guard lock(mutex_);
    return pipelines_.size();
}

VkPipeline PipelineCache::get(const PipelineDesc& desc) {
    const std::lock_guard lock(mutex_);
    if (auto it = pipelines_.find(desc); it != pipelines_.end()) {
        return it->second;
    }
    VkPipeline p = build(desc);
    pipelines_.emplace(desc, p);  // also caches failures (VK_NULL_HANDLE) so they are logged once
    return p;
}

VkPipeline PipelineCache::build(const PipelineDesc& d) {
    const bool eds3 = ctx_->features().extendedDynamicState3;
    const bool dynVertexInput = ctx_->features().vertexInputDynamicState;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, d.vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, d.fs, "main", nullptr};

    VkVertexInputBindingDescription bindings[kMaxVertexBindings]{};
    VkVertexInputAttributeDescription attrs[kMaxVertexAttributes]{};
    for (std::uint32_t i = 0; i < d.bindingCount; ++i) {
        bindings[i] = {i, d.bindings[i].stride, d.bindings[i].perInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
    }
    for (std::uint32_t i = 0; i < d.attributeCount; ++i) {
        attrs[i] = {d.attributes[i].location, d.attributes[i].binding, d.attributes[i].format, d.attributes[i].offset};
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (!dynVertexInput) {
        vi.vertexBindingDescriptionCount = d.bindingCount;
        vi.pVertexBindingDescriptions = bindings;
        vi.vertexAttributeDescriptionCount = d.attributeCount;
        vi.pVertexAttributeDescriptions = attrs;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = d.topology;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = d.polygonMode;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[kMaxColorTargets]{};
    for (std::uint32_t i = 0; i < d.colorCount; ++i) {
        const BlendAttachment& b = d.blend[i];
        cba[i] = {eds3 ? VK_FALSE : static_cast<VkBool32>(b.enable), b.srcColor, b.dstColor, b.colorOp, b.srcAlpha, b.dstAlpha,
                  b.alphaOp, eds3 ? 0xFu : b.writeMask};
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = d.colorCount;
    cb.pAttachments = cba;
    const auto dyn = dynamicStates(*ctx_);
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = static_cast<std::uint32_t>(dyn.size());
    dy.pDynamicStates = dyn.data();
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = d.colorCount;
    ri.pColorAttachmentFormats = d.colorFormats;
    ri.depthAttachmentFormat = d.depthFormat;
    ri.stencilAttachmentFormat = d.stencilFormat;

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
    ci.layout = layout_;
    VkPipeline p = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateGraphicsPipelines(ctx_->device(), cache_, 1, &ci, nullptr, &p); r != VK_SUCCESS) {
        spdlog::error("vulkan: vkCreateGraphicsPipelines: {}", resultString(r));
        return VK_NULL_HANDLE;
    }
    return p;
}

}  // namespace rvk::vk
