// The CPU shader evaluator and its backward slice (src/shader/sm_eval.*, M9 P2 rt-reflections.md 3.2): hand-written vs_3_0 /
// ps_3_0 assembled with d3dx9_39's D3DXAssembleShader (skipped when it is not installed), shaped like the engine's material
// shaders (uv transforms from g_vShaderData c232+, relative instance data, a pixel shader sampling s9 / s10).

#include <windows.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "shader/sm_eval.h"
#include "shader/sm_ir.h"
#include "test.h"

using namespace rvk::shader;
namespace ev = rvk::shader::eval;

namespace {

struct D3DXBuffer {  // ID3DXBuffer
    virtual long __stdcall QueryInterface(const void*, void**) = 0;
    virtual unsigned long __stdcall AddRef() = 0;
    virtual unsigned long __stdcall Release() = 0;
    virtual void* __stdcall GetBufferPointer() = 0;
    virtual unsigned long __stdcall GetBufferSize() = 0;
};
using AssembleFn = long(__stdcall*)(const char*, UINT, const void*, void*, DWORD, D3DXBuffer**, D3DXBuffer**);

AssembleFn assembler() {
    static AssembleFn fn = [] {
        HMODULE m = LoadLibraryW(L"d3dx9_39.dll");
        return m != nullptr ? reinterpret_cast<AssembleFn>(GetProcAddress(m, "D3DXAssembleShader")) : nullptr;
    }();
    return fn;
}

bool build(const char* src, Program& out) {
    D3DXBuffer* code = nullptr;
    D3DXBuffer* err = nullptr;
    const long hr = assembler()(src, static_cast<UINT>(std::strlen(src)), nullptr, nullptr, 0, &code, &err);
    if (err != nullptr) {
        std::printf("  assembler: %.*s\n", static_cast<int>(err->GetBufferSize()), static_cast<const char*>(err->GetBufferPointer()));
        err->Release();
    }
    if (hr < 0 || code == nullptr) return false;
    std::vector<std::uint32_t> tokens(code->GetBufferSize() / 4);
    std::memcpy(tokens.data(), code->GetBufferPointer(), tokens.size() * 4);
    code->Release();
    DecodeResult r = decode(tokens);
    if (!r.ok()) return false;
    out = std::move(r.program);
    return true;
}

bool approxEq(float a, float b) { return std::fabs(a - b) <= 1e-5f * (1.0f + std::fabs(b)); }

// the engine-like vertex shader: o2.xy = (uv + c242) * c241, o2.zw = uv * c236, o3.x/y = dp3((uv * c235, 1), c238 / c239),
// world position from relative instance rows c33[a0.w] ..
const char* kVs = R"(vs_3_0
dcl_position v0
dcl_normal v1
dcl_texcoord v5
dcl_texcoord4 v6
dcl_position o0
dcl_texcoord o2
dcl_texcoord1 o3
dcl_texcoord5 o4.xyz
def c4, 4, 1, -2, 0
mad r0.y, v6.x, c4.x, c4.y
mova a0.w, r0.y
dp4 r1.x, v0, c32[a0.w]
dp4 r1.y, v0, c33[a0.w]
dp4 r1.z, v0, c34[a0.w]
mov r1.w, c4.y
dp4 o0.x, r1, c0
dp4 o0.y, r1, c1
dp4 o0.z, r1, c2
dp4 o0.w, r1, c3
add r2.xy, c242, v5
mul o2.xy, r2, c241
mul o2.zw, v5.xyxy, c236.xyxy
mul r0.xy, c235, v5
mov r0.z, c4.y
dp3 o3.x, r0, c238
dp3 o3.y, r0, c239
mov o3.zw, c4.w
mov o4.xyz, v1
)";

// samples s10 (a normal map) at TEXCOORD0.zw, then s9 at TEXCOORD1.xy offset by the normal map's .x * c5.x
const char* kPs = R"(ps_3_0
dcl_texcoord v0
dcl_texcoord1 v1.xy
dcl_texcoord5 v2.xyz
dcl_2d s9
dcl_2d s10
def c0, 2, -1, 0.5, 0
texld r0, v0.zwzw, s10
mad r0.xy, r0, c0.x, c0.y
mad r1.xy, r0.x, c5.x, v1
texld r2, r1, s9
mul r2.xyz, r2, v2
mov oC0, r2
)";

}  // namespace

