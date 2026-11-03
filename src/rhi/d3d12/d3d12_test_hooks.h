#pragma once

// Pure parts of the D3D12 backend for rvk_tests (tests/d3d12_tests.cpp): the format tables and the PSO raster key. No device is created; the
// functions live in d3d12_device.cpp (the tables are file-local there). Values are the raw DXGI_FORMAT numbers.

#include <cstdint>

#include "rhi/game_device.h"

namespace rvk::rhi::d3d12test {

std::uint32_t dxgiFormat(VkFormat f);                        // typed format (render targets, UAVs, copies, DSV format); 0 = unsupported
std::uint32_t dxgiVertexFormat(VkFormat f);                  // input layout format (the scaled integer ones are read as UNORM / SINT)
std::uint32_t dxgiResourceFormat(VkFormat f, bool sampled);  // resource format (depth that is also sampled: typeless)
std::uint32_t dxgiSrvFormat(VkFormat f);                     // shader resource view format

// the PSO key of a draw's raster / depth / stencil state: equal states give equal keys, different ones do not; bias rounds away from zero
bool rasterKeysEqual(const DynamicStateValues& a, const DynamicStateValues& b);
std::int32_t rasterKeyBias(const DynamicStateValues& d);
// sizeof the key and whether it has no padding (the key is compared / hashed as bytes)
std::uint32_t rasterKeySize();
std::uint32_t rasterKeyFieldBytes();

}  // namespace rvk::rhi::d3d12test
