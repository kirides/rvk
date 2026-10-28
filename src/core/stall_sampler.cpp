#include "core/stall_sampler.h"

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#pragma comment(lib, "dbghelp.lib")

namespace rvk::diag {
namespace {

constexpr int kMaxFrames = 48;

std::unique_ptr<StallSampler> g_present;
std::unique_ptr<StallSampler> g_frame;

std::uint64_t qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return static_cast<std::uint64_t>(v.QuadPart);
}

double qpcFreq() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
}

struct Range {
    std::uint64_t lo = 0, hi = 0;
    bool contains(std::uint64_t pc) const { return pc >= lo && pc < hi; }
};

Range moduleRange(HMODULE m) {
    MODULEINFO mi{};
    if (m == nullptr || !GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi))) return {};
    const auto lo = reinterpret_cast<std::uint64_t>(mi.lpBaseOfDll);
    return {lo, lo + mi.SizeOfImage};
}

// Walks a suspended thread's stack (no allocation, no module lookups: only the unwinder). SEH-guarded: a frame without
// unwind data that is not a leaf gives a bogus return address.
int unwindStack(CONTEXT& ctx, std::uint64_t* pcs, int max) {
    int n = 0;
    __try {
        while (n < max && ctx.Rip != 0) {
            pcs[n++] = ctx.Rip;
            const DWORD64 rsp = ctx.Rsp;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (fe == nullptr) {  // leaf function: the return address is on top of the stack
                ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
            } else {
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fe, &ctx, &handlerData, &establisher, nullptr);
            }
            if (ctx.Rsp <= rsp) break;  // no progress: garbage
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

int sampleThread(HANDLE thread, std::uint64_t* pcs, int max) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &ctx)) return 0;
    return unwindStack(ctx, pcs, max);
}

// module name + nearest named export, cached per module (after the thread was resumed); our own module by its PDB
class Symbolizer {
public:
    ~Symbolizer() {
        if (dbgProcess_ != nullptr) {
            SymCleanup(dbgProcess_);
            CloseHandle(dbgProcess_);
        }
    }
    std::string name(std::uint64_t pc) { return lookup(pc, true); }
    // the function without the offset (the key of a flat profile); unnamed code is bucketed per 4 KiB
    std::string function(std::uint64_t pc) { return lookup(pc, false); }
    // the RVA of `pc` inside our own module (the d3d9.dll proxy), 0 elsewhere: raw material for llvm-symbolizer --inlines
    std::uint32_t ownRva(std::uint64_t pc) {
        HMODULE m = nullptr;
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(pc), &m) ||
            !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&presentSampler), &self) ||
            m != self) {
            return 0;
        }
        return static_cast<std::uint32_t>(pc - reinterpret_cast<std::uint64_t>(m));
    }
    std::string moduleName(std::uint64_t pc) {
        HMODULE m = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(pc), &m)) {
            return "?";
        }
        return module(m).name;
    }

