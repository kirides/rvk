// WS35 (frame-ownership.md 16.26): the d3d9.dll module's global operator new / delete (every form) on core/private_heap: the renderer's allocations stay off the process heap
// the engine shares. The replacement binds this module only (the dynamic CRT's own operator new in vcruntime140 / msvcp140 stays for their internals; blocks they hand us are
// recognised by the missing header and freed with the CRT's free).
#include <cstdlib>
#include <new>

#include "core/private_heap.h"

namespace {

void* allocOrThrow(std::size_t size, std::size_t align) {
    for (;;) {
        if (void* p = rvk::pheap::alloc(size, align)) return p;
        const std::new_handler h = std::get_new_handler();
        if (!h) throw std::bad_alloc();
        h();
    }
}
void* allocNoThrow(std::size_t size, std::size_t align) noexcept {
    for (;;) {
        if (void* p = rvk::pheap::alloc(size, align)) return p;
        const std::new_handler h = std::get_new_handler();
        if (!h) return nullptr;
        try {
            h();
        } catch (...) {
            return nullptr;
        }
    }
}

}  // namespace

void* operator new(std::size_t n) { return allocOrThrow(n, 16); }
void* operator new[](std::size_t n) { return allocOrThrow(n, 16); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return allocNoThrow(n, 16); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return allocNoThrow(n, 16); }
void* operator new(std::size_t n, std::align_val_t a) { return allocOrThrow(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocOrThrow(n, static_cast<std::size_t>(a)); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return allocNoThrow(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return allocNoThrow(n, static_cast<std::size_t>(a)); }

void operator delete(void* p) noexcept { rvk::pheap::release(p, false); }
void operator delete[](void* p) noexcept { rvk::pheap::release(p, false); }
void operator delete(void* p, std::size_t) noexcept { rvk::pheap::release(p, false); }
void operator delete[](void* p, std::size_t) noexcept { rvk::pheap::release(p, false); }
void operator delete(void* p, const std::nothrow_t&) noexcept { rvk::pheap::release(p, false); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { rvk::pheap::release(p, false); }
void operator delete(void* p, std::align_val_t) noexcept { rvk::pheap::release(p, true); }
void operator delete[](void* p, std::align_val_t) noexcept { rvk::pheap::release(p, true); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { rvk::pheap::release(p, true); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { rvk::pheap::release(p, true); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { rvk::pheap::release(p, true); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { rvk::pheap::release(p, true); }
