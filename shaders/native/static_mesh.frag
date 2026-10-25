#version 460
// Native opaque pass, fragment stage (static_mesh.glsl): the engine's material lighting (ge_default_ps.fx / ge_default_phong.fx /
// ge_common.fxh, shaders/material/ge_default.frag) for static meshes, written for the native renderer: ambient (SH, mixed with the
// indoor colour by the vertex ambient occlusion), the directional lightmaps, the sun with the screen-space shadow mask, up to three
// point lights, the 0.45 gamma, the mask's ambient occlusion, radial fog. No specular / self illumination yet.

#extension GL_EXT_buffer_reference_uvec2 : require
#include "static_mesh.glsl"
#include "rvk_common.glsl"

layout(location = 0) in vec3 inWorld;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inTangent;
layout(location = 3) in vec4 inColor0;
layout(location = 4) in vec4 inColor1;
layout(location = 5) in vec4 inUv;
layout(location = 6) in vec2 inLightmap;
layout(location = 7) in vec4 inViewPos;
layout(location = 0) out vec4 outColor;

float sat(float x) { return clamp(x, 0.0, 1.0); }

vec3 ambientSimple(NativeItem it, vec3 n) {
    const vec4 nn = vec4(n, 1.0);
    return vec3(dot(it.ps[15], nn), dot(it.ps[16], nn), dot(it.ps[17], nn));
}

vec3 ambientComplex(NativeItem it, vec3 n) {
    vec3 c = ambientSimple(it, n);
    const vec4 b = n.xyzz * n.yzzx;
    c.r += dot(it.ps[18], b);
    c.g += dot(it.ps[19], b);
    c.b += dot(it.ps[20], b);
    c += it.ps[21].rgb * (n.x * n.x - n.y * n.y);
    return c;
}

// D3D9 alpha test as the engine's state (func 2 less ... 8 always), on a 0..255 reference
bool alphaPass(float a, uint func, float ref) {
    const float q = round(clamp(a, 0.0, 1.0) * 255.0);
    switch (func) {
    case 1u: return false;
    case 2u: return q < ref;
    case 3u: return q == ref;
    case 4u: return q <= ref;
    case 5u: return q > ref;
    case 6u: return q != ref;
    case 7u: return q >= ref;
    default: return true;
    }
}

// the native cascades' sun shadow at a world position (1 lit); `cover` = how far the cascades are the answer (0 beyond their range)
float nativeShadowTerm(NativeShadow sh, vec3 world, vec3 n, vec3 toSun, float viewDepth, out float cover) {
    cover = 0.0;
    int c = -1;
    if (viewDepth < sh.split.x) c = 0;
    else if (viewDepth < sh.split.y) c = 1;
    else if (viewDepth < sh.split.z) c = 2;
    if (c < 0) return 1.0;
    const float texelW = sh.texel[c];
    // slope-scaled bias and normal offset: a surface at a grazing angle to the sun needs more of both (the acne on walls)
    const float ndl = clamp(dot(n, toSun), 0.0, 1.0);
    const float slope = min(sqrt(max(1.0 - ndl * ndl, 0.0)) / max(ndl, 0.15), 8.0);
    const vec4 sc = sh.viewProj[c] * vec4(world + n * (sh.params.z * texelW * (0.5 + 0.5 * min(slope, 4.0))), 1.0);
    const vec2 uv = sc.xy * 0.5 + 0.5;
    const float depth = sc.z - (sh.params.y * texelW * (1.0 + slope)) / sh.range[c];
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const float d = rvkSample2DLod(sh.maps[c], sh.maps.w, uv + vec2(float(x), float(y)) * sh.params.x, 0.0).r;
            lit += depth <= d ? 1.0 : 0.0;
        }
    }
    cover = c < 2 ? 1.0 : 1.0 - smoothstep(sh.split.z * (1.0 - sh.params.w), sh.split.z, viewDepth);
    return lit / 9.0;
}

