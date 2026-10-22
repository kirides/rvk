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

std::int64_t quantise(float v, float scale) { return static_cast<std::int64_t>(std::llround(static_cast<double>(v) * scale)); }

}  // namespace

std::uint64_t hash(const BlasKey& k) {
    std::uint64_t h = 0x84222325CBF29CE4ull;
    h = mix(h, k.vb);
    h = mix(h, k.ib);
    h = mix(h, (static_cast<std::uint64_t>(k.vbGen) << 32) | k.ibGen);
    h = mix(h, (static_cast<std::uint64_t>(k.vertexOffset) << 32) | k.stride);
    h = mix(h, (static_cast<std::uint64_t>(k.first) << 32) | k.primitives);
    h = mix(h, (static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.baseVertex)) << 32) | k.index32);
    return h;
}

bool finite(const Rows& r) {
    for (float v : r) {
        if (!std::isfinite(v)) return false;
    }
    return true;
}

std::uint64_t instanceKey(std::uint64_t blasId, const Rows& rows) {
    std::uint64_t h = mix(0x51ED270B27C6A2B1ull, blasId);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            const float v = rows[static_cast<std::size_t>(row * 4 + col)];
            h = mix(h, static_cast<std::uint64_t>(quantise(v, col == 3 ? 16.0f : 4096.0f)));
        }
    }
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
    Aabb w;
    for (int corner = 0; corner < 8; ++corner) {
        const float p[3] = {(corner & 1) ? box.max[0] : box.min[0], (corner & 2) ? box.max[1] : box.min[1],
                            (corner & 4) ? box.max[2] : box.min[2]};
        float q[3];
        for (int row = 0; row < 3; ++row) {
            q[row] = r[static_cast<std::size_t>(row * 4)] * p[0] + r[static_cast<std::size_t>(row * 4 + 1)] * p[1] +
                     r[static_cast<std::size_t>(row * 4 + 2)] * p[2] + r[static_cast<std::size_t>(row * 4 + 3)];
        }
        w.add(q);
    }
    float d2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        s.c[i] = 0.5f * (w.min[i] + w.max[i]);
        const float e = 0.5f * (w.max[i] - w.min[i]);
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
            owners_[ownerSlot(owner, view)] = key;
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
    if (owner != 0) owners_[slot] = key;
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
        const Instance& in = list_[i];
        const bool stale = frame > in.lastFrame + limits.unseenFrames && time > in.lastTime + limits.unseenSeconds;
        bool tooFar = false;
        if (!stale && camera != nullptr && limits.radius > 0.0f) {
            const Aabb* box = bounds ? bounds(in.blas) : nullptr;
            const Sphere s = worldSphere(box != nullptr ? *box : Aabb{}, in.rows);
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
        for (const Instance& in : list_) byAge.emplace_back(in.lastFrame, in.key);
        const std::size_t excess = list_.size() - limits.maxRegistered;
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
