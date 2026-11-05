// DXIL parity tests of rvk_rhitest (docs/research/hlsl-rhi.md "DXIL parity (S1)"): production shader code drawn / dispatched through the RHI on both
// backends from one HLSL text, compared with the CPU where the result is easy to compute and ALWAYS between the two backends (the first backend
// that runs stores its images / words, the second compares: `rvk_rhitest` runs Vulkan first, then D3D12). What they cover:
//   translated draw   a D3D9 vs_3_0 / ps_3_0 pair (assembled with d3dx9_39, translated by shader::toHlsl) drawn with the real push block (RvkDrawPush: stage
//                     constants through the register windows, the DrawBlock with bindless texture / sampler slots, the FrameBlock reached by reference from the
//                     translator hooks), vertex input locations -> ATTRn / VARYn semantics
//   ubershader draws  shaders/material/ge_default.frag.hlsl built from two captured engine requests (one with seven textures, normal map, specular, a
//                     lightmap, directional + ambient light) and the enhancement hooks (gbuffer + gi: the frame block, derivatives, three render targets)
//   references        shaders/test/parity_refs.comp.hlsl: rvkGiLookup (frame block -> volume header -> probes / depth moments) and the RT geometry record
//                     fetch (rvk_rt.hlsli: records, 16 / 32-bit indices at 2-byte alignment, vertex attributes, material segments)

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
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/d3dx.h"
#include "rhi/rhi.h"
#include "shader/abi.h"
#include "shader/dxc.h"
#include "shader/embedded.h"
#include "shader/hlsl.h"
#include "shader/sm_ir.h"
#include "shader/uber.h"

namespace rvk::parity {
namespace {

namespace abi = shader::abi;

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::printf("  FAIL: %s\n", what.c_str());
    }
}

struct Blobs {
    std::vector<std::uint32_t> spirv;
    std::vector<std::uint8_t> dxil;
    bool ok = false;
    rhi::Shader shader() const { return {spirv, dxil}; }
};

// compiles a source text (or, with an empty `source`, the embedded file `file`) to SPIR-V and DXIL; prints the log on failure
bool compileBoth(const std::string& file, const std::string& source, const char* entry, const char* profile, Blobs& out, int opt = 1) {
    shader::dxc::Request r;
    r.file = file;
    r.source = source;
    r.entry = entry;
    r.profile = profile;
    r.optLevel = opt;
    r.cache = false;
    r.target = shader::dxc::Target::Spirv;
    shader::dxc::Result a = shader::dxc::compile(r);
    r.target = shader::dxc::Target::Dxil;
    shader::dxc::Result b = shader::dxc::compile(r);
    if (!a.ok() || !b.ok()) {
        std::printf("  compile %s failed:\n%s\n%s\n", file.c_str(), a.log.c_str(), b.log.c_str());
        return false;
    }
    out.spirv = std::move(a.words);
    out.dxil = std::move(b.bytes);
    out.ok = true;
    return true;
}

// ---- results shared between the backends ------------------------------------------------------------------------------------------------

struct Stored {
    std::string backend;
    std::vector<float> data;
};
std::map<std::string, Stored> g_first;

// The result of `test` on this backend against the first backend's (stored when this is the first). `tolerance` = the largest allowed absolute
// difference per value; `badShare` = the share of values allowed to exceed it (derivative / filter noise at texel borders). NaNs must match.
void crossCheck(rhi::Device& dev, const char* test, const std::vector<float>& data, float tolerance, float badShare = 0.0f) {
    const std::string backend = rhi::backendName(dev.caps().backend);
    auto it = g_first.find(test);
    if (it == g_first.end()) {
        g_first[test] = {backend, data};
        return;
    }
    const std::vector<float>& ref = it->second.data;
    check(ref.size() == data.size(), std::format("{}: {} has {} values, {} has {}", test, it->second.backend, ref.size(), backend, data.size()));
    if (ref.size() != data.size()) return;
    std::size_t bad = 0, nan = 0, worstAt = 0;
    float worst = 0.0f;
    for (std::size_t i = 0; i < data.size(); ++i) {
        const bool na = std::isnan(ref[i]), nb = std::isnan(data[i]);
        if (na || nb) {
            if (na != nb) ++nan;
            continue;
        }
        const float d = std::fabs(ref[i] - data[i]);
        if (d > tolerance) ++bad;
        if (d > worst) {
            worst = d;
            worstAt = i;
        }
    }
    const bool ok = nan == 0 && static_cast<double>(bad) <= badShare * static_cast<double>(data.size());
    check(ok, std::format("{}: {} vs {}: {} of {} values differ by more than {} (worst {} at {}: {} vs {}), {} NaN mismatch(es)", test, it->second.backend,
                          backend, bad, data.size(), tolerance, worst, worstAt, ref[worstAt], data[worstAt], nan));
    std::printf("  %s: %s vs %s: %zu values, worst difference %g%s\n", test, it->second.backend.c_str(), backend.c_str(), data.size(), worst,
                ok ? "" : " (MISMATCH)");
}

std::vector<float> toFloats(const std::uint8_t* p, std::size_t n) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<float>(p[i]);
    return v;
}

// ---- a small harness: textures, upload buffers, a draw into RGBA8 targets -----------------------------------------------------------------

constexpr std::uint32_t kTarget = 64;

struct Harness {
    rhi::Device& dev;
    std::vector<std::unique_ptr<rhi::Texture>> textures;
    std::vector<std::unique_ptr<rhi::Buffer>> buffers;

    explicit Harness(rhi::Device& d) : dev(d) {}

    rhi::Buffer* makeBuffer(const void* data, std::size_t size, VkBufferUsageFlags usage, const char* name) {
        auto b = dev.createBuffer({size, usage, rhi::Memory::Upload, name});
        if (!b) return nullptr;
        if (data != nullptr) std::memcpy(b->map(), data, size);
        else std::memset(b->map(), 0, size);
        buffers.push_back(std::move(b));
        return buffers.back().get();
    }

