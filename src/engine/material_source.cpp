#include "engine/material_source.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "d3d9/device.h"
#include "engine/memread.h"
#include "engine/symbols.h"

namespace rvk::engine::matsrc {
namespace {

// eSGfxHLSLShaderSrc (shaderq_engine_dump.txt 3606-3638): +0 bCString source / file name, +8 eSGfxEffectMacro* {name, value}
// (16 bytes each, null name ends), +0x10 bCString entry, +0x18 u32 profile, +0x1c u32 D3DX flags, +0x20 byte "source is a file"
constexpr std::size_t kSrcText = 0x00, kSrcMacros = 0x08, kSrcEntry = 0x10, kSrcProfile = 0x18, kSrcFlags = 0x1c, kSrcFile = 0x20;
constexpr std::size_t kMaxMacros = 512;
constexpr std::size_t kMaxText = 1 << 16;
// eCShaderPoolEllement +0x18 -> eCGfxHLSLShader; its +0x18 -> the API shader (FUN_1806bb7c0)
constexpr std::size_t kElementShader = 0x18, kWrapperApi = 0x18;

using CompileFn = void* (*)(void* admin, const void* src, int version, unsigned long flags);
Hook<CompileFn> g_hook;
std::atomic<bool> g_enabled{false};
Resolver g_resolver = nullptr;

struct Entry {
    std::uint64_t hash;        // d3d9::Shader::hash (the run-time name)
    std::uint64_t corpusHash;  // FNV-1a of the bytecode bytes: the corpus file name (trace mode capture)
    bool pixel;
    const d3d9::MaterialSource* source;
};
std::mutex g_mutex;
std::vector<std::unique_ptr<d3d9::MaterialSource>> g_owned;
std::vector<Entry> g_entries;
std::filesystem::path g_output;
bool g_dirty = false;
std::chrono::steady_clock::time_point g_lastDump{};
std::atomic<std::size_t> g_requests{0}, g_resolved{0}, g_unresolved{0};

std::uint64_t fnv1a(const void* data, std::size_t size) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// a NUL-terminated string at `p` (SEH-guarded, in chunks that stay within a page)
std::string readChars(const char* p, std::size_t limit = kMaxText) {
    std::string out;
    if (p == nullptr) return out;
    char buf[256];
    while (out.size() < limit) {
        const auto addr = reinterpret_cast<std::uintptr_t>(p) + out.size();
        const std::size_t n = std::min<std::size_t>(sizeof(buf), 4096 - (addr & 4095));
        if (!safeRead(reinterpret_cast<const void*>(addr), buf, n)) break;
        for (std::size_t i = 0; i < n; ++i) {
            if (buf[i] == 0) return out;
            out.push_back(buf[i]);
        }
    }
    return out;
}

void record(const void* src, void* element) {
    g_requests.fetch_add(1, std::memory_order_relaxed);
    void* wrapper = nullptr;
    void* api = nullptr;
    if (!readAt(element, kElementShader, wrapper) || !readAt(wrapper, kWrapperApi, api) || api == nullptr || g_resolver == nullptr) {
        g_unresolved.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    d3d9::Shader* shader = g_resolver(api);
    if (shader == nullptr) {
        g_unresolved.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_resolved.fetch_add(1, std::memory_order_relaxed);
    if (shader->source.load(std::memory_order_acquire) != nullptr) return;  // the first request of a bytecode names it
    auto s = std::make_unique<d3d9::MaterialSource>();
    s->file = readCString(static_cast<const std::uint8_t*>(src) + kSrcText);
    s->entry = readCString(static_cast<const std::uint8_t*>(src) + kSrcEntry);
    std::uint8_t isFile = 1;
    readAt(src, kSrcProfile, s->profile);
    readAt(src, kSrcFlags, s->flags);
    readAt(src, kSrcFile, isFile);
    s->inlineSource = isFile == 0;
    const std::uint8_t* macros = nullptr;
    if (readAt(src, kSrcMacros, macros) && macros != nullptr) {
        for (std::size_t i = 0; i < kMaxMacros; ++i) {
            const char* name = nullptr;
            const char* value = nullptr;
            if (!readAt(macros, i * 16, name) || name == nullptr) break;
            readAt(macros, i * 16 + 8, value);
            s->macros.emplace_back(readChars(name, 256), readChars(value));
        }
    }
    const d3d9::MaterialSource* raw = s.get();
    const d3d9::MaterialSource* expected = nullptr;
    const std::lock_guard lock(g_mutex);
    if (shader->source.compare_exchange_strong(expected, raw, std::memory_order_acq_rel)) {
        g_entries.push_back({shader->hash, fnv1a(shader->bytecode.data(), shader->bytecode.size() * 4), shader->stage == shader::Stage::Pixel, raw});
        g_owned.push_back(std::move(s));
        g_dirty = true;
    }
}

void* detour(void* admin, const void* src, int version, unsigned long flags) {
    void* element = g_hook.original(admin, src, version, flags);
    if (element != nullptr && g_enabled.load(std::memory_order_relaxed)) record(src, element);
    return element;
}

void appendJson(std::string& out, const std::string& s) {
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", static_cast<unsigned>(c));
            else out.push_back(c);
        }
    }
    out.push_back('"');
}

void dump() {
    std::vector<Entry> copy;
    std::filesystem::path path;
    {
        const std::lock_guard lock(g_mutex);
        if (!g_dirty || g_output.empty()) return;
        copy = g_entries;
        path = g_output;
        g_dirty = false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        std::string line;
        for (const Entry& e : copy) {
            line.clear();
            line += std::format("{{\"hash\":\"{:016x}\",\"corpus\":\"{:016x}\",\"stage\":\"{}\",\"profile\":{},\"flags\":{},\"inline\":{},\"file\":",
                                e.hash, e.corpusHash, e.pixel ? "ps" : "vs", e.source->profile, e.source->flags, e.source->inlineSource ? "true" : "false");
            appendJson(line, e.source->file);
            line += ",\"entry\":";
            appendJson(line, e.source->entry);
            line += ",\"macros\":[";
            bool first = true;
            for (const auto& [name, value] : e.source->macros) {
                if (!first) line.push_back(',');
                first = false;
                line.push_back('[');
                appendJson(line, name);
                line.push_back(',');
                appendJson(line, value);
                line.push_back(']');
            }
            line += "]}\n";
            out.write(line.data(), static_cast<std::streamsize>(line.size()));
        }
    }
    std::filesystem::rename(tmp, path, ec);
    spdlog::info("engine: material sources: {} shaders written to {}", copy.size(), path.string());
}

}  // namespace

void setResolver(Resolver r) { g_resolver = r; }
void setEnabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
void setOutput(std::filesystem::path file) {
    const std::lock_guard lock(g_mutex);
    g_output = std::move(file);
}

bool install() {
    return g_hook.install("eCShaderMaterialAdmin::CompileShaderPoolEllement", reinterpret_cast<CompileFn>(sym().shaderPoolCompile),
                          &detour);
}

void onPresent() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastDump < std::chrono::seconds(15)) return;
    g_lastDump = now;
    dump();
}

Stats stats() {
    const std::lock_guard lock(g_mutex);
    return {g_requests.load(), g_resolved.load(), g_entries.size(), g_unresolved.load()};
}

}  // namespace rvk::engine::matsrc
