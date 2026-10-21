// fx_2_0 parser tests: a hand-built effect binary (no D3DX), the preshader VM opcode by opcode, an effect and shaders
// compiled in-test with d3dx9_39's CPU-only compilers (D3DXCreateEffectCompiler, D3DXCompileShader) whose constant tables
// are compared with D3DXGetShaderConstantTable, and the captured game effects in <repo>/corpus (skipped when absent).

#include <windows.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "fx/effect_binary.h"
#include "shader/sm_ir.h"
#include "test.h"

using namespace rvk::fx;

namespace {

// ---------------------------------------------------------------------------------------------------------------
// d3dx9_39 (CPU functions only)
// ---------------------------------------------------------------------------------------------------------------

struct D3DXBuffer {  // ID3DXBuffer
    virtual long __stdcall QueryInterface(const void*, void**) = 0;
    virtual unsigned long __stdcall AddRef() = 0;
    virtual unsigned long __stdcall Release() = 0;
    virtual void* __stdcall GetBufferPointer() = 0;
    virtual unsigned long __stdcall GetBufferSize() = 0;
};

struct D3DXConstantTableDesc {
    const char* Creator;
    DWORD Version;
    UINT Constants;
};

struct D3DXConstantDesc {
    const char* Name;
    UINT RegisterSet, RegisterIndex, RegisterCount, Class, Type, Rows, Columns, Elements, StructMembers, Bytes;
    const void* DefaultValue;
};

using Handle = const char*;

struct D3DXConstantTable {  // ID3DXConstantTable (prefix of the vtable)
    virtual long __stdcall QueryInterface(const void*, void**) = 0;
    virtual unsigned long __stdcall AddRef() = 0;
    virtual unsigned long __stdcall Release() = 0;
    virtual void* __stdcall GetBufferPointer() = 0;
    virtual unsigned long __stdcall GetBufferSize() = 0;
    virtual long __stdcall GetDesc(D3DXConstantTableDesc*) = 0;
    virtual long __stdcall GetConstantDesc(Handle, D3DXConstantDesc*, UINT*) = 0;
    virtual UINT __stdcall GetSamplerIndex(Handle) = 0;
    virtual Handle __stdcall GetConstant(Handle, UINT) = 0;
    virtual Handle __stdcall GetConstantByName(Handle, const char*) = 0;
    virtual Handle __stdcall GetConstantElement(Handle, UINT) = 0;
};

struct D3DX {
    using CreateEffectCompilerFn = long(__stdcall*)(const char*, UINT, const void*, void*, DWORD, void**, D3DXBuffer**);
    using GetConstantTableFn = long(__stdcall*)(const DWORD*, D3DXConstantTable**);
    using CompileShaderFn = long(__stdcall*)(const char*, UINT, const void*, void*, const char*, const char*, DWORD,
                                             D3DXBuffer**, D3DXBuffer**, D3DXConstantTable**);
    CreateEffectCompilerFn createEffectCompilerFn = nullptr;
    GetConstantTableFn getConstantTableFn = nullptr;
    CompileShaderFn compileShaderFn = nullptr;

    static const D3DX& get() {
        static const D3DX d = [] {
            D3DX x;
            if (HMODULE m = LoadLibraryW(L"d3dx9_39.dll")) {
                x.createEffectCompilerFn =
                    reinterpret_cast<CreateEffectCompilerFn>(GetProcAddress(m, "D3DXCreateEffectCompiler"));
                x.getConstantTableFn =
                    reinterpret_cast<GetConstantTableFn>(GetProcAddress(m, "D3DXGetShaderConstantTable"));
                x.compileShaderFn = reinterpret_cast<CompileShaderFn>(GetProcAddress(m, "D3DXCompileShader"));
            }
            return x;
        }();
        return d;
    }
    bool available() const {
        return createEffectCompilerFn != nullptr && getConstantTableFn != nullptr && compileShaderFn != nullptr;
    }

