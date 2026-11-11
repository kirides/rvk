// Material pack (src/enh/material_pack.*, docs/research/material-pack.md): name matching, manifest parsing, the pack word.

#include <cmath>

#include "engine/weather.h"
#include "enh/material_pack.h"
#include "enh/rain_wetness.h"
#include "test.h"

using namespace rvk::enh::matpack;

TEST(matpack_normalize_key) {
    CHECK(normalizeKey("Arch_House_Wall_01_Diffuse_01") == "arch_house_wall_01_diffuse_01");
    CHECK(normalizeKey("Arch_House_Wall_01_Diffuse_01._ximg") == "arch_house_wall_01_diffuse_01");
    CHECK(normalizeKey("data/Textures/Wall_Diffuse.DDS") == "wall_diffuse");
    CHECK(normalizeKey("C:\\pack\\Wall_Diffuse.png") == "wall_diffuse");
}

TEST(matpack_classify_file) {
    FileName a = classifyFile("Wood_01_Diffuse_01_orm.dds");
    CHECK(a.key == "wood_01_diffuse_01" && a.kind == Kind::Orm);
    a = classifyFile("Wood_01_Diffuse_01_R.PNG");
    CHECK(a.key == "wood_01_diffuse_01" && a.kind == Kind::Roughness);
    a = classifyFile("Wood_01_Diffuse_01_roughness.dds");
    CHECK(a.key == "wood_01_diffuse_01" && a.kind == Kind::Roughness);
    a = classifyFile("Wood_01_Diffuse_01.dds");  // no suffix: not a pack map
    CHECK(a.kind == Kind::Unknown);
    a = classifyFile("_r.dds");  // nothing before the suffix
    CHECK(a.kind == Kind::Unknown);
}

TEST(matpack_glob) {
    CHECK(globMatch("arch_*_diffuse_01", "arch_house_wall_01_diffuse_01"));
    CHECK(globMatch("ARCH_*", "arch_house"));
    CHECK(globMatch("*stone*", "nat_stone_rock_01"));
    CHECK(globMatch("a?c", "abc"));
    CHECK(!globMatch("a?c", "ac"));
    CHECK(!globMatch("wood_*", "stone_wood"));
    CHECK(globMatch("*", ""));
    CHECK(!globMatch("abc", "abcd"));
}

TEST(matpack_manifest) {
    std::string err;
    const auto m = parseManifest(R"(
[defaults]
roughness_scale = 0.9

[material."Wood_*_Diffuse_01"]
roughness_bias = 0.1

[material."Wood_Table_Diffuse_01"]
metalness = 0.2
emissive = 1
map = "table.dds"
bogus = 3
)",
                                 &err);
    CHECK(m.has_value());
    CHECK(m->rules.size() == 2);
    CHECK(m->warnings.size() == 1);  // bogus
    const Constants c = resolveConstants(*m, "wood_table_diffuse_01");
    CHECK(c.roughnessScale && std::fabs(*c.roughnessScale - 0.9f) < 1e-6f);
    CHECK(c.roughnessBias && std::fabs(*c.roughnessBias - 0.1f) < 1e-6f);
    CHECK(c.metalness && std::fabs(*c.metalness - 0.2f) < 1e-6f);
    CHECK(c.map && *c.map == "table.dds");
    const Constants other = resolveConstants(*m, "stone_diffuse_01");
    CHECK(!other.metalness && !other.roughnessBias);
    CHECK(hasRule(*m, "wood_a_diffuse_01"));
    CHECK(!hasRule(*m, "stone_diffuse_01"));
    CHECK(!parseManifest("[defaults\nx", &err).has_value() && !err.empty());
}

TEST(matpack_word_roundtrip) {
    Constants c;
    c.roughnessScale = 0.5f;
    c.roughnessBias = 0.2f;
    c.metalness = 1.0f;
    c.emissive = 1.0f;
    const std::uint32_t w = encodeWord(123, true, c, 0.7f);
    const Decoded d = decodeWord(w);
    CHECK(d.present && d.orm && d.emissive && d.slot == 123);
    CHECK(std::fabs(d.roughnessScale - 0.5f) < 0.04f);
    CHECK(std::fabs(d.roughnessBias - 0.2f) < 0.02f);
    CHECK(std::fabs(d.metalness - 1.0f) < 0.02f);
    // ORM texel (occlusion .8, roughness .5, metalness .75, emissive .25)
    const float t[4] = {0.8f, 0.5f, 0.75f, 0.25f};
    const Surface s = evaluate(d, t);
    CHECK(std::fabs(s.occlusion - 0.8f) < 1e-6f);
    CHECK(std::fabs(s.roughness - (0.5f * d.roughnessScale + d.roughnessBias)) < 1e-6f);
    CHECK(std::fabs(s.metalness - 0.75f * d.metalness) < 1e-6f);
    CHECK(std::fabs(s.emissive - 0.25f) < 1e-6f);
}

