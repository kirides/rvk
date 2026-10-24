#include "hal/vulkan/buffer_lookup.h"

#include <mutex>
#include <unordered_map>

namespace rvk::hal::vulkan {

namespace {

struct Registry {
    std::mutex mutex;
    std::unordered_map<const void*, VertexBufferRef> vertex;
    std::unordered_map<const void*, d3d9::Buffer*> index;
};

Registry& registry() {
    static Registry* r = new Registry();  // never destroyed: HAL objects die during process exit
    return *r;
}

}  // namespace

bool vertexBufferOf(const void* halObject, VertexBufferRef& out) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    const auto it = r.vertex.find(halObject);
    if (it == r.vertex.end()) return false;
    out = it->second;
    return true;
}

bool indexBufferOf(const void* halObject, d3d9::Buffer*& out) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    const auto it = r.index.find(halObject);
    if (it == r.index.end()) return false;
    out = it->second;
    return true;
}

void noteVertexData(const void* halObject, d3d9::Buffer* buffer, std::uint32_t stride) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    r.vertex[halObject] = {buffer, stride};
}

void forgetVertexData(const void* halObject) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    r.vertex.erase(halObject);
}

void noteIndexData(const void* halObject, d3d9::Buffer* buffer) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    r.index[halObject] = buffer;
}

void forgetIndexData(const void* halObject) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    r.index.erase(halObject);
}

}  // namespace rvk::hal::vulkan