    static std::string takeErrors(D3DXBuffer*& err) {
        std::string s;
        if (err != nullptr) {
            s.assign(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
            err->Release();
            err = nullptr;
        }
        return s;
    }

    // .fx source -> fx_2_0 binary (ID3DXEffectCompiler::CompileEffect = vtable slot 59)
    std::vector<std::byte> compileEffect(const std::string& src, std::string* errors = nullptr) const {
        void* compiler = nullptr;
        D3DXBuffer* err = nullptr;
        long hr = createEffectCompilerFn(src.c_str(), static_cast<UINT>(src.size()), nullptr, nullptr, 0, &compiler, &err);
        std::string e = takeErrors(err);
        if (hr < 0 || compiler == nullptr) {
            if (errors != nullptr) {
                *errors = e;
            }
            return {};
        }
        using CompileFn = long(__stdcall*)(void*, DWORD, D3DXBuffer**, D3DXBuffer**);
        auto** vtbl = *reinterpret_cast<void***>(compiler);
        D3DXBuffer* out = nullptr;
        hr = reinterpret_cast<CompileFn>(vtbl[59])(compiler, 0, &out, &err);
        e += takeErrors(err);
        reinterpret_cast<D3DXBuffer*>(compiler)->Release();
        if (errors != nullptr) {
            *errors = e;
        }
        if (hr < 0 || out == nullptr) {
            return {};
        }
        const auto* p = static_cast<const std::byte*>(out->GetBufferPointer());
        std::vector<std::byte> r(p, p + out->GetBufferSize());
        out->Release();
        return r;
    }

    std::vector<std::uint32_t> compileShader(const std::string& src, const char* entry, const char* profile,
                                             std::string* errors = nullptr) const {
        D3DXBuffer* code = nullptr;
        D3DXBuffer* err = nullptr;
        const long hr = compileShaderFn(src.c_str(), static_cast<UINT>(src.size()), nullptr, nullptr, entry, profile, 0,
                                      &code, &err, nullptr);
        const std::string e = takeErrors(err);
        if (errors != nullptr) {
            *errors = e;
        }
        if (hr < 0 || code == nullptr) {
            return {};
        }
        std::vector<std::uint32_t> t(code->GetBufferSize() / 4);
        std::memcpy(t.data(), code->GetBufferPointer(), t.size() * 4);
        code->Release();
        return t;
    }
};

// Compares our constant (recursively) with D3DX's; returns the number of differences (printed).
// Member defaults: D3DX hands out DefaultValue pointers for struct members even when the constant has no default
// (a running offset from 0); they are compared only when the top-level constant has a default.
int compareConstant(D3DXConstantTable* t, Handle h, const Constant& c, const std::string& path, bool topDefault) {
    D3DXConstantDesc d{};
    UINT n = 1;
    if (t->GetConstantDesc(h, &d, &n) < 0) {
        std::printf("  %s: GetConstantDesc failed\n", path.c_str());
        return 1;
    }
    int bad = 0;
    auto check = [&](const char* what, std::uint32_t mine, std::uint32_t ref) {
        if (mine != ref) {
            std::printf("  %s %s: ours %u d3dx %u\n", path.c_str(), what, mine, ref);
            ++bad;
        }
    };
    if (c.name != (d.Name != nullptr ? d.Name : "")) {
        std::printf("  %s: name differs\n", path.c_str());
        ++bad;
    }
    check("registerSet", static_cast<std::uint32_t>(c.registerSet), d.RegisterSet);
    check("registerIndex", c.registerIndex, d.RegisterIndex);
    check("registerCount", c.registerCount, d.RegisterCount);
    check("class", static_cast<std::uint32_t>(c.cls), d.Class);
    check("type", static_cast<std::uint32_t>(c.type), d.Type);
    check("rows", c.rows, d.Rows);
    check("columns", c.columns, d.Columns);
    check("elements", c.elements, d.Elements);
    check("structMembers", c.structMembers, d.StructMembers);
    check("bytes", c.bytes, d.Bytes);
    if (topDefault) {
        check("hasDefault", c.defaultValue.empty() ? 0u : 1u, d.DefaultValue != nullptr ? 1u : 0u);
    }
    if (topDefault && d.DefaultValue != nullptr && !c.defaultValue.empty() &&
        std::memcmp(d.DefaultValue, c.defaultValue.data(), c.defaultValue.size() * 4) != 0) {
        std::printf("  %s: default value differs\n", path.c_str());
        ++bad;
    }
    const bool elements = d.Elements > 1;
    const std::uint32_t count = elements ? d.Elements : d.StructMembers;
    check("members", static_cast<std::uint32_t>(c.members.size()), count);
    for (std::uint32_t i = 0; i < count && i < c.members.size(); ++i) {
        Handle m = elements ? t->GetConstantElement(h, i) : t->GetConstant(h, i);
        if (m == nullptr) {
            ++bad;
            continue;
        }
        bad += compareConstant(t, m, c.members[i], std::format("{}{}{}", path, elements ? "[" : ".",
                                                                elements ? std::to_string(i) + "]" : c.members[i].name),
                               topDefault);
    }
    return bad;
}

// Our CTAB of `tokens` vs D3DXGetShaderConstantTable. Returns the number of differences; `constants` counts the
// compared constants (incl. elements and members).
int compareCtab(const std::vector<std::uint32_t>& tokens, const ConstantTable& mine, int* constants = nullptr) {
    D3DXConstantTable* t = nullptr;
    if (D3DX::get().getConstantTableFn(reinterpret_cast<const DWORD*>(tokens.data()), &t) < 0 || t == nullptr) {
        if (mine.present) {
            std::printf("  D3DX found no constant table\n");
            return 1;
        }
        return 0;
    }
    D3DXConstantTableDesc desc{};
    t->GetDesc(&desc);
    int bad = 0;
    if (desc.Constants != mine.constants.size() || desc.Version != mine.version ||
        std::string(desc.Creator != nullptr ? desc.Creator : "") != mine.creator) {
        std::printf("  CTAB header differs\n");
        ++bad;
    }
    for (UINT i = 0; i < desc.Constants && i < mine.constants.size(); ++i) {
        D3DXConstantDesc top{};
        UINT one = 1;
        Handle h = t->GetConstant(nullptr, i);
        t->GetConstantDesc(h, &top, &one);
        bad += compareConstant(t, h, mine.constants[i], mine.constants[i].name, top.DefaultValue != nullptr);
    }
    if (constants != nullptr) {
        std::function<int(const Constant&)> countAll = [&](const Constant& c) {
            int n = 1;
            for (const Constant& m : c.members) {
                n += countAll(m);
            }
            return n;
        };
        for (const Constant& c : mine.constants) {
            *constants += countAll(c);
        }
    }
    t->Release();
    return bad;
}

// A preshader's CTAB through D3DX: wrapped into a vs_3_0 token stream.
int comparePreshaderCtab(const Preshader& p) {
    const auto body = findComment(p.tokens, fourcc('C', 'T', 'A', 'B'));
    if (body.empty()) {
        return 0;
    }
    std::vector<std::uint32_t> fake{0xFFFE0300u, 0xFFFEu | (static_cast<std::uint32_t>(body.size() + 1) << 16),
                                    fourcc('C', 'T', 'A', 'B')};
    fake.insert(fake.end(), body.begin(), body.end());
    fake.push_back(0x0000FFFFu);
    return compareCtab(fake, p.inputs);
}

bool approx(double a, double b, double tol = 1e-4) { return std::fabs(a - b) <= tol * std::max(1.0, std::fabs(b)); }

std::span<const std::byte> bytesOf(const std::vector<std::uint32_t>& v) { return std::as_bytes(std::span(v)); }

// ---------------------------------------------------------------------------------------------------------------
// hand-built binaries
// ---------------------------------------------------------------------------------------------------------------

std::vector<std::uint32_t> comment(std::uint32_t tag, const std::vector<std::uint32_t>& body) {
    std::vector<std::uint32_t> t{0xFFFEu | (static_cast<std::uint32_t>(body.size() + 1) << 16), tag};
    t.insert(t.end(), body.begin(), body.end());
    return t;
}

void append(std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) { a.insert(a.end(), b.begin(), b.end()); }

struct CtabEntry {
    std::string name;
    std::uint16_t set, index, count, cls, type, rows, columns;
};

// A CTAB comment (header, constant infos, type infos, strings).
std::vector<std::uint32_t> buildCtab(const char* target, std::uint32_t version, const std::vector<CtabEntry>& entries) {
    std::vector<std::byte> b;
    auto put32 = [&b](std::uint32_t v) {
        const auto* p = reinterpret_cast<const std::byte*>(&v);
        b.insert(b.end(), p, p + 4);
    };
    auto put16 = [&b](std::uint16_t v) {
        const auto* p = reinterpret_cast<const std::byte*>(&v);
        b.insert(b.end(), p, p + 2);
    };
    const auto n = static_cast<std::uint32_t>(entries.size());
    const std::uint32_t infoOff = 28, typeOff = infoOff + 20 * n, strOff = typeOff + 16 * n;
    std::string strings = std::string("rvk test") + '\0' + target + '\0';
    std::vector<std::uint32_t> nameOffs;
    for (const CtabEntry& e : entries) {
        nameOffs.push_back(strOff + static_cast<std::uint32_t>(strings.size()));
        strings += e.name + '\0';
    }
    put32(28);
    put32(strOff);
    put32(version);
    put32(n);
    put32(infoOff);
    put32(0);
    put32(strOff + 9);
    for (std::uint32_t i = 0; i < n; ++i) {
        put32(nameOffs[i]);
        put16(entries[i].set);
        put16(entries[i].index);
        put16(entries[i].count);
        put16(0);
        put32(typeOff + 16 * i);
        put32(0);
    }
    for (const CtabEntry& e : entries) {
        put16(e.cls);
        put16(e.type);
        put16(e.rows);
        put16(e.columns);
        put16(1);
        put16(0);
        put32(0);
    }
    for (char c : strings) {
        b.push_back(static_cast<std::byte>(c));
    }
    while (b.size() % 4 != 0) {
        b.push_back(std::byte{0});
    }
    std::vector<std::uint32_t> body(b.size() / 4);
    std::memcpy(body.data(), b.data(), b.size());
    return comment(fourcc('C', 'T', 'A', 'B'), body);
}

struct Opd {
    std::uint32_t table, offset;
    bool relative = false;
    std::uint32_t indexTable = 0, indexOffset = 0;
};

void presIns(std::vector<std::uint32_t>& code, PresOp op, std::uint32_t comps, bool scalar,
             const std::vector<Opd>& inputs, const Opd& out) {
    code.push_back((scalar ? 0x80000000u : 0u) | (static_cast<std::uint32_t>(op) << 20) | comps);
    code.push_back(static_cast<std::uint32_t>(inputs.size()));
    auto operand = [&code](const Opd& o) {
        if (o.relative) {
            append(code, {1, o.indexTable, o.indexOffset});
        } else {
            code.push_back(0);
        }
        append(code, {o.table, o.offset});
    };
    for (const Opd& in : inputs) {
        operand(in);
    }
    operand(out);
}

// version + CLIT + FXLC (+ CTAB) + END
std::vector<std::uint32_t> buildPreshader(std::uint32_t version, const std::vector<double>& literals,
                                          std::uint32_t count, const std::vector<std::uint32_t>& code,
                                          const std::vector<std::uint32_t>& ctab = {}) {
    std::vector<std::uint32_t> t{version};
    if (!literals.empty()) {
        std::vector<std::uint32_t> clit{static_cast<std::uint32_t>(literals.size())};
        for (double d : literals) {
            const auto u = std::bit_cast<std::uint64_t>(d);
            append(clit, {static_cast<std::uint32_t>(u), static_cast<std::uint32_t>(u >> 32)});
        }
        append(t, comment(fourcc('C', 'L', 'I', 'T'), clit));
    }
    std::vector<std::uint32_t> fxlc{count};
    append(fxlc, code);
    append(t, comment(fourcc('F', 'X', 'L', 'C'), fxlc));
    append(t, ctab);
    t.push_back(0x0000FFFFu);
    return t;
}

constexpr std::uint32_t kLit = 1, kIn = 2, kOut = 4, kOutB = 5, kOutI = 6, kTemp = 7;

// fx_2_0 writer for the hand-built effect: "unstructured" area (names, types, values) + structured part.
struct FxWriter {
    std::vector<std::uint32_t> u{0};  // offset 0 unused (Wine: a zero offset is "no name")

