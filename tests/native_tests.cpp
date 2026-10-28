// The native frame's op tap (N5.0, docs/research/engine-frame/README.md): the engine's sort rules, op lookup, attribution.

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "native/coverage.h"
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

TEST(static_lean_check_frames_and_touch_set) {
    using namespace rvk::native;
    // native.lean_check: 0 = never a check frame; N = every N-th frame (frame 0 included), the frame before it a warm-up frame
    using LF = sc::LeanFrame;
    CHECK(sc::leanFrameKind(0, 0) == LF::Lean && sc::leanFrameKind(7, 0) == LF::Lean && sc::leanFrameKind(5, -3) == LF::Lean);
    CHECK(sc::leanFrameKind(0, 10) == LF::Check && sc::leanFrameKind(30, 10) == LF::Check && sc::leanFrameKind(31, 10) == LF::Lean);
    CHECK(sc::leanFrameKind(29, 10) == LF::Warmup && sc::leanFrameKind(28, 10) == LF::Lean);
    CHECK(sc::leanFrameKind(5, 1) == LF::Check && sc::leanFrameKind(6, 1) == LF::Check);
    CHECK(sc::leanFrameKind(4, 2) == LF::Check && sc::leanFrameKind(5, 2) == LF::Warmup);
    // a resource is touched once per frame, again in the next frame
    auto set = std::make_unique<sc::TouchSet<64>>();
    int a = 0, b = 0;
    CHECK(set->first(&a, 1));
    CHECK(!set->first(&a, 1));
    CHECK(set->first(&b, 1));
    CHECK(!set->first(&b, 1));
    CHECK(set->first(&a, 2));
    CHECK(!set->first(&a, 2));
    // many resources: each first touch reported (collisions may report a repeat, never miss a first one)
    std::vector<int> many(1000);
    int firsts = 0;
    for (int& x : many) firsts += set->first(&x, 3) ? 1 : 0;
    CHECK(firsts == 1000);
}

TEST(static_register_runs_merge_across_words) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
    const auto collect = [&](const std::array<std::uint64_t, 4>& m) {
        runs.clear();
        sc::forEachRun(m, [&](std::uint32_t first, std::uint32_t n) { runs.emplace_back(first, n); });
    };
    collect({});
    CHECK(runs.empty());
    // c0..c11, c30, c60..c67 (across words 0 / 1), c255
    std::array<std::uint64_t, 4> m{};
    for (std::uint32_t r : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 30u, 60u, 61u, 62u, 63u, 64u, 65u, 66u, 67u, 255u}) m[r >> 6] |= 1ull << (r & 63);
    collect(m);
    CHECK(runs.size() == 4 && runs[0] == std::make_pair(0u, 12u) && runs[1] == std::make_pair(30u, 1u) && runs[2] == std::make_pair(60u, 8u) &&
          runs[3] == std::make_pair(255u, 1u));
    collect({~0ull, ~0ull, ~0ull, ~0ull});
    CHECK(runs.size() == 1 && runs[0] == std::make_pair(0u, 256u));
    // register ranges: across words, clipped at 256, empty
    collect(sc::regRange(60, 8));
    CHECK(runs.size() == 1 && runs[0] == std::make_pair(60u, 8u));
    collect(sc::regRange(232, 32));
    CHECK(runs.size() == 1 && runs[0] == std::make_pair(232u, 24u));
    collect(sc::regRange(0, 64));
    CHECK(runs.size() == 1 && runs[0] == std::make_pair(0u, 64u));
    collect(sc::regRange(300, 4));
    CHECK(runs.empty());
    collect(sc::regRange(0xfffffff0u, 0x20));
    CHECK(runs.empty());
    collect(sc::andMask(sc::andNot(sc::regRange(0, 40), sc::regRange(0, 31)), sc::regRange(30, 100)));
    CHECK(runs.size() == 1 && runs[0] == std::make_pair(31u, 9u));
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

