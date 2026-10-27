// The native frame's op tap (N5.0, docs/research/engine-frame/README.md): the engine's sort rules, op lookup, attribution.

#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "native/frame_desc.h"
#include "test.h"

using namespace rvk;
using native::Attribution;
using native::DrawInfo;
using native::FrameDesc;
using native::List;
using native::Op;
using native::OpKind;
using native::RawOp;
using Kind = d3d9::draws::Kind;
using Pass = d3d9::scene::Pass;

namespace {

RawOp raw(std::uint32_t key, std::uint32_t sortId, std::uint32_t desc = 0, const void* ctx = nullptr) {
    RawOp r;
    r.context = ctx;
    r.desc = desc;
    r.key = key;
    r.sortId = sortId;
    return r;
}

int cmpZ(const void* a, const void* b) { return native::compareZ(*static_cast<const RawOp*>(a), *static_cast<const RawOp*>(b)); }
int cmpSolid(const void* a, const void* b) { return native::compareSolid(*static_cast<const RawOp*>(a), *static_cast<const RawOp*>(b)); }
int cmpAlpha(const void* a, const void* b) { return native::compareAlpha(*static_cast<const RawOp*>(a), *static_cast<const RawOp*>(b)); }

// a FrameDesc over "live" engine arrays (one vector per list): ops in the arrays' order
struct Fixture {
    std::array<std::vector<RawOp>, native::kListCount> live;
    FrameDesc desc;
    void build() {
        desc.clear();
        for (std::size_t l = 0; l < native::kListCount; ++l) {
            desc.first[l] = static_cast<std::uint32_t>(desc.ops.size());
            desc.count[l] = static_cast<std::uint32_t>(live[l].size());
            desc.liveData[l] = live[l].empty() ? nullptr : reinterpret_cast<const std::uint8_t*>(live[l].data());
            for (std::uint32_t i = 0; i < live[l].size(); ++i) {
                Op op;
                op.raw = live[l][i];
                op.list = static_cast<List>(l);
                op.index = i;
                op.kind = OpKind::Mesh;
                desc.ops.push_back(op);
            }
        }
        desc.valid = true;
    }
    std::array<std::uint32_t, native::kListCount> counts() const {
        std::array<std::uint32_t, native::kListCount> c{};
        for (std::size_t l = 0; l < native::kListCount; ++l) c[l] = static_cast<std::uint32_t>(live[l].size());
        return c;
    }
    const RawOp* at(List l, std::size_t i) const { return &live[static_cast<std::size_t>(l)][i]; }
};

DrawInfo drawOf(const void* op, Pass pass, std::uint32_t index, Kind kind = Kind::StaticMesh) {
    DrawInfo d;
    d.op = op;
    d.pass = pass;
    d.index = index;
    d.kind = kind;
    return d;
}

}  // namespace

TEST(native_comparators_follow_the_engine) {
    // z: front to back by the 24-bit key
    std::vector<RawOp> z{raw(30, 0), raw(10, 5), raw(20, 1)};
    std::qsort(z.data(), z.size(), sizeof(RawOp), cmpZ);
    CHECK(z[0].key == 10 && z[1].key == 20 && z[2].key == 30);
    // solid: sort id (priority, additional id, material id) first, then the key
    std::vector<RawOp> s{raw(5, 2 << 20), raw(9, 1 << 20 | 7), raw(1, 1 << 20 | 7), raw(3, 1 << 20 | 3)};
    std::qsort(s.data(), s.size(), sizeof(RawOp), cmpSolid);
    CHECK(s[0].sortId == (1u << 20 | 3) && s[1].key == 1 && s[2].key == 9 && s[3].sortId == 2u << 20);
    // alpha: ordinary ops back to front, ties by sort id; a priority >= 1000 band first, the higher band first
    std::vector<RawOp> a{raw(10, 5), raw(50, 7), raw(50, 6), raw(1, 1000u << 20), raw(2, 1001u << 20), raw(99, 999u << 20)};
    std::qsort(a.data(), a.size(), sizeof(RawOp), cmpAlpha);
    CHECK(a[0].sortId == 1001u << 20 && a[1].sortId == 1000u << 20);
    CHECK(a[2].key == 99 && a[3].key == 50 && a[3].sortId == 6 && a[4].sortId == 7 && a[5].key == 10);
    CHECK(native::compareAlpha(raw(1, 3), raw(1, 3)) == 0 && native::compareSolid(raw(1, 3), raw(1, 3)) == 0);
}

