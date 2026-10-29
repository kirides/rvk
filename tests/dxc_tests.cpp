// Runtime HLSL pipeline (shader/dxc): embedded sources + include resolution, SPIR-V that passes spirv-val, the disk cache and its
// include hashes. Skipped without dxcompiler.dll next to the test executable (the CMake build copies the vcpkg port's).

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <format>
#include <map>
#include <filesystem>
#include <string>

#include <spirv-tools/libspirv.hpp>

#include "enh/debug_view.h"
#include "shader/abi.h"
#include "shader/dxc.h"
#include "shader/embedded.h"
#include "spirv_offsets.h"
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

// The HLSL ABI (shaders/include/rvk_abi.hlsli) against abi.h and the debug view's C++ push block: offsets from the compiled SPIR-V
TEST(abi_hlsl_layout_matches_cpp) {
    if (!dxc::available()) return;
    const auto words = embedded::spirv("test/abi_frame_layout.comp");
    CHECK(!words.empty());
    if (words.empty()) return;
    const auto m = rvk::test::memberOffsets(words);
    CHECK(m.contains("RvkFrameData") && m.contains("type.PushConstant.DebugViewPush"));
    if (!m.contains("RvkFrameData") || !m.contains("type.PushConstant.DebugViewPush")) return;
    const auto check = [](const std::map<std::string, std::uint32_t>& got, const std::map<std::string, std::size_t>& want, const char* what) {
        CHECK_MSG(got.size() == want.size(), std::format("{}: {} members in HLSL, {} in C++", what, got.size(), want.size()).c_str());
        for (const auto& [name, off] : want) {
            const auto it = got.find(name);
            CHECK_MSG(it != got.end() && it->second == off,
                      std::format("{}.{}: C++ offset {}, HLSL {}", what, name, off, it != got.end() ? std::to_string(it->second) : "missing").c_str());
        }
    };
    using FB = abi::FrameBlock;
    check(m.at("RvkFrameData"),
          {{"viewProj", offsetof(FB, viewProj)}, {"invViewProj", offsetof(FB, invViewProj)}, {"view", offsetof(FB, view)},
           {"proj", offsetof(FB, proj)}, {"invView", offsetof(FB, invView)}, {"invProj", offsetof(FB, invProj)},
           {"prevViewProj", offsetof(FB, prevViewProj)}, {"prevInvViewProj", offsetof(FB, prevInvViewProj)},
           {"cameraPos", offsetof(FB, cameraPos)}, {"prevCameraPos", offsetof(FB, prevCameraPos)},
           {"depthParams", offsetof(FB, depthParams)}, {"viewport", offsetof(FB, viewport)}, {"jitter", offsetof(FB, jitter)},
           {"time", offsetof(FB, time)}, {"frameInfo", offsetof(FB, frameInfo)}, {"images", offsetof(FB, images)},
           {"params", offsetof(FB, params)}, {"rtScene", offsetof(FB, rtScene)}, {"rtInfo", offsetof(FB, rtInfo)},
           {"gi", offsetof(FB, gi)}, {"ao", offsetof(FB, ao)}, {"sgi", offsetof(FB, sgi)}},
          "RvkFrameData");
    using P = rvk::enh::DebugViewPush;
    check(m.at("type.PushConstant.DebugViewPush"),
          {{"frame", offsetof(P, h) + offsetof(abi::PassPushHeader, frame)},
           {"passId", offsetof(P, h) + offsetof(abi::PassPushHeader, passId)},
           {"passFlags", offsetof(P, h) + offsetof(abi::PassPushHeader, passFlags)}, {"depthTex", offsetof(P, depthTex)},
           {"width", offsetof(P, width)}, {"height", offsetof(P, height)}, {"srcTex", offsetof(P, srcTex)},
           {"srcSampler", offsetof(P, srcSampler)}, {"pad", offsetof(P, pad)}},
          "DebugViewPush");
}
