// Ray-traced soft sun shadows, stage 1 (src/enh/rt_shadows.cpp, docs/research/rt-shadows.md): per pixel of the engine's depth
// map within the range, `rays` shadow rays from the surface towards a random point of the sun's disc (a cone of the artistic
// half angle: the penumbra grows with the distance of the occluder, i.e. contact hardening comes from the geometry), against
// the frame's TLAS with the alpha test of cut-outs (leaves) in the candidate loop. Output: the raw visibility (1 lit).

#include "rt_shadow_common.hlsli"
#include "rvk_rt_trace.hlsli"

[[vk::image_format("rgba8")]] RWTexture2D<float4> outRaw : register(u0, space1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    int2 q = int2(id.xy);
    RtShadowParams p = rtsParams();
    if (q.x >= int(p.info.x) || q.y >= int(p.info.y)) return;
    RvkFrame f = rvkLoadFrame(pc.frame);
    float d = rtsDepthMap(p, q);
    float vz = d * f.cameraPos.w;
    float vis = 1.0;
    if (d < RTS_SKY && vz < p.range.x && rvkRtValid(f)) {
        float3 P = rtsPos(f, p, q, d);
        float3 N = rtsNormal(f, p, q, d, P);
        float3 L = normalize(p.sun.xyz);
        // the origin: off the surface towards the camera, more with the distance (the depth's reconstruction error and the
        // RT scene's meshes against the rasterised ones grow with it)
        float3 o = P + N * (p.range.z + vz * 0.0006);
        float3 T = normalize(cross(L, abs(L.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0)));
        float3 B = cross(L, T);
        uint rays = max(p.info.z, 1u);
        if (rays > 1u) {
            // the second ray is for pixels still in doubt: a settled history (long, fully lit or fully blocked, no variance) is
            // confirmed by one ray as well as by two, only the penumbra and new pixels gain from the extra sample
            float4 h = rtsHistory(f, p, P);
            if (h.w > 0.05 && h.y >= 4.0 && max(h.z - h.x * h.x, 0.0) < 0.002 && (h.x < 0.03 || h.x > 0.97)) rays = 1u;
        }
        float sum = 0.0;
        for (uint r = 0u; r < rays; ++r) {
            // a disc sample: interleaved gradient noise per pixel, rotated per frame and ray (the temporal pass integrates them)
            uint fr = p.mode.x * rays + r;
            float u1 = rtsIgn(float2(q), fr);
            float u2 = rtsIgn(float2(q) + float2(37.0, 17.0), fr);
            float rad = sqrt(u1) * p.sun.w;
            float phi = 6.2831853 * u2;
            float3 dir = normalize(L + (T * cos(phi) + B * sin(phi)) * rad);
            // a sample below the surface's tangent plane is no light path at all (the material's N.L already darkens the
            // terminator): counting it lit keeps the surface from shadowing itself at grazing sun, where the random samples
            // that dip into it made an interleaved-gradient-noise comb along every terminator
            if (dot(dir, N) < 0.02) {
                sum += 1.0;
                continue;
            }
            // the ray cone is zero: the sun disc is sampled by the ray direction (the penumbra), so adding its angle (and the
            // pixel's) to the cone as well read a leaf's opacity 5-7 mips down, where the cut-out averages to a solid card and
            // every canopy cast a black blob; the cut-out is tested at its own resolution, the temporal pass averages the noise
            sum += rvkRtVisibility(f, o, dir, 0.25, p.range.y, 0.0, 0.0, p.info.w);
        }
        vis = sum / float(rays);
    }
    outRaw[q] = float4(vis, 0.0, 0.0, 1.0);
}
