// Native scene mirror (docs/research/native-renderer.md N0): identity, lifecycle, in-place updates.

#include "scene/mirror.h"
#include "test.h"

using namespace rvk;

namespace {

scene::EntityState at(float x, std::uint32_t contexts = 1) {
    scene::EntityState s;
    s.world[0] = s.world[5] = s.world[10] = s.world[15] = 1.0f;
    s.world[12] = x;
    s.contexts = contexts;
    return s;
}

}  // namespace

TEST(mirror_lifecycle_and_stable_ids) {
    scene::Mirror m;
    int a = 0, b = 0;
    const std::uint64_t ida = m.cachedIn(&a, at(0), 1).id;
    const std::uint64_t idb = m.cachedIn(&b, at(5), 1).id;
    CHECK(ida != 0 && idb != 0 && ida != idb && m.size() == 2);
    // a repeated cache-in refreshes the same record
    CHECK(m.cachedIn(&a, at(1), 2).id == ida && m.size() == 2 && m.counters().doubleIn == 1);
    // a cache-out removes it; the pointer coming back (the allocator recycles memory) is a NEW entity
    CHECK(m.cachedOut(&a) && m.size() == 1 && !m.contains(&a));
    const std::uint64_t ida2 = m.cachedIn(&a, at(0), 3).id;
    CHECK(ida2 != ida && ida2 != idb);
    // a cache-out of an unknown entity is counted, not an error
    int c = 0;
    CHECK(!m.cachedOut(&c) && m.counters().outUnknown == 1);
}

TEST(mirror_updates_in_place) {
    scene::Mirror m;
    int e = 0;
    const std::uint64_t id = m.cachedIn(&e, at(0), 1).id;
    // the same matrix: refreshed, not moved; a changed one: the same record, one move
    CHECK(m.update(&e, at(0), 2) && m.counters().moved == 0);
    CHECK(m.update(&e, at(7), 3) && m.counters().moved == 1);
    bool seen = false;
    m.forEach([&](const scene::Entity& x) {
        seen = x.id == id && x.moved == 1 && x.state.world[12] == 7.0f && x.updatedFrame == 3 && x.addedFrame == 1;
    });
    CHECK(seen && m.size() == 1);
    int other = 0;
    CHECK(!m.update(&other, at(0), 4));
}

TEST(mirror_delete_is_the_safety_net) {
    scene::Mirror m;
    int e = 0, f = 0;
    m.cachedIn(&e, at(0), 1);
    // deleted while still mirrored: no cache-out came first (counted: the engine path is validated by this counter)
    CHECK(m.deleted(&e) && m.size() == 0 && m.counters().deletedLive == 1);
    // deleted after its cache-out: already gone
    m.cachedIn(&f, at(0), 2);
    m.cachedOut(&f);
    CHECK(!m.deleted(&f) && m.counters().deletedUnknown == 1 && m.counters().deletedLive == 1);
}

TEST(mirror_draw_cross_check) {
    scene::Mirror m;
    int e = 0, unknown = 0;
    m.cachedIn(&e, at(0, 2), 1);
    CHECK(m.noteDraw(&e) && !m.noteDraw(&unknown) && m.noteDraw(nullptr));
    const scene::Counters c = m.counters();
    CHECK(c.drawsResolved == 1 && c.drawsUnresolved == 1);  // null entities (sky, GUI) are not counted
    CHECK(m.withContexts() == 1);
    int bare = 0;
    m.cachedIn(&bare, at(0, 0), 2);
    CHECK(m.size() == 2 && m.withContexts() == 1);
    CHECK(m.pointers().size() == 2);
}

TEST(mirror_mesh_parts_and_lod_switch) {
    scene::Mirror m;
    int e = 0, res0 = 0, res1 = 0, ctx = 0, sub0 = 0, sub1 = 0, sub2 = 0;
    m.cachedIn(&e, at(0), 1);
    scene::MeshInfo lod0;
    lod0.context = &ctx;
    lod0.meshResource = &res0;
    lod0.parts = {{&sub0, 0, 300, 0, 100, nullptr}, {&sub1, 300, 600, 100, 200, nullptr}};
    bool changed = false;
    CHECK(m.setMeshes(&e, {lod0}, &changed) && changed);
    // the same contexts / resources again: nothing to store
    CHECK(m.setMeshes(&e, {lod0}, &changed) && !changed);
    CHECK(m.checkMeshes(&e, [&](const std::vector<scene::MeshInfo>& cur) { return cur.size() == 1 && cur[0].meshResource == &res0; }));
    scene::MeshInfo info;
    scene::MeshPart part;
    CHECK(m.lookupPart(&e, &sub1, info, part) && part.firstIndex == 300 && part.indexCount == 600 && info.meshResource == &res0 && info.parts.empty());
    CHECK(!m.lookupPart(&e, &sub2, info, part));
    // a level of detail switch: another mesh resource (and sub-meshes) for the same context: stored, the old parts are gone
    scene::MeshInfo lod1 = lod0;
    lod1.meshResource = &res1;
    lod1.parts = {{&sub2, 0, 150, 0, 50, nullptr}};
    CHECK(m.setMeshes(&e, {lod1}, &changed) && changed);
    CHECK(!m.lookupPart(&e, &sub0, info, part) && m.lookupPart(&e, &sub2, info, part) && info.meshResource == &res1);
    CHECK(m.meshesOf(&e).size() == 1);
    int other = 0;
    CHECK(!m.setMeshes(&other, {}, nullptr) && !m.checkMeshes(&other, [](const auto&) { return true; }));
}