    // an RGBA8 texture w x h from `rgba` (w * h * 4 bytes), sampled through the heap: its index (0 on failure)
    std::uint32_t addTexture(std::uint32_t w, std::uint32_t h, const std::vector<std::uint8_t>& rgba, const char* name) {
        const std::uint32_t pitch = rhi::bufferRowPitch(VK_FORMAT_R8G8B8A8_UNORM, w);
        auto staging = dev.createBuffer({static_cast<std::uint64_t>(pitch) * h, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Upload, name});
        auto tex = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {w, h, 1}, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, name});
        if (!staging || !tex) return 0;
        for (std::uint32_t y = 0; y < h; ++y) std::memcpy(static_cast<std::uint8_t*>(staging->map()) + static_cast<std::size_t>(y) * pitch, rgba.data() + static_cast<std::size_t>(y) * w * 4, w * 4);
        dev.submitAndWait([&](rhi::CommandList& cl) {
            cl.transition(*tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            cl.copyBufferToTexture(*tex, *staging, 0);
            cl.transition(*tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        textures.push_back(std::move(tex));
        buffers.push_back(std::move(staging));
        return textures.back()->heapSampled();
    }

    // a cube map of 6 faces of w x w RGBA8 texels (face f, texel x, y = c(f, x, y)): its heap index (a TextureCube view)
    template <typename F>
    std::uint32_t addCube(std::uint32_t w, F c, const char* name) {
        const std::uint32_t pitch = rhi::bufferRowPitch(VK_FORMAT_R8G8B8A8_UNORM, w);
        const std::uint64_t faceBytes = static_cast<std::uint64_t>(pitch) * w;
        auto staging = dev.createBuffer({faceBytes * 6, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Upload, name});
        rhi::TextureDesc td{VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {w, w, 1}, 1, 6, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, name};
        td.cube = true;
        auto tex = dev.createTexture(td);
        if (!staging || !tex) return 0;
        for (std::uint32_t face = 0; face < 6; ++face) {
            for (std::uint32_t y = 0; y < w; ++y) {
                for (std::uint32_t x = 0; x < w; ++x) {
                    const std::array<std::uint8_t, 4> px = c(face, x, y);
                    std::memcpy(static_cast<std::uint8_t*>(staging->map()) + face * faceBytes + static_cast<std::size_t>(y) * pitch + x * 4, px.data(), 4);
                }
            }
        }
        dev.submitAndWait([&](rhi::CommandList& cl) {
            cl.transition(*tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            for (std::uint32_t face = 0; face < 6; ++face) cl.copyBufferToTexture(*tex, *staging, face * faceBytes, {0, face});
            cl.transition(*tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        textures.push_back(std::move(tex));
        buffers.push_back(std::move(staging));
        return textures.back()->heapSampled();
    }

    // a w x h texture whose texel (x, y) = f(x, y) (RGBA 0..255)
    template <typename F>
    std::uint32_t addPattern(std::uint32_t w, std::uint32_t h, F f, const char* name) {
        std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::array<std::uint8_t, 4> c = f(x, y);
                std::memcpy(px.data() + (static_cast<std::size_t>(y) * w + x) * 4, c.data(), 4);
            }
        }
        return addTexture(w, h, px, name);
    }
};

// a block pointer pair as the shaders read it (Vulkan: the device address, DX12: heap slot + offset)
std::uint64_t refOf(const rhi::Buffer& b, std::uint64_t offset = 0) {
    const auto r = b.shaderRef(offset);
    return static_cast<std::uint64_t>(r[0]) | (static_cast<std::uint64_t>(r[1]) << 32);
}

// the quad: two triangles, position float3 + uv float2 (20 bytes per vertex), top left (0, 0) .. bottom right (1, 1)
constexpr float kQuad[6][5] = {{-1, 1, 0, 0, 0}, {1, 1, 0, 1, 0}, {-1, -1, 0, 0, 1}, {1, 1, 0, 1, 0}, {1, -1, 0, 1, 1}, {-1, -1, 0, 0, 1}};

struct Target {
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    std::unique_ptr<rhi::Texture> tex;
    std::unique_ptr<rhi::Buffer> readback;
    std::uint32_t pitch = 0;
};

// draws the quad with `pc` into targets (cleared to (0, 0, 0, 1)); fills each target's readback
bool drawQuad(Harness& h, const Blobs& vs, const Blobs& ps, const abi::PushConstants& pc, std::vector<Target>& targets, const char* name) {
    rhi::Device& dev = h.dev;
    auto vb = dev.createBuffer({sizeof(kQuad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, rhi::Memory::Upload, "parity quad"});
    if (!vb) return false;
    std::memcpy(vb->map(), kQuad, sizeof(kQuad));
    const VkVertexInputBindingDescription binding{0, 20, VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[2] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 12}};
    rhi::GraphicsPipelineDesc gd;
    gd.vs = vs.shader();
    gd.ps = ps.shader();
    for (std::size_t i = 0; i < targets.size(); ++i) gd.colorFormats[i] = targets[i].format;
    gd.vertexBindings = std::span<const VkVertexInputBindingDescription>(&binding, 1);
    gd.vertexAttributes = attributes;
    gd.name = name;
    auto pipeline = dev.createGraphicsPipeline(gd);
    if (!pipeline) return false;
    for (Target& t : targets) {
        t.tex = dev.createTexture({VK_IMAGE_TYPE_2D, t.format, {kTarget, kTarget, 1}, 1, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "parity target"});
        t.pitch = rhi::bufferRowPitch(t.format, kTarget);
        t.readback = dev.createBuffer({static_cast<std::uint64_t>(t.pitch) * kTarget, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "parity readback"});
        if (!t.tex || !t.readback) return false;
    }
    dev.submitAndWait([&](rhi::CommandList& cl) {
        for (Target& t : targets) cl.transition(*t.tex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        rhi::RenderingInfo ri;
        ri.colorCount = static_cast<std::uint32_t>(targets.size());
        for (std::size_t i = 0; i < targets.size(); ++i) {
            ri.color[i].texture = targets[i].tex.get();
            ri.color[i].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ri.color[i].clear.float32[3] = 1.0f;
        }
        ri.width = ri.height = kTarget;
        cl.beginRendering(ri);
        cl.bindPipeline(*pipeline);
        cl.setPushConstants(&pc, sizeof(pc));
        rhi::Buffer* buffers[1] = {vb.get()};
        const std::uint64_t offsets[1] = {0};
        const std::uint32_t strides[1] = {20};
        cl.setVertexBuffers(0, buffers, offsets, strides);
        cl.draw(6, 1, 0, 0);
        cl.endRendering();
        for (Target& t : targets) {
            cl.transition(*t.tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            cl.copyTextureToBuffer(*t.readback, 0, *t.tex);
        }
    });
    return true;
}

// the RGBA8 readback as bytes without row padding
std::vector<std::uint8_t> pixelsOf(const Target& t) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(kTarget) * kTarget * 4);
    const auto* src = static_cast<const std::uint8_t*>(t.readback->map());
    for (std::uint32_t y = 0; y < kTarget; ++y) std::memcpy(out.data() + static_cast<std::size_t>(y) * kTarget * 4, src + static_cast<std::size_t>(y) * t.pitch, kTarget * 4);
    return out;
}

// RG16F readback as floats (2 per pixel)
float halfToFloat(std::uint16_t h) {
    const std::uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    const float f = e == 0 ? std::ldexp(static_cast<float>(m), -24) : e == 31 ? INFINITY : std::ldexp(static_cast<float>(m | 1024), static_cast<int>(e) - 25);
    return s ? -f : f;
}

// the translated shader of D3D9 assembly (vertex / pixel), compiled for both targets; false (with the reason printed) on failure
bool translated(const std::string& asmText, bool vertex, const shader::TranslateOptions& options, Blobs& out, const char* name) {
    std::string errors;
    const auto tokens = devtools::D3DX::get().assemble(asmText, &errors);
    if (tokens.empty()) {
        std::printf("  assembling %s failed: %s\n", name, errors.c_str());
        return false;
    }
    const shader::DecodeResult dec = shader::decode(tokens);
    if (!dec.ok()) {
        std::printf("  decoding %s failed: %s\n", name, dec.error.c_str());
        return false;
    }
    // D3D12 links the stages by register: translated stages have the dense signature (the game device's setting; Vulkan ignores the fillers)
    shader::setVaryingFillers(true);
    const shader::HlslResult h = shader::toHlsl(dec.program, options);
    shader::setVaryingFillers(false);
    if (!h.ok()) {
        std::printf("  translating %s failed: %s\n", name, h.error.c_str());
        return false;
    }
    return compileBoth(std::string(name) + ".hlsl", h.hlsl, vertex ? "VSMain" : "PSMain", vertex ? "vs_6_6" : "ps_6_6", out);
}

// a stage's constant block (StageConsts: 256 float4, then int and bool registers) with c[n] set from a table
struct StageBlock {
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(abi::kStageConstsSize, 0);
    void set(std::uint32_t reg, float x, float y, float z, float w) {
        const float v[4] = {x, y, z, w};
        std::memcpy(bytes.data() + reg * 16, v, 16);
    }
};

abi::PushConstants pushFor(const rhi::Buffer& vsConsts, const rhi::Buffer& psConsts, const rhi::Buffer& draw, const rhi::Buffer& frame) {
    abi::PushConstants pc{};
    pc.vsConsts = refOf(vsConsts);
    pc.psConsts = refOf(psConsts);
    pc.draw = refOf(draw);
    pc.frame = refOf(frame);
    pc.legacyWindows();  // every window = the stage's one full block
    return pc;
}

// ---- test 1: a translated vertex + pixel shader pair --------------------------------------------------------------------------------------

// vs: position = M * (x, y, z) + c3 through c0..c2, texcoord0 passed on, colour0 = c4
const char* kVsAsm = R"(vs_3_0
dcl_position v0
dcl_texcoord0 v1
dcl_position o0
dcl_texcoord0 o1
dcl_color0 o2
mul r0, v0.x, c0
mad r0, v0.y, c1, r0
mad r0, v0.z, c2, r0
add o0, r0, c3
mov o1, v1
mov o2, c4
)";
// ps: texture s0 * the interpolated colour * c0
const char* kPsAsm = R"(ps_3_0
dcl_texcoord0 v0
dcl_color0 v1
dcl_2d s0
texld r0, v0, s0
mul r0, r0, v1
mul r0, r0, c0
mov oC0, r0
)";

void testTranslatedDraw(Harness& h) {
    rhi::Device& dev = h.dev;
    const int before = failures;
    if (!devtools::D3DX::get().available()) {
        std::printf("[%s] translated draw: SKIP (d3dx9_39.dll not installed: no D3D9 assembler)\n", rhi::backendName(dev.caps().backend));
        return;
    }
    // hooks: the vertex shader's texcoord0 scaled by FrameBlock params[1].x (the billboard fade variant's input override), the pixel shader's rgb by
    // FrameBlock time.x (an epilogue): both reach the frame block by reference
    shader::TranslateOptions vsOpt, psOpt;
    vsOpt.inputOverrides.push_back({shader::Usage::TexCoord, 0, "float4($v.xy * rvkPtr<RvkFrameData, 16>(pc.frame).Get().params[1].x, $v.zw)"});
    psOpt.epilogue = "    oC0.rgb *= rvkLoadFrame(pc.frame).time.x;";
    Blobs vs, ps;
    if (!translated(kVsAsm, true, vsOpt, vs, "parity_vs") || !translated(kPsAsm, false, psOpt, ps, "parity_ps")) {
        check(false, "translated draw: shaders");
        return;
    }
    // a 4 x 4 texture: texel (x, y) = (64 x + 32, 64 y + 32, 128, 255); nearest sampling, clamp
    const std::uint32_t tex = h.addPattern(4, 4, [](std::uint32_t x, std::uint32_t y) { return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(64 * x + 32), static_cast<std::uint8_t>(64 * y + 32), 128, 255}; }, "parity texture");
    const rhi::SamplerIndex smp = dev.createSampler({VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE});
    StageBlock vsc, psc;
    vsc.set(0, 0.8f, 0, 0, 0);
    vsc.set(1, 0, 0.8f, 0, 0);
    vsc.set(2, 0, 0, 1, 0);
    vsc.set(3, 0, 0, 0, 1);
    vsc.set(4, 1.0f, 0.5f, 1.0f, 1.0f);
    psc.set(0, 1.0f, 0.9f, 0.8f, 1.0f);
    abi::DrawBlock db{};
    db.tex[0] = tex;
    db.smp[0] = smp;
    abi::FrameBlock fb{};
    fb.time[0] = 0.5f;
    fb.params[1][0] = 0.5f;
    auto* bVs = h.makeBuffer(vsc.bytes.data(), vsc.bytes.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "parity vs consts");
    auto* bPs = h.makeBuffer(psc.bytes.data(), psc.bytes.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "parity ps consts");
    auto* bDraw = h.makeBuffer(&db, sizeof(db), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "parity draw block");
    auto* bFrame = h.makeBuffer(&fb, sizeof(fb), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "parity frame block");
    if (tex == 0 || smp == 0 || !bVs || !bPs || !bDraw || !bFrame) {
        check(false, "translated draw: resources");
        return;
    }
    std::vector<Target> targets(1);
    if (!drawQuad(h, vs, ps, pushFor(*bVs, *bPs, *bDraw, *bFrame), targets, "parity translated")) {
        check(false, "translated draw: pipeline / target");
        return;
    }
    const std::vector<std::uint8_t> px = pixelsOf(targets[0]);
    // probes at the centres of the quad's four quadrants: u' = u / 2 (the vertex hook), texel = floor(4 u'), colour = texel x (1, .5, 1, 1) x c0, rgb x 0.5
    int probes = 0;
    for (int qy = 0; qy < 2; ++qy) {
        for (int qx = 0; qx < 2; ++qx) {
            const std::uint32_t x = 16 + 32 * qx, y = 16 + 32 * qy;
            const float ndcX = (static_cast<float>(x) + 0.5f) / kTarget * 2.0f - 1.0f, ndcY = 1.0f - (static_cast<float>(y) + 0.5f) / kTarget * 2.0f;
            const float u = (ndcX / 0.8f + 1.0f) * 0.5f, v = (1.0f - ndcY / 0.8f) * 0.5f;
            const int tx = std::clamp(static_cast<int>(std::floor(u * 0.5f * 4.0f)), 0, 3), ty = std::clamp(static_cast<int>(std::floor(v * 0.5f * 4.0f)), 0, 3);
            const float want[3] = {(64.0f * tx + 32.0f) / 255.0f * 1.0f * 1.0f * 0.5f, (64.0f * ty + 32.0f) / 255.0f * 0.5f * 0.9f * 0.5f,
                                   128.0f / 255.0f * 1.0f * 0.8f * 0.5f};
            const std::uint8_t* p = px.data() + (static_cast<std::size_t>(y) * kTarget + x) * 4;
            bool ok = p[3] == 255;
            for (int c = 0; c < 3; ++c) ok = ok && std::abs(static_cast<int>(p[c]) - static_cast<int>(std::lround(want[c] * 255.0f))) <= 2;
            check(ok, std::format("translated draw, quadrant ({}, {}) texel ({}, {}): got ({}, {}, {}, {}) want ({:.0f}, {:.0f}, {:.0f}, 255)", qx, qy, tx, ty, p[0], p[1], p[2], p[3],
                                  want[0] * 255.0f, want[1] * 255.0f, want[2] * 255.0f));
            ++probes;
        }
    }
    const std::uint8_t* corner = px.data();
    check(corner[0] == 0 && corner[1] == 0 && corner[2] == 0 && corner[3] == 255, "translated draw: background outside the quad");
    crossCheck(dev, "translated draw", toFloats(px.data(), px.size()), 2.0f);
    std::printf("[%s] translated draw: %s\n", rhi::backendName(dev.caps().backend), failures == before ? "PASS" : "FAIL");
}

// ---- test 2: ubershader draws ----------------------------------------------------------------------------------------------------------

// a vertex shader writing the material pixel shader's varyings (locations: TEXCOORD n = n, COLOR0 = 16)
const char* kUberVsAsm = R"(vs_3_0
dcl_position v0
dcl_texcoord0 v1
dcl_position o0
dcl_texcoord0 o1
dcl_texcoord1 o2
dcl_texcoord2 o3
dcl_texcoord3 o4
dcl_texcoord4 o5
dcl_texcoord5 o6
dcl_texcoord6 o7
dcl_texcoord7 o8
dcl_color0 o9
mov o0, v0
mov o1, v1.xyxy
mad o2, v1.yxyx, c1, c2
mov o3, c3
mad o4, v0.xyxy, c4, c5
mad o5, v1.xyxy, c6, c7
mov o6, c8
mov o7, c9
mov o8, v1.yxyx
mad o9, v1.xxyy, c10, c11
)";

