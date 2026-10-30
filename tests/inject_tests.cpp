// Enhancement plumbing (docs/research/injection.md): the GLSL ABI against shader/abi.h (offsets from the embedded SPIR-V), the
// FrameBlock contents, the injection-point registry, the generator's variant hooks, the embedded shaders.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <spirv-tools/libspirv.hpp>

#include "d3d9/inject.h"
#include "d3d9/scene.h"
#include "enh/debug_view.h"
#include "enh/rt_debug.h"
#include "enh/shadows.h"
#include "shader/abi.h"
#include "shader/embedded.h"
#include "shader/dxc.h"
#include "shader/hlsl.h"
#include "shader/sm_ir.h"
#include "shader/spirv.h"
#include "spirv_offsets.h"
#include "test.h"

using namespace rvk;
namespace abi = rvk::shader::abi;

namespace {

using rvk::test::memberOffsets;

}  // namespace

// every embedded shader is there and valid SPIR-V for Vulkan 1.3 (dxc_every_embedded_shader_compiles covers the whole set; the unknown ones are empty)
TEST(embedded_shaders) {
    if (!shader::dxc::available()) return;
    CHECK(!shader::embedded::spirv("debug/inject_view.comp").empty());
    CHECK(shader::embedded::spirv("no/such.comp").empty());
    const std::string_view text = shader::embedded::hlslSource("include/rvk_draw.hlsli");
    CHECK(text.find("RVK_PUSH(RvkDrawPush, pc)") != std::string_view::npos);
}

// the constant register windows: abi.h's tables and rvk_draw.hlsli's RVK_VS_WIN / RVK_PS_WIN macros name the same boundaries
TEST(const_windows_match_hlsl_macros) {
    using namespace shader::abi;
    CHECK(constWindow(true, 0) == 0 && constWindow(true, 11) == 0 && constWindow(true, 12) == 1 && constWindow(true, 30) == 1 &&
          constWindow(true, 31) == 2 && constWindow(true, 231) == 2 && constWindow(true, 232) == 3 && constWindow(true, 255) == 3);
    CHECK(constWindow(false, 4) == 0 && constWindow(false, 5) == 1 && constWindow(false, 6) == 2 && constWindow(false, 13) == 2 &&
          constWindow(false, 14) == 3 && constWindow(false, 223) == 3);
    const std::string text(shader::embedded::hlslSource("include/rvk_draw.hlsli"));
    for (const char* which : {"VS", "PS"}) {
        const std::size_t at = text.find(std::string("#define RVK_") + which + "_WIN(n)");
        CHECK(at != std::string::npos);
        if (at == std::string::npos) continue;
        const std::string line = text.substr(at, text.find('\n', at) - at);
        const std::uint32_t* f = which[0] == 'V' ? kVsWindowFirst : kPsWindowFirst;
        for (std::uint32_t w = 1; w < kConstWindows; ++w) {
            CHECK_MSG(line.find(std::format("< {}u ? {}", f[w], w - 1)) != std::string::npos, line);
        }
    }
}

// ---- the translator: the shared header, variant hooks ---------------------------------------------------------------------

