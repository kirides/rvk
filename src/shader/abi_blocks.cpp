#include "shader/abi_blocks.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <format>
#include <map>
#include <set>

#include "enh/debug_view.h"
#include "enh/depth_z.h"
#include "enh/gbuffer.h"
#include "enh/gi.h"
#include "enh/picture_check.h"
#include "enh/light_table.h"
#include "enh/material_rt.h"
#include "enh/ocean.h"
#include "enh/rt_debug.h"
#include "enh/rt_shadows.h"
#include "enh/shadows.h"
#include "enh/water.h"
#include "enh/water_rt.h"
#include "gpuscene/cluster_table.h"
#include "gpuscene/instance_buffer.h"
#include "gpuscene/mesh_layout.h"
#include "native/own_post.h"
#include "shader/abi.h"
#include "shader/pass_push.h"

namespace rvk::shader::abiblocks {

namespace {

// StageConsts has no C++ struct (abi.h: three arrays at fixed offsets): the layout the HLSL RvkStageData reads
struct StageConstsLayout {
    float c[abi::kFloatConsts][4];
    std::int32_t i[abi::kIntConsts][4];
    std::uint32_t b[abi::kBoolConsts];
};
static_assert(offsetof(StageConstsLayout, i) == abi::kStageConstsIntOffset && offsetof(StageConstsLayout, b) == abi::kStageConstsBoolOffset &&
              sizeof(StageConstsLayout) == abi::kStageConstsSize);

}  // namespace

// ---- the descriptions ------------------------------------------------------------------------------------------------------------------
// BLOCK(HlslName, Kind, CppType) ... END(HlslName, Kind, CppType); inside: F(type, name, n) = the C++ member of the same name, FM(.., member) = a
// member of another name, FO(.., offset) = an explicit byte offset (a field that is a slice of an array / uint64 member), PH = the pass push header.
// Field order = the HLSL struct's member order.
#define BLOCK(HL, KIND, CPPT)       \
    namespace b_##HL {              \
    using C = CPPT;                 \
    constexpr Field kF[] = {
#define END(HL, KIND, CPPT) \
    }                       \
    ;                       \
    }
#define F(type, name, n) {#type, #name, n, static_cast<std::uint32_t>(offsetof(C, name))},
#define FM(type, name, n, member) {#type, #name, n, static_cast<std::uint32_t>(offsetof(C, member))},
#define FO(type, name, n, off) {#type, #name, n, static_cast<std::uint32_t>(off)},
#define PH                                                                                                                                    \
    {"frameref", "frame", 1, static_cast<std::uint32_t>(offsetof(C, h) + offsetof(abi::PassPushHeader, frame))},                            \
        {"uint", "passId", 1, static_cast<std::uint32_t>(offsetof(C, h) + offsetof(abi::PassPushHeader, passId))},                           \
        {"uint", "passFlags", 1, static_cast<std::uint32_t>(offsetof(C, h) + offsetof(abi::PassPushHeader, passFlags))},

// ---- push blocks ----
BLOCK(RvkDrawPush, Push, abi::PushConstants)
FM(uint2, vs, 1, vsConsts) FM(uint2, ps, 1, psConsts) F(uint2, draw, 1) F(uint2, frame, 1) FM(uint2, prevVs, 1, prevVsConsts) F(uint, drawId, 1) F(uint, flags, 1)
FO(uint2, vsw0, 1, offsetof(C, vsWin)) FO(uint2, vsw1, 1, offsetof(C, vsWin) + 8) FO(uint2, vsw2, 1, offsetof(C, vsWin) + 16) FO(uint2, vsw3, 1, offsetof(C, vsWin) + 24)
FO(uint2, psw0, 1, offsetof(C, psWin)) FO(uint2, psw1, 1, offsetof(C, psWin) + 8) FO(uint2, psw2, 1, offsetof(C, psWin) + 16) FO(uint2, psw3, 1, offsetof(C, psWin) + 24)
END(RvkDrawPush, Push, abi::PushConstants)

BLOCK(RvkScenePush, Push, abi::ScenePush)
FM(uint2, records, 1, records) F(uint2, pad0, 1) F(uint2, pad1, 1) F(uint2, frame, 1) F(uint, recordBase, 1) F(uint, pad2, 1) F(uint2, views, 1)
FO(uint2, spare1, 1, offsetof(C, spare)) FO(uint2, spare2, 1, offsetof(C, spare) + 8) FO(uint2, spare3, 1, offsetof(C, spare) + 16) FO(uint2, spare4, 1, offsetof(C, spare) + 24)
FO(uint2, spare5, 1, offsetof(C, spare) + 32) FO(uint2, spare6, 1, offsetof(C, spare) + 40) FO(uint2, spare7, 1, offsetof(C, spare) + 48) FO(uint2, spare8, 1, offsetof(C, spare) + 56)
END(RvkScenePush, Push, abi::ScenePush)

BLOCK(BlitPush, Push, abi::BlitPush)
F(uint, srcTex, 1) F(uint, smp, 1) F(uint, pad0, 1) F(uint, pad1, 1) F(float4, uvRect, 1)
END(BlitPush, Push, abi::BlitPush)

BLOCK(DebugViewPush, Push, enh::DebugViewPush)
PH F(uint, depthTex, 1) F(uint, width, 1) F(uint, height, 1) F(uint, srcTex, 1) F(uint, srcSampler, 1) F(uint, pad, 1)
END(DebugViewPush, Push, enh::DebugViewPush)

BLOCK(DepthZPush, Push, enh::DepthZPush)
PH F(uint, depthTex, 1) F(uint, mapTex, 1) F(uint, width, 1) F(uint, height, 1) F(uint2, stats, 1) F(uint2, pad, 1)
END(DepthZPush, Push, enh::DepthZPush)

BLOCK(PictureCheckPush, Push, enh::PictureCheckPush)
PH F(uint, colorTex, 1) F(uint, depthTex, 1) F(uint, width, 1) F(uint, height, 1) F(uint, mode, 1) F(float, colorTol, 1) F(float, depthTol, 1) F(uint, pad, 1)
F(uint2, stats, 1) F(uint2, pad2, 1)
END(PictureCheckPush, Push, enh::PictureCheckPush)

