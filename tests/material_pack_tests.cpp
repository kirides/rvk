// Material pack (src/enh/material_pack.*, docs/research/material-pack.md): name matching, manifest parsing, the pack word.

#include <cmath>

#include "enh/material_pack.h"
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
    CHECK(std::fabs(evaluate(e, t).roughness - 0.7f) < 0.02f && evaluate(e, t).metalness == 0.0f);
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
