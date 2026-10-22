// Shader translator tests: hand-written D3D9 assembly (assembled with d3dx9_39's D3DXAssembleShader, a CPU-only function)
// -> decode -> listing equal to D3DXDisassembleShader -> GLSL -> SPIR-V -> spirv-val. Covers what the game's corpus lacks
// (loops, subroutines, predicates, relative addressing, all texld flavours). Skipped when d3dx9_39.dll is not installed.

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <spirv-tools/libspirv.hpp>

#include "d3d9/ffshader.h"
#include "shader/glsl.h"
#include "shader/sm_ir.h"
#include "shader/spirv.h"
#include "test.h"

using namespace rvk::shader;

namespace {

struct D3DXBuffer {  // ID3DXBuffer
    virtual long __stdcall QueryInterface(const void*, void**) = 0;
    virtual unsigned long __stdcall AddRef() = 0;
    virtual unsigned long __stdcall Release() = 0;
    virtual void* __stdcall GetBufferPointer() = 0;
    virtual unsigned long __stdcall GetBufferSize() = 0;
};

struct D3DX {
    using AssembleFn = long(__stdcall*)(const char*, UINT, const void*, void*, DWORD, D3DXBuffer**, D3DXBuffer**);
    using DisassembleFn = long(__stdcall*)(const DWORD*, BOOL, const char*, D3DXBuffer**);
    AssembleFn assemble = nullptr;
    DisassembleFn disassemble = nullptr;

    static const D3DX& get() {
        static D3DX d = [] {
            D3DX x;
            if (HMODULE m = LoadLibraryW(L"d3dx9_39.dll")) {
                x.assemble = reinterpret_cast<AssembleFn>(GetProcAddress(m, "D3DXAssembleShader"));
                x.disassemble = reinterpret_cast<DisassembleFn>(GetProcAddress(m, "D3DXDisassembleShader"));
            }
            return x;
        }();
        return d;
    }
    bool available() const { return assemble != nullptr && disassemble != nullptr; }
};

std::vector<std::uint32_t> assemble(const std::string& src, std::string& errors) {
    D3DXBuffer* code = nullptr;
    D3DXBuffer* err = nullptr;
    const long hr = D3DX::get().assemble(src.c_str(), static_cast<UINT>(src.size()), nullptr, nullptr, 0, &code, &err);
    if (err != nullptr) {
        errors.assign(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
        err->Release();
    }
    if (hr < 0 || code == nullptr) {
        return {};
    }
    std::vector<std::uint32_t> tokens(code->GetBufferSize() / 4);
    std::memcpy(tokens.data(), code->GetBufferPointer(), tokens.size() * 4);
    code->Release();
    return tokens;
}

// listing lines without comments / blank lines; dcl/def sorted (their interleaving is not preserved by the decoder)
std::vector<std::string> normalizeListing(const std::string& text) {
    std::vector<std::string> decl, code;
    std::istringstream in(text);
    std::string line;
    static const std::regex exponent(R"(e([+-])0(\d\d))");
    while (std::getline(in, line)) {
        line.erase(std::remove(line.begin(), line.end(), '\0'), line.end());
        line.erase(0, line.find_first_not_of(" \t\r"));
        line.erase(line.find_last_not_of(" \t\r") + 1);
        if (line.empty() || line.starts_with("//")) {
            continue;
        }
        line = std::regex_replace(line, exponent, "e$1$2");
        (line.starts_with("dcl") || line.starts_with("def") ? decl : code).push_back(line);
    }
    std::ranges::sort(decl);
    decl.insert(decl.end(), code.begin(), code.end());
    return decl;
}

std::string referenceListing(const std::vector<std::uint32_t>& tokens) {
    D3DXBuffer* buf = nullptr;
    if (D3DX::get().disassemble(reinterpret_cast<const DWORD*>(tokens.data()), FALSE, nullptr, &buf) < 0 || buf == nullptr) {
        return {};
    }
    std::string s(static_cast<const char*>(buf->GetBufferPointer()), buf->GetBufferSize());
    buf->Release();
    return s;
}

// Assemble, decode, compare listings, translate, compile, validate. Returns the GLSL ("" on failure, CHECKs report).
std::string roundTripTokens(const char* name, const std::vector<std::uint32_t>& tokens);
std::string roundTrip(const char* name, const std::string& source) {
    std::string errors;
    const auto tokens = assemble(source, errors);
    CHECK_MSG(!tokens.empty(), std::string(name) + ": D3DXAssembleShader failed: " + errors);
    if (tokens.empty()) {
        return {};
    }
    return roundTripTokens(name, tokens);
}

// decode (listing equal to D3DX's), translate, compile, validate
std::string roundTripTokens(const char* name, const std::vector<std::uint32_t>& tokens) {
    const DecodeResult dec = decode(tokens);
    CHECK_MSG(dec.ok(), std::string(name) + ": decode: " + dec.error);
    if (!dec.ok()) {
        return {};
    }
    const auto ref = normalizeListing(referenceListing(tokens));
    const auto ours = normalizeListing(disassemble(dec.program));
    if (ref != ours) {
        std::string diff;
        for (std::size_t i = 0; i < std::max(ref.size(), ours.size()); ++i) {
            const std::string a = i < ref.size() ? ref[i] : "<none>";
            const std::string b = i < ours.size() ? ours[i] : "<none>";
            if (a != b) {
                diff += "    ref  " + a + "\n    ours " + b + "\n";
            }
        }
        CHECK_MSG(false, std::string(name) + ": listing differs from D3DX:\n" + diff);
    }
    const GlslResult g = toGlsl(dec.program);
    CHECK_MSG(g.ok(), std::string(name) + ": glsl: " + g.error);
    if (!g.ok()) {
        return {};
    }
    const SpirvResult s = compileGlsl(dec.program.version.stage, g.glsl);
    CHECK_MSG(s.ok(), std::string(name) + ": glslang:\n" + s.log + "\n" + g.glsl);
    if (!s.ok()) {
        return {};
    }
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::string vlog;
    tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* m) { vlog += m; });
    CHECK_MSG(tools.Validate(s.words), std::string(name) + ": spirv-val: " + vlog);
    return g.glsl;
}

