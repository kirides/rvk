// Draw tags, records, resource identity, shader semantics (docs/research/draw-tags.md): pure logic.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "d3d9/defs.h"
#include "d3d9/draws.h"
#include "d3d9/identity.h"
#include "d3d9/semantics.h"
#include "fx/effect_binary.h"
#include "shader/glsl.h"
#include "shader/sm_ir.h"
#include "shader/spirv.h"
#include "test.h"

using namespace rvk::d3d9;
using rvk::d3d9::scene::Pass;
using rvk::d3d9::scene::View;

namespace {

using Op = BufferCopyState::Plan::Op;

// ---- a CTAB comment (layout as in fx_tests.cpp) ----
struct Entry {
    std::string name;
    std::uint16_t set, index, count, cls, type, rows, columns, elements;
};

std::vector<std::uint32_t> ctabComment(const char* target, std::uint32_t version, const std::vector<Entry>& entries) {
    std::vector<std::byte> b;
    auto put32 = [&b](std::uint32_t v) {
        const auto* p = reinterpret_cast<const std::byte*>(&v);
        b.insert(b.end(), p, p + 4);
    };
    auto put16 = [&b](std::uint16_t v) {
        const auto* p = reinterpret_cast<const std::byte*>(&v);
        b.insert(b.end(), p, p + 2);
    };
    const auto n = static_cast<std::uint32_t>(entries.size());
    const std::uint32_t infoOff = 28, typeOff = infoOff + 20 * n, strOff = typeOff + 16 * n;
    std::string strings = std::string("rvk test") + '\0' + target + '\0';
    std::vector<std::uint32_t> nameOffs;
    for (const Entry& e : entries) {
        nameOffs.push_back(strOff + static_cast<std::uint32_t>(strings.size()));
        strings += e.name + '\0';
    }
    put32(28);
    put32(strOff);
    put32(version);
    put32(n);
    put32(infoOff);
    put32(0);
    put32(strOff + 9);
    for (std::uint32_t i = 0; i < n; ++i) {
        put32(nameOffs[i]);
        put16(entries[i].set);
        put16(entries[i].index);
        put16(entries[i].count);
        put16(0);
        put32(typeOff + 16 * i);
        put32(0);
    }
    for (const Entry& e : entries) {
        put16(e.cls);
        put16(e.type);
        put16(e.rows);
        put16(e.columns);
        put16(e.elements);
        put16(0);
        put32(0);
    }
    for (char c : strings) b.push_back(static_cast<std::byte>(c));
    while (b.size() % 4 != 0) b.push_back(std::byte{0});
    std::vector<std::uint32_t> body(b.size() / 4);
    std::memcpy(body.data(), b.data(), b.size());
    std::vector<std::uint32_t> t{0xFFFEu | (static_cast<std::uint32_t>(body.size() + 1) << 16), rvk::fx::fourcc('C', 'T', 'A', 'B')};
    t.insert(t.end(), body.begin(), body.end());
    return t;
}

std::vector<std::uint32_t> shaderWith(std::uint32_t version, const char* target, const std::vector<Entry>& entries) {
    std::vector<std::uint32_t> t{version};
    const auto c = ctabComment(target, version, entries);
    t.insert(t.end(), c.begin(), c.end());
    t.push_back(0x0000FFFFu);
    return t;
}

constexpr std::uint16_t kFloat = 2, kSampler = 3;
// float4[count] / sampler2D[count] (count 1 = not an array)
Entry f(const char* n, std::uint16_t reg, std::uint16_t count) { return {n, kFloat, reg, count, 1, 3, 1, 4, count}; }
Entry s(const char* n, std::uint16_t reg, std::uint16_t count) { return {n, kSampler, reg, count, 4, 12, 1, 1, count}; }

rvk::shader::Reflection vsReflection(std::initializer_list<std::pair<rvk::shader::Usage, std::uint8_t>> inputs) {
    rvk::shader::Reflection r;
    r.version.stage = rvk::shader::Stage::Vertex;
    r.version.major = 3;
    std::uint32_t loc = 0;
    for (const auto& [u, i] : inputs) r.vertexInputs.push_back({loc++, u, i});
    return r;
}

}  // namespace

// ---- identity ----

TEST(identity_ids_unique) {
    const std::uint64_t a = newResourceId(), b = newResourceId(), c = newResourceId();
    CHECK(a != 0 && b > a && c > b);
}

