#pragma once

// Base pieces of the engine's graphics HAL that tools/gen_hal.py does not generate.

#include <cstddef>

namespace rvk::hal {

// The HAL's result type: 1 = success, 0 = failure (eCDX9* return `hr >= 0`, docs/research/hal.md).
enum bEResult : int {
    bEResult_False = 0,
    bEResult_Ok = 1,
};

// Base of every HAL object we hand to the engine. The engine frees ref-counted HAL objects itself (eCAPI*::Release calls the
// destructor slot with flags 0, then bCMemoryAdmin::DeleteObject), so they must come from the engine's allocator.
// Empty: the vtable stays at +0 and the reference count at +8 (EBO).
class EngineObject {
public:
    static void* operator new(std::size_t size);
    static void operator delete(void* p) noexcept;
};

// Resolves bCMemoryAdmin from SharedBase.dll (mapped as a dependency of Engine.dll). Safe under the loader lock.
bool initEngineAllocator();

namespace detail {
void engineFree(void* p) noexcept;
}

// What the engine's non-virtual eCAPI*::Release does (Engine.dll 0x5004a0): decrement, destroy and free at zero.
template <class T>
unsigned long engineRelease(T* p) {
    if (--p->refCount != 0) {
        return static_cast<unsigned long>(p->refCount);
    }
    p->~T();  // virtual: the destructor slot of the dynamic type, flags 0 (no free)
    detail::engineFree(p);
    return 0;
}

template <class T>
void engineAddRef(T* p) {
    ++p->refCount;
}

}  // namespace rvk::hal