BLOCK(FramegenPush, Push, abi::FramegenDebugPush)
PH F(uint, srcTex, 1) F(uint, sampler0, 1) F(uint, mode, 1) F(uint, pad, 1) F(float, nearZ, 1) F(float, farZ, 1) F(float, pad1, 1) F(float, pad2, 1)
END(FramegenPush, Push, abi::FramegenDebugPush)

BLOCK(GbufferPush, Push, enh::GbufferPush)
PH F(uint, depthTex, 1) F(uint, normalTex, 1) F(uint, motionTex, 1) F(uint, aoTex, 1) F(uint, historyTex, 1) F(uint, linearSampler, 1) F(uint, width, 1) F(uint, height, 1)
F(float, radius, 1) F(float, falloff, 1) F(float, strength, 1) F(float, blend, 1) F(uint, frameIndex, 1) F(uint, flags, 1) F(uint, mode, 1) F(uint, pad, 1)
END(GbufferPush, Push, enh::GbufferPush)

BLOCK(GiPush, Push, enh::GiPush)
PH F(uint2, params, 1) F(uint2, pad, 1)
END(GiPush, Push, enh::GiPush)

BLOCK(LightingComposePush, Push, abi::LightingComposePush)
F(uint2, illums, 1) F(uint2, sets, 1) F(uint2, jobs, 1) F(uint2, images, 1) F(uint, count, 1)
FO(uint, pad0, 1, offsetof(C, pad)) FO(uint, pad1, 1, offsetof(C, pad) + 4) FO(uint, pad2, 1, offsetof(C, pad) + 8)
END(LightingComposePush, Push, abi::LightingComposePush)

BLOCK(OwnedCompactPush, Push, abi::OwnedCompactPush)
F(uint2, records, 1) F(uint2, list, 1) F(uint2, jobs, 1) FM(uint2, dst, 1, out) F(uint, countsBase, 1) F(uint, argsBase, 1) F(uint, slotCount, 1) F(uint, viewSlots, 1)
F(uint2, clusters, 1) F(uint2, views, 1) F(uint, clusterRows, 1)
FO(uint, pad0, 1, offsetof(C, pad)) FO(uint, pad1, 1, offsetof(C, pad) + 4) FO(uint, pad2, 1, offsetof(C, pad) + 8)
END(OwnedCompactPush, Push, abi::OwnedCompactPush)

BLOCK(LightClusterPush, Push, enh::LightClusterPush)
PH F(uint2, src, 1) F(uint2, dst, 1) F(uint4, grid, 1) F(float4, zs, 1) F(uint4, offs, 1) F(uint4, shadow, 1) F(uint4, grid1, 1) F(float4, zs1, 1)
END(LightClusterPush, Push, enh::LightClusterPush)

BLOCK(LightShadowPush, Push, enh::LightShadowPush)
PH F(uint4, tex, 1) F(float4, cfg, 1) F(uint4, misc, 1) F(uint2, table, 1)
END(LightShadowPush, Push, enh::LightShadowPush)

BLOCK(MaterialRtPush, Push, enh::MaterialRtPush)
PH F(uint2, params, 1) F(uint2, pad, 1)
END(MaterialRtPush, Push, enh::MaterialRtPush)

BLOCK(MaterialRtCompositePush, Push, enh::MaterialRtCompositePush)
PH F(uint, image, 1) F(uint, sampler, 1) F(uint, scale, 1) F(uint, depth, 1)
END(MaterialRtCompositePush, Push, enh::MaterialRtCompositePush)

BLOCK(PostLumPush, Push, native::PostLumPush)
PH F(uint2, params, 1) F(uint2, pad, 1)
END(PostLumPush, Push, native::PostLumPush)
BLOCK(PostBrightPush, Push, native::PostGroupPush)
PH F(uint2, params, 1) F(uint2, pad, 1)
END(PostBrightPush, Push, native::PostGroupPush)
BLOCK(PostGlowPush, Push, native::PostGroupPush)
PH F(uint2, params, 1) F(uint2, pad, 1)
END(PostGlowPush, Push, native::PostGroupPush)
BLOCK(PostBloomPush, Push, native::PostGroupPush)
PH F(uint2, params, 1) F(uint2, pad, 1)
END(PostBloomPush, Push, native::PostGroupPush)
BLOCK(PostDofMidPush, Push, native::PostGroupPush)
PH F(uint2, params, 1) F(uint2, pad, 1)
END(PostDofMidPush, Push, native::PostGroupPush)

BLOCK(RtDebugPush, Push, enh::RtDebugPush)
PH F(uint, depthTex, 1) F(uint, width, 1) F(uint, height, 1) F(uint, srcTex, 1) F(uint, srcSampler, 1) F(uint, mode, 1) F(uint2, probes, 1)
END(RtDebugPush, Push, enh::RtDebugPush)

BLOCK(SkinPush, Push, abi::SkinPush)
PH F(uint2, table, 1) F(uint2, pad, 1)
END(SkinPush, Push, abi::SkinPush)

BLOCK(RtShadowPush, Push, enh::RtShadowPush)
PH F(uint2, params, 1) F(uint2, pad, 1)
END(RtShadowPush, Push, enh::RtShadowPush)

BLOCK(UpscalePush, Push, abi::DepthPrepPush)
PH F(uint, depthTex, 1) F(uint, width, 1) F(uint, height, 1) F(uint, pad, 1)
END(UpscalePush, Push, abi::DepthPrepPush)

BLOCK(ExposurePush, Push, abi::ExposurePush)
PH F(uint, lumTex, 1) F(float, factor, 1) F(float, lumMin, 1) F(float, lumMax, 1)
END(ExposurePush, Push, abi::ExposurePush)

BLOCK(OceanPush, Push, enh::OceanPush)
PH F(float4, tile, 1) F(float4, wind, 1) F(float4, wind2, 1) F(float4, swell, 1) F(float4, time, 1) F(float4, foam, 1)
END(OceanPush, Push, enh::OceanPush)

BLOCK(FoamPush, Push, enh::OceanFoamPush)
PH F(float4, window, 1) F(float4, prev, 1) F(float4, floorMap, 1) F(uint4, tex, 1) F(uint4, info, 1) F(float4, shoreWave, 1)
END(FoamPush, Push, enh::OceanFoamPush)

BLOCK(MipPush, Push, enh::OceanMipPush)
PH F(uint4, info, 1)
END(MipPush, Push, enh::OceanMipPush)

BLOCK(WaterRtPush, Push, enh::WaterRtPush)
PH F(uint2, params, 1) F(uint2, pad, 1)
END(WaterRtPush, Push, enh::WaterRtPush)

