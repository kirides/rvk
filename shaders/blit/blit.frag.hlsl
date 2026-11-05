#include "rvk_common.hlsli"

struct BlitPush {
    uint srcTex;
    uint smp;
    uint pad0;
    uint pad1;
    float4 uvRect;
};
RVK_PUSH(BlitPush, pc);

float4 PSMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    return rvkSample2DLod(pc.srcTex, pc.smp, uv, 0.0);
}
