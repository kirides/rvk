#pragma once

// From the engine's HAL object pointers (eCGfxVertexData / eCGfxIndexData `+0x20` = the eCAPIVertexData / eCAPIIndexData the engine
// holds, which in Vulkan mode are our HalVertexData / HalIndexData) to the d3d9::Buffer behind them: the native scene (docs/research/
// native-renderer.md N1) reads a mesh resource's buffers without a draw. A pointer the engine hands us is only trusted when it is
// in the set of live HAL objects (the engine memory is not ours). Any thread.

#include <cstdint>

namespace rvk::d3d9 {
class Buffer;
}

namespace rvk::hal::vulkan {

struct VertexBufferRef {
    d3d9::Buffer* buffer = nullptr;
    std::uint32_t stride = 0;
};

// false: not a live vertex / index data object of ours
bool vertexBufferOf(const void* halObject, VertexBufferRef& out);
bool indexBufferOf(const void* halObject, d3d9::Buffer*& out);

// bookkeeping of HalVertexData / HalIndexData (constructor / destructor)
void noteVertexData(const void* halObject, d3d9::Buffer* buffer, std::uint32_t stride);
void forgetVertexData(const void* halObject);
void noteIndexData(const void* halObject, d3d9::Buffer* buffer);
void forgetIndexData(const void* halObject);

}  // namespace rvk::hal::vulkan
