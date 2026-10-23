// rvk_vktest: GPU smoke test of the Vulkan core and the shader contract, without the game.
//
//   rvk_vktest [out.png] [--no-validation]
//   rvk_vktest --gpl-corpus <dir with *.spv> [--no-validation]: compiles every SPIR-V module as a graphics pipeline
//   library (pre-rasterization / fragment shader), as the renderer does at shader creation (driver robustness check)
//   rvk_vktest --passes [--hazard]: the injected-pass building blocks (docs/research/injection.md) with synchronization
//   validation: the debug view's GLSL compute shader (embedded at build time) reads a FrameBlock and writes a transient
//   image, its fullscreen composite multiplies a render target by it; pixels checked. --hazard leaves a barrier out on
//   purpose (two copies into one buffer) and passes when synchronization validation reports it (proves the check is live)
//   rvk_vktest --rt: the optional ray tracing tier: BLAS -> TLAS -> GL_EXT_ray_query compute (shaders/test/rt_query.comp),
//   pixels checked; and a device with rt.allow off (docs/research/raytracing.md). SKIP (exit 0) on a GPU without it
//   rvk_vktest --fsr [dll]: FSR 3.1 through the signed FFX API DLL (third_party/amd_fidelityfx): load, create a native AA
//   context, dispatch the jitter sequence on a constant image under validation, output checked; SKIP (exit 0) without the DLL
//   rvk_vktest --shader-stats: the driver's register / spill / instruction statistics of the water pixel shader (in a
//   pipeline like the renderer's) and of the ray-traced reflections' compute pass (VK_KHR_pipeline_executable_properties)
//
// Off-screen: assembles a D3D9 vs_3_0/ps_3_0 pair (D3DXAssembleShader), translates it (shader/glsl), renders a full-screen
// quad that samples a 64x64 gradient with linear filtering into a 64x64 target through the bindless set, buffer-device-address
// constants and a dynamic-rendering pipeline, reads the pixels back and checks them against D3D9's rasterisation rules
// (pixel centres on integers: the half-pixel fix-up must give 4i-2, not 4i). Exit code 0 = pass, no validation errors.

#include <cmath>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
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
#include "core/caps.h"
#include "d3d9/scene.h"
#include "shader/abi.h"
#include "shader/embedded.h"
#include "shader/glsl.h"
#include "shader/sm_ir.h"
#include "shader/spirv.h"
#include "vk/bindless.h"
#include "vk/context.h"
#include "vk/frames.h"
#include "vk/fsr.h"
#include "vk/image.h"
#include "vk/passes.h"
#include "vk/pipeline.h"
#include "vk/raytracing.h"

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

// the optional ray tracing tier (docs/research/raytracing.md): a device with it (the frame ring then carries the BLAS-input
// usage: validation checks it), the BLAS -> TLAS -> ray query self-test, and a device with rt.allow off (no tier, a reason,
// the self-test skipped). A GPU without ray tracing passes with "SKIP" (the floor path is what matters there).
int rtTest() {
    int failures = 0;
    std::uint32_t verrors = 0;
    {
        vk::Context ctx;
        if (!ctx.create({true, -1, "rvk_vktest rt", true})) {
            return 2;
        }
        for (const auto& l : ctx.capabilities(false).lines()) std::printf("%s\n", l.c_str());
        if (!ctx.features().rayTracing.enabled) {
            std::printf("SKIP: ray tracing off: %s\n", ctx.features().rayTracing.reason.c_str());
            return 0;
        }
        vk::Frames frames;
        if (!frames.create(ctx)) {
            return 2;
        }
        frames.destroy();
        const vk::rt::SelfTestResult t = vk::rt::selfTest(ctx);
        std::printf("self-test (ray tracing on): %s, %.1f ms: %s\n", t.pass ? "pass" : "FAIL", t.milliseconds, t.detail.c_str());
        failures += t.pass ? 0 : 1;
        verrors += ctx.validationErrors();
        ctx.destroy();
    }
    {
        vk::Context ctx;
        if (!ctx.create({true, -1, "rvk_vktest rt off", true, false})) {
            return 2;
        }
        const auto& rt = ctx.features().rayTracing;
        const caps::Report report = ctx.capabilities(false);
        const bool ok = rt.available && !rt.enabled && rt.reason.find("rt.allow") != std::string::npos &&
                        report.satisfies(caps::bit(caps::Cap::RayTracingHardware)) && !report.satisfies(caps::bit(caps::Cap::RayQuery));
        const vk::rt::SelfTestResult t = vk::rt::selfTest(ctx);
        std::printf("rt.allow off: tier %s (%s), self-test %s\n", rt.enabled ? "ON" : "off", rt.reason.c_str(),
                    t.ran ? "RAN" : "skipped");
        failures += ok && !t.ran ? 0 : 1;
        verrors += ctx.validationErrors();
        ctx.destroy();
    }
    const bool pass = failures == 0 && verrors == 0;
    std::printf("%s: rt: %d failure(s), %u validation error(s)\n", pass ? "PASS" : "FAIL", failures, verrors);
    return pass ? 0 : 1;
}

