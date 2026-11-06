#include "native/f4_measure.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"
#include "engine/memread.h"
#include "shader/sm_ir.h"

namespace rvk::native::f4m {

namespace detail {
bool g_on = false;
}

namespace {

constexpr int kVcs = 8;  // view categories: main, cascade 0..3, terrain map, point atlas, other
constexpr const char* kVcNames[kVcs] = {"main", "cas0", "cas1", "cas2", "cas3", "terrain", "point", "other"};
constexpr const char* kCatNames[kCats] = {"solid", "z", "shadowz"};
constexpr const char* kPhaseNames[kPhases] = {"queue", "snap", "light", "rows", "plan", "draw", "other", "issue", "census"};
constexpr int kBins = 9;  // instances per bucket: 1, 2, 3-4, 5-8, 9-16, 17-32, 33-64, 65-128, 129+
constexpr const char* kBinNames[kBins] = {"1", "2", "3-4", "5-8", "9-16", "17-32", "33-64", "65-128", "129+"};

enum Tracker : int {
    kLod48,
    kLod50,
    kLod58,
    kLod5c,
    kRef60,
    kRef64,
    kVar6c,
    kLodAny,    // any of the seven fields
    kMatrix,    // the entity's world matrix (entity +0x60, 64 bytes)
    kIllumPtr,  // the queue entry's illumination pointer (solid)
    kIllumBytes,  // the illumination record's bytes [0, 0x110) (solid)
    kRbTables,  // the render buffer's tables (+0x38..0xb8, +0xe8..0x130)
    kWindMat,   // admin +0xac, 3 x 64
    kWindRock,  // admin +0x6c, 64
    kBbConst,   // a billboard's constant block (c32..c36 + c37+j)
    kTrackers
};
constexpr const char* kTrackNames[kTrackers] = {"lod48", "lod50", "lod58", "lod5c", "ref60", "ref64", "var6c", "lodAny", "matrix", "illumPtr", "illumBytes", "rb", "windMat", "windRock", "bb"};

struct TrackStats {
    std::uint64_t pairs = 0, changed = 0, fresh = 0;
};

struct CatStats {
    std::uint64_t ops = 0, views = 0, hw = 0, sw = 0, entries = 0, instances = 0, buckets = 0, batchesToday = 0, batchInstances = 0, devDraws = 0, callsF4 = 0;
    std::array<std::uint64_t, 4> opsByType{};
    std::array<std::array<std::uint64_t, kBins>, 4> hist{};
    std::array<std::int64_t, kPhases> ticks{};
    std::uint64_t draws = 0, draws1 = 0, drawsFreq0 = 0;  // composed draws, with a stream 1, with a stream-0 instancing divider
    // billboards
    std::uint64_t bbOps = 0, bbDraws = 0, bbViews = 0;
    std::int64_t bbTicks = 0, bbIssueTicks = 0;
};

struct VcStats {
    std::uint64_t views = 0, uniqueTrees = 0, noEntity = 0, leafSeen = 0, twoBuckets = 0;
    std::array<TrackStats, kTrackers> trk{};
};

struct Window {
    std::array<CatStats, kCats> cat{};
    std::array<VcStats, kVcs> vc{};
    // illumination classes of the solid ops' instances (exclusive, priority order): no list, dynamic lights, static ambient SH, > 2 static lights, 1-2 static lights, plain
    std::array<std::uint64_t, 6> illum{};
    std::uint64_t sunOn = 0, sunOff = 0;
    std::array<std::uint64_t, 4> sunMask{};  // per main view: 0 no solid tree op, 1 sun off only, 2 sun on only, 3 both
    std::uint64_t distinctLists = 0;
    std::map<std::uint32_t, std::uint64_t> freq1;  // stream-1 frequency divider of the composed draws
};
Window g_win;

struct Entry {
    std::uint64_t serial = 0, value = 0;
};
std::array<std::unordered_map<std::uint64_t, Entry>, kTrackers> g_track;
std::array<std::uint64_t, kVcs> g_vserial{};

struct ViewKey {
    std::uint64_t frame = ~0ull;
    std::uint32_t viewKind = 0;
    std::int32_t cascade = 0;
    const void* db = nullptr;
    const void* desc = nullptr;
    bool operator==(const ViewKey&) const = default;
};
ViewKey g_key;
bool g_haveView = false;
int g_vc = 0;

// per-view state
std::unordered_map<const void*, std::uint8_t> g_seen;  // bit 1 base tracked, 2 illumination tracked, 4 leaf counted
std::array<std::unordered_set<std::uint64_t>, kCats> g_calls;
std::array<std::uint32_t, kCats> g_opsInView{}, g_bbInView{};
std::unordered_set<const void*> g_lists;
std::uint32_t g_sunMask = 0;

// op state
bool g_active = false;
int g_cat = 0, g_lastCat = 0, g_phase = kOther;
std::int64_t g_start = 0;
const std::size_t* g_drawCount = nullptr;
bool g_zpass = false, g_shadow = false;
std::uint32_t g_type = 0;

// timing
std::int64_t g_freq = 1, g_windowStart = 0;
int g_ticks5 = 0;

std::int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double ms(std::int64_t ticks) { return 1000.0 * static_cast<double>(ticks) / static_cast<double>(g_freq); }

std::uint64_t hashBytes(const void* p, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

int vcOf(std::uint32_t viewKind, std::int32_t cascade) {
    switch (viewKind) {
    case 0: return 0;
    case 1: return 1 + std::clamp(cascade, 0, 3);
    case 2: return 5;
    case 3: return 6;
    default: return 7;
    }
}

void track(int vc, int trk, const void* key, std::uint64_t value) {
    auto [it, inserted] = g_track[trk].try_emplace(reinterpret_cast<std::uint64_t>(key) | (static_cast<std::uint64_t>(vc) << 56));
    Entry& e = it->second;
    TrackStats& s = g_win.vc[vc].trk[trk];
    const std::uint64_t now = g_vserial[vc];
    if (!inserted && e.serial == now) return;  // already read in this view
    if (!inserted && e.serial + 1 == now) {
        ++s.pairs;
        if (e.value != value) ++s.changed;
    } else {
        ++s.fresh;
    }
    e.serial = now;
    e.value = value;
}

int binOf(std::uint32_t n) {
    if (n <= 2) return static_cast<int>(n) - 1;
    if (n <= 4) return 2;
    if (n <= 8) return 3;
    if (n <= 16) return 4;
    if (n <= 32) return 5;
    if (n <= 64) return 6;
    if (n <= 128) return 7;
    return 8;
}

// ---- shaders ----
struct ShaderInfo {
    std::bitset<256> reads;
    bool relative = false, ok = false;
};
std::unordered_map<const d3d9::Shader*, ShaderInfo> g_shaders;
std::unordered_set<const void*> g_streamLogged;

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

void analyse(const d3d9::Shader* s, ShaderInfo& v) {
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

void noteShader(const void* sh, const char* stage) {
    if (sh == nullptr) return;
    const auto* s = static_cast<const d3d9::Shader*>(sh);
    auto [it, inserted] = g_shaders.try_emplace(s);
    if (!inserted) return;
    analyse(s, it->second);
    const ShaderInfo& v = it->second;
    const bool instanced = (s->semantics.flags & d3d9::ShaderSemantics::Instanced) != 0;
    if (!v.ok) {
        spdlog::info("native: f4 ctab: {} {:#x} (hash {:016x}) undecodable", stage, reinterpret_cast<std::uintptr_t>(s), s->hash);
        return;
    }
    spdlog::info("native: f4 ctab: {} {:#x} hash {:016x} first used by {} type {}: float registers read c0..c31 [{}], c32..c39 [{}], c40+ [{}], relative {}, c5 {}, instanced-flag {}",
                 stage, reinterpret_cast<std::uintptr_t>(s), s->hash, kCatNames[g_lastCat], g_type, regRanges(v.reads, 0, 31), regRanges(v.reads, 32, 39),
                 regRanges(v.reads, 40, 255), v.relative ? "yes" : "no", v.reads.test(5) ? "READ" : "not read", instanced ? "yes" : "no");
}

// ---- views ----
void flushView() {
    if (!g_haveView) return;
    for (int c = 0; c < kCats; ++c) {
        if (g_opsInView[c] != 0) {
            ++g_win.cat[c].views;
            g_win.cat[c].callsF4 += g_calls[c].size();
        }
        if (g_bbInView[c] != 0) ++g_win.cat[c].bbViews;
        g_calls[c].clear();
        g_opsInView[c] = g_bbInView[c] = 0;
    }
    VcStats& v = g_win.vc[g_vc];
    ++v.views;
    v.uniqueTrees += g_seen.size();
    if (g_vc == 0) {
        ++g_win.sunMask[g_sunMask & 3];
        g_win.distinctLists += g_lists.size();
    }
    g_seen.clear();
    g_lists.clear();
    g_sunMask = 0;
}

void report();

// a view change (any op of another view than the previous one) flushes the previous view; the 5 s tick runs only there
void viewTick(std::uint64_t frame, std::uint32_t viewKind, std::int32_t cascade, const void* db, const void* desc) {
    const ViewKey key{frame, viewKind, cascade, db, desc};
    if (g_haveView && key == g_key) return;
    flushView();
    const std::int64_t now = qpc();
    if (g_windowStart == 0) g_windowStart = now;
    if (now - g_windowStart >= 5 * g_freq) {
        report();
        g_windowStart = now;
    }
    g_key = key;
    g_haveView = true;
    g_vc = vcOf(viewKind, cascade);
    ++g_vserial[g_vc];
}

int catOf(bool zpass, bool shadow, std::uint32_t viewKind) {
    if (!zpass) return kMainSolid;
    return shadow || viewKind != 0 ? kShadowZ : kMainZ;
}

template <std::size_t N>
struct Snap {
    alignas(8) std::uint8_t b[N];
    bool ok = false;
    void load(const void* p) { ok = p != nullptr && engine::safeReadN<N>(p, b); }
    template <class T>
    T at(std::size_t off) const {
        T v;
        std::memcpy(&v, b + off, sizeof(T));
        return v;
    }
};

}  // namespace

void init() {
    static bool done = false;
    if (done) return;
    done = true;
    char buf[8] = {};
    if (GetEnvironmentVariableA("RVK_F4_MEASURE", buf, sizeof(buf)) != 0 && buf[0] == '1') {
        detail::g_on = true;
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_freq = f.QuadPart;
        spdlog::info("native: f4 measure: on (RVK_F4_MEASURE=1)");
    }
}

int enter(int phase) {
    if (!g_active) return -1;
    const std::int64_t t = qpc();
    g_win.cat[g_cat].ticks[g_phase] += t - g_start;
    const int prev = g_phase;
    g_phase = phase;
    g_start = t;
    return prev;
}

OpScope::OpScope(const std::size_t* drawCount, std::uint64_t frame, std::uint32_t viewKind, std::int32_t cascade, const void* db, const void* desc, bool zpass, bool shadow) {
    init();
    if (!on()) return;
    viewTick(frame, viewKind, cascade, db, desc);
    active = true;
    g_active = true;
    g_cat = g_lastCat = catOf(zpass, shadow, viewKind);
    g_zpass = zpass;
    g_shadow = shadow;
    g_drawCount = drawCount;
    ++g_opsInView[g_cat];
    g_phase = kOther;
    g_start = qpc();
}

OpScope::~OpScope() {
    if (!active) return;
    enter(kOther);
    g_win.cat[g_cat].devDraws += g_drawCount != nullptr ? *g_drawCount : 0;
    g_active = false;
}

IssueScope::IssueScope(int k) {
    if (!on() || k == 0) return;
    kind = k;
    t0 = qpc();
}

IssueScope::~IssueScope() {
    if (kind == 0) return;
    const std::int64_t d = qpc() - t0;
    if (kind == 1) g_win.cat[g_lastCat].ticks[kIssue] += d;
    else g_win.cat[g_lastCat].bbIssueTicks += d;
}

BbScope::BbScope(const std::size_t* drawCount, std::uint64_t frame, std::uint32_t viewKind, std::int32_t cascade, const void* db, const void* desc, bool zpass, bool shadow) {
    init();
    if (!on()) return;
    viewTick(frame, viewKind, cascade, db, desc);
    active = true;
    g_cat = g_lastCat = catOf(zpass, shadow, viewKind);
    g_drawCount = drawCount;
    ++g_bbInView[g_cat];
    g_start = qpc();
}

BbScope::~BbScope() {
    if (!active) return;
    CatStats& c = g_win.cat[g_cat];
    c.bbTicks += qpc() - g_start;
    ++c.bbOps;
    c.bbDraws += g_drawCount != nullptr ? *g_drawCount : 0;
}

void bbConstants(const void* ps, std::uint32_t desc, const void* rows, std::size_t bytes) {
    if (!on() || rows == nullptr) return;
    track(g_vc, kBbConst, reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(ps) ^ (static_cast<std::uintptr_t>(desc) * 0x9e3779b97f4a7c15ull >> 17)),
          hashBytes(rows, bytes));
}

void batches(std::uint32_t planned, std::uint32_t instances) {
    if (!g_active) return;
    CatStats& c = g_win.cat[g_cat];
    c.batchesToday += planned;
    c.batchInstances += instances;
}

void census(const OpInfo& o) {
    if (!g_active) return;
    const int prev = enter(kCensus);
    g_type = o.type;
    CatStats& cs = g_win.cat[g_cat];
    VcStats& vs = g_win.vc[g_vc];
    const bool zpass = g_zpass, shadow = g_shadow;
    ++cs.ops;
    ++cs.opsByType[o.type & 3];
    ++(o.hardware ? cs.hw : cs.sw);
    cs.entries += static_cast<std::uint64_t>(std::max(o.n, 0));
    // the render buffer's tables and the wind block: once per view
    if (o.rb != nullptr) {
        std::uint8_t rb[0x130];
        if (engine::safeRead(o.rb, rb, sizeof(rb))) {
            std::uint64_t h = hashBytes(rb + 0x38, 0xb8 - 0x38);
            h = h * 1099511628211ull ^ hashBytes(rb + 0xe8, 0x130 - 0xe8);
            track(g_vc, kRbTables, o.rb, h);
        }
    }
    if (o.admin != nullptr) {
        std::uint8_t w[192];
        if (engine::safeRead(o.admin + 0xac, w, sizeof(w))) track(g_vc, kWindMat, o.admin, hashBytes(w, sizeof(w)));
        if (engine::safeRead(o.admin + 0x6c, w, 64)) track(g_vc, kWindRock, o.admin, hashBytes(w, 64));
    }
    std::array<std::uint32_t, 64> counts{};
    std::uint32_t sunMaskOp = 0;
    for (std::int32_t i = 0; i < o.n; ++i) {
        const std::uint8_t* e = o.queue + 0x18 * static_cast<std::size_t>(i);
        const std::uint8_t* tree = nullptr;
        const std::uint8_t* illum = nullptr;
        std::memcpy(&tree, e, 8);
        std::memcpy(&illum, e + 8, 8);
        Snap<0x70> ts;
        ts.load(tree);
        if (!ts.ok) continue;
        const std::uint8_t flags = ts.at<std::uint8_t>(0x18);
        const std::int32_t l48 = ts.at<std::int32_t>(0x48), l50 = ts.at<std::int32_t>(0x50), l58 = ts.at<std::int32_t>(0x58), l5c = ts.at<std::int32_t>(0x5c);
        std::uint8_t& f = g_seen[tree];
        if (!(f & 1)) {
            f |= 1;
            if (flags & 0x10) ++vs.noEntity;
            track(g_vc, kLod48, tree, static_cast<std::uint32_t>(l48));
            track(g_vc, kLod50, tree, static_cast<std::uint32_t>(l50));
            track(g_vc, kLod58, tree, static_cast<std::uint32_t>(l58));
            track(g_vc, kLod5c, tree, static_cast<std::uint32_t>(l5c));
            track(g_vc, kRef60, tree, ts.at<std::uint32_t>(0x60));
            track(g_vc, kRef64, tree, ts.at<std::uint32_t>(0x64));
            track(g_vc, kVar6c, tree, ts.at<std::uint32_t>(0x6c));
            std::uint8_t lod[0x6c - 0x48 + 4];
            std::memcpy(lod, ts.b + 0x48, sizeof(lod));
            track(g_vc, kLodAny, tree, hashBytes(lod, sizeof(lod)));
            const std::uint8_t* entity = (flags & 0x10) == 0 ? ts.at<const std::uint8_t*>(0x10) : nullptr;
            float m[16];
            if (entity != nullptr && engine::safeRead(entity + 0x60, m, sizeof(m))) track(g_vc, kMatrix, tree, hashBytes(m, sizeof(m)));
        }
        if (o.type >= 2 && !(f & 4)) {
            f |= 4;
            ++vs.leafSeen;
            if (l58 > -1 && l5c > -1) ++vs.twoBuckets;
        }
        // the buckets: (op, type, LOD)
        const auto add = [&](std::int32_t lod) { ++counts[static_cast<std::size_t>(std::clamp(lod, 0, 63))]; };
        if (o.type <= 1) {
            const std::int32_t lod = o.type == 0 ? l48 : l50;
            if (lod >= 0) add(lod);
        } else if (shadow) {
            if (l58 >= 0) add(std::min(l58 + 1, o.leafLods - 1));
        } else {
            if (l58 > -1) add(l58);
            if (l5c > -1 && l5c != l58) add(l5c);
        }
        if (!zpass && illum != nullptr) {
            Snap<0x98> is;
            is.load(illum);
            if (is.ok) {
                if (!(f & 2)) {
                    f |= 2;
                    track(g_vc, kIllumPtr, tree, reinterpret_cast<std::uintptr_t>(illum));
                    std::uint8_t raw[0x110];
                    if (engine::safeRead(illum, raw, sizeof(raw))) track(g_vc, kIllumBytes, tree, hashBytes(raw, sizeof(raw)));
                }
                const std::uint8_t* list = is.at<const std::uint8_t*>(0);
                const bool sh = is.at<std::uint8_t>(0x90) != 0;
                const std::uint32_t ns = is.at<std::uint8_t>(0xd);
                const bool noSun = is.at<std::uint8_t>(0xc) != 0;
                bool dyn = false, sun = false;
                if (list != nullptr) {
                    std::uint8_t lb[0x35];
                    if (engine::safeRead(list, lb, sizeof(lb))) {
                        sun = lb[0] != 0 && !noSun;
                        dyn = lb[0x34] != 0;
                    }
                    g_lists.insert(list);
                }
                const std::size_t cls = list == nullptr ? 0 : dyn ? 1 : sh ? 2 : ns > 2 ? 3 : ns >= 1 ? 4 : 5;
                ++g_win.illum[cls];
                ++(sun ? g_win.sunOn : g_win.sunOff);
                sunMaskOp |= sun ? 2u : 1u;
            }
        } else if (!zpass) {
            ++g_win.illum[0];
            sunMaskOp |= 1u;
        }
    }
    std::uint32_t any = 0;
    for (std::size_t l = 0; l < counts.size(); ++l) {
        if (counts[l] == 0) continue;
        ++cs.buckets;
        cs.instances += counts[l];
        ++cs.hist[o.type & 3][static_cast<std::size_t>(binOf(counts[l]))];
        any += counts[l];
    }
    if (!zpass) g_sunMask |= sunMaskOp;
    if (any != 0) {
        // the F4 cut rule: one call per (pipeline proxy = shader, vertex buffer, index buffer, topology) and sun value
        const std::uint32_t suns = zpass ? 1u : (sunMaskOp == 0 ? 1u : sunMaskOp);
        for (std::uint32_t s = 0; s < 3; ++s) {
            if (zpass ? s != 0 : (s == 0 || (suns & s) == 0)) continue;
            std::uint64_t k = hashBytes(&o.shader, sizeof(o.shader));
            k = (k ^ hashBytes(&o.vb, sizeof(o.vb))) * 1099511628211ull;
            k = (k ^ hashBytes(&o.ib, sizeof(o.ib))) * 1099511628211ull;
            k = (k ^ o.primType) * 1099511628211ull;
            k = (k ^ s) * 1099511628211ull;
            g_calls[g_cat].insert(k);
        }
    }
    if (prev >= 0) enter(prev);
}

void draw(const void* vs, const void* ps, const void* s1, std::uint32_t s1Offset, std::uint32_t s1Stride, std::uint32_t s1Freq, std::uint32_t s0Freq) {
    if (!g_active) return;
    const int prev = enter(kCensus);
    CatStats& c = g_win.cat[g_cat];
    ++c.draws;
    if (s0Freq != 1 && s0Freq != 0) ++c.drawsFreq0;
    noteShader(vs, "vs");
    noteShader(ps, "ps");
    if (s1 != nullptr) {
        ++c.draws1;
        ++g_win.freq1[s1Freq];
        if (g_streamLogged.insert(s1).second) {
            auto* b = const_cast<d3d9::Buffer*>(static_cast<const d3d9::Buffer*>(s1));
            std::string content = "unreadable";
            const std::uint32_t size = b->size();
            if (s1Stride >= 4 && size >= s1Stride) {
                std::vector<std::uint8_t> data(std::min<std::uint32_t>(size, 1u << 16));
                if (b->readCpu(0, static_cast<std::uint32_t>(data.size()), data.data())) {
                    const std::uint32_t n = static_cast<std::uint32_t>(data.size()) / s1Stride;
                    std::uint32_t okF = 0, okU = 0;
                    for (std::uint32_t i = 0; i < n; ++i) {
                        float f;
                        std::uint32_t u;
                        std::memcpy(&f, data.data() + static_cast<std::size_t>(i) * s1Stride, 4);
                        std::memcpy(&u, data.data() + static_cast<std::size_t>(i) * s1Stride, 4);
                        okF += f == static_cast<float>(i) ? 1 : 0;
                        okU += u == i ? 1 : 0;
                    }
                    std::string first;
                    for (std::uint32_t i = 0; i < std::min<std::uint32_t>(n, 6); ++i) {
                        float f;
                        std::uint32_t u;
                        std::memcpy(&f, data.data() + static_cast<std::size_t>(i) * s1Stride, 4);
                        std::memcpy(&u, data.data() + static_cast<std::size_t>(i) * s1Stride, 4);
                        first += std::format("{}{:g}/{:#x}", first.empty() ? "" : " ", f, u);
                    }
                    content = std::format("{} elements read (of {}): float ramp 0..n-1 in {}, uint ramp in {}; first (float/hex): {}", n, size / s1Stride, okF, okU, first);
                    if (size > data.size()) content += " (first 64 KiB only)";
                }
            }
            spdlog::info("native: f4 stream1: buffer {:#x} first used by {} type {}: size {} bytes, stride {}, offset {}, frequency {:#x} ({} instances at {} copies), stream 0 divider {:#x}; {}",
                         reinterpret_cast<std::uintptr_t>(s1), kCatNames[g_lastCat], g_type, size, s1Stride, s1Offset, s1Freq, s1Freq & 0x3fffffffu,
                         (s1Freq & 0x80000000u) != 0 ? "per-instance" : "per-vertex", s0Freq, content);
        }
    }
    if (prev >= 0) enter(prev);
}

namespace {

void report() {
    ++g_ticks5;
    const Window& w = g_win;
    std::uint64_t mainViews = w.vc[0].views;
    // time: ms summed over the window per category and phase (v = views holding the category)
    for (int c = 0; c < kCats; ++c) {
        const CatStats& s = w.cat[c];
        std::string t;
        double total = 0;
        for (int p = 0; p < kPhases; ++p) {
            t += std::format("{}={:.3f} ", kPhaseNames[p], ms(s.ticks[p]));
            if (p != kCensus) total += ms(s.ticks[p]);
        }
        spdlog::info("native: f4 measure (5 s) time {}: v={} {}sum={:.3f} bb[ops={} v={} draws={} ms={:.3f} issue={:.3f}]", kCatNames[c], s.views, t, total, s.bbOps, s.bbViews,
                     s.bbDraws, ms(s.bbTicks), ms(s.bbIssueTicks));
    }
    for (int c = 0; c < kCats; ++c) {
        const CatStats& s = w.cat[c];
        spdlog::info("native: f4 measure (5 s) census {}: v={} ops={} types={}/{}/{}/{} hw={} sw={} entries={} instances={} buckets={} batchesToday={} batchInst={} f4Calls={} devDraws={} "
                     "draws={} stream1={} freq0={}",
                     kCatNames[c], s.views, s.ops, s.opsByType[0], s.opsByType[1], s.opsByType[2], s.opsByType[3], s.hw, s.sw, s.entries, s.instances, s.buckets, s.batchesToday,
                     s.batchInstances, s.callsF4, s.devDraws, s.draws, s.draws1, s.drawsFreq0);
        std::string h;
        for (int t = 0; t < 4; ++t) {
            std::string row;
            for (int b = 0; b < kBins; ++b) {
                if (s.hist[t][b] != 0) row += std::format("{}:{} ", kBinNames[b], s.hist[t][b]);
            }
            if (!row.empty()) h += std::format("t{}[{}] ", t, row);
        }
        spdlog::info("native: f4 measure (5 s) hist {}: {}", kCatNames[c], h.empty() ? "-" : h);
    }
    for (int v = 0; v < kVcs; ++v) {
        const VcStats& s = w.vc[v];
        if (s.views == 0) continue;
        spdlog::info("native: f4 measure (5 s) trees {}: views={} unique={} noEntity={} leafSeen={} twoBuckets={}", kVcNames[v], s.views, s.uniqueTrees, s.noEntity, s.leafSeen,
                     s.twoBuckets);
    }
    spdlog::info("native: f4 measure (5 s) illum: noList={} dynLights={} staticSh={} staticGt2={} static1to2={} plain={} sunOn={} sunOff={} mainViewSun[none={} off={} on={} both={}] distinctLists={} mainViews={}",
                 w.illum[0], w.illum[1], w.illum[2], w.illum[3], w.illum[4], w.illum[5], w.sunOn, w.sunOff, w.sunMask[0], w.sunMask[1], w.sunMask[2], w.sunMask[3], w.distinctLists,
                 mainViews);
    for (int v = 0; v < kVcs; ++v) {
        std::string row;
        for (int t = 0; t < kTrackers; ++t) {
            const TrackStats& s = w.vc[v].trk[t];
            if (s.pairs + s.fresh == 0) continue;
            row += std::format("{}={}/{}({}) ", kTrackNames[t], s.changed, s.pairs, s.fresh);
        }
        if (!row.empty()) spdlog::info("native: f4 measure (5 s) inval {}: {}", kVcNames[v], row);
    }
    std::string f1;
    for (const auto& [f, n] : w.freq1) f1 += std::format("{:#x}:{} ", f, n);
    spdlog::info("native: f4 measure (5 s) stream1 freq: {}", f1.empty() ? "-" : f1);
    g_win = Window{};
}

}  // namespace

}  // namespace rvk::native::f4m