TEST(particle_rules_follow_update_vertices) {
    // blend: the emitter's 1..7 mapped, other values by the fade against the double 0.9 (0.9f = 0.8999999761... is below it)
    CHECK(sc::particleBlendMode(1, 1.0f) == 2 && sc::particleBlendMode(3, 1.0f) == 5 && sc::particleBlendMode(7, 0.1f) == 8);
    CHECK(sc::particleBlendMode(0, 0.89f) == 2 && sc::particleBlendMode(0, 0.9f) == 2 && sc::particleBlendMode(0, 0.91f) == 0 && sc::particleBlendMode(9, 0.5f) == 2);
    // tiles 4 x 2, frame 5: column-major (5 / 2, 5 % 2) = (2, 1); row-major (5 % 4, 5 / 4) = (1, 1)
    const sc::ParticleTile c = sc::particleTile(5, 4, 2, false, 0.25f, 0.5f);
    CHECK(c.u0 == 0.5f && c.u1 == 0.75f && c.v0 == 0.5f && c.v1 == 1.0f);
    const sc::ParticleTile r = sc::particleTile(5, 4, 2, true, 0.25f, 0.5f);
    CHECK(r.u0 == 0.25f && r.u1 == 0.5f && r.v0 == 0.5f && r.v1 == 1.0f);
    // depth sort: far first, ties 0
    const float axis[4] = {0, 0, 1, 0}, nearP[3] = {0, 0, 1}, farP[3] = {0, 0, 5};
    CHECK(sc::particleDepthOrder(axis, farP, nearP) == -1 && sc::particleDepthOrder(axis, nearP, farP) == 1 &&
          sc::particleDepthOrder(axis, nearP, nearP) == 0);
    // strips: the age fade-in
    CHECK(sc::stripAlpha(0.8f, false, 2.0f, 1.0f) == 0.4f && sc::stripAlpha(0.8f, true, 2.0f, 1.0f) == 0.8f);
    CHECK(sc::stripAlpha(0.8f, false, 0.0f, 1.0f) == 0.8f && sc::stripAlpha(0.8f, false, 2.0f, 5.0f) == 0.8f &&
          sc::stripAlpha(0.8f, false, 2.0f, -1.0f) == 0.0f);
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

TEST(actor_morph_targets_and_bones) {
    // SetupMorphTargets 0x4dd220: the three strongest weights replace the weakest slot (strictly greater), ties keep the first
    const float w[] = {0.1f, 0.5f, 0.3f, 0.7f, 0.0f};
    const sc::MorphPick p = sc::pickMorphTargets(w);
    CHECK(p.target[0] == 3 && p.weight[0] == 0.7f && p.target[1] == 1 && p.weight[1] == 0.5f && p.target[2] == 2 && p.weight[2] == 0.3f);
    // all zero (the usual case: a blink between two closings): nothing replaced, targets 0, weights 0
    const float z[] = {0.0f, 0.0f, 0.0f, 0.0f};
    const sc::MorphPick q = sc::pickMorphTargets(z);
    CHECK(q.target == (std::array<std::uint32_t, 3>{0, 0, 0}) && q.weight[0] == 0.0f && q.weight[2] == 0.0f);
    // one active target lands in slot 0; equal weights later do not displace it
    const float one[] = {0.0f, 0.0f, 0.25f, 0.25f};
    const sc::MorphPick r = sc::pickMorphTargets(one);
    CHECK(r.target[0] == 2 && r.weight[0] == 0.25f && r.target[1] == 3 && r.weight[1] == 0.25f && r.weight[2] == 0.0f);
    CHECK(sc::morphStreamOffset(2, 1000) == 32000);
    // HardRenderActor2::Render: LOD clamp, the u16 primitive index, the bone loop's u16 bound, SetBoneMatrix's register
    CHECK(sc::actorLod(0) == 0 && sc::actorLod(3) == 3 && sc::actorLod(4) == 4 && sc::actorLod(0xffffffffu) == 4);
    CHECK(sc::actorPrimitive(10, 3) == 13 && sc::actorPrimitive(0xffff, 2) == 1);
    CHECK(sc::primitiveBones(0, 66) == 66 && sc::primitiveBones(5, 0) == 0 && sc::primitiveBones(0xfff0, 0x20) == 0);
    CHECK(sc::boneRegister(0) == 33 && sc::boneRegister(65) == 228);
}

TEST(native_attribution_skinned_zero_draws) {
    // a skinned op without draws is explained when its draw condition (frame_tap animationState) lacks a bit
    Op op;
    op.kind = OpKind::Animation;
    op.state = 0x1f;
    CHECK(!native::nothingToDraw(op));
    op.state = 0x1b;  // the hook's resource not cached
    CHECK(native::nothingToDraw(op) && !native::cannotDraw(op));
    op.state = -1;
    CHECK(!native::nothingToDraw(op));
}

TEST(tree_batches_follow_render_branches) {
    int l1 = 0, l2 = 0;
    // solid branches: breaks on the strip length, the light list, the light flags (the instance's or the batch's last) and the limit
    std::vector<sc::TreeInstance> in = {
        {0, 100, 0, &l1, false}, {1, 100, 0, &l1, false}, {2, 60, 900, &l1, false},  // strip change
        {3, 60, 900, &l2, false},                                                    // light list change
        {4, 60, 900, &l2, true},                                                     // static lights: alone
        {5, 60, 900, &l2, false},                                                    // after an alone one
        {6, 60, 900, &l2, false}, {7, 60, 900, &l2, false},                         // limit 2
    };
    std::vector<sc::TreeBatch> b;
    sc::planTreeBatches(in, 2, true, true, b);
    CHECK(b.size() == 6);
    CHECK(b[0].first == 0 && b[0].count == 2 && b[0].strip == 100 && b[0].start == 0);
    CHECK(b[1].first == 2 && b[1].count == 1 && b[1].strip == 60 && b[1].start == 900);
    CHECK(b[2].first == 3 && b[2].count == 1);
    CHECK(b[3].first == 4 && b[3].count == 1);
    CHECK(b[4].first == 5 && b[4].count == 2);
    CHECK(b[5].first == 7 && b[5].count == 1);
    // the z pass: strip and limit only (no light list)
    sc::planTreeBatches(in, 50, false, true, b);
    CHECK(b.size() == 2 && b[0].count == 2 && b[1].count == 6);
    // leaves in the z pass: the limit only; a null light list never opens a batch by itself, the first instance always does
    std::vector<sc::TreeInstance> leaves(5);
    sc::planTreeBatches(leaves, 2, false, false, b);
    CHECK(b.size() == 3 && b[0].count == 2 && b[1].count == 2 && b[2].count == 1 && b[2].first == 4);
    sc::planTreeBatches(std::span<const sc::TreeInstance>{}, 50, true, true, b);
    CHECK(b.empty());
    // a batch starts with its first instance's start index even when a later one's differs (strips equal)
    std::vector<sc::TreeInstance> same = {{0, 40, 10, nullptr, false}, {1, 40, 500, nullptr, false}};
    sc::planTreeBatches(same, 50, false, true, b);
    CHECK(b.size() == 1 && b[0].count == 2 && b[0].start == 10);
}

TEST(tree_ranges_software_and_hardware) {
    // software: n copies, strips joined by degenerate links
    sc::TreeRange r = sc::treeRange(sc::TreeGeometry::Branches, false, 3, 100, 700, 64, 0);
    CHECK(r.numVertices == 192 && r.startIndex == 700 && r.primCount == 302 && r.freq0 == 0);
    r = sc::treeRange(sc::TreeGeometry::Fronds, false, 1, 30, 5, 10, 0);
    CHECK(r.numVertices == 10 && r.startIndex == 5 && r.primCount == 28);
    r = sc::treeRange(sc::TreeGeometry::Leaves, false, 4, 0, 120, 16, 30);
    CHECK(r.numVertices == 64 && r.startIndex == 120 && r.primCount == 40);
    // hardware: one instance's counts, stream 0 INDEXEDDATA | n; branches from index 0
    r = sc::treeRange(sc::TreeGeometry::Branches, true, 3, 100, 700, 64, 0);
    CHECK(r.numVertices == 64 && r.startIndex == 0 && r.primCount == 98 && r.freq0 == (0x40000000u | 3));
    r = sc::treeRange(sc::TreeGeometry::Fronds, true, 2, 30, 5, 10, 0);
    CHECK(r.startIndex == 5 && r.primCount == 28 && r.freq0 == (0x40000000u | 2));
    r = sc::treeRange(sc::TreeGeometry::Leaves, true, 7, 0, 120, 16, 30);
    CHECK(r.numVertices == 16 && r.primCount == 10 && r.startIndex == 120);
}

TEST(grass_batches_follow_vegetation_render) {
    int a = 0, b = 0;
    std::vector<sc::GrassDraw> d;
    // solid: a new light set closes the batch; nodes without instances still switch the illumination
    std::vector<sc::GrassNode> nodes = {{&a, false, 10}, {&a, false, 5}, {&b, false, 0}, {&b, false, 7}, {&b, true, 3}, {&b, false, 2}};
    std::uint32_t end = sc::planGrass(nodes, 100, false, d);
    CHECK(end == 127);
    CHECK(d.size() == 4);
    CHECK(d[0].firstInstance == 0 && d[0].count == 15 && d[0].bufferOffset == 100 && d[0].illumNode == 0);
    CHECK(d[1].firstInstance == 15 && d[1].count == 7 && d[1].bufferOffset == 115 && d[1].illumNode == 2);
    CHECK(d[2].firstInstance == 22 && d[2].count == 3 && d[2].bufferOffset == 122 && d[2].illumNode == 4);
    CHECK(d[3].firstInstance == 25 && d[3].count == 2 && d[3].bufferOffset == 125 && d[3].illumNode == 5);
    // z pass: one batch until the buffer is full (1000): the draw, then DISCARD at 0
    std::vector<sc::GrassNode> big = {{&a, false, 600}, {&b, false, 500}};
    end = sc::planGrass(big, 300, true, d);
    CHECK(end == 400);
    CHECK(d.size() == 2 && d[0].count == 700 && d[0].bufferOffset == 300 && d[0].illumNode == -1 && d[1].count == 400 && d[1].bufferOffset == 0 &&
          d[1].firstInstance == 700);
    // a full buffer inside a node of the solid pass keeps the node's illumination for the rest
    end = sc::planGrass(big, 990, false, d);
    CHECK(d.size() == 4 && d[0].count == 10 && d[0].bufferOffset == 990 && d[0].illumNode == 0 && d[1].count == 590 && d[1].bufferOffset == 0 &&
          d[1].illumNode == 0 && d[2].count == 410 && d[2].bufferOffset == 590 && d[2].illumNode == 1 && d[3].count == 90 &&
          d[3].bufferOffset == 0 && d[3].firstInstance == 1010 && end == 90);
    // a null first light set: no break, no illumination
    std::vector<sc::GrassNode> dark = {{nullptr, false, 4}};
    sc::planGrass(dark, 0, false, d);
    CHECK(d.size() == 1 && d[0].illumNode == -1);
    // the instance bytes and the fade constants
    std::uint8_t entry[0x2c], out[0x2c];
    for (std::size_t i = 0; i < sizeof(entry); ++i) entry[i] = static_cast<std::uint8_t>(i);
    sc::grassInstance(entry, true, true, out);
    float w = 0, l = 0;
    std::memcpy(&w, out + 0xc, 4);
    std::memcpy(&l, out + 0x28, 4);
    CHECK(std::memcmp(out, entry + 4, 12) == 0 && w == 1.0f && std::memcmp(out + 0x10, entry + 0x10, 24) == 0 && l == 0.8f);
    sc::grassInstance(entry, false, false, out);
    std::memcpy(&w, out + 0xc, 4);
    std::memcpy(&l, out + 0x28, 4);
    CHECK(w == 0.0f && l == 0.0f);
    float x = 0, y = 0;
    sc::vegetationAlpha(1000.0f, 3000.0f, 128, x, y);
    CHECK(x == -1.0f / 4000.0f && y == 5000.0f / 4000.0f);
}

TEST(native_attribution_tree_and_grass_zero_draws) {
    Op op;
    op.kind = OpKind::SpeedTree;
    op.state = 7;
    CHECK(!native::nothingToDraw(op));
    op.state = 3;  // no queued tree with the type's geometry at its LOD
    CHECK(native::nothingToDraw(op) && !native::cannotDraw(op));
    op.kind = OpKind::Vegetation;
    op.state = 0xf;
    CHECK(!native::nothingToDraw(op));
    op.state = 0xd;  // the mesh not cached
    CHECK(native::nothingToDraw(op));
    CHECK(native::sc::vsField(12, 0) == native::sc::Field::VsWind && native::sc::vsField(22, 3) == native::sc::Field::VsWind &&
          native::sc::vsField(23, 0) == native::sc::Field::VsLeaves && native::sc::vsField(29, 1) == native::sc::Field::VsLeaves &&
          native::sc::vsField(13, 0) == native::sc::Field::VsAdminOther);
}

TEST(billboard_passes_pair_variants) {
    std::vector<sc::Pass> p = {{0x201, nullptr, 0, 0}, {0x245, nullptr, 3, 0}};
    sc::billboardPassPairs(p);
    CHECK(p.size() == 4 && p[0].flags == 0x601 && p[1].flags == 0x645 && p[2].flags == 0xa01 && p[3].flags == 0xa45 && p[3].firstLight == 3);
    CHECK(sc::vsIndex(p[0].flags) == 4 && sc::vsIndex(p[2].flags) == 5);
}

// N5.5: a shadow map's z list (ExecuteShadowMap): its draws are in the Shadow view's ShadowDepth pass; a main-view pass is a mismatch
TEST(native_attribution_shadow_view_pass) {
    CHECK(native::viewListPass(native::ViewKind::Main, List::ZPass) == Pass::DepthPrepass);
    CHECK(native::viewListPass(native::ViewKind::Cascade, List::ZPass) == Pass::ShadowDepth &&
          native::viewListPass(native::ViewKind::PointAtlas, List::ZPass) == Pass::ShadowDepth);
    CHECK(std::string(native::name(native::ViewKind::Terrain)) == "terrain map");
    Fixture f;
    f.live[static_cast<std::size_t>(List::ZPass)] = {raw(1, 0, 0), raw(2, 0, 0xff)};
    f.build();
    f.desc.kind = native::ViewKind::Cascade;
    f.desc.cascade = 1;
    Attribution a;
    a.begin(&f.desc);
    a.enterOp(f.at(List::ZPass, 0), f.at(List::ZPass, 0));
    a.draw(drawOf(f.at(List::ZPass, 0), Pass::ShadowDepth, 0));
    a.enterOp(f.at(List::ZPass, 1), f.at(List::ZPass, 1));
    a.draw(drawOf(f.at(List::ZPass, 1), Pass::DepthPrepass, 1));  // not the shadow view's pass
    a.end(f.counts());
    CHECK(a.counts().passMismatch == 1 && !a.counts().clean());
    const auto lines = native::reportLines(a.counts(), 3, a.counts(), a.mismatches(), native::ViewKind::Cascade);
    CHECK(!lines.empty() && lines[0].find("frame tap [cascade]") != std::string::npos);
    f.desc.clear();
    CHECK(f.desc.kind == native::ViewKind::Main && f.desc.cascade == -1 && f.desc.shadowMap == nullptr);
}

// ---- N5.5c: the effect draws (native/effect_compose) ---------------------------------------------------------------------------

#include <windows.h>

#include <cstdio>
#include <span>

#include "d3d9/defs.h"
#include "fx/effect_binary.h"
#include "native/effect_compose.h"

namespace {

// .fx source -> fx_2_0 binary with d3dx9_39's effect compiler (ID3DXEffectCompiler::CompileEffect = vtable slot 59); empty: no D3DX
std::vector<std::byte> compileFx(const char* src) {
    struct Buf {
        virtual long __stdcall QueryInterface(const void*, void**) = 0;
        virtual unsigned long __stdcall AddRef() = 0;
        virtual unsigned long __stdcall Release() = 0;
        virtual void* __stdcall GetBufferPointer() = 0;
        virtual unsigned long __stdcall GetBufferSize() = 0;
    };
    using CreateFn = long(__stdcall*)(const char*, UINT, const void*, void*, DWORD, void**, Buf**);
    using CompileFn = long(__stdcall*)(void*, DWORD, Buf**, Buf**);
    HMODULE m = LoadLibraryW(L"d3dx9_39.dll");
    const auto create = m != nullptr ? reinterpret_cast<CreateFn>(GetProcAddress(m, "D3DXCreateEffectCompiler")) : nullptr;
    if (create == nullptr) return {};
    void* compiler = nullptr;
    Buf* err = nullptr;
    if (create(src, static_cast<UINT>(std::strlen(src)), nullptr, nullptr, 0, &compiler, &err) < 0 || compiler == nullptr) {
        if (err != nullptr) {
            std::printf("  fx: %s\n", static_cast<const char*>(err->GetBufferPointer()));
            err->Release();
        }
        return {};
    }
    Buf* out = nullptr;
    auto** vtbl = *reinterpret_cast<void***>(compiler);
    const long hr = reinterpret_cast<CompileFn>(vtbl[59])(compiler, 0, &out, &err);
    reinterpret_cast<Buf*>(compiler)->Release();
    if (err != nullptr) err->Release();
    if (hr < 0 || out == nullptr) return {};
    const auto* p = static_cast<const std::byte*>(out->GetBufferPointer());
    std::vector<std::byte> r(p, p + out->GetBufferSize());
    out->Release();
    return r;
}

// a technique like post_SSM's SSM: stencil states, colour mask, a texture sampler, a matrix, a preshader on a float
constexpr const char* kMaskLikeFx = R"(
float4x4 DepthToShadowMatrix;
float ZDistance;
float2 FadOut;
texture ShadowTexture;
sampler ShadowSampler = sampler_state { texture = <ShadowTexture>; AddressU = CLAMP; AddressV = BORDER; MinFilter = POINT; MagFilter = LINEAR; MipFilter = NONE; };
float4 VS(float4 p : POSITION) : POSITION { return float4(p.xy, ZDistance, 1); }
float4 PS(float2 uv : TEXCOORD0) : COLOR { return tex2D(ShadowSampler, mul(float4(uv, 0, 1), DepthToShadowMatrix).xy) * (FadOut.x * 2 + FadOut.y); }
technique SSM {
    pass p0 {
        AlphaBlendEnable = false; ColorWriteEnable = 0x8; ZEnable = true;
        StencilEnable = true; StencilPass = Incr; StencilFail = Incr; StencilZFail = Keep; StencilFunc = Equal; StencilRef = 0;
        VertexShader = compile vs_3_0 VS();
        PixelShader = compile ps_3_0 PS();
    }
}
)";

}  // namespace

