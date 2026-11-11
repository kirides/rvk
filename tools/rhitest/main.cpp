// rvk_rhitest: the slim RHI (src/rhi/rhi.h) on both backends with the same code and the same HLSL (docs/research/hlsl-rhi.md 5, 6).
//
//   rvk_rhitest [--vulkan] [--d3d12] [--validation | --sync-validation] [--gbv]
//     --validation on D3D12 = the debug layer WITHOUT GPU-based validation; --gbv adds it (2026-11-10, performance.md 34: under GBV the abi probe's
//     largest block (RvkStageData, one load per word) grew the process past 12 GB inside one dispatch, GBV's own instrumentation: our memory stayed
//     at 0.3 GB; use --gbv with RVK_ABI_ONLY / RVK_PICTURE_ONLY / RVK_PARITY_ONLY for focused GBV runs)
//   rvk_rhitest --fsr | --fg [loader dll] [--validation]   FidelityFX on the D3D12 backend (tools/rhitest/ffx.cpp), nothing else
//
// Per backend: a compute pass that reads a constant buffer (b8), a bindless texture + sampler (indices in the constant buffer), a RVK_REF block
// (the frame block: Buffer::shaderRef) and the push constants, writing a storage image that is read back and compared with the CPU; a graphics
// pass (dynamic rendering, fullscreen triangle, constant buffer) into a render target that is read back; a swapchain on a hidden window. Then the
// gap checks, each on both backends from one HLSL source (docs/research/hlsl-rhi.md 6):
//   textures   cube + 3D + mip chain + array layers sampled through the heap (SampleLevel / Load), per-mip / per-layer storage and render target views
//   buffers    read-write buffer references (RVK_BUF_LOAD / STORE / ADD), atomics from 1000 threads
//   vertex     vertex + index buffers (two bindings, one per instance), drawIndexed, drawIndexedIndirectCount with a count smaller than maxDraws
//   parity     production shaders through the RHI (tools/rhitest/parity.cpp): a translated D3D9 draw, ubershader draws, the GI lookup + RT record fetch, compared with
//              the CPU and between the two backends (docs/research/hlsl-rhi.md "DXIL parity (S1)")
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
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/cluster_cull.h"
#include "core/crash_dump.h"
#include "core/profile.h"
#include "enh/tree_wind.h"
#include "rhi/rhi.h"
#include "shader/abi.h"
#include "shader/dxc.h"

namespace rvk::parity {
int run(rhi::Device& dev);  // tools/rhitest/parity.cpp
}
namespace rvk::abiprobe {
int run(rhi::Device& dev);  // tools/rhitest/abi_probe.cpp: every C++ / HLSL shared block read back on the GPU (src/shader/abi_blocks.cpp)
}
namespace rvk::ffxtest {
int run(bool fsr, bool fg, const std::string& dll, bool validation);  // tools/rhitest/ffx.cpp (D3D12 only)
}

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
    Blobs tex, texStore, buf, vtxVs, vtxPs, rt, store8, indCs, indVs, compact, skin, treeWind, picture, dzVs, dzPsOld, dzPsNone, dzFs, dzDerive, pullVs;  // the gap checks
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
    // many ordinary textures first, then the mip texture asks for a LOW heap slot (< 16384: the material pack word keeps 14 bits); it is sampled below
    std::vector<std::unique_ptr<rhi::Texture>> filler;
    for (int i = 0; i < 300; ++i) filler.push_back(dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1, 1, kSampled, "rhi filler"}));
    rhi::TextureDesc mipDesc{VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {8, 8, 1}, 4, 1, kSampled, "rhi mips"};
    mipDesc.lowHeapSlot = true;
    auto mips = dev.createTexture(mipDesc);
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
    check(mips->heapSampled() != 0 && mips->heapSampled() < 16384, std::format("low heap slot texture: slot {} (< 16384)", mips->heapSampled()));
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

