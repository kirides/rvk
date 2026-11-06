// The material table's pure parts (src/gpuscene/material_compose.h / material_graph.h, docs/research/frame-ownership.md F0): the record built from raw engine
// bytes composes back into the registers / bindings of a draw (diffMaterial = the table's verification), and the graph normalisation equals
// tools/material_graph_census.py (the vectors below are that tool's output: tools/tests/test_material_graph_census.py checks the same ones).

#include <bitset>
#include <cstdint>
#include <cstring>
#include <string>

#include "gpuscene/material_compose.h"
#include "gpuscene/material_graph.h"
#include "test.h"

using namespace rvk;
using namespace rvk::gpuscene;
namespace sc = rvk::native::sc;

namespace {

struct Img {
    std::uint8_t b[kMatRawBytes]{};
    float vs[32][4]{}, ps[16][4]{};
};

void put32(std::uint8_t* b, std::uint32_t off, std::uint32_t v) { std::memcpy(b + off, &v, 4); }

Img makeImg(std::uint32_t seed) {
    Img m;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f * 8.0f - 2.0f;
    };
    put32(m.b, 0x9c, 232);
    m.b[0xc1] = 5;
    put32(m.b, 0xcc, 6);
    m.b[0xf1] = 4;
    put32(m.b, 0x50, 1);
    m.b[0x58] = 96;
    m.b[0x219] = 9;
    m.b[0x21b] = 3;
    m.b[0x330] = 1;
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 4; ++c) m.vs[r][c] = rnd();
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m.ps[r][c] = rnd();
    for (std::uint32_t k = 0; k < 3; ++k) {
        const std::uint32_t o = 0xf8 + 0x28 * k;
        put32(m.b, o, 1 + k);                 // ADDRESSU
        put32(m.b, o + 4, 1);                 // ADDRESSV
        put32(m.b, o + 8, k == 1 ? kEngineNoState : 2u);   // MINFILTER
        put32(m.b, o + 0xc, 2);               // MAGFILTER
        put32(m.b, o + 0x10, kEngineNoState);  // MIPFILTER
        put32(m.b, o + 0x18, 0);
        m.b[o + 0x14] = static_cast<std::uint8_t>(k == 2 ? 0xff : 0);  // sRGB int8 = -1
        const std::uint64_t img = 0x1000 + k;
        std::memcpy(m.b + o + 0x20, &img, 8);
    }
    return m;
}

std::uint32_t texOfFake(std::uint32_t stage, const void* res) {
    const auto v = reinterpret_cast<std::uintptr_t>(res);
    return v == 0 ? kMatNoTex : static_cast<std::uint32_t>(v - 0x1000) * 100 + stage;
}

// what a composition holding this material would have in its draw
struct Drawn {
    float vsF[sc::kVsRegs][4]{}, psF[sc::kPsRegs][4]{};
    std::bitset<sc::kVsRegs> vsLive, psLive;
    MaterialDraw d;
};

void composeDraw(const Img& m, Drawn& out, std::uint32_t defFilter) {
    for (int r = 0; r < 5; ++r) {
        std::memcpy(out.vsF[232 + r], m.vs[r], 16);
        out.vsLive[232 + r] = true;
    }
    for (int r = 0; r < 4; ++r) {
        std::memcpy(out.psF[6 + r], m.ps[r], 16);
        out.psLive[6 + r] = true;
    }
    MaterialDraw& d = out.d;
    d.vsF = out.vsF;
    d.psF = out.psF;
    d.vsLive = &out.vsLive;
    d.psLive = &out.psLive;
    d.samplerMask = 0x7u << 9;
    for (auto& t : d.texSlot) t = kMatNoTex;
    d.defFilter = defFilter;
    d.blendMode = 1;
    d.maskRef = 96;
    for (std::uint32_t k = 0; k < 3; ++k) {
        const std::uint32_t o = 0xf8 + 0x28 * k, st = 9 + k;
        std::uint32_t raw[6], rd[6];
        std::memcpy(&rd[0], m.b + o, 4);
        std::memcpy(&rd[1], m.b + o + 4, 4);
        std::memcpy(&rd[2], m.b + o + 0xc, 4);
        std::memcpy(&rd[3], m.b + o + 8, 4);
        std::memcpy(&rd[4], m.b + o + 0x10, 4);
        std::memcpy(&rd[5], m.b + o + 0x18, 4);
        for (int i = 0; i < 6; ++i) raw[i] = rd[i] == kEngineNoState ? defFilter : rd[i];
        for (int i = 0; i < 6; ++i) d.ss[st][i] = raw[i];
        d.ss[st][6] = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(m.b[o + 0x14])));
        d.texSlot[st] = texOfFake(st, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x1000 + k)));
    }
}

