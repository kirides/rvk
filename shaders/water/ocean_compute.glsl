// The FFT ocean's compute passes (src/enh/ocean.cpp, docs/research/ocean.md 2): shared push block, image bindings, the
// spectrum (mirror of src/enh/ocean_spectrum.cpp: keep both in step; rvk_tests checks the C++ side) and complex helpers.

#define RVK_PASS_SHADER
#extension GL_EXT_buffer_reference_uvec2 : require
#include "rvk_abi.glsl"

// one push block for all ocean passes (mirror: enh::OceanPush)
layout(push_constant) uniform OceanPush {
    RVK_PASS_PUSH_HEADER
    vec4 tile;   // x tile size (m), y band kLow, z band kHigh (rad / m), w N
    vec4 wind;   // x alpha, y omega_p, z gamma, w U10 (m / s)
    vec4 wind2;  // x wind angle (rad), y spreading xi, z short-wave cut (m), w cascade index
    vec4 swell;  // x alpha, y omega_p, z angle (rad), w spreading exponent s
    vec4 time;   // x t (s, mod the repeat time), y dt (s), z choppiness, w FFT direction (0 rows, 1 columns)
    vec4 foam;   // x decay (1 / s), y Jacobian threshold, z coverage gain, w repeat time (s)
} pc;

// set 1 (push descriptors): h0 (rgba32f: h0(k), conj(h0(-k))), the two FFT work images (rgba32f: 4 complex fields), the
// displacement (rgba16f: dx, dy, dz in m, foam) and derivatives (rgba16f: d dy/dx, d dy/dz, d dx/dx, d dz/dz)
#define OCEAN_H0 layout(set = 1, binding = 0, rgba32f) uniform image2D
#define OCEAN_WORK_A layout(set = 1, binding = 1, rgba32f) uniform image2D
#define OCEAN_WORK_B layout(set = 1, binding = 2, rgba32f) uniform image2D
#define OCEAN_DISP layout(set = 1, binding = 3, rgba16f) uniform image2D
#define OCEAN_DERIV layout(set = 1, binding = 4, rgba16f) uniform image2D

const float OCEAN_PI = 3.14159265359;
const float OCEAN_G = 9.81;

vec2 cmul(vec2 a, vec2 b) { return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }
vec2 cconj(vec2 a) { return vec2(a.x, -a.y); }
vec2 ci(vec2 a) { return vec2(-a.y, a.x); }  // i * a

// wave vector of texel p of the N x N tile: k = 2 pi (p - N / 2) / L
vec2 oceanWaveVector(ivec2 p) {
    int n = int(pc.tile.w);
    return vec2(p - ivec2(n / 2)) * (2.0 * OCEAN_PI / pc.tile.x);
}

// the dispersion quantised to the repeat time (the animation loops)
float oceanDispersion(float k) {
    float w0 = 2.0 * OCEAN_PI / pc.foam.w;
    return floor(sqrt(OCEAN_G * k) / w0) * w0;
}

// ---- spectrum (ocean_spectrum.cpp) ----------------------------------------------------------------------------------------

float oceanLgamma(float x) {
    float shift = 0.0;
    for (int i = 0; i < 8 && x < 7.0; ++i) {
        shift += log(x);
        x += 1.0;
    }
    float inv = 1.0 / x;
    float inv2 = inv * inv;
    float series = inv * (1.0 / 12.0 - inv2 * (1.0 / 360.0 - inv2 * (1.0 / 1260.0 - inv2 / 1680.0)));
    return (x - 0.5) * log(x) - x + 0.5 * log(2.0 * OCEAN_PI) + series - shift;
}

