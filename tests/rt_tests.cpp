// The RT scene's bookkeeping (src/enh/rt_registry.*, docs/research/rt-reflections.md 3): BLAS keys, instance keys, the
// persistent instance registry (owner replacement, aging, radius, cap), the build budget, the TLAS selection, the
// FrameBlock's RT fields.

#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>

#include "d3d9/scene.h"
#include "enh/rt_registry.h"
#include "shader/abi.h"
#include "test.h"

using namespace rvk;
namespace rt = rvk::enh::rt;

namespace {

rt::Rows at(float x, float y, float z) { return {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z}; }

}  // namespace

TEST(rt_blas_key_identity) {
    rt::BlasKey a{.vb = 5, .ib = 6, .vbGen = 1, .ibGen = 1, .vertexOffset = 0, .stride = 52, .first = 300, .primitives = 100};
    rt::BlasKey b = a;
    CHECK(a == b && rt::hash(a) == rt::hash(b));
    b.vbGen = 2;  // a new GPU copy of the vertex buffer: another BLAS
    CHECK(!(a == b) && rt::hash(a) != rt::hash(b));
    b = a;
    b.first = 600;  // another sub-mesh of the same buffers
    CHECK(rt::hash(a) != rt::hash(b));
    b = a;
    b.baseVertex = 12;
    CHECK(rt::hash(a) != rt::hash(b));
    std::unordered_map<rt::BlasKey, int, rt::BlasKeyHash> m;
    m[a] = 1;
    m[b] = 2;
    CHECK(m.size() == 2 && m[a] == 1);
}

TEST(rt_instance_key_quantised) {
    const rt::Rows r = at(100.0f, 20.0f, -3000.0f);
    rt::Rows close = r;
    close[3] += 0.01f;  // well inside 1/16 unit: the same instance (pre-pass, solid, cascade see bit-equal or near rows)
    CHECK(rt::instanceKey(7, r) == rt::instanceKey(7, close));
    rt::Rows moved = r;
    moved[3] += 1.0f;
    CHECK(rt::instanceKey(7, r) != rt::instanceKey(7, moved));
    CHECK(rt::instanceKey(7, r) != rt::instanceKey(8, r));  // other BLAS at the same place
    rt::Rows rotated = r;
    rotated[0] = 0.0f;
    rotated[1] = -1.0f;
    rotated[4] = 1.0f;
    rotated[5] = 0.0f;
    CHECK(rt::instanceKey(7, r) != rt::instanceKey(7, rotated));
    CHECK(rt::ownerKey(nullptr, 0) == 0);
    int e = 0;
    CHECK(rt::ownerKey(&e, 1) != 0 && rt::ownerKey(&e, 1) != rt::ownerKey(&e, 2));
    rt::Rows bad = r;
    bad[5] = std::nanf("");
    CHECK(rt::finite(r) && !rt::finite(bad));
}