// GPU-driven indexed draws (docs/research/frame-ownership.md 4): a compute pass writes the draw records, the count and an instance-rate stream of uint2
// refs; the vertex shader reads the ref from the stream (firstInstance offsets it) and pulls the positions by address with SV_VertexID. The same
// records are drawn with drawIndexedIndirectCount (count word 6 of 7 records) and drawIndexedIndirect (6 records); both images must equal the CPU's.
void testIndirectPull(rhi::Device& dev, const Shaders& sh) {
    const int before = failures;
    const char* name = "indirect draws: compute records, instance ref stream, vertex pulling";
    if (!sh.indCs.ok || !sh.indVs.ok || !sh.vtxPs.ok) {
        check(false, "indirect pull shaders did not compile");
        report(dev, name, before);
        return;
    }
    if (!dev.caps().drawIndirectFirstInstance || !dev.caps().drawIndirectCount) {
        skip(dev, name, "no drawIndirectFirstInstance / drawIndirectCount");
        return;
    }
    constexpr std::uint32_t kTarget = 64, kRecords = 7, kStride = 20, kArgsOffset = 40, kCountOffset = 16;
    // template quads: v0 (-s,-s) v1 (s,-s) v2 (-s,s) v3 (s,s) for s = 0.9, 0.6, 0.3 (tile-local, 1 = half a tile)
    const float sizes[3] = {0.9f, 0.6f, 0.3f};
    float positions[3][4][2];
    for (int t = 0; t < 3; ++t) {
        const float s = sizes[t];
        const float p[4][2] = {{-s, -s}, {s, -s}, {-s, s}, {s, s}};
        std::memcpy(positions[t], p, sizeof(p));
    }
    // absolute indices (vertexOffset 0 in every record: SV_VertexID of D3D12 ignores the base vertex, Vulkan's VertexIndex includes it): per template t,
    // 9 indices from 9 t: the quad (4 t + 0 1 2, 2 1 3), then its lower-left triangle (4 t + 0 1 2)
    std::uint32_t indices[27];
    for (std::uint32_t t = 0; t < 3; ++t) {
        const std::uint32_t base[9] = {0, 1, 2, 2, 1, 3, 0, 1, 2};
        for (std::uint32_t k = 0; k < 9; ++k) indices[t * 9 + k] = 4 * t + base[k];
    }
    constexpr VkBufferUsageFlags kRw = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto posBuf = dev.createBuffer({sizeof(positions), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi pull positions"});
    auto ib = dev.createBuffer({sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, rhi::Memory::Upload, "rhi pull indices"});
    auto args = dev.createBuffer({kArgsOffset + kRecords * kStride, kRw | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, rhi::Memory::Device, "rhi pull args"});
    auto count = dev.createBuffer({64, kRw | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, rhi::Memory::Device, "rhi pull count"});
    auto refs = dev.createBuffer({10 * 8, kRw | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, rhi::Memory::Device, "rhi pull refs"});
    const std::uint32_t pitch = rhi::bufferRowPitch(VK_FORMAT_R8G8B8A8_UNORM, kTarget);
    std::unique_ptr<rhi::Texture> targets[2];
    std::unique_ptr<rhi::Buffer> readbacks[2];
    for (int k = 0; k < 2; ++k) {
        targets[k] = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {kTarget, kTarget, 1}, 1, 1,
                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "rhi pull target"});
        readbacks[k] = dev.createBuffer({static_cast<std::uint64_t>(pitch) * kTarget, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi pull readback"});
    }
    auto computePipeline = dev.createComputePipeline({sh.indCs.shader(), "rhi indirect records"});
    const VkVertexInputBindingDescription binding{0, 8, VK_VERTEX_INPUT_RATE_INSTANCE};
    const VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32_UINT, 0};
    rhi::GraphicsPipelineDesc gd;
    gd.vs = sh.indVs.shader();
    gd.ps = sh.vtxPs.shader();
    gd.colorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    gd.vertexBindings = {&binding, 1};
    gd.vertexAttributes = {&attribute, 1};
    gd.name = "rhi indirect pull";
    auto pipeline = dev.createGraphicsPipeline(gd);
    if (!posBuf || !ib || !args || !count || !refs || !targets[0] || !targets[1] || !readbacks[0] || !readbacks[1] || !computePipeline || !pipeline) {
        check(false, "indirect pull resource / pipeline creation");
        report(dev, name, before);
        return;
    }
    std::memcpy(posBuf->map(), positions, sizeof(positions));
    std::memcpy(ib->map(), indices, sizeof(indices));
    struct IndPush {
        std::uint32_t args[2], count[2], refs[2], pad[2];
    } cp{};
    const auto a = args->shaderRefRw(kArgsOffset), c = count->shaderRefRw(kCountOffset), r = refs->shaderRefRw(0);
    cp.args[0] = a[0], cp.args[1] = a[1], cp.count[0] = c[0], cp.count[1] = c[1], cp.refs[0] = r[0], cp.refs[1] = r[1];
    struct PullPush {
        std::uint32_t positions[2], pad[2];
    } pp{};
    const auto pr = posBuf->shaderRef(0);
    pp.positions[0] = pr[0], pp.positions[1] = pr[1];
    dev.submitAndWait([&](rhi::CommandList& cl) {
        cl.bufferBarrier(*args, rhi::BufferAccess::ShaderWrite);
        cl.bufferBarrier(*count, rhi::BufferAccess::ShaderWrite);
        cl.bufferBarrier(*refs, rhi::BufferAccess::ShaderWrite);
        cl.bindPipeline(*computePipeline);
        cl.setPushConstants(&cp, sizeof(cp));
        cl.dispatch(1, 1, 1);
        cl.bufferBarrier(*args);  // writes -> indirect arguments / vertex data (DX12: UAV -> shader read, the draw moves them on)
        cl.bufferBarrier(*count);
        cl.bufferBarrier(*refs);
        for (int k = 0; k < 2; ++k) {
            cl.transition(*targets[k], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            rhi::RenderingInfo ri;
            ri.colorCount = 1;
            ri.color[0].texture = targets[k].get();
            ri.color[0].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ri.color[0].clear.float32[3] = 1.0f;
            ri.width = ri.height = kTarget;
            cl.beginRendering(ri);
            cl.bindPipeline(*pipeline);
            cl.setPushConstants(&pp, sizeof(pp));
            rhi::Buffer* buffers[1] = {refs.get()};
            const std::uint64_t offsets[1] = {0};
            const std::uint32_t strides[1] = {8};
            cl.setVertexBuffers(0, buffers, offsets, strides);
            cl.setIndexBuffer(*ib, 0, VK_INDEX_TYPE_UINT32);
            if (k == 0) cl.drawIndexedIndirectCount(*args, kArgsOffset, *count, kCountOffset, kRecords, kStride);
            else cl.drawIndexedIndirect(*args, kArgsOffset, 6, kStride);
            cl.endRendering();
            cl.transition(*targets[k], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            cl.copyTextureToBuffer(*readbacks[k], 0, *targets[k]);
        }
    });
    // expected per tile (the tables of rhi_ind.comp.hlsl): colour id (slot + 1; 0 = nothing drawn), template, half
    struct Tile {
        std::uint32_t id = 0, tpl = 0;
        bool half = false;
    } tiles[16];
    const std::uint32_t instances[6] = {1, 2, 0, 1, 3, 1}, first[6] = {0, 1, 3, 3, 4, 8}, tpl[6] = {0, 1, 2, 1, 0, 2}, tileOf[10] = {0, 1, 2, 3, 4, 5, 6, 3, 7, 8};
    const bool half[6] = {false, false, false, true, true, false};
    for (int rec = 0; rec < 6; ++rec) {
        for (std::uint32_t k = 0; k < instances[rec]; ++k) {
            const std::uint32_t slot = first[rec] + k;
            tiles[tileOf[slot]] = {slot + 1, tpl[rec], half[rec]};
        }
    }
    const auto colour = [](std::uint32_t id) {
        return Rgba{static_cast<std::uint8_t>((id * 53) & 255), static_cast<std::uint8_t>((id * 97 + 50) & 255), static_cast<std::uint8_t>((id * 29 + 120) & 255), 255};
    };
    const char* variant[2] = {"count", "no count"};
    for (int k = 0; k < 2; ++k) {
        const auto* px = static_cast<const std::uint8_t*>(readbacks[k]->map());
        for (int t = 0; t < 16; ++t) {
            // probes in tile-local units (1 = 8 pixels, y up): inside every template and the half triangle / inside the quads but outside the half
            // triangle / inside the largest template only
            const float probes[3][2] = {{-0.12f, -0.12f}, {0.12f, 0.12f}, {0.75f, 0.75f}};
            for (int pi = 0; pi < 3; ++pi) {
                const Tile& tl = tiles[t];
                Rgba want{0, 0, 0, 255};
                if (tl.id != 0) {
                    const bool inside = pi == 0 || (pi == 1 && !tl.half) || (pi == 2 && tl.tpl == 0 && !tl.half);
                    if (inside) want = colour(tl.id);
                }
                const std::uint32_t x = static_cast<std::uint32_t>(static_cast<float>((t & 3) * 16 + 8) + probes[pi][0] * 8.0f);
                const std::uint32_t y = static_cast<std::uint32_t>(static_cast<float>(kTarget - ((t >> 2) * 16 + 8)) - probes[pi][1] * 8.0f);
                const std::uint8_t* p = px + y * pitch + x * 4;
                check(p[0] == want[0] && p[1] == want[1] && p[2] == want[2] && p[3] == want[3],
                      std::format("{}: tile {} probe {} at ({}, {}): ({}, {}, {}, {}) want ({}, {}, {}, {})", variant[k], t, pi, x, y, p[0], p[1], p[2], p[3],
                                  want[0], want[1], want[2], want[3]));
            }
        }
    }
    check(std::memcmp(readbacks[0]->map(), readbacks[1]->map(), static_cast<std::size_t>(pitch) * kTarget) == 0, "count / no-count images differ");
    report(dev, name, before);
}

// S-4 (frame-ownership.md 14.12): the pulled scene vertex shaders' conversions (shaders/include/rvk_pull.hlsli, rvkPullAttr) against the input assembler, BIT FOR BIT,
// for every format the renderer pulls and every code of the 8- and 16-bit formats: 65,536 vertices of 88 bytes (13 attributes, one per format), each read once by the
// pipeline's vertex input (the D3D9 formats as d3d9/convert.cpp maps them, so D3D12 reads UBYTE4 as UNORM like the game) and once through a VertexFetch block of the same
// buffer. The z pre-pass and the colour pass test the depth for equality: a pulled owned z draw and an input-assembler colour draw of the same geometry must get the same
// inputs, not close ones. Half NaNs are counted apart (a NaN payload may be quietened by either path; the engine's data has none).
void testPullFormats(rhi::Device& dev, const Shaders& sh) {
    const int before = failures;
    const char* name = "vertex pulling = the input assembler, bit for bit (S-4)";
    if (!sh.pullVs.ok || !sh.vtxPs.ok) {
        check(false, "pull shaders did not compile");
        report(dev, name, before);
        return;
    }
    constexpr std::uint32_t kVerts = 65536, kStride = 88, kAttrs = 13;
    using PF = shader::abi::PullFormat;
    const bool d3d12 = dev.caps().backend == rhi::Backend::D3D12;
    struct Attr {
        VkFormat format;
        std::uint32_t offset;
        PF pull;
        const char* name;
    };
    const Attr attrs[kAttrs] = {
        {VK_FORMAT_R32G32B32A32_SFLOAT, 0, PF::Float4, "FLOAT4"},
        {VK_FORMAT_B8G8R8A8_UNORM, 16, PF::Bgra8Unorm, "D3DCOLOR"},
        {VK_FORMAT_R8G8B8A8_USCALED, 20, d3d12 ? PF::Rgba8Unorm : PF::Rgba8Uscaled, d3d12 ? "UBYTE4 (D3D12: UNORM)" : "UBYTE4"},
        {VK_FORMAT_R8G8B8A8_UNORM, 24, PF::Rgba8Unorm, "UBYTE4N"},
        {VK_FORMAT_R16G16_SNORM, 28, PF::Rg16Snorm, "SHORT2N"},
        {VK_FORMAT_R16G16B16A16_SNORM, 32, PF::Rgba16Snorm, "SHORT4N"},
        {VK_FORMAT_R16G16_UNORM, 40, PF::Rg16Unorm, "USHORT2N"},
        {VK_FORMAT_R16G16B16A16_UNORM, 44, PF::Rgba16Unorm, "USHORT4N"},
        {VK_FORMAT_R16G16_SFLOAT, 52, PF::Rg16Float, "FLOAT16_2"},
        {VK_FORMAT_R16G16B16A16_SFLOAT, 56, PF::Rgba16Float, "FLOAT16_4"},
        {VK_FORMAT_R32G32B32_SFLOAT, 64, PF::Float3, "FLOAT3"},
        {VK_FORMAT_R32G32_SFLOAT, 76, PF::Float2, "FLOAT2"},
        {VK_FORMAT_R32_SFLOAT, 84, PF::Float1, "FLOAT1"},
    };
    std::vector<std::uint8_t> verts(static_cast<std::size_t>(kVerts + 2) * kStride, 0);
    std::uint32_t seed = 4242;
    const auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return seed;
    };
    for (std::uint32_t v = 0; v < kVerts + 2; ++v) {
        std::uint8_t* p = verts.data() + static_cast<std::size_t>(v) * kStride;
        // floats: random bits (finite, of every exponent); 8-bit channels: (v + 64 c) & 255 = every code in every channel; 16-bit channels: (v + 16384 c) & 0xffff
        for (std::uint32_t o : {0u, 4u, 8u, 12u, 64u, 68u, 72u, 76u, 80u, 84u}) {
            std::uint32_t bits = rnd();
            if (((bits >> 23) & 0xffu) == 0xffu) bits ^= 0x00800000u;  // (no Inf / NaN)
            std::memcpy(p + o, &bits, 4);
        }
        for (std::uint32_t o : {16u, 20u, 24u}) {
            for (std::uint32_t c = 0; c < 4; ++c) p[o + c] = static_cast<std::uint8_t>((v + 64 * c) & 255);
        }
        for (std::uint32_t o : {28u, 40u, 52u}) {
            for (std::uint32_t c = 0; c < 2; ++c) {
                const std::uint16_t w = static_cast<std::uint16_t>((v + 16384 * c) & 0xffff);
                std::memcpy(p + o + 2 * c, &w, 2);
            }
        }
        for (std::uint32_t o : {32u, 44u, 56u}) {
            for (std::uint32_t c = 0; c < 4; ++c) {
                const std::uint16_t w = static_cast<std::uint16_t>((v + 16384 * c) & 0xffff);
                std::memcpy(p + o + 2 * c, &w, 2);
            }
        }
    }
    constexpr std::uint64_t kOutBytes = static_cast<std::uint64_t>(kVerts) * 2 * kAttrs * 16;
    auto vb = dev.createBuffer({verts.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi pull vertices"});
    auto fetchBuf = dev.createBuffer({sizeof(shader::abi::VertexFetch), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi pull fetch block"});
    auto out = dev.createBuffer({kOutBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Device, "rhi pull out"});
    auto readback = dev.createBuffer({kOutBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi pull readback"});
    auto target = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {16, 16, 1}, 1, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "rhi pull target"});
    VkVertexInputAttributeDescription va[kAttrs];
    for (std::uint32_t k = 0; k < kAttrs; ++k) va[k] = {k, 0, attrs[k].format, attrs[k].offset};
    const VkVertexInputBindingDescription binding{0, kStride, VK_VERTEX_INPUT_RATE_VERTEX};
    rhi::GraphicsPipelineDesc gd;
    gd.vs = sh.pullVs.shader();
    gd.ps = sh.vtxPs.shader();
    gd.colorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    gd.vertexBindings = {&binding, 1};
    gd.vertexAttributes = {va, kAttrs};
    gd.name = "rhi pull formats";
    auto pipeline = dev.createGraphicsPipeline(gd);
    if (!vb || !fetchBuf || !out || !readback || !target || !pipeline) {
        check(false, "pull resource / pipeline creation");
        report(dev, name, before);
        return;
    }
    std::memcpy(vb->map(), verts.data(), verts.size());
    shader::abi::VertexFetch f{};
    const auto vr = vb->shaderRef(0);
    f.ref[0][0] = vr[0];
    f.ref[0][1] = vr[1];
    f.stride[0] = kStride;
    f.baseVertex = 0;
    f.valid = shader::abi::kVertexFetchValid;
    for (std::uint32_t k = 0; k < kAttrs; ++k) f.attr[k] = attrs[k].offset | 0u << shader::abi::kPullAttrStreamShift | static_cast<std::uint32_t>(attrs[k].pull) << shader::abi::kPullAttrFormatShift;
    std::memcpy(fetchBuf->map(), &f, sizeof(f));
    struct Push {
        std::uint32_t fetch[2], out[2], count, pad[3];
    } push{};
    const auto fr = fetchBuf->shaderRef(0), orf = out->shaderRefRw(0);
    push.fetch[0] = fr[0], push.fetch[1] = fr[1], push.out[0] = orf[0], push.out[1] = orf[1], push.count = kVerts;
    dev.submitAndWait([&](rhi::CommandList& cl) {
        cl.bufferBarrier(*out, rhi::BufferAccess::ShaderWrite);
        cl.transition(*target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        rhi::RenderingInfo ri;
        ri.colorCount = 1;
        ri.color[0].texture = target.get();
        ri.color[0].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ri.width = ri.height = 16;
        cl.beginRendering(ri);
        cl.bindPipeline(*pipeline);
        cl.setPushConstants(&push, sizeof(push));
        rhi::Buffer* buffers[1] = {vb.get()};
        const std::uint64_t offsets[1] = {0};
        const std::uint32_t strides[1] = {kStride};
        cl.setVertexBuffers(0, buffers, offsets, strides);
        cl.draw(kVerts + 2, 1, 0, 0);  // (a multiple of 3: every vertex is shaded)
        cl.endRendering();
        cl.bufferBarrier(*out, rhi::BufferAccess::IndirectRead);
        cl.copyBuffer(*readback, 0, *out, 0, kOutBytes);
    });
    const auto* o = static_cast<const std::uint32_t*>(readback->map());
    std::uint64_t bad[kAttrs] = {}, nan[kAttrs] = {};
    std::string first[kAttrs];
    for (std::uint32_t v = 0; v < kVerts; ++v) {
        for (std::uint32_t k = 0; k < kAttrs; ++k) {
            const std::uint32_t* ia = o + (static_cast<std::size_t>(v) * 2 * kAttrs + k) * 4;
            const std::uint32_t* pu = o + (static_cast<std::size_t>(v) * 2 * kAttrs + kAttrs + k) * 4;
            if (std::memcmp(ia, pu, 16) == 0) continue;
            bool bothNan = true;
            for (int c = 0; c < 4; ++c) {
                if (ia[c] == pu[c]) continue;
                const bool na = (ia[c] & 0x7f800000u) == 0x7f800000u && (ia[c] & 0x7fffffu) != 0, nb = (pu[c] & 0x7f800000u) == 0x7f800000u && (pu[c] & 0x7fffffu) != 0;
                bothNan = bothNan && na && nb;
            }
            if (bothNan && (attrs[k].pull == PF::Rg16Float || attrs[k].pull == PF::Rgba16Float)) {
                ++nan[k];
                continue;
            }
            if (bad[k]++ == 0) {
                first[k] = std::format("vertex {}: input assembler {:08x} {:08x} {:08x} {:08x}, pulled {:08x} {:08x} {:08x} {:08x}", v, ia[0], ia[1], ia[2], ia[3], pu[0], pu[1], pu[2], pu[3]);
            }
        }
    }
    for (std::uint32_t k = 0; k < kAttrs; ++k) {
        std::printf("%s  %-22s %llu of %u vertices differ%s%s\n", tag(dev).c_str(), attrs[k].name, static_cast<unsigned long long>(bad[k]), kVerts,
                    nan[k] != 0 ? std::format(" ({} NaN codes with another payload, not counted)", nan[k]).c_str() : "", bad[k] != 0 ? (" - first: " + first[k]).c_str() : "");
        check(bad[k] == 0, std::format("{}: {} vertices differ ({})", attrs[k].name, bad[k], first[k]));
    }
    report(dev, name, before);
}

// F2c-4c (frame-ownership.md 7.32): shaders/gpuscene/owned_compact.comp.hlsl, the production compaction pass, on a record table + ref list + jobs the test builds: buckets of
// 150 (several chunks, invalid entries in it: a record without a draw address, a record with indexCount 0, a slot beyond the table), 1, 64 (one full chunk) and 65 entries,
// non-zero count / argument base offsets and argument indices. The counts and the argument records read back must equal the CPU's compaction (order kept, firstInstance =
// the entry's position in the list); the buffer is then moved to the indirect-argument state (bufferBarrier IndirectRead) and copied out (DX12 state tracking).
void testOwnedCompact(rhi::Device& dev, const Shaders& sh) {
    const int before = failures;
    const char* name = "owned draws compaction (F2c-4c)";
    if (!sh.compact.ok) {
        check(false, "compaction shader did not compile");
        report(dev, name, before);
        return;
    }
    constexpr std::uint32_t kSlots = 256, kRecordBytes = 128, kBuckets = 4, kCountsBase = 64, kArgsBase = 256, kFirstArg = 5, kFirstCount = 2;
    const std::uint32_t sizes[kBuckets] = {150, 1, 64, 65};
    std::uint32_t total = 0;
    for (const std::uint32_t n : sizes) total += n;
    constexpr VkBufferUsageFlags kRw = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    const std::uint64_t outBytes = kArgsBase + static_cast<std::uint64_t>(kFirstArg + total) * 20;
    auto records = dev.createBuffer({static_cast<std::uint64_t>(kSlots) * kRecordBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi compact records"});
    auto list = dev.createBuffer({static_cast<std::uint64_t>(total) * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi compact list"});
    auto jobs = dev.createBuffer({kBuckets * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi compact jobs"});
    auto out = dev.createBuffer({outBytes, kRw, rhi::Memory::Device, "rhi compact out"});
    auto fill = dev.createBuffer({outBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Upload, "rhi compact poison"});
    auto readback = dev.createBuffer({outBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi compact readback"});
    auto pipeline = dev.createComputePipeline({sh.compact.shader(), "rhi owned compact"});
    if (!records || !list || !jobs || !out || !fill || !readback || !pipeline) {
        check(false, "compaction resource / pipeline creation");
        report(dev, name, before);
        return;
    }
    // records: slot s draws (1000 + s) indices from 77 s, base vertex -s; slot 7 has no draw address, slot 8 has indexCount 0
    auto* rec = static_cast<std::uint32_t*>(records->map());
    std::memset(rec, 0, static_cast<std::size_t>(kSlots) * kRecordBytes);
    for (std::uint32_t slot = 0; slot < kSlots; ++slot) {
        std::uint32_t* r = rec + static_cast<std::size_t>(slot) * (kRecordBytes / 4);
        if (slot != 7) {
            r[16] = 0x1000u + slot;  // draw address low
            r[17] = 1;               // high
        }
        r[28] = slot == 8 ? 0u : 1000u + slot;
        r[29] = 77u * slot;
        r[30] = static_cast<std::uint32_t>(-static_cast<std::int32_t>(slot));
    }
    struct Entry {
        std::uint32_t slot, view;
    };
    std::vector<Entry> entries;
    std::uint32_t firstEntry[kBuckets];
    std::uint32_t seed = 12345;
    for (std::uint32_t b = 0; b < kBuckets; ++b) {
        firstEntry[b] = static_cast<std::uint32_t>(entries.size());
        for (std::uint32_t k = 0; k < sizes[b]; ++k) {
            seed = seed * 1664525u + 1013904223u;
            std::uint32_t slot = 20 + (seed >> 8) % 200;
            if (b == 0 && (k == 3 || k == 70)) slot = 7;              // no draw address
            else if (b == 0 && (k == 64 || k == 149)) slot = 8;       // indexCount 0
            else if (b == 0 && k == 100) slot = 9999;                 // beyond the table
            else if (b == 2 && k == 63) slot = 7;                     // the last entry of a full chunk
            else if (b == 3 && k == 64) slot = 8;                     // the lone entry of the second chunk
            entries.push_back({slot, b + 1});
        }
    }
    std::memcpy(list->map(), entries.data(), entries.size() * sizeof(Entry));
    auto* jb = static_cast<std::uint32_t*>(jobs->map());
    std::memset(jb, 0, kBuckets * 32);
    for (std::uint32_t b = 0; b < kBuckets; ++b) {
        jb[8 * b + 0] = firstEntry[b];
        jb[8 * b + 1] = sizes[b];
        jb[8 * b + 2] = kFirstArg + firstEntry[b];
        jb[8 * b + 3] = kFirstCount + b;
        jb[8 * b + 4] = sizes[b];  // capacity
    }
    std::memset(fill->map(), 0xAB, outBytes);  // (poison: what the pass leaves alone stays visible)
    struct Push {
        std::uint32_t records[2], list[2], jobs[2], out[2];
        std::uint32_t countsBase, argsBase, slotCount, viewSlots;
        std::uint32_t clusters[2], views[2];
        std::uint32_t clusterRows, pad[3];
    } push{};
    static_assert(sizeof(Push) == 80);
    const auto rr = records->shaderRef(0), lr = list->shaderRef(0), jr = jobs->shaderRef(0), wr = out->shaderRefRw(0);
    push.records[0] = rr[0], push.records[1] = rr[1], push.list[0] = lr[0], push.list[1] = lr[1], push.jobs[0] = jr[0], push.jobs[1] = jr[1], push.out[0] = wr[0], push.out[1] = wr[1];
    push.countsBase = kCountsBase;
    push.argsBase = kArgsBase;
    push.slotCount = kSlots;
    dev.submitAndWait([&](rhi::CommandList& cl) {
        cl.copyBuffer(*out, 0, *fill, 0, outBytes);
        cl.bufferBarrier(*out, rhi::BufferAccess::ShaderWrite);
        cl.bindPipeline(*pipeline);
        cl.setPushConstants(&push, sizeof(push));
        cl.dispatch(kBuckets, 1, 1);
        cl.bufferBarrier(*out, rhi::BufferAccess::IndirectRead);
        cl.copyBuffer(*readback, 0, *out, 0, outBytes);
        cl.bufferBarrier(*out, rhi::BufferAccess::IndirectRead);
    });
    const auto* got = static_cast<const std::uint32_t*>(readback->map());
    std::uint32_t wrongCounts = 0, wrongRecords = 0, comparedRecords = 0;
    for (std::uint32_t b = 0; b < kBuckets; ++b) {
        std::vector<std::uint32_t> want;  // 5 words per valid entry
        for (std::uint32_t k = 0; k < sizes[b]; ++k) {
            const std::uint32_t pos = firstEntry[b] + k, slot = entries[pos].slot;
            if (slot >= kSlots || slot == 7 || slot == 8) continue;
            want.insert(want.end(), {1000u + slot, 1u, 77u * slot, static_cast<std::uint32_t>(-static_cast<std::int32_t>(slot)), pos});
        }
        const std::uint32_t n = static_cast<std::uint32_t>(want.size() / 5);
        const std::uint32_t haveCount = got[kCountsBase / 4 + kFirstCount + b];
        if (haveCount != n) {
            ++wrongCounts;
            std::printf("    bucket %u: count %u (want %u)\n", b, haveCount, n);
            continue;
        }
        for (std::uint32_t k = 0; k < n; ++k) {
            ++comparedRecords;
            const std::uint32_t* a = got + kArgsBase / 4 + static_cast<std::size_t>(kFirstArg + firstEntry[b] + k) * 5;
            if (std::memcmp(a, &want[static_cast<std::size_t>(k) * 5], 20) != 0) {
                if (wrongRecords++ < 4) std::printf("    bucket %u record %u: {%u, %u, %u, %d, %u} want {%u, %u, %u, %d, %u}\n", b, k, a[0], a[1], a[2], static_cast<std::int32_t>(a[3]), a[4], want[k * 5], want[k * 5 + 1], want[k * 5 + 2], static_cast<std::int32_t>(want[k * 5 + 3]), want[k * 5 + 4]);
            }
        }
        // (one past the last record of the bucket is not ours: the next bucket's range starts after the bucket's own entries, so it still holds the poison)
        if (n < sizes[b]) {
            const std::uint32_t* a = got + kArgsBase / 4 + static_cast<std::size_t>(kFirstArg + firstEntry[b] + n) * 5;
            check(a[0] == 0xABABABABu, std::format("bucket {}: the pass wrote beyond its count", b));
        }
    }
    check(wrongCounts == 0, std::format("{} bucket count(s) differ", wrongCounts));
    check(wrongRecords == 0, std::format("{} of {} argument record(s) differ", wrongRecords, comparedRecords));
    check(got[0] == 0xABABABABu && got[kCountsBase / 4 - 1] == 0xABABABABu && got[kCountsBase / 4 + kFirstCount + kBuckets] == 0xABABABABu, "counts written outside the buckets' words");
    report(dev, name, before);
}

// frame-ownership.md 7.47: shaders/debug/picture_check.comp.hlsl, the owned picture check's compute pass, on synthetic scene colour (RGBA16F) + depth (R32F) images: mode 0 stores a
// reference, mode 1 compares another image pair with it. The second pair differs in 5 pixels' colour, 3 pixels' depth (one pixel both), plus two pixels inside the tolerances (no
// difference); the counts, the largest differences and the bounding box read back must be exact; the same pair again must count nothing; a NaN pixel counts as a colour difference.
void testPictureCheck(rhi::Device& dev, const Shaders& sh) {
    const int before = failures;
    const char* name = "owned picture check compare (7.47)";
    if (!sh.picture.ok) {
        check(false, "picture check shader did not compile");
        report(dev, name, before);
        return;
    }
    constexpr std::uint32_t kW = 32, kH = 16;
    constexpr VkImageUsageFlags kSampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto colA = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, {kW, kH, 1}, 1, 1, kSampled, "rhi pc colour A"});
    auto colB = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, {kW, kH, 1}, 1, 1, kSampled, "rhi pc colour B"});
    auto depA = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R32_SFLOAT, {kW, kH, 1}, 1, 1, kSampled, "rhi pc depth A"});
    auto depB = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R32_SFLOAT, {kW, kH, 1}, 1, 1, kSampled, "rhi pc depth B"});
    constexpr VkImageUsageFlags kStore = VK_IMAGE_USAGE_STORAGE_BIT;
    auto refColor = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, {kW, kH, 1}, 1, 1, kStore, "rhi pc ref colour"});
    auto refDepth = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R32_SFLOAT, {kW, kH, 1}, 1, 1, kStore, "rhi pc ref depth"});
    const std::uint32_t pitchC = rhi::bufferRowPitch(VK_FORMAT_R16G16B16A16_SFLOAT, kW), pitchD = rhi::bufferRowPitch(VK_FORMAT_R32_SFLOAT, kW);
    constexpr std::uint64_t kOffA = 0, kOffB = 8192, kOffDA = 16384, kOffDB = 24576;
    auto upload = dev.createBuffer({32768, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Upload, "rhi pc upload"});
    auto stats = dev.createBuffer({64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Device, "rhi pc stats"});
    auto readback = dev.createBuffer({64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi pc readback"});
    auto pipeline = dev.createComputePipeline({sh.picture.shader(), "rhi picture check"});
    if (!colA || !colB || !depA || !depB || !refColor || !refDepth || !upload || !stats || !readback || !pipeline) {
        check(false, "picture check resource / pipeline creation");
        report(dev, name, before);
        return;
    }
    // contents: colour (0.5, 0.5, 0.5) / depth 0.5 everywhere in A; B = A with the differences below
    constexpr std::uint16_t kHalf05 = 0x3800, kHalf20 = 0x4000, kHalf1 = 0x3C00, kHalfNaN = 0x7E00;
    constexpr float kDepth05 = 0.5f, kDepth07 = 0.7f;
    auto* up = static_cast<std::uint8_t*>(upload->map());
    std::memset(up, 0, 32768);
    const auto putColour = [&](std::uint64_t base, std::uint32_t x, std::uint32_t y, std::uint16_t v) {
        std::uint16_t px[4] = {v, v, v, kHalf1};
        std::memcpy(up + base + static_cast<std::size_t>(y) * pitchC + x * 8, px, 8);
    };
    const auto putDepth = [&](std::uint64_t base, std::uint32_t x, std::uint32_t y, float v) { std::memcpy(up + base + static_cast<std::size_t>(y) * pitchD + x * 4, &v, 4); };
    for (std::uint32_t y = 0; y < kH; ++y) {
        for (std::uint32_t x = 0; x < kW; ++x) {
            putColour(kOffA, x, y, kHalf05);
            putColour(kOffB, x, y, kHalf05);
            putDepth(kOffDA, x, y, kDepth05);
            putDepth(kOffDB, x, y, kDepth05);
        }
    }
    const std::array<std::array<std::uint32_t, 2>, 5> colourPx = {{{3, 2}, {10, 5}, {20, 7}, {31, 15}, {0, 0}}};
    const std::array<std::array<std::uint32_t, 2>, 3> depthPx = {{{10, 5}, {25, 9}, {6, 12}}};
    for (const auto& p : colourPx) putColour(kOffB, p[0], p[1], kHalf20);
    for (const auto& p : depthPx) putDepth(kOffDB, p[0], p[1], kDepth07);
    // inside the tolerances: colour 0.5 -> 0.5107 (half), depth 0.5 -> 0.5008
    putColour(kOffB, 12, 12, 0x3818);
    putDepth(kOffDB, 13, 13, 0.5008f);
    struct Push {
        std::uint32_t frame[2], passId, passFlags;
        std::uint32_t colorTex, depthTex, width, height, mode;
        float colorTol, depthTol;
        std::uint32_t pad;
        std::uint32_t stats[2], pad2[2];
    };
    static_assert(sizeof(Push) == 64);
    const auto run = [&](rhi::Texture& color, rhi::Texture& depth, std::uint32_t mode, std::uint32_t(&out)[16]) {
        Push push{};
        push.colorTex = color.heapSampled();
        push.depthTex = depth.heapSampled();
        push.width = kW;
        push.height = kH;
        push.mode = mode;
        push.colorTol = 0.04f;
        push.depthTol = 0.004f;
        const auto sr = stats->shaderRefRw(0);
        push.stats[0] = sr[0];
        push.stats[1] = sr[1];
        dev.submitAndWait([&](rhi::CommandList& cl) {
            cl.fillBuffer(*stats, 0, 64, 0);
            cl.bufferBarrier(*stats, rhi::BufferAccess::ShaderWrite);
            cl.transition(*refColor, VK_IMAGE_LAYOUT_GENERAL);
            cl.transition(*refDepth, VK_IMAGE_LAYOUT_GENERAL);
            cl.bindPipeline(*pipeline);
            rhi::Texture* storage[] = {refColor.get(), refDepth.get()};
            cl.setStorageTextures(storage);
            cl.setPushConstants(&push, sizeof(push));
            cl.dispatch((kW + 7) / 8, (kH + 7) / 8, 1);
            cl.bufferBarrier(*stats, rhi::BufferAccess::ShaderRead);
            cl.copyBuffer(*readback, 0, *stats, 0, 64);
        });
        readback->invalidate();
        std::memcpy(out, readback->map(), 64);
    };
    dev.submitAndWait([&](rhi::CommandList& cl) {
        for (rhi::Texture* t : {colA.get(), colB.get(), depA.get(), depB.get()}) cl.transition(*t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        cl.copyBufferToTexture(*colA, *upload, kOffA);
        cl.copyBufferToTexture(*colB, *upload, kOffB);
        cl.copyBufferToTexture(*depA, *upload, kOffDA);
        cl.copyBufferToTexture(*depB, *upload, kOffDB);
        for (rhi::Texture* t : {colA.get(), colB.get(), depA.get(), depB.get()}) cl.transition(*t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    std::uint32_t s[16] = {};
    run(*colA, *depA, 0, s);   // store the reference
    run(*colA, *depA, 1, s);   // the same pair: nothing differs
    check(s[0] == 0 && s[1] == 0 && s[2] == 0, std::format("identical pair: {} / {} / {} differing (want 0)", s[0], s[1], s[2]));
    run(*colB, *depB, 1, s);
    check(s[0] == 5, std::format("colour differing {} (want 5)", s[0]));
    check(s[1] == 3, std::format("depth differing {} (want 3)", s[1]));
    check(s[2] == 7, std::format("pixels differing {} (want 7)", s[2]));
    float maxC, maxD;
    std::memcpy(&maxC, &s[3], 4);
    std::memcpy(&maxD, &s[4], 4);
    check(std::fabs(maxC - 1.5f / 2.05f) < 1e-3f, std::format("max colour {} (want {})", maxC, 1.5f / 2.05f));
    check(std::fabs(maxD - 0.4f) < 2e-3f, std::format("max depth {} (want 0.4)", maxD));
    check(s[5] == 31 && s[6] == 15 && 0xFFFFu - s[7] == 0 && 0xFFFFu - s[8] == 0, std::format("bounding box [{},{}..{},{}] (want [0,0..31,15])", 0xFFFFu - s[7], 0xFFFFu - s[8], s[5], s[6]));
    // a NaN pixel (colour) counts as a colour difference with a huge value
    {
        auto* up2 = static_cast<std::uint8_t*>(upload->map());
        std::uint16_t px[4] = {kHalfNaN, kHalfNaN, kHalfNaN, kHalf1};
        std::memcpy(up2 + kOffB + static_cast<std::size_t>(9) * pitchC + 17 * 8, px, 8);
        dev.submitAndWait([&](rhi::CommandList& cl) {
            cl.transition(*colB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            cl.copyBufferToTexture(*colB, *upload, kOffB);
            cl.transition(*colB, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        run(*colB, *depB, 1, s);
        check(s[0] == 6 && s[2] == 8, std::format("with a NaN pixel: colour {} (want 6), any {} (want 8)", s[0], s[2]));
    }
    report(dev, name, before);
}

// S-6 (frame-ownership.md 14.10): shaders/depth/depth_z.frag.hlsl derives the engine's linear depth map from the hardware depth. Two triangles at known view depths are drawn the way the
// main view's z pre-pass is under reverse-z (test/depth_z_scene.*): once the old way (R32F colour target = view z / far, cleared 1, + depth) and once colour-less (depth only, a pixel
// shader without outputs); the hardware depth of each is turned into a map by the derive pass and must equal the old pass's map (relative error, sky = 1 on both). With both backends
// in one process, the second backend's map must agree with the first's.
std::vector<float> g_depthZFirst;  // the derived map of the first backend that ran (the cross-backend check)

void testDepthZ(rhi::Device& dev, const Shaders& sh) {
    const int before = failures;
    const char* name = "depth map derived from the hardware depth (S-6)";
    if (!sh.dzVs.ok || !sh.dzPsOld.ok || !sh.dzPsNone.ok || !sh.dzFs.ok || !sh.dzDerive.ok) {
        check(false, "depth_z shaders did not compile");
        report(dev, name, before);
        return;
    }
    constexpr std::uint32_t kW = 96, kH = 64;
    constexpr float kFarPlane = 1000.0f, kNearPlane = 10.0f;
    const auto make = [&](VkFormat f, VkImageUsageFlags u, const char* n) { return dev.createTexture({VK_IMAGE_TYPE_2D, f, {kW, kH, 1}, 1, 1, u, n}); };
    auto depthA = make(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "rhi dz depth A");
    auto depthB = make(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "rhi dz depth B");
    constexpr VkImageUsageFlags kMapUse = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    auto mapOld = make(VK_FORMAT_R32_SFLOAT, kMapUse, "rhi dz map old");
    auto mapA = make(VK_FORMAT_R32_SFLOAT, kMapUse, "rhi dz map A");
    auto mapB = make(VK_FORMAT_R32_SFLOAT, kMapUse, "rhi dz map B");
    const std::uint32_t pitch = rhi::bufferRowPitch(VK_FORMAT_R32_SFLOAT, kW);
    const std::uint64_t bytes = static_cast<std::uint64_t>(pitch) * kH;
    auto readOld = dev.createBuffer({bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi dz read old"});
    auto readA = dev.createBuffer({bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi dz read A"});
    auto readB = dev.createBuffer({bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi dz read B"});
    auto frameBlock = dev.createBuffer({sizeof(shader::abi::FrameBlock), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi dz frame block"});
    if (!depthA || !depthB || !mapOld || !mapA || !mapB || !readOld || !readA || !readB || !frameBlock) {
        check(false, "depth_z resource creation");
        report(dev, name, before);
        return;
    }
    auto* fb = static_cast<shader::abi::FrameBlock*>(frameBlock->map());
    std::memset(fb, 0, sizeof(*fb));
    fb->frameInfo[1] = shader::abi::kFrameReverseZ;
    fb->depthParams[0] = kNearPlane;
    fb->depthParams[1] = 5000.0f;
    fb->depthParams[2] = kFarPlane;
    fb->depthParams[3] = 1.0f / kFarPlane;
    fb->cameraPos[3] = kFarPlane;

    rhi::GraphicsPipelineDesc g;
    g.vs = sh.dzVs.shader();
    g.depthFormat = VK_FORMAT_D32_SFLOAT;
    g.depthTest = true;
    g.depthWrite = true;
    g.depthOp = VK_COMPARE_OP_GREATER_OR_EQUAL;  // reversed
    g.ps = sh.dzPsOld.shader();
    g.colorFormats[0] = VK_FORMAT_R32_SFLOAT;
    g.name = "rhi dz old z pass";
    auto oldPass = dev.createGraphicsPipeline(g);
    g.ps = sh.dzPsNone.shader();
    g.colorFormats[0] = VK_FORMAT_UNDEFINED;
    g.name = "rhi dz depth only";
    auto depthOnly = dev.createGraphicsPipeline(g);
    rhi::GraphicsPipelineDesc d;
    d.vs = sh.dzFs.shader();
    d.ps = sh.dzDerive.shader();
    d.colorFormats[0] = VK_FORMAT_R32_SFLOAT;
    d.name = "rhi dz derive";
    auto derive = dev.createGraphicsPipeline(d);
    if (!oldPass || !depthOnly || !derive) {
        check(false, "depth_z pipeline creation");
        report(dev, name, before);
        return;
    }
    struct Push {
        std::uint32_t frame[2], passId, passFlags;
        std::uint32_t depthTex, mapTex, width, height;
        std::uint32_t stats[2], pad[2];
    };
    static_assert(sizeof(Push) == 48);
    dev.submitAndWait([&](rhi::CommandList& cl) {
        const auto zPass = [&](rhi::Texture& depth, rhi::Texture* map, rhi::Pipeline& pipe) {
            cl.transition(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            rhi::RenderingInfo ri;
            ri.depth.texture = &depth;
            ri.depth.load = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ri.depth.store = VK_ATTACHMENT_STORE_OP_STORE;
            ri.depthClear = {0.0f, 0};  // reversed: infinity
            ri.stencil = false;
            if (map != nullptr) {
                cl.transition(*map, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                ri.color[0].texture = map;
                ri.color[0].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
                ri.color[0].store = VK_ATTACHMENT_STORE_OP_STORE;
                ri.color[0].clear.float32[0] = 1.0f;  // the engine clears the depth map white
                ri.colorCount = 1;
            }
            ri.width = kW;
            ri.height = kH;
            cl.beginRendering(ri);
            cl.bindPipeline(pipe);
            cl.draw(6, 1, 0, 0);
            cl.endRendering();
        };
        const auto deriveInto = [&](rhi::Texture& depth, rhi::Texture& map) {
            cl.transition(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
            cl.transition(map, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            rhi::RenderingInfo ri;
            ri.color[0].texture = &map;
            ri.color[0].load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            ri.color[0].store = VK_ATTACHMENT_STORE_OP_STORE;
            ri.colorCount = 1;
            ri.width = kW;
            ri.height = kH;
            Push push{};
            const auto ref = frameBlock->shaderRef(0);
            push.frame[0] = ref[0];
            push.frame[1] = ref[1];
            push.passId = 1;
            push.depthTex = depth.heapSampled();
            push.width = kW;
            push.height = kH;
            cl.beginRendering(ri);
            cl.bindPipeline(*derive);
            cl.setPushConstants(&push, sizeof(push));
            cl.draw(3, 1, 0, 0);
            cl.endRendering();
        };
        zPass(*depthA, mapOld.get(), *oldPass);  // the engine's z pass: colour + depth
        zPass(*depthB, nullptr, *depthOnly);     // the colour-less pass: depth only
        deriveInto(*depthA, *mapA);
        deriveInto(*depthB, *mapB);
        for (auto [t, b] : {std::pair<rhi::Texture*, rhi::Buffer*>{mapOld.get(), readOld.get()}, {mapA.get(), readA.get()}, {mapB.get(), readB.get()}}) {
            cl.transition(*t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            cl.copyTextureToBuffer(*b, 0, *t);
        }
    });
    readOld->invalidate();
    readA->invalidate();
    readB->invalidate();
    const auto* po = static_cast<const std::uint8_t*>(readOld->map());
    const auto* pa = static_cast<const std::uint8_t*>(readA->map());
    const auto* pb = static_cast<const std::uint8_t*>(readB->map());
    std::uint32_t geometry = 0, sky = 0, skyBad = 0, bad = 0, ab = 0;
    double worst = 0.0;
    std::vector<float> derived(static_cast<std::size_t>(kW) * kH);
    for (std::uint32_t y = 0; y < kH; ++y) {
        for (std::uint32_t x = 0; x < kW; ++x) {
            float o, a, b;
            std::memcpy(&o, po + static_cast<std::size_t>(y) * pitch + x * 4, 4);
            std::memcpy(&a, pa + static_cast<std::size_t>(y) * pitch + x * 4, 4);
            std::memcpy(&b, pb + static_cast<std::size_t>(y) * pitch + x * 4, 4);
            derived[static_cast<std::size_t>(y) * kW + x] = a;
            if (std::memcmp(&a, &b, 4) != 0) ++ab;
            if (o >= 0.9999f) {
                ++sky;
                if (a != 1.0f) ++skyBad;  // the clear value: exactly 1
            } else {
                ++geometry;
                const double rel = std::fabs(static_cast<double>(a) - o) / std::max(static_cast<double>(std::min(a, o)), 1e-6);
                worst = std::max(worst, rel);
                if (rel > 2e-5) ++bad;
            }
        }
    }
    std::printf("  depth_z: %u geometry pixels, %u sky pixels; derived vs the z pass: worst relative error %.3e, %u over 2e-5; sky not 1: %u; depth-only vs colour pass: %u pixels differ\n",
                geometry, sky, worst, bad, skyBad, ab);
    check(geometry > 800 && sky > 800, std::format("the scene covers {} geometry / {} sky pixels (want > 800 of each)", geometry, sky));
    check(bad == 0, std::format("{} pixel(s) of the derived map differ from the z pass's by more than 2e-5 (worst {:.3e})", bad, worst));
    check(skyBad == 0, std::format("{} sky pixel(s) are not exactly 1", skyBad));
    check(ab == 0, std::format("{} pixel(s) differ between the depth of the colour pass and of the depth-only pass", ab));
    if (g_depthZFirst.empty()) {
        g_depthZFirst = derived;
    } else {
        std::uint32_t offBy = 0;
        for (std::size_t i = 0; i < derived.size(); ++i) {
            if (std::fabs(derived[i] - g_depthZFirst[i]) > 1e-4f * std::max(std::min(derived[i], g_depthZFirst[i]), 1e-6f)) ++offBy;
        }
        std::printf("  depth_z: %u of %zu pixels differ by more than 1e-4 between the two backends\n", offBy, derived.size());
        check(offBy * 200 < derived.size(), std::format("{} pixels differ between the backends (want < 0.5 %)", offBy));
    }
    report(dev, name, before);
}

// F5-2 (frame-ownership.md 7.34): the cull half of the same shader on a synthetic cluster table, view table and object table. Entries of 1 / 5 / 40 / 200 / 4096 clusters (a bucket mixes
// them with plain records), one with 4097 clusters (too many: drawn whole), one whose rows belong to another entry (drawn whole), one whose slot has no object window (dropped as
// invalid), views with a perspective camera, objects scaled + moved, a job without the cull flag. The runs read back must equal the CPU reference (core/cluster_cull) bit for
// bit, the buckets' counts too, and the pass must not write beyond a bucket's capacity (a cap smaller than the runs: the excess is dropped, the neighbour bucket untouched).
void testOwnedCull(rhi::Device& dev, const Shaders& sh) {
    const int before = failures;
    const char* name = "owned draws cluster cull (F5-2)";
    if (!sh.compact.ok) {
        check(false, "compaction shader did not compile");
        report(dev, name, before);
        return;
    }
    constexpr std::uint32_t kRecordBytes = 128, kCountsBase = 64, kArgsBase = 256, kRunCap = 8;
    struct Ent {
        std::uint32_t clusters;  // rows
        std::uint32_t kind;      // 0 normal, 1 rows of another entry, 2 no object window
    };
    const Ent ents[] = {{1, 0}, {5, 0}, {40, 0}, {200, 0}, {4096, 0}, {4097, 0}, {40, 1}, {40, 2}, {300, 0}, {17, 0}, {64, 0}, {63, 0}, {65, 0}};
    constexpr std::uint32_t kEnts = sizeof(ents) / sizeof(ents[0]);
    constexpr std::uint32_t kPlainSlots = 4;
    constexpr std::uint32_t kSlots = kEnts + kPlainSlots + 2;
    std::uint32_t seed = 987654321u;
    const auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    // the cluster table: rows of every entry in order, each cluster 3 triangles (9 indices), first index base + 9 c
    struct Row {
        float c[3], r;
        std::uint32_t first, count, range, pad;
    };
    std::vector<Row> rows;
    std::uint32_t rowFirst[kEnts], indexBase[kEnts];
    for (std::uint32_t e = 0; e < kEnts; ++e) {
        rowFirst[e] = static_cast<std::uint32_t>(rows.size());
        indexBase[e] = 100000u * (e + 1);
        for (std::uint32_t c = 0; c < ents[e].clusters; ++c) {
            Row r{};
            r.c[0] = (rnd() - 0.5f) * 800.0f;
            r.c[1] = (rnd() - 0.5f) * 400.0f;
            r.c[2] = (rnd() - 0.2f) * 1200.0f;
            r.r = 2.0f + rnd() * 25.0f;
            r.first = indexBase[e] + 9 * c;
            r.count = 9;
            r.range = ents[e].kind == 1 ? 7777u : 500u + e;  // (kind 1: another entry's id)
            rows.push_back(r);
        }
    }
    // the view table: slot 0 = perspective, slot 1 = the same camera moved (so the second view culls differently)
    float views[2][160] = {};
    for (int v = 0; v < 2; ++v) {
        float* f = views[v];
        const float fy = 1.0f / std::tan(0.5f), aspect = 16.0f / 9.0f, zn = 10.0f, zf = 5000.0f;
        f[0] = fy / aspect;
        f[4 + 1] = fy;
        f[8 + 2] = zf / (zf - zn);
        f[8 + 3] = -zn * zf / (zf - zn) + (v == 1 ? 40.0f : 0.0f);
        f[12 + 2] = 1.0f;
        f[3] = 0.25f;  // an x shear: the second view looks elsewhere
    }
    // the object table: record o at 512 + o * 96 (c31..c35 + info); the world rows of object o: scale s, translation t
    const std::uint32_t kObjPad = 512;
    std::vector<float> objects(kObjPad / 4 + kSlots * 24, 0.0f);
    float world[kSlots][12];
    for (std::uint32_t o = 0; o < kSlots; ++o) {
        const float sc = 0.5f + rnd() * 2.0f;
        std::memset(world[o], 0, sizeof(world[o]));
        world[o][0] = world[o][5] = world[o][10] = sc;
        world[o][3] = (rnd() - 0.5f) * 200.0f;
        world[o][7] = (rnd() - 0.5f) * 100.0f;
        world[o][11] = rnd() * 200.0f;
        std::memcpy(&objects[kObjPad / 4 + o * 24 + 8], world[o], sizeof(world[o]));
    }
    // records
    std::vector<std::uint32_t> rec(static_cast<std::size_t>(kSlots) * kRecordBytes / 4, 0u);
    auto objBuf = dev.createBuffer({objects.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi cull objects"});
    auto clusterBuf = dev.createBuffer({rows.size() * sizeof(Row), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi cull clusters"});
    auto viewBuf = dev.createBuffer({sizeof(views), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi cull views"});
    if (!objBuf || !clusterBuf || !viewBuf) {
        check(false, "cull resource creation");
        report(dev, name, before);
        return;
    }
    std::memcpy(objBuf->map(), objects.data(), objects.size() * 4);
    std::memcpy(clusterBuf->map(), rows.data(), rows.size() * sizeof(Row));
    // (the view blocks are 640 bytes apart: c0..c3 at the block's start)
    std::vector<float> viewBytes(2 * 160, 0.0f);
    for (int v = 0; v < 2; ++v) std::memcpy(&viewBytes[v * 160], views[v], 16 * sizeof(float));
    std::memcpy(viewBuf->map(), viewBytes.data(), viewBytes.size() * 4);
    for (std::uint32_t slot = 0; slot < kSlots; ++slot) {
        std::uint32_t* r = rec.data() + static_cast<std::size_t>(slot) * (kRecordBytes / 4);
        const std::uint32_t e = slot < kEnts ? slot : 0xffffffffu;
        const auto win = objBuf->shaderRef(kObjPad + slot * 96 - 31 * 16);
        if (!(e < kEnts && ents[e].kind == 2)) {
            r[4] = win[0];  // vsWin[2] low / high (record offset 16)
            r[5] = win[1];
        }
        r[16] = 0x1000u + slot;
        r[17] = 1;
        if (e < kEnts) {
            r[27] = 500u + e;  // reserved0 = the entry id
            r[28] = ents[e].clusters * 9u;
            r[29] = indexBase[e];
            r[30] = static_cast<std::uint32_t>(-static_cast<std::int32_t>(slot));
            r[31] = ents[e].clusters <= 65535 ? (rowFirst[e] | ents[e].clusters << 16) : 0u;
        } else {
            r[28] = 600u + slot;  // a plain record
            r[29] = 77u * slot;
            r[30] = 5u;
        }
    }
    auto records = dev.createBuffer({rec.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi cull records"});
    std::memcpy(records->map(), rec.data(), rec.size() * 4);
    // the buckets: A = all culled entries interleaved with plain ones on view 0; B = some on view 1; C = tight capacity; D = the same as A without the cull flag
    struct Item {
        std::uint32_t slot, view;
    };
    std::vector<Item> items;
    struct Bucket {
        std::uint32_t first, count, cap, flags;
    };
    std::vector<Bucket> buckets;
    const auto add = [&](std::uint32_t view, std::uint32_t flags, bool tight, const std::vector<std::uint32_t>& slots) {
        Bucket b{static_cast<std::uint32_t>(items.size()), static_cast<std::uint32_t>(slots.size()), 0, flags};
        for (const std::uint32_t s : slots) {
            items.push_back({s, view});
            b.cap += (flags & 1u) != 0 && s < kEnts ? kRunCap : 1u;
        }
        if (tight) b.cap = std::max(1u, b.cap / 3);
        buckets.push_back(b);
    };
    std::vector<std::uint32_t> all;
    for (std::uint32_t s = 0; s < kEnts + kPlainSlots; ++s) all.push_back(s);
    add(0, 1, false, all);
    std::vector<std::uint32_t> some = {3, 20 % kSlots, 2, 14, 1, 4, 8, 3};
    add(1, 1, false, some);
    add(0, 1, true, {2, 3, 8, 14, 15, 3, 2});   // capacity a third of the worst case: the excess is dropped, nothing leaks into bucket D
    add(0, 0, false, all);                      // no cull flag: whole ranges
    add(5000, 1, false, {2, 3});                // a view slot beyond the table: not culled
    std::uint32_t capTotal = 0;
    std::vector<std::uint32_t> capFirst;
    for (const Bucket& b : buckets) {
        capFirst.push_back(capTotal);
        capTotal += b.cap;
    }
    constexpr std::uint32_t kFirstArg = 3, kFirstCount = 1, kGuard = 4;
    const std::uint64_t outBytes = kArgsBase + static_cast<std::uint64_t>(kFirstArg + capTotal + kGuard) * 20;
    auto list = dev.createBuffer({items.size() * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi cull list"});
    auto jobs = dev.createBuffer({buckets.size() * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi cull jobs"});
    constexpr VkBufferUsageFlags kRw = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    auto out = dev.createBuffer({outBytes, kRw, rhi::Memory::Device, "rhi cull out"});
    auto fill = dev.createBuffer({outBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Upload, "rhi cull poison"});
    auto readback = dev.createBuffer({outBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi cull readback"});
    auto pipeline = dev.createComputePipeline({sh.compact.shader(), "rhi owned cull"});
    if (!list || !jobs || !out || !fill || !readback || !pipeline) {
        check(false, "cull resource / pipeline creation");
        report(dev, name, before);
        return;
    }
    std::memcpy(list->map(), items.data(), items.size() * sizeof(Item));
    auto* jb = static_cast<std::uint32_t*>(jobs->map());
    std::memset(jb, 0, buckets.size() * 32);
    for (std::size_t b = 0; b < buckets.size(); ++b) {
        jb[8 * b + 0] = buckets[b].first;
        jb[8 * b + 1] = buckets[b].count;
        jb[8 * b + 2] = kFirstArg + capFirst[b];
        jb[8 * b + 3] = kFirstCount + static_cast<std::uint32_t>(b);
        jb[8 * b + 4] = buckets[b].cap;
        jb[8 * b + 5] = buckets[b].flags;
    }
    std::memset(fill->map(), 0xAB, outBytes);
    struct Push {
        std::uint32_t records[2], list[2], jobs[2], out[2];
        std::uint32_t countsBase, argsBase, slotCount, viewSlots;
        std::uint32_t clusters[2], views[2];
        std::uint32_t clusterRows, pad[3];
    } push{};
    const auto rr = records->shaderRef(0), lr = list->shaderRef(0), jr = jobs->shaderRef(0), wr = out->shaderRefRw(0), cr = clusterBuf->shaderRef(0), vr = viewBuf->shaderRef(0);
    push.records[0] = rr[0], push.records[1] = rr[1], push.list[0] = lr[0], push.list[1] = lr[1], push.jobs[0] = jr[0], push.jobs[1] = jr[1], push.out[0] = wr[0], push.out[1] = wr[1];
    push.clusters[0] = cr[0], push.clusters[1] = cr[1], push.views[0] = vr[0], push.views[1] = vr[1];
    push.countsBase = kCountsBase;
    push.argsBase = kArgsBase;
    push.slotCount = kSlots;
    push.viewSlots = 2;
    push.clusterRows = static_cast<std::uint32_t>(rows.size());
    dev.submitAndWait([&](rhi::CommandList& cl) {
        cl.copyBuffer(*out, 0, *fill, 0, outBytes);
        cl.bufferBarrier(*out, rhi::BufferAccess::ShaderWrite);
        cl.bindPipeline(*pipeline);
        cl.setPushConstants(&push, sizeof(push));
        cl.dispatch(static_cast<std::uint32_t>(buckets.size()), 1, 1);
        cl.bufferBarrier(*out, rhi::BufferAccess::IndirectRead);
        cl.copyBuffer(*readback, 0, *out, 0, outBytes);
        cl.bufferBarrier(*out, rhi::BufferAccess::IndirectRead);
    });
    const auto* got = static_cast<const std::uint32_t*>(readback->map());
    std::uint32_t wrongCounts = 0, wrongRecords = 0, compared = 0, culledEntries = 0, wholeEntries = 0, emptyEntries = 0, clamped = 0;
    for (std::size_t b = 0; b < buckets.size(); ++b) {
        std::vector<std::uint32_t> want;  // 5 words per record
        for (std::uint32_t k = 0; k < buckets[b].count; ++k) {
            const std::uint32_t pos = buckets[b].first + k, slot = items[pos].slot;
            const std::uint32_t e = slot < kEnts ? slot : 0xffffffffu;
            if (e < kEnts && ents[e].kind == 2) {
                // (no object window: the shader reads address 0 + 528; skipped here, the test never lists such a slot in a culled bucket)
            }
            const std::int32_t vtx = e < kEnts ? -static_cast<std::int32_t>(slot) : 5;
            if (e >= kEnts) {
                want.insert(want.end(), {600u + slot, 1u, 77u * slot, static_cast<std::uint32_t>(vtx), pos});
                continue;
            }
            const std::uint32_t total = ents[e].clusters * 9u;
            const bool cullable = (buckets[b].flags & 1u) != 0 && items[pos].view < 2 && ents[e].clusters <= 4096 && ents[e].kind == 0;
            if (!cullable) {
                want.insert(want.end(), {total, 1u, indexBase[e], static_cast<std::uint32_t>(vtx), pos});
                ++wholeEntries;
                continue;
            }
            clustercull::View v;
            float regs[4][4];
            std::memcpy(regs, views[items[pos].view], sizeof(regs));
            clustercull::makeView(regs, world[slot], clustercull::kMargin, true, v);
            std::vector<std::uint32_t> bits((ents[e].clusters + 31) / 32, 0u);
            for (std::uint32_t c = 0; c < ents[e].clusters; ++c) {
                if (!clustercull::sphereOutside(v, rows[rowFirst[e] + c].c, rows[rowFirst[e] + c].r)) bits[c >> 5] |= 1u << (c & 31u);
            }
            clustercull::Run runs[clustercull::kRunCap];
            const std::uint32_t n = clustercull::extractRuns(bits.data(), ents[e].clusters, clustercull::kRunCap, runs);
            ++culledEntries;
            if (n == 0) ++emptyEntries;
            for (std::uint32_t q = 0; q < n; ++q) {
                const std::uint32_t first = indexBase[e] + 9 * runs[q].first;
                want.insert(want.end(), {9u * (runs[q].end - runs[q].first), 1u, first, static_cast<std::uint32_t>(vtx), pos});
            }
        }
        std::uint32_t haveCount = got[kCountsBase / 4 + kFirstCount + b];
        std::uint32_t n = static_cast<std::uint32_t>(want.size() / 5);
        if (n > buckets[b].cap) {  // (the excess is dropped: only the first `cap` records exist)
            n = buckets[b].cap;
            ++clamped;
        }
        if (haveCount != n) {
            ++wrongCounts;
            std::printf("    bucket %zu: count %u (want %u, cap %u)\n", b, haveCount, n, buckets[b].cap);
            for (std::uint32_t k = 0; k < std::min(haveCount, 8u); ++k) {
                const std::uint32_t* a = got + kArgsBase / 4 + static_cast<std::size_t>(kFirstArg + capFirst[b] + k) * 5;
                std::printf("      gpu {%u, %u, %u, %d, %u}\n", a[0], a[1], a[2], static_cast<std::int32_t>(a[3]), a[4]);
            }
            for (std::uint32_t k = 0; k < std::min(n, 8u); ++k) std::printf("      cpu {%u, %u, %u, %d, %u}\n", want[k * 5], want[k * 5 + 1], want[k * 5 + 2], static_cast<std::int32_t>(want[k * 5 + 3]), want[k * 5 + 4]);
            continue;
        }
        for (std::uint32_t k = 0; k < n; ++k) {
            ++compared;
            const std::uint32_t* a = got + kArgsBase / 4 + static_cast<std::size_t>(kFirstArg + capFirst[b] + k) * 5;
            if (std::memcmp(a, &want[static_cast<std::size_t>(k) * 5], 20) != 0) {
                if (wrongRecords++ < 6) std::printf("    bucket %zu record %u: {%u, %u, %u, %d, %u} want {%u, %u, %u, %d, %u}\n", b, k, a[0], a[1], a[2], static_cast<std::int32_t>(a[3]), a[4], want[k * 5], want[k * 5 + 1], want[k * 5 + 2], static_cast<std::int32_t>(want[k * 5 + 3]), want[k * 5 + 4]);
            }
        }
        // the poison after the bucket's capacity stays (the next bucket's first record is written only if it has one: its range starts at capFirst[b + 1])
        if (n < buckets[b].cap) {
            const std::uint32_t* a = got + kArgsBase / 4 + static_cast<std::size_t>(kFirstArg + capFirst[b] + n) * 5;
            check(a[0] == 0xABABABABu, std::format("bucket {}: the pass wrote beyond its count", b));
        }
    }
    check(wrongCounts == 0, std::format("{} bucket count(s) differ", wrongCounts));
    check(wrongRecords == 0, std::format("{} of {} cull record(s) differ", wrongRecords, compared));
    check(culledEntries > 10 && emptyEntries < culledEntries && clamped >= 1, std::format("the test did not exercise the cull: {} culled entries ({} empty, {} whole), {} clamped", culledEntries, emptyEntries, wholeEntries, clamped));
    check(got[kCountsBase / 4 + kFirstCount + buckets.size()] == 0xABABABABu, "a count written outside the buckets' words");
    {
        const std::uint32_t* g = got + kArgsBase / 4 + static_cast<std::size_t>(kFirstArg + capTotal) * 5;
        bool poison = true;
        for (std::uint32_t k = 0; k < kGuard * 5; ++k) poison = poison && g[k] == 0xABABABABu;
        check(poison, "arguments written past the last bucket's capacity");
    }
    report(dev, name, before);
}

// shaders/rt/rt_skin.comp.hlsl (the ray tracing scene's character skinning, rt-shadows.md 10) on both backends against a CPU reference of the engine's skinned vertex
// shader: two jobs of different vertex layouts in one dispatch (the job / chunk lookup through byte-address loads at 16-byte offsets), random bones and weights,
// bone indices past the palette (ignored), the bones and the vertices behind addresses that are not at the start of their buffer
void testSkin(rhi::Device& dev, const Shaders& sh) {
    const int before = failures;
    const char* name = "character skinning compute (rt_skin)";
    if (!sh.skin.ok) {
        check(false, "rt_skin shader did not compile");
        report(dev, name, before);
        return;
    }
    std::uint32_t seed = 12345;
    const auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    struct Layout {
        std::uint32_t slots, stride, pos, bw, bi, tc;
    };
    const Layout layouts[2] = {{200, 72, 0, 12, 28, 64}, {97, 80, 4, 16, 32, 44}};
    constexpr std::uint32_t kBoneVec4 = 200, kBoneShift = 256, kVertShift = 64;  // the byte offsets the jobs' addresses start at
    std::vector<float> bones[2];
    std::vector<std::uint8_t> verts[2];
    for (int j = 0; j < 2; ++j) {
        bones[j].resize(kBoneVec4 * 4);
        for (float& f : bones[j]) f = rnd() * 2.0f - 1.0f;
        verts[j].assign(static_cast<std::size_t>(layouts[j].slots) * layouts[j].stride, 0);
        for (std::uint32_t i = 0; i < layouts[j].slots; ++i) {
            std::uint8_t* v = verts[j].data() + static_cast<std::size_t>(i) * layouts[j].stride;
            float pos[3] = {rnd() * 100.0f, rnd() * 100.0f, rnd() * 100.0f};
            float w[4] = {rnd(), rnd(), rnd(), rnd()};
            std::uint8_t idx[4];
            for (auto& b : idx) b = static_cast<std::uint8_t>(rnd() < 0.15f ? 66 + static_cast<int>(rnd() * 100.0f) : static_cast<int>(rnd() * 66.0f));
            float tc[2] = {rnd(), rnd()};
            std::memcpy(v + layouts[j].pos, pos, 12);
            std::memcpy(v + layouts[j].bw, w, 16);
            std::memcpy(v + layouts[j].bi, idx, 4);
            std::memcpy(v + layouts[j].tc, tc, 8);
        }
    }
    std::unique_ptr<rhi::Buffer> vb[2], bb[2], out[2], readback[2];
    auto table = dev.createBuffer({16 + 64 * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi skin table"});
    auto pipeline = dev.createComputePipeline({sh.skin.shader(), "rhi rt skin"});
    bool ok = table && pipeline;
    constexpr VkBufferUsageFlags kRw = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    for (int j = 0; j < 2 && ok; ++j) {
        const std::uint64_t outBytes = 16 + static_cast<std::uint64_t>(layouts[j].slots) * 32;
        vb[j] = dev.createBuffer({kVertShift + verts[j].size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi skin vertices"});
        bb[j] = dev.createBuffer({kBoneShift + kBoneVec4 * 16ull, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi skin bones"});
        out[j] = dev.createBuffer({outBytes, kRw, rhi::Memory::Device, "rhi skin out"});
        readback[j] = dev.createBuffer({outBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi skin readback"});
        ok = vb[j] && bb[j] && out[j] && readback[j];
        if (!ok) break;
        std::memcpy(static_cast<std::uint8_t*>(vb[j]->map()) + kVertShift, verts[j].data(), verts[j].size());
        std::memcpy(static_cast<std::uint8_t*>(bb[j]->map()) + kBoneShift, bones[j].data(), bones[j].size() * 4);
    }
    if (!ok) {
        check(false, "skin resource / pipeline creation");
        report(dev, name, before);
        return;
    }
    auto* t = static_cast<std::uint32_t*>(table->map());
    std::memset(t, 0, 16 + 64 * 2);
    t[0] = 2;
    std::uint32_t chunk = 0;
    for (int j = 0; j < 2; ++j) {
        std::uint32_t* w = t + 4 + 16 * j;
        const Layout& l = layouts[j];
        const auto vr = vb[j]->shaderRef(kVertShift), br = bb[j]->shaderRef(kBoneShift), orf = out[j]->shaderRefRw(0);
        w[0] = vr[0], w[1] = vr[1], w[2] = br[0], w[3] = br[1], w[4] = orf[0], w[5] = orf[1];
        w[6] = l.slots;
        w[7] = l.stride;
        w[8] = chunk;
        w[12] = l.pos | (l.bw << 8) | (l.bi << 16) | (l.tc << 24);
        chunk += (l.slots + 63) / 64;
    }
    t[1] = chunk;
    struct Push {
        std::uint32_t frame[2];
        std::uint32_t passId, passFlags;
        std::uint32_t table[2];
        std::uint32_t pad[2];
    } push{};
    const auto tr = table->shaderRef(0);
    push.table[0] = tr[0];
    push.table[1] = tr[1];
    dev.submitAndWait([&](rhi::CommandList& cl) {
        cl.bindPipeline(*pipeline);
        cl.setPushConstants(&push, sizeof(push));
        cl.dispatch(chunk, 1, 1);
        for (int j = 0; j < 2; ++j) {
            cl.bufferBarrier(*out[j], rhi::BufferAccess::IndirectRead);
            cl.copyBuffer(*readback[j], 0, *out[j], 0, 16 + static_cast<std::uint64_t>(layouts[j].slots) * 32);
        }
    });
    int bad = 0;
    float maxErr = 0.0f;
    for (int j = 0; j < 2; ++j) {
        const Layout& l = layouts[j];
        const auto* o = static_cast<const float*>(readback[j]->map()) + 4;  // after the 16-byte header
        for (std::uint32_t i = 0; i < l.slots; ++i) {
            const std::uint8_t* v = verts[j].data() + static_cast<std::size_t>(i) * l.stride;
            float p[4] = {0, 0, 0, 1}, w[4], tc[2];
            std::uint8_t idx[4];
            std::memcpy(p, v + l.pos, 12);
            std::memcpy(w, v + l.bw, 16);
            std::memcpy(idx, v + l.bi, 4);
            std::memcpy(tc, v + l.tc, 8);
            float rows[3][4] = {};
            for (int k = 0; k < 4; ++k) {
                if (idx[k] >= 66 || w[k] == 0.0f) continue;
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 4; ++c) rows[r][c] += bones[j][(1 + 3 * idx[k] + r) * 4 + c] * w[k];
                }
            }
            float expect[8];
            for (int r = 0; r < 3; ++r) expect[r] = p[0] * rows[r][0] + p[1] * rows[r][1] + p[2] * rows[r][2] + p[3] * rows[r][3];
            expect[3] = 1.0f;
            expect[4] = tc[0];
            expect[5] = tc[1];
            expect[6] = 0.0f;
            expect[7] = 1.0f;
            for (int c = 0; c < 8; ++c) {
                const float e = std::abs(o[i * 8 + c] - expect[c]);
                maxErr = std::max(maxErr, e);
                if (e > 1e-3f * std::max(1.0f, std::abs(expect[c])) && bad++ < 5) {
                    std::printf("    job %d slot %u component %d = %f, expected %f\n", j, i, c, o[i * 8 + c], expect[c]);
                }
            }
        }
    }
    check(bad == 0, std::format("{} skinned component(s) differ from the CPU reference (max error {})", bad, maxErr));
    report(dev, name, before);
}

// F4-5 (frame-ownership.md 14.17): shaders/rt/rt_tree_wind.comp.hlsl (the RT scene's SpeedTree wind by compute) on both backends against the CPU reference of the
// vertex shader's position (enh/tree_wind.h): three jobs of the three vertex classes (wind only, leaf mesh, leaf cards) with different declarations (offsets,
// UBYTE4 indices, FLOAT16_2 texture coordinates) through enh::treewind::layoutOf, vertices / constants behind non-zero offsets, and the compare path: a reference
// capture of job 0 with unwritten slots (w = 0), exact slots and slots moved by 0.5 / 2.0 units: max error, compared / missing / over 0.1 / over 1 counts
void testTreeWind(rhi::Device& dev, const Shaders& sh) {
    namespace tw = rvk::enh::treewind;
    const int before = failures;
    const char* name = "SpeedTree wind compute (rt_tree_wind, F4-5)";
    if (!sh.treeWind.ok) {
        check(false, "rt_tree_wind shader did not compile");
        report(dev, name, before);
        return;
    }
    std::uint32_t seed = 4711;
    const auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    const auto sym = [&]() { return rnd() * 2.0f - 1.0f; };
    // (normal values only: the test's texture coordinates are k / 16 and k / 8)
    const auto toHalf = [](float x) -> std::uint16_t {
        if (x == 0.0f) return 0;
        std::uint32_t b = 0;
        std::memcpy(&b, &x, 4);
        const std::uint32_t sign = (b >> 16) & 0x8000u, e = ((b >> 23) & 0xFFu) - 127u + 15u, m = (b >> 13) & 0x3FFu;
        return static_cast<std::uint16_t>(sign | (e << 10) | m);
    };
    struct Job {
        std::uint32_t op, slots, stride;
        std::vector<tw::Element> els;
        tw::Layout layout;
        std::vector<std::uint8_t> bytes;
        std::vector<tw::Vertex> ref;
        std::vector<float> tc;  // 4 per slot
        float consts[tw::kConsts][4];
    };
    Job jobs[3];
    // job 0: a branch (op 0): POSITION F3 @0, BLENDINDICES UBYTE4 @12, BLENDWEIGHT0 F4 @16, TEXCOORD0 F2 @32, stride 40
    jobs[0].op = 0, jobs[0].slots = 150, jobs[0].stride = 40;
    jobs[0].els = {{0, 0, 2, 0, 0}, {0, 12, 5, 2, 0}, {0, 16, 3, 1, 0}, {0, 32, 1, 5, 0}, {1, 0, 0, 5, 4}};
    // job 1: a leaf mesh (op 2): POSITION F3 @0, NORMAL F3 @12, BLENDINDICES UBYTE4 @24, BLENDWEIGHT0 F4 @28, BLENDWEIGHT2 F4 @44, NORMAL1 F3 @60, NORMAL2 F3 @72,
    // TEXCOORD0 F2 @84, stride 92
    jobs[1].op = 2, jobs[1].slots = 70, jobs[1].stride = 92;
    jobs[1].els = {{0, 0, 2, 0, 0}, {0, 12, 2, 3, 0}, {0, 24, 5, 2, 0}, {0, 28, 3, 1, 0}, {0, 44, 3, 1, 2}, {0, 60, 2, 3, 1}, {0, 72, 2, 3, 2}, {0, 84, 1, 5, 0}};
    // job 2: leaf cards (op 3): POSITION F3 @0, BLENDINDICES UBYTE4 @12, BLENDWEIGHT0 F4 @16, BLENDWEIGHT1 F4 @32, BLENDWEIGHT2 F4 @48, TEXCOORD0 FLOAT16_2 @64, stride 68
    jobs[2].op = 3, jobs[2].slots = 65, jobs[2].stride = 68;
    jobs[2].els = {{0, 0, 2, 0, 0}, {0, 12, 5, 2, 0}, {0, 16, 3, 1, 0}, {0, 32, 3, 1, 1}, {0, 48, 3, 1, 2}, {0, 64, 15, 5, 0}};
    const tw::Class want[3] = {tw::kWind, tw::kLeafMesh, tw::kLeafCards};
    for (int j = 0; j < 3; ++j) {
        Job& J = jobs[j];
        J.layout = tw::layoutOf(J.els, J.op);
        check(J.layout.ok && J.layout.cls == want[j], std::format("job {}: layoutOf gives ok {} class {} ({})", j, J.layout.ok, static_cast<int>(J.layout.cls), J.layout.why != nullptr ? J.layout.why : ""));
        for (auto& c : J.consts) {
            for (float& f : c) f = sym();
        }
        // wind matrices near the identity (the real ones are rotations), rows = a rotation-ish matrix with a far translation (the world's coordinates)
        for (int m = 0; m < 3; ++m) {
            for (int r = 0; r < 3; ++r) J.consts[3 * m + r][r] += 1.0f;
        }
        for (int r = 0; r < 3; ++r) {
            J.consts[17 + r][r] += 1.5f;
            J.consts[17 + r][3] = (rnd() - 0.5f) * 40000.0f;
        }
        J.bytes.assign(static_cast<std::size_t>(J.slots) * J.stride, 0);
        J.ref.resize(J.slots);
        J.tc.resize(static_cast<std::size_t>(J.slots) * 4);
        for (std::uint32_t i = 0; i < J.slots; ++i) {
            std::uint8_t* v = J.bytes.data() + static_cast<std::size_t>(i) * J.stride;
            tw::Vertex& x = J.ref[i];
            const auto put = [&](std::uint32_t attr, float* dst, int n) {
                const std::uint32_t word = J.layout.words[attr];
                for (int c = 0; c < n; ++c) dst[c] = sym() * (attr == tw::kPos ? 300.0f : 1.0f);
                std::memcpy(v + (word & 0xFFFFu), dst, sizeof(float) * n);
            };
            put(tw::kPos, x.pos, 3);
            x.pos[3] = 1.0f;
            const std::uint8_t idx[4] = {static_cast<std::uint8_t>(i % 3), 7, 1, 200};
            std::memcpy(v + (J.layout.words[tw::kIndices0] & 0xFFFFu), idx, 4);
            x.indices0[0] = static_cast<float>(idx[0]);
            put(tw::kWeights0, x.weights0, 4);
            x.weights0[0] = rnd();
            std::memcpy(v + (J.layout.words[tw::kWeights0] & 0xFFFFu), x.weights0, 16);
            if (J.layout.cls != tw::kWind) {
                put(tw::kWeights2, x.weights2, 4);
                x.weights2[3] = static_cast<float>(i % 4);
                std::memcpy(v + (J.layout.words[tw::kWeights2] & 0xFFFFu), x.weights2, 16);
            }
            if (J.layout.cls == tw::kLeafMesh) {
                put(tw::kNormal1, x.normal1, 3);
                put(tw::kNormal2, x.normal2, 3);
            }
            if (J.layout.cls == tw::kLeafCards) put(tw::kWeights1, x.weights1, 4);
            float* tc = J.tc.data() + static_cast<std::size_t>(i) * 4;
            tc[0] = static_cast<float>(i % 32) / 16.0f;
            tc[1] = static_cast<float>(i % 7) / 8.0f;
            tc[2] = 0.0f;
            tc[3] = 1.0f;
            const std::uint32_t tw0 = J.layout.words[tw::kTex0];
            if ((tw0 >> 16) == 15) {
                const std::uint16_t h[2] = {toHalf(tc[0]), toHalf(tc[1])};
                std::memcpy(v + (tw0 & 0xFFFFu), h, 4);
            } else {
                std::memcpy(v + (tw0 & 0xFFFFu), tc, 8);
            }
        }
    }
    constexpr std::uint32_t kVertShift = 64, kConstShift = 48, kErrAt = 0, kRefAt = 256;
    std::unique_ptr<rhi::Buffer> vb[3], kb[3];
    std::uint64_t outAt[3];
    std::uint64_t workBytes = kRefAt + 16 + static_cast<std::uint64_t>(jobs[0].slots) * 32;
    for (int j = 0; j < 3; ++j) {
        workBytes = (workBytes + 255) & ~255ull;
        outAt[j] = workBytes;
        workBytes += 16 + static_cast<std::uint64_t>(jobs[j].slots) * 32;
    }
    constexpr VkBufferUsageFlags kRw = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto table = dev.createBuffer({tw::kHeadBytes + tw::kJobWords * 4ull * 3, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi tree wind table"});
    auto work = dev.createBuffer({workBytes, kRw, rhi::Memory::Device, "rhi tree wind work"});
    auto fill = dev.createBuffer({workBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Upload, "rhi tree wind fill"});
    auto readback = dev.createBuffer({workBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "rhi tree wind readback"});
    auto pipeline = dev.createComputePipeline({sh.treeWind.shader(), "rhi rt tree wind"});
    bool ok = table && work && fill && readback && pipeline;
    for (int j = 0; j < 3 && ok; ++j) {
        vb[j] = dev.createBuffer({kVertShift + jobs[j].bytes.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi tree wind vertices"});
        kb[j] = dev.createBuffer({kConstShift + tw::kConsts * 16ull, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "rhi tree wind constants"});
        ok = vb[j] && kb[j];
        if (!ok) break;
        std::memcpy(static_cast<std::uint8_t*>(vb[j]->map()) + kVertShift, jobs[j].bytes.data(), jobs[j].bytes.size());
        std::memcpy(static_cast<std::uint8_t*>(kb[j]->map()) + kConstShift, jobs[j].consts, sizeof(jobs[j].consts));
    }
    if (!ok) {
        check(false, "tree wind resource / pipeline creation");
        report(dev, name, before);
        return;
    }
    // the CPU's expectations
    std::vector<float> expect[3];
    for (int j = 0; j < 3; ++j) {
        expect[j].resize(static_cast<std::size_t>(jobs[j].slots) * 8);
        for (std::uint32_t i = 0; i < jobs[j].slots; ++i) {
            float w[3];
            tw::world(jobs[j].consts, jobs[j].ref[i], jobs[j].layout.cls, w);
            float* e = expect[j].data() + static_cast<std::size_t>(i) * 8;
            e[0] = w[0], e[1] = w[1], e[2] = w[2], e[3] = 1.0f;
            std::memcpy(e + 4, jobs[j].tc.data() + static_cast<std::size_t>(i) * 4, 16);
        }
    }
    // the work buffer: the accumulator 0, the reference capture of job 0 (header + slots: every 5th unwritten, every 7th + 1 moved by 0.5, slot 3 by 2.0), the outputs poisoned
    auto* f = static_cast<std::uint8_t*>(fill->map());
    std::memset(f, 0xAB, workBytes);
    std::memset(f + kErrAt, 0, tw::kErrBytes);
    std::uint32_t refWritten = 0, refOver01 = 0, refOver1 = 0, refMissing = 0;
    {
        auto* r = reinterpret_cast<float*>(f + kRefAt + 16);
        for (std::uint32_t i = 0; i < jobs[0].slots; ++i) {
            float* s = r + static_cast<std::size_t>(i) * 8;
            const float* e = expect[0].data() + static_cast<std::size_t>(i) * 8;
            if (i % 5 == 0) {
                std::memset(s, 0, 32);
                ++refMissing;
                continue;
            }
            const float d = i == 3 ? 2.0f : (i % 7 == 1 ? 0.5f : 0.0f);
            s[0] = e[0], s[1] = e[1], s[2] = e[2] + d, s[3] = 1.0f;
            s[4] = s[5] = s[6] = s[7] = 0.0f;
            ++refWritten;
            if (d > 0.1f) ++refOver01;
            if (d > 1.0f) ++refOver1;
        }
    }
    auto* t = static_cast<std::uint32_t*>(table->map());
    std::memset(t, 0, tw::kHeadBytes + tw::kJobWords * 4ull * 3);
    const auto er = work->shaderRefRw(kErrAt);
    t[0] = 3;
    t[2] = er[0];
    t[3] = er[1];
    std::uint32_t chunk = 0;
    for (int j = 0; j < 3; ++j) {
        std::uint32_t* w = t + 4 + tw::kJobWords * j;
        const Job& J = jobs[j];
        const auto vr = vb[j]->shaderRef(kVertShift), kr = kb[j]->shaderRef(kConstShift), orf = work->shaderRefRw(outAt[j]);
        w[0] = vr[0], w[1] = vr[1], w[2] = kr[0], w[3] = kr[1], w[4] = orf[0], w[5] = orf[1];
        w[6] = J.slots;
        w[7] = J.stride;
        w[8] = chunk;
        w[9] = static_cast<std::uint32_t>(J.layout.cls);
        if (j == 0) {
            const auto rr = work->shaderRefRw(kRefAt + 16);
            w[10] = rr[0], w[11] = rr[1];
        }
        std::memcpy(w + 12, J.layout.words, sizeof(J.layout.words));
        chunk += (J.slots + 63) / 64;
    }
    t[1] = chunk;
    struct Push {
        std::uint32_t frame[2];
        std::uint32_t passId, passFlags;
        std::uint32_t table[2];
        std::uint32_t pad[2];
    } push{};
    const auto tr = table->shaderRef(0);
    push.table[0] = tr[0];
    push.table[1] = tr[1];
    dev.submitAndWait([&](rhi::CommandList& cl) {
        cl.copyBuffer(*work, 0, *fill, 0, workBytes);
        cl.bufferBarrier(*work, rhi::BufferAccess::ShaderWrite);
        cl.bindPipeline(*pipeline);
        cl.setPushConstants(&push, sizeof(push));
        cl.dispatch(chunk, 1, 1);
        cl.bufferBarrier(*work, rhi::BufferAccess::IndirectRead);
        cl.copyBuffer(*readback, 0, *work, 0, workBytes);
    });
    const auto* rb = static_cast<const std::uint8_t*>(readback->map());
    int bad = 0;
    float maxErr = 0.0f;
    for (int j = 0; j < 3; ++j) {
        const auto* o = reinterpret_cast<const float*>(rb + outAt[j] + 16);
        for (std::uint32_t i = 0; i < jobs[j].slots; ++i) {
            for (int c = 0; c < 8; ++c) {
                const float e = expect[j][static_cast<std::size_t>(i) * 8 + c], g = o[static_cast<std::size_t>(i) * 8 + c];
                const float err = std::abs(g - e);
                maxErr = std::max(maxErr, err / std::max(1.0f, std::abs(e)));
                if (!(err <= 4e-6f * std::max(1.0f, std::abs(e)) + 1e-4f) && bad++ < 6) std::printf("    job %d slot %u component %d = %f, expected %f\n", j, i, c, g, e);
            }
        }
        // the header stays the caller's (the shader writes slots only), the slot past the last stays poisoned
        const auto* past = rb + outAt[j] + 16 + static_cast<std::uint64_t>(jobs[j].slots) * 32;
        if (outAt[j] + 16 + static_cast<std::uint64_t>(jobs[j].slots) * 32 + 4 <= workBytes) check(past[0] == 0xAB, std::format("job {} wrote past its last slot", j));
    }
    check(bad == 0, std::format("{} component(s) differ from the CPU reference of the SpeedTree vertex shader (max relative error {})", bad, maxErr));
    std::uint32_t acc[8];
    std::memcpy(acc, rb + kErrAt, sizeof(acc));
    float gotMax = 0.0f;
    std::memcpy(&gotMax, &acc[0], 4);
    check(acc[1] == refWritten && acc[2] == refMissing && acc[3] == refOver01 && acc[4] == refOver1,
          std::format("compare counts: compared {} (want {}), missing {} (want {}), over 0.1 {} (want {}), over 1 {} (want {})", acc[1], refWritten, acc[2], refMissing, acc[3], refOver01, acc[4], refOver1));
    check(std::abs(gotMax - 2.0f) < 0.01f, std::format("compare max error {} (want 2.0)", gotMax));
    report(dev, name, before);
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

bool g_syncValidation = false;

bool runBackend(rhi::Backend backend, bool validation, const Shaders& sh) {
    rhi::DeviceDesc dd;
    dd.backend = backend;
    dd.validation = validation;
    dd.syncValidation = g_syncValidation;
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
    if (std::getenv("RVK_ABI_ONLY") != nullptr) {  // only the ABI probe (a quick run under the D3D12 GPU-based validation)
        failures += abiprobe::run(*dev);
        check(dev->validationErrors() == 0, std::format("{} validation error(s)", dev->validationErrors()));
        std::printf("[%s] %s (RVK_ABI_ONLY)\n", rhi::backendName(backend), failures == before ? "PASS" : "FAIL");
        return failures == before;
    }
    if (std::getenv("RVK_PICTURE_ONLY") != nullptr) {  // only the picture check's compute pass (a run under the D3D12 GPU-based validation)
        testPictureCheck(*dev, sh);
        check(dev->validationErrors() == 0, std::format("{} validation error(s)", dev->validationErrors()));
        std::printf("[%s] %s (RVK_PICTURE_ONLY)\n", rhi::backendName(backend), failures == before ? "PASS" : "FAIL");
        return failures == before;
    }
    testTextures(*dev, sh);
    testBuffers(*dev, sh);
    testVertex(*dev, sh);
    testIndirectPull(*dev, sh);
    testPullFormats(*dev, sh);
    testOwnedCompact(*dev, sh);
    testPictureCheck(*dev, sh);
    testDepthZ(*dev, sh);
    testOwnedCull(*dev, sh);
    testSkin(*dev, sh);
    testTreeWind(*dev, sh);
    testRayQuery(*dev, sh);
    testAccelOps(*dev, sh);
    testTextureOps(*dev, sh);
    failures += abiprobe::run(*dev);
    failures += parity::run(*dev);
    check(dev->validationErrors() == 0, std::format("{} validation error(s)", dev->validationErrors()));
    std::printf("[%s] %s\n", rhi::backendName(backend), failures == before ? "PASS" : "FAIL");
    return failures == before;
}

// --device-lost: the DX12 device-removal report (DRED + a minidump into a temp directory) after ID3D12Device5::RemoveDevice, and PIX events on a live list
bool runDeviceLost(bool validation) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "rvk_dumps_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    rvk::crashdump::setDirectory(dir);
    rhi::DeviceDesc dd;
    dd.backend = rhi::Backend::D3D12;
    dd.validation = validation;
    dd.messages = true;
    dd.name = "rvk_rhitest device lost";
    auto dev = rhi::createD3D12Device(dd);
    if (!dev) {
        std::printf("[d3d12] SKIP: no device\n");
        return true;
    }
    const int before = failures;
    dev->submitAndWait([&](rhi::CommandList& cl) {
        cl.beginRegion("before the loss");
        cl.endRegion();
    });
    check(!dev->deviceLost(), "device alive before the removal");
    check(dev->simulateDeviceLoss(), "RemoveDevice");
    dev->submitAndWait([&](rhi::CommandList& cl) {
        cl.beginRegion("after the loss");
        cl.endRegion();
    });
    check(dev->deviceLost(), "device lost detected");
    bool dump = false, report = false;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        dump = dump || e.path().extension() == ".dmp";
        if (e.path().extension() == ".txt") {
            std::ifstream in(e.path());
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            report = text.find("DRED") != std::string::npos && text.find("device removed") != std::string::npos;
        }
    }
    check(dump, "a minidump was written");
    check(report, "the report names DRED");
    std::printf("[d3d12] device lost: %s (%s)\n", failures == before ? "PASS" : "FAIL", dir.string().c_str());
    return failures == before;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::printf("rvk_rhitest %ls (tracy: %s)\n", exe, prof::kTracy ? "on" : "off");
    }
    bool vulkan = false, d3d12 = false, validation = false, fsr = false, fg = false, deviceLost = false, gbv = false;
    std::string ffxDll;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--fsr" || a == "--fg") {
            (a == "--fsr" ? fsr : fg) = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') ffxDll = argv[++i];
        } else if (a == "--vulkan") vulkan = true;
        else if (a == "--d3d12") d3d12 = true;
        else if (a == "--validation") validation = true;
        else if (a == "--sync-validation") validation = g_syncValidation = true;
        else if (a == "--device-lost") deviceLost = true;
        else if (a == "--gbv") gbv = validation = true;
    }
    // the D3D12 device reads RVK_NO_GBV when its debug layer is on (rhi/d3d12): without --gbv the layer runs without GPU-based validation
    if (validation && !gbv) SetEnvironmentVariableA("RVK_NO_GBV", "1");
    if (validation) std::printf("validation: Vulkan layer%s; D3D12 debug layer %s\n", g_syncValidation ? " + synchronization" : "",
                                gbv ? "+ GPU-based validation (--gbv)" : "without GPU-based validation (--gbv adds it)");
    if (deviceLost) {
        runDeviceLost(validation);
        std::printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);
        return failures == 0 ? 0 : 1;
    }
    if (fsr || fg) return ffxtest::run(fsr, fg, ffxDll, validation);
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
    compileBoth("test/rhi_ind.comp.hlsl", "CSMain", "cs_6_6", sh.indCs);
    compileBoth("test/rhi_ind.vert.hlsl", "VSMain", "vs_6_6", sh.indVs);
    compileBoth("test/rhi_pull.vert.hlsl", "VSMain", "vs_6_6", sh.pullVs);
    compileBoth("gpuscene/owned_compact.comp.hlsl", "CSMain", "cs_6_6", sh.compact);
    compileBoth("rt/rt_skin.comp.hlsl", "CSMain", "cs_6_6", sh.skin);
    compileBoth("rt/rt_tree_wind.comp.hlsl", "CSMain", "cs_6_6", sh.treeWind);
    compileBoth("debug/picture_check.comp.hlsl", "CSMain", "cs_6_6", sh.picture);
    compileBoth("test/depth_z_scene.vert.hlsl", "VSMain", "vs_6_6", sh.dzVs);
    compileBoth("test/depth_z_scene.frag.hlsl", "PSMain", "ps_6_6", sh.dzPsOld);
    compileBoth("test/depth_z_scene.frag.hlsl", "PSDepthOnly", "ps_6_6", sh.dzPsNone);
    compileBoth("debug/fullscreen_rhi.vert.hlsl", "VSMain", "vs_6_6", sh.dzFs);
    compileBoth("depth/depth_z.frag.hlsl", "PSMain", "ps_6_6", sh.dzDerive);
    compileBoth("test/rhi_rt.comp.hlsl", "CSMain", "cs_6_6", sh.rt);
    compileBoth("test/rhi_store8.comp.hlsl", "CSMain", "cs_6_6", sh.store8);
    if (vulkan) runBackend(rhi::Backend::Vulkan, validation, sh);
    if (d3d12) runBackend(rhi::Backend::D3D12, validation, sh);
    std::printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
