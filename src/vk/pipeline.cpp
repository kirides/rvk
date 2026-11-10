#include "vk/pipeline.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

#include <spdlog/spdlog.h>

#include "vk/context.h"
#include "core/profile.h"

namespace rvk::vk {

std::size_t PipelineCache::Hash::operator()(const PipelineDesc& d) const {
    // FNV-1a over 64-bit words (the description is ~520 bytes, hashed per draw that changes it)
    static_assert(sizeof(PipelineDesc) % 4 == 0);
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto* p = reinterpret_cast<const unsigned char*>(&d);
    std::size_t i = 0;
    for (; i + 8 <= sizeof(d); i += 8) {
        std::uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < sizeof(d); ++i) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return static_cast<std::size_t>(h);
}

bool PipelineCache::Eq::operator()(const PipelineDesc& a, const PipelineDesc& b) const {
    return std::memcmp(&a, &b, sizeof(PipelineDesc)) == 0;
}

VkShaderModule createShaderModule(Context& ctx, std::span<const std::uint32_t> spirv, std::string_view name) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size_bytes();
    ci.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateShaderModule(ctx.device(), &ci, nullptr, &m); r != VK_SUCCESS) {
        spdlog::error("vulkan: vkCreateShaderModule: {}", resultString(r));
        return VK_NULL_HANDLE;
    }
    if (!name.empty()) names::shaderModule(ctx, m, name);
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

bool PipelineCache::create(Context& ctx, VkPipelineLayout layout, const std::filesystem::path& cacheFile, bool libraries) {
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
    if (libraries && librariesSupported(ctx)) {
        startLibraries();
        spdlog::info("vulkan: pipelines are linked from graphics pipeline libraries");
    }
    return true;
}

namespace {

// write to <file>.tmp, then replace: a game killed while writing leaves the previous cache intact
bool writeBlob(const std::filesystem::path& file, const std::vector<char>& blob) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        if (!out) {
            return false;
        }
    }
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

}  // namespace

std::vector<char> PipelineCache::data() {
    std::vector<char> blob;
    if (cache_ == VK_NULL_HANDLE) {
        return blob;
    }
    // a VkPipelineCache is internally synchronised: serialising it while workers create pipelines into it is valid (no
    // mutex_, which draws take for every lookup: DXVK keeps lookups lock-free for the same reason)
    const std::lock_guard lock(dataMutex_);
    std::size_t size = 0;
    vkGetPipelineCacheData(ctx_->device(), cache_, &size, nullptr);
    blob.resize(size);
    if (vkGetPipelineCacheData(ctx_->device(), cache_, &size, blob.data()) != VK_SUCCESS) {
        blob.clear();
    }
    blob.resize(std::min(blob.size(), size));
    savedBuilt_ = built_.load(std::memory_order_relaxed);
    return blob;
}

void PipelineCache::save() {
    if (file_.empty()) {
        return;
    }
    if (saver_.joinable()) {
        saver_.join();
    }
    const std::vector<char> blob = data();
    if (!blob.empty()) {
        writeBlob(file_, blob);
    }
}

