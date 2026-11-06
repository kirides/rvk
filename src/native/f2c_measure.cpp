#include "native/f2c_measure.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"
#include "shader/sm_ir.h"

namespace rvk::native::f2c {

namespace detail {
bool g_on = false;
}

namespace {

constexpr std::size_t kSkipCodes = 32;
constexpr std::size_t kLists = 2;
constexpr const char* kListNames[kLists] = {"z", "solid"};

struct OpStats {
    std::uint64_t ops = 0, draws = 0, elig = 0, eligDraws = 0, uncomposed = 0, uncomposedDraws = 0;
    // exclusive attribution (first reason in priority order; index kReasons = composed or not, no reason bit but not composed)
    std::array<std::uint64_t, kExclusions + 1> exc{}, excDraws{};
    std::array<std::uint64_t, kBits> any{}, anyDraws{};
    std::array<std::uint64_t, kSkipCodes> skip{}, skipDraws{};
    void add(const OpStats& o) {
        ops += o.ops;
        draws += o.draws;
        elig += o.elig;
        eligDraws += o.eligDraws;
        uncomposed += o.uncomposed;
        uncomposedDraws += o.uncomposedDraws;
        for (std::size_t i = 0; i < exc.size(); ++i) {
            exc[i] += o.exc[i];
            excDraws[i] += o.excDraws[i];
        }
        for (std::size_t i = 0; i < any.size(); ++i) {
            any[i] += o.any[i];
            anyDraws[i] += o.anyDraws[i];
        }
        for (std::size_t i = 0; i < skip.size(); ++i) {
            skip[i] += o.skip[i];
            skipDraws[i] += o.skipDraws[i];
        }
    }
};

struct TrackStats {
    std::uint64_t pairs = 0, changed = 0, fresh = 0;
    std::array<std::uint64_t, 3> states{};
    void add(const TrackStats& o) {
        pairs += o.pairs;
        changed += o.changed;
        fresh += o.fresh;
        for (std::size_t i = 0; i < states.size(); ++i) states[i] += o.states[i];
    }
};

struct Counts {
    std::array<OpStats, kLists> op;
    std::uint64_t instOps[2] = {0, 0}, instSum[2] = {0, 0};  // [0] per-instance list ops, [1] batch ops: ops, instances
    std::array<TrackStats, kTrackers> trk;
    std::uint64_t frames = 0;
    void add(const Counts& o) {
        for (std::size_t i = 0; i < kLists; ++i) op[i].add(o.op[i]);
        for (std::size_t i = 0; i < kTrackers; ++i) trk[i].add(o.trk[i]);
        frames += o.frames;
        for (int i = 0; i < 2; ++i) {
            instOps[i] += o.instOps[i];
            instSum[i] += o.instSum[i];
        }
    }
};

Counts g_win, g_run;
std::array<std::string, kSkipCodes> g_skipNames;

// the op being measured
struct Cur {
    bool active = false;
    int list = 0;
    std::uint32_t bits = 0;
    bool composed = false;
    int skip = 0;
    std::uint32_t engineDraws = 0;
} g_cur;

std::uint64_t g_serial = 0;

struct Entry {
    std::uint64_t serial = 0;
    std::uint64_t value = 0;
};
std::array<std::unordered_map<const void*, Entry>, kTrackers> g_track;

struct DeviceCount {
    std::string name;
    std::uint64_t prep = 0, replay = 0, winPrep = 0, winReplay = 0;
};
std::vector<DeviceCount> g_observers, g_replacers;  // replacers: prep = handles(), replay = wantsOpaque()

struct VsInfo {
    std::bitset<256> reads;
    bool relative = false, instanced = false, ok = false;
    std::uint64_t draws[2] = {0, 0}, winDraws[2] = {0, 0};
};
std::unordered_map<const d3d9::Shader*, VsInfo> g_vs;

std::string shortName(const char* n) {
    std::string s = n != nullptr ? n : "?";
    for (const char* p : {"class ", "struct "}) {
        if (s.rfind(p, 0) == 0) s.erase(0, std::strlen(p));
    }
    if (s.rfind("rvk::", 0) == 0) s.erase(0, 5);
    return s;
}

DeviceCount& deviceEntry(std::vector<DeviceCount>& v, const char* n) {
    const std::string s = shortName(n);
    for (DeviceCount& d : v) {
        if (d.name == s) return d;
    }
    v.push_back({s});
    return v.back();
}

std::string regRanges(const std::bitset<256>& m, std::uint32_t from, std::uint32_t to) {
    std::string out;
    for (std::uint32_t r = from; r <= to;) {
        if (!m.test(r)) {
            ++r;
            continue;
        }
        std::uint32_t e = r;
        while (e + 1 <= to && m.test(e + 1)) ++e;
        out += out.empty() ? "" : ",";
        out += e == r ? std::format("{}", r) : std::format("{}-{}", r, e);
        r = e + 1;
    }
    return out.empty() ? "-" : out;
}

void analyse(const d3d9::Shader* s, VsInfo& v) {
    shader::DecodeResult r = shader::decode(s->bytecode);
    if (!r.ok()) return;
    const shader::Program& p = r.program;
    std::bitset<256> defd;
    for (const auto& d : p.defs) {
        if (d.op == shader::Op::Def && d.dst.reg.index < 256) defd.set(d.dst.reg.index);
    }
    const auto rows = [](shader::Op op) {
        switch (op) {
        case shader::Op::M4x4:
        case shader::Op::M3x4: return 4;
        case shader::Op::M4x3:
        case shader::Op::M3x3: return 3;
        case shader::Op::M3x2: return 2;
        default: return 0;
        }
    };
    for (const auto& ins : p.instructions) {
        for (std::size_t i = 0; i < ins.src.size(); ++i) {
            const auto& src = ins.src[i];
            if (src.reg.type != shader::RegType::Const) continue;
            if (src.rel.present) {
                v.relative = true;
                for (std::uint32_t k = src.reg.index; k < 256; ++k) v.reads.set(k);
                continue;
            }
            const int n = i == 1 ? std::max(rows(ins.op), 1) : 1;
            for (int k = 0; k < n; ++k) {
                const std::uint32_t reg = src.reg.index + static_cast<std::uint32_t>(k);
                if (reg < 256 && !defd.test(reg)) v.reads.set(reg);
            }
        }
    }
    v.ok = true;
}

std::string fmtOps(const Counts& c) {
    std::string out;
    for (std::size_t l = 0; l < kLists; ++l) {
        const OpStats& o = c.op[l];
        out += std::format("{}{} ops {} draws {} (eligible {} / {}; not composed {} / {})", l == 0 ? "" : " | ", kListNames[l], o.ops, o.draws, o.elig, o.eligDraws,
                           o.uncomposed, o.uncomposedDraws);
        std::string ex;
        for (std::size_t r = 0; r <= kExclusions; ++r) {
            if (o.exc[r] == 0) continue;
            ex += std::format("{}{} {}/{}", ex.empty() ? "" : ", ", r == kExclusions ? "other" : name(static_cast<Reason>(r)), o.exc[r], o.excDraws[r]);
        }
        out += std::format(" excl[{}]", ex);
        std::string an;
        for (std::size_t r = 0; r < kBits; ++r) {
            if (o.any[r] == 0) continue;
            an += std::format("{}{} {}/{}", an.empty() ? "" : ", ", name(static_cast<Reason>(r)), o.any[r], o.anyDraws[r]);
        }
        out += std::format(" any[{}]", an);
        std::string sk;
        for (std::size_t s = 1; s < kSkipCodes; ++s) {
            if (o.skip[s] == 0) continue;
            sk += std::format("{}{} {}/{}", sk.empty() ? "" : ", ", g_skipNames[s].empty() ? std::to_string(s) : g_skipNames[s], o.skip[s], o.skipDraws[s]);
        }
        if (!sk.empty()) out += std::format(" skips[{}]", sk);
    }
    out += std::format(" | render-instance ops: per-instance {} (sum of instances {}), hw batch {} (instances {})", c.instOps[0], c.instSum[0], c.instOps[1], c.instSum[1]);
    return out;
}

std::string fmtTrack(const Counts& c) {
    std::string out;
    for (std::size_t t = 0; t < kTrackers; ++t) {
        const TrackStats& s = c.trk[t];
        if (s.pairs + s.fresh == 0) continue;
        out += std::format("{}{} {}/{} (new {})", out.empty() ? "" : ", ", name(static_cast<Tracker>(t)), s.changed, s.pairs, s.fresh);
        if (t == kHoldsAll) out += std::format(" [all {} / unknown {} / dynamic {}]", s.states[1], s.states[0], s.states[2]);
        if (t == kIllumDirty) out += std::format(" [dirty {} / clean {}]", s.states[1], s.states[0]);
    }
    return out;
}

std::string fmtDevices(bool run) {
    std::string out;
    for (const DeviceCount& d : g_observers) {
        const std::uint64_t p = run ? d.prep : d.winPrep, r = run ? d.replay : d.winReplay;
        if (p + r != 0) out += std::format("{}{} {} (prepare {}, replay {})", out.empty() ? "" : ", ", d.name, p + r, p, r);
    }
    std::string rep;
    for (const DeviceCount& d : g_replacers) {
        const std::uint64_t p = run ? d.prep : d.winPrep, r = run ? d.replay : d.winReplay;
        if (p + r != 0) rep += std::format("{}{} handles {} / wantsOpaque {}", rep.empty() ? "" : ", ", d.name, p, r);
    }
    return std::format("observers[{}] replacers[{}]", out, rep);
}

void logCtab() {
    struct Set {
        std::uint32_t shaders = 0;
        std::uint64_t z = 0, color = 0;
        std::bitset<256> reads;
    };
    std::map<std::uint64_t, Set> sets;  // key: bits c12..c31 (20 bits) | relative << 40
    std::bitset<256> unionAll, unionInst;
    std::uint32_t shaders = 0, instanced = 0, bad = 0;
    std::uint64_t instDraws = 0;
    for (const auto& [s, v] : g_vs) {
        if (!v.ok) {
            ++bad;
            continue;
        }
        if (v.instanced) {
            ++instanced;
            instDraws += v.draws[0] + v.draws[1];
            unionInst |= v.reads;
            continue;
        }
        ++shaders;
        std::uint64_t key = 0;
        for (std::uint32_t r = 12; r <= 31; ++r) {
            if (v.reads.test(r)) key |= 1ull << (r - 12);
        }
        key |= v.relative ? 1ull << 40 : 0;
        Set& st = sets[key];
        ++st.shaders;
        st.z += v.draws[0];
        st.color += v.draws[1];
        st.reads = v.reads;
        unionAll |= v.reads;
    }
    std::vector<std::pair<std::uint64_t, const Set*>> order;
    for (const auto& [k, s] : sets) order.push_back({k, &s});
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.second->z + a.second->color > b.second->z + b.second->color; });
    std::bitset<256> u12;
    for (std::uint32_t r = 12; r <= 31; ++r) u12.set(r, unionAll.test(r));
    spdlog::info("native: f2c measure (run): CTAB static vertex shaders: {} non-instanced ({} instanced with {} draws skipped, {} undecodable), {} distinct c12..c31 read sets; "
                 "union of c12..c31 read: {}; c0..c11 read by any: {}; c32..c35 read by any: {}; c36+ read by any: {}; instanced VS read c0..c11 {} c12..c31 {} c32..c39 {} c40+ {}",
                 shaders, instanced, instDraws, bad, order.size(), regRanges(u12, 12, 31), regRanges(unionAll, 0, 11), regRanges(unionAll, 32, 35),
                 regRanges(unionAll, 36, 255), regRanges(unionInst, 0, 11), regRanges(unionInst, 12, 31), regRanges(unionInst, 32, 39),
                 regRanges(unionInst, 40, 255));
    int shown = 0;
    for (const auto& [k, s] : order) {
        if (++shown > 12) break;
        spdlog::info("native: f2c measure (run): CTAB set c12..c31 = {}{}: {} shaders, draws z {} color {}", regRanges(s->reads, 12, 31),
                     (k >> 40) & 1 ? " (+ relative addressing)" : "", s->shaders, s->z, s->color);
    }
}

}  // namespace

