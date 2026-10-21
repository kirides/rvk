// PipelineCache with VK_EXT_graphics_pipeline_library (docs/research/performance.md 5).
//
// A monolithic vkCreateGraphicsPipelines compiles both shaders: ~13 ms per pipeline on AMD, ~170 pipelines when the world
// first appears (a 2 s stall) and a few more whenever a new material comes into view. With libraries the expensive part
// moves to shader creation (loading time, any thread): a vertex shader gets its pre-rasterization library, a pixel shader
// its fragment-shader library. A draw with a new state combination then only links four libraries (vertex input per
// topology class, the two shader libraries, fragment output per attachment formats) without optimisation; a background
// thread builds the link-time-optimised pipeline from the same libraries and swaps it in.
//
// Used when the device has GPL plus EDS3 blend state and dynamic vertex input: then a pipeline depends only on the shaders,
// the polygon mode, the topology class and the attachment formats, and everything else is dynamic state.
//
// OFF by default ([vulkan] pipeline_libraries): the AMD driver (Adrenalin, RX 7900 XTX, 2026-10) crashes with an access
// violation inside its compiler on 2 of the game's 2561 shaders when they are compiled as fragment-shader libraries
// (corpus ps30_7dd28aa4c2be47a5, ps30_72a979df27ac55e3; monolithic pipelines with the same SPIR-V are fine). Reproduce
// without the game: rvk_vktest --gpl-corpus <bin/rvk/cache/spirv>.

#include <windows.h>

#include <chrono>
#include <cstring>

#include <spdlog/spdlog.h>

#include "vk/context.h"
#include "vk/pipeline.h"

namespace rvk::vk {
namespace {

// the dynamic states of each library part (a library only takes the states of its own part)
std::vector<VkDynamicState> statesOf(const std::vector<VkDynamicState>& all, VkGraphicsPipelineLibraryFlagsEXT part) {
    std::vector<VkDynamicState> s;
    for (VkDynamicState d : all) {
        bool in = false;
        switch (d) {
        case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT:
        case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY:
        case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE:
            in = part == VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
            break;
        case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT:
        case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT:
        case VK_DYNAMIC_STATE_CULL_MODE:
        case VK_DYNAMIC_STATE_FRONT_FACE:
        case VK_DYNAMIC_STATE_DEPTH_BIAS:
        case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE:
            in = part == VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
            break;
        case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE:
        case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE:
        case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP:
        case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE:
        case VK_DYNAMIC_STATE_STENCIL_OP:
        case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
        case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
        case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
            in = part == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
            break;
        case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
        case VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT:
        case VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT:
        case VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT:
            in = part == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
            break;
        default:
            break;
        }
        if (in) {
            s.push_back(d);
        }
    }
    return s;
}

std::uint64_t elapsedNs(std::chrono::steady_clock::time_point t0) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
}

}  // namespace

bool PipelineCache::librariesSupported(const Context& ctx) {
    const auto& f = ctx.features();
    return f.graphicsPipelineLibrary && f.extendedDynamicState3 && f.vertexInputDynamicState;
}

VkPipeline PipelineCache::createLibrary(VkGraphicsPipelineLibraryFlagsEXT part, VkShaderModule module, VkPolygonMode polygonMode,
                                        VkPrimitiveTopology topology, const PipelineDesc* output) {
    const auto all = dynamicStates(*ctx_);
    const auto dyn = statesOf(all, part);
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = static_cast<std::uint32_t>(dyn.size());
    dy.pDynamicStates = dyn.data();

    VkGraphicsPipelineLibraryCreateInfoEXT lib{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
    lib.flags = part;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, &lib};
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
    ci.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
    ci.pDynamicState = &dy;

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[kMaxColorTargets]{};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};

    switch (part) {
    case VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT:
        ia.topology = topology;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        break;
    case VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT:
        stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        stage.module = module;
        stage.pName = "main";
        rs.polygonMode = polygonMode;
        rs.lineWidth = 1.0f;
        ci.stageCount = 1;
        ci.pStages = &stage;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.layout = layout_;
        break;
    case VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT:
        stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stage.module = module;
        stage.pName = "main";
        ci.stageCount = 1;
        ci.pStages = &stage;
        ci.pDepthStencilState = &ds;
        ci.pMultisampleState = &ms;
        ci.layout = layout_;
        break;
    case VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT:
        for (std::uint32_t i = 0; i < output->colorCount; ++i) {
            // blend enable / equation / write mask are dynamic (EDS3)
            cba[i].colorWriteMask = 0xF;
        }
        cb.attachmentCount = output->colorCount;
        cb.pAttachments = cba;
        ri.colorAttachmentCount = output->colorCount;
        ri.pColorAttachmentFormats = output->colorFormats;
        ri.depthAttachmentFormat = output->depthFormat;
        ri.stencilAttachmentFormat = output->stencilFormat;
        ci.pColorBlendState = &cb;
        ci.pMultisampleState = &ms;
        break;
    default:
        return VK_NULL_HANDLE;
    }
    const auto t0 = std::chrono::steady_clock::now();
    VkPipeline p = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateGraphicsPipelines(ctx_->device(), cache_, 1, &ci, nullptr, &p); r != VK_SUCCESS) {
        spdlog::error("vulkan: pipeline library (part {:#x}): {}", static_cast<std::uint32_t>(part), resultString(r));
        return VK_NULL_HANDLE;
    }
    libraries_.fetch_add(1, std::memory_order_relaxed);
    libraryNs_.fetch_add(elapsedNs(t0), std::memory_order_relaxed);
    return p;
}

