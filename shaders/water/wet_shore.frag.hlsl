// The sea's wet beach (src/enh/water.cpp, docs/research/water.md 4.5, ocean.md 4.4): with the wave grid the waterline sits
// lower than the engine's and swashes up and down the beach, so sand the engine always kept under its water (and its painted
// surf) is seen. That sand is wet: before the refraction copy (AfterOpaque) the scene is darkened where the ground lies in
// the coast band of the shore field and not higher than a little above the swash's reach; where the swash has just drained
// away the sand still carries a film of water: darker and glossy (the sky's reflection, strong at grazing views), drying
// within a few seconds. Blend: dst * src.a + src.rgb. The water drawn after it shows the same sand through its shallow
// edge: no seam at the waterline.

#include "rvk_common.hlsli"
#include "water_noise.hlsli"

struct WetPush {
    RVK_PASS_PUSH_HEADER
    uint depthTex;
    uint fieldTex;
    uint smpLinear;
    uint smpPoint;
    float4 shoreMap;  // xy shore field corner (world xz), zw 1 / its extent
    float4 ocean;     // x sea level, y lowering at the coast, z swash amplitude, w coast band
    float4 params;    // x time (s), y wet height above the swash's reach, z darkening (0..1), w swash period (s, 0 = the M7 swash)
    uint4 sky;        // x the sky cube (tCube slot, 0 = none), y its sampler
};
RVK_PUSH(WetPush, pc);

float4 PSMain(float4 position : SV_Position, float2 inUv : TEXCOORD0) : SV_Target0 {
    RvkFrame f = rvkLoadFrame(pc.frame);
    float d = rvkSample2DLod(pc.depthTex, pc.smpPoint, inUv, 0.0).r;
    float4 outColor = float4(0.0, 0.0, 0.0, 1.0);
    if (d >= 0.995) return outColor;  // sky
    float3 G = rvkWorldFromViewDepth(f, inUv, d * f.cameraPos.w);
    float2 fuv = (G.xz - pc.shoreMap.xy) * pc.shoreMap.zw;
    if (any(fuv < float2(0.0, 0.0)) || any(fuv > float2(1.0, 1.0))) return outColor;
    float coastDist = rvkSample2DLod(pc.fieldTex, pc.smpLinear, fuv, 0.0).x;
    // the beach only: in and next to the engine's coast overlay (quay walls further away stay as they are)
    float near = 1.0 - smoothstep(150.0, 500.0, coastDist);
    if (near <= 0.0) return outColor;
    // the swash's highest reach (water_common.hlsli coastOffset at its crest) + a damp margin above it
    float reach = pc.ocean.x - pc.ocean.y + pc.ocean.z;
    float h = G.y - reach;
    float wet = (1.0 - smoothstep(0.0, pc.params.y, h)) * smoothstep(-80.0, -30.0, h) * near;
    // freshly wet just above the water: darker; drying further up
    float k = pc.params.z * wet * lerp(0.65, 1.0, 1.0 - smoothstep(0.0, pc.params.y * 0.6, h));
    float3 sheen = float3(0.0, 0.0, 0.0);
    if (pc.params.w > 0.0 && pc.ocean.z > 0.0) {
        // the FFT ocean's swash (water_ocean.vert): level - lowering + amplitude * sin(w t + along); this point is under
        // water while sin(phase) > y. The time since the water drained away from it: the film's age
        float y = (G.y - (pc.ocean.x - pc.ocean.y)) / pc.ocean.z;
        float omega = 6.2831853 / pc.params.w;
        float ph = omega * pc.params.x + waterNoised(G.xz * 0.00035).x * 0.8;
        if (y > -1.0 && y < 1.0) {
            float leave = 3.14159265 - asin(y);  // the phase at which the draining water uncovers it
            float since = rvkMod(ph - leave, 6.2831853);
            float wetAge = since / omega;
            // under water right now (sin > y): the water draws over it; the film just after it drained: glossy
            // (a soft front: a hard one drew a jagged line along the sand's height lines where the water's own edge, drawn
            // by the wave grid's interpolated vertices, does not lie exactly on it)
            float under = smoothstep(-0.12, 0.12, sin(ph) - y);
            float film = lerp(exp(-wetAge / 1.6), 1.0, under);
            film *= near;
            float3 V = normalize(f.cameraPos.xyz - G);
            float nv = clamp(V.y, 0.0, 1.0);
            float F = 0.02 + 0.98 * pow(1.0 - nv, 5.0);
            float3 R = float3(-V.x, V.y, -V.z);
            float3 skyC = pc.sky.x != 0u ? RVK_TEXCUBE(pc.sky.x).SampleLevel(RVK_SAMP(pc.sky.y), R, 0.0).rgb
                                         : float3(0.0, 0.0, 0.0);
            sheen = clamp(skyC, 0.0, 1.0) * F * film * 0.85;
            // the film darkens the sand further (the light goes into the wet grains) and dims what the sheen reflects
            k = max(k, pc.params.z * 1.35 * film);
            k = min(k, 0.6);
        }
    }
    return float4(sheen, 1.0 - k);
}
