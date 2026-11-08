// The ABI block registry (src/shader/abi_blocks.*, docs/research/hlsl-rhi.md "ABI probe"): the descriptions against the layout models of both
// backends, the production HLSL structs against the descriptions (a new struct used as a push block / RVK_REF / rvkPtr without a probe description
// fails here), and the generated probe shaders compile. The GPU read-back on both backends is `rvk_rhitest` (tools/rhitest/abi_probe.cpp).

#include <algorithm>
#include <cstdint>
#include <format>
#include <set>
#include <string>
#include <vector>

#include "shader/abi_blocks.h"
#include "shader/dxc.h"
#include "shader/embedded.h"
#include "test.h"

using namespace rvk;
using namespace rvk::shader::abiblocks;

TEST(abi_blocks_descriptions_match_the_layout_models) {
    CHECK(blocks().size() > 50);
    std::set<std::string> names;
    for (const Block& b : blocks()) {
        CHECK_MSG(names.insert(b.name).second, std::format("block {} is described twice", b.name).c_str());
        const std::vector<std::uint32_t> cpp = cppWordOffsets(b);
        CHECK_MSG(!cpp.empty(), std::format("{}: unknown type in a field", b.name).c_str());
        // every word inside the C++ struct, none twice
        std::set<std::uint32_t> seen;
        for (const std::uint32_t o : cpp) {
            CHECK_MSG(o + 4 <= b.cppSize, std::format("{}: a field word at byte {} is outside the {} byte struct {}", b.name, o, b.cppSize, b.cpp).c_str());
            CHECK_MSG(seen.insert(o).second, std::format("{}: byte {} is described by two fields", b.name, o).c_str());
        }
        if (b.kind != Kind::Raw) {
            // the probe reads the whole struct: padding included, nothing of the C++ struct left unread
            CHECK_MSG(seen.size() * 4 == b.cppSize, std::format("{}: the fields cover {} of the {} bytes of {}", b.name, seen.size() * 4, b.cppSize, b.cpp).c_str());
            const Rules r = rulesOf(b.kind);
            const std::vector<Decl> d = decls(b);
            const struct {
                Rule rule;
                const char* name;
            } backends[2] = {{r.vulkan, "Vulkan"}, {r.d3d12, "D3D12"}};
            for (const auto& be : backends) {
                const std::vector<std::uint32_t> hlsl = wordOffsets(d, be.rule);
                CHECK_MSG(hlsl.size() == cpp.size(), std::format("{}: word count differs on {}", b.name, be.name).c_str());
                for (std::size_t k = 0; k < std::min(hlsl.size(), cpp.size()); ++k) {
                    CHECK_MSG(hlsl[k] == cpp[k], std::format("{}: word {} is read at byte {} on {} but C++ has it at byte {}", b.name, k, hlsl[k], be.name, cpp[k]).c_str());
                }
            }
            if (b.kind == Kind::Push) CHECK_MSG(b.cppSize <= 128, std::format("{}: a push block over 128 bytes", b.name).c_str());
        } else {
            // rvkRawLoad<T, A> promises the alignment A (16 for a 4-component type)
            for (const Field& f : b.list()) {
                const TypeInfo t = parseType(f.type);
                const std::uint32_t a = t.comps == 1 ? 4 : t.comps == 2 ? 8 : 16;
                CHECK_MSG(f.cppOffset % a == 0, std::format("{}.{}: offset {} is not {}-aligned (rvkRawLoad<{}, {}>)", b.name, f.name, f.cppOffset, a, f.type, a).c_str());
            }
        }
    }
}

TEST(abi_blocks_hlsl_structs_match_their_descriptions) {
    std::vector<Source> sources;
    for (const shader::embedded::SourceFile& f : shader::embedded::hlslSources()) sources.push_back({f.name, f.text});
    CHECK(!sources.empty());
    if (sources.empty()) return;
    const LintResult r = lintSources(sources);
    for (const std::string& e : r.errors) std::printf("  abi lint: %s\n", e.c_str());
    CHECK_MSG(r.errors.empty(), std::format("{} ABI lint error(s)", r.errors.size()).c_str());
    CHECK(r.usesFound > 40);
    CHECK(r.structsChecked > 40);
}

