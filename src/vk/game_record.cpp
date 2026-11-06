// The Vulkan game device's draw-path recording (rhi::GameDevice virtuals over this; docs/research/performance.md 12 + 14):
// pipeline cache, rendering scopes, record threads and packet application. The bodies are the D3D9 model's recording code
// (src/d3d9/record.cpp, src/d3d9/draw.cpp) moved verbatim behind the seam.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>

#include <windows.h>
#include <emmintrin.h>

#include <spdlog/spdlog.h>

#include "vk/game_record.h"
#include "vk/debug_names.h"
#include "vk/image.h"
#include "rhi/d3d_formats.h"
#include "rhi/vulkan/wrap.h"

namespace rvk::vk {

namespace {

constexpr VkFrontFace kFrontFace = VK_FRONT_FACE_CLOCKWISE;

// true (and the cache updated) when `value` differs from the cached one or the cache is not valid yet
template <class T>
bool changed(bool valid, T& cached, const T& value) {
    if (valid && std::memcmp(&cached, &value, sizeof(T)) == 0) {
        return false;
    }
    std::memcpy(&cached, &value, sizeof(T));
    return true;
}

// the labels of VK_EXT_debug_utils in a scope's primary command buffer: the same colours as Device::regionBegin's
void beginScopeLabel(VkCommandBuffer cmd, const char* name) {
    std::uint32_t h = 2166136261u;
    for (const char* c = name; *c != 0; ++c) h = (h ^ static_cast<std::uint8_t>(*c)) * 16777619u;
    const float hue = static_cast<float>(h % 360u) / 60.0f;
    const float x = 1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f);
    const float rgb[6][3] = {{1, x, 0}, {x, 1, 0}, {0, 1, x}, {0, x, 1}, {x, 0, 1}, {1, 0, x}};
    const float* c = rgb[static_cast<int>(hue) % 6];
    VkDebugUtilsLabelEXT l{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    l.pLabelName = name;
    l.color[0] = 0.35f + 0.5f * c[0];
    l.color[1] = 0.35f + 0.5f * c[1];
    l.color[2] = 0.35f + 0.5f * c[2];
    l.color[3] = 1.0f;
    vkCmdBeginDebugUtilsLabelEXT(cmd, &l);
}

}  // namespace

// ---- the pipeline cache ---------------------------------------------------------------------------------------------

bool GameRecord::create(Context& ctx, VkPipelineLayout layout, const std::filesystem::path& cacheFile, bool libraries) {
    return pipelines_.create(ctx, layout, cacheFile, libraries);
}

rhi::GamePipeline GameRecord::resolvePipeline(const PipelineDesc& desc, bool* created) {
    return rhiPipeline(pipelines_.get(desc, created));
}

rhi::GamePipeline GameRecord::findPipeline(const PipelineDesc& desc) const {
    return rhiPipeline(pipelines_.find(desc));
}

bool GameRecord::containsPipeline(const PipelineDesc& desc) const {
    return pipelines_.contains(desc);
}

void GameRecord::prewarmPipeline(const PipelineDesc& desc) {
    pipelines_.prewarm(desc);
}

void GameRecord::prewarmOncePipeline(const PipelineDesc& desc) {
    pipelines_.prewarmOnce(desc);
}

void GameRecord::prepareShader(rhi::ShaderModule module, VkShaderStageFlagBits stage) {
    pipelines_.prepareShader(module, stage);
}

void GameRecord::saveCache() {
    pipelines_.saveAsync();
}

rhi::PipelineStats GameRecord::stats() const {
    rhi::PipelineStats s;
    s.built = pipelines_.built();
    s.buildMs = pipelines_.buildMs();
    s.libraries = pipelines_.libraries();
    s.libraryMs = pipelines_.libraryMs();
    s.optimized = pipelines_.optimized();
    s.optimizeMs = pipelines_.optimizeMs();
    s.prewarmed = pipelines_.prewarmed();
    s.prewarmMs = pipelines_.prewarmMs();
    s.cached = pipelines_.size();
    s.librariesEnabled = pipelines_.usesLibraries();
    return s;
}

// ---- scopes, record threads, chunks (the D3D9 model's record threads, d3d9/record.cpp, moved here) ------------------------

void GameRecord::init(Context& ctx, Frames& frames, Bindless& bindless) {
    ctx_ = &ctx;
    frames_ = &frames;
    bindless_ = &bindless;
}

// the slot's frame has completed (Frames::begin waited): its secondary command buffers can be recorded again
void GameRecord::resetRecordPools(std::uint32_t slot) {
    scopeSecondary_ = false;
    for (auto& t : recordThreads_) {
        vkResetCommandPool(ctx_->device(), t->pools[slot], 0);
        t->used[slot] = 0;
    }
    if (recordSelf_) {
        vkResetCommandPool(ctx_->device(), recordSelf_->pools[slot], 0);
        recordSelf_->used[slot] = 0;
    }
}

void GameRecord::recordThreadMain(RecordThread& t) {
    SetThreadDescription(GetCurrentThread(), L"rvk record");
    for (;;) {
        ChunkEntry* c = nullptr;
        // a chunk usually follows the last one within microseconds: spin a little before the (kernel) wait, so the engine
        // thread's notify mostly finds nobody asleep
        for (int spin = 0; spin < 4000 && recordPending_.load(std::memory_order_acquire) == 0; ++spin) {
            if (recordQuit_) break;
            _mm_pause();
        }
        {
            std::unique_lock lock(recordMutex_);
            if (recordQueue_.empty() && !recordQuit_) {
                recordSleepers_.fetch_add(1);
                recordCv_.wait(lock, [this] { return recordQuit_ || !recordQueue_.empty(); });
                recordSleepers_.fetch_sub(1);
            }
            if (recordQueue_.empty()) {
                if (recordQuit_) return;
                continue;  // the engine thread took the chunk first
            }
            c = recordQueue_.front();
            recordQueue_.pop_front();
            recordPending_.fetch_sub(1, std::memory_order_release);
        }
        recordChunk(t, *c);
        c->done.store(true, std::memory_order_release);
        c->done.notify_all();
    }
}

void GameRecord::recordChunk(RecordThread& t, ChunkEntry& c) {
    std::vector<VkCommandBuffer>& bufs = t.buffers[c.slot];
    if (t.used[c.slot] == bufs.size()) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = t.pools[c.slot];
        ai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(ctx_->device(), &ai, &cb) != VK_SUCCESS) return;  // cmd stays null: the scope drops it
        vk::names::object(*ctx_, cb, VK_OBJECT_TYPE_COMMAND_BUFFER, std::format("rvk record secondary (slot {}) #{}", c.slot, bufs.size()));
        bufs.push_back(cb);
    }
    VkCommandBuffer cmd = bufs[t.used[c.slot]++];
    VkCommandBufferInheritanceRenderingInfo ir{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
    ir.colorAttachmentCount = c.draw.colorCount;
    ir.pColorAttachmentFormats = c.draw.colorFormats;
    ir.depthAttachmentFormat = c.draw.depthFormat;
    ir.stencilAttachmentFormat = c.draw.stencilFormat;
    ir.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkCommandBufferInheritanceInfo ii{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    ii.pNext = &ir;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    bi.pInheritanceInfo = &ii;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return;
    // a secondary command buffer inherits no descriptor sets, pipeline or dynamic state
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.layout, 0, 1, &c.set, 0, nullptr);
    GameRecordState rs;
    for (const rhi::DrawPacket& p : c.draw.packets) {
        if (p.op == rhi::DrawPacket::Op::Clear) {
            const rhi::ClearOp& op = c.draw.clears[p.first];
            vkCmdClearAttachments(cmd, op.count, op.atts, 1, &op.rect);
        } else {
            applyPacket(rs, cmd, c.layout, p);
        }
    }
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) return;
    c.binds = rs.pipelineBinds;
    c.cmd = cmd;
}

