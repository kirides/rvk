#pragma once
// frame-ownership.md 7.27: the tables' per-draw VERIFICATION (the material table's register / texture compare, the illumination table's composed-lighting
// compare, the lighting images' drawn-register copy, the op records' derivation + compare, the object table's matrix check, the window fill's candidate
// compare) only runs on check frames: frame % interval == 0, the same frames as the owned path's check frames (native.owned_check, default 10). On the other
// frames only the maintenance runs (what makes the table content right: slot lookup, the once-per-frame poll / refresh, graph notes). A missed dirty signal
// therefore shows up as a counted DIFFERING / MISS on the next check frame instead of costing every frame.
// `RVK_TABLE_VERIFY_ALL=1` (environment) = every frame, the behaviour before 7.27 (A/B).
#include <cstdint>

namespace rvk::gpuscene {

extern "C" __declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char* name, char* buffer, unsigned long size);

namespace detail {
inline bool g_verifyFrame = true;       // the frame being composed is a check frame (true until the first present: the start-up frames verify)
inline std::uint32_t g_verifyInterval = 10;
inline bool g_checksOff = false;         // native.owned_check = 0: no periodic verification anywhere (perf runs measure the owned frame alone)
// (the Win32 environment: `-Environment` variables are set by the proxy's DllMain with SetEnvironmentVariable, which the CRT's getenv does not see; read at the first present)
inline bool verifyAllEnv() {
    static const bool all = [] {
        char v[4] = {};
        return GetEnvironmentVariableA("RVK_TABLE_VERIFY_ALL", v, sizeof(v)) != 0 && v[0] != '0';
    }();
    return all;
}
}  // namespace detail

// debug.split_verify (the window verifyMasks are only read then): the table fills skip building them otherwise
namespace detail {
inline bool g_windowMasks = false;
}
inline bool windowMasksWanted() { return detail::g_windowMasks; }
inline void setWindowMasksWanted(bool on) { detail::g_windowMasks = on; }

// true while a check frame is composed (engine thread; set by setVerifyFrame at the present)
inline bool verifyFrame() { return detail::g_verifyFrame; }

// interval = native.owned_check (0 = every periodic check off, see checksOff())
inline void setVerifyInterval(int interval) {
    detail::g_checksOff = interval <= 0;
    detail::g_verifyInterval = interval > 0 ? static_cast<std::uint32_t>(interval) : 10u;
    if (detail::g_checksOff) detail::g_verifyFrame = false;
}

// native.owned_check = 0: every periodic verification / read-back / rolling scrub of the owned path is off (the ONE source; env knobs such as
// RVK_TABLE_VERIFY_ALL, RVK_F2C4C_CHECK, RVK_BB_CHECK still override their own stage). Never "every frame", never a division by zero.
inline bool checksOff() { return detail::g_checksOff; }

// the stages' own cadences (240 / 60 / 30 / 10 frames ...) keep their period while checks are on; off = never
inline bool periodic(std::uint64_t frame, std::uint64_t every) { return !detail::g_checksOff && every != 0 && frame % every == 0; }

// called once per present for the NEXT frame's number (the owned path's check kind uses the same frame number: sc::leanFrameKind(frame, interval))
inline void setVerifyFrame(std::uint64_t nextFrame) { detail::g_verifyFrame = detail::verifyAllEnv() || (!detail::g_checksOff && nextFrame % detail::g_verifyInterval == 0); }

}  // namespace rvk::gpuscene
