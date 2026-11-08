// Variants of the game's material pixel shaders for enhancements (src/enh/material_variant.cpp, docs/research/rtgi.md 9-10),
// HLSL port of rvk_material_variant.glsl: pasted into a translated material pixel shader (HlslResult via GlslOptions::prelude,
// after rvk_gi.hlsli; the translator's header already includes rvk_draw.hlsli) or included by the HLSL ubershader
// (material/ge_default.frag.hlsl, RVK_UBER_HOOKS). The C++ side puts these defines in front:
//
//   RVK_MV_AMBIENT        the shader reads g_AmbientLight at c[RVK_MV_AMB_REG] (7 registers) and, with RVK_MV_HAS_IND,
//                         g_IndoorAmbientColor at c[RVK_MV_IND_REG]: every read of them becomes rvk_gi[1..7] / rvk_gi[0]
//                         (constant overrides), filled by the prologue: the GI probes (when the FrameBlock has a volume) and
//                         the reprojected ambient occlusion (G-buffer variants, when the FrameBlock has one)
//   RVK_MV_GBUFFER        the G-buffer family (enh/gbuffer): reads this frame's AO and screen probe factor (computed after the z
//                         pre-pass) at its pixel; writes only the targets a consumer of this session reads (no unread outputs):
//   RVK_MV_GB_NORMAL      + writes the octahedral world normal + material channel + id (SV_Target1)
//   RVK_MV_GB_MOTION      + writes the screen motion (SV_Target2 after the normal, else SV_Target1)
//   RVK_MV_MOTION         + reads the vertex capture's previous world position (location 15): per-object motion (characters;
//                         only with RVK_MV_GB_MOTION)
//   RVK_MV_VNORMAL        the prologue's argument is the interpolated world normal (TEXCOORD4), else float4(0, 0, 0, 0)
//   RVK_MV_KIND_SKINNED   draws::Kind::Skinned (the G-buffer id of characters)
//   RVK_MV_CLUSTER        the point-light registers c[RVK_MV_CL_REG .. + 4 RVK_MV_CL_COUNT) are per-pixel values: the engine's, with the lights the
//                         engine's own list lacks or only this object carries replaced by the best lights of the pixel's cluster of the Full-owned
//                         light table (enh/light_table, rvk_light_model.hlsli); RVK_MV_CL_PARAM = the FrameBlock params index of its reference
//
// Differences against the GLSL text, all because HLSL has no stage-interface globals:
//  - the entry's pixel position is an argument: the entry point starts with rvkMvPrologue(rvk_fragCoord, <normal input or
//    float4(0, 0, 0, 0)>) and ends with rvkMvEpilogue(oC0) (the C++ side requires colour output 0);
//  - the outputs are the globals rvk_mvNormalOut (float4, RVK_MV_GB_NORMAL) / rvk_mvMotionOut (float2, RVK_MV_GB_MOTION), set by
//    rvkMvEpilogue; the entry's epilogue copies them into the output struct members rvk_gbNormal / rvk_gbMotion;
//  - RVK_MV_MOTION: the input at location 15 is a member of the pixel input struct; the entry copies it into the global
//    rvk_prevWorld (a static float4 declared below) before the prologue runs.
// The normal-mapped world normal comes from GlslOptions::dotCaptures (the operand the shader dots with g_AmbientLight[0],
// rvk_gbN); without ambient: the interpolated normal, else the geometric one.
#pragma once

#include "rvk_draw.hlsli"  // pc (the draw's push block), rvkPtr, RvkStageData
#include "rvk_gi.hlsli"    // rvkGi* lookup, RvkGiVolume, rvkGiOctEncode
#include "rvk_light_model.hlsli"  // the light table

#ifndef RVK_MV_INDOOR_FLOOR
#define RVK_MV_INDOOR_FLOOR 0.6  // share of the engine's average ambient kept as a floor (up to RVK_MV_FLOOR_MAX)
#endif
#ifndef RVK_MV_FLOOR_MAX
#define RVK_MV_FLOOR_MAX 0.15  // L0 level above which the floor stops growing
#endif

static float4 rvk_gi[8];  // [0] = g_IndoorAmbientColor, [1..7] = g_AmbientLight
#if defined(RVK_UBER_HOOKS) && defined(RVK_HALF16) && defined(RVK_MV_AMBIENT)
// The ubershader (16-bit build) keeps the ambient as 16 packed words from the end of the prologue on (S-2b, frame-ownership.md 14.8): 16 live VGPRs through
// the texture phase and the light loop instead of 32, the lit core reads it as half anyway; rvkGiGet unpacks one register where it is used
#define RVK_GI_PACKED 1
static uint rvk_giP[16];
void rvkGiPack() {
    [unroll] for (uint i = 0; i < 8; ++i) {
        rvk_giP[2 * i] = f32tof16(rvk_gi[i].x) | (f32tof16(rvk_gi[i].y) << 16);
        rvk_giP[2 * i + 1] = f32tof16(rvk_gi[i].z) | (f32tof16(rvk_gi[i].w) << 16);
    }
}
float4 rvkGiGet(uint i) {
    uint a = rvk_giP[2 * i], b = rvk_giP[2 * i + 1];
    return float4(f16tof32(a & 0xFFFFu), f16tof32(a >> 16), f16tof32(b & 0xFFFFu), f16tof32(b >> 16));
}
#endif
static uint rvk_giCascade = 0xFFu;
static float3 rvk_giN = float3(0.0, 1.0, 0.0);  // geometric normal (towards the viewer)
static float3 rvk_mvP = float3(0.0, 0.0, 0.0);  // world position
static float4 rvk_gbN = float4(0.0, 0.0, 0.0, 0.0);  // the shaded normal (dot capture / interpolated normal)
static float rvk_pbrGb = 0.0;                   // the G-buffer material channel (rvkGbufferMaterialEncode; 0 = no pack entry)
static float rvk_mvAo = 1.0;                    // the reprojected ambient occlusion applied (debug)
static float3 rvk_mvSgi = float3(1.0, 1.0, 1.0);  // the screen probes' factor applied (debug)
static float rvk_mvVao = 1.0;                   // the vertex ambient occlusion / indoor factor (TEXCOORD4.w): the engine's ambient = lerp(c14, SH, it)
static float4 rvk_mvFrag = float4(0.0, 0.0, 0.0, 0.0);  // gl_FragCoord (SV_Position), set by the prologue

#ifdef RVK_MV_GB_NORMAL
static float4 rvk_mvNormalOut = float4(0.0, 0.0, 0.0, 0.0);  // A2B10G10R10: xy octahedral world normal (rvkGiOctEncode), z material channel, w id / 3
#endif
#ifdef RVK_MV_GB_MOTION
static float2 rvk_mvMotionOut = float2(0.0, 0.0);            // R16G16F: previous uv - this uv
#endif
#ifdef RVK_MV_MOTION
static float4 rvk_prevWorld = float4(0.0, 0.0, 0.0, 0.0);  // enh::kPrevWorldLocation (15), copied from the pixel input by the entry
#endif

// this fragment's hardware depth as the engine's D3D depth (rvkHwDepthToD3D in rvk_common.hlsli)
float rvkMvD3dZ(RvkFrame f) { return rvkHwDepthToD3D(f, rvk_mvFrag.z); }