void GameRecord::startRecordThreads() {
    if (!recordThreads_.empty()) return;
    recordQuit_ = false;
    for (std::uint32_t i = 0; i < kRecordThreads; ++i) {
        auto t = std::make_unique<RecordThread>();
        bool ok = true;
        for (std::uint32_t s = 0; s < rhi::kFramesInFlight && ok; ++s) {
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pci.queueFamilyIndex = ctx_->queueFamily();
            ok = vkCreateCommandPool(ctx_->device(), &pci, nullptr, &t->pools[s]) == VK_SUCCESS;
            if (ok && ctx_->namesEnabled()) {
                vk::names::object(*ctx_, t->pools[s], VK_OBJECT_TYPE_COMMAND_POOL, std::format("rvk record worker {} slot {} pool", i, s));
            }
        }
        if (!ok) {
            spdlog::warn("d3d9: record threads: command pool creation failed");
            break;
        }
        RecordThread* raw = t.get();
        recordThreads_.push_back(std::move(t));
        raw->thread = std::thread([this, raw] { recordThreadMain(*raw); });
    }
    recordSelf_ = std::make_unique<RecordThread>();
    for (std::uint32_t s = 0; s < rhi::kFramesInFlight; ++s) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = ctx_->queueFamily();
        if (vkCreateCommandPool(ctx_->device(), &pci, nullptr, &recordSelf_->pools[s]) != VK_SUCCESS) recordSelf_.reset();
        else vk::names::object(*ctx_, recordSelf_->pools[s], VK_OBJECT_TYPE_COMMAND_POOL, std::format("rvk record engine thread slot {} pool", s));
        if (!recordSelf_) break;
    }
    spdlog::info("d3d9: record threads: {} worker(s) + the engine thread", recordThreads_.size());
}

