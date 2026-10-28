// rvk_shadereq: do two pixel shaders compute the same colour? (docs/research/shader-permutations.md 6)
//
//   rvk_shadereq --vs <varyings.vert.spv> --fs <a.spv> <b.spv> [--trials N] [--seed S] [--tol T] [--no-validation]
//   rvk_shadereq --vs-test --vsa <a.spv> <b.spv> --attrs-a <loc:slot,..> --attrs-b <loc:slot,..> --probe <probe.spv> --probe-count N
//                [--trials N] [--seed S] [--tol T] [--billboard-valid]: the vertex shader variant of the check (below); --billboard-valid
//                gives the SpeedTree billboards valid inputs (c_f2PI = 2 pi, 8 images)
//
// Both fragment shaders run in the renderer's draw pipeline (bindless set, buffer-device-address constants, dynamic rendering,
// shader/abi.h push block) behind the same pass-through vertex shader, which outputs the VS constants c[loc] as the varyings
// at locations 0..7 and 16..: a trial = random pixel constants c0..c255, random varyings, random 8x8 textures on every pixel
// sampler; the 2x2 RGBA32F targets are compared (|a - b| <= tol * (1 + max(|a|, |b|)), both NaN = equal). Exit 0 = every trial
// equal. Used with the translated game shader (a) against the GLSL port in shaders/material (b).
//
// --vs-test: two vertex shaders on the same 3 random vertices (a vec4 per attribute *slot*: the --attrs lists map each shader's
// input location to a slot, so differently numbered inputs get the same data) and random VS constants. Both are patched by the
// caller to draw one fixed full-screen triangle and to export their clip position as a varying; the probe fragment shader
// writes N interpolated varyings per pixel into the buffer at `pc.ps` (4x4 pixels), which are compared.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "shader/abi.h"
#include "vk/bindless.h"
#include "vk/context.h"
#include "vk/frames.h"
#include "vk/image.h"
#include "vk/pipeline.h"

using namespace rvk;