private:
    std::string lookup(std::uint64_t pc, bool offset) {
        if (auto hit = cacheByPc_.find(pc ^ (offset ? 1ull << 63 : 0)); hit != cacheByPc_.end()) return hit->second;
        std::string s = resolve(pc, offset);
        cacheByPc_.emplace(pc ^ (offset ? 1ull << 63 : 0), s);
        return s;
    }
    std::string resolve(std::uint64_t pc, bool offset) {
        HMODULE m = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(pc), &m)) {
            return offset ? std::format("0x{:x}", pc) : std::string("?");
        }
        Module& mod = module(m);
        auto rva = static_cast<std::uint32_t>(pc - reinterpret_cast<std::uint64_t>(m));
        // the function's start from the unwind table: exact function granularity also for code without a name
        DWORD64 imageBase = 0;
        const PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
        const std::uint32_t start = fe != nullptr && imageBase == reinterpret_cast<DWORD64>(m) ? fe->BeginAddress : rva;
        if (mod.pdb) {
            alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512]{};
            auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 511;
            DWORD64 disp = 0;
            if (SymFromAddr(dbgProcess_, pc, &disp, si)) {
                return offset ? std::format("{}!{}+0x{:x}", mod.name, si->Name, disp) : std::format("{}!{}", mod.name, si->Name);
            }
            if (!symFailLogged_) {
                symFailLogged_ = true;
                spdlog::info("diag: SymFromAddr({}+0x{:x}) failed: error {}", mod.name, rva, GetLastError());
            }
        }
        if (!offset) {
            // name the function by its start (an export there), else the start itself (symbolize offline: llvm-symbolizer
            // --obj=<module> 0x180000000+start)
            auto ex = std::ranges::lower_bound(mod.exports, start, {}, &std::pair<std::uint32_t, std::string>::first);
            if (ex != mod.exports.end() && ex->first == start) return std::format("{}!{}", mod.name, ex->second);
            if (fe != nullptr) {
                // not exported: the nearest export below as a hint
                if (ex != mod.exports.begin() && start - std::prev(ex)->first < 0x4000) {
                    return std::format("{}@0x{:x} (~{})", mod.name, start, std::prev(ex)->second);
                }
                return std::format("{}@0x{:x}", mod.name, start);
            }
            rva = start;
        }
        auto it = std::upper_bound(mod.exports.begin(), mod.exports.end(), rva,
                                   [](std::uint32_t v, const auto& e) { return v < e.first; });
        if (it != mod.exports.begin()) {
            --it;
            if (rva - it->first < 0x4000) {
                return offset ? std::format("{}!{}+0x{:x}", mod.name, it->second, rva - it->first)
                              : std::format("{}!{}", mod.name, it->second);
            }
        }
        return offset ? std::format("{}+0x{:x}", mod.name, rva) : std::format("{}+0x{:x}", mod.name, rva & ~0xFFFu);
    }

    struct Module {
        std::string name;
        std::vector<std::pair<std::uint32_t, std::string>> exports;  // sorted by RVA
        bool pdb = false;  // symbols through dbghelp (our module)
    };
    Module& module(HMODULE m) {
        auto [it, fresh] = cache_.try_emplace(m);
        if (fresh) load(m, it->second);
        return it->second;
    }
    // our own module's PDB (next to the DLL) through dbghelp, on a private process handle (dbghelp keys its state by the
    // handle: another user in the process keeps its own)
    void loadPdb(HMODULE m, const wchar_t* path, Module& out) {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&presentSampler), &self);
        if (m != self) return;
        if (dbgProcess_ == nullptr) {
            HANDLE h = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                return;
            }
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
            std::wstring dir(path);
            dir.resize(dir.find_last_of(L'\\') == std::wstring::npos ? 0 : dir.find_last_of(L'\\'));
            if (!SymInitializeW(h, dir.c_str(), FALSE)) {
                CloseHandle(h);
                return;
            }
            dbgProcess_ = h;
        }
        out.pdb = SymLoadModuleExW(dbgProcess_, nullptr, path, nullptr, reinterpret_cast<DWORD64>(m), 0, nullptr, 0) != 0 ||
                  GetLastError() == ERROR_SUCCESS;
        spdlog::info("diag: symbols of {}: {}", out.name, out.pdb ? "PDB" : "exports only");
    }
    void load(HMODULE m, Module& out) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* slash = wcsrchr(path, L'\\');
        for (const wchar_t* p = slash != nullptr ? slash + 1 : path; *p != 0; ++p) out.name += static_cast<char>(towlower(*p));
        if (out.name.ends_with(".dll")) out.name.resize(out.name.size() - 4);
        if (out.name.empty()) out.name = "?";
        loadPdb(m, path, out);
        const auto* base = reinterpret_cast<const std::uint8_t*>(m);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (dir.VirtualAddress == 0 || dir.Size == 0) return;
        const auto* ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
        const auto* funcs = reinterpret_cast<const DWORD*>(base + ex->AddressOfFunctions);
        const auto* names = reinterpret_cast<const DWORD*>(base + ex->AddressOfNames);
        const auto* ords = reinterpret_cast<const WORD*>(base + ex->AddressOfNameOrdinals);
        for (DWORD i = 0; i < ex->NumberOfNames; ++i) {
            if (ords[i] >= ex->NumberOfFunctions) continue;
            const DWORD rva = funcs[ords[i]];
            if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) continue;  // forwarder
            out.exports.emplace_back(rva, reinterpret_cast<const char*>(base + names[i]));
        }
        std::ranges::sort(out.exports);
    }
    std::unordered_map<HMODULE, Module> cache_;
    std::unordered_map<std::uint64_t, std::string> cacheByPc_;
    HANDLE dbgProcess_ = nullptr;
    bool symFailLogged_ = false;
};

