#include "enh/rt_registry.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rvk::enh::rt {

namespace {

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    // splitmix64 of the combination: cheap and well distributed
    std::uint64_t x = h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x;
}

}  // namespace

std::uint64_t hash(const BlasKey& k) {
    // multiply-xor over the key's words, one final avalanche (the key is looked up for every sighting)
    constexpr std::uint64_t kMul = 0x9E3779B97F4A7C15ull;
    std::uint64_t h = (k.vb ^ 0x84222325CBF29CE4ull) * kMul;
    h = (h ^ k.ib ^ (h >> 29)) * kMul;
    h = (h ^ ((static_cast<std::uint64_t>(k.vbGen) << 32) | k.ibGen)) * kMul;
    h = (h ^ ((static_cast<std::uint64_t>(k.vertexOffset) << 32) | k.stride)) * kMul;
    h = (h ^ ((static_cast<std::uint64_t>(k.first) << 32) | k.primitives)) * kMul;
    h = (h ^ ((static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.baseVertex)) << 32) | (k.index32 | (k.strip << 1)))) * kMul;
    return h ^ (h >> 31);
}

bool finite(const Rows& r) {
    for (float v : r) {
        if (!std::isfinite(v)) return false;
    }
    return true;
}

std::uint64_t instanceKey(std::uint64_t blasId, const Rows& rows) {
    // the translation to 1/16 unit, the 3x3 part to 1/4096 (round half away from zero), folded by multiply-xor
    constexpr std::uint64_t kMul = 0x9E3779B97F4A7C15ull;
    std::uint64_t h = (blasId ^ 0x51ED270B27C6A2B1ull) * kMul;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            const float v = rows[static_cast<std::size_t>(row * 4 + col)];
            const double scaled = static_cast<double>(v) * (col == 3 ? 16.0 : 4096.0);
            const auto q = static_cast<std::int64_t>(scaled + (v < 0.0f ? -0.5 : 0.5));
            h = (h ^ static_cast<std::uint64_t>(q)) * kMul;
            h ^= h >> 32;
        }
    }
    h = mix(h, 0);
    return h == 0 ? 1 : h;
}

std::uint64_t ownerKey(const void* entity, std::uint32_t part) {
    if (entity == nullptr) return 0;
    const std::uint64_t h = mix(mix(0x2545F4914F6CDD1Dull, reinterpret_cast<std::uintptr_t>(entity)), part);
    return h == 0 ? 1 : h;
}

void Aabb::add(const float p[3]) {
    if (!valid()) {
        for (int i = 0; i < 3; ++i) min[i] = max[i] = p[i];
        return;
    }
    for (int i = 0; i < 3; ++i) {
        min[i] = std::min(min[i], p[i]);
        max[i] = std::max(max[i], p[i]);
    }
}

Sphere worldSphere(const Aabb& box, const Rows& r) {
    Sphere s;
    if (!box.valid()) {
        s.c[0] = r[3];
        s.c[1] = r[7];
        s.c[2] = r[11];
        return s;
    }
    // the transformed box's axis-aligned bounds: its centre is the transformed centre, its half extents |R| x half extents
    const float cx = 0.5f * (box.min[0] + box.max[0]), cy = 0.5f * (box.min[1] + box.max[1]), cz = 0.5f * (box.min[2] + box.max[2]);
    const float hx = 0.5f * (box.max[0] - box.min[0]), hy = 0.5f * (box.max[1] - box.min[1]), hz = 0.5f * (box.max[2] - box.min[2]);
    float d2 = 0.0f;
    for (int row = 0; row < 3; ++row) {
        const float* m = &r[static_cast<std::size_t>(row * 4)];
        s.c[row] = m[0] * cx + m[1] * cy + m[2] * cz + m[3];
        const float e = std::fabs(m[0]) * hx + std::fabs(m[1]) * hy + std::fabs(m[2]) * hz;
        d2 += e * e;
    }
    s.r = std::sqrt(d2);
    return s;
}