BLOCK(WetPush, Push, enh::WetShorePush)
PH F(uint, depthTex, 1) F(uint, fieldTex, 1) F(uint, smpLinear, 1) F(uint, smpPoint, 1) F(float4, shoreMap, 1) F(float4, ocean, 1) F(float4, params, 1) F(uint4, sky, 1)
END(WetPush, Push, enh::WetShorePush)

// ---- blocks in buffers (RVK_REF / rvkPtr) ----
BLOCK(RvkFrameData, Ptr, abi::FrameBlock)
F(float4, viewProj, 4) F(float4, invViewProj, 4) F(float4, view, 4) F(float4, proj, 4) F(float4, invView, 4) F(float4, invProj, 4) F(float4, prevViewProj, 4)
F(float4, prevInvViewProj, 4) F(float4, cameraPos, 1) F(float4, prevCameraPos, 1) F(float4, depthParams, 1) F(float4, viewport, 1) F(float4, jitter, 1) F(float4, time, 1)
F(uint4, frameInfo, 1) F(uint4, images, 1) F(float4, params, 16) F(uint4, rtScene, 1) F(uint4, rtInfo, 1) F(uint4, gi, 1) F(uint4, ao, 1) F(uint4, sgi, 1)
END(RvkFrameData, Ptr, abi::FrameBlock)

BLOCK(RvkStageData, Ptr, StageConstsLayout)
F(float4, c, abi::kFloatConsts) F(int4, i, abi::kIntConsts) F(uint4, b, 4)
END(RvkStageData, Ptr, StageConstsLayout)

BLOCK(RvkDrawData, Ptr, abi::DrawBlock)
F(uint4, tex, 5) F(uint4, smp, 5) F(float4, posFix, 1) FM(uint4, alpha, 1, alphaFunc) F(float4, psC5, 1)
END(RvkDrawData, Ptr, abi::DrawBlock)

BLOCK(RvkDrawRecord, Ptr, abi::DrawRecord)
FM(uint4, vsw01, 1, vsWin) FO(uint4, vsw23, 1, offsetof(C, vsWin) + 16) FM(uint4, psw01, 1, psWin) FO(uint4, psw23, 1, offsetof(C, psWin) + 16)
FM(uint4, drawPrev, 1, draw) FM(uint4, ids, 1, drawId) FM(uint4, slots, 1, materialSlot) FM(uint4, reserved, 1, args)
END(RvkDrawRecord, Ptr, abi::DrawRecord)

BLOCK(RvkVertexFetch, Raw, abi::VertexFetch)
FM(uint4, ref01, 1, ref) FO(uint4, ref23, 1, offsetof(C, ref) + 16) F(uint4, stride, 1) F(int, baseVertex, 1) F(uint, valid, 1) F(uint2, pad, 1) F(uint4, attr, 4)
END(RvkVertexFetch, Raw, abi::VertexFetch)

BLOCK(RvkViewBlock2, Ptr, abi::ViewBlock2)
F(float4, vs0, 12) F(float4, ps0, 5) F(float4, vs1, 19) F(float4, posFix, 1) F(float4, c5, 1) F(uint4, flags, 1) F(uint4, reserved, 1)
END(RvkViewBlock2, Ptr, abi::ViewBlock2)

BLOCK(RvkRtRecordData, Ptr, abi::RtGeometryRecord)
FM(uint4, w0, 1, vertices) FM(uint4, w1, 1, stride) FM(uint4, w2, 1, texcoord1) FM(uint4, w3, 1, albedoTex) FM(uint4, uvMap0, 1, uvMap) FO(uint4, uvMap1, 1, offsetof(C, uvMap) + 16)
FM(uint4, w6, 1, segments)
END(RvkRtRecordData, Ptr, abi::RtGeometryRecord)

BLOCK(RvkRtSegmentData, Ptr, abi::RtMaterialSegment)
FM(uint4, w0, 1, firstPrimitive) FM(uint4, w1, 1, alpha) FM(uint4, uvMap0, 1, uvMap) FO(uint4, uvMap1, 1, offsetof(C, uvMap) + 16)
END(RvkRtSegmentData, Ptr, abi::RtMaterialSegment)

BLOCK(RvkGiVolumeData, Ptr, enh::GiVolumeHeader)
F(uint4, dims, 1) F(uint4, data, 1) F(float4, params, 1) F(uint4, info, 1)
FO(int4, cascade0_origin, 1, offsetof(C, cascade) + 0) FO(float4, cascade0_spacing, 1, offsetof(C, cascade) + 16)
FO(int4, cascade1_origin, 1, offsetof(C, cascade) + 32) FO(float4, cascade1_spacing, 1, offsetof(C, cascade) + 48)
FO(int4, cascade2_origin, 1, offsetof(C, cascade) + 64) FO(float4, cascade2_spacing, 1, offsetof(C, cascade) + 80)
FO(int4, cascade3_origin, 1, offsetof(C, cascade) + 96) FO(float4, cascade3_spacing, 1, offsetof(C, cascade) + 112)
F(float4, sky, 7)
END(RvkGiVolumeData, Ptr, enh::GiVolumeHeader)

BLOCK(GiPassParamsData, Ptr, enh::GiPassParams)
F(uint4, lists, 1) F(uint4, volume, 1) F(uint4, flags, 1) F(float4, rot, 3) F(float4, reach, 1) F(float4, blend, 1) F(float4, shade, 1) F(float4, sunColor, 1)
F(float4, sunDir, 1) F(float4, sky, 7) F(float4, lights, enh::kGiMaxLights * 2) F(uint4, screen, 1) F(uint4, screen2, 1) F(uint4, screen3, 1) F(float4, screen4, 1)
F(uint4, screen5, 1) F(float4, screen6, 1)
END(GiPassParamsData, Ptr, enh::GiPassParams)

BLOCK(GiRefParamsData, Ptr, enh::GiRefParams)
F(uint4, buf, 1) F(uint4, img, 1) F(uint4, size, 1)
END(GiRefParamsData, Ptr, enh::GiRefParams)

BLOCK(MaterialRtParams, Ptr, enh::MaterialRtParams)
F(uint4, tex, 1) F(uint4, tex2, 1) F(uint4, info, 1) F(uint4, trace, 1) F(float4, reach, 1) F(float4, strength, 1) F(float4, fogColor, 1) F(float4, fogParams, 1)
F(float4, indoor, 1) F(float4, sh, 7) F(float4, sunColor, 1) F(float4, sunDir, 1)
END(MaterialRtParams, Ptr, enh::MaterialRtParams)