namespace {

std::vector<std::uint32_t> readSpv(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    return words;
}

void setDynamicState(VkCommandBuffer cmd, const vk::Context& ctx, std::uint32_t w, std::uint32_t h) {
    const VkViewport vp{0.0f, static_cast<float>(h), static_cast<float>(w), -static_cast<float>(h), 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewportWithCount(cmd, 1, &vp);
    vkCmdSetScissorWithCount(cmd, 1, &sc);
    vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetDepthTestEnable(cmd, VK_FALSE);
    vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
    vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(cmd, VK_FALSE);
    vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
    vkCmdSetDepthBiasEnable(cmd, VK_FALSE);
    const float blend[4] = {0, 0, 0, 0};
    vkCmdSetBlendConstants(cmd, blend);
    vkCmdSetPrimitiveRestartEnable(cmd, VK_FALSE);
    if (ctx.features().extendedDynamicState3) {
        const VkBool32 off = VK_FALSE;
        vkCmdSetColorBlendEnableEXT(cmd, 0, 1, &off);
        const VkColorBlendEquationEXT eq{VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
                                         VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
        vkCmdSetColorBlendEquationEXT(cmd, 0, 1, &eq);
        const VkColorComponentFlags mask = 0xF;
        vkCmdSetColorWriteMaskEXT(cmd, 0, 1, &mask);
    }
}

constexpr std::uint32_t kSize = 2;
constexpr std::uint32_t kTextures = 8;
constexpr std::uint32_t kTexSize = 8;

// ---- --vs-test ------------------------------------------------------------------------------------------------------------------

constexpr std::uint32_t kSlots = 24;  // vec4 attribute slots per vertex (tools/shader_equiv.py: SLOT)
constexpr std::uint32_t kProbeSize = 4;

struct AttrMap {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;  // location, slot
};

AttrMap parseAttrs(const std::string& s) {
    AttrMap m;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t c = s.find(',', i);
        const std::string item = s.substr(i, c == std::string::npos ? std::string::npos : c - i);
        const std::size_t colon = item.find(':');
        if (colon != std::string::npos) {
            m.pairs.emplace_back(static_cast<std::uint32_t>(std::atoi(item.c_str())), static_cast<std::uint32_t>(std::atoi(item.c_str() + colon + 1)));
        }
        if (c == std::string::npos) break;
        i = c + 1;
    }
    return m;
}

int vsTest(const std::string& vsA, const std::string& vsB, const AttrMap& mapA, const AttrMap& mapB, const std::string& probePath,
           std::uint32_t probeCount, int trials, std::uint32_t seed, float tol, bool validation, bool billboardValid) {
    vk::Context ctx;
    if (!ctx.create({validation, -1, "rvk_shadereq"})) {
        return 2;
    }
    vk::Frames frames;
    vk::Bindless bindless;
    vk::PipelineCache pipes;
    if (!frames.create(ctx) || !bindless.create(ctx, frames) || !pipes.create(ctx, bindless.pipelineLayout(), {})) {
        return 2;
    }
    VkShaderModule vs[2] = {vk::createShaderModule(ctx, readSpv(vsA)), vk::createShaderModule(ctx, readSpv(vsB))};
    VkShaderModule probe = vk::createShaderModule(ctx, readSpv(probePath));
    if (!vs[0] || !vs[1] || !probe) {
        return 2;
    }
    vk::Image rt;
    rt.create(ctx, {VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {kProbeSize, kProbeSize, 1}, 1, 1, false,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, {}, "shadereq probe target"});

    const AttrMap* maps[2] = {&mapA, &mapB};
    vk::PipelineDesc pd[2]{};
    VkPipeline pipeline[2] = {};
    for (int i = 0; i < 2; ++i) {
        pd[i].vs = vs[i];
        pd[i].fs = probe;
        pd[i].colorCount = 1;
        pd[i].colorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
        pd[i].bindingCount = 1;
        pd[i].bindings[0] = {kSlots * 16, 0};
        pd[i].attributeCount = static_cast<std::uint32_t>(maps[i]->pairs.size());
        for (std::size_t k = 0; k < maps[i]->pairs.size(); ++k) {
            pd[i].attributes[k] = {maps[i]->pairs[k].first, 0, VK_FORMAT_R32G32B32A32_SFLOAT, maps[i]->pairs[k].second * 16};
        }
        pipeline[i] = pipes.get(pd[i]);
        if (pipeline[i] == VK_NULL_HANDLE) {
            std::printf("pipeline %d failed\n", i);
            return 2;
        }
    }

    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> uni(0.05f, 1.0f);
    int badTrials = 0;
    float worst = 0.0f;
    const std::uint32_t probeBytes = kProbeSize * kProbeSize * probeCount * 16;
    for (int trial = 0; trial < trials; ++trial) {
        frames.begin();
        VkCommandBuffer cmd = frames.cmd();
        if (trial == 0) bindless.recordInit(cmd);
        vk::RingAlloc vsC = frames.allocate(shader::abi::kStageConstsSize, 16);
        std::memset(vsC.ptr, 0, shader::abi::kStageConstsSize);
        auto* vc = static_cast<float*>(vsC.ptr);
        for (int i = 0; i < 256 * 4; ++i) vc[i] = uni(rng);
        if (billboardValid) vc[32 * 4 + 2] = 6.2831853f;  // c_f2PI
        vk::RingAlloc db = frames.allocate(sizeof(shader::abi::DrawBlock), 16);
        shader::abi::DrawBlock block{};
        block.alphaFunc = 8;
        std::memcpy(db.ptr, &block, sizeof(block));

        // 3 vertices x kSlots vec4: slot 0 position (w = corner index 0..3), 9 instance index, 18 blend indices (0..2), the rest uniform
        vk::RingAlloc vb = frames.allocate(3 * kSlots * 16, 16);
        auto* vd = static_cast<float*>(vb.ptr);
        for (std::uint32_t v = 0; v < 3; ++v) {
            for (std::uint32_t sl = 0; sl < kSlots; ++sl) {
                float* q = vd + (v * kSlots + sl) * 4;
                for (int c = 0; c < 4; ++c) q[c] = uni(rng);
                if (sl == 0) q[3] = static_cast<float>(rng() % 4);
                if (sl == 9) q[0] = static_cast<float>(rng() % 3);
                if (sl == 18) {
                    for (int c = 0; c < 4; ++c) q[c] = static_cast<float>(rng() % 3);
                }
                if (billboardValid && sl == 19) {  // SpeedTree billboard: texcoord offset, image count, tree azimuth, slice = 2 pi / count
                    q[0] = 0.0f;
                    q[1] = 8.0f;
                    q[2] = uni(rng) * 6.28f;
                    q[3] = 6.2831853f / 8.0f;
                }
            }
        }
        vk::RingAlloc out[2] = {frames.allocate(probeBytes, 16), frames.allocate(probeBytes, 16)};
        if (const char* dump = std::getenv("RVK_EQ_DUMP"); dump != nullptr && std::atoi(dump) == trial) {
            for (std::uint32_t v = 0; v < 3; ++v) {
                std::printf("vertex %u:", v);
                for (std::uint32_t sl = 0; sl < kSlots; ++sl) {
                    const float* q = vd + (v * kSlots + sl) * 4;
                    std::printf(" [%u: %.4f %.4f %.4f %.4f]", sl, q[0], q[1], q[2], q[3]);
                }
                std::putchar(10);
            }
            for (int r : {4, 11, 32, 33, 34, 35, 36}) std::printf("c%d = %.5f %.5f %.5f %.5f%c", r, vc[r * 4], vc[r * 4 + 1], vc[r * 4 + 2], vc[r * 4 + 3], 10);
        }

        for (int i = 0; i < 2; ++i) {
            std::memset(out[i].ptr, 0, probeBytes);
            rt.transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            color.imageView = rt.view();
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {kProbeSize, kProbeSize}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &color;
            vkCmdBeginRendering(cmd, &ri);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline[i]);
            const VkDescriptorSet set = bindless.set();
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bindless.pipelineLayout(), 0, 1, &set, 0, nullptr);
            shader::abi::PushConstants push{vsC.address, out[i].address, db.address, 0, 0, 0, 0};
            push.legacyWindows();
            vkCmdPushConstants(cmd, bindless.pipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                               &push);
            setDynamicState(cmd, ctx, kProbeSize, kProbeSize);
            if (ctx.features().vertexInputDynamicState) {
                const VkVertexInputBindingDescription2EXT b{VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr, 0,
                                                            kSlots * 16, VK_VERTEX_INPUT_RATE_VERTEX, 1};
                std::vector<VkVertexInputAttributeDescription2EXT> at;
                for (const auto& [loc, slot] : maps[i]->pairs) {
                    at.push_back({VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, nullptr, loc, 0, VK_FORMAT_R32G32B32A32_SFLOAT, slot * 16});
                }
                vkCmdSetVertexInputEXT(cmd, 1, &b, static_cast<std::uint32_t>(at.size()), at.data());
            }
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb.buffer, &vb.offset);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
        }
        frames.submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
        frames.waitIdle();