bool BuildBudget::take(std::uint64_t triangles) {
    if (builds_ > 0 && (builds_ >= maxBuilds_ || triangles_ + triangles > maxTriangles_)) return false;
    ++builds_;
    triangles_ += triangles;
    return true;
}

std::uint64_t Registry::ownerSlot(std::uint64_t owner, std::uint8_t view) { return mix(owner, 0xC0FFEEull + view); }

std::pair<Registry::Instance*, bool> Registry::see(std::uint64_t key, std::uint64_t blas, const Rows& rows, std::uint64_t owner,
                                                   std::uint32_t flags, std::uint32_t data, std::uint8_t view, std::uint64_t frame,
                                                   double time) {
    if (auto it = index_.find(key); it != index_.end()) {
        Instance& in = list_[it->second];
        in.lastFrame = frame;
        in.lastTime = time;
        in.flags |= flags;
        if (data != 0) in.data = data;
        in.views |= view;
        if (owner != 0) {
            in.owner = owner;
            if ((in.ownerViews & view) != view) {
                // the owner's latest instance in this view: a holder that is another instance loses its claim
                auto [slot, fresh] = owners_.try_emplace(ownerSlot(owner, view), key);
                if (!fresh && slot->second != key) {
                    if (Instance* prev = find(slot->second)) prev->ownerViews &= static_cast<std::uint8_t>(~view);
                    slot->second = key;
                }
                in.ownerViews |= view;
            }
        }
        return {&in, false};
    }
    // the owner's latest instance in this view (the cascades may draw another level of detail than the main view)
    const std::uint64_t slot = owner != 0 ? ownerSlot(owner, view) : 0;
    if (owner != 0) {
        if (auto it = owners_.find(slot); it != owners_.end() && it->second != key) {
            if (auto j = index_.find(it->second); j != index_.end() && list_[j->second].owner == owner && list_[j->second].lastFrame != frame) {
                removeAt(j->second);
                ++counters_.replaced;
            }
        }
    }
    Instance in;
    in.key = key;
    in.blas = blas;
    in.owner = owner;
    in.rows = rows;
    in.id = nextId_++;
    if (nextId_ == 0) nextId_ = 1;
    in.flags = flags;
    in.data = data;
    in.views = view;
    in.firstFrame = in.lastFrame = frame;
    in.lastTime = time;
    index_[key] = list_.size();
    list_.push_back(in);
    if (owner != 0) {
        if (auto [it, fresh] = owners_.try_emplace(slot, key); !fresh) {
            if (Instance* prev = find(it->second); prev != nullptr && prev != &list_.back()) prev->ownerViews &= static_cast<std::uint8_t>(~view);
            it->second = key;
        }
        list_.back().ownerViews = view;
    }
    ++counters_.added;
    if (onAdded) onAdded(list_.back());
    return {&list_.back(), true};
}

void Registry::removeAt(std::size_t i) {
    const Instance gone = list_[i];
    if (gone.owner != 0) {
        for (std::uint8_t v = 1; v != 0 && v <= gone.views; v = static_cast<std::uint8_t>(v << 1)) {
            if ((gone.views & v) == 0) continue;
            if (auto it = owners_.find(ownerSlot(gone.owner, v)); it != owners_.end() && it->second == gone.key) owners_.erase(it);
        }
    }
    index_.erase(gone.key);
    if (i + 1 != list_.size()) {
        list_[i] = list_.back();
        index_[list_[i].key] = i;
    }
    list_.pop_back();
    if (onRemoved) onRemoved(gone);
}