TEST(abi_blocks_probe_shaders_compile) {
    if (!shader::dxc::available()) return;
    int failed = 0;
    for (const Block& b : blocks()) {
        shader::dxc::Request req;
        req.file = std::format("test/abi_probe_{}.comp.hlsl", b.name);
        req.source = probeHlsl(b);
        req.entry = "CSMain";
        req.profile = "cs_6_6";
        req.target = shader::dxc::Target::Spirv;
        req.cache = false;
        req.optLevel = 1;
        const shader::dxc::Result res = shader::dxc::compile(req);
        if (!res.ok()) {
            ++failed;
            std::printf("  probe of %s did not compile:\n%s\n", b.name, res.log.c_str());
        }
        CHECK(probeWordCount(b) > 0);
    }
    CHECK_MSG(failed == 0, std::format("{} probe shader(s) failed to compile", failed).c_str());
}

// the lint must really bite: a struct without a description, and the historical D3D12 packing bug (a uint2 before a uint4 in a push block)
TEST(abi_blocks_lint_detects_new_structs_and_packing_mismatches) {
    std::vector<std::string> keep;  // the modified texts
    keep.reserve(4);
    std::vector<Source> sources;
    for (const shader::embedded::SourceFile& f : shader::embedded::hlslSources()) sources.push_back({f.name, f.text});
    if (sources.empty()) return;
    // 1. a push block nobody described
    keep.push_back("struct BrandNewPush { RVK_PASS_PUSH_HEADER uint a; };\nRVK_PUSH(BrandNewPush, pc);\n");
    sources.push_back({"lint/brand_new.hlsl", keep.back()});
    {
        const LintResult r = lintSources(sources);
        bool found = false;
        for (const std::string& e : r.errors) found = found || (e.find("BrandNewPush") != std::string::npos && e.find("no ABI probe description") != std::string::npos);
        CHECK_MSG(found, "a struct used as a push block without a description must fail the lint");
    }
    sources.pop_back();
    // 2. LightShadowPush with `uint2 table` moved in front of `uint4 tex` (the layout that read the width as a heap index on D3D12)
    for (Source& s : sources) {
        if (s.name != "lighting/light_shadow_common.hlsli") continue;
        std::string t(s.text);
        const std::size_t a = t.find("uint2 table;");
        const std::size_t b = t.find("uint4 tex;");
        CHECK(a != std::string::npos && b != std::string::npos && b < a);
        if (a == std::string::npos || b == std::string::npos || b >= a) return;
        t.erase(a, std::string("uint2 table;").size());
        t.insert(b, "uint2 table; ");
        keep.push_back(std::move(t));
        s.text = keep.back();
    }
    const LintResult r = lintSources(sources);
    bool found = false;
    for (const std::string& e : r.errors) found = found || (e.find("struct LightShadowPush") != std::string::npos && e.find("D3D12") != std::string::npos);
    CHECK_MSG(found, "a uint2 before a uint4 in a push block must fail the lint on the D3D12 layout");
}

TEST(abi_blocks_layout_models_known_cases) {
    // D3D12 root constants: a vector never straddles 16 bytes, arrays / the frame pointer start on 16
    const auto d3d = [](std::vector<Decl> d) { return wordOffsets(d, Rule::D3D12Cbuffer); };
    const auto vk = [](std::vector<Decl> d) { return wordOffsets(d, Rule::VulkanStd430); };
    CHECK((d3d({{"uint2", 1}, {"uint4", 1}}) == std::vector<std::uint32_t>{0, 4, 16, 20, 24, 28}));
    CHECK((d3d({{"uint", 1}, {"uint2", 1}, {"uint", 1}}) == std::vector<std::uint32_t>{0, 4, 8, 12}));
    CHECK((d3d({{"uint3", 1}, {"uint2", 1}}) == std::vector<std::uint32_t>{0, 4, 8, 16, 20}));
    CHECK((d3d({{"uint", 1}, {"float4", 2}}) == std::vector<std::uint32_t>{0, 16, 20, 24, 28, 32, 36, 40, 44}));
    CHECK((d3d({{"frameref", 1}, {"uint", 1}, {"uint", 1}}) == std::vector<std::uint32_t>{0, 4, 8, 12}));
    CHECK((d3d({{"uint", 1}, {"frameref", 1}}) == std::vector<std::uint32_t>{0, 16, 20}));
    // Vulkan (std430): a uint2 is 8-aligned
    CHECK((vk({{"uint", 1}, {"uint2", 1}}) == std::vector<std::uint32_t>{0, 8, 12}));
    CHECK((vk({{"uint", 1}, {"uint4", 1}}) == std::vector<std::uint32_t>{0, 16, 20, 24, 28}));
    CHECK((wordOffsets({{"uint", 1}, {"uint2", 1}}, Rule::Scalar) == std::vector<std::uint32_t>{0, 4, 8}));
}