const char* name(Reason r) {
    static constexpr const char* n[kBits] = {"instanced (hw batch)", "instance list (per instance)", "overlay flags", "fade<1", "switch material", "no light list", "not table-lit", "hierarchy (ctx+0x34)", "node op",
                                                "shader class", "video image", "sort id (+0x2b8)", "draw inherits stage", "draw no light entry",
                                                "draw cpu stream / non-indexed", "no retain (on-demand compile)", "[info] masked", "[info] depth bias",
                                                "[info] TradeAlphaAsSolid", "[info] lightmaps"};
    return r < kBits ? n[r] : "?";
}

const char* name(Tracker t) {
    static constexpr const char* n[kTrackers] = {"lod (ctx+0x28)", "overlay flags", "illum ptr (+0xf8)", "illum bytes", "illum dirty byte", "sun bit", "holdsAll", "registry complete",
                                                 "material pools", "texture ready / images", "lightmap slots"};
    return t < kTrackers ? n[t] : "?";
}

void init() {
    static bool done = false;
    if (done) return;
    done = true;
    char buf[8]{};
    const DWORD n = GetEnvironmentVariableA("RVK_F2C_MEASURE", buf, sizeof(buf));
    detail::g_on = n > 0 && n < sizeof(buf) && buf[0] == '1';
    if (detail::g_on) spdlog::info("native: f2c measure: on (RVK_F2C_MEASURE=1)");
}

