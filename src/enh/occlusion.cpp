#include "enh/occlusion.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <spdlog/spdlog.h>

#include "shader/abi.h"
#include "shader/embedded.h"

namespace rvk::enh {

namespace {

struct HzbPush {
    shader::abi::PassPushHeader h;
    std::uint32_t srcTex, srcW, srcH, dstW, dstH, pad[3];
};
static_assert(sizeof(HzbPush) == 48);

struct ProbePush {
    shader::abi::PassPushHeader h;
    std::uint64_t spheres;
    std::uint64_t outFlags;
    std::uint32_t count, levels, width, height;
    std::uint32_t hzb[8];
};
static_assert(sizeof(ProbePush) == 80);

}  // namespace

OcclusionProbe::OcclusionProbe(d3d9::Device& device) : dev_(device) {}

OcclusionProbe::~OcclusionProbe() {
    destroyAll();
    for (int l = 0; l < static_cast<int>(kLevels); ++l) dev_.transients().release("occ.hzb" + std::to_string(l));
}

void OcclusionProbe::destroyAll() {
    vk::Context* ctx = &dev_.ctx();
    std::array<std::pair<VkBuffer, VmaAllocation>, vk::Frames::kInFlight> buffers{};
    for (std::size_t i = 0; i < readback_.size(); ++i) buffers[i] = {readback_[i].buffer, readback_[i].alloc};
    const VkPipeline h = hzb_, p = probe_;
    vk::PassLayout layout = layout_;
    dev_.frames().retire([ctx, buffers, h, p, layout]() mutable {
        for (auto& [b, a] : buffers) {
            if (b != VK_NULL_HANDLE) vmaDestroyBuffer(ctx->allocator(), b, a);
        }
        if (h != VK_NULL_HANDLE) vkDestroyPipeline(ctx->device(), h, nullptr);
        if (p != VK_NULL_HANDLE) vkDestroyPipeline(ctx->device(), p, nullptr);
        layout.destroy(*ctx);
    });
    readback_ = {};
    hzb_ = probe_ = VK_NULL_HANDLE;
    layout_ = vk::PassLayout{};
}

bool OcclusionProbe::ensure() {
    if (failed_) return false;
    if (probe_ != VK_NULL_HANDLE) return true;
    vk::Context& ctx = dev_.ctx();
    const VkDescriptorSetLayoutBinding out{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    if (!layout_.create(ctx, dev_.bindless(), {&out, 1})) {
        failed_ = true;
        return false;
    }
    hzb_ = vk::createComputePipeline(ctx, layout_.layout(), shader::embedded::spirv("occlusion/hzb_down.comp"), "occlusion hzb");
    probe_ = vk::createComputePipeline(ctx, layout_.layout(), shader::embedded::spirv("occlusion/probe.comp"), "occlusion probe");
    for (Readback& rb : readback_) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = kMaxSpheres * sizeof(std::uint32_t);
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(ctx.allocator(), &bci, &aci, &rb.buffer, &rb.alloc, &info) != VK_SUCCESS) {
            failed_ = true;
            break;
        }
        rb.mapped = static_cast<std::uint32_t*>(info.pMappedData);
        VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        dai.buffer = rb.buffer;
        rb.address = vkGetBufferDeviceAddress(ctx.device(), &dai);
    }
    if (hzb_ == VK_NULL_HANDLE || probe_ == VK_NULL_HANDLE || failed_) {
        spdlog::error("enh: occlusion probe: pipelines / buffers failed, off for this session");
        failed_ = true;
        destroyAll();
        return false;
    }
    spdlog::info("enh: occlusion probe on (measurement only: {} levels, up to {} spheres)", kLevels, kMaxSpheres);
    return true;
}

// the slot's previous result (its frame has completed: Frames::begin waited for it)
void OcclusionProbe::collect(Readback& rb) {
    if (!rb.pending) return;
    rb.pending = false;
    vmaInvalidateAllocation(dev_.ctx().allocator(), rb.alloc, 0, rb.count * sizeof(std::uint32_t));
    for (std::uint32_t i = 0; i < rb.count; ++i) {
        const std::uint32_t flag = rb.mapped[i];
        const std::uint64_t tris = i < rb.triangles.size() ? rb.triangles[i] : 0;
        ++instances_;
        trianglesAll_ += tris;
        if (flag == 1u) {
            ++hidden_;
            trianglesHidden_ += tris;
        } else if (flag == 2u) {
            ++offscreen_;
        }
    }
    ++frames_;
}