// this fragment's view depth (the engine's row-vector projection: z' = (z a + b) / z)
float rvkMvViewDepth(RvkFrame f) {
    float a = f.proj[2][2], b = f.proj[3][2];
    float z = rvkMvD3dZ(f);
    return abs(z - a) > 1e-9 ? b / (z - a) : 0.0;
}

#ifdef RVK_MV_AMBIENT
#ifdef RVK_MV_GBUFFER
// Fallback for pixels the z pre-pass did not write (characters, alpha: the stored depth there is the background's): a
// depth-aware gather of this frame's AO / screen probe image around the pixel. 12 fixed taps on a spiral (fixed, no per-pixel
// noise: stable), only texels whose stored view depth is close to this surface's count; the share of valid weight is the
// confidence the result is blended in with (rtgi.md 14). float4(value, confidence); .xyz = rgb (sgi) or .x = ao.
// scale: the image's size relative to the render target (1 = same size, 0.5 = the screen probes' low bandwidth images)
float4 rvkMvGather(uint img, float myZ, bool isAo, float scale) {
    int2 sz = rvkSize2D(img);
    int2 c0 = int2(rvk_mvFrag.xy * scale);
    float tol = max(myZ * 0.05, 15.0);
    float3 acc = float3(0.0, 0.0, 0.0);
    float wsum = 0.0, wmax = 0.0;
    for (int i = 0; i < 12; ++i) {
        float r = 4.0 + 16.0 * (float(i) + 0.5) / 12.0;
        float a = float(i) * 2.39996323;
        int2 q = clamp(c0 + int2(round(float2(cos(a), sin(a)) * r * scale)), int2(0, 0), sz - 1);
        float4 t = rvkFetch2D(img, q);
        float z = isAo ? t.g : t.a;
        float w = z > 0.0 ? max(1.0 - abs(z - myZ) / tol, 0.0) : 0.0;
        w *= 1.0 / (1.0 + r * (1.0 / 8.0));
        acc += w * (isAo ? float3(t.r, 0.0, 0.0) : t.rgb);
        wsum += w;
        wmax += 1.0 / (1.0 + r * (1.0 / 8.0));
    }
    if (wsum <= 1e-4) return float4(0.0, 0.0, 0.0, 0.0);
    return float4(acc / wsum, clamp(wsum / (0.5 * wmax), 0.0, 1.0));  // half the taps valid = full confidence
}

// the screen probes' low bandwidth images (half the render size, texel i = pixel 2 i; docs/research/low-bandwidth.md): the 2 x 2 texels around this
// pixel, bilinear x a depth weight (the texel's stored view depth against this surface's: edges keep their own side). Returns the summed weight
// (>= 0.5: a direct hit); `ratio` / `corr` = the weighted means of the first / second image.
float rvkMvSgiTaps(uint img, uint img2, float myZ, out float4 ratio, out float3 corr) {
    float2 hp = rvk_mvFrag.xy * 0.5;
    int2 i0 = int2(floor(hp));
    float2 fr = hp - float2(i0);
    int2 hs = rvkSize2D(img);
    float tol = max(myZ * 0.05, 8.0);
    float3 acc = float3(0.0, 0.0, 0.0);
    float3 cacc = float3(0.0, 0.0, 0.0);
    float wsum = 0.0;
    for (int i = 0; i < 4; ++i) {
        int2 o = int2(i & 1, i >> 1);
        int2 t = clamp(i0 + o, int2(0, 0), hs - 1);
        float4 s = rvkFetch2D(img, t);
        float wz = s.a > 0.0 ? saturate(1.0 - abs(s.a - myZ) / tol) : 0.0;
        float w = (o.x == 0 ? 1.0 - fr.x : fr.x) * (o.y == 0 ? 1.0 - fr.y : fr.y) * wz;
        acc += s.rgb * w;
        if (img2 != 0u) cacc += rvkFetch2D(img2, t).rgb * w;
        wsum += w;
    }
    ratio = float4(acc / max(wsum, 1e-4), 1.0);
    corr = cacc / max(wsum, 1e-4);
    return wsum;
}
#endif
// GI: the probes' irradiance as the draw's ambient (enh/gi)
// the engine's own ambient registers (c15.. = g_AmbientLight, the indoor colour) into rvk_gi
void rvkMvLoadEngineAmbient() {
    for (int i = 0; i < 7; ++i) rvk_gi[1 + i] = rvkRawLoad<float4, 16>(rvkAddrAdd(rvkAddr(rvkPsWin(3u)), uint(RVK_MV_AMB_REG + i) * 16u));  /* c14..c223 = window 3 (abi.h) */
#ifdef RVK_MV_HAS_IND
    rvk_gi[0] = rvkRawLoad<float4, 16>(rvkAddrAdd(rvkAddr(rvkPsWin(3u)), uint(RVK_MV_IND_REG) * 16u));
#else
    rvk_gi[0] = float4(0.0, 0.0, 0.0, 0.0);
#endif
}

// The screen probes' per-surface correction of the DDGI irradiance (rtgi.md 11), read BEFORE the probe lookup (S-2b, frame-ownership.md 14.8: the gathers
// and the lookup no longer overlap: nothing of the 28 summed registers is live while the image taps run): k = the factor per colour channel, dg = the
// directional correction (added to the registers' L1 vector); returns 0 = no correction, 1 = k only, 2 = k and dg
uint rvkMvSgiFactors(RvkFrame f, out float3 kOut, out float3 dgOut) {
    kOut = float3(1.0, 1.0, 1.0);
    dgOut = float3(0.0, 0.0, 0.0);
#ifdef RVK_MV_GBUFFER
    // the screen probes (rtgi.md 11): this frame's per-surface correction of the DDGI irradiance (the registers are per
    // colour channel: c15 / c18 red, c16 / c19 green, c17 / c20 blue, c21.rgb)
    uint4 sg = f.sgi;
    if ((sg.z & RVK_FRAME_SGI_VALID) != 0u && sg.x != 0u) {
        // this frame's (computed after the z pre-pass) at this pixel, when its depth is this surface's
        const bool sgiHalf = (sg.z & RVK_FRAME_SGI_HALF) != 0u;
        float myZ = rvkMvViewDepth(f);
        float4 s = float4(1.0, 1.0, 1.0, 0.0);
        float3 dgTaps = float3(0.0, 0.0, 0.0);
        bool tapsHit = false;
        if (sgiHalf) {
            tapsHit = rvkMvSgiTaps(sg.x, sg.w, myZ, s, dgTaps) >= 0.5;
        } else {
            s = rvkFetch2D(sg.x, int2(rvk_mvFrag.xy));
        }
        bool hit = sgiHalf ? tapsHit : (s.a > 0.0 && abs(s.a - myZ) < max(myZ * 0.02, 4.0));
        bool direct = hit;
        if (!hit) {
            float4 g = rvkMvGather(sg.x, myZ, false, sgiHalf ? 0.5 : 1.0);
            s = float4(lerp(float3(1.0, 1.0, 1.0), g.rgb, g.w), 1.0);
            hit = g.w > 0.0;
        }
        if (hit) {
            float3 k = clamp(s.rgb, float3(0.0, 0.0, 0.0), float3(3.2, 3.2, 3.2));  // luminance ratio (cap 2) x chroma (up to 1.6)
            rvk_mvSgi = k;
            kOut = k;
            // directional correction (gi.directional, rtgi.md 20): the screen probes' slope of the traced irradiance with
            // the normal minus the DDGI's (their luminance, in world space, from the resolve's second image): added to
            // the registers' L1 vector, with the constant term moved so the irradiance at the geometric normal stays
            // what the ratio made it: normal-mapped and curved surfaces then follow the direction the light comes from
            if (direct && sg.w != 0u) {
                float3 dg = sgiHalf ? dgTaps : rvkFetch2D(sg.w, int2(rvk_mvFrag.xy)).rgb;
                float lum = dot(k, float3(0.2126, 0.7152, 0.0722));
                // dc_c = dg x (k_c / lum), applied per channel below
                if (lum > 1e-3) {
                    dgOut = dg / lum;
                    return 2u;
                }
                dgOut = dg;  // (the luminance ~0: dc = dg for every channel)
                return 3u;
            }
            return 1u;
        }
    }
#endif
    return 0u;
}

