// rvk_rhitest: the slim RHI (src/rhi/rhi.h) on both backends with the same code and the same HLSL (docs/research/hlsl-rhi.md 5, 6).
//
//   rvk_rhitest [--vulkan] [--d3d12] [--validation]
//
// Per backend: a compute pass that reads a constant buffer (b8), a bindless texture + sampler (indices in the constant buffer), a RVK_REF block
// (the frame block: Buffer::shaderRef) and the push constants, writing a storage image that is read back and compared with the CPU; a graphics
// pass (dynamic rendering, fullscreen triangle, constant buffer) into a render target that is read back. SKIP when a backend is unavailable
// (a DX12 device without SM 6.6 bindless); exit 1 on any mismatch.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "rhi/rhi.h"
#include "shader/abi.h"
#include "shader/dxc.h"

using namespace rvk;

namespace {

struct Blobs {
    std::vector<std::uint32_t> spirv;
    std::vector<std::uint8_t> dxil;
    rhi::Shader shader() const { return {spirv, dxil}; }
};

bool compileBoth(const std::string& file, const char* entry, const char* profile, Blobs& out) {
    shader::dxc::Request r;
    r.file = file;
    r.entry = entry;
    r.profile = profile;
    r.target = shader::dxc::Target::Spirv;
    shader::dxc::Result a = shader::dxc::compile(r);
    r.target = shader::dxc::Target::Dxil;
    shader::dxc::Result b = shader::dxc::compile(r);
    if (!a.ok() || !b.ok()) {
        std::printf("compile %s failed:\n%s\n%s\n", file.c_str(), a.log.c_str(), b.log.c_str());
        return false;
    }
    out.spirv = std::move(a.words);
    out.dxil = std::move(b.bytes);
    return true;
}

struct CbData {
    float scale[4];
    std::uint32_t ids[4];
};
struct RhiPush {
    std::uint32_t frame[2];
    std::uint32_t passId, passFlags;
    std::uint32_t width, height, pad0, pad1;
};

constexpr std::uint32_t kSize = 64;

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::printf("  FAIL: %s\n", what.c_str());
    }
}

float halfToFloat(std::uint16_t h) {
    const std::uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float f = e == 0 ? std::ldexp(static_cast<float>(m), -24) : e == 31 ? INFINITY : std::ldexp(static_cast<float>(m | 1024), static_cast<int>(e) - 25);
    return s ? -f : f;
}