// --shader-stats: the driver's statistics (VK_KHR_pipeline_executable_properties: registers, spills, instructions) of the
// embedded water pixel shader in a pipeline like the renderer's (the draw layout, the sea grid's vertex shader), and of
// the ray-traced reflections' compute shader (occupancy / spill checks, docs/research/rt-reflections.md 3.3)
int shaderStats() {
    vk::Context ctx;
    vk::ContextOptions o;
    o.validation = false;
    o.appName = "rvk_vktest shader-stats";
    o.pipelineStatistics = true;
    if (!ctx.create(o)) return 2;
    if (!ctx.features().pipelineStatistics) {
        std::printf("SKIP: the driver has no VK_KHR_pipeline_executable_properties\n");
        ctx.destroy();
        return 0;
    }
    vk::Frames frames;
    vk::Bindless bindless;
    if (!frames.create(ctx) || !bindless.create(ctx, frames)) return 2;
    const auto report = [&](VkPipeline p, const char* what) {
        VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
        pi.pipeline = p;
        std::uint32_t n = 0;
        vkGetPipelineExecutablePropertiesKHR(ctx.device(), &pi, &n, nullptr);
        std::vector<VkPipelineExecutablePropertiesKHR> props(n, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
        vkGetPipelineExecutablePropertiesKHR(ctx.device(), &pi, &n, props.data());
        for (std::uint32_t i = 0; i < n; ++i) {
            std::printf("%s: executable %u '%s' (subgroup %u)\n", what, i, props[i].name, props[i].subgroupSize);
            VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
            ei.pipeline = p;
            ei.executableIndex = i;
            std::uint32_t m = 0;
            vkGetPipelineExecutableStatisticsKHR(ctx.device(), &ei, &m, nullptr);
            std::vector<VkPipelineExecutableStatisticKHR> st(m, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
            vkGetPipelineExecutableStatisticsKHR(ctx.device(), &ei, &m, st.data());
            for (const auto& s : st) {
                switch (s.format) {
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: std::printf("    %-28s %s\n", s.name, s.value.b32 ? "true" : "false"); break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: std::printf("    %-28s %lld\n", s.name, static_cast<long long>(s.value.i64)); break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: std::printf("    %-28s %llu\n", s.name, static_cast<unsigned long long>(s.value.u64)); break;
                default: std::printf("    %-28s %.3f\n", s.name, s.value.f64); break;
                }
            }
        }
    };
    int failures = 0;
    for (const char* fsName : {"water/water.frag"}) {
        VkShaderModule vs = vk::createShaderModule(ctx, shader::embedded::spirv("water/water_ocean.vert"));
        VkShaderModule fs = vk::createShaderModule(ctx, shader::embedded::spirv(fsName));
        VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;
        const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        const VkFormat color = VK_FORMAT_R16G16B16A16_SFLOAT;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &color;
        ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
        ci.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = bindless.pipelineLayout();
        VkPipeline p = VK_NULL_HANDLE;
        if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE || vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &p) != VK_SUCCESS) {
            std::printf("FAIL: %s: pipeline\n", fsName);
            ++failures;
        } else {
            report(p, fsName);
            vkDestroyPipeline(ctx.device(), p, nullptr);
        }
        if (vs != VK_NULL_HANDLE) vkDestroyShaderModule(ctx.device(), vs, nullptr);
        if (fs != VK_NULL_HANDLE) vkDestroyShaderModule(ctx.device(), fs, nullptr);
    }
    // the ray-traced reflections' compute pass (needs the tier)
    if (ctx.features().rayTracing.enabled) {
        vk::PassLayout layout;
        const VkDescriptorSetLayoutBinding out{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkShaderModule cs = vk::createShaderModule(ctx, shader::embedded::spirv("water/water_rt.comp"));
        if (layout.create(ctx, bindless, {&out, 1}) && cs != VK_NULL_HANDLE) {
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
            ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = cs;
            ci.stage.pName = "main";
            ci.layout = layout.layout();
            VkPipeline p = VK_NULL_HANDLE;
            if (vkCreateComputePipelines(ctx.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &p) == VK_SUCCESS) {
                report(p, "water/water_rt.comp");
                vkDestroyPipeline(ctx.device(), p, nullptr);
            } else {
                std::printf("FAIL: water/water_rt.comp: pipeline\n");
                ++failures;
            }
        }
        if (cs != VK_NULL_HANDLE) vkDestroyShaderModule(ctx.device(), cs, nullptr);
        layout.destroy(ctx);
    }
    bindless.destroy();
    frames.destroy();
    ctx.destroy();
    std::printf("%s: shader-stats: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}

// prints where an access violation happened (module + offset of the top frames): the FFX DLL has no symbols here
long __stdcall crashReport(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    void* frames[24];
    const USHORT n = CaptureStackBackTrace(0, 24, frames, nullptr);
    std::printf("CRASH: access violation at %p (%s address %p)\n", e->ExceptionRecord->ExceptionAddress,
                e->ExceptionRecord->ExceptionInformation[0] != 0 ? "write" : "read",
                reinterpret_cast<void*>(e->ExceptionRecord->ExceptionInformation[1]));
    auto where = [](void* a) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<const char*>(a), &mod)) {
            return false;
        }
        GetModuleFileNameA(mod, name, MAX_PATH);
        std::printf("  %s + 0x%llx\n", name, static_cast<unsigned long long>(static_cast<char*>(a) - reinterpret_cast<char*>(mod)));
        return true;
    };
    // the faulting thread's stack: words pointing into a loaded module are mostly return addresses
    const auto* sp = reinterpret_cast<void**>(e->ContextRecord->Rsp);
    for (int i = 0, shown = 0; i < 512 && shown < 14; ++i) {
        if (sp[i] != nullptr && where(sp[i])) ++shown;
    }
    (void)frames;
    (void)n;
    return EXCEPTION_CONTINUE_SEARCH;
}

