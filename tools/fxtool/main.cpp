// rvk_fxtool: fx_2_0 effect binary inspection (parser of src/fx/effect_binary.h).
//
//   rvk_fxtool dump <file> [--listing]       dump an effect binary (HLSL source text is compiled with d3dx9_39 first)
//   rvk_fxtool corpus <dir>                  parse every fx_*.bin, decode the shaders, compare every constant table with
//                                            D3DXGetShaderConstantTable, evaluate every preshader, print statistics
//   rvk_fxtool compile <file.fx> <out.bin> [NAME=VALUE ...]   compile an .fx source with D3DX's effect compiler
//
// d3dx9_39.dll is used only as a CPU reference (effect compiler, constant tables); no device is created.

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "fx/effect_binary.h"
#include "shader/sm_ir.h"

namespace fs = std::filesystem;
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
    UINT RegisterSet;
    UINT RegisterIndex;
    UINT RegisterCount;
    UINT Class;
    UINT Type;
    UINT Rows;
    UINT Columns;
    UINT Elements;
    UINT StructMembers;
    UINT Bytes;
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

struct D3DXMacro {
    const char* Name;
    const char* Definition;
};

struct D3DX {
    using CreateEffectCompilerFn = long(__stdcall*)(const char*, UINT, const D3DXMacro*, void*, DWORD, void**,
                                                    D3DXBuffer**);
    using GetConstantTableFn = long(__stdcall*)(const DWORD*, D3DXConstantTable**);
    CreateEffectCompilerFn createEffectCompiler = nullptr;
    GetConstantTableFn getConstantTable = nullptr;

    static const D3DX& get() {
        static const D3DX d = [] {
            D3DX x;
            if (HMODULE m = LoadLibraryW(L"d3dx9_39.dll")) {
                x.createEffectCompiler =
                    reinterpret_cast<CreateEffectCompilerFn>(GetProcAddress(m, "D3DXCreateEffectCompiler"));
                x.getConstantTable =
                    reinterpret_cast<GetConstantTableFn>(GetProcAddress(m, "D3DXGetShaderConstantTable"));
            }
            return x;
        }();
        return d;
    }
    bool available() const { return createEffectCompiler != nullptr && getConstantTable != nullptr; }

