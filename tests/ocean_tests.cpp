// FFT ocean: the spectrum's normalisation and the FFT against a reference DFT (enh/ocean_spectrum.h; the shaders mirror it),
// the sea floor field (enh/sea_floor.h)

#include <cmath>
#include <complex>
#include <filesystem>
#include <random>
#include <vector>

#include "enh/ocean_spectrum.h"
#include "enh/sea_floor.h"
#include "engine/weather.h"
#include "test.h"

using namespace rvk;
namespace oc = rvk::enh::ocean;

TEST(ocean_lgamma_series) {
    for (double x : {0.5, 1.0, 1.5, 3.0, 7.5, 20.0, 81.0, 401.0}) {
        CHECK(std::abs(oc::lgammaApprox(x) - std::lgamma(x)) < 1e-9 * std::max(1.0, std::abs(std::lgamma(x))));
    }
}

// the directional spreading integrates to 1 over [-pi, pi] for every exponent (narrow swell to broad short waves)
TEST(ocean_spreading_is_normalised) {
    for (double s : {0.3, 1.0, 2.5, 9.0, 40.0, 150.0}) {
        double sum = 0.0;
        const int steps = 20000;
        const double d = 2.0 * oc::kPi / steps;
        for (int i = 0; i < steps; ++i) sum += oc::hasselmannD(-oc::kPi + (i + 0.5) * d, s) * d;
        CHECK(std::abs(sum - 1.0) < 2e-3);
    }
    // the swell term narrows the long waves
    CHECK(oc::hasselmannS(0.5, 1.0, 10.0, 1.0) > oc::hasselmannS(0.5, 1.0, 10.0, 0.0));
}

// JONSWAP with the fetch-limited parameters (alpha, w_p from U and F) follows the empirical growth law g Hs / U^2 = 0.0016
// (g F / U^2)^0.5 (Hasselmann et al. 1973) with a constant factor: the two fits of the JONSWAP data differ by ~1.31 at every
// fetch (measured; the presets are chosen with it)
TEST(ocean_jonswap_significant_height) {
    for (const auto& [u, f] : {std::pair{7.0, 40000.0}, std::pair{11.0, 100000.0}, std::pair{16.0, 200000.0}}) {
        const double hs = oc::significantHeight(oc::jonswapFor(u, f));
        const double expected = 0.0016 * u * std::sqrt(f / oc::kGravity);
        CHECK(std::abs(hs / expected - 1.31) < 0.05);
    }
}

// S(k) is S(w, theta) moved to the k-plane: its integral over the plane is the height variance m0 of S(w)
TEST(ocean_spectrum_k_plane_integral) {
    oc::SeaState s;
    s.windSpeed = 9.0f;
    s.fetch = 60000.0f;
    s.swellHeight = 0.0f;
    oc::SpectrumParams p = oc::spectrumParams(s);
    p.shortWaveCut = 0.0f;
    const oc::Jonswap j = oc::jonswapFor(s.windSpeed, s.fetch);
    const double m0 = std::pow(oc::significantHeight(j) / 4.0, 2.0);
    // polar integral: S(k) k dk dtheta, k from w: k = w^2 / g
    double sum = 0.0;
    const int nk = 4000, nt = 720;
    const double k0 = std::pow(0.2 * j.omegaP, 2.0) / oc::kGravity, k1 = std::pow(20.0 * j.omegaP, 2.0) / oc::kGravity;
    const double lk0 = std::log(k0), lk1 = std::log(k1);
    for (int i = 0; i < nk; ++i) {
        const double lk = lk0 + (i + 0.5) * (lk1 - lk0) / nk;
        const double k = std::exp(lk);
        const double dk = k * (lk1 - lk0) / nk;
        for (int t = 0; t < nt; ++t) {
            const double th = -oc::kPi + (t + 0.5) * 2.0 * oc::kPi / nt;
            sum += oc::spectrumK(k * std::cos(th), k * std::sin(th), p) * k * dk * (2.0 * oc::kPi / nt);
        }
    }
    CHECK(std::abs(sum / m0 - 1.0) < 0.02);
    // the swell gets its significant height
    oc::SeaState sw;
    sw.windSpeed = 0.1f;
    sw.swellHeight = 1.2f;
    sw.swellPeriod = 11.0f;
    const oc::SpectrumParams ps = oc::spectrumParams(sw);
    const double hs = oc::significantHeight(oc::Jonswap{ps.swellAlpha, ps.swellOmegaP, 3.3});
    CHECK(std::abs(hs - 1.2) < 0.01);
}