shader::MacroList simpleRequest() {
    return {{"XFX_AMBIENTLIGHT", "true"},
            {"XFX_AMBIENTLIGHTTYPE", "1"},
            {"XFX_COMPRESSEDNORMALMAP", "true"},
            {"XFX_DIRLIGHT", "true"},
            {"XFX_FOG", "false"},
            {"XFX_LIGHTINGTYPE", "1"},
            {"XFX_PNTLIGHTCOUNT", "0"},
            {"XFX_PS_DIFFUSE", "(((tex2D( g_SamplerData[0], XFX_PS_TEX0 )) * (XFX_PS_VERTEXCOLOR)) * (g_ShaderData[0]))"},
            {"XFX_QUALITY", "2"},
            {"XFX_SHADERMODEL_3", "true"},
            {"XFX_SPECULAR", "true"},
            {"XFX_VS_TEX0", "XFX_VS_INPUT_TEX0"}};
}

// a captured request with a normal map, specular, a lightmap, a directional light and the complex ambient light: seven samplers
shader::MacroList heavyRequest() {
    const std::string tex1 = "(( (tex2D( g_SamplerData[1], XFX_PS_TEX1 )).g * g_ShaderData[0].xy + g_ShaderData[0].zw ) * XFX_PS_TANGENTEYE + XFX_PS_TEX1 )";
    const std::string diffuse = "saturate(((tex2D( g_SamplerData[0], " + tex1 + " )) * (g_ShaderData[1]).b) * ((tex2D( g_SamplerData[2], XFX_PS_TEX0 )) * (g_ShaderData[1]).r))";
    const std::string normal = "saturate(((tex2D( g_SamplerData[4], " + tex1 + " )) * (tex2D( g_SamplerData[5], XFX_PS_TEX0 ))) * (g_ShaderData[1]).r)";
    const std::string specular = "(((((tex2D( g_SamplerData[0], " + tex1 + " )) * (g_ShaderData[1]).b) * ((tex2D( g_SamplerData[2], XFX_PS_TEX0 )) * (g_ShaderData[1]).r)) * (tex2D( g_SamplerData[3], " + tex1 +
                                 " )).r) * (g_ShaderData[1]).g)";
    return {{"XFX_AMBIENTLIGHT", "true"},
            {"XFX_AMBIENTLIGHTTYPE", "2"},
            {"XFX_COMPRESSEDNORMALMAP", "true"},
            {"XFX_DIRLIGHT", "true"},
            {"XFX_LIGHTINGTYPE", "1"},
            {"XFX_LIGHTMAP", "true"},
            {"XFX_PNTLIGHTCOUNT", "0"},
            {"XFX_PS_DIFFUSE", diffuse},
            {"XFX_PS_NORMAL", normal},
            {"XFX_PS_SPECULAR", specular},
            {"XFX_PS_SPECULARPOWER", "(tex2D( g_SamplerData[3], " + tex1 + " )).r"},
            {"XFX_QUALITY", "2"},
            {"XFX_SHADERMODEL_3", "true"},
            {"XFX_SPECULAR", "true"},
            {"XFX_TANGENTSPACE", "1"},
            {"XFX_TANGENTSPACE_EYE", "1"},
            {"XFX_VS_TEX0", "( XFX_VS_INPUT_TEX0 * g_vShaderData[0].xy )"},
            {"XFX_VS_TEX1", "XFX_VS_INPUT_TEX0"}};
}