TEST(native_lists_and_find) {
    CHECK(native::listPass(List::ZPass) == Pass::DepthPrepass && native::listPass(List::Sky) == Pass::Solid &&
          native::listPass(List::Refracted) == Pass::Refracted && native::listPass(List::Late) == Pass::AlphaLate);
    CHECK(native::kListSlot[static_cast<std::size_t>(List::ZPass)] == 3 && native::kListSlot[static_cast<std::size_t>(List::Alpha)] == 1);
    Fixture f;
    f.live[static_cast<std::size_t>(List::Solid)] = {raw(1, 1), raw(2, 2), raw(3, 3)};
    f.live[static_cast<std::size_t>(List::Alpha)] = {raw(4, 4)};
    f.build();
    const Op* op = f.desc.find(f.at(List::Solid, 2));
    CHECK(op != nullptr && op->list == List::Solid && op->index == 2 && op->raw.key == 3);
    CHECK(f.desc.find(f.at(List::Alpha, 0)) == &f.desc.ops[3]);
    // inside an array but not at an op's start, or outside every array
    CHECK(f.desc.find(reinterpret_cast<const std::uint8_t*>(f.at(List::Solid, 1)) + 4) == nullptr);
    const RawOp other{};
    CHECK(f.desc.find(&other) == nullptr);
}

TEST(native_attribution_in_order_is_clean) {
    Fixture f;
    f.live[static_cast<std::size_t>(List::ZPass)] = {raw(1, 0, 0xff)};
    f.live[static_cast<std::size_t>(List::Solid)] = {raw(1, 1), raw(2, 2, 0xffffff00)};
    f.live[static_cast<std::size_t>(List::Refracted)] = {raw(5, 5)};
    f.build();
    f.desc.ops[3].emptySlot = true;  // e.g. a skinned op in the refracted list (an empty slot)
    Attribution a;
    a.begin(&f.desc);
    a.enterOp(f.at(List::ZPass, 0), f.at(List::ZPass, 0));
    a.draw(drawOf(f.at(List::ZPass, 0), Pass::DepthPrepass, 0));
    a.draw(drawOf(f.at(List::ZPass, 0), Pass::DepthPrepass, 1));  // a masked sub-mesh alone: whole-mesh z op, 2 draws
    a.draw(drawOf(nullptr, Pass::ShadowMask, 2, Kind::ShadowMask));
    a.enterOp(f.at(List::Solid, 0), f.at(List::Solid, 0));
    DrawInfo base = drawOf(f.at(List::Solid, 0), Pass::Solid, 3);
    a.draw(base);
    base.additive = true;
    base.index = 4;
    a.draw(base);  // a second light pass
    a.enterOp(f.at(List::Solid, 1), f.at(List::Solid, 1));
    DrawInfo inst = drawOf(f.at(List::Solid, 1), Pass::Solid, 5, Kind::InstancedMesh);
    inst.instanced = true;
    a.draw(inst);
    inst.index = 6;
    a.draw(inst);  // two instance batches
    a.draw(drawOf(nullptr, Pass::Solid, 7, Kind::Decal));
    a.enterOp(f.at(List::Refracted, 0), f.at(List::Refracted, 0));
    a.end(f.counts());
    const auto& c = a.counts();
    CHECK(c.entered == 4 && c.opsTotal == 4 && c.matched == 6 && c.noOpTotal == 2 && c.draws == 8);
    CHECK(c.outOfOrder == 0 && c.contentMismatch == 0 && c.notEntered == 0 && c.drawOrder == 0 && c.passMismatch == 0);
    CHECK(c.drawsByPass[static_cast<std::size_t>(Pass::DepthPrepass)] == 2 && c.drawsByPass[static_cast<std::size_t>(Pass::Solid)] == 5);
    CHECK(f.desc.ops[0].notes & native::NoteWholeMesh);
    CHECK(f.desc.ops[1].notes & native::NoteLightPasses);
    CHECK(f.desc.ops[2].notes & native::NoteInstanced);
    CHECK(f.desc.ops[3].notes & native::NoteEmptySlot);
    const auto& solid = c.buckets[static_cast<std::size_t>(List::Solid)][static_cast<std::size_t>(OpKind::Mesh)];
    CHECK(solid.ops == 2 && solid.draws == 4 && solid.multi == 2 && solid.multiLights == 1 && solid.multiInstanced == 1 && solid.maxDraws == 2);
    CHECK(c.clean());
    CHECK(a.mismatches().empty());
    CHECK(native::noteNames(native::NoteInstanced | native::NoteLightPasses) == "instanced+light passes" && native::noteNames(0) == "-");
    const auto lines = native::reportLines(c, 7, c, a.mismatches());
    CHECK(lines.size() == 3 && lines[0].find("clean") != std::string::npos && lines[2].find("Solid/Mesh 2 ops 4 draws") != std::string::npos);
}

