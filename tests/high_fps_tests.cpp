// High fps compatibility (src/engine/high_fps.*, docs/research/fps-limit.md 12): the per-frame scale of the fixes.

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "engine/high_fps.h"
#include "test.h"

using namespace rvk;

namespace {
int sumOfThree(int a, int b, int c, void* /*rsi*/) { return a + b + c; }
}  // namespace

TEST(high_fps_rsi_stub_executes) {
    namespace hf = engine::highfps;
    // the layout of the patch page: the stub, then (16-aligned) the call slot that points at it
    auto* page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    CHECK(page != nullptr);
    if (page == nullptr) return;
    std::memset(page, 0xCC, 4096);
    CHECK(hf::writeRsiStub(page, reinterpret_cast<const void*>(&sumOfThree)) == hf::kRsiStubSize);
    const std::size_t slotOffset = (hf::kRsiStubSize + 15) & ~std::size_t{15};
    void* stubAddr = page;
    std::memcpy(page + slotOffset, &stubAddr, sizeof(void*));
    // the jump target survives the slot write (the first version reserved 16 bytes and the slot clobbered its top byte)
    const void* target = nullptr;
    std::memcpy(&target, page + hf::kRsiStubSize - sizeof(void*), sizeof(void*));
    CHECK(target == reinterpret_cast<const void*>(&sumOfThree));
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    using Fn = int (*)(int, int, int, void*);
    void* viaSlot = nullptr;
    std::memcpy(&viaSlot, page + slotOffset, sizeof(void*));
    CHECK(reinterpret_cast<Fn>(viaSlot)(1, 2, 3, nullptr) == 6);
    VirtualFree(page, 0, MEM_RELEASE);
}

TEST(high_fps_frame_scale) {
    namespace hf = engine::highfps;
    CHECK(hf::frameScale(1.0f / 30.0f) == 1.0f);                     // the patch's reference rate
    CHECK(std::fabs(hf::frameScale(1.0f / 120.0f) - 4.0f) < 1e-4f);  // 4 frames per 30 fps frame
    CHECK(std::fabs(hf::frameScale(1.0f / 240.0f) - 8.0f) < 1e-4f);
    CHECK(hf::frameScale(1.0f / 20.0f) == 1.0f);                     // below 30 fps: the game's behaviour
    CHECK(hf::frameScale(0.0f) == 1.0f);                             // time freeze
    CHECK(hf::frameScale(-1.0f) == 1.0f);
    CHECK(hf::frameScale(1e-7f) == 100.0f);                          // clamped
}