// the enhancement hooks of enh/material_variant.cpp for a G-buffer + GI variant with the vertex normal varying (fixed registers)
const char* kHookDefines =
    "#define RVK_MV_AMBIENT 1\n#define RVK_MV_AMB_REG 15\n#define RVK_MV_HAS_IND 1\n#define RVK_MV_IND_REG 14\n#define RVK_MV_GBUFFER 1\n#define RVK_MV_KIND_SKINNED 3\n"
    "#define RVK_MV_VNORMAL 1\n";

// RVK_PARITY_EXTRA="XFX_A=1;XFX_B=0" adds / overrides macros of the ubershader requests (bisecting a difference); RVK_PARITY_ONLY=<text> runs only the tests whose name contains it
shader::MacroList withExtra(shader::MacroList macros) {
    const char* extra = std::getenv("RVK_PARITY_EXTRA");
    if (extra == nullptr) return macros;
    const std::string e = extra;
    for (std::size_t pos = 0; pos < e.size();) {
        const std::size_t end = std::min(e.find(';', pos), e.size());
        const std::string item = e.substr(pos, end - pos);
        const std::size_t eq = item.find('=');
        if (eq != std::string::npos) {
            const std::string name = item.substr(0, eq), value = item.substr(eq + 1);
            bool found = false;
            for (auto& m : macros) {
                if (m.first == name) {
                    m.second = value;
                    found = true;
                }
            }
            if (!found) macros.emplace_back(name, value);
        }
        pos = end + 1;
    }
    return macros;
}
bool selected(const char* name) {
    const char* only = std::getenv("RVK_PARITY_ONLY");
    return only == nullptr || std::string(name).find(only) != std::string::npos;
}

