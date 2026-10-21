#include "hal/types_base.h"

#include <new>

#include "core/exports.h"

namespace rvk::hal {
namespace {

// class bCMemoryAdmin (SharedBase.dll)
using GetInstanceFn = void* (*)();                                                                   // static bCMemoryAdmin& GetInstance()
using NewObjectFn = void* (*)(void* self, unsigned long long size, const char*, const char*, const char*, int);
using DeleteObjectFn = void (*)(void* self, void* p);

GetInstanceFn g_getInstance = nullptr;
NewObjectFn g_newObject = nullptr;
DeleteObjectFn g_deleteObject = nullptr;

}  // namespace

bool initEngineAllocator() {
    g_getInstance = exports::resolveAs<GetInstanceFn>(L"SharedBase.dll", "?GetInstance@bCMemoryAdmin@@SAAEAV1@XZ");
    g_newObject = exports::resolveAs<NewObjectFn>(L"SharedBase.dll", "?NewObject@bCMemoryAdmin@@QEAAPEAX_KPEBD11H@Z");
    g_deleteObject = exports::resolveAs<DeleteObjectFn>(L"SharedBase.dll", "?DeleteObject@bCMemoryAdmin@@QEAAXPEAX@Z");
    return g_getInstance != nullptr && g_newObject != nullptr && g_deleteObject != nullptr;
}

void* EngineObject::operator new(std::size_t size) {
    // the engine's own HAL objects are allocated as NewObject(size, 0, 0, 0, -1) (eCDX9Device::CreateVertexData)
    void* p = g_newObject != nullptr ? g_newObject(g_getInstance(), size, nullptr, nullptr, nullptr, -1) : nullptr;
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void EngineObject::operator delete(void* p) noexcept { detail::engineFree(p); }

void detail::engineFree(void* p) noexcept {
    if (p != nullptr && g_deleteObject != nullptr) {
        g_deleteObject(g_getInstance(), p);
    }
}

}  // namespace rvk::hal
