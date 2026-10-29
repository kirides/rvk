// Probe of the runtime HLSL pipeline (shader/dxc, docs/research/hlsl-rhi.md): compiled by rvk_tests (dxc_probe_compiles) and
// rvk_vktest; the include proves the embedded include resolution.
#include "include/rvk_probe.hlsli"

[[vk::image_format("r32f")]] RWTexture2D<float> g_out : register(u0, space0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    g_out[id.xy] = rvkProbeValue(id.x, id.y);
}