void rvkMvGi(RvkFrame f, float3 P, float3 n, float3 V) {
    float4 regs[7];
    const bool giOk = rvkGiValid(f) && (f.frameInfo.y & RVK_FRAME_CAMERA_VALID) != 0u;
    float3 sgK, sgDg;
    const uint sgMode = giOk ? rvkMvSgiFactors(f, sgK, sgDg) : 0u;
    // (the volume reference is a real buffer whenever the load below may be hoisted above the giOk test: a zero pair is heap slot 0 = a texture on D3D12, GBV 2026-11-08)
    RvkGiVolume v = rvkPtr<RvkGiVolumeData, 16>(giOk ? f.gi.xy : rvkPsWin(3u));
    if (!giOk || !rvkGiLookupLean(v, P, n, V, regs, rvk_giCascade)) {
        rvkMvLoadEngineAmbient();
        return;
    }
#ifdef RVK_MV_GBUFFER
    if (sgMode != 0u) {
        for (int c = 0; c < 3; ++c) {
            regs[c] *= sgK[c];
            regs[3 + c] *= sgK[c];
        }
        regs[6].rgb *= sgK;
        if (sgMode >= 2u) {
            for (int c = 0; c < 3; ++c) {
                float3 dc = sgMode == 2u ? sgDg * sgK[c] : sgDg;
                regs[c].xyz += dc;
                regs[c].w -= dot(dc, rvk_giN);
            }
        }
    }
#endif
    // the draw's own ambient beyond the outdoor sky (indoor colour, static lights; 0 for outdoor draws) stays on top:
    // engine = f x sky + static (static >= 0): f <= L0(engine) / L0(sky) per channel; that bound keeps the remainder's
    // average >= 0 and makes it 0 for outdoor draws (engine = sky)
    rvkMvLoadEngineAmbient();
    // (the engine's registers and the frame's sky are read where they are used, not copied: 56 live VGPRs less)
    // (rvkGiAverage written out on the registers: no eng[7] / sk[7] copies, 56 live floats less)
    float3 le, ls;
    {
        RvkGiVolume sv = v;
        le = max(float3(rvk_gi[1].w + rvk_gi[4].z / 3.0, rvk_gi[2].w + rvk_gi[5].z / 3.0, rvk_gi[3].w + rvk_gi[6].z / 3.0), float3(0.0, 0.0, 0.0));
        ls = float3(sv.Get().sky[0].w + sv.Get().sky[3].z / 3.0, sv.Get().sky[1].w + sv.Get().sky[4].z / 3.0, sv.Get().sky[2].w + sv.Get().sky[5].z / 3.0);
    }
    float fk = 1.0;  // (GLSL: f)
    for (int c = 0; c < 3; ++c) {
        if (ls[c] > 1e-4) fk = min(fk, le[c] / ls[c]);
    }
    fk = clamp(fk, 0.0, 1.0);
    if ((v.Get().info.x & RVK_GI_FLAG_KEEP_STATIC) != 0u) {
        for (int i = 0; i < 7; ++i) regs[i] += rvk_gi[1 + i] - v.Get().sky[i] * fk;
    }
    // minimum ambient (rtgi.md 13, 18): the probe volume is dark where little light reaches (barrels / tables in the pub were
    // black: the engine's own indoor fill, a dim hemisphere there, is gone) while the engine lit them with it. The average is
    // lifted to a share (0.6) of the engine's, capped at a dim level: never lifts canopy or
    // contact shade of bright ambient, only deep darks (a fade by engine L0 cannot tell the cave from the sunny outdoors: rtgi.md 18)
    {
        // the shadow fill (gi.shadow_fill, rtgi.md 21): a share of the engine's whole ambient as a floor, anywhere (the traced
        // shade under trees / cliffs measured 5 - 40 % darker than the engine's flat ambient); bits 8-15 of info.x
        float fill = float((v.Get().info.x >> 8) & 0xFFu) * (1.0 / 255.0);
        // of the ambient the engine really shows at this pixel (rtgi.md 24): lerp(c14, SH, vertex AO), the level's baked indoor
        // factor (0 in caves / interiors: there the engine's ambient is its dim indoor colour, not the sky's)
#ifdef RVK_MV_HAS_IND
        float3 shown = lerp(max(rvk_gi[0].rgb, float3(0.0, 0.0, 0.0)), le, rvk_mvVao);
#else
        float3 shown = le;
#endif
        float3 want = max(min(le, (float3)(RVK_MV_FLOOR_MAX)) * RVK_MV_INDOOR_FLOOR, shown * fill);
        float3 have = max(rvkGiAverage(regs), float3(0.0, 0.0, 0.0));
        float3 add = max(want - have, float3(0.0, 0.0, 0.0));
        regs[0].w += add.x;
        regs[1].w += add.y;
        regs[2].w += add.z;
    }
    float k = clamp(v.Get().params.x, 0.0, 1.0);
    for (int i = 0; i < 6; ++i) rvk_gi[1 + i] = lerp(rvk_gi[1 + i], regs[i], k);
    rvk_gi[7] = float4(lerp(rvk_gi[7].rgb, regs[6].rgb, k), rvk_gi[7].w);
    rvk_gi[0] = float4(lerp(rvk_gi[0].rgb, rvkGiAverage(regs), k), rvk_gi[0].w);
}

#ifdef RVK_MV_GBUFFER
// ambient occlusion (enh/gbuffer GTAO): this frame's (after the z pre-pass) at this pixel scales the ambient
// (read before the probe lookup, applied after it: rvkMvAmbientNow; 1 = no occlusion image / not valid for this pixel)
float rvkMvAoFactor(RvkFrame f) {
    uint4 a = f.ao;
    if ((a.z & RVK_FRAME_AO_VALID) == 0u || a.x == 0u) return 1.0;
    // this frame's (computed after the z pre-pass) at this pixel, when its depth is this surface's
    float2 s = rvkFetch2D(a.x, int2(rvk_mvFrag.xy)).rg;
    float myZ = rvkMvViewDepth(f);
    if (s.g <= 0.0 || abs(s.g - myZ) > max(myZ * 0.02, 4.0)) {
        float4 g = rvkMvGather(a.x, myZ, true, 1.0);
        if (g.w <= 0.0) return 1.0;
        s = float2(lerp(1.0, g.x, g.w), myZ);
    }
    float ao = clamp(s.r, 0.0, 1.0);
    float fk = lerp(1.0, ao, clamp(asfloat(a.w), 0.0, 1.0));
    rvk_mvAo = fk;
    return fk;
}
void rvkMvAoApply(float fk) {
    if (fk == 1.0) return;
    for (int i = 1; i < 7; ++i) rvk_gi[i] *= fk;
    rvk_gi[7].rgb *= fk;
    rvk_gi[0].rgb *= fk;
}
#endif
#endif

