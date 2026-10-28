#include "native/coverage.h"

#include <algorithm>
#include <format>
#include <map>
#include <tuple>

namespace rvk::native {

namespace {
struct Key {
    Coverage::View view;
    Coverage::Pass pass;
    Coverage::Kind kind;
    std::uint16_t reason;
};
Key split(std::uint64_t key) {
    return {static_cast<Coverage::View>(key & 0xff), static_cast<Coverage::Pass>(key >> 8 & 0xff), static_cast<Coverage::Kind>(key >> 16 & 0xff),
            static_cast<std::uint16_t>(key >> 24 & 0xffff)};
}
std::string percent(std::uint64_t part, std::uint64_t whole) {
    if (whole == 0) return "-";
    return std::format("{:.3f} %", 100.0 * static_cast<double>(part) / static_cast<double>(whole));
}
}  // namespace

std::uint16_t Coverage::reasonId(std::string_view reason) {
    if (reason.data() == lastPtr_ && reason.size() == lastLen_ && reasons_[lastId_] == reason) return lastId_;
    std::uint16_t id = 0;
    bool found = false;
    for (std::size_t i = 2; i < reasons_.size(); ++i) {
        if (reasons_[i] == reason) {
            id = static_cast<std::uint16_t>(i);
            found = true;
            break;
        }
    }
    if (!found) {
        if (reasons_.size() >= 0xffff) return static_cast<std::uint16_t>(reasons_.size() - 1);
        reasons_.emplace_back(reason);
        id = static_cast<std::uint16_t>(reasons_.size() - 1);
    }
    lastPtr_ = reason.data();
    lastLen_ = reason.size();
    lastId_ = id;
    return id;
}

std::uint64_t Coverage::oursTotal() const {
    std::uint64_t n = 0;
    for (const auto& [key, count] : counts_) {
        if (split(key).reason == kOurs) n += count;
    }
    return n;
}

std::uint64_t Coverage::replacedTotal() const {
    std::uint64_t n = 0;
    for (const auto& [key, count] : counts_) {
        if (split(key).reason == kReplaced) n += count;
    }
    return n;
}

std::uint64_t Coverage::engineTotal() const {
    std::uint64_t n = 0;
    for (const auto& [key, count] : counts_) {
        if (split(key).reason > kReplaced) n += count;
    }
    return n;
}

std::vector<std::pair<std::string, std::uint64_t>> Coverage::engineByReason() const {
    std::map<std::string, std::uint64_t> by;
    for (const auto& [key, count] : counts_) {
        const Key k = split(key);
        if (k.reason > kReplaced) by[reasons_[k.reason]] += count;
    }
    std::vector<std::pair<std::string, std::uint64_t>> out(by.begin(), by.end());
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

void Coverage::add(const Coverage& o) {
    for (const auto& [key, count] : o.counts_) {
        const Key k = split(key);
        const std::uint16_t r = k.reason <= kReplaced ? k.reason : reasonId(o.reasons_[k.reason]);
        at(k.view, k.pass, k.kind, r) += count;
    }
    lastPtr_ = nullptr;  // the cached pointer may name one of `o`'s strings
}

std::vector<std::string> Coverage::lines(std::string_view title, std::size_t maxReasons) const {
    std::vector<std::string> out;
    const std::uint64_t ours = oursTotal(), engine = engineTotal(), replaced = replacedTotal(), all = ours + engine;
    out.push_back(std::format("native: coverage{}: {} draws: ours {} ({}), engine {} ({}); engine draws replaced by ours {}{}", title, all, ours,
                              percent(ours, all), engine, percent(engine, all), replaced,
                              replaced != ours ? std::format(" (own draws without a replaced one: {})", static_cast<std::int64_t>(ours - replaced))
                                               : std::string()));
    // per view / pass: ours / engine
    std::map<std::pair<int, int>, std::pair<std::uint64_t, std::uint64_t>> byPass;
    for (const auto& [key, count] : counts_) {
        const Key k = split(key);
        if (k.reason == kReplaced) continue;
        auto& e = byPass[{static_cast<int>(k.view), static_cast<int>(k.pass)}];
        (k.reason == kOurs ? e.first : e.second) += count;
    }
    std::string line;
    for (const auto& [vp, oe] : byPass) {
        line += std::format("{}{}/{} {} / {}", line.empty() ? "" : ", ", d3d9::scene::name(static_cast<View>(vp.first)),
                            d3d9::scene::name(static_cast<Pass>(vp.second)), oe.first, oe.second);
    }
    if (!line.empty()) out.push_back(std::format("native: coverage{}: by view/pass (ours / engine): {}", title, line));
    // engine draws by reason, each with its view / pass / kind buckets
    std::map<std::uint16_t, std::vector<std::pair<std::uint64_t, std::tuple<int, int, int>>>> buckets;
    std::map<std::uint16_t, std::uint64_t> totals;
    for (const auto& [key, count] : counts_) {
        const Key k = split(key);
        if (k.reason <= kReplaced) continue;
        buckets[k.reason].push_back({count, {static_cast<int>(k.view), static_cast<int>(k.pass), static_cast<int>(k.kind)}});
        totals[k.reason] += count;
    }
    std::vector<std::pair<std::uint64_t, std::uint16_t>> order;
    for (const auto& [r, n] : totals) order.push_back({n, r});
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = 0; i < order.size() && i < maxReasons; ++i) {
        auto& b = buckets[order[i].second];
        std::stable_sort(b.begin(), b.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        std::string where;
        for (std::size_t j = 0; j < b.size() && j < 6; ++j) {
            const auto& [v, p, kd] = b[j].second;
            where += std::format("{}{}/{}/{} {}", where.empty() ? "" : ", ", d3d9::scene::name(static_cast<View>(v)), d3d9::scene::name(static_cast<Pass>(p)),
                                 d3d9::draws::name(static_cast<Kind>(kd)), b[j].first);
        }
        if (b.size() > 6) where += std::format(", +{} more", b.size() - 6);
        out.push_back(std::format("native: coverage{}:   engine, {}: {} ({}): {}", title, reasons_[order[i].second], order[i].first,
                                  percent(order[i].first, all), where));
    }
    if (order.size() > maxReasons) out.push_back(std::format("native: coverage{}:   (+{} more reasons)", title, order.size() - maxReasons));
    return out;
}

}  // namespace rvk::native
