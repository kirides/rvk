#include "vk/crash_markers.h"

#include <algorithm>
#include <cstring>
#include <format>

#include <spdlog/spdlog.h>

namespace rvk::vk {

bool CrashMarkers::create(Context& ctx) {
    destroy();
    // VK_AMD_buffer_marker when the driver has it; else vkCmdFillBuffer with a full barrier before each marker (serializes the
    // GPU: every marker then says exactly which region was running)
    amd_ = ctx.features().bufferMarker && vkCmdWriteBufferMarker2AMD != nullptr;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = 64;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(ctx.allocator(), &bci, &aci, &buffer_, &alloc_, &info) != VK_SUCCESS || info.pMappedData == nullptr) {
        spdlog::warn("vulkan: crash markers: no host-visible buffer");
        destroy();
        return false;
    }
    ctx_ = &ctx;
    mapped_ = static_cast<volatile std::uint32_t*>(info.pMappedData);
    mapped_[0] = mapped_[1] = 0;
    next_ = 0;
    spdlog::info("vulkan: crash markers on ({}): the unfinished regions are logged after a device loss",
                 amd_ ? "VK_AMD_buffer_marker" : "fill buffer + barriers: the GPU runs the regions one after another");
    return true;
}

void CrashMarkers::destroy() {
    if (buffer_ != VK_NULL_HANDLE && ctx_ != nullptr) vmaDestroyBuffer(ctx_->allocator(), buffer_, alloc_);
    buffer_ = VK_NULL_HANDLE;
    alloc_ = nullptr;
    mapped_ = nullptr;
}

void CrashMarkers::write(VkCommandBuffer cmd, std::uint64_t frame, const char* text, bool start) {
    if (!valid()) return;
    const std::uint32_t id = ++next_;
    Event& e = events_[id % kEvents];
    e.frame = frame;
    e.start = start;
    std::strncpy(e.text, text, sizeof(e.text) - 1);
    e.text[sizeof(e.text) - 1] = 0;
    if (!amd_) {
        // everything before the marker finished (and became visible) before it is written: slot 0 = the last region started,
        // slot 1 = the last region ended
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
        vkCmdFillBuffer(cmd, buffer_, start ? 0 : 4, 4, id);
        // later work waits for the write
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier2(cmd, &dep);
        return;
    }
    // start: written when the command processor gets here (slot 0); end: when everything before it is done (slot 1)
    vkCmdWriteBufferMarker2AMD(cmd, start ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, buffer_,
                               start ? 0 : 4, id);
}

void CrashMarkers::begin(VkCommandBuffer cmd, std::uint64_t frame, const char* text) { write(cmd, frame, text, true); }
void CrashMarkers::end(VkCommandBuffer cmd, std::uint64_t frame, const char* text) { write(cmd, frame, text, false); }

std::string CrashMarkers::report() const {
    if (!valid()) return {};
    const std::uint32_t started = mapped_[0], finished = mapped_[1], recorded = next_;
    std::string out = std::format("crash markers: {} events recorded, GPU reached event {} (start markers), finished up to event {} (end markers)",
                                  recorded, started, finished);
    // the events after the last finished one up to the last reached one (and the one after, which may have been in flight)
    const std::uint32_t from = finished > 0 ? finished : 1, to = std::min(recorded, std::max(started, finished) + 1);
    const std::uint32_t first = to > from + 40 ? to - 40 : from;
    if (first > from) out += std::format(" ({} earlier events not listed)", first - from);
    for (std::uint32_t id = first; id <= to; ++id) {
        if (recorded - id >= kEvents) continue;
        const Event& e = events_[id % kEvents];
        out += std::format("\n    event {}: {} {} (frame {}){}", id, e.start ? "start" : "end  ", e.text, e.frame,
                           id == started ? "   <- last start the GPU reached" : id == finished ? "   <- last end" : "");
    }
    return out;
}

}  // namespace rvk::vk