#ifdef RVK_MV_CLUSTER
#ifndef RVK_MV_BAKE_REST
#define RVK_MV_BAKE_REST 4.0  // the pixel's other light (its lit luminance x this) the lamp's share is measured against
#endif
// ---- the Full-owned light table (enh/light_table, docs/research/gpu-scene.md 9.7) ------------------------------------------------------------
// The shader's light loop reads its lights from rvk_cl (constant overrides of c[RVK_MV_CL_REG ..]); the prologue fills them: the engine's values
// (this draw's list), then, when the table is valid for this frame, per pixel
//   - an engine light the table does not know (no table light within 8 units in the pixel's cluster) stays (dynamic lights the registry misses, spells, flashes), as does
//     every engine light with a shadow channel selector (cubemap shadows: the table has none yet) together with its table twin (excluded)
//   - the other slots take the best lights of the pixel's cluster, scored by luminance x falloff x (0.25 + 0.75 sat(N.L)); an unused slot gets
//     colour 0 (the loop still runs, adds nothing)
// style physical: the colour of every table light is scaled by phys(d) / orig(d) (rvkLmPhysicalRatio).
// The per-pixel point-light registers are NOT held as 4 x COUNT float4 any more (S-2b, frame-ownership.md 14.8: 48 live VGPRs through the light loop): a slot
// is compact state (where it comes from + one colour factor) and rvkClReg(slot, comp) reads the values where the light loop uses them: the engine's
// register (a uniform load) or the table light's record (one divergent load), colour x factor.
static uint rvk_clSrc[RVK_MV_CL_COUNT];  // 0 = the slot's own engine registers (kept lights, the table not used), n = table light n - 1
static float rvk_clK[RVK_MV_CL_COUNT];   // the colour factor of the slot (physical law x ray-traced shadow; 0 = an unused slot)
static RvkAddr rvk_clLb;                 // the table's light array
static uint rvk_clCount = 0xFFFFFFFFu;  // lights in the pixel's cluster (0xFFFFFFFF = table not used: the engine's values)
static uint rvk_clUsed = 0;             // table lights written into the registers
static bool rvk_clPhysical = false;     // lighting.style = physical and the table valid for this pixel: the PBR evaluation (rvkMvPbr) runs on every lit draw
static float rvk_clDefRough = 0.7;      // the roughness of surfaces without a pack entry in the physical style (materials.default_roughness)
static float rvk_clShadow = 1.0;        // the smallest ray-traced shadow visibility among them (lighting.debug = shadows)
static float rvk_clShadowTested = 0.0;
static float2 rvk_clBake = float2(0.0, 0.0);  // baked light under lamp shadows (rvkMvBakedShade): x sum of (lamp light x occlusion), y sum of lamp light, over the pixel's shadow-casting lamps
static float rvk_clKeptState = 0.0;  // debug: 1 an engine light kept with a table twin without shadow id, 2 kept and unknown to the table, 3 kept shadow-channel light  // 1 when a table light with a shadow id was among them

// the ray-traced visibility of table light `sid` at this pixel (enh/light_table: pass light.shadow; entries = id + q / 64); 1 = lit / not traced
float rvkMvLightShadow(uint slot, uint size, uint sid) {
    int2 px = int2(rvk_mvFrag.xy);
    if (slot == 0u || sid == 0u || px.x >= int(size & 0xFFFFu) || px.y >= int(size >> 16)) return 1.0;
    float4 e = rvkFetch2D(max(slot, 1u), px);
    float v = 1.0;
    [unroll] for (uint k = 0; k < RVK_LS_ENTRIES; ++k) {
        if (e[k] > 0.0 && uint(floor(e[k] + 0.0001)) == sid) v = floor(frac(e[k] + 0.0001) * 64.0) / 63.0;
    }
    return v;
}

float4 rvkMvEngineReg(uint reg) { return rvkRawLoad<float4, 16>(rvkAddrAdd(rvkAddr(rvkPsWin(3u)), reg * 16u)); }

// register `comp` (0 position + 1 / R^2, 1 diffuse, 2 specular, 3 shadow channel selector) of point-light slot `slot` as the light loop reads it
float4 rvkClReg(uint slot, uint comp) {
    uint src = rvk_clSrc[slot];
    float4 e = rvkMvEngineReg(uint(RVK_MV_CL_REG) + 4u * slot + comp);
    if (src == 0u) {
        if (comp == 1u || comp == 2u) e.rgb *= rvk_clK[slot];
        return e;
    }
    // (a zero reference must never be read: the compiler may hoist this load above the `src == 0` return as a select, and a zero pair is heap slot 0 = a texture on D3D12;
    // rvk_clLb is a real buffer from rvkMvCluster's first line, the index is clamped)
    RvkAddr rec = rvkAddrAdd(rvk_clLb, (max(src, 1u) - 1u) * RVK_LIGHT_BYTES);
    if (comp == 0u) return rvkRawLoad<float4, 16>(rec);
    if (comp == 3u) return float4(0.0, 0.0, 0.0, 0.0);
    float4 c = rvkRawLoad<float4, 16>(rvkAddrAdd(rec, comp == 1u ? 16u : 32u));
    return float4(c.rgb * rvk_clK[slot], e.w);
}