TEST(shader_eval_material_pair) {
    if (assembler() == nullptr) {
        std::printf("  (d3dx9_39.dll not installed: skipped)\n");
        return;
    }
    Program vs, ps;
    CHECK(build(kVs, vs));
    CHECK(build(kPs, ps));
    if (vs.instructions.empty() || ps.instructions.empty()) return;

    float vc[256][4]{};
    // instance 0 at c33..c35 (a0.w = floor(4 * 0 + 1 + 0.5) = 1 -> c33): translation (100, 200, 300)
    const float rows[3][4] = {{1, 0, 0, 100}, {0, 1, 0, 200}, {0, 0, 1, 300}};
    std::memcpy(vc[33], rows, sizeof(rows));
    for (int i = 0; i < 4; ++i) vc[i][i] = 1.0f;  // view-projection = identity
    const float c241[4] = {2, 3, 0, 0}, c242[4] = {0.5f, 0.25f, 0, 0}, c236[4] = {4, 4, 0, 0}, c235[4] = {2, 2, 0, 0};
    const float c238[4] = {0, -1, 0.1f, 0}, c239[4] = {1, 0, 0.2f, 0};  // a 90 degree rotation + offset
    std::memcpy(vc[241], c241, 16);
    std::memcpy(vc[242], c242, 16);
    std::memcpy(vc[236], c236, 16);
    std::memcpy(vc[235], c235, 16);
    std::memcpy(vc[238], c238, 16);
    std::memcpy(vc[239], c239, 16);
    float pc[224][4]{};
    pc[5][0] = 0.1f;

    ev::Env venv;
    venv.floats = vc;
    venv.floatCount = 256;
    venv.inputs[0] = {1, 2, 3, 1};
    venv.inputs[1] = {0, 0, 1, 1};
    venv.inputs[5] = {0.25f, 0.75f, 0, 1};
    venv.inputs[6] = {0, 0, 0, 1};
    const ev::Result vr = ev::run(vs, venv);
    CHECK_MSG(vr.ok, vr.error);
    CHECK(approxEq(vr.outputs[0][0], 101.0f) && approxEq(vr.outputs[0][1], 202.0f) && approxEq(vr.outputs[0][2], 303.0f));
    CHECK(approxEq(vr.outputs[2][0], (0.25f + 0.5f) * 2.0f) && approxEq(vr.outputs[2][1], (0.75f + 0.25f) * 3.0f));
    CHECK(approxEq(vr.outputs[2][2], 1.0f) && approxEq(vr.outputs[2][3], 3.0f));
    CHECK(approxEq(vr.outputs[3][0], -1.5f + 0.1f) && approxEq(vr.outputs[3][1], 0.5f + 0.2f));

    // the pixel shader with the vertex outputs: the s9 coordinate (texld s10 returns 0.75 -> x = 0.5 -> offset 0.05)
    ev::Env penv;
    penv.floats = pc;
    penv.floatCount = 224;
    penv.inputs[0] = vr.outputs[2];
    penv.inputs[1] = vr.outputs[3];
    penv.inputs[2] = vr.outputs[4];
    ev::Vec4 coord{};
    bool seen = false;
    const std::size_t at = ev::firstSample(ps, 9);
    CHECK(at == 3 && ev::firstSample(ps, 10) == 0 && ev::firstSample(ps, 11) == SIZE_MAX);
    penv.sample = [&](std::uint32_t s, const ev::Vec4& c, std::size_t i) {
        if (i == at) {
            coord = c;
            seen = true;
        }
        return s == 10 ? ev::Vec4{0.75f, 0.75f, 1.0f, 1.0f} : ev::Vec4{0.2f, 0.4f, 0.6f, 1.0f};
    };
    const ev::Result pr = ev::run(ps, penv);
    CHECK_MSG(pr.ok, pr.error);
    CHECK(seen && approxEq(coord[0], -1.4f + 0.05f) && approxEq(coord[1], 0.7f + 0.05f));
    CHECK(approxEq(pr.outputs[0][0], 0.2f * 0.0f) && approxEq(pr.outputs[0][2], 0.6f * 1.0f));

    // slices: s9's coordinate reads c5 (not the def'd c0? it does: c0 is def'd, so not listed) and inputs v0.zw, v1.xy
    const ev::Slice ps9 = ev::sliceSource(ps, at, 0, 0x3);
    CHECK(!ps9.flow && !ps9.relative);
    CHECK(ps9.floatConsts.size() == 1 && ps9.floatConsts[0] == 5);
    CHECK(ps9.inputs[0] == 0xC && ps9.inputs[1] == 0x3 && ps9.inputs[2] == 0);
    // the vertex outputs o2.zw + o3.xy: c235, c236, c238, c239 (not c241 / c242: o2.xy is not needed), input v5
    std::array<std::uint8_t, 16> outs{};
    outs[2] = 0xC;
    outs[3] = 0x3;
    const ev::Slice vsl = ev::sliceOutputs(vs, outs);
    CHECK(!vsl.relative && !vsl.flow);
    CHECK((vsl.floatConsts == std::vector<std::uint16_t>{235, 236, 238, 239}));
    CHECK(vsl.inputs[5] != 0 && vsl.inputs[0] == 0 && vsl.inputs[6] == 0);
    // the position output depends on the relative instance rows
    std::array<std::uint8_t, 16> pos{};
    pos[0] = 0xF;
    const ev::Slice psl = ev::sliceOutputs(vs, pos);
    CHECK(psl.relative && psl.inputs[0] != 0 && psl.inputs[6] != 0);
}

TEST(shader_eval_flow_control) {
    if (assembler() == nullptr) return;
    Program vs;
    CHECK(build(R"(vs_3_0
dcl_position v0
dcl_position o0
dcl_texcoord o1
defi i0, 3, 0, 0, 0
def c0, 0, 1, 0, 0
mov r0, c0.x
rep i0
add r0.x, r0.x, c0.y
endrep
if b0
mov r0.y, c0.y
else
mov r0.y, -c0.y
endif
mov o0, v0
mov o1, r0
)",
                vs));
    if (vs.instructions.empty()) return;
    std::uint32_t bools[16]{};
    ev::Env env;
    env.bools = bools;
    ev::Result r = ev::run(vs, env);
    CHECK_MSG(r.ok, r.error);
    CHECK(approxEq(r.outputs[1][0], 3.0f) && approxEq(r.outputs[1][1], -1.0f));
    bools[0] = 1;
    r = ev::run(vs, env);
    CHECK(r.ok && approxEq(r.outputs[1][1], 1.0f));
    std::array<std::uint8_t, 16> outs{};
    outs[1] = 0x3;
    const ev::Slice s = ev::sliceOutputs(vs, outs);
    CHECK(s.flow && s.usesInts);
}