void GameRecord::stopRecordThreads() {
    {
        const std::lock_guard lock(recordMutex_);
        recordQuit_ = true;
    }
    recordCv_.notify_all();
    for (auto& t : recordThreads_) {
        if (t->thread.joinable()) t->thread.join();
        for (VkCommandPool pool : t->pools) {
            if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(ctx_->device(), pool, nullptr);
        }
    }
    recordThreads_.clear();
    if (recordSelf_) {
        for (VkCommandPool pool : recordSelf_->pools) {
            if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(ctx_->device(), pool, nullptr);
        }
        recordSelf_.reset();
    }
    chunkPool_.clear();
    chunksUsed_ = 0;
    scopeItems_.clear();
    pipelines_.destroy();
}

// hand a chunk of packets to the backend: in scopeChunk order it is one scope item; a worker starts on it at once
void GameRecord::scopeChunk(rhi::DrawChunk&& chunk) {
    if (chunksUsed_ == chunkPool_.size()) {
        chunkPool_.push_back(std::make_unique<ChunkEntry>());
        chunkPool_.back()->draw.packets.reserve(rhi::kChunkPackets);
    }
    ChunkEntry* e = chunkPool_[chunksUsed_++].get();
    e->draw = std::move(chunk);
    e->slot = frames_->currentSlot();
    e->set = bindless_->set();
    e->layout = bindless_->pipelineLayout();
    e->cmd = VK_NULL_HANDLE;
    e->binds = 0;
    e->done.store(false, std::memory_order_relaxed);
    ScopeItem item;
    item.kind = ScopeItem::Kind::Chunk;
    item.chunk = e;
    scopeItems_.push_back(item);
    if (recordThreads_.empty()) {
        e->done.store(true);  // no worker could be started (pool creation failed): the chunk's draws are dropped
        return;
    }
    {
        const std::lock_guard lock(recordMutex_);
        recordQueue_.push_back(e);
        recordPending_.fetch_add(1, std::memory_order_release);
    }
    if (recordSleepers_.load() != 0) recordCv_.notify_one();  // a spinning worker takes it without a wake-up
}