void testUberDraw(Harness& h, const char* name, const shader::MacroList& macros, const char* entry, const std::string& hooks, bool gbuffer, bool heavy) {
    rhi::Device& dev = h.dev;
    const int before = failures;
    const std::string label = std::string("ubershader draw (") + name + ")";
    if (!selected(label.c_str())) return;
    if (!devtools::D3DX::get().available()) {
        std::printf("[%s] %s: SKIP (d3dx9_39.dll not installed)\n", rhi::backendName(dev.caps().backend), label.c_str());
        return;
    }
    Blobs vs, ps;
    shader::TranslateOptions none;
    const std::string psText = shader::uberPixelHlsl(entry, withExtra(macros), hooks);
    if (!translated(kUberVsAsm, true, none, vs, "parity_uber_vs") || !compileBoth("parity_uber.hlsl", psText, "PSMain", "ps_6_6", ps)) {
        check(false, label + ": shaders");
        return;
    }
    // textures for the engine's sampler slots: 0 shadow mask, 1 refraction, 2 depth map, 4 / 5 lightmaps, 9 .. 15 the material (g_SamplerData[0..6])
    abi::DrawBlock db{};
    const rhi::SamplerIndex linear = dev.createSampler({VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT});
    std::uint32_t seed = 1;
    for (std::uint32_t slot : {0u, 1u, 2u, 4u, 5u, 9u, 10u, 11u, 12u, 13u, 14u, 15u}) {
        if (!heavy && slot != 9u) continue;
        const std::uint32_t s = seed++;
        db.tex[slot] = h.addPattern(4, 4,
                                    [s](std::uint32_t x, std::uint32_t y) {
                                        // smooth gradients, a different phase per texture
                                        const float a = 0.5f + 0.45f * std::sin(0.9f * static_cast<float>(x) + 0.7f * static_cast<float>(s));
                                        const float b = 0.5f + 0.45f * std::cos(0.8f * static_cast<float>(y) + 0.5f * static_cast<float>(s));
                                        const float c = 0.5f + 0.4f * std::sin(0.5f * static_cast<float>(x + y) + static_cast<float>(s));
                                        return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(a * 255), static_cast<std::uint8_t>(b * 255), static_cast<std::uint8_t>(c * 255), 255};
                                    },
                                    "parity material texture");
        db.smp[slot] = linear;
        if (db.tex[slot] == 0) {
            check(false, label + ": texture");
            return;
        }
    }
    // constants: the vertex shader's registers (varyings) and a deterministic pixel register file in 0.15 .. 0.85
    StageBlock vsc, psc;
    vsc.set(1, 0.5f, 0.5f, 0.3f, 0.3f);
    vsc.set(2, 0.1f, 0.2f, 0.3f, 0.4f);
    vsc.set(3, 0.2f, 0.3f, 0.5f, 1.0f);
    vsc.set(4, 0.2f, 0.2f, 0.2f, 0.2f);
    vsc.set(5, 0.4f, 0.5f, 0.6f, 1.0f);
    vsc.set(6, 0.1f, 0.1f, 0.1f, 0.1f);
    vsc.set(7, 0.3f, 0.8f, 0.3f, 0.9f);
    vsc.set(8, 0.7f, 0.2f, 0.3f, 0.0f);
    vsc.set(9, 0.3f, 0.3f, 0.9f, 0.0f);
    vsc.set(10, 0.3f, 0.4f, 0.5f, 0.6f);
    vsc.set(11, 0.4f, 0.5f, 0.6f, 0.8f);
    for (std::uint32_t r = 0; r < 224; ++r) {
        const auto f = [r](std::uint32_t k) { return 0.15f + 0.7f * (0.5f + 0.5f * std::sin(0.37f * static_cast<float>(r) + 1.3f * static_cast<float>(k))); };
        psc.set(r, f(0), f(1), f(2), f(3));
    }
    psc.set(24, 0.3f, 0.8f, 0.5f, 0.0f);  // the directional light's direction
    abi::FrameBlock fb{};
    fb.time[0] = 0.5f;
    auto* bVs = h.makeBuffer(vsc.bytes.data(), vsc.bytes.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "parity vs consts");
    auto* bPs = h.makeBuffer(psc.bytes.data(), psc.bytes.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "parity ps consts");
    auto* bDraw = h.makeBuffer(&db, sizeof(db), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "parity draw block");
    auto* bFrame = h.makeBuffer(&fb, sizeof(fb), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "parity frame block");
    if (linear == 0 || !bVs || !bPs || !bDraw || !bFrame) {
        check(false, label + ": resources");
        return;
    }
    std::vector<Target> targets(gbuffer ? 3 : 1);
    if (gbuffer) {
        targets[1].format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        targets[2].format = VK_FORMAT_R16G16_SFLOAT;
    }
    if (!drawQuad(h, vs, ps, pushFor(*bVs, *bPs, *bDraw, *bFrame), targets, "parity uber")) {
        check(false, label + ": pipeline / target");
        return;
    }
    const std::vector<std::uint8_t> px = pixelsOf(targets[0]);
    // the picture must have something in it (not the clear colour) and vary
    std::uint32_t lit = 0, distinct = 0;
    for (std::size_t i = 0; i < px.size(); i += 4) {
        if (px[i] != 0 || px[i + 1] != 0 || px[i + 2] != 0) ++lit;
    }
    for (std::size_t i = 4; i < px.size(); i += 4) {
        if (std::memcmp(&px[i], &px[i - 4], 3) != 0) ++distinct;
    }
    const std::uint8_t* centre = px.data() + (static_cast<std::size_t>(kTarget / 2) * kTarget + kTarget / 2) * 4;
    check(lit > 200, std::format("{}: {} lit pixels of {} (centre pixel ({}, {}, {}, {}))", label, lit, kTarget * kTarget, centre[0], centre[1], centre[2], centre[3]));
    check(distinct > 200, std::format("{}: only {} pixel changes along the rows (a flat picture)", label, distinct));
    crossCheck(dev, label.c_str(), toFloats(px.data(), px.size()), 4.0f, 0.004f);
    if (gbuffer) {
        // target 1: A2B10G10R10 (the octahedral normal + id), compared as raw 10-bit fields; target 2: motion RG16F
        const auto* n = static_cast<const std::uint8_t*>(targets[1].readback->map());
        std::vector<float> normals;
        for (std::uint32_t y = 0; y < kTarget; ++y) {
            for (std::uint32_t x = 0; x < kTarget; ++x) {
                std::uint32_t w;
                std::memcpy(&w, n + static_cast<std::size_t>(y) * targets[1].pitch + x * 4, 4);
                normals.push_back(static_cast<float>(w & 1023u) / 4.0f);
                normals.push_back(static_cast<float>((w >> 10) & 1023u) / 4.0f);
                normals.push_back(static_cast<float>((w >> 30) & 3u) * 85.0f);
            }
        }
        {
            std::size_t nz = 0;
            for (float v : normals) nz += v != 0.0f;
            std::printf("  %s normals: %zu of %zu values non-zero, centre (%g, %g, %g)\n", label.c_str(), nz, normals.size(), normals[(kTarget / 2 * kTarget + kTarget / 2) * 3],
                        normals[(kTarget / 2 * kTarget + kTarget / 2) * 3 + 1], normals[(kTarget / 2 * kTarget + kTarget / 2) * 3 + 2]);
        }
        crossCheck(dev, (label + " normals").c_str(), normals, 6.0f, 0.01f);
        const auto* m = static_cast<const std::uint8_t*>(targets[2].readback->map());
        std::vector<float> motion;
        for (std::uint32_t y = 0; y < kTarget; ++y) {
            for (std::uint32_t x = 0; x < kTarget; ++x) {
                std::uint16_t hh[2];
                std::memcpy(hh, m + static_cast<std::size_t>(y) * targets[2].pitch + x * 4, 4);
                motion.push_back(halfToFloat(hh[0]) * 100.0f);
                motion.push_back(halfToFloat(hh[1]) * 100.0f);
            }
        }
        crossCheck(dev, (label + " motion").c_str(), motion, 0.5f, 0.01f);
    }
    std::printf("[%s] %s: %s\n", rhi::backendName(dev.caps().backend), label.c_str(), failures == before ? "PASS" : "FAIL");
}

#include "parity_water.inl"

// ---- test 3: references ---------------------------------------------------------------------------------------------------------------------

struct RtVertex {
    float pos[3];
    float normal[3];
    float uv0[2];
    std::int16_t uv1[2];
};
static_assert(sizeof(RtVertex) == 36);