TEST(native_attribution_finds_mismatches) {
    Fixture f;
    f.live[static_cast<std::size_t>(List::Solid)] = {raw(1, 1), raw(2, 2), raw(3, 3)};
    f.build();
    Attribution a;
    a.begin(&f.desc);
    // op 1 before op 0; the live bytes of op 0 differ from the sorted copy; op 2 never runs; an unexplained draw without op
    a.enterOp(f.at(List::Solid, 1), f.at(List::Solid, 1));
    RawOp changed = *f.at(List::Solid, 0);
    changed.key = 77;
    a.enterOp(f.at(List::Solid, 0), &changed);
    a.draw(drawOf(f.at(List::Solid, 1), Pass::Solid, 0));  // op 1's draw while op 0 executes
    a.draw(drawOf(nullptr, Pass::Solid, 1, Kind::StaticMesh));
    const RawOp stranger{};
    a.enterOp(&stranger, nullptr);
    a.end(f.counts());
    const auto& c = a.counts();
    CHECK(c.outOfOrder == 2 && c.contentMismatch == 1 && c.notEntered == 1 && c.drawOrder == 1 && c.unknownOps == 1);
    CHECK(c.noOp[static_cast<std::size_t>(Kind::StaticMesh)] == 1);
    CHECK(!c.clean());
    CHECK(!a.mismatches().empty());
}

TEST(native_attribution_without_z_prepass) {
    Fixture f;
    f.live[static_cast<std::size_t>(List::ZPass)] = {raw(1, 0)};
    f.live[static_cast<std::size_t>(List::Solid)] = {raw(1, 1)};
    f.build();
    f.desc.zPrepass = false;  // caps 0x1590 off: Execute skips the z list
    Attribution a;
    a.begin(&f.desc);
    a.enterOp(f.at(List::Solid, 0), f.at(List::Solid, 0));
    a.draw(drawOf(f.at(List::Solid, 0), Pass::Solid, 0));
    // the list grew during Execute: counted
    auto counts = f.counts();
    counts[static_cast<std::size_t>(List::Solid)] = 2;
    a.end(counts);
    CHECK(a.counts().outOfOrder == 0 && a.counts().notEntered == 0 && a.counts().countMismatch == 1);
    {
        native::TapCounts sum;
        sum.add(a.counts());
        sum.add(a.counts());
        CHECK(sum.views == 2 && sum.matched == 2 && sum.countMismatch == 2);
    }
}