TEST(identity_static_buffer_generations) {
    BufferCopyState st;
    CHECK(st.generation() == 0);
    // first use: a VkBuffer with everything
    auto p = st.plan(false, 1, 256, false);
    CHECK(p.op == Op::NewCopy && p.lo == 0 && p.hi == 256);
    st.committed(p, false, 1);
    CHECK(st.generation() == 1);
    // used again without a write (same or later frames): same VkBuffer, same generation
    CHECK(st.plan(false, 1, 256, true).op == Op::None);
    CHECK(st.plan(false, 7, 256, true).op == Op::None);
    // a read-only lock changes nothing
    st.lock(0, 0, LOCK_READONLY, 256);
    CHECK(st.plan(false, 7, 256, true).op == Op::None);
    // a write: a new VkBuffer (whole), generation 2
    st.lock(16, 16, 0, 256);
    p = st.plan(false, 7, 256, true);
    CHECK(p.op == Op::NewCopy && p.lo == 0 && p.hi == 256);
    // the allocation failed (no commit): still pending, generation unchanged
    CHECK(st.generation() == 1);
    st.committed(p, false, 7);
    CHECK(st.generation() == 2);
    CHECK(st.plan(false, 8, 256, true).op == Op::None);
}

TEST(identity_ring_buffer_generations) {
    BufferCopyState st;
    // frame 1: the first ring copy, whole
    auto p = st.plan(true, 1, 1024, false);
    CHECK(p.op == Op::NewCopy && p.lo == 0 && p.hi == 1024);
    st.committed(p, true, 1);
    CHECK(st.generation() == 1);
    // NOOVERWRITE append in the same frame: the frame's copy is patched, same generation
    st.lock(100, 50, LOCK_NOOVERWRITE, 1024);
    p = st.plan(true, 1, 1024, false);
    CHECK(p.op == Op::Patch && p.lo == 100 && p.hi == 150);
    st.committed(p, true, 1);
    CHECK(st.generation() == 1);
    // nothing written: nothing to do
    CHECK(st.plan(true, 1, 1024, false).op == Op::None);
    // a new frame: a new ring copy (whole: no DISCARD)
    p = st.plan(true, 2, 1024, false);
    CHECK(p.op == Op::NewCopy && p.lo == 0 && p.hi == 1024);
    st.committed(p, true, 2);
    CHECK(st.generation() == 2);
    // DISCARD in the same frame: a new copy of only what was written after it
    st.lock(0, 64, LOCK_DISCARD, 1024);
    p = st.plan(true, 2, 1024, false);
    CHECK(p.op == Op::NewCopy && p.lo == 0 && p.hi == 64);
    st.committed(p, true, 2);
    CHECK(st.generation() == 3);
    // a plain lock (no flags) in the same frame: draws recorded before keep the old copy, a new whole copy
    st.lock(8, 8, 0, 1024);
    p = st.plan(true, 2, 1024, false);
    CHECK(p.op == Op::NewCopy && p.lo == 0 && p.hi == 1024);
    st.committed(p, true, 2);
    CHECK(st.generation() == 4);
    // lock ranges clamp to the buffer
    st.lock(1000, 100, LOCK_NOOVERWRITE, 1024);
    p = st.plan(true, 2, 1024, false);
    CHECK(p.op == Op::Patch && p.lo == 1000 && p.hi == 1024);
}

TEST(identity_texture_lock_dirty) {
    // a write lock uploads the locked rect, whether or not the level was uploaded before
    CHECK(textureLockDirty(0, false) == LockDirty::Locked);
    CHECK(textureLockDirty(0, true) == LockDirty::Locked);
    CHECK(textureLockDirty(LOCK_DISCARD, true) == LockDirty::Locked);
    CHECK(textureLockDirty(LOCK_NOSYSLOCK, false) == LockDirty::Locked);
    // READONLY on a fresh level: D3D9 keeps it dirty as a whole until the first upload (eCGfxImageData::OnRead writes the
    // savegame thumbnail through such a lock)
    CHECK(textureLockDirty(LOCK_READONLY, false) == LockDirty::WholeLevel);
    CHECK(textureLockDirty(LOCK_READONLY | LOCK_NOSYSLOCK, false) == LockDirty::WholeLevel);
    // READONLY once the image holds data (uploaded or a blit destination): nothing
    CHECK(textureLockDirty(LOCK_READONLY, true) == LockDirty::None);
}

// ---- shader semantics ----

