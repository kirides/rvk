#include "enh/ocean_spectrum.h"

#include <algorithm>
#include <cmath>

namespace rvk::enh::ocean {

namespace {

float lerpf(float a, float b, float t) { return a + (b - a) * t; }

}  // namespace

SeaState seaStateForLevel(float level) {
    // calm / moderate / rough / storm: the wind sea's U10 and fetch (JONSWAP Hs of the wind sea 0.5 / 1.3 / 2.6 / 4.3 m), the
    // swell's height and period, the choppiness. Risen's island is small and its harbour sheltered: the shore field
    // attenuates all of it towards the coast (ocean.md 4)
    struct Level {
        float wind, fetch, swellH, swellT, chop;
    };
    static constexpr Level kLevels[4] = {
        {5.0f, 25000.0f, 0.80f, 10.0f, 0.75f},
        {8.0f, 60000.0f, 1.10f, 11.0f, 0.90f},
        {11.5f, 110000.0f, 1.50f, 12.0f, 1.10f},
        {15.0f, 180000.0f, 2.20f, 13.0f, 1.25f},
    };
    const float l = std::clamp(level, 0.0f, 3.0f);
    const int i = std::min(static_cast<int>(l), 2);
    const float t = l - static_cast<float>(i);
    const Level& a = kLevels[i];
    const Level& b = kLevels[i + 1];
    SeaState s;
    s.windSpeed = lerpf(a.wind, b.wind, t);
    // fetch interpolated in log space (it spans an order of magnitude)
    s.fetch = std::exp(lerpf(std::log(a.fetch), std::log(b.fetch), t));
    s.swellHeight = lerpf(a.swellH, b.swellH, t);
    s.swellPeriod = lerpf(a.swellT, b.swellT, t);
    s.choppiness = lerpf(a.chop, b.chop, t);
    return s;
}

// Hasselmann et al. 1973 (JONSWAP), as in Horvath 2015 eq. 28-30: alpha = 0.076 (U^2 / (F g))^0.22,
// w_p = 22 (g^2 / (U F))^(1/3)
Jonswap jonswapFor(double windSpeed, double fetch) {
    const double u = std::max(windSpeed, 0.1);
    const double f = std::max(fetch, 100.0);
    Jonswap j;
    j.alpha = 0.076 * std::pow(u * u / (f * kGravity), 0.22);
    j.omegaP = 22.0 * std::cbrt(kGravity * kGravity / (u * f));
    j.gamma = 3.3;
    return j;
}

double jonswap(double omega, const Jonswap& j) {
    if (omega <= 1e-6 || j.alpha <= 0.0) return 0.0;
    const double sigma = omega <= j.omegaP ? 0.07 : 0.09;
    const double d = omega - j.omegaP;
    const double r = std::exp(-d * d / (2.0 * sigma * sigma * j.omegaP * j.omegaP));
    const double ratio = j.omegaP / omega;
    return j.alpha * kGravity * kGravity / std::pow(omega, 5.0) * std::exp(-1.25 * ratio * ratio * ratio * ratio) * std::pow(j.gamma, r);
}

double significantHeight(const Jonswap& j) {
    // m0 = integral S(w) dw; the spectrum lives around w_p (it vanishes below 0.4 w_p and falls as w^-5 above)
    double m0 = 0.0;
    const double w0 = 0.2 * j.omegaP, w1 = 20.0 * j.omegaP;
    const int steps = 20000;
    const double dw = (w1 - w0) / steps;
    for (int i = 0; i < steps; ++i) m0 += jonswap(w0 + (i + 0.5) * dw, j) * dw;
    return 4.0 * std::sqrt(m0);
}

double hasselmannS(double omega, double omegaP, double windSpeed, double xi) {
    const double r = omega / std::max(omegaP, 1e-6);
    double s = 0.0;
    if (r <= 1.0) {
        s = 6.97 * std::pow(r, 4.06);
    } else {
        const double mu = -2.33 - 1.45 * (windSpeed * omegaP / kGravity - 1.17);
        s = 9.77 * std::pow(r, mu);
    }
    // Horvath 2015 eq. 45: swell (long waves from far away) narrows the spreading of the longer waves
    s += 16.0 * std::tanh(std::min(1.0 / std::max(r, 1e-3), 20.0)) * xi * xi;
    return std::clamp(s, 0.0, 200.0);
}

double lgammaApprox(double x) {
    // shift into the range where Stirling's series is exact to float precision, then ln Gamma(x) = ln Gamma(x + n) - ln(x ... x+n-1)
    double shift = 0.0;
    while (x < 7.0) {
        shift += std::log(x);
        x += 1.0;
    }
    const double inv = 1.0 / x;
    const double inv2 = inv * inv;
    const double series = inv * (1.0 / 12.0 - inv2 * (1.0 / 360.0 - inv2 * (1.0 / 1260.0 - inv2 / 1680.0)));
    return (x - 0.5) * std::log(x) - x + 0.5 * std::log(2.0 * kPi) + series - shift;
}

double hasselmannD(double theta, double s) {
    // Q(s) = 2^(2s - 1) / pi * Gamma(s + 1)^2 / Gamma(2s + 1)
    const double lnQ = (2.0 * s - 1.0) * std::log(2.0) - std::log(kPi) + 2.0 * lgammaApprox(s + 1.0) - lgammaApprox(2.0 * s + 1.0);
    const double c = std::abs(std::cos(0.5 * theta));
    if (c <= 0.0) return 0.0;
    return std::exp(lnQ + 2.0 * s * std::log(c));
}

SpectrumParams spectrumParams(const SeaState& s) {
    SpectrumParams p;
    const Jonswap w = jonswapFor(s.windSpeed, s.fetch);
    const double scale2 = static_cast<double>(s.scale) * s.scale;  // amplitude scale -> variance
    p.windAlpha = static_cast<float>(w.alpha * scale2);
    p.windOmegaP = static_cast<float>(w.omegaP);
    p.windGamma = static_cast<float>(w.gamma);
    p.windSpeed = s.windSpeed;
    p.windAngle = s.windAngle;
    p.spreadXi = s.spreadXi;
    // the swell: a JONSWAP shape at its own peak, alpha chosen so its significant height is swellHeight
    Jonswap sw;
    sw.omegaP = 2.0 * kPi / std::max(s.swellPeriod, 1.0f);
    sw.alpha = 1.0;
    sw.gamma = 3.3;
    const double hs1 = significantHeight(sw);
    const double ratio = hs1 > 0.0 ? s.swellHeight / hs1 : 0.0;
    p.swellAlpha = static_cast<float>(ratio * ratio * scale2);
    p.swellOmegaP = static_cast<float>(sw.omegaP);
    p.swellAngle = s.swellAngle;
    p.swellSpread = s.swellSpread;
    return p;
}

namespace {

double wrapAngle(double a) {
    while (a > kPi) a -= 2.0 * kPi;
    while (a < -kPi) a += 2.0 * kPi;
    return a;
}

}  // namespace

double spectrumK(double kx, double kz, const SpectrumParams& p) {
    const double k = std::sqrt(kx * kx + kz * kz);
    if (k < 1e-6) return 0.0;
    const double omega = std::sqrt(kGravity * k);
    const double dOmegaDk = kGravity / (2.0 * omega);
    const double theta = std::atan2(kz, kx);
    double s = 0.0;
    if (p.windAlpha > 0.0f) {
        const Jonswap j{p.windAlpha, p.windOmegaP, p.windGamma};
        const double spread = hasselmannS(omega, p.windOmegaP, p.windSpeed, p.spreadXi);
        s += jonswap(omega, j) * hasselmannD(wrapAngle(theta - p.windAngle), spread);
    }
    if (p.swellAlpha > 0.0f) {
        const Jonswap j{p.swellAlpha, p.swellOmegaP, 3.3};
        s += jonswap(omega, j) * hasselmannD(wrapAngle(theta - p.swellAngle), p.swellSpread);
    }
    // S(w, theta) dw dtheta = S(k) k dk dtheta: S(k) = S(w, theta) dw/dk / k; the smallest ripples damped
    const double cut = static_cast<double>(p.shortWaveCut) * k;
    return s * dOmegaDk / k * std::exp(-cut * cut);
}

Cascades cascadesFor(int quality) {
    Cascades cs;
    // tile sizes in metres: non-commensurate (no common repeat), each band = [4 * 2 pi / L_next, ...)
    static constexpr float kLow3[3] = {250.0f, 50.0f, 10.0f};
    static constexpr float kFour[4] = {310.0f, 87.0f, 23.0f, 6.0f};
    if (quality <= 0) {
        cs.size = 128;
        cs.count = 3;
        for (int i = 0; i < 3; ++i) cs.c[static_cast<std::size_t>(i)].length = kLow3[i];
    } else {
        cs.size = quality >= 2 ? 512 : 256;
        cs.count = 4;
        for (int i = 0; i < 4; ++i) cs.c[static_cast<std::size_t>(i)].length = kFour[i];
    }
    for (int i = 0; i < cs.count; ++i) {
        Cascade& c = cs.c[static_cast<std::size_t>(i)];
        c.kLow = i == 0 ? 0.0f : cs.c[static_cast<std::size_t>(i - 1)].kHigh;
        c.kHigh = i + 1 < cs.count ? 4.0f * 2.0f * static_cast<float>(kPi) / cs.c[static_cast<std::size_t>(i + 1)].length : 1e9f;
    }
    return cs;
}

void cascadeStats(Cascades& cs, const SpectrumParams& p) {
    for (int c = 0; c < cs.count; ++c) {
        Cascade& cc = cs.c[static_cast<std::size_t>(c)];
        const double dk = 2.0 * kPi / cc.length;
        double hv = 0.0, sv = 0.0;
        for (int z = 0; z < cs.size; ++z) {
            for (int x = 0; x < cs.size; ++x) {
                const auto kv = waveVector(x, z, cs.size, cc.length);
                const double k = std::sqrt(double(kv[0]) * kv[0] + double(kv[1]) * kv[1]);
                if (k < cc.kLow || k >= cc.kHigh) continue;
                const double e = spectrumK(kv[0], kv[1], p) * dk * dk;
                hv += e;
                sv += e * k * k;
            }
        }
        cc.heightVar = static_cast<float>(hv);
        cc.slopeVar = static_cast<float>(sv);
    }
}

// ---- random numbers, h0 ---------------------------------------------------------------------------------------------------

std::uint32_t hashU32(std::uint32_t v) {
    // PCG output permutation (O'Neill), as in "Hash Functions for GPU Rendering" (Jarzynski, Olano 2020)
    const std::uint32_t state = v * 747796405u + 2891336453u;
    const std::uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

std::array<float, 2> gaussianPair(std::uint32_t x, std::uint32_t z, std::uint32_t c) {
    const std::uint32_t h1 = hashU32(x + hashU32(z + hashU32(c + 0x9e3779b9u)));
    const std::uint32_t h2 = hashU32(h1 ^ 0x68bc21ebu);
    const float u1 = std::max(static_cast<float>(h1 >> 8) * (1.0f / 16777216.0f), 1e-7f);
    const float u2 = static_cast<float>(h2 >> 8) * (1.0f / 16777216.0f);
    const float r = std::sqrt(-2.0f * std::log(u1));
    const float a = 6.28318530718f * u2;
    return {r * std::cos(a), r * std::sin(a)};
}

std::array<float, 2> waveVector(int x, int z, int n, float length) {
    const float s = 6.28318530718f / length;
    return {static_cast<float>(x - n / 2) * s, static_cast<float>(z - n / 2) * s};
}

std::complex<float> h0(int x, int z, int n, const Cascade& cascade, std::uint32_t cascadeIndex, const SpectrumParams& p) {
    const auto kv = waveVector(x, z, n, cascade.length);
    const float k = std::sqrt(kv[0] * kv[0] + kv[1] * kv[1]);
    if (k < cascade.kLow || k >= cascade.kHigh || k < 1e-6f) return {0.0f, 0.0f};
    const float dk = 6.28318530718f / cascade.length;
    // E|h0|^2 = S dk^2 / 2 per wave vector: h(k) pairs h0(k) with conj(h0(-k)), so the real field's variance is
    // sum over k of (|h0(k)|^2 + |h0(-k)|^2) = sum S dk^2
    const float amp = 0.5f * std::sqrt(static_cast<float>(spectrumK(kv[0], kv[1], p)) * dk * dk);
    const auto g = gaussianPair(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z), cascadeIndex);
    return {g[0] * amp, g[1] * amp};
}

float dispersion(float k) {
    const float w0 = 6.28318530718f / kRepeatTime;
    return std::floor(std::sqrt(static_cast<float>(kGravity) * k) / w0) * w0;
}

// ---- FFT ------------------------------------------------------------------------------------------------------------------

namespace {

std::uint32_t reverseBits(std::uint32_t v, int bits) {
    std::uint32_t r = 0;
    for (int i = 0; i < bits; ++i) r |= ((v >> i) & 1u) << (bits - 1 - i);
    return r;
}

}  // namespace

void ifft1D(std::complex<float>* data, int n, int stride) {
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    std::vector<std::complex<float>> buf(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) buf[reverseBits(static_cast<std::uint32_t>(i), bits)] = data[static_cast<std::ptrdiff_t>(i) * stride];
    for (int half = 1; half < n; half *= 2) {
        // butterfly j: the shader's thread j of this stage
        for (int j = 0; j < n / 2; ++j) {
            const int group = j / half, pos = j % half;
            const int i0 = group * 2 * half + pos, i1 = i0 + half;
            const float ang = 3.14159265359f * static_cast<float>(pos) / static_cast<float>(half);
            const std::complex<float> w(std::cos(ang), std::sin(ang));
            const std::complex<float> a = buf[static_cast<std::size_t>(i0)];
            const std::complex<float> b = buf[static_cast<std::size_t>(i1)] * w;
            buf[static_cast<std::size_t>(i0)] = a + b;
            buf[static_cast<std::size_t>(i1)] = a - b;
        }
    }
    for (int i = 0; i < n; ++i) data[static_cast<std::ptrdiff_t>(i) * stride] = buf[static_cast<std::size_t>(i)];
}

void ifft2D(std::vector<std::complex<float>>& data, int n) {
    for (int z = 0; z < n; ++z) ifft1D(data.data() + static_cast<std::ptrdiff_t>(z) * n, n, 1);
    for (int x = 0; x < n; ++x) ifft1D(data.data() + x, n, n);
}

std::vector<std::complex<double>> idft2D(const std::vector<std::complex<float>>& data, int n) {
    std::vector<std::complex<double>> out(static_cast<std::size_t>(n) * n);
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            std::complex<double> sum = 0.0;
            for (int kz = 0; kz < n; ++kz) {
                for (int kx = 0; kx < n; ++kx) {
                    const double ang = 2.0 * kPi * (static_cast<double>(kx) * x + static_cast<double>(kz) * z) / n;
                    sum += std::complex<double>(data[static_cast<std::size_t>(kz) * n + kx]) * std::complex<double>(std::cos(ang), std::sin(ang));
                }
            }
            out[static_cast<std::size_t>(z) * n + x] = sum;
        }
    }
    return out;
}