    std::uint32_t off() const { return static_cast<std::uint32_t>(u.size() * 4); }
    std::uint32_t string(const std::string& s) {
        const std::uint32_t o = off();
        u.push_back(static_cast<std::uint32_t>(s.size() + 1));
        std::vector<std::uint32_t> w((s.size() + 4) / 4, 0);
        std::memcpy(w.data(), s.data(), s.size());
        append(u, w);
        return o;
    }
    std::uint32_t type(ParamType t, ParamClass c, const std::string& name, const std::string& semantic,
                       std::uint32_t rows = 1, std::uint32_t columns = 1) {
        const std::uint32_t n = string(name);
        const std::uint32_t s = semantic.empty() ? 0 : string(semantic);
        const std::uint32_t o = off();
        append(u, {static_cast<std::uint32_t>(t), static_cast<std::uint32_t>(c), n, s, 0});
        if (c == ParamClass::Vector) {
            append(u, {columns, rows});
        } else if (c != ParamClass::Object) {
            append(u, {rows, columns});
        }
        return o;
    }
    std::uint32_t value(const std::vector<std::uint32_t>& v) {
        const std::uint32_t o = off();
        append(u, v);
        return o;
    }
    std::uint32_t intType() { return type(ParamType::Int, ParamClass::Scalar, "", ""); }
};

std::uint32_t f2u(float f) { return std::bit_cast<std::uint32_t>(f); }

// vs_2_0 { mov oPos, c0; mov oD0, c1 } with a CTAB ("color" -> c0) and a preshader writing c1.x = k * 2
std::vector<std::uint32_t> handShader() {
    std::vector<std::uint32_t> t{0xFFFE0200u};
    append(t, buildCtab("vs_2_0", 0xFFFE0200u, {{"color", 2, 0, 1, 1, 3, 1, 4}}));
    std::vector<std::uint32_t> code;
    presIns(code, PresOp::Mul, 1, false, {{kIn, 0}, {kLit, 0}}, {kOut, 4});
    append(t, comment(fourcc('P', 'R', 'E', 'S'),
                      buildPreshader(0x46580201u, {2, 0, 0, 0}, 1, code,
                                     buildCtab("fx_2_0", 0x46580201u, {{"k", 2, 0, 1, 0, 3, 1, 1}}))));
    append(t, {0x02000001u, 0xC00F0000u, 0xA0E40000u, 0x02000001u, 0xD00F0000u, 0xA0E40001u, 0x0000FFFFu});
    return t;
}

std::vector<std::byte> handEffect() {
    FxWriter w;
    std::vector<std::uint32_t> s;  // structured
    // parameters
    append(s, {4, 1, 0, 4});  // 4 parameters, 1 technique, shader count, 4 objects
    // [0] float4 color : COLOR = {1,2,3,4} < int anno = 7; >
    append(s, {w.type(ParamType::Float, ParamClass::Vector, "color", "COLOR", 1, 4),
               w.value({f2u(1), f2u(2), f2u(3), f2u(4)}), 0, 1});
    append(s, {w.type(ParamType::Int, ParamClass::Scalar, "anno", ""), w.value({7})});
    // [1] texture tex (object 1)
    append(s, {w.type(ParamType::Texture, ParamClass::Object, "tex", ""), w.value({1}), 0, 0});
    // [2] sampler2D sam = sampler_state { Texture = <tex> (object 2); AddressU = 3; }
    append(s, {w.type(ParamType::Sampler2D, ParamClass::Object, "sam", ""),
               w.value({2, 0xA4, 256, w.type(ParamType::Texture, ParamClass::Object, "", ""), w.value({2}), 0xA5, 256,
                        w.intType(), w.value({3})}),
               0, 0});
    // [3] float k = 2
    append(s, {w.type(ParamType::Float, ParamClass::Scalar, "k", ""), w.value({f2u(2)}), 0, 0});
    // technique T { pass P { ZEnable = 0; VertexShader = <object 3>; AlphaRef = k * 10; } }
    append(s, {w.string("T"), 0, 1});
    append(s, {w.string("P"), 0, 3});
    append(s, {0x00, 0, w.intType(), w.value({0})});
    append(s, {0x92, 0, w.type(ParamType::VertexShader, ParamClass::Object, "", ""), w.value({3})});
    append(s, {0x0A, 0, w.intType(), w.value({0})});
    // object data: none inline; resources: 3
    append(s, {0, 3});
    auto resource = [&s](std::uint32_t tech, std::uint32_t index, std::uint32_t element, std::uint32_t state,
                         std::uint32_t usage, const std::vector<std::uint32_t>& blob, std::uint32_t bytes) {
        append(s, {tech, index, element, state, usage, bytes});
        append(s, blob);
    };
    resource(kNone, 2, kNone, 0, 1, {0x00786574u}, 4);  // "tex\0"
    const auto vs = handShader();
    resource(0, 0, kNone, 1, 0, vs, static_cast<std::uint32_t>(vs.size() * 4));
    std::vector<std::uint32_t> code;
    presIns(code, PresOp::Mul, 1, false, {{kIn, 0}, {kLit, 0}}, {kOut, 0});
    const auto expr = buildPreshader(0x46580200u, {10, 0, 0, 0}, 1, code,
                                     buildCtab("fx_2_0", 0x46580200u, {{"k", 2, 0, 1, 0, 3, 1, 1}}));
    resource(0, 0, kNone, 2, 0, expr, static_cast<std::uint32_t>(expr.size() * 4));

    std::vector<std::uint32_t> all{kEffectMagic, w.off()};
    append(all, w.u);
    append(all, s);
    std::vector<std::byte> b(all.size() * 4);
    std::memcpy(b.data(), all.data(), b.size());
    return b;
}

std::filesystem::path corpusDir() {
    wchar_t env[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"RVK_CORPUS", env, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return std::filesystem::path(env);
    }
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "corpus";
}

std::vector<std::byte> readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::vector<char> c((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::byte> b(c.size());
    std::memcpy(b.data(), c.data(), c.size());
    return b;
}

float reg(const ConstantRegisters& r, std::size_t i) { return i < r.f.size() ? r.f[i] : -12345.0f; }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------

TEST(fx_state_table) {
    const auto t = stateTable();
    CHECK(t.size() == 179);
    CHECK(stateInfo(0x00)->cls == StateClass::RenderState && stateInfo(0x00)->op == 7);  // D3DRS_ZENABLE
    CHECK(std::string(stateInfo(0x49)->name) == "D3DRS_COLORWRITEENABLE" && stateInfo(0x49)->op == 168);
    CHECK(std::string(stateInfo(0x66)->name) == "D3DRS_BLENDOPALPHA" && stateInfo(0x66)->op == 209);
    CHECK(stateInfo(0x67)->cls == StateClass::TextureStage && stateInfo(0x67)->op == 1);  // D3DTSS_COLOROP
    CHECK(stateInfo(0x7d)->cls == StateClass::Transform && stateInfo(0x7d)->op == 256);  // D3DTS_WORLD
    CHECK(stateInfo(0x92)->cls == StateClass::VertexShader && stateInfo(0x93)->cls == StateClass::PixelShader);
    CHECK(stateInfo(0xa4)->cls == StateClass::Texture);
    CHECK(stateInfo(0xa5)->cls == StateClass::SamplerState && stateInfo(0xa5)->op == 1);  // D3DSAMP_ADDRESSU
    CHECK(stateInfo(0xab)->op == 7);                                                       // D3DSAMP_MIPFILTER
    CHECK(stateInfo(0xb2)->cls == StateClass::SetSampler);
    CHECK(stateInfo(0xb3) == nullptr);
    CHECK(stateValueName(0x08, 1) == "D3DCULL_NONE");
    CHECK(stateValueName(0x49, 9) == "RA");
    CHECK(stateValueName(0xa9, 2) == "D3DTEXF_LINEAR");
    CHECK(stateValueName(0x10, 5).empty());  // FOGCOLOR: no symbolic values
}

TEST(fx_hand_built_effect) {
    const auto bin = handEffect();
    const EffectResult r = parseEffect(bin);
    CHECK_MSG(r.ok(), r.error);
    if (!r.ok()) {
        return;
    }
    const Effect& e = r.effect;
    CHECK(e.parameters.size() == 4 && e.techniques.size() == 1 && e.objects.size() == 4);
    const Parameter& color = e.parameters[0];
    CHECK(color.name == "color" && color.semantic == "COLOR" && color.cls == ParamClass::Vector);
    CHECK(color.rows == 1 && color.columns == 4 && color.data.size() == 16 && color.floatAt(2) == 3.0f);
    CHECK(color.annotations.size() == 1 && color.annotations[0].dwordAt(0) == 7);
    CHECK(e.findParameter("color@anno") == &color.annotations[0]);
    CHECK(e.findParameter("nope") == nullptr && e.findParameter("color[0]") == nullptr);
    const Parameter& sam = e.parameters[2];
    CHECK(sam.type == ParamType::Sampler2D && sam.samplerStates.size() == 2);
    if (sam.samplerStates.size() == 2) {
        CHECK(sam.samplerStates[0].kind == StateKind::ParamRef && sam.samplerStates[0].reference == "tex");
        CHECK(sam.samplerStates[0].referenceParam == 1);
        CHECK(sam.samplerStates[1].kind == StateKind::Constant && sam.samplerStates[1].value.dwordAt(0) == 3);
    }
    const Pass& pass = e.techniques[0].passes[0];
    CHECK(e.techniques[0].name == "T" && pass.name == "P" && pass.states.size() == 3);
    if (pass.states.size() != 3) {
        return;
    }
    CHECK(pass.states[0].info()->op == 7 && pass.states[0].value.dwordAt(0) == 0);
    const State& vsState = pass.states[1];
    CHECK(vsState.kind == StateKind::Constant && vsState.shader == 0 && e.shaders.size() == 1);
    if (e.shaders.size() == 1) {
        const Shader& vs = e.shaders[0];
        CHECK(vs.stage == rvk::shader::Stage::Vertex && vs.major == 2 && vs.objectId == 3);
        CHECK(rvk::shader::decode(vs.tokens).ok());
        CHECK(vs.constants.present && vs.constants.target == "vs_2_0" && vs.constants.creator == "rvk test");
        CHECK(vs.constants.constants.size() == 1 && vs.constants.constants[0].name == "color");
        CHECK(vs.preshader != kNone);
        ParameterValues values = ParameterValues::defaults(e);
        ConstantRegisters regs = evaluateShaderConstants(e, vs, values);
        CHECK(reg(regs, 0) == 1.0f && reg(regs, 3) == 4.0f && reg(regs, 4) == 4.0f);  // c1.x = k * 2
        const float k = 5.0f;
        values.setFloats(e, "k", std::span(&k, 1));
        regs = evaluateShaderConstants(e, vs, values);
        CHECK(reg(regs, 4) == 10.0f);
        const State& alphaRef = pass.states[2];
        CHECK(alphaRef.kind == StateKind::Expression && alphaRef.expression != kNone);
        if (alphaRef.expression != kNone) {
            const auto v = evaluateExpression(e, e.preshaders[alphaRef.expression], values, ParamType::Int, 1);
            CHECK(v.size() == 1 && v[0] == 50);
        }
    }
    const std::string dump = dumpEffect(e);
    CHECK(dump.find("D3DRS_ZENABLE") != std::string::npos && dump.find("<tex>") != std::string::npos);

    // every truncation fails cleanly, as does a damaged header
    int truncatedOk = 0;
    for (std::size_t n = 0; n < bin.size(); n += 4) {
        truncatedOk += parseEffect(std::span(bin).first(n)).ok() ? 1 : 0;
    }
    CHECK(truncatedOk == 0);
    auto bad = bin;
    bad[0] = std::byte{0};
    CHECK(!parseEffect(bad).ok());
    const std::string src = "float4 x; technique T { pass P { } }";
    const EffectResult text = parseEffect(std::as_bytes(std::span(src)));
    CHECK(!text.ok() && text.error.find("source") != std::string::npos);
}

TEST(fx_preshader_ops) {
    std::vector<std::uint32_t> code;
    int n = 0;
    auto ins = [&](PresOp op, std::uint32_t comps, bool scalar, const std::vector<Opd>& in, const Opd& out) {
        presIns(code, op, comps, scalar, in, out);
        ++n;
    };
    // c0 = (0.25, -2.25, 3, 0.5), c1 = (1, 2, 3, 4); literals (10, 20, 1, 3.6)
    ins(PresOp::Rcp, 1, false, {{kIn, 0}}, {kOut, 0});                       // oc0.x = 4
    ins(PresOp::Rsq, 1, false, {{kIn, 3}}, {kOut, 1});                       // oc0.y = 1/sqrt(0.5)
    ins(PresOp::Exp, 1, false, {{kIn, 2}}, {kOut, 2});                       // oc0.z = 8
    ins(PresOp::Log, 1, false, {{kIn, 7}}, {kOut, 3});                       // oc0.w = 2
    ins(PresOp::Frc, 1, false, {{kIn, 1}}, {kOut, 4});                       // oc1.x = 0.75
    ins(PresOp::Neg, 1, false, {{kIn, 0}}, {kOut, 5});                       // oc1.y = -0.25
    ins(PresOp::Min, 2, false, {{kIn, 2}, {kIn, 6}}, {kOut, 6});             // oc1.zw = (3, 0.5)
    ins(PresOp::Max, 2, false, {{kIn, 0}, {kIn, 4}}, {kOut, 8});             // oc2.xy = (1, 2)
    ins(PresOp::Lt, 1, false, {{kIn, 0}, {kIn, 4}}, {kOut, 10});             // oc2.z = 1
    ins(PresOp::Ge, 1, false, {{kIn, 1}, {kIn, 5}}, {kOut, 11});             // oc2.w = 0
    ins(PresOp::Cmp, 1, false, {{kIn, 1}, {kLit, 0}, {kLit, 1}}, {kOut, 12});   // oc3.x = 20
    ins(PresOp::Movc, 1, false, {{kIn, 0}, {kLit, 0}, {kLit, 1}}, {kOut, 13});  // oc3.y = 10
    ins(PresOp::Sin, 1, false, {{kIn, 2}}, {kOut, 14});                      // oc3.z = sin 3
    ins(PresOp::Cos, 1, false, {{kIn, 2}}, {kOut, 15});                      // oc3.w = cos 3
    ins(PresOp::Dot, 4, false, {{kIn, 0}, {kIn, 4}}, {kOut, 16});            // oc4.x = 6.75
    ins(PresOp::Mul, 3, true, {{kIn, 0}, {kIn, 5}}, {kOut, 17});             // oc4.yzw = 0.25 * (2, 3, 4)
    ins(PresOp::Mov, 1, false, {{kLit, 2}}, {kTemp, 0});                     // r0.x = 1
    ins(PresOp::Mov, 4, false, {{kIn, 0, true, kTemp, 0}}, {kOut, 20});      // oc5 = c[r0.x] = c1
    ins(PresOp::Add, 1, false, {{kIn, 0}, {kIn, 4}}, {kOut, 24});            // oc6.x = 1.25
    ins(PresOp::Mov, 1, false, {{kIn, 0}}, {kOutB, 8});                      // ob2 = true (offset in components)
    ins(PresOp::Mov, 1, false, {{kLit, 3}}, {kOutI, 4});                     // oi1.x = 4
    ins(PresOp::Atan2, 1, false, {{kIn, 0}, {kIn, 4}}, {kOut, 25});          // oc6.y = atan2(0.25, 1)
    ins(PresOp::DotSwiz, 1, false, {{kIn, 0}, {kIn, 1}, {kIn, 2}, {kIn, 4}, {kIn, 5}, {kIn, 6}}, {kOut, 26});
    const auto blob = buildPreshader(0x46580200u, {10, 20, 1, 3.6}, static_cast<std::uint32_t>(n), code);
    const PreshaderResult pr = parsePreshader(blob);
    CHECK_MSG(pr.ok(), pr.error);
    if (!pr.ok()) {
        return;
    }
    const Preshader& p = pr.preshader;
    CHECK(p.code.size() == static_cast<std::size_t>(n) && p.literals.size() == 4 && p.version == 0x46580200u);
    CHECK(p.outputRegisters == 7 && p.outBoolRegisters == 3 && p.outIntRegisters == 2 && p.tempRegisters == 1);
    PresRegisters r;
    r.input = {0.25f, -2.25f, 3.0f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f};
    runPreshader(p, r);
    const auto& o = r.output;
    CHECK(o.size() >= 28);
    if (o.size() < 28) {
        return;
    }
    CHECK(o[0] == 4.0f && approx(o[1], 1.0 / std::sqrt(0.5)) && o[2] == 8.0f && o[3] == 2.0f);
    CHECK(o[4] == 0.75f && o[5] == -0.25f && o[6] == 3.0f && o[7] == 0.5f);
    CHECK(o[8] == 1.0f && o[9] == 2.0f && o[10] == 1.0f && o[11] == 0.0f);
    CHECK(o[12] == 20.0f && o[13] == 10.0f && approx(o[14], std::sin(3.0)) && approx(o[15], std::cos(3.0)));
    CHECK(o[16] == 6.75f && o[17] == 0.5f && o[18] == 0.75f && o[19] == 1.0f);
    CHECK(o[20] == 1.0f && o[21] == 2.0f && o[22] == 3.0f && o[23] == 4.0f);
    CHECK(o[24] == 1.25f && approx(o[25], std::atan2(0.25, 1.0)));
    CHECK(approx(o[26], 0.25 * 1 + -2.25 * 2 + 3 * 3));
    CHECK(r.outBool.size() >= 3 && r.outBool[2] == 1u);
    CHECK(r.outInt.size() >= 8 && r.outInt[4] == 4);
    CHECK(disassemblePreshader(p).find("dot") != std::string::npos);

    // unknown opcode / wrong operand count / bad table are rejected
    std::vector<std::uint32_t> bad;
    presIns(bad, static_cast<PresOp>(0x7ff), 1, false, {{kIn, 0}}, {kOut, 0});
    CHECK(!parsePreshader(buildPreshader(0x46580200u, {}, 1, bad)).ok());
    bad.clear();
    presIns(bad, PresOp::Add, 1, false, {{kIn, 0}}, {kOut, 0});
    CHECK(!parsePreshader(buildPreshader(0x46580200u, {}, 1, bad)).ok());
    bad.clear();
    presIns(bad, PresOp::Mov, 1, false, {{kIn, 0}}, {kLit, 0});
    CHECK(!parsePreshader(buildPreshader(0x46580200u, {}, 1, bad)).ok());
    bad.clear();
    presIns(bad, PresOp::Mov, 1, false, {{kLit, 4}}, {kOut, 0});  // literal out of range
    CHECK(!parsePreshader(buildPreshader(0x46580200u, {1, 2, 3, 4}, 1, bad)).ok());
}

namespace {

constexpr const char* kTestEffect = R"(
float4x4 g_mWVP : WORLDVIEWPROJECTION;
float3 g_arr[3] = { {1,2,3}, {4,5,6}, {7,8,9} };
struct Light { float3 dir; float power; };
Light g_light = { {0,1,0}, 2 };
int g_count = 3;
bool g_flag = true;
float g_time : TIME < string UIName = "Time"; float UIMin = 0; > = 0.5;
float g_alpha = 0.25;
int g_index = 1;
float4 g_color = {0.5, 0.25, 1, 2};
texture g_tex < string ResourceName = "foo.dds"; >;
sampler2D g_sam = sampler_state { Texture = <g_tex>; AddressU = Wrap; AddressV = Clamp; MinFilter = Linear; MagFilter = Point; MipFilter = None; };

struct VSO { float4 pos : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };

VSO VS(float4 p : POSITION, uniform int mode)
{
    VSO o;
    o.pos = mul(p, g_mWVP);
    float s = sin(g_time * 3.0) + cos(g_time);
    float e = exp(g_alpha) + log(g_alpha + 1) + rsqrt(g_time + 2) + frac(g_time * 7);
    float m = min(g_alpha, g_time) + max(g_alpha * 2, g_time) + (g_alpha < g_time ? 10 : 20) + (g_time >= 1 ? 100 : 200);
    float d = dot(g_color.xyz, g_light.dir) + g_light.power / g_color.w;
    o.col = float4(s, e, m, d) + g_arr[mode].xyzz;
    o.uv = p.xy * g_count;
    return o;
}

float4 PS(VSO i) : COLOR
{
    float4 c = tex2D(g_sam, i.uv) * i.col;
    if (g_flag) c *= 2;
    return c;
}

VertexShader g_vs[2] = { compile vs_2_0 VS(0), compile vs_2_0 VS(1) };

technique T0 < string Desc = "first"; >
{
    pass P0 < int Order = 1; >
    {
        ZEnable = false;
        AlphaRef = (int)(g_alpha * 255);
        CullMode = None;
        SrcBlend = SrcAlpha;
        DestBlend = InvSrcAlpha;
        ColorWriteEnable = Red | Alpha;
        VertexShader = (g_vs[g_index]);
        PixelShader = compile ps_2_0 PS();
    }
    pass P1
    {
        VertexShader = compile vs_3_0 VS(2);
        PixelShader = compile ps_3_0 PS();
        Sampler[1] = <g_sam>;
        Texture[2] = <g_tex>;
        AlphaTestEnable = <g_flag>;
    }
}
)";

// o.col of VS(mode) computed on the CPU, for the preshader check
std::array<double, 4> expectedColor(double t, double a, int mode) {
    const double arr[3][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
    const double color[4] = {0.5, 0.25, 1, 2};
    const double s = std::sin(t * 3.0) + std::cos(t);
    const double e = std::exp(a) + std::log(a + 1) + 1.0 / std::sqrt(t + 2) + (t * 7 - std::floor(t * 7));
    const double m = std::min(a, t) + std::max(a * 2, t) + (a < t ? 10 : 20) + (t >= 1 ? 100 : 200);
    const double d = color[0] * 0 + color[1] * 1 + color[2] * 0 + 2.0 / color[3];
    return {s + arr[mode][0], e + arr[mode][1], m + arr[mode][2], d + arr[mode][2]};
}

}  // namespace

TEST(fx_d3dx_compiled_effect) {
    if (!D3DX::get().available()) {
        std::printf("  (skipped: d3dx9_39.dll not available)\n");
        return;
    }
    std::string errors;
    const auto bin = D3DX::get().compileEffect(kTestEffect, &errors);
    CHECK_MSG(!bin.empty(), errors);
    const EffectResult r = parseEffect(bin);
    CHECK_MSG(r.ok(), r.error);
    if (!r.ok()) {
        return;
    }
    const Effect& e = r.effect;
    CHECK(e.parameters.size() == 12 && e.techniques.size() == 1 && e.shaders.size() == 5);
    const Parameter* arr = e.findParameter("g_arr");
    CHECK(arr != nullptr && arr->elements == 3 && arr->members.size() == 3 && arr->data.size() == 36);
    CHECK(e.findParameter("g_arr[2]") != nullptr && e.findParameter("g_arr[2]")->dataOffset == 24);
    const Parameter* power = e.findParameter("g_light.power");
    CHECK(power != nullptr && power->dataOffset == 12 && e.parameters[2].floatAt(3) == 2.0f);
    CHECK(e.findParameter("g_time@UIName") != nullptr);
    const Parameter* uiName = e.findParameter("g_time@UIName");
    CHECK(uiName != nullptr && uiName->objectId != kNone && e.objects[uiName->objectId].string() == "Time");
    const Parameter* vsArr = e.findParameter("g_vs");
    CHECK(vsArr != nullptr && vsArr->elements == 2 && e.objects[vsArr->members[1].objectId].shader != kNone);
    const Technique* t = e.findTechnique("T0");
    CHECK(t != nullptr && t->passes.size() == 2 && t->annotations.size() == 1);
    if (t == nullptr || t->passes.size() != 2) {
        return;
    }
    ParameterValues values = ParameterValues::defaults(e);
    int expressions = 0, selectors = 0;
    for (const State& s : t->passes[0].states) {
        if (s.kind == StateKind::Expression && s.info()->op == 24) {  // AlphaRef = (int)(g_alpha * 255)
            ++expressions;
            CHECK(evaluateExpression(e, e.preshaders[s.expression], values, ParamType::Int, 1)[0] == 63);
        }
        if (s.kind == StateKind::ArraySelector) {
            ++selectors;
            CHECK(s.reference == "g_vs" && s.referenceParam == 11);
            CHECK(evaluateExpression(e, e.preshaders[s.expression], values, ParamType::Int, 1)[0] == 1);
        }
        if (s.info()->op == 168) {  // ColorWriteEnable = Red | Alpha
            CHECK(s.value.dwordAt(0) == 9);
        }
    }
    CHECK(expressions == 1 && selectors == 1);
    bool sawSetSampler = false;
    for (const State& s : t->passes[1].states) {
        if (s.info()->cls == StateClass::SetSampler) {
            sawSetSampler = s.index == 1 && s.kind == StateKind::ParamRef && s.reference == "g_sam";
        }
    }
    CHECK(sawSetSampler);

    // constant tables vs D3DX, decode
    for (const Shader& s : e.shaders) {
        CHECK(compareCtab(s.tokens, s.constants) == 0);
        CHECK(rvk::shader::decode(s.tokens).ok());
    }
    for (const Preshader& p : e.preshaders) {
        CHECK_MSG(comparePreshaderCtab(p) == 0, dumpConstantTable(p.inputs));
    }

    // preshaders vs CPU, two parameter sets (flipping the comparisons)
    for (const auto& [time, alpha] : {std::pair{0.5f, 0.25f}, std::pair{1.5f, 0.75f}}) {
        values.setFloats(e, "g_time", std::span(&time, 1));
        values.setFloats(e, "g_alpha", std::span(&alpha, 1));
        for (int mode = 0; mode < 2; ++mode) {
            const Shader& vs = e.shaders[e.objects[vsArr->members[mode].objectId].shader];
            const ConstantRegisters regs = evaluateShaderConstants(e, vs, values);
            const auto want = expectedColor(time, alpha, mode);
            for (int c = 0; c < 4; ++c) {
                CHECK_MSG(approx(reg(regs, 20 + c), want[c]),
                          std::format("mode {} t {} c5.{}: {} vs {}", mode, time, "xyzw"[c], reg(regs, 20 + c), want[c]));
            }
            CHECK(reg(regs, 16) == 3.0f);  // c4.x = g_count (int -> float)
        }
        const float a255 = alpha * 255;
        for (const State& s : t->passes[0].states) {
            if (s.kind == StateKind::Expression) {
                CHECK(evaluateExpression(e, e.preshaders[s.expression], values, ParamType::Int, 1)[0] ==
                      static_cast<std::uint32_t>(static_cast<int>(a255)));
            }
        }
    }
    // matrix: effect parameter row-major, shader constant column_major -> c[i] = column i
    float m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = static_cast<float>(i);
    }
    values.setFloats(e, "g_mWVP", m);
    const ConstantRegisters regs = evaluateShaderConstants(e, e.shaders[0], values);
    CHECK(reg(regs, 0) == 0.0f && reg(regs, 1) == 4.0f && reg(regs, 2) == 8.0f && reg(regs, 3) == 12.0f);
    CHECK(reg(regs, 4) == 1.0f && reg(regs, 15) == 15.0f);
    // the pixel shader's bool parameter in a float register
    const Shader& ps = e.shaders[t->passes[0].states.back().shader];
    CHECK(ps.stage == rvk::shader::Stage::Pixel && reg(evaluateShaderConstants(e, ps, values), 0) == 1.0f);
}

TEST(fx_ctab_compile_shader) {
    if (!D3DX::get().available()) {
        std::printf("  (skipped: d3dx9_39.dll not available)\n");
        return;
    }
    const std::string src = R"(
struct S { float4 a; float2x3 b[2]; int c; };
S g_s;
row_major float4x3 g_rm = { 1,2,3, 4,5,6, 7,8,9, 10,11,12 };
float4x3 g_cm;
float g_scalars[5] = { 1, 2, 3, 4, 5 };
float4 g_vec[4];
int g_loops = 4;
bool g_b0, g_b1;
sampler2D g_tex[2];
samplerCUBE g_cube;
float4 main(float4 uv : TEXCOORD0) : COLOR
{
    float4 r = g_s.a + float4(mul(uv.xyz, transpose(g_s.b[1])), g_s.c, 1);
    r.xyz += mul(uv, g_rm) + mul(uv, g_cm);
    [loop] for (int i = 0; i < g_loops; ++i) r += g_vec[i] * g_scalars[i];
    if (g_b0) r *= 2;
    if (g_b1) r += tex2D(g_tex[1], uv.xy) + texCUBE(g_cube, uv.xyz);
    return r + tex2D(g_tex[0], uv.yx);
}
)";
    std::string errors;
    const auto tokens = D3DX::get().compileShader(src, "main", "ps_3_0", &errors);
    CHECK_MSG(!tokens.empty(), errors);
    if (tokens.empty()) {
        return;
    }
    const ConstantTableResult ct = parseConstantTable(tokens);
    CHECK_MSG(ct.ok(), ct.error);
    CHECK(ct.table.present && ct.table.target == "ps_3_0");
    int compared = 0;
    CHECK(compareCtab(tokens, ct.table, &compared) == 0);
    CHECK(compared > 15);
    bool sawBool = false, sawInt = false, sawSamplerArray = false, sawStruct = false;
    for (const Constant& c : ct.table.constants) {
        sawBool |= c.registerSet == RegisterSet::Bool;
        sawInt |= c.registerSet == RegisterSet::Int4;
        sawSamplerArray |= c.registerSet == RegisterSet::Sampler && c.elements == 2;
        sawStruct |= c.cls == ParamClass::Struct && c.members.size() == 3;
    }
    CHECK_MSG(sawBool && sawInt && sawSamplerArray && sawStruct, dumpConstantTable(ct.table));
    CHECK(rvk::shader::decode(tokens).ok());