TEST(native_attribution_context_rules) {
    Fixture f;
    f.live[static_cast<std::size_t>(List::Solid)] = {raw(0, 1), raw(0, 2)};
    f.live[static_cast<std::size_t>(List::Alpha)] = {raw(9, 3), raw(8, 4), raw(7, 5)};
    f.build();
    f.desc.ops[0].kind = OpKind::SpeedTreeBB;
    f.desc.ops[1].kind = OpKind::Vegetation;
    f.desc.ops[2].kind = OpKind::Strip;
    f.desc.ops[2].state = 1;  // one segment: RenderAlpha draws nothing
    f.desc.ops[3].kind = OpKind::Particle;
    f.desc.ops[3].state = 0;  // no live particle, yet it draws below: a contradiction
    Attribution a;
    a.begin(&f.desc);
    a.enterOp(f.at(List::Solid, 0), f.at(List::Solid, 0));
    DrawInfo bb = drawOf(f.at(List::Solid, 0), Pass::Solid, 0, Kind::SpeedTreeBillboard);
    bb.vsHash = 1;
    a.draw(bb);
    bb.vsHash = 2;
    bb.index = 1;
    a.draw(bb);  // the same pass with the second billboard VS variant
    a.enterOp(f.at(List::Solid, 1), f.at(List::Solid, 1));
    DrawInfo grass = drawOf(f.at(List::Solid, 1), Pass::Solid, 2, Kind::Vegetation);
    grass.instanced = true;
    a.draw(grass);
    grass.index = 3;
    a.draw(grass);
    a.enterOp(f.at(List::Alpha, 0), f.at(List::Alpha, 0));
    a.enterOp(f.at(List::Alpha, 1), f.at(List::Alpha, 1));
    a.draw(drawOf(f.at(List::Alpha, 1), Pass::Alpha, 4, Kind::Particle));
    a.enterOp(f.at(List::Alpha, 2), f.at(List::Alpha, 2));
    DrawInfo zfill = drawOf(f.at(List::Alpha, 2), Pass::Alpha, 5);
    zfill.colorWrite = false;
    a.draw(zfill);
    a.draw(drawOf(f.at(List::Alpha, 2), Pass::Alpha, 6));  // the z-fill pass, then the colour pass
    a.end(f.counts());
    CHECK(f.desc.ops[0].notes & native::NoteBillboardPair);
    CHECK(f.desc.ops[1].notes & native::NoteGrassBatches);
    CHECK(f.desc.ops[2].notes & native::NoteNothingToDraw);
    CHECK(f.desc.ops[4].notes & native::NoteZFill);
    CHECK(a.counts().stateContradiction == 1 && !a.counts().clean());
    const auto& strip = a.counts().buckets[static_cast<std::size_t>(List::Alpha)][static_cast<std::size_t>(OpKind::Strip)];
    CHECK(strip.zero == 1 && strip.zeroNothing == 1 && strip.zeroOther == 0);
}

// ---- N5.1 static ops: the engine's composition rules (native/static_compose) ----------------------------------------------

#include "native/static_compose.h"

namespace sc = rvk::native::sc;

TEST(static_passes_follow_get_passes) {
    int illum = 0;
    // no illumination: one base pass with the lightmap combi
    std::vector<sc::Pass> p;
    sc::PassInputs in;
    in.combi = 1;
    CHECK(sc::buildPasses(in, p) == 1 && p[0].flags == (1u << 9 | 1u << 4) && p[0].illum == nullptr);
    // 4 lights (2 static + 2 dynamic), sun: a base pass with 3 lights + sun + lightmap, an additive pass with 1
    p.clear();
    in.illum = &illum;
    in.hasList = true;
    in.listSun = 1;
    in.staticLights = 2;
    in.dynamicLights = 2;
    CHECK(sc::buildPasses(in, p) == 2);
    CHECK(sc::passSun(p[0].flags) && sc::passLights(p[0].flags) == 3 && sc::passCombi(p[0].flags) == 1 && sc::passKind(p[0].flags) == 0);
    CHECK(!sc::passSun(p[1].flags) && sc::passLights(p[1].flags) == 1 && sc::passCombi(p[1].flags) == 0 && sc::passKind(p[1].flags) == 1);
    CHECK(p[0].firstLight == 0 && p[1].firstLight == 3);
    // the entity's "no sun" byte, instanced, a z-fill pass first
    p.clear();
    in.illumNoSun = 1;
    in.instanced = true;
    in.zFill = true;
    in.staticLights = in.dynamicLights = 0;
    CHECK(sc::buildPasses(in, p) == 2 && p[0].flags == 0x80u && p[1].flags == (1u << 4));
    // permutation indices (FUN_1806cc610)
    CHECK(sc::vsIndex(1u << 9) == 3 && sc::vsIndex(0) == 0);
    CHECK(sc::psIndex(1u | 2u << 2 | 1u << 4) == 2 + ((1 + 2) * 3 + 0) * 4);
    CHECK(sc::psIndex(1u << 6 | 1u << 2) == 1 + 4);
}

