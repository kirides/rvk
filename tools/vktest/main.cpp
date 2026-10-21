// rvk_vktest: GPU smoke test of the Vulkan core and the shader contract, without the game.
//
//   rvk_vktest [out.png] [--no-validation]
//
// Off-screen: assembles a D3D9 vs_3_0/ps_3_0 pair (D3DXAssembleShader), translates it (shader/glsl), renders a full-screen
// quad that samples a 64x64 gradient with linear filtering into a 64x64 target through the bindless set, buffer-device-address
// constants and a dynamic-rendering pipeline, reads the pixels back and checks them against D3D9's rasterisation rules
// (pixel centres on integers: the half-pixel fix-up must give 4i-2, not 4i). Exit code 0 = pass, no validation errors.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "common/d3dx.h"
#include "shader/abi.h"
#include "shader/glsl.h"
#include "shader/sm_ir.h"
#include "shader/spirv.h"
#include "vk/bindless.h"
#include "vk/context.h"
#include "vk/frames.h"
#include "vk/image.h"
#include "vk/pipeline.h"

using namespace rvk;

namespace {

constexpr std::uint32_t kSize = 64;

const char* kVs = R"(
vs_3_0
dcl_position v0
dcl_texcoord v1
dcl_position o0
dcl_texcoord o1
m4x4 o0, v0, c0
mov o1, v1
)";

const char* kPs = R"(
ps_3_0
dcl_texcoord v0.xy
dcl_2d s0
texld r0, v0, s0
mul oC0, r0, c0
)";

VkShaderModule translate(vk::Context& ctx, const char* src) {
    std::string errors;
    const auto tokens = devtools::D3DX::get().assemble(src, &errors);
    if (tokens.empty()) {
        spdlog::error("assemble: {}", errors);
        return VK_NULL_HANDLE;
    }
    const auto dec = shader::decode(tokens);
    if (!dec.ok()) {
        spdlog::error("decode: {}", dec.error);
        return VK_NULL_HANDLE;
    }
    const auto g = shader::toGlsl(dec.program);
    if (!g.ok()) {
        spdlog::error("glsl: {}", g.error);
        return VK_NULL_HANDLE;
    }
    const auto s = shader::compileGlsl(dec.program.version.stage, g.glsl);
    if (!s.ok()) {
        spdlog::error("glslang: {}", s.log);
        return VK_NULL_HANDLE;
    }
    return vk::createShaderModule(ctx, s.words);
}

void setDynamicState(VkCommandBuffer cmd, const vk::Context& ctx, std::uint32_t w, std::uint32_t h) {
    // D3D9 y-down screen space with a y-up clip space: flip through a negative viewport height
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

}  // namespace