std::vector<float> heightField(const Cascades& cs, int cascade, const SpectrumParams& p, float t) {
    const int n = cs.size;
    const Cascade& c = cs.c[static_cast<std::size_t>(cascade)];
    std::vector<std::complex<float>> hk(static_cast<std::size_t>(n) * n);
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const auto kv = waveVector(x, z, n, c.length);
            const float k = std::sqrt(kv[0] * kv[0] + kv[1] * kv[1]);
            const float w = dispersion(k) * t;
            const std::complex<float> e(std::cos(w), std::sin(w));
            // -k: texel (n - x, n - z) mod n
            const std::complex<float> hm = h0((n - x) % n, (n - z) % n, n, c, static_cast<std::uint32_t>(cascade), p);
            hk[static_cast<std::size_t>(z) * n + x] = h0(x, z, n, c, static_cast<std::uint32_t>(cascade), p) * e + std::conj(hm) * std::conj(e);
        }
    }
    ifft2D(hk, n);
    std::vector<float> out(hk.size());
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const float sign = ((x + z) & 1) != 0 ? -1.0f : 1.0f;
            out[static_cast<std::size_t>(z) * n + x] = hk[static_cast<std::size_t>(z) * n + x].real() * sign;
        }
    }
    return out;
}

}  // namespace rvk::enh::ocean
