#include "core/private_heap.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <malloc.h>

namespace rvk::pheap {
namespace {

constexpr std::uint64_t kMagic = 0x52564B5048454150ull;  // "RVKPHEAP"

struct Hdr {
    std::uint64_t tag;    // kMagic ^ address of the user pointer
    std::uint32_t shard;  // heap index
    std::uint32_t back;   // user pointer - heap block
};
static_assert(sizeof(Hdr) == 16);

HANDLE g_heaps[kShards];
std::atomic<int> g_state{0};  // 0 = not created, 1 = being created, 2 = ready
bool g_private = true;
bool g_fill = false;  // RVK_HEAP_FILL=1 (debug): 0xCD into every new block, 0xDD over every freed one
std::atomic<std::uint32_t> g_threads{0};
std::atomic<std::uint64_t> g_foreign{0};
void* g_foreignSite[8];
thread_local std::uint32_t t_shard = 0xFFFFFFFFu;  // constant-initialised, no destructor: safe at any point of a thread's life

void init() noexcept {
    int expected = 0;
    if (g_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        char v[8] = {};
        const DWORD n = GetEnvironmentVariableA("RVK_PRIVATE_HEAP", v, sizeof v);
        g_private = !(n == 1 && v[0] == '0');
        char f[8] = {};
        g_fill = GetEnvironmentVariableA("RVK_HEAP_FILL", f, sizeof f) == 1 && f[0] == '1';
        for (auto& h : g_heaps) h = nullptr;
        if (g_private) {
            for (auto& h : g_heaps) {
                h = HeapCreate(0, 64 * 1024, 0);
                if (!h) {
                    g_private = false;
                    break;
                }
            }
        }
        if (!g_private) {
            for (auto& h : g_heaps) h = GetProcessHeap();
        }
        g_state.store(2, std::memory_order_release);
    } else {
        while (g_state.load(std::memory_order_acquire) != 2) Sleep(0);
    }
}

inline std::uint32_t shard() noexcept {
    if (t_shard == 0xFFFFFFFFu) {
        const std::uint32_t n = g_threads.fetch_add(1, std::memory_order_relaxed);
        t_shard = n == 0 ? 0 : 1 + (n - 1) % (kShards - 1);
    }
    return t_shard;
}

bool readTag(const void* at, std::uint64_t& out) noexcept {
    // p - 16 is on p's page unless p starts a page: only then can the read fault (a foreign page-aligned block)
    if ((reinterpret_cast<std::uintptr_t>(at) & 0xFFF) < 0xFF0) {
        out = *static_cast<const std::uint64_t*>(at);
        return true;
    }
    __try {
        out = *static_cast<const volatile std::uint64_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const Hdr* headerOf(const void* p) noexcept {
    const auto a = reinterpret_cast<std::uintptr_t>(p);
    if (a < 4096 || (a & 15) != 0) return nullptr;
    const auto* h = reinterpret_cast<const Hdr*>(a - sizeof(Hdr));
    std::uint64_t tag = 0;
    if (!readTag(h, tag) || tag != (kMagic ^ a)) return nullptr;
    return h;
}

}  // namespace

namespace {

// Blocks freed by a thread of another shard are not HeapFree'd there (that would take the owner's heap lock: the engine thread's shard must not wait for a worker's free of a large
// buffer): they are pushed on the owner shard's lock-free stack (the link lives in the freed block's first bytes) and the owner's next alloc / release frees them.
std::atomic<void*> g_remote[kShards];

void freeLocal(void* p) noexcept {
    const Hdr* h = headerOf(p);
    if (!h) return;  // (cannot happen: the tag was valid when the block was queued)
    const Hdr hd = *h;
    const_cast<Hdr*>(h)->tag = 0;  // a double free is then a foreign free instead of a heap corruption
    if (g_fill) {
        const SIZE_T total = HeapSize(g_heaps[hd.shard < kShards ? hd.shard : 0], 0, static_cast<std::uint8_t*>(p) - hd.back);
        if (total != static_cast<SIZE_T>(-1) && total > hd.back) std::memset(p, 0xDD, total - hd.back);
    }
    HeapFree(g_heaps[hd.shard < kShards ? hd.shard : 0], 0, static_cast<std::uint8_t*>(p) - hd.back);
}

void drainRemote(std::uint32_t s) noexcept {
    if (g_remote[s].load(std::memory_order_relaxed) == nullptr) return;
    void* n = g_remote[s].exchange(nullptr, std::memory_order_acquire);
    while (n != nullptr) {
        void* next = *static_cast<void**>(n);
        freeLocal(n);
        n = next;
    }
}

}  // namespace

void* alloc(std::size_t size, std::size_t align) noexcept {
    if (g_state.load(std::memory_order_acquire) != 2) init();
    if (align < 16) align = 16;
    if (size < sizeof(void*)) size = sizeof(void*);  // (room for the remote-free link)
    const std::size_t total = size + sizeof(Hdr) + (align > 16 ? align : 0);
    if (total < size) return nullptr;
    const std::uint32_t s = shard();
    if (g_private) drainRemote(s);
    auto* base = static_cast<std::uint8_t*>(HeapAlloc(g_heaps[s], 0, total));
    if (!base) return nullptr;
    auto a = reinterpret_cast<std::uintptr_t>(base) + sizeof(Hdr);
    a = (a + (align - 1)) & ~(static_cast<std::uintptr_t>(align) - 1);
    auto* h = reinterpret_cast<Hdr*>(a - sizeof(Hdr));
    h->tag = kMagic ^ a;
    h->shard = s;
    h->back = static_cast<std::uint32_t>(a - reinterpret_cast<std::uintptr_t>(base));
    if (g_fill) std::memset(reinterpret_cast<void*>(a), 0xCD, size);
    return reinterpret_cast<void*>(a);
}

void release(void* p, bool alignedForm) noexcept {
    if (!p) return;
    if (const Hdr* h = headerOf(p)) {
        const std::uint32_t owner = h->shard < kShards ? h->shard : 0;
        if (g_private) {
            const std::uint32_t mine = shard();
            if (owner != mine) {
                // another shard's block: queue it for its owner (no wait on that heap's lock)
                void* head = g_remote[owner].load(std::memory_order_relaxed);
                do {
                    *static_cast<void**>(p) = head;
                } while (!g_remote[owner].compare_exchange_weak(head, p, std::memory_order_release, std::memory_order_relaxed));
                return;
            }
            drainRemote(mine);
        }
        freeLocal(p);
        return;
    }
    // a foreign block (the CRT's own operator new in msvcp140 / vcruntime140 handed it to us): counted, the first call sites kept (diagnostics; the reverse, a block of ours
    // reaching the CRT's free, cannot be seen from here: it would be a heap fault)
    {
        const std::uint64_t i = g_foreign.fetch_add(1, std::memory_order_relaxed);
        if (i < 8) g_foreignSite[i] = _ReturnAddress();
    }
    if (alignedForm) _aligned_free(p);
    else std::free(p);
}

std::uint64_t foreignFrees() noexcept { return g_foreign.load(std::memory_order_relaxed); }
const void* foreignSite(std::uint32_t i) noexcept { return i < 8 ? g_foreignSite[i] : nullptr; }

bool owns(const void* p) noexcept { return headerOf(p) != nullptr; }
bool privateActive() noexcept {
    if (g_state.load(std::memory_order_acquire) != 2) init();
    return g_private;
}
std::size_t usableSize(const void* p) noexcept {
    const Hdr* h = headerOf(p);
    if (!h) return 0;
    const SIZE_T n = HeapSize(g_heaps[h->shard < kShards ? h->shard : 0], 0, static_cast<const std::uint8_t*>(p) - h->back);
    return n == static_cast<SIZE_T>(-1) ? 0 : n - h->back;
}

}  // namespace rvk::pheap
