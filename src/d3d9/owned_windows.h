#pragma once

// Window override (docs/research/frame-ownership.md 7.8 / 8, F1a): a caller that owns persistent register images (the Material table's records, the
// per-view block) lets the device point some register windows of the next draws (abi.h kVsWindowFirst / kPsWindowFirst) at them instead of uploading
// them from the register file. Pure data: the device (Device::setOwnedWindows, splitStage) reads it, gpuscene/window_tables.* fills it.

#include <cstdint>

#include "rhi/game_device.h"
#include "shader/abi.h"

namespace rvk::d3d9 {

struct OwnedWindows {
    enum class Kind : std::uint8_t { None, Material, View, Lighting, Object };
    // why a window the caller could have owned is not (device statistics): 0 owned / not asked, 1 switch material, 2 other
    enum Why : std::uint8_t { WhyNone = 0, WhySwitch = 1, WhyOther = 2 };
    struct Win {
        Kind kind = Kind::None;
        std::uint8_t why = WhyNone;
        rhi::GpuRef base;                 // the biased base: image address - imageFirst * 16, so c[n] keeps the D3D9 register number
        std::uint32_t covered = 0;        // registers the image holds from the window's first one (the draw's need must fit)
        std::uint32_t verifyMask = 0;     // bit i = register (window first + i) is one the draw composed (debug.split_verify compares only those)
        std::uint32_t ignoreMask = 0;     // bit i = register (window first + i) is not meaningful for the draw: left out of debug.split_verify's comparisons and diagnostic
        std::uint32_t wOnlyMask = 0;      // bit i = only the .w of register i is meaningful (the others are carry-over of the engine's register file): only .w is compared
        const float* image = nullptr;     // CPU twin of the window's registers from its first one (debug.split_verify), `covered` float4s
    };
    Win w[2][shader::abi::kConstWindows];  // [stage 0 VS / 1 PS][window]
    void clear() {
        for (auto& s : w) {
            for (auto& x : s) x = Win{};
        }
    }
};

// Device::ownedStats (per 5 s window logged by gpuscene/window_tables)
struct OwnedStats {
    std::uint64_t draws = 0;                      // draws prepared with a window override set
    std::uint64_t material[2] = {};               // windows served from a Material record (VS window 3, PS window 2)
    std::uint64_t lighting = 0;                   // PS window 3 served from a lighting image (F1b)
    std::uint64_t view[2] = {};                   // windows served from a view block (VS window 0, PS window 0)
    std::uint64_t view1 = 0;                      // VS window 1 (c12..c30) served from a view block (F2c-1, native.owned_objects)
    std::uint64_t shortVs1 = 0, shortVs2 = 0;     // of fallbackShort: VS window 1 / 2 images shorter than the shader reads (instanced draws: VS window 2)
    std::uint64_t object = 0;                     // VS window 2 (c31..c35) served from the object table (F2c-1)
    std::uint64_t uploaded = 0, reused = 0;       // the other windows of those draws: written to the ring / the cached ring block reused
    std::uint64_t fallbackSwitch = 0, fallbackOther = 0, fallbackShort = 0;  // windows the caller offered no image for / an image too short for the shader
    std::uint64_t verified = 0, mismatches = 0, uncomposedDiffer = 0;   // windows compared with the register file (debug.split_verify), mismatching
};

}  // namespace rvk::d3d9
