#pragma once

// The FFT ocean's wave spectrum and cascades (docs/research/ocean.md 2). Pure CPU logic, no Vulkan: the GPU evaluates the same
// formulas in shaders/water/ocean_spectrum.glsl (keep both in step); the tests (tests/ocean_tests.cpp) check the
// normalisation of the spectrum and the FFT against a reference DFT with these functions.
//
// Implemented from the papers, not from code:
//  * J. Tessendorf, "Simulating Ocean Water" (SIGGRAPH course notes 2001-2004): the statistical wave field, h(k, t) =
//    h0(k) e^{i w t} + conj(h0(-k)) e^{-i w t}, the choppy displacement D = -i k/|k| h, the derivatives in k-space, the
//    repeat-time quantisation of the dispersion, the Jacobian of the horizontal displacement (folding -> foam);
//  * C. J. Horvath, "Empirical directional wave spectra for computer graphics" (DigiPro 2015): the JONSWAP spectrum with the
//    fetch-limited parameters (Hasselmann et al. 1973), the Hasselmann directional spreading with its normalisation Q(s) and
//    the swell term (s + 16 tanh(w_p / w) xi^2), the conversion S(w, theta) -> S(k) (dw/dk / k);
//  * multiple cascades with disjoint wave-number bands (Ghost of Tsushima / Atlas / Crest talks: each band lives in one tile
//    size, so no frequency is counted twice).
//
// Units: the spectrum in SI (metres, seconds); Risen's world unit is the centimetre (shaders scale by kUnitsPerMetre).

#include <array>
#include <complex>
#include <cstdint>
#include <vector>

namespace rvk::enh::ocean {

inline constexpr double kGravity = 9.81;           // m / s^2
inline constexpr float kUnitsPerMetre = 100.0f;    // Risen world units are centimetres
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr float kRepeatTime = 256.0f;       // s: the dispersion is quantised so the animation loops after this
inline constexpr int kMaxCascades = 4;

// One sea: the local wind sea (JONSWAP, fetch-limited) + a swell from far away (JONSWAP shape, narrow spreading)
struct SeaState {
    float windSpeed = 7.0f;       // U10, m / s
    float fetch = 40000.0f;       // m
    float windAngle = 0.0f;       // rad, direction the wind blows towards (world xz: x = cos, z = sin)
    float spreadXi = 0.2f;        // Horvath's swell parameter of the wind sea (narrower spreading)
    float swellHeight = 0.5f;     // significant wave height of the swell, m
    float swellPeriod = 10.0f;    // its peak period, s
    float swellAngle = 0.4f;      // rad
    float swellSpread = 40.0f;    // its Hasselmann exponent s (large = narrow)
    float choppiness = 0.85f;     // horizontal displacement scale (lambda)
    float scale = 1.0f;           // the player's wave scale (amplitude)
};

// sea states by level: 0 calm, 1 moderate, 2 rough, 3 storm (levels in between are interpolated); windAngle / scale are not
// set (the caller's)
SeaState seaStateForLevel(float level);

// JONSWAP parameters of the wind sea
struct Jonswap {
    double alpha = 0.0;   // Phillips constant
    double omegaP = 0.0;  // peak angular frequency
    double gamma = 3.3;   // peak enhancement
};
Jonswap jonswapFor(double windSpeed, double fetch);
// S(omega) [m^2 s] (no directional part)
double jonswap(double omega, const Jonswap& j);
// the significant wave height of a JONSWAP spectrum: 4 sqrt(m0) (numerical integral)
double significantHeight(const Jonswap& j);
// Hasselmann's spreading exponent s(omega) of the wind sea (Horvath 2015 eq. 39 + the swell term)
double hasselmannS(double omega, double omegaP, double windSpeed, double xi);
// D(theta) = Q(s) |cos(theta / 2)|^{2 s}, normalised over theta in [-pi, pi]
double hasselmannD(double theta, double s);
// ln Gamma(x) for x > 0 (the series the shader uses)
double lgammaApprox(double x);

// Everything the shaders need for a sea state (also the CPU reference)
struct SpectrumParams {
    float windAlpha = 0.0f, windOmegaP = 0.0f, windGamma = 3.3f, windSpeed = 0.0f;
    float windAngle = 0.0f, spreadXi = 0.0f;
    float swellAlpha = 0.0f, swellOmegaP = 0.0f, swellAngle = 0.0f, swellSpread = 0.0f;
    float shortWaveCut = 0.01f;  // m: exp(-(k l)^2) damps the smallest ripples
};
SpectrumParams spectrumParams(const SeaState& s);
// the directional spectrum S(k) [m^4] at the wave vector (kx, kz) [rad / m] (deep water: w = sqrt(g k)): integrating it over
// the k-plane gives the height variance
double spectrumK(double kx, double kz, const SpectrumParams& p);

// The cascades: tile sizes and wave-number bands (each wave number lives in exactly one cascade)
struct Cascade {
    float length = 0.0f;   // tile size, m
    float kLow = 0.0f;     // |k| band [kLow, kHigh) in rad / m
    float kHigh = 0.0f;
    float heightVar = 0.0f;  // height variance of the band (m^2) for a sea state (cascadeStats)
    float slopeVar = 0.0f;   // slope variance of the band
};
struct Cascades {
    int size = 256;  // N x N per cascade
    int count = 4;
    std::array<Cascade, kMaxCascades> c{};
};
// quality 0 low (3 x 128), 1 medium (4 x 256), 2 high (4 x 512)
Cascades cascadesFor(int quality);
// fills heightVar / slopeVar of each cascade for a sea state (sums of S(k) dk^2 over the cascade's grid)
void cascadeStats(Cascades& cs, const SpectrumParams& p);

// ---- the GPU's random numbers and h0 (mirrors of ocean_spectrum.glsl) -----------------------------------------------------

std::uint32_t hashU32(std::uint32_t x);
// two independent standard normal numbers for texel (x, z) of cascade c
std::array<float, 2> gaussianPair(std::uint32_t x, std::uint32_t z, std::uint32_t c);
// wave vector of texel (x, z) of an N x N tile of size L: k = 2 pi (i - N / 2) / L
std::array<float, 2> waveVector(int x, int z, int n, float length);
// h0 of texel (x, z) (0 outside the cascade's band): 0.5 (xi_r + i xi_i) sqrt(S(k) dk^2) (so the real field's variance is
// sum S(k) dk^2, see ocean_spectrum.cpp)
std::complex<float> h0(int x, int z, int n, const Cascade& cascade, std::uint32_t cascadeIndex, const SpectrumParams& p);
// the quantised dispersion (deep water, loops after kRepeatTime)
float dispersion(float k);

// ---- FFT (the algorithm of ocean_fft.comp: in-place radix-2, bit-reversed load, inverse sign) -----------------------------

// in place, one line of n (power of two) complex values: out[x] = sum_k in[k] e^{+2 pi i k x / n}
void ifft1D(std::complex<float>* data, int n, int stride = 1);
// rows then columns of an n x n grid (row-major)
void ifft2D(std::vector<std::complex<float>>& data, int n);
// the reference: the direct sum (O(n^4))
std::vector<std::complex<double>> idft2D(const std::vector<std::complex<float>>& data, int n);

// the height field of a cascade at time t on the CPU (h(k, t) -> IFFT -> (-1)^(x + z) sign), metres
std::vector<float> heightField(const Cascades& cs, int cascade, const SpectrumParams& p, float t);

}  // namespace rvk::enh::ocean