void rvkMvCluster(RvkFrame f, float3 P, float3 ng) {
    rvk_clLb = rvkAddr(rvkPsWin(3u));  // a valid buffer until the table is found (see rvkClReg)
    [unroll] for (uint k0 = 0; k0 < RVK_MV_CL_COUNT; ++k0) {
        rvk_clSrc[k0] = 0u;
        rvk_clK[k0] = 1.0;
    }
    uint2 ref = asuint(f.params[RVK_MV_CL_PARAM].xy);
    float4 ctl = f.params[RVK_MV_CL_PARAM + 1];  // x enabled, y style (0 original, 1 physical), z debug view
    const bool tableOk = (ref.x | ref.y) != 0u && ctl.x >= 0.5;
    RvkAddr base = rvkAddr(tableOk ? ref : rvkPsWin(3u));  // (never the zero pair: the loads below may be hoisted above the test)
    uint4 h0 = rvkRawLoad<uint4, 16>(base);
    if (!tableOk || h0.x != f.frameInfo.x + 1u) return;  // no table / not built this frame (no main view yet, or the pass was skipped)
    rvk_clPhysical = ctl.y > 0.5;
    rvk_clDefRough = ctl.w;
    uint4 offs = rvkRawLoad<uint4, 16>(rvkAddrAdd(base, 48u));
    float2 uv = rvk_mvFrag.xy * f.viewport.zw;
    RvkAddr cb = rvkLightClusterAt(base, rvkMvViewDepth(f), uv);
    uint cnt = min(rvkRawLoad<uint, 4>(cb), RVK_LIGHT_MAX_PER_CLUSTER);
    rvk_clCount = cnt;

    // the engine's lights at this pixel: positions, which carry a shadow selector
    float3 ePos[RVK_MV_CL_COUNT];
    uint selMask = 0u, litMask = 0u, knownMask = 0u;
    uint twinSid[RVK_MV_CL_COUNT];  // the ray-traced shadow id of the table twin of each engine light (0 = none)
    [unroll] for (uint i0 = 0; i0 < RVK_MV_CL_COUNT; ++i0) twinSid[i0] = 0u;
    [unroll] for (uint i = 0; i < RVK_MV_CL_COUNT; ++i) {
        float4 pr = rvkMvEngineReg(uint(RVK_MV_CL_REG) + 4u * i), col = rvkMvEngineReg(uint(RVK_MV_CL_REG) + 4u * i + 1u), sel = rvkMvEngineReg(uint(RVK_MV_CL_REG) + 4u * i + 3u);
        ePos[i] = pr.xyz;
        float3 lv = pr.xyz - P;
        if (rvkLmLum(col.rgb) > 0.0 && pr.w > 0.0 && dot(lv, lv) * pr.w < 1.0) litMask |= 1u << i;
        if (dot(abs(sel), float4(1.0, 1.0, 1.0, 1.0)) > 0.0) selMask |= 1u << i;
    }
    // the cluster's lights: score, keep the best RVK_MV_CL_COUNT
    float cScore[RVK_MV_CL_COUNT];
    uint cIdx[RVK_MV_CL_COUNT];
    [unroll] for (uint i = 0; i < RVK_MV_CL_COUNT; ++i) {
        cScore[i] = 0.0;
        cIdx[i] = 0xFFFFFFFFu;
    }
    RvkAddr lb = rvkAddrAdd(base, offs.x);
    rvk_clLb = lb;
    for (uint j = 0; j < cnt; ++j) {
        uint idx = rvkRawLoad<uint, 4>(rvkAddrAdd(cb, 4u + 4u * j));
        float4 pr = rvkRawLoad<float4, 16>(rvkAddrAdd(lb, idx * RVK_LIGHT_BYTES));
        {
            // lamp shadows on the BAKED light: the lightmaps / probes of static surfaces hold this lamp's light unshadowed by the characters and props, so the
            // traced visibility of every shadow-casting lamp also darkens the pixel by the lamp's share of its light (rvkMvBakedShade, the epilogue)
            uint bsid = rvkRawLoad<uint, 4>(rvkAddrAdd(lb, idx * RVK_LIGHT_BYTES + 52u));
            if (bsid != 0u) {
                float3 blv = pr.xyz - P;
                float bfall = 1.0 - saturate(dot(blv, blv) * pr.w);
                float bnl = saturate(dot(ng, normalize(blv)));
                float4 bcr = rvkRawLoad<float4, 16>(rvkAddrAdd(lb, idx * RVK_LIGHT_BYTES + 16u));
                float be = rvkLmLum(bcr.rgb) * bfall * bnl;
                if (be > 0.0) {
                    float bv = rvkMvLightShadow(h0.z, h0.w, bsid);
                    rvk_clBake += float2(be * (1.0 - bv), be);
                }
            }
        }
        bool twin = false;
        [unroll] for (uint i = 0; i < RVK_MV_CL_COUNT; ++i) {
            float3 dd = ePos[i] - pr.xyz;
            if (dot(dd, dd) < RVK_LM_MATCH_DIST2) {
                knownMask |= 1u << i;
                // (the engine's own light of this object keeps its slot and its colour: the engine weights every static light per object, 0.2 .. 2.0 of
                // the registry's colour (gpu-scene.md 9.10), which the table cannot know; its table twin must not count a second time)
                if ((selMask >> i & 1u) != 0u || rvkLmLum(rvkMvEngineReg(uint(RVK_MV_CL_REG) + 4u * i + 1u).rgb) > 0.0) {
                    twin = true;
                    if ((selMask >> i & 1u) == 0u) {
                        uint tsid = rvkRawLoad<uint, 4>(rvkAddrAdd(lb, idx * RVK_LIGHT_BYTES + 52u));
                        if (tsid != 0u) twinSid[i] = tsid;
                    }
                }
            }
        }
        if (twin) continue;
        float3 lv = pr.xyz - P;
        float fall = 1.0 - saturate(dot(lv, lv) * pr.w);
        if (fall <= 0.0) continue;
        float4 cr = rvkRawLoad<float4, 16>(rvkAddrAdd(lb, idx * RVK_LIGHT_BYTES + 16u));
        float nl = saturate(dot(ng, normalize(lv)));
        float sc = rvkLmLum(cr.rgb) * fall * (0.25 + 0.75 * nl);
        // insertion into the sorted top list
        [unroll] for (uint i = 0; i < RVK_MV_CL_COUNT; ++i) {
            if (sc > cScore[i]) {
                float ts = cScore[i];
                uint ti = cIdx[i];
                cScore[i] = sc;
                cIdx[i] = idx;
                sc = ts;
                idx = ti;
                if (idx == 0xFFFFFFFFu) sc = 0.0;
            }
        }
    }
    // the slots: kept (shadow selector, or lit by a light the table does not know) or refilled from the top list
    uint next = 0;
    [unroll] for (uint i = 0; i < RVK_MV_CL_COUNT; ++i) {
        bool keep = (selMask >> i & 1u) != 0u || rvkLmLum(rvkMvEngineReg(uint(RVK_MV_CL_REG) + 4u * i + 1u).rgb) > 0.0;
        if (keep) {
            float kk = 1.0;
            if (ctl.y > 0.5 && (selMask >> i & 1u) == 0u && (knownMask >> i & 1u) != 0u) {
                // physical style: the engine's light of this slot follows the physical distance law as well
                float3 lv = ePos[i] - P;
                kk = rvkLmPhysicalRatio(dot(lv, lv), rvkMvEngineReg(uint(RVK_MV_CL_REG) + 4u * i).w);
            }
            if (twinSid[i] == 0u) rvk_clKeptState = max(rvk_clKeptState, (selMask >> i & 1u) != 0u ? 3.0 : ((knownMask >> i & 1u) != 0u ? 1.0 : 2.0));
            if (twinSid[i] != 0u) {
                // the engine's own (unshadowed) light of this slot: its table twin's ray-traced visibility shades it (the shadow-channel lights keep the engine's mask)
                float sv = rvkMvLightShadow(h0.z, h0.w, twinSid[i]);
                kk *= sv;
                rvk_clShadow = min(rvk_clShadow, sv);
                rvk_clShadowTested = 1.0;
            }
            rvk_clK[i] = kk;
            continue;
        }
        uint pick = 0xFFFFFFFFu;
        [unroll] for (uint q = 0; q < RVK_MV_CL_COUNT; ++q) {
            if (q == next) pick = cIdx[q];
        }
        if (pick != 0xFFFFFFFFu) {
            ++next;
            float k = 1.0;
            const uint pk = min(pick, 0xFFFFu);  // (never an out-of-table record index, whatever a hoisted load does)
            if (ctl.y > 0.5) {
                float4 pr = rvkRawLoad<float4, 16>(rvkAddrAdd(lb, pk * RVK_LIGHT_BYTES));
                float3 lv = pr.xyz - P;
                k = rvkLmPhysicalRatio(dot(lv, lv), pr.w);
            }
            uint sid = rvkRawLoad<uint, 4>(rvkAddrAdd(lb, pk * RVK_LIGHT_BYTES + 52u));
            if (sid != 0u) {
                float sv = rvkMvLightShadow(h0.z, h0.w, sid);
                k *= sv;
                rvk_clShadow = min(rvk_clShadow, sv);
                rvk_clShadowTested = 1.0;
            }
            rvk_clSrc[i] = pick + 1u;
            rvk_clK[i] = k;
            ++rvk_clUsed;
        } else {
            rvk_clK[i] = 0.0;  // an unused slot: colour 0 (the loop still runs, adds nothing)
        }
    }
}
#endif