void frameBegin() {
    ++g_serial;
    ++g_win.frames;
    if (g_serial % 300 == 0) {
        for (auto& m : g_track) std::erase_if(m, [](const auto& kv) { return kv.second.serial + 120 < g_serial; });
    }
}

void opBegin(int list) {
    g_cur = {};
    g_cur.active = true;
    g_cur.list = list != 0 ? 1 : 0;
}

bool opActive() { return g_cur.active; }

void bit(Reason r) {
    if (g_cur.active) g_cur.bits |= 1u << r;
}

void opComposed(bool composed, int skipCode, const char* skipName) {
    if (!g_cur.active) return;
    g_cur.composed = composed;
    g_cur.skip = skipCode;
    if (skipCode > 0 && static_cast<std::size_t>(skipCode) < kSkipCodes && g_skipNames[static_cast<std::size_t>(skipCode)].empty() && skipName != nullptr) {
        g_skipNames[static_cast<std::size_t>(skipCode)] = skipName;
    }
}

void noteInstances(bool batch, std::uint32_t n) {
    ++g_win.instOps[batch ? 1 : 0];
    g_win.instSum[batch ? 1 : 0] += n;
}

void engineDraw() {
    if (g_cur.active) ++g_cur.engineDraws;
}

void opEnd(std::uint32_t ourDraws) {
    if (!g_cur.active) return;
    OpStats& o = g_win.op[static_cast<std::size_t>(g_cur.list)];
    const std::uint64_t draws = g_cur.composed ? ourDraws : g_cur.engineDraws;
    ++o.ops;
    o.draws += draws;
    std::size_t first = kExclusions;
    for (std::size_t r = 0; r < kBits; ++r) {
        if ((g_cur.bits >> r & 1u) == 0) continue;
        ++o.any[r];
        o.anyDraws[r] += draws;
        if (r < kExclusions && first == kExclusions) first = r;
    }
    if (!g_cur.composed) {
        ++o.uncomposed;
        o.uncomposedDraws += draws;
        const std::size_t s = std::min<std::size_t>(static_cast<std::size_t>(std::max(g_cur.skip, 0)), kSkipCodes - 1);
        ++o.skip[s];
        o.skipDraws[s] += draws;
    }
    if (first != kExclusions) {
        ++o.exc[first];
        o.excDraws[first] += draws;
    } else if (!g_cur.composed) {
        ++o.exc[kExclusions];
        o.excDraws[kExclusions] += draws;
    } else {
        ++o.elig;
        o.eligDraws += draws;
    }
    g_cur = {};
}