TEST(semantics_names) {
    CHECK(semanticOf("g_vViewProjection") == Semantic::ViewProjection);
    CHECK(semanticOf("g_CamPosAndFar") == Semantic::CamPosAndFar);
    CHECK(semanticOf("g_vCamPosAndFar") == Semantic::CamPosAndFar);
    CHECK(semanticOf("smpRefract") == Semantic::Refract);
    CHECK(semanticOf("nothing") == Semantic::Count);
    CHECK(std::string(name(Semantic::DirLight)) == "DirLight");
    CHECK(flagNames(ShaderSemantics::Material | ShaderSemantics::Skinned) == "Material Skinned");
}

TEST(semantics_vertex_shaders) {
    using U = rvk::shader::Usage;
    // a skinned material VS: bones in g_vInstanceData, BLENDINDICES input
    const auto skinned = shaderWith(0xFFFE0300u, "vs_3_0",
                                    {f("g_vViewProjection", 0, 4), f("g_vCamPosAndFar", 11, 1), f("g_vInstanceData", 32, 200)});
    auto sem = shaderSemantics(skinned, vsReflection({{U::Position, 0}, {U::BlendWeight, 0}, {U::BlendIndices, 0}, {U::TexCoord, 0}}));
    CHECK(sem.hasCtab);
    CHECK(sem[Semantic::ViewProjection].reg == 0 && sem[Semantic::ViewProjection].count == 4);
    CHECK(sem[Semantic::InstanceData].reg == 32 && sem[Semantic::InstanceData].count == 200);
    CHECK(sem[Semantic::CamPosAndFar].reg == 11);
    CHECK(!sem[Semantic::WindMatrix].present());
    CHECK(sem.has(ShaderSemantics::Material | ShaderSemantics::Skinned));
    CHECK(!sem.has(ShaderSemantics::Instanced));
    // SpeedTree: BLENDINDICES carry wind data, not bones
    const auto tree = shaderWith(0xFFFE0300u, "vs_3_0",
                                 {f("g_vViewProjection", 0, 4), f("g_vWindMatrix", 14, 9), f("g_vLeafAngles", 26, 4),
                                  f("g_vInstanceData", 32, 200)});
    sem = shaderSemantics(tree, vsReflection({{U::Position, 0}, {U::BlendIndices, 0}, {U::TexCoord, 4}}));
    CHECK(sem.has(ShaderSemantics::SpeedTree | ShaderSemantics::Leaves | ShaderSemantics::Instanced));
    CHECK(!sem.has(ShaderSemantics::Skinned));
    // no CTAB at all
    const std::vector<std::uint32_t> bare{0xFFFE0300u, 0x0000FFFFu};
    sem = shaderSemantics(bare, vsReflection({{U::Position, 0}}));
    CHECK(!sem.hasCtab && sem.flags == 0);
}

TEST(semantics_pixel_shaders) {
    const auto water = shaderWith(0xFFFF0300u, "ps_3_0",
                                  {f("g_CamPosAndFar", 0, 1), f("g_FogColor", 3, 1), f("g_AmbientLight", 15, 7), f("g_DirLight", 22, 3),
                                   s("smpShadowMask", 0, 1), s("smpRefract", 1, 1), s("smpDepthmap", 2, 1), s("g_SamplerData", 9, 4)});
    rvk::shader::Reflection r;
    r.version.stage = rvk::shader::Stage::Pixel;
    const auto sem = shaderSemantics(water, r);
    CHECK(sem[Semantic::Refract].reg == 1);
    CHECK(sem[Semantic::DepthMap].reg == 2);
    CHECK(sem[Semantic::SamplerData].reg == 9 && sem[Semantic::SamplerData].count == 4);
    CHECK(sem[Semantic::AmbientLight].reg == 15 && sem[Semantic::AmbientLight].count == 7);
    CHECK(sem.has(ShaderSemantics::Material | ShaderSemantics::Refracts | ShaderSemantics::ReadsDepth | ShaderSemantics::DirLight));
    CHECK(!sem.has(ShaderSemantics::Lightmapped));
    // a name in the wrong register set is ignored
    const auto odd = shaderWith(0xFFFF0300u, "ps_3_0", {f("smpRefract", 1, 1)});
    CHECK(!shaderSemantics(odd, r)[Semantic::Refract].present());
}

// ---- tags, classification, histogram ----