TEST(hlsl_abi_header_and_hooks) {
    if (!shader::dxc::available()) return;
    // ps_3_0: mov r0, c15; mov oC0, r0
    const std::vector<std::uint32_t> ps{0xFFFF0300u, 0x02000001u, 0x800F0000u, 0xA0E4000Fu, 0x02000001u, 0x800F0800u, 0x80E40000u,
                                        0x0000FFFFu};
    const auto dec = shader::decode(ps);
    CHECK(dec.ok());
    if (!dec.ok()) return;
    const auto plain = shader::toHlsl(dec.program);
    CHECK(plain.ok());
    CHECK(plain.hlsl.find("#include \"rvk_draw.hlsli\"") != std::string::npos);
    CHECK(plain.hlsl.find("RVK_PSC(15)") != std::string::npos);  // through the pixel window of c15 (abi.h)
    const auto plainSpv = shader::compileHlsl(shader::Stage::Pixel, plain.hlsl);
    CHECK_MSG(plainSpv.ok(), plainSpv.log);
    // the push block of a translated shader is the 112-byte one (the windows after flags); split constants are on
    if (plainSpv.ok()) {
        const auto m = rvk::test::memberOffsets(plainSpv.words);
        const std::string name = "type.PushConstant.RvkDrawPush";
        CHECK(m.contains(name) && m.at(name).size() == 9 && m.at(name).at("flags") == 44 && m.at(name).at("vsw") == 48 && m.at(name).at("psw") == 80);
    }
    CHECK(plain.reflection.splitConsts && plain.reflection.windowEnd[3] == 16 && plain.reflection.windowEnd[0] == 0);
    // all hooks empty = identical output
    CHECK(shader::toHlsl(dec.program, shader::TranslateOptions{}).hlsl == plain.hlsl);

    shader::TranslateOptions o;
    o.extraOutputs = "    float4 rvk_extra : SV_Target1;";
    o.epilogue = "    rvk_out.rvk_extra = float4(float(pc.drawId), 0.0, 0.0, 1.0);";
    o.constOverrides[15] = "$c * 0.5 + pc.frame.Get().params[0]";
    const auto hooked = shader::toHlsl(dec.program, o);
    CHECK(hooked.ok());
    CHECK(hooked.hlsl.find("(RVK_PSC(15) * 0.5 + pc.frame.Get().params[0])") != std::string::npos);
    CHECK(hooked.hlsl.find("case 15: return") != std::string::npos);  // relative reads too
    CHECK(hooked.hlsl.find("rvk_out.rvk_extra = float4(float(pc.drawId)") < hooked.hlsl.find("rvk_out.oC0 = oC0"));
    const auto hookedSpv = shader::compileHlsl(shader::Stage::Pixel, hooked.hlsl);
    CHECK_MSG(hookedSpv.ok(), hookedSpv.log + "\n" + hooked.hlsl);

    // vertex shader epilogue: after rvk_pos, before the half-pixel fix
    const std::vector<std::uint32_t> vs{0xFFFE0300u, 0x0200001Fu, 0x80000000u, 0x900F0000u, 0x0200001Fu, 0x80000000u, 0xE00F0000u,
                                        0x02000001u, 0xE00F0000u, 0x90E40000u, 0x0000FFFFu};
    const auto vdec = shader::decode(vs);
    CHECK(vdec.ok());
    if (vdec.ok()) {
        shader::TranslateOptions vo;
        vo.extraOutputs = "    [[vk::location(30)]] float4 rvk_clip : VARY30;";
        vo.epilogue = "    rvk_out.rvk_clip = rvk_pos;";
        const auto v = shader::toHlsl(vdec.program, vo);
        CHECK(v.ok());
        CHECK(v.hlsl.find("rvk_out.rvk_clip = rvk_pos;") < v.hlsl.find("rvk_pos.xy += rvk_draw.posFix.xy"));
        const auto vspv = shader::compileHlsl(shader::Stage::Vertex, v.hlsl);
        CHECK_MSG(vspv.ok(), vspv.log);
        // input override: v# assigned from the expression (with the attribute as rvk_in.v#) before the body runs
        shader::TranslateOptions io;
        io.inputOverrides.push_back({shader::Usage::Position, 0, "float4($v.xy * pc.frame.Get().params[1].x, $v.zw)"});
        const auto iv = shader::toHlsl(vdec.program, io);
        CHECK(iv.ok());
        CHECK(iv.hlsl.find("    v0 = (float4(rvk_in.v0.xy * pc.frame.Get().params[1].x, rvk_in.v0.zw));") < iv.hlsl.find("    rvk_main();"));
        const auto ispv = shader::compileHlsl(shader::Stage::Vertex, iv.hlsl);
        CHECK_MSG(ispv.ok(), ispv.log + "\n" + iv.hlsl);
        // an override of a semantic the shader does not declare changes nothing
        shader::TranslateOptions none;
        none.inputOverrides.push_back({shader::Usage::BlendWeight, 1, "$v * 2.0"});
        CHECK(shader::toHlsl(vdec.program, none).hlsl == shader::toHlsl(vdec.program).hlsl);
    }
}

