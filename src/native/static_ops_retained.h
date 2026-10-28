#pragma once

// V3 retained composition (docs/research/vulkanize/plan.md section 3 / V3 result): the read log of a composition. While a
// sub-mesh draw group (StaticOps::materialDraws / specialPassDraw) is composed for the first time, every read of engine memory
// it makes is logged (address, size, value or hash); the next frame the group's draws are taken from the retained entry when
// all those locations still hold the same values (the composition is a function of them, of its explicit key and of the
// per-frame values it re-reads at replay).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rvk::native::ret {

struct ReadRec {
    const std::uint8_t* addr = nullptr;
    std::uint64_t value = 0;  // kind 0: the 1 / 2 / 4 / 8 bytes; kind 1: hash of `size` bytes
    std::uint32_t size = 0;
    std::uint32_t kind = 0;
};

struct ReadLog {
    std::vector<ReadRec> v;
    bool failed = false;  // an unreadable location was met: the composition is not retained
};

// the log being written (null: not recording)
inline ReadLog* gLog = nullptr;

std::uint64_t hashBytes(const void* p, std::size_t n);

// one scoped pause of the logging (reads that are values re-read at every replay)
struct NoLog {
    ReadLog* saved;
    NoLog() : saved(gLog) { gLog = nullptr; }
    ~NoLog() { gLog = saved; }
    NoLog(const NoLog&) = delete;
    NoLog& operator=(const NoLog&) = delete;
};

inline void logWord(const void* p, std::size_t off, std::size_t size, std::uint64_t bits, bool ok) {
    ReadLog* l = gLog;
    if (!ok) {
        l->failed = true;
        return;
    }
    l->v.push_back({static_cast<const std::uint8_t*>(p) + off, bits, static_cast<std::uint32_t>(size), 0});
}

inline void logBlock(const void* p, std::size_t off, const void* data, std::size_t n, bool ok) {
    ReadLog* l = gLog;
    if (!ok) {
        l->failed = true;
        return;
    }
    l->v.push_back({static_cast<const std::uint8_t*>(p) + off, hashBytes(data, n), static_cast<std::uint32_t>(n), 1});
}

// sorts and de-duplicates a log (the same location read many times)
void finalize(std::vector<ReadRec>& v);
// true when every logged location still holds what was read (an unreadable one: false)
bool validate(const ReadRec* r, std::size_t n);

}  // namespace rvk::native::ret
