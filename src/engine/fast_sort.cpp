#include "engine/fast_sort.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "engine/memread.h"

namespace rvk::engine::fastsort {
namespace {

// eCRenderOp2: key (24-bit view depth) at +0x10, sort id at +0x14 (native/frame_desc.h RawOp)
struct Op {
    std::uint64_t a, b, c;
    std::uint32_t key() const { return static_cast<std::uint32_t>(c); }
    std::uint32_t sortId() const { return static_cast<std::uint32_t>(c >> 32); }
};
static_assert(sizeof(Op) == 24);

struct CmpZ {
    int operator()(const Op* x, const Op* y) const { return x->key() < y->key() ? -1 : x->key() > y->key() ? 1 : 0; }
};
struct CmpSolid {
    int operator()(const Op* x, const Op* y) const { return x->c < y->c ? -1 : x->c > y->c ? 1 : 0; }
};
struct CmpAlpha {
    int operator()(const Op* x, const Op* y) const {
        constexpr std::uint32_t kBand = 0xfff00000u, kHigh = 0x3e800000u;
        const std::uint32_t la = x->sortId() & kBand, lb = y->sortId() & kBand;
        if (la >= kHigh || lb >= kHigh) {
            if (la > lb) return -1;
            if (la < lb) return 1;
        }
        if (y->key() < x->key()) return -1;
        if (y->key() > x->key()) return 1;
        if (x->sortId() < y->sortId()) return -1;
        if (x->sortId() > y->sortId()) return 1;
        return 0;
    }
};

inline void swapOps(Op* x, Op* y) {
    const Op t = *x;
    *x = *y;
    *y = t;
}

// The CRT's qsort (Visual Studio 2017, read from the disassembly in Engine.dll; sizes <= 8 by selection sort, else median of
// three and a two-sided partition), with the comparator inlined. Comparator arguments are in the CRT's order.
template <class C>
void crtQsort(Op* base, std::size_t num) {
    if (num < 2) return;
    C cmp;
    Op* lo = base;
    Op* hi = base + (num - 1);
    Op* loStk[62];
    Op* hiStk[62];
    int stk = 0;
    for (;;) {
        const std::size_t size = static_cast<std::size_t>(hi - lo) + 1;
        if (size <= 8) {
            for (Op* end = hi; lo < end; --end) {
                Op* mx = lo;
                for (Op* p = lo + 1; p <= end; ++p) {
                    if (cmp(p, mx) > 0) mx = p;
                }
                if (mx != end) swapOps(mx, end);
            }
        } else {
            Op* mid = lo + size / 2;
            if (cmp(lo, mid) > 0) swapOps(lo, mid);
            if (cmp(lo, hi) > 0) swapOps(lo, hi);
            if (cmp(mid, hi) > 0) swapOps(mid, hi);
            Op* loguy = lo;
            Op* higuy = hi;
            for (;;) {
                if (mid > loguy) {
                    do {
                        ++loguy;
                    } while (loguy < mid && cmp(loguy, mid) <= 0);
                }
                if (mid <= loguy) {
                    do {
                        ++loguy;
                    } while (loguy <= hi && cmp(loguy, mid) <= 0);
                }
                do {
                    --higuy;
                } while (higuy > mid && cmp(higuy, mid) > 0);
                if (higuy < loguy) break;
                swapOps(loguy, higuy);
                if (mid == higuy) mid = loguy;
            }
            ++higuy;
            if (mid < higuy) {
                do {
                    --higuy;
                } while (higuy > mid && cmp(higuy, mid) == 0);
            }
            if (mid >= higuy) {
                do {
                    --higuy;
                } while (higuy > lo && cmp(higuy, mid) == 0);
            }
            if (higuy - lo >= hi - loguy) {
                if (lo < higuy) {
                    loStk[stk] = lo;
                    hiStk[stk] = higuy;
                    ++stk;
                }
                if (loguy < hi) {
                    lo = loguy;
                    continue;
                }
            } else {
                if (loguy < hi) {
                    loStk[stk] = loguy;
                    hiStk[stk] = hi;
                    ++stk;
                }
                if (lo < higuy) {
                    hi = higuy;
                    continue;
                }
            }
        }
        if (--stk < 0) return;
        lo = loStk[stk];
        hi = hiStk[stk];
    }
}

using CmpFn = int (*)(const void*, const void*);
using QsortFn = void (*)(void*, std::size_t, std::size_t, CmpFn);
Hook<QsortFn> g_qsort;
std::atomic<int> g_mode{1};
std::uintptr_t g_cmpZ = 0, g_cmpSolid = 0, g_cmpAlpha = 0;
std::atomic<std::uint64_t> g_mismatch{0}, g_checked{0};
// cost of the sorts of the op lists (mode 0: the engine's qsort, mode 1: ours), logged every 5 s (-Perf)
using Clock = std::chrono::steady_clock;
std::uint64_t g_ns = 0, g_lists = 0, g_ops = 0;
Clock::time_point g_logAt = Clock::now();
void account(Clock::time_point t0, std::size_t num, int mode) {
    const auto t1 = Clock::now();
    g_ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    ++g_lists;
    g_ops += num;
    if (t1 - g_logAt >= std::chrono::seconds(5)) {
        const double secs = std::chrono::duration<double>(t1 - g_logAt).count();
        spdlog::info("perf: op list sorts ({}): {} lists / {} ops per s, {:.3f} ms per s ({:.2f} us per list)", mode == 0 ? "engine qsort" : "typed", static_cast<std::uint64_t>(g_lists / secs),
                     static_cast<std::uint64_t>(g_ops / secs), g_ns * 1e-6 / secs, g_lists ? g_ns * 1e-3 / g_lists : 0.0);
        g_ns = g_lists = g_ops = 0;
        g_logAt = t1;
    }
}

void detourQsort(void* base, std::size_t num, std::size_t width, CmpFn cmp) {
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode == 0 && width == sizeof(Op) && num > 1 && base != nullptr) {
        const auto c = reinterpret_cast<std::uintptr_t>(cmp);
        if (c == g_cmpZ || c == g_cmpSolid || c == g_cmpAlpha) {
            const auto t0 = Clock::now();
            g_qsort.original(base, num, width, cmp);
            account(t0, num, 0);
            return;
        }
    }
    if (mode != 0 && width == sizeof(Op) && num > 1 && base != nullptr) {
        const auto c = reinterpret_cast<std::uintptr_t>(cmp);
        if (c == g_cmpZ || c == g_cmpSolid || c == g_cmpAlpha) {
            const Cmp kind = c == g_cmpZ ? Cmp::Z : c == g_cmpSolid ? Cmp::Solid : Cmp::Alpha;
            if (mode == 2) {
                std::vector<Op> copy(static_cast<Op*>(base), static_cast<Op*>(base) + num);
                g_qsort.original(base, num, width, cmp);
                sortOps(copy.data(), num, kind);
                if (const auto n = g_checked.fetch_add(1) + 1; n % 2000 == 0) {
                    spdlog::info("engine: fast sort: {} lists compared with the engine qsort, {} differ", n, g_mismatch.load());
                }
                if (std::memcmp(copy.data(), base, num * sizeof(Op)) != 0 && g_mismatch.fetch_add(1) < 4) {
                    spdlog::error("engine: fast sort: result differs from the engine qsort ({} ops, comparator {})", num, static_cast<int>(kind));
                }
                return;
            }
            const auto t0 = Clock::now();
            sortOps(base, num, kind);
            account(t0, num, 1);
            return;
        }
    }
    g_qsort.original(base, num, width, cmp);
}

}  // namespace