void testReferences(Harness& h) {
    rhi::Device& dev = h.dev;
    const int before = failures;
    Blobs cs;
    if (!compileBoth("test/parity_refs.comp.hlsl", {}, "CSMain", "cs_6_6", cs, 3)) {
        check(false, "references: shader");
        return;
    }
    constexpr std::uint32_t kCount = 32;
    constexpr std::uint32_t kStrideWords = 64;
    constexpr VkBufferUsageFlags kSrv = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    // the GI volume: 4 x 4 x 4 probes, one cascade of spacing 100 at the origin
    std::vector<float> header(76, 0.0f);  // 304 bytes
    auto setU = [&](std::size_t word, std::uint32_t v) { std::memcpy(&header[word], &v, 4); };
    setU(0, 4);
    setU(1, 4);
    setU(2, 4);
    setU(3, 1);  // dims, cascades
    header[8] = 1.0f;
    header[9] = 0.2f;
    header[10] = 0.1f;
    header[11] = 1.0f;  // params: strength, normal bias, view bias, blend band
    setU(14, 64);       // info.z probes per cascade
    // cascade[0] at word 16: origin int4 = 0, spacing = (100, 0.01, 10, 0)
    header[20] = 100.0f;
    header[21] = 0.01f;
    header[22] = 10.0f;
    // probes: key = (cell, VALID | count << 8), regs
    struct Probe {
        std::int32_t key[4];
        float reg[7][4];
    };
    static_assert(sizeof(Probe) == 128);
    std::vector<Probe> probes(64);
    std::vector<float> moments(64 * 64 * 2);
    for (int cy = 0; cy < 4; ++cy) {
        for (int cz = 0; cz < 4; ++cz) {
            for (int cx = 0; cx < 4; ++cx) {
                const int index = (cy * 4 + cz) * 4 + cx;  // rvkGiSlot
                Probe& p = probes[static_cast<std::size_t>(index)];
                p.key[0] = cx;
                p.key[1] = cy;
                p.key[2] = cz;
                p.key[3] = 1 | (5 << 8);
                for (int k = 0; k < 7; ++k) {
                    p.reg[k][0] = 0.1f * static_cast<float>(k) + 0.01f * static_cast<float>(cx);
                    p.reg[k][1] = 0.02f * static_cast<float>(cy) + 0.05f * static_cast<float>(k);
                    p.reg[k][2] = 0.03f * static_cast<float>(cz) + 0.1f;
                    p.reg[k][3] = 0.5f + 0.01f * static_cast<float>(cx + cy + cz + k);
                }
                for (int t = 0; t < 64; ++t) {
                    const float mean = 2.0f + 0.1f * static_cast<float>(t % 7) + 0.05f * static_cast<float>(cx);
                    moments[(static_cast<std::size_t>(index) * 64 + static_cast<std::size_t>(t)) * 2] = mean;
                    moments[(static_cast<std::size_t>(index) * 64 + static_cast<std::size_t>(t)) * 2 + 1] = mean * mean + 0.5f;
                }
            }
        }
    }
    auto* bProbes = h.makeBuffer(probes.data(), sizeof(Probe) * probes.size(), kSrv, "parity probes");
    auto* bMoments = h.makeBuffer(moments.data(), moments.size() * 4, kSrv, "parity moments");
    if (!bProbes || !bMoments) {
        check(false, "references: buffers");
        return;
    }
    const std::uint64_t probesRef = refOf(*bProbes), momentsRef = refOf(*bMoments);
    setU(4, static_cast<std::uint32_t>(probesRef));
    setU(5, static_cast<std::uint32_t>(probesRef >> 32));
    setU(6, static_cast<std::uint32_t>(momentsRef));
    setU(7, static_cast<std::uint32_t>(momentsRef >> 32));
    auto* bHeader = h.makeBuffer(header.data(), header.size() * 4, kSrv, "parity gi header");

    // the RT scene's records: record 0 with 16-bit indices at a 2-byte offset, record 1 with 32-bit indices and two material segments
    RtVertex verts[6];
    for (int j = 0; j < 6; ++j) {
        const float fj = static_cast<float>(j);
        verts[j] = {{fj * 1.5f, fj * 0.5f + 1.0f, 2.0f - fj * 0.25f}, {0.1f * fj, 1.0f, 0.2f}, {fj * 0.1f, 1.0f - fj * 0.05f}, {static_cast<std::int16_t>(j * 1000), static_cast<std::int16_t>(-j * 2000)}};
    }
    const std::uint16_t idx16[1 + 9] = {0xFFFF, 0, 1, 2, 1, 2, 3, 2, 3, 4};  // the first element is padding: the record's indices start 2 bytes in
    const std::uint32_t idx32[1 + 9] = {0xFFFFFFFFu, 3, 4, 5, 0, 2, 4, 1, 3, 5};
    auto* bVerts = h.makeBuffer(verts, sizeof(verts), kSrv, "parity vertices");
    auto* bIdx16 = h.makeBuffer(idx16, sizeof(idx16), kSrv, "parity indices 16");
    auto* bIdx32 = h.makeBuffer(idx32, sizeof(idx32), kSrv, "parity indices 32");
    struct Segment {
        std::uint32_t w0[4], w1[4];
        float uvMap0[4], uvMap1[4];
    };
    static_assert(sizeof(Segment) == 64);
    const Segment segs[2] = {{{0, 2, 21, 4}, {0x0105, 1, 0, 0}, {0.5f, 0, 0.1f, 0}, {0, 0.5f, 0.2f, 0}}, {{2, 2, 22, 5}, {0x0207, 0, 0, 0}, {0.25f, 0, 0.3f, 0}, {0, 0.25f, 0.4f, 0}}};
    auto* bSegs = h.makeBuffer(segs, sizeof(segs), kSrv, "parity segments");
    if (!bHeader || !bVerts || !bIdx16 || !bIdx32 || !bSegs) {
        check(false, "references: buffers");
        return;
    }
    struct Record {
        std::uint32_t w0[4], w1[4], w2[4], w3[4];
        float uvMap0[4], uvMap1[4];
        std::uint32_t w6[4];
    };
    static_assert(sizeof(Record) == 112);
    constexpr std::uint32_t kFloat3 = 2u, kFloat2 = 1u, kShort2N = 9u;
    const auto lo = [](std::uint64_t v) { return static_cast<std::uint32_t>(v); };
    const auto hi = [](std::uint64_t v) { return static_cast<std::uint32_t>(v >> 32); };
    const std::uint64_t vRef = refOf(*bVerts), i16Ref = refOf(*bIdx16, 2), i32Ref = refOf(*bIdx32, 4), sRef = refOf(*bSegs);
    Record recs[2] = {};
    for (int r = 0; r < 2; ++r) {
        Record& rec = recs[r];
        rec.w0[0] = lo(vRef);
        rec.w0[1] = hi(vRef);
        const std::uint64_t ir = r == 0 ? i16Ref : i32Ref;
        rec.w0[2] = lo(ir);
        rec.w0[3] = hi(ir);
        rec.w1[0] = 36;                                       // stride
        rec.w1[1] = r == 0 ? 0u : (1u | 64u);                 // flags: INDEX32 | SEGMENTED
        rec.w1[2] = 12u | (kFloat3 << 16);                    // normal
        rec.w1[3] = 24u | (kFloat2 << 16);                    // texcoord0
        rec.w2[0] = 32u | (kShort2N << 16);                   // texcoord1
        rec.w2[1] = 0xFFFFFFFFu;                              // texcoord6: absent
        rec.w2[3] = static_cast<std::uint32_t>(100 + r);      // instanceId
        rec.w3[0] = 7;                                        // albedoTex
        rec.w3[1] = 3;                                        // albedoSampler
        rec.w3[2] = 0x0105;                                   // alpha
        rec.w3[3] = 0;                                        // uvSet
        rec.uvMap0[0] = 1.0f;
        rec.uvMap0[2] = 0.25f;
        rec.uvMap1[1] = 1.0f;
        rec.uvMap1[2] = 0.5f;
        if (r == 1) {
            rec.w6[0] = lo(sRef);
            rec.w6[1] = hi(sRef);
            rec.w6[2] = 2;
        }
    }
    auto* bRecs = h.makeBuffer(recs, sizeof(recs), kSrv, "parity records");

    abi::FrameBlock fb{};
    const std::uint64_t headerRef = bHeader ? refOf(*bHeader) : 0, recsRef = bRecs ? refOf(*bRecs) : 0;
    fb.gi[0] = lo(headerRef);
    fb.gi[1] = hi(headerRef);
    fb.gi[2] = abi::kFrameGiValid;
    fb.rtScene[0] = 1;  // (a non-zero TLAS address: rvkRtValid is not used by the test)
    fb.rtScene[2] = lo(recsRef);
    fb.rtScene[3] = hi(recsRef);
    fb.rtInfo[2] = abi::kFrameRtValid;
    auto* bFrame = h.makeBuffer(&fb, sizeof(fb), kSrv, "parity frame block");
    constexpr VkBufferUsageFlags kRw = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto out = dev.createBuffer({kCount * kStrideWords * 4, kRw, rhi::Memory::Device, "parity results"});
    auto readback = dev.createBuffer({kCount * kStrideWords * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "parity results readback"});
    auto pipeline = dev.createComputePipeline({cs.shader(), "parity refs"});
    if (!bRecs || !bFrame || !out || !readback || !pipeline) {
        check(false, "references: resources / pipeline");
        return;
    }
    struct Push {
        std::uint32_t frame[2], passId, passFlags, outBuf[2], count, pad;
    } push{};
    const auto fr = bFrame->shaderRef(0), ro = out->shaderRefRw(0);
    push.frame[0] = fr[0];
    push.frame[1] = fr[1];
    push.outBuf[0] = ro[0];
    push.outBuf[1] = ro[1];
    push.count = kCount;
    dev.submitAndWait([&](rhi::CommandList& cl) {
        cl.bufferBarrier(*out, rhi::BufferAccess::ShaderWrite);
        cl.bindPipeline(*pipeline);
        cl.setPushConstants(&push, sizeof(push));
        cl.dispatch(1, 1, 1);
        cl.bufferBarrier(*out);
        cl.copyBuffer(*readback, 0, *out, 0, kCount * kStrideWords * 4);
    });
    readback->invalidate();
    const auto* w = static_cast<const std::uint32_t*>(readback->map());
    const auto F = [&](std::uint32_t i, std::uint32_t k) {
        float f;
        std::memcpy(&f, &w[i * kStrideWords + k], 4);
        return f;
    };
    // CPU expectations of the RT part (words 30 ..): the interpolated position / normal / uv, the material segment, the triangle
    int bad = 0;
    std::string first;
    const auto flag = [&](bool ok, const std::string& what) {
        if (!ok && ++bad == 1) first = what;
    };
    for (std::uint32_t i = 0; i < kCount; ++i) {
        const std::uint32_t r = i & 1u, prim = i % 3u;
        const std::uint32_t tri16[3][3] = {{0, 1, 2}, {1, 2, 3}, {2, 3, 4}}, tri32[3][3] = {{3, 4, 5}, {0, 2, 4}, {1, 3, 5}};
        const std::uint32_t* t = r == 0 ? tri16[prim] : tri32[prim];
        const float b1 = 0.2f + 0.05f * static_cast<float>(i & 3u), b2 = 0.3f;
        const float wt[3] = {1.0f - b1 - b2, b1, b2};
        float pos[3] = {}, nrm[3] = {}, uv0[2] = {}, uv1[2] = {};
        for (int k = 0; k < 3; ++k) {
            const RtVertex& v = verts[t[k]];
            for (int c = 0; c < 3; ++c) {
                pos[c] += v.pos[c] * wt[k];
                nrm[c] += v.normal[c] * wt[k];
            }
            for (int c = 0; c < 2; ++c) {
                uv0[c] += v.uv0[c] * wt[k];
                uv1[c] += std::clamp(static_cast<float>(v.uv1[c]) / 32767.0f, -1.0f, 1.0f) * wt[k];
            }
        }
        const float tol = 1e-4f;
        for (int c = 0; c < 3; ++c) {
            flag(std::fabs(F(i, 30 + c) - pos[c]) < tol, std::format("invocation {}: position[{}] {} want {}", i, c, F(i, 30 + c), pos[c]));
            flag(std::fabs(F(i, 33 + c) - nrm[c]) < tol, std::format("invocation {}: normal[{}] {} want {}", i, c, F(i, 33 + c), nrm[c]));
        }
        for (int c = 0; c < 2; ++c) {
            flag(std::fabs(F(i, 36 + c) - uv0[c]) < tol, std::format("invocation {}: uv0[{}] {} want {}", i, c, F(i, 36 + c), uv0[c]));
            flag(std::fabs(F(i, 38 + c) - uv1[c]) < tol, std::format("invocation {}: uv1[{}] {} want {}", i, c, F(i, 38 + c), uv1[c]));
            flag(F(i, 40 + c) == 0.0f, std::format("invocation {}: the absent lightmap uv is {}", i, F(i, 40 + c)));
        }
        const std::uint32_t wantTex = r == 0 ? 7u : (prim < 2 ? 21u : 22u);
        flag(w[i * kStrideWords + 45] == wantTex, std::format("invocation {}: albedoTex {} want {}", i, w[i * kStrideWords + 45], wantTex));
        flag(w[i * kStrideWords + 57] == t[0] && w[i * kStrideWords + 58] == t[1] && w[i * kStrideWords + 59] == t[2],
             std::format("invocation {}: triangle ({}, {}, {}) want ({}, {}, {})", i, w[i * kStrideWords + 57], w[i * kStrideWords + 58], w[i * kStrideWords + 59], t[0], t[1], t[2]));
        flag(w[i * kStrideWords + 0] == 1u, std::format("invocation {}: the GI lookup found no probe (hit {})", i, w[i * kStrideWords + 0]));
        flag(w[i * kStrideWords + 1] == 0u, std::format("invocation {}: GI cascade {}", i, w[i * kStrideWords + 1]));
    }
    check(bad == 0, std::format("references: {} CPU mismatch(es), first: {}", bad, first));
    std::vector<float> all(kCount * kStrideWords);
    for (std::uint32_t i = 0; i < all.size(); ++i) {
        // integers as floats of their value (floats stay floats): the words that hold ids / flags are compared exactly as numbers
        const std::uint32_t k = i % kStrideWords;
        const bool isInt = k == 0 || k == 1 || (k >= 45 && k <= 48) || (k >= 57 && k <= 60);
        all[i] = isInt ? static_cast<float>(w[i]) : F(i / kStrideWords, k);
    }
    crossCheck(dev, "references", all, 2e-4f);
    std::printf("[%s] references (GI lookup, RT records): %s\n", rhi::backendName(dev.caps().backend), failures == before ? "PASS" : "FAIL");
}