// the cascades' bands are disjoint and cover the spectrum: their variances add up to (nearly) all of it
TEST(ocean_cascades_cover_the_spectrum) {
    for (int q = 0; q < 3; ++q) {
        oc::Cascades cs = oc::cascadesFor(q);
        for (int i = 0; i + 1 < cs.count; ++i) CHECK(cs.c[i].kHigh == cs.c[i + 1].kLow);
        for (int i = 0; i < cs.count; ++i) {
            // each band below its tile's Nyquist wave number
            const float nyquist = static_cast<float>(oc::kPi) * cs.size / cs.c[i].length;
            CHECK(i + 1 == cs.count || cs.c[i].kHigh < nyquist);
        }
    }
    oc::Cascades cs = oc::cascadesFor(1);
    oc::SeaState s = oc::seaStateForLevel(1.0f);
    s.swellHeight = 0.0f;
    oc::SpectrumParams p = oc::spectrumParams(s);
    p.shortWaveCut = 0.0f;
    oc::cascadeStats(cs, p);
    double total = 0.0;
    for (int i = 0; i < cs.count; ++i) total += cs.c[i].heightVar;
    const double m0 = std::pow(oc::significantHeight(oc::jonswapFor(s.windSpeed, s.fetch)) / 4.0, 2.0);
    CHECK(std::abs(total / m0 - 1.0) < 0.08);
    CHECK(cs.c[0].heightVar > cs.c[1].heightVar && cs.c[1].heightVar > cs.c[2].heightVar);
}

// the in-place radix-2 IFFT (the algorithm of ocean_fft.comp) equals the direct sum
TEST(ocean_fft_matches_dft) {
    for (int n : {8, 32}) {
        std::mt19937 rng(1234u + static_cast<unsigned>(n));
        std::normal_distribution<float> nd;
        std::vector<std::complex<float>> data(static_cast<std::size_t>(n) * n);
        for (auto& v : data) v = {nd(rng), nd(rng)};
        const auto ref = oc::idft2D(data, n);
        oc::ifft2D(data, n);
        double maxErr = 0.0, maxRef = 0.0;
        for (std::size_t i = 0; i < data.size(); ++i) {
            maxErr = std::max(maxErr, std::abs(std::complex<double>(data[i]) - ref[i]));
            maxRef = std::max(maxRef, std::abs(ref[i]));
        }
        CHECK(maxErr < 1e-5 * maxRef * n);
    }
}

// h0 with E|h0|^2 = S dk^2 / 2: the height field of a cascade has the band's variance (statistically) and is real
TEST(ocean_height_field_variance) {
    oc::Cascades cs = oc::cascadesFor(0);  // 3 x 128
    oc::SeaState s = oc::seaStateForLevel(2.0f);
    const oc::SpectrumParams p = oc::spectrumParams(s);
    oc::cascadeStats(cs, p);
    for (int c = 0; c < cs.count; ++c) {
        double var = 0.0;
        int samples = 0;
        for (float t : {0.0f, 3.7f, 11.0f}) {
            const std::vector<float> h = oc::heightField(cs, c, p, t);
            double mean = 0.0;
            for (float v : h) mean += v;
            mean /= static_cast<double>(h.size());
            for (float v : h) var += (v - mean) * (v - mean);
            samples += static_cast<int>(h.size());
        }
        var /= samples;
        // the longest cascade has few modes carrying most energy (large statistical spread)
        CHECK(std::abs(var / cs.c[c].heightVar - 1.0) < (c == 0 ? 0.5 : 0.2));
    }
}

// ---- the sea floor field (enh/sea_floor.h) ----------------------------------------------------------------------------------

namespace sf = rvk::enh::seafloor;

namespace {

// a quad (two triangles) at heights y00 .. y11 over [x0, x1] x [z0, z1]
void quad(std::vector<enh::shore::Triangle>& out, float x0, float z0, float x1, float z1, float y00, float y10, float y01, float y11) {
    out.push_back({{x0, y00, z0}, {x1, y10, z0}, {x1, y11, z1}});
    out.push_back({{x0, y00, z0}, {x1, y11, z1}, {x0, y01, z1}});
}

}  // namespace

TEST(ocean_phase_speed_limits) {
    // deep water: g T / 2 pi; shallow: sqrt(g d)
    CHECK(std::abs(sf::phaseSpeed(500.0f, 8.0f) - 9.81f * 8.0f / 6.2831853f) < 0.05f);
    CHECK(std::abs(sf::phaseSpeed(0.2f, 8.0f) - std::sqrt(9.81f * 0.2f)) < 0.05f);
    CHECK(sf::phaseSpeed(2.0f, 8.0f) < sf::phaseSpeed(10.0f, 8.0f));
}