TEST(matpack_word_slot_range) {
    for (const std::uint32_t slot : {1u, 2u, 255u, 8192u, 16383u}) {
        CHECK(slotFitsWord(slot));
        const Decoded d = decodeWord(encodeWord(slot, true, Constants{}, 0.7f));
        CHECK(d.slot == slot && d.present && d.orm && !d.derived);
    }
    for (const std::uint32_t slot : {16384u, 16385u, 65535u}) CHECK(!slotFitsWord(slot));  // not encodable: MaterialPack::finish drops the entry
    const Decoded dv = decodeWord(encodeDerivedWord(0.5f));
    CHECK(dv.derived && dv.slot == 0 && dv.present);
}

TEST(matpack_word_constants_only) {
    Constants c;
    c.roughness = 0.4f;
    c.metalness = 1.0f;
    const Decoded d = decodeWord(encodeWord(0, false, c, 0.7f));
    CHECK(d.present && d.slot == 0 && !d.orm);
    const float t[4] = {1, 1, 1, 0};
    const Surface s = evaluate(d, t);
    CHECK(std::fabs(s.roughness - 0.4f) < 0.02f);
    CHECK(std::fabs(s.metalness - 1.0f) < 0.02f);
    // nothing given: the default roughness, dielectric
    const Decoded e = decodeWord(encodeWord(0, false, Constants{}, 0.7f));
    CHECK(std::fabs(evaluate(e, t).roughness - 0.7f) < 0.04f && evaluate(e, t).metalness == 0.0f);
}

TEST(matpack_word_roughness_map) {
    // a roughness-only map: R is the roughness, the metalness is the constant (default 0), a constant roughness is ignored
    const Decoded d = decodeWord(encodeWord(5, false, Constants{}, 0.7f));
    const float t[4] = {0.3f, 0.9f, 0.9f, 0.9f};
    const Surface s = evaluate(d, t);
    CHECK(std::fabs(s.roughness - 0.3f) < 0.01f && s.metalness == 0.0f && s.occlusion == 1.0f && s.emissive == 0.0f);
    Constants c;
    c.roughness = 0.2f;
    const Decoded k = decodeWord(encodeWord(5, false, c, 0.7f));
    CHECK(std::fabs(evaluate(k, t).roughness - 0.3f) < 0.01f);  // the constant does not replace a map's roughness
    CHECK(decodeWord(0).present == false);
}

TEST(matpack_word_glass_wet) {
    Constants c;
    c.transmission = 1.0f;
    c.wetness = 1.0f;
    c.roughness = 0.04f;
    const Decoded d = decodeWord(encodeWord(0, false, c, 0.7f));
    CHECK(d.glass && d.present && std::fabs(d.wetness - 1.0f) < 1e-6f);
    c.wetness = 0.5f;  // 2 bits: 0, 1/3, 2/3, 1
    CHECK(std::fabs(decodeWord(encodeWord(0, false, c, 0.7f)).wetness - 2.0f / 3.0f) < 1e-6f);
    CHECK(!decodeWord(encodeWord(0, false, Constants{}, 0.7f)).glass);
    // the layout fields do not overlap: every field at its maximum
    Constants m;
    m.roughnessScale = 1.875f;
    m.roughnessBias = 1.0f;
    m.metalness = 1.0f;
    m.wetness = 1.0f;
    m.transmission = 1.0f;
    m.emissive = 1.0f;
    const Decoded e = decodeWord(encodeWord(16383, true, m, 0.7f));
    CHECK(e.slot == 16383 && e.orm && e.emissive && e.glass && e.roughnessScale == 1.875f && e.roughnessBias == 1.0f && e.metalness == 1.0f && e.wetness == 1.0f);
}

TEST(rain_of_weather_share_of_rain_states) {
    CHECK(rvk::engine::rainOfWeather({}) == 0.0f);
    CHECK(rvk::engine::rainOfWeather({{"Sunny_Full", 1.0f}}) == 0.0f);
    CHECK(rvk::engine::rainOfWeather({{"Stormy_Rain", 1.0f}}) == 1.0f);
    const float half = rvk::engine::rainOfWeather({{"Sunny_Half_02", 0.5f}, {"Stormy_Rain", 0.5f}});
    CHECK(std::fabs(half - 0.5f) < 1e-5f);
    // a dungeon state is dry and carries weight: the rain does not reach the player inside
    const float inside = rvk::engine::rainOfWeather({{"Dungeon_Cave", 0.75f}, {"Stormy_Rain", 0.25f}});
    CHECK(std::fabs(inside - 0.25f) < 1e-5f);
}