void GameRecord::scopeLabel(bool begin, const char* text) {
    ScopeItem item;
    item.kind = begin ? ScopeItem::Kind::LabelBegin : ScopeItem::Kind::LabelEnd;
    if (begin) std::strncpy(item.text, text, sizeof(item.text) - 1);
    scopeItems_.push_back(item);
}

// end of a rendering scope: the engine thread takes the chunks no worker has started on and records them itself
// instead of idling; then the scope's items (chunks, labels) are executed in order
void GameRecord::endScope() {
    VkCommandBuffer cmd = frames_->cmd();
    if (scopeSecondary_) {
        while (recordSelf_) {
            ChunkEntry* e = nullptr;
            {
                const std::lock_guard lock(recordMutex_);
                if (recordQueue_.empty()) break;
                e = recordQueue_.front();
                recordQueue_.pop_front();
                recordPending_.fetch_sub(1, std::memory_order_release);
            }
            recordChunk(*recordSelf_, *e);
            e->done.store(true, std::memory_order_release);
            e->done.notify_all();
        }
        VkCommandBuffer run[64];
        std::uint32_t runCount = 0;
        const auto flushRun = [&] {
            if (runCount > 0) vkCmdExecuteCommands(cmd, runCount, run);
            runCount = 0;
        };
        for (const ScopeItem& item : scopeItems_) {
            if (item.kind == ScopeItem::Kind::Chunk) {
                ChunkEntry* e = item.chunk;
                while (!e->done.load(std::memory_order_acquire)) e->done.wait(false, std::memory_order_acquire);
                chunkBinds_ += e->binds;
                if (e->cmd == VK_NULL_HANDLE) continue;
                if (runCount == 64) flushRun();
                run[runCount++] = e->cmd;
            } else {
                flushRun();
                if (item.kind == ScopeItem::Kind::LabelBegin) beginScopeLabel(cmd, item.text);
                else vkCmdEndDebugUtilsLabelEXT(cmd);
            }
        }
        flushRun();
        scopeItems_.clear();
        chunksUsed_ = 0;
    }
    vkCmdEndRendering(cmd);
    if (scopeSecondary_) {
        // executing secondary command buffers leaves the primary's pipeline, dynamic state and descriptor sets undefined
        rec_.reset();
        const VkDescriptorSet set = bindless_->set();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bindless_->pipelineLayout(), 0, 1, &set, 0, nullptr);
        scopeSecondary_ = false;
    }
}

