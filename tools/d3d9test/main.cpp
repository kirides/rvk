// rvk_d3d9test: checks d3d9::Device against Direct3D 9 semantics, off-screen with validation on.
//
//   rvk_d3d9test [--validation | --no-validation] [--backend vulkan|d3d12] [--keep-images] [--present-test [0]]
// --backend d3d12 runs the same cases on the D3D12 game device (docs/research/dx12-game-port.md WP3 / WP4).
//
// Every case renders into the 64x64 back buffer with D3D9-style calls (states, streams, D3DX-assembled shaders), reads the
// pixels back (readBackBuffer) and compares them with what D3D9 produces. Exit code 0 = all pass and no validation errors.

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "common/d3dx.h"
#include "d3d9/convert.h"
#include "d3d9/device.h"
#include "d3d9/fvf.h"
#include "gpuscene/draw_record_table.h"
#include "gpuscene/view_table.h"
#include "rhi/d3d_formats.h"

using namespace rvk;
using namespace rvk::d3d9;

namespace {

constexpr std::uint32_t kSize = 64;
bool g_keepImages = false;
static rhi::Backend g_backend = rhi::Backend::Vulkan;  // --backend
int g_failures = 0;

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
    // (every frame's table flush goes into the upload list that runs before the frame's draws)
    const auto end = [&](const char* name) {
        tab.flush(dev);
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
        std::memcpy(in.psC5, regs[viewSlot == 9 ? 1 : 0].ps5, sizeof(in.psC5));
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
        refused += dev.ownedDerive(cd[0], in, windowsOf(40), tab.blockRef(slots[0]), tab.zeroRef(), true, out) == 17;
        std::vector<Device::OwnedItem> items;
        for (std::uint32_t k = 0; k < kQuads; ++k) items.push_back({&cd[k], dev.ownedBucketKey(cd[k], 3), slots[k]});
        Device::OwnedArena arena;
        const bool reserved = dev.ownedReserve(4, arena);  // (room for four of the eight)
        const bool tooMany = reserved && !dev.ownedFlush(items, arena, 3, tab.recordsRef(), tab.recordsShadow(), gpuscene::DrawRecordTable::kCapacity);
        Device::OwnedArena arena2;
        const bool reserved2 = dev.ownedReserve(kQuads, arena2);
        const bool noTable = reserved2 && !dev.ownedFlush(items, arena2, 3, rhi::GpuRef{}, tab.recordsShadow(), gpuscene::DrawRecordTable::kCapacity);
        const bool badSlot = reserved2 && !dev.ownedFlush(items, arena2, 40, tab.recordsRef(), tab.recordsShadow(), gpuscene::DrawRecordTable::kCapacity);
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
    std::printf("  owned persistent: %llu derivations, %llu refused, %llu buckets / %llu indirect packets / %llu items; %llu slot writes, %llu unchanged, %llu allocated, %llu released, %llu reused\n",
                static_cast<unsigned long long>(dev.ownedPersistStats.derived), static_cast<unsigned long long>(dev.ownedPersistStats.refused), static_cast<unsigned long long>(dev.ownedPersistStats.buckets),
                static_cast<unsigned long long>(dev.ownedPersistStats.packets), static_cast<unsigned long long>(dev.ownedPersistStats.items), static_cast<unsigned long long>(tab.stats().writes),
                static_cast<unsigned long long>(tab.stats().same), static_cast<unsigned long long>(tab.stats().allocs), static_cast<unsigned long long>(tab.stats().releases),
                static_cast<unsigned long long>(tab.stats().reused));
    for (const std::uint32_t s : slots) {
        if (s != gpuscene::kNoSlot) tab.release(s);
    }
    tab.setActive(false);
    tab.destroy();
    dev.setSceneViews(false);
    dev.setSceneDraws(false, 0);
    dev.release(std::move(vb));
    dev.release(std::move(ib));
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

// F4-1 owned submit (frame-ownership.md 7.17) through the device's own path: four quads of a translated vertex shader that reads VS window 0 (c0) and window 2 (c32.w, c33)
// are drawn normally (reference), captured as retained scene draws, and then submitted with Device::submitOwned from the retained packets alone: the register file holds
// junk, the windows are persistent images of the caller's (a view-block-like c0 image, a billboard-constants-like c31.. image). The pictures must be identical, the four
// draws must leave as one indirect run; refusals (a window image shorter than the shader reads, a null base, a dry run) must draw nothing, a changed image must change
// the picture (the draw really reads it)
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

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool validation = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-validation") validation = false;
        if (a == "--validation") validation = true;
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
        {"stencil", testStencil}, {"scope clear", testScopeClear}, {"no-stencil depth", testNoStencilDepth},     {"render to texture", testRenderToTexture}, {"dxt1", testDxt1},
        {"instancing", testInstancing}, {"UP + scissor", testUpAndScissor}, {"stretch rect", testStretchRect},
        {"mip chain stretch rect", testMipChainStretchRect}, {"dynamic buffer", testDynamicBuffer}, {"dynamic buffer across frames", testDynamicBufferAcrossFrames},
        {"partial texture lock", testPartialLock}, {"lock after upload", testLockAfterUpload},
        {"read-only lock of a fresh texture", testReadOnlyLockOfFreshTexture},
        {"scene draws (indirect)", testSceneDraws}, {"owned replay (records)", testOwnedReplay}, {"owned replay v2 (view table)", testOwnedReplayV2}, {"owned persistent (F2c-4a)", testOwnedPersistent}, {"owned windows (VS 1 / 2)", testOwnedWindows}, {"owned submit (F4-1)", testOwnedSubmit}, {"fvf vertex buffer", testFvf}, {"cube map face lock", testCubeLock}, {"load surface from surface", testLoadSurfaceFromSurface},
    };
    for (const auto& [name, fn] : cases) {
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
    const std::uint32_t verrors = dev.game().validationErrors();
    dev.destroy();
    std::printf("%s: %d failure(s), %u validation error(s)\n", g_failures == 0 && verrors == 0 ? "PASS" : "FAIL", g_failures, verrors);
    return g_failures == 0 && verrors == 0 ? 0 : 1;
}
