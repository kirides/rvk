#include "d3d9/identity.h"

#include <algorithm>
#include <atomic>

#include "d3d9/defs.h"

namespace rvk::d3d9 {

std::uint64_t newResourceId() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

void BufferCopyState::lock(std::uint32_t offset, std::uint32_t size, std::uint32_t flags, std::uint32_t bufferSize) {
    if ((flags & LOCK_READONLY) != 0) {
        return;
    }
    // the written range (size 0 = up to the end)
    const std::uint32_t lo = std::min(offset, bufferSize);
    const std::uint32_t hi =
        size == 0 ? bufferSize : static_cast<std::uint32_t>(std::min<std::uint64_t>(std::uint64_t{offset} + size, bufferSize));
    if ((flags & LOCK_DISCARD) != 0) {
        // the old contents are undefined from here on: a fresh GPU copy needs only what is written after the DISCARD
        gpuFrame_ = 0;  // draws recorded before keep the old GPU copy
        discarded_ = true;
        dirtyLo_ = lo;
        dirtyHi_ = hi;
    } else {
        if ((flags & LOCK_NOOVERWRITE) == 0) {
            gpuFrame_ = 0;  // a plain lock: new GPU copy (of the whole buffer), draws recorded before keep the old one
            discarded_ = false;
        }
        dirtyLo_ = dirty_ ? std::min(dirtyLo_, lo) : lo;
        dirtyHi_ = dirty_ ? std::max(dirtyHi_, hi) : hi;
    }
    dirty_ = true;
}

BufferCopyState::Plan BufferCopyState::plan(bool ring, std::uint64_t frame, std::uint32_t bufferSize, bool hasStatic) const {
    Plan p;
    if (ring) {
        // ring copy per frame; NOOVERWRITE appends go into the same copy (the CPU writes land before the GPU runs). Only the
        // bytes written since the last copy are copied (the sprite batcher locks one quad of a 448 KiB buffer per Lock).
        if (gpuFrame_ != frame) {
            // after a DISCARD only the range written since is defined; otherwise the copy needs everything (a new frame)
            p.op = Plan::Op::NewCopy;
            p.lo = discarded_ ? dirtyLo_ : 0;
            p.hi = discarded_ ? dirtyHi_ : bufferSize;
        } else if (dirty_ && dirtyHi_ > dirtyLo_) {
            p.op = Plan::Op::Patch;
            p.lo = dirtyLo_;
            p.hi = dirtyHi_;
        }
        return p;
    }
    // static buffer: a fresh GPU buffer per update, so draws recorded earlier keep the old contents
    if (dirty_ || !hasStatic) {
        p.op = Plan::Op::NewCopy;
        p.lo = 0;
        p.hi = bufferSize;
    }
    return p;
}

void BufferCopyState::committed(const Plan& plan, bool ring, std::uint64_t frame) {
    if (plan.op == Plan::Op::NewCopy) {
        ++generation_;
    }
    if (ring) {
        if (plan.op == Plan::Op::NewCopy) {
            gpuFrame_ = frame;
        }
        dirty_ = false;
        discarded_ = false;
        dirtyLo_ = dirtyHi_ = 0;
    } else if (plan.op == Plan::Op::NewCopy) {
        dirty_ = false;
    }
}

}  // namespace rvk::d3d9
