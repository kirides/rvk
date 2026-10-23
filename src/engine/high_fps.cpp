#include "engine/high_fps.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <spdlog/spdlog.h>

#include "core/settings.h"
#include "engine/symbols.h"

namespace rvk::engine::highfps {
namespace {

// A redirected call: `call qword ptr [rip + disp32]` (FF 15) at `rva`, whose slot is the import `slotRva` (verified)
struct Site {
    const wchar_t* module;
    std::uint32_t rva;
    std::uint32_t slotRva;
    const char* name;
};
constexpr Site kFallAbs{L"Game.dll", 0x8e5c7f, 0xabdab8, "fall check (g_fabsf in CheckChangedMovementStates)"};
constexpr Site kClimbHigh{L"Game.dll", 0x8e8c31, 0xabd858, "climb (GetInvScaled in ProcessClimbHigh)"};
constexpr Site kClimbStandard{L"Game.dll", 0x8e90da, 0xabd858, "climb (GetInvScaled in ProcessClimbStandard)"};
constexpr Site kThunder{L"Engine.dll", 0x710eaf, 0x805650, "thunder roll (Rand in eCWeatherAdmin::Process)"};
// Game.dll: the climb code's minimum frame time (gCCharacterMovement_PS::EL_GetFrameTimeInSeconds, 0.0084 s)
constexpr std::uint32_t kClimbClampRva = 0xf63174;

using TimerInstanceFn = void* (*)();
using FrameTimeFn = float (*)(const void* timer);
using RandFn = float (*)(void* twister, float max);
using InvScaledFn = float* (*)(const float* self, float* result, float divisor);
using GetPositionFn = float* (*)(const void* movement, float* result);
using SetPositionFn = void (*)(void* movement, const float* position, bool a, bool b);

TimerInstanceFn g_timer = nullptr;
FrameTimeFn g_frameTime = nullptr;
RandFn g_rand = nullptr;
InvScaledFn g_invScaled = nullptr;
GetPositionFn g_getPosition = nullptr;
SetPositionFn g_setPosition = nullptr;
float g_climbClamp = 0.0084f;

std::atomic<bool> g_loggedFall{false}, g_loggedThunder{false}, g_loggedClimb{false};

bool enabled() {
    const settings::Setting* s = settings::renderer().highFps;
    return s != nullptr && s->asBool() && g_timer != nullptr && g_frameTime != nullptr;
}

float frameTime() { return g_frameTime(g_timer()); }

void logOnce(std::atomic<bool>& flag, const char* what, float scale) {
    if (!flag.exchange(true)) spdlog::info("engine: high fps: {} active (x{:.2f} at this frame rate)", what, scale);
}

// ---- the replacements (game thread) ----

float fallAbs(float dy) {
    float a = std::fabs(dy);
    if (enabled()) {
        const float k = frameScale(frameTime());
        if (k > 1.0f) logOnce(g_loggedFall, "fall check scaling", k);
        a *= k;
    }
    return a;
}

float thunderRand(void* twister, float max) {
    float r = g_rand(twister, max);
    if (enabled()) {
        const float k = frameScale(frameTime());
        if (k > 1.0f) logOnce(g_loggedThunder, "thunder roll scaling", k);
        r *= k;
    }
    return r;
}

// reached through a stub that passes the caller's RSI (the gCCharacterMovement_PS) as the fourth argument
float* climbInvScaled(const float* correction, float* result, float divisor, void* movement) {
    if (!enabled() || movement == nullptr || g_getPosition == nullptr || g_setPosition == nullptr) {
        return g_invScaled(correction, result, divisor);
    }
    const float dt = frameTime();
    if (dt <= 0.0f || dt >= g_climbClamp) return g_invScaled(correction, result, divisor);  // the game's math is exact here
    logOnce(g_loggedClimb, "climb Y correction applied directly", frameScale(dt));
    float position[3];
    g_getPosition(movement, position);
    position[1] += correction[1];
    g_setPosition(movement, position, true, true);
    result[0] = correction[0] / divisor;
    result[1] = 0.0f;
    result[2] = correction[2] / divisor;
    return result;
}

// ---- patching ----

// executable memory within +-2 GiB of a module (rip-relative slots / stubs)
struct NearPage {
    std::uint8_t* base = nullptr;
    std::size_t used = 0;

    bool allocate(std::uintptr_t around) {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const std::uintptr_t gran = si.dwAllocationGranularity;
        const std::uintptr_t range = 0x70000000;  // ~1.75 GiB
        const std::uintptr_t lo = around > range ? around - range : gran;
        const std::uintptr_t hi = around + range;
        for (int dir = 0; dir < 2 && base == nullptr; ++dir) {
            std::uintptr_t a = (around / gran) * gran;
            while (a > lo && a < hi) {
                a = dir == 0 ? a - gran : a + gran;
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) == 0) break;
                if (mbi.State != MEM_FREE) continue;
                if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
                    base = static_cast<std::uint8_t*>(p);
                    break;
                }
            }
        }
        return base != nullptr;
    }
    std::uint8_t* take(std::size_t n) {
        used = (used + 15) & ~std::size_t{15};
        if (base == nullptr || used + n > 4096) return nullptr;
        std::uint8_t* p = base + used;
        used += n;
        return p;
    }
    void seal() const {
        DWORD old = 0;
        if (base != nullptr) VirtualProtect(base, 4096, PAGE_EXECUTE_READ, &old);
    }
};