void GameRecord::beginScope(const rhi::RenderingInfo& info, bool secondary) {
    VkCommandBuffer cmd = frames_->cmd();
    VkRenderingAttachmentInfo colors[rhi::kMaxColorTargets]{};
    for (std::uint32_t i = 0; i < info.colorCount; ++i) {
        const rhi::RenderingInfo::Attachment& a = info.color[i];
        vk::Image& img = rhi::vulkan::imageOf(*a.texture);
        img.transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo& ri = colors[i];
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ri.imageView = img.attachmentView(*ctx_, a.mip, a.layer);
        ri.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ri.loadOp = a.load == VK_ATTACHMENT_LOAD_OP_CLEAR ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        ri.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (ri.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) ri.clearValue.color = a.clear;
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    bool stencilOn = false;
    VkRenderingAttachmentInfo stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (info.depth.texture != nullptr) {
        vk::Image& img = rhi::vulkan::imageOf(*info.depth.texture);
        img.transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = img.attachmentView(*ctx_, info.depth.mip, info.depth.layer);
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = info.depth.load == VK_ATTACHMENT_LOAD_OP_CLEAR ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (depth.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            depth.clearValue.depthStencil.depth = info.depthClear.depth;
            depth.clearValue.depthStencil.stencil = info.depthClear.stencil;
        }
        if (vk::hasStencil(img.format())) {
            stencil = depth;
            stencilOn = true;
        }
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {info.width, info.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = info.colorCount;
    ri.pColorAttachments = info.colorCount > 0 ? colors : nullptr;
    ri.pDepthAttachment = info.depth.texture != nullptr ? &depth : nullptr;
    ri.pStencilAttachment = stencilOn ? &stencil : nullptr;
    if (secondary) ri.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
    vkCmdBeginRendering(cmd, &ri);
    scopeSecondary_ = secondary;
    if (secondary) startRecordThreads();  // the first threaded scope starts the workers (idempotent)
}

std::uint64_t GameRecord::takePipelineBinds() {
    const std::uint64_t n = rec_.pipelineBinds + chunkBinds_;
    rec_.pipelineBinds = 0;
    chunkBinds_ = 0;
    return n;
}

// foreign commands (the overlay's pipeline / dynamic state / descriptor set) left the frame undefined for the next draw:
// reset the recorded state and rebind the frame's bindless set
void GameRecord::invalidateRecordingState() {
    rec_.reset();
    const VkDescriptorSet set = bindless_->set();
    vkCmdBindDescriptorSets(frames_->cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, bindless_->pipelineLayout(), 0, 1, &set, 0, nullptr);
}

// the inline path: one packet recorded into the frame's command list now
void GameRecord::applyDrawPacket(const rhi::DrawPacket& packet) {
    applyPacket(rec_, frames_->cmd(), bindless_->pipelineLayout(), packet);
}

// ---- packet application (the D3D9 model's, src/d3d9/draw.cpp, moved verbatim; vi.buffers / indexBuffer are Buffer handles) ---

// the device is going away: join the record workers, destroy the pipeline cache (its own prewarm / library threads
// and saved blob go with it). Idempotent: vk::GameDeviceImpl's destructor calls this before the renderer's Context /
// Frames / Bindless are gone, ~GameRecord runs later and finds everything already stopped
void GameRecord::shutdown() {
    stopRecordThreads();  // joins the workers, releases their pools and destroys the pipeline cache (all idempotent)
}

GameRecord::~GameRecord() {
    shutdown();
}

void GameRecord::applyPacket(GameRecordState& rs, VkCommandBuffer cmd, VkPipelineLayout layout, const rhi::DrawPacket& p) {
    if (p.vi != rs.lastVi) {
        applyVertexInput(rs, cmd, *p.vi);
        rs.lastVi = p.vi;
    }
    const VkPipeline pipeline = vkPipeline(p.pipeline);
    if (pipeline != rs.bound) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        rs.bound = pipeline;
        ++rs.pipelineBinds;
    }
    if (p.dyn != rs.lastDyn) {
        applyDynamicState(rs, cmd, *p.dyn);
        rs.lastDyn = p.dyn;
    }
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p.push), &p.push);
    if (p.op == rhi::DrawPacket::Op::DrawIndexedIndirect) {
        // F2b-1 scene draws: p.count records in a ring block (frame-ownership.md 7.11)
        bindIndexBuffer(rs, cmd, reinterpret_cast<VkBuffer>(p.indexBuffer), p.indexOffset, p.index32);
        vkCmdDrawIndexedIndirect(cmd, reinterpret_cast<VkBuffer>(p.indirectBuffer), p.indirectOffset, p.count, rhi::kIndirectStride);
    } else if (p.op == rhi::DrawPacket::Op::DrawIndexed) {
        bindIndexBuffer(rs, cmd, reinterpret_cast<VkBuffer>(p.indexBuffer), p.indexOffset, p.index32);
        vkCmdDrawIndexed(cmd, p.count, p.instances, p.first, p.vertexOffset, 0);
    } else {
        vkCmdDraw(cmd, p.count, p.instances, p.first, 0);
    }
}

