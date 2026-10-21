#pragma once

// Runtime support of the generated tracing wrappers (hal/trace_generated.h): call statistics, argument logging, the
// wrapper <-> engine object map.

#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <type_traits>

#include "hal/types_base.h"

namespace rvk::hal::trace {

inline constexpr int kMaxInterfaces = 16;
inline constexpr int kMaxSlots = 256;

struct SlotStats {
    std::atomic<std::uint64_t> calls{0};
};
SlotStats& stats(int iface, int slot);
// how many calls of each slot are logged with their arguments (config [trace] log_first)
int logFirst();
void setLogFirst(int n);

template <class T>
std::string argString(const T& v) {
    if constexpr (std::is_same_v<T, bool>) {
        return v ? "true" : "false";
    } else if constexpr (std::is_floating_point_v<T>) {
        return std::format("{}", v);
    } else if constexpr (std::is_integral_v<T>) {
        return std::format("{}", static_cast<long long>(v));
    } else if constexpr (std::is_enum_v<T>) {
        return std::format("{}", static_cast<long long>(v));
    } else if constexpr (std::is_pointer_v<T> || std::is_member_pointer_v<T>) {
        return std::format("{}", reinterpret_cast<const void*>(v));
    } else {
        return std::format("&{}", static_cast<const void*>(&v));
    }
}

void logCall(int iface, int slot, std::uint64_t n, const std::string& args);
void logResult(int iface, int slot, const std::string& result);

class CallScope {
public:
    template <class... A>
    CallScope(int iface, int slot, const A&... a) : iface_(iface), slot_(slot) {
        const std::uint64_t n = ++stats(iface, slot).calls;
        log_ = n <= static_cast<std::uint64_t>(logFirst());
        if (log_) {
            std::string s;
            ((s += (s.empty() ? "" : ", ") + argString(a)), ...);
            logCall(iface, slot, n, s);
        }
    }
    template <class R>
    void result(const R& r) const {
        if (log_) {
            logResult(iface_, slot_, argString(r));
        }
    }

private:
    int iface_;
    int slot_;
    bool log_ = false;
};

// wrapper <-> engine object
void registerWrapper(void* wrapper, void* inner);
void unregisterWrapper(void* wrapper);
void* innerOf(void* maybeWrapper);  // the engine object behind a wrapper, or the argument itself
void* wrapperOf(void* inner);       // the existing wrapper of an engine object, or nullptr

template <class T>
T* unwrap(T* p) {
    return static_cast<T*>(innerOf(p));
}

// Releases the reference a wrapper holds on its engine object (ref-counted interfaces), or destroys it (eCAPIPrimitive).
// Never `delete` an engine object: its deleting destructor frees with the CRT heap, but HAL objects come from
// bCMemoryAdmin::NewObject. The engine itself destroys with flags 0 and frees with bCMemoryAdmin::DeleteObject.
template <class T>
void releaseInner(int /*iface*/, void* wrapper, T* inner) {
    unregisterWrapper(wrapper);
    if (inner == nullptr) {
        return;
    }
    if constexpr (requires { inner->refCount; }) {
        engineRelease(inner);
    } else {
        inner->~T();
        detail::engineFree(inner);
    }
}

}  // namespace rvk::hal::trace
