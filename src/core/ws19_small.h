#pragma once
// WS19 small fixes (docs/research/frame-ownership.md 14.61): `RVK_WS19S_OFF=1` (Win32 environment, read at the first use) = the behaviour before them, for the A/B:
// the empty material-pack provider's constant stamp, the water's sea-floor learning without the owned register load, recLiveWhy's cluster check by the store's state serial
extern "C" __declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char* name, char* buffer, unsigned long size);

namespace rvk {

inline bool ws19SmallOff() {
    static const bool off = [] {
        char v[8] = {};
        return GetEnvironmentVariableA("RVK_WS19S_OFF", v, sizeof(v)) != 0 && v[0] != '0';
    }();
    return off;
}

}  // namespace rvk