void GameRecord::applyVertexInput(GameRecordState& rs, VkCommandBuffer cmd, const rhi::VertexInputValues& vi) {
    GameVI& vc = rs.vi;
    const std::uint32_t n = vi.bufferCount;
    if (n > 0 && (!vc.buffersValid || vc.bufferCount != n ||
                  std::memcmp(vc.buffers, vi.buffers, sizeof(VkBuffer) * n) != 0 ||
                  std::memcmp(vc.offsets, vi.offsets, sizeof(VkDeviceSize) * n) != 0)) {
        // the buffers come in as rhi Buffer handles (the Vulkan value is the VkBuffer)
        VkBuffer buffers[rhi::kMaxVertexBindings];
        VkDeviceSize offsets[rhi::kMaxVertexBindings];
        for (std::uint32_t i = 0; i < n; ++i) {
            buffers[i] = reinterpret_cast<VkBuffer>(vi.buffers[i]);
            offsets[i] = static_cast<VkDeviceSize>(vi.offsets[i]);
        }
        vkCmdBindVertexBuffers(cmd, 0, n, buffers, offsets);
        vc.bufferCount = n;
        std::memcpy(vc.buffers, buffers, sizeof(VkBuffer) * n);
        std::memcpy(vc.offsets, offsets, sizeof(VkDeviceSize) * n);
        vc.buffersValid = true;
    }
    if (!vi.dynamicLayout) return;
    if (vc.layoutValid && vc.bindingCount == vi.bindingCount && vc.attributeCount == vi.attributeCount &&
        std::memcmp(vc.bindings, vi.bindings, sizeof(vi.bindings[0]) * vi.bindingCount) == 0 &&
        std::memcmp(vc.attributes, vi.attributes, sizeof(vi.attributes[0]) * vi.attributeCount) == 0) {
        return;
    }
    VkVertexInputBindingDescription2EXT b[rhi::kMaxVertexBindings]{};
    VkVertexInputAttributeDescription2EXT a[rhi::kMaxVertexAttributes]{};
    for (std::uint32_t i = 0; i < vi.bindingCount; ++i) {
        b[i] = {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr, i, vi.bindings[i].stride,
                vi.bindings[i].perInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, 1};
    }
    for (std::uint32_t i = 0; i < vi.attributeCount; ++i) {
        a[i] = {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, nullptr, vi.attributes[i].location,
                vi.attributes[i].binding, vi.attributes[i].format, vi.attributes[i].offset};
    }
    vkCmdSetVertexInputEXT(cmd, vi.bindingCount, b, vi.attributeCount, a);
    vc.bindingCount = vi.bindingCount;
    vc.attributeCount = vi.attributeCount;
    std::memcpy(vc.bindings, vi.bindings, sizeof(vi.bindings[0]) * vi.bindingCount);
    std::memcpy(vc.attributes, vi.attributes, sizeof(vi.attributes[0]) * vi.attributeCount);
    vc.layoutValid = true;
}