    // .fx source -> fx_2_0 binary (ID3DXEffectCompiler::CompileEffect = vtable slot 59)
    std::vector<std::byte> compileEffect(const std::string& src, const std::vector<D3DXMacro>& defines,
                                         std::string* errors) const {
        std::vector<D3DXMacro> macros = defines;
        macros.push_back({nullptr, nullptr});
        void* compiler = nullptr;
        D3DXBuffer* err = nullptr;
        long hr = createEffectCompiler(src.c_str(), static_cast<UINT>(src.size()), macros.data(), nullptr, 0, &compiler,
                                       &err);
        auto takeErrors = [&] {
            if (err != nullptr) {
                if (errors != nullptr) {
                    errors->append(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
                }
                err->Release();
                err = nullptr;
            }
        };
        takeErrors();
        if (hr < 0 || compiler == nullptr) {
            return {};
        }
        using CompileFn = long(__stdcall*)(void*, DWORD, D3DXBuffer**, D3DXBuffer**);
        auto** vtbl = *reinterpret_cast<void***>(compiler);
        D3DXBuffer* out = nullptr;
        hr = reinterpret_cast<CompileFn>(vtbl[59])(compiler, 0, &out, &err);
        takeErrors();
        reinterpret_cast<D3DXBuffer*>(compiler)->Release();
        if (hr < 0 || out == nullptr) {
            return {};
        }
        const auto* p = static_cast<const std::byte*>(out->GetBufferPointer());
        std::vector<std::byte> r(p, p + out->GetBufferSize());
        out->Release();
        return r;
    }
};

std::vector<std::byte> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::vector<char> c((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::byte> b(c.size());
    std::memcpy(b.data(), c.data(), c.size());
    return b;
}

// Effect binary from a file: compiled with D3DX first when it is HLSL source text.
std::vector<std::byte> loadEffect(const fs::path& p, bool* compiled, std::string* errors) {
    std::vector<std::byte> data = readFile(p);
    *compiled = false;
    if (!isEffectBinary(data) && D3DX::get().available()) {
        std::string src(reinterpret_cast<const char*>(data.data()), data.size());
        while (!src.empty() && src.back() == '\0') {
            src.pop_back();
        }
        data = D3DX::get().compileEffect(src, {}, errors);
        *compiled = true;
    }
    return data;
}

// ---------------------------------------------------------------------------------------------------------------
// CTAB comparison against D3DXGetShaderConstantTable
// ---------------------------------------------------------------------------------------------------------------

struct CtabCheck {
    int constants = 0;  // constants compared (incl. members)
    std::vector<std::string> mismatches;
};

// Member defaults: D3DX hands out DefaultValue pointers for struct members even when the constant has no default
// (a running offset from 0); they are compared only when the top-level constant has a default.
void compareConstant(D3DXConstantTable* t, Handle h, const Constant& c, const std::string& path, bool topDefault,
                     CtabCheck& out) {
    D3DXConstantDesc d{};
    UINT n = 1;
    if (t->GetConstantDesc(h, &d, &n) < 0) {
        out.mismatches.push_back(path + ": GetConstantDesc failed");
        return;
    }
    ++out.constants;
    auto check = [&](const char* what, std::uint32_t mine, std::uint32_t ref) {
        if (mine != ref) {
            out.mismatches.push_back(std::format("{} {}: ours {} d3dx {}", path, what, mine, ref));
        }
    };
    if (c.name != (d.Name != nullptr ? d.Name : "")) {
        out.mismatches.push_back(std::format("{} name: ours {} d3dx {}", path, c.name, d.Name ? d.Name : "(null)"));
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
        out.mismatches.push_back(path + ": default value differs");
    }
    std::uint32_t count = 0;
    bool elements = false;
    if (d.Elements > 1) {
        count = d.Elements;
        elements = true;
    } else if (d.StructMembers != 0) {
        count = d.StructMembers;
    }
    check("members", static_cast<std::uint32_t>(c.members.size()), count);
    for (std::uint32_t i = 0; i < std::min<std::uint32_t>(count, static_cast<std::uint32_t>(c.members.size())); ++i) {
        Handle m = elements ? t->GetConstantElement(h, i) : t->GetConstant(h, i);
        if (m == nullptr) {
            out.mismatches.push_back(std::format("{}: member {} handle null", path, i));
            continue;
        }
        compareConstant(t, m, c.members[i], std::format("{}{}{}", path, elements ? "[" : ".",
                                                        elements ? std::to_string(i) + "]" : c.members[i].name),
                        topDefault, out);
    }
}

// Compares our CTAB of `tokens` with D3DX's. Returns false if D3DX could not load one.
bool compareCtab(const std::vector<std::uint32_t>& tokens, const ConstantTable& mine, CtabCheck& out) {
    D3DXConstantTable* t = nullptr;
    const long hr = D3DX::get().getConstantTable(reinterpret_cast<const DWORD*>(tokens.data()), &t);
    if (hr < 0 || t == nullptr) {
        if (mine.present) {
            out.mismatches.push_back(std::format("D3DX found no constant table (hr 0x{:08x})", static_cast<unsigned>(hr)));
        }
        return false;
    }
    D3DXConstantTableDesc desc{};
    t->GetDesc(&desc);
    if (desc.Constants != mine.constants.size()) {
        out.mismatches.push_back(std::format("constant count: ours {} d3dx {}", mine.constants.size(), desc.Constants));
    }
    if (desc.Version != mine.version) {
        out.mismatches.push_back("version differs");
    }
    if (std::string(desc.Creator ? desc.Creator : "") != mine.creator) {
        out.mismatches.push_back("creator differs");
    }
    for (UINT i = 0; i < std::min<UINT>(desc.Constants, static_cast<UINT>(mine.constants.size())); ++i) {
        D3DXConstantDesc top{};
        UINT one = 1;
        Handle h = t->GetConstant(nullptr, i);
        t->GetConstantDesc(h, &top, &one);
        compareConstant(t, h, mine.constants[i], mine.constants[i].name, top.DefaultValue != nullptr, out);
    }
    t->Release();
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------------------------------------------

int cmdDump(const fs::path& file, bool listing) {
    bool compiled = false;
    std::string errors;
    const auto data = loadEffect(file, &compiled, &errors);
    if (compiled) {
        std::printf("(HLSL source compiled with d3dx9_39%s)\n", data.empty() ? " FAILED" : "");
        if (!errors.empty()) {
            std::printf("%s\n", errors.c_str());
        }
    }
    const EffectResult r = parseEffect(data);
    if (!r.ok()) {
        std::printf("error: %s\n", r.error.c_str());
        return 1;
    }
    DumpOptions o;
    o.shaderListings = listing;
    std::fputs(dumpEffect(r.effect, o).c_str(), stdout);
    return 0;
}

int cmdCompile(const fs::path& in, const fs::path& out, const std::vector<std::string>& defs) {
    if (!D3DX::get().available()) {
        std::printf("d3dx9_39.dll not available\n");
        return 1;
    }
    std::vector<std::string> names, values;
    for (const std::string& d : defs) {
        const auto eq = d.find('=');
        names.push_back(d.substr(0, eq));
        values.push_back(eq == std::string::npos ? "1" : d.substr(eq + 1));
    }
    std::vector<D3DXMacro> macros;
    for (std::size_t i = 0; i < names.size(); ++i) {
        macros.push_back({names[i].c_str(), values[i].c_str()});
    }
    const auto src = readFile(in);
    std::string errors;
    const auto bin =
        D3DX::get().compileEffect(std::string(reinterpret_cast<const char*>(src.data()), src.size()), macros, &errors);
    if (!errors.empty()) {
        std::printf("%s\n", errors.c_str());
    }
    if (bin.empty()) {
        return 1;
    }
    std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(bin.data()), std::streamsize(bin.size()));
    std::printf("%s: %zu bytes\n", out.string().c_str(), bin.size());
    return 0;
}

struct Stats {
    std::map<std::string, std::pair<int, std::set<std::string>>> states;  // name -> (assignments, effects)
    std::map<std::string, int> stateKinds;
    std::map<std::string, int> presOps;
    std::map<std::string, int> paramTypes;
    std::map<std::string, int> shaderModels;
    std::map<std::string, int> regsets;
    std::map<std::string, std::set<std::string>> stateValues;  // state -> distinct values
    std::map<std::string, int> semantics;
    std::map<std::string, int> misc;
    std::map<std::string, int> variants;  // technique list -> binaries
    int effects = 0, techniques = 0, passes = 0, shaders = 0, preshaders = 0, shaderPreshaders = 0;
    int expressionStates = 0, annotations = 0;
};

void countState(Stats& st, const State& s, const std::string& effect) {
    const StateInfo* info = s.info();
    auto& e = st.states[std::format("0x{:02x} {}", s.id, info->name)];
    ++e.first;
    e.second.insert(effect);
    ++st.stateKinds[stateKindName(s.kind)];
    if (s.kind == StateKind::Constant && s.value.cls != ParamClass::Object && s.value.dataSize == 4) {
        const std::string sym = stateValueName(s.id, s.value.dwordAt(0));
        st.stateValues[info->name].insert(sym.empty() ? std::format("{}", s.value.type == ParamType::Float
                                                                              ? std::format("{}", s.value.floatAt(0))
                                                                              : std::to_string(s.value.dwordAt(0)))
                                                      : sym);
    } else if (s.kind != StateKind::Constant) {
        st.stateValues[info->name].insert(std::string("<") + stateKindName(s.kind) + ">");
    }
}

int cmdCorpus(const fs::path& dir) {
    if (!D3DX::get().available()) {
        std::printf("note: d3dx9_39.dll not available, no CTAB cross-check\n");
    }
    Stats st;
    int failures = 0, ctabChecked = 0, ctabConstants = 0, decoded = 0;
    std::map<std::string, std::string> uniqueDumps;
    for (const auto& entry : fs::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (!name.starts_with("fx_") || entry.path().extension() != ".bin") {
            continue;
        }
        bool compiled = false;
        std::string errors;
        const auto data = loadEffect(entry.path(), &compiled, &errors);
        const EffectResult r = parseEffect(data);
        if (!r.ok()) {
            std::printf("FAIL %s: %s\n", name.c_str(), r.error.c_str());
            ++failures;
            continue;
        }
        const Effect& e = r.effect;
        ++st.effects;
        std::string techs;
        for (const Technique& t : e.techniques) {
            techs += t.name + " ";
        }
        std::printf("ok   %s%s: %zu params, %zu techniques (%s), %zu shaders, %zu preshaders\n", name.c_str(),
                    compiled ? " (source, compiled)" : "", e.parameters.size(), e.techniques.size(), techs.c_str(),
                    e.shaders.size(), e.preshaders.size());
        for (const Parameter& p : e.parameters) {
            ++st.paramTypes[std::format("{} {}", className(p.cls), typeName(p.type))];
            st.annotations += static_cast<int>(p.annotations.size());
            if (!p.semantic.empty()) {
                ++st.semantics[p.semantic];
            }
            if (p.isArray()) {
                ++st.misc["array parameters"];
            }
            if (p.cls == ParamClass::Struct) {
                ++st.misc["struct parameters"];
            }
            if ((p.flags & kParamShared) != 0) {
                ++st.misc["shared parameters"];
            }
            for (const State& s : p.samplerStates) {
                countState(st, s, name);
                ++st.misc[std::format("sampler_state assignment index {}", s.index)];
            }
            for (const Parameter& m : p.members) {
                for (const State& s : m.samplerStates) {
                    countState(st, s, name);
                }
            }
        }
        ++st.variants[techs];
        for (const Technique& t : e.techniques) {
            ++st.techniques;
            st.misc["technique annotations"] += static_cast<int>(t.annotations.size());
            for (const Pass& p : t.passes) {
                ++st.passes;
                st.misc["pass annotations"] += static_cast<int>(p.annotations.size());
                for (const State& s : p.states) {
                    countState(st, s, name);
                    if (s.index != 0) {
                        ++st.misc[std::format("pass state {} index {}", s.info()->name, s.index)];
                    }
                }
            }
        }
        // shaders: decode + CTAB vs D3DX + preshader evaluation with the default parameter values
        const ParameterValues values = ParameterValues::defaults(e);
        for (const Shader& s : e.shaders) {
            ++st.shaders;
            ++st.shaderModels[std::format("{}s_{}_{}", s.stage == rvk::shader::Stage::Vertex ? 'v' : 'p', s.major,
                                          s.minor)];
            const auto d = rvk::shader::decode(s.tokens);
            if (!d.ok()) {
                std::printf("  shader object %u: decode error %s\n", s.objectId, d.error.c_str());
                ++failures;
            } else {
                ++decoded;
            }
            for (const Constant& c : s.constants.constants) {
                ++st.regsets[registerSetName(c.registerSet)];
            }
            if (D3DX::get().available()) {
                CtabCheck check;
                compareCtab(s.tokens, s.constants, check);
                ++ctabChecked;
                ctabConstants += check.constants;
                for (const std::string& m : check.mismatches) {
                    std::printf("  shader object %u CTAB mismatch: %s\n", s.objectId, m.c_str());
                    ++failures;
                }
            }
            (void)evaluateShaderConstants(e, s, values);
            if (s.preshader != kNone) {
                ++st.shaderPreshaders;
            }
        }
        for (const Preshader& p : e.preshaders) {
            ++st.preshaders;
            st.misc["preshader instructions (total)"] += static_cast<int>(p.code.size());
            st.misc["preshader max instructions"] =
                std::max(st.misc["preshader max instructions"], static_cast<int>(p.code.size()));
            st.misc["preshader max temps"] = std::max(st.misc["preshader max temps"], static_cast<int>(p.tempRegisters));
            ++st.misc[std::format("preshader version 0x{:08x}", p.version)];
            if (p.outIntRegisters != 0 || p.outBoolRegisters != 0) {
                ++st.misc["preshaders writing int/bool registers"];
            }
            for (const PresInstruction& ins : p.code) {
                if (ins.output.reg.table == PresTable::Output) {
                    ++st.misc["preshader writes to float4 constants"];
                }
                for (const PresOperand& o : ins.inputs) {
                    if (o.relative) {
                        ++st.misc["preshader relative addressing"];
                    }
                }
                ++st.presOps[std::format("{}{}", presOpName(ins.op), ins.scalar ? " (scalar)" : "")];
            }
            if (D3DX::get().available() && p.inputs.present) {
                // the preshader's CTAB through D3DX: a vs_3_0 token stream with the same CTAB comment
                const auto body = findComment(p.tokens, fourcc('C', 'T', 'A', 'B'));
                std::vector<std::uint32_t> fake{0xFFFE0300u, 0xFFFEu | (static_cast<std::uint32_t>(body.size() + 1) << 16),
                                                fourcc('C', 'T', 'A', 'B')};
                fake.insert(fake.end(), body.begin(), body.end());
                fake.push_back(0x0000FFFFu);
                CtabCheck check;
                compareCtab(fake, p.inputs, check);
                ++ctabChecked;
                ctabConstants += check.constants;
                for (const std::string& m : check.mismatches) {
                    std::printf("  preshader CTAB mismatch: %s\n", m.c_str());
                    ++failures;
                }
            }
        }
        uniqueDumps.emplace(dumpEffect(e), name);
    }

    std::printf("\n%d effects, %d techniques, %d passes, %d shaders (%d decoded, %d with preshader), %d preshaders\n",
                st.effects, st.techniques, st.passes, st.shaders, decoded, st.shaderPreshaders, st.preshaders);
    std::printf("CTAB cross-check: %d shaders, %d constants (incl. members) compared with D3DXGetShaderConstantTable\n",
                ctabChecked, ctabConstants);
    std::printf("distinct effect dumps: %zu\n", uniqueDumps.size());
    std::printf("\nstate assignments (count, effects, values):\n");
    for (const auto& [n, v] : st.states) {
        const std::string key = n.substr(n.find(' ') + 1);
        std::string vals;
        for (const std::string& s : st.stateValues[key]) {
            vals += s + " ";
        }
        std::printf("  %-36s %5d %3zu  %s\n", n.c_str(), v.first, v.second.size(), vals.c_str());
    }
    std::printf("\nstate kinds:\n");
    for (const auto& [n, c] : st.stateKinds) {
        std::printf("  %-20s %d\n", n.c_str(), c);
    }
    std::printf("\npreshader opcodes:\n");
    for (const auto& [n, c] : st.presOps) {
        std::printf("  %-20s %d\n", n.c_str(), c);
    }
    std::printf("\nparameter types:\n");
    for (const auto& [n, c] : st.paramTypes) {
        std::printf("  %-28s %d\n", n.c_str(), c);
    }
    std::printf("\nshader models:\n");
    for (const auto& [n, c] : st.shaderModels) {
        std::printf("  %-10s %d\n", n.c_str(), c);
    }
    std::printf("\nsemantics:\n");
    for (const auto& [n, c] : st.semantics) {
        std::printf("  %-28s %d\n", n.c_str(), c);
    }
    std::printf("\nvariants (technique lists -> binaries):\n");
    for (const auto& [n, c] : st.variants) {
        std::printf("  %3d  %s\n", c, n.c_str());
    }
    std::printf("\nmisc:\n");
    for (const auto& [n, c] : st.misc) {
        std::printf("  %-48s %d\n", n.c_str(), c);
    }
    std::printf("\nshader CTAB register sets:\n");
    for (const auto& [n, c] : st.regsets) {
        std::printf("  %-10s %d\n", n.c_str(), c);
    }
    std::printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "dump") == 0) {
        return cmdDump(argv[2], argc >= 4 && std::strcmp(argv[3], "--listing") == 0);
    }
    if (argc >= 3 && std::strcmp(argv[1], "corpus") == 0) {
        return cmdCorpus(argv[2]);
    }
    if (argc >= 4 && std::strcmp(argv[1], "compile") == 0) {
        return cmdCompile(argv[2], argv[3], std::vector<std::string>(argv + 4, argv + argc));
    }
    std::printf("usage: rvk_fxtool dump <file> [--listing] | corpus <dir> | compile <file.fx> <out.bin> [NAME=VALUE...]\n");
    return 2;
}
