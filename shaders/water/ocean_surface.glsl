// The FFT ocean's surface (src/enh/ocean.h, src/enh/sea_floor.h, docs/research/ocean.md): sampling the cascades, the sea
// floor field, the attenuation towards the shore and inside the harbour, the waves rolling in. Included by water_common.glsl.

bool fftMode(WaterBlock w) { return w.fftInfo.x == 1u && w.fftInfo.y > 0u; }

// the sea floor field at xz (ocean_shore.glsl); before the first bake the M7 shore field (distance to the coast overlay)
// stands in
SeaFloor seaFloor(WaterBlock w, vec2 xz) {
    if (w.floorTex.z != 0u) return seaFloorAt(w.floorTex.x, w.floorTex.y, w.smpOurs.x, w.floorMap, xz);
    SeaFloor f;
    f.valid = false;
    vec4 sf = shoreField(w, xz);
    f.sd = sf.x;
    f.depth = 60.0 + sf.x * 0.06;
    f.dir = sf.zw;
    f.travel = sf.x / 600.0;
    f.exposure = 1.0;
    f.overlay = sf.x;
    return f;
}

// how much of each cascade survives at a place: waves feel the bottom when the water is shallower than about half their
// length (the long ones first), they need open water to exist (no swell right at the waterline), and harbour basins / coves
// are sheltered from the long waves (exposure) while the short wind chop still forms inside
vec4 cascadeWeights(WaterBlock w, SeaFloor f) {
    // the share of each cascade a fully sheltered basin loses: the long swell most of it (what enters through the mouth
    // spreads into the basin and decays: the exposure field's diffraction), the short wind chop little (it forms inside)
    const vec4 SHELTER = vec4(0.72, 0.6, 0.3, 0.1);
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

// around a swimming or wading player the waves are capped (the engine's physics plane stays flat: his swim line is the
// engine's sea level). Keyed to the player's position (eCWeatherAdmin's, w.player) when known: full while he swims (his
// origin ~250 units below the sea level), partly while he wades, not while he stands on the beach (the surf rolls in in
// front of him). Without it: around the camera while the camera is down at the water.
float nearCameraCap(WaterBlock w, vec2 xz, vec3 cam) {
    float capFrac = clamp(w.misc.x / max(0.5 * w.seaInfo.x, 1.0), 0.0, 1.0);
    vec3 c = cam;
    float low = 1.0 - smoothstep(200.0, 700.0, cam.y - w.ocean.x);
    if (w.player.w > 0.5) {
        c = w.player.xyz;
        low = 1.0 - smoothstep(-150.0, -40.0, c.y - w.ocean.x);
    }
    float d = length(xz - c.xz);
    return mix(1.0, mix(capFrac, 1.0, smoothstep(700.0, 3000.0, d)), low);
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
// removed (it goes into the specular's roughness instead of sparkling) + the slopes of the macro surface: xy the longest
// cascade alone (the swell), zw the two longest (the Fresnel weight follows them, the short waves only bend the reflected ray)
FftSample fftSamplePixel(WaterBlock w, vec2 xz, vec4 wt, out float lostVar, out vec4 macroSlope) {
    FftSample s;
    s.d = vec3(0.0);
    vec4 dv = vec4(0.0);
    s.foam = 0.0;
    lostVar = 0.0;
    macroSlope = vec4(0.0);
    uint n = w.fftInfo.y;
    float octaves = max(log2(float(w.fftInfo.w)) - 2.0, 1.0);
    for (uint c = 0u; c < n; ++c) {
        if (wt[c] > 0.0) {
            vec2 uv = xz / w.fftLength[c];
            vec4 der = texture(sampler2D(t2D[nonuniformEXT(w.fftDeriv[c])], samp[nonuniformEXT(w.fftInfo.z)]), uv);
            vec4 disp = texture(sampler2D(t2D[nonuniformEXT(w.fftDisp[c])], samp[nonuniformEXT(w.fftInfo.z)]), uv);
            float lod = textureQueryLod(sampler2D(t2D[nonuniformEXT(w.fftDeriv[c])], samp[nonuniformEXT(w.fftInfo.z)]), uv).y;
            lostVar += w.fftSlopeVar[c] * wt[c] * wt[c] * clamp(lod / octaves, 0.0, 1.0);
            dv += der * wt[c];
            if (c < 3u) s.foam = max(s.foam, disp.w * wt[c]);
        }
        if (c == 0u) macroSlope.xy = vec2(dv.x / max(1.0 + dv.z, 0.2), dv.y / max(1.0 + dv.w, 0.2));
        if (c == 1u) macroSlope.zw = vec2(dv.x / max(1.0 + dv.z, 0.2), dv.y / max(1.0 + dv.w, 0.2));
    }
    s.slope = vec2(dv.x / max(1.0 + dv.z, 0.2), dv.y / max(1.0 + dv.w, 0.2));
    s.jac = (1.0 + dv.z) * (1.0 + dv.w);
    return s;
}
#endif

ShoreTrain shoreTrain(WaterBlock w, SeaFloor f, float t) { return shoreTrainAt(w.shoreWave, f, t); }