std::size_t Registry::age(const float camera[3], std::uint64_t frame, double time, const Limits& limits,
                          const std::function<const Aabb*(std::uint64_t)>& bounds) {
    std::size_t dropped = 0;
    for (std::size_t i = 0; i < list_.size();) {
        Instance& in = list_[i];
        if (in.pinned) {
            ++i;
            continue;
        }
        const bool stale = frame > in.lastFrame + limits.unseenFrames && time > in.lastTime + limits.unseenSeconds;
        bool tooFar = false;
        if (!stale && camera != nullptr && limits.radius > 0.0f) {
            if (!in.sphereValid) {
                const Aabb* box = bounds ? bounds(in.blas) : nullptr;
                in.sphere = worldSphere(box != nullptr ? *box : Aabb{}, in.rows);
                in.sphereValid = box != nullptr;  // without bounds (BLAS not built yet): its translation, again next time
            }
            const Sphere& s = in.sphere;
            const float dx = s.c[0] - camera[0], dy = s.c[1] - camera[1], dz = s.c[2] - camera[2];
            tooFar = std::sqrt(dx * dx + dy * dy + dz * dz) - s.r > limits.radius;
        }
        if (stale || tooFar) {
            (stale ? counters_.aged : counters_.tooFar)++;
            removeAt(i);
            ++dropped;
        } else {
            ++i;
        }
    }
    if (list_.size() > limits.maxRegistered) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> byAge;  // last frame, key
        byAge.reserve(list_.size());
        for (const Instance& in : list_) {
            if (!in.pinned) byAge.emplace_back(in.lastFrame, in.key);
        }
        const std::size_t excess = std::min(list_.size() - limits.maxRegistered, byAge.size());
        std::nth_element(byAge.begin(), byAge.begin() + static_cast<std::ptrdiff_t>(excess), byAge.end());
        for (std::size_t k = 0; k < excess; ++k) {
            if (auto it = index_.find(byAge[k].second); it != index_.end()) {
                removeAt(it->second);
                ++counters_.capped;
                ++dropped;
            }
        }
    }
    return dropped;
}

std::size_t Registry::removeIf(const std::function<bool(const Instance&)>& pred) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < list_.size();) {
        if (pred(list_[i])) {
            removeAt(i);
            ++n;
        } else {
            ++i;
        }
    }
    counters_.dropped += n;
    return n;
}

std::size_t Registry::removeBlas(std::uint64_t blas) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < list_.size();) {
        if (list_[i].blas == blas) {
            removeAt(i);
            ++n;
        } else {
            ++i;
        }
    }
    counters_.dropped += n;
    return n;
}

void Registry::clear() {
    while (!list_.empty()) removeAt(list_.size() - 1);
    index_.clear();
    owners_.clear();
}

bool Registry::supersededBy(const Instance& in, std::uint8_t preferred) const {
    if (in.owner == 0 || (in.views & preferred) != 0) return false;
    const auto it = owners_.find(ownerSlot(in.owner, preferred));
    return it != owners_.end() && it->second != in.key && index_.contains(it->second);
}

const Registry::Instance* Registry::find(std::uint64_t key) const {
    const auto it = index_.find(key);
    return it != index_.end() ? &list_[it->second] : nullptr;
}

Registry::Instance* Registry::find(std::uint64_t key) {
    const auto it = index_.find(key);
    return it != index_.end() ? &list_[it->second] : nullptr;
}

std::vector<std::uint32_t> selectForTlas(const std::vector<Registry::Instance>& list, const float camera[3], std::uint32_t max,
                                         const std::function<bool(const Registry::Instance&)>& take) {
    std::vector<std::uint32_t> out;
    out.reserve(list.size());
    for (std::uint32_t i = 0; i < list.size(); ++i) {
        if (!take || take(list[i])) out.push_back(i);
    }
    if (out.size() <= max) return out;
    const auto dist2 = [&](const Registry::Instance& in) {
        const float dx = in.rows[3] - camera[0], dy = in.rows[7] - camera[1], dz = in.rows[11] - camera[2];
        return dx * dx + dy * dy + dz * dz;
    };
    std::nth_element(out.begin(), out.begin() + max, out.end(), [&](std::uint32_t a, std::uint32_t b) {
        const auto& ia = list[a];
        const auto& ib = list[b];
        if (ia.lastFrame != ib.lastFrame) return ia.lastFrame > ib.lastFrame;
        return dist2(ia) < dist2(ib);
    });
    out.resize(max);
    std::sort(out.begin(), out.end());  // stable order in the TLAS (the records follow the registry)
    return out;
}

}  // namespace rvk::enh::rt
