// Runtime HLSL pipeline (shader/dxc): embedded sources + include resolution, SPIR-V that passes spirv-val, the disk cache and its
// include hashes. Skipped without dxcompiler.dll next to the test executable (the CMake build copies the vcpkg port's).

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <filesystem>
#include <string>

#include <spirv-tools/libspirv.hpp>

#include "enh/debug_view.h"
#include "enh/rt_debug.h"
#include "enh/shadows.h"
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

namespace {
// the same comparison for every HLSL layout test below
void checkHlsl(const std::map<std::string, std::uint32_t>& got, const std::map<std::string, std::size_t>& want, const char* what) {
    CHECK_MSG(got.size() == want.size(), std::format("{}: {} members in HLSL, {} in C++", what, got.size(), want.size()).c_str());
    for (const auto& [name, off] : want) {
        const auto it = got.find(name);
        CHECK_MSG(it != got.end() && it->second == off,
                  std::format("{}.{}: C++ offset {}, HLSL {}", what, name, off, it != got.end() ? std::to_string(it->second) : "missing").c_str());
    }
}
}  // namespace

// the draw ABI (shaders/include/rvk_draw.hlsli: push block, StageConsts, DrawBlock)
TEST(abi_hlsl_draw_layout_matches_cpp) {
    if (!dxc::available()) return;
    const auto words = embedded::spirv("test/abi_layout.comp");
    CHECK(!words.empty());
    if (words.empty()) return;
    const auto m = rvk::test::memberOffsets(words);
    CHECK(m.contains("type.PushConstant.RvkDrawPush") && m.contains("RvkStageData") && m.contains("RvkDrawData"));
    if (!m.contains("type.PushConstant.RvkDrawPush") || !m.contains("RvkStageData") || !m.contains("RvkDrawData")) return;
    using PC = abi::PushConstants;
    checkHlsl(m.at("type.PushConstant.RvkDrawPush"),
              {{"vs", offsetof(PC, vsConsts)}, {"ps", offsetof(PC, psConsts)}, {"draw", offsetof(PC, draw)}, {"frame", offsetof(PC, frame)},
               {"prevVs", offsetof(PC, prevVsConsts)}, {"drawId", offsetof(PC, drawId)}, {"flags", offsetof(PC, flags)},
               {"vsw0", offsetof(PC, vsWin)}, {"vsw1", offsetof(PC, vsWin) + 8}, {"vsw2", offsetof(PC, vsWin) + 16}, {"vsw3", offsetof(PC, vsWin) + 24},
               {"psw0", offsetof(PC, psWin)}, {"psw1", offsetof(PC, psWin) + 8}, {"psw2", offsetof(PC, psWin) + 16}, {"psw3", offsetof(PC, psWin) + 24}},
              "RvkDrawPush");
    checkHlsl(m.at("RvkStageData"), {{"c", 0}, {"i", abi::kStageConstsIntOffset}, {"b", abi::kStageConstsBoolOffset}}, "RvkStageData");
    using DB = abi::DrawBlock;
    // alpha = {alphaFunc, alphaRef, pad0, pad1}
    CHECK(offsetof(DB, alphaRef) == offsetof(DB, alphaFunc) + 4 && offsetof(DB, pad) == offsetof(DB, alphaFunc) + 8);
    checkHlsl(m.at("RvkDrawData"),
              {{"tex", offsetof(DB, tex)}, {"smp", offsetof(DB, smp)}, {"posFix", offsetof(DB, posFix)}, {"alpha", offsetof(DB, alphaFunc)},
               {"psC5", offsetof(DB, psC5)}},
              "RvkDrawData");
}

// the RT scene's geometry record (shaders/include/rvk_rt.hlsli: seven uint4) and the RT debug view's push block
TEST(abi_hlsl_rt_layout_matches_cpp) {
    if (!dxc::available()) return;
    const auto words = embedded::spirv("test/abi_rt_layout.comp");
    CHECK(!words.empty());
    if (words.empty()) return;
    const auto m = rvk::test::memberOffsets(words);
    CHECK(m.contains("RvkRtRecordData") && m.contains("RvkRtSegmentData") && m.contains("type.PushConstant.RtDebugPush"));
    if (!m.contains("RvkRtRecordData") || !m.contains("RvkRtSegmentData") || !m.contains("type.PushConstant.RtDebugPush")) return;
    using R = abi::RtGeometryRecord;
    // the HLSL record is w0..w6 = the C++ record's 16-byte rows; inside a row the C++ fields must follow in the order the shader unpacks
    CHECK(offsetof(R, indices) == offsetof(R, vertices) + 8 && offsetof(R, flags) == offsetof(R, stride) + 4 &&
          offsetof(R, normal) == offsetof(R, stride) + 8 && offsetof(R, texcoord0) == offsetof(R, stride) + 12 &&
          offsetof(R, texcoord6) == offsetof(R, texcoord1) + 4 && offsetof(R, color0) == offsetof(R, texcoord1) + 8 &&
          offsetof(R, instanceId) == offsetof(R, texcoord1) + 12 && offsetof(R, albedoSampler) == offsetof(R, albedoTex) + 4 &&
          offsetof(R, alpha) == offsetof(R, albedoTex) + 8 && offsetof(R, uvSet) == offsetof(R, albedoTex) + 12 &&
          offsetof(R, segmentCount) == offsetof(R, segments) + 8);
    checkHlsl(m.at("RvkRtRecordData"),
              {{"w0", offsetof(R, vertices)}, {"w1", offsetof(R, stride)}, {"w2", offsetof(R, texcoord1)}, {"w3", offsetof(R, albedoTex)},
               {"uvMap0", offsetof(R, uvMap)}, {"uvMap1", offsetof(R, uvMap) + 16}, {"w6", offsetof(R, segments)}},
              "RvkRtRecordData");
    using S = abi::RtMaterialSegment;
    CHECK(offsetof(S, primitiveCount) == offsetof(S, firstPrimitive) + 4 && offsetof(S, albedoTex) == offsetof(S, firstPrimitive) + 8 &&
          offsetof(S, albedoSampler) == offsetof(S, firstPrimitive) + 12 && offsetof(S, uvSet) == offsetof(S, alpha) + 4);
    checkHlsl(m.at("RvkRtSegmentData"),
              {{"w0", offsetof(S, firstPrimitive)}, {"w1", offsetof(S, alpha)}, {"uvMap0", offsetof(S, uvMap)}, {"uvMap1", offsetof(S, uvMap) + 16}},
              "RvkRtSegmentData");
    using P = rvk::enh::RtDebugPush;
    checkHlsl(m.at("type.PushConstant.RtDebugPush"),
              {{"frame", offsetof(P, h) + offsetof(abi::PassPushHeader, frame)},
               {"passId", offsetof(P, h) + offsetof(abi::PassPushHeader, passId)},
               {"passFlags", offsetof(P, h) + offsetof(abi::PassPushHeader, passFlags)}, {"depthTex", offsetof(P, depthTex)},
               {"width", offsetof(P, width)}, {"height", offsetof(P, height)}, {"srcTex", offsetof(P, srcTex)},
               {"srcSampler", offsetof(P, srcSampler)}, {"mode", offsetof(P, mode)}, {"probes", offsetof(P, probes)}},
              "RtDebugPush");
}