TEST(static_blend_states_per_mode) {
    sc::BlendInputs b;
    // masked at fade 1: no blending, depth EQUAL, the alpha test word with the mask reference
    b.mode = 1;
    b.maskRef = 128;
    sc::RenderStates r = sc::passStates(1u << 9, b, 1.0f);
    CHECK(!r.blend && r.depthFunc == 3 && r.alphaTest == 2 && r.alphaRef == 128 && r.colorWrite && !r.alphaWrite && r.depthWriteMaster == 0);
    // masked fading: blended, LESSEQUAL, the reference scaled
    r = sc::passStates(1u << 9, b, 0.5f);
    CHECK(r.blend && r.src == 5 && r.dst == 6 && r.depthFunc == 4 && r.alphaRef == 64);
    // instanced passes ignore the fade (bit 9 clear)
    r = sc::passStates(0, b, 0.5f);
    CHECK(!r.blend && r.depthFunc == 3);
    // normal opaque / additive light pass
    b.mode = 0;
    r = sc::passStates(1u << 9, b, 1.0f);
    CHECK(!r.blend && r.src == 2 && r.dst == 1 && r.depthFunc == 4);
    r = sc::passStates(1u << 9 | 1u << 6, b, 1.0f);
    CHECK(r.blend && r.src == 2 && r.dst == 2);
    // an opacity node: blended, depth write off
    b.hasOpacity = true;
    r = sc::passStates(1u << 9, b, 1.0f);
    CHECK(r.blend && r.src == 5 && r.dst == 6 && r.depthWriteMaster == 1);
    // modulate
    b = {};
    b.mode = 3;
    r = sc::passStates(1u << 9, b, 1.0f);
    CHECK(r.blend && r.src == 9 && r.dst == 3 && r.depthWriteMaster == 1);
    // z-fill
    r = sc::passStates(1u << 9 | 2u << 6, b, 1.0f);
    CHECK(!r.blend && !r.colorWrite && r.depthWriteMaster == 2);
    // device states: the visual's depth write when no master, colour bits, fill from the admin's master mode
    const sc::Visual v{3, 2, 4, 2};
    auto s = sc::deviceStates(sc::zPassStates(true, true), v, 0);
    CHECK(s[sc::StZFunc] == 4 && s[sc::StZWrite] == 1 && s[sc::StColorWrite] == 0xF && s[sc::StBlend] == 0 && s[sc::StCull] == 2 && s[sc::StFill] == 3);
    b = {};
    s = sc::deviceStates(sc::passStates(1u << 9, b, 1.0f), v, 2);
    CHECK(s[sc::StZWrite] == 1 && s[sc::StColorWrite] == 7 && s[sc::StFill] == 2);
}

TEST(static_whole_mesh_z_merges) {
    // a, b plain (merge), c masked (alone), d blended (skipped, ends the run), e plain
    std::vector<sc::ZSub> subs(5);
    for (std::uint32_t i = 0; i < 5; ++i) subs[i] = {true, false, false, 10 * i, 10, 30 * i, 30};
    subs[2].masked = true;
    subs[3].blended = true;
    std::vector<sc::ZDraw> plan;
    sc::planWholeMeshZ(subs, plan);
    CHECK(plan.size() == 3);
    CHECK(plan[0].startIndex == 0 && plan[0].primCount == 20 && plan[0].numVertices == 20 && plan[0].shaderOf == 1);
    CHECK(plan[1].startIndex == 60 && plan[1].primCount == 10 && plan[1].shaderOf == 2);
    CHECK(plan[2].startIndex == 120 && plan[2].primCount == 10 && plan[2].shaderOf == 4);
    // a sub-mesh without a ready material is passed over without ending the run
    subs = {{true, false, false, 0, 4, 0, 6}, {false}, {true, false, false, 4, 4, 6, 6}};
    plan.clear();
    sc::planWholeMeshZ(subs, plan);
    CHECK(plan.size() == 1 && plan[0].primCount == 4 && plan[0].numVertices == 8 && plan[0].shaderOf == 2);
}

TEST(static_instance_batches) {
    int listA = 0, listB = 0;
    std::vector<sc::Instance> in(7);
    for (auto& i : in) i.list = &listA;
    in[3].list = &listB;  // a new light list breaks the batch
    in[5].alone = true;   // static lights: ends its batch after it
    std::vector<sc::Batch> out;
    sc::planBatches(in, 50, out);
    CHECK(out.size() == 4);
    CHECK(out[0].first == 0 && out[0].count == 3);
    CHECK(out[1].first == 3 && out[1].count == 1);
    CHECK(out[2].first == 4 && out[2].count == 2);
    CHECK(out[3].first == 6 && out[3].count == 1);
    // the batch limit
    std::vector<sc::Instance> many(120);
    for (auto& i : many) i.list = &listA;
    out.clear();
    sc::planBatches(many, 50, out);
    CHECK(out.size() == 3 && out[0].count == 50 && out[1].count == 50 && out[2].first == 100 && out[2].count == 20);
}

