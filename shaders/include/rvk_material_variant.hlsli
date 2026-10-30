// Variants of the game's material pixel shaders for enhancements (src/enh/material_variant.cpp, docs/research/rtgi.md 9-10),
// HLSL port of rvk_material_variant.glsl: pasted into a translated material pixel shader (HlslResult via GlslOptions::prelude,
// after rvk_gi.hlsli; the translator's header already includes rvk_draw.hlsli) or included by the HLSL ubershader
// (material/ge_default.frag.hlsl, RVK_UBER_HOOKS). The C++ side puts these defines in front:
//
//   RVK_MV_AMBIENT        the shader reads g_AmbientLight at c[RVK_MV_AMB_REG] (7 registers) and, with RVK_MV_HAS_IND,
//                         g_IndoorAmbientColor at c[RVK_MV_IND_REG]: every read of them becomes rvk_gi[1..7] / rvk_gi[0]
//                         (constant overrides), filled by the prologue: the GI probes (when the FrameBlock has a volume) and
//                         the reprojected ambient occlusion (G-buffer variants, when the FrameBlock has one)
//   RVK_MV_GBUFFER        writes the thin G-buffer: location 1 octahedral world normal + id, location 2 motion (enh/gbuffer);
//                         reads this frame's AO and screen probe factor (computed after the z pre-pass) at its pixel
//   RVK_MV_MOTION         + reads the vertex capture's previous world position (location 15): per-object motion (characters)
//   RVK_MV_VNORMAL        the prologue's argument is the interpolated world normal (TEXCOORD4), else float4(0, 0, 0, 0)
//   RVK_MV_KIND_SKINNED   draws::Kind::Skinned (the G-buffer id of characters)
//
// Differences against the GLSL text, all because HLSL has no stage-interface globals:
//  - the entry's pixel position is an argument: the entry point starts with rvkMvPrologue(rvk_fragCoord, <normal input or
//    float4(0, 0, 0, 0)>) and ends with rvkMvEpilogue(oC0) (the C++ side requires colour output 0);
//  - the outputs of RVK_MV_GBUFFER are the globals rvk_mvNormalOut (float4) / rvk_mvMotionOut (float2), set by rvkMvEpilogue;
//    the entry's epilogue copies them into the output struct members rvk_gbNormal (SV_Target1) / rvk_gbMotion (SV_Target2);
//  - RVK_MV_MOTION: the input at location 15 is a member of the pixel input struct; the entry copies it into the global
//    rvk_prevWorld (a static float4 declared below) before the prologue runs.
// The normal-mapped world normal comes from GlslOptions::dotCaptures (the operand the shader dots with g_AmbientLight[0],
// rvk_gbN); without ambient: the interpolated normal, else the geometric one.
#pragma once

#include "rvk_draw.hlsli"  // pc (the draw's push block), rvkPtr, RvkStageData
#include "rvk_gi.hlsli"    // rvkGi* lookup, RvkGiVolume, rvkGiOctEncode

#ifndef RVK_MV_INDOOR_FLOOR
#define RVK_MV_INDOOR_FLOOR 0.6  // share of the engine's average ambient kept as a floor (up to RVK_MV_FLOOR_MAX)
#endif
#ifndef RVK_MV_FLOOR_MAX
#define RVK_MV_FLOOR_MAX 0.15  // L0 level above which the floor stops growing
#endif

static float4 rvk_gi[8];  // [0] = g_IndoorAmbientColor, [1..7] = g_AmbientLight
static uint rvk_giCascade = 0xFFu;
static float3 rvk_giN = float3(0.0, 1.0, 0.0);  // geometric normal (towards the viewer)
static float3 rvk_mvP = float3(0.0, 0.0, 0.0);  // world position
static float4 rvk_gbN = float4(0.0, 0.0, 0.0, 0.0);  // the shaded normal (dot capture / interpolated normal)
static float rvk_mvAo = 1.0;                    // the reprojected ambient occlusion applied (debug)
static float3 rvk_mvSgi = float3(1.0, 1.0, 1.0);  // the screen probes' factor applied (debug)
static float rvk_mvVao = 1.0;                   // the vertex ambient occlusion / indoor factor (TEXCOORD4.w): the engine's ambient = lerp(c14, SH, it)
static float4 rvk_mvFrag = float4(0.0, 0.0, 0.0, 0.0);  // gl_FragCoord (SV_Position), set by the prologue

