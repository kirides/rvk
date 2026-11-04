#include "engine/load_accel.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "engine/memread.h"

namespace rvk::engine::loadaccel {

// ---------------------------------------------------------------------------------------------------------------------------
// the absorption image (pure)

namespace {

constexpr std::uint32_t kTaps = 32;

inline std::uint32_t packAbsorb(std::uint32_t left, std::uint32_t right, std::uint32_t down, std::uint32_t up, float scale) {
    const auto cv = [scale](std::uint32_t sum) { return static_cast<std::uint32_t>(static_cast<std::int64_t>(static_cast<float>(sum) * scale)); };
    return (((cv(up) << 8 | cv(right)) << 8 | cv(down)) << 8) | cv(left);
}

}  // namespace

void absorbNaive(const std::uint8_t* src, std::uint8_t* dst, std::uint32_t w, std::uint32_t h, std::uint32_t pitch, float scale) {
    if (w == 0 || h == 0) return;
    const auto B = [&](std::uint32_t row, std::uint32_t col) -> std::uint32_t {
        return src[static_cast<std::size_t>(row) * pitch + static_cast<std::size_t>(col) * 4 + 2];
    };
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint32_t right = 0, left = 0, down = 0, up = 0;
            for (std::uint32_t k = 0; k < kTaps; ++k) {
                right += B(y, (x + k) % w);
                left += B(y, (x - k) % w);  // the engine's unsigned wrap
                down += B((y + k) % h, x);
                up += B((y - k) % h, x);
            }
            const std::uint32_t v = packAbsorb(left, right, down, up, scale);
            std::memcpy(dst + static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(x) * 4, &v, 4);
        }
    }
}

