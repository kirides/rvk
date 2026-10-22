// The FFT ocean's surface (src/enh/ocean.h, src/enh/sea_floor.h, docs/research/ocean.md): sampling the cascades, the sea
// floor field, the attenuation towards the shore and inside the harbour, the waves rolling in. Included by water_common.glsl.

bool fftMode(WaterBlock w) { return w.fftInfo.x == 1u && w.fftInfo.y > 0u; }

// the sea floor field (src/enh/sea_floor.h): signed distance to the waterline, depth, direction the waves travel to the
// shore (x coherence), travel time to the shore, exposure to the open sea, distance to the engine's coast overlay
struct SeaFloor {
    float sd;        // world units, + water
    float depth;     // world units (sea level - the highest surface), negative on land
    vec2 dir;        // unit direction towards the shore x coherence of the fronts
    float travel;    // s
    float exposure;  // 0 sheltered .. 1 open
    float overlay;   // distance to the engine's coast overlay (beaches)
    bool valid;
};

SeaFloor seaFloor(WaterBlock w, vec2 xz) {
    SeaFloor f;
    f.valid = w.floorTex.z != 0u;
    if (!f.valid) {
        // no floor field yet: the M7 shore field (distance to the coast overlay) as a stand-in
        vec4 sf = shoreField(w, xz);
        f.sd = sf.x;
        f.depth = 60.0 + sf.x * 0.06;
        f.dir = sf.zw;
        f.travel = sf.x / 600.0;
        f.exposure = 1.0;
        f.overlay = sf.x;
        return f;
    }
    vec2 uv = (xz - w.floorMap.xy) * w.floorMap.zw;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        f.sd = 30000.0;
        f.depth = 3000.0;
        f.dir = vec2(0.0);
        f.travel = 60.0;
        f.exposure = 1.0;
        f.overlay = 30000.0;
        return f;
    }
    vec4 a = rvkSample2DLod(w.floorTex.x, w.smpOurs.x, uv, 0.0);
    vec4 b = rvkSample2DLod(w.floorTex.y, w.smpOurs.x, uv, 0.0);
    f.sd = a.x;
    f.depth = a.y;
    f.dir = a.zw;
    f.travel = b.x;
    f.exposure = b.y;
    f.overlay = b.z;
    return f;
}

// how much of each cascade survives at a place: waves feel the bottom when the water is shallower than about half their
// length (the long ones first), they need open water to exist (no swell right at the waterline), and harbour basins / coves
// are sheltered from the long waves (exposure) while the short wind chop still forms inside
vec4 cascadeWeights(WaterBlock w, SeaFloor f) {
    // the share of each cascade a fully sheltered basin loses (long swell: most of it; short chop: little)
    const vec4 SHELTER = vec4(0.85, 0.75, 0.45, 0.2);
    vec4 wt = vec4(0.0);
    float d = max(f.depth, 0.0);
    for (int c = 0; c < 4; ++c) {
        float lambda = max(w.fftBand[c], 1.0);
        float depthT = 1.0 - exp(-d / (0.18 * lambda));
        float distT = smoothstep(0.0, 0.8 * lambda, f.sd);
        float shelter = mix(1.0, f.exposure, SHELTER[c]);
        wt[c] = depthT * distT * shelter;
    }
    // breaking limit: the sea is never higher than ~0.4 x its depth (amplitude ~ Hs / 2)
    float amp = 0.5 * w.seaInfo.x * max(wt.x, max(wt.y, 0.3 * wt.z));
    wt *= clamp(0.4 * d / max(amp, 1.0), 0.0, 1.0);
    return wt;
}

// near the camera at the sea's level (wading, swimming) the waves are capped (the engine's physics plane stays flat)
float nearCameraCap(WaterBlock w, vec2 xz, vec3 cam) {
    float camDist = length(xz - cam.xz);
    float capFrac = clamp(w.misc.x / max(0.5 * w.seaInfo.x, 1.0), 0.0, 1.0);
    float low = 1.0 - smoothstep(200.0, 700.0, cam.y - w.ocean.x);  // only when the camera is down at the water (swimming, wading)
    return mix(1.0, mix(capFrac, 1.0, smoothstep(700.0, 3000.0, camDist)), low);
}

