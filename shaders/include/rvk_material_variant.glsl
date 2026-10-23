// Variants of the game's material pixel shaders for enhancements (src/enh/material_variant.cpp, docs/research/rtgi.md 9-10):
// the translator pastes this text (after rvk_abi.glsl and rvk_gi.glsl, embedded at build time) into a translated material
// pixel shader (GlslOptions::prelude), the C++ side puts these defines in front:
//
//   RVK_MV_AMBIENT        the shader reads g_AmbientLight at c[RVK_MV_AMB_REG] (7 registers) and, with RVK_MV_HAS_IND,
//                         g_IndoorAmbientColor at c[RVK_MV_IND_REG]: every read of them becomes rvk_gi[1..7] / rvk_gi[0]
//                         (constant overrides), filled by the prologue: the GI probes (when the FrameBlock has a volume) and
//                         the reprojected ambient occlusion (G-buffer variants, when the FrameBlock has one)
//   RVK_MV_GBUFFER        writes the thin G-buffer: location 1 octahedral world normal + id, location 2 motion (enh/gbuffer)
//   RVK_MV_MOTION         + reads the vertex capture's previous world position (location 15): per-object motion (characters)
//   RVK_MV_VNORMAL        the prologue's argument is the interpolated world normal (TEXCOORD4), else vec4(0)
//   RVK_MV_KIND_SKINNED   draws::Kind::Skinned (the G-buffer id of characters)
//
// main() starts with rvkMvPrologue(<normal input or vec4(0)>) and ends with rvkMvEpilogue(oC0) (the C++ side requires colour
// output 0). The normal-mapped world normal comes from GlslOptions::dotCaptures (the operand the shader dots with
// g_AmbientLight[0], rvk_gbN); without ambient: the interpolated normal, else the geometric one.

vec4 rvk_gi[8];  // [0] = g_IndoorAmbientColor, [1..7] = g_AmbientLight
uint rvk_giCascade = 0xFFu;
vec3 rvk_giN = vec3(0.0, 1.0, 0.0);  // geometric normal (towards the viewer)
vec3 rvk_mvP = vec3(0.0);            // world position
vec4 rvk_gbN = vec4(0.0);            // the shaded normal (dot capture / interpolated normal)
float rvk_mvAo = 1.0;                // the reprojected ambient occlusion applied (debug)
vec3 rvk_mvSgi = vec3(1.0);          // the screen probes' factor applied (debug)

#ifdef RVK_MV_GBUFFER
layout(location = 1) out vec4 rvk_gbNormal;  // A2B10G10R10: xy octahedral world normal (rvkGiOctEncode), z 0, w id / 3
layout(location = 2) out vec2 rvk_gbMotion;  // R16G16F: previous uv - this uv
#endif
#ifdef RVK_MV_MOTION
layout(location = 15) in vec4 rvk_prevWorld;  // enh::kPrevWorldLocation
#endif

#ifdef RVK_MV_AMBIENT
// GI: the probes' irradiance as the draw's ambient (enh/gi)
void rvkMvGi(vec3 P, vec3 n, vec3 V) {
    if (!rvkGiValid(pc.frame) || (pc.frame.frameInfo.y & RVK_FRAME_CAMERA_VALID) == 0u) return;
    vec4 regs[7];
    if (!rvkGiLookup(pc.frame, P, n, V, regs, rvk_giCascade)) return;
    RvkGiVolume v = rvkGiVolume(pc.frame);
#ifdef RVK_MV_GBUFFER
    // the screen probes (rtgi.md 11): last frame's per-surface correction of the DDGI irradiance at this point's previous
    // screen position (the registers are per colour channel: c15 / c18 red, c16 / c19 green, c17 / c20 blue, c21.rgb)
    uvec4 sg = pc.frame.sgi;
    if ((sg.z & RVK_FRAME_SGI_VALID) != 0u && sg.x != 0u) {
        vec4 pp = pc.frame.prevViewProj * vec4(P, 1.0);
        vec2 suv = pp.xy / max(pp.w, 1e-6) * vec2(0.5, -0.5) + 0.5;
        // outside last frame's screen (turning: the strip that just came into view) the nearest edge texel continues it: no
        // hard edge between corrected and uncorrected light
        suv = clamp(suv, vec2(0.0), vec2(1.0));
        if (pp.w > 1e-4) {
            vec4 s = textureLod(sampler2D(t2D[sg.x], samp[sg.y]), suv, 0.0);
            if (s.a > 0.5) {
                vec3 k = clamp(s.rgb, vec3(0.0), vec3(2.0));
                rvk_mvSgi = k;
                for (int c = 0; c < 3; ++c) {
                    regs[c] *= k[c];
                    regs[3 + c] *= k[c];
                }
                regs[6].rgb *= k;
            }
        }
    }
#endif
    // the draw's own ambient beyond the outdoor sky (indoor colour, static lights; 0 for outdoor draws) stays on top:
    // engine = f x sky + static (static >= 0): f <= L0(engine) / L0(sky) per channel; that bound keeps the remainder's
    // average >= 0 and makes it 0 for outdoor draws (engine = sky)
    if ((v.info.x & RVK_GI_FLAG_KEEP_STATIC) != 0u) {
        vec4 eng[7], sk[7];
        for (int i = 0; i < 7; ++i) {
            eng[i] = rvk_gi[1 + i];
            sk[i] = v.sky[i];
        }
        vec3 le = max(rvkGiAverage(eng), vec3(0.0)), ls = rvkGiAverage(sk);
        float f = 1.0;
        for (int c = 0; c < 3; ++c) {
            if (ls[c] > 1e-4) f = min(f, le[c] / ls[c]);
        }
        f = clamp(f, 0.0, 1.0);
        for (int i = 0; i < 7; ++i) regs[i] += eng[i] - sk[i] * f;
    }
    float k = clamp(v.params.x, 0.0, 1.0);
    for (int i = 0; i < 6; ++i) rvk_gi[1 + i] = mix(rvk_gi[1 + i], regs[i], k);
    rvk_gi[7] = vec4(mix(rvk_gi[7].rgb, regs[6].rgb, k), rvk_gi[7].w);
    rvk_gi[0] = vec4(mix(rvk_gi[0].rgb, rvkGiAverage(regs), k), rvk_gi[0].w);
}