BLOCK(PostLumParams, Ptr, native::PostLumParams)
F(uint4, tex, 1)
FO(float4, stage0_uv, 1, offsetof(C, stage) + 0) FO(float4, stage0_tap, 1, offsetof(C, stage) + 16) FO(float4, stage1_uv, 1, offsetof(C, stage) + 32)
FO(float4, stage1_tap, 1, offsetof(C, stage) + 48) FO(float4, stage2_uv, 1, offsetof(C, stage) + 64) FO(float4, stage2_tap, 1, offsetof(C, stage) + 80)
FO(float4, stage3_uv, 1, offsetof(C, stage) + 96) FO(float4, stage3_tap, 1, offsetof(C, stage) + 112) FO(float4, stage4_uv, 1, offsetof(C, stage) + 128)
FO(float4, stage4_tap, 1, offsetof(C, stage) + 144) FO(float4, stage5_uv, 1, offsetof(C, stage) + 160) FO(float4, stage5_tap, 1, offsetof(C, stage) + 176)
FO(float4, stage6_uv, 1, offsetof(C, stage) + 192) FO(float4, stage6_tap, 1, offsetof(C, stage) + 208)
F(float4, adapt, 1)
END(PostLumParams, Ptr, native::PostLumParams)

BLOCK(PostBrightParams, Ptr, native::PostBrightParams)
F(uint4, tex, 1) F(float4, uv, 1) F(float4, tap, 1) F(float4, ps, 1) F(uint4, size, 1)
END(PostBrightParams, Ptr, native::PostBrightParams)

BLOCK(PostGlowParams, Ptr, native::PostGlowParams)
F(uint4, tex, 1) F(float4, uvH, 1) F(float4, centerH, 1) F(float4, offH, 6) F(float4, uvS, 1) F(float4, centerS, 1) F(float4, light, 1) F(float4, sw, 1)
END(PostGlowParams, Ptr, native::PostGlowParams)

BLOCK(PostBloomParams, Ptr, native::PostBloomParams)
F(uint4, tex, 1) F(uint4, size, 1) F(float4, uv, 1) F(float4, center, 1) F(float4, off, 6) F(float4, blend, 1)
END(PostBloomParams, Ptr, native::PostBloomParams)

BLOCK(PostDofMidParams, Ptr, native::PostDofMidParams)
F(uint4, tex, 1) F(uint4, size, 1)
END(PostDofMidParams, Ptr, native::PostDofMidParams)

BLOCK(WaterRtParams, Ptr, enh::WaterRtParams)
F(uint4, tex, 1) F(uint4, info, 1) F(uint4, trace, 1) F(float4, reach, 1) F(float4, fogColor, 1) F(float4, fogParams, 1) F(float4, indoor, 1) F(float4, sh, 7)
F(float4, sunColor, 1) F(float4, sunDir, 1) F(float4, plane, enh::kWaterRtMaxBodies) F(float4, box, enh::kWaterRtMaxBodies)
END(WaterRtParams, Ptr, enh::WaterRtParams)

BLOCK(WaterBlockData, Ptr, enh::WaterBlock)
F(uint4, tex, 5) F(uint4, smp, 5) F(float4, posFix, 1) FM(uint4, alphaTint, 1, alphaFunc) F(float4, camFar, 1) F(float4, fogColor, 1) F(float4, fogParams, 1) F(float4, mat, 8)
F(float4, indoor, 1) F(float4, sh, 7) F(float4, sunColor, 1) F(float4, sunSpec, 1) F(float4, sunDir, 1) F(float4, vsTc, 6) F(float4, uvMapU, 1) F(float4, uvMapV, 1)
F(float4, meshFrame, 1) F(float4, meshExtra, 1) F(float4, ocean, 1) F(float4, grid, 1) F(float4, shoreMap, 1) F(float4, look, 1) F(float4, misc, 1) F(uint4, res, 1)
F(uint4, res2, 1) F(uint4, smpOurs, 1) F(float4, falls, 4) F(float4, lights, 12) F(uint4, lightInfo, 1) F(float4, fftLength, 1) F(uint4, fftDisp, 1) F(uint4, fftDeriv, 1)
F(float4, fftSlopeVar, 1) F(float4, fftBand, 1) F(uint4, fftInfo, 1) F(float4, seaInfo, 1) F(float4, floorMap, 1) F(uint4, floorTex, 1) F(float4, shoreWave, 1)
F(float4, player, 1) F(float4, foamMap, 1) F(uint4, foamTex, 1) F(uint4, rtRefl, 1)
END(WaterBlockData, Ptr, enh::WaterBlock)

BLOCK(ShadowBlock, Ptr, enh::ShadowBlock)
F(float4, cascade, 12) F(float4, split, 1) F(float4, texel, 3) F(uint4, maps, 1) F(uint4, res, 1) F(float4, vsm, 3) F(float4, vsmParams, 1) F(float4, params, 1)
F(float4, target, 1) F(uint4, mode, 1) F(uint4, rt, 1) F(float4, rtParams, 1) F(float4, depthRemap, 1)
END(ShadowBlock, Ptr, enh::ShadowBlock)

BLOCK(RtShadowParams, Ptr, enh::RtShadowParams)
F(uint4, tex, 1) F(uint4, info, 1) F(uint4, mode, 1) F(float4, sun, 1) F(float4, range, 1)
END(RtShadowParams, Ptr, enh::RtShadowParams)

BLOCK(RvkLight, Ptr, enh::GpuLight)
F(float4, posRcp, 1) F(float4, colRange, 1) F(float4, specFlags, 1) F(float4, misc, 1)
END(RvkLight, Ptr, enh::GpuLight)

// ---- raw records (rvkRawLoad at constant offsets) ----
BLOCK(RvkInstance, Raw, gpuscene::InstanceRecord)
F(float4, world, 4) F(float4, prev, 4) FM(uint4, info, 1, generation) FM(uint4, moved, 1, movedFrame)
END(RvkInstance, Raw, gpuscene::InstanceRecord)

