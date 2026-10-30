// rvk_rhitest: the slim RHI (src/rhi/rhi.h) on both backends with the same code and the same HLSL (docs/research/hlsl-rhi.md 5, 6).
//
//   rvk_rhitest [--vulkan] [--d3d12] [--validation]
//
// Per backend: a compute pass that reads a constant buffer (b8), a bindless texture + sampler (indices in the constant buffer), a RVK_REF block
// (the frame block: Buffer::shaderRef) and the push constants, writing a storage image that is read back and compared with the CPU; a graphics
// pass (dynamic rendering, fullscreen triangle, constant buffer) into a render target that is read back; a swapchain on a hidden window. Then the
// gap checks, each on both backends from one HLSL source (docs/research/hlsl-rhi.md 6):
//   textures   cube + 3D + mip chain + array layers sampled through the heap (SampleLevel / Load), per-mip / per-layer storage and render target views
//   buffers    read-write buffer references (RVK_BUF_LOAD / STORE / ADD), atomics from 1000 threads
//   vertex     vertex + index buffers (two bindings, one per instance), drawIndexed, drawIndexedIndirectCount with a count smaller than maxDraws
//   ray query  a BLAS + a two-instance TLAS, inline ray queries (the scene and the expected pixels of src/vk/raytracing.cpp selfTest)
// A check SKIPs (with the reason) where a backend lacks the capability (DeviceCaps::drawIndirectCount / rayQuery); SKIP when a backend is
// unavailable (a DX12 device without SM 6.6 bindless); exit 1 on any mismatch.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
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
    bool ok = false;
    rhi::Shader shader() const { return {spirv, dxil}; }
};

// the shaders of all checks (compiled once, to SPIR-V and DXIL)
struct Shaders {
    Blobs cs, vs, ps;                      // the basic check
    Blobs tex, texStore, buf, vtxVs, vtxPs, rt, store8;  // the gap checks
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
    out.ok = true;
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

// ---- the gap checks ---------------------------------------------------------------------------------------------------------

std::string tag(rhi::Device& dev) { return std::string("[") + rhi::backendName(dev.caps().backend) + "] "; }

// "[Vulkan] name: PASS / FAIL / SKIP"
void report(rhi::Device& dev, const char* name, int before) { std::printf("%s%s: %s\n", tag(dev).c_str(), name, failures == before ? "PASS" : "FAIL"); }
void skip(rhi::Device& dev, const char* name, const char* why) { std::printf("%s%s: SKIP (%s)\n", tag(dev).c_str(), name, why); }

using Rgba = std::array<std::uint8_t, 4>;

// writes colour(x, y, z) into a w x h x d block of RGBA8 texels at `base` (rows bufferRowPitch apart)
template <typename F>
void fillTexels(std::uint8_t* base, std::uint32_t w, std::uint32_t h, std::uint32_t d, F colour) {
    const std::uint32_t pitch = rhi::bufferRowPitch(VK_FORMAT_R8G8B8A8_UNORM, w);
    for (std::uint32_t z = 0; z < d; ++z) {
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                const Rgba c = colour(x, y, z);
                std::memcpy(base + (static_cast<std::size_t>(z) * h + y) * pitch + x * 4, c.data(), 4);
            }
        }
    }
}

bool nearTo(const float* got, const float* want, float tolerance = 2e-3f) {
    for (int c = 0; c < 4; ++c) {
        if (std::fabs(got[c] - want[c]) > tolerance) return false;
    }
    return true;
}

