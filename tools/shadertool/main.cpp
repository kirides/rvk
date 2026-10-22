// rvk_shadertool: offline driver of the D3D9 -> SPIR-V shader translator.
//
//   rvk_shadertool corpus <dir> [--out <dir>] [--max-errors N] [--tint]
//                                                                translate + validate every vs*/ps* file of a capture corpus
//                                                                (--tint: the pixel shaders' tint debug-view variant)
//   rvk_shadertool show <file>                                   disassembly, GLSL, compiler log of one shader
//   rvk_shadertool refdisasm <file>                              D3DXDisassembleShader (d3dx9_39.dll) as the reference listing
//   rvk_shadertool evalcheck <dir>                               the CPU evaluator (shader/sm_eval.h) on every SM 3.0 shader of a
//                                                                corpus: failures by reason, pixel shaders sampling s9, slices
//
// The corpus comes from mode = "trace" (bin\rvk\corpus). Exit code 0 when every shader passes.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <format>
#include <map>
#include <string>
#include <vector>

#include <spirv-tools/libspirv.hpp>

#include "shader/glsl.h"
#include "shader/sm_eval.h"
#include "shader/sm_ir.h"
#include "shader/spirv.h"

namespace fs = std::filesystem;
using namespace rvk::shader;

namespace {

std::vector<std::uint32_t> readTokens(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::uint32_t> tokens(bytes.size() / 4);
    std::memcpy(tokens.data(), bytes.data(), tokens.size() * 4);
    return tokens;
}

struct Outcome {
    std::string stage;  // "decode", "glsl", "compile", "validate", "" = ok
    std::string message;
    std::string disasm;
    std::string glsl;
};

Outcome translate(const std::vector<std::uint32_t>& tokens, spvtools::SpirvTools& tools, const GlslOptions& options = {}) {
    Outcome o;
    const DecodeResult dec = decode(tokens);
    if (!dec.ok()) {
        o.stage = "decode";
        o.message = dec.error;
        return o;
    }
    o.disasm = disassemble(dec.program);
    const GlslResult g = toGlsl(dec.program, options);
    if (!g.ok()) {
        o.stage = "glsl";
        o.message = g.error;
        return o;
    }
    o.glsl = g.glsl;
    const SpirvResult s = compileGlsl(dec.program.version.stage, g.glsl);
    if (!s.ok()) {
        o.stage = "compile";
        o.message = s.log;
        return o;
    }
    std::string vlog;
    tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* m) { vlog += m; vlog += "\n"; });
    if (!tools.Validate(s.words)) {
        o.stage = "validate";
        o.message = vlog;
    }
    return o;
}

int cmdCorpus(const fs::path& dir, const fs::path& out, int maxErrors, const GlslOptions& options) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::map<std::string, int> ok, failed;
    std::map<std::string, int> failuresByMessage;
    int printed = 0;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir)) {
        const auto name = e.path().filename().string();
        if (e.is_regular_file() && (name.starts_with("vs") || name.starts_with("ps"))) {
            files.push_back(e.path());
        }
    }
    std::ranges::sort(files);
    for (const auto& f : files) {
        const std::string kind = f.filename().string().substr(0, f.filename().string().find('_'));
        const Outcome o = translate(readTokens(f), tools, options);
        if (o.stage.empty()) {
            ++ok[kind];
            continue;
        }
        ++failed[kind];
        const std::string firstLine = o.message.substr(0, o.message.find('\n'));
        ++failuresByMessage[o.stage + ": " + firstLine.substr(0, 120)];
        if (printed < maxErrors) {
            ++printed;
            std::printf("FAIL %s [%s] %s\n", f.filename().string().c_str(), o.stage.c_str(), firstLine.c_str());
        }
        if (!out.empty()) {
            fs::create_directories(out);
            std::ofstream(out / (f.stem().string() + ".txt")) << o.stage << "\n" << o.message << "\n---- disasm\n"
                                                              << o.disasm << "\n---- glsl\n" << o.glsl;
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\n%-6s %8s %8s\n", "kind", "ok", "failed");
    int totalFailed = 0;
    std::map<std::string, int> kinds = ok;
    for (const auto& [k, n] : failed) kinds[k] += 0;
    for (const auto& [k, n] : kinds) {
        std::printf("%-6s %8d %8d\n", k.c_str(), ok[k], failed[k]);
        totalFailed += failed[k];
    }
    if (!failuresByMessage.empty()) {
        std::printf("\nfailure reasons:\n");
        for (const auto& [m, n] : failuresByMessage) {
            std::printf("%6d  %s\n", n, m.c_str());
        }
    }
    std::printf("\n%zu shaders in %.1f s\n", files.size(), secs);
    return totalFailed == 0 ? 0 : 1;
}

int cmdShow(const fs::path& file) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    const Outcome o = translate(readTokens(file), tools);
    std::printf("%s\n---- glsl\n%s\n", o.disasm.c_str(), o.glsl.c_str());
    if (!o.stage.empty()) {
        std::printf("---- FAILED at %s\n%s\n", o.stage.c_str(), o.message.c_str());
        return 1;
    }
    std::printf("---- ok\n");
    return 0;
}