struct FftSample {
    vec3 d;       // displacement, world units
    vec2 slope;   // d height / d xz (of the displaced surface)
    float foam;   // Jacobian foam (temporal)
    float jac;    // stretch (1 + dDx/dx)(1 + dDz/dz): < 1 compressed (crests)
};

// the cascades at the undisplaced xz, weighted; footprint = the vertex spacing there (world units): finer waves are taken
// from the mip chain (no aliasing on the coarse grid levels)
FftSample fftSampleLod(WaterBlock w, vec2 xz, vec4 wt, float footprint) {
    FftSample s;
    s.d = vec3(0.0);
    vec4 dv = vec4(0.0);  // d dy/dx, d dy/dz, d dx/dx, d dz/dz
    s.foam = 0.0;
    uint n = w.fftInfo.y;
    float texels = float(w.fftInfo.w);
    for (uint c = 0u; c < n; ++c) {
        if (wt[c] <= 0.0) continue;
        float len = w.fftLength[c];
        vec2 uv = xz / len;
        float lod = log2(max(footprint * texels / len, 1.0));
        vec4 disp = textureLod(sampler2D(t2D[nonuniformEXT(w.fftDisp[c])], samp[nonuniformEXT(w.fftInfo.z)]), uv, lod);
        vec4 der = textureLod(sampler2D(t2D[nonuniformEXT(w.fftDeriv[c])], samp[nonuniformEXT(w.fftInfo.z)]), uv, lod);
        s.d += disp.xyz * (100.0 * wt[c]);
        dv += der * wt[c];
        if (c < 3u) s.foam = max(s.foam, disp.w * wt[c]);
    }
    s.slope = vec2(dv.x / max(1.0 + dv.z, 0.2), dv.y / max(1.0 + dv.w, 0.2));
    s.jac = (1.0 + dv.z) * (1.0 + dv.w);
    return s;
}

#ifdef WATER_FRAGMENT
// the same per pixel (implicit derivatives: trilinear filtering by the screen footprint) + the slope variance the filtering
// removed (it goes into the specular's roughness instead of sparkling) + the slope of the two longest cascades alone (the
// macro surface: the Fresnel weight follows it, the short waves only bend the reflected ray)
FftSample fftSamplePixel(WaterBlock w, vec2 xz, vec4 wt, out float lostVar, out vec2 macroSlope) {
    FftSample s;
    s.d = vec3(0.0);
    vec4 dv = vec4(0.0);
    s.foam = 0.0;
    lostVar = 0.0;
    macroSlope = vec2(0.0);
    uint n = w.fftInfo.y;
    float octaves = max(log2(float(w.fftInfo.w)) - 2.0, 1.0);
    for (uint c = 0u; c < n; ++c) {
        if (wt[c] <= 0.0) continue;
        vec2 uv = xz / w.fftLength[c];
        vec4 der = texture(sampler2D(t2D[nonuniformEXT(w.fftDeriv[c])], samp[nonuniformEXT(w.fftInfo.z)]), uv);
        vec4 disp = texture(sampler2D(t2D[nonuniformEXT(w.fftDisp[c])], samp[nonuniformEXT(w.fftInfo.z)]), uv);
        float lod = textureQueryLod(sampler2D(t2D[nonuniformEXT(w.fftDeriv[c])], samp[nonuniformEXT(w.fftInfo.z)]), uv).y;
        lostVar += w.fftSlopeVar[c] * wt[c] * wt[c] * clamp(lod / octaves, 0.0, 1.0);
        dv += der * wt[c];
        if (c == 1u) macroSlope = vec2(dv.x / max(1.0 + dv.z, 0.2), dv.y / max(1.0 + dv.w, 0.2));
        if (c < 3u) s.foam = max(s.foam, disp.w * wt[c]);
    }
    s.slope = vec2(dv.x / max(1.0 + dv.z, 0.2), dv.y / max(1.0 + dv.w, 0.2));
    s.jac = (1.0 + dv.z) * (1.0 + dv.w);
    return s;
}
#endif