VkPipeline PipelineCache::shaderLibrary(VkShaderModule module, bool vertex, VkPolygonMode polygonMode) {
    const LibKey key{reinterpret_cast<std::uint64_t>(module), vertex ? static_cast<std::uint32_t>(polygonMode) : 0xFFFFFFFFu};
    {
        const std::lock_guard lock(libMutex_);
        if (auto it = shaderLibs_.find(key); it != shaderLibs_.end()) {
            return it->second;
        }
    }
    // compiled outside the lock (loader threads prepare shaders concurrently); a lost race destroys its duplicate
    VkPipeline p = createLibrary(vertex ? VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT
                                        : VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
                                 module, polygonMode, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, nullptr);
    const std::lock_guard lock(libMutex_);
    auto [it, inserted] = shaderLibs_.emplace(key, p);
    if (!inserted && p != VK_NULL_HANDLE) {
        vkDestroyPipeline(ctx_->device(), p, nullptr);
    }
    return it->second;
}

void PipelineCache::prepareShader(VkShaderModule module, VkShaderStageFlagBits stage) {
    if (useLibraries_ && module != VK_NULL_HANDLE) {
        shaderLibrary(module, stage == VK_SHADER_STAGE_VERTEX_BIT, VK_POLYGON_MODE_FILL);
    }
}

VkPipeline PipelineCache::link(const PipelineDesc& d, bool optimize) {
    VkPipeline libs[4]{};
    {
        // vertex input interface per topology class, fragment output per attachment formats (few of each)
        const std::lock_guard lock(libMutex_);
        VkPipeline& vi = vertexInputLibs_[d.topology];
        if (vi == VK_NULL_HANDLE) {
            vi = createLibrary(VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, VK_NULL_HANDLE, VK_POLYGON_MODE_FILL,
                               d.topology, nullptr);
        }
        libs[0] = vi;
        OutputKey ok{};
        ok.colorCount = d.colorCount;
        std::memcpy(ok.colorFormats, d.colorFormats, sizeof(ok.colorFormats));
        ok.depthFormat = d.depthFormat;
        ok.stencilFormat = d.stencilFormat;
        VkPipeline& out = outputLibs_[ok];
        if (out == VK_NULL_HANDLE) {
            out = createLibrary(VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT, VK_NULL_HANDLE, VK_POLYGON_MODE_FILL,
                                d.topology, &d);
        }
        libs[3] = out;
    }
    libs[1] = shaderLibrary(d.vs, true, d.polygonMode);
    libs[2] = shaderLibrary(d.fs, false, VK_POLYGON_MODE_FILL);
    for (VkPipeline l : libs) {
        if (l == VK_NULL_HANDLE) {
            return VK_NULL_HANDLE;
        }
    }
    VkPipelineLibraryCreateInfoKHR li{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
    li.libraryCount = 4;
    li.pLibraries = libs;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &li};
    ci.flags = optimize ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0;
    ci.layout = layout_;
    VkPipeline p = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateGraphicsPipelines(ctx_->device(), cache_, 1, &ci, nullptr, &p); r != VK_SUCCESS) {
        spdlog::error("vulkan: pipeline link{}: {}", optimize ? " (optimised)" : "", resultString(r));
        return VK_NULL_HANDLE;
    }
    return p;
}

void PipelineCache::optimizer() {
    SetThreadDescription(GetCurrentThread(), L"rvk pipeline optimizer");
    std::unique_lock lock(optMutex_);
    for (;;) {
        optCv_.wait(lock, [&] { return optQuit_ || !optQueue_.empty(); });
        if (optQuit_) {
            return;
        }
        const PipelineDesc d = optQueue_.front();
        optQueue_.pop_front();
        lock.unlock();
        const auto t0 = std::chrono::steady_clock::now();
        VkPipeline p = link(d, true);
        if (p != VK_NULL_HANDLE) {
            optimized_.fetch_add(1, std::memory_order_relaxed);
            optimizeNs_.fetch_add(elapsedNs(t0), std::memory_order_relaxed);
            const std::lock_guard pl(mutex_);
            auto it = pipelines_.find(d);
            if (it != pipelines_.end()) {
                // command buffers may still use the fast-linked pipeline: it is kept until destroy()
                superseded_.push_back(it->second);
                it->second = p;
            } else {
                superseded_.push_back(p);
            }
        }
        lock.lock();
    }
}

void PipelineCache::startLibraries() {
    useLibraries_ = true;
    optQuit_ = false;
    optThread_ = std::thread([this] { optimizer(); });
}

void PipelineCache::stopLibraries() {
    if (optThread_.joinable()) {
        {
            const std::lock_guard lock(optMutex_);
            optQuit_ = true;
            optQueue_.clear();
        }
        optCv_.notify_all();
        optThread_.join();
    }
}

void PipelineCache::destroyLibraries() {
    for (VkPipeline p : superseded_) {
        vkDestroyPipeline(ctx_->device(), p, nullptr);
    }
    superseded_.clear();
    for (auto& [k, p] : shaderLibs_) {
        if (p != VK_NULL_HANDLE) vkDestroyPipeline(ctx_->device(), p, nullptr);
    }
    shaderLibs_.clear();
    for (auto& [k, p] : vertexInputLibs_) {
        if (p != VK_NULL_HANDLE) vkDestroyPipeline(ctx_->device(), p, nullptr);
    }
    vertexInputLibs_.clear();
    for (auto& [k, p] : outputLibs_) {
        if (p != VK_NULL_HANDLE) vkDestroyPipeline(ctx_->device(), p, nullptr);
    }
    outputLibs_.clear();
    useLibraries_ = false;
}

}  // namespace rvk::vk