// The prologue in three pieces (S-3, frame-ownership.md 14.7): the ubershader runs rvkMvBegin first and the two heavy ones where their results are
// used (rvkMvAmbientNow right before the ambient term, rvkMvClusterNow right before the point lights), so the 32 + 48 registers of the GI /
// cluster results are not live across the texture fetches and the normal map; the translated shaders (a prologue at the top of the entry) call
// rvkMvPrologue = all three in the old order.
// `fragCoord` = the pixel's SV_Position (HLSL has no gl_FragCoord outside the entry point)
void rvkMvBegin(float4 fragCoord, float4 vertexNormal) {
    rvk_mvFrag = fragCoord;
    RvkFrame f = rvkLoadFrame(pc.frame);
    // the pixel's world position (hardware depth + the FrameBlock camera; D3D9 pixel centres) and geometric normal
    float2 uv = (fragCoord.xy - 0.5) * f.viewport.zw;
    // the depth was rasterised with the camera jitter (src/enh/upscale): the fragment is at the pixel's NDC - J (rvk_common.hlsli)
    float2 jitterNdc = rvkJitterNdc(f);
    float4 w = mul(float4(uv.x * 2.0 - 1.0 - jitterNdc.x, 1.0 - uv.y * 2.0 - jitterNdc.y, rvkMvD3dZ(f), 1.0), f.invViewProj);
    float3 P = w.xyz / w.w;
    float3 n = cross(ddx(P), ddy(P));
    float3 V = normalize(f.cameraPos.xyz - P);
    n = dot(n, n) > 1e-12 ? normalize(n) : V;
    if (dot(n, V) < 0.0) n = -n;
    rvk_mvP = P;
    rvk_giN = n;
#ifdef RVK_MV_VNORMAL
    rvk_gbN = float4(vertexNormal.xyz, 0.0);
#endif
#ifdef RVK_MV_VNORMAL
    rvk_mvVao = clamp(vertexNormal.w, 0.0, 1.0);
#endif
}

#ifdef RVK_MV_CLUSTER
void rvkMvClusterNow() { rvkMvCluster(rvkLoadFrame(pc.frame), rvk_mvP, rvk_giN); }
#endif

#ifdef RVK_MV_AMBIENT
void rvkMvAmbientNow() {
    RvkFrame f = rvkLoadFrame(pc.frame);
#ifdef RVK_MV_GBUFFER
    const float aoK = rvkMvAoFactor(f);  // (the image taps first, then the lookup: the two do not overlap)
#endif
    rvkMvGi(f, rvk_mvP, rvk_giN, normalize(f.cameraPos.xyz - rvk_mvP));  // (loads the engine's registers into rvk_gi after the probe lookup: 32 VGPRs fewer live in it)
#ifdef RVK_MV_GBUFFER
    rvkMvAoApply(aoK);
#endif
#ifdef RVK_GI_PACKED
    rvkGiPack();
#endif
}
#endif

void rvkMvPrologue(float4 fragCoord, float4 vertexNormal) {
    rvkMvBegin(fragCoord, vertexNormal);
#ifdef RVK_MV_CLUSTER
    rvkMvClusterNow();
#endif
#ifdef RVK_MV_AMBIENT
    rvkMvAmbientNow();
#endif
}

#ifdef RVK_MV_PBR
// ---- material pack (docs/research/material-pack.md, src/enh/material_pack.*): physically based specular on top of the engine's lighting ----------
// Draws whose material has a pack entry carry its word in the DrawBlock (alpha.w): bindless slot of the map (bits 0-13), flags and constants
// above (enh::matpack::encodeWord). No entry = word 0 = this function returns before touching anything (the pixels are the engine's).
// C++ side defines: RVK_MV_PBR_DIR_REG (c# of g_DirLight, -1 none), RVK_MV_PBR_PNT_REG / RVK_MV_PBR_PNT_COUNT (g_PointLights, 4 registers each),
// RVK_MV_PBR_SHADOW_SLOT (the shadow mask's sampler). The translator captures the diffuse texld (coordinate, colour) of sampler s9.
// Frame params[2] = (quality 0 off .. 3, specular strength, environment strength, debug view).
#ifdef RVK_MV_CLUSTER
#define RVK_MV_PL(k) rvkClReg((k) >> 2, (k) & 3)  // the per-pixel point-light registers (RVK_MV_PBR_PNT_REG == RVK_MV_CL_REG)
#else
#define RVK_MV_PL(k) RVK_PSC(RVK_MV_PBR_PNT_REG + (k))
#endif
static float4 rvk_pbrUv = float4(0.0, 0.0, 0.0, 0.0);      // the diffuse coordinate (xy)
static float4 rvk_pbrAlbedo = float4(0.5, 0.5, 0.5, 1.0);  // the diffuse sample
static bool rvk_pbrOk = false;                              // the shader sampled s9

float rvkPbrGgx(float NoH, float a) {
    float a2 = a * a;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * d * d + 1e-6);
}
float rvkPbrVis(float NoV, float NoL, float a) {  // height-correlated Smith GGX, divided by 4 NoL NoV
    float a2 = a * a;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / (gv + gl + 1e-4);
}
float3 rvkPbrLight(float3 N, float3 V, float3 L, float3 radiance, float a, float3 F0, float atten) {
    float NoL = saturate(dot(N, L));
    if (NoL <= 0.0 || atten <= 0.0) return float3(0.0, 0.0, 0.0);
    float3 H = normalize(V + L);
    float NoH = saturate(dot(N, H)), VoH = saturate(dot(V, H)), NoV = max(dot(N, V), 1e-3);
    float3 F = F0 + (1.0 - F0) * pow(1.0 - VoH, 5.0);
    return radiance * F * (rvkPbrGgx(NoH, a) * rvkPbrVis(NoV, NoL, a) * NoL * atten);
}

