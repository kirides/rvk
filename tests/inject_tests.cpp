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
#include "shader/glsl.h"
#include "shader/sm_ir.h"
#include "shader/spirv.h"
#include "test.h"

using namespace rvk;
namespace abi = rvk::shader::abi;

namespace {

// struct name -> member name -> Offset decoration, from a SPIR-V module (OpName 5, OpMemberName 6, OpMemberDecorate 72)
std::map<std::string, std::map<std::string, std::uint32_t>> memberOffsets(std::span<const std::uint32_t> words) {
    std::map<std::uint32_t, std::string> names;
    std::map<std::uint32_t, std::map<std::uint32_t, std::string>> memberNames;
    std::map<std::uint32_t, std::map<std::uint32_t, std::uint32_t>> offsets;
    const auto str = [](std::span<const std::uint32_t> w) {
        std::string s;
        for (std::uint32_t x : w) {
            for (int b = 0; b < 4; ++b) {
                const char c = static_cast<char>((x >> (8 * b)) & 0xff);
                if (c == '\0') return s;
                s += c;
            }
        }
        return s;
    };
    for (std::size_t i = 5; i < words.size();) {
        const std::uint32_t op = words[i] & 0xffff, n = words[i] >> 16;
        if (n == 0 || i + n > words.size()) break;
        const auto ins = words.subspan(i, n);
        if (op == 5 && n >= 3) names[ins[1]] = str(ins.subspan(2));
        if (op == 6 && n >= 4) memberNames[ins[1]][ins[2]] = str(ins.subspan(3));
        if (op == 72 && n >= 5 && ins[3] == 35) offsets[ins[1]][ins[2]] = ins[4];  // Offset
        i += n;
    }
    std::map<std::string, std::map<std::string, std::uint32_t>> out;
    for (const auto& [id, members] : offsets) {
        auto& o = out[names[id]];
        for (const auto& [m, off] : members) o[memberNames[id][m]] = off;
    }
    return out;
}

void checkOffsets(const std::map<std::string, std::uint32_t>& glsl, const std::map<std::string, std::size_t>& cpp, const char* what) {
    CHECK_MSG(glsl.size() == cpp.size(), std::format("{}: {} members in GLSL, {} in C++", what, glsl.size(), cpp.size()));
    for (const auto& [name, off] : cpp) {
        const auto it = glsl.find(name);
        CHECK_MSG(it != glsl.end() && it->second == off,
                  std::format("{}.{}: C++ offset {}, GLSL {}", what, name, off, it != glsl.end() ? std::to_string(it->second) : "missing"));
    }
}

}  // namespace

// ---- the GLSL ABI header against the C++ structs --------------------------------------------------------------------------

TEST(abi_glsl_layout_matches_cpp) {
    const auto words = shader::embedded::spirv("test/abi_layout.comp");
    CHECK(!words.empty());
    const auto m = memberOffsets(words);
    CHECK(m.contains("RvkPush") && m.contains("RvkFrame") && m.contains("RvkDraw") && m.contains("RvkStage"));
    if (!m.contains("RvkPush") || !m.contains("RvkFrame") || !m.contains("RvkDraw") || !m.contains("RvkStage")) return;
    using PC = abi::PushConstants;
    checkOffsets(m.at("RvkPush"),
                 {{"vs", offsetof(PC, vsConsts)}, {"ps", offsetof(PC, psConsts)}, {"draw", offsetof(PC, draw)},
                  {"frame", offsetof(PC, frame)}, {"prevVs", offsetof(PC, prevVsConsts)}, {"drawId", offsetof(PC, drawId)},
                  {"flags", offsetof(PC, flags)}, {"vsw", offsetof(PC, vsWin)}, {"psw", offsetof(PC, psWin)}},
                 "RvkPush");
    using DB = abi::DrawBlock;
    checkOffsets(m.at("RvkDraw"),
                 {{"tex", offsetof(DB, tex)}, {"smp", offsetof(DB, smp)}, {"posFix", offsetof(DB, posFix)},
                  {"alphaFunc", offsetof(DB, alphaFunc)}, {"alphaRef", offsetof(DB, alphaRef)}, {"pad0", offsetof(DB, pad)},
                  {"pad1", offsetof(DB, pad) + 4}, {"psC5", offsetof(DB, psC5)}},
                 "RvkDraw");
    checkOffsets(m.at("RvkStage"), {{"c", 0}, {"i", abi::kStageConstsIntOffset}, {"b", abi::kStageConstsBoolOffset}}, "RvkStage");
    using FB = abi::FrameBlock;
    checkOffsets(m.at("RvkFrame"),
                 {{"viewProj", offsetof(FB, viewProj)}, {"invViewProj", offsetof(FB, invViewProj)}, {"view", offsetof(FB, view)},
                  {"proj", offsetof(FB, proj)}, {"invView", offsetof(FB, invView)}, {"invProj", offsetof(FB, invProj)},
                  {"prevViewProj", offsetof(FB, prevViewProj)}, {"prevInvViewProj", offsetof(FB, prevInvViewProj)},
                  {"cameraPos", offsetof(FB, cameraPos)}, {"prevCameraPos", offsetof(FB, prevCameraPos)},
                  {"depthParams", offsetof(FB, depthParams)}, {"viewport", offsetof(FB, viewport)}, {"jitter", offsetof(FB, jitter)},
                  {"time", offsetof(FB, time)}, {"frameInfo", offsetof(FB, frameInfo)}, {"images", offsetof(FB, images)},
                  {"params", offsetof(FB, params)}, {"rtScene", offsetof(FB, rtScene)}, {"rtInfo", offsetof(FB, rtInfo)},
                  {"gi", offsetof(FB, gi)}, {"ao", offsetof(FB, ao)}, {"sgi", offsetof(FB, sgi)}},
                 "RvkFrame");
}