TEST(static_switch_and_constants) {
    CHECK(sc::constantSwitchIndex(5, 3, 0) == 2);
    CHECK(sc::constantSwitchIndex(5, 3, 1) == 2 && sc::constantSwitchIndex(-2, 3, 1) == 0);
    CHECK(sc::constantSwitchIndex(3, 3, 2) == 1);  // ping-pong 0 1 2 1 0 ...
    CHECK(sc::samplerSwitchIndex(7, 2, 4, 0, 2) == 1 && sc::samplerSwitchIndex(7, 2, 4, 0, 1) == -1);
    float c3[4], c4[4];
    sc::fogConstants(true, 0xff204060u, 100.0f, 300.0f, 2.0f, c3, c4);
    CHECK(c3[0] == 0x20 / 255.0f && c3[1] == 0x40 / 255.0f && c3[2] == 0x60 / 255.0f && c3[3] == 0.0f);
    CHECK(c4[1] == 1.0f / (200.0f / 2.0f) && c4[0] == -(c4[1] * 100.0f) && c4[2] == 2.0f && c4[3] == 0.0f);
    sc::fogConstants(false, 0xff204060u, 100.0f, 300.0f, 2.0f, c3, c4);
    CHECK(c3[0] == 0.0f && c4[1] == 0.0f);
    float c1[4];
    sc::viewToScreen(1600, 900, c1);
    CHECK(c1[0] == 0.5f && c1[1] == -0.5f && c1[2] == 0.5f / 1600.0f + 0.5f);
    float sh[7][4]{}, out[7][4]{};
    const float indoor[3] = {0.5f, 0.25f, 1.0f};
    sh[0][3] = 2.0f;
    sc::ambientSh(sh, indoor, 0.75f, nullptr, out);
    CHECK(out[0][3] == 0.25f * 0.5f + 1.5f && out[2][3] == 0.25f * 1.0f);
}

TEST(static_compare_counts_fields) {
    auto a = std::make_unique<sc::DrawDesc>();
    auto e = std::make_unique<sc::DrawDesc>();
    e->vsLive.set(33);
    a->vsSrc[33] = sc::Src::Native;
    a->vsF[33][0] = e->vsF[33][0] = 1.0f;
    CHECK(sc::compare(*a, *e).fields.none());
    a->vsF[33][1] = 2.0f;
    auto d = sc::compare(*a, *e);
    CHECK(d.fields.test(static_cast<std::size_t>(sc::Field::VsInstance)) && !d.first.empty());
    a->vsF[33][1] = 0.0f;
    e->psLive.set(15);
    d = sc::compare(*a, *e);
    CHECK(d.fields.test(static_cast<std::size_t>(sc::Field::UnownedConstant)));
    // sampler states only for the captured draw's sampled stages, blend factors only while blending
    a->psSrc[15] = sc::Src::Native;
    a->samplers[3].texture = &d;
    a->states[sc::StSrc] = 5;
    CHECK(sc::compare(*a, *e).fields.none());
    e->samplerMask = 1u << 3;
    CHECK(sc::compare(*a, *e).fields.test(static_cast<std::size_t>(sc::Field::Texture)));
}

TEST(scene_compare_bool_int_tag_and_classes) {
    auto a = std::make_unique<sc::DrawDesc>();
    auto e = std::make_unique<sc::DrawDesc>();
    auto d = sc::compare(*a, *e);
    CHECK(d.fields.none() && d.biRegs == 64);
    e->psB[3] = 1;
    CHECK(sc::compare(*a, *e).fields.test(static_cast<std::size_t>(sc::Field::PsBool)));
    e->psB[3] = 0;
    e->vsI[2][1] = 7;
    CHECK(sc::compare(*a, *e).fields.test(static_cast<std::size_t>(sc::Field::VsInt)));
    e->vsI[2][1] = 0;
    // the water classification is part of the tag (the enhanced water's DrawReplacer decides by it)
    e->tagWater = 2;
    CHECK(sc::compare(*a, *e).fields.test(static_cast<std::size_t>(sc::Field::Tag)));
    a->tagWater = 2;
    CHECK(sc::compare(*a, *e).fields.none());
    // draw classes: kind 0 base, 1 additive light pass, 2 z-fill; overlays win
    CHECK(sc::passClass(0x200, false) == sc::DrawClass::Base);
    CHECK(sc::passClass(0x200 | 0x40 | (3u << 2), false) == sc::DrawClass::Additive);
    CHECK(sc::passClass(0x280, false) == sc::DrawClass::ZFill);
    CHECK(sc::passClass(0x40, true) == sc::DrawClass::Overlay);
    sc::CompareCounts c;
    c.classDraws[static_cast<std::size_t>(sc::DrawClass::Additive)] = 3;
    c.classClean[static_cast<std::size_t>(sc::DrawClass::Additive)] = 3;
    c.listDraws[5] = 3;
    c.listClean[5] = 3;
    static constexpr const char* kNames[] = {"z", "solid", "debug", "sky", "refracted", "alpha", "late"};
    const std::string s = sc::summary(c, kNames, std::size(kNames));
    CHECK(s.find("additive light pass 3/3") != std::string::npos && s.find("alpha 3/3") != std::string::npos);
}

