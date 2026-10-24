#pragma once

// The native scene mirror (docs/research/native-renderer.md, stage N0): our own record of every cached-in entity of the
// engine's world, driven by the engine's lifecycle events (cache-in / cache-out / delete) and not by the draw stream. An
// entity's identity is its pointer for as long as it is cached in; the id is stable while it lives and never reused, so a
// recycled pointer is a new entity. Pure logic, no engine access (the hooks are engine/scene_mirror.*), unit-tested in
// tests/mirror_tests.cpp. Thread-safe (the engine thread feeds it, the render thread reads it).

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rvk::scene {

using Matrix = std::array<float, 16>;  // the engine's bCMatrix: row-major, translation in elements 12..14

// what the engine says about an entity at a moment (engine/scene_mirror.cpp fills it from entity + 0x28 / 0x60 / 0xb8 / 0xf0)
struct EntityState {
    Matrix world{};
    std::uint32_t flags = 0;                 // entity +0x28
    float sphere[4]{};                       // world tree sphere: centre xyz, radius
    std::uint32_t contexts = 0;              // render contexts built for it
    std::array<const void*, 8> kinds{};      // the first contexts' vtables (a context's kind)
};

struct Entity {
    const void* ptr = nullptr;
    std::uint64_t id = 0;                    // stable while cached in, never reused
    EntityState state;
    std::uint64_t addedFrame = 0, updatedFrame = 0;
    std::uint32_t moved = 0;                 // times its world matrix changed since it was added
};

struct Counters {
    std::uint64_t added = 0, removed = 0;
    std::uint64_t doubleIn = 0;              // cache-in of an entity already mirrored (refreshed, same id)
    std::uint64_t outUnknown = 0;            // cache-out of an entity we never saw come in (cached in before the hooks)
    std::uint64_t deletedLive = 0;           // deleted while still mirrored: no cache-out came first
    std::uint64_t deletedUnknown = 0;
    std::uint64_t updated = 0, moved = 0;    // refreshes, of which the world matrix changed
    std::uint64_t drawsResolved = 0, drawsUnresolved = 0;  // cross-check against the draw tags
};

class Mirror {
public:
    // the entity entered the cached-in set; returns its record (valid until the next change)
    const Entity& cachedIn(const void* ptr, const EntityState& state, std::uint64_t frame);
    bool cachedOut(const void* ptr);   // false: not mirrored
    bool deleted(const void* ptr);     // the entity is being destroyed; false: not mirrored
    // a fresh state (the world matrix moved?); false: not mirrored
    bool update(const void* ptr, const EntityState& state, std::uint64_t frame);

    bool contains(const void* ptr) const;
    std::size_t size() const;
    std::size_t withContexts() const;
    // a draw's entity: counts it as resolved / unresolved; null entities (GUI, sky, particles) are not counted
    bool noteDraw(const void* entity);
    // visits every entity under the lock (the callback must not call back into the mirror)
    void forEach(const std::function<void(const Entity&)>& fn) const;
    // the pointers (to refresh them without holding the lock)
    std::vector<const void*> pointers() const;
    Counters counters() const;
    void clear();

    static bool sameMatrix(const Matrix& a, const Matrix& b);

private:
    mutable std::mutex mutex_;
    std::unordered_map<const void*, Entity> map_;
    std::uint64_t nextId_ = 1;
    Counters counters_;
};

}  // namespace rvk::scene