void OcclusionProbe::run(d3d9::PassContext& ctx, std::span<const OccSphere> spheres, std::span<const std::uint32_t> triangles) {
    if (!ensure() || ctx.depthMap == nullptr || spheres.empty()) return;
    const std::uint32_t slot = dev_.frames().currentSlot();
    Readback& rb = readback_[slot];
    collect(rb);
    const double now = dev_.secondsSinceCreate();
    if (frames_ > 0 && now - lastLog_ >= 5.0) {
        lastLog_ = now;
        const double n = static_cast<double>(std::max<std::uint64_t>(instances_, 1));
        spdlog::info("enh: occlusion probe: {} frames, {:.0f} instances per frame: {:.1f} % hidden behind the z pre-pass, {:.1f} % outside the "
                     "screen; triangles: {:.1f} % of the tested ones hidden",
                     frames_, static_cast<double>(instances_) / static_cast<double>(frames_), 100.0 * static_cast<double>(hidden_) / n,
                     100.0 * static_cast<double>(offscreen_) / n,
                     100.0 * static_cast<double>(trianglesHidden_) / static_cast<double>(std::max<std::uint64_t>(trianglesAll_, 1)));
        frames_ = instances_ = hidden_ = offscreen_ = trianglesAll_ = trianglesHidden_ = 0;
    }
    VkCommandBuffer cmd = ctx.cmd;
    if (!dev_.readable(ctx.depthMap, cmd)) return;
    const std::uint32_t w = ctx.depthMap->desc().width, h = ctx.depthMap->desc().height;

    // the pyramid: level l is (w, h) >> (l + 1)
    std::array<vk::Transients::Entry*, kLevels> level{};
    std::uint32_t levels = 0, lw = w, lh = h;
    for (std::uint32_t l = 0; l < kLevels && (lw > 1 || lh > 1); ++l) {
        const std::uint32_t dw = std::max(1u, (lw + 1) / 2), dh = std::max(1u, (lh + 1) / 2);
        level[l] = dev_.transients().get("occ.hzb" + std::to_string(l), {VK_FORMAT_R32_SFLOAT, {dw, dh}});
        if (level[l] == nullptr) return;
        lw = dw;
        lh = dh;
        ++levels;
    }
    if (levels == 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hzb_);
    layout_.bindSet0(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev_.bindless());
    std::uint32_t srcTex = ctx.depthMap->bindlessSlot(), sw = w, sh = h;
    for (std::uint32_t l = 0; l < levels; ++l) {
        const VkExtent2D e = level[l]->desc.extent;
        level[l]->image.transition(cmd, VK_IMAGE_LAYOUT_GENERAL);
        HzbPush push{};
        push.h.frame = ctx.frameBlock;
        push.srcTex = srcTex;
        push.srcW = sw;
        push.srcH = sh;
        push.dstW = e.width;
        push.dstH = e.height;
        layout_.pushStorageImage(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, 0, level[l]->image.view());
        layout_.push(cmd, &push, sizeof(push));
        vkCmdDispatch(cmd, (e.width + 7) / 8, (e.height + 7) / 8, 1);
        level[l]->image.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        srcTex = level[l]->slot;
        sw = e.width;
        sh = e.height;
    }

    // the spheres (the frame's ring) and the test
    const std::uint32_t count = static_cast<std::uint32_t>(std::min<std::size_t>(spheres.size(), kMaxSpheres));
    vk::RingAlloc ring = dev_.frames().allocate(count * sizeof(OccSphere), 16);
    if (ring.ptr == nullptr) return;
    std::memcpy(ring.ptr, spheres.data(), count * sizeof(OccSphere));
    ProbePush push{};
    push.h.frame = ctx.frameBlock;
    push.spheres = ring.address;
    push.outFlags = rb.address;
    push.count = count;
    push.levels = levels;
    push.width = w;
    push.height = h;
    for (std::uint32_t l = 0; l < levels; ++l) push.hzb[l] = level[l]->slot;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, probe_);
    layout_.push(cmd, &push, sizeof(push));
    vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);
    // the pyramid levels are rewritten next frame: their layout stays SHADER_READ_ONLY until then (tracked by the transition)
    rb.count = count;
    rb.triangles.assign(triangles.begin(), triangles.begin() + std::min<std::size_t>(count, triangles.size()));
    rb.pending = true;
}

}  // namespace rvk::enh