std::uint64_t hashBytes(const void* p, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    const auto* b = static_cast<const std::uint8_t*>(p);
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, b + i, 8);
        h = (h ^ w) * 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

void track(Tracker t, const void* key, std::uint64_t value) {
    Entry& e = g_track[t][key];
    if (e.serial == g_serial) return;  // once per frame per key
    TrackStats& s = g_win.trk[t];
    if ((t == kHoldsAll || t == kIllumDirty) && value < 3) ++s.states[value];
    if (e.serial != 0 && e.serial + 1 == g_serial) {
        ++s.pairs;
        if (e.value != value) ++s.changed;
    } else {
        ++s.fresh;
    }
    e.serial = g_serial;
    e.value = value;
}

void noteObserver(const char* typeName, int path) {
    DeviceCount& d = deviceEntry(g_observers, typeName);
    if (path == 0) ++d.winPrep;
    else ++d.winReplay;
}

void noteReplacer(const char* typeName, int what) {
    DeviceCount& d = deviceEntry(g_replacers, typeName);
    if (what == 0) ++d.winPrep;
    else ++d.winReplay;
}

void vsDraw(const d3d9::Shader* vs, bool zPass) {
    if (vs == nullptr) return;
    auto [it, inserted] = g_vs.try_emplace(vs);
    VsInfo& v = it->second;
    if (inserted) {
        v.instanced = (vs->semantics.flags & d3d9::ShaderSemantics::Instanced) != 0;
        analyse(vs, v);
    }
    ++v.winDraws[zPass ? 0 : 1];
}

void report(std::uint64_t frames) {
    static int ticks = 0;
    ++ticks;
    // the window: ops finished so far (the op in flight stays for the next tick)
    for (auto& v : g_vs) {
        v.second.draws[0] += v.second.winDraws[0];
        v.second.draws[1] += v.second.winDraws[1];
        v.second.winDraws[0] = v.second.winDraws[1] = 0;
    }
    spdlog::info("native: f2c measure (5 s): frames {} (main views {}); {}; inval changed/pairs (new) [{}]; {}", frames, g_win.frames, fmtOps(g_win), fmtTrack(g_win),
                 fmtDevices(false));
    g_run.add(g_win);
    for (DeviceCount& d : g_observers) {
        d.prep += d.winPrep;
        d.replay += d.winReplay;
        d.winPrep = d.winReplay = 0;
    }
    for (DeviceCount& d : g_replacers) {
        d.prep += d.winPrep;
        d.replay += d.winReplay;
        d.winPrep = d.winReplay = 0;
    }
    g_win = {};
    if (ticks % 6 == 0) {
        spdlog::info("native: f2c measure (run): main views {}; {}; inval changed/pairs (new) [{}]; {}", g_run.frames, fmtOps(g_run), fmtTrack(g_run), fmtDevices(true));
        logCtab();
    }
}

}  // namespace rvk::native::f2c