// ---- FrameBlock ----------------------------------------------------------------------------------------------------------

namespace {

d3d9::scene::CameraInput cameraAt(float x, float y, float z) {
    using d3d9::scene::Mat4;
    d3d9::scene::CameraInput in;
    in.view = Mat4::identity();
    in.view.at(3, 0) = -x;
    in.view.at(3, 1) = -y;
    in.view.at(3, 2) = -z;
    // D3D LH perspective, near 10, far 1000, 90 degrees
    const float n = 10.0f, f = 1000.0f;
    in.projection = Mat4{};
    in.projection.at(0, 0) = 1.0f;
    in.projection.at(1, 1) = 1.0f;
    in.projection.at(2, 2) = f / (f - n);
    in.projection.at(2, 3) = 1.0f;
    in.projection.at(3, 2) = -n * f / (f - n);
    in.viewProjection = d3d9::scene::mul(in.view, in.projection);
    d3d9::scene::inverse(in.view, in.inverseView);
    in.position = {x, y, z};
    in.engineFar = 1000.0f;
    return in;
}

}  // namespace

TEST(frame_block_fill) {
    using namespace d3d9::scene;
    CameraBlock cur, last;
    FrameExtras x;
    x.frameIndex = 7;
    x.now = 3.5;
    x.width = 1600;
    x.height = 900;
    x.images = {11, 12, 13, 2};
    std::array<std::array<float, 4>, 16> params{};
    params[0] = {1.0f, 0.6f, 500.0f, 0.0f};
    x.params = &params;
    abi::FrameBlock fb{};

    // no camera at all: identities, no flags, time = now
    fillFrameBlock(fb, cur, last, x);
    CHECK(fb.frameInfo[0] == 7 && fb.frameInfo[1] == 0);
    CHECK(fb.viewProj[0] == 1.0f && fb.viewProj[5] == 1.0f && fb.viewProj[1] == 0.0f);
    CHECK(fb.time[0] == 3.5f && fb.viewport[0] == 1600.0f && std::abs(fb.viewport[3] - 1.0f / 900.0f) < 1e-9f);
    CHECK(fb.images[2] == 13 && fb.params[0][2] == 500.0f);

    // frame 1 with its main view
    cur.set(cameraAt(1, 2, 3), 1, 1.0);
    fillFrameBlock(fb, cur, last, x);
    CHECK(fb.frameInfo[1] == abi::kFrameCameraValid);
    CHECK(std::memcmp(fb.viewProj, cur.viewProjection.m.data(), sizeof(fb.viewProj)) == 0);
    CHECK(std::memcmp(fb.prevViewProj, fb.viewProj, sizeof(fb.viewProj)) == 0);  // no previous: = current
    CHECK(fb.cameraPos[0] == 1.0f && fb.cameraPos[3] == 1000.0f && std::abs(fb.depthParams[0] - 10.0f) < 1e-2f);
    CHECK(std::abs(fb.depthParams[3] - 0.001f) < 1e-7f);
    // GLSL reads the stored matrix transposed: (viewProj^T) * p (column) must equal p * viewProj (row) of the C++ side
    {
        const float p[4] = {5.0f, 6.0f, 50.0f, 1.0f};
        float glsl[4]{}, cpp[4]{};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) glsl[r] += fb.viewProj[c * 4 + r] * p[c];  // column-major mat4 * vec4
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k) cpp[c] += p[k] * cur.viewProjection.at(k, c);
        for (int i = 0; i < 4; ++i) CHECK(std::abs(glsl[i] - cpp[i]) < 1e-4f);
        CHECK(std::abs(cpp[3] - 47.0f) < 1e-4f);  // w = view depth (z - camera z)
    }

    // next frame before its main view: predicted from the last frame
    last = cur;
    cur.advance();
    fillFrameBlock(fb, cur, last, x);
    CHECK(fb.frameInfo[1] == abi::kFrameCameraPredicted);
    CHECK(fb.cameraPos[0] == 1.0f);
    // ... and after it: previous = frame 1, delta time
    cur.set(cameraAt(4, 2, 3), 2, 1.25);
    fillFrameBlock(fb, cur, last, x);
    CHECK(fb.frameInfo[1] == (abi::kFrameCameraValid | abi::kFramePrevValid));
    CHECK(fb.cameraPos[0] == 4.0f && fb.prevCameraPos[0] == 1.0f);
    CHECK(std::memcmp(fb.prevViewProj, last.viewProjection.m.data(), sizeof(fb.prevViewProj)) == 0);
    CHECK(std::abs(fb.time[0] - 1.25f) < 1e-6f && std::abs(fb.time[1] - 0.25f) < 1e-6f);
}