        const auto* ra = static_cast<const float*>(out[0].ptr);
        const auto* rb = static_cast<const float*>(out[1].ptr);
        bool bad = false;
        float trialWorst = 0.0f;
        std::uint32_t firstBad = ~0u, covered = 0, badVaryings = 0;
        for (std::uint32_t i = 0; i < probeBytes / 4; ++i) {
            const bool na = std::isnan(ra[i]), nb = std::isnan(rb[i]);
            if (na || nb) {
                if (na != nb) {
                    bad = true;
                    if (firstBad == ~0u) firstBad = i;
                }
                continue;
            }
            if (ra[i] != 0.0f) ++covered;
            const float d = std::fabs(ra[i] - rb[i]) / (1.0f + std::fmax(std::fabs(ra[i]), std::fabs(rb[i])));
            trialWorst = std::fmax(trialWorst, d);
            if (d > tol) {
                bad = true;
                if (firstBad == ~0u) firstBad = i;
                badVaryings |= 1u << ((i / 4) % probeCount);
            }
        }
        worst = std::fmax(worst, trialWorst);
        if (const char* dump = std::getenv("RVK_EQ_DUMP"); dump != nullptr && std::atoi(dump) == trial) {
            for (std::uint32_t px = 0; px < kProbeSize * kProbeSize; ++px) {
                std::printf("pix %u:", px);
                for (std::uint32_t c = 0; c < 4; ++c) std::printf(" %.6f/%.6f", ra[(px * probeCount) * 4 + c], rb[(px * probeCount) * 4 + c]);  // probe 0, all components
                std::putchar(10);
            }
            for (std::uint32_t k = 0; k < probeCount; ++k) {
                std::printf("probe %u px0: a =%.6f %.6f %.6f %.6f   b = %.6f %.6f %.6f %.6f%c", k, ra[k * 4], ra[k * 4 + 1], ra[k * 4 + 2], ra[k * 4 + 3],
                            rb[k * 4], rb[k * 4 + 1], rb[k * 4 + 2], rb[k * 4 + 3], 10);
            }
        }
        if (covered == 0) {
            std::printf("trial %d: nothing was rasterised (the probe wrote no values)\n", trial);
            bad = true;
        }
        if (bad) {
            ++badTrials;
            std::printf("trial %d DIFFERS (worst relative %.3g)", trial, trialWorst);
            if (firstBad != ~0u) {
                const std::uint32_t px = firstBad / (probeCount * 4), loc = (firstBad / 4) % probeCount, comp = firstBad % 4;
                std::printf(": pixel %u probe-varying %u component %u: a = %.6f, b = %.6f", px, loc, comp, ra[firstBad], rb[firstBad]);
            }
            std::printf("; differing probe-varyings mask 0x%x\n", badVaryings);
        }
    }
    frames.retire([&] {});
    rt.destroy(ctx);
    vkDestroyShaderModule(ctx.device(), vs[0], nullptr);
    vkDestroyShaderModule(ctx.device(), vs[1], nullptr);
    vkDestroyShaderModule(ctx.device(), probe, nullptr);
    pipes.destroy();
    bindless.destroy();
    frames.destroy();
    const std::uint32_t verrors = ctx.validationErrors();
    ctx.destroy();
    std::printf("%s: %d of %d trials differ (worst relative difference %.3g), %u validation error(s)\n",
                badTrials == 0 && verrors == 0 ? "EQUAL" : "DIFFERENT", badTrials, trials, worst, verrors);
    return badTrials == 0 && verrors == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string vsPath, fsA, fsB, vsA, vsB, attrsA, attrsB, probePath;
    bool vsTestMode = false, billboardValid = false;
    std::uint32_t probeCount = 0;
    int trials = 8;
    std::uint32_t seed = 1;
    float tol = 1e-4f;
    bool validation = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--vs" && i + 1 < argc) {
            vsPath = argv[++i];
        } else if (a == "--fs" && i + 2 < argc) {
            fsA = argv[++i];
            fsB = argv[++i];
        } else if (a == "--trials" && i + 1 < argc) {
            trials = std::atoi(argv[++i]);
        } else if (a == "--seed" && i + 1 < argc) {
            seed = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (a == "--tol" && i + 1 < argc) {
            tol = static_cast<float>(std::atof(argv[++i]));
        } else if (a == "--no-validation") {
            validation = false;
        } else if (a == "--vs-test") {
            vsTestMode = true;
        } else if (a == "--billboard-valid") {
            billboardValid = true;
        } else if (a == "--vsa" && i + 2 < argc) {
            vsA = argv[++i];
            vsB = argv[++i];
        } else if (a == "--attrs-a" && i + 1 < argc) {
            attrsA = argv[++i];
        } else if (a == "--attrs-b" && i + 1 < argc) {
            attrsB = argv[++i];
        } else if (a == "--probe" && i + 1 < argc) {
            probePath = argv[++i];
        } else if (a == "--probe-count" && i + 1 < argc) {
            probeCount = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        }
    }
    if (vsTestMode) {
        return vsTest(vsA, vsB, parseAttrs(attrsA), parseAttrs(attrsB), probePath, probeCount, trials, seed, tol, validation, billboardValid);
    }
    if (vsPath.empty() || fsA.empty()) {
        std::printf("usage: rvk_shadereq --vs <varyings.spv> --fs <a.spv> <b.spv> [--trials N] [--seed S] [--tol T]\n");
        return 2;
    }
    vk::Context ctx;
    if (!ctx.create({validation, -1, "rvk_shadereq"})) {
        return 2;
    }
    vk::Frames frames;
    vk::Bindless bindless;
    vk::PipelineCache pipes;
    if (!frames.create(ctx) || !bindless.create(ctx, frames) || !pipes.create(ctx, bindless.pipelineLayout(), {})) {
        return 2;
    }
    const auto vsWords = readSpv(vsPath), aWords = readSpv(fsA), bWords = readSpv(fsB);
    VkShaderModule vs = vk::createShaderModule(ctx, vsWords);
    VkShaderModule fa = vk::createShaderModule(ctx, aWords);
    VkShaderModule fb = vk::createShaderModule(ctx, bWords);
    if (!vs || !fa || !fb) {
        return 2;
    }

    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> uni(0.05f, 1.0f);

    // textures: random RGBA8, linear / clamp
    vk::Image tex[kTextures];
    for (auto& t : tex) {
        t.create(ctx, {VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {kTexSize, kTexSize, 1}, 1, 1, false,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, {}, "shadereq texture"});
    }
    VkSampler sampler = VK_NULL_HANDLE;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    vkCreateSampler(ctx.device(), &sci, nullptr, &sampler);
    const std::uint32_t samplerSlot = bindless.addSampler(sampler);
    std::uint32_t texSlot[kTextures];
    for (std::uint32_t i = 0; i < kTextures; ++i) {
        texSlot[i] = bindless.addTexture(vk::TextureKind::Tex2D, tex[i].view());
    }

    vk::Image rt[2];
    for (auto& r : rt) {
        r.create(ctx, {VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, {kSize, kSize, 1}, 1, 1, false,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, {}, "shadereq target"});
    }
    VkBuffer readback[2] = {};
    VmaAllocation readbackAlloc[2] = {};
    VmaAllocationInfo readbackInfo[2] = {};
    for (int i = 0; i < 2; ++i) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = kSize * kSize * 16;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        vmaCreateBuffer(ctx.allocator(), &bci, &aci, &readback[i], &readbackAlloc[i], &readbackInfo[i]);
    }

    vk::PipelineDesc pd[2]{};
    VkPipeline pipeline[2] = {};
    const VkShaderModule fs[2] = {fa, fb};
    for (int i = 0; i < 2; ++i) {
        pd[i].vs = vs;
        pd[i].fs = fs[i];
        pd[i].colorCount = 1;
        pd[i].colorFormats[0] = VK_FORMAT_R32G32B32A32_SFLOAT;
        pipeline[i] = pipes.get(pd[i]);
        if (pipeline[i] == VK_NULL_HANDLE) {
            std::printf("pipeline %d failed (interface mismatch between the vertex and fragment shader?)\n", i);
            return 2;
        }
    }

    int badTrials = 0;
    float worst = 0.0f;
    for (int trial = 0; trial < trials; ++trial) {
        frames.begin();
        VkCommandBuffer cmd = frames.cmd();
        if (trial == 0) {
            bindless.recordInit(cmd);
            for (auto& t : tex) {
                vk::RingAlloc staging = frames.allocate(kTexSize * kTexSize * 4, 16);
                auto* px = static_cast<std::uint8_t*>(staging.ptr);
                for (std::uint32_t i = 0; i < kTexSize * kTexSize * 4; ++i) {
                    px[i] = static_cast<std::uint8_t>(rng() & 0xff);
                }
                t.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy region{};
                region.bufferOffset = staging.offset;
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {kTexSize, kTexSize, 1};
                vkCmdCopyBufferToImage(cmd, staging.buffer, t.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                t.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
        vk::RingAlloc vsC = frames.allocate(shader::abi::kStageConstsSize, 16);
        vk::RingAlloc psC = frames.allocate(shader::abi::kStageConstsSize, 16);
        std::memset(vsC.ptr, 0, shader::abi::kStageConstsSize);
        std::memset(psC.ptr, 0, shader::abi::kStageConstsSize);
        auto* vc = static_cast<float*>(vsC.ptr);
        auto* pc = static_cast<float*>(psC.ptr);
        for (int i = 0; i < 256 * 4; ++i) {
            vc[i] = uni(rng);
            pc[i] = uni(rng);
        }
        vk::RingAlloc db = frames.allocate(sizeof(shader::abi::DrawBlock), 16);
        shader::abi::DrawBlock block{};
        for (std::uint32_t s = 0; s < shader::abi::kSamplerSlots; ++s) {
            block.tex[s] = texSlot[s % kTextures];
            block.smp[s] = samplerSlot;
        }
        block.alphaFunc = 8;  // always
        std::memcpy(db.ptr, &block, sizeof(block));

        shader::abi::PushConstants push{vsC.address, psC.address, db.address, 0, 0, 0, 0};

        push.legacyWindows();
        for (int i = 0; i < 2; ++i) {
            rt[i].transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            color.imageView = rt[i].view();
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = {{-7.0f, -7.0f, -7.0f, -7.0f}};  // marks discarded pixels
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {kSize, kSize}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &color;
            vkCmdBeginRendering(cmd, &ri);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline[i]);
            const VkDescriptorSet set = bindless.set();
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bindless.pipelineLayout(), 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(cmd, bindless.pipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                               &push);
            setDynamicState(cmd, ctx, kSize, kSize);
            if (ctx.features().vertexInputDynamicState) {
                vkCmdSetVertexInputEXT(cmd, 0, nullptr, 0, nullptr);
            }
            vkCmdDraw(cmd, 3, 1, 0, 0);  // one big triangle (the pass-through VS places it)
            vkCmdEndRendering(cmd);
            rt[i].transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy back{};
            back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            back.imageExtent = {kSize, kSize, 1};
            vkCmdCopyImageToBuffer(cmd, rt[i].image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback[i], 1, &back);
        }
        frames.submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
        frames.waitIdle();

        const auto* ra = static_cast<const float*>(readbackInfo[0].pMappedData);
        const auto* rb = static_cast<const float*>(readbackInfo[1].pMappedData);
        bool bad = false;
        float trialWorst = 0.0f;
        int discarded = 0;
        for (std::uint32_t i = 0; i < kSize * kSize * 4; ++i) {
            const bool na = std::isnan(ra[i]), nb = std::isnan(rb[i]);
            if (na || nb) {
                if (na != nb) bad = true;
                continue;
            }
            if (ra[i] == -7.0f && rb[i] == -7.0f) {
                ++discarded;
                continue;
            }
            const float d = std::fabs(ra[i] - rb[i]) / (1.0f + std::fmax(std::fabs(ra[i]), std::fabs(rb[i])));
            trialWorst = std::fmax(trialWorst, d);
            if (d > tol) bad = true;
        }
        worst = std::fmax(worst, trialWorst);
        if (bad) {
            ++badTrials;
            std::printf("trial %d DIFFERS (worst relative %.3g):\n", trial, trialWorst);
            for (int p = 0; p < 2; ++p) {
                std::printf("  px%d a = %.5f %.5f %.5f %.5f   b = %.5f %.5f %.5f %.5f\n", p, ra[p * 4], ra[p * 4 + 1], ra[p * 4 + 2],
                            ra[p * 4 + 3], rb[p * 4], rb[p * 4 + 1], rb[p * 4 + 2], rb[p * 4 + 3]);
            }
        } else if (trial == 0) {
            std::printf("trial 0: a = %.5f %.5f %.5f %.5f   b = %.5f %.5f %.5f %.5f%s\n", ra[0], ra[1], ra[2], ra[3], rb[0], rb[1], rb[2],
                        rb[3], discarded ? "  (discarded)" : "");
        }
    }

    for (int i = 0; i < 2; ++i) {
        vmaDestroyBuffer(ctx.allocator(), readback[i], readbackAlloc[i]);
        rt[i].destroy(ctx);
    }
    frames.retire([&] {});
    for (auto& t : tex) t.destroy(ctx);
    vkDestroySampler(ctx.device(), sampler, nullptr);
    vkDestroyShaderModule(ctx.device(), vs, nullptr);
    vkDestroyShaderModule(ctx.device(), fa, nullptr);
    vkDestroyShaderModule(ctx.device(), fb, nullptr);
    pipes.destroy();
    bindless.destroy();
    frames.destroy();
    const std::uint32_t verrors = ctx.validationErrors();
    ctx.destroy();
    std::printf("%s: %d of %d trials differ (worst relative difference %.3g), %u validation error(s)\n",
                badTrials == 0 && verrors == 0 ? "EQUAL" : "DIFFERENT", badTrials, trials, worst, verrors);
    return badTrials == 0 && verrors == 0 ? 0 : 1;
}