// the shadow mask's block (shaders/shadows/shadow_block.hlsli) against enh::ShadowBlock
TEST(shadows_block_hlsl_matches_cpp) {
    if (!dxc::available()) return;
    const auto words = embedded::spirv("test/shadow_layout.comp");
    CHECK(!words.empty());
    if (words.empty()) return;
    const auto m = rvk::test::memberOffsets(words);
    CHECK(m.contains("ShadowBlock"));
    if (!m.contains("ShadowBlock")) return;
    using B = rvk::enh::ShadowBlock;
    checkHlsl(m.at("ShadowBlock"),
              {{"cascade", offsetof(B, cascade)}, {"split", offsetof(B, split)}, {"texel", offsetof(B, texel)},
               {"maps", offsetof(B, maps)}, {"res", offsetof(B, res)}, {"vsm", offsetof(B, vsm)},
               {"vsmParams", offsetof(B, vsmParams)}, {"params", offsetof(B, params)}, {"target", offsetof(B, target)},
               {"mode", offsetof(B, mode)}, {"rt", offsetof(B, rt)}, {"rtParams", offsetof(B, rtParams)},
               {"depthRemap", offsetof(B, depthRemap)}},
              "ShadowBlock");
    // the old bug: the vertex shader read DrawBlock.posFix from this block; a ShadowBlock member must never sit where a DrawBlock's posFix does unless it is the remap itself
    CHECK(offsetof(B, depthRemap) != offsetof(abi::DrawBlock, posFix));
}

// every embedded HLSL shader (a name with a stage extension) compiles and validates: the CI guard of the port
TEST(dxc_every_embedded_shader_compiles) {
    if (!dxc::available()) return;
    std::size_t n = 0;
    for (const auto& f : embedded::hlslSources()) {
        std::string_view name = f.name;
        if (!name.ends_with(".hlsl")) continue;
        name.remove_suffix(5);
        if (!(name.ends_with(".comp") || name.ends_with(".vert") || name.ends_with(".frag"))) continue;
        ++n;
        const auto words = embedded::spirv(name);
        CHECK_MSG(!words.empty(), std::string(name).c_str());
        if (words.empty()) continue;
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
        CHECK_MSG(tools.Validate(words.data(), words.size()), std::string(name).c_str());
    }
    CHECK(n >= 30);
}

// DXIL parity (docs/research/hlsl-rhi.md "DXIL parity (S1)"): every embedded shader compiles to DXIL SM 6.6 signed by dxil.dll and fits the DX12 backend's fixed
// root signature (push constants <= 128 bytes at b0, the root CBV at b8 space1, storage images u0..u7 space1; everything else from the heaps). rt_query is the
// Vulkan floor's ray query self-test with its own descriptor layout (rvk_shadertool dxil-check lists the same exemption).
TEST(dxil_every_embedded_shader_compiles_signed) {
    if (!dxc::available()) return;
    std::size_t n = 0;
    for (const auto& f : embedded::hlslSources()) {
        std::string_view name = f.name;
        if (!name.ends_with(".hlsl")) continue;
        name.remove_suffix(5);
        if (!(name.ends_with(".comp") || name.ends_with(".vert") || name.ends_with(".frag"))) continue;
        ++n;
        const auto bytes = embedded::dxil(name);
        CHECK_MSG(!bytes.empty(), std::string(name).c_str());
        if (bytes.empty()) continue;
        CHECK_MSG(bytes.size() > 20 && std::memcmp(bytes.data(), "DXBC", 4) == 0, std::string(name).c_str());
        if (name == "test/rt_query.comp") continue;
        const std::string violation = dxc::dxilContractViolation(bytes);
        CHECK_MSG(violation.empty(), (std::string(name) + ": " + violation).c_str());
    }
    CHECK(n >= 30);
}

