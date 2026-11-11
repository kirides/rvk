// rvk_d3d9test: checks d3d9::Device against Direct3D 9 semantics, off-screen with validation on.
//
//   rvk_d3d9test [--validation | --no-validation] [--backend vulkan|d3d12] [--keep-images] [--present-test [0]] [--only <name part>[,...]]
// --backend d3d12 runs the same cases on the D3D12 game device (docs/research/dx12-game-port.md WP3 / WP4).
//
// Every case renders into the 64x64 back buffer with D3D9-style calls (states, streams, D3DX-assembled shaders), reads the
// pixels back (readBackBuffer) and compares them with what D3D9 produces. Exit code 0 = all pass and no validation errors.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "common/d3dx.h"
#include "d3d9/convert.h"
#include "d3d9/device.h"
#include "d3d9/fvf.h"
#include "enh/material_variant.h"
#include "d3d9/particle_gpu.h"
#include "native/static_compose.h"
#include "enh/rt_scene.h"
#include "gpuscene/bone_table.h"
#include "gpuscene/row_table.h"
#include "engine/scene_table.h"
#include "gpuscene/cluster_table.h"
#include "gpuscene/object_table.h"
#include "gpuscene/draw_record_table.h"
#include "gpuscene/owned_compact.h"
#include "gpuscene/shutdown.h"
#include "gpuscene/view_table.h"
#include "native/tree_rows.h"
#include "rhi/d3d_formats.h"

using namespace rvk;
using namespace rvk::d3d9;

namespace {

constexpr std::uint32_t kSize = 64;
bool g_keepImages = false;
static rhi::Backend g_backend = rhi::Backend::Vulkan;  // --backend
int g_failures = 0;

// counts error-level log lines (the device teardown: validation messages are logged as errors)
struct ErrorCountSink final : spdlog::sinks::base_sink<std::mutex> {
    std::atomic<std::uint32_t> errors{0};

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        if (msg.level >= spdlog::level::err) ++errors;
    }
    void flush_() override {}
};

struct Vertex {
    float x, y, z, w;
    std::uint32_t color;  // D3DCOLOR
    float u, v;
};

const VertexElement kDecl[] = {
    {0, 0, DECLTYPE_FLOAT4, 0, 0 /*POSITION*/, 0},
    {0, 16, DECLTYPE_D3DCOLOR, 0, 10 /*COLOR*/, 0},
    {0, 20, DECLTYPE_FLOAT2, 0, 5 /*TEXCOORD*/, 0},
    {kDeclEndStream, 0, DECLTYPE_UNUSED, 0, 0, 0},
};

// instancing: stream 1 holds a per-instance offset (TEXCOORD1)
const VertexElement kInstDecl[] = {
    {0, 0, DECLTYPE_FLOAT4, 0, 0, 0},
    {0, 16, DECLTYPE_D3DCOLOR, 0, 10, 0},
    {0, 20, DECLTYPE_FLOAT2, 0, 5, 0},
    {1, 0, DECLTYPE_FLOAT4, 0, 5, 1},
    {kDeclEndStream, 0, DECLTYPE_UNUSED, 0, 0, 0},
};

const char* kVs = R"(
vs_3_0
dcl_position v0
dcl_color v1
dcl_texcoord v2
dcl_position o0
dcl_color o1
dcl_texcoord o2
mov o0, v0
mov o1, v1
mov o2, v2
)";

const char* kVsInstanced = R"(
vs_3_0
dcl_position v0
dcl_color v1
dcl_texcoord v2
dcl_texcoord1 v3
dcl_position o0
dcl_color o1
dcl_texcoord o2
add o0, v0, v3
mov o1, v1
mov o2, v2
)";

const char* kPsColor = R"(
ps_3_0
dcl_color v0
mov oC0, v0
)";

const char* kPsTexture = R"(
ps_3_0
dcl_texcoord v0.xy
dcl_2d s0
texld oC0, v0, s0
)";

std::shared_ptr<Shader> shaderFrom(Device& dev, const char* src) {
    std::string err;
    const auto tokens = devtools::D3DX::get().assemble(src, &err);
    if (tokens.empty()) {
        std::printf("assemble failed: %s\n", err.c_str());
        return nullptr;
    }
    auto sh = dev.createShader(tokens);
    if (sh) sh->wait();  // (the module is compiled on the shader workers; a draw whose shader has none yet is skipped)
    return sh;
}

// a quad in D3D9 clip space covering [x0,x1] x [y0,y1] (y up), as a triangle list in clockwise screen order
std::vector<Vertex> quad(float x0, float y0, float x1, float y1, float z, std::uint32_t color) {
    return {{x0, y1, z, 1, color, 0, 0}, {x1, y1, z, 1, color, 1, 0}, {x0, y0, z, 1, color, 0, 1},
            {x1, y1, z, 1, color, 1, 0}, {x1, y0, z, 1, color, 1, 1}, {x0, y0, z, 1, color, 0, 1}};
}

struct Pixels {
    std::vector<std::uint8_t> bgra;
    std::uint32_t w = 0, h = 0;
    std::uint32_t at(std::uint32_t x, std::uint32_t y) const {
        const std::uint8_t* p = bgra.data() + (static_cast<std::size_t>(y) * w + x) * 4;
        return (static_cast<std::uint32_t>(p[3]) << 24) | (static_cast<std::uint32_t>(p[2]) << 16) |
               (static_cast<std::uint32_t>(p[1]) << 8) | p[0];  // 0xAARRGGBB
    }
};

Pixels capture(Device& dev, const char* name) {
    Pixels p;
    dev.readBackBuffer(p.bgra, p.w, p.h);
    if (g_keepImages) {
        std::vector<std::uint8_t> rgba(p.bgra.size());
        for (std::size_t i = 0; i + 3 < p.bgra.size(); i += 4) {
            rgba[i] = p.bgra[i + 2];
            rgba[i + 1] = p.bgra[i + 1];
            rgba[i + 2] = p.bgra[i];
            rgba[i + 3] = 255;
        }
        stbi_write_png((std::string("d3d9test_") + name + ".png").c_str(), static_cast<int>(p.w), static_cast<int>(p.h), 4, rgba.data(),
                       static_cast<int>(p.w * 4));
    }
    return p;
}

bool similar(std::uint32_t a, std::uint32_t b, int tol = 2) {
    for (int s = 0; s < 24; s += 8) {  // RGB only (the back buffer is X8R8G8B8)
        if (std::abs(static_cast<int>((a >> s) & 0xFF) - static_cast<int>((b >> s) & 0xFF)) > tol) {
            return false;
        }
    }
    return true;
}

void expect(const char* test, const Pixels& p, std::uint32_t x, std::uint32_t y, std::uint32_t want, int tol = 2) {
    const std::uint32_t got = p.at(x, y);
    if (!similar(got, want, tol)) {
        std::printf("  FAIL %s: pixel (%u,%u) = %06X, expected %06X\n", test, x, y, got & 0xFFFFFF, want & 0xFFFFFF);
        ++g_failures;
    }
}

// the common state of every case: back buffer + depth, default states, quad shaders
struct Fixture {
    Device& dev;
    std::shared_ptr<Shader> vs, vsInst, psColor, psTexture;
    std::unique_ptr<VertexDecl> decl, instDecl;
    std::unique_ptr<Buffer> vb;

    void reset() {
        dev.setRenderTarget(0, dev.backBuffer());
        dev.setDepthStencil(dev.autoDepthStencil());
        for (std::uint32_t rs = 0; rs < RS_COUNT; ++rs) {
            dev.setRenderState(rs, defaultRenderState(rs));
        }
        dev.setRenderState(RS_CULLMODE, CULL_NONE);
        for (std::uint32_t s = 0; s < 16; ++s) {
            dev.setTexture(s, nullptr);
        }
        dev.setVertexDecl(decl.get());
        dev.setVertexShader(vs);
        dev.setPixelShader(psColor);
        dev.setStreamSourceFreq(0, 1);
        dev.setStreamSourceFreq(1, 1);
        dev.clear(CLEAR_TARGET | CLEAR_ZBUFFER | CLEAR_STENCIL, 0xFF000000, 1.0f, 0);
    }

    void draw(const std::vector<Vertex>& v) {
        void* p = vb->lock(0, 0, LOCK_DISCARD);
        std::memcpy(p, v.data(), v.size() * sizeof(Vertex));
        vb->unlock();
        dev.setStreamSource(0, vb.get(), 0, sizeof(Vertex));
        dev.drawPrimitive(PT_TRIANGLELIST, 0, static_cast<std::uint32_t>(v.size() / 3));
    }
};

void testClear(Fixture& f) {
    f.reset();
    f.dev.clear(CLEAR_TARGET, 0xFF336699, 1.0f, 0);
    const Pixels p = capture(f.dev, "clear");
    expect("clear", p, 0, 0, 0x336699);
    expect("clear", p, 63, 63, 0x336699);
}

void testRasterRules(Fixture& f) {
    // a quad over the left half of the target: D3D9 covers pixel columns 0..31 exactly (pixel centres on integers)
    f.reset();
    f.draw(quad(-1, -1, 0, 1, 0.5f, 0xFFFF0000));
    const Pixels p = capture(f.dev, "raster");
    expect("raster", p, 31, 10, 0xFF0000);
    expect("raster", p, 32, 10, 0x000000);
    expect("raster", p, 0, 0, 0xFF0000);
    expect("raster", p, 0, 63, 0xFF0000);
}

void testCull(Fixture& f) {
    // quad() is clockwise on screen; D3D9's default CULL_CCW keeps it, CULL_CW removes it
    f.reset();
    f.dev.setRenderState(RS_CULLMODE, CULL_CCW);
    f.draw(quad(-1, -1, 0, 1, 0.5f, 0xFF00FF00));
    f.dev.setRenderState(RS_CULLMODE, CULL_CW);
    f.draw(quad(0, -1, 1, 1, 0.5f, 0xFF00FF00));
    const Pixels p = capture(f.dev, "cull");
    expect("cull (CCW culling keeps clockwise)", p, 10, 10, 0x00FF00);
    expect("cull (CW culling removes clockwise)", p, 50, 10, 0x000000);
}

void testDepth(Fixture& f) {
    f.reset();
    f.draw(quad(-1, -1, 1, 1, 0.3f, 0xFF0000FF));  // near
    f.draw(quad(-1, -1, 1, 1, 0.6f, 0xFFFF0000));  // far: rejected by LESSEQUAL
    f.dev.setRenderState(RS_ZENABLE, ZB_FALSE);
    f.draw(quad(0, -1, 1, 1, 0.9f, 0xFF00FF00));  // depth off: drawn over
    const Pixels p = capture(f.dev, "depth");
    expect("depth test", p, 10, 10, 0x0000FF);
    expect("depth disabled", p, 50, 10, 0x00FF00);
}

void testBlend(Fixture& f) {
    f.reset();
    f.draw(quad(-1, -1, 1, 1, 0.5f, 0xFF0000FF));
    f.dev.setRenderState(RS_ALPHABLENDENABLE, 1);
    f.dev.setRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
    f.dev.setRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
    f.dev.setRenderState(RS_ZFUNC, CMP_ALWAYS);
    f.draw(quad(-1, -1, 1, 1, 0.5f, 0x80FF0000));  // 50 % red over blue
    f.dev.setRenderState(RS_ALPHABLENDENABLE, 0);
    f.dev.setRenderState(RS_COLORWRITEENABLE, 0x2);  // green only (D3DCOLORWRITEENABLE_GREEN)
    f.draw(quad(0, -1, 1, 1, 0.5f, 0xFF00FF00));
    const Pixels p = capture(f.dev, "blend");
    expect("blend", p, 10, 10, 0x80007F, 2);
    expect("color write mask", p, 50, 10, 0x80FF7F, 2);
}

void testAlphaTest(Fixture& f) {
    f.reset();
    f.dev.setRenderState(RS_ALPHATESTENABLE, 1);
    f.dev.setRenderState(RS_ALPHAFUNC, CMP_GREATER);
    f.dev.setRenderState(RS_ALPHAREF, 0x80);
    f.draw(quad(-1, -1, 0, 1, 0.5f, 0x40FFFFFF));  // alpha 0x40: discarded
    f.draw(quad(0, -1, 1, 1, 0.5f, 0xC0FFFFFF));   // alpha 0xC0: kept
    const Pixels p = capture(f.dev, "alphatest");
    expect("alpha test reject", p, 10, 10, 0x000000);
    expect("alpha test pass", p, 50, 10, 0xFFFFFF);
}

void testStencil(Fixture& f) {
    f.reset();
    f.dev.setRenderState(RS_STENCILENABLE, 1);
    f.dev.setRenderState(RS_STENCILFUNC, CMP_ALWAYS);
    f.dev.setRenderState(RS_STENCILPASS, STENCILOP_REPLACE);
    f.dev.setRenderState(RS_STENCILREF, 1);
    f.dev.setRenderState(RS_COLORWRITEENABLE, 0);
    f.draw(quad(-1, -1, 0, 1, 0.5f, 0xFFFFFFFF));  // stencil = 1 on the left half, no colour
    f.dev.setRenderState(RS_COLORWRITEENABLE, 0xF);
    f.dev.setRenderState(RS_STENCILFUNC, CMP_EQUAL);
    f.dev.setRenderState(RS_STENCILPASS, STENCILOP_KEEP);
    f.dev.setRenderState(RS_ZENABLE, ZB_FALSE);
    f.draw(quad(-1, -1, 1, 1, 0.5f, 0xFFFFFF00));
    const Pixels p = capture(f.dev, "stencil");
    expect("stencil pass", p, 10, 10, 0xFFFF00);
    expect("stencil fail", p, 50, 10, 0x000000);
}

void testRenderToTexture(Fixture& f) {
    TextureDesc d;
    d.width = d.height = 32;
    d.format = rhi::d3dfmt::A8R8G8B8;
    d.usage = USAGE_RENDERTARGET;
    d.pool = POOL_DEFAULT;
    auto rt = f.dev.createTexture(d);
    f.reset();
    f.dev.setRenderTarget(0, rt->surface(0, 0));
    f.dev.setDepthStencil(nullptr);
    f.dev.clear(CLEAR_TARGET, 0xFF00FFFF, 1.0f, 0);
    f.draw(quad(-1, -1, 0, 1, 0.5f, 0xFFFF00FF));  // left half magenta
    f.dev.setRenderTarget(0, f.dev.backBuffer());
    f.dev.setDepthStencil(f.dev.autoDepthStencil());
    f.dev.setPixelShader(f.psTexture);
    f.dev.setTexture(0, rt.get());
    f.dev.setSamplerState(0, SAMP_MAGFILTER, TEXF_POINT);
    f.dev.setSamplerState(0, SAMP_MINFILTER, TEXF_POINT);
    f.dev.setSamplerState(0, SAMP_ADDRESSU, TADDRESS_CLAMP);
    f.dev.setSamplerState(0, SAMP_ADDRESSV, TADDRESS_CLAMP);
    f.draw(quad(-1, -1, 1, 1, 0.5f, 0xFFFFFFFF));
    const Pixels p = capture(f.dev, "rtt");
    expect("render to texture (left)", p, 10, 10, 0xFF00FF);
    expect("render to texture (right)", p, 50, 10, 0x00FFFF);
    f.dev.setTexture(0, nullptr);
    f.dev.release(std::move(rt));
}

// Clears that open a scope are folded into its load ops (performance.md 32.5): same pixels as vkCmdClearAttachments, whole-area or partial
void testScopeClear(Fixture& f) {
    f.reset();
    capture(f.dev, "scope_clear_0");  // (the scope ends: the next clear is the first command of a new one)
    f.dev.clear(CLEAR_TARGET, 0xFF336699, 1.0f, 0);
    f.draw(quad(-1, -1, 0, 1, 0.5f, 0xFFFF0000));
    Pixels p = capture(f.dev, "scope_clear_1");
    expect("scope clear (colour kept right of the quad)", p, 50, 10, 0x336699);
    expect("scope clear (quad on top)", p, 10, 10, 0xFF0000);
    // a partial clear (viewport = left half) must leave the right half alone
    f.dev.setViewport({0, 0, 32, 64, 0.0f, 1.0f});
    f.dev.clear(CLEAR_TARGET, 0xFF00FF00, 1.0f, 0);
    f.dev.setViewport({0, 0, 64, 64, 0.0f, 1.0f});
    p = capture(f.dev, "scope_clear_2");
    expect("partial clear (cleared half)", p, 10, 10, 0x00FF00);
    expect("partial clear (other half untouched)", p, 50, 10, 0x336699);
    // depth: a near quad, then a fresh scope that clears depth + stencil first: a far quad then passes the depth test
    f.reset();
    f.draw(quad(-1, -1, 1, 1, 0.3f, 0xFF0000FF));
    capture(f.dev, "scope_clear_3");
    f.dev.clear(CLEAR_ZBUFFER | CLEAR_STENCIL, 0, 1.0f, 0);
    f.draw(quad(-1, -1, 1, 1, 0.6f, 0xFFFF0000));
    p = capture(f.dev, "scope_clear_4");
    expect("depth clear as a load op", p, 32, 32, 0xFF0000);
    // colour + depth with a clear value other than 1 (z = 0.5): a quad at 0.6 is rejected, one at 0.4 drawn
    f.reset();
    capture(f.dev, "scope_clear_5");
    f.dev.clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF102030, 0.5f, 0);
    f.draw(quad(-1, -1, 0, 1, 0.6f, 0xFFFF0000));
    f.draw(quad(0, -1, 1, 1, 0.4f, 0xFF00FF00));
    p = capture(f.dev, "scope_clear_6");
    expect("clear z 0.5: far quad rejected", p, 10, 10, 0x102030);
    expect("clear z 0.5: near quad drawn", p, 50, 10, 0x00FF00);
}

// A depth surface without stencil bits (D24X8) has no stencil plane even where the image has one (D32S8 on AMD): a stencil test there passes
void testNoStencilDepth(Fixture& f) {
    TextureDesc rd;
    rd.width = rd.height = 32;
    rd.format = rhi::d3dfmt::A8R8G8B8;
    rd.usage = USAGE_RENDERTARGET;
    rd.pool = POOL_DEFAULT;
    auto rt = f.dev.createTexture(rd);
    TextureDesc dd = rd;
    dd.format = rhi::d3dfmt::D24X8;
    dd.usage = USAGE_DEPTHSTENCIL;
    auto ds = f.dev.createTexture(dd);
    f.reset();
    capture(f.dev, "nostencil_0");
    const std::uint64_t before = f.dev.stats().stencilNoPlaneDraws;
    f.dev.setRenderTarget(0, rt->surface(0, 0));
    f.dev.setDepthStencil(ds->surface(0, 0));
    f.dev.clear(CLEAR_TARGET | CLEAR_ZBUFFER | CLEAR_STENCIL, 0xFF00FFFF, 1.0f, 0);
    f.dev.setRenderState(RS_STENCILENABLE, 1);
    f.dev.setRenderState(RS_STENCILFUNC, CMP_NEVER);  // D3D9: no stencil buffer = the test passes
    f.draw(quad(-1, -1, 0, 1, 0.5f, 0xFFFF00FF));
    f.dev.setRenderState(RS_STENCILENABLE, 0);
    f.dev.setRenderTarget(0, f.dev.backBuffer());
    f.dev.setDepthStencil(f.dev.autoDepthStencil());
    f.dev.setPixelShader(f.psTexture);
    f.dev.setTexture(0, rt.get());
    f.dev.setSamplerState(0, SAMP_MAGFILTER, TEXF_POINT);
    f.dev.setSamplerState(0, SAMP_MINFILTER, TEXF_POINT);
    f.dev.setSamplerState(0, SAMP_ADDRESSU, TADDRESS_CLAMP);
    f.dev.setSamplerState(0, SAMP_ADDRESSV, TADDRESS_CLAMP);
    f.draw(quad(-1, -1, 1, 1, 0.5f, 0xFFFFFFFF));
    const Pixels p = capture(f.dev, "nostencil");
    expect("no-stencil depth: NEVER passes (drawn)", p, 10, 10, 0xFF00FF);
    expect("no-stencil depth: outside the quad", p, 50, 10, 0x00FFFF);
    if (f.dev.stats().stencilNoPlaneDraws != before + 1) {
        std::printf("  FAIL no-stencil depth: stencilNoPlaneDraws %llu, expected %llu\n", static_cast<unsigned long long>(f.dev.stats().stencilNoPlaneDraws),
                    static_cast<unsigned long long>(before + 1));
        ++g_failures;
    }
    f.dev.setTexture(0, nullptr);
    f.dev.release(std::move(rt));
    f.dev.release(std::move(ds));
}

// S-6 (native.depth_z): a pass without colour attachment (a NULL colour target + the depth buffer: the sun cascades) draws a pixel shader that only writes colour without any pixel
// shader; the depth it writes is the same: a full-screen quad behind it (drawn into the back buffer, same depth buffer) shows only where nothing was drawn
void testDepthOnlyPass(Fixture& f) {
    TextureDesc nd;
    nd.width = nd.height = 64;
    nd.format = rhi::d3dfmt::NULLFMT;
    nd.usage = USAGE_RENDERTARGET;
    nd.pool = POOL_DEFAULT;
    auto nullRt = f.dev.createTexture(nd);
    f.reset();
    f.dev.setDepthZ(Device::DepthZ::PsLess);
    (void)f.dev.takeDepthZStats();
    f.dev.setRenderTarget(0, nullRt->surface(0, 0));
    f.draw(quad(-1, -1, 0, 1, 0.3f, 0xFFFF0000));  // the left half: depth only, no colour
    f.dev.setRenderTarget(0, f.dev.backBuffer());
    f.draw(quad(-1, -1, 1, 1, 0.5f, 0xFF00FF00));  // everything, farther: hidden where the first quad wrote depth
    const Pixels p = capture(f.dev, "depth_only_pass");
    expect("depth-only pass: depth written (the farther quad is hidden)", p, 10, 10, 0x000000);
    expect("depth-only pass: elsewhere the farther quad shows", p, 50, 10, 0x00FF00);
    const Device::DepthZStats st = f.dev.takeDepthZStats();
    if (st.psDropped != 1 || st.psKept != 0) {
        std::printf("  FAIL depth-only pass: pixel shaders left out %llu (kept %llu), expected 1 (0)\n", static_cast<unsigned long long>(st.psDropped), static_cast<unsigned long long>(st.psKept));
        ++g_failures;
    }
    f.dev.setDepthZ(Device::DepthZ::Off);
    f.dev.release(std::move(nullRt));
}

void testDxt1(Fixture& f) {
    // one DXT1 block per 4x4: colour0 = pure red (0xF800), colour1 = pure blue (0x001F), all indices 0 -> red
    TextureDesc d;
    d.width = d.height = 8;
    d.format = rhi::d3dfmt::DXT1;
    auto tex = f.dev.createTexture(d);
    std::vector<std::uint8_t> blocks(4 * 8);
    for (int b = 0; b < 4; ++b) {
        std::uint8_t* p = blocks.data() + b * 8;
        const std::uint16_t c0 = b == 3 ? 0x001F : 0xF800;
        const std::uint16_t c1 = 0x0000;
        std::memcpy(p, &c0, 2);
        std::memcpy(p + 2, &c1, 2);
        std::memset(p + 4, 0, 4);
    }
    tex->setLevelData(0, 0, blocks, 16);
    f.reset();
    f.dev.setPixelShader(f.psTexture);
    f.dev.setTexture(0, tex.get());
    f.dev.setSamplerState(0, SAMP_MAGFILTER, TEXF_POINT);
    f.dev.setSamplerState(0, SAMP_MINFILTER, TEXF_POINT);
    f.draw(quad(-1, -1, 1, 1, 0.5f, 0xFFFFFFFF));
    const Pixels p = capture(f.dev, "dxt1");
    expect("dxt1 block 0", p, 10, 10, 0xFF0000, 8);
    expect("dxt1 block 3", p, 50, 50, 0x0000FF, 8);
    f.dev.setTexture(0, nullptr);
    f.dev.release(std::move(tex));
}

void testInstancing(Fixture& f) {
    f.reset();
    auto inst = f.dev.createBuffer(2 * 16, 0, POOL_MANAGED, false, false);
    const float offsets[8] = {0, 0, 0, 0, 1, 0, 0, 0};  // instance 1 moves one unit right
    std::memcpy(inst->lock(0, 0, 0), offsets, sizeof(offsets));
    inst->unlock();
    f.dev.setVertexDecl(f.instDecl.get());
    f.dev.setVertexShader(f.vsInst);
    const auto v = quad(-1, -1, 0, 0, 0.5f, 0xFF00FF00);  // bottom-left quadrant
    void* p = f.vb->lock(0, 0, LOCK_DISCARD);
    std::memcpy(p, v.data(), v.size() * sizeof(Vertex));
    f.vb->unlock();
    f.dev.setStreamSource(0, f.vb.get(), 0, sizeof(Vertex));
    f.dev.setStreamSource(1, inst.get(), 0, 16);
    f.dev.setStreamSourceFreq(0, STREAMSOURCE_INDEXEDDATA | 2);
    f.dev.setStreamSourceFreq(1, STREAMSOURCE_INSTANCEDATA | 1);
    f.dev.drawPrimitive(PT_TRIANGLELIST, 0, 2);
    f.dev.setStreamSourceFreq(0, 1);
    f.dev.setStreamSourceFreq(1, 1);
    f.dev.setStreamSource(1, nullptr, 0, 0);
    const Pixels px = capture(f.dev, "instancing");
    expect("instance 0", px, 10, 50, 0x00FF00);
    expect("instance 1", px, 50, 50, 0x00FF00);
    expect("no instance", px, 10, 10, 0x000000);
    f.dev.release(std::move(inst));
}

// F11 (frame-ownership.md 14.26, native.own_particles): particle records expanded by the GPU (Device::particleVs = a translated particle-style vertex shader whose
// inputs rvkParticleExpand builds from the instance record + VS c232..c247) against the same particles expanded on the CPU by the C++ twin
// (d3d9::particle::expand) and drawn through the input assembler with the engine's vertex layout. Five ops: every orientation mode, entity coordinates with a
// rotated entity matrix, the axis in entity space, rotations about R / U / L, pinned corners, normal modes 0 / 2 / 3; four pixel shaders show COLOR0 (the
// facing bytes), the uv + COLOR1, the normal, the blend frame's uv. The pictures must be equal (a corner one ulp off may move an edge pixel: a handful allowed)
void testOwnParticles(Fixture& f) {
    namespace pt = rvk::d3d9::particle;
    const char* vsSrc = R"(
vs_2_0
dcl_position v0
dcl_normal v1
dcl_color v2
dcl_color1 v3
dcl_texcoord v4
dcl_texcoord1 v5
def c4, 1, 0, 0, 0
dp4 r0.x, v0, c31
dp4 r0.y, v0, c32
dp4 r0.z, v0, c33
mov r0.w, c4.x
dp4 oPos.x, r0, c0
dp4 oPos.y, r0, c1
dp4 oPos.z, r0, c2
dp4 oPos.w, r0, c3
mov oD0, v2
mov oT0.xy, v4
mov oT1.xy, v5
mov oT2, v1
mov oT3, v3
)";
    const char* psSrc[4] = {"ps_2_0\ndcl v0\nmov oC0, v0\n",
                            "ps_2_0\ndcl t0.xy\ndcl t3\ndef c0, 0, 0, 0, 1\nmov r0, c0\nmov r0.xy, t0\nmov r0.z, t3.x\nmov oC0, r0\n",
                            "ps_2_0\ndcl t2\ndef c0, 0.5, 0.5, 0.5, 1\nmad r0, t2, c0, c0\nmov r0.w, c0.w\nmov oC0, r0\n",
                            "ps_2_0\ndcl t1.xy\ndef c0, 0, 0, 0, 1\nmov r0, c0\nmov r0.xy, t1\nmov oC0, r0\n"};
    auto vs = shaderFrom(f.dev, vsSrc);
    std::shared_ptr<Shader> ps[4];
    for (int i = 0; i < 4; ++i) ps[i] = shaderFrom(f.dev, psSrc[i]);
    if (!vs || !ps[0] || !ps[1] || !ps[2] || !ps[3]) {
        std::printf("  FAIL own particles: shaders\n");
        ++g_failures;
        return;
    }
    // the engine's particle vertex layout (CreateVisual 0x5d7620)
    const VertexElement engineDecl[] = {
        {0, 0x00, DECLTYPE_FLOAT3, 0, 0 /*POSITION*/, 0},  {0, 0x0c, DECLTYPE_FLOAT3, 0, 3 /*NORMAL*/, 0},  {0, 0x18, DECLTYPE_D3DCOLOR, 0, 10 /*COLOR*/, 0},
        {0, 0x1c, DECLTYPE_D3DCOLOR, 0, 10, 1},            {0, 0x20, DECLTYPE_FLOAT2, 0, 5 /*TEXCOORD*/, 0}, {0, 0x28, DECLTYPE_FLOAT2, 0, 5, 1},
        {kDeclEndStream, 0, DECLTYPE_UNUSED, 0, 0, 0},
    };
    auto edecl = f.dev.createVertexDecl(engineDecl);
    const char* why = nullptr;
    Shader* pvs = f.dev.particleVs(*vs, pt::kKindParticle, edecl.get(), &why);
    VertexDecl* pdecl = f.dev.particleDecl(pt::kKindParticle);
    if (pvs == nullptr || pdecl == nullptr) {
        std::printf("  FAIL own particles: the expansion form: %s\n", why != nullptr ? why : "no declaration");
        ++g_failures;
        return;
    }
    constexpr std::uint32_t kQuads = 6;
    auto evb = f.dev.createBuffer(kQuads * 4 * pt::kVertexStride, USAGE_DYNAMIC | USAGE_WRITEONLY, POOL_DEFAULT, false, false);
    auto eib = f.dev.createBuffer(kQuads * 6 * 2, 0, POOL_MANAGED, true, false);
    auto rvb = f.dev.createBuffer(kQuads * pt::kRecordStride, USAGE_DYNAMIC | USAGE_WRITEONLY, POOL_DEFAULT, false, false);
    auto qib = f.dev.createBuffer(16, 0, POOL_MANAGED, true, false);
    auto dummy = f.dev.createBuffer(64, 0, POOL_MANAGED, false, false);
    const std::uint16_t order[6] = {0, 1, 2, 0, 2, 3};
    {
        auto* p = static_cast<std::uint16_t*>(eib->lock(0, 0, 0));
        for (std::uint32_t q = 0; q < kQuads; ++q) {
            for (int i = 0; i < 6; ++i) p[6 * q + i] = static_cast<std::uint16_t>(4 * q + order[i]);
        }
        eib->unlock();
        std::memcpy(qib->lock(0, 0, 0), order, sizeof(order));
        qib->unlock();
        std::memset(dummy->lock(0, 0, 0), 0, 64);
        dummy->unlock();
    }
    // the view: world x / y in [-8, 8] -> clip, z = 0.5; world identity; c34.w = 1
    const float vp[4][4] = {{0.125f, 0, 0, 0}, {0, 0.125f, 0, 0}, {0, 0, 0, 0.5f}, {0, 0, 0, 1}};
    const float world[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    struct Op {
        int mode, normalMode;
        bool entityCoords, axisInEntity, pinned;
    };
    const Op ops[] = {{0, 0, false, false, false}, {1, 1, true, false, false}, {2, 2, false, true, true}, {4, 2, true, true, false}, {3, 3, false, false, false}};
    int failures = 0, opIndex = 0;
    std::uint32_t worstPixels = 0;
    for (const Op& op : ops) {
        pt::OpConsts k{};
        // a camera at (0, 0, -20): rows right / up / look
        k.c[0][0] = 1.0f;
        k.c[1][1] = 1.0f;
        k.c[2][2] = 1.0f;
        k.c[3][2] = -20.0f;
        k.c[3][3] = static_cast<float>(op.mode);
        // the entity: a turn of 30 degrees about z, at (0.5, -0.25, 0); its inverse without translation (rows)
        const float cz = 0.8660254f, sz = 0.5f;
        const float E[16] = {cz, sz, 0, 0, -sz, cz, 0, 0, 0, 0, 1, 0, 0.5f, -0.25f, 0, 1};
        const float Mi[16] = {cz, -sz, 0, 0, sz, cz, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::memcpy(k.c[4], E, 64);
        if (op.entityCoords) std::memcpy(k.c[8], Mi, 64);
        k.c[12][0] = 0.3f;
        k.c[12][1] = 0.4f;
        k.c[12][2] = -0.866f;
        k.c[12][3] = static_cast<float>((op.entityCoords ? pt::kFlagEntityCoords : 0u) | (op.axisInEntity ? pt::kFlagAxisInEntity : 0u) |
                                        (op.pinned ? pt::kFlagPinned : 0u) | static_cast<std::uint32_t>(op.normalMode) << 4);
        k.c[13][0] = -6.0f;
        k.c[13][1] = 6.0f;
        k.c[13][3] = 0.25f;  // pivot A
        k.c[14][0] = 1.0f;   // Eo
        k.c[14][3] = -0.5f;  // pivot B
        k.c[15][1] = 1.0f;   // up
        std::vector<pt::Record> recs(kQuads);
        for (std::uint32_t q = 0; q < kQuads; ++q) {
            pt::Record& r = recs[q];
            r.r[0][0] = -5.1f + 4.9f * static_cast<float>(q % 3);
            r.r[0][1] = -3.3f + 6.2f * static_cast<float>(q / 3);
            r.r[0][2] = 0.7f * static_cast<float>(q) - 1.0f;
            r.r[0][3] = 1.3f + 0.21f * static_cast<float>(q);  // hx
            r.r[1][0] = 0.6f - 0.1f * static_cast<float>(q);   // d
            r.r[1][1] = 0.8f;
            r.r[1][2] = 0.15f * static_cast<float>(q);
            r.r[1][3] = 1.1f + 0.13f * static_cast<float>(q);  // hy
            const std::uint32_t mask = q % 4 == 0 ? 0u : q % 4 == 1 ? 1u : q % 4 == 2 ? 4u : 7u;
            const float ang[3] = {0.4f + 0.1f * static_cast<float>(q), -0.7f, 1.2f - 0.2f * static_cast<float>(q)};
            r.r[2][0] = std::sin(ang[0]);
            r.r[2][1] = std::cos(ang[0]);
            r.r[2][2] = std::sin(ang[1]);
            r.r[2][3] = std::cos(ang[1]);
            r.r[3][0] = std::sin(ang[2]);
            r.r[3][1] = std::cos(ang[2]);
            r.mask = static_cast<float>(mask);
            r.r[3][2] = 0.15f * static_cast<float>(q + 1);  // age ratio
            r.r[3][3] = 1.0f - 0.1f * static_cast<float>(q);
            r.r[4][0] = 0.25f * static_cast<float>(q % 4), r.r[4][1] = 0.5f, r.r[4][2] = r.r[4][0] + 0.25f, r.r[4][3] = 1.0f;
            r.r[5][0] = 0.1f, r.r[5][1] = 0.2f * static_cast<float>(q % 3), r.r[5][2] = 0.6f, r.r[5][3] = 0.9f;
            r.colour1 = 0xff000000u | (40u * q) << 16 | (255u - 30u * q) << 8 | 0x33u;
        }
        // the CPU reference vertices (the C++ twin) and the records
        {
            auto* v = static_cast<std::uint8_t*>(evb->lock(0, 0, LOCK_DISCARD));
            for (std::uint32_t q = 0; q < kQuads; ++q) {
                for (std::uint32_t c = 0; c < 4; ++c) pt::expand(k, recs[q], c, v + (4 * q + c) * pt::kVertexStride);
            }
            evb->unlock();
            std::memcpy(rvb->lock(0, 0, LOCK_DISCARD), recs.data(), recs.size() * sizeof(pt::Record));
            rvb->unlock();
        }
        for (int s = 0; s < 4; ++s) {
            Pixels pic[2];
            for (int pass = 0; pass < 2; ++pass) {
                f.reset();
                f.dev.setRenderState(RS_ZENABLE, 0);
                f.dev.setPixelShader(ps[s]);
                f.dev.setVertexShaderConstantF(0, vp[0], 4);
                f.dev.setVertexShaderConstantF(31, world[0], 3);
                if (pass == 0) {
                    f.dev.setVertexShader(vs);
                    f.dev.setVertexDecl(edecl.get());
                    f.dev.setStreamSource(0, evb.get(), 0, pt::kVertexStride);
                    f.dev.setIndices(eib.get());
                    f.dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, kQuads * 4, 0, kQuads * 2);
                } else {
                    // as StaticOps::particleToGpu sets the draw: the expansion form, the record stream at instance rate, the quad's 6 indices, c232..c247
                    f.dev.setVertexShaderRef(ShaderRef(pvs));
                    f.dev.setVertexDecl(pdecl);
                    f.dev.setStreamSource(0, dummy.get(), 0, 16);
                    f.dev.setStreamSourceFreq(0, STREAMSOURCE_INDEXEDDATA | kQuads);
                    f.dev.setStreamSource(1, rvb.get(), 0, pt::kRecordStride);
                    f.dev.setStreamSourceFreq(1, STREAMSOURCE_INSTANCEDATA | 1);
                    f.dev.setIndices(qib.get());
                    f.dev.setVertexShaderConstantF(pt::kConstFirst, k.c[0], pt::kConstCount);
                    f.dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, 4, 0, 2);
                    f.dev.setStreamSourceFreq(0, 1);
                    f.dev.setStreamSourceFreq(1, 1);
                    f.dev.setStreamSource(1, nullptr, 0, 0);
                }
                pic[pass] = capture(f.dev, pass == 0 ? "particles_cpu" : "particles_gpu");
            }
            std::uint32_t off = 0, lit = 0;
            for (std::uint32_t y = 0; y < pic[0].h; ++y) {
                for (std::uint32_t x = 0; x < pic[0].w; ++x) {
                    if (!similar(pic[0].at(x, y), pic[1].at(x, y), 1)) ++off;
                    if ((pic[0].at(x, y) & 0xffffff) != 0) ++lit;
                }
            }
            worstPixels = std::max(worstPixels, off);
            if (off > 3 || lit < 200) {
                std::printf("  FAIL own particles: op %d (mode %d) pixel shader %d: %u pixels differ, %u lit\n", opIndex, op.mode, s, off, lit);
                ++failures;
            }
        }
        ++opIndex;
    }
    // the precipitation's form: eCSkyObject billboard records (0x30) expanded by rvkSkyBillboardExpand against sc::skyBillboardVertices through the input assembler
    {
        const char* skyVs = R"(
vs_2_0
dcl_position v0
dcl_color v1
dcl_texcoord v2
dcl_texcoord1 v3
def c4, 1, 0, 0, 0
mov r0, v0
add r0.xy, r0, v3
mov r0.w, c4.x
dp4 oPos.x, r0, c0
dp4 oPos.y, r0, c1
dp4 oPos.z, r0, c2
dp4 oPos.w, r0, c3
mov oD0, v1
mov oT0.xy, v2
)";
        auto svs = shaderFrom(f.dev, skyVs);
        auto sps = shaderFrom(f.dev, "ps_2_0\ndcl v0\ndcl t0.xy\nmov r0, v0\nmov r0.xy, t0\nmov oC0, r0\n");
        const VertexElement skyDecl[] = {
            {0, 0x00, DECLTYPE_FLOAT3, 0, 0, 0}, {0, 0x0c, DECLTYPE_D3DCOLOR, 0, 10, 0}, {0, 0x10, DECLTYPE_FLOAT2, 0, 5, 0}, {0, 0x18, DECLTYPE_FLOAT2, 0, 5, 1},
            {kDeclEndStream, 0, DECLTYPE_UNUSED, 0, 0, 0},
        };
        auto sdecl = f.dev.createVertexDecl(skyDecl);
        const char* swhy = nullptr;
        Shader* spvs = svs ? f.dev.particleVs(*svs, pt::kKindSkyBillboard, sdecl.get(), &swhy) : nullptr;
        VertexDecl* spdecl = f.dev.particleDecl(pt::kKindSkyBillboard);
        if (spvs == nullptr || !sps || spdecl == nullptr) {
            std::printf("  FAIL own particles: the sky billboard form: %s\n", swhy != nullptr ? swhy : "shaders");
            ++failures;
        } else {
            constexpr std::uint32_t kDrops = 9;
            std::vector<std::uint8_t> bb(kDrops * pt::kSkyRecordStride);
            for (std::uint32_t i = 0; i < kDrops; ++i) {
                float o[12] = {-5.3f + 5.1f * static_cast<float>(i % 3), -5.2f + 4.9f * static_cast<float>(i / 3), 0.25f, 0.4f + 0.05f * static_cast<float>(i), 0.1f,
                               0.2f, 1.7f - 0.1f * static_cast<float>(i), 0.0f, 0.1f * static_cast<float>(i), 0.0f, 0.1f * static_cast<float>(i) + 0.4f, 1.0f};
                const std::uint32_t colour = 0xff000000u | (25u * i) << 16 | 0x8080u;
                std::memcpy(&o[7], &colour, 4);
                std::memcpy(bb.data() + i * pt::kSkyRecordStride, o, sizeof(o));
            }
            std::vector<std::uint8_t> verts;
            rvk::native::sc::skyBillboardVertices(bb, kDrops, verts);
            auto svb = f.dev.createBuffer(static_cast<std::uint32_t>(verts.size()), 0, POOL_MANAGED, false, false);
            auto srv = f.dev.createBuffer(static_cast<std::uint32_t>(bb.size()), 0, POOL_MANAGED, false, false);
            auto sib = f.dev.createBuffer(kDrops * 12, 0, POOL_MANAGED, true, false);
            std::memcpy(svb->lock(0, 0, 0), verts.data(), verts.size());
            svb->unlock();
            std::memcpy(srv->lock(0, 0, 0), bb.data(), bb.size());
            srv->unlock();
            auto* ip = static_cast<std::uint16_t*>(sib->lock(0, 0, 0));
            for (std::uint32_t q = 0; q < kDrops; ++q) {
                for (int i = 0; i < 6; ++i) ip[6 * q + i] = static_cast<std::uint16_t>(4 * q + order[i]);
            }
            sib->unlock();
            Pixels pic[2];
            for (int pass = 0; pass < 2; ++pass) {
                f.reset();
                f.dev.setRenderState(RS_ZENABLE, 0);
                f.dev.setPixelShader(sps);
                f.dev.setVertexShaderConstantF(0, vp[0], 4);
                if (pass == 0) {
                    f.dev.setVertexShader(svs);
                    f.dev.setVertexDecl(sdecl.get());
                    f.dev.setStreamSource(0, svb.get(), 0, 0x20);
                    f.dev.setIndices(sib.get());
                    f.dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, kDrops * 4, 0, kDrops * 2);
                } else {
                    f.dev.setVertexShaderRef(ShaderRef(spvs));
                    f.dev.setVertexDecl(spdecl);
                    f.dev.setStreamSource(0, dummy.get(), 0, 16);
                    f.dev.setStreamSourceFreq(0, STREAMSOURCE_INDEXEDDATA | kDrops);
                    f.dev.setStreamSource(1, srv.get(), 0, pt::kSkyRecordStride);
                    f.dev.setStreamSourceFreq(1, STREAMSOURCE_INSTANCEDATA | 1);
                    f.dev.setIndices(qib.get());
                    f.dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, 4, 0, 2);
                    f.dev.setStreamSourceFreq(0, 1);
                    f.dev.setStreamSourceFreq(1, 1);
                    f.dev.setStreamSource(1, nullptr, 0, 0);
                }
                pic[pass] = capture(f.dev, pass == 0 ? "precip_cpu" : "precip_gpu");
            }
            std::uint32_t off = 0, lit = 0;
            for (std::uint32_t y = 0; y < pic[0].h; ++y) {
                for (std::uint32_t x = 0; x < pic[0].w; ++x) {
                    if (pic[0].at(x, y) != pic[1].at(x, y)) ++off;
                    if ((pic[0].at(x, y) & 0xffffff) != 0) ++lit;
                }
            }
            if (off != 0 || lit < 200) {
                std::printf("  FAIL own particles: sky billboards: %u pixels differ, %u lit\n", off, lit);
                ++failures;
            }
            f.dev.setIndices(nullptr);
            for (auto* b : {&svb, &srv, &sib}) f.dev.release(std::move(*b));
        }
        f.dev.release(std::move(sdecl));
    }
    // strips: per-vertex records (rvkStripExpand: the colours lerped in the shader) against the C++ twin's vertices, a non-indexed triangle strip
    {
        const char* stripVs = R"(
vs_2_0
dcl_position v0
dcl_color v1
dcl_texcoord v2
dcl_texcoord1 v3
def c4, 1, 0, 0, 0
mov r0, v0
mov r0.w, c4.x
dp4 oPos.x, r0, c0
dp4 oPos.y, r0, c1
dp4 oPos.z, r0, c2
dp4 oPos.w, r0, c3
mov oD0, v1
mov oT0.xy, v2
mov oT1.xy, v3
)";
        auto tvs = shaderFrom(f.dev, stripVs);
        auto tps = shaderFrom(f.dev, "ps_2_0\ndcl v0\ndcl t0.xy\nmov r0, v0\nmov r0.y, t0.x\nmov oC0, r0\n");
        const VertexElement stripDecl[] = {
            {0, 0x00, DECLTYPE_FLOAT3, 0, 0, 0}, {0, 0x0c, DECLTYPE_D3DCOLOR, 0, 10, 0}, {0, 0x10, DECLTYPE_FLOAT2, 0, 5, 0}, {0, 0x18, DECLTYPE_FLOAT2, 0, 5, 1},
            {kDeclEndStream, 0, DECLTYPE_UNUSED, 0, 0, 0},
        };
        auto tdecl = f.dev.createVertexDecl(stripDecl);
        const char* twhy = nullptr;
        Shader* tpvs = tvs ? f.dev.particleVs(*tvs, pt::kKindStrip, tdecl.get(), &twhy) : nullptr;
        VertexDecl* tpdecl = f.dev.particleDecl(pt::kKindStrip);
        if (tpvs == nullptr || !tps || tpdecl == nullptr) {
            std::printf("  FAIL own particles: the strip form: %s\n", twhy != nullptr ? twhy : "shaders");
            ++failures;
        } else {
            constexpr std::uint32_t kSeg = 8;
            const float ca[4] = {0.9f, 0.2f, 0.1f, 0.8f}, cb[4] = {0.1f, 0.7f, 1.0f, 0.3f};
            std::vector<pt::StripRecord> recs(2 * kSeg);
            std::vector<std::uint8_t> verts(2 * kSeg * 0x20);
            float u = 0.0f, v = 0.0f;
            for (std::uint32_t i = 0; i < kSeg; ++i) {
                const float x = -7.0f + 14.0f * static_cast<float>(i) / (kSeg - 1);
                recs[2 * i] = {{x, -3.0f + 0.3f * static_cast<float>(i), 0.5f}, u, 0.5f + 0.06f * static_cast<float>(i), 0.0f};
                recs[2 * i + 1] = {{x + 0.4f, 3.5f, 0.5f}, v, 0.5f + 0.06f * static_cast<float>(i), 1.0f};
                u += 0.13f;
                v += 0.11f;
            }
            for (std::uint32_t k = 0; k < 2 * kSeg; ++k) pt::expandStrip(ca, cb, recs[k], verts.data() + k * 0x20);
            auto tvb = f.dev.createBuffer(static_cast<std::uint32_t>(verts.size()), 0, POOL_MANAGED, false, false);
            auto trv = f.dev.createBuffer(static_cast<std::uint32_t>(recs.size() * sizeof(pt::StripRecord)), 0, POOL_MANAGED, false, false);
            std::memcpy(tvb->lock(0, 0, 0), verts.data(), verts.size());
            tvb->unlock();
            std::memcpy(trv->lock(0, 0, 0), recs.data(), recs.size() * sizeof(pt::StripRecord));
            trv->unlock();
            float cs[2][4];
            std::memcpy(cs[0], ca, 16);
            std::memcpy(cs[1], cb, 16);
            Pixels pic[2];
            for (int pass = 0; pass < 2; ++pass) {
                f.reset();
                f.dev.setRenderState(RS_ZENABLE, 0);
                f.dev.setPixelShader(tps);
                f.dev.setVertexShaderConstantF(0, vp[0], 4);
                if (pass == 0) {
                    f.dev.setVertexShader(tvs);
                    f.dev.setVertexDecl(tdecl.get());
                    f.dev.setStreamSource(0, tvb.get(), 0, 0x20);
                } else {
                    f.dev.setVertexShaderRef(ShaderRef(tpvs));
                    f.dev.setVertexDecl(tpdecl);
                    f.dev.setStreamSource(0, trv.get(), 0, pt::kStripRecordStride);
                    f.dev.setVertexShaderConstantF(pt::kConstFirst, cs[0], 2);
                }
                f.dev.drawPrimitive(PT_TRIANGLESTRIP, 0, 2 * kSeg - 2);
                pic[pass] = capture(f.dev, pass == 0 ? "strip_cpu" : "strip_gpu");
            }
            std::uint32_t off = 0, lit = 0;
            for (std::uint32_t y = 0; y < pic[0].h; ++y) {
                for (std::uint32_t x = 0; x < pic[0].w; ++x) {
                    if (pic[0].at(x, y) != pic[1].at(x, y)) ++off;
                    if ((pic[0].at(x, y) & 0xffffff) != 0) ++lit;
                }
            }
            if (off != 0 || lit < 200) {
                std::printf("  FAIL own particles: strips: %u pixels differ, %u lit\n", off, lit);
                ++failures;
            }
            for (auto* b : {&tvb, &trv}) f.dev.release(std::move(*b));
        }
        f.dev.release(std::move(tdecl));
    }
    f.dev.setIndices(nullptr);
    if (failures != 0) g_failures += failures;
    else std::printf("  own particles: 5 ops x 4 pictures + the sky billboards + a strip equal to the CPU twin's (worst %u edge pixels)\n", worstPixels);
    for (auto* b : {&evb, &eib, &rvb, &qib, &dummy}) f.dev.release(std::move(*b));
    f.dev.release(std::move(edecl));
}

// F7-1 (frame-ownership.md 7.40): the vegetation's retained instance stream = one dynamic buffer holding many batches' instances, a batch drawn from a stream
// OFFSET; the buffer stays untouched across frames (the GPU copy persists) and is rewritten (DISCARD, the whole range) only when the source changed
void testRetainedInstanceStream(Fixture& f) {
    f.reset();
    auto inst = f.dev.createBuffer(4 * 16, USAGE_DYNAMIC | USAGE_WRITEONLY, POOL_DEFAULT, false, false);
    const auto fill = [&](const float (&x)[4]) {
        float data[16] = {};
        for (int i = 0; i < 4; ++i) data[i * 4] = x[i];
        void* p = inst->lock(0, 4 * 16, LOCK_DISCARD);
        std::memcpy(p, data, sizeof(data));
        inst->unlock();
    };
    const auto drawBatch = [&](std::uint32_t firstInstance, const char* name) {
        f.reset();
        f.dev.setVertexDecl(f.instDecl.get());
        f.dev.setVertexShader(f.vsInst);
        const auto v = quad(-1, -1, 0, 0, 0.5f, 0xFF00FF00);  // bottom-left quadrant
        void* p = f.vb->lock(0, 0, LOCK_DISCARD);
        std::memcpy(p, v.data(), v.size() * sizeof(Vertex));
        f.vb->unlock();
        f.dev.setStreamSource(0, f.vb.get(), 0, sizeof(Vertex));
        f.dev.setStreamSource(1, inst.get(), firstInstance * 16, 16);
        f.dev.setStreamSourceFreq(0, STREAMSOURCE_INDEXEDDATA | 2);
        f.dev.setStreamSourceFreq(1, STREAMSOURCE_INSTANCEDATA | 1);
        f.dev.drawPrimitive(PT_TRIANGLELIST, 0, 2);
        f.dev.setStreamSourceFreq(0, 1);
        f.dev.setStreamSourceFreq(1, 1);
        f.dev.setStreamSource(1, nullptr, 0, 0);
        return capture(f.dev, name);
    };
    // instances {0, 1, 1, 0}: the batch from instance 1 = two instances both one unit to the right
    fill({0.0f, 1.0f, 1.0f, 0.0f});
    Pixels px = drawBatch(1, "retained_inst_1");
    expect("batch at offset 1: nothing at the left", px, 10, 50, 0x000000);
    expect("batch at offset 1: right", px, 50, 50, 0x00FF00);
    // the next frame, the buffer untouched
    px = drawBatch(1, "retained_inst_2");
    expect("untouched buffer, next frame: nothing at the left", px, 10, 50, 0x000000);
    expect("untouched buffer, next frame: right", px, 50, 50, 0x00FF00);
    // another batch of the same buffer: instances 0 and 1 = one left, one right
    px = drawBatch(0, "retained_inst_3");
    expect("batch at offset 0: left", px, 10, 50, 0x00FF00);
    expect("batch at offset 0: right", px, 50, 50, 0x00FF00);
    // the source changed: the whole range rewritten
    fill({0.0f, 0.0f, 0.0f, 1.0f});
    px = drawBatch(1, "retained_inst_4");
    expect("rewritten: left", px, 10, 50, 0x00FF00);
    expect("rewritten: right empty", px, 50, 50, 0x000000);
    f.dev.release(std::move(inst));
}

void testUpAndScissor(Fixture& f) {
    f.reset();
    const auto v = quad(-1, -1, 1, 1, 0.5f, 0xFFFF8000);
    f.dev.setRenderState(RS_SCISSORTESTENABLE, 1);
    f.dev.setScissorRect(0, 0, 16, 64);
    f.dev.drawPrimitiveUP(PT_TRIANGLELIST, 2, v.data(), sizeof(Vertex));
    f.dev.setRenderState(RS_SCISSORTESTENABLE, 0);
    const Pixels p = capture(f.dev, "up_scissor");
    expect("DrawPrimitiveUP inside scissor", p, 8, 30, 0xFF8000);
    expect("DrawPrimitiveUP outside scissor", p, 20, 30, 0x000000);
}

void testStretchRect(Fixture& f) {
    TextureDesc d;
    d.width = d.height = 64;
    d.format = rhi::d3dfmt::A8R8G8B8;
    d.usage = USAGE_RENDERTARGET;
    d.pool = POOL_DEFAULT;
    auto rt = f.dev.createTexture(d);
    f.reset();
    f.dev.setRenderTarget(0, rt->surface(0, 0));
    f.dev.setDepthStencil(nullptr);
    f.dev.clear(CLEAR_TARGET, 0xFF123456, 1.0f, 0);
    f.dev.setRenderTarget(0, f.dev.backBuffer());
    const std::int32_t dstRect[4] = {32, 0, 64, 64};
    f.dev.stretchRect(rt->surface(0, 0), nullptr, f.dev.backBuffer(), dstRect, false);
    const Pixels p = capture(f.dev, "stretchrect");
    expect("stretch rect", p, 50, 10, 0x123456);
    expect("stretch rect untouched", p, 10, 10, 0x000000);
    f.dev.release(std::move(rt));
}

// the engine's mip chain generation: StretchRect between the levels of one texture (a blit onto itself, GENERAL layout), then sampled
void testMipChainStretchRect(Fixture& f) {
    TextureDesc d;
    d.width = d.height = 64;
    d.levels = 3;
    d.format = rhi::d3dfmt::A8R8G8B8;
    d.usage = USAGE_RENDERTARGET;
    d.pool = POOL_DEFAULT;
    auto tex = f.dev.createTexture(d);
    f.reset();
    f.dev.setRenderTarget(0, tex->surface(0, 0));
    f.dev.setDepthStencil(nullptr);
    f.dev.clear(CLEAR_TARGET, 0xFF336699, 1.0f, 0);
    f.dev.setRenderTarget(0, f.dev.backBuffer());
    f.dev.stretchRect(tex->surface(0, 0), nullptr, tex->surface(1, 0), nullptr, true);
    f.dev.stretchRect(tex->surface(1, 0), nullptr, tex->surface(2, 0), nullptr, true);
    const Pixels p = capture(f.dev, "mipchain");  // (presents: the validation layer sees the recorded blits)
    (void)p;
    f.dev.release(std::move(tex));
}

void testDynamicBuffer(Fixture& f) {
    // DISCARD then NOOVERWRITE appends within one frame: every draw sees its own data
    f.reset();
    const auto a = quad(-1, -1, 0, 1, 0.5f, 0xFFFF0000);
    const auto b = quad(0, -1, 1, 1, 0.5f, 0xFF0000FF);
    auto dyn = f.dev.createBuffer(static_cast<std::uint32_t>(12 * sizeof(Vertex)), USAGE_DYNAMIC | USAGE_WRITEONLY, POOL_DEFAULT,
                                  false, false);
    std::memcpy(dyn->lock(0, 0, LOCK_DISCARD), a.data(), 6 * sizeof(Vertex));
    dyn->unlock();
    f.dev.setStreamSource(0, dyn.get(), 0, sizeof(Vertex));
    f.dev.drawPrimitive(PT_TRIANGLELIST, 0, 2);
    std::memcpy(dyn->lock(6 * sizeof(Vertex), 6 * sizeof(Vertex), LOCK_NOOVERWRITE), b.data(), 6 * sizeof(Vertex));
    dyn->unlock();
    f.dev.drawPrimitive(PT_TRIANGLELIST, 6, 2);
    // DISCARD again with other data: the first two draws keep theirs
    const auto c = quad(-1, -1, 1, 1, 0.5f, 0xFF00FF00);
    std::memcpy(dyn->lock(0, 0, LOCK_DISCARD), c.data(), 6 * sizeof(Vertex));
    dyn->unlock();
    f.dev.setRenderState(RS_ZFUNC, CMP_LESS);  // equal depth: rejected, so only the stencil-free earlier draws show
    f.dev.drawPrimitive(PT_TRIANGLELIST, 0, 2);
    const Pixels p = capture(f.dev, "dynamic");
    expect("dynamic discard", p, 10, 10, 0xFF0000);
    expect("dynamic nooverwrite", p, 50, 10, 0x0000FF);
    f.dev.setStreamSource(0, nullptr, 0, 0);
    f.dev.release(std::move(dyn));
}

void testDynamicBufferAcrossFrames(Fixture& f) {
    // data written in an earlier frame (DISCARD + NOOVERWRITE) is still drawn in a later frame without a new lock
    f.reset();
    const auto a = quad(-1, -1, 0, 1, 0.5f, 0xFFFF0000);
    const auto b = quad(0, -1, 1, 1, 0.5f, 0xFF0000FF);
    auto dyn = f.dev.createBuffer(static_cast<std::uint32_t>(64 * sizeof(Vertex)), USAGE_DYNAMIC | USAGE_WRITEONLY, POOL_DEFAULT,
                                  false, false);
    std::memcpy(dyn->lock(0, static_cast<std::uint32_t>(6 * sizeof(Vertex)), LOCK_DISCARD), a.data(), 6 * sizeof(Vertex));
    dyn->unlock();
    f.dev.setStreamSource(0, dyn.get(), 0, sizeof(Vertex));
    f.dev.drawPrimitive(PT_TRIANGLELIST, 0, 2);
    std::memcpy(dyn->lock(40 * sizeof(Vertex), 6 * sizeof(Vertex), LOCK_NOOVERWRITE), b.data(), 6 * sizeof(Vertex));
    dyn->unlock();
    f.dev.drawPrimitive(PT_TRIANGLELIST, 40, 2);
    capture(f.dev, "dynamic_frames_1");  // ends the frame
    f.reset();
    f.dev.setStreamSource(0, dyn.get(), 0, sizeof(Vertex));
    f.dev.drawPrimitive(PT_TRIANGLELIST, 0, 2);
    f.dev.drawPrimitive(PT_TRIANGLELIST, 40, 2);
    const Pixels p = capture(f.dev, "dynamic_frames_2");
    expect("dynamic data of an earlier frame (discard range)", p, 10, 10, 0xFF0000);
    expect("dynamic data of an earlier frame (nooverwrite range)", p, 50, 10, 0x0000FF);
    f.dev.setStreamSource(0, nullptr, 0, 0);
    f.dev.release(std::move(dyn));
}

// fills rows [y0, y1) x columns [x0, x1) of a locked A8R8G8B8 level with `color`; `bits` points at (rx, ry)
void fillLocked(const Texture::Locked& lk, std::uint32_t rx, std::uint32_t ry, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
                std::uint32_t y1, std::uint32_t color) {
    for (std::uint32_t y = y0; y < y1; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(lk.bits) + static_cast<std::size_t>(y - ry) * lk.rowPitch);
        for (std::uint32_t x = x0; x < x1; ++x) {
            row[x - rx] = color;
        }
    }
}

void drawTexture(Fixture& f, Texture* tex, const char* name, Pixels& out) {
    f.reset();
    f.dev.setPixelShader(f.psTexture);
    f.dev.setTexture(0, tex);
    f.dev.setSamplerState(0, SAMP_MAGFILTER, TEXF_POINT);
    f.dev.setSamplerState(0, SAMP_MINFILTER, TEXF_POINT);
    f.dev.setSamplerState(0, SAMP_ADDRESSU, TADDRESS_CLAMP);
    f.dev.setSamplerState(0, SAMP_ADDRESSV, TADDRESS_CLAMP);
    f.draw(quad(-1, -1, 1, 1, 0.5f, 0xFFFFFFFF));
    out = capture(f.dev, name);
    f.dev.setTexture(0, nullptr);
}

void testPartialLock(Fixture& f) {
    // a managed texture updated by partial LockRect after it was sampled (the engine's glyph atlases): earlier contents survive
    TextureDesc d;
    d.width = d.height = 64;
    d.format = rhi::d3dfmt::A8R8G8B8;
    d.pool = POOL_MANAGED;
    auto tex = f.dev.createTexture(d);
    Texture::Locked lk;
    const std::int32_t a[4] = {0, 0, 32, 32};  // top-left: red
    tex->lock(0, 0, a, 0, lk);
    fillLocked(lk, 0, 0, 0, 0, 32, 32, 0xFFFF0000);
    tex->unlock(0, 0);
    Pixels p;
    drawTexture(f, tex.get(), "partial_lock_1", p);
    expect("partial lock A", p, 10, 10, 0xFF0000);
    expect("partial lock A: rest zero", p, 50, 50, 0x000000);
    const std::int32_t b[4] = {32, 32, 64, 64};  // bottom-right: blue, in a later frame
    tex->lock(0, 0, b, 0, lk);
    fillLocked(lk, 32, 32, 32, 32, 64, 64, 0xFF0000FF);
    tex->unlock(0, 0);
    drawTexture(f, tex.get(), "partial_lock_2", p);
    expect("partial lock B", p, 50, 50, 0x0000FF);
    expect("partial lock: A survives B", p, 10, 10, 0xFF0000);
    expect("partial lock: untouched", p, 50, 10, 0x000000);
    // the CPU copy is kept: a read-only lock sees both regions
    tex->lock(0, 0, nullptr, LOCK_READONLY, lk);
    const auto* px = static_cast<const std::uint32_t*>(lk.bits);
    if (px[0] != 0xFFFF0000u || px[63 * (lk.rowPitch / 4) + 63] != 0xFF0000FFu) {
        std::printf("  FAIL partial lock: read-only lock sees %08X / %08X\n", px[0], px[63 * (lk.rowPitch / 4) + 63]);
        ++g_failures;
    }
    tex->unlock(0, 0);
    f.dev.release(std::move(tex));
}

void testLockAfterUpload(Fixture& f) {
    // a file texture (setLevelData) drops its CPU copy after the upload; a later whole-level lock without DISCARD that
    // writes one corner must still see the file contents everywhere else (read back from the GPU)
    TextureDesc d;
    d.width = d.height = 64;
    d.format = rhi::d3dfmt::A8R8G8B8;
    d.pool = POOL_MANAGED;
    auto tex = f.dev.createTexture(d);
    std::vector<std::uint32_t> green(64 * 64, 0xFF00FF00u);
    tex->setLevelData(0, 0, {reinterpret_cast<const std::uint8_t*>(green.data()), green.size() * 4}, 64 * 4);
    Pixels p;
    drawTexture(f, tex.get(), "lock_after_upload_1", p);
    expect("file texture", p, 50, 50, 0x00FF00);
    Texture::Locked lk;
    tex->lock(0, 0, nullptr, 0, lk);
    fillLocked(lk, 0, 0, 0, 0, 16, 16, 0xFFFFFF00);
    tex->unlock(0, 0);
    drawTexture(f, tex.get(), "lock_after_upload_2", p);
    expect("lock after upload: written corner", p, 5, 5, 0xFFFF00);
    expect("lock after upload: file contents kept", p, 50, 50, 0x00FF00);
    f.dev.release(std::move(tex));
}

void testReadOnlyLockOfFreshTexture(Fixture& f) {
    // eCGfxImageData::OnRead (the savegame thumbnails) fills a fresh managed texture through LockImageLayer(level,
    // D3DLOCK_READONLY): D3D9 keeps a fresh level dirty until its first upload, so the written texels are sampled
    TextureDesc d;
    d.width = d.height = 64;
    d.format = rhi::d3dfmt::A8R8G8B8;
    d.pool = POOL_MANAGED;
    auto tex = f.dev.createTexture(d);
    Texture::Locked lk;
    tex->lock(0, 0, nullptr, LOCK_READONLY, lk);
    fillLocked(lk, 0, 0, 0, 0, 64, 64, 0xFF00FF00);
    fillLocked(lk, 0, 0, 0, 0, 16, 16, 0xFFFF0000);
    tex->unlock(0, 0);
    Pixels p;
    drawTexture(f, tex.get(), "readonly_fresh_1", p);
    expect("read-only lock of a fresh level", p, 50, 50, 0x00FF00);
    expect("read-only lock of a fresh level: corner", p, 5, 5, 0xFF0000);
    // after the upload a READONLY lock adds nothing: a write through it is not uploaded (D3D9 behaves alike)
    tex->lock(0, 0, nullptr, LOCK_READONLY, lk);
    fillLocked(lk, 0, 0, 32, 32, 64, 64, 0xFF0000FF);
    tex->unlock(0, 0);
    drawTexture(f, tex.get(), "readonly_fresh_2", p);
    expect("read-only lock after the upload", p, 50, 50, 0x00FF00);
    f.dev.release(std::move(tex));
}

void testFvf(Fixture& f) {
    // an FVF vertex buffer (XYZ | DIFFUSE | TEX2, the engine's 0x242): stride and declaration from the FVF, like
    // IDirect3DDevice9::SetFVF with a vertex shader bound
    constexpr std::uint32_t kFvf = FVF_XYZ | FVF_DIFFUSE | (2u << FVF_TEXCOUNT_SHIFT);
    struct FvfVertex {
        float x, y, z;
        std::uint32_t color;
        float u0, v0, u1, v1;
    };
    static_assert(sizeof(FvfVertex) == 32);
    const std::uint32_t stride = fvfVertexSize(kFvf);
    if (stride != sizeof(FvfVertex)) {
        std::printf("  FAIL fvf: stride %u, expected %zu\n", stride, sizeof(FvfVertex));
        ++g_failures;
        return;
    }
    auto decl = f.dev.createVertexDecl(fvfDeclaration(kFvf));
    auto vb = f.dev.createBuffer(6 * stride, USAGE_WRITEONLY, POOL_MANAGED, false, false);
    const FvfVertex v[6] = {{-1, 1, 0.5f, 0xFFFF0000, 0, 0, 9, 9}, {0, 1, 0.5f, 0xFFFF0000, 1, 0, 9, 9},
                            {-1, -1, 0.5f, 0xFFFF0000, 0, 1, 9, 9}, {0, 1, 0.5f, 0xFFFF0000, 1, 0, 9, 9},
                            {0, -1, 0.5f, 0xFFFF0000, 1, 1, 9, 9}, {-1, -1, 0.5f, 0xFFFF0000, 0, 1, 9, 9}};
    std::memcpy(vb->lock(0, 0, 0), v, sizeof v);
    vb->unlock();
    f.reset();
    f.dev.setVertexDecl(decl.get());
    f.dev.setStreamSource(0, vb.get(), 0, stride);
    f.dev.drawPrimitive(PT_TRIANGLELIST, 0, 2);
    const Pixels p = capture(f.dev, "fvf");
    expect("fvf: left half drawn", p, 10, 30, 0xFF0000);
    expect("fvf: right half untouched", p, 50, 30, 0x000000);
    f.dev.setStreamSource(0, nullptr, 0, 0);
    f.dev.setVertexDecl(nullptr);
    f.dev.release(std::move(vb));
    f.dev.release(std::move(decl));
}

const char* kPsCube = R"(
ps_3_0
def c0, 1, 0, 0, 0
dcl_texcoord v0.xy
dcl_cube s0
mov r0.xy, v0
mov r0.z, c0.x
texld oC0, r0, s0
)";

void testCubeLock(Fixture& f) {
    // eCAPICubeMap::LockCubeMapLayer: lock one face of a file cube map whose CPU copy was dropped after the upload (read back
    // from that face's layer), then write a face and sample it
    const auto ps = shaderFrom(f.dev, kPsCube);
    if (!ps) {
        ++g_failures;
        return;
    }
    TextureDesc d;
    d.type = TextureType::Cube;
    d.width = d.height = 8;
    d.format = rhi::d3dfmt::A8R8G8B8;
    d.pool = POOL_MANAGED;
    auto cube = f.dev.createTexture(d);
    const std::uint32_t faceColors[6] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFF00FFFF, 0xFFFF00FF};
    for (std::uint32_t face = 0; face < 6; ++face) {
        std::vector<std::uint32_t> px(64, faceColors[face]);
        cube->setLevelData(0, face, {reinterpret_cast<const std::uint8_t*>(px.data()), px.size() * 4}, 8 * 4);
    }
    auto drawCube = [&](const char* name) {
        f.reset();
        f.dev.setPixelShader(ps);
        f.dev.setTexture(0, cube.get());
        f.draw(quad(-1, -1, 1, 1, 0.5f, 0xFFFFFFFF));
        Pixels p = capture(f.dev, name);
        f.dev.setTexture(0, nullptr);
        return p;
    };
    Pixels p = drawCube("cube_lock_1");
    expect("cube: +Z face sampled", p, 32, 32, faceColors[4]);
    Texture::Locked lk;
    if (!cube->lock(0, 2, nullptr, LOCK_READONLY, lk)) {
        std::printf("  FAIL cube lock: face 2 could not be locked\n");
        ++g_failures;
    } else {
        const std::uint32_t got = static_cast<const std::uint32_t*>(lk.bits)[9];
        cube->unlock(0, 2);
        if (got != faceColors[2]) {
            std::printf("  FAIL cube lock: read-only lock of face 2 sees %08X, expected %08X\n", got, faceColors[2]);
            ++g_failures;
        }
    }
    if (cube->lock(0, 4, nullptr, 0, lk)) {
        fillLocked(lk, 0, 0, 0, 0, 8, 8, 0xFF808080);
        cube->unlock(0, 4);
    }
    p = drawCube("cube_lock_2");
    expect("cube: written face sampled", p, 32, 32, 0x808080);
    if (cube->lock(0, 0, nullptr, LOCK_READONLY, lk)) {
        const std::uint32_t got = static_cast<const std::uint32_t*>(lk.bits)[0];
        cube->unlock(0, 0);
        if (got != faceColors[0]) {
            std::printf("  FAIL cube lock: face 0 after writing face 4 is %08X\n", got);
            ++g_failures;
        }
    }
    f.dev.release(std::move(cube));
}

void testLoadSurfaceFromSurface(Fixture& f) {
    // D3DXLoadSurfaceFromSurface(filter NONE) of a DXT1 file texture into an A8R8G8B8 system-memory texture (the sky absorption
    // image): blocks 0-2 red, block 3 blue, decoded into the CPU copy
    TextureDesc d;
    d.width = d.height = 8;
    d.format = rhi::d3dfmt::DXT1;
    auto src = f.dev.createTexture(d);
    std::vector<std::uint8_t> blocks(4 * 8);
    for (int b = 0; b < 4; ++b) {
        const std::uint16_t c0 = b == 3 ? 0x001F : 0xF800, c1 = 0;
        std::memcpy(blocks.data() + b * 8, &c0, 2);
        std::memcpy(blocks.data() + b * 8 + 2, &c1, 2);
    }
    src->setLevelData(0, 0, blocks, 16);
    TextureDesc dd;
    dd.width = dd.height = 8;
    dd.format = rhi::d3dfmt::A8R8G8B8;
    dd.pool = POOL_SYSTEMMEM;
    auto dst = f.dev.createTexture(dd);
    std::string why;
    if (!f.dev.loadSurfaceFromSurface(dst.get(), 0, nullptr, src.get(), 0, nullptr, false, &why)) {
        std::printf("  FAIL load surface: %s\n", why.c_str());
        ++g_failures;
    } else {
        Texture::Locked lk;
        dst->lock(0, 0, nullptr, LOCK_READONLY, lk);
        const auto at = [&](int x, int y) {
            return reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(lk.bits) + y * lk.rowPitch)[x];
        };
        const std::uint32_t a = at(1, 1), b = at(6, 6);
        dst->unlock(0, 0);
        if (!similar(a, 0xFF0000, 8) || !similar(b, 0x0000FF, 8) || (a >> 24) != 0xFF) {
            std::printf("  FAIL load surface: texels %08X / %08X, expected FFFF0000 / FF0000FF\n", a, b);
            ++g_failures;
        }
    }
    // a sub-rectangle: the blue block alone, scaled into the top-left 2x2 of a cleared destination
    Texture::Locked lk;
    dst->lock(0, 0, nullptr, LOCK_DISCARD, lk);
    for (int y = 0; y < 8; ++y) std::memset(static_cast<std::uint8_t*>(lk.bits) + y * lk.rowPitch, 0, 32);
    dst->unlock(0, 0);
    const std::int32_t sr[4] = {4, 4, 8, 8}, dr[4] = {0, 0, 2, 2};
    if (!f.dev.loadSurfaceFromSurface(dst.get(), 0, dr, src.get(), 0, sr, false, &why)) {
        std::printf("  FAIL load surface (rects): %s\n", why.c_str());
        ++g_failures;
    } else {
        dst->lock(0, 0, nullptr, LOCK_READONLY, lk);
        const auto* row0 = static_cast<const std::uint32_t*>(lk.bits);
        const auto* row1 = reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(lk.bits) + lk.rowPitch);
        const std::uint32_t in = row1[1], out = row0[2];
        dst->unlock(0, 0);
        if (!similar(in, 0x0000FF, 8) || out != 0) {
            std::printf("  FAIL load surface (rects): inside %08X, outside %08X\n", in, out);
            ++g_failures;
        }
    }
    f.dev.release(std::move(dst));
    f.dev.release(std::move(src));
}


// F2b-1 scene draws (frame-ownership.md 7.11) through the device's own path: eight quads drawn as indexed static-mesh draws of the main view's solid pass,
// with scene draws off (reference) and on (one indirect call over eight records); the same with a normal draw in the middle of the batch (a stale scene
// packet must not leak into it) and with the null-address guard forced (every scene draw must fall back, the picture stays the same)
void testSceneDraws(Fixture& f) {
    Device& dev = f.dev;
    dev.setSceneDraws(true, 0);
    if (!dev.sceneDrawsActive()) {
        std::printf("  SKIP scene draws: not available on this device\n");
        return;
    }
    constexpr std::uint32_t kQuads = 8;
    std::vector<Vertex> verts;
    for (std::uint32_t k = 0; k < kQuads; ++k) {
        const float x0 = -1.0f + 0.25f * static_cast<float>(k), x1 = x0 + 0.25f;
        const std::uint32_t color = 0xFF000000u | (k * 30u + 20u) << 16 | (255u - k * 25u) << 8 | (k & 1u ? 200u : 40u);
        verts.push_back({x0, 0.5f, 0.5f, 1, color, 0, 0});
        verts.push_back({x1, 0.5f, 0.5f, 1, color, 1, 0});
        verts.push_back({x0, -0.5f, 0.5f, 1, color, 0, 1});
        verts.push_back({x1, -0.5f, 0.5f, 1, color, 1, 1});
    }
    const std::uint16_t idx[6] = {0, 1, 2, 1, 3, 2};
    auto vb = dev.createBuffer(static_cast<std::uint32_t>(verts.size() * sizeof(Vertex)), USAGE_WRITEONLY, POOL_MANAGED, false, false);
    auto ib = dev.createBuffer(sizeof(idx), USAGE_WRITEONLY, POOL_MANAGED, true, false);
    std::memcpy(vb->lock(0, 0, 0), verts.data(), verts.size() * sizeof(Vertex));
    vb->unlock();
    std::memcpy(ib->lock(0, 0, 0), idx, sizeof(idx));
    ib->unlock();
    // mode 0: scene on for every quad; 1: quad 3 drawn normally (inside the batch); 2: the guard forced
    const auto render = [&](bool scene, int mode) {
        f.reset();
        dev.drawTag() = {};
        dev.drawTag().source = draws::Source::Mesh;
        dev.phases().push(scene::View::Main, scene::Pass::Solid);
        dev.setStreamSource(0, vb.get(), 0, sizeof(Vertex));
        dev.setIndices(ib.get());
        dev.setSceneFaultInject(mode == 2);
        for (std::uint32_t k = 0; k < kQuads; ++k) {
            dev.setSceneIssue(scene && !(mode == 1 && k == 3));
            dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
        }
        dev.setSceneIssue(false);
        dev.setSceneFaultInject(false);
        dev.phases().pop();
        return capture(dev, scene ? "scene" : "scene_ref");
    };
    const Pixels ref = render(false, 0);
    const auto same = [&](const Pixels& p, const char* what) {
        std::uint32_t bad = 0;
        for (std::size_t i = 0; i < ref.bgra.size() && i < p.bgra.size(); ++i) bad += ref.bgra[i] != p.bgra[i];
        if (bad != 0 || ref.bgra.size() != p.bgra.size()) {
            std::printf("  FAIL scene draws (%s): %u bytes differ from the normal draws\n", what, bad);
            ++g_failures;
        }
    };
    expect("scene reference quad 0", ref, 2, 32, 0xFF000000u | (20u << 16) | (255u << 8) | 40u);
    Pixels first;
    bool ready = false;
    for (int i = 0; i < 1200 && !ready; ++i) {
        const auto before = dev.sceneStats();
        first = render(true, 0);
        ready = dev.sceneStats().sceneDraws - before.sceneDraws == kQuads;
        if (!ready) Sleep(50);  // (the scene shaders / pipelines are built in the background; those draws were normal)
    }
    if (!ready) {
        std::printf("  FAIL scene draws: the scene variants never became ready\n");
        ++g_failures;
        return;
    }
    same(first, "all scene");
    const auto s0 = dev.sceneStats();
    same(render(true, 0), "all scene, again");
    const auto s1 = dev.sceneStats();
    if (s1.sceneDraws - s0.sceneDraws != kQuads || s1.indirectCalls - s0.indirectCalls != 1 || s1.records - s0.records != kQuads) {
        std::printf("  FAIL scene draws: %llu scene draws / %llu indirect calls / %llu records for %u quads (expected one call over all)\n",
                    static_cast<unsigned long long>(s1.sceneDraws - s0.sceneDraws), static_cast<unsigned long long>(s1.indirectCalls - s0.indirectCalls),
                    static_cast<unsigned long long>(s1.records - s0.records), kQuads);
        ++g_failures;
    }
    // a normal draw in the middle of the batch: no stale record index may leak into it (the bug of the first version)
    same(render(true, 1), "a normal draw inside the batch");
    const auto s2 = dev.sceneStats();
    if (s2.sceneDraws - s1.sceneDraws != kQuads - 1) {
        std::printf("  FAIL scene draws: %llu scene draws with one quad drawn normally\n", static_cast<unsigned long long>(s2.sceneDraws - s1.sceneDraws));
        ++g_failures;
    }
    // the guard: a null address refuses every scene draw, the normal path draws them
    same(render(true, 2), "null address guard");
    const auto s3 = dev.sceneStats();
    if (s3.sceneDraws != s2.sceneDraws || s3.fbNullAddress - s2.fbNullAddress != kQuads) {
        std::printf("  FAIL scene draws: the guard let %llu scene draws through and refused %llu of %u\n", static_cast<unsigned long long>(s3.sceneDraws - s2.sceneDraws),
                    static_cast<unsigned long long>(s3.fbNullAddress - s2.fbNullAddress), kQuads);
        ++g_failures;
    }
    // and scene draws work again after the refusals
    same(render(true, 0), "scene again");
    dev.setSceneDraws(false, 0);
    dev.release(std::move(vb));
    dev.release(std::move(ib));
}

// F2c-3 owned replay (frame-ownership.md 7.16): eight quads drawn normally (reference), captured as retained draws, then submitted through Device::replayOwned
// (the DrawBlock derived, the windows from the caller, no register file) as scene draws: one indirect call over eight records, 0 bytes differ; every refusal
// (null address guard, a shader that is not bound, a stream that is not the recorded one) leaves nothing behind and the next replays work again
void testOwnedReplay(Fixture& f) {
    Device& dev = f.dev;
    dev.setSceneDraws(true, 0);
    if (!dev.sceneDrawsActive()) {
        std::printf("  SKIP owned replay: scene draws are not available on this device\n");
        return;
    }
    constexpr std::uint32_t kQuads = 8;
    std::vector<Vertex> verts;
    for (std::uint32_t k = 0; k < kQuads; ++k) {
        const float x0 = -1.0f + 0.25f * static_cast<float>(k), x1 = x0 + 0.25f;
        const std::uint32_t color = 0xFF000000u | (k * 30u + 20u) << 16 | (255u - k * 25u) << 8 | (k & 1u ? 200u : 40u);
        verts.push_back({x0, 0.5f, 0.5f, 1, color, 0, 0});
        verts.push_back({x1, 0.5f, 0.5f, 1, color, 1, 0});
        verts.push_back({x0, -0.5f, 0.5f, 1, color, 0, 1});
        verts.push_back({x1, -0.5f, 0.5f, 1, color, 1, 1});
    }
    const std::uint16_t idx[6] = {0, 1, 2, 1, 3, 2};
    auto vb = dev.createBuffer(static_cast<std::uint32_t>(verts.size() * sizeof(Vertex)), USAGE_WRITEONLY, POOL_MANAGED, false, false);
    auto ib = dev.createBuffer(sizeof(idx), USAGE_WRITEONLY, POOL_MANAGED, true, false);
    auto other = dev.createBuffer(static_cast<std::uint32_t>(verts.size() * sizeof(Vertex)), USAGE_WRITEONLY, POOL_MANAGED, false, false);
    std::memcpy(vb->lock(0, 0, 0), verts.data(), verts.size() * sizeof(Vertex));
    vb->unlock();
    std::memcpy(other->lock(0, 0, 0), verts.data(), verts.size() * sizeof(Vertex));
    other->unlock();
    std::memcpy(ib->lock(0, 0, 0), idx, sizeof(idx));
    ib->unlock();
    const auto begin = [&] {
        f.reset();
        dev.drawTag() = {};
        dev.drawTag().source = draws::Source::Mesh;
        dev.phases().push(scene::View::Main, scene::Pass::Solid);
        dev.setStreamSource(0, vb.get(), 0, sizeof(Vertex));
        dev.setIndices(ib.get());
    };
    const auto end = [&](const char* name) {
        dev.setSceneIssue(false);
        dev.setSceneFaultInject(false);
        dev.phases().pop();
        return capture(dev, name);
    };
    // the reference: normal draws
    begin();
    for (std::uint32_t k = 0; k < kQuads; ++k) dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
    const Pixels ref = end("owned_ref");
    expect("owned reference quad 0", ref, 2, 32, 0xFF000000u | (20u << 16) | (255u << 8) | 40u);
    const auto same = [&](const Pixels& p, const char* what) {
        std::uint32_t bad = 0;
        for (std::size_t i = 0; i < ref.bgra.size() && i < p.bgra.size(); ++i) bad += ref.bgra[i] != p.bgra[i];
        if (bad != 0 || ref.bgra.size() != p.bgra.size()) {
            std::printf("  FAIL owned replay (%s): %u bytes differ from the normal draws\n", what, bad);
            ++g_failures;
        }
    };
    // the retained draws: made as scene draws once their variants / pipelines exist
    std::vector<Device::CachedDraw> cd(kQuads);
    bool ready = false;
    for (int i = 0; i < 1200 && !ready; ++i) {
        begin();
        dev.setSceneIssue(true);
        for (std::uint32_t k = 0; k < kQuads; ++k) {
            dev.beginCapture(&cd[k]);
            dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
            dev.endCapture();
        }
        end("owned_capture");
        ready = true;
        for (const auto& c : cd) ready = ready && c.valid && c.scenePipeline != GamePipeline::None;
        if (!ready) Sleep(50);
    }
    if (!ready) {
        std::printf("  FAIL owned replay: the draws were never captured as scene draws\n");
        ++g_failures;
        return;
    }
    const OwnedWindows none;  // (the test shaders read no register window)
    const auto replay = [&](std::uint32_t k, Buffer* stream) {
        Device::DeriveIn in;
        in.streams[0] = stream;
        in.strides[0] = sizeof(Vertex);
        in.indices = ib.get();
        return dev.replayOwned(cd[k], in, none);
    };
    const auto all = [&](const char* name, int expectRc, Buffer* stream) {
        begin();
        dev.setSceneIssue(true);
        for (std::uint32_t k = 0; k < kQuads; ++k) {
            const int rc = replay(k, stream);
            if (rc != expectRc) {
                std::printf("  FAIL owned replay (%s): quad %u returned %d, expected %d\n", name, k, rc, expectRc);
                ++g_failures;
            }
        }
        return end(name);
    };
    const auto s0 = dev.sceneStats();
    const auto o0 = dev.ownedReplayStats;
    const Pixels first = all("owned_replay", 0, vb.get());
    same(first, "eight owned draws");
    const auto s1 = dev.sceneStats();
    if (s1.indirectCalls - s0.indirectCalls != 1 || s1.records - s0.records != kQuads || dev.ownedReplayStats.drawn - o0.drawn != kQuads) {
        std::printf("  FAIL owned replay: %llu indirect calls / %llu records / %llu owned draws for %u quads (expected one call over all)\n",
                    static_cast<unsigned long long>(s1.indirectCalls - s0.indirectCalls), static_cast<unsigned long long>(s1.records - s0.records),
                    static_cast<unsigned long long>(dev.ownedReplayStats.drawn - o0.drawn), kQuads);
        ++g_failures;
    }
    std::printf("  owned replay: %llu owned draws in %llu indirect call(s) over %llu records, %d refusals so far\n", static_cast<unsigned long long>(dev.ownedReplayStats.drawn - o0.drawn), static_cast<unsigned long long>(s1.indirectCalls - s0.indirectCalls), static_cast<unsigned long long>(s1.records - s0.records), static_cast<int>(dev.ownedReplayStats.attempts - dev.ownedReplayStats.drawn));
    // refusals: nothing may be queued (the picture is the cleared target), the reason is counted
    {
        begin();
        dev.setSceneIssue(true);
        dev.setSceneFaultInject(true);
        int refused = 0;
        for (std::uint32_t k = 0; k < kQuads; ++k) refused += replay(k, vb.get()) == 15;
        const Pixels p = end("owned_guard");
        if (refused != static_cast<int>(kQuads) || p.bgra[(32 * p.w + 2) * 4 + 1] != 0) {
            std::printf("  FAIL owned replay: the null address guard refused %d of %u and drew something\n", refused, kQuads);
            ++g_failures;
        }
    }
    {
        // a stream that is not the recorded one: refused as a binding change (6), nothing drawn
        begin();
        dev.setSceneIssue(true);
        int refused = 0;
        for (std::uint32_t k = 0; k < kQuads; ++k) refused += replay(k, other.get()) == 6;
        const Pixels p = end("owned_binding");
        if (refused != static_cast<int>(kQuads) || p.bgra[(32 * p.w + 2) * 4 + 1] != 0) {
            std::printf("  FAIL owned replay: a changed stream refused %d of %u and drew something\n", refused, kQuads);
            ++g_failures;
        }
    }
    {
        // the shaders of the record are not the bound ones: refused (11)
        begin();
        dev.setSceneIssue(true);
        dev.setVertexShader(f.vsInst);
        const int rc = replay(0, vb.get());
        end("owned_shader");
        if (rc != 11) {
            std::printf("  FAIL owned replay: another vertex shader returned %d, expected 11\n", rc);
            ++g_failures;
        }
    }
    // and the owned draws work again after the refusals
    same(all("owned_replay_again", 0, vb.get()), "owned draws after the refusals");
    dev.setSceneDraws(false, 0);
    dev.release(std::move(vb));
    dev.release(std::move(ib));
    dev.release(std::move(other));
}

// F2c-4-0 owned replay on scene ABI v2 (frame-ownership.md 7.19): the same eight quads, drawn (a) normally with the registers set (the reference), (b) as owned draws
// whose VIEW registers (VS c0 / c12, PS c0 / c5.xyz, posFix) come from ViewBlock2 slots of a view table the test owns (a host-visible buffer + its CPU twin: the
// device's setSceneViewTable). Checked: 0 bytes differ, one indirect call over the eight records; two view slots (4 quads each, other registers) = two calls and
// the reference's picture; a table slot whose bytes differ from the registers changes the picture (the draws read it); a stale posFix is refused (19), a window
// that is not the slot's (18), a slot out of range (17), a missing table / no v2 pipeline (16); every refusal draws nothing and v2 works again afterwards
void testOwnedReplayV2(Fixture& f) {
    Device& dev = f.dev;
    dev.setSceneDraws(true, 0);
    if (!dev.sceneDrawsActive()) {
        std::printf("  SKIP owned replay v2: scene draws are not available on this device\n");
        return;
    }
    static const char* kVs = R"(
vs_3_0
dcl_position v0
dcl_color v1
dcl_position o0
dcl_color o1
mov o0, v0
add o1, v1, c0
add o1, o1, c12
)";
    static const char* kPs = R"(
ps_3_0
dcl_color v0
add r0, v0, c0
mul r0, r0, c5
mov oC0, r0
)";
    const auto vsV = shaderFrom(dev, kVs);
    const auto psV = shaderFrom(dev, kPs);
    if (!vsV || !psV) {
        std::printf("  FAIL owned replay v2: shaders\n");
        ++g_failures;
        return;
    }
    constexpr std::uint32_t kQuads = 8, kSlots = 16;
    std::vector<Vertex> verts;
    for (std::uint32_t k = 0; k < kQuads; ++k) {
        const float x0 = -1.0f + 0.25f * static_cast<float>(k), x1 = x0 + 0.25f;
        const std::uint32_t color = 0xFF000000u | (k * 6u + 10u) << 16 | (30u + k * 4u) << 8 | (k & 1u ? 40u : 20u);
        verts.push_back({x0, 0.5f, 0.5f, 1, color, 0, 0});
        verts.push_back({x1, 0.5f, 0.5f, 1, color, 1, 0});
        verts.push_back({x0, -0.5f, 0.5f, 1, color, 0, 1});
        verts.push_back({x1, -0.5f, 0.5f, 1, color, 1, 1});
    }
    const std::uint16_t idx[6] = {0, 1, 2, 1, 3, 2};
    auto vb = dev.createBuffer(static_cast<std::uint32_t>(verts.size() * sizeof(Vertex)), USAGE_WRITEONLY, POOL_MANAGED, false, false);
    auto ib = dev.createBuffer(sizeof(idx), USAGE_WRITEONLY, POOL_MANAGED, true, false);
    std::memcpy(vb->lock(0, 0, 0), verts.data(), verts.size() * sizeof(Vertex));
    vb->unlock();
    std::memcpy(ib->lock(0, 0, 0), idx, sizeof(idx));
    ib->unlock();
    // the view table: kSlots ViewBlock2 in a host-visible buffer (+ the CPU twin the device compares posFix / c5 with)
    rhi::Device* rd = dev.rhi();
    auto tableBuf = rd->createBuffer({static_cast<std::uint64_t>(kSlots) * shader::abi::kViewBlock2Size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "owned replay v2 view table"});
    if (!tableBuf) {
        std::printf("  FAIL owned replay v2: view table buffer\n");
        ++g_failures;
        return;
    }
    const auto tw = tableBuf->shaderRef(0);
    const rhi::GpuRef tableRef = rhi::GpuRef::fromWords(tw[0], tw[1]);
    std::vector<shader::abi::ViewBlock2> twin(kSlots);
    std::memset(twin.data(), 0, twin.size() * sizeof(shader::abi::ViewBlock2));
    const auto upload = [&] { std::memcpy(tableBuf->map(), twin.data(), twin.size() * sizeof(shader::abi::ViewBlock2)); };
    // the registers of view `v` (0 / 1): VS c0, c12; PS c0, c5 (xyz = the view's, w = the draw's own)
    struct Regs {
        float vs0[4], vs12[4], ps0[4], ps5[4];
    };
    const Regs regs[2] = {{{0.10f, 0.05f, 0.02f, 0.0f}, {0.02f, 0.10f, 0.05f, 0.0f}, {0.30f, 0.20f, 0.10f, 0.0f}, {0.9f, 0.8f, 0.7f, 0.5f}},
                          {{0.02f, 0.12f, 0.06f, 0.0f}, {0.12f, 0.02f, 0.03f, 0.0f}, {0.05f, 0.25f, 0.30f, 0.0f}, {0.6f, 1.0f, 0.5f, 0.5f}}};
    const auto setRegs = [&](const Regs& r) {
        dev.setVertexShaderConstantF(0, r.vs0, 1);
        dev.setVertexShaderConstantF(12, r.vs12, 1);
        dev.setPixelShaderConstantF(0, r.ps0, 1);
        dev.setPixelShaderConstantF(5, r.ps5, 1);
    };
    // slot `slot` of the table = view `v`'s registers + the device's current posFix
    const auto fillSlot = [&](std::uint32_t slot, const Regs& r, bool stalePosFix) {
        shader::abi::ViewBlock2& b = twin[slot];
        std::memset(&b, 0, sizeof(b));
        std::memcpy(b.vs0[0], r.vs0, 16);
        std::memcpy(b.vs1[0], r.vs12, 16);
        std::memcpy(b.ps0[0], r.ps0, 16);
        dev.viewPosFix(b.posFix);
        if (stalePosFix) b.posFix[0] += 0.001f;
        std::memcpy(b.c5, r.ps5, 3 * sizeof(float));
        upload();
    };
    const auto windowsOf = [&](std::uint32_t slot) {
        OwnedWindows ow;
        const auto set = [&](OwnedWindows::Win& w, bool vertex, std::uint32_t win, std::uint32_t covered) {
            w.kind = OwnedWindows::Kind::View;
            w.base = rhi::GpuRef{gpuscene::ViewTable::windowBase(tableRef, slot, vertex, win)};
            w.covered = covered;
        };
        set(ow.w[0][0], true, 0, 12);
        set(ow.w[0][1], true, 1, 19);
        set(ow.w[1][0], false, 0, 5);
        ow.viewSlot = slot;
        return ow;
    };
    const auto begin = [&] {
        f.reset();
        dev.drawTag() = {};
        dev.drawTag().source = draws::Source::Mesh;
        dev.phases().push(scene::View::Main, scene::Pass::Solid);
        dev.setVertexShader(vsV);
        dev.setPixelShader(psV);
        dev.setStreamSource(0, vb.get(), 0, sizeof(Vertex));
        dev.setIndices(ib.get());
    };
    const auto end = [&](const char* name) {
        dev.setSceneIssue(false);
        dev.setSceneFaultInject(false);
        dev.phases().pop();
        return capture(dev, name);
    };
    const auto same = [&](const Pixels& ref, const Pixels& p, const char* what) {
        std::uint32_t bad = 0;
        for (std::size_t i = 0; i < ref.bgra.size() && i < p.bgra.size(); ++i) bad += ref.bgra[i] != p.bgra[i];
        if (bad != 0 || ref.bgra.size() != p.bgra.size()) {
            std::printf("  FAIL owned replay v2 (%s): %u bytes differ from the normal draws\n", what, bad);
            ++g_failures;
        }
        return bad;
    };
    // references: all eight quads with view 0; four with view 0 then four with view 1
    begin();
    setRegs(regs[0]);
    for (std::uint32_t k = 0; k < kQuads; ++k) dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
    const Pixels ref1 = end("ownedv2_ref1");
    begin();
    for (std::uint32_t k = 0; k < kQuads; ++k) {
        setRegs(regs[k < 4 ? 0 : 1]);
        dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
    }
    const Pixels ref2 = end("ownedv2_ref2");
    // the retained draws, with the v2 pipelines once they exist (the view table is announced first: the v2 form is decided only while it is on)
    dev.setSceneViewTable(tableRef, twin.data(), kSlots);
    dev.setSceneViews(true);
    std::vector<Device::CachedDraw> cd(kQuads);
    bool ready = false;
    for (int i = 0; i < 1200 && !ready; ++i) {
        begin();
        setRegs(regs[0]);
        dev.setSceneIssue(true);
        for (std::uint32_t k = 0; k < kQuads; ++k) {
            dev.beginCapture(&cd[k]);
            dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
            dev.endCapture();
        }
        end("ownedv2_capture");
        ready = true;
        for (const auto& c : cd) ready = ready && c.valid && c.scenePipeline != GamePipeline::None && c.scenePipeline2 != GamePipeline::None;
        if (!ready) Sleep(50);
    }
    if (!ready) {
        std::printf("  FAIL owned replay v2: the draws never got their v2 pipelines\n");
        ++g_failures;
        return;
    }
    const auto replay = [&](std::uint32_t k, const OwnedWindows& ow) {
        Device::DeriveIn in;
        in.streams[0] = vb.get();
        in.strides[0] = sizeof(Vertex);
        in.indices = ib.get();
        (void)k;
        const Regs& r = regs[ow.viewSlot == 9 ? 1 : 0];
        std::memcpy(in.psC5, r.ps5, sizeof(in.psC5));  // (xyz = the view's, w = the draw's own)
        return dev.replayOwned(cd[k], in, ow);
    };
    const auto run = [&](const char* name, std::uint32_t slotA, std::uint32_t slotB, int expectRc, const char* label) {
        begin();
        dev.setSceneIssue(true);
        const OwnedWindows a = windowsOf(slotA), b = windowsOf(slotB);
        int refused = 0;
        for (std::uint32_t k = 0; k < kQuads; ++k) {
            const int rc = replay(k, k < 4 ? a : b);
            refused += rc == expectRc;
            if (rc != expectRc) {
                std::printf("  FAIL owned replay v2 (%s): quad %u returned %d, expected %d\n", label, k, rc, expectRc);
                ++g_failures;
            }
        }
        (void)refused;
        return end(name);
    };
    fillSlot(3, regs[0], false);
    fillSlot(9, regs[1], false);
    // 1. all eight on view slot 3 (the registers of view 0): one indirect call over eight records
    const auto s0 = dev.sceneStats();
    const auto o0 = dev.ownedReplayStats;
    same(ref1, run("ownedv2_one", 3, 3, 0, "one view slot"), "eight owned draws, one view slot");
    const auto s1 = dev.sceneStats();
    if (s1.indirectCalls - s0.indirectCalls != 1 || s1.records - s0.records != kQuads || dev.ownedReplayStats.drawnV2 - o0.drawnV2 != kQuads) {
        std::printf("  FAIL owned replay v2: %llu indirect calls / %llu records / %llu v2 draws for %u quads (expected one call over all)\n", static_cast<unsigned long long>(s1.indirectCalls - s0.indirectCalls),
                    static_cast<unsigned long long>(s1.records - s0.records), static_cast<unsigned long long>(dev.ownedReplayStats.drawnV2 - o0.drawnV2), kQuads);
        ++g_failures;
    }
    // 1b. F-VAR (frame-ownership.md 7.44): camera jitter (upscaling) on the main view's geometry: the draws' posFix and the view slot's carry the jitter, the owned draws must equal the
    // normally drawn jittered draws (the picture differs from the unjittered reference: the test is not vacuous)
    {
        Texture* hdr = dev.backBuffer()->texture();
        dev.tagImage(hdr, scene::ImageRole::SceneHdr);
        dev.setJitterSource([](std::uint64_t) { return std::array<float, 2>{0.37f, -0.21f}; });
        dev.beginMainView();
        float jx = 0.0f, jy = 0.0f;
        dev.phases().push(scene::View::Main, scene::Pass::Solid);
        dev.geometryJitter(jx, jy);
        dev.phases().pop();
        if (jx == 0.0f && jy == 0.0f) {
            std::printf("  SKIP owned replay v2 jitter: the camera jitter did not reach the main view's geometry here\n");
        } else {
            begin();
            setRegs(regs[0]);
            for (std::uint32_t k = 0; k < kQuads; ++k) dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
            const Pixels refJ = end("ownedv2_refjit");
            std::uint32_t moved = 0;
            for (std::size_t i = 0; i < ref1.bgra.size() && i < refJ.bgra.size(); ++i) moved += ref1.bgra[i] != refJ.bgra[i];
            if (moved == 0) {
                std::printf("  FAIL owned replay v2 jitter: the jitter did not change the normal draws\n");
                ++g_failures;
            }
            dev.phases().push(scene::View::Main, scene::Pass::Solid);
            fillSlot(3, regs[0], false);
            dev.phases().pop();
            const auto j0 = dev.ownedReplayStats;
            same(refJ, run("ownedv2_jit", 3, 3, 0, "jittered"), "eight owned draws under camera jitter");
            if (dev.ownedReplayStats.drawnV2 - j0.drawnV2 != kQuads) {
                std::printf("  FAIL owned replay v2 jitter: %llu of %u draws were owned\n", static_cast<unsigned long long>(dev.ownedReplayStats.drawnV2 - j0.drawnV2), kQuads);
                ++g_failures;
            }
            // a slot written without the jitter (the jitter moved after it was written): stale posFix, refused (19), nothing drawn
            fillSlot(3, regs[0], false);
            (void)run("ownedv2_jit_stale", 3, 3, 19, "jitter not in the slot");
            std::printf("  owned replay v2 jitter: %u bytes moved by the jitter, owned draws equal\n", moved);
            // 1c. F-VAR2 (frame-ownership.md 7.45): upscale.cutout_thicken under jitter. The thickened cut-out is a flag word in the DrawBlock's alphaFunc (constant per environment);
            // an owned draw must carry it like the normal draw does. Quads with an alpha gradient + a GREATER alpha test: the threshold drops where the alpha changes fast
            {
                std::vector<Vertex> v2 = verts;
                for (std::uint32_t k = 0; k < kQuads; ++k) {
                    for (std::uint32_t j = 0; j < 4; ++j) {
                        Vertex& v = v2[4 * k + j];
                        v.color = (v.color & 0x00FFFFFFu) | ((j & 1u) != 0 ? 0xFF000000u : 0x00000000u);
                    }
                }
                auto vb2 = dev.createBuffer(static_cast<std::uint32_t>(v2.size() * sizeof(Vertex)), USAGE_WRITEONLY, POOL_MANAGED, false, false);
                std::memcpy(vb2->lock(0, 0, 0), v2.data(), v2.size() * sizeof(Vertex));
                vb2->unlock();
                const auto alphaTest = [&](bool on) {
                    dev.setRenderState(RS_ALPHATESTENABLE, on ? 1 : 0);
                    dev.setRenderState(RS_ALPHAFUNC, CMP_GREATER);
                    dev.setRenderState(RS_ALPHAREF, 0x40);
                };
                const auto draws2 = [&](const char* name) {
                    begin();
                    dev.setStreamSource(0, vb2.get(), 0, sizeof(Vertex));
                    alphaTest(true);
                    setRegs(regs[0]);
                    for (std::uint32_t k = 0; k < kQuads; ++k) dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
                    const Pixels p = end(name);
                    alphaTest(false);
                    return p;
                };
                dev.setCutoutThicken(0.0f);
                dev.setRenderRatio(1.0f);
                const Pixels plain = draws2("ownedv2_thick_plain");
                dev.setRenderRatio(2.0f);
                dev.setCutoutThicken(1.0f);
                const Pixels thick = draws2("ownedv2_thick_ref");
                std::uint32_t changed = 0;
                for (std::size_t i = 0; i < plain.bgra.size() && i < thick.bgra.size(); ++i) changed += plain.bgra[i] != thick.bgra[i];
                if (changed == 0) {
                    std::printf("  FAIL owned replay v2 thicken: the thickened cut-out did not change the normal draws\n");
                    ++g_failures;
                }
                std::vector<Device::CachedDraw> cd2(kQuads);
                bool ready2 = false;
                for (int i = 0; i < 1200 && !ready2; ++i) {
                    begin();
                    dev.setStreamSource(0, vb2.get(), 0, sizeof(Vertex));
                    alphaTest(true);
                    setRegs(regs[0]);
                    dev.setSceneIssue(true);
                    for (std::uint32_t k = 0; k < kQuads; ++k) {
                        dev.beginCapture(&cd2[k]);
                        dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
                        dev.endCapture();
                    }
                    end("ownedv2_thick_capture");
                    alphaTest(false);
                    ready2 = true;
                    for (const auto& c : cd2) ready2 = ready2 && c.valid && c.scenePipeline != GamePipeline::None && c.scenePipeline2 != GamePipeline::None;
                    if (!ready2) Sleep(50);
                }
                if (!ready2) {
                    std::printf("  FAIL owned replay v2 thicken: the draws never got their v2 pipelines\n");
                    ++g_failures;
                } else {
                    dev.phases().push(scene::View::Main, scene::Pass::Solid);
                    fillSlot(3, regs[0], false);
                    dev.phases().pop();
                    const auto t0 = dev.ownedReplayStats;
                    begin();
                    dev.setStreamSource(0, vb2.get(), 0, sizeof(Vertex));
                    dev.setSceneIssue(true);
                    const OwnedWindows ow = windowsOf(3);
                    for (std::uint32_t k = 0; k < kQuads; ++k) {
                        Device::DeriveIn in;
                        in.streams[0] = vb2.get();
                        in.strides[0] = sizeof(Vertex);
                        in.indices = ib.get();
                        in.alphaFunc = CMP_GREATER;
                        in.alphaRef = static_cast<float>(0x40);
                        std::memcpy(in.psC5, regs[0].ps5, sizeof(in.psC5));
                        const int rc = dev.replayOwned(cd2[k], in, ow);
                        if (rc != 0) {
                            std::printf("  FAIL owned replay v2 thicken: quad %u returned %d\n", k, rc);
                            ++g_failures;
                        }
                    }
                    same(thick, end("ownedv2_thick_owned"), "eight owned draws with the thickened cut-out");
                    if (dev.ownedReplayStats.drawnV2 - t0.drawnV2 != kQuads) {
                        std::printf("  FAIL owned replay v2 thicken: %llu of %u draws were owned\n", static_cast<unsigned long long>(dev.ownedReplayStats.drawnV2 - t0.drawnV2), kQuads);
                        ++g_failures;
                    }
                    std::printf("  owned replay v2 thicken: %u bytes moved by the thickened cut-out, owned draws equal\n", changed);
                }
                dev.setCutoutThicken(0.0f);
                dev.setRenderRatio(1.0f);
            }
        }
        dev.setJitterSource(nullptr);
        dev.tagImage(hdr, scene::ImageRole::None);
        fillSlot(3, regs[0], false);
    }
    // 2. two view slots: 3 for the first four quads, 9 for the last four: two calls, the second picture
    {
        const auto t0 = dev.sceneStats();
        same(ref2, run("ownedv2_two", 3, 9, 0, "two view slots"), "two view slots");
        const auto t1 = dev.sceneStats();
        if (t1.indirectCalls - t0.indirectCalls != 2) {
            std::printf("  FAIL owned replay v2: two view slots made %llu indirect calls (expected 2)\n", static_cast<unsigned long long>(t1.indirectCalls - t0.indirectCalls));
            ++g_failures;
        }
    }
    // 3. a table slot whose bytes are not the registers: the picture must change (the view is really read)
    {
        fillSlot(3, regs[1], false);
        const Pixels p = run("ownedv2_wrongbytes", 3, 3, 19, "other bytes in the slot");  // (the c5.xyz guard refuses: the derived inputs say view 0)
        fillSlot(3, regs[0], false);
        (void)p;
    }
    // 4. a stale posFix (the jitter / viewport of the view changed after the slot was written): refused (19), nothing drawn; rewritten: drawn again
    {
        fillSlot(3, regs[0], true);
        const Pixels p = run("ownedv2_stale", 3, 3, 19, "stale posFix");
        if (p.bgra[(32 * p.w + 2) * 4 + 1] != 0) {
            std::printf("  FAIL owned replay v2: a refused draw left something behind\n");
            ++g_failures;
        }
        fillSlot(3, regs[0], false);
        same(ref1, run("ownedv2_fresh", 3, 3, 0, "slot rewritten"), "slot rewritten");
    }
    // 5. windows that are not the slot's (the caller built them for another slot): refused (18)
    {
        begin();
        dev.setSceneIssue(true);
        OwnedWindows bad = windowsOf(3);
        bad.viewSlot = 9;  // (the windows point at slot 3, the packet would read slot 9)
        int refused = 0;
        for (std::uint32_t k = 0; k < kQuads; ++k) refused += replay(k, bad) == 18;
        end("ownedv2_windows");
        if (refused != static_cast<int>(kQuads)) {
            std::printf("  FAIL owned replay v2: windows of another slot refused %d of %u (code 18)\n", refused, kQuads);
            ++g_failures;
        }
    }
    // 6. a slot out of range: 17 (above the table / above the device's ref streams)
    {
        begin();
        dev.setSceneIssue(true);
        OwnedWindows over = windowsOf(40);
        int refused = 0;
        for (std::uint32_t k = 0; k < kQuads; ++k) refused += replay(k, over) == 17;
        end("ownedv2_range");
        if (refused != static_cast<int>(kQuads)) {
            std::printf("  FAIL owned replay v2: a slot out of range refused %d of %u (code 17)\n", refused, kQuads);
            ++g_failures;
        }
    }
    // 7. the scene packet's guard (a null address): refused (15)
    {
        begin();
        dev.setSceneIssue(true);
        dev.setSceneFaultInject(true);
        const OwnedWindows a = windowsOf(3);
        int refused = 0;
        for (std::uint32_t k = 0; k < kQuads; ++k) refused += replay(k, a) == 15;
        end("ownedv2_guard");
        if (refused != static_cast<int>(kQuads)) {
            std::printf("  FAIL owned replay v2: the null address guard refused %d of %u\n", refused, kQuads);
            ++g_failures;
        }
    }
    // 8. a null view table (`views` = 0): refused (16), nothing drawn; announced again: owned v2 draws work
    {
        dev.setSceneViewTable(rhi::GpuRef{}, nullptr, 0);
        const OwnedWindows a = windowsOf(3);
        begin();
        dev.setSceneIssue(true);
        int refused = 0;
        for (std::uint32_t k = 0; k < kQuads; ++k) refused += replay(k, a) == 16;
        const Pixels p = end("ownedv2_noviews");
        if (refused != static_cast<int>(kQuads) || p.bgra[(32 * p.w + 2) * 4 + 1] != 0) {
            std::printf("  FAIL owned replay v2: a null view table refused %d of %u (code 16) and drew something\n", refused, kQuads);
            ++g_failures;
        }
        dev.setSceneViewTable(tableRef, twin.data(), kSlots);
        same(ref1, run("ownedv2_again", 3, 3, 0, "after the refusals"), "owned v2 draws after the refusals");
    }
    std::printf("  owned replay v2: %llu v2 draws drawn, %d refusals in all\n", static_cast<unsigned long long>(dev.ownedReplayStats.drawnV2),
                static_cast<int>(dev.ownedReplayStats.attempts - dev.ownedReplayStats.drawn));
    dev.setSceneViews(false);
    dev.setSceneDraws(false, 0);
    dev.release(std::move(vb));
    dev.release(std::move(ib));
}

// F2c-4b: a bulk observer for the owned-draw lists (DrawObserver::bulkOwned / observeOwned; frame-ownership.md 7.23). It only counts; its default observeOwned is the per-draw loop
struct BulkTestObserver final : DrawObserver {
    bool bulk = true;
    int calls = 0, lastCalls = 0, records = 0, entered = 0, left = 0, observed = 0, nullTag = 0, loadedRegs = 0;
    bool observes(draws::Kind kind, scene::Phase phase) const override { return kind == draws::Kind::StaticMesh && phase.view == scene::View::Main && phase.pass == scene::Pass::Solid; }
    void observe(Device&, const ReplaceQuery& q) override {
        ++observed;
        if (q.tag == nullptr || q.textures == nullptr) ++nullTag;
    }
    Registers registersRead() const override { return Registers::Material; }
    bool bulkOwned() const override { return bulk; }
    void observeOwned(Device& d, const OwnedFrame& f) override {
        ++calls;
        if (f.last) ++lastCalls;
        records += static_cast<int>(f.count);
        left += static_cast<int>(f.leftCount);
        for (std::size_t i = 0; i < f.count; ++i) {
            if ((f.records[i].wanted & f.observerBit) != 0) entered += f.records[i].entered ? 1 : 0;
        }
        DrawObserver::observeOwned(d, f);
    }
};

// F2c-4a persistent owned draws through the device's own path (frame-ownership.md 7.21): the eight quads of the v2 case, each owned draw a SLOT of the real
// gpuscene::drawrecords() tables (derived once by Device::ownedDerive, written by DrawRecordTable::write, uploaded by its flush) and drawn by Device::ownedFlush as
// indirect draws over a CPU-written ref list. Checked against the normal draws (0 bytes differ): (1) one bucket = one indirect call over eight slots; (2) the same
// slots in the next frame with nothing derived again; (3) a slot rewritten (its draw's alpha test changed: the quad vanishes) and rewritten back; (4) a released slot is
// NOT handed out again before the deferral (kFramesInFlight + 1 presents) is over, and the reused slot draws; (5) two view slots = two buckets = two calls and the
// reference's second picture; (6) the guards: a window of another slot (18), a null block address (15), more items than the reserved arena, a null records table:
// nothing drawn, drawn again right after
void testOwnedPersistent(Fixture& f) {
    Device& dev = f.dev;
    dev.setSceneDraws(true, 0);
    if (!dev.sceneDrawsActive()) {
        std::printf("  SKIP owned persistent: scene draws are not available on this device\n");
        return;
    }
    static const char* kVs = R"(
vs_3_0
dcl_position v0
dcl_color v1
dcl_position o0
dcl_color o1
mov o0, v0
add o1, v1, c0
add o1, o1, c12
)";
    static const char* kPs = R"(
ps_3_0
dcl_color v0
add r0, v0, c0
mul r0, r0, c5
mov oC0, r0
)";
    const auto vsV = shaderFrom(dev, kVs);
    const auto psV = shaderFrom(dev, kPs);
    if (!vsV || !psV) {
        std::printf("  FAIL owned persistent: shaders\n");
        ++g_failures;
        return;
    }
    constexpr std::uint32_t kQuads = 8, kSlots = 64;  // (F3-4: the shadow views' slots 32..61 are tested too: the whole view table)
    std::vector<Vertex> verts;
    for (std::uint32_t k = 0; k < kQuads; ++k) {
        const float x0 = -1.0f + 0.25f * static_cast<float>(k), x1 = x0 + 0.25f;
        const std::uint32_t color = 0xFF000000u | (k * 6u + 10u) << 16 | (30u + k * 4u) << 8 | (k & 1u ? 40u : 20u);
        verts.push_back({x0, 0.5f, 0.5f, 1, color, 0, 0});
        verts.push_back({x1, 0.5f, 0.5f, 1, color, 1, 0});
        verts.push_back({x0, -0.5f, 0.5f, 1, color, 0, 1});
        verts.push_back({x1, -0.5f, 0.5f, 1, color, 1, 1});
    }
    const std::uint16_t idx[6] = {0, 1, 2, 1, 3, 2};
    auto vb = dev.createBuffer(static_cast<std::uint32_t>(verts.size() * sizeof(Vertex)), USAGE_WRITEONLY, POOL_MANAGED, false, false);
    auto ib = dev.createBuffer(sizeof(idx), USAGE_WRITEONLY, POOL_MANAGED, true, false);
    std::memcpy(vb->lock(0, 0, 0), verts.data(), verts.size() * sizeof(Vertex));
    vb->unlock();
    std::memcpy(ib->lock(0, 0, 0), idx, sizeof(idx));
    ib->unlock();
    rhi::Device* rd = dev.rhi();
    auto tableBuf = rd->createBuffer({static_cast<std::uint64_t>(kSlots) * shader::abi::kViewBlock2Size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "owned persistent view table"});
    if (!tableBuf) {
        std::printf("  FAIL owned persistent: view table buffer\n");
        ++g_failures;
        return;
    }
    const auto tw = tableBuf->shaderRef(0);
    const rhi::GpuRef tableRef = rhi::GpuRef::fromWords(tw[0], tw[1]);
    std::vector<shader::abi::ViewBlock2> twin(kSlots);
    std::memset(twin.data(), 0, twin.size() * sizeof(shader::abi::ViewBlock2));
    const auto upload = [&] { std::memcpy(tableBuf->map(), twin.data(), twin.size() * sizeof(shader::abi::ViewBlock2)); };
    struct Regs {
        float vs0[4], vs12[4], ps0[4], ps5[4];
    };
    const Regs regs[2] = {{{0.10f, 0.05f, 0.02f, 0.0f}, {0.02f, 0.10f, 0.05f, 0.0f}, {0.30f, 0.20f, 0.10f, 0.0f}, {0.9f, 0.8f, 0.7f, 0.5f}},
                          {{0.02f, 0.12f, 0.06f, 0.0f}, {0.12f, 0.02f, 0.03f, 0.0f}, {0.05f, 0.25f, 0.30f, 0.0f}, {0.6f, 1.0f, 0.5f, 0.5f}}};
    const auto setRegs = [&](const Regs& r) {
        dev.setVertexShaderConstantF(0, r.vs0, 1);
        dev.setVertexShaderConstantF(12, r.vs12, 1);
        dev.setPixelShaderConstantF(0, r.ps0, 1);
        dev.setPixelShaderConstantF(5, r.ps5, 1);
    };
    const auto fillSlot = [&](std::uint32_t slot, const Regs& r) {
        shader::abi::ViewBlock2& b = twin[slot];
        std::memset(&b, 0, sizeof(b));
        std::memcpy(b.vs0[0], r.vs0, 16);
        std::memcpy(b.vs1[0], r.vs12, 16);
        std::memcpy(b.ps0[0], r.ps0, 16);
        dev.viewPosFix(b.posFix);
        std::memcpy(b.c5, r.ps5, 3 * sizeof(float));
        upload();
    };
    const auto windowsOf = [&](std::uint32_t slot) {
        OwnedWindows ow;
        const auto set = [&](OwnedWindows::Win& w, bool vertex, std::uint32_t win, std::uint32_t covered) {
            w.kind = OwnedWindows::Kind::View;
            w.base = rhi::GpuRef{gpuscene::ViewTable::windowBase(tableRef, slot, vertex, win)};
            w.covered = covered;
        };
        set(ow.w[0][0], true, 0, 12);
        set(ow.w[0][1], true, 1, 19);
        set(ow.w[1][0], false, 0, 5);
        ow.viewSlot = slot;
        return ow;
    };
    const auto begin = [&] {
        f.reset();
        dev.drawTag() = {};
        dev.drawTag().source = draws::Source::Mesh;
        dev.phases().push(scene::View::Main, scene::Pass::Solid);
        dev.setVertexShader(vsV);
        dev.setPixelShader(psV);
        dev.setStreamSource(0, vb.get(), 0, sizeof(Vertex));
        dev.setIndices(ib.get());
    };
    gpuscene::DrawRecordTable& tab = gpuscene::drawrecords();
    tab.setActive(true);
    if (!tab.ensure(dev)) {
        std::printf("  FAIL owned persistent: the draw record tables\n");
        ++g_failures;
        return;
    }
    // F-INT (frame-ownership.md 7.43): capacity. Every slot can be taken, the next request is a counted refusal, released slots come back only after the frames in flight
    {
        std::vector<std::uint32_t> all;
        for (;;) {
            const std::uint32_t s = tab.alloc();
            if (s == gpuscene::kNoSlot) break;
            all.push_back(s);
        }
        bool uniq = true;
        std::vector<std::uint8_t> seen(gpuscene::DrawRecordTable::kCapacity, 0);
        for (const std::uint32_t s : all) {
            uniq = uniq && s < gpuscene::DrawRecordTable::kCapacity && seen[s] == 0;
            if (s < seen.size()) seen[s] = 1;
        }
        if (all.size() != gpuscene::DrawRecordTable::kCapacity || !uniq || tab.stats().full != 1 || tab.liveCount() != gpuscene::DrawRecordTable::kCapacity) {
            std::printf("  FAIL record table capacity: %zu slots handed out (capacity %u), unique %d, refusals %llu\n", all.size(), gpuscene::DrawRecordTable::kCapacity, uniq,
                        static_cast<unsigned long long>(tab.stats().full));
            ++g_failures;
        }
        for (auto it = all.rbegin(); it != all.rend(); ++it) tab.release(*it);  // (descending: the free list hands the LOWEST slots out first, the draws below get the highest)
        if (tab.alloc() != gpuscene::kNoSlot) {
            std::printf("  FAIL record table capacity: a released slot was handed out before the frames in flight were done\n");
            ++g_failures;
        }
        for (std::uint32_t i = 0; i < rhi::kFramesInFlight + 2; ++i) tab.flush(dev);
        const std::uint32_t again = tab.alloc();
        if (again == gpuscene::kNoSlot) {
            std::printf("  FAIL record table capacity: no slot came back after the deferral\n");
            ++g_failures;
        }
        tab.release(again);
        for (std::uint32_t i = 0; i < rhi::kFramesInFlight + 2; ++i) tab.flush(dev);
    }
    // the slots below are the table's LAST ones: the draws read records far above the old 4,096 (and above any 16-bit field)
    std::vector<std::uint32_t> parked;
    for (std::uint32_t i = 0; i + 16 < gpuscene::DrawRecordTable::kCapacity; ++i) {
        const std::uint32_t s = tab.alloc();
        if (s == gpuscene::kNoSlot) break;
        parked.push_back(s);
    }
    // F2c-4c: the draws below go through the GPU compaction (the compute pass + DrawIndexedIndirectCount) when the device offers it; the pipeline is built in the background
    gpuscene::OwnedCompact& compact = gpuscene::ownedcompact();
    compact.setCheckEvery(1);
    bool gpuCompact = false;
    for (int i = 0; i < 1200 && !gpuCompact; ++i) {
        gpuCompact = compact.usable(dev);
        if (!gpuCompact) Sleep(50);
    }
    if (!gpuCompact) std::printf("  owned persistent: GPU compaction not available here (the CPU-written arguments are tested)\n");
    // (every frame's table flush goes into the upload list that runs before the frame's draws; the compaction's dispatches follow it)
    const auto end = [&](const char* name) {
        tab.flush(dev);
        compact.flush(dev);
        dev.setSceneIssue(false);
        dev.phases().pop();
        return capture(dev, name);
    };
    const auto bad = [&](const Pixels& ref, const Pixels& p, const char* what) {
        std::uint32_t n = 0;
        for (std::size_t i = 0; i < ref.bgra.size() && i < p.bgra.size(); ++i) n += ref.bgra[i] != p.bgra[i];
        if (n != 0 || ref.bgra.size() != p.bgra.size()) {
            std::printf("  FAIL owned persistent (%s): %u bytes differ from the normal draws\n", what, n);
            ++g_failures;
        }
        return n;
    };
    // references: all eight with view 0; seven (quad 3 left out: its alpha test never passes); four + four on two views
    begin();
    setRegs(regs[0]);
    for (std::uint32_t k = 0; k < kQuads; ++k) dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
    const Pixels ref1 = end("ownedp_ref1");
    begin();
    setRegs(regs[0]);
    for (std::uint32_t k = 0; k < kQuads; ++k) {
        if (k != 3) dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
    }
    const Pixels ref7 = end("ownedp_ref7");
    begin();
    for (std::uint32_t k = 0; k < kQuads; ++k) {
        setRegs(regs[k < 4 ? 0 : 1]);
        dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
    }
    const Pixels ref2 = end("ownedp_ref2");
    dev.setSceneViewTable(tableRef, twin.data(), kSlots);
    dev.setSceneViews(true);
    std::vector<Device::CachedDraw> cd(kQuads);
    bool ready = false;
    for (int i = 0; i < 1200 && !ready; ++i) {
        begin();
        setRegs(regs[0]);
        dev.setSceneIssue(true);
        for (std::uint32_t k = 0; k < kQuads; ++k) {
            dev.beginCapture(&cd[k]);
            dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
            dev.endCapture();
        }
        end("ownedp_capture");
        ready = true;
        for (const auto& c : cd) ready = ready && c.valid && c.scenePipeline != GamePipeline::None && c.scenePipeline2 != GamePipeline::None;
        if (!ready) Sleep(50);
    }
    if (!ready) {
        std::printf("  FAIL owned persistent: the draws never got their v2 pipelines\n");
        ++g_failures;
        return;
    }
    fillSlot(3, regs[0]);
    fillSlot(9, regs[1]);
    std::uint32_t slots[kQuads];
    for (auto& s : slots) s = gpuscene::kNoSlot;
    const auto deriveQuad = [&](std::uint32_t k, std::uint32_t viewSlot, std::uint32_t alphaFunc, Device::OwnedPersist& out) {
        Device::DeriveIn in;
        in.streams[0] = vb.get();
        in.strides[0] = sizeof(Vertex);
        in.indices = ib.get();
        in.alphaFunc = alphaFunc;
        std::memcpy(in.psC5, regs[viewSlot == 9 || viewSlot == 46 ? 1 : 0].ps5, sizeof(in.psC5));
        const OwnedWindows ow = windowsOf(viewSlot);
        return dev.ownedDerive(cd[k], in, ow, tab.blockRef(slots[k]), tab.zeroRef(), true, out);
    };
    // one frame of owned draws: quads `from`..`to` on `viewSlot`, derived only where `derive[k]`; the items' bucket calls
    const auto frame = [&](const char* name, std::uint32_t viewSlot0, std::uint32_t viewSlot1, std::uint32_t split, const bool* derive, const std::uint32_t* alpha, std::uint64_t& calls) {
        begin();
        dev.setSceneIssue(true);
        std::vector<Device::OwnedItem> items[2];
        for (std::uint32_t k = 0; k < kQuads; ++k) {
            const std::uint32_t vs = k < split ? viewSlot0 : viewSlot1;
            if (slots[k] == gpuscene::kNoSlot) slots[k] = tab.alloc();
            if (derive != nullptr && derive[k]) {
                Device::OwnedPersist out;
                const int rc = deriveQuad(k, vs, alpha != nullptr ? alpha[k] : 8u, out);
                if (rc != 0) {
                    std::printf("  FAIL owned persistent (%s): quad %u derive returned %d\n", name, k, rc);
                    ++g_failures;
                }
                tab.write(slots[k], out.rec, out.blk);
                items[k < split ? 0 : 1].push_back({&cd[k], out.bucketKey, slots[k]});
            } else {
                // (nothing derived: the bucket key of the retained draw is a function of it and the view slot)
                items[k < split ? 0 : 1].push_back({&cd[k], dev.ownedBucketKey(cd[k], vs), slots[k]});
            }
        }
        const auto s0 = dev.sceneStats();
        for (std::uint32_t g = 0; g < 2; ++g) {
            if (items[g].empty()) continue;
            Device::OwnedArena arena;
            if (!dev.ownedReserve(static_cast<std::uint32_t>(items[g].size()), arena) || !dev.ownedFlush(items[g], arena, g == 0 ? viewSlot0 : viewSlot1, tab.recordsRef(), tab.recordsShadow(), gpuscene::DrawRecordTable::kCapacity)) {
                std::printf("  FAIL owned persistent (%s): the flush of group %u was refused\n", name, g);
                ++g_failures;
            }
        }
        calls = dev.sceneStats().indirectCalls - s0.indirectCalls;
        return end(name);
    };
    std::uint64_t calls = 0;
    bool all[kQuads];
    for (auto& a : all) a = true;
    const auto w0 = tab.stats().writes;
    // 1. eight owned draws on one view slot: ONE indirect call
    bad(ref1, frame("ownedp_one", 3, 3, kQuads, all, nullptr, calls), "eight persistent draws, one bucket");
    if (calls != 1) {
        std::printf("  FAIL owned persistent: %llu indirect calls for one bucket of %u draws (expected 1)\n", static_cast<unsigned long long>(calls), kQuads);
        ++g_failures;
    }
    const auto w1 = tab.stats().writes;
    if (w1 - w0 != kQuads) {
        std::printf("  FAIL owned persistent: %llu slot writes for %u first derivations\n", static_cast<unsigned long long>(w1 - w0), kQuads);
        ++g_failures;
    }
    // 2. the next frame: the same slots, nothing derived again
    const auto d0 = dev.ownedPersistStats.derived;
    bad(ref1, frame("ownedp_again", 3, 3, kQuads, nullptr, nullptr, calls), "the same slots, nothing derived");
    if (dev.ownedPersistStats.derived != d0 || tab.stats().writes != w1) {
        std::printf("  FAIL owned persistent: a frame of unchanged persistent draws derived / wrote something\n");
        ++g_failures;
    }
    // 3. a slot rewritten: quad 3's alpha test never passes (the slot's block changes), then rewritten back
    {
        bool one[kQuads] = {};
        std::uint32_t alpha[kQuads];
        for (auto& a : alpha) a = 8u;
        one[3] = true;
        alpha[3] = CMP_NEVER;
        bad(ref7, frame("ownedp_rewrite", 3, 3, kQuads, one, alpha, calls), "a slot rewritten (quad 3 discarded)");
        alpha[3] = 8u;
        bad(ref1, frame("ownedp_rewrite_back", 3, 3, kQuads, one, alpha, calls), "the slot rewritten back");
    }
    // 4. slot reuse is deferred: a released slot is not handed out again before kFramesInFlight + 1 presents
    {
        const std::uint32_t released = slots[7];
        tab.release(released);
        slots[7] = gpuscene::kNoSlot;
        const std::uint32_t fresh = tab.alloc();
        if (fresh == released || fresh == gpuscene::kNoSlot) {
            std::printf("  FAIL owned persistent: slot %u was handed out again right after its release\n", released);
            ++g_failures;
        }
        for (std::uint32_t i = 0; i < rhi::kFramesInFlight; ++i) {
            tab.flush(dev);
            const std::uint32_t other = tab.alloc();
            if (other == released) {
                std::printf("  FAIL owned persistent: slot %u was reused %u presents after its release (deferral %u + 1)\n", released, i + 1, rhi::kFramesInFlight);
                ++g_failures;
            }
            if (other != gpuscene::kNoSlot) tab.release(other);
        }
        tab.flush(dev);
        std::uint32_t got = gpuscene::kNoSlot;
        bool reused = false;
        for (int i = 0; i < 8 && !reused; ++i) {
            got = tab.alloc();
            reused = got == released;
        }
        if (!reused) {
            std::printf("  FAIL owned persistent: slot %u never came back after the deferral\n", released);
            ++g_failures;
        }
        slots[7] = got;  // (quad 7 on a reused slot: derived again, drawn the same)
        bool one[kQuads] = {};
        one[7] = true;
        bad(ref1, frame("ownedp_reuse", 3, 3, kQuads, one, nullptr, calls), "quad 7 on a reused slot");
        tab.release(fresh);
    }
    // 5. two view slots = two buckets = two indirect calls, the reference's second picture
    {
        bool derive[kQuads];
        for (auto& d : derive) d = true;
        bad(ref2, frame("ownedp_two", 3, 9, 4, derive, nullptr, calls), "two view slots");
        if (calls != 2) {
            std::printf("  FAIL owned persistent: two view slots made %llu indirect calls (expected 2)\n", static_cast<unsigned long long>(calls));
            ++g_failures;
        }
    }
    // 6. the guards
    {
        begin();
        dev.setSceneIssue(true);
        Device::OwnedPersist out;
        Device::DeriveIn in;
        in.streams[0] = vb.get();
        in.strides[0] = sizeof(Vertex);
        in.indices = ib.get();
        std::memcpy(in.psC5, regs[0].ps5, sizeof(in.psC5));
        OwnedWindows other = windowsOf(3);
        other.viewSlot = 9;
        int refused = 0;
        refused += dev.ownedDerive(cd[0], in, other, tab.blockRef(slots[0]), tab.zeroRef(), true, out) == 18;
        refused += dev.ownedDerive(cd[0], in, windowsOf(3), rhi::GpuRef{}, tab.zeroRef(), true, out) == 15;
        refused += dev.ownedDerive(cd[0], in, windowsOf(70), tab.blockRef(slots[0]), tab.zeroRef(), true, out) == 17;
        std::vector<Device::OwnedItem> items;
        for (std::uint32_t k = 0; k < kQuads; ++k) items.push_back({&cd[k], dev.ownedBucketKey(cd[k], 3), slots[k]});
        Device::OwnedArena arena;
        const bool reserved = dev.ownedReserve(4, arena);  // (room for four of the eight)
        const bool tooMany = reserved && !dev.ownedFlush(items, arena, 3, tab.recordsRef(), tab.recordsShadow(), gpuscene::DrawRecordTable::kCapacity);
        Device::OwnedArena arena2;
        const bool reserved2 = dev.ownedReserve(kQuads, arena2);
        const bool noTable = reserved2 && !dev.ownedFlush(items, arena2, 3, rhi::GpuRef{}, tab.recordsShadow(), gpuscene::DrawRecordTable::kCapacity);
        const bool badSlot = reserved2 && !dev.ownedFlush(items, arena2, 70, tab.recordsRef(), tab.recordsShadow(), gpuscene::DrawRecordTable::kCapacity);
        // (an item whose slot was never written / is out of range)
        std::vector<Device::OwnedItem> bogus = {{&cd[0], dev.ownedBucketKey(cd[0], 3), gpuscene::DrawRecordTable::kCapacity}, {&cd[1], dev.ownedBucketKey(cd[1], 3), tab.alloc()}};
        Device::OwnedArena arena3;
        const bool bogusRefused = dev.ownedReserve(2, arena3) && !dev.ownedFlush(bogus, arena3, 3, tab.recordsRef(), tab.recordsShadow(), gpuscene::DrawRecordTable::kCapacity);
        tab.release(bogus[1].slot);
        const Pixels p = end("ownedp_guards");
        if (refused != 3 || !tooMany || !noTable || !badSlot || !bogusRefused || p.bgra[(32 * p.w + 2) * 4 + 1] != 0) {
            std::printf("  FAIL owned persistent: guards: %d of 3 derivations refused as expected, more items than the arena refused %d, null records table refused %d, a view slot out of range refused %d, an unwritten / out-of-range slot refused %d, drew something %d\n",
                        refused, tooMany, noTable, badSlot, bogusRefused, p.bgra[(32 * p.w + 2) * 4 + 1] != 0);
            ++g_failures;
        }
        bad(ref1, frame("ownedp_after_guards", 3, 3, kQuads, nullptr, nullptr, calls), "persistent draws after the refusals");
    }
    // 6b. F3-1 (frame-ownership.md 7.36): the env epoch is kept PER VIEW SLOT: the passes of a frame with owned shadows come in the order main z, cascade 0, cascade 1, main solid and
    // every slot keeps its epoch while nothing changed (a shared memo would move all of them every pass: every persistent draw would be derived again each frame); a viewport change
    // (a shadow map of another size, a resolution switch) moves every slot's epoch
    {
        dev.setSceneIssue(true);
        constexpr std::uint32_t kSlotsUnderTest[4] = {0, 4, 5, 1};
        const auto epochs = [&](std::uint64_t* e, bool* ok) {
            for (std::uint32_t i = 0; i < 4; ++i) e[i] = dev.ownedEnvEpoch(kSlotsUnderTest[i] == 1, kSlotsUnderTest[i], ok[i]);
        };
        std::uint64_t e1[4], e2[4], e3[4];
        bool ok1[4], ok2[4], ok3[4];
        epochs(e1, ok1);
        epochs(e2, ok2);
        const Viewport saved = dev.viewport();
        Viewport changed = saved;
        changed.width = saved.width / 2;
        changed.height = saved.height / 2;
        dev.setViewport(changed);
        epochs(e3, ok3);
        dev.setViewport(saved);
        bool good = true;
        for (std::uint32_t i = 0; i < 4; ++i) {
            good = good && ok1[i] && ok2[i] && ok3[i] && e1[i] == e2[i] && e3[i] != e1[i];
            for (std::uint32_t j = i + 1; j < 4; ++j) good = good && e1[i] != e1[j];
        }
        if (!good) {
            std::printf("  FAIL owned persistent: env epochs per view slot: stable %d/%d/%d/%d, moved by a viewport change %d/%d/%d/%d (ok flags %d%d%d%d)\n", e1[0] == e2[0], e1[1] == e2[1], e1[2] == e2[2], e1[3] == e2[3],
                        e3[0] != e1[0], e3[1] != e1[1], e3[2] != e1[2], e3[3] != e1[3], ok1[0], ok1[1], ok1[2], ok1[3]);
            ++g_failures;
        }
        // (back to the unchanged viewport: the check frames' ownedEnvSame must see the old epoch of slot 4 as stale now and the newest one as current)
        epochs(e2, ok2);
        if (!dev.ownedEnvSame(false, 4, e2[1]) || dev.ownedEnvSame(false, 4, e1[1]) || dev.ownedEnvSame(false, 5, e2[1])) {
            std::printf("  FAIL owned persistent: ownedEnvSame per view slot\n");
            ++g_failures;
        }
        dev.setSceneIssue(false);
    }
    // 6c. F3-4 (frame-ownership.md 14.29): the terrain map's / the point-light atlas cells' view slots (32, 33 + i; the trees' 47, 48 + i). (1) Persistent draws on cell slots 33
    // and 46 draw exactly like the normal draws with those views (two buckets, two calls); (2) every slot keeps its OWN env epoch memo: before F3-4 every slot >= 8 shared
    // the memo of slot 7, so cells 0 and 13 of one frame would move each other's epoch at every pass (every atlas draw derived again each frame); the same for the trees' slots
    {
        fillSlot(33, regs[0]);
        fillSlot(46, regs[1]);
        bool derive[kQuads];
        for (auto& d : derive) d = true;
        bad(ref2, frame("ownedp_cells", 33, 46, 4, derive, nullptr, calls), "atlas cell slots 33 / 46");
        if (calls != 2) {
            std::printf("  FAIL owned persistent: two atlas cell slots made %llu indirect calls (expected 2)\n", static_cast<unsigned long long>(calls));
            ++g_failures;
        }
        const auto d0c = dev.ownedPersistStats.derived;
        bad(ref2, frame("ownedp_cells_again", 33, 46, 4, nullptr, nullptr, calls), "atlas cell slots again, nothing derived");
        if (dev.ownedPersistStats.derived != d0c) {
            std::printf("  FAIL owned persistent: a frame of unchanged atlas cell draws derived something\n");
            ++g_failures;
        }
        dev.setSceneIssue(true);
        constexpr std::uint32_t kCellSlots[5] = {7, 32, 33, 46, 61};
        std::uint64_t e1[5], e2[5];
        bool ok1[5], ok2[5];
        for (std::uint32_t i = 0; i < 5; ++i) e1[i] = dev.ownedEnvEpoch(false, kCellSlots[i], ok1[i]);
        for (std::uint32_t i = 0; i < 5; ++i) e2[i] = dev.ownedEnvEpoch(false, kCellSlots[i], ok2[i]);
        bool good = true;
        for (std::uint32_t i = 0; i < 5; ++i) {
            good = good && ok1[i] && ok2[i] && e1[i] == e2[i] && dev.ownedEnvSame(false, kCellSlots[i], e1[i]);
            for (std::uint32_t j = i + 1; j < 5; ++j) good = good && e1[i] != e1[j];
        }
        // a cell of another size (viewport) moves ITS slot's epoch only: slot 46 keeps its epoch while 33 is asked under another viewport
        const Viewport saved = dev.viewport();
        Viewport changed = saved;
        changed.width = saved.width / 2;
        changed.height = saved.height / 2;
        dev.setViewport(changed);
        bool okc = false;
        const std::uint64_t e33c = dev.ownedEnvEpoch(false, 33, okc);
        dev.setViewport(saved);
        good = good && okc && e33c != e1[2] && dev.ownedEnvSame(false, 46, e1[3]) && !dev.ownedEnvSame(false, 33, e1[2]);
        // the trees' slots (47 terrain map, 48 / 61 atlas cells, 8 a cascade's): one memo each
        constexpr std::uint32_t kTreeSlots[4] = {8, 47, 48, 61};
        std::uint64_t t1[4], t2[4];
        bool tok[4];
        for (std::uint32_t i = 0; i < 4; ++i) t1[i] = dev.treeEnvEpoch(false, kTreeSlots[i], tok[i]);
        for (std::uint32_t i = 0; i < 4; ++i) t2[i] = dev.treeEnvEpoch(false, kTreeSlots[i], tok[i]);
        for (std::uint32_t i = 0; i < 4; ++i) {
            good = good && tok[i] && t1[i] == t2[i] && dev.treeEnvSame(false, kTreeSlots[i], t1[i]);
            for (std::uint32_t j = i + 1; j < 4; ++j) good = good && t1[i] != t1[j];
        }
        if (!good) {
            std::printf("  FAIL owned persistent: shadow views' env epochs per slot (cells %llu/%llu/%llu/%llu/%llu, a changed cell moved its own epoch %d, the other cell kept its own %d, trees %llu/%llu/%llu/%llu)\n",
                        static_cast<unsigned long long>(e1[0]), static_cast<unsigned long long>(e1[1]), static_cast<unsigned long long>(e1[2]), static_cast<unsigned long long>(e1[3]),
                        static_cast<unsigned long long>(e1[4]), e33c != e1[2], dev.ownedEnvSame(false, 46, e1[3]), static_cast<unsigned long long>(t1[0]), static_cast<unsigned long long>(t1[1]),
                        static_cast<unsigned long long>(t1[2]), static_cast<unsigned long long>(t1[3]));
            ++g_failures;
        }
        dev.setSceneIssue(false);
    }    // 7. F2c-4b bulk observers: the records of a pass arrive in observeOwned (not before), with entered / left against the previous pass of the kind; a mid-pass flush does not
    // disturb the deltas; a per-draw (not bulk) observer sees every draw at once; the register images are loaded only on request; an ineligible kind stays per-draw
    {
        BulkTestObserver obs;
        dev.addDrawObserver(&obs);
        begin();
        OwnedWindows ows[kQuads];
        for (auto& w : ows) w = windowsOf(3);
        const auto pass = [&](std::uint32_t from, std::uint32_t to, bool midFlush) {
            dev.ownedBulkBegin(true);
            for (std::uint32_t k = from; k < to; ++k) {
                Device::OwnedObsInfo info;
                info.id = slots[k];
                info.sig = 11;
                info.epoch = 22;
                info.changed = k == from;
                if (!dev.ownedObserve(cd[k], ows[k], info)) {
                    std::printf("  FAIL owned bulk: an observer wanted a capture\n");
                    ++g_failures;
                }
                if (midFlush && k + 1 == (from + to) / 2) dev.ownedBulkFlush(false);
            }
            dev.ownedBulkFlush(true);
        };
        const auto expect = [&](const char* what, int calls, int records, int entered, int left, int observed) {
            if (obs.calls != calls || obs.records != records || obs.entered != entered || obs.left != left || obs.observed != observed || obs.nullTag != 0) {
                std::printf("  FAIL owned bulk (%s): calls %d records %d entered %d left %d observed %d (expected %d / %d / %d / %d / %d), draws without tag / textures %d\n", what, obs.calls, obs.records,
                            obs.entered, obs.left, obs.observed, calls, records, entered, left, observed, obs.nullTag);
                ++g_failures;
            }
        };
        pass(0, kQuads, false);
        expect("first pass: everything entered", 1, 8, 8, 0, 8);
        pass(0, 6, false);
        expect("quads 6, 7 left", 2, 14, 8, 2, 14);
        pass(0, kQuads, true);
        expect("quads 6, 7 entered again; a mid-pass flush", 4, 22, 10, 2, 22);
        if (obs.lastCalls != 3) {
            std::printf("  FAIL owned bulk: the `last` call came %d times (expected once per pass = 3)\n", obs.lastCalls);
            ++g_failures;
        }
        // not bulk: the draws are shown at once, nothing is delivered later
        const int callsBefore = obs.calls, observedBefore = obs.observed;
        obs.bulk = false;
        pass(0, kQuads, false);
        if (obs.calls != callsBefore || obs.observed != observedBefore + 8) {
            std::printf("  FAIL owned bulk: a per-draw observer saw %d draws of 8 (bulk calls %d -> %d)\n", obs.observed - observedBefore, callsBefore, obs.calls);
            ++g_failures;
        }
        dev.removeDrawObserver(&obs);
        dev.phases().pop();
        std::printf("  owned bulk observers: %llu flushes, %llu records, %llu entered, %llu left\n", static_cast<unsigned long long>(dev.ownedBulkStats.flushes), static_cast<unsigned long long>(dev.ownedBulkStats.records),
                    static_cast<unsigned long long>(dev.ownedBulkStats.entered), static_cast<unsigned long long>(dev.ownedBulkStats.left));
    }
    // 7. F9 / F14 (frame-ownership.md 14.20, native.owned_ordered): blended draws whose result depends on their order (four quads over the same pixels, alpha blending, two pixel
    // shaders = two pipelines alternating A B A B): with setSceneOrdered they get their scene forms although they are not batchable (CachedDraw::ordered); an ORDERED flush keeps
    // them as four runs (four indirect calls) and equals the normal draws byte for byte; the same items in an unordered flush are merged into two buckets (A A B B) and
    // differ (the case is sensitive to the order)
    {
        static const char* kPsB = R"(
ps_3_0
dcl_color v0
mul r0, v0, c5
mov oC0, r0
)";
        const auto psB = shaderFrom(dev, kPsB);
        std::vector<Vertex> ov;
        const std::uint32_t colors[4] = {0xFFE02010u, 0xFF10D030u, 0xFF2040F0u, 0xFFF0F0F0u};
        for (std::uint32_t k = 0; k < 4; ++k) {
            ov.push_back({-0.6f, 0.6f, 0.5f, 1, colors[k], 0, 0});
            ov.push_back({0.6f, 0.6f, 0.5f, 1, colors[k], 1, 0});
            ov.push_back({-0.6f, -0.6f, 0.5f, 1, colors[k], 0, 1});
            ov.push_back({0.6f, -0.6f, 0.5f, 1, colors[k], 1, 1});
        }
        auto vbo = dev.createBuffer(static_cast<std::uint32_t>(ov.size() * sizeof(Vertex)), USAGE_WRITEONLY, POOL_MANAGED, false, false);
        std::memcpy(vbo->lock(0, 0, 0), ov.data(), ov.size() * sizeof(Vertex));
        vbo->unlock();
        const auto beginO = [&] {
            f.reset();
            dev.drawTag() = {};
            dev.drawTag().source = draws::Source::Mesh;
            dev.phases().push(scene::View::Main, scene::Pass::Alpha);
            dev.setVertexShader(vsV);
            dev.setStreamSource(0, vbo.get(), 0, sizeof(Vertex));
            dev.setIndices(ib.get());
            dev.setRenderState(RS_ALPHABLENDENABLE, 1);
            dev.setRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
            dev.setRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
            dev.setRenderState(RS_ZWRITEENABLE, 0);
            setRegs(regs[0]);
        };
        const auto psOf = [&](std::uint32_t k) { return (k & 1u) != 0 ? psB : psV; };
        beginO();
        for (std::uint32_t k = 0; k < 4; ++k) {
            dev.setPixelShader(psOf(k));
            dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
        }
        const Pixels refO = end("ownedo_ref");
        dev.setSceneOrdered(true);
        std::vector<Device::CachedDraw> cdo(4);
        bool readyO = static_cast<bool>(psB);
        for (int i = 0; i < 1200 && readyO; ++i) {
            beginO();
            dev.setSceneIssue(true);
            for (std::uint32_t k = 0; k < 4; ++k) {
                dev.setPixelShader(psOf(k));
                dev.beginCapture(&cdo[k]);
                dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
                dev.endCapture();
            }
            end("ownedo_capture");
            bool all4 = true;
            for (const auto& c : cdo) all4 = all4 && c.valid && c.ordered && !c.packet.batchable && c.scenePipeline2 != GamePipeline::None;
            if (all4) break;
            if (i == 1199) readyO = false;
            Sleep(50);
        }
        if (!readyO) {
            std::printf("  FAIL owned ordered: the blended draws never got their ordered v2 scene forms\n");
            ++g_failures;
        } else {
            std::uint32_t oslots[4];
            for (auto& s : oslots) s = tab.alloc();
            const auto frameO = [&](const char* name, bool ordered, std::uint64_t& callsO) {
                beginO();
                dev.setSceneIssue(true);
                std::vector<Device::OwnedItem> items;
                for (std::uint32_t k = 0; k < 4; ++k) {
                    dev.setPixelShader(psOf(k));
                    Device::DeriveIn in;
                    in.streams[0] = vbo.get();
                    in.strides[0] = sizeof(Vertex);
                    in.indices = ib.get();
                    std::memcpy(in.psC5, regs[0].ps5, sizeof(in.psC5));
                    Device::OwnedPersist out;
                    const int rc = dev.ownedDerive(cdo[k], in, windowsOf(3), tab.blockRef(oslots[k]), tab.zeroRef(), true, out);
                    if (rc != 0) {
                        std::printf("  FAIL owned ordered (%s): quad %u derive returned %d\n", name, k, rc);
                        ++g_failures;
                        continue;
                    }
                    tab.write(oslots[k], out.rec, out.blk);
                    items.push_back({&cdo[k], out.bucketKey, oslots[k]});
                }
                const auto s0 = dev.sceneStats();
                Device::OwnedArena arena;
                if (!dev.ownedReserve(static_cast<std::uint32_t>(items.size()), arena) ||
                    !dev.ownedFlush(items, arena, 3, tab.recordsRef(), tab.recordsShadow(), gpuscene::DrawRecordTable::kCapacity, ordered)) {
                    std::printf("  FAIL owned ordered (%s): the flush was refused\n", name);
                    ++g_failures;
                }
                callsO = dev.sceneStats().indirectCalls - s0.indirectCalls;
                return end(name);
            };
            std::uint64_t callsO = 0;
            bad(refO, frameO("ownedo_ordered", true, callsO), "ordered flush of four blended draws");
            if (callsO != 4) {
                std::printf("  FAIL owned ordered: %llu indirect calls for the runs A B A B (expected 4)\n", static_cast<unsigned long long>(callsO));
                ++g_failures;
            }
            const Pixels merged = frameO("ownedo_merged", false, callsO);
            std::uint32_t nd = 0;
            for (std::size_t i = 0; i < refO.bgra.size() && i < merged.bgra.size(); ++i) nd += refO.bgra[i] != merged.bgra[i];
            if (callsO != 2 || nd == 0) {
                std::printf("  FAIL owned ordered: the unordered flush made %llu calls (expected 2) and differs in %u bytes (expected > 0: the case must see the order)\n",
                            static_cast<unsigned long long>(callsO), nd);
                ++g_failures;
            }
            bad(refO, frameO("ownedo_ordered_again", true, callsO), "ordered flush again");
            // the pending run (StaticOps' runs of ordered ops): quads 0..2 derived and left pending behind Device::setOrderedHook, quad 3 drawn the normal way: the device must
            // queue the run before quad 3's packet (submitInterned's hook), so the picture is the reference's
            {
                struct Run {
                    Device* dev = nullptr;
                    std::vector<Device::OwnedItem> items;
                    Device::OwnedArena arena;
                    int calls = 0;
                    bool ok = false;
                } run;
                run.dev = &dev;
                beginO();
                dev.setSceneIssue(true);
                for (std::uint32_t k = 0; k < 3; ++k) {
                    dev.setPixelShader(psOf(k));
                    Device::DeriveIn in;
                    in.streams[0] = vbo.get();
                    in.strides[0] = sizeof(Vertex);
                    in.indices = ib.get();
                    std::memcpy(in.psC5, regs[0].ps5, sizeof(in.psC5));
                    Device::OwnedPersist out;
                    if (dev.ownedDerive(cdo[k], in, windowsOf(3), tab.blockRef(oslots[k]), tab.zeroRef(), true, out) != 0) {
                        std::printf("  FAIL owned ordered (pending run): quad %u derive refused\n", k);
                        ++g_failures;
                        continue;
                    }
                    tab.write(oslots[k], out.rec, out.blk);
                    run.items.push_back({&cdo[k], out.bucketKey, oslots[k]});
                }
                dev.setSceneIssue(false);
                const bool reserved = dev.ownedReserve(4, run.arena);
                dev.setOrderedHook(
                    [](void* ctx) {
                        Run& r = *static_cast<Run*>(ctx);
                        ++r.calls;
                        r.ok = r.dev->ownedFlush(r.items, r.arena, 3, gpuscene::drawrecords().recordsRef(), gpuscene::drawrecords().recordsShadow(), gpuscene::DrawRecordTable::kCapacity, true);
                    },
                    &run);
                dev.setOrderedPending(reserved);
                dev.setPixelShader(psOf(3));
                dev.drawIndexedPrimitive(PT_TRIANGLELIST, 12, 0, 4, 0, 2);  // (quad 3: the hook queues the run first)
                const Pixels pr = end("ownedo_pending_run");
                dev.setOrderedHook(nullptr, nullptr);
                if (run.calls != 1 || !run.ok || dev.orderedPending()) {
                    std::printf("  FAIL owned ordered (pending run): the hook ran %d times, flush ok %d, still pending %d\n", run.calls, run.ok, dev.orderedPending());
                    ++g_failures;
                }
                bad(refO, pr, "a pending run queued before the next normal draw");
            }
            // F6-2 regression (the device loss of 2026-11-11 08:41, frame-ownership.md 14.23): a pending run, then a BATCHABLE composed scene draw (an opaque solid-pass draw:
            // its record joins sceneRecs_ and the packet waits in the batch). The run must be queued before that record is pushed: queued from submitInterned (after
            // toScenePacket), its flush recorded the batch and emptied sceneRecs_, and the waiting packet kept an index into the emptied list (the run dropped by the guard,
            // or with a refilled list another draw's record: a null window read on the GPU)
            {
                const auto opaque = [&] {
                    dev.setRenderState(RS_ALPHABLENDENABLE, 0);
                    dev.setRenderState(RS_ZWRITEENABLE, 1);
                    dev.setPixelShader(psOf(3));
                };
                const auto opaqueState = [&] {
                    dev.phases().push(scene::View::Main, scene::Pass::Solid);
                    opaque();
                };
                // (the order: opaque quad 3 (waits in the batch), the ordered run of the blended quads 0..2, opaque quad 3 again: the last one covers the blend, so a lost draw shows)
                beginO();
                opaqueState();
                dev.drawIndexedPrimitive(PT_TRIANGLELIST, 12, 0, 4, 0, 2);
                dev.setRenderState(RS_ALPHABLENDENABLE, 1);
                dev.setRenderState(RS_ZWRITEENABLE, 0);
                for (std::uint32_t k = 0; k < 3; ++k) {
                    dev.setPixelShader(psOf(k));
                    dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
                }
                opaque();
                dev.drawIndexedPrimitive(PT_TRIANGLELIST, 12, 0, 4, 0, 2);
                dev.phases().pop();
                const Pixels refP = end("ownedo_pending_scene_ref");
                Device::CachedDraw cq;
                bool readyQ = false;
                for (int i = 0; i < 1200 && !readyQ; ++i) {
                    beginO();
                    dev.setSceneIssue(true);
                    opaqueState();
                    dev.beginCapture(&cq);
                    dev.drawIndexedPrimitive(PT_TRIANGLELIST, 12, 0, 4, 0, 2);
                    dev.endCapture();
                    dev.phases().pop();
                    end("ownedo_pending_scene_capture");
                    readyQ = cq.valid && cq.packet.batchable && cq.scenePipeline != GamePipeline::None;
                    if (!readyQ) Sleep(50);
                }
                if (!readyQ) {
                    std::printf("  FAIL owned ordered (pending run + scene draw): the opaque draw never got its batchable scene form\n");
                    ++g_failures;
                } else {
                    struct Run {
                        Device* dev = nullptr;
                        std::vector<Device::OwnedItem> items;
                        Device::OwnedArena arena;
                        int calls = 0;
                        bool ok = false;
                    } run;
                    run.dev = &dev;
                    beginO();
                    dev.setSceneIssue(true);
                    for (std::uint32_t k = 0; k < 3; ++k) {
                        dev.setPixelShader(psOf(k));
                        Device::DeriveIn in;
                        in.streams[0] = vbo.get();
                        in.strides[0] = sizeof(Vertex);
                        in.indices = ib.get();
                        std::memcpy(in.psC5, regs[0].ps5, sizeof(in.psC5));
                        Device::OwnedPersist out;
                        if (dev.ownedDerive(cdo[k], in, windowsOf(3), tab.blockRef(oslots[k]), tab.zeroRef(), true, out) != 0) {
                            std::printf("  FAIL owned ordered (pending run + scene draw): quad %u derive refused\n", k);
                            ++g_failures;
                            continue;
                        }
                        tab.write(oslots[k], out.rec, out.blk);
                        run.items.push_back({&cdo[k], out.bucketKey, oslots[k]});
                    }
                    const bool reserved = dev.ownedReserve(4, run.arena);
                    dev.setOrderedHook(
                        [](void* ctx) {
                            Run& r = *static_cast<Run*>(ctx);
                            ++r.calls;
                            r.ok = r.dev->ownedFlush(r.items, r.arena, 3, gpuscene::drawrecords().recordsRef(), gpuscene::drawrecords().recordsShadow(), gpuscene::DrawRecordTable::kCapacity, true);
                        },
                        &run);
                    const std::uint64_t dropped0 = dev.sceneStats().fbNullAddress;
                    opaqueState();
                    dev.setSceneIssue(true);
                    // (packet-cache replays, the game's path of a composed draw: replayDraw makes the scene packet without prepareDraw)
                    Device::ReplayInputs rin;
                    rin.streams[0] = vbo.get();
                    rin.strides[0] = sizeof(Vertex);
                    rin.indices = ib.get();
                    const bool r1 = dev.replayDraw(cq, rin);  // (a batchable scene draw: it waits in the batch with its record)
                    dev.setOrderedPending(reserved);
                    const bool r2 = dev.replayDraw(cq, rin);  // (another one: the run must be queued before its record joins the list)
                    if (!r1 || !r2) {
                        std::printf("  FAIL owned ordered (pending run + scene draw): a replay was refused (%d %d)\n", r1, r2);
                        ++g_failures;
                    }
                    dev.phases().pop();
                    const Pixels pq = end("ownedo_pending_scene");
                    dev.setOrderedHook(nullptr, nullptr);
                    const std::uint64_t dropped = dev.sceneStats().fbNullAddress - dropped0;
                    if (run.calls != 1 || !run.ok || dev.orderedPending() || dropped != 0) {
                        std::printf("  FAIL owned ordered (pending run + scene draw): the hook ran %d times, flush ok %d, still pending %d, scene runs dropped %llu\n", run.calls, run.ok, dev.orderedPending(),
                                    static_cast<unsigned long long>(dropped));
                        ++g_failures;
                    }
                    bad(refP, pq, "a pending run, then a batchable composed scene draw");
                    std::printf("  owned ordered: pending run + a batchable composed scene draw: run queued first, 0 scene runs dropped, = the normal draws\n");
                }
            }
            std::printf("  owned ordered: four blended draws in A B A B order: ordered flush = the normal draws, unordered (merged) flush differs in %u bytes\n", nd);
            // GPU safety (the device loss of 2026-11-11, frame-ownership.md 14.22) and F6-2 / 14.23 item 2: the alpha list's herbs bind stream 3 at instance rate (0x80000001) while
            // stream 0 is not instanced (one instance: the attribute reads element 0). The device binds such a stream per vertex with stride 0 (Device::instRateAsVertex): the
            // draw gets the ordered scene forms, and an ordered flush that puts it at list position 2 (firstInstance 2: an instance-rate binding would read element 2 of a
            // one-element buffer) must equal the normal draws byte for byte. A truly instanced draw (stream 0 INDEXEDDATA) keeps its instance-rate binding and gets NO ordered form.
            {
                const float off[4] = {0.15f, -0.1f, 0.0f, 0.0f};
                auto instBuf = dev.createBuffer(16, USAGE_WRITEONLY, POOL_MANAGED, false, false);
                std::memcpy(instBuf->lock(0, 0, 0), off, sizeof(off));
                instBuf->unlock();
                const auto herbState = [&] {
                    dev.setVertexDecl(f.instDecl.get());
                    dev.setVertexShader(f.vsInst);
                    dev.setStreamSource(1, instBuf.get(), 0, 16);
                    dev.setStreamSourceFreq(1, STREAMSOURCE_INSTANCEDATA | 1u);
                };
                const auto plainState = [&] {
                    dev.setStreamSourceFreq(1, 1u);
                    dev.setStreamSource(1, nullptr, 0, 0);
                    dev.setVertexDecl(f.decl.get());
                    dev.setVertexShader(vsV);
                };
                // the reference: quads 0, 1 and the herb-like quad (quad 2's vertices + the instance offset), drawn normally with the instance-rate binding (ordered off)
                dev.setSceneOrdered(false);
                beginO();
                for (std::uint32_t k = 0; k < 2; ++k) {
                    dev.setPixelShader(psOf(k));
                    dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
                }
                herbState();
                dev.setPixelShader(psOf(2));
                dev.drawIndexedPrimitive(PT_TRIANGLELIST, 8, 0, 4, 0, 2);
                plainState();
                const Pixels refH = end("ownedo_herb_ref");
                dev.setSceneOrdered(true);
                Device::CachedDraw ci;
                bool readyH = false;
                for (int i = 0; i < 1200 && !readyH; ++i) {
                    beginO();
                    dev.setSceneIssue(true);
                    herbState();
                    dev.setPixelShader(psOf(2));
                    dev.beginCapture(&ci);
                    dev.drawIndexedPrimitive(PT_TRIANGLELIST, 8, 0, 4, 0, 2);
                    dev.endCapture();
                    plainState();
                    end("ownedo_herb_capture");
                    readyH = ci.valid && ci.ordered && ci.scenePipeline2 != GamePipeline::None;
                    if (!readyH) Sleep(50);
                }
                bool perInst = false, stride0 = false;
                for (std::uint32_t bnd = 0; bnd < ci.vi.bindingCount && bnd < rhi::kMaxVertexBindings; ++bnd) {
                    perInst = perInst || ci.vi.bindings[bnd].perInstance != 0;
                    stride0 = stride0 || (ci.vi.bindings[bnd].stride == 0 && ci.vi.buffers[bnd] != 0);
                }
                if (!readyH || perInst || (ci.vi.dynamicLayout && !stride0)) {
                    std::printf("  FAIL owned ordered: the herb-like draw (an instance-data stream, stream 0 not instanced) got no ordered form or kept an instance-rate binding (ordered %d, v2 %llu, per instance %d, stride 0 %d)\n",
                                ci.ordered, static_cast<unsigned long long>(ci.scenePipeline2), perInst, stride0);
                    ++g_failures;
                } else {
                    beginO();
                    dev.setSceneIssue(true);
                    std::vector<Device::OwnedItem> items;
                    for (std::uint32_t k = 0; k < 3; ++k) {
                        Device::DeriveIn in;
                        in.streams[0] = vbo.get();
                        in.strides[0] = sizeof(Vertex);
                        if (k == 2) {
                            in.streams[1] = instBuf.get();
                            in.strides[1] = 16;
                            herbState();
                        }
                        dev.setPixelShader(psOf(k));
                        in.indices = ib.get();
                        std::memcpy(in.psC5, regs[0].ps5, sizeof(in.psC5));
                        Device::OwnedPersist out;
                        const Device::CachedDraw& cd = k == 2 ? ci : cdo[k];
                        const int rc = dev.ownedDerive(cd, in, windowsOf(3), tab.blockRef(oslots[k]), tab.zeroRef(), true, out);
                        if (rc != 0) {
                            std::printf("  FAIL owned ordered (herb): item %u derive returned %d\n", k, rc);
                            ++g_failures;
                            continue;
                        }
                        tab.write(oslots[k], out.rec, out.blk);
                        items.push_back({&cd, out.bucketKey, oslots[k]});
                    }
                    Device::OwnedArena arena;
                    if (!dev.ownedReserve(static_cast<std::uint32_t>(items.size()), arena) ||
                        !dev.ownedFlush(items, arena, 3, tab.recordsRef(), tab.recordsShadow(), gpuscene::DrawRecordTable::kCapacity, true)) {
                        std::printf("  FAIL owned ordered (herb): the flush was refused\n");
                        ++g_failures;
                    }
                    plainState();
                    bad(refH, end("ownedo_herb_owned"), "an ordered flush with the herb-like draw at list position 2 (instance data per vertex, stride 0)");
                    std::printf("  owned ordered: herb-like draw (instance-data stream 1, stream 0 not instanced): bound per vertex with stride 0, ordered flush at position 2 = the normal draws\n");
                }
                // a hardware-instanced draw (stream 0 with INDEXEDDATA): its instance-rate binding stays, no ordered form
                beginO();
                dev.setSceneIssue(true);
                herbState();
                dev.setStreamSourceFreq(0, STREAMSOURCE_INDEXEDDATA | 1u);
                dev.setPixelShader(psOf(0));
                Device::CachedDraw cn;
                dev.beginCapture(&cn);
                dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, 4, 0, 2);
                dev.endCapture();
                dev.setStreamSourceFreq(0, 1u);
                plainState();
                end("ownedo_instrate");
                if (cn.ordered || cn.scenePipeline2 != GamePipeline::None) {
                    std::printf("  FAIL owned ordered: a hardware-instanced draw got the ordered scene forms (ordered %d, v2 pipeline %llu)\n", cn.ordered,
                                static_cast<unsigned long long>(cn.scenePipeline2));
                    ++g_failures;
                }
                // and the flush's own guard refuses such an item even if it were marked ordered
                Device::CachedDraw forged = cdo[0];
                forged.vi.bindingCount = std::max<std::uint32_t>(forged.vi.bindingCount, 2);
                forged.vi.bindings[1].perInstance = 1;
                std::vector<Device::OwnedItem> items = {{&forged, dev.ownedBucketKey(forged, 3), oslots[0]}};
                Device::OwnedArena arena;
                beginO();
                const bool refused = dev.ownedReserve(1, arena) && !dev.ownedFlush(items, arena, 3, tab.recordsRef(), tab.recordsShadow(), gpuscene::DrawRecordTable::kCapacity, true);
                end("ownedo_instrate_guard");
                if (!refused) {
                    std::printf("  FAIL owned ordered: the flush took an ordered item with an instance-rate binding\n");
                    ++g_failures;
                }
            }
            for (const std::uint32_t s : oslots) tab.release(s);
            // the COMPOSED scene forms in the solid pass: opaque draws (batchable: they wait in the batch with their records) and blended ones (ordered: recorded alone, after
            // the batch) interleaved over the same pixels; the result must be the normal draws' (a blended draw's record joining the batch's record list, or the batch
            // regrouped around it, would draw the wrong order or read a wrong record)
            const auto drawMix = [&](bool scene) {
                f.reset();
                dev.drawTag() = {};
                dev.drawTag().source = draws::Source::Mesh;
                dev.phases().push(scene::View::Main, scene::Pass::Solid);
                dev.setVertexShader(vsV);
                dev.setStreamSource(0, vbo.get(), 0, sizeof(Vertex));
                dev.setIndices(ib.get());
                setRegs(regs[0]);
                dev.setSceneIssue(scene);
                for (std::uint32_t k = 0; k < 4; ++k) {
                    const bool blend = (k & 1u) != 0;
                    dev.setRenderState(RS_ALPHABLENDENABLE, blend ? 1u : 0u);
                    dev.setRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
                    dev.setRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
                    dev.setRenderState(RS_ZWRITEENABLE, blend ? 0u : 1u);
                    dev.setPixelShader(psOf(k));
                    dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
                }
            };
            drawMix(false);
            const Pixels refM = end("ownedo_mix_ref");
            bool mixed = false;
            for (int i = 0; i < 400 && !mixed; ++i) {
                const std::uint64_t sd0 = dev.sceneStats().sceneDraws;
                drawMix(true);
                const Pixels pm = end("ownedo_mix_scene");
                if (dev.sceneStats().sceneDraws - sd0 == 4) {
                    mixed = true;
                    bad(refM, pm, "composed scene forms, opaque + blended interleaved");
                } else {
                    Sleep(25);
                }
            }
            if (!mixed) {
                std::printf("  FAIL owned ordered: the interleaved opaque / blended draws never all became scene draws\n");
                ++g_failures;
            }
        }
        dev.setSceneOrdered(false);
        dev.release(std::move(vbo));
    }
    // F2c-4c: the GPU compaction made the arguments of every flush above, and the check frames' read-backs (every frame here) found them equal to the CPU's
    if (gpuCompact) {
        for (int i = 0; i < 4; ++i) {
            begin();
            end("ownedp_collect");  // (frames with no draws: the earlier frames' read-backs are collected once they completed)
        }
        const gpuscene::OwnedCompact::Stats& cs = compact.stats();
        std::printf("  owned compaction: %llu flushes / %llu items / %llu buckets / %llu dispatches, CPU fallbacks: full %llu, args differ %llu; checks %llu (%llu buckets, %llu records) DIFFERING %llu\n",
                    static_cast<unsigned long long>(cs.flushes), static_cast<unsigned long long>(cs.items), static_cast<unsigned long long>(cs.buckets), static_cast<unsigned long long>(cs.dispatches),
                    static_cast<unsigned long long>(cs.cpuFull), static_cast<unsigned long long>(cs.argsMismatch), static_cast<unsigned long long>(cs.checks), static_cast<unsigned long long>(cs.checkedBuckets),
                    static_cast<unsigned long long>(cs.checkedRecords), static_cast<unsigned long long>(cs.differing));
        if (cs.dispatches == 0 || cs.flushes == 0 || cs.checks == 0 || cs.checkedRecords == 0 || cs.differing != 0 || cs.countDiffering != 0) {
            std::printf("  FAIL owned compaction: no dispatches / no verified records / the GPU's arguments differ from the CPU's\n");
            ++g_failures;
        }
    }
    std::printf("  owned persistent: %llu derivations, %llu refused, %llu buckets / %llu indirect packets / %llu items; %llu slot writes, %llu unchanged, %llu allocated, %llu released, %llu reused\n",
                static_cast<unsigned long long>(dev.ownedPersistStats.derived), static_cast<unsigned long long>(dev.ownedPersistStats.refused), static_cast<unsigned long long>(dev.ownedPersistStats.buckets),
                static_cast<unsigned long long>(dev.ownedPersistStats.packets), static_cast<unsigned long long>(dev.ownedPersistStats.items), static_cast<unsigned long long>(tab.stats().writes),
                static_cast<unsigned long long>(tab.stats().same), static_cast<unsigned long long>(tab.stats().allocs), static_cast<unsigned long long>(tab.stats().releases),
                static_cast<unsigned long long>(tab.stats().reused));
    for (const std::uint32_t s : slots) {
        if (s != gpuscene::kNoSlot) tab.release(s);
    }
    tab.setActive(false);
    compact.destroy();
    tab.destroy();
    dev.setSceneViews(false);
    dev.setSceneDraws(false, 0);
    dev.release(std::move(vb));
    dev.release(std::move(ib));
}

// S-4 (native.vertex_pull, frame-ownership.md 14.12): persistent owned draws through the pulled vertex shaders (Device::computePullFetch -> CachedDraw::pull ->
// ownedDerive's fetch block -> DrawRecordTable's fetch table -> ownedFlush's pulled buckets -> the compaction with vertexOffset 0). Eight quads in two vertex layouts (two
// buffers, strides 28 and 48: two buckets with their vertex input, ONE pulled), drawn as the game draws them: a z pre-pass through the input assembler, then the owned
// colour draws with ZFUNC EQUAL. Every pixel must equal the input-assembler reference (a clip position one ulp off leaves the clear colour). Negative control: the
// position read as FLOAT3 instead of FLOAT4 (w = 1 + 1 ulp in the data becomes 1) must lose pixels. Guards: a slot without a fetch block is drawn with its vertex input.
void testOwnedPull(Fixture& f) {
    Device& dev = f.dev;
    dev.setSceneDraws(true, 0);
    if (!dev.sceneDrawsActive()) {
        std::printf("  SKIP owned pull: scene draws are not available on this device\n");
        return;
    }
    static const char* kVs = R"(
vs_3_0
dcl_position v0
dcl_color v1
dcl_position o0
dcl_color o1
dp4 o0.x, v0, c1
dp4 o0.y, v0, c2
dp4 o0.z, v0, c3
dp4 o0.w, v0, c4
add o1, v1, c0
)";
    static const char* kPs = R"(
ps_3_0
dcl_color v0
add r0, v0, c0
mov oC0, r0
)";
    const auto vsV = shaderFrom(dev, kVs);
    const auto psV = shaderFrom(dev, kPs);
    if (!vsV || !psV) {
        std::printf("  FAIL owned pull: shaders\n");
        ++g_failures;
        return;
    }
    const VertexElement kDeclB[] = {
        {0, 0, DECLTYPE_FLOAT4, 0, 0 /*POSITION*/, 0},
        {0, 32, DECLTYPE_D3DCOLOR, 0, 10 /*COLOR*/, 0},
        {0, 36, DECLTYPE_FLOAT2, 0, 5 /*TEXCOORD*/, 0},
        {kDeclEndStream, 0, DECLTYPE_UNUSED, 0, 0, 0},
    };
    auto declB = dev.createVertexDecl(std::span<const VertexElement>(kDeclB));
    constexpr std::uint32_t kQuads = 8, kSlots = 16, kStrideA = sizeof(Vertex), kStrideB = 48;
    const float w1 = std::nextafter(1.0f, 2.0f);  // (1 + 1 ulp: the negative control's FLOAT3 read makes it 1)
    std::vector<std::uint8_t> bytesA, bytesB;
    for (std::uint32_t k = 0; k < kQuads; ++k) {
        const float x0 = -0.93f + 0.231f * static_cast<float>(k), x1 = x0 + 0.217f;
        const std::uint32_t color = 0xFF000000u | (k * 21u + 10u) << 16 | (30u + k * 17u) << 8 | (k & 1u ? 140u : 20u);
        const float corner[4][2] = {{x0, 0.61f}, {x1, 0.57f}, {x0, -0.53f}, {x1, -0.49f}};
        for (const auto& c : corner) {
            const float pos[4] = {c[0], c[1], 0.3f + 0.037f * static_cast<float>(k), w1};
            std::vector<std::uint8_t>& out = k < 4 ? bytesA : bytesB;
            const std::uint32_t stride = k < 4 ? kStrideA : kStrideB;
            const std::size_t at = out.size();
            out.resize(at + stride, 0);
            std::memcpy(out.data() + at, pos, 16);
            std::memcpy(out.data() + at + (k < 4 ? 16 : 32), &color, 4);
        }
    }
    const std::uint16_t idx[6] = {0, 1, 2, 1, 3, 2};
    auto vbA = dev.createBuffer(static_cast<std::uint32_t>(bytesA.size()), USAGE_WRITEONLY, POOL_MANAGED, false, false);
    auto vbB = dev.createBuffer(static_cast<std::uint32_t>(bytesB.size()), USAGE_WRITEONLY, POOL_MANAGED, false, false);
    auto ib = dev.createBuffer(sizeof(idx), USAGE_WRITEONLY, POOL_MANAGED, true, false);
    std::memcpy(vbA->lock(0, 0, 0), bytesA.data(), bytesA.size());
    vbA->unlock();
    std::memcpy(vbB->lock(0, 0, 0), bytesB.data(), bytesB.size());
    vbB->unlock();
    std::memcpy(ib->lock(0, 0, 0), idx, sizeof(idx));
    ib->unlock();
    rhi::Device* rd = dev.rhi();
    auto tableBuf = rd->createBuffer({static_cast<std::uint64_t>(kSlots) * shader::abi::kViewBlock2Size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "owned pull view table"});
    if (!tableBuf) {
        std::printf("  FAIL owned pull: view table buffer\n");
        ++g_failures;
        return;
    }
    const auto tw = tableBuf->shaderRef(0);
    const rhi::GpuRef tableRef = rhi::GpuRef::fromWords(tw[0], tw[1]);
    std::vector<shader::abi::ViewBlock2> twin(kSlots);
    std::memset(twin.data(), 0, twin.size() * sizeof(shader::abi::ViewBlock2));
    // c0 the colour offset, c1..c4 a matrix of values no float holds exactly (the dot products round: a reordered or differently fed position differs in the last bits)
    const float vsC[5][4] = {{0.05f, 0.02f, 0.01f, 0.0f}, {0.9131f, 0.0313f, 0.0071f, 0.0137f}, {-0.0211f, 0.8873f, 0.0093f, -0.0119f}, {0.0123f, 0.0456f, 0.7891f, 0.1011f}, {0.0011f, -0.0007f, 0.0021f, 0.9973f}};
    const float psC0[4] = {0.10f, 0.05f, 0.02f, 0.0f};
    const auto setRegs = [&] {
        dev.setVertexShaderConstantF(0, vsC[0], 5);
        dev.setPixelShaderConstantF(0, psC0, 1);
    };
    const auto fillSlot = [&](std::uint32_t slot) {
        shader::abi::ViewBlock2& b = twin[slot];
        std::memset(&b, 0, sizeof(b));
        std::memcpy(b.vs0[0], vsC, sizeof(vsC));
        std::memcpy(b.ps0[0], psC0, 16);
        dev.viewPosFix(b.posFix);
        std::memcpy(tableBuf->map(), twin.data(), twin.size() * sizeof(shader::abi::ViewBlock2));
    };
    const auto windowsOf = [&](std::uint32_t slot) {
        OwnedWindows ow;
        const auto set = [&](OwnedWindows::Win& w, bool vertex, std::uint32_t win, std::uint32_t covered) {
            w.kind = OwnedWindows::Kind::View;
            w.base = rhi::GpuRef{gpuscene::ViewTable::windowBase(tableRef, slot, vertex, win)};
            w.covered = covered;
        };
        set(ow.w[0][0], true, 0, 12);
        set(ow.w[0][1], true, 1, 19);
        set(ow.w[1][0], false, 0, 5);
        ow.viewSlot = slot;
        return ow;
    };
    const auto bindQuad = [&](std::uint32_t k) {
        dev.setVertexDecl(k < 4 ? f.decl.get() : declB.get());
        dev.setStreamSource(0, k < 4 ? vbA.get() : vbB.get(), 0, k < 4 ? kStrideA : kStrideB);
    };
    const auto drawQuad = [&](std::uint32_t k) { dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * (k % 4)), 0, 4, 0, 2); };
    const auto begin = [&] {
        f.reset();
        dev.drawTag() = {};
        dev.drawTag().source = draws::Source::Mesh;
        dev.phases().push(scene::View::Main, scene::Pass::Solid);
        dev.setVertexShader(vsV);
        dev.setPixelShader(psV);
        dev.setIndices(ib.get());
        setRegs();
    };
    const auto zPrepass = [&] {
        dev.setRenderState(RS_ZENABLE, 1);
        dev.setRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        dev.setRenderState(RS_ZWRITEENABLE, 1);
        dev.setRenderState(RS_COLORWRITEENABLE, 0);
        for (std::uint32_t k = 0; k < kQuads; ++k) {
            bindQuad(k);
            drawQuad(k);
        }
        dev.setRenderState(RS_ZFUNC, CMP_EQUAL);
        dev.setRenderState(RS_ZWRITEENABLE, 0);
        dev.setRenderState(RS_COLORWRITEENABLE, 0xF);
    };
    gpuscene::DrawRecordTable& tab = gpuscene::drawrecords();
    tab.setActive(true);
    gpuscene::OwnedCompact& compact = gpuscene::ownedcompact();
    compact.setCheckEvery(1);
    const auto end = [&](const char* name) {
        tab.flush(dev);
        compact.flush(dev);
        dev.setSceneIssue(false);
        dev.phases().pop();
        return capture(dev, name);
    };
    const auto differ = [&](const Pixels& a, const Pixels& b) {
        std::uint32_t n = 0;
        for (std::size_t i = 0; i < a.bgra.size() && i < b.bgra.size(); ++i) n += a.bgra[i] != b.bgra[i];
        return a.bgra.size() != b.bgra.size() ? 0xffffffffu : n;
    };
    // references: the EQUAL colour pass through the input assembler after the input-assembler z pass; and the plain LESSEQUAL picture (the same: no holes)
    begin();
    zPrepass();
    for (std::uint32_t k = 0; k < kQuads; ++k) {
        bindQuad(k);
        drawQuad(k);
    }
    const Pixels refEq = end("ownedpull_ref_equal");
    begin();
    dev.setRenderState(RS_ZENABLE, 1);
    dev.setRenderState(RS_ZFUNC, CMP_LESSEQUAL);
    for (std::uint32_t k = 0; k < kQuads; ++k) {
        bindQuad(k);
        drawQuad(k);
    }
    const Pixels refLe = end("ownedpull_ref_lessequal");
    if (differ(refEq, refLe) != 0) {
        std::printf("  FAIL owned pull: the input-assembler EQUAL pass differs from its LESSEQUAL picture (%u bytes)\n", differ(refEq, refLe));
        ++g_failures;
    }
    if (!tab.ensure(dev)) {
        std::printf("  FAIL owned pull: the draw record tables\n");
        ++g_failures;
        return;
    }
    for (int i = 0; i < 1200 && !compact.usable(dev); ++i) Sleep(50);
    dev.setSceneViewTable(tableRef, twin.data(), kSlots);
    dev.setSceneViews(true);
    dev.setVertexPull(true);
    std::vector<Device::CachedDraw> cd(kQuads);
    bool ready = false;
    for (int i = 0; i < 1200 && !ready; ++i) {
        begin();
        zPrepass();
        dev.setSceneIssue(true);
        for (std::uint32_t k = 0; k < kQuads; ++k) {
            bindQuad(k);
            dev.beginCapture(&cd[k]);
            drawQuad(k);
            dev.endCapture();
        }
        end("ownedpull_capture");
        ready = true;
        for (const auto& c : cd) ready = ready && c.valid && c.scenePipeline2 != GamePipeline::None && c.scenePipelinePull != GamePipeline::None && c.pull.valid == shader::abi::kVertexFetchValid;
        if (!ready) Sleep(50);
    }
    if (!ready) {
        std::printf("  FAIL owned pull: the draws never got their pulled pipelines (layouts pullable %llu, refused format %llu / stream %llu / address %llu / alignment %llu)\n",
                    static_cast<unsigned long long>(dev.vertexPullStats().layoutOk), static_cast<unsigned long long>(dev.vertexPullStats().layoutRefused[0]),
                    static_cast<unsigned long long>(dev.vertexPullStats().layoutRefused[1]), static_cast<unsigned long long>(dev.vertexPullStats().layoutRefused[2]),
                    static_cast<unsigned long long>(dev.vertexPullStats().layoutRefused[3]));
        ++g_failures;
        dev.setVertexPull(false);
        return;
    }
    if (cd[0].pull.stride[0] != kStrideA || cd[4].pull.stride[0] != kStrideB || cd[5].pull.baseVertex != 4 || cd[0].pull.attr[1] == 0) {
        std::printf("  FAIL owned pull: fetch blocks: strides %u / %u, base vertex of quad 5 %d, colour word %#x\n", cd[0].pull.stride[0], cd[4].pull.stride[0], cd[5].pull.baseVertex, cd[0].pull.attr[1]);
        ++g_failures;
    }
    fillSlot(3);
    std::uint32_t slots[kQuads];
    for (auto& s : slots) s = tab.alloc();
    // one frame: the input-assembler z pass, then the eight owned colour draws (derived on view slot 3; `mutate` edits a derivation before it is written)
    const auto frame = [&](const char* name, auto mutate, std::uint64_t& calls, std::uint64_t& pulled) {
        begin();
        zPrepass();
        dev.setSceneIssue(true);
        std::vector<Device::OwnedItem> items;
        for (std::uint32_t k = 0; k < kQuads; ++k) {
            Device::DeriveIn in;
            in.streams[0] = k < 4 ? vbA.get() : vbB.get();
            in.strides[0] = k < 4 ? kStrideA : kStrideB;
            in.indices = ib.get();
            in.alphaFunc = 8u;
            Device::OwnedPersist out;
            const int rc = dev.ownedDerive(cd[k], in, windowsOf(3), tab.blockRef(slots[k]), tab.zeroRef(), true, out);
            if (rc != 0) {
                std::printf("  FAIL owned pull (%s): quad %u derive returned %d\n", name, k, rc);
                ++g_failures;
            }
            mutate(k, out);
            tab.write(slots[k], out.rec, out.blk, &out.fetch);
            items.push_back({&cd[k], out.bucketKey, slots[k]});
        }
        const auto s0 = dev.sceneStats();
        const std::uint64_t p0 = dev.vertexPullActs();
        Device::OwnedArena arena;
        if (!dev.ownedReserve(kQuads, arena) || !dev.ownedFlush(items, arena, 3, tab.recordsRef(), tab.recordsShadow(), gpuscene::DrawRecordTable::kCapacity)) {
            std::printf("  FAIL owned pull (%s): the flush was refused\n", name);
            ++g_failures;
        }
        calls = dev.sceneStats().indirectCalls - s0.indirectCalls;
        pulled = dev.vertexPullActs() - p0;
        return end(name);
    };
    std::uint64_t calls = 0, pulled = 0;
    const auto none = [](std::uint32_t, Device::OwnedPersist&) {};
    // 1. pulled: two layouts, ONE indirect command, the reference's pixels
    for (int round = 0; round < 2; ++round) {  // (round 2: the slots hold their blocks already)
        const Pixels p = frame("ownedpull_on", none, calls, pulled);
        if (differ(refEq, p) != 0 || calls != 1 || pulled != kQuads) {
            std::printf("  FAIL owned pull (round %d): %u bytes differ from the input-assembler EQUAL picture, %llu indirect commands (expected 1), %llu of %u items pulled\n", round, differ(refEq, p),
                        static_cast<unsigned long long>(calls), static_cast<unsigned long long>(pulled), kQuads);
            ++g_failures;
        }
    }
    // 2. not pulled (the setting off: the derivation writes no fetch block): two buckets, the same pixels
    dev.setVertexPull(false);
    {
        const Pixels p = frame("ownedpull_off", none, calls, pulled);
        if (differ(refEq, p) != 0 || calls != 2 || pulled != 0) {
            std::printf("  FAIL owned pull (off): %u bytes differ, %llu indirect commands (expected 2), %llu pulled\n", differ(refEq, p), static_cast<unsigned long long>(calls), static_cast<unsigned long long>(pulled));
            ++g_failures;
        }
    }
    dev.setVertexPull(true);
    // 3. a slot without a valid fetch block (GPU safety): drawn with its vertex input in a bucket of its own, the same pixels
    {
        const std::uint64_t nf0 = dev.vertexPullStats().noFetch;
        const Pixels p = frame("ownedpull_guard", [](std::uint32_t k, Device::OwnedPersist& out) { if (k == 6) out.fetch.valid = 0; }, calls, pulled);
        if (differ(refEq, p) != 0 || calls != 2 || pulled != kQuads - 1 || dev.vertexPullStats().noFetch != nf0 + 1) {
            std::printf("  FAIL owned pull (guard): %u bytes differ, %llu indirect commands (expected 2), %llu pulled (expected 7), no-fetch refusals %llu\n", differ(refEq, p),
                        static_cast<unsigned long long>(calls), static_cast<unsigned long long>(pulled), static_cast<unsigned long long>(dev.vertexPullStats().noFetch - nf0));
            ++g_failures;
        }
    }
    // 4. negative control: the position read as FLOAT3 (w = 1 instead of 1 + 1 ulp): the EQUAL pass must lose pixels (the check above can see a last-bit difference)
    {
        const auto asFloat3 = [](std::uint32_t, Device::OwnedPersist& out) {
            const std::uint32_t w = out.fetch.attr[0];
            out.fetch.attr[0] = (w & ~(0xffu << shader::abi::kPullAttrFormatShift)) | static_cast<std::uint32_t>(shader::abi::PullFormat::Float3) << shader::abi::kPullAttrFormatShift;
        };
        const Pixels p = frame("ownedpull_negative", asFloat3, calls, pulled);
        const std::uint32_t n = differ(refEq, p);
        std::printf("  owned pull: negative control (position w one ulp off): %u bytes differ\n", n);
        if (n == 0) {
            std::printf("  FAIL owned pull: the negative control did not change the picture (the EQUAL check cannot see a last-bit difference)\n");
            ++g_failures;
        }
    }
    const gpuscene::OwnedCompact::Stats& cs = compact.stats();
    if (cs.differing != 0 || cs.countDiffering != 0) {
        std::printf("  FAIL owned pull: the GPU compaction's arguments differ from the CPU's (%llu records, %llu counts)\n", static_cast<unsigned long long>(cs.differing), static_cast<unsigned long long>(cs.countDiffering));
        ++g_failures;
    }
    std::printf("  owned pull: compaction checks %llu, records %llu; table fetch ref %s\n", static_cast<unsigned long long>(cs.checks), static_cast<unsigned long long>(cs.checkedRecords),
                tab.fetchRef().null() ? "null" : "set");
    for (const std::uint32_t s : slots) tab.release(s);
    dev.setVertexPull(false);
    tab.setActive(false);
    compact.destroy();
    tab.destroy();
    dev.setSceneViews(false);
    dev.setSceneDraws(false, 0);
    dev.release(std::move(vbA));
    dev.release(std::move(vbB));
    dev.release(std::move(ib));
    dev.release(std::move(declB));
}

// F5-2 cluster cull through the owned path (frame-ownership.md 7.34, native.cluster_cull): a 40 x 40 grid (3,200 triangles, x / y in [-3, 3]: the viewport shows a ninth of it)
// is drawn as a clustered static mesh by the owned persistent path (the record carries its cluster rows, the compaction pass culls with the view table's c0..c3 and the object
// window's world rows). The picture must equal the normal draw's in four situations: (A) the grid inside a unit clip cube: most clusters culled; (B) the object moved
// out of the view: nothing drawn; (C) a view whose frustum holds the whole grid: one run over the whole range, nothing culled; (D) the setting off. The check frames' exact
// per-cluster evaluation on the CPU (no margin) finds no false negative; the indices submitted drop in (A) and (B).
void testClusterCull(Fixture& f) {
    Device& dev = f.dev;
    dev.setSceneDraws(true, 0);
    if (!dev.sceneDrawsActive()) {
        std::printf("  SKIP cluster cull: scene draws are not available on this device\n");
        return;
    }
    // world = (x, y, z) * rows (c33..c35), fade c32.w; the clip position is the world position (the view block's c0..c3 describe the same frustum)
    static const char* kVs = R"(
vs_3_0
dcl_position v0
dcl_color v1
dcl_position o0
dcl_color o1
mov r0.xyz, v0
mov r0.w, c32.w
dp4 o0.x, r0, c33
dp4 o0.y, r0, c34
dp4 o0.z, r0, c35
mov o0.w, c32.w
mov o1, v1
)";
    static const char* kPs = R"(
ps_3_0
dcl_color v0
mov oC0, v0
)";
    const auto vsV = shaderFrom(dev, kVs);
    const auto psV = shaderFrom(dev, kPs);
    const VertexElement kDeclC[] = {
        {0, 0, DECLTYPE_FLOAT3, 0, 0 /*POSITION*/, 0},
        {0, 12, DECLTYPE_D3DCOLOR, 0, 10 /*COLOR*/, 0},
        {kDeclEndStream, 0, DECLTYPE_UNUSED, 0, 0, 0},
    };
    auto decl = dev.createVertexDecl(kDeclC);
    if (!vsV || !psV || !decl) {
        std::printf("  FAIL cluster cull: shaders / declaration\n");
        ++g_failures;
        return;
    }
    vsV->semantics.flags |= ShaderSemantics::Material;
    vsV->semantics.ranges[static_cast<std::size_t>(Semantic::InstanceData)] = {32, 1};
    constexpr int kN = 40;
    struct V {
        float x, y, z;
        std::uint32_t color;
    };
    std::vector<V> verts;
    for (int y = 0; y <= kN; ++y) {
        for (int x = 0; x <= kN; ++x) {
            const std::uint32_t c = 0xFF000000u | (static_cast<std::uint32_t>(x * 6) & 255u) << 16 | (static_cast<std::uint32_t>(y * 6) & 255u) << 8 | static_cast<std::uint32_t>((x * y) & 255);
            verts.push_back({-3.0f + 6.0f * static_cast<float>(x) / kN, -3.0f + 6.0f * static_cast<float>(y) / kN, 0.5f, c});
        }
    }
    std::vector<std::uint16_t> idx;
    for (int y = 0; y < kN; ++y) {
        for (int x = 0; x < kN; ++x) {
            const std::uint16_t a = static_cast<std::uint16_t>(y * (kN + 1) + x), b = a + 1, c = static_cast<std::uint16_t>(a + kN + 1), d = c + 1;
            if ((x + y) & 1) idx.insert(idx.end(), {a, b, c, b, d, c});
            else idx.insert(idx.end(), {a, d, c, a, b, d});
        }
    }
    const std::uint32_t tris = static_cast<std::uint32_t>(idx.size() / 3);
    auto vb = dev.createBuffer(static_cast<std::uint32_t>(verts.size() * sizeof(V)), USAGE_WRITEONLY, POOL_MANAGED, false, false);
    auto ib = dev.createBuffer(static_cast<std::uint32_t>(idx.size() * 2), USAGE_WRITEONLY, POOL_MANAGED, true, false);
    std::memcpy(vb->lock(0, 0, 0), verts.data(), verts.size() * sizeof(V));
    vb->unlock();
    std::memcpy(ib->lock(0, 0, 0), idx.data(), idx.size() * 2);
    ib->unlock();
    rhi::Device* rd = dev.rhi();
    constexpr std::uint32_t kSlots = 16, kObjPad = 512;
    auto tableBuf = rd->createBuffer({static_cast<std::uint64_t>(kSlots) * shader::abi::kViewBlock2Size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "cluster cull view table"});
    auto objBuf = rd->createBuffer({kObjPad + 4 * 96, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "cluster cull objects"});
    if (!tableBuf || !objBuf) {
        std::printf("  FAIL cluster cull: view / object table buffers\n");
        ++g_failures;
        return;
    }
    const auto tw = tableBuf->shaderRef(0);
    const rhi::GpuRef tableRef = rhi::GpuRef::fromWords(tw[0], tw[1]);
    std::vector<shader::abi::ViewBlock2> twin(kSlots);
    std::memset(twin.data(), 0, twin.size() * sizeof(shader::abi::ViewBlock2));
    // object records: A = identity, B = moved 10 units to the right, C = scaled 0.5 + moved to the left (half of it still visible)
    struct Obj {
        float rows[3][4];
        float fade;
    };
    const Obj objs[3] = {{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}, 1.0f}, {{{1, 0, 0, 10}, {0, 1, 0, 0}, {0, 0, 1, 0}}, 1.0f}, {{{0.5f, 0, 0, -0.4f}, {0, 0.5f, 0, 0.2f}, {0, 0, 1, 0}}, 1.0f}};
    {
        auto* base = static_cast<float*>(objBuf->map());
        std::memset(base, 0, kObjPad + 4 * 96);
        for (std::uint32_t o = 0; o < 3; ++o) {
            float* r = base + (kObjPad + o * 96) / 4;  // c31 .. c35
            r[4 + 3] = objs[o].fade;                   // c32.w
            std::memcpy(r + 8, objs[o].rows, sizeof(objs[o].rows));
        }
    }
    const auto objBase = [&](std::uint32_t o) {
        const auto w = objBuf->shaderRef(kObjPad + o * 96 - 31 * 16);
        return rhi::GpuRef::fromWords(w[0], w[1]);
    };
    const auto setObject = [&](std::uint32_t o) {
        const float c32[4] = {0, 0, 0, objs[o].fade};
        dev.setVertexShaderConstantF(32, c32, 1);
        dev.setVertexShaderConstantF(33, objs[o].rows[0], 3);
    };
    const auto fillView = [&](std::uint32_t slot, float sx, float sy) {
        shader::abi::ViewBlock2& b = twin[slot];
        std::memset(&b, 0, sizeof(b));
        b.vs0[0][0] = sx;
        b.vs0[1][1] = sy;
        b.vs0[2][2] = 1.0f;
        b.vs0[3][3] = 1.0f;
        dev.viewPosFix(b.posFix);
        std::memcpy(tableBuf->map(), twin.data(), twin.size() * sizeof(shader::abi::ViewBlock2));
    };
    const auto windows = [&](std::uint32_t slot, std::uint32_t obj) {
        OwnedWindows ow;
        const auto set = [&](OwnedWindows::Win& w, bool vertex, std::uint32_t win, std::uint32_t covered) {
            w.kind = OwnedWindows::Kind::View;
            w.base = rhi::GpuRef{gpuscene::ViewTable::windowBase(tableRef, slot, vertex, win)};
            w.covered = covered;
        };
        set(ow.w[0][0], true, 0, 12);
        set(ow.w[0][1], true, 1, 19);
        set(ow.w[1][0], false, 0, 5);
        ow.w[0][2].kind = OwnedWindows::Kind::Object;
        ow.w[0][2].base = objBase(obj);
        ow.w[0][2].covered = 5;
        ow.viewSlot = slot;
        return ow;
    };
    gpuscene::ClusterStore& store = gpuscene::clusterstore();
    const auto begin = [&] {
        f.reset();
        dev.drawTag() = {};
        dev.drawTag().source = draws::Source::Mesh;
        dev.phases().push(scene::View::Main, scene::Pass::Solid);
        dev.setVertexDecl(decl.get());
        dev.setVertexShader(vsV);
        dev.setPixelShader(psV);
        dev.setStreamSource(0, vb.get(), 0, sizeof(V));
        dev.setIndices(ib.get());
    };
    gpuscene::DrawRecordTable& tab = gpuscene::drawrecords();
    tab.setActive(true);
    if (!tab.ensure(dev)) {
        std::printf("  FAIL cluster cull: the draw record tables\n");
        ++g_failures;
        return;
    }
    gpuscene::OwnedCompact& compact = gpuscene::ownedcompact();
    compact.setCheckEvery(1);
    bool gpuCompact = false;
    for (int i = 0; i < 1200 && !gpuCompact; ++i) {
        gpuCompact = compact.usable(dev);
        if (!gpuCompact) Sleep(50);
    }
    if (!gpuCompact) {
        std::printf("  SKIP cluster cull: GPU compaction is not available here\n");
        tab.setActive(false);
        tab.destroy();
        dev.setSceneDraws(false, 0);
        return;
    }
    // (the CPU twin of the object window for the check frames: the test's own buffer stands in for the object table)
    dev.setCullWorldRowsHook([&](std::uint64_t baseBits, float world[12]) {
        for (std::uint32_t o = 0; o < 3; ++o) {
            if (objBase(o).bits == baseBits) {
                std::memcpy(world, objs[o].rows, 12 * sizeof(float));
                return true;
            }
        }
        return false;
    });
    const auto end = [&](const char* name) {
        store.flush(dev);
        tab.flush(dev);
        compact.flush(dev);
        dev.setSceneIssue(false);
        dev.phases().pop();
        return capture(dev, name);
    };
    const auto bad = [&](const Pixels& ref, const Pixels& p, const char* what) {
        std::uint32_t n = 0;
        for (std::size_t i = 0; i < ref.bgra.size() && i < p.bgra.size(); ++i) n += ref.bgra[i] != p.bgra[i];
        if (n != 0 || ref.bgra.size() != p.bgra.size()) {
            std::printf("  FAIL cluster cull (%s): %u bytes differ from the normal draw\n", what, n);
            ++g_failures;
        }
        return n;
    };
    const auto lit = [](const Pixels& p) {
        std::uint32_t n = 0;
        for (std::size_t i = 0; i + 3 < p.bgra.size(); i += 4) n += (p.bgra[i] | p.bgra[i + 1] | p.bgra[i + 2]) != 0;
        return n;
    };
    // references: the normal draws of objects A, B, C
    Pixels ref[3];
    for (std::uint32_t o = 0; o < 3; ++o) {
        begin();
        setObject(o);
        dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, static_cast<std::uint32_t>(verts.size()), 0, tris);
        ref[o] = end("clustercull_ref");
    }
    if (lit(ref[0]) < 100 || lit(ref[1]) != 0 || lit(ref[2]) < 100) {
        std::printf("  FAIL cluster cull: the references are not what the test needs (lit pixels %u / %u / %u)\n", lit(ref[0]), lit(ref[1]), lit(ref[2]));
        ++g_failures;
    }
    // the clusters: ask, wait for the worker, capture the draw that takes the copy
    dev.setClusters(true);
    dev.setClusterCull(true);
    dev.setSceneViewTable(tableRef, twin.data(), kSlots);
    dev.setSceneViews(true);
    fillView(1, 1.0f, 1.0f);   // the unit clip cube
    fillView(0, 0.25f, 0.25f); // x / y in [-4, 4]: the whole grid
    Device::CachedDraw cd;
    bool ready = false;
    for (int i = 0; i < 1600 && !ready; ++i) {
        begin();
        setObject(0);
        dev.setSceneIssue(true);
        dev.beginCapture(&cd);
        dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, static_cast<std::uint32_t>(verts.size()), 0, tris);
        dev.endCapture();
        end("clustercull_capture");
        ready = cd.valid && cd.scenePipeline != GamePipeline::None && cd.scenePipeline2 != GamePipeline::None && cd.clusterUsing;
        if (!ready) Sleep(25);
    }
    if (!ready) {
        std::printf("  FAIL cluster cull: the draw never took the clustered copy / got its v2 pipelines (valid %d, pipelines %d / %d, clusters: using %d, requested %llu built %llu failed %llu)\n", cd.valid,
                    static_cast<int>(cd.scenePipeline), static_cast<int>(cd.scenePipeline2), cd.clusterUsing, static_cast<unsigned long long>(store.stats().requested),
                    static_cast<unsigned long long>(store.stats().built), static_cast<unsigned long long>(store.stats().failedBuild));
        ++g_failures;
        dev.setCullWorldRowsHook(nullptr);
        compact.destroy();
        tab.setActive(false);
        tab.destroy();
        store.destroy(&dev);
        dev.setClusters(false);
        dev.setClusterCull(false);
        dev.setSceneViews(false);
        dev.setSceneDraws(false, 0);
        return;
    }
    std::uint32_t slot = tab.alloc();
    const auto owned = [&](const char* name, std::uint32_t viewSlot, std::uint32_t obj, const Pixels& want, const char* what) {
        begin();
        setObject(obj);
        dev.setSceneIssue(true);
        Device::DeriveIn in;
        in.streams[0] = vb.get();
        in.strides[0] = sizeof(V);
        in.indices = ib.get();
        in.alphaFunc = 8u;
        const OwnedWindows ow = windows(viewSlot, obj);
        Device::OwnedPersist out;
        const int rc = dev.ownedDerive(cd, in, ow, tab.blockRef(slot), tab.zeroRef(), true, out);
        if (rc != 0) {
            std::printf("  FAIL cluster cull (%s): derive returned %d\n", what, rc);
            ++g_failures;
        }
        tab.write(slot, out.rec, out.blk);
        const bool culled = (out.rec.args[3] >> 16) != 0;
        std::vector<Device::OwnedItem> items = {{&cd, out.bucketKey, slot}};
        Device::OwnedArena arena;
        if (!dev.ownedReserve(1, arena) || !dev.ownedFlush(items, arena, viewSlot, tab.recordsRef(), tab.recordsShadow(), gpuscene::DrawRecordTable::kCapacity)) {
            std::printf("  FAIL cluster cull (%s): the flush was refused\n", what);
            ++g_failures;
        }
        const Pixels p = end(name);
        bad(want, p, what);
        return culled;
    };
    const gpuscene::OwnedCompact::Stats s0 = compact.stats();
    const bool carriesRows = owned("clustercull_a", 1, 0, ref[0], "A: most clusters outside the clip cube");
    if (!carriesRows) {
        std::printf("  FAIL cluster cull: the record does not carry cluster rows (entry not Ready / contiguous, or the packet is not the copy's range)\n");
        ++g_failures;
    }
    owned("clustercull_b", 1, 1, ref[1], "B: the object outside the view");
    owned("clustercull_c", 0, 0, ref[0], "C: a frustum holding the whole grid");
    owned("clustercull_c2", 1, 2, ref[2], "A2: scaled and moved, half visible");
    dev.setClusterCull(false);
    owned("clustercull_d", 1, 0, ref[0], "D: the setting off");
    dev.setClusterCull(true);
    // F3-2 (7.37): a sun cascade's view slot (4 .. 7) with native.cluster_cull_shadows: the cull tests the four SIDE planes only. The test's vertex shader takes the position from the
    // object rows, so the view block only decides the cull: slot 4 = the unit clip cube (A: most clusters outside the sides: culled, B: outside: nothing, A2 half), slot 5 = a block whose
    // NEAR plane lies at z >= 2 (every cluster of the grid is "in front of the near plane": a main-view cull would drop the whole draw, a cascade keeps it: pixels equal the reference)
    dev.setClusterCullShadows(true);
    fillView(4, 1.0f, 1.0f);
    const std::uint64_t cascadeBefore = compact.stats().idxAfter[3];
    owned("clustercull_e", 4, 0, ref[0], "E: a cascade slot, most clusters outside the side planes");
    owned("clustercull_f", 4, 1, ref[1], "F: a cascade slot, the object outside");
    owned("clustercull_g", 4, 2, ref[2], "G: a cascade slot, half visible");
    fillView(5, 0.25f, 0.25f);
    twin[5].vs0[2][3] = -2.0f;  // clip z = z - 2 < 0 for the whole grid
    std::memcpy(tableBuf->map(), twin.data(), twin.size() * sizeof(shader::abi::ViewBlock2));
    owned("clustercull_h", 5, 0, ref[0], "H: a cascade slot whose near plane is beyond the object: kept (side planes only)");
    {
        // the control: the same block on a MAIN slot culls the whole draw (the near plane applies): the picture is empty, which differs from the reference
        fillView(1, 0.25f, 0.25f);
        twin[1].vs0[2][3] = -2.0f;
        std::memcpy(tableBuf->map(), twin.data(), twin.size() * sizeof(shader::abi::ViewBlock2));
        const Pixels before = ref[0];
        begin();
        setObject(0);
        dev.setSceneIssue(true);
        Device::DeriveIn in;
        in.streams[0] = vb.get();
        in.strides[0] = sizeof(V);
        in.indices = ib.get();
        in.alphaFunc = 8u;
        const OwnedWindows ow = windows(1, 0);
        Device::OwnedPersist out;
        const int rc = dev.ownedDerive(cd, in, ow, tab.blockRef(slot), tab.zeroRef(), true, out);
        tab.write(slot, out.rec, out.blk);
        std::vector<Device::OwnedItem> items = {{&cd, out.bucketKey, slot}};
        Device::OwnedArena arena;
        const bool flushed = rc == 0 && dev.ownedReserve(1, arena) && dev.ownedFlush(items, arena, 1, tab.recordsRef(), tab.recordsShadow(), gpuscene::DrawRecordTable::kCapacity);
        const Pixels p = end("clustercull_control");
        if (!flushed || lit(p) >= lit(before) / 4) {
            std::printf("  FAIL cluster cull: the control (a main slot whose near plane is beyond the object) drew %u lit pixels of %u (the near plane must cull it), flushed %d\n", lit(p), lit(before), flushed);
            ++g_failures;
        }
        fillView(1, 1.0f, 1.0f);
    }
    if (compact.stats().idxAfter[3] == cascadeBefore) {
        std::printf("  FAIL cluster cull: no cascade flush was counted (view kind 3)\n");
        ++g_failures;
    }
    dev.setClusterCullShadows(false);
    owned("clustercull_i", 4, 0, ref[0], "I: a cascade slot with native.cluster_cull_shadows off: whole range");
    for (int i = 0; i < 4; ++i) {
        begin();
        end("clustercull_collect");  // (the read-backs of the earlier frames complete)
    }
    const gpuscene::OwnedCompact::Stats& cs = compact.stats();
    const auto d = [&](std::uint64_t a, std::uint64_t b) { return static_cast<unsigned long long>(a - b); };
    std::printf("  cluster cull: %llu culled-capable items, %llu items checked, %llu clusters (%llu inside by the exact test, %llu drawn), false negatives %llu, extras %llu, whole-range %llu, empty %llu, runs %llu, "
                "inexact %llu; indices %llu -> %llu; compaction DIFFERING %llu\n",
                d(cs.cullItems, s0.cullItems), d(cs.cullChecked, s0.cullChecked), d(cs.cullClusters, s0.cullClusters), d(cs.cullExactVisible, s0.cullExactVisible), d(cs.cullCovered, s0.cullCovered),
                d(cs.falseNegatives, s0.falseNegatives), d(cs.extras, s0.extras), d(cs.wholeRange, s0.wholeRange), d(cs.emptyItems, s0.emptyItems), d(cs.runs, s0.runs), d(cs.inexact, s0.inexact),
                d(cs.idxBefore[0] + cs.idxBefore[1], s0.idxBefore[0] + s0.idxBefore[1]), d(cs.idxAfter[0] + cs.idxAfter[1], s0.idxAfter[0] + s0.idxAfter[1]), d(cs.differing, s0.differing));
    if (cs.cullChecked - s0.cullChecked < 4 || cs.cullItems - s0.cullItems < 4 || cs.falseNegatives != 0 || cs.differing != 0 || cs.inexact != 0 || cs.badRuns != 0) {
        std::printf("  FAIL cluster cull: checks (items checked %llu, culled-capable %llu, false negatives %llu, differing %llu, inexact %llu, bad runs %llu)\n", d(cs.cullChecked, s0.cullChecked),
                    d(cs.cullItems, s0.cullItems), static_cast<unsigned long long>(cs.falseNegatives), static_cast<unsigned long long>(cs.differing), static_cast<unsigned long long>(cs.inexact),
                    static_cast<unsigned long long>(cs.badRuns));
        ++g_failures;
    }
    // A + B + C + A2 + D: the submitted indices (all views here are main solid) are well below five whole draws, and B drew nothing
    const std::uint64_t whole = static_cast<std::uint64_t>(tris) * 3 * 5, after = cs.idxAfter[0] + cs.idxAfter[1] - s0.idxAfter[0] - s0.idxAfter[1];
    if (cs.emptyItems - s0.emptyItems < 1 || cs.wholeRange - s0.wholeRange < 1 || after >= whole - whole / 4 || cs.extras - s0.extras > (cs.cullClusters - s0.cullClusters) / 2) {
        std::printf("  FAIL cluster cull: no item without a record (%llu), no whole-range item (%llu), or the cull did not reduce the submitted indices (%llu of %llu)\n", d(cs.emptyItems, s0.emptyItems),
                    d(cs.wholeRange, s0.wholeRange), static_cast<unsigned long long>(after), static_cast<unsigned long long>(whole));
        ++g_failures;
    }
    tab.release(slot);
    dev.setCullWorldRowsHook(nullptr);
    tab.setActive(false);
    compact.destroy();
    tab.destroy();
    store.destroy(&dev);
    dev.setClusters(false);
    dev.setClusterCull(false);
    dev.setSceneViews(false);
    dev.setSceneDraws(false, 0);
    dev.release(std::move(vb));
    dev.release(std::move(ib));
}

// F5-1 clusters of large static meshes through the device's own path (frame-ownership.md 7.33, native.clusters): a 40 x 40 quad grid (3,200 triangles, 16-bit indices)
// is drawn as a static mesh (a rigid material vertex shader: the semantics are set by hand, POSITION FLOAT3 at offset 0) (a) the normal way and (b) with the setting on: the
// first draw asks the worker, a later one is drawn from the arena copy of the indices in cluster order. 0 bytes may differ from (a); the copy's read-back (Vulkan) and the
// multiset check of the ordered triples pass; a rewritten index buffer makes a new range (a new copy, the old one freed); off = nothing left.
void testClusters(Fixture& f) {
    Device& dev = f.dev;
    static const char* kVs = R"(
vs_3_0
dcl_position v0
dcl_color v1
dcl_position o0
dcl_color o1
mov o0, v0
mov o1, v1
)";
    static const char* kPs = R"(
ps_3_0
dcl_color v0
mov oC0, v0
)";
    const auto vsV = shaderFrom(dev, kVs);
    const auto psV = shaderFrom(dev, kPs);
    const VertexElement kDeclC[] = {
        {0, 0, DECLTYPE_FLOAT3, 0, 0 /*POSITION*/, 0},
        {0, 12, DECLTYPE_D3DCOLOR, 0, 10 /*COLOR*/, 0},
        {kDeclEndStream, 0, DECLTYPE_UNUSED, 0, 0, 0},
    };
    auto decl = dev.createVertexDecl(kDeclC);
    if (!vsV || !psV || !decl) {
        std::printf("  FAIL clusters: shaders / declaration\n");
        ++g_failures;
        return;
    }
    vsV->semantics.flags |= ShaderSemantics::Material;
    vsV->semantics.ranges[static_cast<std::size_t>(Semantic::InstanceData)] = {32, 1};
    constexpr int kN = 40;
    struct V {
        float x, y, z;
        std::uint32_t color;
    };
    std::vector<V> verts;
    for (int y = 0; y <= kN; ++y) {
        for (int x = 0; x <= kN; ++x) {
            const std::uint32_t c = 0xFF000000u | (static_cast<std::uint32_t>(x * 6) & 255u) << 16 | (static_cast<std::uint32_t>(y * 6) & 255u) << 8 | static_cast<std::uint32_t>((x * y) & 255);
            verts.push_back({-0.9f + 1.8f * static_cast<float>(x) / kN, -0.9f + 1.8f * static_cast<float>(y) / kN, 0.5f, c});
        }
    }
    std::vector<std::uint16_t> idx;
    for (int y = 0; y < kN; ++y) {
        for (int x = 0; x < kN; ++x) {
            const std::uint16_t a = static_cast<std::uint16_t>(y * (kN + 1) + x), b = a + 1, c = static_cast<std::uint16_t>(a + kN + 1), d = c + 1;
            if ((x + y) & 1) idx.insert(idx.end(), {a, b, c, b, d, c});
            else idx.insert(idx.end(), {a, d, c, a, b, d});
        }
    }
    const std::uint32_t tris = static_cast<std::uint32_t>(idx.size() / 3);
    auto vb = dev.createBuffer(static_cast<std::uint32_t>(verts.size() * sizeof(V)), USAGE_WRITEONLY, POOL_MANAGED, false, false);
    auto ib = dev.createBuffer(static_cast<std::uint32_t>(idx.size() * 2), USAGE_WRITEONLY, POOL_MANAGED, true, false);
    std::memcpy(vb->lock(0, 0, 0), verts.data(), verts.size() * sizeof(V));
    vb->unlock();
    const auto writeIndices = [&](const std::vector<std::uint16_t>& v) {
        std::memcpy(ib->lock(0, 0, 0), v.data(), v.size() * 2);
        ib->unlock();
    };
    writeIndices(idx);
    gpuscene::ClusterStore& store = gpuscene::clusterstore();
    const auto frame = [&](const char* name) {
        f.reset();
        dev.drawTag() = {};
        dev.drawTag().source = draws::Source::Mesh;
        dev.phases().push(scene::View::Main, scene::Pass::Solid);
        dev.setVertexDecl(decl.get());
        dev.setVertexShader(vsV);
        dev.setPixelShader(psV);
        dev.setStreamSource(0, vb.get(), 0, sizeof(V));
        dev.setIndices(ib.get());
        dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, static_cast<std::uint32_t>(verts.size()), 0, tris);
        store.flush(dev);
        dev.phases().pop();
        return capture(dev, name);
    };
    const auto differ = [&](const Pixels& a, const Pixels& b, const char* what) {
        std::uint32_t n = 0;
        for (std::size_t i = 0; i < a.bgra.size() && i < b.bgra.size(); ++i) n += a.bgra[i] != b.bgra[i];
        if (n != 0 || a.bgra.size() != b.bgra.size()) {
            std::printf("  FAIL clusters (%s): %u bytes differ from the normal draw\n", what, n);
            ++g_failures;
        }
    };
    // the variant B of the indices: one triangle with its vertices rotated (the same triangle, another ordered triple), for the rewrite step
    std::vector<std::uint16_t> idxB = idx;
    std::rotate(idxB.begin() + 30, idxB.begin() + 31, idxB.begin() + 33);
    const Pixels refA = frame("clusters_ref_a");
    writeIndices(idxB);
    const Pixels refB = frame("clusters_ref_b");
    writeIndices(idx);
    const auto waitReady = [&](std::uint64_t uploaded, const char* what) {
        for (int i = 0; i < 400; ++i) {
            frame("clusters_wait");
            if (store.stats().uploaded >= uploaded) return true;
            Sleep(25);
        }
        std::printf("  FAIL clusters: %s never got its clustered copy (requested %llu built %llu failed build %llu)\n", what, static_cast<unsigned long long>(store.stats().requested),
                    static_cast<unsigned long long>(store.stats().built), static_cast<unsigned long long>(store.stats().failedBuild));
        ++g_failures;
        return false;
    };
    dev.setClusters(true);
    if (waitReady(1, "the grid")) {
        const std::uint64_t used0 = store.stats().perDrawUsed;
        differ(refA, frame("clusters_a1"), "range A, first clustered draw");
        differ(refA, frame("clusters_a2"), "range A, again");
        if (store.stats().perDrawUsed < used0 + 2) {
            std::printf("  FAIL clusters: the draws did not take the clustered copy (%llu uses)\n", static_cast<unsigned long long>(store.stats().perDrawUsed));
            ++g_failures;
        }
        // the rewrite: another generation of the index buffer = another range: a new copy, the old one freed at the next drain
        writeIndices(idxB);
        if (waitReady(2, "the rewritten grid")) {
            differ(refB, frame("clusters_b1"), "range B");
            frame("clusters_free");
            if (store.stats().freed < 1) {
                std::printf("  FAIL clusters: the copy of the replaced index buffer generation was not freed (%llu)\n", static_cast<unsigned long long>(store.stats().freed));
                ++g_failures;
            }
        }
        for (int i = 0; i < 4; ++i) frame("clusters_collect");  // (the read-backs complete)
        const gpuscene::ClusterStore::Stats& st = store.stats();
        std::printf("  clusters: %llu ranges requested, %llu uploaded (%llu clusters, %llu triangles, %.1f KiB), %llu draws on a copy, multiset checks %llu bad %llu, read-backs %llu DIFFERING %llu\n",
                    static_cast<unsigned long long>(st.requested), static_cast<unsigned long long>(st.uploaded), static_cast<unsigned long long>(st.clusters), static_cast<unsigned long long>(st.triangles),
                    static_cast<double>(st.arenaBytes) / 1024.0, static_cast<unsigned long long>(st.perDrawUsed), static_cast<unsigned long long>(st.multisetChecked),
                    static_cast<unsigned long long>(st.multisetBad), static_cast<unsigned long long>(st.readbackChecked), static_cast<unsigned long long>(st.readbackBad));
        const bool vulkan = dev.game().caps().backend == rhi::Backend::Vulkan;
        if (st.multisetBad != 0 || st.readbackBad != 0 || st.multisetChecked < 2 || (vulkan && st.readbackChecked < 1) || st.clusters < 2 * (tris / 128) || st.failedBuild != 0 || st.failedData != 0) {
            std::printf("  FAIL clusters: checks (multiset bad %llu, read-back bad %llu / checked %llu, clusters %llu, build failures %llu, data failures %llu)\n",
                        static_cast<unsigned long long>(st.multisetBad), static_cast<unsigned long long>(st.readbackBad), static_cast<unsigned long long>(st.readbackChecked),
                        static_cast<unsigned long long>(st.clusters), static_cast<unsigned long long>(st.failedBuild), static_cast<unsigned long long>(st.failedData));
            ++g_failures;
        }
    }
    // off: nothing left, the draws are the engine's again
    dev.setClusters(false);
    if (store.arenaBytes() != 0 || store.rowsUsed() != 0) {
        std::printf("  FAIL clusters: switched off, but %llu arena bytes / %u rows are left\n", static_cast<unsigned long long>(store.arenaBytes()), store.rowsUsed());
        ++g_failures;
    }
    differ(refB, frame("clusters_off"), "after switching off");
    store.destroy(&dev);
    dev.release(std::move(vb));
    dev.release(std::move(ib));
}

// F4-3 owned SpeedTrees through the device's own path (frame-ownership.md 7.25): a tree-like hardware-instanced draw (one quad, six instances, stream 1 = the identity
// ramp, a vertex shader that reads its row through c32[a0.x] with a0 = 4 x the ramp value, c12 from the view block) is drawn (a) the normal way with the rows in the
// register file and (b) as ONE retained template (the scene ABI v2 form of the draw, its DrawBlock a persistent slot) with a ring block of rows (window 2 = c31 header +
// c32 + 4 p) and Device::ownedTreeFlush: one run of six instances, three runs of two (one indirect call, three argument records, the same picture), the rows in the
// reverse order (the picture follows the rows: they are really read), and the guards (a null window base, an instance count that is not the rows', a draw without a
// v2 pipeline): nothing drawn, drawn again right after. 0 bytes may differ from the normal draws.
// (F4-4: once for the main view's solid pass in slot 3 and once for a cascade's shadow depth pass in view slot 9 = a tree cascade slot)
void testOwnedTreesIn(Fixture& f, scene::View phaseView, scene::Pass phasePass, std::uint32_t viewSlot, const char* label) {
    Device& dev = f.dev;
    std::printf("  owned trees in %s (view slot %u)\n", label, viewSlot);
    dev.setSceneShadows(phaseView == scene::View::Shadow);
    dev.setSceneDraws(true, 0);
    if (!dev.sceneDrawsActive()) {
        std::printf("  SKIP owned trees: scene draws are not available on this device\n");
        return;
    }
    static const char* kVsTree = R"(
vs_3_0
dcl_position v0
dcl_color v1
dcl_texcoord4 v3
dcl_position o0
dcl_color o1
add r0.x, v3.x, v3.x
add r0.x, r0.x, r0.x
mova a0.x, r0.x
mov r1, c32[a0.x]
mov r2, c33[a0.x]
add o0, v0, r1
mul r3, v1, r2
add o1, r3, c12
)";
    static const char* kPsTree = R"(
ps_3_0
dcl_color v0
add oC0, v0, c14
)";
    const VertexElement kTreeDecl[] = {
        {0, 0, DECLTYPE_FLOAT4, 0, 0 /*POSITION*/, 0},
        {0, 16, DECLTYPE_D3DCOLOR, 0, 10 /*COLOR*/, 0},
        {0, 20, DECLTYPE_FLOAT2, 0, 5 /*TEXCOORD*/, 0},
        {1, 0, DECLTYPE_FLOAT1, 0, 5 /*TEXCOORD*/, 4},
        {kDeclEndStream, 0, DECLTYPE_UNUSED, 0, 0, 0},
    };
    const auto vsT = shaderFrom(dev, kVsTree);
    const auto psT = shaderFrom(dev, kPsTree);
    auto decl = dev.createVertexDecl(kTreeDecl);
    if (!vsT || !psT || !decl) {
        std::printf("  FAIL owned trees: shaders / declaration\n");
        ++g_failures;
        return;
    }
    constexpr std::uint32_t kInst = 6, kSlots = 16, kRamp = 64;
    std::vector<Vertex> verts = {{0.0f, 0.5f, 0.5f, 1, 0xFFFFFFFFu, 0, 0}, {0.25f, 0.5f, 0.5f, 1, 0xFFFFFFFFu, 1, 0}, {0.0f, -0.5f, 0.5f, 1, 0xFFFFFFFFu, 0, 1}, {0.25f, -0.5f, 0.5f, 1, 0xFFFFFFFFu, 1, 1}};
    const std::uint16_t idx[6] = {0, 1, 2, 1, 3, 2};
    auto vb = dev.createBuffer(static_cast<std::uint32_t>(verts.size() * sizeof(Vertex)), USAGE_WRITEONLY, POOL_MANAGED, false, false);
    auto ib = dev.createBuffer(sizeof(idx), USAGE_WRITEONLY, POOL_MANAGED, true, false);
    auto ramp = dev.createBuffer(kRamp * 4, USAGE_WRITEONLY, POOL_MANAGED, false, false);
    std::memcpy(vb->lock(0, 0, 0), verts.data(), verts.size() * sizeof(Vertex));
    vb->unlock();
    std::memcpy(ib->lock(0, 0, 0), idx, sizeof(idx));
    ib->unlock();
    {
        float r[kRamp];
        for (std::uint32_t i = 0; i < kRamp; ++i) r[i] = rvk::native::tro::rampValue(i);  // (the game's ramp: the LOCAL instance index, identity for the first 50)
        std::memcpy(ramp->lock(0, 0, 0), r, sizeof(r));
        ramp->unlock();
    }
    rhi::Device* rd = dev.rhi();
    auto tableBuf = rd->createBuffer({static_cast<std::uint64_t>(kSlots) * shader::abi::kViewBlock2Size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "owned trees view table"});
    if (!tableBuf) {
        std::printf("  FAIL owned trees: view table buffer\n");
        ++g_failures;
        return;
    }
    const auto tw = tableBuf->shaderRef(0);
    const rhi::GpuRef tableRef = rhi::GpuRef::fromWords(tw[0], tw[1]);
    std::vector<shader::abi::ViewBlock2> twin(kSlots);
    std::memset(twin.data(), 0, twin.size() * sizeof(shader::abi::ViewBlock2));
    const float c12[4] = {0.02f, 0.10f, 0.05f, 0.0f};
    {
        shader::abi::ViewBlock2& b = twin[viewSlot];
        std::memcpy(b.vs1[0], c12, 16);
        dev.viewPosFix(b.posFix);
        std::memcpy(tableBuf->map(), twin.data(), twin.size() * sizeof(shader::abi::ViewBlock2));
    }
    OwnedWindows vw;
    {
        const auto set = [&](OwnedWindows::Win& w, bool vertex, std::uint32_t win, std::uint32_t covered) {
            w.kind = OwnedWindows::Kind::View;
            w.base = rhi::GpuRef{gpuscene::ViewTable::windowBase(tableRef, viewSlot, vertex, win)};
            w.covered = covered;
        };
        set(vw.w[0][0], true, 0, 12);
        set(vw.w[0][1], true, 1, 19);
        set(vw.w[1][0], false, 0, 5);
        vw.viewSlot = viewSlot;
    }
    // the rows: c32 = (dx, 0, 0, 0), c33 = the tint
    float rows[kInst][8];
    for (std::uint32_t i = 0; i < kInst; ++i) {
        const float r[8] = {-1.0f + 0.3f * static_cast<float>(i), 0.0f, 0.0f, 0.0f, 0.2f + 0.14f * static_cast<float>(i), 0.9f - 0.12f * static_cast<float>(i), 0.3f, 1.0f};
        std::memcpy(rows[i], r, sizeof(r));
    }
    const auto begin = [&] {
        f.reset();
        dev.drawTag() = {};
        dev.drawTag().source = draws::Source::SpeedTree;
        dev.phases().push(phaseView, phasePass);
        dev.setVertexDecl(decl.get());
        dev.setVertexShader(vsT);
        dev.setPixelShader(psT);
        dev.setStreamSource(0, vb.get(), 0, sizeof(Vertex));
        dev.setStreamSource(1, ramp.get(), 0, 4);
        {
            const float zero[4] = {};  // (PS c14 = the lighting entry 0: a second run of this test must not see the last run's value)
            dev.setPixelShaderConstantF(14, zero, 1);
        }
        dev.setStreamSourceFreq(0, STREAMSOURCE_INDEXEDDATA | kInst);
        dev.setStreamSourceFreq(1, STREAMSOURCE_INSTANCEDATA | 1);
        dev.setIndices(ib.get());
    };
    gpuscene::DrawRecordTable& tab = gpuscene::drawrecords();
    tab.setActive(true);
    if (!tab.ensure(dev)) {
        std::printf("  FAIL owned trees: the draw record tables\n");
        ++g_failures;
        return;
    }
    const auto end = [&](const char* name) {
        tab.flush(dev);
        dev.setSceneIssue(false);
        dev.phases().pop();
        dev.setStreamSourceFreq(0, 1);
        dev.setStreamSourceFreq(1, 1);
        return capture(dev, name);
    };
    const auto bad = [&](const Pixels& ref, const Pixels& p, const char* what) {
        std::uint32_t n = 0;
        for (std::size_t i = 0; i < ref.bgra.size() && i < p.bgra.size(); ++i) n += ref.bgra[i] != p.bgra[i];
        if (n != 0 || ref.bgra.size() != p.bgra.size()) {
            std::printf("  FAIL owned trees (%s): %u bytes differ from the normal draws\n", what, n);
            ++g_failures;
        }
        return n;
    };
    // reference: the normal instanced draw, rows in the register file
    const auto setRegs = [&](bool reverse) {
        dev.setVertexShaderConstantF(12, c12, 1);
        for (std::uint32_t i = 0; i < kInst; ++i) dev.setVertexShaderConstantF(32 + 4 * i, rows[reverse ? kInst - 1 - i : i], 2);
    };
    // (a cascade's reference = six single-instance draws, each at its own ramp offset: the shadow phase's instanced draw is not the reference of the instanced form)
    const bool single = phaseView == scene::View::Shadow;
    const auto drawRef = [&](bool reverse) {
        begin();
        setRegs(reverse);
        if (!single) {
            dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, 4, 0, 2);
            return;
        }
        for (std::uint32_t i = 0; i < kInst; ++i) {
            dev.setStreamSource(1, ramp.get(), 4 * i, 4);
            dev.setStreamSourceFreq(0, STREAMSOURCE_INDEXEDDATA | 1);
            dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, 4, 0, 2);
        }
    };
    drawRef(false);
    const Pixels ref = end("ownedt_ref");
    // (the reversed rows: instance p draws row 5 - p; the picture differs from the reference)
    drawRef(true);
    const Pixels refRev = end("ownedt_refrev");
    dev.setSceneViewTable(tableRef, twin.data(), kSlots);
    dev.setSceneViews(true);
    dev.setSceneTrees(true);
    Device::CachedDraw cd;
    bool ready = false;
    for (int i = 0; i < 1200 && !ready; ++i) {
        begin();
        setRegs(false);
        dev.setSceneIssue(true);
        dev.beginCapture(&cd);
        dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, 4, 0, 2);
        dev.endCapture();
        end("ownedt_capture");
        ready = cd.valid && cd.scenePipeline2 != GamePipeline::None;
        if (!ready) Sleep(50);
    }
    if (!ready) {
        std::printf("  FAIL owned trees: the draw never got its v2 pipeline\n");
        ++g_failures;
        return;
    }
    if (cd.scenePipeline != GamePipeline::None) {
        std::printf("  FAIL owned trees: a tree draw got a scene ABI v1 pipeline (its composed packet would lose the instance count)\n");
        ++g_failures;
    }
    // the template: derived once, its DrawBlock a persistent slot
    // lighting images (PS window 3, c14..): entry e = 24 registers at kPadL + 512 e, c14 = kLight[e]; the base is biased by 14 x 16 as the lighting images' are
    constexpr std::uint64_t kPadL = 4096;
    const float kLight[3][4] = {{0.0f, 0.0f, 0.0f, 0.0f}, {0.25f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.15f, 0.35f, 0.0f}};
    auto lightBuf = rd->createBuffer({kPadL + 3 * 512, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "owned trees lighting"});
    if (!lightBuf) {
        std::printf("  FAIL owned trees: lighting buffer\n");
        ++g_failures;
        return;
    }
    {
        auto* lb = static_cast<std::uint8_t*>(lightBuf->map());
        std::memset(lb, 0, static_cast<std::size_t>(kPadL + 3 * 512));
        for (std::uint32_t e = 0; e < 3; ++e) std::memcpy(lb + kPadL + 512u * e, kLight[e], 16);
        lightBuf->flush();
    }
    const auto lightBase = [&](std::uint32_t e) {
        const auto w = lightBuf->shaderRef(kPadL + 512u * e);
        return rhi::GpuRef::fromWords(w[0], w[1]).plus(-14 * 16).bits;
    };
    const std::uint32_t slot = tab.alloc();
    OwnedWindows ow = vw;
    ow.w[1][3].kind = OwnedWindows::Kind::Lighting;
    ow.w[1][3].base = rhi::GpuRef{lightBase(0)};
    ow.w[1][3].covered = 24;
    ow.w[0][2].kind = OwnedWindows::Kind::Object;
    ow.w[0][2].base = tab.zeroRef().plus(-static_cast<std::int64_t>(shader::abi::kVsWindowFirst[2]) * 16);
    ow.w[0][2].covered = shader::abi::kVsWindowFirst[3] - shader::abi::kVsWindowFirst[2];
    {
        begin();
        dev.setSceneIssue(true);
        Device::DeriveIn in;
        in.streams[0] = vb.get();
        in.strides[0] = sizeof(Vertex);
        in.streams[1] = ramp.get();
        in.strides[1] = 4;
        in.indices = ib.get();
        in.alphaFunc = 8u;
        Device::OwnedPersist out;
        const int rc = dev.ownedDerive(cd, in, ow, tab.blockRef(slot), tab.zeroRef(), true, out);
        if (rc != 0) {
            std::printf("  FAIL owned trees: the template's derivation returned %d\n", rc);
            ++g_failures;
            end("ownedt_derive");
            return;
        }
        tab.write(slot, out.rec, out.blk);
        end("ownedt_derive");
    }
    // one frame: the rows in a ring block (c31 header, then 64 bytes per position), the runs of the items, optionally the rows reversed / a guard broken
    // (lightOf: the lighting entry of every position, empty = all entry 0. One DrawRecord per distinct entry, as the game's pass lays them out; recBreak 1 = a record index out of range, 2 = a null lighting base)
    const auto frame = [&](const char* name, const std::vector<std::uint32_t>& runs, bool reverse, std::uint32_t breakWhat, std::uint64_t& calls, bool& flushed, const std::vector<std::uint32_t>& lightOf = {},
                           std::uint32_t recBreak = 0) {
        begin();
        dev.setSceneIssue(true);
        rhi::RingAlloc ra = dev.allocOwnedRing((32u + 4u * kInst) * 16u, 16);
        auto* base = static_cast<std::uint8_t*>(ra.ptr);
        std::memset(base, 0, 32u * 16u);
        for (std::uint32_t p = 0; p < kInst; ++p) std::memcpy(base + (32u + 4u * p) * 16u, rows[reverse ? kInst - 1 - p : p], sizeof(rows[0]));
        std::vector<Device::TreeItem> items;
        for (const std::uint32_t n : runs) {
            Device::TreeItem it;
            it.cd = &cd;
            it.bucket = dev.ownedBucketKey(cd, viewSlot);
            it.instances = n;
            items.push_back(it);
        }
        std::vector<shader::abi::DrawRecord> recs;
        std::vector<std::uint32_t> posRec;
        std::uint32_t recOfEntry[3] = {~0u, ~0u, ~0u};
        for (std::uint32_t p = 0; p < kInst; ++p) {
            const std::uint32_t e = lightOf.empty() ? 0 : lightOf[p];
            if (recOfEntry[e] == ~0u) {
                recOfEntry[e] = static_cast<std::uint32_t>(recs.size());
                shader::abi::DrawRecord r = tab.record(slot);
                r.vsWin[2] = breakWhat == 1 ? 0 : ra.ref.bits;
                r.psWin[3] = recBreak == 2 ? 0 : lightBase(e);
                recs.push_back(r);
            }
            posRec.push_back(recOfEntry[e]);
        }
        if (recBreak == 1) posRec[3] = static_cast<std::uint32_t>(recs.size());
        std::uint32_t positions = kInst;
        if (breakWhat == 2) positions = kInst + 1;
        const auto s0 = dev.sceneStats();
        flushed = dev.ownedTreeFlush(items, recs, posRec, viewSlot, positions);
        calls = dev.sceneStats().indirectCalls - s0.indirectCalls;
        return end(name);
    };
    std::uint64_t calls = 0;
    bool flushed = false;
    bad(ref, frame("ownedt_one", {kInst}, false, 0, calls, flushed), "one run of six instances");
    if (!flushed || calls != 1) {
        std::printf("  FAIL owned trees: one run: flushed %d, %llu indirect calls (expected 1)\n", flushed, static_cast<unsigned long long>(calls));
        ++g_failures;
    }
    bad(ref, frame("ownedt_three", {2, 2, 2}, false, 0, calls, flushed), "three runs of two instances");
    if (!flushed || calls != 1) {
        std::printf("  FAIL owned trees: three runs of one bucket: flushed %d, %llu indirect calls (expected 1)\n", flushed, static_cast<unsigned long long>(calls));
        ++g_failures;
    }
    bad(refRev, frame("ownedt_rev", {kInst}, true, 0, calls, flushed), "the rows in the reverse order");
    {
        const Pixels p = frame("ownedt_nullbase", {kInst}, false, 1, calls, flushed);
        const Pixels q = frame("ownedt_count", {kInst}, false, 2, calls, flushed);
        const bool count = !flushed;
        if (p.bgra[(32 * p.w + 2) * 4 + 1] != 0 || q.bgra[(32 * q.w + 2) * 4 + 1] != 0 || !count) {
            std::printf("  FAIL owned trees: guards: a null window base / an instance count that is not the positions drew something or was accepted (count refused %d)\n", count);
            ++g_failures;
        }
    }
    bad(ref, frame("ownedt_after_guards", {kInst}, false, 0, calls, flushed), "the draw after the refusals");
    // per-instance lighting in ONE draw: the reference = one normal draw per instance (its own ramp offset = its row, its own c14); the owned draw = one item of six positions
    // with up to three records (entries 0 / 1 / 2) and the ref list giving each position its record: ONE indirect command, 0 bytes may differ
    for (const std::vector<std::uint32_t>& lightOf : {std::vector<std::uint32_t>{1, 2, 0, 1, 2, 0}, std::vector<std::uint32_t>{0, 0, 1, 1, 2, 2}, std::vector<std::uint32_t>{2, 2, 2, 2, 2, 2}}) {
        begin();
        for (std::uint32_t i = 0; i < kInst; ++i) {
            setRegs(false);
            dev.setPixelShaderConstantF(14, kLight[lightOf[i]], 1);
            dev.setStreamSource(1, ramp.get(), 4 * i, 4);
            dev.setStreamSourceFreq(0, STREAMSOURCE_INDEXEDDATA | 1);
            dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, 4, 0, 2);
        }
        const Pixels lref = end("ownedt_lref");
        bad(lref, frame("ownedt_light", {kInst}, false, 0, calls, flushed, lightOf), "per-instance lighting in one draw");
        if (!flushed || calls != 1) {
            std::printf("  FAIL owned trees: per-instance lighting: flushed %d, %llu indirect calls (expected 1)\n", flushed, static_cast<unsigned long long>(calls));
            ++g_failures;
        }
    }
    {
        // (the lighting really is read: entry 2 everywhere differs from entry 0 everywhere)
        const Pixels l0 = frame("ownedt_light0", {kInst}, false, 0, calls, flushed, {0, 0, 0, 0, 0, 0});
        const Pixels l2 = frame("ownedt_light2", {kInst}, false, 0, calls, flushed, {2, 2, 2, 2, 2, 2});
        std::uint32_t nd = 0;
        for (std::size_t i = 0; i < l0.bgra.size() && i < l2.bgra.size(); ++i) nd += l0.bgra[i] != l2.bgra[i];
        if (nd == 0) {
            std::printf("  FAIL owned trees: the lighting entries do not change the picture (not read)\n");
            ++g_failures;
        }
        // guards: a record index out of range / a null lighting base: refused, nothing drawn, the next frame is right again
        const std::uint64_t r0 = dev.treeFlushStats.refused;
        frame("ownedt_badrec", {kInst}, false, 0, calls, flushed, {1, 2, 0, 1, 2, 0}, 1);
        const bool f1 = flushed;
        frame("ownedt_badlight", {kInst}, false, 0, calls, flushed, {1, 2, 0, 1, 2, 0}, 2);
        if (f1 || flushed || dev.treeFlushStats.refused - r0 != 2) {
            std::printf("  FAIL owned trees: guards: a record index out of range / a null lighting base was accepted (%d / %d, refused %llu)\n", f1, flushed,
                        static_cast<unsigned long long>(dev.treeFlushStats.refused - r0));
            ++g_failures;
        }
        bad(ref, frame("ownedt_after_light_guards", {kInst}, false, 0, calls, flushed), "the draw after the lighting refusals");
    }
    {
        // MORE THAN 50 POSITIONS (frame-ownership.md 7.46): the translated vertex shader's relative constant read (`rvk_cRel`) returns zero from c256 on, so rows laid out as
        // c32 + 4 p for p >= 56 read zeros (50 .. 55: the material window's bytes) - in the game most trees of a pass vanished. The pass lays the rows out in chunks of 50 (own
        // c31 prefix + 50 rows), the records of a chunk's positions carry window 2's base shifted by that chunk, the ramp holds `position mod 50`: 60 positions = chunks of
        // 50 + 10, as one run of 60 and as runs of 30 + 30 (items cut at the chunk boundary). The reference = one normal draw per instance, 0 bytes may differ. (With the old
        // layout, rampValue(p) = p, this fails: positions 56 .. 59 read zeros, 50 .. 55 the material window.)
        constexpr std::uint32_t kBig = 60;
        float big[kBig][8];
        for (std::uint32_t p = 0; p < kBig; ++p) {
            const float r[8] = {-0.95f + 0.145f * static_cast<float>(p % 13), -0.9f + 0.4f * static_cast<float>(p / 13), 0.0f, 0.0f, 0.2f + 0.1f * static_cast<float>(p % 7),
                                0.9f - 0.2f * static_cast<float>(p % 5), 0.3f, 1.0f};
            std::memcpy(big[p], r, sizeof(r));
        }
        begin();
        for (std::uint32_t p = 0; p < kBig; ++p) {
            dev.setVertexShaderConstantF(12, c12, 1);
            dev.setVertexShaderConstantF(32, big[p], 2);
            dev.setStreamSource(1, ramp.get(), 0, 4);
            dev.setStreamSourceFreq(0, STREAMSOURCE_INDEXEDDATA | 1);
            dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, 4, 0, 2);
        }
        const Pixels bigRef = end("ownedt_bigref");
        const auto bigFrame = [&](const char* name, const std::vector<std::uint32_t>& runs, std::uint64_t& bigCalls, bool& bigFlushed) {
            using rvk::native::tro::chunkBase;
            using rvk::native::tro::chunkCount;
            using rvk::native::tro::chunkOf;
            using rvk::native::tro::kChunkBytes;
            using rvk::native::tro::kChunkPrefixRegs;
            using rvk::native::tro::rowOffset;
            begin();
            dev.setSceneIssue(true);
            rhi::RingAlloc ra = dev.allocOwnedRing(static_cast<std::uint64_t>(chunkCount(kBig)) * kChunkBytes, 16);
            auto* base = static_cast<std::uint8_t*>(ra.ptr);
            for (std::uint32_t k = 0; k < chunkCount(kBig); ++k) std::memset(base + chunkBase(k), 0, static_cast<std::size_t>(kChunkPrefixRegs) * 16);
            std::vector<Device::TreeItem> items;
            std::vector<shader::abi::DrawRecord> recs;
            std::vector<std::uint32_t> posRec;
            std::uint32_t pos = 0;
            for (const std::uint32_t n : runs) {
                Device::TreeItem it;
                it.cd = &cd;
                it.bucket = dev.ownedBucketKey(cd, viewSlot);
                std::uint32_t chunk = ~0u, rec = 0;
                for (std::uint32_t q = 0; q < n; ++q, ++pos) {
                    if (chunkOf(pos) != chunk) {
                        if (it.instances != 0) items.push_back(it);
                        it.instances = 0;
                        chunk = chunkOf(pos);
                        shader::abi::DrawRecord r = tab.record(slot);
                        r.vsWin[2] = ra.ref.plus(static_cast<std::int64_t>(chunkBase(chunk))).bits;
                        r.psWin[3] = lightBase(0);
                        rec = static_cast<std::uint32_t>(recs.size());
                        recs.push_back(r);
                    }
                    std::memcpy(base + rowOffset(pos), big[pos], sizeof(big[0]));
                    posRec.push_back(rec);
                    ++it.instances;
                }
                if (it.instances != 0) items.push_back(it);
            }
            const auto s0 = dev.sceneStats();
            bigFlushed = dev.ownedTreeFlush(items, recs, posRec, viewSlot, kBig);
            bigCalls = dev.sceneStats().indirectCalls - s0.indirectCalls;
            return end(name);
        };
        std::uint64_t bigCalls = 0;
        bool bigFlushed = false;
        bad(bigRef, bigFrame("ownedt_big", {30, 30}, bigCalls, bigFlushed), "60 positions in two chunks (runs of 30 / 30)");
        if (!bigFlushed || bigCalls != 1) {
            std::printf("  FAIL owned trees: 60 positions: flushed %d, %llu indirect calls (expected 1)\n", bigFlushed, static_cast<unsigned long long>(bigCalls));
            ++g_failures;
        }
        bad(bigRef, bigFrame("ownedt_big_one", {kBig}, bigCalls, bigFlushed), "60 positions in one run");
    }
    std::printf("  owned trees: %llu flushes, %llu items, %llu instances, %llu indirect packets, %llu refused\n", static_cast<unsigned long long>(dev.treeFlushStats.flushes),
                static_cast<unsigned long long>(dev.treeFlushStats.items), static_cast<unsigned long long>(dev.treeFlushStats.instances), static_cast<unsigned long long>(dev.treeFlushStats.packets),
                static_cast<unsigned long long>(dev.treeFlushStats.refused));
    tab.release(slot);
    tab.setActive(false);
    tab.destroy();
    dev.setSceneTrees(false);
    dev.setSceneShadows(false);
    dev.setSceneViews(false);
    dev.setSceneDraws(false, 0);
    dev.release(std::move(vb));
    dev.release(std::move(ib));
    dev.release(std::move(ramp));
}

// F7-2 owned vegetation flush through the device's own path (frame-ownership.md 7.42): a grass-like hardware-instanced draw (one quad, six instances, stream 1 = per-instance
// vertex data {offset float4, tint float4}, c12 from the view block, c14 = the lighting entry) is drawn (a) the normal way with the instance data in a vertex buffer and (b) as
// ONE retained template whose stream 1 is a stand-in buffer (the scene ABI v2 form, its DrawBlock a persistent slot) with Device::ownedTreeFlush and the instance block
// (Device::VegInstances: the bytes in list order, copied into a ring block that replaces the stand-in): one item of six positions, three items of two (one indirect call), the
// instances in the reverse order (the picture follows the bytes: they are really read), a per-position record (two records, two lighting entries); guards: no instance data, a
// template without a stream-1 binding, an instance count that is not the positions, a null window base: nothing drawn, drawn again right after. 0 bytes may differ.
void testOwnedVegetation(Fixture& f) {
    Device& dev = f.dev;
    dev.setSceneShadows(false);
    dev.setSceneDraws(true, 0);
    if (!dev.sceneDrawsActive()) {
        std::printf("  SKIP owned vegetation: scene draws are not available on this device\n");
        return;
    }
    static const char* kVsVeg = R"(
vs_3_0
dcl_position v0
dcl_color v1
dcl_texcoord4 v3
dcl_texcoord5 v4
dcl_position o0
dcl_color o1
add o0, v0, v3
mul r0, v1, v4
add o1, r0, c12
)";
    static const char* kPsVeg = R"(
ps_3_0
dcl_color v0
add oC0, v0, c14
)";
    const VertexElement kDecl[] = {
        {0, 0, DECLTYPE_FLOAT4, 0, 0 /*POSITION*/, 0},
        {0, 16, DECLTYPE_D3DCOLOR, 0, 10 /*COLOR*/, 0},
        {0, 20, DECLTYPE_FLOAT2, 0, 5 /*TEXCOORD*/, 0},
        {1, 0, DECLTYPE_FLOAT4, 0, 5 /*TEXCOORD*/, 4},
        {1, 16, DECLTYPE_FLOAT4, 0, 5 /*TEXCOORD*/, 5},
        {kDeclEndStream, 0, DECLTYPE_UNUSED, 0, 0, 0},
    };
    const auto vsT = shaderFrom(dev, kVsVeg);
    const auto psT = shaderFrom(dev, kPsVeg);
    auto decl = dev.createVertexDecl(kDecl);
    if (!vsT || !psT || !decl) {
        std::printf("  FAIL owned vegetation: shaders / declaration\n");
        ++g_failures;
        return;
    }
    constexpr std::uint32_t kInst = 6, kSlots = 16, kStride = 32, kViewSlot = 13;
    std::vector<Vertex> verts = {{0.0f, 0.5f, 0.5f, 1, 0xFFFFFFFFu, 0, 0}, {0.25f, 0.5f, 0.5f, 1, 0xFFFFFFFFu, 1, 0}, {0.0f, -0.5f, 0.5f, 1, 0xFFFFFFFFu, 0, 1}, {0.25f, -0.5f, 0.5f, 1, 0xFFFFFFFFu, 1, 1}};
    const std::uint16_t idx[6] = {0, 1, 2, 1, 3, 2};
    auto vb = dev.createBuffer(static_cast<std::uint32_t>(verts.size() * sizeof(Vertex)), USAGE_WRITEONLY, POOL_MANAGED, false, false);
    auto ib = dev.createBuffer(sizeof(idx), USAGE_WRITEONLY, POOL_MANAGED, true, false);
    auto stand = dev.createBuffer(kStride * 16, USAGE_WRITEONLY, POOL_MANAGED, false, false);
    auto instBuf = dev.createBuffer(kStride * kInst, USAGE_WRITEONLY, POOL_MANAGED, false, false);
    auto instRev = dev.createBuffer(kStride * kInst, USAGE_WRITEONLY, POOL_MANAGED, false, false);
    std::memcpy(vb->lock(0, 0, 0), verts.data(), verts.size() * sizeof(Vertex));
    vb->unlock();
    std::memcpy(ib->lock(0, 0, 0), idx, sizeof(idx));
    ib->unlock();
    {
        std::vector<std::uint8_t> z(kStride * 16, 0);
        std::memcpy(stand->lock(0, 0, 0), z.data(), z.size());
        stand->unlock();
    }
    // the instances: offset (dx, 0, 0, 0), tint
    float inst[kInst][8];
    for (std::uint32_t i = 0; i < kInst; ++i) {
        const float r[8] = {-1.0f + 0.3f * static_cast<float>(i), 0.0f, 0.0f, 0.0f, 0.2f + 0.14f * static_cast<float>(i), 0.9f - 0.12f * static_cast<float>(i), 0.3f, 1.0f};
        std::memcpy(inst[i], r, sizeof(r));
    }
    float instR[kInst][8];
    for (std::uint32_t i = 0; i < kInst; ++i) std::memcpy(instR[i], inst[kInst - 1 - i], sizeof(instR[i]));
    std::memcpy(instBuf->lock(0, 0, 0), inst, sizeof(inst));
    instBuf->unlock();
    std::memcpy(instRev->lock(0, 0, 0), instR, sizeof(instR));
    instRev->unlock();
    rhi::Device* rd = dev.rhi();
    auto tableBuf = rd->createBuffer({static_cast<std::uint64_t>(kSlots) * shader::abi::kViewBlock2Size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "owned vegetation view table"});
    if (!tableBuf) {
        std::printf("  FAIL owned vegetation: view table buffer\n");
        ++g_failures;
        return;
    }
    const auto tw = tableBuf->shaderRef(0);
    const rhi::GpuRef tableRef = rhi::GpuRef::fromWords(tw[0], tw[1]);
    std::vector<shader::abi::ViewBlock2> twin(kSlots);
    std::memset(twin.data(), 0, twin.size() * sizeof(shader::abi::ViewBlock2));
    const float c12[4] = {0.02f, 0.10f, 0.05f, 0.0f};
    {
        shader::abi::ViewBlock2& b = twin[kViewSlot];
        std::memcpy(b.vs1[0], c12, 16);
        dev.viewPosFix(b.posFix);
        std::memcpy(tableBuf->map(), twin.data(), twin.size() * sizeof(shader::abi::ViewBlock2));
    }
    OwnedWindows vw;
    {
        const auto set = [&](OwnedWindows::Win& w, bool vertex, std::uint32_t win, std::uint32_t covered) {
            w.kind = OwnedWindows::Kind::View;
            w.base = rhi::GpuRef{gpuscene::ViewTable::windowBase(tableRef, kViewSlot, vertex, win)};
            w.covered = covered;
        };
        set(vw.w[0][0], true, 0, 12);
        set(vw.w[0][1], true, 1, 19);
        set(vw.w[1][0], false, 0, 5);
        vw.viewSlot = kViewSlot;
    }
    const auto begin = [&](Buffer* s1) {
        f.reset();
        dev.drawTag() = {};
        dev.drawTag().source = draws::Source::Vegetation;
        dev.phases().push(scene::View::Main, scene::Pass::Solid);
        dev.setVertexDecl(decl.get());
        dev.setVertexShader(vsT);
        dev.setPixelShader(psT);
        dev.setStreamSource(0, vb.get(), 0, sizeof(Vertex));
        dev.setStreamSource(1, s1, 0, kStride);
        {
            const float zero[4] = {};
            dev.setPixelShaderConstantF(14, zero, 1);
        }
        dev.setStreamSourceFreq(0, STREAMSOURCE_INDEXEDDATA | kInst);
        dev.setStreamSourceFreq(1, STREAMSOURCE_INSTANCEDATA | 1);
        dev.setIndices(ib.get());
    };
    gpuscene::DrawRecordTable& tab = gpuscene::drawrecords();
    tab.setActive(true);
    if (!tab.ensure(dev)) {
        std::printf("  FAIL owned vegetation: the draw record tables\n");
        ++g_failures;
        return;
    }
    const auto end = [&](const char* name) {
        tab.flush(dev);
        dev.setSceneIssue(false);
        dev.phases().pop();
        dev.setStreamSourceFreq(0, 1);
        dev.setStreamSourceFreq(1, 1);
        return capture(dev, name);
    };
    const auto bad = [&](const Pixels& ref, const Pixels& p, const char* what) {
        std::uint32_t n = 0;
        for (std::size_t i = 0; i < ref.bgra.size() && i < p.bgra.size(); ++i) n += ref.bgra[i] != p.bgra[i];
        if (n != 0 || ref.bgra.size() != p.bgra.size()) {
            std::printf("  FAIL owned vegetation (%s): %u bytes differ from the normal draws\n", what, n);
            ++g_failures;
        }
        return n;
    };
    const auto drawRef = [&](Buffer* s1, const char* name) {
        begin(s1);
        dev.setVertexShaderConstantF(12, c12, 1);
        dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, 4, 0, 2);
        return end(name);
    };
    const Pixels ref = drawRef(instBuf.get(), "ownedv_ref");
    const Pixels refRev = drawRef(instRev.get(), "ownedv_refrev");
    dev.setSceneViewTable(tableRef, twin.data(), kSlots);
    dev.setSceneViews(true);
    dev.setSceneVeg(true);
    Device::CachedDraw cd;
    bool ready = false;
    for (int i = 0; i < 1200 && !ready; ++i) {
        begin(stand.get());
        dev.setVertexShaderConstantF(12, c12, 1);
        dev.setSceneIssue(true);
        dev.beginCapture(&cd);
        dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, 4, 0, 2);
        dev.endCapture();
        end("ownedv_capture");
        ready = cd.valid && cd.scenePipeline2 != GamePipeline::None;
        if (!ready) Sleep(50);
    }
    if (!ready) {
        std::printf("  FAIL owned vegetation: the draw never got its v2 pipeline\n");
        ++g_failures;
        return;
    }
    if (cd.scenePipeline != GamePipeline::None) {
        std::printf("  FAIL owned vegetation: a grass draw got a scene ABI v1 pipeline (its composed packet would lose the instance count)\n");
        ++g_failures;
    }
    constexpr std::uint64_t kPadL = 4096;
    const float kLight[2][4] = {{0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.15f, 0.35f, 0.0f}};
    auto lightBuf = rd->createBuffer({kPadL + 2 * 512, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "owned vegetation lighting"});
    if (!lightBuf) {
        std::printf("  FAIL owned vegetation: lighting buffer\n");
        ++g_failures;
        return;
    }
    {
        auto* lb = static_cast<std::uint8_t*>(lightBuf->map());
        std::memset(lb, 0, static_cast<std::size_t>(kPadL + 2 * 512));
        for (std::uint32_t e = 0; e < 2; ++e) std::memcpy(lb + kPadL + 512u * e, kLight[e], 16);
        lightBuf->flush();
    }
    const auto lightBase = [&](std::uint32_t e) {
        const auto w = lightBuf->shaderRef(kPadL + 512u * e);
        return rhi::GpuRef::fromWords(w[0], w[1]).plus(-14 * 16).bits;
    };
    const std::uint32_t slot = tab.alloc();
    OwnedWindows ow = vw;
    ow.w[1][3].kind = OwnedWindows::Kind::Lighting;
    ow.w[1][3].base = rhi::GpuRef{lightBase(0)};
    ow.w[1][3].covered = 24;
    ow.w[0][2].kind = OwnedWindows::Kind::Object;
    ow.w[0][2].base = tab.zeroRef().plus(-static_cast<std::int64_t>(shader::abi::kVsWindowFirst[2]) * 16);
    ow.w[0][2].covered = shader::abi::kVsWindowFirst[3] - shader::abi::kVsWindowFirst[2];
    {
        begin(stand.get());
        dev.setSceneIssue(true);
        Device::DeriveIn in;
        in.streams[0] = vb.get();
        in.strides[0] = sizeof(Vertex);
        in.streams[1] = stand.get();
        in.strides[1] = kStride;
        in.indices = ib.get();
        in.alphaFunc = 8u;
        Device::OwnedPersist out;
        const int rc = dev.ownedDerive(cd, in, ow, tab.blockRef(slot), tab.zeroRef(), true, out);
        if (rc != 0) {
            std::printf("  FAIL owned vegetation: the template's derivation returned %d\n", rc);
            ++g_failures;
            end("ownedv_derive");
            return;
        }
        tab.write(slot, out.rec, out.blk);
        end("ownedv_derive");
    }
    // one frame: the runs of the items, the instance bytes in list order (optionally reversed), per position a record (lightOf: entry per position, empty = entry 0)
    const auto frame = [&](const char* name, const std::vector<std::uint32_t>& runs, bool reverse, std::uint32_t breakWhat, std::uint64_t& calls, bool& flushed, const std::vector<std::uint32_t>& lightOf = {}) {
        begin(stand.get());
        dev.setSceneIssue(true);
        std::vector<Device::TreeItem> items;
        for (const std::uint32_t n : runs) {
            Device::TreeItem it;
            it.cd = &cd;
            it.bucket = dev.ownedBucketKey(cd, kViewSlot);
            it.instances = n;
            items.push_back(it);
        }
        std::vector<shader::abi::DrawRecord> recs;
        std::vector<std::uint32_t> posRec;
        std::uint32_t recOfEntry[2] = {~0u, ~0u};
        for (std::uint32_t p = 0; p < kInst; ++p) {
            const std::uint32_t e = lightOf.empty() ? 0 : lightOf[p];
            if (recOfEntry[e] == ~0u) {
                recOfEntry[e] = static_cast<std::uint32_t>(recs.size());
                shader::abi::DrawRecord r = tab.record(slot);
                r.vsWin[2] = breakWhat == 1 ? 0 : tab.zeroRef().plus(-static_cast<std::int64_t>(shader::abi::kVsWindowFirst[2]) * 16).bits;
                r.psWin[3] = lightBase(e);
                recs.push_back(r);
            }
            posRec.push_back(recOfEntry[e]);
        }
        std::uint32_t positions = kInst;
        if (breakWhat == 2) positions = kInst + 1;
        const Device::VegInstances vi{reinterpret_cast<const std::uint8_t*>(reverse ? instR : inst), kStride};
        const Device::VegInstances none{};
        const auto s0 = dev.sceneStats();
        flushed = dev.ownedTreeFlush(items, recs, posRec, kViewSlot, positions, breakWhat == 3 ? &none : &vi);
        calls = dev.sceneStats().indirectCalls - s0.indirectCalls;
        return end(name);
    };
    std::uint64_t calls = 0;
    bool flushed = false;
    bad(ref, frame("ownedv_one", {kInst}, false, 0, calls, flushed), "one run of six instances");
    if (!flushed || calls != 1) {
        std::printf("  FAIL owned vegetation: one run: flushed %d, %llu indirect calls (expected 1)\n", flushed, static_cast<unsigned long long>(calls));
        ++g_failures;
    }
    bad(ref, frame("ownedv_three", {2, 2, 2}, false, 0, calls, flushed), "three runs of two instances");
    if (!flushed || calls != 1) {
        std::printf("  FAIL owned vegetation: three runs of one bucket: flushed %d, %llu indirect calls (expected 1)\n", flushed, static_cast<unsigned long long>(calls));
        ++g_failures;
    }
    bad(refRev, frame("ownedv_rev", {kInst}, true, 0, calls, flushed), "the instance bytes in the reverse order");
    {
        const Pixels p = frame("ownedv_nullbase", {kInst}, false, 1, calls, flushed);
        const Pixels q = frame("ownedv_count", {kInst}, false, 2, calls, flushed);
        const bool count = !flushed;
        const Pixels r = frame("ownedv_nodata", {kInst}, false, 3, calls, flushed);
        const bool nodata = !flushed;
        if (p.bgra[(32 * p.w + 2) * 4 + 1] != 0 || q.bgra[(32 * q.w + 2) * 4 + 1] != 0 || r.bgra[(32 * r.w + 2) * 4 + 1] != 0 || !count || !nodata) {
            std::printf("  FAIL owned vegetation: guards: a null window base / an instance count that is not the positions / no instance data drew something or was accepted (count refused %d, no data refused %d)\n", count, nodata);
            ++g_failures;
        }
    }
    bad(ref, frame("ownedv_after_guards", {kInst}, false, 0, calls, flushed), "the draw after the refusals");
    // per-position lighting records in one draw: the reference = one normal draw per instance (its own stream-1 offset, its own c14)
    {
        const std::vector<std::uint32_t> lightOf = {1, 0, 1, 1, 0, 0};
        begin(instBuf.get());
        for (std::uint32_t i = 0; i < kInst; ++i) {
            dev.setVertexShaderConstantF(12, c12, 1);
            dev.setPixelShaderConstantF(14, kLight[lightOf[i]], 1);
            dev.setStreamSource(1, instBuf.get(), kStride * i, kStride);
            dev.setStreamSourceFreq(0, STREAMSOURCE_INDEXEDDATA | 1);
            dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, 4, 0, 2);
        }
        const Pixels lref = end("ownedv_lref");
        bad(lref, frame("ownedv_light", {kInst}, false, 0, calls, flushed, lightOf), "per-position lighting records in one draw");
        if (!flushed || calls != 1) {
            std::printf("  FAIL owned vegetation: per-position lighting: flushed %d, %llu indirect calls (expected 1)\n", flushed, static_cast<unsigned long long>(calls));
            ++g_failures;
        }
    }
    // a template whose packet has no stream-1 binding: refused
    {
        Device::CachedDraw nocd = cd;
        nocd.streamMask &= ~2u;
        std::vector<Device::TreeItem> items(1);
        items[0].cd = &nocd;
        items[0].bucket = dev.ownedBucketKey(nocd, kViewSlot);
        items[0].instances = kInst;
        std::vector<shader::abi::DrawRecord> recs{tab.record(slot)};
        std::vector<std::uint32_t> posRec(kInst, 0u);
        begin(stand.get());
        dev.setSceneIssue(true);
        const std::uint64_t r0 = dev.treeFlushStats.refused;
        const Device::VegInstances vi{reinterpret_cast<const std::uint8_t*>(inst), kStride};
        const bool ok = dev.ownedTreeFlush(items, recs, posRec, kViewSlot, kInst, &vi);
        end("ownedv_nostream");
        if (ok || dev.treeFlushStats.refused - r0 != 1) {
            std::printf("  FAIL owned vegetation: a template without a stream-1 binding was accepted (%d, refused %llu)\n", ok, static_cast<unsigned long long>(dev.treeFlushStats.refused - r0));
            ++g_failures;
        }
    }
    bad(ref, frame("ownedv_final", {kInst}, false, 0, calls, flushed), "the draw after all refusals");
    std::printf("  owned vegetation: %llu flushes, %llu items, %llu instances, %llu indirect packets, %llu refused\n", static_cast<unsigned long long>(dev.treeFlushStats.flushes),
                static_cast<unsigned long long>(dev.treeFlushStats.items), static_cast<unsigned long long>(dev.treeFlushStats.instances), static_cast<unsigned long long>(dev.treeFlushStats.packets),
                static_cast<unsigned long long>(dev.treeFlushStats.refused));
    tab.release(slot);
    tab.setActive(false);
    tab.destroy();
    dev.setSceneVeg(false);
    dev.setSceneViews(false);
    dev.setSceneDraws(false, 0);
    dev.release(std::move(vb));
    dev.release(std::move(ib));
    dev.release(std::move(stand));
    dev.release(std::move(instBuf));
    dev.release(std::move(instRev));
}

void testOwnedTrees(Fixture& f) {
    testOwnedTreesIn(f, scene::View::Main, scene::Pass::Solid, 3, "the main view");
    testOwnedTreesIn(f, scene::View::Shadow, scene::Pass::ShadowDepth, 9, "a cascade");
}

// F2c-1 owned windows through the device's own path (frame-ownership.md 7.13): a translated vertex shader that reads VS window 1 (c12) and window 2 (c32.w, c33)
// is drawn (a) with the registers set the normal way and (b) with the register file holding junk and the windows served from persistent images
// (a view block's c12..c30 + an object record's c31..c35 in one host-visible buffer behind a 4 KiB pad, window bases = image - first x 16, as the tables
// build them): the pictures must agree, the stats must count one VS1 + one VS2 window, an image shorter than the shader's read extent must fall back to the
// registers, and a changed image must change the picture (the draw really reads it)
void testOwnedWindows(Fixture& f) {
    Device& dev = f.dev;
    static const char* kVsOwned = R"(
vs_3_0
dcl_position v0
dcl_color v1
dcl_texcoord v2
dcl_position o0
dcl_color o1
dcl_texcoord o2
mov o0, v0
add r0, c12, c33
mul o1, r0, c32.w
mov o2, v2
)";
    const auto vsOwned = shaderFrom(dev, kVsOwned);
    if (!vsOwned) {
        std::printf("  FAIL owned windows: shader\n");
        ++g_failures;
        return;
    }
    rhi::Device* rd = dev.rhi();
    constexpr std::uint64_t kPad = 4096, kObjectOffset = kPad + 1024;
    auto buf = rd->createBuffer({kObjectOffset + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "owned windows test"});
    if (!buf) {
        std::printf("  FAIL owned windows: buffer\n");
        ++g_failures;
        return;
    }
    auto* bytes = static_cast<std::uint8_t*>(buf->map());
    std::memset(bytes, 0, static_cast<std::size_t>(kObjectOffset + 256));
    const float c12[4] = {0.2f, 0.1f, 0.3f, 0.5f};
    std::memcpy(bytes + kPad, c12, sizeof(c12));  // the view block's vs1 register 0 = c12
    struct Record {
        float regs[5][4];  // c31 .. c35 (gpuscene::ObjectRecord)
        std::uint32_t info[4];
    } rec{};
    rec.regs[1][3] = 0.5f;  // c32.w
    const float c33[4] = {0.3f, 0.4f, 0.1f, 0.5f};
    std::memcpy(rec.regs[2], c33, sizeof(c33));
    rec.info[3] = 1;
    std::memcpy(bytes + kObjectOffset, &rec, sizeof(rec));
    buf->flush();
    const auto imageRef = [&](std::uint64_t offset) {
        const auto w = buf->shaderRef(offset);
        return rhi::GpuRef::fromWords(w[0], w[1]);
    };
    OwnedWindows ow;
    ow.w[0][1].kind = OwnedWindows::Kind::View;
    ow.w[0][1].base = imageRef(kPad).plus(-12 * 16);
    ow.w[0][1].covered = 19;
    ow.w[0][2].kind = OwnedWindows::Kind::Object;
    ow.w[0][2].base = imageRef(kObjectOffset).plus(-31 * 16);
    ow.w[0][2].covered = 5;
    const auto render = [&](bool owned, bool junk, const char* name) {
        f.reset();
        dev.setVertexShader(vsOwned);
        const float junk4[4] = {0.9f, 0.9f, 0.9f, 0.9f};
        {
            // the register file: the true values (reference) or junk (the served windows must not read it)
            const float* a = junk ? junk4 : c12;
            dev.setVertexShaderConstantF(12, a, 1);
            const float c32[4] = {junk ? 0.9f : 0.0f, 0.0f, 0.0f, junk ? 0.9f : 0.5f};
            dev.setVertexShaderConstantF(32, c32, 1);
            dev.setVertexShaderConstantF(33, junk ? junk4 : c33, 1);
        }
        if (owned) dev.setOwnedWindows(&ow);
        f.draw(quad(-1, -1, 1, 1, 0.5f, 0xFFFFFFFF));
        dev.setOwnedWindows(nullptr);
        return capture(dev, name);
    };
    const Pixels ref = render(false, false, "owned_ref");
    expect("owned windows reference", ref, 32, 32, 0xFF000000u | (64u << 16) | (64u << 8) | 51u, 3);  // (0.25, 0.25, 0.2)
    const OwnedStats s0 = dev.ownedStats();
    const Pixels served = render(true, true, "owned_served");
    const OwnedStats s1 = dev.ownedStats();
    std::uint32_t bad = 0;
    for (std::size_t i = 0; i < ref.bgra.size() && i < served.bgra.size(); ++i) bad += ref.bgra[i] != served.bgra[i];
    if (bad != 0 || ref.bgra.size() != served.bgra.size()) {
        std::printf("  FAIL owned windows: %u bytes differ between the register file and the served images\n", bad);
        ++g_failures;
    }
    if (s1.view1 - s0.view1 != 1 || s1.object - s0.object != 1) {
        std::printf("  FAIL owned windows: served %llu VS1 / %llu VS2 windows (expected 1 / 1)\n", static_cast<unsigned long long>(s1.view1 - s0.view1),
                    static_cast<unsigned long long>(s1.object - s0.object));
        ++g_failures;
    }
    // a changed image changes the picture
    const float c12b[4] = {0.6f, 0.5f, 0.1f, 0.5f};
    std::memcpy(bytes + kPad, c12b, sizeof(c12b));
    buf->flush();
    const Pixels changed = render(true, true, "owned_changed");
    expect("owned windows, changed image", changed, 32, 32, 0xFF000000u | (115u << 16) | (115u << 8) | 25u, 3);  // ((0.6 + 0.3) x 0.5, (0.5 + 0.4) x 0.5, (0.1 + 0.1) x 0.5)
    // an object image shorter than the shader's read extent (c31..c33 = 3 registers): the registers are read, the window is counted as short
    std::memcpy(bytes + kPad, c12, sizeof(c12));
    buf->flush();
    ow.w[0][2].covered = 2;
    const OwnedStats s2 = dev.ownedStats();
    const Pixels fallback = render(true, false, "owned_short");  // (the register file holds the true values here)
    const OwnedStats s3 = dev.ownedStats();
    bad = 0;
    for (std::size_t i = 0; i < ref.bgra.size() && i < fallback.bgra.size(); ++i) bad += ref.bgra[i] != fallback.bgra[i];
    if (bad != 0 || s3.shortVs2 - s2.shortVs2 != 1 || s3.object != s2.object) {
        std::printf("  FAIL owned windows: a short object image: %u bytes differ, %llu short VS2 windows, %llu served\n", bad, static_cast<unsigned long long>(s3.shortVs2 - s2.shortVs2),
                    static_cast<unsigned long long>(s3.object - s2.object));
        ++g_failures;
    }
    // a null base is never used (the guard of the tables): the register file is read
    ow.w[0][2].covered = 5;
    ow.w[0][2].base = rhi::GpuRef{};
    const Pixels nullBase = render(true, false, "owned_null");
    bad = 0;
    for (std::size_t i = 0; i < ref.bgra.size() && i < nullBase.bgra.size(); ++i) bad += ref.bgra[i] != nullBase.bgra[i];
    if (bad != 0) {
        std::printf("  FAIL owned windows: a null window base was used (%u bytes differ)\n", bad);
        ++g_failures;
    }
}

// F6-1 bone table (frame-ownership.md 7.40) through the device's own path: a vertex shader that skins its quad with the bone rows c33..c35, adds the morph weights c31 and scales
// by the fade c32.w is drawn normally (reference, registers), then with VS window 2 served from the table's image (kind Bones: the register file holds junk). The pictures must be
// identical; the same key served twice with the same content writes nothing, with another content in the same frame is refused (the GPU sees one value per frame) and the draw keeps
// reading the first image, a new frame takes the new image (= the reference of the new registers); a slot released after the idle frames is not handed out again before the
// frames in flight are done; guards (no wrapper / no image / no registers / too many) serve nothing; the GPU read-backs of the table's uploads must not differ
void testBoneTable(Fixture& f) {
    Device& dev = f.dev;
    static const char* kVsBone = R"(
vs_3_0
dcl_position v0
dcl_color v1
dcl_texcoord v2
dcl_position o0
dcl_color o1
dcl_texcoord o2
dp4 o0.x, v0, c33
dp4 o0.y, v0, c34
dp4 o0.z, v0, c35
mov o0.w, v0.w
mul r0, v1, c32.w
add o1, r0, c31
mov o2, v2
)";
    const auto vsBone = shaderFrom(dev, kVsBone);
    if (!vsBone) {
        std::printf("  FAIL bone table: shader\n");
        ++g_failures;
        return;
    }
    gpuscene::BoneTable& bt = gpuscene::bonetable();
    bt.destroy();
    bt.setActive(true);
    for (int i = 0; i < 3 && !bt.ready(); ++i) bt.flush(dev);  // (the first flush creates the buffer)
    if (!bt.ready()) {
        std::printf("  FAIL bone table: the buffer was not created\n");
        ++g_failures;
        bt.setActive(false);
        return;
    }
    struct Img {
        float weights[3];
        float fade;
        float rows[3][4];
    };
    const Img imgA = {{0.1f, 0.2f, 0.3f}, 0.5f, {{0.5f, 0, 0, 0}, {0, 0.5f, 0, 0}, {0, 0, 1, 0}}};
    const Img imgB = {{0.0f, 0.0f, 0.1f}, 1.0f, {{0.25f, 0, 0, 0}, {0, 0.25f, 0, 0}, {0, 0, 1, 0}}};
    const auto regsOf = [](const Img& i, float (*regs)[4]) {
        std::memset(regs, 0, sizeof(float) * 4 * gpuscene::kBoneRegs);
        std::memcpy(regs[0], i.weights, 12);
        regs[1][3] = i.fade;
        std::memcpy(regs[2], i.rows, sizeof(i.rows));
    };
    static float regsA[gpuscene::kBoneRegs][4], regsB[gpuscene::kBoneRegs][4];
    regsOf(imgA, regsA);
    regsOf(imgB, regsB);
    int wrapperA = 0, wrapperB = 0;
    const gpuscene::BoneKey keyA{&wrapperA, 0, 0x3f000000u}, keyB{&wrapperB, 3, 0x3f800000u};
    const auto setRegisters = [&](const Img& i, bool junk) {
        const float j4[4] = {0.9f, 0.9f, 0.9f, 0.9f};
        const float c31[4] = {junk ? 0.9f : i.weights[0], junk ? 0.9f : i.weights[1], junk ? 0.9f : i.weights[2], 0.0f};
        const float c32[4] = {0, 0, 0, junk ? 0.9f : i.fade};
        dev.setVertexShaderConstantF(31, c31, 1);
        dev.setVertexShaderConstantF(32, c32, 1);
        for (std::uint32_t r = 0; r < 3; ++r) dev.setVertexShaderConstantF(33 + r, junk ? j4 : i.rows[r], 1);
    };
    const auto render = [&](const Img& i, const d3d9::OwnedWindows::Win* win, const char* name) {
        f.reset();
        dev.setVertexShader(vsBone);
        setRegisters(i, win != nullptr);
        OwnedWindows ow;
        if (win != nullptr) {
            ow.w[0][2] = *win;
            dev.setOwnedWindows(&ow);
        }
        f.draw(quad(-1, -1, 1, 1, 0.5f, 0xFFFFFFFF));
        dev.setOwnedWindows(nullptr);
        bt.flush(dev);  // (the table's uploads go into the frame's upload list, which runs before the draws at the submit below)
        return capture(dev, name);
    };
    const auto differ = [](const Pixels& a, const Pixels& b) {
        std::uint32_t n = 0;
        for (std::size_t i = 0; i < a.bgra.size() && i < b.bgra.size(); ++i) n += a.bgra[i] != b.bgra[i];
        return a.bgra.size() != b.bgra.size() ? 0xffffffffu : n;
    };
    const Pixels refA = render(imgA, nullptr, "bone_ref_a");
    const Pixels refB = render(imgB, nullptr, "bone_ref_b");
    // (the quad's centre: white x 0.5 + the weights (0.1, 0.2, 0.3) = (0.6, 0.7, 0.8); the quad covers a quarter of the target)
    expect("bone table reference A", refA, 32, 32, 0xFF000000u | (153u << 16) | (178u << 8) | 204u, 3);
    expect("bone table reference A (outside the quad)", refA, 2, 2, 0xFF000000u, 3);
    if (differ(refA, refB) == 0) {
        std::printf("  FAIL bone table: the two images draw the same picture (the shader does not read them)\n");
        ++g_failures;
    }
    // serve A, draw with junk registers: equal to the reference
    d3d9::OwnedWindows::Win winA, winA2;
    {
        const OwnedStats s0 = dev.ownedStats();
        if (bt.serve(keyA, regsA, 2 + 3, winA) != gpuscene::BoneWhy::Served || winA.kind != OwnedWindows::Kind::Bones || winA.covered != gpuscene::kBoneRegs || winA.base.null() || winA.image == nullptr) {
            std::printf("  FAIL bone table: the first serve\n");
            ++g_failures;
            bt.setActive(false);
            return;
        }
        const Pixels p = render(imgA, &winA, "bone_served_a");
        const OwnedStats s1 = dev.ownedStats();
        const std::uint32_t n = differ(refA, p);
        if (n != 0) {
            std::printf("  FAIL bone table: %u bytes differ between the register file and the served image\n", n);
            ++g_failures;
        }
        if (s1.bones - s0.bones != 1) {
            std::printf("  FAIL bone table: the device served %llu bone windows (expected 1)\n", static_cast<unsigned long long>(s1.bones - s0.bones));
            ++g_failures;
        }
    }
    // (a new frame: the table flushed in the render above) the same content again = nothing written
    {
        const gpuscene::BoneStats b0 = bt.stats();
        const std::uint64_t slots0 = bt.tableStats().slotsUploaded;
        const bool again = bt.serve(keyA, regsA, 5, winA2) == gpuscene::BoneWhy::Served && winA2.base.bits == winA.base.bits;
        bt.flush(dev);
        if (!again || bt.stats().unchanged != b0.unchanged + 1 || bt.stats().rewrites != b0.rewrites || bt.tableStats().slotsUploaded != slots0) {
            std::printf("  FAIL bone table: the same content again: served %d, unchanged %llu -> %llu, rewrites %llu -> %llu, slots uploaded %llu -> %llu\n", again, static_cast<unsigned long long>(b0.unchanged),
                        static_cast<unsigned long long>(bt.stats().unchanged), static_cast<unsigned long long>(b0.rewrites), static_cast<unsigned long long>(bt.stats().rewrites),
                        static_cast<unsigned long long>(slots0), static_cast<unsigned long long>(bt.tableStats().slotsUploaded));
            ++g_failures;
        }
    }
    // another content in the same frame: refused, the window is left alone and the draw reads the first image
    {
        d3d9::OwnedWindows::Win w1, w2, untouched;
        untouched.kind = OwnedWindows::Kind::Object;  // (a marker: serve must not write `out` on a refusal)
        w2 = untouched;
        if (bt.serve(keyA, regsB, 5, w1) != gpuscene::BoneWhy::Served) {
            std::printf("  FAIL bone table: the new frame's first serve of A with another content\n");
            ++g_failures;
        }
        const gpuscene::BoneStats b0 = bt.stats();
        const gpuscene::BoneWhy again = bt.serve(keyA, regsA, 5, w2);
        if (again != gpuscene::BoneWhy::Conflict || w2.kind != OwnedWindows::Kind::Object || bt.stats().why[static_cast<std::size_t>(gpuscene::BoneWhy::Conflict)] !=
                                                                                                  b0.why[static_cast<std::size_t>(gpuscene::BoneWhy::Conflict)] + 1) {
            std::printf("  FAIL bone table: another content in the same frame was not refused (%d)\n", static_cast<int>(again));
            ++g_failures;
        }
        // the frame's image is B (the first serve of the frame): the draw with the window = the reference of B
        const Pixels p = render(imgB, &w1, "bone_served_b");
        if (const std::uint32_t n = differ(refB, p); n != 0) {
            std::printf("  FAIL bone table: a new frame's content: %u bytes differ from the reference of the new registers\n", n);
            ++g_failures;
        }
        d3d9::OwnedWindows::Win sw;
        if (!bt.servedNow(keyA, sw) == false) {
            // (flushed by the render above: a new frame has begun, nothing is served in it yet)
            std::printf("  FAIL bone table: servedNow holds in a frame that has not served the key\n");
            ++g_failures;
        }
        if (bt.serve(keyA, regsB, 5, w1) != gpuscene::BoneWhy::Served || !bt.servedNow(keyA, sw) || sw.base.bits != w1.base.bits) {
            std::printf("  FAIL bone table: servedNow\n");
            ++g_failures;
        }
    }
    // guards: nothing is served, nothing is written
    {
        d3d9::OwnedWindows::Win w;
        const gpuscene::BoneKey none{nullptr, 0, 0};
        const bool ok = bt.serve(none, regsA, 5, w) == gpuscene::BoneWhy::NotReady && bt.serve(keyB, nullptr, 5, w) == gpuscene::BoneWhy::NotReady && bt.serve(keyB, regsA, 0, w) == gpuscene::BoneWhy::TooLong &&
                        bt.serve(keyB, regsA, 1, w) == gpuscene::BoneWhy::TooLong && bt.serve(keyB, regsA, gpuscene::kBoneRegs + 1, w) == gpuscene::BoneWhy::TooLong && w.kind == OwnedWindows::Kind::None &&
                        !bt.windowFor(keyB, w);
        if (!ok) {
            std::printf("  FAIL bone table: the guards (no wrapper / no registers / too few / too many) served something\n");
            ++g_failures;
        }
    }
    // idle release + deferred reuse: A idles for kIdleFrames, its slot is released, B (served every frame) never gets that slot before the frames in flight are done
    {
        d3d9::OwnedWindows::Win a0, b0, b1;
        bt.windowFor(keyA, a0);
        for (std::uint64_t i = 0; i < gpuscene::BoneTable::kIdleFrames + 2; ++i) {
            bt.serve(keyB, regsB, 5, b0);
            bt.flush(dev);
        }
        const bool released = !bt.windowFor(keyA, a0) && bt.stats().released >= 1 && b0.base.bits != 0;
        if (!released) {
            std::printf("  FAIL bone table: the idle slot was not released (live slots %zu)\n", bt.liveSlots());
            ++g_failures;
        }
        d3d9::OwnedWindows::Win old;
        old.base.bits = 0;
        // (the released slot's base = the first slot's: keyA was the first key served after destroy())
        const std::uint64_t firstBase = winA.base.bits;
        const gpuscene::BoneKey keyC{&wrapperA, 7, 0x3f800000u}, keyD{&wrapperA, 8, 0x3f800000u};
        d3d9::OwnedWindows::Win wc, wd;
        bool reusedEarly = false;
        if (bt.serve(keyC, regsA, 5, wc) == gpuscene::BoneWhy::Served) reusedEarly = wc.base.bits == firstBase;
        if (reusedEarly) {
            std::printf("  FAIL bone table: a released slot was handed out again before the frames in flight were done\n");
            ++g_failures;
        }
        for (std::uint32_t i = 0; i < rhi::kFramesInFlight + 2; ++i) {
            bt.serve(keyB, regsB, 5, b1);
            bt.serve(keyC, regsA, 5, wc);
            bt.flush(dev);
        }
        const bool back = bt.serve(keyD, regsA, 5, wd) == gpuscene::BoneWhy::Served && wd.base.bits == firstBase;
        if (!back) {
            std::printf("  FAIL bone table: the released slot never came back after the deferral\n");
            ++g_failures;
        }
    }
    // the GPU read-backs of everything uploaded (every 240th flush): none differs
    render(imgB, nullptr, "bone_end");  // (a submit: the frames' read-back copies complete)
    for (int i = 0; i < 40; ++i) {
        bt.flush(dev);
        Sleep(10);
    }
    if (bt.tableStats().verifyDone == 0 || bt.tableStats().verifyBad != 0) {
        std::printf("  FAIL bone table: GPU read-backs %llu, differing slots %llu\n", static_cast<unsigned long long>(bt.tableStats().verifyDone), static_cast<unsigned long long>(bt.tableStats().verifyBad));
        ++g_failures;
    }
    std::printf("  bone table: %llu requests, %llu served (%llu unchanged, %llu rewritten), %llu slots, %llu released, %llu refused, read-backs %llu\n", static_cast<unsigned long long>(bt.stats().requests),
                static_cast<unsigned long long>(bt.stats().served), static_cast<unsigned long long>(bt.stats().unchanged), static_cast<unsigned long long>(bt.stats().rewrites),
                static_cast<unsigned long long>(bt.stats().newSlots), static_cast<unsigned long long>(bt.stats().released), static_cast<unsigned long long>(bt.stats().requests - bt.stats().served),
                static_cast<unsigned long long>(bt.tableStats().verifyDone));
    bt.setActive(false);
    dev.setOwnedWindows(nullptr);
    bt.destroy();
}

// F4-1 owned submit (frame-ownership.md 7.17) through the device's own path: four quads of a translated vertex shader that reads VS window 0 (c0) and window 2 (c32.w, c33)
// are drawn normally (reference), captured as retained scene draws, and then submitted with Device::submitOwned from the retained packets alone: the register file holds
// junk, the windows are persistent images of the caller's (a view-block-like c0 image, a billboard-constants-like c31.. image). The pictures must be identical, the four
// draws must leave as one indirect run; refusals (a window image shorter than the shader reads, a null base, a dry run) must draw nothing, a changed image must change
// the picture (the draw really reads it)
// F2f (frame-ownership.md 14.19): the instance row table's images as VS window 2 of a static draw (c32.w, c33..c35): drawn through the device's window override with junk in the
// register file, equal to the register-file reference; a released slot is refused (GPU safety: no window into a freed image) and handed out again only after the deferral
void testInstanceRows(Fixture& f) {
    Device& dev = f.dev;
    static const char* kVs = R"(
vs_3_0
dcl_position v0
dcl_color v1
dcl_texcoord v2
dcl_position o0
dcl_color o1
dcl_texcoord o2
dp4 o0.x, v0, c33
dp4 o0.y, v0, c34
dp4 o0.z, v0, c35
mov o0.w, v0.w
mul o1, v1, c32.w
mov o2, v2
)";
    const auto vs = shaderFrom(dev, kVs);
    if (!vs) {
        std::printf("  FAIL instance rows: shader\n");
        ++g_failures;
        return;
    }
    gpuscene::RowTable& rt = gpuscene::rowtable();
    rt.releaseGpu();
    rt.setActive(true);
    for (int i = 0; i < 3 && !rt.ready(); ++i) rt.flush(dev);
    if (!rt.ready()) {
        std::printf("  FAIL instance rows: the buffer was not created\n");
        ++g_failures;
        rt.setActive(false);
        return;
    }
    float regsA[gpuscene::kObjectRegs][4] = {{9, 9, 9, 9}, {7, 7, 7, 0.5f}, {0.5f, 0, 0, 0.25f}, {0, 0.5f, 0, 0}, {0, 0, 1, 0}};
    float regsB[gpuscene::kObjectRegs][4] = {{9, 9, 9, 9}, {7, 7, 7, 1.0f}, {0.25f, 0, 0, -0.5f}, {0, 0.25f, 0, 0.25f}, {0, 0, 1, 0}};
    const std::uint32_t bits = 0x1eu;  // c32 .. c35 composed
    const auto setRegisters = [&](const float (*r)[4], bool junk) {
        const float j4[4] = {0.9f, 0.9f, 0.9f, 0.9f};
        for (std::uint32_t i = 1; i < gpuscene::kObjectRegs; ++i) dev.setVertexShaderConstantF(31 + i, junk ? j4 : r[i], 1);
    };
    const auto render = [&](const float (*r)[4], const d3d9::OwnedWindows::Win* win, const char* name) {
        f.reset();
        dev.setVertexShader(vs);
        setRegisters(r, win != nullptr);
        OwnedWindows ow;
        if (win != nullptr) {
            ow.w[0][2] = *win;
            dev.setOwnedWindows(&ow);
        }
        f.draw(quad(-1, -1, 1, 1, 0.5f, 0xFFFFFFFF));
        dev.setOwnedWindows(nullptr);
        rt.flush(dev);
        return capture(dev, name);
    };
    const auto differ = [](const Pixels& a, const Pixels& b) {
        std::uint32_t n = 0;
        for (std::size_t i = 0; i < a.bgra.size() && i < b.bgra.size(); ++i) n += a.bgra[i] != b.bgra[i];
        return a.bgra.size() != b.bgra.size() ? 0xffffffffu : n;
    };
    const Pixels refA = render(regsA, nullptr, "rows_ref_a");
    const Pixels refB = render(regsB, nullptr, "rows_ref_b");
    if (differ(refA, refB) == 0) {
        std::printf("  FAIL instance rows: the two images draw the same picture (the shader does not read them)\n");
        ++g_failures;
    }
    const std::uint32_t a = rt.alloc(regsA, bits), b = rt.alloc(regsB, bits);
    d3d9::OwnedWindows::Win wa, wb;
    if (a == gpuscene::kNoSlot || b == gpuscene::kNoSlot || a == b || !rt.windowFor(a, bits, wa) || !rt.windowFor(b, bits, wb) || wa.kind != OwnedWindows::Kind::Object || wa.covered != 5 ||
        wa.base.null() || wa.image == nullptr || !rt.same(a, regsA, bits) || rt.same(a, regsB, bits)) {
        std::printf("  FAIL instance rows: alloc / window\n");
        ++g_failures;
        rt.setActive(false);
        return;
    }
    rt.flush(dev);  // (the images' upload)
    const OwnedStats s0 = dev.ownedStats();
    const Pixels pa = render(regsA, &wa, "rows_served_a");
    const Pixels pb = render(regsB, &wb, "rows_served_b");
    const OwnedStats s1 = dev.ownedStats();
    if (const std::uint32_t n = differ(refA, pa); n != 0) {
        std::printf("  FAIL instance rows: image A: %u bytes differ from the register-file reference\n", n);
        ++g_failures;
    }
    if (const std::uint32_t n = differ(refB, pb); n != 0) {
        std::printf("  FAIL instance rows: image B: %u bytes differ from the register-file reference\n", n);
        ++g_failures;
    }
    if (s1.object - s0.object != 2) {
        std::printf("  FAIL instance rows: the device served %llu object windows (expected 2)\n", static_cast<unsigned long long>(s1.object - s0.object));
        ++g_failures;
    }
    // a c31-reading draw gets no image
    if (rt.alloc(regsA, bits | 1u) != gpuscene::kNoSlot) {
        std::printf("  FAIL instance rows: an image with c31 composed was handed out\n");
        ++g_failures;
    }
    // released: no window any more; the slot comes back only after kFramesInFlight + 1 presents
    rt.release(a);
    d3d9::OwnedWindows::Win dead;
    if (rt.windowFor(a, bits, dead)) {
        std::printf("  FAIL instance rows: a released slot still has a window\n");
        ++g_failures;
    }
    bool early = false;
    for (std::uint32_t i = 0; i < rhi::kFramesInFlight; ++i) {
        const std::uint32_t c = rt.alloc(regsB, bits);
        if (c == a) early = true;
        if (c != gpuscene::kNoSlot) rt.release(c);
        rt.flush(dev);
    }
    if (early) {
        std::printf("  FAIL instance rows: a released slot was handed out before the deferral\n");
        ++g_failures;
    }
    rt.release(b);
    for (std::uint32_t i = 0; i < rhi::kFramesInFlight + 2; ++i) rt.flush(dev);
    rt.setActive(false);
    rt.releaseGpu();
    std::printf("  instance rows (F2f): served draws equal the register file, release deferred\n");
}

void testOwnedSubmit(Fixture& f) {
    Device& dev = f.dev;
    dev.setSceneDraws(true, 0);
    if (!dev.sceneDrawsActive()) {
        std::printf("  SKIP owned submit: scene draws are not available on this device\n");
        return;
    }
    static const char* kVsOwned2 = R"(
vs_3_0
dcl_position v0
dcl_color v1
dcl_texcoord v2
dcl_position o0
dcl_color o1
dcl_texcoord o2
mov o0, v0
add r0, c0, c33
mul o1, r0, c32.w
mov o2, v2
)";
    const auto vsOwned = shaderFrom(dev, kVsOwned2);
    if (!vsOwned) {
        std::printf("  FAIL owned submit: shader\n");
        ++g_failures;
        return;
    }
    constexpr std::uint32_t kQuads = 4;
    std::vector<Vertex> verts;
    for (std::uint32_t k = 0; k < kQuads; ++k) {
        const float x0 = -1.0f + 0.5f * static_cast<float>(k), x1 = x0 + 0.5f;
        verts.push_back({x0, 0.5f, 0.5f, 1, 0xFFFFFFFFu, 0, 0});
        verts.push_back({x1, 0.5f, 0.5f, 1, 0xFFFFFFFFu, 1, 0});
        verts.push_back({x0, -0.5f, 0.5f, 1, 0xFFFFFFFFu, 0, 1});
        verts.push_back({x1, -0.5f, 0.5f, 1, 0xFFFFFFFFu, 1, 1});
    }
    const std::uint16_t idx[6] = {0, 1, 2, 1, 3, 2};
    auto vb = dev.createBuffer(static_cast<std::uint32_t>(verts.size() * sizeof(Vertex)), USAGE_WRITEONLY, POOL_MANAGED, false, false);
    auto ib = dev.createBuffer(sizeof(idx), USAGE_WRITEONLY, POOL_MANAGED, true, false);
    std::memcpy(vb->lock(0, 0, 0), verts.data(), verts.size() * sizeof(Vertex));
    vb->unlock();
    std::memcpy(ib->lock(0, 0, 0), idx, sizeof(idx));
    ib->unlock();
    rhi::Device* rd = dev.rhi();
    constexpr std::uint64_t kPad = 4096, kViewOffset = kPad, kBbOffset = kPad + 1024, kBbBytes = 201 * 16;
    auto buf = rd->createBuffer({kBbOffset + kBbBytes + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "owned submit test"});
    if (!buf) {
        std::printf("  FAIL owned submit: buffer\n");
        ++g_failures;
        return;
    }
    auto* bytes = static_cast<std::uint8_t*>(buf->map());
    std::memset(bytes, 0, static_cast<std::size_t>(kBbOffset + kBbBytes + 256));
    const float c0[4] = {0.2f, 0.1f, 0.3f, 0.5f};
    std::memcpy(bytes + kViewOffset, c0, sizeof(c0));
    // the constants image from c31: c32.w = 0.5, c33 = (0.3, 0.4, 0.1, 0.5)
    const float c32[4] = {0.0f, 0.0f, 0.0f, 0.5f}, c33[4] = {0.3f, 0.4f, 0.1f, 0.5f};
    std::memcpy(bytes + kBbOffset + 16, c32, sizeof(c32));
    std::memcpy(bytes + kBbOffset + 32, c33, sizeof(c33));
    buf->flush();
    const auto imageRef = [&](std::uint64_t offset) {
        const auto w = buf->shaderRef(offset);
        return rhi::GpuRef::fromWords(w[0], w[1]);
    };
    OwnedWindows win;
    win.w[0][0].kind = OwnedWindows::Kind::View;
    win.w[0][0].base = imageRef(kViewOffset);
    win.w[0][0].covered = 12;
    win.w[0][2].kind = OwnedWindows::Kind::Object;
    win.w[0][2].base = imageRef(kBbOffset).plus(-31 * 16);
    win.w[0][2].covered = 201;
    const auto setup = [&](bool junk) {
        f.reset();
        dev.drawTag() = {};
        dev.drawTag().source = draws::Source::Mesh;
        dev.setVertexShader(vsOwned);
        const float junk4[4] = {0.9f, 0.9f, 0.9f, 0.9f};
        const float j32[4] = {0.9f, 0.0f, 0.0f, 0.9f};
        dev.setVertexShaderConstantF(0, junk ? junk4 : c0, 1);
        dev.setVertexShaderConstantF(32, junk ? j32 : c32, 1);
        dev.setVertexShaderConstantF(33, junk ? junk4 : c33, 1);
        dev.setStreamSource(0, vb.get(), 0, sizeof(Vertex));
        dev.setIndices(ib.get());
        dev.phases().push(scene::View::Main, scene::Pass::Solid);
    };
    const auto bindings = [&] {
        Device::DeriveIn in;
        in.streams[0] = vb.get();
        in.strides[0] = sizeof(Vertex);
        in.indices = ib.get();
        return in;
    };
    // reference: normal draws, the register file true, scene draws off
    setup(false);
    for (std::uint32_t k = 0; k < kQuads; ++k) dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
    dev.phases().pop();
    const Pixels ref = capture(dev, "owned_submit_ref");
    expect("owned submit reference", ref, 8, 32, 0xFF000000u | (64u << 16) | (64u << 8) | 51u, 3);  // ((0.2 + 0.3) x 0.5, (0.1 + 0.4) x 0.5, (0.3 + 0.1) x 0.5)
    // retained scene draws of the four quads (the scene variants and the pipeline are built in the background: retried until the draws are scene draws)
    Device::CachedDraw cd[kQuads];
    bool ready = false;
    for (int attempt = 0; attempt < 1200 && !ready; ++attempt) {
        setup(false);
        dev.setSceneIssue(true);
        ready = true;
        for (std::uint32_t k = 0; k < kQuads; ++k) {
            dev.beginCapture(&cd[k]);
            dev.drawIndexedPrimitive(PT_TRIANGLELIST, static_cast<std::int32_t>(4 * k), 0, 4, 0, 2);
            ready = dev.endCapture() && cd[k].valid && cd[k].scenePipeline != GamePipeline::None && ready;
        }
        dev.setSceneIssue(false);
        dev.phases().pop();
        capture(dev, "owned_submit_capture");
        if (!ready) Sleep(50);
    }
    if (!ready) {
        std::printf("  FAIL owned submit: the draws were never retained as scene draws\n");
        ++g_failures;
        return;
    }
    const auto same = [&](const Pixels& p, const char* what) {
        std::uint32_t bad = 0;
        for (std::size_t i = 0; i < ref.bgra.size() && i < p.bgra.size(); ++i) bad += ref.bgra[i] != p.bgra[i];
        if (bad != 0 || ref.bgra.size() != p.bgra.size()) {
            std::printf("  FAIL owned submit (%s): %u bytes differ from the normal draws\n", what, bad);
            ++g_failures;
        }
    };
    const auto run = [&](const char* name, std::uint32_t quads, Device::OwnedWhy want, bool dry) {
        setup(true);  // (junk in the register file: nothing may read it)
        for (std::uint32_t k = 0; k < quads; ++k) {
            const Device::OwnedWhy r = dev.submitOwned(cd[k], bindings(), win, dry);
            if (r != want) {
                std::printf("  FAIL owned submit (%s): quad %u answered %u, expected %u\n", name, k, static_cast<unsigned>(r), static_cast<unsigned>(want));
                ++g_failures;
            }
        }
        dev.phases().pop();
        return capture(dev, name);
    };
    const auto s0 = dev.sceneStats();
    const std::uint64_t o0 = dev.ownedSubmitted();
    const Pixels owned = run("owned_submit", kQuads, Device::OwnedWhy::Submitted, false);
    const auto s1 = dev.sceneStats();
    same(owned, "owned");
    if (dev.ownedSubmitted() - o0 != kQuads || s1.indirectCalls - s0.indirectCalls != 1 || s1.records - s0.records != kQuads) {
        std::printf("  FAIL owned submit: %llu submitted / %llu indirect calls / %llu records for %u quads (expected one call over all)\n",
                    static_cast<unsigned long long>(dev.ownedSubmitted() - o0), static_cast<unsigned long long>(s1.indirectCalls - s0.indirectCalls),
                    static_cast<unsigned long long>(s1.records - s0.records), kQuads);
        ++g_failures;
    }
    // a dry run draws nothing (a cleared back buffer: the quads' pixel is black)
    const Pixels dry = run("owned_submit_dry", kQuads, Device::OwnedWhy::Submitted, true);
    if ((dry.at(8, 32) & 0xFFFFFFu) != 0) {
        std::printf("  FAIL owned submit: a dry run drew (pixel %06X)\n", dry.at(8, 32) & 0xFFFFFF);
        ++g_failures;
    }
    // a constants image shorter than the shader reads: refused, nothing drawn
    win.w[0][2].covered = 2;
    const std::uint64_t o1 = dev.ownedSubmitted();
    const Pixels shortImg = run("owned_submit_short", kQuads, Device::OwnedWhy::Window, false);
    if (dev.ownedSubmitted() != o1 || (shortImg.at(8, 32) & 0xFFFFFFu) != 0) {
        std::printf("  FAIL owned submit: a short window image was used (pixel %06X)\n", shortImg.at(8, 32) & 0xFFFFFF);
        ++g_failures;
    }
    // a null base: refused
    win.w[0][2].covered = 201;
    const rhi::GpuRef goodBase = win.w[0][2].base;
    win.w[0][2].base = rhi::GpuRef{};
    run("owned_submit_null", kQuads, Device::OwnedWhy::Window, false);
    win.w[0][2].base = goodBase;
    // the null-address guard of the scene packets refuses the draw too (nothing drawn)
    dev.setSceneFaultInject(true);
    const std::uint64_t o2 = dev.ownedSubmitted();
    run("owned_submit_guard", kQuads, Device::OwnedWhy::Packet, false);
    dev.setSceneFaultInject(false);
    if (dev.ownedSubmitted() != o2) {
        std::printf("  FAIL owned submit: the null-address guard let %llu draws through\n", static_cast<unsigned long long>(dev.ownedSubmitted() - o2));
        ++g_failures;
    }
    // and it works again afterwards
    same(run("owned_submit_again", kQuads, Device::OwnedWhy::Submitted, false), "owned again");
    // a changed image changes the picture (the draw really reads the window)
    const float c33b[4] = {0.6f, 0.5f, 0.1f, 0.5f};
    std::memcpy(bytes + kBbOffset + 32, c33b, sizeof(c33b));
    buf->flush();
    const Pixels changed = run("owned_submit_changed", kQuads, Device::OwnedWhy::Submitted, false);
    expect("owned submit, changed image", changed, 8, 32, 0xFF000000u | (102u << 16) | (77u << 8) | 51u, 3);  // ((0.2 + 0.6) x 0.5, (0.1 + 0.5) x 0.5, (0.3 + 0.1) x 0.5)
    dev.setSceneDraws(false, 0);
    dev.release(std::move(vb));
    dev.release(std::move(ib));
}

// 14.26: the bone-motion forms of an instance-list draw (Device::rigidListPredict, the push form) into a G-buffer motion target. Two instances of one draw key (an
// instanced-mesh draw: tag source Mesh, render-op node 0xffffff; VS window 2 uploaded, no object record): A moves 0.2 in x every frame (alternating), B stands still.
// With the instances' identities (setInstanceIdentity, as StaticOps names them) A gets its own motion (the previous rows block: uv motion -0.1 / +0.1 in x), B none; without
// them (two placements under one key: the old list path) A gets camera motion only (0): the negative control. The motion target (R16G16F, the G-buffer's format) is read
// through a visualisation draw (motion x 4 + 0.5 into the back buffer)
void testListMotion(Fixture& f) {
    Device& dev = f.dev;
    if (!dev.game().gameCaps().vertexStores && g_backend == rhi::Backend::Vulkan) {
        std::printf("  (list motion: needs vertex stores: skipped)\n");
        return;
    }
    static const char* kVsRigid = R"(
vs_3_0
dcl_position v0
dcl_color v1
dcl_texcoord v2
dcl_position o0
dcl_color o1
dcl_texcoord o2
dp4 o0.x, v0, c33
dp4 o0.y, v0, c34
dp4 o0.z, v0, c35
mov o0.w, v0.w
mov o1, v1
mov o2, v2
)";
    static const char* kPsShow = R"(
ps_3_0
def c0, 4, 4, 0, 0
def c1, 0.5, 0.5, 0, 1
dcl_texcoord v0.xy
dcl_2d s0
texld r0, v0, s0
mad oC0, r0, c0, c1
)";
    const auto vs = shaderFrom(dev, kVsRigid);
    static const char* kPsMat = R"(
ps_3_0
dcl_color v0
mov r0, v0
mov oC0, r0
)";
    const auto ps = shaderFrom(dev, kPsMat);  // (marked as an engine material shader below)
    const auto show = shaderFrom(dev, kPsShow);
    if (!vs || !ps || !show) {
        std::printf("  FAIL list motion: shader\n");
        ++g_failures;
        return;
    }
    // (the material flag makes the G-buffer variants apply: gbufferLit / materialVariantOptions)
    const std::uint32_t psFlagsBefore = ps->semantics.flags;
    ps->semantics.flags |= ShaderSemantics::Material;
    const std::uint32_t bw = dev.backBuffer()->width(), bh = dev.backBuffer()->height();
    TextureDesc md;
    md.width = bw;
    md.height = bh;
    md.format = rhi::d3dfmt::G16R16F;
    md.usage = USAGE_RENDERTARGET;
    md.pool = POOL_DEFAULT;
    auto motionTex = dev.createTexture(md);
    if (!motionTex) {
        std::printf("  FAIL list motion: motion target\n");
        ++g_failures;
        ps->semantics.flags = psFlagsBefore;
        return;
    }
    using GO = enh::GbufferOutputs;
    dev.setVariantProvider(VariantSlot::GbufferM, {enh::materialVariantTag("gbuf_m"), [](const Shader& s, shader::TranslateOptions& o) {
                                                       return enh::materialVariantOptions(s, enh::MaterialVariant::Gbuffer, o, GO::Motion);
                                                   }});
    dev.setVariantProvider(VariantSlot::GbufferMotionM, {enh::materialVariantTag("gbufm_m"), [](const Shader& s, shader::TranslateOptions& o) {
                                                             return enh::materialVariantOptions(s, enh::MaterialVariant::GbufferMotion, o, GO::Motion);
                                                         }});
    dev.setBoneMotion(1);
    dev.setRigidMotion(true);
    dev.setRigidMotionWanted(true);
    Texture* hdr = dev.backBuffer()->texture();
    dev.tagImage(hdr, scene::ImageRole::SceneHdr);
    Device::GbufferTargets gt{nullptr, &motionTex->rhi(), bw, bh, true};
    dev.setGbuffer(gt);
    const Vertex verts[4] = {{-0.15f, 0.15f, 0.5f, 1, 0xFFFFFFFF, 0, 0}, {0.15f, 0.15f, 0.5f, 1, 0xFFFFFFFF, 1, 0}, {-0.15f, -0.15f, 0.5f, 1, 0xFFFFFFFF, 0, 1}, {0.15f, -0.15f, 0.5f, 1, 0xFFFFFFFF, 1, 1}};
    const std::uint16_t idx[6] = {0, 1, 2, 1, 3, 2};
    auto vb = dev.createBuffer(sizeof(verts), USAGE_WRITEONLY, POOL_MANAGED, false, false);
    auto ib = dev.createBuffer(sizeof(idx), USAGE_WRITEONLY, POOL_MANAGED, true, false);
    std::memcpy(vb->lock(0, 0, 0), verts, sizeof(verts));
    vb->unlock();
    std::memcpy(ib->lock(0, 0, 0), idx, sizeof(idx));
    ib->unlock();
    scene::CameraInput cam;
    cam.view = cam.projection = cam.viewProjection = cam.inverseView = scene::Mat4::identity();
    cam.engineFar = 1000.0f;
    alignas(16) static unsigned char entity[0x100];
    static int instA = 0, instB = 0;
    const float ax = -0.5f, ay = 0.4f, bx = 0.4f, by = -0.4f;
    bool sceneForm = false;  // the scene form (native.owned_instances: an instance-list draw's scene record, prevVs = the previous rows block) instead of the push form
    const auto frame = [&](int k, bool identities, bool showMotion) -> Pixels {
        f.reset();
        dev.setMainCamera(cam);
        dev.beginMainView();
        dev.phases().push(scene::View::Main, scene::Pass::Solid);
        dev.setRenderTarget(0, dev.backBuffer());
        dev.setVertexShader(vs);
        dev.setPixelShader(ps);
        dev.drawTag() = {};
        dev.drawTag().source = draws::Source::Mesh;
        dev.drawTag().opDesc = 0xffffff00u;  // (an instanced batch's render-op node: Kind::InstancedMesh)
        dev.drawTag().entity = entity;
        OwnedWindows ow;  // (window 2 uploaded: no object record)
        dev.setOwnedWindows(&ow);
        dev.setStreamSource(0, vb.get(), 0, sizeof(Vertex));
        dev.setIndices(ib.get());
        const auto place = [&](float tx, float ty, const void* id, std::uint32_t color) {
            const float c31[4] = {0, 0, 0, 0}, c32[4] = {0, 0, 0, 1};
            const float rows[3][4] = {{1, 0, 0, tx}, {0, 1, 0, ty}, {0, 0, 1, 0}};
            dev.setVertexShaderConstantF(31, c31, 1);
            dev.setVertexShaderConstantF(32, c32, 1);
            dev.setVertexShaderConstantF(33, rows[0], 3);
            (void)color;
            dev.setInstanceIdentity(identities ? id : nullptr);
            dev.setSceneIssue(sceneForm);
            dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, 4, 0, 2);
            dev.setSceneIssue(false);
        };
        // (the list re-sorted every frame: B first on odd frames; with identities the order does not matter)
        const float atx = ax + ((k & 1) != 0 ? 0.2f : 0.0f);
        if ((k & 1) != 0) {
            place(bx, by, &instB, 0);
            place(atx, ay, &instA, 0);
        } else {
            place(atx, ay, &instA, 0);
            place(bx, by, &instB, 0);
        }
        dev.setInstanceIdentity(nullptr);
        dev.setOwnedWindows(nullptr);
        dev.drawTag() = {};
        dev.phases().pop();
        Pixels p;
        if (showMotion) {
            dev.setRenderTarget(0, dev.backBuffer());
            dev.setDepthStencil(nullptr);
            dev.setVertexShader(f.vs);
            dev.setPixelShader(show);
            dev.setTexture(0, motionTex.get());
            dev.setSamplerState(0, SAMP_MAGFILTER, TEXF_POINT);
            dev.setSamplerState(0, SAMP_MINFILTER, TEXF_POINT);
            dev.setVertexDecl(f.decl.get());
            f.draw(quad(-1, -1, 1, 1, 0.5f, 0xFFFFFFFF));
            dev.setTexture(0, nullptr);
            p = capture(dev, sceneForm ? "listmotion_scene" : identities ? "listmotion_id" : "listmotion_noid");
        }
        dev.boneMotionFrame();
        dev.present();
        return p;
    };
    // the pixel of a placement's centre (D3D9 clip space y up -> rows from the top)
    const auto px = [&](float x, float y) { return std::pair<std::uint32_t, std::uint32_t>{static_cast<std::uint32_t>((x * 0.5f + 0.5f) * bw), static_cast<std::uint32_t>((0.5f - y * 0.5f) * bh)}; };
    const auto red = [](std::uint32_t c) { return static_cast<int>((c >> 16) & 0xFF); };
    const auto green = [](std::uint32_t c) { return static_cast<int>((c >> 8) & 0xFF); };
    const auto run = [&](bool identities, std::uint64_t& moved, Pixels& last) {
        const Device::BoneMotionStats s0 = dev.boneStats;
        // (the variants build in the background and the G-buffer arms after a frame that found its pipelines: up to ~10 s)
        // (the scene form: until both placements of a frame were scene draws, i.e. the scene bone pipelines exist; a draw whose pipeline is pending falls back)
        // (bounded by time, not by frame count: a frame is ~0.1 ms while the forms are pending, so 1200 frames were ~7 s, and the DXC compiles of the variant forms take
        // longer than that on a starved CPU (another game, parallel test runs): the case then ran its reads before the forms existed: all pixels 0.5 = no motion)
        int sceneFrames = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
        for (int k = 0; std::chrono::steady_clock::now() < deadline; ++k) {
            const std::uint64_t sd0 = dev.sceneStats().sceneDraws;
            frame(k, identities, false);
            sceneFrames = dev.sceneStats().sceneDraws - sd0 == 2 ? sceneFrames + 1 : 0;
            if (dev.gbufferArmed() && (!identities || dev.boneStats.listMoving - s0.listMoving >= 8) && k >= 8 && (!sceneForm || sceneFrames >= 4)) break;
            if (k > 4) Sleep(5);
        }
        frame(0, identities, false);  // (A at ax: the previous frame of the odd one)
        // an odd frame: A at ax + 0.2 now, ax last frame (moved +0.2: previous - current = -0.2 in x = -0.1 in uv: red 0.5 - 0.4 = 0.1); then an even frame (+0.1 in uv)
        last = frame(1, identities, true);
        moved = dev.boneStats.listMoving - s0.listMoving;
        frame(1, identities, false);  // (a read-back submits the frame: the frame value moves twice, so the next frame finds no rows of "the last frame" and draws A without motion)
        return frame(2, identities, true);
    };
    std::uint64_t movedId = 0, movedNo = 0;
    Pixels oddId, oddNo;
    const Pixels evenId = run(true, movedId, oddId);
    const Device::BoneMotionStats sMid = dev.boneStats;
    const Pixels evenNo = run(false, movedNo, oddNo);
    // the scene form: the same with scene draws on (setSceneInstances: the instance-list draw takes the scene bone forms, its record's prevVs = the block)
    std::uint64_t movedSc = 0, sceneDrawsSc = 0, predictedSc = 0;
    Pixels oddSc, evenSc;
    dev.setSceneDraws(true, 0);
    const bool sceneOk = dev.sceneDrawsActive();
    if (sceneOk) {
        dev.setSceneInstances(true);
        dev.setSceneViews(true);  // (an instance-list draw is a scene candidate only with the v2 ABI on; without a view table its scene form is v1)
        sceneForm = true;
        const auto sc0 = dev.sceneStats();
        const std::uint64_t pr0 = dev.boneStats.rigidPredicted;
        evenSc = run(true, movedSc, oddSc);
        sceneDrawsSc = dev.sceneStats().sceneDraws - sc0.sceneDraws;
        predictedSc = dev.boneStats.rigidPredicted - pr0;
        sceneForm = false;
        dev.setSceneInstances(false);
        dev.setSceneViews(false);
    }
    dev.setSceneDraws(false, 0);
    const auto [aOx, aOy] = px(ax + 0.2f, ay);
    const auto [aEx, aEy] = px(ax, ay);
    const auto [bX, bY] = px(bx, by);
    std::printf("  list motion: armed %d, moved forms with identities %llu (by identity %llu) / without %llu; A odd r %d g %d, A even r %d g %d, B r %d g %d; control A odd r %d, even r %d\n",
                dev.gbufferArmed() ? 1 : 0, static_cast<unsigned long long>(movedId), static_cast<unsigned long long>(sMid.instMoving), static_cast<unsigned long long>(movedNo),
                red(oddId.at(aOx, aOy)), green(oddId.at(aOx, aOy)), red(evenId.at(aEx, aEy)), green(evenId.at(aEx, aEy)), red(evenId.at(bX, bY)), green(evenId.at(bX, bY)),
                red(oddNo.at(aOx, aOy)), red(evenNo.at(aEx, aEy)));
    const auto within = [](int v, int want, int tol) { return std::abs(v - want) <= tol; };
    const bool okId = movedId >= 2 && sMid.instMoving >= 2 && within(red(oddId.at(aOx, aOy)), 26, 10) && within(red(evenId.at(aEx, aEy)), 230, 10) &&
                      within(green(oddId.at(aOx, aOy)), 128, 6) && within(red(evenId.at(bX, bY)), 128, 6) && within(green(evenId.at(bX, bY)), 128, 6);
    const bool okNo = within(red(oddNo.at(aOx, aOy)), 128, 6) && within(red(evenNo.at(aEx, aEy)), 128, 6);
    if (!dev.gbufferArmed() || !okId) {
        std::printf("  FAIL list motion: the moving instance of a two-instance list has no motion of its own (or the still one moves)\n");
        ++g_failures;
    }
    if (!okNo) {
        std::printf("  FAIL list motion: the negative control (no identities: an ambiguous list) shows motion\n");
        ++g_failures;
    }
    if (!sceneOk) {
        std::printf("  (list motion: scene draws not available here: the scene form skipped)\n");
    } else {
        std::printf("  list motion, scene form: %llu scene draws, %llu predicted with the scene bone forms; A odd r %d, A even r %d, B r %d\n", static_cast<unsigned long long>(sceneDrawsSc),
                    static_cast<unsigned long long>(predictedSc), red(oddSc.at(aOx, aOy)), red(evenSc.at(aEx, aEy)), red(evenSc.at(bX, bY)));
        if (sceneDrawsSc == 0 || predictedSc < 2 || !within(red(oddSc.at(aOx, aOy)), 26, 10) || !within(red(evenSc.at(aEx, aEy)), 230, 10) || !within(red(evenSc.at(bX, bY)), 128, 6)) {
            std::printf("  FAIL list motion: the scene form of the moving instance has no motion of its own\n");
            ++g_failures;
        }
    }
    // a soak of both forms (GPU-assisted validation runs: hundreds of moved draws through the previous rows block)
    {
        const std::uint64_t m0 = dev.boneStats.listMoving;
        for (int k = 0; k < 300; ++k) frame(k, true, false);
        std::uint64_t mScene = 0;
        dev.setSceneDraws(true, 0);
        if (dev.sceneDrawsActive()) {
            dev.setSceneInstances(true);
            dev.setSceneViews(true);
            sceneForm = true;
            const std::uint64_t s1 = dev.boneStats.listMoving;
            for (int k = 0; k < 300; ++k) frame(k, true, false);
            mScene = dev.boneStats.listMoving - s1;
            sceneForm = false;
            dev.setSceneInstances(false);
            dev.setSceneViews(false);
        }
        dev.setSceneDraws(false, 0);
        const std::uint64_t mAll = dev.boneStats.listMoving - m0;
        std::printf("  list motion soak: %llu moved draws (scene form %llu)\n", static_cast<unsigned long long>(mAll), static_cast<unsigned long long>(mScene));
        const std::uint64_t want = mScene > 0 ? 580u : 290u;  // (300 frames per form, A moves in every one)
        if (mAll < want) {
            std::printf("  FAIL list motion: the soak drew %llu moved forms (expected at least %llu)\n", static_cast<unsigned long long>(mAll), static_cast<unsigned long long>(want));
            ++g_failures;
        }
    }
    dev.setGbuffer({});
    dev.tagImage(hdr, scene::ImageRole::None);
    dev.setBoneMotion(0);
    dev.setRigidMotion(false);
    dev.setRigidMotionWanted(false);
    dev.setVariantProvider(VariantSlot::GbufferM, {});
    dev.setVariantProvider(VariantSlot::GbufferMotionM, {});
    ps->semantics.flags = psFlagsBefore;
    dev.release(std::move(vb));
    dev.release(std::move(ib));
    dev.release(std::move(motionTex));
}

}  // namespace

// --present-test [vsync 0/1]: a real window, clears + presents 300 frames, prints the frame rate (vsync pacing check)
int presentTest(bool vsync) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"rvk_present_test";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"rvk present test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 400,
                                nullptr, nullptr, wc.hInstance, nullptr);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    Device dev;
    DeviceOptions o;
    o.backend = g_backend;
    o.window = hwnd;
    o.width = static_cast<std::uint32_t>(rc.right);
    o.height = static_cast<std::uint32_t>(rc.bottom);
    o.vsync = vsync;
    if (!dev.create(o)) return 2;
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kFrames = 300;
    for (int i = 0; i < kFrames; ++i) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        dev.clear(CLEAR_TARGET, 0xFF000000u | static_cast<std::uint32_t>(i * 0x010101), 1.0f, 0);
        dev.present();
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("vsync %d: %d frames in %.2f s = %.1f fps (acquire failures %llu)\n", vsync ? 1 : 0, kFrames, secs, kFrames / secs,
                static_cast<unsigned long long>(dev.stats().acquireFailures));
    dev.destroy();
    DestroyWindow(hwnd);
    return 0;
}

// 14.13 bone-history motion (native.bone_motion) through the device's own path: the bone table's history (the previous frame's image at the history delta, c231.x = 1 valid;
// invalid after a gap / a tag change / a root jump; a still part writes nothing) and the compare mode's capture vertex shader: a skinned quad drawn with the capture variant (the
// RT scene's cap6 + the compare text) whose VS window 2 is the table's image; frame by frame the image alternates A / B, the capture chain's previous position (the previous
// frame's capture) and the bone path's (the translated program run again on the history image) must agree EXACTLY; a negative control (the capture chain pointed at the frame
// before the previous one) must show the difference. Vulkan only (the compare buffer is reached by address); the history part runs on both backends
struct BoneCaptureObserver final : DrawObserver {
    bool want = false;
    rhi::RingAlloc cur, prev;  // this frame's capture, the one the header names as the previous capture
    std::uint32_t bit = 0;
    bool observes(draws::Kind, scene::Phase phase) const override { return phase.view == scene::View::Main && phase.pass == scene::Pass::Solid; }
    Registers registersRead() const override { return Registers::Material; }
    void observe(Device& d, const ReplaceQuery&) override {
        if (!want || cur.ptr == nullptr) return;
        bit = d.boneCompareBit();
        std::uint32_t header[4] = {4u | bit, 0, 0, 0};
        if (prev.ptr != nullptr) {
            header[1] = prev.ref.low();
            header[2] = prev.ref.high();
            header[3] = 4;
        }
        std::memcpy(cur.ptr, header, 16);
        d.requestVertexCapture(cur);
    }
};

void testBoneMotion(Fixture& f) {
    Device& dev = f.dev;
    gpuscene::BoneTable& bt = gpuscene::bonetable();
    bt.destroy();
    bt.setHistory(true);
    bt.setActive(true);
    for (int i = 0; i < 3 && !bt.ready(); ++i) bt.flush(dev);
    if (!bt.ready() || !bt.history()) {
        std::printf("  FAIL bone motion: the bone table with history was not created\n");
        ++g_failures;
        bt.setActive(false);
        return;
    }
    static float regsA[gpuscene::kBoneRegs][4], regsB[gpuscene::kBoneRegs][4], regsJ[gpuscene::kBoneRegs][4];
    const auto image = [](float (*regs)[4], float sx, float tx) {
        std::memset(regs, 0, sizeof(float) * 4 * gpuscene::kBoneRegs);
        regs[1][3] = 1.0f;  // (c32.w: the fade)
        const float rows[3][4] = {{sx, 0, 0, tx}, {0, sx, 0, 0}, {0, 0, 1, 0}};
        std::memcpy(regs[2], rows, sizeof(rows));
    };
    image(regsA, 0.5f, 0.0f);
    image(regsB, 0.25f, 0.1f);
    image(regsJ, 0.25f, 900.0f);  // (a root jump of 900 units: a teleport)
    int wrapper = 0;
    const gpuscene::BoneKey key{&wrapper, 0, 0x3f800000u};
    const auto prevValid = [&] {
        const gpuscene::BoneRecord* p = bt.prevImage(key);
        return p != nullptr && p->regs[gpuscene::kHistoryFlag][0] == 1.0f;
    };
    const auto prevEquals = [&](const float (*regs)[4]) {
        const gpuscene::BoneRecord* p = bt.prevImage(key);
        return p != nullptr && std::memcmp(p->regs[2], regs[2], 3 * 16) == 0;
    };
    // ---- the history (CPU twin; the GPU read-back of the whole table, history included, runs every 240th flush)
    {
        d3d9::OwnedWindows::Win w;
        bool ok = bt.serve(key, regsA, 5, w, 7) == gpuscene::BoneWhy::Served && !prevValid();  // new slot: no history
        bt.flush(dev);
        const rhi::GpuRef base = w.base;
        ok = ok && !bt.prevOf(base).null() && bt.prevOf(base.plus(16)).null() && bt.prevOf(rhi::GpuRef{}).null();
        ok = ok && bt.serve(key, regsB, 5, w, 7) == gpuscene::BoneWhy::Served && prevValid() && prevEquals(regsA);  // served last frame: prev = A
        bt.flush(dev);
        const gpuscene::BoneStats s0 = bt.stats();
        ok = ok && bt.serve(key, regsB, 5, w, 7) == gpuscene::BoneWhy::Served && prevValid() && prevEquals(regsB);  // B again: prev = B (copied once)
        bt.flush(dev);
        const std::uint64_t slots0 = bt.tableStats().slotsUploaded;
        ok = ok && bt.serve(key, regsB, 5, w, 7) == gpuscene::BoneWhy::Served && prevValid() && prevEquals(regsB) && bt.stats().histKept == s0.histKept + 1;  // still: nothing written
        bt.flush(dev);
        ok = ok && bt.tableStats().slotsUploaded == slots0;
        ok = ok && bt.serve(key, regsA, 5, w, 8) == gpuscene::BoneWhy::Served && !prevValid() && bt.stats().histTag == s0.histTag + 1;  // tag moved: invalid
        bt.flush(dev);
        bt.flush(dev);  // (a frame without a serve: a gap)
        ok = ok && bt.serve(key, regsA, 5, w, 8) == gpuscene::BoneWhy::Served && !prevValid() && bt.stats().histGap > s0.histGap;
        bt.flush(dev);
        ok = ok && bt.serve(key, regsJ, 5, w, 8) == gpuscene::BoneWhy::Served && !prevValid() && bt.stats().histJump == s0.histJump + 1;  // teleport: invalid
        bt.flush(dev);
        ok = ok && bt.serve(key, regsJ, 5, w, 8) == gpuscene::BoneWhy::Served && prevValid() && prevEquals(regsJ);
        bt.flush(dev);
        if (!ok) {
            const gpuscene::BoneStats& s = bt.stats();
            std::printf("  FAIL bone motion: history (copied %llu kept %llu gap %llu tag %llu jump %llu)\n", static_cast<unsigned long long>(s.histCopied), static_cast<unsigned long long>(s.histKept),
                        static_cast<unsigned long long>(s.histGap), static_cast<unsigned long long>(s.histTag), static_cast<unsigned long long>(s.histJump));
            ++g_failures;
        }
    }
    if (g_backend != rhi::Backend::Vulkan || !dev.game().gameCaps().vertexStores) {
        std::printf("  (bone motion: the compare part is Vulkan only / needs vertex stores: skipped)\n");
        bt.destroy();
        bt.setActive(false);
        bt.setHistory(false);
        return;
    }
    // ---- the compare mode's capture vertex shader (the palette read relative, as the game's skinned shaders do: the bone forms' validity word follows the shader's
    // addressing, 14.15 b)
    static const char* kVsBone = R"(
vs_3_0
def c5, 0, 0, 0, 0
dcl_position v0
dcl_color v1
dcl_texcoord v2
dcl_position o0
dcl_color o1
dcl_texcoord o2
mova a0.x, c5.x
dp4 o0.x, v0, c33[a0.x]
dp4 o0.y, v0, c34[a0.x]
dp4 o0.z, v0, c35[a0.x]
mov o0.w, v0.w
mov o1, v1
mov o2, v2
)";
    const auto vsBone = shaderFrom(dev, kVsBone);
    if (!vsBone) {
        std::printf("  FAIL bone motion: shader\n");
        ++g_failures;
        return;
    }
    dev.setBoneMotion(2);
    dev.setVariantProvider(VariantSlot::Capture, {Device::captureTag(), enh::RtScene::captureOptions});
    BoneCaptureObserver obs;
    dev.addDrawObserver(&obs);
    const Vertex verts[4] = {{-0.5f, 0.5f, 0.5f, 1, 0xFFFFFFFF, 0, 0}, {0.5f, 0.5f, 0.5f, 1, 0xFFFFFFFF, 1, 0}, {-0.5f, -0.5f, 0.5f, 1, 0xFFFFFFFF, 0, 1}, {0.5f, -0.5f, 0.5f, 1, 0xFFFFFFFF, 1, 1}};
    const std::uint16_t idx[6] = {0, 1, 2, 1, 3, 2};
    auto vb = dev.createBuffer(sizeof(verts), USAGE_WRITEONLY, POOL_MANAGED, false, false);
    auto ib = dev.createBuffer(sizeof(idx), USAGE_WRITEONLY, POOL_MANAGED, true, false);
    std::memcpy(vb->lock(0, 0, 0), verts, sizeof(verts));
    vb->unlock();
    std::memcpy(ib->lock(0, 0, 0), idx, sizeof(idx));
    ib->unlock();
    scene::CameraInput cam;
    cam.view = cam.projection = cam.viewProjection = cam.inverseView = scene::Mat4::identity();
    cam.engineFar = 1000.0f;
    rhi::RingAlloc caps[3];  // the captures of the last three frames
    const auto frame = [&](int k, bool stale) {
        f.reset();
        dev.setMainCamera(cam);
        dev.setVertexShader(vsBone);
        const float junk[4] = {0.9f, 0.9f, 0.9f, 0.9f};
        for (std::uint32_t r = 31; r < 36; ++r) dev.setVertexShaderConstantF(r, junk, 1);
        d3d9::OwnedWindows::Win w;
        bt.serve(key, (k & 1) == 0 ? regsA : regsB, 5, w, 1);
        OwnedWindows ow;
        ow.w[0][2] = w;
        dev.setOwnedWindows(&ow);
        caps[2] = caps[1];
        caps[1] = caps[0];
        caps[0] = dev.game().allocCapture(16 + 4 * 32, 16);
        if (caps[0].ptr != nullptr) std::memset(caps[0].ptr, 0, 16 + 4 * 32);
        obs.want = true;
        obs.cur = caps[0];
        obs.prev = stale ? caps[2] : caps[1];
        dev.drawTag() = {};
        dev.phases().push(scene::View::Main, scene::Pass::Solid);
        dev.setStreamSource(0, vb.get(), 0, sizeof(Vertex));
        dev.setIndices(ib.get());
        dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, 4, 0, 2);
        dev.phases().pop();
        dev.setOwnedWindows(nullptr);
        bt.flush(dev);
        dev.boneMotionFrame();
        dev.present();
    };
    const auto run = [&](bool stale, int frames) {
        const d3d9::Device::BoneMotionStats s0 = dev.boneStats;
        dev.boneStats.maxPx = dev.boneStats.maxWorld = 0.0f;
        for (int k = 0; k < frames; ++k) {
            frame(k, stale);
            if (dev.boneStats.compared - s0.compared >= 40) break;
            if (k > 8 && dev.boneStats.compared == s0.compared) Sleep(5);  // (the capture variant compiles in the background)
        }
        for (int k = 0; k < 6; ++k) frame(k, stale);  // (the compare slots are read kFramesInFlight + 2 frames later)
        d3d9::Device::BoneMotionStats d = dev.boneStats;
        d.compared -= s0.compared;
        d.compareMiss -= s0.compareMiss;
        d.compareOver -= s0.compareOver;
        return d;
    };
    const d3d9::Device::BoneMotionStats good = run(false, 600);
    std::printf("  bone motion compare: %llu vertices compared, max %.6f px / %.6f units, %llu above 0.01 px, %llu without a previous capture\n", static_cast<unsigned long long>(good.compared),
                good.maxPx, good.maxWorld, static_cast<unsigned long long>(good.compareOver), static_cast<unsigned long long>(good.compareMiss));
    if (good.compared == 0 || good.maxWorld != 0.0f || good.maxPx != 0.0f || obs.bit == 0) {
        std::printf("  FAIL bone motion: the capture chain and the bone path disagree (or nothing was compared; header bit %08x)\n", obs.bit);
        ++g_failures;
    }
    const d3d9::Device::BoneMotionStats bad = run(true, 60);
    std::printf("  bone motion compare, negative control (the capture of two frames ago): %llu compared, max %.6f units\n", static_cast<unsigned long long>(bad.compared), bad.maxWorld);
    if (bad.compared == 0 || bad.maxWorld < 0.01f) {
        std::printf("  FAIL bone motion: the negative control shows no difference\n");
        ++g_failures;
    }
    obs.want = false;
    dev.removeDrawObserver(&obs);
    dev.setBoneMotion(0);
    dev.setVariantProvider(VariantSlot::Capture, {});
    dev.release(std::move(vb));
    dev.release(std::move(ib));
    bt.destroy();
    bt.setActive(false);
    bt.setHistory(false);
}

// 14.15 b: rigid-object history motion. The object table's history records (CPU twin: changed slot = last frame's record, valid; still = the record itself; a new
// record / a jump = invalid; prevOf guards), then the compare mode's capture vertex shader on a rigid shader (no relative addressing: the object record's validity
// word c36.w and the object table's history delta) against the capture chain, identity camera: 0 difference; negative control: the capture of two frames ago.
void testRigidMotion(Fixture& f) {
    Device& dev = f.dev;
    gpuscene::ObjectTable& ot = gpuscene::objecttable();
    ot.setHistory(true);
    ot.setActive(true);
    for (int i = 0; i < 3 && !ot.ready(); ++i) ot.flush(dev);
    if (!ot.ready() || !ot.history()) {
        std::printf("  FAIL rigid motion: the object table with history was not created\n");
        ++g_failures;
        ot.setActive(false);
        return;
    }
    alignas(16) static unsigned char entity[0x100];
    std::memset(entity, 0, sizeof(entity));
    const auto matrix = [](float tx, float s, float m[16]) {
        const float r[16] = {s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1, 0, tx, 0, 0, 1};  // (row-vector convention: the translation in the last row)
        std::memcpy(m, r, sizeof(r));
    };
    float mA[16], mB[16], mJ[16];
    matrix(0.0f, 0.5f, mA);
    matrix(0.1f, 0.25f, mB);
    matrix(900.0f, 0.25f, mJ);
    std::memcpy(entity + 0x60, mA, sizeof(mA));
    engine::scenetable::testCacheIn(entity);
    const std::uint32_t slot = engine::scenetable::find(entity);
    const auto serve = [&](const float m[16], d3d9::OwnedWindows::Win& w) {
        float composed[gpuscene::kObjectRegs][4] = {};
        gpuscene::objectWorldRows(m, reinterpret_cast<float(*)[4]>(composed[2]));
        return ot.serve(entity, composed, 0x1Cu, w) == gpuscene::ObjectWhy::Served;
    };
    const auto histRows = [&](const float m[16]) {
        const gpuscene::ObjectRecord* h = ot.historyRecord(slot);
        float rows[3][4];
        gpuscene::objectWorldRows(m, rows);
        return h != nullptr && std::memcmp(h->regs[2], rows, sizeof(rows)) == 0;
    };
    const auto histValid = [&] {
        const gpuscene::ObjectRecord* h = ot.historyRecord(slot);
        return h != nullptr && h->info[3] == 1u;
    };
    {
        d3d9::OwnedWindows::Win w;
        bool ok = slot != UINT32_MAX && serve(mA, w);
        ot.flush(dev);
        ok = ok && !histValid();  // a new record: no history
        const rhi::GpuRef base = w.base;
        ok = ok && !ot.prevOf(base).null() && ot.prevOf(base.plus(16)).null() && ot.prevOf(rhi::GpuRef{}).null();
        ok = ok && serve(mA, w);
        ot.flush(dev);
        ok = ok && !histValid() && histRows(mA);  // still: history = the record, marked invalid (the camera's motion)
        engine::scenetable::testMove(entity, mB);
        ok = ok && serve(mB, w);
        ot.flush(dev);
        ok = ok && histValid() && histRows(mA);  // moved: history = last frame's record
        ok = ok && serve(mB, w);
        ot.flush(dev);
        ok = ok && !histValid() && histRows(mB);  // settled: invalid again
        const std::uint64_t jumps = ot.historyStats().invalidJump;
        engine::scenetable::testMove(entity, mJ);
        ok = ok && serve(mJ, w);
        ot.flush(dev);
        ok = ok && !histValid() && ot.historyStats().invalidJump == jumps + 1;  // a jump: invalid
        ok = ok && serve(mJ, w);
        ot.flush(dev);
        ok = ok && !histValid() && histRows(mJ);
        if (!ok) {
            const auto& h = ot.historyStats();
            std::printf("  FAIL rigid motion: history (slot %u, changed %llu valid %llu new %llu jump %llu settled %llu)\n", slot, static_cast<unsigned long long>(h.changed),
                        static_cast<unsigned long long>(h.valid), static_cast<unsigned long long>(h.invalidNew), static_cast<unsigned long long>(h.invalidJump),
                        static_cast<unsigned long long>(h.settled));
            ++g_failures;
        }
    }
    if (g_backend != rhi::Backend::Vulkan || !dev.game().gameCaps().vertexStores) {
        std::printf("  (rigid motion: the compare part is Vulkan only / needs vertex stores: skipped)\n");
        engine::scenetable::testCacheOut(entity);
        ot.setActive(false);
        return;
    }
    static const char* kVsRigid = R"(
vs_3_0
dcl_position v0
dcl_color v1
dcl_texcoord v2
dcl_position o0
dcl_color o1
dcl_texcoord o2
dp4 o0.x, v0, c33
dp4 o0.y, v0, c34
dp4 o0.z, v0, c35
mov o0.w, v0.w
mov o1, v1
mov o2, v2
)";
    const auto vs = shaderFrom(dev, kVsRigid);
    if (!vs) {
        std::printf("  FAIL rigid motion: shader\n");
        ++g_failures;
        return;
    }
    dev.setBoneMotion(2);
    dev.setRigidMotion(true);
    dev.setVariantProvider(VariantSlot::Capture, {Device::captureTag(), enh::RtScene::captureOptions});
    BoneCaptureObserver obs;
    dev.addDrawObserver(&obs);
    const Vertex verts[4] = {{-0.5f, 0.5f, 0.5f, 1, 0xFFFFFFFF, 0, 0}, {0.5f, 0.5f, 0.5f, 1, 0xFFFFFFFF, 1, 0}, {-0.5f, -0.5f, 0.5f, 1, 0xFFFFFFFF, 0, 1}, {0.5f, -0.5f, 0.5f, 1, 0xFFFFFFFF, 1, 1}};
    const std::uint16_t idx[6] = {0, 1, 2, 1, 3, 2};
    auto vb = dev.createBuffer(sizeof(verts), USAGE_WRITEONLY, POOL_MANAGED, false, false);
    auto ib = dev.createBuffer(sizeof(idx), USAGE_WRITEONLY, POOL_MANAGED, true, false);
    std::memcpy(vb->lock(0, 0, 0), verts, sizeof(verts));
    vb->unlock();
    std::memcpy(ib->lock(0, 0, 0), idx, sizeof(idx));
    ib->unlock();
    scene::CameraInput cam;
    cam.view = cam.projection = cam.viewProjection = cam.inverseView = scene::Mat4::identity();
    cam.engineFar = 1000.0f;
    rhi::RingAlloc caps[3];
    const auto frame = [&](int k, bool stale) {
        f.reset();
        dev.setMainCamera(cam);
        dev.setVertexShader(vs);
        const float junk[4] = {0.9f, 0.9f, 0.9f, 0.9f};
        for (std::uint32_t r = 31; r < 36; ++r) dev.setVertexShaderConstantF(r, junk, 1);
        const float* m = (k & 1) == 0 ? mA : mB;
        engine::scenetable::testMove(entity, m);
        d3d9::OwnedWindows::Win w;
        serve(m, w);
        OwnedWindows ow;
        ow.w[0][2] = w;
        dev.setOwnedWindows(&ow);
        caps[2] = caps[1];
        caps[1] = caps[0];
        caps[0] = dev.game().allocCapture(16 + 4 * 32, 16);
        if (caps[0].ptr != nullptr) std::memset(caps[0].ptr, 0, 16 + 4 * 32);
        obs.want = true;
        obs.cur = caps[0];
        obs.prev = stale ? caps[2] : caps[1];
        dev.drawTag() = {};
        dev.phases().push(scene::View::Main, scene::Pass::Solid);
        dev.setStreamSource(0, vb.get(), 0, sizeof(Vertex));
        dev.setIndices(ib.get());
        dev.drawIndexedPrimitive(PT_TRIANGLELIST, 0, 0, 4, 0, 2);
        dev.phases().pop();
        dev.setOwnedWindows(nullptr);
        ot.flush(dev);
        dev.boneMotionFrame();
        dev.present();
    };
    const auto run = [&](bool stale, int frames) {
        // (the compare slots are read kFramesInFlight + 2 frames late: the previous case's last results drain first)
        for (int k = 0; k < 8; ++k) frame(k, stale);
        const d3d9::Device::BoneMotionStats s0 = dev.boneStats;
        dev.boneStats.maxPx = dev.boneStats.maxWorld = 0.0f;
        for (int k = 0; k < frames; ++k) {
            frame(k, stale);
            if (dev.boneStats.compared - s0.compared >= 40) break;
            if (k > 8 && dev.boneStats.compared == s0.compared) Sleep(5);
        }
        for (int k = 0; k < 6; ++k) frame(k, stale);
        d3d9::Device::BoneMotionStats d = dev.boneStats;
        d.compared -= s0.compared;
        return d;
    };
    const d3d9::Device::BoneMotionStats good = run(false, 600);
    std::printf("  rigid motion compare: %llu vertices compared, max %.6f px / %.6f units\n", static_cast<unsigned long long>(good.compared), good.maxPx, good.maxWorld);
    if (good.compared == 0 || good.maxWorld != 0.0f || good.maxPx != 0.0f || obs.bit == 0) {
        std::printf("  FAIL rigid motion: the capture chain and the object history disagree (or nothing was compared; header bit %08x)\n", obs.bit);
        ++g_failures;
    }
    const d3d9::Device::BoneMotionStats bad = run(true, 60);
    std::printf("  rigid motion compare, negative control: %llu compared, max %.6f units\n", static_cast<unsigned long long>(bad.compared), bad.maxWorld);
    if (bad.compared == 0 || bad.maxWorld < 0.01f) {
        std::printf("  FAIL rigid motion: the negative control shows no difference\n");
        ++g_failures;
    }
    obs.want = false;
    dev.removeDrawObserver(&obs);
    dev.setBoneMotion(0);
    dev.setRigidMotion(false);
    dev.setVariantProvider(VariantSlot::Capture, {});
    dev.release(std::move(vb));
    dev.release(std::move(ib));
    engine::scenetable::testCacheOut(entity);
    ot.setActive(false);
}

// The backend's teardown with game-mode resources alive: vertex capture chunks in both frame slots (D3D12: D3DBuffers in the capture pool) and the GPU
// timestamp read-back. Nothing is checked here; main() destroys the device after the last case with these still allocated and counts the errors logged
// during it (D3D12 crashed at the game's exit: the capture pool's buffers outlived the queue / fence / heap allocator, ~D3D12DeviceImpl's member order).
void testTeardownWithCaptures(Fixture& f) {
    Device& dev = f.dev;
    const bool ts = dev.game().enableTimestamps();
    int got = 0;
    for (int k = 0; k < 4; ++k) {
        f.reset();
        for (int i = 0; i < 3; ++i) {
            const rhi::RingAlloc a = dev.game().allocCapture((6ull << 20) + 256 * i, 16);  // (several chunks per slot)
            if (!a.ref.null() || a.gpuVa != 0) ++got;
        }
        dev.present();
    }
    std::printf("  teardown with captures: %d capture allocations alive (timestamps %s); the device teardown at the end checks them\n", got, ts ? "on" : "unavailable");
    if (got == 0) std::printf("  (this backend has no capture pool: nothing to tear down)\n");
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool validation = true, syncValidation = false;
    std::vector<std::string> only;  // --only <name part>[,<name part>...]: run the matching cases only (bisecting)
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--only" && i + 1 < argc) {
            const std::string list = argv[++i];
            for (std::size_t p = 0; p <= list.size();) {
                const std::size_t e = std::min(list.find(',', p), list.size());
                if (e > p) only.push_back(list.substr(p, e - p));
                p = e + 1;
            }
        }
        if (a == "--no-validation") validation = false;
        if (a == "--validation") validation = true;
        if (a == "--sync-validation") validation = syncValidation = true;
        if (a == "--backend" && i + 1 < argc) {
            const std::string b = argv[++i];
            if (b == "vulkan") g_backend = rhi::Backend::Vulkan;
            else if (b == "d3d12") g_backend = rhi::Backend::D3D12;
            else {
                std::printf("unknown backend '%s' (vulkan | d3d12)\n", b.c_str());
                return 2;
            }
        }
        if (a == "--keep-images") g_keepImages = true;
        if (a == "--present-test") return presentTest(i + 1 >= argc || std::string(argv[i + 1]) != "0");
    }
    if (!devtools::D3DX::get().available()) {
        std::printf("d3dx9_39.dll not available\n");
        return 2;
    }
    Device dev;
    DeviceOptions o;
    o.backend = g_backend;
    o.width = o.height = kSize;
    o.depthFormat = rhi::d3dfmt::D24S8;
    o.validation = validation;
    o.syncValidation = syncValidation;
    o.offscreen = true;
    if (!dev.create(o)) {
        return 2;
    }
    Fixture f{dev};
    f.vs = shaderFrom(dev, kVs);
    f.vsInst = shaderFrom(dev, kVsInstanced);
    f.psColor = shaderFrom(dev, kPsColor);
    f.psTexture = shaderFrom(dev, kPsTexture);
    f.decl = dev.createVertexDecl(kDecl);
    f.instDecl = dev.createVertexDecl(kInstDecl);
    f.vb = dev.createBuffer(64 * sizeof(Vertex), USAGE_DYNAMIC | USAGE_WRITEONLY, POOL_DEFAULT, false, false);
    if (!f.vs || !f.vsInst || !f.psColor || !f.psTexture) {
        return 2;
    }
    const std::pair<const char*, std::function<void(Fixture&)>> cases[] = {
        {"clear", testClear},         {"raster rules", testRasterRules}, {"cull", testCull},
        {"depth", testDepth},         {"blend", testBlend},              {"alpha test", testAlphaTest},
        {"stencil", testStencil}, {"scope clear", testScopeClear}, {"no-stencil depth", testNoStencilDepth}, {"depth-only pass without a pixel shader (S-6)", testDepthOnlyPass},     {"render to texture", testRenderToTexture}, {"dxt1", testDxt1},
        {"instancing", testInstancing}, {"retained instance stream (F7-1)", testRetainedInstanceStream}, {"own particles (F11)", testOwnParticles}, {"UP + scissor", testUpAndScissor}, {"stretch rect", testStretchRect},
        {"mip chain stretch rect", testMipChainStretchRect}, {"dynamic buffer", testDynamicBuffer}, {"dynamic buffer across frames", testDynamicBufferAcrossFrames},
        {"partial texture lock", testPartialLock}, {"lock after upload", testLockAfterUpload},
        {"read-only lock of a fresh texture", testReadOnlyLockOfFreshTexture},
        {"scene draws (indirect)", testSceneDraws}, {"owned replay (records)", testOwnedReplay}, {"owned replay v2 (view table)", testOwnedReplayV2}, {"owned persistent (F2c-4a)", testOwnedPersistent}, {"owned pull (S-4)", testOwnedPull}, {"clusters (F5-1)", testClusters}, {"cluster cull (F5-2)", testClusterCull}, {"owned trees (F4-3)", testOwnedTrees}, {"owned vegetation (F7-2)", testOwnedVegetation}, {"owned windows (VS 1 / 2)", testOwnedWindows}, {"bone table (F6-1)", testBoneTable}, {"instance rows (F2f)", testInstanceRows}, {"bone motion (14.13)", testBoneMotion}, {"rigid motion (14.15 b)", testRigidMotion}, {"list motion (14.26)", testListMotion}, {"owned submit (F4-1)", testOwnedSubmit}, {"fvf vertex buffer", testFvf}, {"cube map face lock", testCubeLock}, {"load surface from surface", testLoadSurfaceFromSurface},
        {"teardown with captures", testTeardownWithCaptures},  // (keep last: the device is destroyed right after it)
    };
    for (const auto& [name, fn] : cases) {
        if (!only.empty() && std::none_of(only.begin(), only.end(), [&](const std::string& part) { return std::string_view(name).find(part) != std::string_view::npos; })) continue;
        const int before = g_failures;
        fn(f);
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", name);
    }
    dev.release(std::move(f.vb));
    dev.release(std::move(f.decl));
    dev.release(std::move(f.instDecl));
    f.vs.reset();
    f.vsInst.reset();
    f.psColor.reset();
    f.psTexture.reset();
    std::uint32_t verrors = dev.game().validationErrors();
    gpuscene::releaseGpuObjects(dev);  // (the process-wide tables' buffers: before the device, as HalDevice does)
    // the teardown's errors count too (the layer's leak check at vkDestroyDevice, VUID-vkDestroyDevice-device-05137, comes after the device's counter is gone)
    const auto teardown = std::make_shared<ErrorCountSink>();
    spdlog::default_logger()->sinks().push_back(teardown);
    dev.destroy();
    spdlog::default_logger()->sinks().pop_back();
    if (teardown->errors > 0) std::printf("  FAIL teardown: %u error(s) logged while the device was destroyed\n", teardown->errors.load());
    verrors += teardown->errors.load();
    std::printf("%s: %d failure(s), %u validation error(s)\n", g_failures == 0 && verrors == 0 ? "PASS" : "FAIL", g_failures, verrors);
    return g_failures == 0 && verrors == 0 ? 0 : 1;
}
