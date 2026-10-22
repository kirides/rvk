// The sea floor field and the waves rolling in towards the shore (docs/research/ocean.md 4): plain functions of the field's
// textures and the train's parameters (no WaterBlock), shared by the water draws (ocean_surface.glsl) and the FFT ocean's
// foam pass (ocean_foam.comp). Needs rvk_common.glsl (rvkSample2DLod) and water_noise.glsl.

#ifndef OCEAN_SHORE_GLSL
#define OCEAN_SHORE_GLSL

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

// floorMap: xy the field's corner (world xz), zw 1 / its extent; smp: linear clamp
SeaFloor seaFloorAt(uint texA, uint texB, uint smp, vec4 floorMap, vec2 xz) {
    SeaFloor f;
    f.valid = true;
    vec2 uv = (xz - floorMap.xy) * floorMap.zw;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        f.sd = 30000.0;
        f.depth = 3000.0;
        f.dir = vec2(0.0);
        f.travel = 60.0;
        f.exposure = 1.0;
        f.overlay = 30000.0;
        return f;
    }
    vec4 a = rvkSample2DLod(texA, smp, uv, 0.0);
    vec4 b = rvkSample2DLod(texB, smp, uv, 0.0);
    f.sd = a.x;
    f.depth = a.y;
    f.dir = a.zw;
    f.travel = b.x;
    f.exposure = b.y;
    f.overlay = b.z;
    return f;
}

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

// shoreWave: x period (s), y amplitude (world units), z breaker index
ShoreTrain shoreTrainAt(vec4 shoreWave, SeaFloor f, float t) {
    ShoreTrain r;
    r.d = vec3(0.0);
    r.slope = vec2(0.0);
    r.crest = 0.0;
    r.breaking = 0.0;
    r.phase = 0.0;
    r.weight = 0.0;
    float period = max(shoreWave.x, 2.0);
    float coh = length(f.dir);
    float depth = max(f.depth, 0.0);
    float env = (1.0 - smoothstep(2.5 * period, 5.0 * period, f.travel)) * smoothstep(0.15, 0.6, coh) * mix(0.25, 1.0, f.exposure);
    if (env <= 0.0 || f.sd <= 0.0 || shoreWave.y <= 0.0) return r;
    vec2 dir = f.dir / max(coh, 1e-4);
    float omega = 6.2831853 / period;
    float th = omega * (t + f.travel);
    float c = phaseSpeedUnits(depth, period);
    float k = omega / max(c, 1.0);  // local wave number (world units)
    // shoaling (Green's law, mild) then the breaking limit; inside the surf zone the broken bore keeps ~0.5 x the depth
    float shoal = clamp(pow(800.0 / max(depth, 20.0), 0.25), 1.0, 1.35);
    float hRaw = shoreWave.y * shoal * env;
    float hBreak = shoreWave.z * depth;
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

// The surf of the train at a place (docs/research/ocean.md 4.4): the white front where a crest breaks (only on segments of the
// crest at a time: the break peels along the crest), the bores rolling on across the surf zone, the trail behind them.
struct Surf {
    float front;     // 0..1 on the breaking crest / the bore's face
    float trail;     // 1 just behind the crest, decaying until the next one
    float breakW;    // 0..1 the crest breaks here (x the segment that breaks now)
    float surfZone;  // 0..1 inside the surf zone (between the break and the beach), x the train's weight
    float patchy;    // 0..1 slow patches of the broken water
};

Surf surfAt(vec4 shoreWave, SeaFloor f, ShoreTrain st, vec2 xz, float t) {
    Surf s;
    float saw = fract((st.phase - 1.5707963) / 6.2831853);  // 0 just behind the crest, growing until the next one
    s.front = smoothstep(0.86, 0.99, sin(st.phase + 0.3));
    s.trail = exp(-saw * 4.5);
    // the breaking segments move along the crest (peeling): noise advected along the crest by a flow map (two phases
    // cross-faded, so the domain never shears apart)
    float coh = length(f.dir);
    vec2 along = coh > 1e-4 ? vec2(-f.dir.y, f.dir.x) / coh : vec2(0.0);
    float ph0 = fract(t / 8.0), ph1 = fract(t / 8.0 + 0.5);
    float n0 = waterNoise(xz / 900.0 + along * (ph0 * 2.2) + vec2(0.0, 3.7));
    float n1 = waterNoise(xz / 900.0 + along * (ph1 * 2.2) + vec2(5.1, 0.0));
    float seg = mix(n0, n1, abs(ph0 * 2.0 - 1.0));
    float segment = smoothstep(0.2, 0.7, 0.5 + 0.7 * seg);
    s.breakW = st.breaking * st.weight * mix(0.3, 1.0, segment);
    // the surf zone: where the incoming waves break (depth ~ height / breaker index) and between there and the beach
    float breakDepth = max(shoreWave.y * st.weight / max(shoreWave.z, 0.1), 1.0);
    s.surfZone = max(st.breaking, 1.0 - smoothstep(0.6 * breakDepth, 1.3 * breakDepth, max(f.depth, 0.0))) * st.weight;
    s.patchy = smoothstep(0.3, 0.75, 0.5 + 0.5 * waterNoise(xz / 650.0 + vec2(t * 0.01, 0.0)));
    return s;
}

#endif