bool isSystemModule(std::string_view name) {
    return name == "ntdll" || name == "kernelbase" || name == "kernel32" || name == "win32u" || name == "user32" ||
           name == "gdi32" || name == "gdi32full" || name == "ucrtbase" || name == "msvcp140" || name == "vcruntime140";
}

}  // namespace

StallSampler* presentSampler() { return g_present.get(); }

void enablePresentSampler(const char* what, double thresholdMs) {
    if (g_present == nullptr) g_present = std::make_unique<StallSampler>(what, thresholdMs);
}

StallSampler* frameSampler() { return g_frame.get(); }

void enableFrameSampler() {
    if (g_frame == nullptr) g_frame = std::make_unique<StallSampler>("engine thread between presents", 0.0, true);
}

struct StallSampler::Impl {
    std::string what;
    bool profile = false;
    double freq = qpcFreq();
    std::uint64_t thresholdTicks = 0;
    Range stopRanges[2];  // our module and Engine.dll: stacks are cut there

    std::atomic<HANDLE> thread{nullptr};
    std::atomic<DWORD> threadId{0};
    std::atomic<std::uint64_t> callStart{0};  // QPC at enter(), 0 = not inside the call
    std::atomic<std::uint32_t> callSeq{0};

    std::mutex mutex;  // never held while the watched thread is suspended
    std::map<std::vector<std::uint64_t>, std::uint32_t> stacks;
    std::uint32_t samples = 0;
    std::uint32_t calls = 0, longCalls = 0;
    double callMs = 0.0, longMs = 0.0, maxMs = 0.0;
    std::uint64_t windowStart = qpc();

    std::atomic<bool> quit{false};
    std::thread sampler;
    Symbolizer sym;  // sampler thread only

    void run();
    void report(double seconds);
    void reportProfile(const std::map<std::vector<std::uint64_t>, std::uint32_t>& taken, std::uint32_t n);
};