// ---- test 5: a pipeline of every embedded shader ----------------------------------------------------------------------------------------------

// Every embedded compute shader becomes a compute pipeline and every vertex / pixel pair a graphics pipeline on this backend: DXC's validation says the DXIL is sound, this says the driver
// accepts it with the fixed root signature (D3D12) / the pass layout (Vulkan) and the debug layers have nothing to say (the checks of main.cpp count their errors at the end).
void testPipelines(rhi::Device& dev) {
    const int before = failures;
    const char* label = "pipelines of every embedded shader";
    if (!selected(label)) return;
    int compute = 0, graphics = 0;
    const auto computeOf = [&](const std::string& name, const char* defines) {
        const auto spv = shader::embedded::spirvVariant(name, defines);
        const auto dxil = shader::embedded::dxilVariant(name, defines);
        if (spv.empty() || dxil.empty()) {
            check(false, std::format("{}: compile failed", name));
            return;
        }
        rhi::ComputePipelineDesc cd;
        cd.cs = {spv, dxil};
        cd.name = name.c_str();
        auto p = dev.createComputePipeline(cd);
        check(p != nullptr, std::format("compute pipeline of {}", name));
        compute += p != nullptr ? 1 : 0;
    };
    for (const auto& f : shader::embedded::hlslSources()) {
        std::string name(f.name);
        if (!name.ends_with(".comp.hlsl")) continue;
        name.resize(name.size() - 5);
        if (name == "test/rt_query.comp") continue;  // (the Vulkan ray query self-test: its own descriptor layout)
        if (name == "water/ocean_fft.comp") {
            for (int n = 128; n <= 512; n *= 2) {
                int l = 0;
                while ((1 << l) < n) ++l;
                computeOf(name, std::format("OCEAN_N={};OCEAN_LOG2N={}", n, l).c_str());
            }
        } else {
            computeOf(name, "");
        }
    }
    // vertex + pixel pairs the renderer builds (fullscreen passes, the water, the shadow mask)
    struct Pair {
        const char* vs;
        const char* ps;
        VkFormat format;
    };
    const Pair pairs[] = {{"debug/fullscreen_rhi.vert", "debug/inject_view.frag", VK_FORMAT_R16G16B16A16_SFLOAT},
                          {"debug/fullscreen_rhi.vert", "framegen/debug.frag", VK_FORMAT_R8G8B8A8_UNORM},
                          {"debug/fullscreen_rhi.vert", "gbuffer/gbuffer_debug.frag", VK_FORMAT_R16G16B16A16_SFLOAT},
                          {"debug/fullscreen_rhi.vert", "gbuffer/ao_white.frag", VK_FORMAT_R8G8B8A8_UNORM},
                          {"debug/fullscreen_rhi.vert", "rt/rt_debug.frag", VK_FORMAT_R16G16B16A16_SFLOAT},
                          {"debug/fullscreen_rhi.vert", "water/wet_shore.frag", VK_FORMAT_R16G16B16A16_SFLOAT},
                          {"water/water_ocean.vert", "water/water.frag", VK_FORMAT_R16G16B16A16_SFLOAT},
                          {"shadows/shadow_mask.vert", "shadows/shadow_mask.frag", VK_FORMAT_R8G8B8A8_UNORM}};
    for (const Pair& p : pairs) {
        const auto vs = shader::embedded::spirv(p.vs), ps = shader::embedded::spirv(p.ps);
        const auto vd = shader::embedded::dxil(p.vs), pd = shader::embedded::dxil(p.ps);
        if (vs.empty() || ps.empty() || vd.empty() || pd.empty()) {
            check(false, std::format("{} + {}: compile failed", p.vs, p.ps));
            continue;
        }
        rhi::GraphicsPipelineDesc gd;
        gd.vs = {vs, vd};
        gd.ps = {ps, pd};
        gd.colorFormats[0] = p.format;
        gd.name = p.ps;
        auto pipeline = dev.createGraphicsPipeline(gd);
        check(pipeline != nullptr, std::format("graphics pipeline of {} + {}", p.vs, p.ps));
        graphics += pipeline != nullptr ? 1 : 0;
    }
    std::printf("  %d compute + %d graphics pipelines created\n", compute, graphics);
    std::printf("[%s] %s: %s\n", rhi::backendName(dev.caps().backend), label, failures == before ? "PASS" : "FAIL");
}

