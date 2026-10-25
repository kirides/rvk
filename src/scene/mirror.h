#pragma once

// The native scene mirror (docs/research/native-renderer.md, stage N0): our own record of every cached-in entity of the
// engine's world, driven by the engine's lifecycle events (cache-in / cache-out / delete) and not by the draw stream. An
// entity's identity is its pointer for as long as it is cached in; the id is stable while it lives and never reused, so a
// recycled pointer is a new entity. Pure logic, no engine access (the hooks are engine/scene_mirror.*), unit-tested in
// tests/mirror_tests.cpp. Thread-safe (the engine thread feeds it, the render thread reads it).

#include <array>
#include <atomic>
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
    float viewRange = 0.0f, alpha = 1.0f;    // entity +0xcc (ViewRange: 0 = no distance cull), +0xc8 (its own alpha)
    std::array<const void*, 8> kinds{};      // the first contexts' vtables (a context's kind)
};

// one sub-mesh of a mesh resource (eCSubMesh, stride 0x48): an indexed triangle list range of the resource's buffers
struct MeshPart {
    const void* subMesh = nullptr;           // eCSubMesh* (also the draw tag's subMesh)
    std::uint32_t firstIndex = 0, indexCount = 0;      // +0x28, +0x2c (triangle lists: indexCount / 3 primitives)
    std::uint32_t firstVertex = 0, vertexCount = 0;    // +0x30, +0x34 (the indices are absolute)
    const void* material = nullptr;          // eCMaterialResource2* (+0x38)
};

// a mesh render context (eCRenderContextMesh) of an entity and the mesh resource it currently draws (its level of detail)
struct MeshInfo {
    const void* context = nullptr;           // eCRenderContextMesh*
    const void* meshResource = nullptr;      // eCMeshResource2* (context +0x28): changes with the level of detail
    const void* visual = nullptr;            // eCGfxVisual* (resource +0x88)
    const void* vertexData[3]{};             // eCGfxVertexData* of streams 0..2 (visual +0x30 / +0x38 / +0x40)
    const void* indexData = nullptr;         // eCGfxIndexData* (visual +0x58)
    const void* declaration = nullptr;       // vertex declaration object (visual +0x50)
    std::vector<MeshPart> parts;
};

struct Entity {
    const void* ptr = nullptr;
    std::uint64_t id = 0;                    // stable while cached in, never reused
    EntityState state;
    std::vector<MeshInfo> meshes;            // its mesh render contexts (native-renderer.md N1)
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
    // the entity's mesh contexts (as read from the engine now); stored when they differ from the stored ones (a level of detail
    // switch changes a context's mesh resource); false: not mirrored. `changed` tells whether it was stored.
    bool setMeshes(const void* ptr, std::vector<MeshInfo>&& meshes, bool* changed = nullptr);
    // the stored mesh contexts of an entity (a copy; empty when not mirrored)
    std::vector<MeshInfo> meshesOf(const void* ptr) const;
    // `check` sees the stored mesh contexts under the lock (no copy); its verdict is returned (false when not mirrored)
    bool checkMeshes(const void* ptr, const std::function<bool(const std::vector<MeshInfo>&)>& check) const;
    // the sub-mesh `subMesh` of the entity: its mesh context (parts left empty) and part; false when unknown
    bool lookupPart(const void* ptr, const void* subMesh, MeshInfo& info, MeshPart& part) const;

    // the entity's world matrix (one lookup); false when not mirrored
    bool worldOf(const void* ptr, Matrix& out) const;

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

    // bumped by every change a cache of mirror parts must notice: an entity added / removed, its mesh contexts replaced (level of detail), its world
    // matrix changed (any thread)
    std::uint64_t version() const { return version_.load(std::memory_order_relaxed); }
    // the two parts of it: the set of entities / their mesh contexts (add, remove, level of detail), and the world matrices of entities with meshes
    std::uint64_t structureVersion() const { return structure_.load(std::memory_order_relaxed); }
    std::uint64_t transformVersion() const { return transform_.load(std::memory_order_relaxed); }
    // a copy of the entity's state (one lookup); false when not mirrored
    bool stateOf(const void* ptr, EntityState& out) const;

    static bool sameMatrix(const Matrix& a, const Matrix& b);

private:
    mutable std::mutex mutex_;
    std::unordered_map<const void*, Entity> map_;
    std::uint64_t nextId_ = 1;
    Counters counters_;
    std::atomic<std::uint64_t> version_{1}, structure_{1}, transform_{1};
};

}  // namespace rvk::scene