TEST(rain_wetness_rises_fast_and_dries_slowly) {
    float wet = 0.0f;
    for (int i = 0; i < 60; ++i) wet = rvk::enh::rain::wetnessStep(wet, 1.0f, 1.0f);  // a minute of rain
    CHECK(wet > 0.85f && wet <= 1.0f);
    const float wetNow = wet;
    for (int i = 0; i < 60; ++i) wet = rvk::enh::rain::wetnessStep(wet, 0.0f, 1.0f);  // a minute of sun: still mostly wet
    CHECK(wet < wetNow && wet > wetNow - 0.3f);
    for (int i = 0; i < 300; ++i) wet = rvk::enh::rain::wetnessStep(wet, 0.0f, 1.0f);
    CHECK(wet == 0.0f);
    // light rain (0.3) holds the level at its share, never above it
    wet = 0.0f;
    for (int i = 0; i < 400; ++i) wet = rvk::enh::rain::wetnessStep(wet, 0.3f, 1.0f);
    CHECK(std::fabs(wet - 0.3f) < 0.01f);
}

TEST(matpack_derive_roughness_from_power) {
    // n = power * 128; GGX alpha = sqrt(2 / (n + 2)); the pack's roughness r has alpha = r * r
    CHECK(std::fabs(roughnessFromPower(0.25f) - std::sqrt(std::sqrt(2.0f / 34.0f))) < 1e-6f);
    CHECK(roughnessFromPower(1.0f) < roughnessFromPower(0.25f));  // a higher exponent is smoother
    CHECK(roughnessFromPower(0.0f) > 0.99f);                      // n = 0: fully rough
}

TEST(matpack_derive_from_specular_rules) {
    DeriveOptions o;
    o.enabled = true;
    SpecularInfo s;
    CHECK(!deriveFromSpecular(s, o, 0.7f).valid);  // no Specular role: no data
    s.hasMask = true;
    s.maskLum = 1.0f;
    s.powerKnown = true;
    s.power = 0.25f;
    const Derived d = deriveFromSpecular(s, o, 0.7f);
    CHECK(d.valid && d.strength == 1.0f && std::fabs(d.roughness - glossGate(roughnessFromPower(0.25f))) < 1e-6f);
    CHECK(d.roughness > 0.7f);  // a broad matte lobe (n = 32): gated to rough, not traced
    s.power = 1.0f;
    CHECK(deriveFromSpecular(s, o, 0.7f).roughness < 0.4f);  // n = 128: really glossy, keeps its roughness
    s.power = 0.25f;
    s.power = 8.0f;  // very smooth: clamped to the minimum
    CHECK(std::fabs(deriveFromSpecular(s, o, 0.7f).roughness - o.minRoughness) < 1e-6f);
    s.power = 0.0f;  // very rough: clamped to the maximum
    CHECK(deriveFromSpecular(s, o, 0.7f).roughness >= o.maxRoughness);
    s.power = 0.25f;
    s.maskLum = 0.5f;  // a weak mask pushes towards 1
    const Derived w = deriveFromSpecular(s, o, 0.7f);
    CHECK(w.roughness > d.roughness && w.roughness < 1.0f);
    s.maskLum = 0.0f;  // black constant mask: no highlight
    CHECK(!deriveFromSpecular(s, o, 0.7f).valid);
    s.maskMap = true;  // a map: fixed strength
    CHECK(std::fabs(deriveFromSpecular(s, o, 0.7f).strength - kMapMaskStrength) < 1e-6f);
    s.powerKnown = false;  // a power texture: the default roughness stands in
    {
        const float g = glossGate(0.7f);
        CHECK(std::fabs(deriveFromSpecular(s, o, 0.7f).roughness - (g + (1.0f - g) * (1.0f - kMapMaskStrength))) < 1e-5f);
    }
    o.enabled = false;
    CHECK(!deriveFromSpecular(s, o, 0.7f).valid);  // off: nothing
}

TEST(matpack_derived_word_roundtrip) {
    const std::uint32_t w = encodeDerivedWord(0.5f);
    const Decoded d = decodeWord(w);
    CHECK(d.present && d.derived && !d.orm && d.slot == 0 && !d.glass && !d.emissive && d.metalness == 0.0f && d.wetness == 0.0f);
    const float t[4] = {1, 1, 1, 0};
    CHECK(std::fabs(evaluate(d, t).roughness - 0.5f) < 1.0f / 255.0f);
    // a real ORM word (slot != 0) is never read as derived; a constant entry (bit 15 clear) neither
    CHECK(!decodeWord(encodeWord(5, true, Constants{}, 0.7f)).derived);
    CHECK(!decodeWord(encodeWord(0, false, Constants{}, 0.7f)).derived);
}