// phase speed (world units / s) of a wave of period p at depth d (world units), Fenton and McKee (sea_floor.cpp phaseSpeed)
float phaseSpeedUnits(float depth, float p) {
    float l0 = 9.81 * p * p / 6.2831853;
    float d = max(depth / 100.0, 0.01);
    float l = l0 * pow(tanh(pow(6.2831853 * d / l0, 0.75)), 2.0 / 3.0);
    return 100.0 * l / p;
}

// The waves rolling in (docs/research/ocean.md 4.3): a wave train whose phase is the travel time to the shore (the floor
// field's eikonal solution): crests move towards the coast at the local phase speed, get closer together and turn parallel to
// the depth contours in the shallows (shoaling, refraction), grow a little (shoaling), steepen and break where they reach
// ~0.78 x the depth, and run out as broken water in the surf zone. Only where the fronts are coherent and the water is open.
struct ShoreTrain {
    vec3 d;
    vec2 slope;
    float crest;     // 0..1 crest steepness
    float breaking;  // 0..1 how much this wave is breaking here
    float phase;     // radians (foam bands follow it)
    float weight;
};

ShoreTrain shoreTrain(WaterBlock w, SeaFloor f, float t) {
    ShoreTrain r;
    r.d = vec3(0.0);
    r.slope = vec2(0.0);
    r.crest = 0.0;
    r.breaking = 0.0;
    r.phase = 0.0;
    r.weight = 0.0;
    float period = max(w.shoreWave.x, 2.0);
    float coh = length(f.dir);
    float depth = max(f.depth, 0.0);
    float env = (1.0 - smoothstep(2.5 * period, 5.0 * period, f.travel)) * smoothstep(0.15, 0.6, coh) * mix(0.25, 1.0, f.exposure);
    if (env <= 0.0 || f.sd <= 0.0 || w.shoreWave.y <= 0.0) return r;
    vec2 dir = f.dir / max(coh, 1e-4);
    float omega = 6.2831853 / period;
    float th = omega * (t + f.travel);
    float c = phaseSpeedUnits(depth, period);
    float k = omega / max(c, 1.0);  // local wave number (world units)
    // shoaling (Green's law, mild) then the breaking limit; inside the surf zone the broken bore keeps ~0.5 x the depth
    float shoal = clamp(pow(800.0 / max(depth, 20.0), 0.25), 1.0, 1.35);
    float hRaw = w.shoreWave.y * shoal * env;
    float hBreak = w.shoreWave.z * depth;
    r.breaking = smoothstep(0.7, 1.1, hRaw / max(hBreak, 1.0));
    float h = min(hRaw, mix(hBreak, 0.5 * depth, r.breaking));
    float a = 0.5 * h;
    // a steeper, forward-leaning crest as it breaks (second harmonic), horizontal motion towards the shore
    float q = mix(0.25, 0.75, r.breaking);
    float s1 = sin(th), c1 = cos(th);
    float s2 = sin(2.0 * th - 0.9), c2 = cos(2.0 * th - 0.9);
    float hw = a * (s1 + 0.35 * r.breaking * s2);
    float dh = a * (c1 + 0.7 * r.breaking * c2);  // d hw / d th
    r.d = vec3(dir.x * q * a * c1, hw, dir.y * q * a * c1);
    // d th / d xz = omega grad T = -omega dir / c
    r.slope = -dir * (k * dh);
    r.crest = smoothstep(0.4, 1.0, s1) * clamp(a * k * 3.0, 0.0, 1.0);
    r.phase = th;
    r.weight = env;
    return r;
}
