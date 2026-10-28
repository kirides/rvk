// d3d9::Device record threads: the opaque passes' draw packets recorded into secondary command buffers on worker threads.
// See device.h ("record threads") and docs/research/performance.md 14.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>

#include <windows.h>
#include <emmintrin.h>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"

namespace rvk::d3d9 {

namespace {

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

// Which scopes the record threads take: the main view's z pre-pass and solid pass, the sun cascades' depth pass (the passes
// whose draws are batched: docs/research/performance.md 12), when the device has the threads' prerequisites.
bool Device::wantSecondaryScope() const {
    // with fewer than four hardware threads the workers would compete with the engine thread and the driver
    static const bool enoughCores = std::thread::hardware_concurrency() >= 4;
    if (!recordThreadsWanted_ || !enoughCores) return false;
    const scene::Phase phase = phases_.current();
    return (phase.view == scene::View::Main && (phase.pass == scene::Pass::DepthPrepass || phase.pass == scene::Pass::Solid)) ||
           (phase.view == scene::View::Shadow && phase.pass == scene::Pass::ShadowDepth);
}

void Device::startRecordThreads() {
    if (!recordThreads_.empty()) return;
    recordQuit_ = false;
    for (std::uint32_t i = 0; i < kRecordThreads; ++i) {
        auto t = std::make_unique<RecordThread>();
        bool ok = true;
        for (std::uint32_t s = 0; s < vk::Frames::kInFlight && ok; ++s) {
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pci.queueFamilyIndex = ctx_.queueFamily();
            ok = vkCreateCommandPool(ctx_.device(), &pci, nullptr, &t->pools[s]) == VK_SUCCESS;
            if (ok && ctx_.namesEnabled()) {
                vk::names::object(ctx_, t->pools[s], VK_OBJECT_TYPE_COMMAND_POOL, std::format("rvk record worker {} slot {} pool", i, s));
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
    for (std::uint32_t s = 0; s < vk::Frames::kInFlight; ++s) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = ctx_.queueFamily();
        if (vkCreateCommandPool(ctx_.device(), &pci, nullptr, &recordSelf_->pools[s]) != VK_SUCCESS) recordSelf_.reset();
        else vk::names::object(ctx_, recordSelf_->pools[s], VK_OBJECT_TYPE_COMMAND_POOL, std::format("rvk record engine thread slot {} pool", s));
        if (!recordSelf_) break;
    }
    spdlog::info("d3d9: record threads: {} worker(s) + the engine thread, chunks of {} draws", recordThreads_.size(), kChunkPackets);
}

void Device::stopRecordThreads() {
    {
        const std::lock_guard lock(recordMutex_);
        recordQuit_ = true;
    }
    recordCv_.notify_all();
    for (auto& t : recordThreads_) {
        if (t->thread.joinable()) t->thread.join();
        for (VkCommandPool pool : t->pools) {
            if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(ctx_.device(), pool, nullptr);
        }
    }
    recordThreads_.clear();
    if (recordSelf_) {
        for (VkCommandPool pool : recordSelf_->pools) {
            if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(ctx_.device(), pool, nullptr);
        }
        recordSelf_.reset();
    }
    chunkPool_.clear();
    chunksUsed_ = 0;
    openChunk_ = nullptr;
    scopeItems_.clear();
}

// the slot's frame has completed (Frames::begin waited): its secondary command buffers can be recorded again
void Device::resetRecordPools(std::uint32_t slot) {
    scopeSecondary_ = false;
    for (auto& t : recordThreads_) {
        vkResetCommandPool(ctx_.device(), t->pools[slot], 0);
        t->used[slot] = 0;
    }
    if (recordSelf_) {
        vkResetCommandPool(ctx_.device(), recordSelf_->pools[slot], 0);
        recordSelf_->used[slot] = 0;
    }
}

void Device::recordThreadMain(RecordThread& t) {
    SetThreadDescription(GetCurrentThread(), L"rvk record");
    for (;;) {
        RecordChunk* c = nullptr;
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

void Device::recordChunk(RecordThread& t, RecordChunk& c) {
    std::vector<VkCommandBuffer>& bufs = t.buffers[c.slot];
    if (t.used[c.slot] == bufs.size()) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = t.pools[c.slot];
        ai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(ctx_.device(), &ai, &cb) != VK_SUCCESS) return;  // cmd stays null: the scope drops it
        vk::names::object(ctx_, cb, VK_OBJECT_TYPE_COMMAND_BUFFER, std::format("rvk record secondary (slot {}) #{}", c.slot, bufs.size()));
        bufs.push_back(cb);
    }
    VkCommandBuffer cmd = bufs[t.used[c.slot]++];
    VkCommandBufferInheritanceRenderingInfo ir{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
    ir.colorAttachmentCount = c.colorCount;
    ir.pColorAttachmentFormats = c.colorFormats;
    ir.depthAttachmentFormat = c.depthFormat;
    ir.stencilAttachmentFormat = c.stencilFormat;
    ir.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkCommandBufferInheritanceInfo ii{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    ii.pNext = &ir;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    bi.pInheritanceInfo = &ii;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return;
    // a secondary command buffer inherits no descriptor sets, pipeline or dynamic state
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.layout, 0, 1, &c.set, 0, nullptr);
    RecordState rs;
    for (const DrawPacket& p : c.packets) {
        if (p.op == DrawPacket::Op::Clear) {
            const ClearOp& op = c.clears[p.first];
            vkCmdClearAttachments(cmd, op.count, op.atts, 1, &op.rect);
        } else {
            applyPacket(rs, cmd, c.layout, p);
        }
    }
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) return;
    c.binds = rs.pipelineBinds;
    c.cmd = cmd;
}

Device::RecordChunk& Device::currentChunk() {
    if (openChunk_ == nullptr) {
        if (chunksUsed_ == chunkPool_.size()) {
            chunkPool_.push_back(std::make_unique<RecordChunk>());
            chunkPool_.back()->packets.reserve(kChunkPackets);
        }
        RecordChunk& c = *chunkPool_[chunksUsed_++];
        c.packets.clear();
        c.clears.clear();
        c.colorCount = colorCount_;
        std::copy(colorFormats_, colorFormats_ + kRenderTargets, c.colorFormats);
        c.depthFormat = depthFormat_;
        c.stencilFormat = stencilFormat_;
        c.set = bindless_.set();
        c.layout = bindless_.pipelineLayout();
        c.slot = frames_.currentSlot();
        c.cmd = VK_NULL_HANDLE;
        c.binds = 0;
        c.done.store(false, std::memory_order_relaxed);
        openChunk_ = &c;
    }
    return *openChunk_;
}

void Device::emitPacket(const DrawPacket& p) {
    RecordChunk& c = currentChunk();
    c.packets.push_back(p);
    if (c.packets.size() >= kChunkPackets) closeChunk();
}

void Device::emitClear(const ClearOp& op) {
    RecordChunk& c = currentChunk();
    DrawPacket p;
    p.op = DrawPacket::Op::Clear;
    p.first = static_cast<std::uint32_t>(c.clears.size());
    c.clears.push_back(op);
    c.packets.push_back(p);
}

void Device::closeChunk() {
    if (openChunk_ == nullptr) return;
    RecordChunk* c = openChunk_;
    openChunk_ = nullptr;
    startRecordThreads();
    ScopeItem item;
    item.kind = ScopeItem::Kind::Chunk;
    item.chunk = c;
    scopeItems_.push_back(item);
    if (recordThreads_.empty()) {
        c->done.store(true);  // no worker could be started (pool creation failed): the chunk's draws are dropped
        return;
    }
    {
        const std::lock_guard lock(recordMutex_);
        recordQueue_.push_back(c);
        recordPending_.fetch_add(1, std::memory_order_release);
    }
    if (recordSleepers_.load() != 0) recordCv_.notify_one();  // a spinning worker takes it without a wake-up
}

void Device::scopeLabel(bool begin, const char* text) {
    closeChunk();  // the labels stay in order with the chunks around them
    ScopeItem item;
    item.kind = begin ? ScopeItem::Kind::LabelBegin : ScopeItem::Kind::LabelEnd;
    if (begin) std::strncpy(item.text, text, sizeof(item.text) - 1);
    scopeItems_.push_back(item);
}

void Device::finishSecondaryScope() {
    closeChunk();
    // the engine thread takes the chunks no worker has started on and records them itself instead of idling
    while (recordSelf_) {
        RecordChunk* c = nullptr;
        {
            const std::lock_guard lock(recordMutex_);
            if (recordQueue_.empty()) break;
            c = recordQueue_.front();
            recordQueue_.pop_front();
            recordPending_.fetch_sub(1, std::memory_order_release);
        }
        recordChunk(*recordSelf_, *c);
        c->done.store(true, std::memory_order_release);
    }
    VkCommandBuffer cmd = frames_.cmd();
    VkCommandBuffer run[64];
    std::uint32_t runCount = 0;
    const auto flushRun = [&] {
        if (runCount > 0) vkCmdExecuteCommands(cmd, runCount, run);
        runCount = 0;
    };
    for (const ScopeItem& item : scopeItems_) {
        if (item.kind == ScopeItem::Kind::Chunk) {
            RecordChunk& c = *item.chunk;
            while (!c.done.load(std::memory_order_acquire)) c.done.wait(false, std::memory_order_acquire);
            perf_.pipelineBinds += c.binds;
            if (c.cmd == VK_NULL_HANDLE) continue;
            if (runCount == 64) flushRun();
            run[runCount++] = c.cmd;
        } else {
            flushRun();
            if (item.kind == ScopeItem::Kind::LabelBegin) beginScopeLabel(cmd, item.text);
            else vkCmdEndDebugUtilsLabelEXT(cmd);
        }
    }
    flushRun();
    scopeItems_.clear();
    chunksUsed_ = 0;
    scopeSecondary_ = false;
}

}  // namespace rvk::d3d9
