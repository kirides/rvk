// The ABI probe's shared header (src/shader/abi_blocks.cpp probeHlsl, tools/rhitest/abi_probe.cpp): not used by the renderer. Every probe shader
// declares one ABI block through the production binding path, reads every field and stores the raw 32-bit words with PUT in logical order into
// the storage image u0 / space1 (one R32_UINT texel per word).
#pragma once

[[vk::image_format("r32ui")]] RWTexture2D<uint> g_probeOut : register(u0, space1);
#define PUT(v) g_probeOut[uint2(n++, 0)] = (v)
