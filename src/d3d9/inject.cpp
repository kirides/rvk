#include "d3d9/inject.h"

#include <algorithm>

namespace rvk::d3d9::inject {

const char* name(Point p) {
    switch (p) {
        case Point::BeforeDepthPrepass: return "BeforeDepthPrepass";
        case Point::AfterDepthPrepass: return "AfterDepthPrepass";
        case Point::BeforeSolid: return "BeforeSolid";
        case Point::AfterOpaque: return "AfterOpaque";
        case Point::AfterRefractionCopy: return "AfterRefractionCopy";
        case Point::BeforeAlpha: return "BeforeAlpha";
        case Point::AfterMainView: return "AfterMainView";
        case Point::BeforeGui: return "BeforeGui";
        case Point::Count: break;
    }
    return "?";
}

std::uint32_t Registry::add(Point point, std::string name, PassFn fn, const void* owner, int order) {
    if (point >= Point::Count || !fn) return 0;
    auto& list = lists_[static_cast<std::size_t>(point)];
    Entry e{nextId_++, point, order, std::move(name), owner, std::move(fn)};
    const auto pos = std::upper_bound(list.begin(), list.end(), order, [](int o, const Entry& x) { return o < x.order; });
    const std::uint32_t id = e.id;
    list.insert(pos, std::move(e));
    updateMask();
    return id;
}

bool Registry::remove(std::uint32_t id) {
    for (auto& list : lists_) {
        const auto it = std::ranges::find(list, id, &Entry::id);
        if (it != list.end()) {
            list.erase(it);
            updateMask();
            return true;
        }
    }
    return false;
}

std::size_t Registry::removeOwner(const void* owner) {
    std::size_t n = 0;
    for (auto& list : lists_) n += std::erase_if(list, [&](const Entry& e) { return e.owner == owner; });
    updateMask();
    return n;
}

std::size_t Registry::size() const {
    std::size_t n = 0;
    for (const auto& list : lists_) n += list.size();
    return n;
}

void Registry::updateMask() {
    mask_ = 0;
    for (std::size_t i = 0; i < kPointCount; ++i) {
        if (!lists_[i].empty()) mask_ |= 1u << i;
    }
}

bool Firings::take(Point p, bool inMain) {
    if (!inMainView(p)) return true;
    if (!inMain) return false;
    const std::uint32_t b = 1u << static_cast<std::uint32_t>(p);
    if ((fired_ & b) != 0) return false;
    fired_ |= b;
    return true;
}

}  // namespace rvk::d3d9::inject