BLOCK(RvkMeshPart, Raw, gpuscene::PartRecord)
F(uint4, info, 1) F(uint4, index, 1) F(uint4, range, 1) FO(uint4, s0, 1, offsetof(C, stream)) FO(uint4, s1, 1, offsetof(C, stream) + 16) FO(uint4, s2, 1, offsetof(C, stream) + 32)
F(uint4, material, 1) F(uint4, owner, 1)
END(RvkMeshPart, Raw, gpuscene::PartRecord)

BLOCK(RvkClusterRow, Raw, gpuscene::ClusterRow)
FM(float4, sphere, 1, c) FM(uint4, info, 1, firstIndex)
END(RvkClusterRow, Raw, gpuscene::ClusterRow)

#undef BLOCK
#undef END
#undef F
#undef FM
#undef FO
#undef PH

std::span<const Block> blocks() {
#define REG(HL, KIND, CPPT) Block{#HL, #CPPT, Kind::KIND, static_cast<std::uint32_t>(sizeof(CPPT)), b_##HL::kF, std::size(b_##HL::kF)},
    static const Block kBlocks[] = {
        REG(RvkDrawPush, Push, abi::PushConstants) REG(RvkScenePush, Push, abi::ScenePush) REG(BlitPush, Push, abi::BlitPush)
        REG(DebugViewPush, Push, enh::DebugViewPush) REG(PictureCheckPush, Push, enh::PictureCheckPush) REG(DepthZPush, Push, enh::DepthZPush) REG(FramegenPush, Push, abi::FramegenDebugPush) REG(GbufferPush, Push, enh::GbufferPush)
        REG(GiPush, Push, enh::GiPush) REG(LightingComposePush, Push, abi::LightingComposePush) REG(OwnedCompactPush, Push, abi::OwnedCompactPush)
        REG(LightClusterPush, Push, enh::LightClusterPush) REG(LightShadowPush, Push, enh::LightShadowPush) REG(MaterialRtPush, Push, enh::MaterialRtPush)
        REG(MaterialRtCompositePush, Push, enh::MaterialRtCompositePush) REG(PostLumPush, Push, native::PostLumPush) REG(PostBrightPush, Push, native::PostGroupPush)
        REG(PostGlowPush, Push, native::PostGroupPush) REG(PostBloomPush, Push, native::PostGroupPush) REG(PostDofMidPush, Push, native::PostGroupPush)
        REG(RtDebugPush, Push, enh::RtDebugPush) REG(SkinPush, Push, abi::SkinPush) REG(RtShadowPush, Push, enh::RtShadowPush) REG(UpscalePush, Push, abi::DepthPrepPush)
        REG(ExposurePush, Push, abi::ExposurePush) REG(OceanPush, Push, enh::OceanPush) REG(FoamPush, Push, enh::OceanFoamPush) REG(MipPush, Push, enh::OceanMipPush)
        REG(WaterRtPush, Push, enh::WaterRtPush) REG(WetPush, Push, enh::WetShorePush)
        REG(RvkFrameData, Ptr, abi::FrameBlock) REG(RvkStageData, Ptr, StageConstsLayout) REG(RvkDrawData, Ptr, abi::DrawBlock) REG(RvkDrawRecord, Ptr, abi::DrawRecord)
        REG(RvkViewBlock2, Ptr, abi::ViewBlock2) REG(RvkRtRecordData, Ptr, abi::RtGeometryRecord) REG(RvkRtSegmentData, Ptr, abi::RtMaterialSegment)
        REG(RvkGiVolumeData, Ptr, enh::GiVolumeHeader) REG(GiPassParamsData, Ptr, enh::GiPassParams) REG(GiRefParamsData, Ptr, enh::GiRefParams)
        REG(MaterialRtParams, Ptr, enh::MaterialRtParams) REG(PostLumParams, Ptr, native::PostLumParams) REG(PostBrightParams, Ptr, native::PostBrightParams)
        REG(PostGlowParams, Ptr, native::PostGlowParams) REG(PostBloomParams, Ptr, native::PostBloomParams) REG(PostDofMidParams, Ptr, native::PostDofMidParams)
        REG(WaterRtParams, Ptr, enh::WaterRtParams) REG(WaterBlockData, Ptr, enh::WaterBlock) REG(ShadowBlock, Ptr, enh::ShadowBlock)
        REG(RtShadowParams, Ptr, enh::RtShadowParams) REG(RvkLight, Ptr, enh::GpuLight)
        REG(RvkInstance, Raw, gpuscene::InstanceRecord) REG(RvkMeshPart, Raw, gpuscene::PartRecord) REG(RvkClusterRow, Raw, gpuscene::ClusterRow) REG(RvkVertexFetch, Raw, abi::VertexFetch)
    };
#undef REG
    return kBlocks;
}

const Block* findBlock(std::string_view name) {
    for (const Block& b : blocks()) {
        if (name == b.name) return &b;
    }
    return nullptr;
}

// ---- types and layout --------------------------------------------------------------------------------------------------------------------
TypeInfo parseType(std::string_view t) {
    TypeInfo r;
    if (t == "frameref") {
        r.ok = r.frameRef = true;
        r.comps = 2;
        return r;
    }
    std::string_view base;
    if (t.starts_with("uint")) {
        base = "uint";
        r.base = 'u';
    } else if (t.starts_with("int")) {
        base = "int";
        r.base = 'i';
    } else if (t.starts_with("float")) {
        base = "float";
        r.base = 'f';
    } else {
        return r;
    }
    const std::string_view rest = t.substr(base.size());
    if (rest.empty()) r.comps = 1;
    else if (rest.size() == 1 && rest[0] >= '2' && rest[0] <= '4') r.comps = static_cast<std::uint32_t>(rest[0] - '0');
    else return r;
    r.ok = true;
    return r;
}

namespace {
std::uint32_t roundUp(std::uint32_t v, std::uint32_t a) { return (v + a - 1) / a * a; }
}  // namespace