void absorbFast(const std::uint8_t* src, std::uint8_t* dst, std::uint32_t w, std::uint32_t h, std::uint32_t pitch, float scale) {
    if (w == 0 || h == 0) return;
    const auto B = [&](std::uint32_t row, std::uint32_t col) -> std::uint32_t {
        return src[static_cast<std::size_t>(row) * pitch + static_cast<std::size_t>(col) * 4 + 2];
    };
    std::vector<std::uint32_t> right(w), left(w), down(w), up(w);
    // the vertical sums of row 0: down = rows (0 + k) % h
    for (std::uint32_t x = 0; x < w; ++x) {
        std::uint32_t d = 0;
        for (std::uint32_t k = 0; k < kTaps; ++k) d += B(k % h, x);
        down[x] = d;
    }
    for (std::uint32_t y = 0; y < h; ++y) {
        // up = rows (y - k) with the engine's unsigned wrap: direct while the window reaches below row 0, then sliding
        if (y < kTaps) {
            for (std::uint32_t x = 0; x < w; ++x) {
                std::uint32_t u = 0;
                for (std::uint32_t k = 0; k < kTaps; ++k) u += B((y - k) % h, x);
                up[x] = u;
            }
        } else {
            for (std::uint32_t x = 0; x < w; ++x) up[x] += B(y, x) - B(y - kTaps, x);
        }
        // the row's horizontal sums
        {
            std::uint32_t r = 0;
            for (std::uint32_t k = 0; k < kTaps; ++k) r += B(y, k % w);
            for (std::uint32_t x = 0; x < w; ++x) {
                right[x] = r;
                r += B(y, (x + kTaps) % w) - B(y, x);
            }
            const std::uint32_t direct = std::min(w, kTaps);
            for (std::uint32_t x = 0; x < direct; ++x) {
                std::uint32_t l = 0;
                for (std::uint32_t k = 0; k < kTaps; ++k) l += B(y, (x - k) % w);
                left[x] = l;
            }
            for (std::uint32_t x = kTaps; x < w; ++x) left[x] = left[x - 1] + B(y, x) - B(y, x - kTaps);
        }
        std::uint8_t* out = dst + static_cast<std::size_t>(y) * pitch;
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t v = packAbsorb(left[x], right[x], down[x], up[x], scale);
            std::memcpy(out + static_cast<std::size_t>(x) * 4, &v, 4);
        }
        // down for row y + 1: drop row y, add row (y + 32) % h
        if (y + 1 < h) {
            for (std::uint32_t x = 0; x < w; ++x) down[x] += B((y + kTaps) % h, x) - B(y, x);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// engine hooks

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<int> g_readKiB{0};
std::atomic<int> g_absorbMode{0};
std::atomic<bool> g_stats{false};
std::atomic<bool> g_verify{false};
std::atomic<bool> g_installed{false};

struct Counters {
    std::atomic<std::uint64_t> reads{0}, hits{0}, refills{0}, passThrough{0}, bytesServed{0}, bytesFetched{0};
    std::atomic<std::uint64_t> readNs{0}, verified{0}, verifyDiffer{0};
    std::atomic<std::uint64_t> sounds{0}, soundNs{0}, soundMaxNs{0};
    std::atomic<std::uint64_t> absorbs{0}, absorbNs{0}, absorbPixels{0}, absorbCompared{0}, absorbDiffer{0};
};
Counters g_c;

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

// ---- packed file read buffer ----

using PackedReadFn = std::uint64_t (*)(std::uint8_t*, void*, std::uint32_t);
Hook<PackedReadFn> g_packed;

struct Entry {
    const void* self = nullptr;
    std::uint64_t vol = 0;
    std::int64_t base = 0;
    std::uint32_t size = 0;
    std::int64_t start = 0;  // file position (inside the packed file) of buf[0]
    std::uint32_t len = 0;
    std::uint32_t chunk = 2048;  // the next refill's size: doubles with every sequential miss
    std::uint64_t lastUseMs = 0;
    std::vector<std::uint8_t> buf;
};
constexpr std::size_t kEntries = 32;
std::array<Entry, kEntries> g_entries;
std::mutex g_entryMutex;

// The stream subobject `self` (rcx of the engine's function) holds, at negative offsets: the volume's file stream (-0x38), the
// packed file's base offset in the volume (-0x30), the read position (-0x28, 64 bit) and the size (-0x20, 32 bit). The engine's
// function seeks the volume to base + position and reads, then advances the position: two system calls per Read.
std::uint64_t detourPackedRead(std::uint8_t* self, void* dst, std::uint32_t count) {
    const int kib = g_readKiB.load(std::memory_order_relaxed);
    const std::uint32_t cap = static_cast<std::uint32_t>(std::clamp(kib, 0, 1024)) * 1024u;
    const bool stats = g_stats.load(std::memory_order_relaxed);
    const std::uint64_t t0 = stats ? nowNs() : 0;
    struct Timer {
        bool on;
        std::uint64_t t0;
        ~Timer() {
            if (on) g_c.readNs.fetch_add(nowNs() - t0, std::memory_order_relaxed);
        }
    } timer{stats, t0};
    g_c.reads.fetch_add(1, std::memory_order_relaxed);
    if (cap == 0 || count == 0 || count > cap / 4 || dst == nullptr) {
        g_c.passThrough.fetch_add(1, std::memory_order_relaxed);
        return g_packed.original(self, dst, count);
    }
    const std::uint64_t vol = *reinterpret_cast<const std::uint64_t*>(self - 0x38);
    const std::int64_t base = *reinterpret_cast<const std::int64_t*>(self - 0x30);
    std::int64_t* posField = reinterpret_cast<std::int64_t*>(self - 0x28);
    const std::int64_t pos = *posField;
    const std::uint32_t size = *reinterpret_cast<const std::uint32_t*>(self - 0x20);
    if (vol == 0 || pos < 0 || static_cast<std::uint64_t>(pos) + count > size) {  // the engine's own error / clamp path
        g_c.passThrough.fetch_add(1, std::memory_order_relaxed);
        return g_packed.original(self, dst, count);
    }
    const std::uint64_t nowMs = GetTickCount64();
    const std::lock_guard lock(g_entryMutex);
    Entry& e = g_entries[(reinterpret_cast<std::uintptr_t>(self) >> 4) % kEntries];
    const bool same = e.self == self && e.vol == vol && e.base == base && e.size == size && nowMs - e.lastUseMs < 1000;
    if (same && pos >= e.start && pos + count <= e.start + static_cast<std::int64_t>(e.len)) {
        std::memcpy(dst, e.buf.data() + (pos - e.start), count);
        *posField = pos + count;
        e.lastUseMs = nowMs;
        if (g_verify.load(std::memory_order_relaxed)) {  // the engine's own read of the same bytes (it leaves the position where ours did)
            std::vector<std::uint8_t> ref(count);
            *posField = pos;
            const std::uint64_t n = g_packed.original(self, ref.data(), count);
            const bool ok = n == count && std::memcmp(ref.data(), dst, count) == 0 && *posField == pos + count;
            g_c.verified.fetch_add(1, std::memory_order_relaxed);
            if (!ok && g_c.verifyDiffer.fetch_add(1, std::memory_order_relaxed) < 4) {
                spdlog::error("engine: packed read buffer: bytes differ from the engine's read (pos {}, {} bytes)", pos, count);
            }
        }
        g_c.hits.fetch_add(1, std::memory_order_relaxed);
        g_c.bytesServed.fetch_add(count, std::memory_order_relaxed);
        return count;
    }
    // miss: read ahead. The first miss of a stream reads little; every sequential miss doubles the read up to the cap.
    std::uint32_t chunk = 2048;
    if (same && pos >= e.start && pos <= e.start + static_cast<std::int64_t>(e.len) + 64) chunk = std::min(cap, e.chunk * 2);
    chunk = std::min<std::uint32_t>(std::max(chunk, count), size - static_cast<std::uint32_t>(pos));
    if (e.buf.size() < cap) e.buf.resize(cap);
    const std::uint64_t got = g_packed.original(self, e.buf.data(), chunk);  // seeks base + pos, reads, pos += got
    if (got < count) {
        // short read (end of file or an I/O error): the engine's behaviour for exactly this request
        e.self = nullptr;
        *posField = pos;
        return g_packed.original(self, dst, count);
    }
    e.self = self;
    e.vol = vol;
    e.base = base;
    e.size = size;
    e.start = pos;
    e.len = static_cast<std::uint32_t>(got);
    e.chunk = chunk;
    e.lastUseMs = nowMs;
    std::memcpy(dst, e.buf.data(), count);
    *posField = pos + count;
    g_c.refills.fetch_add(1, std::memory_order_relaxed);
    g_c.bytesFetched.fetch_add(got, std::memory_order_relaxed);
    g_c.bytesServed.fetch_add(count, std::memory_order_relaxed);
    return count;
}

// ---- FMOD createSound (statistics only) ----

using CreateSoundFn = int (*)(void*, const char*, unsigned, void*, void**);
Hook<CreateSoundFn> g_createSound;

int detourCreateSound(void* system, const char* name, unsigned mode, void* exinfo, void** sound) {
    if (!g_stats.load(std::memory_order_relaxed)) return g_createSound.original(system, name, mode, exinfo, sound);
    const std::uint64_t t0 = nowNs();
    const int r = g_createSound.original(system, name, mode, exinfo, sound);
    const std::uint64_t dt = nowNs() - t0;
    g_c.sounds.fetch_add(1, std::memory_order_relaxed);
    g_c.soundNs.fetch_add(dt, std::memory_order_relaxed);
    std::uint64_t mx = g_c.soundMaxNs.load(std::memory_order_relaxed);
    while (dt > mx && !g_c.soundMaxNs.compare_exchange_weak(mx, dt, std::memory_order_relaxed)) {
    }
    return r;
}

// ---- the absorption image loop (mid-function patch of eCSkydome_PS::CreateAbsorbationImage) ----

constexpr std::uint32_t kAbsorbEnterRva = 0x6e34a7;  // mov eax,[rsp+0x64]; xor esi,esi  (before the loop)
constexpr std::uint32_t kAbsorbExitRva = 0x6e3a15;   // mov rbx,[rsp+0x50]               (after the loop)
constexpr std::uint32_t kAbsorbLoopTestRva = 0x6e34ad;
constexpr std::uint32_t kAbsorbAfterExitRva = 0x6e3a1a;
constexpr std::uint32_t kAbsorbSkipRva = 0x6e3a36;
constexpr std::uint32_t kAbsorbScaleRva = 0x9f1590;  // the float every sum is multiplied with

const std::uint8_t* g_engineBase = nullptr;
std::uint64_t g_absorbT0 = 0;
std::vector<std::uint8_t> g_absorbScratch;  // mode 2: our image, compared after the engine's loop
std::uint32_t g_absW = 0, g_absH = 0, g_absPitch = 0;

struct AbsorbFrame {
    std::uint32_t w = 0, h = 0, pitch = 0;
    const std::uint8_t* src = nullptr;
    std::uint8_t* dst = nullptr;
    bool ok = false;
};

// the locals of CreateAbsorbationImage: layers desc width / height at rsp+0x60 / +0x64, the pitch at rbp+0x88, the two locked
// pixel pointers at rbp-0x48 (source) and rbp-0x40 (destination)
AbsorbFrame readFrame(const std::uint8_t* rsp, const std::uint8_t* rbp) {
    AbsorbFrame f;
    std::uint64_t ps = 0, pd = 0;
    if (!safeLoad4(rsp + 0x60, f.w) || !safeLoad4(rsp + 0x64, f.h) || !safeLoad4(rbp + 0x88, f.pitch) || !safeLoad8(rbp - 0x48, ps) ||
        !safeLoad8(rbp - 0x40, pd)) {
        return f;
    }
    f.src = reinterpret_cast<const std::uint8_t*>(ps);
    f.dst = reinterpret_cast<std::uint8_t*>(pd);
    f.ok = f.w >= 1 && f.w <= 8192 && f.h >= 1 && f.h <= 8192 && f.pitch >= f.w * 4ull && f.pitch <= 65536 * 4ull &&
           static_cast<std::uint64_t>(f.pitch) * f.h < (1ull << 30) && f.src != nullptr && f.dst != nullptr && f.src != f.dst;
    return f;
}

float scaleOf() {
    float s = 0.0f;
    if (g_engineBase == nullptr || !safeRead(g_engineBase + kAbsorbScaleRva, &s, sizeof(s))) return 0.0f;
    return s;
}

// returns 1 = the destination is filled, skip the engine's loop; 0 = run the engine's loop
std::uintptr_t rvkAbsorbEnter(const std::uint8_t* rsp, const std::uint8_t* rbp) {
    g_absorbT0 = nowNs();
    const int mode = g_absorbMode.load(std::memory_order_relaxed);
    if (mode == 0) return 0;
    const AbsorbFrame f = readFrame(rsp, rbp);
    const float scale = scaleOf();
    if (!f.ok || !(scale > 0.0f && scale < 1.0f)) {
        spdlog::warn("engine: absorption image: unexpected frame ({}x{} pitch {}, scale {}): the engine's loop runs", f.w, f.h, f.pitch, scale);
        return 0;
    }
    if (mode == 2) {
        g_absW = f.w;
        g_absH = f.h;
        g_absPitch = f.pitch;
        g_absorbScratch.assign(static_cast<std::size_t>(f.pitch) * f.h, 0);
        absorbFast(f.src, g_absorbScratch.data(), f.w, f.h, f.pitch, scale);
        return 0;
    }
    absorbFast(f.src, f.dst, f.w, f.h, f.pitch, scale);
    g_c.absorbs.fetch_add(1, std::memory_order_relaxed);
    g_c.absorbPixels.fetch_add(static_cast<std::uint64_t>(f.w) * f.h, std::memory_order_relaxed);
    g_c.absorbNs.fetch_add(nowNs() - g_absorbT0, std::memory_order_relaxed);
    return 1;
}

// after the engine's loop (all modes)
void rvkAbsorbExit(const std::uint8_t* rsp, const std::uint8_t* rbp) {
    const int mode = g_absorbMode.load(std::memory_order_relaxed);
    const std::uint64_t dt = nowNs() - g_absorbT0;
    if (mode == 2 && !g_absorbScratch.empty()) {
        const AbsorbFrame f = readFrame(rsp, rbp);
        if (f.ok && f.w == g_absW && f.h == g_absH && f.pitch == g_absPitch) {
            std::uint64_t differ = 0;
            for (std::uint32_t y = 0; y < f.h; ++y) {
                differ += std::memcmp(f.dst + static_cast<std::size_t>(y) * f.pitch, g_absorbScratch.data() + static_cast<std::size_t>(y) * f.pitch,
                                      static_cast<std::size_t>(f.w) * 4) != 0;
            }
            g_c.absorbCompared.fetch_add(1, std::memory_order_relaxed);
            g_c.absorbDiffer.fetch_add(differ, std::memory_order_relaxed);
            spdlog::info("engine: absorption image {}x{} (pitch {}): engine loop {:.1f} ms, rows differing from the sliding-window result: {}", f.w, f.h,
                         f.pitch, dt * 1e-6, differ);
        }
        g_absorbScratch.clear();
    } else if (mode == 0) {
        g_c.absorbs.fetch_add(1, std::memory_order_relaxed);
        g_c.absorbNs.fetch_add(dt, std::memory_order_relaxed);
        const AbsorbFrame f = readFrame(rsp, rbp);
        if (f.ok) g_c.absorbPixels.fetch_add(static_cast<std::uint64_t>(f.w) * f.h, std::memory_order_relaxed);
    }
}

void* allocNear(const void* target, std::size_t size) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const std::uintptr_t gran = si.dwAllocationGranularity;
    const auto t = reinterpret_cast<std::uintptr_t>(target);
    for (std::uintptr_t d = gran; d < 0x70000000; d += gran) {
        for (const std::uintptr_t a : {(t & ~(gran - 1)) - d, (t & ~(gran - 1)) + d}) {
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return p;
        }
    }
    return nullptr;
}

struct Emitter {
    std::uint8_t* p;
    std::uint8_t* start;
    void bytes(std::initializer_list<std::uint8_t> b) {
        for (const auto v : b) *p++ = v;
    }
    void imm64(std::uint64_t v) {
        std::memcpy(p, &v, 8);
        p += 8;
    }
    std::size_t size() const { return static_cast<std::size_t>(p - start); }
};

bool writeCode(void* where, const void* bytes, std::size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(where, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(where, bytes, n);
    VirtualProtect(where, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), where, n);
    return true;
}

bool installAbsorb(const std::uint8_t* base) {
    static constexpr std::array<std::uint8_t, 6> kEnterBytes{0x8b, 0x44, 0x24, 0x64, 0x33, 0xf6};
    static constexpr std::array<std::uint8_t, 5> kExitBytes{0x48, 0x8b, 0x5c, 0x24, 0x50};
    static constexpr std::array<std::uint8_t, 10> kCheckBytes{0x85, 0xc0, 0x0f, 0x84, 0x81, 0x05, 0x00, 0x00, 0x4c, 0x89};
    std::array<std::uint8_t, 10> got{};
    if (!safeRead(base + kAbsorbEnterRva, got.data(), kEnterBytes.size()) || std::memcmp(got.data(), kEnterBytes.data(), kEnterBytes.size()) != 0 ||
        !safeRead(base + kAbsorbExitRva, got.data(), kExitBytes.size()) || std::memcmp(got.data(), kExitBytes.data(), kExitBytes.size()) != 0 ||
        !safeRead(base + kAbsorbLoopTestRva, got.data(), kCheckBytes.size()) || std::memcmp(got.data(), kCheckBytes.data(), 8) != 0) {
        spdlog::warn("engine: absorption image: loop bytes differ from the expected build: not patched");
        return false;
    }
    auto* mem = static_cast<std::uint8_t*>(allocNear(base + kAbsorbEnterRva, 4096));
    if (mem == nullptr) {
        spdlog::warn("engine: absorption image: no code memory near Engine.dll");
        return false;
    }
    const auto abs = [&](std::uint32_t rva) { return reinterpret_cast<std::uint64_t>(base + rva); };
    // stub A (before the loop): call rvkAbsorbEnter(rsp, rbp); 1 = done (reload rbx / rdi, jump behind the loop), 0 = run the loop
    Emitter a{mem, mem};
    a.bytes({0x40, 0xf6, 0xc4, 0x0f});        // test spl, 15
    a.bytes({0x75, 0x00});                    // jnz orig (patched below)
    std::uint8_t* jnzAlign = a.p - 1;
    a.bytes({0x48, 0x89, 0xe1});              // mov rcx, rsp
    a.bytes({0x48, 0x89, 0xea});              // mov rdx, rbp
    a.bytes({0x48, 0xb8});                    // mov rax, imm64
    a.imm64(reinterpret_cast<std::uint64_t>(&rvkAbsorbEnter));
    a.bytes({0xff, 0xd0});                    // call rax
    a.bytes({0x85, 0xc0});                    // test eax, eax
    a.bytes({0x75, 0x00});                    // jnz skip (patched below)
    std::uint8_t* jnzSkip = a.p - 1;
    *jnzAlign = static_cast<std::uint8_t>(a.p - (jnzAlign + 1));
    a.bytes({0x8b, 0x44, 0x24, 0x64});        // orig: mov eax,[rsp+0x64]
    a.bytes({0x33, 0xf6});                    // xor esi, esi
    a.bytes({0x49, 0xbb});                    // mov r11, imm64
    a.imm64(abs(kAbsorbLoopTestRva));
    a.bytes({0x41, 0xff, 0xe3});              // jmp r11
    *jnzSkip = static_cast<std::uint8_t>(a.p - (jnzSkip + 1));
    a.bytes({0x48, 0x8b, 0x5c, 0x24, 0x50});  // skip: mov rbx,[rsp+0x50]
    a.bytes({0x48, 0x8b, 0x7d, 0xb0});        // mov rdi,[rbp-0x50]
    a.bytes({0x49, 0xbb});
    a.imm64(abs(kAbsorbSkipRva));
    a.bytes({0x41, 0xff, 0xe3});
    // stub B (after the loop): call rvkAbsorbExit(rsp, rbp), then the instruction it replaced
    std::uint8_t* b = mem + 256;
    Emitter e{b, b};
    e.bytes({0x40, 0xf6, 0xc4, 0x0f});        // test spl, 15
    e.bytes({0x75, 0x00});                    // jnz tail
    std::uint8_t* jnzTail = e.p - 1;
    e.bytes({0x48, 0x89, 0xe1});
    e.bytes({0x48, 0x89, 0xea});
    e.bytes({0x48, 0xb8});
    e.imm64(reinterpret_cast<std::uint64_t>(&rvkAbsorbExit));
    e.bytes({0xff, 0xd0});
    *jnzTail = static_cast<std::uint8_t>(e.p - (jnzTail + 1));
    e.bytes({0x48, 0x8b, 0x5c, 0x24, 0x50});  // mov rbx,[rsp+0x50]
    e.bytes({0x49, 0xbb});
    e.imm64(abs(kAbsorbAfterExitRva));
    e.bytes({0x41, 0xff, 0xe3});
    FlushInstructionCache(GetCurrentProcess(), mem, 4096);
    const auto rel32 = [](const std::uint8_t* from, const std::uint8_t* to) {
        return static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(to) - reinterpret_cast<std::intptr_t>(from));
    };
    std::array<std::uint8_t, 6> patchA{0xe9, 0, 0, 0, 0, 0x90};
    const std::int32_t da = rel32(base + kAbsorbEnterRva + 5, mem);
    std::memcpy(&patchA[1], &da, 4);
    std::array<std::uint8_t, 5> patchB{0xe9, 0, 0, 0, 0};
    const std::int32_t db = rel32(base + kAbsorbExitRva + 5, b);
    std::memcpy(&patchB[1], &db, 4);
    if (!writeCode(const_cast<std::uint8_t*>(base + kAbsorbEnterRva), patchA.data(), patchA.size()) ||
        !writeCode(const_cast<std::uint8_t*>(base + kAbsorbExitRva), patchB.data(), patchB.size())) {
        spdlog::warn("engine: absorption image: could not write the patch");
        return false;
    }
    return true;
}

