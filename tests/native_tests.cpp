// The native frame's op tap (N5.0, docs/research/engine-frame/README.md): the engine's sort rules, op lookup, attribution.

#include <cstdlib>
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
