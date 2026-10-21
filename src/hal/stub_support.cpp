#include "hal/stub_support.h"

#include <array>
#include <atomic>

#include <spdlog/spdlog.h>

#include "hal/api_generated.h"
#include "hal/slots_generated.h"

namespace rvk::hal {
namespace {

std::array<std::array<std::atomic<bool>, 256>, 16> g_seen{};
std::atomic<int> g_count{0};

const char* slotName(int iface, int slot) {
    static const std::array<std::span<const char* const>, kInterfaceCount> tables = {
        keCAPIVertexDataSlots, keCAPIIndexDataSlots, keCAPITextureSlots,   keCAPICubeMapSlots,
        keCAPIVolumeSlots,     keCAPIPrimitiveSlots, keCAPIHLSLShaderSlots, keCAPIEffectSlots,
        keCAPISpriteSlots,     keCAPIFontSlots,      keCAPIDeviceSlots,    keCAPIInstanceSlots,
    };
    if (iface < 0 || iface >= kInterfaceCount || slot < 0 || static_cast<std::size_t>(slot) >= tables[static_cast<std::size_t>(iface)].size()) {
        return "?";
    }
    return tables[static_cast<std::size_t>(iface)][static_cast<std::size_t>(slot)];
}

}  // namespace

void noteUnimplemented(int iface, int slot) {
    if (iface < 0 || iface >= 16 || slot < 0 || slot >= 256) {
        return;
    }
    if (!g_seen[static_cast<std::size_t>(iface)][static_cast<std::size_t>(slot)].exchange(true)) {
        ++g_count;
        spdlog::warn("hal: {}::{} (slot {}) is not implemented", iface < kInterfaceCount ? kInterfaceNames[iface] : "?",
                     slotName(iface, slot), slot);
    }
}

void* stubStorage() {
    alignas(16) static unsigned char storage[4096]{};
    return storage;
}

int unimplementedCount() { return g_count.load(); }

}  // namespace rvk::hal