bool contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

}  // namespace

TEST(shader_vs3_control_flow) {
    if (!D3DX::get().available()) {
        std::printf("  (skipped: d3dx9_39.dll not available)\n");
        return;
    }
    const std::string glsl = roundTrip("vs3_control_flow", R"(
vs_3_0
dcl_position v0
dcl_texcoord v1
dcl_position o0
dcl_texcoord o1
dcl_texcoord1 o2.xy
dcl_texcoord2 o2.zw
dcl_2d s0
defi i0, 4, 0, 1, 0
defb b1, true
def c0, 0, 1, 0.5, 2
mov r0, c0.x
loop aL, i0
  add r0, r0, c10[aL]
  break_gt r0.x, c0.w
endloop
rep i0
  add r0, r0, c0.y
endrep
mova a0.x, v1.x
mov r1, c20[a0.x]
setp_gt p0, r0, c0.x
(p0) add r1, r1, c0.y
(!p0.y) mul r1.y, r1.y, c0.z
if b0
  call l1
endif
callnz l1, b1
if_lt r0.x, c0.y
  mul r0, r0, c0.z
else
  mul r0, r0, c0.w
endif
texldl r2, v1, s0
m4x4 r3, v0, c30
m3x3 r4.xyz, v0, c40
sincos r5.xy, r0.x
lit r6, r0
dst r7, r0, r1
mov o0, r3
mov o1, r2
mov o2.xy, r4.xy
mov o2.zw, r5.xy
ret
label l1
  add r1, r1, c0.w
ret
)");
    CHECK(contains(glsl, "for (int rvk_k0"));
    CHECK(contains(glsl, "aL += "));
    CHECK(contains(glsl, "rvk_cRel(aL + 10)"));
    CHECK(contains(glsl, "rvk_cRel(a0.x + 20)"));
    CHECK(contains(glsl, "void l1()"));
    CHECK(contains(glsl, "if (p0.x) r1.x"));
    CHECK(contains(glsl, "if (!p0.y) r1.y"));
    CHECK(contains(glsl, "textureLod(sampler2D(t2D[pc.draw.tex[16]]"));  // vertex sampler s0 -> slot 16
    CHECK(contains(glsl, "rvk_out1 = o2;"));                               // TEXCOORD1 and TEXCOORD2 packed in o2
    CHECK(contains(glsl, "rvk_out2 = o2;"));
}

