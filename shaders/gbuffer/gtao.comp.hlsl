// Ground-truth ambient occlusion (M10 P7, src/enh/gbuffer.cpp, docs/research/rtgi.md 10): XeGTAO-style horizon search (Jimenez
// et al. 2016, "Practical realtime strategies for accurate indirect occlusion"; the slice formulation as in Intel's XeGTAO,
// MIT, reimplemented) from the main view's hardware depth and the G-buffer normal, per pixel 2 slices x 6 steps on each side,
// the slice angle and step offsets jittered per pixel and frame (the temporal filter integrates them). Pixels the material
// passes did not reach (no G-buffer id: sky, unlit effects) also get the camera's motion here.

#include "gbuffer_common.hlsli"

[[vk::image_format("r32f")]] RWTexture2D<float> outAo : register(u0, space1);
// pc.flags: 1 = the AO (after the z pre-pass), 2 = the G-buffer's motion fill (after the opaque scene)
[[vk::image_format("rg16f")]] RWTexture2D<float2> motionImage : register(u1, space1);

static const int GB_SLICES = 2;
static const int GB_STEPS = 8;

float depthAt(int2 p) { return rvkHwDepthToD3D(rvkLoadFrame(pc.frame), rvkFetch2D(pc.depthTex, p).r); }

// interleaved gradient noise (Jimenez 2014), shifted per frame
float ign(float2 p, uint frame) {
    p += 5.588238 * float(frame & 63u);
    return frac(52.9829189 * frac(0.06711056 * p.x + 0.00583715 * p.y));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    int2 p = int2(id.xy);
    if (p.x >= int(pc.width) || p.y >= int(pc.height)) return;
    RvkFrame f = rvkLoadFrame(pc.frame);
    float2 size = float2(pc.width, pc.height);
    float2 uv = (float2(p) + 0.5) / size;
    float z = depthAt(p);
    if ((pc.flags & 2u) != 0u) {
        // the camera's motion where no material pass wrote one (id 0)
        float4 nrm = rvkFetch2D(pc.normalTex, p);
        if (nrm.w < 0.16) motionImage[p] = gbCameraMotion(f, gbWorldPos(f, uv, z));
        return;
    }
    if (z >= 1.0) {
        outAo[p] = 1.0;
        return;
    }
    float3 P = gbViewPos(f, uv, z);
    // the geometric normal from the depth (the G-buffer's is normal-mapped: bumps would tilt it into the depth surface and
    // occlude themselves): per axis the neighbour nearer in depth (no smearing across edges)
    int2 maxP = int2(size) - 1;
    float3 pl = gbViewPos(f, uv - float2(1.0 / size.x, 0.0), depthAt(max(p - int2(1, 0), int2(0, 0))));
    float3 pr = gbViewPos(f, uv + float2(1.0 / size.x, 0.0), depthAt(min(p + int2(1, 0), maxP)));
    float3 pu = gbViewPos(f, uv - float2(0.0, 1.0 / size.y), depthAt(max(p - int2(0, 1), int2(0, 0))));
    float3 pd = gbViewPos(f, uv + float2(0.0, 1.0 / size.y), depthAt(min(p + int2(0, 1), maxP)));
    float3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;
    float3 dy = abs(pd.z - P.z) < abs(P.z - pu.z) ? pd - P : P - pu;
    float3 N = cross(dy, dx);
    N = dot(N, N) > 1e-12 ? normalize(N) : float3(0.0, 0.0, -1.0);
    float3 V = normalize(-P);
    if (dot(N, V) < 0.0) N = -N;
    // the radius on screen (pixels)
    float radiusPx = pc.radius * f.proj[0][0] / max(P.z, 1.0) * 0.5 * size.x;
    if (radiusPx < 1.5) {
        outAo[p] = 1.0;
        return;
    }
    radiusPx = min(radiusPx, 160.0);
    float n0 = ign(float2(p), pc.frameIndex), n1 = ign(float2(p) + float2(13.0, 7.0), pc.frameIndex);
    float falloffRange = max(pc.falloff * pc.radius, 1.0);
    float visibility = 0.0;
    for (int s = 0; s < GB_SLICES; ++s) {
        float phi = (float(s) + n0) * GB_PI / float(GB_SLICES);
        float2 dir = float2(cos(phi), sin(phi));     // screen (pixels, y down)
        float3 dirV = float3(dir.x, -dir.y, 0.0);    // the same direction in view space (y up)
        float3 orthoDir = dirV - dot(dirV, V) * V;
        float3 axis = normalize(cross(orthoDir, V));
        float3 projN = N - axis * dot(N, axis);
        float projLen = length(projN);
        float cosN = clamp(dot(projN, V) / max(projLen, 1e-5), 0.0, 1.0);
        float n = (dot(orthoDir, projN) >= 0.0 ? 1.0 : -1.0) * acos(cosN);
        // the horizons on the + and - side, starting at the tangent plane
        float hcPlus = cos(n + GB_HALF_PI), hcMinus = cos(n - GB_HALF_PI);
        float lowPlus = hcPlus, lowMinus = hcMinus;
        for (int j = 0; j < GB_STEPS; ++j) {
            float t = (float(j) + n1) / float(GB_STEPS);
            float off = max(t * t * radiusPx, float(j) + 1.0);
            for (int side = 0; side < 2; ++side) {
                float sgn = side == 0 ? 1.0 : -1.0;
                float2 q = float2(p) + 0.5 + sgn * dir * off;
                if (any(q < float2(0.0, 0.0)) || any(q >= size)) continue;
                float sz = depthAt(int2(q));
                float3 S = gbViewPos(f, q / size, sz);
                float3 d = S - P;
                float dl = length(d);
                if (dl < 1e-3) continue;
                float shc = dot(d / dl, V);
                float weight = clamp((pc.radius - dl) / falloffRange, 0.0, 1.0);
                if (side == 0) {
                    hcPlus = max(hcPlus, lerp(lowPlus, shc, weight));
                } else {
                    hcMinus = max(hcMinus, lerp(lowMinus, shc, weight));
                }
            }
        }
        float hPlus = n + clamp(acos(clamp(hcPlus, -1.0, 1.0)) - n, -GB_HALF_PI, GB_HALF_PI);
        float hMinus = n + clamp(-acos(clamp(hcMinus, -1.0, 1.0)) - n, -GB_HALF_PI, GB_HALF_PI);
        float sinN = sin(n);
        float arcPlus = -cos(2.0 * hPlus - n) + cosN + 2.0 * hPlus * sinN;
        float arcMinus = -cos(2.0 * hMinus - n) + cosN + 2.0 * hMinus * sinN;
        visibility += projLen * 0.25 * (arcPlus + arcMinus);
    }
    float ao = clamp(visibility / float(GB_SLICES), 0.0, 1.0);
    outAo[p] = ao;
}
