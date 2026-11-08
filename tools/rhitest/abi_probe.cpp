// The ABI probe (docs/research/hlsl-rhi.md "ABI probe"): for EVERY block C++ and HLSL share (src/shader/abi_blocks.cpp, one field description each) a
// generated compute shader declares the block through the production binding path (push constants / root constants, a pointer pair read through
// rvkPtr, raw loads), reads every field and stores each 32-bit word raw into a storage image; the host fills the C++ struct with a unique pattern per
// word (0xB0000000 | byte offset), dispatches on the backend under test and compares: a word that arrives from another offset names the offset it
// was read from. Same code on Vulkan and D3D12 (the same HLSL text compiled to SPIR-V / DXIL).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "rhi/rhi.h"
#include "shader/abi.h"
#include "shader/abi_blocks.h"
#include "shader/dxc.h"

namespace rvk::abiprobe {

namespace {

using namespace shader::abiblocks;

constexpr std::uint32_t kPattern = 0xB0000000u;
constexpr std::uint32_t kImageWidth = 4096;

std::uint32_t pattern(std::uint32_t offset) { return kPattern | offset; }

std::string describe(std::uint32_t got) {
    if ((got & 0xF0000000u) == kPattern) return std::format("the word at byte {}", got & 0x0FFFFFFFu);
    return std::format("garbage 0x{:08x}", got);
}

struct Expected {
    std::uint32_t value;
    const char* field;
    std::uint32_t element;
    std::uint32_t component;
};

// the value of every word the probe stores, in logical order
std::vector<Expected> expectedWords(const Block& b) {
    std::vector<Expected> out;
    for (const Field& f : b.list()) {
        const TypeInfo t = parseType(f.type);
        if (t.frameRef) {
            // the probe puts frameBlock.time.x / .y of the frame block the pointer leads to
            out.push_back({pattern(static_cast<std::uint32_t>(offsetof(shader::abi::FrameBlock, time))), f.name, 0, 0});
            out.push_back({pattern(static_cast<std::uint32_t>(offsetof(shader::abi::FrameBlock, time)) + 4), f.name, 0, 1});
            continue;
        }
        for (std::uint32_t e = 0; e < f.count; ++e) {
            for (std::uint32_t k = 0; k < t.comps; ++k) out.push_back({pattern(f.cppOffset + e * t.comps * 4 + 4 * k), f.name, e, k});
        }
    }
    return out;
}

const char* backendName(rhi::Device& dev) { return rhi::backendName(dev.caps().backend); }

}  // namespace

int run(rhi::Device& dev) {
    const bool d3d12 = dev.caps().backend == rhi::Backend::D3D12;
    const std::string tag = std::string("[") + backendName(dev) + "] ";
    if (!shader::dxc::available()) {
        std::printf("%sabi probe: SKIP (no DXC)\n", tag.c_str());
        return 0;
    }
    int failures = 0;
    int blocksOk = 0, wordsRead = 0, skipped = 0;

    auto out = dev.createTexture({VK_IMAGE_TYPE_2D, VK_FORMAT_R32_UINT, {kImageWidth, 1, 1}, 1, 1, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "abi probe out"});
    const std::uint32_t pitch = rhi::bufferRowPitch(VK_FORMAT_R32_UINT, kImageWidth);
    auto readback = dev.createBuffer({pitch, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "abi probe readback"});
    // the frame block a header's frame pointer leads to: the pattern, so time.x / time.y name their own offsets
    auto frame = dev.createBuffer({sizeof(shader::abi::FrameBlock), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, "abi probe frame block"});
    if (!out || !readback || !frame) {
        std::printf("%sabi probe: FAIL (resources)\n", tag.c_str());
        return 1;
    }
    {
        auto* w = static_cast<std::uint32_t*>(frame->map());
        for (std::uint32_t i = 0; i < sizeof(shader::abi::FrameBlock) / 4; ++i) w[i] = pattern(i * 4);
    }
    const auto frameRef = frame->shaderRef(0);

    // every described block, then the mutant: a push block whose C++ twin is packed naively (uint at 0, uint2 at 4, uint4 at 12): both backends re-align it
    static const Field kMutantFields[] = {{"uint", "a", 1, 0}, {"uint2", "b", 1, 4}, {"uint4", "c", 1, 12}};
    std::vector<Block> all(blocks().begin(), blocks().end());
    all.push_back({"AbiMutantPush", "(packed C++ twin)", Kind::Push, 28, kMutantFields, 3});
    int mutantWords = 0;
    for (const Block& b : all) {
        const std::vector<Expected> want = expectedWords(b);
        if (want.size() != probeWordCount(b) || want.size() > kImageWidth) {
            std::printf("  FAIL: %s: probe word count %zu / %u\n", b.name, want.size(), probeWordCount(b));
            ++failures;
            continue;
        }
        // a frame pointer read from the wrong place would be a garbage address: refuse the dispatch when the layout model says the pointer is misplaced
        if (b.kind == Kind::Push) {
            const Rules r = rulesOf(b.kind);
            const auto model = wordOffsets(decls(b), d3d12 ? r.d3d12 : r.vulkan);
            const auto cpp = cppWordOffsets(b);
            bool safe = model.size() == cpp.size();
            std::size_t w = 0;
            for (const Field& f : b.list()) {
                const TypeInfo t = parseType(f.type);
                if (safe && t.frameRef && (model[w] != cpp[w] || model[w + 1] != cpp[w + 1])) safe = false;
                w += static_cast<std::size_t>(t.comps) * f.count;
            }
            if (!safe) {
                std::printf("  FAIL: %s: the frame pointer is predicted at another offset than C++ puts it (%s layout): GPU read skipped\n", b.name, backendName(dev));
                ++failures;
                ++skipped;
                continue;
            }
        }

        // the generated probe, for this backend's target
        shader::dxc::Request req;
        req.file = std::format("test/abi_probe_{}.comp.hlsl", b.name);
        req.source = probeHlsl(b);
        req.entry = "CSMain";
        req.profile = "cs_6_6";
        req.cache = false;
        req.optLevel = 1;
        req.target = d3d12 ? shader::dxc::Target::Dxil : shader::dxc::Target::Spirv;
        const shader::dxc::Result res = shader::dxc::compile(req);
        if (!res.ok()) {
            std::printf("  FAIL: %s: probe did not compile:\n%s\n", b.name, res.log.c_str());
            ++failures;
            continue;
        }
        rhi::Shader sh;
        if (d3d12) sh.dxil = res.bytes;
        else sh.spirv = res.words;
        const std::string pipeName = std::string("abi probe ") + b.name;
        auto pipeline = dev.createComputePipeline({sh, pipeName.c_str()});
        if (!pipeline) {
            std::printf("  FAIL: %s: pipeline creation\n", b.name);
            ++failures;
            continue;
        }

        // the C++ block, every word a pattern; a Push block travels as push constants of its C++ size, the others sit in a buffer the push block points at
        std::vector<std::uint32_t> pushWords;
        std::unique_ptr<rhi::Buffer> data;
        if (b.kind == Kind::Push) {
            pushWords.assign((b.cppSize + 3) / 4, 0);
            for (std::size_t i = 0; i < pushWords.size(); ++i) pushWords[i] = pattern(static_cast<std::uint32_t>(i * 4));
            for (const Field& f : b.list()) {
                if (parseType(f.type).frameRef) {
                    pushWords[f.cppOffset / 4] = frameRef[0];
                    pushWords[f.cppOffset / 4 + 1] = frameRef[1];
                }
            }
        } else {
            // over-allocated and patterned to the end: a read that strays shows its offset, a D3D12 read past the end is 0
            const std::uint32_t bytes = (b.cppSize * 2 + 4096 + 15) / 16 * 16;
            const std::string dataName = std::string("abi probe data ") + b.name;
            data = dev.createBuffer({bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Upload, dataName.c_str()});
            if (!data) {
                std::printf("  FAIL: %s: buffer creation\n", b.name);
                ++failures;
                continue;
            }
            auto* w = static_cast<std::uint32_t*>(data->map());
            for (std::uint32_t i = 0; i < bytes / 4; ++i) w[i] = pattern(i * 4);
            const auto ref = data->shaderRef(0);
            pushWords = {ref[0], ref[1]};
        }

        dev.submitAndWait([&](rhi::CommandList& cl) {
            cl.transition(*out, VK_IMAGE_LAYOUT_GENERAL);
            cl.bindPipeline(*pipeline);
            cl.setPushConstants(pushWords.data(), static_cast<std::uint32_t>(pushWords.size() * 4));
            rhi::Texture* storage[1] = {out.get()};
            cl.setStorageTextures(storage);
            cl.dispatch(1, 1, 1);
            cl.transition(*out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            cl.copyTextureToBuffer(*readback, 0, *out);
        });
        const auto* got = static_cast<const std::uint32_t*>(readback->map());
        int bad = 0;
        const bool mutant = &b == &all.back();
        for (std::size_t i = 0; i < want.size(); ++i) {
            if (got[i] == want[i].value) continue;
            if (mutant) {
                ++bad;
                continue;
            }
            if (++bad <= 6) {
                const Expected& e = want[i];
                std::printf("  FAIL: %s.%s%s%s: C++ has it at %s, the GPU read %s\n", b.name, e.field, e.element > 0 ? std::format("[{}]", e.element).c_str() : "",
                            std::format(".{}", "xyzw"[std::min(e.component, 3u)]).c_str(), describe(e.value).c_str(), describe(got[i]).c_str());
            }
        }
        if (bad > 6) std::printf("  ... %d more word(s) of %s differ\n", bad - 6, b.name);
        if (mutant) {
            // the proof that the probe bites: a block laid out as a naively packed C++ struct (uint, uint2, uint4) must NOT read back
            if (bad == 0) {
                std::printf("  FAIL: the mutant block (uint + uint2 + uint4 packed to 4 bytes) read back exactly: the probe does not detect packing mismatches\n");
                ++failures;
            } else {
                mutantWords = bad;
            }
        } else if (bad != 0) {
            ++failures;
        } else {
            ++blocksOk;
            wordsRead += static_cast<int>(want.size());
        }
    }
    std::printf("%sabi probe: %d of %zu blocks read back exactly (%d words), %d skipped; mutant block detected (%d of 7 words misread); %d failed: %s\n", tag.c_str(),
                blocksOk, blocks().size(), wordsRead, skipped, mutantWords, failures, failures == 0 ? "PASS" : "FAIL");
    return failures;
}

}  // namespace rvk::abiprobe
