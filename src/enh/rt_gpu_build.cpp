#include "enh/rt_gpu_build.h"

namespace rvk::enh::rtbuild {

namespace abi = shader::abi;

void compose(const Header& h, std::uint32_t k, const Entry& e, const InstanceInput* inputs, const BlasRow* blas, const MaterialRow* materials,
             const Literal* literals, VkAccelerationStructureInstanceKHR& vi, abi::RtGeometryRecord& r) {
    vi = VkAccelerationStructureInstanceKHR{};
    r = abi::RtGeometryRecord{};
    if ((e.input & kEntryLiteral) != 0) {
        const std::uint32_t i = e.input & ~kEntryLiteral;
        if (literals == nullptr || i >= h.literals) return;
        vi = literals[i].vi;
        r = literals[i].r;
        return;
    }
    if (e.input >= h.inputCap) return;
    const InstanceInput& in = inputs[e.input];
    if (in.blasRow >= h.blasCap || in.material >= h.matCap) return;
    const BlasRow& b = blas[in.blasRow];
    const MaterialRow& m = materials[in.material];
    const bool alpha = (e.mode & kModeAlpha) != 0;
    const bool glass = (e.mode & kModeGlass) != 0;
    const std::uint32_t variant = e.mode & kModeVariantMask;
    const std::uint32_t segCount = e.mode >> kModeSegShift;

    std::memcpy(vi.transform.matrix, in.rows, sizeof(vi.transform.matrix));
    vi.instanceCustomIndex = k;
    vi.mask = glass ? abi::kRtMaskGlass : alpha ? abi::kRtMaskAlphaTested : abi::kRtMaskOpaque;
    vi.instanceShaderBindingTableRecordOffset = 0;
    vi.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | (alpha ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR : VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR);
    vi.accelerationStructureReference = static_cast<std::uint64_t>(b.as[0]) | (static_cast<std::uint64_t>(b.as[1]) << 32);

    r.vertices[0] = b.vertices[0];
    r.vertices[1] = b.vertices[1];
    r.indices[0] = b.indices[0];
    r.indices[1] = b.indices[1];
    r.stride = b.stride;
    r.flags = (in.flags & ~abi::kRtRecordAlphaTested) | (alpha ? abi::kRtRecordAlphaTested : 0u) | (((in.data >> 16) & 0xFFu) << abi::kRtRecordKindShift);
    r.normal = b.normal;
    r.texcoord0 = b.texcoord0;
    r.texcoord1 = b.texcoord1;
    r.texcoord6 = b.texcoord6;
    r.color0 = b.color0;
    r.instanceId = in.instanceId;
    r.alpha = recordAlpha(in.flags, in.data, m.alphaWord) | (m.packLod << abi::kRtAlphaPackLodShift);
    r.albedoTex = m.albedoTex;
    r.albedoSampler = m.albedoSampler;
    r.packWord = m.packWord;
    r.uvSet = m.uvSet;
    std::memcpy(r.uvMap, m.uvMap, sizeof(r.uvMap));
    if (segCount != 0) {
        r.segments[0] = e.seg[0];
        r.segments[1] = e.seg[1];
        r.segmentCount = segCount;
        r.flags |= abi::kRtRecordSegmented;
    }
    if (variant == 1) {
        vi.accelerationStructureReference = static_cast<std::uint64_t>(b.proxyAs[0]) | (static_cast<std::uint64_t>(b.proxyAs[1]) << 32);
        r.indices[0] = b.proxyIndices[0];
        r.indices[1] = b.proxyIndices[1];
    } else if (variant == 2) {
        std::memcpy(vi.transform.matrix, in.boxRows, sizeof(vi.transform.matrix));
        vi.accelerationStructureReference = static_cast<std::uint64_t>(h.boxAs[0]) | (static_cast<std::uint64_t>(h.boxAs[1]) << 32);
        r.vertices[0] = h.boxVertices[0];
        r.vertices[1] = h.boxVertices[1];
        r.indices[0] = h.boxIndices[0];
        r.indices[1] = h.boxIndices[1];
        r.stride = 12;
        r.flags &= ~(abi::kRtRecordIndex32 | abi::kRtRecordAlphaTested | abi::kRtRecordSpeedTree | abi::kRtRecordInstanced);
        r.normal = r.texcoord0 = r.texcoord1 = r.texcoord6 = r.color0 = abi::kRtAttribAbsent;
        r.albedoTex = 0;
        r.alpha = 8u;
        r.uvSet = abi::kRtUvNoTexture | abi::kRtUvOpacityOpaque;
        std::memset(r.uvMap, 0, sizeof(r.uvMap));
        std::memcpy(&r.uvMap[0][3], &kBoxColour, 4);
    }
}

}  // namespace rvk::enh::rtbuild
