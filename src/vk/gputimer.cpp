#include "vk/gputimer.h"

#include <spdlog/spdlog.h>

#include "vk/context.h"

namespace rvk::vk {

bool GpuTimer::create(Context& ctx, std::uint32_t slots) {
    ctx_ = &ctx;
    if (!ctx.features().hostQueryReset || ctx.features().timestampPeriod <= 0.0f) {
        spdlog::warn("vulkan: GPU timestamps unavailable (hostQueryReset {}, timestamp period {})", ctx.features().hostQueryReset,
                     ctx.features().timestampPeriod);
        return false;
    }
    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = kQueriesPerSlot * slots;
    if (vkCreateQueryPool(ctx.device(), &ci, nullptr, &pool_) != VK_SUCCESS) {
        pool_ = VK_NULL_HANDLE;
        return false;
    }
    vkResetQueryPool(ctx.device(), pool_, 0, ci.queryCount);
    nsPerTick_ = ctx.features().timestampPeriod;
    slots_.resize(slots);
    scratch_.resize(kQueriesPerSlot * 2);
    return true;
}

void GpuTimer::destroy() {
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(ctx_->device(), pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    slots_.clear();
}

GpuTimer::Result GpuTimer::collect(std::uint32_t slot) {
    Result r;
    if (!enabled() || slot >= slots_.size()) {
        return r;
    }
    Slot& s = slots_[slot];
    const auto n = static_cast<std::uint32_t>(s.marks.size());
    if (n >= 2) {
        // value + availability per query
        const VkResult res =
            vkGetQueryPoolResults(ctx_->device(), pool_, slot * kQueriesPerSlot, n, n * 2 * sizeof(std::uint64_t), scratch_.data(),
                                  2 * sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (res == VK_SUCCESS || res == VK_NOT_READY) {
            r.valid = true;
            std::uint64_t first[2] = {0, 0}, last[2] = {0, 0}, prevMain = 0;
            bool seen[2] = {false, false};
            for (std::uint32_t i = 0; i < n; ++i) {
                if (scratch_[i * 2 + 1] == 0) {
                    r.valid = false;  // not written (a submission that never happened) or not available
                    break;
                }
                const std::uint64_t t = scratch_[i * 2];
                const auto k = static_cast<std::size_t>(s.marks[i].stream);
                if (!seen[k]) {
                    first[k] = t;
                    seen[k] = true;
                } else if (s.details && s.marks[i].stream == Stream::Main) {
                    r.segments.push_back({std::move(s.marks[i].label), static_cast<double>(t - prevMain) * nsPerTick_ * 1e-6});
                }
                last[k] = t;
                if (s.marks[i].stream == Stream::Main) prevMain = t;
            }
            if (r.valid) {
                r.mainMs = static_cast<double>(last[0] - first[0]) * nsPerTick_ * 1e-6;
                r.uploadMs = static_cast<double>(last[1] - first[1]) * nsPerTick_ * 1e-6;
            } else {
                r.segments.clear();
            }
        }
    }
    if (n > 0) {
        vkResetQueryPool(ctx_->device(), pool_, slot * kQueriesPerSlot, n);
    }
    s.marks.clear();
    s.details = false;
    return r;
}

void GpuTimer::start(std::uint32_t slot, bool details) {
    if (enabled() && slot < slots_.size()) {
        slots_[slot].details = details;
    }
}

void GpuTimer::write(VkCommandBuffer cmd, std::uint32_t slot, Stream stream, std::string&& label) {
    if (!enabled() || slot >= slots_.size()) {
        return;
    }
    Slot& s = slots_[slot];
    if (s.marks.size() >= kQueriesPerSlot) {
        return;
    }
    const auto q = static_cast<std::uint32_t>(s.marks.size());
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_, slot * kQueriesPerSlot + q);
    s.marks.push_back({q, stream, s.details ? std::move(label) : std::string()});
}

void GpuTimer::mark(VkCommandBuffer cmd, std::uint32_t slot, Stream stream, const char* label) {
    write(cmd, slot, stream, slots_.empty() || !details(slot) ? std::string() : std::string(label));
}

void GpuTimer::mark(VkCommandBuffer cmd, std::uint32_t slot, Stream stream, std::string&& label) {
    write(cmd, slot, stream, std::move(label));
}

}  // namespace rvk::vk