std::vector<std::uint32_t> wordOffsets(const std::vector<Decl>& ds, Rule rule, std::uint32_t* size) {
    std::vector<std::uint32_t> out;
    std::uint32_t off = 0;
    for (const Decl& d : ds) {
        const TypeInfo t = parseType(d.type);
        if (!t.ok) return {};
        const std::uint32_t bytes = t.comps * 4;
        const bool array = d.count > 1;
        std::uint32_t align = 4, stride = bytes;
        switch (rule) {
        case Rule::VulkanStd430:
            align = t.frameRef ? 8 : t.comps == 1 ? 4 : t.comps == 2 ? 8 : 16;
            stride = array ? roundUp(bytes, align) : bytes;
            break;
        case Rule::D3D12Cbuffer:
            if (array || t.frameRef) {
                align = 16;
                stride = roundUp(bytes, 16);
            }
            break;
        case Rule::Scalar: break;
        }
        off = roundUp(off, align);
        if (rule == Rule::D3D12Cbuffer && !array && !t.frameRef && (off % 16) + bytes > 16) off = roundUp(off, 16);
        for (std::uint32_t e = 0; e < d.count; ++e) {
            for (std::uint32_t k = 0; k < t.comps; ++k) out.push_back(off + e * stride + 4 * k);
        }
        off += rule == Rule::VulkanStd430 && array ? d.count * stride : (d.count - 1) * stride + bytes;
    }
    if (size != nullptr) *size = off;
    return out;
}

std::vector<Decl> decls(const Block& b) {
    std::vector<Decl> r;
    for (const Field& f : b.list()) r.push_back({f.type, f.count});
    return r;
}

std::vector<std::uint32_t> cppWordOffsets(const Block& b) {
    std::vector<std::uint32_t> out;
    for (const Field& f : b.list()) {
        const TypeInfo t = parseType(f.type);
        if (!t.ok) return {};
        for (std::uint32_t e = 0; e < f.count; ++e) {
            for (std::uint32_t k = 0; k < t.comps; ++k) out.push_back(f.cppOffset + e * t.comps * 4 + 4 * k);
        }
    }
    return out;
}

Rules rulesOf(Kind k) {
    switch (k) {
    case Kind::Push: return {Rule::VulkanStd430, Rule::D3D12Cbuffer};
    case Kind::Ptr: return {Rule::VulkanStd430, Rule::Scalar};
    case Kind::Raw: return {Rule::Scalar, Rule::Scalar};
    }
    return {Rule::Scalar, Rule::Scalar};
}

std::uint32_t probeWordCount(const Block& b) {
    std::uint32_t n = 0;
    for (const Field& f : b.list()) {
        const TypeInfo t = parseType(f.type);
        n += t.comps * f.count;
    }
    return n;
}

// ---- the probe shader ----------------------------------------------------------------------------------------------------------------
std::string probeHlsl(const Block& b) {
    std::string s = "// generated by abi_blocks.cpp probeHlsl: the ABI probe of ";
    s += b.name;
    s += "\n#include \"rvk_common.hlsli\"\n#include \"abi_probe.hlsli\"\n";
    static const char* kComp[4] = {"x", "y", "z", "w"};
    const auto reads = [&](std::string& out, const std::string& prefix) {
        for (const Field& f : b.list()) {
            const TypeInfo t = parseType(f.type);
            if (t.frameRef) {
                // the frame pointer: load the block it points at and put two of its words (the probe's frame block is the pattern buffer)
                out += std::format("    {{ RvkFrameData fd_ = RVK_LOAD(RvkFrameData, pc.{}); PUT(asuint(fd_.time.x)); PUT(asuint(fd_.time.y)); }}\n", f.name);
                continue;
            }
            for (std::uint32_t e = 0; e < f.count; ++e) {
                const std::string elem = f.count > 1 ? std::format("{}{}[{}]", prefix, f.name, e) : prefix + f.name;
                for (std::uint32_t k = 0; k < t.comps; ++k) {
                    out += std::format("    PUT(asuint({}{}));\n", elem, t.comps == 1 ? "" : std::string(".") + kComp[k]);
                }
            }
        }
    };
    const auto structBody = [&](std::string& out) {
        out += "struct ProbeBlock {\n";
        for (const Field& f : b.list()) {
            const TypeInfo t = parseType(f.type);
            if (t.frameRef) out += std::format("    RVK_REF(RvkFrameData) {};\n", f.name);
            else if (f.count > 1) out += std::format("    {} {}[{}];\n", f.type, f.name, f.count);
            else out += std::format("    {} {};\n", f.type, f.name);
        }
        out += "};\n";
    };
    switch (b.kind) {
    case Kind::Push:
        structBody(s);
        s += "RVK_PUSH(ProbeBlock, pc);\n[numthreads(1, 1, 1)]\nvoid CSMain() {\n    uint n = 0;\n";
        reads(s, "pc.");
        s += "}\n";
        break;
    case Kind::Ptr:
        structBody(s);
        s += "struct ProbePtrPush { uint2 ptr; };\nRVK_PUSH(ProbePtrPush, pc);\n[numthreads(1, 1, 1)]\nvoid CSMain() {\n    uint n = 0;\n";
        s += "    ProbeBlock blk = rvkPtr<ProbeBlock, 16>(pc.ptr).Get();\n";
        reads(s, "blk.");
        s += "}\n";
        break;
    case Kind::Raw:
        s += "struct ProbePtrPush { uint2 ptr; };\nRVK_PUSH(ProbePtrPush, pc);\n[numthreads(1, 1, 1)]\nvoid CSMain() {\n    uint n = 0;\n";
        for (const Field& f : b.list()) {
            const TypeInfo t = parseType(f.type);
            const std::uint32_t align = t.comps == 1 ? 4 : t.comps == 2 ? 8 : 16;
            for (std::uint32_t e = 0; e < f.count; ++e) {
                s += std::format("    {{ {} v_ = rvkRawLoad<{}, {}>(rvkAddrAdd(rvkAddr(pc.ptr), {}u));\n", f.type, f.type, align, f.cppOffset + e * t.comps * 4);
                for (std::uint32_t k = 0; k < t.comps; ++k) s += std::format("      PUT(asuint(v_{}));\n", t.comps == 1 ? "" : std::string(".") + kComp[k]);
                s += "    }\n";
            }
        }
        s += "}\n";
        break;
    }
    return s;
}

// ---- the lint ----------------------------------------------------------------------------------------------------------------------------
namespace {

std::string stripComments(std::string_view t) {
    std::string r;
    r.reserve(t.size());
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '/' && i + 1 < t.size() && t[i + 1] == '/') {
            while (i < t.size() && t[i] != '\n') ++i;
            if (i < t.size()) r.push_back('\n');
        } else if (t[i] == '/' && i + 1 < t.size() && t[i + 1] == '*') {
            i += 2;
            while (i + 1 < t.size() && !(t[i] == '*' && t[i + 1] == '/')) {
                if (t[i] == '\n') r.push_back('\n');
                ++i;
            }
            ++i;
        } else {
            r.push_back(t[i]);
        }
    }
    return r;
}

