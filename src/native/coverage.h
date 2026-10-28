#pragma once

// N5.8 the completeness proof (docs/research/engine-frame/README.md "16. N5.8 result"): every draw that reaches the device
// (d3d9::DrawTap, the start of prepareDraw) counted by who issued it: our composition (a native unit's own draw) or the engine;
// engine draws dropped because we drew their unit ourselves are counted apart ("replaced"), they are not draws of the frame.
// Each engine draw that stays carries the reason it was left to the engine (a group switched off, compare only, a unit's skip
// reason, outside every composed path, ...). Keyed by view x pass x draw kind x reason. Pure (no engine access): tests
// tests/native_tests.cpp `native_coverage_*`. rvk.log `native: coverage ...` every 5 s, with -LogFrame for that frame.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "d3d9/draws.h"
#include "d3d9/scene.h"

namespace rvk::native {

class Coverage {
public:
    using View = d3d9::scene::View;
    using Pass = d3d9::scene::Pass;
    using Kind = d3d9::draws::Kind;

    // one of our own draws (a composed unit issued by the renderer)
    void ours(View v, Pass p, Kind k) { ++at(v, p, k, kOurs); }
    // an engine draw dropped because the renderer drew its unit itself
    void replaced(View v, Pass p, Kind k) { ++at(v, p, k, kReplaced); }
    // an engine draw that stays, with the reason it was left to the engine (copied at its first use)
    void engine(View v, Pass p, Kind k, std::string_view reason) { ++at(v, p, k, reasonId(reason)); }

    std::uint64_t oursTotal() const;
    std::uint64_t replacedTotal() const;
    std::uint64_t engineTotal() const;
    std::uint64_t draws() const { return oursTotal() + engineTotal(); }
    // engine draws by reason (all views), sorted by count, largest first
    std::vector<std::pair<std::string, std::uint64_t>> engineByReason() const;
    bool empty() const { return counts_.empty(); }
    void clear() { counts_.clear(); }
    void add(const Coverage& o);

    // the report: a summary line, the per view / pass lines (ours / engine), the engine draws by reason with their
    // view / pass / kind (at most `maxReasons`)
    std::vector<std::string> lines(std::string_view title, std::size_t maxReasons = 24) const;

private:
    static constexpr std::uint16_t kOurs = 0, kReplaced = 1;
    std::uint16_t reasonId(std::string_view reason);
    std::uint64_t& at(View v, Pass p, Kind k, std::uint16_t reason) {
        const std::uint64_t key = static_cast<std::uint64_t>(v) | static_cast<std::uint64_t>(p) << 8 | static_cast<std::uint64_t>(k) << 16 |
                                  static_cast<std::uint64_t>(reason) << 24;
        return counts_[key];
    }
    std::unordered_map<std::uint64_t, std::uint64_t> counts_;
    // the reason names (0 ours, 1 replaced, then the engine reasons in the order met); the last lookup is cached by pointer
    std::vector<std::string> reasons_{"ours", "replaced"};
    const char* lastPtr_ = nullptr;
    std::size_t lastLen_ = 0;
    std::uint16_t lastId_ = 0;
};

}  // namespace rvk::native