// ---- test 6: the ocean's mip chain by compute (ocean_mips.comp) against a CPU 2 x 2 box chain --------------------------------------------------

float halfToF(std::uint16_t h) {
    const std::uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
    float v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : std::ldexp(static_cast<float>(m | 1024u), static_cast<int>(e) - 25);
    return s ? -v : v;
}

std::uint16_t floatToHalf(float f) {  // round to nearest even; |f| < 65504, no NaN / Inf inputs
    const std::uint32_t s = f < 0 ? 0x8000u : 0u;
    f = std::fabs(f);
    if (f < 6.103515625e-05f) return static_cast<std::uint16_t>(s | static_cast<std::uint32_t>(std::lrintf(f * 16777216.0f)));
    int e;
    const float fr = std::frexp(f, &e);  // f = fr * 2^e, fr in [0.5, 1)
    const long m = std::lrintf(std::ldexp(fr, 11));  // 1024..2048
    int ex = e + 14;
    long mm = m;
    if (mm == 2048) {
        mm = 1024;
        ++ex;
    }
    return static_cast<std::uint16_t>(s | (static_cast<std::uint32_t>(ex) << 10) | static_cast<std::uint32_t>(mm - 1024));
}

void testOceanMips(Harness& h) {
    rhi::Device& dev = h.dev;
    const int before = failures;
    const char* label = "ocean mip chain by compute";
    if (!selected(label)) return;
    Blobs cs;
    if (!compileBoth("water/ocean_mips.comp.hlsl", "", "CSMain", "cs_6_6", cs)) {
        check(false, "ocean_mips: compile");
        return;
    }
    // the embedded text is picked by the file name when `source` is empty (as the other embedded compiles of this file)
    for (std::uint32_t n : {128u, 512u}) {
        std::uint32_t mips = 1;
        while ((1u << (mips - 1)) < n) ++mips;
        rhi::TextureDesc td{VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, {n, n, 1}, mips, 1,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "ocean mips test"};
        auto tex = dev.createTexture(td);
        const std::uint32_t pitch0 = rhi::bufferRowPitch(td.format, n);
        auto staging = dev.createBuffer({static_cast<std::uint64_t>(pitch0) * n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Upload, "ocean mips staging"});
        std::uint64_t total = 0;
        std::vector<std::uint64_t> offs(mips);
        for (std::uint32_t m = 0; m < mips; ++m) {
            offs[m] = total;
            total += (rhi::copyBytes(td, m) + 511) & ~511ull;
        }
        auto readback = dev.createBuffer({total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "ocean mips readback"});
        auto pipeline = dev.createComputePipeline({cs.shader(), "ocean mips"});
        if (!tex || !staging || !readback || !pipeline) {
            check(false, "ocean_mips: resources / pipeline");
            return;
        }
        // mip 0: a smooth field + noise + a foam-like channel (values the ocean writes: metres, slopes, 0..1), as halves
        std::vector<float> level(static_cast<std::size_t>(n) * n * 4);
        std::uint32_t seed = 12345u;
        for (std::uint32_t y = 0; y < n; ++y) {
            for (std::uint32_t x = 0; x < n; ++x) {
                for (int c = 0; c < 4; ++c) {
                    seed = seed * 1664525u + 1013904223u;
                    const float noise = static_cast<float>(seed >> 8) / 16777216.0f - 0.5f;
                    const float v = (c == 3 ? 0.5f : 0.0f) + std::sin(0.05f * static_cast<float>(x + 3 * c)) * std::cos(0.07f * static_cast<float>(y)) + 0.6f * noise;
                    const std::uint16_t hv = floatToHalf(v);
                    std::memcpy(static_cast<std::uint8_t*>(staging->map()) + static_cast<std::size_t>(y) * pitch0 + (x * 4 + c) * 2, &hv, 2);
                    level[(static_cast<std::size_t>(y) * n + x) * 4 + c] = halfToF(hv);
                }
            }
        }
        struct Push {
            std::uint64_t frame;
            std::uint32_t passId, passFlags, info[4];
        };
        dev.submitAndWait([&](rhi::CommandList& cl) {
            cl.transition(*tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            cl.copyBufferToTexture(*tex, *staging, 0, {0, 0});
            cl.transition(*tex, VK_IMAGE_LAYOUT_GENERAL);
            cl.bindPipeline(*pipeline);
            for (int stage = 0; stage < 2; ++stage) {
                const std::uint32_t first = stage == 0 ? 0u : 6u, levels = stage == 0 ? 6u : mips - 7u;
                if (levels == 0) continue;
                Push p{};
                p.info[0] = n >> first;
                p.info[1] = levels;
                rhi::StorageView views[7];
                for (std::uint32_t i = 0; i < 7; ++i) views[i] = {tex.get(), std::min(first + i, mips - 1u), 0};
                cl.setStorageViews(views);
                cl.setPushConstants(&p, sizeof(p));
                const std::uint32_t groups = stage == 0 ? n / 64u : 1u;
                cl.dispatch(groups, groups, 1);
                cl.computeBarrier();
            }
            cl.transition(*tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            for (std::uint32_t m = 1; m < mips; ++m) cl.copyTextureToBuffer(*readback, offs[m], *tex, {m, 0});
        });
        readback->invalidate();
        const auto* base = static_cast<const std::uint8_t*>(readback->map());
        // CPU chain: the average of 2 x 2 of the previous level in float, rounded to half only at the end (the shader's groups keep float inside)
        std::vector<float> all;
        float worst = 0.0f;
        std::uint32_t size = n;
        for (std::uint32_t m = 1; m < mips; ++m) {
            const std::uint32_t o = size / 2, pitch = rhi::bufferRowPitch(td.format, o);
            std::vector<float> next(static_cast<std::size_t>(o) * o * 4);
            for (std::uint32_t y = 0; y < o; ++y) {
                for (std::uint32_t x = 0; x < o; ++x) {
                    for (int c = 0; c < 4; ++c) {
                        const auto at = [&](std::uint32_t xx, std::uint32_t yy) { return level[(static_cast<std::size_t>(yy) * size + xx) * 4 + c]; };
                        const float want = 0.25f * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1));
                        next[(static_cast<std::size_t>(y) * o + x) * 4 + c] = want;
                        std::uint16_t hv;
                        std::memcpy(&hv, base + offs[m] + static_cast<std::size_t>(y) * pitch + (x * 4 + c) * 2, 2);
                        const float got = halfToF(hv);
                        worst = std::max(worst, std::fabs(got - want));
                        all.push_back(got);
                    }
                }
            }
            level = std::move(next);
            size = o;
        }
        check(worst < 2e-3f, std::format("ocean_mips {}: worst difference to the CPU chain {}", n, worst));
        std::printf("  ocean mips %u (%u levels): worst difference to the CPU chain %g\n", n, mips - 1, worst);
        crossCheck(dev, n == 128 ? "ocean mips 128" : "ocean mips 512", all, 1e-3f);
    }
    std::printf("[%s] %s: %s\n", rhi::backendName(dev.caps().backend), label, failures == before ? "PASS" : "FAIL");
}

}  // namespace

int run(rhi::Device& dev) {
    failures = 0;
    Harness h(dev);
    testPipelines(dev);
    testOceanMips(h);
    testTranslatedDraw(h);
    testUberDraw(h, "diffuse * vertex colour", simpleRequest(), "PS_Default", {}, false, false);
    testUberDraw(h, "normal map, specular, lightmap, lights", heavyRequest(), "PS_Default", {}, false, true);
    testUberDraw(h, "gbuffer + gi hooks", heavyRequest(), "PS_Default", kHookDefines, true, true);
    testWaterDraw(h);
    testReferences(h);
    return failures;
}

}  // namespace rvk::parity