std::string trim(std::string_view s) {
    std::size_t a = 0, e = s.size();
    while (a < e && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (e > a && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(a, e - a));
}

bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// a tiny integer expression evaluator: numbers (u suffix), names (constants), + - *
struct Constants {
    std::map<std::string, std::string> text;
    bool eval(std::string_view expr, std::int64_t& out, int depth = 0) const {
        if (depth > 8) return false;
        std::vector<std::int64_t> vals;
        std::vector<char> ops;
        std::size_t i = 0;
        const auto term = [&](std::int64_t& v) {
            while (i < expr.size() && std::isspace(static_cast<unsigned char>(expr[i]))) ++i;
            if (i >= expr.size()) return false;
            if (expr[i] == '(') {
                int d = 1;
                std::size_t j = i + 1;
                while (j < expr.size() && d > 0) {
                    d += expr[j] == '(' ? 1 : expr[j] == ')' ? -1 : 0;
                    ++j;
                }
                const bool ok = eval(expr.substr(i + 1, j - i - 2), v, depth + 1);
                i = j;
                return ok;
            }
            std::size_t j = i;
            while (j < expr.size() && isIdent(expr[j])) ++j;
            const std::string tok(expr.substr(i, j - i));
            i = j;
            if (tok.empty()) return false;
            if (std::isdigit(static_cast<unsigned char>(tok[0]))) {
                v = std::stoll(tok, nullptr, 0);
                return true;
            }
            const auto it = text.find(tok);
            return it != text.end() && eval(it->second, v, depth + 1);
        };
        std::int64_t v = 0;
        if (!term(v)) return false;
        vals.push_back(v);
        while (true) {
            while (i < expr.size() && std::isspace(static_cast<unsigned char>(expr[i]))) ++i;
            if (i >= expr.size()) break;
            const char op = expr[i++];
            if (op != '+' && op != '-' && op != '*') return false;
            if (!term(v)) return false;
            if (op == '*') vals.back() *= v;
            else {
                ops.push_back(op);
                vals.push_back(v);
            }
        }
        std::int64_t r = vals[0];
        for (std::size_t k = 0; k < ops.size(); ++k) r = ops[k] == '+' ? r + vals[k + 1] : r - vals[k + 1];
        out = r;
        return true;
    }
};

struct StructDef {
    std::string file;
    std::string body;
};

void collect(const std::string& text, const std::string& file, Constants& consts, std::multimap<std::string, StructDef>& structs) {
    // constants: `static const uint NAME = expr;` and `#define NAME expr`
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = trim(std::string_view(text).substr(pos, eol - pos));
        if (line.starts_with("static const uint ")) {
            const std::size_t eq = line.find('=');
            const std::size_t semi = line.find(';');
            if (eq != std::string::npos && semi != std::string::npos && semi > eq) consts.text[trim(line.substr(18, eq - 18))] = trim(line.substr(eq + 1, semi - eq - 1));
        } else if (line.starts_with("#define ")) {
            const std::string rest = trim(line.substr(8));
            std::size_t sp = 0;
            while (sp < rest.size() && isIdent(rest[sp])) ++sp;
            if (sp > 0 && sp < rest.size() && rest[sp] == ' ') consts.text[rest.substr(0, sp)] = trim(rest.substr(sp));
        }
        pos = eol + 1;
    }
    // structs
    pos = 0;
    while ((pos = text.find("struct ", pos)) != std::string::npos) {
        if (pos > 0 && isIdent(text[pos - 1])) {
            pos += 7;
            continue;
        }
        std::size_t i = pos + 7;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        std::size_t j = i;
        while (j < text.size() && isIdent(text[j])) ++j;
        const std::string name = text.substr(i, j - i);
        while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j]))) ++j;
        if (name.empty() || j >= text.size() || text[j] != '{') {
            pos = j;
            continue;
        }
        int d = 1;
        std::size_t k = j + 1;
        while (k < text.size() && d > 0) {
            d += text[k] == '{' ? 1 : text[k] == '}' ? -1 : 0;
            ++k;
        }
        structs.emplace(name, StructDef{file, text.substr(j + 1, k - j - 2)});
        pos = k;
    }
}

// the member declarations of a struct body as Decls (nested structs inlined); errors appended
bool parseBody(const std::string& body, const Constants& consts, const std::multimap<std::string, StructDef>& structs, std::vector<Decl>& out, std::string& err, int depth = 0) {
    if (depth > 4) {
        err = "nesting too deep";
        return false;
    }
    std::size_t pos = 0;
    while (pos < body.size()) {
        std::size_t semi = body.find(';', pos);
        if (semi == std::string::npos) semi = body.size();
        std::string stmt = trim(std::string_view(body).substr(pos, semi - pos));
        pos = semi + 1;
        if (stmt.empty()) continue;
        // the pass header macro stands for the pointer + two words
        const std::string kHeader = "RVK_PASS_PUSH_HEADER";
        if (stmt.starts_with(kHeader)) {
            out.push_back({"frameref", 1});
            out.push_back({"uint", 1});
            out.push_back({"uint", 1});
            stmt = trim(stmt.substr(kHeader.size()));
            if (stmt.empty()) continue;
        }
        std::string type;
        std::size_t i = 0;
        if (stmt.starts_with("RVK_REF(")) {
            const std::size_t close = stmt.find(')');
            if (close == std::string::npos) {
                err = "bad RVK_REF member: " + stmt;
                return false;
            }
            const std::string arg = trim(stmt.substr(8, close - 8));
            if (arg != "RvkFrameData") {
                err = "an RVK_REF(" + arg + ") member is not supported by the lint (push blocks hold the frame pointer only)";
                return false;
            }
            type = "frameref";
            i = close + 1;
        } else {
            while (i < stmt.size() && isIdent(stmt[i])) ++i;
            type = stmt.substr(0, i);
        }
        std::string rest = trim(stmt.substr(i));
        // declarators: name[expr], ...
        std::size_t dp = 0;
        while (dp < rest.size()) {
            std::size_t comma = std::string::npos;
            int br = 0;
            for (std::size_t q = dp; q < rest.size(); ++q) {
                if (rest[q] == '[') ++br;
                else if (rest[q] == ']') --br;
                else if (rest[q] == ',' && br == 0) {
                    comma = q;
                    break;
                }
            }
            const std::string decl = trim(std::string_view(rest).substr(dp, comma == std::string::npos ? std::string::npos : comma - dp));
            dp = comma == std::string::npos ? rest.size() : comma + 1;
            if (decl.empty()) continue;
            std::uint32_t count = 1;
            const std::size_t lb = decl.find('[');
            if (lb != std::string::npos) {
                const std::size_t rb = decl.find(']', lb);
                std::int64_t v = 0;
                if (rb == std::string::npos || !consts.eval(decl.substr(lb + 1, rb - lb - 1), v) || v < 1) {
                    err = "array size not evaluable: " + decl;
                    return false;
                }
                count = static_cast<std::uint32_t>(v);
            }
            if (parseType(type).ok) {
                out.push_back({type, count});
            } else {
                const auto it = structs.find(type);
                if (it == structs.end()) {
                    err = "unknown member type '" + type + "'";
                    return false;
                }
                std::vector<Decl> inner;
                if (!parseBody(it->second.body, consts, structs, inner, err, depth + 1)) return false;
                for (std::uint32_t e = 0; e < count; ++e) out.insert(out.end(), inner.begin(), inner.end());
            }
        }
    }
    return true;
}

