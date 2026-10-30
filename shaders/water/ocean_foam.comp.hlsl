// The surf's foam coverage (docs/research/ocean.md 4.4): a camera-centred window of the sea (N x N cells, addressed
// toroidally: cell c lives in texel c mod N, so the window moves with the camera without copying) holding per cell
//   r  fresh white water: set where a crest breaks and on the bores rolling across the surf zone, decays in ~1 s
//   g  residual foam: what the white water leaves behind; decays slowly (~8 s), drifts towards the beach with the broken
//      waves and spreads (the shading turns low coverage into lace: water_noise.hlsli foamDensity)
//   b  contact foam: at walls, rocks and hulls, pulsing with the slosh
// Each frame: the history (last frame's image, bilinear through a wrapping sampler) advected back along the drift, decayed,
// the sources of this frame added. Cells entering the window start from an estimate of the steady state.
#include "rvk_common.hlsli"
#include "water_noise.hlsli"
#include "ocean_shore.hlsli"

// mirror: enh::OceanFoamPush
struct FoamPush {
    RVK_PASS_PUSH_HEADER
    float4 window;     // xy the window's first cell (cell index), z cell size (world units), w N
    float4 prev;       // xy last frame's first cell, z dt (s), w 1 = the history is valid
    float4 floorMap;   // the floor field: xy corner (world xz), zw 1 / extent
    uint4 tex;         // x floor A, y floor B, z the history (t2D), w linear clamp sampler
    uint4 info;        // x linear wrap sampler
    float4 shoreWave;  // x period (s), y amplitude (world units), z breaker index, w time (s, the water's)
};
RVK_PUSH(FoamPush, pc);

[[vk::image_format("rgba16f")]] RWTexture2D<float4> foamOut : register(u0, space1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    int2 p = int2(id.xy);
    int n = int(pc.window.w);
    if (p.x >= n || p.y >= n) return;
    int2 first = int2(pc.window.xy);
    int2 cell = first + ((p - first) % n + n) % n;  // the cell of the window this texel holds
    float cs = pc.window.z;
    float2 xz = (float2(cell) + 0.5) * cs;
    int2 pf = int2(pc.prev.xy);
    bool known = pc.prev.w > 0.5 && all(cell >= pf) && all(cell < pf + int2(n, n));
    float dt = pc.prev.z;
    float t = pc.shoreWave.w;

    SeaFloor f = seaFloorAt(pc.tex.x, pc.tex.y, pc.tex.w, pc.floorMap, xz);
    if (f.sd <= 0.0) {
        foamOut[p] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    ShoreTrain st = shoreTrainAt(pc.shoreWave, f, t);
    Surf s = surfAt(pc.shoreWave, f, st, xz, t);
    float coh = length(f.dir);
    float2 dir = coh > 1e-4 ? f.dir / coh : float2(0.0, 0.0);

    // ---- sources ----
    // a crest plunging (where and while it breaks), the bores rolling across the surf zone after it
    // (a bore loses its white water as it runs on towards the beach: less of it the further it is past the break)
    // rough seas: more and longer-lasting foam (amplitude of the waves rolling in: 25 .. 160 units)
    float storm = smoothstep(50.0, 160.0, pc.shoreWave.y);
    float white = max(s.breakW * s.front, s.surfZone * s.front * lerp(0.2, 0.55, s.patchy) * lerp(0.5, 1.0, st.breaking) * lerp(1.0, 1.6, storm));
    // walls, rocks, hulls: the water sloshing against them (a local oscillation at the train's period, out of phase along
    // the wall)
    float period = max(pc.shoreWave.x, 2.0);
    float slosh = 0.5 + 0.5 * sin(6.2831853 / period * t + 4.0 * waterNoise(xz / 500.0));
    float wall = (1.0 - smoothstep(25.0, 55.0 + 90.0 * slosh, f.sd)) * lerp(0.35, 0.8, slosh);

    // ---- history: advected back along the drift (the broken waves carry their foam towards the beach), spread ----
    float4 old;
    if (known) {
        float drift = 20.0 + 90.0 * s.surfZone;  // world units / s
        float2 back = xz - dir * drift * dt;
        float span = cs * float(n);
        uint hist = pc.tex.z;
        uint smp = pc.info.x;
        old = rvkSample2DLod(hist, smp, back / span, 0.0);
        // spreading: a little of the neighbours mixed in
        float4 nb = rvkSample2DLod(hist, smp, (back + float2(cs, 0.0)) / span, 0.0) + rvkSample2DLod(hist, smp, (back - float2(cs, 0.0)) / span, 0.0) +
                    rvkSample2DLod(hist, smp, (back + float2(0.0, cs)) / span, 0.0) + rvkSample2DLod(hist, smp, (back - float2(0.0, cs)) / span, 0.0);
        old = lerp(old, nb * 0.25, clamp(dt * 0.6, 0.0, 1.0));
        if (any(isnan(old)) || any(isinf(old))) old = float4(0.0, 0.0, 0.0, 0.0);
    } else {
        // a cell entering the window: about the steady state (the residual foam of the surf zone, the contact foam)
        old = float4(0.0, s.surfZone * (0.08 + 0.15 * s.trail) * s.patchy, wall * 0.6, 0.0);
    }

    // ---- decay: the white water turns into residual foam, the residual foam dissolves ----
    float fr = old.r * exp(-2.2 * dt);
    float res = old.g * exp(-lerp(0.28, 0.12, storm) * dt) + (old.r - fr) * lerp(0.16, 0.45, storm);
    // outside the surf zone (deeper water) the foam dissolves faster
    res *= exp(-dt * 0.25 * (1.0 - s.surfZone));
    fr = max(fr, clamp(white, 0.0, 1.0));
    float con = max(old.b * exp(-0.9 * dt), wall);
    foamOut[p] = float4(clamp(fr, 0.0, 1.0), clamp(res, 0.0, 1.0), clamp(con, 0.0, 1.0), 0.0);
}
