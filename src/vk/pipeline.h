#pragma once

// Graphics pipelines for dynamic rendering. A PipelineDesc holds only what Vulkan 1.3 cannot set dynamically (shaders, vertex
// input, attachment formats, blend state and polygon mode unless VK_EXT_extended_dynamic_state3 is enabled); everything else
// (viewport, scissor, cull mode, front face, topology, depth/stencil state, depth bias, blend constants) is dynamic state the
// renderer sets per draw. Pipelines are cached by the byte image of the description.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rhi/game_device.h"
#include "vk/vk.h"

namespace rvk::vk {

class Context;

// rhi::ShaderModule <-> VkShaderModule (the Vulkan game device creates the modules; the handle value is the VkShaderModule)
inline VkShaderModule vkModule(rhi::ShaderModule m) { return reinterpret_cast<VkShaderModule>(static_cast<std::uint64_t>(m)); }
inline rhi::ShaderModule rhiModule(VkShaderModule m) { return static_cast<rhi::ShaderModule>(reinterpret_cast<std::uint64_t>(m)); }

// pipeline descriptions + vertex input / blend vocabulary live in rhi/game_device.h (the seam's vocabulary); the Vulkan pipeline
// code keeps the vk names
using rhi::BlendAttachment;
using rhi::VertexBinding;
using rhi::VertexAttribute;
using rhi::kMaxColorTargets;
using rhi::kMaxVertexBindings;
using rhi::kMaxVertexAttributes;
using rhi::PipelineDesc;

class PipelineCache {
public:
    // libraries: use VK_EXT_graphics_pipeline_library when librariesSupported (pipeline_library.cpp)
    bool create(Context& ctx, VkPipelineLayout layout, const std::filesystem::path& cacheFile, bool libraries = false);
    void destroy();
    static bool librariesSupported(const Context& ctx);
    bool usesLibraries() const { return useLibraries_; }

    // thread-safe; VK_NULL_HANDLE on failure (logged once per description). With libraries a new description is linked
    // from precompiled libraries (fast, unoptimised) and replaced by its link-time-optimised build in the background.
    VkPipeline get(const PipelineDesc& desc, bool* created = nullptr);  // created: this call made a new pipeline
    // builds `desc` on a background thread unless it exists: pipelines known from earlier runs, before their first draw
    // (urgent: a draw waits for it; served before the others)
    void prewarm(const PipelineDesc& desc, bool urgent = false);
    // the same for draw-time requests that repeat every frame until the pipeline exists (shader variants of enhancements): a
    // description is queued once
    void prewarmOnce(const PipelineDesc& desc);
    // with libraries: compiles the shader's library now (pre-rasterization or fragment shader state); thread-safe
    void prepareShader(rhi::ShaderModule module, VkShaderStageFlagBits stage);
    // writes the VkPipelineCache blob (at shutdown)
    void save();
    // the same while running, when pipelines were built since the last save: the blob is read on the calling thread, the
    // file written on a background thread (<file>.tmp, then renamed)
    void saveAsync();
    std::size_t size() const;
    // statistics: pipelines built (vkCreateGraphicsPipelines calls) and the time spent in them, cumulative
    std::uint64_t built() const { return built_.load(std::memory_order_relaxed); }
    double buildMs() const { return static_cast<double>(buildNs_.load(std::memory_order_relaxed)) * 1e-6; }
    // libraries compiled (mostly at shader creation) and optimised pipelines built in the background, cumulative
    std::uint64_t libraries() const { return libraries_.load(std::memory_order_relaxed); }
    double libraryMs() const { return static_cast<double>(libraryNs_.load(std::memory_order_relaxed)) * 1e-6; }
    std::uint64_t optimized() const { return optimized_.load(std::memory_order_relaxed); }
    std::uint64_t prewarmed() const { return prewarmed_.load(std::memory_order_relaxed); }
    double prewarmMs() const { return static_cast<double>(prewarmNs_.load(std::memory_order_relaxed)) * 1e-6; }
    double optimizeMs() const { return static_cast<double>(optimizeNs_.load(std::memory_order_relaxed)) * 1e-6; }