// a beach sloping into the sea along -z (1:50): depth, signed distance, travel time and direction behave
TEST(ocean_floor_beach) {
    std::vector<enh::shore::Triangle> tris;
    quad(tris, -8000.0f, -30000.0f, 8000.0f, 5000.0f, -600.0f, -600.0f, 100.0f, 100.0f);  // y = 0.02 z (z from -30000)
    sf::Heights h;
    h.reset(sf::gridAround(-7000.0f, -29000.0f, 7000.0f, 4000.0f, 0.0f, 256, 32.0f));  // inside the quad: every texel known
    h.add(tris);
    CHECK(h.known() == h.grid.count());
    const sf::Field f = sf::bake(h, 0.0f, {});
    CHECK(f.valid());
    // the waterline at z = 0 (y = 0)
    const auto shore = f.at(0.0f, -10000.0f);
    CHECK(std::abs(shore[0] - 10000.0f) < 3.0f * f.grid.cell);  // signed distance
    CHECK(std::abs(shore[1] - 200.0f) < 10.0f);                  // depth
    CHECK(shore[3] > 0.8f);                                      // travelling towards +z (the shore)
    const auto landSide = f.at(0.0f, 3000.0f);
    CHECK(landSide[0] < -2000.0f && landSide[1] < 0.0f);
    // the travel time grows offshore, slower in the shallows: the first 5,000 units take longer than the next 5,000
    const float t5 = f.at(0.0f, -5000.0f)[4], t10 = f.at(0.0f, -10000.0f)[4], t15 = f.at(0.0f, -15000.0f)[4];
    CHECK(t5 > 0.0f && t10 > t5 && t15 > t10);
    CHECK(t5 > t15 - t10);
    // an open beach is fully exposed
    CHECK(f.at(0.0f, -20000.0f)[5] > 0.9f);
}

// a harbour: a basin behind walls with a narrow mouth is sheltered, the sea outside is not
TEST(ocean_floor_harbour_exposure) {
    std::vector<enh::shore::Triangle> tris;
    quad(tris, -60000.0f, -60000.0f, 60000.0f, 60000.0f, -1000.0f, -1000.0f, -1000.0f, -1000.0f);  // 10 m deep sea floor
    // walls around [-6000, 6000]^2, a 1,200-wide mouth in the south wall
    quad(tris, -6000.0f, 5500.0f, 6000.0f, 6000.0f, 300.0f, 300.0f, 300.0f, 300.0f);
    quad(tris, -6000.0f, -6000.0f, -5500.0f, 6000.0f, 300.0f, 300.0f, 300.0f, 300.0f);
    quad(tris, 5500.0f, -6000.0f, 6000.0f, 6000.0f, 300.0f, 300.0f, 300.0f, 300.0f);
    quad(tris, -6000.0f, -6000.0f, -600.0f, -5500.0f, 300.0f, 300.0f, 300.0f, 300.0f);
    quad(tris, 600.0f, -6000.0f, 6000.0f, -5500.0f, 300.0f, 300.0f, 300.0f, 300.0f);
    sf::Heights h;
    h.reset(sf::gridAround(-60000.0f, -60000.0f, 60000.0f, 60000.0f, 0.0f, 512, 32.0f));
    h.add(tris);
    const sf::Field f = sf::bake(h, 0.0f, {});
    const float inside = f.at(-3000.0f, 3000.0f)[5];
    const float outside = f.at(0.0f, -30000.0f)[5];
    CHECK(inside < 0.3f);
    CHECK(outside > 0.9f);
    CHECK(f.at(0.0f, 5800.0f)[0] < 0.0f);  // the wall is land
}

TEST(ocean_floor_cache_round_trip) {
    sf::Heights h;
    h.reset(sf::gridAround(0.0f, 0.0f, 1000.0f, 1000.0f, 0.0f, 16, 1.0f));
    for (std::size_t i = 0; i < h.top.size(); i += 3) h.top[i] = static_cast<float>(i);
    const auto file = std::filesystem::temp_directory_path() / "rvk_floor_test.bin";
    CHECK(h.save(file));
    sf::Heights g;
    g.reset(h.grid);
    g.top[1] = 5.0f;
    CHECK(g.load(file));
    CHECK(g.top[3] == 3.0f && g.top[1] == 5.0f && g.top[0] == 0.0f);
    sf::Heights other;
    other.reset(sf::gridAround(0.0f, 0.0f, 2000.0f, 1000.0f, 0.0f, 16, 1.0f));
    CHECK(!other.load(file));  // another grid
    std::filesystem::remove(file);
}

// the sea state follows the engine's weather blend (engine/weather.h)
TEST(ocean_sea_state_from_weather) {
    using rvk::engine::WeatherState;
    CHECK(!rvk::engine::seaLevelOfWeather({}).has_value());
    CHECK(!rvk::engine::seaLevelOfWeather({{"Dungeon_Blue6_Big", 1.0f}}).has_value());
    CHECK(std::abs(*rvk::engine::seaLevelOfWeather({{"Stormy_Rain", 1.0f}, {"Sunny_Full", 0.0f}}) - 3.0f) < 1e-4f);
    CHECK(*rvk::engine::seaLevelOfWeather({{"Sunny_Full", 1.0f}}) < 1.0f);
    const float mix = *rvk::engine::seaLevelOfWeather({{"Sunny_Half_02", 0.5f}, {"Stormy_Rain", 0.5f}});
    CHECK(mix > 1.5f && mix < 2.5f);
    // levels map onto growing seas
    CHECK(oc::seaStateForLevel(3.0f).windSpeed > oc::seaStateForLevel(1.5f).windSpeed);
    CHECK(oc::seaStateForLevel(0.0f).windSpeed < oc::seaStateForLevel(1.0f).windSpeed);
}