TEST(draws_classify) {
    using draws::Kind;
    using draws::Source;
    CHECK(draws::classify(Source::Mesh, 0x00000102u, View::Main, Pass::Solid) == Kind::StaticMesh);
    CHECK(draws::classify(Source::Mesh, 0xffffff00u, View::Main, Pass::Solid) == Kind::InstancedMesh);
    CHECK(draws::classify(Source::Mesh, 0x00000003u, View::Main, Pass::Refracted) == Kind::Refracted);
    CHECK(draws::classify(Source::Mesh, 0, View::Shadow, Pass::ShadowDepth) == Kind::StaticMesh);
    CHECK(draws::classify(Source::Skinned, 0, View::Main, Pass::Alpha) == Kind::Skinned);
    CHECK(draws::classify(Source::SpeedTree, 2, View::Main, Pass::Solid) == Kind::SpeedTree);
    CHECK(draws::classify(Source::Sky, 0, View::SkyCube, Pass::Sky) == Kind::Sky);
    CHECK(draws::classify(Source::Particle, 0, View::Main, Pass::Alpha) == Kind::Particle);
    CHECK(draws::classify(Source::Precipitation, 0, View::Main, Pass::AlphaLate) == Kind::Precipitation);
    CHECK(draws::classify(Source::None, 0, View::Gui, Pass::Gui) == Kind::Gui);
    CHECK(draws::classify(Source::None, 0, View::Post, Pass::Post) == Kind::Post);
    CHECK(draws::classify(Source::None, 0, View::Main, Pass::ShadowMask) == Kind::ShadowMask);
    CHECK(draws::classify(Source::None, 0, View::Main, Pass::Solid) == Kind::Unknown);
    CHECK(std::string(draws::name(Kind::SpeedTreeBillboard)) == "SpeedTreeBillboard");
    CHECK(std::string(draws::name(Source::Vegetation)) == "Vegetation");
}

TEST(draws_histogram_and_records) {
    draws::FrameDraws fd;
    fd.count(draws::Kind::StaticMesh, View::Main);
    fd.count(draws::Kind::StaticMesh, View::Main);
    fd.count(draws::Kind::StaticMesh, View::Shadow);
    fd.count(draws::Kind::Skinned, View::Main);
    fd.count(draws::Kind::Post, View::Post);
    fd.count(draws::Kind::Post, View::Post);
    draws::Record* r = fd.add();
    CHECK(r != nullptr);
    r->kind = draws::Kind::Skinned;
    CHECK(fd.current().size() == 1 && fd.last().empty());
    fd.endFrame();
    CHECK(fd.current().empty() && fd.last().size() == 1 && fd.last()[0].kind == draws::Kind::Skinned);
    const auto& h = fd.lastHistogram();
    CHECK(h.total == 6);
    CHECK(h.kind(draws::Kind::StaticMesh) == 3);
    CHECK_MSG(h.summary() == "StaticMesh 3 [Main 2, Shadow 1], Post 2 [Post 2], Skinned 1 [Main 1]", h.summary());
    // the next frame starts empty
    fd.endFrame();
    CHECK(fd.last().empty() && fd.lastHistogram().total == 0 && fd.lastHistogram().summary() == "(no draws)");
    // recording off: no records, the histogram still counts
    fd.setRecording(false);
    CHECK(fd.add() == nullptr);
    fd.count(draws::Kind::Gui, View::Gui);
    fd.endFrame();
    CHECK(fd.last().empty() && fd.lastHistogram().total == 1);
    // the cap: further draws are dropped and counted
    fd.setRecording(true);
    for (std::size_t i = 0; i < draws::FrameDraws::kMaxRecords + 5; ++i) fd.add();
    fd.endFrame();
    CHECK(fd.last().size() == draws::FrameDraws::kMaxRecords && fd.lastDropped() == 5);
}