TEST(shader_ps3_textures) {
    if (!D3DX::get().available()) {
        std::printf("  (skipped: d3dx9_39.dll not available)\n");
        return;
    }
    const std::string glsl = roundTrip("ps3_textures", R"(
ps_3_0
dcl_texcoord v0
dcl_color v1
dcl vPos.xy
dcl vFace
dcl_2d s0
dcl_cube s1
dcl_volume s2
def c0, 0, 1, 0.5, -1
texld r0, v0, s0
texldp r1, v0, s0
texldb r2, v0, s0
texld r3, v0, s1
texld r4, v0, s2
texldd r5, v0, s0, r6, r7
dsx r6, v0
dsy r7, v0
add r0.xy, r0, vPos
if_gt vFace, c0.x
  mov r0.w, c0.y
endif
cmp r0, r0, c0.y, c0.x
lrp r8, c0.z, r0, r1
nrm r2.xyz, r1
pow r2.w, r1.x, c0.w
dp2add r3.x, r0, r1, c0.z
mad_sat r0, r0, -r1_abs, -r2
texkill r0
mov oC0, r0
mov oC1, v1
mov oDepth, c0.z
)");
    CHECK(contains(glsl, "textureProj(sampler2D"));
    CHECK(contains(glsl, "samplerCube(tCube[pc.draw.tex[1]]"));
    CHECK(contains(glsl, "sampler3D(t3D[pc.draw.tex[2]]"));
    CHECK(contains(glsl, "textureGrad("));
    CHECK(contains(glsl, "gl_FragCoord.xy - vec2(0.5)"));
    CHECK(contains(glsl, "gl_FrontFacing"));
    CHECK(contains(glsl, "discard;"));
    CHECK(contains(glsl, "gl_FragDepth"));
    CHECK(contains(glsl, "layout(location = 1) out vec4 rvk_oC1;"));
    CHECK(contains(glsl, "rvk_alphaTest(oC0.a)"));
}

TEST(shader_sm2) {
    if (!D3DX::get().available()) {
        std::printf("  (skipped: d3dx9_39.dll not available)\n");
        return;
    }
    const std::string vs = roundTrip("vs2", R"(
vs_2_0
dcl_position v0
dcl_texcoord v1
m4x4 oPos, v0, c0
mov oT0, v1
mov oD0, c4
mov oFog, c5.x
)");
    CHECK(contains(vs, "layout(location = 0) out vec4 rvk_out0;"));   // oT0 -> TEXCOORD0
    CHECK(contains(vs, "layout(location = 16) out vec4 rvk_out16;")); // oD0 -> COLOR0
    CHECK(contains(vs, "layout(location = 18) out vec4 rvk_out18;")); // oFog -> FOG
    const std::string ps = roundTrip("ps2", R"(
ps_2_0
dcl t0.xy
dcl v0
dcl_2d s0
texld r0, t0, s0
mul r0, r0, v0
mov oC0, r0
)");
    CHECK(contains(ps, "layout(location = 0) in vec4 t0;"));
    CHECK(contains(ps, "layout(location = 16) in vec4 v0;"));
}

TEST(shader_decode_errors) {
    const std::uint32_t truncated[] = {0xFFFF0300u, 0x02000001u, 0x800F0800u};  // mov without its source and without END
    CHECK(!decode(truncated).ok());
    const std::uint32_t sm1[] = {0xFFFF0101u, 0x0000FFFFu};
    CHECK(!decode(sm1).ok());
    CHECK(!decode({}).ok());
}

