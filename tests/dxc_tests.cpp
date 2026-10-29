// Runtime HLSL pipeline (shader/dxc): embedded sources + include resolution, SPIR-V that passes spirv-val, the disk cache and its
// include hashes. Skipped without dxcompiler.dll next to the test executable (the CMake build copies the vcpkg port's).

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include <spirv-tools/libspirv.hpp>

#include "shader/dxc.h"
#include "shader/embedded.h"
#include "test.h"

using namespace rvk::shader;

namespace {

bool validSpirv(const std::vector<std::uint32_t>& words, std::string* message = nullptr) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* m) {
        if (message) *message += m;
    });
    return tools.Validate(words);
}

dxc::Request probeRequest() {
    dxc::Request r;
    r.file = "test/dxc_probe.comp.hlsl";
    r.entry = "CSMain";
    r.profile = "cs_6_6";
    return r;
}

}  // namespace

TEST(dxc_embedded_sources) {
    CHECK(!embedded::hlslSource("test/dxc_probe.comp.hlsl").empty());
    CHECK(!embedded::hlslSource("include/rvk_probe.hlsli").empty());
    CHECK(embedded::hlslSource("nope.hlsl").empty());
}

TEST(dxc_probe_compiles_to_valid_spirv) {
    if (!dxc::available()) {
        std::printf("  SKIP: %.*s\n", static_cast<int>(dxc::unavailableReason().size()), dxc::unavailableReason().data());
        return;
    }
    const dxc::Result r = dxc::compile(probeRequest());
    CHECK_MSG(r.ok(), r.log.c_str());
    if (!r.ok()) return;
    std::string message;
    CHECK_MSG(validSpirv(r.words, &message), message.c_str());
    CHECK(r.words[0] == 0x07230203u);
    // the name embedded::spirv() is asked for by the renderer
    CHECK(!embedded::spirv("test/dxc_probe.comp").empty());
}

TEST(dxc_error_is_reported) {
    if (!dxc::available()) return;
    dxc::Request r = probeRequest();
    r.entry = "NoSuchEntry";
    const dxc::Result bad = dxc::compile(r);
    CHECK(!bad.ok());
    CHECK(!bad.log.empty());
    r = probeRequest();
    r.file = "test/does_not_exist.hlsl";
    CHECK(!dxc::compile(r).ok());
}

TEST(dxc_disk_cache_follows_includes) {
    if (!dxc::available()) return;
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "rvk_dxc_cache_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    dxc::setCacheDirectory(dir);
    std::string include = "float rvkProbeValue(uint x, uint y) { return 1.0; }\n";
    dxc::setSourceReader([&](std::string_view path, std::string& text) {
        if (path == "test/dxc_probe.comp.hlsl") {
            text = "#include \"include/rvk_probe.hlsli\"\n[[vk::image_format(\"r32f\")]] RWTexture2D<float> o : register(u0);\n"
                   "[numthreads(8,8,1)] void CSMain(uint3 id : SV_DispatchThreadID) { o[id.xy] = rvkProbeValue(id.x, id.y); }\n";
            return true;
        }
        if (path == "include/rvk_probe.hlsli") {
            text = include;
            return true;
        }
        return false;
    });
    const dxc::Result first = dxc::compile(probeRequest());
    CHECK_MSG(first.ok(), first.log.c_str());
    CHECK(!first.fromCache);
    const dxc::Result second = dxc::compile(probeRequest());
    CHECK(second.ok());
    CHECK(second.fromCache);
    CHECK(second.words == first.words);
    include = "float rvkProbeValue(uint x, uint y) { return 2.0; }\n";  // the include changed: the entry is stale
    const dxc::Result third = dxc::compile(probeRequest());
    CHECK(third.ok());
    CHECK(!third.fromCache);
    CHECK(third.words != first.words);
    dxc::setSourceReader(nullptr);
    dxc::setCacheDirectory({});
    fs::remove_all(dir, ec);
}

TEST(dxc_debug_info_variant) {
    if (!dxc::available()) return;
    dxc::Request r = probeRequest();
    r.debugInfo = true;
    const dxc::Result dbg = dxc::compile(r);
    CHECK_MSG(dbg.ok(), dbg.log.c_str());
    if (!dbg.ok()) return;
    std::string message;
    CHECK_MSG(validSpirv(dbg.words, &message), message.c_str());
}