TEST(native_effect_pass_states_samplers_constants) {
    namespace fxc = native::fxc;
    const auto bin = compileFx(kMaskLikeFx);
    if (bin.empty()) {
        std::printf("  (skipped: d3dx9_39.dll not available)\n");
        return;
    }
    const auto r = fx::parseEffect(bin);
    CHECK(r.ok());
    if (!r.ok()) return;
    const fx::Effect& e = r.effect;
    fx::ParameterValues v = fx::ParameterValues::defaults(e);
    const float z = 0.75f, fad[2] = {40.0f, -4.0f};
    v.setFloats(e, "ZDistance", std::span<const float>(&z, 1));
    v.setFloats(e, "FadOut", std::span<const float>(fad, 2));
    int texObject = 0, vsObject = 0, psObject = 0;
    std::vector<const void*> textures(e.parameters.size(), nullptr);
    textures[e.parameterIndex("ShadowTexture")] = &texObject;
    std::vector<const void*> shaders(e.shaders.size(), nullptr);
    for (std::size_t s = 0; s < e.shaders.size(); ++s) shaders[s] = e.shaders[s].stage == shader::Stage::Vertex ? &vsObject : &psObject;
    auto d = std::make_unique<fxc::EffectDraw>();
    d->rs[d3d9::RS_CULLMODE] = 3;  // inherited
    d->rs[d3d9::RS_ZFUNC] = 4;
    d->ss[0][d3d9::SAMP_SRGBTEXTURE] = 1;  // inherited: the sampler_state does not name it
    fxc::applyPass(e, e.techniques[0].passes[0], v, textures, shaders, *d);
    CHECK(!d->unsupported);
    CHECK(d->vs == &vsObject && d->ps == &psObject);
    CHECK(d->rs[d3d9::RS_STENCILENABLE] == 1 && d->rs[d3d9::RS_STENCILPASS] == 7 && d->rs[d3d9::RS_STENCILFAIL] == 7 &&
          d->rs[d3d9::RS_STENCILZFAIL] == 1 && d->rs[d3d9::RS_STENCILFUNC] == 3 && d->rs[d3d9::RS_COLORWRITEENABLE] == 8 &&
          d->rs[d3d9::RS_ALPHABLENDENABLE] == 0 && d->rs[d3d9::RS_ZENABLE] == 1);
    CHECK(d->rsWritten.test(d3d9::RS_STENCILFUNC) && !d->rsWritten.test(d3d9::RS_CULLMODE) && d->rs[d3d9::RS_CULLMODE] == 3);
    // the sampler register the PS CTAB assigns: texture + the sampler_state's states, the rest as it was
    CHECK(d->textures[0] == &texObject && d->ss[0][d3d9::SAMP_ADDRESSU] == 3 && d->ss[0][d3d9::SAMP_ADDRESSV] == 4 &&
          d->ss[0][d3d9::SAMP_MINFILTER] == 1 && d->ss[0][d3d9::SAMP_MAGFILTER] == 2 && d->ss[0][d3d9::SAMP_MIPFILTER] == 0 &&
          d->ss[0][d3d9::SAMP_SRGBTEXTURE] == 1);
    // the constants: ZDistance somewhere in the VS file, FadOut.x * 2 + FadOut.y (a preshader) or FadOut itself in the PS's
    bool zFound = false;
    for (std::uint32_t reg = 0; reg < d->vsRegsWritten; ++reg) zFound = zFound || d->vsF[reg][0] == z;
    CHECK(zFound && d->vsRegsWritten > 0 && d->psRegsWritten > 0);
    bool fadFound = false;
    for (std::uint32_t reg = 0; reg < d->psRegsWritten; ++reg) {
        for (int c = 0; c < 4; ++c) fadFound = fadFound || d->psF[reg][c] == 76.0f;
        fadFound = fadFound || (d->psF[reg][0] == 40.0f && d->psF[reg][1] == -4.0f);
    }
    CHECK(fadFound);
    // eCGfxVisual::Execute after the pass: its four states unless the HAL's master flag holds that state
    fxc::applyVisual(*d, 3, 1, 5, 1, fxc::Masters{});
    CHECK(d->rs[d3d9::RS_ZFUNC] == 5 && d->rs[d3d9::RS_CULLMODE] == 1 && d->rs[d3d9::RS_ZWRITEENABLE] == 0 && d->rs[d3d9::RS_FILLMODE] == 3);
    fxc::Masters m;
    m.depthFunc = true;
    fxc::applyVisual(*d, 3, 1, 8, 2, m);
    CHECK(d->rs[d3d9::RS_ZFUNC] == 5 && d->rs[d3d9::RS_ZWRITEENABLE] == 1);
    // the comparison: equal to itself; one stencil state off -> the stencil class, the sampler state of a live stage -> its class
    auto c = std::make_unique<fxc::EffectDraw>(*d);
    fxc::Live live;
    live.stages = 1;
    live.ps.set(0);
    fxc::Diff diff = fxc::compare(*d, *c, live);
    CHECK(diff.fields.none() && diff.stages == 1 && diff.regs == 1 && diff.states == fxc::kRenderStates);
    c->rs[d3d9::RS_STENCILREF] = 1;
    c->ss[0][d3d9::SAMP_MIPMAPLODBIAS] = 7;
    c->viewport.width = 3;
    diff = fxc::compare(*d, *c, live);
    CHECK(diff.fields.test(static_cast<std::size_t>(fxc::Field::Stencil)) && diff.fields.test(static_cast<std::size_t>(fxc::Field::SamplerState)) &&
          diff.fields.test(static_cast<std::size_t>(fxc::Field::Viewport)) && diff.fields.count() == 3);
    c->ss[0][d3d9::SAMP_MIPMAPLODBIAS] = d->ss[0][d3d9::SAMP_MIPMAPLODBIAS];
    live.stages = 0;  // a stage the pixel shader does not sample is not compared
    c->textures[0] = nullptr;
    diff = fxc::compare(*d, *c, live);
    CHECK(!diff.fields.test(static_cast<std::size_t>(fxc::Field::Texture)));
}