// HLSL structs that are reached through a pointer but have no C++ struct to describe (written by the GPU only) or are unrelated to the
// probe's purpose; every entry needs its reason
const std::map<std::string, const char*>& exemptions() {
    static const std::map<std::string, const char*> m = {
        {"RvkGiProbe", "written and read by GPU passes only (gi_blend.comp / lookups): no C++ struct, int4 + float4 members (layout-safe), 128 byte stride"},
        {"RvkLightHeader", "written by the light cluster compute pass and read by shaders: no C++ struct, uint4 / float4 members only"},
    };
    return m;
}

}  // namespace

LintResult lintSources(std::span<const Source> sources) {
    LintResult res;
    Constants consts;
    std::multimap<std::string, StructDef> structs;
    std::vector<std::pair<std::string, std::string>> texts;  // (file, comment-free text)
    for (const Source& s : sources) {
        if (s.name.starts_with("test/")) continue;
        texts.emplace_back(std::string(s.name), stripComments(s.text));
        collect(texts.back().second, texts.back().first, consts, structs);
    }
    // 1. every struct used as a push block / RVK_REF / rvkPtr has a description of the right kind
    struct Use {
        std::string file;
        Kind kind;
    };
    std::map<std::string, Use> uses;
    const auto scan = [&](const std::string& file, const std::string& text, std::string_view key, char terminator, Kind kind) {
        std::size_t pos = 0;
        while ((pos = text.find(key, pos)) != std::string::npos) {
            const std::size_t start = pos;
            std::size_t i = pos + key.size();
            pos = i;
            if (start > 0 && isIdent(text[start - 1])) continue;  // part of a longer name
            while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            std::size_t j = i;
            while (j < text.size() && isIdent(text[j])) ++j;
            const std::string name = text.substr(i, j - i);
            while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j]))) ++j;
            if (name.empty() || j >= text.size() || text[j] != terminator) continue;
            if (name == "T") continue;  // the macro / template definitions
            uses.emplace(name, Use{file, kind});
        }
    };
    for (const auto& [file, text] : texts) {
        scan(file, text, "RVK_PUSH(", ',', Kind::Push);
        scan(file, text, "RVK_REF(", ')', Kind::Ptr);
        scan(file, text, "rvkPtr<", ',', Kind::Ptr);
        scan(file, text, "ConstantBuffer<", '>', Kind::Push);
    }
    for (const auto& [name, use] : uses) {
        ++res.usesFound;
        if (exemptions().contains(name)) continue;
        const Block* b = findBlock(name);
        if (b == nullptr) {
            res.errors.push_back(std::format("{}: struct {} is used as a {} but has no ABI probe description (add it to src/shader/abi_blocks.cpp)", use.file, name,
                                             use.kind == Kind::Push ? "push block / constant buffer" : "RVK_REF / rvkPtr block"));
        } else if ((use.kind == Kind::Push) != (b->kind == Kind::Push)) {
            res.errors.push_back(std::format("{}: struct {} is used as a {} but described as {}", use.file, name, use.kind == Kind::Push ? "push block" : "pointer block",
                                             b->kind == Kind::Push ? "a push block" : "a pointer block"));
        }
    }
    // 2. every definition of a described struct has the description's layout on both backends
    for (const Block& b : blocks()) {
        if (b.kind == Kind::Raw) continue;
        const auto range = structs.equal_range(b.name);
        if (range.first == range.second) {
            res.errors.push_back(std::format("block {} has a description but no HLSL struct of that name exists", b.name));
            continue;
        }
        const Rules rules = rulesOf(b.kind);
        const std::vector<Decl> want = decls(b);
        std::uint32_t wantSizeV = 0, wantSizeD = 0;
        const auto wantV = wordOffsets(want, rules.vulkan, &wantSizeV);
        const auto wantD = wordOffsets(want, rules.d3d12, &wantSizeD);
        for (auto it = range.first; it != range.second; ++it) {
            ++res.structsChecked;
            std::vector<Decl> got;
            std::string err;
            if (!parseBody(it->second.body, consts, structs, got, err)) {
                res.errors.push_back(std::format("{}: struct {}: {}", it->second.file, b.name, err));
                continue;
            }
            const auto gotV = wordOffsets(got, rules.vulkan);
            const auto gotD = wordOffsets(got, rules.d3d12);
            for (int backend = 0; backend < 2; ++backend) {
                const auto& g = backend == 0 ? gotV : gotD;
                const auto& w = backend == 0 ? wantV : wantD;
                const char* bn = backend == 0 ? "Vulkan" : "D3D12";
                if (g.size() != w.size()) {
                    res.errors.push_back(std::format("{}: struct {}: {} words, the description has {} ({} layout)", it->second.file, b.name, g.size(), w.size(), bn));
                    break;
                }
                for (std::size_t k = 0; k < g.size(); ++k) {
                    if (g[k] != w[k]) {
                        res.errors.push_back(std::format("{}: struct {}: word {} sits at byte {} in the HLSL struct, {} in the description ({} layout)", it->second.file,
                                                         b.name, k, g[k], w[k], bn));
                        break;
                    }
                }
            }
        }
    }
    return res;
}

}  // namespace rvk::shader::abiblocks