// Gap 1: textures. A cube, a 3D texture, a mip chain and a 2D array are filled through copies of single subresources and sampled by one compute
// shader through the bindless heap; storage views of one mip / layer are written by another; render target views of a mip / layer are cleared.
void testTextures(rhi::Device& dev, const Shaders& sh) {
    const int before = failures;
    if (!sh.tex.ok || !sh.texStore.ok) {
        check(false, "texture shaders did not compile");
        report(dev, "textures", before);
        return;
    }
    constexpr VkImageUsageFlags kSampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // (a cube's layers are forced to 6)
    auto cube = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {4, 4, 1}, 1, 1, kSampled, "rhi cube", true});
    auto volume = dev.createTexture({VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, {4, 4, 4}, 1, 1, kSampled, "rhi volume"});
    auto mips = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {8, 8, 1}, 4, 1, kSampled, "rhi mips"});
    auto array = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {4, 4, 1}, 1, 3, kSampled, "rhi array"});
    auto out = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, {16, 1, 1}, 1, 1, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  "rhi tex out"});
    auto upload = dev.createBuffer({32768, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Upload, "rhi tex upload"});
    auto readback = dev.createBuffer({4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi tex readback"});
    const rhi::SamplerIndex point = dev.createSampler({VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE});
    auto sample = dev.createComputePipeline({sh.tex.shader(), "rhi tex sample"});
    if (!cube || !volume || !mips || !array || !out || !upload || !readback || point == 0 || !sample) {
        check(false, "texture resource / pipeline creation");
        report(dev, "textures", before);
        return;
    }
    check(cube->desc().layers == 6 && cube->heapSampled() != 0 && volume->heapSampled() != 0 && mips->heapSampled() != 0 &&
              array->heapSampled() != 0 && array->heapSampledLayer(1) != 0 && array->heapSampledLayer(2) != 0 && array->heapSampledLayer(3) == 0,
          "heap indices of the texture kinds");
    check(array->heapSampledLayer(0) == array->heapSampled() && array->heapSampledLayer(1) != array->heapSampledLayer(2), "array layer heap indices");

    // contents: upload offsets are multiples of 512 (the DX12 placement alignment)
    const auto faceColour = [](std::uint32_t f) { return Rgba{static_cast<std::uint8_t>(f * 40 + 10), static_cast<std::uint8_t>(255 - f * 30), static_cast<std::uint8_t>(20 + f * 10), 255}; };
    const auto voxelColour = [](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return Rgba{static_cast<std::uint8_t>(x * 60 + 10), static_cast<std::uint8_t>(y * 60 + 10), static_cast<std::uint8_t>(z * 60 + 10), 255};
    };
    const auto mipColour = [](std::uint32_t m) { return Rgba{static_cast<std::uint8_t>(m * 50 + 10), static_cast<std::uint8_t>(200 - m * 40), static_cast<std::uint8_t>(100 + m * 10), 255}; };
    const auto layerColour = [](std::uint32_t l) { return Rgba{static_cast<std::uint8_t>(30 + l * 70), 90, static_cast<std::uint8_t>(220 - l * 60), 255}; };
    auto* up = static_cast<std::uint8_t*>(upload->map());
    std::memset(up, 0, 32768);
    constexpr std::uint64_t kFaceOffset = 0, kVolumeOffset = 6144, kMipOffset = 10240, kLayerOffset = 14336;
    std::array<std::uint64_t, 4> mipOffsets{};
    {
        std::uint64_t o = kMipOffset;
        for (std::uint32_t m = 0; m < 4; ++m) {
            mipOffsets[m] = o;
            o += (rhi::copyBytes(mips->desc(), m) + 511) / 512 * 512;
        }
    }
    check(mipOffsets[3] + 256 <= kLayerOffset, "upload layout");
    for (std::uint32_t f = 0; f < 6; ++f) fillTexels(up + kFaceOffset + f * 1024, 4, 4, 1, [&](auto, auto, auto) { return faceColour(f); });
    fillTexels(up + kVolumeOffset, 4, 4, 4, [&](std::uint32_t x, std::uint32_t y, std::uint32_t z) { return voxelColour(x, y, z); });
    for (std::uint32_t m = 0; m < 4; ++m) {
        const std::uint32_t e = std::max(8u >> m, 1u);
        fillTexels(up + mipOffsets[m], e, e, 1, [&](auto, auto, auto) { return mipColour(m); });
    }
    for (std::uint32_t l = 0; l < 3; ++l) fillTexels(up + kLayerOffset + l * 1024, 4, 4, 1, [&](auto, auto, auto) { return layerColour(l); });

    struct TexPush {
        std::uint32_t cube, volume, mips, sampler;
        std::uint32_t layers[4];
    } push{cube->heapSampled(), volume->heapSampled(), mips->heapSampled(), point, {array->heapSampledLayer(0), array->heapSampledLayer(1), array->heapSampledLayer(2), 0}};

    dev.submitAndWait([&](rhi::CommandList& cl) {
        for (rhi::Texture* t : {cube.get(), volume.get(), mips.get(), array.get()}) cl.transition(*t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        for (std::uint32_t f = 0; f < 6; ++f) cl.copyBufferToTexture(*cube, *upload, kFaceOffset + f * 1024, {0, f});
        cl.copyBufferToTexture(*volume, *upload, kVolumeOffset);
        for (std::uint32_t m = 0; m < 4; ++m) cl.copyBufferToTexture(*mips, *upload, mipOffsets[m], {m, 0});
        for (std::uint32_t l = 0; l < 3; ++l) cl.copyBufferToTexture(*array, *upload, kLayerOffset + l * 1024, {0, l});
        for (rhi::Texture* t : {cube.get(), volume.get(), mips.get(), array.get()}) cl.transition(*t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        cl.transition(*out, VK_IMAGE_LAYOUT_GENERAL);
        cl.bindPipeline(*sample);
        cl.setPushConstants(&push, sizeof(push));
        rhi::Texture* storage[1] = {out.get()};
        cl.setStorageTextures(storage);
        cl.dispatch(1, 1, 1);
        cl.transition(*out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        cl.copyTextureToBuffer(*readback, 0, *out);
    });
    {
        const auto* r = static_cast<const float*>(readback->map());
        const auto unorm = [](Rgba c) { return std::array<float, 4>{c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f}; };
        std::array<std::array<float, 4>, 16> want{};
        for (std::uint32_t f = 0; f < 6; ++f) want[f] = unorm(faceColour(f));
        want[6] = unorm(voxelColour(1, 2, 3));
        want[7] = unorm(voxelColour(3, 0, 2));
        want[8] = unorm(mipColour(1));
        want[9] = unorm(mipColour(2));
        want[10] = unorm(mipColour(3));
        want[11] = unorm(mipColour(2));
        for (std::uint32_t l = 0; l < 3; ++l) want[12 + l] = unorm(layerColour(l));
        want[15] = {-1.0f, -1.0f, -1.0f, -1.0f};
        static const char* names[16] = {"cube +X", "cube -X", "cube +Y", "cube -Y", "cube +Z", "cube -Z", "volume (1,2,3)", "volume (3,0,2)",
                                        "mip 1",   "mip 2",   "mip 3",   "mip 2 load", "layer 0",  "layer 1", "layer 2",        "untouched"};
        for (int i = 0; i < 16; ++i) {
            check(nearTo(r + i * 4, want[i].data()), std::format("{}: got ({:.3f}, {:.3f}, {:.3f}, {:.3f}) want ({:.3f}, {:.3f}, {:.3f}, {:.3f})", names[i],
                                                                 r[i * 4], r[i * 4 + 1], r[i * 4 + 2], r[i * 4 + 3], want[i][0], want[i][1], want[i][2], want[i][3]));
        }
    }

    // storage views of a mip level / a layer / a volume's mip level
    constexpr VkImageUsageFlags kStore = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    auto mipStore = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, {8, 8, 1}, 2, 1, kStore, "rhi mip store"});
    auto layerStore = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, {4, 4, 1}, 1, 3, kStore, "rhi layer store"});
    auto volumeStore = dev.createTexture({VK_IMAGE_TYPE_3D, VK_FORMAT_R32G32B32A32_SFLOAT, {4, 4, 4}, 2, 1, kStore, "rhi volume store"});
    auto store = dev.createComputePipeline({sh.texStore.shader(), "rhi tex store"});
    auto readStore = dev.createBuffer({4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi store readback"});
    if (!mipStore || !layerStore || !volumeStore || !store || !readStore) {
        check(false, "storage view resource / pipeline creation");
        report(dev, "textures", before);
        return;
    }
    dev.submitAndWait([&](rhi::CommandList& cl) {
        for (rhi::Texture* t : {mipStore.get(), layerStore.get(), volumeStore.get()}) cl.transition(*t, VK_IMAGE_LAYOUT_GENERAL);
        cl.bindPipeline(*store);
        const rhi::StorageView views[3] = {{mipStore.get(), 1, 0}, {layerStore.get(), 0, 2}, {volumeStore.get(), 1, 0}};
        cl.setStorageViews(views);
        cl.dispatch(1, 1, 1);
        for (rhi::Texture* t : {mipStore.get(), layerStore.get(), volumeStore.get()}) cl.transition(*t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        cl.copyTextureToBuffer(*readStore, 0, *mipStore, {1, 0});
        cl.copyTextureToBuffer(*readStore, 1024, *layerStore, {0, 2});
        cl.copyTextureToBuffer(*readStore, 2048, *volumeStore, {1, 0});
    });
    {
        const auto* base = static_cast<const std::uint8_t*>(readStore->map());
        const auto texel = [&](std::uint64_t offset, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t h) {
            return reinterpret_cast<const float*>(base + offset + (static_cast<std::size_t>(z) * h + y) * 256 + x * 16);
        };
        int bad = 0;
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                const float wantMip[4] = {x / 4.0f, y / 4.0f, 0.5f, 1.0f};
                const float wantLayer[4] = {0.25f, static_cast<float>(x), static_cast<float>(y), 1.0f};
                if (!nearTo(texel(0, x, y, 0, 4), wantMip) || !nearTo(texel(1024, x, y, 0, 4), wantLayer)) ++bad;
            }
        }
        check(bad == 0, std::format("storage view of mip 1 / layer 2: {} bad texel(s)", bad));
        int badVol = 0;
        for (std::uint32_t z = 0; z < 2; ++z) {
            for (std::uint32_t y = 0; y < 2; ++y) {
                for (std::uint32_t x = 0; x < 2; ++x) {
                    const float want[4] = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), 0.5f};
                    if (!nearTo(texel(2048, x, y, z, 2), want)) ++badVol;
                }
            }
        }
        check(badVol == 0, std::format("storage view of the volume's mip 1: {} bad texel(s)", badVol));
    }

    // render target views of a mip level / a layer
    auto target = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {8, 8, 1}, 2, 3, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                     "rhi layered target"});
    auto readTarget = dev.createBuffer({4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi target readback"});
    if (!target || !readTarget) {
        check(false, "render target view resource creation");
        report(dev, "textures", before);
        return;
    }
    dev.submitAndWait([&](rhi::CommandList& cl) {
        cl.transition(*target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        for (int pass = 0; pass < 2; ++pass) {
            rhi::RenderingInfo ri;
            ri.colorCount = 1;
            ri.color[0].texture = target.get();
            ri.color[0].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ri.color[0].mip = pass == 0 ? 0 : 1;
            ri.color[0].layer = pass == 0 ? 1 : 2;
            ri.color[0].clear.float32[pass == 0 ? 0 : 2] = 1.0f;
            ri.color[0].clear.float32[3] = 1.0f;
            ri.width = ri.height = pass == 0 ? 8 : 4;
            cl.beginRendering(ri);
            cl.endRendering();
        }
        cl.transition(*target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        cl.copyTextureToBuffer(*readTarget, 0, *target, {0, 1});
        cl.copyTextureToBuffer(*readTarget, 2048, *target, {1, 2});
    });
    {
        const auto* base = static_cast<const std::uint8_t*>(readTarget->map());
        int bad = 0;
        for (std::uint32_t y = 0; y < 8; ++y) {
            for (std::uint32_t x = 0; x < 8; ++x) {
                const std::uint8_t* p = base + y * 256 + x * 4;
                if (p[0] != 255 || p[1] != 0 || p[2] != 0 || p[3] != 255) ++bad;
            }
        }
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                const std::uint8_t* p = base + 2048 + y * 256 + x * 4;
                if (p[0] != 0 || p[1] != 0 || p[2] != 255 || p[3] != 255) ++bad;
            }
        }
        check(bad == 0, std::format("render target views of (mip 0, layer 1) / (mip 1, layer 2): {} bad texel(s)", bad));
    }
    report(dev, "textures", before);
}

// Gap 2: buffers the shaders write, atomics.
void testBuffers(rhi::Device& dev, const Shaders& sh) {
    const int before = failures;
    if (!sh.buf.ok) {
        check(false, "buffer shader did not compile");
        report(dev, "buffer UAVs", before);
        return;
    }
    constexpr std::uint32_t kThreads = 1000;
    constexpr VkBufferUsageFlags kUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto data = dev.createBuffer({kThreads * 2 * 4, kUsage, rhi::Memory::Device, "rhi rw data"});
    auto counters = dev.createBuffer({64, kUsage, rhi::Memory::Device, "rhi rw counters"});
    auto zeros = dev.createBuffer({64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Upload, "rhi zeros"});
    auto readData = dev.createBuffer({kThreads * 2 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi rw data readback"});
    auto readCounters = dev.createBuffer({64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi rw counters readback"});
    auto pipeline = dev.createComputePipeline({sh.buf.shader(), "rhi buf"});
    if (!data || !counters || !zeros || !readData || !readCounters || !pipeline) {
        check(false, "buffer resource / pipeline creation");
        report(dev, "buffer UAVs", before);
        return;
    }
    check(dev.caps().backend == rhi::Backend::Vulkan || (data->heapIndexRw() != 0 && data->heapIndexRw() != data->heapIndex()), "heapIndexRw on DX12");
    std::memset(zeros->map(), 0, 64);
    struct BufPush {
        std::uint32_t data[2], counters[2], count, pad[3];
    } push{};
    const auto d = data->shaderRefRw(0), c = counters->shaderRefRw(0);
    push.data[0] = d[0];
    push.data[1] = d[1];
    push.counters[0] = c[0];
    push.counters[1] = c[1];
    push.count = kThreads;
    dev.submitAndWait([&](rhi::CommandList& cl) {
        cl.copyBuffer(*counters, 0, *zeros, 0, 64);
        cl.bufferBarrier(*counters, rhi::BufferAccess::ShaderWrite);
        cl.bufferBarrier(*data, rhi::BufferAccess::ShaderWrite);
        cl.bindPipeline(*pipeline);
        cl.setPushConstants(&push, sizeof(push));
        cl.dispatch((kThreads + 63) / 64, 1, 1);
        cl.bufferBarrier(*data);  // writes -> the copies out (DX12: UAV -> shader read state, then the copy state)
        cl.bufferBarrier(*counters);
        cl.copyBuffer(*readData, 0, *data, 0, kThreads * 2 * 4);
        cl.copyBuffer(*readCounters, 0, *counters, 0, 64);
    });
    const auto* w = static_cast<const std::uint32_t*>(readData->map());
    int bad = 0;
    for (std::uint32_t i = 0; i < kThreads; ++i) {
        if (w[i] != 2 * i + 1 || w[kThreads + i] != 2 * i + 2) ++bad;
    }
    check(bad == 0, std::format("data words: {} bad", bad));
    const auto* k = static_cast<const std::uint32_t*>(readCounters->map());
    std::uint32_t buckets[8] = {};
    for (std::uint32_t i = 0; i < kThreads; ++i) buckets[i & 7] += i;
    check(k[0] == kThreads, std::format("atomic counter {} (want {})", k[0], kThreads));
    check(k[1] == kThreads * (kThreads - 1) / 2, std::format("sum of the atomic's returned values {} (want {})", k[1], kThreads * (kThreads - 1) / 2));
    for (int b = 0; b < 8; ++b) check(k[2 + b] == buckets[b], std::format("bucket {}: {} (want {})", b, k[2 + b], buckets[b]));
    report(dev, "buffer UAVs", before);
}

// Gap 3: vertex input (two bindings, one per instance), index buffer, drawIndexed, indirect draws with a count
void testVertex(rhi::Device& dev, const Shaders& sh) {
    const int before = failures;
    if (!sh.vtxVs.ok || !sh.vtxPs.ok) {
        check(false, "vertex shaders did not compile");
        report(dev, "vertex / index / indirect draws", before);
        return;
    }
    struct Vtx {
        float x, y;
        std::uint8_t r, g, b, a;
    };
    // triangle k around (cx, cy): the base positions; shifts are applied per instance
    struct Tri {
        float cx, cy;
        Rgba colour;
    };
    const Tri tris[5] = {{-0.5f, 0.5f, {255, 0, 0, 255}},   {0.5f, 0.9f, {0, 255, 0, 255}}, {-0.5f, -0.1f, {0, 0, 255, 255}},
                         {0.5f, -0.1f, {255, 255, 0, 255}}, {0.0f, 0.0f, {255, 255, 255, 255}}};
    std::array<Vtx, 16> verts{};  // [0] is junk: the binding starts at offset 12
    verts[0] = {9.0f, 9.0f, 1, 2, 3, 4};
    for (int k = 0; k < 5; ++k) {
        const Tri& t = tris[k];
        verts[1 + 3 * k + 0] = {t.cx - 0.2f, t.cy - 0.2f, t.colour[0], t.colour[1], t.colour[2], t.colour[3]};
        verts[1 + 3 * k + 1] = {t.cx + 0.2f, t.cy - 0.2f, t.colour[0], t.colour[1], t.colour[2], t.colour[3]};
        verts[1 + 3 * k + 2] = {t.cx, t.cy + 0.2f, t.colour[0], t.colour[1], t.colour[2], t.colour[3]};
    }
    const float shifts[4][2] = {{0.0f, 0.0f}, {0.0f, -0.4f}, {0.0f, -0.4f}, {0.0f, 0.0f}};
    const std::uint16_t indices[10] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0, 1, 2, 1, 2, 0};  // the first 8 bytes are junk
    // draw records, 32 bytes apart: tri2 once from instance 2; tri3 twice from instance 0; tri4 (not drawn: the count says 2)
    struct Cmd {
        std::uint32_t indexCount, instanceCount, firstIndex;
        std::int32_t vertexOffset;
        std::uint32_t firstInstance, pad[3];
    };
    const Cmd cmds[3] = {{3, 1, 0, 6, 2, {}}, {3, 2, 0, 9, 0, {}}, {3, 1, 0, 12, 0, {}}};
    static_assert(sizeof(Cmd) == 32);

    constexpr std::uint32_t kTarget = 64;
    auto vb = dev.createBuffer({sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, rhi::Memory::Upload, "rhi vertices"});
    auto ib = dev.createBuffer({sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, rhi::Memory::Upload, "rhi indices"});
    auto instances = dev.createBuffer({sizeof(shifts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, rhi::Memory::Upload, "rhi instances"});
    auto staging = dev.createBuffer({256, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Upload, "rhi indirect staging"});
    constexpr VkBufferUsageFlags kIndirect = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto args = dev.createBuffer({sizeof(cmds), kIndirect, rhi::Memory::Device, "rhi draw args"});
    auto countBuffer = dev.createBuffer({64, kIndirect, rhi::Memory::Device, "rhi draw count"});
    const std::uint32_t pitch = rhi::bufferRowPitch(VK_FORMAT_R8G8B8A8_UNORM, kTarget);
    auto target = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {kTarget, kTarget, 1}, 1, 1,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "rhi vertex target"});
    auto readback = dev.createBuffer({static_cast<std::uint64_t>(pitch) * kTarget, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi vertex readback"});
    if (!vb || !ib || !instances || !staging || !args || !countBuffer || !target || !readback) {
        check(false, "vertex resource creation");
        report(dev, "vertex / index / indirect draws", before);
        return;
    }
    std::memcpy(vb->map(), verts.data(), sizeof(verts));
    std::memcpy(ib->map(), indices, sizeof(indices));
    std::memcpy(instances->map(), shifts, sizeof(shifts));
    std::memcpy(staging->map(), cmds, sizeof(cmds));
    *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(staging->map()) + 128 + 16) = 2;  // the count, at countOffset 16 of the count buffer

    const VkVertexInputBindingDescription bindings[2] = {{0, sizeof(Vtx), VK_VERTEX_INPUT_RATE_VERTEX}, {1, 8, VK_VERTEX_INPUT_RATE_INSTANCE}};
    const VkVertexInputAttributeDescription attributes[3] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 8}, {2, 1, VK_FORMAT_R32G32_SFLOAT, 0}};
    rhi::GraphicsPipelineDesc gd;
    gd.vs = sh.vtxVs.shader();
    gd.ps = sh.vtxPs.shader();
    gd.colorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    gd.vertexBindings = bindings;
    gd.vertexAttributes = attributes;
    gd.name = "rhi vertex";
    auto pipeline = dev.createGraphicsPipeline(gd);
    if (!pipeline) {
        check(false, "vertex pipeline creation");
        report(dev, "vertex / index / indirect draws", before);
        return;
    }
    const bool indirect = dev.caps().drawIndirectCount;
    dev.submitAndWait([&](rhi::CommandList& cl) {
        if (indirect) {
            cl.copyBuffer(*args, 0, *staging, 0, sizeof(cmds));
            cl.copyBuffer(*countBuffer, 0, *staging, 128, 64);
            cl.bufferBarrier(*args);  // (Vulkan: the transfer writes before the indirect reads; DX12 transitions in the draw)
            cl.bufferBarrier(*countBuffer);
        }
        cl.transition(*target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        rhi::RenderingInfo ri;
        ri.colorCount = 1;
        ri.color[0].texture = target.get();
        ri.color[0].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ri.color[0].clear.float32[3] = 1.0f;
        ri.width = ri.height = kTarget;
        cl.beginRendering(ri);
        cl.bindPipeline(*pipeline);
        rhi::Buffer* buffers[2] = {vb.get(), instances.get()};
        const std::uint64_t offsets[2] = {sizeof(Vtx), 0};  // skip the junk vertex
        const std::uint32_t strides[2] = {sizeof(Vtx), 8};
        cl.setVertexBuffers(0, buffers, offsets, strides);
        cl.setIndexBuffer(*ib, 8, VK_INDEX_TYPE_UINT16);  // skip the junk indices
        cl.drawIndexed(3, 1, 0, 0, 0);                    // triangle 0, shift 0
        cl.drawIndexed(3, 1, 3, 3, 1);                    // triangle 1 (indices 1 2 0 + vertex offset 3), shift 1
        if (indirect) cl.drawIndexedIndirectCount(*args, 0, *countBuffer, 16, 3, sizeof(Cmd));
        cl.endRendering();
        cl.transition(*target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        cl.copyTextureToBuffer(*readback, 0, *target);
    });
    // probes at the triangles' centroids after the shifts
    const auto* px = static_cast<const std::uint8_t*>(readback->map());
    const auto probe = [&](float cx, float cy, Rgba want, const char* what) {
        const std::uint32_t x = static_cast<std::uint32_t>((cx * 0.5f + 0.5f) * kTarget);
        const std::uint32_t y = static_cast<std::uint32_t>((0.5f - (cy - 0.2f / 3.0f) * 0.5f) * kTarget);
        const std::uint8_t* p = px + y * pitch + x * 4;
        check(p[0] == want[0] && p[1] == want[1] && p[2] == want[2] && p[3] == want[3],
              std::format("{} at pixel ({}, {}): ({}, {}, {}, {}) want ({}, {}, {}, {})", what, x, y, p[0], p[1], p[2], p[3], want[0], want[1], want[2], want[3]));
    };
    const Rgba black{0, 0, 0, 255};
    probe(-0.5f, 0.5f, tris[0].colour, "direct draw, triangle 0");
    probe(0.5f, 0.5f, tris[1].colour, "direct draw, triangle 1 (firstInstance 1, vertex offset, first index)");
    if (indirect) {
        probe(-0.5f, -0.5f, tris[2].colour, "indirect record 0 (firstInstance 2)");
        probe(0.5f, -0.1f, tris[3].colour, "indirect record 1, instance 0");
        probe(0.5f, -0.5f, tris[3].colour, "indirect record 1, instance 1");
        probe(0.0f, 0.0f, black, "indirect record 2 (beyond the count)");
    } else {
        probe(-0.5f, -0.5f, black, "triangle 2 (no indirect draws)");
    }
    probe(-0.9f, -0.9f, black, "background");
    report(dev, indirect ? "vertex / index / indirect draws" : "vertex / index / direct draws", before);
    if (!indirect) skip(dev, "indirect draws with a count", "no drawIndirectCount");
}

// the pixel of src/vk/raytracing.cpp selfTest's scene (rt_query.comp's encoding); `ambiguous`: on a triangle edge (either result is valid)
std::uint32_t expectedRay(std::uint32_t px, std::uint32_t py, std::uint32_t size, bool& ambiguous, float dz = 0.0f) {
    constexpr std::uint32_t kIndexA = 7, kIndexB = 9;
    const float x = (static_cast<float>(px) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
    const float y = (static_cast<float>(py) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
    const auto onEdge = [](float a, float b) { return std::abs(a - b) < 1e-3f; };
    ambiguous = onEdge(x + y, 0.0f) || onEdge(std::abs(x), 0.8f) || onEdge(std::abs(y), 0.8f) || onEdge(x, 0.0f);
    // instance A: identity, z = 0 (t = 10), the triangle (-0.8,-0.8) (0.8,-0.8) (-0.8,0.8), opaque
    if (x >= -0.8f && y >= -0.8f && x + y <= 0.0f) return (1u << 31) | (static_cast<std::uint32_t>(40.0f + 4.0f * dz) << 8) | (kIndexA + 1);
    // instance B: rotated 180 degrees about z, z = 1 (t = 11), non-opaque: the shader accepts barycentric u < 0.5, i.e. object x = -x < 0
    if (x <= 0.8f && y <= 0.8f && x + y >= 0.0f && x > 0.0f) return (1u << 31) | (static_cast<std::uint32_t>(44.0f + 4.0f * dz) << 8) | (kIndexB + 1);
    return 0;
}

// Gap 4: ray queries
void testRayQuery(rhi::Device& dev, const Shaders& sh) {
    const int before = failures;
    if (!dev.caps().rayQuery) {
        skip(dev, "ray query", dev.caps().backend == rhi::Backend::Vulkan ? "no ray query / acceleration structure with a bindless TLAS array" : "no DXR tier 1.1");
        return;
    }
    if (!sh.rt.ok) {
        check(false, "ray query shader did not compile");
        report(dev, "ray query", before);
        return;
    }
    constexpr std::uint32_t kRtSize = 64;
    constexpr std::uint32_t kIndexA = 7, kIndexB = 9;
    const float verts[9] = {-0.8f, -0.8f, 0.0f, 0.8f, -0.8f, 0.0f, -0.8f, 0.8f, 0.0f};
    const std::uint32_t idx[3] = {0, 1, 2};
    auto vertices = dev.createBuffer({sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, rhi::Memory::Upload, "rhi rt vertices"});
    auto indices = dev.createBuffer({sizeof(idx), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, rhi::Memory::Upload, "rhi rt indices"});
    auto instances = dev.createBuffer({2 * sizeof(VkAccelerationStructureInstanceKHR), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi rt instances"});
    constexpr std::uint32_t kBytes = kRtSize * kRtSize * 4;
    auto output = dev.createBuffer({kBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Device, "rhi rt output"});
    auto readback = dev.createBuffer({kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi rt readback"});
    auto pipeline = dev.createComputePipeline({sh.rt.shader(), "rhi ray query"});
    if (!vertices || !indices || !instances || !output || !readback || !pipeline) {
        check(false, "ray query resource / pipeline creation");
        report(dev, "ray query", before);
        return;
    }
    std::memcpy(vertices->map(), verts, sizeof(verts));
    std::memcpy(indices->map(), idx, sizeof(idx));
    rhi::TriangleGeometry tri;
    tri.vertices = vertices.get();
    tri.vertexCount = 3;
    tri.indices = indices.get();
    tri.indexCount = 3;
    tri.indexType = VK_INDEX_TYPE_UINT32;
    tri.opaque = true;
    auto blas = dev.createBlas({&tri, 1});
    auto tlas = dev.createTlas(2);
    if (!blas || !tlas) {
        check(false, "acceleration structure creation");
        report(dev, "ray query", before);
        return;
    }
    check(!blas->isTlas() && tlas->isTlas() && blas->address() != 0 && (tlas->tlasSlot() != 0 || dev.caps().backend == rhi::Backend::Vulkan),"acceleration structure properties");  // (Vulkan: slot 0 of the bindless array is valid)
    // two instances of the BLAS: A as it is (opaque), B rotated 180 degrees about z, 1 further away, forced non-opaque. The record is the same 64
    // bytes on both APIs (VkAccelerationStructureInstanceKHR = D3D12_RAYTRACING_INSTANCE_DESC)
    VkAccelerationStructureInstanceKHR inst[2]{};
    inst[0].transform = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    inst[0].instanceCustomIndex = kIndexA;
    inst[0].mask = 0xFF;
    inst[0].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    inst[0].accelerationStructureReference = blas->address();
    inst[1] = inst[0];
    inst[1].transform = {{{-1, 0, 0, 0}, {0, -1, 0, 0}, {0, 0, 1, 1}}};
    inst[1].instanceCustomIndex = kIndexB;
    inst[1].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
    static_assert(sizeof(inst) == 128);
    std::memcpy(instances->map(), inst, sizeof(inst));
    struct RtPush {
        std::uint32_t out[2], tlas, width, height, pad[3];
    } push{};
    const auto o = output->shaderRefRw(0);
    push.out[0] = o[0];
    push.out[1] = o[1];
    push.tlas = tlas->tlasSlot();
    push.width = push.height = kRtSize;
    dev.submitAndWait([&](rhi::CommandList& cl) {
        cl.bufferBarrier(*output, rhi::BufferAccess::ShaderWrite);
        cl.buildBlas(*blas);
        cl.buildTlas(*tlas, *instances, 0, 2);
        cl.bindPipeline(*pipeline);
        cl.setPushConstants(&push, sizeof(push));
        cl.dispatch(kRtSize / 8, kRtSize / 8, 1);
        cl.bufferBarrier(*output);
        cl.copyBuffer(*readback, 0, *output, 0, kBytes);
    });
    const auto* px = static_cast<const std::uint32_t*>(readback->map());
    int bad = 0, hitsA = 0, hitsB = 0, misses = 0, skipped = 0;
    std::string first;
    for (std::uint32_t y = 0; y < kRtSize; ++y) {
        for (std::uint32_t x = 0; x < kRtSize; ++x) {
            bool ambiguous = false;
            const std::uint32_t want = expectedRay(x, y, kRtSize, ambiguous);
            const std::uint32_t got = px[y * kRtSize + x];
            if (ambiguous) {
                ++skipped;
                continue;
            }
            if (got != want) {
                if (bad++ == 0) first = std::format("pixel ({}, {}) = 0x{:08x}, expected 0x{:08x}", x, y, got, want);
                continue;
            }
            (got == 0 ? misses : (got & 0xFF) == kIndexA + 1 ? hitsA : hitsB)++;
        }
    }
    check(bad == 0 && hitsA > 0 && hitsB > 0 && misses > 0,
          std::format("{} wrong pixel(s) {}; {} opaque hits, {} alpha-tested hits, {} misses, {} edge pixels skipped", bad, first, hitsA, hitsB, misses, skipped));
    report(dev, "ray query", before);
}


// Gap 6 (rt_scene's needs, hlsl-rhi.md 8): BLAS builds by raw addresses in a batch with pooled scratch, a refit (update), compacted sizes + compaction, a
// TLAS built from a raw address, buffer fills and memory barriers.
void testAccelOps(rhi::Device& dev, const Shaders& sh) {
    const int before = failures;
    if (!dev.caps().rayQuery) {
        skip(dev, "acceleration structure ops", "no ray query");
        return;
    }
    if (!sh.rt.ok) {
        check(false, "ray query shader did not compile");
        report(dev, "acceleration structure ops", before);
        return;
    }
    constexpr std::uint32_t kRtSize = 64;
    constexpr std::uint32_t kIndexA = 7, kIndexB = 9;
    constexpr std::uint32_t kBytes = kRtSize * kRtSize * 4;
    float verts[9] = {-0.8f, -0.8f, 0.0f, 0.8f, -0.8f, 0.0f, -0.8f, 0.8f, 0.0f};
    const std::uint32_t idx[3] = {0, 1, 2};
    const float verts2[9] = {-0.2f, -0.2f, 3.0f, 0.2f, -0.2f, 3.0f, -0.2f, 0.2f, 3.0f};  // a second BLAS in the batch (not traced)
    auto vertices = dev.createBuffer({sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, rhi::Memory::Upload, "rhi accel vertices"});
    auto vertices2 = dev.createBuffer({sizeof(verts2), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, rhi::Memory::Upload, "rhi accel vertices 2"});
    auto indices = dev.createBuffer({sizeof(idx), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, rhi::Memory::Upload, "rhi accel indices"});
    auto instances = dev.createBuffer({2 * sizeof(VkAccelerationStructureInstanceKHR), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi accel instances"});
    auto output = dev.createBuffer({kBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Device, "rhi accel output"});
    auto readback = dev.createBuffer({kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi accel readback"});
    auto pipeline = dev.createComputePipeline({sh.rt.shader(), "rhi accel ray query"});
    auto query = dev.createSizeQuery(4);
    if (!vertices || !vertices2 || !indices || !instances || !output || !readback || !pipeline || !query) {
        check(false, "acceleration structure ops: resource / pipeline creation");
        report(dev, "acceleration structure ops", before);
        return;
    }
    std::memcpy(vertices->map(), verts, sizeof(verts));
    std::memcpy(vertices2->map(), verts2, sizeof(verts2));
    std::memcpy(indices->map(), idx, sizeof(idx));
    // raw addresses (the geometry of a BLAS in the renderer's own memory) and the second BLAS's geometry brought by the build
    rhi::TriangleGeometry tri;
    tri.vertexAddress = vertices->gpuAddress();
    tri.vertexCount = 3;
    tri.indexAddress = indices->gpuAddress();
    tri.indexCount = 3;
    tri.indexType = VK_INDEX_TYPE_UINT32;
    tri.opaque = true;
    rhi::TriangleGeometry tri2 = tri;
    tri2.vertexAddress = vertices2->gpuAddress();
    rhi::BlasDesc bd;
    bd.geometries = std::span<const rhi::TriangleGeometry>(&tri, 1);
    bd.allowCompaction = true;
    bd.name = "rhi accel blas";
    auto blas = dev.createBlas(bd);
    auto blas2 = dev.createBlas(bd);
    rhi::BlasDesc bu = bd;
    bu.allowCompaction = false;
    bu.allowUpdate = true;
    bu.name = "rhi accel blas (refit)";
    auto blasU = dev.createBlas(bu);
    rhi::TlasDesc td;
    td.maxInstances = 2;
    td.preferFastBuild = true;
    td.name = "rhi accel tlas";
    auto tlas = dev.createTlas(td);
    if (!blas || !blas2 || !blasU || !tlas) {
        check(false, "acceleration structure ops: creation");
        report(dev, "acceleration structure ops", before);
        return;
    }
    check(blas->storageBytes() > 0 && tlas->storageBytes() > 0, "storageBytes");
    const auto writeInstances = [&](std::uint64_t blasAddress) {
        VkAccelerationStructureInstanceKHR inst[2]{};
        inst[0].transform = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
        inst[0].instanceCustomIndex = kIndexA;
        inst[0].mask = 0xFF;
        inst[0].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst[0].accelerationStructureReference = blasAddress;
        inst[1] = inst[0];
        inst[1].transform = {{{-1, 0, 0, 0}, {0, -1, 0, 0}, {0, 0, 1, 1}}};
        inst[1].instanceCustomIndex = kIndexB;
        inst[1].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
        std::memcpy(instances->map(), inst, sizeof(inst));
    };
    struct RtPush {
        std::uint32_t out[2], tlas, width, height, pad[3];
    } push{};
    const auto o = output->shaderRefRw(0);
    push.out[0] = o[0];
    push.out[1] = o[1];
    push.tlas = tlas->tlasSlot();
    push.width = push.height = kRtSize;
    // trace + compare with the analytic result (`dz`: how far the geometry moved along the ray)
    const auto trace = [&](const char* what, float dz) {
        const auto* px = static_cast<const std::uint32_t*>(readback->map());
        int bad = 0, hits = 0;
        std::string first;
        for (std::uint32_t y = 0; y < kRtSize; ++y) {
            for (std::uint32_t x = 0; x < kRtSize; ++x) {
                bool ambiguous = false;
                const std::uint32_t want = expectedRay(x, y, kRtSize, ambiguous, dz);
                if (ambiguous) continue;
                const std::uint32_t got = px[y * kRtSize + x];
                if (got != want) {
                    if (bad++ == 0) first = std::format("pixel ({}, {}) = 0x{:08x}, expected 0x{:08x}", x, y, got, want);
                } else if (got != 0) {
                    ++hits;
                }
            }
        }
        check(bad == 0 && hits > 0, std::format("{}: {} wrong pixel(s) {}; {} hits", what, bad, first, hits));
    };
    const auto dispatchAndRead = [&](rhi::CommandList& cl) {
        cl.bufferBarrier(*output, rhi::BufferAccess::ShaderWrite);
        cl.bindPipeline(*pipeline);
        cl.setPushConstants(&push, sizeof(push));
        cl.dispatch(kRtSize / 8, kRtSize / 8, 1);
        cl.bufferBarrier(*output);
        cl.copyBuffer(*readback, 0, *output, 0, kBytes);
    };

    // 1. batch of two BLASes (the second with geometry of its own) + sizes + a TLAS from a raw address; the output buffer is filled first
    writeInstances(blas->address());
    dev.submitAndWait([&](rhi::CommandList& cl) {
        cl.fillBuffer(*output, 0, kBytes, 0xDEADBEEFu);
        cl.memoryBarrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        const rhi::BlasBuild builds[3] = {{blas.get(), {}, false}, {blas2.get(), std::span<const rhi::TriangleGeometry>(&tri2, 1), false}, {blasU.get(), {}, false}};
        cl.buildBlases(builds);
        rhi::AccelerationStructure* list[2] = {blas.get(), blas2.get()};
        cl.queryCompactedSizes(*query, 0, list);
        cl.buildTlasAt(*tlas, instances->gpuAddress(), 2);
        dispatchAndRead(cl);
    });
    trace("batched builds, raw addresses", 0.0f);
    std::uint64_t sizes[2]{};
    check(query->read(0, 2, sizes) && sizes[0] > 0 && sizes[1] > 0 && sizes[0] <= blas->storageBytes(),
          std::format("compacted sizes {} / {} bytes of {}", sizes[0], sizes[1], blas->storageBytes()));

    // 2. compaction: copy into a structure of the queried size, point the instances at it
    auto compacted = dev.createBlasStorage(sizes[0], "rhi accel blas compacted");
    if (!compacted) {
        check(false, "createBlasStorage");
        report(dev, "acceleration structure ops", before);
        return;
    }
    writeInstances(compacted->address());
    dev.submitAndWait([&](rhi::CommandList& cl) {
        cl.compactBlas(*compacted, *blas);
        cl.buildTlasAt(*tlas, instances->gpuAddress(), 2);
        dispatchAndRead(cl);
    });
    trace(std::format("compacted ({} -> {} bytes)", blas->storageBytes(), compacted->storageBytes()).c_str(), 0.0f);

    // 3. refit: the vertices move 5 units along the ray, the update-capable BLAS is refitted in place
    writeInstances(blasU->address());
    dev.submitAndWait([&](rhi::CommandList& cl) {
        cl.buildTlasAt(*tlas, instances->gpuAddress(), 2);
        dispatchAndRead(cl);
    });
    trace("update-capable BLAS, first build", 0.0f);
    for (int i = 0; i < 3; ++i) verts[i * 3 + 2] = 5.0f;
    std::memcpy(vertices->map(), verts, sizeof(verts));
    vertices->flush();
    dev.submitAndWait([&](rhi::CommandList& cl) {
        const rhi::BlasBuild b = {blasU.get(), {}, true};
        cl.buildBlases(std::span<const rhi::BlasBuild>(&b, 1));
        cl.buildTlasAt(*tlas, instances->gpuAddress(), 2);
        dispatchAndRead(cl);
    });
    trace("refit (update)", 5.0f);

    // 4. fillBuffer of a range
    auto fb = dev.createBuffer({256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Device, "rhi fill"});
    auto fbRead = dev.createBuffer({256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi fill readback"});
    if (fb && fbRead) {
        dev.submitAndWait([&](rhi::CommandList& cl) {
            cl.fillBuffer(*fb, 0, 256, 0u);
            cl.bufferBarrier(*fb);
            cl.fillBuffer(*fb, 64, 128, 0x01020304u);
            cl.bufferBarrier(*fb);
            cl.copyBuffer(*fbRead, 0, *fb, 0, 256);
        });
        const auto* w = static_cast<const std::uint32_t*>(fbRead->map());
        int bad = 0;
        for (int i = 0; i < 64; ++i) bad += w[i] != (i >= 16 && i < 48 ? 0x01020304u : 0u) ? 1 : 0;
        check(bad == 0, std::format("fillBuffer: {} wrong words", bad));
    } else {
        check(false, "fillBuffer resources");
    }
    report(dev, "acceleration structure ops", before);
}

// Gap 5 (the pass migration's needs, hlsl-rhi.md 8): eight storage images in one dispatch (u0..u7), clears of mip levels, per-mip layout transitions,
// texture -> texture copies of a box, and (Vulkan) blits down a mip chain.
void testTextureOps(rhi::Device& dev, const Shaders& sh) {
    const int before = failures;
    if (!sh.store8.ok) {
        check(false, "store8 shader did not compile");
        report(dev, "texture ops", before);
        return;
    }
    // ---- eight storage images
    constexpr VkImageUsageFlags kStore = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    std::array<std::unique_ptr<rhi::Texture>, 8> imgs;
    for (std::size_t i = 0; i < imgs.size(); ++i) imgs[i] = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R32_SFLOAT, {4, 2, 1}, 1, 1, kStore, "rhi store8"});
    auto store8 = dev.createComputePipeline({sh.store8.shader(), "rhi store8"});
    auto read = dev.createBuffer({16384, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi texops readback"});
    bool created = store8 != nullptr && read != nullptr;
    for (auto& t : imgs) created = created && t != nullptr;
    if (!created) {
        check(false, "texture ops resource / pipeline creation");
        report(dev, "texture ops", before);
        return;
    }
    dev.submitAndWait([&](rhi::CommandList& cl) {
        std::array<rhi::Texture*, 8> raw{};
        for (std::size_t i = 0; i < imgs.size(); ++i) {
            raw[i] = imgs[i].get();
            cl.transition(*imgs[i], VK_IMAGE_LAYOUT_GENERAL);
        }
        cl.bindPipeline(*store8);
        cl.setStorageTextures(raw);
        cl.dispatch(1, 1, 1);
        cl.computeBarrier();
        for (std::size_t i = 0; i < imgs.size(); ++i) {
            cl.transition(*imgs[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            cl.copyTextureToBuffer(*read, i * 1024, *imgs[i]);
        }
    });
    {
        const auto* base = static_cast<const std::uint8_t*>(read->map());
        int bad = 0;
        for (std::uint32_t i = 0; i < 8; ++i) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                const float a = *reinterpret_cast<const float*>(base + i * 1024 + x * 4);
                const float b = *reinterpret_cast<const float*>(base + i * 1024 + 256 + x * 4);
                if (a != static_cast<float>(i + 1) || b != static_cast<float>((i + 1) * 10)) ++bad;
            }
        }
        check(bad == 0, std::format("eight storage images (u0..u7): {} bad texel(s)", bad));
    }

    // ---- clears, per-mip transitions, a box copy
    auto mipTex = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, {8, 8, 1}, 3, 1, kStore, "rhi texops mips"});
    auto copyDst = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, {8, 8, 1}, 1, 1, kStore, "rhi texops copy dst"});
    if (!mipTex || !copyDst) {
        check(false, "texture ops mip texture creation");
        report(dev, "texture ops", before);
        return;
    }
    VkClearColorValue red{}, green{}, blue{};
    red.float32[0] = 1.0f;
    red.float32[3] = 1.0f;
    green.float32[1] = 1.0f;
    green.float32[3] = 1.0f;
    blue.float32[2] = 1.0f;
    blue.float32[3] = 1.0f;
    dev.submitAndWait([&](rhi::CommandList& cl) {
        cl.transition(*mipTex, VK_IMAGE_LAYOUT_GENERAL);
        cl.transition(*copyDst, VK_IMAGE_LAYOUT_GENERAL);
        cl.clearTexture(*mipTex, red, 0, 3);
        cl.clearTexture(*copyDst, blue);
        cl.transitionMips(*mipTex, VK_IMAGE_LAYOUT_GENERAL, 1, 1);  // the same layout again: the first clear is visible to the second
        cl.clearTexture(*mipTex, green, 1, 1);
        // mip 0 is copied into the blue image (a 4 x 4 box from (2, 2) to (1, 1)), the levels end up in different layouts before all are read back
        cl.transitionMips(*mipTex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
        cl.transition(*copyDst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        rhi::TextureCopy box;
        box.srcOffset = {2, 2, 0};
        box.dstOffset = {1, 1, 0};
        box.extent = {4, 4, 1};
        cl.copyTexture(*copyDst, *mipTex, std::span<const rhi::TextureCopy>(&box, 1));
        // (two textures in one barrier)
        const rhi::TextureTransition both[] = {{copyDst.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}, {mipTex.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1, 2}};
        cl.transitions(both);
        cl.copyTextureToBuffer(*read, 0, *mipTex, {0, 0});
        cl.copyTextureToBuffer(*read, 2048, *mipTex, {1, 0});
        cl.copyTextureToBuffer(*read, 3072, *mipTex, {2, 0});
        cl.copyTextureToBuffer(*read, 4096, *copyDst, {0, 0});
    });
    {
        const auto* base = static_cast<const std::uint8_t*>(read->map());
        const auto px = [&](std::uint64_t offset, std::uint32_t x, std::uint32_t y) { return reinterpret_cast<const float*>(base + offset + y * 256 + x * 16); };
        const float wantRed[4] = {1, 0, 0, 1}, wantGreen[4] = {0, 1, 0, 1}, wantBlue[4] = {0, 0, 1, 1};
        check(nearTo(px(0, 3, 3), wantRed) && nearTo(px(0, 7, 7), wantRed), "clear of mips 0..2 (mip 0)");
        check(nearTo(px(2048, 0, 0), wantGreen) && nearTo(px(2048, 3, 3), wantGreen), "clear of mip 1 only");
        check(nearTo(px(3072, 0, 0), wantRed) && nearTo(px(3072, 1, 1), wantRed), "mip 2 keeps the first clear");
        int bad = 0;
        for (std::uint32_t y = 0; y < 8; ++y) {
            for (std::uint32_t x = 0; x < 8; ++x) {
                const bool inside = x >= 1 && x < 5 && y >= 1 && y < 5;
                if (!nearTo(px(4096, x, y), inside ? wantRed : wantBlue)) ++bad;
            }
        }
        check(bad == 0, std::format("copyTexture of a box: {} bad texel(s)", bad));
    }

    // ---- blits down a mip chain
    if (!dev.caps().textureBlit) {
        skip(dev, "texture blit", "no scaling copy on this backend");
    } else {
        auto chain = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, {8, 8, 1}, 3, 1, kStore, "rhi texops blit chain"});
        auto chainRead = dev.createBuffer({4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi blit readback"});
        if (!chain || !chainRead) {
            check(false, "blit resource creation");
        } else {
            VkClearColorValue colour{};
            colour.float32[0] = 0.25f;
            colour.float32[1] = 0.5f;
            colour.float32[2] = 0.75f;
            colour.float32[3] = 1.0f;
            dev.submitAndWait([&](rhi::CommandList& cl) {
                cl.transition(*chain, VK_IMAGE_LAYOUT_GENERAL);
                cl.clearTexture(*chain, colour, 0, 1);
                cl.transitionMips(*chain, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
                cl.transitionMips(*chain, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, 2);
                for (std::uint32_t m = 1; m < 3; ++m) {
                    rhi::TextureBlit b;
                    b.src = {m - 1, 0};
                    b.dst = {m, 0};
                    check(cl.blitTexture(*chain, *chain, std::span<const rhi::TextureBlit>(&b, 1), VK_FILTER_LINEAR), "blitTexture");
                    cl.transitionMips(*chain, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m, 1);
                }
                cl.copyTextureToBuffer(*chainRead, 0, *chain, {1, 0});
                cl.copyTextureToBuffer(*chainRead, 1024, *chain, {2, 0});
            });
            const auto* base = static_cast<const std::uint8_t*>(chainRead->map());
            const auto half4 = [&](std::uint64_t offset, std::uint32_t x, std::uint32_t y, float out[4]) {
                const auto* h = reinterpret_cast<const std::uint16_t*>(base + offset + y * 256 + x * 8);
                for (int c = 0; c < 4; ++c) out[c] = halfToFloat(h[c]);
            };
            const float want[4] = {0.25f, 0.5f, 0.75f, 1.0f};
            float got[4];
            half4(0, 3, 3, got);
            check(nearTo(got, want), std::format("blit to mip 1: ({:.3f}, {:.3f}, {:.3f}, {:.3f})", got[0], got[1], got[2], got[3]));
            half4(1024, 1, 1, got);
            check(nearTo(got, want), std::format("blit to mip 2: ({:.3f}, {:.3f}, {:.3f}, {:.3f})", got[0], got[1], got[2], got[3]));
        }
    }
    report(dev, "texture ops", before);
}

bool runBackend(rhi::Backend backend, bool validation, const Shaders& sh) {
    rhi::DeviceDesc dd;
    dd.backend = backend;
    dd.validation = validation;
    dd.name = "rvk_rhitest";
    dd.rayTracing = true;
    auto dev = backend == rhi::Backend::Vulkan ? rhi::createVulkanDevice(dd) : rhi::createD3D12Device(dd);
    if (!dev) {
        std::printf("[%s] SKIP: no device meeting the floor\n", rhi::backendName(backend));
        return true;
    }
    std::printf("[%s] adapter '%s'", rhi::backendName(backend), dev->caps().adapterName.c_str());
    if (backend == rhi::Backend::D3D12) {
        const std::uint32_t agility = dev->caps().agilitySdkVersion;
        std::printf(", %s\n", agility != 0 ? ("Agility SDK " + std::to_string(agility)).c_str() : "system D3D12 runtime");
    } else {
        std::printf("\n");
    }
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

    std::unique_ptr<rhi::Pipeline> compute = dev->createComputePipeline({sh.cs.shader(), "rhi compute"});
    rhi::GraphicsPipelineDesc gd;
    gd.vs = sh.vs.shader();
    gd.ps = sh.ps.shader();
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
    testTextures(*dev, sh);
    testBuffers(*dev, sh);
    testVertex(*dev, sh);
    testRayQuery(*dev, sh);
    testAccelOps(*dev, sh);
    testTextureOps(*dev, sh);
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
    Shaders sh;
    // the basic check's shaders must compile; a failing gap shader fails only its own check (the compile errors are printed)
    if (!compileBoth("test/rhi_cb.comp.hlsl", "CSMain", "cs_6_6", sh.cs) || !compileBoth("debug/fullscreen.vert.hlsl", "VSMain", "vs_6_6", sh.vs) ||
        !compileBoth("test/rhi_cb.frag.hlsl", "PSMain", "ps_6_6", sh.ps)) {
        return 1;
    }
    compileBoth("test/rhi_tex.comp.hlsl", "CSMain", "cs_6_6", sh.tex);
    compileBoth("test/rhi_texstore.comp.hlsl", "CSMain", "cs_6_6", sh.texStore);
    compileBoth("test/rhi_buf.comp.hlsl", "CSMain", "cs_6_6", sh.buf);
    compileBoth("test/rhi_vtx.vert.hlsl", "VSMain", "vs_6_6", sh.vtxVs);
    compileBoth("test/rhi_vtx.frag.hlsl", "PSMain", "ps_6_6", sh.vtxPs);
    compileBoth("test/rhi_rt.comp.hlsl", "CSMain", "cs_6_6", sh.rt);
    compileBoth("test/rhi_store8.comp.hlsl", "CSMain", "cs_6_6", sh.store8);
    if (vulkan) runBackend(rhi::Backend::Vulkan, validation, sh);
    if (d3d12) runBackend(rhi::Backend::D3D12, validation, sh);
    std::printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