// the shadow mask's own parameter values (eCImageFilterShadowMask::OnUpdateOutput 0x668560) with the engine's constants
TEST(native_effect_mask_parameters) {
    namespace fxc = native::fxc;
    const fxc::MaskConstants k;  // 2.5, -0.5, 0.5, 0.2, 2
    float kernel = 0, offset[4], scale[2];
    fxc::cascadeFilter(k, 1024, kernel, offset, scale);
    CHECK(kernel == 2.5f / 1024.0f && offset[0] == -0.5f / 1024.0f && offset[1] == offset[0] && offset[2] == 0.5f / 1024.0f &&
          offset[3] == offset[2] && scale[0] == 1024.0f && scale[1] == 1024.0f);
    float fade[2];
    fxc::fadeOut(k, 7500.0f, 60000.0f, fade);  // the recorded frame: (40, -4)
    CHECK(fade[0] == 40.0f && fade[1] == -4.0f);
    // ZDistance: the next split's start as a projected depth, (f P33 + P43) / f
    CHECK(fxc::zDistance(100.0f, 1.0f, -10.0f) == 0.9f);
    const float mat[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    float t[16];
    fxc::transpose(mat, t);
    CHECK(t[1] == 5 && t[4] == 2 && t[15] == 16 && t[3] == 13);
    CHECK(fxc::stateField(d3d9::RS_STENCILWRITEMASK) == fxc::Field::Stencil && fxc::stateField(d3d9::RS_ZFUNC) == fxc::Field::Depth &&
          fxc::stateField(d3d9::RS_COLORWRITEENABLE) == fxc::Field::ColorWrite && fxc::stateField(d3d9::RS_FOGENABLE) == fxc::Field::OtherState);
}

// the shadow views' setup (eCDepthMap, UpdateCamera 0x5a13e0)
TEST(native_effect_shadow_setup_rules) {
    namespace fxc = native::fxc;
    CHECK(fxc::isDepthFormat(6) && fxc::isDepthFormat(3) && !fxc::isDepthFormat(8) && !fxc::isDepthFormat(2));
    CHECK(fxc::depthMapClearFlags(6) == 6 && fxc::depthMapClearFlags(8) == 7);
    const float box[6] = {-100.0f, -50.0f, -100000.0f, 300.0f, 150.0f, 2000.0f};
    float sc[3], tr[3];
    fxc::shadowProjection(box, sc, tr);
    CHECK(sc[0] == 2.0f / 400.0f && sc[1] == 2.0f / 200.0f && sc[2] == 1.0f / 102000.0f);
    CHECK(tr[0] == 200.0f * -0.5f * sc[0] && tr[1] == 100.0f * -0.5f * sc[1] && tr[2] == -(sc[2] * -100000.0f));
    float bias[16];
    fxc::shadowTextureBias(1024, 1e-4f, bias);
    CHECK(bias[0] == 0.5f && bias[5] == -0.5f && bias[10] == 1.0f && bias[15] == 1.0f && bias[12] == 0.5f / 1024.0f + 0.5f && bias[13] == bias[12] &&
          bias[14] == -1e-4f && bias[1] == 0.0f && bias[3] == 0.0f);
}

// N5.7: the post chain's semantic parameter rules (ColorMatrix 0x758980, Camera 0x289b10 kinds 10 / 11, SkyDome 0x289fc0 kinds 4 / 5)
TEST(native_post_semantic_rules) {
    namespace fxc = native::fxc;
    const float tint[3] = {1.0f, 0.5f, 0.25f};
    const float w[3] = {0.3f, 0.59f, 0.11f};
    float m[9];
    fxc::colorMatrix(2.0f, 1.0f, tint, 1.0f, w, m);  // saturation 1: a diagonal of E t
    CHECK(m[0] == 2.0f && m[4] == 1.0f && m[8] == 0.5f && m[1] == 0.0f && m[3] == 0.0f && m[7] == 0.0f);
    fxc::colorMatrix(1.0f, 0.0f, tint, 1.0f, w, m);  // saturation 0: every row the luminance weights x t_i
    CHECK(m[0] == 0.3f && m[1] == 0.59f && m[2] == 0.11f && m[3] == 0.3f * 0.5f && m[8] == 0.11f * 0.25f);
    float v[2];
    fxc::focalRangeNear(0.0f, 1000.0f, 1e-5f, -1.0f, v);
    CHECK(v[0] == 0.0f && v[1] == 0.0f);
    fxc::focalRangeNear(250.0f, 1000.0f, 1e-5f, -1.0f, v);  // n = 0.25: (-4, 1): near CoC = sat(1 - d / n)
    CHECK(v[0] == -4.0f && v[1] == 1.0f);
    fxc::focalRangeFar(500.0f, 0.0f, 1000.0f, 1.0f, v);  // end <= start: the far plane; r = 0.5: (2, -1)
    CHECK(v[0] == 2.0f && v[1] == -1.0f);
    fxc::focalRangeFar(0.0f, 2000.0f, 1000.0f, 1.0f, v);  // r >= 1: off
    CHECK(v[0] == 0.0f && v[1] == 0.0f);
    float s[3] = {3.0f, -7.0f, 5.0f};
    fxc::clampScreenVector(s, 1.0f, -1.0f);
    CHECK(s[0] == 1.0f && s[1] == -1.0f && s[2] == 5.0f);
    float nanv[3] = {std::nanf(""), std::nanf(""), 0.0f};
    fxc::clampScreenVector(nanv, 1.0f, -1.0f);
    CHECK(nanv[0] == 1.0f && nanv[1] == 1.0f);
    CHECK(fxc::sunFlare(-0.5f) == 0.25f && fxc::sunFlare(0.5f) == 0.0f && fxc::sunFlare(0.0f) == 0.0f);
}

// N5.7: the sprite batcher (RenderBatches 0x757180 / DrawBatch 0x7570b0)
TEST(native_sprite_batches_follow_render_batches) {
    namespace fxc = native::fxc;
    int a = 0, b = 0;
    const fxc::SpriteKey recs[] = {{&a, 0}, {&a, 0}, {&b, 0}, {&b, 0x1000}, {&b, 0x1000}};
    const fxc::SpritePlan p = fxc::planSprites(recs, 0x1000, 0, 0);
    // a batch is closed by the next record's change (its `why`) and drawn with the state DrawBatch sets for it: the texture
    // break before record 2 draws [0, 2) with image a; the blend break before record 3 draws [2, 3) with image b (the blend still
    // 0: record 0's blend equals +0x98's initial 0, no SetBlend); the end draws [3, 5) after SetBlend(0x1000)
    CHECK(p.batches.size() == 3);
    CHECK(p.batches[0].why == fxc::SpriteBatch::Why::Texture && p.batches[0].setImage && p.batches[0].image == &a && !p.batches[0].setBlend &&
          p.batches[0].start == 0 && p.batches[0].count == 2);
    CHECK(p.batches[1].why == fxc::SpriteBatch::Why::Blend && p.batches[1].setImage && p.batches[1].image == &b && !p.batches[1].setBlend &&
          p.batches[1].start == 2 && p.batches[1].count == 1 && p.batches[1].firstRecord == 2);
    CHECK(p.batches[2].why == fxc::SpriteBatch::Why::End && p.batches[2].setBlend && p.batches[2].blend == 0x1000 && !p.batches[2].setImage &&
          p.batches[2].count == 2 && p.batches[2].start == 3 && p.batches[2].firstRecord == 3);
    CHECK(p.discard[0] && !p.discard[1] && p.quadOffset[4] == 4 * fxc::kSpriteQuad && p.endStart == 5 && p.endCount == 0);
    // a full buffer: start + count == capacity -> draw, then from 0 (DISCARD)
    const fxc::SpriteKey same[] = {{&a, 0}, {&a, 0}, {&a, 0}};
    const fxc::SpritePlan f = fxc::planSprites(same, 3, 1, 0);
    CHECK(f.batches.size() == 2 && f.batches[0].why == fxc::SpriteBatch::Why::Full && f.batches[0].start == 1 && f.batches[0].count == 2 &&
          f.discard[2] && f.quadOffset[2] == 0 && f.batches[1].start == 0 && f.batches[1].count == 1);
    // the quad: (p0, u0 v0), (p1, u1 v0), (p3, u1 v1), (p2, u0 v1)
    std::uint8_t rec[fxc::kSpriteRecord]{};
    const float corners[8] = {1, 2, 3, 4, 5, 6, 7, 8}, uv[4] = {0.1f, 0.2f, 0.3f, 0.4f}, z = 0.5f;
    const std::uint32_t colour = 0x80ff00ffu;
    std::memcpy(rec, corners, 32);
    std::memcpy(rec + 0x20, uv, 16);
    std::memcpy(rec + 0x30, &z, 4);
    std::memcpy(rec + 0x40, &colour, 4);
    std::uint8_t q[fxc::kSpriteQuad];
    fxc::spriteQuad(rec, 1.0f, q);
    const auto f32 = [&](int off) { float x; std::memcpy(&x, q + off, 4); return x; };
    CHECK(f32(0) == 1 && f32(4) == 2 && f32(8) == 0.5f && f32(0x14) == 0.1f && f32(0x18) == 0.2f);
    CHECK(f32(0x1c) == 3 && f32(0x20) == 4 && f32(0x30) == 0.3f && f32(0x34) == 0.2f);
    CHECK(f32(0x38) == 7 && f32(0x3c) == 8 && f32(0x4c) == 0.3f && f32(0x50) == 0.4f);
    CHECK(f32(0x54) == 5 && f32(0x58) == 6 && f32(0x68) == 0.1f && f32(0x6c) == 0.4f);
    std::uint32_t c;
    std::memcpy(&c, q + 0x10 + 3 * fxc::kSpriteVertex, 4);
    CHECK(c == colour);
    std::uint16_t idx[6];
    fxc::spriteQuadIndices(3, idx);
    CHECK(idx[0] == 12 && idx[1] == 14 && idx[2] == 15 && idx[3] == 12 && idx[4] == 13 && idx[5] == 14);
    std::uint32_t en, src, dst;
    CHECK(fxc::spriteBlend(0, en, src, dst) && en == 1 && src == 5 && dst == 6);
    CHECK(fxc::spriteBlend(0x5000, en, src, dst) && en == 0 && src == 2 && dst == 1);
    CHECK(fxc::spriteBlend(0x3000, en, src, dst) && src == 9 && dst == 1 && !fxc::spriteBlend(0x6000, en, src, dst));
    // the w lane is not compared
    std::uint8_t q2[fxc::kSpriteQuad];
    std::memcpy(q2, q, sizeof(q));
    q2[0x0c] ^= 0xff;
    std::size_t at = 0;
    std::uint64_t wl = 0;
    CHECK(fxc::spriteBytesEqual(q, q2, at, wl) && wl == 1);
    q2[0x10] ^= 1;
    CHECK(!fxc::spriteBytesEqual(q, q2, at, wl) && at == 0x10);
}

TEST(native_coverage_counts_by_source_and_reason) {
    using V = d3d9::scene::View;
    native::Coverage c;
    c.ours(V::Main, Pass::Solid, Kind::StaticMesh);
    c.ours(V::Main, Pass::Solid, Kind::StaticMesh);
    c.replaced(V::Main, Pass::Solid, Kind::StaticMesh);
    c.replaced(V::Main, Pass::Solid, Kind::StaticMesh);
    std::string reason = "Particles: no live particle";
    c.engine(V::Main, Pass::Alpha, Kind::Particle, reason);
    reason = "Particles: other reason";  // same buffer, new content: a new reason
    c.engine(V::Main, Pass::Alpha, Kind::Particle, reason);
    c.engine(V::Main, Pass::Alpha, Kind::Particle, "Particles: other reason");
    CHECK(c.oursTotal() == 2 && c.replacedTotal() == 2 && c.engineTotal() == 3 && c.draws() == 5);
    const auto by = c.engineByReason();
    CHECK(by.size() == 2 && by[0].first == "Particles: other reason" && by[0].second == 2 && by[1].second == 1);
    native::Coverage total;
    total.engine(V::Gui, Pass::Gui, Kind::Gui, "GUI: outside a sprite flush");
    total.add(c);
    total.add(c);
    CHECK(total.oursTotal() == 4 && total.engineTotal() == 7);
    const auto tb = total.engineByReason();
    CHECK(tb.size() == 3 && tb[0].first == "Particles: other reason" && tb[0].second == 4);
    const auto lines = total.lines(" (test)");
    CHECK(lines.size() == 5);
    CHECK(lines[0].find("11 draws") != std::string::npos);
    total.clear();
    CHECK(total.empty() && total.draws() == 0);
}

TEST(native_pssm_pass_states_follow_the_mask_loop) {
    namespace fxc = native::fxc;
    // a cell: passes 0 / 1 INCR / DECR into zfail (camera inside the cell frustum) or pass, pass 2 clears with NOTEQUAL
    fxc::PssmState s{};
    s.cull = 9;  // whatever the visual held
    s = fxc::pssmPass(0, true, s);
    CHECK(s.fail == 1 && s.zfail == 7 && s.pass == 1 && s.func == 8 && s.cull == 2 && s.depthFunc == 4);
    s = fxc::pssmPass(1, true, s);
    CHECK(s.zfail == 8 && s.pass == 1 && s.cull == 3 && s.depthFunc == 4);
    s = fxc::pssmPass(2, true, s);
    CHECK(s.fail == 2 && s.zfail == 2 && s.pass == 2 && s.func == 6 && s.cull == 1 && s.depthFunc == 8);
    // the next cell starts fresh (camera outside: the pass slot)
    s = fxc::pssmPass(0, false, s);
    CHECK(s.fail == 1 && s.zfail == 1 && s.pass == 7 && s.func == 8 && s.cull == 2);
    s = fxc::pssmPass(1, false, s);
    CHECK(s.zfail == 1 && s.pass == 8);
    fxc::EffectDraw d;
    fxc::applyPssmStencil(d, s);
    CHECK(d.rs[d3d9::RS_STENCILENABLE] == 1 && d.rs[d3d9::RS_STENCILPASS] == 8 && d.rs[d3d9::RS_STENCILFUNC] == 8 && d.rs[d3d9::RS_STENCILREF] == 0 &&
          d.rs[d3d9::RS_STENCILMASK] == 0xffffffffu && d.rs[d3d9::RS_STENCILWRITEMASK] == 0xffffffffu);
    CHECK(fxc::kPssmIndices[6] == 4 && fxc::kPssmIndices[17] == 3);
}