float oceanJonswap(float omega, float alpha, float omegaP, float gamma) {
    if (omega <= 1e-6 || alpha <= 0.0) return 0.0;
    float sigma = omega <= omegaP ? 0.07 : 0.09;
    float d = omega - omegaP;
    float r = exp(-d * d / (2.0 * sigma * sigma * omegaP * omegaP));
    float ratio = omegaP / omega;
    float r2 = ratio * ratio;
    // alpha g^2 / w^5 in two steps (w^5 underflows nothing here, but keeps the range sane at w ~ 10)
    return alpha * OCEAN_G * OCEAN_G / (omega * omega * omega * omega * omega) * exp(-1.25 * r2 * r2) * pow(gamma, r);
}

float oceanHasselmannS(float omega, float omegaP, float windSpeed, float xi) {
    float r = omega / max(omegaP, 1e-6);
    float s;
    if (r <= 1.0) s = 6.97 * pow(r, 4.06);
    else s = 9.77 * pow(r, -2.33 - 1.45 * (windSpeed * omegaP / OCEAN_G - 1.17));
    s += 16.0 * tanh(min(1.0 / max(r, 1e-3), 20.0)) * xi * xi;
    return clamp(s, 0.0, 200.0);
}

float oceanHasselmannD(float theta, float s) {
    float lnQ = (2.0 * s - 1.0) * log(2.0) - log(OCEAN_PI) + 2.0 * oceanLgamma(s + 1.0) - oceanLgamma(2.0 * s + 1.0);
    float c = abs(cos(0.5 * theta));
    if (c <= 0.0) return 0.0;
    return exp(lnQ + 2.0 * s * log(c));
}

float oceanWrap(float a) { return a - 2.0 * OCEAN_PI * floor((a + OCEAN_PI) / (2.0 * OCEAN_PI)); }

// S(k) [m^4] at the wave vector k (rad / m)
float oceanSpectrumK(vec2 kv) {
    float k = length(kv);
    if (k < 1e-6) return 0.0;
    float omega = sqrt(OCEAN_G * k);
    float dOmegaDk = OCEAN_G / (2.0 * omega);
    float theta = atan(kv.y, kv.x);
    float s = 0.0;
    if (pc.wind.x > 0.0) {
        float spread = oceanHasselmannS(omega, pc.wind.y, pc.wind.w, pc.wind2.y);
        s += oceanJonswap(omega, pc.wind.x, pc.wind.y, pc.wind.z) * oceanHasselmannD(oceanWrap(theta - pc.wind2.x), spread);
    }
    if (pc.swell.x > 0.0) {
        s += oceanJonswap(omega, pc.swell.x, pc.swell.y, 3.3) * oceanHasselmannD(oceanWrap(theta - pc.swell.z), pc.swell.w);
    }
    float cut = pc.wind2.z * k;
    return s * dOmegaDk / k * exp(-cut * cut);
}

// ---- random numbers (ocean_spectrum.cpp hashU32 / gaussianPair) -----------------------------------------------------------

uint oceanHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

vec2 oceanGaussianPair(uint x, uint z, uint c) {
    uint h1 = oceanHash(x + oceanHash(z + oceanHash(c + 0x9e3779b9u)));
    uint h2 = oceanHash(h1 ^ 0x68bc21ebu);
    float u1 = max(float(h1 >> 8) * (1.0 / 16777216.0), 1e-7);
    float u2 = float(h2 >> 8) * (1.0 / 16777216.0);
    float r = sqrt(-2.0 * log(u1));
    float a = 6.28318530718 * u2;
    return vec2(r * cos(a), r * sin(a));
}

// h0 of texel p (0 outside the cascade's band)
vec2 oceanH0(ivec2 p) {
    vec2 kv = oceanWaveVector(p);
    float k = length(kv);
    if (k < pc.tile.y || k >= pc.tile.z || k < 1e-6) return vec2(0.0);
    float dk = 2.0 * OCEAN_PI / pc.tile.x;
    float amp = 0.5 * sqrt(oceanSpectrumK(kv)) * dk;
    return oceanGaussianPair(uint(p.x), uint(p.y), uint(pc.wind2.w)) * amp;
}