// FSR 3.1 native AA through the FFX API DLL on a constant image: every pixel of the output must keep the input colour.
int fsrTest(const std::string& dllArg, bool validation) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a crash inside the DLL must not swallow the output
    AddVectoredExceptionHandler(1, crashReport);
    constexpr std::uint32_t kW = 256, kH = 144;
    const std::filesystem::path dll = dllArg.empty() ? std::filesystem::path(RVK_FFX_DLL) : std::filesystem::path(dllArg);
    if (!std::filesystem::exists(dll)) {
        std::printf("SKIP: %s not found\n", dll.string().c_str());
        return 0;
    }
    vk::Context ctx;
    if (!ctx.create({validation, -1, "rvk_vktest"})) return 2;
    vk::Frames frames;
    if (!frames.create(ctx)) return 2;

    vk::Fsr fsr;
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
        return ok;
    };
    if (check(fsr.load(dll), "load the DLL")) {
        std::printf("versions: %s\n", fsr.versions(ctx).c_str());
        vk::FsrCreateInfo ci;
        ci.maxRenderSize = ci.maxUpscaleSize = {kW, kH};
        ci.hdr = true;
        ci.debugChecking = true;
        if (check(fsr.createContext(ctx, ci), "ffxCreateContext")) {
            std::printf("provider: %s\n", fsr.providerName().c_str());
            const int phases = fsr.jitterPhaseCount(kW, kW);
            std::printf("jitter phases (native AA): %d\n", phases);
            check(phases > 0, "jitter phase count");

            const VkImageUsageFlags inUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            vk::Image color, depth, motion, output;
            color.create(ctx, {VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, {kW, kH, 1}, 1, 1, false, inUsage, {}, "fsr color"});
            depth.create(ctx, {VK_IMAGE_TYPE_2D, VK_FORMAT_R32_SFLOAT, {kW, kH, 1}, 1, 1, false, inUsage, {}, "fsr depth"});
            motion.create(ctx, {VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16_SFLOAT, {kW, kH, 1}, 1, 1, false, inUsage, {}, "fsr motion"});
            output.create(ctx, {VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, {kW, kH, 1}, 1, 1, false,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, {},
                                "fsr output"});
            VkBuffer readback = VK_NULL_HANDLE;
            VmaAllocation readbackAlloc = VK_NULL_HANDLE;
            VmaAllocationInfo readbackInfo{};
            {
                VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bci.size = kW * kH * 8;
                bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_AUTO;
                aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                vmaCreateBuffer(ctx.allocator(), &bci, &aci, &readback, &readbackAlloc, &readbackInfo);
            }
            constexpr int kFrames = 12;
            for (int frame = 0; frame < kFrames; ++frame) {
                frames.begin();
                VkCommandBuffer cmd = frames.cmd();
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                auto clear = [&](vk::Image& img, VkClearColorValue v) {
                    img.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    vkCmdClearColorImage(cmd, img.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
                };
                clear(color, {{0.25f, 0.5f, 0.75f, 1.0f}});
                clear(depth, {{0.5f, 0.0f, 0.0f, 0.0f}});
                clear(motion, {{0.0f, 0.0f, 0.0f, 0.0f}});
                vk::FsrDispatch d;
                d.cmd = cmd;
                d.color = &color;
                d.depth = &depth;
                d.motion = &motion;
                d.output = &output;
                d.renderSize = d.upscaleSize = {kW, kH};
                d.reset = frame == 0;
                d.cameraNear = 1.0f;
                d.cameraFar = 1000.0f;
                d.cameraFovY = 1.0f;
                fsr.jitterOffset(frame % std::max(phases, 1), phases, d.jitterX, d.jitterY);
                check(fsr.dispatch(d), "ffxDispatch");
                if (frame == kFrames - 1) {
                    output.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    VkBufferImageCopy back{};
                    back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    back.imageExtent = {kW, kH, 1};
                    vkCmdCopyImageToBuffer(cmd, output.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &back);
                }
                frames.submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
                frames.waitIdle();
            }
            const auto* px = static_cast<const std::uint16_t*>(readbackInfo.pMappedData);
            const float want[3] = {0.25f, 0.5f, 0.75f};
            int bad = 0;
            float worst = 0.0f;
            for (std::uint32_t i = 0; i < kW * kH; ++i) {
                for (int c = 0; c < 3; ++c) {
                    const float got = halfToFloat(px[i * 4 + c]);
                    const float e = std::fabs(got - want[c]);
                    worst = std::max(worst, e);
                    if (e > 0.02f && bad++ < 4) std::printf("pixel %u channel %d = %f (want %f)\n", i, c, got, want[c]);
                }
            }
            std::printf("output: %d channel(s) off, worst error %f\n", bad, worst);
            check(bad == 0, "output keeps the constant input");
            vmaDestroyBuffer(ctx.allocator(), readback, readbackAlloc);
            frames.retire([&] {});
            color.destroy(ctx);
            depth.destroy(ctx);
            motion.destroy(ctx);
            output.destroy(ctx);
        }
        fsr.destroy();
    }
    frames.destroy();
    const std::uint32_t verrors = ctx.validationErrors();
    ctx.destroy();
    if (verrors != 0) ++failures;
    std::printf("%s: fsr: %d failure(s), %u validation error(s)\n", failures == 0 ? "PASS" : "FAIL", failures, verrors);
    return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    std::string out = "vktest.png";
    bool validation = true;
    std::string gplDir;
    bool passes = false, hazard = false, rtMode = false, fsrMode = false;
    std::string fsrDll;
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
        } else if (a == "--rt") {
            rtMode = true;
        } else if (a == "--fsr") {
            fsrMode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') fsrDll = argv[++i];
        } else if (a == "--shader-stats") {
            return shaderStats();
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
    if (rtMode) {
        return rtTest();
    }
    if (fsrMode) {
        return fsrTest(fsrDll, validation);
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
