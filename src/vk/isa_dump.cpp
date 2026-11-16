#include "vk/isa_dump.h"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "vk/context.h"

namespace rvk::vk::isa {
namespace {

struct State {
    std::string dir;
    bool on = false;
};

const State& state() {
    static const State s = [] {
        State r;
        char buf[1024]{};
        const DWORD n = GetEnvironmentVariableA("RVK_ISA_DUMP", buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) {
            r.dir.assign(buf, n);
            r.on = true;
            std::error_code ec;
            std::filesystem::create_directories(r.dir, ec);
        }
        return r;
    }();
    return s;
}

std::string safeName(std::string_view s) {
    std::string r;
    for (const char c : s) r += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') ? c : '_';
    if (r.size() > 120) r.resize(120);
    return r;
}

std::string statValue(const VkPipelineExecutableStatisticKHR& s) {
    char b[64];
    switch (s.format) {
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: return s.value.b32 ? "true" : "false";
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(s.value.i64)); return b;
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: std::snprintf(b, sizeof(b), "%llu", static_cast<unsigned long long>(s.value.u64)); return b;
    default: std::snprintf(b, sizeof(b), "%.3f", s.value.f64); return b;
    }
}

}  // namespace

bool enabled() { return state().on; }
const std::string& directory() { return state().dir; }

VkPipelineCreateFlags flags(const Context& ctx) {
    if (!state().on || !ctx.features().pipelineStatistics) return 0;
    return VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
}

void dump(const Context& ctx, VkPipeline pipeline, std::string_view label) {
    if (!state().on || pipeline == VK_NULL_HANDLE || !ctx.features().pipelineStatistics) return;
    static std::atomic<unsigned> seq{0};
    static std::mutex csvMutex;
    static std::atomic<std::uint64_t> textBytes{0};
    static const std::uint64_t textCap = [] {  // RVK_ISA_DUMP_MB: cap of the written text (default 3072 MiB; the statistics are always written)
        char b[16]{};
        const DWORD n = GetEnvironmentVariableA("RVK_ISA_DUMP_MB", b, sizeof(b));
        return (n > 0 && n < sizeof(b) ? std::strtoull(b, nullptr, 10) : 3072ull) << 20;
    }();
    const unsigned id = seq.fetch_add(1);
    VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    pi.pipeline = pipeline;
    std::uint32_t n = 0;
    if (vkGetPipelineExecutablePropertiesKHR(ctx.device(), &pi, &n, nullptr) != VK_SUCCESS || n == 0) return;
    std::vector<VkPipelineExecutablePropertiesKHR> props(n, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    vkGetPipelineExecutablePropertiesKHR(ctx.device(), &pi, &n, props.data());
    char head[32];
    std::snprintf(head, sizeof(head), "%05u_", id);
    const std::filesystem::path path = std::filesystem::path(state().dir) / (std::string(head) + safeName(label) + ".txt");
    std::ofstream out(path, std::ios::binary);
    out << "label: " << label << "\n";
    std::string csv;
    for (std::uint32_t i = 0; i < n; ++i) {
        out << "\n=== executable " << i << " '" << props[i].name << "' stages 0x" << std::hex << props[i].stages << std::dec << " subgroup " << props[i].subgroupSize
            << " (" << props[i].description << ")\n";
        VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        ei.pipeline = pipeline;
        ei.executableIndex = i;
        std::uint32_t m = 0;
        vkGetPipelineExecutableStatisticsKHR(ctx.device(), &ei, &m, nullptr);
        std::vector<VkPipelineExecutableStatisticKHR> st(m, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        vkGetPipelineExecutableStatisticsKHR(ctx.device(), &ei, &m, st.data());
        char line[512];
        std::snprintf(line, sizeof(line), "%05u,\"%.*s\",%u,\"%s\",%u", id, static_cast<int>(label.size()), label.data(), i, props[i].name, props[i].subgroupSize);
        csv += line;
        for (const auto& s : st) {
            out << "  stat " << s.name << " = " << statValue(s) << "\n";
            csv += std::string(",\"") + s.name + "=" + statValue(s) + "\"";
        }
        csv += "\n";
        std::uint32_t r = 0;
        vkGetPipelineExecutableInternalRepresentationsKHR(ctx.device(), &ei, &r, nullptr);
        std::vector<VkPipelineExecutableInternalRepresentationKHR> irs(r, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
        vkGetPipelineExecutableInternalRepresentationsKHR(ctx.device(), &ei, &r, irs.data());  // (sizes only)
        std::vector<std::vector<char>> data(r);
        for (std::uint32_t k = 0; k < r; ++k) {
            data[k].resize(irs[k].dataSize);
            irs[k].pData = data[k].data();
        }
        vkGetPipelineExecutableInternalRepresentationsKHR(ctx.device(), &ei, &r, irs.data());
        for (std::uint32_t k = 0; k < r; ++k) {
            out << "  internal representation '" << irs[k].name << "' (" << (irs[k].isText ? "text" : "binary") << ", " << irs[k].dataSize << " bytes): " << irs[k].description << "\n";
            if (irs[k].dataSize == 0) continue;
            if (irs[k].isText && std::string_view(irs[k].name).starts_with("ISA")) {
                // instruction count + wave size from the text (wave32 code names exec_lo / vcc_lo): two more CSV columns
                const std::string_view t(static_cast<const char*>(irs[k].pData), irs[k].dataSize);
                std::size_t instr = 0, w32 = 0, w64 = 0;
                for (std::size_t pos = 0; pos < t.size();) {
                    const std::size_t e = std::min(t.find('\n', pos), t.size());
                    const std::string_view ln = t.substr(pos, e - pos);
                    if (ln.size() > 3 && ln[0] == '\t' && ln[1] >= 'a' && ln[1] <= 'z') {
                        ++instr;
                        if (ln.find("exec_lo") != std::string_view::npos || ln.find("vcc_lo") != std::string_view::npos) ++w32;
                        else if (ln.find("exec") != std::string_view::npos || ln.find("vcc") != std::string_view::npos) ++w64;
                    }
                    pos = e + 1;
                }
                csv.pop_back();  // the line's newline
                csv += ",\"instr=" + std::to_string(instr) + "\",\"wave=" + (w64 > w32 ? "64" : "32") + "\"\n";
            }
            if (irs[k].isText && textBytes.fetch_add(irs[k].dataSize) < textCap) {
                out << "----- begin " << irs[k].name << "\n";
                out.write(static_cast<const char*>(irs[k].pData), static_cast<std::streamsize>(irs[k].dataSize));
                out << "\n----- end " << irs[k].name << "\n";
            } else {
                const std::filesystem::path bin = std::filesystem::path(state().dir) / (std::string(head) + safeName(label) + "_e" + std::to_string(i) + "_" + safeName(irs[k].name) + ".bin");
                std::ofstream(bin, std::ios::binary).write(static_cast<const char*>(irs[k].pData), static_cast<std::streamsize>(irs[k].dataSize));
            }
        }
    }
    const std::lock_guard lock(csvMutex);
    std::ofstream(std::filesystem::path(state().dir) / "stats.csv", std::ios::app | std::ios::binary) << csv;
}

}  // namespace rvk::vk::isa
