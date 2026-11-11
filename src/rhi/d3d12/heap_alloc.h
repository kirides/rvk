#pragma once
// A CPU-writable range of descriptors of a heap with a free list (D3D12 backend), plus an optional reserved LOW range for resources whose shader-side
// index must stay small (the material pack word keeps the pack map's bindless slot in 14 bits = slots < 16384, enh/material_pack.h). The general
// alloc() skips the reserved range, allocLow() serves only it.
#include <cstdint>
#include <mutex>
#include <vector>

namespace rvk::rhi {

struct HeapAllocator {
    std::uint32_t next = 1;  // 0 is reserved (the default descriptor; the texture heap reserves kReservedSlots)
    std::uint32_t limit = 0;
    std::uint32_t lowBegin = 0, lowEnd = 0;  // [lowBegin, lowEnd) reserved for allocLow (empty = none)
    std::uint32_t lowNext = 0;
    std::vector<std::uint32_t> freeList, lowFree;
    std::mutex mutex;

    void reserveLow(std::uint32_t begin, std::uint32_t end) {
        lowBegin = begin;
        lowEnd = end;
        lowNext = begin;
    }
    std::uint32_t alloc() {
        const std::lock_guard lock(mutex);
        if (!freeList.empty()) {
            const std::uint32_t i = freeList.back();
            freeList.pop_back();
            return i;
        }
        if (next == lowBegin && lowEnd > lowBegin) next = lowEnd;  // hop over the reserved range
        return next < limit ? next++ : 0;
    }
    // 0 = the low range is full (or none)
    std::uint32_t allocLow() {
        const std::lock_guard lock(mutex);
        if (!lowFree.empty()) {
            const std::uint32_t i = lowFree.back();
            lowFree.pop_back();
            return i;
        }
        return lowNext < lowEnd ? lowNext++ : 0;
    }
    void release(std::uint32_t i) {
        if (i == 0) return;
        const std::lock_guard lock(mutex);
        (i >= lowBegin && i < lowEnd ? lowFree : freeList).push_back(i);
    }
};

}  // namespace rvk::rhi
