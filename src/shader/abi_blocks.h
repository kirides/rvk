#pragma once

// The ABI block registry (docs/research/hlsl-rhi.md "ABI probe"): ONE field description per block that C++ and HLSL share (push blocks, blocks
// read through a RVK_REF / rvkPtr, raw records), in src/shader/abi_blocks.cpp. Each field = {HLSL type, name, element count, C++ byte offset};
// the C++ offset is an offsetof() of the production struct, so a renamed / removed member does not compile. Three consumers:
//   - tools/rhitest/abi_probe.cpp generates a test-only compute shader per block FROM the description (the block declared through the same
//     binding path the production shaders use), has the GPU read every field on Vulkan and D3D12 and compares what it read with the C++ memory;
//   - tests/abi_blocks_tests.cpp checks the description against the layout models below on both backends, parses every production HLSL struct
//     that is used as a push block / RVK_REF / rvkPtr (shaders/**, not shaders/test) and requires it to match its description (an HLSL
//     struct without a description fails: add the block to abi_blocks.cpp);
//   - the models are what the probe uses to refuse a GPU read that would start from a garbage address (a mismatching RVK_REF).
// Layout rules (what the compilers do; the probe confirms them on the real compilers):
//   Vulkan push constants / RVK_REF blocks (DXC, std430-like): scalar 4 / 4, vecN size 4N, alignment 4 / 8 / 16 / 16, arrays of vectors stride = size
//     rounded to the alignment, a pointer member 8 / 8.
//   D3D12 push constants (root constants = an HLSL constant buffer): a vector never straddles a 16-byte boundary (it moves to the next one), an
//     array / struct / RVK_REF member starts on 16 bytes and every array element has a 16-byte stride.
//   D3D12 RVK_REF blocks / rvkRawLoad (ByteAddressBuffer.Load<T>): natural 4-byte packing, no 16-byte rule.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rvk::shader::abiblocks {

enum class Kind : std::uint8_t {
    Push,  // RVK_PUSH(T, pc): push constants (Vulkan) / root constants (D3D12)
    Ptr,   // a block in a buffer reached by rvkPtr<T, 16>(pair).Get() / RVK_REF(T)
    Raw,   // a record read by rvkRawLoad<T, A> at constant offsets (no HLSL struct): the offsets must keep the alignment
};

struct Field {
    const char* type;           // HLSL: uint / int / float, vector 2..4, or "frameref" (RVK_REF(RvkFrameData): the header's frame pointer)
    const char* name;           // the HLSL member name (a flattened path for nested structs: cascade0_origin)
    std::uint32_t count;        // array elements (1 = not an array)
    std::uint32_t cppOffset;    // byte offset of the first element in the C++ struct
};

struct Block {
    const char* name;           // the HLSL struct name
    const char* cpp;            // the C++ type (for messages)
    Kind kind;
    std::uint32_t cppSize;      // sizeof the C++ struct
    const Field* fields;
    std::size_t fieldCount;
    std::span<const Field> list() const { return {fields, fieldCount}; }
};

std::span<const Block> blocks();
const Block* findBlock(std::string_view name);

// ---- types and layout models -----------------------------------------------------------------------------------------------------
struct TypeInfo {
    bool ok = false;
    bool frameRef = false;
    char base = 'u';            // u / i / f
    std::uint32_t comps = 1;    // 1..4 (a frameRef counts 2 words)
};
TypeInfo parseType(std::string_view type);

enum class Rule : std::uint8_t { VulkanStd430, D3D12Cbuffer, Scalar };
struct Decl {
    std::string type;
    std::uint32_t count = 1;
};
// the byte offset of every 32-bit word of the declarations in logical order (field after field, element after element, component after component)
// under `rule`; `size` = the block's size in that layout. Empty on an unknown type.
std::vector<std::uint32_t> wordOffsets(const std::vector<Decl>& decls, Rule rule, std::uint32_t* size = nullptr);
std::vector<Decl> decls(const Block& b);
// the offsets C++ puts the words at (cppOffset + element * stride + 4 * component; vectors packed, arrays of them tight)
std::vector<std::uint32_t> cppWordOffsets(const Block& b);
// the rule of a block's kind per backend: {Vulkan, D3D12}
struct Rules {
    Rule vulkan, d3d12;
};
Rules rulesOf(Kind k);

// ---- the probe shader ----------------------------------------------------------------------------------------------------------------
// HLSL text of the probe of `b` (entry CSMain, includes "abi_probe.hlsli" = shaders/test/abi_probe.hlsli): declares the block through the production
// binding path (Push: RVK_PUSH(Block, pc); Ptr: rvkPtr<Block, 16>(pc.ptr).Get(); Raw: rvkRawLoad at the C++ offsets), reads every field and stores
// the raw 32-bit words with asuint into the output image u0 / space1 in logical order.
std::string probeHlsl(const Block& b);
// words the probe writes
std::uint32_t probeWordCount(const Block& b);

// ---- the lint: production HLSL structs against the descriptions --------------------------------------------------------------------
struct LintResult {
    std::vector<std::string> errors;  // human-readable
    std::uint32_t structsChecked = 0;
    std::uint32_t usesFound = 0;
};
// `sources` = every HLSL file (name, text); files under test/ are skipped
struct Source {
    std::string_view name;
    std::string_view text;
};
LintResult lintSources(std::span<const Source> sources);

}  // namespace rvk::shader::abiblocks