// the RT scene's geometry record (shaders/include/rvk_rt.glsl) and the RT debug view's push block (shaders/rt/rt_debug.glsl)
TEST(abi_glsl_rt_layout_matches_cpp) {
    const auto m = memberOffsets(shader::embedded::spirv("test/abi_rt_layout.comp"));
    CHECK(m.contains("RvkRtRecord") && m.contains("RtDebugPush"));
    if (!m.contains("RvkRtRecord") || !m.contains("RtDebugPush")) return;
    using R = abi::RtGeometryRecord;
    checkOffsets(m.at("RvkRtRecord"),
                 {{"vertices", offsetof(R, vertices)}, {"indices", offsetof(R, indices)}, {"stride", offsetof(R, stride)},
                  {"flags", offsetof(R, flags)}, {"normal", offsetof(R, normal)}, {"texcoord0", offsetof(R, texcoord0)},
                  {"texcoord1", offsetof(R, texcoord1)}, {"texcoord6", offsetof(R, texcoord6)}, {"color0", offsetof(R, color0)},
                  {"instanceId", offsetof(R, instanceId)}, {"albedoTex", offsetof(R, albedoTex)},
                  {"albedoSampler", offsetof(R, albedoSampler)}, {"alpha", offsetof(R, alpha)}, {"uvSet", offsetof(R, uvSet)},
                  {"uvMap0", offsetof(R, uvMap)}, {"uvMap1", offsetof(R, uvMap) + 16}, {"segments", offsetof(R, segments)},
                  {"segmentCount", offsetof(R, segmentCount)}, {"pad", offsetof(R, pad)}},
                 "RvkRtRecord");
    CHECK(m.contains("RvkRtSegment"));
    if (m.contains("RvkRtSegment")) {
        using S = abi::RtMaterialSegment;
        checkOffsets(m.at("RvkRtSegment"),
                     {{"firstPrimitive", offsetof(S, firstPrimitive)}, {"primitiveCount", offsetof(S, primitiveCount)},
                      {"albedoTex", offsetof(S, albedoTex)}, {"albedoSampler", offsetof(S, albedoSampler)}, {"alpha", offsetof(S, alpha)},
                      {"uvSet", offsetof(S, uvSet)}, {"pad0", offsetof(S, pad)}, {"pad1", offsetof(S, pad) + 4},
                      {"uvMap0", offsetof(S, uvMap)}, {"uvMap1", offsetof(S, uvMap) + 16}},
                     "RvkRtSegment");
    }
    using P = enh::RtDebugPush;
    checkOffsets(m.at("RtDebugPush"),
                 {{"frame", offsetof(P, h) + offsetof(abi::PassPushHeader, frame)},
                  {"passId", offsetof(P, h) + offsetof(abi::PassPushHeader, passId)},
                  {"passFlags", offsetof(P, h) + offsetof(abi::PassPushHeader, passFlags)}, {"depthTex", offsetof(P, depthTex)},
                  {"width", offsetof(P, width)}, {"height", offsetof(P, height)}, {"srcTex", offsetof(P, srcTex)},
                  {"srcSampler", offsetof(P, srcSampler)}, {"mode", offsetof(P, mode)}, {"probes", offsetof(P, probes)}},
                 "RtDebugPush");
}