bool runBackend(rhi::Backend backend, bool validation, const Blobs& cs, const Blobs& vs, const Blobs& ps) {
    rhi::DeviceDesc dd;
    dd.backend = backend;
    dd.validation = validation;
    dd.name = "rvk_rhitest";
    auto dev = backend == rhi::Backend::Vulkan ? rhi::createVulkanDevice(dd) : rhi::createD3D12Device(dd);
    if (!dev) {
        std::printf("[%s] SKIP: no device meeting the floor\n", rhi::backendName(backend));
        return true;
    }
    std::printf("[%s] adapter '%s'\n", rhi::backendName(backend), dev->caps().adapterName.c_str());
    const int before = failures;

    // resources
    const std::uint32_t pitch = rhi::bufferRowPitch(VK_FORMAT_R8G8B8A8_UNORM, kSize);
    auto upload = dev->createBuffer({static_cast<std::uint64_t>(pitch) * kSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Upload, "rhi input pixels"});
    auto frameBlock = dev->createBuffer({sizeof(shader::abi::FrameBlock), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi frame block"});
    auto cbuf = dev->createBuffer({256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, rhi::Memory::Upload, "rhi constant buffer"});
    const std::uint32_t outPitch = rhi::bufferRowPitch(VK_FORMAT_R32G32B32A32_SFLOAT, kSize);
    const std::uint32_t rtPitch = rhi::bufferRowPitch(VK_FORMAT_R8G8B8A8_UNORM, kSize);
    auto readCompute = dev->createBuffer({static_cast<std::uint64_t>(outPitch) * kSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi readback 1"});
    auto readRender = dev->createBuffer({static_cast<std::uint64_t>(rtPitch) * kSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi readback 2"});
    auto input = dev->createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {kSize, kSize, 1}, 1, 1,
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "rhi input"});
    auto output = dev->createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, {kSize, kSize, 1}, 1, 1,
                                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "rhi output"});
    auto target = dev->createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {kSize, kSize, 1}, 1, 1,
                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "rhi target"});
    if (!upload || !frameBlock || !cbuf || !readCompute || !readRender || !input || !output || !target) {
        std::printf("  FAIL: resource creation\n");
        ++failures;
        return false;
    }
    const rhi::SamplerIndex sampler = dev->createSampler({VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE});
    check(input->heapSampled() != 0 && sampler != 0, "heap indices of the texture / sampler");

    // contents: pixel (x, y) = (4x, 4y, 128, 255); the frame block's time and param 1; the constant buffer
    auto* px = static_cast<std::uint8_t*>(upload->map());
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            std::uint8_t* p = px + static_cast<std::size_t>(y) * pitch + x * 4;
            p[0] = static_cast<std::uint8_t>(x * 4);
            p[1] = static_cast<std::uint8_t>(y * 4);
            p[2] = 128;
            p[3] = 255;
        }
    }
    auto* fb = static_cast<shader::abi::FrameBlock*>(frameBlock->map());
    std::memset(fb, 0, sizeof(*fb));
    fb->time[0] = 1.5f;
    fb->params[1][0] = 2.0f;
    auto* cb = static_cast<CbData*>(cbuf->map());
    *cb = {{0.5f, 2.0f, 1.0f, 0.25f}, {input->heapSampled(), sampler, 0, 0}};

    std::unique_ptr<rhi::Pipeline> compute = dev->createComputePipeline({cs.shader(), "rhi compute"});
    rhi::GraphicsPipelineDesc gd;
    gd.vs = vs.shader();
    gd.ps = ps.shader();
    gd.colorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    gd.name = "rhi graphics";
    std::unique_ptr<rhi::Pipeline> graphics = dev->createGraphicsPipeline(gd);
    if (!compute || !graphics) {
        std::printf("  FAIL: pipeline creation\n");
        ++failures;
        return false;
    }

    dev->submitAndWait([&](rhi::CommandList& cl) {
        cl.beginRegion("rhi test");
        cl.transition(*input, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        cl.copyBufferToTexture(*input, *upload, 0);
        cl.transition(*input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // compute: constant buffer + bindless texture + RVK_REF + push constants + storage image
        cl.transition(*output, VK_IMAGE_LAYOUT_GENERAL);
        RhiPush push{};
        const auto ref = frameBlock->shaderRef(0);
        push.frame[0] = ref[0];
        push.frame[1] = ref[1];
        push.passId = 7;
        push.width = push.height = kSize;
        cl.bindPipeline(*compute);
        cl.setPushConstants(&push, sizeof(push));
        cl.setConstantBuffer(*cbuf, 0, 256);
        rhi::Texture* storage[1] = {output.get()};
        cl.setStorageTextures(storage);
        cl.dispatch(kSize / 8, kSize / 8, 1);
        cl.transition(*output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        cl.copyTextureToBuffer(*readCompute, 0, *output);
        // graphics: dynamic rendering into the target, the constant buffer's colour
        cl.transition(*target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        rhi::RenderingInfo ri;
        ri.colorCount = 1;
        ri.color[0].texture = target.get();
        ri.color[0].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ri.color[0].store = VK_ATTACHMENT_STORE_OP_STORE;
        ri.color[0].clear.float32[2] = 1.0f;
        ri.color[0].clear.float32[3] = 1.0f;
        ri.width = ri.height = kSize;
        cl.beginRendering(ri);
        cl.bindPipeline(*graphics);
        cl.setConstantBuffer(*cbuf, 0, 256);
        cl.draw(3, 1, 0, 0);
        cl.endRendering();
        cl.transition(*target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        cl.copyTextureToBuffer(*readRender, 0, *target);
        cl.endRegion();
    });

    // compare: compute output (linear filter at texel centres = the texel)
    const auto* out = static_cast<const std::uint8_t*>(readCompute->map());
    int bad = 0;
    float worst = 0.0f;
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const float* o = reinterpret_cast<const float*>(out + static_cast<std::size_t>(y) * outPitch + x * 16);
            const float want[4] = {(static_cast<float>(x * 4) / 255.0f) * 0.5f + 1.5f, (static_cast<float>(y * 4) / 255.0f) * 2.0f + 2.0f,
                                   (128.0f / 255.0f) * 1.0f + 7.0f, 1.0f * 0.25f + static_cast<float>(x + y)};
            for (int c = 0; c < 4; ++c) {
                const float d = std::fabs(o[c] - want[c]);
                worst = std::max(worst, d);
                if (d > 2e-3f) ++bad;
            }
        }
    }
    check(bad == 0, std::format("compute output: {} bad channel(s), worst error {}", bad, worst));
    // graphics: the whole target = (0.5, 1.0 (clamped from 2), 1, 0.25) in UNORM8
    const auto* rt = static_cast<const std::uint8_t*>(readRender->map());
    int badPx = 0;
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const std::uint8_t* p = rt + static_cast<std::size_t>(y) * rtPitch + x * 4;
            if (std::abs(static_cast<int>(p[0]) - 128) > 1 || p[1] != 255 || p[2] != 255 || std::abs(static_cast<int>(p[3]) - 64) > 1) ++badPx;
        }
    }
    check(badPx == 0, std::format("graphics output: {} bad pixel(s), first = ({}, {}, {}, {})", badPx, rt[0], rt[1], rt[2], rt[3]));
    // swapchain: a hidden window, clear + read back the back buffer, present, resize, again
    {
        RECT rc{0, 0, static_cast<LONG>(kSize), static_cast<LONG>(kSize)};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        HWND hwnd = CreateWindowExW(0, L"STATIC", L"rvk_rhitest", WS_OVERLAPPEDWINDOW, 0, 0, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
        auto sc = hwnd != nullptr ? dev->createSwapchain({hwnd, kSize, kSize, true}) : nullptr;
        check(sc != nullptr, "swapchain creation");
        for (int pass = 0; sc != nullptr && pass < 2; ++pass) {
            if (pass == 1) check(sc->resize(kSize, kSize), "swapchain resize");
            const std::uint32_t w = sc->width(), h = sc->height(), bytes = rhi::formatBytes(sc->format());
            const std::uint32_t pitchSc = rhi::bufferRowPitch(sc->format(), w);
            auto rb = dev->createBuffer({static_cast<std::uint64_t>(pitchSc) * h, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi readback swapchain"});
            const bool ok = rb != nullptr && dev->renderAndPresent(*sc, [&](rhi::CommandList& cl, rhi::Texture& back) {
                cl.transition(back, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                rhi::RenderingInfo ri;
                ri.colorCount = 1;
                ri.color[0].texture = &back;
                ri.color[0].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
                ri.color[0].clear.float32[0] = 1.0f;
                ri.color[0].clear.float32[1] = 0.5f;
                ri.color[0].clear.float32[3] = 1.0f;
                ri.width = w;
                ri.height = h;
                cl.beginRendering(ri);
                cl.endRendering();
                cl.transition(back, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                cl.copyTextureToBuffer(*rb, 0, back);
            });
            check(ok, "renderAndPresent");
            if (ok && bytes == 4) {
                const auto* p = static_cast<const std::uint8_t*>(rb->map());
                const bool bgra = sc->format() == VK_FORMAT_B8G8R8A8_UNORM || sc->format() == VK_FORMAT_B8G8R8A8_SRGB;
                const int r = bgra ? p[2] : p[0], g = p[1], b = bgra ? p[0] : p[2];
                check(r == 255 && std::abs(g - 128) <= 1 && b == 0 && p[3] == 255, std::format("swapchain pixel ({}, {}, {}, {})", r, g, b, p[3]));
            }
        }
        sc.reset();
        if (hwnd != nullptr) DestroyWindow(hwnd);
    }
    check(dev->validationErrors() == 0, std::format("{} validation error(s)", dev->validationErrors()));
    std::printf("[%s] %s\n", rhi::backendName(backend), failures == before ? "PASS" : "FAIL");
    return failures == before;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool vulkan = false, d3d12 = false, validation = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--vulkan") vulkan = true;
        else if (a == "--d3d12") d3d12 = true;
        else if (a == "--validation") validation = true;
    }
    if (!vulkan && !d3d12) vulkan = d3d12 = true;
    if (!shader::dxc::available()) {
        std::printf("SKIP: %s\n", std::string(shader::dxc::unavailableReason()).c_str());
        return 0;
    }
    Blobs cs, vs, ps;
    if (!compileBoth("test/rhi_cb.comp.hlsl", "CSMain", "cs_6_6", cs) || !compileBoth("debug/fullscreen.vert.hlsl", "VSMain", "vs_6_6", vs) ||
        !compileBoth("test/rhi_cb.frag.hlsl", "PSMain", "ps_6_6", ps)) {
        return 1;
    }
    if (vulkan) runBackend(rhi::Backend::Vulkan, validation, cs, vs, ps);
    if (d3d12) runBackend(rhi::Backend::D3D12, validation, cs, vs, ps);
    std::printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