std::uint64_t g_logAt = 0;
Counters g_last;

}  // namespace

void setReadCacheKiB(int kib) {
    g_readKiB.store(std::clamp(kib, 0, 1024), std::memory_order_relaxed);
    const std::lock_guard lock(g_entryMutex);
    for (auto& e : g_entries) e.self = nullptr;
}
void setAbsorbMode(int mode) { g_absorbMode.store(std::clamp(mode, 0, 2), std::memory_order_relaxed); }
void setStats(int level) {
    g_stats.store(level >= 1, std::memory_order_relaxed);
    g_verify.store(level >= 2, std::memory_order_relaxed);
}

void install() {
    if (g_installed.exchange(true)) return;
    g_engineBase = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(L"Engine.dll"));
    const auto* fs = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(L"FileSystem2.dll"));
    bool any = false;
    if (fs != nullptr) {
        // CFFPackedDirectFileStream::Read: mov [rsp+8],rbx; mov [rsp+10h],rbp; mov [rsp+18h],rsi; push rdi; sub rsp,70h
        static constexpr std::array<std::uint8_t, 20> kPrologue{0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10,
                                                                 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x70};
        std::array<std::uint8_t, 20> got{};
        if (safeRead(fs + 0x23540, got.data(), got.size()) && got == kPrologue) {
            any |= g_packed.install("CFFPackedDirectFileStream::Read", reinterpret_cast<PackedReadFn>(const_cast<std::uint8_t*>(fs + 0x23540)),
                                    &detourPackedRead);
        } else {
            spdlog::warn("engine: FileSystem2 packed read: prologue differs from the expected build: no read buffer");
        }
    } else {
        spdlog::warn("engine: FileSystem2.dll is not mapped: no read buffer");
    }
    if (const HMODULE fmod = GetModuleHandleW(L"fmod.dll")) {
        const auto fn = reinterpret_cast<CreateSoundFn>(GetProcAddress(
            fmod, "?createSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAUFMOD_CREATESOUNDEXINFO@@PEAPEAVSound@2@@Z"));
        if (fn != nullptr) any |= g_createSound.install("FMOD::System::createSound", fn, &detourCreateSound);
    }
    if (g_engineBase != nullptr) installAbsorb(g_engineBase);
    if (any && !hooks::enableAll()) spdlog::warn("engine: load accel: hooks not enabled");
    spdlog::info("engine: load accel: packed read buffer {}, FMOD createSound statistics {}, absorption image patch {}", g_packed.installed() ? "hooked" : "off",
                 g_createSound.installed() ? "hooked" : "off", g_engineBase != nullptr ? "checked" : "off");
}