TEST(draws_tint) {
    using draws::Kind;
    using draws::TintMode;
    CHECK(draws::tintColor(TintMode::Off, Kind::SpeedTree, View::Main, Pass::Solid) == 0);
    const std::uint32_t tree = draws::tintColor(TintMode::Kind, Kind::SpeedTree, View::Main, Pass::Solid);
    const std::uint32_t water = draws::tintColor(TintMode::Kind, Kind::Refracted, View::Main, Pass::Refracted);
    const std::uint32_t hero = draws::tintColor(TintMode::Kind, Kind::Skinned, View::Main, Pass::Solid);
    CHECK(tree != 0 && water != 0 && hero != 0 && tree != water && water != hero && tree != hero);
    CHECK((tree >> 24) != 0);  // a strength
    // data targets and the post chain / GUI are never tinted
    CHECK(draws::tintColor(TintMode::Kind, Kind::SpeedTree, View::Shadow, Pass::ShadowDepth) == 0);
    CHECK(draws::tintColor(TintMode::Kind, Kind::StaticMesh, View::Main, Pass::DepthPrepass) == 0);
    CHECK(draws::tintColor(TintMode::Kind, Kind::ShadowMask, View::Main, Pass::ShadowMask) == 0);
    CHECK(draws::tintColor(TintMode::Kind, Kind::Post, View::Post, Pass::Post) == 0);
    CHECK(draws::tintColor(TintMode::Kind, Kind::Gui, View::Gui, Pass::Gui) == 0);
    CHECK(draws::tintColor(TintMode::Kind, Kind::Sky, View::SkyCube, Pass::Sky) == 0);
    // by pass
    CHECK(draws::tintColor(TintMode::Pass, Kind::StaticMesh, View::Main, Pass::Refracted) ==
          draws::tintColor(TintMode::Pass, Kind::Skinned, View::Main, Pass::Refracted));
    CHECK(draws::tintColor(TintMode::Pass, Kind::StaticMesh, View::Main, Pass::Solid) !=
          draws::tintColor(TintMode::Pass, Kind::StaticMesh, View::Main, Pass::Alpha));
}

// the tint variant of a pixel shader: only with the option, and it compiles
TEST(draws_tint_epilogue) {
    // ps_3_0: mov oC0, c0
    const std::vector<std::uint32_t> tokens{0xFFFF0300u, 0x02000001u, 0x800F0800u, 0xA0E40000u, 0x0000FFFFu};
    const auto dec = rvk::shader::decode(tokens);
    CHECK(dec.ok());
    if (!dec.ok()) return;
    const auto plain = rvk::shader::toGlsl(dec.program);
    const auto tinted = rvk::shader::toGlsl(dec.program, rvk::shader::GlslOptions{.tintEpilogue = true});
    CHECK(plain.ok() && tinted.ok());
    CHECK(plain.glsl.find("unpackUnorm4x8") == std::string::npos);
    CHECK(tinted.glsl.find("pc.draw.pad0 != 0u") != std::string::npos);
    CHECK(rvk::shader::compileGlsl(rvk::shader::Stage::Pixel, plain.glsl).ok());
    CHECK(rvk::shader::compileGlsl(rvk::shader::Stage::Pixel, tinted.glsl).ok());
    // a vertex shader ignores the option
    const std::vector<std::uint32_t> vs{0xFFFE0300u, 0x0200001Fu, 0x80000000u, 0x900F0000u, 0x0200001Fu, 0x80000000u, 0xE00F0000u,
                                        0x02000001u, 0xE00F0000u, 0x90E40000u, 0x0000FFFFu};
    const auto vdec = rvk::shader::decode(vs);
    CHECK(vdec.ok());
    if (vdec.ok()) {
        CHECK(rvk::shader::toGlsl(vdec.program).glsl == rvk::shader::toGlsl(vdec.program, {.tintEpilogue = true}).glsl);
    }
}

TEST(draws_classify_water) {
    using draws::Water;
    CHECK(draws::classifyWater("Special_Water_Ocean_01_Diffuse_01") == Water::Ocean);
    CHECK(draws::classifyWater("Special_Water_Island_01_Diffuse_01") == Water::Lake);
    CHECK(draws::classifyWater("Special_Water_Swamp_01_Diffuse_01") == Water::Swamp);
    CHECK(draws::classifyWater("Special_Water_River_01_Diffuse_01") == Water::River);
    CHECK(draws::classifyWater("Special_Water_Fall_01_Diffuse_01") == Water::Waterfall);
    CHECK(draws::classifyWater("Special_Water_Falls_01_Diffuse_01") == Water::Waterfall);
    CHECK(draws::classifyWater("Special_Water_Shore_02_Diffuse_01") == Water::Shore);
    CHECK(draws::classifyWater("Arch_Obj_Bathtub_Water_01_Diffuse_01") == Water::Other);
    CHECK(draws::classifyWater("Nat_Obj_Crystal_01_Diffuse_02") == Water::None);
    CHECK(draws::classifyWater("Nat_Sea_Bottom_01_Diffuse_01") == Water::None);
    CHECK(draws::classifyWater("") == Water::None);
    CHECK(std::string(draws::name(Water::Shore)) == "Shore");
}