// the shadow mask's block (shaders/shadows/shadow_block.glsl, shared by its vertex and fragment shader) against enh::ShadowBlock:
// the vertex shader reads depthRemap from the block that replaces the draw's DrawBlock (docs/research/engine-frame/shadow-bug.md)
TEST(shadows_block_glsl_matches_cpp) {
    const auto m = memberOffsets(shader::embedded::spirv("test/shadow_layout.comp"));
    CHECK(m.contains("ShadowBlock"));
    if (!m.contains("ShadowBlock")) return;
    using B = enh::ShadowBlock;
    checkOffsets(m.at("ShadowBlock"),
                 {{"cascade", offsetof(B, cascade)}, {"split", offsetof(B, split)}, {"texel", offsetof(B, texel)},
                  {"maps", offsetof(B, maps)}, {"res", offsetof(B, res)}, {"vsm", offsetof(B, vsm)},
                  {"vsmParams", offsetof(B, vsmParams)}, {"params", offsetof(B, params)}, {"target", offsetof(B, target)},
                  {"mode", offsetof(B, mode)}, {"rt", offsetof(B, rt)}, {"rtParams", offsetof(B, rtParams)},
                  {"depthRemap", offsetof(B, depthRemap)}},
                 "ShadowBlock");
    // the old bug: the vertex shader read RvkDraw.posFix from this block; a ShadowBlock member must never sit where a DrawBlock's
    // posFix does unless it is the remap itself
    CHECK(offsetof(B, depthRemap) != offsetof(abi::DrawBlock, posFix));
}

TEST(abi_glsl_pass_push_matches_cpp) {
    const auto m = memberOffsets(shader::embedded::spirv("test/abi_pass_layout.comp"));
    CHECK(m.contains("DebugViewPush"));
    if (!m.contains("DebugViewPush")) return;
    using P = enh::DebugViewPush;
    checkOffsets(m.at("DebugViewPush"),
                 {{"frame", offsetof(P, h) + offsetof(abi::PassPushHeader, frame)},
                  {"passId", offsetof(P, h) + offsetof(abi::PassPushHeader, passId)},
                  {"passFlags", offsetof(P, h) + offsetof(abi::PassPushHeader, passFlags)}, {"depthTex", offsetof(P, depthTex)},
                  {"width", offsetof(P, width)}, {"height", offsetof(P, height)}, {"srcTex", offsetof(P, srcTex)},
                  {"srcSampler", offsetof(P, srcSampler)}, {"pad", offsetof(P, pad)}},
                 "DebugViewPush");
}

// every embedded shader is there and valid SPIR-V for Vulkan 1.3; the ABI text is the header translated shaders get
TEST(embedded_shaders) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::size_t n = 0;
    for (const auto& e : shader::embedded::all()) {
        ++n;
        CHECK_MSG(!e.spirv.empty() && e.spirv[0] == 0x07230203u, std::string(e.name));
        CHECK_MSG(tools.Validate(e.spirv.data(), e.spirv.size()), std::string(e.name));
    }
    CHECK(n >= 5);
    CHECK(!shader::embedded::spirv("debug/inject_view.comp").empty());
    CHECK(shader::embedded::spirv("no/such.comp").empty());
    const std::string_view text = shader::embedded::abiGlsl();
    CHECK(text.find("uniform RvkPush") != std::string_view::npos);
    CHECK(!text.starts_with("#version") && text.find("\n#version") == std::string_view::npos);  // the translator writes its own
}