void sortOps(void* base, std::size_t count, Cmp cmp) {
    auto* p = static_cast<Op*>(base);
    switch (cmp) {
        case Cmp::Z: crtQsort<CmpZ>(p, count); break;
        case Cmp::Solid: crtQsort<CmpSolid>(p, count); break;
        case Cmp::Alpha: crtQsort<CmpAlpha>(p, count); break;
    }
}

bool installed() { return g_qsort.installed(); }

void setMode(int mode) {
    g_mode.store(mode, std::memory_order_relaxed);
    if (mode == 2) spdlog::info("engine: fast sort: compare mode (every sorted list is checked against the engine qsort)");
}

bool install() {
    constexpr std::uint32_t kQsortRva = 0x4a4aa0, kCmpZRva = 0x66e670, kCmpSolidRva = 0x66e6a0, kCmpAlphaRva = 0x66e600;
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(L"Engine.dll"));
    if (base == nullptr) return false;
    // the CRT qsort: push r12, r13, r14; sub rsp, 0x450 (native/frame_tap.cpp checks the same bytes)
    constexpr std::array<std::uint8_t, 13> kPrologue{0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x81, 0xec, 0x50, 0x04, 0x00, 0x00};
    // the comparators' first bytes (native/frame_tap.cpp kSortSites)
    constexpr std::array<std::uint8_t, 10> pz{0x44, 0x8b, 0x41, 0x10, 0x8b, 0x4a, 0x10, 0x44, 0x3b, 0xc1};
    constexpr std::array<std::uint8_t, 10> ps{0x4c, 0x8b, 0x41, 0x10, 0x48, 0x8b, 0x4a, 0x10, 0x4c, 0x3b};
    constexpr std::array<std::uint8_t, 10> pa{0x44, 0x8b, 0x51, 0x14, 0x45, 0x8b, 0xca, 0x41, 0x81, 0xe1};
    auto matches = [&](std::uint32_t rva, const auto& bytes) {
        std::array<std::uint8_t, 16> got{};
        if (!safeRead(base + rva, got.data(), bytes.size())) return false;
        return std::memcmp(got.data(), bytes.data(), bytes.size()) == 0;
    };
    if (!matches(kQsortRva, kPrologue) || !matches(kCmpZRva, pz) || !matches(kCmpSolidRva, ps) || !matches(kCmpAlphaRva, pa)) {
        spdlog::warn("engine: fast sort: qsort / comparator bytes differ from the expected build: not installed");
        return false;
    }
    g_cmpZ = reinterpret_cast<std::uintptr_t>(base + kCmpZRva);
    g_cmpSolid = reinterpret_cast<std::uintptr_t>(base + kCmpSolidRva);
    g_cmpAlpha = reinterpret_cast<std::uintptr_t>(base + kCmpAlphaRva);
    return g_qsort.install("qsort", reinterpret_cast<QsortFn>(const_cast<std::uint8_t*>(base + kQsortRva)), &detourQsort);
}

}  // namespace rvk::engine::fastsort