MaterialRecord build(const Img& m) {
    MaterialRecord r{};
    fillMaterialRecord(r, m.b, m.vs, m.ps, texOfFake);
    r.info[0] = 1;
    return r;
}

}  // namespace

TEST(material_table_record_size_matches_the_shader_stride) {
    CHECK(sizeof(MaterialRecord) == 1104);
    CHECK(offsetof(MaterialRecord, vs) == 80);
    CHECK(offsetof(MaterialRecord, ps) == 80 + 512);
    CHECK(offsetof(MaterialRecord, slots) == 80 + 512 + 256);
}

TEST(material_table_record_composes_back_into_the_draw) {
    for (std::uint32_t seed = 1; seed <= 20; ++seed) {
        const Img m = makeImg(seed);
        const MaterialRecord r = build(m);
        CHECK(r.counts[0] == 232 && r.counts[1] == 5 && r.counts[2] == 6 && r.counts[3] == 4);
        CHECK(matBaseStage(r) == 9 && matSlotCount(r) == 3);
        CHECK((r.info[3] & MatHasOpacity) != 0);
        Drawn dr;
        composeDraw(m, dr, 0x31 + seed);
        const MaterialDiff df = diffMaterial(r, dr.d);
        CHECK(!df.differs());
        CHECK(df.regsCompared == 9 && df.texCompared == 3 && df.stateCompared == 3 && df.blendCompared == 1);
    }
}

TEST(material_table_diff_names_each_kind_of_difference) {
    const Img m = makeImg(7);
    const MaterialRecord r = build(m);
    {
        Drawn dr;
        composeDraw(m, dr, 5);
        dr.vsF[234][2] += 1.0f;
        const MaterialDiff df = diffMaterial(r, dr.d);
        CHECK(df.regsDiffer == 1 && df.firstReg == 234 && df.texDiffer == 0);
    }
    {
        Drawn dr;
        composeDraw(m, dr, 5);
        dr.psF[9][0] += 1.0f;
        const MaterialDiff df = diffMaterial(r, dr.d);
        CHECK(df.regsDiffer == 1 && df.firstReg == 1009);
    }
    {
        Drawn dr;
        composeDraw(m, dr, 5);
        dr.d.texSlot[10] += 1;
        const MaterialDiff df = diffMaterial(r, dr.d);
        CHECK(df.texDiffer == 1 && df.firstStage == 10 && df.regsDiffer == 0);
    }
    {
        Drawn dr;
        composeDraw(m, dr, 5);
        dr.d.ss[11][3] ^= 1;
        const MaterialDiff df = diffMaterial(r, dr.d);
        CHECK(df.stateDiffer == 1 && df.firstStage == 11 && df.firstSsWord == 3);
    }
    {
        Drawn dr;
        composeDraw(m, dr, 5);
        dr.d.maskRef = 95;
        CHECK(diffMaterial(r, dr.d).blendDiffer == 1);
        dr.d.maskRef = 96;
        dr.d.blendMode = 3;
        CHECK(diffMaterial(r, dr.d).blendDiffer == 1);
        dr.d.checkBlend = false;
        CHECK(diffMaterial(r, dr.d).blendDiffer == 0);
    }
    {
        // a texture composed as inherited / unresolved is not compared
        Drawn dr;
        composeDraw(m, dr, 5);
        dr.d.texSlot[9] = kMatNoTex;
        const MaterialDiff df = diffMaterial(r, dr.d);
        CHECK(!df.differs() && df.texCompared == 2);
    }
}

TEST(material_table_default_filter_is_a_draw_parameter) {
    const Img m = makeImg(3);
    const MaterialRecord r = build(m);
    Drawn dr;
    composeDraw(m, dr, 7);
    CHECK(!diffMaterial(r, dr.d).differs());
    dr.d.defFilter = 8;  // the gfx admin's default filter changed: the slots holding "no state" resolve differently, the record is untouched
    CHECK(diffMaterial(r, dr.d).stateDiffer == 3);  // all three slots hold "no state" in MIPFILTER
}

TEST(material_table_switch_written_registers_are_not_compared) {
    const Img m = makeImg(11);
    MaterialRecord r = build(m);
    r.sw[0] = 1u << 2;               // VS pool register 2 (c234)
    r.sw[1] = (1u << 1) | (1u << (16 + 1));  // PS pool register 1 (c7), sampler slot 1
    Drawn dr;
    composeDraw(m, dr, 5);
    dr.vsF[234][0] += 3.0f;
    dr.psF[7][1] += 3.0f;
    dr.d.texSlot[10] += 5;
    dr.d.ss[10][0] ^= 4;
    const MaterialDiff df = diffMaterial(r, dr.d);
    CHECK(!df.differs());
    CHECK(df.regsSwitch == 2 && df.regsCompared == 7 && df.texCompared == 2);
}