void GameRecord::applyDynamicState(GameRecordState& rs, VkCommandBuffer cmd, const rhi::DynamicStateValues& s) {
    GameDyn& c = rs.dyn;
    const bool v = c.valid;
    if (!v) {
        // a new command buffer: nothing is set yet (the cached colour attachment count would carry over otherwise)
        c.colorCount = 0;
        // constant for every draw
        vkCmdSetFrontFace(cmd, kFrontFace);
        vkCmdSetPrimitiveRestartEnable(cmd, VK_FALSE);
    }
    if (changed(v, c.viewport, s.viewport)) vkCmdSetViewportWithCount(cmd, 1, &s.viewport);
    if (changed(v, c.scissor, s.scissor)) vkCmdSetScissorWithCount(cmd, 1, &s.scissor);
    if (changed(v, c.cull, s.cull)) vkCmdSetCullMode(cmd, s.cull);
    if (changed(v, c.topology, s.topology)) vkCmdSetPrimitiveTopology(cmd, s.topology);
    if (changed(v, c.depthTest, s.depthTest)) vkCmdSetDepthTestEnable(cmd, s.depthTest);
    if (changed(v, c.depthWrite, s.depthWrite)) vkCmdSetDepthWriteEnable(cmd, s.depthWrite);
    if (changed(v, c.depthCompare, s.depthCompare)) vkCmdSetDepthCompareOp(cmd, s.depthCompare);
    if (changed(v, c.stencilTest, s.stencilTest)) vkCmdSetStencilTestEnable(cmd, s.stencilTest);
    {
        const bool f1 = changed(v, c.stencilFront, s.stencilFront);
        if (changed(v, c.stencilFrontCompare, s.stencilFrontCompare) || f1) {
            vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_BIT, s.stencilFront[0], s.stencilFront[1], s.stencilFront[2], s.stencilFrontCompare);
        }
        const bool b1 = changed(v, c.stencilBack, s.stencilBack);
        if (changed(v, c.stencilBackCompare, s.stencilBackCompare) || b1) {
            vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_BACK_BIT, s.stencilBack[0], s.stencilBack[1], s.stencilBack[2], s.stencilBackCompare);
        }
    }
    if (changed(v, c.stencilMasks[0], s.stencilMasks[0])) vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, s.stencilMasks[0]);
    if (changed(v, c.stencilMasks[1], s.stencilMasks[1])) vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, s.stencilMasks[1]);
    if (changed(v, c.stencilMasks[2], s.stencilMasks[2])) vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, s.stencilMasks[2]);
    if (changed(v, c.depthBiasEnable, s.depthBiasEnable)) vkCmdSetDepthBiasEnable(cmd, s.depthBiasEnable);
    if (changed(v, c.depthBias, s.depthBias)) vkCmdSetDepthBias(cmd, s.depthBias[0], 0.0f, s.depthBias[1]);
    if (changed(v, c.blendConstants, s.blendConstants)) vkCmdSetBlendConstants(cmd, s.blendConstants);
    if (s.colorCount > 0) {
        const std::uint32_t n = s.colorCount;
        // attachments beyond the count keep stale values: only the first n are compared and recorded
        const bool sameCount = v && c.colorCount >= n;
        if (!sameCount || std::memcmp(c.blendEnable, s.blendEnable, sizeof(VkBool32) * n) != 0) {
            vkCmdSetColorBlendEnableEXT(cmd, 0, n, s.blendEnable);
            std::memcpy(c.blendEnable, s.blendEnable, sizeof(VkBool32) * n);
        }
        if (!sameCount || std::memcmp(c.blendEquation, s.blendEquation, sizeof(VkColorBlendEquationEXT) * n) != 0) {
            vkCmdSetColorBlendEquationEXT(cmd, 0, n, s.blendEquation);
            std::memcpy(c.blendEquation, s.blendEquation, sizeof(VkColorBlendEquationEXT) * n);
        }
        if (!sameCount || std::memcmp(c.writeMask, s.writeMask, sizeof(VkColorComponentFlags) * n) != 0) {
            vkCmdSetColorWriteMaskEXT(cmd, 0, n, s.writeMask);
            std::memcpy(c.writeMask, s.writeMask, sizeof(VkColorComponentFlags) * n);
        }
        if (!sameCount) {
            c.colorCount = n;
        }
    }
    c.valid = true;
}

// the index buffer binding, recorded only when it differs from this command buffer's last one
void GameRecord::bindIndexBuffer(GameRecordState& rs, VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, bool index32) {
    GameIB& c = rs.ib;
    if (!c.valid || c.buffer != buffer || c.offset != offset || c.index32 != index32) {
        vkCmdBindIndexBuffer(cmd, buffer, offset, index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        c = {true, index32, buffer, offset};
    }
}

}  // namespace rvk::vk
