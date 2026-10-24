#include "scene/mirror.h"

#include <cstring>

namespace rvk::scene {

bool Mirror::sameMatrix(const Matrix& a, const Matrix& b) { return std::memcmp(a.data(), b.data(), sizeof(Matrix)) == 0; }

const Entity& Mirror::cachedIn(const void* ptr, const EntityState& state, std::uint64_t frame) {
    std::lock_guard lock(mutex_);
    auto [it, fresh] = map_.try_emplace(ptr);
    Entity& e = it->second;
    if (fresh) {
        e.ptr = ptr;
        e.id = nextId_++;
        e.addedFrame = frame;
        ++counters_.added;
    } else {
        ++counters_.doubleIn;
        if (!sameMatrix(e.state.world, state.world)) ++e.moved;
    }
    e.state = state;
    e.updatedFrame = frame;
    return e;
}

bool Mirror::cachedOut(const void* ptr) {
    std::lock_guard lock(mutex_);
    if (map_.erase(ptr) == 0) {
        ++counters_.outUnknown;
        return false;
    }
    ++counters_.removed;
    return true;
}

bool Mirror::deleted(const void* ptr) {
    std::lock_guard lock(mutex_);
    if (map_.erase(ptr) == 0) {
        ++counters_.deletedUnknown;
        return false;
    }
    ++counters_.deletedLive;
    ++counters_.removed;
    return true;
}

bool Mirror::update(const void* ptr, const EntityState& state, std::uint64_t frame) {
    std::lock_guard lock(mutex_);
    const auto it = map_.find(ptr);
    if (it == map_.end()) return false;
    Entity& e = it->second;
    ++counters_.updated;
    if (!sameMatrix(e.state.world, state.world)) {
        ++e.moved;
        ++counters_.moved;
    }
    e.state = state;
    e.updatedFrame = frame;
    return true;
}

bool Mirror::contains(const void* ptr) const {
    std::lock_guard lock(mutex_);
    return map_.contains(ptr);
}

std::size_t Mirror::size() const {
    std::lock_guard lock(mutex_);
    return map_.size();
}

std::size_t Mirror::withContexts() const {
    std::lock_guard lock(mutex_);
    std::size_t n = 0;
    for (const auto& [ptr, e] : map_) n += e.state.contexts != 0 ? 1u : 0u;
    return n;
}

bool Mirror::noteDraw(const void* entity) {
    if (entity == nullptr) return true;
    std::lock_guard lock(mutex_);
    const bool known = map_.contains(entity);
    ++(known ? counters_.drawsResolved : counters_.drawsUnresolved);
    return known;
}

void Mirror::forEach(const std::function<void(const Entity&)>& fn) const {
    std::lock_guard lock(mutex_);
    for (const auto& [ptr, e] : map_) fn(e);
}

std::vector<const void*> Mirror::pointers() const {
    std::lock_guard lock(mutex_);
    std::vector<const void*> out;
    out.reserve(map_.size());
    for (const auto& [ptr, e] : map_) out.push_back(ptr);
    return out;
}

Counters Mirror::counters() const {
    std::lock_guard lock(mutex_);
    return counters_;
}

void Mirror::clear() {
    std::lock_guard lock(mutex_);
    map_.clear();
}

}  // namespace rvk::scene