void main() {
    NativeItem it = NativeItem(pc.item);
    const uint flags = it.tex2.y;

    vec4 diffuse = vec4(0.5, 0.5, 0.5, 1.0);
    if ((flags & NATIVE_NO_ALBEDO) == 0u) diffuse = rvkSample2D(it.tex0.x, it.tex0.y, inUv.xy);
    if ((flags & NATIVE_MASKED) != 0u && !alphaPass(diffuse.a, (it.off1.w >> 8) & 255u, it.params.x)) discard;

    // tangent space, the normal map
    vec3 N = normalize(inNormal);
    vec3 T = normalize(inTangent);
    vec3 B = cross(N, T);
    // no face flip: the engine's viewport is flipped, so every triangle it keeps is 'back facing' here; its materials flip the tangent frame only when
    // double sided, and every static draw uses a culling mode (docs/research/native-renderer.md 9)
    vec3 tn = vec3(0.0, 0.0, 1.0);
    vec3 n = N;
    if ((flags & NATIVE_NORMALMAP) != 0u && it.tex0.z != 0u && pc.passFlags != 12u) {
        const vec4 m = (rvkSample2D(it.tex0.z, it.tex0.w, inUv.zw) - 0.5) * 2.0;
        if ((flags & NATIVE_DXT5NM) != 0u) tn = vec3(m.w, m.y, sqrt(max(1.0 - m.w * m.w - m.y * m.y, 0.0)));
        else tn = normalize(m.xyz);
        n = tn.x * T + tn.y * B + tn.z * N;
    }

    // screen position and the engine's shadow mask / ambient occlusion (s0: .a sun shadow, .r ambient occlusion)
    const vec2 screen = (inViewPos.xy / inViewPos.w) * it.ps[1].xy + it.ps[1].zw;
    vec4 mask = vec4(1.0);
    if (it.tex1.x != 0u && (flags & NATIVE_NO_MASK) == 0u) mask = rvkSample2D(it.tex1.x, it.tex1.y, screen);

    // the sun's shadow: the native cascades inside their range (fading to the engine's mask beyond), else the engine's mask
    float sunShadow = mask.a;
    if (pc.shadow != uvec2(0u)) {
        NativeShadow sh = NativeShadow(pc.shadow);
        float cover;
        const float ours = nativeShadowTerm(sh, inWorld, N, normalize(it.ps[24].xyz), inViewPos.w, cover);
        sunShadow = mix(sunShadow, ours, cover);
    }

    vec3 acc = vec3(0.0);
    vec3 ambTerm = vec3(0.0), lmTerm = vec3(0.0), sunTerm = vec3(0.0);
    // ambient: SH of the normal, the indoor colour where the vertex occlusion is high
    if ((flags & NATIVE_NO_AMBIENT) == 0u) {
        vec3 amb = (flags & NATIVE_AMB_COMPLEX) != 0u ? ambientComplex(it, n) : ambientSimple(it, n);
        amb = mix(it.ps[14].rgb, amb, inColor1.a);
        ambTerm = amb;
        acc += amb;
    }
    // directional lightmaps: two maps, the ambient light direction in their alpha channels
    if ((flags & NATIVE_LIGHTMAP) != 0u) {
        vec4 l1 = rvkSample2D(it.tex1.z, it.tex2.x, inLightmap);
        vec4 l2 = rvkSample2D(it.tex1.w, it.tex2.x, inLightmap);
        l1.a = l1.a * 2.0 - 1.0;
        l2.a = l2.a * 2.0 - 1.0;
        const vec3 dir = vec3(l1.a, l2.a, sqrt(max(1.0 - l1.a * l1.a - l2.a * l2.a, 0.0)));
        const float factor = sat(dot(tn, dir));
        lmTerm = mix(l2, l1, factor).rgb * 4.0;
        acc += lmTerm;
    }
    // the sun (Lambert x the shadow mask)
    if ((flags & NATIVE_NO_SUN) == 0u) {
        const vec3 L = it.ps[24].xyz;
        const float lamb = sat(dot(L, n)) * sunShadow;
        const float selfShadow = (flags & NATIVE_NORMALMAP) != 0u ? sat(4.0 * dot(L, N)) : 1.0;
        sunTerm = it.ps[22].rgb * lamb * selfShadow;
        acc += sunTerm;
    }
    // point lights
    const uint pointCount = it.off1.w & 255u;
    for (uint i = 0u; i < pointCount && i < 4u; ++i) {
        const vec4 posRange = it.ps[25 + 4 * i];
        vec3 Lp = posRange.xyz - inWorld;
        const float falloff = 1.0 - sat(dot(Lp, Lp) * posRange.w);
        Lp = normalize(Lp);
        acc += it.ps[26 + 4 * i].rgb * sat(dot(Lp, n)) * falloff;
    }
    // gamma (high quality) and the mask's screen-space ambient occlusion
    acc = pow(max(acc, vec3(0.0)), vec3(0.45)) * mask.r;

    vec3 color = diffuse.rgb * acc;
    // radial fog
    const float dist = length(it.ps[0].xyz - inWorld);
    const float fog = sat(min(dist * it.ps[4].y + it.ps[4].x, it.ps[4].z));
    color = mix(color, it.ps[3].rgb, fog);
    // debug views (native.debug)
    switch (pc.passFlags) {
    case 1u:
    case 9u:
    case 10u: color = diffuse.rgb; break;
    case 2u: color = n * 0.5 + 0.5; break;
    case 3u: color = vec3(mask.a); break;
    case 4u: color = vec3(mask.r); break;
    case 5u: color = ambTerm; break;
    case 6u: color = lmTerm; break;
    case 7u: color = sunTerm; break;
    case 11u: color = vec3(sunShadow); break;
    case 13u: color = tn * 0.5 + 0.5; break;
    case 8u:
        color = vec3((flags & NATIVE_LIGHTMAP) != 0u ? 1.0 : 0.0, (flags & NATIVE_NORMALMAP) != 0u ? 1.0 : 0.0, (flags & NATIVE_NO_ALBEDO) != 0u ? 1.0 : 0.0);
        if (it.tex1.x == 0u) color += vec3(0.3);
        if ((flags & NATIVE_NO_SUN) != 0u) color.rg *= 0.5;
        if ((flags & NATIVE_NO_MASK) != 0u) color.b *= 0.5;
        break;
    default: break;
    }
    outColor = vec4(color, 1.0);
}