// the generated fixed-function shaders (d3d9/ffshader.h: the engine's debug drawing) equal what D3DX assembles from the
// listing they stand for, and translate
TEST(shader_fixed_function) {
    if (!D3DX::get().available()) {
        std::printf("  (skipped: d3dx9_39.dll not available)\n");
        return;
    }
    namespace ff = rvk::d3d9::ff;
    const auto same = [](const char* name, const std::vector<std::uint32_t>& ours, const std::string& source) {
        std::string errors;
        const auto ref = assemble(source, errors);
        CHECK_MSG(!ref.empty(), std::string(name) + ": " + errors);
        CHECK_MSG(normalizeListing(referenceListing(ref)) == normalizeListing(referenceListing(ours)),
                  std::string(name) + ":\n" + referenceListing(ours));
        CHECK(!roundTripTokens(name, ours).empty());
    };
    same("ff_vs_colour", ff::vertexShader(true), R"(vs_3_0
dcl_position v0
dcl_color v1
dcl_position o0
dcl_color o1
dp4 o0.x, v0, c248
dp4 o0.y, v0, c249
dp4 o0.z, v0, c250
dp4 o0.w, v0, c251
mov o1, v1
)");
    same("ff_vs_white", ff::vertexShader(false), R"(vs_3_0
dcl_position v0
dcl_position o0
dcl_color o1
def c247, 1, 1, 1, 1
dp4 o0.x, v0, c248
dp4 o0.y, v0, c249
dp4 o0.z, v0, c250
dp4 o0.w, v0, c251
mov o1, c247
)");
    same("ff_ps", ff::pixelShader(), R"(ps_3_0
dcl_color v0
mad oC0, v0, c220, c221
)");
}

TEST(fixed_function_stage0) {
    namespace ff = rvk::d3d9::ff;
    // DrawLineList: colour and alpha = SELECTARG1(TFACTOR)
    ff::Stage0 s{.colorOp = 2, .colorArg1 = 3, .colorArg2 = 2, .alphaOp = 2, .alphaArg1 = 3, .alphaArg2 = 2};
    ff::Linear l = ff::reduce(s, 0x80FF0000u);
    CHECK(l.mul == (std::array<float, 4>{0, 0, 0, 0}));
    CHECK(l.add[0] == 1.0f && l.add[1] == 0.0f && l.add[2] == 0.0f && l.add[3] > 0.50f && l.add[3] < 0.51f);
    // D3D9 defaults (MODULATE texture x diffuse, alpha = texture): the vertex colour, alpha 1
    l = ff::reduce(ff::Stage0{}, 0);
    CHECK(l.mul == (std::array<float, 4>{1, 1, 1, 0}) && l.add == (std::array<float, 4>{0, 0, 0, 1}));
    // MODULATE diffuse x TFACTOR
    l = ff::reduce(ff::Stage0{.colorOp = 4, .colorArg1 = 0, .colorArg2 = 3, .alphaOp = 4, .alphaArg1 = 0, .alphaArg2 = 3}, 0xFF808080u);
    CHECK(l.mul[0] > 0.50f && l.mul[0] < 0.51f && l.mul[3] == 1.0f && l.add[0] == 0.0f);
    // stage 0 disabled: the diffuse colour
    l = ff::reduce(ff::Stage0{.colorOp = 1}, 0);
    CHECK(l.mul == (std::array<float, 4>{1, 1, 1, 1}));
    // world * view * projection, transposed: a translation lands in the last column = row 3 of the result's w row
    const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float tr[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1};
    const auto m = ff::transposedProduct(tr, id, id);
    CHECK(m[3] == 5.0f && m[7] == 6.0f && m[11] == 7.0f && m[15] == 1.0f && m[12] == 0.0f);
    CHECK(ff::supportedFvf(0x002) && ff::supportedFvf(0x042) && !ff::supportedFvf(0x004) && !ff::supportedFvf(0x006));
}