void rvkMvPbr(RvkFrame f, inout float4 c0) {
    RvkDrawData d = rvkLoadDraw();
    uint word = d.alpha.w;
    bool hasEntry = (word & (1u << 14)) != 0u && f.params[2].x >= 0.5;
    // lighting.style = physical (enh/light_table): every lit draw of a pixel the table covers gets the PBR evaluation; without a pack entry the surface is a
    // dielectric of the default roughness (no maps, no wetness)
    bool phys = false;
#ifdef RVK_MV_CLUSTER
    phys = rvk_clPhysical && rvk_clCount != 0xFFFFFFFFu;
#endif
    if (!hasEntry && !phys) return;
    float quality = hasEntry ? f.params[2].x : 3.0;
    if (!hasEntry) word = 0u;
    uint slot = word & 0x3FFFu;
    bool orm = (word & (1u << 15)) != 0u;
    float rScale = float((word >> 16) & 15u) * (1.0 / 8.0);
    float rBias = float((word >> 20) & 15u) * (1.0 / 15.0);
    float mScale = float((word >> 24) & 15u) * (1.0 / 15.0);
    float wet = float((word >> 28) & 3u) * (1.0 / 3.0) * f.params[3].x;  // the entry's wetness x materials.wetness
    bool glass = (word & (1u << 30)) != 0u;
    // a DERIVED word (no pack entry; roughness from the engine's specular data, enh::matpack::kWordDerived): slot 0 + bit 15, roughness = bits 16-23 / 255
    bool derived = hasEntry && slot == 0u && orm;
    if (derived) {
        rScale = 0.0;
        rBias = float((word >> 16) & 255u) * (1.0 / 255.0);
    }
#ifdef RVK_MV_CLUSTER
    if (!hasEntry) {
        rScale = 0.0;
        rBias = rvk_clDefRough;
        wet = 0.0;
    }
#endif
    float4 t = float4(1.0, 1.0, 1.0, 0.0);
    if (slot != 0u && rvk_pbrOk) t = RVK_TEX2D(slot).Sample(RVK_SAMP(rvkDrawSmp(d, 9u)), rvk_pbrUv.xy);
    float rough = saturate((slot == 0u ? 1.0 : (orm ? t.g : t.r)) * rScale + rBias);
    rough *= 1.0 - 0.7 * wet;  // wet surfaces: smoother ...
    float occl = slot != 0u && orm ? t.r : 1.0;
    float metal = saturate(slot != 0u && orm ? t.b * mScale : mScale);
    if (hasEntry) rvk_pbrGb = rvkGbufferMaterialEncode(rough, metal);
    float emis = slot != 0u && orm && (word >> 31) != 0u ? t.a : 0.0;
    uint dbg = uint(f.params[2].w + 0.5);
    if (dbg == 1u) { c0.rgb = float3(rough, rough, rough); return; }
    if (dbg == 2u) { c0.rgb = float3(metal, metal, metal); return; }
    if (dbg == 3u) { c0.rgb = float3(occl, rough, metal); return; }
    if (dbg == 4u) { c0.rgb = lerp(c0.rgb, derived ? float3(1.0, 0.55, 0.05) : float3(0.1, 1.0, 0.2), 0.6); return; }  // coverage: green = pack entry, orange = derived

    float3 P = rvk_mvP;
    float3 V = normalize(f.cameraPos.xyz - P);
    float3 N = dot(rvk_gbN.xyz, rvk_gbN.xyz) > 1e-8 ? normalize(rvk_gbN.xyz) : rvk_giN;
    if (dot(N, V) < 0.0) N = reflect(N, V);  // back-facing normal-mapped texels: stay in the front hemisphere
    float3 albedo = rvk_pbrOk ? saturate(rvk_pbrAlbedo.rgb) : float3(0.5, 0.5, 0.5);
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
    float a = max(rough * rough, 0.0025);

    // the engine's fog on this pixel (ge_default.fxh RadialFog): undone so the new terms and the metal diffuse reduction act on the lit colour
    float4 fogColor = RVK_PSC(3), fogSet = RVK_PSC(4);
    float fogK = saturate(min(distance(f.cameraPos.xyz, P) * fogSet.y + fogSet.x, fogSet.z));
    float3 lit = fogK < 0.999 ? (c0.rgb - fogColor.rgb * fogK) / (1.0 - fogK) : c0.rgb;
    lit *= 1.0 - 0.35 * wet;  // ... and darker (water in the pores)
    if (phys) {
        // energy conservation: the light the specular lobe reflects is not diffused: the diffuse is scaled by (1 - F(N.V)) relative to normal incidence
        // (the engine's look is the normal-incidence one), less so on rough surfaces whose lobe spreads the energy
        float3 Fv = F0 + (float3(1.0, 1.0, 1.0) - F0) * pow(1.0 - saturate(dot(N, V)), 5.0);
        float3 kD = saturate((float3(1.0, 1.0, 1.0) - Fv) / (float3(1.0, 1.0, 1.0) - F0));
        lit *= lerp(float3(1.0, 1.0, 1.0), kD, 1.0 - rough);
    }

    // lights: the sun (specular colour, the shadow mask's alpha), the point lights of this pass
    float4 shadowMask = float4(1.0, 1.0, 1.0, 1.0);
#if RVK_MV_PBR_SHADOW_SLOT >= 0
    // (only when the pixel shader samples the mask itself: the draw path transitions the textures a shader declares, others may be render targets)
    uint shadowTex = rvkDrawTex(d, uint(RVK_MV_PBR_SHADOW_SLOT));
    if (shadowTex != 0u) shadowMask = RVK_TEX2D(shadowTex).Sample(RVK_SAMP(rvkDrawSmp(d, uint(RVK_MV_PBR_SHADOW_SLOT))), (rvk_mvFrag.xy) * f.viewport.zw);
#endif
    float3 specSum = float3(0.0, 0.0, 0.0);
    // (a derived surface keeps the engine's own Phong highlights: no direct GGX on top, only the environment reflection and the G-buffer channel)
#if RVK_MV_PBR_DIR_REG >= 0
    if (!derived) specSum += rvkPbrLight(N, V, normalize(RVK_PSC(RVK_MV_PBR_DIR_REG + 2).xyz), RVK_PSC(RVK_MV_PBR_DIR_REG + 1).rgb, a, F0, shadowMask.a);
#endif
#if RVK_MV_PBR_PNT_COUNT > 0
    if (quality > 1.5 && !derived) {
        [unroll] for (int i = 0; i < RVK_MV_PBR_PNT_COUNT; ++i) {
            float4 pr = RVK_MV_PL(4 * i);
            float3 lv = pr.xyz - P;
            float fall = 1.0 - saturate(dot(lv, lv) * pr.w);
            float sh = 1.0 - dot(RVK_MV_PL(4 * i + 3), shadowMask);
            // (the engine's point lights often carry a weak specular colour: the brighter of it and the diffuse colour drives the highlight)
            specSum += rvkPbrLight(N, V, normalize(lv), max(RVK_MV_PL(4 * i + 2).rgb, RVK_MV_PL(4 * i + 1).rgb), a, F0, fall * saturate(sh));
        }
    }
#endif
    // the sky cube (display-space colour) blurred by the roughness, weighted by the split-sum Fresnel and the indoor factor
    float3 env = float3(0.0, 0.0, 0.0);
    if (f.images.w != 0u && f.params[2].z > 0.0) {
        float3 R = reflect(-V, N);
        float NoV = saturate(dot(N, V));
        float3 Fe = F0 + (max(float3(1.0 - rough, 1.0 - rough, 1.0 - rough), F0) - F0) * pow(1.0 - NoV, 5.0);
        float3 sky = RVK_TEXCUBE(f.images.w).SampleLevel(RVK_SAMP(rvkDrawSmp(d, 9u)), R, rough * 6.0).rgb;
        // (rough surfaces reflect a wide blur of the sky: the lobe's energy is spread, so its grazing peak is toned down)
        // (the sky cube is drawn at its own brightness: scaled by how much light reaches the surface (the engine's lit colour), so a night plaza does not reflect a day sky)
        float lit01 = saturate(dot(lit, float3(0.3, 0.59, 0.11)) * 3.0);
        env = sky * Fe * smoothstep(-0.15, 0.25, R.y) * rvk_mvVao * occl * f.params[2].z * lerp(1.0, 0.25, rough) * lit01 * 0.7;
        // never more than a third of the engine's own lit brightness of this surface (a reflection cannot outshine the light that reaches it)
        float lumLit = dot(max(lit, float3(0.0, 0.0, 0.0)), float3(0.3, 0.59, 0.11));
        float lumEnv = dot(env, float3(0.3, 0.59, 0.11));
        if (lumEnv > 1e-4) env *= min(1.0, (0.35 * lumLit + 0.0004) / lumEnv);
    }
    float3 spec = pow(max(specSum * f.params[2].y, float3(0.0, 0.0, 0.0)), float3(0.45, 0.45, 0.45));
    spec = min(spec, float3(4.0, 4.0, 4.0));
    // diffuse: less of it on metals; the rest is the engine's lighting
    // the added light never exceeds 0.15 (rough) to 0.55 (smooth) of what the engine's lighting already shows on this surface (+ a small floor): a moon / sun at grazing angles on a rough
    // dark floor otherwise doubles its brightness (the first night / dock shots); metals keep their own scale (they have little diffuse to compare with)
    float3 addLight = spec + env;
    float lumAdd = dot(addLight, float3(0.3, 0.59, 0.11));
    float lumBase = dot(max(lit, float3(0.0, 0.0, 0.0)), float3(0.3, 0.59, 0.11));
    // (smooth surfaces may shine up to 0.55 of their own brightness, rough ones 0.15: a rough floor under a moon must not turn into frost)
    float capLum = lerp((lerp(0.55, 0.15, saturate(rough * 2.0)) * lumBase + 0.0004), 1.0e3, glass ? 1.0 : metal);
    if (lumAdd > capLum) addLight *= capLum / lumAdd;
    float3 shaded = lit * (1.0 - metal) + addLight + albedo * emis;
    c0.rgb = lerp(shaded, fogColor.rgb, fogK);
}
#endif

