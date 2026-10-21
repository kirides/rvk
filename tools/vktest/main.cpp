// rvk_vktest: GPU smoke test of the Vulkan core and the shader contract, without the game.
//
//   rvk_vktest [out.png] [--no-validation]
//   rvk_vktest --gpl-corpus <dir with *.spv> [--no-validation]: compiles every SPIR-V module as a graphics pipeline
//   library (pre-rasterization / fragment shader), as the renderer does at shader creation (driver robustness check)
//   rvk_vktest --passes [--hazard]: the injected-pass building blocks (docs/research/injection.md) with synchronization
//   validation: the debug view's GLSL compute shader (embedded at build time) reads a FrameBlock and writes a transient
//   image, its fullscreen composite multiplies a render target by it; pixels checked. --hazard leaves a barrier out on
//   purpose (two copies into one buffer) and passes when synchronization validation reports it (proves the check is live)
//
// Off-screen: assembles a D3D9 vs_3_0/ps_3_0 pair (D3DXAssembleShader), translates it (shader/glsl), renders a full-screen
// quad that samples a 64x64 gradient with linear filtering into a 64x64 target through the bindless set, buffer-device-address
// constants and a dynamic-rendering pipeline, reads the pixels back and checks them against D3D9's rasterisation rules
// (pixel centres on integers: the half-pixel fix-up must give 4i-2, not 4i). Exit code 0 = pass, no validation errors.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "common/d3dx.h"
#include "d3d9/scene.h"
#include "shader/abi.h"
#include "shader/embedded.h"
#include "shader/glsl.h"
#include "shader/sm_ir.h"
#include "shader/spirv.h"
#include "vk/bindless.h"
#include "vk/context.h"
#include "vk/frames.h"
#include "vk/image.h"
#include "vk/passes.h"
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

// SEH around the driver call: a crashing compile is counted instead of ending the run (diagnostics only; the driver may be
// left in a bad state afterwards)
bool prepareGuarded(vk::PipelineCache& pipes, VkShaderModule m, VkShaderStageFlagBits stage) {
    __try {
        pipes.prepareShader(m, stage);
        return true;
    } __except (1) {
        return false;
    }
}

// every module of `dir` through PipelineCache::prepareShader; prints each file before compiling it (a driver crash shows
// which one)
int gplCorpus(const std::filesystem::path& dir, bool validation) {
    vk::Context ctx;
    if (!ctx.create({validation, -1, "rvk_vktest"})) {
        return 2;
    }
    if (!vk::PipelineCache::librariesSupported(ctx)) {
        std::printf("graphics pipeline libraries not supported\n");
        return 2;
    }
    vk::Frames frames;
    vk::Bindless bindless;
    vk::PipelineCache pipes;
    if (!frames.create(ctx) || !bindless.create(ctx, frames) || !pipes.create(ctx, bindless.pipelineLayout(), {}, true)) {
        return 2;
    }
    std::vector<VkShaderModule> modules;
    int n = 0, crashes = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() != ".spv") continue;
        std::ifstream in(e.path(), std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<std::uint32_t> words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), words.size() * 4);
        // OpEntryPoint (opcode 15): execution model 0 = Vertex, 4 = Fragment
        int model = -1;
        for (std::size_t i = 5; i < words.size();) {
            const std::uint32_t op = words[i] & 0xFFFF, len = words[i] >> 16;
            if (op == 15 && i + 1 < words.size()) {
                model = static_cast<int>(words[i + 1]);
                break;
            }
            i += len == 0 ? 1 : len;
        }
        if (model != 0 && model != 4) continue;
        std::printf("%s %s\n", model == 0 ? "vs" : "ps", e.path().filename().string().c_str());
        std::fflush(stdout);
        VkShaderModule m = vk::createShaderModule(ctx, words);
        if (m == VK_NULL_HANDLE) continue;
        if (!prepareGuarded(pipes, m, model == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT)) {
            std::printf("CRASH %s\n", e.path().filename().string().c_str());
            ++crashes;
        }
        modules.push_back(m);
        ++n;
    }
    std::printf("%d modules, %d driver crashes, %u validation errors\n", n, crashes, ctx.validationErrors());
    pipes.destroy();
    for (VkShaderModule m : modules) vkDestroyShaderModule(ctx.device(), m, nullptr);
    bindless.destroy();
    frames.destroy();
    return ctx.validationErrors() == 0 && crashes == 0 ? 0 : 1;
}

float halfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1fu, mant = h & 0x3ffu;
    float v = exp == 0 ? std::ldexp(static_cast<float>(mant), -24)
                       : exp == 31 ? INFINITY : std::ldexp(static_cast<float>(mant | 0x400u), static_cast<int>(exp) - 25);
    return sign != 0 ? -v : v;
}

// the debug injection view's two passes on a 64x64 "scene" (cleared to 0.5): compute (tint mode, strength 1, time 0) into a
// transient factor image, composite scene *= factor. Synchronization validation must stay silent (or, with `hazard`, report
// the missing barrier between two copies into the read-back buffer).
int passTest(bool hazard) {
    vk::Context ctx;
    if (!ctx.create({true, -1, "rvk_vktest passes", true})) {
        return 2;
    }
    vk::Frames frames;
    vk::Bindless bindless;
    if (!frames.create(ctx) || !bindless.create(ctx, frames)) {
        return 2;
    }
    vk::Transients transients;
    transients.init(ctx, frames, bindless);
    vk::PassLayout layout;
    const VkDescriptorSetLayoutBinding out{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    if (!layout.create(ctx, bindless, {&out, 1})) {
        return 2;
    }
    const VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkPipeline compute = vk::createComputePipeline(ctx, layout.layout(), shader::embedded::spirv("debug/inject_view.comp"), "test compute");
    vk::PassGraphicsDesc gd;
    gd.vs = shader::embedded::spirv("debug/fullscreen.vert");
    gd.fs = shader::embedded::spirv("debug/inject_view.frag");
    gd.color = fmt;
    gd.blend.enable = 1;
    gd.blend.srcColor = VK_BLEND_FACTOR_DST_COLOR;
    gd.blend.dstColor = VK_BLEND_FACTOR_ZERO;
    gd.blend.srcAlpha = VK_BLEND_FACTOR_ZERO;
    gd.blend.dstAlpha = VK_BLEND_FACTOR_ONE;
    gd.name = "test composite";
    VkPipeline composite = vk::createPassGraphicsPipeline(ctx, layout.layout(), gd);
    if (compute == VK_NULL_HANDLE || composite == VK_NULL_HANDLE) {
        return 2;
    }
    vk::Image scene;
    scene.create(ctx, {VK_IMAGE_TYPE_2D, fmt, {kSize, kSize, 1}, 1, 1, false,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, {}, "test scene"});
    VkBuffer readback = VK_NULL_HANDLE;
    VmaAllocation readbackAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo readbackInfo{};
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = kSize * kSize * 8;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        vmaCreateBuffer(ctx.allocator(), &bci, &aci, &readback, &readbackAlloc, &readbackInfo);
    }

    frames.begin();
    VkCommandBuffer cmd = frames.cmd();
    bindless.recordInit(cmd);
    // the frame block: no camera, the debug view's parameters (tint, strength 1)
    std::array<std::array<float, 4>, 16> params{};
    params[shader::abi::kFrameParamDebugView] = {2.0f, 1.0f, 500.0f, 0.0f};
    d3d9::scene::FrameExtras x;
    x.now = 0.0;
    x.width = kSize;
    x.height = kSize;
    x.params = &params;
    vk::RingAlloc fb = frames.allocate(sizeof(shader::abi::FrameBlock), 16);
    d3d9::scene::fillFrameBlock(*static_cast<shader::abi::FrameBlock*>(fb.ptr), {}, {}, x);

    // the scene: cleared to 0.5
    scene.transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = scene.view();
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color = {{0.5f, 0.5f, 0.5f, 1.0f}};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {kSize, kSize}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdEndRendering(cmd);

    struct Push {
        shader::abi::PassPushHeader h;
        std::uint32_t depthTex, width, height, srcTex, srcSampler, pad;
    } push{};
    vk::Transients::Entry* factor = transients.get("test factor", {fmt, {kSize, kSize}});
    if (factor == nullptr) {
        return 2;
    }
    push.h.frame = fb.address;
    push.width = kSize;
    push.height = kSize;
    push.srcTex = factor->slot;
    // two frames: the second one's compute write must be ordered after the first one's composite read
    for (int frame = 0; frame < 2; ++frame) {
        factor->image.transition(cmd, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute);
        layout.bindSet0(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bindless);
        layout.pushStorageImage(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, 0, factor->image.view());
        layout.push(cmd, &push, sizeof(push));
        vkCmdDispatch(cmd, kSize / 8, kSize / 8, 1);
        factor->image.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (frame == 1) break;  // the scene gets one composite
        scene.transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport vp{0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f};
        const VkRect2D sc{{0, 0}, {kSize, kSize}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite);
        layout.bindSet0(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bindless);
        layout.push(cmd, &push, sizeof(push));
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    }
    scene.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy back{};
    back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    back.imageExtent = {kSize, kSize, 1};
    vkCmdCopyImageToBuffer(cmd, scene.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &back);
    // --hazard: a second copy into the same buffer without a barrier (write after write). Not a second dispatch: this layer
    // (SDK 1.4.363) does not track the storage-image writes of these pipelines (measured: two dispatches without a barrier
    // go unreported), so the check covers barriers between attachments, transfers and the pass helpers' layout changes
    if (hazard) vkCmdCopyImageToBuffer(cmd, scene.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &back);
    frames.submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    frames.waitIdle();

    // expected: 0.5 * the tint colour at time 0 (inject_view.comp)
    float expect[3];
    const float phase[3] = {0.0f, 0.33f, 0.67f};
    for (int c = 0; c < 3; ++c) expect[c] = 0.5f * (0.5f + 0.5f * std::cos(6.2831853f * phase[c]));
    const auto* px = static_cast<const std::uint16_t*>(readbackInfo.pMappedData);
    int bad = 0;
    for (std::uint32_t i = 0; i < kSize * kSize; ++i) {
        for (int c = 0; c < 4; ++c) {
            const float v = halfToFloat(px[i * 4 + c]);
            const float e = c < 3 ? expect[c] : 1.0f;
            if (std::abs(v - e) > 2e-3f && bad++ < 5) {
                std::printf("pixel %u channel %d = %f, expected %f\n", i, c, v, e);
            }
        }
    }
    vmaDestroyBuffer(ctx.allocator(), readback, readbackAlloc);
    transients.destroy();
    frames.waitIdle();
    scene.destroy(ctx);
    vkDestroyPipeline(ctx.device(), compute, nullptr);
    vkDestroyPipeline(ctx.device(), composite, nullptr);
    layout.destroy(ctx);
    frames.destroy();  // runs the retired destructors (the transient image, its bindless slot) before the heap goes
    bindless.destroy();
    const std::uint32_t verrors = ctx.validationErrors();
    ctx.destroy();
    const bool pass = bad == 0 && (hazard ? verrors > 0 : verrors == 0);
    std::printf("%s: passes: %d bad value(s), %u validation error(s)%s\n", pass ? "PASS" : "FAIL", bad, verrors,
                hazard ? " (a hazard was expected)" : "");
    return pass ? 0 : 1;
}

int main(int argc, char** argv) {
    std::string out = "vktest.png";
    bool validation = true;
    std::string gplDir;
    bool passes = false, hazard = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-validation") {
            validation = false;
        } else if (a == "--gpl-corpus" && i + 1 < argc) {
            gplDir = argv[++i];
        } else if (a == "--passes") {
            passes = true;
        } else if (a == "--hazard") {
            hazard = true;
        } else {
            out = a;
        }
    }
    if (!gplDir.empty()) {
        return gplCorpus(gplDir, validation);
    }
    if (passes) {
        return passTest(hazard);
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