TEST(draw_flags) {
    const std::uint32_t f = abi::drawFlags(5, 7, 1);
    CHECK((f & abi::kDrawKindMask) == 5);
    CHECK(((f >> abi::kDrawPassShift) & 0xf) == 7);
    CHECK(((f >> abi::kDrawViewShift) & 0xf) == 1);
    CHECK((f & abi::kDrawFlagTinted) == 0);
    CHECK(static_cast<std::uint32_t>(d3d9::scene::Pass::Count) <= 16 && static_cast<std::uint32_t>(d3d9::scene::View::Count) <= 16);
}

// ---- injection registry --------------------------------------------------------------------------------------------------

TEST(inject_registry) {
    using d3d9::inject::Point;
    d3d9::inject::Registry r;
    CHECK(r.empty() && !r.any(Point::AfterOpaque));
    int owner1 = 0, owner2 = 0;
    const auto nop = [](d3d9::PassContext&) {};
    const std::uint32_t a = r.add(Point::AfterOpaque, "a", nop, &owner1, 0);
    const std::uint32_t b = r.add(Point::AfterOpaque, "b", nop, &owner2, -5);
    const std::uint32_t c = r.add(Point::AfterOpaque, "c", nop, &owner1, 0);
    const std::uint32_t d = r.add(Point::BeforeGui, "d", nop, &owner2);
    CHECK(a != 0 && b != 0 && c != 0 && d != 0 && a != b && b != c);
    CHECK(r.add(Point::AfterOpaque, "empty", d3d9::inject::PassFn{}) == 0);
    CHECK(r.any(Point::AfterOpaque) && r.any(Point::BeforeGui) && !r.any(Point::AfterMainView));
    const auto& l = r.passes(Point::AfterOpaque);
    CHECK(l.size() == 3 && l[0].name == "b" && l[1].name == "a" && l[2].name == "c");  // order, then registration order
    CHECK(r.remove(a) && !r.remove(a));
    CHECK(r.passes(Point::AfterOpaque).size() == 2);
    CHECK(r.removeOwner(&owner2) == 2);
    CHECK(!r.any(Point::BeforeGui) && r.any(Point::AfterOpaque) && r.size() == 1);
    CHECK(r.removeOwner(&owner1) == 1 && r.empty());
}

TEST(inject_firings) {
    using d3d9::inject::Point;
    d3d9::inject::Firings f;
    CHECK(!f.take(Point::AfterOpaque, false));  // main-view points outside a main view
    CHECK(f.take(Point::BeforeGui, false) && f.take(Point::BeforeGui, false));
    CHECK(f.take(Point::BeforePresent, false) && !d3d9::inject::inMainView(Point::BeforePresent));  // the frame's end: no main view
    f.beginMainView();
    CHECK(f.mainViews() == 1);
    CHECK(f.take(Point::AfterOpaque, true));      // the refraction copy
    CHECK(!f.take(Point::AfterOpaque, true));     // DoBeginRenderAlpha afterwards: already run
    CHECK(f.take(Point::AfterRefractionCopy, true));
    f.beginMainView();                            // a second main view: again
    CHECK(f.mainViews() == 2 && f.take(Point::AfterOpaque, true));
    f.endFrame();
    CHECK(f.mainViews() == 0);
    CHECK(std::string(d3d9::inject::name(Point::AfterMainView)) == "AfterMainView");
    CHECK(std::string(d3d9::inject::name(Point::BeforePresent)) == "BeforePresent");
}
