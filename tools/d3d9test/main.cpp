// rvk_d3d9test: checks d3d9::Device against Direct3D 9 semantics, off-screen with validation on.
//
//   rvk_d3d9test [--no-validation] [--keep-images]
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
#include "vk/formats.h"

using namespace rvk;
using namespace rvk::d3d9;

namespace {

constexpr std::uint32_t kSize = 64;
bool g_keepImages = false;
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
    return dev.createShader(tokens);
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
    d.format = vk::d3dfmt::A8R8G8B8;
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

void testDxt1(Fixture& f) {
    // one DXT1 block per 4x4: colour0 = pure red (0xF800), colour1 = pure blue (0x001F), all indices 0 -> red
    TextureDesc d;
    d.width = d.height = 8;
    d.format = vk::d3dfmt::DXT1;
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
    d.format = vk::d3dfmt::A8R8G8B8;
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
    d.format = vk::d3dfmt::A8R8G8B8;
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
    d.format = vk::d3dfmt::A8R8G8B8;
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
    d.format = vk::d3dfmt::A8R8G8B8;
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
    d.format = vk::d3dfmt::A8R8G8B8;
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
    d.format = vk::d3dfmt::DXT1;
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
    dd.format = vk::d3dfmt::A8R8G8B8;
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
        if (a == "--keep-images") g_keepImages = true;
        if (a == "--present-test") return presentTest(i + 1 >= argc || std::string(argv[i + 1]) != "0");
    }
    if (!devtools::D3DX::get().available()) {
        std::printf("d3dx9_39.dll not available\n");
        return 2;
    }
    Device dev;
    DeviceOptions o;
    o.width = o.height = kSize;
    o.depthFormat = vk::d3dfmt::D24S8;
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
        {"stencil", testStencil},     {"render to texture", testRenderToTexture}, {"dxt1", testDxt1},
        {"instancing", testInstancing}, {"UP + scissor", testUpAndScissor}, {"stretch rect", testStretchRect},
        {"dynamic buffer", testDynamicBuffer}, {"dynamic buffer across frames", testDynamicBufferAcrossFrames},
        {"partial texture lock", testPartialLock}, {"lock after upload", testLockAfterUpload},
        {"read-only lock of a fresh texture", testReadOnlyLockOfFreshTexture},
        {"fvf vertex buffer", testFvf}, {"cube map face lock", testCubeLock}, {"load surface from surface", testLoadSurfaceFromSurface},
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
    const std::uint32_t verrors = dev.ctx().validationErrors();
    dev.destroy();
    std::printf("%s: %d failure(s), %u validation error(s)\n", g_failures == 0 && verrors == 0 ? "PASS" : "FAIL", g_failures, verrors);
    return g_failures == 0 && verrors == 0 ? 0 : 1;
}