TEST(scene_alpha_pass_states) {
    // an alpha-list op of a blended material with EnableAlphaZWrite: the z-fill pass (kind 2) first, then the colour pass;
    // the base pass blends with the material's mode, depth write off
    sc::PassInputs in;
    in.zFill = true;
    std::vector<sc::Pass> p;
    sc::buildPasses(in, p);
    CHECK(p.size() == 2 && sc::passKind(p[0].flags) == 2 && sc::passKind(p[1].flags) == 0);
    sc::BlendInputs b;
    b.mode = 7;  // Brighten
    const auto z = sc::deviceStates(sc::passStates(p[0].flags, b, 0.5f), sc::Visual{}, 0);
    CHECK(z[sc::StColorWrite] == 0 && z[sc::StZWrite] == 1 && z[sc::StBlend] == 0);
    const auto c = sc::deviceStates(sc::passStates(p[1].flags, b, 0.5f), sc::Visual{}, 0);
    CHECK(c[sc::StBlend] == 1 && c[sc::StSrc] == 5 && c[sc::StDst] == 2 && c[sc::StZWrite] == 0 && c[sc::StColorWrite] == 7);
    // a fading opaque material (mode 0, fade < 1): blended SrcAlpha / InvSrcAlpha, depth write as the visual's (not IsBlended)
    sc::BlendInputs o;
    const auto f = sc::deviceStates(sc::passStates(0x200, o, 0.5f), sc::Visual{}, 0);
    CHECK(f[sc::StBlend] == 1 && f[sc::StSrc] == 5 && f[sc::StDst] == 6 && f[sc::StZWrite] == 1);
    // a refracted material (water): unblended, depth write on
    sc::BlendInputs w;
    w.refracted = true;
    const auto r = sc::deviceStates(sc::passStates(0x200, w, 1.0f), sc::Visual{}, 0);
    CHECK(r[sc::StBlend] == 0 && r[sc::StZWrite] == 1);
}

TEST(native_refraction_copy_place) {
    native::FrameDesc f;
    f.first = {0, 4, 9, 9, 10, 12, 20};
    CHECK(native::refractionCopyInPlace(f, 10));
    CHECK(!native::refractionCopyInPlace(f, 9));
    CHECK(!native::refractionCopyInPlace(f, 11));
}

// ---- N5.2c: the sky dome op's billboards and the opaque billboards' queue -------------------------------------------------