    static std::vector<VkDynamicState> dynamicStates(const Context& ctx);
    // whether a pipeline of `desc` exists (built or prewarmed); any thread
    bool contains(const PipelineDesc& desc) const {
        const std::lock_guard lock(mutex_);
        return pipelines_.contains(desc);
    }
    // the pipeline of `desc` when it exists, else VK_NULL_HANDLE (never builds); any thread
    VkPipeline find(const PipelineDesc& desc) const {
        const std::lock_guard lock(mutex_);
        const auto it = pipelines_.find(desc);
        return it != pipelines_.end() ? it->second : VK_NULL_HANDLE;
    }

private:
    VkPipeline build(const PipelineDesc& desc);
    void prewarmWorker();
    void stopPrewarm();
    std::vector<std::thread> prewarmThreads_;
    std::mutex prewarmMutex_;
    std::condition_variable prewarmCv_;
    std::deque<PipelineDesc> prewarmQueue_;
    std::deque<PipelineDesc> prewarmUrgent_;  // prewarmOnce: draw-time requests
    bool prewarmQuit_ = false;
    struct HashOf {
        std::size_t operator()(const PipelineDesc& d) const;
    };
    struct EqOf {
        bool operator()(const PipelineDesc& a, const PipelineDesc& b) const;
    };
    std::unordered_set<PipelineDesc, HashOf, EqOf> prewarmRequested_;  // prewarmOnce (under prewarmMutex_)
    std::atomic<std::uint64_t> prewarmed_{0};
    std::atomic<std::uint64_t> prewarmNs_{0};

    // pipeline libraries (pipeline_library.cpp)
    struct LibKey {
        std::uint64_t module;
        std::uint32_t polygonMode;  // 0xFFFFFFFF for fragment shaders
        auto operator<=>(const LibKey&) const = default;
    };
    struct OutputKey {
        std::uint32_t colorCount;
        VkFormat colorFormats[kMaxColorTargets];
        VkFormat depthFormat;
        VkFormat stencilFormat;
        auto operator<=>(const OutputKey&) const = default;
    };
    VkPipeline createLibrary(VkGraphicsPipelineLibraryFlagsEXT part, VkShaderModule module, VkPolygonMode polygonMode,
                             VkPrimitiveTopology topology, const PipelineDesc* output);
    VkPipeline shaderLibrary(VkShaderModule module, bool vertex, VkPolygonMode polygonMode);
    VkPipeline link(const PipelineDesc& d, bool optimize);
    void optimizer();
    void startLibraries();
    void stopLibraries();
    void destroyLibraries();
    bool useLibraries_ = false;
    std::mutex libMutex_;
    std::map<LibKey, VkPipeline> shaderLibs_;
    std::map<VkPrimitiveTopology, VkPipeline> vertexInputLibs_;
    std::map<OutputKey, VkPipeline> outputLibs_;
    std::vector<VkPipeline> superseded_;  // fast-linked pipelines replaced by optimised ones (under mutex_)
    std::thread optThread_;
    std::mutex optMutex_;
    std::condition_variable optCv_;
    std::deque<PipelineDesc> optQueue_;
    bool optQuit_ = false;
    std::atomic<std::uint64_t> libraries_{0};
    std::atomic<std::uint64_t> libraryNs_{0};
    std::atomic<std::uint64_t> optimized_{0};
    std::atomic<std::uint64_t> optimizeNs_{0};

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
    std::unordered_set<PipelineDesc, Hash, Eq> building_;  // descriptions being compiled outside mutex_ (under mutex_)
    std::condition_variable builtCv_;                      // a description left building_
    std::mutex dataMutex_;                                 // data(): savedBuilt_ and one serialisation at a time
    std::atomic<std::uint64_t> built_{0};
    std::atomic<std::uint64_t> buildNs_{0};
    std::uint64_t savedBuilt_ = 0;  // built_ at the last save
    std::thread saver_;
    std::vector<char> data();
};

// `name` (when not empty and names are on): the module's debug name; pipelines built from it are named after it
VkShaderModule createShaderModule(Context& ctx, std::span<const std::uint32_t> spirv, std::string_view name = {});

}  // namespace rvk::vk