    // the same constants through setConstant: row_major float4x3 -> 4 registers of 3; column_major -> 3 registers of 4
    const Constant* rm = ct.table.find("g_rm");
    CHECK(rm != nullptr && rm->cls == ParamClass::MatrixRows && rm->registerCount == 4);
    if (rm != nullptr) {
        Parameter p;
        p.cls = ParamClass::MatrixRows;
        p.type = ParamType::Float;
        p.rows = 4;
        p.columns = 3;
        std::vector<float> data(12);
        for (int i = 0; i < 12; ++i) {
            data[i] = static_cast<float>(i + 1);
        }
        ConstantRegisters regs;
        CHECK(setConstant(*rm, p, std::as_bytes(std::span(data)), regs));
        const std::size_t base = std::size_t(rm->registerIndex) * 4;
        CHECK(reg(regs, base + 0) == 1.0f && reg(regs, base + 2) == 3.0f && reg(regs, base + 4) == 4.0f &&
              reg(regs, base + 14) == 12.0f);
        const Constant* cm = ct.table.find("g_cm");
        CHECK(cm != nullptr && cm->cls == ParamClass::MatrixColumns && cm->registerCount == 3);
        if (cm != nullptr) {
            ConstantRegisters regs2;
            CHECK(setConstant(*cm, p, std::as_bytes(std::span(data)), regs2));
            const std::size_t b2 = std::size_t(cm->registerIndex) * 4;
            // register j = column j = (m[0][j], m[1][j], m[2][j], m[3][j])
            CHECK(reg(regs2, b2 + 0) == 1.0f && reg(regs2, b2 + 1) == 4.0f && reg(regs2, b2 + 3) == 10.0f &&
                  reg(regs2, b2 + 4) == 2.0f && reg(regs2, b2 + 11) == 12.0f);
        }
    }
    // no CTAB: a hand-written shader without comments
    const std::vector<std::uint32_t> bare{0xFFFF0200u, 0x02000001u, 0x800F0800u, 0xA0E40000u, 0x0000FFFFu};
    const ConstantTableResult none = parseConstantTable(bare);
    CHECK(none.ok() && !none.table.present);
}

TEST(fx_corpus) {
    const auto dir = corpusDir();
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        std::printf("  (skipped: no corpus at %s)\n", dir.string().c_str());
        return;
    }
    const bool d3dx = D3DX::get().available();
    int effects = 0, shaders = 0, preshaders = 0, constants = 0, sources = 0, skipped = 0;
    int wobble = 0, eyeAdapt = 0, glow = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (!name.starts_with("fx_") || entry.path().extension() != ".bin") {
            continue;
        }
        auto data = readFile(entry.path());
        if (!isEffectBinary(data)) {
            if (!d3dx) {
                ++skipped;
                continue;
            }
            std::string src(reinterpret_cast<const char*>(data.data()), data.size());
            while (!src.empty() && src.back() == '\0') {
                src.pop_back();
            }
            data = D3DX::get().compileEffect(src);
            ++sources;
        }
        const EffectResult r = parseEffect(data);
        CHECK_MSG(r.ok(), name + ": " + r.error);
        if (!r.ok()) {
            continue;
        }
        ++effects;
        const Effect& e = r.effect;
        ParameterValues values = ParameterValues::defaults(e);
        for (const Shader& s : e.shaders) {
            ++shaders;
            const auto d = rvk::shader::decode(s.tokens);
            CHECK_MSG(d.ok(), name + ": " + d.error);
            if (d3dx) {
                CHECK_MSG(compareCtab(s.tokens, s.constants, &constants) == 0, name);
            }
            (void)evaluateShaderConstants(e, s, values);
        }
        for (const Preshader& p : e.preshaders) {
            ++preshaders;
            if (d3dx) {
                CHECK_MSG(comparePreshaderCtab(p) == 0, name);
            }
        }
        // spot checks against the HLSL sources (post_wobble.fx, post_hdr.fx, post_bloom.fx)
        auto set = [&](const char* n, std::initializer_list<float> v) {
            values.setFloats(e, n, std::span(v.begin(), v.size()));
        };
        auto withPreshaderInput = [&](const char* input, auto&& fn) {
            for (const Shader& s : e.shaders) {
                if (s.preshader != kNone && e.preshaders[s.preshader].inputs.find(input) != nullptr) {
                    fn(evaluateShaderConstants(e, s, values));
                }
            }
        };
        if (e.findTechnique("TWobble") != nullptr) {
            // o.TexCoord0 = uv + 0.5f / SrcTextureSize + sin(uv * Amplitude + FrameTime * Speed) * Wobble
            set("SrcTextureSize", {800, 600});
            set("Speed", {2});
            set("FrameTime", {3});
            withPreshaderInput("Speed", [&](const ConstantRegisters& regs) {
                CHECK(approx(reg(regs, 0), 0.5 / 800) && approx(reg(regs, 1), 0.5 / 600) && reg(regs, 4) == 6.0f);
                ++wobble;
            });
        }
        if (e.findParameter("EyeAdapt") != nullptr) {
            // fLum = fLumOld + (fLum - fLumOld) * (1 - pow(0.98f, EyeAdapt * FrameTime))
            set("EyeAdapt", {0.1f});
            set("FrameTime", {0.5f});
            withPreshaderInput("EyeAdapt", [&](const ConstantRegisters& regs) {
                CHECK(approx(reg(regs, 4), 1.0 - std::pow(0.98, 0.1 * 0.5)));
                ++eyeAdapt;
            });
        }
        if (e.findParameter("FrameRedundancy") != nullptr) {
            // ColResult.a = min(FrameRedundancy * FrameTime, MaxOpacity)
            for (const float frameTime : {2.0f, 10.0f}) {
                set("FrameRedundancy", {0.1f});
                set("MaxOpacity", {0.6f});
                set("FrameTime", {frameTime});
                withPreshaderInput("FrameRedundancy", [&](const ConstantRegisters& regs) {
                    CHECK(approx(reg(regs, 0), std::min(0.1 * frameTime, 0.6)));
                    ++glow;
                });
            }
        }
    }
    std::printf("  %d effects (%d from source), %d shaders, %d preshaders, %d CTAB constants vs D3DX%s; spot checks "
                "%d/%d/%d\n",
                effects, sources, shaders, preshaders, constants, skipped ? " (source effects skipped)" : "", wobble,
                eyeAdapt, glow);
    CHECK(effects > 0);
}