int cmdRefDisasm(const fs::path& file) {
    HMODULE d3dx = LoadLibraryW(L"d3dx9_39.dll");
    if (d3dx == nullptr) {
        std::printf("d3dx9_39.dll not found\n");
        return 2;
    }
    struct Buffer {  // ID3DXBuffer
        virtual long __stdcall QueryInterface(const void*, void**) = 0;
        virtual unsigned long __stdcall AddRef() = 0;
        virtual unsigned long __stdcall Release() = 0;
        virtual void* __stdcall GetBufferPointer() = 0;
        virtual unsigned long __stdcall GetBufferSize() = 0;
    };
    using Fn = long(__stdcall*)(const DWORD*, BOOL, const char*, Buffer**);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(d3dx, "D3DXDisassembleShader"));
    const auto tokens = readTokens(file);
    Buffer* buf = nullptr;
    if (fn == nullptr || fn(reinterpret_cast<const DWORD*>(tokens.data()), FALSE, nullptr, &buf) < 0 || buf == nullptr) {
        std::printf("D3DXDisassembleShader failed\n");
        return 1;
    }
    std::fwrite(buf->GetBufferPointer(), 1, buf->GetBufferSize(), stdout);
    buf->Release();
    return 0;
}

int cmdEvalCheck(const fs::path& dir) {
    namespace ev = rvk::shader::eval;
    std::map<std::string, int> errors;
    int total = 0, ok = 0, ps9 = 0, ps9Flow = 0, ps9Rel = 0, ps9Consts = 0;
    static float consts[256][4];
    for (int i = 0; i < 256; ++i) consts[i][0] = consts[i][1] = consts[i][2] = consts[i][3] = 0.5f;
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("vs30_", 0) != 0 && name.rfind("ps30_", 0) != 0) continue;
        const DecodeResult dec = decode(readTokens(e.path()));
        if (!dec.ok()) continue;
        ++total;
        ev::Env env;
        env.floats = consts;
        env.floatCount = 224;
        for (auto& v : env.inputs) v = {0.25f, 0.5f, 0.75f, 1.0f};
        const ev::Result r = ev::run(dec.program, env);
        if (r.ok) {
            ++ok;
        } else {
            std::string key = r.error.substr(0, r.error.find(" at token"));
            ++errors[key];
        }
        if (dec.program.version.stage == Stage::Pixel) {
            const std::size_t at = ev::firstSample(dec.program, 9);
            if (at != SIZE_MAX) {
                ++ps9;
                const ev::Slice s = ev::sliceSource(dec.program, at, 0, 0x3);
                ps9Flow += s.flow ? 1 : 0;
                ps9Rel += s.relative ? 1 : 0;
                ps9Consts += static_cast<int>(s.floatConsts.size());
            }
        }
    }
    std::printf("evaluated %d of %d SM 3.0 shaders without error\n", ok, total);
    for (const auto& [k, n] : errors) std::printf("  %5d  %s\n", n, k.c_str());
    std::printf("pixel shaders sampling s9: %d (slice with flow control %d, relative %d, %.2f constants on average)\n", ps9, ps9Flow, ps9Rel,
                ps9 > 0 ? static_cast<double>(ps9Consts) / ps9 : 0.0);
    return ok == total ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: rvk_shadertool corpus <dir> [--out <dir>] [--max-errors N] [--tint] | show <file> | refdisasm <file>\n");
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "corpus") {
        fs::path out;
        int maxErrors = 20;
        GlslOptions options;
        for (int i = 3; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--tint") {
                options.tintEpilogue = true;
            } else if (i + 1 < argc && a == "--out") {
                out = argv[++i];
            } else if (i + 1 < argc && a == "--max-errors") {
                maxErrors = std::atoi(argv[++i]);
            }
        }
        return cmdCorpus(argv[2], out, maxErrors, options);
    }
    if (cmd == "show") return cmdShow(argv[2]);
    if (cmd == "refdisasm") return cmdRefDisasm(argv[2]);
    if (cmd == "evalcheck") return cmdEvalCheck(argv[2]);
    std::printf("unknown command %s\n", cmd.c_str());
    return 2;
}
