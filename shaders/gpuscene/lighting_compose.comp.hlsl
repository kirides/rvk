// The lighting register images' compose pass (src/gpuscene/lighting_images.*, frame-ownership.md 7.9): thread i composes entry i = (illumination slot,
// compose parameters) of the job table from the illumination / light-set records with `rvkIllumCompose` and stores the 24 float4 (c14 .. c37) at
// images + 4096 + i x 384. Free entries (illum = 0xffffffff) are skipped. Registers the engine function leaves alone (c14.w) are 0.

#include "rvk_illum.hlsli"

struct LightingComposePush {
    uint2 illums;  // read: the IllumRecord table
    uint2 sets;    // read: the LightSetRecord table
    uint2 jobs;    // read: uint4 per entry: x illum slot (0xffffffff = free), y flags (bit 0 tableLit, bit 1 holdsAll), z n, w firstLight
    uint2 images;  // write: the images buffer
    uint count;    // entry slots in use
    uint pad0;
    uint pad1;
    uint pad2;
};
RVK_PUSH(LightingComposePush, pc);

static const uint LIGHTING_IMAGE_PAD = 4096u;
static const uint LIGHTING_IMAGE_STRIDE = 384u;

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.count) return;
    const uint4 job = rvkRawLoad<uint4, 16>(rvkAddrAdd(rvkAddr(pc.jobs), id.x * 16u));
    if (job.x == 0xffffffffu) return;
    RvkIllumParams p;
    p.tableLit = job.y & 1u;
    p.holdsAll = (job.y >> 1u) & 1u;
    p.n = job.z;
    p.firstLight = job.w;
    float4 L[24];
    [unroll] for (uint k = 0u; k < 24u; ++k) L[k] = float4(0, 0, 0, 0);
    rvkIllumCompose(pc.illums, job.x, pc.sets, p, L);
    const RvkAddr dst = rvkAddrAdd(rvkAddr(pc.images), LIGHTING_IMAGE_PAD + id.x * LIGHTING_IMAGE_STRIDE);
    [unroll] for (uint r = 0u; r < 24u; ++r) rvkRwStore<float4, 16>(rvkAddrAdd(dst, r * 16u), L[r]);
}