void rvkMvEpilogue(inout float4 c0) {
    RvkFrame f = rvkLoadFrame(pc.frame);
#ifdef RVK_MV_CLUSTER
    // lighting.debug: 1 = lights in the pixel's cluster (blue 1 .. red 16+), 2 = table lights that fill the shader's slots (0 .. 3)
    {
        uint dbg = uint(f.params[RVK_MV_CL_PARAM + 1].z + 0.5);
        if (dbg == 1u && rvk_clCount != 0xFFFFFFFFu) {
            float t = saturate(float(rvk_clCount) / 16.0);
            c0.rgb = rvk_clCount == 0u ? float3(0.0, 0.0, 0.0) : lerp(float3(0.0, 0.3, 1.0), float3(1.0, 0.1, 0.0), t);
        } else if (dbg == 3u && rvk_clCount != 0xFFFFFFFFu) {
            // ray-traced light shadows: grey = no shadow-casting light here, white lit .. red shadowed
            c0.rgb = rvk_clShadowTested > 0.5 ? lerp(float3(1.0, 0.0, 0.0), float3(1.0, 1.0, 1.0), rvk_clShadow) : (rvk_clKeptState > 2.5 ? float3(0.6, 0.0, 0.6) : rvk_clKeptState > 1.5 ? float3(0.0, 0.5, 0.0) : rvk_clKeptState > 0.5 ? float3(0.6, 0.6, 0.0) : float3(0.0, 0.0, 0.25));
        } else if (dbg == 2u && rvk_clCount != 0xFFFFFFFFu) {
            c0.rgb = float3(rvk_clUsed == 1u || rvk_clUsed == 3u ? 1.0 : 0.0, rvk_clUsed >= 2u ? 1.0 : 0.0, rvk_clUsed == 0u ? 0.3 : 0.0);
        }
    }
#endif
#ifdef RVK_MV_CLUSTER
    // lamp shadows on baked light (not on characters: their lamp light is the table's, already shaded)
    if (rvk_clBake.x > 0.0 && (rvkDrawFlags() & RVK_DRAW_KIND_MASK) != uint(RVK_MV_KIND_SKINNED)) {
        float dark = saturate(f.params[RVK_MV_CL_PARAM].z);  // lighting.light_shadow_baked
        float lumC = dot(max(c0.rgb, float3(0.0, 0.0, 0.0)), float3(0.3, 0.59, 0.11));
        c0.rgb *= 1.0 - dark * saturate(rvk_clBake.x / (rvk_clBake.y + lumC * RVK_MV_BAKE_REST + 0.05));
    }
#endif
#ifdef RVK_MV_PBR
    rvkMvPbr(f, c0);
#endif
#ifdef RVK_MV_GB_NORMAL
    float3 n = dot(rvk_gbN.xyz, rvk_gbN.xyz) > 1e-8 ? normalize(rvk_gbN.xyz) : rvk_giN;
    uint kind = rvkDrawFlags() & RVK_DRAW_KIND_MASK;
    rvk_mvNormalOut = float4(rvkGiOctEncode(n), rvk_pbrGb, kind == uint(RVK_MV_KIND_SKINNED) ? 2.0 / 3.0 : 1.0 / 3.0);
#endif
#ifdef RVK_MV_GB_MOTION
    float3 prevP = rvk_mvP;
#ifdef RVK_MV_MOTION
    if (rvk_prevWorld.w > 0.5) prevP = rvk_prevWorld.xyz;
#endif
    // screen motion: the previous frame's uv of this surface point - this frame's (both projected: the same convention)
    float4 cc = mul(float4(rvk_mvP, 1.0), f.viewProj);
    float4 pp = mul(float4(prevP, 1.0), f.prevViewProj);
    float2 cuv = cc.xy / max(cc.w, 1e-6) * float2(0.5, -0.5) + 0.5;
    float2 puv = pp.xy / max(pp.w, 1e-6) * float2(0.5, -0.5) + 0.5;
    rvk_mvMotionOut = (pp.w > 1e-4 && cc.w > 1e-4) ? puv - cuv : float2(0.0, 0.0);
#endif
#ifdef RVK_MV_AMBIENT
    // gi.debug: 1 the ambient on a white surface (geometric normal), 2 the GI cascade used
    if (rvkGiValid(f)) {
        uint dbg = rvkGiVolume(f).Get().info.y;
        if (dbg == 1u) {
            float4 r[7];
#ifdef RVK_GI_PACKED
            for (int i = 0; i < 7; ++i) r[i] = rvkGiGet(1 + i);
#else
            for (int i = 0; i < 7; ++i) r[i] = rvk_gi[1 + i];
#endif
            c0 = float4(pow(max(rvkGiEvalRegs(r, rvk_giN), float3(0.0, 0.0, 0.0)), float3(0.45, 0.45, 0.45)), c0.a);
        } else if (dbg == 3u) {
            c0 = float4(rvk_mvVao, rvk_mvVao, rvk_mvVao, c0.a);
        } else if (dbg == 2u) {
            float3 col = rvk_giCascade == 0u ? float3(1.0, 0.25, 0.25)
                       : rvk_giCascade == 1u ? float3(0.25, 1.0, 0.25)
                       : rvk_giCascade == 2u ? float3(0.3, 0.45, 1.0)
                                             : float3(0.3, 0.0, 0.0);
            c0 = float4(col * (0.35 + 0.65 * clamp(dot(c0.rgb, float3(0.333, 0.333, 0.333)) * 2.0, 0.0, 1.0)), c0.a);
        }
    }
#endif
}