TEST(rt_registry_dedupe_and_owner_replacement) {
    rt::Registry reg;
    int refs = 0;
    reg.onAdded = [&](const rt::Registry::Instance&) { ++refs; };
    reg.onRemoved = [&](const rt::Registry::Instance&) { --refs; };
    int entity = 0;
    const std::uint64_t owner = rt::ownerKey(&entity, 3);
    const rt::Rows a = at(0, 0, 0), b = at(500, 0, 0);
    // frame 1: the depth pre-pass, the solid pass and a cascade draw the same object: one instance, views OR-ed
    auto [i1, n1] = reg.see(rt::instanceKey(1, a), 1, a, owner, 0x2, 0, 1, 1, 0.0);
    CHECK(n1);
    const std::uint32_t id = i1->id;
    auto [i2, n2] = reg.see(rt::instanceKey(1, a), 1, a, owner, 0x8, 0x105, 2, 1, 0.0);
    CHECK(!n2 && reg.size() == 1 && i2->id == id && i2->views == 3 && i2->flags == 0xA && i2->data == 0x105);
    // frame 2: the object moved (or switched its level of detail: another BLAS): its old instance (not seen this frame) is
    // replaced at once
    reg.see(rt::instanceKey(1, b), 1, b, owner, 0, 0, 1, 2, 0.1);
    CHECK(reg.size() == 1 && reg.find(rt::instanceKey(1, a)) == nullptr && reg.counters().replaced == 1 && refs == 1);
    // frame 3: the owner draws two instances in one frame: both stay
    reg.see(rt::instanceKey(1, b), 1, b, owner, 0, 0, 1, 3, 0.2);
    reg.see(rt::instanceKey(1, a), 1, a, owner, 0, 0, 1, 3, 0.2);
    CHECK(reg.size() == 2 && refs == 2);
    // the cascades (view 2) draw another level of detail of the owner than the main view: both stay; a later switch in the
    // main view replaces only the main view's instance
    const rt::Rows c = at(900, 0, 0);
    reg.clear();
    reg.see(rt::instanceKey(5, a), 5, a, owner, 0, 0, 2, 10, 1.0);  // cascade: LOD 1
    reg.see(rt::instanceKey(4, a), 4, a, owner, 0, 0, 1, 10, 1.0);  // main: LOD 0
    CHECK(reg.size() == 2);
    reg.see(rt::instanceKey(5, a), 5, a, owner, 0, 0, 2, 11, 1.1);  // cascade again (the owner's latest overall)
    reg.see(rt::instanceKey(6, c), 6, c, owner, 0, 0, 1, 11, 1.1);  // main switched: LOD 0 -> LOD 2 elsewhere
    CHECK(reg.size() == 2 && reg.find(rt::instanceKey(4, a)) == nullptr && reg.find(rt::instanceKey(5, a)) != nullptr);
    // the TLAS takes the main view's level of detail: the cascade-only one is superseded while the main one lives
    const rt::Registry::Instance* lod1 = reg.find(rt::instanceKey(5, a));
    const rt::Registry::Instance* lod2 = reg.find(rt::instanceKey(6, c));
    CHECK(lod1 != nullptr && lod2 != nullptr && reg.supersededBy(*lod1, 1) && !reg.supersededBy(*lod2, 1));
    // no owner (instanced batches): never replaced
    reg.see(rt::instanceKey(2, a), 2, a, 0, 0, 0, 1, 4, 0.3);
    reg.see(rt::instanceKey(2, b), 2, b, 0, 0, 0, 1, 5, 0.4);
    CHECK(reg.size() == 4);
    CHECK(reg.removeBlas(2) == 2 && reg.size() == 2 && refs == 2);
    reg.clear();
    CHECK(reg.size() == 0 && refs == 0);
}

TEST(rt_registry_aging_radius_cap) {
    rt::Registry reg;
    rt::Limits l;
    l.unseenFrames = 100;
    l.unseenSeconds = 5.0;
    l.radius = 10000.0f;
    l.maxRegistered = 1000;
    const float cam[3] = {0, 0, 0};
    const rt::Rows nearRows = at(100, 0, 0), farRows = at(12000, 0, 0);
    reg.see(rt::instanceKey(1, nearRows), 1, nearRows, 0, 0, 0, 1, 1, 0.0);
    reg.see(rt::instanceKey(2, farRows), 2, farRows, 0, 0, 0, 1, 1, 0.0);
    // a big object whose bounds reach into the radius stays although its origin lies outside
    rt::Aabb big;
    const float lo[3] = {-3000, -10, -10}, hi[3] = {3000, 10, 10};
    big.add(lo);
    big.add(hi);
    const auto bounds = [&](std::uint64_t blas) -> const rt::Aabb* { return blas == 2 ? &big : nullptr; };
    CHECK(reg.age(cam, 2, 0.1, l, bounds) == 0 && reg.size() == 2);
    // without the bounds it is too far
    CHECK(reg.age(cam, 2, 0.1, l, nullptr) == 1 && reg.size() == 1 && reg.counters().tooFar == 1);
    // unseen: both frames AND seconds must pass (a 300 fps burst or a frozen game clock alone does not age it)
    CHECK(reg.age(cam, 500, 1.0, l, nullptr) == 0);
    CHECK(reg.age(cam, 50, 100.0, l, nullptr) == 0);
    CHECK(reg.age(cam, 500, 100.0, l, nullptr) == 1 && reg.size() == 0 && reg.counters().aged == 1);
    // cap: the longest unseen go first
    l.maxRegistered = 3;
    for (int i = 0; i < 5; ++i) {
        const rt::Rows r = at(static_cast<float>(i * 10), 0, 0);
        reg.see(rt::instanceKey(3, r), 3, r, 0, 0, 0, 1, 1000 + static_cast<std::uint64_t>(i), 200.0);
    }
    CHECK(reg.age(cam, 1005, 200.5, l, nullptr) == 2 && reg.size() == 3 && reg.counters().capped == 2);
    for (const auto& in : reg.instances()) CHECK(in.lastFrame >= 1002);
}