NearPage g_gamePage, g_enginePage;

// the site's `call [rip + disp]` -> its slot address, or 0 when the bytes are not the expected call through `slotRva`
std::uintptr_t checkSite(const Site& s, std::uintptr_t moduleBase) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(moduleBase + s.rva);
    if (p[0] != 0xFF || p[1] != 0x15) return 0;
    std::int32_t disp = 0;
    std::memcpy(&disp, p + 2, 4);
    const std::uintptr_t slot = moduleBase + s.rva + 6 + static_cast<std::intptr_t>(disp);
    return slot == moduleBase + s.slotRva ? slot : 0;
}

// points the site's call at a new slot holding `target` (optionally a stub `mov r9, rsi; jmp target` first); returns the
// function the import slot held (the original callee), null on failure
void* redirect(const Site& s, NearPage& page, void* target, bool passRsi) {
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(s.module));
    if (moduleBase == 0) return nullptr;
    const std::uintptr_t importSlot = checkSite(s, moduleBase);
    if (importSlot == 0) {
        spdlog::warn("engine: high fps: {}: unexpected code at {:#x} (another game build?): not patched", s.name, s.rva);
        return nullptr;
    }
    if (page.base == nullptr && !page.allocate(moduleBase)) {
        spdlog::warn("engine: high fps: no memory near the module for {}", s.name);
        return nullptr;
    }
    void* original = nullptr;
    std::memcpy(&original, reinterpret_cast<const void*>(importSlot), sizeof(void*));
    void* callee = target;
    if (passRsi) {
        std::uint8_t* stub = page.take(16);
        if (stub == nullptr) return nullptr;
        const std::uint8_t code[] = {0x49, 0x89, 0xF1, 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};  // mov r9, rsi; jmp [rip]
        std::memcpy(stub, code, sizeof(code));
        std::memcpy(stub + sizeof(code), &target, sizeof(void*));
        callee = stub;
    }
    std::uint8_t* slot = page.take(sizeof(void*));
    if (slot == nullptr) return nullptr;
    std::memcpy(slot, &callee, sizeof(void*));
    const auto insn = reinterpret_cast<std::uint8_t*>(moduleBase + s.rva);
    const std::intptr_t disp = reinterpret_cast<std::intptr_t>(slot) - reinterpret_cast<std::intptr_t>(insn + 6);
    if (disp < INT32_MIN || disp > INT32_MAX) return nullptr;
    const auto d32 = static_cast<std::int32_t>(disp);
    DWORD old = 0;
    if (!VirtualProtect(insn + 2, 4, PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    std::memcpy(insn + 2, &d32, 4);
    VirtualProtect(insn + 2, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), insn, 6);
    spdlog::info("engine: high fps: {} redirected", s.name);
    return original;
}

}  // namespace

float frameScale(float frameTimeSeconds) {
    if (!(frameTimeSeconds > 0.0f)) return 1.0f;
    const float k = (1.0f / 30.0f) / frameTimeSeconds;
    return k < 1.0f ? 1.0f : k > 100.0f ? 100.0f : k;
}

bool install() {
    const auto& s = sym();
    g_timer = reinterpret_cast<TimerInstanceFn>(s.timerGetInstance);
    g_frameTime = reinterpret_cast<FrameTimeFn>(s.timerFrameTimeSeconds);
    g_getPosition = reinterpret_cast<GetPositionFn>(s.movementGetPosition);
    g_setPosition = reinterpret_cast<SetPositionFn>(s.movementSetPosition);
    if (g_timer == nullptr || g_frameTime == nullptr) {
        spdlog::warn("engine: high fps: eCTimer not found: compat.high_fps has no effect");
        return false;
    }
    if (const auto game = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"Game.dll")); game != 0) {
        float clamp = 0.0f;
        std::memcpy(&clamp, reinterpret_cast<const void*>(game + kClimbClampRva), sizeof(clamp));
        if (clamp > 0.004f && clamp < 0.02f) g_climbClamp = clamp;
    }
    int patched = 0;
    if (redirect(kFallAbs, g_gamePage, reinterpret_cast<void*>(&fallAbs), false) != nullptr) ++patched;
    // both climb sites call the same import: the second redirect returns the same original
    if (void* o = redirect(kClimbHigh, g_gamePage, reinterpret_cast<void*>(&climbInvScaled), true)) {
        g_invScaled = reinterpret_cast<InvScaledFn>(o);
        ++patched;
    }
    if (void* o = redirect(kClimbStandard, g_gamePage, reinterpret_cast<void*>(&climbInvScaled), true)) {
        g_invScaled = reinterpret_cast<InvScaledFn>(o);
        ++patched;
    }
    if (void* o = redirect(kThunder, g_enginePage, reinterpret_cast<void*>(&thunderRand), false)) {
        g_rand = reinterpret_cast<RandFn>(o);
        ++patched;
    }
    g_gamePage.seal();
    g_enginePage.seal();
    spdlog::info("engine: high fps compatibility: {} of 4 call sites patched (climb clamp {:.4f} s)", patched, g_climbClamp);
    return patched > 0;
}

}  // namespace rvk::engine::highfps
