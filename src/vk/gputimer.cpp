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

bool GpuTimer::createPassStats(Context& ctx, std::uint32_t slots) {
    ctx_ = &ctx;
    if (!ctx.features().passStats || !ctx.features().hostQueryReset) return false;
    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
    ci.queryCount = kPassQueriesPerSlot * slots;
    ci.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT | VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                            VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                            VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
    if (vkCreateQueryPool(ctx.device(), &ci, nullptr, &statPool_) != VK_SUCCESS) {
        statPool_ = VK_NULL_HANDLE;
        return false;
    }
    VkQueryPoolCreateInfo oi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    oi.queryType = VK_QUERY_TYPE_OCCLUSION;
    oi.queryCount = kPassQueriesPerSlot * slots;
    if (vkCreateQueryPool(ctx.device(), &oi, nullptr, &occPool_) != VK_SUCCESS) {
        vkDestroyQueryPool(ctx.device(), statPool_, nullptr);
        statPool_ = occPool_ = VK_NULL_HANDLE;
        return false;
    }
    vkResetQueryPool(ctx.device(), statPool_, 0, ci.queryCount);
    vkResetQueryPool(ctx.device(), occPool_, 0, oi.queryCount);
    passCount_.assign(slots, 0);
    passCalls_.assign(std::size_t{kPassQueriesPerSlot} * slots * 4, 0);
    spdlog::info("vulkan: RVK_PASS_STATS: per-pass pipeline statistics + occlusion queries on (timings with it on are not the shipped ones)");
    return true;
}

void GpuTimer::passBegin(VkCommandBuffer cmd, std::uint32_t slot) {
    if (statPool_ == VK_NULL_HANDLE || slot >= passCount_.size() || passCount_[slot] >= kPassQueriesPerSlot) return;
    const std::uint32_t q = slot * kPassQueriesPerSlot + passCount_[slot];
    vkCmdBeginQuery(cmd, statPool_, q, 0);
    vkCmdBeginQuery(cmd, occPool_, q, VK_QUERY_CONTROL_PRECISE_BIT);
}

void GpuTimer::passEnd(VkCommandBuffer cmd, std::uint32_t slot, const std::uint64_t calls[4]) {
    if (statPool_ == VK_NULL_HANDLE || slot >= passCount_.size() || passCount_[slot] >= kPassQueriesPerSlot) return;
    const std::uint32_t q = slot * kPassQueriesPerSlot + passCount_[slot]++;
    for (int i = 0; i < 4; ++i) passCalls_[std::size_t{q} * 4 + i] = calls[i];
    vkCmdEndQuery(cmd, occPool_, q);
    vkCmdEndQuery(cmd, statPool_, q);
}

void GpuTimer::destroy() {
    if (statPool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(ctx_->device(), statPool_, nullptr);
        vkDestroyQueryPool(ctx_->device(), occPool_, nullptr);
        statPool_ = occPool_ = VK_NULL_HANDLE;
    }
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
    if (statPool_ != VK_NULL_HANDLE && slot < passCount_.size() && passCount_[slot] > 0) {
        const std::uint32_t k = passCount_[slot];
        std::vector<std::uint64_t> st(std::size_t{k} * 6), oc(k);  // (5 statistics + availability per query; sample count)
        const VkResult a = vkGetQueryPoolResults(ctx_->device(), statPool_, slot * kPassQueriesPerSlot, k, st.size() * 8, st.data(), 6 * 8,
                                                 VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        const VkResult b = vkGetQueryPoolResults(ctx_->device(), occPool_, slot * kPassQueriesPerSlot, k, oc.size() * 8, oc.data(), 8,
                                                 VK_QUERY_RESULT_64_BIT);
        if (r.valid && !r.segments.empty() && (a == VK_SUCCESS || a == VK_NOT_READY) && b == VK_SUCCESS) {
            std::uint32_t next = 0;
            for (Segment& sg : r.segments) {
                if (!sg.label.starts_with("pass ") || sg.label.size() < 6 || sg.label[5] < '0' || sg.label[5] > '9' || next >= k) continue;  // (the engine's scopes "pass N: ...", not the injected "pass <name> at <point>")
                const std::uint64_t* v = &st[std::size_t{next} * 6];
                if (v[5] != 0) {
                    sg.stat = {true, v[0], v[1], v[2], v[3], v[4], oc[next] >= (1ull << 62) ? 0 : oc[next], {}};  // (all ones = the counter returned nothing for that scope)
                    for (int i = 0; i < 4; ++i) sg.stat.calls[i] = passCalls_[std::size_t{slot * kPassQueriesPerSlot + next} * 4 + i];
                }
                ++next;
            }
        }
        vkResetQueryPool(ctx_->device(), statPool_, slot * kPassQueriesPerSlot, k);
        vkResetQueryPool(ctx_->device(), occPool_, slot * kPassQueriesPerSlot, k);
        passCount_[slot] = 0;
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