TEST(material_table_only_the_pools_of_the_default_class_are_compared) {
    Img m = makeImg(5);
    put32(m.b, 0x9c, 200);  // a VS pool starting below c232: the global pool owns c200..c231 in the default class
    const MaterialRecord r = build(m);
    Drawn dr;
    composeDraw(m, dr, 5);
    for (int i = 0; i < 5; ++i) dr.vsLive[200 + i] = true;
    dr.vsF[201][0] = 99.0f;  // not the record's: but the composition takes c201 from the global pool
    CHECK(!diffMaterial(r, dr.d).differs());
}

TEST(material_table_graph_normalisation_equals_the_census_tool) {
    CHECK(normaliseGraph("g_ShaderData[3].xy + g_vShaderData[ 12 ] * g_SamplerData[2]") == "g_ShaderData[#].xy + g_vShaderData[#] * g_SamplerData[#]");
    CHECK(normaliseGraph("x * 1.0f") == "x * 1.0f");
    CHECK(normaliseGraph("(tex2D( g_SamplerData[0], XFX_PS_TEX0 )) * g_ShaderData[0]") == "(tex2D( g_SamplerData[#], XFX_PS_TEX0 )) * g_ShaderData[#]");
    CHECK(normaliseGraph("xg_ShaderData[1] g_ShaderData [ 7 ] g_ShaderData[a] g_SamplerData[ 3]+g_ShaderData[11]") ==
          "xg_ShaderData[1] g_ShaderData[#] g_ShaderData[a] g_SamplerData[#]+g_ShaderData[#]");
    CHECK(normaliseGraph("g_vShaderData[0].x*g_vShaderData[10]") == "g_vShaderData[#].x*g_vShaderData[#]");
}

TEST(material_table_graph_text_and_static_macros_equal_the_census_tool) {
    const MacroList macros = {{"XFX_PS_DIFFUSE", "tex2D(g_SamplerData[2],XFX_PS_TEX0)*g_ShaderData[5]"},
                              {"XFX_VS_TEX0", "XFX_VS_INPUT_TEX0"},
                              {"XFX_QUALITY", "2"},
                              {"XFX_DIRLIGHT", "true"},
                              {"XFX_SKINNED", "true"},
                              {"XFX_DOUBLESIDED", "true"}};
    CHECK(graphText(macros, false) == "XFX_PS_DIFFUSE=tex2D(g_SamplerData[#],XFX_PS_TEX0)*g_ShaderData[#] ;; XFX_VS_TEX0=XFX_VS_INPUT_TEX0");
    CHECK(graphText(macros, true) == "XFX_VS_TEX0=XFX_VS_INPUT_TEX0");
    CHECK(staticText(macros) == "XFX_DOUBLESIDED=true XFX_QUALITY=2 XFX_SKINNED=true");
}

TEST(material_table_graph_ids_ignore_indices_and_split_on_structure) {
    const auto id = [](const std::string& diffuse, const char* entry, const char* extra) {
        MacroList m = {{"XFX_PS_DIFFUSE", diffuse}, {"XFX_QUALITY", "2"}};
        if (extra[0] != 0) m.emplace_back(extra, "true");
        const std::string g = graphText(m, false);
        return std::make_pair(graphId("ge_default_phong.fx", entry, g), pipelineId(graphId("ge_default_phong.fx", entry, g), staticText(m)));
    };
    const auto a = id("tex2D(g_SamplerData[0],T)*g_ShaderData[0]", "PS_Default", "");
    const auto b = id("tex2D(g_SamplerData[3],T)*g_ShaderData[5]", "PS_Default", "");
    CHECK(a == b);  // indices are an assignment
    const auto c = id("tex2D(g_SamplerData[0],T)*g_ShaderData[0]", "PS_Default", "XFX_SKINNED");
    CHECK(c.first == a.first && c.second != a.second);  // a static macro splits the pipeline, not the graph
    const auto d = id("tex2D(g_SamplerData[0],T)*0.5f", "PS_Default", "");
    CHECK(d.first != a.first);
    const auto e = id("tex2D(g_SamplerData[0],T)*g_ShaderData[0]", "PS_DefaultZPass", "");
    CHECK(e.first != a.first);  // the entry is part of the id
    CHECK(a.first != 0 && a.second != 0);
}