// flat profile: per module (leaf), self per function (leaf), inclusive per function (once per stack)
void StallSampler::Impl::reportProfile(const std::map<std::vector<std::uint64_t>, std::uint32_t>& taken, std::uint32_t n) {
    std::unordered_map<std::string, std::uint32_t> modules, self, incl;
    for (const auto& [stack, count] : taken) {
        if (stack.empty()) continue;
        modules[sym.moduleName(stack[0])] += count;
        self[sym.function(stack[0])] += count;
        std::vector<std::string> seen;
        for (const std::uint64_t pc : stack) {
            std::string f = sym.function(pc);
            if (std::ranges::find(seen, f) != seen.end()) continue;
            incl[f] += count;
            seen.push_back(std::move(f));
        }
    }
    const auto top = [&](const std::unordered_map<std::string, std::uint32_t>& m, std::size_t k, const char* label) {
        std::vector<std::pair<std::uint32_t, const std::string*>> v;
        for (const auto& [name, c] : m) v.emplace_back(c, &name);
        std::ranges::sort(v, [](const auto& a, const auto& b) { return a.first > b.first; });
        for (std::size_t i = 0; i < std::min(v.size(), k); ++i) {
            spdlog::info("diag: profile {} {:5.1f} % {}", label, 100.0 * v[i].first / n, *v[i].second);
        }
    };
    top(modules, 12, "module");
    top(self, 160, "self");
    top(incl, 120, "incl");
    // our module's leaf addresses (16-byte buckets): `llvm-symbolizer --obj=d3d9.dll --inlines 0x180000000+<rva>` gives lines
    std::unordered_map<std::uint32_t, std::uint32_t> pcs;
    for (const auto& [stack, count] : taken) {
        if (stack.empty()) continue;
        if (const std::uint32_t rva = sym.ownRva(stack[0]); rva != 0) pcs[rva & ~0xFu] += count;
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> v(pcs.begin(), pcs.end());
    std::ranges::sort(v, [](const auto& a, const auto& b) { return a.second > b.second; });
    for (std::size_t i = 0; i < std::min<std::size_t>(v.size(), 800); ++i) {
        spdlog::info("diag: profile pc {:5.2f} % 0x{:x}", 100.0 * v[i].second / n, v[i].first);
    }
    // call sites: the first frame inside our module (the leaf pc itself, else the return address - 1) with everything below
    // it (inclusive per source line; the leaf's module tells who we called: the CRT's memcpy / memcmp, ntdll, the driver)
    std::map<std::pair<std::uint32_t, std::string>, std::uint32_t> sites;
    // the CRT's callers: leaf in vcruntime140 / ucrtbase, the caller = the first frame outside them
    std::map<std::pair<std::string, std::string>, std::uint32_t> crt;
    // the caller of that function (the next frame of ours in another function): who reaches operator new / steady_clock::now
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::string>, std::uint32_t> sites2;
    const auto funcStart = [](std::uint64_t pc) -> std::uint32_t {
        DWORD64 base = 0;
        const PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(pc, &base, nullptr);
        return fe != nullptr ? fe->BeginAddress : 0;
    };
    for (const auto& [stack, count] : taken) {
        if (stack.empty()) continue;
        const std::string leafModule = sym.moduleName(stack[0]);
        for (std::size_t i = 0; i < stack.size(); ++i) {
            if (const std::uint32_t rva = sym.ownRva(stack[i]); rva != 0) {
                sites[{i == 0 ? rva : rva - 1, i == 0 ? std::string("self") : leafModule}] += count;
                const std::uint32_t f1 = funcStart(stack[i]);
                for (std::size_t j = i + 1; j < stack.size(); ++j) {
                    if (const std::uint32_t r2 = sym.ownRva(stack[j]); r2 != 0 && funcStart(stack[j]) != f1) {
                        sites2[{r2 - 1, f1, leafModule}] += count;
                        break;
                    }
                }
                break;
            }
        }
        if (leafModule == "vcruntime140" || leafModule == "ucrtbase") {
            for (std::size_t i = 1; i < stack.size(); ++i) {
                const std::string m = sym.moduleName(stack[i]);
                if (m == "vcruntime140" || m == "ucrtbase") continue;
                crt[{sym.function(stack[0]), sym.function(stack[i])}] += count;
                if (const std::uint32_t rva = sym.ownRva(stack[i]); rva != 0) {
                    crt[{sym.function(stack[0]), std::format("site 0x{:x}", rva - 1)}] += count;
                }
                break;
            }
        }
    }
    std::vector<std::pair<std::uint32_t, const std::pair<std::uint32_t, std::string>*>> sv;
    for (const auto& [k, c] : sites) sv.emplace_back(c, &k);
    std::ranges::sort(sv, [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = 0; i < std::min<std::size_t>(sv.size(), 250); ++i) {
        spdlog::info("diag: profile site {:5.2f} % 0x{:x} {}", 100.0 * sv[i].first / n, sv[i].second->first, sv[i].second->second);
    }
    std::vector<std::pair<std::uint32_t, const std::tuple<std::uint32_t, std::uint32_t, std::string>*>> s2v;
    for (const auto& [k, c] : sites2) s2v.emplace_back(c, &k);
    std::ranges::sort(s2v, [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = 0; i < std::min<std::size_t>(s2v.size(), 250); ++i) {
        const auto& [r2, f1, lm] = *s2v[i].second;
        spdlog::info("diag: profile site2 {:5.2f} % 0x{:x} 0x{:x} {}", 100.0 * s2v[i].first / n, r2, f1, lm);
    }
    std::vector<std::pair<std::uint32_t, const std::pair<std::string, std::string>*>> cv;
    for (const auto& [k, c] : crt) cv.emplace_back(c, &k);
    std::ranges::sort(cv, [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = 0; i < std::min<std::size_t>(cv.size(), 120); ++i) {
        spdlog::info("diag: profile crt {:5.2f} % {} <- {}", 100.0 * cv[i].first / n, cv[i].second->first, cv[i].second->second);
    }
}

StallSampler::StallSampler(std::string what, double thresholdMs, bool profile) : impl_(new Impl) {
    impl_->what = std::move(what);
    impl_->profile = profile;
    impl_->thresholdTicks = static_cast<std::uint64_t>(thresholdMs * impl_->freq / 1000.0);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&presentSampler), &self);
    impl_->stopRanges[0] = moduleRange(self);
    impl_->stopRanges[1] = moduleRange(GetModuleHandleW(L"Engine.dll"));
    impl_->sampler = std::thread([i = impl_] { i->run(); });
    spdlog::info("diag: {} stall sampler on (samples after {:.1f} ms inside the call, report every 5 s)", impl_->what, thresholdMs);
}

StallSampler::~StallSampler() {
    impl_->quit = true;
    if (impl_->sampler.joinable()) impl_->sampler.join();
    delete impl_;
}

void StallSampler::enter() {
    const DWORD tid = GetCurrentThreadId();
    if (impl_->threadId.load(std::memory_order_relaxed) != tid) {
        // a new watched thread (rare: the present thread is recreated with the swapchain's device); old handles leak
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (h == nullptr) return;
        impl_->thread.store(h);
        impl_->threadId.store(tid);
    }
    impl_->callSeq.fetch_add(1);
    impl_->callStart.store(qpc());
}

void StallSampler::leave() {
    const std::uint64_t start = impl_->callStart.exchange(0);
    if (start == 0) return;
    const double ms = static_cast<double>(qpc() - start) * 1000.0 / impl_->freq;
    const std::lock_guard lock(impl_->mutex);
    ++impl_->calls;
    impl_->callMs += ms;
    if (static_cast<std::uint64_t>(ms * impl_->freq / 1000.0) >= impl_->thresholdTicks) {
        ++impl_->longCalls;
        impl_->longMs += ms;
    }
    impl_->maxMs = std::max(impl_->maxMs, ms);
}

void StallSampler::Impl::run() {
    SetThreadDescription(GetCurrentThread(), L"rvk stall sampler");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    std::uint64_t pcs[kMaxFrames];
    while (!quit.load()) {
        if (timer != nullptr) {
            LARGE_INTEGER due{};
            due.QuadPart = -5000;  // 0.5 ms
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 100);
        } else {
            Sleep(1);
        }
        const std::uint64_t now = qpc();
        if (static_cast<double>(now - windowStart) >= 5.0 * freq) {
            report(static_cast<double>(now - windowStart) / freq);
            windowStart = now;
        }
        const std::uint64_t start = callStart.load();
        if (start == 0 || now - start < thresholdTicks) continue;
        const std::uint32_t seq = callSeq.load();
        HANDLE h = thread.load();
        if (h == nullptr || SuspendThread(h) == static_cast<DWORD>(-1)) continue;
        const int n = sampleThread(h, pcs, kMaxFrames);
        const bool same = callSeq.load() == seq && callStart.load() == start;
        ResumeThread(h);
        if (!same || n == 0) continue;
        int keep = n;
        for (int i = 0; i < n && !profile; ++i) {
            if (stopRanges[0].contains(pcs[i]) || stopRanges[1].contains(pcs[i])) {
                keep = i + 1;
                break;
            }
        }
        const std::lock_guard lock(mutex);
        ++stacks[std::vector<std::uint64_t>(pcs, pcs + keep)];
        ++samples;
    }
    if (timer != nullptr) CloseHandle(timer);
}

void StallSampler::Impl::report(double seconds) {
    std::map<std::vector<std::uint64_t>, std::uint32_t> taken;
    std::uint32_t n = 0, nCalls = 0, nLong = 0;
    double sumMs = 0.0, sumLong = 0.0, mx = 0.0;
    {
        const std::lock_guard lock(mutex);
        taken.swap(stacks);
        n = samples;
        nCalls = calls;
        nLong = longCalls;
        sumMs = callMs;
        sumLong = longMs;
        mx = maxMs;
        samples = calls = longCalls = 0;
        callMs = longMs = maxMs = 0.0;
    }
    if (nCalls == 0) return;
    // the state the cadence may key on: focus, cursor shown / clipped
    std::string state;
    {
        DWORD fgPid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
        CURSORINFO ci{sizeof(ci)};
        GetCursorInfo(&ci);
        RECT clip{}, screen{0, 0, GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)};
        GetClipCursor(&clip);
        const bool clipped = (clip.right - clip.left) < (screen.right - screen.left) || (clip.bottom - clip.top) < (screen.bottom - screen.top);
        state = std::format("foreground {}, cursor {}, cursor {}", fgPid == GetCurrentProcessId() ? "ours" : "other",
                            (ci.flags & CURSOR_SHOWING) != 0 ? "shown" : "hidden", clipped ? "clipped" : "free");
    }
    spdlog::info("diag: {}: {:.1f} calls/s, avg {:.2f} ms, max {:.2f} ms; {} calls >= threshold ({:.2f} ms avg); {} samples; {}", what,
                 nCalls / seconds, sumMs / nCalls, mx, nLong, nLong > 0 ? sumLong / nLong : 0.0, n, state);
    if (n == 0) return;
    if (profile) {
        reportProfile(taken, n);
        return;
    }
    // who owns the wait: the first frame (from the leaf) outside the OS's user-mode DLLs
    std::map<std::string, std::uint32_t> owners;
    for (const auto& [stack, count] : taken) {
        std::string owner = "system only";
        for (const std::uint64_t pc : stack) {
            std::string m = sym.moduleName(pc);
            if (!isSystemModule(m)) {
                owner = std::move(m);
                break;
            }
        }
        owners[owner] += count;
    }
    std::vector<std::pair<std::uint32_t, std::string>> byOwner;
    for (auto& [o, c] : owners) byOwner.emplace_back(c, o);
    std::ranges::sort(byOwner, std::greater{});
    std::string ownerText;
    for (const auto& [c, o] : byOwner) ownerText += std::format("{}{} {:.0f} %", ownerText.empty() ? "" : ", ", o, 100.0 * c / n);
    spdlog::info("diag: {}: time by first non-system module: {}", what, ownerText);
    std::vector<std::pair<std::uint32_t, const std::vector<std::uint64_t>*>> top;
    for (const auto& [stack, count] : taken) top.emplace_back(count, &stack);
    std::ranges::sort(top, [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = 0; i < std::min<std::size_t>(top.size(), 4); ++i) {
        std::string text;
        for (const std::uint64_t pc : *top[i].second) text += (text.empty() ? "" : " < ") + sym.name(pc);
        spdlog::info("diag: {}: stack {} ({:.0f} %): {}", what, i + 1, 100.0 * top[i].first / n, text);
    }
}

}  // namespace rvk::diag