int main(int argc, char** argv) {
    std::string out = "vktest.png";
    bool validation = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-validation") {
            validation = false;
        } else {
            out = a;
        }
    }
    if (!devtools::D3DX::get().available()) {
        std::printf("d3dx9_39.dll not available\n");
        return 2;
    }
    vk::Context ctx;
    if (!ctx.create({validation, -1, "rvk_vktest"})) {
        return 2;
    }
    vk::Frames frames;
    vk::Bindless bindless;
    vk::PipelineCache pipes;
    if (!frames.create(ctx) || !bindless.create(ctx, frames) || !pipes.create(ctx, bindless.pipelineLayout(), {})) {
        return 2;
    }
    VkShaderModule vs = translate(ctx, kVs);
    VkShaderModule fs = translate(ctx, kPs);
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        return 2;
    }

    vk::Image tex;
    vk::Image rt;
    tex.create(ctx, {VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {kSize, kSize, 1}, 1, 1, false,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, {}, "vktest gradient"});
    rt.create(ctx, {VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {kSize, kSize, 1}, 1, 1, false,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, {}, "vktest target"});
    VkSampler sampler = VK_NULL_HANDLE;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(ctx.device(), &sci, nullptr, &sampler);
    const std::uint32_t samplerSlot = bindless.addSampler(sampler);
    const std::uint32_t texSlot = bindless.addTexture(vk::TextureKind::Tex2D, tex.view());

    VkBuffer readback = VK_NULL_HANDLE;
    VmaAllocation readbackAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo readbackInfo{};
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = kSize * kSize * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        vmaCreateBuffer(ctx.allocator(), &bci, &aci, &readback, &readbackAlloc, &readbackInfo);
    }

    frames.begin();
    VkCommandBuffer cmd = frames.cmd();
    bindless.recordInit(cmd);

    // gradient: r = 4x, g = 4y, b = 255, a = 255
    vk::RingAlloc staging = frames.allocate(kSize * kSize * 4, 16);
    auto* px = static_cast<std::uint8_t*>(staging.ptr);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            std::uint8_t* p = px + (y * kSize + x) * 4;
            p[0] = static_cast<std::uint8_t>(4 * x);
            p[1] = static_cast<std::uint8_t>(4 * y);
            p[2] = 255;
            p[3] = 255;
        }
    }
    tex.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.bufferOffset = staging.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kSize, kSize, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, tex.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    tex.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // constants: VS c0..c3 = identity (m4x4), PS c0 = tint (1, 1, 0.5, 1)
    vk::RingAlloc vsC = frames.allocate(shader::abi::kStageConstsSize, 16);
    vk::RingAlloc psC = frames.allocate(shader::abi::kStageConstsSize, 16);
    std::memset(vsC.ptr, 0, shader::abi::kStageConstsSize);
    std::memset(psC.ptr, 0, shader::abi::kStageConstsSize);
    auto* vc = static_cast<float*>(vsC.ptr);
    for (int r = 0; r < 4; ++r) {
        vc[r * 4 + r] = 1.0f;
    }
    const float tint[4] = {1.0f, 1.0f, 0.5f, 1.0f};
    std::memcpy(psC.ptr, tint, sizeof(tint));
    vk::RingAlloc db = frames.allocate(sizeof(shader::abi::DrawBlock), 16);
    shader::abi::DrawBlock block{};
    block.tex[0] = texSlot;
    block.smp[0] = samplerSlot;
    block.posFix[0] = 1.0f / kSize;
    block.posFix[1] = -1.0f / kSize;
    block.alphaFunc = 8;
    std::memcpy(db.ptr, &block, sizeof(block));

    // full-screen quad: D3D9 clip space (y up), texcoord (0,0) at the top left
    struct Vertex {
        float x, y, z, w, u, v;
    };
    const Vertex quad[6] = {{-1, 1, 0, 1, 0, 0}, {1, 1, 0, 1, 1, 0}, {-1, -1, 0, 1, 0, 1},
                            {1, 1, 0, 1, 1, 0},  {1, -1, 0, 1, 1, 1}, {-1, -1, 0, 1, 0, 1}};
    vk::RingAlloc vb = frames.allocate(sizeof(quad), 16);
    std::memcpy(vb.ptr, quad, sizeof(quad));

    vk::PipelineDesc pd{};
    pd.vs = vs;
    pd.fs = fs;
    pd.colorCount = 1;
    pd.colorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    pd.bindingCount = 1;
    pd.bindings[0] = {sizeof(Vertex), 0};
    pd.attributeCount = 2;
    pd.attributes[0] = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    pd.attributes[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, 16};
    VkPipeline pipeline = pipes.get(pd);
    if (pipeline == VK_NULL_HANDLE) {
        return 2;
    }

    rt.transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = rt.view();
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{1.0f, 0.0f, 1.0f, 1.0f}};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {kSize, kSize}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDescriptorSet set = bindless.set();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bindless.pipelineLayout(), 0, 1, &set, 0, nullptr);
    const shader::abi::PushConstants push{vsC.address, psC.address, db.address};
    vkCmdPushConstants(cmd, bindless.pipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    setDynamicState(cmd, ctx, kSize, kSize);
    if (ctx.features().vertexInputDynamicState) {
        const VkVertexInputBindingDescription2EXT b{VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr, 0,
                                                    sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX, 1};
        const VkVertexInputAttributeDescription2EXT a[2] = {
            {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, nullptr, 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
            {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, nullptr, 1, 0, VK_FORMAT_R32G32_SFLOAT, 16}};
        vkCmdSetVertexInputEXT(cmd, 1, &b, 2, a);
    }
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb.buffer, &vb.offset);
    vkCmdDraw(cmd, 6, 1, 0, 0);
    vkCmdEndRendering(cmd);

    rt.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy back{};
    back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    back.imageExtent = {kSize, kSize, 1};
    vkCmdCopyImageToBuffer(cmd, rt.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &back);
    frames.submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    frames.waitIdle();

    const auto* result = static_cast<const std::uint8_t*>(readbackInfo.pMappedData);
    stbi_write_png(out.c_str(), kSize, kSize, 4, result, kSize * 4);
    int bad = 0;
    auto expect = [](std::uint32_t i) { return i == 0 ? 0 : static_cast<int>(4 * i) - 2; };  // D3D9: halfway between texels i-1, i
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const std::uint8_t* p = result + (y * kSize + x) * 4;
            const bool ok = std::abs(p[0] - expect(x)) <= 1 && std::abs(p[1] - expect(y)) <= 1 && std::abs(p[2] - 128) <= 1 && p[3] == 255;
            if (!ok && bad++ < 5) {
                std::printf("pixel (%u,%u) = (%d,%d,%d,%d), expected (%d,%d,128,255)\n", x, y, p[0], p[1], p[2], p[3], expect(x),
                            expect(y));
            }
        }
    }

    vmaDestroyBuffer(ctx.allocator(), readback, readbackAlloc);
    frames.retire([&] {});
    tex.destroy(ctx);
    rt.destroy(ctx);
    vkDestroySampler(ctx.device(), sampler, nullptr);
    vkDestroyShaderModule(ctx.device(), vs, nullptr);
    vkDestroyShaderModule(ctx.device(), fs, nullptr);
    pipes.destroy();
    bindless.destroy();
    frames.destroy();
    const std::uint32_t verrors = ctx.validationErrors();
    ctx.destroy();
    std::printf("%s: %d bad pixel(s), %u validation error(s), image %s\n", bad == 0 && verrors == 0 ? "PASS" : "FAIL", bad, verrors,
                out.c_str());
    return bad == 0 && verrors == 0 ? 0 : 1;
}
