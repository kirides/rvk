#include "vk/frames.h"

#include <algorithm>
#include <chrono>

#include <spdlog/spdlog.h>

#include "vk/context.h"

namespace rvk::vk {

bool Frames::create(Context& ctx) {
    ctx_ = &ctx;
    VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    tci.initialValue = 0;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci};
    if (vkCreateSemaphore(ctx.device(), &sci, nullptr, &timeline_) != VK_SUCCESS) {
        return false;
    }
    ctx.setName(timeline_, VK_OBJECT_TYPE_SEMAPHORE, "rvk frame timeline");
    for (auto& s : slots_) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = ctx.queueFamily();
        if (vkCreateCommandPool(ctx.device(), &pci, nullptr, &s.pool) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = s.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(ctx.device(), &ai, &s.cmd);
        vkAllocateCommandBuffers(ctx.device(), &ai, &s.upload);
    }
    return newChunk(kChunkSize);
}

void Frames::destroy() {
    if (ctx_ == nullptr) {
        return;
    }
    waitIdle();
    runRetired(UINT64_MAX);
    for (auto& c : chunks_) {
        vmaDestroyBuffer(ctx_->allocator(), c.buffer, c.allocation);
    }
    chunks_.clear();
    freeChunks_.clear();
    for (auto& s : slots_) {
        if (s.pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(ctx_->device(), s.pool, nullptr);
            s.pool = VK_NULL_HANDLE;
        }
    }
    if (timeline_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(ctx_->device(), timeline_, nullptr);
        timeline_ = VK_NULL_HANDLE;
    }
    ctx_ = nullptr;
}

std::uint64_t Frames::completedValue() const {
    std::uint64_t v = 0;
    vkGetSemaphoreCounterValue(ctx_->device(), timeline_, &v);
    return v;
}

void Frames::waitFor(std::uint64_t value) const {
    if (value == 0) {
        return;
    }
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline_;
    wi.pValues = &value;
    vkWaitSemaphores(ctx_->device(), &wi, UINT64_MAX);
}

void Frames::waitIdle() {
    if (ctx_ != nullptr) {
        ctx_->waitIdle();
    }
}

void Frames::begin() {
    Slot& s = slots_[slot()];
    lastSlotWaitMs_ = 0.0;
    if (frameValue_ > kInFlight && completedValue() < frameValue_ - kInFlight) {
        const auto t0 = std::chrono::steady_clock::now();
        waitFor(frameValue_ - kInFlight);
        lastSlotWaitMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    runRetired(completedValue());
    for (std::size_t c : s.chunks) {
        chunks_[c].used = 0;
        freeChunks_.push_back(c);
    }
    s.chunks.clear();
    currentChunk_ = SIZE_MAX;
    vkResetCommandPool(ctx_->device(), s.pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cmd, &bi);
    s.uploadBegun = false;
    recording_ = true;
}

VkCommandBuffer Frames::uploadCmd() {
    Slot& s = slots_[slot()];
    if (!s.uploadBegun) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(s.upload, &bi);
        s.uploadBegun = true;
    }
    return s.upload;
}

bool Frames::submit(VkSemaphore waitAcquire, VkSemaphore signalRendered) {
    Slot& s = slots_[slot()];
    VkCommandBufferSubmitInfo cbs[2]{};
    std::uint32_t ncb = 0;
    if (s.uploadBegun) {
        vkEndCommandBuffer(s.upload);
        cbs[ncb++] = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, s.upload, 0};
    }
    vkEndCommandBuffer(s.cmd);
    cbs[ncb++] = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, s.cmd, 0};

    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = waitAcquire;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    VkSemaphoreSubmitInfo signal[2]{};
    std::uint32_t nsignal = 0;
    signal[nsignal++] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, timeline_, frameValue_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
    if (signalRendered != VK_NULL_HANDLE) {
        signal[nsignal++] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, signalRendered, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
    }
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = waitAcquire != VK_NULL_HANDLE ? 1u : 0u;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = ncb;
    si.pCommandBufferInfos = cbs;
    si.signalSemaphoreInfoCount = nsignal;
    si.pSignalSemaphoreInfos = signal;
    const VkResult r = ctx_->queueSubmit(si);
    recording_ = false;
    ++frameValue_;
    if (r != VK_SUCCESS) {
        spdlog::error("vulkan: vkQueueSubmit2: {}", resultString(r));
        return false;
    }
    return true;
}

void Frames::retire(std::function<void()> fn) {
    const std::lock_guard lock(retireMutex_);
    // between frames the last submitted frame (frameValue_ - 1) is the last possible user
    retired_.emplace_back(recording_ ? frameValue_ : frameValue_ - 1, std::move(fn));
}

void Frames::runRetired(std::uint64_t completed) {
    std::vector<std::function<void()>> due;
    {
        const std::lock_guard lock(retireMutex_);
        auto it = std::stable_partition(retired_.begin(), retired_.end(), [&](const auto& e) { return e.first > completed; });
        for (auto j = it; j != retired_.end(); ++j) {
            due.push_back(std::move(j->second));
        }
        retired_.erase(it, retired_.end());
    }
    for (auto& fn : due) {
        fn();
    }
}

bool Frames::newChunk(VkDeviceSize minSize) {
    if (!freeChunks_.empty()) {
        for (auto it = freeChunks_.begin(); it != freeChunks_.end(); ++it) {
            if (chunks_[*it].size >= minSize && recording_) {
                currentChunk_ = *it;
                freeChunks_.erase(it);
                slots_[slot()].chunks.push_back(currentChunk_);
                return true;
            }
        }
    }
    Chunk c;
    c.size = std::max(kChunkSize, minSize);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = c.size;
    // indirect: the batched draws' generated-command sequences (d3d9 Device::flushBatch) are written here
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (ctx_->features().rayTracing.enabled) {
        // dynamic vertex / index data lives here: a BLAS build may read it (docs/research/raytracing.md 2)
        bci.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(ctx_->allocator(), &bci, &aci, &c.buffer, &c.allocation, &info) != VK_SUCCESS) {
        spdlog::error("vulkan: upload ring chunk of {} MiB failed", c.size >> 20);
        return false;
    }
    c.ptr = static_cast<std::uint8_t*>(info.pMappedData);
    VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    dai.buffer = c.buffer;
    c.address = vkGetBufferDeviceAddress(ctx_->device(), &dai);
    ctx_->setName(c.buffer, VK_OBJECT_TYPE_BUFFER, "rvk upload ring");
    chunks_.push_back(c);
    if (recording_) {
        currentChunk_ = chunks_.size() - 1;
        slots_[slot()].chunks.push_back(currentChunk_);
    } else {
        freeChunks_.push_back(chunks_.size() - 1);
    }
    return true;
}

RingAlloc Frames::allocate(VkDeviceSize size, VkDeviceSize alignment) {
    ringBytes_ += size;
    if (currentChunk_ != SIZE_MAX) {
        Chunk& c = chunks_[currentChunk_];
        const VkDeviceSize offset = (c.used + alignment - 1) & ~(alignment - 1);
        if (offset + size <= c.size) {
            c.used = offset + size;
            return {c.buffer, offset, c.ptr + offset, c.address + offset};
        }
    }
    if (!newChunk(size)) {
        return {};
    }
    Chunk& c = chunks_[currentChunk_];
    c.used = size;
    return {c.buffer, 0, c.ptr, c.address};
}

}  // namespace rvk::vk