#ifdef RVK_MV_GBUFFER
// ambient occlusion (enh/gbuffer GTAO): last frame's image at this point's previous screen position scales the ambient
void rvkMvAo(vec3 P) {
    uvec4 a = pc.frame.ao;
    if ((a.z & RVK_FRAME_AO_VALID) == 0u || a.x == 0u) return;
    vec4 pp = pc.frame.prevViewProj * vec4(P, 1.0);
    if (pp.w <= 1e-4) return;
    vec2 uv = pp.xy / pp.w * vec2(0.5, -0.5) + 0.5;
    uv = clamp(uv, vec2(0.0), vec2(1.0));  // outside last frame's screen: its edge continues (no hard edge when turning)
    float ao = clamp(textureLod(sampler2D(t2D[a.x], samp[a.y]), uv, 0.0).r, 0.0, 1.0);
    float f = mix(1.0, ao, clamp(uintBitsToFloat(a.w), 0.0, 1.0));
    rvk_mvAo = f;
    for (int i = 1; i < 7; ++i) rvk_gi[i] *= f;
    rvk_gi[7].rgb *= f;
    rvk_gi[0].rgb *= f;
}
#endif
#endif

void rvkMvPrologue(vec4 vertexNormal) {
#ifdef RVK_MV_AMBIENT
    for (int i = 0; i < 7; ++i) rvk_gi[1 + i] = pc.ps.c[RVK_MV_AMB_REG + i];
#ifdef RVK_MV_HAS_IND
    rvk_gi[0] = pc.ps.c[RVK_MV_IND_REG];
#else
    rvk_gi[0] = vec4(0.0);
#endif
#endif
    // the pixel's world position (hardware depth + the FrameBlock camera; D3D9 pixel centres) and geometric normal
    vec2 uv = (gl_FragCoord.xy - 0.5) * pc.frame.viewport.zw;
    vec4 w = pc.frame.invViewProj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, gl_FragCoord.z, 1.0);
    vec3 P = w.xyz / w.w;
    vec3 n = cross(dFdx(P), dFdy(P));
    vec3 V = normalize(pc.frame.cameraPos.xyz - P);
    n = dot(n, n) > 1e-12 ? normalize(n) : V;
    if (dot(n, V) < 0.0) n = -n;
    rvk_mvP = P;
    rvk_giN = n;
#ifdef RVK_MV_VNORMAL
    rvk_gbN = vec4(vertexNormal.xyz, 0.0);
#endif
#ifdef RVK_MV_AMBIENT
    rvkMvGi(P, n, V);
#ifdef RVK_MV_GBUFFER
    rvkMvAo(P);
#endif
#endif
}

void rvkMvEpilogue(inout vec4 c0) {
#ifdef RVK_MV_GBUFFER
    vec3 n = dot(rvk_gbN.xyz, rvk_gbN.xyz) > 1e-8 ? normalize(rvk_gbN.xyz) : rvk_giN;
    uint kind = pc.flags & RVK_DRAW_KIND_MASK;
    rvk_gbNormal = vec4(rvkGiOctEncode(n), 0.0, kind == uint(RVK_MV_KIND_SKINNED) ? 2.0 / 3.0 : 1.0 / 3.0);
    vec3 prevP = rvk_mvP;
#ifdef RVK_MV_MOTION
    if (rvk_prevWorld.w > 0.5) prevP = rvk_prevWorld.xyz;
#endif
    // screen motion: the previous frame's uv of this surface point - this frame's (both projected: the same convention)
    vec4 cc = pc.frame.viewProj * vec4(rvk_mvP, 1.0);
    vec4 pp = pc.frame.prevViewProj * vec4(prevP, 1.0);
    vec2 cuv = cc.xy / max(cc.w, 1e-6) * vec2(0.5, -0.5) + 0.5;
    vec2 puv = pp.xy / max(pp.w, 1e-6) * vec2(0.5, -0.5) + 0.5;
    rvk_gbMotion = pp.w > 1e-4 && cc.w > 1e-4 ? puv - cuv : vec2(0.0);
#endif
#ifdef RVK_MV_AMBIENT
    // gi.debug: 1 the ambient on a white surface (geometric normal), 2 the GI cascade used
    if (rvkGiValid(pc.frame)) {
        uint dbg = rvkGiVolume(pc.frame).info.y;
        if (dbg == 1u) {
            vec4 r[7];
            for (int i = 0; i < 7; ++i) r[i] = rvk_gi[1 + i];
            c0 = vec4(pow(max(rvkGiEvalRegs(r, rvk_giN), vec3(0.0)), vec3(0.45)), c0.a);
        } else if (dbg == 2u) {
            vec3 col = rvk_giCascade == 0u ? vec3(1.0, 0.25, 0.25)
                     : rvk_giCascade == 1u ? vec3(0.25, 1.0, 0.25)
                     : rvk_giCascade == 2u ? vec3(0.3, 0.45, 1.0)
                                           : vec3(0.3, 0.0, 0.0);
            c0 = vec4(col * (0.35 + 0.65 * clamp(dot(c0.rgb, vec3(0.333)) * 2.0, 0.0, 1.0)), c0.a);
        }
    }
#endif
}