// the constant register windows: abi.h's tables and rvk_abi.glsl's RVK_VS_WIN / RVK_PS_WIN macros name the same boundaries
TEST(const_windows_match_glsl_macros) {
    using namespace shader::abi;
    CHECK(constWindow(true, 0) == 0 && constWindow(true, 11) == 0 && constWindow(true, 12) == 1 && constWindow(true, 30) == 1 &&
          constWindow(true, 31) == 2 && constWindow(true, 231) == 2 && constWindow(true, 232) == 3 && constWindow(true, 255) == 3);
    CHECK(constWindow(false, 4) == 0 && constWindow(false, 5) == 1 && constWindow(false, 6) == 2 && constWindow(false, 13) == 2 &&
          constWindow(false, 14) == 3 && constWindow(false, 223) == 3);
    const std::string text(shader::embedded::abiGlsl());
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

TEST(glsl_abi_header_and_hooks) {
    // ps_3_0: mov r0, c15; mov oC0, r0
    const std::vector<std::uint32_t> ps{0xFFFF0300u, 0x02000001u, 0x800F0000u, 0xA0E4000Fu, 0x02000001u, 0x800F0800u, 0x80E40000u,
                                        0x0000FFFFu};
    const auto dec = shader::decode(ps);
    CHECK(dec.ok());
    if (!dec.ok()) return;
    const auto plain = shader::toGlsl(dec.program);
    CHECK(plain.ok());
    CHECK(plain.glsl.find(shader::embedded::abiGlsl()) != std::string::npos);  // verbatim
    CHECK(plain.glsl.find("RVK_PSC(15)") != std::string::npos);  // through the pixel window of c15 (abi.h)
    const auto plainSpv = shader::compileGlsl(shader::Stage::Pixel, plain.glsl);
    CHECK_MSG(plainSpv.ok(), plainSpv.log);
    // the push block of a translated shader is the 112-byte one (the windows after flags); split constants are on
    if (plainSpv.ok()) {
        const auto m = memberOffsets(plainSpv.words);
        CHECK(m.contains("RvkPush") && m.at("RvkPush").size() == 9 && m.at("RvkPush").at("flags") == 44 &&
              m.at("RvkPush").at("vsw") == 48 && m.at("RvkPush").at("psw") == 80);
    }
    CHECK(plain.reflection.splitConsts && plain.reflection.windowEnd[3] == 16 && plain.reflection.windowEnd[0] == 0);
    // all hooks empty = identical output
    CHECK(shader::toGlsl(dec.program, shader::GlslOptions{}).glsl == plain.glsl);

    shader::GlslOptions o;
    o.extraGlobals = "layout(location = 1) out vec4 rvk_extra;";
    o.epilogue = "    rvk_extra = vec4(float(pc.drawId), 0.0, 0.0, 1.0);";
    o.constOverrides[15] = "$c * 0.5 + pc.frame.params[0]";
    const auto hooked = shader::toGlsl(dec.program, o);
    CHECK(hooked.ok());
    CHECK(hooked.glsl.find("(RVK_PSC(15) * 0.5 + pc.frame.params[0])") != std::string::npos);
    CHECK(hooked.glsl.find("case 15: return") != std::string::npos);  // relative reads too
    CHECK(hooked.glsl.find("rvk_extra = vec4(float(pc.drawId)") < hooked.glsl.find("rvk_oC0 = oC0"));
    const auto hookedSpv = shader::compileGlsl(shader::Stage::Pixel, hooked.glsl);
    CHECK_MSG(hookedSpv.ok(), hookedSpv.log + "\n" + hooked.glsl);

    // vertex shader epilogue: after rvk_pos, before the half-pixel fix
    const std::vector<std::uint32_t> vs{0xFFFE0300u, 0x0200001Fu, 0x80000000u, 0x900F0000u, 0x0200001Fu, 0x80000000u, 0xE00F0000u,
                                        0x02000001u, 0xE00F0000u, 0x90E40000u, 0x0000FFFFu};
    const auto vdec = shader::decode(vs);
    CHECK(vdec.ok());
    if (vdec.ok()) {
        shader::GlslOptions vo;
        vo.extraGlobals = "layout(location = 30) out vec4 rvk_clip;";
        vo.epilogue = "    rvk_clip = rvk_pos;";
        const auto v = shader::toGlsl(vdec.program, vo);
        CHECK(v.ok());
        CHECK(v.glsl.find("rvk_clip = rvk_pos;") < v.glsl.find("rvk_pos.xy += pc.draw.posFix.xy"));
        const auto vspv = shader::compileGlsl(shader::Stage::Vertex, v.glsl);
        CHECK_MSG(vspv.ok(), vspv.log);
        // input override: the attribute is fetched as rvk_in#, v# assigned from the expression before the body runs
        shader::GlslOptions io;
        io.inputOverrides.push_back({shader::Usage::Position, 0, "vec4($v.xy * pc.frame.params[1].x, $v.zw)"});
        const auto iv = shader::toGlsl(vdec.program, io);
        CHECK(iv.ok());
        CHECK(iv.glsl.find("in vec4 rvk_in0;") != std::string::npos);
        CHECK(iv.glsl.find("    v0 = (vec4(rvk_in0.xy * pc.frame.params[1].x, rvk_in0.zw));") < iv.glsl.find("    rvk_main();"));
        const auto ispv = shader::compileGlsl(shader::Stage::Vertex, iv.glsl);
        CHECK_MSG(ispv.ok(), ispv.log + "\n" + iv.glsl);
        // an override of a semantic the shader does not declare changes nothing
        shader::GlslOptions none;
        none.inputOverrides.push_back({shader::Usage::BlendWeight, 1, "$v * 2.0"});
        CHECK(shader::toGlsl(vdec.program, none).glsl == shader::toGlsl(vdec.program).glsl);
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
}