#ifdef RVK_MV_GBUFFER
static float4 rvk_mvNormalOut = float4(0.0, 0.0, 0.0, 0.0);  // -> SV_Target1, A2B10G10R10: xy octahedral world normal (rvkGiOctEncode), z 0, w id / 3
static float2 rvk_mvMotionOut = float2(0.0, 0.0);            // -> SV_Target2, R16G16F: previous uv - this uv
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
float4 rvkMvGather(uint img, float myZ, bool isAo) {
    int2 sz = rvkSize2D(img);
    int2 c0 = int2(rvk_mvFrag.xy);
    float tol = max(myZ * 0.05, 15.0);
    float3 acc = float3(0.0, 0.0, 0.0);
    float wsum = 0.0, wmax = 0.0;
    for (int i = 0; i < 12; ++i) {
        float r = 4.0 + 16.0 * (float(i) + 0.5) / 12.0;
        float a = float(i) * 2.39996323;
        int2 q = clamp(c0 + int2(round(float2(cos(a), sin(a)) * r)), int2(0, 0), sz - 1);
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
#endif
// GI: the probes' irradiance as the draw's ambient (enh/gi)
void rvkMvGi(RvkFrame f, float3 P, float3 n, float3 V) {
    if (!rvkGiValid(f) || (f.frameInfo.y & RVK_FRAME_CAMERA_VALID) == 0u) return;
    float4 regs[7];
    if (!rvkGiLookup(f, P, n, V, regs, rvk_giCascade)) return;
    RvkGiVolume v = rvkGiVolume(f);
#ifdef RVK_MV_GBUFFER
    // the screen probes (rtgi.md 11): this frame's per-surface correction of the DDGI irradiance (the registers are per
    // colour channel: c15 / c18 red, c16 / c19 green, c17 / c20 blue, c21.rgb)
    uint4 sg = f.sgi;
    if ((sg.z & RVK_FRAME_SGI_VALID) != 0u && sg.x != 0u) {
        // this frame's (computed after the z pre-pass) at this pixel, when its depth is this surface's
        float4 s = rvkFetch2D(sg.x, int2(rvk_mvFrag.xy));
        float myZ = rvkMvViewDepth(f);
        {
            bool hit = s.a > 0.0 && abs(s.a - myZ) < max(myZ * 0.02, 4.0);
            bool direct = hit;
            if (!hit) {
                float4 g = rvkMvGather(sg.x, myZ, false);
                s = float4(lerp(float3(1.0, 1.0, 1.0), g.rgb, g.w), 1.0);
                hit = g.w > 0.0;
            }
            if (hit) {
                float3 k = clamp(s.rgb, float3(0.0, 0.0, 0.0), float3(3.2, 3.2, 3.2));  // luminance ratio (cap 2) x chroma (up to 1.6)
                rvk_mvSgi = k;
                for (int c = 0; c < 3; ++c) {
                    regs[c] *= k[c];
                    regs[3 + c] *= k[c];
                }
                regs[6].rgb *= k;
                // directional correction (gi.directional, rtgi.md 20): the screen probes' slope of the traced irradiance with
                // the normal minus the DDGI's (their luminance, in world space, from the resolve's second image): added to
                // the registers' L1 vector, with the constant term moved so the irradiance at the geometric normal stays
                // what the ratio made it: normal-mapped and curved surfaces then follow the direction the light comes from
                if (direct && sg.w != 0u) {
                    float3 dg = rvkFetch2D(sg.w, int2(rvk_mvFrag.xy)).rgb;
                    float lum = dot(k, float3(0.2126, 0.7152, 0.0722));
                    for (int c = 0; c < 3; ++c) {
                        float3 dc = dg * (lum > 1e-3 ? k[c] / lum : 1.0);
                        regs[c].xyz += dc;
                        regs[c].w -= dot(dc, rvk_giN);
                    }
                }
            }
        }
    }
#endif
    // the draw's own ambient beyond the outdoor sky (indoor colour, static lights; 0 for outdoor draws) stays on top:
    // engine = f x sky + static (static >= 0): f <= L0(engine) / L0(sky) per channel; that bound keeps the remainder's
    // average >= 0 and makes it 0 for outdoor draws (engine = sky)
    float4 eng[7], sk[7];
    for (int i = 0; i < 7; ++i) {
        eng[i] = rvk_gi[1 + i];
        sk[i] = v.Get().sky[i];
    }
    float3 le = max(rvkGiAverage(eng), float3(0.0, 0.0, 0.0));
    float3 ls = rvkGiAverage(sk);
    float fk = 1.0;  // (GLSL: f)
    for (int c = 0; c < 3; ++c) {
        if (ls[c] > 1e-4) fk = min(fk, le[c] / ls[c]);
    }
    fk = clamp(fk, 0.0, 1.0);
    if ((v.Get().info.x & RVK_GI_FLAG_KEEP_STATIC) != 0u) {
        for (int i = 0; i < 7; ++i) regs[i] += eng[i] - sk[i] * fk;
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
void rvkMvAo(RvkFrame f, float3 P) {
    uint4 a = f.ao;
    if ((a.z & RVK_FRAME_AO_VALID) == 0u || a.x == 0u) return;
    // this frame's (computed after the z pre-pass) at this pixel, when its depth is this surface's
    float2 s = rvkFetch2D(a.x, int2(rvk_mvFrag.xy)).rg;
    float myZ = rvkMvViewDepth(f);
    if (s.g <= 0.0 || abs(s.g - myZ) > max(myZ * 0.02, 4.0)) {
        float4 g = rvkMvGather(a.x, myZ, true);
        if (g.w <= 0.0) return;
        s = float2(lerp(1.0, g.x, g.w), myZ);
    }
    float ao = clamp(s.r, 0.0, 1.0);
    float fk = lerp(1.0, ao, clamp(asfloat(a.w), 0.0, 1.0));
    rvk_mvAo = fk;
    for (int i = 1; i < 7; ++i) rvk_gi[i] *= fk;
    rvk_gi[7].rgb *= fk;
    rvk_gi[0].rgb *= fk;
}
#endif
#endif

// `fragCoord` = the pixel's SV_Position (HLSL has no gl_FragCoord outside the entry point)
void rvkMvPrologue(float4 fragCoord, float4 vertexNormal) {
    rvk_mvFrag = fragCoord;
    RvkFrame f = rvkLoadFrame(pc.frame);
#ifdef RVK_MV_AMBIENT
    for (int i = 0; i < 7; ++i) rvk_gi[1 + i] = rvkRawLoad<float4, 16>(rvkAddrAdd(rvkAddr(pc.psw3), uint(RVK_MV_AMB_REG + i) * 16u));  /* c14..c223 = window 3 (abi.h) */
#ifdef RVK_MV_HAS_IND
    rvk_gi[0] = rvkRawLoad<float4, 16>(rvkAddrAdd(rvkAddr(pc.psw3), uint(RVK_MV_IND_REG) * 16u));
#else
    rvk_gi[0] = float4(0.0, 0.0, 0.0, 0.0);
#endif
#endif
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
#ifdef RVK_MV_AMBIENT
    rvkMvGi(f, P, n, V);
#ifdef RVK_MV_GBUFFER
    rvkMvAo(f, P);
#endif
#endif
}

void rvkMvEpilogue(inout float4 c0) {
    RvkFrame f = rvkLoadFrame(pc.frame);
#ifdef RVK_MV_GBUFFER
    float3 n = dot(rvk_gbN.xyz, rvk_gbN.xyz) > 1e-8 ? normalize(rvk_gbN.xyz) : rvk_giN;
    uint kind = pc.flags & RVK_DRAW_KIND_MASK;
    rvk_mvNormalOut = float4(rvkGiOctEncode(n), 0.0, kind == uint(RVK_MV_KIND_SKINNED) ? 2.0 / 3.0 : 1.0 / 3.0);
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
            for (int i = 0; i < 7; ++i) r[i] = rvk_gi[1 + i];
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