TEST(sky_billboard_vertices_follow_fill_vertex_buffer) {
    // one billboard: position (1, 2, 3), s = (2, 0), r = (0, 3), colour 0x80808080, uv rect (0.25, 0.5, 0.75, 1)
    std::uint8_t bb[sc::kSkyBillboardSize]{};
    const float pos[3] = {1, 2, 3}, axes[4] = {2, 0, 0, 3}, uv[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    const std::uint32_t colour = 0x80808080u;
    std::memcpy(bb, pos, 12);
    std::memcpy(bb + 0xc, axes, 16);
    std::memcpy(bb + 0x1c, &colour, 4);
    std::memcpy(bb + 0x20, uv, 16);
    std::vector<std::uint8_t> v;
    sc::skyBillboardVertices(bb, 1, v);
    CHECK(v.size() == 4 * sc::kSkyVertexStride);
    const auto f = [&](std::uint32_t vertex, std::uint32_t off) {
        float x = 0;
        std::memcpy(&x, v.data() + vertex * sc::kSkyVertexStride + off, 4);
        return x;
    };
    // corners -s + r, s + r, s - r, -s - r; uv (u0 v0), (u1 v0), (u1 v1), (u0 v1)
    CHECK(f(0, 0x18) == -2 && f(0, 0x1c) == 3 && f(1, 0x18) == 2 && f(1, 0x1c) == 3);
    CHECK(f(2, 0x18) == 2 && f(2, 0x1c) == -3 && f(3, 0x18) == -2 && f(3, 0x1c) == -3);
    CHECK(f(0, 0x10) == 0.25f && f(0, 0x14) == 0.5f && f(1, 0x10) == 0.75f && f(2, 0x14) == 1.0f && f(3, 0x10) == 0.25f);
    for (std::uint32_t k = 0; k < 4; ++k) {
        std::uint32_t c = 0;
        std::memcpy(&c, v.data() + k * sc::kSkyVertexStride + 0xc, 4);
        CHECK(f(k, 0) == 1 && f(k, 4) == 2 && f(k, 8) == 3 && c == colour);
    }
    std::vector<std::uint8_t> idx;
    sc::skyQuadIndices(2, idx);
    std::uint16_t q[12];
    std::memcpy(q, idx.data(), sizeof(q));
    CHECK(idx.size() == 24 && q[0] == 0 && q[2] == 2 && q[3] == 0 && q[5] == 3 && q[6] == 4 && q[11] == 7);
}

TEST(sky_dome_states) {
    // the dome's BlendMode 4 (One / InvSrcAlpha): blended, depth write off, colour without alpha, LESSEQUAL
    sc::BlendInputs b;
    b.mode = 4;
    const auto s = sc::deviceStates(sc::skyDomeStates(b, 1.0f), sc::Visual{3, 1, 0, 1}, 0);
    CHECK(s[sc::StBlend] == 1 && s[sc::StSrc] == 2 && s[sc::StDst] == 6 && s[sc::StZWrite] == 0 && s[sc::StZFunc] == 4);
    CHECK(s[sc::StColorWrite] == 7 && s[sc::StCull] == 1 && s[sc::StAlphaTest] == 0);
    // opaque: the visual's depth write
    sc::BlendInputs o;
    const auto t = sc::deviceStates(sc::skyDomeStates(o, 1.0f), sc::Visual{3, 1, 0, 2}, 0);
    CHECK(t[sc::StBlend] == 0 && t[sc::StZWrite] == 1);
}

TEST(billboard_queue_records_and_cycles) {
    int a = 0, b = 0;
    std::vector<sc::BillboardEntry> e(5);
    e[0] = {&a, nullptr, nullptr, 0, 0, 3};
    e[1] = {&a, nullptr, nullptr, 0, 0, 2};    // same keys: the same record
    e[2] = {&b, nullptr, nullptr, 0, 0, 1};    // another material: a new record
    e[3] = {&b, nullptr, nullptr, 0, 0, 0};    // no quads: nothing
    e[4] = {&b, nullptr, nullptr, 0, 0, 995};  // 6 + 995 > 1000: the cycle's records drawn first, a new cycle at quad 0
    std::vector<sc::BillboardRecord> r;
    std::vector<sc::BillboardPlace> p;
    sc::planBillboards(e, 1000, r, p);
    CHECK(r.size() == 3);
    CHECK(r[0].cycle == 0 && r[0].firstQuad == 0 && r[0].quads == 5 && r[0].keys.material == &a);
    CHECK(r[1].cycle == 0 && r[1].firstQuad == 5 && r[1].quads == 1);
    CHECK(r[2].cycle == 1 && r[2].firstQuad == 0 && r[2].quads == 995);
    CHECK(p[1].cycle == 0 && p[1].quad == 3 && p[3].cycle == -1 && p[4].cycle == 1 && p[4].quad == 0);
    // a light set / switch / blend change also starts a record
    e[1].switchValue = 2;
    sc::planBillboards(e, 1000, r, p);
    CHECK(r.size() == 4 && r[1].firstQuad == 3 && r[1].quads == 2);
    std::vector<std::uint8_t> idx;
    sc::billboardQuadIndices(1, 1, idx);
    std::uint16_t q[6];
    std::memcpy(q, idx.data(), sizeof(q));
    CHECK(q[0] == 4 && q[1] == 5 && q[2] == 7 && q[3] == 5 && q[4] == 6 && q[5] == 7);
}