void onPresent() {
    if (!g_installed.load(std::memory_order_relaxed)) return;
    const std::uint64_t now = GetTickCount64();
    if (g_logAt == 0) g_logAt = now;
    if (now - g_logAt < 5000) return;
    g_logAt = now;
    const std::uint64_t reads = g_c.reads.load(), absorbs = g_c.absorbs.load(), sounds = g_c.sounds.load();
    if (reads == g_last.reads.load() && absorbs == g_last.absorbs.load() && sounds == g_last.sounds.load()) return;
    const auto d = [](const std::atomic<std::uint64_t>& a, const std::atomic<std::uint64_t>& b) { return a.load() - b.load(); };
    const std::uint64_t dReads = d(g_c.reads, g_last.reads), dHits = d(g_c.hits, g_last.hits), dRefills = d(g_c.refills, g_last.refills);
    if (dReads != 0) {
        spdlog::info("engine: stream: packed reads {} ({} from the buffer, {} refills of {:.0f} KiB avg, {} passed through), {:.1f} KiB served, {:.1f} KiB fetched{}", dReads,
                     dHits, dRefills, dRefills != 0 ? d(g_c.bytesFetched, g_last.bytesFetched) / 1024.0 / dRefills : 0.0,
                     d(g_c.passThrough, g_last.passThrough), d(g_c.bytesServed, g_last.bytesServed) / 1024.0,
                     d(g_c.bytesFetched, g_last.bytesFetched) / 1024.0,
                     g_stats.load() ? std::format(", {:.1f} ms in the hook", d(g_c.readNs, g_last.readNs) * 1e-6) : std::string());
        if (g_verify.load()) spdlog::info("engine: stream: packed read buffer verified {} hits against the engine, {} differ", g_c.verified.load(), g_c.verifyDiffer.load());
    }
    const std::uint64_t dAbs = d(g_c.absorbs, g_last.absorbs);
    if (dAbs != 0) {
        spdlog::info("engine: stream: absorption images {} ({:.1f} Mpixel) {:.1f} ms ({})", dAbs, d(g_c.absorbPixels, g_last.absorbPixels) * 1e-6,
                     d(g_c.absorbNs, g_last.absorbNs) * 1e-6, g_absorbMode.load() == 0 ? "engine loop" : "sliding window");
    }
    const std::uint64_t dSounds = d(g_c.sounds, g_last.sounds);
    if (dSounds != 0) {
        spdlog::info("engine: stream: sounds created {} in {:.1f} ms (longest so far {:.1f} ms)", dSounds, d(g_c.soundNs, g_last.soundNs) * 1e-6,
                     g_c.soundMaxNs.load() * 1e-6);
    }
    g_last.reads = g_c.reads.load();
    g_last.hits = g_c.hits.load();
    g_last.refills = g_c.refills.load();
    g_last.passThrough = g_c.passThrough.load();
    g_last.bytesServed = g_c.bytesServed.load();
    g_last.bytesFetched = g_c.bytesFetched.load();
    g_last.readNs = g_c.readNs.load();
    g_last.sounds = g_c.sounds.load();
    g_last.soundNs = g_c.soundNs.load();
    g_last.absorbs = g_c.absorbs.load();
    g_last.absorbNs = g_c.absorbNs.load();
    g_last.absorbPixels = g_c.absorbPixels.load();
}

}  // namespace rvk::engine::loadaccel