void PipelineCache::saveAsync() {
    if (file_.empty() || savedBuilt_ == built_.load(std::memory_order_relaxed)) {
        return;
    }
    if (saver_.joinable()) {
        saver_.join();
    }
    std::vector<char> blob = data();
    if (blob.empty()) {
        return;
    }
    // the game is often ended by killing it: the cache is written while it runs, the file I/O off the render thread
    saver_ = std::thread([file = file_, blob = std::move(blob)] {
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = writeBlob(file, blob);
        spdlog::debug("vulkan: pipeline cache saved ({} KiB{}, {:.1f} ms)", blob.size() / 1024, ok ? "" : ", FAILED",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    });
}

void PipelineCache::destroy() {
    if (ctx_ == nullptr) {
        return;
    }
    stopPrewarm();
    stopLibraries();
    save();
    for (auto& [d, p] : pipelines_) {
        if (p != VK_NULL_HANDLE) {
            vkDestroyPipeline(ctx_->device(), p, nullptr);
        }
    }
    pipelines_.clear();
    destroyLibraries();
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

void PipelineCache::prewarm(const PipelineDesc& desc, bool urgent) {
    {
        const std::lock_guard lock(prewarmMutex_);
        if (prewarmThreads_.empty()) {
            // a few threads: the driver compiles in parallel; the render thread keeps its core
            const unsigned n = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
            for (unsigned i = 0; i < n; ++i) {
                prewarmThreads_.emplace_back([this] { prewarmWorker(); });
            }
        }
        (urgent ? prewarmUrgent_ : prewarmQueue_).push_back(desc);
    }
    prewarmCv_.notify_one();
}

std::size_t PipelineCache::HashOf::operator()(const PipelineDesc& d) const { return Hash{}(d); }
bool PipelineCache::EqOf::operator()(const PipelineDesc& a, const PipelineDesc& b) const { return Eq{}(a, b); }

void PipelineCache::prewarmOnce(const PipelineDesc& desc) {
    {
        const std::lock_guard lock(prewarmMutex_);
        if (!prewarmRequested_.insert(desc).second) return;
    }
    prewarm(desc, true);  // a draw is waiting for it (skipped meanwhile): ahead of the pipelines known from earlier runs
}

void PipelineCache::prewarmWorker() {
    SetThreadDescription(GetCurrentThread(), L"rvk pipeline prewarm");
    std::unique_lock lock(prewarmMutex_);
    for (;;) {
        prewarmCv_.wait(lock, [&] { return prewarmQuit_ || !prewarmQueue_.empty() || !prewarmUrgent_.empty(); });
        if (prewarmQuit_) {
            return;
        }
        std::deque<PipelineDesc>& q = !prewarmUrgent_.empty() ? prewarmUrgent_ : prewarmQueue_;
        const PipelineDesc d = q.front();
        q.pop_front();
        lock.unlock();
        bool claimed = false;
        {
            // claimed under the lock, built outside it: draws meanwhile find their pipelines, a get() of this one waits for it
            // instead of compiling it a second time (DXVK: parallel duplicate compiles are "problematic on some drivers")
            const std::lock_guard pl(mutex_);
            claimed = !pipelines_.contains(d) && building_.insert(d).second;
        }
        if (claimed) {
            const auto t0 = std::chrono::steady_clock::now();
            VkPipeline p = build(d);
            const auto ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            if (p != VK_NULL_HANDLE) {
                prewarmed_.fetch_add(1, std::memory_order_relaxed);
                prewarmNs_.fetch_add(ns, std::memory_order_relaxed);
            }
            {
                const std::lock_guard pl(mutex_);
                if (p != VK_NULL_HANDLE) pipelines_.emplace(d, p);  // a failure is left to get(), which logs and caches it
                building_.erase(d);
            }
            builtCv_.notify_all();
        }
        lock.lock();
    }
}

void PipelineCache::stopPrewarm() {
    {
        const std::lock_guard lock(prewarmMutex_);
        prewarmQuit_ = true;
        prewarmQueue_.clear();
        prewarmUrgent_.clear();
    }
    prewarmCv_.notify_all();
    for (auto& t : prewarmThreads_) {
        t.join();
    }
    prewarmThreads_.clear();
}

VkPipeline PipelineCache::get(const PipelineDesc& desc, bool* created) {
    std::unique_lock lock(mutex_);
    if (created != nullptr) {
        *created = false;
    }
    for (;;) {
        if (auto it = pipelines_.find(desc); it != pipelines_.end()) {
            return it->second;
        }
        if (!building_.contains(desc)) break;
        builtCv_.wait(lock);  // another thread compiles this description: its result, not a second compile
    }
    if (created != nullptr) {
        *created = true;
    }
    // compiled outside the lock (it was held across vkCreateGraphicsPipelines, ~13 ms per pipeline on AMD: every lookup of the
    // record threads and prewarm workers waited for it)
    building_.insert(desc);
    lock.unlock();
    const auto t0 = std::chrono::steady_clock::now();
    VkPipeline p = useLibraries_ ? link(desc, false) : VK_NULL_HANDLE;
    const bool linked = p != VK_NULL_HANDLE;
    if (!linked) {
        p = build(desc);
    }
    built_.fetch_add(1, std::memory_order_relaxed);
    buildNs_.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
                       std::memory_order_relaxed);
    lock.lock();
    pipelines_.emplace(desc, p);  // also caches failures (VK_NULL_HANDLE) so they are logged once
    building_.erase(desc);
    lock.unlock();
    builtCv_.notify_all();
    if (linked) {
        {
            const std::lock_guard ol(optMutex_);
            optQueue_.push_back(desc);
        }
        optCv_.notify_one();
    }
    return p;
}

VkPipeline PipelineCache::build(const PipelineDesc& d) {
    RVK_ZONE("vk::PipelineCache::build");
    const bool eds3 = ctx_->features().extendedDynamicState3;
    const bool dynVertexInput = ctx_->features().vertexInputDynamicState;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vkModule(d.vs), "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, vkModule(d.fs), "main", nullptr};

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
    names::pipeline(*ctx_, p, vkModule(d.vs), vkModule(d.fs));
    return p;
}

}  // namespace rvk::vk