TEST(rt_world_sphere) {
    rt::Aabb box;
    CHECK(!box.valid());
    const float a[3] = {-1, -2, -3}, b[3] = {1, 2, 3};
    box.add(a);
    box.add(b);
    CHECK(box.valid());
    // scaled by 2 and moved to (10, 0, 0): extents 2, 4, 6 -> radius sqrt(4 + 16 + 36)
    const rt::Rows r = {2, 0, 0, 10, 0, 2, 0, 0, 0, 0, 2, 0};
    const rt::Sphere s = rt::worldSphere(box, r);
    CHECK(std::fabs(s.c[0] - 10.0f) < 1e-4f && std::fabs(s.c[1]) < 1e-4f && std::fabs(s.r - std::sqrt(56.0f)) < 1e-3f);
    const rt::Sphere p = rt::worldSphere(rt::Aabb{}, r);  // unknown bounds: the translation
    CHECK(p.c[0] == 10.0f && p.r == 0.0f);
}

TEST(rt_build_budget) {
    rt::BuildBudget b(3, 1000);
    CHECK(b.take(5000));   // the first always goes through, even over the triangle budget
    CHECK(!b.take(10));    // ... which is then used up
    rt::BuildBudget c(2, 1000);
    CHECK(c.take(400) && c.take(500) && !c.take(1));  // the build count
    rt::BuildBudget d(10, 1000);
    CHECK(d.take(600) && !d.take(500) && d.take(400) && d.builds() == 2 && d.triangles() == 1000);
}

TEST(rt_select_for_tlas) {
    rt::Registry reg;
    const float cam[3] = {0, 0, 0};
    for (int i = 0; i < 6; ++i) {
        const rt::Rows r = at(static_cast<float>(100 * (6 - i)), 0, 0);  // farther first
        reg.see(rt::instanceKey(1 + static_cast<std::uint64_t>(i % 2), r), 1 + static_cast<std::uint64_t>(i % 2), r, 0, 0, 0, 1,
                i < 2 ? 5 : 9, 0.0);
    }
    // BLAS 2 not built yet: only BLAS 1's instances
    const auto ready = [](const rt::Registry::Instance& in) { return in.blas == 1; };
    auto all = rt::selectForTlas(reg.instances(), cam, 100, ready);
    CHECK(all.size() == 3);
    // over the cap: this frame's sightings first, then the nearest
    auto two = rt::selectForTlas(reg.instances(), cam, 2, nullptr);
    CHECK(two.size() == 2);
    for (std::uint32_t i : two) CHECK(reg.instances()[i].lastFrame == 9);
    float farthest = 0.0f;
    for (std::uint32_t i : two) farthest = std::max(farthest, reg.instances()[i].rows[3]);
    CHECK(farthest <= 200.0f);  // the two nearest of frame 9: x = 100, 200
}

TEST(rt_frame_block_fields) {
    using namespace d3d9::scene;
    CameraBlock cur, last;
    FrameExtras x;
    shader::abi::FrameBlock fb{};
    fillFrameBlock(fb, cur, last, x);
    CHECK(fb.rtScene[0] == 0 && fb.rtInfo[2] == 0);  // no scene: zero (shaders test RVK_FRAME_RT_VALID)
    x.rtTlas = 0x0000001234567890ull;
    x.rtRecords = 0x00000ABC00001000ull;
    x.rtInstances = 42;
    x.rtRecordCount = 42;
    fillFrameBlock(fb, cur, last, x);
    CHECK(fb.rtScene[0] == 0x34567890u && fb.rtScene[1] == 0x12u && fb.rtScene[2] == 0x1000u && fb.rtScene[3] == 0xABCu);
    CHECK(fb.rtInfo[0] == 42 && fb.rtInfo[1] == 42 && fb.rtInfo[2] == shader::abi::kFrameRtValid);
}
